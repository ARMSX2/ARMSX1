#include "gpu_hw_rt.h"

#ifdef USE_HARDWARE

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/log.h"

/*
    Everything below is a scale-parameterised clone of the three live rasterizers in
    psx/dev/gpu.c (gpu_render_triangle:251, gpu_render_rect:453, gpu_render_flat_line:669)
    plus the four VRAM transfer commands. Substituting S = 1 has to reduce each one to
    exactly the operations the software path performs, in the same order, on the same
    values — that is what makes 1x byte-identical rather than merely similar.

    The rules being applied (the backend):
      scales by S      vertex positions, drawing area, fill/copy rectangles, scanout origin
      does NOT scale   texture UVs, texpage/CLUT addressing, the texture window, the
                       dither matrix index (divided back down to native), and every read
                       of gpu->vram, which stays a native 1024x512 surface

    Sampling convention: a native coordinate n maps to render-target coordinate n*S and
    the edge functions are evaluated at integer render-target corners, exactly as the
    software path evaluates them at integer native corners. Combined with the half-open
    loop bounds this makes a primitive spanning native [a, b) cover exactly (b-a)*S
    columns, with no half-pixel term anywhere —  warns specifically against adding
    one, because it is invisible at S=1 and wrong at every other scale.
*/

#define BGR555(c) \
    (((c & 0x0000f8) >> 3) | \
     ((c & 0x00f800) >> 6) | \
     ((c & 0xf80000) >> 9))

#define CLAMP(v, d, u) ((v) <= (d)) ? (d) : (((v) >= (u)) ? (u) : (v))

/* gpu.c:9 — sign-extend an 11-bit coordinate. */
#define SE10(v) ((int16_t)((v) << 5) >> 5)

/* gpu.c:173, evaluated on plain ints instead of vertex_t so it can take scaled
   coordinates. Worst case at S=8 is 16384*16384, comfortably inside int32. */
#define RT_EDGE(ax, ay, bx, by, cx, cy) \
    (((bx) - (ax)) * ((cy) - (ay)) - ((by) - (ay)) * ((cx) - (ax)))

/* gpu.c:248 — the top-left fill rule. Scale-invariant: multiplying every coordinate by a
   positive S preserves each comparison, so this is applied to scaled coordinates. */
#define RT_TL(z, ax, ay, bx, by) \
    ((z < 0) || ((z == 0) && ((by > ay) || ((by == ay) && (bx < ax)))))

typedef struct {
    psx_gpu_backend_t base;

    int scale;
    uint16_t* rt;          /* 1024*S x 512*S, BGR555, same packing as gpu->vram */
    int rt_w, rt_h;        /* in render-target pixels */
} armsx_hw_rt_t;

static armsx_hw_rt_t* rt_self(psx_gpu_backend_t* be) {
    return (armsx_hw_rt_t*)be->impl;
}

static int rt_min3(int a, int b, int c) {
    int m = (a <= b) ? a : b;

    return (m <= c) ? m : c;
}

static int rt_max3(int a, int b, int c) {
    int m = (a > b) ? a : b;

    return (m > c) ? m : c;
}

/* ------------------------------------------------------------------------------------
   Polygons — mirrors gpu_render_triangle (gpu.c:251-449)
   ------------------------------------------------------------------------------------ */

