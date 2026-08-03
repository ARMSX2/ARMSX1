#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* Before ../log.h, which pulls in frontend/diagnostics.h and its printf/fprintf/putc
   macros. Including <stdio.h> after those macros exist is a redeclaration minefield. */
#include <stdio.h>
#include <stdarg.h>

#include "gpu.h"
#include "../log.h"
#include "../perf.h"
#include "../pgxp.h"

#ifdef USE_HARDWARE
#include "gpu_backend.h"

/* Every backend hook below is guarded by this. A NULL backend is the default and leaves
   the software rasterizer byte-identical to the pre-backend behaviour.
   HW_RENDERER_DESIGN.md §5.2 tabulates the hook sites. */
#define GPU_BACKEND_HAS(gpu, fn) ((gpu)->backend && (gpu)->backend->fn)

/* A backend with PSX_GPU_BACKEND_SOFTWARE_SHADOW set draws into its own upscaled target
   and relies on the core to keep gpu->vram authoritative, so the software rasterizer runs
   as well. Without the flag the backend has replaced the software path outright. */
#define GPU_BACKEND_SHADOWS(gpu) \
    ((gpu)->backend && ((gpu)->backend->flags & PSX_GPU_BACKEND_SOFTWARE_SHADOW))
#endif

#define SE10(v) ((int16_t)((v) << 5) >> 5)

#if defined(HW_DEBUG)
#define GPU_HW_DEBUG(...) psxe_diag_logf("gpu", __VA_ARGS__)
#else
#define GPU_HW_DEBUG(...) do { } while (0)
#endif

int g_psx_gpu_dither_kernel[] = {
    -4, +0, -3, +1,
    +2, -2, +3, -1,
    -3, +1, -4, +0,
    +3, -1, +2, -2,
};

uint16_t gpu_to_bgr555(uint32_t color) {
    return ((color & 0x0000f8) >> 3) |
           ((color & 0x00f800) >> 6) |
           ((color & 0xf80000) >> 9);
}

#define BGR555(c) \
    (((c & 0x0000f8) >> 3) | \
     ((c & 0x00f800) >> 6) | \
     ((c & 0xf80000) >> 9))

// #define BGR555(c) gpu_to_bgr555(c)

/* Buckets the drawing offset that was in force as each primitive rasterized. TEST BUILDS
   ONLY -- see the note on the census fields in gpu.h for why this must not exist in a
   shipped binary. Without ARMSX_TEST_OFFSET_CENSUS the call sites below expand to nothing
   and the rasterizer entry points are exactly what they were before it was written. */
#ifdef ARMSX_TEST_OFFSET_CENSUS
static void gpu_offset_census(psx_gpu_t* gpu) {
    const int16_t y = (int16_t)gpu->off_y;
    unsigned i;

    gpu->frame_prims++;

    for (i = 0; i < gpu->off_hist_used; i++) {
        if (gpu->off_hist_y[i] == y) {
            gpu->off_hist_n[i]++;

            return;
        }
    }

    /* More than four distinct offsets in one frame is not something this is trying to
       describe; the overflow lands in the last bucket so the total still adds up. */
    if (gpu->off_hist_used < 4) {
        gpu->off_hist_y[gpu->off_hist_used] = y;
        gpu->off_hist_n[gpu->off_hist_used] = 1;
        gpu->off_hist_used++;
    } else {
        gpu->off_hist_n[3]++;
    }
}
#else
#define gpu_offset_census(gpu) ((void)0)
#endif

int min3(int a, int b, int c) {
    int m = (a <= b) ? a : b;

    return (m <= c) ? m : c;
}

int max3(int a, int b, int c) {
    int m = (a > b) ? a : b;

    return (m > c) ? m : c;
}

/* ---- primitive dump ---------------------------------------------------------------------

   ONE FRAME of GP0 traffic, written to a file the user can pull, armed by a marker file —
   the same switch mechanism frontend/gpu_hw_gl.c uses (gl_debug_marker: `hwgl_dump`,
   `hwgl_paint_reject`, ...). This exists because "a rectangle appears around the character"
   cannot be resolved by reading the rasterizer: the question is WHICH primitive covers that
   rectangle and what blend mode / texture page / clip rect it carries, and only the actual
   command stream answers that.

   Cost when idle: one `if (gpu->dbg_file)` per primitive (a NULL that is never written, so
   it predicts perfectly) and one fopen() every GPU_DUMP_POLL_FRAMES vblanks while a capture
   is still affordable. Once the budget is spent even the probe stops. NOTHING here runs per
   pixel, and nothing runs per frame — dma.c already taught this project what an unbounded
   error-level log does to a session.

   The capture is one frame per arming by construction: the marker is deleted the moment a
   capture starts, and the file is closed at the very next vblank.

   Output goes to its own FILE*, not through psxe_diag_logf(), for three reasons: it must
   work with diagnostics logging switched off, ~1000 timestamped lines would drown armsx.log,
   and the user can then send one small file instead of the whole log. Writes use fputs() on
   purpose — frontend/diagnostics.h #defines fprintf/fputc/putc into the diag pipe, which
   would mirror every line into armsx.log anyway.
*/

#define GPU_DUMP_MARKER      "gpu_prim_dump"
#define GPU_DUMP_FILE        "gpu_prim_dump.txt"
#define GPU_DUMP_POLL_FRAMES 30    /* ~0.5 s between marker probes */
#define GPU_DUMP_MAX_LINES   8000  /* per captured frame; a PS1 frame is ~1k primitives */
#define GPU_DUMP_BUDGET      4     /* captures per process, each needing its own `touch` */

/* ---- multi-frame arming (HW_RENDERER_DESIGN.md §0.5.14) -----------------------------------

   A one-frame capture cannot see an OSCILLATION. Xenogears flashes a whole-screen colour that
   swings between consecutive frames and shows a vertically offset duplicate of the frame that
   clips in and out; both are per-FRAME effects, and the question "what differs between frame N
   and frame N+1" is unanswerable from one frame no matter how detailed that frame is.

   One further arming, sharing all of the machinery above:

     gpu_frame_summary  240 consecutive frames (~4 s), ONE census line per frame, to
                        gpu_frame_summary.txt. Per-primitive lines are suppressed, so the cost
                        is a handful of counters per primitive and the file stays ~300 lines.
                        This is the one that answers "what oscillates".

   The summary line carries the whole display state (display_mode, disp_start, the display
   window, and the DERIVED height that psx.c hands the frontend) precisely because a scanout
   that changes shape between frames is the other reported symptom.
*/
#define GPU_SUMMARY_MARKER      "gpu_frame_summary"
#define GPU_SUMMARY_FILE        "gpu_frame_summary.txt"
#define GPU_SUMMARY_FRAMES      240   /* ~4 s at 60 Hz */
/* Raised from 4 after the first Xenogears capture: the colour-flashing scene reported
   big=10..13 while only 4 were itemised, and the 4 that won were opaque background terrain
   that happened to be submitted first — so the semi-transparent effect quads, the only
   plausible carriers of a whole-screen colour swing, were exactly the ones crowded out.
   240 frames x 13 lines is still ~3k lines, well inside GPU_DUMP_MAX_LINES. */
#define GPU_SUMMARY_BIG_MAX     12    /* full-screen-ish primitives itemised per frame */

/* gpu->dbg_dir is stored WITH a trailing '/', so this is a plain concatenation. */
static int gpu_dump_path(psx_gpu_t* gpu, char* out, size_t cap, const char* name) {
    size_t dir_len, name_len;

    if (!gpu->dbg_dir)
        return 0;

    dir_len = strlen(gpu->dbg_dir);
    name_len = strlen(name);

    if ((dir_len + name_len + 1u) > cap)
        return 0;

    memcpy(out, gpu->dbg_dir, dir_len);
    memcpy(out + dir_len, name, name_len + 1u);

    return 1;
}

/* Writes regardless of dbg_quiet. The summary's own lines go through here; everything the
   rest of the file emits goes through gpu_dumpf() and is dropped while a summary runs. */
static void gpu_dump_rawf(psx_gpu_t* gpu, const char* fmt, ...) {
    char line[2048];
    va_list ap;
    FILE* f = (FILE*)gpu->dbg_file;

    if (!f)
        return;

    if (gpu->dbg_lines >= GPU_DUMP_MAX_LINES) {
        if (gpu->dbg_lines == GPU_DUMP_MAX_LINES) {
            gpu->dbg_lines++;
            fputs("... truncated: GPU_DUMP_MAX_LINES reached\n", f);
        }

        return;
    }

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    fputs(line, f);
    fputs("\n", f);

    gpu->dbg_lines++;
}

static void gpu_dumpf(psx_gpu_t* gpu, const char* fmt, ...) {
    char line[2048];
    va_list ap;
    FILE* f = (FILE*)gpu->dbg_file;

    if (!f || gpu->dbg_quiet)
        return;

    if (gpu->dbg_lines >= GPU_DUMP_MAX_LINES) {
        if (gpu->dbg_lines == GPU_DUMP_MAX_LINES) {
            gpu->dbg_lines++;
            fputs("... truncated: GPU_DUMP_MAX_LINES reached\n", f);
        }

        return;
    }

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    fputs(line, f);
    fputs("\n", f);

    gpu->dbg_lines++;
}

static const char* gpu_dump_depth_name(int depth) {
    switch (depth & 3) {
        case 0:  return "4bpp-clut";
        case 1:  return "8bpp-clut";
        case 2:  return "15bpp-direct";
        default: return "15bpp-direct(reserved)";
    }
}

/* GPUSTAT bits 5-6 / texpage bits 5-6. B is what VRAM already holds, F the incoming pixel. */
static const char* gpu_dump_tmode_name(int mode) {
    switch (mode & 3) {
        case 0:  return "B/2+F/2";
        case 1:  return "B+F";
        case 2:  return "B-F";
        default: return "B+F/4";
    }
}

static void gpu_dump_poly_attrib(char* out, size_t cap, uint8_t attrib) {
    snprintf(out, cap, "%s%s%s%s%s",
        (attrib & PA_TEXTURED) ? "TEXTURED|" : "",
        (attrib & PA_TRANSP)   ? "TRANSP|"   : "",
        (attrib & PA_RAW)      ? "RAW|"      : "",
        (attrib & PA_SHADED)   ? "SHADED|"   : "",
        (attrib & PA_QUAD)     ? "QUAD"      : "TRI");
}

static void gpu_dump_rect_attrib(char* out, size_t cap, uint8_t attrib) {
    static const char* const kSize[4] = { "VARIABLE", "1x1", "8x8", "16x16" };

    snprintf(out, cap, "%s%s%s%s",
        (attrib & RA_TEXTURED) ? "TEXTURED|" : "",
        (attrib & RA_TRANSP)   ? "TRANSP|"   : "",
        (attrib & RA_RAW)      ? "RAW|"      : "",
        kSize[(attrib >> 3) & 3]);
}

/* The drawing context every primitive is rasterised against. Repeated on each line on
   purpose: the whole point is to catch a clip rect or texture window that was set for one
   pass and never restored, which is invisible in a header printed once. */
static void gpu_dump_ctx(psx_gpu_t* gpu, char* out, size_t cap) {
    snprintf(out, cap,
        "draw=(%u,%u)-(%u,%u) off=(%d,%d) texw=(mx=%u,my=%u,ox=%u,oy=%u) "
        "stat=%08x statmode=%d maskset=%d maskchk=%d dither=%d",
        gpu->draw_x1, gpu->draw_y1, gpu->draw_x2, gpu->draw_y2,
        gpu->off_x, gpu->off_y,
        gpu->texw_mx, gpu->texw_my, gpu->texw_ox, gpu->texw_oy,
        gpu->gpustat,
        (gpu->gpustat >> 5) & 3,
        psx_gpu_mask_set(gpu) ? 1 : 0,
        psx_gpu_mask_check(gpu) ? 1 : 0,
        psx_gpu_dither_enabled(gpu) ? 1 : 0);
}

/* ---- frame census (summary mode) --------------------------------------------------------

   Mirrors of psx.c's psx_get_dmode_width()/psx_get_dmode_height(). Duplicated rather than
   called because psx.c is not linked into the parity harnesses, and because what the census
   has to report is exactly the number the FRONTEND will size its texture from — if that
   number moves between frames, the picture changes shape, which is a reported symptom.
   Keep these two in step with psx.c.
*/
static int gpu_dump_dmode_width(const psx_gpu_t* gpu) {
    static const int kHres[4] = { 256, 320, 512, 640 };

    return (gpu->display_mode & 0x40) ? 368 : kHres[gpu->display_mode & 3];
}

static int gpu_dump_dmode_height(const psx_gpu_t* gpu) {
    int disp;

    if (gpu->display_mode & 0x4)
        return 480;

    disp = (int)gpu->disp_y2 - (int)gpu->disp_y1;

    return (disp < (255 - 16)) ? disp : 240;
}

/* A primitive covering at least half the display in BOTH axes. A global colour transform —
   a screen flash, a fade, a fog pass — is one of these, so they are itemised individually
   while everything else is only counted. */
static int gpu_dump_is_big(const psx_gpu_t* gpu, int xmin, int ymin, int xmax, int ymax) {
    const int w = gpu_dump_dmode_width(gpu);
    const int h = gpu_dump_dmode_height(gpu);

    return ((xmax - xmin) * 2 >= w) && ((ymax - ymin) * 2 >= h) && (h > 0);
}

/* The one call every per-primitive dump helper makes in summary mode, in place of its line.
   `colour` is the primitive's first-vertex colour, which for the flat untextured quads a
   screen effect is made of IS the effect. */
static void gpu_dump_census(psx_gpu_t* gpu, const char* kind, unsigned attrib, int transp,
                            int tmode, int textured, uint32_t colour,
                            int xmin, int ymin, int xmax, int ymax) {
    if (transp) {
        gpu->dbg_transp++;
        gpu->dbg_tmode[tmode & 3]++;
    }

    if (!gpu_dump_is_big(gpu, xmin, ymin, xmax, ymax))
        return;

    if (gpu->dbg_big >= GPU_SUMMARY_BIG_MAX) {
        gpu->dbg_big++;   /* still counted, so the summary line can say it overflowed */

        return;
    }

    gpu->dbg_big++;

    gpu_dump_rawf(gpu,
        "F%04d   BIG %s attrib=%02x bbox=(%d,%d)-(%d,%d) transp=%d tmode=%d[%s] %s c=%06x "
        "maskset=%d maskchk=%d stat=%08x tpage=(%u,%u)",
        gpu->dbg_frame_idx, kind, attrib, xmin, ymin, xmax, ymax,
        transp, tmode, gpu_dump_tmode_name(tmode),
        textured ? "textured" : "flat", colour,
        psx_gpu_mask_set(gpu) ? 1 : 0, psx_gpu_mask_check(gpu) ? 1 : 0, gpu->gpustat,
        gpu->texp_x, gpu->texp_y);
}

/*
    Per-TRIANGLE size report, and the reason it exists.

    The bbox printed on the main line is the whole quad's, but the rasterizers cull per
    triangle after the quad split, so a quad's bbox does not tell you whether anything was
    dropped. This appends the span of each triangle exactly as gpu_render_triangle() will see
    it, and flags two things by name so a capture can be grepped rather than replayed:

      OVERSIZE  at least one triangle exceeds hardware's 1023x511 limit, so hardware would
                NOT render it. `culled=` says whether THIS build dropped it, which is
                PSX_GPU_ACCURACY_PRIM_SIZE's state.
      SAT       at least one vertex sits exactly on a GTE screen-coordinate saturation value
                (-1024 or +1023), i.e. the projection ran off and got clamped. This is the
                usual generator of an OVERSIZE primitive, and the pair together is the
                signature of a triangle stretched to a point at the screen edge.

    See HW_RENDERER_DESIGN.md §0.5.13.
*/
static const char* gpu_dump_poly_size(psx_gpu_t* gpu, const poly_data_t* poly, char* out,
                                      size_t cap) {
    /* Same split as the dispatch below: quad -> (0,1,2) and (1,2,3), triangle -> (0,1,2). */
    static const int tri_idx[2][3] = { { 0, 1, 2 }, { 1, 2, 3 } };
    const int ntri = (poly->attrib & PA_QUAD) ? 2 : 1;
    const int nv = (poly->attrib & PA_QUAD) ? 4 : 3;
    int oversize = 0, saturated = 0, culled = 0, t, i;
    size_t used = 0;
    int written;

    out[0] = '\0';

    written = snprintf(out, cap, " span=");

    if (written < 0 || (size_t)written >= cap)
        return out;

    used = (size_t)written;

    for (t = 0; t < ntri; t++) {
        int xmin, xmax, ymin, ymax;

        xmin = xmax = poly->v[tri_idx[t][0]].x;
        ymin = ymax = poly->v[tri_idx[t][0]].y;

        for (i = 1; i < 3; i++) {
            const int vx = poly->v[tri_idx[t][i]].x;
            const int vy = poly->v[tri_idx[t][i]].y;

            if (vx < xmin) xmin = vx;
            if (vx > xmax) xmax = vx;
            if (vy < ymin) ymin = vy;
            if (vy > ymax) ymax = vy;
        }

        /* The drawing offset is a translation, so spans are the same before and after it. */
        if (((xmax - xmin) > 1023) || ((ymax - ymin) > 511))
            oversize = 1;

        if (psx_gpu_prim_oversize(gpu, xmax - xmin, ymax - ymin))
            culled = 1;

        written = snprintf(out + used, cap - used, "%s(%d,%d)",
                           t ? "/" : "", xmax - xmin, ymax - ymin);

        if (written < 0 || (size_t)written >= (cap - used))
            return out;

        used += (size_t)written;
    }

    for (i = 0; i < nv; i++) {
        if ((poly->v[i].x == 1023) || (poly->v[i].x == -1024) ||
            (poly->v[i].y == 1023) || (poly->v[i].y == -1024))
            saturated = 1;
    }

    if (oversize) {
        written = snprintf(out + used, cap - used, " OVERSIZE culled=%s", culled ? "yes" : "no");

        if (written < 0 || (size_t)written >= (cap - used))
            return out;

        used += (size_t)written;
    }

    if (saturated)
        snprintf(out + used, cap - used, " SAT");

    return out;
}

