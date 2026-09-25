#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psx/psx.h"
#include "psx/input/sda.h"
#include "psx/input/multitap.h"
#include "psx/input/dualshock_state.h"
#include "psx/log.h"

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

static uint8_t exchange(psx_pad_t* port, uint8_t value) {
    psx_pad_write16(port, 0, value);
    return psx_pad_read8(port, 0);
}

static void begin(psx_pad_t* port) {
    psx_pad_write16(port, 10, 0);
    psx_pad_write16(port, 10, CTRL_TXEN | CTRL_JOUT);
    assert(exchange(port, 1) == 0xff);
}

static void packet(psx_pad_t* port, uint8_t command, const uint8_t params[6],
                   const uint8_t* expected, unsigned length) {
    begin(port);
    for (unsigned i = 1; i < length; ++i) {
        const uint8_t sent = i == 1 ? command : i >= 3 && params ? params[i - 3] : 0;
        const uint8_t got = exchange(port, sent);
        if (got != expected[i])
            fprintf(stderr, "command=%02x index=%u got=%02x expected=%02x\n", command, i, got, expected[i]);
        assert(got == expected[i]);
        assert((port->dest[0] != 0) == (i + 1 < length));
    }
}

static const uint8_t digital[] = {0xff, 0x41, 0x5a, 0xff, 0xff};
static const uint8_t analog[] = {0xff, 0x73, 0x5a, 0xff, 0xff, 0x80, 0x80, 0x80, 0x80};
static const uint8_t config[] = {0xff, 0xf3, 0x5a, 0, 0, 0, 0, 0, 0};
static const uint8_t enter_config[] = {1, 0, 0, 0, 0, 0};

