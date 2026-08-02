#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ic.h"
#include "../state.h"
#include "../texrep.h"

#define PSX_GPU_BEGIN 0x1f801810
#define PSX_GPU_SIZE  0x8
#define PSX_GPU_END   0x1f801814

#define PSX_GPU_FB_WIDTH 1024
#define PSX_GPU_FB_HEIGHT 512

// Use this when updating your texture
#define PSX_GPU_FB_STRIDE 2048

// 0x100000 * 2
#define PSX_GPU_VRAM_SIZE (0x200000)

/* Slots in psx_gpu_t::buf, the GP0 argument accumulator. Every fixed-length command fits:
   the largest, GP0(3C) (shaded textured quad), is 12 words. A POLYLINE has no fixed length
   at all, which is why psx_gpu_write32() has to bound the index against this rather than
   trust the command to end -- see the clamp there and the terminator read in gpu_line(). */
#define PSX_GPU_CMD_BUF_SIZE 16

#define PSX_GPU_CLOCK_NTSC 53693175 // 53.693175 MHz
#define PSX_GPU_CLOCK_FREQ_NTSC 53.693175f // 53.693175 MHz
#define PSX_GPU_CLOCK_FREQ_PAL 53.203425f // 53.203425 MHz

enum {
    GPU_EVENT_DMODE,
    GPU_EVENT_VBLANK,
    GPU_EVENT_VBLANK_END,
    GPU_EVENT_HBLANK,
    GPU_EVENT_HBLANK_END,
    GPU_EVENT_VBLANK_TIMER
};

enum {
    GPU_STATE_RECV_CMD,
    GPU_STATE_RECV_ARGS,
    GPU_STATE_RECV_DATA
};

struct psx_gpu_t;

typedef struct psx_gpu_t psx_gpu_t;

typedef void (*psx_gpu_cmd_t)(psx_gpu_t*);
typedef void (*psx_gpu_event_callback_t)(psx_gpu_t*);

enum {
    RS_VARIABLE,
    RS_1X1,
    RS_8X8,
    RS_16X16
};

enum {
    RA_RAW      = 0x01,
    RA_TRANSP   = 0x02,
    RA_TEXTURED = 0x04
};

enum {
    PA_RAW      = 0x01,
    PA_TRANSP   = 0x02,
    PA_TEXTURED = 0x04,
    PA_QUAD     = 0x08,
    PA_SHADED   = 0x10
};

/* These three carry struct tags so psx/dev/gpu_backend.h can name them in the backend
   ABI without including this header (which would be circular). */
typedef struct vertex_t {
    int16_t x, y;
    uint32_t c;
    uint8_t tx, ty;

    /* PGXP (psx/pgxp.c). When precise_valid is set, px/py are the GTE's
       pre-truncation screen coordinates (px/py truncate to x/y by
       construction — the attach validated the source word) and pw is the
       depth the projection divide used (SZ3), for perspective-correct
       interpolation. precise_valid is DEFINED only on the poly parse path
       (gpu_poly fills it for all four v[]s, hit or miss) and on gpu_rect /
       gpu_line's vertices, where it is always 0. NOT serialized: state
       save/load keeps the classic 10-byte layout (gpu_save_vertex) and PGXP
       resets on load. */
    float px, py, pw;
    int precise_valid;
} vertex_t;

typedef struct poly_data_t {
    uint8_t attrib;
    vertex_t v[4];
    uint16_t clut, texp;
} poly_data_t;

typedef struct rect_data_t {
    uint8_t attrib;
    vertex_t v0;
    uint16_t clut;
    uint16_t width, height;
} rect_data_t;

#ifdef USE_HARDWARE
/* Rasterizer backend seam. Opaque here on purpose — the vtable lives in
   psx/dev/gpu_backend.h and only gpu.c and the backend implementation include it, so the
   core never sees a graphics API type. NULL means "software rasterizer", which is the
   default and stays bit-identical to the pre-backend behaviour. */
struct psx_gpu_backend;
#endif

struct psx_gpu_t {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    void* udata[4];

