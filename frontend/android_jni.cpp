/*
    ARMSX — in-process Android JNI host.

    The historical Android path runs the core inside its own SDL activity
    (com.nanodata.armsx.EmulatorActivity extends org.libsdl.app.SDLActivity), which owns the
    whole screen. That makes the Jetpack Compose front-end's touch overlay, hotkeys, pause
    overlay, RetroAchievements and Discord presence impossible: they live in a different
    process-visible Activity than the pixels.

    This file adds the alternative: the core renders into a Surface that the Compose UI hosts,
    inside the SAME process, driven from kr.co.iefriends.pcsx2.NativeApp. It is purely
    additive — external_main / EmulatorActivity keep working untouched.

    HOW THE SURFACE HAND-OFF WORKS
    ------------------------------
    1. The Compose SurfaceView calls NativeApp.onNativeSurfaceChanged(surface, w, h). We take
       an ANativeWindow with ANativeWindow_fromSurface() and keep it (refcounted).
    2. NativeApp.runVMThread(path) is called on a dedicated thread. It builds an SDL_Window +
       SDL_Renderer for that surface and hands them to the core as
       external_main_ex(argc, argv, window, renderer). The core's initializeWindowAndRenderer()
       adopts them instead of creating its own (frontend/main.cpp, external_window_ /
       external_renderer_).
    3. We first try SDL_CreateWindowFrom(nativeWindow), which is the direct route. SDL's Android
       video backend does NOT implement CreateSDLWindowFrom (see
       third_party/SDL/src/video/android/SDL_androidvideo.c — only CreateSDLWindow is wired),
       and it additionally resolves its window through org.libsdl.app.SDLActivity's static
       JNI glue, which does not exist when Compose owns the Activity. So the realistic path
       today is the fallback below; the SDL_CreateWindowFrom attempt is kept because it is the
       correct thing to use the moment a backend supports it.
    4. Fallback ("blit bridge"): SDL runs on its `dummy` video driver, we own an SDL_Surface
       framebuffer and a software SDL_Renderer over it, and after every presented frame the
       core calls back into PresentToSurface(), which copies the framebuffer into the
       ANativeWindow with ANativeWindow_lock()/unlockAndPost(). ANativeWindow_setBuffersGeometry
       lets SurfaceFlinger do the (free, GPU) upscale to the real surface size, so the
       CPU-side blit stays at the framebuffer resolution.

    THREADING
    ---------
    Every JNI entry point except runVMThread() is called from the Android UI thread while the
    emulation loop runs on its own thread. None of them touch SDL or the psx_t directly: they
    park requests through the mutex-guarded psxe_host_* API in frontend/main.cpp, which the
    emulation loop drains once per frame (the same pattern as the pending-launch-argument
    queue). The only shared object this file touches across threads is the ANativeWindow, and
    that is refcounted and guarded by g_surface_mutex — never held across the blocking
    ANativeWindow_lock().
*/

#if defined(__ANDROID__)

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <dlfcn.h>

#include <SDL.h>

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "config.h"
#include "../psx/pgxp.h"
#include "../psx/state.h"
/* [cheats] — the GameShark engine. Read psx/cheats.h before touching the four natives at the
   bottom of this file: the format choice, the threading contract (this file is the UI thread;
   the emulation thread only ever adopts a published program) and the hardcore interlock are
   all argued there. */
#include "../psx/cheats.h"
}

// Presentation backend abstraction. Used here only to (a) publish the ANativeWindow so a GPU
// backend can bind EGL / a VkSurfaceKHR straight to it, and (b) stand the CPU blit bridge
// down once one has.
#include "render.h"
// Surface lifecycle goes to the on-device diag log as well as logcat: a black screen after a
// task switch is diagnosed from armsx.log, which a tester can send, and never from logcat,
// which needs adb. Transition events only — nothing here is per-frame.
#include "diagnostics.h"
/* Control surface only: this TU never touches a VkImage. */
#define ARMSX_RENDER_SHADERS_NO_VK
#include "render_shaders.h"

// RetroAchievements. This file supplies the two host services the module needs — a blocking
// HTTP transport (kr.co.iefriends.pcsx2.HttpClient) and unlock-sound playback
// (NativeApp.playSound) — and exposes the whole thing to Kotlin through NativeApp.
#include "achievements.h"

// PSXE_HOST_STAT_* — the layout of the performance snapshot getStatistics() hands Kotlin.
#include "host_stats.h"
#include "gpu_profile.h"
// ADPF CPU clock hint + emulation-thread affinity. Both setters are safe from this (UI) thread
// with no VM running: they park a value that the emulation loop reads. See perf_hint.h.
#include "perf_hint.h"
#include <cstdio>

// Implemented in frontend/main.cpp.
extern "C" {
int external_main_ex(int argc, const char* argv[], void* external_window, void* external_renderer);
void psxe_enqueue_launch_argument(const char* argument);
void psxe_host_set_embedded(int enabled);
void psxe_host_set_present_callback(void (*callback)(void*), void* user);
void psxe_host_set_paused(int paused);
void psxe_host_set_audio_suspended(int suspended);
void psxe_host_set_presentation_suspended(int suspended);
void psxe_host_request_shutdown(void);
void psxe_host_request_reset(void);
void psxe_host_set_fast_forward(int enabled);
void psxe_host_set_display_aspect(int mode);
void psxe_host_set_display_aspect_custom(float ratio);
void psxe_host_set_integer_scaling(int enabled);
void psxe_host_set_portrait_render_top(int enabled);
void psxe_host_set_portrait_render_top_inset(int pixels);
void psxe_host_set_stretch_mode(int enabled);
/* [video] display feature set. All present-time except the widescreen hack, which is a GTE
   change and therefore core state. See frontend/main.cpp for the override contract. */
void psxe_host_set_deinterlace(int mode);
void psxe_host_set_overscan_crop(int mode);
void psxe_host_set_display_rotation(int degrees);
void psxe_host_set_widescreen_hack(int enabled);
void psxe_host_set_gl_video_options(int texture_filter, int downsample, int line_detect);
/* Texture dumping / replacement (psx/texrep.h). Parked by the host and applied on the
   emulation thread; see frontend/main.cpp. */
void psxe_host_set_texture_options(int dump, int replace, const char* dir);
void psxe_host_set_speed_limits(int frame_limit, int speed_percent, int fps_limit, float fast_forward_speed,
                                int frame_skip);
void psxe_host_set_audio_backends_ready(int ready);
void psxe_host_request_screenshot(const char* path);
void psxe_host_request_disc_swap(const char* path);
int psxe_host_vm_active(void);
int psxe_host_loop_running(void);
float psxe_host_measured_fps(void);
float psxe_host_nominal_frame_rate(void);
unsigned int psxe_host_presented_frames(void);
void psxe_host_set_stats_enabled(int enabled);
unsigned int psxe_host_stats(double* out, unsigned int count);
void psxe_host_pad_button(int code, int range, int pressed);
void psxe_host_pad_analog(int stick, int x, int y);
/* The same two addressed to one of the four players behind a port-1 multitap. Player 0 is
   the port itself, so these are what the plain forms call. With no tap attached the core
   DROPS players 1..3 rather than folding them onto player 1. */
void psxe_host_pad_button_player(int player, int code, int range, int pressed);
void psxe_host_pad_analog_player(int player, int stick, int x, int y);
void psxe_host_reset_pad_state(void);
/* [input] multitap and [emulation] rewind / runahead. See psx/rewind.h and
   psx/input/multitap.h; every one of these is applied on the emulation thread. */
void psxe_host_set_multitap(int enabled);
void psxe_host_set_rewind(int enabled, int seconds, int frequency);
void psxe_host_set_runahead(int frames);
void psxe_host_set_rewind_active(int active);
void psxe_host_rewind_step(void);
unsigned int psxe_host_rewind_snapshot_bytes(void);
unsigned int psxe_host_rewind_bytes_used(void);
}

#define ARMSX_JNI_TAG "ARMSX-JNI"
#define ARMSX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ARMSX_JNI_TAG, __VA_ARGS__)
#define ARMSX_LOGW(...) __android_log_print(ANDROID_LOG_WARN, ARMSX_JNI_TAG, __VA_ARGS__)
#define ARMSX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ARMSX_JNI_TAG, __VA_ARGS__)

