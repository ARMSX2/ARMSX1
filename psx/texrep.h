#ifndef PSX_TEXREP_H
#define PSX_TEXREP_H

/*
    ARMSX — PlayStation texture dumping and replacement.

    WHY THIS IS NOT A PORT OF ANYTHING
    ----------------------------------
    A PS2 emulator can key replacements off a texture OBJECT: the GS is handed a base
    pointer, a width, a height, a format and a palette, and that tuple names a thing. The
    PlayStation has no such object. GP0 hands the GPU a texture PAGE (a 64-halfword x 256-row
    window into the single 1 MB VRAM), a colour depth, a CLUT position and per-vertex UVs, and
    the "texture" is whatever bytes happen to live in the rectangle those UVs sweep at the
    moment the primitive is drawn. The same page holds a different image ten primitives later.

    So a key here has to be CONTENT-ADDRESSED. What identifies a PS1 texture is the bytes it
    samples, not where it lives.

    THE KEY
    -------
    A 64-bit FNV-1a over, in this order:

      1. a 9-byte header:  depth (0=4bpp, 1=8bpp, 2=15bpp), w lo, w hi, h lo, h hi,
                           texw_mx, texw_my, texw_ox, texw_oy
      2. the CLUT:         16 halfwords at 4bpp, 256 at 8bpp, none at 15bpp,
                           little-endian, read straight out of VRAM
      3. the texel data:   the VRAM halfwords backing the sampled rectangle, row by row,
                           little-endian, ONLY the halfwords the rectangle actually covers

    Every halfword is folded in as two bytes (lo, hi) so the key does not depend on host
    endianness. The rectangle is in NATIVE TEXEL space, post-texture-window (see below), and
    the halfword span per row is derived from it by the depth's packing: 4 texels per halfword
    at 4bpp, 2 at 8bpp, 1 at 15bpp.

    The header is in the key because a 32x32 region and the 64x16 region that shares its
    bytes are different textures, and because the texture window changes what a given UV
    samples. The CLUT is in the key because palette swaps (the same indices, a different
    palette) are different textures and are exactly the case a pack author cares about.

    THE TEXTURE WINDOW (GP0(E2))
    ----------------------------
    Hardware rewrites every texcoord as  t = (t AND NOT(mask*8)) OR (offset AND mask)*8,
    which this core has already pre-shifted into texw_mx/texw_ox. That transform is
    many-to-one: a polygon whose UVs sweep 0..255 through a mask of 0xF8 samples only eight
    distinct columns.

    Keying on the RAW UV bounding box would therefore give the same 8x8 wall texture a
    different key in every game that tiles it over a different span, and the dump would be the
    same eight columns repeated 32 times. So the rectangle is FOLDED THROUGH THE WINDOW
    FIRST: psx_texrep_fold_axis() applies the window transform to every value in the sampled
    range and takes the min and max. That is exact for any mask, including the non-contiguous
    ones hardware permits (mask=1 gives NOT(8) = 0xF7, which frees bits 0-2 and 4-7 but not
    bit 3), because it enumerates rather than reasons. It runs at most 256 iterations per
    axis, once per keyed primitive, and only when the feature is on.

    The transform is idempotent -- ((t & ~m) | (o & m)) & ~m | (o & m) == (t & ~m) | (o & m)
    -- so sampling the folded rectangle through the ordinary sampler reproduces exactly the
    texels the game sees, and the sampler can fold an incoming UV again at fetch time without
    landing outside the image.

    ON-DISK FORMAT
    --------------
      ps1-<key>-<w>x<h>-<bpp>.png     e.g.  ps1-3f2a91c0de4b7188-64x32-4.png

    <key> is 16 lowercase hex digits, big-endian. Everything after it is DOCUMENTATION: the
    loader parses the first 16-hex-digit run in the basename and ignores the rest, so a pack
    author may rename ps1-3f2a...-64x32-4.png to ps1-3f2a...-aerith-dress.png and it still
    matches. A bare <key>.png works too.

    PNG, 8-bit RGBA (colour type 6), non-interlaced, is what the dumper writes. The loader
    accepts colour types 0, 2, 3, 4 and 6 at bit depth 8, plus indexed at 1/2/4 bits, which
    covers what every editor saves.

    ALPHA IS THE PLAYSTATION'S SEMI-TRANSPARENCY BIT, NOT A BLEND FACTOR. There is no such
    thing as a partially transparent PS1 texel; there is bit 15 ("STP"), which selects
    whether the primitive's blend equation applies to that texel at all.

        A = 0          the transparent texel (VRAM 0x0000). Nothing is drawn.
        A = 255        opaque texel, STP = 0
        1 <= A <= 254  STP = 1 -- the primitive's blend mode applies

    The dumper writes exactly 0, 128 and 255, so a dump fed straight back as a replacement is
    BIT-IDENTICAL to no replacement at all: 5-bit channels expand as (v<<3)|(v>>2) and
    contract as v>>3, which round-trips every one of the 32 levels exactly. That identity is
    what tests/gpu_texrep_parity.c asserts on all three rasterizers.

    One consequence worth putting in the pack author's hands: OPAQUE BLACK IS TRANSPARENT.
    RGB 000000 with A=255 packs to 0x0000, which is the PlayStation's transparent texel and
    is discarded by every rasterizer. Paint 000008 for a near-black that draws.

    RESOLUTION, FILTERING AND MODULATION
    ------------------------------------
    A replacement must be an INTEGER multiple of the region it replaces: (w*S) x (h*S) for
    S in 1..PSX_TEXREP_MAX_SCALE. Anything else is rejected with a log line naming the file
    and both sizes.

    * Modulation (HW_RENDERER_DESIGN.md §0.5.15). The sampler returns a BGR555 texel, so a
      replacement re-enters gpu.c at exactly the point a VRAM texel would have, and
      psx_gpu_modulate_channel() truncates it the same way. A higher-resolution replacement
      therefore does NOT get more colour precision: it gets more TEXELS, each of which is
      still a 15-bit value modulated by the same truncating multiply. That is deliberate --
      the render target is BGR555 in all three rasterizers, so extra precision would be
      thrown away at the write anyway, and keeping the pipeline identical is what lets the
      round-trip identity above hold.

    * Filtering ([video] texture_filter). A bound replacement BYPASSES the filter and is
      point-sampled at its own resolution. The filter exists to hide the size of a native
      texel; a replacement that is S times finer has already done that, and running bilinear
      or xBR over it would blur detail the author drew. It also keeps the three rasterizers
      in agreement: the CPU paths and the shader would otherwise have to reproduce three
      filter kernels over the same atlas.

    COST WHEN DISABLED
    ------------------
    psx_gpu_t::texrep is NULL and psx_gpu_t::texrep_bind.img is NULL. gpu_poly()/gpu_rect()
    skip the bind on a NULL `texrep`, one predictable branch per primitive. gpu_fetch_texel()
    tests texrep_bind.img, which is a load from a struct it has already touched and a branch
    that is never taken. NOTHING allocates, scans, hashes or opens a file. There is no
    per-frame work at all.
*/

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct psx_gpu_t;
struct psx_texrep_t;

