#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/dev/gpu.h"
#include "../psx/dev/gpu_backend.h"
#include "../psx/perf.h"
#include "../frontend/gpu_hw.h"
#include "../frontend/gpu_hw_rt.h"

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

void psx_ic_irq(psx_ic_t* ic, int id) {
    (void)ic;
    (void)id;
}

/* psx/dev/gpu.c carries save-state code that pulls in psx/state.c, which in turn wants
   every other device in the machine. None of it is exercised here, so it is stubbed
   rather than linked. */
void psx_sw_u8(psx_state_writer_t* w, uint8_t v) { (void)w; (void)v; }
void psx_sw_u16(psx_state_writer_t* w, uint16_t v) { (void)w; (void)v; }
void psx_sw_u32(psx_state_writer_t* w, uint32_t v) { (void)w; (void)v; }
void psx_sw_i32(psx_state_writer_t* w, int32_t v) { (void)w; (void)v; }
void psx_sw_f32(psx_state_writer_t* w, float v) { (void)w; (void)v; }
void psx_sw_u16_array(psx_state_writer_t* w, const uint16_t* v, size_t n) { (void)w; (void)v; (void)n; }
uint8_t psx_sr_u8(psx_state_reader_t* r) { (void)r; return 0; }
uint16_t psx_sr_u16(psx_state_reader_t* r) { (void)r; return 0; }
uint32_t psx_sr_u32(psx_state_reader_t* r) { (void)r; return 0; }
int32_t psx_sr_i32(psx_state_reader_t* r) { (void)r; return 0; }
float psx_sr_f32(psx_state_reader_t* r) { (void)r; return 0.0f; }
void psx_sr_u16_array(psx_state_reader_t* r, uint16_t* v, size_t n) { (void)r; (void)v; (void)n; }

/* Applied to every psx_gpu_t the corpus builds, so the mask-bit and dither-gate fixes are
   exercised in BOTH states: off (the historical behaviour, and the default) and on. */
static uint32_t g_test_accuracy = 0;

/* OR-ed into GPUSTAT immediately before every draw. With g_test_accuracy == 0 (the
   shipping default) GPUSTAT bits 9, 11 and 12 must have NO observable effect, so rendering
   the corpus with these bits forced on and forced off has to produce identical VRAM. That
   is the regression guard for "the software rasterizer stays untouched by default". */
static uint32_t g_test_force_gpustat = 0;

static psx_gpu_t* make_gpu(void) {
    psx_gpu_t* gpu = psx_gpu_create();
    if (!gpu)
        return NULL;
    psx_gpu_init(gpu, NULL);
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = PSX_GPU_FB_WIDTH - 1;
    gpu->draw_y2 = PSX_GPU_FB_HEIGHT - 1;
    psx_gpu_set_accuracy_flags(gpu, g_test_accuracy);
    return gpu;
}

static int run_case(const char* name, vertex_t a, vertex_t b, vertex_t c, poly_data_t data, int edge) {
    psx_gpu_t* reference = make_gpu();
    psx_gpu_t* accelerated = make_gpu();
    if (!reference || !accelerated) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    for (size_t index = 0; index < PSX_GPU_VRAM_SIZE / sizeof(uint16_t); ++index) {
        const uint16_t value = (uint16_t)((index * 1103515245u + 12345u) >> 16);
        reference->vram[index] = value;
        accelerated->vram[index] = value;
    }

    gpu_render_triangle(reference, a, b, c, data, edge);
    gpu_hw_render_triangle(accelerated, a, b, c, data, edge);

    const int mismatch = memcmp(reference->vram, accelerated->vram, PSX_GPU_VRAM_SIZE) != 0;
    psx_gpu_destroy(reference);
    psx_gpu_destroy(accelerated);
    if (mismatch) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=vram-mismatch\n", name);
        return 1;
    }
    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

/* ------------------------------------------------------------------------------------
   Internal-resolution backend gate (frontend/gpu_hw_rt.c).

   Two properties are checked, and together they are what makes upscaling trustworthy:

     1x parity      at internal_scale = 1 the backend's render target must be
                    BYTE-IDENTICAL to the software rasterizer's VRAM. If it is not, the
                    coordinate math is wrong and every higher multiplier is wrong too.

     scale coherence sampling the internal_scale = N render target at (x*N, y*N) must
                    reproduce the 1x render target exactly. The edge functions scale by
                    N^2, so coverage, the top-left fill rule, barycentric ratios, UVs and
                    the native-indexed dither are all supposed to be scale-invariant at
                    those sample points. This catches the half-pixel class of bug that
                    HW_RENDERER_DESIGN.md §3.3 warns is invisible at 1x.
   ------------------------------------------------------------------------------------ */

static void seed_vram(psx_gpu_t* gpu) {
    for (size_t index = 0; index < PSX_GPU_VRAM_SIZE / sizeof(uint16_t); ++index)
        gpu->vram[index] = (uint16_t)((index * 1103515245u + 12345u) >> 16);
}

/* Texture page parked at VRAM (512, 0), CLUT at (0, 400). Encoded the way gpu.c:254-258
   decodes it: texp[3:0] = x/64, texp[4] = y/256, texp[6:5] = blend mode, texp[8:7] = depth.
   clut[5:0] = x/16, clut[14:6] = y. */
#define TEXP_AT_512(depth, mode) \
    ((uint16_t)(8u | ((uint32_t)(mode) << 5) | ((uint32_t)(depth) << 7)))
#define CLUT_AT_0_400 ((uint16_t)(400u << 6))

/* Sprites read the texture page from the latched gpu->texp_* registers (gpu.c:501-503),
   not from a command word, so they have to be set up separately. */
static void latch_texpage(psx_gpu_t* gpu, int depth) {
    gpu->texp_x = 512;
    gpu->texp_y = 0;
    gpu->texp_d = (uint32_t)depth;
}

/* Mirrors the dispatch order in gpu.c: backend first, software second. */
static void emit_poly(psx_gpu_t* gpu, const poly_data_t* poly) {
    gpu->gpustat |= g_test_force_gpustat;

    if (gpu->backend && gpu->backend->draw_poly)
        gpu->backend->draw_poly(gpu->backend, gpu, poly);

    if (poly->attrib & PA_QUAD) {
        gpu_render_triangle(gpu, poly->v[0], poly->v[1], poly->v[2], *poly, 1);
        gpu_render_triangle(gpu, poly->v[1], poly->v[2], poly->v[3], *poly, 1);
    } else {
        gpu_render_triangle(gpu, poly->v[0], poly->v[1], poly->v[2], *poly, 0);
    }
}

static void emit_rect(psx_gpu_t* gpu, const rect_data_t* rect) {
    gpu->gpustat |= g_test_force_gpustat;

    if (gpu->backend && gpu->backend->draw_rect)
        gpu->backend->draw_rect(gpu->backend, gpu, rect);

    gpu_render_rect(gpu, *rect);
}

static void emit_line(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, uint16_t color) {
    gpu->gpustat |= g_test_force_gpustat;
    if (gpu->backend && gpu->backend->draw_line)
        gpu->backend->draw_line(gpu->backend, gpu, &v0, &v1, color);

    gpu_render_flat_line(gpu, v0, v1, color);
}

/* The corpus every backend case runs. Deliberately covers the fill-rule and clipping
   edges: shared-edge pairs, a degenerate triangle, negative drawing offsets, primitives
   straddling each drawing-area boundary, and all four semi-transparency modes. */