static void gpu_dump_poly(psx_gpu_t* gpu, const poly_data_t* poly) {
    char attrib[64], ctx[320], verts[640], tex[192], size[96];
    const int textured = (poly->attrib & PA_TEXTURED) != 0;
    const int nv = (poly->attrib & PA_QUAD) ? 4 : 3;
    /* gpu_render_triangle() picks the mode exactly this way. */
    const int tmode = textured ? ((poly->texp >> 5) & 3) : ((gpu->gpustat >> 5) & 3);
    const int depth = (poly->texp >> 7) & 3;
    int xmin, ymin, xmax, ymax, i;
    size_t used = 0;

    if (gpu->dbg_summary) {
        gpu->dbg_poly++;

        xmin = xmax = poly->v[0].x + gpu->off_x;
        ymin = ymax = poly->v[0].y + gpu->off_y;

        for (i = 1; i < nv; i++) {
            const int vx = poly->v[i].x + gpu->off_x;
            const int vy = poly->v[i].y + gpu->off_y;

            if (vx < xmin) xmin = vx;
            if (vx > xmax) xmax = vx;
            if (vy < ymin) ymin = vy;
            if (vy > ymax) ymax = vy;
        }

        gpu_dump_census(gpu, "poly", poly->attrib, (poly->attrib & PA_TRANSP) != 0, tmode,
                        textured, poly->v[0].c, xmin, ymin, xmax, ymax);

        return;
    }

    xmin = xmax = poly->v[0].x + gpu->off_x;
    ymin = ymax = poly->v[0].y + gpu->off_y;
    verts[0] = '\0';

    for (i = 0; i < nv; i++) {
        const int vx = poly->v[i].x + gpu->off_x;
        const int vy = poly->v[i].y + gpu->off_y;
        int written;

        if (vx < xmin) xmin = vx;
        if (vx > xmax) xmax = vx;
        if (vy < ymin) ymin = vy;
        if (vy > ymax) ymax = vy;

        written = snprintf(verts + used, sizeof(verts) - used,
                           " v%d=(%d,%d c=%06x uv=%u,%u)",
                           i, poly->v[i].x, poly->v[i].y, poly->v[i].c,
                           (unsigned)poly->v[i].tx, (unsigned)poly->v[i].ty);

        if (written < 0 || (size_t)written >= (sizeof(verts) - used))
            break;

        used += (size_t)written;
    }

    if (textured) {
        snprintf(tex, sizeof(tex),
                 "texp=%04x tpage=(%u,%u) depth=%d[%s] clut=%04x clutpos=(%u,%u)",
                 poly->texp,
                 (unsigned)((poly->texp & 0xf) << 6),
                 (unsigned)((poly->texp & 0x10) << 4),
                 depth, gpu_dump_depth_name(depth),
                 poly->clut,
                 (unsigned)((poly->clut & 0x3f) << 4),
                 (unsigned)((poly->clut >> 6) & 0x1ff));
    } else {
        snprintf(tex, sizeof(tex), "untextured");
    }

    gpu_dump_poly_attrib(attrib, sizeof(attrib), poly->attrib);
    gpu_dump_ctx(gpu, ctx, sizeof(ctx));

    gpu_dumpf(gpu,
        "%05d poly attrib=%02x[%s] bbox=(%d,%d)-(%d,%d) tmode=%d[%s] %s %s%s%s",
        gpu->dbg_prims, poly->attrib, attrib,
        xmin, ymin, xmax, ymax,
        tmode, gpu_dump_tmode_name(tmode),
        tex, ctx, gpu_dump_poly_size(gpu, poly, size, sizeof(size)), verts);
}

static void gpu_dump_rect(psx_gpu_t* gpu, const rect_data_t* rect) {
    char attrib[64], ctx[320], tex[192];
    const int textured = (rect->attrib & RA_TEXTURED) != 0;
    /* Sprites carry no texpage word; gpu_render_rect() reads the latched GPUSTAT mode. */
    const int tmode = (gpu->gpustat >> 5) & 3;
    int w, h, x0, y0;

    switch ((rect->attrib >> 3) & 3) {
        case RS_1X1:   w = 1;  h = 1;  break;
        case RS_8X8:   w = 8;  h = 8;  break;
        case RS_16X16: w = 16; h = 16; break;
        default:       w = rect->width; h = rect->height; break;
    }

    /* gpu_render_rect() offsets then re-sign-extends to 11 bits; mirror it so the bbox
       printed here is the one that actually gets rasterised. */
    x0 = SE10(rect->v0.x + gpu->off_x);
    y0 = SE10(rect->v0.y + gpu->off_y);

    if (gpu->dbg_summary) {
        gpu->dbg_rect++;
        gpu_dump_census(gpu, "rect", rect->attrib, (rect->attrib & RA_TRANSP) != 0, tmode,
                        textured, rect->v0.c, x0, y0, x0 + w, y0 + h);

        return;
    }

    if (textured) {
        snprintf(tex, sizeof(tex),
                 "uv=(%u,%u) tpage=(%u,%u) depth=%u[%s] clut=%04x clutpos=(%u,%u)",
                 (unsigned)rect->v0.tx, (unsigned)rect->v0.ty,
                 gpu->texp_x, gpu->texp_y,
                 gpu->texp_d, gpu_dump_depth_name((int)gpu->texp_d),
                 rect->clut,
                 (unsigned)((rect->clut & 0x3f) << 4),
                 (unsigned)((rect->clut >> 6) & 0x1ff));
    } else {
        snprintf(tex, sizeof(tex), "untextured");
    }

    gpu_dump_rect_attrib(attrib, sizeof(attrib), rect->attrib);
    gpu_dump_ctx(gpu, ctx, sizeof(ctx));

    gpu_dumpf(gpu,
        "%05d rect attrib=%02x[%s] bbox=(%d,%d)-(%d,%d) size=%dx%d tmode=%d[%s] c=%06x %s %s",
        gpu->dbg_prims, rect->attrib, attrib,
        x0, y0, x0 + w, y0 + h, w, h,
        tmode, gpu_dump_tmode_name(tmode),
        rect->v0.c, tex, ctx);
}

static void gpu_dump_prim_line(psx_gpu_t* gpu, const vertex_t* v0, const vertex_t* v1,
                               uint8_t attrib) {
    char ctx[320];
    const int tmode = (gpu->gpustat >> 5) & 3;
    /* min()/max() live further down this file; ternaries keep the dump self-contained. */
    const int x0 = v0->x + gpu->off_x, x1 = v1->x + gpu->off_x;
    const int y0 = v0->y + gpu->off_y, y1 = v1->y + gpu->off_y;

    if (gpu->dbg_summary) {
        gpu->dbg_line_prims++;
        gpu_dump_census(gpu, "line", attrib, (attrib & 0x02) != 0, tmode, 0, v0->c,
                        (x0 < x1) ? x0 : x1, (y0 < y1) ? y0 : y1,
                        (x0 > x1) ? x0 : x1, (y0 > y1) ? y0 : y1);

        return;
    }

    gpu_dump_ctx(gpu, ctx, sizeof(ctx));

    gpu_dumpf(gpu,
        "%05d line attrib=%02x[%s%s] bbox=(%d,%d)-(%d,%d) tmode=%d[%s] "
        "v0=(%d,%d c=%06x) v1=(%d,%d c=%06x) %s",
        gpu->dbg_prims, attrib,
        (attrib & 0x02) ? "TRANSP|" : "",
        (attrib & 0x10) ? "SHADED" : "FLAT",
        (x0 < x1) ? x0 : x1, (y0 < y1) ? y0 : y1,
        (x0 > x1) ? x0 : x1, (y0 > y1) ? y0 : y1,
        tmode, gpu_dump_tmode_name(tmode),
        v0->x, v0->y, v0->c, v1->x, v1->y, v1->c, ctx);
}

static void gpu_dump_frame_end(psx_gpu_t* gpu) {
    char summary[256];
    FILE* f = (FILE*)gpu->dbg_file;

    if (!f)
        return;

    /* Summary mode ends every frame with its one census line instead. Everything the frame
       needs to be compared against its neighbour is on it: the display state that decides
       the picture's SHAPE, and the blend census that decides its COLOUR. */
    if (gpu->dbg_summary) {
        gpu_dump_rawf(gpu,
            "F%04d mode=%06x hres=%d vresbit=%d dheight=%d %s %s %s enable=%d "
            "disp_start=(%u,%u) disp_h=(%u,%u) disp_v=(%u,%u) "
            "stat=%08x statmode=%d maskset=%d maskchk=%d off=(%d,%d) draw=(%u,%u)-(%u,%u) "
            "prims=%d poly=%d rect=%d line=%d transp=%d tmode=[%d,%d,%d,%d] "
            "fill=%d fillc=%06x copy=%d up=%d down=%d big=%d",
            gpu->dbg_frame_idx, gpu->display_mode,
            gpu_dump_dmode_width(gpu), (gpu->display_mode & 0x04) ? 480 : 240,
            gpu_dump_dmode_height(gpu),
            (gpu->display_mode & 0x10) ? "24bpp" : "15bpp",
            (gpu->display_mode & 0x20) ? "interlace" : "progressive",
            psx_gpu_is_pal_mode(gpu) ? "PAL" : "NTSC",
            (gpu->gpustat & 0x00800000) ? 0 : 1,
            gpu->disp_x, gpu->disp_y, gpu->disp_x1, gpu->disp_x2, gpu->disp_y1, gpu->disp_y2,
            gpu->gpustat, (gpu->gpustat >> 5) & 3,
            psx_gpu_mask_set(gpu) ? 1 : 0, psx_gpu_mask_check(gpu) ? 1 : 0,
            gpu->off_x, gpu->off_y,
            gpu->draw_x1, gpu->draw_y1, gpu->draw_x2, gpu->draw_y2,
            gpu->dbg_prims, gpu->dbg_poly, gpu->dbg_rect, gpu->dbg_line_prims,
            gpu->dbg_transp,
            gpu->dbg_tmode[0], gpu->dbg_tmode[1], gpu->dbg_tmode[2], gpu->dbg_tmode[3],
            gpu->dbg_fill, gpu->dbg_fill_c,
            gpu->dbg_copy, gpu->dbg_upload,
            gpu->dbg_download, gpu->dbg_big);

        fflush(f);

        /* The file stays open across the frames of one arming: 240 fopen/fclose pairs is
           240 more chances to interleave with whatever else writes to this directory. */
        return;
    }

    /* Written directly rather than through gpu_dumpf(): a capture that hit the line cap has
       to keep its summary, which is exactly the case where the cap would swallow it. */
    snprintf(summary, sizeof(summary), "--- end capture #%d: %d primitives, %d lines%s ---\n",
             gpu->dbg_seq, gpu->dbg_prims, gpu->dbg_lines,
             (gpu->dbg_lines > GPU_DUMP_MAX_LINES) ? " (TRUNCATED)" : "");
    fputs(summary, f);

    fflush(f);
    fclose(f);

    gpu->dbg_file = NULL;
}

/* Zeroes the per-frame census. Deliberately does NOT touch dbg_lines: in summary mode the
   line cap is the budget for the WHOLE arming, not for one frame of it. */
static void gpu_dump_census_reset(psx_gpu_t* gpu) {
    gpu->dbg_prims = 0;
    gpu->dbg_poly = 0;
    gpu->dbg_rect = 0;
    gpu->dbg_line_prims = 0;
    gpu->dbg_transp = 0;
    gpu->dbg_tmode[0] = gpu->dbg_tmode[1] = gpu->dbg_tmode[2] = gpu->dbg_tmode[3] = 0;
    gpu->dbg_fill = 0;
    gpu->dbg_copy = 0;
    gpu->dbg_upload = 0;
    gpu->dbg_download = 0;
    gpu->dbg_fill_c = 0;
    gpu->dbg_fill_area = 0;
    gpu->dbg_big = 0;
}

static void gpu_dump_frame_begin(psx_gpu_t* gpu) {
    static const int kHres[4] = { 256, 320, 512, 640 };
    char path[1024];
    FILE* f;

    if (!gpu_dump_path(gpu, path, sizeof(path), GPU_DUMP_FILE))
        return;

    f = fopen(path, "a");

    if (!f)
        return;

    gpu->dbg_file = f;
    gpu->dbg_lines = 0;
    gpu->dbg_prims = 0;
    gpu->dbg_seq++;

    gpu_dumpf(gpu, "=== gpu_prim_dump capture #%d (frame index %d, %d more to go) ===",
              gpu->dbg_seq, gpu->dbg_frame_idx, (gpu->dbg_frames > 0) ? gpu->dbg_frames - 1 : 0);
    gpu_dumpf(gpu,
        "display_mode=%06x hres=%d%s vres=%d %s %s %s display_enable=%d",
        gpu->display_mode,
        (gpu->display_mode & 0x40) ? 368 : kHres[gpu->display_mode & 3],
        (gpu->display_mode & 0x40) ? "(hres2)" : "",
        (gpu->display_mode & 0x04) ? 480 : 240,
        (gpu->display_mode & 0x10) ? "24bpp" : "15bpp",
        (gpu->display_mode & 0x20) ? "interlace" : "progressive",
        psx_gpu_is_pal_mode(gpu) ? "PAL" : "NTSC",
        (gpu->gpustat & 0x00800000) ? 0 : 1);
    gpu_dumpf(gpu,
        "disp_start=(%u,%u) disp_h=(%u,%u) disp_v=(%u,%u)",
        gpu->disp_x, gpu->disp_y, gpu->disp_x1, gpu->disp_x2, gpu->disp_y1, gpu->disp_y2);
    gpu_dumpf(gpu,
        "gpustat=%08x statmode=%d[%s] maskbit_set=%d maskbit_chk=%d dither_bit=%d "
        "accuracy_flags=%02x (mask_bit=%s dither_gate=%s prim_size=%s[%s])",
        gpu->gpustat,
        (gpu->gpustat >> 5) & 3, gpu_dump_tmode_name((gpu->gpustat >> 5) & 3),
        (gpu->gpustat & 0x0800) ? 1 : 0,
        (gpu->gpustat & 0x1000) ? 1 : 0,
        (gpu->gpustat & 0x0200) ? 1 : 0,
        gpu->accuracy_flags,
        (gpu->accuracy_flags & PSX_GPU_ACCURACY_MASK_BIT) ? "on" : "off",
        (gpu->accuracy_flags & PSX_GPU_ACCURACY_DITHER_GATE) ? "on" : "off",
        (gpu->accuracy_flags & PSX_GPU_ACCURACY_PRIM_SIZE) ? "on" : "off",
        (gpu->accuracy_flags & PSX_GPU_ACCURACY_PRIM_SIZE) ? "1023x511" : "2048x1024");
    gpu_dumpf(gpu,
        "draw=(%u,%u)-(%u,%u) off=(%d,%d) texw=(mx=%u,my=%u,ox=%u,oy=%u) "
        "tpage=(%u,%u) depth=%u[%s]",
        gpu->draw_x1, gpu->draw_y1, gpu->draw_x2, gpu->draw_y2,
        gpu->off_x, gpu->off_y,
        gpu->texw_mx, gpu->texw_my, gpu->texw_ox, gpu->texw_oy,
        gpu->texp_x, gpu->texp_y, gpu->texp_d, gpu_dump_depth_name((int)gpu->texp_d));
    gpu_dumpf(gpu, "--- primitives (bbox and vertex coords are VRAM space, offset applied) ---");
}

/* Summary mode opens its file ONCE for the whole arming and holds it; the per-frame work is
   then a census reset and one line. dbg_quiet silences every other gpu_dumpf() in the file,
   so a 240-frame capture cannot be swamped by GP0(E1) texpage traffic. */
static void gpu_dump_summary_begin(psx_gpu_t* gpu) {
    char path[1024];
    FILE* f;

    if (!gpu_dump_path(gpu, path, sizeof(path), GPU_SUMMARY_FILE))
        return;

    f = fopen(path, "a");

    if (!f)
        return;

    gpu->dbg_file = f;
    gpu->dbg_lines = 0;
    gpu->dbg_seq++;
    gpu->dbg_summary = 1;
    gpu->dbg_quiet = 1;
    gpu->dbg_frame_idx = 0;
    gpu->dbg_frames = GPU_SUMMARY_FRAMES;

    gpu_dump_census_reset(gpu);

    gpu_dump_rawf(gpu, "=== gpu_frame_summary capture #%d: %d consecutive frames ===",
                  gpu->dbg_seq, GPU_SUMMARY_FRAMES);
    gpu_dump_rawf(gpu,
        "=== one line per frame; F<idx>. dheight is the height psx_get_dmode_height() hands "
        "the frontend. tmode=[B/2+F/2,B+F,B-F,B+F/4] counts SEMI-TRANSPARENT primitives. ===");
    gpu_dump_rawf(gpu,
        "=== BIG lines are primitives covering >=half the display in both axes, max %d/frame. ===",
        GPU_SUMMARY_BIG_MAX);
    gpu_dump_rawf(gpu,
        "=== col=<class>:<mean RRGGBB>/<count>/<textured count>, classes op,t0..t3 = opaque and "
        "the four semi-transparent blend modes. Size-blind: EVERY primitive is counted. ===");
    gpu_dump_rawf(gpu,
        "=== e5=<n>[(x,y)@prim ...] offsets in order; st=<n>[RAWWORD@prim:src] raw GP0(E3/E4/E5), src C=cpu O=ot-dma B=block-dma. "
        "fillr is the largest GP0(02) in ABSOLUTE VRAM coords. unk = unhandled GP0 words. ===");
}

static void gpu_dump_summary_finish(psx_gpu_t* gpu) {
    FILE* f = (FILE*)gpu->dbg_file;

    if (f) {
        gpu_dump_rawf(gpu, "--- end gpu_frame_summary capture #%d: %d frames, %d lines%s ---",
                      gpu->dbg_seq, gpu->dbg_frame_idx + 1, gpu->dbg_lines,
                      (gpu->dbg_lines > GPU_DUMP_MAX_LINES) ? " (TRUNCATED)" : "");
        fflush(f);
        fclose(f);
    }

    gpu->dbg_file = NULL;
    gpu->dbg_summary = 0;
    gpu->dbg_quiet = 0;
    gpu->dbg_frames = 0;
}

/* Probes one marker. Returns 1 and consumes both the marker and a budget unit on a hit. */
static int gpu_dump_marker_armed(psx_gpu_t* gpu, const char* name) {
    char path[1024];
    FILE* marker;

    if (!gpu_dump_path(gpu, path, sizeof(path), name))
        return 0;

    marker = fopen(path, "rb");

    if (!marker)
        return 0;

    fclose(marker);

    /* Deleted before the capture starts, so a failed open cannot leave the marker armed
       and a successful one cannot capture more frames than it was asked for. */
    remove(path);

    gpu->dbg_budget--;

    return 1;
}