/* Largest region that will be keyed at all. A PS1 texture page is 256x256 texels, so this is
   the whole page; anything claiming to be bigger is a UV bounding box that has wrapped and is
   not a texture. Also bounds the dump decode and the hash walk. */
#define PSX_TEXREP_MAX_DIM     256
#define PSX_TEXREP_MAX_SCALE   8

/* A decoded replacement. Owned by psx_texrep_t and never freed or moved while a session
   runs, so a rasterizer backend may cache a pointer to one across frames -- see `be_*`. */
typedef struct psx_texrep_image_t {
    uint64_t key;
    uint16_t nw, nh;      /* native texel size of the region this stands in for */
    uint16_t pw, ph;      /* nw * scale, nh * scale */
    uint8_t  scale;       /* 1..PSX_TEXREP_MAX_SCALE */
    uint8_t* rgba;        /* pw * ph * 4, owned */

    /* Owned by the rasterizer backend, untouched by this module beyond zeroing at load.
       frontend/gpu_hw_gl.c uses them to record where the image sits in its atlas; the CPU
       rasterizers never look. `be_epoch` lets a backend notice a rebuilt pack. */
    uint32_t be_epoch;
    uint16_t be_x, be_y;
    uint8_t  be_resident;
    uint8_t  be_pad;
} psx_texrep_image_t;

