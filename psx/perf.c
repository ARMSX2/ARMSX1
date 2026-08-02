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
