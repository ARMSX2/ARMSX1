/*
    ARMSX — hardware rasterizer backend ABI (DRAFT / INERT).

    This header is a DESIGN ARTIFACT. It is not referenced by the Makefile, not included
    by anything, and no implementation exists. See frontend/HW_RENDERER_DESIGN.md.

    Note the basename collision with the existing frontend/gpu_hw.h — that file is the
    vestigial SDL presentation shim (see HW_RENDERER_DESIGN.md §5.5) and is unrelated.
    This one uses a distinct include guard on purpose.

    Layering contract
    -----------------
      * This header is pure C99 + <stdint.h>. It must NEVER pull in a GL, GLES, EGL or
        Vulkan header, because psx/dev/gpu.c will include it (or rather, the relocated
        psx/dev/gpu_backend.h half of it) and the emulator core must stay API-agnostic.
      * Every function pointer may be NULL. A NULL psx_gpu_backend_t* on psx_gpu_t means
        "use the software rasterizer", which must remain bit-identical to today.
      * All coordinates crossing this boundary are NATIVE PlayStation VRAM coordinates.
        The backend owns the resolution scale and applies it internally. The core never
        learns that upscaling exists. (HW_RENDERER_DESIGN.md §3.1)
*/

#ifndef ARMSX_GPU_HW_GL_H
#define ARMSX_GPU_HW_GL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct psx_gpu_t;
struct psx_gpu_backend;

/* ------------------------------------------------------------------------------------
   Vertex format

   float x/y rather than int16_t is deliberate: it costs nothing now and is the seam a
   later PGXP-style precision pass needs (HW_RENDERER_DESIGN.md §2.2, §7.6). Through
   Stage 4 these always hold exact integers, and `w` is always 1.0.

   `color` keeps psx/dev/gpu.c's own 0x00BBGGRR packing (gpu.c:1134-1137) so no
   conversion happens on the hot path.

   `texpage` and `clut` are the RAW 16-bit command words (gpu.c:1105-1106), decoded in
   the fragment shader. They are per-vertex, not per-draw, specifically so that a batch
   can span texpage changes without a state break — see HW_RENDERER_DESIGN.md §7.3.
   ------------------------------------------------------------------------------------ */

typedef struct psx_hw_vertex {
    float    x, y;      /* native VRAM px; drawing offset ALREADY applied (gpu.c:279-284) */
    float    w;         /* 1.0 today; PGXP hook. Never used for perspective divide yet.   */
    uint32_t color;     /* 0x00BBGGRR                                                     */
    uint16_t u, v;      /* native texel coords, 0..255                                    */
    uint16_t texpage;   /* raw texpage word: [3:0] px64 [4] py256 [6:5] transp [8:7] depth */
    uint16_t clut;      /* raw CLUT word: [5:0] x/16, [14:6] y                            */
    uint16_t _pad;
} psx_hw_vertex_t;

/* Primitive attribute flags. Intentionally mirror gpu.h's PA_*/RA_* so the parser in
   gpu_poly()/gpu_rect() can pass them through unmodified. */
enum {
    PSX_HW_RAW       = 0x01,  /* no vertex-colour modulation (gpu.c:367-368)   */
    PSX_HW_TRANSP    = 0x02,  /* semi-transparent (gpu.c:259)                  */
    PSX_HW_TEXTURED  = 0x04,  /* (gpu.c:355)                                   */
    PSX_HW_QUAD      = 0x08,  /* 4 vertices; backend emits 2 triangles         */
    PSX_HW_SHADED    = 0x10   /* Gouraud (gpu.c:325)                           */
};

/* Semi-transparency modes, GPUSTAT bits 6:5 / texpage bits 6:5 (gpu.c:262-266).
   See HW_RENDERER_DESIGN.md §2.5 for the fixed-function blend mapping. */
enum {
    PSX_HW_BLEND_HALF_B_HALF_F = 0,  /* 0.5*B + 0.5*F  gpu.c:411-415 */
    PSX_HW_BLEND_B_PLUS_F      = 1,  /* B + F          gpu.c:416-420 */
    PSX_HW_BLEND_B_MINUS_F     = 2,  /* B - F          gpu.c:421-425 */
    PSX_HW_BLEND_B_PLUS_QF     = 3   /* B + 0.25*F     gpu.c:426-430 */
};

