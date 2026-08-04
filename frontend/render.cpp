/*
    ARMSX — presentation backend dispatch.

    Owns the backend vtable dispatch, the shared letterbox math and the process-wide
    hand-off slots (Android ANativeWindow, replaceable Vulkan loader). The backends
    themselves live in render_sdl.cpp / render_gl.cpp / render_vk.cpp.
*/

#include "render_internal.h"
#include "render_shaders.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

extern "C" {
#include "diagnostics.h"
}

namespace {

std::mutex g_native_window_lock;
void* g_native_window = nullptr;
int g_native_window_width = 0;
int g_native_window_height = 0;
bool g_native_window_claimed = false;
unsigned long g_native_window_generation = 0;

/* GL driver selection. `g_gl_driver_explicit` exists because two sources set this and they
   arrive in the wrong order: the host UI pushes its choice through JNI before the VM thread
   starts, and settings.toml is only parsed afterwards, inside the core. An explicit
   selection therefore has to survive the config load rather than be overwritten by it. */
std::mutex g_gl_driver_lock;
armsx_render_gl_driver_t g_gl_driver = ARMSX_RENDER_GL_DRIVER_SYSTEM;
bool g_gl_driver_explicit = false;

std::mutex g_active_name_lock;
std::string g_active_name;

} // namespace

