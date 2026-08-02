#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

/*
    ARMSX — PlayStation GPU rasterizer backend ABI.

    This is the seam between psx/dev/gpu.c (the emulated GPU: command parsing, VRAM,
    CRTC timing) and whatever actually turns primitives into pixels. See
    frontend/HW_RENDERER_DESIGN.md — the hook sites below are §5.2's table.

    Layering contract
    -----------------
      * Pure C99 + <stdint.h>. This header must NEVER pull in a GL, GLES, EGL or Vulkan
        header: the emulator core includes it and has to stay API-agnostic.
      * gpu.h stores only an opaque `struct psx_gpu_backend*`, so nothing in the core
        needs this header except gpu.c itself and the backend implementation.
      * A NULL backend pointer means "software rasterizer", which stays bit-identical to
        the behaviour before any of this existed. Every hook in gpu.c is NULL-guarded.
      * Every function pointer is individually optional. A backend that only implements
        draw_poly leaves the rest NULL and the core keeps doing those itself.
      * All coordinates crossing this boundary are NATIVE PlayStation VRAM coordinates
        (0..1023 x 0..511). The backend owns the internal-resolution scale and applies it
        internally; the core never learns that upscaling exists. HW_RENDERER_DESIGN.md §3.1.
*/

#include <stdint.h>

struct psx_gpu_t;

/* Tagged in gpu.h so this header can name them without including it. */
struct vertex_t;
struct poly_data_t;
struct rect_data_t;

/*
    Set when the backend renders into its own target and needs gpu->vram to stay
    authoritative anyway — the core then ALSO runs the software rasterizer, so host VRAM
    keeps working for texture fetches, GPUREAD drains (GP0(C0)) and the whole-VRAM debug
    view with no readback and no dirty-region tracking at all.

    This is HW_RENDERER_DESIGN.md §4.6 option 3 ("render everything twice"), chosen
    deliberately for the first stage: it costs CPU but it makes Stage 5's coherency work
    (§4, the highest-variance part of the port) unnecessary to ship something correct.
    A GPU backend that owns VRAM itself leaves this clear and implements §4 instead.

    Ordering note: when this is set the core calls the BACKEND FIRST and the software
    rasterizer second, so both sample exactly the same pre-write VRAM state for textures.
*/
#define PSX_GPU_BACKEND_SOFTWARE_SHADOW 0x01

typedef struct psx_gpu_backend {
    void*    impl;
    uint32_t flags;      /* PSX_GPU_BACKEND_* */

    /* ---- lifetime ---- */
    void (*destroy)(struct psx_gpu_backend* be);

    /* ---- drawing ----
       draw_poly gets the whole poly_data_t rather than pre-split triangles: the backend
       needs `attrib` for the raw/transp/shaded/quad flags and splits the quad into
       (v0,v1,v2) + (v1,v2,v3) itself, exactly as gpu_poly() does (gpu.c:1166-1171). */
    void (*draw_poly)(struct psx_gpu_backend* be, struct psx_gpu_t* gpu,
                      const struct poly_data_t* poly);
    void (*draw_rect)(struct psx_gpu_backend* be, struct psx_gpu_t* gpu,
                      const struct rect_data_t* rect);
    /* Flat only. gpu_render_flat_line() discards v1's colour (gpu.c:1252) and applies
       neither dithering nor blending; polylines are parsed but never drawn (gpu.c:1193). */
    void (*draw_line)(struct psx_gpu_backend* be, struct psx_gpu_t* gpu,
                      const struct vertex_t* v0, const struct vertex_t* v1,
                      uint16_t color_bgr555);

    /* ---- VRAM transfers, all in NATIVE coordinates ---- */

    /* GP0(02), gpu.c:1839-1851. Ignores the drawing area and the mask bit. */
    void (*fill_vram)(struct psx_gpu_backend* be,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint16_t color_bgr555);

    /* GP0(80), gpu.c:1877-1885. */
    void (*copy_vram)(struct psx_gpu_backend* be,
                      uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                      uint32_t w, uint32_t h);

    /* GP0(A0), gpu.c:1260-1325. Called ONCE for the whole rectangle when the transfer
       completes (gpu.c:1318), never per halfword. `src` is native 16-bit VRAM and
       `src_stride_px` is in halfwords. */
    void (*upload_vram)(struct psx_gpu_backend* be,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        const uint16_t* src, uint32_t src_stride_px);

    /* GP0(C0), gpu.c:1776-1802, called at SETUP time before psx_gpu_read32() starts
       draining (gpu.c:98-118). A backend with PSX_GPU_BACKEND_SOFTWARE_SHADOW set has
       nothing to do here — host VRAM is already correct — which is why the first stage
       has no readback stall at all. */
    void (*download_vram)(struct psx_gpu_backend* be,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint16_t* dst, uint32_t dst_stride_px);

    /* ---- frame ---- */

    /* GPU_EVENT_VBLANK (gpu.c:2152) — the only frame boundary this core exposes. */
    void (*end_frame)(struct psx_gpu_backend* be, struct psx_gpu_t* gpu);

    /* ---- scanout (HW_RENDERER_DESIGN.md §4.4) ----
       Returns the upscaled render target, or NULL when the backend has nothing to
       present and the core should fall back to gpu->vram. `disp_x`/`disp_y` are the
       native scanout origin (gpu.c:2202); the returned pointer is already offset to it.
       *out_stride_bytes is the row pitch of the returned buffer. */
    const void* (*display_buffer)(struct psx_gpu_backend* be,
                                  uint32_t disp_x, uint32_t disp_y,
                                  uint32_t* out_stride_bytes);

    /* Internal-resolution multiplier actually in use, >= 1. */
    int (*resolution_scale)(struct psx_gpu_backend* be);
} psx_gpu_backend_t;

#endif
