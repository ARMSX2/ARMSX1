#pragma once
// Frame generation: motion-compensated in-between pictures for every display path.
//
// One algorithm, three implementations that must stay in step:
//   * the GLSL below, compiled to SPIR-V for the Vulkan display (frame_generation_vk.h) and run as
//     GLES 3.1 compute for the OpenGL display (frame_generation_gl.h);
//   * CpuFrameGen below, for the software displays and as the reference the GPU versions are
//     tested against (psxlab/tools/framegen).
//
// Per new game frame:
//   1. luma   - the frame is averaged down to about 240 lines (`s` source pixels per luma pixel),
//               luma = (c0 + 2*c1 + c2) / 4, which is the same for RGBA and BGRA pixels.
//   2. coarse - luma averaged 4x4 again (~80x60).
//   3. coarse motion - one vector per 4x4 coarse pixels, full search +-8 coarse pixels (+-32 luma).
//   4. fine motion   - one vector per 8x8 luma pixels: the zero vector and the coarse vectors of the
//               cell and its four neighbours, each refined +-2; checkerboard SAD. The summed block
//               error also decides scene cuts.
// Per in-between picture at time t (0 = previous frame, 1 = newest):
//   5. interpolate - per output pixel, the four nearest block vectors and zero are tried; the one
//               whose previous/newest samples agree best is used to fetch both frames along the
//               motion path. Where even the best disagrees (something appearing or disappearing)
//               the nearer frame is used instead of a blend, so there are no ghosts; after a scene
//               cut the nearer frame is shown unchanged.
//
// Vectors point from the previous frame to the newest: content at newest-frame position p came
// from p - v, so at time t it is at prev(q - t v) and newest(q + (1 - t) v).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace psxfg {

constexpr int kCoarseRadius = 8;     // coarse pixels
constexpr int kRefine = 2;           // luma pixels
constexpr uint32_t kCutMilli = 90;   // mean block error (x1000) above which the frames are unrelated
constexpr float kAgreeLow = 0.04f, kAgreeHigh = 0.12f;

struct Dims {
    int w = 0, h = 0, s = 1;
    int mw = 0, mh = 0;    // luma
    int cw = 0, ch = 0;    // coarse luma
    int cbw = 0, cbh = 0;  // coarse motion blocks
    int fbw = 0, fbh = 0;  // fine motion blocks
    bool operator==(const Dims& o) const { return w == o.w && h == o.h; }
};
inline int divUp(int a, int b) { return (a + b - 1) / b; }
inline Dims dims(int w, int h) {
    Dims d;
    d.w = w; d.h = h; d.s = std::max(1, (h + 120) / 240);
    d.mw = divUp(w, d.s); d.mh = divUp(h, d.s);
    d.cw = divUp(d.mw, 4); d.ch = divUp(d.mh, 4);
    d.cbw = divUp(d.cw, 4); d.cbh = divUp(d.ch, 4);
    d.fbw = divUp(d.mw, 8); d.fbh = divUp(d.mh, 8);
    return d;
}

// ---- GLSL -------------------------------------------------------------------------------------
// Parameters are two ivec4 and one vec4: push constants on Vulkan, plain uniforms on GLES.
// BIND(vulkanBinding, glesUnit): Vulkan uses one descriptor set for every pass; GLES 3.1 only
// promises four image units per shader, so each pass numbers its own units from zero.
// psxlab/tools/framegen/build_shaders.py extracts the R"GLSL(...)GLSL" strings to build SPIR-V.
constexpr const char* kVulkanPrelude = R"GLSL(#version 450
#define BIND(vk, gl) set = 0, binding = vk
layout(push_constant) uniform Params { ivec4 i0; ivec4 i1; vec4 f0; } params;
#define I0 params.i0
#define I1 params.i1
#define F0 params.f0
)GLSL";
constexpr const char* kGlesPrelude = R"GLSL(#version 310 es
#define BIND(vk, gl) binding = gl
precision highp float;
precision highp int;
precision highp image2D;
precision highp sampler2D;
uniform highp ivec4 I0;
uniform highp ivec4 I1;
uniform highp vec4 F0;
)GLSL";
constexpr const char* kCommon = R"GLSL(
float lumaOf(vec4 c) { return (c.r + 2.0 * c.g + c.b) * 0.25; }
)GLSL";