static void draw_corpus(psx_gpu_t* gpu) {
    poly_data_t flat = {.attrib = 0};
    flat.v[0].c = flat.v[1].c = flat.v[2].c = 0x40a0f0;
    flat.v[0] = (vertex_t){.x = 10, .y = 12, .c = 0x40a0f0};
    flat.v[1] = (vertex_t){.x = 92, .y = 24, .c = 0x40a0f0};
    flat.v[2] = (vertex_t){.x = 38, .y = 105, .c = 0x40a0f0};
    emit_poly(gpu, &flat);

    /* Shared-edge pair: no gap and no double-cover at any scale. */
    poly_data_t quad = {.attrib = PA_QUAD | PA_SHADED};
    quad.v[0] = (vertex_t){.x = 140, .y = 30, .c = 0x0000ff};
    quad.v[1] = (vertex_t){.x = 260, .y = 44, .c = 0x00ff00};
    quad.v[2] = (vertex_t){.x = 132, .y = 150, .c = 0xff0000};
    quad.v[3] = (vertex_t){.x = 255, .y = 165, .c = 0xffff00};
    emit_poly(gpu, &quad);

    /* Degenerate / zero-area. */
    poly_data_t degenerate = {.attrib = 0};
    degenerate.v[0] = (vertex_t){.x = 400, .y = 400, .c = 0x00ff00};
    degenerate.v[1] = (vertex_t){.x = 400, .y = 400, .c = 0x00ff00};
    degenerate.v[2] = (vertex_t){.x = 460, .y = 400, .c = 0x00ff00};
    emit_poly(gpu, &degenerate);

    /* Clipped against every drawing-area boundary, with a negative offset applied. */
    gpu->draw_x1 = 64;
    gpu->draw_y1 = 48;
    gpu->draw_x2 = 320;
    gpu->draw_y2 = 240;
    gpu->off_x = -12;
    gpu->off_y = -9;

    poly_data_t clipped = {.attrib = PA_SHADED};
    clipped.v[0] = (vertex_t){.x = 40, .y = 20, .c = 0x3080c0};
    clipped.v[1] = (vertex_t){.x = 360, .y = 96, .c = 0xc08030};
    clipped.v[2] = (vertex_t){.x = 100, .y = 300, .c = 0x20f0a0};
    emit_poly(gpu, &clipped);

    /* All four blend modes, against live destination content. */
    for (int mode = 0; mode < 4; ++mode) {
        gpu->gpustat = (gpu->gpustat & ~(3u << 5)) | ((uint32_t)mode << 5);

        poly_data_t blended = {.attrib = PA_TRANSP};
        blended.v[0].c = blended.v[1].c = blended.v[2].c = 0xffffff;
        blended.v[0] = (vertex_t){.x = (int16_t)(90 + mode * 34), .y = 70, .c = 0xffffff};
        blended.v[1] = (vertex_t){.x = (int16_t)(126 + mode * 34), .y = 96, .c = 0xffffff};
        blended.v[2] = (vertex_t){.x = (int16_t)(96 + mode * 34), .y = 150, .c = 0xffffff};
        emit_poly(gpu, &blended);
    }

    gpu->off_x = 0;
    gpu->off_y = 0;

    /* Sprites: each size class, opaque and semi-transparent. */
    for (int size = 0; size < 4; ++size) {
        rect_data_t rect = {0};
        rect.attrib = (uint8_t)((size << 3) | (size == 1 ? RA_TRANSP : 0));
        rect.v0 = (vertex_t){.x = (int16_t)(80 + size * 40), .y = 190, .c = 0x60c0a0};
        rect.width = 23;
        rect.height = 17;
        emit_rect(gpu, &rect);
    }

    /* Textured polygons: every depth, plus the raw and semi-transparent variants. The
       texel-0 discard, the CLUT decode and the (t*m)/128 modulation all live here. */
    for (int depth = 0; depth < 3; ++depth) {
        const uint8_t variants[] = {
            PA_TEXTURED,
            PA_TEXTURED | PA_SHADED,
            PA_TEXTURED | PA_RAW,
            PA_TEXTURED | PA_TRANSP
        };

        for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
            poly_data_t tex = {.attrib = variants[i]};

            tex.texp = TEXP_AT_512(depth, (int)i & 3);
            tex.clut = CLUT_AT_0_400;
            tex.v[0] = (vertex_t){.x = (int16_t)(70 + depth * 60), .y = 260, .c = 0x9070b0, .tx = 4,  .ty = 6};
            tex.v[1] = (vertex_t){.x = (int16_t)(126 + depth * 60), .y = 272, .c = 0x30c060, .tx = 60, .ty = 12};
            tex.v[2] = (vertex_t){.x = (int16_t)(84 + depth * 60), .y = 330, .c = 0xd0a020, .tx = 12, .ty = 58};
            emit_poly(gpu, &tex);
        }
    }

    /* Textured sprites, which point-sample and inherit the latched texture page. */
    for (int depth = 0; depth < 3; ++depth) {
        latch_texpage(gpu, depth);

        rect_data_t tex_rect = {0};
        tex_rect.attrib = RA_TEXTURED | (depth == 1 ? RA_TRANSP : 0);
        tex_rect.clut = CLUT_AT_0_400;
        tex_rect.v0 = (vertex_t){.x = (int16_t)(70 + depth * 50), .y = 350, .c = 0x808080, .tx = 8, .ty = 8};
        tex_rect.width = 21;
        tex_rect.height = 19;
        emit_rect(gpu, &tex_rect);
    }

    /* Mask bit: lay down a run of primitives with "set mask while drawing" on, then draw
       over them with "check mask before draw" on. With PSX_GPU_ACCURACY_MASK_BIT enabled
       the second set must be occluded; with it off both sets draw normally. GPUSTAT bit 9
       is toggled too so the dither gate is exercised in both states. */
    gpu->gpustat |= 0x0800;                 /* set mask while drawing   */
    gpu->gpustat &= ~0x0200u;               /* dither disabled          */

    poly_data_t masker = {.attrib = PA_SHADED};
    masker.v[0] = (vertex_t){.x = 230, .y = 198, .c = 0x40f080};
    masker.v[1] = (vertex_t){.x = 306, .y = 210, .c = 0x8040f0};
    masker.v[2] = (vertex_t){.x = 250, .y = 236, .c = 0xf08040};
    emit_poly(gpu, &masker);

    gpu->gpustat &= ~0x0800u;
    gpu->gpustat |= 0x1000;                 /* check mask before draw   */
    gpu->gpustat |= 0x0200;                 /* dither enabled           */

    poly_data_t maskee = {.attrib = PA_SHADED};
    maskee.v[0] = (vertex_t){.x = 236, .y = 200, .c = 0xf0f0f0};
    maskee.v[1] = (vertex_t){.x = 312, .y = 222, .c = 0x101010};
    maskee.v[2] = (vertex_t){.x = 256, .y = 238, .c = 0x8080f0};
    emit_poly(gpu, &maskee);

    rect_data_t maskee_rect = {0};
    maskee_rect.attrib = 0;
    maskee_rect.v0 = (vertex_t){.x = 232, .y = 202, .c = 0x2080d0};
    maskee_rect.width = 40;
    maskee_rect.height = 30;
    emit_rect(gpu, &maskee_rect);

    gpu->gpustat &= ~0x1800u;

    /* Lines: shallow, steep and reversed, so both Bresenham branches run. */
    emit_line(gpu, (vertex_t){.x = 70, .y = 60}, (vertex_t){.x = 300, .y = 130}, 0x7c1f);
    emit_line(gpu, (vertex_t){.x = 290, .y = 220}, (vertex_t){.x = 110, .y = 55}, 0x03ff);

    /* Transfers. fill and copy go through the same hooks gpu.c calls. */
    if (gpu->backend && gpu->backend->fill_vram)
        gpu->backend->fill_vram(gpu->backend, 512, 300, 64, 32, 0x1f00);

    for (int y = 300; y < 332; y++)
        for (int x = 512; x < 576; x++)
            gpu->vram[x + (y * 1024)] = 0x1f00;

    if (gpu->backend && gpu->backend->copy_vram)
        gpu->backend->copy_vram(gpu->backend, 512, 300, 600, 360, 64, 32);

    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            gpu->vram[(600 + x) + ((360 + y) * 1024)] =
                gpu->vram[(512 + x) + ((300 + y) * 1024)];
        }
    }
}

/* Reads the backend's render target at native coordinate (x, y): the top-left texel of
   that native pixel's SxS block. */
static uint16_t rt_texel(psx_gpu_backend_t* be, int scale, int x, int y) {
    uint32_t stride_bytes = 0;
    const uint16_t* base = (const uint16_t*)be->display_buffer(be, 0, 0, &stride_bytes);
    const size_t stride_px = stride_bytes / sizeof(uint16_t);

    return base[(size_t)x * scale + ((size_t)y * scale * stride_px)];
}

/* Renders the corpus twice at the shipping defaults, once with GPUSTAT bits 9/11/12
   forced high and once with them left alone, and requires the two VRAMs to be identical. */
static int run_default_inertness_case(const char* name) {
    psx_gpu_t* a = make_gpu();
    psx_gpu_t* b = make_gpu();

    if (!a || !b) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    seed_vram(a);
    seed_vram(b);

    g_test_force_gpustat = 0;
    draw_corpus(a);

    g_test_force_gpustat = 0x1a00;   /* dither-enable + set-mask + check-mask */
    draw_corpus(b);
    g_test_force_gpustat = 0;

    const int mismatch = memcmp(a->vram, b->vram, PSX_GPU_VRAM_SIZE) != 0;

    psx_gpu_destroy(a);
    psx_gpu_destroy(b);

    if (mismatch) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=default-path-changed "
                "(GPUSTAT 9/11/12 altered output with accuracy flags off)\n", name);
        return 1;
    }

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

static int run_backend_case(const char* name, int scale) {
    psx_gpu_t* gpu = make_gpu();

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    seed_vram(gpu);

    psx_gpu_backend_t* be = armsx_hw_rt_create(gpu, scale);

    if (!be) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=backend-create\n", name);
        psx_gpu_destroy(gpu);
        return 1;
    }

    psx_gpu_set_backend(gpu, be);
    draw_corpus(gpu);

    /* At scale 1 the render target must equal the software rasterizer's VRAM exactly. */
    int mismatch = 0;
    int first_x = -1, first_y = -1;

    if (scale == 1) {
        for (int y = 0; y < PSX_GPU_FB_HEIGHT && !mismatch; y++) {
            for (int x = 0; x < PSX_GPU_FB_WIDTH; x++) {
                if (rt_texel(be, 1, x, y) != gpu->vram[x + (y * 1024)]) {
                    mismatch = 1;
                    first_x = x;
                    first_y = y;
                    break;
                }
            }
        }
    }

    psx_gpu_set_backend(gpu, NULL);

    if (mismatch) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=rt-vram-mismatch at (%d,%d) rt=%04x vram=%04x\n",
                name, first_x, first_y,
                rt_texel(be, 1, first_x, first_y),
                gpu->vram[first_x + (first_y * 1024)]);
    }

    armsx_hw_rt_destroy(be);
    psx_gpu_destroy(gpu);

    if (mismatch)
        return 1;

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

/* Renders the corpus at 1x and at `scale`, then requires the scaled target sampled every
   `scale` pixels to reproduce the 1x target. */
static int run_scale_case(const char* name, int scale) {
    psx_gpu_t* a = make_gpu();
    psx_gpu_t* b = make_gpu();

    if (!a || !b) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    seed_vram(a);
    seed_vram(b);

    psx_gpu_backend_t* be1 = armsx_hw_rt_create(a, 1);
    psx_gpu_backend_t* ben = armsx_hw_rt_create(b, scale);

    if (!be1 || !ben) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=backend-create\n", name);
        return 1;
    }

    psx_gpu_set_backend(a, be1);
    psx_gpu_set_backend(b, ben);

    draw_corpus(a);
    draw_corpus(b);

    int mismatch = 0;
    int first_x = -1, first_y = -1;

    for (int y = 0; y < PSX_GPU_FB_HEIGHT && !mismatch; y++) {
        for (int x = 0; x < PSX_GPU_FB_WIDTH; x++) {
            if (rt_texel(be1, 1, x, y) != rt_texel(ben, scale, x, y)) {
                mismatch = 1;
                first_x = x;
                first_y = y;
                break;
            }
        }
    }

    if (mismatch) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=scale-incoherent at native (%d,%d) 1x=%04x %dx=%04x\n",
                name, first_x, first_y,
                rt_texel(be1, 1, first_x, first_y), scale,
                rt_texel(ben, scale, first_x, first_y));
    }

    psx_gpu_set_backend(a, NULL);
    psx_gpu_set_backend(b, NULL);
    armsx_hw_rt_destroy(be1);
    armsx_hw_rt_destroy(ben);
    psx_gpu_destroy(a);
    psx_gpu_destroy(b);

    if (mismatch)
        return 1;

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

