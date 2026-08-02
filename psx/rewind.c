/*
    ARMSX PS1 core — rewind + runahead. Design notes live in rewind.h.

    One snapshot type, one capture path, one restore path. The ring and the
    single runahead slot are two USERS of that pool, not two implementations.
*/

#include "rewind.h"
#include "psx.h"
#include "state.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* The pool                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    void* data;       /* owned; kept across captures so the allocator is idle */
    size_t capacity;  /* bytes allocated */
    size_t size;      /* bytes actually written by the last capture, 0 = empty */
} psx_rewind_snapshot_t;

/* Capture into `slot`, reusing whatever allocation it already holds. */
static int snapshot_capture(psx_rewind_snapshot_t* slot, psx_t* psx) {
    size_t size = 0;
    int result;

    if (!slot || !psx)
        return PSX_STATE_ERR_ARG;

    result = psx_save_state_to_memory_ex(psx, &slot->data, &slot->capacity, &size,
                                         PSX_STATE_SAVE_NO_THUMBNAIL);

    /* A failed capture leaves the slot EMPTY rather than stale: restoring a
       snapshot that describes a different moment is worse than not restoring. */
    slot->size = (result == PSX_STATE_OK) ? size : 0;

    return result;
}

static int snapshot_restore(const psx_rewind_snapshot_t* slot, psx_t* psx) {
    if (!slot || !slot->data || !slot->size)
        return PSX_STATE_ERR_MISSING;

    return psx_load_state_from_memory(psx, slot->data, slot->size);
}

static void snapshot_free(psx_rewind_snapshot_t* slot) {
    if (!slot)
        return;

    free(slot->data);
    slot->data = NULL;
    slot->capacity = 0;
    slot->size = 0;
}

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

/* Ring. head is where the NEXT snapshot goes; the newest is head-1. */
static psx_rewind_snapshot_t* g_ring = NULL;
static int g_ring_capacity = 0;
static int g_ring_count = 0;
static int g_ring_head = 0;

static int g_enabled = 0;
static int g_seconds = PSX_REWIND_DEFAULT_SECONDS;
static int g_frequency = PSX_REWIND_DEFAULT_FREQUENCY;

/* Frames since the last snapshot, and the interval derived from the machine's
   nominal rate. Recomputed whenever the rate changes so a PAL game does not
   silently snapshot 20% more often than an NTSC one. */
static int g_frame_counter = 0;
static int g_frame_interval = 30;
static double g_last_frame_rate = 0.0;

/* Measured size of one snapshot. 0 until the first capture. */
static size_t g_measured_size = 0;

/* Logged once each, so a device that cannot afford the ring says so without
   turning the log into a per-frame firehose. */
static int g_logged_alloc_failure = 0;
static int g_logged_budget_clamp = 0;

static int g_runahead_frames = 0;
static psx_rewind_snapshot_t g_runahead_slot = {NULL, 0, 0};

