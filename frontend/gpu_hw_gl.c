#include "gpu_hw_gl.h"

#if defined(USE_HARDWARE) && defined(ARMSX_ENABLE_GL) && defined(__ANDROID__)
#define ARMSX_HW_GL_BUILD 1
#endif

#ifdef USE_HARDWARE

#include <stdlib.h>

/*
    Reads the ARMSX_GL_MASK_BIT escape hatch. Outside ARMSX_HW_GL_BUILD with the predicate
    below, so the whole chain — environment to decision — is exercisable on the host; the
    env read was otherwise the one link between "the opt-in exists" and "the opt-in works"
    that no test could reach.

    Exact "1" only. A prefix match would make ARMSX_GL_MASK_BIT=0... or a stray "10" enable
    an unproven rasterizer path, and this switch exists precisely because that path silently
    took over a session once already.
*/
int armsx_hw_gl_mask_bit_opt_in(void) {
    const char* v = getenv("ARMSX_GL_MASK_BIT");

    return v && v[0] == '1' && v[1] == '\0';
}

/*
    Deliberately OUTSIDE ARMSX_HW_GL_BUILD: it touches no GL, and a host test has to be able
    to compile this file and link the rule on a machine with no GLES at all. See the contract
    in gpu_hw_gl.h and the truth table in tests/gpu_rasterizer_select.c.

    The opt-in is checked FIRST and on its own line because it is the whole point: without it
    this must return 0 for every hardware combination, including the one that reports perfect
    support. That is what keeps a session with accurate_mask_bit on the CPU rasterizer, which
    is the rasterizer against which mask-from-texel was validated.
*/
int armsx_hw_gl_mask_bit_supported(int have_fbfetch, int driver_trusted, int is_angle,
                                   int opt_in) {
    if (!opt_in)
        return 0;

    /* Framebuffer fetch is the only mechanism in GLES that hands a fragment shader the
       destination, which the mask CHECK needs. MediaTek Mali advertises it and returns zero
       or stale destination colour, and ANGLE has been seen to crash the compiler on it —
       either way the mask check would silently do nothing. */
    return have_fbfetch && driver_trusted && !is_angle;
}

int armsx_hw_gl_use_cpu_fallback(int rasterizer_mode, int internal_scale) {
    return rasterizer_mode == 2 || (rasterizer_mode == 1 && internal_scale > 1);
}
#endif

#ifdef ARMSX_HW_GL_BUILD

#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Types and constants only; every entry point is dlsym'd out of the same provider the
   present path bound, exactly as frontend/render_gl.cpp does. */
#include <EGL/egl.h>

#include "diagnostics.h"
#include "gpu_profile.h"
#include "render.h"

#if !defined(EGL_OPENGL_ES3_BIT_KHR)
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

/*
    ============================================================================================
    The software rasterizer, in a fragment shader.
    ============================================================================================

    Everything below reproduces psx/dev/gpu.c's three live rasterizers on the GPU. The
    unusual part — and the reason this is not a textbook GPU_HW bring-up — is that it does
    NOT use hardware attribute interpolation.

    A GPU interpolates at the fragment centre (x+0.5, y+0.5). gpu_render_triangle evaluates
    its barycentrics at the integer coordinate (x, y) (gpu.c:331-332). That half-pixel is a
    guaranteed parity failure for every Gouraud and every textured polygon, and it is
    invisible at S=1 only if you never look. So instead:

      * every vertex carries ALL THREE of the triangle's vertices, colours and UVs as flat
        attributes;
      * the fragment shader recomputes the edge functions itself at
              pf = gl_FragCoord.xy / S - 0.5
        which at S == 1 is EXACTLY (x, y) — the software's own sample point — and at S > 1
        is the natural centred sub-pixel position, so one shader serves both;
      * the top-left fill rule (gpu.c:266-267) and the half-open bounding box
        (gpu.c:320-321) are evaluated in the shader too, so coverage matches by
        construction instead of by trusting the GPU's fill rule to agree;
      * gpu_fetch_texel_bilinear (gpu.c:225), which polygons use unconditionally, is ported
        verbatim as four texelFetches. the backend deviation #1 is therefore
        REPRODUCED, not "fixed" — parity first.

    That costs ~10 extra ALU ops per fragment, which no GPU notices, and it is what makes
    "1x is pixel-identical" a design property rather than a hope.

    Blending (the backend, refined). With blending permanently enabled as
    (ONE, SRC_ALPHA) + FUNC_ADD:

        opaque fragment      rgb = F,        a = 0    ->  F*1 + B*0 = F   (a plain write)
        semi-transparency 0  rgb = F*0.5,    a = 0.5  ->  0.5F + 0.5B
        semi-transparency 1  rgb = F,        a = 1    ->  F + B
        semi-transparency 3  rgb = F*0.25,   a = 1    ->  0.25F + B

    so opaque primitives, modes 0/1/3, and the per-texel STP mix INSIDE one primitive all
    coexist in a single draw call with no state change at all. Only mode 2 (B - F,
    FUNC_REVERSE_SUBTRACT) needs its own range, and only textured mode-2 primitives need
    the two-pass split. The render target is RGBA8 rather than RGB5_A1 precisely because
    the 8-bit arithmetic then works out exactly: the shader emits 5-bit values
    pre-multiplied to 8 bits (c5*8), every blend result stays a multiple of 4, and the
    >> 3 at readback recovers the software's integer answer.

    Known divergence, recorded rather than hidden: a pixel that is blended MORE THAN ONCE
    keeps 8-bit precision in the render target where the software path re-truncates to 5
    bits after every blend. Single-layer transparency is exact; the second layer can differ
    by one 5-bit step. Fixing it needs programmable blending (framebuffer fetch), which is
    driver-gated on this platform and is deliberately not the baseline.

    MASK MODE is the exception, and it is where all of the above changes shape. Once
    PSX_GPU_ACCURACY_MASK_BIT is on, `.a` has to carry VRAM bit 15 and can no longer be the
    SRC_ALPHA blend factor, so the blend moves into the fragment shader via
    GL_EXT_shader_framebuffer_fetch — which the mask CHECK needs anyway, being a read of the
    destination. The arithmetic is the same four modes with the same `a`, plus the 5-bit
    re-truncation the paragraph above says is missing, so multi-layer transparency becomes
    exact there for free. Without the extension the backend declines and the CPU rasterizer
    takes the session. the backend.

    VRAM coherency (the backend) is one-way here, because
    PSX_GPU_BACKEND_SOFTWARE_SHADOW is set and gpu->vram is therefore always correct. The
    only job is keeping the native-resolution vram texture in sync with host VRAM, which is
    a pure upload problem with a known-good source. See the dirty-tile block below.
*/

/* ---- GL types and enums ------------------------------------------------------------- */
/* Declared here rather than included, exactly as frontend/render_gl.cpp does, so this file
   needs no GL headers on any platform and cannot drift out of sync with the NDK's. */

typedef unsigned int  GLenum;
typedef unsigned char GLboolean;
typedef unsigned int  GLbitfield;
typedef int           GLint;
typedef unsigned int  GLuint;
typedef int           GLsizei;
typedef float         GLfloat;
typedef char          GLchar;
typedef intptr_t      GLintptr;
typedef intptr_t      GLsizeiptr;

#define GL_NO_ERROR                     0
#define GL_FALSE                        0
#define GL_TRUE                         1
#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_ZERO                         0
#define GL_ONE                          1
#define GL_SRC_ALPHA                    0x0302
#define GL_FUNC_ADD                     0x8006
#define GL_FUNC_REVERSE_SUBTRACT        0x800B
#define GL_BLEND                        0x0BE2
#define GL_SCISSOR_TEST                 0x0C11
#define GL_DEPTH_TEST                   0x0B71
#define GL_STENCIL_TEST                 0x0B90
#define GL_CULL_FACE                    0x0B44
#define GL_DITHER                       0x0BD0
#define GL_COLOR_BUFFER_BIT             0x00004000
#define GL_TEXTURE_2D                   0x0DE1
#define GL_TEXTURE0                     0x84C0
#define GL_TEXTURE1                     0x84C1
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_NEAREST                      0x2600
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_RGBA                         0x1908
#define GL_RGBA8                        0x8058
#define GL_RG                           0x8227
#define GL_RG8                          0x822B
#define GL_RED_INTEGER                  0x8D94
#define GL_R16UI                        0x8234
#define GL_UNSIGNED_BYTE                0x1401
#define GL_UNSIGNED_SHORT               0x1403
#define GL_UNSIGNED_INT                 0x1405
#define GL_FLOAT                        0x1406
#define GL_ARRAY_BUFFER                 0x8892
#define GL_STREAM_DRAW                  0x88E0
#define GL_VERTEX_SHADER                0x8B31
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_INFO_LOG_LENGTH              0x8B84
#define GL_FRAMEBUFFER                  0x8D40
#define GL_READ_FRAMEBUFFER             0x8CA8
#define GL_DRAW_FRAMEBUFFER             0x8CA9
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_UNPACK_ROW_LENGTH            0x0CF2
#define GL_PACK_ALIGNMENT               0x0D05
#define GL_PACK_ROW_LENGTH              0x0D02
#define GL_MAX_TEXTURE_SIZE             0x0D33
#define GL_IMPLEMENTATION_COLOR_READ_TYPE   0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define GL_VENDOR                       0x1F00
#define GL_VERSION                      0x1F02
#define GL_RENDERER                     0x1F01
#define GL_EXTENSIONS                   0x1F03

/* ---- entry points ------------------------------------------------------------------- */

typedef struct {
    void (*ActiveTexture)(GLenum);
    void (*AttachShader)(GLuint, GLuint);
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar*);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*BindTexture)(GLenum, GLuint);
    void (*BindVertexArray)(GLuint);
    void (*BlendEquation)(GLenum);
    void (*BlendFunc)(GLenum, GLenum);
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                            GLbitfield, GLenum);
    void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*Clear)(GLbitfield);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (*CompileShader)(GLuint);
    GLuint (*CreateProgram)(void);
    GLuint (*CreateShader)(GLenum);
    void (*DeleteBuffers)(GLsizei, const GLuint*);
    void (*DeleteFramebuffers)(GLsizei, const GLuint*);
    void (*DeleteProgram)(GLuint);
    void (*DeleteShader)(GLuint);
    void (*DeleteTextures)(GLsizei, const GLuint*);
    void (*DeleteVertexArrays)(GLsizei, const GLuint*);
    void (*Disable)(GLenum);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*Enable)(GLenum);
    void (*EnableVertexAttribArray)(GLuint);
    void (*Finish)(void);
    /* Trailing underscore only because the macro that loads it takes the field name and the
       symbol name separately, and this keeps them visually distinct. */
    void (*FramebufferTexture2D_)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*GenBuffers)(GLsizei, GLuint*);
    void (*GenFramebuffers)(GLsizei, GLuint*);
    void (*GenTextures)(GLsizei, GLuint*);
    void (*GenVertexArrays)(GLsizei, GLuint*);
    GLenum (*GetError)(void);
    void (*GetIntegerv)(GLenum, GLint*);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (*GetProgramiv)(GLuint, GLenum, GLint*);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (*GetShaderiv)(GLuint, GLenum, GLint*);
    const unsigned char* (*GetString)(GLenum);
    GLint (*GetUniformLocation)(GLuint, const GLchar*);
    void (*LinkProgram)(GLuint);
    void (*PixelStorei)(GLenum, GLint);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                       const void*);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                          const void*);
    void (*Uniform1f)(GLint, GLfloat);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform1iv)(GLint, GLsizei, const GLint*);
    void (*Uniform2f)(GLint, GLfloat, GLfloat);
    void (*Uniform2i)(GLint, GLint, GLint);
    void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void (*UseProgram)(GLuint);
    void (*VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void*);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
} gl_api_t;

/* ---- vertex format ------------------------------------------------------------------ */

/* 72 bytes. Every triangle's three vertices carry the whole primitive, because the
   fragment shader does its own barycentric evaluation (see the file header). PS1 frames
   are a few thousand triangles at most, so ~200 bytes per triangle is not the constraint;
   draw-call count is. */
typedef struct {
    float    pos[2];     /* this vertex, native coords, drawing offset already applied */
    float    tri0[4];    /* x0,y0,x1,y1 -- post-winding-swap, offset applied            */
    float    tri1[2];    /* x2,y2                                                       */
    float    triw[3];    /* PGXP: GTE w (SZ3) of a/b/c; {1,1,1} marks an integer triangle,
                            which the shader reads as "affine, use the exact bdiv path". */
    uint8_t  col0[4];    /* 0xBBGGRR of vertex a, as gpu.c stores it                    */
    uint8_t  col1[4];
    uint8_t  col2[4];
    uint16_t uv01[4];    /* u0,v0,u1,v1                                                 */
    uint16_t uv2[4];     /* u2,v2,texpage,clut                                          */
    uint16_t misc[4];    /* flags, transp_mode, -, -                                    */
    uint8_t  texwin[4];  /* mx,my,ox,oy, all pre-shifted by 3 by GP0(E2)                */
    /*
        Texture replacement (psx/texrep.h), packed:
          [0]  atlas x | (atlas y << 16)   -- where the image sits in repl_tex
          [1]  u0 | (v0 << 8) | (S << 16)  -- folded rect origin and the integer upscale
          [2]  (nw-1) | ((nh-1) << 16)     -- folded rect size, for the shader's clamp
          [3]  reserved, always 0

        S == 0 means "no replacement", which is what every vertex carries unless the feature
        is on AND a pack file matched THIS primitive. It is per-VERTEX for the same reason
        GLF_MASK_CHECK is per vertex: a uniform would be per-DRAW and would break the batch at
        every primitive, and PS1 games change texture page constantly.
    */
    uint32_t repl[4];
} gl_vertex_t;

/* Mirrors the PA_ and RA_ attribute bits where their values coincide, so the CPU side can
   pass gpu.c's attrib byte through almost unchanged and only add the two bits gpu.c has no
   concept of. */
enum {
    GLF_RAW      = 0x0001,
    GLF_TRANSP   = 0x0002,
    GLF_TEXTURED = 0x0004,
    GLF_SHADED   = 0x0010,
    GLF_SPRITE   = 0x0100,  /* point-sampled, UV from the position delta, never dithered */
    GLF_DITHER   = 0x0200,
    /*
        GP0(E6), resolved per PRIMITIVE by psx_gpu_mask_check()/psx_gpu_mask_set() and carried
        per VERTEX rather than as a uniform — which is the one design decision that makes the
        mask bit free here. the backend maps it to the stencil test, whose
        reference value is per-DRAW; that would break the batch at every GP0(E6), and PS1 games
        toggle it between primitives (Silent Hill's fog does it per object). In the vertex it
        costs nothing and never splits a range.
    */
    GLF_MASK_CHECK = 0x0400,
    GLF_MASK_SET   = 0x0800
};

/* Blend state a draw range runs under. Only mode 2 needs the second one — see the header. */
enum { GL_RANGE_ADD = 0, GL_RANGE_SUB = 1 };

typedef struct {
    int     first;      /* first vertex */
    int     count;
    uint8_t blend;      /* GL_RANGE_* */
    uint8_t stp_pass;   /* 0 = all fragments, 1 = opaque only, 2 = semi-transparent only */
    int16_t sx, sy, sw, sh;  /* scissor, native coords */
} gl_range_t;

/* ---- dirty-tile tracking ------------------------------------------------------------ */

/* 16x16 native texels per tile -> 64 x 32 tiles over VRAM, one uint64 per tile row.
   A bounding rectangle alone is not enough: a game with its framebuffer at the top-left
   and its textures at the bottom-right would produce a box covering all of VRAM and force
   a full 1 MB upload every frame (the backend). */
#define TILE_SHIFT 4
#define TILES_X    64
#define TILES_Y    32

typedef struct { uint64_t row[TILES_Y]; } tilemap_t;

/* ---- backend ------------------------------------------------------------------------ */

typedef struct {
    psx_gpu_backend_t base;
    psx_gpu_t*        gpu;

    gl_api_t gl;
    void*    gles_library;

    /* Only populated when we had to make our own context because the presentation backend
       is not OpenGL. See gl_acquire_context(). */
    void*      egl_library;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    int        owns_context;

    int scale;
    int rt_w, rt_h;

    GLuint rt_tex, rt_fbo;
    GLuint vram_tex;
    GLuint vram_fbo;                   /* vram_tex as a TARGET:  row 1's GPU->GPU resolve */
    int    vram_fbo_state;             /* 0 untried, 1 complete, -1 unusable */
    GLuint scratch_tex, scratch_fbo;   /* GP0(80) needs a bounce; GL forbids self-blit */
    int    scratch_w, scratch_h;
    GLuint resolve_tex, resolve_fbo;   /* packed BGR555 scanout, read straight to the host */
    int    resolve_w, resolve_h;
    GLuint xfer_tex, xfer_fbo;         /* packed BGR555 at NATIVE size, for GPU->host reads */
    int    xfer_w, xfer_h;

    /*
        Texture replacement atlas (psx/texrep.h). ONE RGBA8 texture, shelf-packed, sampled
        with texelFetch. Created lazily the first time a replacement is actually bound, so a
        session with the feature off never allocates it and the whole block below stays zero.

        No eviction. When a frame needs more replacement pixels than the atlas holds, the
        PENDING BATCH is flushed -- which is the only thing that still needs the pixels
        currently in it -- and the packer starts over. That is why residency is a generation
        (`repl_gen`) rather than a flag: resetting the atlas is one increment, and every
        psx_texrep_image_t stamped with an older generation is implicitly evicted.

        `repl_epoch` tracks psx_texrep_epoch(), so switching packs in the settings screen
        drops the atlas without this file having to be told.
    */
    GLuint   repl_tex;
    int      repl_dim;                 /* square edge; 0 = never created */
    int      repl_x, repl_y, repl_shelf;
    uint32_t repl_gen;
    uint32_t repl_epoch;
    uint32_t cur_repl[4];              /* a_repl for the primitive being emitted */
    int      repl_full_logged;

    GLuint vbo, vao, quad_vao;

    GLuint prog_draw;
    GLint  u_draw_rt_size, u_draw_scale, u_draw_vram, u_draw_dither, u_draw_dither_on,
           u_draw_stp_pass, u_draw_paint, u_draw_filter, u_draw_tex_trunc, u_draw_mask_texel,
           u_draw_repl;
    GLuint prog_xfer;
    GLint  u_xfer_rect, u_xfer_rt_size, u_xfer_scale, u_xfer_vram;
    GLuint prog_resolve;
    GLint  u_res_rt, u_res_origin, u_res_limit, u_res_step, u_res_box, u_res_mask;
    GLuint prog_gres;                  /* the same resolve, with a uint output */
    GLint  u_gres_rt, u_gres_limit, u_gres_step, u_gres_half, u_gres_mask;

    /*
        PSX_GPU_ACCURACY_MASK_BIT is on AND this backend can serve it, i.e. the draw shader was
        compiled with its mask stage and the render target's ALPHA CHANNEL now carries VRAM bit
        15 (the backend). Everything the flag switches:

          * the fragment shader does its own blending (framebuffer fetch) instead of the
            fixed-function unit, because alpha can only be the blend factor or the mask bit and
            not both —  "alpha-channel conflict", resolved in favour of the mask bit;
          * GL_BLEND is therefore OFF for the draw program;
          * both resolve shaders reconstruct bit 15 from alpha instead of writing 0.

        Off, every one of those reverts textually to what shipped, so the default path is
        byte-identical.
    */
    int mask_mode;

    gl_vertex_t* verts;
    int          vert_count, vert_cap;
    gl_range_t*  ranges;
    int          range_count, range_cap;

    tilemap_t dirty;    /* host VRAM the vram texture has not seen yet */
    tilemap_t sampled;  /* VRAM the pending batch samples; a write here forces a flush */
    int       any_dirty, any_sampled;

    /*
         row 1 /  `gpu_dirty`: regions the RASTERIZER wrote, which therefore exist
        ONLY in the render target. A textured draw that samples one of these must not be fed
        from host VRAM — that is stale the moment the software shadow stops running — so
        those tiles are resolved render target -> vram_tex on the GPU, with no CPU round trip.

        Maintained unconditionally, because the bitmap costs nothing; it is only ACTED ON
        when `gpu_own` is set (see below), so the shadow-on build's behaviour is unchanged
        to the instruction unless the marker arms it.
    */
    tilemap_t gpu_dirty;
    int       any_gpu_dirty;

    /*
        "the render target owns rendered content", i.e. draws mark gpu_dirty rather than
        dirty and gl_note_sample() resolves from the render target. Implied by !shadow;
        forceable by `hwgl_gpu_resolve` WHILE the shadow is still on, which is the whole
        point — the 1x parity gate then becomes a direct oracle for the resolve, with the
        software shadow still installed as the reference. See .
    */
    int       gpu_own;
    int       dbg_gpu_resolve;

    uint8_t* readback;      /* packed BGR555, w*S x h*S */
    size_t   readback_cap;
    GLenum   read_format, read_type;   /* the 2-bytes-per-pixel fast path, when offered */
    int      read_packed;
    int      read_probed;

    uint8_t* native_rb;     /* staging for gl_readback_rect(), packed BGR555 at native size */
    size_t   native_rb_cap;

    /* Set from base.flags at create(). While it is on, gpu->vram is authoritative (the core
       runs the software rasterizer too) and every GPU->host read below is diagnostic rather
       than load-bearing. Clearing it is what  proper means; every path is already written
       for both worlds so that switch stays a one-liner. */
    int      shadow;

    int rt_bound;
    int have_context;   /* teardown must not issue GL calls without one */
    int failed;
    int error_streak;

    /* Diagnostic switches, read once at create() from marker FILES next to the diag log,
       so a single build can be A/B'd from adb (`run-as <pkg> touch files/logs/<name>`)
       without a rebuild or a settings-schema change. Both default ON — the ON state is the
       shipping behaviour and the OFF state exists only to reproduce the pre-fix build as a
       control. See the backend. */
    int dbg_tri_bbox;     /* off: submit the triangle itself, i.e. the coverage bug */
    int dbg_upscale_parity; /* `hwgl_upscale_parity`: one upscaled-vs-shadow report */
    int dbg_paint;          /* `hwgl_paint_reject`: paint coverage-rejected fragments magenta */
    int dbg_defer_dirty;  /* off: mark the destination dirty before note_sample */
    int dbg_dump;         /* dump one scanout + the software shadow at frame 3000 */

    /* `hwgl_vram_diff`: run the whole-VRAM readback-vs-shadow diff every kVramDiffPeriod
       frames instead of only on the downgrade path, so it can be taken at a BUSY frame and
       at more than one moment.  single 0/524288 was real but was one sample, and
       the second copy of it landed on a black CD-load screen (softnonzero=0) and is
       vacuous. Only meaningful while the shadow is on — it IS the reference. */
    int dbg_vram_diff;

    /* `hwgl_geom`: one shot at frame 3000, the first 8 textured samples with their page
       rectangle, the drawing area, the display origin and which of the two tilemaps they
       intersect. Reasoning about which regions overlap which produced three wrong answers
       in a row; this prints them. */
    int dbg_geom;
    int geom_left;
    int geom_draw_left;

    /*
         automatic downgrade, and the two markers that make it TESTABLE.

        The rule itself is  option 1: a running average of GP0(C0) readback bytes over
        the last 60 frames above `c0_limit` means this game reads VRAM faster than a GPU
        rasterizer can serve it, so seed host VRAM from the render target, say so in the log,
        and hand the session back to a CPU rasterizer for good.

        The markers exist because the trigger CANNOT fire on its own here: with the software
        shadow in place GP0(C0) costs nothing, and a whole Crash Bandicoot session moves 96
        bytes through it (). A fallback that has never executed is not a fallback, so:

          * hwgl_c0_trip       — c0_limit = 0, i.e. ANY GP0(C0) traffic inside the 60-frame
                                 window trips it. Fires from REAL game traffic, so it proves
                                 the counter, the window and the trigger are wired to the
                                 emulated GPU and not just to each other.
          * hwgl_force_downgrade — trips unconditionally at frame 900 whether or not the game
                                 ever touches GP0(C0). Deterministic, so it proves the LADDER
                                 (seed, teardown, fallback rasterizer, picture keeps moving)
                                 in a game that reads no VRAM at all.
    */
    int      dbg_c0_trip;
    int      dbg_force_downgrade;
    uint64_t c0_limit;          /* bytes per frame, averaged over the window */
    uint32_t c0_frame_bytes;    /* GP0(C0) bytes in the frame being emulated now */
    uint32_t c0_window[60];     /* per-frame history; the running average's ring */
    uint64_t c0_window_sum;
    int      c0_window_idx;
    int      downgraded;        /* the ladder has already been walked; never twice */

    /* Diagnostics. Reported every kStatsPeriod frames; this is the only instrumentation
       that exists for "is the GPU path actually batching", which  says is the whole
       difference between 60 fps and 12 on a tiler. */
    uint64_t frames;
    uint32_t stat_draws, stat_ranges, stat_prims, stat_syncs, stat_sync_px;
    uint32_t stat_adopted;           /* frames presented straight off the GPU, no readback */
    uint32_t stat_reads, stat_read_px;  /* gl_readback_rect():  stall, when it happens */
    uint32_t stat_gres, stat_gres_px;   /*  row 1's GPU->GPU resolve; NOT a stall */
    uint64_t stat_readback_bytes;    /* GP0(C0), for  accounting */

    /* The brokered seam (armsx_hw_gl_present_texture). Sticky: a present backend that
       cannot take a GL texture is a property of the run, not of the frame, so it is asked
       once and then left alone — otherwise every frame would pay a wasted resolve. */
    int      adopt_disabled;
    int      adopt_logged;

    /* The 1x parity gate. make test-gpu cannot reach a GL backend, so this measures it.
       The buckets matter more than the total: "0.2% of pixels differ" is not actionable,
       but "every one of them is BLANK where the software has content" and "every one of
       them is within one 5-bit step" point at completely different bugs — a primitive the
       backend never drew versus rounding in the blend. FAR is the only bucket that means
       the rasterization itself disagrees. */
    uint64_t parity_pixels, parity_diff;
    uint64_t parity_blank;   /* GPU 0, software non-zero: the backend missed a write */
    uint64_t parity_extra;   /* GPU non-zero, software 0: the backend drew something extra */
    uint64_t parity_near;    /* every channel within one 5-bit step: rounding / dither */
    uint64_t parity_far;     /* genuinely different colours */
    /* MASK BIT ONLY: the colour agrees and bit 15 does not. Its own bucket because it is the
       one bit the GL path cannot get right by accident — it comes from a framebuffer-fetch
       read of the destination, not from the arithmetic every other bucket measures. Counted
       (and included in parity_diff) only while mask_mode is on; the comparison masks bit 15
       off otherwise, exactly as it always did. */
    uint64_t parity_mask;
    /* Four samples rather than one. A single coordinate cannot distinguish "one broken
       region" from "speckle spread over the whole frame", and that distinction is the
       difference between hunting a transfer path and hunting a coverage rule. */
    uint16_t parity_blank_x[4], parity_blank_y[4], parity_blank_want[4];
    int      parity_blank_n;
    uint16_t parity_far_x[4], parity_far_y[4], parity_far_got[4], parity_far_want[4];
    int      parity_far_n;
    /* Mismatches with an orthogonal neighbour that also mismatches. High means clustered
       (a region the backend got wrong); low means isolated speckle (an edge rule). */
    uint64_t parity_clustered;
    /* Worst single scanout in the window, and how many scanouts were perfectly clean.
       "1.1% of pixels over 1800 frames" is a completely different bug depending on whether
       every frame is 1.1% wrong or forty frames are 50% wrong and the rest are exact. */
    uint64_t parity_worst_frame;
    uint64_t parity_clean_frames, parity_frames;
    int      parity_done;
} hw_gl_t;

static char g_status[192] = "not attempted";

static void gl_status(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, args);
    va_end(args);
    psxe_diag_logf("hwgl", "%s", g_status);
}

const char* armsx_hw_gl_status(void) {
    return g_status;
}

/*
    [video] texture_filter / downsample / line_detect.

    Process-global rather than per-instance for one concrete reason: this backend is
    DESTROYED AND RECREATED behind the frontend's back — checkRasterizerHealth() does it on
    any GL failure, and a scale change does it too. Options that lived only on hw_gl_t would
    silently revert to their defaults there, which is the "setting worked until something
    else happened" failure this project has shipped before. The instance reads these every
    time it needs them, so a create() in between changes nothing.

    Plain ints, no lock: each is written by the UI thread and read by the emulation thread,
    both as a single aligned word, and every value is independently valid — a torn read is
    not expressible. All three are 0 (= off / nearest / disabled) by default.
*/
static int g_opt_texture_filter = 0;   /* 0 nearest, 1 bilinear, 2 xBR-style */
static int g_opt_downsample = 0;       /* 0 or 1 = off, 2..8 = requested box factor */
static int g_opt_line_detect = 0;      /* 0 disabled, 1 quads, 2 basic */
/* Mirrors PSX_GPU_ACCURACY_TEX_MODULATE for the shader; re-read from the gpu each flush
   so it cannot go stale the way a create()-time copy would. */
static int g_opt_tex_trunc = 0;
/* Mirrors psx_gpu_mask_from_texel() != 0 for the shader, re-read per primitive for the same
   reason. It is the third argument of PSX_GPU_MASK_WRITE and the ONE that is not carried per
   vertex, because it follows the accuracy flag rather than GPUSTAT. */
static int g_opt_mask_texel = 0;

void armsx_hw_gl_set_video_options(int texture_filter, int downsample, int line_detect) {
    g_opt_texture_filter = (texture_filter >= 0 && texture_filter <= 2) ? texture_filter : 0;
    g_opt_downsample = (downsample < 2) ? 0 : (downsample > 8 ? 8 : downsample);
    g_opt_line_detect = (line_detect >= 0 && line_detect <= 2) ? line_detect : 0;
}

