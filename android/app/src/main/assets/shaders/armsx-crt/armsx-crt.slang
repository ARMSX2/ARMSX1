#version 450

/*
    ARMSX CRT — a single-pass CRT simulation for RetroArch (slang) shader chains.

    Copyright (c) 2026 Moon. Part of ARMSX; see the repository LICENSE.

    WRITTEN FOR ARMSX RATHER THAN ADAPTED. The libretro `slang-shaders` collection is a
    mixed-licence work (largely GPL-2.0-or-later) and ARMSX is proprietary, so nothing from
    it is bundled. The in-app shader manager still downloads that collection on request —
    that is the user's own copy, not something ARMSX redistributes.

    ONE PASS, ON PURPOSE. Every effect here is a per-fragment function of at most two
    texture fetches, so the whole chain costs one full-screen pass at the letterboxed
    output size. Multi-pass CRT presets (blur/bloom/reflection passes) are several times
    the cost for a look that is not better on a 5-inch handheld panel.

    THE INPUT IS ALWAYS NATIVE PLAYSTATION RESOLUTION. The ARMSX shader layer downsamples
    an internal-resolution frame back to native before the chain runs, so SourceSize.y is
    the real scanline count (240 for NTSC 240p) whatever [video] internal_scale is set to.
    The scanline maths below depends on that and would draw 720 scanlines on a 3x frame.
*/

layout(push_constant) uniform Push
{
    vec4 SourceSize;    /* xy = texels, zw = 1/texels */
    vec4 OutputSize;
    uint FrameCount;
} params;

/* Parameters live in the UBO rather than the push constant: push-constant space is only
   guaranteed to be 128 bytes and the three vec4s + uint above already claim 52 of them. */
layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    float crt_scanline;
    float crt_beam_min;
    float crt_beam_max;
    float crt_mask_type;
    float crt_mask;
    float crt_sharp;
    float crt_curve;
    float crt_corner;
    float crt_vignette;
    float crt_bright;
    float crt_gamma_in;
    float crt_gamma_out;
} global;

/* A parameter whose maximum equals its minimum is a CAPTION, not a control — ARMSX's
   parameter editor renders those as section headings, so the run below each one reads as a
   labelled group instead of one flat list of twelve sliders. */
#pragma parameter cap_beam "[ Scanlines ]" 0.0 0.0 0.0 0.0
#pragma parameter crt_scanline "Scanline depth" 1.0 0.0 1.0 0.05
#pragma parameter crt_beam_min "Beam width, dark areas" 0.55 0.20 1.50 0.05
#pragma parameter crt_beam_max "Beam width, bright areas" 1.05 0.20 1.50 0.05

#pragma parameter cap_mask "[ Phosphor mask ]" 0.0 0.0 0.0 0.0
#pragma parameter crt_mask_type "Mask (0 off, 1 grille, 2 slot, 3 shadow)" 1.0 0.0 3.0 1.0
#pragma parameter crt_mask "Mask strength" 0.45 0.00 1.00 0.05

#pragma parameter cap_geom "[ Geometry ]" 0.0 0.0 0.0 0.0
#pragma parameter crt_sharp "Horizontal sharpness" 0.75 0.00 1.00 0.05
#pragma parameter crt_curve "Screen curvature" 0.0 0.0 0.30 0.01
#pragma parameter crt_corner "Corner rounding" 0.0 0.0 0.05 0.005
#pragma parameter crt_vignette "Vignette" 0.0 0.0 1.00 0.05

#pragma parameter cap_colour "[ Colour ]" 0.0 0.0 0.0 0.0
#pragma parameter crt_bright "Brightness compensation" 1.30 0.50 2.50 0.05
#pragma parameter crt_gamma_in "CRT gamma (in)" 2.40 1.00 4.00 0.05
#pragma parameter crt_gamma_out "Display gamma (out)" 2.20 1.00 4.00 0.05

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

/* Barrel distortion about the centre. Deliberately mild: on a handheld the picture is
   already small, and anything past ~0.15 costs more edge than it buys atmosphere. */
vec2 curve_uv(vec2 uv, float amount)
{
    if (amount <= 0.0001) {
        return uv;
    }
    vec2 c = uv * 2.0 - 1.0;
    c *= 1.0 + dot(c, c) * amount * 0.25;
    return c * 0.5 + 0.5;
}

/* Sharp bilinear along x only.

   Pure nearest-neighbour at a non-integer scale drops or doubles columns unevenly, which
   reads as crawling vertical edges; pure bilinear is a blurry mess at 3-4x. This keeps the
   texel flat across its middle and confines the ramp to roughly one output pixel, so edges
   stay crisp and evenly spaced. At sharpness 1 it degenerates to nearest, at 0 to plain
   bilinear. Vertical is handled by the beam profile instead — that IS the scanline. */
float sharp_x(float texel_x, float px_per_texel, float sharpness)
{
    float f = fract(texel_x);
    float ramp = clamp((1.0 - sharpness) + 1.0 / max(px_per_texel, 1.0) * sharpness, 0.0001, 1.0);
    return clamp((f - 0.5) / ramp + 0.5, 0.0, 1.0);
}

/* Scanline beam profile: a Gaussian whose width grows with the line's own brightness, so a
   bright line blooms and closes the gap while a dark one stays a thin stripe. That
   brightness dependence is the whole difference between "looks like a CRT" and "looks like
   a grid drawn over the picture". Per-channel because a saturated red line should bloom in
   red only. */
