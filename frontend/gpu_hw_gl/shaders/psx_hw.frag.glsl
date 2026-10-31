#version 300 es
// ARMSX — PS1 hardware rasterizer, fragment stage (DRAFT / INERT).
// Not compiled by anything; the runtime shader is embedded in frontend/gpu_hw_gl.c.
//
// Covers the hard case: textured + Gouraud + semi-transparent, with a 4bpp/8bpp CLUT
// lookup out of a VRAM-as-texture. Every step below cites the software rasterizer line in
// psx/dev/gpu.c that it reproduces.
//
// The order of operations is load-bearing and matches gpu_render_triangle():
//   texture window -> texel fetch -> texel==0 discard -> STP capture -> modulate
//   -> dither -> 5-bit truncate -> semi-transparency scaling -> mask bit
//
// This shader does NOT implement the blend equations itself. Three of the four map onto
// fixed-function blending; see the u_blend_mode block near the bottom and backend .

precision highp float;
precision highp int;
precision highp usampler2D;

// ---- varyings -----------------------------------------------------------------------
in      vec4  v_color;
in      vec2  v_uv;
flat in uvec4 v_uv_limit;
flat in uint  v_texpage;
flat in uint  v_clut;

// ---- resources ----------------------------------------------------------------------
// Native-resolution VRAM, 1024x512, R16UI. NOT the render target — GLES3 forbids sampling
// what you render to, and PS1 textures live in the same memory as the framebuffer, so a
// separate read copy is mandatory (backend ). R16UI rather than RGBA5551 because
// CLUT indexing needs exact integer bits, not a normalized float round-trip.
uniform usampler2D u_vram;

// ---- per-draw state -----------------------------------------------------------------
uniform int  u_resolution_scale;    // S
uniform bool u_textured;            // PA_TEXTURED, gpu.c:355
uniform bool u_raw;                 // PA_RAW,      gpu.c:367
uniform bool u_shaded;              // PA_SHADED,   gpu.c:325
uniform bool u_semi_transparent;    // PA_TRANSP,   gpu.c:259
uniform int  u_blend_mode;          // 0..3, gpu.c:410-431
uniform bool u_dither;              // GPUSTAT bit 9 (latched gpu.c:1941, never read there)
uniform bool u_true_color;          // enhancement: skip the 5-bit truncation
uniform bool u_clamp_uv;            //  halo fix; only meaningful when S > 1

// Texture window, GP0(E2). Arrives PRE-SHIFTED by 3 exactly as gpu.c:1948-1951 stores it,
// so the masking below is gpu.c:176-177 verbatim.
uniform uvec2 u_texwin_mask;
uniform uvec2 u_texwin_offset;

// Mask bit, GP0(E6) — gpu.c:1965-1967 is an empty stub, so this is net-new behaviour.
// "check before draw" is a stencil test on the host side, not a shader concern.
// "set on draw" reaches the shader only because the written mask bit also has to land in
// the render target's alpha channel for download fidelity (backend ).
uniform bool u_set_mask;

// STP pass selector for the two-pass split that blend mode 2 requires (backend ).
//   0 = single pass (ADD-family, or untextured)
//   1 = opaque pass  : discard fragments whose texel STP bit is set
//   2 = blended pass : discard fragments whose texel STP bit is clear
uniform int u_stp_pass;

// The PS1 4x4 dither matrix, gpu.c:17-22.
const int kDither[16] = int[16](
    -4,  0, -3,  1,
     2, -2,  3, -1,
    -3,  1, -4,  0,
     3, -1,  2, -2
);

out vec4 o_color;

// -------------------------------------------------------------------------------------
// VRAM access
// -------------------------------------------------------------------------------------
uint vramLoad(uint x, uint y) {
    // The & 1023 / & 511 wrap is FREE here and incidentally fixes a real bug: gpu.c's
    // texel fetch (gpu.c:184, :193, :202) never wraps tpx + tx at 1024, so a 15bpp page
    // at x=960 with u=255 reads into the next VRAM row instead of wrapping.
    // backend ,  item 8.
    return texelFetch(u_vram, ivec2(int(x & 1023u), int(y & 511u)), 0).r;
}