/* Builds <diag log directory>/<name>. The diag log path is the one app-private location
   this file can reach without new plumbing, and it is exactly where `run-as` already
   works. Returns 0 when diagnostics are off, which is the shipping default. */
static int gl_debug_path(char* out, size_t cap, const char* name) {
    const char* logp = psxe_diag_log_path();
    const char* slash;
    size_t dir_len;

    if (!logp || !logp[0])
        return 0;

    slash = strrchr(logp, '/');

    if (!slash)
        return 0;

    dir_len = (size_t)(slash - logp) + 1u;

    if ((dir_len + strlen(name) + 1u) > cap)
        return 0;

    memcpy(out, logp, dir_len);
    memcpy(out + dir_len, name, strlen(name) + 1u);

    return 1;
}

static int gl_debug_marker(const char* name) {
    char path[1024];
    FILE* f;

    if (!gl_debug_path(path, sizeof(path), name))
        return 0;

    f = fopen(path, "rb");

    if (!f)
        return 0;

    fclose(f);

    return 1;
}

static hw_gl_t* gl_self(psx_gpu_backend_t* be) {
    return (hw_gl_t*)be;
}

/* ---- shaders ------------------------------------------------------------------------- */

static const char* kDrawVS =
"#version 300 es\n"
"precision highp float;\n"
"precision highp int;\n"
"in vec2  a_pos;\n"
"in vec4  a_tri0;\n"
"in vec2  a_tri1;\n"
"in uvec4 a_col0;\n"
"in uvec4 a_col1;\n"
"in uvec4 a_col2;\n"
"in uvec4 a_uv01;\n"
"in uvec4 a_uv2;\n"
"in uvec4 a_misc;\n"
"in uvec4 a_texwin;\n"
"in uvec4 a_repl;\n"
"in vec3  a_triw;\n"
"uniform vec2 u_rt_size;\n"
"uniform float u_scale;\n"
"flat out vec4  v_tri0;\n"
"flat out vec2  v_tri1;\n"
"flat out uvec4 v_col0;\n"
"flat out uvec4 v_col1;\n"
"flat out uvec4 v_col2;\n"
"flat out uvec4 v_uv01;\n"
"flat out uvec4 v_uv2;\n"
"flat out uvec4 v_misc;\n"
"flat out uvec4 v_texwin;\n"
"flat out uvec4 v_repl;\n"
"flat out vec3  v_triw;\n"
"void main() {\n"
"    v_tri0 = a_tri0; v_tri1 = a_tri1;\n"
"    v_col0 = a_col0; v_col1 = a_col1; v_col2 = a_col2;\n"
"    v_uv01 = a_uv01; v_uv2 = a_uv2; v_misc = a_misc; v_texwin = a_texwin;\n"
"    v_repl = a_repl;\n"
"    v_triw = a_triw;\n"
"    vec2 rt = a_pos * u_scale;\n"
"    gl_Position = vec4(rt / u_rt_size * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

/*
    NOTE: no `#version` line. This is the BODY only — gl_build_draw_fs() prepends the version,
    and, when the mask bit is on, the framebuffer-fetch `#extension`, `#define ARMSX_GL_MASK`
    and PSX_GPU_MASK_GLSL (psx/dev/gpu.h), which is where the two mask expressions come from.
    Both directives are required to precede any other token, so they cannot live here.
*/
static const char* kDrawFS =
"precision highp float;\n"
"precision highp int;\n"
"precision highp usampler2D;\n"
"uniform highp usampler2D u_vram;\n"
"uniform float u_scale;\n"
"uniform int   u_dither[16];\n"
"uniform int   u_dither_on;\n"
"uniform int   u_stp_pass;\n"
"uniform int   u_tex_trunc;\n"
/* psx_gpu_mask_from_texel() != 0, i.e. the accuracy flag itself. Read only inside the mask
   stage; declared unconditionally so the uniform location lookup is one code path. */
"uniform int   u_mask_texel;\n"
"uniform int   u_paint;\n"
/* [video] texture_filter: 0 = nearest/legacy, 1 = bilinear, 2 = xBR-style. A uniform rather
   than a recompile because this file has no runtime shader-rebuild path (the four programs
   are linked once in create()), and because the setting is meant to be live-toggleable.
   0 is the shipping default and takes the identical branch the file had before filtering
   existed. */
"uniform int   u_filter;\n"
/* The replacement atlas (psx/texrep.h). Declared unconditionally so the uniform lookup is one
   code path; sampled only when v_repl says a replacement is bound, which is never unless the
   feature is on. texelFetch, so no sampler filtering state can leak in. */
"uniform highp sampler2D u_repl;\n"
"flat in vec4  v_tri0;\n"
"flat in vec2  v_tri1;\n"
"flat in uvec4 v_col0;\n"
"flat in uvec4 v_col1;\n"
"flat in uvec4 v_col2;\n"
"flat in uvec4 v_uv01;\n"
"flat in uvec4 v_uv2;\n"
"flat in uvec4 v_misc;\n"
"flat in uvec4 v_texwin;\n"
"flat in uvec4 v_repl;\n"
"flat in vec3  v_triw;\n"
/* `inout` is how GL_EXT_shader_framebuffer_fetch spells "this output starts out holding the
   destination pixel". That read is the mask CHECK, and it is also what lets the blend happen
   here rather than in the fixed-function unit — see the mask stage at the bottom. */
"#ifdef ARMSX_GL_MASK\n"
"inout vec4 o_color;\n"
"#else\n"
"out vec4 o_color;\n"
"#endif\n"
"const uint F_RAW = 1u, F_TRANSP = 2u, F_TEXTURED = 4u, F_SHADED = 16u;\n"
"const uint F_SPRITE = 256u, F_DITHER = 512u;\n"
/* GLF_MASK_CHECK / GLF_MASK_SET. */
"const uint F_MASK_CHECK = 1024u, F_MASK_SET = 2048u;\n"
/* gpu.c indexes VRAM linearly and does NOT wrap the texture page at x=1024
   (the backend deviation #8). Reproducing that means addressing linearly
   here too; the row mask only keeps a pathological page from reading past the surface,
   which is a latent overrun in the software path itself. */
"uint vram_at(int lin) {\n"
"    return texelFetch(u_vram, ivec2(lin & 1023, (lin >> 10) & 511), 0).r;\n"
"}\n"
"uint fetch_texel(int tx, int ty) {\n"
"    int mx = int(v_texwin.x), my = int(v_texwin.y);\n"
"    tx = (tx & ~mx) | (int(v_texwin.z) & mx);\n"
"    ty = (ty & ~my) | (int(v_texwin.w) & my);\n"
"    tx &= 0xff; ty &= 0xff;\n"
"    int texp = int(v_uv2.z), clut = int(v_uv2.w);\n"
"    int tpx = (texp & 0xf) << 6;\n"
"    int tpy = (texp & 0x10) << 4;\n"
"    int clutx = (clut & 0x3f) << 4;\n"
"    int cluty = (clut >> 6) & 0x1ff;\n"
"    int depth = (texp >> 7) & 3;\n"
"    if (depth == 0) {\n"
"        uint w = vram_at((tpx + (tx >> 2)) + ((tpy + ty) << 10));\n"
"        int idx = int((w >> uint((tx & 3) << 2)) & 0xfu);\n"
"        return vram_at((clutx + idx) + (cluty << 10));\n"
"    }\n"
"    if (depth == 1) {\n"
"        uint w = vram_at((tpx + (tx >> 1)) + ((tpy + ty) << 10));\n"
"        int idx = int((w >> uint((tx & 1) << 3)) & 0xffu);\n"
"        return vram_at((clutx + idx) + (cluty << 10));\n"
"    }\n"
"    return vram_at((tpx + tx) + ((tpy + ty) << 10));\n"
"}\n"
/* ---- texture replacement (psx/texrep.h) -------------------------------------------------
   The window transform is the SAME two lines fetch_texel() opens with, because the folded
   rectangle recorded in v_repl is in post-window texel space -- the transform is idempotent,
   so applying it here lands inside the image by construction and the clamp inside texrep_at()
   only catches interpolation that strayed outside the vertex UV box.

   The atlas mapping and the RGBA8 -> BGR555 pack both come from PSX_TEXREP_GLSL, the same
   text psx_texrep_sample() implements in C for the two CPU rasterizers. One formula, one
   place: three copies of a formula is how ,  and  all happened.

   fx/fy are the SUB-TEXEL, in 0..S-1. A replacement is S times finer than the native texel
   grid, and without this every SxS block would show one replacement pixel -- an upscaled pack
   would look exactly like the original. */
"uint fetch_repl(float tx, float ty, vec2 frac) {\n"
"    int mx = int(v_texwin.x), my = int(v_texwin.y);\n"
"    int itx = ((int(floor(tx)) & ~mx) | (int(v_texwin.z) & mx)) & 0xff;\n"
"    int ity = ((int(floor(ty)) & ~my) | (int(v_texwin.w) & my)) & 0xff;\n"
"    int s  = int(v_repl.y >> 16);\n"
"    int u0 = int(v_repl.y & 0xffu);\n"
"    int v0 = int((v_repl.y >> 8) & 0xffu);\n"
"    int nw = int(v_repl.z & 0xffffu) + 1;\n"
"    int nh = int((v_repl.z >> 16) & 0xffffu) + 1;\n"
"    int ax = int(v_repl.x & 0xffffu);\n"
"    int ay = int((v_repl.x >> 16) & 0xffffu);\n"
"    int fx = int(floor(frac.x * float(s)));\n"
"    int fy = int(floor(frac.y * float(s)));\n"
"    ivec2 p = texrep_at(itx, ity, u0, v0, nw, nh, s, ax, ay, fx, fy);\n"
"    return texrep_pack(texelFetch(u_repl, p, 0));\n"
"}\n"
/* gpu_fetch_texel_bilinear, gpu.c:225. Polygons use it unconditionally; the early-out on
   the top-left tap and the OR of all four mask bits are both load-bearing for parity. */
/* ---- [video] texture_filter ------------------------------------------------------------
   These run on the TEXTURE SAMPLE and nothing else. Coverage has already been decided by the
   time control reaches them (: once per NATIVE pixel at pc = vec2(pn)), so no filter
   mode can move an edge, open a seam or change which fragments survive. The only fragment
   any of them can kill is one whose nearest texel is 0 — the transparent-texel discard that
   was already there, kept deliberately so a filtered edge cannot bleed a cut-out shape.

   u_filter == 0 is untouched legacy (fetch_bilinear below, the software rasterizer's own
   sub-texel blend) and is the ONLY mode the 1x parity gate means anything under. */
"vec3 c5(uint v) {\n"
"    return vec3(float(v & 31u), float((v >> 5) & 31u), float((v >> 10) & 31u));\n"
"}\n"
/* Weighted 2x2 around the TEXEL CENTRE (tx - 0.5), with transparent taps dropped from the
   weight sum instead of averaged in as black — averaging them in is what puts a dark halo
   around every cut-out sprite. */
"uint fetch_smooth(float tx, float ty) {\n"
"    if (fetch_texel(int(floor(tx)), int(floor(ty))) == 0u) return 0u;\n"
"    float bx = tx - 0.5, by = ty - 0.5;\n"
"    float x0 = floor(bx), y0 = floor(by);\n"
"    float fx = bx - x0, fy = by - y0;\n"
"    uint s00 = fetch_texel(int(x0), int(y0));\n"
"    uint s10 = fetch_texel(int(x0) + 1, int(y0));\n"
"    uint s01 = fetch_texel(int(x0), int(y0) + 1);\n"
"    uint s11 = fetch_texel(int(x0) + 1, int(y0) + 1);\n"
"    vec4 w = vec4((1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),\n"
"                  (1.0 - fx) * fy,         fx * fy);\n"
"    w *= vec4(s00 != 0u ? 1.0 : 0.0, s10 != 0u ? 1.0 : 0.0,\n"
"              s01 != 0u ? 1.0 : 0.0, s11 != 0u ? 1.0 : 0.0);\n"
"    float ws = w.x + w.y + w.z + w.w;\n"
"    if (ws <= 0.0) return 0u;\n"
"    vec3 acc = w.x * c5(s00) + w.y * c5(s10) + w.z * c5(s01) + w.w * c5(s11);\n"
"    uvec3 q = uvec3(clamp(acc / ws, 0.0, 31.0));\n"
"    return q.x | (q.y << 5) | (q.z << 10) | ((s00 | s10 | s01 | s11) & 0x8000u);\n"
"}\n"
/* xBR-style: edge-DIRECTED, which is the family's whole idea — interpolate ALONG a detected
   edge and stay sharp across it, instead of blurring both ways like a plain box. The two
   diagonals of the same 2x2 are compared in luma; the one with the smaller difference is the
   direction the edge runs, and only that pair is blended. When neither diagonal wins clearly
   there is no edge to follow and it degrades to fetch_smooth().

   This is a 2x2 kernel, NOT the full multi-pass xBR rule set: it costs the same four taps as
   bilinear and needs no extra passes, and it keeps hard pixel-art boundaries that a box blur
   softens. Named "xBR-style" in the UI for exactly that reason. */
"uint fetch_xbr(float tx, float ty) {\n"
"    if (fetch_texel(int(floor(tx)), int(floor(ty))) == 0u) return 0u;\n"
"    float bx = tx - 0.5, by = ty - 0.5;\n"
"    float x0 = floor(bx), y0 = floor(by);\n"
"    float fx = bx - x0, fy = by - y0;\n"
"    uint s00 = fetch_texel(int(x0), int(y0));\n"
"    uint s10 = fetch_texel(int(x0) + 1, int(y0));\n"
"    uint s01 = fetch_texel(int(x0), int(y0) + 1);\n"
"    uint s11 = fetch_texel(int(x0) + 1, int(y0) + 1);\n"
"    if (s00 == 0u || s10 == 0u || s01 == 0u || s11 == 0u) return fetch_smooth(tx, ty);\n"
"    vec3 lw = vec3(0.299, 0.587, 0.114);\n"
"    float lA = abs(dot(c5(s00), lw) - dot(c5(s11), lw));\n"
"    float lB = abs(dot(c5(s10), lw) - dot(c5(s01), lw));\n"
"    if (abs(lA - lB) < 1.0) return fetch_smooth(tx, ty);\n"
"    vec3 acc;\n"
"    if (lA < lB) {\n"
"        float t = clamp((fx + fy) * 0.5, 0.0, 1.0);\n"
"        acc = mix(c5(s00), c5(s11), t);\n"
"    } else {\n"
"        float t = clamp((fx - fy + 1.0) * 0.5, 0.0, 1.0);\n"
"        acc = mix(c5(s01), c5(s10), t);\n"
"    }\n"
"    uvec3 q = uvec3(clamp(acc, 0.0, 31.0));\n"
"    return q.x | (q.y << 5) | (q.z << 10) | ((s00 | s10 | s01 | s11) & 0x8000u);\n"
"}\n"
"uint fetch_bilinear(float tx, float ty) {\n"
"    float txf = floor(tx), tyf = floor(ty);\n"
"    float txc = txf + 1.0, tyc = tyf + 1.0;\n"
"    uint s0 = fetch_texel(int(txf), int(tyf));\n"
"    if (s0 == 0u) return 0u;\n"
"    uint s1 = fetch_texel(int(txc), int(tyf));\n"
"    uint s2 = fetch_texel(int(txf), int(tyc));\n"
"    uint s3 = fetch_texel(int(txc), int(tyc));\n"
"    vec3 c0 = vec3(float(s0 & 31u), float((s0 >> 5) & 31u), float((s0 >> 10) & 31u));\n"
"    vec3 c1 = vec3(float(s1 & 31u), float((s1 >> 5) & 31u), float((s1 >> 10) & 31u));\n"
"    vec3 c2 = vec3(float(s2 & 31u), float((s2 >> 5) & 31u), float((s2 >> 10) & 31u));\n"
"    vec3 c3 = vec3(float(s3 & 31u), float((s3 >> 5) & 31u), float((s3 >> 10) & 31u));\n"
"    vec3 q1 = c0 * (txc - tx) + c1 * (tx - txf);\n"
"    vec3 q2 = c2 * (txc - tx) + c3 * (tx - txf);\n"
"    uvec3 q = uvec3(q1 * (tyc - ty) + q2 * (ty - tyf));\n"
"    return q.x | (q.y << 5) | (q.z << 10) | ((s0 | s1 | s2 | s3) & 0x8000u);\n"
"}\n"
/*
   Barycentric divide, with the one case that matters made exact.

   gpu.c divides in C, where IEEE-754 division is correctly rounded to 0.5 ULP. GLSL ES 3.0
   only requires highp division to be accurate to 2.5 ULP (spec 4.5.1). Everywhere that does
   not matter — except at a TEXEL BOUNDARY, where the exact quotient is an integer and the
   two implementations therefore land on opposite sides of floor(). One flipped floor()
   selects a different texel, and adjacent texels in a font atlas are "opaque" and
   "transparent", so the disagreement is not a rounding step, it is a whole pixel appearing
   or vanishing. That is what the residual far/blank/extra buckets in
   the backend turned out to be, and why they show up as one-pixel-tall
   runs along scanlines.

   When the true quotient is the integer r, num == r*den exactly. Both sides of that test
   are exact for real PS1 triangle sizes (|z| and |area| stay well inside 2^24), so the
   comparison is reliable, and a correctly-rounded division of an exactly representable
   value returns it unchanged — which is precisely what gpu.c gets. When the test fails,
   nothing is claimed and the ordinary quotient stands, so this can only ever agree more.
*/
"float bdiv(float num, float den) {\n"
"    float q = num / den;\n"
"    float r = floor(q + 0.5);\n"
"    return (r * den == num) ? r : q;\n"
"}\n"
"bool tl(float z, vec2 a, vec2 b) {\n"
"    return (z < 0.0) || ((z == 0.0) && ((b.y > a.y) || ((b.y == a.y) && (b.x < a.x))));\n"
"}\n"
"void main() {\n"
"    uint flags = v_misc.x;\n"
"    int  mode  = int(v_misc.y);\n"
"    vec2  pf = gl_FragCoord.xy / u_scale - 0.5;\n"
"    ivec2 pn = ivec2(floor(gl_FragCoord.xy / u_scale));\n"
"    vec2 A = v_tri0.xy, B = v_tri0.zw, C = v_tri1;\n"
"    float z0 = 0.0, z1 = 0.0, z2 = 0.0, area = 1.0;\n"
"    bool sprite = (flags & F_SPRITE) != 0u;\n"
"    if (!sprite) {\n"
"        vec2 lo = min(A, min(B, C));\n"
"        vec2 hi = max(A, max(B, C));\n"
/* pf — the SUBPIXEL position — because the three edge tests below use pf, and the two halves
   of one coverage test must agree about granularity.
   With pn (the native pixel index) a subpixel genuinely inside the triangle could still be
   thrown away: a triangle starting at x = 100.2 owns the subpixel at 100.33, but pn is 100 and
   100 < 100.2 rejected the whole native pixel. The neighbouring triangle ends at 100.2 and does
   not cover it either, so nothing drew it — a gap along every shared edge, which is the seam.
   Identical at 1x, where gl_FragCoord.x = px + 0.5 makes pf.x exactly px. */
/* Coverage rejection is ACCUMULATED rather than discarded piecemeal so the probe below can
   paint rejected fragments instead of dropping them. With u_paint == 0 (shipping) the final
   discard fires for exactly the same set of fragments as the old early discards — the only
   cost is computing z1/z2 for fragments the bbox already rejected, which is noise. */
/* COVERAGE IS DECIDED ONCE PER NATIVE PIXEL, at the native pixel's own coordinate, exactly
   as psx/dev/gpu.c decides it — and the decision applies to every subpixel of the block.

   This is the resolution of the seam hunt, and the reasoning is worth keeping:
   the original code tested the bounding box at native granularity (pn) but the top-left edge
   rule at subpixel positions (pf); a later attempt made both subpixel. NEITHER matches the
   software rasterizer, and mixed or subpixel rules can strand a subpixel that no primitive
   claims — the probe (hwgl_paint_reject) showed exactly that: dark unclaimed hairlines
   between abutting accepted quads. Native-granularity coverage is the one rule PROVEN
   watertight: it reproduces software coverage identically at every scale (1x parity 0.005%),
   so a hole here requires a hole in the software image too.

   The cost is honest: polygon edges quantize to native pixels at upscale — textures and
   Gouraud shading still gain from the higher resolution (interpolation below stays at pf),
   but edge silhouettes step at native granularity. Smooth subpixel edges need real GPU
   rasterization with a watertight fill rule, which is the planned PGXP-era design, not a
   per-fragment discard rule.

   zc* are the coverage barycentrics at the native coordinate; z0..z2 (at pf) remain the
   INTERPOLATION barycentrics. float(pn) and integer vertices keep every zc product inside
   float-exact range, so the tie-break comparisons below are exact at every scale. */
"        vec2 pc = vec2(pn);\n"
"        float zc0, zc1, zc2;\n"
"        bool rej = false;\n"
"        if (pc.x < lo.x || pc.x >= hi.x) rej = true;\n"
"        if (pc.y < lo.y || pc.y >= hi.y) rej = true;\n"
"        zc0 = (C.x - B.x) * (pc.y - B.y) - (C.y - B.y) * (pc.x - B.x);\n"
"        if (tl(zc0, B, C)) rej = true;\n"
"        zc1 = (A.x - C.x) * (pc.y - C.y) - (A.y - C.y) * (pc.x - C.x);\n"
"        if (tl(zc1, C, A)) rej = true;\n"
"        zc2 = (B.x - A.x) * (pc.y - A.y) - (B.y - A.y) * (pc.x - A.x);\n"
"        if (tl(zc2, A, B)) rej = true;\n"
"        z0 = (C.x - B.x) * (pf.y - B.y) - (C.y - B.y) * (pf.x - B.x);\n"
"        z1 = (A.x - C.x) * (pf.y - C.y) - (A.y - C.y) * (pf.x - C.x);\n"
"        z2 = (B.x - A.x) * (pf.y - A.y) - (B.y - A.y) * (pf.x - A.x);\n"
"        area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);\n"
"        if (area == 0.0) rej = true;\n"
/* A native-accepted subpixel can sit slightly outside the triangle, where the pf
   barycentrics go negative — Gouraud/UV would EXTRAPOLATE there, and a weight clamp would
   darken (weights stop summing to the area: the same artefact class being fixed). Fall back
   to the native-centre barycentrics instead: zc* passed coverage, so they are inside by
   construction, and the edge subpixel takes exactly the value software gives that pixel.
   Interior subpixels keep smooth subpixel interpolation. At 1x, pf == pc, so nothing moves. */
"        if (min(z0, min(z1, z2)) < 0.0) { z0 = zc0; z1 = zc1; z2 = zc2; }\n"
/* Probe v2. v1 painted REJECTED fragments magenta and was misleading by construction: a later
   triangle's rejected bbox fragments overwrote an earlier triangle's correct pixels, so any
   dense mesh drowned in magenta regardless of correctness. Inverted: ACCEPTED non-sprite
   geometry paints flat green (sprites flat blue, below), as a plain write immune to blend
   state, BEFORE the texture/STP stages so texel-transparency cannot hide geometric coverage.
   Reading the hairlines is then unambiguous and order-proof:
     green hairline   -> the pixel WAS drawn by accepted geometry; the bug is in the value
                         path (texture fetch / interpolation / blending / pass split);
     dark hairline    -> no geometry accepted the pixel: a genuine coverage hole (or it was
                         never generated at all). */
"        if (rej) discard;\n"
"        if (u_paint != 0) { o_color = vec4(0.0, 248.0/255.0, 0.0, 0.0); return; }\n"
/* z0/z1/z2 stay as computed at pf and are reused for interpolation. They were briefly
   re-evaluated at the native pixel centre, which made things worse in a subtle way: coverage
   is decided per subpixel, so a triangle can own subpixels of a pixel whose CENTRE lies in its
   neighbour, and the barycentrics for that centre are negative — colour and UV then extrapolate
   outside the triangle. A subpixel that passed the tests above is by definition inside, so
   interpolating at the same position it was tested at is the only self-consistent choice. */
"    }\n"
"    if (sprite && u_paint != 0) { o_color = vec4(0.0, 0.0, 248.0/255.0, 0.0); return; }\n"
/* ---- mask CHECK, GP0(E6) bit 1 ----------------------------------------------------------
   "check mask before draw": the write is skipped where the DESTINATION already has bit 15.
   o_color is the destination here (framebuffer fetch), and the render target's alpha is the
   mask bit — 0.0 or 1.0 exactly, written by this shader and by nothing else that blends.

   Placed after coverage and before the texture fetch, mirroring the order in gpu.c:1180 and
   gpu_hw_rt.c:170 — where it is also a `continue` before anything is sampled. Per FRAGMENT,
   not per native pixel: gpu_hw_rt.c tests the render-target pixel too, and at 1x the two are
   the same pixel.  coverage contract is about GEOMETRY and is untouched by this. */
"#ifdef ARMSX_GL_MASK\n"
"    if (PSX_GPU_MASK_SKIP((flags & F_MASK_CHECK) != 0u, o_color.a >= 0.5)) discard;\n"
"#endif\n"
"    vec3 md;\n"
"    if ((flags & F_SHADED) != 0u) {\n"
"        vec3 n = z0 * vec3(v_col0.xyz) + z1 * vec3(v_col1.xyz) + z2 * vec3(v_col2.xyz);\n"
"        vec3 c = vec3(bdiv(n.x, area), bdiv(n.y, area), bdiv(n.z, area));\n"
/* Indexed by the ABSOLUTE native VRAM coordinate, matching gpu.c and gpu_hw_rt.c. It used
   to subtract the primitive's bounding-box origin, which re-phased the 4x4 kernel per
   primitive and put a seam at every shared edge of a gradient. pn is already the native
   coordinate (gl_FragCoord / u_scale), so this is scale-invariant for free. */
"        if (u_dither_on != 0 && (flags & F_DITHER) != 0u) {\n"
"            int dx = pn.x & 3;\n"
"            int dy = pn.y & 3;\n"
"            c += float(u_dither[dx + dy * 4]);\n"
"        }\n"
"        md = floor(clamp(c, 0.0, 255.0) + 0.5);\n"
"    } else {\n"
"        md = vec3(v_col0.xyz);\n"
"    }\n"
"    bool transp = (flags & F_TRANSP) != 0u;\n"
/* The SOURCE TEXEL's bit 15, which is the other half of GP0(E6) bit 0 and the half
   found missing. Untextured primitives leave it false and therefore write a 0 mask bit, which
   is what gpu.c does (its `stp` is initialised to 0 per pixel and only the textured branch
   assigns it). The filtered fetches return the OR of their taps' bit 15, exactly as
   gpu_fetch_texel_bilinear does, so this is the same bit the software path would take. */
"    bool stp = false;\n"
"    vec3 col;\n"
"    if ((flags & F_TEXTURED) != 0u) {\n"
"        uint texel;\n"
/* v_repl.y's high half is the replacement's integer upscale, and 0 means "none" -- which is
   what it is on every vertex unless [video] texture_replacements is on AND a pack file
   matched this primitive. psx/texrep.h. */
"        bool repl = (v_repl.y >> 16) != 0u;\n"
"        if (sprite) {\n"
/* A sprite walks one texel per NATIVE pixel, so the fragment's position inside its native
   pixel IS the sub-texel. Native space and gl_FragCoord share an orientation here (the
   vertex shader puts native y=0 at the bottom, where gl_FragCoord.y is 0), so no flip. */
"            if (repl) {\n"
"                texel = fetch_repl(float(int(v_uv01.x) + (pn.x - int(A.x))),\n"
"                                   float(int(v_uv01.y) + (pn.y - int(A.y))),\n"
"                                   fract(gl_FragCoord.xy / u_scale));\n"
"            } else {\n"
"                texel = fetch_texel(int(v_uv01.x) + (pn.x - int(A.x)),\n"
"                                    int(v_uv01.y) + (pn.y - int(A.y)));\n"
"            }\n"
"        } else {\n"
/* PGXP perspective-correct texturing. zN are the same unnormalized barycentrics the affine
   path uses; dividing each by its vertex w and renormalizing by the sum is the standard
   1/w-interpolation, so `area` is not needed in that branch. Every non-PGXP triangle ships
   triw = {1,1,1}, which keeps the exact bdiv path byte-identical — the 1x parity gate is
   meaningful only with PGXP off, by design (PGXP moves geometry on purpose). The UV clamp
   below applies to BOTH branches. */
"            float tx, ty;\n"
"            if (v_triw.x != v_triw.y || v_triw.x != v_triw.z) {\n"
"                float iw0 = z0 / v_triw.x, iw1 = z1 / v_triw.y, iw2 = z2 / v_triw.z;\n"
"                float wsum = iw0 + iw1 + iw2;\n"
"                tx = (iw0 * float(v_uv01.x) + iw1 * float(v_uv01.z) + iw2 * float(v_uv2.x)) / wsum;\n"
"                ty = (iw0 * float(v_uv01.y) + iw1 * float(v_uv01.w) + iw2 * float(v_uv2.y)) / wsum;\n"
"            } else {\n"
"                tx = bdiv(z0 * float(v_uv01.x) + z1 * float(v_uv01.z) + z2 * float(v_uv2.x), area);\n"
"                ty = bdiv(z0 * float(v_uv01.y) + z1 * float(v_uv01.w) + z2 * float(v_uv2.y), area);\n"
"            }\n"
/* Clamp UV to the triangle's OWN vertex range.
   Coverage is decided per subpixel while colour and UV are interpolated at the native pixel
   CENTRE, so a triangle can legitimately own subpixels of a pixel whose centre lies just
   inside its neighbour. The barycentrics for that centre are then slightly negative and the
   interpolated UV walks off the end of the intended texels, into whatever sits next to them
   in VRAM — a dark fringe on exactly the pixels along a shared edge.
   Clamping to the triangle's own three UVs cannot change a correctly-interpolated pixel,
   because any point genuinely inside the triangle already interpolates within that range. It
   only bounds the extrapolated ones. */