vec3 beam(float dist, vec3 level)
{
    vec3 width = mix(vec3(global.crt_beam_min), vec3(global.crt_beam_max), level);
    vec3 d = vec3(dist) / max(width, vec3(0.05));
    return exp2(-2.0 * d * d);
}

/* Phosphor mask, evaluated in PHYSICAL OUTPUT PIXELS (gl_FragCoord), never in source
   texels: the mask is a property of the glass, so it must not scale with the emulated
   resolution or with curvature. */
vec3 phosphor(vec2 pixel, float type, float strength)
{
    if (strength <= 0.001 || type < 0.5) {
        return vec3(1.0);
    }

    /* The unlit channels dip to `dim` rather than to zero. A hard 0 is what makes bundled
       CRT presets look black and tinted on a phone at 3-4x; the strength slider then has
       to be pushed so low that the mask disappears entirely. */
    const float dim = 0.35;
    vec3 m = vec3(1.0);

    if (type < 1.5) {
        /* Aperture grille: vertical R/G/B stripes, one triad per 3 output pixels. */
        float i = mod(floor(pixel.x), 3.0);
        m = (i < 0.5) ? vec3(1.0, dim, dim)
          : (i < 1.5) ? vec3(dim, 1.0, dim)
                      : vec3(dim, dim, 1.0);
    } else if (type < 2.5) {
        /* Slot mask: the same triads, shifted half a triad every two rows, which is what
           breaks the vertical stripe up into the brick pattern of a consumer TV. */
        float shift = mod(floor(pixel.y / 2.0), 2.0) * 1.5;
        float i = mod(floor(pixel.x) + shift, 3.0);
        m = (i < 0.5) ? vec3(1.0, dim, dim)
          : (i < 1.5) ? vec3(dim, 1.0, dim)
                      : vec3(dim, dim, 1.0);
        /* Darken the gap between slots. */
        m *= mix(1.0, 0.8, step(1.5, mod(floor(pixel.y), 3.0)));
    } else {
        /* Shadow mask: a 2x1 staggered dot triad, softer and less directional. */
        float shift = mod(floor(pixel.y), 2.0) * 1.5;
        float i = mod(floor(pixel.x) + shift, 3.0);
        m = (i < 0.5) ? vec3(1.0, dim, dim)
          : (i < 1.5) ? vec3(dim, 1.0, dim)
                      : vec3(dim, dim, 1.0);
    }

    return mix(vec3(1.0), m, strength);
}

void main()
{
    vec2 uv = curve_uv(vTexCoord, global.crt_curve);

    /* Everything the curved tube pushes off-screen is bezel, not picture. */
    vec2 in_bounds = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    float inside = in_bounds.x * in_bounds.y;

    vec2 tex_size = params.SourceSize.xy;
    vec2 coord = uv * tex_size - 0.5;

    /* ---- horizontal ---- */
    float px_per_texel = params.OutputSize.x / max(tex_size.x, 1.0);
    float sx = (floor(coord.x) + 0.5 + sharp_x(coord.x, px_per_texel, global.crt_sharp))
               / tex_size.x;

    /* ---- vertical: the two scanlines straddling this fragment ---- */
    float y0 = floor(coord.y);
    float dy = coord.y - y0;

    vec3 c0 = texture(Source, vec2(sx, (y0 + 0.5) / tex_size.y)).rgb;
    vec3 c1 = texture(Source, vec2(sx, (y0 + 1.5) / tex_size.y)).rgb;

    /* Work in linear light. Compositing scanlines and a mask in sRGB is what produces the
       muddy grey that makes people turn CRT shaders back off. */
    c0 = pow(clamp(c0, 0.0, 1.0), vec3(global.crt_gamma_in));
    c1 = pow(clamp(c1, 0.0, 1.0), vec3(global.crt_gamma_in));

    vec3 scanned = c0 * beam(dy, c0) + c1 * beam(1.0 - dy, c1);
    vec3 flat_mix = mix(c0, c1, dy);
    vec3 color = mix(flat_mix, scanned, clamp(global.crt_scanline, 0.0, 1.0));

    /* ---- mask, in output-pixel space ---- */
    color *= phosphor(gl_FragCoord.xy, global.crt_mask_type, global.crt_mask);

    /* Scanlines and the mask both throw light away; this puts it back. Applied in linear
       light and before the output gamma, so it brightens without washing out black. */
    color *= global.crt_bright;

    /* ---- vignette and rounded corners ---- */
    if (global.crt_vignette > 0.001) {
        vec2 v = vTexCoord * (1.0 - vTexCoord) * 15.0;
        color *= mix(1.0, clamp(pow(v.x * v.y, 0.25), 0.0, 1.0), global.crt_vignette);
    }
    if (global.crt_corner > 0.0001) {
        vec2 d = abs(uv - 0.5) - (0.5 - global.crt_corner);
        float r = length(max(d, 0.0)) / max(global.crt_corner, 0.0001);
        inside *= 1.0 - smoothstep(0.9, 1.0, r);
    }

    color = pow(max(color, 0.0), vec3(1.0 / global.crt_gamma_out));
    FragColor = vec4(color * inside, 1.0);
}
