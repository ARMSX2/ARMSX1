/*
    ARMSX — GLES 3.0 / GL 3.3-core presentation backend.

    WHAT THIS IS
    ------------
    A real GPU present path for the software-rasterized PlayStation framebuffer. The PS1 GPU
    (psx/dev/gpu.c) is untouched and stays authoritative; this file only uploads its output
    into a texture and draws one full-screen quad.

    WHY IT IS NOT JUST glTexImage2D + a passthrough shader
    ------------------------------------------------------
    The core hands us one of two formats:

      * SDL_PIXELFORMAT_BGR555 — packed 16-bit, bit layout [15]=mask [14:10]=B [9:5]=G
        [4:0]=R. GLES 3.0 has no GL_UNSIGNED_SHORT_1_5_5_5_REV (that is desktop-GL only), so
        there is no native upload format for it. We upload the raw 16 bits as a two-channel
        GL_RG8 texture (x = low byte, y = high byte, little-endian) and unpack in the
        fragment shader. Because the texels are then *bytes*, hardware bilinear filtering
        would blend the packed representation rather than the colour, so the bilinear
        variant does its own 4-tap texelFetch + decode + lerp. Nearest is exact.

      * SDL_PIXELFORMAT_RGB24 — 3 bytes/pixel, uploaded as GL_RGB8 and sampled normally
        (hardware nearest/linear).

    Source rows are 2048 bytes apart (PSX_GPU_FB_STRIDE) regardless of the pixel size, which
    is not expressible with GL_UNPACK_ROW_LENGTH for the 3-byte format (2048/3 is not an
    integer), so the dirty row range is repacked into a contiguous staging buffer before the
    single glTexSubImage2D. The caller's dirty-row optimisation is preserved end to end.

    CONTEXT CREATION
    ----------------
    Two providers behind one implementation:

      * SDL   — SDL_GL_CreateContext on an SDL_WINDOW_OPENGL window, SDL_GL_SwapWindow,
                SDL_GL_SetSwapInterval. Entry points come from SDL_GL_GetProcAddress, so
                there is no GL loader dependency (no glad/glew/epoxy).
      * EGL   — Android only. Binds EGL straight to the ANativeWindow the Compose host
                registered via armsx_render_set_native_window(). This exists because the
                in-process Android host runs SDL on its `dummy` video driver (SDL's Android
                backend needs org.libsdl.app.SDLActivity, which Compose does not provide),
                so SDL_GL_CreateContext is not an option there.

    Every GL entry point goes through a function-pointer table populated by whichever
    provider is active, and this file declares its own GL types/enums so it needs no GL
    headers at all on any platform.
*/

#include "render_internal.h"
#include "gpu_profile.h"

#if defined(ARMSX_ENABLE_GL)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#define ARMSX_GL_HAVE_EGL 1
#if !defined(EGL_OPENGL_ES3_BIT_KHR)
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif
#endif

namespace {

/* Thread id, for the EGL-context-affinity trace. gettid() only reached bionic's headers at
   API 30 and this builds against 26, so go through the syscall directly. */
long CurrentThreadId() {
#if defined(__ANDROID__)
    return static_cast<long>(::syscall(__NR_gettid));
#else
    return 0;
#endif
}

/* ---- minimal GL typedefs / enums (no GL headers anywhere) -------------------------- */

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef std::ptrdiff_t GLintptr;
typedef std::ptrdiff_t GLsizeiptr;

constexpr GLenum GL_FALSE_ = 0;
constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_RGB = 0x1907;
constexpr GLenum GL_RGB8 = 0x8051;
constexpr GLenum GL_RG = 0x8227;
constexpr GLenum GL_RG8 = 0x822B;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_RENDERER_STR = 0x1F01;
constexpr GLenum GL_VERSION_STR = 0x1F02;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;

struct GlApi {
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*Clear)(GLbitfield) = nullptr;
    void (*Disable)(GLenum) = nullptr;
    GLenum (*GetError)() = nullptr;
    const unsigned char* (*GetString)(GLenum) = nullptr;
    void (*PixelStorei)(GLenum, GLint) = nullptr;