namespace {

// --- Surface state (UI thread writes, emulation thread reads) --------------------------

std::mutex g_surface_mutex;
ANativeWindow* g_native_window = nullptr;
int g_surface_width = 0;
int g_surface_height = 0;

// What was last handed to the presentation backends (armsx_render_set_native_window). Kept so a
// surfaceChanged that reports the SAME window at the SAME size does not bump the generation:
// every bump costs the active backend a VkSurfaceKHR/EGLSurface + swapchain rebuild, and a
// layout pass that changed nothing must not buy one. Guarded by g_surface_mutex.
ANativeWindow* g_published_window = nullptr;
int g_published_width = 0;
int g_published_height = 0;

// Framebuffer geometry. Fixed for the duration of one run so the core's SDL_Renderer and its
// textures stay valid; later surface resizes only re-apply the buffers geometry and let
// SurfaceFlinger rescale. (Gameplay is orientation-locked, so this is a scale, not a stretch.)
int g_fb_width = 0;
int g_fb_height = 0;

// ★ "The window's buffer geometry is not known to match g_fb_width/g_fb_height."
//
// The bridge blits with an UNSCALED row copy, so it is only correct while the dequeued buffer
// is exactly the framebuffer size — SurfaceFlinger then scales that buffer up to the window.
// Nothing else in the process guarantees that, because a GPU backend resets the geometry the
// moment it binds: render_gl.cpp CreateEglContext() calls
//
//     ANativeWindow_setBuffersGeometry(win, 0, 0, native_visual)
//
// and (0,0) means "revert to the window's NATURAL size" — 1920x1080 on a 1080p handheld, while
// this framebuffer stays at the 720p cap. When that backend then fails or is switched away
// from, the fallback ladder lands on the bridge with the geometry still reverted, and the
// unscaled copy puts a 1280x720 picture in the TOP-LEFT of a 1920x1080 buffer with the right
// third and bottom third left black. That was the whole bug: a uniform 2/3 top-left inset,
// identical at every source resolution because it is a property of the WINDOW, not the frame.
//
// ApplyBuffersGeometryLocked() could not repair it — it stands down while the window is
// claimed, and its only callers are CreateHostWindowAndRenderer() (once per run) and
// onNativeSurfaceChanged() (a real SurfaceView layout pass, which never happens in a
// landscape-locked session). So the bridge re-asserts its own geometry instead, from the one
// place that knows it is genuinely presenting.
std::atomic_bool g_bridge_geometry_dirty{true};

// Last mismatch reported by the bridge, so a device that genuinely refuses the geometry logs
// once per distinct pair instead of once per frame. Bridge thread only.
int g_last_mismatch_buffer_w = 0;
int g_last_mismatch_buffer_h = 0;

// --- Run state --------------------------------------------------------------------------

std::atomic_bool g_run_active{false};
SDL_Window* g_sdl_window = nullptr;
SDL_Renderer* g_sdl_renderer = nullptr;
SDL_Surface* g_sdl_surface = nullptr;
bool g_window_is_native = false; // SDL_CreateWindowFrom() succeeded: SDL presents on its own.
bool g_owns_sdl_video = false;

// App files dir, resolved once per run in runVMThread(). Save states hang off it
// (<files_dir>/savestates/), and those JNI calls arrive on the UI thread, so it is cached here
// rather than re-resolved through the Context on every slot press.
std::mutex g_files_dir_mutex;
std::string g_files_dir;

std::string CachedFilesDir() {
    std::lock_guard<std::mutex> lock(g_files_dir_mutex);
    return g_files_dir;
}

// --- Custom Vulkan driver (adrenotools) ----------------------------------------------------
//
// CustomDriver.kt installs an adrenotools driver pack (meta.json + libvulkan_freedreno.so)
// under filesDir/drivers/<id>/ and calls setCustomVulkanDriver() with that directory, the
// library name, a writable redirect directory for the driver's shader cache, and the app's
// nativeLibraryDir (where adrenotools' hook .so's would live).
//
// The selection is turned into an armsx_vk_library_open_fn and handed to the Vulkan backend
// through armsx_render_set_vulkan_loader(). libvulkan is NEVER linked; it is dlopen()ed
// behind that hook precisely so this substitution is possible.
//
// TWO loading strategies, in order:
//
//   1. adrenotools — dlopen("libadrenotools.so") and call adrenotools_open_libvulkan(). This
//      is the correct route: it loads the SYSTEM loader inside a patched linker namespace so
//      the vendor ICD resolves to the custom one, and redirects the driver's file IO out of
//      /system. It also needs adrenotools' hook libraries (libmain_hook.so /
//      libopengl_hook.so) next to it. NONE of those binaries are in this project yet, so
//      today this path simply does not fire — it is resolved by name so that dropping
//      libadrenotools.so + its hooks into jniLibs turns it on with no code change.
//
//   2. Direct dlopen of the driver .so. Mesa's Android Vulkan drivers (Turnip and friends)
//      export the full vkGetInstanceProcAddr entry point, not just the ICD negotiation
//      symbols, so opening one directly gives a usable Vulkan implementation. This is what
//      actually runs today. It gets no file-redirect hook, so the driver's shader cache may
//      be disabled or go to the driver's own default; correctness is unaffected.
//
// A driver that fails BOTH is not an error the user can be left guessing about: the loader
// returns NULL, the Vulkan backend falls back to the system loader, and the reported renderer
// name deliberately does NOT say "custom driver" (frontend/render_vk.cpp).

std::mutex g_vk_driver_mutex;
std::string g_vk_driver_dir;
std::string g_vk_driver_name;
std::string g_vk_driver_redirect_dir;
std::string g_vk_driver_hook_dir;

/*
    Feature flags, taken from the REAL header now that adrenotools is vendored.

    These used to be mirrored by hand "because the header is not vendored here", and the mirror
    was wrong: ADRENOTOOLS_DRIVER_CUSTOM is 1 << 0, not 1 << 2. 1 << 2 is
    ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT. So every call asked adrenotools to import a GPU
    mapping — with a null mapping — and never once asked it to load a custom driver, which is
    exactly what it then did not do. Copying constants out of a header is how that happens;
    include the header.
*/
#if defined(ARMSX_HAVE_ADRENOTOOLS)
#include <adrenotools/priv.h>
constexpr int kAdrenotoolsDriverFileRedirect = ADRENOTOOLS_DRIVER_FILE_REDIRECT;
constexpr int kAdrenotoolsDriverCustom = ADRENOTOOLS_DRIVER_CUSTOM;
#else
constexpr int kAdrenotoolsDriverFileRedirect = 1 << 1;
constexpr int kAdrenotoolsDriverCustom = 1 << 0;
#endif

using AdrenotoolsOpenLibvulkanFn = void* (*)(int dlopen_flags, int feature_flags,
                                             const char* tmp_lib_dir, const char* hook_lib_dir,
                                             const char* custom_driver_dir,
                                             const char* custom_driver_name,
                                             const char* file_redirect_dir,
                                             void* user_mapping_handle);

#if defined(ARMSX_HAVE_ADRENOTOOLS)
/*
    adrenotools is now LINKED IN (third_party/libadrenotools, BSD 2-Clause), not dlopen()ed.

    The dlopen("libadrenotools.so") route only ever worked if someone dropped a prebuilt shared
    object next to the app, which nobody had — so the code fell through to a direct dlopen of
    the driver, which cannot work: a Turnip pack links against libcutils and other system
    libraries that an app's classloader namespace does not resolve. That is the whole reason
    adrenotools exists.

    Declared here rather than by including <adrenotools/driver.h> so this file keeps compiling
    unchanged when the library is absent (ARMSX_HAVE_ADRENOTOOLS undefined).
*/
extern "C" void* adrenotools_open_libvulkan(int dlopenMode, int featureFlags,
                                            const char* tmpLibDir, const char* hookLibDir,
                                            const char* customDriverDir,
                                            const char* customDriverName,
                                            const char* fileRedirectDir,
                                            void** userMappingHandle);
#endif

void* OpenCustomVulkanDriver(void* /*user*/) {
    std::string driver_dir;
    std::string driver_name;
    std::string redirect_dir;
    std::string hook_dir;
    {
        std::lock_guard<std::mutex> lock(g_vk_driver_mutex);
        driver_dir = g_vk_driver_dir;
        driver_name = g_vk_driver_name;
        redirect_dir = g_vk_driver_redirect_dir;
        hook_dir = g_vk_driver_hook_dir;
    }

    if (driver_dir.empty() || driver_name.empty()) {
        return nullptr; // System loader.
    }

#if defined(ARMSX_HAVE_ADRENOTOOLS)
    // 0. adrenotools, LINKED IN. This is the route that actually works.
    {
        const std::string tmp_dir = CachedFilesDir();
        int flags = kAdrenotoolsDriverCustom;

        if (!redirect_dir.empty()) {
            flags |= kAdrenotoolsDriverFileRedirect;
        }

        /*
            Matched to ARMSX2's working invocation (VKLoader.cpp), which loads this exact driver
            on this exact device. Two differences were enough to break it:

            RTLD_NOW WITHOUT RTLD_LOCAL. adrenotools maps the driver into a bypassed linker
            namespace and the Vulkan loader has to see its symbols; RTLD_LOCAL keeps them
            private to the handle and defeats the point.

            tmpLibDir = nullptr, NOT a real directory. The parameter is the API-<29 fallback:
            passing a path forces adrenotools down the copy-the-hook-to-disk route, while NULL
            selects the memfd path, which is what every modern device wants. Handing it a valid
            writable directory looks helpful and is the opposite.
        */
        void* handle = adrenotools_open_libvulkan(
            RTLD_NOW, flags,
            nullptr,
            hook_dir.empty() ? nullptr : hook_dir.c_str(),
            driver_dir.c_str(), driver_name.c_str(),
            redirect_dir.empty() ? nullptr : redirect_dir.c_str(),
            nullptr);

        if (handle) {
            ARMSX_LOGI("Custom Vulkan driver loaded via adrenotools: %s%s", driver_dir.c_str(),
                       driver_name.c_str());
            return handle;
        }

        /* hookLibDir is the usual culprit when this fails: adrenotools dlopens libmain_hook.so
           and friends BY NAME out of that directory, so they have to be packaged into the app's
           nativeLibraryDir (build.sh stages them). Fall through and try the direct dlopen, which
           still works for the rare driver that links nothing from /system. */
        /* Log every input, not just the hook dir. adrenotools returns a bare NULL with no
           reason, so the only way to tell "wrong path" from "unsupported device" is to see
           exactly what it was handed. tmpLibDir in particular must be a writable directory —
           adrenotools COPIES the hook into it — and it is empty until the JNI has been told
           the files dir. */
        ARMSX_LOGW("adrenotools_open_libvulkan failed. driver=%s%s hookLibDir=%s tmpLibDir=%s "
                   "fileRedirectDir=%s flags=0x%x; trying a direct dlopen",
                   driver_dir.c_str(), driver_name.c_str(),
                   hook_dir.empty() ? "(EMPTY)" : hook_dir.c_str(),
                   "(null, memfd)",
                   redirect_dir.empty() ? "(EMPTY)" : redirect_dir.c_str(), flags);
    }
#endif

    // 1. adrenotools as a separate .so, if someone ships one.
    if (void* adrenotools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL)) {
        auto open_libvulkan = reinterpret_cast<AdrenotoolsOpenLibvulkanFn>(
            dlsym(adrenotools, "adrenotools_open_libvulkan"));
        if (open_libvulkan) {
            const std::string tmp_dir = CachedFilesDir();
            int flags = kAdrenotoolsDriverCustom;
            if (!redirect_dir.empty()) {
                flags |= kAdrenotoolsDriverFileRedirect;
            }
            void* handle = open_libvulkan(RTLD_NOW | RTLD_LOCAL, flags,
                                          tmp_dir.empty() ? nullptr : tmp_dir.c_str(),
                                          hook_dir.empty() ? nullptr : hook_dir.c_str(),
                                          driver_dir.c_str(), driver_name.c_str(),
                                          redirect_dir.empty() ? nullptr : redirect_dir.c_str(),
                                          nullptr);
            if (handle) {
                ARMSX_LOGI("Custom Vulkan driver loaded via adrenotools: %s%s", driver_dir.c_str(),
                           driver_name.c_str());
                return handle;
            }
            ARMSX_LOGW("adrenotools_open_libvulkan failed for %s%s; trying a direct dlopen",
                       driver_dir.c_str(), driver_name.c_str());
        } else {
            ARMSX_LOGW("libadrenotools.so has no adrenotools_open_libvulkan");
        }
        dlclose(adrenotools);
    }

    // 2. Direct dlopen of the driver itself.
    const std::string path = driver_dir + driver_name;
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        /* dlerror() CONSUMES the message: calling it twice in one expression (once to test,
           once to print) returns the real reason and then NULL, and the log said
           "could not be opened: (null)" — which is exactly as useless as no log at all.
           Read it ONCE. */
        const char* why = dlerror();
        ARMSX_LOGE("Custom Vulkan driver %s could not be opened: %s", path.c_str(),
                   why ? why : "(no error reported)");
        return nullptr;
    }

    // A driver pack that is not actually a Vulkan implementation must not be handed to the
    // backend as one — it would fail later and much less legibly.
    if (!dlsym(handle, "vkGetInstanceProcAddr")) {
        ARMSX_LOGE("Custom Vulkan driver %s exports no vkGetInstanceProcAddr; ignoring it.",
                   path.c_str());
        dlclose(handle);
        return nullptr;
    }

    ARMSX_LOGI("Custom Vulkan driver loaded by direct dlopen: %s", path.c_str());
    return handle;
}

// Cap on the CPU-side framebuffer. A 1080p+ software blit twice per frame is the single most
// expensive thing in this path, so the framebuffer is scaled down (aspect preserved) and the
// compositor scales it back up. Override with ARMSX_ANDROID_FB_HEIGHT.
constexpr int kDefaultMaxFramebufferHeight = 720;

int MaxFramebufferHeight() {
    if (const char* override_value = std::getenv("ARMSX_ANDROID_FB_HEIGHT")) {
        const int parsed = std::atoi(override_value);
        if (parsed >= 120 && parsed <= 4320) {
            return parsed;
        }
    }
    return kDefaultMaxFramebufferHeight;
}

void ComputeFramebufferSize(int surface_width, int surface_height, int* out_width, int* out_height) {
    // The rule itself lives in render.cpp so a host-side gate can pin it against the buffer
    // geometry it has to agree with; see armsx_render_host_framebuffer_size().
    armsx_render_host_framebuffer_size(surface_width, surface_height, MaxFramebufferHeight(),
                                       out_width, out_height);
}

// Must be called with g_surface_mutex held.
void ApplyBuffersGeometryLocked() {
    if (!g_native_window || g_window_is_native) {
        return;
    }

    // A GPU backend that owns this Surface has already set the buffers geometry to match its
    // EGLConfig / swapchain. Overwriting it with the blit-bridge framebuffer size would
    // invalidate its window surface.
    if (armsx_render_native_window_claimed()) {
        return;
    }

    int width = g_fb_width;
    int height = g_fb_height;
    if (width <= 0 || height <= 0) {
        ComputeFramebufferSize(g_surface_width, g_surface_height, &width, &height);
    }

    ANativeWindow_setBuffersGeometry(g_native_window, width, height, WINDOW_FORMAT_RGBX_8888);
}

// --- Present bridge ----------------------------------------------------------------------

// Called by the emulation loop once per frame, right after it presented into our software
// renderer. g_surface_mutex is only held long enough to take a reference; the blocking
// ANativeWindow_lock() happens outside it so a Surface teardown on the UI thread can never
// be stuck behind a stalled buffer queue.
void PresentToSurface(void* /*user*/) {
    if (g_window_is_native) {
        return; // SDL owns presentation in that mode.
    }

    // A GL/Vulkan backend bound to this Surface posts its own buffers (eglSwapBuffers /
    // vkQueuePresentKHR). The core also stops calling us in that case; this is the second
    // line of defence against two producers on one buffer queue.
    if (armsx_render_native_window_claimed()) {
        // That backend owns the geometry now and will have reset it to the window's natural
        // size. Whenever the bridge gets the window back, it has to put its own size back
        // first — see g_bridge_geometry_dirty.
        g_bridge_geometry_dirty.store(true, std::memory_order_release);
        return;
    }

    ANativeWindow* window = nullptr;
    SDL_Surface* surface = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_surface_mutex);
        if (!g_native_window || !g_sdl_surface) {
            return;
        }

        // Re-assert the bridge's buffer geometry before the first blit of a stretch it owns.
        // Cheap: one call per hand-off, not per frame.
        if (g_bridge_geometry_dirty.exchange(false, std::memory_order_acq_rel)) {
            ApplyBuffersGeometryLocked();
            psxe_diag_logf("renderer", "blit bridge re-applied buffer geometry %dx%d",
                           g_fb_width, g_fb_height);
        }

        window = g_native_window;
        surface = g_sdl_surface;
        ANativeWindow_acquire(window);
    }

    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(window, &buffer, nullptr) == 0) {
        if (buffer.width > 0 && buffer.height > 0 && surface->w > 0 && surface->h > 0 &&
            buffer.bits && surface->pixels) {
            const size_t dst_pitch = static_cast<size_t>(buffer.stride) * 4u;
            const size_t src_pitch = static_cast<size_t>(surface->pitch);
            auto* dst = static_cast<uint8_t*>(buffer.bits);
            const auto* src = static_cast<const uint8_t*>(surface->pixels);

            if (buffer.width == surface->w && buffer.height == surface->h) {
                // The contract holds: a straight row copy, and SurfaceFlinger scales the
                // buffer to the window. This is the only path that should ever run.
                const size_t row_bytes = static_cast<size_t>(surface->w) * 4u;
                for (int row = 0; row < surface->h; ++row) {
                    std::memcpy(dst + (static_cast<size_t>(row) * dst_pitch),
                                src + (static_cast<size_t>(row) * src_pitch), row_bytes);
                }
            } else {
                // ★ The sizes disagree, so this frame CANNOT be posted as a row copy.
                //
                // The old code clamped both axes with std::min() and copied anyway, which is
                // what turned a geometry mismatch into a silent 2/3 top-left inset instead of
                // an obvious failure. Fill the buffer instead — nearest-neighbour, which is
                // exactly what the compositor would have done — so the picture is always
                // whole. Slower, and deliberately loud, but never wrong.
                //
                // Reached only when ANativeWindow_setBuffersGeometry did not take: the
                // re-assert above fixes the case this was found in, and this keeps a device
                // that refuses the request working rather than half-black.
                if (buffer.width != g_last_mismatch_buffer_w ||
                    buffer.height != g_last_mismatch_buffer_h) {
                    g_last_mismatch_buffer_w = buffer.width;
                    g_last_mismatch_buffer_h = buffer.height;
                    ARMSX_LOGW("Blit bridge: buffer %dx%d != framebuffer %dx%d; scaling. "
                               "ANativeWindow_setBuffersGeometry did not take.",
                               buffer.width, buffer.height, surface->w, surface->h);
                    psxe_diag_logf("renderer",
                                   "blit bridge size mismatch buffer=%dx%d framebuffer=%dx%d "
                                   "(scaling fallback)",
                                   buffer.width, buffer.height, surface->w, surface->h);
                }

                for (int row = 0; row < buffer.height; ++row) {
                    const int src_row =
                        static_cast<int>((static_cast<int64_t>(row) * surface->h) / buffer.height);
                    const auto* src_line = reinterpret_cast<const uint32_t*>(
                        src + (static_cast<size_t>(src_row) * src_pitch));
                    auto* dst_line =
                        reinterpret_cast<uint32_t*>(dst + (static_cast<size_t>(row) * dst_pitch));
                    for (int col = 0; col < buffer.width; ++col) {
                        const int src_col = static_cast<int>(
                            (static_cast<int64_t>(col) * surface->w) / buffer.width);
                        dst_line[col] = src_line[src_col];
                    }
                }
            }
        }
        ANativeWindow_unlockAndPost(window);
    }

    ANativeWindow_release(window);
}

