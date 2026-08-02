#ifndef PSX_PERF_H
#define PSX_PERF_H

/*
    Per-frame work counters for a host performance overlay.

    WHY THIS EXISTS
    ---------------
    The Android front-end draws its OSD in Jetpack Compose; there is no imgui/FSUI left in
    the core to draw one natively, and no way for the UI thread to ask a running machine
    what it just did. These counters are the answer: the emulation thread bumps them in
    place as work happens, frontend/main.cpp drains them once per frame, and the numbers
    reach Kotlin through psxe_host_stats() -> NativeApp -> com.armsx2.ui.GameOsd.

    WHAT IS COUNTED, AND WHAT IS NOT
    --------------------------------
    Only things the core genuinely does. Every field below is an exact event count taken at
    the site that performs the work — there is no sampling, no estimation and no modelling.
    Notably absent is a per-subsystem TIME split: this core interleaves the R3000A, the GPU
    rasteriser and the devices at instruction granularity (psx_update() runs ~565k times a
    frame), so timing them apart would cost far more than the work being measured. Host wall
    time is measured instead where it is cheap and meaningful — around whole frame phases,
    in frontend/main.cpp.

    The software rasteriser is the one place where a count IS a time: every rasteriser in
    psx/dev/gpu.c is a bounding-box loop, so the box area is exactly the number of inner-loop
    iterations the primitive will cost. gpu_raster_pixels is therefore a precise measure of
    rasteriser work, obtained for free from values the rasteriser already computed.

    COST WHEN DISABLED
    ------------------
    Every macro is wrapped in `if (g_psx_perf_enabled)`. Disabled, a counter site is one load
    of a hot global plus a perfectly-predicted branch. Nothing here is placed inside a
    per-instruction or per-pixel loop — the two paths that run millions of times a frame
    (psx_cpu_cycle and the rasteriser inner loops) are deliberately untouched, and their
    numbers are derived once per frame from state the core already maintains.

    THREADING
    ---------
    Written only from the emulation thread. psx_perf_take_frame() is likewise emulation-thread
    only (frontend/main.cpp calls it at the frame boundary); the host-visible snapshot it
    produces is what crosses threads, published as atomics by the frontend.
*/

#include <stdint.h>

enum {
    PSX_PERF_PRIM_TRIANGLE = 0,
    PSX_PERF_PRIM_RECT,
    PSX_PERF_PRIM_LINE,
    PSX_PERF_PRIM_KINDS
};

/* Indices match the DMA channel numbers in the hardware (DMA0..DMA6). */
enum {
    PSX_PERF_DMA_MDEC_IN = 0,
    PSX_PERF_DMA_MDEC_OUT,
    PSX_PERF_DMA_GPU,
    PSX_PERF_DMA_CDROM,
    PSX_PERF_DMA_SPU,
    PSX_PERF_DMA_PIO,
    PSX_PERF_DMA_OTC,
    PSX_PERF_DMA_CHANNELS
};

typedef struct {
    /* R3000A. Both are read once per frame from state the CPU already keeps, so the
       interpreter's inner loop carries no instrumentation at all. */
    uint64_t cpu_instructions;
    uint64_t cpu_cycles;

    /* Geometry coprocessor: one count per executed GTE operation, across all three
       dispatch paths (interpreter, cached interpreter, IRQ fast path). */
    uint64_t gte_ops;

    /* Rasteriser. gpu_raster_pixels is the exact inner-loop iteration count (see above),
       gpu_vram_words counts halfwords moved by the VRAM transfer/fill/copy commands. */
    uint32_t gpu_primitives[PSX_PERF_PRIM_KINDS];
    uint64_t gpu_raster_pixels;
    uint64_t gpu_vram_words;

    /* SPU. Voices are accumulated per generated sample; the host divides to get the mean
       number of voices that were actually playing over the frame. */
    uint64_t spu_samples;
    uint64_t spu_voice_samples;

    /* MDEC: macroblock decode calls, and the 8x8 blocks run through the IDCT inside them. */
    uint32_t mdec_macroblocks;
    uint32_t mdec_blocks;

    /* Sectors pulled off the disc image, whatever the container (cue/bin, CHD, raw). */
    uint32_t cdrom_sectors;

    uint64_t dma_words[PSX_PERF_DMA_CHANNELS];
} psx_perf_counters_t;