/* The replacement in force for the primitive currently being drawn. `img` NULL means "no
   replacement", which is the state the field is permanently in unless replacement is on AND
   a pack file matched. u0/v0 are the folded rectangle's origin in native texel space. */
typedef struct psx_texrep_bind_t {
    const psx_texrep_image_t* img;
    uint16_t u0, v0;
} psx_texrep_bind_t;

/* ---- lifetime -------------------------------------------------------------------------

   `dir` is the base directory. Dumps go to <dir>/dump, packs are read from
   <dir>/replacements; both are created on demand. A NULL or empty dir, or both flags zero,
   destroys any existing instance and puts gpu->texrep back to NULL -- which is the disabled
   state described in the header comment, not a "disabled but allocated" one.

   Safe to call repeatedly with the same arguments; it only rebuilds when something changed.
   Owns its copy of `dir`. */
void psx_texrep_configure(struct psx_gpu_t* gpu, int dump_enabled, int replace_enabled,
                          const char* dir);
void psx_texrep_destroy(struct psx_gpu_t* gpu);

/* Bumped every time the pack is rebuilt, so a backend can drop its atlas. Never 0. */
uint32_t psx_texrep_epoch(const struct psx_gpu_t* gpu);

/* ---- per-primitive --------------------------------------------------------------------

   Called ONCE per textured primitive from gpu_poly()/gpu_rect(), before the backend hook and
   before the software rasterizer, so all three consume the same decision. Sets
   gpu->texrep_bind. psx_texrep_unbind() clears it and must be called on every path out.

   `u`/`v` are the RAW per-vertex UVs, `n` how many are meaningful (3 or 4 for a polygon).
   For a sprite pass n = 2 with the top-left UV and the bottom-right one it sweeps to. */
void psx_texrep_bind_prim(struct psx_gpu_t* gpu, const uint16_t* u, const uint16_t* v, int n,
                          uint32_t tpx, uint32_t tpy, uint32_t clutx, uint32_t cluty,
                          int depth);
void psx_texrep_unbind(struct psx_gpu_t* gpu);

/* Any write to VRAM invalidates the per-frame key cache: the cache is keyed on ADDRESSES and
   the key is over CONTENT, so a texture uploaded over another at the same page would
   otherwise keep the old key. Cheap -- a generation counter bump. */
void psx_texrep_invalidate(struct psx_gpu_t* gpu);

/* ---- sampling -------------------------------------------------------------------------

   The mapping from a native UV to a pixel of the bound replacement, shared by all three
   rasterizers. The GLES rasterizer cannot call it, so the arithmetic is ALSO written once as
   PSX_TEXREP_GLSL below and frontend/gpu_hw_gl.c compiles that text -- the same construction
   psx/dev/gpu.h uses for PSX_GPU_MASK_GLSL, and for the same reason: three copies of a
   formula is how §0.5.12, §0.5.13 and §0.5.15 all happened.

   tx/ty are the un-windowed float UVs the rasterizer interpolated. The fractional part
   selects the sub-texel when scale > 1; a rasterizer running at native resolution passes
   whole numbers and gets sub-texel 0, which is the only thing it can display anyway. */