// --- SDL bring-up ------------------------------------------------------------------------

// Register SDL's org.libsdl.app.SDLAudioManager glue.
//
// SDL_RunAudio()/SDL_CaptureAudio() call Android_JNI_AudioSetThreadPriority() unconditionally
// on every audio thread, whichever backend is in use (src/audio/SDL_audio.c). That is
//   CallStaticVoidMethod(env, mAudioManagerClass, midAudioSetThreadPriority, ...)
// and mAudioManagerClass is only assigned by SDLAudioManager.nativeSetupJNI(), which SDLActivity
// normally triggers. In-process there is no SDLActivity, so the very first audio callback aborted
// the *whole process* with "CallStaticVoidMethod received NULL jclass" — from a thread SDL owns,
// so there is nothing to catch. There is no hint or backend choice that avoids that call.
//
// libSDL2.so's own JNI_OnLoad already did RegisterNatives() for org/libsdl/app/SDLAudioManager, so
// the native method is wired up; it simply was never invoked. Invoking it ourselves fills in
// mAudioManagerClass + the method IDs. SDLAudioManager.audioSetThreadPriority() only touches
// Thread.currentThread() and android.os.Process — no Context, no SDLActivity singleton — so it is
// safe with Compose owning the Activity.
//
// Deliberately NOT done for SDLActivity/SDLControllerManager: their statics dereference
// SDLActivity.mSingleton / SDL.getContext(), which really are null here. Audio glue only.
void EnsureSdlAudioJniGlue(JNIEnv* env) {
    static bool attempted = false;
    if (attempted || !env) {
        return;
    }
    attempted = true;

    auto clear_exception = [env]() {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    };

    jclass audio_manager = env->FindClass("org/libsdl/app/SDLAudioManager");
    if (!audio_manager) {
        clear_exception();
        ARMSX_LOGW("org.libsdl.app.SDLAudioManager not found; SDL audio would abort — audio disabled");
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        return;
    }

    jmethodID setup = env->GetStaticMethodID(audio_manager, "nativeSetupJNI", "()I");
    if (!setup) {
        clear_exception();
        env->DeleteLocalRef(audio_manager);
        ARMSX_LOGW("SDLAudioManager.nativeSetupJNI missing; SDL audio would abort — audio disabled");
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        return;
    }

    env->CallStaticIntMethod(audio_manager, setup);
    if (env->ExceptionCheck()) {
        clear_exception();
        env->DeleteLocalRef(audio_manager);
        ARMSX_LOGW("SDLAudioManager.nativeSetupJNI threw; audio disabled");
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        return;
    }

    // nativeSetupJNI() fills in mAudioManagerClass and the method IDs, but NOT
    // SDLAudioManager.mContext — SDLActivity normally sets that separately. Everything the
    // openslES path touches is happy without it, so this is not on the critical path; it is
    // what makes the OTHER backends selectable. Android_DetectDevices(), which SDL_AudioInit()
    // calls unconditionally for aaudio, goes
    //   SDLAudioManager.getAudioOutputDevices() -> mContext.getSystemService(AUDIO_SERVICE)
    // and a null mContext there is an NPE thrown back into a JNI frame that does not check —
    // i.e. a hard abort, not a failed init. setContext() only assigns a static field, so it is
    // safe to do unconditionally; the [audio] driver setting stays pinned to openslES unless
    // it lands.
    bool context_ready = false;
    jclass native_app = env->FindClass("kr/co/iefriends/pcsx2/NativeApp");
    jmethodID get_context = native_app
        ? env->GetStaticMethodID(native_app, "getContext", "()Landroid/content/Context;")
        : nullptr;
    jmethodID set_context = env->GetStaticMethodID(audio_manager, "setContext", "(Landroid/content/Context;)V");
    clear_exception();

    if (get_context && set_context) {
        jobject context = env->CallStaticObjectMethod(native_app, get_context);
        clear_exception();

        if (context) {
            env->CallStaticVoidMethod(audio_manager, set_context, context);
            if (env->ExceptionCheck()) {
                clear_exception();
            } else {
                context_ready = true;
            }
            env->DeleteLocalRef(context);
        }
    }

    if (native_app) {
        env->DeleteLocalRef(native_app);
    }

    env->DeleteLocalRef(audio_manager);
    psxe_host_set_audio_backends_ready(context_ready ? 1 : 0);
    ARMSX_LOGI("SDL audio JNI glue registered (org.libsdl.app.SDLAudioManager), context=%s",
               context_ready ? "ok" : "unavailable (openslES only)");
}

bool EnsureSdlVideo() {
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        return true;
    }

    // SDL's `android` video driver resolves its window (and its DPI, and its event pump)
    // through org.libsdl.app.SDLActivity's static JNI glue, which is never set up when the
    // Compose Activity owns the screen — initialising it would dereference a null jclass.
    // The dummy driver gives us a valid SDL_Window handle with no display of its own, which
    // is exactly what the blit bridge wants. setenv(..., 0) so an explicit
    // SDL_VIDEODRIVER from the environment still wins.
    setenv("SDL_VIDEODRIVER", "dummy", 0);

    // Audio driver: openslES unless the [audio] driver setting says otherwise, and the core
    // only honours that setting once EnsureSdlAudioJniGlue() has reported the Java side is
    // fully wired (ArmsxApp::applyAudioDriverSetting, frontend/main.cpp). This is the floor.
    //
    // Every other Android audio backend SDL ships reaches org.libsdl.app.SDLAudioManager's
    // static JNI glue, which SDLActivity.nativeSetupJNI() would normally register — and with
    // Compose owning the Activity there is no SDLActivity to do it:
    //
    //   * "android"  — drives java AudioTrack directly. Aborts on the first call.
    //   * "aaudio"   — the stream itself is pure native, BUT SDL wires
    //                  impl->DetectDevices = Android_DetectDevices (src/audio/aaudio/SDL_aaudio.c),
    //                  and SDL_AudioInit() unconditionally calls DetectDevices() right after the
    //                  driver initialises. Android_DetectDevices does
    //                  CallStaticObjectMethod(env, mAudioManagerClass, midGetAudioOutputDevices)
    //                  with mAudioManagerClass == NULL -> "JNI DETECTED ERROR IN APPLICATION:
    //                  CallStaticObjectMethod received NULL jclass", i.e. a hard SIGABRT inside
    //                  SDL_InitSubSystem(SDL_INIT_AUDIO). That is what killed the armsx-vm thread
    //                  immediately after the core logged "settings loaded".
    //
    // EnsureSdlAudioJniGlue() now registers that glue AND hands SDLAudioManager a Context, so
    // aaudio is reachable — but only when both of those actually landed, and only when the
    // user asked for it. openslES leaves impl->DetectDevices unset entirely
    // (src/audio/openslES/SDL_openslES.c line ~732 keeps it commented out), so SDL_AudioInit()
    // substitutes its no-op stub and the whole driver stays inside the native OpenSL ES API.
    setenv("SDL_AUDIODRIVER", "openslES", 1);

    // We are initialising SDL from a plain JNI thread, NOT from SDL_main. Without this, SDL
    // refuses to init with "Application didn't initialize properly, did you include SDL_main.h
    // in the file containing your main() function?" — which is exactly what happened: the VM
    // thread died immediately and the UI snapped back to the library.
    SDL_SetMainReady();

    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        ARMSX_LOGE("SDL video init failed: %s", SDL_GetError());
        return false;
    }

    g_owns_sdl_video = true;
    ARMSX_LOGI("SDL video driver: %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
    return true;
}

// Build the SDL_Window / SDL_Renderer pair the core will adopt. Returns false when there is
// no usable surface.
bool CreateHostWindowAndRenderer() {
    ANativeWindow* window = nullptr;
    int surface_width = 0;
    int surface_height = 0;
    {
        std::lock_guard<std::mutex> lock(g_surface_mutex);
        window = g_native_window;
        surface_width = g_surface_width;
        surface_height = g_surface_height;
        if (window) {
            ANativeWindow_acquire(window);
        }
    }

    if (!window) {
        ARMSX_LOGE("runVMThread: no Surface yet — call onNativeSurfaceChanged first");
        return false;
    }

    if (surface_width <= 0 || surface_height <= 0) {
        surface_width = ANativeWindow_getWidth(window);
        surface_height = ANativeWindow_getHeight(window);
    }

    if (!EnsureSdlVideo()) {
        ANativeWindow_release(window);
        return false;
    }

    // 1) The direct route, per the design: hand SDL the ANativeWindow. Unsupported by SDL's
    //    Android/dummy backends today, so this is expected to fail — kept so the moment a
    //    backend implements CreateSDLWindowFrom the accelerated path lights up for free.
    g_window_is_native = false;
    g_sdl_window = SDL_CreateWindowFrom(window);
    if (g_sdl_window) {
        g_window_is_native = true;
        ARMSX_LOGI("Adopted ANativeWindow via SDL_CreateWindowFrom");
        g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, -1, SDL_RENDERER_ACCELERATED);
        if (!g_sdl_renderer) {
            ARMSX_LOGW("Accelerated renderer failed (%s), trying software", SDL_GetError());
            g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (g_sdl_renderer) {
            ANativeWindow_release(window);
            return true;
        }

        ARMSX_LOGW("No renderer for the adopted window (%s), falling back", SDL_GetError());
        SDL_DestroyWindow(g_sdl_window);
        g_sdl_window = nullptr;
        g_window_is_native = false;
    } else {
        ARMSX_LOGI("SDL_CreateWindowFrom unsupported (%s), using the blit bridge", SDL_GetError());
    }

    // 2) Publish the raw ANativeWindow for the GPU presentation backends. When settings.toml
    //    asks for gpu_backend = "opengl" (or "vulkan"), the core binds EGL / a VkSurfaceKHR
    //    straight to it and never touches the software renderer built below; see
    //    ArmsxApp::initializeWindowAndRenderer(). The blit bridge is still constructed so the
    //    fallback ladder always has somewhere to land.
    armsx_render_set_native_window(window, surface_width, surface_height);

    // 3) Blit bridge: offscreen framebuffer + software renderer, posted by PresentToSurface().
    ComputeFramebufferSize(surface_width, surface_height, &g_fb_width, &g_fb_height);
    // A new framebuffer size the window has not been told about yet. Re-armed here rather than
    // relying on the static initialiser because the host keeps the library loaded between
    // games, so this runs many times per process.
    g_bridge_geometry_dirty.store(true, std::memory_order_release);
    g_last_mismatch_buffer_w = 0;
    g_last_mismatch_buffer_h = 0;
    ARMSX_LOGI("Blit bridge: surface %dx%d framebuffer %dx%d",
               surface_width, surface_height, g_fb_width, g_fb_height);

    g_sdl_window = SDL_CreateWindow("ARMSX", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                    g_fb_width, g_fb_height, 0);
    if (!g_sdl_window) {
        ARMSX_LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        ANativeWindow_release(window);
        return false;
    }

    // RGBA32 is byte order R,G,B,A on little-endian, matching WINDOW_FORMAT_RGBX_8888, so the
    // per-row copy in PresentToSurface() is a straight memcpy with no swizzle.
    g_sdl_surface = SDL_CreateRGBSurfaceWithFormat(0, g_fb_width, g_fb_height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!g_sdl_surface) {
        ARMSX_LOGE("Framebuffer allocation failed: %s", SDL_GetError());
        SDL_DestroyWindow(g_sdl_window);
        g_sdl_window = nullptr;
        ANativeWindow_release(window);
        return false;
    }
    SDL_FillRect(g_sdl_surface, nullptr, SDL_MapRGBA(g_sdl_surface->format, 0, 0, 0, 255));

    g_sdl_renderer = SDL_CreateSoftwareRenderer(g_sdl_surface);
    if (!g_sdl_renderer) {
        ARMSX_LOGE("SDL_CreateSoftwareRenderer failed: %s", SDL_GetError());
        SDL_FreeSurface(g_sdl_surface);
        g_sdl_surface = nullptr;
        SDL_DestroyWindow(g_sdl_window);
        g_sdl_window = nullptr;
        ANativeWindow_release(window);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_surface_mutex);
        ApplyBuffersGeometryLocked();
    }

    ANativeWindow_release(window);
    return true;
}