// -------------------------------------------------------------------------------------
// Texel fetch — gpu_fetch_texel(), gpu.c:175-205
// -------------------------------------------------------------------------------------
uint fetchTexel(uvec2 uv) {
    // Texture window. gpu.c:176-179.
    uint u = (uv.x & ~u_texwin_mask.x) | (u_texwin_offset.x & u_texwin_mask.x);
    uint v = (uv.y & ~u_texwin_mask.y) | (u_texwin_offset.y & u_texwin_mask.y);
    u &= 0xffu;   // 256x256 page wrap, gpu.c:178-179
    v &= 0xffu;

    // Texpage decode, gpu.c:254-258.
    uint tpx   = (v_texpage & 0xfu) << 6;    // 0..960, 64px granularity
    uint tpy   = (v_texpage & 0x10u) << 4;   // 0 or 256
    uint depth = (v_texpage >> 7) & 3u;

    // CLUT decode, gpu.c:256-257.
    uint clutx = (v_clut & 0x3fu) << 4;      // 0..1008, 16px granularity
    uint cluty = (v_clut >> 6) & 0x1ffu;

    if (depth == 0u) {
        // 4bpp paletted — gpu.c:183-189.
        uint word  = vramLoad(tpx + (u >> 2), tpy + v);
        uint index = (word >> ((u & 3u) << 2)) & 0xfu;
        return vramLoad(clutx + index, cluty);
    } else if (depth == 1u) {
        // 8bpp paletted — gpu.c:192-198.
        uint word  = vramLoad(tpx + (u >> 1), tpy + v);
        uint index = (word >> ((u & 1u) << 3)) & 0xffu;
        return vramLoad(clutx + index, cluty);
    }

    // 15bpp direct — gpu.c:201-203. (depth 3 behaves as depth 2.)
    return vramLoad(tpx + u, tpy + v);
}

// 16-bit BGR555 -> [0,1] float, expanding 5 bits to 8 with <<3 so that 31 -> 248, which is
// what gpu.c:398-400 and gpu.c:539-541 do. Round-tripping through BGR555() is exact.
vec3 unpack555(uint t) {
    uvec3 c = uvec3(t & 0x1fu, (t >> 5) & 0x1fu, (t >> 10) & 0x1fu);
    return vec3(c << 3u) / 255.0;
}

