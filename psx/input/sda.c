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
    sda->status = 0x5a;
    memset(sda->rumble_map, 0xff, sizeof(sda->rumble_map));
}

void psxi_sda_reset_transfer(psxi_sda_t* sda) {
    sda->state = SDA_STATE_TX_HIZ;
    sda->tx_data = 0xff;
    sda->tx_data_ready = 0;
    sda->command = 0;
    sda->parameter = 0;
    sda->response_length = 0;
    memset(sda->response, 0, sizeof(sda->response));
}

uint16_t psxi_sda_button_state(const psxi_sda_t* sda) {
    uint16_t buttons = sda->sw;
    if (sda->sa_mode == SA_MODE_DIGITAL && !sda->config_mode) {
        if (sda->adc2 <= 64) buttons &= ~PSXI_SW_SDA_PAD_LEFT;
        if (sda->adc2 >= 192) buttons &= ~PSXI_SW_SDA_PAD_RIGHT;
        if (sda->adc3 <= 64) buttons &= ~PSXI_SW_SDA_PAD_UP;
        if (sda->adc3 >= 192) buttons &= ~PSXI_SW_SDA_PAD_DOWN;
    }
    return buttons;
}

static void sda_prepare_response(psxi_sda_t* sda, uint8_t command) {
    const int analog = sda->sa_mode == SA_MODE_ANALOG || sda->config_mode;
    unsigned halfwords = analog ? 3 : 1;
    if (!analog && sda->dualshock_enabled) {
        for (unsigned i = 2; i < sizeof(sda->rumble_map); ++i)
            if (sda->rumble_map[i] != 0xff)
                halfwords = 1 + i / 2;
    }
    memset(sda->response, 0, sizeof(sda->response));
    sda->command = command;
    sda->parameter = 0;
    sda->response_length = 3 + halfwords * 2;
    sda->response[0] = 0xff;
    sda->response[1] = sda->config_mode ? 0xf3 :
        (uint8_t)((sda->sa_mode == SA_MODE_ANALOG ? 0x70 : 0x40) | halfwords);
    sda->response[2] = sda->status;
    if (command == 0x42 || (command == 0x43 && !sda->config_mode)) {
        const uint16_t buttons = psxi_sda_button_state(sda);
        sda->response[3] = (uint8_t)buttons;
        sda->response[4] = (uint8_t)(buttons >> 8);
        if (analog) {
            sda->response[5] = sda->adc0;
            sda->response[6] = sda->adc1;
            sda->response[7] = sda->adc2;
            sda->response[8] = sda->adc3;
        }
    } else if (command == 0x45) {
        sda->response[3] = 1;
        sda->response[4] = 2;
        sda->response[5] = (uint8_t)sda->sa_mode;
        sda->response[6] = 2;
        sda->response[7] = 1;
    }
}

uint32_t psxi_sda_read(void* udata) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;
    if (sda->state == SDA_STATE_TX_HIZ) {
        psxi_sda_reset_transfer(sda);
        sda->state = SDA_STATE_TX_IDL;
        sda->tx_data_ready = 1;
        return 0xff;
    }
    if (!sda->command || sda->state >= sda->response_length) {
        psxi_sda_reset_transfer(sda);
        return 0xff;
    }
    const uint8_t data = sda->response[sda->state++];
    if (sda->state == sda->response_length) {
        if (sda->command == 0x43) {
            sda->config_mode = sda->parameter == 1;
            if (sda->config_mode) {
                sda->dualshock_enabled = 1;
                sda->status = 0x5a;
            }
        }
        psxi_sda_reset_transfer(sda);
    }
    sda->tx_data = data;
    return data;
}

void psxi_sda_write(void* udata, uint16_t value) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;
    const uint8_t data = (uint8_t)value;
    if (sda->state == SDA_STATE_TX_IDL) {
        if (data != 0x42 && data != 0x43 &&
            !(sda->config_mode && data >= 0x40 && data <= 0x4f)) {
            psxi_sda_reset_transfer(sda);
            return;
        }
        sda_prepare_response(sda, data);
        sda->tx_data_ready = 1;
        if (data == 0x44) {
            memset(sda->rumble_map, 0xff, sizeof(sda->rumble_map));
            memset(sda->motor, 0, sizeof(sda->motor));
        }
        return;
    }
    if (sda->state < SDA_STATE_TX_SWL || !sda->command)
        return;
    const unsigned index = (unsigned)sda->state - SDA_STATE_TX_SWL;
    if (index >= sizeof(sda->rumble_map))
        return;
    if (!index)
        sda->parameter = data;
    switch (sda->command) {
        case 0x42:
            if (sda->dualshock_enabled) {
                const uint8_t motor = sda->rumble_map[index];
                if (motor < 2)
                    sda->motor[motor] = motor ? data : (data & 1) * 255;
            } else if (index == 1) {
                sda->motor[0] = (sda->parameter & 0xc0) == 0x40 && (data & 1) ? 255 : 0;
            }
            break;
        case 0x44:
            if (!index && data <= 1)
                psxi_sda_set_analog_mode(sda, data);
            else if (index == 1)
                sda->analog_locked = (data & 3) == 3;
            break;
        case 0x46:
            if (!index && data <= 1) {
                sda->response[5] = 1;
                sda->response[6] = data ? 1 : 2;
                sda->response[7] = data;
                sda->response[8] = data ? 0x14 : 0x0a;
            }
            break;
        case 0x47:
            if (!index && !data) {
                sda->response[5] = 2;
                sda->response[7] = 1;
            }
            break;
        case 0x48:
            if (!index && data <= 1)
                sda->response[7] = 1;
            break;
        case 0x4c:
            if (!index && data <= 1)
                sda->response[6] = data ? 7 : 4;
            break;
        case 0x4d:
            sda->response[sda->state] = sda->rumble_map[index];
            sda->rumble_map[index] = data;
            if (index == 5) {
                for (unsigned motor = 0; motor < 2; ++motor)
                    if (!memchr(sda->rumble_map, motor, sizeof(sda->rumble_map)))
                        sda->motor[motor] = 0;
            }
            break;
    }
}