    void (*GenTextures)(GLsizei, GLuint*) = nullptr;
    void (*DeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void (*BindTexture)(GLenum, GLuint) = nullptr;
    void (*ActiveTexture)(GLenum) = nullptr;
    void (*TexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;

    GLuint (*CreateShader)(GLenum) = nullptr;
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void (*CompileShader)(GLuint) = nullptr;
    void (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (*DeleteShader)(GLuint) = nullptr;

    GLuint (*CreateProgram)() = nullptr;
    void (*AttachShader)(GLuint, GLuint) = nullptr;
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar*) = nullptr;
    void (*LinkProgram)(GLuint) = nullptr;
    void (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (*UseProgram)(GLuint) = nullptr;
    void (*DeleteProgram)(GLuint) = nullptr;
    GLint (*GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void (*Uniform1i)(GLint, GLint) = nullptr;
    void (*Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;

    void (*GenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void (*BindVertexArray)(GLuint) = nullptr;
    void (*DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void (*GenBuffers)(GLsizei, GLuint*) = nullptr;
    void (*BindBuffer)(GLenum, GLuint) = nullptr;
    void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (*DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
    void (*EnableVertexAttribArray)(GLuint) = nullptr;
    void (*DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
};

typedef void* (*GlProcLoader)(const char* name, void* user);

bool LoadGlApi(GlApi& gl, GlProcLoader loader, void* user, std::string& missing) {
    struct Entry {
        const char* name;
        void** slot;
    };

    const Entry entries[] = {
        {"glViewport", (void**)&gl.Viewport},
        {"glClearColor", (void**)&gl.ClearColor},
        {"glClear", (void**)&gl.Clear},
        {"glDisable", (void**)&gl.Disable},
        {"glGetError", (void**)&gl.GetError},
        {"glGetString", (void**)&gl.GetString},
        {"glPixelStorei", (void**)&gl.PixelStorei},
        {"glGenTextures", (void**)&gl.GenTextures},
        {"glDeleteTextures", (void**)&gl.DeleteTextures},
        {"glBindTexture", (void**)&gl.BindTexture},
        {"glActiveTexture", (void**)&gl.ActiveTexture},
        {"glTexParameteri", (void**)&gl.TexParameteri},
        {"glTexImage2D", (void**)&gl.TexImage2D},
        {"glTexSubImage2D", (void**)&gl.TexSubImage2D},
        {"glCreateShader", (void**)&gl.CreateShader},
        {"glShaderSource", (void**)&gl.ShaderSource},
        {"glCompileShader", (void**)&gl.CompileShader},
        {"glGetShaderiv", (void**)&gl.GetShaderiv},
        {"glGetShaderInfoLog", (void**)&gl.GetShaderInfoLog},
        {"glDeleteShader", (void**)&gl.DeleteShader},
        {"glCreateProgram", (void**)&gl.CreateProgram},
        {"glAttachShader", (void**)&gl.AttachShader},
        {"glBindAttribLocation", (void**)&gl.BindAttribLocation},
        {"glLinkProgram", (void**)&gl.LinkProgram},
        {"glGetProgramiv", (void**)&gl.GetProgramiv},
        {"glGetProgramInfoLog", (void**)&gl.GetProgramInfoLog},
        {"glUseProgram", (void**)&gl.UseProgram},
        {"glDeleteProgram", (void**)&gl.DeleteProgram},
        {"glGetUniformLocation", (void**)&gl.GetUniformLocation},
        {"glUniform1i", (void**)&gl.Uniform1i},
        {"glUniform2f", (void**)&gl.Uniform2f},
        {"glGenVertexArrays", (void**)&gl.GenVertexArrays},
        {"glBindVertexArray", (void**)&gl.BindVertexArray},
        {"glDeleteVertexArrays", (void**)&gl.DeleteVertexArrays},
        {"glGenBuffers", (void**)&gl.GenBuffers},
        {"glBindBuffer", (void**)&gl.BindBuffer},
        {"glBufferData", (void**)&gl.BufferData},
        {"glDeleteBuffers", (void**)&gl.DeleteBuffers},
        {"glVertexAttribPointer", (void**)&gl.VertexAttribPointer},
        {"glEnableVertexAttribArray", (void**)&gl.EnableVertexAttribArray},
        {"glDrawArrays", (void**)&gl.DrawArrays},
    };

    for (const Entry& entry : entries) {
        void* address = loader(entry.name, user);
        if (!address) {
            missing = entry.name;
            return false;
        }
        *entry.slot = address;
    }

    return true;
}

/* ---- shaders ----------------------------------------------------------------------- */

/* u_uvoff/u_uvscale carry [video] overscan_crop as a sub-rectangle in normalised texture
   space; u_rot carries [video] display_rotation in quarter turns clockwise. The defaults
   (0,0)/(1,1)/0 reproduce the original one-liner exactly, and the rotation is applied to the
   SOURCE coordinate, so it is the inverse of the turn the picture makes: rotating the image
   90 degrees clockwise means dest (x,y) reads source (y, 1-x). */
const char* kVertexBody =
    "in vec2 a_pos;\n"
    "uniform vec2 u_uvoff;\n"
    "uniform vec2 u_uvscale;\n"
    "uniform int u_rot;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 b = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
    "    if (u_rot == 1)      b = vec2(b.y, 1.0 - b.x);\n"
    "    else if (u_rot == 2) b = vec2(1.0 - b.x, 1.0 - b.y);\n"
    "    else if (u_rot == 3) b = vec2(1.0 - b.y, b.x);\n"
    "    v_uv = u_uvoff + b * u_uvscale;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

const char* kFragmentRgb =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texsize;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    o_color = vec4(texture(u_tex, v_uv).rgb, 1.0);\n"
    "}\n";

/* BGR555 packed into RG8: x = low byte, y = high byte (little-endian).
   value = hi*256 + lo, layout [15]=mask [14:10]=B [9:5]=G [4:0]=R. */
const char* kDecode555 =
    "vec3 armsx_decode(vec2 rg) {\n"
    "    float lo = floor(rg.x * 255.0 + 0.5);\n"
    "    float hi = floor(rg.y * 255.0 + 0.5);\n"
    "    float r = mod(lo, 32.0);\n"
    "    float g = floor(lo / 32.0) + mod(hi, 4.0) * 8.0;\n"
    "    float b = mod(floor(hi / 4.0), 32.0);\n"
    "    return vec3(r, g, b) / 31.0;\n"
    "}\n";

const char* kFragment555Nearest =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texsize;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    o_color = vec4(armsx_decode(texture(u_tex, v_uv).rg), 1.0);\n"
    "}\n";

/* Hardware bilinear would interpolate the packed bytes, not the colour, so the four taps
   are fetched unfiltered, decoded, and blended here. */
const char* kFragment555Linear =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texsize;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    vec2 t = v_uv * u_texsize - 0.5;\n"
    "    vec2 f = fract(t);\n"
    "    ivec2 base = ivec2(floor(t));\n"
    "    ivec2 limit = ivec2(u_texsize) - ivec2(1);\n"
    "    ivec2 lo = clamp(base, ivec2(0), limit);\n"
    "    ivec2 hi = clamp(base + ivec2(1), ivec2(0), limit);\n"
    "    vec3 c00 = armsx_decode(texelFetch(u_tex, ivec2(lo.x, lo.y), 0).rg);\n"
    "    vec3 c10 = armsx_decode(texelFetch(u_tex, ivec2(hi.x, lo.y), 0).rg);\n"
    "    vec3 c01 = armsx_decode(texelFetch(u_tex, ivec2(lo.x, hi.y), 0).rg);\n"
    "    vec3 c11 = armsx_decode(texelFetch(u_tex, ivec2(hi.x, hi.y), 0).rg);\n"
    "    o_color = vec4(mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y), 1.0);\n"
    "}\n";

enum ProgramKind {
    kProgramRgb = 0,
    kProgram555Nearest = 1,
    kProgram555Linear = 2,
    kProgramCount = 3
};

/* ---- backend state ------------------------------------------------------------------ */

#if defined(ARMSX_GL_HAVE_EGL)
/*
    Every EGL entry point goes through this table rather than the linked libEGL, so the whole
    provider can be swapped for ANGLE's (libEGL_angle.so) at runtime. There is no #ifdef and
    no second code path: "system" and "ANGLE" differ only in which library the table was
    resolved out of.
*/
struct EglApi {
    EGLDisplay (*GetDisplay)(EGLNativeDisplayType) = nullptr;
    EGLBoolean (*Initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*Terminate)(EGLDisplay) = nullptr;
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint*) = nullptr;
    EGLBoolean (*BindAPI)(EGLenum) = nullptr;
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = nullptr;
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*) = nullptr;
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*SwapInterval)(EGLDisplay, EGLint) = nullptr;
    EGLint (*GetError)() = nullptr;
    EGLContext (*GetCurrentContext)() = nullptr;
    EGLSurface (*GetCurrentSurface)(EGLint) = nullptr;
    __eglMustCastToProperFunctionPointerType (*GetProcAddress)(const char*) = nullptr;
    const char* (*QueryString)(EGLDisplay, EGLint) = nullptr;
};
#endif

struct GlRenderer {
    armsx_renderer_t base;

    GlApi gl{};
    bool es_profile = true;

    /* SDL provider */
    SDL_Window* window = nullptr;
    SDL_GLContext sdl_context = nullptr;

#if defined(ARMSX_GL_HAVE_EGL)
    /* EGL provider */
    bool use_egl = false;
    EglApi egl{};
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLContext egl_context = EGL_NO_CONTEXT;
    ANativeWindow* native_window = nullptr;
    void* egl_library = nullptr;
    void* gles_library = nullptr;
    /* What we ACTUALLY bound to, which is not always what was asked for: a missing or
       unloadable ANGLE falls back to the system driver, and that has to stay visible. */
    armsx_render_gl_driver_t active_gl_driver = ARMSX_RENDER_GL_DRIVER_SYSTEM;
#endif

    GLuint texture = 0;
    int texture_width = 0;
    int texture_height = 0;
    Uint32 texture_format = SDL_PIXELFORMAT_UNKNOWN;
    int texture_filter = -1; /* -1 unset, 0 nearest, 1 linear */

    /* A GPU-resident frame adopted from the rasterizer instead of uploaded (see
       armsx_renderer_adopt_gl_texture). Non-zero means present sources from this and the
       CPU `texture` above is untouched; the next OpUploadFrame() clears it, so the two
       sources can never both be live. */
    GLuint external_texture = 0;
    int external_width = 0;
    int external_height = 0;
    Uint32 external_format = SDL_PIXELFORMAT_UNKNOWN;

    GLuint programs[kProgramCount] = {0, 0, 0};
    GLint uniform_tex[kProgramCount] = {-1, -1, -1};
    GLint uniform_texsize[kProgramCount] = {-1, -1, -1};
    /* [video] overscan_crop + display_rotation, all on the shared vertex shader. */
    GLint uniform_uvoff[kProgramCount] = {-1, -1, -1};
    GLint uniform_uvscale[kProgramCount] = {-1, -1, -1};
    GLint uniform_rot[kProgramCount] = {-1, -1, -1};

    GLuint vao = 0;
    GLuint vbo = 0;

    std::vector<uint8_t> staging;
    std::string driver_name;

    bool has_frame = false;
    int output_width = 0;
    int output_height = 0;
    bool vsync = false;

    /* Bring-up trace. The first few frames are logged in full; after that only anomalies. */
    long context_thread = 0;
    int upload_traces = 0;
    int upload_checks = 0;
    int present_traces = 0;
    long rebinds = 0;
    long swap_failures = 0;
    bool swapped_once = false;
    /* Window-surface rebuild state (see armsx_render_native_window_generation). The config is
       kept because rebuilding needs the SAME EGLConfig the context was created with — a context
       and surface from different configs are not compatible. */
    void*         egl_config_kept = nullptr;
    unsigned long window_generation = 0;
    long          surface_rebuilds = 0;
};

GlRenderer* Self(armsx_renderer_t* base) {
    return static_cast<GlRenderer*>(base->impl);
}

/* Drains and reports the GL error queue. Every step that can fail calls this: a GLES driver
   reports nothing on its own, so a present path that quietly does nothing is indistinguishable
   from one that works and happens to draw a black frame. Returns true when the queue was
   clean. */
bool GlCheck(GlRenderer* self, const char* step) {
    if (!self->gl.GetError) {
        return true;
    }

    bool clean = true;
    for (int guard = 0; guard < 8; ++guard) {
        const GLenum error = self->gl.GetError();
        if (error == 0) {
            break;
        }
        clean = false;
        armsx_render_log("renderer", "GL error 0x%04x after %s", (unsigned)error, step);
    }
    return clean;
}

/* ---- proc loaders -------------------------------------------------------------------- */

void* SdlProcLoader(const char* name, void*) {
    return SDL_GL_GetProcAddress(name);
}

#if defined(ARMSX_GL_HAVE_EGL)
void* EglProcLoader(const char* name, void* user) {
    auto* self = static_cast<GlRenderer*>(user);
    if (!self) {
        return nullptr;
    }
    if (self->gles_library) {
        if (void* address = dlsym(self->gles_library, name)) {
            return address;
        }
    }
    if (!self->egl.GetProcAddress) {
        return nullptr;
    }
    return reinterpret_cast<void*>(self->egl.GetProcAddress(name));
}
#endif

/* ---- shader helpers ------------------------------------------------------------------- */

std::string ShaderHeader(bool es_profile) {
    if (es_profile) {
        return "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
    }
    return "#version 330 core\n";
}

GLuint CompileShader(GlRenderer* self, GLenum type, const std::string& source) {
    const char* kind = type == GL_VERTEX_SHADER ? "vertex" : "fragment";

    const GLuint shader = self->gl.CreateShader(type);
    if (!shader) {
        /* The one failure that used to leave no trace whatsoever: a zero name here aborted
           the whole present with nothing logged, which on screen is a black window. */
        armsx_render_log("renderer", "GL glCreateShader(%s) returned 0", kind);
        GlCheck(self, "glCreateShader");
        return 0;
    }

    const char* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    self->gl.ShaderSource(shader, 1, &text, &length);
    self->gl.CompileShader(shader);

    char log[1024];
    GLsizei log_length = 0;
    log[0] = '\0';
    self->gl.GetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(log)), &log_length, log);
    log[sizeof(log) - 1] = '\0';

    GLint status = 0;
    self->gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == static_cast<GLint>(GL_FALSE_)) {
        armsx_render_log("renderer", "GL %s shader compile failed: %s", kind, log);
        self->gl.DeleteShader(shader);
        return 0;
    }

    if (log_length > 0) {
        /* A shader that compiles but warns is worth seeing: it is usually the precision or
           extension mismatch behind a driver-specific black frame. */
        armsx_render_log("renderer", "GL %s shader compiled with diagnostics: %s", kind, log);
    }

    return shader;
}

bool BuildProgram(GlRenderer* self, ProgramKind kind) {
    if (self->programs[kind]) {
        return true;
    }

    const std::string header = ShaderHeader(self->es_profile);
    std::string fragment = header;
    switch (kind) {
        case kProgramRgb:
            fragment += kFragmentRgb;
            break;
        case kProgram555Nearest:
            fragment += kDecode555;
            fragment += kFragment555Nearest;
            break;
        case kProgram555Linear:
        default:
            fragment += kDecode555;
            fragment += kFragment555Linear;
            break;
    }

    const GLuint vs = CompileShader(self, GL_VERTEX_SHADER, header + kVertexBody);
    if (!vs) {
        return false;
    }
    const GLuint fs = CompileShader(self, GL_FRAGMENT_SHADER, fragment);
    if (!fs) {
        self->gl.DeleteShader(vs);
        return false;
    }

    const GLuint program = self->gl.CreateProgram();
    if (!program) {
        armsx_render_log("renderer", "GL glCreateProgram returned 0 (program kind %d)", (int)kind);
        GlCheck(self, "glCreateProgram");
        self->gl.DeleteShader(vs);
        self->gl.DeleteShader(fs);
        return false;
    }

    self->gl.AttachShader(program, vs);
    self->gl.AttachShader(program, fs);
    self->gl.BindAttribLocation(program, 0, "a_pos");
    self->gl.LinkProgram(program);
    self->gl.DeleteShader(vs);
    self->gl.DeleteShader(fs);

    char log[1024];
    GLsizei log_length = 0;
    log[0] = '\0';
    self->gl.GetProgramInfoLog(program, static_cast<GLsizei>(sizeof(log)), &log_length, log);
    log[sizeof(log) - 1] = '\0';

    GLint status = 0;
    self->gl.GetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == static_cast<GLint>(GL_FALSE_)) {
        armsx_render_log("renderer", "GL program link failed (kind %d): %s", (int)kind, log);
        self->gl.DeleteProgram(program);
        return false;
    }

    self->programs[kind] = program;
    self->uniform_tex[kind] = self->gl.GetUniformLocation(program, "u_tex");
    self->uniform_texsize[kind] = self->gl.GetUniformLocation(program, "u_texsize");
    self->uniform_uvoff[kind] = self->gl.GetUniformLocation(program, "u_uvoff");
    self->uniform_uvscale[kind] = self->gl.GetUniformLocation(program, "u_uvscale");
    self->uniform_rot[kind] = self->gl.GetUniformLocation(program, "u_rot");

    armsx_render_log("renderer", "GL program kind=%d linked id=%u u_tex=%d u_texsize=%d%s%s",
                     (int)kind, program, self->uniform_tex[kind], self->uniform_texsize[kind],
                     log_length > 0 ? " log=" : "", log_length > 0 ? log : "");
    GlCheck(self, "program link");
    return true;
}

bool CreateQuad(GlRenderer* self) {
    static const GLfloat vertices[] = {
        -1.0f, -1.0f,
        1.0f, -1.0f,
        -1.0f, 1.0f,
        1.0f, 1.0f,
    };

    self->gl.GenVertexArrays(1, &self->vao);
    self->gl.BindVertexArray(self->vao);
    self->gl.GenBuffers(1, &self->vbo);
    self->gl.BindBuffer(GL_ARRAY_BUFFER, self->vbo);
    self->gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(vertices)), vertices, GL_STATIC_DRAW);
    self->gl.EnableVertexAttribArray(0);
    self->gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, static_cast<GLsizei>(2 * sizeof(GLfloat)), nullptr);
    self->gl.BindVertexArray(0);

    const bool clean = GlCheck(self, "full-screen quad setup");
    armsx_render_log("renderer", "GL quad vao=%u vbo=%u", self->vao, self->vbo);
    return clean && self->vao != 0 && self->vbo != 0;
}