static void rt_render_triangle(armsx_hw_rt_t* rt, psx_gpu_t* gpu,
                               vertex_t v0, vertex_t v1, vertex_t v2,
                               const poly_data_t* data) {
    const int s = rt->scale;

    /* Texture addressing is native throughout — none of this scales. */
    int tpx = (data->texp & 0xf) << 6;
    int tpy = (data->texp & 0x10) << 4;
    int clutx = (data->clut & 0x3f) << 4;
    int cluty = (data->clut >> 6) & 0x1ff;
    int depth = (data->texp >> 7) & 3;
    /* Per-PRIMITIVE default only. It is reset into a per-PIXEL `transp` inside the loop:
       declaring it once here and assigning the texel's bit 15 into it latched the first
       opaque texel for the rest of the primitive, which is the sprite-sized rectangle
       gpu.c:738-745 describes. gpu.c fixed it; this rasterizer had not. */
    const int transp_default = (data->attrib & PA_TRANSP) != 0;
    int transp_mode;

    if (data->attrib & PA_TEXTURED) {
        transp_mode = (data->texp >> 5) & 3;
    } else {
        transp_mode = (gpu->gpustat >> 5) & 3;
    }

    vertex_t a = v0, b, c;

    /* Winding fix on the unscaled vertices, exactly as gpu.c:271-277. */
    if (RT_EDGE(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y) < 0) {
        b = v2;
        c = v1;
    } else {
        b = v1;
        c = v2;
    }

    int ax = a.x + gpu->off_x, ay = a.y + gpu->off_y;
    int bx = b.x + gpu->off_x, by = b.y + gpu->off_y;
    int cx = c.x + gpu->off_x, cy = c.y + gpu->off_y;

    int xmin = rt_min3(ax, bx, cx);
    int ymin = rt_min3(ay, by, cy);
    int xmax = rt_max3(ax, bx, cx);
    int ymax = rt_max3(ay, by, cy);

    /* The core's own size rejection, kept so the two rasterizers agree on which primitives
       get dropped. psx_gpu_prim_oversize() is the single definition of the rule; it applies
       hardware's 1023x511 limit when PSX_GPU_ACCURACY_PRIM_SIZE is on and the historical
       (2x too permissive) 2048x1024 otherwise. */
    if (psx_gpu_prim_oversize(gpu, xmax - xmin, ymax - ymin))
        return;

    /* From here on everything is in render-target space. */
    int sax = ax * s, say = ay * s;
    int sbx = bx * s, sby = by * s;
    int scx = cx * s, scy = cy * s;

    float area = (float)RT_EDGE(sax, say, sbx, sby, scx, scy);

    /* Read from the same helpers the software path uses so the two cannot drift. */
    const int mask_check = psx_gpu_mask_check(gpu);
    const uint16_t mask_set = psx_gpu_mask_set(gpu) ? 0x8000 : 0x0000;
    const uint16_t mask_from_texel = psx_gpu_mask_from_texel(gpu);
    const int dither_on = psx_gpu_dither_enabled(gpu);

    /* Drawing area is a screen-space rectangle, so it scales; it is inclusive on both
       ends (gpu.c:298), which at scale covers S render-target pixels per native one. */
    int clip_x1 = (int)gpu->draw_x1 * s;
    int clip_y1 = (int)gpu->draw_y1 * s;
    int clip_x2 = (int)gpu->draw_x2 * s + (s - 1);
    int clip_y2 = (int)gpu->draw_y2 * s + (s - 1);

    /* Walking only the part of the bounding box the scissor can accept is exactly
       equivalent to testing per pixel — the body writes nothing outside it — but it keeps
       a primitive that is mostly off-screen from costing S^2 wasted iterations, which
       matters a great deal more here than it does at native resolution. */
    int y_begin = ymin * s, y_end = ymax * s;
    int x_begin = xmin * s, x_end = xmax * s;

    if (y_begin < clip_y1) y_begin = clip_y1;
    if (y_end > (clip_y2 + 1)) y_end = clip_y2 + 1;
    if (x_begin < clip_x1) x_begin = clip_x1;
    if (x_end > (clip_x2 + 1)) x_end = clip_x2 + 1;

    for (int y = y_begin; y < y_end; y++) {
        for (int x = x_begin; x < x_end; x++) {
            /* Both PER-PIXEL, and both for the same reason — see transp_default above and
               psx_gpu_mask_from_texel(). */
            int transp = transp_default;
            uint16_t stp = 0;

            if (mask_check && (rt->rt[x + (y * rt->rt_w)] & 0x8000))
                continue;

            float z0 = (float)RT_EDGE(sbx, sby, scx, scy, x, y);

            if (RT_TL(z0, sbx, sby, scx, scy))
                continue;

            float z1 = (float)RT_EDGE(scx, scy, sax, say, x, y);

            if (RT_TL(z1, scx, scy, sax, say))
                continue;

            float z2 = (float)RT_EDGE(sax, say, sbx, sby, x, y);

            if (RT_TL(z2, sax, say, sbx, sby))
                continue;

            uint16_t color = 0;
            uint32_t mod   = 0;

            if (data->attrib & PA_SHADED) {
                float cr = (z0 * ((a.c >>  0) & 0xff) + z1 * ((b.c >>  0) & 0xff) + z2 * ((c.c >>  0) & 0xff)) / area;
                float cg = (z0 * ((a.c >>  8) & 0xff) + z1 * ((b.c >>  8) & 0xff) + z2 * ((c.c >>  8) & 0xff)) / area;
                float cb = (z0 * ((a.c >> 16) & 0xff) + z1 * ((b.c >> 16) & 0xff) + z2 * ((c.c >> 16) & 0xff)) / area;

                /* Dither is indexed in NATIVE pixels (): dividing the render-target
                   coordinate back down makes each dither cell an SxS block instead of
                   high-frequency noise that gets worse as S grows. Both x and y are >= 0
                   here because the clip test above already rejected everything outside the
                   drawing area, so the truncating division is a floor.

                   The native coordinate is then used ABSOLUTELY, not relative to the
                   primitive's bounding box — see the matching comment in gpu.c. */
                int dy = (y / s) & 3;
                int dx = (x / s) & 3;

                int dither = dither_on ? g_psx_gpu_dither_kernel[dx + (dy * 4)] : 0;

                cr += dither;
                cg += dither;
                cb += dither;

                cr = (cr >= 255.0f) ? 255.0f : ((cr <= 0.0f) ? 0.0f : cr);
                cg = (cg >= 255.0f) ? 255.0f : ((cg <= 0.0f) ? 0.0f : cg);
                cb = (cb >= 255.0f) ? 255.0f : ((cb <= 0.0f) ? 0.0f : cb);

                unsigned int ucr = roundf(cr);
                unsigned int ucg = roundf(cg);
                unsigned int ucb = roundf(cb);

                mod = (ucb << 16) | (ucg << 8) | ucr;
            } else {
                mod = data->v[0].c;
            }

            if (data->attrib & PA_TEXTURED) {
                /* z/area is scale-invariant (both scale by S^2), so UVs stay native. */
                float tx = ((z0 * a.tx) + (z1 * b.tx) + (z2 * c.tx)) / area;
                float ty = ((z0 * a.ty) + (z1 * b.ty) + (z2 * c.ty)) / area;

                /* Sampled from gpu->vram, which is native and kept authoritative by the
                   software shadow. Polygons filter, sprites do not — gpu.c:359 vs :498. */
                /* Point-sample unless the user asked for filtering. The PS1 point-samples;
                   filtering unconditionally bleeds neighbouring texels across texture-atlas
                   cell boundaries, which shows up as smearing on foliage and similar atlased
                   art. Honours [video] texture_filter, whose documented default is nearest. */
                /* gpu_fetch_texel_f() is gpu_fetch_texel() with the SAME cast when no
                   replacement is bound, and the sub-texel selector when one is: at scale S
                   the interpolated UV varies within a native texel, which is what lets an
                   Sx replacement actually show here. psx/texrep.h; psx_gpu_filter_active()
                   is the shared "replacement bypasses the filter" rule. */
                uint16_t texel = psx_gpu_filter_active(gpu)
                    ? gpu_fetch_texel_bilinear(gpu, tx, ty, tpx, tpy, clutx, cluty, depth)
                    : gpu_fetch_texel_f(gpu, tx, ty, tpx, tpy, clutx, cluty, depth);

                if (!texel)
                    continue;

                /* Same bit 15 the blend decision uses; see gpu.c's matching comment. */
                stp = texel & 0x8000;

                if (data->attrib & PA_TRANSP)
                    transp = (texel & 0x8000) != 0;

                if (data->attrib & PA_RAW) {
                    color = texel;
                } else {
                    /* psx_gpu_modulate_channel() is the single definition of the blend,
                       shared by all three rasterizers. Hardware divides by 128 with integer
                       truncation; this used to round, which turns levels 1..8 into fixed
                       points and makes a frame-feedback trail permanent. See gpu.h. */
                    unsigned int ucr = psx_gpu_modulate_channel(
                        gpu, (texel >> 0 ) & 0x1f, (mod >> 0 ) & 0xff);
                    unsigned int ucg = psx_gpu_modulate_channel(
                        gpu, (texel >> 5 ) & 0x1f, (mod >> 8 ) & 0xff);
                    unsigned int ucb = psx_gpu_modulate_channel(
                        gpu, (texel >> 10) & 0x1f, (mod >> 16) & 0xff);

                    uint32_t rgb = ucr | (ucg << 8) | (ucb << 16);

                    color = BGR555(rgb);
                }
            } else {
                color = BGR555(mod);
            }

            float cr = ((color >> 0 ) & 0x1f) << 3;
            float cg = ((color >> 5 ) & 0x1f) << 3;
            float cb = ((color >> 10) & 0x1f) << 3;

            if (transp) {
                /* Blending reads the destination, which is the render target — at S=1
                   that is the same surface the software path reads from gpu->vram. */
                uint16_t back = rt->rt[x + (y * rt->rt_w)];

                float br = ((back >> 0 ) & 0x1f) << 3;
                float bg = ((back >> 5 ) & 0x1f) << 3;
                float bb = ((back >> 10) & 0x1f) << 3;

                switch (transp_mode) {
                    case 0: {
                        cr = (0.5f * br) + (0.5f * cr);
                        cg = (0.5f * bg) + (0.5f * cg);
                        cb = (0.5f * bb) + (0.5f * cb);
                    } break;
                    case 1: {
                        cr = br + cr;
                        cg = bg + cg;
                        cb = bb + cb;
                    } break;
                    case 2: {
                        cr = br - cr;
                        cg = bg - cg;
                        cb = bb - cb;
                    } break;
                    case 3: {
                        cr = br + (0.25 * cr);
                        cg = bg + (0.25 * cg);
                        cb = bb + (0.25 * cb);
                    } break;
                }

                cr = (cr >= 255.0f) ? 255.0f : ((cr <= 0.0f) ? 0.0f : cr);
                cg = (cg >= 255.0f) ? 255.0f : ((cg <= 0.0f) ? 0.0f : cg);
                cb = (cb >= 255.0f) ? 255.0f : ((cb <= 0.0f) ? 0.0f : cb);

                unsigned int ucr = roundf(cr);
                unsigned int ucg = roundf(cg);
                unsigned int ucb = roundf(cb);

                uint32_t rgb = ucr | (ucg << 8) | (ucb << 16);

                color = BGR555(rgb);
            }

            /* `force_mask || texel_bit15` (the backend); mask_from_texel
               is 0 unless the accuracy flag is on, so the default path is unchanged. */
            rt->rt[x + (y * rt->rt_w)] = color | mask_set | (stp & mask_from_texel);
        }
    }
}

