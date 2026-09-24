#include "../psx/psx.h"
#include "../psx/instruction_fetch.h"
#include <assert.h>
#include <stdio.h>

static void compare(psx_t* p, uint32_t address) {
    uint32_t expected = psx_bus_read32(p->bus, address);
    uint32_t cycles = psx_bus_get_access_cycles(p->bus);
    assert(psx_instruction_fetch(p->bus, address) == expected);
    assert(psx_bus_get_access_cycles(p->bus) == cycles);
}
int main(void) {
    FILE* f = fopen("fetch-bios.bin", "wb"); assert(f);
    uint32_t seed = 0x12345678;
    for (unsigned i=0; i<PSX_BIOS_SIZE/4; ++i) {
        seed = seed * 1664525u + 1013904223u;
        assert(fwrite(&seed, 4, 1, f) == 1);
    }
    fclose(f);
    psx_t* p = psx_create(); assert(p && psx_init(p, "fetch-bios.bin", NULL) == 0);
    for (size_t i=0; i<p->ram->size; ++i) p->ram->buf[i] = (unsigned char)(i ^ (i >> 9));
    const uint32_t segments[] = {0, 0x80000000u, 0xa0000000u};
    p->ram->bus_delay = 7; p->bios->bus_delay = 13;
    for (unsigned mode=0; mode<8; ++mode) {
        p->mc2->ram_size = mode << 9;
        for (unsigned s=0; s<3; ++s) {
            for (unsigned offset=0; offset<PSX_RAM_SIZE; offset+=4092)
                compare(p, segments[s] | offset);
            compare(p, segments[s] | (PSX_RAM_SIZE-4));
            for (unsigned offset=0; offset<PSX_BIOS_SIZE; offset+=1020)
                compare(p, segments[s] | (PSX_BIOS_BEGIN+offset));
            compare(p, segments[s] | PSX_SCRATCHPAD_BEGIN);
        }
    }
    /* A direct modification models DMA/state restoration through another alias. */
    p->ram->buf[0x1000] ^= 0xff;
    compare(p, 0x80001000); compare(p, 0xa0201000);
    psx_destroy(p); remove("fetch-bios.bin");
    puts("Instruction fetch: RAM mirrors, BIOS, access timing and fallback passed");
    return 0;
}