/* ------------------------------------------------------------------------------------
   GP0(E6) bit 0 == 0 means "write the SOURCE TEXEL's bit 15 into the destination mask
   bit", not "write 0". Only the force-to-1 half used to be implemented, so every textured
   write cleared bit 15.

   This reproduces the idiom Silent Hill uses, which is what that broke (gpu_prim_dump
   capture #2, primitives 00002 / 00833):

     1. prime the frame with a "set mask while drawing" quad, so bit 15 is 1 everywhere;
     2. draw the world — geometry whose texels carry STP=1 must KEEP bit 15;
     3. per object, draw one flat semi-transparent quad over its screen bounding box with
        "check mask before draw" on, to fog it.

   With step 2 clearing bit 15, step 3 is never masked and paints its whole bounding box —
   a lighter rectangle around every character. The corpus-based cases cannot catch this:
   they compare the two rasterizers against each other, and both were wrong the same way.
   This one asserts the behaviour outright.
   ------------------------------------------------------------------------------------ */
static int check_mask_from_texel(const char* name, uint32_t accuracy,
                                 uint16_t texel, int expect_masked) {
    psx_gpu_t* gpu = psx_gpu_create();

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_gpu_init(gpu, NULL);
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = PSX_GPU_FB_WIDTH - 1;
    gpu->draw_y2 = PSX_GPU_FB_HEIGHT - 1;
    gpu->off_x = 0;
    gpu->off_y = 0;
    psx_gpu_set_accuracy_flags(gpu, accuracy);

    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);

    /* 15bpp texture page at (512, 0) so `texel` reaches the rasterizer verbatim, bit 15
       included. Every bilinear tap of UV (8,8) lands inside this block. */
    for (int y = 0; y < 16; y++)
        for (int x = 512; x < 528; x++)
            gpu->vram[x + (y * 1024)] = texel;

    /* Additive blend, dither off, mask bits clear. */
    gpu->gpustat = (gpu->gpustat & ~0x1a60u) | (1u << 5);

    /* Step 2: an opaque, non-raw textured quad. Constant UVs on all four vertices, so the
       interpolated coordinate is exactly (8,8) at every covered pixel. */
    poly_data_t tex = {.attrib = PA_TEXTURED | PA_QUAD};
    tex.texp = TEXP_AT_512(2, 0);
    tex.clut = CLUT_AT_0_400;
    tex.v[0] = (vertex_t){.x = 100, .y = 100, .c = 0x808080, .tx = 8, .ty = 8};
    tex.v[1] = (vertex_t){.x = 140, .y = 100, .c = 0x808080, .tx = 8, .ty = 8};
    tex.v[2] = (vertex_t){.x = 100, .y = 140, .c = 0x808080, .tx = 8, .ty = 8};
    tex.v[3] = (vertex_t){.x = 140, .y = 140, .c = 0x808080, .tx = 8, .ty = 8};
    gpu_render_triangle(gpu, tex.v[0], tex.v[1], tex.v[2], tex, 1);
    gpu_render_triangle(gpu, tex.v[1], tex.v[2], tex.v[3], tex, 1);

    /* Off the shared diagonal, so the fill rule cannot make this ambiguous. */
    const int px = 110, py = 112;
    const uint16_t after_texture = gpu->vram[px + (py * 1024)];

    if (after_texture == 0) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=probe-not-covered\n", name);
        psx_gpu_destroy(gpu);
        return 1;
    }

    const int mask_written = (after_texture & 0x8000) != 0;

    if (mask_written != expect_masked) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=mask-bit-not-taken-from-texel "
                "texel=%04x vram=%04x expected bit15=%d\n",
                name, texel, after_texture, expect_masked);
        psx_gpu_destroy(gpu);
        return 1;
    }

    /* Step 3: the fog quad. Untextured, semi-transparent, "check mask before draw" on. */
    gpu->gpustat |= 0x1000u;

    poly_data_t fog = {.attrib = PA_QUAD | PA_TRANSP | PA_SHADED};

    for (int i = 0; i < 4; i++) {
        fog.v[i] = (vertex_t){.x = (int16_t)((i & 1) ? 150 : 90),
                              .y = (int16_t)((i & 2) ? 150 : 90),
                              .c = 0xf0f0f0};
    }

    gpu_render_triangle(gpu, fog.v[0], fog.v[1], fog.v[2], fog, 1);
    gpu_render_triangle(gpu, fog.v[1], fog.v[2], fog.v[3], fog, 1);

    const uint16_t after_fog = gpu->vram[px + (py * 1024)];
    const int fogged = (after_fog != after_texture);

    psx_gpu_destroy(gpu);

    /* Masked pixels must survive the fog quad untouched; unmasked ones must not. */
    if (fogged == expect_masked) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=%s texel=%04x before=%04x after=%04x\n",
                name,
                expect_masked ? "fog-quad-overwrote-masked-pixel"
                              : "fog-quad-skipped-unmasked-pixel",
                texel, after_texture, after_fog);
        return 1;
    }

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

/* ------------------------------------------------------------------------------------
   The mask stage as a CONTRACT, over its whole input matrix.

   PSX_GPU_MASK_WRITE / PSX_GPU_MASK_SKIP (psx/dev/gpu.h) are not a tidier way to spell what
   gpu.c already does — they are the text the GLES rasterizer's shader is COMPILED FROM
   (frontend/gpu_hw_gl.c stringifies them into GLSL, which accepts `||` and `&&` unchanged).
   A fourth hand-written copy of the rule, in a second language, is exactly how §0.5.12,
   §0.5.13, §0.5.15 and the nearest-filter bug all happened: every rasterizer wrong the same
   way, so every comparison between them passed.

   This pins the two expressions to psx/dev/gpu.c's OBSERVED output over all sixteen
   combinations of (set-mask, check-mask, texel STP, destination bit 15), in both accuracy
   states. It is deliberately not tautological: the prediction comes from the macros, the
   observation comes from rendering.

   The GL side of the same contract is measured on device — gl_mask_selftest() at attach and
   the `maskbit` bucket in the 1x parity gate.
   ------------------------------------------------------------------------------------ */
static int check_mask_contract(const char* name, uint32_t accuracy) {
    psx_gpu_t* gpu = psx_gpu_create();
    int failed = 0;
    int row;

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_gpu_init(gpu, NULL);
    psx_gpu_set_accuracy_flags(gpu, accuracy);

    for (row = 0; row < 16; ++row) {
        const int force     = (row >> 0) & 1;
        const int check     = (row >> 1) & 1;
        const int texel_stp = (row >> 2) & 1;
        const int dst_bit15 = (row >> 3) & 1;

        /* 15bpp page at (512,0), one uniform texel so every tap agrees. Non-RAW, modulated
           by 0x80 — the identity modulator — so the drawn colour is the texel's low 15 bits
           and the ONLY thing that can set bit 15 in the result is the mask stage. */
        const uint16_t texel = (uint16_t)(0x0421u | (texel_stp ? 0x8000u : 0x0000u));
        const uint16_t primed = (uint16_t)(dst_bit15 ? 0x8000u : 0x0000u);
        const int px = 110, py = 112;

        uint16_t got;
        int expect_skip, expect_mask;
        int x, y;

        gpu->draw_x1 = 0;
        gpu->draw_y1 = 0;
        gpu->draw_x2 = PSX_GPU_FB_WIDTH - 1;
        gpu->draw_y2 = PSX_GPU_FB_HEIGHT - 1;
        gpu->off_x = 0;
        gpu->off_y = 0;

        memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);

        for (y = 0; y < 16; y++)
            for (x = 512; x < 528; x++)
                gpu->vram[x + (y * 1024)] = texel;

        gpu->vram[px + (py * 1024)] = primed;

        /* Opaque, dither off, GP0(E6) as this row asks. */
        gpu->gpustat = (gpu->gpustat & ~0x1a60u) |
                       (force ? 0x0800u : 0u) | (check ? 0x1000u : 0u);

        {
            poly_data_t tex = {.attrib = PA_TEXTURED | PA_QUAD};

            tex.texp = TEXP_AT_512(2, 0);
            tex.clut = CLUT_AT_0_400;
            tex.v[0] = (vertex_t){.x = 100, .y = 100, .c = 0x808080, .tx = 8, .ty = 8};
            tex.v[1] = (vertex_t){.x = 140, .y = 100, .c = 0x808080, .tx = 8, .ty = 8};
            tex.v[2] = (vertex_t){.x = 100, .y = 140, .c = 0x808080, .tx = 8, .ty = 8};
            tex.v[3] = (vertex_t){.x = 140, .y = 140, .c = 0x808080, .tx = 8, .ty = 8};
            gpu_render_triangle(gpu, tex.v[0], tex.v[1], tex.v[2], tex, 1);
            gpu_render_triangle(gpu, tex.v[1], tex.v[2], tex.v[3], tex, 1);
        }

        got = gpu->vram[px + (py * 1024)];

        /* THE PREDICTION, from the shared macros and the shared helpers — nothing else. */
        expect_skip = PSX_GPU_MASK_SKIP(psx_gpu_mask_check(gpu), dst_bit15);
        expect_mask = PSX_GPU_MASK_WRITE(psx_gpu_mask_set(gpu),
                                         psx_gpu_mask_from_texel(gpu) != 0, texel_stp);

        if (expect_skip) {
            if (got != primed) {
                fprintf(stderr,
                        "GPU_PARITY failed case=%s reason=masked-pixel-was-written "
                        "force=%d check=%d stp=%d dst15=%d primed=%04x got=%04x\n",
                        name, force, check, texel_stp, dst_bit15, primed, got);
                failed = 1;
            }

            continue;
        }

        if ((got & 0x7fffu) != 0x0421u) {
            fprintf(stderr,
                    "GPU_PARITY failed case=%s reason=write-did-not-land "
                    "force=%d check=%d stp=%d dst15=%d got=%04x want colour 0421\n",
                    name, force, check, texel_stp, dst_bit15, got);
            failed = 1;
            continue;
        }

        if ((((got & 0x8000u) != 0u) ? 1 : 0) != (expect_mask ? 1 : 0)) {
            fprintf(stderr,
                    "GPU_PARITY failed case=%s reason=mask-bit-disagrees-with-contract "
                    "force=%d check=%d stp=%d dst15=%d got=%04x contract says bit15=%d\n",
                    name, force, check, texel_stp, dst_bit15, got, expect_mask ? 1 : 0);
            failed = 1;
        }
    }

    psx_gpu_destroy(gpu);

    if (failed)
        return 1;

    printf("GPU_PARITY passed case=%s (16 rows)\n", name);
    return 0;
}

