#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "scratchpad.h"

psx_scratchpad_t* psx_scratchpad_create(void) {
    return (psx_scratchpad_t*)calloc(1, sizeof(psx_scratchpad_t));
}

void psx_scratchpad_init(psx_scratchpad_t* scratchpad) {
    memset(scratchpad, 0, sizeof(psx_scratchpad_t));

    scratchpad->io_base = PSX_SCRATCHPAD_BEGIN;
    scratchpad->io_size = PSX_SCRATCHPAD_SIZE;

    scratchpad->buf = (uint8_t*)malloc(PSX_SCRATCHPAD_SIZE);
}

uint32_t psx_scratchpad_read32(psx_scratchpad_t* scratchpad, uint32_t offset) {
    return *((uint32_t*)(scratchpad->buf + offset));
}

uint16_t psx_scratchpad_read16(psx_scratchpad_t* scratchpad, uint32_t offset) {
    return *((uint16_t*)(scratchpad->buf + offset));
}

uint8_t psx_scratchpad_read8(psx_scratchpad_t* scratchpad, uint32_t offset) {
    return scratchpad->buf[offset];
}

void psx_scratchpad_write32(psx_scratchpad_t* scratchpad, uint32_t offset, uint32_t value) {
    *((uint32_t*)(scratchpad->buf + offset)) = value;
}

void psx_scratchpad_write16(psx_scratchpad_t* scratchpad, uint32_t offset, uint16_t value) {
    *((uint16_t*)(scratchpad->buf + offset)) = value;
}

void psx_scratchpad_write8(psx_scratchpad_t* scratchpad, uint32_t offset, uint8_t value) {
    scratchpad->buf[offset] = value;
}

void psx_scratchpad_save_state(psx_scratchpad_t* scratchpad, psx_state_writer_t* w) {
    psx_sw_u32(w, PSX_SCRATCHPAD_SIZE);
    psx_sw_bytes(w, scratchpad->buf, PSX_SCRATCHPAD_SIZE);
}

int psx_scratchpad_load_state(psx_scratchpad_t* scratchpad, psx_state_reader_t* r) {
    uint32_t size = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (size != PSX_SCRATCHPAD_SIZE)
        return PSX_STATE_ERR_GEOMETRY;

    psx_sr_bytes(r, scratchpad->buf, PSX_SCRATCHPAD_SIZE);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_scratchpad_destroy(psx_scratchpad_t* scratchpad) {
    free(scratchpad->buf);
    free(scratchpad);
}