uint16_t psx_texrep_sample(const struct psx_gpu_t* gpu, float tx, float ty);

/* Exposed for tests/gpu_texrep_parity.c: the axis fold described in the header comment. */
void psx_texrep_fold_axis(unsigned lo_raw, unsigned count, unsigned mask, unsigned off,
                          uint16_t* out_lo, uint16_t* out_count);

/* ---- backend enumeration ---------------------------------------------------------------

   frontend/gpu_hw_gl.c needs the pixels of a bound image to put them in its atlas. Nothing
   else does. */
static inline const psx_texrep_image_t* psx_texrep_bound_image(const psx_texrep_bind_t* b) {
    return b ? b->img : (const psx_texrep_image_t*)0;
}

/*
    THE atlas mapping, written once.

    Given the folded rectangle origin (u0, v0), the image's scale S, the atlas origin
    (ax, ay), and a texel (itx, ity) that has ALREADY been through the texture window, the
    replacement pixel is at

        ax + (clamp(itx - u0, 0, nw-1) * S + fx,   ay + clamp(ity - v0, 0, nh-1) * S + fy)

    with fx, fy in 0..S-1 taken from the fractional part of the interpolated UV. The clamp is
    not decoration: a rasterizer may interpolate a hair outside the vertex UV bounding box the
    fold was computed from, and without it that reads a neighbouring image out of the atlas.

    frontend/gpu_hw_gl.c COMPILES THIS TEXT -- it is the GLES rasterizer's only definition of
    the mapping, the same construction PSX_GPU_MASK_GLSL uses in psx/dev/gpu.h. GLSL cannot be
    run on the build machine, so tests/gpu_texrep_parity.c closes the loop from the other
    side: it transcribes the text below into C by hand and asserts psx_texrep_sample() agrees
    with it over a matrix of scales, texture windows, rectangles and sub-texel positions. Two
    independent transcriptions of one text is the strongest check available without a GPU, and
    it is strictly better than the situation §0.5.12/§0.5.13/§0.5.15 were found in, where the
    formula existed three times and no test compared any two of them.

    The precision statements are repeated here because this text is prepended to the draw
    fragment shader AHEAD of its own: GLSL ES 3.00 gives `float` no default precision in a
    fragment shader, so texrep_pack() would not compile without them. Repeating a precision
    statement is legal.
*/
#define PSX_TEXREP_GLSL \
    "precision highp float;\n" \
    "precision highp int;\n" \
    "int texrep_clamp(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }\n" \
    "ivec2 texrep_at(int itx, int ity, int u0, int v0, int nw, int nh,\n" \
    "                int s, int ax, int ay, int fx, int fy) {\n" \
    "    int lx = texrep_clamp(itx - u0, nw - 1);\n" \
    "    int ly = texrep_clamp(ity - v0, nh - 1);\n" \
    "    return ivec2(ax + lx * s + texrep_clamp(fx, s - 1),\n" \
    "                 ay + ly * s + texrep_clamp(fy, s - 1));\n" \
    "}\n" \
    /* RGBA8 -> BGR555 + bit 15, the alpha contract from psx/texrep.h. Identical to the tail
       of psx_texrep_sample(); both must stay so or the shader and the CPU rasterizers
       disagree on the same pack. */ \
    "uint texrep_pack(vec4 c) {\n" \
    "    int a = int(c.a * 255.0 + 0.5);\n" \
    "    if (a == 0) return 0u;\n" \
    "    uint r = uint(int(c.r * 255.0 + 0.5) >> 3);\n" \
    "    uint g = uint(int(c.g * 255.0 + 0.5) >> 3);\n" \
    "    uint b = uint(int(c.b * 255.0 + 0.5) >> 3);\n" \
    "    uint v = r | (g << 5) | (b << 10);\n" \
    "    if (a != 255) v |= 0x8000u;\n" \
    "    return v;\n" \
    "}\n"

#ifdef __cplusplus
}
#endif

#endif