    uint16_t* vram;
    uint16_t* empty;
    int display_enable;

    // State data
    uint32_t buf[PSX_GPU_CMD_BUF_SIZE];
    uint32_t recv_data;
    int buf_index;
    int cmd_args_remaining;
    int cmd_data_remaining;
    int line_done;
    vertex_t prev_line_vertex;

    // Command counters
    uint32_t color;
    uint32_t xpos, ypos;
    uint32_t xsiz, ysiz;
    uint32_t tsiz;
    uint32_t addr;
    uint32_t xcnt, ycnt;
    vertex_t v0, v1, v2, v3;
    uint32_t pal, texp;
    uint32_t c0_xcnt, c0_ycnt;
    uint32_t c0_addr;
    int c0_xsiz, c0_ysiz;
    int c0_tsiz;
    int gp1_10h_req;

    // GPU state
    uint32_t state;

    uint32_t display_mode;
    uint32_t gpuread;
    uint32_t gpustat;

    // Drawing area
    uint32_t draw_x1, draw_y1;
    uint32_t draw_x2, draw_y2;

    // Drawing offset
    int32_t off_x, off_y;

    // PSX_GPU_ACCURACY_* opt-in fixes; 0 reproduces the historical behaviour exactly.
    uint32_t accuracy_flags;
    int texture_filter;   /* 0 nearest (hardware), 1 bilinear, 2 xBR-style */

    /* Texture dumping / replacement (psx/texrep.h). BOTH are NULL unless the feature was
       switched on: psx_texrep_configure() allocates `texrep` only then, and `texrep_bind` is
       only ever written non-NULL by psx_texrep_bind_prim(), which gpu_poly()/gpu_rect() skip
       entirely on a NULL `texrep`. So the disabled cost is one never-taken branch per
       primitive and one never-taken branch per texel — no allocation, no per-frame work.
       `texrep` is owned; freed in psx_gpu_destroy. */
    struct psx_texrep_t* texrep;
    psx_texrep_bind_t    texrep_bind;

    /* Marker-armed one-shot primitive dump (gpu.c, "primitive dump"). All zero unless
       psx_gpu_debug_set_log_dir() was called AND the marker file is present, and the
       whole thing is inert in that state — one branch per primitive, no per-frame work
       once the capture budget is spent. dbg_dir is owned; freed in psx_gpu_destroy. */
    char* dbg_dir;
    void* dbg_file;   /* FILE* while a frame is being captured, else NULL */
    int dbg_poll;     /* vblanks left before the next marker probe */
    int dbg_budget;   /* captures still allowed this process */
    int dbg_lines;    /* lines written for the frame in flight */
    int dbg_seq;      /* 1-based index of the capture in flight */
    int dbg_prims;    /* primitives seen in the frame in flight */

    /* Multi-frame capture (gpu.c, "frame summary"). dbg_frames counts the frames still to
       be captured by the arming in flight; 0/1 is the original one-shot behaviour. In
       summary mode the per-primitive lines are replaced by ONE census line per frame, which
       is what makes a 240-frame capture affordable — a per-frame OSCILLATION is invisible in
       a single frame no matter how detailed it is. See HW_RENDERER_DESIGN.md §0.5.14. */
    int dbg_frames;      /* frames left in this arming */
    int dbg_summary;     /* 1 = census only, no per-primitive lines */
    int dbg_quiet;       /* 1 = gpu_dumpf() drops everything (summary uses its own writer) */
    int dbg_frame_idx;   /* 0-based frame index within the arming */
    int dbg_poly, dbg_rect, dbg_line_prims;  /* per-frame primitive census */
    int dbg_transp;                          /* primitives with semi-transparency on */
    int dbg_tmode[4];                        /* those, by blend mode (B/2+F/2, B+F, B-F, B+F/4) */
    int dbg_fill, dbg_copy, dbg_upload, dbg_download;   /* VRAM ops this frame */
    uint32_t dbg_fill_c;   /* colour of the largest GP0(02) fill this frame */
    int dbg_fill_area;     /* its area, so "largest" is decidable */
    int dbg_big;           /* full-screen-ish primitives itemised this frame */