void DestroyHostWindowAndRenderer() {
    // Stop the present callback before anything it reads goes away.
    psxe_host_set_present_callback(nullptr, nullptr);

    // The core has already torn its renderer down by the time we get here, so the
    // ANativeWindow must stop being advertised to the presentation backends.
    armsx_render_set_native_window(nullptr, 0, 0);

    {
        std::lock_guard<std::mutex> lock(g_surface_mutex);
        // Keep the published-window mirror truthful, or the next surfaceChanged that reports
        // the same (still perfectly live) Surface would be deduped against a slot this
        // teardown has already nulled.
        g_published_window = nullptr;
        g_published_width = 0;
        g_published_height = 0;
        // The core already destroyed the renderer/window it owns? No: with an external
        // window/renderer the core never destroys them (owns_window_/owns_renderer_ stay
        // false), so teardown is ours.
        if (g_sdl_renderer) {
            SDL_DestroyRenderer(g_sdl_renderer);
            g_sdl_renderer = nullptr;
        }
        if (g_sdl_surface) {
            SDL_FreeSurface(g_sdl_surface);
            g_sdl_surface = nullptr;
        }
        if (g_sdl_window) {
            SDL_DestroyWindow(g_sdl_window);
            g_sdl_window = nullptr;
        }
        g_window_is_native = false;
        g_fb_width = 0;
        g_fb_height = 0;
    }

    if (g_owns_sdl_video) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        g_owns_sdl_video = false;
    }
}

// --- Small JNI helpers --------------------------------------------------------------------

std::string JStringToUtf8(JNIEnv* env, jstring value) {
    if (!env || !value) {
        return {};
    }

    const char* utf = env->GetStringUTFChars(value, nullptr);
    if (!utf) {
        return {};
    }

    std::string result(utf);
    env->ReleaseStringUTFChars(value, utf);
    return result;
}

// SDL_GetPrefPath() goes through org.libsdl.app.SDLActivity, which is not in play here, so the
// core would end up with a null pref path (no settings.toml, no memcards). Resolve the app's
// files dir straight off the Context that NativeApp already holds. Same directory
// SDL_GetPrefPath("nanodata", "armsx") returns under the SDL activity, so both paths share
// one data folder.
std::string ResolveAppFilesDir(JNIEnv* env) {
    if (!env) {
        return {};
    }

    auto clear_exception = [env]() {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    };

    jclass native_app = env->FindClass("kr/co/iefriends/pcsx2/NativeApp");
    if (!native_app) {
        clear_exception();
        return {};
    }

    jmethodID get_context = env->GetStaticMethodID(native_app, "getContext", "()Landroid/content/Context;");
    if (!get_context) {
        clear_exception();
        env->DeleteLocalRef(native_app);
        return {};
    }

    jobject context = env->CallStaticObjectMethod(native_app, get_context);
    clear_exception();
    env->DeleteLocalRef(native_app);
    if (!context) {
        return {};
    }

    std::string result;
    jclass context_class = env->GetObjectClass(context);
    jmethodID get_files_dir = context_class
        ? env->GetMethodID(context_class, "getFilesDir", "()Ljava/io/File;")
        : nullptr;
    if (get_files_dir) {
        jobject files_dir = env->CallObjectMethod(context, get_files_dir);
        clear_exception();
        if (files_dir) {
            jclass file_class = env->GetObjectClass(files_dir);
            jmethodID get_absolute_path = file_class
                ? env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;")
                : nullptr;
            if (get_absolute_path) {
                auto path = static_cast<jstring>(env->CallObjectMethod(files_dir, get_absolute_path));
                clear_exception();
                if (path) {
                    result = JStringToUtf8(env, path);
                    env->DeleteLocalRef(path);
                }
            }
            if (file_class) {
                env->DeleteLocalRef(file_class);
            }
            env->DeleteLocalRef(files_dir);
        }
    } else {
        clear_exception();
    }

    if (context_class) {
        env->DeleteLocalRef(context_class);
    }
    env->DeleteLocalRef(context);
    return result;
}

// --- RetroAchievements host services -------------------------------------------------------
//
// frontend/achievements.cpp owns rc_client but deliberately has no transport of its own, so the
// two things it needs from Android are wired up here:
//
//   * HTTP. Each rc_client server call runs on a worker thread the achievements module spawns,
//     and that thread blocks in kr.co.iefriends.pcsx2.HttpClient.doRequest(). It is a plain JNI
//     thread, so it has to attach itself — the same reason SDL's audio glue exists above.
//   * Unlock sound. NativeApp.playSound() is fire-and-forget MediaPlayer playback; ARMSX ships
//     no RA sounds, so this only ever fires for a sound the user imported.
//
// Nothing here calls SDL, so it is all safe from the UI thread with no session running — which
// is the normal case, because the RetroAchievements screen is reachable from the library.

JavaVM* g_java_vm = nullptr;

// Cached global refs, resolved on first use from a thread that definitely has the app
// classloader (a raw AttachCurrentThread thread only sees the system one, where FindClass on an
// app class returns NoClassDefFoundError).
jclass g_http_client_class = nullptr;
jmethodID g_http_do_request = nullptr;
jfieldID g_http_status_field = nullptr;
jfieldID g_http_content_type_field = nullptr;
jfieldID g_http_data_field = nullptr;
bool g_http_glue_ready = false;

jclass g_native_app_class = nullptr;
jmethodID g_native_app_play_sound = nullptr;
jmethodID g_native_app_notice = nullptr;

// RAII attach for the achievements module's worker threads.
class ScopedJniThread {
  public:
    ScopedJniThread() {
        if (!g_java_vm) {
            return;
        }
        if (g_java_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_OK) {
            return;
        }
        if (g_java_vm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
            attached_ = true;
        } else {
            env_ = nullptr;
        }
    }

    ~ScopedJniThread() {
        if (attached_ && g_java_vm) {
            g_java_vm->DetachCurrentThread();
        }
    }

    ScopedJniThread(const ScopedJniThread&) = delete;
    ScopedJniThread& operator=(const ScopedJniThread&) = delete;

    JNIEnv* env() const { return env_; }

