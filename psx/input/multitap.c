/*
    This file is part of the PSXE Emulator Project

    Sony PlayStation Multitap emulator. Protocol notes live in multitap.h.
*/

#include "multitap.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>

/* Read command. Anything else ends the transfer rather than being answered with
   made-up bytes — see multitap_write(). */
#define MULTITAP_CMD_READ 0x42

psxi_multitap_t* psxi_multitap_create(void) {
    return (psxi_multitap_t*)malloc(sizeof(psxi_multitap_t));
}

void psxi_multitap_init(psxi_multitap_t* tap) {
    int i;

    memset(tap, 0, sizeof(psxi_multitap_t));

    for (i = 0; i < PSXI_MULTITAP_SLOTS; i++)
        psxi_sda_init(&tap->sub[i], SDA_MODEL_DIGITAL);

    tap->pos = 0;
    tap->tx_ready = 0;
    tap->last_cmd = 0;
}

void psxi_multitap_set_analog_mode(psxi_multitap_t* tap, int enabled) {
    int i;

    for (i = 0; i < PSXI_MULTITAP_SLOTS; i++)
        psxi_sda_set_analog_mode(&tap->sub[i], enabled);
}

/* One sub-pad's eight bytes. The sub-pads are polled through their FIELDS rather
   than through psxi_sda_read(): that function is a transfer state machine driven
   by the host's own byte clock, and the tap has to produce a whole packet in one
   go without disturbing it. */
static void multitap_pack_slot(const psxi_sda_t* sda, uint8_t* out) {
    out[0] = sda->model;
    out[1] = 0x5a;
    out[2] = (uint8_t)(sda->sw & 0xff);
    out[3] = (uint8_t)((sda->sw >> 8) & 0xff);

    if (sda->sa_mode == SA_MODE_ANALOG) {
        out[4] = sda->adc0;
        out[5] = sda->adc1;
        out[6] = sda->adc2;
        out[7] = sda->adc3;
    } else {
        /* A digital pad stops after the two button bytes. The tap's block is a
           fixed eight bytes, so the rest is the idle line level. */
        out[4] = 0xff;
        out[5] = 0xff;
        out[6] = 0xff;
        out[7] = 0xff;
    }
}

static void multitap_build_packet(psxi_multitap_t* tap) {
    int i;

    tap->buf[0] = 0xff; /* hi-Z: the reply to the 01h address byte */
    tap->buf[1] = PSXI_MULTITAP_ID_LO;
    tap->buf[2] = PSXI_MULTITAP_ID_HI;

    for (i = 0; i < PSXI_MULTITAP_SLOTS; i++)
        multitap_pack_slot(&tap->sub[i], &tap->buf[3 + (i * 8)]);
}

static uint32_t multitap_read(void* udata) {
    psxi_multitap_t* tap = (psxi_multitap_t*)udata;
    uint8_t data;

    /* Start of a transfer: latch every sub-pad's buttons now, so all four
       players are sampled at the same instant. */
    if (tap->pos == 0)
        multitap_build_packet(tap);

    data = tap->buf[tap->pos];

    tap->pos++;

    if (tap->pos >= PSXI_MULTITAP_PACKET_SIZE) {
        tap->pos = 0;
        tap->tx_ready = 0;
    } else {
        tap->tx_ready = 1;
    }

    return data;
}

static void multitap_write(void* udata, uint16_t data) {
    psxi_multitap_t* tap = (psxi_multitap_t*)udata;

    /* The 01h address byte is consumed by psx/dev/pad.c and never arrives here,
       so the first byte this sees is the command — at which point exactly one
       byte (the hi-Z) has been read back. */
    if (tap->pos != 1)
        return;

    tap->last_cmd = (uint8_t)(data & 0xff);

    if (tap->last_cmd == MULTITAP_CMD_READ)
        return;

    /* Not a poll. The config-mode escape (43h) and the rumble/motor commands are
       addressed to an individual pad and have no meaning to the tap's packet;
       answering them with 5Ah and 32 button bytes would be worse than not
       answering. End the transfer, which the SIO reads as "no device replied",
       and let the next poll start clean.

       Consequence worth knowing: a game that forces analog mode through 43h gets
       nowhere with a tap plugged in, so [input] analog_mode_default is what
       decides the sub-pads' mode. */
    tap->pos = 0;
    tap->tx_ready = 0;
}