"            float ulo = min(float(v_uv01.x), min(float(v_uv01.z), float(v_uv2.x)));\n"
"            float uhi = max(float(v_uv01.x), max(float(v_uv01.z), float(v_uv2.x)));\n"
"            float vlo = min(float(v_uv01.y), min(float(v_uv01.w), float(v_uv2.y)));\n"
"            float vhi = max(float(v_uv01.y), max(float(v_uv01.w), float(v_uv2.y)));\n"
"            tx = clamp(tx, ulo, uhi);\n"
"            ty = clamp(ty, vlo, vhi);\n"
/* Polygons only. Sprites keep the integer texel walk above: their UV never has a fractional
   part, so a filter would be a no-op on them without also synthesising a sub-texel position —
   and doing that softens exactly the 2D HUD/text a player wants pixel-exact. */
/* A bound replacement BYPASSES u_filter and is point-sampled at its own resolution -- the
   same rule psx_gpu_filter_active() states for the two CPU rasterizers, and for the same two
   reasons: the filter exists to hide the size of a NATIVE texel, and three filter kernels
   would otherwise have to agree over the atlas as well as over VRAM. psx/texrep.h. */
"            if (repl)               texel = fetch_repl(tx, ty, fract(vec2(tx, ty)));\n"
"            else if (u_filter == 1) texel = fetch_smooth(tx, ty);\n"
"            else if (u_filter == 2) texel = fetch_xbr(tx, ty);\n"
/* Mode 0 is NEAREST and must actually point-sample — it previously called fetch_bilinear(),
   so "nearest" filtered, matching the other two rasterizers' identical bug. All three now
   point-sample at 0, so the 1x parity gate still compares like with like. */
"            else                    texel = fetch_texel(int(floor(tx)), int(floor(ty)));\n"
"        }\n"
"        if (texel == 0u) discard;\n"
"        stp = (texel & 0x8000u) != 0u;\n"
"        if ((flags & F_TRANSP) != 0u) transp = stp;\n"
"        if ((flags & F_RAW) != 0u) {\n"
"            col = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u));\n"
"        } else {\n"
"            vec3 t = vec3(float((texel & 31u) << 3u), float(((texel >> 5) & 31u) << 3u),\n"
"                          float(((texel >> 10) & 31u) << 3u));\n"
/* Hardware divides by 128 with integer truncation (psx-spx). This shader rounded, which
   makes levels 1..8 fixed points and a frame-feedback trail permanent — see
   psx_gpu_modulate_channel() in psx/dev/gpu.h and the backend. t and md
   are integer-valued and t*md <= 63240, so t*md/128.0 is EXACT in float (128 is a power of
   two) and floor() cannot land a level low. */
"            vec3 c = (u_tex_trunc != 0) ? floor(clamp(t * md / 128.0, 0.0, 255.0))\n"
"                                        : floor(clamp(t * md / 128.0, 0.0, 255.0) + 0.5);\n"
"            col = floor(c / 8.0);\n"
"        }\n"
"    } else {\n"
"        col = floor(md / 8.0);\n"
"    }\n"
"    if (u_stp_pass == 1 && transp) discard;\n"
"    if (u_stp_pass == 2 && !transp) discard;\n"
/* col is 0..31 per channel. Emitting c5*8 keeps every blend result a multiple of 4, so
   the >> 3 at scanout recovers the software's integer answer exactly. */
"    vec3 F = col * 8.0;\n"
"    float a = 0.0;\n"
"    if (transp) {\n"
"        if (mode == 0)      { F *= 0.5;  a = 0.5; }\n"
"        else if (mode == 3) { F *= 0.25; a = 1.0; }\n"
"        else                {            a = 1.0; }\n"
"    }\n"
"#ifdef ARMSX_GL_MASK\n"
/* ---- mask SET, GP0(E6) bit 0 -- and, unavoidably, the blend --------------------------------
    "alpha-channel conflict", stated there and resolved here: `.a` is either the
   fixed-function SRC_ALPHA blend factor or the mask bit, and it cannot be both. The mask bit
   wins, so the blend moves into the shader — which framebuffer fetch already made possible,
   since o_color arrives holding the destination.

   The arithmetic is the SAME `a` the fixed-function unit would have used, so this reproduces
   the four modes without a second table: ADD is `F + B*a` and mode 2's reverse-subtract is
   `B*a - F`. `transp && mode == 2` selects exactly the fragments the SUB range covered, so
   the existing u_stp_pass split stays correct and is left alone.

   B is recovered as an EXACT 8-bit integer, and the result is re-truncated to 5 bits before
   it is stored. That second step is not cosmetic: psx/dev/gpu.c blends in 8 bits and then
   packs to BGR555, so its destination is 5-bit, while the RGBA8 target kept 8 — the "a pixel
   blended more than once can differ by one 5-bit step" divergence in the file header. Doing
   the blend here makes matching it free, so the mask path does. */
/* `dst`, not `B` — A/B/C are already the triangle's three vertices in this scope. */
"    vec3 dst = floor(o_color.rgb * 255.0 + 0.5);\n"
"    vec3 blended = (transp && mode == 2) ? (dst * a - F) : (F + dst * a);\n"
"    blended = floor(clamp(blended, 0.0, 255.0) + 0.5);\n"
"    blended = floor(blended / 8.0) * 8.0;\n"
"    bool mbit = PSX_GPU_MASK_WRITE((flags & F_MASK_SET) != 0u, u_mask_texel != 0, stp);\n"
"    o_color = vec4(blended / 255.0, mbit ? 1.0 : 0.0);\n"
"#else\n"
/* The shipping path, textually unchanged: the fixed-function unit does the blend, `.a` is its
   factor, and the render target has nowhere to keep bit 15. */
"    o_color = vec4(F / 255.0, a);\n"
"#endif\n"
"}\n";

/* GP0(A0) upload and the initial/load-state seed: replicate a native VRAM rectangle into
   the upscaled target. Issued with GL_BLEND OFF (gl_blit_from_vram), so alpha is free to
   carry the source halfword's bit 15 straight into the target's mask channel — which is what
   gpu.c:2282 does, it stores the uploaded halfword verbatim, bit 15 included. With the mask
   bit off nothing ever reads that alpha and the RGB written is identical either way. */
static const char* kXferVS =
"#version 300 es\n"
"precision highp float;\n"
"uniform vec4 u_rect;\n"
"uniform vec2 u_rt_size;\n"
"uniform float u_scale;\n"
"void main() {\n"
"    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
"    vec2 p = (u_rect.xy + c * u_rect.zw) * u_scale;\n"
"    gl_Position = vec4(p / u_rt_size * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

static const char* kXferFS =
"#version 300 es\n"
"precision highp float;\n"
"precision highp int;\n"
"precision highp usampler2D;\n"
"uniform highp usampler2D u_vram;\n"
"uniform float u_scale;\n"
"out vec4 o_color;\n"
"void main() {\n"
"    ivec2 pn = ivec2(floor(gl_FragCoord.xy / u_scale));\n"
"    uint t = texelFetch(u_vram, ivec2(pn.x & 1023, pn.y & 511), 0).r;\n"
"    o_color = vec4(float((t & 31u) << 3u) / 255.0, float(((t >> 5) & 31u) << 3u) / 255.0,\n"
"                   float(((t >> 10) & 31u) << 3u) / 255.0, float((t >> 15) & 1u));\n"
"}\n";

/* Resolve: pack a region of the render target back down to BGR555 on the GPU so a readback
   is 2 bytes per pixel and needs no CPU conversion at all. Two callers, one shader:

     * SCANOUT — u_step = 1. Every render-target texel becomes one output texel, so the
       result is the upscaled display region, which is what both the present texture and the
       old glReadPixels path want.
     * NATIVE READBACK — u_step = S. One output texel per NATIVE VRAM texel, sampling the
       sub-texel at +S/2 (the centre-ish one; at S == 1 that is +0, so 1x is bit-for-bit the
       scanout path and the parity gate still measures the same pixels). This is what
       GP0(C0), the save-state flush and  downgrade seed need: host VRAM is native, so
       the downsample has to happen somewhere and doing it on the GPU keeps the transfer to
       2 bytes per native texel instead of 2·S².
*/
static const char* kResolveVS =
"#version 300 es\n"
"precision highp float;\n"
"void main() {\n"
"    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
"    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

static const char* kResolveFS =
"#version 300 es\n"
"precision highp float;\n"
"precision highp int;\n"
"uniform sampler2D u_rt;\n"
"uniform ivec2 u_origin;\n"
"uniform ivec2 u_limit;\n"
"uniform int u_step;\n"
/* [video] downsample. 1 = the historical single-tap behaviour, textually unchanged in the
   else-branch below so the scanout and the native readback are bit-identical to before the
   key existed. N > 1 box-averages the NxN render-target block whose top-left corner the step
   landed on — the same average gl_parity_check_upscaled() computes on the CPU (
   instrument), which is why that harness is the reference for this resolve.
   The loop is bounded by a compile-time constant with a break, not by u_box directly: GLSL ES
   3.00 allows a dynamic bound, but drivers unroll a constant one and this shader runs once
   per output pixel of every frame. */
"uniform int u_box;\n"
/* 1 while the mask bit is on: the target's alpha IS VRAM bit 15, so put it back. 0 restores
   the original packing exactly, because with the mask bit off that alpha is the residue of
   the fixed-function blend factor and means nothing. The box branch cannot produce a
   meaningful mask bit (it averages S x S native pixels) and writes 0 there. */
"uniform int u_mask;\n"
"out vec4 o_color;\n"
"void main() {\n"
"    ivec2 p = ivec2(gl_FragCoord.xy) * u_step + u_origin;\n"
"    vec4 c;\n"
"    if (u_box > 1) {\n"
"        vec3 acc = vec3(0.0);\n"
"        for (int by = 0; by < 8; by++) {\n"
"            if (by >= u_box) break;\n"
"            for (int bx = 0; bx < 8; bx++) {\n"
"                if (bx >= u_box) break;\n"
"                acc += texelFetch(u_rt, clamp(p + ivec2(bx, by), ivec2(0), u_limit), 0).rgb;\n"
"            }\n"
"        }\n"
"        c = vec4(acc / float(u_box * u_box), 0.0);\n"
"    } else {\n"
"        c = texelFetch(u_rt, clamp(p, ivec2(0), u_limit), 0);\n"
"    }\n"
"    uint r = uint(c.r * 255.0 + 0.5) >> 3;\n"
"    uint g = uint(c.g * 255.0 + 0.5) >> 3;\n"
"    uint b = uint(c.b * 255.0 + 0.5) >> 3;\n"
"    uint v = r | (g << 5) | (b << 10);\n"
"    if (u_mask != 0 && c.a >= 0.5) v |= 0x8000u;\n"
"    o_color = vec4(float(v & 255u) / 255.0, float((v >> 8) & 255u) / 255.0, 0.0, 1.0);\n"
"}\n";

/*
    GPU-to-GPU resolve needed before the software shadow can be dropped.

    Identical arithmetic to kResolveFS, but it writes a UINT rather than two normalised
    bytes, because its destination is vram_tex — the R16UI native VRAM mirror the draw
    shader samples. R16UI is colour-renderable in GLES 3.0, so this is an FBO and a draw:
    no glReadPixels, no CPU round trip, no pipeline stall. The 5-bit values recovered here
    are exactly what a readback would produce (: the draw shader emits c5*8, every blend
    result stays a multiple of 4, and >> 3 recovers the software's integer answer).

    Bit 15 comes from the target's ALPHA once the mask bit is on (u_mask), and is written as 0
    otherwise — which is the KNOWN divergence the file shipped with: with no mask channel the
    RGBA8 render target cannot keep it, and gpu.c:800 does preserve a source texel's bit 15
    through a raw textured write, so a game that renders a sprite sheet on the GPU and then
    samples it back with semi-transparency loses the per-texel STP flag. The parity gate masks
    bit 15 off on both sides in that state. With the mask bit on there is a real channel to
    read and the round trip is lossless.
*/
static const char* kGResFS =
"#version 300 es\n"
"precision highp float;\n"
"precision highp int;\n"
"uniform sampler2D u_rt;\n"
"uniform ivec2 u_limit;\n"
"uniform int u_step;\n"
"uniform int u_half;\n"
"uniform int u_mask;\n"
"out uvec4 o_val;\n"
"void main() {\n"
"    ivec2 p = clamp(ivec2(gl_FragCoord.xy) * u_step + ivec2(u_half), ivec2(0), u_limit);\n"
"    vec4 c = texelFetch(u_rt, p, 0);\n"
"    uint r = uint(c.r * 255.0 + 0.5) >> 3;\n"
"    uint g = uint(c.g * 255.0 + 0.5) >> 3;\n"
"    uint b = uint(c.b * 255.0 + 0.5) >> 3;\n"
"    uint v = r | (g << 5) | (b << 10);\n"
"    if (u_mask != 0 && c.a >= 0.5) v |= 0x8000u;\n"
"    o_val = uvec4(v, 0u, 0u, 0u);\n"
"}\n";

/* ---- tile helpers -------------------------------------------------------------------- */

static void tiles_clear(tilemap_t* t) {
    memset(t, 0, sizeof(*t));
}

/* Marks the tiles a native-coordinate rectangle touches. Coordinates are clamped rather
   than wrapped: a transfer that runs off the edge is dropped by the core too. */
static void tiles_mark(tilemap_t* t, int x, int y, int w, int h) {
    int x0, y0, x1, y1, ty;

    if ((w <= 0) || (h <= 0))
        return;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w; if (x1 > 1024) x1 = 1024;
    y1 = y + h; if (y1 > 512)  y1 = 512;

    if ((x0 >= x1) || (y0 >= y1))
        return;

    x0 >>= TILE_SHIFT; y0 >>= TILE_SHIFT;
    x1 = (x1 - 1) >> TILE_SHIFT; y1 = (y1 - 1) >> TILE_SHIFT;

    for (ty = y0; ty <= y1; ty++) {
        uint64_t mask;

        if (x1 >= 63)
            mask = ~(uint64_t)0 << x0;
        else
            mask = ((((uint64_t)1 << (x1 + 1)) - 1) & ~((((uint64_t)1 << x0) - 1)));

        t->row[ty] |= mask;
    }
}

static int tiles_intersects(const tilemap_t* t, int x, int y, int w, int h) {
    int x0, y0, x1, y1, ty;

    if ((w <= 0) || (h <= 0))
        return 0;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w; if (x1 > 1024) x1 = 1024;
    y1 = y + h; if (y1 > 512)  y1 = 512;

    if ((x0 >= x1) || (y0 >= y1))
        return 0;

    x0 >>= TILE_SHIFT; y0 >>= TILE_SHIFT;
    x1 = (x1 - 1) >> TILE_SHIFT; y1 = (y1 - 1) >> TILE_SHIFT;

    for (ty = y0; ty <= y1; ty++) {
        uint64_t mask;

        if (x1 >= 63)
            mask = ~(uint64_t)0 << x0;
        else
            mask = ((((uint64_t)1 << (x1 + 1)) - 1) & ~((((uint64_t)1 << x0) - 1)));

        if (t->row[ty] & mask)
            return 1;
    }

    return 0;
}

/* ---- program helpers ----------------------------------------------------------------- */

static GLuint gl_compile(hw_gl_t* g, GLenum type, const char* src, const char* label) {
    GLuint shader = g->gl.CreateShader(type);
    GLint  ok = 0;

    if (!shader)
        return 0;

    g->gl.ShaderSource(shader, 1, &src, NULL);
    g->gl.CompileShader(shader);
    g->gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char log[1024];
        GLsizei written = 0;

        log[0] = '\0';
        g->gl.GetShaderInfoLog(shader, (GLsizei)sizeof(log), &written, log);
        log[sizeof(log) - 1] = '\0';
        gl_status("shader %s failed to compile: %s", label, log);
        g->gl.DeleteShader(shader);

        return 0;
    }

    return shader;
}

static const char* kDrawAttribs[] = {
    "a_pos", "a_tri0", "a_tri1", "a_col0", "a_col1", "a_col2",
    "a_uv01", "a_uv2", "a_misc", "a_texwin", "a_triw", "a_repl"
};

static GLuint gl_link(hw_gl_t* g, const char* vs_src, const char* fs_src,
                      const char* const* attribs, int attrib_count, const char* label) {
    GLuint vs = gl_compile(g, GL_VERTEX_SHADER, vs_src, label);
    GLuint fs = 0;
    GLuint prog = 0;
    GLint  ok = 0;
    int    i;

    if (!vs)
        return 0;

    fs = gl_compile(g, GL_FRAGMENT_SHADER, fs_src, label);

    if (!fs) {
        g->gl.DeleteShader(vs);
        return 0;
    }

    prog = g->gl.CreateProgram();

    if (!prog) {
        g->gl.DeleteShader(vs);
        g->gl.DeleteShader(fs);
        return 0;
    }

    g->gl.AttachShader(prog, vs);
    g->gl.AttachShader(prog, fs);

    /* Explicit binding rather than layout(location=) qualifiers: both are legal in GLSL ES
       3.00, but this keeps the C-side attribute indices and the shader in one place. */
    for (i = 0; i < attrib_count; i++)
        g->gl.BindAttribLocation(prog, (GLuint)i, attribs[i]);

    g->gl.LinkProgram(prog);
    g->gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    g->gl.DeleteShader(vs);
    g->gl.DeleteShader(fs);

    if (!ok) {
        char log[1024];
        GLsizei written = 0;

        log[0] = '\0';
        g->gl.GetProgramInfoLog(prog, (GLsizei)sizeof(log), &written, log);
        log[sizeof(log) - 1] = '\0';
        gl_status("program %s failed to link: %s", label, log);
        g->gl.DeleteProgram(prog);

        return 0;
    }

    return prog;
}

/* Whole-word search of GL_EXTENSIONS. glGetStringi() is the GLES 3.0 way, but glGetString()
   still returns the space-separated list there and this file dlsym's its entry points one by
   one — one fewer symbol to load, and a substring match would say yes to
   GL_EXT_shader_framebuffer_fetch_non_coherent, which needs a barrier this backend does not
   issue and would therefore read stale destination colour. */
static int gl_has_extension(hw_gl_t* g, const char* name) {
    const unsigned char* list = g->gl.GetString ? g->gl.GetString(GL_EXTENSIONS) : NULL;
    const char* p = (const char*)list;
    const size_t n = strlen(name);

    while (p && *p) {
        const char* end;

        while (*p == ' ')
            p++;

        end = strchr(p, ' ');

        if (!end)
            end = p + strlen(p);

        if (((size_t)(end - p) == n) && (memcmp(p, name, n) == 0))
            return 1;

        p = end;
    }

    return 0;
}

/*
    The draw fragment shader's source, assembled.

    Two things have to precede every other token in a GLSL ES translation unit — `#version`
    and `#extension` — so neither can live in kDrawFS, and the mask stage's two expressions
    come from psx/dev/gpu.h rather than from this file (PSX_GPU_MASK_GLSL: the SAME macro
    bodies the software rasterizer's contract test drives, stringified). The caller owns the
    result and frees it after linking.
*/
static char* gl_build_draw_fs(int mask_mode) {
    static const char* kVersion = "#version 300 es\n";
    static const char* kMaskHead =
        "#extension GL_EXT_shader_framebuffer_fetch : require\n"
        "#define ARMSX_GL_MASK 1\n"
        PSX_GPU_MASK_GLSL;
    /* Unconditional: fetch_repl() in kDrawFS calls texrep_at()/texrep_pack(), and the
       replacement path is switched off by v_repl carrying zero, not by recompiling. Same
       construction as the mask stage — the ONE definition of the atlas mapping lives in
       psx/texrep.h and both the shader and psx_texrep_sample() are built from it. */
    static const char* kTexrep = PSX_TEXREP_GLSL;
    const char* head = mask_mode ? kMaskHead : "";
    const size_t len = strlen(kVersion) + strlen(head) + strlen(kTexrep) +
                       strlen(kDrawFS) + 1u;
    char* out = (char*)malloc(len);

    if (!out)
        return NULL;

    out[0] = '\0';
    strcat(out, kVersion);
    strcat(out, head);
    strcat(out, kTexrep);
    strcat(out, kDrawFS);

    return out;
}

/* ---- render-target binding ----------------------------------------------------------- */

/*
    frontend/render_gl.cpp's present path never calls glBindFramebuffer and its loader table
    contains no framebuffer entry points at all — it structurally assumes FBO 0 is bound. If
    this backend leaves its own FBO bound on the way out, present renders into it and the
    screen goes black with NO GL error. Rebinding 0 is entirely on us, which is what
    gl_release() is for, and it is called from both end_frame() and display_buffer().
*/
static void gl_bind_rt(hw_gl_t* g) {
    if (g->rt_bound)
        return;

    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->rt_fbo);
    g->gl.Viewport(0, 0, g->rt_w, g->rt_h);
    g->gl.Disable(GL_DEPTH_TEST);
    g->gl.Disable(GL_STENCIL_TEST);
    g->gl.Disable(GL_CULL_FACE);
    g->gl.Disable(GL_DITHER);

    /* In mask mode the fragment shader does the blend itself (framebuffer fetch), because the
       alpha channel is the mask bit and can no longer be the SRC_ALPHA factor. Leaving the
       blend unit on would then apply a SECOND blend on top of the shader's answer, with the
       mask bit as its factor — a corruption that looks like "transparency is wrong sometimes",
       which is why it is turned off in the one place every path passes through. */
    if (g->mask_mode) {
        g->gl.Disable(GL_BLEND);
    } else {
        g->gl.Enable(GL_BLEND);
        g->gl.BlendFunc(GL_ONE, GL_SRC_ALPHA);
        g->gl.BlendEquation(GL_FUNC_ADD);
    }

    g->gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g->rt_bound = 1;
}

/* Deliberately NOT short-circuited on rt_bound. gl_display_buffer() and the two
   ensure_*() helpers bind a DIFFERENT framebuffer of ours and clear rt_bound, so an
   early-out here would leave that one bound and present would render into it — a black
   screen with no GL error, which is precisely the failure the comment above describes. */
static void gl_release(hw_gl_t* g) {
    g->gl.Disable(GL_SCISSOR_TEST);
    g->gl.Disable(GL_BLEND);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g->gl.BindVertexArray(0);
    g->gl.UseProgram(0);
    g->gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    g->rt_bound = 0;
}

/* ---- vram texture sync ---------------------------------------------------------------- */

/* Uploads every dirty tile that overlaps the requested rectangle and clears those bits.
   Called only when a textured primitive is about to sample a region host VRAM has written
   since the last sync — the framebuffer being permanently dirty costs nothing as long as
   nothing samples it, which is the normal case. */
static void gl_sync_vram(hw_gl_t* g, int x, int y, int w, int h) {
    const uint16_t* vram = g->gpu->vram;
    int x0, y0, x1, y1, ty;

    if (!g->any_dirty || (w <= 0) || (h <= 0))
        return;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w; if (x1 > 1024) x1 = 1024;
    y1 = y + h; if (y1 > 512)  y1 = 512;

    if ((x0 >= x1) || (y0 >= y1))
        return;

    x0 >>= TILE_SHIFT; y0 >>= TILE_SHIFT;
    x1 = (x1 - 1) >> TILE_SHIFT; y1 = (y1 - 1) >> TILE_SHIFT;

    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->vram_tex);
    g->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    g->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 1024);

    for (ty = y0; ty <= y1; ty++) {
        uint64_t mask;
        uint64_t hit;
        int lo;

        if (x1 >= 63)
            mask = ~(uint64_t)0 << x0;
        else
            mask = ((((uint64_t)1 << (x1 + 1)) - 1) & ~((((uint64_t)1 << x0) - 1)));

        hit = g->dirty.row[ty] & mask;

        if (!hit)
            continue;

        /*
            One upload per CONTIGUOUS RUN of dirty tiles, not one per row-span.

            The span version — first set bit to last set bit, everything in between along
            for the ride — was correct only because host VRAM was authoritative everywhere,
            so re-uploading a clean tile was a no-op. Once the render target owns rendered
            content that stops being true: a clean tile inside the span may hold texels that
            exist ONLY on the GPU, and uploading stale host VRAM over it silently corrupts
            them with nothing left marked to repair it. Exact runs never touch a tile that
            was not marked.
        */
        lo = x0;

        while (lo <= x1) {
            int hi;
            int px, py, pw, ph;

            if (!(hit & ((uint64_t)1 << lo))) {
                lo++;
                continue;
            }

            hi = lo;

            while ((hi + 1 <= x1) && (hit & ((uint64_t)1 << (hi + 1))))
                hi++;

            px = lo << TILE_SHIFT;
            py = ty << TILE_SHIFT;
            pw = ((hi + 1) << TILE_SHIFT) - px;
            ph = 1 << TILE_SHIFT;

            if (px + pw > 1024) pw = 1024 - px;
            if (py + ph > 512)  ph = 512 - py;

            g->gl.TexSubImage2D(GL_TEXTURE_2D, 0, px, py, pw, ph, GL_RED_INTEGER,
                                GL_UNSIGNED_SHORT, vram + px + (size_t)py * 1024);

            g->stat_syncs++;
            g->stat_sync_px += (uint32_t)(pw * ph);

            lo = hi + 1;
        }

        g->dirty.row[ty] &= ~hit;
    }

    g->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    g->any_dirty = 0;
    for (ty = 0; ty < TILES_Y; ty++) {
        if (g->dirty.row[ty]) {
            g->any_dirty = 1;
            break;
        }
    }
}

static void gl_mark_dirty(hw_gl_t* g, int x, int y, int w, int h) {
    tiles_mark(&g->dirty, x, y, w, h);
    g->any_dirty = 1;
}

/* ----  row 1: the GPU -> GPU resolve ------------------------------------------------ */

/*  ladder, defined next to the seed it walks. A resolve that cannot be served is a
   correctness failure once the shadow is gone, so this is reachable from here. */
static void gl_downgrade(hw_gl_t* g, const char* reason);

/* vram_tex as a render target. Lazily, because it is only ever needed once something
   actually samples a region the rasterizer drew, and a driver that refuses R16UI as a
   colour attachment must not take the whole backend down at create() time on a build where
   the shadow is still doing the work. */
static int gl_ensure_vram_fbo(hw_gl_t* g) {
    if (g->vram_fbo_state)
        return g->vram_fbo_state > 0;

    g->gl.GenFramebuffers(1, &g->vram_fbo);

    if (!g->vram_fbo) {
        g->vram_fbo_state = -1;
        return 0;
    }

    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->vram_fbo);
    g->gl.FramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                g->vram_tex, 0);
    g->rt_bound = 0;

    if (g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        psxe_diag_logf("hwgl", "vram_tex (R16UI) is not framebuffer-complete;  row 1's "
                               "GPU->GPU resolve is unavailable");
        g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        g->gl.DeleteFramebuffers(1, &g->vram_fbo);
        g->vram_fbo = 0;
        g->vram_fbo_state = -1;
        return 0;
    }

    g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g->vram_fbo_state = 1;

    return 1;
}

