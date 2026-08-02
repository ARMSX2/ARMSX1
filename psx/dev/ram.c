#include "ram.h"
#include "../log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

psx_ram_t* psx_ram_create(void) {
    return (psx_ram_t*)malloc(sizeof(psx_ram_t));
}

void psx_ram_init(psx_ram_t* ram, psx_mc2_t* mc2, int size) {
    memset(ram, 0, sizeof(psx_ram_t));

    ram->io_base = PSX_RAM_BEGIN;
    ram->io_size = PSX_RAM_SIZE;

    ram->mc2 = mc2;
    ram->buf = (uint8_t*)malloc(size);
    ram->size = size;

    // Size has to be a multiple of 2MB, default to 2MB
    if (size & 0x1ffff)
        size = RAM_SIZE_2MB;

    memset(ram->buf, RAM_INIT_FILL, size);
}

uint32_t psx_ram_read32(psx_ram_t* ram, uint32_t offset) {
    if (((ram->mc2->ram_size >> 9) & 7) == 3)
        if (offset >= 0x400000)
            return 0xffffffff;

    offset &= ram->size - 1;

    return *((uint32_t*)(ram->buf + offset));
}

uint16_t psx_ram_read16(psx_ram_t* ram, uint32_t offset) {
    if (((ram->mc2->ram_size >> 9) & 7) == 3)
        if (offset >= 0x400000)
            return 0xffff;

    offset &= ram->size - 1;

    return *((uint16_t*)(ram->buf + offset));
}

uint8_t psx_ram_read8(psx_ram_t* ram, uint32_t offset) {
    if (((ram->mc2->ram_size >> 9) & 7) == 3)
        if (offset >= 0x400000)
            return 0xff;

    offset &= ram->size - 1;

    return ram->buf[offset];
}

void psx_ram_write32(psx_ram_t* ram, uint32_t offset, uint32_t value) {
    offset &= ram->size - 1;

    *((uint32_t*)(ram->buf + offset)) = value;
}

void psx_ram_write16(psx_ram_t* ram, uint32_t offset, uint16_t value) {
    offset &= ram->size - 1;

    *((uint16_t*)(ram->buf + offset)) = value;
}

void psx_ram_write8(psx_ram_t* ram, uint32_t offset, uint8_t value) {
    offset &= ram->size - 1;

    ram->buf[offset] = value;
}

void psx_ram_save_state(psx_ram_t* ram, psx_state_writer_t* w) {
    psx_sw_u32(w, (uint32_t)ram->size);
    psx_sw_bytes(w, ram->buf, ram->size);
}

int psx_ram_load_state(psx_ram_t* ram, psx_state_reader_t* r) {
    uint32_t size = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    /* Restore into the existing allocation. Reallocating would invalidate any
       reference the front-end (or a debugger view) holds. */
    if (size != (uint32_t)ram->size)
        return PSX_STATE_ERR_GEOMETRY;

    psx_sr_bytes(r, ram->buf, ram->size);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_ram_destroy(psx_ram_t* ram) {
    free(ram->buf);
    free(ram);
}