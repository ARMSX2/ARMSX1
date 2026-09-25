#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "psx/dev/mcd.h"
#include "psx/dev/pad.h"
#include "psx/log.h"

static char sio_lines[8][1024];
static unsigned sio_line_count;

static void capture_sio(log_Event* event) {
    if (strncmp(event->fmt, "sio0:", 5))
        return;
    assert(sio_line_count < 8);
    va_list args;
    va_copy(args, event->ap);
    int size = vsnprintf(sio_lines[sio_line_count], sizeof(sio_lines[0]), event->fmt, args);
    va_end(args);
    assert(size > 0 && (size_t)size < sizeof(sio_lines[0]));
    ++sio_line_count;
}

static unsigned transfer(psx_pad_t* pad, unsigned input) {
    psx_pad_write16(pad, 0, input);
    return psx_pad_read8(pad, 0);
}

static void poll_both_slots(psx_pad_t* pad) {
    psx_pad_write16(pad, 10, 0);
    psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT);
    assert(transfer(pad, 1) == 0xff);
    assert(transfer(pad, 0x42) == 0x41);
    assert(transfer(pad, 0) == 0x5a);
    assert(transfer(pad, 0) == 0xff);
    assert(transfer(pad, 0) == 0xff);
    psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT | CTRL_SLOT);
    assert(transfer(pad, 1) == 0xff);
    psx_pad_write16(pad, 10, 0);
}

static uint32_t trace_stub_read(void* unused) { (void)unused; return 0xff; }
static void trace_stub_write(void* unused, uint16_t data) { (void)unused; (void)data; }
static int trace_stub_query(void* unused) { (void)unused; return 1; }

static void test_sio_trace(void) {
    assert(log_add_callback(capture_sio, NULL, LOG_DEBUG) == 0);
    log_set_quiet(true);
    log_set_level(LOG_INFO);
    psx_pad_t* pad = psx_pad_create();
    psx_pad_init(pad, NULL);
    psxi_sda_t* joy = psxi_sda_create();
    psxi_sda_init(joy, SDA_MODEL_DIGITAL);
    psx_input_t* input = psx_input_create();
    psx_input_init(input);
    psxi_sda_init_input(joy, input);
    psx_pad_attach_joy(pad, 0, input);
    for (unsigned i = 0; i < 1000; ++i) poll_both_slots(pad);
    psx_pad_update(pad, 33868800);
    assert(!sio_line_count && !pad->trace_pending && !pad->trace_open && !pad->trace_cycles);

    log_set_level(LOG_DEBUG);
    for (unsigned i = 0; i < 1000; ++i) poll_both_slots(pad);
    assert(!sio_line_count);
    assert(pad->trace[0][0].commands[0x42] == 1000);
    assert(pad->trace[1][0].commands[0] == 1000);
    assert(pad->trace[1][0].incomplete[0] == 1000);
    psx_state_writer_t before, after;
    psx_sw_init(&before);
    psx_sw_init(&after);
    psx_pad_save_state(pad, &before);
    psx_pad_update(pad, 33868799);
    assert(!sio_line_count);
    psx_pad_update(pad, 1);
    psx_pad_save_state(pad, &after);
    assert(before.size == after.size && !memcmp(before.buf, after.buf, before.size));
    psx_sw_free(&before);
    psx_sw_free(&after);
    assert(sio_line_count == 2 && !pad->trace_pending);
    assert(strstr(sio_lines[0], "slot0 JOY transactions=1000 complete=1000 incomplete=0 bytes=4..4"));
    assert(strstr(sio_lines[0], "[42:1000/0]"));
    assert(strstr(sio_lines[1], "slot1 JOY transactions=1000 complete=0 incomplete=1000 bytes=0..0"));
    assert(strstr(sio_lines[1], "[00:1000/1000]"));
    for (unsigned slot = 0; slot < 2; ++slot) {
        psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT | (slot ? CTRL_SLOT : 0));
        assert(transfer(pad, 0x81) == 0xff);
        psx_pad_write16(pad, 10, 0);
    }
    assert(sio_line_count == 2);
    psx_pad_update(pad, 33868800);
    assert(sio_line_count == 4);
    assert(strstr(sio_lines[2], "slot0 MCD transactions=1 complete=0 incomplete=1"));
    assert(strstr(sio_lines[3], "slot1 MCD transactions=1 complete=0 incomplete=1"));
    psx_pad_destroy(pad);
    assert(sio_line_count == 4);

    pad = psx_pad_create();
    psx_pad_init(pad, NULL);
    input = psx_input_create();
    psx_input_init(input);
    input->read_func = trace_stub_read;
    input->write_func = trace_stub_write;
    input->query_fifo_func = trace_stub_query;
    psx_pad_attach_joy(pad, 0, input);
    for (unsigned cmd = 0; cmd < 256; ++cmd) {
        psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT);
        transfer(pad, 1);
        transfer(pad, cmd);
        psx_pad_write16(pad, 10, 0);
    }
    assert(sio_line_count == 4);
    psx_pad_update(pad, 33868800);
    assert(sio_line_count == 5);
    assert(strstr(sio_lines[4], "transactions=256 complete=0 incomplete=256"));
    assert(strstr(sio_lines[4], "other_commands=240 other_total=240 other_incomplete=240"));
    psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT);
    transfer(pad, 1);
    transfer(pad, 0x52);
    psx_pad_update(pad, 33868800);
    assert(sio_line_count == 6 && strstr(sio_lines[5], "open slot0 JOY cmd=52 bytes=1"));
    psx_pad_destroy(pad);
    assert(sio_line_count == 7 && strstr(sio_lines[6], "[52:1/1]"));
    log_set_level(LOG_WARN);
    log_set_quiet(false);
}