  private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

// Resolve HttpClient + NativeApp.playSound / NativeApp.onAchievementNotice. Must be called from a
// thread whose JNIEnv can see
// app classes — every RetroAchievements JNI entry point does, which is why this hangs off
// EnsureAchievementsReady() rather than off the worker threads.
void ResolveAchievementsGlue(JNIEnv* env) {
    if (g_http_glue_ready || !env) {
        return;
    }

    auto clear_exception = [env]() {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    };

    jclass http_local = env->FindClass("kr/co/iefriends/pcsx2/HttpClient");
    if (!http_local) {
        clear_exception();
        ARMSX_LOGE("HttpClient not found; RetroAchievements has no transport");
        return;
    }

    jclass response_local = env->FindClass("kr/co/iefriends/pcsx2/HttpClient$Response");
    if (!response_local) {
        clear_exception();
        env->DeleteLocalRef(http_local);
        ARMSX_LOGE("HttpClient$Response not found; RetroAchievements has no transport");
        return;
    }

    g_http_do_request = env->GetStaticMethodID(
        http_local, "doRequest",
        "(Ljava/lang/String;Ljava/lang/String;[BLjava/lang/String;I)Lkr/co/iefriends/pcsx2/HttpClient$Response;");
    g_http_status_field = env->GetFieldID(response_local, "statusCode", "I");
    g_http_content_type_field = env->GetFieldID(response_local, "contentType", "Ljava/lang/String;");
    g_http_data_field = env->GetFieldID(response_local, "data", "[B");
    clear_exception();

    if (!g_http_do_request || !g_http_status_field || !g_http_content_type_field || !g_http_data_field) {
        env->DeleteLocalRef(response_local);
        env->DeleteLocalRef(http_local);
        ARMSX_LOGE("HttpClient signature mismatch; RetroAchievements has no transport");
        return;
    }

    g_http_client_class = static_cast<jclass>(env->NewGlobalRef(http_local));
    env->DeleteLocalRef(response_local);
    env->DeleteLocalRef(http_local);

    jclass native_app_local = env->FindClass("kr/co/iefriends/pcsx2/NativeApp");
    if (native_app_local) {
        g_native_app_play_sound = env->GetStaticMethodID(native_app_local, "playSound", "(Ljava/lang/String;)V");
        clear_exception();
        g_native_app_notice = env->GetStaticMethodID(
            native_app_local, "onAchievementNotice",
            "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
        clear_exception();

        // Either one is reason enough to keep the class alive — losing the global ref because the
        // other went missing would take a working feature down with a broken one.
        if (g_native_app_play_sound || g_native_app_notice) {
            g_native_app_class = static_cast<jclass>(env->NewGlobalRef(native_app_local));
        }
        // Said out loud because a silently unresolved method is exactly how an achievements
        // integration ends up earning unlocks that never reach the screen.
        if (!g_native_app_notice) {
            ARMSX_LOGE("NativeApp.onAchievementNotice(int,String,String,String,String,int) not "
                       "found; RetroAchievements notifications will not be shown");
        }
        env->DeleteLocalRef(native_app_local);
    } else {
        clear_exception();
        ARMSX_LOGE("NativeApp not found; RetroAchievements has no notification channel");
    }

    g_http_glue_ready = true;
    ARMSX_LOGI("RetroAchievements transport ready (notifications=%s, sound=%s)",
               g_native_app_notice ? "yes" : "no",
               g_native_app_play_sound ? "yes" : "no");
}

void AchievementsHttpRequest(const char* url, const char* post_data, const char* /*content_type*/,
                             const char* user_agent, int timeout_ms,
                             armsx_ach_http_response* out, void* /*user*/) {
    if (!out) {
        return;
    }

    out->status_code = -1; // HttpClient's generic transport-failure sentinel

    if (!g_http_glue_ready || !g_http_client_class || !url) {
        return;
    }

    ScopedJniThread thread;
    JNIEnv* env = thread.env();
    if (!env) {
        return;
    }

    auto clear_exception = [env]() {
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    };

    jstring j_url = env->NewStringUTF(url);
    jstring j_method = env->NewStringUTF(post_data ? "POST" : "GET");
    jstring j_user_agent = env->NewStringUTF(user_agent ? user_agent : "");
    jbyteArray j_post = nullptr;
    if (post_data) {
        const jsize length = static_cast<jsize>(std::strlen(post_data));
        j_post = env->NewByteArray(length);
        if (j_post && length > 0) {
            env->SetByteArrayRegion(j_post, 0, length, reinterpret_cast<const jbyte*>(post_data));
        }
    }

    jobject response = env->CallStaticObjectMethod(g_http_client_class, g_http_do_request,
                                                   j_url, j_method, j_post, j_user_agent,
                                                   static_cast<jint>(timeout_ms));
    clear_exception();

    if (response) {
        out->status_code = static_cast<int>(env->GetIntField(response, g_http_status_field));

        auto content_type = static_cast<jstring>(env->GetObjectField(response, g_http_content_type_field));
        if (content_type) {
            out->content_type = JStringToUtf8(env, content_type);
            env->DeleteLocalRef(content_type);
        }

        auto data = static_cast<jbyteArray>(env->GetObjectField(response, g_http_data_field));
        if (data) {
            const jsize length = env->GetArrayLength(data);
            if (length > 0) {
                out->body.resize(static_cast<size_t>(length));
                env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte*>(out->body.data()));
            }
            env->DeleteLocalRef(data);
        }

        env->DeleteLocalRef(response);
    }

    if (j_post) {
        env->DeleteLocalRef(j_post);
    }
    env->DeleteLocalRef(j_user_agent);
    env->DeleteLocalRef(j_method);
    env->DeleteLocalRef(j_url);
}

void AchievementsPlaySound(const char* path, void* /*user*/) {
    if (!path || !*path || !g_native_app_class || !g_native_app_play_sound) {
        return;
    }

    ScopedJniThread thread;
    JNIEnv* env = thread.env();
    if (!env) {
        return;
    }

    jstring j_path = env->NewStringUTF(path);
    env->CallStaticVoidMethod(g_native_app_class, g_native_app_play_sound, j_path);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(j_path);
}

// The one route RetroAchievements has to something the user can actually see: signing in, the game
// summary at boot, an unlock, a leaderboard attempt. NativeApp.onAchievementNotice() hops to the
// main looper and hands the toast to com.armsx2.ui.RaToasts — the stacking notification host that
// WindowImpl mounts above every screen, in-game and library alike.
//
// [image_url] is an https RetroAchievements image; it is handed over as a URL rather than a
// downloaded file because the app already has one image pipeline (Coil, with the shared
// files/cover_cache disk cache) and a second downloader in native code would duplicate it.
//
// Called from the achievements module's notice pump, which runs on the emulation thread (a Java
// thread, so ScopedJniThread finds an env rather than attaching) with no achievements lock held.
// Never called from an rc_client callback directly — see the notice queue in achievements.cpp.
void AchievementsNotify(int kind, const char* key, const char* title, const char* detail,
                        const char* image_url, int duration_ms, void* /*user*/) {
    if (!title || !*title || !g_native_app_class || !g_native_app_notice) {
        return;
    }

    ScopedJniThread thread;
    JNIEnv* env = thread.env();
    if (!env) {
        return;
    }

    // Every string is passed non-null; the Kotlin side treats "" as absent. NewStringUTF(nullptr)
    // returns null, which would land as a Kotlin null on a non-null parameter and throw.
    jstring j_key = env->NewStringUTF(key ? key : "");
    jstring j_title = env->NewStringUTF(title);
    jstring j_detail = env->NewStringUTF(detail ? detail : "");
    jstring j_image = env->NewStringUTF(image_url ? image_url : "");

    env->CallStaticVoidMethod(g_native_app_class, g_native_app_notice, static_cast<jint>(kind),
                              j_key, j_title, j_detail, j_image, static_cast<jint>(duration_ms));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (j_image) {
        env->DeleteLocalRef(j_image);
    }
    if (j_detail) {
        env->DeleteLocalRef(j_detail);
    }
    if (j_title) {
        env->DeleteLocalRef(j_title);
    }
    if (j_key) {
        env->DeleteLocalRef(j_key);
    }
}

// Called at the top of every RetroAchievements JNI entry point. The screen is reachable from the
// library, i.e. before any run has started, so the pref path the achievements store lives under
// has to be resolved here too — psxe_cfg_set_pref_path() is a no-op once set, so runVMThread's
// later call agrees with this one instead of fighting it.
void EnsureAchievementsReady(JNIEnv* env) {
    static bool handlers_installed = false;

    if (env) {
        ResolveAchievementsGlue(env);

        std::string files_dir = CachedFilesDir();
        if (files_dir.empty()) {
            files_dir = ResolveAppFilesDir(env);
            if (!files_dir.empty()) {
                std::lock_guard<std::mutex> lock(g_files_dir_mutex);
                if (g_files_dir.empty()) {
                    g_files_dir = files_dir;
                }
            }
        }
        if (!files_dir.empty()) {
            psxe_cfg_set_pref_path(files_dir.c_str());
        }
    }

    if (!handlers_installed && g_http_glue_ready) {
        armsx_ach_set_http_handler(AchievementsHttpRequest, nullptr);
        armsx_ach_set_sound_handler(AchievementsPlaySound, nullptr);
        armsx_ach_set_notify_handler(AchievementsNotify, nullptr);
        handlers_installed = true;
    }

    armsx_ach_startup();
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    // Kept so the RetroAchievements HTTP/sound workers — plain pthreads the achievements module
    // spawns, with no Java frame above them — can attach themselves.
    g_java_vm = vm;
    return JNI_VERSION_1_6;
}

// ------------------------------------------------------------------------------------------
// kr.co.iefriends.pcsx2.NativeApp
// ------------------------------------------------------------------------------------------

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceCreated(JNIEnv*, jclass) {
    ARMSX_LOGI("onNativeSurfaceCreated");
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceChanged(JNIEnv* env, jclass, jobject surface, jint w, jint h) {
    ANativeWindow* window = (env && surface) ? ANativeWindow_fromSurface(env, surface) : nullptr;

    std::lock_guard<std::mutex> lock(g_surface_mutex);
    if (g_native_window) {
        ANativeWindow_release(g_native_window);
    }
    g_native_window = window;
    g_surface_width = w;
    g_surface_height = h;

    // Before a run starts the framebuffer follows the surface. Once the core owns a renderer
    // built on it, the geometry is frozen and only the compositor's scale changes.
    if (!g_run_active.load(std::memory_order_acquire)) {
        g_fb_width = 0;
        g_fb_height = 0;
    }

    ApplyBuffersGeometryLocked();

    /* Unconditionally, even though the call above usually did it: this is a BRAND NEW
       ANativeWindow with default geometry, and ApplyBuffersGeometryLocked() stands down while
       a GPU backend holds the claim. Without this the bridge would inherit the window at its
       natural size on every backend hand-off after a resume. */
    g_bridge_geometry_dirty.store(true, std::memory_order_release);

    /* ★ Tell the presentation backends the window changed.
       Android destroys the SurfaceView's ANativeWindow when the activity stops and hands back
       a BRAND NEW one on resume. render.cpp keeps a generation counter for exactly that, and
       render_vk.cpp / render_gl.cpp rebuild their VkSurfaceKHR / EGLSurface when it moves — but
       until this call existed the ONLY publisher was CreateHostWindowAndRenderer(), i.e. once
       per VM run. The counter therefore never moved inside a session, the rebuild never fired,
       and a resumed game kept presenting into the window that had already been destroyed: the
       loop runs, the OSD updates, and the picture is black until the game is restarted (which
       re-enters runVMThread and publishes the new window). Both backends take their own
       ANativeWindow reference, so publishing the raw pointer here is the same contract
       CreateHostWindowAndRenderer() already uses. */
    const bool republish = (window != g_published_window) ||
                           (static_cast<int>(w) != g_published_width) ||
                           (static_cast<int>(h) != g_published_height);

    if (republish) {
        g_published_window = window;
        g_published_width = static_cast<int>(w);
        g_published_height = static_cast<int>(h);
        armsx_render_set_native_window(window, static_cast<int>(w), static_cast<int>(h));
    }

    ARMSX_LOGI("onNativeSurfaceChanged %dx%d window=%p published=%s generation=%lu",
               static_cast<int>(w), static_cast<int>(h), static_cast<void*>(window),
               republish ? "yes" : "unchanged", armsx_render_native_window_generation());
    /* Also into the diag log: this is the lifecycle event that explains a black resume, and it
       is a once-per-transition line, not a per-frame one. */
    psxe_diag_logf("renderer", "host surface changed %dx%d window=%p published=%s generation=%lu",
                   static_cast<int>(w), static_cast<int>(h), static_cast<void*>(window),
                   republish ? "yes" : "unchanged", armsx_render_native_window_generation());
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceDestroyed(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_surface_mutex);

    /* Stop advertising the window BEFORE dropping our reference to it: a backend that reads
       the slot after this point gets null and holds off rebuilding until a real surface
       arrives, instead of building on an object that is going away. */
    g_published_window = nullptr;
    g_published_width = 0;
    g_published_height = 0;
    armsx_render_set_native_window(nullptr, 0, 0);

    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
    }
    g_surface_width = 0;
    g_surface_height = 0;
    ARMSX_LOGI("onNativeSurfaceDestroyed generation=%lu", armsx_render_native_window_generation());
    psxe_diag_logf("renderer", "host surface destroyed generation=%lu",
                   armsx_render_native_window_generation());
}

// Blocking: boots [path] and runs the emulation loop until shutdown(). Call on a dedicated
// thread, after onNativeSurfaceChanged has delivered a Surface. Returns true on a clean exit.
extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runVMThread(JNIEnv* env, jclass, jstring path) {
    bool expected = false;
    if (!g_run_active.compare_exchange_strong(expected, true)) {
        ARMSX_LOGE("runVMThread: a run is already active");
        return JNI_FALSE;
    }

    const std::string game_path = JStringToUtf8(env, path);

    const std::string files_dir = ResolveAppFilesDir(env);
    {
        std::lock_guard<std::mutex> lock(g_files_dir_mutex);
        g_files_dir = files_dir;
    }
    if (!files_dir.empty()) {
        psxe_cfg_set_pref_path(files_dir.c_str());
        ARMSX_LOGI("Pref path: %s", files_dir.c_str());
    } else {
        ARMSX_LOGW("Could not resolve the app files dir; the core will fall back to SDL_GetPrefPath");
    }

    // Must happen on this (Java-attached) thread, before SDL opens any audio device: the audio
    // thread SDL spawns later calls straight into this glue and aborts the process without it.
    EnsureSdlAudioJniGlue(env);

    // Same reason, for RetroAchievements: this resolves HttpClient (the RA transport) and
    // NativeApp.onAchievementNotice (the notification channel) through a JNIEnv that can see app
    // classes, which a bare AttachCurrentThread thread cannot.
    //
    // It used to run ONLY from the RetroAchievements JNI entry points, i.e. only if the user had
    // opened the achievements screen at some point in this process. A cold boot straight into a
    // game therefore reached armsx_ach_startup() with no transport installed at all: the saved
    // token login answered itself with a client error, the disc was never identified, and no
    // notification could be delivered because nothing had told the module where to send one.
    // Every session needs this, not just the ones that visit the screen.
    EnsureAchievementsReady(env);

    psxe_host_set_embedded(1);
    psxe_host_reset_pad_state();

    if (!CreateHostWindowAndRenderer()) {
        psxe_host_set_embedded(0);
        g_run_active.store(false, std::memory_order_release);
        return JNI_FALSE;
    }

    psxe_host_set_present_callback(PresentToSurface, nullptr);

    // EmulatorActivity.getArguments() passes `--bios <filesDir>/bios.bin` (copyBundledBios
    // deposits it there). Mirror that so the in-process path boots with the same BIOS instead
    // of depending on settings.toml alone — but only when that file actually exists;
    // settings.toml's [bios] override_file / search_path is the normal source otherwise.
    //
    // NEVER probe with SDL_RWFromFile() here. On Android SDL_RWFromFile falls through to the
    // asset manager (Android_JNI_FileOpen) when the plain fopen() misses, and that resolves
    // org.libsdl.app.SDLActivity's static JNI glue, which does not exist in-process. With
    // CheckJNI on (any debug build) that is an immediate, fatal
    // "CallStaticObjectMethod received NULL jclass" abort — which is exactly what killed the
    // armsx-vm thread here before external_main_ex() was ever reached. Plain POSIX stat().
    std::string bios_path;
    if (!files_dir.empty()) {
        std::string candidate = files_dir;
        if (candidate.back() != '/') {
            candidate.push_back('/');
        }
        candidate += "bios.bin";

        struct stat info{};
        if (::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0) {
            bios_path = std::move(candidate);
            ARMSX_LOGI("runVMThread: bundled BIOS at %s", bios_path.c_str());
        } else {
            ARMSX_LOGI("runVMThread: no %s; deferring BIOS selection to settings.toml",
                       candidate.c_str());
        }
    }

    std::vector<const char*> argv;
    argv.push_back("armsx");
    if (!bios_path.empty()) {
        argv.push_back("--bios");
        argv.push_back(bios_path.c_str());
    }
    /*
        No game path means "Boot BIOS" from the library drawer (MainActivityRuntime.startBios
        passes ""). That needs an EXPLICIT launch argument, not merely the absence of one:
        `--bios <file>` only selects WHICH BIOS image to use, and with no positional argument
        the core parses zero launch requests, falls through to "nothing to boot" and shows its
        landing/game-list window — which on Android does not exist, because the JNI front end
        replaced FSUI. The result was a session that initialised a renderer (phase=frontend-init
        in armsx.log), emulated nothing, and returned 0 whenever the user backed out. Clicking
        Boot BIOS looked like it did nothing because it did nothing.

        There is no CLI flag for it — the launch URI is the only way in. It must use the
        core's own scheme: LaunchForArgument() only routes to LaunchForUri() when the argument
        starts with "armsx:", and LaunchForUri() accepts exactly "armsx:" or "web+armsx:" and
        returns an empty request for anything else. `?kind=bios` is checked before any path
        decoding, so it is the most direct spelling.

        NOT `bios://boot`: that is what the core PRINTS for a BIOS session (a display path),
        never something it parses. Passing it yields kind=none and "Invalid launch request."
    */
    static const char* const kBiosLaunchUri = "armsx:?kind=bios";

    argv.push_back(game_path.empty() ? kBiosLaunchUri : game_path.c_str());
    argv.push_back(nullptr);

    ARMSX_LOGI("runVMThread: booting %s", game_path.empty() ? "BIOS (bios://boot)"
                                                            : game_path.c_str());
    const int result = external_main_ex(static_cast<int>(argv.size()) - 1, argv.data(),
                                        g_sdl_window, g_sdl_renderer);
    ARMSX_LOGI("runVMThread: core returned %d", result);

    DestroyHostWindowAndRenderer();
    psxe_host_set_embedded(0);
    psxe_host_reset_pad_state();
    g_run_active.store(false, std::memory_order_release);

    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

// Pin a custom Vulkan driver for the next renderer init, or pass empty strings to revert to
// the system loader. Called from CustomDriver.applyToNative() on the UI thread, before
// runVMThread(); the Vulkan backend reads it when it opens its loader.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setCustomVulkanDriver(JNIEnv* env, jclass, jstring driver_dir,
                                                           jstring driver_name, jstring redirect_dir,
                                                           jstring hook_lib_dir) {
    const std::string dir = JStringToUtf8(env, driver_dir);
    const std::string name = JStringToUtf8(env, driver_name);

    {
        std::lock_guard<std::mutex> lock(g_vk_driver_mutex);
        g_vk_driver_dir = dir;
        g_vk_driver_name = name;
        g_vk_driver_redirect_dir = JStringToUtf8(env, redirect_dir);
        g_vk_driver_hook_dir = JStringToUtf8(env, hook_lib_dir);
    }

    if (dir.empty() || name.empty()) {
        armsx_render_set_vulkan_loader(nullptr, nullptr);
        ARMSX_LOGI("Custom Vulkan driver cleared; the system loader will be used.");
        return;
    }

    armsx_render_set_vulkan_loader(OpenCustomVulkanDriver, nullptr);
    ARMSX_LOGI("Custom Vulkan driver selected: %s%s (applies on the next renderer init)",
               dir.c_str(), name.c_str());
}

// "system" or "angle". Selects the GLES implementation the OpenGL backend binds to. Wins over
// settings.toml's [video] gl_driver, which is only a default (see render.h).
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setGlDriver(JNIEnv* env, jclass, jstring driver) {
    const std::string value = JStringToUtf8(env, driver);
    const bool angle = SDL_strcasecmp(value.c_str(), "angle") == 0;
    armsx_render_set_gl_driver(angle ? ARMSX_RENDER_GL_DRIVER_ANGLE : ARMSX_RENDER_GL_DRIVER_SYSTEM);
    ARMSX_LOGI("GL driver selection: %s (applies on the next renderer init)",
               armsx_render_gl_driver_token(armsx_render_gl_driver()));
}

extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getGlDriver(JNIEnv* env, jclass) {
    return env->NewStringUTF(armsx_render_gl_driver_token(armsx_render_gl_driver()));
}

// The backend that actually survived the fallback ladder, never the one that was requested.
// Empty string (never null) when no renderer is up.
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getActiveRenderer(JNIEnv* env, jclass) {
    char name[128];
    char profile[320];
    char combined[464];

    name[0] = '\0';
    armsx_render_active_name(name, static_cast<int>(sizeof(name)));

    /* Append the identified GPU and driver.
       This row is what ends up in a screenshot attached to a bug report, and "OpenGL ES" alone
       does not say which driver produced the artefact. Every per-GPU decision this build makes
       keys off the profile, so the report should carry the same string the code branched on —
       otherwise triage starts by asking the reporter to find it. */
    profile[0] = '\0';
    armsx_gpu_profile_describe(profile, sizeof(profile));

    if (profile[0] != '\0') {
        std::snprintf(combined, sizeof(combined), "%s | %s", name, profile);
        return env->NewStringUTF(combined);
    }

    return env->NewStringUTF(name);
}

// Live display-mode control. 0 = classic 4:3, 1 = square 1:1, 2 = wide 16:9; < 0 reverts to
// settings.toml. Applies on the very next presented frame — aspect and stretch are pure
// presentation properties, so there is nothing to restart.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPortraitRenderTop(JNIEnv*, jclass, jboolean top) {
    psxe_host_set_portrait_render_top(top == JNI_TRUE ? 1 : 0);
}