/* ---- context providers ---------------------------------------------------------------- */

/*
    An EGL context is current *per thread*, and a thread with no context current silently
    drops every GL call: no GL error, no EGL error, no log — just a black window.

    For the Android in-process host this is currently a no-op guard: NativeApp.runVMThread()
    calls external_main_ex() straight through, so the renderer is created on the same
    `armsx-vm` thread that later issues upload_frame()/present(), and the context is already
    current there. The guard is kept because that is an accident of the call graph rather than
    a contract — a renderer rebuilt from anywhere else (recreateManagedRenderer on a vsync
    change, teardown from a different thread) would otherwise fail exactly this way. The
    rebind counter keeps any such migration visible in the log instead of silent.
*/
bool EnsureContextCurrent(GlRenderer* self) {
#if defined(ARMSX_GL_HAVE_EGL)
    if (self->use_egl) {
        if (self->egl.GetCurrentContext() == self->egl_context &&
            self->egl.GetCurrentSurface(EGL_DRAW) == self->egl_surface) {
            return true;
        }

        if (self->egl.MakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                           self->egl_context) != EGL_TRUE) {
            armsx_render_log("renderer", "GL: eglMakeCurrent on thread %ld failed (0x%04x)",
                             CurrentThreadId(), self->egl.GetError());
            return false;
        }

        self->rebinds++;
        if (self->rebinds <= 4) {
            armsx_render_log("renderer",
                             "GL: EGL context re-bound to thread %ld (created on %ld, rebind #%ld)",
                             CurrentThreadId(), self->context_thread, self->rebinds);
        }
        return true;
    }
#endif
    if (self->window && self->sdl_context) {
        return SDL_GL_MakeCurrent(self->window, self->sdl_context) == 0;
    }
    return true;
}

