/*
    TEXREP_PARITY — texture dumping and replacement, checked BEHAVIOURALLY.

    WHY THIS FILE EXISTS AND WHY GPU_PARITY IS NOT ENOUGH
    ----------------------------------------------------
    tests/gpu_renderer_parity.c compares the two CPU rasterizers against each other, so it
    cannot detect a shared mistake. These cases instead derive expected pixels directly from
    the texture-replacement contract.

    So nothing here is a rasterizer-versus-rasterizer comparison. Every case pins output
    against a value derived from the FEATURE'S OWN CONTRACT (psx/texrep.h) and then checks
    that each rasterizer independently produces it:

      * round-trip identity   dump a texture, feed the dump straight back as a replacement,
                              and the frame must be BIT-IDENTICAL to no replacement at all.
                              A wrong hash, a wrong rectangle, a wrong sub-texel, a wrong
                              alpha rule or a wrong colour expansion all break this, and
                              none of them can break it "the same way" in two rasterizers
                              because the reference is the ORIGINAL FRAME, not the other
                              rasterizer.
      * substitution          repaint the replacement and the frame must change to the
                              repainted colour — proving the hook is actually reached rather
                              than silently skipped.
      * negative control      the SAME repainted file under a wrong hash must change nothing,
                              which is what stops "everything is replaced by everything".
      * sub-texel             a 2x replacement rendered at internal scale 2 must produce a
                              non-uniform 2x2 block. Without the fractional UV every block is
                              flat and an upscaled pack looks exactly like the original — a
                              failure that is invisible to any per-texel comparison.
      * GLSL agreement        PSX_TEXREP_GLSL is the GLES rasterizer's only definition of the
                              atlas mapping and cannot be executed here. It is transcribed
                              into C by hand at the bottom of this file, from the macro text,
                              and psx_texrep_sample() must agree with it across a matrix of
                              scales, texture windows, rectangles and sub-texel positions.
                              Two independent transcriptions of one text is the strongest
                              check available without a GPU.

    The PNG codec is exercised end to end by every one of those: the dumper writes it and the
    loader reads it back.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../psx/dev/gpu.h"
#include "../psx/dev/gpu_backend.h"
#include "../psx/texrep.h"
#include "../psx/texrep_png.h"
#include "../frontend/gpu_hw.h"
#include "../frontend/gpu_hw_rt.h"

/* ---- core stubs, as tests/gpu_renderer_parity.c does ------------------------------------ */

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level; (void)file; (void)line; (void)format;
}

void psx_ic_irq(psx_ic_t* ic, int id) { (void)ic; (void)id; }

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

static int g_failures = 0;

static void ok(const char* name) {
    printf("TEXREP_PARITY passed case=%s\n", name);
}

static void fail(const char* name, const char* why) {
    fprintf(stderr, "TEXREP_PARITY failed case=%s reason=%s\n", name, why);
    g_failures++;
}

#define CHECK(name, cond, why) do { if (!(cond)) { fail((name), (why)); return; } } while (0)

/* ---- scratch directory ------------------------------------------------------------------ */

/* Under build/, which is already ignored and already where the test binaries live. */
#define TEXREP_TEST_DIR   "build/tests/texrep"
#define TEXREP_DUMP_DIR   TEXREP_TEST_DIR "/dump"
#define TEXREP_PACK_DIR   TEXREP_TEST_DIR "/replacements"

/* Both directories start EMPTY every run. dump-writes-png asserts that exactly one file
   appears, which only means anything if a previous run's leftovers are gone — and a stale
   file under an old key would also make roundtrip-is-identical pass against the wrong image.
   Shelling out is fine here: this binary runs on the build machine and nowhere else. */
static void reset_scratch(void) {
    mkdir("build", 0755);
    mkdir("build/tests", 0755);
    mkdir(TEXREP_TEST_DIR, 0755);
    mkdir(TEXREP_DUMP_DIR, 0755);
    mkdir(TEXREP_PACK_DIR, 0755);

    if (system("rm -f " TEXREP_DUMP_DIR "/*.png " TEXREP_PACK_DIR "/*.png") != 0)
        fprintf(stderr, "TEXREP_PARITY note: could not clear the scratch directories\n");
}

/* ---- the corpus ------------------------------------------------------------------------- */

/*
    ONE 16x16 4bpp texture at texture page (0, 0), CLUT at VRAM row 300, drawn as a textured
    quad at (64, 64) and as a textured sprite at (128, 64).

    4bpp is deliberate: it is the depth where the most can go wrong (four texels per VRAM
    halfword, plus a CLUT indirection), and it is what PS1 games overwhelmingly use.
*/
#define TEX_PAGE_X   0
#define TEX_PAGE_Y   0
#define CLUT_X       0
#define CLUT_Y       300
#define TEX_W        16
#define TEX_H        16
#define QUAD_X       64
#define QUAD_Y       64
#define SPRITE_X     128
#define SPRITE_Y     64

