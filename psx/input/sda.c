/*
    This file is part of the PSXE Emulator Project

    Sony PlayStation Standard Digital/Analog Controller Emulator
*/

#include "sda.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>

psxi_sda_t* psxi_sda_create(void) {
    return (psxi_sda_t*)malloc(sizeof(psxi_sda_t));
}

void psxi_sda_init(psxi_sda_t* sda, uint16_t model) {
    memset(sda, 0, sizeof(psxi_sda_t));

    sda->tx_data = 0xff;
    sda->tx_data_ready = 1;
    sda->prev_model = model;
    sda->model = model;
    sda->state = SDA_STATE_TX_HIZ;
    sda->sw = 0xffff;
    sda->sa_mode = SA_MODE_DIGITAL;
    sda->adc0 = 0x80;
    sda->adc1 = 0x80;
    sda->adc2 = 0x80;
    sda->adc3 = 0x80;
}

uint32_t psxi_sda_read(void* udata) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    switch (sda->state) {
        case SDA_STATE_TX_HIZ: sda->tx_data = 0xff; break;
        case SDA_STATE_TX_IDL: sda->tx_data = sda->model; break;
        case SDA_STATE_TX_IDH: sda->tx_data = 0x5a; break;
        case SDA_STATE_TX_SWL: sda->tx_data = sda->sw & 0xff; break;

        // Digital pad stops sending data here
        case SDA_STATE_TX_SWH: {
            if (sda->sa_mode == SA_MODE_ANALOG) {
                sda->tx_data_ready = 1;
                sda->state = SDA_STATE_TX_ADC0;
            } else {
                sda->tx_data_ready = 0;
                sda->state = SDA_STATE_TX_HIZ;
            }

            return sda->sw >> 8;
        } break;

        case SDA_STATE_TX_ADC0: sda->tx_data = sda->adc0; break;
        case SDA_STATE_TX_ADC1: sda->tx_data = sda->adc1; break;
        case SDA_STATE_TX_ADC2: sda->tx_data = sda->adc2; break;

        // Analog pad stops sending data here
        case SDA_STATE_TX_ADC3: {
            sda->tx_data_ready = 0;
            sda->state = SDA_STATE_TX_HIZ;

            if (sda->model == 0xf3)
                sda->model = sda->prev_model;

            return sda->adc3;
        } break;
        
    }

    // printf("  sda read %u -> %02x\n", sda->state, sda->tx_data);

    sda->tx_data_ready = 1;
    sda->state++;

    return sda->tx_data;
}

void psxi_sda_write(void* udata, uint16_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    // To-do: Handle TAP and MOT bytes here

    if (data == 0x43) {
        if (sda->sa_mode == SA_MODE_ANALOG) {
            sda->prev_model = sda->model;
            sda->model = 0xf3;
        } else {
            sda->tx_data = 0xff;
            sda->tx_data_ready = 0;
            sda->state = SDA_STATE_TX_HIZ;
        }
    }
}

void psxi_sda_set_analog_mode(psxi_sda_t* sda, int enabled) {
    const int mode = enabled ? SA_MODE_ANALOG : SA_MODE_DIGITAL;

    if (sda->sa_mode == mode)
        return;

    sda->sa_mode = mode;

    /* The pad we emulate is a DualShock, so its two IDs are fixed: 0x41 digital, 0x73
       analog. Two deliberate differences from how the ANALOG button used to do this:

         - 0x73 (SDA_MODEL_ANALOG_PAD), not 0x53. 0x53 is the SCPH-1110 analog JOYSTICK — a
           different device class. Both stream four ADC bytes so it mostly went unnoticed
           while analog mode was unreachable, but the config-mode command in
           psxi_sda_write() (0x43 -> 0xf3) only makes sense for the analog controller, and
           booting analog by default now shows this ID to every game.

         - the digital ID is restored from the constant, not from prev_model. prev_model is
           scratch space shared with the config-mode escape, so a game that entered config
           mode left 0x73 sitting in it and a later switch back to digital would have kept
           reporting the analog ID. */
    sda->model = (mode == SA_MODE_ANALOG) ? SDA_MODEL_ANALOG_PAD : SDA_MODEL_DIGITAL;
    sda->prev_model = sda->model;

    log_info("sda: Switched to %s mode", mode == SA_MODE_ANALOG ? "analog" : "digital");
}

void psxi_sda_on_button_press(void* udata, uint32_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    /* The pad's ANALOG button. Still a toggle — the front-end's analog_mode_default setting
       only picks the mode the pad STARTS in. */
    if (data == PSXI_SW_SDA_ANALOG) {
        psxi_sda_set_analog_mode(sda, sda->sa_mode != SA_MODE_ANALOG);

        return;
    }

    sda->sw &= ~data;
}

void psxi_sda_on_button_release(void* udata, uint32_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    sda->sw |= data;
}

// To-do: Implement analog mode
void psxi_sda_on_analog_change(void* udata, uint32_t axis, uint16_t data) {
    // Suppress warning until we implement analog mode
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    switch (axis) {
        case PSXI_AX_SDA_RIGHT_HORZ: sda->adc0 = data; break;
        case PSXI_AX_SDA_RIGHT_VERT: sda->adc1 = data; break;
        case PSXI_AX_SDA_LEFT_HORZ: sda->adc2 = data; break;
        case PSXI_AX_SDA_LEFT_VERT: sda->adc3 = data; break;
    }
}

int psxi_sda_query_fifo(void* udata) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    return sda->tx_data_ready;
}

void psxi_sda_save_state(psxi_sda_t* sda, psx_state_writer_t* w) {
    psx_sw_u8(w, sda->prev_model);
    psx_sw_u8(w, sda->model);
    psx_sw_i32(w, sda->state);
    psx_sw_i32(w, sda->sa_mode);
    psx_sw_u16(w, sda->sw);
    psx_sw_u8(w, sda->tx_data);
    psx_sw_i32(w, sda->tx_data_ready);
    psx_sw_u8(w, sda->adc0);
    psx_sw_u8(w, sda->adc1);
    psx_sw_u8(w, sda->adc2);
    psx_sw_u8(w, sda->adc3);
}

int psxi_sda_load_state(psxi_sda_t* sda, psx_state_reader_t* r) {
    sda->prev_model = psx_sr_u8(r);
    sda->model = psx_sr_u8(r);
    sda->state = psx_sr_i32(r);
    sda->sa_mode = psx_sr_i32(r);
    sda->sw = psx_sr_u16(r);
    sda->tx_data = psx_sr_u8(r);
    sda->tx_data_ready = psx_sr_i32(r);
    sda->adc0 = psx_sr_u8(r);
    sda->adc1 = psx_sr_u8(r);
    sda->adc2 = psx_sr_u8(r);
    sda->adc3 = psx_sr_u8(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (sda->state < SDA_STATE_TX_HIZ || sda->state > SDA_STATE_TX_ADC3)
        return PSX_STATE_ERR_TRUNCATED;

    return PSX_STATE_OK;
}

void psxi_sda_init_input(psxi_sda_t* sda, psx_input_t* input) {
    input->udata = sda;
    input->kind = PSX_INPUT_KIND_SDA;
    input->write_func = psxi_sda_write;
    input->read_func = psxi_sda_read;
    input->on_button_press_func = psxi_sda_on_button_press;
    input->on_button_release_func = psxi_sda_on_button_release;
    input->on_analog_change_func = psxi_sda_on_analog_change;
    input->query_fifo_func = psxi_sda_query_fifo;
}

void psxi_sda_destroy(psxi_sda_t* sda) {
    free(sda);
}