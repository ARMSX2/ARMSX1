#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "exp2.h"

psx_exp2_t* psx_exp2_create(void) {
    return (psx_exp2_t*)calloc(1, sizeof(psx_exp2_t));
}

void psx_exp2_init(psx_exp2_t* exp2, exp2_tty_tx atcons_tx, exp2_tty_tx duart_tx) {
    memset(exp2, 0, sizeof(psx_exp2_t));

    exp2->io_base = PSX_EXP2_BEGIN;
    exp2->io_size = PSX_EXP2_SIZE;
    exp2->atcons_tx = atcons_tx;
    exp2->duart_tx = duart_tx;
}

void psx_exp2_atcons_put(psx_exp2_t* exp2, char c) {
    exp2->atc_stat |= 0x10;
    exp2->atc_rx = c;
}

void psx_exp2_duart_put(psx_exp2_t* exp2, char c) {
    /* To-do */
}

uint32_t psx_exp2_read32(psx_exp2_t* exp2, uint32_t offset) {
    return 0;
}

uint16_t psx_exp2_read16(psx_exp2_t* exp2, uint32_t offset) {
    return 0;
}

uint8_t psx_exp2_read8(psx_exp2_t* exp2, uint32_t offset) {
    switch (offset) {
        case EXP2_DTL_ATC_STAT:
            return exp2->atc_stat | 8;

        case EXP2_DTL_ATC_DATA:
            exp2->atc_stat &= 0xef;
            return exp2->atc_rx;
    }

    return 0;
}

void psx_exp2_write32(psx_exp2_t* exp2, uint32_t offset, uint32_t value) {
    log_warn("Unhandled 32-bit EXP2 write at offset %08x (%08x)", offset, value);
}

void psx_exp2_write16(psx_exp2_t* exp2, uint32_t offset, uint16_t value) {
    log_warn("Unhandled 16-bit EXP2 write at offset %08x (%04x)", offset, value);
}

void psx_exp2_write8(psx_exp2_t* exp2, uint32_t offset, uint8_t value) {
    switch (offset) {
        case EXP2_DTL_ATC_DATA:
            if (exp2->atcons_tx)
                exp2->atcons_tx(exp2->atcons_udata, value);
            return;
        break;

        case EXP2_LED:
        case EXP2_POST:
        case EXP2_POST2:
            // To-do: Do something with this data
            return;
        break;
    }

    log_warn("Unhandled 8-bit EXP2 write at offset %08x (%02x)", offset, value);
}

void psx_exp2_save_state(psx_exp2_t* exp2, psx_state_writer_t* w) {
    psx_sw_u8(w, exp2->atc_stat);
    psx_sw_u8(w, exp2->atc_rx);
}

int psx_exp2_load_state(psx_exp2_t* exp2, psx_state_reader_t* r) {
    exp2->atc_stat = psx_sr_u8(r);
    exp2->atc_rx = psx_sr_u8(r);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_exp2_destroy(psx_exp2_t* exp2) {
    free(exp2);
}