#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx/dev/cdrom/cdrom.h"
#include "psx/perf.h"

static unsigned reads;
static uint32_t last_read;
static uint32_t end_lba;
static uint32_t match_lba;
static int interleave;
static uint8_t coding;
static int failures;
static psx_disc_t disc;

static void check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "XA AUDIO failed: %s\n", message);
        ++failures;
    }
}

int psx_disc_read(psx_disc_t* source, uint32_t lba, void* buffer) {
    (void)source;
    ++reads;
    last_read = lba;
    if (lba >= end_lba)
        return TS_FAR;
    uint8_t* sector = buffer;
    memset(sector, 0, CD_SECTOR_SIZE);
    sector[0x10] = 1;
    sector[0x11] = (lba == match_lba || (interleave && (lba - 150) % interleave == 0)) ? 0 : 1;
    sector[0x12] = 0x64;
    sector[0x13] = coding;
    return TS_DATA;
}

int psx_disc_get_track_number(psx_disc_t* source, uint32_t lba) {
    (void)source;
    (void)lba;
    return 1;
}

int psx_disc_get_track_lba(psx_disc_t* source, int track) {
    (void)source;
    (void)track;
    return 150;
}

void psx_ic_irq(psx_ic_t* ic, int irq) { (void)ic; (void)irq; }
uint8_t cdrom_get_stat(psx_cdrom_t* cdrom) { (void)cdrom; return 0; }
void cdrom_set_int(psx_cdrom_t* cdrom, int irq) { (void)cdrom; (void)irq; }

static psx_cdrom_t* setup(void) {
    psx_cdrom_t* cdrom = calloc(1, sizeof(*cdrom));
    if (!cdrom) abort();
    cdrom->disc = &disc;
    cdrom->mode = MODE_XA_ADPCM | MODE_XA_FILTER;
    cdrom->xa_playing = 1;
    cdrom->read_ongoing = 1;
    cdrom->xa_file = 1;
    cdrom->lba = cdrom->xa_lba = 150;
    cdrom->vol[0] = cdrom->vol[2] = 128;
    reads = 0;
    last_read = 0;
    end_lba = 200000;
    match_lba = UINT32_MAX;
    interleave = 0;
    coding = 1;
    psx_audio_diag_set_enabled(1);
    return cdrom;
}

static void test_transport_boundary(void) {
    psx_cdrom_t* cdrom = setup();
    int16_t output[8];
    memset(output, 0x55, sizeof(output));
    psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
    check(reads == 1 && last_read == 150 && cdrom->xa_lba == 151, "missing channel cannot scan ahead of transport");
    check(cdrom->xa_playing == 1, "waiting for a channel keeps XA active");
    check(g_psx_audio_diag.xa_stop_far == 0, "missing future channel is not disc EOF");
    for (size_t i = 0; i < 8; ++i) check(output[i] == 0, "deferred audio is silence");
    psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
    check(reads == 1, "waiting on the same transport position does not reread sectors");
    match_lba = 158;
    cdrom->lba = 157;
    psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
    check(reads == 8 && cdrom->xa_lba == 158, "only newly available interleaved sectors are walked");
    cdrom->lba = 158;
    psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
    check(reads == 9 && cdrom->xa_remaining_samples > 0, "matching sector resumes when transport reaches it");
    free(cdrom);
}

static void test_catch_up_bound(void) {
    psx_cdrom_t* cdrom = setup();
    int16_t output[2] = {1, 1};
    cdrom->lba = 199999;
    psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
    check(reads > 0 && reads <= 32, "stale read position has bounded work per audio request");
    check(cdrom->xa_playing && output[0] == 0 && output[1] == 0, "bounded catch-up defers without ending XA");
    check(g_psx_audio_diag.xa_walk_peak == reads, "deferred scan contributes to diagnostic walk peak");
    free(cdrom);
}

