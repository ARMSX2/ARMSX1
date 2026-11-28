#pragma once
// GLES 3.1 frame generation (see frame_generation.h): the same GLSL as the Vulkan display,
// compiled at run time. Owned by the OpenGL display (hardware_gpu.cpp) on its context.
//
// Capture draws the frame into history with the display's own presentation shader: the caller
// binds beginCapture()'s framebuffer, draws exactly what it would draw to the window, then calls
// endCapture(). The history therefore holds window-orientation pictures and draw() shows one
// without any flip. Every call leaves framebuffer 0 bound; callers rebind their buffers and image
// units before their next draw, as hardware_gpu.cpp already does.
#include <GLES3/gl31.h>
#include <stdexcept>
#include <string>
#include <initializer_list>
#include "frame_generation.h"

namespace psxfg {

class GlFrameGen {
public:
    ~GlFrameGen() { release(); }
    /** Compile on the current context. Throws when GLES 3.1 compute is unusable. */
    void init() {
        if (programs[0]) return;
        try {
            for (int pass = 0; pass < PASS_COUNT; pass++) {
                std::string source = std::string(kGlesPrelude) + kCommon + passSource(pass);
                programs[pass] = link({compile(GL_COMPUTE_SHADER, source.c_str())});
                i0[pass] = glGetUniformLocation(programs[pass], "I0");
                i1[pass] = glGetUniformLocation(programs[pass], "I1");
                f0[pass] = glGetUniformLocation(programs[pass], "F0");
            }
            blit = link({compile(GL_VERTEX_SHADER, kBlitVertex), compile(GL_FRAGMENT_SHADER, kBlitFragment)});
            blitTexture = glGetUniformLocation(blit, "picture");
            glGenVertexArrays(1, &vao);
        } catch (...) { release(); throw; }
    }
    void release() {
        dropTextures();
        for (auto& p : programs) { if (p) glDeleteProgram(p); p = 0; }
        if (blit) glDeleteProgram(blit);
        if (vao) glDeleteVertexArrays(1, &vao);
        blit = 0; vao = 0;
    }
    void resize(int w, int h) {
        Dims next = dims(w, h);
        if (ready && next == d) return;
        dropTextures();
        d = next;
        for (int i = 0; i < 2; i++) {
            color[i] = texture(GL_RGBA8, d.w, d.h, true);
            luma[i] = texture(GL_R32F, d.mw, d.mh, false);
            coarse[i] = texture(GL_R32F, d.cw, d.ch, false);
            glGenFramebuffers(1, &framebuffer[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color[i], 0);
            bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (!complete) { dropTextures(); throw std::runtime_error("Frame generation framebuffer incomplete"); }
        }
        coarseMv = texture(GL_RGBA32F, d.cbw, d.cbh, false);
        fineMv = texture(GL_RGBA32F, d.fbw, d.fbh, false);
        out = texture(GL_RGBA8, d.w, d.h, true);
        glGenBuffers(1, &stats);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, stats);
        uint32_t zero[2] = {0, 0};
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof zero, zero, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        ready = true; havePrevious = false; motionValid = false;
    }
    bool sized(int w, int h) const { return ready && d.w == w && d.h == h; }
    void reset() { havePrevious = false; motionValid = false; }
    int width() const { return d.w; }
    int height() const { return d.h; }

    /** Bind the framebuffer the next frame is drawn into, with a viewport covering it. */
    void beginCapture() {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer[current ^ 1]);
        glViewport(0, 0, d.w, d.h);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
    }
    void endCapture(bool estimate) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        current ^= 1;
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        int prev = current ^ 1;
        glActiveTexture(GL_TEXTURE0);  // the luma pass reads its frame from unit 0
        glBindTexture(GL_TEXTURE_2D, color[current]);
        run(LUMA, {d.w, d.h, d.s, 0}, {d.mw, d.mh, 0, 0}, 0, d.mw, d.mh, {{luma[current], GL_WRITE_ONLY, GL_R32F}});
        run(COARSE, {d.mw, d.mh, 0, 0}, {d.cw, d.ch, 0, 0}, 0, d.cw, d.ch, {{luma[current], GL_READ_ONLY, GL_R32F}, {coarse[current], GL_WRITE_ONLY, GL_R32F}});
        motionValid = false;
        if (estimate && havePrevious) {
            run(COARSE_MOTION, {d.cw, d.ch, 0, 0}, {d.cbw, d.cbh, 0, 0}, 0, d.cbw, d.cbh,
                {{coarse[prev], GL_READ_ONLY, GL_R32F}, {coarse[current], GL_READ_ONLY, GL_R32F}, {coarseMv, GL_WRITE_ONLY, GL_RGBA32F}});
            uint32_t zero[2] = {0, 0};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, stats);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof zero, zero);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, stats);
            run(FINE_MOTION, {d.mw, d.mh, d.fbw, d.fbh}, {d.cbw, d.cbh, 0, 0}, 0, d.fbw, d.fbh,
                {{luma[prev], GL_READ_ONLY, GL_R32F}, {luma[current], GL_READ_ONLY, GL_R32F}, {coarseMv, GL_READ_ONLY, GL_RGBA32F}, {fineMv, GL_WRITE_ONLY, GL_RGBA32F}});
            motionValid = true;
        }
        havePrevious = true;
        glActiveTexture(GL_TEXTURE0);
    }
    bool canInterpolate() const { return motionValid; }
    void interpolate(float t, bool nearest) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, color[current ^ 1]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, color[current]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, stats);
        run(INTERPOLATE, {d.w, d.h, d.s, nearest ? 1 : 0}, {d.fbw, d.fbh, 0, 0}, t, d.w, d.h,
            {{fineMv, GL_READ_ONLY, GL_RGBA32F}, {out, GL_WRITE_ONLY, GL_RGBA8}});
        glActiveTexture(GL_TEXTURE0);
    }
    GLuint output() const { return out; }
    GLuint newest() const { return color[current]; }
    /** Draw `picture` (output() or newest()) over the current viewport. */
    void draw(GLuint picture, bool filter) {
        glUseProgram(blit);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, picture);
        GLint mode = filter ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
        glUniform1i(blitTexture, 0);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        // Interpolation samples these with the sampler state it expects.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