// Punch-hole/notch height in SURFACE pixels, sent from EmulationSurface on creation and on every
// rotation. Portrait only: in landscape the cutout is on a side and shifting vertically is noise.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPortraitRenderTopInset(JNIEnv*, jclass, jint pixels) {
    psxe_host_set_portrait_render_top_inset(static_cast<int>(pixels));
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setDisplayAspect(JNIEnv*, jclass, jint mode) {
    psxe_host_set_display_aspect(static_cast<int>(mode));
    ARMSX_LOGI("display aspect -> %d", static_cast<int>(mode));
}
// Live custom width/height ratio, used while the mode is 3 (custom). <= 0 clears the
// override and falls back to settings.toml.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setDisplayAspectCustom(JNIEnv*, jclass, jfloat ratio) {
    psxe_host_set_display_aspect_custom(static_cast<float>(ratio));
}
// Live integer scaling: snap the output to a whole multiple of the source. Negative clears the
// override back to settings.toml.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setIntegerScaling(JNIEnv*, jclass, jboolean enabled) {
    psxe_host_set_integer_scaling(enabled == JNI_TRUE ? 1 : 0);
}



extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setStretchMode(JNIEnv*, jclass, jboolean enabled) {
    psxe_host_set_stretch_mode(enabled == JNI_TRUE ? 1 : 0);
    ARMSX_LOGI("stretch mode -> %s", enabled == JNI_TRUE ? "true" : "false");
}

// ---- [video] display feature set ------------------------------------------------------
//
// Six natives behind the Video tab's new rows. Five of them only affect how the finished
// frame is presented or how the GLES rasterizer samples/resolves, so they apply on the next
// frame with no VM interaction and are safe with no session running at all. The sixth
// (widescreen) changes the GTE's projection, which is core state — also safe live, for the
// same reason PGXP is: the next projected vertex simply uses the new scale.
//
// Every one of these ALSO has a settings.toml key that the core reads at launch. These
// natives exist so the change is visible immediately instead of at the next boot; the Kotlin
// side writes both.

// 0 = weave (default), 1 = bob, 2 = adaptive. Negative clears back to settings.toml.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setDeinterlaceMode(JNIEnv*, jclass, jint mode) {
    psxe_host_set_deinterlace((int)mode);
    ARMSX_LOGI("deinterlace -> %d", (int)mode);
}

// 0 = none (default), 1 = small, 2 = full. Negative clears back to settings.toml.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setOverscanCrop(JNIEnv*, jclass, jint mode) {
    psxe_host_set_overscan_crop((int)mode);
    ARMSX_LOGI("overscan crop -> %d", (int)mode);
}

// Degrees: 0 / 90 / 180 / 270. Anything else clears back to settings.toml.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setDisplayRotation(JNIEnv*, jclass, jint degrees) {
    psxe_host_set_display_rotation((int)degrees);
    ARMSX_LOGI("display rotation -> %d deg", (int)degrees);
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setWidescreenHack(JNIEnv*, jclass, jboolean enabled) {
    psxe_host_set_widescreen_hack(enabled == JNI_TRUE ? 1 : 0);
    ARMSX_LOGI("widescreen hack -> %s", enabled == JNI_TRUE ? "true" : "false");
}

// One call for the three GLES-rasterizer options, because the backend stores them together
// and a partial push would need a read-modify-write the Kotlin side does not have.
//   texture_filter 0 nearest / 1 bilinear / 2 xBR-style
//   downsample     0 off, 2..8 box factor (effective factor must divide the internal scale)
//   line_detect    0 disabled / 1 quads / 2 basic
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setGlVideoOptions(JNIEnv*, jclass, jint texture_filter,
                                                       jint downsample, jint line_detect) {
    psxe_host_set_gl_video_options((int)texture_filter, (int)downsample, (int)line_detect);
    ARMSX_LOGI("gl video options -> filter=%d downsample=%d line_detect=%d",
               (int)texture_filter, (int)downsample, (int)line_detect);
}

/*
    Texture dumping and replacement (psx/texrep.h).

    `dir` is the BASE folder: dumps land in <dir>/dump, packs are read from
    <dir>/replacements, and both are created on demand. The app passes a PER-GAME path
    (…/textures/<serial>) because only it knows the disc serial; an empty string makes the
    core fall back to <prefs>/textures.

    Both flags off tears the subsystem down and returns the emulator to a state where nothing
    at all is allocated and the rasterizers take one never-taken branch per texel.
*/
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPs1TextureOptions(JNIEnv* env, jclass, jboolean dump,
                                                          jboolean replace, jstring dir) {
    const char* chars = dir ? env->GetStringUTFChars(dir, nullptr) : nullptr;

    psxe_host_set_texture_options(dump == JNI_TRUE ? 1 : 0,
                                  replace == JNI_TRUE ? 1 : 0,
                                  chars ? chars : "");
    ARMSX_LOGI("ps1 texture options -> dump=%s replace=%s dir=%s",
               dump == JNI_TRUE ? "true" : "false",
               replace == JNI_TRUE ? "true" : "false",
               chars ? chars : "(default)");

    if (chars)
        env->ReleaseStringUTFChars(dir, chars);
}

// PGXP (psx/pgxp.c): sub-pixel vertex precision. Core-global, so this is safe to flip
// while the VM runs — the module validates every attach against live RAM contents, and
// the worst case after a toggle is one frame of plain integer vertices. The boot-time
// value comes from settings.toml ([video] pgxp) through frontend/main.cpp; this native
// exists so the Kotlin video tab can live-toggle without a restart.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPgxpEnabled(JNIEnv*, jclass, jboolean enabled) {
    psx_pgxp_set_enabled(enabled == JNI_TRUE ? 1 : 0);
    ARMSX_LOGI("pgxp -> %s", enabled == JNI_TRUE ? "true" : "false");
}