/*
    Mirror image of gl_sync_vram(): same tile walk,
    same "one span per tile row" collapse, but the source is the render target and the
    transfer never leaves the GPU.

    The caller must have flushed — this binds a different framebuffer and rebinds TEXTURE0,
    and rendering into vram_tex while the draw program still samples it is undefined.
*/
static void gl_resolve_gpu_tiles(hw_gl_t* g, int x, int y, int w, int h) {
    int x0, y0, x1, y1, ty;

    if (!g->any_gpu_dirty || (w <= 0) || (h <= 0))
        return;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w; if (x1 > 1024) x1 = 1024;
    y1 = y + h; if (y1 > 512)  y1 = 512;

    if ((x0 >= x1) || (y0 >= y1))
        return;

    x0 >>= TILE_SHIFT; y0 >>= TILE_SHIFT;
    x1 = (x1 - 1) >> TILE_SHIFT; y1 = (y1 - 1) >> TILE_SHIFT;

    if (!gl_ensure_vram_fbo(g)) {
        /* Nothing correct can be done here. With the shadow on, host VRAM is still right
           and the caller's gl_sync_vram() has already produced the right texels, so this is
           only a lost optimisation. Without it, the picture would be silently wrong, which
           is precisely what  ladder exists for. */
        if (!g->shadow)
            gl_downgrade(g, "vram_tex is not renderable, so  row 1 cannot be served");

        g->any_gpu_dirty = 0;
        tiles_clear(&g->gpu_dirty);
        return;
    }

    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->vram_fbo);
    g->rt_bound = 0;
    g->gl.Viewport(0, 0, 1024, 512);
    g->gl.Disable(GL_BLEND);
    g->gl.Enable(GL_SCISSOR_TEST);
    g->gl.UseProgram(g->prog_gres);
    g->gl.Uniform1i(g->u_gres_rt, 0);
    g->gl.Uniform2i(g->u_gres_limit, g->rt_w - 1, g->rt_h - 1);
    g->gl.Uniform1i(g->u_gres_step, g->scale);
    /* Same sub-texel choice as gl_readback_rect(): the centre-ish one, +0 at S == 1. */
    g->gl.Uniform1i(g->u_gres_half, g->scale / 2);
    g->gl.Uniform1i(g->u_gres_mask, g->mask_mode);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->rt_tex);
    g->gl.BindVertexArray(g->quad_vao);

    for (ty = y0; ty <= y1; ty++) {
        uint64_t mask;
        uint64_t hit;
        int lo;

        if (x1 >= 63)
            mask = ~(uint64_t)0 << x0;
        else
            mask = ((((uint64_t)1 << (x1 + 1)) - 1) & ~((((uint64_t)1 << x0) - 1)));

        hit = g->gpu_dirty.row[ty] & mask;

        if (!hit)
            continue;

        /*
            EXACT RUNS, and this is the whole correctness of the pass.

            The first version resolved first-set-bit to last-set-bit, which drags every
            clean tile in between through the render target. That round trip is not
            lossless: the RGBA8 target has no bit 15, so a 16-bit texture texel loses its
            STP flag and — far worse — a 4bpp texture word loses the high bit of one of its
            four PALETTE INDICES. Sampling a page whose untouched half had been laundered
            that way is how a 0.2 % whole-VRAM divergence turned into 4.92 % of the display
            window disagreeing (measured, ). A tile that the rasterizer did not write
            must never be touched.
        */
        lo = x0;

        while (lo <= x1) {
            int hi;
            int px, py, pw, ph;

            if (!(hit & ((uint64_t)1 << lo))) {
                lo++;
                continue;
            }

            hi = lo;

            while ((hi + 1 <= x1) && (hit & ((uint64_t)1 << (hi + 1))))
                hi++;

            px = lo << TILE_SHIFT;
            py = ty << TILE_SHIFT;
            pw = ((hi + 1) << TILE_SHIFT) - px;
            ph = 1 << TILE_SHIFT;

            if (px + pw > 1024) pw = 1024 - px;
            if (py + ph > 512)  ph = 512 - py;

            g->gl.Scissor(px, py, pw, ph);
            g->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            g->stat_gres++;
            g->stat_gres_px += (uint32_t)(pw * ph);

            lo = hi + 1;
        }

        g->gpu_dirty.row[ty] &= ~hit;

        /* A tile whose content now comes from the render target must NOT be overwritten by
           a later host upload of the same tile: host VRAM does not have the rendered
           texels. gl_sync_vram() runs first by construction (gl_note_sample), so clearing
           the CPU-dirty bit here is the correct resolution of "both maps claim this tile". */
        g->dirty.row[ty] &= ~hit;
    }

    g->any_gpu_dirty = 0;
    for (ty = 0; ty < TILES_Y; ty++) {
        if (g->gpu_dirty.row[ty]) {
            g->any_gpu_dirty = 1;
            break;
        }
    }

    g->any_dirty = 0;
    for (ty = 0; ty < TILES_Y; ty++) {
        if (g->dirty.row[ty]) {
            g->any_dirty = 1;
            break;
        }
    }

    g->gl.Disable(GL_SCISSOR_TEST);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

/*
    A region the RASTERIZER just wrote.

    With the shadow on, gpu.c has already put those texels into host VRAM, so marking them
    CPU-dirty and re-uploading is both correct and cheaper than a resolve. With the shadow
    off (or `hwgl_gpu_resolve` armed) the render target is the only copy that has them, and
    the same mark has to go into gpu_dirty instead — nothing else in this file changes.
*/
static void gl_mark_drawn(hw_gl_t* g, int x, int y, int w, int h) {
    if (g->gpu_own) {
        /*
            CLIPPED TO THE DRAWING AREA, and that clip is load-bearing rather than tidy.

            A primitive's bounding box is not what it writes: gpu.c:322-325 rejects every
            pixel outside [draw_x1,draw_x2] x [draw_y1,draw_y2] and gl_range_for() scissors
            to the same rectangle, so a large polygon clipped to a 512x240 drawing area can
            have a bounding box covering half of VRAM while touching none of it.

            Over-marking is free when the mark means "re-upload this from host VRAM" — the
            data is identical, so the extra work is wasted, not wrong. It is NOT free when
            the mark means "launder this tile through the render target": the RGBA8 target
            has no bit 15, so an untouched tile carrying a 4bpp TEXTURE loses the high bit
            of one palette index in four and an untouched 16-bit texel loses its STP flag.
            That is what took the 1x parity window from 0.4708 % to 4.918 % () — not
            a rasterization bug at all, a tilemap that claimed more than the rasterizer
            wrote.
        */
        const psx_gpu_t* gpu = g->gpu;
        int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        const int dx0 = (int)gpu->draw_x1;
        const int dy0 = (int)gpu->draw_y1;
        const int dx1 = (int)gpu->draw_x2 + 1;   /* draw_x2 is INCLUSIVE */
        const int dy1 = (int)gpu->draw_y2 + 1;

        if (x0 < dx0) x0 = dx0;
        if (y0 < dy0) y0 = dy0;
        if (x1 > dx1) x1 = dx1;
        if (y1 > dy1) y1 = dy1;

        if (g->dbg_geom && (g->frames == 3500) && (g->geom_draw_left > 0) &&
            ((y + h > 256) || (y0 >= 256))) {
            g->geom_draw_left--;
            psxe_diag_logf("hwgl",
                           "geom-draw: bbox=(%d,%d %dx%d) clipped=(%d,%d %dx%d) "
                           "drawarea=(%u,%u)-(%u,%u)",
                           x, y, w, h, x0, y0, x1 - x0, y1 - y0,
                           gpu->draw_x1, gpu->draw_y1, gpu->draw_x2, gpu->draw_y2);
        }

        if ((x0 >= x1) || (y0 >= y1))
            return;

        tiles_mark(&g->gpu_dirty, x0, y0, x1 - x0, y1 - y0);
        g->any_gpu_dirty = 1;
        return;
    }

    gl_mark_dirty(g, x, y, w, h);
}

/* GP0(80)'s destination, which the drawing area does NOT clip (gpu.c:1977-1984). */
static void gl_mark_gpu_dirty(hw_gl_t* g, int x, int y, int w, int h) {
    tiles_mark(&g->gpu_dirty, x, y, w, h);
    g->any_gpu_dirty = 1;
}

/* ---- batching -------------------------------------------------------------------------- */

static void gl_flush(hw_gl_t* g);

/* A write into VRAM that the pending batch is going to sample is a read-after-write hazard:
   the batch has to go out before host VRAM changes underneath it. This is only the flush
   half; the two halves have to straddle gl_note_sample() on the drawing paths, which is
   why they are separable. See gl_triangle(). */
static void gl_write_flush(hw_gl_t* g, int x, int y, int w, int h) {
    if (g->any_sampled && tiles_intersects(&g->sampled, x, y, w, h))
        gl_flush(g);
}

/* Both halves, for the VRAM transfer commands: there host VRAM either already holds the
   new contents (GP0(A0)) or is written by the software shadow immediately after, and
   nothing in between can sample and re-clear the tile. */
static void gl_write_hazard(hw_gl_t* g, int x, int y, int w, int h) {
    gl_write_flush(g, x, y, w, h);
    gl_mark_dirty(g, x, y, w, h);
}

static int gl_reserve(hw_gl_t* g, int verts) {
    if (g->vert_count + verts > g->vert_cap) {
        int cap = g->vert_cap ? g->vert_cap * 2 : 4096;
        gl_vertex_t* grown;

        while (cap < g->vert_count + verts)
            cap *= 2;

        /* 64k triangles is far past any real PS1 frame; flushing is always correct, so an
           upper bound costs nothing but keeps a runaway display list from eating the heap. */
        if (cap > 196608) {
            gl_flush(g);
            cap = g->vert_cap ? g->vert_cap : 4096;

            if (g->vert_count + verts > cap)
                return 0;

            return 1;
        }

        grown = (gl_vertex_t*)realloc(g->verts, (size_t)cap * sizeof(gl_vertex_t));

        if (!grown)
            return 0;

        g->verts = grown;
        g->vert_cap = cap;
    }

    return 1;
}

static gl_range_t* gl_range_for(hw_gl_t* g, int blend, int stp_pass) {
    const psx_gpu_t* gpu = g->gpu;
    int sx = (int)gpu->draw_x1;
    int sy = (int)gpu->draw_y1;
    int sw = (int)gpu->draw_x2 - sx + 1;
    int sh = (int)gpu->draw_y2 - sy + 1;
    gl_range_t* r;

    if (g->range_count) {
        r = &g->ranges[g->range_count - 1];

        if ((r->blend == blend) && (r->stp_pass == stp_pass) &&
            (r->sx == sx) && (r->sy == sy) && (r->sw == sw) && (r->sh == sh) &&
            (r->first + r->count == g->vert_count)) {
            return r;
        }
    }

    if (g->range_count + 1 > g->range_cap) {
        int cap = g->range_cap ? g->range_cap * 2 : 256;
        gl_range_t* grown = (gl_range_t*)realloc(g->ranges, (size_t)cap * sizeof(gl_range_t));

        if (!grown)
            return NULL;

        g->ranges = grown;
        g->range_cap = cap;
    }

    r = &g->ranges[g->range_count++];
    r->first = g->vert_count;
    r->count = 0;
    r->blend = (uint8_t)blend;
    r->stp_pass = (uint8_t)stp_pass;
    r->sx = (int16_t)sx;
    r->sy = (int16_t)sy;
    r->sw = (int16_t)sw;
    r->sh = (int16_t)sh;

    return r;
}

static void gl_flush(hw_gl_t* g) {
    int i;

    if (!g->range_count) {
        g->vert_count = 0;
        tiles_clear(&g->sampled);
        g->any_sampled = 0;
        return;
    }

    gl_bind_rt(g);

    g->gl.BindVertexArray(g->vao);
    g->gl.BindBuffer(GL_ARRAY_BUFFER, g->vbo);
    g->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)g->vert_count * sizeof(gl_vertex_t)),
                     g->verts, GL_STREAM_DRAW);

    g->gl.UseProgram(g->prog_draw);
    g->gl.Uniform2f(g->u_draw_rt_size, (GLfloat)g->rt_w, (GLfloat)g->rt_h);
    g->gl.Uniform1f(g->u_draw_scale, (GLfloat)g->scale);
    g->gl.Uniform1i(g->u_draw_vram, 0);
    /* Per flush, not per create: the filter is live-toggleable and the backend is not
       recreated when it changes. One Uniform1i per flush is noise next to the draw calls. */
    g->gl.Uniform1i(g->u_draw_filter, g_opt_texture_filter);
    g->gl.Uniform1i(g->u_draw_tex_trunc, g_opt_tex_trunc);
    g->gl.Uniform1i(g->u_draw_mask_texel, g_opt_mask_texel);
    /* Texture unit 1 is the replacement atlas (psx/texrep.h). Bound unconditionally so the
       sampler is never left pointing at unit 0's usampler2D — a GLES driver may reject two
       samplers of different types on one unit even when the branch that reads the second is
       never taken. Costs one Uniform1i and, when the atlas exists, one BindTexture. */
    g->gl.Uniform1i(g->u_draw_repl, 1);

    if (g->repl_tex) {
        g->gl.ActiveTexture(GL_TEXTURE1);
        g->gl.BindTexture(GL_TEXTURE_2D, g->repl_tex);
    }

    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->vram_tex);
    g->gl.Enable(GL_SCISSOR_TEST);

    /* See gl_bind_rt(): in mask mode the shader blends and the blend unit must stay off. The
       ranges still carry their ADD/SUB split — the shader reproduces the same arithmetic from
       `transp` and `mode`, and the stp_pass split that goes with it is still what decides
       which fragments each range keeps. */
    if (g->mask_mode) {
        g->gl.Disable(GL_BLEND);
    } else {
        g->gl.Enable(GL_BLEND);
        g->gl.BlendFunc(GL_ONE, GL_SRC_ALPHA);
    }

    for (i = 0; i < g->range_count; i++) {
        const gl_range_t* r = &g->ranges[i];

        /* An inverted drawing area (draw_x2 < draw_x1) is legal for the game to set and
           draws nothing. glScissor would reject the negative width with GL_INVALID_VALUE,
           which end_frame() would read as a broken pipeline. */
        if ((r->count <= 0) || (r->sw <= 0) || (r->sh <= 0))
            continue;

        g->gl.Scissor(r->sx * g->scale, r->sy * g->scale,
                      r->sw * g->scale, r->sh * g->scale);

        if (!g->mask_mode)
            g->gl.BlendEquation(r->blend == GL_RANGE_SUB ? GL_FUNC_REVERSE_SUBTRACT
                                                         : GL_FUNC_ADD);

        g->gl.Uniform1i(g->u_draw_stp_pass, r->stp_pass);
        g->gl.DrawArrays(GL_TRIANGLES, r->first, r->count);
        g->stat_draws++;
    }

    g->gl.BlendEquation(GL_FUNC_ADD);
    g->stat_ranges += (uint32_t)g->range_count;

    g->vert_count = 0;
    g->range_count = 0;
    tiles_clear(&g->sampled);
    g->any_sampled = 0;
}

/* ---- texture replacement atlas (psx/texrep.h) ------------------------------------------- */

/* Created on FIRST USE, not in create(): a session with the feature off never pays for it,
   and the size is a plain 2048 square (16 MiB) capped by whatever the driver will give us.
   2048 holds 1024 native 64x64 textures at 1x, or 64 of them at 4x; when it runs out the
   packer resets, which costs draw calls and not correctness. */
static int gl_repl_ready(hw_gl_t* g) {
    GLint max_dim = 0;
    int dim = 2048;

    if (g->repl_dim)
        return 1;

    if (!g->gl.GenTextures || !g->gl.TexSubImage2D)
        return 0;

    g->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_dim);

    if (max_dim > 0 && max_dim < dim)
        dim = (int)max_dim;

    if (dim < 512)
        return 0;

    g->gl.GenTextures(1, &g->repl_tex);

    if (!g->repl_tex)
        return 0;

    g->gl.ActiveTexture(GL_TEXTURE1);
    g->gl.BindTexture(GL_TEXTURE_2D, g->repl_tex);
    /* NEAREST/CLAMP for tidiness only — every read is a texelFetch, which ignores both. */
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dim, dim, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->vram_tex);

    g->repl_dim = dim;
    g->repl_gen = 1;
    psxe_diag_logf("hwgl", "texture replacement atlas %dx%d", dim, dim);

    return 1;
}

/* Shelf packer. One pass, then a flush-and-reset, then one more pass — an image that does not
   fit an EMPTY atlas can never fit, so two attempts is the whole loop. */
static int gl_repl_pack(hw_gl_t* g, psx_texrep_image_t* im) {
    const int w = (int)im->pw;
    const int h = (int)im->ph;
    int attempt;

    if (w > g->repl_dim || h > g->repl_dim) {
        if (!g->repl_full_logged) {
            g->repl_full_logged = 1;
            psxe_diag_logf("hwgl", "replacement %dx%d does not fit a %d atlas; skipped",
                           w, h, g->repl_dim);
        }

        return 0;
    }

    for (attempt = 0; attempt < 2; attempt++) {
        if ((g->repl_x + w) > g->repl_dim) {
            g->repl_x = 0;
            g->repl_y += g->repl_shelf;
            g->repl_shelf = 0;
        }

        if ((g->repl_y + h) <= g->repl_dim) {
            g->gl.ActiveTexture(GL_TEXTURE1);
            g->gl.BindTexture(GL_TEXTURE_2D, g->repl_tex);
            g->gl.TexSubImage2D(GL_TEXTURE_2D, 0, g->repl_x, g->repl_y, w, h,
                                GL_RGBA, GL_UNSIGNED_BYTE, im->rgba);
            g->gl.ActiveTexture(GL_TEXTURE0);
            g->gl.BindTexture(GL_TEXTURE_2D, g->vram_tex);

            im->be_x = (uint16_t)g->repl_x;
            im->be_y = (uint16_t)g->repl_y;
            im->be_resident = 1;
            im->be_epoch = g->repl_gen;

            g->repl_x += w;

            if (h > g->repl_shelf)
                g->repl_shelf = h;

            return 1;
        }

        /* Full. The pending batch is the only thing that still needs what is in the atlas,
           so draw it and start over. Everything stamped with the old generation is thereby
           evicted without touching it. */
        gl_flush(g);
        g->repl_gen++;
        g->repl_x = 0;
        g->repl_y = 0;
        g->repl_shelf = 0;
    }

    return 0;
}

/*
    Resolves gpu->texrep_bind into the packed a_repl attribute for the primitive about to be
    emitted. Called at the top of every draw hook, so `cur_repl` is authoritative and stale
    values from an earlier primitive cannot leak into an untextured one.

    Zero-cost while the feature is off: texrep_bind.img is NULL, and this returns after one
    branch without touching GL.
*/
static void gl_repl_resolve(hw_gl_t* g, psx_gpu_t* gpu) {
    const psx_texrep_image_t* im = gpu->texrep_bind.img;
    psx_texrep_image_t* mut;
    uint32_t epoch;

    g->cur_repl[0] = 0;
    g->cur_repl[1] = 0;
    g->cur_repl[2] = 0;
    g->cur_repl[3] = 0;

    if (!im)
        return;

    if (!gl_repl_ready(g))
        return;

    epoch = psx_texrep_epoch(gpu);

    if (epoch != g->repl_epoch) {
        /* The pack was rebuilt (settings change): every be_* stamp now describes an atlas
           layout that no longer applies. */
        g->repl_epoch = epoch;
        g->repl_gen++;
        g->repl_x = 0;
        g->repl_y = 0;
        g->repl_shelf = 0;
    }

    /* be_x/be_y/be_resident/be_epoch are declared BACKEND-OWNED in psx/texrep.h; the module
       zeroes them at load and never reads them. The bind hands out a const pointer because
       nothing else may write the image, so the cast is the ownership statement. */
    mut = (psx_texrep_image_t*)im;

    if (!(mut->be_resident && mut->be_epoch == g->repl_gen)) {
        if (!gl_repl_pack(g, mut))
            return;
    }

    g->cur_repl[0] = (uint32_t)mut->be_x | ((uint32_t)mut->be_y << 16);
    g->cur_repl[1] = (uint32_t)gpu->texrep_bind.u0 |
                     ((uint32_t)gpu->texrep_bind.v0 << 8) |
                     ((uint32_t)mut->scale << 16);
    g->cur_repl[2] = (uint32_t)(mut->nw - 1u) | ((uint32_t)(mut->nh - 1u) << 16);
    g->cur_repl[3] = 0;
}

/* Region of VRAM a textured primitive reads: the whole texture page plus the CLUT row.
   Deliberately coarse — narrowing it to the primitive's UV box is a later optimisation and
   being too WIDE only costs an unnecessary sync, never correctness. */
static void gl_note_sample(hw_gl_t* g, uint16_t texp, uint16_t clut) {
    int tpx = (texp & 0xf) << 6;
    int tpy = (texp & 0x10) << 4;
    int depth = (texp >> 7) & 3;
    int clutx = (clut & 0x3f) << 4;
    int cluty = (clut >> 6) & 0x1ff;
    int words = (depth == 0) ? 64 : ((depth == 1) ? 128 : 256);

    if (g->dbg_geom && (g->frames == 3500) && (g->geom_left > 0)) {
        g->geom_left--;
        psxe_diag_logf("hwgl",
                       "geom: page=(%d,%d %dx256) depth=%d clut=(%d,%d) draw=(%u,%u)-(%u,%u) "
                       "disp=(%u,%u) cpudirty=%d gpudirty=%d",
                       tpx, tpy, words, depth, clutx, cluty,
                       g->gpu->draw_x1, g->gpu->draw_y1, g->gpu->draw_x2, g->gpu->draw_y2,
                       g->gpu->disp_x, g->gpu->disp_y,
                       tiles_intersects(&g->dirty, tpx, tpy, words, 256),
                       tiles_intersects(&g->gpu_dirty, tpx, tpy, words, 256));
    }

    if (tiles_intersects(&g->dirty, tpx, tpy, words, 256)) {
        gl_flush(g);
        gl_sync_vram(g, tpx, tpy, words, 256);
    }

    /*  row 1, and the ORDER is load-bearing: the host upload above may have rewritten
       whole 16x16 tiles that the rasterizer also drew into, so the render target's copy has
       to land afterwards to win. gl_resolve_gpu_tiles() clears the CPU-dirty bit for every
       tile it serves, which is what stops the next upload from undoing it again. */
    if (g->any_gpu_dirty && tiles_intersects(&g->gpu_dirty, tpx, tpy, words, 256)) {
        gl_flush(g);
        gl_resolve_gpu_tiles(g, tpx, tpy, words, 256);
    }

    if (depth < 2) {
        if (tiles_intersects(&g->dirty, clutx, cluty, 256, 1)) {
            gl_flush(g);
            gl_sync_vram(g, clutx, cluty, 256, 1);
        }

        if (g->any_gpu_dirty && tiles_intersects(&g->gpu_dirty, clutx, cluty, 256, 1)) {
            gl_flush(g);
            gl_resolve_gpu_tiles(g, clutx, cluty, 256, 1);
        }

        tiles_mark(&g->sampled, clutx, cluty, 256, 1);
    }

    tiles_mark(&g->sampled, tpx, tpy, words, 256);
    g->any_sampled = 1;
}

/* ---- primitive emission ---------------------------------------------------------------- */

static void gl_fill_common(gl_vertex_t* v, const float tri[6], const float triw[3],
                           const uint32_t col[3],
                           const uint16_t uv[6], uint16_t texp, uint16_t clut,
                           uint16_t flags, uint16_t mode, const psx_gpu_t* gpu) {
    v->tri0[0] = tri[0]; v->tri0[1] = tri[1];
    v->tri0[2] = tri[2]; v->tri0[3] = tri[3];
    v->tri1[0] = tri[4]; v->tri1[1] = tri[5];
    v->triw[0] = triw[0]; v->triw[1] = triw[1]; v->triw[2] = triw[2];

    v->col0[0] = (uint8_t)(col[0] & 0xff);
    v->col0[1] = (uint8_t)((col[0] >> 8) & 0xff);
    v->col0[2] = (uint8_t)((col[0] >> 16) & 0xff);
    v->col0[3] = 0;
    v->col1[0] = (uint8_t)(col[1] & 0xff);
    v->col1[1] = (uint8_t)((col[1] >> 8) & 0xff);
    v->col1[2] = (uint8_t)((col[1] >> 16) & 0xff);
    v->col1[3] = 0;
    v->col2[0] = (uint8_t)(col[2] & 0xff);
    v->col2[1] = (uint8_t)((col[2] >> 8) & 0xff);
    v->col2[2] = (uint8_t)((col[2] >> 16) & 0xff);
    v->col2[3] = 0;

    v->uv01[0] = uv[0]; v->uv01[1] = uv[1];
    v->uv01[2] = uv[2]; v->uv01[3] = uv[3];
    v->uv2[0]  = uv[4]; v->uv2[1]  = uv[5];
    v->uv2[2]  = texp;  v->uv2[3]  = clut;

    v->misc[0] = flags;
    v->misc[1] = mode;
    v->misc[2] = 0;
    v->misc[3] = 0;

    v->texwin[0] = (uint8_t)gpu->texw_mx;
    v->texwin[1] = (uint8_t)gpu->texw_my;
    v->texwin[2] = (uint8_t)gpu->texw_ox;
    v->texwin[3] = (uint8_t)gpu->texw_oy;
}

/*
    Emits one triangle, split across as many draw ranges as the blend state needs. `tri` is
    already winding-corrected and offset-applied, in the software's (a, b, c) order.

    THE GEOMETRY SUBMITTED IS THE BOUNDING BOX, NOT THE TRIANGLE. This is not an
    optimisation gone wrong; it is the only construction under which 1x can be
    pixel-identical, and submitting the triangle itself is what made it not be.

    The hardware rasterizer decides coverage by testing the FRAGMENT CENTRE (x+0.5, y+0.5)
    against the submitted primitive. gpu_render_triangle tests the INTEGER CORNER (x, y)
    (gpu.c:331-332). Those two tests do not select the same pixels, and the shader cannot
    repair the difference: it can discard a fragment the hardware produced, but it can
    never invent one the hardware declined to produce. Every pixel whose corner is inside
    the triangle while its centre is outside is a pixel the software rasterizer writes and
    the GPU silently leaves alone — which is precisely the `blank` bucket (a pixel nothing
    ever drew) and the `far` bucket (a pixel where an older primitive's colour survives),
    64% and 32% of the 1x mismatches measured in the backend.

    Shifting the triangle by half a pixel would line the two sample grids up, but it makes
    correctness depend on GL's fill rule agreeing with gpu.c's TL() macro at samples that
    land exactly on an edge — and it does not: this vertex shader maps native y=0 to the
    BOTTOM of the framebuffer, so GL's "top" edge is gpu.c's bottom one and the two rules
    are mirrored on every horizontal edge.

    Rasterizing the bounding box removes the hardware's fill rule from the answer entirely.
    The box spans native [xmin, xmax) x [ymin, ymax), so at scale S the fragments generated
    are exactly the render-target pixels the software's (and gpu_hw_rt.c's) loop visits,
    and the shader's own bounding-box test, three edge functions and TL rule decide every
    pixel — which is what the file header already claimed and what the code did not do.

    The cost is fragments discarded outside the triangle, ~2x the triangle's own area. The
     measurements say that is affordable: 1x and 2x both ran at ~153% uncapped with
    emu ~10.8 ms, i.e. quadrupling the fragment count cost nothing measurable, so doubling
    it at 1x costs nothing either. Narrowing this back down later means offsetting the
    three edge lines outward and intersecting them, which is exact for fat triangles and
    numerically nasty for slivers; it is an optimisation, not a fix, and it is not needed
    to ship.
*/
static void gl_emit_tri(hw_gl_t* g, const float tri[6], const float triw[3],
                        const uint32_t col[3],
                        const uint16_t uv[6], uint16_t texp, uint16_t clut,
                        uint16_t flags, uint16_t mode) {
    const psx_gpu_t* gpu = g->gpu;
    float lo_x, lo_y, hi_x, hi_y;
    int passes[2];
    int blends[2];
    int npass = 1;
    int p;

    lo_x = hi_x = tri[0];
    lo_y = hi_y = tri[1];

    for (p = 1; p < 3; p++) {
        if (tri[p * 2 + 0] < lo_x) lo_x = tri[p * 2 + 0];
        if (tri[p * 2 + 0] > hi_x) hi_x = tri[p * 2 + 0];
        if (tri[p * 2 + 1] < lo_y) lo_y = tri[p * 2 + 1];
        if (tri[p * 2 + 1] > hi_y) hi_y = tri[p * 2 + 1];
    }

    /* Half-open in both axes, so a zero-width or zero-height box draws nothing — exactly
       as gpu.c's `for (x = xmin; x < xmax; x++)` does. */
    if ((hi_x <= lo_x) || (hi_y <= lo_y))
        return;

    /* Modes 0, 1 and 3 and every opaque primitive share one blend state, so they never
       break a batch. Only mode 2's reverse-subtract needs its own range, and only a
       TEXTURED mode-2 primitive needs the two-pass split, because there the per-texel STP
       bit means both kinds of fragment live in the same primitive. */
    if ((flags & GLF_TRANSP) && (mode == 2)) {
        if (flags & GLF_TEXTURED) {
            npass = 2;
            passes[0] = 1; blends[0] = GL_RANGE_ADD;
            passes[1] = 2; blends[1] = GL_RANGE_SUB;
        } else {
            passes[0] = 0; blends[0] = GL_RANGE_SUB;
        }
    } else {
        passes[0] = 0; blends[0] = GL_RANGE_ADD;
    }

    for (p = 0; p < npass; p++) {
        gl_range_t* r;
        gl_vertex_t* v;
        static const int kx[6] = {0, 1, 1, 0, 1, 0};
        static const int ky[6] = {0, 0, 1, 0, 1, 1};
        const int nvert = g->dbg_tri_bbox ? 6 : 3;
        int i;

        if (!gl_reserve(g, nvert))
            return;

        r = gl_range_for(g, blends[p], passes[p]);

        if (!r)
            return;

        v = &g->verts[g->vert_count];

        /* The real triangle still travels in tri0/tri1 as flat attributes, so the fragment
           shader is unchanged; only the geometry that generates the fragments differs. */
        for (i = 0; i < nvert; i++) {
            gl_fill_common(&v[i], tri, triw, col, uv, texp, clut, flags, mode, gpu);
            v[i].repl[0] = g->cur_repl[0];
            v[i].repl[1] = g->cur_repl[1];
            v[i].repl[2] = g->cur_repl[2];
            v[i].repl[3] = g->cur_repl[3];

            if (g->dbg_tri_bbox) {
                v[i].pos[0] = kx[i] ? hi_x : lo_x;
                v[i].pos[1] = ky[i] ? hi_y : lo_y;
            } else {
                v[i].pos[0] = tri[i * 2 + 0];
                v[i].pos[1] = tri[i * 2 + 1];
            }
        }

        g->vert_count += nvert;
        r->count += nvert;
    }

    g->stat_prims++;
}

/* Axis-aligned quad in native coordinates, covering [x0, x1) x [y0, y1). Sprites and the
   per-pixel quads a Bresenham line expands into both come through here. */