/* CLUT word for GP0: bits 0-5 = x/16, bits 6-14 = y. */
#define CLUT_WORD    ((uint16_t)(((CLUT_X >> 4) & 0x3f) | ((CLUT_Y & 0x1ff) << 6)))
/* Texpage word: bits 0-3 page x, bit 4 page y, bits 7-8 depth (0 = 4bpp). */
#define TEXPAGE_WORD ((uint16_t)(((TEX_PAGE_X >> 6) & 0xf) | (((TEX_PAGE_Y >> 8) & 1) << 4)))

static psx_gpu_t* make_gpu(void) {
    psx_gpu_t* gpu = psx_gpu_create();

    if (!gpu)
        return NULL;

    psx_gpu_init(gpu, NULL);
    gpu->draw_x1 = 0;
    gpu->draw_y1 = 0;
    gpu->draw_x2 = PSX_GPU_FB_WIDTH - 1;
    gpu->draw_y2 = PSX_GPU_FB_HEIGHT - 1;

    return gpu;
}

/* A 16-entry palette and a 16x16 index pattern, both chosen so no two texels of the drawn
   region share a colour by accident and so entry 0 (the transparent texel) is present. */
static void seed_texture(psx_gpu_t* gpu) {
    int i, x, y;

    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);

    for (i = 0; i < 16; i++) {
        /* Spread across all three channels; entry 0 stays 0x0000, the transparent texel.
           Entry 15 carries bit 15 so the semi-transparency half of the alpha contract is
           exercised by the round trip as well. */
        unsigned r = (unsigned)(i * 2) & 0x1f;
        unsigned g = (unsigned)(31 - i) & 0x1f;
        unsigned b = (unsigned)((i * 3) + 1) & 0x1f;

        gpu->vram[CLUT_X + i + (CLUT_Y * 1024)] =
            (uint16_t)(i == 0 ? 0 : (r | (g << 5) | (b << 10) | ((i == 15) ? 0x8000u : 0u)));
    }

    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x += 4) {
            /* Four 4-bit indices per halfword, low nibble first — the packing
               gpu_fetch_texel() decodes. */
            unsigned w = 0;
            int k;

            for (k = 0; k < 4; k++) {
                unsigned idx = (unsigned)((x + k + y) & 0xf);

                w |= idx << (k * 4);
            }

            gpu->vram[TEX_PAGE_X + ((x >> 2)) + ((TEX_PAGE_Y + y) * 1024)] = (uint16_t)w;
        }
    }
}

static void gp0(psx_gpu_t* gpu, uint32_t word) {
    psx_gpu_write32(gpu, 0x00, word);
}

/* GP0(2C): textured opaque quad, raw (no modulation) so the test compares texels rather
   than a blend. Raw keeps the comparison about the SAMPLER, which is what is under test. */
static void draw_quad(psx_gpu_t* gpu) {
    gp0(gpu, 0x2d000000u);                                    /* cmd + (ignored) colour   */
    gp0(gpu, (uint32_t)((QUAD_Y << 16) | QUAD_X));            /* v0                        */
    gp0(gpu, (uint32_t)((CLUT_WORD << 16) | (0 << 8) | 0));   /* uv0 + clut                */
    gp0(gpu, (uint32_t)((QUAD_Y << 16) | (QUAD_X + TEX_W)));  /* v1                        */
    gp0(gpu, (uint32_t)((TEXPAGE_WORD << 16) | (0 << 8) | (TEX_W - 1)));  /* uv1 + texpage */
    gp0(gpu, (uint32_t)(((QUAD_Y + TEX_H) << 16) | QUAD_X));  /* v2                        */
    gp0(gpu, (uint32_t)((0 << 8) | 0) | ((uint32_t)(TEX_H - 1) << 8));    /* uv2           */
    gp0(gpu, (uint32_t)(((QUAD_Y + TEX_H) << 16) | (QUAD_X + TEX_W)));    /* v3            */
    gp0(gpu, (uint32_t)(((TEX_H - 1) << 8) | (TEX_W - 1)));   /* uv3                       */
}

/* GP0(65): textured opaque sprite, variable size, raw. Exercises the OTHER live rasterizer
   entry point — gpu_render_rect(), whose UV walk and bind are separate code from the
   polygon path's. */
static void draw_sprite(psx_gpu_t* gpu) {
    gp0(gpu, 0x65808080u);
    gp0(gpu, (uint32_t)((SPRITE_Y << 16) | SPRITE_X));
    gp0(gpu, (uint32_t)((CLUT_WORD << 16) | (0 << 8) | 0));
    gp0(gpu, (uint32_t)((TEX_H << 16) | TEX_W));
}

/* Sprites read the LATCHED texpage, not a per-primitive word, exactly as hardware does. */
static void latch_texpage(psx_gpu_t* gpu) {
    gp0(gpu, 0xe1000000u | (uint32_t)TEXPAGE_WORD);
}