// I0 = (w, h, s, 0), I1 = (mw, mh, 0, 0)
constexpr const char* kLumaShader = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(BIND(1, 0)) uniform highp sampler2D currColor;
layout(BIND(3, 0), r32f) uniform writeonly highp image2D lumaCurr;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= I1.x || p.y >= I1.y) return;
    int s = I0.z;
    ivec2 last = ivec2(I0.x - 1, I0.y - 1);
    float sum = 0.0;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
            sum += lumaOf(texelFetch(currColor, min(p * s + ivec2(x, y), last), 0));
    imageStore(lumaCurr, p, vec4(sum / float(s * s)));
}
)GLSL";

// I0 = (mw, mh, 0, 0), I1 = (cw, ch, 0, 0)
constexpr const char* kCoarseShader = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(BIND(3, 0), r32f) uniform readonly highp image2D lumaCurr;
layout(BIND(5, 1), r32f) uniform writeonly highp image2D coarseCurr;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= I1.x || p.y >= I1.y) return;
    ivec2 last = ivec2(I0.x - 1, I0.y - 1);
    float sum = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            sum += imageLoad(lumaCurr, min(p * 4 + ivec2(x, y), last)).r;
    imageStore(coarseCurr, p, vec4(sum / 16.0));
}
)GLSL";

// I0 = (cw, ch, 0, 0), I1 = (cbw, cbh, 0, 0); writes (vector in luma pixels, cost, 0)
constexpr const char* kCoarseMotionShader = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(BIND(4, 0), r32f) uniform readonly highp image2D coarsePrev;
layout(BIND(5, 1), r32f) uniform readonly highp image2D coarseCurr;
layout(BIND(6, 2), rgba32f) uniform writeonly highp image2D coarseMv;
const int RADIUS = 8;
void main() {
    ivec2 b = ivec2(gl_GlobalInvocationID.xy);
    if (b.x >= I1.x || b.y >= I1.y) return;
    ivec2 last = ivec2(I0.x - 1, I0.y - 1);
    ivec2 base = b * 4;
    float block[16];
    for (int i = 0; i < 16; i++) block[i] = imageLoad(coarseCurr, min(base + ivec2(i & 3, i >> 2), last)).r;
    float best = 1e30;
    ivec2 found = ivec2(0);
    for (int vy = -RADIUS; vy <= RADIUS; vy++)
        for (int vx = -RADIUS; vx <= RADIUS; vx++) {
            float sad = 0.0;
            for (int i = 0; i < 16; i++)
                sad += abs(block[i] - imageLoad(coarsePrev, clamp(base + ivec2(i & 3, i >> 2) - ivec2(vx, vy), ivec2(0), last)).r);
            float cost = sad / 16.0 + 0.002 * float(abs(vx) + abs(vy));
            if (cost < best) { best = cost; found = ivec2(vx, vy); }
        }
    imageStore(coarseMv, b, vec4(vec2(found * 4), best, 0.0));
}
)GLSL";