static void test_partial_tail(void) {
    psx_cdrom_t* cdrom = setup();
    int16_t output[8];
    memset(output, 0x55, sizeof(output));
    cdrom->xa_buf[0x13] = 1;
    cdrom->xa_remaining_samples = 2;
    cdrom->xa_left_resample_buf[0] = cdrom->xa_left_resample_buf[1] = 1234;
    cdrom->xa_right_resample_buf[0] = cdrom->xa_right_resample_buf[1] = -1234;
    cdrom->xa_lba = 151;
    cdrom->cdda_playing = 1;
    cdrom->cdda_remaining_samples = 8;
    cdrom->cdda_buf[0] = 999;
    psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
    check(output[0] == 1234 && output[1] == -1234 && output[2] == 1234 && output[3] == -1234,
          "already decoded samples survive a deferred fetch");
    check(output[4] == 0 && output[5] == 0 && output[6] == 0 && output[7] == 0,
          "only the unavailable tail is silenced");
    check(reads == 0 && cdrom->xa_playing, "decoded tail cannot advance future disc position");
    check(cdrom->cdda_sample_index == 0, "deferred XA cannot consume CDDA samples");
    free(cdrom);
}

static void test_terminal_end(void) {
    psx_cdrom_t* cdrom = setup();
    int16_t output[8];
    memset(output, 0x55, sizeof(output));
    end_lba = 150;
    cdrom->xa_buf[0x13] = 1;
    cdrom->xa_remaining_samples = 2;
    cdrom->xa_left_resample_buf[0] = cdrom->xa_left_resample_buf[1] = 1234;
    cdrom->xa_right_resample_buf[0] = cdrom->xa_right_resample_buf[1] = -1234;
    cdrom->cdda_playing = 1;
    cdrom->cdda_remaining_samples = 8;
    cdrom->cdda_buf[0] = 999;
    psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
    check(reads == 1 && !cdrom->xa_playing, "actual EOF ends XA");
    check(output[0] == 1234 && output[1] == -1234 && output[2] == 1234 && output[3] == -1234,
          "decoded prefix survives EOF");
    for (size_t i = 4; i < 8; ++i) check(output[i] == 0, "EOF clears the unavailable tail");
    check(cdrom->cdda_sample_index == 0, "terminal XA cannot overwrite decoded audio with CDDA");
    free(cdrom);
}

static void test_interleaved_clock(unsigned sectors_per_second, int interval, uint8_t format) {
    psx_cdrom_t* cdrom = setup();
    int16_t output[882 * 2];
    interleave = interval;
    coding = format;
    const unsigned sector_samples = ((format & 1) ? XA_STEREO_RESAMPLE_SIZE : XA_MONO_RESAMPLE_SIZE) *
                                    ((format & 4) ? 2 : 1);
    for (unsigned frame = 0; frame < 100; ++frame) {
        cdrom->lba = 150 + (frame + 1) * sectors_per_second / 50;
        psx_cdrom_get_audio_samples(cdrom, output, sizeof(output));
        check(cdrom->xa_playing, "valid interleaved XA stays active");
        check(last_read <= cdrom->lba, "interleave never reads future sectors");
        const unsigned expected_sectors = ((frame + 1) * 882 + sector_samples - 1) / sector_samples;
        check(g_psx_audio_diag.xa_sectors == expected_sectors,
              "interleaved stream decodes every required sector without starvation");
    }
    free(cdrom);
}

int main(void) {
    test_transport_boundary();
    test_catch_up_bound();
    test_partial_tail();
    test_terminal_end();
    test_interleaved_clock(75, 4, 1);
    test_interleaved_clock(150, 8, 1);
    test_interleaved_clock(75, 8, 0);
    test_interleaved_clock(150, 16, 0);
    test_interleaved_clock(75, 8, 5);
    test_interleaved_clock(150, 16, 5);
    test_interleaved_clock(75, 16, 4);
    test_interleaved_clock(150, 32, 4);
    if (failures) return 1;
    puts("XA AUDIO PASS transport-bound, bounded catch-up, retry, partial tail, EOF, 8 clock/interleave formats");
    return 0;
}