static void command(psx_pad_t* pad, unsigned cmd) {
    psx_pad_write16(pad, 10, 0);
    psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT);
    assert(transfer(pad, 0x81) == 0xff);
    assert(transfer(pad, cmd) == pad->mcd_slot[0]->flag);
    assert(transfer(pad, 0) == 0x5a);
    assert(transfer(pad, 0) == 0x5d);
}

static void address(psx_pad_t* pad, unsigned sector) {
    assert(transfer(pad, sector >> 8) == 0);
    assert(transfer(pad, sector & 255) == (sector >> 8));
}

static void write_sector(psx_pad_t* pad, unsigned sector, unsigned seed, int bad_checksum) {
    command(pad, 'W');
    address(pad, sector);
    unsigned checksum = (sector >> 8) ^ (sector & 255);
    unsigned previous = sector & 255;
    for (unsigned i = 0; i < 128; ++i) {
        unsigned byte = (seed + i) & 255;
        assert(transfer(pad, byte) == previous);
        checksum ^= byte;
        previous = byte;
    }
    assert(transfer(pad, checksum ^ bad_checksum) == previous);
    assert(transfer(pad, 0) == 0x5c);
    assert(transfer(pad, 0) == 0x5d);
    assert(transfer(pad, 0) == (sector >= 1024 ? 0xff : (bad_checksum ? 'N' : 'G')));
    assert(pad->dest[0] == 0);
}

static void read_sector(psx_pad_t* pad, unsigned sector, unsigned seed) {
    command(pad, 'R');
    address(pad, sector);
    assert(transfer(pad, 0) == 0x5c);
    assert(transfer(pad, 0) == 0x5d);
    assert(transfer(pad, 0) == (sector >> 8));
    assert(transfer(pad, 0) == (sector & 255));
    unsigned checksum = (sector >> 8) ^ (sector & 255);
    for (unsigned i = 0; i < 128; ++i) {
        unsigned byte = (seed + i) & 255;
        assert(transfer(pad, 0) == byte);
        checksum ^= byte;
    }
    assert(transfer(pad, 0) == checksum);
    assert(transfer(pad, 0) == 'G');
    assert(pad->dest[0] == 0);
}