#if defined(ARMSX_GL_HAVE_EGL)
/* Rebuild the EGL window surface when Android has handed us a new ANativeWindow.

   Called once per frame from GlSwap. The context, all GL objects and the whole upscaled VRAM
   target SURVIVE — only the window surface is recreated — so resuming costs nothing beyond one
   eglCreateWindowSurface and the frame it happens on. Returns false if there is no window to
   present to (app fully backgrounded), which tells the caller to skip the swap rather than
   fail it five times and give up. */
bool GlEnsureWindowSurface(GlRenderer* self) {
    const unsigned long generation = armsx_render_native_window_generation();

    if (generation == self->window_generation && self->egl_surface != EGL_NO_SURFACE) {
        return true;
    }

    void* const window = armsx_render_native_window();

    /* Tear the old surface down even when there is no replacement yet: it refers to a window
       that no longer exists, and holding it keeps the driver's buffers alive for a dead
       compositor object. */
    if (self->egl_surface != EGL_NO_SURFACE) {
        self->egl.MakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context);
        self->egl.DestroySurface(self->egl_display, self->egl_surface);
        self->egl_surface = EGL_NO_SURFACE;
    }

    /* The reference taken in CreateEglContext() belonged to the EGLSurface just destroyed.
       Released here, and re-taken below for the replacement, so OpShutdown always releases the
       window this renderer is actually bound to. */
    if (self->native_window) {
        ANativeWindow_release(self->native_window);
        self->native_window = nullptr;
    }

    if (!window) {
        /* Do NOT consume the generation here. The next publish bumps it again, but if this null
           were recorded as "handled" and the app were killed mid-background, a resume that
           reused the same counter value would skip the rebuild. Cheap to re-check. */
        return false;
    }

    /* Re-match the window's buffer format to the config, exactly as the initial bring-up does.
       A replacement window arrives with the SurfaceView's own format, and the EGL spec requires
       the two to agree — without this the rebuild is an EGL_BAD_MATCH on any device whose
       chosen config is not the window's default, i.e. a permanent black screen after a
       task-switch rather than a recovered one. */
    {
        EGLint native_visual = 0;
        if (self->egl.GetConfigAttrib(self->egl_display, (EGLConfig)self->egl_config_kept,
                                      EGL_NATIVE_VISUAL_ID, &native_visual) == EGL_TRUE &&
            native_visual != 0) {
            ANativeWindow_setBuffersGeometry(static_cast<ANativeWindow*>(window), 0, 0,
                                             native_visual);
        }
    }

    self->egl_surface = self->egl.CreateWindowSurface(
        self->egl_display, (EGLConfig)self->egl_config_kept, (EGLNativeWindowType)window, nullptr);
    if (self->egl_surface == EGL_NO_SURFACE) {
        armsx_render_log("renderer", "GL: surface rebuild failed (0x%04x); will retry next frame",
                         self->egl.GetError());
        return false;
    }

    if (self->egl.MakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                              self->egl_context) != EGL_TRUE) {
        armsx_render_log("renderer", "GL: eglMakeCurrent after rebuild failed (0x%04x)",
                         self->egl.GetError());
        self->egl.DestroySurface(self->egl_display, self->egl_surface);
        self->egl_surface = EGL_NO_SURFACE;
        return false;
    }

    /* Hold a reference for the lifetime of the new EGLSurface, and re-raise the claim: the
       host's blit bridge must stay parked while this backend owns the window. */
    ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
    self->native_window = static_cast<ANativeWindow*>(window);
    armsx_render_set_native_window_claimed(true);

    self->window_generation = generation;
    self->swap_failures = 0;
    armsx_render_log("renderer", "GL: window surface rebuilt after resume (#%ld) window=%p",
                     ++self->surface_rebuilds, window);
    return true;
}
#endif

void GlSwap(GlRenderer* self) {
#if defined(ARMSX_GL_HAVE_EGL)
    if (self->use_egl) {
        if (!GlEnsureWindowSurface(self)) {
            return;
        }
        if (self->egl.SwapBuffers(self->egl_display, self->egl_surface) != EGL_TRUE) {
            if (++self->swap_failures <= 5) {
                armsx_render_log("renderer", "GL: eglSwapBuffers failed (0x%04x)", self->egl.GetError());
            }
        } else if (!self->swapped_once) {
            /* The line that splits "we never posted a frame" from "we posted and something
               downstream ate it". Worth one log entry. */
            self->swapped_once = true;
            armsx_render_log("renderer", "GL: first eglSwapBuffers posted on thread %ld",
                             CurrentThreadId());
        }
        return;
    }
#endif
    if (self->window) {
        SDL_GL_SwapWindow(self->window);
    }
}

void GlQueryDrawableSize(GlRenderer* self, int* width, int* height) {
    int w = 0;
    int h = 0;
#if defined(ARMSX_GL_HAVE_EGL)
    if (self->use_egl) {
        EGLint value = 0;
        if (self->egl.QuerySurface(self->egl_display, self->egl_surface, EGL_WIDTH, &value) == EGL_TRUE) {
            w = value;
        }
        value = 0;
        if (self->egl.QuerySurface(self->egl_display, self->egl_surface, EGL_HEIGHT, &value) == EGL_TRUE) {
            h = value;
        }
    } else
#endif
    if (self->window) {
        SDL_GL_GetDrawableSize(self->window, &w, &h);
    }

    if (width) {
        *width = w > 0 ? w : self->output_width;
    }
    if (height) {
        *height = h > 0 ? h : self->output_height;
    }
}

void GlSetSwapInterval(GlRenderer* self, bool enabled) {
    self->vsync = enabled;
#if defined(ARMSX_GL_HAVE_EGL)
    if (self->use_egl) {
        self->egl.SwapInterval(self->egl_display, enabled ? 1 : 0);
        return;
    }
#endif
    SDL_GL_SetSwapInterval(enabled ? 1 : 0);
}

#if defined(ARMSX_GL_HAVE_EGL)
/*
    Resolves the EGL table out of either the system driver or ANGLE.

    ANGLE ships in the APK as libEGL_angle.so + libGLESv2_angle.so, so both are in the app's
    native library dir and dlopen()able by soname. libEGL_angle.so pulls libGLESv2_angle.so
    in itself; we also dlopen it directly so GL entry points resolve by dlsym rather than
    exclusively through eglGetProcAddress.

    The system driver is always the last candidate. A device where ANGLE is missing or
    refuses to load must degrade to a working renderer, never to no renderer — but it must
    also never do so QUIETLY, which is what active_gl_driver and the log line below are for.
*/
bool LoadEglApi(GlRenderer* self, armsx_render_gl_driver_t wanted) {
    struct Provider {
        armsx_render_gl_driver_t driver;
        const char* label;
        const char* egl_library;
        const char* gles_libraries[2];
    };

    static const Provider kAngle = {ARMSX_RENDER_GL_DRIVER_ANGLE, "ANGLE", "libEGL_angle.so",
                                    {"libGLESv2_angle.so", nullptr}};
    static const Provider kSystem = {ARMSX_RENDER_GL_DRIVER_SYSTEM, "system", "libEGL.so",
                                     {"libGLESv3.so", "libGLESv2.so"}};

    const Provider* order[2] = {&kSystem, nullptr};
    if (wanted == ARMSX_RENDER_GL_DRIVER_ANGLE) {
        order[0] = &kAngle;
        order[1] = &kSystem;
    }

    for (const Provider* provider : order) {
        if (!provider) {
            break;
        }

        void* library = dlopen(provider->egl_library, RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            armsx_render_log("renderer", "EGL: dlopen(%s) failed: %s", provider->egl_library,
                             dlerror() ? dlerror() : "(no error)");
            continue;
        }

        EglApi api{};
        struct Entry {
            const char* name;
            void** slot;
        };
        const Entry entries[] = {
            {"eglGetDisplay", (void**)&api.GetDisplay},
            {"eglInitialize", (void**)&api.Initialize},
            {"eglTerminate", (void**)&api.Terminate},
            {"eglChooseConfig", (void**)&api.ChooseConfig},
            {"eglGetConfigAttrib", (void**)&api.GetConfigAttrib},
            {"eglBindAPI", (void**)&api.BindAPI},
            {"eglCreateContext", (void**)&api.CreateContext},
            {"eglDestroyContext", (void**)&api.DestroyContext},
            {"eglCreateWindowSurface", (void**)&api.CreateWindowSurface},
            {"eglDestroySurface", (void**)&api.DestroySurface},
            {"eglMakeCurrent", (void**)&api.MakeCurrent},
            {"eglQuerySurface", (void**)&api.QuerySurface},
            {"eglSwapBuffers", (void**)&api.SwapBuffers},
            {"eglSwapInterval", (void**)&api.SwapInterval},
            {"eglGetError", (void**)&api.GetError},
            {"eglGetCurrentContext", (void**)&api.GetCurrentContext},
            {"eglGetCurrentSurface", (void**)&api.GetCurrentSurface},
            {"eglGetProcAddress", (void**)&api.GetProcAddress},
            {"eglQueryString", (void**)&api.QueryString},
        };

        const char* missing = nullptr;
        for (const Entry& entry : entries) {
            void* address = dlsym(library, entry.name);
            if (!address) {
                missing = entry.name;
                break;
            }
            *entry.slot = address;
        }

        if (missing) {
            armsx_render_log("renderer", "EGL: %s (%s) is missing %s; not usable.", provider->label,
                             provider->egl_library, missing);
            dlclose(library);
            continue;
        }

        self->egl = api;
        self->egl_library = library;
        self->active_gl_driver = provider->driver;

        for (const char* name : provider->gles_libraries) {
            if (!name) {
                break;
            }
            self->gles_library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (self->gles_library) {
                break;
            }
        }

        if (provider->driver != wanted) {
            /* The loud half of "ANGLE selected can never silently mean system driver". */
            armsx_render_log("renderer",
                             "EGL: %s was requested but is unavailable; FELL BACK to the %s "
                             "driver. The renderer name will say so.",
                             armsx_render_gl_driver_token(wanted), provider->label);
        }
        armsx_render_log("renderer", "EGL: provider=%s egl=%s gles=%s", provider->label,
                         provider->egl_library,
                         self->gles_library ? "dlopen ok" : "eglGetProcAddress only");
        return true;
    }

    armsx_render_log("renderer", "EGL: no usable EGL implementation could be opened.");
    return false;
}

