#ifndef PSX_PGXP_H
#define PSX_PGXP_H

/*
    PGXP — precise-vertex pipeline.

    The GTE computes screen coordinates internally at much higher precision than
    the 11-bit signed integers games read out of the SXY FIFO. This module
    captures the pre-truncation coordinates at RTPS/RTPT retirement, follows the
    truncated words as the game copies them into RAM (SWC2 directly, or
    MFC2 + SW), and re-attaches the precise values when the very same words come
    back through GPU DMA as GP0 polygon vertices. A hardware rasterizer backend
    can then place vertices at sub-pixel positions instead of snapped integers,
    which is what removes the PS1's polygon wobble.

    Everything here is DERIVED data:
      * it is never serialized into save states (psx/state.c calls
        psx_pgxp_reset() on load instead — precision degrades to plain integers
        for the one frame it takes the GTE to re-transform the scene);
      * a miss anywhere simply leaves the vertex integer-only, which is exactly
        the behaviour with the feature off.

    Validation rule (load-bearing, do not weaken): a cached precise coordinate
    is only ever attached when the cached low-precision 32-bit word equals the
    word actually being consumed. The game may overwrite a tracked address, or
    hand-modify a vertex after reading it from the GTE; a stale attach produces
    geometry far worse than no PGXP at all.

    The module is process-global, matching the single-machine reality of every
    frontend in this repo. All entry points are cheap no-ops while disabled;
    hot call sites additionally guard with psx_pgxp_active() so the disabled
    cost is one predictable branch. Default is OFF ([video] pgxp in
    settings.toml) and with it off nothing in the pipeline changes behaviour.
*/

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vertex_t;

/* Read-only outside psx/pgxp.c. Exposed so per-instruction call sites can gate
   on a single global load instead of a function call. */
extern int g_psx_pgxp_enabled;

#define psx_pgxp_active() (g_psx_pgxp_enabled)

/* Lifetime / control. Enabling allocates the address cache (~10 MiB) on first
   use and keeps it across later toggles, so flipping the setting mid-game can
   never race the emulation thread against free(). */
void psx_pgxp_set_enabled(int enabled);
int  psx_pgxp_enabled(void);

/* Drop every derived value: GTE FIFO shadow, CPU register shadow, address
   cache, and the DMA->GP0 address queue. Wired into psx_soft_reset() and
   the save-state load path. */
void psx_pgxp_reset(void);

/* ---- capture (psx/cpu.c, GTE) ----
   Called as an RTPS/RTPT result lands in SXY2 (GTE_RTP/GTE_RTP_DQ retirement).
   `sxy` is the packed (sy << 16) | (sx & 0xffff) register value the game will
   read; fx/fy are the pre-truncation screen coordinates in pixels; fw is the
   depth used for the projection divide (SZ3), for later perspective-correct
   interpolation. Shifts the module's 3-deep FIFO shadow exactly like the
   hardware FIFO shifts. */
void psx_pgxp_gte_vertex(uint32_t sxy, float fx, float fy, float fw);

/* Called on every GTE data-register write (MTC2 / LWC2 / SXYP pushes) so
   direct writes into the SXY FIFO invalidate — and reg-15 pushes shift — the
   shadow. `reg` is the raw 0..63 index gte_write_register() receives. */
void psx_pgxp_gte_reg_write(uint32_t reg, uint32_t value);

/* ---- tracking (psx/cpu.c, memory traffic) ---- */

/* SWC2: the GTE register `reg` (0..31 data space) was stored to `addr` with
   contents `value`. SXY sources create/overwrite a cache entry; anything else
   invalidates a stale entry at that address. */
void psx_pgxp_cpu_swc2(uint32_t addr, uint32_t value, uint32_t reg);

/* MFC2: GTE data register `reg` was read into CPU register `rt` (value is what
   will land after the load delay). Keeps a per-CPU-register shadow so a later
   plain SW can carry the precision along. */
void psx_pgxp_cpu_mfc2(uint32_t rt, uint32_t value, uint32_t reg);

/* SW: CPU register `rt` (contents `value`) was stored to `addr`. Attaches the
   register shadow when it still matches, otherwise invalidates the address. */
void psx_pgxp_cpu_sw(uint32_t addr, uint32_t value, uint32_t rt);

/* ---- submission (psx/dev/dma.c -> psx/dev/gpu.c) ----
   dma.c notes the RAM source address immediately before writing each word to
   GP0 (0x1f801810). The bus write lands in psx_gpu_write32() synchronously, so
   a single pending latch is enough: the GPU consumes it into a per-buf-slot
   address table (psx_pgxp_gp0_slot) or discards it (image data words). A GP0
   word arriving with no note pending — every CPU MMIO write — records "no
   address", so a stale DMA address can never attach to the wrong word. */
void psx_pgxp_note_gp0_word(uint32_t addr);
void psx_pgxp_gp0_slot(int buf_index);
void psx_pgxp_gp0_discard(void);

/* ---- lookup (psx/dev/gpu.c, vertex parse) ----
   `word` is gpu->buf[buf_index] (the packed y<<16|x vertex word), `buf_index`
   selects the source address noted for that slot. On a validated hit fills
   v->px/py/pw and sets v->precise_valid; otherwise clears precise_valid (and
   gives px/py/pw inert values), so calling it unconditionally also serves as
   the field initializer for downstream consumers. */
void psx_pgxp_poly_vertex(struct vertex_t* v, uint32_t word, int buf_index);

#ifdef __cplusplus
}
#endif

#endif /* PSX_PGXP_H */