static int clamp_int(int value, int low, int high) {
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static void ring_free(void) {
    int i;

    if (g_ring) {
        for (i = 0; i < g_ring_capacity; i++)
            snapshot_free(&g_ring[i]);

        free(g_ring);
    }

    g_ring = NULL;
    g_ring_capacity = 0;
    g_ring_count = 0;
    g_ring_head = 0;
}

/* Snapshots the current configuration asks for. */
static int ring_target_capacity(void) {
    int capacity = g_seconds * g_frequency;

    if (capacity < 1)
        capacity = 1;

    return capacity;
}

/* Bytes the ring holds RIGHT NOW (allocated, which is what the OS sees). */
size_t psx_rewind_bytes_used(void) {
    size_t total = 0;
    int i;

    for (i = 0; i < g_ring_capacity; i++)
        total += g_ring[i].capacity;

    return total;
}

size_t psx_rewind_snapshot_bytes(void) {
    return g_measured_size ? g_measured_size : PSX_REWIND_ESTIMATED_SNAPSHOT_BYTES;
}

int psx_rewind_snapshot_count(void) {
    return g_ring_count;
}

int psx_rewind_capacity(void) {
    return g_ring_capacity;
}

/* True when adding one more allocated snapshot would blow the budget. The ring
   then evicts its oldest entry instead, so the depth quietly shrinks rather than
   the allocation failing — see the memory note in rewind.h. */
static int ring_over_budget(void) {
    size_t used = psx_rewind_bytes_used();
    size_t next = psx_rewind_snapshot_bytes();

    if (used + next <= PSX_REWIND_MAX_BYTES)
        return 0;

    if (!g_logged_budget_clamp) {
        g_logged_budget_clamp = 1;
        log_info("rewind: holding %u MiB, at the %u MiB budget — depth capped at %d snapshots",
                 (unsigned)(used / (1024u * 1024u)),
                 (unsigned)(PSX_REWIND_MAX_BYTES / (1024u * 1024u)),
                 g_ring_count);
    }

    return 1;
}

static int ring_alloc(int capacity) {
    ring_free();

    g_ring = (psx_rewind_snapshot_t*)calloc((size_t)capacity, sizeof(psx_rewind_snapshot_t));

    if (!g_ring) {
        log_error("rewind: could not allocate a %d-snapshot ring; rewind stays off", capacity);
        g_enabled = 0;
        return 0;
    }

    g_ring_capacity = capacity;
    g_ring_count = 0;
    g_ring_head = 0;
    g_logged_alloc_failure = 0;
    g_logged_budget_clamp = 0;

    return 1;
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                              */
/* ------------------------------------------------------------------------- */

void psx_rewind_configure(int enabled, int seconds, int frequency) {
    const int want_enabled = enabled ? 1 : 0;
    const int want_seconds = clamp_int(seconds, PSX_REWIND_MIN_SECONDS, PSX_REWIND_MAX_SECONDS);
    const int want_frequency =
        clamp_int(frequency, PSX_REWIND_MIN_FREQUENCY, PSX_REWIND_MAX_FREQUENCY);

    if (want_enabled == g_enabled && want_seconds == g_seconds && want_frequency == g_frequency)
        return;

    g_seconds = want_seconds;
    g_frequency = want_frequency;
    g_enabled = want_enabled;
    g_frame_counter = 0;
    /* Force the interval to be recomputed on the next frame. */
    g_last_frame_rate = 0.0;

    if (!g_enabled) {
        /* Nothing is kept while it is off: the whole point is that a user who
           has not asked for rewind pays nothing for it, memory included. */
        ring_free();
        log_info("rewind: off");
        return;
    }

    if (!ring_alloc(ring_target_capacity()))
        return;

    {
        const size_t snapshot = psx_rewind_snapshot_bytes();
        const size_t requested = (size_t)g_ring_capacity * snapshot;
        /* What the budget will actually let the ring reach — the depth stops growing there,
           so reporting only the requested figure would overstate how far back it goes. */
        const int affordable = (int)(PSX_REWIND_MAX_BYTES / (snapshot ? snapshot : 1));
        const int effective = g_ring_capacity < affordable ? g_ring_capacity : affordable;

        log_info("rewind: on, %d s at %d/s (%d snapshots, ~%u MiB)",
                 g_seconds, g_frequency, effective,
                 (unsigned)(((size_t)effective * snapshot) / (1024u * 1024u)));

        if (effective < g_ring_capacity)
            log_info("rewind: %d s at %d/s would need ~%u MiB, over the %u MiB budget — "
                     "capped at ~%d s of history",
                     g_seconds, g_frequency, (unsigned)(requested / (1024u * 1024u)),
                     (unsigned)(PSX_REWIND_MAX_BYTES / (1024u * 1024u)),
                     effective / (g_frequency ? g_frequency : 1));
    }
}

int psx_rewind_enabled(void) {
    return g_enabled;
}

void psx_runahead_configure(int frames) {
    const int want = clamp_int(frames, 0, PSX_RUNAHEAD_MAX_FRAMES);

    if (want == g_runahead_frames)
        return;

    g_runahead_frames = want;

    if (!g_runahead_frames) {
        snapshot_free(&g_runahead_slot);
        log_info("runahead: off");
        return;
    }

    log_info("runahead: %d frame%s (costs one state save + load + %d re-simulated frame%s "
             "every frame)",
             g_runahead_frames, g_runahead_frames == 1 ? "" : "s",
             g_runahead_frames, g_runahead_frames == 1 ? "" : "s");
}

int psx_runahead_frames(void) {
    return g_runahead_frames;
}

/* ------------------------------------------------------------------------- */
/* Rewind                                                                     */
/* ------------------------------------------------------------------------- */

static void rewind_update_interval(double frame_rate) {
    double rate = frame_rate;
    int interval;

    if (rate <= 1.0)
        rate = 60.0;

    if (rate == g_last_frame_rate)
        return;

    g_last_frame_rate = rate;

    interval = (int)((rate / (double)g_frequency) + 0.5);
    g_frame_interval = interval < 1 ? 1 : interval;
}

void psx_rewind_notify_frame(psx_t* psx, double frame_rate) {
    psx_rewind_snapshot_t* slot;
    int result;

    if (!g_enabled || !g_ring || !psx)
        return;

    rewind_update_interval(frame_rate);

    if (++g_frame_counter < g_frame_interval)
        return;

    g_frame_counter = 0;

    slot = &g_ring[g_ring_head];

    /* With the ring full, head IS the oldest entry, so overwriting it evicts —
       and it already owns an allocation, so the footprint does not move. A slot
       that has never been used would GROW the footprint; if that crosses the
       budget, hold what we have and stop deepening instead. */
    if (!slot->data && ring_over_budget())
        return;

    result = snapshot_capture(slot, psx);

    if (result != PSX_STATE_OK) {
        if (!g_logged_alloc_failure) {
            g_logged_alloc_failure = 1;
            log_error("rewind: snapshot failed (%s); rewind will hold nothing new",
                      psx_state_strerror(result));
        }
        return;
    }

    if (!g_measured_size)
        g_measured_size = slot->size;

    g_ring_head = (g_ring_head + 1) % g_ring_capacity;

    if (g_ring_count < g_ring_capacity)
        g_ring_count++;
}

int psx_rewind_step_back(psx_t* psx) {
    int index;
    int result;

    if (!psx)
        return PSX_STATE_ERR_ARG;

    if (!g_enabled || !g_ring || !g_ring_count)
        return PSX_STATE_ERR_MISSING;

    index = (g_ring_head - 1 + g_ring_capacity) % g_ring_capacity;

    result = snapshot_restore(&g_ring[index], psx);

    /* Consumed either way. A snapshot that would not apply is not going to
       start applying on the next press, and leaving it in place would wedge the
       rewind on one bad entry. */
    g_ring[index].size = 0;
    g_ring_head = index;
    g_ring_count--;

    /* The look-ahead slot described a future of the timeline we just left. */
    snapshot_free(&g_runahead_slot);

    /* Snapshots resume from the moment we landed on, not from wherever the
       counter happened to be. */
    g_frame_counter = 0;

    if (result != PSX_STATE_OK)
        log_error("rewind: snapshot would not apply (%s)", psx_state_strerror(result));

    return result;
}

void psx_rewind_reset(void) {
    int i;

    for (i = 0; i < g_ring_capacity; i++)
        g_ring[i].size = 0;

    g_ring_count = 0;
    g_ring_head = 0;
    g_frame_counter = 0;

    snapshot_free(&g_runahead_slot);
}

void psx_rewind_shutdown(void) {
    ring_free();
    snapshot_free(&g_runahead_slot);
    g_measured_size = 0;
    g_frame_counter = 0;
    g_last_frame_rate = 0.0;
}

/* ------------------------------------------------------------------------- */
/* Runahead                                                                   */
/* ------------------------------------------------------------------------- */

int psx_runahead_save(psx_t* psx) {
    const int result = snapshot_capture(&g_runahead_slot, psx);

    if (result == PSX_STATE_OK && !g_measured_size)
        g_measured_size = g_runahead_slot.size;

    return result;
}

int psx_runahead_restore(psx_t* psx) {
    return snapshot_restore(&g_runahead_slot, psx);
}

int psx_runahead_has_snapshot(void) {
    return g_runahead_slot.size != 0;
}

void psx_runahead_discard(void) {
    g_runahead_slot.size = 0;
}