bool CreateEglContext(GlRenderer* self, ANativeWindow* native_window) {
    if (!LoadEglApi(self, armsx_render_gl_driver())) {
        return false;
    }

    self->egl_display = self->egl.GetDisplay(EGL_DEFAULT_DISPLAY);
    if (self->egl_display == EGL_NO_DISPLAY) {
        armsx_render_log("renderer", "eglGetDisplay failed (0x%04x)", self->egl.GetError());
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (self->egl.Initialize(self->egl_display, &major, &minor) != EGL_TRUE) {
        armsx_render_log("renderer", "eglInitialize failed (0x%04x)", self->egl.GetError());
        self->egl_display = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };

    armsx_render_log("renderer", "EGL: initialized %d.%d on thread %ld window=%p (%dx%d)", (int)major,
                     (int)minor, CurrentThreadId(), static_cast<void*>(native_window),
                     ANativeWindow_getWidth(native_window), ANativeWindow_getHeight(native_window));

    /* eglChooseConfig's sort order is "most colour bits first", so the head of the list is
       not necessarily a config whose EGL_NATIVE_VISUAL_ID is a format ANativeWindow accepts.
       Walk the matches and take the first usable one, logging every candidate: the config is
       the first thing to check when a context comes up clean and renders nothing. */
    EGLConfig configs[16] = {nullptr};
    EGLint config_count = 0;
    if (self->egl.ChooseConfig(self->egl_display, config_attribs, configs,
                        static_cast<EGLint>(sizeof(configs) / sizeof(configs[0])),
                        &config_count) != EGL_TRUE || config_count < 1) {
        armsx_render_log("renderer", "eglChooseConfig found no ES3 window config (0x%04x)", self->egl.GetError());
        return false;
    }

    /* android_pixel_format values ANativeWindow_setBuffersGeometry() accepts. */
    static const EGLint kWindowVisuals[] = {1 /* RGBA_8888 */,  2 /* RGBX_8888 */,
                                            3 /* RGB_888 */,    4 /* RGB_565 */,
                                            22 /* RGBA_FP16 */, 43 /* RGBA_1010102 */};

    EGLConfig config = configs[0];
    EGLint native_visual = 0;
    bool picked = false;
    for (EGLint index = 0; index < config_count; ++index) {
        EGLint red = 0, green = 0, blue = 0, alpha = 0, samples = 0, visual = 0;
        self->egl.GetConfigAttrib(self->egl_display, configs[index], EGL_RED_SIZE, &red);
        self->egl.GetConfigAttrib(self->egl_display, configs[index], EGL_GREEN_SIZE, &green);
        self->egl.GetConfigAttrib(self->egl_display, configs[index], EGL_BLUE_SIZE, &blue);
        self->egl.GetConfigAttrib(self->egl_display, configs[index], EGL_ALPHA_SIZE, &alpha);
        self->egl.GetConfigAttrib(self->egl_display, configs[index], EGL_SAMPLES, &samples);
        self->egl.GetConfigAttrib(self->egl_display, configs[index], EGL_NATIVE_VISUAL_ID, &visual);

        bool usable = samples == 0;
        if (usable) {
            usable = false;
            for (const EGLint candidate : kWindowVisuals) {
                if (visual == candidate) {
                    usable = true;
                    break;
                }
            }
        }

        if (index < 4 || (usable && !picked)) {
            armsx_render_log("renderer",
                             "EGL: config[%d] r%d g%d b%d a%d samples=%d visual=%d%s", (int)index,
                             (int)red, (int)green, (int)blue, (int)alpha, (int)samples, (int)visual,
                             usable && !picked ? " <- chosen" : "");
        }

        if (usable && !picked) {
            config = configs[index];
            native_visual = visual;
            picked = true;
        }
    }

    if (!picked) {
        self->egl.GetConfigAttrib(self->egl_display, config, EGL_NATIVE_VISUAL_ID, &native_visual);
        armsx_render_log("renderer",
                         "EGL: no config with a window-compatible visual among %d matches; using the "
                         "first (visual=%d)", (int)config_count, (int)native_visual);
    }

    /* Take the window now, before its geometry is touched. The JNI host re-applies the blit
       bridge's framebuffer geometry from the UI thread on every SurfaceView layout pass
       (android_jni.cpp ApplyBuffersGeometryLocked), and it only stands down once this flag is
       set — so raising it any later leaves a window in which a resize/reformat lands on top of
       a live EGLSurface. DestroyEglContext() clears it again on every failure path. */
    armsx_render_set_native_window_claimed(true);

    /* Match the window buffer format to the chosen config, as the EGL spec requires on
       Android. Width/height stay 0 so the surface keeps its natural size. */
    if (native_visual != 0) {
        const int32_t geometry = ANativeWindow_setBuffersGeometry(native_window, 0, 0, native_visual);
        if (geometry != 0) {
            armsx_render_log("renderer",
                             "EGL: ANativeWindow_setBuffersGeometry(0,0,visual %d) failed (%d); the "
                             "window keeps the host's buffer geometry.", (int)native_visual,
                             (int)geometry);
        }
    }

    /* The current rendering API is per-thread state with a process-wide default. Nothing here
       should have moved it off ES, but binding it explicitly is free and the failure it
       prevents — a desktop-GL context created and made current with no error anywhere — is
       indistinguishable from a black window. */
    if (self->egl.BindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        armsx_render_log("renderer", "self->egl.BindAPI(EGL_OPENGL_ES_API) failed (0x%04x)", self->egl.GetError());
    }

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    self->egl_context = self->egl.CreateContext(self->egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (self->egl_context == EGL_NO_CONTEXT) {
        armsx_render_log("renderer", "self->egl.CreateContext(ES3) failed (0x%04x)", self->egl.GetError());
        return false;
    }

    self->egl_config_kept = (void*)config;
    self->window_generation = armsx_render_native_window_generation();

    self->egl_surface = self->egl.CreateWindowSurface(self->egl_display, config, native_window, nullptr);
    if (self->egl_surface == EGL_NO_SURFACE) {
        armsx_render_log("renderer", "eglCreateWindowSurface failed (0x%04x)", self->egl.GetError());
        return false;
    }

    if (self->egl.MakeCurrent(self->egl_display, self->egl_surface, self->egl_surface, self->egl_context) != EGL_TRUE) {
        armsx_render_log("renderer", "eglMakeCurrent failed (0x%04x)", self->egl.GetError());
        return false;
    }

    EGLint surface_width = 0;
    EGLint surface_height = 0;
    self->egl.QuerySurface(self->egl_display, self->egl_surface, EGL_WIDTH, &surface_width);
    self->egl.QuerySurface(self->egl_display, self->egl_surface, EGL_HEIGHT, &surface_height);
    armsx_render_log("renderer",
                     "EGL: context=%p surface=%p (%dx%d) visual=%d current on thread %ld, gles=%s",
                     static_cast<void*>(self->egl_context), static_cast<void*>(self->egl_surface),
                     (int)surface_width, (int)surface_height, (int)native_visual,
                     CurrentThreadId(), self->gles_library ? "dlopen ok" : "eglGetProcAddress only");

    if (surface_width <= 0 || surface_height <= 0) {
        armsx_render_log("renderer", "EGL: window surface has no area; nothing could be presented.");
        return false;
    }

    self->context_thread = CurrentThreadId();
    self->use_egl = true;
    self->es_profile = true;
    /* Hold our own reference: the JNI host releases its ANativeWindow the moment the Compose
       Surface goes away, and the EGLSurface must outlive that race. */
    ANativeWindow_acquire(native_window);
    self->native_window = native_window;
    return true;
}

void DestroyEglContext(GlRenderer* self) {
    /* Paired with the claim CreateEglContext() takes before it touches the window: without
       this, a failed bring-up would leave the host's blit bridge permanently stood down and
       the fallback ladder would present into nothing. */
    armsx_render_set_native_window_claimed(false);

    /* Reachable with an empty table: LoadEglApi() can fail before anything was created. */
    if (self->egl_display != EGL_NO_DISPLAY && self->egl.MakeCurrent) {
        self->egl.MakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (self->egl_surface != EGL_NO_SURFACE) {
            self->egl.DestroySurface(self->egl_display, self->egl_surface);
        }
        if (self->egl_context != EGL_NO_CONTEXT) {
            self->egl.DestroyContext(self->egl_display, self->egl_context);
        }
        self->egl.Terminate(self->egl_display);
    }
    self->egl_display = EGL_NO_DISPLAY;
    self->egl_surface = EGL_NO_SURFACE;
    self->egl_context = EGL_NO_CONTEXT;

    if (self->gles_library) {
        dlclose(self->gles_library);
        self->gles_library = nullptr;
    }
    if (self->egl_library) {
        dlclose(self->egl_library);
        self->egl_library = nullptr;
    }
    self->egl = EglApi{};
    if (self->native_window) {
        ANativeWindow_release(self->native_window);
        self->native_window = nullptr;
    }
}
#endif /* ARMSX_GL_HAVE_EGL */

/* ---- texture ---------------------------------------------------------------------------- */

struct GlFormat {
    GLint internal_format;
    GLenum format;
    int source_bytes_per_pixel;
    bool packed_555;
};

bool ResolveFormat(Uint32 sdl_format, GlFormat* out) {
    switch (sdl_format) {
        case SDL_PIXELFORMAT_BGR555:
            out->internal_format = static_cast<GLint>(GL_RG8);
            out->format = GL_RG;
            out->source_bytes_per_pixel = 2;
            out->packed_555 = true;
            return true;
        case SDL_PIXELFORMAT_RGB24:
            out->internal_format = static_cast<GLint>(GL_RGB8);
            out->format = GL_RGB;
            out->source_bytes_per_pixel = 3;
            out->packed_555 = false;
            return true;
        default:
            return false;
    }
}

void ApplyTextureFilter(GlRenderer* self, bool linear, bool packed_555) {
    /* Packed 16-bit texels can never be hardware-filtered (see the file header); the
       bilinear variant is done in the shader instead. */
    const bool hardware_linear = linear && !packed_555;
    const int wanted = hardware_linear ? 1 : 0;
    if (self->texture_filter == wanted) {
        return;
    }
    const GLint mode = static_cast<GLint>(hardware_linear ? GL_LINEAR : GL_NEAREST);
    self->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
    self->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
    self->texture_filter = wanted;
}

/* ---- ops ----------------------------------------------------------------------------- */

const char* OpDriverName(armsx_renderer_t* base) {
    return Self(base)->driver_name.c_str();
}

bool OpIsAccelerated(armsx_renderer_t*) {
    return true;
}

SDL_Renderer* OpSdlHandle(armsx_renderer_t*) {
    return nullptr;
}

void OpResize(armsx_renderer_t* base, int width, int height) {
    GlRenderer* self = Self(base);
    if (width > 0) {
        self->output_width = width;
    }
    if (height > 0) {
        self->output_height = height;
    }
}

void OpOutputSize(armsx_renderer_t* base, int* width, int* height) {
    GlQueryDrawableSize(Self(base), width, height);
}

void OpSetVsync(armsx_renderer_t* base, bool enabled) {
    GlSetSwapInterval(Self(base), enabled);
}

bool OpUploadFrame(armsx_renderer_t* base,
                   const void* pixels,
                   int width,
                   int height,
                   int pitch,
                   Uint32 sdl_format,
                   int dirty_first_row,
                   int dirty_last_row) {
    GlRenderer* self = Self(base);
    /* A CPU upload always wins back the source, so an adopted texture can never linger and
       shadow it. Cleared before the argument checks so a rejected upload still releases it. */
    self->external_texture = 0;
    if (!pixels || width <= 0 || height <= 0 || pitch <= 0) {
        return false;
    }

    GlFormat format{};
    if (!ResolveFormat(sdl_format, &format)) {
        armsx_render_log("renderer", "GL backend cannot upload %s", SDL_GetPixelFormatName(sdl_format));
        return false;
    }

    if (!EnsureContextCurrent(self)) {
        return false;
    }

    if (self->upload_traces < 3) {
        self->upload_traces++;
        armsx_render_log("renderer",
                         "GL upload #%d thread=%ld %dx%d pitch=%d format=%s rows=%d..%d",
                         self->upload_traces, CurrentThreadId(), width, height, pitch,
                         SDL_GetPixelFormatName(sdl_format), dirty_first_row, dirty_last_row);
    }

    self->gl.ActiveTexture(GL_TEXTURE0);

    if (!self->texture || width != self->texture_width || height != self->texture_height ||
        sdl_format != self->texture_format) {
        if (!self->texture) {
            self->gl.GenTextures(1, &self->texture);
        }
        self->gl.BindTexture(GL_TEXTURE_2D, self->texture);
        self->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        self->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        self->texture_filter = -1;
        ApplyTextureFilter(self, false, format.packed_555);
        self->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        self->gl.TexImage2D(GL_TEXTURE_2D, 0, format.internal_format, width, height, 0, format.format,
                            GL_UNSIGNED_BYTE, nullptr);
        self->texture_width = width;
        self->texture_height = height;
        self->texture_format = sdl_format;
        /* Fresh storage has undefined contents: force a full upload this frame. */
        dirty_first_row = 0;
        dirty_last_row = height - 1;
        if (!GlCheck(self, "texture storage allocation")) {
            return false;
        }
    } else {
        self->gl.BindTexture(GL_TEXTURE_2D, self->texture);
    }

    /* Storage exists, so the quad has something to sample from. Marking the frame here
       rather than after the copy matters: an empty dirty span returns early, and gating
       present() on "a copy happened" meant a single skipped span could stop the backend
       swapping at all — which on an embedded Surface is a frozen (black) window, not a
       stale frame. */
    self->has_frame = true;

    if (dirty_last_row < dirty_first_row) {
        return true;
    }

    if (dirty_first_row < 0) {
        dirty_first_row = 0;
    }
    if (dirty_last_row > height - 1) {
        dirty_last_row = height - 1;
    }

    const int rows = dirty_last_row - dirty_first_row + 1;
    const size_t row_bytes = static_cast<size_t>(width) * static_cast<size_t>(format.source_bytes_per_pixel);

    /* Rows are PSX_GPU_FB_STRIDE bytes apart; repack the dirty span so the whole update is
       one glTexSubImage2D. GL_UNPACK_ROW_LENGTH cannot express a 2048-byte stride for the
       3-byte format. */
    self->staging.resize(row_bytes * static_cast<size_t>(rows));
    const auto* source = static_cast<const uint8_t*>(pixels);
    for (int row = 0; row < rows; ++row) {
        std::memcpy(self->staging.data() + (static_cast<size_t>(row) * row_bytes),
                    source + (static_cast<size_t>(dirty_first_row + row) * static_cast<size_t>(pitch)),
                    row_bytes);
    }

    self->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    self->gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, dirty_first_row, width, rows, format.format,
                           GL_UNSIGNED_BYTE, self->staging.data());

    /* Bounded: glGetError() is a full pipeline sync on a tiler, so it must not survive
       bring-up into the steady-state frame loop. */
    if (self->upload_checks < 3) {
        self->upload_checks++;
        GlCheck(self, "glTexSubImage2D");
    }
    return true;
}

/*
    Presents whatever state the backend is in. The only early-outs left are the two that make
    a swap impossible — no current context, and a zero-area window surface; a missing frame,
    an unsupported format or a shader that would not build all fall through to clear + swap.

    That is deliberate. This backend owns the host's Surface outright — the JNI blit bridge
    stands down the moment it claims the window — so a present that returns without swapping
    does not leave a stale frame on screen, it leaves the last thing anyone posted, forever.
    Every such bail was therefore a permanent black window indistinguishable from a broken
    renderer; now each one still posts a frame and says why in the log.
*/
void OpPresent(armsx_renderer_t* base, const armsx_render_frame_params_t* params) {
    GlRenderer* self = Self(base);

    if (!EnsureContextCurrent(self)) {
        return;
    }

    int out_w = 0;
    int out_h = 0;
    GlQueryDrawableSize(self, &out_w, &out_h);
    if (out_w <= 0 || out_h <= 0) {
        if (self->present_traces < 3) {
            self->present_traces++;
            armsx_render_log("renderer", "GL present: drawable has no area (%dx%d).", out_w, out_h);
        }
        return;
    }
    self->output_width = out_w;
    self->output_height = out_h;

    const bool linear = params && params->linear_filter;

    GlFormat format{};
    ProgramKind kind = kProgramRgb;
    /* Prefer a frame the rasterizer left on the GPU: no readback, no re-upload. */
    const bool use_external = self->external_texture != 0;
    const GLuint src_texture = use_external ? self->external_texture : self->texture;
    const int src_width = use_external ? self->external_width : self->texture_width;
    const int src_height = use_external ? self->external_height : self->texture_height;
    const Uint32 src_format = use_external ? self->external_format : self->texture_format;
    bool drawable = src_texture != 0 && self->has_frame && ResolveFormat(src_format, &format);
    if (drawable) {
        if (format.packed_555) {
            kind = linear ? kProgram555Linear : kProgram555Nearest;
        }
        if (!BuildProgram(self, kind)) {
            /* Already logged in detail by CompileShader/BuildProgram. Fall through so the
               window keeps getting frames rather than freezing on the failure. */
            drawable = false;
        }
    }

    SDL_Rect dst{0, 0, 0, 0};
    if (drawable) {
        armsx_render_compute_dst(out_w, out_h, src_width, src_height, params, &dst);
    }

    self->gl.Disable(GL_DEPTH_TEST);
    self->gl.Disable(GL_CULL_FACE);
    self->gl.Disable(GL_BLEND);
    self->gl.Disable(GL_SCISSOR_TEST);

    self->gl.Viewport(0, 0, out_w, out_h);
    self->gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    self->gl.Clear(GL_COLOR_BUFFER_BIT);

    if (drawable && dst.w > 0 && dst.h > 0) {
        /* SDL_Rect is top-left origin; GL viewports are bottom-left. */
        self->gl.Viewport(dst.x, out_h - dst.y - dst.h, dst.w, dst.h);

        self->gl.UseProgram(self->programs[kind]);
        self->gl.ActiveTexture(GL_TEXTURE0);
        self->gl.BindTexture(GL_TEXTURE_2D, src_texture);
        ApplyTextureFilter(self, linear, format.packed_555);

        if (self->uniform_tex[kind] >= 0) {
            self->gl.Uniform1i(self->uniform_tex[kind], 0);
        }
        if (self->uniform_texsize[kind] >= 0) {
            self->gl.Uniform2f(self->uniform_texsize[kind], static_cast<GLfloat>(src_width),
                               static_cast<GLfloat>(src_height));
        }

        /* [video] overscan_crop / display_rotation. Clamped against the real texture size so
           a stale crop from a previous, larger display mode can never sample past the edge —
           the display resolution changes mid-game (256/320/368/512/640) and the crop is
           recomputed from it every frame, but a frame in between must still be safe. */
        {
            float off_x = 0.0f, off_y = 0.0f, sc_x = 1.0f, sc_y = 1.0f;

            if (params && params->crop_w > 0 && params->crop_h > 0 && src_width > 0 &&
                src_height > 0) {
                int cx = params->crop_x < 0 ? 0 : params->crop_x;
                int cy = params->crop_y < 0 ? 0 : params->crop_y;
                int cw = params->crop_w;
                int ch = params->crop_h;

                if (cx > src_width - 1) cx = src_width - 1;
                if (cy > src_height - 1) cy = src_height - 1;
                if (cw > src_width - cx) cw = src_width - cx;
                if (ch > src_height - cy) ch = src_height - cy;

                off_x = static_cast<float>(cx) / static_cast<float>(src_width);
                off_y = static_cast<float>(cy) / static_cast<float>(src_height);
                sc_x = static_cast<float>(cw) / static_cast<float>(src_width);
                sc_y = static_cast<float>(ch) / static_cast<float>(src_height);
            }

            if (self->uniform_uvoff[kind] >= 0) {
                self->gl.Uniform2f(self->uniform_uvoff[kind], off_x, off_y);
            }
            if (self->uniform_uvscale[kind] >= 0) {
                self->gl.Uniform2f(self->uniform_uvscale[kind], sc_x, sc_y);
            }
            if (self->uniform_rot[kind] >= 0) {
                self->gl.Uniform1i(self->uniform_rot[kind],
                                   params ? (params->rotation & 3) : 0);
            }
        }

        self->gl.BindVertexArray(self->vao);
        self->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        self->gl.BindVertexArray(0);
        self->gl.UseProgram(0);
    }

    if (self->present_traces < 3) {
        self->present_traces++;
        armsx_render_log("renderer",
                         "GL present #%d thread=%ld drawable=%s out=%dx%d dst=%d,%d %dx%d "
                         "tex=%dx%d(id %u) format=%s program=%u(kind %d) vao=%u",
                         self->present_traces, CurrentThreadId(), drawable ? "true" : "false", out_w,
                         out_h, dst.x, dst.y, dst.w, dst.h, self->texture_width,
                         self->texture_height, self->texture,
                         SDL_GetPixelFormatName(self->texture_format), self->programs[kind],
                         (int)kind, self->vao);
        GlCheck(self, "present");
    }

    GlSwap(self);
}

void OpPresentBlank(armsx_renderer_t* base) {
    GlRenderer* self = Self(base);
    if (!EnsureContextCurrent(self)) {
        return;
    }
    int out_w = 0;
    int out_h = 0;
    GlQueryDrawableSize(self, &out_w, &out_h);
    if (out_w <= 0 || out_h <= 0) {
        return;
    }
    self->gl.Viewport(0, 0, out_w, out_h);
    self->gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    self->gl.Clear(GL_COLOR_BUFFER_BIT);
    GlSwap(self);
}

void OpShutdown(armsx_renderer_t* base) {
    GlRenderer* self = Self(base);

    /* Deleting objects needs the context on THIS thread too, and teardown does not
       necessarily run on the thread that presented. */
    EnsureContextCurrent(self);

    if (self->gl.DeleteTextures && self->texture) {
        self->gl.DeleteTextures(1, &self->texture);
    }
    for (int index = 0; index < kProgramCount; ++index) {
        if (self->gl.DeleteProgram && self->programs[index]) {
            self->gl.DeleteProgram(self->programs[index]);
        }
    }
    if (self->gl.DeleteBuffers && self->vbo) {
        self->gl.DeleteBuffers(1, &self->vbo);
    }
    if (self->gl.DeleteVertexArrays && self->vao) {
        self->gl.DeleteVertexArrays(1, &self->vao);
    }

#if defined(ARMSX_GL_HAVE_EGL)
    if (self->use_egl) {
        DestroyEglContext(self);
        armsx_render_set_native_window_claimed(false);
    } else
#endif
    if (self->sdl_context) {
        SDL_GL_DeleteContext(self->sdl_context);
        self->sdl_context = nullptr;
    }

    armsx_render_set_active_name("");
    delete self;
}

/* Adopt a GPU-resident frame. Returns false without changing anything when the format is
   not one this backend can sample, which is the caller's cue to keep using upload_frame(). */
bool OpAdoptGlTexture(armsx_renderer_t* base, unsigned int texture, int width, int height,
                      Uint32 sdl_format) {
    GlRenderer* self = Self(base);

    if (texture == 0) {
        self->external_texture = 0;
        return true;
    }

    GlFormat format{};
    if (width <= 0 || height <= 0 || !ResolveFormat(sdl_format, &format)) {
        return false;
    }

    self->external_texture = static_cast<GLuint>(texture);
    self->external_width = width;
    self->external_height = height;
    self->external_format = sdl_format;
    self->has_frame = true;
    return true;
}

const armsx_render_ops_t kGlOps = {
    OpDriverName,
    OpIsAccelerated,
    OpSdlHandle,
    OpResize,
    OpOutputSize,
    OpSetVsync,
    OpUploadFrame,
    OpPresent,
    OpPresentBlank,
    OpShutdown,
    OpAdoptGlTexture,
};

bool FinishSetup(GlRenderer* self, GlProcLoader loader, void* user, bool vsync) {
    std::string missing;
    if (!LoadGlApi(self->gl, loader, user, missing)) {
        armsx_render_log("renderer", "GL entry point unavailable: %s", missing.c_str());
        return false;
    }

    const unsigned char* version = self->gl.GetString(GL_VERSION_STR);
    const unsigned char* renderer_string = self->gl.GetString(GL_RENDERER_STR);
    const unsigned char* vendor = self->gl.GetString(GL_VENDOR);

    if (!version && !renderer_string && !vendor) {
        /* Every string query coming back NULL is the signature of GL calls landing on no
           context at all, which otherwise produces a black window and nothing else. */
        armsx_render_log("renderer", "GL: glGetString returned nothing; there is no current context.");
        GlCheck(self, "glGetString");
        return false;
    }

    self->driver_name = renderer_string ? reinterpret_cast<const char*>(renderer_string) : "OpenGL";

    armsx_render_log("renderer", "GL backend ready vendor=%s renderer=%s version=%s profile=%s",
                     vendor ? reinterpret_cast<const char*>(vendor) : "(unknown)",
                     renderer_string ? reinterpret_cast<const char*>(renderer_string) : "(unknown)",
                     version ? reinterpret_cast<const char*>(version) : "(unknown)",
                     self->es_profile ? "GLES3" : "GL core");

    /* Feed the same strings to the GPU profile. This matters most under ANGLE, whose renderer
       string names itself with the real GPU parenthesised inside it —
       "ANGLE (ARM, Vulkan 1.1.177 (Mali-G77 MC9), ...)" — so a reader that stops at the vendor
       loses every Mali fact at the exact moment they are most likely to be needed, ANGLE being
       what people turn on when the native driver already misbehaved. */
    armsx_gpu_profile_note_gl(vendor ? reinterpret_cast<const char*>(vendor) : nullptr,
                              renderer_string ? reinterpret_cast<const char*>(renderer_string) : nullptr,
                              version ? reinterpret_cast<const char*>(version) : nullptr);
    {
        char profile_line[320];
        armsx_gpu_profile_describe(profile_line, sizeof(profile_line));
        armsx_render_log("renderer", "GPU profile: %s", profile_line);
    }

    /* Whatever the driver left behind while we were resolving entry points is not ours. */
    GlCheck(self, "entry point resolution");

    if (!CreateQuad(self)) {
        armsx_render_log("renderer", "GL full-screen quad setup failed.");
        return false;
    }

    /* Build the program the core starts on up front rather than on the first present: a
       driver that refuses this GLSL should say so during bring-up, where the fallback ladder
       can still act on it, not silently at frame 1. The other two variants stay on demand —
       losing the bilinear 555 shader only costs filtering, not the picture. */
    if (!BuildProgram(self, kProgram555Nearest)) {
        armsx_render_log("renderer", "GL present shader could not be built.");
        return false;
    }

    GlSetSwapInterval(self, vsync);
    GlQueryDrawableSize(self, &self->output_width, &self->output_height);
    armsx_render_log("renderer", "GL drawable %dx%d vsync=%s", self->output_width,
                     self->output_height, vsync ? "true" : "false");
    return GlCheck(self, "GL backend setup");
}

} // namespace