// ---- Host CPU scheduling levers -------------------------------------------------------
//
// Two settings that act on the PHONE, not on the emulated console, and two natives that do
// nothing but park a value. Both are called from the Android UI thread — setAdpfEnabled at
// Activity setup and again whenever the switch moves, setAffinityMode just before
// runVMThread — so neither may touch the VM, and neither does: perf_hint.c keeps the state in
// an atomic and the emulation thread acts on it inside its own frame loop. Safe with no VM.
//
// Neither is a measured win on any device. Both ship EXPERIMENTAL and off; see perf_hint.h
// for what ADPF actually reports and why it is the work duration rather than the frame time.

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAdpfEnabled(JNIEnv*, jclass, jboolean enabled) {
    armsx_perf_hint_set_adpf_enabled(enabled == JNI_TRUE ? 1 : 0);
    ARMSX_LOGI("adpf clock hint -> %s", enabled == JNI_TRUE ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAffinityMode(JNIEnv*, jclass, jint mode) {
    armsx_affinity_set_mode(static_cast<int>(mode));
    ARMSX_LOGI("affinity mode -> %d (normalised %d)", static_cast<int>(mode),
               armsx_affinity_mode());
}

// ---- RetroArch (.slangp) shader chains ------------------------------------------------
//
// Three real natives backing com.armsx2.ShaderParams and Settings.applyTo(). All three are
// safe with no VM and no renderer: the request is recorded and the presenting thread picks
// it up on its next frame, and parameter enumeration only parses a file.
//
// Whether librashader is even present is decided lazily inside render_shaders.cpp, and its
// absence is logged once and degrades to plain presentation. Nothing here can fail loudly.

// Select the chain. An empty path is "none" regardless of `enabled`.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setShaderChain(JNIEnv* env, jclass, jboolean enabled,
                                                   jstring preset_path) {
    const std::string path = JStringToUtf8(env, preset_path);
    armsx_shader_set_chain(enabled == JNI_TRUE, path.c_str());
}

// The preset's parameters as a JSON array, in declaration order (order is load-bearing: a
// parameter whose maximum equals its minimum is a caption for the run that follows it).
//
// NEVER returns null, and never returns something JSONArray() would throw on. The Kotlin
// caller feeds the result straight to JSONArray(), where both null and "" throw — the old
// stub returned "" and so logged a spurious parse warning on every single call.
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_shaderPresetParams(JNIEnv* env, jclass, jstring preset_path) {
    const std::string path = JStringToUtf8(env, preset_path);
    char* json = armsx_shader_preset_params(path.c_str());
    jstring result = env->NewStringUTF(json ? json : "[]");
    armsx_shader_free_string(json);
    // NewStringUTF returns null on allocation failure; "[]" is a last-resort that at least
    // parses. Returning null here has crashed this app before.
    return result ? result : env->NewStringUTF("[]");
}

// Queue parameter assignments for `presetPath`'s chain, applied by the presenting thread on
// its next frame. A set of ASSIGNMENTS, not the whole state: an omitted parameter keeps what
// the chain has, which is why ShaderParams.pushEffective sends initial values explicitly
// rather than omitting reset ones.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setShaderChainParams(JNIEnv* env, jclass, jstring preset_path,
                                                         jobjectArray names, jfloatArray values) {
    const std::string path = JStringToUtf8(env, preset_path);
    if (path.empty() || !names || !values) {
        return;
    }

    // Trust neither length: a mismatched pair is a caller bug, and reading past the shorter
    // of the two would be an out-of-bounds read rather than a dropped parameter.
    const jsize name_count = env->GetArrayLength(names);
    const jsize value_count = env->GetArrayLength(values);
    const jsize count = name_count < value_count ? name_count : value_count;
    if (count <= 0) {
        return;
    }

    std::vector<float> floats(static_cast<size_t>(count), 0.0f);
    env->GetFloatArrayRegion(values, 0, count, floats.data());

    // Own the strings for the duration of the call: the pointers handed across must outlive
    // the loop that fills the table, so the jstring locals cannot be released early.
    std::vector<std::string> owned(static_cast<size_t>(count));
    std::vector<const char*> table(static_cast<size_t>(count), nullptr);
    for (jsize i = 0; i < count; ++i) {
        auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        owned[static_cast<size_t>(i)] = JStringToUtf8(env, name);
        if (name) {
            env->DeleteLocalRef(name);
        }
        table[static_cast<size_t>(i)] =
            owned[static_cast<size_t>(i)].empty() ? nullptr : owned[static_cast<size_t>(i)].c_str();
    }

    armsx_shader_queue_params(path.c_str(), table.data(), floats.data(), static_cast<int>(count));
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_pause(JNIEnv*, jclass) {
    psxe_host_set_paused(1);
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_resume(JNIEnv*, jclass) {
    psxe_host_set_paused(0);
}

// The Activity went off-screen / came back. Stops the PLATFORM audio stream, which
// SDL_PauseAudioDevice() (and therefore pause() above) leaves running — see
// ArmsxSession::setAudioSuspended(). Ignored when [audio] background_playback is on.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAudioSuspended(JNIEnv*, jclass, jboolean suspended) {
    psxe_host_set_audio_suspended(suspended == JNI_TRUE ? 1 : 0);
    ARMSX_LOGI("audio suspended -> %s", suspended == JNI_TRUE ? "true" : "false");
}

// App off-screen: stop drawing/posting frames. The VM being paused is NOT enough — the loop
// kept uploading and posting to an invisible window, measured at ~6.5 jiffies/s of system
// time with the screen off. Separate from audio suspension on purpose.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPresentationSuspended(JNIEnv*, jclass, jboolean suspended) {
    psxe_host_set_presentation_suspended(suspended == JNI_TRUE ? 1 : 0);
    ARMSX_LOGI("presentation suspended -> %s", suspended == JNI_TRUE ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_shutdown(JNIEnv*, jclass) {
    psxe_host_request_shutdown();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_hasActiveVM(JNIEnv*, jclass) {
    const bool active = g_run_active.load(std::memory_order_acquire) ||
                        psxe_host_loop_running() != 0 ||
                        psxe_host_vm_active() != 0;
    return active ? JNI_TRUE : JNI_FALSE;
}

// index: the front-end's pad code space (Android KEYCODE_* for the pad, 200 = analog-mode
// button, 110-113 / 120-123 = left/right stick directions). range: 0..32767 magnitude for the
// stick directions, ignored (treated as a full press) for digital buttons.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPadButton(JNIEnv*, jclass, jint index, jint range, jboolean pressed) {
    psxe_host_pad_button(static_cast<int>(index), static_cast<int>(range), pressed == JNI_TRUE ? 1 : 0);
}

// stick: 0 = left, 1 = right. x/y: 0x00..0xFF centred on 0x80, matching the PS1 pad's ADC.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPadAnalog(JNIEnv*, jclass, jint stick, jint x, jint y) {
    psxe_host_pad_analog(static_cast<int>(stick), static_cast<int>(x), static_cast<int>(y));
}

// The same two, addressed to one of the four players behind a port-1 Multitap. `player` is
// 0..3 (A..D) and is clamped, not wrapped. With no tap in the port the core drops anything
// above 0 — deliberately, so a routing bug reads as a dead player 2 rather than as two
// players silently driving the same pad.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPadButtonForPlayer(JNIEnv*, jclass, jint player, jint index,
                                                           jint range, jboolean pressed) {
    psxe_host_pad_button_player(static_cast<int>(player), static_cast<int>(index),
                                static_cast<int>(range), pressed == JNI_TRUE ? 1 : 0);
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPadAnalogForPlayer(JNIEnv*, jclass, jint player, jint stick,
                                                           jint x, jint y) {
    psxe_host_pad_analog_player(static_cast<int>(player), static_cast<int>(stick),
                                static_cast<int>(x), static_cast<int>(y));
}

// ---- Multitap, rewind, runahead -------------------------------------------------------
//
// All five park their work for the emulation thread (the first three allocate or rebuild
// machine state; the last two are read once per frame). None of them needs a running VM:
// with nothing booted the request simply sits until one is, and the launch path applies the
// same values from settings.toml anyway.

// [input] multitap. Live: the core swaps the device in controller port 1 at the next
// instruction boundary, so a game that is already running sees the tap plugged in (or
// unplugged) without a restart. Persist it in settings.toml too, or the next launch reverts.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setMultitapEnabled(JNIEnv*, jclass, jboolean enabled) {
    psxe_host_set_multitap(enabled == JNI_TRUE ? 1 : 0);
    ARMSX_LOGI("multitap -> %s", enabled == JNI_TRUE ? "true" : "false");
}

// [emulation] rewind. seconds x frequency is a MEMORY figure — see rewindSnapshotBytes()
// below and psx/rewind.h. Passing enabled=false frees the ring immediately.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setRewind(JNIEnv*, jclass, jboolean enabled, jint seconds,
                                               jint frequency) {
    psxe_host_set_rewind(enabled == JNI_TRUE ? 1 : 0, static_cast<int>(seconds),
                         static_cast<int>(frequency));
    ARMSX_LOGI("rewind -> %s (%d s at %d/s)", enabled == JNI_TRUE ? "true" : "false",
               static_cast<int>(seconds), static_cast<int>(frequency));
}

// [emulation] runahead, 0..5 frames. 0 is off and is the default.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setRunahead(JNIEnv*, jclass, jint frames) {
    psxe_host_set_runahead(static_cast<int>(frames));
    ARMSX_LOGI("runahead -> %d", static_cast<int>(frames));
}

// Hold-to-rewind. true while the bound button is down; the emulation thread then steps one
// snapshot back per frame instead of advancing. Not logged — it is pressed constantly.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setRewindActive(JNIEnv*, jclass, jboolean active) {
    psxe_host_set_rewind_active(active == JNI_TRUE ? 1 : 0);
}

// One-shot step back, for a menu row or a tap. Additive with the hold above.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_rewindStepBack(JNIEnv*, jclass) {
    psxe_host_rewind_step();
}

// Bytes ONE rewind snapshot costs. Measured from the running machine once a state has been
// captured, an estimate before that — which is what lets the settings row show a real figure
// with no game loaded. The UI multiplies by seconds x frequency.
extern "C" JNIEXPORT jlong JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_rewindSnapshotBytes(JNIEnv*, jclass) {
    return static_cast<jlong>(psxe_host_rewind_snapshot_bytes());
}

// Bytes the ring is holding RIGHT NOW. 0 while rewind is off — nothing is allocated then.
extern "C" JNIEXPORT jlong JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_rewindBytesHeld(JNIEnv*, jclass) {
    return static_cast<jlong>(psxe_host_rewind_bytes_used());
}