/* Called from the vblank-start branch of gpu_hblank_event(), i.e. once per frame. */
static void gpu_dump_vblank(psx_gpu_t* gpu) {
    if (gpu->dbg_file) {
        gpu_dump_frame_end(gpu);

        if (gpu->dbg_frames > 0)
            gpu->dbg_frames--;

        if (gpu->dbg_frames > 0) {
            gpu->dbg_frame_idx++;

            if (gpu->dbg_summary) {
                /* File is still open; only the census turns over. */
                gpu_dump_census_reset(gpu);
            } else {
                gpu_dump_frame_begin(gpu);
            }
        } else if (gpu->dbg_summary) {
            gpu_dump_summary_finish(gpu);
        }

        return;
    }

    if (!gpu->dbg_dir || (gpu->dbg_budget <= 0))
        return;

    if (--gpu->dbg_poll > 0)
        return;

    gpu->dbg_poll = GPU_DUMP_POLL_FRAMES;

    if (gpu_dump_marker_armed(gpu, GPU_SUMMARY_MARKER)) {
        gpu_dump_summary_begin(gpu);

        return;
    }

    if (gpu_dump_marker_armed(gpu, GPU_DUMP_MARKER)) {
        gpu->dbg_frames = 1;
        gpu->dbg_frame_idx = 0;
        gpu_dump_frame_begin(gpu);
    }
}

void psx_gpu_debug_set_log_dir(psx_gpu_t* gpu, const char* dir) {
    size_t len;
    char* copy;

    if (!gpu)
        return;

    if (gpu->dbg_file) {
        fclose((FILE*)gpu->dbg_file);
        gpu->dbg_file = NULL;
    }

    /* A capture in flight is abandoned here, so the mode flags must go with it or the next
       one inherits summary mode and writes a census into a full dump. */
    gpu->dbg_summary = 0;
    gpu->dbg_quiet = 0;
    gpu->dbg_frames = 0;
    gpu->dbg_frame_idx = 0;

    free(gpu->dbg_dir);
    gpu->dbg_dir = NULL;
    gpu->dbg_poll = 0;
    gpu->dbg_budget = 0;

    if (!dir || !dir[0])
        return;

    len = strlen(dir);
    /* +1 for a trailing '/' we may have to add, +1 for the terminator. */
    copy = (char*)malloc(len + 2u);

    if (!copy)
        return;

    memcpy(copy, dir, len);

    if (copy[len - 1u] != '/')
        copy[len++] = '/';

    copy[len] = '\0';

    gpu->dbg_dir = copy;
    gpu->dbg_poll = 1;     /* probe on the very next vblank */
    gpu->dbg_budget = GPU_DUMP_BUDGET;
}

psx_gpu_t* psx_gpu_create(void) {
    return (psx_gpu_t*)malloc(sizeof(psx_gpu_t));
}

void psx_gpu_init(psx_gpu_t* gpu, psx_ic_t* ic) {
    memset(gpu, 0, sizeof(psx_gpu_t));

    gpu->io_base = PSX_GPU_BEGIN;
    gpu->io_size = PSX_GPU_SIZE;

    gpu->vram = (uint16_t*)malloc(PSX_GPU_VRAM_SIZE);
    gpu->empty = malloc(PSX_GPU_VRAM_SIZE);

    memset(gpu->empty, 0, PSX_GPU_VRAM_SIZE);

    gpu->state = GPU_STATE_RECV_CMD;
    gpu->gpustat |= 0x800000;

    // Default window size, this is not normally needed
    gpu->display_mode = 1;

    gpu->ic = ic;
#ifdef USE_HARDWARE
    /* Software rasterizer by default; the frontend installs a backend only when
       [video] renderer = "hardware" asks for one. */
    gpu->backend = NULL;
#endif

    GPU_HW_DEBUG(
        "gpu-init display_mode=0x%08x gpustat=0x%08x draw=(%u,%u)-(%u,%u) disp=(%u,%u)-(%u,%u) offset=(%d,%d) pal=%s",
        gpu->display_mode,
        gpu->gpustat,
        gpu->draw_x1,
        gpu->draw_y1,
        gpu->draw_x2,
        gpu->draw_y2,
        gpu->disp_x1,
        gpu->disp_y1,
        gpu->disp_x2,
        gpu->disp_y2,
        gpu->off_x,
        gpu->off_y,
        psx_gpu_is_pal_mode(gpu) ? "true" : "false"
    );
}

uint32_t psx_gpu_read32(psx_gpu_t* gpu, uint32_t offset) {
    switch (offset) {
        case 0x00: {
            /*
                GPUREAD is a LATCH, not a strobe. It holds the last word the GPU placed
                there — a VRAM->CPU transfer word, or a GP1(10h) answer — until something
                overwrites it. GP1(10h) indices 0, 1 and 6 are documented as "returns
                nothing", meaning the previous contents stay readable.

                This started at 0 on every read, so any read outside an active transfer
                answered 0 rather than the retained word, and the "returns nothing"
                indices actively destroyed the latch instead of preserving it.
                gpu->gpuread already existed and was already serialised in the save state;
                it simply was not the value being returned.
                Gate: gpuinfo-roundtrip (quiet-index arm) in tests/gpu_renderer_parity.c.
            */
            uint32_t data = gpu->gpuread;

            if (gpu->c0_tsiz) {
                /* A transfer word replaces the latch outright; it is assembled with |=. */
                data = 0;

                data |= gpu->vram[gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt * 1024))];

                gpu->c0_xcnt += 1;

                if (gpu->c0_xcnt == gpu->c0_xsiz) {
                    gpu->c0_ycnt += 1;
                    gpu->c0_xcnt = 0;
                }

                data |= gpu->vram[gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt * 1024))] << 16;

                gpu->c0_xcnt += 1;

                if (gpu->c0_xcnt == gpu->c0_xsiz) {
                    gpu->c0_ycnt += 1;
                    gpu->c0_xcnt = 0;
                }

                gpu->c0_tsiz -= 2;
            }

            if (gpu->gp1_10h_req) {
                switch (gpu->gp1_10h_req & 7) {
                    case 2: {
                        data = ((gpu->texw_oy / 8) << 15) | ((gpu->texw_ox / 8) << 10) | ((gpu->texw_my / 8) << 5) | (gpu->texw_mx / 8);
                    } break;
                    case 3: {
                        data = (gpu->draw_y1 << 10) | gpu->draw_x1;
                    } break;
                    case 4: {
                        data = (gpu->draw_y2 << 10) | gpu->draw_x2;
                    } break;
                    case 5: {
                        /*
                            GP0(E5) packs X in bits 0-10 and Y in bits 11-21 — 11-bit SIGNED
                            fields — unlike E3/E4 above, which give X 10 bits and Y 9 at bit
                            10. off_x/off_y are stored sign-extended (see the decode at
                            GP0(E5)), so mask back to 11 bits to reconstruct the raw payload
                            exactly as the game wrote it.

                            This was `(off_y << 10) | off_x`, copy-pasted from cases 3/4: the
                            readback handed every game its own drawing offset one bit position
                            short (y=224 read back as the E3-layout word for 224, which E5
                            decodes as 112), and a negative off_x would have smeared its sign
                            bits across the whole response. Xenogears builds battle draw-env
                            packets FROM this readback (GP1(10h).5 -> OR 0xE5000000 -> DMA
                            list); we then executed our own wrong answer and clipped ~13% of
                            battle frames. Hardware echoes the payload verbatim; now so do we.
                            Gate: gpuinfo-roundtrip in tests/gpu_renderer_parity.c.
                        */
                        data = (((uint32_t)gpu->off_y & 0x7ff) << 11) |
                               ((uint32_t)gpu->off_x & 0x7ff);
                    } break;
                    case 7: {
                        /*
                            GP1(10h).7 - Read GPU Type. Every retail console reports 2
                            (the 208-pin GPU); 0 means the early 160-pin part found only
                            in pre-production PU-7 boards.

                            There was no case 7 here, so the query fell through and
                            returned whatever was left in GPUREAD — 0 on a cold boot.
                            Any code that version-checks the GPU therefore saw
                            prototype silicon. Same defect class as GPUINFO(5) and the
                            GP1(08) mirror: a readback describing hardware we are not.
                            Gate: gpuinfo-gpu-type in tests/gpu_renderer_parity.c.
                        */
                        data = 2;
                    } break;
                }

                gpu->gp1_10h_req = 0;
            }

            gpu->gpuread = data;

            return data;
        } break;
        case 0x04: return gpu->gpustat | 0x1c000000;
    }

    log_warn("Unhandled 32-bit GPU read at offset %08x", offset);

    return 0x0;
}

uint16_t psx_gpu_read16(psx_gpu_t* gpu, uint32_t offset) {
    printf("Unhandled 16-bit GPU read at offset %08x\n", offset);

    return 0;

    // exit(1);
}

uint8_t psx_gpu_read8(psx_gpu_t* gpu, uint32_t offset) {
    printf("Unhandled 8-bit GPU read at offset %08x\n", offset);

    return 0;

    // exit(1);
}

int min(int x0, int x1) {
    return (x0 <= x1) ? x0 : x1;
}

int max(int x0, int x1) {
    return (x0 >= x1) ? x0 : x1;
}

#define EDGE(a, b, c) ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x))

uint16_t gpu_fetch_texel(psx_gpu_t* gpu, uint16_t tx, uint16_t ty, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth) {
    /* Texture replacement (psx/texrep.h). `img` is NULL for the whole session unless the
       feature was switched on AND a pack file matched THIS primitive, so the cost here is a
       load from a struct this function is about to touch anyway and a branch that is never
       taken. It sits above the window transform because psx_texrep_sample() applies the same
       transform itself — the sampler is shared with gpu_fetch_texel_f(), which reaches it
       with the fractional UV the sub-texel needs.

       Placing it in THIS function rather than at the call sites is deliberate: frontend/
       gpu_hw_rt.c calls straight into it, so the software rasterizer and the CPU hardware
       rasterizer cannot end up disagreeing about which texels got replaced. */
    if (gpu->texrep_bind.img)
        return psx_texrep_sample(gpu, (float)tx, (float)ty);

    tx = (tx & ~gpu->texw_mx) | (gpu->texw_ox & gpu->texw_mx);
    ty = (ty & ~gpu->texw_my) | (gpu->texw_oy & gpu->texw_my);
    tx &= 0xff;
    ty &= 0xff;

    switch (depth) {
        // 4-bit
        case 0: {
            uint16_t texel = gpu->vram[(tpx + (tx >> 2)) + ((tpy + ty) * 1024)];

            int index = (texel >> ((tx & 0x3) << 2)) & 0xf;

            return gpu->vram[(clutx + index) + (cluty * 1024)];
        } break;

        // 8-bit
        case 1: {
            uint16_t texel = gpu->vram[(tpx + (tx >> 1)) + ((tpy + ty) * 1024)];

            int index = (texel >> ((tx & 0x1) << 3)) & 0xff;

            return gpu->vram[(clutx + index) + (cluty * 1024)];
        } break;

        // 15-bit
        default: {
            return gpu->vram[(tpx + tx) + ((tpy + ty) * 1024)];
        } break;
    }
}

uint16_t gpu_fetch_texel_bilinear(psx_gpu_t* gpu, float tx, float ty, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth) {
    float txf = floorf(tx);
    float tyf = floorf(ty);
    float txc = txf + 1.0f;
    float tyc = tyf + 1.0f;

    int s0 = gpu_fetch_texel(gpu, (int)txf, (int)tyf, tpx, tpy, clutx, cluty, depth);

    if (!s0)
        return 0;

    int s1 = gpu_fetch_texel(gpu, (int)txc, (int)tyf, tpx, tpy, clutx, cluty, depth);
    int s2 = gpu_fetch_texel(gpu, (int)txf, (int)tyc, tpx, tpy, clutx, cluty, depth);
    int s3 = gpu_fetch_texel(gpu, (int)txc, (int)tyc, tpx, tpy, clutx, cluty, depth);

    float s0r = (s0 >> 0) & 0x1f;
    float s0g = (s0 >> 5) & 0x1f;
    float s0b = (s0 >> 10) & 0x1f;
    float s1r = (s1 >> 0) & 0x1f;
    float s1g = (s1 >> 5) & 0x1f;
    float s1b = (s1 >> 10) & 0x1f;
    float s2r = (s2 >> 0) & 0x1f;
    float s2g = (s2 >> 5) & 0x1f;
    float s2b = (s2 >> 10) & 0x1f;
    float s3r = (s3 >> 0) & 0x1f;
    float s3g = (s3 >> 5) & 0x1f;
    float s3b = (s3 >> 10) & 0x1f;

    float q1r = s0r * (txc - tx) + s1r * (tx - txf);
    float q1g = s0g * (txc - tx) + s1g * (tx - txf);
    float q1b = s0b * (txc - tx) + s1b * (tx - txf);
    float q2r = s2r * (txc - tx) + s3r * (tx - txf);
    float q2g = s2g * (txc - tx) + s3g * (tx - txf);
    float q2b = s2b * (txc - tx) + s3b * (tx - txf);
    int qr = q1r * (tyc - ty) + q2r * (ty - tyf);
    int qg = q1g * (tyc - ty) + q2g * (ty - tyf);
    int qb = q1b * (tyc - ty) + q2b * (ty - tyf);

    return qr | (qg << 5) | (qb << 10) | (s0 & 0x8000) | (s1 & 0x8000) | (s2 & 0x8000) | (s3 & 0x8000);
}

#define TL(z, a, b) \
    ((z < 0) || ((z == 0) && ((b.y > a.y) || ((b.y == a.y) && (b.x < a.x)))))