static int run_mask_contract_case(void) {
    int failed = 0;

    /* Flag on: the rule hardware implements, and the one the GL shader compiles. */
    failed |= check_mask_contract("mask-contract-accurate", PSX_GPU_ACCURACY_MASK_BIT);

    /* Flag off: both expressions must collapse to "never skip, never set", because
       psx_gpu_mask_check/set/from_texel all return 0 there. Same macros, same test — this is
       what stops the contract from being written to fit only the accurate path. */
    failed |= check_mask_contract("mask-contract-inert-by-default", 0);

    return failed;
}

static int run_mask_from_texel_case(void) {
    const uint32_t on = PSX_GPU_ACCURACY_MASK_BIT;
    int failed = 0;

    /* STP=1 texel: bit 15 must be written, and the fog quad must be masked out. */
    failed |= check_mask_from_texel("mask-from-texel-stp-set", on, 0x8421, 1);

    /* STP=0 texel: bit 15 stays clear and the fog quad draws. Without this control the
       case above would also pass if bit 15 were simply forced on for every write. */
    failed |= check_mask_from_texel("mask-from-texel-stp-clear", on, 0x0421, 0);

    /* Accuracy flag off: the texel half must be inert, like the rest of the mask bit. */
    failed |= check_mask_from_texel("mask-from-texel-inert-by-default", 0, 0x8421, 0);

    return failed;
}

/* ------------------------------------------------------------------------------------
   Hardware's polygon size cull, asserted behaviourally.

   psx-spx: the maximum distance between two vertices is 1023 horizontally and 511
   vertically, and polygons exceeding that are NOT rendered. This core rejected at
   2048x1024 in all three rasterizers, so a triangle whose projected vertex the GTE
   saturated to +1023 was drawn as a wedge stretching to the screen edge instead of being
   dropped. Comparative cases cannot catch it: all three rasterizers were over-permissive
   the same way, which is why GPU_PARITY passed throughout.

   The geometry below is the Silent Hill shape from gpu_prim_dump_capture.txt #00876:
   two vertices at sane screen positions and a third at the saturation value.
   ------------------------------------------------------------------------------------ */
static int check_prim_size(const char* name, uint32_t accuracy, int16_t apex_y,
                           int expect_drawn) {
    psx_gpu_t* gpu = psx_gpu_create();

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_gpu_init(gpu, NULL);
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = PSX_GPU_FB_WIDTH - 1;
    gpu->draw_y2 = PSX_GPU_FB_HEIGHT - 1;
    gpu->off_x = 0;
    gpu->off_y = 0;
    psx_gpu_set_accuracy_flags(gpu, accuracy);

    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);

    /* Opaque, untextured, flat. Nothing here depends on blending or the mask bit. */
    poly_data_t tri = {.attrib = 0};
    tri.v[0] = (vertex_t){.x = 60,  .y = 100, .c = 0xf0f0f0};
    tri.v[1] = (vertex_t){.x = 200, .y = 100, .c = 0xf0f0f0};
    tri.v[2] = (vertex_t){.x = 130, .y = apex_y, .c = 0xf0f0f0};

    gpu_render_triangle(gpu, tri.v[0], tri.v[1], tri.v[2], tri, 1);

    /* Just inside the triangle a few rows below the flat top edge: covered whenever the
       primitive is rasterized at all, at either apex height. */
    const uint16_t probe = gpu->vram[130 + (110 * 1024)];
    const int drawn = (probe != 0);

    psx_gpu_destroy(gpu);

    if (drawn != expect_drawn) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=%s yspan=%d vram=%04x\n",
                name,
                expect_drawn ? "in-range-primitive-was-culled"
                             : "oversize-primitive-was-rasterized",
                (int)apex_y - 100, probe);
        return 1;
    }

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

static int run_prim_size_case(void) {
    const uint32_t on = PSX_GPU_ACCURACY_PRIM_SIZE;
    int failed = 0;

    /* yspan 923 > 511: hardware drops it, so with the flag on nothing may be written. */
    failed |= check_prim_size("prim-size-oversize-culled", on, 1023, 0);

    /* yspan 400 <= 511: the control. Without it the case above would also pass if the
       rasterizer had simply stopped drawing anything. */
    failed |= check_prim_size("prim-size-in-range-drawn", on, 500, 1);

    /* Flag off restores the historical 2048x1024, i.e. the same oversize triangle draws.
       This is what makes the setting a usable A/B rather than a one-way change. */
    failed |= check_prim_size("prim-size-inert-by-default", 0, 1023, 1);

    return failed;
}

/* ------------------------------------------------------------------------------------
   Texture modulation must TRUNCATE, asserted through a frame-feedback loop.

   Hardware: `(tex5 << 3) * mod8 >> 7`, integer, truncating (psx-spx). This core rounded.
   The difference is one level on 4.9% of inputs and nothing on the identity modulator, so
   no single-primitive comparison can see it -- and all three rasterizers rounded the same
   way, so the corpus cases could not either.

   A feedback loop makes it a permanent artifact. Silent Hill's loading screen blits the
   previous frame back over itself every frame through this multiply, alternating an
   identity pass (mod 0x80) with a decay pass (mod 0x7f); that is how the running
   character's motion blur fades. Rounding turns `round(t5 * 7.9375)` back into `t5 * 8`
   for every t5 <= 8, so levels 1..8 are fixed points: the trail decays to 8/31 and stays
   there forever. This case runs that exact loop.
   ------------------------------------------------------------------------------------ */
static int check_tex_modulate_decay(const char* name, uint32_t accuracy, int expect_black) {
    psx_gpu_t* gpu = psx_gpu_create();

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_gpu_init(gpu, NULL);
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = PSX_GPU_FB_WIDTH - 1;
    gpu->draw_y2 = PSX_GPU_FB_HEIGHT - 1;
    gpu->off_x = 0;
    gpu->off_y = 0;
    psx_gpu_set_accuracy_flags(gpu, accuracy);
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);

    /* 15bpp-direct page at (512,0), exactly like the game's framebuffer-as-texture blit. */
    gpu->texp_x = 512;
    gpu->texp_y = 0;
    gpu->texp_d = 2;
    gpu->gpustat &= ~0x1a60u;

    const int dst_x = 300, dst_y = 300;
    int level = 31;          /* full white trail */
    int iterations = 0;

    for (int frame = 0; frame < 400 && level > 0; frame++) {
        const uint16_t src = (uint16_t)(level | (level << 5) | (level << 10));

        gpu->vram[512] = src;                                /* the texel we sample */
        gpu->vram[dst_x + (dst_y * 1024)] = 0;               /* the pixel we blit into */

        rect_data_t r = {0};
        r.attrib = RA_TEXTURED;                              /* opaque, blended, variable */
        r.width = 1;
        r.height = 1;
        r.clut = 0;
        /* Alternating identity / decay pass, as captured from the game. */
        r.v0 = (vertex_t){.x = (int16_t)dst_x, .y = (int16_t)dst_y,
                          .c = (frame & 1) ? 0x808080u : 0x7f7f7fu, .tx = 0, .ty = 0};

        gpu_render_rect(gpu, r);

        level = gpu->vram[dst_x + (dst_y * 1024)] & 0x1f;
        iterations = frame + 1;
    }

    psx_gpu_destroy(gpu);

    const int reached_black = (level == 0);

    if (reached_black != expect_black) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=%s level=%d after %d frames\n",
                name,
                expect_black ? "feedback-trail-never-fades"
                             : "feedback-trail-faded-with-the-flag-off",
                level, iterations);
        return 1;
    }

    printf("GPU_PARITY passed case=%s (level=%d after %d frames)\n", name, level, iterations);
    return 0;
}

static int run_tex_modulate_case(void) {
    const uint32_t on = PSX_GPU_ACCURACY_TEX_MODULATE;
    int failed = 0;

    /* With truncation the trail must reach black, as it does on hardware. */
    failed |= check_tex_modulate_decay("tex-modulate-trail-fades", on, 1);

    /* Flag off restores rounding, and the trail sticks forever. This is the control that
       proves the case above is actually measuring the rounding and not something else. */
    failed |= check_tex_modulate_decay("tex-modulate-inert-by-default", 0, 0);

    return failed;
}

