#include "perf.h"

#include <string.h>

/* Deliberately plain globals rather than fields on psx_t: the counters must be reachable
   from leaf functions all over the core (a rasteriser has no psx_t, psx_disc_read has no
   device handle), and keeping them out of the device structs keeps psx/state.c — and every
   save state already on disk — untouched. */
int g_psx_perf_enabled = 0;
psx_perf_counters_t g_psx_perf;

void psx_perf_set_enabled(int enabled) {
    memset(&g_psx_perf, 0, sizeof(g_psx_perf));

    g_psx_perf_enabled = enabled ? 1 : 0;
}

void psx_perf_take_frame(psx_perf_counters_t* out) {
    if (out)
        *out = g_psx_perf;

    memset(&g_psx_perf, 0, sizeof(g_psx_perf));
}

/* Same reasoning as g_psx_perf above: the audio counters are written from psx_spu_get_sample(),
   from the reverb network and from the CD-ROM XA fetcher, none of which hold a psx_t. */
int g_psx_audio_diag_enabled = 0;
psx_audio_diag_t g_psx_audio_diag;

void psx_audio_diag_set_enabled(int enabled) {
    memset(&g_psx_audio_diag, 0, sizeof(g_psx_audio_diag));

    /* Minima, so they cannot start at the memset's zero. */
    g_psx_audio_diag.mainvol_min_l = 0xffffffffu;
    g_psx_audio_diag.mainvol_min_r = 0xffffffffu;
    g_psx_audio_diag.spu_ram_lo = 0xffffffffu;

    g_psx_audio_diag_enabled = enabled ? 1 : 0;
}

psx_audio_pcm_probe_t g_psx_audio_pcm_probe = NULL;
void* g_psx_audio_pcm_probe_context = NULL;

psx_xa_probe_t g_psx_xa_probe = NULL;

psx_disc_timing_t g_psx_disc_timing;

int g_psx_work_diag_enabled;
psx_work_diag_t g_psx_work_diag;
uint64_t (*g_psx_work_clock)(void);
void psx_work_diag_begin(uint64_t (*clock_fn)(void)) {
    /* Preserve the prime-period sampling phase across frames, avoiding a
       repeated sample of the same instruction at every vblank. */
    uint32_t phase = g_psx_work_diag.cpu_sample_phase;
    memset(&g_psx_work_diag, 0, sizeof(g_psx_work_diag));
    g_psx_work_diag.cpu_sample_phase = phase;
    g_psx_work_clock = clock_fn;
    g_psx_work_diag_enabled = clock_fn != NULL;
}
uint64_t psx_work_diag_start(void) {
    return g_psx_work_diag_enabled ? g_psx_work_clock() : 0;
}
void psx_work_diag_end(unsigned category, uint64_t start) {
    if (!g_psx_work_diag_enabled || category >= PSX_WORK_COUNT) return;
    uint64_t elapsed = g_psx_work_clock() - start;
    g_psx_work_diag.ticks[category] += elapsed;
    ++g_psx_work_diag.calls[category];
    if (elapsed > g_psx_work_diag.max_ticks[category])
        g_psx_work_diag.max_ticks[category] = elapsed;
}