void gpu_render_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge) {
    gpu_offset_census(gpu);

    vertex_t a, b, c, p;

    int tpx = (data.texp & 0xf) << 6;
    int tpy = (data.texp & 0x10) << 4;
    int clutx = (data.clut & 0x3f) << 4;
    int cluty = (data.clut >> 6) & 0x1ff;
    int depth = (data.texp >> 7) & 3;
    /* Per-PRIMITIVE default. Reset into `transp` at the top of EVERY pixel below —
       it must not survive from one pixel to the next; see the note there. */
    const int transp_default = (data.attrib & PA_TRANSP) != 0;
    int transp_mode;

    if (data.attrib & PA_TEXTURED) {
        transp_mode = (data.texp >> 5) & 3;
    } else {
        transp_mode = (gpu->gpustat >> 5) & 3;
    }

    a = v0;

    /* Ensure the winding order is correct */
    if (EDGE(v0, v1, v2) < 0) {
        b = v2;
        c = v1;
    } else {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x;
    b.x += gpu->off_x;
    c.x += gpu->off_x;
    a.y += gpu->off_y;
    b.y += gpu->off_y;
    c.y += gpu->off_y;

    int xmin = min3(a.x, b.x, c.x);
    int ymin = min3(a.y, b.y, c.y);
    int xmax = max3(a.x, b.x, c.x);
    int ymax = max3(a.y, b.y, c.y);

    /* Hardware's size cull. Shared with frontend/gpu_hw_rt.c and gpu_hw_gl.c through
       psx_gpu_prim_oversize() so all three agree on which primitives get dropped. */
    if (psx_gpu_prim_oversize(gpu, xmax - xmin, ymax - ymin))
        return;

    PSX_PERF_RASTER(PSX_PERF_PRIM_TRIANGLE, xmax - xmin, ymax - ymin);

    float area = EDGE(a, b, c);

    const int mask_check = psx_gpu_mask_check(gpu);
    const uint16_t mask_set = psx_gpu_mask_set(gpu) ? 0x8000 : 0x0000;
    /* GP0(E6) bit 0 == 0 means "take the written mask bit from the texel" (gpu.h). */
    const uint16_t mask_from_texel = psx_gpu_mask_from_texel(gpu);
    const int dither_on = psx_gpu_dither_enabled(gpu);

    for (int y = ymin; y < ymax; y++) {
        for (int x = xmin; x < xmax; x++) {
            /* PER-PIXEL, and this reset is the whole point.

               `transp` used to be declared once per primitive and then ASSIGNED inside this
               loop from the texel's bit 15. The first texel with that bit clear latched it to
               0 for every remaining pixel of the primitive, so a semi-transparent sprite drew
               correctly until its first opaque texel and turned solid from there on — a hard
               rectangle exactly the size of the sprite. That is Silent Hill's box around the
               player, and it reproduced on both rasterizers because both had the bug.
               (That was ONE of Silent Hill's boxes. The other was the dropped texel mask
               bit — see `stp` just below.) */
            int transp = transp_default;
            /* The source pixel's mask bit. Untextured primitives have no source texel and so
               write 0; see psx_gpu_mask_from_texel(). Per-pixel for the same reason `transp`
               is — latching it per primitive would reintroduce exactly the bug above. */
            uint16_t stp = 0;
            int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                     (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

            if (!bc)
                continue;

            if (mask_check && (gpu->vram[x + (y * 1024)] & 0x8000))
                continue;

            p.x = x;
            p.y = y;

            float z0 = EDGE(b, c, p);

            if (TL(z0, b, c))
                continue;

            float z1 = EDGE(c, a, p);

            if (TL(z1, c, a))
                continue;

            float z2 = EDGE(a, b, p);

            if (TL(z2, a, b))
                continue;

            uint16_t color = 0;
            uint32_t mod   = 0;

            if (data.attrib & PA_SHADED) {
                float cr = (z0 * ((a.c >>  0) & 0xff) + z1 * ((b.c >>  0) & 0xff) + z2 * ((c.c >>  0) & 0xff)) / area;
                float cg = (z0 * ((a.c >>  8) & 0xff) + z1 * ((b.c >>  8) & 0xff) + z2 * ((c.c >>  8) & 0xff)) / area;
                float cb = (z0 * ((a.c >> 16) & 0xff) + z1 * ((b.c >> 16) & 0xff) + z2 * ((c.c >> 16) & 0xff)) / area;

                /* ABSOLUTE framebuffer coordinate, not bounding-box relative.

                   Hardware indexes the 4x4 kernel with the low two bits of the VRAM x/y
                   being written, so the pattern is fixed to the framebuffer and every
                   primitive that touches a pixel dithers it identically. Indexing from the
                   primitive's own bounding box instead re-phases the kernel per primitive,
                   so a smooth gradient split across adjacent Gouraud polygons picks up a
                   visible discontinuity at every shared edge. Mirrored in
                   frontend/gpu_hw_rt.c and the GL draw shader — all three must agree or the
                   parity gate breaks. */
                int dy = y & 3;
                int dx = x & 3;

                int dither = dither_on ? g_psx_gpu_dither_kernel[dx + (dy * 4)] : 0;

                cr += dither;
                cg += dither;
                cb += dither;

                // Saturate (clamp) to 00-ff
                cr = (cr >= 255.0f) ? 255.0f : ((cr <= 0.0f) ? 0.0f : cr);
                cg = (cg >= 255.0f) ? 255.0f : ((cg <= 0.0f) ? 0.0f : cg);
                cb = (cb >= 255.0f) ? 255.0f : ((cb <= 0.0f) ? 0.0f : cb);

                unsigned int ucr = roundf(cr);
                unsigned int ucg = roundf(cg);
                unsigned int ucb = roundf(cb);

                uint32_t rgb = (ucb << 16) | (ucg << 8) | ucr;

                mod = rgb;
            } else {
                mod = data.v[0].c;
            }

            if (data.attrib & PA_TEXTURED) {
                float tx = ((z0 * a.tx) + (z1 * b.tx) + (z2 * c.tx)) / area;
                float ty = ((z0 * a.ty) + (z1 * b.ty) + (z2 * c.ty)) / area;

                /* Filter 0 POINT-SAMPLES, as the hardware does. This used to filter
                   unconditionally, which bleeds neighbouring texels across texture-atlas cell
                   boundaries — the smearing reported on Silent Hill's foliage. Filtering is an
                   enhancement the user opts into via [video] texture_filter; it is not the
                   console's behaviour, and all three rasterizers agree on this. */
                uint16_t texel = psx_gpu_filter_active(gpu)
                    ? gpu_fetch_texel_bilinear(gpu, tx, ty, tpx, tpy, clutx, cluty, depth)
                    : gpu_fetch_texel_f(gpu, tx, ty, tpx, tpy, clutx, cluty, depth);

                if (!texel)
                    continue;

                /* Same bit 15 the blend decision below uses, so the two cannot disagree.
                   gpu_fetch_texel_bilinear() ORs bit 15 across its 2x2 tap, which spreads the
                   written mask bit a texel outward — an artefact of the (non-hardware)
                   bilinear filter, not of this rule. Both rasterizers share the sampler, so
                   the parity gate still holds. */
                stp = texel & 0x8000;

                if (data.attrib & PA_TRANSP)
                    transp = (texel & 0x8000) != 0;

                if (data.attrib & PA_RAW) {
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
                uint16_t back = gpu->vram[x + (y * 1024)];

                float br = ((back >> 0 ) & 0x1f) << 3;
                float bg = ((back >> 5 ) & 0x1f) << 3;
                float bb = ((back >> 10) & 0x1f) << 3;

                // Do we use transp or gpustat here?
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

            /* `force_mask || texel_bit15` (HW_RENDERER_DESIGN.md §2.6). With the accuracy
               flag off mask_from_texel is 0 and this is byte-identical to `color | mask_set`,
               which is what keeps the shipping default path untouched. */
            gpu->vram[x + (y * 1024)] = color | mask_set | (stp & mask_from_texel);
        }
    }
}

#define CLAMP(v, d, u) ((v) <= (d)) ? (d) : (((v) >= (u)) ? (u) : (v))

void gpu_render_rect(psx_gpu_t* gpu, rect_data_t data) {
    uint16_t width, height;

    gpu_offset_census(gpu);

    switch ((data.attrib >> 3) & 3) {
        case RS_VARIABLE: { width = data.width; height = data.height; } break;
        case RS_1X1     : { width = 1         ; height = 1          ; } break;
        case RS_8X8     : { width = 8         ; height = 8          ; } break;
        case RS_16X16   : { width = 16        ; height = 16         ; } break;
    }

    int textured = (data.attrib & RA_TEXTURED) != 0;
    /* Per-PRIMITIVE default; reset into `transp` per pixel (see gpu_render_triangle). */
    const int transp_default = (data.attrib & RA_TRANSP) != 0;
    int transp_mode = (gpu->gpustat >> 5) & 3;

    int clutx = (data.clut & 0x3f) << 4;
    int cluty = (data.clut >> 6) & 0x1ff;

    /* Offset coordinates */
    data.v0.x += gpu->off_x;
    data.v0.y += gpu->off_y;
    data.v0.x = SE10(data.v0.x);
    data.v0.y = SE10(data.v0.y);

    /* Calculate bounding box */
    int xmax = data.v0.x + width;
    int ymax = data.v0.y + height;

    xmax = CLAMP(xmax, -1024, 1024);
    ymax = CLAMP(ymax, -1024, 1024);
    data.v0.x = CLAMP(data.v0.x, -1024, 1024);
    data.v0.y = CLAMP(data.v0.y, -1024, 1024);

    PSX_PERF_RASTER(PSX_PERF_PRIM_RECT, xmax - data.v0.x, ymax - data.v0.y);

    int32_t xc = 0, yc = 0;

    const int mask_check = psx_gpu_mask_check(gpu);
    const uint16_t mask_set = psx_gpu_mask_set(gpu) ? 0x8000 : 0x0000;
    /* GP0(E6) bit 0 == 0 means "take the written mask bit from the texel" (gpu.h). */
    const uint16_t mask_from_texel = psx_gpu_mask_from_texel(gpu);

    for (int16_t y = data.v0.y; y < ymax; y++) {
        for (int16_t x = data.v0.x; x < xmax; x++) {
            /* PER-PIXEL, and this reset is the whole point.

               `transp` used to be declared once per primitive and then ASSIGNED inside this
               loop from the texel's bit 15. The first texel with that bit clear latched it to
               0 for every remaining pixel of the primitive, so a semi-transparent sprite drew
               correctly until its first opaque texel and turned solid from there on — a hard
               rectangle exactly the size of the sprite. That is Silent Hill's box around the
               player, and it reproduced on both rasterizers because both had the bug.
               (That was ONE of Silent Hill's boxes. The other was the dropped texel mask
               bit — see `stp` just below.) */
            int transp = transp_default;
            /* The source pixel's mask bit; see psx_gpu_mask_from_texel(). */
            uint16_t stp = 0;
            int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                     (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

            if (!bc)
                goto skip;

            if (mask_check && (gpu->vram[x + (y * 1024)] & 0x8000))
                goto skip;

            uint16_t color;

            if (textured) {
                uint16_t texel = gpu_fetch_texel(
                    gpu,
                    data.v0.tx + xc, data.v0.ty + yc,
                    gpu->texp_x, gpu->texp_y,
                    clutx, cluty,
                    gpu->texp_d
                );

                if (!texel)
                    goto skip;

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
                uint16_t back = gpu->vram[x + (y * 1024)];

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

            /* `force_mask || texel_bit15` — see the matching write in gpu_render_triangle. */
            gpu->vram[x + (y * 1024)] = color | mask_set | (stp & mask_from_texel);

            skip:

            ++xc;
        }

        xc = 0;

        ++yc;
    }
}

void plotLineLow(psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
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
        int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                 (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

        if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0) && bc)
            gpu->vram[x + (y * 1024)] = color;

        if (d > 0) {
            y += yi;
            d += (2 * (dy - dx));
        } else {
            d += 2*dy;
        }
    }
}

void plotLineHigh(psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
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
        int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                 (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

        if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0) && bc)
            gpu->vram[x + (y * 1024)] = color;

        if (d > 0) {
            x = x + xi;
            d += (2 * (dx - dy));
        } else {
            d += 2*dx;
        }
    }
}

void plotLine(psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1) {
            plotLineLow(gpu, x1, y1, x0, y0, color);
        } else {
            plotLineLow(gpu, x0, y0, x1, y1, color);
        }
    } else {
        if (y0 > y1) {
            plotLineHigh(gpu, x1, y1, x0, y0, color);
        } else {
            plotLineHigh(gpu, x0, y0, x1, y1, color);
        }
    }
}

void gpu_render_flat_line(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, uint32_t color) {
    gpu_offset_census(gpu);

    v0.x += gpu->off_x;
    v0.y += gpu->off_y;
    v1.x += gpu->off_x;
    v1.y += gpu->off_y;

    /* Bresenham steps the longer axis once per pixel, so that span IS the plot's cost. */
    PSX_PERF_RASTER(PSX_PERF_PRIM_LINE,
        max(abs(v1.x - v0.x), abs(v1.y - v0.y)), 1);

    plotLine(gpu, v0.x, v0.y, v1.x, v1.y, color);
}

void gpu_render_flat_rectangle(psx_gpu_t* gpu, vertex_t v, uint32_t w, uint32_t h, uint32_t color) {
    /* Offset coordinates */
    v.x += gpu->off_x;
    v.y += gpu->off_y;

    /* Calculate bounding box */
    int xmin = max(v.x, gpu->draw_x1);
    int ymin = max(v.y, gpu->draw_y1);
    int xmax = min(xmin + w, gpu->draw_x2);
    int ymax = min(ymin + h, gpu->draw_y2);

    PSX_PERF_RASTER(PSX_PERF_PRIM_RECT, xmax - xmin, ymax - ymin);

    GPU_HW_DEBUG(
        "soft-flat-rect color=%08x origin=(%d,%d) size=%ux%u bounds=(%d,%d)-(%d,%d)",
        color,
        v.x,
        v.y,
        w,
        h,
        xmin,
        ymin,
        xmax,
        ymax
    );

    for (uint32_t y = ymin; y < ymax; y++) {
        for (uint32_t x = xmin; x < xmax; x++) {
            int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                     (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

            if (!bc)
                continue;

            gpu->vram[x + (y * 1024)] = color;
        }
    }
}

void gpu_render_textured_rectangle(psx_gpu_t* gpu, vertex_t v, uint32_t w, uint32_t h, uint16_t clutx, uint16_t cluty, uint32_t color) {
    vertex_t a = v;

    a.x += gpu->off_x;
    a.y += gpu->off_y;

    int xmin = max(a.x, gpu->draw_x1);
    int ymin = max(a.y, gpu->draw_y1);
    int xmax = min(xmin + w, gpu->draw_x2);
    int ymax = min(ymin + h, gpu->draw_y2);

    uint32_t xc = 0, yc = 0;

    PSX_PERF_RASTER(PSX_PERF_PRIM_RECT, xmax - xmin, ymax - ymin);

    GPU_HW_DEBUG(
        "soft-textured-rect color=%08x origin=(%d,%d) size=%ux%u page=(%u,%u) clut=(%u,%u) bounds=(%d,%d)-(%d,%d)",
        color,
        v.x,
        v.y,
        w,
        h,
        gpu->texp_x,
        gpu->texp_y,
        clutx,
        cluty,
        xmin,
        ymin,
        xmax,
        ymax
    );

    for (int y = ymin; y < ymax; y++) {
        for (int x = xmin; x < xmax; x++) {
            uint16_t texel = gpu_fetch_texel(
                gpu,
                a.tx + xc, a.ty + yc,
                gpu->texp_x, gpu->texp_y,
                clutx, cluty,
                gpu->texp_d
            );

            ++xc;

            gpu->vram[x + (y * 1024)] = texel;
        }

        xc = 0;

        ++yc;
    }
}

void gpu_render_flat_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t color) {
    vertex_t a, b, c;

    a = v0;

    /* Ensure the winding order is correct */
    if (EDGE(v0, v1, v2) < 0) {
        b = v2;
        c = v1;
    } else {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x;
    a.y += gpu->off_y;
    b.x += gpu->off_x;
    b.y += gpu->off_y;
    c.x += gpu->off_x;
    c.y += gpu->off_y;

    int xmin = max(min(min(a.x, b.x), c.x), gpu->draw_x1);
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_y1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2); 
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_y2);

    PSX_PERF_RASTER(PSX_PERF_PRIM_TRIANGLE, xmax - xmin, ymax - ymin);

    GPU_HW_DEBUG(
        "soft-flat-tri color=%08x bounds=(%d,%d)-(%d,%d) vertices=(%d,%d)-(%d,%d)-(%d,%d)",
        color,
        xmin,
        ymin,
        xmax,
        ymax,
        a.x,
        a.y,
        b.x,
        b.y,
        c.x,
        c.y
    );

    for (int y = ymin; y < ymax; y++) {
        for (int x = xmin; x < xmax; x++) {
            int z0 = ((b.x - a.x) * (y - a.y)) - ((b.y - a.y) * (x - a.x));
            int z1 = ((c.x - b.x) * (y - b.y)) - ((c.y - b.y) * (x - b.x));
            int z2 = ((a.x - c.x) * (y - c.y)) - ((a.y - c.y) * (x - c.x));

            if ((z0 >= 0) && (z1 >= 0) && (z2 >= 0)) {
                gpu->vram[x + (y * 1024)] = BGR555(color);
            }
        }
    }
}

void gpu_render_shaded_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2) {
    vertex_t a, b, c, p;

    a = v0;

    /* Ensure the winding order is correct */
    if (EDGE(v0, v1, v2) < 0) {
        b = v2;
        c = v1;
    } else {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x;
    a.y += gpu->off_y;
    b.x += gpu->off_x;
    b.y += gpu->off_y;
    c.x += gpu->off_x;
    c.y += gpu->off_y;

    int xmin = max(min(min(a.x, b.x), c.x), gpu->draw_x1);
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_y1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2); 
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_y2);

    int area = EDGE(a, b, c);

    PSX_PERF_RASTER(PSX_PERF_PRIM_TRIANGLE, xmax - xmin, ymax - ymin);

    GPU_HW_DEBUG(
        "soft-shaded-tri bounds=(%d,%d)-(%d,%d) area=%d v0=(%d,%d c=%08x) v1=(%d,%d c=%08x) v2=(%d,%d c=%08x)",
        xmin,
        ymin,
        xmax,
        ymax,
        area,
        a.x,
        a.y,
        a.c,
        b.x,
        b.y,
        b.c,
        c.x,
        c.y,
        c.c
    );

    for (int y = ymin; y < ymax; y++) {
        for (int x = xmin; x < xmax; x++) {
            p.x = x;
            p.y = y;

            float z0 = EDGE((float)b, (float)c, (float)p);
            float z1 = EDGE((float)c, (float)a, (float)p);
            float z2 = EDGE((float)a, (float)b, (float)p);

            if ((z0 >= 0) && (z1 >= 0) && (z2 >= 0)) {
                int cr = (z0 * ((a.c >>  0) & 0xff) + z1 * ((b.c >>  0) & 0xff) + z2 * ((c.c >>  0) & 0xff)) / area;
                int cg = (z0 * ((a.c >>  8) & 0xff) + z1 * ((b.c >>  8) & 0xff) + z2 * ((c.c >>  8) & 0xff)) / area;
                int cb = (z0 * ((a.c >> 16) & 0xff) + z1 * ((b.c >> 16) & 0xff) + z2 * ((c.c >> 16) & 0xff)) / area;

                // Calculate positions within our 4x4 dither
                // kernel
                int dy = (y - ymin) % 4;
                int dx = (x - xmin) % 4;

                // Shift two pixels horizontally on the last
                // two scanlines?
                // if (dy > 1) {
                //     dx = ((x + 2) - xmin) % 4;
                // }

                int dither = g_psx_gpu_dither_kernel[dx + (dy * 4)];

                // Add to the original 8-bit color values
                cr += dither;
                cg += dither;
                cb += dither;

                // Saturate (clamp) to 00-ff
                cr = (cr >= 0xff) ? 0xff : ((cr <= 0) ? 0 : cr);
                cg = (cg >= 0xff) ? 0xff : ((cg <= 0) ? 0 : cg);
                cb = (cb >= 0xff) ? 0xff : ((cb <= 0) ? 0 : cb);

                uint32_t color = (cb << 16) | (cg << 8) | cr;

                gpu->vram[x + (y * 1024)] = BGR555(color);
            }
        }
    }
}

