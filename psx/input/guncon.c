/*
    This file is part of the PSXE Emulator Project

    Sony PlayStation Standard Digital/Analog Controller Emulator
*/

#include "guncon.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>

const char* states[] = {
    "HIZ",
    "IDL",
    "IDH",
    "SWL",
    "SWH",
    "XPL",
    "XPH",
    "YPL",
    "YPH"
};

psxi_guncon_t* psxi_guncon_create(void) {
    return (psxi_guncon_t*)malloc(sizeof(psxi_guncon_t));
}

void psxi_guncon_init(psxi_guncon_t* guncon) {
    memset(guncon, 0, sizeof(psxi_guncon_t));

    guncon->tx_data = 0xff;
    guncon->tx_data_ready = 1;
    guncon->state = GUNCON_STATE_TX_HIZ;
    guncon->sw = 0xffff;
    guncon->x = 0x4d;
    guncon->y = 0x19;
    guncon->sx = 384;
    guncon->sy = 240;
}

uint32_t psxi_guncon_read(void* udata) {
    psxi_guncon_t* guncon = (psxi_guncon_t*)udata;

    switch (guncon->state) {
        case GUNCON_STATE_TX_HIZ: guncon->tx_data = 0xff; break;
        case GUNCON_STATE_TX_IDL: guncon->tx_data = GUNCON_IDL; break;
        case GUNCON_STATE_TX_IDH: guncon->tx_data = GUNCON_IDH; break;
        case GUNCON_STATE_TX_SWL: guncon->tx_data = guncon->sw & 0xff; break;
        case GUNCON_STATE_TX_SWH: guncon->tx_data = guncon->sw >> 8; break;
        case GUNCON_STATE_TX_XPL: guncon->tx_data = guncon->x & 0xff; break;
        case GUNCON_STATE_TX_XPH: guncon->tx_data = guncon->x >> 8; break;
        case GUNCON_STATE_TX_YPL: guncon->tx_data = guncon->y & 0xff; break;
        case GUNCON_STATE_TX_YPH: {
            // printf("guncon: read state %u (%s) -> %02x\n", guncon->state, states[guncon->state], guncon->y >> 8);

            guncon->tx_data_ready = 0;
            guncon->state = GUNCON_STATE_TX_HIZ;

            return guncon->y >> 8;
        } break;
    }

    // printf("guncon: read state %u (%s) -> %02x\n", guncon->state, states[guncon->state], guncon->tx_data);

    guncon->tx_data_ready = 1;
    guncon->state++;

    return guncon->tx_data;
}

void psxi_guncon_write(void* udata, uint16_t data) {
    psxi_guncon_t* guncon = (psxi_guncon_t*)udata;

    // printf("guncon: write %02x\n", data);

    (void)guncon;
}

void psxi_guncon_on_button_press(void* udata, uint32_t data) {
    psxi_guncon_t* guncon = (psxi_guncon_t*)udata;

    guncon->sw &= ~data;
}

void psxi_guncon_on_button_release(void* udata, uint32_t data) {
    psxi_guncon_t* guncon = (psxi_guncon_t*)udata;

    guncon->sw |= data;
}

void psxi_guncon_on_analog_change(void* udata, uint32_t axis, uint16_t data) {
    psxi_guncon_t* guncon = (psxi_guncon_t*)udata;

    switch (axis) {
        case PSXI_AX_GUNCON_X: {
            data *= (461.0f - 77.0f) / (float)guncon->sx;
            data += 77;

            guncon->x = data;
        } break;

        case PSXI_AX_GUNCON_Y: {
            data *= (248.0f - 25.0f) / (float)guncon->sy;
            data += 25;

            guncon->y = data;
        } break;

        case PSXI_AX_GUNCON_SX: {
            guncon->sx = data;
        } break;

        case PSXI_AX_GUNCON_SY: {
            guncon->sy = data;
        } break;
    }
}

int psxi_guncon_query_fifo(void* udata) {
    psxi_guncon_t* guncon = (psxi_guncon_t*)udata;

    return guncon->tx_data_ready;
}

void psxi_guncon_save_state(psxi_guncon_t* guncon, psx_state_writer_t* w) {
    psx_sw_i32(w, guncon->state);
    psx_sw_u16(w, guncon->sw);
    psx_sw_u8(w, guncon->tx_data);
    psx_sw_i32(w, guncon->tx_data_ready);
    psx_sw_u16(w, guncon->x);
    psx_sw_u16(w, guncon->y);
    psx_sw_u16(w, guncon->sx);
    psx_sw_u16(w, guncon->sy);
}

int psxi_guncon_load_state(psxi_guncon_t* guncon, psx_state_reader_t* r) {
    guncon->state = psx_sr_i32(r);
    guncon->sw = psx_sr_u16(r);
    guncon->tx_data = psx_sr_u8(r);
    guncon->tx_data_ready = psx_sr_i32(r);
    guncon->x = psx_sr_u16(r);
    guncon->y = psx_sr_u16(r);
    guncon->sx = psx_sr_u16(r);
    guncon->sy = psx_sr_u16(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (guncon->state < GUNCON_STATE_TX_HIZ || guncon->state > GUNCON_STATE_TX_YPH)
        return PSX_STATE_ERR_TRUNCATED;

    return PSX_STATE_OK;
}

void psxi_guncon_init_input(psxi_guncon_t* guncon, psx_input_t* input) {
    input->udata = guncon;
    input->kind = PSX_INPUT_KIND_GUNCON;
    input->write_func = psxi_guncon_write;
    input->read_func = psxi_guncon_read;
    input->on_button_press_func = psxi_guncon_on_button_press;
    input->on_button_release_func = psxi_guncon_on_button_release;
    input->on_analog_change_func = psxi_guncon_on_analog_change;
    input->query_fifo_func = psxi_guncon_query_fifo;
}

void psxi_guncon_destroy(psxi_guncon_t* guncon) {
    free(guncon);
}