static void rt_draw_poly(psx_gpu_backend_t* be, psx_gpu_t* gpu, const poly_data_t* poly) {
    armsx_hw_rt_t* rt = rt_self(be);

    /* Same split as gpu_poly (gpu.c:1166-1171). */
    if (poly->attrib & PA_QUAD) {
        rt_render_triangle(rt, gpu, poly->v[0], poly->v[1], poly->v[2], poly);
        rt_render_triangle(rt, gpu, poly->v[1], poly->v[2], poly->v[3], poly);
    } else {
        rt_render_triangle(rt, gpu, poly->v[0], poly->v[1], poly->v[2], poly);
    }
}

/* ------------------------------------------------------------------------------------
   Sprites — mirrors gpu_render_rect (gpu.c:453-597)
   ------------------------------------------------------------------------------------ */

static void rt_draw_rect(psx_gpu_backend_t* be, psx_gpu_t* gpu, const rect_data_t* in) {
    armsx_hw_rt_t* rt = rt_self(be);
    const int s = rt->scale;

    rect_data_t data = *in;
    uint16_t width = 0, height = 0;

    switch ((data.attrib >> 3) & 3) {
        case RS_VARIABLE: { width = data.width; height = data.height; } break;
        case RS_1X1     : { width = 1         ; height = 1          ; } break;
        case RS_8X8     : { width = 8         ; height = 8          ; } break;
        case RS_16X16   : { width = 16        ; height = 16         ; } break;
    }

    int textured = (data.attrib & RA_TEXTURED) != 0;
    /* Per-PRIMITIVE default; reset into a per-PIXEL `transp` in the loop below, for the same
       reason as rt_render_triangle(). */
    const int transp_default = (data.attrib & RA_TRANSP) != 0;
    /* Sprites always take the blend mode from GPUSTAT (gpu.c:465) — the texpage bits were
       latched there by the last textured polygon (gpu.c:1115). */
    int transp_mode = (gpu->gpustat >> 5) & 3;

    int clutx = (data.clut & 0x3f) << 4;
    int cluty = (data.clut >> 6) & 0x1ff;

    /* The double sign-extension at gpu.c:471-474 is a real behavioural detail: the offset
       is added and the result re-truncated to 11 bits before clamping. */
    data.v0.x += gpu->off_x;
    data.v0.y += gpu->off_y;
    data.v0.x = SE10(data.v0.x);
    data.v0.y = SE10(data.v0.y);

    int xmax = data.v0.x + width;
    int ymax = data.v0.y + height;

    xmax = CLAMP(xmax, -1024, 1024);
    ymax = CLAMP(ymax, -1024, 1024);
    int x0 = CLAMP(data.v0.x, -1024, 1024);
    int y0 = CLAMP(data.v0.y, -1024, 1024);

    const int mask_check = psx_gpu_mask_check(gpu);
    const uint16_t mask_set = psx_gpu_mask_set(gpu) ? 0x8000 : 0x0000;
    const uint16_t mask_from_texel = psx_gpu_mask_from_texel(gpu);

    int clip_x1 = (int)gpu->draw_x1 * s;
    int clip_y1 = (int)gpu->draw_y1 * s;
    int clip_x2 = (int)gpu->draw_x2 * s + (s - 1);
    int clip_y2 = (int)gpu->draw_y2 * s + (s - 1);

    /* Same scissor narrowing as the triangle path. The texel counters below are derived
       from the absolute position rather than a running increment, so skipping columns
       cannot desynchronise them. */
    int y_begin = y0 * s, y_end = ymax * s;
    int x_begin = x0 * s, x_end = xmax * s;

    if (y_begin < clip_y1) y_begin = clip_y1;
    if (y_end > (clip_y2 + 1)) y_end = clip_y2 + 1;
    if (x_begin < clip_x1) x_begin = clip_x1;
    if (x_end > (clip_x2 + 1)) x_end = clip_x2 + 1;

    for (int y = y_begin; y < y_end; y++) {
        for (int x = x_begin; x < x_end; x++) {
            /* Both PER-PIXEL — see transp_default above and psx_gpu_mask_from_texel(). */
            int transp = transp_default;
            uint16_t stp = 0;

            if (mask_check && (rt->rt[x + (y * rt->rt_w)] & 0x8000))
                continue;

            uint16_t color;

            if (textured) {
                /* Texel offsets advance once per NATIVE pixel, so an SxS block samples a
                   single texel — that is what keeps 2D art and UI crisp rather than
                   resampled (). In gpu.c the equivalent counter is xc/yc, which
                   increments even on skipped pixels and is therefore just x - x0. */
                int xc = (x - (x0 * s)) / s;
                int yc = (y - (y0 * s)) / s;

                /* Texture replacement (psx/texrep.h). The branch is on the SAME field
                   gpu_fetch_texel() tests, so neither rasterizer can replace a texel the
                   other did not; the fraction is the SxS block's position inside the native
                   texel, which is what lets an Sx replacement show above 1x. Split out
                   rather than folded into a float UV because the integer fetch below has to
                   stay TEXTUALLY what it was: data.v0.tx + xc can go negative, and
                   round-tripping a negative through float would truncate toward zero and
                   land on a different texel than the modular conversion does. */
                uint16_t texel;

                if (gpu->texrep_bind.img) {
                    float fx = (float)((x - (x0 * s)) % s) / (float)s;
                    float fy = (float)((y - (y0 * s)) % s) / (float)s;

                    texel = psx_texrep_sample(gpu,
                                              (float)(data.v0.tx + xc) + fx,
                                              (float)(data.v0.ty + yc) + fy);
                } else {
                    texel = gpu_fetch_texel(
                        gpu,
                        data.v0.tx + xc, data.v0.ty + yc,
                        gpu->texp_x, gpu->texp_y,
                        clutx, cluty,
                        gpu->texp_d
                    );
                }

                if (!texel)
                    continue;

                /* Sprites point-sample, so this really is the one source texel. */
                stp = texel & 0x8000;

                if ((data.attrib & RA_TRANSP) != 0)
                    transp = (texel & 0x8000) != 0;

                /* Same shared blend as the triangle path; see gpu.h. This is the site
                   Silent Hill's loading-screen feedback blit goes through. */
                unsigned int ucr = psx_gpu_modulate_channel(
                    gpu, (texel >> 0 ) & 0x1f, (data.v0.c >> 0 ) & 0xff);
                unsigned int ucg = psx_gpu_modulate_channel(
                    gpu, (texel >> 5 ) & 0x1f, (data.v0.c >> 8 ) & 0xff);
                unsigned int ucb = psx_gpu_modulate_channel(
                    gpu, (texel >> 10) & 0x1f, (data.v0.c >> 16) & 0xff);

                uint32_t rgb = ucr | (ucg << 8) | (ucb << 16);

                color = BGR555(rgb);
            } else {
                color = BGR555(data.v0.c);
            }

            float cr = ((color >> 0 ) & 0x1f) << 3;
            float cg = ((color >> 5 ) & 0x1f) << 3;
            float cb = ((color >> 10) & 0x1f) << 3;

            if (transp) {
                uint16_t back = rt->rt[x + (y * rt->rt_w)];

                float br = ((back >> 0 ) & 0x1f) << 3;
                float bg = ((back >> 5 ) & 0x1f) << 3;
                float bb = ((back >> 10) & 0x1f) << 3;

                switch (transp_mode) {
                    case 0: {
                        cr = (0.5f * br) + (0.5f * cr);
                        cg = (0.5f * bg) + (0.5f * cg);
                        cb = (0.5f * bb) + (0.5f * cb);
                    } break;
                    case 1: {
                        cr = br + cr;
                        cg = bg + cg;
                        cb = bb + cb;
                    } break;
                    case 2: {
                        cr = br - cr;
                        cg = bg - cg;
                        cb = bb - cb;
                    } break;
                    case 3: {
                        cr = br + (0.25f * cr);
                        cg = bg + (0.25f * cg);
                        cb = bb + (0.25f * cb);
                    } break;
                }

                cr = (cr >= 255.0f) ? 255.0f : ((cr <= 0.0f) ? 0.0f : cr);
                cg = (cg >= 255.0f) ? 255.0f : ((cg <= 0.0f) ? 0.0f : cg);
                cb = (cb >= 255.0f) ? 255.0f : ((cb <= 0.0f) ? 0.0f : cb);

                unsigned int ucr = roundf(cr);
                unsigned int ucg = roundf(cg);
                unsigned int ucb = roundf(cb);

                uint32_t rgb = ucr | (ucg << 8) | (ucb << 16);

                color = BGR555(rgb);
            }

            /* `force_mask || texel_bit15` (the backend); mask_from_texel
               is 0 unless the accuracy flag is on, so the default path is unchanged. */
            rt->rt[x + (y * rt->rt_w)] = color | mask_set | (stp & mask_from_texel);
        }
    }
}

