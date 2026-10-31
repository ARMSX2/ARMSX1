#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psx/psx.h"

static void transfer(psx_t* psx, unsigned channel, uint32_t address,
                     uint32_t count, uint32_t control) {
    const uint32_t base = channel * 16;
    psx_dma_write32(psx->dma, base, address);
    assert(psx_dma_read32(psx->dma, base) == address);
    psx_dma_write32(psx->dma, base + 4, count);
    psx_dma_write32(psx->dma, base + 8, control);
}

static void gpu_requests(psx_t* psx) {
    const uint32_t aliases[] = {0, 0x200000, 0x600000, 0x80000000};
    for (unsigned alias = 0; alias < sizeof(aliases) / sizeof(aliases[0]); ++alias) {
        for (uint32_t low = 0; low < 4; ++low) {
            psx_bus_write32(psx->bus, 0x1000, 0xe3000000 | (17 << 10) | 5);
            psx->gpu->draw_x1 = psx->gpu->draw_y1 = 0;
            const uint32_t address = aliases[alias] + 0x1000 + low;
            transfer(psx, 2, address, 0x10001, 0x01000201);
            assert(psx->gpu->draw_x1 == 5 && psx->gpu->draw_y1 == 17);
            assert(psx_dma_read32(psx->dma, 0x20) == address + 4);

            psx->gpu->gpuread = 0x12345678;
            psx_bus_write32(psx->bus, 0x1ffffc, 0);
            psx_bus_write32(psx->bus, 0, 0);
            transfer(psx, 2, 0x1ffffc + low, 0x10002, 0x01000200);
            assert(psx_bus_read32(psx->bus, 0x1ffffc) == 0x12345678);
            assert(psx_bus_read32(psx->bus, 0) == 0x12345678);
            assert((psx_dma_read32(psx->dma, 0x20) & 3) == low);
        }
    }
}

static void gpu_linked_headers(psx_t* psx) {
    for (uint32_t first_low = 0; first_low < 4; ++first_low) {
        for (uint32_t next_low = 0; next_low < 4; ++next_low) {
            psx_bus_write32(psx->bus, 0x1000, 0x01002000 | next_low);
            psx_bus_write32(psx->bus, 0x1004, 0xe3000000 | (19 << 10) | 7);
            psx_bus_write32(psx->bus, 0x2000, 0x01ffffff);
            psx_bus_write32(psx->bus, 0x2004, 0xe4000000 | (211 << 10) | 319);
            psx->gpu->draw_x1 = psx->gpu->draw_y1 = 0;
            psx->gpu->draw_x2 = psx->gpu->draw_y2 = 0;
            transfer(psx, 2, 0x1000 + first_low, 0, 0x01000401);
            assert(psx->gpu->draw_x1 == 7 && psx->gpu->draw_y1 == 19);
            assert(psx->gpu->draw_x2 == 319 && psx->gpu->draw_y2 == 211);
        }
    }
}

static void otc_chain(psx_t* psx) {
    psx->dma->dpcr |= DPCR_DMA6EN;
    for (uint32_t low = 0; low < 4; ++low) {
        psx_bus_write32(psx->bus, 0x1ffc, 0xaabbccdd);
        psx_bus_write32(psx->bus, 0x200c, 0x55667788);
        transfer(psx, 6, 0x2008 + low, 3, 0x11000002);
        assert(psx_bus_read32(psx->bus, 0x2008) == 0x2004);
        assert(psx_bus_read32(psx->bus, 0x2004) == 0x2000);
        assert(psx_bus_read32(psx->bus, 0x2000) == 0xffffff);
        assert(psx_bus_read32(psx->bus, 0x1ffc) == 0xaabbccdd);
        assert(psx_bus_read32(psx->bus, 0x200c) == 0x55667788);
        assert((psx_dma_read32(psx->dma, 0x60) & 3) == low);

        transfer(psx, 6, 0x200000 + low, 2, 0x11000002);
        assert(psx_bus_read32(psx->bus, 0) == 0x1ffffc);
        assert(psx_bus_read32(psx->bus, 0x1ffffc) == 0xffffff);
    }
}

static void mdec_transfers(psx_t* psx) {
    for (uint32_t low = 0; low < 4; ++low) {
        psx_bus_write32(psx->bus, 0x3000, 0x00012345);
        transfer(psx, 0, 0x3000 + low, 0x10001, 0x01000201);
        assert(psx->mdec->cmd == 0x00012345);
        assert(psx_dma_read32(psx->dma, 0) == 0x3004 + low);

        const uint32_t output[] = {0x11223344, 0x55667788};
        free(psx->mdec->output);
        psx->mdec->output = malloc(sizeof(output));
        assert(psx->mdec->output);
        memcpy(psx->mdec->output, output, sizeof(output));
        psx->mdec->output_index = 0;
        psx->mdec->output_words_remaining = 2;
        transfer(psx, 1, 0x1ffffc + low, 0x10002, 0x01000200);
        assert(psx_bus_read32(psx->bus, 0x1ffffc) == output[0]);
        assert(psx_bus_read32(psx->bus, 0) == output[1]);
        assert(psx_dma_read32(psx->dma, 0x10) == 0x200004 + low);
    }
}

int main(void) {
    log_set_quiet(1);
    char bios_path[] = "/tmp/armsx-dma-bios-XXXXXX";
    int fd = mkstemp(bios_path);
    assert(fd >= 0 && ftruncate(fd, 512 * 1024) == 0 && close(fd) == 0);
    psx_t* psx = psx_create();
    assert(psx && !psx_init(psx, bios_path, NULL));
    assert(unlink(bios_path) == 0);
    gpu_requests(psx);
    gpu_linked_headers(psx);
    otc_chain(psx);
    mdec_transfers(psx);
    psx_destroy(psx);
    puts("DMA aligned words, linked headers, OTC chains and RAM mirrors passed");
    return 0;
}