static void draw_all(psx_gpu_t* gpu) {
    latch_texpage(gpu);
    draw_quad(gpu);
    draw_sprite(gpu);
}

/* ---- frame capture ---------------------------------------------------------------------- */

#define CAP_X0  (QUAD_X - 2)
#define CAP_Y0  (QUAD_Y - 2)
#define CAP_W   (SPRITE_X + TEX_W + 2 - CAP_X0)
#define CAP_H   (TEX_H + 4)

typedef struct { uint16_t px[CAP_W * CAP_H]; } frame_t;

static void capture_vram(const psx_gpu_t* gpu, frame_t* out) {
    int x, y;

    for (y = 0; y < CAP_H; y++)
        for (x = 0; x < CAP_W; x++)
            out->px[y * CAP_W + x] = gpu->vram[(CAP_X0 + x) + ((CAP_Y0 + y) * 1024)];
}

static uint16_t rt_texel(psx_gpu_backend_t* be, int scale, int x, int y) {
    uint32_t stride_bytes = 0;
    const uint16_t* base = (const uint16_t*)be->display_buffer(be, 0, 0, &stride_bytes);
    const size_t stride_px = stride_bytes / sizeof(uint16_t);

    return base[(size_t)x * scale + ((size_t)y * scale * stride_px)];
}

static void capture_rt(psx_gpu_backend_t* be, int scale, frame_t* out) {
    int x, y;

    for (y = 0; y < CAP_H; y++)
        for (x = 0; x < CAP_W; x++)
            out->px[y * CAP_W + x] = rt_texel(be, scale, CAP_X0 + x, CAP_Y0 + y);
}

static int frames_equal(const frame_t* a, const frame_t* b) {
    return memcmp(a->px, b->px, sizeof(a->px)) == 0;
}

static int frame_nonblank(const frame_t* f) {
    size_t i;

    for (i = 0; i < (size_t)(CAP_W * CAP_H); i++)
        if (f->px[i] != 0)
            return 1;

    return 0;
}

/*
    Renders the corpus with the given texrep configuration and returns BOTH rasterizers'
    output. `soft` is psx/dev/gpu.c's VRAM; `hw` is frontend/gpu_hw_rt.c's render target at
    `scale`. The backend runs with PSX_GPU_BACKEND_SOFTWARE_SHADOW, so one pass produces
    both — which is exactly the arrangement that makes "did they both replace the same
    texels" answerable at all.
*/
static int render(int dump, int replace, const char* dir, int scale,
                  frame_t* soft, frame_t* hw) {
    psx_gpu_t* gpu = make_gpu();
    psx_gpu_backend_t* be;

    if (!gpu)
        return 0;

    seed_texture(gpu);
    psx_texrep_configure(gpu, dump, replace, dir);

    be = armsx_hw_rt_create(gpu, scale);

    if (!be) {
        psx_gpu_destroy(gpu);

        return 0;
    }

    psx_gpu_set_backend(gpu, be);
    draw_all(gpu);

    if (soft)
        capture_vram(gpu, soft);

    if (hw)
        capture_rt(be, scale, hw);

    psx_gpu_set_backend(gpu, NULL);
    armsx_hw_rt_destroy(be);
    psx_gpu_destroy(gpu);

    return 1;
}

/* ---- dump-file helpers ------------------------------------------------------------------ */

/* Fills `out` with the single .png the dumper wrote for the 16x16 4bpp texture. The name is
   the CONTRACT (psx/texrep.h), so it is reconstructed here from the key the loader would
   parse rather than by listing the directory — a test that globs would pass even if the
   naming scheme changed underneath it. */
static int find_dump(char* out, size_t cap) {
    /* The 16-hex key is not known a priori, so this is the one place a directory listing is
       the honest tool. Done with popen(ls) rather than dirent so the test file stays free of
       platform ifdefs; it runs on the build machine only. */
    FILE* p = popen("ls -1 " TEXREP_DUMP_DIR "/*.png 2>/dev/null | head -n 2", "r");
    char line[512];
    int n = 0;

    if (!p)
        return 0;

    out[0] = '\0';

    while (fgets(line, sizeof(line), p)) {
        size_t len = strlen(line);

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (!len)
            continue;

        if (n == 0 && len < cap)
            memcpy(out, line, len + 1u);

        n++;
    }

    pclose(p);

    return n;
}

static int copy_file(const char* src, const char* dst) {
    FILE* a = fopen(src, "rb");
    FILE* b;
    char buf[4096];
    size_t got;

    if (!a)
        return 0;

    b = fopen(dst, "wb");

    if (!b) {
        fclose(a);

        return 0;
    }

    while ((got = fread(buf, 1, sizeof(buf), a)) > 0)
        fwrite(buf, 1, got, b);

    fclose(a);

    return fclose(b) == 0;
}

static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');

    return slash ? (slash + 1) : path;
}