static int multitap_query_fifo(void* udata) {
    psxi_multitap_t* tap = (psxi_multitap_t*)udata;

    return tap->tx_ready;
}

/* The vtable's per-device entry points address slot A. Slots B..D are reached
   through psxi_multitap_button_press() and friends, because psx_input_t has no
   room for a player index — see psx_pad_button_press_player() in psx/dev/pad.c. */
static void multitap_on_button_press(void* udata, uint32_t data) {
    psxi_multitap_button_press((psxi_multitap_t*)udata, 0, data);
}

static void multitap_on_button_release(void* udata, uint32_t data) {
    psxi_multitap_button_release((psxi_multitap_t*)udata, 0, data);
}

static void multitap_on_analog_change(void* udata, uint32_t axis, uint16_t data) {
    psxi_multitap_analog_change((psxi_multitap_t*)udata, 0, axis, data);
}

void psxi_multitap_button_press(psxi_multitap_t* tap, int slot, uint32_t data) {
    if (!tap || slot < 0 || slot >= PSXI_MULTITAP_SLOTS)
        return;

    psxi_sda_on_button_press(&tap->sub[slot], data);
}

void psxi_multitap_button_release(psxi_multitap_t* tap, int slot, uint32_t data) {
    if (!tap || slot < 0 || slot >= PSXI_MULTITAP_SLOTS)
        return;

    psxi_sda_on_button_release(&tap->sub[slot], data);
}

void psxi_multitap_analog_change(psxi_multitap_t* tap, int slot, uint32_t axis, uint16_t data) {
    if (!tap || slot < 0 || slot >= PSXI_MULTITAP_SLOTS)
        return;

    psxi_sda_on_analog_change(&tap->sub[slot], axis, data);
}

void psxi_multitap_save_state(psxi_multitap_t* tap, psx_state_writer_t* w) {
    int i;

    psx_sw_i32(w, tap->pos);
    psx_sw_i32(w, tap->tx_ready);
    psx_sw_u8(w, tap->last_cmd);

    /* The packet itself is derived from the sub-pads, but a state taken mid-
       transfer has to resume mid-transfer with the SAME bytes the game was part
       way through reading — rebuilding it would hand it a packet from a
       different instant. */
    psx_sw_bytes(w, tap->buf, sizeof(tap->buf));

    for (i = 0; i < PSXI_MULTITAP_SLOTS; i++)
        psxi_sda_save_state(&tap->sub[i], w);
}

int psxi_multitap_load_state(psxi_multitap_t* tap, psx_state_reader_t* r) {
    int i;
    int rc;

    tap->pos = psx_sr_i32(r);
    tap->tx_ready = psx_sr_i32(r);
    tap->last_cmd = psx_sr_u8(r);

    psx_sr_bytes(r, tap->buf, sizeof(tap->buf));

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (tap->pos < 0 || tap->pos >= PSXI_MULTITAP_PACKET_SIZE)
        return PSX_STATE_ERR_TRUNCATED;

    for (i = 0; i < PSXI_MULTITAP_SLOTS; i++) {
        rc = psxi_sda_load_state(&tap->sub[i], r);

        if (rc != PSX_STATE_OK)
            return rc;
    }

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psxi_multitap_init_input(psxi_multitap_t* tap, psx_input_t* input) {
    input->udata = tap;
    input->kind = PSX_INPUT_KIND_MULTITAP;
    input->write_func = multitap_write;
    input->read_func = multitap_read;
    input->on_button_press_func = multitap_on_button_press;
    input->on_button_release_func = multitap_on_button_release;
    input->on_analog_change_func = multitap_on_analog_change;
    input->query_fifo_func = multitap_query_fifo;
}

void psxi_multitap_destroy(psxi_multitap_t* tap) {
    /* The sub-pads live inside the struct, so this really is one free(). */
    free(tap);
}