/* ------------------------------------------------------------------------------------
   Lines — mirrors gpu_render_flat_line / plotLine (gpu.c:599-676)

   Bresenham is stepped in NATIVE space and each step fills an SxS block, so the line
   keeps its native thickness as S grows and the pixel set at S=1 is exactly the software
   one.  suggests expanding to a quad instead; that is the right answer for a GPU
   backend but would not reproduce Bresenham's exact diagonals at 1x.
   ------------------------------------------------------------------------------------ */

static void rt_plot(armsx_hw_rt_t* rt, psx_gpu_t* gpu, int x, int y, uint16_t color) {
    const int s = rt->scale;

    int bc = (x >= (int)gpu->draw_x1) && (x <= (int)gpu->draw_x2) &&
             (y >= (int)gpu->draw_y1) && (y <= (int)gpu->draw_y2);

    if (!((x < 1024) && (y < 512) && (x >= 0) && (y >= 0) && bc))
        return;

    for (int by = 0; by < s; by++) {
        uint16_t* row = rt->rt + ((y * s + by) * rt->rt_w) + (x * s);

        for (int bx = 0; bx < s; bx++)
            row[bx] = color;
    }
}

static void rt_plot_line_low(armsx_hw_rt_t* rt, psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int yi = 1;

    if (dy < 0) {
        yi = -1;
        dy = -dy;
    }

    int d = (2 * dy) - dx;
    int y = y0;

    for (int x = x0; x < x1; x++) {
        rt_plot(rt, gpu, x, y, color);

        if (d > 0) {
            y += yi;
            d += (2 * (dy - dx));
        } else {
            d += 2 * dy;
        }
    }
}