void psxi_sda_set_analog_mode(psxi_sda_t* sda, int enabled) {
    const int mode = enabled ? SA_MODE_ANALOG : SA_MODE_DIGITAL;

    if (sda->sa_mode == mode)
        return;

    sda->sa_mode = mode;

    sda->model = (mode == SA_MODE_ANALOG) ? SDA_MODEL_ANALOG_PAD : SDA_MODEL_DIGITAL;
    sda->prev_model = sda->model;

    log_info("sda: Switched to %s mode", mode == SA_MODE_ANALOG ? "analog" : "digital");
}

void psxi_sda_on_button_press(void* udata, uint32_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    if (data == PSXI_SW_SDA_ANALOG) {
        if (!sda->analog_locked && !sda->config_mode) {
            psxi_sda_set_analog_mode(sda, sda->sa_mode != SA_MODE_ANALOG);
            memset(sda->rumble_map, 0xff, sizeof(sda->rumble_map));
            memset(sda->motor, 0, sizeof(sda->motor));
            if (sda->dualshock_enabled)
                sda->status = 0;
        }

        return;
    }

    sda->sw &= ~data;
}

void psxi_sda_on_button_release(void* udata, uint32_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    sda->sw |= data;
}

void psxi_sda_on_analog_change(void* udata, uint32_t axis, uint16_t data) {
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
    memset(&sda->config_mode, 0, sizeof(*sda) - offsetof(psxi_sda_t, config_mode));
    sda->status = 0x5a;
    memset(sda->rumble_map, 0xff, sizeof(sda->rumble_map));
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

    if (sda->state < SDA_STATE_TX_HIZ || sda->state > SDA_STATE_TX_ADC3 ||
        sda->sa_mode < SA_MODE_DIGITAL || sda->sa_mode > SA_MODE_ANALOG ||
        sda->tx_data_ready < 0 || sda->tx_data_ready > 1)
        return PSX_STATE_ERR_TRUNCATED;

    if (sda->state > SDA_STATE_TX_IDL) {
        sda_prepare_response(sda, 0x42);
        if (sda->state >= sda->response_length)
            sda->response_length = 9;
    }
    if (sda->model == 0xf3)
        sda->model = sda->prev_model;

    return PSX_STATE_OK;
}

void psxi_sda_save_extended_state(const psxi_sda_t* sda, psx_state_writer_t* w) {
    psx_sw_u8(w, (uint8_t)sda->state);
    psx_sw_u8(w, sda->config_mode);
    psx_sw_u8(w, sda->analog_locked);
    psx_sw_u8(w, sda->dualshock_enabled);
    psx_sw_u8(w, sda->status);
    psx_sw_u8(w, sda->command);
    psx_sw_u8(w, sda->parameter);
    psx_sw_u8(w, sda->response_length);
    psx_sw_bytes(w, sda->response, sizeof(sda->response));
    psx_sw_bytes(w, sda->rumble_map, sizeof(sda->rumble_map));
    psx_sw_bytes(w, sda->motor, sizeof(sda->motor));
}

int psxi_sda_load_extended_state(psxi_sda_t* sda, psx_state_reader_t* r) {
    psxi_sda_t value = *sda;
    value.state = psx_sr_u8(r);
    value.config_mode = psx_sr_u8(r);
    value.analog_locked = psx_sr_u8(r);
    value.dualshock_enabled = psx_sr_u8(r);
    value.status = psx_sr_u8(r);
    value.command = psx_sr_u8(r);
    value.parameter = psx_sr_u8(r);
    value.response_length = psx_sr_u8(r);
    psx_sr_bytes(r, value.response, sizeof(value.response));
    psx_sr_bytes(r, value.rumble_map, sizeof(value.rumble_map));
    psx_sr_bytes(r, value.motor, sizeof(value.motor));
    if (r->error || value.state > SDA_STATE_TX_ADC3 || value.config_mode > 1 ||
        value.analog_locked > 1 || value.dualshock_enabled > 1 ||
        (value.config_mode && !value.dualshock_enabled) ||
        (value.status != 0 && value.status != 0x5a) ||
        (value.motor[0] != 0 && value.motor[0] != 255))
        return PSX_STATE_ERR_TRUNCATED;
    if (value.state == SDA_STATE_TX_HIZ ||
        (value.state == SDA_STATE_TX_IDL && !value.command)) {
        if (value.command || value.response_length)
            return PSX_STATE_ERR_TRUNCATED;
    } else if ((value.response_length != 5 && value.response_length != 7 && value.response_length != 9) ||
               value.state >= value.response_length || value.command < 0x40 || value.command > 0x4f ||
               (!value.config_mode && value.command != 0x42 && value.command != 0x43) ||
               (value.config_mode && value.response_length != 9)) {
        return PSX_STATE_ERR_TRUNCATED;
    }
    *sda = value;
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
