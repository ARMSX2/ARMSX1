#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../psx/dev/cdrom/cdrom.h"
#include "../psx/dev/cdrom/xa_search_budget.h"

#include "../psx/perf.h"

#undef puts

int cdrom_get_xa_samples(psx_cdrom_t*, void*, size_t);
static unsigned reads;
static unsigned target = 150;
static int end_of_disc;
static unsigned probe_calls;
static void capture_sector(void* context, const psx_xa_probe_sector_t* p) {
    assert(context == &probe_calls);
    assert(p->lba == target && p->output_offset == 0);
    assert(p->rate == 37800 && p->frames == XA_STEREO_SAMPLES);
    assert(p->sector[0x10] == 1 && p->sector[0x11] == 2);
    for (unsigned i = 0; i < 4; ++i) assert(p->history[i] == 0);
    for (unsigned i = 0; i < p->frames; ++i) assert(p->left[i] == 0 && p->right[i] == 0);
    ++probe_calls;
}


int psx_disc_read(psx_disc_t* disc, uint32_t lba, void* buf) {
    (void)disc;
    ++reads;
    memset(buf, 0, CD_SECTOR_SIZE);
    if (end_of_disc) return TS_FAR;
    uint8_t* sector = buf;
    sector[0x10] = 1;
    sector[0x11] = lba == target ? 2 : 3;
    sector[0x12] = 4;
    sector[0x13] = 1;
    return TS_DATA;
}

int main(void) {
    /* A large gap spent mixing between reads must not consume the I/O budget. */
    xa_search_budget_t budget = {0};
    xa_search_budget_charge(&budget, 100, 200);
    xa_search_budget_charge(&budget, 100000000, 100000100);
    assert(budget.read_ns == 200 && xa_search_budget_available(&budget));
    /* Slow reads yield after a minimum interleaved run; zero-clock hosts
       still obey the hard sector cap. */
    budget.reads = 15; budget.read_ns = 3000000;
    assert(xa_search_budget_available(&budget));
    xa_search_budget_charge(&budget, 1, 2);
    assert(!xa_search_budget_available(&budget));
    budget.reads = 63; budget.read_ns = 0;
    assert(xa_search_budget_available(&budget));
    xa_search_budget_charge(&budget, 0, 0);
    assert(!xa_search_budget_available(&budget));
    static psx_cdrom_t cd;
    int16_t out[64];
    g_psx_xa_probe = capture_sector;
    g_psx_audio_pcm_probe_context = &probe_calls;
    cd.lba = 100000; /* This test isolates search budgeting, not transport timing. */
    cd.xa_playing = 1;
    cd.mode = MODE_XA_ADPCM | MODE_XA_FILTER;
    cd.xa_file = 1; cd.xa_channel = 2;
    memset(out, 0x55, sizeof(out));
    assert(cdrom_get_xa_samples(&cd, out, sizeof(out)) == 1);
    assert(reads > 0 && reads <= 64);
    assert(cd.xa_playing && cd.xa_lba == reads);
    for (unsigned i = 0; i < 64; ++i) assert(out[i] == 0);
    /* Resume without rescanning, then accept the requested channel. */
    unsigned calls = 0;
    while (cd.xa_lba <= target && calls++ < 200) {
        unsigned before = reads;
        assert(cdrom_get_xa_samples(&cd, out, sizeof(out)) == 1);
        assert(reads - before <= 64);
        assert(cd.xa_playing);
    }
    assert(cd.xa_lba == target + 1 && reads == target + 1);
    assert(probe_calls == 1);
    g_psx_xa_probe = NULL;
    g_psx_audio_pcm_probe_context = NULL;
    /* Yield after a buffered sample: preserve its output and silence the tail. */
    cd.xa_remaining_samples = 1; cd.xa_sample_index = 0;
    cd.xa_buf[0x13] = 1;
    cd.xa_left_resample_buf[0] = 100;
    cd.xa_right_resample_buf[0] = 200;
    cd.vol[0] = cd.vol[2] = 0x80;
    target = 10000;
    memset(out, 0x55, sizeof(out));
    assert(cdrom_get_xa_samples(&cd, out, sizeof(out)) == 1);
    assert(out[0] == 100 && out[1] == 200);
    for (unsigned i = 2; i < 64; ++i) assert(out[i] == 0);
    assert(cd.xa_playing);
    /* Muting consumes samples; unmuting must not replay the start of a sector. */
    cd.xa_remaining_samples = 4; cd.xa_sample_index = 0;
    cd.xa_buf[0x13] = 1; cd.mute = 1;
    cd.xa_left_resample_buf[2] = 300; cd.xa_right_resample_buf[2] = 400;
    assert(cdrom_get_xa_samples(&cd, out, 8) == 1);
    assert(cd.xa_sample_index == 2 && cd.xa_remaining_samples == 2);
    assert(out[0] == 0 && out[1] == 0);
    cd.mute = 0;
    assert(cdrom_get_xa_samples(&cd, out, 4) == 1);
    assert(out[0] == 300 && out[1] == 400);
    cd.xa_remaining_samples = 0;
    /* A real end remains distinct from a deferred search. */
    end_of_disc = 1;
    assert(cdrom_get_xa_samples(&cd, out, sizeof(out)) == 1);
    for (unsigned i = 0; i < 64; ++i) assert(out[i] == 0);
    assert(!cd.xa_playing);
    puts("XA search budget tests passed");
}