    // Texture Window
    uint32_t texw_mx, texw_my;
    uint32_t texw_ox, texw_oy;

    // CLUT offset
    uint32_t clut_x, clut_y;

    // Texture page
    uint32_t texp_x, texp_y;
    uint32_t texp_d;

    // Display area
    uint32_t disp_x, disp_y;
    uint32_t disp_x1, disp_x2;
    uint32_t disp_y1, disp_y2;

    /* ---- drawing-offset census: TEST BUILDS ONLY ------------------------------------------

       Residue of the GPUINFO(5) investigation, retained ONLY because the
       `offset-stream-integrity` case in tests/gpu_renderer_parity.c asserts on it and nothing
       else can: it is what proves the GP0 intake neither swallowed, replayed nor invented a
       GP0(E5) across a VRAM fill and an upload, and that every primitive rasterized under the
       offset that was in force when it was submitted.

       Compiled out unless ARMSX_TEST_OFFSET_CENSUS is defined, which ONLY the $(TEST_GPU_BIN)
       rule in the Makefile does. The reason is not the handful of bytes: gpu_offset_census()
       runs once per primitive, at the top of gpu_render_triangle() / _rect() / _flat_line(),
       and at ~1800 primitives a frame a PGO run would shape branch layout and inlining in the
       hottest function in the emulator around code that exists only to serve a test. In a
       release build these fields do not exist and the three rasterizer entry points are
       byte-for-byte what they were before the census was written.

       Note that this changes sizeof(psx_gpu_t) between a test build and a release build. That
       is safe because psx_gpu_save_state() serialises field by field rather than blitting the
       struct, and because every gate compiles the whole core from source with its own flags —
       no object built with the define is ever linked against one built without it. */
#ifdef ARMSX_TEST_OFFSET_CENSUS
    uint32_t gp0_e5_raw;         /* last GP0(E5) payload, verbatim, before extraction */
    uint32_t gp0_e5_count;       /* GP0(E5) writes; >1 per frame means the offset moved */
    int16_t  off_hist_y[4];      /* the offset actually in force as each primitive drew, */
    uint32_t off_hist_n[4];      /* bucketed by Y, overflow folded into the last bucket */
    uint8_t  off_hist_used;
    uint32_t frame_prims;
#endif

    // Timing and IRQs
    float cycles;
    int line;

    psx_ic_t* ic;

    psx_gpu_event_callback_t event_cb_table[8];
#ifdef USE_HARDWARE
    struct psx_gpu_backend* backend;
#endif
};

static inline int psx_gpu_is_pal_mode(const psx_gpu_t* gpu) {
    return gpu && ((gpu->display_mode & 0x8) != 0);
}

static inline float psx_gpu_clock_frequency(const psx_gpu_t* gpu) {
    return psx_gpu_is_pal_mode(gpu) ? PSX_GPU_CLOCK_FREQ_PAL : PSX_GPU_CLOCK_FREQ_NTSC;
}

static inline float psx_gpu_frame_rate(const psx_gpu_t* gpu) {
    return psx_gpu_is_pal_mode(gpu) ? 49.76f : 59.29f;
}

psx_gpu_t* psx_gpu_create(void);
void psx_gpu_init(psx_gpu_t*, psx_ic_t*);
uint32_t psx_gpu_read32(psx_gpu_t*, uint32_t);
uint16_t psx_gpu_read16(psx_gpu_t*, uint32_t);
uint8_t psx_gpu_read8(psx_gpu_t*, uint32_t);
void psx_gpu_write32(psx_gpu_t*, uint32_t, uint32_t);
void psx_gpu_write16(psx_gpu_t*, uint32_t, uint16_t);
void psx_gpu_write8(psx_gpu_t*, uint32_t, uint8_t);
/* VRAM plus every GP0/GP1 latch and the scanline/dot-clock counters. The
   udata[], event_cb_table[], ic and renderer members are host-side and are
   left in place across a load. */