// I0 = (mw, mh, fbw, fbh), I1 = (cbw, cbh, 0, 0); writes (vector in luma pixels, cost, 0)
constexpr const char* kFineMotionShader = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(BIND(2, 0), r32f) uniform readonly highp image2D lumaPrev;
layout(BIND(3, 1), r32f) uniform readonly highp image2D lumaCurr;
layout(BIND(6, 2), rgba32f) uniform readonly highp image2D coarseMv;
layout(BIND(7, 3), rgba32f) uniform writeonly highp image2D fineMv;
layout(BIND(9, 0), std430) buffer Stats { uint errorSum; uint blocks; } stats;
const int REFINE = 2;
float cost(ivec2 base, ivec2 v, ivec2 last) {
    float sad = 0.0;
    for (int y = 0; y < 8; y++)
        for (int x = (y & 1); x < 8; x += 2) {
            ivec2 p = base + ivec2(x, y);
            sad += abs(imageLoad(lumaCurr, min(p, last)).r - imageLoad(lumaPrev, clamp(p - v, ivec2(0), last)).r);
        }
    return sad / 32.0 + 0.001 * float(abs(v.x) + abs(v.y));
}
ivec2 coarseAt(ivec2 cell, ivec2 cellLast) { return ivec2(imageLoad(coarseMv, clamp(cell, ivec2(0), cellLast)).xy); }
void main() {
    ivec2 b = ivec2(gl_GlobalInvocationID.xy);
    if (b.x >= I0.z || b.y >= I0.w) return;
    ivec2 last = ivec2(I0.x - 1, I0.y - 1), cell = b / 2, cellLast = ivec2(I1.x - 1, I1.y - 1);
    ivec2 candidates[6];
    candidates[0] = ivec2(0);
    candidates[1] = coarseAt(cell, cellLast);
    candidates[2] = coarseAt(cell + ivec2(-1, 0), cellLast);
    candidates[3] = coarseAt(cell + ivec2(1, 0), cellLast);
    candidates[4] = coarseAt(cell + ivec2(0, -1), cellLast);
    candidates[5] = coarseAt(cell + ivec2(0, 1), cellLast);
    float best = 1e30;
    ivec2 found = ivec2(0);
    for (int c = 0; c < 6; c++) {
        bool seen = false;
        for (int k = 0; k < c; k++) if (candidates[k] == candidates[c]) seen = true;
        if (seen) continue;
        for (int dy = -REFINE; dy <= REFINE; dy++)
            for (int dx = -REFINE; dx <= REFINE; dx++) {
                ivec2 v = candidates[c] + ivec2(dx, dy);
                float e = cost(b * 8, v, last);
                if (e < best) { best = e; found = v; }
            }
    }
    imageStore(fineMv, b, vec4(vec2(found), best, 0.0));
    atomicAdd(stats.errorSum, uint(best * 1000.0 + 0.5));
    atomicAdd(stats.blocks, 1u);
}
)GLSL";

// I0 = (w, h, s, nearest), I1 = (fbw, fbh, 0, 0), F0 = (t, 0, 0, 0)
constexpr const char* kInterpolateShader = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(BIND(0, 0)) uniform highp sampler2D prevColor;
layout(BIND(1, 1)) uniform highp sampler2D currColor;
layout(BIND(7, 0), rgba32f) uniform readonly highp image2D fineMv;
layout(BIND(8, 1), rgba8) uniform writeonly highp image2D outColor;
layout(BIND(9, 0), std430) readonly buffer Stats { uint errorSum; uint blocks; } stats;
const uint CUT_MILLI = 90u;
vec4 samplePrev(vec2 at, ivec2 last) {
    if (I0.w != 0) return texelFetch(prevColor, clamp(ivec2(floor(at)), ivec2(0), last), 0);
    return textureLod(prevColor, at / vec2(I0.xy), 0.0);
}
vec4 sampleCurr(vec2 at, ivec2 last) {
    if (I0.w != 0) return texelFetch(currColor, clamp(ivec2(floor(at)), ivec2(0), last), 0);
    return textureLod(currColor, at / vec2(I0.xy), 0.0);
}
float disagreement(ivec2 a, ivec2 b, ivec2 last) {
    return abs(lumaOf(texelFetch(prevColor, clamp(a, ivec2(0), last), 0)) - lumaOf(texelFetch(currColor, clamp(b, ivec2(0), last), 0)));
}
void main() {
    ivec2 q = ivec2(gl_GlobalInvocationID.xy);
    if (q.x >= I0.x || q.y >= I0.y) return;
    ivec2 last = ivec2(I0.x - 1, I0.y - 1);
    float t = F0.x;
    if (stats.blocks == 0u || stats.errorSum > CUT_MILLI * stats.blocks) {
        imageStore(outColor, q, vec4((t < 0.5 ? texelFetch(prevColor, q, 0) : texelFetch(currColor, q, 0)).rgb, 1.0));
        return;
    }
    int si = I0.z;
    float s = float(si);
    vec2 centre = vec2(q) + 0.5;
    ivec2 b0 = ivec2(floor(centre / (s * 8.0) - 0.5)), cellLast = ivec2(I1.x - 1, I1.y - 1);
    vec2 candidates[5];
    candidates[0] = imageLoad(fineMv, clamp(b0, ivec2(0), cellLast)).xy;
    candidates[1] = imageLoad(fineMv, clamp(b0 + ivec2(1, 0), ivec2(0), cellLast)).xy;
    candidates[2] = imageLoad(fineMv, clamp(b0 + ivec2(0, 1), ivec2(0), cellLast)).xy;
    candidates[3] = imageLoad(fineMv, clamp(b0 + ivec2(1, 1), ivec2(0), cellLast)).xy;
    candidates[4] = vec2(0.0);
    float best = 1e30;
    vec2 found = vec2(0.0);
    for (int c = 0; c < 5; c++) {
        bool seen = false;
        for (int k = 0; k < c; k++) if (candidates[k] == candidates[c]) seen = true;
        if (seen) continue;
        vec2 v = candidates[c] * s;
        ivec2 a = ivec2(floor(centre - t * v)), b = ivec2(floor(centre + (1.0 - t) * v));
        float e = disagreement(a, b, last)
            + disagreement(a + ivec2(si, 0), b + ivec2(si, 0), last)
            + disagreement(a - ivec2(si, 0), b - ivec2(si, 0), last)
            + disagreement(a + ivec2(0, si), b + ivec2(0, si), last)
            + disagreement(a - ivec2(0, si), b - ivec2(0, si), last);
        e *= 0.2;
        if (e < best) { best = e; found = v; }
    }
    vec4 ca = samplePrev(centre - t * found, last), cb = sampleCurr(centre + (1.0 - t) * found, last);
    float weight = smoothstep(0.04, 0.12, best);
    vec4 color = mix(mix(ca, cb, t), t < 0.5 ? ca : cb, weight);
    imageStore(outColor, q, vec4(color.rgb, 1.0));
}
)GLSL";