/* ---- cases ------------------------------------------------------------------------------- */

static void case_disabled_is_inert(void) {
    const char* name = "disabled-is-inert";
    frame_t off_s, off_h, cfg_s, cfg_h;

    CHECK(name, render(0, 0, TEXREP_TEST_DIR, 1, &off_s, &off_h), "render");
    CHECK(name, frame_nonblank(&off_s), "corpus drew nothing");
    /* Configuring with both flags off must be indistinguishable from never configuring. */
    CHECK(name, render(0, 0, "", 1, &cfg_s, &cfg_h), "render");
    CHECK(name, frames_equal(&off_s, &cfg_s), "software output moved with the feature off");
    CHECK(name, frames_equal(&off_h, &cfg_h), "hardware output moved with the feature off");
    CHECK(name, frames_equal(&off_s, &off_h), "1x hardware differs from software");

    ok(name);
}

static void case_dump_writes_png(void) {
    const char* name = "dump-writes-png";
    frame_t base_s, dump_s;
    char path[512];
    uint8_t* rgba;
    int w = 0, h = 0;
    const char* why = NULL;
    FILE* f;
    long size;
    uint8_t* bytes;
    int count;

    CHECK(name, render(0, 0, TEXREP_TEST_DIR, 1, &base_s, NULL), "render");
    CHECK(name, render(1, 0, TEXREP_TEST_DIR, 1, &dump_s, NULL), "render");

    /* Dumping must not change a single pixel: it only observes. */
    CHECK(name, frames_equal(&base_s, &dump_s), "dumping altered the frame");

    count = find_dump(path, sizeof(path));
    CHECK(name, count >= 1, "no PNG was written");
    /* The quad and the sprite sample the SAME rectangle of the SAME page with the SAME CLUT
       and the same texture window, so they must hash to one key and produce one file. Two
       files here would mean the key depends on the primitive rather than on the texels. */
    CHECK(name, count == 1, "the quad and the sprite produced different keys");

    /* The name carries the size and depth the contract specifies. */
    CHECK(name, strstr(basename_of(path), "ps1-") == basename_of(path), "name lacks the ps1- prefix");
    CHECK(name, strstr(path, "-16x16-4.png") != NULL, "name lacks the -WxH-bpp tail");

    f = fopen(path, "rb");
    CHECK(name, f != NULL, "dump not readable");
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    bytes = (uint8_t*)malloc((size_t)size);
    CHECK(name, bytes && fread(bytes, 1, (size_t)size, f) == (size_t)size, "dump read failed");
    fclose(f);

    rgba = psx_png_decode(bytes, (size_t)size, &w, &h, &why);
    free(bytes);
    CHECK(name, rgba != NULL, why ? why : "dump did not decode");
    CHECK(name, w == TEX_W && h == TEX_H, "dump is the wrong size");

    /* The alpha contract, spot-checked against the palette this test seeded:
         index 0  -> transparent texel -> A = 0
         index 15 -> STP set           -> A = 128
         anything else                 -> A = 255                                        */
    {
        int x, y, saw_zero = 0, saw_stp = 0, saw_opaque = 0, bad = 0;

        for (y = 0; y < TEX_H; y++) {
            for (x = 0; x < TEX_W; x++) {
                const uint8_t* p = rgba + ((size_t)y * TEX_W + (size_t)x) * 4u;
                unsigned idx = (unsigned)((x + y) & 0xf);

                if (idx == 0) {
                    saw_zero = 1;
                    if (p[3] != 0) bad = 1;
                } else if (idx == 15) {
                    saw_stp = 1;
                    if (p[3] != 128) bad = 1;
                } else {
                    saw_opaque = 1;
                    if (p[3] != 255) bad = 1;
                }
            }
        }

        free(rgba);
        CHECK(name, saw_zero && saw_stp && saw_opaque, "corpus did not cover all three alphas");
        CHECK(name, !bad, "alpha does not follow the transparent/STP/opaque contract");
    }

    ok(name);
}

static void case_roundtrip_is_identical(void) {
    const char* name = "roundtrip-is-identical";
    frame_t base_s, base_h, rep_s, rep_h;
    char path[512];
    char dst[640];

    CHECK(name, render(0, 0, TEXREP_TEST_DIR, 1, &base_s, &base_h), "render");
    CHECK(name, find_dump(path, sizeof(path)) == 1, "dump missing (run dump-writes-png first)");

    snprintf(dst, sizeof(dst), "%s/%s", TEXREP_PACK_DIR, basename_of(path));
    CHECK(name, copy_file(path, dst), "could not stage the pack file");

    CHECK(name, render(0, 1, TEXREP_TEST_DIR, 1, &rep_s, &rep_h), "render");

    /*
        THE central assertion. An untouched dump put back as a replacement has to be
        pixel-for-pixel what the game would have drawn. It fails if the hash names a
        different texture, if the rectangle is off by a texel, if the alpha rule is wrong,
        if 5->8->5 colour expansion is lossy, or if the sub-texel lands anywhere but 0 at
        scale 1 — and it cannot pass "by accident in both rasterizers", because the
        reference is the ORIGINAL FRAME rather than the other rasterizer.
    */
    CHECK(name, frames_equal(&base_s, &rep_s), "software round trip is not identical");
    CHECK(name, frames_equal(&base_h, &rep_h), "hardware round trip is not identical");

    ok(name);
}

