// Desktop test for the GLES 3.1 frame generation path, run through ANGLE (for example the copy in
// the Android SDK emulator's lib64/gles_angle). Compares GlFrameGen with CpuFrameGen.
//
//   g++ -O2 -std=c++17 -I<ndk>/sysroot/usr/include -Ipsxlab/app/src/main/cpp \
//       psxlab/tools/framegen/framegen_gles_test.cpp <angle>/libEGL.dll <angle>/libGLESv2.dll -o framegen_gles_test
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include "frame_generation_gl.h"

using psxfg::CpuFrameGen;
static uint32_t rgb(int r, int g, int b) { return 0xff000000u | uint32_t(std::clamp(r, 0, 255)) | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(b, 0, 255)) << 16; }
static uint32_t background(float x, float y) {
    return rgb(int(128 + 60 * std::sin(x * 0.11f) + 40 * std::cos(y * 0.07f + x * 0.03f)), int(110 + 70 * std::sin(y * 0.13f + 1.0f)),
               int(90 + 50 * std::cos((x + y) * 0.05f) + 30 * std::sin(x * 0.31f)));
}
static uint32_t scene(int x, int y, float time, float scale) {
    float px = x / scale, py = y / scale, sx = 100 + 14 * time, sy = 80 + 6 * time;
    if (px >= sx && px < sx + 60 && py >= sy && py < sy + 48) return ((int(std::floor((px - sx) / 6)) + int(std::floor((py - sy) / 6))) & 1) ? rgb(240, 40, 40) : rgb(30, 30, 220);
    return background(px + 10 * time, py + 3 * time);
}
static int failures = 0;
static void expect(bool ok, const char* what, double value) { std::printf("  %s %s (%.3f)\n", ok ? "ok  " : "FAIL", what, value); if (!ok) failures++; }

int main() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, nullptr, nullptr)) { std::printf("no EGL\n"); return 2; }
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40 /*EGL_OPENGL_ES3_BIT*/, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint count = 0;
    if (!eglChooseConfig(display, attrs, &config, 1, &count) || count < 1) { std::printf("no config\n"); return 2; }
    const EGLint pbufferAttrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    const EGLint contextAttrs[] = {0x3098 /*MAJOR*/, 3, 0x30FB /*MINOR*/, 1, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) { std::printf("no ES 3.1 context\n"); return 2; }
    std::printf("GLES: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    psxfg::GlFrameGen gen;
    gen.init();
    for (float scale : {1.0f, 2.0f}) {
        int w = int(320 * scale), h = int(240 * scale);
        std::printf("sprite + pan %dx%d\n", w, h);
        std::vector<uint32_t> frames[2];
        for (int f = 0; f < 2; f++) { frames[f].resize(size_t(w) * h); for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) frames[f][size_t(y) * w + x] = scene(x, y, float(f), scale); }
        CpuFrameGen cpu;
        cpu.capture(frames[0].data(), w, h, w, false);
        cpu.capture(frames[1].data(), w, h, w, true);
        gen.resize(w, h);
        gen.reset();
        GLuint input = 0;
        glGenTextures(1, &input);
        glBindTexture(GL_TEXTURE_2D, input);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        for (int f = 0; f < 2; f++) {
            glBindTexture(GL_TEXTURE_2D, input);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, frames[f].data());
            gen.beginCapture();
            gen.draw(input, false);
            gen.endCapture(f == 1);
        }
        expect(gen.canInterpolate(), "motion estimated", 0);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        for (float t : {0.5f, 0.25f, 0.75f}) {
            gen.interpolate(t, false);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gen.output(), 0);
            std::vector<uint32_t> g(size_t(w) * h);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            std::vector<uint32_t> c(size_t(w) * h);
            cpu.interpolate(t, false, c.data(), w);
            double sum = 0; long offCount = 0;
            for (size_t i = 0; i < g.size(); i++) { bool off = false; for (int k = 0; k < 3; k++) { int e = std::abs(int((g[i] >> (8 * k)) & 255) - int((c[i] >> (8 * k)) & 255)); sum += e; off |= e > 8; } offCount += off; }
            char what[128]; std::snprintf(what, sizeof what, "t=%.2f gles matches cpu (mean, %.3f%% pixels off by >8)", t, 100.0 * offCount / g.size());
            expect(sum / (g.size() * 3.0) < 1.0 && offCount * 1000 < long(g.size()) * 5, what, sum / (g.size() * 3.0));
        }
        GLenum error = glGetError();
        expect(error == GL_NO_ERROR, "no GL errors", double(error));
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &input);
    }
    gen.release();
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