extern "C" {

void armsx_render_gl_prepare_attributes(void) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

armsx_renderer_t* armsx_render_create_gl(SDL_Window* window, const armsx_render_config_t* config) {
    const bool vsync = config && config->vsync;

    auto* self = new GlRenderer();
    self->base.ops = &kGlOps;
    self->base.impl = self;
    self->base.backend = ARMSX_RENDER_BACKEND_OPENGL;
    self->window = window;

#if defined(ARMSX_GL_HAVE_EGL)
    /* Preferred on Android: the Compose host's ANativeWindow. SDL runs on the `dummy` video
       driver there, so SDL_GL_CreateContext has nothing to attach to. */
    armsx_render_log("renderer", "GL backend: create on thread %ld, native window=%p, sdl window=%p",
                     CurrentThreadId(), armsx_render_native_window(), static_cast<void*>(window));
    if (auto* native_window = static_cast<ANativeWindow*>(armsx_render_native_window())) {
        if (CreateEglContext(self, native_window)) {
            if (FinishSetup(self, EglProcLoader, self, vsync)) {
                /* Already claimed inside CreateEglContext, before the window's geometry was
                   touched; this is the point at which the claim becomes permanent. */
                armsx_render_set_native_window_claimed(true);
                /* Names the provider we ACTUALLY bound to, not the one that was asked for. */
                const std::string name =
                    std::string("OpenGL ES (") +
                    (self->active_gl_driver == ARMSX_RENDER_GL_DRIVER_ANGLE ? "ANGLE" : "system") + ")";
                armsx_render_set_active_name(name.c_str());
                armsx_render_log("renderer", "GL backend bound to the host ANativeWindow via EGL: %s",
                                 name.c_str());
                return &self->base;
            }
        }
        armsx_render_log("renderer", "EGL bring-up on the host ANativeWindow failed; trying SDL GL.");
        DestroyEglContext(self);
        self->use_egl = false;
    }
#endif

    if (!window) {
        armsx_render_log("renderer", "GL backend needs a window (or a registered ANativeWindow).");
        delete self;
        return nullptr;
    }

    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_OPENGL) == 0) {
        armsx_render_log("renderer", "GL backend: window was not created with SDL_WINDOW_OPENGL.");
        delete self;
        return nullptr;
    }

    self->sdl_context = SDL_GL_CreateContext(window);
    self->es_profile = true;

    if (!self->sdl_context) {
        armsx_render_log("renderer", "SDL_GL_CreateContext(GLES 3.0) failed: %s; retrying GL 3.3 core.",
                         SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        self->sdl_context = SDL_GL_CreateContext(window);
        self->es_profile = false;
    }

    if (!self->sdl_context) {
        armsx_render_log("renderer", "SDL_GL_CreateContext failed: %s", SDL_GetError());
        delete self;
        return nullptr;
    }

    SDL_GL_MakeCurrent(window, self->sdl_context);

    if (!FinishSetup(self, SdlProcLoader, nullptr, vsync)) {
        SDL_GL_DeleteContext(self->sdl_context);
        delete self;
        return nullptr;
    }

    armsx_render_set_active_name(self->es_profile ? "OpenGL ES (system)" : "OpenGL (system)");
    return &self->base;
}

} // extern "C"

#else /* !ARMSX_ENABLE_GL */

extern "C" {

void armsx_render_gl_prepare_attributes(void) {}

armsx_renderer_t* armsx_render_create_gl(SDL_Window*, const armsx_render_config_t*) {
    return nullptr;
}

} // extern "C"

#endif