void psx_gpu_save_state(psx_gpu_t*, psx_state_writer_t*);
int psx_gpu_load_state(psx_gpu_t*, psx_state_reader_t*);
void psx_gpu_destroy(psx_gpu_t*);
void psx_gpu_set_udata(psx_gpu_t*, int, void*);
void psx_gpu_set_event_callback(psx_gpu_t*, int, psx_gpu_event_callback_t);
void* psx_gpu_get_display_buffer(psx_gpu_t*);
void psx_gpu_update(psx_gpu_t*, int);
void gpu_render_triangle(psx_gpu_t*, vertex_t, vertex_t, vertex_t, poly_data_t, int);
void gpu_render_rect(psx_gpu_t*, rect_data_t);
void gpu_render_flat_line(psx_gpu_t*, vertex_t, vertex_t, uint32_t);
uint16_t gpu_fetch_texel(psx_gpu_t*, uint16_t, uint16_t, uint32_t, uint32_t, uint16_t, uint16_t, int);
/* Used unconditionally by polygons (gpu.c:359) — sprites point-sample instead. A backend
   has to call the same one the software path would to stay pixel-identical at 1x. */
uint16_t gpu_fetch_texel_bilinear(psx_gpu_t*, float, float, uint32_t, uint32_t, uint16_t, uint16_t, int);

/*
    The float-UV form of gpu_fetch_texel(), for the rasterizer sites that still hold the
    interpolated coordinate. Identical to gpu_fetch_texel() in every respect when no
    replacement is bound — the cast is textually the one those call sites already performed —
    and when one IS bound the fractional part selects the sub-texel, which is the only way a
    replacement finer than the native texel grid can show at an internal resolution above 1x.

    gpu_fetch_texel() carries the same branch, so a call site that only has integers (the
    sprite paths) still gets its replacement; it just samples sub-texel 0, which is all a
    rasterizer running at native resolution could display anyway.

    Replacements deliberately BYPASS [video] texture_filter: see psx/texrep.h.
*/
static inline uint16_t gpu_fetch_texel_f(psx_gpu_t* gpu, float tx, float ty,
                                         uint32_t tpx, uint32_t tpy,
                                         uint16_t clutx, uint16_t cluty, int depth) {
    if (gpu->texrep_bind.img)
        return psx_texrep_sample(gpu, tx, ty);

    return gpu_fetch_texel(gpu, (uint16_t)tx, (uint16_t)ty, tpx, tpy, clutx, cluty, depth);
}

/* Texture filtering mode for the CPU rasterizers: 0 nearest (HARDWARE BEHAVIOUR, default),
   1 bilinear, 2 xBR-style. The PS1 point-samples; bilinear is an enhancement, and applying it
   unconditionally bleeds neighbouring texels across texture-atlas cell boundaries, which reads
   as smearing on foliage and other atlased art. Mirrors [video] texture_filter. */
void psx_gpu_set_texture_filter(psx_gpu_t*, int);
int  psx_gpu_texture_filter(const psx_gpu_t*);

/* [video] texture_filter is an enhancement over the NATIVE texel grid; a bound replacement
   has already replaced that grid at its own resolution, so filtering it would blur detail the
   pack author drew and would force three filter kernels to agree over the atlas as well as
   over VRAM. All three rasterizers consult this one predicate. */
static inline int psx_gpu_filter_active(const psx_gpu_t* gpu) {
    return psx_gpu_texture_filter(gpu) && !gpu->texrep_bind.img;
}

/* The standard PS1 4x4 dither matrix (gpu.c:17-22). */
extern int g_psx_gpu_dither_kernel[];