enum Pass { LUMA, COARSE, COARSE_MOTION, FINE_MOTION, INTERPOLATE, PASS_COUNT };
inline const char* passSource(int pass) {
    switch (pass) {
        case LUMA: return kLumaShader;
        case COARSE: return kCoarseShader;
        case COARSE_MOTION: return kCoarseMotionShader;
        case FINE_MOTION: return kFineMotionShader;
        default: return kInterpolateShader;
    }
}

// ---- CPU ----------------------------------------------------------------------------------------
// Pixels are 32-bit with the three colour bytes in bits 0..23 in either order (Java ARGB or RGBA);
// output alpha is opaque.
class CpuFrameGen {
public:
    struct Vec { float x = 0, y = 0, cost = 0; };
    Dims d;
    std::vector<uint32_t> frame[2];
    std::vector<float> luma[2], coarse[2], fullLuma[2];
    std::vector<Vec> coarseMv, fineMv;
    uint32_t errorSum = 0, blocks = 0;
    int current = 0;
    bool havePrevious = false, motionValid = false;
    /** Worker threads for motion search and interpolation (the software display paths use several). */
    int threads = 1;

    /** Run body(first, last) over [0, count) split across `threads`, the calling thread included. */
    template <typename Body> void parallel(int count, const Body& body) const {
        int n = std::clamp(threads, 1, 8);
        if (n == 1 || count < 8) { body(0, count); return; }
        std::vector<std::thread> workers;
        int step = divUp(count, n);
        for (int i = 1; i < n; i++) {
            int first = i * step, last = std::min(count, first + step);
            if (first < last) workers.emplace_back([&body, first, last] { body(first, last); });
        }
        body(0, std::min(count, step));
        for (auto& worker : workers) worker.join();
    }

    static float lumaOf(uint32_t c) { return (float(c & 255) / 255.0f + 2.0f * (float((c >> 8) & 255) / 255.0f) + float((c >> 16) & 255) / 255.0f) * 0.25f; }