static void gl_emit_quad(hw_gl_t* g, int x0, int y0, int x1, int y1,
                         uint32_t color, uint16_t u0, uint16_t v0_,
                         uint16_t texp, uint16_t clut, uint16_t flags, uint16_t mode) {
    /* Sprites, fills and line-expansion quads are integer by definition. */
    static const float kNoW[3] = {1.0f, 1.0f, 1.0f};
    float tri[6];
    uint32_t col[3];
    uint16_t uv[6];
    const psx_gpu_t* gpu = g->gpu;
    int passes[2];
    int blends[2];
    int npass = 1;
    int p;

    if ((x1 <= x0) || (y1 <= y0))
        return;

    /* tri0.xy carries the box origin, which the sprite path in the fragment shader uses as
       the UV reference point; tri0.zw / tri1 are unused there. */
    tri[0] = (float)x0; tri[1] = (float)y0;
    tri[2] = (float)x1; tri[3] = (float)y1;
    tri[4] = 0.0f;      tri[5] = 0.0f;

    col[0] = color; col[1] = color; col[2] = color;
    uv[0] = u0; uv[1] = v0_; uv[2] = 0; uv[3] = 0; uv[4] = 0; uv[5] = 0;

    flags |= GLF_SPRITE;

    if ((flags & GLF_TRANSP) && (mode == 2)) {
        if (flags & GLF_TEXTURED) {
            npass = 2;
            passes[0] = 1; blends[0] = GL_RANGE_ADD;
            passes[1] = 2; blends[1] = GL_RANGE_SUB;
        } else {
            passes[0] = 0; blends[0] = GL_RANGE_SUB;
        }
    } else {
        passes[0] = 0; blends[0] = GL_RANGE_ADD;
    }

    for (p = 0; p < npass; p++) {
        gl_range_t* r;
        gl_vertex_t* v;
        static const int kx[6] = {0, 1, 1, 0, 1, 0};
        static const int ky[6] = {0, 0, 1, 0, 1, 1};
        int i;

        if (!gl_reserve(g, 6))
            return;

        r = gl_range_for(g, blends[p], passes[p]);

        if (!r)
            return;

        v = &g->verts[g->vert_count];

        for (i = 0; i < 6; i++) {
            gl_fill_common(&v[i], tri, kNoW, col, uv, texp, clut, flags, mode, gpu);
            v[i].repl[0] = g->cur_repl[0];
            v[i].repl[1] = g->cur_repl[1];
            v[i].repl[2] = g->cur_repl[2];
            v[i].repl[3] = g->cur_repl[3];
            v[i].pos[0] = (float)(kx[i] ? x1 : x0);
            v[i].pos[1] = (float)(ky[i] ? y1 : y0);
        }

        g->vert_count += 6;
        r->count += 6;
    }

    g->stat_prims++;
}

/* ---- drawing hooks ---------------------------------------------------------------------- */

static int gl_min3(int a, int b, int c) { int m = (a <= b) ? a : b; return (m <= c) ? m : c; }
static int gl_max3(int a, int b, int c) { int m = (a > b) ? a : b; return (m > c) ? m : c; }

#define GL_EDGE(ax, ay, bx, by, cx, cy) \
    (((bx) - (ax)) * ((cy) - (ay)) - ((by) - (ay)) * ((cx) - (ax)))

static void gl_triangle(hw_gl_t* g, psx_gpu_t* gpu, const poly_data_t* poly,
                        vertex_t v0, vertex_t v1, vertex_t v2) {
    vertex_t a, b, c;
    float tri[6];
    float triw[3];
    int pgxp;
    uint32_t col[3];
    uint16_t uv[6];
    uint16_t flags = 0;
    uint16_t mode;
    int xmin, ymin, xmax, ymax;

    a = v0;

    /* PGXP gate, decided BEFORE the winding swap so the swap itself can honour it.

       All-or-nothing per triangle: mixing one precise vertex with two integer ones would crack
       the shared edges  sealed. On top of precise_valid (core-validated source word,
       psx/pgxp.c), each precise coordinate must agree with its own integer truncation within
       one pixel — a stale cache attach shows up as a vertex teleporting somewhere plausible,
       and the tolerance is what turns "bizarre geometry" into "falls back to integer".
       pw > 0 guards the shader's 1/w divisions. */
    {
        const float tol = 1.0f;

        pgxp = v0.precise_valid && v1.precise_valid && v2.precise_valid &&
               (v0.pw > 0.0f) && (v1.pw > 0.0f) && (v2.pw > 0.0f) &&
               (fabsf(v0.px - (float)v0.x) <= tol) && (fabsf(v0.py - (float)v0.y) <= tol) &&
               (fabsf(v1.px - (float)v1.x) <= tol) && (fabsf(v1.py - (float)v1.y) <= tol) &&
               (fabsf(v2.px - (float)v2.x) <= tol) && (fabsf(v2.py - (float)v2.y) <= tol);
    }

    /* gpu.c:289-295 — enforce positive area, then apply the drawing offset.

       For a PRECISE triangle the swap must follow the precise edge sign, not the integer one:
       near-degenerate slivers can flip sign between the two, and a triangle whose float area
       is negative fails every top-left test in the shader — it does not distort, it VANISHES
       (missing face polys on the Crash title screen, found the hard way). */
    if (pgxp) {
        const float pe = (v1.px - v0.px) * (v2.py - v0.py) -
                         (v1.py - v0.py) * (v2.px - v0.px);

        if (pe < 0.0f) {
            b = v2;
            c = v1;
        } else {
            b = v1;
            c = v2;
        }
    } else if (GL_EDGE(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y) < 0) {
        b = v2;
        c = v1;
    } else {
        b = v1;
        c = v2;
    }

    a.x += (int16_t)gpu->off_x; b.x += (int16_t)gpu->off_x; c.x += (int16_t)gpu->off_x;
    a.y += (int16_t)gpu->off_y; b.y += (int16_t)gpu->off_y; c.y += (int16_t)gpu->off_y;


    xmin = gl_min3(a.x, b.x, c.x);
    ymin = gl_min3(a.y, b.y, c.y);
    xmax = gl_max3(a.x, b.x, c.x);
    ymax = gl_max3(a.y, b.y, c.y);

    /* Hardware rejects at 1023x511; this core historically rejected at 2048x1024, twice as
       permissive in both axes. psx_gpu_prim_oversize() holds the rule for all three
       rasterizers and picks between the two on PSX_GPU_ACCURACY_PRIM_SIZE. */
    if (psx_gpu_prim_oversize(gpu, xmax - xmin, ymax - ymin))
        return;

    if (pgxp) {
        /* A precise triangle can poke up to a pixel outside its integer bounding box; widen
           what gets flushed and marked so the tile bookkeeping can never miss it. The size
           REJECT above stays on the integer box on purpose — same accept/reject decision as
           the software shadow. */
        xmin -= 1; ymin -= 1; xmax += 1; ymax += 1;
    }

    if (poly->attrib & PA_RAW)      flags |= GLF_RAW;
    if (poly->attrib & PA_TRANSP)   flags |= GLF_TRANSP;
    if (poly->attrib & PA_TEXTURED) flags |= GLF_TEXTURED;
    if (poly->attrib & PA_SHADED)   flags |= GLF_SHADED;
    if (psx_gpu_dither_enabled(gpu)) flags |= GLF_DITHER;
    /* GP0(E6), from the SAME two helpers gpu.c:1151 and gpu_hw_rt.c:139 resolve it with, so
       the three rasterizers cannot disagree about what the game asked for. Per primitive
       because that is what they are; carried per vertex because a uniform would break the
       batch at every GP0(E6) (see GLF_MASK_CHECK). */
    if (psx_gpu_mask_check(gpu))    flags |= GLF_MASK_CHECK;
    if (psx_gpu_mask_set(gpu))      flags |= GLF_MASK_SET;

    mode = (poly->attrib & PA_TEXTURED) ? (uint16_t)((poly->texp >> 5) & 3)
                                        : (uint16_t)((gpu->gpustat >> 5) & 3);

    /* Order matters, in two directions.

       The flush has to come FIRST: it clears the sampled map, so a sample noted before it
       is thrown away and a later write into that texture would not force a resync.

       The dirty MARK has to come LAST, after the emit, and that is why the two halves are
       separated. Marking the destination before gl_note_sample() lets a primitive that
       samples a page overlapping its own destination hit dirty ∩ page, sync that region
       out of host VRAM — which does not yet contain this primitive, the software shadow
       runs after this hook returns (gpu.c:1213-1225) — and CLEAR the tile as a side
       effect. Nothing re-marks it, so every later sample of that page reads stale texels
       until something else happens to write there. */
    gl_write_flush(g, xmin, ymin, xmax - xmin, ymax - ymin);

    if (!g->dbg_defer_dirty)
        gl_mark_drawn(g, xmin, ymin, xmax - xmin, ymax - ymin);

    if (poly->attrib & PA_TEXTURED)
        gl_note_sample(g, poly->texp, poly->clut);

    if (pgxp) {
        /* Precise coords are pre-offset SXY space; apply the drawing offset exactly as the
           integer path did above. */
        const float offx = (float)(int16_t)gpu->off_x;
        const float offy = (float)(int16_t)gpu->off_y;

        tri[0] = a.px + offx; tri[1] = a.py + offy;
        tri[2] = b.px + offx; tri[3] = b.py + offy;
        tri[4] = c.px + offx; tri[5] = c.py + offy;
        /* Texture correction only when the w's are mutually sane. One outlier w smears the
           whole triangle's texture (the perspective divide amplifies it); positions stay
           precise either way, UV just falls back to the exact affine path. 32x within one
           triangle is far beyond anything a legitimate PS1 scene produces. */
        {
            float wmin = a.pw, wmax = a.pw;

            if (b.pw < wmin) wmin = b.pw;
            if (b.pw > wmax) wmax = b.pw;
            if (c.pw < wmin) wmin = c.pw;
            if (c.pw > wmax) wmax = c.pw;

            if (wmax > wmin * 32.0f) {
                triw[0] = 1.0f; triw[1] = 1.0f; triw[2] = 1.0f;
            } else {
                triw[0] = a.pw; triw[1] = b.pw; triw[2] = c.pw;
            }
        }
    } else {
        tri[0] = (float)a.x; tri[1] = (float)a.y;
        tri[2] = (float)b.x; tri[3] = (float)b.y;
        tri[4] = (float)c.x; tri[5] = (float)c.y;
        triw[0] = 1.0f; triw[1] = 1.0f; triw[2] = 1.0f;
    }

    /* Non-shaded primitives modulate with data.v[0].c (gpu.c:379), which is always the
       ORIGINAL v0 — and a == v0 because the winding swap only exchanges v1 and v2. */
    col[0] = a.c; col[1] = b.c; col[2] = c.c;
    uv[0] = a.tx; uv[1] = a.ty;
    uv[2] = b.tx; uv[3] = b.ty;
    uv[4] = c.tx; uv[5] = c.ty;

    gl_emit_tri(g, tri, triw, col, uv, poly->texp, poly->clut, flags, mode);

    if (g->dbg_defer_dirty)
        gl_mark_drawn(g, xmin, ymin, xmax - xmin, ymax - ymin);
}

/*
    [video] line_detect — games draw thin lines as flat polygons, and a polygon with ZERO
    extent in one axis draws nothing at all: gpu.c's `for (y = ymin; y < ymax; y++)` is
    half-open, and gl_emit_tri() bails on `hi_y <= lo_y` to match it. That is the polygon
    that "disappears", and rescuing it is what this setting is for.

    What it does NOT do is change the coverage RULE.  contract is that coverage is
    decided once per native pixel from the triangle's own edges; this only moves the
    triangle's vertices before that, exactly as a game moving them itself would. Coverage
    stays watertight and stays equal to the software rasterizer's for the same geometry —
    the geometry is simply no longer degenerate.

    Which vertices move: the ones already at the far edge of the thin axis. When the extent
    is zero every vertex is at that edge, so instead the SECOND HALF of the strip moves —
    PS1 quads arrive in strip order (v0,v1 one edge, v2,v3 the other), so moving v2/v3 turns
    a zero-height quad into a one-pixel-tall rectangle whose two triangles tile it exactly.
    A triangle moves v1/v2, which recovers a wedge rather than a full line: strictly better
    than vanishing, and honestly less than a real line renderer would do.

      quads  — quads only, and only the vanishing (zero-extent) case. Conservative: it can
               only ever add pixels that a real console showed and this backend did not.
      basic  — triangles too, and also polygons that are already exactly one pixel thin,
               which widens them to two. That is the aggressive setting: on this backend a
               1px poly does NOT vanish when upscaling (coverage is native-granular), so
               `basic` deliberately makes such lines thicker rather than fixing a defect.
*/
static int gl_expand_thin_poly(poly_data_t* p, int nv, int mode) {
    int i, xmin, xmax, ymin, ymax, thin_limit;

    xmin = xmax = p->v[0].x;
    ymin = ymax = p->v[0].y;

    for (i = 1; i < nv; i++) {
        if (p->v[i].x < xmin) xmin = p->v[i].x;
        if (p->v[i].x > xmax) xmax = p->v[i].x;
        if (p->v[i].y < ymin) ymin = p->v[i].y;
        if (p->v[i].y > ymax) ymax = p->v[i].y;
    }

    thin_limit = (mode == 2) ? 1 : 0;

    /* The long axis must actually be long: a 1x1 dot is not a line, and widening it would
       double every point sprite a game draws as a degenerate poly. */
    if (((ymax - ymin) <= thin_limit) && ((xmax - xmin) >= 2)) {
        if (ymax == ymin) {
            for (i = nv / 2; i < nv; i++)
                p->v[i].y = (int16_t)(p->v[i].y + 1);
        } else {
            for (i = 0; i < nv; i++)
                if (p->v[i].y == ymax)
                    p->v[i].y = (int16_t)(p->v[i].y + 1);
        }

        return 1;
    }

    if (((xmax - xmin) <= thin_limit) && ((ymax - ymin) >= 2)) {
        if (xmax == xmin) {
            for (i = nv / 2; i < nv; i++)
                p->v[i].x = (int16_t)(p->v[i].x + 1);
        } else {
            for (i = 0; i < nv; i++)
                if (p->v[i].x == xmax)
                    p->v[i].x = (int16_t)(p->v[i].x + 1);
        }

        return 1;
    }

    return 0;
}

static void gl_draw_poly(psx_gpu_backend_t* be, psx_gpu_t* gpu, const poly_data_t* poly) {
    hw_gl_t* g = gl_self(be);
    poly_data_t adjusted;

    if (g->failed)
        return;

    /* Keep the shader's blend rounding in step with psx_gpu_modulate_channel(). Re-read
       per primitive rather than cached at create(): one predictable store, and it can never
       be stale if the accuracy flags are set after the backend attaches. */
    g_opt_tex_trunc = (psx_gpu_accuracy_flags(gpu) & PSX_GPU_ACCURACY_TEX_MODULATE) ? 1 : 0;
    g_opt_mask_texel = psx_gpu_mask_from_texel(gpu) ? 1 : 0;

    /* Texture replacement, decided by gpu_poly()/gpu_rect() BEFORE this hook ran, so the
       shader and the two CPU rasterizers are answering the same question. Returns after one
       branch when nothing is bound, which is always unless the feature is on. psx/texrep.h. */
    gl_repl_resolve(g, gpu);


    /* Off by default, and when off this is one compare against a global before the original
       code runs untouched — the copy only happens for a polygon that actually qualifies. */
    if (g_opt_line_detect &&
        ((poly->attrib & PA_QUAD) || (g_opt_line_detect == 2))) {
        const int nv = (poly->attrib & PA_QUAD) ? 4 : 3;

        adjusted = *poly;

        if (gl_expand_thin_poly(&adjusted, nv, g_opt_line_detect)) {
            /* Precise (PGXP) coordinates describe the ORIGINAL vertex; keeping them would
               put gl_triangle's 1px attach tolerance in charge of undoing the expansion.
               Dropping them makes the expanded polygon plainly integer, which is what it
               now is. */
            int i;

            for (i = 0; i < 4; i++)
                adjusted.v[i].precise_valid = 0;

            poly = &adjusted;
        }
    }

    if (poly->attrib & PA_QUAD) {
        gl_triangle(g, gpu, poly, poly->v[0], poly->v[1], poly->v[2]);
        gl_triangle(g, gpu, poly, poly->v[1], poly->v[2], poly->v[3]);
    } else {
        gl_triangle(g, gpu, poly, poly->v[0], poly->v[1], poly->v[2]);
    }
}

#define GL_CLAMPI(v, d, u) (((v) <= (d)) ? (d) : (((v) >= (u)) ? (u) : (v)))
#define GL_SE10(v) ((int16_t)((v) << 5) >> 5)

static void gl_draw_rect(psx_gpu_backend_t* be, psx_gpu_t* gpu, const rect_data_t* in) {
    hw_gl_t* g = gl_self(be);
    rect_data_t data = *in;
    uint16_t width = 0, height = 0;
    uint16_t flags = 0;
    uint16_t mode;
    uint16_t texp;
    int x0, y0, x1, y1;

    if (g->failed)
        return;

    /* Keep the shader's blend rounding in step with psx_gpu_modulate_channel(). Re-read
       per primitive rather than cached at create(): one predictable store, and it can never
       be stale if the accuracy flags are set after the backend attaches. */
    g_opt_tex_trunc = (psx_gpu_accuracy_flags(gpu) & PSX_GPU_ACCURACY_TEX_MODULATE) ? 1 : 0;
    g_opt_mask_texel = psx_gpu_mask_from_texel(gpu) ? 1 : 0;

    /* Texture replacement, decided by gpu_poly()/gpu_rect() BEFORE this hook ran, so the
       shader and the two CPU rasterizers are answering the same question. Returns after one
       branch when nothing is bound, which is always unless the feature is on. psx/texrep.h. */
    gl_repl_resolve(g, gpu);


    switch ((data.attrib >> 3) & 3) {
        case RS_VARIABLE: width = data.width; height = data.height; break;
        case RS_1X1:      width = 1;  height = 1;  break;
        case RS_8X8:      width = 8;  height = 8;  break;
        case RS_16X16:    width = 16; height = 16; break;
    }

    /* gpu.c:498-510 — offset, re-sign-extend to 11 bits, then clamp the box to +-1024. The
       double SE10 is a real behavioural detail, not redundancy. */
    data.v0.x += (int16_t)gpu->off_x;
    data.v0.y += (int16_t)gpu->off_y;
    data.v0.x = GL_SE10(data.v0.x);
    data.v0.y = GL_SE10(data.v0.y);

    x1 = data.v0.x + width;
    y1 = data.v0.y + height;
    x1 = GL_CLAMPI(x1, -1024, 1024);
    y1 = GL_CLAMPI(y1, -1024, 1024);
    x0 = GL_CLAMPI((int)data.v0.x, -1024, 1024);
    y0 = GL_CLAMPI((int)data.v0.y, -1024, 1024);

    if (data.attrib & RA_RAW)      flags |= GLF_RAW;
    if (data.attrib & RA_TRANSP)   flags |= GLF_TRANSP;
    if (data.attrib & RA_TEXTURED) flags |= GLF_TEXTURED;
    /* Sprites honour GP0(E6) exactly as polygons do — gpu.c:1387 and gpu_hw_rt.c:384 resolve
       it from the same helpers in gpu_render_rect too. */
    if (psx_gpu_mask_check(gpu))   flags |= GLF_MASK_CHECK;
    if (psx_gpu_mask_set(gpu))     flags |= GLF_MASK_SET;

    /* Sprites read the persistent texpage a previous textured POLYGON latched (gpu.c:536),
       so it is rebuilt here into the same 16-bit word layout the shader decodes. */
    texp = (uint16_t)(((gpu->texp_x >> 6) & 0xf) | ((gpu->texp_y >> 4) & 0x10) |
                      ((gpu->texp_d & 3) << 7));
    mode = (uint16_t)((gpu->gpustat >> 5) & 3);

    /* Split for the same reason as gl_triangle() — the mark has to survive note_sample. */
    gl_write_flush(g, x0, y0, x1 - x0, y1 - y0);

    if (!g->dbg_defer_dirty)
        gl_mark_drawn(g, x0, y0, x1 - x0, y1 - y0);

    if (data.attrib & RA_TEXTURED)
        gl_note_sample(g, texp, data.clut);

    gl_emit_quad(g, x0, y0, x1, y1, data.v0.c, data.v0.tx, data.v0.ty,
                 texp, data.clut, flags, mode);

    if (g->dbg_defer_dirty)
        gl_mark_drawn(g, x0, y0, x1 - x0, y1 - y0);
}

/*
    Lines. gpu_render_flat_line (gpu.c:704) is an integer Bresenham that writes raw BGR555
    with no dithering, no blending and no modulation. Rather than approximate it with a
    widened quad — which the backend admits is "genuinely fiddly" and only
    approximately right — the same Bresenham runs here and emits one S x S block per plotted
    pixel. That is exact by construction, and lines are rare enough (wireframe debug output
    and a few racing HUDs) that the vertex cost does not matter.
*/
static void gl_plot(hw_gl_t* g, int x, int y, uint32_t color24) {
    if ((x < 0) || (y < 0) || (x >= 1024) || (y >= 512))
        return;

    gl_emit_quad(g, x, y, x + 1, y + 1, color24, 0, 0, 0, 0, 0, 0);
}

static void gl_line_low(hw_gl_t* g, int x0, int y0, int x1, int y1, uint32_t color24) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int yi = 1;
    int d, y, x;

    if (dy < 0) { yi = -1; dy = -dy; }

    d = (2 * dy) - dx;
    y = y0;

    for (x = x0; x < x1; x++) {
        gl_plot(g, x, y, color24);

        if (d > 0) {
            y += yi;
            d += 2 * (dy - dx);
        } else {
            d += 2 * dy;
        }
    }
}

static void gl_line_high(hw_gl_t* g, int x0, int y0, int x1, int y1, uint32_t color24) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xi = 1;
    int d, x, y;

    if (dx < 0) { xi = -1; dx = -dx; }

    d = (2 * dx) - dy;
    x = x0;

    for (y = y0; y < y1; y++) {
        gl_plot(g, x, y, color24);

        if (d > 0) {
            x += xi;
            d += 2 * (dx - dy);
        } else {
            d += 2 * dx;
        }
    }
}

static void gl_draw_line(psx_gpu_backend_t* be, psx_gpu_t* gpu,
                         const vertex_t* pv0, const vertex_t* pv1, uint16_t color_bgr555) {
    hw_gl_t* g = gl_self(be);
    int x0 = pv0->x + gpu->off_x;
    int y0 = pv0->y + gpu->off_y;
    int x1 = pv1->x + gpu->off_x;
    int y1 = pv1->y + gpu->off_y;
    /* The shader re-truncates to 5 bits, so the BGR555 the core already packed is expanded
       back to the 0x00BBGGRR the vertex format carries. */
    uint32_t color24 = (uint32_t)((color_bgr555 & 0x1f) << 3) |
                       (uint32_t)((((color_bgr555 >> 5) & 0x1f) << 3) << 8) |
                       (uint32_t)((((color_bgr555 >> 10) & 0x1f) << 3) << 16);
    int lo_x, lo_y, hi_x, hi_y;

    if (g->failed)
        return;

    /* Lines are never textured, so gpu_line() never binds a replacement; this only makes
       sure the previous primitive's a_repl cannot leak into these vertices. After the
       failed check, like the poly and rect hooks. */
    gl_repl_resolve(g, gpu);

    lo_x = x0 < x1 ? x0 : x1;
    hi_x = x0 < x1 ? x1 : x0;
    lo_y = y0 < y1 ? y0 : y1;
    hi_y = y0 < y1 ? y1 : y0;
    /* Not gl_write_hazard(): a LINE is a rasterizer write, so under gpu_own the render
       target owns the result and the mark belongs in gpu_dirty. */
    gl_write_flush(g, lo_x, lo_y, hi_x - lo_x + 1, hi_y - lo_y + 1);
    gl_mark_drawn(g, lo_x, lo_y, hi_x - lo_x + 1, hi_y - lo_y + 1);

    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1) gl_line_low(g, x1, y1, x0, y0, color24);
        else         gl_line_low(g, x0, y0, x1, y1, color24);
    } else {
        if (y0 > y1) gl_line_high(g, x1, y1, x0, y0, color24);
        else         gl_line_high(g, x0, y0, x1, y1, color24);
    }
}

/* ---- VRAM transfers ---------------------------------------------------------------------- */

/* Defined below, next to the scanout resolve they share a shader with. */
static int  gl_readback_rect(hw_gl_t* g, int x, int y, int w, int h,
                             uint16_t* dst, uint32_t dst_stride_px);
static void gl_vram_diff(hw_gl_t* g, const char* when);

static void gl_fill_vram(psx_gpu_backend_t* be, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint16_t color) {
    hw_gl_t* g = gl_self(be);

    if (g->failed || !w || !h)
        return;

    gl_write_hazard(g, (int)x, (int)y, (int)w, (int)h);
    gl_flush(g);
    gl_bind_rt(g);

    /* GP0(02) ignores the drawing area and the mask bit (gpu.c:1930-1935), so this is a
       scissored clear rather than a scissored draw. */
    g->gl.Enable(GL_SCISSOR_TEST);
    g->gl.Scissor((GLint)x * g->scale, (GLint)y * g->scale,
                  (GLsizei)w * g->scale, (GLsizei)h * g->scale);
    g->gl.ClearColor((GLfloat)((color & 0x1f) << 3) / 255.0f,
                     (GLfloat)(((color >> 5) & 0x1f) << 3) / 255.0f,
                     (GLfloat)(((color >> 10) & 0x1f) << 3) / 255.0f,
                     0.0f);
    g->gl.Clear(GL_COLOR_BUFFER_BIT);
}

static int gl_ensure_scratch(hw_gl_t* g, int w, int h) {
    if ((g->scratch_tex != 0) && (g->scratch_w >= w) && (g->scratch_h >= h))
        return 1;

    if (!g->scratch_tex) {
        g->gl.GenTextures(1, &g->scratch_tex);
        g->gl.GenFramebuffers(1, &g->scratch_fbo);
    }

    if (!g->scratch_tex || !g->scratch_fbo)
        return 0;

    g->scratch_w = w > g->scratch_w ? w : g->scratch_w;
    g->scratch_h = h > g->scratch_h ? h : g->scratch_h;

    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->scratch_tex);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g->scratch_w, g->scratch_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->scratch_fbo);
    g->gl.FramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                g->scratch_tex, 0);
    g->rt_bound = 0;

    return g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void gl_copy_vram(psx_gpu_backend_t* be, uint32_t sx, uint32_t sy,
                         uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    hw_gl_t* g = gl_self(be);
    int s = g->scale;

    if (g->failed || !w || !h)
        return;

    if ((sx + w > 1024) || (sy + h > 512) || (dx + w > 1024) || (dy + h > 512)) {
        /* gpu.c:1978-1982 drops out-of-range texels one at a time rather than wrapping.
           Clamping the rectangle is the same result for every in-range texel, which is all
           the software path keeps either. */
        uint32_t maxw = 1024 - (sx > dx ? sx : dx);
        uint32_t maxh = 512 - (sy > dy ? sy : dy);

        if ((sx >= 1024) || (dx >= 1024) || (sy >= 512) || (dy >= 512))
            return;

        if (w > maxw) w = maxw;
        if (h > maxh) h = maxh;

        if (!w || !h)
            return;
    }

    gl_write_hazard(g, (int)dx, (int)dy, (int)w, (int)h);

    /*
         row 4, and the condition on `src` is NOT an optimisation — it is the whole
        correctness of the rule.

        gpu.c copies host VRAM -> host VRAM right after this returns. If the source was
        GPU-only, the host destination is stale and the render target's is right, so the
        destination has to become gpu_dirty. If the source was NOT GPU-only, both copies
        agree and host VRAM is the copy to prefer, because taking it through the render
        target is LOSSY: RGBA8 has no bit 15, and Crash animates 8bpp texture pages with
        GP0(80), where bit 15 is the top bit of a palette index. Marking unconditionally
        cost 0.53 percentage points of 1x parity (0.4708 % -> 0.9986 %, ) for exactly
        that reason.
    */
    if (g->gpu_own && tiles_intersects(&g->gpu_dirty, (int)sx, (int)sy, (int)w, (int)h))
        gl_mark_gpu_dirty(g, (int)dx, (int)dy, (int)w, (int)h);

    gl_flush(g);

    /* GL forbids a blit whose source and destination regions overlap inside one FBO, so the
       copy bounces through a scratch target. Copies are rare (scrolling backgrounds and
       screen effects) and this keeps the UPSCALED content, which reading back from host
       VRAM would not. */
    if (!gl_ensure_scratch(g, (int)w * s, (int)h * s)) {
        gl_status("GP0(80) scratch target unavailable; disabling the GL rasterizer");
        g->failed = 1;
        return;
    }

    g->gl.Disable(GL_SCISSOR_TEST);
    g->gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g->rt_fbo);
    g->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g->scratch_fbo);
    g->gl.BlitFramebuffer((GLint)sx * s, (GLint)sy * s,
                          (GLint)(sx + w) * s, (GLint)(sy + h) * s,
                          0, 0, (GLint)w * s, (GLint)h * s,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g->gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g->scratch_fbo);
    g->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g->rt_fbo);
    g->gl.BlitFramebuffer(0, 0, (GLint)w * s, (GLint)h * s,
                          (GLint)dx * s, (GLint)dy * s,
                          (GLint)(dx + w) * s, (GLint)(dy + h) * s,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    g->rt_bound = 0;
}

/* Replicates a native VRAM rectangle into the upscaled target. Host VRAM already holds the
   data by the time this runs (gpu.c:1382-1386), so the vram texture is refreshed first and
   the render target is then filled from it — one code path for GP0(A0), the initial seed
   and the load-state re-seed. */
static void gl_blit_from_vram(hw_gl_t* g, int x, int y, int w, int h) {
    gl_flush(g);
    gl_bind_rt(g);

    g->gl.Disable(GL_SCISSOR_TEST);
    /* A PLAIN WRITE, stated rather than implied. It used to be one by arithmetic — alpha 0
       under (ONE, SRC_ALPHA) — but the upload now writes bit 15 into alpha, and that value
       must land unmodified rather than become a blend factor. Disabling the unit is the same
       result for RGB in both modes; gl_flush() re-establishes the blend state it wants. */
    g->gl.Disable(GL_BLEND);
    g->gl.UseProgram(g->prog_xfer);
    g->gl.Uniform4f(g->u_xfer_rect, (GLfloat)x, (GLfloat)y, (GLfloat)w, (GLfloat)h);
    g->gl.Uniform2f(g->u_xfer_rt_size, (GLfloat)g->rt_w, (GLfloat)g->rt_h);
    g->gl.Uniform1f(g->u_xfer_scale, (GLfloat)g->scale);
    g->gl.Uniform1i(g->u_xfer_vram, 0);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->vram_tex);
    g->gl.BindVertexArray(g->quad_vao);
    g->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g->stat_draws++;
}