/*
    Opt-in accuracy fixes. Both of these are behaviours real hardware has and this core
    has never had, so turning them on CHANGES OUTPUT in games that rely on them — see
    HW_RENDERER_DESIGN.md §7.1, which ranks the mask bit as the highest-regression item in
    the whole port. They therefore default to 0, which reproduces the historical behaviour
    exactly, and both rasterizers (software and any backend) read the same flags so the 1x
    parity gate holds in either state.
*/
enum {
    /* GP0(E6): "set mask while drawing" (GPUSTAT bit 11) and "check mask before draw"
       (GPUSTAT bit 12). gpu.c:2051 was an empty stub before this. */
    PSX_GPU_ACCURACY_MASK_BIT    = 0x01,
    /* Honour GPUSTAT bit 9 instead of dithering every Gouraud primitive unconditionally.
       gpu.c latches the bit (gpu.c:2026) but never read it. */
    PSX_GPU_ACCURACY_DITHER_GATE = 0x02,
    /* Drop polygons at the size hardware drops them: see psx_gpu_prim_oversize(). */
    PSX_GPU_ACCURACY_PRIM_SIZE   = 0x04,
    /* Truncate texture modulation instead of rounding: see psx_gpu_modulate_channel(). */
    PSX_GPU_ACCURACY_TEX_MODULATE = 0x08
};

void psx_gpu_set_accuracy_flags(psx_gpu_t*, uint32_t);
uint32_t psx_gpu_accuracy_flags(const psx_gpu_t*);

/*
    One channel of texture-blend modulation, shared so the rasterizers cannot drift.

    Hardware (psx-spx, "Texture Color Blending"): the 5-bit texture channel is expanded to
    8 bits, multiplied by the 8-bit primitive colour, divided by 128 with INTEGER division,
    and saturated. `tex5*8 * mod8 >> 7`. Division truncates.

    This core computed the same product in float and then `roundf()`d it. On any ordinary
    primitive that is a <=1/31 difference nobody can see — it differs on 4.9% of the 32x256
    (texel, modulator) pairs and always by exactly one level, never at the identity
    modulator 0x80.

    It is NOT invisible inside a frame-feedback loop, and Silent Hill's loading screen is
    one: every frame it blits the previous frame back over itself through this very
    multiply, alternating an identity pass (mod 0x80) with a decay pass (mod 0x7f), which is
    how the running character's motion blur is supposed to fade out.

    With truncation every level 1..31 decays: a trail reaches black in ~60 frames.
    With rounding, `round(t5 * 7.9375)` lands back on `t5 * 8` for every t5 <= 8, so levels
    1..8 are FIXED POINTS. The trail decays to 8/31 and then sticks there forever — a
    permanent 26%-brightness ghost of every pose the character has ever been in. See
    HW_RENDERER_DESIGN.md §0.5.15.

    tex5 is 0..31, mod8 is 0..255; the result is the 8-bit channel the framebuffer packer
    then shifts down to 5 bits.
*/
static inline unsigned int psx_gpu_modulate_channel(const psx_gpu_t* gpu,
                                                    unsigned int tex5, unsigned int mod8) {
    if (gpu->accuracy_flags & PSX_GPU_ACCURACY_TEX_MODULATE) {
        const unsigned int c = ((tex5 << 3) * mod8) >> 7;

        return (c > 255u) ? 255u : c;
    }

    {
        /* Historical path, kept bit-identical: float multiply then round-half-up. For a
           non-negative value that is exactly what roundf() computes. */
        float c = (float)((tex5 << 3) * mod8) / 128.0f;

        c = (c >= 255.0f) ? 255.0f : ((c <= 0.0f) ? 0.0f : c);

        return (unsigned int)(c + 0.5f);
    }
}

/* Hardware's polygon/line size cull, as a shared predicate so the three rasterizers cannot
   drift apart on which primitives get dropped.

   The rule (psx-spx, "GPU Render Polygon Commands"): the maximum distance between two
   vertices is 1023 horizontally and 511 vertically, and polygons exceeding that are NOT
   rendered at all. DuckStation states the same limit as MAX_PRIMITIVE_WIDTH 1024 /
   MAX_PRIMITIVE_HEIGHT 512, rejecting at >=.

   This core shipped `> 2048 / > 1024` in all three rasterizers, i.e. twice as permissive as
   hardware in both axes, and two of them carry a comment saying so. The difference is not
   academic: the GTE saturates projected SX/SY to -1024..+1023, so geometry that runs past
   the near plane arrives with one or more vertices pinned at exactly +1023, and games rely
   on the GPU throwing those primitives away. Drawing them instead produces a triangle
   stretched from the model down to the screen edge, tapering to the saturated vertex.

   Measured in Silent Hill (SLUS-00707), one frame of the foggy-street scene
   (gpu_prim_dump_capture.txt): 16 of 1540 rasterized triangles exceed the hardware limit,
   14 vertices sit exactly at the saturation value, and 2 of the oversized triangles overlap
   the visible draw area. See HW_RENDERER_DESIGN.md §0.5.13.

   Spans are taken on the integer bounding box AFTER the drawing offset, which is a pure
   translation and so cannot change them. */