/* ------------------------------------------------------------------------------------
   GP0 argument intake must not run off the end of gpu->buf[].

   A polyline (GP0 bit 27) is the one command with no argument count: gpu_line() parks
   cmd_args_remaining at -1 and only the game's 0x50005000 terminator ends it, so
   psx_gpu_write32()'s RECV_ARGS branch takes every following word. Unbounded, a polyline
   longer than the 16-slot buffer -- or a desynced stream where the terminator never
   arrives -- walked buf[] straight into the struct members that follow it: recv_data,
   then buf_index, at which point the command stream owns the index and the writes go
   wherever it says, and on through the command counters into the drawing area and the
   drawing offset.

   The drawing offset and drawing area are the assertion because they sit at the far end
   of that walk (buf slots 78-83 on a 64-bit build). If they still hold what this test set,
   nothing between buf[16] and them was reached either.
   ------------------------------------------------------------------------------------ */
#define OVERRUN_OFF_X   (-77)
#define OVERRUN_OFF_Y   (123)
#define OVERRUN_DRAW_X1 (5u)
#define OVERRUN_DRAW_Y1 (9u)
#define OVERRUN_DRAW_X2 (601u)
#define OVERRUN_DRAW_Y2 (411u)

/* GP0(48): monochrome opaque polyline. Bit 27 (0x08 of the command byte) is what makes it
   a polyline; bit 28 (shaded) is clear. */
#define OVERRUN_POLYLINE_CMD 0x48808080u

/* Slot in gpu->buf[] a member of psx_gpu_t occupies once the write walks past the end of
   the buffer. Derived rather than hardcoded so the aimed case below still points at the
   drawing area if the struct is reordered. */
#define OVERRUN_SLOT(field) \
    ((uint32_t)((offsetof(psx_gpu_t, field) - offsetof(psx_gpu_t, buf)) / sizeof(uint32_t)))

static psx_gpu_t* make_overrun_gpu(void) {
    psx_gpu_t* gpu = psx_gpu_create();

    if (!gpu)
        return NULL;

    psx_gpu_init(gpu, NULL);

    /* Distinctive and non-zero, so a stray word landing on any of them shows up. */
    gpu->off_x = OVERRUN_OFF_X;
    gpu->off_y = OVERRUN_OFF_Y;
    gpu->draw_x1 = OVERRUN_DRAW_X1;
    gpu->draw_y1 = OVERRUN_DRAW_Y1;
    gpu->draw_x2 = OVERRUN_DRAW_X2;
    gpu->draw_y2 = OVERRUN_DRAW_Y2;

    return gpu;
}

static int check_overrun_state(const char* name, psx_gpu_t* gpu) {
    const int buf_slots = (int)(sizeof(gpu->buf) / sizeof(gpu->buf[0]));

    if (gpu->off_x != OVERRUN_OFF_X || gpu->off_y != OVERRUN_OFF_Y) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=drawing-offset-clobbered offset=(%d,%d)\n",
                name, gpu->off_x, gpu->off_y);
        return 1;
    }

    if (gpu->draw_x1 != OVERRUN_DRAW_X1 || gpu->draw_y1 != OVERRUN_DRAW_Y1 ||
        gpu->draw_x2 != OVERRUN_DRAW_X2 || gpu->draw_y2 != OVERRUN_DRAW_Y2) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=drawing-area-clobbered area=(%u,%u)-(%u,%u)\n",
                name, gpu->draw_x1, gpu->draw_y1, gpu->draw_x2, gpu->draw_y2);
        return 1;
    }

    /* The first thing the overrun reaches, and the one that turns a bounded overflow into
       an arbitrary write: once a stream word lands on buf_index, every later word goes
       wherever that word said. */
    if (gpu->buf_index < 0 || gpu->buf_index > buf_slots) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=buf-index-out-of-range index=%d slots=%d\n",
                name, gpu->buf_index, buf_slots);
        return 1;
    }

    return 0;
}

/* The realistic shape: a stream that opens a polyline and never terminates it. */
static int check_polyline_unterminated(void) {
    const char* name = "polyline-unterminated-overrun";
    psx_gpu_t* gpu = make_overrun_gpu();
    int failed;
    int i;

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_gpu_write32(gpu, 0x00, OVERRUN_POLYLINE_CMD);

    /* Far more words than buf[] holds. Every one reads as a plausible vertex (x=20, y=0)
       and none matches the 0x50005000 terminator, so the command never ends on its own. */
    for (i = 0; i < 4096; i++)
        psx_gpu_write32(gpu, 0x00, 0x00000014u);

    failed = check_overrun_state(name, gpu);
    psx_gpu_destroy(gpu);

    if (!failed)
        printf("GPU_PARITY passed case=%s\n", name);

    return failed;
}

/* The same overrun, aimed. The case above proves the buffer holds; this one proves the
   drawing state is genuinely out of reach, by sending the stream that would otherwise land
   on it: 16 words to fill buf[] and reach recv_data, one word onto buf_index carrying the
   drawing area's slot, then six words that would be draw_x1..off_y. Without the clamp the
   seventeenth word redirects the index and the last six overwrite the drawing state; with
   it, none of them leave buf[]. */
static int check_polyline_aimed_at_draw_state(void) {
    const char* name = "polyline-unterminated-aimed-at-draw-state";
    psx_gpu_t* gpu = make_overrun_gpu();
    int failed;
    int i;

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_gpu_write32(gpu, 0x00, OVERRUN_POLYLINE_CMD);

    /* buf[1..16]: the rest of the buffer, then recv_data. */
    for (i = 0; i < 16; i++)
        psx_gpu_write32(gpu, 0x00, 0x00000014u);

    /* buf[17] is buf_index itself. */
    psx_gpu_write32(gpu, 0x00, OVERRUN_SLOT(draw_x1));

    /* Six words for the six slots the redirected index now points at. Deliberately not
       more: two slots past off_y is an owned pointer that psx_gpu_destroy() frees. */
    for (i = 0; i < 6; i++)
        psx_gpu_write32(gpu, 0x00, 0x00000014u);

    failed = check_overrun_state(name, gpu);
    psx_gpu_destroy(gpu);

    if (!failed)
        printf("GPU_PARITY passed case=%s\n", name);

    return failed;
}

/* The control, and the reason the bound SATURATES instead of dropping the word. gpu_line()
   looks for the terminator in the last slot written, so a clamp that stopped storing once
   buf[] filled would leave a long-but-perfectly-legal polyline stuck in RECV_ARGS until the
   next GP1 reset -- trading an overrun for a hang. Parking overflow words in the last slot
   keeps the terminator visible, and this case is what says so. */
static int check_polyline_long_still_terminates(void) {
    const char* name = "polyline-long-still-terminates";
    psx_gpu_t* gpu = make_overrun_gpu();
    int failed;
    int i;

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_gpu_write32(gpu, 0x00, OVERRUN_POLYLINE_CMD);

    /* Comfortably past the 16 slots, but properly terminated -- a 40-vertex polyline is
       nothing unusual for a wireframe or a starfield. */
    for (i = 0; i < 40; i++)
        psx_gpu_write32(gpu, 0x00, 0x00000014u);

    psx_gpu_write32(gpu, 0x00, 0x55555555u);

    failed = check_overrun_state(name, gpu);

    if (!failed && gpu->state != GPU_STATE_RECV_CMD) {
        fprintf(stderr,
                "GPU_PARITY failed case=%s reason=terminator-missed state=%u\n",
                name, gpu->state);
        failed = 1;
    }

    psx_gpu_destroy(gpu);

    if (!failed)
        printf("GPU_PARITY passed case=%s\n", name);

    return failed;
}

static int run_polyline_overrun_case(void) {
    int failed = 0;

    failed |= check_polyline_unterminated();
    failed |= check_polyline_aimed_at_draw_state();
    failed |= check_polyline_long_still_terminates();

    return failed;
}

/* Axis-aligned corpus: sprites, fills, copies and uploads only. Every one of these lands
   on integer native pixel boundaries, so at scale N each native pixel MUST come out as a
   uniform NxN block. Polygons and lines are excluded because sub-pixel variation inside a
   block is exactly what upscaling is supposed to produce for them. */
static void draw_axis_aligned_corpus(psx_gpu_t* gpu) {
    gpu->draw_x1 = 64;
    gpu->draw_y1 = 48;
    gpu->draw_x2 = 320;
    gpu->draw_y2 = 240;

    /* Straddles all four drawing-area edges, so a scissor that is off by a single
       render-target column leaves a partially-filled block behind. */
    const int probe[][2] = { {50, 40}, {310, 40}, {50, 232}, {310, 232}, {100, 100} };

    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); ++i) {
        rect_data_t rect = {0};
        rect.attrib = 0;
        rect.v0 = (vertex_t){.x = (int16_t)probe[i][0], .y = (int16_t)probe[i][1], .c = 0x60c0a0};
        rect.width = 24;
        rect.height = 20;
        emit_rect(gpu, &rect);
    }

    /* A 1x1 sprite has to become a solid NxN block (HW_RENDERER_DESIGN.md §3.4). */
    rect_data_t tiny = {0};
    tiny.attrib = (uint8_t)(RS_1X1 << 3);
    tiny.v0 = (vertex_t){.x = 200, .y = 120, .c = 0xf0308f};
    emit_rect(gpu, &tiny);

    /* Textured sprites point-sample, so each native pixel must still be one texel across
       its whole NxN block. This is what fails if the texel counter advances per
       render-target pixel instead of per native pixel. */
    for (int depth = 0; depth < 3; ++depth) {
        latch_texpage(gpu, depth);

        rect_data_t tex_rect = {0};
        tex_rect.attrib = RA_TEXTURED;
        tex_rect.clut = CLUT_AT_0_400;
        tex_rect.v0 = (vertex_t){.x = (int16_t)(120 + depth * 44), .y = 160, .c = 0x808080, .tx = 5, .ty = 9};
        tex_rect.width = 26;
        tex_rect.height = 22;
        emit_rect(gpu, &tex_rect);
    }

    if (gpu->backend && gpu->backend->fill_vram)
        gpu->backend->fill_vram(gpu->backend, 512, 300, 64, 32, 0x1f00);

    for (int y = 300; y < 332; y++)
        for (int x = 512; x < 576; x++)
            gpu->vram[x + (y * 1024)] = 0x1f00;

    if (gpu->backend && gpu->backend->copy_vram)
        gpu->backend->copy_vram(gpu->backend, 512, 300, 600, 360, 64, 32);

    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 64; x++)
            gpu->vram[(600 + x) + ((360 + y) * 1024)] = gpu->vram[(512 + x) + ((300 + y) * 1024)];

    /* Kept clear of the texture page at (512,0)-(767,255) so the sprites above sample
       stable data. */
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++)
            gpu->vram[(700 + x) + ((420 + y) * 1024)] = (uint16_t)(0x0421 * (x + y));

    if (gpu->backend && gpu->backend->upload_vram)
        gpu->backend->upload_vram(gpu->backend, 700, 420, 16, 16, gpu->vram, 1024);
}