// Release everything the host is holding down (overlay teardown, focus loss).
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_resetKeyStatus(JNIEnv*, jclass) {
    psxe_host_reset_pad_state();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_saveScreenshot(JNIEnv* env, jclass, jstring path) {
    const std::string target = JStringToUtf8(env, path);
    psxe_host_request_screenshot(target.empty() ? nullptr : target.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_resetGame(JNIEnv*, jclass) {
    psxe_host_request_reset();
}

// Save states. psx_state_request_slot() parks the op and blocks until the emulation thread runs
// it from psx_update(), so these two are safe to call from the UI thread — but they DO block, so
// the Kotlin side calls them off the main thread.
//
// The wait has to outlast a frame the emulator is slow to finish; it must NOT outlast a paused
// VM, because psx_update() is not running then and the request would sit there for the whole
// timeout. Two seconds is long enough for the former and short enough that the latter reads as
// a failed press rather than a hang.
static constexpr int kStateRequestTimeoutMs = 2000;

// Returns the raw PSX_STATE_* code so a caller can tell the advisory memory-card verdicts
// (PSX_STATE_ERR_CARD_NEWER / _DIVERGED) from a real failure. Those two mean the state is
// intact and untouched, and that the user should be asked before it is applied.
static jint RequestStateSlotCode(int op, jint slot, unsigned flags) {
    const std::string files_dir = CachedFilesDir();
    if (files_dir.empty()) {
        ARMSX_LOGE("state slot %d: no files dir (no run has started)", static_cast<int>(slot));
        return PSX_STATE_ERR_ARG;
    }

    const int result = psx_state_request_slot_ex(op, static_cast<int>(slot), files_dir.c_str(),
                                                 kStateRequestTimeoutMs, flags);
    if (result == PSX_STATE_ERR_CARD_NEWER || result == PSX_STATE_ERR_CARD_DIVERGED) {
        // Not an error: nothing was applied, and the Kotlin side is about to put the question
        // to the user. Logged at info so a diag file does not read as though a load broke.
        ARMSX_LOGI("load slot %d deferred to user: %s (%d)", static_cast<int>(slot),
                   psx_state_strerror(result), result);
        return result;
    }

    if (result != PSX_STATE_OK) {
        ARMSX_LOGE("%s slot %d failed: %s (%d)", op == PSX_STATE_OP_SAVE ? "save" : "load",
                   static_cast<int>(slot), psx_state_strerror(result), result);
        return result;
    }

    ARMSX_LOGI("%s slot %d ok", op == PSX_STATE_OP_SAVE ? "save" : "load", static_cast<int>(slot));
    return PSX_STATE_OK;
}

static jboolean RequestStateSlot(int op, jint slot) {
    return RequestStateSlotCode(op, slot, 0) == PSX_STATE_OK ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_saveStateToSlot(JNIEnv*, jclass, jint slot) {
    return RequestStateSlot(PSX_STATE_OP_SAVE, slot);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_loadStateFromSlot(JNIEnv*, jclass, jint slot) {
    // Legacy entry point. Keeps its exact original meaning — "load it, tell me yes or no" —
    // by opting out of the card check, so a caller that has not been taught to ask the user
    // cannot start silently refusing loads that used to work.
    return RequestStateSlotCode(PSX_STATE_OP_LOAD, slot,
                                PSX_STATE_LOAD_IGNORE_CARD_DIVERGENCE) == PSX_STATE_OK
               ? JNI_TRUE
               : JNI_FALSE;
}

// The checked load. Returns a PSX_STATE_* code; the Kotlin guard (SaveStateGuard.kt) turns
// ERR_CARD_NEWER / ERR_CARD_DIVERGED into the warning dialog and re-issues with
// ignoreCardDivergence = true if the user chooses to load anyway.
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_loadStateFromSlotChecked(JNIEnv*, jclass, jint slot,
                                                              jboolean ignore_card_divergence) {
    const unsigned flags =
        ignore_card_divergence ? PSX_STATE_LOAD_IGNORE_CARD_DIVERGENCE : 0u;

    return RequestStateSlotCode(PSX_STATE_OP_LOAD, slot, flags);
}

// PNG preview for a state, or an empty array when there is none. Never null — the picker's
// caller treats null as "unreadable" rather than "no preview".
//
// A state written before previews existed simply has no THUMB section; that returns
// PSX_STATE_ERR_MISSING, which is NOT an error worth surfacing. The bytes are PNG precisely
// because these are handed to android.graphics.BitmapFactory, which does not reliably decode
// the BMP that the core's own screenshot path (SDL_SaveBMP) produces — feeding it BMP yields a
// silent null and a blank tile indistinguishable from "nothing was saved".
static jbyteArray ThumbnailToByteArray(JNIEnv* env, void* data, size_t size) {
    if (!data || !size) {
        return env->NewByteArray(0);
    }

    jbyteArray out = env->NewByteArray(static_cast<jsize>(size));

    if (out) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(size),
                                reinterpret_cast<const jbyte*>(data));
    }

    return out ? out : env->NewByteArray(0);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getImageSlot(JNIEnv* env, jclass, jint slot) {
    const std::string files_dir = CachedFilesDir();

    if (files_dir.empty()) {
        return env->NewByteArray(0);
    }

    void* data = nullptr;
    size_t size = 0;

    psx_state_slot_thumbnail(static_cast<int>(slot), files_dir.c_str(), &data, &size);

    jbyteArray out = ThumbnailToByteArray(env, data, size);
    free(data);

    return out;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getSaveStateImage(JNIEnv* env, jclass, jstring path) {
    const std::string target = JStringToUtf8(env, path);

    if (target.empty()) {
        return env->NewByteArray(0);
    }

    void* data = nullptr;
    size_t size = 0;

    psx_state_read_thumbnail(target.c_str(), &data, &size);

    jbyteArray out = ThumbnailToByteArray(env, data, size);
    free(data);

    return out;
}

// Disc path recorded for [slot], or "" when the slot is empty.
//
// The save/load picker marks a slot occupied by this being non-empty, and DISABLES the tile for
// loading when it is empty. While this was a stub returning "", every slot read as empty, so Load
// was permanently greyed out and saved states were unreachable — states were being written the
// whole time. Never return null: the picker's callers treat null as "unreadable", not "empty".
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getGamePathSlot(JNIEnv* env, jclass, jint slot) {
    const std::string files_dir = CachedFilesDir();
    char disc_path[1024] = {0};

    if (files_dir.empty() ||
        !psx_state_slot_info(static_cast<int>(slot), files_dir.c_str(), disc_path, sizeof(disc_path))) {
        return env->NewStringUTF("");
    }

    return env->NewStringUTF(disc_path);
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setFastForward(JNIEnv*, jclass, jboolean enabled) {
    psxe_host_set_fast_forward(enabled == JNI_TRUE ? 1 : 0);
}

// The frame-pacing policy, pushed live so the pause menu's Frame limit / Emulation speed /
// Frame rate cap / Fast-forward speed / Frame skip rows take effect without relaunching the
// game. The same five values are persisted in settings.toml's [runtime] table and read there at
// boot; this is the live edge, not a second source of truth.
//
// frame_skip is carried here because the same panel edits it and Ps1Pacing pushes the policy in
// one go — but it is a PRESENTATION control, not a speed one: 0 off, 1..5 fixed, -1 adaptive.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setSpeedLimits(JNIEnv*, jclass, jboolean frame_limit,
                                                    jint speed_percent, jint fps_limit,
                                                    jfloat fast_forward_speed, jint frame_skip) {
    psxe_host_set_speed_limits(frame_limit == JNI_TRUE ? 1 : 0, static_cast<int>(speed_percent),
                               static_cast<int>(fps_limit), static_cast<float>(fast_forward_speed),
                               static_cast<int>(frame_skip));
}

// Session telemetry for the Compose OSD. The front-end cut imgui/FSUI out of the core, so there
// is no native overlay left to draw an FPS counter — com.armsx2.ui.GameOsd draws it instead and
// polls these. Non-blocking atomic reads of what the emulation loop published on its last frame;
// all three read 0 while nothing is running.
extern "C" JNIEXPORT jfloat JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getFPS(JNIEnv*, jclass) {
    return static_cast<jfloat>(psxe_host_measured_fps());
}

extern "C" JNIEXPORT jfloat JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getNominalFrameRate(JNIEnv*, jclass) {
    return static_cast<jfloat>(psxe_host_nominal_frame_rate());
}

extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getPresentedFrameCount(JNIEnv*, jclass) {
    return static_cast<jint>(psxe_host_presented_frames() & 0x7FFFFFFFu);
}

// The detailed performance overlay (per-subsystem work counters + host frame-phase timing).
// Arming it is what turns the core's counters on, so the Compose overlay MUST call
// setStatisticsEnabled(false) when the block is hidden — leaving it armed keeps every counter
// site in the emulator live for a panel nobody is looking at.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setStatisticsEnabled(JNIEnv*, jclass, jboolean enabled) {
    psxe_host_set_stats_enabled(enabled == JNI_TRUE ? 1 : 0);
}

// Refills a caller-owned double[] (see PSXE_HOST_STAT_* / com.armsx2.ui.GameOsd.Stat) and
// returns how many entries it wrote — 0 until the first half-second window has closed. The
// array is owned by Kotlin and reused, so polling this allocates nothing.
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getStatistics(JNIEnv* env, jclass, jdoubleArray out) {
    if (!env || !out) {
        return 0;
    }

    const jsize capacity = env->GetArrayLength(out);

    if (capacity <= 0) {
        return 0;
    }

    double values[PSXE_HOST_STAT_COUNT] = {};
    const unsigned int written = psxe_host_stats(values, (unsigned int)PSXE_HOST_STAT_COUNT);

    if (!written) {
        return 0;
    }

    const jsize n = capacity < (jsize)written ? capacity : (jsize)written;
    env->SetDoubleArrayRegion(out, 0, n, values);

    return static_cast<jint>(n);
}

// Queue a game/URI for the running loop to boot (disc swap from the library without tearing
// the process down). Mirrors EmulatorActivity's nativeEnqueueLaunchArgument.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_enqueueLaunchArgument(JNIEnv* env, jclass, jstring argument) {
    const std::string value = JStringToUtf8(env, argument);
    if (!value.empty()) {
        psxe_enqueue_launch_argument(value.c_str());
    }
}

// ------------------------------------------------------------------------------------------
// RetroAchievements (kr.co.iefriends.pcsx2.NativeApp)
// ------------------------------------------------------------------------------------------
//
// SOFTCORE ONLY. setHardcoreMode() is a deliberate no-op and both hardcore queries answer false
// unconditionally — see frontend/achievements.h. Nothing below can turn hardcore on.
//
// NEVER return a null jstring from any of these. The Kotlin achievements screen calls
// .orEmpty()/optString on the result and a null used to take the whole app down the moment the
// screen opened, which is why the Java stubs returned "" and "{}" rather than null.

namespace {

jstring Utf8ToJString(JNIEnv* env, const std::string& value) {
    jstring result = env ? env->NewStringUTF(value.c_str()) : nullptr;
    if (!result && env) {
        // NewStringUTF can only fail on OOM; hand back an empty string rather than a null the
        // caller is not prepared for.
        env->ExceptionClear();
        result = env->NewStringUTF("");
    }
    return result;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getAchievementsJSON(JNIEnv* env, jclass) {
    EnsureAchievementsReady(env);
    return Utf8ToJString(env, armsx_ach_get_json());
}

// Hash of an image that is NOT mounted, for the library's per-game progress lookup. Reads the
// disc, so the Kotlin side calls it off the UI thread.
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getAchievementsHashForPath(JNIEnv* env, jclass, jstring image_path) {
    EnsureAchievementsReady(env);
    const std::string path = JStringToUtf8(env, image_path);
    return Utf8ToJString(env, path.empty() ? std::string() : armsx_ach_hash_for_path(path.c_str()));
}

extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getRichPresence(JNIEnv* env, jclass) {
    return Utf8ToJString(env, armsx_ach_get_rich_presence());
}

// Blocking: runs the login round trip to completion. Returns "" on success, otherwise a message
// to show the user.
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_loginAchievements(JNIEnv* env, jclass, jstring username, jstring password) {
    EnsureAchievementsReady(env);
    const std::string user = JStringToUtf8(env, username);
    const std::string pass = JStringToUtf8(env, password);
    return Utf8ToJString(env, armsx_ach_login(user.c_str(), pass.c_str()));
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_logoutAchievements(JNIEnv* env, jclass) {
    EnsureAchievementsReady(env);
    armsx_ach_logout();
}

// Softcore only. Accepted and ignored so the lifted ARMSX2 UI cannot wedge, and logged so a
// stray call is visible rather than silent.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setHardcoreMode(JNIEnv*, jclass, jboolean enabled) {
    if (enabled == JNI_TRUE) {
        ARMSX_LOGW("setHardcoreMode(true) ignored: ARMSX RetroAchievements support is softcore only");
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_changeDisc(JNIEnv* env, jclass, jstring path) {
    if (!path) {
        return JNI_FALSE;
    }

    const char* chars = env->GetStringUTFChars(path, nullptr);

    if (!chars) {
        return JNI_FALSE;
    }

    /* Parked, not performed here: this runs on the UI thread and psx_swap_disc() reopens the
       image under a machine the emulation thread is still stepping. Returns true for "accepted",
       not "completed" — the swap happens at the next instruction boundary. */
    psxe_host_request_disc_swap(chars);
    env->ReleaseStringUTFChars(path, chars);

    return psxe_host_vm_active() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_isHardcoreMode(JNIEnv*, jclass) {
    return armsx_ach_hardcore_active() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_isHardcorePersisted(JNIEnv*, jclass) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsOption(JNIEnv* env, jclass, jstring key, jboolean enabled) {
    EnsureAchievementsReady(env);
    const std::string name = JStringToUtf8(env, key);
    armsx_ach_set_option(name.c_str(), enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsOptionInt(JNIEnv* env, jclass, jstring key, jint value) {
    EnsureAchievementsReady(env);
    const std::string name = JStringToUtf8(env, key);
    armsx_ach_set_option_int(name.c_str(), static_cast<int>(value));
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsUnlockSound(JNIEnv* env, jclass, jstring path) {
    EnsureAchievementsReady(env);
    const std::string value = JStringToUtf8(env, path);
    armsx_ach_set_unlock_sound(value.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsHostOverride(JNIEnv* env, jclass, jstring host) {
    EnsureAchievementsReady(env);
    const std::string value = JStringToUtf8(env, host);
    armsx_ach_set_host_override(value.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_clearAchievementsHostOverride(JNIEnv* env, jclass) {
    EnsureAchievementsReady(env);
    armsx_ach_clear_host_override();
}

// ---- cheats ----------------------------------------------------------------------------
//
// Four natives, all real. The catalogue they operate on is process-global (psx/cheats.c), and
// these run on the UI thread, so the contract Kotlin has to keep is simple and is stated in
// NativeApp: only ever drive these for the game that is actually running. Editing another
// game's selection from the settings hub writes settings.toml and takes effect at ITS next
// launch — it must not reach in here and swap the running game's catalogue out.
//
// Nothing here can be reached while a frame is mid-apply: psx_cheats_apply() adopts a
// published program at a frame boundary and never dereferences the catalogue at all.

/* Parse `path` as the current catalogue. Returns the entry count, or -1 if the file is not
   readable. An empty path clears — which is how "this game has no cheat file" is expressed. */
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_cheatsLoad(JNIEnv* env, jclass, jstring path) {
    const std::string value = JStringToUtf8(env, path);
    return static_cast<jint>(psx_cheats_load_file(value.c_str()));
}

/*
    The loaded catalogue, as the CORE parsed it.

    Record separator U+001E, field separator U+001F, following usbDeviceTypes() in the sibling
    project. Fields: name, line count, "1"/"0" for "contains a code type this build does not
    implement", description.

    The point of exposing this at all is that the UI's own reader and the engine's parser can
    then be COMPARED rather than assumed equal — a cheats screen that lists an entry the
    engine did not parse is a lie, and this is what makes it detectable.
*/
extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_cheatsList(JNIEnv* env, jclass) {
    /* psx_cheats_describe(), not the per-entry accessors: it builds the whole thing under the
       module's lock and hands back a copy, so a load landing on the other thread cannot free
       the strings mid-walk. The accessors borrow, and this is not the owning thread. */
    char* described = psx_cheats_describe();
    jstring result = env->NewStringUTF(described ? described : "");
    free(described);
    return result;
}

/*
    Arm exactly `names` and publish the result for the emulation thread — the LIVE path, so a
    cheat switched on mid-game takes effect on the next frame rather than at the next launch.

    Returns the number armed, or -1 when RetroAchievements hardcore is active (in which case
    nothing is armed and the caller should say why). The gate is asked here as well as every
    frame in main.cpp so a user gets told at the moment of the attempt.
*/
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_cheatsApply(JNIEnv* env, jclass, jboolean master,
                                                 jobjectArray names) {
    if (armsx_ach_hardcore_active()) {
        psx_cheats_set_inhibited(1);
        psx_cheats_arm(0, nullptr, 0, nullptr);
        return -1;
    }

    const jsize count = names ? env->GetArrayLength(names) : 0;

    // Own the strings for the duration of the call: the table hands out pointers into these,
    // so the jstring locals cannot be released before psx_cheats_arm() has read them.
    std::vector<std::string> owned(static_cast<size_t>(count < 0 ? 0 : count));
    std::vector<const char*> table(owned.size(), nullptr);

    for (jsize i = 0; i < count; ++i) {
        auto entry = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        owned[static_cast<size_t>(i)] = JStringToUtf8(env, entry);
        if (entry) {
            env->DeleteLocalRef(entry);
        }
        table[static_cast<size_t>(i)] = owned[static_cast<size_t>(i)].c_str();
    }

    return static_cast<jint>(psx_cheats_arm(master == JNI_TRUE ? 1 : 0,
                                            table.empty() ? nullptr : table.data(),
                                            static_cast<int>(table.size()), nullptr));
}

/* How many entries the running machine currently has armed. The UI shows this rather than its
   own count of ticked boxes, so "you ticked five" and "five are running" cannot disagree. */
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_cheatsArmedCount(JNIEnv*, jclass) {
    return static_cast<jint>(psx_cheats_armed_count());
}

#else

// Keeps the translation unit non-empty on non-Android targets (-pedantic).
extern "C" int armsx_android_jni_unavailable(void) {
    return 0;
}

#endif // __ANDROID__