    /** Make `pixels` the newest frame; estimate motion against the previous one when asked. */
    void capture(const uint32_t* pixels, int w, int h, int stride, bool estimate) {
        Dims next = dims(w, h);
        bool sameSize = havePrevious && next == d;
        if (!sameSize) { d = next; for (auto& f : frame) f.assign(size_t(w) * h, 0xff000000u); havePrevious = false; }
        current ^= 1;
        auto& f = frame[current];
        for (int y = 0; y < h; y++) std::memcpy(f.data() + size_t(y) * w, pixels + size_t(y) * stride, size_t(w) * 4);
        buildLuma(current);
        motionValid = false;
        if (sameSize && estimate) { estimateMotion(); motionValid = true; }
        havePrevious = true;
    }
    bool canInterpolate() const { return motionValid; }
    const std::vector<uint32_t>& newest() const { return frame[current]; }
    const std::vector<uint32_t>& previous() const { return frame[current ^ 1]; }

    /** Rows [first, last) of the picture at time t; interpolate() splits a frame across threads. */
    void interpolateRows(float t, bool nearest, uint32_t* out, int stride, int first, int last) const {
        const auto& prev = frame[current ^ 1];
        const auto& curr = frame[current];
        const float* lp = fullLuma[current ^ 1].data();
        const float* lc = fullLuma[current].data();
        const int w = d.w, h = d.h, si = d.s;
        const float s = float(si);
        if (blocks == 0 || errorSum > kCutMilli * blocks) {
            const auto& pick = t < 0.5f ? prev : curr;
            for (int y = first; y < last; y++) for (int x = 0; x < w; x++) out[size_t(y) * stride + x] = pick[size_t(y) * w + x] | 0xff000000u;
            return;
        }
        auto clampX = [w](int x) { return x < 0 ? 0 : x >= w ? w - 1 : x; };
        auto clampY = [h](int y) { return y < 0 ? 0 : y >= h ? h - 1 : y; };
        auto disagreement = [&](int ax, int ay, int bx, int by) {
            float a = lp[size_t(clampY(ay)) * w + clampX(ax)], b = lc[size_t(clampY(by)) * w + clampX(bx)];
            return a > b ? a - b : b - a;
        };
        const float cell = s * 8.0f;
        for (int y = first; y < last; y++) {
            const float cy = float(y) + 0.5f;
            const int by = int(std::floor(cy / cell - 0.5f));
            const int row0 = std::clamp(by, 0, d.fbh - 1), row1 = std::clamp(by + 1, 0, d.fbh - 1);
            uint32_t* line = out + size_t(y) * stride;
            for (int x = 0; x < w; x++) {
                const float cx = float(x) + 0.5f;
                const int bx = int(std::floor(cx / cell - 0.5f));
                const int col0 = std::clamp(bx, 0, d.fbw - 1), col1 = std::clamp(bx + 1, 0, d.fbw - 1);
                const Vec* v[4] = {&fineMv[size_t(row0) * d.fbw + col0], &fineMv[size_t(row0) * d.fbw + col1],
                                   &fineMv[size_t(row1) * d.fbw + col0], &fineMv[size_t(row1) * d.fbw + col1]};
                float cand[5][2] = {{v[0]->x, v[0]->y}, {v[1]->x, v[1]->y}, {v[2]->x, v[2]->y}, {v[3]->x, v[3]->y}, {0, 0}};
                float best = 1e30f, fx = 0, fy = 0;
                for (int c = 0; c < 5; c++) {
                    bool seen = false;
                    for (int k = 0; k < c && !seen; k++) seen = cand[k][0] == cand[c][0] && cand[k][1] == cand[c][1];
                    if (seen) continue;
                    const float vx = cand[c][0] * s, vy = cand[c][1] * s;
                    const int ax = int(std::floor(cx - t * vx)), ay = int(std::floor(cy - t * vy));
                    const int qx = int(std::floor(cx + (1.0f - t) * vx)), qy = int(std::floor(cy + (1.0f - t) * vy));
                    float e = disagreement(ax, ay, qx, qy) + disagreement(ax + si, ay, qx + si, qy) + disagreement(ax - si, ay, qx - si, qy)
                        + disagreement(ax, ay + si, qx, qy + si) + disagreement(ax, ay - si, qx, qy - si);
                    e *= 0.2f;
                    if (e < best) { best = e; fx = vx; fy = vy; }
                }
                float ca[3], cb[3];
                sample(prev, cx - t * fx, cy - t * fy, nearest, ca);
                sample(curr, cx + (1.0f - t) * fx, cy + (1.0f - t) * fy, nearest, cb);
                const float u = std::clamp((best - kAgreeLow) / (kAgreeHigh - kAgreeLow), 0.0f, 1.0f);
                const float weight = u * u * (3.0f - 2.0f * u);
                uint32_t pixel = 0xff000000u;
                for (int k = 0; k < 3; k++) {
                    const float blend = ca[k] + (cb[k] - ca[k]) * t, nearer = t < 0.5f ? ca[k] : cb[k];
                    const float value = (blend + (nearer - blend) * weight) * 255.0f + 0.5f;
                    pixel |= uint32_t(value <= 0.0f ? 0 : value >= 255.0f ? 255 : int(value)) << (8 * k);
                }
                line[x] = pixel;
            }
        }
    }
    void interpolate(float t, bool nearest, uint32_t* out, int stride) const {
        parallel(d.h, [&](int first, int last) { interpolateRows(t, nearest, out, stride, first, last); });
    }

private:
    void sample(const std::vector<uint32_t>& f, float px, float py, bool nearest, float* rgb) const {
        const int w = d.w, h = d.h;
        auto texel = [&](int x, int y) { return f[size_t(y < 0 ? 0 : y >= h ? h - 1 : y) * w + (x < 0 ? 0 : x >= w ? w - 1 : x)]; };
        if (nearest) {
            uint32_t c = texel(int(std::floor(px)), int(std::floor(py)));
            for (int k = 0; k < 3; k++) rgb[k] = float((c >> (8 * k)) & 255) / 255.0f;
            return;
        }
        float sx = px - 0.5f, sy = py - 0.5f;
        int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        float ax = sx - float(x0), ay = sy - float(y0);
        uint32_t c00 = texel(x0, y0), c10 = texel(x0 + 1, y0), c01 = texel(x0, y0 + 1), c11 = texel(x0 + 1, y0 + 1);
        for (int k = 0; k < 3; k++) {
            const int shift = 8 * k;
            const float p00 = float((c00 >> shift) & 255), p10 = float((c10 >> shift) & 255), p01 = float((c01 >> shift) & 255), p11 = float((c11 >> shift) & 255);
            const float top = p00 + (p10 - p00) * ax, bottom = p01 + (p11 - p01) * ax;
            rgb[k] = (top + (bottom - top) * ay) * (1.0f / 255.0f);
        }
    }
    void buildLuma(int index) {
        const auto& f = frame[index];
        auto& full = fullLuma[index];
        full.resize(f.size());
        for (size_t i = 0; i < f.size(); i++) full[i] = lumaOf(f[i]);
        auto& l = luma[index];
        auto& c = coarse[index];
        l.assign(size_t(d.mw) * d.mh, 0);
        c.assign(size_t(d.cw) * d.ch, 0);
        for (int y = 0; y < d.mh; y++)
            for (int x = 0; x < d.mw; x++) {
                float sum = 0;
                for (int j = 0; j < d.s; j++)
                    for (int i = 0; i < d.s; i++)
                        sum += full[size_t(std::min(y * d.s + j, d.h - 1)) * d.w + std::min(x * d.s + i, d.w - 1)];
                l[size_t(y) * d.mw + x] = sum / float(d.s * d.s);
            }
        for (int y = 0; y < d.ch; y++)
            for (int x = 0; x < d.cw; x++) {
                float sum = 0;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++)
                        sum += l[size_t(std::min(y * 4 + j, d.mh - 1)) * d.mw + std::min(x * 4 + i, d.mw - 1)];
                c[size_t(y) * d.cw + x] = sum / 16.0f;
            }
    }
    void estimateMotion() {
        const auto& cp = coarse[current ^ 1];
        const auto& cc = coarse[current];
        const auto& lp = luma[current ^ 1];
        const auto& lc = luma[current];
        coarseMv.assign(size_t(d.cbw) * d.cbh, Vec{});
        parallel(d.cbh, [&](int firstRow, int lastRow) {
        for (int by = firstRow; by < lastRow; by++)
            for (int bx = 0; bx < d.cbw; bx++) {
                float block[16];
                for (int i = 0; i < 16; i++) block[i] = cc[size_t(std::min(by * 4 + (i >> 2), d.ch - 1)) * d.cw + std::min(bx * 4 + (i & 3), d.cw - 1)];
                float best = 1e30f; int fx = 0, fy = 0;
                for (int vy = -kCoarseRadius; vy <= kCoarseRadius; vy++)
                    for (int vx = -kCoarseRadius; vx <= kCoarseRadius; vx++) {
                        float sad = 0;
                        for (int i = 0; i < 16; i++) {
                            int px = std::clamp(bx * 4 + (i & 3) - vx, 0, d.cw - 1), py = std::clamp(by * 4 + (i >> 2) - vy, 0, d.ch - 1);
                            sad += std::fabs(block[i] - cp[size_t(py) * d.cw + px]);
                        }
                        float cost = sad / 16.0f + 0.002f * float(std::abs(vx) + std::abs(vy));
                        if (cost < best) { best = cost; fx = vx; fy = vy; }
                    }
                coarseMv[size_t(by) * d.cbw + bx] = Vec{float(fx * 4), float(fy * 4), best};
            }
        });
        fineMv.assign(size_t(d.fbw) * d.fbh, Vec{});
        errorSum = 0; blocks = 0;
        auto coarseAt = [&](int cx, int cy, int* out) {
            const Vec& v = coarseMv[size_t(std::clamp(cy, 0, d.cbh - 1)) * d.cbw + std::clamp(cx, 0, d.cbw - 1)];
            out[0] = int(v.x); out[1] = int(v.y);
        };
        auto cost = [&](int baseX, int baseY, int vx, int vy) {
            float sad = 0;
            for (int y = 0; y < 8; y++)
                for (int x = (y & 1); x < 8; x += 2) {
                    int px = baseX + x, py = baseY + y;
                    float a = lc[size_t(std::min(py, d.mh - 1)) * d.mw + std::min(px, d.mw - 1)];
                    float b = lp[size_t(std::clamp(py - vy, 0, d.mh - 1)) * d.mw + std::clamp(px - vx, 0, d.mw - 1)];
                    sad += std::fabs(a - b);
                }
            return sad / 32.0f + 0.001f * float(std::abs(vx) + std::abs(vy));
        };
        parallel(d.fbh, [&](int firstRow, int lastRow) {
        for (int by = firstRow; by < lastRow; by++)
            for (int bx = 0; bx < d.fbw; bx++) {
                int cand[6][2] = {{0, 0}};
                int cx = bx / 2, cy = by / 2;
                coarseAt(cx, cy, cand[1]); coarseAt(cx - 1, cy, cand[2]); coarseAt(cx + 1, cy, cand[3]);
                coarseAt(cx, cy - 1, cand[4]); coarseAt(cx, cy + 1, cand[5]);
                float best = 1e30f; int fx = 0, fy = 0;
                for (int c = 0; c < 6; c++) {
                    bool seen = false;
                    for (int k = 0; k < c; k++) if (cand[k][0] == cand[c][0] && cand[k][1] == cand[c][1]) seen = true;
                    if (seen) continue;
                    for (int dy = -kRefine; dy <= kRefine; dy++)
                        for (int dx = -kRefine; dx <= kRefine; dx++) {
                            int vx = cand[c][0] + dx, vy = cand[c][1] + dy;
                            float e = cost(bx * 8, by * 8, vx, vy);
                            if (e < best) { best = e; fx = vx; fy = vy; }
                        }
                }
                fineMv[size_t(by) * d.fbw + bx] = Vec{float(fx), float(fy), best};
            }
        });
        for (const Vec& v : fineMv) { errorSum += uint32_t(v.cost * 1000.0f + 0.5f); blocks++; }
    }
};

}  // namespace psxfg