static int run_block_uniformity_case(const char* name, int scale) {
    psx_gpu_t* gpu = make_gpu();

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    seed_vram(gpu);

    psx_gpu_backend_t* be = armsx_hw_rt_create(gpu, scale);

    if (!be) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=backend-create\n", name);
        psx_gpu_destroy(gpu);
        return 1;
    }

    psx_gpu_set_backend(gpu, be);
    draw_axis_aligned_corpus(gpu);

    uint32_t stride_bytes = 0;
    const uint16_t* base = (const uint16_t*)be->display_buffer(be, 0, 0, &stride_bytes);
    const size_t stride_px = stride_bytes / sizeof(uint16_t);

    int failed = 0;

    for (int y = 0; y < PSX_GPU_FB_HEIGHT && !failed; y++) {
        for (int x = 0; x < PSX_GPU_FB_WIDTH && !failed; x++) {
            const uint16_t want = gpu->vram[x + (y * 1024)];

            for (int by = 0; by < scale && !failed; by++) {
                for (int bx = 0; bx < scale; bx++) {
                    const uint16_t got = base[((size_t)x * scale + bx) +
                                              (((size_t)y * scale + by) * stride_px)];

                    if (got != want) {
                        fprintf(stderr,
                                "GPU_PARITY failed case=%s reason=block-not-uniform at native (%d,%d) "
                                "sub (%d,%d) got=%04x want=%04x\n",
                                name, x, y, bx, by, got, want);
                        failed = 1;
                        break;
                    }
                }
            }
        }
    }

    psx_gpu_set_backend(gpu, NULL);
    armsx_hw_rt_destroy(be);
    psx_gpu_destroy(gpu);

    if (failed)
        return 1;

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

/* ------------------------------------------------------------------------------------
   GP0 COMMAND INTAKE — sprites must survive the trip from a command word to a pixel.

   WHY THIS EXISTS, AND WHY 23 GREEN CASES DID NOT CATCH IT

   Every other case in this file calls gpu_render_triangle() / gpu_render_rect() DIRECTLY.
   That is a rasterizer test, and it is structurally blind to the half of the GPU that
   decides whether the rasterizer is ever called: psx_gpu_write32()'s state machine, the
   per-command argument count in gpu_rect(), the type dispatch in psx_gpu_update_cmd(),
   and whatever else sits between a GP0 word and a primitive.

   The failure that motivated this gate was "the PS1 BIOS draws its logo but not the
   SONY COMPUTER ENTERTAINMENT text under it", with the performance overlay reporting
   `0 rect` on a frame that reported non-zero triangles. Text is textured sprites. A
   rect that is never SUBMITTED and a rect that is submitted and rasterized wrongly look
   nothing alike from inside gpu.c, and only one of them moves that counter — so the
   counter is asserted here too, as the machine-readable form of "the primitive happened".

   WHAT IS PINNED, AND AGAINST WHAT

   Not against the other rasterizer. §0.5.12, §0.5.13 and §0.5.15 were each one formula
   written identically wrong in all three rasterizers, and comparative gates passed
   through every one of them. Expected VRAM here is computed from the CONTRACT:

     * a RAW sprite writes the texel unmodified, so the expectation is the texel this
       test itself placed in the texture page — no emulator code participates in it;
     * a modulated sprite at the identity modulator 0x808080 also writes the texel
       unchanged (tex5*8 * 0x80 >> 7 == tex5*8), so the same literal expectation holds
       across the blend path without re-deriving the blend;
     * an untextured rect writes BGR555 of its own command colour.

   The 4-bit CLUT case is the one PS1 text actually uses, and it is the only case that
   exercises the CLUT indirection through the intake.
   ------------------------------------------------------------------------------------ */

/* Texture page at VRAM (512, 0); CLUT for the 4-bit case at (0, 400). Same layout the
   backend cases use, so a reader only has to learn it once. */
#define GP0_TEX_PAGE_X 512
#define GP0_TEX_PAGE_Y 0
#define GP0_CLUT_X     0
#define GP0_CLUT_Y     400

/* A deterministic, always-non-zero BGR555 texel. Zero is special-cased by the rasterizer
   as "fully transparent, skip the pixel", so a texture of zeroes would let a completely
   dead rect path pass by writing nothing and being asked for nothing. */
static uint16_t gp0_texel_at(int x, int y) {
    return (uint16_t)(0x0421u * (unsigned)(((x * 7 + y * 13) % 30) + 1));
}

static void gp0_seed_texture(psx_gpu_t* gpu) {
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++)
            gpu->vram[(GP0_TEX_PAGE_X + x) + ((GP0_TEX_PAGE_Y + y) * 1024)] = gp0_texel_at(x, y);
}

/* 16 CLUT entries, and a 4-bit page whose every nibble is a known index. */
static void gp0_seed_clut4(psx_gpu_t* gpu) {
    for (int i = 0; i < 16; i++)
        gpu->vram[(GP0_CLUT_X + i) + (GP0_CLUT_Y * 1024)] = (uint16_t)(0x0421u * (unsigned)(i + 1));

    /* Halfword h of row y holds indices for texels 4h..4h+3. Index = (tx + ty) & 0xf. */
    for (int y = 0; y < 256; y++) {
        for (int h = 0; h < 64; h++) {
            uint16_t word = 0;

            for (int n = 0; n < 4; n++) {
                const int tx = h * 4 + n;
                word |= (uint16_t)(((unsigned)((tx + y) & 0xf)) << (n * 4));
            }

            gpu->vram[(GP0_TEX_PAGE_X + h) + ((GP0_TEX_PAGE_Y + y) * 1024)] = word;
        }
    }
}

static uint16_t gp0_clut4_expected(psx_gpu_t* gpu, int tx, int ty) {
    return gpu->vram[(GP0_CLUT_X + ((tx + ty) & 0xf)) + (GP0_CLUT_Y * 1024)];
}

static void gp0(psx_gpu_t* gpu, uint32_t word) {
    psx_gpu_write32(gpu, 0x00, word);
}

/* GP0(E1): texpage x/64, y/256, blend mode, colour depth. */
static void gp0_texpage(psx_gpu_t* gpu, int depth) {
    gp0(gpu, 0xe1000000u | (uint32_t)(GP0_TEX_PAGE_X / 64) |
             ((uint32_t)(GP0_TEX_PAGE_Y / 256) << 4) | ((uint32_t)depth << 7));
}

static int gp0_check_pixels(const char* name, const char* what, psx_gpu_t* gpu,
                            int x0, int y0, int w, int h,
                            const uint16_t* expect) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            const uint16_t got = gpu->vram[(x0 + dx) + ((y0 + dy) * 1024)];
            const uint16_t want = expect[dy * w + dx];

            if (got != want) {
                fprintf(stderr,
                        "GPU_PARITY failed case=%s reason=%s at (%d,%d) got=%04x want=%04x\n",
                        name, what, x0 + dx, y0 + dy, got, want);
                return 1;
            }
        }
    }

    return 0;
}