static inline int psx_gpu_prim_oversize(const psx_gpu_t* gpu, int xspan, int yspan) {
    if (gpu->accuracy_flags & PSX_GPU_ACCURACY_PRIM_SIZE)
        return (xspan > 1023) || (yspan > 511);

    return (xspan > 2048) || (yspan > 1024);
}

/*
    One-shot, marker-armed primitive dump.

    `dir` is the directory that holds the diagnostics log (frontend/diagnostics.c's
    psxe_diag_log_path() minus the file name; a trailing '/' is optional). The GPU keeps its
    OWN copy, so the caller's buffer may go away. NULL or "" disables the dump and releases
    any copy already held.

    Deliberately pushed in rather than pulled: psx/ must not link frontend/diagnostics.c —
    tests/gpu_renderer_parity.c and tests/cpu_differential.c compile gpu.c without it.

    With a directory set, gpu.c probes for a file named `gpu_prim_dump` in it every ~half
    second at vblank. When the marker appears, the NEXT frame — exactly one — is written to
    `gpu_prim_dump.txt` beside it and the marker is deleted, so one `touch` buys one frame.
*/
void psx_gpu_debug_set_log_dir(psx_gpu_t*, const char* dir);

/* Resolved once per primitive by both rasterizers so they cannot drift apart. */
static inline int psx_gpu_mask_check(const psx_gpu_t* gpu) {
    return (gpu->accuracy_flags & PSX_GPU_ACCURACY_MASK_BIT) && ((gpu->gpustat & 0x1000) != 0);
}

static inline int psx_gpu_mask_set(const psx_gpu_t* gpu) {
    return (gpu->accuracy_flags & PSX_GPU_ACCURACY_MASK_BIT) && ((gpu->gpustat & 0x0800) != 0);
}

/*
    The OTHER half of GP0(E6) bit 0, and the half that was missing.

    "Set mask while drawing" is 0=TextureBit15, 1=ForceBit15=1. psx_gpu_mask_set() above is
    only the force case; when the bit is CLEAR hardware still writes a mask bit — the source
    texel's bit 15 — and untextured primitives write 0. HW_RENDERER_DESIGN.md §2.6 states the
    rule as `force_mask || texel_bit15`.

    Dropping the texel half is what put a lighter rectangle around every character in Silent
    Hill: the game primes the frame with a full-screen quad at "set mask" (so bit 15 is 1
    everywhere), draws the world, then draws one flat semi-transparent quad per object over
    that object's screen bounding box at "set+check mask" to fog it. Geometry whose texels
    carry STP=1 is supposed to keep bit 15 and be skipped by that fog quad; with the texel
    half missing every textured write cleared bit 15, nothing was skipped, and the fog quad
    painted its whole bounding box. See gpu_prim_dump capture #2 primitives 00407 / 00833.

    Gated by the same accuracy flag as the rest of the mask bit so the shipping default path
    stays byte-identical (tests/gpu_renderer_parity.c "default-path-unchanged").
*/
static inline uint16_t psx_gpu_mask_from_texel(const psx_gpu_t* gpu) {
    return (gpu->accuracy_flags & PSX_GPU_ACCURACY_MASK_BIT) ? 0x8000u : 0x0000u;
}

