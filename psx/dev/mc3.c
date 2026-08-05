#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mc3.h"
#include "../log.h"

psx_mc3_t* psx_mc3_create(void) {
    return (psx_mc3_t*)calloc(1, sizeof(psx_mc3_t));
}

/*
  FFFE0130h 4        Cache Control
*/
void psx_mc3_init(psx_mc3_t* mc3) {
    memset(mc3, 0, sizeof(psx_mc3_t));

    mc3->io_base = PSX_MC3_BEGIN;
    mc3->io_size = PSX_MC3_SIZE;
}

uint32_t psx_mc3_read32(psx_mc3_t* mc3, uint32_t offset) {
    switch (offset) {
        case 0x00: return mc3->cache_control;
    }

    log_warn("Unhandled 32-bit MC3 read at offset %08x", offset);

    return 0x0;
}

uint16_t psx_mc3_read16(psx_mc3_t* mc3, uint32_t offset) {
    log_warn("Unhandled 16-bit MC3 read at offset %08x", offset);

    return 0x0;
}

uint8_t psx_mc3_read8(psx_mc3_t* mc3, uint32_t offset) {
    log_warn("Unhandled 8-bit MC3 read at offset %08x", offset);

    return 0x0;
}

void psx_mc3_write32(psx_mc3_t* mc3, uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: mc3->cache_control = value; break;

        default: {
            log_warn("Unhandled 32-bit MC3 write at offset %08x (%08x)", offset, value);
        } break;
    }
}

void psx_mc3_write16(psx_mc3_t* mc3, uint32_t offset, uint16_t value) {
    log_warn("Unhandled 16-bit MC3 write at offset %08x (%04x)", offset, value);
}

void psx_mc3_write8(psx_mc3_t* mc3, uint32_t offset, uint8_t value) {
    log_warn("Unhandled 8-bit MC3 write at offset %08x (%02x)", offset, value);
}

void psx_mc3_save_state(psx_mc3_t* mc3, psx_state_writer_t* w) {
    psx_sw_u32(w, mc3->cache_control);
}

int psx_mc3_load_state(psx_mc3_t* mc3, psx_state_reader_t* r) {
    mc3->cache_control = psx_sr_u32(r);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_mc3_destroy(psx_mc3_t* mc3) {
    free(mc3);
}