void main() {
    vec3 color;
    bool stp = false;     // per-texel semi-transparency bit, gpu.c:364-365

    if (u_textured) {
        vec2 uv = v_uv;

        // Clamp the interpolated UV into the primitive's own texel box. Prevents the
        // 1-texel halo that magnification pulls in from a neighbouring sprite sharing the
        // texture page (backend ). No-op at S == 1.
        if (u_clamp_uv) {
            uv = clamp(uv, vec2(v_uv_limit.xy), vec2(v_uv_limit.zw));
        }

        // Point sampling. Note this deliberately does NOT match the software renderer,
        // which bilinear-filters polygons unconditionally (gpu.c:359) while point-sampling
        // sprites (gpu.c:498). Hardware point-samples both. backend  items 1-2 —
        // ship point sampling by default with a "smooth textures" toggle that restores
        // the old look.
        uint texel = fetchTexel(uvec2(floor(uv)));

        // Texel 0x0000 is fully transparent and is dropped before anything else.
        // gpu.c:361-362 (polys) and gpu.c:506-507 (rects).
        if (texel == 0u) discard;

        stp = (texel & 0x8000u) != 0u;

        // Two-pass split for blend mode 2 (B - F), which has no alpha value that makes
        // reverse-subtract an identity write for the opaque texels. backend .
        if (u_stp_pass == 1 && stp)  discard;
        if (u_stp_pass == 2 && !stp) discard;

        vec3 t = unpack555(texel);

        if (u_raw) {
            // Raw texture: written through verbatim, no modulation. gpu.c:367-368.
            color = t;
        } else {
            // Modulation: (texel8 * vertex8) / 128, so 0x80 is identity. gpu.c:374-380.
            // v_color is the Gouraud-interpolated colour for shaded primitives
            // (gpu.c:326-328) or the flat primitive colour (gpu.c:352).
            color = clamp(t * v_color.rgb * (255.0 / 128.0), 0.0, 1.0);
        }
    } else {
        color = v_color.rgb;
        // An untextured primitive has no STP bit; `transp` is constant across the whole
        // primitive (gpu.c:259), so it always takes the single-pass path, mode 2 included.
        stp = u_semi_transparent;
    }

    // ---- dithering ------------------------------------------------------------------
    // Indexed by the NATIVE coordinate, not the upscaled fragment coordinate. Indexing by
    // gl_FragCoord directly would give a 4-pixel pattern at every scale, which reads as
    // high-frequency noise that worsens with S and shimmers in motion. Dividing by S makes
    // each dither cell an SxS block, i.e. what native output looks like magnified.
    // backend .
    //
    // KNOWN INTENTIONAL DIVERGENCE: gpu.c indexes the kernel relative to the primitive's
    // bounding box (gpu.c:330-331), which makes the pattern move with the primitive and
    // breaks up gradients across adjacent primitives. That is a bug; this is correct.
    // Also, gpu.c only ever dithers Gouraud primitives (gpu.c:325) and never consults
    // GPUSTAT bit 9. backend ,  items 4-6.
    if (u_dither && !u_raw) {
        ivec2 native = ivec2(gl_FragCoord.xy) / u_resolution_scale;
        float d = float(kDither[(native.x & 3) + ((native.y & 3) << 2)]) / 255.0;
        color = clamp(color + vec3(d), 0.0, 1.0);
    }

    // ---- 5-bit truncation ------------------------------------------------------------
    // gpu.c does this implicitly on every VRAM write via BGR555() (gpu.c:392, :395, :443).
    // The HW path must do it explicitly or upscaled output drifts away from native.
    if (!u_true_color) {
        color = floor(color * 31.0 + 0.5) / 31.0;
    }

    // ---- semi-transparency -----------------------------------------------------------
    // The blend EQUATION is fixed-function; this shader only pre-scales F and picks the
    // alpha that drives dst factor SRC_ALPHA. Host blend state is:
    //
    //   modes 0/1/3 : glBlendEquation(GL_FUNC_ADD)
    //   mode  2     : glBlendEquation(GL_FUNC_REVERSE_SUBTRACT)
    //   all modes   : glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ONE, GL_ZERO)
    //
    // giving result = src*1 + dst*src_alpha (or dst*src_alpha - src*1 for mode 2).
    // The separate alpha func (ONE, ZERO) is what lets the alpha channel store the mask
    // bit instead of a blended alpha — see below and backend .
    //
    //   mode 0: emit F*0.5, a=0.5 -> 0.5F + 0.5B   (gpu.c:411-415)
    //   mode 1: emit F,     a=1.0 -> F + B         (gpu.c:416-420)
    //   mode 2: emit F,     a=1.0 -> B - F         (gpu.c:421-425)
    //   mode 3: emit F*0.25,a=1.0 -> 0.25F + B     (gpu.c:426-430)
    //
    // Opaque texels inside a semi-transparent textured primitive emit a=0.0 and unscaled
    // F, so F + B*0 = F: a plain write with no state change and no second draw. That trick
    // works for the ADD family only, which is exactly why mode 2 needs u_stp_pass.
    float blend_alpha = 0.0;

    if (u_semi_transparent && stp) {
        if (u_blend_mode == 0) {
            color *= 0.5;
            blend_alpha = 0.5;
        } else if (u_blend_mode == 3) {
            color *= 0.25;
            blend_alpha = 1.0;
        } else {
            // modes 1 and 2: F passes through unscaled
            blend_alpha = 1.0;
        }
    }

    // ---- mask bit --------------------------------------------------------------------
    // The stencil buffer carries the mask bit for the "check before draw" test (that is a
    // host-side glStencilFunc, not visible here). The alpha channel carries a redundant
    // copy so that download_vram() can reconstruct bit 15 of the 16-bit VRAM word without
    // a second stencil readback. Because the alpha blend func is (ONE, ZERO), whatever is
    // written here lands in the target unmodified.
    //
    // gpu.c never implements any of this (gpu.c:1965-1967), so enabling it WILL change
    // output relative to the software renderer. backend , .
    float mask_out = (u_set_mask || stp) ? 1.0 : 0.0;

    // Fragments that participate in fixed-function blending need blend_alpha in .a, and
    // fragments that do not need mask_out in .a. They cannot both be there. Resolution:
    // when blending is active the mask bit is written by the stencil op alone, and the
    // alpha-channel copy is refreshed by the host during download from the stencil buffer.
    o_color = vec4(color, (u_semi_transparent && stp) ? blend_alpha : mask_out);
}