typedef struct psx_hw_primitive {
    psx_hw_vertex_t v[4];
    uint8_t         attrib;      /* PSX_HW_* bits                                   */
    uint8_t         vertex_count;/* 3 or 4                                          */
    uint8_t         blend_mode;  /* PSX_HW_BLEND_*; only meaningful with _TRANSP    */
    uint8_t         dither;      /* GPUSTAT bit 9. gpu.c latches it (1941) but never
                                    reads it — the HW path is where it starts to
                                    matter. HW_RENDERER_DESIGN.md §1.6            */
} psx_hw_primitive_t;

/* ------------------------------------------------------------------------------------
   Backend interface

   Hook sites in psx/dev/gpu.c are tabulated in HW_RENDERER_DESIGN.md §5.2.
   ------------------------------------------------------------------------------------ */

typedef struct psx_gpu_backend {
    void* impl;

    /* ---- lifetime ---- */
    void (*destroy)(struct psx_gpu_backend* be);

    /* ---- drawing (gpu.c:1155-1172, :1059, :1252) ---- */
    void (*draw_poly)(struct psx_gpu_backend* be, const psx_hw_primitive_t* prim);
    void (*draw_rect)(struct psx_gpu_backend* be, const psx_hw_primitive_t* prim);
    /* Flat only: gpu_render_flat_line() discards v1's colour (gpu.c:1252) and applies
       neither dithering nor blending. Polylines are parsed but never drawn today
       (gpu.c:1193-1232). */
    void (*draw_line)(struct psx_gpu_backend* be,
                      const psx_hw_vertex_t* v0,
                      const psx_hw_vertex_t* v1,
                      uint16_t color_bgr555);

    /* ---- VRAM transfers, all in NATIVE coordinates ---- */

    /* GP0(02). Ignores drawing area AND mask bit — gpu.c:1804-1857 (see the commented-out
       clip test at gpu.c:1842-1846, which is correctly commented out). */
    void (*fill_vram)(struct psx_gpu_backend* be,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint16_t color_bgr555);

    /* GP0(80), gpu.c:1859-1891. Stays entirely GPU-side at scale; never round-trips.
       NOTE: gpu.c's own implementation neither wraps at 1024/512 nor handles overlapping
       rects. The backend should do it correctly and accept the divergence
       (HW_RENDERER_DESIGN.md §7.4 item 9). */
    void (*copy_vram)(struct psx_gpu_backend* be,
                      uint32_t sx, uint32_t sy,
                      uint32_t dx, uint32_t dy,
                      uint32_t w, uint32_t h);

    /* GP0(A0), gpu.c:1260-1325. Called ONCE for the whole rect at transfer completion
       (gpu.c:1318), never per halfword. `src` is native 16-bit, `src_stride_px` halfwords. */
    void (*upload_vram)(struct psx_gpu_backend* be,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        const uint16_t* src, uint32_t src_stride_px);

    /* GP0(C0), gpu.c:1776-1802. Called at SETUP time, before psx_gpu_read32() begins
       draining (gpu.c:98-118), so the drain sees valid host data. THIS IS THE STALL —
       it is the only entry point that forces a GPU->CPU readback. Downsamples from the
       upscaled target to native. HW_RENDERER_DESIGN.md §4.3, §4.6. */
    void (*download_vram)(struct psx_gpu_backend* be,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint16_t* dst, uint32_t dst_stride_px);

    /* ---- state (each of these is a potential batch break) ---- */

    /* GP0(E3)/(E4), gpu.c:1953-1960. INCLUSIVE on both ends, matching gpu.c:298-299.
       Maps to a scissor rect scaled by S. */
    void (*set_drawing_area)(struct psx_gpu_backend* be,
                             uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);

    /* GP0(E5), gpu.c:1961-1964. Advisory: the core already bakes the offset into vertex
       positions (gpu.c:279-284), so a backend may ignore this and avoid a batch break. */
    void (*set_drawing_offset)(struct psx_gpu_backend* be, int32_t off_x, int32_t off_y);

    /* GP0(E2), gpu.c:1947-1952. Values arrive PRE-SHIFTED by 3, exactly as gpu.c stores
       them, so the shader applies gpu.c:176-177 verbatim. */
    void (*set_texture_window)(struct psx_gpu_backend* be,
                               uint32_t mask_x, uint32_t mask_y,
                               uint32_t off_x,  uint32_t off_y);

    /* GP0(E6), gpu.c:1965-1967 — currently an empty stub, so this is net-new behaviour
       in this emulator. HW_RENDERER_DESIGN.md §2.6, §7.1. */
    void (*set_mask_bits)(struct psx_gpu_backend* be, int set_on_draw, int check_before_draw);

    /* GP0(E1) and the texpage latch at gpu.c:1111-1117 change GPUSTAT bits 5-6 (blend
       mode) and bit 9 (dither), which sprites read (gpu.c:465). */
    void (*set_draw_state)(struct psx_gpu_backend* be, uint32_t gpustat);

    /* ---- frame / coherency ---- */

    /* Flush the pending batch. Idempotent. */
    void (*flush)(struct psx_gpu_backend* be);

    /* GPU_EVENT_VBLANK, gpu.c:2152-2155 — the ONLY frame boundary this core exposes. */
    void (*end_frame)(struct psx_gpu_backend* be);

    /* Runtime resolution scale change. Reallocates the render target and re-uploads from
       the host VRAM shadow. Clamped to [1, GL_MAX_TEXTURE_SIZE/1024] by the backend. */
    void (*set_resolution_scale)(struct psx_gpu_backend* be, int scale);
    int  (*resolution_scale)(const struct psx_gpu_backend* be);

    /* Scanout without a round trip (HW_RENDERER_DESIGN.md §4.4). Returns an opaque
       backend-native texture handle plus the subrect, in UPSCALED coordinates, that the
       present layer should sample. Returns 0 when the display is disabled
       (GPUSTAT bit 23, gpu.c:2199) or when 24bpp forces the native path. */
    uintptr_t (*display_texture)(struct psx_gpu_backend* be,
                                 int* out_x, int* out_y, int* out_w, int* out_h);

    /* ---- diagnostics: feeds the readback-heavy auto-downgrade in §4.6 ---- */
    void (*frame_stats)(const struct psx_gpu_backend* be,
                        uint32_t* out_draw_calls,
                        uint32_t* out_readback_bytes,
                        uint32_t* out_batch_breaks);
} psx_gpu_backend_t;