extern int g_psx_perf_enabled;
extern psx_perf_counters_t g_psx_perf;

/* Arming also zeroes the counters, so the first frame after the overlay is switched on is
   not a partial one. Safe to call redundantly. */
void psx_perf_set_enabled(int enabled);

/* Copy the live counters into [out] and reset them for the next frame. Emulation thread. */
void psx_perf_take_frame(psx_perf_counters_t* out);

#define PSX_PERF_ADD(field, n) \
    do { if (g_psx_perf_enabled) g_psx_perf.field += (uint64_t)(n); } while (0)

#define PSX_PERF_INC(field) PSX_PERF_ADD(field, 1)

/* One call per primitive, placed after the rasteriser has clamped its bounding box (and
   after any degenerate-primitive bail-out) so the area recorded is the work really done. */
#define PSX_PERF_RASTER(kind, w, h)                                                     \
    do {                                                                                \
        if (g_psx_perf_enabled) {                                                       \
            int _pw = (int)(w), _ph = (int)(h);                                         \
            ++g_psx_perf.gpu_primitives[(kind)];                                         \
            if (_pw > 0 && _ph > 0)                                                     \
                g_psx_perf.gpu_raster_pixels += (uint64_t)_pw * (uint64_t)_ph;          \
        }                                                                               \
    } while (0)

/* ---- audio_diag: one-shot audio capture -------------------------------------------------

   A player reporting "the audio is broken" cannot tell apart four things that need opposite
   fixes: the emulator not keeping up (host underruns), the XA decoder producing the wrong
   stream, the mixer being at the wrong level, and the SPU reverb feedback loop railing. All
   four sound bad. This block is what separates them in ONE run.

   Armed by a marker file named `audio_diag` next to armsx.log; frontend/main.cpp polls for
   it, deletes it, opens a bounded capture window and writes `audio_diag.txt`. Same mechanism
   as `gpu_prim_dump`, `perf_log` and gl_debug_marker().

   COST WHEN DISABLED
   ------------------
   Every write site is inside `if (g_psx_audio_diag_enabled)` — one load of a hot global and a
   perfectly-predicted branch. The hottest site is psx_spu_get_sample(), which runs 44100 times
   a second, i.e. ~0.07% of the rate of the paths perf.h already refuses to touch. NOTHING here
   is per-CPU-instruction or per-pixel, and the XA sub-header ring is written once per 2352-byte
   sector (~19 times a second), never per sample.

   PEAKS ARE PRE-CLAMP WHERE IT MATTERS
   ------------------------------------
   dry_peak / revin_peak are the raw accumulator values BEFORE psx_spu_get_sample()'s int16
   clamp, so "how far past full scale is this" is answerable rather than being flattened to
   32767. Inside spu_get_reverb_sample() the values are read back AFTER the existing SAT()
   macros on purpose: hoisting those expressions to see them pre-clamp would have meant editing
   the reverb arithmetic while investigating the reverb, and a value pinned at the rail is the
   same evidence. `*_railed` counts samples landing within 68 of full scale — chance alone puts
   a sample there about 1 time in 240, so a railed loop is unmistakable against the sample count.
*/

#define PSX_AUDIO_DIAG_XA_HDRS 32
#define PSX_AUDIO_DIAG_CD_CMDS 24

/* Why every sector decision is recorded, not just the accepted ones: "the music plays but the
   dialogue does not" is a statement about TWO XA streams in one track, and it can only be
   answered by seeing the fetcher's verdict on each sector next to the filter that was in force
   when it decided. An accept-only log cannot distinguish "the dialogue channel never arrived"
   from "it arrived and was rejected" from "the walk to find it was aborted on the way". */
enum {
    PSX_XA_VERDICT_ACCEPT_NOFILTER = 0, /* audio sector, MODE_XA_FILTER off                  */
    PSX_XA_VERDICT_ACCEPT_MATCH,        /* audio sector, file+channel both matched           */
    PSX_XA_VERDICT_REJECT_FILTER,       /* audio sector, file/channel mismatch -> keep walking */
    PSX_XA_VERDICT_SKIP_NONAUDIO,       /* submode bit2 clear -> keep walking                */
    PSX_XA_VERDICT_STOP_EOR,            /* submode bit0 set -> XA playback abandoned entirely */
    PSX_XA_VERDICT_KINDS
};