/* One non-wrapping rectangle of host VRAM, into the vram texture and then the render
   target. Split out because GP0(A0)'s wrap case needs it up to four times. */
static void gl_upload_rect(hw_gl_t* g, int x, int y, int w, int h) {
    if ((w <= 0) || (h <= 0))
        return;

    gl_write_hazard(g, x, y, w, h);
    gl_flush(g);
    gl_sync_vram(g, x, y, w, h);
    gl_blit_from_vram(g, x, y, w, h);
}

static void gl_upload_vram(psx_gpu_backend_t* be, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, const uint16_t* src,
                           uint32_t src_stride_px) {
    hw_gl_t* g = gl_self(be);

    (void)src;
    (void)src_stride_px;

    if (g->failed || !w || !h)
        return;

    if ((x + w > 1024) || (y + h > 512)) {
        /*
            GP0(A0) wraps per halfword at 1024/512 (gpu.c:1348-1349), so the destination is
            not one rectangle — it is up to four, one per corner the transfer wraps into.

            The old handling re-seeded the WHOLE surface from host VRAM, which is correct
            only while the software shadow keeps host VRAM authoritative. Without the shadow
            it would upload 1 MB of stale host VRAM over the render target and erase every
            pixel the rasterizer had drawn — a wrapping 32x32 texture upload would blank the
            frame. The wrapped rectangle is enumerated instead, which is correct in both
            worlds and cheaper in each.
        */
        int xs[2], ws[2], ys[2], hs[2];
        int nx = 1, ny = 1, ix, iy;

        if (x >= 1024) x &= 0x3ff;
        if (y >= 512)  y &= 0x1ff;

        if (w > 1024) w = 1024;
        if (h > 512)  h = 512;

        xs[0] = (int)x; ws[0] = (int)w;
        ys[0] = (int)y; hs[0] = (int)h;

        if (x + w > 1024) {
            ws[0] = 1024 - (int)x;
            xs[1] = 0;
            ws[1] = (int)w - ws[0];
            nx = 2;
        }

        if (y + h > 512) {
            hs[0] = 512 - (int)y;
            ys[1] = 0;
            hs[1] = (int)h - hs[0];
            ny = 2;
        }

        for (iy = 0; iy < ny; iy++)
            for (ix = 0; ix < nx; ix++)
                gl_upload_rect(g, xs[ix], ys[iy], ws[ix], hs[iy]);

        return;
    }

    gl_upload_rect(g, (int)x, (int)y, (int)w, (int)h);

    /* A whole-surface upload is the load-state re-seed (gpu.c:2574): host VRAM has just
       been replaced wholesale and the render target now mirrors it exactly, so nothing is
       GPU-only any more. Leaving the map set would only cost redundant resolves, but a
       state load is exactly the moment to have it be empty. */
    if (!x && !y && (w == 1024) && (h == 512)) {
        tiles_clear(&g->gpu_dirty);
        g->any_gpu_dirty = 0;
    }
}

/*
    GP0(C0), the backend — the only trigger that forces a GPU->CPU transfer,
    and therefore the one  ladder is built around.

    While PSX_GPU_BACKEND_SOFTWARE_SHADOW is set there is nothing to fetch: gpu->vram is
    authoritative, the GPUREAD drain already has correct data and there is no stall. The
    traffic is measured either way, because that measurement is what decides whether this game
    can live without the shadow at all.
*/
static void gl_download_vram(psx_gpu_backend_t* be, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h, uint16_t* dst, uint32_t dst_stride_px) {
    hw_gl_t* g = gl_self(be);
    const uint64_t bytes = (uint64_t)w * (uint64_t)h * 2u;

    g->stat_readback_bytes += bytes;
    g->c0_frame_bytes += (uint32_t)((bytes > 0xffffffffu) ? 0xffffffffu : bytes);

    if (g->shadow || g->failed)
        return;

    if (!dst || !dst_stride_px)
        return;

    gl_readback_rect(g, (int)x, (int)y, (int)w, (int)h, dst, dst_stride_px);
}

/* ---- frame / scanout ---------------------------------------------------------------------- */

#define GL_STATS_PERIOD 600

/*  averaging window, in frames. Must match the size of hw_gl_t::c0_window. */
#define kC0Window ((int)(sizeof(((hw_gl_t*)0)->c0_window) / sizeof(uint32_t)))

/*  threshold: 256 KB/frame, "a quarter of VRAM". */
#define kC0LimitDefault ((uint64_t)256 * 1024)

/* `hwgl_vram_diff` period, in frames. 600 is ten seconds at 60 fps, which is short enough
   that a session walks through several distinct scenes and long enough that the 1 MB
   glReadPixels each one costs does not itself change what is being measured. */
#define kVramDiffPeriod 600

static void gl_end_frame(psx_gpu_backend_t* be, psx_gpu_t* gpu) {
    hw_gl_t* g = gl_self(be);
    GLenum err;

    (void)gpu;

    if (g->failed)
        return;

    gl_flush(g);

    /* Drain the queue, not just the head. A single error can come from anywhere in the
       process — including the present path, which shares this context — so one is not
       evidence that THIS backend is broken. A run of them is, and a broken pipeline
       silently renders nothing, which is the one thing this must never present as
       "working". */
    err = GL_NO_ERROR;

    while (g->gl.GetError() != GL_NO_ERROR)
        err = 1;

    if (err != GL_NO_ERROR) {
        g->error_streak++;

        if (g->error_streak >= 30) {
            gl_status("GL errors on %d consecutive frames (last at frame %llu); disabling "
                      "the GL rasterizer",
                      g->error_streak, (unsigned long long)g->frames);
            g->failed = 1;
            gl_release(g);
            return;
        }
    } else {
        g->error_streak = 0;
    }

    g->frames++;

    if ((g->frames % GL_STATS_PERIOD) == 0) {
        psxe_diag_logf("hwgl",
                       "frame=%llu scale=%d draws/f=%.1f ranges/f=%.1f prims/f=%.1f "
                       "vramsync/f=%.2f syncpx/f=%.0f c0bytes=%llu c0avg=%llu "
                       "reads/f=%.2f readpx/f=%.0f gres/f=%.2f grespx/f=%.0f "
                       "zerocopy=%u/%d",
                       (unsigned long long)g->frames, g->scale,
                       (double)g->stat_draws / GL_STATS_PERIOD,
                       (double)g->stat_ranges / GL_STATS_PERIOD,
                       (double)g->stat_prims / GL_STATS_PERIOD,
                       (double)g->stat_syncs / GL_STATS_PERIOD,
                       (double)g->stat_sync_px / GL_STATS_PERIOD,
                       (unsigned long long)g->stat_readback_bytes,
                       (unsigned long long)(g->c0_window_sum / kC0Window),
                       (double)g->stat_reads / GL_STATS_PERIOD,
                       (double)g->stat_read_px / GL_STATS_PERIOD,
                       (double)g->stat_gres / GL_STATS_PERIOD,
                       (double)g->stat_gres_px / GL_STATS_PERIOD,
                       g->stat_adopted, GL_STATS_PERIOD);
        g->stat_draws = 0;
        g->stat_ranges = 0;
        g->stat_prims = 0;
        g->stat_syncs = 0;
        g->stat_sync_px = 0;
        g->stat_adopted = 0;
        g->stat_reads = 0;
        g->stat_read_px = 0;
        g->stat_gres = 0;
        g->stat_gres_px = 0;
    }

    /* Periodic whole-VRAM diff. AFTER gl_flush() above, so the render target holds a
       complete frame, and gated on the shadow because the shadow is the reference. */
    if (g->dbg_vram_diff && g->shadow && ((g->frames % kVramDiffPeriod) == 0))
        gl_vram_diff(g, "periodic");

    /*
         automatic downgrade. The window is a plain ring so the average tracks the last
        60 frames rather than a whole session — a game that reads VRAM hard for two seconds
        during a transition and then stops must NOT strand the session on the CPU, and a
        session-long total cannot tell those two apart.

        Note the ordering: the trigger runs at the END of the frame, after gl_flush(), so the
        render target holds a complete frame when gl_seed_host_vram() reads it.
    */
    {
        const uint32_t frame_bytes = g->c0_frame_bytes;
        uint64_t avg;

        g->c0_frame_bytes = 0;
        g->c0_window_sum -= g->c0_window[g->c0_window_idx];
        g->c0_window[g->c0_window_idx] = frame_bytes;
        g->c0_window_sum += frame_bytes;
        g->c0_window_idx = (g->c0_window_idx + 1) % kC0Window;

        avg = g->c0_window_sum / kC0Window;

        /* `> 0` matters: with hwgl_c0_trip the limit is 0, and without it a session that
           never touches GP0(C0) would satisfy `0 >= 0` on its very first frame. */
        if (!g->downgraded && (g->c0_window_sum > 0) && (avg >= g->c0_limit)) {
            gl_downgrade(g, "GP0(C0) readback traffic over the 60-frame limit");
            return;
        }

        if (!g->downgraded && g->dbg_force_downgrade && (g->frames >= 900)) {
            gl_downgrade(g, "hwgl_force_downgrade marker (deliberate test of the ladder)");
            return;
        }
    }

    gl_release(g);
}

static int gl_display_width(const psx_gpu_t* gpu) {
    static const int kHres[4] = {256, 320, 512, 640};
    int w = (gpu->display_mode & 0x40) ? 368 : kHres[gpu->display_mode & 3];

    return (w == 368) ? 384 : w;
}

static int gl_display_height(const psx_gpu_t* gpu) {
    int disp;

    if (gpu->display_mode & 0x4)
        return 480;

    disp = (int)gpu->disp_y2 - (int)gpu->disp_y1;

    return (disp < (255 - 16)) ? disp : 240;
}

/* The 2-bytes-per-pixel readback is only legal if the implementation offers it; ES 3.0
   guarantees nothing beyond RGBA/UNSIGNED_BYTE. Probing beats assuming. Queried against
   whichever RG8 FBO is bound — both of ours are RG8, so one answer serves both — and cached,
   because it is a pipeline query and both call sites are on the hot path. */
static void gl_probe_read_format(hw_gl_t* g) {
    GLint fmt = 0, type = 0;

    if (g->read_probed)
        return;

    g->gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
    g->gl.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);

    g->read_packed = ((GLenum)fmt == GL_RG) && ((GLenum)type == GL_UNSIGNED_BYTE);
    g->read_format = g->read_packed ? GL_RG : GL_RGBA;
    g->read_type = GL_UNSIGNED_BYTE;
    g->read_probed = 1;
}

static int gl_ensure_resolve(hw_gl_t* g, int w, int h) {
    if ((g->resolve_tex != 0) && (g->resolve_w == w) && (g->resolve_h == h))
        return 1;

    if (!g->resolve_tex) {
        g->gl.GenTextures(1, &g->resolve_tex);
        g->gl.GenFramebuffers(1, &g->resolve_fbo);
    }

    if (!g->resolve_tex || !g->resolve_fbo)
        return 0;

    g->resolve_w = w;
    g->resolve_h = h;

    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->resolve_tex);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->resolve_fbo);
    g->gl.FramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                g->resolve_tex, 0);
    g->rt_bound = 0;

    if (g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 0;

    gl_probe_read_format(g);

    return 1;
}

/* ---- GPU -> host VRAM readback (the backend) ---------------------------- */

/* An RG8 target at NATIVE VRAM size, i.e. one texel per PlayStation halfword. Separate from
   resolve_tex on purpose: that one is sized to the display region every frame and sharing it
   would make a GP0(C0) of a texture page thrash the scanout's allocation. 1 MB at worst. */
static int gl_ensure_xfer(hw_gl_t* g, int w, int h) {
    if ((g->xfer_tex != 0) && (g->xfer_w >= w) && (g->xfer_h >= h))
        return 1;

    if (!g->xfer_tex) {
        g->gl.GenTextures(1, &g->xfer_tex);
        g->gl.GenFramebuffers(1, &g->xfer_fbo);
    }

    if (!g->xfer_tex || !g->xfer_fbo)
        return 0;

    g->xfer_w = w > g->xfer_w ? w : g->xfer_w;
    g->xfer_h = h > g->xfer_h ? h : g->xfer_h;

    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->xfer_tex);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RG8, g->xfer_w, g->xfer_h, 0, GL_RG,
                     GL_UNSIGNED_BYTE, NULL);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->xfer_fbo);
    g->gl.FramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                g->xfer_tex, 0);
    g->rt_bound = 0;

    if (g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 0;

    gl_probe_read_format(g);

    return 1;
}

/*
    Reads a NATIVE-coordinate rectangle of the render target back into host memory as packed
    BGR555, downsampling by S on the GPU. This is  "the stall": it flushes the batch and
    then does a glReadPixels, which on a tiler is a full pipeline sync. Everything above it is
    written so that it happens as rarely as possible.

    dst is native VRAM layout; dst_stride_px is in halfwords. Returns 0 without touching dst
    if anything went wrong, so a caller can leave host VRAM alone rather than corrupt it.
*/
static int gl_readback_rect(hw_gl_t* g, int x, int y, int w, int h,
                            uint16_t* dst, uint32_t dst_stride_px) {
    size_t need;
    int row;

    if (g->failed || !dst || (w <= 0) || (h <= 0))
        return 0;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > 1024) w = 1024 - x;
    if (y + h > 512)  h = 512 - y;

    if ((w <= 0) || (h <= 0))
        return 0;

    gl_flush(g);

    if (!gl_ensure_xfer(g, w, h)) {
        gl_status("VRAM readback target unavailable; disabling the GL rasterizer");
        g->failed = 1;
        gl_release(g);
        return 0;
    }

    need = (size_t)w * (size_t)h * (g->read_packed ? 2u : 4u);

    if (need > g->native_rb_cap) {
        uint8_t* grown = (uint8_t*)realloc(g->native_rb, need);

        if (!grown) {
            gl_status("VRAM readback staging allocation of %zu bytes failed", need);
            g->failed = 1;
            gl_release(g);
            return 0;
        }

        g->native_rb = grown;
        g->native_rb_cap = need;
    }

    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->xfer_fbo);
    g->rt_bound = 0;
    g->gl.Viewport(0, 0, w, h);
    g->gl.Disable(GL_SCISSOR_TEST);
    g->gl.Disable(GL_BLEND);
    g->gl.UseProgram(g->prog_resolve);
    g->gl.Uniform1i(g->u_res_rt, 0);
    /* + S/2 picks the centre-ish sub-texel of each native texel. At S == 1 that is +0, so the
       1x path is exactly the scanout path and the parity gate keeps measuring what it did. */
    g->gl.Uniform2i(g->u_res_origin, (GLint)(x * g->scale + g->scale / 2),
                    (GLint)(y * g->scale + g->scale / 2));
    g->gl.Uniform2i(g->u_res_limit, g->rt_w - 1, g->rt_h - 1);
    g->gl.Uniform1i(g->u_res_step, g->scale);
    g->gl.Uniform1i(g->u_res_box, 1);   /* native readback never downsamples a block */
    /* Host VRAM is 16-bit and bit 15 is part of it: GP0(C0),  downgrade seed and the
       whole-VRAM diff all want the mask bit, not a 15-bit approximation of the pixel. */
    g->gl.Uniform1i(g->u_res_mask, g->mask_mode);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->rt_tex);
    g->gl.BindVertexArray(g->quad_vao);
    g->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    g->gl.PixelStorei(GL_PACK_ALIGNMENT, g->read_packed ? 2 : 4);
    g->gl.ReadPixels(0, 0, w, h, g->read_format, g->read_type, g->native_rb);

    if (!g->read_packed) {
        const uint8_t* src = g->native_rb;
        uint8_t* pack = g->native_rb;
        size_t i, count = (size_t)w * (size_t)h;

        for (i = 0; i < count; i++) {
            pack[0] = src[0];
            pack[1] = src[1];
            pack += 2;
            src += 4;
        }
    }

    for (row = 0; row < h; row++) {
        memcpy(dst + (size_t)(y + row) * dst_stride_px + x,
               g->native_rb + (size_t)row * (size_t)w * 2u,
               (size_t)w * 2u);
    }

    gl_release(g);
    g->stat_reads++;
    g->stat_read_px += (uint32_t)(w * h);

    return 1;
}

/*
     "downloading vram_rt once to seed gpu->vram" — the step that makes the downgrade
    transparent instead of a visible glitch, because whatever rasterizer takes over next reads
    host VRAM and would otherwise inherit whatever was last there.

    With the software shadow still installed gpu->vram is ALREADY authoritative and is the
    more accurate of the two copies (it has not been through the render target's 8-bit blend
    precision, ), so overwriting it would be a downgrade in the literal sense. The read is
    still performed, into a scratch buffer, and the two copies are DIFFED — which turns the
    seed path from untested code into a whole-VRAM parity measurement, strictly stronger than
    gl_parity_check's display window, and exactly the number that says whether dropping the
    shadow is safe. When the shadow goes, the same read lands in gpu->vram instead.
*/

/*
    The measurement half of the seed, split out so it can be taken WITHOUT walking the
    ladder.  quoted one 0/524288 at frame 256, which was real, and a second copy that
    landed on a black CD-load screen with softnonzero=0 and compared zeros to zeros. One
    sample of a number this load-bearing is not enough and a vacuous sample is worse than
    none, so `hwgl_vram_diff` repeats it on a period and every line carries its own frame
    number and its own softnonzero — the sample is self-describing, so a busy frame can be
    told from a black one in the log rather than by assertion.

    Only meaningful while the shadow is on: the shadow IS the reference.
*/
static void gl_vram_diff(hw_gl_t* g, const char* when) {
    uint16_t* scratch;
    uint64_t diff = 0, nonzero = 0, gpunonzero = 0;
    uint64_t blank = 0, extra = 0, near_ = 0, far_ = 0;
    uint16_t fx[4], fy[4], fg[4], fw[4];
    int fn = 0;
    size_t i;

    scratch = (uint16_t*)malloc(1024u * 512u * sizeof(uint16_t));

    if (!scratch) {
        psxe_diag_logf("hwgl", "VRAM diff (%s) skipped: out of memory", when);
        return;
    }

    if (gl_readback_rect(g, 0, 0, 1024, 512, scratch, 1024)) {
        for (i = 0; i < 1024u * 512u; i++) {
            const uint16_t a = (uint16_t)(scratch[i] & 0x7fff);
            const uint16_t b = (uint16_t)(g->gpu->vram[i] & 0x7fff);

            if (b) nonzero++;
            if (a) gpunonzero++;

            if (a == b)
                continue;

            diff++;

            /* The same buckets gl_parity_check() uses, and for the same reason: a raw count
               is not actionable. "all within one 5-bit step" is  documented
               multi-blend precision divergence and is fine; a FAR count is the rasterization
               itself disagreeing and is not. */
            if (!a) {
                blank++;
            } else if (!b) {
                extra++;
            } else {
                const int dr = (int)(a & 31u)         - (int)(b & 31u);
                const int dg = (int)((a >> 5) & 31u)  - (int)((b >> 5) & 31u);
                const int db = (int)((a >> 10) & 31u) - (int)((b >> 10) & 31u);

                if ((dr >= -1) && (dr <= 1) && (dg >= -1) && (dg <= 1) &&
                    (db >= -1) && (db <= 1)) {
                    near_++;
                } else {
                    if (fn < 4) {
                        fx[fn] = (uint16_t)(i & 1023u);
                        fy[fn] = (uint16_t)(i >> 10);
                        fg[fn] = a;
                        fw[fn] = b;
                        fn++;
                    }

                    far_++;
                }
            }
        }

        psxe_diag_logf("hwgl",
                       "whole-VRAM readback vs software shadow (%s, frame %llu): %llu/%u "
                       "texels differ (%.4f%%), softnonzero=%llu gpunonzero=%llu "
                       "blank=%llu extra=%llu near=%llu far=%llu",
                       when, (unsigned long long)g->frames,
                       (unsigned long long)diff, 1024u * 512u,
                       100.0 * (double)diff / (double)(1024u * 512u),
                       (unsigned long long)nonzero, (unsigned long long)gpunonzero,
                       (unsigned long long)blank, (unsigned long long)extra,
                       (unsigned long long)near_, (unsigned long long)far_);

        if (fn) {
            psxe_diag_logf("hwgl",
                           "whole-VRAM far samples [(%u,%u) %04x/%04x (%u,%u) %04x/%04x "
                           "(%u,%u) %04x/%04x (%u,%u) %04x/%04x] n=%d",
                           fx[0], fy[0], fg[0], fw[0],
                           fn > 1 ? fx[1] : 0, fn > 1 ? fy[1] : 0,
                           fn > 1 ? fg[1] : 0, fn > 1 ? fw[1] : 0,
                           fn > 2 ? fx[2] : 0, fn > 2 ? fy[2] : 0,
                           fn > 2 ? fg[2] : 0, fn > 2 ? fw[2] : 0,
                           fn > 3 ? fx[3] : 0, fn > 3 ? fy[3] : 0,
                           fn > 3 ? fg[3] : 0, fn > 3 ? fw[3] : 0,
                           fn);
        }
    } else {
        psxe_diag_logf("hwgl", "whole-VRAM readback FAILED during the %s diff", when);
    }

    free(scratch);
}

static void gl_seed_host_vram(hw_gl_t* g) {
    if (!g->shadow) {
        if (!gl_readback_rect(g, 0, 0, 1024, 512, g->gpu->vram, 1024))
            psxe_diag_logf("hwgl", "VRAM seed FAILED; host VRAM is whatever it was");
        else
            psxe_diag_logf("hwgl", "host VRAM seeded from the render target (1024x512)");

        return;
    }

    gl_vram_diff(g, "downgrade seed");
}

/*
     option 1, the whole ladder. Loud by contract: "explicitly and logged, never
    silently". gl_status() is what frontend/main.cpp's checkRasterizerHealth() prints when it
    notices `failed`, so the reason reaches the user-visible log as well as the diag file.

    Setting `failed` is the handover. checkRasterizerHealth() (main.cpp) then drops the
    adopted scanout texture, detaches the backend, destroys it and installs the CPU
    internal-resolution rasterizer — which is immune to this failure mode by construction,
    since it keeps the software shadow and therefore never reads VRAM back at all — or the
    plain software rasterizer if that also declines. That path already existed for GL errors;
    this is the first thing that makes it fire for a reason other than a broken driver.
*/
static void gl_downgrade(hw_gl_t* g, const char* reason) {
    if (g->downgraded)
        return;

    g->downgraded = 1;

    psxe_diag_logf("hwgl",
                   "AUTOMATIC DOWNGRADE at frame %llu: %s. GP0(C0) total=%llu bytes, "
                   "window=%llu bytes over %d frames (limit %llu bytes/frame). "
                   "the backend.",
                   (unsigned long long)g->frames, reason,
                   (unsigned long long)g->stat_readback_bytes,
                   (unsigned long long)g->c0_window_sum,
                   (int)(sizeof(g->c0_window) / sizeof(g->c0_window[0])),
                   (unsigned long long)g->c0_limit);

    gl_seed_host_vram(g);

    /* gl_status() last, so the string frontend/main.cpp echoes through log_error() is this
       one and not a stale "GL rasterizer up". Reason-led rather than cause-led: the same
       ladder is walked by the deliberate test markers, and a message that asserted "this game
       reads VRAM back too fast" would then be a lie in the log. */
    gl_status("automatic downgrade (%s); handing the session to a CPU rasterizer", reason);

    g->failed = 1;
    gl_release(g);
}

/*
    The 1x parity gate, on device.

    tests/gpu_renderer_parity.c cannot reach this backend: it runs on the host, where there
    is no GL context, so it proves things about the software and CPU rasterizers only. The
    requirement that 1x be pixel-identical therefore has to be MEASURED at runtime or it is
    just an assertion.

    This is nearly free because it reuses the scanout readback that has already happened:
    at scale 1 the packed BGR555 the GPU just produced is the same shape as the region of
    gpu->vram the software shadow wrote, so the comparison is one pass over the visible
    framebuffer. It runs for the first few hundred frames of a 1x session and then stops,
    reporting the mismatch rate and the first disagreement it saw.

    Bit 15 is masked on both sides UNLESS the mask bit is on, in which case it is compared
    like every other bit and disagreements land in their own `mask` bucket. That comparison is
    the standing GL-vs-CPU check for : a GL mask stage that silently disagreed with
    psx/dev/gpu.c — a framebuffer fetch returning stale destination alpha, a flag that never
    reaches the shader, an inverted test — shows up here as a large `mask` count against an
    otherwise clean frame. With the mask bit off the render target has no mask channel at all
    and bit 15 is a known, deliberate difference rather than a parity failure.
*/
static void gl_parity_check(hw_gl_t* g, uint32_t disp_x, uint32_t disp_y, int w, int h) {
    const uint16_t* got = (const uint16_t*)g->readback;
    const uint16_t* want;
    const uint16_t keep = g->mask_mode ? 0xffffu : 0x7fffu;
    uint64_t frame_diff;
    int y, x;

    /* Three windows, not one, and each is reported and then RESET. The first 600 frames of
       any disc are the BIOS and the boot logos — under 10 primitives a frame — so a clean
       number there says almost nothing about a rasterizer. The counters have to survive
       into the frames where prims/f is in the hundreds before "1x is pixel-identical"
       means anything. */
    if (g->parity_done || (w <= 0) || (h <= 0))
        return;

    if ((disp_x + (uint32_t)w > 1024) || (disp_y + (uint32_t)h > 512))
        return;

    want = g->gpu->vram + disp_x + (size_t)disp_y * 1024;

    frame_diff = g->parity_diff;
    g->parity_frames++;

    /* One frame of both surfaces, straight to disk, when the marker file is present. A
       difference map is worth more than any number of buckets once the buckets stop
       narrowing it down: it shows immediately whether the residue is edges, one region, a
       whole primitive class or an offset. */
    if (g->dbg_dump && (g->frames == 3000)) {
        char path[1024];
        FILE* f;

        g->dbg_dump = 0;

        if (gl_debug_path(path, sizeof(path), "parity_gpu.bin")) {
            f = fopen(path, "wb");

            if (f) {
                fwrite(got, 2u, (size_t)w * (size_t)h, f);
                fclose(f);
            }
        }

        if (gl_debug_path(path, sizeof(path), "parity_soft.bin")) {
            f = fopen(path, "wb");

            if (f) {
                for (y = 0; y < h; y++)
                    fwrite(want + (size_t)y * 1024, 2u, (size_t)w, f);

                fclose(f);
            }
        }

        psxe_diag_logf("hwgl", "1x parity dump frame=%llu %dx%d disp=(%u,%u)",
                       (unsigned long long)g->frames, w, h, disp_x, disp_y);
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint16_t a = (uint16_t)(got[(size_t)y * w + x] & keep);
            uint16_t b = (uint16_t)(want[(size_t)y * 1024 + x] & keep);

            g->parity_pixels++;

            if (a == b)
                continue;

            g->parity_diff++;

            /* Bit 15 only, i.e. the two agree about the colour and disagree about the mask.
               Bucketed FIRST and separately: it is a different failure from any of the three
               below, and folding it into `far` (which compares 5-bit channels) would report a
               mask disagreement as a colour disagreement and send the next reader to the
               blend arithmetic. */
            if (((a ^ b) & 0x8000u) != 0u) {
                g->parity_mask++;

                if ((uint16_t)(a & 0x7fffu) == (uint16_t)(b & 0x7fffu))
                    continue;
            }

            a = (uint16_t)(a & 0x7fffu);
            b = (uint16_t)(b & 0x7fffu);

            /* Neighbour test, left and up only — those two are already known, and a cluster
               is symmetric so counting one side of each axis finds it just as reliably. */
            if (x > 0) {
                const uint16_t la = (uint16_t)(got[(size_t)y * w + (x - 1)] & 0x7fff);
                const uint16_t lb = (uint16_t)(want[(size_t)y * 1024 + (x - 1)] & 0x7fff);

                if (la != lb)
                    g->parity_clustered++;
            } else if (y > 0) {
                const uint16_t ua = (uint16_t)(got[(size_t)(y - 1) * w + x] & 0x7fff);
                const uint16_t ub = (uint16_t)(want[(size_t)(y - 1) * 1024 + x] & 0x7fff);

                if (ua != ub)
                    g->parity_clustered++;
            }

            /* Bucket it. The channel deltas are computed in 5-bit space because that is the
               space the software rasterizer works in, so "within one step" here means the
               two answers straddle a single integer rounding boundary. */
            if (!a) {
                if (g->parity_blank_n < 4) {
                    const int n = g->parity_blank_n++;

                    g->parity_blank_x[n] = (uint16_t)(disp_x + x);
                    g->parity_blank_y[n] = (uint16_t)(disp_y + y);
                    g->parity_blank_want[n] = b;
                }

                g->parity_blank++;
            } else if (!b) {
                g->parity_extra++;
            } else {
                const int dr = (int)(a & 31u)         - (int)(b & 31u);
                const int dg = (int)((a >> 5) & 31u)  - (int)((b >> 5) & 31u);
                const int db = (int)((a >> 10) & 31u) - (int)((b >> 10) & 31u);

                if ((dr >= -1) && (dr <= 1) && (dg >= -1) && (dg <= 1) &&
                    (db >= -1) && (db <= 1)) {
                    g->parity_near++;
                } else {
                    if (g->parity_far_n < 4) {
                        const int n = g->parity_far_n++;

                        g->parity_far_x[n] = (uint16_t)(disp_x + x);
                        g->parity_far_y[n] = (uint16_t)(disp_y + y);
                        g->parity_far_got[n] = a;
                        g->parity_far_want[n] = b;
                    }

                    g->parity_far++;
                }
            }
        }
    }

    frame_diff = g->parity_diff - frame_diff;

    if (!frame_diff)
        g->parity_clean_frames++;
    else if (frame_diff > g->parity_worst_frame)
        g->parity_worst_frame = frame_diff;

    if ((g->frames == 600) || (g->frames == 1800) || (g->frames == 3600)) {
        psxe_diag_logf("hwgl",
                       "1x parity scanouts=%llu clean=%llu worst_scanout=%llu bbox=%d "
                       "defer=%d",
                       (unsigned long long)g->parity_frames,
                       (unsigned long long)g->parity_clean_frames,
                       (unsigned long long)g->parity_worst_frame,
                       g->dbg_tri_bbox, g->dbg_defer_dirty);

        psxe_diag_logf("hwgl",
                       "1x parity frame=%llu pixels=%llu differing=%llu (%.4f%%) "
                       "blank=%llu extra=%llu near=%llu far=%llu clustered=%llu "
                       "maskbit=%llu (%s)",
                       (unsigned long long)g->frames,
                       (unsigned long long)g->parity_pixels,
                       (unsigned long long)g->parity_diff,
                       g->parity_pixels ? (100.0 * (double)g->parity_diff /
                                           (double)g->parity_pixels) : 0.0,
                       (unsigned long long)g->parity_blank,
                       (unsigned long long)g->parity_extra,
                       (unsigned long long)g->parity_near,
                       (unsigned long long)g->parity_far,
                       (unsigned long long)g->parity_clustered,
                       (unsigned long long)g->parity_mask,
                       g->mask_mode ? "compared" : "not compared, mask bit off");

        psxe_diag_logf("hwgl",
                       "1x parity samples blank[(%u,%u)=%04x (%u,%u)=%04x (%u,%u)=%04x "
                       "(%u,%u)=%04x] n=%d",
                       g->parity_blank_x[0], g->parity_blank_y[0], g->parity_blank_want[0],
                       g->parity_blank_x[1], g->parity_blank_y[1], g->parity_blank_want[1],
                       g->parity_blank_x[2], g->parity_blank_y[2], g->parity_blank_want[2],
                       g->parity_blank_x[3], g->parity_blank_y[3], g->parity_blank_want[3],
                       g->parity_blank_n);

        psxe_diag_logf("hwgl",
                       "1x parity samples far[(%u,%u) %04x/%04x (%u,%u) %04x/%04x "
                       "(%u,%u) %04x/%04x (%u,%u) %04x/%04x] n=%d",
                       g->parity_far_x[0], g->parity_far_y[0],
                       g->parity_far_got[0], g->parity_far_want[0],
                       g->parity_far_x[1], g->parity_far_y[1],
                       g->parity_far_got[1], g->parity_far_want[1],
                       g->parity_far_x[2], g->parity_far_y[2],
                       g->parity_far_got[2], g->parity_far_want[2],
                       g->parity_far_x[3], g->parity_far_y[3],
                       g->parity_far_got[3], g->parity_far_want[3],
                       g->parity_far_n);

        /* Reset so the next window is attributable on its own rather than dominated by
           whatever the previous one accumulated. */
        g->parity_pixels = 0;
        g->parity_diff = 0;
        g->parity_blank = 0;
        g->parity_extra = 0;
        g->parity_near = 0;
        g->parity_far = 0;
        g->parity_mask = 0;
        g->parity_clustered = 0;
        g->parity_worst_frame = 0;
        g->parity_clean_frames = 0;
        g->parity_frames = 0;
        g->parity_blank_n = 0;
        g->parity_far_n = 0;
        memset(g->parity_blank_x, 0, sizeof(g->parity_blank_x));
        memset(g->parity_blank_y, 0, sizeof(g->parity_blank_y));
        memset(g->parity_blank_want, 0, sizeof(g->parity_blank_want));
        memset(g->parity_far_x, 0, sizeof(g->parity_far_x));
        memset(g->parity_far_y, 0, sizeof(g->parity_far_y));
        memset(g->parity_far_got, 0, sizeof(g->parity_far_got));
        memset(g->parity_far_want, 0, sizeof(g->parity_far_want));

        if (g->frames == 3600)
            g->parity_done = 1;
    }
}

