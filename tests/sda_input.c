#include <assert.h>
#include <stdio.h>

#include "psx/input/sda.h"
#include "psx/input/multitap.h"

static void poll_pad(psxi_sda_t* pad, uint8_t* bytes, unsigned count) {
    psx_input_t input = {0};
    psxi_sda_init_input(pad, &input);
    for (unsigned i = 0; i < count; ++i) {
        input.write_func(input.udata, i == 1 ? 0x42 : 0);
        bytes[i] = (uint8_t)input.read_func(input.udata);
    }
    assert(!input.query_fifo_func(input.udata));
}

static void digital_fallback(void) {
    psxi_sda_t pad;
    uint8_t reply[9];
    psxi_sda_init(&pad, SDA_MODEL_DIGITAL);
    psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_LEFT_HORZ, 0);
    psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_LEFT_VERT, 0);
    poll_pad(&pad, reply, 5);
    assert(reply[0] == 0xff && reply[1] == 0x41 && reply[2] == 0x5a);
    assert(reply[3] == (uint8_t)~(PSXI_SW_SDA_PAD_LEFT | PSXI_SW_SDA_PAD_UP));
    assert(reply[4] == 0xff && pad.sw == 0xffff);

    psxi_sda_on_button_press(&pad, PSXI_SW_SDA_PAD_LEFT | PSXI_SW_SDA_CROSS);
    psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_LEFT_HORZ, 128);
    psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_LEFT_VERT, 128);
    poll_pad(&pad, reply, 5);
    assert(reply[3] == (uint8_t)~PSXI_SW_SDA_PAD_LEFT);
    assert(reply[4] == (uint8_t)~(PSXI_SW_SDA_CROSS >> 8));

    psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_LEFT_HORZ, 255);
    psxi_sda_on_button_press(&pad, PSXI_SW_SDA_PAD_RIGHT);
    psxi_sda_on_button_release(&pad, PSXI_SW_SDA_PAD_RIGHT);
    poll_pad(&pad, reply, 5);
    assert((reply[3] & PSXI_SW_SDA_PAD_RIGHT) == 0);
    psxi_sda_on_button_release(&pad, PSXI_SW_SDA_PAD_LEFT | PSXI_SW_SDA_CROSS);
    psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_LEFT_HORZ, 128);
    psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_RIGHT_HORZ, 0);
    poll_pad(&pad, reply, 5);
    assert(reply[3] == 0xff && reply[4] == 0xff);

    for (unsigned x = 0; x < 256; ++x) {
        psxi_sda_on_analog_change(&pad, PSXI_AX_SDA_LEFT_HORZ, x);
        const uint16_t buttons = psxi_sda_button_state(&pad);
        assert(((buttons & PSXI_SW_SDA_PAD_LEFT) == 0) == (x <= 64));
        assert(((buttons & PSXI_SW_SDA_PAD_RIGHT) == 0) == (x >= 192));
    }

    psxi_sda_set_analog_mode(&pad, 1);
    poll_pad(&pad, reply, 9);
    assert(reply[1] == SDA_MODEL_ANALOG_PAD && reply[3] == 0xff && reply[4] == 0xff);
    assert(reply[5] == 0 && reply[6] == 128 && reply[7] == 255 && reply[8] == 128);
    psxi_sda_set_analog_mode(&pad, 0);
    poll_pad(&pad, reply, 5);
    assert((reply[3] & PSXI_SW_SDA_PAD_RIGHT) == 0);
}

static void multitap_fallback(void) {
    psxi_multitap_t tap;
    psx_input_t input = {0};
    uint8_t reply[PSXI_MULTITAP_PACKET_SIZE];
    const unsigned directions[] = {PSXI_SW_SDA_PAD_LEFT, PSXI_SW_SDA_PAD_RIGHT,
        PSXI_SW_SDA_PAD_UP, PSXI_SW_SDA_PAD_DOWN};
    psxi_multitap_init(&tap);
    psxi_multitap_init_input(&tap, &input);
    for (unsigned player = 0; player < 4; ++player) {
        psxi_multitap_analog_change(&tap, player,
            player < 2 ? PSXI_AX_SDA_LEFT_HORZ : PSXI_AX_SDA_LEFT_VERT, player % 2 ? 255 : 0);
    }
    for (unsigned i = 0; i < sizeof(reply); ++i) reply[i] = input.read_func(input.udata);
    assert(reply[1] == PSXI_MULTITAP_ID_LO && !input.query_fifo_func(input.udata));
    for (unsigned player = 0; player < 4; ++player) {
        const unsigned offset = 3 + player * 8;
        assert(reply[offset] == SDA_MODEL_DIGITAL);
        assert(reply[offset + 2] == (uint8_t)~directions[player]);
        assert(reply[offset + 4] == 0xff);
    }
}

int main(void) {
    digital_fallback();
    multitap_fallback();
    puts("SDA input serial and multitap tests passed");
    return 0;
}