extern "C" {

void armsx_render_log(const char* category, const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    psxe_diag_log_line(category ? category : "renderer", buffer);
#if defined(__ANDROID__)
    /* psxe_diag_log_line only reaches the on-device diag file, and only when diagnostics are
       enabled. Backend bring-up failures have to be visible in `adb logcat` unconditionally,
       so mirror every renderer line there under the host's tag. */
    __android_log_print(ANDROID_LOG_INFO, "ARMSX-JNI", "[%s] %s", category ? category : "renderer",
                        buffer);
#endif
}

const char* armsx_render_backend_name(armsx_render_backend_t backend) {
    switch (backend) {
        case ARMSX_RENDER_BACKEND_SDL_ACCELERATED:
            return "SDL accelerated";
        case ARMSX_RENDER_BACKEND_OPENGL:
            return "OpenGL";
        case ARMSX_RENDER_BACKEND_VULKAN:
            return "Vulkan (experimental)";
        case ARMSX_RENDER_BACKEND_SDL_SOFTWARE:
        default:
            return "SDL software";
    }
}

const char* armsx_render_backend_token(armsx_render_backend_t backend) {
    switch (backend) {
        case ARMSX_RENDER_BACKEND_SDL_ACCELERATED:
            return "sdl-accelerated";
        case ARMSX_RENDER_BACKEND_OPENGL:
            return "opengl";
        case ARMSX_RENDER_BACKEND_VULKAN:
            return "vulkan";
        case ARMSX_RENDER_BACKEND_SDL_SOFTWARE:
        default:
            return "software";
    }
}

bool armsx_render_backend_compiled_in(armsx_render_backend_t backend) {
    switch (backend) {
        case ARMSX_RENDER_BACKEND_OPENGL:
#if defined(ARMSX_ENABLE_GL)
            return true;
#else
            return false;
#endif
        case ARMSX_RENDER_BACKEND_VULKAN:
#if defined(ARMSX_ENABLE_VULKAN)
            return true;
#else
            return false;
#endif
        default:
            return true;
    }
}

Uint32 armsx_render_window_flags(armsx_render_backend_t backend) {
    switch (backend) {
        case ARMSX_RENDER_BACKEND_OPENGL:
#if defined(ARMSX_ENABLE_GL)
            return SDL_WINDOW_OPENGL;
#else
            return 0;
#endif
        case ARMSX_RENDER_BACKEND_VULKAN:
#if defined(ARMSX_ENABLE_VULKAN)
            return SDL_WINDOW_VULKAN;
#else
            return 0;
#endif
        default:
            return 0;
    }
}

void armsx_render_prepare_window_attributes(armsx_render_backend_t backend) {
#if defined(ARMSX_ENABLE_GL)
    if (backend == ARMSX_RENDER_BACKEND_OPENGL) {
        armsx_render_gl_prepare_attributes();
    }
#else
    (void)backend;
#endif
}

armsx_renderer_t* armsx_renderer_create(armsx_render_backend_t backend,
                                        SDL_Window* window,
                                        const armsx_render_config_t* config) {
    armsx_render_config_t local{};
    if (config) {
        local = *config;
    }
    local.backend = backend;

    armsx_renderer_t* created = nullptr;
    switch (backend) {
        case ARMSX_RENDER_BACKEND_OPENGL:
#if defined(ARMSX_ENABLE_GL)
            created = armsx_render_create_gl(window, &local);
#else
            armsx_render_log("renderer", "OpenGL backend requested but not compiled in.");
#endif
            break;
        case ARMSX_RENDER_BACKEND_VULKAN:
#if defined(ARMSX_ENABLE_VULKAN)
            created = armsx_render_create_vk(window, &local);
#else
            armsx_render_log("renderer", "Vulkan backend requested but not compiled in.");
#endif
            break;
        case ARMSX_RENDER_BACKEND_SDL_ACCELERATED:
        case ARMSX_RENDER_BACKEND_SDL_SOFTWARE:
        default:
            created = armsx_render_create_sdl(window, &local);
            break;
    }

    /* Tell the shader layer which backend genuinely came up — not which one was requested,
       because callers retry with a different one after a failure. This is the whole reason
       "you enabled a shader chain on a backend that cannot run one" gets SAID rather than
       the toggle silently doing nothing, which is this port's most common defect shape. */
    if (created) {
        armsx_shader_note_backend((int)created->backend);
    }
    return created;
}

armsx_renderer_t* armsx_renderer_create_from_sdl(SDL_Renderer* renderer) {
    return armsx_render_adopt_sdl(renderer);
}

void armsx_renderer_destroy(armsx_renderer_t* renderer) {
    if (!renderer) {
        return;
    }
    renderer->ops->shutdown(renderer);
}

armsx_render_backend_t armsx_renderer_backend(const armsx_renderer_t* renderer) {
    return renderer ? renderer->backend : ARMSX_RENDER_BACKEND_SDL_SOFTWARE;
}

const char* armsx_renderer_driver_name(const armsx_renderer_t* renderer) {
    if (!renderer || !renderer->ops->driver_name) {
        return "(none)";
    }
    return renderer->ops->driver_name(const_cast<armsx_renderer_t*>(renderer));
}

bool armsx_renderer_is_accelerated(const armsx_renderer_t* renderer) {
    if (!renderer || !renderer->ops->is_accelerated) {
        return false;
    }
    return renderer->ops->is_accelerated(const_cast<armsx_renderer_t*>(renderer));
}

SDL_Renderer* armsx_renderer_sdl(const armsx_renderer_t* renderer) {
    if (!renderer || !renderer->ops->sdl_handle) {
        return nullptr;
    }
    return renderer->ops->sdl_handle(const_cast<armsx_renderer_t*>(renderer));
}

void armsx_renderer_resize(armsx_renderer_t* renderer, int width, int height) {
    if (!renderer || !renderer->ops->resize) {
        return;
    }
    renderer->ops->resize(renderer, width, height);
}

void armsx_renderer_output_size(const armsx_renderer_t* renderer, int* width, int* height) {
    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
    if (!renderer || !renderer->ops->output_size) {
        return;
    }
    renderer->ops->output_size(const_cast<armsx_renderer_t*>(renderer), width, height);
}

void armsx_renderer_set_vsync(armsx_renderer_t* renderer, bool enabled) {
    if (!renderer || !renderer->ops->set_vsync) {
        return;
    }
    renderer->ops->set_vsync(renderer, enabled);
}

bool armsx_renderer_upload_frame(armsx_renderer_t* renderer,
                                 const void* pixels,
                                 int width,
                                 int height,
                                 int pitch,
                                 Uint32 sdl_format,
                                 int dirty_first_row,
                                 int dirty_last_row) {
    if (!renderer || !renderer->ops->upload_frame) {
        return false;
    }
    return renderer->ops->upload_frame(renderer, pixels, width, height, pitch, sdl_format,
                                       dirty_first_row, dirty_last_row);
}

void armsx_renderer_present(armsx_renderer_t* renderer, const armsx_render_frame_params_t* params) {
    if (!renderer || !renderer->ops->present) {
        return;
    }

    armsx_render_frame_params_t local{};
    if (params) {
        local = *params;
    }
    renderer->ops->present(renderer, &local);
}

void armsx_renderer_present_blank(armsx_renderer_t* renderer) {
    if (!renderer || !renderer->ops->present_blank) {
        return;
    }
    renderer->ops->present_blank(renderer);
}

bool armsx_renderer_adopt_gl_texture(armsx_renderer_t* renderer, unsigned int texture,
                                     int width, int height, Uint32 sdl_format) {
    if (!renderer || !renderer->ops || !renderer->ops->adopt_gl_texture) {
        return false;
    }
    return renderer->ops->adopt_gl_texture(renderer, texture, width, height, sdl_format);
}

void armsx_render_set_native_window(void* native_window, int width, int height) {
    std::lock_guard<std::mutex> lock(g_native_window_lock);
    /* Bump on EVERY publish, including the null of a surfaceDestroyed.

       Android destroys the ANativeWindow when the app goes to the background and hands back a
       BRAND NEW one on resume. A renderer that built an EGLSurface/VkSurfaceKHR on the old
       window keeps a handle to a dead object: eglSwapBuffers fails silently, no frame is ever
       posted, and because the OSD is drawn by the renderer it freezes too — while the emulation
       thread keeps running, so audio continues. That is exactly the "black screen with sound
       after task-switching" report. Nothing else in the pipeline can notice, so the counter is
       how a renderer learns its window is stale. */
    ++g_native_window_generation;
    g_native_window = native_window;
    g_native_window_width = width;
    g_native_window_height = height;

    /* The claim is deliberately NOT cleared here.

       It answers "does a GPU backend own presentation on this Surface", which a surfaceDestroyed
       does not change: the backend is still alive and is about to rebuild on the replacement
       window. Both backends raise it (CreateSurface / CreateEglContext) and lower it themselves
       (OpShutdown / DestroyEglContext), so clearing it from here was a third writer with no
       matching setter on the rebuild path — and it un-parked the host's CPU blit bridge for the
       gap, putting two producers on one buffer queue and letting the bridge stamp its own
       WINDOW_FORMAT_RGBX_8888 geometry over an EGLConfig's native visual (EGL_BAD_MATCH on the
       next eglCreateWindowSurface). The teardown that really does end presentation
       (DestroyHostWindowAndRenderer) runs AFTER the backend has been destroyed and has therefore
       already lowered it. */
}

unsigned long armsx_render_native_window_generation(void) {
    std::lock_guard<std::mutex> lock(g_native_window_lock);
    return g_native_window_generation;
}

void* armsx_render_native_window(void) {
    std::lock_guard<std::mutex> lock(g_native_window_lock);
    return g_native_window;
}

bool armsx_render_native_window_size(int* width, int* height) {
    std::lock_guard<std::mutex> lock(g_native_window_lock);
    if (!g_native_window || g_native_window_width <= 0 || g_native_window_height <= 0) {
        return false;
    }
    if (width) {
        *width = g_native_window_width;
    }
    if (height) {
        *height = g_native_window_height;
    }
    return true;
}

bool armsx_render_native_window_claimed(void) {
    std::lock_guard<std::mutex> lock(g_native_window_lock);
    return g_native_window_claimed;
}

void armsx_render_set_native_window_claimed(bool claimed) {
    std::lock_guard<std::mutex> lock(g_native_window_lock);
    g_native_window_claimed = claimed;
}

void armsx_render_set_gl_driver(armsx_render_gl_driver_t driver) {
    std::lock_guard<std::mutex> lock(g_gl_driver_lock);
    g_gl_driver = driver;
    g_gl_driver_explicit = true;
}

armsx_render_gl_driver_t armsx_render_gl_driver(void) {
    std::lock_guard<std::mutex> lock(g_gl_driver_lock);
    return g_gl_driver;
}

const char* armsx_render_gl_driver_token(armsx_render_gl_driver_t driver) {
    return driver == ARMSX_RENDER_GL_DRIVER_ANGLE ? "angle" : "system";
}

void armsx_render_set_gl_driver_default(armsx_render_gl_driver_t driver) {
    std::lock_guard<std::mutex> lock(g_gl_driver_lock);
    if (!g_gl_driver_explicit) {
        g_gl_driver = driver;
    }
}

void armsx_render_set_active_name(const char* name) {
    std::lock_guard<std::mutex> lock(g_active_name_lock);
    g_active_name = name ? name : "";
}

int armsx_render_active_name(char* buffer, int size) {
    if (!buffer || size <= 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_active_name_lock);
    const int length = (int)g_active_name.size() < size - 1 ? (int)g_active_name.size() : size - 1;
    std::memcpy(buffer, g_active_name.c_str(), (size_t)length);
    buffer[length] = '\0';
    return length;
}

void armsx_render_host_framebuffer_size(int surface_width,
                                        int surface_height,
                                        int max_short_edge,
                                        int* out_width,
                                        int* out_height) {
    int width = surface_width > 1 ? surface_width : 1;
    int height = surface_height > 1 ? surface_height : 1;

    const int cap = max_short_edge > 0 ? max_short_edge : ARMSX_RENDER_HOST_FB_MAX_SHORT_EDGE;
    const int short_edge = width < height ? width : height;
    if (short_edge > cap) {
        const long long scaled_width = ((long long)width * (long long)cap) / short_edge;
        const long long scaled_height = ((long long)height * (long long)cap) / short_edge;
        width = scaled_width > 1 ? (int)scaled_width : 1;
        height = scaled_height > 1 ? (int)scaled_height : 1;
    }

    /* Even dimensions keep the row copy and the compositor's scaler happy. */
    width &= ~1;
    height &= ~1;

    if (out_width) {
        *out_width = width > 2 ? width : 2;
    }
    if (out_height) {
        *out_height = height > 2 ? height : 2;
    }
}

int armsx_render_source_bytes_per_pixel(Uint32 sdl_format) {
    switch (sdl_format) {
        case SDL_PIXELFORMAT_BGR555:
            return 2;
        case SDL_PIXELFORMAT_RGB24:
            return 3;
        default:
            return 0;
    }
}

/*
    Verbatim port of the destination-rect math the SDL_RenderCopy path used
    (frontend/main.cpp ArmsxSession::draw before the refactor):

        target = full output; if !stretch, fit `aspect` inside it and centre.

    The float -> int truncation is preserved on purpose so the SDL and GPU backends land on
    exactly the same pixels.
*/
void armsx_render_compute_dst(int output_width,
                              int output_height,
                              int source_width,
                              int source_height,
                              const armsx_render_frame_params_t* params,
                              SDL_Rect* out_dst) {
    if (!out_dst) {
        return;
    }

    const float display_width = (float)(output_width > 1 ? output_width : 1);
    const float display_height = (float)(output_height > 1 ? output_height : 1);

    /* [video] overscan_crop. The rect being presented is what has to be fitted, so every
       source dimension below is the CROPPED one. Zero (the default) means the whole frame,
       so the arithmetic collapses to what it was. */
    if (params && params->crop_w > 0 && params->crop_h > 0) {
        source_width = params->crop_w;
        source_height = params->crop_h;
    }

    /* [video] display_rotation. A quarter turn transposes the picture, so the rect that gets
       fitted is the transposed one — a 4:3 frame at 90 degrees is a 3:4 rect. Doing it here
       rather than in each backend keeps every backend letterboxing identically, which is the
       whole reason this function is shared. */
    const int rotation = (params && params->rotation > 0) ? (params->rotation & 3) : 0;

    if (rotation & 1) {
        const int swap = source_width;
        source_width = source_height;
        source_height = swap;
    }

    float aspect = params ? params->aspect : 0.0f;
    if (!(aspect > 0.0f)) {
        const int safe_height = source_height > 0 ? source_height : 1;
        aspect = source_width > 0 ? ((float)source_width / (float)safe_height) : (4.0f / 3.0f);
    } else if (rotation & 1) {
        /* The caller's aspect describes the UPRIGHT frame (main.cpp hands over a hard 4:3),
           so it has to be inverted with the picture or a rotated frame is presented at the
           landscape ratio and squashed. */
        aspect = 1.0f / aspect;
    }

    float target_width = display_width;
    float target_height = display_height;
    float offset_x = 0.0f;
    float offset_y = 0.0f;

    if (!params || !params->stretch) {
        target_width = display_width;
        target_height = target_width / aspect;

        if (target_height > display_height) {
            target_height = display_height;
            target_width = target_height * aspect;
        }

        offset_x = (display_width - target_width) * 0.5f;
        offset_y = (display_height - target_height) * 0.5f;

        /* Portrait: top-align instead of centring, and start below the camera cutout.
           Centring is right on a landscape screen, where the leftover space is split evenly
           above and below. In portrait the leftover is large and all of it is wanted in ONE
           place — under the image, for the touch controls. Centred, the controls overlap the
           game and the game sits under the punch-hole camera at the same time. */
        if (params && params->portrait_top && display_height > display_width) {
            const float inset = (float)(params->portrait_top_inset > 0 ? params->portrait_top_inset : 0);

            /* Never push the image off the bottom: on a short window the inset can exceed the
               slack, and clamping keeps the whole frame visible rather than cropping it. */
            offset_y = (inset + target_height <= display_height)
                           ? inset
                           : (display_height - target_height);

            if (offset_y < 0.0f) {
                offset_y = 0.0f;
            }
        }
    }

    /* Integer scaling, applied AFTER the aspect fit so the aspect choice still decides the
       shape and this only quantises the size. Derived from the fitted height rather than the
       width because the PS1's horizontal resolution varies per game (256/320/368/512/640)
       while the vertical is essentially always 240 or 480 — snapping the axis that actually
       corresponds to scanlines is what removes the uneven-row shimmer. Width then follows
       from the aspect, so a 4:3 or 16:9 pick is preserved exactly.

       Deliberately skipped when the window cannot fit even 1x: clamping to zero would blank
       the screen, and a user on a small window is better served by the fitted image. */
    if (params && params->integer_scaling && !params->stretch && source_height > 0) {
        const int factor = (int)(target_height / (float)source_height);
        if (factor >= 1) {
            target_height = (float)(factor * source_height);
            target_width = target_height * aspect;
            offset_x = (display_width - target_width) * 0.5f;
            offset_y = (display_height - target_height) * 0.5f;
        }
    }

    out_dst->x = (int)offset_x;
    out_dst->y = (int)offset_y;
    out_dst->w = (int)target_width;
    out_dst->h = (int)target_height;
}

} // extern "C"