/*
    Scanout, in two halves.

    gl_scanout_resolve() packs the display region back down to BGR555 ON THE GPU, into
    resolve_tex. That texture is byte-for-byte what the CPU upload path hands the present
    layer — frontend/render_gl.cpp's ResolveFormat() maps SDL_PIXELFORMAT_BGR555 onto GL_RG8
    holding packed 555, which is exactly this target's format — so there are two ways out of
    it, and only one of them moves any bytes:

      * armsx_hw_gl_present_texture() hands the TEXTURE to the present layer through
        armsx_renderer_adopt_gl_texture(). The pixels never leave the GPU. This is the
        brokered presentation seam, removing both S^2 scanout
        terms (the readback and the re-upload) plus the per-frame pipeline sync.
      * gl_display_buffer() reads it back at 2 bytes per pixel, for every caller that
        genuinely needs host pixels: a non-GL present backend, the screenshot path, and the
        1x parity gate. On a tiler that readback is a full pipeline sync, which is the whole
        reason the first path exists.

    On success the resolve FBO is LEFT BOUND: a caller that wants the pixels reads them
    immediately, and every caller must gl_release() afterwards or present renders into our
    FBO and the screen goes black (see gl_bind_rt's comment).
*/
/*
    [video] downsample — the effective box factor, which is NOT simply the requested one.

    The factor has to DIVIDE the internal scale or the blocks straddle native pixel edges and
    the result shimmers instead of resolving. So the request is a ceiling: take the largest
    divisor of the scale that is <= it. At 1x nothing divides usefully and the answer is 1
    (off), which is why the setting says "hardware rasterizer, above 1x" in the UI rather than
    silently doing nothing.

    Returns 1 when downsampling is off, and 1 is the value that keeps every uniform below at
    exactly what it was before this feature existed.
*/
static int gl_downsample_factor(const hw_gl_t* g) {
    int want = g_opt_downsample;
    int f;

    if (want < 2)
        return 1;

    if (want > g->scale)
        want = g->scale;

    for (f = want; f >= 2; f--) {
        if ((g->scale % f) == 0)
            return f;
    }

    return 1;
}

/* The multiplier the PRESENTED image carries over native, i.e. the internal scale after
   downsampling. Everything outside this file that asks "how much bigger is the scanout"
   must use this and not g->scale — the render target is still g->scale, the picture is not. */
static int gl_present_scale(const hw_gl_t* g) {
    return g->scale / gl_downsample_factor(g);
}

static int gl_scanout_resolve(hw_gl_t* g, uint32_t disp_x, uint32_t disp_y,
                              int* out_w, int* out_h) {
    int w, h, sw, sh, box;

    if (g->failed)
        return 0;

    w = gl_display_width(g->gpu);
    h = gl_display_height(g->gpu);

    if ((w <= 0) || (h <= 0))
        return 0;

    /* Deliberately NOT clamped against the VRAM edge. frontend/main.cpp sizes its upload
       from psx_get_display_width/height and multiplies by the scale, so a buffer narrower
       than that would be read past the end. The resolve shader clamps its texelFetch
       instead, which edge-replicates rather than reading out of bounds. */
    /* Downsampling shrinks the RESOLVE TARGET, not the render target: the frame is still
       rasterized at g->scale and each output texel box-averages a box x box block of it. */
    box = gl_downsample_factor(g);
    sw = (w * g->scale) / box;
    sh = (h * g->scale) / box;

    gl_flush(g);

    if (!gl_ensure_resolve(g, sw, sh)) {
        gl_status("scanout resolve target unavailable; disabling the GL rasterizer");
        g->failed = 1;
        gl_release(g);
        return 0;
    }

    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->resolve_fbo);
    g->rt_bound = 0;
    g->gl.Viewport(0, 0, sw, sh);
    g->gl.Disable(GL_SCISSOR_TEST);
    g->gl.Disable(GL_BLEND);
    g->gl.UseProgram(g->prog_resolve);
    g->gl.Uniform1i(g->u_res_rt, 0);
    g->gl.Uniform2i(g->u_res_origin, (GLint)disp_x * g->scale, (GLint)disp_y * g->scale);
    g->gl.Uniform2i(g->u_res_limit, g->rt_w - 1, g->rt_h - 1);
    /* box == 1 restores the original pair exactly: step 1 is one output texel per RT texel
       and box 1 takes the single-tap branch in kResolveFS. */
    g->gl.Uniform1i(g->u_res_step, box);
    g->gl.Uniform1i(g->u_res_box, box);
    /* The scanout carries bit 15 too, which costs nothing (both present paths decode BGR555
       and ignore it — render_gl.cpp's armsx_decode masks it off, render_vk.cpp's
       Convert555Rows never reads it) and is what lets gl_parity_check() compare the mask bit
       against the software shadow instead of masking it off on both sides. */
    g->gl.Uniform1i(g->u_res_mask, g->mask_mode);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->rt_tex);
    g->gl.BindVertexArray(g->quad_vao);
    g->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    return 1;
}

/* Pulls the resolved region into g->readback as packed BGR555. Requires the resolve FBO to
   be bound, i.e. gl_scanout_resolve() must have just returned 1. */
static int gl_scanout_read(hw_gl_t* g, int sw, int sh) {
    size_t need = (size_t)sw * (size_t)sh * (g->read_packed ? 2u : 4u);

    if (need > g->readback_cap) {
        uint8_t* grown = (uint8_t*)realloc(g->readback, need);

        if (!grown) {
            gl_status("scanout staging allocation of %zu bytes failed", need);
            g->failed = 1;
            gl_release(g);
            return 0;
        }

        g->readback = grown;
        g->readback_cap = need;
    }

    g->gl.PixelStorei(GL_PACK_ALIGNMENT, g->read_packed ? 2 : 4);
    g->gl.ReadPixels(0, 0, sw, sh, g->read_format, g->read_type, g->readback);

    if (!g->read_packed) {
        /* Fallback: no 2-byte read format was offered, so gather the two bytes the resolve
           pass already packed into R and G out of the RGBA the implementation insisted on.
           In place, forwards — dst always trails src. */
        const uint8_t* src = g->readback;
        uint8_t* dst = g->readback;
        size_t i;
        size_t count = (size_t)sw * (size_t)sh;

        for (i = 0; i < count; i++) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst += 2;
            src += 4;
        }
    }

    return 1;
}

/*
    UPSCALED parity — the gate the 1x one structurally cannot be.

    gl_parity_check() only ever runs at scale 1, so it reads 13/13 green while the renderer has
    visible seams at 3x. That is not an oversight in it; a 1x check compares one GPU pixel to one
    software pixel and there is simply nowhere for an upscale-only defect to show. Every
    hypothesis about the seams so far has been argued from reading the shader, and four of them
    were wrong, so this measures instead.

    For each NATIVE pixel it looks at the whole scale x scale block the GPU produced and sorts it:

      clean  - every subpixel equals the software shadow. Nothing to explain.
      edge   - subpixels disagree with each other, but each one matches the software value at
               this pixel or at one of its eight neighbours. That is what a higher-resolution
               rasterisation of a polygon boundary is SUPPOSED to look like, and is not a bug.
      seam   - at least one subpixel matches nothing in the 3x3 software neighbourhood. It is
               not a sharper edge and it is not the shadow's colour: it is a pixel the GPU
               invented, which is exactly what the reported seams look like.

    The seam bucket is the whole point. It separates "upscaling reveals the console's own texture
    edges" from "the rasteriser is producing wrong pixels", which is the question four rounds of
    shader-reading could not answer. First few offenders are logged with coordinates and values.

    Armed by the `hwgl_upscale_parity` marker, so it costs nothing in a normal session.
*/
static void gl_parity_check_upscaled(hw_gl_t* g, uint32_t disp_x, uint32_t disp_y, int w, int h) {
    const uint16_t* got = (const uint16_t*)g->readback;
    const uint16_t* want;
    const int S = g->scale;
    const int stride = w * S;
    uint64_t close = 0, off = 0, bad = 0, bad_uniform = 0;
    uint64_t worst_dev = 0;
    int worst_x = -1, worst_y = -1;
    int reported = 0;
    int y, x;

    if (!g->dbg_upscale_parity || (S <= 1) || (w <= 0) || (h <= 0))
        return;

    if ((disp_x + (uint32_t)w > 1024) || (disp_y + (uint32_t)h > 512))
        return;

    /* Boot frames prove nothing: with almost no geometry there are no boundaries to get wrong. */
    if (g->frames < 3000)
        return;

    want = g->gpu->vram + disp_x + (size_t)disp_y * 1024;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint16_t ref = want[x + (size_t)y * 1024] & 0x7fff;
            const int rr = ref & 31, rg = (ref >> 5) & 31, rb = (ref >> 10) & 31;
            int sr = 0, sg = 0, sb = 0;
            int sy, sx;
            int dr, dg, db, dev;
            int block_uniform;

            {
                const uint16_t f0 = got[(size_t)(y * S) * stride + x * S] & 0x7fff;

                block_uniform = 1;

                for (sy = 0; sy < S; sy++) {
                    for (sx = 0; sx < S; sx++) {
                        const uint16_t sub =
                            got[(size_t)(y * S + sy) * stride + (x * S + sx)] & 0x7fff;

                        if (sub != f0)
                            block_uniform = 0;

                        sr += sub & 31;
                        sg += (sub >> 5) & 31;
                        sb += (sub >> 10) & 31;
                    }
                }
            }

            /* Box-average the block back down to one pixel and compare with the shadow.
               A correct upscaled render of a flat area averages to exactly the software value;
               a correct EDGE averages to something between the two sides, so a small deviation
               is expected and fine. Only a large one means the GPU drew something the software
               rasteriser never would — which is the actual definition of the bug being chased.
               (The previous version compared each subpixel against the 3x3 software
               neighbourhood and called anything unmatched a seam. That counted legitimate
               Gouraud interpolation — which by design produces colours absent from the 1x
               image — as failures, and reported 17% on a picture that is mostly correct.) */
            sr /= (S * S); sg /= (S * S); sb /= (S * S);

            dr = sr > rr ? sr - rr : rr - sr;
            dg = sg > rg ? sg - rg : rg - sg;
            db = sb > rb ? sb - rb : rb - sb;
            dev = dr > dg ? dr : dg;
            dev = dev > db ? dev : db;

            if ((uint64_t)dev > worst_dev) {
                worst_dev = (uint64_t)dev;
                worst_x = x;
                worst_y = y;
            }

            if (dev <= 1) {
                close++;
            } else if (dev <= 4) {
                off++;
            } else {
                bad++;

                /* UNIFORM + darker is the decisive split. A flat block that averages darker than
                   software cannot be a coverage or edge problem — every subpixel agreed, so the
                   rasteriser covered the pixel exactly and still produced the wrong colour. That
                   is blending or quantisation. A non-uniform block is an edge, where some
                   deviation is legitimate. */
                if (block_uniform)
                    bad_uniform++;

                if (reported < 8) {
                    psxe_diag_logf("hwgl",
                                   "upscale bad: native=(%d,%d) avg=(%d,%d,%d) want=(%d,%d,%d) "
                                   "dev=%d",
                                   x, y, sr, sg, sb, rr, rg, rb, dev);
                    reported++;
                }
            }
        }
    }

    psxe_diag_logf("hwgl",
                   "upscale parity frame=%llu scale=%d pixels=%d close=%llu off=%llu bad=%llu "
                   "(%.4f%% bad) worst=%llu at (%d,%d)",
                   (unsigned long long)g->frames, S, w * h,
                   (unsigned long long)close, (unsigned long long)off,
                   (unsigned long long)bad,
                   (w * h) ? (100.0 * (double)bad / (double)(w * h)) : 0.0,
                   (unsigned long long)worst_dev, worst_x, worst_y);

    psxe_diag_logf("hwgl",
                   "upscale bad split: uniform=%llu (flat blocks, so blending/quantisation) "
                   "non_uniform=%llu (edges)",
                   (unsigned long long)bad_uniform,
                   (unsigned long long)(bad - bad_uniform));

    g->dbg_upscale_parity = 0;
}

static const void* gl_display_buffer(psx_gpu_backend_t* be, uint32_t disp_x, uint32_t disp_y,
                                     uint32_t* out_stride_bytes) {
    hw_gl_t* g = gl_self(be);
    int w = 0, h = 0;
    const int ps = gl_present_scale(g);

    if (!gl_scanout_resolve(g, disp_x, disp_y, &w, &h))
        return NULL;

    if (!gl_scanout_read(g, w * ps, h * ps))
        return NULL;

    gl_release(g);

    /* Both parity harnesses read g->readback assuming a stride of w * g->scale and compare
       against the software shadow at that granularity. Downsampling makes the buffer
       w * ps wide and deliberately no longer equal to the shadow, so the harnesses are
       skipped rather than fed a buffer they would misread. ps == g->scale when the feature
       is off, which is every shipping configuration and every gate run. */
    if (ps == g->scale) {
        if (g->scale == 1)
            gl_parity_check(g, disp_x, disp_y, w, h);
        else
            gl_parity_check_upscaled(g, disp_x, disp_y, w, h);
    }

    if (out_stride_bytes)
        *out_stride_bytes = (uint32_t)(w * ps * 2);

    return g->readback;
}

/*
    The brokered seam, caller side. the backend specifies the renderer side
    (armsx_renderer_adopt_gl_texture); this is the half that decides when it is legal.

    Returns 1 only when the present layer has adopted the resolved texture, in which case the
    frontend MUST skip armsx_renderer_upload_frame() for this frame. Every 0 is a working
    fallback to display_buffer(), never an error: the present backend is user-selectable at
    runtime and the pixels are always still available through the readback path.

    The refusals, and why each one has to be here rather than in the present layer:

      * owns_context — when the presentation backend is not OpenGL this backend made its own
        EGL context, so resolve_tex is a name in OUR namespace and means nothing (or worse,
        something else) in the presenter's. The present layer cannot detect this; we can.
      * gpustat bit 23 — display disabled. psx_gpu_get_display_surface() hands back native
        VRAM in that case (gpu.c:2351), so adopting would put a stale frame on screen.
      * adopt_disabled — sticky after the first refusal from the present layer, so a Vulkan
        or SDL present path costs exactly one wasted resolve rather than one per frame.
*/
int armsx_hw_gl_present_texture(psx_gpu_backend_t* be, struct armsx_renderer* renderer) {
    hw_gl_t* g;
    int w = 0, h = 0, sw, sh;

    if (!be || !renderer)
        return 0;

    g = gl_self(be);

    if (g->failed || !g->have_context || g->owns_context || g->adopt_disabled)
        return 0;

    if (g->gpu->gpustat & 0x800000)
        return 0;

    if (!gl_scanout_resolve(g, g->gpu->disp_x, g->gpu->disp_y, &w, &h))
        return 0;

    /* The adopted texture is the RESOLVE target, so its size follows the presented scale,
       not the render-target scale — they differ only while downsampling is on. */
    sw = w * gl_present_scale(g);
    sh = h * gl_present_scale(g);

    /*
        The 1x parity gate outlives the readback it used to ride on. 1x being pixel-identical
        is the gate for every scale above it, and a texture that bypasses the readback is
        exactly the kind of change that can break it silently, so while the gate is still
        running (it stops itself at frame 3600, and only ever runs at scale 1) the readback
        is kept — costing precisely what today's build costs — and only then does the seam
        become free. the backend's warning about g->readback.
    */
    if ((g->scale == 1) && !g->parity_done) {
        if (gl_scanout_read(g, sw, sh)) {
            gl_release(g);
            gl_parity_check(g, g->gpu->disp_x, g->gpu->disp_y, w, h);
        } else {
            return 0;   /* gl_scanout_read() already released and failed the backend */
        }
    } else {
        gl_release(g);
    }

    if (!armsx_renderer_adopt_gl_texture(renderer, (unsigned int)g->resolve_tex, sw, sh,
                                         SDL_PIXELFORMAT_BGR555)) {
        g->adopt_disabled = 1;
        gl_status("present layer will not adopt a GL texture; staying on the readback path");
        return 0;
    }

    if (!g->adopt_logged) {
        g->adopt_logged = 1;
        psxe_diag_logf("hwgl", "zero-copy scanout: presenting resolve texture %u (%dx%d) "
                               "directly, no glReadPixels", (unsigned)g->resolve_tex, sw, sh);
    }

    g->stat_adopted++;

    return 1;
}

/* What the core and the frontend mean by "internal-resolution multiplier in use" is the
   multiplier of the image they are handed, which downsampling reduces. main.cpp multiplies
   the native display size by exactly this to size its upload, so returning g->scale here
   with downsampling on would size the texture for pixels that no longer exist. */
static int gl_resolution_scale(psx_gpu_backend_t* be) {
    return gl_present_scale(gl_self(be));
}

int armsx_hw_gl_failed(const psx_gpu_backend_t* backend) {
    return backend ? ((const hw_gl_t*)backend)->failed : 0;
}

/* ---- entry-point loading -------------------------------------------------------------------- */

/*
    Provider selection.

    When the presentation backend IS OpenGL we must bind to the same GLES provider
    frontend/render_gl.cpp bound, or we get system-GLES function pointers operating on an
    ANGLE context. armsx_render_active_name() reports what actually survived that backend's
    fallback ladder — never what was requested — so it is the authoritative source. Its
    documented shapes are "OpenGL ES (ANGLE)" and "OpenGL ES (system)"; matching on the
    prefix rather than a substring keeps "SDL accelerated (opengl)" from looking like a hit.

    When it is NOT OpenGL — Vulkan is the present path confirmed working on the test device
    — there is no context to borrow, and the system EGL is used to make our own. That is
    sound because this backend's output leaves as PIXELS (the scanout resolve reads back to
    the host), not as a shared GPU texture, so it does not need to live in the presenter's
    context at all. It costs one extra context; it buys the GPU rasterizer working under
    every presentation backend instead of exactly one.
*/
static void* gl_open_provider(int* out_is_angle, int* out_present_is_gl) {
    char name[128];
    void* lib = NULL;

    name[0] = '\0';
    armsx_render_active_name(name, (int)sizeof(name));

    *out_present_is_gl = (strncmp(name, "OpenGL ES", 9) == 0);
    *out_is_angle = *out_present_is_gl && (strstr(name, "ANGLE") != NULL);

    if (*out_is_angle) {
        lib = dlopen("libGLESv2_angle.so", RTLD_NOW | RTLD_LOCAL);
    } else {
        lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);

        if (!lib)
            lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    }

    if (!lib) {
        const char* err = dlerror();

        gl_status("dlopen of the %s GLES library failed: %s",
                  *out_is_angle ? "ANGLE" : "system", err ? err : "(no error)");
    }

    return lib;
}

/* Creates a 1x1 pbuffer context so the rasterizer has somewhere to run when the present
   path is not OpenGL. Returns 0 and logs on any failure; the caller then declines. */
static int gl_make_own_context(hw_gl_t* g, int is_angle) {
    EGLDisplay (*GetDisplay)(EGLNativeDisplayType);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint*, EGLint*);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
    EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLint (*GetError)(void);
    void* lib;
    EGLConfig config;
    EGLint count = 0;
    EGLint major = 0, minor = 0;

    static const EGLint kConfig[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    static const EGLint kPbuffer[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    static const EGLint kContext[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };

    lib = dlopen(is_angle ? "libEGL_angle.so" : "libEGL.so", RTLD_NOW | RTLD_LOCAL);

    if (!lib) {
        const char* err = dlerror();

        gl_status("dlopen of libEGL failed: %s", err ? err : "(no error)");
        return 0;
    }

    *(void**)&GetDisplay = dlsym(lib, "eglGetDisplay");
    *(void**)&Initialize = dlsym(lib, "eglInitialize");
    *(void**)&ChooseConfig = dlsym(lib, "eglChooseConfig");
    *(void**)&CreateContext = dlsym(lib, "eglCreateContext");
    *(void**)&CreatePbufferSurface = dlsym(lib, "eglCreatePbufferSurface");
    *(void**)&MakeCurrent = dlsym(lib, "eglMakeCurrent");
    *(void**)&BindAPI = dlsym(lib, "eglBindAPI");
    *(void**)&GetError = dlsym(lib, "eglGetError");

    if (!GetDisplay || !Initialize || !ChooseConfig || !CreateContext ||
        !CreatePbufferSurface || !MakeCurrent || !GetError) {
        gl_status("libEGL is missing an entry point the GL rasterizer needs");
        dlclose(lib);
        return 0;
    }

    g->egl_display = GetDisplay(EGL_DEFAULT_DISPLAY);

    if (g->egl_display == EGL_NO_DISPLAY) {
        gl_status("eglGetDisplay failed (0x%04x)", (unsigned)GetError());
        dlclose(lib);
        return 0;
    }

    /* eglInitialize on an already-initialised display is a no-op that bumps nothing we
       own, which is why this deliberately never calls eglTerminate: the presentation
       backend may be using the same display. */
    if (!Initialize(g->egl_display, &major, &minor)) {
        gl_status("eglInitialize failed (0x%04x)", (unsigned)GetError());
        dlclose(lib);
        return 0;
    }

    if (BindAPI)
        BindAPI(EGL_OPENGL_ES_API);

    if (!ChooseConfig(g->egl_display, kConfig, &config, 1, &count) || (count < 1)) {
        gl_status("no ES3 pbuffer config available (0x%04x)", (unsigned)GetError());
        dlclose(lib);
        return 0;
    }

    g->egl_context = CreateContext(g->egl_display, config, EGL_NO_CONTEXT, kContext);

    if (g->egl_context == EGL_NO_CONTEXT) {
        gl_status("eglCreateContext failed (0x%04x)", (unsigned)GetError());
        dlclose(lib);
        return 0;
    }

    g->egl_surface = CreatePbufferSurface(g->egl_display, config, kPbuffer);

    if (g->egl_surface == EGL_NO_SURFACE) {
        gl_status("eglCreatePbufferSurface failed (0x%04x)", (unsigned)GetError());
        dlclose(lib);
        return 0;
    }

    if (!MakeCurrent(g->egl_display, g->egl_surface, g->egl_surface, g->egl_context)) {
        gl_status("eglMakeCurrent on the rasterizer's own context failed (0x%04x)",
                  (unsigned)GetError());
        dlclose(lib);
        return 0;
    }

    g->egl_library = lib;
    g->owns_context = 1;

    return 1;
}

#define GL_LOAD(field, symbol)                                                    \
    do {                                                                          \
        *(void**)&g->gl.field = dlsym(lib, symbol);                               \
        if (!g->gl.field) {                                                       \
            gl_status("GLES entry point %s is missing; the GL rasterizer cannot " \
                      "run on this driver", symbol);                              \
            return 0;                                                             \
        }                                                                         \
    } while (0)

static int gl_load_api(hw_gl_t* g, void* lib) {
    GL_LOAD(ActiveTexture, "glActiveTexture");
    GL_LOAD(AttachShader, "glAttachShader");
    GL_LOAD(BindAttribLocation, "glBindAttribLocation");
    GL_LOAD(BindBuffer, "glBindBuffer");
    GL_LOAD(BindFramebuffer, "glBindFramebuffer");
    GL_LOAD(BindTexture, "glBindTexture");
    GL_LOAD(BindVertexArray, "glBindVertexArray");
    GL_LOAD(BlendEquation, "glBlendEquation");
    GL_LOAD(BlendFunc, "glBlendFunc");
    GL_LOAD(BlitFramebuffer, "glBlitFramebuffer");
    GL_LOAD(BufferData, "glBufferData");
    GL_LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    GL_LOAD(Clear, "glClear");
    GL_LOAD(ClearColor, "glClearColor");
    GL_LOAD(ColorMask, "glColorMask");
    GL_LOAD(CompileShader, "glCompileShader");
    GL_LOAD(CreateProgram, "glCreateProgram");
    GL_LOAD(CreateShader, "glCreateShader");
    GL_LOAD(DeleteBuffers, "glDeleteBuffers");
    GL_LOAD(DeleteFramebuffers, "glDeleteFramebuffers");
    GL_LOAD(DeleteProgram, "glDeleteProgram");
    GL_LOAD(DeleteShader, "glDeleteShader");
    GL_LOAD(DeleteTextures, "glDeleteTextures");
    GL_LOAD(DeleteVertexArrays, "glDeleteVertexArrays");
    GL_LOAD(Disable, "glDisable");
    GL_LOAD(DrawArrays, "glDrawArrays");
    GL_LOAD(Enable, "glEnable");
    GL_LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    GL_LOAD(Finish, "glFinish");
    GL_LOAD(FramebufferTexture2D_, "glFramebufferTexture2D");
    GL_LOAD(GenBuffers, "glGenBuffers");
    GL_LOAD(GenFramebuffers, "glGenFramebuffers");
    GL_LOAD(GenTextures, "glGenTextures");
    GL_LOAD(GenVertexArrays, "glGenVertexArrays");
    GL_LOAD(GetError, "glGetError");
    GL_LOAD(GetIntegerv, "glGetIntegerv");
    GL_LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    GL_LOAD(GetProgramiv, "glGetProgramiv");
    GL_LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    GL_LOAD(GetShaderiv, "glGetShaderiv");
    GL_LOAD(GetString, "glGetString");
    GL_LOAD(GetUniformLocation, "glGetUniformLocation");
    GL_LOAD(LinkProgram, "glLinkProgram");
    GL_LOAD(PixelStorei, "glPixelStorei");
    GL_LOAD(ReadPixels, "glReadPixels");
    GL_LOAD(Scissor, "glScissor");
    GL_LOAD(ShaderSource, "glShaderSource");
    GL_LOAD(TexImage2D, "glTexImage2D");
    GL_LOAD(TexParameteri, "glTexParameteri");
    GL_LOAD(TexSubImage2D, "glTexSubImage2D");
    GL_LOAD(Uniform1f, "glUniform1f");
    GL_LOAD(Uniform1i, "glUniform1i");
    GL_LOAD(Uniform1iv, "glUniform1iv");
    GL_LOAD(Uniform2f, "glUniform2f");
    GL_LOAD(Uniform2i, "glUniform2i");
    GL_LOAD(Uniform4f, "glUniform4f");
    GL_LOAD(UseProgram, "glUseProgram");
    GL_LOAD(VertexAttribIPointer, "glVertexAttribIPointer");
    GL_LOAD(VertexAttribPointer, "glVertexAttribPointer");
    GL_LOAD(Viewport, "glViewport");

    return 1;
}

#undef GL_LOAD

/* ---- creation ------------------------------------------------------------------------------- */

static void gl_setup_attribs(hw_gl_t* g) {
    const GLsizei stride = (GLsizei)sizeof(gl_vertex_t);

    g->gl.BindVertexArray(g->vao);
    g->gl.BindBuffer(GL_ARRAY_BUFFER, g->vbo);

    g->gl.EnableVertexAttribArray(0);
    g->gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                              (const void*)(size_t)offsetof(gl_vertex_t, pos));
    g->gl.EnableVertexAttribArray(1);
    g->gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                              (const void*)(size_t)offsetof(gl_vertex_t, tri0));
    g->gl.EnableVertexAttribArray(2);
    g->gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (const void*)(size_t)offsetof(gl_vertex_t, tri1));
    g->gl.EnableVertexAttribArray(3);
    g->gl.VertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, col0));
    g->gl.EnableVertexAttribArray(4);
    g->gl.VertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, col1));
    g->gl.EnableVertexAttribArray(5);
    g->gl.VertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, col2));
    g->gl.EnableVertexAttribArray(6);
    g->gl.VertexAttribIPointer(6, 4, GL_UNSIGNED_SHORT, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, uv01));
    g->gl.EnableVertexAttribArray(7);
    g->gl.VertexAttribIPointer(7, 4, GL_UNSIGNED_SHORT, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, uv2));
    g->gl.EnableVertexAttribArray(8);
    g->gl.VertexAttribIPointer(8, 4, GL_UNSIGNED_SHORT, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, misc));
    g->gl.EnableVertexAttribArray(9);
    g->gl.VertexAttribIPointer(9, 4, GL_UNSIGNED_BYTE, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, texwin));
    g->gl.EnableVertexAttribArray(10);
    g->gl.VertexAttribPointer(10, 3, GL_FLOAT, GL_FALSE, stride,
                              (const void*)(size_t)offsetof(gl_vertex_t, triw));
    /* Texture replacement; zero on every vertex unless a pack matched. psx/texrep.h. */
    g->gl.EnableVertexAttribArray(11);
    g->gl.VertexAttribIPointer(11, 4, GL_UNSIGNED_INT, stride,
                               (const void*)(size_t)offsetof(gl_vertex_t, repl));

    g->gl.BindVertexArray(0);
}