static void dualshock_protocol(void) {
    psx_pad_t port;
    psxi_sda_t pad;
    psx_input_t input = {0};
    psx_pad_init(&port, NULL);
    psxi_sda_init(&pad, SDA_MODEL_DIGITAL);
    psxi_sda_init_input(&pad, &input);
    psx_pad_attach_joy(&port, 0, &input);
    packet(&port, 0x42, NULL, digital, sizeof(digital));
    packet(&port, 0x43, NULL, digital, sizeof(digital));
    assert(!pad.config_mode);

    begin(&port);
    assert(exchange(&port, 0x43) == 0x41);
    assert(exchange(&port, 0) == 0x5a);
    assert(exchange(&port, 1) == 0xff);
    psx_pad_write16(&port, 10, 0);
    assert(!pad.config_mode);
    packet(&port, 0x43, enter_config, digital, sizeof(digital));
    assert(pad.config_mode && pad.dualshock_enabled);
    const uint8_t identify_digital[] = {0xff, 0xf3, 0x5a, 1, 2, 0, 2, 1, 0};
    packet(&port, 0x45, NULL, identify_digital, sizeof(identify_digital));
    psx_pad_write16(&port, 10, CTRL_REST);
    assert(pad.config_mode);
    const uint8_t analog_lock[] = {1, 3, 0, 0, 0, 0};
    packet(&port, 0x44, analog_lock, config, sizeof(config));
    assert(pad.sa_mode == SA_MODE_ANALOG && pad.analog_locked);
    for (unsigned key = 0; key < 256; ++key) {
        const uint8_t params[] = {1, (uint8_t)key, 0, 0, 0, 0};
        packet(&port, 0x44, params, config, sizeof(config));
        assert(pad.sa_mode == SA_MODE_ANALOG && pad.analog_locked == ((key & 3) == 3));
    }
    const uint8_t identify_analog[] = {0xff, 0xf3, 0x5a, 1, 2, 1, 2, 1, 0};
    packet(&port, 0x45, NULL, identify_analog, sizeof(identify_analog));
    const uint8_t poll_config[] = {0xff, 0xf3, 0x5a, 0xff, 0xff, 0x80, 0x80, 0x80, 0x80};
    packet(&port, 0x42, NULL, poll_config, sizeof(poll_config));
    packet(&port, 0x43, NULL, config, sizeof(config));
    assert(!pad.config_mode);
    psxi_sda_on_button_press(&pad, PSXI_SW_SDA_ANALOG);
    packet(&port, 0x42, NULL, analog, sizeof(analog));

    begin(&port);
    assert(exchange(&port, 0x45) == 0xff && !port.dest[0]);
    packet(&port, 0x43, enter_config, analog, sizeof(analog));
    const uint8_t invalid_mode_unlock[] = {0xfe, 2, 0, 0, 0, 0};
    packet(&port, 0x44, invalid_mode_unlock, config, sizeof(config));
    assert(pad.sa_mode == SA_MODE_ANALOG && !pad.analog_locked);
    const uint8_t selector1[] = {1, 0, 0, 0, 0, 0};
    const uint8_t selector2[] = {2, 0, 0, 0, 0, 0};
    const uint8_t actuator0[] = {0xff, 0xf3, 0x5a, 0, 0, 1, 2, 0, 0x0a};
    const uint8_t actuator1[] = {0xff, 0xf3, 0x5a, 0, 0, 1, 1, 1, 0x14};
    packet(&port, 0x46, NULL, actuator0, sizeof(actuator0));
    packet(&port, 0x46, selector1, actuator1, sizeof(actuator1));
    packet(&port, 0x46, selector2, config, sizeof(config));
    const uint8_t combination[] = {0xff, 0xf3, 0x5a, 0, 0, 2, 0, 1, 0};
    packet(&port, 0x47, NULL, combination, sizeof(combination));
    packet(&port, 0x47, selector1, config, sizeof(config));
    const uint8_t mode0[] = {0xff, 0xf3, 0x5a, 0, 0, 0, 4, 0, 0};
    const uint8_t mode1[] = {0xff, 0xf3, 0x5a, 0, 0, 0, 7, 0, 0};
    packet(&port, 0x4c, NULL, mode0, sizeof(mode0));
    packet(&port, 0x4c, selector1, mode1, sizeof(mode1));
    packet(&port, 0x4c, selector2, config, sizeof(config));
    const uint8_t motors[] = {0, 1, 0xff, 0xff, 0xff, 0xff};
    const uint8_t empty_map[] = {0xff, 0xf3, 0x5a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t echo_map[] = {0xff, 0xf3, 0x5a, 0, 1, 0xff, 0xff, 0xff, 0xff};
    packet(&port, 0x4d, motors, empty_map, sizeof(empty_map));
    packet(&port, 0x4d, motors, echo_map, sizeof(echo_map));
    packet(&port, 0x43, NULL, config, sizeof(config));
    const uint8_t rumble[] = {1, 0x43, 0, 0, 0, 0};
    packet(&port, 0x42, rumble, analog, sizeof(analog));
    assert(pad.motor[0] == 255 && pad.motor[1] == 0x43 && !pad.config_mode);
    psxi_sda_on_button_press(&pad, PSXI_SW_SDA_ANALOG);
    assert(pad.sa_mode == SA_MODE_DIGITAL && pad.status == 0 && !pad.motor[0] && !pad.motor[1]);
    const uint8_t digital_status0[] = {0xff, 0x41, 0, 0xff, 0xff};
    packet(&port, 0x43, enter_config, digital_status0, sizeof(digital_status0));
    packet(&port, 0x45, NULL, identify_digital, sizeof(identify_digital));
    const uint8_t long_map[] = {0xff, 0xff, 0xff, 0xff, 0xff, 1};
    packet(&port, 0x4d, long_map, empty_map, sizeof(empty_map));
    packet(&port, 0x43, NULL, config, sizeof(config));
    const uint8_t digital_long[] = {0xff, 0x43, 0x5a, 0xff, 0xff, 0, 0, 0, 0};
    const uint8_t long_rumble[] = {0, 0, 0, 0, 0, 0x67};
    packet(&port, 0x42, long_rumble, digital_long, sizeof(digital_long));
    assert(pad.motor[1] == 0x67);
}

static void analog_axis_bytes(void) {
    psx_pad_t port;
    psxi_sda_t pad;
    psx_input_t input = {0};
    psx_pad_init(&port, NULL);
    psxi_sda_init(&pad, SDA_MODEL_DIGITAL);
    psxi_sda_init_input(&pad, &input);
    psx_pad_attach_joy(&port, 0, &input);
    packet(&port, 0x43, enter_config, digital, sizeof(digital));
    const uint8_t analog_lock[] = {1, 3, 0, 0, 0, 0};
    packet(&port, 0x44, analog_lock, config, sizeof(config));
    packet(&port, 0x43, NULL, config, sizeof(config));
    const uint32_t axes[] = {PSXI_AX_SDA_RIGHT_HORZ, PSXI_AX_SDA_RIGHT_VERT,
        PSXI_AX_SDA_LEFT_HORZ, PSXI_AX_SDA_LEFT_VERT};
    const uint8_t initial[] = {0x11, 0x22, 0x33, 0x44};
    psx_pad_button_press(&port, 0, PSXI_SW_SDA_L3 | PSXI_SW_SDA_CROSS);
    for (unsigned axis = 0; axis < 4; ++axis) {
        uint8_t expected[] = {0xff, 0x73, 0x5a, 0xfd, 0xbf, 0x11, 0x22, 0x33, 0x44};
        for (unsigned i = 0; i < 4; ++i)
            psx_pad_analog_change(&port, 0, axes[i], initial[i]);
        for (unsigned value = 0; value < 256; ++value) {
            psx_pad_analog_change(&port, 0, axes[axis], value);
            expected[5 + axis] = value;
            packet(&port, 0x42, NULL, expected, sizeof(expected));
        }
    }
    psx_pad_button_release(&port, 0, PSXI_SW_SDA_L3 | PSXI_SW_SDA_CROSS);
    const uint8_t positions[][2] = {{128, 128}, {128, 0}, {255, 0}, {255, 128},
        {255, 255}, {128, 255}, {0, 255}, {0, 128}, {0, 0}};
    for (unsigned left = 0; left < 9; ++left) {
        for (unsigned right = 0; right < 9; ++right) {
            uint8_t expected[] = {0xff, 0x73, 0x5a, 0xff, 0xff,
                positions[right][0], positions[right][1], positions[left][0], positions[left][1]};
            for (unsigned axis = 0; axis < 4; ++axis)
                psx_pad_analog_change(&port, 0, axes[axis], expected[5 + axis]);
            packet(&port, 0x42, NULL, expected, sizeof(expected));
        }
    }
}

static void analog_poll_trace(void) {
    psx_pad_t port;
    psxi_sda_t pad;
    psx_input_t input = {0};
    psx_pad_init(&port, NULL);
    psxi_sda_init(&pad, SDA_MODEL_DIGITAL);
    psxi_sda_init_input(&pad, &input);
    psx_pad_attach_joy(&port, 0, &input);
    psxi_sda_set_analog_mode(&pad, 1);
    const int level = log_get_level();
    log_set_level(LOG_INFO);
    packet(&port, 0x42, NULL, analog, sizeof(analog));
    assert(!port.trace[0][0].analog_polls);
    log_set_level(LOG_DEBUG);
    packet(&port, 0x42, NULL, analog, sizeof(analog));
    assert(port.trace[0][0].analog_polls == 1);
    assert(!memcmp(port.trace[0][0].last_poll, analog, sizeof(analog)));
    psx_pad_analog_change(&port, 0, PSXI_AX_SDA_RIGHT_HORZ, 0);
    psx_pad_analog_change(&port, 0, PSXI_AX_SDA_LEFT_VERT, 255);
    const uint8_t moved[] = {0xff, 0x73, 0x5a, 0xff, 0xff, 0, 128, 128, 255};
    packet(&port, 0x42, NULL, moved, sizeof(moved));
    assert(port.trace[0][0].analog_polls == 2);
    assert(!memcmp(port.trace[0][0].last_poll, moved, sizeof(moved)));
    const uint8_t minimum[] = {0, 128, 128, 128};
    const uint8_t maximum[] = {128, 128, 128, 255};
    assert(!memcmp(port.trace[0][0].axis_min, minimum, sizeof(minimum)));
    assert(!memcmp(port.trace[0][0].axis_max, maximum, sizeof(maximum)));
    begin(&port);
    assert(exchange(&port, 0x42) == 0x73);
    assert(exchange(&port, 0) == 0x5a);
    psx_pad_write16(&port, 10, 0);
    assert(port.trace[0][0].analog_polls == 2);
    assert(!memcmp(port.trace[0][0].last_poll, moved, sizeof(moved)));
    log_set_level(level);
}

static uint32_t read32(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void transfer_state_roundtrips(void) {
    const uint8_t commands[] = {0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x4c, 0x4d};
    for (unsigned command = 0; command < sizeof(commands); ++command) {
        for (unsigned split = 0; split <= 18; ++split) {
            psx_pad_t port[2];
            psxi_sda_t pad[2];
            psx_input_t input[2] = {{0}};
            for (unsigned i = 0; i < 2; ++i) {
                psx_pad_init(&port[i], NULL);
                psxi_sda_init(&pad[i], SDA_MODEL_DIGITAL);
                psxi_sda_init_input(&pad[i], &input[i]);
                psx_pad_attach_joy(&port[i], 0, &input[i]);
            }
            packet(&port[0], 0x43, enter_config, digital, sizeof(digital));
            psx_pad_write16(&port[0], 10, 0);
            psx_pad_write16(&port[0], 10, CTRL_TXEN | CTRL_JOUT);
            const uint8_t bytes[] = {1, commands[command], 0, 1, 3, 0xff, 0xff, 0xff, 0xff};
            for (unsigned i = 0; i < split; ++i) {
                if (i & 1) psx_pad_read8(&port[0], 0);
                else psx_pad_write16(&port[0], 0, bytes[i / 2]);
            }
            psx_state_writer_t base, ext;
            psx_state_reader_t reader;
            psx_sw_init(&base);
            psx_sw_init(&ext);
            psx_pad_save_state(&port[0], &base);
            psxi_dualshock_save_state(&port[0], &ext);
            psx_sr_init(&reader, base.buf, base.size);
            assert(psx_pad_load_state(&port[1], &reader) == PSX_STATE_OK);
            psx_sr_init(&reader, ext.buf, ext.size);
            assert(psxi_dualshock_load_state(&port[1], &reader, 1) == PSX_STATE_OK);
            for (unsigned i = split; i < sizeof(bytes) * 2; ++i) {
                if (i & 1) {
                    assert(psx_pad_read8(&port[0], 0) == psx_pad_read8(&port[1], 0));
                } else {
                    psx_pad_write16(&port[0], 0, bytes[i / 2]);
                    psx_pad_write16(&port[1], 0, bytes[i / 2]);
                }
            }
            assert(!memcmp(&pad[0], &pad[1], sizeof(pad[0])));
            psx_sw_free(&base);
            psx_sw_free(&ext);
        }
    }
}

static void write32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static size_t find_extension(const uint8_t* data, size_t size) {
    for (size_t pos = PSX_STATE_HEADER_SIZE; pos + PSX_STATE_SECTION_HEADER_SIZE <= size;) {
        if (read32(data + pos) == PSX_SS_PAD_EXT) return pos;
        assert(!read32(data + pos + 12));
        pos += PSX_STATE_SECTION_HEADER_SIZE + read32(data + pos + 8);
    }
    assert(0);
    return 0;
}

static void dualshock_save_states(void) {
    char bios_path[] = "/tmp/armsx-dualshock-bios-XXXXXX";
    int fd = mkstemp(bios_path);
    assert(fd >= 0 && ftruncate(fd, 512 * 1024) == 0 && close(fd) == 0);
    psx_t* machine = psx_create();
    assert(machine && !psx_init(machine, bios_path, NULL));
    assert(unlink(bios_path) == 0);
    psxi_sda_t* pad = psxi_sda_create();
    psx_input_t* input = psx_input_create();
    psxi_sda_init(pad, SDA_MODEL_DIGITAL);
    psxi_sda_init_input(pad, input);
    psx_pad_attach_joy(machine->pad, 0, input);
    packet(machine->pad, 0x43, enter_config, digital, sizeof(digital));
    const uint8_t analog_lock[] = {1, 3, 0, 0, 0, 0};
    packet(machine->pad, 0x44, analog_lock, config, sizeof(config));
    begin(machine->pad);
    assert(exchange(machine->pad, 0x45) == 0xf3);
    assert(exchange(machine->pad, 0) == 0x5a);
    assert(exchange(machine->pad, 0) == 1);

    void* data = NULL;
    size_t capacity = 0, size = 0;
    assert(psx_save_state_to_memory_ex(machine, &data, &capacity, &size, PSX_STATE_SAVE_NO_THUMBNAIL) == PSX_STATE_OK);
    psxi_sda_init(pad, SDA_MODEL_DIGITAL);
    assert(psx_load_state_from_memory(machine, data, size) == PSX_STATE_OK);
    assert(pad->config_mode && pad->analog_locked && pad->sa_mode == SA_MODE_ANALOG);
    const uint8_t remaining[] = {2, 1, 2, 1, 0};
    for (unsigned i = 0; i < sizeof(remaining); ++i)
        assert(exchange(machine->pad, 0) == remaining[i]);
    assert(!machine->pad->dest[0] && pad->config_mode);

    const size_t extension = find_extension(data, size);
    const size_t payload = extension + PSX_STATE_SECTION_HEADER_SIZE;
    const uint32_t length = read32((uint8_t*)data + extension + 8);
    uint8_t* corrupt = malloc(size);
    assert(corrupt);
    memcpy(corrupt, data, size);
    const psxi_sda_t before = *pad;
    machine->cpu->pc = 0x12345678;
    corrupt[payload + 4 + 1] = 2;
    assert(psx_load_state_from_memory(machine, corrupt, size) == PSX_STATE_ERR_TRUNCATED);
    assert(machine->cpu->pc == 0x12345678 && !memcmp(pad, &before, sizeof(before)));
    memcpy(corrupt, data, size);
    write32(corrupt + payload, PSX_INPUT_KIND_MULTITAP);
    assert(psx_load_state_from_memory(machine, corrupt, size) == PSX_STATE_ERR_GEOMETRY);
    assert(machine->cpu->pc == 0x12345678 && !memcmp(pad, &before, sizeof(before)));
    memcpy(corrupt, data, size);
    memmove(corrupt + payload + length - 1, corrupt + payload + length, size - payload - length);
    write32(corrupt + extension + 8, length - 1);
    assert(psx_load_state_from_memory(machine, corrupt, size - 1) == PSX_STATE_ERR_TRUNCATED);
    assert(machine->cpu->pc == 0x12345678 && !memcmp(pad, &before, sizeof(before)));

    memcpy(corrupt, data, size);
    memmove(corrupt + extension, corrupt + payload + length, size - payload - length);
    write32(corrupt + 16, read32(corrupt + 16) - 1);
    assert(psx_load_state_from_memory(machine, corrupt, size - PSX_STATE_SECTION_HEADER_SIZE - length) == PSX_STATE_OK);
    assert(!pad->config_mode && !pad->analog_locked && pad->sa_mode == SA_MODE_ANALOG);
    packet(machine->pad, 0x42, NULL, analog, sizeof(analog));
    psx_state_writer_t legacy;
    psx_sw_init(&legacy);
    psxi_sda_save_state(pad, &legacy);
    assert(legacy.size == 21);
    psx_sw_free(&legacy);
    free(corrupt);
    free(data);
    psx_destroy(machine);
}

int main(void) {
    digital_fallback();
    multitap_fallback();
    dualshock_protocol();
    analog_axis_bytes();
    analog_poll_trace();
    transfer_state_roundtrips();
    dualshock_save_states();
    puts("SDA digital, DualShock protocol, multitap and save-state tests passed");
    return 0;
}