int main(void) {
    test_sio_trace();
    char directory[] = "/tmp/armsx-memory-card-XXXXXX";
    assert(mkdtemp(directory));
    char path[256];
    snprintf(path, sizeof(path), "%s/card.mcd", directory);
    psx_pad_t* pad = psx_pad_create();
    psx_pad_init(pad, NULL);
    psxi_sda_t* joy = psxi_sda_create();
    psxi_sda_init(joy, SDA_MODEL_DIGITAL);
    psx_input_t* input = psx_input_create();
    psx_input_init(input);
    psxi_sda_init_input(joy, input);
    psx_pad_attach_joy(pad, 0, input);
    psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT);
    assert(transfer(pad, 0x01) == 0xff);
    assert(transfer(pad, 0x42) == 0x41);
    psx_pad_write16(pad, 10, CTRL_REST);
    psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT);
    assert(transfer(pad, 0x01) == 0xff);
    assert(transfer(pad, 0x42) == 0x41);
    psx_pad_write16(pad, 10, 0);
    psx_pad_write16(pad, 10, CTRL_TXEN | CTRL_JOUT);
    assert(transfer(pad, 0x01) == 0xff);
    assert(transfer(pad, 0x42) == 0x41);
    assert(transfer(pad, 0) == 0x5a);
    assert(psx_pad_attach_mcd(pad, 0, path) == 0);
    psx_mcd_t* card = pad->mcd_slot[0];
    assert(card->buf[0] == 'M' && card->buf[1] == 'C');
    for (unsigned sector = 0; sector < 36; ++sector) {
        unsigned checksum = 0;
        for (unsigned i = 0; i < 128; ++i) checksum ^= card->buf[sector * 128 + i];
        assert(checksum == 0);
        if (sector > 0 && sector < 16) assert(card->buf[sector * 128] == 0xa0);
    }
    command(pad, 'S');
    unsigned id[] = {0x5c, 0x5d, 4, 0, 0, 0x80};
    for (unsigned i = 0; i < sizeof(id) / sizeof(id[0]); ++i) assert(transfer(pad, 0) == id[i]);
    assert(pad->dest[0] == 0 && card->flag == 8);
    write_sector(pad, 0, 1, 0);
    write_sector(pad, 511, 2, 0);
    write_sector(pad, 512, 3, 0);
    write_sector(pad, 1023, 4, 0);
    read_sector(pad, 0, 1);
    read_sector(pad, 511, 2);
    read_sector(pad, 512, 3);
    read_sector(pad, 1023, 4);
    assert(card->flag == 0);
    write_sector(pad, 1024, 99, 0);
    read_sector(pad, 0, 1);
    write_sector(pad, 6, 20, 1);
    command(pad, 'R');
    address(pad, 1024);
    assert(transfer(pad, 0) == 0x5c);
    assert(transfer(pad, 0) == 0x5d);
    assert(transfer(pad, 0) == 0xff);
    assert(transfer(pad, 0) == 0xff && pad->dest[0] == 0);

    command(pad, 'R');
    address(pad, 1023);
    for (int i = 0; i < 4; ++i) transfer(pad, 0);
    for (int i = 0; i < 10; ++i) transfer(pad, 0);
    psx_state_writer_t writer;
    psx_sw_init(&writer);
    psx_mcd_save_state(card, &writer);
    psx_state_reader_t reader;
    psx_sr_init(&reader, writer.buf, writer.size);
    psx_mcd_reset(card);
    assert(psx_mcd_load_state(card, &reader) == PSX_STATE_OK);
    assert(card->addr == 1023 * 128 + 10);
    assert(psx_mcd_read(card) == 14);
    writer.buf[8] = writer.buf[9] = writer.buf[10] = writer.buf[11] = 0;
    psx_sr_init(&reader, writer.buf, writer.size);
    assert(psx_mcd_load_state(card, &reader) == PSX_STATE_ERR_TRUNCATED);
    psx_sw_free(&writer);

    psx_pad_write16(pad, 10, CTRL_REST);
    assert(pad->dest[0] == 0 && card->state == MCD_STATE_TX_HIZ);
    command(pad, 'S');
    for (unsigned i = 0; i < sizeof(id) / sizeof(id[0]); ++i) transfer(pad, 0);
    psx_mcd_update(card, 33868800);
    assert(!card->dirty);
    psx_mcd_t* reopened = psx_mcd_create();
    assert(psx_mcd_init(reopened, path) == 0);
    assert(reopened->buf[1023 * 128] == 4 && reopened->buf[0] == 1);
    psx_mcd_destroy(reopened);
    char temporary[260];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    assert(mkdir(temporary, 0700) == 0);
    write_sector(pad, 1023, 9, 0);
    assert(psx_mcd_flush(card) != 0 && card->dirty);
    reopened = psx_mcd_create();
    assert(psx_mcd_init(reopened, path) == 0 && reopened->buf[1023 * 128] == 4);
    psx_mcd_destroy(reopened);
    assert(rmdir(temporary) == 0);
    assert(psx_mcd_flush(card) == 0 && !card->dirty);
    psx_pad_destroy(pad);

    FILE* file = fopen(path, "wb");
    assert(file && fwrite("preserve", 1, 8, file) == 8 && fclose(file) == 0);
    card = psx_mcd_create();
    assert(psx_mcd_init(card, path) != 0);
    psx_mcd_destroy(card);
    struct stat info;
    assert(stat(path, &info) == 0 && info.st_size == 8);
    file = fopen(path, "rb");
    char preserved[8];
    assert(file && fread(preserved, 1, 8, file) == 8 && fclose(file) == 0);
    assert(memcmp(preserved, "preserve", 8) == 0);
    assert(unlink(path) == 0 && rmdir(directory) == 0);
    puts("memory card protocol, address range, persistence, state and invalid-file checks passed");
    return 0;
}