void gpu_render_textured_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth) {
    vertex_t a, b, c;

    a = v0;

    /* Ensure the winding order is correct */
    if (EDGE(v0, v1, v2) < 0) {
        b = v2;
        c = v1;
    } else {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x;
    a.y += gpu->off_y;
    b.x += gpu->off_x;
    b.y += gpu->off_y;
    c.x += gpu->off_x;
    c.y += gpu->off_y;

    int xmin = max(min(min(a.x, b.x), c.x), gpu->draw_x1);
    int ymin = max(min(min(a.y, b.y), c.y), gpu->draw_y1);
    int xmax = min(max(max(a.x, b.x), c.x), gpu->draw_x2); 
    int ymax = min(max(max(a.y, b.y), c.y), gpu->draw_y2);

    uint32_t area = EDGE(a, b, c);

    PSX_PERF_RASTER(PSX_PERF_PRIM_TRIANGLE, xmax - xmin, ymax - ymin);

    GPU_HW_DEBUG(
        "soft-textured-tri page=(%u,%u) clut=(%u,%u) depth=%d bounds=(%d,%d)-(%d,%d) area=%u v0=(%d,%d c=%08x tx=%u ty=%u) v1=(%d,%d c=%08x tx=%u ty=%u) v2=(%d,%d c=%08x tx=%u ty=%u)",
        tpx,
        tpy,
        clutx,
        cluty,
        depth,
        xmin,
        ymin,
        xmax,
        ymax,
        area,
        a.x,
        a.y,
        a.c,
        a.tx,
        a.ty,
        b.x,
        b.y,
        b.c,
        b.tx,
        b.ty,
        c.x,
        c.y,
        c.c,
        c.tx,
        c.ty
    );

    for (int y = ymin; y < ymax; y++) {
        for (int x = xmin; x < xmax; x++) {
            vertex_t p;

            p.x = x;
            p.y = y;

            float z0 = EDGE((float)b, (float)c, (float)p);
            float z1 = EDGE((float)c, (float)a, (float)p);
            float z2 = EDGE((float)a, (float)b, (float)p);

            if ((z0 >= 0) && (z1 >= 0) && (z2 >= 0)) {
                uint32_t tx = ((z0 * a.tx) + (z1 * b.tx) + (z2 * c.tx)) / area;
                uint32_t ty = ((z0 * a.ty) + (z1 * b.ty) + (z2 * c.ty)) / area;

                uint16_t color = gpu_fetch_texel(
                    gpu,
                    tx, ty,
                    tpx, tpy,
                    clutx, cluty,
                    depth
                );

                if (!color) continue;

                gpu->vram[x + (y * 1024)] = color;
            }
        }
    }
}

#define I32(v, b) (((int32_t)((v) << (31-b))) >> (31-b))

void gpu_rect(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;

            int size = (gpu->buf[0] >> 27) & 3;
            int textured = (gpu->buf[0] & 0x04000000) != 0;

            GPU_HW_DEBUG(
                "rect-cmd raw=0x%08x attrib=0x%02x size=%d textured=%s",
                gpu->buf[0],
                gpu->buf[0] >> 24,
                size,
                textured ? "true" : "false"
            );

            gpu->cmd_args_remaining = 1 + (size == RS_VARIABLE) + textured;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                rect_data_t rect;

                rect.attrib = gpu->buf[0] >> 24;

                int textured = (rect.attrib & RA_TEXTURED) != 0;
                int raw      = (rect.attrib & RA_RAW) != 0;

                // Add 1 if is textured
                int size_offset = 2 + textured;

                /* PGXP applies to polygons only; keep the flag defined for
                   any backend that inspects it on the shared vertex_t. */
                rect.v0.precise_valid = 0;
                rect.v0.px = 0.0f;
                rect.v0.py = 0.0f;
                rect.v0.pw = 1.0f;

                rect.v0.c   = gpu->buf[0] & 0xffffff;
                rect.v0.x   = SE10(gpu->buf[1] & 0xffff);
                rect.v0.y   = SE10(gpu->buf[1] >> 16);
                rect.v0.tx  = (gpu->buf[2] >> 0) & 0xff;
                rect.v0.ty  = (gpu->buf[2] >> 8) & 0xff;
                rect.clut   = gpu->buf[2] >> 16;
                rect.width  = gpu->buf[size_offset] & 0xffff;
                rect.height = gpu->buf[size_offset] >> 16;

                if (textured && raw)
                    rect.v0.c = 0x808080;

                GPU_HW_DEBUG(
                    "rect-dispatch attrib=0x%02x v0=(%d,%d c=%08x tx=%u ty=%u) clut=0x%04x size=%ux%u raw=%s textured=%s",
                    rect.attrib,
                    rect.v0.x,
                    rect.v0.y,
                    rect.v0.c,
                    rect.v0.tx,
                    rect.v0.ty,
                    rect.clut,
                    rect.width,
                    rect.height,
                    raw ? "true" : "false",
                    textured ? "true" : "false"
                );

                if (gpu->dbg_file) {
                    gpu->dbg_prims++;
                    gpu_dump_rect(gpu, &rect);
                }

                /* Texture replacement, resolved ONCE here so the backend and the software
                   rasterizer below cannot reach different answers. Skipped entirely while
                   gpu->texrep is NULL, which is the shipping default. psx/texrep.h. */
                if (gpu->texrep && textured) {
                    /* Sprites carry no texpage word: gpu_render_rect() reads the latched
                       GPUSTAT page, and the size comes from the attrib exactly as it does
                       there. The UV range is the INCLUSIVE raw span the sprite sweeps, which
                       may run past 255 and wrap — psx_texrep_bind_prim() folds it. */
                    static const uint16_t kFixed[4] = { 0, 1, 8, 16 };
                    int rsz = (rect.attrib >> 3) & 3;
                    uint16_t rw = (rsz == RS_VARIABLE) ? (uint16_t)rect.width : kFixed[rsz];
                    uint16_t rh = (rsz == RS_VARIABLE) ? (uint16_t)rect.height : kFixed[rsz];
                    uint16_t su[2], sv[2];

                    if (rw && rh) {
                        su[0] = rect.v0.tx;
                        su[1] = (uint16_t)(rect.v0.tx + rw - 1u);
                        sv[0] = rect.v0.ty;
                        sv[1] = (uint16_t)(rect.v0.ty + rh - 1u);

                        psx_texrep_bind_prim(gpu, su, sv, 2,
                                             gpu->texp_x, gpu->texp_y,
                                             (rect.clut & 0x3f) << 4,
                                             (rect.clut >> 6) & 0x1ff,
                                             (int)gpu->texp_d);
                    }
                }

#ifdef USE_HARDWARE
                if (GPU_BACKEND_HAS(gpu, draw_rect))
                    gpu->backend->draw_rect(gpu->backend, gpu, &rect);

                if (!GPU_BACKEND_HAS(gpu, draw_rect) || GPU_BACKEND_SHADOWS(gpu))
#endif
                    gpu_render_rect(gpu, rect);

                psx_texrep_unbind(gpu);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_poly(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;

            int shaded   = (gpu->buf[0] & 0x10000000) != 0;
            int quad     = (gpu->buf[0] & 0x08000000) != 0;
            int textured = (gpu->buf[0] & 0x04000000) != 0;

            GPU_HW_DEBUG(
                "poly-cmd raw=0x%08x shaded=%s quad=%s textured=%s",
                gpu->buf[0],
                shaded ? "true" : "false",
                quad ? "true" : "false",
                textured ? "true" : "false"
            );

            int fields_per_vertex = 1 + shaded + textured;
            int vertices = 3 + quad;
 
            gpu->cmd_args_remaining = (fields_per_vertex * vertices) - shaded;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                poly_data_t poly;

                poly.attrib = gpu->buf[0] >> 24;

                int shaded   = (poly.attrib & PA_SHADED) != 0;
                int textured = (poly.attrib & PA_TEXTURED) != 0;

                int color_offset = shaded * (2 + textured);
                int vert_offset = 1 + (textured | shaded) +
                                      (textured & shaded);
                int texc_offset = textured * (2 + shaded);
                int texp_offset = textured * (4 + shaded);

                poly.clut = gpu->buf[2] >> 16;
                poly.texp = gpu->buf[texp_offset] >> 16;
                /* Read only by the GPU_HW_DEBUG() below, which compiles to nothing unless
                   HW_DEBUG is defined. Cast so the default build does not warn. */
                const bool poly_quad = (poly.attrib & PA_QUAD) != 0;
                (void)poly_quad;

                // Undocumented behavior?
                // Fixes Mortal Kombat II, Bubble Bobble, Driver 1 & 2
                if (textured) {
                    gpu->texp_x = (poly.texp & 0xf) << 6;
                    gpu->texp_y = (poly.texp & 0x10) << 4;
                    gpu->texp_d = (poly.texp >> 7) & 0x3;
                    gpu->gpustat &= 0xfffffe00;
                    gpu->gpustat |= poly.texp & 0x1ff;
                }

                GPU_HW_DEBUG(
                    "poly-dispatch attrib=0x%02x shaded=%s quad=%s textured=%s clut=0x%04x texp=0x%04x "
                    "v0=(%d,%d c=%08x tx=%u ty=%u) v1=(%d,%d c=%08x tx=%u ty=%u) v2=(%d,%d c=%08x tx=%u ty=%u) v3=(%d,%d c=%08x tx=%u ty=%u)",
                    poly.attrib,
                    shaded ? "true" : "false",
                    poly_quad ? "true" : "false",
                    textured ? "true" : "false",
                    poly.clut,
                    poly.texp,
                    poly.v[0].x, poly.v[0].y, poly.v[0].c, poly.v[0].tx, poly.v[0].ty,
                    poly.v[1].x, poly.v[1].y, poly.v[1].c, poly.v[1].tx, poly.v[1].ty,
                    poly.v[2].x, poly.v[2].y, poly.v[2].c, poly.v[2].tx, poly.v[2].ty,
                    poly.v[3].x, poly.v[3].y, poly.v[3].c, poly.v[3].tx, poly.v[3].ty
                );

                poly.v[0].c = gpu->buf[0+0*color_offset] & 0xffffff;
                poly.v[1].c = gpu->buf[0+1*color_offset] & 0xffffff;
                poly.v[2].c = gpu->buf[0+2*color_offset] & 0xffffff;
                poly.v[3].c = gpu->buf[0+3*color_offset] & 0xffffff;
                poly.v[0].x = SE10(gpu->buf[1+0*vert_offset] & 0xffff);
                poly.v[1].x = SE10(gpu->buf[1+1*vert_offset] & 0xffff);
                poly.v[2].x = SE10(gpu->buf[1+2*vert_offset] & 0xffff);
                poly.v[3].x = SE10(gpu->buf[1+3*vert_offset] & 0xffff);
                poly.v[0].y = SE10(gpu->buf[1+0*vert_offset] >> 16);
                poly.v[1].y = SE10(gpu->buf[1+1*vert_offset] >> 16);
                poly.v[2].y = SE10(gpu->buf[1+2*vert_offset] >> 16);
                poly.v[3].y = SE10(gpu->buf[1+3*vert_offset] >> 16);
                poly.v[0].tx = gpu->buf[2+0*texc_offset] & 0xff;
                poly.v[1].tx = gpu->buf[2+1*texc_offset] & 0xff;
                poly.v[2].tx = gpu->buf[2+2*texc_offset] & 0xff;
                poly.v[3].tx = gpu->buf[2+3*texc_offset] & 0xff;
                poly.v[0].ty = (gpu->buf[2+0*texc_offset] >> 8) & 0xff;
                poly.v[1].ty = (gpu->buf[2+1*texc_offset] >> 8) & 0xff;
                poly.v[2].ty = (gpu->buf[2+2*texc_offset] >> 8) & 0xff;
                poly.v[3].ty = (gpu->buf[2+3*texc_offset] >> 8) & 0xff;

                /* PGXP: attach precise GTE coordinates wherever the source RAM
                   word (noted by dma.c per buf slot) still matches the vertex
                   word parsed above. Runs for all four v[]s unconditionally —
                   on a miss, with PGXP off, or for the unused v[3] of a
                   triangle it just defines precise_valid=0, so the backends
                   never read uninitialized fields. */
                for (int pv = 0; pv < 4; pv++)
                    psx_pgxp_poly_vertex(&poly.v[pv],
                                         gpu->buf[1 + pv * vert_offset],
                                         1 + pv * vert_offset);

                /* AFTER the parse above. The GPU_HW_DEBUG line further up prints poly.v[]
                   before it is filled in, which is exactly the trap this avoids. */
                if (gpu->dbg_file) {
                    gpu->dbg_prims++;
                    gpu_dump_poly(gpu, &poly);
                }

                /* Texture replacement, resolved ONCE for the whole primitive — including
                   BOTH triangles of a quad, which is what makes a replaced quad seamless
                   instead of two halves keyed independently. Skipped entirely while
                   gpu->texrep is NULL, the shipping default. psx/texrep.h. */
                if (gpu->texrep && textured) {
                    int nv = (poly.attrib & PA_QUAD) ? 4 : 3;
                    uint16_t su[4], sv[4];
                    int pv;

                    for (pv = 0; pv < nv; pv++) {
                        su[pv] = (uint16_t)poly.v[pv].tx;
                        sv[pv] = (uint16_t)poly.v[pv].ty;
                    }

                    psx_texrep_bind_prim(gpu, su, sv, nv,
                                         (poly.texp & 0xf) << 6,
                                         (poly.texp & 0x10) << 4,
                                         (poly.clut & 0x3f) << 4,
                                         (poly.clut >> 6) & 0x1ff,
                                         (poly.texp >> 7) & 3);
                }

#ifdef USE_HARDWARE
                /* Backend first, software second: both then sample the same pre-write
                   VRAM for their texture fetches. */
                if (GPU_BACKEND_HAS(gpu, draw_poly))
                    gpu->backend->draw_poly(gpu->backend, gpu, &poly);

                if (!GPU_BACKEND_HAS(gpu, draw_poly) || GPU_BACKEND_SHADOWS(gpu))
#endif
                {
                    if (poly.attrib & PA_QUAD) {
                        gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 1);
                        gpu_render_triangle(gpu, poly.v[1], poly.v[2], poly.v[3], poly, 1);
                    } else {
                        gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 0);
                    }
                }

                psx_texrep_unbind(gpu);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_line(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;

            int shaded   = (gpu->buf[0] & 0x10000000) != 0;
            int polyline = (gpu->buf[0] & 0x08000000) != 0;

            gpu->cmd_args_remaining = polyline ? -1 : (shaded ? 3 : 2);
            gpu->line_done = 0;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (gpu->buf[0] & 0x08000000) {
                /* The word psx_gpu_write32() just stored. It clamps buf_index into range
                   before every store, so this lands on a real slot for any stream -- and
                   once a long polyline saturates, on the last slot, which is still the most
                   recent word. Re-derived defensively rather than trusted because buf_index
                   also comes back unvalidated from a save state, where 0 would read
                   buf[-1]. */
                int last = gpu->buf_index - 1;

                if (last < 0)
                    last = 0;
                if (last >= PSX_GPU_CMD_BUF_SIZE)
                    last = PSX_GPU_CMD_BUF_SIZE - 1;

                if ((gpu->buf[last] & 0xf000f000) == 0x50005000) {
                    gpu->state = GPU_STATE_RECV_CMD;

                    return;
                }

                /* -1 is a sentinel here, not a count, and psx_gpu_write32() decrements it
                   once per word. Re-pin it so a polyline that runs long cannot walk it down
                   to INT_MIN; nothing on this path reads the value. */
                gpu->cmd_args_remaining = -1;

                // int shaded = (gpu->buf[0] & 0x10000000) != 0;

                // if (shaded) {
                //     if (gpu->buf_index > 2) {

                //     }
                // }

                // if (gpu->buf_index > overflow) {
                //     vertex_t v0, v1;

                //     if (shaded) {
                //         v0.c = gpu->buf[0] & 0xffffff;
                //         v1.c = gpu->buf[4] & 0xffffff;
                //         v0.x = gpu->buf[1] & 0xffff;
                //         v0.y = gpu->buf[1] >> 16;
                //         v1.x = gpu->buf[3] & 0xffff;
                //         v1.y = gpu->buf[3] >> 16;
                //     } else {
                //         v0.c = gpu->buf[0] & 0xffffff;
                //         v1.c = gpu->buf[0] & 0xffffff;
                //         v0.x = gpu->buf[1] & 0xffff;
                //         v0.y = gpu->buf[1] >> 16;
                //         v1.x = gpu->buf[2] & 0xffff;
                //         v1.y = gpu->buf[2] >> 16;
                //     }

                //     gpu->prev_line_vertex = v1;

                //     gpu_render_flat_line(gpu, v0, v1, gpu->buf[0] & 0xffffff);

                //     gpu->buf_index = 1;
                // }
            } else if (!gpu->cmd_args_remaining) {
                vertex_t v0, v1;

                /* PGXP: lines stay integer in v1 (see design doc §PGXP). */
                v0.precise_valid = 0;
                v0.px = 0.0f; v0.py = 0.0f; v0.pw = 1.0f;
                v1.precise_valid = 0;
                v1.px = 0.0f; v1.py = 0.0f; v1.pw = 1.0f;

                if (gpu->buf[0] & 0x10000000) {
                    v0.c = gpu->buf[0] & 0xffffff;
                    v1.c = gpu->buf[2] & 0xffffff;
                    v0.x = gpu->buf[1] & 0xffff;
                    v0.y = gpu->buf[1] >> 16;
                    v1.x = gpu->buf[3] & 0xffff;
                    v1.y = gpu->buf[3] >> 16;
                } else {
                    v0.c = gpu->buf[0] & 0xffffff;
                    v1.c = gpu->buf[0] & 0xffffff;
                    v0.x = gpu->buf[1] & 0xffff;
                    v0.y = gpu->buf[1] >> 16;
                    v1.x = gpu->buf[2] & 0xffff;
                    v1.y = gpu->buf[2] >> 16;
                }

                if (gpu->dbg_file) {
                    gpu->dbg_prims++;
                    gpu_dump_prim_line(gpu, &v0, &v1, (uint8_t)(gpu->buf[0] >> 24));
                }

#ifdef USE_HARDWARE
                if (GPU_BACKEND_HAS(gpu, draw_line))
                    gpu->backend->draw_line(gpu->backend, gpu, &v0, &v1,
                                            BGR555(gpu->buf[0] & 0xffffff));

                if (!GPU_BACKEND_HAS(gpu, draw_line) || GPU_BACKEND_SHADOWS(gpu))
#endif
                    gpu_render_flat_line(gpu, v0, v1, BGR555(gpu->buf[0] & 0xffffff));

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_a0(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                // Save static data
                gpu->xpos = gpu->buf[1] & 0x3ff;
                gpu->ypos = (gpu->buf[1] >> 16) & 0x1ff;
                gpu->xsiz = gpu->buf[2] & 0xffff;
                gpu->ysiz = gpu->buf[2] >> 16;
                gpu->xsiz = ((gpu->xsiz - 1) & 0x3ff) + 1;
                gpu->ysiz = ((gpu->ysiz - 1) & 0x1ff) + 1;
                gpu->tsiz = ((gpu->xsiz * gpu->ysiz) + 1) & 0xfffffffe;
                gpu->addr = gpu->xpos + (gpu->ypos * 1024);

                PSX_PERF_ADD(gpu_vram_words, gpu->tsiz);
                gpu->xcnt = 0;
                gpu->ycnt = 0;

                if (gpu->dbg_file) {
                    gpu->dbg_prims++;
                    gpu->dbg_upload++;
                    gpu_dumpf(gpu, "%05d upload   GP0(A0) dst=(%u,%u) size=%ux%u words=%u",
                              gpu->dbg_prims, gpu->xpos, gpu->ypos,
                              gpu->xsiz, gpu->ysiz, gpu->tsiz);
                }
            }
        } break;

        case GPU_STATE_RECV_DATA: {
            unsigned int xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
            unsigned int ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;

            gpu->vram[xpos + (ypos * 1024)] = gpu->recv_data & 0xffff;

            ++gpu->xcnt;

            xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
            ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;

            if (gpu->xcnt == gpu->xsiz) {
                ++gpu->ycnt;
                gpu->xcnt = 0;

                ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;
                xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
            }

            gpu->vram[xpos + (ypos * 1024)] = gpu->recv_data >> 16;

            ++gpu->xcnt;
            
            if (gpu->xcnt == gpu->xsiz) {
                ++gpu->ycnt;
                gpu->xcnt = 0;

                xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
                ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;
            }

            gpu->tsiz -= 2;

            if (!gpu->tsiz) {
#ifdef USE_HARDWARE
                /* Once per transfer, not once per halfword: host VRAM already holds the
                   whole rectangle by now, so the backend just replicates it. */
                if (GPU_BACKEND_HAS(gpu, upload_vram))
                    gpu->backend->upload_vram(gpu->backend, gpu->xpos, gpu->ypos,
                                              gpu->xsiz, gpu->ysiz, gpu->vram, 1024);
#endif
                gpu->xcnt = 0;
                gpu->ycnt = 0;

                /* GP0(A0) has just rewritten a VRAM rectangle, and a texture key is over
                   VRAM CONTENT while the key cache is keyed on ADDRESSES. Streaming games
                   upload over the same page every few frames, so without this a replacement
                   would keep matching the texture that used to be there. psx/texrep.h. */
                psx_texrep_invalidate(gpu);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_28(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 4;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->v0.x = gpu->buf[1] & 0xffff;
                gpu->v0.y = gpu->buf[1] >> 16;
                gpu->v1.x = gpu->buf[2] & 0xffff;
                gpu->v1.y = gpu->buf[2] >> 16;
                gpu->v2.x = gpu->buf[3] & 0xffff;
                gpu->v2.y = gpu->buf[3] >> 16;
                gpu->v3.x = gpu->buf[4] & 0xffff;
                gpu->v3.y = gpu->buf[4] >> 16;
                gpu->color = gpu->buf[0] & 0xffffff;

                gpu_render_flat_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, gpu->color);
                gpu_render_flat_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, gpu->color);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_30(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 5;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->v0.c = gpu->buf[0] & 0xffffff;
                gpu->v0.x = gpu->buf[1] & 0xffff;
                gpu->v0.y = gpu->buf[1] >> 16;
                gpu->v1.c = gpu->buf[2] & 0xffffff;
                gpu->v1.x = gpu->buf[3] & 0xffff;
                gpu->v1.y = gpu->buf[3] >> 16;
                gpu->v2.c = gpu->buf[4] & 0xffffff;
                gpu->v2.x = gpu->buf[5] & 0xffff;
                gpu->v2.y = gpu->buf[5] >> 16;

                gpu_render_shaded_triangle(gpu, gpu->v0, gpu->v1, gpu->v2);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_38(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 7;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->v0.c = gpu->buf[0] & 0xffffff;
                gpu->v0.x = gpu->buf[1] & 0xffff;
                gpu->v0.y = gpu->buf[1] >> 16;
                gpu->v1.c = gpu->buf[2] & 0xffffff;
                gpu->v1.x = gpu->buf[3] & 0xffff;
                gpu->v1.y = gpu->buf[3] >> 16;
                gpu->v2.c = gpu->buf[4] & 0xffffff;
                gpu->v2.x = gpu->buf[5] & 0xffff;
                gpu->v2.y = gpu->buf[5] >> 16;
                gpu->v3.c = gpu->buf[6] & 0xffffff;
                gpu->v3.x = gpu->buf[7] & 0xffff;
                gpu->v3.y = gpu->buf[7] >> 16;

                gpu_render_shaded_triangle(gpu, gpu->v0, gpu->v1, gpu->v2);
                gpu_render_shaded_triangle(gpu, gpu->v1, gpu->v2, gpu->v3);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_3c(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 11;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                uint32_t texp = gpu->buf[5] >> 16;
                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->pal   = gpu->buf[2] >> 16;
                gpu->v0.tx = gpu->buf[2] & 0xff;
                gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
                gpu->v1.tx = gpu->buf[5] & 0xff;
                gpu->v1.ty = (gpu->buf[5] >> 8) & 0xff;
                gpu->v2.tx = gpu->buf[8] & 0xff;
                gpu->v2.ty = (gpu->buf[8] >> 8) & 0xff;
                gpu->v3.tx = gpu->buf[11] & 0xff;
                gpu->v3.ty = (gpu->buf[11] >> 8) & 0xff;
                gpu->v0.x = gpu->buf[1] & 0xffff;
                gpu->v0.y = gpu->buf[1] >> 16;
                gpu->v1.x = gpu->buf[4] & 0xffff;
                gpu->v1.y = gpu->buf[4] >> 16;
                gpu->v2.x = gpu->buf[7] & 0xffff;
                gpu->v2.y = gpu->buf[7] >> 16;
                gpu->v3.x = gpu->buf[10] & 0xffff;
                gpu->v3.y = gpu->buf[10] >> 16;

                uint16_t clutx = (gpu->pal & 0x3f) << 4;
                uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
                uint16_t tpx = (texp & 0xf) << 6;
                uint16_t tpy = (texp & 0x10) << 4;
                uint16_t depth = (texp >> 7) & 0x3;

                gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
                gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_2c(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 8;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                uint32_t texp = gpu->buf[4] >> 16;
                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->pal   = gpu->buf[2] >> 16;
                gpu->v0.tx = gpu->buf[2] & 0xff;
                gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
                gpu->v1.tx = gpu->buf[4] & 0xff;
                gpu->v1.ty = (gpu->buf[4] >> 8) & 0xff;
                gpu->v2.tx = gpu->buf[6] & 0xff;
                gpu->v2.ty = (gpu->buf[6] >> 8) & 0xff;
                gpu->v3.tx = gpu->buf[8] & 0xff;
                gpu->v3.ty = (gpu->buf[8] >> 8) & 0xff;
                gpu->v0.x = gpu->buf[1] & 0xffff;
                gpu->v0.y = gpu->buf[1] >> 16;
                gpu->v1.x = gpu->buf[3] & 0xffff;
                gpu->v1.y = gpu->buf[3] >> 16;
                gpu->v2.x = gpu->buf[5] & 0xffff;
                gpu->v2.y = gpu->buf[5] >> 16;
                gpu->v3.x = gpu->buf[7] & 0xffff;
                gpu->v3.y = gpu->buf[7] >> 16;

                uint16_t clutx = (gpu->pal & 0x3f) << 4;
                uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
                uint16_t tpx = (texp & 0xf) << 6;
                uint16_t tpy = (texp & 0x10) << 4;
                uint16_t depth = (texp >> 7) & 0x3;

                gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
                gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_24(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 6;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                uint32_t texp = gpu->buf[4] >> 16;
                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->pal   = gpu->buf[2] >> 16;
                gpu->v0.tx = gpu->buf[2] & 0xff;
                gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
                gpu->v1.tx = gpu->buf[4] & 0xff;
                gpu->v1.ty = (gpu->buf[4] >> 8) & 0xff;
                gpu->v2.tx = gpu->buf[6] & 0xff;
                gpu->v2.ty = (gpu->buf[6] >> 8) & 0xff;
                gpu->v0.x = gpu->buf[1] & 0xffff;
                gpu->v0.y = gpu->buf[1] >> 16;
                gpu->v1.x = gpu->buf[3] & 0xffff;
                gpu->v1.y = gpu->buf[3] >> 16;
                gpu->v2.x = gpu->buf[5] & 0xffff;
                gpu->v2.y = gpu->buf[5] >> 16;

                uint16_t clutx = (gpu->pal & 0x3f) << 4;
                uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
                uint16_t tpx = (texp & 0xf) << 6;
                uint16_t tpy = (texp & 0x10) << 4;
                uint16_t depth = (texp >> 7) & 0x3;

                gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
                gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

// Monochrome Opaque Quadrilateral
void gpu_cmd_2d(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 8;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                uint32_t texp = gpu->buf[4] >> 16;
                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->pal   = gpu->buf[2] >> 16;
                gpu->v0.tx = gpu->buf[2] & 0xff;
                gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
                gpu->v1.tx = gpu->buf[4] & 0xff;
                gpu->v1.ty = (gpu->buf[4] >> 8) & 0xff;
                gpu->v2.tx = gpu->buf[6] & 0xff;
                gpu->v2.ty = (gpu->buf[6] >> 8) & 0xff;
                gpu->v3.tx = gpu->buf[8] & 0xff;
                gpu->v3.ty = (gpu->buf[8] >> 8) & 0xff;
                gpu->v0.x = gpu->buf[1] & 0xffff;
                gpu->v0.y = gpu->buf[1] >> 16;
                gpu->v1.x = gpu->buf[3] & 0xffff;
                gpu->v1.y = gpu->buf[3] >> 16;
                gpu->v2.x = gpu->buf[5] & 0xffff;
                gpu->v2.y = gpu->buf[5] >> 16;
                gpu->v3.x = gpu->buf[7] & 0xffff;
                gpu->v3.y = gpu->buf[7] >> 16;

                uint16_t clutx = (gpu->pal & 0x3f) << 4;
                uint16_t cluty = (gpu->pal >> 6) & 0x1ff;
                uint16_t tpx = (texp & 0xf) << 6;
                uint16_t tpy = (texp & 0x10) << 4;
                uint16_t depth = (texp >> 7) & 0x3;

                gpu_render_textured_triangle(gpu, gpu->v0, gpu->v1, gpu->v2, tpx, tpy, clutx, cluty, depth);
                gpu_render_textured_triangle(gpu, gpu->v1, gpu->v2, gpu->v3, tpx, tpy, clutx, cluty, depth);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_64(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 3;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = gpu->buf[1] & 0xffff;
                gpu->v0.y  = gpu->buf[1] >> 16;
                gpu->v0.tx = gpu->buf[2] & 0xff;
                gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
                gpu->pal   = gpu->buf[2] >> 16;

                uint32_t w = gpu->buf[3] & 0xffff;
                uint32_t h = gpu->buf[3] >> 16;
                uint16_t clutx = (gpu->pal & 0x3f) << 4;
                uint16_t cluty = (gpu->pal >> 6) & 0x1ff;

                gpu_render_textured_rectangle(gpu, gpu->v0, w, h, clutx, cluty, gpu->color);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_7c(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = gpu->buf[1] & 0xffff;
                gpu->v0.y  = gpu->buf[1] >> 16;
                gpu->v0.tx = gpu->buf[2] & 0xff;
                gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
                gpu->pal   = gpu->buf[2] >> 16;

                uint32_t w = 16;
                uint32_t h = 16;
                uint16_t clutx = (gpu->pal & 0x3f) << 4;
                uint16_t cluty = (gpu->pal >> 6) & 0x1ff;

                gpu_render_textured_rectangle(gpu, gpu->v0, w, h, clutx, cluty, gpu->color);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_74(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = gpu->buf[1] & 0xffff;
                gpu->v0.y  = gpu->buf[1] >> 16;
                gpu->v0.tx = gpu->buf[2] & 0xff;
                gpu->v0.ty = (gpu->buf[2] >> 8) & 0xff;
                gpu->pal   = gpu->buf[2] >> 16;

                uint32_t w = 8;
                uint32_t h = 8;
                uint16_t clutx = (gpu->pal & 0x3f) << 4;
                uint16_t cluty = (gpu->pal >> 6) & 0x1ff;

                gpu_render_textured_rectangle(gpu, gpu->v0, w, h, clutx, cluty, gpu->color);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_60(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = gpu->buf[1] & 0xffff;
                gpu->v0.y  = gpu->buf[1] >> 16;
                gpu->xsiz  = gpu->buf[2] & 0xffff;
                gpu->ysiz  = gpu->buf[2] >> 16;

                gpu->v0.x += gpu->off_x;
                gpu->v0.y += gpu->off_y;

                gpu_render_flat_rectangle(gpu, gpu->v0, gpu->xsiz, gpu->ysiz, BGR555(gpu->color));

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_68(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 1;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = gpu->buf[1] & 0xffff;
                gpu->v0.y  = gpu->buf[1] >> 16;

                /*
                    GP0(68h) IS a 1x1 rectangle, so it goes through the rectangle path like
                    every other primitive instead of writing gpu->vram directly.

                    The direct write it replaces diverged three ways at once:
                      - no SE10() on the offset coordinates, so an 11-bit signed coordinate
                        that should wrap instead indexed straight out of the 1024x512 array;
                      - no clip to the drawing area, so it drew where nothing may draw;
                      - no backend hook, so the GL rasterizer never learned the pixel existed.
                    The third is invisible while the software shadow still backs VRAM and
                    becomes a missing pixel the moment the shadow is removed.

                    Note the offset is NOT pre-added here any more: gpu_render_rect() adds
                    off_x/off_y itself, and doing it in both places double-shifted the dot.
                */
                {
                    rect_data_t rect;

                    rect.attrib = (uint8_t)(RS_1X1 << 3);
                    rect.v0 = gpu->v0;
                    rect.clut = 0;
                    rect.width = 1;
                    rect.height = 1;

#ifdef USE_HARDWARE
                    if (GPU_BACKEND_HAS(gpu, draw_rect))
                        gpu->backend->draw_rect(gpu->backend, gpu, &rect);

                    if (!GPU_BACKEND_HAS(gpu, draw_rect) || GPU_BACKEND_SHADOWS(gpu))
#endif
                        gpu_render_rect(gpu, rect);
                }

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_40(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = gpu->buf[1] & 0xffff;
                gpu->v0.y  = gpu->buf[1] >> 16;
                gpu->v1.x  = gpu->buf[2] & 0xffff;
                gpu->v1.y  = gpu->buf[2] >> 16;

                gpu_render_flat_line(gpu, gpu->v0, gpu->v1, BGR555(gpu->color));

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_c0(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->c0_xcnt = 0;
                gpu->c0_ycnt = 0;
                uint32_t c0_xpos = gpu->buf[1] & 0xffff;
                uint32_t c0_ypos = gpu->buf[1] >> 16;
                gpu->c0_xsiz = gpu->buf[2] & 0xffff;
                gpu->c0_ysiz = gpu->buf[2] >> 16;
                c0_xpos = c0_xpos & 0x3ff;
                c0_ypos = c0_ypos & 0x1ff;
                gpu->c0_xsiz = ((gpu->c0_xsiz - 1) & 0x3ff) + 1;
                gpu->c0_ysiz = ((gpu->c0_ysiz - 1) & 0x1ff) + 1;
                gpu->c0_tsiz = ((gpu->c0_xsiz * gpu->c0_ysiz) + 1) & 0xfffffffe;
                gpu->c0_addr = c0_xpos + (c0_ypos * 1024);

                PSX_PERF_ADD(gpu_vram_words, gpu->c0_tsiz);

                if (gpu->dbg_file) {
                    gpu->dbg_prims++;
                    gpu->dbg_download++;
                    gpu_dumpf(gpu, "%05d download GP0(C0) src=(%u,%u) size=%dx%d",
                              gpu->dbg_prims, c0_xpos, c0_ypos,
                              gpu->c0_xsiz, gpu->c0_ysiz);
                }

#ifdef USE_HARDWARE
                /* Before psx_gpu_read32() starts draining (gpu.c:98), so the drain sees
                   valid host data. A software-shadow backend leaves this NULL — gpu->vram
                   is already correct and there is no readback stall. */
                if (GPU_BACKEND_HAS(gpu, download_vram))
                    gpu->backend->download_vram(gpu->backend, c0_xpos, c0_ypos,
                                                gpu->c0_xsiz, gpu->c0_ysiz,
                                                gpu->vram, 1024);
#endif

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_02(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = gpu->buf[1] & 0xffff;
                gpu->v0.y  = gpu->buf[1] >> 16;
                gpu->xsiz  = gpu->buf[2] & 0xffff;
                gpu->ysiz  = gpu->buf[2] >> 16;

                gpu->v0.x = (gpu->v0.x & 0x3f0);
                gpu->v0.y = gpu->v0.y & 0x1ff;
                gpu->xsiz = (((gpu->xsiz & 0x3ff) + 0x0f) & 0xfffffff0);
                gpu->ysiz = gpu->ysiz & 0x1ff;

                uint16_t color = BGR555(gpu->color);

                PSX_PERF_ADD(gpu_vram_words, (uint64_t)gpu->xsiz * (uint64_t)gpu->ysiz);

                if (gpu->dbg_file) {
                    gpu->dbg_prims++;
                    gpu->dbg_fill++;

                    /* Summary mode keeps only the LARGEST fill of the frame: a whole-screen
                       clear colour that swings between frames is the cheapest possible
                       explanation for a whole-screen colour that swings between frames. */
                    if ((int)(gpu->xsiz * gpu->ysiz) > gpu->dbg_fill_area) {
                        gpu->dbg_fill_area = (int)(gpu->xsiz * gpu->ysiz);
                        gpu->dbg_fill_c = gpu->color;
                    }

                    gpu_dumpf(gpu,
                        "%05d fill     GP0(02) at=(%d,%d) size=%ux%u c=%06x bgr555=%04x "
                        "(ignores draw area and mask bit)",
                        gpu->dbg_prims, gpu->v0.x, gpu->v0.y,
                        gpu->xsiz, gpu->ysiz, gpu->color, color);
                }

                // printf("02 draw=(%u,%u-%u,%u) v0=(%u,%u) siz=(%u,%u)\n",
                //     gpu->draw_x1,
                //     gpu->draw_y1,
                //     gpu->draw_x2,
                //     gpu->draw_y2,
                //     gpu->v0.x,
                //     gpu->v0.y,
                //     gpu->xsiz,
                //     gpu->ysiz
                // );

#ifdef USE_HARDWARE
                if (GPU_BACKEND_HAS(gpu, fill_vram))
                    gpu->backend->fill_vram(gpu->backend, gpu->v0.x, gpu->v0.y,
                                            gpu->xsiz, gpu->ysiz, color);
#endif

                for (int y = gpu->v0.y; y < (gpu->v0.y + gpu->ysiz); y++) {
                    for (int x = gpu->v0.x; x < (gpu->v0.x + gpu->xsiz); x++) {
                        // This shouldn't be needed
                        // int bc = (x >= gpu->draw_x1) && (x <= gpu->draw_x2) &&
                        //          (y >= gpu->draw_y1) && (y <= gpu->draw_y2);

                        // if (!bc)
                        //     continue;

                        if ((x < 1024) && (y < 512) && (x >= 0) && (y >= 0))
                            gpu->vram[x + (y * 1024)] = color;
                    }
                }

                psx_texrep_invalidate(gpu);   /* GP0(02) filled VRAM; see gpu_cmd_a0. */

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_80(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 3;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                uint32_t srcx = gpu->buf[1] & 0xffff;
                uint32_t srcy = gpu->buf[1] >> 16;
                uint32_t dstx = gpu->buf[2] & 0xffff;
                uint32_t dsty = gpu->buf[2] >> 16;
                uint32_t xsiz = gpu->buf[3] & 0xffff;
                uint32_t ysiz = gpu->buf[3] >> 16;

                /* A copy touches the rectangle twice (read + write). */
                PSX_PERF_ADD(gpu_vram_words, 2ull * (uint64_t)xsiz * (uint64_t)ysiz);

                if (gpu->dbg_file) {
                    gpu->dbg_prims++;
                    gpu->dbg_copy++;
                    gpu_dumpf(gpu, "%05d copy     GP0(80) src=(%u,%u) dst=(%u,%u) size=%ux%u",
                              gpu->dbg_prims, srcx, srcy, dstx, dsty, xsiz, ysiz);
                }

#ifdef USE_HARDWARE
                /* Runs before the host copy so a backend that reads gpu->vram for its
                   source still sees the pre-copy contents. */
                if (GPU_BACKEND_HAS(gpu, copy_vram))
                    gpu->backend->copy_vram(gpu->backend, srcx, srcy, dstx, dsty, xsiz, ysiz);
#endif

                for (int y = 0; y < ysiz; y++) {
                    for (int x = 0; x < xsiz; x++) {
                        int dstb = ((dstx + x) < 1024) && ((dsty + y) < 512);
                        int srcb = ((srcx + x) < 1024) && ((srcy + y) < 512);
                        
                        if (dstb && srcb)
                            gpu->vram[(dstx + x) + (dsty + y) * 1024] = gpu->vram[(srcx + x) + (srcy + y) * 1024];
                    }
                }

                psx_texrep_invalidate(gpu);   /* GP0(80) moved VRAM; see gpu_cmd_a0. */

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void psx_gpu_update_cmd(psx_gpu_t* gpu) {
    int type = (gpu->buf[0] >> 29) & 7;

    switch (type) {
        case 1: gpu_poly(gpu); return;
        case 2: gpu_line(gpu); return;
        case 3: gpu_rect(gpu); return;
    }

    switch (gpu->buf[0] >> 24) {
        case 0x00: /* nop */ break;
        case 0x01: /* Cache clear */ break;
        case 0x02: gpu_cmd_02(gpu); break;
        case 0x24: gpu_cmd_24(gpu); break;
        case 0x25: gpu_cmd_24(gpu); break;
        case 0x26: gpu_cmd_24(gpu); break;
        case 0x27: gpu_cmd_24(gpu); break;
        case 0x28: gpu_cmd_28(gpu); break;
        case 0x2a: gpu_cmd_28(gpu); break;
        case 0x2c: gpu_cmd_2d(gpu); break;
        case 0x2d: gpu_cmd_2d(gpu); break;
        case 0x2e: gpu_cmd_2d(gpu); break;
        case 0x2f: gpu_cmd_2d(gpu); break;
        case 0x30: gpu_cmd_30(gpu); break;
        case 0x32: gpu_cmd_30(gpu); break;
        case 0x38: gpu_cmd_38(gpu); break;
        case 0x3c: gpu_cmd_3c(gpu); break;
        case 0x3e: gpu_cmd_3c(gpu); break;
        case 0x40: gpu_cmd_40(gpu); break;
        case 0x60: gpu_cmd_60(gpu); break;
        case 0x62: gpu_cmd_60(gpu); break;
        case 0x64: gpu_cmd_64(gpu); break;
        case 0x65: gpu_cmd_64(gpu); break;
        case 0x66: gpu_cmd_64(gpu); break;
        case 0x67: gpu_cmd_64(gpu); break;
        case 0x68: gpu_cmd_68(gpu); break;
        case 0x74: gpu_cmd_74(gpu); break;
        case 0x75: gpu_cmd_74(gpu); break;
        case 0x76: gpu_cmd_74(gpu); break;
        case 0x77: gpu_cmd_74(gpu); break;
        case 0x7c: gpu_cmd_7c(gpu); break;
        case 0x7d: gpu_cmd_7c(gpu); break;
        case 0x7e: gpu_cmd_7c(gpu); break;
        case 0x7f: gpu_cmd_7c(gpu); break;
        case 0x80: gpu_cmd_80(gpu); break;
        case 0xa0: gpu_cmd_a0(gpu); break;
        case 0xc0: gpu_cmd_c0(gpu); break;
        case 0xe1: {
            gpu->gpustat &= 0xfffff800;
            gpu->gpustat |= gpu->buf[0] & 0x7ff;
            gpu->texp_x = (gpu->gpustat & 0xf) << 6;
            gpu->texp_y = (gpu->gpustat & 0x10) << 4;
            gpu->texp_d = (gpu->gpustat >> 7) & 0x3;

            if (gpu->dbg_file)
                gpu_dumpf(gpu,
                    "%05d ..state  GP0(E1) texpage word=%06x tpage=(%u,%u) depth=%u[%s] "
                    "tmode=%u[%s] dither=%d",
                    gpu->dbg_prims, gpu->buf[0] & 0xffffff,
                    gpu->texp_x, gpu->texp_y,
                    gpu->texp_d, gpu_dump_depth_name((int)gpu->texp_d),
                    (gpu->gpustat >> 5) & 3, gpu_dump_tmode_name((gpu->gpustat >> 5) & 3),
                    (gpu->gpustat & 0x0200) ? 1 : 0);
        } break;
        case 0xe2: {
            gpu->texw_mx = ((gpu->buf[0] >> 0 ) & 0x1f) << 3;
            gpu->texw_my = ((gpu->buf[0] >> 5 ) & 0x1f) << 3;
            gpu->texw_ox = ((gpu->buf[0] >> 10) & 0x1f) << 3;
            gpu->texw_oy = ((gpu->buf[0] >> 15) & 0x1f) << 3;

            if (gpu->dbg_file)
                gpu_dumpf(gpu, "%05d ..state  GP0(E2) texwindow word=%06x mx=%u my=%u ox=%u oy=%u",
                          gpu->dbg_prims, gpu->buf[0] & 0xffffff,
                          gpu->texw_mx, gpu->texw_my, gpu->texw_ox, gpu->texw_oy);
        } break;
        case 0xe3: {
            gpu->draw_x1 = (gpu->buf[0] >> 0 ) & 0x3ff;
            gpu->draw_y1 = (gpu->buf[0] >> 10) & 0x1ff;

            if (gpu->dbg_file)
                gpu_dumpf(gpu, "%05d ..state  GP0(E3) draw_tl=(%u,%u) [draw=(%u,%u)-(%u,%u)]",
                          gpu->dbg_prims, gpu->draw_x1, gpu->draw_y1,
                          gpu->draw_x1, gpu->draw_y1, gpu->draw_x2, gpu->draw_y2);
        } break;
        case 0xe4: {
            gpu->draw_x2 = (gpu->buf[0] >> 0 ) & 0x3ff;
            gpu->draw_y2 = (gpu->buf[0] >> 10) & 0x1ff;

            if (gpu->dbg_file)
                gpu_dumpf(gpu, "%05d ..state  GP0(E4) draw_br=(%u,%u) [draw=(%u,%u)-(%u,%u)]",
                          gpu->dbg_prims, gpu->draw_x2, gpu->draw_y2,
                          gpu->draw_x1, gpu->draw_y1, gpu->draw_x2, gpu->draw_y2);
        } break;
        case 0xe5: {
#ifdef ARMSX_TEST_OFFSET_CENSUS
            /* Verbatim, before extraction. See the note on gp0_e5_raw in gpu.h. */
            gpu->gp0_e5_raw = gpu->buf[0] & 0xffffff;
            gpu->gp0_e5_count++;
#endif

            gpu->off_x = ((int32_t)(((gpu->buf[0] >> 0 ) & 0x7ff) << 21)) >> 21;
            gpu->off_y = ((int32_t)(((gpu->buf[0] >> 11) & 0x7ff) << 21)) >> 21;

            if (gpu->dbg_file)
                gpu_dumpf(gpu, "%05d ..state  GP0(E5) offset=(%d,%d)",
                          gpu->dbg_prims, gpu->off_x, gpu->off_y);
        } break;
        case 0xe6: {
            /* Bit 0 -> GPUSTAT bit 11 (set mask while drawing),
               bit 1 -> GPUSTAT bit 12 (check mask before draw).
               The latch is unconditional so GPUSTAT reads back correctly either way;
               whether the rasterizers ACT on it is gated by PSX_GPU_ACCURACY_MASK_BIT,
               which is off by default. */
            gpu->gpustat &= ~0x1800;
            gpu->gpustat |= (gpu->buf[0] & 0x3) << 11;

            if (gpu->dbg_file)
                gpu_dumpf(gpu, "%05d ..state  GP0(E6) mask set=%d check=%d (honoured=%s)",
                          gpu->dbg_prims,
                          (gpu->gpustat & 0x0800) ? 1 : 0,
                          (gpu->gpustat & 0x1000) ? 1 : 0,
                          (gpu->accuracy_flags & PSX_GPU_ACCURACY_MASK_BIT) ? "yes" : "no");
        } break;
        default: {
            // log_set_quiet(0);
            // log_fatal("Unhandled GP0(%02Xh)", gpu->buf[0] >> 24);
            // log_set_quiet(1);

            // exit(1);
        } break;
    }
}

void psx_gpu_write32(psx_gpu_t* gpu, uint32_t offset, uint32_t value) {
    switch (offset) {
        // GP0
        case 0x00: {
            switch (gpu->state) {
                case GPU_STATE_RECV_CMD: {
                    gpu->buf_index = 0;

                    /* PGXP: bind the DMA-noted source address (if any) to the
                       buf slot this word lands in; an MMIO write finds no note
                       pending and records "no address". */
                    if (psx_pgxp_active())
                        psx_pgxp_gp0_slot(0);

                    gpu->buf[gpu->buf_index++] = value;

                    psx_gpu_update_cmd(gpu);
                } break;

                case GPU_STATE_RECV_ARGS: {
                    /* A polyline (GP0 bit 27) has no argument count: gpu_line() parks
                       cmd_args_remaining at -1 and only the game's 0x50005000 terminator
                       ends the command, so this branch runs for EVERY following word with
                       nothing of its own to stop it. Unbounded, a polyline longer than
                       buf[] -- or a desynced stream where the terminator never arrives --
                       walks straight off the end into recv_data, then buf_index (which
                       hands the index itself to the command stream), then the command
                       counters, and on into off_x/off_y and the drawing area.

                       Saturate rather than drop: gpu_line() looks for the terminator in the
                       LAST slot written, so parking every overflow word in the final slot
                       keeps a long-but-well-formed polyline ending on its terminator
                       instead of sitting in RECV_ARGS until the next GP1 reset. Nothing
                       else can reach the clamp -- the longest fixed-length command,
                       GP0(3C), is 12 words. The low side is guarded too because buf_index
                       is restored verbatim from a save state. */
                    if (gpu->buf_index < 0 || gpu->buf_index >= PSX_GPU_CMD_BUF_SIZE)
                        gpu->buf_index = PSX_GPU_CMD_BUF_SIZE - 1;

                    if (psx_pgxp_active())
                        psx_pgxp_gp0_slot(gpu->buf_index);

                    gpu->buf[gpu->buf_index++] = value;
                    gpu->cmd_args_remaining--;

                    psx_gpu_update_cmd(gpu);
                } break;

                case GPU_STATE_RECV_DATA: {
                    /* PGXP: image-upload words never carry vertices; drop the
                       note so it cannot leak onto a later command word. */
                    if (psx_pgxp_active())
                        psx_pgxp_gp0_discard();

                    gpu->recv_data = value;

                    psx_gpu_update_cmd(gpu);
                } break;
            }

            return;
        } break;

        // GP1
        case 0x04: {
            uint8_t cmd = value >> 24;

            switch (cmd) {
                // Display enable
                case 0x03: {
                    /* GPU_HW_DEBUG()-only, like poly_quad above. */
                    const uint32_t before = gpu->gpustat;
                    (void)before;
                    gpu->gpustat &= ~0x00800000;
                    gpu->gpustat |= (value << 23) & 0x00800000;
                    GPU_HW_DEBUG("gp1-display-enable value=%08x gpustat=%08x->%08x display_enable=%d", value, before, gpu->gpustat, (gpu->gpustat & 0x00800000) != 0);
                } break;
                case 0x04: {
                } break;
                case 0x05: {
                    gpu->disp_x = value & 0x3ff;
                    gpu->disp_y = (value >> 10) & 0x1ff;
                    GPU_HW_DEBUG("gp1-display-start value=%08x disp=(%u,%u)", value, gpu->disp_x, gpu->disp_y);
                } break;
                case 0x06: {
                    gpu->disp_x1 = value & 0xfff;
                    gpu->disp_x2 = (value >> 12) & 0xfff;
                    GPU_HW_DEBUG("gp1-display-range-h value=%08x disp_x=(%u,%u)", value, gpu->disp_x1, gpu->disp_x2);
                } break;
                case 0x07: {
                    /*
                        GP1(07) - Display range on screen (vertical), per psx-spx:

                            bits 0-9   Y1 (first scanline shown)
                            bits 10-19 Y2 (last scanline shown)

                        TEN bits each, not nine. The masks here were 0x1ff, which
                        silently dropped bit 9 of BOTH fields: any Y1 >= 512 wrapped,
                        and — the reachable half — Y2's bit 9 landed nowhere, so a
                        game asking for a range ending at scanline 512..1023 got one
                        ending 512 lines earlier. 240p titles keep Y2 <= 0x100 and so
                        never notice (Crash sets disp_v=(16,256)); a 480i title that
                        counts in field lines does.

                        Same defect class as the GPUINFO(5) layout and the GP1(08)
                        mode mirror above: a decode that silently narrows a field the
                        hardware keeps.
                        Gate: gp1-vrange-10bit in tests/gpu_renderer_parity.c.
                    */
                    gpu->disp_y1 = value & 0x3ff;
                    gpu->disp_y2 = (value >> 10) & 0x3ff;
                    GPU_HW_DEBUG("gp1-display-range-v value=%08x disp_y=(%u,%u)", value, gpu->disp_y1, gpu->disp_y2);
                } break;
                case 0x08:
                    gpu->display_mode = value & 0xffffff;

                    /*
                        Mirror the mode into GPUSTAT, per psx-spx:

                            GP1(08).0-1 hres1     -> GPUSTAT.17-18
                            GP1(08).2   vres      -> GPUSTAT.19
                            GP1(08).3   PAL/NTSC  -> GPUSTAT.20
                            GP1(08).4   24bpp     -> GPUSTAT.21
                            GP1(08).5   interlace -> GPUSTAT.22
                            GP1(08).6   hres2     -> GPUSTAT.16
                            GP1(08).7   reverse   -> GPUSTAT.14

                        These were never written, so a game reading its video mode back saw
                        every field as 0 — 256x240, 15bpp, progressive, NTSC — regardless of
                        what it had set. A readback lying about live state, the same defect
                        class as GPUINFO(5) and GTE ORGB. For a 512-wide title (Crash runs
                        GP1(08)=0x02) the readback claimed a 256-wide screen: an engine that
                        sizes its own culling viewport from GPUSTAT drops exactly the outer
                        flank geometry, which is the measured Crash wedge — the game submits
                        no triangles over those pixels while the GPU stream stays well-formed.
                        Confirmed in the device dump header: display_mode=000002 (hres=512)
                        alongside stat=8000001e (mode bits all zero).
                        Gate: gp1-mode-mirror in tests/gpu_renderer_parity.c.
                    */
                    gpu->gpustat = (gpu->gpustat & ~0x007f4000u)
                                 | ((value & 0x3fu) << 17)
                                 | ((value & 0x40u) << 10)
                                 | ((value & 0x80u) << 7);

                    GPU_HW_DEBUG(
                        "gp1-display-mode value=%08x display_mode=0x%08x video_standard=%s",
                        value,
                        gpu->display_mode,
                        psx_gpu_is_pal_mode(gpu) ? "PAL" : "NTSC"
                    );

                    if (gpu->event_cb_table[GPU_EVENT_DMODE])
                        gpu->event_cb_table[GPU_EVENT_DMODE](gpu);
                break;

                case 0x10: {
                    gpu->gp1_10h_req = value & 7;
                    GPU_HW_DEBUG("gp1-texpage-query value=%08x req=%u", value, gpu->gp1_10h_req);
                } break;
            }

            if (gpu->dbg_file) {
                static const int kDumpHres[4] = { 256, 320, 512, 640 };

                if (cmd == 0x08)
                    gpu_dumpf(gpu,
                        "%05d ..state  GP1(08) display_mode=%06x hres=%d%s vres=%d %s %s %s",
                        gpu->dbg_prims, gpu->display_mode,
                        (gpu->display_mode & 0x40) ? 368 : kDumpHres[gpu->display_mode & 3],
                        (gpu->display_mode & 0x40) ? "(hres2)" : "",
                        (gpu->display_mode & 0x04) ? 480 : 240,
                        (gpu->display_mode & 0x10) ? "24bpp" : "15bpp",
                        (gpu->display_mode & 0x20) ? "interlace" : "progressive",
                        (gpu->display_mode & 0x08) ? "PAL" : "NTSC");
                else
                    gpu_dumpf(gpu,
                        "%05d ..state  GP1(%02X) args=%06x disp_start=(%u,%u) "
                        "disp_h=(%u,%u) disp_v=(%u,%u) display_enable=%d",
                        gpu->dbg_prims, cmd, value & 0xffffff,
                        gpu->disp_x, gpu->disp_y,
                        gpu->disp_x1, gpu->disp_x2, gpu->disp_y1, gpu->disp_y2,
                        (gpu->gpustat & 0x00800000) ? 0 : 1);
            }

            log_trace("GP1(%02Xh) args=%06x", value >> 24, value & 0xffffff);

            return;
        } break;
    }

    log_warn("Unhandled 32-bit GPU write at offset %08x (%08x)", offset, value);
}

void psx_gpu_write16(psx_gpu_t* gpu, uint32_t offset, uint16_t value) {
    printf("Unhandled 16-bit GPU write at offset %08x (%04x)\n", offset, value);
}

void psx_gpu_write8(psx_gpu_t* gpu, uint32_t offset, uint8_t value) {
    printf("Unhandled 8-bit GPU write at offset %08x (%02x)\n", offset, value);
}

void psx_gpu_set_event_callback(psx_gpu_t* gpu, int event, psx_gpu_event_callback_t cb) {
    gpu->event_cb_table[event] = cb;
}

void psx_gpu_set_udata(psx_gpu_t* gpu, int index, void* udata) {
    gpu->udata[index] = udata;
}

#define GPU_CYCLES_PER_HDRAW_NTSC 2560.0f
#define GPU_CYCLES_PER_SCANL_NTSC 3413.0f
#define GPU_SCANS_PER_VDRAW_NTSC 240
#define GPU_SCANS_PER_FRAME_NTSC 263
#define GPU_CYCLES_PER_HDRAW_PAL 2560.0f
#define GPU_CYCLES_PER_SCANL_PAL 3406.0f
#define GPU_SCANS_PER_VDRAW_PAL  288
#define GPU_SCANS_PER_FRAME_PAL  314

void gpu_hblank_event(psx_gpu_t* gpu) {
    const int scans_per_vdraw = psx_gpu_is_pal_mode(gpu) ? GPU_SCANS_PER_VDRAW_PAL : GPU_SCANS_PER_VDRAW_NTSC;
    const int scans_per_frame = psx_gpu_is_pal_mode(gpu) ? GPU_SCANS_PER_FRAME_PAL : GPU_SCANS_PER_FRAME_NTSC;

    if (gpu->line < scans_per_vdraw) {
        if (gpu->line & 1) {
            gpu->gpustat |= 1 << 31;
        } else {
            gpu->gpustat &= ~(1 << 31);
        }

        // HACK!! More games are fine with this
        // but others, like Dead or Alive, will refuse
        // to boot because this frequency is not fast
        // enough. Sending T2 IRQs every line fixes DoA
        // but breaks a bunch of games, so I'll keep this
        // like this until I actually fix the timers
        // Games that seem to care about T2 timing:
        // - Street Fighter Alpha 2
        // - Dead or Alive
        // - NBA Jam
        // - Doom
        // - Devil Dice
        // - Zanac x Zanac
        // - Soukyugurentai
        // - Mortal Kombat
        // - PaRappa the Rapper
        // - In The Hunt
        // - Crash Bandicoot
        // - Jackie Chan Stuntmaster
        // - etc.
        // Masking with 7 breaks Street Fighter Alpha 2. The game 
        // just stops sending commands to the CDROM while on
        // Player Select. It probably uses T2 IRQs to time
        // GetlocP commands, if the timer is too slow it will
        // break.
        // if (!(gpu->line & 7))
        //     psx_ic_irq(gpu->ic, IC_SPU);
            // psx_ic_irq(gpu->ic, IC_SPU);
    } else {
        gpu->gpustat &= ~(1 << 31);
    }

    gpu->line++;

    if (gpu->line == scans_per_vdraw) {
        /* The one frame boundary the core exposes, so it is also where the marker-armed
           primitive dump opens and closes its single-frame capture. */
        gpu_dump_vblank(gpu);

#ifdef ARMSX_TEST_OFFSET_CENSUS
        /* The offset census (see gpu_offset_census) is per-frame, so it is cleared here. */
        gpu->off_hist_used = 0;
        gpu->frame_prims = 0;
#endif

        GPU_HW_DEBUG(
            "vblank-start line=%d mode=%s gpustat=0x%08x display_mode=0x%08x draw=(%u,%u)-(%u,%u) disp=(%u,%u)-(%u,%u) offset=(%d,%d)",
            gpu->line,
            psx_gpu_is_pal_mode(gpu) ? "PAL" : "NTSC",
            gpu->gpustat,
            gpu->display_mode,
            gpu->draw_x1,
            gpu->draw_y1,
            gpu->draw_x2,
            gpu->draw_y2,
            gpu->disp_x1,
            gpu->disp_y1,
            gpu->disp_x2,
            gpu->disp_y2,
            gpu->off_x,
            gpu->off_y
        );
#ifdef USE_HARDWARE
        /* The only frame boundary this core exposes (HW_RENDERER_DESIGN.md §4.5). */
        if (GPU_BACKEND_HAS(gpu, end_frame))
            gpu->backend->end_frame(gpu->backend, gpu);
#endif

        /* Texture replacement key cache. The KEY is over VRAM CONTENT, the cache is keyed on
           ADDRESSES, so anything that rewrites VRAM under a page invalidates it. GP0(02),
           GP0(80) and GP0(A0) invalidate at the point of the write; the rasterizers write
           VRAM too (that is how a render-to-texture effect works) and invalidating there
           would defeat the cache entirely, so the frame boundary catches those. A texture
           rendered into and re-sampled WITHIN one frame can therefore keep the previous
           frame's key for the rest of that frame — the documented limit of the cache.
           Costs one increment per frame while enabled, nothing while disabled. */
        psx_texrep_invalidate(gpu);

        if (gpu->event_cb_table[GPU_EVENT_VBLANK])
            gpu->event_cb_table[GPU_EVENT_VBLANK](gpu);

        psx_ic_irq(gpu->ic, IC_VBLANK);
    } else if (gpu->line == scans_per_frame) {
        GPU_HW_DEBUG(
            "vblank-end line=%d mode=%s gpustat=0x%08x display_mode=0x%08x",
            gpu->line,
            psx_gpu_is_pal_mode(gpu) ? "PAL" : "NTSC",
            gpu->gpustat,
            gpu->display_mode
        );
        if (gpu->event_cb_table[GPU_EVENT_VBLANK_END])
            gpu->event_cb_table[GPU_EVENT_VBLANK_END](gpu);

        gpu->line = 0;
    }
}

void psx_gpu_update(psx_gpu_t* gpu, int cyc) {
    const float cycles_per_hdraw = psx_gpu_is_pal_mode(gpu) ? GPU_CYCLES_PER_HDRAW_PAL : GPU_CYCLES_PER_HDRAW_NTSC;
    const float cycles_per_scanline = psx_gpu_is_pal_mode(gpu) ? GPU_CYCLES_PER_SCANL_PAL : GPU_CYCLES_PER_SCANL_NTSC;
    const float gpu_clock = psx_gpu_clock_frequency(gpu);

    int prev_hblank = (gpu->cycles >= cycles_per_hdraw) &&
                      (gpu->cycles <= cycles_per_scanline);

    // Convert CPU (~33.8 MHz) cycles to GPU (~53.7 MHz) cycles
    gpu->cycles += (float)cyc * (gpu_clock / PSX_CPU_FREQ);

    int curr_hblank = (gpu->cycles >= cycles_per_hdraw) &&
                      (gpu->cycles <= cycles_per_scanline);

    if (curr_hblank && !prev_hblank) {
        if (gpu->event_cb_table[GPU_EVENT_HBLANK])
            gpu->event_cb_table[GPU_EVENT_HBLANK](gpu);

        gpu_hblank_event(gpu);
    } else if (prev_hblank && !curr_hblank) {
        if (gpu->event_cb_table[GPU_EVENT_HBLANK_END])
            gpu->event_cb_table[GPU_EVENT_HBLANK_END](gpu);

        gpu->cycles -= cycles_per_scanline;
    }
}

void psx_gpu_set_accuracy_flags(psx_gpu_t* gpu, uint32_t flags) {
    if (gpu)
        gpu->accuracy_flags = flags;
}

void psx_gpu_set_texture_filter(psx_gpu_t* gpu, int mode) {
    if (!gpu) return;
    gpu->texture_filter = (mode >= 0 && mode <= 2) ? mode : 0;
}

int psx_gpu_texture_filter(const psx_gpu_t* gpu) {
    return gpu ? gpu->texture_filter : 0;
}

uint32_t psx_gpu_accuracy_flags(const psx_gpu_t* gpu) {
    return gpu ? gpu->accuracy_flags : 0u;
}

void* psx_gpu_get_display_buffer(psx_gpu_t* gpu) {
    if (gpu->gpustat & 0x800000)
        return gpu->empty;

    return gpu->vram + (gpu->disp_x + (gpu->disp_y * 1024));
}

#ifdef USE_HARDWARE
void psx_gpu_set_backend(psx_gpu_t* gpu, struct psx_gpu_backend* backend) {
    if (gpu)
        gpu->backend = backend;
}

int psx_gpu_resolution_scale(psx_gpu_t* gpu) {
    if (!GPU_BACKEND_HAS(gpu, resolution_scale))
        return 1;

    int scale = gpu->backend->resolution_scale(gpu->backend);

    return (scale >= 1) ? scale : 1;
}

int psx_gpu_backend_owns_display(psx_gpu_t* gpu) {
    return GPU_BACKEND_HAS(gpu, display_buffer) ? 1 : 0;
}

const void* psx_gpu_get_display_surface(psx_gpu_t* gpu, int want_native,
                                        int* out_scale, uint32_t* out_stride_bytes) {
    int scale = 1;
    uint32_t stride = PSX_GPU_FB_STRIDE;
    const void* buffer = NULL;

    if (gpu) {
        if (!want_native && GPU_BACKEND_HAS(gpu, display_buffer) &&
            !(gpu->gpustat & 0x800000)) {
            uint32_t backend_stride = 0;
            const void* scaled = gpu->backend->display_buffer(
                gpu->backend, gpu->disp_x, gpu->disp_y, &backend_stride);

            if (scaled && backend_stride) {
                buffer = scaled;
                stride = backend_stride;
                scale = psx_gpu_resolution_scale(gpu);
            }
        }

        if (!buffer)
            buffer = psx_gpu_get_display_buffer(gpu);
    }

    if (out_scale)
        *out_scale = scale;

    if (out_stride_bytes)
        *out_stride_bytes = stride;

    return buffer;
}
#endif


/* ---------------------------------------------------------------------------
   Save state.

   Saved: the whole VRAM allocation plus every command-FIFO / drawing-context /
   display-timing latch below.

   NOT saved (pointers, host resources, derived values):
     - gpu->vram / gpu->empty          allocations; restored in place
     - gpu->udata[4]                   front-end hooks (udata[1] is the timer!)
     - gpu->event_cb_table[8]          front-end vblank/hblank callbacks
     - gpu->ic                         host wiring
     - gpu->backend                    the rasterizer backend vtable
   --------------------------------------------------------------------------- */

static void gpu_save_vertex(psx_state_writer_t* w, const vertex_t* v) {
    psx_sw_u16(w, (uint16_t)v->x);
    psx_sw_u16(w, (uint16_t)v->y);
    psx_sw_u32(w, v->c);
    psx_sw_u8(w, v->tx);
    psx_sw_u8(w, v->ty);
}

static void gpu_load_vertex(psx_state_reader_t* r, vertex_t* v) {
    v->x = (int16_t)psx_sr_u16(r);
    v->y = (int16_t)psx_sr_u16(r);
    v->c = psx_sr_u32(r);
    v->tx = psx_sr_u8(r);
    v->ty = psx_sr_u8(r);
}

void psx_gpu_save_state(psx_gpu_t* gpu, psx_state_writer_t* w) {
    int i;

    psx_sw_u32(w, PSX_GPU_VRAM_SIZE);
    psx_sw_u16_array(w, gpu->vram, PSX_GPU_VRAM_SIZE / sizeof(uint16_t));

    psx_sw_i32(w, gpu->display_enable);

    for (i = 0; i < 16; i++)
        psx_sw_u32(w, gpu->buf[i]);

    psx_sw_u32(w, gpu->recv_data);
    psx_sw_i32(w, gpu->buf_index);
    psx_sw_i32(w, gpu->cmd_args_remaining);
    psx_sw_i32(w, gpu->cmd_data_remaining);
    psx_sw_i32(w, gpu->line_done);
    gpu_save_vertex(w, &gpu->prev_line_vertex);

    psx_sw_u32(w, gpu->color);
    psx_sw_u32(w, gpu->xpos);
    psx_sw_u32(w, gpu->ypos);
    psx_sw_u32(w, gpu->xsiz);
    psx_sw_u32(w, gpu->ysiz);
    psx_sw_u32(w, gpu->tsiz);
    psx_sw_u32(w, gpu->addr);
    psx_sw_u32(w, gpu->xcnt);
    psx_sw_u32(w, gpu->ycnt);

    gpu_save_vertex(w, &gpu->v0);
    gpu_save_vertex(w, &gpu->v1);
    gpu_save_vertex(w, &gpu->v2);
    gpu_save_vertex(w, &gpu->v3);

    psx_sw_u32(w, gpu->pal);
    psx_sw_u32(w, gpu->texp);
    psx_sw_u32(w, gpu->c0_xcnt);
    psx_sw_u32(w, gpu->c0_ycnt);
    psx_sw_u32(w, gpu->c0_addr);
    psx_sw_i32(w, gpu->c0_xsiz);
    psx_sw_i32(w, gpu->c0_ysiz);
    psx_sw_i32(w, gpu->c0_tsiz);
    psx_sw_i32(w, gpu->gp1_10h_req);

    psx_sw_u32(w, gpu->state);
    psx_sw_u32(w, gpu->display_mode);
    psx_sw_u32(w, gpu->gpuread);
    psx_sw_u32(w, gpu->gpustat);

    psx_sw_u32(w, gpu->draw_x1);
    psx_sw_u32(w, gpu->draw_y1);
    psx_sw_u32(w, gpu->draw_x2);
    psx_sw_u32(w, gpu->draw_y2);
    psx_sw_i32(w, gpu->off_x);
    psx_sw_i32(w, gpu->off_y);

    psx_sw_u32(w, gpu->texw_mx);
    psx_sw_u32(w, gpu->texw_my);
    psx_sw_u32(w, gpu->texw_ox);
    psx_sw_u32(w, gpu->texw_oy);
    psx_sw_u32(w, gpu->clut_x);
    psx_sw_u32(w, gpu->clut_y);
    psx_sw_u32(w, gpu->texp_x);
    psx_sw_u32(w, gpu->texp_y);
    psx_sw_u32(w, gpu->texp_d);

    psx_sw_u32(w, gpu->disp_x);
    psx_sw_u32(w, gpu->disp_y);
    psx_sw_u32(w, gpu->disp_x1);
    psx_sw_u32(w, gpu->disp_x2);
    psx_sw_u32(w, gpu->disp_y1);
    psx_sw_u32(w, gpu->disp_y2);

    /* Video timing. Without these the resumed machine lands mid-scanline and
       the vblank/hblank cadence (and therefore timers 0 and 1) drifts. */
    psx_sw_f32(w, gpu->cycles);
    psx_sw_i32(w, gpu->line);
}

int psx_gpu_load_state(psx_gpu_t* gpu, psx_state_reader_t* r) {
    uint32_t vram_size;
    int i;

    vram_size = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (vram_size != PSX_GPU_VRAM_SIZE)
        return PSX_STATE_ERR_GEOMETRY;

    psx_sr_u16_array(r, gpu->vram, PSX_GPU_VRAM_SIZE / sizeof(uint16_t));

    gpu->display_enable = psx_sr_i32(r);

    for (i = 0; i < 16; i++)
        gpu->buf[i] = psx_sr_u32(r);

    gpu->recv_data = psx_sr_u32(r);
    gpu->buf_index = psx_sr_i32(r);
    gpu->cmd_args_remaining = psx_sr_i32(r);
    gpu->cmd_data_remaining = psx_sr_i32(r);
    gpu->line_done = psx_sr_i32(r);
    gpu_load_vertex(r, &gpu->prev_line_vertex);

    gpu->color = psx_sr_u32(r);
    gpu->xpos = psx_sr_u32(r);
    gpu->ypos = psx_sr_u32(r);
    gpu->xsiz = psx_sr_u32(r);
    gpu->ysiz = psx_sr_u32(r);
    gpu->tsiz = psx_sr_u32(r);
    gpu->addr = psx_sr_u32(r);
    gpu->xcnt = psx_sr_u32(r);
    gpu->ycnt = psx_sr_u32(r);

    gpu_load_vertex(r, &gpu->v0);
    gpu_load_vertex(r, &gpu->v1);
    gpu_load_vertex(r, &gpu->v2);
    gpu_load_vertex(r, &gpu->v3);

    gpu->pal = psx_sr_u32(r);
    gpu->texp = psx_sr_u32(r);
    gpu->c0_xcnt = psx_sr_u32(r);
    gpu->c0_ycnt = psx_sr_u32(r);
    gpu->c0_addr = psx_sr_u32(r);
    gpu->c0_xsiz = psx_sr_i32(r);
    gpu->c0_ysiz = psx_sr_i32(r);
    gpu->c0_tsiz = psx_sr_i32(r);
    gpu->gp1_10h_req = psx_sr_i32(r);

    gpu->state = psx_sr_u32(r);
    gpu->display_mode = psx_sr_u32(r);
    gpu->gpuread = psx_sr_u32(r);
    gpu->gpustat = psx_sr_u32(r);

    /* States saved before the GP1(08) -> GPUSTAT mirror existed carry zeros in the mode
       bits, and a game rarely re-sends GP1(08) after boot — recompute the mirror from the
       restored display_mode so a loaded state can't reintroduce the lying readback. */
    gpu->gpustat = (gpu->gpustat & ~0x007f4000u)
                 | ((gpu->display_mode & 0x3fu) << 17)
                 | ((gpu->display_mode & 0x40u) << 10)
                 | ((gpu->display_mode & 0x80u) << 7);

    gpu->draw_x1 = psx_sr_u32(r);
    gpu->draw_y1 = psx_sr_u32(r);
    gpu->draw_x2 = psx_sr_u32(r);
    gpu->draw_y2 = psx_sr_u32(r);
    gpu->off_x = psx_sr_i32(r);
    gpu->off_y = psx_sr_i32(r);

    gpu->texw_mx = psx_sr_u32(r);
    gpu->texw_my = psx_sr_u32(r);
    gpu->texw_ox = psx_sr_u32(r);
    gpu->texw_oy = psx_sr_u32(r);
    gpu->clut_x = psx_sr_u32(r);
    gpu->clut_y = psx_sr_u32(r);
    gpu->texp_x = psx_sr_u32(r);
    gpu->texp_y = psx_sr_u32(r);
    gpu->texp_d = psx_sr_u32(r);

    gpu->disp_x = psx_sr_u32(r);
    gpu->disp_y = psx_sr_u32(r);
    gpu->disp_x1 = psx_sr_u32(r);
    gpu->disp_x2 = psx_sr_u32(r);
    gpu->disp_y1 = psx_sr_u32(r);
    gpu->disp_y2 = psx_sr_u32(r);

    gpu->cycles = psx_sr_f32(r);
    gpu->line = psx_sr_i32(r);

#ifdef USE_HARDWARE
    /* VRAM was replaced wholesale, so any backend render target is now stale. Re-seed it
       from the restored contents — the upload hook already replicates native texels at
       scale, so this is the same path a GP0(A0) transfer takes. */
    if (!r->error && GPU_BACKEND_HAS(gpu, upload_vram))
        gpu->backend->upload_vram(gpu->backend, 0, 0,
                                  PSX_GPU_FB_WIDTH, PSX_GPU_FB_HEIGHT,
                                  gpu->vram, PSX_GPU_FB_WIDTH);
#endif

    /* VRAM was replaced wholesale, so every cached texture key describes the old contents. */
    psx_texrep_invalidate(gpu);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_gpu_destroy(psx_gpu_t* gpu) {
    /* Primitive dump: close a capture that was still in flight and release the directory
       copy this object owns (psx_gpu_debug_set_log_dir). */
    if (gpu->dbg_file) {
        fclose((FILE*)gpu->dbg_file);
        gpu->dbg_file = NULL;
    }

    free(gpu->dbg_dir);
    gpu->dbg_dir = NULL;

    /* Frees the pack index and every decoded replacement, and clears texrep_bind. */
    psx_texrep_destroy(gpu);

    free(gpu->vram);
    free(gpu);
}