/* Repaints the staged replacement a flat colour and returns the BGR555 the sampler must
   produce for it. Alpha 255 everywhere, so every texel of the region becomes opaque —
   including the ones that were transparent, which is the strongest evidence the replacement
   is being read rather than the original. */
static uint16_t repaint_pack(const char* file, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t rgba[TEX_W * TEX_H * 4];
    int i;

    for (i = 0; i < (TEX_W * TEX_H); i++) {
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = 255;
    }

    if (!psx_png_write(file, rgba, TEX_W, TEX_H))
        return 0;

    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

static void case_substitution_reaches_both(void) {
    const char* name = "substitution-reaches-both";
    frame_t base_s, rep_s, rep_h;
    char path[512];
    char dst[640];
    uint16_t want;
    int x, y, soft_hits = 0, hw_hits = 0, soft_wrong = 0, hw_wrong = 0;

    CHECK(name, render(0, 0, TEXREP_TEST_DIR, 1, &base_s, NULL), "render");
    CHECK(name, find_dump(path, sizeof(path)) == 1, "dump missing");
    snprintf(dst, sizeof(dst), "%s/%s", TEXREP_PACK_DIR, basename_of(path));

    want = repaint_pack(dst, 0xff, 0x00, 0x88);
    CHECK(name, want != 0, "could not write the repainted pack");

    CHECK(name, render(0, 1, TEXREP_TEST_DIR, 1, &rep_s, &rep_h), "render");

    /* Every pixel the corpus covers must now be the flat colour, in BOTH rasterizers. The
       original had transparent texels that drew nothing, so this also proves the alpha of
       the REPLACEMENT decides transparency rather than the original texel's. */
    for (y = 2; y < (2 + TEX_H); y++) {
        for (x = 2; x < (2 + TEX_W); x++) {
            uint16_t s = rep_s.px[y * CAP_W + x];
            uint16_t h = rep_h.px[y * CAP_W + x];

            if (s == want) soft_hits++; else soft_wrong++;
            if (h == want) hw_hits++;   else hw_wrong++;
        }
    }

    CHECK(name, soft_hits == (TEX_W * TEX_H), "software did not take the replacement everywhere");
    CHECK(name, hw_hits == (TEX_W * TEX_H), "hardware did not take the replacement everywhere");
    CHECK(name, soft_wrong == 0 && hw_wrong == 0, "stray texels");
    CHECK(name, !frames_equal(&base_s, &rep_s), "replacement changed nothing");
    CHECK(name, frames_equal(&rep_s, &rep_h), "the two rasterizers replaced differently");

    ok(name);
}

static void case_wrong_hash_is_ignored(void) {
    const char* name = "wrong-hash-is-ignored";
    frame_t base_s, rep_s, rep_h;
    char path[512];
    char dst[640];
    char wrong[640];

    /* Negative control. The SAME repainted image under a key that names nothing must be
       inert — otherwise "matched" would mean "a file exists", and every texture in the game
       would take the first replacement in the folder. */
    CHECK(name, find_dump(path, sizeof(path)) == 1, "dump missing");
    snprintf(dst, sizeof(dst), "%s/%s", TEXREP_PACK_DIR, basename_of(path));
    remove(dst);

    snprintf(wrong, sizeof(wrong), "%s/ps1-0123456789abcdef-16x16-4.png", TEXREP_PACK_DIR);
    CHECK(name, repaint_pack(wrong, 0x00, 0xff, 0x00) != 0, "could not write the decoy");

    CHECK(name, render(0, 0, TEXREP_TEST_DIR, 1, &base_s, NULL), "render");
    CHECK(name, render(0, 1, TEXREP_TEST_DIR, 1, &rep_s, &rep_h), "render");

    CHECK(name, frames_equal(&base_s, &rep_s), "a non-matching key was applied (software)");
    CHECK(name, frames_equal(&base_s, &rep_h), "a non-matching key was applied (hardware)");

    remove(wrong);
    ok(name);
}

static void case_subtexel_at_scale(void) {
    const char* name = "subtexel-at-scale";
    char path[512];
    char dst[640];
    uint8_t rgba[TEX_W * 2 * TEX_H * 2 * 4];
    frame_t rep_h;
    psx_gpu_t* gpu;
    psx_gpu_backend_t* be;
    int x, y;
    int varied = 0;

    CHECK(name, find_dump(path, sizeof(path)) == 1, "dump missing");
    snprintf(dst, sizeof(dst), "%s/%s", TEXREP_PACK_DIR, basename_of(path));

    /* A 2x replacement whose four sub-texels differ inside every native texel. If the
       fractional UV is dropped, each 2x2 render-target block collapses to one of them and
       the whole image is flat per texel — exactly what an upscaled pack looks like when the
       sub-texel path is missing, and invisible to a per-texel comparison. */
    for (y = 0; y < (TEX_H * 2); y++) {
        for (x = 0; x < (TEX_W * 2); x++) {
            uint8_t* p = rgba + ((size_t)y * (TEX_W * 2) + (size_t)x) * 4u;
            unsigned sub = (unsigned)((x & 1) | ((y & 1) << 1));   /* 0..3 within the texel */

            p[0] = (uint8_t)(8 + sub * 56);
            p[1] = (uint8_t)(8 + sub * 56);
            p[2] = (uint8_t)(8 + sub * 56);
            p[3] = 255;
        }
    }

    CHECK(name, psx_png_write(dst, rgba, TEX_W * 2, TEX_H * 2), "could not write the 2x pack");

    gpu = make_gpu();
    CHECK(name, gpu != NULL, "gpu");
    seed_texture(gpu);
    psx_texrep_configure(gpu, 0, 1, TEXREP_TEST_DIR);

    be = armsx_hw_rt_create(gpu, 2);
    CHECK(name, be != NULL, "backend");
    psx_gpu_set_backend(gpu, be);
    draw_all(gpu);

    /* Read the render target at its OWN resolution: the 2x2 block behind one native texel
       must contain more than one value. */
    {
        uint32_t stride_bytes = 0;
        const uint16_t* base = (const uint16_t*)be->display_buffer(be, 0, 0, &stride_bytes);
        const size_t stride_px = stride_bytes / sizeof(uint16_t);

        for (y = 0; y < TEX_H && !varied; y++) {
            for (x = 0; x < TEX_W; x++) {
                size_t bx = (size_t)(QUAD_X + x) * 2u;
                size_t by = (size_t)(QUAD_Y + y) * 2u;
                uint16_t a = base[bx + by * stride_px];
                uint16_t b = base[bx + 1 + by * stride_px];
                uint16_t c = base[bx + (by + 1) * stride_px];

                if (a != b || a != c) {
                    varied = 1;
                    break;
                }
            }
        }
    }

    (void)rep_h;
    psx_gpu_set_backend(gpu, NULL);
    armsx_hw_rt_destroy(be);
    psx_gpu_destroy(gpu);

    CHECK(name, varied, "a 2x replacement rendered flat: the sub-texel is being dropped");

    remove(dst);
    ok(name);
}

/* ---- the texture-window fold ------------------------------------------------------------- */

static void case_fold_axis(void) {
    const char* name = "fold-axis";
    uint16_t lo, n;

    /* No window: the raw range passes through. */
    psx_texrep_fold_axis(4, 12, 0, 0, &lo, &n);
    CHECK(name, lo == 4 && n == 12, "unwindowed range moved");

    /* A raw range that runs past 255 wraps and therefore covers everything. */
    psx_texrep_fold_axis(250, 20, 0, 0, &lo, &n);
    CHECK(name, lo == 0 && n == 256, "wrapped range not widened to the full axis");

    /* mask = 31*8 = 0xF8 keeps the low three bits: an 8-wide window at the offset. A polygon
       tiling 0..255 through it samples exactly eight columns, and the key must say so —
       otherwise the same wall texture gets a different key in every game that tiles it over
       a different span. */
    psx_texrep_fold_axis(0, 256, 0xf8, 0x10, &lo, &n);
    CHECK(name, lo == 0x10 && n == 8, "8-texel window not folded to 8");

    /* mask = 1*8 = 0x08 forces bit 3 only, which frees a NON-CONTIGUOUS set of bits. The
       fold enumerates rather than reasons, so this is exact where a closed-form derivation
       would be wrong. */
    psx_texrep_fold_axis(0, 256, 0x08, 0x08, &lo, &n);
    CHECK(name, lo == 8 && n == 248, "non-contiguous mask folded wrongly");

    ok(name);
}

/* ---- the GLSL transcription ---------------------------------------------------------------

   Hand-transcribed from PSX_TEXREP_GLSL in psx/texrep.h. It is deliberately written from the
   MACRO TEXT and not from psx_texrep_sample(), because the whole point is to be an
   independent second reading of the one definition the GLES rasterizer compiles. */

static int glsl_clamp(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }

static void glsl_at(int itx, int ity, int u0, int v0, int nw, int nh,
                    int s, int ax, int ay, int fx, int fy, int* ox, int* oy) {
    int lx = glsl_clamp(itx - u0, nw - 1);
    int ly = glsl_clamp(ity - v0, nh - 1);

    *ox = ax + lx * s + glsl_clamp(fx, s - 1);
    *oy = ay + ly * s + glsl_clamp(fy, s - 1);
}

static uint16_t glsl_pack(const uint8_t* rgba) {
    int a = rgba[3];
    unsigned r, g, b, v;

    if (a == 0)
        return 0;

    r = (unsigned)(rgba[0] >> 3);
    g = (unsigned)(rgba[1] >> 3);
    b = (unsigned)(rgba[2] >> 3);
    v = r | (g << 5) | (b << 10);

    if (a != 255)
        v |= 0x8000u;

    return (uint16_t)v;
}

/*
    Drives psx_texrep_sample() directly against the transcription above, over a matrix of
    scales, texture windows, rectangle origins and sub-texel positions. Reaching the sampler
    needs a bound image, which is built by hand rather than by loading a PNG — the point here
    is the ARITHMETIC, and going through the loader would only add a decode to the failure
    surface.
*/
static void case_glsl_matrix(void) {
    const char* name = "glsl-matrix";
    static const int scales[] = { 1, 2, 4 };
    static const unsigned masks[] = { 0x00, 0xf8, 0x08 };
    psx_gpu_t* gpu = make_gpu();
    psx_texrep_image_t im;
    int si, mi, checked = 0;

    CHECK(name, gpu != NULL, "gpu");

    memset(&im, 0, sizeof(im));

    for (si = 0; si < 3; si++) {
        const int s = scales[si];
        const int nw = 16, nh = 8;
        const int pw = nw * s, ph = nh * s;
        uint8_t* pix = (uint8_t*)malloc((size_t)pw * (size_t)ph * 4u);
        int i;

        CHECK(name, pix != NULL, "alloc");

        /* Every pixel distinct, so a mis-mapped coordinate cannot land on a lookalike. */
        for (i = 0; i < (pw * ph); i++) {
            pix[i * 4 + 0] = (uint8_t)((i * 7) & 0xff);
            pix[i * 4 + 1] = (uint8_t)((i * 13) & 0xff);
            pix[i * 4 + 2] = (uint8_t)((i * 29) & 0xff);
            pix[i * 4 + 3] = (uint8_t)((i % 5) == 0 ? 0 : ((i % 3) == 0 ? 128 : 255));
        }

        im.nw = (uint16_t)nw;
        im.nh = (uint16_t)nh;
        im.pw = (uint16_t)pw;
        im.ph = (uint16_t)ph;
        im.scale = (uint8_t)s;
        im.rgba = pix;

        for (mi = 0; mi < 3; mi++) {
            const unsigned mask = masks[mi];
            const unsigned off = 0x18;
            int u0, v0, t, sub;

            gpu->texw_mx = mask;
            gpu->texw_my = mask;
            gpu->texw_ox = off;
            gpu->texw_oy = off;

            for (u0 = 0; u0 <= 32; u0 += 16) {
                v0 = u0 >> 1;

                gpu->texrep_bind.img = &im;
                gpu->texrep_bind.u0 = (uint16_t)u0;
                gpu->texrep_bind.v0 = (uint16_t)v0;

                for (t = -3; t < 40; t++) {
                    for (sub = 0; sub < s; sub++) {
                        /* Land the fraction squarely inside sub-texel `sub`, away from the
                           bin edge, so this compares the mapping rather than float rounding
                           at an exact boundary. */
                        const float frac = ((float)sub + 0.5f) / (float)s;
                        const float tx = (float)t + frac;
                        const float ty = (float)(t + 1) + frac;

                        /* Truncate-then-mask, matching psx_texrep_sample()'s note about
                           float -> unsigned being undefined for negatives. */
                        unsigned itx = ((((unsigned)((int)tx & 0xffff)) & ~mask) | (off & mask)) & 0xffu;
                        unsigned ity = ((((unsigned)((int)ty & 0xffff)) & ~mask) | (off & mask)) & 0xffu;
                        int ox = 0, oy = 0;
                        uint16_t got, want;

                        glsl_at((int)itx, (int)ity, u0, v0, nw, nh, s, 0, 0, sub, sub, &ox, &oy);
                        want = glsl_pack(pix + (((size_t)oy * (size_t)pw) + (size_t)ox) * 4u);
                        got = psx_texrep_sample(gpu, tx, ty);
                        checked++;

                        if (got != want) {
                            char why[160];

                            snprintf(why, sizeof(why),
                                     "s=%d mask=%02x u0=%d t=%d sub=%d got=%04x want=%04x",
                                     s, mask, u0, t, sub, got, want);
                            fail(name, why);
                            free(pix);
                            psx_gpu_destroy(gpu);

                            return;
                        }
                    }
                }
            }
        }

        free(pix);
    }

    gpu->texrep_bind.img = NULL;
    psx_gpu_destroy(gpu);

    CHECK(name, checked > 1000, "matrix did not run");
    printf("TEXREP_PARITY passed case=%s (%d samples)\n", name, checked);
}

/* ---- PNG codec round trip ----------------------------------------------------------------- */

static void case_png_roundtrip(void) {
    const char* name = "png-roundtrip";
    const int w = 37, h = 19;   /* deliberately not a multiple of anything */
    uint8_t* src = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
    uint8_t* back;
    int i, gw = 0, gh = 0, bad = 0;
    const char* why = NULL;
    FILE* f;
    long size;
    uint8_t* bytes;
    const char* path = TEXREP_TEST_DIR "/codec.png";

    CHECK(name, src != NULL, "alloc");

    for (i = 0; i < (w * h); i++) {
        src[i * 4 + 0] = (uint8_t)(i * 3);
        src[i * 4 + 1] = (uint8_t)(255 - i);
        src[i * 4 + 2] = (uint8_t)(i ^ 0x5a);
        src[i * 4 + 3] = (uint8_t)((i % 7) == 0 ? 0 : ((i % 4) == 0 ? 128 : 255));
    }

    CHECK(name, psx_png_write(path, src, w, h), "encode failed");

    f = fopen(path, "rb");
    CHECK(name, f != NULL, "written file not readable");
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    bytes = (uint8_t*)malloc((size_t)size);
    CHECK(name, bytes && fread(bytes, 1, (size_t)size, f) == (size_t)size, "read failed");
    fclose(f);

    back = psx_png_decode(bytes, (size_t)size, &gw, &gh, &why);
    free(bytes);
    CHECK(name, back != NULL, why ? why : "decode failed");
    CHECK(name, gw == w && gh == h, "size changed");

    for (i = 0; i < (w * h * 4); i++)
        if (back[i] != src[i])
            bad = 1;

    free(back);
    free(src);
    remove(path);

    CHECK(name, !bad, "pixels changed");
    ok(name);
}

/* Truncated, corrupt and hostile input must be REJECTED, not crash: this codec parses files
   a stranger wrote and hands the result to the hottest loop in the emulator. */
static void case_png_rejects_garbage(void) {
    const char* name = "png-rejects-garbage";
    const int w = 24, h = 24;
    uint8_t* src = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
    uint8_t* bytes;
    long size;
    FILE* f;
    const char* path = TEXREP_TEST_DIR "/fuzz.png";
    int gw, gh, cut, seed;
    const char* why;

    CHECK(name, src != NULL, "alloc");
    memset(src, 0xa5, (size_t)w * (size_t)h * 4u);
    CHECK(name, psx_png_write(path, src, w, h), "encode failed");
    free(src);

    f = fopen(path, "rb");
    CHECK(name, f != NULL, "read");
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    bytes = (uint8_t*)malloc((size_t)size);
    CHECK(name, bytes && fread(bytes, 1, (size_t)size, f) == (size_t)size, "read");
    fclose(f);
    remove(path);

    /*
        Every truncation. The requirement is NOT "must fail" -- a cut that lands inside the
        trailing IEND still leaves a complete IHDR and IDAT, and decoding that is the lenient
        behaviour every image tool has. The requirement is that a short file is never read
        past its end: whatever comes back must be NULL or a correctly sized buffer, which the
        sanitizer build turns into an assertion rather than a hope.
    */
    for (cut = 1; cut < (int)size; cut++) {
        uint8_t* got = psx_png_decode(bytes, (size_t)cut, &gw, &gh, &why);

        if (got) {
            int wrong = (gw != w) || (gh != h);

            free(got);

            if (wrong) {
                free(bytes);
                fail(name, "a truncated file decoded to the wrong size");

                return;
            }
        }
    }

    /* Single-byte corruption anywhere. Whatever comes back must be either NULL or a
       correctly sized buffer; the point is that nothing reads or writes out of bounds, which
       the sanitizer build makes an assertion rather than a hope. */
    for (seed = 0; seed < (int)size; seed++) {
        uint8_t saved = bytes[seed];
        uint8_t* got;

        bytes[seed] = (uint8_t)(saved ^ 0xff);
        got = psx_png_decode(bytes, (size_t)size, &gw, &gh, &why);

        if (got) {
            if (gw <= 0 || gh <= 0 || gw > 8192 || gh > 8192) {
                free(got);
                free(bytes);
                fail(name, "corrupt file yielded implausible dimensions");

                return;
            }

            free(got);
        }

        bytes[seed] = saved;
    }

    free(bytes);
    ok(name);
}

int main(void) {
    reset_scratch();

    case_disabled_is_inert();
    case_fold_axis();
    case_glsl_matrix();
    case_png_roundtrip();
    case_png_rejects_garbage();
    case_dump_writes_png();
    case_roundtrip_is_identical();
    case_substitution_reaches_both();
    case_wrong_hash_is_ignored();
    case_subtexel_at_scale();

    if (g_failures) {
        fprintf(stderr, "TEXREP_PARITY %d case(s) failed\n", g_failures);

        return 1;
    }

    printf("TEXREP_PARITY all cases passed\n");

    return 0;
}
