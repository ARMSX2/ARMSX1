#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "bus_init.h"
#include "log.h"

#define RANGE(v, s, e) ((v >= s) && (v < e))

const uint32_t g_psx_bus_region_mask_table[] = {
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0x7fffffff, 0x1fffffff, 0xffffffff, 0xffffffff
};

uint32_t psx_bus_physical_address(uint32_t addr) {
    return addr & g_psx_bus_region_mask_table[addr >> 29];
}
psx_bus_t* psx_bus_create(void) {
    return (psx_bus_t*)calloc(1, sizeof(psx_bus_t));
}

void psx_bus_init(psx_bus_t* bus) {
    if (bus)
        memset(bus, 0, sizeof(*bus));
}

void psx_bus_set_write_observer(psx_bus_t* bus, psx_bus_write_observer_t observer, void* udata) {
    if (!bus)
        return;

    bus->write_observer = observer;
    bus->write_observer_udata = udata;
}

void psx_bus_save_state(psx_bus_t* bus, psx_state_writer_t* w) {
    psx_sw_u32(w, bus->access_cycles);
}

int psx_bus_load_state(psx_bus_t* bus, psx_state_reader_t* r) {
    bus->access_cycles = psx_sr_u32(r);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_bus_destroy(psx_bus_t* bus) {
    free(bus);
}

#define HANDLE_READ(dev, bits) \
    if (RANGE(addr, bus->dev->io_base, (bus->dev->io_base + bus->dev->io_size))) { \
        bus->access_cycles = bus->dev->bus_delay; \
        return psx_ ## dev ## _read ## bits (bus->dev, addr - bus->dev->io_base); \
    }
#define HANDLE_WRITE(dev, bits) \
    if (RANGE(addr, bus->dev->io_base, (bus->dev->io_base + bus->dev->io_size))) { \
        bus->access_cycles = bus->dev->bus_delay; \
        psx_ ## dev ## _write ## bits (bus->dev, addr - bus->dev->io_base, value); \
        if (bus->write_observer) \
            bus->write_observer(bus->write_observer_udata, addr, (bits) / 8); \
        return; \
    }

uint32_t psx_bus_read32(psx_bus_t* bus, uint32_t addr) {
    uint32_t vaddr = addr;

    addr = psx_bus_physical_address(addr);

    if (addr & 0x3) {
        log_fatal("Unaligned 32-bit read from %08x:%08x", vaddr, addr);
    }

    /* Check the non-overlapping RAM range first on this hot path. */
    HANDLE_READ(ram, 32);
    HANDLE_READ(bios, 32);
    HANDLE_READ(dma, 32);
    HANDLE_READ(exp1, 32);
    HANDLE_READ(exp2, 32);
    HANDLE_READ(mc1, 32);
    HANDLE_READ(mc2, 32);
    HANDLE_READ(mc3, 32);
    HANDLE_READ(ic, 32);
    HANDLE_READ(scratchpad, 32);
    HANDLE_READ(gpu, 32);
    HANDLE_READ(spu, 32);
    HANDLE_READ(timer, 32);
    HANDLE_READ(cdrom, 32);
    HANDLE_READ(pad, 32);
    HANDLE_READ(mdec, 32);

    log_fatal("Unhandled 32-bit read from %08x:%08x", vaddr, addr);

    //exit(1);

    return 0x00000000;
}

static uint16_t sio_ctrl;

uint16_t psx_bus_read16(psx_bus_t* bus, uint32_t addr) {
    bus->access_cycles = 2;

    uint32_t vaddr = addr;

    addr = psx_bus_physical_address(addr);

    if (addr & 0x1) {
        log_fatal("Unaligned 16-bit read from %08x:%08x", vaddr, addr);
    }

    HANDLE_READ(ram, 16);
    HANDLE_READ(bios, 16);
    HANDLE_READ(dma, 16);
    HANDLE_READ(exp1, 16);
    HANDLE_READ(exp2, 16);
    HANDLE_READ(mc1, 16);
    HANDLE_READ(mc2, 16);
    HANDLE_READ(mc3, 16);
    HANDLE_READ(ic, 16);
    HANDLE_READ(scratchpad, 16);
    HANDLE_READ(gpu, 16);
    HANDLE_READ(spu, 16);
    HANDLE_READ(timer, 16);
    HANDLE_READ(cdrom, 16);
    HANDLE_READ(pad, 16);
    HANDLE_READ(mdec, 16);

    if (addr == 0x1f80105a)
        return sio_ctrl;

    if (addr == 0x1f801054)
        return 0x05;

    if (addr == 0x1f400004)
        return 0xc8;

    if (addr == 0x1f400006)
        return 0x1fe0;

    if (log_first_access(addr, false, 16))
        printf("Unhandled 16-bit read from %08x:%08x\n", vaddr, addr);

    // exit(1);

    return 0x0000;
}

uint8_t psx_bus_read8(psx_bus_t* bus, uint32_t addr) {
    bus->access_cycles = 2;

    // uint32_t vaddr = addr;

    addr = psx_bus_physical_address(addr);

    HANDLE_READ(ram, 8);
    HANDLE_READ(bios, 8);
    HANDLE_READ(dma, 8);
    HANDLE_READ(exp1, 8);
    HANDLE_READ(exp2, 8);
    HANDLE_READ(mc1, 8);
    HANDLE_READ(mc2, 8);
    HANDLE_READ(mc3, 8);
    HANDLE_READ(ic, 8);
    HANDLE_READ(scratchpad, 8);
    HANDLE_READ(gpu, 8);
    HANDLE_READ(spu, 8);
    HANDLE_READ(timer, 8);
    HANDLE_READ(cdrom, 8);
    HANDLE_READ(pad, 8);
    HANDLE_READ(mdec, 8);

    // printf("Unhandled 8-bit read from %08x:%08x\n", vaddr, addr);

    //exit(1);

    return 0x00;
}

void psx_bus_write32(psx_bus_t* bus, uint32_t addr, uint32_t value) {
    bus->access_cycles = 0;

    uint32_t vaddr = addr;

    addr = psx_bus_physical_address(addr);

    if (addr & 0x3) {
        log_fatal("Unaligned 32-bit write to %08x:%08x (%08x)", vaddr, addr, value);
    }

    HANDLE_WRITE(ram, 32);
    HANDLE_WRITE(bios, 32);
    HANDLE_WRITE(dma, 32);
    HANDLE_WRITE(exp1, 32);
    HANDLE_WRITE(exp2, 32);
    HANDLE_WRITE(mc1, 32);
    HANDLE_WRITE(mc2, 32);
    HANDLE_WRITE(mc3, 32);
    HANDLE_WRITE(ic, 32);
    HANDLE_WRITE(scratchpad, 32);
    HANDLE_WRITE(gpu, 32);
    HANDLE_WRITE(spu, 32);
    HANDLE_WRITE(timer, 32);
    HANDLE_WRITE(cdrom, 32);
    HANDLE_WRITE(pad, 32);
    HANDLE_WRITE(mdec, 32);

    if (log_first_access(addr, true, 32))
        printf("Unhandled 32-bit write to %08x:%08x (%08x)\n", vaddr, addr, value);

    //exit(1);
}


void psx_bus_write16(psx_bus_t* bus, uint32_t addr, uint32_t value) {
    bus->access_cycles = 0;

    uint32_t vaddr = addr;

    addr = psx_bus_physical_address(addr);

    if (addr & 0x1) {
        log_fatal("Unaligned 16-bit write to %08x:%08x (%04x)", vaddr, addr, value);
    }

    HANDLE_WRITE(ram, 16);
    HANDLE_WRITE(bios, 16);
    HANDLE_WRITE(dma, 16);
    HANDLE_WRITE(exp1, 16);
    HANDLE_WRITE(exp2, 16);
    HANDLE_WRITE(mc1, 16);
    HANDLE_WRITE(mc2, 16);
    HANDLE_WRITE(mc3, 16);
    HANDLE_WRITE(ic, 16);
    HANDLE_WRITE(scratchpad, 16);
    HANDLE_WRITE(gpu, 16);
    HANDLE_WRITE(spu, 16);
    HANDLE_WRITE(timer, 16);
    HANDLE_WRITE(cdrom, 16);
    HANDLE_WRITE(pad, 16);
    HANDLE_WRITE(mdec, 16);

    // if (addr == 0x1f80105a) { sio_ctrl = value; return; }

    if (log_first_access(addr, true, 16))
        printf("Unhandled 16-bit write to %08x:%08x (%04x)\n", vaddr, addr, value);

    //exit(1);
}

void psx_bus_write8(psx_bus_t* bus, uint32_t addr, uint32_t value) {
    bus->access_cycles = 0;

    uint32_t vaddr = addr;

    addr = psx_bus_physical_address(addr);

    HANDLE_WRITE(ram, 8);
    HANDLE_WRITE(bios, 8);
    HANDLE_WRITE(dma, 8);
    HANDLE_WRITE(exp1, 8);
    HANDLE_WRITE(exp2, 8);
    HANDLE_WRITE(mc1, 8);
    HANDLE_WRITE(mc2, 8);
    HANDLE_WRITE(mc3, 8);
    HANDLE_WRITE(ic, 8);
    HANDLE_WRITE(scratchpad, 8);
    HANDLE_WRITE(gpu, 8);
    HANDLE_WRITE(spu, 8);
    HANDLE_WRITE(timer, 8);
    HANDLE_WRITE(cdrom, 8);
    HANDLE_WRITE(pad, 8);
    HANDLE_WRITE(mdec, 8);

    if (log_first_access(addr, true, 8))
        printf("Unhandled 8-bit write to %08x:%08x (%02x)\n", vaddr, addr, value);

    //exit(1);
}

void psx_bus_init_bios(psx_bus_t* bus, psx_bios_t* bios) {
    bus->bios = bios;
}

void psx_bus_init_ram(psx_bus_t* bus, psx_ram_t* ram) {
    bus->ram = ram;
}

void psx_bus_init_dma(psx_bus_t* bus, psx_dma_t* dma) {
    bus->dma = dma;
}

void psx_bus_init_exp1(psx_bus_t* bus, psx_exp1_t* exp1) {
    bus->exp1 = exp1;
}

void psx_bus_init_exp2(psx_bus_t* bus, psx_exp2_t* exp2) {
    bus->exp2 = exp2;
}

void psx_bus_init_mc1(psx_bus_t* bus, psx_mc1_t* mc1) {
    bus->mc1 = mc1;
}

void psx_bus_init_mc2(psx_bus_t* bus, psx_mc2_t* mc2) {
    bus->mc2 = mc2;
}

void psx_bus_init_mc3(psx_bus_t* bus, psx_mc3_t* mc3) {
    bus->mc3 = mc3;
}

void psx_bus_init_ic(psx_bus_t* bus, psx_ic_t* ic) {
    bus->ic = ic;
}

void psx_bus_init_scratchpad(psx_bus_t* bus, psx_scratchpad_t* scratchpad) {
    bus->scratchpad = scratchpad;
}

void psx_bus_init_gpu(psx_bus_t* bus, psx_gpu_t* gpu) {
    bus->gpu = gpu;
}

void psx_bus_init_spu(psx_bus_t* bus, psx_spu_t* spu) {
    bus->spu = spu;
}

void psx_bus_init_timer(psx_bus_t* bus, psx_timer_t* timer) {
    bus->timer = timer;
}

void psx_bus_init_cdrom(psx_bus_t* bus, psx_cdrom_t* cdrom) {
    bus->cdrom = cdrom;
}

void psx_bus_init_pad(psx_bus_t* bus, psx_pad_t* pad) {
    bus->pad = pad;
}

void psx_bus_init_mdec(psx_bus_t* bus, psx_mdec_t* mdec) {
    bus->mdec = mdec;
}

uint32_t psx_bus_get_access_cycles(psx_bus_t* bus) {
    uint32_t cycles = bus->access_cycles;

    bus->access_cycles = 0;

    return cycles;
}

#undef HANDLE_READ
#undef HANDLE_WRITE