private:
    static constexpr const char* kBlitVertex = R"GLSL(#version 310 es
out highp vec2 uv;
void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0, 1); uv = p; }
)GLSL";
    static constexpr const char* kBlitFragment = R"GLSL(#version 310 es
precision highp float;
in highp vec2 uv;
uniform sampler2D picture;
out vec4 color;
void main() { color = vec4(texture(picture, uv).rgb, 1.0); }
)GLSL";
    struct Binding { GLuint texture; GLenum access; GLenum format; };
    GLuint programs[PASS_COUNT] = {};
    GLint i0[PASS_COUNT] = {}, i1[PASS_COUNT] = {}, f0[PASS_COUNT] = {};
    GLuint blit = 0, vao = 0;
    GLint blitTexture = -1;
    GLuint color[2] = {}, luma[2] = {}, coarse[2] = {}, framebuffer[2] = {}, coarseMv = 0, fineMv = 0, out = 0, stats = 0;
    Dims d;
    int current = 0;
    bool ready = false, havePrevious = false, motionValid = false;

    static GLuint compile(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(shader, sizeof log, nullptr, log);
            glDeleteShader(shader);
            throw std::runtime_error(std::string("Frame generation shader: ") + log);
        }
        return shader;
    }
    static GLuint link(std::initializer_list<GLuint> shaders) {
        GLuint program = glCreateProgram();
        for (GLuint s : shaders) glAttachShader(program, s);
        glLinkProgram(program);
        for (GLuint s : shaders) glDeleteShader(s);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetProgramInfoLog(program, sizeof log, nullptr, log);
            glDeleteProgram(program);
            throw std::runtime_error(std::string("Frame generation program: ") + log);
        }
        return program;
    }
    static GLuint texture(GLenum format, int w, int h, bool sampled) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, format, w, h);
        GLint filter = sampled ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        return t;
    }
    void dropTextures() {
        GLuint textures[] = {color[0], color[1], luma[0], luma[1], coarse[0], coarse[1], coarseMv, fineMv, out};
        for (GLuint t : textures) if (t) glDeleteTextures(1, &t);
        for (GLuint& f : framebuffer) { if (f) glDeleteFramebuffers(1, &f); f = 0; }
        if (stats) glDeleteBuffers(1, &stats);
        color[0] = color[1] = luma[0] = luma[1] = coarse[0] = coarse[1] = coarseMv = fineMv = out = stats = 0;
        ready = false; havePrevious = false; motionValid = false;
    }
    void run(int pass, std::initializer_list<int> a, std::initializer_list<int> b, float t, int w, int h, std::initializer_list<Binding> images) {
        glUseProgram(programs[pass]);
        int x[4] = {}, y[4] = {}, k = 0;
        for (int v : a) x[k++] = v;
        k = 0; for (int v : b) y[k++] = v;
        glUniform4i(i0[pass], x[0], x[1], x[2], x[3]);
        glUniform4i(i1[pass], y[0], y[1], y[2], y[3]);
        if (f0[pass] >= 0) glUniform4f(f0[pass], t, 0, 0, 0);
        GLuint unit = 0;
        for (const Binding& image : images) glBindImageTexture(unit++, image.texture, 0, GL_FALSE, 0, image.access, image.format);
        glDispatchCompute(GLuint(divUp(w, 8)), GLuint(divUp(h, 8)), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    }
};

}  // namespace psxfg