static void rt_plot_line_high(armsx_hw_rt_t* rt, psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xi = 1;

    if (dx < 0) {
        xi = -1;
        dx = -dx;
    }

    int d = (2 * dx) - dy;
    int x = x0;

    for (int y = y0; y < y1; y++) {
        rt_plot(rt, gpu, x, y, color);

        if (d > 0) {
            x = x + xi;
            d += (2 * (dx - dy));
        } else {
            d += 2 * dx;
        }
    }
}

static void rt_draw_line(psx_gpu_backend_t* be, psx_gpu_t* gpu,
                         const vertex_t* in0, const vertex_t* in1, uint16_t color) {
    armsx_hw_rt_t* rt = rt_self(be);

    int x0 = in0->x + gpu->off_x;
    int y0 = in0->y + gpu->off_y;
    int x1 = in1->x + gpu->off_x;
    int y1 = in1->y + gpu->off_y;

    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1) {
            rt_plot_line_low(rt, gpu, x1, y1, x0, y0, color);
        } else {
            rt_plot_line_low(rt, gpu, x0, y0, x1, y1, color);
        }
    } else {
        if (y0 > y1) {
            rt_plot_line_high(rt, gpu, x1, y1, x0, y0, color);
        } else {
            rt_plot_line_high(rt, gpu, x0, y0, x1, y1, color);
        }
    }
}