/*
    The mask bit, PROVEN on this device before the backend is handed over.

    Comparing rasterizers against each other is blind when both share the same error, and the
    GL path has a second failure mode the
    CPU ones do not: framebuffer fetch is a driver capability that can be advertised and then
    return stale or zero destination colour (see armsx_gpu_profile_t::fbfetch_gl — MediaTek
    Mali and ANGLE are the known cases). The mask CHECK is one `if` on that value, so a driver
    that lies about it silently un-implements the whole feature and puts Silent Hill's box
    back around the player — which nothing else in this file would notice.

    So it is measured, once, at attach, through the real draw path:

      1. a "set mask" sprite is drawn, so the destination carries bit 15 = 1;
      2. a second, differently coloured sprite is drawn over it with "check mask" on. It must
         be REJECTED — the pixel keeps colour 1, with its mask bit intact;
      3. the CONTROL: the same overdraw one pixel to the right with "check mask" off, which
         must land. Without it the test would also pass on a backend that had simply stopped
         drawing anything.

    Read back through the ordinary resolve path, so it also proves that bit 15 survives the
    render target -> host round trip that GP0(C0) and the downgrade seed depend on.

    Runs before the initial whole-surface seed, so the four pixels it dirties are overwritten
    a moment later and never reach a frame. Returns 0 to REFUSE the attach: the software
    rasterizer is right, and a GL path that reintroduces the mask-check bug is not an acceptable
    fallback for it.
*/
static int gl_mask_selftest(hw_gl_t* g) {
    psx_gpu_t* gpu = g->gpu;
    const uint32_t save_x1 = gpu->draw_x1, save_y1 = gpu->draw_y1;
    const uint32_t save_x2 = gpu->draw_x2, save_y2 = gpu->draw_y2;
    /* VRAM (0,0), because gl_readback_rect() writes at dst[y * stride + x] — a native-VRAM
       layout — so the origin is the one place a two-halfword destination buffer is enough. */
    const int px = 0, py = 0;
    const uint32_t kFirst  = 0x0000f8u;   /* red   -> BGR555 0x001f */
    const uint32_t kSecond = 0xf80000u;   /* blue  -> BGR555 0x7c00 */
    uint16_t probe[4];
    int ok;

    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = 1023;
    gpu->draw_y2 = 511;

    /* Step 1. Untextured, opaque, "set mask while drawing". */
    gl_emit_quad(g, px, py, px + 2, py + 1, kFirst, 0, 0, 0, 0, GLF_MASK_SET, 0);
    gl_flush(g);

    /* Steps 2 and 3, in one batch on purpose: the check must see the write from step 1
       through the same framebuffer-fetch read a real frame uses, not through a flush. */
    gl_emit_quad(g, px, py, px + 1, py + 1, kSecond, 0, 0, 0, 0, GLF_MASK_CHECK, 0);
    gl_emit_quad(g, px + 1, py, px + 2, py + 1, kSecond, 0, 0, 0, 0, 0, 0);
    gl_flush(g);

    memset(probe, 0, sizeof(probe));

    if (!gl_readback_rect(g, px, py, 2, 1, probe, 2)) {
        gpu->draw_x1 = save_x1; gpu->draw_y1 = save_y1;
        gpu->draw_x2 = save_x2; gpu->draw_y2 = save_y2;
        gl_status("mask-bit self-test could not read the render target back");
        return 0;
    }

    gpu->draw_x1 = save_x1; gpu->draw_y1 = save_y1;
    gpu->draw_x2 = save_x2; gpu->draw_y2 = save_y2;

    /* Checked pixel: colour 1 survived AND kept its mask bit. Control: colour 2 landed AND
       its own mask bit is CLEAR — which is the other half of  control, since a
       backend that simply forced bit 15 on every write would pass the first half. */
    ok = (probe[0] == (uint16_t)(0x001f | 0x8000)) && (probe[1] == 0x7c00u);

    if (!ok) {
        gl_status("mask-bit self-test failed (checked=%04x want=801f, control=%04x want=7c00); "
                  "using the CPU rasterizer", probe[0], probe[1]);
        return 0;
    }

    psxe_diag_logf("hwgl", "mask-bit self-test passed: checked=%04x control=%04x "
                           "(GP0(E6) set+check honoured, bit 15 survives the resolve)",
                   probe[0], probe[1]);

    return 1;
}

psx_gpu_backend_t* armsx_hw_gl_create(psx_gpu_t* gpu, int scale) {
    hw_gl_t* g;
    void* lib;
    int is_angle = 0;
    int present_is_gl = 0;
    GLint max_texture = 0;
    const unsigned char* version;

    if (!gpu || !gpu->vram) {
        gl_status("no GPU/VRAM to attach to");
        return NULL;
    }

    lib = gl_open_provider(&is_angle, &present_is_gl);

    if (!lib)
        return NULL;

    g = (hw_gl_t*)calloc(1, sizeof(hw_gl_t));

    if (!g) {
        gl_status("out of memory");
        dlclose(lib);
        return NULL;
    }

    g->gpu = gpu;
    g->gles_library = lib;

    /* Default ON = the shipping behaviour; the markers only exist to reproduce the pre-fix
       build as a control on the same binary. the backend. */
    g->dbg_tri_bbox = !gl_debug_marker("hwgl_no_bbox");
    g->dbg_upscale_parity = gl_debug_marker("hwgl_upscale_parity");
    g->dbg_paint = gl_debug_marker("hwgl_paint_reject");
    g->dbg_defer_dirty = !gl_debug_marker("hwgl_no_defer");
    g->dbg_dump = gl_debug_marker("hwgl_dump");

    /* `hwgl_no_adopt` forces the readback scanout path even when the seam is available, so
       the zero-copy present can be A/B'd against its own binary on the same scene — which is
       the only way  numbers are comparable at all ( methodology). */
    g->adopt_disabled = gl_debug_marker("hwgl_no_adopt");

    /*  two test markers. See the hw_gl_t comment: the trigger cannot fire on its own
       while the software shadow makes GP0(C0) free, so it has to be armed deliberately. */
    g->dbg_c0_trip = gl_debug_marker("hwgl_c0_trip");
    g->dbg_force_downgrade = gl_debug_marker("hwgl_force_downgrade");
    g->c0_limit = g->dbg_c0_trip ? 0u : kC0LimitDefault;

    /*  row 1 and its measurement, both off by default while the shadow is on. */
    g->dbg_gpu_resolve = gl_debug_marker("hwgl_gpu_resolve");
    g->dbg_vram_diff = gl_debug_marker("hwgl_vram_diff");
    g->dbg_geom = gl_debug_marker("hwgl_geom");
    g->geom_left = 8;
    g->geom_draw_left = 12;

    if (!gl_load_api(g, lib)) {
        free(g);
        dlclose(lib);
        return NULL;
    }

    /* glGetString only returns NULL when no context is current on this thread, which
       answers the whole availability question in one call. */
    version = g->gl.GetString(GL_VERSION);

    if (!version) {
        if (!gl_make_own_context(g, is_angle)) {
            armsx_hw_gl_destroy(&g->base);
            return NULL;
        }

        version = g->gl.GetString(GL_VERSION);

        if (!version) {
            gl_status("made an EGL context but no GL is current on it; staying off the GL "
                      "rasterizer");
            armsx_hw_gl_destroy(&g->base);
            return NULL;
        }
    }

    g->have_context = 1;

    /* The presentation backend normally records these strings first. SDL acceleration does
       not use render_gl.cpp, however, and the rasterizer may also own an independent pbuffer
       context. Record the context we are actually about to compile against before consulting
       any per-driver gate below. This is particularly important on MediaTek Mali: the host hint
       marks the SoC and these strings supply the Mali identity, together disabling the known-bad
       framebuffer-fetch path instead of leaving an "unknown" profile to opt into it. */
    {
        const unsigned char* vendor = g->gl.GetString(GL_VENDOR);
        const unsigned char* renderer = g->gl.GetString(GL_RENDERER);
        armsx_gpu_profile_note_gl((const char*)vendor, (const char*)renderer,
                                  (const char*)version);
    }

    /*
        The mask bit, and the one capability it needs. the backend.

         planned this as stencil work. It is not: the mask CHECK is a read-modify-write
        against the destination, and the only mechanism in GLES that gives a fragment shader
        the destination is framebuffer fetch. (Stencil can express the check, but its
        reference value is per-DRAW while "set mask" varies per FRAGMENT, and nothing in
        GLES 3.0 can read a stencil buffer back into vram_tex or into GP0(C0) — the resolve
        and readback paths would have no way to carry bit 15. Alpha can carry it everywhere;
        stencil cannot leave the FBO.)

        So: with the extension, implement it properly. Without it, decline — and say exactly
        why, because "the toggle does nothing" is what this cost the user once already. The
        profile veto matters as much as the extension string: MediaTek Mali advertises
        framebuffer fetch and returns zero or stale destination colour, and under ANGLE it has
        been seen to crash the compiler outright (armsx_gpu_profile_t::fbfetch_gl). Either way
        the mask CHECK would silently do nothing.
    */
    if (psx_gpu_accuracy_flags(gpu) & PSX_GPU_ACCURACY_MASK_BIT) {
        const armsx_gpu_profile_t* profile = armsx_gpu_profile_get();
        const int have_ext = gl_has_extension(g, "GL_EXT_shader_framebuffer_fetch");
        const int trusted = profile ? profile->fbfetch_gl : 1;

        /* The framebuffer-fetch mask path is NEW and unverified on device: it landed the same
           evening a regression appeared where the BIOS could not draw its own text, on an
           Adreno 740 that reports have_ext=yes/trusted=yes and therefore took this branch.
           Declining is the behaviour every confirmed fix was validated against (
           mask-from-texel lives in the CPU rasterizer), so the safe default is to keep
           declining until the GL path is proven against a real boot. Set ARMSX_GL_MASK_BIT=1
           to opt in and test it.

           The decision itself lives in armsx_hw_gl_mask_bit_supported() (gpu_hw_gl.h) so a
           host test can pin it without a GL context — see tests/gpu_rasterizer_select.c. */
        const int allow_gl_mask = armsx_hw_gl_mask_bit_opt_in();

        if (armsx_hw_gl_mask_bit_supported(have_ext, trusted, is_angle, allow_gl_mask)) {
            g->mask_mode = 1;
        } else {
            gl_status("accurate mask bit needs GL_EXT_shader_framebuffer_fetch (present=%s, "
                      "driver trusted=%s, angle=%s, opt-in=%s); using the CPU rasterizer, "
                      "which implements it",
                      have_ext ? "yes" : "no", trusted ? "yes" : "no", is_angle ? "yes" : "no",
                      allow_gl_mask ? "yes" : "no");
            armsx_hw_gl_destroy(&g->base);
            return NULL;
        }
    }

    g->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);

    if (max_texture < 1024) {
        gl_status("GL_MAX_TEXTURE_SIZE is %d; VRAM needs 1024", (int)max_texture);
        armsx_hw_gl_destroy(&g->base);
        return NULL;
    }

    if (scale < 1) scale = 1;
    if (scale > ARMSX_HW_GL_MAX_SCALE) scale = ARMSX_HW_GL_MAX_SCALE;

    while ((scale > 1) && ((1024 * scale) > max_texture))
        scale--;

    g->scale = scale;
    g->rt_w = 1024 * scale;
    g->rt_h = 512 * scale;

    g->base.impl = g;
    /* The shadow stays on deliberately — see the header. It is what makes GP0(C0) free and
       turns  two-way coherency into a one-way upload. */
    /*
        `hwgl_no_shadow` is  step 3 — g->shadow = 0 — behind a marker instead of in the
        default, and it exists for ONE reason: the economic case for the whole of  is the
        claim that most of the ~9 ms `emu` phase is the software shadow, and that claim
        cannot be tested without turning the shadow off.

        It is NOT the flip.  row 1 is measurably not correct yet (: arming
        `hwgl_gpu_resolve` moves the 1x parity window from 0.4708 % to 0.9986 %), so a
        session run this way renders a picture that may be wrong and MUST NOT be used to
        save a state — psx_gpu_save_state() serialises gpu->vram, which nothing refreshes
        from the render target. The TIMING is valid regardless: the same primitives are
        still submitted to the same backend, and the only thing that stops happening is the
        software rasterizer.
    */
    g->base.flags = gl_debug_marker("hwgl_no_shadow") ? 0u
                                                      : PSX_GPU_BACKEND_SOFTWARE_SHADOW;
    g->shadow = (g->base.flags & PSX_GPU_BACKEND_SOFTWARE_SHADOW) ? 1 : 0;
    /* Without the shadow the render target is the only copy of anything the rasterizer drew,
       so  row 1 is mandatory. `hwgl_gpu_resolve` turns it on while the shadow is still
       there, which is how it gets an oracle. */
    g->gpu_own = !g->shadow || g->dbg_gpu_resolve;
    g->base.destroy = NULL;   /* owned by armsx_hw_gl_destroy(), not by the core */
    g->base.draw_poly = gl_draw_poly;
    g->base.draw_rect = gl_draw_rect;
    g->base.draw_line = gl_draw_line;
    g->base.fill_vram = gl_fill_vram;
    g->base.copy_vram = gl_copy_vram;
    g->base.upload_vram = gl_upload_vram;
    g->base.download_vram = gl_download_vram;
    g->base.end_frame = gl_end_frame;
    g->base.display_buffer = gl_display_buffer;
    g->base.resolution_scale = gl_resolution_scale;

    /* Programs. The draw one is assembled rather than a literal, because its mask stage is
       compiled in or out and comes from psx/dev/gpu.h — see gl_build_draw_fs(). */
    {
        char* draw_fs = gl_build_draw_fs(g->mask_mode);

        if (!draw_fs) {
            gl_status("out of memory assembling the draw shader");
            armsx_hw_gl_destroy(&g->base);
            return NULL;
        }

        g->prog_draw = gl_link(g, kDrawVS, draw_fs, kDrawAttribs, 10, "draw");
        free(draw_fs);
    }

    g->prog_xfer = gl_link(g, kXferVS, kXferFS, NULL, 0, "xfer");
    g->prog_resolve = gl_link(g, kResolveVS, kResolveFS, NULL, 0, "resolve");
    g->prog_gres = gl_link(g, kResolveVS, kGResFS, NULL, 0, "gres");

    if (!g->prog_draw || !g->prog_xfer || !g->prog_resolve || !g->prog_gres) {
        /* A driver that advertises the extension and then rejects the `inout` output is a
           real failure mode (ANGLE); name it, without rewriting g_status — which already
           carries the compiler's own message and is what the frontend logs. */
        if (!g->prog_draw && g->mask_mode)
            psxe_diag_logf("hwgl", "the draw shader that failed above is the mask-bit variant "
                                   "(GL_EXT_shader_framebuffer_fetch); the driver advertised "
                                   "the extension and then rejected it");

        armsx_hw_gl_destroy(&g->base);
        return NULL;
    }

    g->u_draw_rt_size   = g->gl.GetUniformLocation(g->prog_draw, "u_rt_size");
    g->u_draw_scale     = g->gl.GetUniformLocation(g->prog_draw, "u_scale");
    g->u_draw_vram      = g->gl.GetUniformLocation(g->prog_draw, "u_vram");
    g->u_draw_dither    = g->gl.GetUniformLocation(g->prog_draw, "u_dither");
    g->u_draw_dither_on = g->gl.GetUniformLocation(g->prog_draw, "u_dither_on");
    g->u_draw_stp_pass  = g->gl.GetUniformLocation(g->prog_draw, "u_stp_pass");
    g->u_draw_paint     = g->gl.GetUniformLocation(g->prog_draw, "u_paint");
    g->u_draw_filter    = g->gl.GetUniformLocation(g->prog_draw, "u_filter");
    g->u_draw_repl      = g->gl.GetUniformLocation(g->prog_draw, "u_repl");
    g->u_draw_tex_trunc = g->gl.GetUniformLocation(g->prog_draw, "u_tex_trunc");
    g->u_draw_mask_texel = g->gl.GetUniformLocation(g->prog_draw, "u_mask_texel");
    g->u_xfer_rect      = g->gl.GetUniformLocation(g->prog_xfer, "u_rect");
    g->u_xfer_rt_size   = g->gl.GetUniformLocation(g->prog_xfer, "u_rt_size");
    g->u_xfer_scale     = g->gl.GetUniformLocation(g->prog_xfer, "u_scale");
    g->u_xfer_vram      = g->gl.GetUniformLocation(g->prog_xfer, "u_vram");
    g->u_res_rt         = g->gl.GetUniformLocation(g->prog_resolve, "u_rt");
    g->u_res_origin     = g->gl.GetUniformLocation(g->prog_resolve, "u_origin");
    g->u_res_limit      = g->gl.GetUniformLocation(g->prog_resolve, "u_limit");
    g->u_res_step       = g->gl.GetUniformLocation(g->prog_resolve, "u_step");
    g->u_res_box        = g->gl.GetUniformLocation(g->prog_resolve, "u_box");
    g->u_res_mask       = g->gl.GetUniformLocation(g->prog_resolve, "u_mask");
    g->u_gres_rt        = g->gl.GetUniformLocation(g->prog_gres, "u_rt");
    g->u_gres_limit     = g->gl.GetUniformLocation(g->prog_gres, "u_limit");
    g->u_gres_step      = g->gl.GetUniformLocation(g->prog_gres, "u_step");
    g->u_gres_half      = g->gl.GetUniformLocation(g->prog_gres, "u_half");
    g->u_gres_mask      = g->gl.GetUniformLocation(g->prog_gres, "u_mask");

    /* Render target. */
    g->gl.GenTextures(1, &g->rt_tex);
    g->gl.ActiveTexture(GL_TEXTURE0);
    g->gl.BindTexture(GL_TEXTURE_2D, g->rt_tex);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g->rt_w, g->rt_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g->gl.GenFramebuffers(1, &g->rt_fbo);
    g->gl.BindFramebuffer(GL_FRAMEBUFFER, g->rt_fbo);
    g->gl.FramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                g->rt_tex, 0);

    if (g->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl_status("render target %dx%d is not framebuffer-complete", g->rt_w, g->rt_h);
        g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        armsx_hw_gl_destroy(&g->base);
        return NULL;
    }

    /* Native-resolution VRAM mirror. R16UI so the shader does exact integer bit extraction
       with no float round trip — RGBA5551 sampled as normalized float could not index a
       CLUT exactly (the backend). */
    g->gl.GenTextures(1, &g->vram_tex);
    g->gl.BindTexture(GL_TEXTURE_2D, g->vram_tex);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    g->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 1024, 512, 0, GL_RED_INTEGER,
                     GL_UNSIGNED_SHORT, gpu->vram);

    g->gl.GenBuffers(1, &g->vbo);
    g->gl.GenVertexArrays(1, &g->vao);
    g->gl.GenVertexArrays(1, &g->quad_vao);
    gl_setup_attribs(g);

    /* Dither kernel: a plain uniform int[16] rather than a texture. Indexed by the NATIVE
       coordinate in the shader, so the pattern is scale-invariant. */
    g->gl.UseProgram(g->prog_draw);
    g->gl.Uniform1iv(g->u_draw_dither, 16, g_psx_gpu_dither_kernel);
    g->gl.Uniform1i(g->u_draw_dither_on, 1);
    g->gl.Uniform1i(g->u_draw_stp_pass, 0);
    g->gl.Uniform1i(g->u_draw_paint, g->dbg_paint);
    g->gl.Uniform1i(g->u_draw_mask_texel, g->mask_mode);

    /* Prove the mask bit on THIS driver before anything depends on it — and do it before the
       seed below, which overwrites every pixel the probe touched. Skipped only under
       `hwgl_paint_reject`, which replaces every fragment's colour with a coverage marker and
       would fail the probe by design. */
    g->rt_bound = 0;

    if (g->mask_mode && !g->dbg_paint && !gl_mask_selftest(g)) {
        armsx_hw_gl_destroy(&g->base);
        return NULL;
    }

    /* Seed the render target from whatever VRAM already holds — a backend installed at boot
       sees a cleared surface, one installed after a load-state sees the restored one. */
    g->rt_bound = 0;
    gl_blit_from_vram(g, 0, 0, 1024, 512);
    gl_release(g);

    if (g->gl.GetError() != GL_NO_ERROR) {
        gl_status("GL errors during setup; refusing to install the GL rasterizer");
        armsx_hw_gl_destroy(&g->base);
        return NULL;
    }

    /* Every state this line reports is settings- or marker-driven, and  records that a
       pushed settings.toml does not reliably survive to the core. This line is the proof that
       what is running is what was asked for; nothing downstream should be believed without
       it. */
    gl_status("GL rasterizer up: %s, %s context, scale=%dx, target=%dx%d, mask=%s, "
              "zerocopy=%s, shadow=%s, c0limit=%llu/frame%s",
              is_angle ? "ANGLE" : "system GLES",
              g->owns_context ? "own EGL" : (present_is_gl ? "shared with present"
                                                           : "pre-existing"),
              g->scale, g->rt_w, g->rt_h,
              g->mask_mode ? "on (fbfetch)" : "off",
              g->adopt_disabled ? "off (hwgl_no_adopt)"
                                : (g->owns_context ? "unavailable (own context)" : "armed"),
              g->shadow ? "on" : "off",
              (unsigned long long)g->c0_limit,
              g->dbg_force_downgrade ? " forcedowngrade=frame900"
                                     : (g->dbg_c0_trip ? " (hwgl_c0_trip)" : ""));
    /* Separate line rather than more fields on the one above: gl_status() has a 192-byte
       buffer and silently truncates, which would take the shadow= field with it. */
    psxe_diag_logf("hwgl", "coherency: gpuown=%s%s vramdiff=%s max_texture=%d",
                   g->gpu_own ? "on" : "off",
                   (g->gpu_own && g->shadow) ? " (hwgl_gpu_resolve, shadow still on)" : "",
                   g->dbg_vram_diff ? "every 600 frames" : "off", (int)max_texture);
    psxe_diag_logf("hwgl", "GL_VERSION=%s renderer=%s", (const char*)version,
                   (const char*)(g->gl.GetString(GL_RENDERER)
                                     ? g->gl.GetString(GL_RENDERER)
                                     : (const unsigned char*)"?"));

    return &g->base;
}

void armsx_hw_gl_destroy(psx_gpu_backend_t* backend) {
    hw_gl_t* g = (hw_gl_t*)backend;

    if (!g)
        return;

    if (!g->have_context)
        goto release_libraries;

    g->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    if (g->gl.DeleteTextures) {
        if (g->rt_tex)      g->gl.DeleteTextures(1, &g->rt_tex);
        if (g->vram_tex)    g->gl.DeleteTextures(1, &g->vram_tex);
        if (g->repl_tex)    g->gl.DeleteTextures(1, &g->repl_tex);
        if (g->scratch_tex) g->gl.DeleteTextures(1, &g->scratch_tex);
        if (g->resolve_tex) g->gl.DeleteTextures(1, &g->resolve_tex);
        if (g->xfer_tex)    g->gl.DeleteTextures(1, &g->xfer_tex);
    }

    if (g->gl.DeleteFramebuffers) {
        if (g->rt_fbo)      g->gl.DeleteFramebuffers(1, &g->rt_fbo);
        if (g->vram_fbo)    g->gl.DeleteFramebuffers(1, &g->vram_fbo);
        if (g->scratch_fbo) g->gl.DeleteFramebuffers(1, &g->scratch_fbo);
        if (g->resolve_fbo) g->gl.DeleteFramebuffers(1, &g->resolve_fbo);
        if (g->xfer_fbo)    g->gl.DeleteFramebuffers(1, &g->xfer_fbo);
    }

    if (g->gl.DeleteBuffers && g->vbo)
        g->gl.DeleteBuffers(1, &g->vbo);

    if (g->gl.DeleteVertexArrays) {
        if (g->vao)      g->gl.DeleteVertexArrays(1, &g->vao);
        if (g->quad_vao) g->gl.DeleteVertexArrays(1, &g->quad_vao);
    }

    if (g->gl.DeleteProgram) {
        if (g->prog_draw)    g->gl.DeleteProgram(g->prog_draw);
        if (g->prog_xfer)    g->gl.DeleteProgram(g->prog_xfer);
        if (g->prog_resolve) g->gl.DeleteProgram(g->prog_resolve);
        if (g->prog_gres)    g->gl.DeleteProgram(g->prog_gres);
    }

release_libraries:

    if (g->owns_context && g->egl_library) {
        EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
        EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
        EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);

        *(void**)&MakeCurrent = dlsym(g->egl_library, "eglMakeCurrent");
        *(void**)&DestroyContext = dlsym(g->egl_library, "eglDestroyContext");
        *(void**)&DestroySurface = dlsym(g->egl_library, "eglDestroySurface");

        if (MakeCurrent)
            MakeCurrent(g->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (DestroySurface && (g->egl_surface != EGL_NO_SURFACE))
            DestroySurface(g->egl_display, g->egl_surface);

        if (DestroyContext && (g->egl_context != EGL_NO_CONTEXT))
            DestroyContext(g->egl_display, g->egl_context);

        /* No eglTerminate: the presentation backend may share this display. */
    }

    free(g->verts);
    free(g->ranges);
    free(g->readback);
    free(g->native_rb);

    if (g->egl_library)
        dlclose(g->egl_library);

    if (g->gles_library)
        dlclose(g->gles_library);

    free(g);
}

#else /* !ARMSX_HW_GL_BUILD */

#ifdef USE_HARDWARE

psx_gpu_backend_t* armsx_hw_gl_create(psx_gpu_t* gpu, int scale) {
    (void)gpu;
    (void)scale;
    return NULL;
}

void armsx_hw_gl_destroy(psx_gpu_backend_t* backend) {
    (void)backend;
}

/* Accepted and discarded on platforms without the GLES backend, so the frontend can push
   settings unconditionally instead of guarding every call site. */
void armsx_hw_gl_set_video_options(int texture_filter, int downsample, int line_detect) {
    (void)texture_filter;
    (void)downsample;
    (void)line_detect;
}

int armsx_hw_gl_failed(const psx_gpu_backend_t* backend) {
    (void)backend;
    return 0;
}

const char* armsx_hw_gl_status(void) {
    return "not built on this platform";
}

int armsx_hw_gl_present_texture(psx_gpu_backend_t* backend, struct armsx_renderer* renderer) {
    (void)backend;
    (void)renderer;
    return 0;
}

#endif

#endif
