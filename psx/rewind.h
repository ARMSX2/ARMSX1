/*
    ARMSX PS1 core — rewind + runahead.

    ============================================================================
    ONE SNAPSHOT POOL, TWO FEATURES
    ============================================================================

    Both features are "take a save state, put it back later", so they share ONE
    internal snapshot type and one capture/restore path (psx_rewind_snapshot_t in
    rewind.c). They differ only in how many snapshots are kept and when they are
    put back:

      REWIND    keeps a RING of snapshots taken every Nth frame, and steps
                BACKWARDS through it on demand. The ring is the memory cost.

      RUNAHEAD  keeps exactly ONE snapshot, taken after the frame that really
                happened, and puts it back at the start of the next frame. The
                cost is CPU, not memory: one save + one load + N re-simulated
                frames, every single frame.

    Snapshots are the ordinary save-state stream (psx_save_state_to_memory)
    minus the preview image — the PNG encode is far too expensive to run twice a
    second, and the section is optional by design, so a snapshot loads through
    the ordinary psx_load_state_from_memory() with no special case anywhere.

    Because it IS an ordinary save state, everything the state format guarantees
    holds here too: the PGXP shadow caches are NOT serialised and are reset by
    the load (psx/pgxp.h), and the cached-interpreter block cache is invalidated.

    ============================================================================
    MEMORY IS THE DESIGN CONSTRAINT
    ============================================================================

    A PS1 snapshot is ~3.5 MiB (2 MiB RAM + 1 MiB VRAM + 512 KiB SPU RAM + the
    small device blocks). That is small next to a PS2 state and ENORMOUS next to
    what a handheld can spare once it is multiplied by a ring:

        10 s at 2/s  =  20 snapshots  ~=   70 MiB   (the default)
        30 s at 4/s  = 120 snapshots  ~=  420 MiB
        60 s at 4/s  = 240 snapshots  ~=  840 MiB   (unusable)

    So: nothing is allocated until rewind is actually switched on, the ring is
    capped by PSX_REWIND_MAX_BYTES regardless of what the settings ask for (the
    depth shrinks instead of the allocator failing), and a front-end is expected
    to show the computed figure next to the control rather than hiding it.

    ============================================================================
    THREADING
    ============================================================================

    EVERY function here must be called on the emulation thread. They read and
    write the machine. A host whose UI thread wants to change the configuration
    parks the request and lets the emulation thread apply it — see
    psxe_host_set_rewind() in frontend/main.cpp.
*/

#ifndef PSX_REWIND_H
#define PSX_REWIND_H

#include <stddef.h>

struct psx_t;

/* Hard ceiling on everything the ring may hold, whatever the settings say. The
   depth is reduced to fit; it is never the allocator's job to say no. */
#define PSX_REWIND_MAX_BYTES ((size_t)384u * 1024u * 1024u)

/* Bounds the front-end and the config parser share, so "10 s at 2/s" means the
   same number of snapshots on both sides. */
#define PSX_REWIND_MIN_SECONDS 1
#define PSX_REWIND_MAX_SECONDS 60
#define PSX_REWIND_MIN_FREQUENCY 1
#define PSX_REWIND_MAX_FREQUENCY 8
#define PSX_REWIND_DEFAULT_SECONDS 10
#define PSX_REWIND_DEFAULT_FREQUENCY 2

#define PSX_RUNAHEAD_MAX_FRAMES 5

/* Rough size of one snapshot before any has actually been taken, so a front-end
   can label its control before a game is even running. Replaced by the real
   figure (psx_rewind_snapshot_bytes) as soon as one snapshot exists. */
#define PSX_REWIND_ESTIMATED_SNAPSHOT_BYTES ((size_t)3670016u) /* ~3.5 MiB */

/* ---- Configuration (emulation thread) ---------------------------------- */

/* enabled == 0 frees the ring immediately. seconds/frequency are clamped into
   the ranges above. Re-configuring with the same values is a no-op; changing
   the depth drops what is held (the timeline it described no longer matches). */
void psx_rewind_configure(int enabled, int seconds, int frequency);
int psx_rewind_enabled(void);

/* 0 disables. Clamped to 0..PSX_RUNAHEAD_MAX_FRAMES. */
void psx_runahead_configure(int frames);
int psx_runahead_frames(void);

/* ---- Rewind ------------------------------------------------------------ */

/* Call once per emulated frame, AFTER the frame has been produced.
   frame_rate is the machine's nominal rate (used only to turn the configured
   snapshots-per-second into a frame interval); <= 0 falls back to 60. */
void psx_rewind_notify_frame(struct psx_t* psx, double frame_rate);

/* Put the machine back to the newest snapshot and drop it, so repeated calls
   walk backwards. Returns PSX_STATE_OK, PSX_STATE_ERR_MISSING when the ring is
   empty (i.e. "as far back as it goes" — not an error worth reporting), or a
   negative psx/state.h code when the snapshot would not apply. */
int psx_rewind_step_back(struct psx_t* psx);

/* Drop every snapshot AND the runahead slot. The timeline they describe is gone
   after a reset, a disc swap or a state load, and putting one back afterwards
   would resume a machine that no longer exists. */
void psx_rewind_reset(void);

/* Release every allocation. Called from psx_destroy(). */
void psx_rewind_shutdown(void);

/* ---- Runahead ---------------------------------------------------------- */

/* Capture / restore the single look-ahead snapshot. save() overwrites whatever
   was there; restore() applies it and KEEPS it (the caller decides when the
   slot is stale — see psx_runahead_discard). */
int psx_runahead_save(struct psx_t* psx);
int psx_runahead_restore(struct psx_t* psx);
int psx_runahead_has_snapshot(void);
void psx_runahead_discard(void);

/* ---- Introspection, for a front-end's UI ------------------------------- */

/* Bytes currently held by the ring (allocated, not merely used). */
size_t psx_rewind_bytes_used(void);
/* Size of one snapshot: measured if any has been taken, otherwise the estimate. */
size_t psx_rewind_snapshot_bytes(void);
/* Snapshots held right now, and how many the current configuration can hold. */
int psx_rewind_snapshot_count(void);
int psx_rewind_capacity(void);

#endif