/* ------------------------------------------------------------------------------------
   GLES 3.0 backend creation

   Requires a current GL context on the CALLING thread — which must be the emulation
   thread, since psx_gpu_write32() drives every entry point above. See
   HW_RENDERER_DESIGN.md §5.3 for why this needs coordinating with the code that owns
   thread setup before any of this lands.
   ------------------------------------------------------------------------------------ */

typedef struct psx_hw_gl_config {
    int  resolution_scale;      /* 1..8                                                */
    int  true_color;            /* skip the 5-bit truncation (§6 enhancement)          */
    int  dithering;             /* honour GPUSTAT bit 9; 0 disables entirely           */
    int  bilinear_textures;     /* restore the software path's look (gpu.c:359, §7.4)  */
    int  allow_framebuffer_fetch; /* programmable blend where supported; see §2.5 for
                                     why this must be driver-gated, not just probed    */
} psx_hw_gl_config_t;

/* proc_loader mirrors the pattern already used by frontend/render_gl.cpp (GlProcLoader)
   so the two can share one loader. Returns NULL if the context is unusable. */
typedef void* (*psx_hw_gl_proc_loader_t)(const char* name, void* user);

psx_gpu_backend_t* psx_hw_gl_create(psx_hw_gl_proc_loader_t proc_loader,
                                    void* loader_user,
                                    const psx_hw_gl_config_t* config);

/* Cheap capability probe; safe to call without a context. */
int psx_hw_gl_is_supported(void);

#ifdef __cplusplus
}
#endif

#endif /* ARMSX_GPU_HW_GL_H */