/*
    Drives four sprite/rect forms through psx_gpu_write32() and checks that each one
    (a) leaves the command state machine ready for the next command, (b) moves the
    PSX_PERF_PRIM_RECT counter, and (c) writes the exact pixels the contract calls for.

    The final step submits a TRIANGLE after all of them. A rect whose argument count is
    wrong does not merely fail to draw — it eats the following command's words, so the
    triangle is the guard against a rect desynchronising the stream behind it, which is
    the failure mode that takes a whole frame down rather than one primitive.
*/
static int run_gp0_rect_intake_case(void) {
    const char* name = "gp0-rect-intake";
    static uint16_t expect[32 * 16];
    psx_gpu_t* gpu = make_gpu();
    psx_perf_counters_t counters;
    int failed = 0;

    if (!gpu) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=allocation\n", name);
        return 1;
    }

    psx_perf_set_enabled(1);
    psx_perf_take_frame(&counters);       /* zero the window */

    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    gp0_seed_texture(gpu);
    gp0_texpage(gpu, 2);                  /* 15-bit, so a texel is itself */

    if (gpu->texp_x != GP0_TEX_PAGE_X || gpu->texp_y != GP0_TEX_PAGE_Y || gpu->texp_d != 2) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=texpage-not-latched "
                        "got=(%u,%u,%u)\n", name, gpu->texp_x, gpu->texp_y, gpu->texp_d);
        psx_gpu_destroy(gpu);
        return 1;
    }

    /* ---- 1. GP0(65): RAW textured variable-size sprite, 32x16 at (20,10), UV (0,0).
       RAW means the texel is written unmodified, so the expectation is literally the
       texture this test wrote. ---- */
    for (int dy = 0; dy < 16; dy++)
        for (int dx = 0; dx < 32; dx++)
            expect[dy * 32 + dx] = gp0_texel_at(dx, dy);

    gp0(gpu, 0x65000000u);
    gp0(gpu, (10u << 16) | 20u);
    gp0(gpu, 0u);                          /* clut = 0, uv = (0,0) */
    gp0(gpu, (16u << 16) | 32u);

    if (gpu->state != GPU_STATE_RECV_CMD || gpu->cmd_args_remaining != 0) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=raw-sprite-left-command-open "
                        "state=%d remaining=%d\n",
                name, gpu->state, gpu->cmd_args_remaining);
        failed = 1;
    }

    failed |= gp0_check_pixels(name, "raw-sprite-pixels", gpu, 20, 10, 32, 16, expect);

    /* ---- 2. GP0(64) at the identity modulator: hardware's blend returns the texel
       unchanged at 0x80 per channel, so the SAME expectation must hold through the
       modulate path. A modulation formula that stops being the identity at 0x80 fails
       here, which is the §0.5.15 class of defect. ---- */
    gp0(gpu, 0x64808080u);
    gp0(gpu, (60u << 16) | 20u);
    gp0(gpu, 0u);
    gp0(gpu, (16u << 16) | 32u);

    failed |= gp0_check_pixels(name, "modulated-sprite-pixels", gpu, 20, 60, 32, 16, expect);

    /* ---- 3. GP0(60): untextured variable-size rect. Three words, not four — the arm of
       gpu_rect()'s argument count that a textured-only test never reaches. ---- */
    for (int i = 0; i < 8 * 8; i++)
        expect[i] = (uint16_t)(((0x7fu >> 3) << 10) | ((0x7fu >> 3) << 5) | (0x7fu >> 3));

    gp0(gpu, 0x607f7f7fu);
    gp0(gpu, (100u << 16) | 100u);
    gp0(gpu, (8u << 16) | 8u);

    if (gpu->state != GPU_STATE_RECV_CMD || gpu->cmd_args_remaining != 0) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=mono-rect-left-command-open "
                        "state=%d remaining=%d\n",
                name, gpu->state, gpu->cmd_args_remaining);
        failed = 1;
    }

    failed |= gp0_check_pixels(name, "mono-rect-pixels", gpu, 100, 100, 8, 8, expect);

    /* ---- 4. GP0(7D): RAW 8x8 FIXED-size sprite in 4-bit CLUT mode — what PS1 font glyphs
       actually are. Fixed size is a THREE-word command (no size word), so this is also the
       size-arm of the argument count, and it goes through the CLUT indirection. ---- */
    gp0_seed_clut4(gpu);
    gp0_texpage(gpu, 0);

    for (int dy = 0; dy < 8; dy++)
        for (int dx = 0; dx < 8; dx++)
            expect[dy * 8 + dx] = gp0_clut4_expected(gpu, 4 + dx, 4 + dy);

    gp0(gpu, 0x7d000000u);
    gp0(gpu, (200u << 16) | 200u);
    gp0(gpu, ((uint32_t)((GP0_CLUT_Y << 6) | (GP0_CLUT_X / 16)) << 16) | (4u << 8) | 4u);

    if (gpu->state != GPU_STATE_RECV_CMD || gpu->cmd_args_remaining != 0) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=fixed-sprite-left-command-open "
                        "state=%d remaining=%d\n",
                name, gpu->state, gpu->cmd_args_remaining);
        failed = 1;
    }

    failed |= gp0_check_pixels(name, "clut4-sprite-pixels", gpu, 200, 200, 8, 8, expect);

    /* ---- 5. The counter contract. Four rects went in; the overlay must be able to say so.
       `0 rect` on a frame that drew rects is how this regression was reported, and a
       counter that cannot be trusted turns the next report into a guess. ---- */
    psx_perf_take_frame(&counters);

    if (counters.gpu_primitives[PSX_PERF_PRIM_RECT] != 4) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=rect-counter got=%u want=4\n",
                name, counters.gpu_primitives[PSX_PERF_PRIM_RECT]);
        failed = 1;
    }

    /* ---- 6. Stream integrity: a triangle submitted AFTER the rects must still land. If a
       rect consumed the wrong number of words, this is what notices. ---- */
    gp0(gpu, 0x20ffffffu);
    gp0(gpu, (300u << 16) | 300u);
    gp0(gpu, (300u << 16) | 340u);
    gp0(gpu, (340u << 16) | 300u);

    psx_perf_take_frame(&counters);

    if (counters.gpu_primitives[PSX_PERF_PRIM_TRIANGLE] != 1) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=stream-desync-after-rects "
                        "triangles=%u want=1\n",
                name, counters.gpu_primitives[PSX_PERF_PRIM_TRIANGLE]);
        failed = 1;
    }

    if (!gpu->vram[305 + (305 * 1024)]) {
        fprintf(stderr, "GPU_PARITY failed case=%s reason=triangle-after-rects-not-drawn\n",
                name);
        failed = 1;
    }

    psx_perf_set_enabled(0);
    psx_gpu_destroy(gpu);

    if (failed)
        return 1;

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

/*
    MASK BIT vs TEXTURED CONTENT — "turning accurate_mask_bit on must not make textures
    disappear."

    WHY THIS EXISTS

    The reported symptom was that with accurate_mask_bit = true the PS1 BIOS drew its logo
    but not the SONY COMPUTER ENTERTAINMENT text under it, and Xenogears lost its menus,
    while untextured geometry kept drawing. Turning the setting off restored everything.

    The existing mask cases (mask-from-texel-*, mask-contract-*) pin the WRITE and SKIP rules
    over their input matrix, but they do it a pixel at a time against hand-built inputs. None
    of them draws a textured SPRITE and asks the blunt question a user would: did the thing
    appear? These do, and they do it over repeated draws, because the field report was of
    degradation across a session rather than a wrong first frame.

    THE INVARIANT

    The accuracy flag enables the mask rules; GP0(E6) decides whether the game is USING them.
    A game that never sends GP0(E6) must therefore render byte-identically whether the flag
    is on or off — measured directly against the real BIOS, which never sets those bits at
    all. And a sprite drawn into a clean destination must write every pixel it covers under
    every GP0(E6) setting, because "check mask" only skips where bit 15 is ALREADY set.
*/
static int run_mask_bit_textured_sprite_case(void) {
    const char* name = "mask-bit-textured-sprite";
    int failed = 0;

    /* ---- 1. Inert unless the game asks. Draw the same sprite with the flag off and on,
       with GP0(E6) never sent, and require identical VRAM. This is the property the field
       failure violated, and the one the real BIOS depends on. ---- */
    {
        psx_gpu_t* off = make_gpu();
        psx_gpu_t* on = make_gpu();

        psx_gpu_set_accuracy_flags(off, 0);
        psx_gpu_set_accuracy_flags(on, PSX_GPU_ACCURACY_MASK_BIT);

        for (psx_gpu_t** it = (psx_gpu_t*[]){off, on, NULL}; *it; it++) {
            memset((*it)->vram, 0, PSX_GPU_VRAM_SIZE);
            gp0_seed_texture(*it);
            gp0_texpage(*it, 2);

            /* Half the page carries STP, so the texel half of the write rule is exercised
               rather than sitting at zero. */
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++)
                    if ((x + y) & 1)
                        (*it)->vram[(GP0_TEX_PAGE_X + x) + (y * 1024)] |= 0x8000u;

            gp0(*it, 0x64808080u);
            gp0(*it, (40u << 16) | 40u);
            gp0(*it, 0u);
            gp0(*it, (16u << 16) | 32u);
        }

        /* Bits 0-14 only. Bit 15 is EXPECTED to differ: with the flag on, a textured write
           carries the texel's bit 15 into the destination, which is what hardware does and
           what §0.5.12's mask-from-texel exists to restore. Bit 15 is not displayed, so the
           picture must be identical even though the halfwords are not — and "the picture is
           identical" is precisely the claim the field failure disproved. */
        for (size_t i = 0; i < PSX_GPU_VRAM_SIZE / sizeof(uint16_t); i++) {
            if ((off->vram[i] & 0x7fff) != (on->vram[i] & 0x7fff)) {
                fprintf(stderr,
                        "GPU_PARITY failed case=%s reason=flag-changed-visible-colour "
                        "at vram[%zu] off=%04x on=%04x\n",
                        name, i, off->vram[i], on->vram[i]);
                failed = 1;

                break;
            }
        }

        psx_gpu_destroy(off);
        psx_gpu_destroy(on);
    }

    /* ---- 2. A sprite into a CLEAN destination must appear in full, for every GP0(E6).
       "Check mask" skips only where bit 15 is already set; a cleared buffer has none, so
       every setting must draw all 512 pixels. ---- */
    for (int e6 = 0; e6 < 4; e6++) {
        psx_gpu_t* gpu = make_gpu();
        int painted = 0;

        psx_gpu_set_accuracy_flags(gpu, PSX_GPU_ACCURACY_MASK_BIT);
        memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
        gp0_seed_texture(gpu);
        gp0_texpage(gpu, 2);

        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                if ((x + y) & 1)
                    gpu->vram[(GP0_TEX_PAGE_X + x) + (y * 1024)] |= 0x8000u;

        gp0(gpu, 0xe6000000u | (uint32_t)e6);

        gp0(gpu, 0x64808080u);
        gp0(gpu, (40u << 16) | 40u);
        gp0(gpu, 0u);
        gp0(gpu, (16u << 16) | 32u);

        for (int y = 40; y < 56; y++)
            for (int x = 40; x < 72; x++)
                if (gpu->vram[x + y * 1024])
                    painted++;

        if (painted != 32 * 16) {
            fprintf(stderr, "GPU_PARITY failed case=%s reason=sprite-vanished E6=%d "
                            "painted=%d/512\n", name, e6, painted);
            failed = 1;
        }

        psx_gpu_destroy(gpu);
    }

    /* ---- 2b. OVERDRAW without a clear, with the game asking for NOTHING (E6 = 0).
       This is the case that actually discriminates, and the first version of this test
       missed it: a sprite drawn into a CLEAN buffer draws in full even if the mask check is
       spuriously active, because a cleared buffer has no bit 15 anywhere to trip it. The
       failure only shows when a previous textured draw has already written bit 15 from its
       texels and a SECOND, different draw has to overwrite it.

       So: draw, then draw again with a different modulator, and require the second draw to
       win everywhere — checked against the same two draws with the flag off. With the flag
       on and no GP0(E6) sent, nothing may be skipped. ---- */
    {
        psx_gpu_t* off = make_gpu();
        psx_gpu_t* on = make_gpu();

        psx_gpu_set_accuracy_flags(off, 0);
        psx_gpu_set_accuracy_flags(on, PSX_GPU_ACCURACY_MASK_BIT);

        for (psx_gpu_t** it = (psx_gpu_t*[]){off, on, NULL}; *it; it++) {
            memset((*it)->vram, 0, PSX_GPU_VRAM_SIZE);
            gp0_seed_texture(*it);
            gp0_texpage(*it, 2);

            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++)
                    if ((x + y) & 1)
                        (*it)->vram[(GP0_TEX_PAGE_X + x) + (y * 1024)] |= 0x8000u;

            /* First draw lays down bit 15 from the texels. */
            gp0(*it, 0x64808080u);
            gp0(*it, (40u << 16) | 40u);
            gp0(*it, 0u);
            gp0(*it, (16u << 16) | 32u);

            /* Second draw, different modulator, same footprint. It must win everywhere. */
            gp0(*it, 0x64204080u);
            gp0(*it, (40u << 16) | 40u);
            gp0(*it, 0u);
            gp0(*it, (16u << 16) | 32u);
        }

        for (int y = 40; y < 56; y++) {
            for (int x = 40; x < 72; x++) {
                const size_t i = (size_t)x + (size_t)y * 1024u;

                if ((off->vram[i] & 0x7fff) != (on->vram[i] & 0x7fff)) {
                    fprintf(stderr,
                            "GPU_PARITY failed case=%s reason=overdraw-blocked-by-mask "
                            "at (%d,%d) off=%04x on=%04x\n",
                            name, x, y, off->vram[i], on->vram[i]);
                    failed = 1;
                    y = 56;

                    break;
                }
            }
        }

        psx_gpu_destroy(off);
        psx_gpu_destroy(on);
    }

    /* ---- 3. No accumulation. Redrawing a sprite over a destination that a previous draw
       already mask-marked must keep producing the same picture, as long as the buffer is
       cleared between frames the way a game clears it. A rule that degrades frame over frame
       is the shape of the reported failure, so it gets its own assertion. ---- */
    {
        psx_gpu_t* gpu = make_gpu();
        int first = -1;

        psx_gpu_set_accuracy_flags(gpu, PSX_GPU_ACCURACY_MASK_BIT);
        memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
        gp0_seed_texture(gpu);
        gp0_texpage(gpu, 2);

        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                if ((x + y) & 1)
                    gpu->vram[(GP0_TEX_PAGE_X + x) + (y * 1024)] |= 0x8000u;

        /* set + check, the combination §0.5.12's Silent Hill fog uses. */
        gp0(gpu, 0xe6000003u);

        for (int frame = 0; frame < 8; frame++) {
            int painted = 0;

            /* GP0(02) fill: how a game clears, and it ignores the mask bit on hardware.
               If this stops clearing bit 15 the buffer silently becomes unwritable, which
               is exactly the accumulation this case is here to catch. */
            gp0(gpu, 0x02000000u);
            gp0(gpu, (40u << 16) | 40u);
            gp0(gpu, (16u << 16) | 32u);

            gp0(gpu, 0x64808080u);
            gp0(gpu, (40u << 16) | 40u);
            gp0(gpu, 0u);
            gp0(gpu, (16u << 16) | 32u);

            for (int y = 40; y < 56; y++)
                for (int x = 40; x < 72; x++)
                    if (gpu->vram[x + y * 1024] & 0x7fff)
                        painted++;

            if (first < 0)
                first = painted;

            if (painted != first) {
                fprintf(stderr, "GPU_PARITY failed case=%s reason=degrades-over-frames "
                                "frame=%d painted=%d first=%d\n", name, frame, painted, first);
                failed = 1;

                break;
            }
        }

        if (first <= 0) {
            fprintf(stderr, "GPU_PARITY failed case=%s reason=nothing-drawn-at-all\n", name);
            failed = 1;
        }

        psx_gpu_destroy(gpu);
    }

    if (failed)
        return 1;

    printf("GPU_PARITY passed case=%s\n", name);
    return 0;
}