/*
    The mask stage as two expressions, in a form BOTH C and GLSL accept verbatim.

    The GLES rasterizer (frontend/gpu_hw_gl.c) cannot call the helpers above: its mask stage
    runs per fragment inside a shader. Writing the same rule out a fourth time, in a second
    language, is exactly how the four bugs of 2026-08-01 happened — all three rasterizers
    were wrong IDENTICALLY, which makes any comparison between them blind. So the rule is
    written ONCE here and the GL shader is COMPILED FROM THIS TEXT: PSX_GPU_MASK_GLSL below
    stringifies these same macro bodies into two `#define`s that are prepended to the
    fragment shader source. `||` and `&&` mean the same thing in C and in GLSL ES, and
    macros (rather than functions) are also what Adreno's shader compiler wants — see
    armsx_gpu_profile_t::shader_helpers_must_be_macros.

    WRITE — the destination's bit 15 after the write, GP0(E6) bit 0:
        force       psx_gpu_mask_set()        "set mask while drawing"
        from_texel  psx_gpu_mask_from_texel() non-zero once the accuracy flag is on
        texel_stp   bit 15 of the source texel; 0 for untextured primitives
      i.e. `force || texel_bit15`, HW_RENDERER_DESIGN.md §2.6 and §0.5.12.

    SKIP — whether the write happens at all, GP0(E6) bit 1:
        check       psx_gpu_mask_check()      "check mask before draw"
        dst_bit15   bit 15 already in the destination pixel

    tests/gpu_renderer_parity.c drives BOTH against the software rasterizer's observed
    output over the whole input matrix ("mask-contract-*"), so these are pinned to
    psx/dev/gpu.c's behaviour rather than to themselves.
*/
#define PSX_GPU_MASK_WRITE(force, from_texel, texel_stp) ((force) || ((from_texel) && (texel_stp)))
#define PSX_GPU_MASK_SKIP(check, dst_bit15)              ((check) && (dst_bit15))

/* Two levels so the argument is macro-expanded before it is stringified. */
#define PSX_GPU_STR_(x) #x
#define PSX_GPU_STR(x)  PSX_GPU_STR_(x)

/* The same two bodies, as GLSL source. Prepended to the fragment shader by the backend. */
#define PSX_GPU_MASK_GLSL \
    "#define PSX_GPU_MASK_WRITE(force, from_texel, texel_stp) " \
        PSX_GPU_STR(PSX_GPU_MASK_WRITE(force, from_texel, texel_stp)) "\n" \
    "#define PSX_GPU_MASK_SKIP(check, dst_bit15) " \
        PSX_GPU_STR(PSX_GPU_MASK_SKIP(check, dst_bit15)) "\n"

static inline int psx_gpu_dither_enabled(const psx_gpu_t* gpu) {
    if (!(gpu->accuracy_flags & PSX_GPU_ACCURACY_DITHER_GATE))
        return 1;

    return (gpu->gpustat & 0x0200) != 0;
}

#ifdef USE_HARDWARE
/* Install the rasterizer backend, or NULL to go back to the software path. The GPU does
   not take ownership — the caller destroys the backend after psx_gpu_destroy(). */
void psx_gpu_set_backend(psx_gpu_t*, struct psx_gpu_backend*);

/* Internal-resolution multiplier of the installed backend; 1 when there is none. */
int psx_gpu_resolution_scale(psx_gpu_t*);

/* Non-zero when a backend is installed AND owns the scanout, i.e. when
   psx_gpu_get_display_surface() will hand back the backend's target rather than gpu->vram.
   The frontend needs this at scale 1 too: a GPU backend's output must be presented even
   when it is the same size as host VRAM, or a broken backend would be invisible behind the
   software shadow's still-correct image. */
int psx_gpu_backend_owns_display(psx_gpu_t*);

/* Scanout source. Hands back the backend's upscaled target when one is installed, and
   gpu->vram at native resolution otherwise or when `want_native` is set (the whole-VRAM
   debug view and 24bpp scanout both have to stay native — HW_RENDERER_DESIGN.md §4.4).
   *out_scale and *out_stride_bytes describe the buffer that was returned. */
const void* psx_gpu_get_display_surface(psx_gpu_t*, int want_native,
                                        int* out_scale, uint32_t* out_stride_bytes);
#endif

#endif