/* ------------------------------------------------------------------------------------
   VRAM transfers
   ------------------------------------------------------------------------------------ */

/* GP0(02), gpu.c:1839-1851. Ignores the drawing area, like the software path. */
static void rt_fill_vram(psx_gpu_backend_t* be, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint16_t color) {
    armsx_hw_rt_t* rt = rt_self(be);
    const int s = rt->scale;

    for (int ny = (int)y; ny < (int)(y + h); ny++) {
        if ((ny >= 512) || (ny < 0))
            continue;

        for (int nx = (int)x; nx < (int)(x + w); nx++) {
            if ((nx >= 1024) || (nx < 0))
                continue;

            for (int by = 0; by < s; by++) {
                uint16_t* row = rt->rt + ((ny * s + by) * rt->rt_w) + (nx * s);

                for (int bx = 0; bx < s; bx++)
                    row[bx] = color;
            }
        }
    }
}

/* GP0(80), gpu.c:1877-1885. The software path neither wraps nor handles overlap; walking
   the rectangle in the same order at scale reproduces both behaviours. */
static void rt_copy_vram(psx_gpu_backend_t* be, uint32_t sx, uint32_t sy,
                         uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    armsx_hw_rt_t* rt = rt_self(be);
    const int s = rt->scale;

    for (uint32_t ny = 0; ny < h; ny++) {
        for (uint32_t nx = 0; nx < w; nx++) {
            int dstb = ((dx + nx) < 1024) && ((dy + ny) < 512);
            int srcb = ((sx + nx) < 1024) && ((sy + ny) < 512);

            if (!(dstb && srcb))
                continue;

            for (int by = 0; by < s; by++) {
                const uint16_t* src = rt->rt + (((int)(sy + ny) * s + by) * rt->rt_w) + ((int)(sx + nx) * s);
                uint16_t* dst = rt->rt + (((int)(dy + ny) * s + by) * rt->rt_w) + ((int)(dx + nx) * s);

                memmove(dst, src, (size_t)s * sizeof(uint16_t));
            }
        }
    }
}