int main(void) {
    int failed = 0;
    poly_data_t flat = {.attrib = 0};
    flat.v[0].c = flat.v[1].c = flat.v[2].c = 0x40a0f0;
    failed |= run_case(
        "flat",
        (vertex_t){.x = 10, .y = 12, .c = 0x40a0f0},
        (vertex_t){.x = 92, .y = 24, .c = 0x40a0f0},
        (vertex_t){.x = 38, .y = 105, .c = 0x40a0f0},
        flat,
        0
    );

    poly_data_t shaded = {.attrib = PA_SHADED};
    failed |= run_case(
        "shaded-dithered",
        (vertex_t){.x = 120, .y = 80, .c = 0x0000ff},
        (vertex_t){.x = 220, .y = 100, .c = 0x00ff00},
        (vertex_t){.x = 160, .y = 210, .c = 0xff0000},
        shaded,
        0
    );

    poly_data_t transparent = {.attrib = PA_TRANSP};
    transparent.v[0].c = transparent.v[1].c = transparent.v[2].c = 0xffffff;
    failed |= run_case(
        "semi-transparent",
        (vertex_t){.x = 300, .y = 40, .c = 0xffffff},
        (vertex_t){.x = 410, .y = 70, .c = 0xffffff},
        (vertex_t){.x = 340, .y = 170, .c = 0xffffff},
        transparent,
        1
    );

    /* The internal-resolution backend. hw-1x is the correctness gate the whole upscaling
       feature rests on; the hw-2x/3x/4x cases prove the scaling is coherent with it.
       3x is included on purpose — an erroneous *0.5 term hides at even multipliers. */
    /* Non-negotiable: with the accuracy flags off, the new GP0(E6) latch and the dither
       gate must be completely inert, so the default software path behaves as it always has. */
    failed |= run_default_inertness_case("default-path-unchanged");

    /* GP0(E6) bit 0 == 0 writes the texel's bit 15, not 0. Behavioural, not comparative:
       both rasterizers used to get this wrong identically, so parity alone passed. */
    failed |= run_mask_from_texel_case();

    /* The same rule as a contract over its whole input matrix, because the GLES rasterizer's
       shader is compiled from PSX_GPU_MASK_WRITE / PSX_GPU_MASK_SKIP and a shader cannot be
       run here. Pins those two expressions to gpu.c's observed output. */
    failed |= run_mask_contract_case();

    /* Hardware drops polygons bigger than 1023x511. Behavioural for the same reason: all
       three rasterizers were over-permissive identically. */
    failed |= run_prim_size_case();

    /* Texture blending truncates on hardware. Behavioural via a feedback loop: the
       difference is one level and invisible on any single primitive. */
    failed |= run_tex_modulate_case();

    /* A polyline never ends on a count, only on a terminator the game has to send. The GP0
       intake must survive one that never arrives without writing past gpu->buf[]. */
    failed |= run_polyline_overrun_case();

    /* Sprites, driven through the GP0 intake rather than by calling the rasterizer. Every
       other case in this file starts at gpu_render_*(), so nothing here covered the path
       that decides whether a rect is submitted at all — which is what "the BIOS draws its
       logo but not its text, and the overlay says 0 rect" turned out to be about. */
    failed |= run_gp0_rect_intake_case();

    /* Turning accurate_mask_bit on must not make textured content disappear — the setting
       is inert until a game sends GP0(E6), and a sprite into a clean buffer draws in full
       under every E6. The mask cases above pin the rules per pixel; this asks whether the
       sprite actually shows up, and whether it keeps showing up frame after frame. */
    failed |= run_mask_bit_textured_sprite_case();

    failed |= run_backend_case("hw-1x-matches-software", 1);

    /* Same gate with the opt-in accuracy fixes on. This is what stops the mask bit or the
       dither gate being implemented in one rasterizer and not the other. */
    g_test_accuracy = PSX_GPU_ACCURACY_MASK_BIT | PSX_GPU_ACCURACY_DITHER_GATE |
                      PSX_GPU_ACCURACY_PRIM_SIZE | PSX_GPU_ACCURACY_TEX_MODULATE;
    failed |= run_backend_case("hw-1x-matches-software-accurate", 1);
    failed |= run_scale_case("hw-2x-scale-coherent-accurate", 2);
    g_test_accuracy = 0;
    failed |= run_scale_case("hw-2x-scale-coherent", 2);
    failed |= run_scale_case("hw-3x-scale-coherent", 3);
    failed |= run_scale_case("hw-4x-scale-coherent", 4);

    /* Every sub-pixel of an axis-aligned primitive, not just the block's top-left corner.
       This is what catches a half-pixel or off-by-one scissor error: the scale-coherence
       cases above only sample 1 render-target pixel in S^2 and would miss it. */
    failed |= run_block_uniformity_case("hw-2x-blocks-uniform", 2);
    failed |= run_block_uniformity_case("hw-3x-blocks-uniform", 3);
    failed |= run_block_uniformity_case("hw-4x-blocks-uniform", 4);

    if (failed)
        return 1;
    puts("GPU_PARITY all cases passed");
    return 0;
}
