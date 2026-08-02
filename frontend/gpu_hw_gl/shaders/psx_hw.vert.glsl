#version 300 es
// ARMSX — PS1 hardware rasterizer, vertex stage (DRAFT / INERT).
// Not compiled by anything. See frontend/HW_RENDERER_DESIGN.md §2.3.
//
// Positions arrive in NATIVE PlayStation VRAM pixel coordinates with the drawing offset
// already applied CPU-side, exactly as psx/dev/gpu.c:279-284 does today. This stage's
// only geometric job is to scale by u_resolution_scale and map into clip space over the
// full 1024x512 VRAM. Texture coordinates are NEVER scaled (design doc §3.1).

precision highp float;
precision highp int;

// ---- attributes (psx_hw_vertex_t, gpu_hw.h) ----------------------------------------
in vec3  a_pos;       // x, y in native VRAM px; z carries w (1.0 until PGXP lands)
in vec4  a_color;     // 0x00BBGGRR unpacked to [0,1] by the vertex-format declaration
in uvec2 a_uv;        // native texel coords, 0..255
in uint  a_texpage;   // raw texpage word  (gpu.c:1106)
in uint  a_clut;      // raw CLUT word     (gpu.c:1105)

// Per-primitive UV bounding box, replicated to every vertex of the primitive by the
// batcher. Used to kill the 1-texel halo that magnification produces when an interpolated
// UV drifts onto a neighbouring sprite in the same texture page (design doc §3.2 item 2).
// At S == 1 this clamp is a no-op; it only ever engages when upscaling.
in uvec4 a_uv_limit;  // (min_u, min_v, max_u, max_v)

// ---- uniforms -----------------------------------------------------------------------
uniform int u_resolution_scale;   // S

// ---- varyings -----------------------------------------------------------------------
out      vec4  v_color;
out      vec2  v_uv;              // interpolated, native texel space
flat out uvec4 v_uv_limit;
flat out uint  v_texpage;
flat out uint  v_clut;

void main() {
    float s = float(u_resolution_scale);

    // Native pixel n covers render-target columns [n*S, n*S + S - 1]. Their fragment
    // centres land at n*S + 0.5 .. n*S + S - 0.5, all strictly inside [n*S, (n+1)*S), so
    // a primitive spanning native [a, b) covers exactly (b - a) * S columns.
    //
    // DO NOT add a half-pixel offset here. Adding +0.5*S is the classic upscaling bug: at
    // S == 1 it shifts everything by the same half pixel and most tests still look right,
    // but at S == 2 and S == 3 it shows up as a half-native-pixel shear. Always verify at
    // an odd scale (design doc §3.3, §6 Stage 4 risk).
    vec2 scaled = a_pos.xy * s;

    // Clip space over the whole 1024x512 VRAM, scaled. Y is flipped because VRAM row 0 is
    // the top row while GL's framebuffer origin is bottom-left.
    vec2 vram = vec2(1024.0 * s, 512.0 * s);
    vec2 ndc  = vec2(scaled.x / vram.x * 2.0 - 1.0,
                     1.0 - scaled.y / vram.y * 2.0);

    gl_Position = vec4(ndc, 0.0, 1.0);

    v_color    = a_color;
    v_uv       = vec2(a_uv);
    v_uv_limit = a_uv_limit;
    v_texpage  = a_texpage;
    v_clut     = a_clut;
}