/* GP0(A0), gpu.c:1286-1314. `src` is the whole native VRAM; the wrap masks match the
   software path's. Each native texel becomes an SxS block — upload data is native and
   never gains detail from a higher internal resolution (). */
static void rt_upload_vram(psx_gpu_backend_t* be, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h,
                           const uint16_t* src, uint32_t src_stride_px) {
    armsx_hw_rt_t* rt = rt_self(be);
    const int s = rt->scale;

    if (!src)
        return;

    for (uint32_t ny = 0; ny < h; ny++) {
        uint32_t vy = (y + ny) & 0x1ff;

        for (uint32_t nx = 0; nx < w; nx++) {
            uint32_t vx = (x + nx) & 0x3ff;
            uint16_t texel = src[vx + (vy * src_stride_px)];

            for (int by = 0; by < s; by++) {
                uint16_t* row = rt->rt + (((int)vy * s + by) * rt->rt_w) + ((int)vx * s);

                for (int bx = 0; bx < s; bx++)
                    row[bx] = texel;
            }
        }
    }
}

/* ------------------------------------------------------------------------------------
   Frame / scanout / lifetime
   ------------------------------------------------------------------------------------ */

static const void* rt_display_buffer(psx_gpu_backend_t* be, uint32_t disp_x, uint32_t disp_y,
                                     uint32_t* out_stride_bytes) {
    armsx_hw_rt_t* rt = rt_self(be);
    const int s = rt->scale;

    if (out_stride_bytes)
        *out_stride_bytes = (uint32_t)rt->rt_w * sizeof(uint16_t);

    /* The scanout origin is a screen position, so it scales (). */
    return rt->rt + ((int)disp_x * s) + ((int)disp_y * s * rt->rt_w);
}