typedef struct {
    /* --- SPU mixer, per generated sample --------------------------------------------- */
    uint32_t spu_samples;          /* psx_spu_get_sample() calls inside the window        */
    uint32_t spu_silent;           /* returns of 0 because SPUCNT bit15 (enable) is clear */
    uint32_t spu_voices_peak;      /* most voices sounding in any single sample           */
    uint64_t spu_voices_sum;       /* / spu_samples = mean voices sounding                */
    int32_t  dry_peak_l, dry_peak_r;     /* |voice sum| BEFORE the int16 clamp            */
    uint32_t dry_clip;                   /* samples where that clamp engaged              */
    int32_t  revin_peak_l, revin_peak_r; /* |EON voice sum| entering the reverb, pre-clamp */
    uint32_t revin_clip;
    uint32_t revsum_clip;                /* dry+wet clamp at the SPU output                */
    int32_t  mainvol_l, mainvol_r;       /* raw 1F801D80/D82 as LAST seen by the mixer     */
    /* ...and the smallest seen anywhere in the window. The last-value-only fields above cannot
       tell a constant main volume from one that dipped and recovered inside the period, which
       is the one thing that could make an output peak smaller than the gain predicts. Set to
       0xFFFFFFFF when a window opens so the first sample always wins. */
    uint32_t mainvol_min_l, mainvol_min_r;

    /* --- reverb network (spu_get_reverb_sample) --------------------------------------- */
    uint32_t rev_calls;            /* 22050 Hz while reverb is on: 0 means it never ran   */
    uint32_t revfb_railed;         /* mLSAME/mRSAME writes at the rail: a saturating loop */
    uint32_t revout_railed;        /* reverb output at the rail                           */
    int32_t  revout_peak_l, revout_peak_r;

    /* --- SPU RAM transfer path (CD data -> SPU RAM -> a voice) -------------------------
       The route streamed speech and music actually take. spu_ram_writes == 0 while voices are
       keyed on and sounding means the sample data is never arriving and the voices are looping
       over whatever was already there. spu_fifo_drop > 0 means the 32-entry transfer FIFO was
       overrun — which before the bound in spu.c was a write straight through tfifo_index and
       into the voice decoder states. */
    uint32_t spu_ram_writes;
    uint32_t spu_fifo_drop;
    /* Sound RAM IRQs raised. Zero while SPUCNT bit6 is set means the game armed the interrupt
       it uses to time streaming refills and never received one.

       Rate matters as much as presence. A streaming game arms SPUIRQA on a point its own
       metronome voice crosses at a fixed rate, and that rate is a property of the voice, not
       of the video output — so spu_irq_raised landing at or just under the frame rate is the
       signature of interrupts being quantised to the frame rather than delivered when the
       voice actually got there. See the block comment above psx_spu_tick(). */
    uint32_t spu_irq_raised;
    /* Where the frame's samples were produced. ticked = spread across the frame by
       psx_spu_tick() on the CPU's timeline, which is what lets the guest service an SPU
       interrupt between two samples. inline = generated by the front-end in a lump at the
       frame boundary with the CPU stopped, which cannot deliver an interrupt at all. inline
       should be a small remainder; inline >> ticked means the SPU is effectively back on the
       old burst model and any streaming game will starve. */
    uint32_t spu_gen_ticked;
    uint32_t spu_gen_inline;
    uint32_t spu_ram_lo;           /* seeded to 0xFFFFFFFF when a window opens */
    uint32_t spu_ram_hi;

    /* Per-voice contribution, post-decode and pre-mix. Indexed by voice number. */
    int32_t  voice_peak_l[24];
    int32_t  voice_peak_r[24];
    /* Streaming behaviour: the last ADPCM block flags seen (bit0 loop-end, bit1 loop-repeat,
       bit2 loop-start), how many times the voice looped back to its repeat address, and how
       many times it was stopped by an end-without-repeat block. A stream that dies shows
       voice_stop going 0 -> 1 at the instant the audio cuts out. */
    uint8_t  voice_flags[24];
    uint32_t voice_loopend[24];
    uint32_t voice_stop[24];

    /* --- voice volume register formats seen at key-on ---------------------------------- */
    uint32_t kon_voices;
    uint32_t kon_vol_negative;     /* bit14 set, bit15 clear: a NEGATIVE 15-bit volume    */
    uint32_t kon_vol_sweep;        /* bit15 set: sweep mode                               */

    /* --- CD / XA ---------------------------------------------------------------------- */
    uint32_t xa_sectors;           /* sub-headers accepted and decoded                    */
    uint32_t xa_filter_rejects;    /* skipped because file/channel did not match Setfilter */
    uint32_t xa_skip_nonaudio;     /* walked past because the submode audio bit was clear */
    uint32_t xa_stop_eor;          /* fetch abandoned on submode bit0                     */
    uint32_t xa_stop_far;          /* fetch abandoned because the read ran off the disc   */
    uint32_t xa_starved;           /* cdrom_get_xa_samples() gave up mid-buffer           */
    uint32_t xa_walk_peak;         /* most sectors walked to satisfy one fetch            */
    int32_t  xa_peak_l, xa_peak_r; /* |XA| as written into the host buffer, post-volume   */
    uint32_t cdda_sectors;

    /* Ring of the most recent XA sector DECISIONS — accepted, rejected and stopped alike.
       Drained and reset by the host each snapshot, so the file shows how the stream evolved
       rather than one instant. */
    uint32_t xa_hdr_seen;          /* pushed since the last drain (may exceed the ring)   */
    uint32_t xa_hdr_head;
    struct {
        uint32_t lba;
        uint8_t  file;
        uint8_t  chan;
        uint8_t  submode;          /* 0x12: bit0 EOR, 2 audio, 3 data, 5 form2, 7 EOF     */
        uint8_t  coding;           /* 0x13: bit0 stereo, bit2 18.9kHz, bit4 8bit, bit6 emph */
        /* The filter in force at the moment of the verdict. Recorded per entry because
           Setfilter can be reissued between sectors, and a stale filter is one of the
           candidate explanations for a stream that goes quiet. */
        uint8_t  filter_file;
        uint8_t  filter_chan;
        uint8_t  filter_on;        /* MODE_XA_FILTER at that instant                      */
        uint8_t  verdict;          /* PSX_XA_VERDICT_*                                    */
    } xa_hdr[PSX_AUDIO_DIAG_XA_HDRS];

    /* --- CD-ROM command log ------------------------------------------------------------
       "XA never runs" has exactly two explanations and they need opposite fixes: the game
       never asked for it, or it asked and we dropped the request. Only the command stream
       tells them apart, so every command is logged with its parameters AND the drive state it
       found — a Setmode arriving while read_ongoing=1 is a different event from one arriving
       at idle, because cdrom_cmd_setmode() calls cdrom_pause(). Commands are rare (tens per
       second at most), so this is nowhere near a hot path. */
    uint32_t cd_cmd_seen;
    uint32_t cd_cmd_head;
    struct {
        uint8_t cmd;
        uint8_t nparams;
        uint8_t param[4];
        uint8_t mode_before;   /* cdrom->mode as it stood when the command arrived */
        uint8_t xa_playing;    /* drive state at that instant, before execution    */
        uint8_t read_ongoing;
        uint8_t state;
    } cd_cmd[PSX_AUDIO_DIAG_CD_CMDS];

    /* --- host mixer (frontend/main.cpp MixPsxAudio) ------------------------------------ */
    uint32_t mix_wrap;             /* CD+SPU sums that left int16 range                   */
    uint32_t mix_sat;              /* ...and were saturated instead of wrapping           */
    int32_t  mix_peak_l, mix_peak_r;
} psx_audio_diag_t;

extern int g_psx_audio_diag_enabled;
extern psx_audio_diag_t g_psx_audio_diag;

/* Arming zeroes the block, so a window never carries counts in from before it opened. */
void psx_audio_diag_set_enabled(int enabled);

/* max(dst, |v|), for the peak fields above. */
#define PSX_AUDIO_DIAG_PEAK(dst, v)                                                     \
    do {                                                                                \
        int32_t _av = (int32_t)(v);                                                     \
        if (_av < 0) _av = -_av;                                                        \
        if (_av > (dst)) (dst) = _av;                                                   \
    } while (0)

/* Within 68 of int16 full scale in either direction. */
#define PSX_AUDIO_DIAG_RAILED(v) (((v) >= 32700) || ((v) <= -32700))

#endif