static int rt_resolution_scale(psx_gpu_backend_t* be) {
    return rt_self(be)->scale;
}

static void rt_destroy(psx_gpu_backend_t* be) {
    armsx_hw_rt_destroy(be);
}

psx_gpu_backend_t* armsx_hw_rt_create(psx_gpu_t* gpu, int scale) {
    if (scale < 1)
        scale = 1;

    if (scale > ARMSX_HW_RT_MAX_SCALE)
        scale = ARMSX_HW_RT_MAX_SCALE;

    armsx_hw_rt_t* rt = (armsx_hw_rt_t*)calloc(1, sizeof(armsx_hw_rt_t));

    if (!rt)
        return NULL;

    rt->scale = scale;
    rt->rt_w = PSX_GPU_FB_WIDTH * scale;
    rt->rt_h = PSX_GPU_FB_HEIGHT * scale;
    rt->rt = (uint16_t*)calloc((size_t)rt->rt_w * (size_t)rt->rt_h, sizeof(uint16_t));

    if (!rt->rt) {
        log_error("hw-rt: could not allocate a %dx%d render target (%zu MB) for scale %dx",
                  rt->rt_w, rt->rt_h,
                  ((size_t)rt->rt_w * (size_t)rt->rt_h * sizeof(uint16_t)) >> 20,
                  scale);
        free(rt);

        return NULL;
    }

    rt->base.impl = rt;
    /* gpu->vram stays authoritative; the core runs the software rasterizer as well. */
    rt->base.flags = PSX_GPU_BACKEND_SOFTWARE_SHADOW;
    rt->base.destroy = rt_destroy;
    rt->base.draw_poly = rt_draw_poly;
    rt->base.draw_rect = rt_draw_rect;
    rt->base.draw_line = rt_draw_line;
    rt->base.fill_vram = rt_fill_vram;
    rt->base.copy_vram = rt_copy_vram;
    rt->base.upload_vram = rt_upload_vram;
    /* download_vram stays NULL: host VRAM is already correct, so GP0(C0) costs nothing. */
    rt->base.display_buffer = rt_display_buffer;
    rt->base.resolution_scale = rt_resolution_scale;

    /* Seed from whatever VRAM already holds, so a backend installed mid-session (or after
       a save state is loaded) starts coherent instead of black. */
    if (gpu && gpu->vram)
        rt_upload_vram(&rt->base, 0, 0, PSX_GPU_FB_WIDTH, PSX_GPU_FB_HEIGHT, gpu->vram, PSX_GPU_FB_WIDTH);

    log_info("hw-rt: internal resolution %dx (%dx%d render target, %zu MB)",
             scale, rt->rt_w, rt->rt_h,
             ((size_t)rt->rt_w * (size_t)rt->rt_h * sizeof(uint16_t)) >> 20);

    return &rt->base;
}

void armsx_hw_rt_destroy(psx_gpu_backend_t* backend) {
    if (!backend)
        return;

    armsx_hw_rt_t* rt = rt_self(backend);

    if (!rt)
        return;

    free(rt->rt);
    free(rt);
}

#endif
