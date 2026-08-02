#include <SDL.h>
#include <SDL_gamecontroller.h>
#include <SDL_render.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if !defined(UWP_TARGET)
#include <dbghelp.h>
#endif
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(PSVITA_TARGET)
#if defined(__has_include)
#if __has_include(<execinfo.h>)
#include <dlfcn.h>
#include <execinfo.h>
#define PSXE_HAS_EXECINFO 1
#endif
#endif
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#endif

#if defined(__ANDROID__)
#include <jni.h>
#include <android/log.h>
// Boot-path tracing. The core's own diagnostics are file-backed and off by default
// (quiet=true / logging_enabled=false in settings.toml), so a silent early return in
// ArmsxApp::run() left no trace anywhere. These go straight to logcat under the same tag
// frontend/android_jni.cpp uses, unconditionally.
#define ARMSX_BOOTLOG(...) __android_log_print(ANDROID_LOG_INFO, "ARMSX-JNI", __VA_ARGS__)
#define ARMSX_BOOTERR(...) __android_log_print(ANDROID_LOG_ERROR, "ARMSX-JNI", __VA_ARGS__)
#else
#define ARMSX_BOOTLOG(...) ((void)0)
#define ARMSX_BOOTERR(...) ((void)0)
#endif

extern "C" {
#include "../psx/psx.h"
#include "../psx/perf.h"
#include "../psx/pgxp.h"
/* psx_cpu_set_widescreen_hack() — the GTE X-projection scale behind [video] widescreen_hack. */
#include "../psx/cpu.h"
#include "../psx/rewind.h"
#include "../psx/state.h"
#include "../psx/texrep.h"
/* [cheats] — the GameShark engine. Read psx/cheats.h before touching any of the four call
   sites here: the format choice, where enablement is stored and why, and the per-frame cost
   contract are all argued there. */
#include "../psx/cheats.h"
#include "../psx/dev/cdrom/cdrom.h"
#include "../psx/dev/gpu.h"
/* audioDiagSnapshot() reads SPUCNT, the mixer volumes and the reverb work-area pointers
   directly; the mixer only ever needed the handle. */
#include "../psx/dev/spu.h"
#include "../psx/dev/input.h"
#include "../psx/dev/pad.h"
#include "../psx/dev/timer.h"
#include "../psx/input/multitap.h"
#include "../psx/input/sda.h"
#include "common.h"
#include "config.h"
#include "toml.h"
}

#include "achievements.h"
#include "archive.h"
#include "host_stats.h"
#include "host_usage.h"
// ADPF CPU clock hint + emulation-thread affinity. Both are host scheduling levers, both are
// default-off, and both compile to empty bodies off Android — see frontend/perf_hint.h.
#include "perf_hint.h"
#include "render.h"
// IconsFontAwesome5.h (from fsui-lib's imgui) removed with the FSUI cut — the ICON_FA_* glyphs were
// used only by the deleted native menus.
#ifdef USE_HARDWARE
#include "gpu_hw.h"
#include "gpu_hw_gl.h"
#include "gpu_hw_rt.h"
#endif

#undef main

namespace {

constexpr Uint32 kPauseChordGraceMs = 120;

constexpr bool SupportsManagedWindowSizing() {
#if defined(__ANDROID__) || defined(IOS_TARGET) || defined(__EMSCRIPTEN__) || defined(UWP_TARGET) || defined(PSVITA_TARGET)
    return false;
#else
    return true;
#endif
}

constexpr bool DefaultVsyncEnabled() {
#if defined(UWP_TARGET)
    return false;
#else
    return true;
#endif
}

#ifdef USE_HARDWARE
// Kept numerically compatible with armsx_render_backend_t (frontend/render.h) so the two
// never drift; the settings layer speaks GpuBackend, the presentation layer speaks
// armsx_render_backend_t, and RenderBackendFor() is the single conversion point.
enum class GpuBackend {
    Software = 0,
    SDLAccelerated = 1,
    OpenGL = 2,
    Vulkan = 3,
};
#endif

class ArmsxApp;

ArmsxApp* g_active_app = nullptr;
std::atomic_bool g_crash_reporting{false};
std::mutex g_pending_launch_lock;
std::vector<std::string> g_pending_launch_arguments;
std::vector<std::string> g_pending_web_errors;

// ---------------------------------------------------------------------------
// Embedded-host control surface.
//
// When the core is hosted *in-process* by another UI (the Android Jetpack Compose
// front-end via frontend/android_jni.cpp, or any other embedder), that UI runs on its
// own thread and needs to poke the emulation loop: pause, reset, screenshot, pad input.
// Every one of those requests is parked here under g_host_control_lock and drained by
// ArmsxApp::applyHostControlRequests() on the emulation thread — exactly the same
// mutex-guarded hand-off the pending-launch-argument queue already uses. Nothing below
// touches SDL or the psx_t from the caller's thread.
// ---------------------------------------------------------------------------

// Pad command kinds parked by the host.
enum class HostPadCommandKind {
    Digital = 0,
    Analog = 1,
};

// Players an embedded host may address. Four, because a Multitap in port 1 carries four
// pads (PSXI_MULTITAP_SLOTS); with no tap only player 0 has anywhere to go and the rest are
// dropped by psx_pad_*_player().
constexpr int kHostMaxPlayers = 4;

struct HostPadCommand {
    HostPadCommandKind kind = HostPadCommandKind::Digital;
    uint32_t mask = 0;   // Digital: PSXI_SW_SDA_* bit.
    bool pressed = false; // Digital.
    int stick = 0;        // Analog: 0 = left, 1 = right.
    int x = 0x80;         // Analog: 0x00..0xFF centred on 0x80.
    int y = 0x80;
    // Which player behind the port-1 multitap this is for (0..3). Always 0 without a tap,
    // and psx_pad_*_player() drops anything above 0 in that case, so a routing mistake shows
    // up as a dead player rather than two players sharing one pad.
    int player = 0;
};

// Presentation-only live overrides. Unlike everything else the host parks below, these need
// no VM cooperation at all — the emulation thread just reads them on its way into present() —
// so they are plain atomics rather than queued requests. -1 means "no override, use
// settings.toml".
std::atomic<int> g_host_display_aspect{-1};

/* Custom display aspect (width/height) for display_aspect == 3. Stored as a float rather
   than a ratio pair because that is all compute_dst consumes; <= 0 means "no live override,
   use settings.toml". */
std::atomic<float> g_host_display_aspect_custom{0.0f};

/* Live integer-scaling override. -1 = no override, use settings.toml. */
std::atomic<int> g_host_integer_scaling{-1};
/* Portrait layout. -1 on portrait_top means "follow settings"; there is no settings key today,
   so it resolves to ON — top-aligning is what the touch overlay assumes. */
std::atomic<int> g_host_portrait_top{-1};
std::atomic<int> g_host_portrait_top_inset{0};

/* Sanity bounds for a user-entered ratio. Wide enough for anything anyone would want
   (1:2 through 4:1) and narrow enough that a typo cannot produce a degenerate dst rect. */
constexpr float kMinCustomAspect = 0.5f;
constexpr float kMaxCustomAspect = 4.0f;

// App is off-screen: skip the whole draw+post. A plain atomic rather than a parked request,
// because the loop reads it every tick and there is nothing to sequence against the
// emulation thread — it only gates presentation, never machine state.
std::atomic<bool> g_host_presentation_suspended{false};
std::atomic<int> g_host_stretch_mode{-1};

/* Rewind ENGAGEMENT, as opposed to its configuration above. Plain atomics rather than queued
   requests, and deliberately: a hold-to-rewind button is read once per frame by the emulation
   thread on its way into ArmsxSession::runFrame(), exactly like the presentation flags. Going
   through the request lock would put the input path behind the same mutex as disc swaps.
     active — the button is held; every frame steps one snapshot back.
     steps  — one-shot steps (a menu row / a tap), drained in the same place. */
std::atomic<bool> g_host_rewind_active{false};
std::atomic<int> g_host_rewind_step_requests{0};

/* ---- [video] display features the PRESENT path consumes, as live overrides ----------------
   Same contract as g_host_stretch_mode above: -1 means "no host has spoken, defer to
   settings.toml", anything else is the host's choice and outranks the file. The Android
   front-end pushes all three through JNI before the core ever opens settings.toml, so a
   file-derived value must never overwrite them — that is the per-game-INI-shadows-the-UI
   failure in another shape. Read on the presenting thread once per frame. */
/* Texture dumping / replacement (psx/texrep.h), requested from the UI thread and applied on
   the EMULATION thread once per frame (ArmsxApp::updateTexture). psx_texrep_configure() frees
   every decoded replacement, so calling it from the JNI thread while the rasterizers hold
   psx_gpu_t::texrep_bind.img would be a use-after-free. -1 on the two ints means "no request
   pending"; the string is guarded by its own mutex because it is not trivially atomic. */
std::mutex g_host_texture_lock;
std::string g_host_texture_dir;
std::atomic<int> g_host_texture_dump{-1};
std::atomic<int> g_host_texture_replace{-1};
std::atomic<bool> g_host_texture_pending{false};

std::atomic<int> g_host_deinterlace{-1};      // 0 weave, 1 bob, 2 adaptive
std::atomic<int> g_host_overscan_crop{-1};    // 0 none, 1 small, 2 full
std::atomic<int> g_host_display_rotation{-1}; // quarter turns clockwise 0..3

std::mutex g_host_control_lock;
bool g_host_embedded = false;
bool g_host_pause_pending = false;
bool g_host_pause_value = false;
bool g_host_audio_suspend_pending = false;
bool g_host_audio_suspend_value = false;
bool g_host_shutdown_pending = false;
bool g_host_reset_pending = false;
bool g_host_fast_forward_pending = false;
bool g_host_fast_forward_value = false;
bool g_host_speed_limits_pending = false;
bool g_host_speed_limit_frame_limit = true;
int g_host_speed_limit_percent = 100;
int g_host_speed_limit_fps = 0;
double g_host_speed_limit_fast_forward = 2.0;
int g_host_speed_limit_frame_skip = 0;
bool g_host_screenshot_pending = false;
std::string g_host_screenshot_path;
/* Disc swap. Parked here rather than performed on the calling (UI) thread: psx_swap_disc()
   reopens the image under the running machine, and doing that while the emulation thread is
   mid-instruction is a use-after-free waiting to happen. Drained at the same instruction
   boundary as every other host request. */
bool g_host_disc_swap_pending = false;
std::string g_host_disc_swap_path;
std::vector<HostPadCommand> g_host_pad_queue;
/* [input] multitap, live. Parked like every other request that touches the machine: the
   toggle destroys and rebuilds the device the SIO reads through, which is not something to
   do from the UI thread mid-instruction. */
bool g_host_multitap_pending = false;
bool g_host_multitap_value = false;
/* [emulation] rewind / runahead configuration, same reasoning: psx_rewind_configure() frees
   and allocates the snapshot ring. */
bool g_host_rewind_config_pending = false;
bool g_host_rewind_config_enabled = false;
int g_host_rewind_config_seconds = PSX_REWIND_DEFAULT_SECONDS;
int g_host_rewind_config_frequency = PSX_REWIND_DEFAULT_FREQUENCY;
bool g_host_runahead_pending = false;
int g_host_runahead_value = 0;
void (*g_host_present_callback)(void*) = nullptr;
void* g_host_present_user = nullptr;

std::atomic_bool g_host_vm_active{false};
std::atomic_bool g_host_loop_running{false};
// Set by an embedded host that has wired up everything SDL's non-OpenSL-ES Android audio
// backends need (SDLAudioManager's JNI glue AND its static Context). Until it is, the
// [audio] driver setting is clamped to openslES — see ArmsxApp::applyAudioDriverSetting().
std::atomic_bool g_host_audio_backends_ready{false};

// Read-only session telemetry for an embedded host. The Android front-end draws its OSD in
// Compose (there is no imgui/FSUI left to draw one natively), so the numbers the core used to
// print for itself have to be published instead. Plain atomics rather than the request lock:
// they are written once per frame here and polled from the UI thread, and a torn read of a
// frame rate is worth less than making the OSD contend with the emulation loop.
std::atomic<float> g_host_measured_fps{0.0f};
std::atomic<float> g_host_nominal_fps{0.0f};
std::atomic<unsigned int> g_host_presented_frames{0u};

// The full performance overlay (per-subsystem work counters + host phase timing) is opt-in
// and OFF here, so a player who never opens it pays for none of it: this flag also arms the
// core-side counters through psx_perf_set_enabled(), and the phase timers below are skipped
// entirely while it is clear. PSXE_HOST_STAT_* indices, frontend/host_stats.h.
std::atomic_bool g_host_stats_enabled{false};
std::mutex g_host_stats_lock;
double g_host_stats[PSXE_HOST_STAT_COUNT] = {};
// Has a window ever closed and published into g_host_stats since the overlay was last armed?
// psxe_host_stats() is documented to write nothing until it has, and the UI needs that: an
// unpublished snapshot is all zeroes, and a screenful of zeroes reads as a measurement ("your
// CPU is doing nothing") rather than as no data. Guarded by g_host_stats_lock.
bool g_host_stats_published = false;

// `perf_log`, a marker file next to the diag log (the same mechanism frontend/gpu_hw_gl.c
// uses for its A/B switches — `adb shell run-as com.nanodata.armsx touch files/logs/perf_log`).
// When it is present the overlay is armed from here and its snapshot is written to the diag
// log every window, which is what makes a measurement a number in a file rather than a
// screenshot to be read by eye. Absent — the shipping default — nothing below runs at all.
// Evaluated once, on first use, so it is read after psxe_diag_initialize() has a path.
bool perfLogEnabled() {
    static const bool enabled = [] {
        const char* log_path = psxe_diag_log_path();
        if (!log_path || !log_path[0]) {
            return false;
        }
        const char* slash = std::strrchr(log_path, '/');
        if (!slash) {
            return false;
        }
        std::string path(log_path, static_cast<size_t>(slash - log_path) + 1u);
        path += "perf_log";
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        std::fclose(file);
        return true;
    }();

    return enabled;
}

/*
    `resume_probe` — the marker that settles "which side came back empty" after a task switch.

    A black picture after returning from the background has two candidate causes that look
    identical from the outside, and reading the code cannot separate them:

      * the SOURCE is empty — the GLES rasterizer's render target (or the software framebuffer)
        lost its contents, so a correct present layer is faithfully presenting nothing;
      * the PRESENT side is empty — the source is intact and the window / swapchain / presented
        image is what came back dead.

    Armed, the first few frames the frontend uploads after presentation resumes are scanned for
    a single non-zero byte and the answer is written to the diag log:

        nonzero=0    the source came back empty  -> the rasterizer side
        nonzero>0    the source is intact        -> the present side

    `touch files/logs/resume_probe` to arm; absent (the shipping default) this costs one integer
    compare per frame and nothing else. Same marker mechanism as `perf_log` above and as
    frontend/gpu_hw_gl.c's gl_debug_marker().
*/
bool resumeProbeEnabled() {
    static const bool enabled = [] {
        const char* log_path = psxe_diag_log_path();
        if (!log_path || !log_path[0]) {
            return false;
        }
        const char* slash = std::strrchr(log_path, '/');
        if (!slash) {
            return false;
        }
        std::string path(log_path, static_cast<size_t>(slash - log_path) + 1u);
        path += "resume_probe";
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        std::fclose(file);
        return true;
    }();

    return enabled;
}

// Frames still to be sampled by the probe above. Set on the suspended -> resumed edge by the
// main loop, consumed in ArmsxSession::updateTexture(). Emulation thread only.
int g_resume_probe_frames = 0;
// Previous value of g_host_presentation_suspended as the loop last saw it, so the edge can be
// detected without another atomic.
bool g_presentation_was_suspended = false;

// Invoked on the emulation thread once per frame, right after the frame has been
// presented into the (external) renderer. The Android host uses it to push the software
// framebuffer into its ANativeWindow.
void HostNotifyFramePresented() {
    void (*callback)(void*) = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_host_control_lock);
        callback = g_host_present_callback;
        user = g_host_present_user;
    }

    if (callback) {
        callback(user);
    }
}

constexpr double kUiFrameRate = 60.0;

// Frame skip ([runtime] frame_skip). Presentation only — see ArmsxApp::shouldSkipPresent().
//
// kMaxFrameSkip caps the fixed modes at "present 1 of every 6", which is ~10 fps on screen for
// a 60 Hz game: past that the picture stops being usable and the remaining saving is small,
// because what is left in the frame is emulation, not presentation.
//
// kAdaptiveFrameSkipRun is the FLOOR the adaptive mode may never go below: at most two dropped
// presents in a row, i.e. never worse than 1-in-3 (~20 fps at 60). Adaptive reacts to lateness,
// and a device that is late every single frame would otherwise stop drawing entirely.
constexpr int kMaxFrameSkip = 5;
constexpr int kAdaptiveFrameSkipRun = 2;

const char* LogLevelTitle(int level) {
    switch (level) {
        case LOG_TRACE: return "trace";
        case LOG_DEBUG: return "debug";
        case LOG_INFO: return "info";
        case LOG_WARN: return "warn";
        case LOG_ERROR: return "error";
        case LOG_FATAL: return "fatal";
        default: return "unknown";
    }
}

void StructuredLogCallback(log_Event* ev) {
    if (!ev) {
        return;
    }

    // log_log() gates only its own stderr path on the level (psx/log.c); extra callbacks are
    // filtered by the level they were registered with, and this one is registered at LOG_TRACE
    // because it is installed before settings are loaded. Without this check log_set_level()
    // does nothing for the diag file, and the per-frame register traces in dma.c/gpu.c/cpu.c
    // land in armsx.log at ~850 lines/s on the emulation thread whatever log_level says.
    if (ev->level < log_get_level()) {
        return;
    }

    char message[4096] = {};
    vsnprintf(message, sizeof(message), ev->fmt, ev->ap);
    psxe_diag_logf("psx", "%s %s:%d %s", LogLevelTitle(ev->level), ev->file, ev->line, message);
}

void SdlLogOutput(void*, int category, SDL_LogPriority priority, const char* message) {
    psxe_diag_logf("sdl", "category=%d priority=%d %s", category, static_cast<int>(priority), message ? message : "");
}

[[noreturn]] void ReportNativeCrash(const char* reason);

#if defined(_WIN32) && !defined(UWP_TARGET)
LONG WINAPI WindowsUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_info) {
    const DWORD code = exception_info && exception_info->ExceptionRecord
        ? exception_info->ExceptionRecord->ExceptionCode
        : 0u;
    char reason[64] = {};
    SDL_snprintf(reason, sizeof(reason), "SEH 0x%08lx", static_cast<unsigned long>(code));
    ReportNativeCrash(reason);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

double CounterTicksToMilliseconds(uint64_t ticks) {
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        return 0.0;
    }

    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(frequency);
}

std::string RendererFlagsTitle(Uint32 flags) {
    std::string title;

    auto append = [&](const char* value) {
        if (!title.empty()) {
            title.append("|");
        }
        title.append(value);
    };

    if (flags & SDL_RENDERER_SOFTWARE) {
        append("software");
    }
    if (flags & SDL_RENDERER_ACCELERATED) {
        append("accelerated");
    }
    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        append("present-vsync");
    }
    if (flags & SDL_RENDERER_TARGETTEXTURE) {
        append("target-texture");
    }

    if (title.empty()) {
        title = "none";
    }

    return title;
}

[[noreturn]] void ReportNativeCrash(const char* reason);
void WriteNativeStackTraceImpl();

enum class LaunchKind {
    None,
    Bios,
    Disc,
    Exe,
};

const char* CpuEngineTitle(psx_cpu_execution_mode_t mode) {
    return mode == PSX_CPU_INTERPRETER ? "Interpreter" : "Cached interpreter";
}

const char* CpuEngineSettingToken(psx_cpu_execution_mode_t mode) {
    return mode == PSX_CPU_INTERPRETER ? "interpreter" : "cached";
}

std::optional<psx_cpu_execution_mode_t> ParseCpuEngine(const char* value) {
    if (!value || !value[0]) {
        return std::nullopt;
    }

    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered == "interpreter" || lowered == "reference") {
        return PSX_CPU_INTERPRETER;
    }
    if (lowered == "cached" || lowered == "cached-interpreter") {
        return PSX_CPU_CACHED_INTERPRETER;
    }
    return std::nullopt;
}

const char* LaunchKindTitle(LaunchKind kind) {
    switch (kind) {
        case LaunchKind::Bios: return "bios";
        case LaunchKind::Disc: return "disc";
        case LaunchKind::Exe: return "exe";
        case LaunchKind::None:
        default:
            return "none";
    }
}

struct CliFlags {
    bool bios = false;
    bool bios_folder = false;
    bool model = false;
    bool region = false;
    bool scale = false;
    bool settings_file = false;
    bool quiet = false;
    bool log_level = false;
    bool exp_rom = false;
    bool exe = false;
    bool cdrom = false;
    bool cpu_engine = false;
    bool has_boot_request = false;
};

struct LaunchRequest {
    LaunchKind kind = LaunchKind::None;
    std::filesystem::path path;
    std::string label;
};

void EnqueuePendingLaunchArgument(std::string argument) {
    if (argument.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    g_pending_launch_arguments.push_back(std::move(argument));
}

std::vector<std::string> DrainPendingLaunchArguments() {
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    std::vector<std::string> pending;
    pending.swap(g_pending_launch_arguments);
    return pending;
}

void EnqueueWebError(std::string message) {
    if (message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    g_pending_web_errors.push_back(std::move(message));
}

std::vector<std::string> DrainWebErrors() {
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    std::vector<std::string> pending;
    pending.swap(g_pending_web_errors);
    return pending;
}

// Local replacement for fsui::UiState after the FSUI cut. Holds only the fields the non-menu
// code still reads (library scan paths + sort/view prefs persisted in settings.toml, and the two
// overlay flags a couple of call sites poke). Menus are owned by the Jetpack Compose front-end now.
struct ArmsxUiState {
    int default_game_view = 0;
    int game_sort = 2;
    bool game_sort_reverse = false;
    std::vector<std::filesystem::path> game_list_paths;
    std::vector<std::filesystem::path> game_list_recursive_paths;
    std::filesystem::path covers_path;
    bool show_settings_overlay = false;
    bool show_performance_overlay = false;
};

// Local replacement for fsui::CurrentGameInfo after the FSUI cut (used only by crash-context logging).
struct ArmsxGameInfo {
    bool has_game = false;
    std::string title;
    std::string subtitle;
    std::string title_id;
    std::filesystem::path path;
};

struct FrontendSettings {
    std::string settings_path;
    std::string bios_override;
    std::string bios_search = "bios";
    std::string model = "scph1001";
    std::string region = "auto";
    std::string exp_path;
    std::string default_exe_path;
    int scale = 3;
    int log_level = LOG_INFO;
    bool quiet = true;
    bool logging_enabled = false;
    bool vsync_enabled = DefaultVsyncEnabled();
    psx_cpu_execution_mode_t cpu_engine = PSX_CPU_CACHED_INTERPRETER;
#ifdef USE_HARDWARE
    GpuBackend gpu_backend = GpuBackend::Software;
    // Rasterizer, orthogonal to gpu_backend above (which is the *presentation* path).
    // false keeps psx/dev/gpu.c's software rasterizer, which is the default.
    bool hw_rasterizer = false;
    // 1 = auto (GLES when a GL context exists, CPU otherwise), 2 = force CPU, 3 = force
    // GLES. See config.h; the UI only ever writes 0 or 1.
    int rasterizer_mode = 0;
    int internal_scale = 1;
#endif
    // Opt-in GPU accuracy fixes. Both change how games look relative to every previous
    // release, so both default off and both apply to the software path and any backend
    // alike (psx_gpu_set_accuracy_flags) so the 1x parity gate holds either way.
    bool accurate_mask_bit = false;
    bool accurate_dither = false;
    // Hardware's polygon size cull (1023x511). See psx_gpu_prim_oversize().
    bool accurate_prim_size = false;
    // Hardware's truncating texture blend. See psx_gpu_modulate_channel().
    bool accurate_tex_modulate = false;
    // PGXP (psx/pgxp.c): sub-pixel vertex precision. Core-global and default off;
    // only the hardware rasterizer backends consume the precise coordinates.
    bool pgxp = false;
    /* ---- [video] display/video feature set. All default OFF/neutral; the native gates below
       are only reachable because these defaults match frontend/config.c's exactly. */
    bool widescreen_hack = false;
    int texture_filter = 0;    // 0 nearest, 1 bilinear, 2 xBR-style (GLES rasterizer)
    /* Texture dumping / replacement (psx/texrep.h). Both default OFF and the subsystem
       allocates nothing until one is on. texture_dir is the BASE folder; empty means
       "<prefs>/textures", which ResolveTextureDir() below fills in. */
    bool texture_dump = false;
    bool texture_replacements = false;
    std::string texture_dir;
    int downsample = 0;        // 0 off, 2..8 box factor (GLES rasterizer, above 1x)
    int deinterlace = 0;       // 0 weave, 1 bob, 2 adaptive (480-line modes only)
    int overscan_crop = 0;     // 0 none, 1 small, 2 full
    int display_rotation = 0;  // quarter turns clockwise, 0..3
    int line_detect = 0;       // 0 disabled, 1 quads, 2 basic (GLES rasterizer)
    bool texture_scale_mode = false;
    bool debug_panel = false;
    bool stretch_mode = false;
    int display_aspect = 0;
    // Width/height for display_aspect == 3. 16:9 as the starting point because that is what
    // a user reaching for "custom" on a widescreen handheld most often wants.
    float display_aspect_custom = 16.0f / 9.0f;
    // Snap the output to a whole multiple of the source. Off by default: it shrinks the
    // picture (a 1920x1080 window fits only 4x of a 240-line frame = 960 tall), which is
    // a deliberate trade the user opts into, not something to impose.
    bool integer_scaling = false;
    int upscale_height = 480;
    // [runtime] frame pacing. frame_limit gates the limiter, speed_percent scales the game's
    // own rate, fps_limit is an optional absolute ceiling on top (0 = none), and
    // fast_forward_speed replaces speed_percent while fast-forward is engaged. A 0 multiplier
    // (or frame_limit = false) means uncapped: the pacer stops waiting entirely.
    bool frame_limit = true;
    int speed_percent = 100;
    int fps_limit = 0;
    double fast_forward_speed = 2.0;
    // [runtime] frame_skip. Orthogonal to the four above: they decide how fast the MACHINE
    // runs, this decides how many of the frames it produces are actually put on screen.
    // 0 = off, 1..5 = fixed (present one, skip N), -1 = adaptive. See ArmsxApp::shouldSkipPresent().
    int frame_skip = 0;
    // [runtime] host CPU scheduling levers, both EXPERIMENTAL and both off by default. Neither
    // touches emulation: adpf_clock_hint reports per-frame work to Android's performance-hint
    // service, affinity_mode pins the emulation thread to a core cluster. See perf_hint.h.
    bool adpf_clock_hint = false;
    int affinity_mode = 0;
    // [audio]. Consumed by ArmsxSession's SDL device + mixer and by psx/dev/spu.c; see the
    // per-field notes in frontend/config.h.
    int audio_volume = 100;
    int audio_ff_volume = 100;
    bool audio_muted = false;
    bool audio_mute_fast_forward = false;
    bool audio_swap_channels = false;
    bool audio_skip_reverb = false;
    int audio_buffer_ms = 13;
    int audio_driver = 1; // 0=SDL default, 1=openslES, 2=aaudio (Android only)
    // Opt-in: keep emulating and playing while the app is off-screen. Off means the host's
    // background notification parks the VM and stops the platform audio stream.
    bool audio_background_playback = false;
    // [input] analog_mode_default — which mode the emulated DualShock reports at boot.
    // The pad's ANALOG button still toggles at runtime; this is only the starting state.
    bool analog_mode_default = true;
    // [input] multitap — a Multitap in port 1 (psx/input/multitap.c), four players on one
    // port. Off by default: a tap answers the pad poll with its own ID, so a game that does
    // not understand one sees NO controller at all.
    bool multitap = false;
    // [emulation] rewind / runahead. Both off by default and both documented in
    // psx/rewind.h; rewind_seconds x rewind_frequency is a MEMORY figure (~3.5 MiB each).
    bool rewind = false;
    int rewind_seconds = PSX_REWIND_DEFAULT_SECONDS;
    int rewind_frequency = PSX_REWIND_DEFAULT_FREQUENCY;
    int runahead = 0;
    // [cheats] — GameShark codes, applied once per frame by psx/cheats.c. `cheats_file` is a
    // path to a `.cht` catalogue and `cheats_enabled_codes` names which of its entries are
    // armed. Both are PER-GAME values written by the launcher, which is deliberate: an
    // enablement list that could end up in a global layer is how the sibling PS2 project
    // armed "Infinite Health" in every game a user owned. Empty by default, so a settings.toml
    // written before this section existed arms nothing.
    bool cheats_enabled = false;
    std::string cheats_file;
    std::vector<std::string> cheats_enabled_codes;
    ArmsxUiState ui_state{};
};

struct PendingChordButton {
    bool physical_down = false;
    bool pending = false;
    bool forwarded = false;
    Uint32 pending_since = 0;
};

enum class FsuiWindowState {
    None,
    Landing,
    StartGame,
    Exit,
    GameList,
    Settings,
    PauseMenu,
};

std::string ToLower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string Trim(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();

    while ((begin < end) && std::isspace(static_cast<unsigned char>(value[begin]))) {
        begin++;
    }

    while ((end > begin) && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string NormalizeModel(std::string_view value) {
    std::string out;

    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    return out;
}

std::string DefaultSettingsPath() {
    const char* pref = psxe_cfg_get_pref_path();

    if (pref && pref[0]) {
        return std::string(pref) + "settings.toml";
    }

    return "settings.toml";
}

std::filesystem::path DefaultBrowseDirectory() {
    const char* pref = psxe_cfg_get_pref_path();

    if (pref && pref[0]) {
        return std::filesystem::path(pref);
    }

    return std::filesystem::current_path();
}

std::filesystem::path DefaultDiagnosticsLogPath() {
    std::filesystem::path path = DefaultBrowseDirectory() / "logs";
#if defined(UWP_TARGET)
    path /= "armsx-uwp.log";
#else
    path /= "armsx.log";
#endif
    return path;
}

std::string AppendBrowseRootHint(std::string summary) {
#if defined(_WIN32)
    if (!summary.empty() && summary.back() != ' ') {
        summary.push_back(' ');
    }
    summary += "Use Filesystem Roots or Parent Directory to switch drives.";
#endif
    return summary;
}

bool PathsMatch(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    return ToLower(left.lexically_normal().generic_string()) == ToLower(right.lexically_normal().generic_string());
#else
    return left.lexically_normal() == right.lexically_normal();
#endif
}

bool PathListContains(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
    return std::any_of(paths.begin(), paths.end(), [&](const std::filesystem::path& path) {
        return PathsMatch(path, candidate);
    });
}

bool FileContainsCaseInsensitive(const std::filesystem::path& path, const std::string& needle_lower) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return ToLower(content).find(needle_lower) != std::string::npos;
}

bool CueDirectoryReferencesImage(const std::filesystem::path& image_path) {
    if (!image_path.has_parent_path()) {
        return false;
    }

    const std::string needle = ToLower(image_path.filename().string());
    std::error_code ec;

    for (const auto& item : std::filesystem::directory_iterator(image_path.parent_path(), ec)) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file()) {
            continue;
        }
        if (ToLower(item.path().extension().string()) != ".cue") {
            continue;
        }
        if (FileContainsCaseInsensitive(item.path(), needle)) {
            return true;
        }
    }

    return false;
}

bool IsLikelyBiosImagePath(const std::filesystem::path& path, const FrontendSettings& settings) {
    const std::string ext = ToLower(path.extension().string());
    if (ext != ".bin" && ext != ".rom") {
        return false;
    }

    if (!settings.bios_override.empty() && PathsMatch(path, std::filesystem::path(settings.bios_override))) {
        return true;
    }

    const std::string stem = NormalizeModel(path.stem().string());
    if (stem == "bios" || stem == "biosbin") {
        return true;
    }
    if (stem.starts_with("scph")) {
        return true;
    }

    return false;
}

std::string NormalizePathString(std::string value) {
#if defined(_WIN32)
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':') {
        value.push_back('/');
    }
#endif
    return value;
}

bool StartsWithCaseInsensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }

    for (size_t index = 0; index < prefix.size(); index++) {
        const unsigned char lhs = static_cast<unsigned char>(value[index]);
        const unsigned char rhs = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

int HexValue(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

std::string PercentDecode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t index = 0; index < value.size(); index++) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if ((ch == '%') && ((index + 2) < value.size())) {
            const int hi = HexValue(static_cast<unsigned char>(value[index + 1]));
            const int lo = HexValue(static_cast<unsigned char>(value[index + 2]));
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                index += 2;
                continue;
            }
        }

        decoded.push_back(ch == '+' ? ' ' : static_cast<char>(ch));
    }

    return decoded;
}

std::optional<std::string> NormalizedPickerSelection(const std::string& path) {
    const std::string trimmed = Trim(path);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    return NormalizePathString(trimmed);
}

std::string WithHiddenId(std::string_view label, std::string_view id) {
    std::string out(label);
    out += "##";
    out += id;
    return out;
}

// The FSUI app-icon lookup (ReadSdlFileBytes / MaterializeBundledFsuiAppIcon /
// ResolveFsuiAppIconPath) was removed with the FSUI cut. It was dead code — nothing called
// ResolveFsuiAppIconPath — but it was a landmine: ReadSdlFileBytes used SDL_RWFromFile with a
// RELATIVE path, and on the in-process Android host SDL falls through to Android_JNI_FileOpen,
// which aborts the process ("CallStaticObjectMethod received NULL jclass") because SDLActivity
// glue is never registered when Compose owns the Activity. Use POSIX stat/fopen here, never SDL_RW*.

bool IsDiscPath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".cue" || ext == ".bin" || ext == ".iso" || ext == ".img"
#ifdef USE_CHD
        || ext == ".chd"
#endif
        ;
}

bool IsExePath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".exe" || ext == ".ps-exe" || ext == ".psexe";
}

bool IsZipPath(const std::filesystem::path& path) {
    return armsx::IsZipPath(path);
}

std::string StemToTitle(const std::filesystem::path& path) {
    std::string value = path.stem().string();

    if (value.empty()) {
        value = path.filename().string();
    }

    std::replace(value.begin(), value.end(), '_', ' ');

    return Trim(value);
}

/* Empty means "derive it": <prefs>/textures, beside the other per-user data. Android never
   takes that branch — the app pushes an explicit per-game path, because only it knows the
   disc serial and the core has no business guessing one. */
std::string ResolveTextureDir(const std::string& configured) {
    if (!configured.empty()) {
        return configured;
    }

    const char* prefs = psxe_cfg_get_pref_path();

    if (prefs && *prefs) {
        return (std::filesystem::path(prefs) / "textures").string();
    }

    return "textures";
}

std::string EscapeTomlString(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);

    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }

    return out;
}

const char* AspectToString(int aspect) {
    switch (aspect) {
        case 1: return "square";
        case 2: return "wide16x9";
        case 3: return "custom";
        default: return "classic";
    }
}

const char* AspectTitle(int aspect) {
    switch (aspect) {
        case 1: return "Square";
        case 2: return "Wide 16:9";
        default: return "Classic";
    }
}

/* ---- [video] display/video feature tokens. Must match frontend/config.c's parser exactly:
   these are what SaveSettings() writes back, and an unrecognised token silently reverts the
   setting to its default the next time the file is read. */
const char* TextureFilterToString(int mode) {
    switch (mode) {
        case 1: return "bilinear";
        case 2: return "xbr";
        default: return "nearest";
    }
}

const char* DeinterlaceToString(int mode) {
    switch (mode) {
        case 1: return "bob";
        case 2: return "adaptive";
        default: return "weave";
    }
}

const char* OverscanToString(int mode) {
    switch (mode) {
        case 1: return "small";
        case 2: return "full";
        default: return "none";
    }
}

const char* LineDetectToString(int mode) {
    switch (mode) {
        case 1: return "quads";
        case 2: return "basic";
        default: return "disabled";
    }
}

const char* UpscaleToString(int height) {
    switch (height) {
        case 720: return "720p";
        case 1080: return "1080p";
        case 1440: return "1440p";
        case 2160: return "2160p";
        default: return "480p";
    }
}

const char* AudioDriverToString(int driver) {
    switch (driver) {
        case 0: return "default";
        case 2: return "aaudio";
        default: return "opensles";
    }
}

// Always written with a decimal point so the value round-trips as a TOML float. (The reader
// takes a bare integer too, but the two writers of this file — SaveSettings here and
// Ps1SettingsStore on the Kotlin side — should agree on one shape.)
std::string FastForwardSpeedToString(double speed) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.2f", speed > 0.0 ? speed : 0.0);
    return buffer;
}

std::string BoolTitle(bool value) {
    return value ? "On" : "Off";
}

std::string RendererValueTitle(const FrontendSettings& settings) {
    std::string value = settings.texture_scale_mode ? "Linear" : "Nearest";

    if (settings.stretch_mode) {
        value += ", Stretch";
    } else {
        value += ", Fit";
    }

    return value;
}

#ifdef USE_HARDWARE
constexpr bool SupportsHardwareGpuBackend() {
    return true;
}

armsx_render_backend_t RenderBackendFor(GpuBackend backend) {
    switch (backend) {
        case GpuBackend::SDLAccelerated:
            return ARMSX_RENDER_BACKEND_SDL_ACCELERATED;
        case GpuBackend::OpenGL:
            return ARMSX_RENDER_BACKEND_OPENGL;
        case GpuBackend::Vulkan:
            return ARMSX_RENDER_BACKEND_VULKAN;
        case GpuBackend::Software:
        default:
            return ARMSX_RENDER_BACKEND_SDL_SOFTWARE;
    }
}

const char* GpuBackendTitle(GpuBackend backend) {
    return armsx_render_backend_name(RenderBackendFor(backend));
}

const char* GpuBackendSettingToken(GpuBackend backend) {
    return armsx_render_backend_token(RenderBackendFor(backend));
}

/* [video] renderer. "hardware" is the token the Compose UI writes and means AUTO; the two
   explicit ones only ever arrive from a hand-edited settings.toml and are round-tripped so
   an A/B choice is not silently collapsed the next time the UI saves. */
#ifdef USE_HARDWARE
const char* RendererSettingToken(const FrontendSettings& settings) {
    if (!settings.hw_rasterizer) {
        return "software";
    }

    switch (settings.rasterizer_mode) {
        case 2:  return "hardware-cpu";
        case 3:  return "hardware-gl";
        default: return "hardware";
    }
}
#endif

std::optional<GpuBackend> ParseGpuBackendOverride(const char* value) {
    if (!value || !value[0]) {
        return std::nullopt;
    }

    const std::string lowered = ToLower(value);
    if (lowered == "sdl-accelerated" || lowered == "hardware" || lowered == "hardware (experimental)" || lowered == "hw" || lowered == "hw-renderer") {
        return GpuBackend::SDLAccelerated;
    }

    if (lowered == "opengl" || lowered == "gl" || lowered == "gles" || lowered == "gles3" || lowered == "opengl-es") {
        return GpuBackend::OpenGL;
    }

    if (lowered == "vulkan" || lowered == "vk") {
        return GpuBackend::Vulkan;
    }

    if (lowered == "software" || lowered == "sw" || lowered == "sw-renderer") {
        return GpuBackend::Software;
    }

    return std::nullopt;
}
#endif

int RegionFromSettings(const FrontendSettings& settings) {
    const std::string region = ToLower(settings.region);

    if (region == "pal") {
        return CDR_REGION_EUROPE;
    }

    if (region == "ntsc") {
        const std::string model = NormalizeModel(settings.model);
        static const std::array<const char*, 8> japan_models = {
            "scph1000",
            "scph3000",
            "scph3500",
            "scph5000",
            "scph5500",
            "scph7000",
            "scph7003",
            "scph100",
        };

        if (std::find(japan_models.begin(), japan_models.end(), model) != japan_models.end()) {
            return CDR_REGION_JAPAN;
        }

        return CDR_REGION_AMERICA;
    }

    const std::string model = NormalizeModel(settings.model);
    static const std::array<const char*, 8> europe_models = {
        "scph1002",
        "scph5502",
        "scph5552",
        "scph7002",
        "scph7502",
        "scph9002",
        "scph102a",
        "scph102b",
    };
    static const std::array<const char*, 8> japan_models = {
        "scph1000",
        "scph3000",
        "scph3500",
        "scph5000",
        "scph5500",
        "scph7000",
        "scph7003",
        "scph100",
    };

    if (std::find(europe_models.begin(), europe_models.end(), model) != europe_models.end()) {
        return CDR_REGION_EUROPE;
    }

    if (std::find(japan_models.begin(), japan_models.end(), model) != japan_models.end()) {
        return CDR_REGION_JAPAN;
    }

    return CDR_REGION_AMERICA;
}

LaunchRequest LaunchForPath(const std::filesystem::path& path) {
    LaunchRequest request;
    if (path.empty()) {
        return request;
    }

    request.path = std::filesystem::path(NormalizePathString(path.string()));
    request.label = StemToTitle(request.path);

    if (IsExePath(request.path)) {
        request.kind = LaunchKind::Exe;
    } else if (IsDiscPath(request.path)) {
        request.kind = LaunchKind::Disc;
    }

    return request;
}

std::optional<LaunchKind> ParseUriLaunchKind(std::string_view value) {
    const std::string lower = ToLower(Trim(value));

    if (lower == "bios") {
        return LaunchKind::Bios;
    }
    if (lower == "disc" || lower == "cdrom") {
        return LaunchKind::Disc;
    }
    if (lower == "exe") {
        return LaunchKind::Exe;
    }

    return std::nullopt;
}

std::optional<std::string> QueryValue(std::string_view query, std::string_view key) {
    size_t begin = 0;

    while (begin <= query.size()) {
        const size_t end = query.find('&', begin);
        const std::string_view pair = query.substr(begin, end == std::string_view::npos ? query.size() - begin : end - begin);
        const size_t sep = pair.find('=');
        const std::string_view pair_key = pair.substr(0, sep);

        if (ToLower(PercentDecode(pair_key)) == ToLower(key)) {
            if (sep == std::string_view::npos) {
                return std::string();
            }
            return PercentDecode(pair.substr(sep + 1));
        }

        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }

    return std::nullopt;
}

std::string DecodeLaunchUriPathPayload(std::string_view payload) {
    std::string decoded = PercentDecode(payload);

    if (decoded.rfind("///", 0) == 0) {
        decoded.erase(0, 2);
    } else if (decoded.rfind("//", 0) == 0) {
        decoded.erase(0, 2);
    }

#if defined(_WIN32)
    if (decoded.size() >= 3 &&
        decoded[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(decoded[1])) &&
        decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }
#endif

    return NormalizePathString(decoded);
}

LaunchRequest LaunchForUri(std::string_view uri) {
    LaunchRequest request;

    std::string_view remainder = uri;
    if (StartsWithCaseInsensitive(uri, "armsx:")) {
        remainder = uri.substr(6);
    } else if (StartsWithCaseInsensitive(uri, "web+armsx:")) {
        remainder = uri.substr(10);
    } else {
        return request;
    }
    const size_t fragment_pos = remainder.find('#');
    if (fragment_pos != std::string_view::npos) {
        remainder = remainder.substr(0, fragment_pos);
    }

    std::string_view query;
    const size_t query_pos = remainder.find('?');
    if (query_pos != std::string_view::npos) {
        query = remainder.substr(query_pos + 1);
        remainder = remainder.substr(0, query_pos);
    }

    std::optional<LaunchKind> kind = std::nullopt;
    if (const std::optional<std::string> kind_value = QueryValue(query, "kind")) {
        kind = ParseUriLaunchKind(*kind_value);
    }

    if (kind == LaunchKind::Bios) {
        request.kind = LaunchKind::Bios;
        request.label = "PlayStation BIOS";
        return request;
    }

    std::string launch_path;
    if (const std::optional<std::string> query_path = QueryValue(query, "path")) {
        launch_path = NormalizePathString(*query_path);
    } else {
        launch_path = DecodeLaunchUriPathPayload(remainder);
    }

    const std::string launch_path_lower = ToLower(Trim(launch_path));
    if (launch_path_lower == "bios" || launch_path_lower == "/bios") {
        request.kind = LaunchKind::Bios;
        request.label = "PlayStation BIOS";
        return request;
    }

    request = LaunchForPath(std::filesystem::path(launch_path));
    if (request.kind == LaunchKind::None) {
        return request;
    }

    if (kind == LaunchKind::Disc || kind == LaunchKind::Exe) {
        request.kind = *kind;
    }

    return request;
}

LaunchRequest LaunchForArgument(std::string_view argument, std::optional<LaunchKind> forced_kind = std::nullopt) {
    LaunchRequest request;
    const std::string trimmed = Trim(argument);
    if (trimmed.empty()) {
        return request;
    }

    if (StartsWithCaseInsensitive(trimmed, "armsx:")) {
        request = LaunchForUri(trimmed);
    } else {
        const std::filesystem::path path(trimmed);
#ifdef USE_CHD
        if (IsZipPath(path)) {
            std::string archive_error;
            const auto extracted = armsx::ExtractZipLaunchCandidate(path, archive_error);
            if (!extracted.has_value()) {
                psxe_diag_logf(
                    "launch",
                    "ZIP launch failed path=%s error=%s",
                    path.string().c_str(),
                    archive_error.c_str()
                );
                return request;
            }
            request = LaunchForPath(*extracted);
        } else
#endif
        {
            request = LaunchForPath(path);
        }
    }

    if (request.kind == LaunchKind::None) {
        return request;
    }

    if (forced_kind == LaunchKind::Bios) {
        return LaunchRequest{.kind = LaunchKind::Bios, .path = {}, .label = "PlayStation BIOS"};
    }

    if ((forced_kind == LaunchKind::Disc || forced_kind == LaunchKind::Exe) && !request.path.empty()) {
        request.kind = *forced_kind;
    }

    return request;
}



#ifdef USE_HARDWARE
#endif





std::vector<std::string> ModelChoices() {
    return {
        "scph1000", "scph1001", "scph1002", "scph3000", "scph3500", "scph5000", "scph5500",
        "scph5501", "scph5502", "scph5552", "scph7000", "scph7001", "scph7002", "scph7003",
        "scph7501", "scph7502", "scph9002", "scph100", "scph101", "scph102a", "scph102b", "scph102c",
    };
}



std::time_t FileTimeToTimeT(const std::filesystem::file_time_type& value) {
    using namespace std::chrono;
    const auto adjusted = time_point_cast<system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + system_clock::now()
    );
    return system_clock::to_time_t(adjusted);
}

void CopyTomlString(const toml_datum_t& datum, std::string& out) {
    if (!datum.ok || !datum.u.s) {
        return;
    }

    out = datum.u.s;
    free(datum.u.s);
}

void CopyTomlPathString(const toml_datum_t& datum, std::string& out) {
    if (!datum.ok || !datum.u.s) {
        return;
    }

    out = NormalizePathString(datum.u.s);
    free(datum.u.s);
}

void CopyTomlArrayStrings(const toml_array_t* array, std::vector<std::filesystem::path>& out) {
    out.clear();

    if (!array) {
        return;
    }

    const int count = toml_array_nelem(array);
    out.reserve(static_cast<size_t>(count));

    for (int index = 0; index < count; index++) {
        toml_datum_t value = toml_string_at(array, index);

        if (!value.ok || !value.u.s) {
            continue;
        }

        out.emplace_back(NormalizePathString(value.u.s));
        free(value.u.s);
    }
}

/* The plain-text sibling of the above, for lists that are NOT paths. `[cheats] enabled_codes`
   holds cheat NAMES, so putting them through NormalizePathString() would quietly rewrite a
   name containing a backslash or a double space and the entry would then match nothing. */
void CopyTomlArrayPlainStrings(const toml_array_t* array, std::vector<std::string>& out) {
    out.clear();

    if (!array) {
        return;
    }

    const int count = toml_array_nelem(array);
    out.reserve(static_cast<size_t>(count));

    for (int index = 0; index < count; index++) {
        toml_datum_t value = toml_string_at(array, index);

        if (!value.ok || !value.u.s) {
            continue;
        }

        out.emplace_back(value.u.s);
        free(value.u.s);
    }
}

CliFlags ScanCliFlags(int argc, const char* argv[]) {
    CliFlags flags;

    for (int index = 1; index < argc; index++) {
        const std::string_view arg(argv[index] ? argv[index] : "");

        auto mark_value_opt = [&](const char* short_name, const char* long_name, bool* target) {
            const std::string_view short_opt(short_name);
            const std::string_view long_opt(long_name);

            if (arg == short_opt || arg == long_opt) {
                *target = true;
                index++;
                return true;
            }

            if (arg.starts_with(long_opt) && arg.size() > (long_opt.size() + 1) && arg[long_opt.size()] == '=') {
                *target = true;
                return true;
            }

            return false;
        };

        if (mark_value_opt("-b", "--bios", &flags.bios) ||
            mark_value_opt("-B", "--bios-folder", &flags.bios_folder) ||
            mark_value_opt("-M", "--model", &flags.model) ||
            mark_value_opt("-r", "--region", &flags.region) ||
            mark_value_opt("-s", "--scale", &flags.scale) ||
            mark_value_opt("-S", "--settings-file", &flags.settings_file) ||
            mark_value_opt("-L", "--log-level", &flags.log_level) ||
            mark_value_opt("-e", "--exp-rom", &flags.exp_rom) ||
            mark_value_opt("-x", "--exe", &flags.exe) ||
            mark_value_opt("", "--cpu-engine", &flags.cpu_engine) ||
            mark_value_opt("", "--cdrom", &flags.cdrom)) {
            continue;
        }

        if (arg == "-q" || arg == "--quiet") {
            flags.quiet = true;
            continue;
        }

        if (!arg.empty() && arg[0] != '-') {
            flags.cdrom = true;
        }
    }

    flags.has_boot_request = flags.cdrom || flags.exe;

    return flags;
}

/*
    `[cheats]` out of an already-parsed settings.toml.

    Split out of LoadExtraSettings() because SaveSettings() needs the same read: it rewrites
    the whole file from the snapshot the session booted with, so a cheat the user switched on
    from the settings screen mid-session would otherwise be written straight back out again.
    That is the exact failure the comment above RefreshSessionStartSettingsFromDisk()
    describes for the upscale rows, and there is no reason to learn it twice.
*/
void ReadCheatSettings(toml_table_t* root, FrontendSettings& settings) {
    toml_table_t* cheats = root ? toml_table_in(root, "cheats") : nullptr;

    if (!cheats) {
        return;
    }

    toml_datum_t enabled = toml_bool_in(cheats, "enabled");

    if (enabled.ok) {
        settings.cheats_enabled = enabled.u.b != 0;
    }

    toml_datum_t file = toml_string_in(cheats, "file");

    if (file.ok && file.u.s) {
        settings.cheats_file = file.u.s;
        free(file.u.s);
    }

    CopyTomlArrayPlainStrings(toml_array_in(cheats, "enabled_codes"), settings.cheats_enabled_codes);
}

/* Re-read `[cheats]` straight off disk. See ReadCheatSettings() for why. */
void RefreshCheatSettingsFromDisk(FrontendSettings& settings) {
    const std::filesystem::path target = settings.settings_path.empty()
                                             ? std::filesystem::path(DefaultSettingsPath())
                                             : std::filesystem::path(settings.settings_path);

    FILE* file = fopen(target.string().c_str(), "rb");

    if (!file) {
        return;
    }

    char error[256] = {};
    toml_table_t* root = toml_parse_file(file, error, sizeof(error));
    fclose(file);

    if (!root) {
        return;
    }

    ReadCheatSettings(root, settings);
    toml_free(root);
}

/*
    Load the game's cheat catalogue and arm the selected entries.

    Called once per boot, and again whenever the host asks for a live re-arm. The hardcore
    interlock is applied FIRST so that a build where hardcore is ever switched on cannot arm
    anything at all, rather than arming and relying on something further downstream to stop
    it.
*/
void ApplyCheatSettings(const FrontendSettings& settings) {
    psx_cheats_set_inhibited(armsx_ach_hardcore_active() ? 1 : 0);

    if (!settings.cheats_enabled || settings.cheats_file.empty()) {
        /* Clearing rather than leaving the previous game's catalogue in place: a list that
           outlived its disc is how a cheat ends up armed in a game it was never meant for. */
        psx_cheats_load_file("");
        psx_cheats_arm(0, nullptr, 0, nullptr);
        return;
    }

    if (psx_cheats_load_file(settings.cheats_file.c_str()) < 0) {
        psx_cheats_arm(0, nullptr, 0, nullptr);
        return;
    }

    std::vector<const char*> names;
    names.reserve(settings.cheats_enabled_codes.size());

    for (const std::string& name : settings.cheats_enabled_codes) {
        names.push_back(name.c_str());
    }

    int missing = 0;
    const int armed = psx_cheats_arm(1, names.empty() ? nullptr : names.data(),
                                     static_cast<int>(names.size()), &missing);

    if (missing > 0) {
        /* Said out loud rather than ignored. A selection that no longer matches its file is
           the most confusing way for this feature to fail — the switch is on and nothing
           happens — and it is exactly what renaming an entry in the .cht produces. */
        log_warn("cheats: %d selected entr%s not present in %s", missing, missing == 1 ? "y is" : "ies are",
                 settings.cheats_file.c_str());
    }

    (void)armed;
}

void LoadExtraSettings(FrontendSettings& settings, const CliFlags& cli) {
    if (settings.settings_path.empty()) {
        return;
    }

    FILE* file = fopen(settings.settings_path.c_str(), "rb");

    if (!file) {
        return;
    }

    char error[256] = {};
    toml_table_t* root = toml_parse_file(file, error, sizeof(error));
    fclose(file);

    if (!root) {
        return;
    }

    if (toml_table_t* bios = toml_table_in(root, "bios")) {
        if (!cli.bios_folder) {
            CopyTomlPathString(toml_string_in(bios, "search_path"), settings.bios_search);
        }

        if (!cli.model) {
            CopyTomlString(toml_string_in(bios, "preferred_model"), settings.model);
        }

        if (!cli.bios) {
            CopyTomlPathString(toml_string_in(bios, "override_file"), settings.bios_override);
        }
    }

    if (toml_table_t* console = toml_table_in(root, "console")) {
        if (!cli.region) {
            CopyTomlString(toml_string_in(console, "region"), settings.region);
        }
    }

    if (toml_table_t* runtime = toml_table_in(root, "runtime")) {
        if (!cli.scale) {
            toml_datum_t value = toml_int_in(runtime, "display_scale");
            if (value.ok) {
                settings.scale = static_cast<int>(value.u.i);
            }
        }

        if (!cli.log_level) {
            toml_datum_t value = toml_int_in(runtime, "log_level");
            if (value.ok) {
                settings.log_level = static_cast<int>(value.u.i);
            }
        }

        if (!cli.quiet) {
            bool resolved_logging_enabled = false;
            toml_datum_t logging_enabled = toml_bool_in(runtime, "logging_enabled");
            if (logging_enabled.ok) {
                settings.logging_enabled = logging_enabled.u.b != 0;
                settings.quiet = !settings.logging_enabled;
                resolved_logging_enabled = true;
            }

            toml_datum_t value = toml_bool_in(runtime, "quiet");
            if (!resolved_logging_enabled && value.ok) {
                settings.quiet = value.u.b != 0;
                settings.logging_enabled = !settings.quiet;
            }
        }

        toml_datum_t frame_limit = toml_bool_in(runtime, "frame_limit");
        if (frame_limit.ok) {
            settings.frame_limit = frame_limit.u.b != 0;
        }

        toml_datum_t speed_percent = toml_int_in(runtime, "speed_percent");
        if (speed_percent.ok) {
            settings.speed_percent = static_cast<int>(speed_percent.u.i);
        }

        toml_datum_t fps_limit = toml_int_in(runtime, "fps_limit");
        if (fps_limit.ok) {
            settings.fps_limit = static_cast<int>(fps_limit.u.i);
        }

        // Written as a float by the UI, but a hand-edited file may hold a bare integer and
        // toml_double_in only matches floats — try both rather than silently falling back to 2x.
        toml_datum_t ff_speed = toml_double_in(runtime, "fast_forward_speed");
        if (ff_speed.ok) {
            settings.fast_forward_speed = ff_speed.u.d;
        } else {
            toml_datum_t ff_speed_int = toml_int_in(runtime, "fast_forward_speed");
            if (ff_speed_int.ok) {
                settings.fast_forward_speed = static_cast<double>(ff_speed_int.u.i);
            }
        }

        toml_datum_t frame_skip = toml_int_in(runtime, "frame_skip");
        if (frame_skip.ok) {
            settings.frame_skip = static_cast<int>(frame_skip.u.i);
        }
    }

    if (toml_table_t* audio = toml_table_in(root, "audio")) {
        toml_datum_t volume = toml_int_in(audio, "volume");
        if (volume.ok) {
            settings.audio_volume = static_cast<int>(volume.u.i);
        }

        toml_datum_t ff_volume = toml_int_in(audio, "fast_forward_volume");
        if (ff_volume.ok) {
            settings.audio_ff_volume = static_cast<int>(ff_volume.u.i);
        }

        toml_datum_t muted = toml_bool_in(audio, "muted");
        if (muted.ok) {
            settings.audio_muted = muted.u.b != 0;
        }

        toml_datum_t mute_ff = toml_bool_in(audio, "mute_fast_forward");
        if (mute_ff.ok) {
            settings.audio_mute_fast_forward = mute_ff.u.b != 0;
        }

        toml_datum_t swap_channels = toml_bool_in(audio, "swap_channels");
        if (swap_channels.ok) {
            settings.audio_swap_channels = swap_channels.u.b != 0;
        }

        toml_datum_t skip_reverb = toml_bool_in(audio, "skip_reverb");
        if (skip_reverb.ok) {
            settings.audio_skip_reverb = skip_reverb.u.b != 0;
        }

        toml_datum_t buffer_ms = toml_int_in(audio, "buffer_ms");
        if (buffer_ms.ok) {
            settings.audio_buffer_ms = static_cast<int>(buffer_ms.u.i);
        }

        toml_datum_t driver = toml_string_in(audio, "driver");
        if (driver.ok && driver.u.s) {
            const std::string value = ToLower(driver.u.s);
            if (value == "aaudio") {
                settings.audio_driver = 2;
            } else if (value == "default" || value == "auto") {
                settings.audio_driver = 0;
            } else {
                settings.audio_driver = 1;
            }
            free(driver.u.s);
        }

        toml_datum_t background_playback = toml_bool_in(audio, "background_playback");
        if (background_playback.ok) {
            settings.audio_background_playback = background_playback.u.b != 0;
        }
    }

    if (toml_table_t* paths = toml_table_in(root, "paths")) {
        if (!cli.exp_rom) {
            CopyTomlPathString(toml_string_in(paths, "expansion_rom"), settings.exp_path);
        }

        if (!cli.exe) {
            CopyTomlPathString(toml_string_in(paths, "default_psx_exe"), settings.default_exe_path);
        }
    }

    if (toml_table_t* video = toml_table_in(root, "video")) {
        toml_datum_t vsync = toml_bool_in(video, "vsync");
        if (vsync.ok) {
            settings.vsync_enabled = vsync.u.b != 0;
        }

#ifdef USE_HARDWARE
        toml_datum_t gpu_backend = toml_string_in(video, "gpu_backend");
        if (gpu_backend.ok && gpu_backend.u.s) {
            if (std::optional<GpuBackend> backend = ParseGpuBackendOverride(gpu_backend.u.s)) {
                settings.gpu_backend = *backend;
            } else {
                settings.gpu_backend = GpuBackend::Software;
            }
            free(gpu_backend.u.s);
        }
#endif

        toml_datum_t texture_scale_mode = toml_bool_in(video, "texture_scale_mode");
        if (texture_scale_mode.ok) {
            settings.texture_scale_mode = texture_scale_mode.u.b != 0;
        }

        toml_datum_t debug_panel = toml_bool_in(video, "debug_panel");
        if (debug_panel.ok) {
            settings.debug_panel = debug_panel.u.b != 0;
        }

        toml_datum_t stretch_mode = toml_bool_in(video, "stretch_mode");
        if (stretch_mode.ok) {
            settings.stretch_mode = stretch_mode.u.b != 0;
        }

        toml_datum_t display_aspect = toml_string_in(video, "display_aspect");
        if (display_aspect.ok && display_aspect.u.s) {
            const std::string value = ToLower(display_aspect.u.s);
            if (value == "square") {
                settings.display_aspect = 1;
            } else if (value == "wide16x9") {
                settings.display_aspect = 2;
            } else if (value == "custom") {
                settings.display_aspect = 3;
            } else {
                settings.display_aspect = 0;
            }
            free(display_aspect.u.s);
        }

        toml_datum_t integer_scaling = toml_bool_in(video, "integer_scaling");
        if (integer_scaling.ok) {
            settings.integer_scaling = integer_scaling.u.b;
        }

        toml_datum_t display_aspect_custom = toml_double_in(video, "display_aspect_custom");
        if (display_aspect_custom.ok) {
            const float ratio = static_cast<float>(display_aspect_custom.u.d);
            // Ignore an out-of-range value rather than clamping it: a clamp would silently
            // present something the user did not ask for, which reads as "custom is broken".
            if (ratio >= kMinCustomAspect && ratio <= kMaxCustomAspect) {
                settings.display_aspect_custom = ratio;
            }
        }

        toml_datum_t wide_upscale = toml_string_in(video, "wide_upscale");
        if (wide_upscale.ok && wide_upscale.u.s) {
            const std::string value = ToLower(wide_upscale.u.s);
            if (value == "720p") settings.upscale_height = 720;
            else if (value == "1080p") settings.upscale_height = 1080;
            else if (value == "1440p") settings.upscale_height = 1440;
            else if (value == "2160p") settings.upscale_height = 2160;
            else settings.upscale_height = 480;
            free(wide_upscale.u.s);
        }
    }

    if (toml_table_t* input = toml_table_in(root, "input")) {
        toml_datum_t analog_mode_default = toml_bool_in(input, "analog_mode_default");
        if (analog_mode_default.ok) {
            settings.analog_mode_default = analog_mode_default.u.b != 0;
        }

        toml_datum_t multitap = toml_bool_in(input, "multitap");
        if (multitap.ok) {
            settings.multitap = multitap.u.b != 0;
        }
    }

    // [emulation]. An ABSENT table leaves every field at the struct default, which is what a
    // settings.toml written before these keys existed must resolve to.
    if (toml_table_t* emulation = toml_table_in(root, "emulation")) {
        toml_datum_t rewind = toml_bool_in(emulation, "rewind");
        if (rewind.ok) {
            settings.rewind = rewind.u.b != 0;
        }

        toml_datum_t rewind_seconds = toml_int_in(emulation, "rewind_seconds");
        if (rewind_seconds.ok) {
            settings.rewind_seconds = static_cast<int>(rewind_seconds.u.i);
        }

        toml_datum_t rewind_frequency = toml_int_in(emulation, "rewind_frequency");
        if (rewind_frequency.ok) {
            settings.rewind_frequency = static_cast<int>(rewind_frequency.u.i);
        }

        toml_datum_t runahead = toml_int_in(emulation, "runahead");
        if (runahead.ok) {
            settings.runahead = static_cast<int>(runahead.u.i);
        }
    }

    if (toml_table_t* library = toml_table_in(root, "library")) {
        CopyTomlArrayStrings(toml_array_in(library, "folders"), settings.ui_state.game_list_paths);
        CopyTomlArrayStrings(toml_array_in(library, "recursive_folders"), settings.ui_state.game_list_recursive_paths);
    }

    ReadCheatSettings(root, settings);

    if (toml_table_t* fsui_state = toml_table_in(root, "fsui")) {
        toml_datum_t default_view = toml_int_in(fsui_state, "default_game_view");
        toml_datum_t sort = toml_int_in(fsui_state, "game_sort");
        toml_datum_t reverse = toml_bool_in(fsui_state, "game_sort_reverse");

        if (default_view.ok) {
            settings.ui_state.default_game_view = static_cast<int>(default_view.u.i);
        }
        if (sort.ok) {
            settings.ui_state.game_sort = static_cast<int>(sort.u.i);
        }
        if (reverse.ok) {
            settings.ui_state.game_sort_reverse = reverse.u.b != 0;
        }
    }

    toml_free(root);
}

FrontendSettings BuildSettings(const psxe_config_t* cfg, const CliFlags& cli) {
    FrontendSettings settings;
#ifdef USE_HARDWARE
    std::optional<GpuBackend> env_gpu_backend;
#endif

    settings.settings_path = (cfg && cfg->settings_path && cfg->settings_path[0]) ? cfg->settings_path : DefaultSettingsPath();
    settings.bios_override = (cfg && cfg->bios && cfg->bios[0]) ? cfg->bios : "";
    settings.bios_search = (cfg && cfg->bios_search && cfg->bios_search[0]) ? cfg->bios_search : "bios";
    settings.model = (cfg && cfg->model && cfg->model[0]) ? cfg->model : "scph1001";
    settings.region = (cfg && cfg->region && cfg->region[0]) ? cfg->region : "auto";
    settings.exp_path = (cfg && cfg->exp_path && cfg->exp_path[0]) ? cfg->exp_path : "";
    settings.default_exe_path = (cfg && cfg->exe && cfg->exe[0]) ? cfg->exe : "";
    settings.scale = cfg ? cfg->scale : 3;
    settings.log_level = cfg ? cfg->log_level : LOG_INFO;
    settings.quiet = cfg ? (cfg->quiet != 0) : true;
    settings.logging_enabled = !settings.quiet;
    settings.vsync_enabled = cfg ? (cfg->vsync_enabled != 0) : DefaultVsyncEnabled();
    settings.cpu_engine = cfg && !cfg->cpu_engine ? PSX_CPU_INTERPRETER : PSX_CPU_CACHED_INTERPRETER;
    if (const char* env_cpu_engine = std::getenv("ARMSX_CPU_ENGINE")) {
        if (std::optional<psx_cpu_execution_mode_t> parsed = ParseCpuEngine(env_cpu_engine)) {
            settings.cpu_engine = *parsed;
        }
    }
#if defined(USE_HARDWARE)
    // psxe_config_t::gpu_backend is the same 0..3 encoding as GpuBackend (frontend/config.c).
    switch (cfg ? cfg->gpu_backend : 0) {
        case 1: settings.gpu_backend = GpuBackend::SDLAccelerated; break;
        case 2: settings.gpu_backend = GpuBackend::OpenGL; break;
        case 3: settings.gpu_backend = GpuBackend::Vulkan; break;
        default: settings.gpu_backend = GpuBackend::Software; break;
    }
    if (const char* env_backend = std::getenv("ARMSX_GPU_BACKEND")) {
        env_gpu_backend = ParseGpuBackendOverride(env_backend);
    }
    settings.accurate_mask_bit = cfg ? (cfg->accurate_mask_bit != 0) : false;
    settings.accurate_dither = cfg ? (cfg->accurate_dither != 0) : false;
    settings.accurate_prim_size = cfg ? (cfg->accurate_prim_size != 0) : false;
    settings.accurate_tex_modulate = cfg ? (cfg->accurate_tex_modulate != 0) : false;
    settings.pgxp = cfg ? (cfg->pgxp != 0) : false;
    settings.widescreen_hack = cfg ? (cfg->widescreen_hack != 0) : false;
    settings.texture_filter = cfg ? cfg->texture_filter : 0;
    settings.texture_dump = cfg ? (cfg->texture_dump != 0) : false;
    settings.texture_replacements = cfg ? (cfg->texture_replacements != 0) : false;
    settings.texture_dir = (cfg && cfg->texture_dir) ? cfg->texture_dir : "";
    settings.downsample = cfg ? cfg->downsample : 0;
    settings.deinterlace = cfg ? cfg->deinterlace : 0;
    settings.overscan_crop = cfg ? cfg->overscan_crop : 0;
    settings.display_rotation = cfg ? cfg->display_rotation : 0;
    settings.line_detect = cfg ? cfg->line_detect : 0;
    settings.hw_rasterizer = cfg ? (cfg->renderer != 0) : false;
    settings.rasterizer_mode = cfg ? cfg->renderer : 0;
    settings.internal_scale = cfg ? cfg->internal_scale : 1;
    if (settings.internal_scale < 1) {
        settings.internal_scale = 1;
    }
#endif
    settings.texture_scale_mode = cfg ? (cfg->texture_scale_mode != 0) : false;
    settings.debug_panel = cfg ? (cfg->debug_panel != 0) : false;
    settings.stretch_mode = cfg ? (cfg->stretch_mode != 0) : false;
    settings.display_aspect = cfg ? cfg->display_aspect : 0;
    if (cfg && cfg->display_aspect_custom > 0.0f)
        settings.display_aspect_custom = cfg->display_aspect_custom;
    settings.integer_scaling = cfg ? cfg->integer_scaling != 0 : false;
    settings.upscale_height = cfg ? cfg->upscale_height : 480;
    settings.analog_mode_default = cfg ? (cfg->analog_mode_default != 0) : true;
    settings.multitap = cfg ? (cfg->multitap != 0) : false;
    settings.rewind = cfg ? (cfg->rewind != 0) : false;
    settings.rewind_seconds = cfg ? cfg->rewind_seconds : PSX_REWIND_DEFAULT_SECONDS;
    settings.rewind_frequency = cfg ? cfg->rewind_frequency : PSX_REWIND_DEFAULT_FREQUENCY;
    settings.runahead = cfg ? cfg->runahead : 0;
    settings.frame_limit = cfg ? (cfg->frame_limit != 0) : true;
    settings.speed_percent = cfg ? cfg->speed_percent : 100;
    settings.fps_limit = cfg ? cfg->fps_limit : 0;
    settings.fast_forward_speed = cfg ? cfg->fast_forward_speed : 2.0;
    settings.frame_skip = cfg ? cfg->frame_skip : 0;
    settings.adpf_clock_hint = cfg ? (cfg->adpf_clock_hint != 0) : false;
    settings.affinity_mode = cfg ? cfg->affinity_mode : 0;
    settings.audio_volume = cfg ? cfg->audio_volume : 100;
    settings.audio_ff_volume = cfg ? cfg->audio_ff_volume : 100;
    settings.audio_muted = cfg ? (cfg->audio_muted != 0) : false;
    settings.audio_mute_fast_forward = cfg ? (cfg->audio_mute_fast_forward != 0) : false;
    settings.audio_swap_channels = cfg ? (cfg->audio_swap_channels != 0) : false;
    settings.audio_skip_reverb = cfg ? (cfg->audio_skip_reverb != 0) : false;
    settings.audio_buffer_ms = cfg ? cfg->audio_buffer_ms : 13;
    settings.audio_driver = cfg ? cfg->audio_driver : 1;
    settings.audio_background_playback = cfg ? (cfg->audio_background_playback != 0) : false;
    settings.ui_state.show_settings_overlay = settings.debug_panel;
    settings.ui_state.show_performance_overlay = settings.debug_panel;

    if (!cli.bios && !settings.bios_override.empty()) {
        const std::filesystem::path bios_path(settings.bios_override);
        if (!std::filesystem::exists(bios_path) && (settings.bios_override == "bios.bin")) {
            settings.bios_override.clear();
        }
    }

    LoadExtraSettings(settings, cli);

#if defined(USE_HARDWARE)
    if (env_gpu_backend) {
        settings.gpu_backend = *env_gpu_backend;
    }
#endif

    settings.scale = std::max(1, settings.scale);
    settings.log_level = std::clamp(settings.log_level, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
    settings.logging_enabled = !settings.quiet;
    // LoadExtraSettings re-reads the same file straight from TOML, so clamp AFTER it rather
    // than only in config.c: a hand-edited fast_forward_speed = 0.01 would otherwise reach the
    // frame pacer. 0 is preserved as the "uncapped" sentinel.
    if (settings.fast_forward_speed > 0.0) {
        settings.fast_forward_speed = std::clamp(settings.fast_forward_speed, 1.0, 16.0);
    } else {
        settings.fast_forward_speed = 0.0;
    }
    settings.speed_percent = std::clamp(settings.speed_percent, 10, 1000);
    settings.fps_limit = std::clamp(settings.fps_limit, 0, 1000);
    // -1 is the adaptive sentinel, so the floor is -1 and not 0. Clamped here as well as in
    // config.c because LoadExtraSettings re-reads the raw TOML over the top of cfg.
    settings.frame_skip = std::clamp(settings.frame_skip, -1, kMaxFrameSkip);
    settings.audio_volume = std::clamp(settings.audio_volume, 0, 200);
    settings.audio_ff_volume = std::clamp(settings.audio_ff_volume, 0, 200);
    settings.audio_buffer_ms = std::clamp(settings.audio_buffer_ms, 2, 100);
    settings.audio_driver = std::clamp(settings.audio_driver, 0, 2);
    settings.ui_state.show_settings_overlay = settings.debug_panel;
    settings.ui_state.show_performance_overlay = settings.debug_panel;

    if (settings.ui_state.covers_path.empty()) {
        settings.ui_state.covers_path = DefaultBrowseDirectory() / "covers";
    }

    return settings;
}

/*
    Re-reads the SESSION-START keys from the file on disk.

    SaveSettings() regenerates the whole settings.toml from the core's in-memory snapshot, which
    was loaded when the session started. The launcher writes the same file while that session is
    running — that is how the pause menu's Upscale row works — so anything the user changed
    mid-session was silently written back to its old value the next time the core saved.

    That is exactly the reported "upscale buttons do nothing": the UI wrote internal_scale = 4,
    the core still held 1, and the core's own save put 1 back before the restart could read it.

    The core is authoritative for what it changes at RUNTIME (vsync). It is NOT authoritative for
    the keys it merely read at boot, so those are taken from disk at write time.
*/
void RefreshSessionStartSettingsFromDisk(FrontendSettings& settings) {
    const std::filesystem::path target = settings.settings_path.empty()
                                             ? std::filesystem::path(DefaultSettingsPath())
                                             : std::filesystem::path(settings.settings_path);

    std::ifstream in(target, std::ios::binary);

    if (!in) {
        return; /* No file yet: the in-memory values ARE the truth. */
    }

    std::string line;

    while (std::getline(in, line)) {
        const auto eq = line.find('=');

        if (eq == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        const auto trim = [](std::string& text) {
            const auto first = text.find_first_not_of(" \t\r\n\"");
            const auto last = text.find_last_not_of(" \t\r\n\"");
            text = (first == std::string::npos) ? std::string() : text.substr(first, last - first + 1);
        };

        trim(key);
        trim(value);

        if (key == "internal_scale") {
            const int parsed = std::atoi(value.c_str());

            if (parsed >= 1) {
                settings.internal_scale = parsed;
            }
        } else if (key == "renderer") {
            if (value == "hardware-gl" || value == "opengl" || value == "gl") {
                settings.rasterizer_mode = 3;
                settings.hw_rasterizer = true;
            } else if (value == "hardware-cpu" || value == "cpu") {
                settings.rasterizer_mode = 2;
                settings.hw_rasterizer = true;
            } else if (value == "hardware" || value == "hw" || value == "internal" ||
                       value == "upscale") {
                settings.rasterizer_mode = 1;
                settings.hw_rasterizer = true;
            } else if (value == "software") {
                settings.rasterizer_mode = 0;
                settings.hw_rasterizer = false;
            }
        }
    }
}

bool SaveSettings(const FrontendSettings& settings_in) {
    /* Copy so the caller's snapshot is untouched; only what goes to DISK is refreshed. */
    FrontendSettings settings = settings_in;
    RefreshSessionStartSettingsFromDisk(settings);
    /* Same reasoning, section-aware: the launcher writes [cheats] while this session runs. */
    RefreshCheatSettingsFromDisk(settings);

    const std::filesystem::path target = settings.settings_path.empty() ? std::filesystem::path(DefaultSettingsPath()) : std::filesystem::path(settings.settings_path);
    std::filesystem::path temp = target;
    temp += ".tmp";

    try {
        if (target.has_parent_path()) {
            std::filesystem::create_directories(target.parent_path());
        }
    } catch (...) {
        return false;
    }

    std::ofstream out(temp, std::ios::binary | std::ios::trunc);

    if (!out.is_open()) {
        return false;
    }

    auto write_path_array = [&](const std::vector<std::filesystem::path>& paths) {
        out << "[";
        for (size_t index = 0; index < paths.size(); index++) {
            if (index) {
                out << ", ";
            }
            out << "\"" << EscapeTomlString(paths[index].string()) << "\"";
        }
        out << "]";
    };

    /* Plain strings, not paths: `[cheats] enabled_codes` holds cheat NAMES. */
    auto write_string_array = [&](const std::vector<std::string>& values) {
        out << "[";
        for (size_t index = 0; index < values.size(); index++) {
            if (index) {
                out << ", ";
            }
            out << "\"" << EscapeTomlString(values[index]) << "\"";
        }
        out << "]";
    };

    out
        << "# Settings file generated by ARMSX\n\n"
        << "psxe_version = \"" << STR(REP_VERSION) << "\"\n\n"
        << "[bios]\n"
        << "    search_path     = \"" << EscapeTomlString(settings.bios_search) << "\"\n"
        << "    preferred_model = \"" << EscapeTomlString(settings.model) << "\"\n"
        << "    override_file   = \"" << EscapeTomlString(settings.bios_override) << "\"\n\n"
        << "[console]\n"
        << "    region          = \"" << EscapeTomlString(settings.region) << "\"\n\n"
        << "[cpu]\n"
        << "    execution_mode  = \"" << CpuEngineSettingToken(settings.cpu_engine) << "\"\n\n"
        << "[runtime]\n"
        << "    display_scale = " << settings.scale << "\n"
        << "    logging_enabled = " << (settings.logging_enabled ? "true" : "false") << "\n"
        << "    log_level = " << settings.log_level << "\n"
        << "    quiet = " << (settings.logging_enabled ? "false" : "true") << "\n"
        << "    frame_limit = " << (settings.frame_limit ? "true" : "false") << "\n"
        << "    speed_percent = " << settings.speed_percent << "\n"
        << "    fps_limit = " << settings.fps_limit << "\n"
        << "    fast_forward_speed = " << FastForwardSpeedToString(settings.fast_forward_speed) << "\n"
        << "    frame_skip = " << settings.frame_skip << "\n"
        << "    adpf_clock_hint = " << (settings.adpf_clock_hint ? "true" : "false") << "\n"
        << "    affinity_mode = " << settings.affinity_mode << "\n\n"
        << "[paths]\n"
        << "    expansion_rom = \"" << EscapeTomlString(settings.exp_path) << "\"\n"
        << "    default_psx_exe = \"" << EscapeTomlString(settings.default_exe_path) << "\"\n\n"
        << "[video]\n"
        << "    vsync = " << (settings.vsync_enabled ? "true" : "false") << "\n"
#ifdef USE_HARDWARE
        << "    gpu_backend = \"" << GpuBackendSettingToken(settings.gpu_backend) << "\"\n"
        // Round-trips the explicit hardware-cpu / hardware-gl tokens rather than collapsing
        // them back to "hardware", so an A/B choice made in settings.toml survives the next
        // save from the UI.
        << "    renderer = \"" << RendererSettingToken(settings) << "\"\n"
        << "    internal_scale = " << settings.internal_scale << "\n"
#endif
        << "    accurate_mask_bit = " << (settings.accurate_mask_bit ? "true" : "false") << "\n"
        << "    accurate_dither = " << (settings.accurate_dither ? "true" : "false") << "\n"
        << "    accurate_prim_size = " << (settings.accurate_prim_size ? "true" : "false") << "\n"
        << "    accurate_tex_modulate = " << (settings.accurate_tex_modulate ? "true" : "false") << "\n"
        << "    pgxp = " << (settings.pgxp ? "true" : "false") << "\n"
        // Every one of these MUST be written. [video] is rewritten wholesale from this
        // explicit list, so a key with no line here is dropped from settings.toml the first
        // time anything saves — the setting appears to work and then silently reverts.
        << "    widescreen_hack = " << (settings.widescreen_hack ? "true" : "false") << "\n"
        << "    texture_filter = \"" << TextureFilterToString(settings.texture_filter) << "\"\n"
        << "    texture_dump = " << (settings.texture_dump ? "true" : "false") << "\n"
        << "    texture_replacements = " << (settings.texture_replacements ? "true" : "false") << "\n"
        << "    texture_dir = \"" << EscapeTomlString(settings.texture_dir) << "\"\n"
        << "    downsample = " << settings.downsample << "\n"
        << "    deinterlace = \"" << DeinterlaceToString(settings.deinterlace) << "\"\n"
        << "    overscan_crop = \"" << OverscanToString(settings.overscan_crop) << "\"\n"
        << "    display_rotation = " << (settings.display_rotation & 3) * 90 << "\n"
        << "    line_detect = \"" << LineDetectToString(settings.line_detect) << "\"\n"
        << "    texture_scale_mode = " << (settings.texture_scale_mode ? "true" : "false") << "\n"
        << "    debug_panel = " << (settings.debug_panel ? "true" : "false") << "\n"
        << "    stretch_mode = " << (settings.stretch_mode ? "true" : "false") << "\n"
        << "    display_aspect = \"" << AspectToString(settings.display_aspect) << "\"\n"
        << "    display_aspect_custom = " << settings.display_aspect_custom << "\n"
        << "    integer_scaling = " << (settings.integer_scaling ? "true" : "false") << "\n"
        << "    wide_upscale = \"" << UpscaleToString(settings.upscale_height) << "\"\n\n"
        << "[input]\n"
        << "    analog_mode_default = " << (settings.analog_mode_default ? "true" : "false") << "\n"
        << "    multitap = " << (settings.multitap ? "true" : "false") << "\n\n"
        << "[emulation]\n"
        << "    rewind = " << (settings.rewind ? "true" : "false") << "\n"
        << "    rewind_seconds = " << settings.rewind_seconds << "\n"
        << "    rewind_frequency = " << settings.rewind_frequency << "\n"
        << "    runahead = " << settings.runahead << "\n\n"
        // [cheats]. MUST be written for the same reason as every other section: this rewrites
        // settings.toml wholesale from an explicit list, so a key with no line here is erased
        // the first time the core saves and the user's cheat selection silently disappears.
        // The values come from RefreshCheatSettingsFromDisk() above, not from the boot
        // snapshot, so a selection made while the game is running survives this save.
        << "[cheats]\n"
        << "    enabled = " << (settings.cheats_enabled ? "true" : "false") << "\n"
        << "    file = \"" << EscapeTomlString(settings.cheats_file) << "\"\n"
        << "    enabled_codes = ";
    write_string_array(settings.cheats_enabled_codes);
    out
        << "\n\n"
        << "[audio]\n"
        << "    volume = " << settings.audio_volume << "\n"
        << "    fast_forward_volume = " << settings.audio_ff_volume << "\n"
        << "    muted = " << (settings.audio_muted ? "true" : "false") << "\n"
        << "    mute_fast_forward = " << (settings.audio_mute_fast_forward ? "true" : "false") << "\n"
        << "    swap_channels = " << (settings.audio_swap_channels ? "true" : "false") << "\n"
        << "    skip_reverb = " << (settings.audio_skip_reverb ? "true" : "false") << "\n"
        << "    buffer_ms = " << settings.audio_buffer_ms << "\n"
        << "    driver = \"" << AudioDriverToString(settings.audio_driver) << "\"\n"
        << "    background_playback = " << (settings.audio_background_playback ? "true" : "false") << "\n\n"
        << "[library]\n"
        << "    folders = ";
    write_path_array(settings.ui_state.game_list_paths);
    out << "\n    recursive_folders = ";
    write_path_array(settings.ui_state.game_list_recursive_paths);
    out
        << "\n\n[fsui]\n"
        << "    default_game_view = " << settings.ui_state.default_game_view << "\n"
        << "    game_sort = " << settings.ui_state.game_sort << "\n"
        << "    game_sort_reverse = " << (settings.ui_state.game_sort_reverse ? "true" : "false") << "\n";

    out.flush();
    if (!out.good()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }

    out.close();
    if (!out.good()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }

#if defined(_WIN32)
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
#endif

    return true;
}

void MixPsxAudio(psx_t* psx, uint8_t* buffer, int size) {
    if (!psx || !buffer || size <= 0) {
        return;
    }

    psx_cdrom_t* cdrom = psx->cdrom;
    psx_spu_t* spu = psx->spu;

    std::memset(buffer, 0, static_cast<size_t>(size));

    psx_cdrom_get_audio_samples(cdrom, buffer, size);
    psx_spu_update_cdda_buffer(spu, cdrom->cdda_buf);

    for (int sample = 0; sample < (size >> 2); sample++) {
        // Drain first, generate only as a fallback. psx_spu_tick() has already produced this
        // frame's samples spread across the frame, from psx_update(), which is the ONLY way an
        // SPU interrupt raised mid-frame can reach a running CPU. Generating here instead
        // stops the machine, and every interrupt raised in the lump collapses into one at the
        // frame boundary — the starvation described above psx_spu_tick() in psx/dev/spu.c.
        //
        // The fallback is not dead code: a frame that reaches vblank in fewer cycles than its
        // sample budget covers leaves a genuine shortfall, and so does the frame a state load
        // lands in. Filling it here keeps the per-frame sample count exactly what the queueing
        // and resampling below have always been handed.
        uint32_t value = 0;

        if (!psx_spu_pop_sample(spu, &value)) {
            value = psx_spu_get_sample(spu);

            if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.spu_gen_inline++;
            }
        }

        const int16_t left = static_cast<int16_t>(value & 0xffff);
        const int16_t right = static_cast<int16_t>(value >> 16);

        // This is the ONE place CD/XA and the SPU meet. The add used to be a bare 16-bit +=
        // with no saturation, so a loud disc track under a loud sound effect wrapped from full
        // positive to full negative — heard as a hard buzz, not as distortion.
        //
        // It measured wrap=0 in all three MGS captures, so on its own it was not worth
        // touching. It is saturated now because the SPU main-volume fix (spu_main_gain, 6 dB)
        // doubles what the SPU contributes here: the dry sum already reached 30016 of 32767
        // during an alert, so post-fix the SPU alone lands near full scale and ANY CD audio
        // added on top would wrap. Saturating cannot alter a sample that was not going to
        // wrap, so this is a no-op everywhere the old behaviour was already correct.
        auto* out_l = reinterpret_cast<int16_t*>(&buffer[(sample << 2) + 0]);
        auto* out_r = reinterpret_cast<int16_t*>(&buffer[(sample << 2) + 2]);

        const int sum_l = *out_l + left;
        const int sum_r = *out_r + right;

        if (g_psx_audio_diag_enabled) {
            if (sum_l > INT16_MAX || sum_l < INT16_MIN || sum_r > INT16_MAX || sum_r < INT16_MIN) {
                g_psx_audio_diag.mix_wrap++;
                g_psx_audio_diag.mix_sat++;
            }

            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.mix_peak_l, sum_l);
            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.mix_peak_r, sum_r);
        }

        *out_l = static_cast<int16_t>(std::clamp(sum_l, -32768, 32767));
        *out_r = static_cast<int16_t>(std::clamp(sum_r, -32768, 32767));
    }
}

class ArmsxSession {
  public:
    ~ArmsxSession() {
        destroy();
    }

    bool create(armsx_renderer_t* renderer, const FrontendSettings& settings, const LaunchRequest& request, std::string& error) {
        destroy();

        if (!renderer) {
            error = "Display is not ready.";
            return false;
        }

        render_ = renderer;
        vblank_counter_ = 0;

        psxe_config_t cfg{};
        cfg.bios = settings.bios_override.empty() ? nullptr : settings.bios_override.c_str();
        cfg.bios_search = settings.bios_search.c_str();
        cfg.model = settings.model.c_str();

        char* bios_path = psxe_cfg_get_bios_path(&cfg);
        std::unique_ptr<char, decltype(&free)> bios_guard(bios_path, &free);

        if (!bios_path || !bios_path[0]) {
            error = "No BIOS could be resolved. Set a BIOS override file or a BIOS folder that contains the selected model.";
            return false;
        }

        psx_ = psx_create();
        if (!psx_) {
            error = "Failed to allocate a PSX session.";
            return false;
        }

        const char* expansion = settings.exp_path.empty() ? nullptr : settings.exp_path.c_str();
        const int init_result = psx_init(psx_, bios_path, expansion);

        if (init_result != 0) {
            error = "Failed to initialize the emulator core. Check the BIOS and expansion ROM paths.";
            destroy();
            return false;
        }

        psx_cpu_set_execution_mode(psx_get_cpu(psx_), settings.cpu_engine);
        psxe_diag_logf("cpu", "execution engine=%s", CpuEngineSettingToken(settings.cpu_engine));

        psx_gpu_t* gpu = psx_get_gpu(psx_);

        {
            uint32_t accuracy = 0;

            if (settings.accurate_mask_bit) {
                accuracy |= PSX_GPU_ACCURACY_MASK_BIT;
            }

            if (settings.accurate_dither) {
                accuracy |= PSX_GPU_ACCURACY_DITHER_GATE;
            }

            if (settings.accurate_prim_size) {
                accuracy |= PSX_GPU_ACCURACY_PRIM_SIZE;
            }

            if (settings.accurate_tex_modulate) {
                accuracy |= PSX_GPU_ACCURACY_TEX_MODULATE;
            }

            psx_gpu_set_accuracy_flags(gpu, accuracy);
            psx_gpu_set_texture_filter(gpu, settings.texture_filter);

            /* Texture dumping / replacement (psx/texrep.h). Applied at boot from
               settings.toml; Android re-pushes a per-game directory through
               setPs1TextureOptions once it knows the disc serial. Both off is the shipping
               default and allocates nothing. */
            {
                const std::string dir = ResolveTextureDir(settings.texture_dir);

                psx_texrep_configure(gpu,
                                     settings.texture_dump ? 1 : 0,
                                     settings.texture_replacements ? 1 : 0,
                                     dir.c_str());

                if (settings.texture_dump || settings.texture_replacements) {
                    log_info("Texture %s%s%s from %s",
                             settings.texture_dump ? "dumping" : "",
                             (settings.texture_dump && settings.texture_replacements) ? " + " : "",
                             settings.texture_replacements ? "replacement" : "",
                             dir.c_str());
                }
            }

            if (accuracy) {
                log_info("GPU accuracy fixes: mask_bit=%s dither_gate=%s prim_size=%s tex_modulate=%s",
                         settings.accurate_mask_bit ? "on" : "off",
                         settings.accurate_dither ? "on" : "off",
                         settings.accurate_prim_size ? "on" : "off",
                         settings.accurate_tex_modulate ? "on" : "off");
            }
        }

        // Marker-armed one-shot GP0 primitive dump (psx/dev/gpu.c). The directory is PUSHED
        // in rather than pulled because psx/ must not link frontend/diagnostics.c — both GPU
        // test harnesses compile gpu.c without it. Completely inert until someone creates
        // files/logs/gpu_prim_dump, exactly like gpu_hw_gl.c's gl_debug_marker() switches.
        {
            const char* diag_log_path = psxe_diag_log_path();
            const char* diag_slash = diag_log_path ? std::strrchr(diag_log_path, '/') : nullptr;

            if (diag_slash) {
                const std::string diag_dir(
                    diag_log_path, static_cast<size_t>(diag_slash - diag_log_path) + 1u);
                psx_gpu_debug_set_log_dir(gpu, diag_dir.c_str());
            }
        }

        // PGXP. Applied whether on or off so a settings change between runs
        // always lands; the setter itself is idempotent and logs transitions.
        psx_pgxp_set_enabled(settings.pgxp ? 1 : 0);

        // Widescreen hack (psx/cpu.c GTE). Same reasoning as PGXP: core-global, applied
        // unconditionally so a change between runs always lands, and idempotent.
        psx_cpu_set_widescreen_hack(settings.widescreen_hack ? 1 : 0);

        // GLES rasterizer video options. Pushed BEFORE the backend is created below, and
        // deliberately not a create() argument: the backend is recreated behind our back on
        // any GL failure and these have to survive that. Harmless when the GLES backend is
        // not built or not selected.
#ifdef USE_HARDWARE
        armsx_hw_gl_set_video_options(settings.texture_filter, settings.downsample,
                                      settings.line_detect);
#endif

        // Host CPU scheduling levers from [runtime]. DEFAULTS, not assignments: the Android
        // front-end pushes both through JNI before the core ever opens settings.toml, and a
        // file that predates these keys carries the off value for both. Applying it here as if
        // it were a user choice would silently un-toggle whatever the UI just set — the exact
        // shape of the "per-game INI shadows the higher layer" bug. perf_hint.c drops these on
        // the floor once a host has spoken.
        armsx_perf_hint_adpf_set_default(settings.adpf_clock_hint ? 1 : 0);
        armsx_affinity_set_default_mode(settings.affinity_mode);

#ifdef USE_HARDWARE
        // Internal-resolution rasterizer. Entirely opt-in: with [video] renderer =
        // "software" (the default) no backend is installed and psx/dev/gpu.c behaves
        // exactly as it always has. See frontend/gpu_hw_rt.h.
        if (settings.hw_rasterizer) {
            // Two implementations of one ABI. The GLES one rasterizes on the GPU, so its
            // CPU cost does not grow with the internal scale at all; the CPU one is
            // graphics-API-independent and is the fallback when there is no GL context (a
            // Vulkan or SDL presentation backend). See frontend/gpu_hw_gl.h.
            const int mode = settings.rasterizer_mode;

            rasterizer_mode_ = mode;
            internal_scale_ = settings.internal_scale;

            if (mode != 2) {
                hw_rt_backend_ = armsx_hw_gl_create(gpu, settings.internal_scale);
                hw_rt_is_gl_ = (hw_rt_backend_ != nullptr);

                if (!hw_rt_backend_) {
                    // Never silent: armsx_hw_gl_status() says which of the availability
                    // gates rejected it.
                    log_info("GLES rasterizer not used: %s", armsx_hw_gl_status());
                }
            }

            if (!hw_rt_backend_ && (mode != 3)) {
                hw_rt_backend_ = armsx_hw_rt_create(gpu, settings.internal_scale);
                hw_rt_is_gl_ = false;
            }

            if (hw_rt_backend_) {
                psx_gpu_set_backend(gpu, hw_rt_backend_);
                log_info("Rasterizer: %s at %dx internal resolution",
                         hw_rt_is_gl_ ? "GLES (GPU)" : "internal-resolution (CPU)",
                         psx_gpu_resolution_scale(gpu));
            } else {
                log_error("Hardware rasterizer unavailable at %dx; staying on the software path",
                          settings.internal_scale);
            }
        }
#endif

        psx_gpu_set_event_callback(gpu, GPU_EVENT_DMODE, nullptr);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK, SessionVblankEvent);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK, psxe_gpu_hblank_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK_END, psxe_gpu_vblank_end_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK_END, psxe_gpu_hblank_end_event_cb);
        psx_gpu_set_udata(gpu, 0, this);
        psx_gpu_set_udata(gpu, 1, psx_->timer);
        psx_gpu_set_udata(gpu, 2, nullptr);

#ifdef USE_HARDWARE
        // "Hardware backend active" now means "presentation is GPU-accelerated", whichever
        // backend provided it. The PlayStation rasterizer itself is unaffected either way.
        hardware_backend_active_ = armsx_renderer_is_accelerated(render_);
        if (settings.gpu_backend != GpuBackend::Software && !hardware_backend_active_) {
            psxe_diag_logf("renderer", "GPU presentation requested but unavailable; using software presentation.");
        } else if (hardware_backend_active_) {
            psxe_diag_logf(
                "renderer",
                "Accelerated presentation enabled backend=%s driver=%s; PlayStation VRAM remains software-authoritative.",
                armsx_render_backend_name(armsx_renderer_backend(render_)),
                armsx_renderer_driver_name(render_)
            );
        }
#endif

        psx_cdrom_set_region(psx_get_cdrom(psx_), RegionFromSettings(settings));

        // [input] analog_mode_default. Default OFF = digital, matching real hardware. It was
        // briefly defaulted ON so the sticks were discoverable (the ANALOG button has no
        // standard Android keycode), and that broke input outright in pre-DualShock titles:
        // Crash Bandicoot (1996) mis-handles a pad reporting model 0x73 at boot, leaving BOTH
        // the touch overlay and a physical pad dead. Pad code 200 still toggles live either way.
        //
        // [input] multitap decides WHICH device goes in the port — one pad, or a tap carrying
        // four of them. Both paths go through attachPadDevice() so the live toggle below
        // cannot drift from what a launch builds.
        analog_mode_default_ = settings.analog_mode_default;

        if (!attachPadDevice(settings.multitap, settings.analog_mode_default)) {
            error = "Failed to create the controller input bridge.";
            destroy();
            return false;
        }

        ARMSX_BOOTLOG("core: pad boot mode=%s (analog_mode_default=%s) multitap=%s",
            settings.analog_mode_default ? "analog" : "digital",
            settings.analog_mode_default ? "true" : "false",
            settings.multitap ? "true" : "false");

        // Rewind + runahead (psx/rewind.h). Applied here rather than at process start so a
        // relaunch with changed settings picks them up, and so nothing is allocated until a
        // machine actually exists to snapshot.
        psx_rewind_configure(settings.rewind ? 1 : 0, settings.rewind_seconds,
                             settings.rewind_frequency);
        psx_runahead_configure(settings.runahead);
        runahead_restore_pending_ = false;

        // [cheats]. Armed here, at the same point and for the same reason as rewind above: a
        // relaunch with a changed selection has to pick it up, and there is no machine to
        // patch until one exists. Costs a file read of a few KB when the game has cheats and
        // literally nothing when it does not.
        ApplyCheatSettings(settings);

        const std::string slot1 = std::string(psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "") + "slot1.mcd";
        const std::string slot2 = std::string(psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "") + "slot2.mcd";
        psx_pad_attach_mcd(psx_->pad, 0, slot1.c_str());
        psx_pad_attach_mcd(psx_->pad, 1, slot2.c_str());

        switch (request.kind) {
            case LaunchKind::Disc:
                if (psx_cdrom_open(psx_get_cdrom(psx_), request.path.string().c_str()) == 0) {
                    error = "Failed to open the selected disc image.";
                    destroy();
                    return false;
                }
                disc_path_ = request.path;
                exe_path_.clear();
                title_ = request.label.empty() ? StemToTitle(request.path) : request.label;
                break;

            case LaunchKind::Exe:
                while (psx_->cpu->pc != 0x80030000) {
                    psx_update(psx_);
                }
                psx_load_exe(psx_, request.path.string().c_str());
                exe_path_ = request.path;
                disc_path_.clear();
                title_ = request.label.empty() ? StemToTitle(request.path) : request.label;
                break;

            case LaunchKind::Bios:
                disc_path_.clear();
                exe_path_.clear();
                title_ = "PlayStation BIOS";
                break;

            case LaunchKind::None:
            default:
                error = "Invalid launch request.";
                destroy();
                return false;
        }

        launch_kind_ = request.kind;
#if defined(HW_DEBUG)
#ifdef USE_HARDWARE
        if (hardware_backend_active_ && psx_ && psx_->gpu) {
            int output_width = 0;
            int output_height = 0;
            armsx_renderer_output_size(render_, &output_width, &output_height);
            psxe_diag_logf(
                "hw",
                "session-create kind=%s title=%s renderer=%p output=%dx%d texture=%dx%d timing=%s target_fps=%.3f display_mode=0x%08x gpustat=0x%08x",
                LaunchKindTitle(request.kind),
                title_.c_str(),
                (void*)render_,
                output_width,
                output_height,
                texture_width_,
                texture_height_,
                timingModeTitle(),
                targetFrameRate(),
                psx_->gpu->display_mode,
                psx_->gpu->gpustat
            );
        }
#endif
#endif
        psxe_diag_breadcrumbf(
            "Session created kind=%s title=%s path=%s bios=%s model=%s region=%s",
            LaunchKindTitle(request.kind),
            title_.c_str(),
            request.path.empty() ? "(none)" : request.path.string().c_str(),
            bios_path,
            settings.model.c_str(),
            settings.region.c_str()
        );

        applyAudioSettings(settings);
        setSpeedLimits(settings.frame_limit, settings.speed_percent, settings.fps_limit,
                       settings.fast_forward_speed, settings.frame_skip);

        // A session being (re)initialised over a live device has to give the old one back first.
        // openAudioDevice() below no-ops while one is open, and the previous code just overwrote
        // audio_dev_ — which leaked the device and left buffer_ms stuck on the old value.
        closeAudioDevice();

        // Remembered so the device can be reopened byte-for-byte identically when the app comes
        // back from the background — see setAudioSuspended(). allowed_changes = 0, so SDL always
        // hands the callback exactly this format and `obtained` never differs.
        audio_desired_ = SDL_AudioSpec{};
        audio_desired_.freq = kAudioMixRate;
        audio_desired_.format = AUDIO_S16SYS;
        audio_desired_.channels = 2;
        // [audio] buffer_ms. The historical value was CD_SECTOR_SIZE >> 2 = 588 frames
        // (13.3 ms at 44.1 kHz), which the 13 ms default reproduces to within a millisecond.
        audio_desired_.samples = static_cast<Uint16>(std::clamp(
            (kAudioMixRate * std::clamp(settings.audio_buffer_ms, 2, 100)) / 1000, 64, 8192));
        audio_desired_.callback = AudioUpdate;
        audio_desired_.userdata = this;

        paused_ = false;
        // A fresh session owns a fresh device, so the background latch has to start clear or the
        // first return from the background would be swallowed as a no-op.
        audio_suspended_ = false;
        fast_forward_enabled_ = false;
        debug_view_ = false;
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        openAudioDevice();
        updateTexture(settings);

        return true;
    }

    // Open (or reopen) the SDL device from the remembered spec and reset everything the queue
    // and the rate converter carry across it. No-op if a device is already open.
    void openAudioDevice() {
        if (audio_dev_) {
            return;
        }

        audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &audio_desired_, nullptr, 0);
        if (!audio_dev_) {
            // Only worth shouting about once per failure run; ensureAudioDevice() retries.
            if (!audio_reopen_failed_) {
                audio_reopen_failed_ = true;
                psxe_diag_logf("audio", "open failed: %s", SDL_GetError());
            }
            return;
        }

        audio_reopen_failed_ = false;
        audio_device_ever_opened_ = true;
        audio_sample_accumulator_ = 0.0;
        audio_queue_.clear();
        audio_queue_read_offset_ = 0;
        audio_consumed_bytes_ = 0;
        resetAudioRateConverter();
        updateAudioPlaybackState();
    }

    /**
     * Retry a reopen that did not take.
     *
     * The whole point of closing the device on background is that it can be given back — and the
     * one way this fix could be WORSE than the bug is a return from the background whose
     * SDL_OpenAudioDevice() fails (the output can be momentarily unavailable while another app is
     * still tearing its own stream down), leaving the game silent for the rest of the session
     * with nothing to retry it. Called once per frame from ArmsxApp::runFrame().
     *
     * Only ever retries a device that HAS worked in this session: a boot where audio never came
     * up at all is a different problem (no driver, dummy driver) and must not spin here.
     */
    void ensureAudioDevice() {
        if (audio_dev_ || audio_suspended_ || !audio_device_ever_opened_) {
            return;
        }

        const Uint64 now = SDL_GetTicks64();
        if (audio_reopen_next_attempt_ != 0 && now < audio_reopen_next_attempt_) {
            return;
        }
        audio_reopen_next_attempt_ = now + 1000;
        openAudioDevice();
    }

    // Tear the SDL device down completely.
    //
    // ★ Closing is the ONLY way to actually stop the stream on Android. SDL_PauseAudioDevice()
    // does not: SDL's audio thread keeps running and keeps handing the backend buffers, it just
    // fills them with silence, and the OpenSL ES player stays PLAYING. AudioFlinger therefore
    // keeps our track Active, the output thread never reaches standby, and audioserver holds a
    // PARTIAL_WAKE_LOCK ('AudioMix', charged to this app's uid) for as long as we are in the
    // background — measured on a sleeping Retroid Pocket 6, where it was the only wake lock left
    // on the device. SDL's own openslES_PauseDevices() would park the player properly, but it is
    // NOT exported from libSDL2.so (935 exported symbols, none of them openslES_*), so linking
    // against it produces a library that will not load.
    //
    // SDL_CloseAudioDevice() reaches openslES_DestroyPCMPlayer(), which SetPlayState(STOPPED)s
    // and Destroy()s the player object. The audio thread is joined first, and it cannot wedge:
    // the player is still playing while SDL waits, so the buffer-queue callback keeps posting the
    // semaphore the thread is parked on until it notices the shutdown flag.
    void closeAudioDevice() {
        if (!audio_dev_) {
            return;
        }

        SDL_LockAudioDevice(audio_dev_);
        resetAudioQueueLocked();
        SDL_UnlockAudioDevice(audio_dev_);
        SDL_PauseAudioDevice(audio_dev_, 1);
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
    }

    // Push the [audio] table at the live session. Called at create() and again whenever the
    // host re-applies settings, so volume/mute/swap/skip-reverb take effect without a relaunch.
    // buffer_ms and driver are NOT live: both are properties of an already-open SDL device.
    void applyAudioSettings(const FrontendSettings& settings) {
        audio_volume_ = std::clamp(settings.audio_volume, 0, 200);
        audio_ff_volume_ = std::clamp(settings.audio_ff_volume, 0, 200);
        audio_muted_ = settings.audio_muted;
        audio_mute_fast_forward_ = settings.audio_mute_fast_forward;
        audio_swap_channels_ = settings.audio_swap_channels;

        if (psx_ && psx_->spu) {
            psx_spu_set_reverb_disabled(psx_->spu, settings.audio_skip_reverb ? 1 : 0);
        }

        if (audio_dev_) {
            updateAudioPlaybackState();
        }
    }

    void destroy() {
        // A capture still open here would leak its FILE* and lose its tail; closing it also
        // clears g_psx_audio_diag_enabled so a fresh session starts unarmed.
        audioDiagEnd();

        if (psx_) {
            psxe_diag_breadcrumbf(
                "Session destroyed kind=%s title=%s path=%s",
                LaunchKindTitle(launch_kind_),
                title_.empty() ? "(none)" : title_.c_str(),
                disc_path_.empty() ? (exe_path_.empty() ? "(none)" : exe_path_.string().c_str()) : disc_path_.string().c_str()
            );
        }

        // Already closed when the app is being torn down straight out of the background; the
        // latches are cleared so a session that gets rebuilt reopens normally and does not
        // inherit the last session's reopen-retry state.
        closeAudioDevice();
        audio_suspended_ = false;
        audio_device_ever_opened_ = false;
        audio_reopen_failed_ = false;
        audio_reopen_next_attempt_ = 0;

        resetAudioRateConverter();

        // [cheats]. Drops the catalogue and the compiled program, so the next game starts with
        // nothing armed instead of inheriting this disc's selection — and so the strings this
        // module owns are freed rather than living for the process.
        psx_cheats_shutdown();

        texture_snapshot_.clear();
        frame_uploaded_ = false;

#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_destroy(hw_renderer_);
            hw_renderer_ = nullptr;
        }
#endif

        const bool input_attached = psx_ && psx_->pad && (psx_->pad->joy_slot[0] == input_);

#ifdef USE_HARDWARE
        // Detach before the GPU goes away: psx_gpu_t does not own the backend.
        if (hw_rt_backend_) {
            releaseAdoptedTexture();

            if (psx_) {
                psx_gpu_set_backend(psx_get_gpu(psx_), nullptr);
            }

            if (hw_rt_is_gl_) {
                armsx_hw_gl_destroy(hw_rt_backend_);
            } else {
                armsx_hw_rt_destroy(hw_rt_backend_);
            }

            hw_rt_backend_ = nullptr;
            hw_rt_is_gl_ = false;
        }
#endif

        if (psx_) {
            psx_destroy(psx_);
            psx_ = nullptr;
        }

        if (input_ && !input_attached) {
            psx_input_destroy(input_);
        }

        input_ = nullptr;
        render_ = nullptr;
        disc_path_.clear();
        exe_path_.clear();
        title_.clear();
        launch_kind_ = LaunchKind::None;
        paused_ = false;
        fast_forward_enabled_ = false;
        debug_view_ = false;
        vblank_counter_ = 0;
#ifdef USE_HARDWARE
        hardware_backend_active_ = false;
#endif
    }

    bool valid() const {
        return psx_ != nullptr;
    }

    psx_t* psx() const {
        return psx_;
    }

    psx_pad_t* pad() const {
        return psx_ ? psx_->pad : nullptr;
    }

    void rebindRenderer(armsx_renderer_t* renderer, const FrontendSettings& settings) {
        render_ = renderer;
#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_set_renderer(hw_renderer_, armsx_renderer_sdl(render_));
            if (texture_width_ > 0 && texture_height_ > 0) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
        }
        hardware_backend_active_ = armsx_renderer_is_accelerated(render_);
#endif
        texture_snapshot_.clear();
        frame_uploaded_ = false;
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        updateTexture(settings);
    }

    void setPaused(bool paused) {
        paused_ = paused;
        if (audio_dev_) {
            if (paused) {
                SDL_LockAudioDevice(audio_dev_);
                resetAudioQueueLocked();
                SDL_UnlockAudioDevice(audio_dev_);
            }
            updateAudioPlaybackState();
        }
    }

    /**
     * Release the audio device entirely because the app went off-screen, and take it back when
     * it returns.
     *
     * Kept SEPARATE from setPaused(): the in-game pause menu wants the emulation frozen with the
     * device still open (that is the cheap, instant path, and the overlay is still on screen).
     * This is the app-is-not-on-screen case, where holding the device costs a wake lock and
     * leaks the tail of the mix out of a sleeping handheld. See closeAudioDevice() for why
     * pausing the device is not enough and closing it is what actually works.
     */
    void setAudioSuspended(bool suspended) {
        if (audio_suspended_ == suspended) {
            return;
        }
        audio_suspended_ = suspended;

        if (suspended) {
            closeAudioDevice();
        } else {
            openAudioDevice();
        }
    }

    bool audioSuspended() const {
        return audio_suspended_;
    }

    bool paused() const {
        return paused_;
    }

    void setFastForwardEnabled(bool enabled) {
        if (fast_forward_enabled_ == enabled) {
            return;
        }

        fast_forward_enabled_ = enabled;
        // Drop whatever is queued on BOTH edges. Entering fast-forward, the queue holds
        // real-time audio the converter is about to start compressing; leaving it, the queue
        // holds compressed audio. Either way the tail belongs to the old rate.
        clearQueuedAudio();
        resetAudioRateConverter();

        if (audio_dev_) {
            updateAudioPlaybackState();
        }
    }

    bool fastForwardEnabled() const {
        return fast_forward_enabled_;
    }

    // The whole speed policy in one call: frame_limit gates the limiter, speed_percent scales
    // the game's own rate, fps_limit is an optional absolute ceiling and fast_forward_speed
    // replaces speed_percent while fast-forward is engaged (0 = uncapped). Set from
    // [runtime] at launch and by psxe_host_set_speed_limits() when the UI moves.
    //
    // frame_skip rides along because the host edits it from the same panel and pushes the whole
    // policy at once — but it is NOT a speed control and nothing below reads it. It changes how
    // many emulated frames reach the screen, never how fast they are produced, so it is
    // deliberately kept out of targetFrameRate() and out of the audio converter's decision.
    void setSpeedLimits(bool frame_limit, int speed_percent, int fps_limit, double fast_forward_speed,
                        int frame_skip) {
        const bool converter_was_active = audioRateConversionActive();

        frame_limit_ = frame_limit;
        speed_percent_ = std::clamp(speed_percent, 10, 1000);
        fps_limit_ = std::clamp(fps_limit, 0, 1000);
        fast_forward_speed_ = (fast_forward_speed > 0.0) ? std::clamp(fast_forward_speed, 1.0, 16.0) : 0.0;
        frame_skip_ = std::clamp(frame_skip, -1, kMaxFrameSkip);

        // A speed change re-aims the converter; leaving its phase and rate estimate behind
        // would have it glide across the change instead of tracking it.
        if (converter_was_active != audioRateConversionActive()) {
            clearQueuedAudio();
        }
        resetAudioRateConverter();
    }

    double fastForwardSpeed() const {
        return fast_forward_speed_;
    }

    // 0 = off, 1..kMaxFrameSkip = fixed, -1 = adaptive. Read once per frame by the app loop.
    int frameSkip() const {
        return frame_skip_;
    }

    // The sentinel targetFrameRate() hands back for "no cap". Exposed so the app loop can tell
    // an unreachable target apart from a real one — adaptive frame skip must not treat a
    // deliberately uncapped session as "running behind".
    static constexpr double uncappedFrameRate() {
        return kUncappedFrameRate;
    }

    /*
        Build controller port 1's device and plug it in.

        ONE builder for both the launch path and the live Multitap toggle, so the two cannot
        drift — a tap that behaves differently depending on whether it was switched on before
        or after the game booted is exactly the sort of half-wiring this project keeps hitting.

        psx_pad_attach_joy() destroys whatever was in the slot first, so this is also the
        detach. The SIO destination is cleared afterwards because the swap can land in the
        middle of a transfer; the game simply re-polls on its next frame.
    */
    bool attachPadDevice(bool multitap, bool analog_default) {
        if (!psx_ || !psx_->pad) {
            return false;
        }

        psx_input_t* input = psx_input_create();

        if (!input) {
            return false;
        }

        psx_input_init(input);

        if (multitap) {
            psxi_multitap_t* tap = psxi_multitap_create();

            if (!tap) {
                psx_input_destroy(input);
                return false;
            }

            psxi_multitap_init(tap);
            psxi_multitap_set_analog_mode(tap, analog_default ? 1 : 0);
            psxi_multitap_init_input(tap, input);
        } else {
            psxi_sda_t* sda = psxi_sda_create();

            if (!sda) {
                psx_input_destroy(input);
                return false;
            }

            psxi_sda_init(sda, SDA_MODEL_DIGITAL);
            psxi_sda_set_analog_mode(sda, analog_default ? 1 : 0);
            psxi_sda_init_input(sda, input);
        }

        psx_pad_attach_joy(psx_->pad, 0, input);
        psx_->pad->dest[0] = 0;

        input_ = input;
        multitap_attached_ = multitap;

        return true;
    }

    bool multitapAttached() const {
        return multitap_attached_;
    }

    /* Live Multitap toggle. Emulation thread only — it destroys the device the SIO reads
       through. Everything the old device was holding down goes with it, which is correct:
       the guest is being told a different controller was plugged in. */
    bool setMultitapEnabled(bool enabled) {
        if (!psx_ || !psx_->pad || enabled == multitap_attached_) {
            return false;
        }

        if (!attachPadDevice(enabled, analog_mode_default_)) {
            log_error("multitap: could not rebuild the port %s a tap", enabled ? "with" : "without");
            return false;
        }

        /* A snapshot taken with the other device in the port would be refused on load
           (pad_load_joy checks the device kind), so drop them rather than leave a rewind
           buffer that silently fails on every step. */
        psx_rewind_reset();
        runahead_restore_pending_ = false;

        log_info("multitap: port 1 now has %s", enabled ? "a multitap (4 players)" : "one pad");

        return true;
    }

    /* Undo the previous frame's look-ahead.

       MUST run before this frame's input is applied: a snapshot carries the pad's button
       word, so restoring after an input pass would silently swallow every press that landed
       in between — and the press edge is never re-sent, so the button would simply never
       reach the game. Called from ArmsxApp::runFrame() as its first act. */
    void runaheadRestore() {
        if (!runahead_restore_pending_ || !psx_) {
            return;
        }

        runahead_restore_pending_ = false;

        const int result = psx_runahead_restore(psx_);

        if (result == PSX_STATE_ERR_MISSING) {
            /* The slot was deliberately dropped between the look-ahead and now — a reset, a
               disc swap, a rewind step, or the user loading a state (all of which call
               psx_rewind_reset). Putting a stale future back over any of those would undo
               what the user just asked for, so this is the CORRECT outcome, not a failure. */
            return;
        }

        if (result != PSX_STATE_OK) {
            /* Disarm rather than retry: a slot that will not apply now will not apply later,
               and re-running the look-ahead on a machine that never got put back would
               compound the drift. */
            psx_runahead_configure(0);
            log_error("runahead: could not restore the look-ahead snapshot (%s); runahead off",
                      psx_state_strerror(result));
        }
    }

    /* One emulated frame: step until the GPU raises the next vblank. Shared by the ordinary
       advance and by runahead's re-simulated frames, so the two cannot diverge. Returns the
       instruction count. */
    std::uint32_t stepOneFrame() {
        const std::uint64_t start_vblank = vblank_counter_;
        std::uint32_t steps = 0;

        while (vblank_counter_ == start_vblank) {
            psx_update(psx_);
            steps++;

            if (steps >= kMaxFrameSteps) {
                psxe_diag_logf("timing", "Session frame advance exceeded vblank step budget title=%s",
                               title_.empty() ? "(none)" : title_.c_str());
                break;
            }
        }

        return steps;
    }

    /* Hold-to-rewind / one-shot step back. Returns true when the machine was moved BACKWARDS,
       in which case the caller must not also advance it. Returns true (without moving) when
       rewind is engaged but the ring is empty — holding rewind at the end of the buffer
       freezes rather than resuming forward play, which is what every other emulator does and
       what the button visibly promises. */
    bool rewindFrameIfRequested() {
        int steps = g_host_rewind_step_requests.exchange(0, std::memory_order_acq_rel);
        const bool active = g_host_rewind_active.load(std::memory_order_acquire);

        if (active) {
            steps++;
        }

        if (steps <= 0) {
            return false;
        }

        if (!psx_rewind_enabled()) {
            return false;
        }

        bool moved = false;

        for (int i = 0; i < steps; i++) {
            if (psx_rewind_step_back(psx_) != PSX_STATE_OK) {
                break;
            }

            moved = true;
        }

        if (moved) {
            /* The look-ahead described a future of the timeline we just left. */
            runahead_restore_pending_ = false;
        }

        return true;
    }

    std::uint32_t runFrame() {
        if (!psx_ || paused_) {
            return 0;
        }

#if defined(HW_DEBUG)
        traceHardwareFrameState("frame-start", 0);
#endif

#ifdef USE_HARDWARE
        if (hardware_backend_active_ && hw_renderer_) {
            armsx_hw_renderer_begin_frame(hw_renderer_);
        }
#endif

        // Rewind ([emulation] rewind). Checked before anything is stepped: stepping back IS
        // this frame's work, so the machine must not also be advanced. The hardware frame that
        // was just opened is still closed by the caller's finishHardwareFrame().
        if (rewindFrameIfRequested()) {
            return 0;
        }

        std::uint32_t steps = 0;

        // R3000A accounting. The equivalent lives in psx_run_frame(), which this front-end
        // never calls — it drives psx_update() itself, so the OSD's instruction and cycle
        // rows read a flat zero while every other counter worked. Derived, not instrumented:
        // the step count IS the instruction count, and the CPU already sums executed clocks
        // into total_cycles, so psx_cpu_cycle() (half a million calls a frame) stays free of
        // any counter.
        const std::uint32_t cpu_cycles_before = psx_ ? psx_->cpu->total_cycles : 0;
        const auto account_cpu = [&]() {
            if (!g_psx_perf_enabled || !psx_) {
                return;
            }
            g_psx_perf.cpu_instructions += steps;
            // total_cycles is a wrapping uint32; the unsigned difference is exact over a frame.
            g_psx_perf.cpu_cycles +=
                static_cast<std::uint32_t>(psx_->cpu->total_cycles - cpu_cycles_before);
        };

        // Before the frame, not after: this is what grants the SPU its per-frame sample budget
        // so psx_spu_tick() can spend it across the frame from psx_update(). See
        // beginAudioFrame() and the block comment above psx_spu_tick() in psx/dev/spu.c.
        beginAudioFrame();

        steps = stepOneFrame();

        account_cpu();

        /*
            [cheats]. THE application point: once per REAL emulated frame, at the vblank the
            frame just reached.

            Deliberately not inside stepOneFrame() — that also runs runahead's re-simulated
            frames, whose whole timeline is thrown away and restored at the top of the next
            frame, so patching them would be work nobody ever sees. And deliberately nowhere
            near psx_update(), which runs half a million times a frame.

            The hardcore interlock is re-asserted from the authoritative source every frame
            rather than being set once at boot and trusted. It is one call returning a
            compile-time false today; when hardcore is eventually switched on, cheats go off
            on the very next frame with nothing left to remember to wire up.

            Cost with no cheats armed: psx_cheats_apply() is a branch on a pointer plus one
            atomic load, sixty times a second. See psx/cheats.h.
        */
        psx_cheats_set_inhibited(armsx_ach_hardcore_active() ? 1 : 0);
        psx_cheats_apply(psx_);

        queueAudioForFrame();

        // The grant ends with the frame it was made for. A frame that reached vblank in fewer
        // cycles than its budget covered still has some left, and everything below this line —
        // runahead's re-simulated frames above all — drives psx_update() outside any frame the
        // host is going to pull. Revoking it here is what keeps "the SPU only ever produces the
        // frames the audio device asked for" true by construction rather than by inspection.
        if (psx_) {
            psx_spu_begin_frame(psx_->spu, 0);
        }

        // Rewind snapshot for the frame that just happened. After the audio pull so the
        // snapshot describes a whole frame, and before runahead so the ring records the real
        // timeline rather than the look-ahead.
        psx_rewind_notify_frame(psx_, frameRate());

        /*
            Runahead ([emulation] runahead).

            The machine has advanced exactly one frame — that is the timeline. Snapshot it,
            then run N MORE frames with the same held input and WITHOUT pulling audio, and
            leave VRAM showing that Nth frame. What gets presented is therefore N frames into
            the future, which is the whole point: the picture reacts to a press N frames
            sooner. The snapshot is put back at the top of the next host frame
            (runaheadRestore), before any new input, so the machine still only ever advances
            one frame per frame.

            Audio deliberately comes from the REAL frame only. Queueing the look-ahead frames
            as well would play each frame's audio N+1 times.

            The cost is unavoidable and is charged every single frame: one state save, one
            state load, and N extra emulated frames. On a device that is already at 100% this
            does not hide latency, it halves the frame rate — which is why the default is 0
            and why the UI says so.
        */
        const int runahead = psx_runahead_frames();

        if (runahead > 0) {
            if (psx_runahead_save(psx_) == PSX_STATE_OK) {
                for (int i = 0; i < runahead; i++) {
                    stepOneFrame();
                }

                runahead_restore_pending_ = true;
            } else {
                psx_runahead_configure(0);
                log_error("runahead: snapshot failed; runahead off");
            }
        }

#if defined(HW_DEBUG)
        traceHardwareFrameState("frame-end", steps);
#endif
        return steps;
    }

    void setDebugView(bool enabled) {
        debug_view_ = enabled;
    }

    bool debugView() const {
        return debug_view_;
    }

    bool reset() {
        if (!psx_) {
            return false;
        }

        psxe_diag_breadcrumbf("Soft reset requested title=%s", title_.empty() ? "(none)" : title_.c_str());
        setFastForwardEnabled(false);
        // psx_soft_reset() empties the rewind ring and the look-ahead slot; this clears the
        // front-end's half of that handshake so the next frame does not try to restore one.
        runahead_restore_pending_ = false;
        psx_soft_reset(psx_);
        return true;
    }

    bool swapDisc(const std::filesystem::path& path) {
        if (!psx_) {
            return false;
        }

        if (psx_swap_disc(psx_, path.string().c_str()) == 0) {
            return false;
        }

        setFastForwardEnabled(false);
        runahead_restore_pending_ = false;
        disc_path_ = path;
        launch_kind_ = LaunchKind::Disc;
        title_ = StemToTitle(path);
        psxe_diag_breadcrumbf("Disc swapped path=%s", path.string().c_str());
        psx_soft_reset(psx_);
        return true;
    }

    ArmsxGameInfo currentGameInfo() const {
        ArmsxGameInfo info;

        if (!psx_) {
            return info;
        }

        info.has_game = true;
        info.title = title_.empty() ? "ARMSX" : title_;
        info.subtitle = launch_kind_ == LaunchKind::Bios ? "No disc inserted" : (disc_path_.empty() ? exe_path_.filename().string() : disc_path_.filename().string());
        info.title_id = NormalizeModel(info.title);
        info.path = launch_kind_ == LaunchKind::Bios ? std::filesystem::path("bios://boot") : (disc_path_.empty() ? exe_path_ : disc_path_);
        return info;
    }

    // settingsOverlayLines / performanceOverlayLines removed with the FSUI cut (imgui overlays).

#ifdef USE_HARDWARE
    // HW_RENDERER_DESIGN.md §4.6: where the GPU path cannot serve a game it falls back
    // EXPLICITLY AND LOGGED, never silently. The GLES backend disables itself on a
    // persistent GL error rather than presenting an empty render target that looks like a
    // black screen with no clue attached; this is the frontend half of that contract.
    // The present layer may be sourcing straight from the GL rasterizer's scanout texture
    // (armsx_hw_gl_present_texture). That texture dies with the backend, and a deleted GL
    // name can be recycled by any later object, so the adoption has to be dropped BEFORE the
    // backend is torn down — otherwise present samples whatever now owns that name.
    void releaseAdoptedTexture() {
        if (render_ && hw_rt_is_gl_) {
            armsx_renderer_adopt_gl_texture(render_, 0, 0, 0, SDL_PIXELFORMAT_UNKNOWN);
            frame_uploaded_ = false;
        }
    }

    void checkRasterizerHealth() {
        if (!hw_rt_backend_ || !hw_rt_is_gl_ || !armsx_hw_gl_failed(hw_rt_backend_)) {
            return;
        }

        psx_gpu_t* gpu = psx_ ? psx_get_gpu(psx_) : nullptr;

        log_error("GLES rasterizer disabled itself: %s", armsx_hw_gl_status());
        releaseAdoptedTexture();
        psx_gpu_set_backend(gpu, nullptr);
        armsx_hw_gl_destroy(hw_rt_backend_);
        hw_rt_backend_ = nullptr;
        hw_rt_is_gl_ = false;

        if (gpu && (rasterizer_mode_ != 3)) {
            hw_rt_backend_ = armsx_hw_rt_create(gpu, internal_scale_);

            if (hw_rt_backend_) {
                psx_gpu_set_backend(gpu, hw_rt_backend_);
                log_info("Fell back to the CPU internal-resolution rasterizer at %dx",
                         psx_gpu_resolution_scale(gpu));
                return;
            }
        }

        log_info("Fell back to the software rasterizer.");
        texture_snapshot_.clear();
        frame_uploaded_ = false;
    }
#endif

    /* ---- [video] deinterlace ---------------------------------------------------------
       This core has no field concept at all: in a 480-line mode psx_get_dmode_height()
       returns 480 and the frame is read as 480 consecutive VRAM rows, i.e. whatever the
       game left there, which is a WEAVE of both fields. That is mode 0 and it is what has
       always shipped.

       bob      present field 0 only, each kept line repeated over the line it replaces.
                Halves vertical detail and removes combing outright.
       adaptive measure combing on this frame and bob only when it is actually there, so a
                static or progressive-in-a-480-mode scene keeps full detail.

       Geometry is deliberately PRESERVED (lines are repeated, not dropped): the texture keeps
       its width, height and pitch, so the dirty-row scan, the snapshot, the crop rect and the
       aspect all stay exactly as they were. The alternative — halving the height — would have
       to be understood by four other pieces of code.

       Costs nothing at all in weave, and nothing but a sparse read in adaptive-on-a-static
       scene. */
    static int deinterlaceLuma(uint16_t v) {
        return (int)(v & 31u) + (int)((v >> 5) & 31u) + (int)((v >> 10) & 31u);
    }

    /* Sparse three-line comb detector: a combed pixel sits far from BOTH vertical neighbours
       while those neighbours agree with each other, which is what a field mismatch looks like
       and ordinary vertical detail does not. Sampled every other native row and every 4th
       pixel — this runs per frame and the answer only has to be right in aggregate. */
    bool frameIsCombed(const uint8_t* source, uint32_t stride, int native_w, int native_h,
                       int scale) const {
        if (native_h < 4 || native_w < 8) {
            return false;
        }

        const int threshold = 6;   /* 5-bit channels summed: ~6/93 of full range */
        long sampled = 0;
        long combed = 0;

        for (int n = 1; n + 1 < native_h; n += 2) {
            const auto* above = reinterpret_cast<const uint16_t*>(
                source + (size_t)((n - 1) * scale) * stride);
            const auto* mid = reinterpret_cast<const uint16_t*>(
                source + (size_t)(n * scale) * stride);
            const auto* below = reinterpret_cast<const uint16_t*>(
                source + (size_t)((n + 1) * scale) * stride);

            for (int x = 0; x < native_w; x += 4) {
                const int sx = x * scale;
                const int a = deinterlaceLuma(above[sx]);
                const int b = deinterlaceLuma(mid[sx]);
                const int c = deinterlaceLuma(below[sx]);
                const int da = a - b;
                const int dc = c - b;

                sampled++;

                if (((da > threshold) && (dc > threshold)) ||
                    ((da < -threshold) && (dc < -threshold))) {
                    combed++;
                }
            }
        }

        /* 2% of sampled pixels. Below that it is film grain or dither, not a field split. */
        return (sampled > 0) && ((combed * 50) > sampled);
    }

    const uint8_t* deinterlaceFrame(const uint8_t* source, uint32_t stride, int mode, int scale) {
        if (scale < 1) {
            scale = 1;
        }

        const int native_h = texture_height_ / scale;
        const int native_w = texture_width_ / scale;

        if ((native_h < 4) || (texture_height_ % scale) != 0) {
            return source;
        }

        if (mode == 2 && !frameIsCombed(source, stride, native_w, native_h, scale)) {
            return source;   /* adaptive, and this frame does not need it */
        }

        const size_t need = (size_t)stride * (size_t)texture_height_;

        if (deint_buffer_.size() != need) {
            deint_buffer_.assign(need, 0);
        }

        for (int row = 0; row < texture_height_; ++row) {
            const int n = row / scale;
            const int k = row % scale;
            /* Field 0 is the even NATIVE lines; both lines of a pair come from the even one. */
            const int src_row = (n & ~1) * scale + k;

            std::memcpy(deint_buffer_.data() + (size_t)row * stride,
                        source + (size_t)src_row * stride,
                        (size_t)texture_width_ * (size_t)SDL_BYTESPERPIXEL(texture_format_));
        }

        return deint_buffer_.data();
    }

    /* Live override wins over settings.toml, matching every other display lever here. */
    static int resolveDeinterlace(const FrontendSettings& settings) {
        const int host = g_host_deinterlace.load(std::memory_order_acquire);
        return host < 0 ? settings.deinterlace : host;
    }

    static int resolveOverscanCrop(const FrontendSettings& settings) {
        const int host = g_host_overscan_crop.load(std::memory_order_acquire);
        return host < 0 ? settings.overscan_crop : host;
    }

    static int resolveDisplayRotation(const FrontendSettings& settings) {
        const int host = g_host_display_rotation.load(std::memory_order_acquire);
        return (host < 0 ? settings.display_rotation : host) & 3;
    }

    /* True only for a frame this pass can actually act on: an interlaced (480-line) mode, a
       16-bit frame, and not the whole-VRAM debug view. 24bpp playback is packed RGB888 whose
       rows are not the console's scanlines in the first place. */
    bool deinterlaceApplies(const FrontendSettings& settings) const {
        return (resolveDeinterlace(settings) != 0) && !debug_view_ && psx_ && psx_->gpu &&
               ((psx_->gpu->display_mode & 0x4) != 0) && psx_get_display_format(psx_) == 0;
    }

    /* Drains a setPs1TextureOptions() request onto the EMULATION thread. psx_texrep_configure
       frees every decoded replacement and the rasterizers hold pointers into them, so this
       cannot run on the caller's thread. One relaxed atomic load per frame when idle. */
    void applyPendingTextureOptions() {
        if (!g_host_texture_pending.load(std::memory_order_acquire) || !psx_) {
            return;
        }

        g_host_texture_pending.store(false, std::memory_order_release);

        const int dump = g_host_texture_dump.load(std::memory_order_acquire);
        const int replace = g_host_texture_replace.load(std::memory_order_acquire);
        std::string dir;

        {
            std::lock_guard<std::mutex> lock(g_host_texture_lock);
            dir = g_host_texture_dir;
        }

        const std::string resolved = ResolveTextureDir(dir);

        psx_texrep_configure(psx_get_gpu(psx_), dump > 0 ? 1 : 0, replace > 0 ? 1 : 0,
                             resolved.c_str());
        log_info("Texture options: dump=%s replace=%s dir=%s",
                 dump > 0 ? "on" : "off", replace > 0 ? "on" : "off", resolved.c_str());
    }

    void updateTexture(const FrontendSettings& settings) {
        if (!psx_ || !render_) {
            return;
        }

#ifdef USE_HARDWARE
        checkRasterizerHealth();
#endif

        applyPendingTextureOptions();

        int next_width = debug_view_ ? PSX_GPU_FB_WIDTH : static_cast<int>(psx_get_display_width(psx_));
        int next_height = debug_view_ ? PSX_GPU_FB_HEIGHT : static_cast<int>(psx_get_display_height(psx_));
        // Falls back to the last NATIVE size, not texture_width_/height_, which may already
        // carry an internal-resolution multiplier and would get scaled a second time below.
        if (next_width <= 0) {
            next_width = texture_native_width_ > 0 ? texture_native_width_ : 320;
        }
        if (next_height <= 0) {
            next_height = texture_native_height_ > 0 ? texture_native_height_ : 240;
        }
        texture_native_width_ = next_width;
        texture_native_height_ = next_height;
        const Uint32 next_format = debug_view_ || !psx_get_display_format(psx_) ? SDL_PIXELFORMAT_BGR555 : SDL_PIXELFORMAT_RGB24;
        const bool use_vram_source = debug_view_ || !psx_ || !psx_->gpu ? false : ((psx_->gpu->disp_y + next_height) > PSX_GPU_FB_HEIGHT);

        // Internal-resolution scanout. The whole-VRAM debug view and 24bpp playback both
        // have to stay native: 24bpp reinterprets VRAM bytes as packed RGB888, which the
        // upscaled target does not contain (HW_RENDERER_DESIGN.md §4.4). Everything else
        // reads the backend's render target directly, so the upscaled pixels reach the
        // present layer without a downsample.
        int display_scale = 1;
        Uint32 display_stride = PSX_GPU_FB_STRIDE;
        const void* scaled_surface = nullptr;
#ifdef USE_HARDWARE
        const bool want_native_scanout = use_vram_source || (next_format != SDL_PIXELFORMAT_BGR555);

        // The brokered seam (HW_RENDERER_DESIGN.md §0.5.3/§0.5.5). When the GLES rasterizer
        // and the GL present backend share a context, the frame the rasterizer just produced
        // is already a texture in the presenter's namespace: hand it over instead of reading
        // it back and uploading it again. Both of those are S^2 in the internal scale, and on
        // a tiler the readback is also a full pipeline sync — together they are the largest
        // remaining cost in the upscaled path.
        //
        // Every condition here is a fallback to the readback path, not an error. The debug
        // VRAM view and 24bpp playback need native pixels (§4.4), and armsx_hw_gl_present_texture()
        // refuses on its own side whenever the texture would be meaningless to the presenter.
        //
        // Deinterlacing also declines the seam: it is a CPU pass over the finished frame, so
        // the pixels have to come back to the host. It is opt-in and only fires in 480-line
        // modes, so the fast path is untouched in every other case.
        const bool adopted_gl_texture =
            !deinterlaceApplies(settings) &&
            !want_native_scanout && !debug_view_ && psx_ && psx_->gpu && hw_rt_backend_ &&
            hw_rt_is_gl_ && (armsx_renderer_backend(render_) == ARMSX_RENDER_BACKEND_OPENGL) &&
            (armsx_hw_gl_present_texture(hw_rt_backend_, render_) != 0);

        if (adopted_gl_texture) {
            // The adopted texture is the render target at internal resolution, so the
            // presentation size is the same one the upload path would have produced — and
            // armsx_render_compute_dst() therefore letterboxes it identically.
            display_scale = psx_gpu_resolution_scale(psx_->gpu);
            next_width *= display_scale;
            next_height *= display_scale;
        } else if (!want_native_scanout && psx_ && psx_->gpu) {
            uint32_t backend_stride = PSX_GPU_FB_STRIDE;
            const bool backend_scanout = psx_gpu_backend_owns_display(psx_->gpu) != 0;
            const void* surface =
                psx_gpu_get_display_surface(psx_->gpu, 0, &display_scale, &backend_stride);

            // Note the "|| backend_scanout": a GPU backend at 1x must still be the thing on
            // screen. Gating on display_scale > 1 alone would leave the software shadow's
            // (correct) image visible at 1x and hide a completely broken GPU path, which is
            // exactly the scale the parity gate lives at.
            if (surface && backend_stride && ((display_scale > 1) || backend_scanout)) {
                scaled_surface = surface;
                display_stride = static_cast<Uint32>(backend_stride);
                next_width *= display_scale;
                next_height *= display_scale;
            } else {
                // No backend, or it handed back the native surface (display disabled).
                display_scale = 1;
            }
        }
#endif

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            int output_width = 0;
            int output_height = 0;
            armsx_renderer_output_size(render_, &output_width, &output_height);
            psxe_diag_logf(
                "hw",
                "texture-select frame=%llu display_mode=0x%08x gpustat=0x%08x display_enable=%d dmode=%ux%u display=%ux%u aspect=%.3f disp_window=(%u,%u)-(%u,%u) disp_y=%u offset=(%d,%d) output=%dx%d next=%dx%d format=%s source=%s stretch=%s debug=%s timing=%s target_fps=%.3f",
                static_cast<unsigned long long>(vblank_counter_),
                psx_ && psx_->gpu ? psx_->gpu->display_mode : 0u,
                psx_ && psx_->gpu ? psx_->gpu->gpustat : 0u,
                psx_ && psx_->gpu ? psx_->gpu->display_enable : 0,
                psx_ ? psx_get_dmode_width(psx_) : 0u,
                psx_ ? psx_get_dmode_height(psx_) : 0u,
                psx_ ? psx_get_display_width(psx_) : 0u,
                psx_ ? psx_get_display_height(psx_) : 0u,
                psx_ ? psx_get_display_aspect(psx_) : 0.0,
                psx_ && psx_->gpu ? psx_->gpu->disp_x1 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y1 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_x2 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y2 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y : 0u,
                psx_ && psx_->gpu ? psx_->gpu->off_x : 0,
                psx_ && psx_->gpu ? psx_->gpu->off_y : 0,
                output_width,
                output_height,
                next_width,
                next_height,
                SDL_GetPixelFormatName(next_format),
                use_vram_source ? "vram" : "display",
                settings.stretch_mode ? "true" : "false",
                debug_view_ ? "true" : "false",
                timingModeTitle(),
                targetFrameRate()
            );
        }
#endif

        if ((next_width != texture_width_) || (next_height != texture_height_) || (next_format != texture_format_)) {
            texture_width_ = next_width;
            texture_height_ = next_height;
            texture_format_ = next_format;
            texture_snapshot_.clear();
            frame_uploaded_ = false;

#ifdef USE_HARDWARE
            if (hw_renderer_) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
#endif
        }

#ifdef USE_HARDWARE
        if (adopted_gl_texture) {
            // There is no CPU frame this frame, so the snapshot the dirty-row scan compares
            // against describes a frame the present layer is no longer showing. Dropping it
            // costs nothing (the vector keeps its capacity) and makes whichever frame falls
            // back to upload_frame() do a full upload, which is the only safe answer.
            texture_snapshot_.clear();
            frame_uploaded_ = true;

            // Say so rather than going quiet: on the brokered seam the frame never becomes CPU
            // pixels, so the probe below cannot look at it, and an armed run that logged nothing
            // would read as "the probe is broken".
            if (g_resume_probe_frames > 0) {
                --g_resume_probe_frames;
                psxe_diag_logf("hw",
                               "resume-probe left=%d source=adopted-gl-texture %dx%d (no CPU frame "
                               "to sample; the GL render target IS the presented image)",
                               g_resume_probe_frames, texture_width_, texture_height_);
            }

            return;
        }
#endif

        const void* display_buffer = scaled_surface
                                         ? scaled_surface
                                         : (use_vram_source ? psx_get_vram(psx_) : psx_get_display_buffer(psx_));
        if (!display_buffer) {
            return;
        }

        const int bytes_per_pixel = SDL_BYTESPERPIXEL(texture_format_);
        const size_t row_bytes = static_cast<size_t>(texture_width_) * static_cast<size_t>(bytes_per_pixel);
        const size_t snapshot_size = static_cast<size_t>(display_stride) * static_cast<size_t>(texture_height_);
        const auto* source = static_cast<const uint8_t*>(display_buffer);

        // [video] deinterlace. Substitutes an equally-shaped buffer, so everything below —
        // the dirty scan, the snapshot and the upload — is unchanged and unaware.
        if (deinterlaceApplies(settings)) {
            source = deinterlaceFrame(source, display_stride, resolveDeinterlace(settings),
                                      display_scale);
        }

        // Dirty-row scan: only the changed span of the framebuffer is handed to the backend.
        // Unchanged from the pre-abstraction path; every backend gets the same row range.
        int first_dirty_row = 0;
        int last_dirty_row = texture_height_ - 1;
        if (texture_snapshot_.size() == snapshot_size) {
            first_dirty_row = texture_height_;
            last_dirty_row = -1;
            for (int row = 0; row < texture_height_; ++row) {
                const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(display_stride);
                if (std::memcmp(source + offset, texture_snapshot_.data() + offset, row_bytes) != 0) {
                    first_dirty_row = std::min(first_dirty_row, row);
                    last_dirty_row = row;
                }
            }
        } else {
            texture_snapshot_.assign(snapshot_size, 0);
        }

        // `resume_probe`: is the picture we are about to hand the present layer actually there?
        // See resumeProbeEnabled() for how to read the answer. Disarmed this is one compare.
        if (g_resume_probe_frames > 0) {
            --g_resume_probe_frames;

            size_t nonzero = 0;
            int first_nonzero_row = -1;
            for (int row = 0; row < texture_height_; ++row) {
                const uint8_t* line = source + (static_cast<size_t>(row) * static_cast<size_t>(display_stride));
                for (size_t byte = 0; byte < row_bytes; ++byte) {
                    if (line[byte]) {
                        ++nonzero;
                    }
                }
                if (nonzero && first_nonzero_row < 0) {
                    first_nonzero_row = row;
                }
            }

            psxe_diag_logf("hw",
                           "resume-probe left=%d source=%s %dx%d stride=%u format=%s nonzero=%zu "
                           "first_row=%d dirty=[%d..%d] scale=%d",
                           g_resume_probe_frames,
                           scaled_surface ? "backend-readback"
                                          : (use_vram_source ? "vram-shadow" : "software-display"),
                           texture_width_, texture_height_, (unsigned)display_stride,
                           SDL_GetPixelFormatName(texture_format_), nonzero, first_nonzero_row,
                           first_dirty_row, last_dirty_row, display_scale);
        }

        if (!armsx_renderer_upload_frame(render_,
                                         source,
                                         texture_width_,
                                         texture_height_,
                                         static_cast<int>(display_stride),
                                         texture_format_,
                                         first_dirty_row,
                                         last_dirty_row)) {
            return;
        }

        frame_uploaded_ = true;

        for (int row = first_dirty_row; row <= last_dirty_row; ++row) {
            const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(display_stride);
            std::memcpy(texture_snapshot_.data() + offset, source + offset, row_bytes);
        }
    }

    // Present the emulated framebuffer through the active presentation backend
    // (frontend/render.h). The aspect decision stays here because it is a *settings* concern;
    // the letterbox math itself is shared by every backend (armsx_render_compute_dst).
    void draw(const FrontendSettings& settings) {
        if (!frame_uploaded_ || !render_) {
            return;
        }

        // Live overrides from the host UI (JNI). Aspect and stretch are pure presentation
        // properties — nothing in the VM depends on them — so changing either from the
        // in-game menu takes effect on the very next frame rather than at the next launch.
        int display_aspect = settings.display_aspect;
        bool stretch = settings.stretch_mode;
        if (const int override_aspect = g_host_display_aspect.load(std::memory_order_acquire);
            override_aspect >= 0) {
            display_aspect = override_aspect;
        }
        if (const int override_stretch = g_host_stretch_mode.load(std::memory_order_acquire);
            override_stretch >= 0) {
            stretch = override_stretch != 0;
        }

        float aspect = 4.0f / 3.0f;
        if (debug_view_) {
            aspect = static_cast<float>(texture_width_) / static_cast<float>(std::max(texture_height_, 1));
        } else if (display_aspect == 3) {
            /* Custom. The live override wins over the file so an in-game slider is immediate;
               both are sanity-clamped because a zero or negative ratio would make
               compute_dst produce a degenerate rect and the screen go black. */
            const float live = g_host_display_aspect_custom.load(std::memory_order_acquire);
            const float configured = settings.display_aspect_custom;
            const float chosen = live > 0.0f ? live : configured;
            aspect = (chosen >= kMinCustomAspect && chosen <= kMaxCustomAspect) ? chosen
                                                                               : (4.0f / 3.0f);
        } else if (display_aspect == 2) {
            aspect = 16.0f / 9.0f;
        } else if (display_aspect == 1) {
            aspect = 1.0f;
        } else {
            // Classic is a HARD 4:3 — the display aspect of a PlayStation on a CRT, which is
            // what every mode from 256x240 to 640x480 was authored for. It is deliberately NOT
            // psx_get_display_aspect(): that returns the framebuffer's own pixel ratio capped
            // at 4:3, so a 256-wide game came out at 1.067 — visually identical to Square, and
            // the reason "Classic" and "Square" looked like the same dead setting.
            aspect = 4.0f / 3.0f;
        }

        armsx_render_frame_params_t params{};
        params.stretch = stretch;
        params.linear_filter = settings.texture_scale_mode;
        params.portrait_top = g_host_portrait_top.load(std::memory_order_acquire) != 0;
        params.portrait_top_inset = g_host_portrait_top_inset.load(std::memory_order_acquire);
        params.integer_scaling = g_host_integer_scaling.load(std::memory_order_acquire) < 0
            ? settings.integer_scaling
            : g_host_integer_scaling.load(std::memory_order_acquire) != 0;
        params.aspect = aspect;

        /* ---- [video] overscan_crop -----------------------------------------------------
           Trims the blanking border games leave around the picture and lets the kept region
           fill the same destination rect — i.e. it zooms, which is what makes it worth
           having. Expressed as a fraction of each edge rather than fixed pixel counts
           because the horizontal resolution varies per game (256/320/368/512/640) and a
           fixed 8px means something different in each.

             small  1/32 of each edge (~3%)  — the usual garbage column/row
             full   1/16 of each edge (~6%)  — the "all borders" setting

           Skipped for the whole-VRAM debug view, where the border is the point. */
        const int crop_mode = debug_view_ ? 0 : resolveOverscanCrop(settings);

        if (crop_mode > 0 && texture_width_ > 16 && texture_height_ > 16) {
            const int div = (crop_mode == 2) ? 16 : 32;
            const int inset_x = texture_width_ / div;
            const int inset_y = texture_height_ / div;

            params.crop_x = inset_x;
            params.crop_y = inset_y;
            params.crop_w = texture_width_ - inset_x * 2;
            params.crop_h = texture_height_ - inset_y * 2;
        }

        /* [video] display_rotation. Pure presentation: compute_dst transposes the rect and
           each backend spins its own draw. The debug view rotates too — there is no reason
           for it not to. */
        params.rotation = resolveDisplayRotation(settings);

        if (aspect_traces_ < 3 || display_aspect != last_logged_aspect_mode_ ||
            stretch != last_logged_stretch_) {
            aspect_traces_++;
            last_logged_aspect_mode_ = display_aspect;
            last_logged_stretch_ = stretch;
            psxe_diag_logf("renderer",
                           "present mode=%s (%d) stretch=%s aspect=%.4f source=%dx%d",
                           display_aspect == 3 ? "custom"
                                               : (display_aspect == 2 ? "wide16x9"
                                                  : (display_aspect == 1 ? "square1x1"
                                                                        : "classic4x3")),
                           display_aspect, stretch ? "true" : "false", aspect, texture_width_,
                           texture_height_);
        }

        armsx_renderer_present(render_, &params);

        // The old SDL path also composited armsx_hw_renderer_overlay_texture() here. That
        // accessor is a compatibility stub that unconditionally returns NULL
        // (frontend/gpu_hw.c), so the blit was dead code and is not reproduced.
    }

    void finishHardwareFrame() {
#ifdef USE_HARDWARE
        if (hardware_backend_active_ && hw_renderer_) {
            armsx_hw_renderer_end_frame(hw_renderer_);
        }
#endif
    }

#if defined(HW_DEBUG)
    void traceHardwareFrameState(const char* stage, std::uint32_t steps) const {
#ifdef USE_HARDWARE
        if (!hardware_backend_active_ || !psx_ || !psx_->gpu) {
            return;
        }

        const psx_gpu_t* gpu = psx_->gpu;
        psxe_diag_logf(
            "hw",
            "%s frame=%llu steps=%u timing=%s target_fps=%.3f display_mode=0x%08x gpustat=0x%08x display_enable=%d dmode=%ux%u display=%ux%u draw=(%u,%u)-(%u,%u) disp=(%u,%u)-(%u,%u) offset=(%d,%d) texture=%dx%d renderer=%p",
            stage ? stage : "frame-state",
            static_cast<unsigned long long>(vblank_counter_),
            steps,
            timingModeTitle(),
            targetFrameRate(),
            gpu->display_mode,
            gpu->gpustat,
            (gpu->gpustat & 0x00800000) != 0,
            psx_get_dmode_width(psx_),
            psx_get_dmode_height(psx_),
            psx_get_display_width(psx_),
            psx_get_display_height(psx_),
            gpu->draw_x1,
            gpu->draw_y1,
            gpu->draw_x2,
            gpu->draw_y2,
            gpu->disp_x1,
            gpu->disp_y1,
            gpu->disp_x2,
            gpu->disp_y2,
            gpu->off_x,
            gpu->off_y,
            texture_width_,
            texture_height_,
            (void*)render_
        );
#else
        (void)stage;
        (void)steps;
#endif
    }
#endif

    bool saveScreenshot(const std::filesystem::path& path) {
        if (!psx_) {
            return false;
        }

        const int width = texture_width_;
        const int height = texture_height_;
        const Uint32 format = texture_format_;
        // texture_width_/height_ already carry the internal-resolution multiplier, so the
        // source and its stride have to come from the same surface uploadFrame() used —
        // reading the native buffer at a scaled height would run off the end of VRAM.
        int shot_scale = 1;
        Uint32 shot_stride = PSX_GPU_FB_STRIDE;
        const void* source = debug_view_ ? psx_get_vram(psx_) : psx_get_display_buffer(psx_);

#ifdef USE_HARDWARE
        if (!debug_view_ && (format == SDL_PIXELFORMAT_BGR555) && psx_->gpu) {
            uint32_t backend_stride = PSX_GPU_FB_STRIDE;
            const void* surface =
                psx_gpu_get_display_surface(psx_->gpu, 0, &shot_scale, &backend_stride);

            if (surface && (shot_scale > 1)) {
                source = surface;
                shot_stride = static_cast<Uint32>(backend_stride);
            } else {
                shot_scale = 1;
            }
        }
#endif

        if (!debug_view_ && (shot_scale == 1) &&
            ((psx_->gpu->disp_y + texture_height_) > PSX_GPU_FB_HEIGHT)) {
            source = psx_get_vram(psx_);
        }

        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);

        if (!surface) {
            return false;
        }

        const size_t row_size = static_cast<size_t>(surface->pitch);
        const uint8_t* src = static_cast<const uint8_t*>(source);
        uint8_t* dst = static_cast<uint8_t*>(surface->pixels);

        for (int row = 0; row < height; row++) {
            std::memcpy(dst + (static_cast<size_t>(row) * static_cast<size_t>(surface->pitch)), src + (static_cast<size_t>(row) * static_cast<size_t>(shot_stride)), row_size);
        }

        const int result = SDL_SaveBMP(surface, path.string().c_str());
        SDL_FreeSurface(surface);
        return result == 0;
    }

    double frameRate() const {
        if (!psx_ || !psx_->gpu) {
            return 59.29;
        }

        return static_cast<double>(psx_gpu_frame_rate(psx_->gpu));
    }

    // The rate the frame pacer aims for. "Uncapped" returns a deliberately unreachable rate
    // rather than 0: waitForFrameDeadline() then always finds the deadline already passed and
    // never sleeps, which is exactly "run as fast as the device manages" — no special case in
    // the pacer, and no divide-by-zero in updateFramePeriod().
    double targetFrameRate() const {
        if (fast_forward_enabled_) {
            return (fast_forward_speed_ <= 0.0) ? kUncappedFrameRate : frameRate() * fast_forward_speed_;
        }

        if (!frame_limit_) {
            return kUncappedFrameRate;
        }

        double target = frameRate() * (static_cast<double>(speed_percent_) / 100.0);

        // The absolute cap only ever lowers the target — a 120 fps "cap" on a 59.94 Hz game
        // must not make it run at double speed.
        if (fps_limit_ > 0) {
            target = std::min(target, static_cast<double>(fps_limit_));
        }

        return std::max(target, 1.0);
    }

    // Whether the emulated stream needs rate-converting to reach the device.
    //
    // False for a plain, limited, 100% session — and there the audio path is byte-for-byte
    // what it was before any of this existed. True whenever the emulation is deliberately
    // running at something other than realtime: fast-forward, the limiter switched off, or a
    // speed percentage / fps cap that is not the game's own rate.
    bool audioRateConversionActive() const {
        if (fast_forward_enabled_ || !frame_limit_) {
            return true;
        }

        if (speed_percent_ != 100) {
            return true;
        }

        return (fps_limit_ > 0) && (static_cast<double>(fps_limit_) < frameRate());
    }

    // What the converter should expect the emulation to run at, used only to seed its rate
    // estimate. The closed loop finds the truth from there; this just avoids an audible
    // sweep on the first tenth of a second.
    double expectedSpeedMultiplier() const {
        if (fast_forward_enabled_) {
            return (fast_forward_speed_ > 0.0) ? fast_forward_speed_ : 2.0;
        }

        if (!frame_limit_) {
            return 2.0;
        }

        const double nominal = frameRate();
        return (nominal > 0.0) ? std::max(targetFrameRate() / nominal, 0.05) : 1.0;
    }

    std::uint64_t vblankCounter() const {
        return vblank_counter_;
    }

#ifdef USE_HARDWARE
    bool hardwareBackendActive() const {
        return hardware_backend_active_;
    }

    int textureWidth() const {
        return texture_width_;
    }

    int textureHeight() const {
        return texture_height_;
    }

    Uint32 textureFormat() const {
        return texture_format_;
    }
#endif

    const char* timingModeTitle() const {
        if (!psx_ || !psx_->gpu) {
            return "NTSC-like";
        }

        return psx_gpu_is_pal_mode(psx_->gpu) ? "PAL-like" : "NTSC-like";
    }

  private:
    static void SessionVblankEvent(psx_gpu_t* gpu) {
        if (gpu) {
            if (auto* session = static_cast<ArmsxSession*>(gpu->udata[0])) {
                session->vblank_counter_++;
            }

            psxe_gpu_vblank_timer_event_cb(gpu);
        }
    }

    static void AudioUpdate(void* userdata, uint8_t* buffer, int size) {
        if (!buffer || size <= 0) {
            return;
        }

        std::memset(buffer, 0, static_cast<size_t>(size));

        auto* session = static_cast<ArmsxSession*>(userdata);
        if (!session) {
            return;
        }

        session->consumeQueuedAudio(buffer, static_cast<size_t>(size));
    }

    void updateAudioPlaybackState() {
        if (!audio_dev_) {
            return;
        }

        // Fast-forward no longer parks the device. The emulated stream is resampled down to
        // the device rate in queueAudioForFrame() instead, so there is a continuous stream to
        // play; only a paused VM stops the device, and a paused VM really has nothing to feed
        // it. "Mute during fast-forward" is a gain of zero, not a stopped device — stopping
        // and restarting an OpenSL ES stream on every fast-forward edge pops.
        //
        // audio_suspended_ is belt-and-braces: backgrounding closes the device outright, so this
        // normally has no device to act on. It matters only if a future path leaves one open
        // while the app is off-screen — a setPaused(false) landing then must not hand the
        // callback back its real samples.
        SDL_PauseAudioDevice(audio_dev_, (paused_ || audio_suspended_) ? 1 : 0);
    }

    /*
        `audio_diag` — ONE bounded audio capture, armed by a marker file.

        Four things sound identical to a player and need opposite fixes: the emulation not
        keeping up (host underruns), the XA decoder producing the wrong stream, the mix sitting
        at the wrong level, and the SPU reverb feedback loop railing. Reading the code cannot
        separate them, and two plausible fixes reasoned from a symptom description have already
        been landed on this project and failed. So: measure first.

        `touch files/logs/audio_diag` arms it. The marker is deleted the instant a capture
        starts, so one touch buys exactly one window; ~10 s of it, sampled five times a second,
        written to `audio_diag.txt` beside the marker. Four windows per process, then the probe
        stops for good.

        Cost when idle: one fopen() every kAudioDiagPollFrames frames while budget remains, and
        one predicted branch per generated sample everywhere psx/perf.h's counters are written.
        Nothing here runs per CPU instruction or per pixel.

        Written through its own FILE* rather than psxe_diag_logf() for the reasons gpu.c gives:
        it has to work with diagnostics logging off, it would otherwise drown armsx.log, and a
        player can send one small file instead of the whole log.
    */
    bool audioDiagPath(std::string& out, const char* name) const {
        const char* log_path = psxe_diag_log_path();
        if (!log_path || !log_path[0]) {
            return false;
        }

        const char* slash = std::strrchr(log_path, '/');
        if (!slash) {
            return false;
        }

        out.assign(log_path, static_cast<size_t>(slash - log_path) + 1u);
        out += name;
        return true;
    }

    void audioDiagLine(const char* fmt, ...) {
        if (!audio_diag_file_) {
            return;
        }

        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);

        std::fputs(line, audio_diag_file_);
        std::fputs("\n", audio_diag_file_);
    }

    // Everything the SPU, the CD-ROM and the host queue looked like over the last snapshot
    // period, then the counters are zeroed so the next line describes the next period only.
    void audioDiagSnapshot() {
        psx_spu_t* spu = psx_->spu;
        psx_cdrom_t* cdrom = psx_->cdrom;
        psx_audio_diag_t& d = g_psx_audio_diag;

        size_t queued = 0;
        uint32_t underruns = 0;
        uint64_t underrun_bytes = 0;
        uint32_t overflows = 0;
        SDL_LockAudioDevice(audio_dev_);
        queued = audio_queue_.size() - audio_queue_read_offset_;
        underruns = audio_underruns_;
        underrun_bytes = audio_underrun_bytes_;
        overflows = audio_overflow_resets_;
        audio_underruns_ = 0;
        audio_underrun_bytes_ = 0;
        audio_overflow_resets_ = 0;
        SDL_UnlockAudioDevice(audio_dev_);

        audio_diag_snapshots_++;

        audioDiagLine("");
        audioDiagLine("[t=%.1fs] host: fps_target=%.2f queued=%zu bytes (%.1f ms) "
                      "underruns=%u short=%llu bytes overflow_resets=%u rate_ratio=%.4f "
                      "ff=%d paused=%d",
            static_cast<double>(audio_diag_snapshots_) *
                (static_cast<double>(kAudioDiagSnapshotFrames) / frameRate()),
            frameRate(),
            queued,
            (static_cast<double>(queued) / static_cast<double>(kAudioBytesPerSample)) *
                1000.0 / static_cast<double>(kAudioMixRate),
            underruns,
            static_cast<unsigned long long>(underrun_bytes),
            overflows,
            audio_rate_ratio_,
            fast_forward_enabled_ ? 1 : 0,
            paused_ ? 1 : 0);

        // SPUCNT decoded in full: bit15 enable, bit7 reverb master, bit2 CD-audio reverb,
        // bit0 CD-audio enable. Bits 0 and 2 are the ones nothing in this core consumes.
        audioDiagLine("  spu: spucnt=%04x [enable=%d unmute=%d reverb=%d cd_reverb=%d cd_enable=%d] "
                      "host_skip_reverb=%d mainvol=%04x/%04x(min %04x/%04x) vlout=%04x vrout=%04x "
                      "cdaivol=%08x extivol=%08x eon=%06x mbase=%04x revbaddr=%05x",
            spu->spucnt,
            (spu->spucnt & 0x8000) ? 1 : 0,
            (spu->spucnt & 0x4000) ? 1 : 0,
            (spu->spucnt & 0x0080) ? 1 : 0,
            (spu->spucnt & 0x0004) ? 1 : 0,
            (spu->spucnt & 0x0001) ? 1 : 0,
            spu->reverb_disabled,
            spu->mainlvol, spu->mainrvol,
            d.mainvol_min_l & 0xffffu, d.mainvol_min_r & 0xffffu,
            spu->vlout, spu->vrout,
            spu->cdaivol, spu->extivol,
            spu->eon & 0xffffffu, spu->mbase, spu->revbaddr);

        audioDiagLine("  spu levels: samples=%u silent=%u voices avg=%.2f peak=%u | "
                      "dry_peak=%d/%d dry_clip=%u | revin_peak=%d/%d revin_clip=%u | "
                      "revsum_clip=%u",
            d.spu_samples, d.spu_silent,
            d.spu_samples ? (static_cast<double>(d.spu_voices_sum) /
                             static_cast<double>(d.spu_samples)) : 0.0,
            d.spu_voices_peak,
            d.dry_peak_l, d.dry_peak_r, d.dry_clip,
            d.revin_peak_l, d.revin_peak_r, d.revin_clip,
            d.revsum_clip);

        // rev_calls == 0 with reverb=1 above means the network never ran. revfb_railed
        // approaching rev_calls means the feedback line is pinned: a self-sustaining loop.
        audioDiagLine("  reverb: calls=%u fb_railed=%u (%.1f%%) out_railed=%u out_peak=%d/%d",
            d.rev_calls, d.revfb_railed,
            d.rev_calls ? (100.0 * static_cast<double>(d.revfb_railed) /
                           static_cast<double>(d.rev_calls)) : 0.0,
            d.revout_railed, d.revout_peak_l, d.revout_peak_r);

        audioDiagLine("  keyon: voices=%u negative_volume=%u sweep_mode=%u | "
                      "spu_ram: writes=%u fifo_drop=%u range=%05x-%05x taddr=%05x xfer_mode=%d "
                      "| irq9=%05x en=%d raised=%u (%.1f/s) | spu_gen: ticked=%u inline=%u",
            d.kon_voices, d.kon_vol_negative, d.kon_vol_sweep,
            d.spu_ram_writes, d.spu_fifo_drop,
            (d.spu_ram_lo == 0xffffffffu) ? 0u : d.spu_ram_lo, d.spu_ram_hi,
            spu->taddr, (spu->spucnt >> 4) & 3,
            (unsigned)(spu->irq9addr << 3), (spu->spucnt & 0x40) ? 1 : 0, d.spu_irq_raised,
            // Per second, because the number this has to be compared against is a rate: the
            // metronome voice's own crossing rate. Landing at the frame rate is the tell that
            // interrupts are being quantised to the frame instead of delivered on time.
            static_cast<double>(d.spu_irq_raised) /
                (static_cast<double>(kAudioDiagSnapshotFrames) / frameRate()),
            d.spu_gen_ticked, d.spu_gen_inline);

        // Live state of every sounding voice, sampled at snapshot time (no hot-path cost).
        // phase 0..4 = attack/decay/sustain/release/end. A voice parked in SUSTAIN (2) at a
        // high env with the level never falling is the signature of a note that was never
        // keyed off — which is what "a sound that plays forever" looks like from in here.
        {
            char voices[4096];
            size_t used = 0;
            int listed = 0;

            voices[0] = '\0';

            for (int v = 0; v < 24 && used + 160u < sizeof(voices); v++) {
                if (!spu->data[v].playing) {
                    continue;
                }

                // Is the sample data this voice is reading actually there? 16 bytes at the
                // current decode address; all-zero means the voice is looping over blank SPU
                // RAM, which is what "keyed on, full envelope, no sound" looks like when the
                // transfer never delivered. blank=1 with spu_ram_writes=0 is the whole story.
                const uint32_t addr = spu->data[v].current_addr & (SPU_RAM_SIZE - 1u);
                int blank = 1;
                for (uint32_t b = 0; b < 16u; b++) {
                    if (spu->ram[(addr + b) & (SPU_RAM_SIZE - 1u)] != 0) {
                        blank = 0;
                        break;
                    }
                }

                // pitch: 1000h = 44100 Hz. Printed rather than inferred from how far ca moves,
                // because ring position aliases: a rate of d and a rate of d+ring look the same.
                const int written = std::snprintf(voices + used, sizeof(voices) - used,
                    "%s%d:vol=%04x/%04x env=%04x ph=%d sa=%05x ra=%05x ca=%05x blank=%d "
                    "pitch=%04x out=%d/%d flags=%02x le=%u st=%u ign=%d",
                    listed ? " | " : "",
                    v, spu->voice[v].volumel, spu->voice[v].volumer,
                    spu->voice[v].envcvol, spu->data[v].adsr_phase,
                    (unsigned)(spu->voice[v].adsaddr << 3),
                    (unsigned)(spu->voice[v].adraddr << 3), addr, blank,
                    spu->voice[v].adsampr,
                    d.voice_peak_l[v], d.voice_peak_r[v],
                    d.voice_flags[v], d.voice_loopend[v], d.voice_stop[v],
                    spu->data[v].ignore_loop_addr);

                if (written <= 0) {
                    break;
                }

                used += static_cast<size_t>(written);
                listed++;
            }

            audioDiagLine("  voices(%d): %s", listed, listed ? voices : "(none sounding)");
        }

        audioDiagLine("  cd: mode=%02x [xa_adpcm=%d xa_filter=%d report=%d autopause=%d cdda=%d] "
                      "xa_playing=%d xa_mute=%d mute=%d filter=file %02x/chan %02x "
                      "lba=%u xa_lba=%u atv=%02x/%02x/%02x/%02x",
            cdrom->mode,
            (cdrom->mode & 0x40) ? 1 : 0,
            (cdrom->mode & 0x08) ? 1 : 0,
            (cdrom->mode & 0x04) ? 1 : 0,
            (cdrom->mode & 0x02) ? 1 : 0,
            (cdrom->mode & 0x01) ? 1 : 0,
            cdrom->xa_playing, cdrom->xa_mute, cdrom->mute,
            cdrom->xa_file, cdrom->xa_channel,
            cdrom->lba, cdrom->xa_lba,
            cdrom->vol[0], cdrom->vol[1], cdrom->vol[2], cdrom->vol[3]);

        // Every CD-ROM command issued in the period, oldest first. This is what separates
        // "the game never asked for XA" from "it asked and we dropped it": look for
        // CdlSetmode and read bit6 (0x40) of its parameter against mode_before. It also
        // exposes the cdrom_cmd_setmode() -> cdrom_pause() interaction directly, because a
        // Setmode arriving with read=1 or xa=1 is one that stopped a drive that was running.
        const uint32_t cmds = std::min<uint32_t>(d.cd_cmd_seen, PSX_AUDIO_DIAG_CD_CMDS);
        if (cmds > 0) {
            audioDiagLine("  cd_cmds: %u this period", d.cd_cmd_seen);
        }
        for (uint32_t i = 0; i < cmds; i++) {
            const uint32_t slot =
                (d.cd_cmd_head + PSX_AUDIO_DIAG_CD_CMDS - cmds + i) % PSX_AUDIO_DIAG_CD_CMDS;
            char params[32];
            size_t used = 0;

            params[0] = '\0';
            for (uint8_t p = 0; p < d.cd_cmd[slot].nparams && used + 4u < sizeof(params); p++) {
                const int written = std::snprintf(params + used, sizeof(params) - used,
                                                  "%02x ", d.cd_cmd[slot].param[p]);
                if (written <= 0) {
                    break;
                }
                used += static_cast<size_t>(written);
            }

            audioDiagLine("    cmd %02x %-14s params=[%s] mode_before=%02x xa=%d read=%d state=%d",
                d.cd_cmd[slot].cmd,
                psx_cdrom_command_name(d.cd_cmd[slot].cmd),
                used ? params : "-",
                d.cd_cmd[slot].mode_before,
                d.cd_cmd[slot].xa_playing,
                d.cd_cmd[slot].read_ongoing,
                d.cd_cmd[slot].state);
        }

        audioDiagLine("  xa: accepted=%u filter_rejects=%u nonaudio_skips=%u stop_eor=%u "
                      "stop_far=%u starved=%u walk_peak=%u peak=%d/%d cdda_sectors=%u | "
                      "mix: wrap=%u sat=%u peak=%d/%d",
            d.xa_sectors, d.xa_filter_rejects, d.xa_skip_nonaudio, d.xa_stop_eor,
            d.xa_stop_far, d.xa_starved, d.xa_walk_peak,
            d.xa_peak_l, d.xa_peak_r, d.cdda_sectors,
            d.mix_wrap, d.mix_sat, d.mix_peak_l, d.mix_peak_r);

        // Every sector decision, oldest first, each with the filter that was in force when the
        // fetcher decided. This is what settles "music plays, dialogue does not":
        //   REJECT_FILTER entries whose file/chan is the dialogue stream -> the filter is wrong
        //   no entry for the dialogue channel at all                     -> it never arrived
        //   ACCEPT entries for it but silence                            -> the fault is downstream
        //   STOP_EOR reached while walking (chan != filter_chan)         -> the walk was aborted
        static const char* const kVerdict[PSX_XA_VERDICT_KINDS] = {
            "ACCEPT(nofilter)", "ACCEPT(match)", "REJECT(filter)",
            "SKIP(nonaudio)", "STOP(eor)"
        };

        const uint32_t ring = std::min<uint32_t>(d.xa_hdr_seen, PSX_AUDIO_DIAG_XA_HDRS);
        for (uint32_t i = 0; i < ring; i++) {
            const uint32_t slot =
                (d.xa_hdr_head + PSX_AUDIO_DIAG_XA_HDRS - ring + i) % PSX_AUDIO_DIAG_XA_HDRS;
            const uint8_t sm = d.xa_hdr[slot].submode;
            const uint8_t ci = d.xa_hdr[slot].coding;
            const uint8_t vd = d.xa_hdr[slot].verdict;

            audioDiagLine("    xa lba=%u %-16s file=%02x chan=%02x vs filter=%02x/%02x(on=%d) "
                          "submode=%02x[eor=%d video=%d audio=%d data=%d trig=%d form2=%d "
                          "rt=%d eof=%d] coding=%02x[%s %s %s emphasis=%d]",
                d.xa_hdr[slot].lba,
                (vd < PSX_XA_VERDICT_KINDS) ? kVerdict[vd] : "?",
                d.xa_hdr[slot].file, d.xa_hdr[slot].chan,
                d.xa_hdr[slot].filter_file, d.xa_hdr[slot].filter_chan,
                d.xa_hdr[slot].filter_on,
                sm,
                (sm & 0x01) ? 1 : 0, (sm & 0x02) ? 1 : 0, (sm & 0x04) ? 1 : 0,
                (sm & 0x08) ? 1 : 0, (sm & 0x10) ? 1 : 0, (sm & 0x20) ? 1 : 0,
                (sm & 0x40) ? 1 : 0, (sm & 0x80) ? 1 : 0,
                ci,
                (ci & 0x01) ? "stereo" : "mono",
                (ci & 0x04) ? "18.9kHz" : "37.8kHz",
                (ci & 0x10) ? "8bit" : "4bit",
                (ci & 0x40) ? 1 : 0);
        }

        // Period counters only; the window totals come from the arithmetic on these lines.
        // The minima have to be re-seeded past the memset or the next period reports 0000.
        std::memset(&g_psx_audio_diag, 0, sizeof(g_psx_audio_diag));
        g_psx_audio_diag.mainvol_min_l = 0xffffffffu;
        g_psx_audio_diag.mainvol_min_r = 0xffffffffu;
        g_psx_audio_diag.spu_ram_lo = 0xffffffffu;
    }

    void audioDiagBegin() {
        std::string path;
        if (!audioDiagPath(path, "audio_diag.txt")) {
            return;
        }

        audio_diag_file_ = std::fopen(path.c_str(), "a");
        if (!audio_diag_file_) {
            return;
        }

        audio_diag_seq_++;
        audio_diag_frames_left_ = kAudioDiagFrames;
        audio_diag_snapshots_ = 0;

        audioDiagLine("=== audio_diag capture #%d (%d frames, ~%.0f s) ===",
                      audio_diag_seq_, kAudioDiagFrames,
                      static_cast<double>(kAudioDiagFrames) / frameRate());

        // Also breadcrumbed so "did it fire?" is answerable from armsx.log alone.
        psxe_diag_logf("audio", "audio_diag: capture #%d armed, %d left this process",
                       audio_diag_seq_, audio_diag_budget_);
        audioDiagLine("title=%s disc=%s", title_.c_str(),
                      disc_path_.empty() ? "(none)" : disc_path_.string().c_str());
        audioDiagLine("device: mix_rate=%d buffer=%u frames (%.1f ms) volume=%d ff_volume=%d "
                      "muted=%d swap=%d skip_reverb=%d",
                      kAudioMixRate, audio_desired_.samples,
                      (static_cast<double>(audio_desired_.samples) * 1000.0) /
                          static_cast<double>(kAudioMixRate),
                      audio_volume_, audio_ff_volume_,
                      audio_muted_ ? 1 : 0, audio_swap_channels_ ? 1 : 0,
                      psx_->spu->reverb_disabled);

        SDL_LockAudioDevice(audio_dev_);
        audio_underruns_ = 0;
        audio_underrun_bytes_ = 0;
        audio_overflow_resets_ = 0;
        SDL_UnlockAudioDevice(audio_dev_);

        psx_audio_diag_set_enabled(1);
    }

    void audioDiagEnd() {
        psx_audio_diag_set_enabled(0);

        if (!audio_diag_file_) {
            return;
        }

        char tail[256];
        std::snprintf(tail, sizeof(tail), "--- end capture #%d: %d snapshots ---\n",
                      audio_diag_seq_, audio_diag_snapshots_);
        std::fputs(tail, audio_diag_file_);
        std::fflush(audio_diag_file_);
        std::fclose(audio_diag_file_);
        audio_diag_file_ = nullptr;
    }

    // Called once per emulated frame, after the audio for that frame has been mixed and queued
    // so a snapshot describes a whole frame's worth of work.
    void runAudioDiag() {
        if (audio_diag_file_) {
            if ((audio_diag_frames_left_ % kAudioDiagSnapshotFrames) == 0) {
                audioDiagSnapshot();
            }

            if (--audio_diag_frames_left_ <= 0) {
                audioDiagEnd();
            }

            return;
        }

        if (--audio_diag_poll_ > 0) {
            return;
        }

        audio_diag_poll_ = kAudioDiagPollFrames;

        std::string path;
        if (!audioDiagPath(path, "audio_diag")) {
            return;
        }

        FILE* marker = std::fopen(path.c_str(), "rb");
        if (!marker) {
            return;
        }
        std::fclose(marker);

        // Deleted before the capture opens, so a failed fopen() cannot leave the marker armed
        // and a successful one cannot run a second window off one touch.
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(path), ec);

        // The budget is checked AFTER the marker is consumed, and the refusal is written into
        // the output file the user actually retrieves. A spent budget used to be indistinguishable
        // from a marker that was never noticed: the marker just sat there and nothing happened,
        // which reads as "the instrumentation is broken" rather than "relaunch the app".
        if (audio_diag_budget_ <= 0) {
            std::string out;
            if (audioDiagPath(out, "audio_diag.txt")) {
                if (FILE* f = std::fopen(out.c_str(), "a")) {
                    // fputs, not fprintf: frontend/diagnostics.h #defines fprintf into the diag
                    // pipe, which would both fail to compile as std::fprintf and mirror the line
                    // into armsx.log. Same reason psx/dev/gpu.c's dump writes with fputs().
                    char note[192];
                    std::snprintf(note, sizeof(note),
                                  "=== marker seen but the per-process capture budget of %d is "
                                  "spent; relaunch the app to re-arm ===\n", kAudioDiagBudget);
                    std::fputs(note, f);
                    std::fclose(f);
                }
            }

            psxe_diag_logf("audio", "audio_diag: budget spent, relaunch to re-arm");

            return;
        }

        audio_diag_budget_--;
        audioDiagBegin();
    }

    /* Decides how many samples the frame ABOUT TO RUN owes the audio device, and grants the
       SPU exactly that many to produce while it runs.

       This used to be the first thing queueAudioForFrame() did, i.e. after the frame, and the
       SPU then produced all of them in one lump with the CPU stopped. That is what capped the
       SPU interrupt rate at the video frame rate; the full measurement is in the block comment
       above psx_spu_tick() in psx/dev/spu.c. Splitting the decision out and moving it ahead of
       the frame is the whole change — the count per frame is identical, only the instants the
       samples are produced at have moved onto the CPU's timeline.

       Deliberately mirrors queueAudioForFrame()'s old early-outs: no device means no grant,
       so a session without audio leaves the SPU exactly as frozen as it was before. */
    void beginAudioFrame() {
        audio_frame_samples_ = 0;

        if (!psx_ || !audio_dev_) {
            return;
        }

        // The SPU is pulled by the frontend, not free-running: psx_spu_get_sample() advances
        // the voice cursors, the ADSR envelopes and the key-on/key-off latches once per call.
        // So this asks for the same number of samples per EMULATED frame whatever the
        // wall-clock speed is. Fast-forward is dealt with after the mix, by resampling — never
        // by pulling fewer samples, which would stretch every envelope and eat note edges.
        audio_sample_accumulator_ += static_cast<double>(kAudioMixRate) / frameRate();
        const int sample_count = static_cast<int>(audio_sample_accumulator_);
        if (sample_count <= 0) {
            return;
        }

        audio_sample_accumulator_ -= static_cast<double>(sample_count);
        audio_frame_samples_ = sample_count;

        psx_spu_begin_frame(psx_->spu, sample_count);
    }

    void queueAudioForFrame() {
        if (!psx_ || !audio_dev_) {
            return;
        }

        const int sample_count = audio_frame_samples_;
        if (sample_count <= 0) {
            return;
        }

        const size_t byte_count = static_cast<size_t>(sample_count) * kAudioBytesPerSample;
        if (byte_count == 0) {
            return;
        }

        std::vector<uint8_t> frame_audio(byte_count);
        MixPsxAudio(psx_, frame_audio.data(), static_cast<int>(frame_audio.size()));
        applyOutputShaping(frame_audio.data(), static_cast<size_t>(sample_count));

        size_t queued_samples = 0;
        size_t drained_samples = 0;
        if (audioRateConversionActive()) {
            SDL_LockAudioDevice(audio_dev_);
            queued_samples = (audio_queue_.size() - audio_queue_read_offset_) / kAudioBytesPerSample;
            drained_samples = audio_consumed_bytes_ / kAudioBytesPerSample;
            audio_consumed_bytes_ = 0;
            SDL_UnlockAudioDevice(audio_dev_);

            updateAudioRateRatio(sample_count, queued_samples, drained_samples);
            frame_audio = resampleToDeviceRate(frame_audio.data(), static_cast<size_t>(sample_count));
            if (frame_audio.empty()) {
                return;
            }
        }

        SDL_LockAudioDevice(audio_dev_);
        const size_t queue_before = audio_queue_.size() - audio_queue_read_offset_;
        compactAudioQueueLocked();
        if ((audio_queue_.size() - audio_queue_read_offset_) > kMaxQueuedAudioBytes) {
            // The opposite failure to an underrun: audio was produced faster than the device
            // took it and half a second of it is thrown away in one go, which is heard as a
            // jump. `audio_diag` counts it so the two are never confused for each other.
            audio_overflow_resets_++;
            resetAudioQueueLocked();
        }
        audio_queue_.insert(audio_queue_.end(), frame_audio.begin(), frame_audio.end());
        const size_t queue_after = audio_queue_.size() - audio_queue_read_offset_;
        SDL_UnlockAudioDevice(audio_dev_);

        // Only read by the HW_DEBUG trace below.
        (void)queue_before;
        (void)queue_after;
        (void)drained_samples;

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            psxe_diag_logf(
                "audio",
                "frame-audio frame=%llu samples=%d bytes=%zu queue_before=%zu queue_after=%zu accumulator=%.3f frame_rate=%.3f fast_forward=%s rate_ratio=%.3f drained=%zu paused=%s",
                static_cast<unsigned long long>(vblank_counter_),
                sample_count,
                frame_audio.size(),
                queue_before,
                queue_after,
                audio_sample_accumulator_,
                frameRate(),
                fast_forward_enabled_ ? "true" : "false",
                audio_rate_ratio_,
                drained_samples,
                paused_ ? "true" : "false"
            );
        }
#endif

        // After the mix and the queue push, so a snapshot describes a whole frame.
        runAudioDiag();
    }

    // Volume / mute / channel swap, applied to the freshly mixed frame. Deliberately a no-op
    // at the defaults (volume 100, nothing muted, no swap) so normal playback is byte-for-byte
    // what it was before the [audio] table existed.
    void applyOutputShaping(uint8_t* data, size_t sample_count) {
        const int volume = fast_forward_enabled_ ? audio_ff_volume_ : audio_volume_;
        const bool silent = audio_muted_ || volume == 0
            || (fast_forward_enabled_ && audio_mute_fast_forward_);

        if (silent) {
            // Zeroed rather than skipped: MixPsxAudio() has already run, so the SPU state is
            // correct either way, and feeding the device silence beats letting it run dry.
            std::memset(data, 0, sample_count * kAudioBytesPerSample);
            return;
        }

        auto* samples = reinterpret_cast<int16_t*>(data);

        if (audio_swap_channels_) {
            for (size_t index = 0; index < sample_count; index++) {
                std::swap(samples[(index << 1) + 0], samples[(index << 1) + 1]);
            }
        }

        if (volume == 100) {
            return;
        }

        for (size_t index = 0; index < (sample_count << 1); index++) {
            const int scaled = (static_cast<int>(samples[index]) * volume) / 100;
            samples[index] = static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
        }
    }

    // Track how far the emulated stream has to be rate-converted to reach the device.
    //
    // The obvious answer — divide by the requested multiplier — breaks the moment the device
    // cannot actually reach it: a phone managing 1.3x while the user asked for 4x would have
    // its queue starved solid, and "unlimited" has no nominal multiplier to divide by at all.
    // So the ratio comes from what the device REALLY drained since the last frame (the
    // feed-forward term, which is the whole answer in steady state) plus a slow pull toward a
    // target queue depth, which absorbs the mismatch in either direction.
    //
    // Both inputs arrive in device-buffer-sized lumps, so the estimate is smoothed hard before
    // it becomes a ratio: an estimate that tracked the lumps would wobble the pitch audibly.
    void updateAudioRateRatio(int input_samples, size_t queued_samples, size_t drained_samples) {
        if (audio_rate_output_estimate_ <= 0.0) {
            // First frame at this speed: seed from the configured multiplier so the loop only
            // has to trim, instead of ramping from 1x with an audible sweep.
            audio_rate_output_estimate_ =
                static_cast<double>(input_samples) / std::max(expectedSpeedMultiplier(), 0.05);
        }

        const double raw_output = static_cast<double>(drained_samples)
            + (static_cast<double>(kAudioRateTargetQueueSamples) - static_cast<double>(queued_samples))
                  / kAudioRateQueueCorrectionFrames;

        audio_rate_output_estimate_ += kAudioRateSmoothing * (raw_output - audio_rate_output_estimate_);
        audio_rate_output_estimate_ = std::max(audio_rate_output_estimate_, 1.0);
        audio_rate_ratio_ = std::clamp(
            static_cast<double>(input_samples) / audio_rate_output_estimate_, kAudioRateMinRatio, kAudioRateMaxRatio);
    }

    // Convert the frame to the device rate. `audio_rate_ratio_` is input samples per output
    // sample, so above 1 the stream is compressed (fast-forward, limiter off, speed > 100%)
    // and below 1 it is stretched (speed < 100%, or an fps cap under the game's own rate).
    //
    // Compressing uses a box average over the samples being collapsed rather than "keep every
    // Nth": plain decimation of a 44.1 kHz stream aliases badly — the SPU's own pitch
    // conversion leaves plenty of energy near Nyquist — and averaging the samples that would
    // have been thrown away costs two adds each. Stretching holds the last output, which is
    // rough but only ever reached below 100% speed.
    //
    // Neither pitch is preserved: at 2x the audio comes out an octave up, which is what a
    // fast-forward without a time-stretcher sounds like everywhere else. Phase and the partial
    // accumulator survive across frames, so a fractional ratio neither drifts nor clicks at
    // the frame boundary.
    std::vector<uint8_t> resampleToDeviceRate(const uint8_t* data, size_t input_samples) {
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(static_cast<double>(input_samples) / audio_rate_ratio_ + 4.0)
                    * kAudioBytesPerSample);

        auto emit = [&](int16_t left, int16_t right) {
            const uint8_t* left_bytes = reinterpret_cast<const uint8_t*>(&left);
            const uint8_t* right_bytes = reinterpret_cast<const uint8_t*>(&right);
            out.insert(out.end(), left_bytes, left_bytes + sizeof(int16_t));
            out.insert(out.end(), right_bytes, right_bytes + sizeof(int16_t));
        };

        for (size_t index = 0; index < input_samples; index++) {
            audio_rate_accumulator_left_ += samples[(index << 1) + 0];
            audio_rate_accumulator_right_ += samples[(index << 1) + 1];
            audio_rate_accumulator_count_++;
            audio_rate_phase_ += 1.0;

            while (audio_rate_phase_ >= audio_rate_ratio_) {
                audio_rate_phase_ -= audio_rate_ratio_;

                // The inner iterations of this loop (ratio below 1) have no fresh input to
                // average, so they repeat the last output rather than divide by zero.
                if (audio_rate_accumulator_count_ > 0) {
                    audio_rate_last_left_ =
                        static_cast<int16_t>(audio_rate_accumulator_left_ / audio_rate_accumulator_count_);
                    audio_rate_last_right_ =
                        static_cast<int16_t>(audio_rate_accumulator_right_ / audio_rate_accumulator_count_);
                    audio_rate_accumulator_left_ = 0;
                    audio_rate_accumulator_right_ = 0;
                    audio_rate_accumulator_count_ = 0;
                }

                emit(audio_rate_last_left_, audio_rate_last_right_);
            }
        }

        return out;
    }

    void resetAudioRateConverter() {
        audio_rate_ratio_ = 1.0;
        audio_rate_output_estimate_ = 0.0;
        audio_rate_phase_ = 0.0;
        audio_rate_accumulator_left_ = 0;
        audio_rate_accumulator_right_ = 0;
        audio_rate_accumulator_count_ = 0;
        audio_rate_last_left_ = 0;
        audio_rate_last_right_ = 0;
        audio_consumed_bytes_ = 0;
    }

    void consumeQueuedAudio(uint8_t* buffer, size_t size) {
        const size_t available = audio_queue_.size() - audio_queue_read_offset_;
        const size_t to_copy = std::min(size, available);

        // `audio_diag`. THE discriminator: a short read here means the emulation did not
        // produce a frame's worth of audio in time and the device is playing the silence
        // AudioUpdate() memset in. That sounds like a broken mixer to a player, but the fix
        // is speed, not arithmetic. Counted on SDL's audio thread, read by the emulation
        // thread under SDL_LockAudioDevice() — the same discipline audio_consumed_bytes_ uses.
        if (to_copy < size) {
            audio_underruns_++;
            audio_underrun_bytes_ += (size - to_copy);
        }

        if (to_copy > 0) {
            std::memcpy(buffer, audio_queue_.data() + audio_queue_read_offset_, to_copy);
            audio_queue_read_offset_ += to_copy;
            // Feeds the fast-forward rate estimate above. Only ever touched from here (inside
            // SDL's audio lock) and from the emulation thread under SDL_LockAudioDevice().
            audio_consumed_bytes_ += to_copy;
        }

        if (audio_queue_read_offset_ >= audio_queue_.size()) {
            resetAudioQueueLocked();
        } else if (audio_queue_read_offset_ >= kAudioQueueCompactThreshold) {
            compactAudioQueueLocked();
        }
    }

    void clearQueuedAudio() {
        if (!audio_dev_) {
            resetAudioQueueLocked();
            return;
        }

        SDL_LockAudioDevice(audio_dev_);
        resetAudioQueueLocked();
        SDL_UnlockAudioDevice(audio_dev_);
    }

    void resetAudioQueueLocked() {
        audio_sample_accumulator_ = 0.0;
        audio_queue_.clear();
        audio_queue_read_offset_ = 0;
    }

    void compactAudioQueueLocked() {
        if (audio_queue_read_offset_ == 0) {
            return;
        }

        if (audio_queue_read_offset_ >= audio_queue_.size()) {
            resetAudioQueueLocked();
            return;
        }

        audio_queue_.erase(audio_queue_.begin(), audio_queue_.begin() + static_cast<std::ptrdiff_t>(audio_queue_read_offset_));
        audio_queue_read_offset_ = 0;
    }

    static constexpr int kAudioMixRate = 44100;
    // `audio_diag`: ~10 s of capture, sampled five times a second, four arms per process.
    static constexpr int kAudioDiagFrames = 1800;
    static constexpr int kAudioDiagSnapshotFrames = 30;
    static constexpr int kAudioDiagPollFrames = 30;
    static constexpr int kAudioDiagBudget = 16;
    static constexpr size_t kAudioBytesPerSample = sizeof(int16_t) * 2;
    static constexpr size_t kMaxQueuedAudioBytes = static_cast<size_t>(kAudioMixRate * sizeof(int16_t) * 2 / 2);
    static constexpr size_t kAudioQueueCompactThreshold = 4096;
    static constexpr std::uint32_t kMaxFrameSteps = PSX_CPU_CPS / 8u;
    // Queue depth the rate converter aims for: ~46 ms, long enough that the device never runs
    // dry between two emulated frames, short enough to stay responsive.
    static constexpr int kAudioRateTargetQueueSamples = 2048;
    // Frames the queue-depth correction is spread over, and how hard the whole estimate is
    // smoothed. Both slow on purpose — see updateAudioRateRatio().
    static constexpr double kAudioRateQueueCorrectionFrames = 16.0;
    static constexpr double kAudioRateSmoothing = 0.05;
    // Ratio bounds. The low end matches the 10% floor on speed_percent; the high end is a
    // sanity rail, not a real limit (32x of a 44.1 kHz stream is already inaudible mush).
    static constexpr double kAudioRateMinRatio = 0.1;
    static constexpr double kAudioRateMaxRatio = 32.0;
    // Stand-in for "no cap" in targetFrameRate(). Far above anything a PS1 core reaches, so
    // waitForFrameDeadline() always finds the deadline already passed and never sleeps.
    static constexpr double kUncappedFrameRate = 100000.0;

    // Presentation goes through the backend abstraction (frontend/render.h); the session no
    // longer owns an SDL_Texture of its own.
    armsx_renderer_t* render_ = nullptr;
    psx_t* psx_ = nullptr;
    psx_input_t* input_ = nullptr;
    // What is actually in controller port 1, and the boot analog mode the live Multitap
    // toggle has to rebuild it with (settings.analog_mode_default is not in scope there).
    bool multitap_attached_ = false;
    bool analog_mode_default_ = false;
    // A look-ahead ran last frame and its snapshot is still waiting to be put back. See
    // runaheadRestore() for why the restore cannot happen at the end of the frame that took it.
    bool runahead_restore_pending_ = false;
    bool frame_uploaded_ = false;
    std::vector<uint8_t> texture_snapshot_;
#ifdef USE_HARDWARE
    armsx_hw_renderer_t* hw_renderer_ = nullptr;
    // Internal-resolution rasterizer backend; null means the software path.
    psx_gpu_backend_t* hw_rt_backend_ = nullptr;
    bool hw_rt_is_gl_ = false;
    // Remembered from the settings so checkRasterizerHealth() can rebuild a fallback
    // rasterizer without a FrontendSettings in hand.
    int rasterizer_mode_ = 0;
    int internal_scale_ = 1;
#endif
    SDL_AudioDeviceID audio_dev_ = 0;
    std::vector<uint8_t> audio_queue_;
    size_t audio_queue_read_offset_ = 0;
    double audio_sample_accumulator_ = 0.0;
    /* Samples beginAudioFrame() committed to for the frame in flight, and therefore the
       budget psx_spu_tick() was granted. Read back by queueAudioForFrame() so the two cannot
       disagree about how long the frame was. */
    int audio_frame_samples_ = 0;
    // Bytes the device has taken since the emulation thread last looked; drives the
    // fast-forward rate estimate. Written under SDL's audio lock on both sides.
    size_t audio_consumed_bytes_ = 0;
    // `audio_diag` only. Same locking discipline as audio_consumed_bytes_.
    uint32_t audio_underruns_ = 0;
    uint64_t audio_underrun_bytes_ = 0;
    uint32_t audio_overflow_resets_ = 0;
    // One-shot capture state; see runAudioDiag(). All emulation thread.
    FILE* audio_diag_file_ = nullptr;
    int audio_diag_frames_left_ = 0;
    int audio_diag_poll_ = 0;
    int audio_diag_budget_ = kAudioDiagBudget;
    int audio_diag_seq_ = 0;
    int audio_diag_snapshots_ = 0;
    // [audio], mirrored here so the audio path never reaches back into FrontendSettings.
    int audio_volume_ = 100;
    int audio_ff_volume_ = 100;
    bool audio_muted_ = false;
    bool audio_mute_fast_forward_ = false;
    bool audio_swap_channels_ = false;
    // Rate converter state (see updateAudioRateRatio / resampleToDeviceRate).
    double audio_rate_ratio_ = 1.0;
    double audio_rate_output_estimate_ = 0.0;
    double audio_rate_phase_ = 0.0;
    int32_t audio_rate_accumulator_left_ = 0;
    int32_t audio_rate_accumulator_right_ = 0;
    int audio_rate_accumulator_count_ = 0;
    int16_t audio_rate_last_left_ = 0;
    int16_t audio_rate_last_right_ = 0;
    std::filesystem::path disc_path_;
    std::filesystem::path exe_path_;
    std::string title_;
    LaunchKind launch_kind_ = LaunchKind::None;
    bool paused_ = false;
    // App is off-screen (backgrounded / screen off). Independent of paused_: the pause menu
    // freezes the VM but keeps the device open, this closes it outright.
    bool audio_suspended_ = false;
    // The spec the device was opened with, kept so it can be reopened identically on the way
    // back from the background.
    SDL_AudioSpec audio_desired_{};
    bool audio_device_ever_opened_ = false; // gate for ensureAudioDevice()'s retry
    bool audio_reopen_failed_ = false;      // one log line per failure run, not one per retry
    Uint64 audio_reopen_next_attempt_ = 0;  // SDL_GetTicks64() throttle for the retry
    bool fast_forward_enabled_ = false;
    // [runtime] frame pacing; see setSpeedLimits(). fast_forward_speed_ 0 = uncapped.
    bool frame_limit_ = true;
    int speed_percent_ = 100;
    int fps_limit_ = 0;
    double fast_forward_speed_ = 2.0;
    // [runtime] frame_skip. Held here only because the host pushes it with the pacing policy;
    // the session itself never acts on it (the app loop owns presentation).
    int frame_skip_ = 0;
    bool debug_view_ = false;
    /* Aspect/stretch trace: the first few frames, then only when the mode actually changes,
       so logcat proves which display mode is in force without spamming per frame. */
    int aspect_traces_ = 0;
    int last_logged_aspect_mode_ = -1;
    bool last_logged_stretch_ = false;
#ifdef USE_HARDWARE
    bool hardware_backend_active_ = false;
#endif
    std::uint64_t vblank_counter_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    // Pre-multiplier display size, kept so the degenerate "display width reported 0"
    // fallback cannot compound the internal-resolution scale frame after frame.
    /* [video] deinterlace scratch. Same pitch and height as the frame it replaces; kept
       across frames so a steady 480-line game allocates once. */
    std::vector<uint8_t> deint_buffer_;
    int texture_native_width_ = 0;
    int texture_native_height_ = 0;
    Uint32 texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
};

class GameplayInputRouter {
  public:
    ~GameplayInputRouter() {
        if (controller_) {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
    }

    void attach(psx_pad_t* pad) {
        clearAll();
        pad_ = pad;
    }

    void detach() {
        clearAll();
        pad_ = nullptr;
    }

    void onFsuiOpened() {
        fsui_owns_input_ = true;
        clearAll();
    }

    void onFsuiClosed() {
        fsui_owns_input_ = false;
        clearAll();
    }

    bool takePauseRequest() {
        const bool requested = pause_requested_;
        pause_requested_ = false;
        return requested;
    }

    void tick(bool fsui_active) {
        if (!fsui_active) {
            flushChordButtons(SDL_GetTicks());
        }
    }


    void processEvent(const SDL_Event& event, ArmsxSession* session, bool fsui_active) {
        flushChordButtons(SDL_GetTicks());
        handleControllerLifecycle(event);

        if (!session || !session->valid()) {
            return;
        }

        if (event.type == SDL_KEYDOWN) {
            if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE && !fsui_active) {
                pause_requested_ = true;
                return;
            }

            if (fsui_active || fsui_owns_input_ || event.key.repeat != 0) {
                return;
            }

            const uint32_t mask = buttonForKey(event.key.keysym.sym);
            if (mask) {
                press(mask);
            }
            return;
        }

        if (event.type == SDL_KEYUP) {
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                return;
            }

            if (fsui_active || fsui_owns_input_) {
                return;
            }

            const uint32_t mask = buttonForKey(event.key.keysym.sym);
            if (mask) {
                release(mask);
            }
            return;
        }

        if (!controller_) {
            return;
        }

        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller_);
        if (!joystick) {
            return;
        }

        const SDL_JoystickID controller_id = SDL_JoystickInstanceID(joystick);

        if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
            if (event.cbutton.which != controller_id) {
                return;
            }

            const bool pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
            const SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(event.cbutton.button);

            if (button == SDL_CONTROLLER_BUTTON_START || button == SDL_CONTROLLER_BUTTON_BACK ||
                button == SDL_CONTROLLER_BUTTON_GUIDE || button == SDL_CONTROLLER_BUTTON_MISC1) {
                handlePauseChordButton(button, pressed, fsui_active);
                return;
            }

            if (fsui_active || fsui_owns_input_) {
                return;
            }

            const uint32_t mask = buttonForController(button);
            if (mask) {
                if (pressed) {
                    press(mask);
                } else {
                    release(mask);
                }
            }
            return;
        }

        if (event.type == SDL_CONTROLLERAXISMOTION) {
            if (event.caxis.which != controller_id || fsui_active || fsui_owns_input_ || !pad_) {
                return;
            }

            const uint16_t mapped = static_cast<uint16_t>((static_cast<int>(event.caxis.value) + INT16_MAX + 1) / 0x100);
            switch (event.caxis.axis) {
                case SDL_CONTROLLER_AXIS_RIGHTX:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_HORZ, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_RIGHTY:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_VERT, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTX:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_HORZ, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_VERT, mapped);
                    break;
#ifdef CONTROLLER_GENERIC
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                    handleTrigger(PSXI_SW_SDA_L2, trigger_left_down_, event.caxis.value);
                    break;
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                    handleTrigger(PSXI_SW_SDA_R2, trigger_right_down_, event.caxis.value);
                    break;
#endif
                default:
                    break;
            }
        }
    }

  private:
    void handleControllerLifecycle(const SDL_Event& event) {
        if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!controller_ && SDL_IsGameController(event.cdevice.which)) {
                controller_ = SDL_GameControllerOpen(event.cdevice.which);
            }
            return;
        }

        if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller_) {
            SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller_);
            if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which) {
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
                start_button_ = {};
                select_button_ = {};
                trigger_left_down_ = false;
                trigger_right_down_ = false;
            }
        }
    }

    void handlePauseChordButton(SDL_GameControllerButton button, bool pressed, bool fsui_active) {
        PendingChordButton* state = nullptr;
        PendingChordButton* other = nullptr;

        if (button == SDL_CONTROLLER_BUTTON_START) {
            state = &start_button_;
            other = &select_button_;
        } else {
            state = &select_button_;
            other = &start_button_;
        }

        if (pressed) {
            state->physical_down = true;
            if (!fsui_active && !fsui_owns_input_ && !pause_latched_) {
                state->pending = true;
                state->pending_since = SDL_GetTicks();
                if (other->physical_down && other->pending) {
                    pause_latched_ = true;
                    pause_requested_ = true;
                    state->pending = false;
                    other->pending = false;
                    clearAll();
                }
            }
            return;
        }

        state->physical_down = false;

        if (pause_latched_) {
            if (!start_button_.physical_down && !select_button_.physical_down) {
                pause_latched_ = false;
                start_button_ = {};
                select_button_ = {};
            }
            return;
        }

        if (state->pending) {
            const uint32_t mask = (state == &start_button_) ? PSXI_SW_SDA_START : PSXI_SW_SDA_SELECT;
            press(mask);
            release(mask);
            state->pending = false;
            return;
        }

        if (state->forwarded) {
            const uint32_t mask = (state == &start_button_) ? PSXI_SW_SDA_START : PSXI_SW_SDA_SELECT;
            release(mask);
            state->forwarded = false;
        }
    }

    void flushChordButtons(Uint32 now) {
        if (pause_latched_ || fsui_owns_input_) {
            return;
        }

        flushOneChordButton(start_button_, select_button_, PSXI_SW_SDA_START, now);
        flushOneChordButton(select_button_, start_button_, PSXI_SW_SDA_SELECT, now);
    }

    void flushOneChordButton(PendingChordButton& state, PendingChordButton& other, uint32_t mask, Uint32 now) {
        if (!state.pending || other.physical_down || !pad_) {
            return;
        }

        if ((now - state.pending_since) < kPauseChordGraceMs) {
            return;
        }

        press(mask);
        state.pending = false;
        state.forwarded = true;
    }

    void press(uint32_t mask) {
        if (!pad_ || !mask) {
            return;
        }

        if ((active_digital_mask_ & mask) == 0) {
            psx_pad_button_press(pad_, 0, mask);
            active_digital_mask_ |= mask;
        }
    }

    void release(uint32_t mask) {
        if (!pad_ || !mask) {
            return;
        }

        if ((active_digital_mask_ & mask) != 0) {
            psx_pad_button_release(pad_, 0, mask);
            active_digital_mask_ &= ~mask;
        }
    }

    void clearAll() {
        if (!pad_) {
            active_digital_mask_ = 0;
            return;
        }

        static const std::array<uint32_t, 17> masks = {
            PSXI_SW_SDA_SELECT, PSXI_SW_SDA_L3, PSXI_SW_SDA_R3, PSXI_SW_SDA_START,
            PSXI_SW_SDA_PAD_UP, PSXI_SW_SDA_PAD_RIGHT, PSXI_SW_SDA_PAD_DOWN, PSXI_SW_SDA_PAD_LEFT,
            PSXI_SW_SDA_L2, PSXI_SW_SDA_R2, PSXI_SW_SDA_L1, PSXI_SW_SDA_R1,
            PSXI_SW_SDA_TRIANGLE, PSXI_SW_SDA_CIRCLE, PSXI_SW_SDA_CROSS, PSXI_SW_SDA_SQUARE,
            PSXI_SW_SDA_ANALOG,
        };

        for (uint32_t mask : masks) {
            if ((active_digital_mask_ & mask) != 0) {
                psx_pad_button_release(pad_, 0, mask);
            }
        }

        active_digital_mask_ = 0;
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_HORZ, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_VERT, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_HORZ, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_VERT, 0x80);
    }

#ifdef CONTROLLER_GENERIC
    void handleTrigger(uint32_t mask, bool& down_flag, Sint16 value) {
        const bool pressed = value > 8000;
        if (pressed == down_flag) {
            return;
        }

        down_flag = pressed;
        if (pressed) {
            press(mask);
        } else {
            release(mask);
        }
    }
#endif

    static uint32_t buttonForKey(SDL_Keycode key) {
        switch (key) {
            case SDLK_x: return PSXI_SW_SDA_CROSS;
            case SDLK_a: return PSXI_SW_SDA_SQUARE;
            case SDLK_w: return PSXI_SW_SDA_TRIANGLE;
            case SDLK_d: return PSXI_SW_SDA_CIRCLE;
            case SDLK_RETURN: return PSXI_SW_SDA_START;
            case SDLK_s: return PSXI_SW_SDA_SELECT;
            case SDLK_UP: return PSXI_SW_SDA_PAD_UP;
            case SDLK_DOWN: return PSXI_SW_SDA_PAD_DOWN;
            case SDLK_LEFT: return PSXI_SW_SDA_PAD_LEFT;
            case SDLK_RIGHT: return PSXI_SW_SDA_PAD_RIGHT;
            case SDLK_q: return PSXI_SW_SDA_L1;
            case SDLK_e: return PSXI_SW_SDA_R1;
            case SDLK_1: return PSXI_SW_SDA_L2;
            case SDLK_3: return PSXI_SW_SDA_R2;
            case SDLK_z: return PSXI_SW_SDA_L3;
            case SDLK_c: return PSXI_SW_SDA_R3;
            case SDLK_2: return PSXI_SW_SDA_ANALOG;
            default: return 0;
        }
    }

    static uint32_t buttonForController(SDL_GameControllerButton button) {
        switch (button) {
            case SDL_CONTROLLER_BUTTON_A: return PSXI_SW_SDA_CROSS;
            case SDL_CONTROLLER_BUTTON_X: return PSXI_SW_SDA_SQUARE;
            case SDL_CONTROLLER_BUTTON_Y: return PSXI_SW_SDA_TRIANGLE;
            case SDL_CONTROLLER_BUTTON_B: return PSXI_SW_SDA_CIRCLE;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return PSXI_SW_SDA_PAD_UP;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PSXI_SW_SDA_PAD_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PSXI_SW_SDA_PAD_LEFT;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PSXI_SW_SDA_PAD_RIGHT;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return PSXI_SW_SDA_L1;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PSXI_SW_SDA_R1;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: return PSXI_SW_SDA_L3;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return PSXI_SW_SDA_R3;
            default: return 0;
        }
    }

    psx_pad_t* pad_ = nullptr;
    SDL_GameController* controller_ = nullptr;
    uint32_t active_digital_mask_ = 0;
    PendingChordButton start_button_{};
    PendingChordButton select_button_{};
    bool pause_requested_ = false;
    bool pause_latched_ = false;
    bool fsui_owns_input_ = false;
    bool trigger_left_down_ = false;
    bool trigger_right_down_ = false;
};

class ArmsxApp {
  public:
    ArmsxApp(int argc, const char* argv[], void* external_window, void* external_renderer)
        : argc_(argc), argv_(argv), external_window_(static_cast<SDL_Window*>(external_window)),
          external_renderer_(static_cast<SDL_Renderer*>(external_renderer)), cli_(ScanCliFlags(argc, argv)) {}

    int run() {
        ARMSX_BOOTLOG("core: run() enter argc=%d external_window=%p external_renderer=%p",
                      argc_, static_cast<void*>(external_window_), static_cast<void*>(external_renderer_));
        for (int index = 0; index < argc_; ++index) {
            ARMSX_BOOTLOG("core: argv[%d]=%s", index, argv_ && argv_[index] ? argv_[index] : "(null)");
        }

        psxe_diag_initialize(psxe_cfg_get_pref_path());
        psxe_diag_breadcrumbf("ARMSX startup argc=%d", argc_);
        psxe_diag_logf("diag", "Pref path: %s", psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "(none)");

        // RetroAchievements. After the pref path is resolved (its state file lives there) and
        // before anything can boot, so a saved login is already restoring when a game arrives.
        armsx_ach_startup();

        static bool log_callback_installed = false;
        if (!log_callback_installed) {
            log_add_callback(StructuredLogCallback, nullptr, LOG_TRACE);
            SDL_LogSetOutputFunction(SdlLogOutput, nullptr);
            log_callback_installed = true;
        }

        psxe_config_t* cfg = psxe_cfg_create();
        if (!cfg) {
            ARMSX_BOOTERR("core: psxe_cfg_create() failed");
            return 1;
        }

        psxe_cfg_init(cfg);
        psxe_cfg_load_defaults(cfg);
        psxe_cfg_load(cfg, argc_, const_cast<const char**>(argv_));
        settings_ = BuildSettings(cfg, cli_);
        psxe_cfg_destroy(cfg);

        applyLoggingSettings("startup");
#if defined(USE_HARDWARE)
        if (const char* env_backend = std::getenv("ARMSX_GPU_BACKEND")) {
            psxe_diag_breadcrumbf(
                "GPU backend env override=%s resolved=%s",
                env_backend,
                GpuBackendTitle(settings_.gpu_backend)
            );
            psxe_diag_logf(
                "diag",
                "GPU backend env override=%s resolved=%s",
                env_backend,
                GpuBackendTitle(settings_.gpu_backend)
            );
        }
#endif
        psxe_diag_breadcrumbf("Settings loaded model=%s region=%s logging_enabled=%s log_level=%d",
            settings_.model.c_str(),
            settings_.region.c_str(),
            settings_.logging_enabled ? "true" : "false",
            settings_.log_level);

        ARMSX_BOOTLOG("core: settings loaded model=%s region=%s bios_override=%s bios_search=%s",
                      settings_.model.c_str(), settings_.region.c_str(),
                      settings_.bios_override.empty() ? "(none)" : settings_.bios_override.c_str(),
                      settings_.bios_search.empty() ? "(none)" : settings_.bios_search.c_str());

        installCrashHandlers();

        if (!initializeSdl()) {
            ARMSX_BOOTERR("core: initializeSdl() failed: %s", SDL_GetError());
            shutdown();
            return 1;
        }
        ARMSX_BOOTLOG("core: initializeSdl() ok");

        if (!initializeWindowAndRenderer()) {
            ARMSX_BOOTERR("core: initializeWindowAndRenderer() failed: %s", SDL_GetError());
            shutdown();
            return 1;
        }
        ARMSX_BOOTLOG("core: initializeWindowAndRenderer() ok window=%p renderer=%p backend=%s",
                      static_cast<void*>(window_), static_cast<void*>(render_),
                      armsx_render_backend_name(armsx_renderer_backend(render_)));

        if (!initializeFsui()) {
            ARMSX_BOOTERR("core: initializeFsui() failed");
            shutdown();
            return 1;
        }

        logRendererBootstrap("frontend-init", kUiFrameRate);
        refreshGameList(true);

        g_active_app = this;
        initializeWebLaunchSupport();
        consumePendingLaunchArguments();

        ARMSX_BOOTLOG("core: boot request=%s cli_launch=%s cli_argument=%s",
                      cli_.has_boot_request ? "true" : "false",
                      pending_cli_launch_.has_value() ? "present" : "absent",
                      pending_cli_argument_.empty() ? "(none)" : pending_cli_argument_.c_str());

        if (cli_.has_boot_request) {
            if (pending_cli_launch_.has_value()) {
                const bool launched = launchSession(*pending_cli_launch_, false);
                ARMSX_BOOTLOG("core: cli launchSession -> %s%s%s",
                              launched ? "ok" : "FAILED",
                              launched ? "" : " error=",
                              launched ? "" : (pending_error_dialog_.has_value() ? pending_error_dialog_->c_str() : "(none)"));
            } else if (!pending_cli_argument_.empty()) {
                pending_error_dialog_ = "Unsupported launch path or URI.";
                ARMSX_BOOTERR("core: unsupported launch path or URI: %s", pending_cli_argument_.c_str());
            }
        }

        consumePendingLaunchArguments();

        // FSUI removed: a plain SDL event pump + frame loop replaces fsui::RunSdlMainLoop, and there
        // is no in-app landing menu — the game to boot arrives via CLI args or the JNI launch queue
        // (Jetpack Compose owns all menus). With nothing queued this simply shows a black window.
        ARMSX_BOOTLOG("core: entering main loop running_=%s session_valid=%s",
                      running_ ? "true" : "false", session_.valid() ? "true" : "false");
        g_host_loop_running.store(true, std::memory_order_release);
        uint64_t loop_frames = 0;
        while (running_) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handleEvent(event);
            }
            runFrame();
            ++loop_frames;
            // Proof-of-life only: the first few frames confirm the loop actually spun up, then
            // roughly once a minute so a wedged loop is still visible in a bug report without
            // turning logcat into a firehose.
            if (loop_frames <= 3 || (loop_frames % 3600) == 0) {
                ARMSX_BOOTLOG("core: loop frame=%llu session_valid=%s",
                              static_cast<unsigned long long>(loop_frames),
                              session_.valid() ? "true" : "false");
            }
        }
        ARMSX_BOOTLOG("core: main loop exited after %llu frames", static_cast<unsigned long long>(loop_frames));
        g_host_loop_running.store(false, std::memory_order_release);
        g_host_vm_active.store(false, std::memory_order_release);
        // Before shutdown(): this waits briefly for an unlock submitted on the last frame to
        // reach the server rather than dropping it with the process.
        armsx_ach_shutdown();
        shutdown();
        g_active_app = nullptr;

        return 0;
    }

    void writeCrashContext(const char* reason) const {
        const ArmsxGameInfo info = session_.currentGameInfo();
        psxe_diag_logf("crash", "Crash reason: %s", reason ? reason : "(unknown)");
        psxe_diag_logf("crash", "Running game: %s", info.has_game ? info.title.c_str() : "(none)");
        psxe_diag_logf("crash", "Game path: %s", info.path.empty() ? "(none)" : info.path.string().c_str());
        psxe_diag_logf("crash", "BIOS override: %s", settings_.bios_override.empty() ? "(none)" : settings_.bios_override.c_str());
        psxe_diag_logf("crash", "BIOS folder: %s", settings_.bios_search.c_str());
        psxe_diag_logf("crash", "Model=%s Region=%s Expansion=%s",
            settings_.model.c_str(),
            settings_.region.c_str(),
            settings_.exp_path.empty() ? "(none)" : settings_.exp_path.c_str());
        logRendererBootstrap("crash", currentTargetFrameRate());
        logCpuState();
        psxe_diag_dump_breadcrumbs();
    }

  private:
    void initializeWebLaunchSupport() {
#if defined(__EMSCRIPTEN__)
        static bool initialized = false;
        if (initialized) {
            return;
        }

        initialized = true;
        EM_ASM({
            try {
                const searchParams = new URLSearchParams(window.location.search);
                let launchUri = searchParams.get('uri') || '';
                if (!launchUri && window.location.hash) {
                    const hash = window.location.hash.startsWith('#') ? window.location.hash.substring(1) : window.location.hash;
                    launchUri = new URLSearchParams(hash).get('uri') || '';
                }

                if (launchUri.startsWith('web+armsx:')) {
                    launchUri = 'armsx:' + launchUri.substring('web+armsx:'.length);
                }

                if (window.location.protocol === 'https:' && typeof navigator !== 'undefined' &&
                    typeof navigator.registerProtocolHandler === 'function') {
                    try {
                        navigator.registerProtocolHandler(
                            'web+armsx',
                            window.location.origin + window.location.pathname + '?uri=%s',
                            'ARMSX'
                        );
                    } catch (error) {
                        console.warn('ARMSX protocol handler registration skipped', error);
                    }
                }

                if (launchUri && typeof Module !== 'undefined' && typeof Module.ccall === 'function') {
                    Module.ccall('psxe_enqueue_launch_argument', null, ['string'], [launchUri]);
                }
            } catch (error) {
                console.warn('ARMSX web launch bootstrap failed', error);
            }
        });
#endif
    }

    void requestWebFiles(bool directory) {
#if defined(__EMSCRIPTEN__)
        if (directory) {
            EM_ASM({ window.ARMSXWebFiles.openDirectory(); });
        } else {
            EM_ASM({ window.ARMSXWebFiles.openFiles(); });
        }
#else
        (void)directory;
#endif
    }

    void queueLaunchArgument(std::string_view argument, bool close_ui, const char* error_message = "Unsupported launch path or URI.") {
        const LaunchRequest request = LaunchForArgument(argument);
        psxe_diag_logf("launch", "Launch argument=%s kind=%s", std::string(argument).c_str(), LaunchKindTitle(request.kind));
        if (request.kind == LaunchKind::None) {
            pending_error_dialog_ = error_message;
            if (!session_.valid()) {
                showLandingWindow();
            }
            return;
        }

        queueLaunchRequest(request, close_ui);
    }

    void consumePendingLaunchArguments() {
        for (const std::string& message : DrainWebErrors()) {
            pending_error_dialog_ = message;
            if (!session_.valid()) {
                showLandingWindow();
            }
        }
        for (const std::string& argument : DrainPendingLaunchArguments()) {
            queueLaunchArgument(argument, true);
        }
    }

    void queueLaunchRequest(const LaunchRequest& request, bool close_ui) {
        if (request.kind == LaunchKind::None) {
            return;
        }

        deferred_launch_ = request;
        close_ui_after_launch_ = close_ui;
    }

    void queueLaunchSelection(const std::string& path, std::optional<LaunchKind> forced_kind = std::nullopt) {
        const std::optional<std::string> selection = NormalizedPickerSelection(path);
        if (!selection.has_value()) {
            return;
        }

        const LaunchRequest request = LaunchForArgument(*selection, forced_kind);
        if (request.kind != LaunchKind::None) {
            queueLaunchRequest(request, true);
        }
    }

    void queueDiscSwapSelection(const std::string& path) {
        const std::optional<std::string> selection = NormalizedPickerSelection(path);
        if (!selection.has_value()) {
            return;
        }

        deferred_change_disc_ = std::filesystem::path(*selection);
    }

    SDL_Renderer* sdlRenderer() const {
        return armsx_renderer_sdl(render_);
    }

#ifdef USE_HARDWARE
    bool hardwareRendererAvailable() const {
        if (!owns_renderer_ || external_renderer_ || !render_) {
            return false;
        }
        return SupportsHardwareGpuBackend();
    }
#endif

    // Backend selection with a strict fallback ladder, so a device that cannot bring up the
    // requested GPU backend still boots on the historical SDL path instead of failing.
    //   opengl -> sdl-accelerated -> software
    //   vulkan -> opengl -> sdl-accelerated -> software
    bool createManagedRenderer(bool vsync_enabled) {
        armsx_render_config_t config{};
        config.vsync = vsync_enabled;
        config.linear_filter = settings_.texture_scale_mode;

        std::vector<armsx_render_backend_t> ladder;
#ifdef USE_HARDWARE
        const armsx_render_backend_t requested = RenderBackendFor(settings_.gpu_backend);
#else
        const armsx_render_backend_t requested = ARMSX_RENDER_BACKEND_SDL_SOFTWARE;
#endif
        ladder.push_back(requested);
        if (requested == ARMSX_RENDER_BACKEND_VULKAN) {
            ladder.push_back(ARMSX_RENDER_BACKEND_OPENGL);
        }
        if (requested == ARMSX_RENDER_BACKEND_VULKAN || requested == ARMSX_RENDER_BACKEND_OPENGL) {
            ladder.push_back(ARMSX_RENDER_BACKEND_SDL_ACCELERATED);
        }
        if (requested != ARMSX_RENDER_BACKEND_SDL_SOFTWARE) {
            ladder.push_back(ARMSX_RENDER_BACKEND_SDL_SOFTWARE);
        }

        for (const armsx_render_backend_t backend : ladder) {
            if (!armsx_render_backend_compiled_in(backend)) {
                continue;
            }

            // A GL/Vulkan backend needs matching window flags; a window we did not create
            // with them can only host the SDL backends. The EGL provider is exempt because
            // it binds to the host's ANativeWindow, not to the SDL window.
            const Uint32 needed = armsx_render_window_flags(backend);
            if (needed && window_ && (SDL_GetWindowFlags(window_) & needed) != needed &&
                !armsx_render_native_window()) {
                psxe_diag_logf("renderer", "Skipping %s: window lacks the required SDL flags.",
                               armsx_render_backend_name(backend));
                continue;
            }

            render_ = armsx_renderer_create(backend, window_, &config);
            if (render_) {
                break;
            }

            psxe_diag_logf("renderer", "Backend %s unavailable (%s); falling back.",
                           armsx_render_backend_name(backend), SDL_GetError());
        }

        owns_renderer_ = render_ != nullptr;

        if (!render_) {
            psxe_diag_logf(
                "renderer",
                "Renderer initialization failed requested_vsync=%s error=%s",
                vsync_enabled ? "true" : "false",
                SDL_GetError()
            );
            return false;
        }

        psxe_diag_logf(
            "renderer",
            "Selected presentation backend=%s driver=%s accelerated=%s",
            armsx_render_backend_name(armsx_renderer_backend(render_)),
            armsx_renderer_driver_name(render_),
            armsx_renderer_is_accelerated(render_) ? "true" : "false"
        );

        managed_renderer_vsync_ = vsync_enabled;
        return true;
    }

    void shutdownFsuiFrontend() {
    }


    void showLandingWindow() {
    }

    void showStartGameWindow() {
    }

    void showExitWindow() {
    }

    void showGameListWindow() {
    }

    void switchToSettingsWindow() {
    }

    void returnToMainWindow() {
    }

    void showPauseMenuWindow() {
    }

    void restoreUiWindow() {
    }

    bool recreateManagedRenderer(bool desired_vsync) {
        if (!owns_renderer_ || external_renderer_ || !window_) {
            return false;
        }

        const bool had_session = session_.valid();
        const bool was_paused = had_session ? session_.paused() : false;
        const FsuiWindowState restore_window = ui_window_state_;
        const bool previous_vsync = managed_renderer_vsync_;
        const bool should_be_paused_after_restore = had_session && (was_paused || restore_window != FsuiWindowState::None);

        auto restore_previous_renderer = [&](const char* phase, const char* message) {
            settings_.vsync_enabled = previous_vsync;
            SaveSettings(settings_);

            if (render_) {
                armsx_renderer_destroy(render_);
                render_ = nullptr;
            }
            owns_renderer_ = false;

            if (!createManagedRenderer(previous_vsync) || !initializeFsui()) {
                psxe_diag_logf("renderer", "Renderer recovery failed phase=%s error=%s", phase ? phase : "(none)", SDL_GetError());
                running_ = false;
                return false;
            }

            if (had_session) {
                session_.rebindRenderer(render_, settings_);
                session_.setPaused(should_be_paused_after_restore);
            }

            ui_window_state_ = restore_window;
            restoreUiWindow();
            resetFramePacing("renderer-recovery");
            logRendererBootstrap(phase, currentTargetFrameRate());
            pending_error_dialog_ = message;
            return false;
        };

        if (had_session) {
            session_.setPaused(true);
        }

        shutdownFsuiFrontend();

        if (render_) {
            armsx_renderer_destroy(render_);
            render_ = nullptr;
        }
        owns_renderer_ = false;

        if (!createManagedRenderer(desired_vsync)) {
            return restore_previous_renderer("renderer-recreate-recover", "Failed to apply the requested VSync mode; restored the previous renderer.");
        }

        if (!initializeFsui()) {
            return restore_previous_renderer("renderer-recreate-recover", "Failed to rebuild the UI after changing VSync; restored the previous renderer.");
        }

        if (had_session) {
            session_.rebindRenderer(render_, settings_);
            session_.setPaused(should_be_paused_after_restore);
        }

        ui_window_state_ = restore_window;
        restoreUiWindow();
        resetFramePacing("renderer-recreate");
        logRendererBootstrap("renderer-recreate", currentTargetFrameRate());
        return true;
    }

    void installCrashHandlers() {
        static bool installed = false;
        if (installed) {
            return;
        }

        installed = true;

        std::set_terminate([]() {
            ReportNativeCrash("std::terminate");
        });

        auto signal_handler = [](int signal_value) {
            switch (signal_value) {
                case SIGABRT:
                    ReportNativeCrash("SIGABRT");
                    break;
                case SIGSEGV:
                    ReportNativeCrash("SIGSEGV");
                    break;
                case SIGILL:
                    ReportNativeCrash("SIGILL");
                    break;
                case SIGFPE:
                    ReportNativeCrash("SIGFPE");
                    break;
                default:
                    ReportNativeCrash("signal");
                    break;
            }
        };

        std::signal(SIGABRT, signal_handler);
        std::signal(SIGSEGV, signal_handler);
        std::signal(SIGILL, signal_handler);
        std::signal(SIGFPE, signal_handler);

#if defined(_WIN32) && !defined(UWP_TARGET)
        SetUnhandledExceptionFilter(&WindowsUnhandledExceptionFilter);
#endif
    }

    double currentTargetFrameRate() const {
        if (session_.valid() && !session_.paused()) {
            return session_.targetFrameRate();
        }

        return kUiFrameRate;
    }

    void updateFramePeriod(double frame_rate) {
#if !defined(__EMSCRIPTEN__)
        const double safe_rate = std::max(frame_rate, 1.0);
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        const uint64_t ticks = std::max<uint64_t>(
            1,
            static_cast<uint64_t>(std::llround(static_cast<double>(frequency) / safe_rate))
        );

        if (frame_period_ticks_ != ticks) {
            frame_period_ticks_ = ticks;
            next_frame_deadline_ = 0;
            psxe_diag_breadcrumbf(
                "Frame pacing target updated fps=%.2f period_ms=%.3f",
                safe_rate,
                CounterTicksToMilliseconds(frame_period_ticks_)
            );
        }
#else
        (void)frame_rate;
#endif
    }

    void waitForFrameDeadline(double frame_rate) {
#if !defined(__EMSCRIPTEN__)
        updateFramePeriod(frame_rate);

        const uint64_t frequency = SDL_GetPerformanceFrequency();
        const uint64_t slack_ticks = std::max<uint64_t>(1, frequency / 2000u);
        uint64_t now = SDL_GetPerformanceCounter();

        // How late the loop arrived for THIS frame — the only honest measure of "the device
        // cannot keep up", and what adaptive frame skip acts on. Sampled here because this is
        // the one point that knows it: the resync branch below rewrites the deadline, and a
        // frame that overran shows up as lateness at the START of the frame after it.
        frame_lateness_ticks_ = (next_frame_deadline_ != 0 && now > next_frame_deadline_)
                                    ? (now - next_frame_deadline_)
                                    : 0;

        if (next_frame_deadline_ == 0) {
            next_frame_deadline_ = now;
            return;
        }

        if (frame_period_ticks_ != 0 && now > (next_frame_deadline_ + (frame_period_ticks_ * 2u))) {
            psxe_diag_breadcrumbf(
                "Frame pacing resync lateness_ms=%.3f",
                CounterTicksToMilliseconds(now - next_frame_deadline_)
            );
            next_frame_deadline_ = now;
            return;
        }

        while (now + slack_ticks < next_frame_deadline_) {
            const double wait_ms = CounterTicksToMilliseconds(next_frame_deadline_ - now);
            if (wait_ms > 1.5) {
                SDL_Delay(static_cast<Uint32>(wait_ms - 1.0));
            } else {
                std::this_thread::yield();
            }
            now = SDL_GetPerformanceCounter();
        }

        while (now < next_frame_deadline_) {
            std::this_thread::yield();
            now = SDL_GetPerformanceCounter();
        }
#else
        (void)frame_rate;
#endif
    }

    void advanceFrameDeadline() {
#if !defined(__EMSCRIPTEN__)
        if (frame_period_ticks_ == 0) {
            return;
        }

        if (next_frame_deadline_ == 0) {
            next_frame_deadline_ = SDL_GetPerformanceCounter();
        }

        next_frame_deadline_ += frame_period_ticks_;
#endif
    }

    void resetFramePacing(const char* reason) {
#if !defined(__EMSCRIPTEN__)
        next_frame_deadline_ = 0;
        frame_period_ticks_ = 0;
        frame_lateness_ticks_ = 0;
        if (reason && reason[0]) {
            psxe_diag_breadcrumbf("Frame pacing reset reason=%s", reason);
        }
#else
        (void)reason;
#endif
    }

    // Park the skip counter so the next decision resolves to "present". The run limit is never
    // above kMaxFrameSkip, so this is unconditional without having to know which mode is live.
    void requestPresentNextFrame() {
        frames_skipped_run_ = kMaxFrameSkip;
    }

    /**
     * Frame skip. Should THIS frame's presentation be dropped?
     *
     * ⚠ Presentation only. The machine is always stepped to the next vblank by
     * ArmsxSession::runFrame(), which is also where queueAudioForFrame() pulls the SPU — the SPU
     * is front-end-pulled, so skipping an emulated frame would advance every voice cursor and
     * ADSR envelope by the wrong amount and starve the device besides. What a skipped frame
     * drops is updateTexture() (the dirty-row scan + the framebuffer upload, and on the GLES
     * rasterizer the render-target readback) and draw() (the present itself). Guest timing,
     * audio and save states are bit-identical either way.
     *
     * Two modes, because they answer different questions:
     *   fixed (1..kMaxFrameSkip)  present one frame then skip N. Predictable, applies whether or
     *                             not the device is struggling, and is what "frame skip" means to
     *                             anyone arriving from another emulator. Also the battery lever.
     *   adaptive (-1)             skip only when the previous frame overran its budget, and never
     *                             more than kAdaptiveFrameSkipRun in a row. Costs nothing on a
     *                             device that keeps up and is the one that actually rescues one
     *                             that does not — which is why it is the mode the UI offers first.
     *
     * Deliberately NOT engaged when the pacer has no deadline to miss (frame limit off, or
     * fast-forward with an unlimited multiplier): "late" is meaningless there, adaptive would
     * skip every frame it is allowed to, and the result would read as a second, invisible
     * limiter fighting the one the user switched off. Fixed skip still applies in those modes,
     * because the user asked for a ratio rather than for a rescue.
     */
    bool shouldSkipPresent(bool stepping) {
        // A paused session presents the same still frame every tick and that IS the pause menu's
        // backdrop; a pending screenshot needs the frame's real geometry through updateTexture().
        if (!stepping || deferred_screenshot_) {
            requestPresentNextFrame();
            return false;
        }

        const int mode = session_.frameSkip();
        if (mode == 0) {
            requestPresentNextFrame();
            return false;
        }

        const int max_run = (mode > 0) ? std::min(mode, kMaxFrameSkip) : kAdaptiveFrameSkipRun;
        if (frames_skipped_run_ >= max_run) {
            frames_skipped_run_ = 0;
            return false;
        }

        if (mode < 0 && !runningBehindDeadline()) {
            frames_skipped_run_ = 0;
            return false;
        }

        frames_skipped_run_++;
        frames_dropped_++;
        return true;
    }

    // Was the loop late enough arriving for this frame that dropping a present is worth it?
    // A quarter of a frame period: small enough to react before the stutter is audible in the
    // pacing, large enough that ordinary scheduler jitter does not trip it.
    bool runningBehindDeadline() const {
#if !defined(__EMSCRIPTEN__)
        if (frame_period_ticks_ == 0) {
            return false;
        }

        // No real deadline to miss — see shouldSkipPresent().
        if (currentTargetFrameRate() >= ArmsxSession::uncappedFrameRate()) {
            return false;
        }

        return frame_lateness_ticks_ > (frame_period_ticks_ / 4u);
#else
        return false;
#endif
    }

    std::filesystem::path diagnosticsLogPath() const {
        const char* live_path = psxe_diag_log_path();
        if (live_path && live_path[0]) {
            return std::filesystem::path(live_path);
        }

        return DefaultDiagnosticsLogPath();
    }

    void applyLoggingSettings(const char* reason) {
        const bool enable_logs = settings_.logging_enabled;
        settings_.quiet = !enable_logs;

        if (!enable_logs && psxe_diag_is_enabled()) {
            psxe_diag_logf("diag", "Debug logging disabled reason=%s", reason ? reason : "(none)");
        }

        psxe_diag_set_enabled(enable_logs ? 1 : 0);
        log_set_level(settings_.log_level);
        log_set_quiet(settings_.quiet ? 1 : 0);

        if (enable_logs) {
            psxe_diag_logf(
                "diag",
                "Debug logging enabled reason=%s level=%s path=%s",
                reason ? reason : "(none)",
                log_level_string(settings_.log_level),
                diagnosticsLogPath().string().c_str()
            );
        }
    }

    void logRendererBootstrap(const char* phase, double frame_rate) const {
        if (!render_) {
            return;
        }

        SDL_RendererInfo info{};
        if (SDL_Renderer* sdl = sdlRenderer()) {
            SDL_GetRendererInfo(sdl, &info);
        }

        int output_width = 0;
        int output_height = 0;
        armsx_renderer_output_size(render_, &output_width, &output_height);

        int refresh_rate = 0;
        SDL_DisplayMode display_mode{};
        if (window_) {
            const int display_index = SDL_GetWindowDisplayIndex(window_);
            if (display_index >= 0 && SDL_GetCurrentDisplayMode(display_index, &display_mode) == 0) {
                refresh_rate = display_mode.refresh_rate;
            }
        }

        const char* timing_title = session_.valid() ? session_.timingModeTitle() : "UI";
        psxe_diag_logf(
            "renderer",
            "phase=%s source=%s driver=%s flags=%s output=%dx%d refresh_hz=%d timing=%s target_fps=%.2f frame_period_ms=%.3f requested_vsync=%s",
            phase ? phase : "(none)",
            external_renderer_ ? "external" : "internal",
            armsx_renderer_driver_name(render_),
            info.name ? RendererFlagsTitle(info.flags).c_str()
                      : armsx_render_backend_name(armsx_renderer_backend(render_)),
            output_width,
            output_height,
            refresh_rate,
            timing_title,
            frame_rate,
            frame_rate > 0.0 ? (1000.0 / frame_rate) : 0.0,
            (owns_renderer_ && !external_renderer_) ? (managed_renderer_vsync_ ? "true" : "false") : "host-controlled"
        );
    }

    void logUiRendererState(const char* stage) const {
        // SDL-renderer-specific state dump; the GL/Vulkan backends have no equivalent.
        SDL_Renderer* renderer_ = sdlRenderer();
        if (!renderer_) {
            return;
        }

        int window_width = 0;
        int window_height = 0;
        if (window_) {
            SDL_GetWindowSize(window_, &window_width, &window_height);
        }

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);

        SDL_Rect viewport = {0, 0, 0, 0};
        SDL_Rect clip_rect = {0, 0, 0, 0};
        int logical_width = 0;
        int logical_height = 0;
        float scale_x = 0.0f;
        float scale_y = 0.0f;

        SDL_RenderGetViewport(renderer_, &viewport);
        SDL_RenderGetClipRect(renderer_, &clip_rect);
        SDL_RenderGetLogicalSize(renderer_, &logical_width, &logical_height);
        SDL_RenderGetScale(renderer_, &scale_x, &scale_y);

        const SDL_Texture* target = SDL_GetRenderTarget(renderer_);
        const Uint32 window_flags = window_ ? SDL_GetWindowFlags(window_) : 0u;
        const bool clip_enabled = SDL_RenderIsClipEnabled(renderer_) == SDL_TRUE;
        const bool integer_scale = SDL_RenderGetIntegerScale(renderer_) == SDL_TRUE;

        float imgui_display_x = 0.0f;
        float imgui_display_y = 0.0f;
        float imgui_scale_x = 0.0f;
        float imgui_scale_y = 0.0f;
        float imgui_font_scale = 0.0f;
        int imgui_backend_flags = 0;
        int imgui_config_flags = 0;

        psxe_diag_logf(
            "ui",
            "%s window=%dx%d window_flags=0x%x output=%dx%d target=%p viewport=%d,%d %dx%d clip=%d,%d %dx%d scale=(%f,%f) logical=%dx%d clip_enabled=%s integer_scale=%s imgui_display=(%.1f,%.1f) imgui_fb_scale=(%.2f,%.2f) imgui_font_scale=%.3f backend_flags=0x%x config_flags=0x%x session_hw=%s texture=%dx%d format=%s",
            stage ? stage : "(state)",
            window_width,
            window_height,
            window_flags,
            output_width,
            output_height,
            (const void*)target,
            viewport.x,
            viewport.y,
            viewport.w,
            viewport.h,
            clip_rect.x,
            clip_rect.y,
            clip_rect.w,
            clip_rect.h,
            scale_x,
            scale_y,
            logical_width,
            logical_height,
            clip_enabled ? "true" : "false",
            integer_scale ? "true" : "false",
            imgui_display_x,
            imgui_display_y,
            imgui_scale_x,
            imgui_scale_y,
            imgui_font_scale,
            imgui_backend_flags,
            imgui_config_flags,
#ifdef USE_HARDWARE
            session_.valid() ? (session_.hardwareBackendActive() ? "true" : "false") : "false",
            session_.valid() ? session_.textureWidth() : 0,
            session_.valid() ? session_.textureHeight() : 0,
            session_.valid() ? SDL_GetPixelFormatName(session_.textureFormat()) : "(none)"
#else
            "false",
            0,
            0,
            "(none)"
#endif
        );
    }

    void logCpuState() const {
        if (!session_.valid() || !session_.psx() || !session_.psx()->cpu) {
            psxe_diag_logf("crash", "CPU state unavailable.");
            return;
        }

        const psx_cpu_t* cpu = session_.psx()->cpu;
        psxe_diag_logf("crash", "r0=%08x at=%08x v0=%08x v1=%08x", cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3]);
        psxe_diag_logf("crash", "a0=%08x a1=%08x a2=%08x a3=%08x", cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7]);
        psxe_diag_logf("crash", "t0=%08x t1=%08x t2=%08x t3=%08x", cpu->r[8], cpu->r[9], cpu->r[10], cpu->r[11]);
        psxe_diag_logf("crash", "t4=%08x t5=%08x t6=%08x t7=%08x", cpu->r[12], cpu->r[13], cpu->r[14], cpu->r[15]);
        psxe_diag_logf("crash", "s0=%08x s1=%08x s2=%08x s3=%08x", cpu->r[16], cpu->r[17], cpu->r[18], cpu->r[19]);
        psxe_diag_logf("crash", "s4=%08x s5=%08x s6=%08x s7=%08x", cpu->r[20], cpu->r[21], cpu->r[22], cpu->r[23]);
        psxe_diag_logf("crash", "t8=%08x t9=%08x k0=%08x k1=%08x", cpu->r[24], cpu->r[25], cpu->r[26], cpu->r[27]);
        psxe_diag_logf("crash", "gp=%08x sp=%08x fp=%08x ra=%08x", cpu->r[28], cpu->r[29], cpu->r[30], cpu->r[31]);
        psxe_diag_logf(
            "crash",
            "pc=%08x next=%08x saved=%08x hi=%08x lo=%08x epc=%08x opcode=%08x",
            cpu->pc,
            cpu->next_pc,
            cpu->saved_pc,
            cpu->hi,
            cpu->lo,
            cpu->cop0_r[COP0_EPC],
            cpu->opcode
        );
    }

    // [audio] driver, applied just before SDL_INIT_AUDIO — the only moment SDL reads
    // SDL_AUDIODRIVER. Android only; every other platform is left with SDL's own probe order,
    // which is what it has always used.
    //
    // On Android the choice is genuinely dangerous, not merely a preference: SDL's "android"
    // and "aaudio" backends reach org.libsdl.app.SDLAudioManager's static JNI glue, and with
    // Compose owning the Activity there is no SDLActivity to have set it up. openslES is the
    // one backend that stays entirely inside the native OpenSL ES API, which is why
    // frontend/android_jni.cpp forces it before anything else runs. That host also registers
    // the glue itself and hands SDLAudioManager a Context; only once it reports that it
    // managed both is aaudio (whose SDL_AudioInit unconditionally enumerates devices through
    // that Context) safe to select. "default" hands the choice back to SDL's probe order,
    // which starts with the "android" backend and is therefore gated the same way.
    void applyAudioDriverSetting() {
#if defined(__ANDROID__)
        const bool embedded = []() {
            std::lock_guard<std::mutex> lock(g_host_control_lock);
            return g_host_embedded;
        }();

        if (!embedded) {
            return;
        }

        int driver = settings_.audio_driver;
        if (driver != 1 && !g_host_audio_backends_ready.load(std::memory_order_acquire)) {
            ARMSX_BOOTLOG("core: audio driver '%s' needs host JNI audio glue that is not ready; using openslES",
                          AudioDriverToString(driver));
            driver = 1;
        }

        switch (driver) {
            case 0:
                unsetenv("SDL_AUDIODRIVER");
                break;
            case 2:
                setenv("SDL_AUDIODRIVER", "aaudio", 1);
                break;
            default:
                setenv("SDL_AUDIODRIVER", "openslES", 1);
                break;
        }
#endif
    }

    bool initializeSdl() {
        Uint32 required = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;

        // Embedded hosts own input (Compose touch overlay + Android InputDevice routing) and
        // there is no org.libsdl.app activity backing SDL's Android joystick JNI glue, so
        // asking for SDL_INIT_GAMECONTROLLER there would call into a null activity class.
        {
            std::lock_guard<std::mutex> lock(g_host_control_lock);
            if (g_host_embedded) {
                required &= ~static_cast<Uint32>(SDL_INIT_GAMECONTROLLER);
            }
        }

        psxe_diag_breadcrumbf("Initializing SDL subsystems flags=0x%x", required);

        // Audio is not worth failing the whole boot over: a silent game still beats a game that
        // never appears. Bring the mandatory subsystems up first, then try audio on its own.
        const Uint32 mandatory = required & ~static_cast<Uint32>(SDL_INIT_AUDIO);
        const bool wants_audio = (required & SDL_INIT_AUDIO) != 0;

        if (SDL_WasInit(0) == 0) {
            owns_sdl_ = true;
            if (SDL_Init(mandatory) != 0) {
                ARMSX_BOOTERR("core: SDL_Init(0x%x) failed: %s", mandatory, SDL_GetError());
                psxe_diag_logf("sdl", "SDL_Init failed: %s", SDL_GetError());
                return false;
            }
        } else if (SDL_InitSubSystem(mandatory) != 0) {
            ARMSX_BOOTERR("core: SDL_InitSubSystem(0x%x) failed: %s", mandatory, SDL_GetError());
            psxe_diag_logf("sdl", "SDL_InitSubSystem failed: %s", SDL_GetError());
            return false;
        }

        if (wants_audio) {
            applyAudioDriverSetting();
        }

        if (wants_audio && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            ARMSX_BOOTERR("core: audio init failed (continuing silently): %s", SDL_GetError());
            psxe_diag_logf("sdl", "Audio init failed (continuing without audio): %s", SDL_GetError());
        } else if (wants_audio) {
            ARMSX_BOOTLOG("core: audio driver=%s",
                          SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");
        }

        if (argc_ > 1) {
            for (int index = 1; index < argc_; index++) {
                const std::string_view arg(argv_[index] ? argv_[index] : "");

                auto set_boot_argument = [&](std::string_view value, std::optional<LaunchKind> forced_kind = std::nullopt) {
                    pending_cli_argument_ = std::string(value);
                    pending_cli_launch_ = LaunchForArgument(value, forced_kind);
                };

                if (arg == "--cdrom") {
                    if ((index + 1) < argc_) {
                        set_boot_argument(argv_[index + 1] ? argv_[index + 1] : "", LaunchKind::Disc);
                        index++;
                    }
                } else if (arg == "-x" || arg == "--exe") {
                    if ((index + 1) < argc_) {
                        set_boot_argument(argv_[index + 1] ? argv_[index + 1] : "", LaunchKind::Exe);
                        index++;
                    }
                } else if (arg.starts_with("--cdrom=")) {
                    set_boot_argument(arg.substr(8), LaunchKind::Disc);
                } else if (arg.starts_with("--exe=")) {
                    set_boot_argument(arg.substr(6), LaunchKind::Exe);
                } else if (!arg.empty() && arg[0] != '-') {
                    set_boot_argument(arg);
                }
            }
        }

        return true;
    }

    bool initializeWindowAndRenderer() {
        if (external_window_) {
            window_ = external_window_;
        } else {
            int window_width = 1280;
            int window_height = 720;
#if defined(__EMSCRIPTEN__)
            double css_width = 0.0;
            double css_height = 0.0;
            if (emscripten_get_element_css_size("#canvas", &css_width, &css_height) == EMSCRIPTEN_RESULT_SUCCESS &&
                css_width > 0.0 && css_height > 0.0) {
                window_width = std::max(1, static_cast<int>(std::lround(css_width)));
                window_height = std::max(1, static_cast<int>(std::lround(css_height)));
            }
#endif
            Uint32 flags = 0;
            if (SupportsManagedWindowSizing()) {
                flags |= SDL_WINDOW_RESIZABLE;
            }

            // A GL/Vulkan drawable has to be requested at window-creation time, and the GL
            // attributes (ES 3.0 profile) have to be set before SDL_CreateWindow().
#ifdef USE_HARDWARE
            const armsx_render_backend_t wanted = RenderBackendFor(settings_.gpu_backend);
#else
            const armsx_render_backend_t wanted = ARMSX_RENDER_BACKEND_SDL_SOFTWARE;
#endif
            armsx_render_prepare_window_attributes(wanted);
            flags |= armsx_render_window_flags(wanted);

            window_ = SDL_CreateWindow(
                "ARMSX",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                window_width,
                window_height,
                flags
            );

            if (!window_ && (flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN))) {
                // The GPU drawable was refused; retry as a plain window so the SDL fallback
                // ladder in createManagedRenderer() still has something to attach to.
                psxe_diag_logf("renderer",
                               "Window with a %s drawable failed (%s); retrying without it.",
                               armsx_render_backend_name(wanted), SDL_GetError());
                flags &= ~(Uint32)(SDL_WINDOW_OPENGL | SDL_WINDOW_VULKAN);
                window_ = SDL_CreateWindow(
                    "ARMSX",
                    SDL_WINDOWPOS_CENTERED,
                    SDL_WINDOWPOS_CENTERED,
                    window_width,
                    window_height,
                    flags
                );
            }

            owns_window_ = window_ != nullptr;
        }

        if (!window_) {
            psxe_diag_logf("renderer", "Window initialization failed: %s", SDL_GetError());
            return false;
        }

        render_ = nullptr;

#ifdef USE_HARDWARE
        // An embedder-supplied SDL_Renderer is normally adopted as-is. The exception is the
        // Android in-process host: it hands us a *software* renderer over an SDL_Surface that
        // it then CPU-blits into its ANativeWindow. When the user asked for a GPU backend and
        // that ANativeWindow was registered (armsx_render_set_native_window), we bind to the
        // surface directly instead and the host's blit bridge stands down.
        const bool prefer_native_gpu = external_renderer_ && armsx_render_native_window() &&
                                       (settings_.gpu_backend == GpuBackend::OpenGL ||
                                        settings_.gpu_backend == GpuBackend::Vulkan);
#else
        const bool prefer_native_gpu = false;
#endif

        if (prefer_native_gpu) {
            if (!createManagedRenderer(settings_.vsync_enabled)) {
                psxe_diag_logf("renderer", "GPU backend on the host surface failed; adopting the host renderer.");
            } else if (!armsx_render_native_window_claimed()) {
                // The ladder fell all the way back to an SDL backend on our own window, which
                // is not what the host surface expects. Drop it and adopt the host renderer.
                armsx_renderer_destroy(render_);
                render_ = nullptr;
                owns_renderer_ = false;
            }
        }

        if (!render_ && external_renderer_) {
            render_ = armsx_renderer_create_from_sdl(external_renderer_);
            owns_renderer_ = false;
        } else if (!render_) {
            if (!createManagedRenderer(settings_.vsync_enabled)) {
                return false;
            }
        }

        if (!render_) {
            psxe_diag_logf("renderer", "Renderer initialization failed: %s", SDL_GetError());
        }

        return render_ != nullptr;
    }

    bool initializeFsui() {
        return true;
    }

    void shutdown() {
        psxe_diag_breadcrumbf("Frontend shutdown");
        // Close the ADPF session while still on the thread that owns it. The session is bound
        // to this tid, and the next run in the same process (the Android host keeps the
        // library loaded between games) gets a different one.
        armsx_perf_hint_shutdown();
        input_router_.detach();
        // Drop the psx_t RetroAchievements reads memory through before it is freed. Covers the
        // early-failure paths that reach shutdown() without ever entering the main loop.
        armsx_ach_session_ended();
        session_.destroy();

        shutdownFsuiFrontend();

        // Adopted renderers (armsx_renderer_create_from_sdl) never own the embedder's
        // SDL_Renderer, so this is safe for both the managed and the external case.
        if (render_) {
            armsx_renderer_destroy(render_);
        }
        if (owns_window_ && window_) {
            SDL_DestroyWindow(window_);
        }

        render_ = nullptr;
        window_ = nullptr;

        if (owns_sdl_) {
            SDL_Quit();
        }

        psxe_diag_shutdown();
    }

    void handleEvent(const SDL_Event& event) {

        if (event.type == SDL_QUIT) {
            running_ = false;
            return;
        }

        if (event.type == SDL_DROPFILE) {
            if (event.drop.file) {
                queueLaunchArgument(event.drop.file, true);
                SDL_free(event.drop.file);
            }
            return;
        }

        if (event.type == SDL_DROPCOMPLETE) {
            return;
        }

        // FSUI removed: input is never gated by a native menu window (Compose owns menus).
        const bool fsui_active = false;
        input_router_.processEvent(event, session_.valid() ? &session_ : nullptr, fsui_active);

        if (event.type == SDL_KEYDOWN && session_.valid() && event.key.repeat == 0 && event.key.keysym.sym == SDLK_RETURN) {
            psx_exp2_atcons_put(session_.psx()->exp2, 13);
        }

        // A pause request is consumed but no longer opens a native menu; the Compose overlay owns
        // pause via JNI. Draining it keeps the router state clean.
        if (session_.valid()) {
            (void)input_router_.takePauseRequest();
        }
    }

    void runFrame() {
        // Affinity Control Mode. Here rather than once at startup because sched_setaffinity()
        // acts on the CALLING thread and this is the emulation thread, and because polling an
        // atomic per frame is what turns a boot-only setting into a live one. Costs one relaxed
        // load while the mode is unchanged, which is every frame in the default (off) case.
        armsx_affinity_apply_emulation_thread();

        /* Runahead: put the machine back where the timeline really is, BEFORE anything
           applies this frame's input. A snapshot carries the pad's button word, so a restore
           that happened after an input pass would swallow the press outright —
           applyHostPadCommands only ever sends the EDGE, so it would never be re-sent. No-op
           unless [emulation] runahead is on.

           Deliberately NOT gated on !paused(): the pause menu is exactly where a save state
           gets taken, and a save taken while a look-ahead was still outstanding would record
           the FUTURE rather than the timeline the player is on. */
        if (session_.valid()) {
            session_.runaheadRestore();
        }

        input_router_.tick(false);

        if (deferred_vsync_.has_value()) {
            const bool desired_vsync = *deferred_vsync_;
            deferred_vsync_.reset();

            if (owns_renderer_ && !external_renderer_ && desired_vsync != managed_renderer_vsync_) {
                recreateManagedRenderer(desired_vsync);
                if (!running_) {
                    return;
                }
            }
        }

        consumePendingLaunchArguments();
        applyHostControlRequests();

        // Backstop for a background→foreground reopen that failed. Deliberately AFTER the
        // request drain so a resume that just landed gets its device on this same frame.
        if (session_.valid()) {
            session_.ensureAudioDevice();
        }

        // Save/load-state requests are parked by the JNI thread and drained by
        // psx_state_service_requests() from inside psx_update(). psx_update() does not run while
        // the session is paused — and the pause menu is exactly where Save/Load State is pressed.
        // Drain here too, on this same (emulation) thread, or every state op taken from the menu
        // would sit parked until it timed out.
        if (session_.valid() && session_.paused()) {
            psx_state_service_requests();
        }

        waitForFrameDeadline(currentTargetFrameRate());

        // `touch files/logs/perf_log` sends the overlay's own snapshot to the diag log as well
        // as to the UI. The numbers otherwise exist only inside Compose, which makes every
        // measurement a screenshot read by eye — and HW_RENDERER_DESIGN.md §0.5.3 records a
        // sweep that produced six screenshots OF THE LIBRARY because of exactly that. With the
        // marker present two runs are matched on the primitive counters as numbers. Re-armed
        // rather than armed once, so a UI that switches the overlay off cannot silence it.
        if (perfLogEnabled() && !g_host_stats_enabled.load(std::memory_order_relaxed)) {
            g_host_stats_enabled.store(true, std::memory_order_relaxed);
            psx_perf_set_enabled(1);
        }

        // Phase timing for the overlay. Gated, not just unread: with the overlay off these
        // three clock reads never happen and psx_perf's counters are not armed either.
        const bool stats_enabled = g_host_stats_enabled.load(std::memory_order_relaxed);
        const uint64_t phase_frame_begin = stats_enabled ? SDL_GetPerformanceCounter() : 0;

        std::uint32_t session_steps = 0;
        const bool session_stepping = session_.valid() && !session_.paused();

        // Frame skip ([runtime] frame_skip). Decided BEFORE the step, because the work it drops
        // starts at updateTexture(): the dirty-row scan, the framebuffer upload and — on the
        // GLES rasterizer — the render-target readback, all of which are presentation, not
        // emulation. runFrame() itself always runs; see shouldSkipPresent() for why.
        const bool skip_present = shouldSkipPresent(session_stepping);

        // ADPF work bracket opens HERE — after waitForFrameDeadline() has already slept off the
        // limiter's share of the frame. What ADPF must be told is how long the emulation thread
        // WORKED, and a frame's wall clock is work + sleep: report that and every frame looks
        // like it consumed exactly its budget, which is a constant, not a signal. This span is
        // the same one the perf overlay prints as `emu`.
        //
        // A non-stepping tick (paused, no disc, menu open) is not a frame at all; pausing the
        // bracket keeps the pause out of the next report. Default off: one atomic load.
        if (session_stepping) {
            armsx_perf_hint_frame_begin(currentTargetFrameRate());
        } else {
            armsx_perf_hint_pause();
        }

        if (session_stepping) {
            session_steps = session_.runFrame();
            if (!skip_present) {
                session_.updateTexture(settings_);
            }
            // Always: begin_frame ran inside runFrame() and the GPU rasterizer's frame has to be
            // closed whether or not anyone is going to look at it.
            session_.finishHardwareFrame();
        }
        (void)session_steps;

        // ...and closes BEFORE the present. The present is real work, but on this port it runs
        // on this same thread and can block in the buffer queue waiting on the display — idle
        // time that would land in the report wearing work's clothes. A GPU-bound game therefore
        // reports a short work duration and gets no CPU boost, which is the right answer: its
        // CPU is not the thing that is short.
        armsx_perf_hint_frame_end();

        const uint64_t phase_emu_end = stats_enabled ? SDL_GetPerformanceCounter() : 0;

        // RetroAchievements pump. Unconditional: it also drives the saved-login restore and the
        // deferred disc identification, neither of which needs a stepping session.
        armsx_ach_frame_update(session_stepping);

        publishSessionTelemetry(session_stepping);

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-pre-present");
            psxe_diag_logf(
                "hw",
                "frame-present frame=%llu steps=%u paused=%s fast_forward=%s texture=%dx%d format=%s target_fps=%.3f",
                static_cast<unsigned long long>(session_.vblankCounter()),
                session_steps,
                session_.paused() ? "true" : "false",
                session_.fastForwardEnabled() ? "true" : "false",
                session_.textureWidth(),
                session_.textureHeight(),
                SDL_GetPixelFormatName(session_.textureFormat()),
                session_.targetFrameRate()
            );
        }
#endif

        if (pending_error_dialog_.has_value()) {
            psxe_diag_logf("ui", "error: %s", pending_error_dialog_->c_str());
            pending_error_dialog_.reset();
        }

        // Present the emulated frame through the active backend. No imgui/FSUI pass — the
        // Jetpack Compose front-end owns every menu/overlay.
        //
        // ...unless the app is off-screen. Drawing to a surface nobody can see burns real
        // battery: with the VM parked by the background pause, the loop was still measured
        // accumulating ~6.5 jiffies/s of system time purely uploading and posting frames to
        // an invisible window. Skipping the draw is safe because there is no observer to go
        // stale for — and the first frame after resume repaints it anyway.
        //
        // Deliberately its own flag, not `paused_` and not `audio_suspended_`: the pause
        // menu is on-screen and MUST keep presenting, and [audio] background_playback can
        // legitimately keep audio alive while the screen is off.
        const bool presentation_suspended =
            g_host_presentation_suspended.load(std::memory_order_acquire);

        // Coming back on-screen: arm the resume probe for the next few uploads. Deliberately
        // counted in UPLOADS rather than seconds — a session that comes back paused does not
        // upload until the user resumes it, which is exactly the frame worth sampling.
        if (g_presentation_was_suspended && !presentation_suspended) {
            if (resumeProbeEnabled()) {
                g_resume_probe_frames = 8;
            }
            psxe_diag_logf("renderer", "presentation resumed (window generation %lu)",
                           armsx_render_native_window_generation());
        } else if (!g_presentation_was_suspended && presentation_suspended) {
            psxe_diag_logf("renderer", "presentation suspended (window generation %lu)",
                           armsx_render_native_window_generation());
        }
        g_presentation_was_suspended = presentation_suspended;

        // ...or unless frame skip dropped this one. Same shape as the suspend above and for the
        // same reason, but a different question: suspended means "nobody can see it", skipped
        // means "the device cannot afford to draw every one". Note the ORDER — a suspended frame
        // still has to hit the pacing wait below, so skip cannot short-circuit it.
        if (presentation_suspended) {
            // Nothing to post, but the loop still has to pace itself or it becomes a spin.
            waitForFrameDeadline(currentTargetFrameRate());
        } else if (skip_present) {
            // Nothing at all: no upload happened, nothing is posted, and the surface keeps the
            // last frame that was. Deliberately NO extra pacing wait — the saved time is the
            // whole point, and advanceFrameDeadline() below keeps the emulated cadence exact.
        } else if (session_.valid()) {
            session_.draw(settings_);
        } else if (render_) {
            // No session: still hand the host a (black) frame every tick so an embedded
            // surface keeps getting posted instead of freezing on stale contents.
            armsx_renderer_present_blank(render_);
        }

        g_host_vm_active.store(session_.valid(), std::memory_order_release);
        // Only count frames that were actually posted. The counter is what the host polls to
        // decide the VM is alive and advancing (the auto-load-on-boot gate reads it), so
        // ticking it while nothing is being drawn would report progress that is not happening.
        // A skipped frame therefore does NOT tick it — which is also what makes the counter the
        // instrument that proves frame skip is really skipping.
        if (session_.valid() && !presentation_suspended && !skip_present) {
            g_host_presented_frames.fetch_add(1u, std::memory_order_relaxed);
        }
        // When a GPU backend owns the host's ANativeWindow it has already posted the frame
        // (eglSwapBuffers / vkQueuePresentKHR). Running the host's CPU blit bridge on top of
        // that would fight the buffer queue for the same surface.
        if (!presentation_suspended && !skip_present && !armsx_render_native_window_claimed()) {
            HostNotifyFramePresented();
        }

        publishFrameSkipTrace(session_stepping);
        publishFrameStats(stats_enabled, session_stepping, phase_frame_begin, phase_emu_end);

        applyDeferredActions();

        advanceFrameDeadline();
    }

    // Frame skip's own measurement, in the diag log rather than the OSD.
    //
    // "It looks smoother" is not evidence, and the OSD's FPS row deliberately keeps counting
    // EMULATED frames (that is what "is the game running at full speed" means), so nothing on
    // screen would show whether a single present was actually dropped. This prints the ratio
    // roughly every four seconds while skipping is armed: emulated frames in the window against
    // the ones that got posted, plus the pacing lateness adaptive mode is reacting to.
    //
    // Costs nothing while frame_skip = 0 — the counters only move when a frame is skipped, and
    // psxe_diag_logf() is itself suppressed unless [runtime] logging_enabled is on.
    void publishFrameSkipTrace(bool stepping) {
        if (!stepping) {
            frame_skip_trace_frames_ = 0;
            frames_dropped_ = 0;
            return;
        }

        if (session_.frameSkip() == 0) {
            frame_skip_trace_frames_ = 0;
            frames_dropped_ = 0;
            return;
        }

        if (++frame_skip_trace_frames_ < kFrameSkipTraceFrames) {
            return;
        }

        psxe_diag_logf("timing",
                       "frame skip mode=%d window_frames=%u dropped=%u presented_total=%u "
                       "late_ms=%.2f target_fps=%.2f",
                       session_.frameSkip(),
                       frame_skip_trace_frames_,
                       frames_dropped_,
                       g_host_presented_frames.load(std::memory_order_relaxed),
                       CounterTicksToMilliseconds(frame_lateness_ticks_),
                       currentTargetFrameRate());

        frame_skip_trace_frames_ = 0;
        frames_dropped_ = 0;
    }

    // Full performance overlay: drain the core's per-frame work counters, add the host phase
    // timing measured around them, and publish the pair as one snapshot for psxe_host_stats().
    //
    // Accumulated over the same half-second window the frame rate uses and divided by the frames
    // in it, so every number the overlay shows is a per-frame average rather than whichever
    // single frame happened to be current when the UI polled. WORST_FRAME_MS is the exception —
    // averaging a spike away is precisely the thing that hides a stutter.
    //
    // Nothing here runs while the overlay is off: psx_perf's counters are not armed, so the
    // whole core carries zero instrumentation, and the phase timers in runFrame() are skipped.
    void publishFrameStats(bool enabled, bool stepped, uint64_t frame_begin, uint64_t emu_end) {
        if (!enabled) {
            if (stats_window_frames_) {
                stats_window_frames_ = 0;
                stats_window_start_ = 0;
                stats_worst_frame_ticks_ = 0;
                stats_accum_ = {};
                stats_prev_frame_begin_ = 0;
            }

            return;
        }

        psx_perf_counters_t counters{};
        psx_perf_take_frame(&counters);

        if (!stepped) {
            // A paused or closed session did no guest work; keep the last published snapshot
            // rather than averaging zeroes into it, and restart the window on resume.
            stats_prev_frame_begin_ = 0;

            return;
        }

        const uint64_t now = SDL_GetPerformanceCounter();
        const uint64_t frequency = std::max<uint64_t>(1, SDL_GetPerformanceFrequency());

        // The frame's own wall clock: begin-to-begin, so it includes the limiter sleep that
        // precedes the emulation step. The first frame of a window has no predecessor.
        const uint64_t frame_ticks = stats_prev_frame_begin_ ? (frame_begin - stats_prev_frame_begin_) : 0;
        stats_prev_frame_begin_ = frame_begin;

        stats_accum_.emu_ticks += emu_end - frame_begin;
        stats_accum_.present_ticks += now - emu_end;
        stats_accum_.frame_ticks += frame_ticks;
        stats_accum_.counters.cpu_instructions += counters.cpu_instructions;
        stats_accum_.counters.cpu_cycles += counters.cpu_cycles;
        stats_accum_.counters.gte_ops += counters.gte_ops;
        for (int i = 0; i < PSX_PERF_PRIM_KINDS; ++i)
            stats_accum_.counters.gpu_primitives[i] += counters.gpu_primitives[i];
        stats_accum_.counters.gpu_raster_pixels += counters.gpu_raster_pixels;
        stats_accum_.counters.gpu_vram_words += counters.gpu_vram_words;
        stats_accum_.counters.spu_samples += counters.spu_samples;
        stats_accum_.counters.spu_voice_samples += counters.spu_voice_samples;
        stats_accum_.counters.mdec_macroblocks += counters.mdec_macroblocks;
        stats_accum_.counters.mdec_blocks += counters.mdec_blocks;
        stats_accum_.counters.cdrom_sectors += counters.cdrom_sectors;
        for (int i = 0; i < PSX_PERF_DMA_CHANNELS; ++i)
            stats_accum_.counters.dma_words[i] += counters.dma_words[i];

        if (frame_ticks > stats_worst_frame_ticks_)
            stats_worst_frame_ticks_ = frame_ticks;

        ++stats_window_frames_;

        if (stats_window_start_ == 0) {
            stats_window_start_ = now;

            return;
        }

        if ((now - stats_window_start_) < (frequency / 2u))
            return;

        const double frames = static_cast<double>(stats_window_frames_);
        const double ms_per_tick = 1000.0 / static_cast<double>(frequency);
        const auto per_frame = [frames](uint64_t total) {
            return static_cast<double>(total) / frames;
        };

        double out[PSXE_HOST_STAT_COUNT] = {};

        const double frame_ms = per_frame(stats_accum_.frame_ticks) * ms_per_tick;
        const double emu_ms = per_frame(stats_accum_.emu_ticks) * ms_per_tick;
        const double present_ms = per_frame(stats_accum_.present_ticks) * ms_per_tick;

        out[PSXE_HOST_STAT_FRAME_MS] = frame_ms;
        out[PSXE_HOST_STAT_EMU_MS] = emu_ms;
        out[PSXE_HOST_STAT_PRESENT_MS] = present_ms;
        out[PSXE_HOST_STAT_IDLE_MS] = std::max(0.0, frame_ms - emu_ms - present_ms);
        out[PSXE_HOST_STAT_WORST_FRAME_MS] = static_cast<double>(stats_worst_frame_ticks_) * ms_per_tick;

        const psx_perf_counters_t& c = stats_accum_.counters;

        out[PSXE_HOST_STAT_CPU_INSTRUCTIONS] = per_frame(c.cpu_instructions);
        out[PSXE_HOST_STAT_CPU_CYCLES] = per_frame(c.cpu_cycles);
        out[PSXE_HOST_STAT_GTE_OPS] = per_frame(c.gte_ops);
        out[PSXE_HOST_STAT_GPU_TRIANGLES] = per_frame(c.gpu_primitives[PSX_PERF_PRIM_TRIANGLE]);
        out[PSXE_HOST_STAT_GPU_RECTS] = per_frame(c.gpu_primitives[PSX_PERF_PRIM_RECT]);
        out[PSXE_HOST_STAT_GPU_LINES] = per_frame(c.gpu_primitives[PSX_PERF_PRIM_LINE]);
        out[PSXE_HOST_STAT_GPU_PIXELS] = per_frame(c.gpu_raster_pixels);
        out[PSXE_HOST_STAT_GPU_VRAM_WORDS] = per_frame(c.gpu_vram_words);
        // Voices are per generated SAMPLE, so this divides by samples, not frames: the result
        // is how many of the 24 voices were sounding on average, which is what "SPU load" means.
        out[PSXE_HOST_STAT_SPU_VOICES] = c.spu_samples
            ? (static_cast<double>(c.spu_voice_samples) / static_cast<double>(c.spu_samples))
            : 0.0;
        out[PSXE_HOST_STAT_MDEC_MACROBLOCKS] = per_frame(c.mdec_macroblocks);
        out[PSXE_HOST_STAT_MDEC_BLOCKS] = per_frame(c.mdec_blocks);
        out[PSXE_HOST_STAT_CDROM_SECTORS] = per_frame(c.cdrom_sectors);

        uint64_t dma_total = 0;
        for (int i = 0; i < PSX_PERF_DMA_CHANNELS; ++i)
            dma_total += c.dma_words[i];

        out[PSXE_HOST_STAT_DMA_WORDS] = per_frame(dma_total);
        out[PSXE_HOST_STAT_DMA_GPU_WORDS] = per_frame(c.dma_words[PSX_PERF_DMA_GPU]);
        out[PSXE_HOST_STAT_DMA_SPU_WORDS] = per_frame(c.dma_words[PSX_PERF_DMA_SPU]);
        out[PSXE_HOST_STAT_DMA_MDEC_WORDS] =
            per_frame(c.dma_words[PSX_PERF_DMA_MDEC_IN] + c.dma_words[PSX_PERF_DMA_MDEC_OUT]);
        out[PSXE_HOST_STAT_DMA_CDROM_WORDS] = per_frame(c.dma_words[PSX_PERF_DMA_CDROM]);
        out[PSXE_HOST_STAT_DMA_OTC_WORDS] = per_frame(c.dma_words[PSX_PERF_DMA_OTC]);

        psx_t* machine = session_.psx();

        if (machine) {
            out[PSXE_HOST_STAT_WIDTH] = static_cast<double>(psx_get_display_width(machine));
            out[PSXE_HOST_STAT_HEIGHT] = static_cast<double>(psx_get_display_height(machine));
        }

        /* Host device usage. Sampled here rather than on the UI side because the poll is
           rate-limited internally and this is already the once-per-window publish point. The
           unavailable sentinel is negative and is passed through untouched — see host_usage.h
           on why a missing GPU counter must not be substituted with something plausible. */
        {
            armsx_host_usage_t usage{};
            armsx_host_usage_sample(&usage);
            out[PSXE_HOST_STAT_HOST_CPU_PERCENT] = usage.cpu_percent;
            out[PSXE_HOST_STAT_HOST_CPU_CORES] = static_cast<double>(usage.cpu_cores);
            out[PSXE_HOST_STAT_HOST_RAM_MB] = usage.ram_mb;
            out[PSXE_HOST_STAT_HOST_RAM_AVAILABLE_MB] = usage.ram_available_mb;
            out[PSXE_HOST_STAT_HOST_GPU_PERCENT] = usage.gpu_percent;
        }

        /* Read from the GPU rather than from settings.internal_scale: the requested scale and
           the live one differ whenever the rasterizer fell back (no GL context, or the §4.6
           downgrade fired). The OSD must show what is running, not what was asked for. */
        {
            psx_gpu_t* scale_gpu = machine ? psx_get_gpu(machine) : nullptr;
            out[PSXE_HOST_STAT_INTERNAL_SCALE] =
                scale_gpu ? (double)psx_gpu_resolution_scale(scale_gpu) : 1.0;
        }

        {
            std::lock_guard<std::mutex> lock(g_host_stats_lock);
            std::memcpy(g_host_stats, out, sizeof(out));
            g_host_stats_published = true;
        }

        if (perfLogEnabled()) {
            psxe_diag_logf(
                "perf",
                "fps=%.1f frame=%.2f emu=%.2f present=%.2f idle=%.2f worst=%.2f "
                "tri=%.0f rect=%.0f line=%.0f px=%.0f vramwords=%.0f disp=%.0fx%.0f frames=%u",
                frame_ms > 0.0 ? (1000.0 / frame_ms) : 0.0, frame_ms, emu_ms, present_ms,
                out[PSXE_HOST_STAT_IDLE_MS], out[PSXE_HOST_STAT_WORST_FRAME_MS],
                out[PSXE_HOST_STAT_GPU_TRIANGLES], out[PSXE_HOST_STAT_GPU_RECTS],
                out[PSXE_HOST_STAT_GPU_LINES], out[PSXE_HOST_STAT_GPU_PIXELS],
                out[PSXE_HOST_STAT_GPU_VRAM_WORDS], out[PSXE_HOST_STAT_WIDTH],
                out[PSXE_HOST_STAT_HEIGHT], stats_window_frames_);
        }

        stats_window_start_ = now;
        stats_window_frames_ = 0;
        stats_worst_frame_ticks_ = 0;
        stats_accum_ = {};
    }

    // Publish what the emulation loop knows about itself for an embedded host to poll
    // (psxe_host_measured_fps / psxe_host_nominal_frame_rate). The Android front-end draws its
    // OSD in Compose, so this is the only route those numbers have out of the core.
    //
    // The measured rate is averaged over a window rather than derived from the last frame: a
    // per-frame reciprocal jitters far too much to read, and this is a number a human stares at.
    // Everything resets the moment the session stops stepping (paused, closed, or between games)
    // so a resumed game can never briefly show the rate it had before the pause.
    void publishSessionTelemetry(bool stepped) {
        if (!stepped) {
            fps_window_start_ = 0;
            fps_window_frames_ = 0;
            g_host_measured_fps.store(0.0f, std::memory_order_relaxed);
            g_host_nominal_fps.store(
                session_.valid() ? static_cast<float>(session_.frameRate()) : 0.0f,
                std::memory_order_relaxed);
            return;
        }

        g_host_nominal_fps.store(static_cast<float>(session_.frameRate()), std::memory_order_relaxed);

        const uint64_t now = SDL_GetPerformanceCounter();
        if (fps_window_start_ == 0) {
            fps_window_start_ = now;
            fps_window_frames_ = 0;
            return;
        }

        ++fps_window_frames_;

        const uint64_t frequency = std::max<uint64_t>(1, SDL_GetPerformanceFrequency());
        const uint64_t elapsed = now - fps_window_start_;
        if (elapsed < (frequency / 2u)) {
            return; // Half-second window: settled enough to read, quick enough to react.
        }

        const double seconds = static_cast<double>(elapsed) / static_cast<double>(frequency);
        g_host_measured_fps.store(
            static_cast<float>(static_cast<double>(fps_window_frames_) / seconds),
            std::memory_order_relaxed);
        fps_window_start_ = now;
        fps_window_frames_ = 0;
    }

    // Fast-forward has to escape the DISPLAY's refresh rate, not just the frame-pacing deadline.
    // targetFrameRate() doubles the deadline, but with vsync on the present blocks anyway
    // (eglSwapInterval(1) / VK_PRESENT_MODE_FIFO), so on a 60 Hz panel a ~59.94 fps game
    // fast-forwards to ~60 fps — i.e. not at all, which is exactly how it reads on a phone. The
    // swap interval is therefore dropped for the duration and restored from the user's setting
    // afterwards. Only ever touched on a fast-forward edge, so a normal session keeps whatever
    // the renderer was built with. Safe on every backend: GL sets the swap interval live, Vulkan
    // just invalidates the swapchain so the next present rebuilds it with the new present mode,
    // and the SDL blit-bridge backend implements no set_vsync at all (a no-op).
    void applyPresentVsync(bool fast_forward) {
        if (!render_ || fast_forward_vsync_dropped_ == fast_forward) {
            return;
        }

        fast_forward_vsync_dropped_ = fast_forward;
        armsx_renderer_set_vsync(render_, settings_.vsync_enabled && !fast_forward);
        psxe_diag_breadcrumbf("Present vsync %s for fast-forward",
                              fast_forward ? "dropped" : "restored");
    }


    // Drain everything the embedded host parked since the last frame. Runs on the
    // emulation thread, so it is the only place that touches the psx_t / SDL state.
    void applyHostControlRequests() {
        std::vector<HostPadCommand> pad_commands;
        bool pause_pending = false;
        bool pause_value = false;
        bool audio_suspend_pending = false;
        bool audio_suspend_value = false;
        bool shutdown_pending = false;
        bool reset_pending = false;
        bool fast_forward_pending = false;
        bool fast_forward_value = false;
        bool speed_limits_pending = false;
        bool speed_limit_frame_limit = true;
        int speed_limit_percent = 100;
        int speed_limit_fps = 0;
        double speed_limit_fast_forward = 2.0;
        int speed_limit_frame_skip = 0;
        bool screenshot_pending = false;
        bool disc_swap_pending = false;
        std::string disc_swap_path;
        std::string screenshot_path;
        bool multitap_pending = false;
        bool multitap_value = false;
        bool rewind_config_pending = false;
        bool rewind_config_enabled = false;
        int rewind_config_seconds = PSX_REWIND_DEFAULT_SECONDS;
        int rewind_config_frequency = PSX_REWIND_DEFAULT_FREQUENCY;
        bool runahead_pending = false;
        int runahead_value = 0;

        {
            std::lock_guard<std::mutex> lock(g_host_control_lock);
            pad_commands.swap(g_host_pad_queue);
            std::swap(pause_pending, g_host_pause_pending);
            pause_value = g_host_pause_value;
            std::swap(audio_suspend_pending, g_host_audio_suspend_pending);
            audio_suspend_value = g_host_audio_suspend_value;
            std::swap(shutdown_pending, g_host_shutdown_pending);
            std::swap(reset_pending, g_host_reset_pending);
            std::swap(fast_forward_pending, g_host_fast_forward_pending);
            fast_forward_value = g_host_fast_forward_value;
            std::swap(speed_limits_pending, g_host_speed_limits_pending);
            speed_limit_frame_limit = g_host_speed_limit_frame_limit;
            speed_limit_percent = g_host_speed_limit_percent;
            speed_limit_fps = g_host_speed_limit_fps;
            speed_limit_fast_forward = g_host_speed_limit_fast_forward;
            speed_limit_frame_skip = g_host_speed_limit_frame_skip;
            std::swap(screenshot_pending, g_host_screenshot_pending);
            screenshot_path.swap(g_host_screenshot_path);
            std::swap(disc_swap_pending, g_host_disc_swap_pending);
            disc_swap_path.swap(g_host_disc_swap_path);
            std::swap(multitap_pending, g_host_multitap_pending);
            multitap_value = g_host_multitap_value;
            std::swap(rewind_config_pending, g_host_rewind_config_pending);
            rewind_config_enabled = g_host_rewind_config_enabled;
            rewind_config_seconds = g_host_rewind_config_seconds;
            rewind_config_frequency = g_host_rewind_config_frequency;
            std::swap(runahead_pending, g_host_runahead_pending);
            runahead_value = g_host_runahead_value;
        }

        /* Multitap BEFORE the pad commands: a toggle in the same pass as a press must reach
           the device that is actually going to be in the port, not the one being thrown away
           (whose held-button state goes with it). */
        if (multitap_pending) {
            settings_.multitap = multitap_value;

            if (session_.valid() && session_.setMultitapEnabled(multitap_value)) {
                /* Every held bit belonged to the device that was just destroyed. */
                for (int i = 0; i < kHostMaxPlayers; i++) {
                    host_pad_mask_player_[i] = 0;
                    host_pad_pressed_prev_pass_[i] = 0;
                }
            }
        }

        if (rewind_config_pending) {
            settings_.rewind = rewind_config_enabled;
            settings_.rewind_seconds = rewind_config_seconds;
            settings_.rewind_frequency = rewind_config_frequency;
            psx_rewind_configure(rewind_config_enabled ? 1 : 0, rewind_config_seconds,
                                 rewind_config_frequency);
        }

        if (runahead_pending) {
            settings_.runahead = runahead_value;
            psx_runahead_configure(runahead_value);
        }

        applyHostPadCommands(pad_commands);

        // [audio] background_playback is the user opting IN to a game that keeps running with the
        // screen off. Default off: quiet, underrunning audio out of a sleeping device is never
        // what anyone wanted, and a PS1 core burning a core in your pocket even less so.
        // Checked here rather than in psxe_host_set_audio_suspended() so the live settings are
        // in scope, and so the toggle applies to a request that is already parked.
        const bool suspend_audio = audio_suspend_value && !settings_.audio_background_playback;

        // Un-suspend BEFORE the resume and suspend AFTER the pause, so the device is never asked
        // to carry frames it cannot play: coming back, the stream is running again before the VM
        // produces its first sample; going away, the VM has already stopped producing.
        if (audio_suspend_pending && !suspend_audio && session_.valid()) {
            session_.setAudioSuspended(false);
        }

        if (pause_pending && session_.valid()) {
            session_.setPaused(pause_value);
            if (!pause_value) {
                resetFramePacing("host-resume");
            }
        }

        if (audio_suspend_pending && suspend_audio && session_.valid()) {
            session_.setAudioSuspended(true);
        }

        if (reset_pending) {
            deferred_reset_ = true;
        }

        // Speed limits before the fast-forward edge: engaging fast-forward in the same pass
        // that changed its multiplier must use the NEW multiplier, not the previous one.
        if (speed_limits_pending) {
            settings_.frame_limit = speed_limit_frame_limit;
            settings_.speed_percent = speed_limit_percent;
            settings_.fps_limit = speed_limit_fps;
            settings_.fast_forward_speed = speed_limit_fast_forward;
            settings_.frame_skip = speed_limit_frame_skip;

            if (session_.valid()) {
                session_.setSpeedLimits(speed_limit_frame_limit, speed_limit_percent, speed_limit_fps,
                                        speed_limit_fast_forward, speed_limit_frame_skip);
                resetFramePacing("host-speed-limits");
            }
            // Whatever run of skipped presents was in flight belongs to the old policy; the
            // frame after a change must be drawn so the user sees the setting land.
            requestPresentNextFrame();
        }

        if (fast_forward_pending) {
            deferred_fast_forward_value_ = fast_forward_value;
        }

        if (screenshot_pending) {
            deferred_screenshot_ = true;
            deferred_screenshot_path_ = std::move(screenshot_path);
        }

        if (disc_swap_pending && !disc_swap_path.empty()) {
            /* swapDisc() clears fast-forward and re-tags the launch kind itself. A failure is
               logged rather than dialogued: on Android there is no modal to show it in, and the
               running game simply keeps the disc it had — which is the correct outcome. */
            if (!session_.swapDisc(std::filesystem::path(disc_swap_path))) {
                log_error("disc swap failed: %s", disc_swap_path.c_str());
            } else {
                resetFramePacing("change-disc");
            }
        }

        if (shutdown_pending) {
            running_ = false;
        }
    }

    // A press and its release can both land inside one frame (a quick tap on the touch
    // overlay, a macro, a turbo pulse, `adb shell input keyevent`). Applying both would
    // leave the guest seeing nothing at all, so a release whose press happened in this
    // pass — or in the one before it — is pushed back for the next frame.
    //
    // TWO frames, not one, deliberately. One frame is not reliably observable: the guest
    // polls the pad over the serial link at a phase of its own choosing, and a tap that
    // opened and closed inside a single emulated frame was missed roughly half the time
    // (measured on Crash Bandicoot's title menu — two injected d-pad taps moved the
    // selection one step). A real controller press already spans several frames, so the
    // extra frame costs held input nothing: prev_pass is empty by the time its release
    // arrives, and the release is applied immediately.
    void applyHostPadCommands(std::vector<HostPadCommand>& commands) {
        // Consume the previous pass's presses even on an empty drain, so a long-held
        // button's release is never delayed by a stale mask.
        uint32_t pressed_prev_pass[kHostMaxPlayers];
        for (int i = 0; i < kHostMaxPlayers; i++) {
            pressed_prev_pass[i] = host_pad_pressed_prev_pass_[i];
            host_pad_pressed_prev_pass_[i] = 0;
        }

        if (commands.empty()) {
            return;
        }

        psx_pad_t* pad = session_.valid() ? session_.pad() : nullptr;
        if (!pad) {
            for (int i = 0; i < kHostMaxPlayers; i++) {
                host_pad_mask_player_[i] = 0;
            }
            return;
        }

        uint32_t pressed_this_pass[kHostMaxPlayers] = {};
        std::vector<HostPadCommand> deferred;

        for (const HostPadCommand& command : commands) {
            if (command.kind == HostPadCommandKind::Analog) {
                const int stick = command.stick != 0 ? 1 : 0;
                const uint16_t x = static_cast<uint16_t>(std::clamp(command.x, 0, 0xFF));
                const uint16_t y = static_cast<uint16_t>(std::clamp(command.y, 0, 0xFF));
                psx_pad_analog_change_player(pad, 0, command.player,
                    stick == 0 ? PSXI_AX_SDA_LEFT_HORZ : PSXI_AX_SDA_RIGHT_HORZ, x);
                psx_pad_analog_change_player(pad, 0, command.player,
                    stick == 0 ? PSXI_AX_SDA_LEFT_VERT : PSXI_AX_SDA_RIGHT_VERT, y);
                continue;
            }

            if (!command.mask) {
                continue;
            }

            // Held-button bookkeeping is PER PLAYER. Sharing one mask across a multitap
            // would let player 2's release cancel player 1's identical press — the two are
            // different buttons on different pads and only look the same as a bit.
            const int player = std::clamp(command.player, 0, kHostMaxPlayers - 1);
            uint32_t& held = host_pad_mask_player_[player];

            if (command.pressed) {
                if ((held & command.mask) == 0) {
                    psx_pad_button_press_player(pad, 0, player, command.mask);
                    held |= command.mask;
                }
                pressed_this_pass[player] |= command.mask;
                continue;
            }

            if (((pressed_this_pass[player] | pressed_prev_pass[player]) & command.mask) != 0) {
                deferred.push_back(command);
                continue;
            }

            if ((held & command.mask) != 0) {
                psx_pad_button_release_player(pad, 0, player, command.mask);
                held &= ~command.mask;
            }
        }

        for (int i = 0; i < kHostMaxPlayers; i++) {
            host_pad_pressed_prev_pass_[i] = pressed_this_pass[i];
        }

        if (!deferred.empty()) {
            std::lock_guard<std::mutex> lock(g_host_control_lock);
            g_host_pad_queue.insert(g_host_pad_queue.begin(), deferred.begin(), deferred.end());
        }
    }

    void applyDeferredActions() {
        if (deferred_launch_.has_value()) {
            const LaunchRequest request = *deferred_launch_;
            deferred_launch_.reset();
            psxe_diag_breadcrumbf("Deferred launch kind=%s path=%s",
                LaunchKindTitle(request.kind),
                request.path.empty() ? "(none)" : request.path.string().c_str());
            if (request.kind != LaunchKind::None) {
                launchSession(request, close_ui_after_launch_);
            }
            close_ui_after_launch_ = false;
        }

        if (deferred_exit_to_library_) {
            deferred_exit_to_library_ = false;
            exitToLibrary();
        }

        if (deferred_reset_) {
            deferred_reset_ = false;
            if (session_.valid()) {
                session_.reset();
                session_.setPaused(false);
                resetFramePacing("deferred-reset");
            }
        }

        if (deferred_change_disc_.has_value()) {
            const std::filesystem::path path = *deferred_change_disc_;
            deferred_change_disc_.reset();

            if (path.empty()) {
                returnToMainWindow();
                return;
            }

            if (!session_.swapDisc(path)) {
                pending_error_dialog_ = "Failed to swap to the selected disc image.";
            } else {
                session_.setPaused(false);
                resetFramePacing("change-disc");
                refreshGameList(false);
                // A new disc is a different RA game: drop the old set and re-identify.
                armsx_ach_session_started(session_.psx());
                returnToMainWindow();
            }
        }

        if (deferred_screenshot_) {
            deferred_screenshot_ = false;

            // An embedded host (JNI saveScreenshot) supplies its own destination; the
            // built-in path keeps the timestamped snap/ folder. Either way the payload is
            // written by SDL_SaveBMP, so a caller asking for ".png" still gets BMP bytes.
            if (!deferred_screenshot_path_.empty()) {
                const std::filesystem::path target(deferred_screenshot_path_);
                deferred_screenshot_path_.clear();
                try {
                    if (target.has_parent_path()) {
                        std::filesystem::create_directories(target.parent_path());
                    }
                    if (!session_.saveScreenshot(target)) {
                        pending_error_dialog_ = "Failed to write the screenshot.";
                    }
                } catch (...) {
                    pending_error_dialog_ = "Failed to prepare the screenshot folder.";
                }
            } else {
                const std::filesystem::path snap_dir = DefaultBrowseDirectory() / "snap";
                try {
                    std::filesystem::create_directories(snap_dir);
                    const std::time_t now = std::time(nullptr);
                    char name[64] = {};
                    std::strftime(name, sizeof(name), "armsx-%Y%m%d-%H%M%S.bmp", std::localtime(&now));
                    if (!session_.saveScreenshot(snap_dir / name)) {
                        pending_error_dialog_ = "Failed to write the screenshot.";
                    }
                } catch (...) {
                    pending_error_dialog_ = "Failed to prepare the screenshot folder.";
                }
            }
        }

        // The single fast-forward entry point. There used to be a second one — a
        // deferred_fast_forward_toggle_ flag with its own copy of this block, meant for an
        // in-core hotkey — but nothing ever set it (the toggle lives in the Compose front-end
        // and arrives through psxe_host_set_fast_forward as an absolute value), so it was two
        // code paths where only one ran. The toggle is now resolved by the caller.
        if (deferred_fast_forward_value_.has_value()) {
            const bool enabled = *deferred_fast_forward_value_;
            deferred_fast_forward_value_.reset();
            if (session_.valid() && session_.fastForwardEnabled() != enabled) {
                session_.setFastForwardEnabled(enabled);
                resetFramePacing("host-fast-forward");
            }
            // Outside the guard above: a request that arrives with no session (the front-end
            // clears fast-forward as it closes a game) still has to hand the swap interval back.
            applyPresentVsync(session_.valid() && session_.fastForwardEnabled());
        }
    }

    bool launchSession(const LaunchRequest& request, bool close_ui) {
        std::string error;
        psxe_diag_breadcrumbf("Launching session kind=%s path=%s close_ui=%s",
            LaunchKindTitle(request.kind),
            request.path.empty() ? "(none)" : request.path.string().c_str(),
            close_ui ? "true" : "false");

        if (!session_.create(render_, settings_, request, error)) {
            pending_error_dialog_ = error;
            psxe_diag_logf("launch", "Session launch failed kind=%s error=%s", LaunchKindTitle(request.kind), error.c_str());
            if (request.kind == LaunchKind::Bios) {
                showLandingWindow();
            } else {
                showGameListWindow();
            }
            return false;
        }

        input_router_.attach(session_.pad());
        resetFramePacing("launch-session");
        applyWindowMetrics();
        logRendererBootstrap("session-launch", session_.frameRate());
        refreshGameList(false);

        // Publish the machine to RetroAchievements. Identification (disc hash + set download)
        // is deferred to the first frame update so the disc read never lands inside a boot.
        armsx_ach_session_started(session_.psx());

        if (close_ui) {
            returnToMainWindow();
        }

        return true;
    }

    void exitToLibrary() {
        psxe_diag_breadcrumbf("Exit to library requested");
        input_router_.detach();
        armsx_ach_session_ended();
        session_.destroy();
        resetFramePacing("exit-to-library");
        showLandingWindow();
    }


    void refreshGameList(bool full_rescan) {
        (void)full_rescan;
    }










    void applyWindowMetrics() {
        if (!SupportsManagedWindowSizing() || !owns_window_ || !window_) {
            return;
        }

        int width = 1280;
        int height = 720;

        if (session_.valid()) {
            if (session_.debugView()) {
                width = PSX_GPU_FB_WIDTH;
                height = PSX_GPU_FB_HEIGHT;
            } else if (settings_.display_aspect == 2) {
                height = std::max(240, settings_.upscale_height);
                width = static_cast<int>((16.0f / 9.0f) * static_cast<float>(height));
            } else if (settings_.display_aspect == 1) {
                width = 320 * std::max(1, settings_.scale);
                height = width;
            } else {
                int base_width = 320;
                if (session_.psx()) {
                    const int display_width = static_cast<int>(psx_get_dmode_width(session_.psx()));
                    if (display_width == 256 || display_width == 320) {
                        base_width = display_width;
                    } else if (display_width == 368) {
                        base_width = 384;
                    }
                }

                width = base_width * std::max(1, settings_.scale);
                height = 240 * std::max(1, settings_.scale);
            }
        }
        SDL_SetWindowSize(window_, width, height);
    }

    std::filesystem::path initialBrowseDirectory() const {
        if (session_.valid()) {
            const ArmsxGameInfo info = session_.currentGameInfo();
            if (!info.path.empty() && info.path.has_parent_path()) {
                return info.path.parent_path();
            }
        }

        if (!settings_.ui_state.game_list_paths.empty()) {
            return settings_.ui_state.game_list_paths.front();
        }

        if (!settings_.ui_state.game_list_recursive_paths.empty()) {
            return settings_.ui_state.game_list_recursive_paths.front();
        }

        return DefaultBrowseDirectory();
    }

    std::filesystem::path browseDirectoryForPath(const std::string& configured_path, bool expect_directory) const {
        std::error_code ec;

        if (!configured_path.empty()) {
            const std::filesystem::path candidate(configured_path);

            if (std::filesystem::exists(candidate, ec)) {
                if (expect_directory && std::filesystem::is_directory(candidate, ec)) {
                    return candidate;
                }

                if (!expect_directory) {
                    if (std::filesystem::is_directory(candidate, ec)) {
                        return candidate;
                    }

                    if (candidate.has_parent_path() && std::filesystem::exists(candidate.parent_path(), ec)) {
                        return candidate.parent_path();
                    }
                }
            }

            if (candidate.has_parent_path() && std::filesystem::exists(candidate.parent_path(), ec)) {
                return candidate.parent_path();
            }
        }

        return initialBrowseDirectory();
    }

    int argc_ = 0;
    const char* const* argv_ = nullptr;
    SDL_Window* external_window_ = nullptr;
    SDL_Renderer* external_renderer_ = nullptr;
    CliFlags cli_{};
    FrontendSettings settings_{};
    bool running_ = true;
    bool owns_sdl_ = false;
    bool owns_window_ = false;
    bool owns_renderer_ = false;
    SDL_Window* window_ = nullptr;
    armsx_renderer_t* render_ = nullptr;
    ArmsxSession session_{};
    GameplayInputRouter input_router_{};
    std::optional<std::string> pending_error_dialog_{};
    std::optional<LaunchRequest> deferred_launch_{};
    std::optional<std::filesystem::path> deferred_change_disc_{};
    std::optional<bool> deferred_vsync_{};
    std::optional<std::string> pending_settings_page_restore_{};
    std::optional<LaunchRequest> pending_cli_launch_{};
    std::string pending_cli_argument_;
    bool close_ui_after_launch_ = false;
    bool deferred_exit_to_library_ = false;
    bool deferred_reset_ = false;
    bool deferred_screenshot_ = false;
    // Embedded-host (JNI) additions: an explicit screenshot destination, an absolute
    // fast-forward request, and the digital pad bits the host currently holds down.
    std::string deferred_screenshot_path_;
    std::optional<bool> deferred_fast_forward_value_{};
    // Digital bits the host currently holds down, PER PLAYER (a multitap carries four
    // pads behind one port). Index 0 is the only one that ever moves without a tap.
    uint32_t host_pad_mask_player_[kHostMaxPlayers] = {};
    // Bits pressed by the PREVIOUS applyHostPadCommands pass — a tap's release is held
    // back until they have aged out, so every press is visible to the guest for two
    // emulated frames (see applyHostPadCommands).
    uint32_t host_pad_pressed_prev_pass_[kHostMaxPlayers] = {};
    FsuiWindowState ui_window_state_ = FsuiWindowState::None;
    bool managed_renderer_vsync_ = DefaultVsyncEnabled();
    uint64_t next_frame_deadline_ = 0;
    uint64_t frame_period_ticks_ = 0;
    // How late the loop arrived for the current frame, sampled in waitForFrameDeadline(). 0
    // whenever the limiter had headroom to sleep. Adaptive frame skip is the only reader.
    uint64_t frame_lateness_ticks_ = 0;
    // Frame skip ([runtime] frame_skip) — see shouldSkipPresent(). frames_skipped_run_ counts
    // consecutive dropped presents and is parked at kMaxFrameSkip to mean "draw the next one";
    // the other two are the diag trace's window.
    int frames_skipped_run_ = kMaxFrameSkip;
    uint32_t frames_dropped_ = 0;
    uint32_t frame_skip_trace_frames_ = 0;
    // ~4 s at 60 Hz. Long enough that the ratio is readable, rare enough that the trace cannot
    // become the thing that costs the frame.
    static constexpr uint32_t kFrameSkipTraceFrames = 240;
    // True while applyPresentVsync() is holding the swap interval down for fast-forward.
    bool fast_forward_vsync_dropped_ = false;
    // Rolling window behind psxe_host_measured_fps() — see publishSessionTelemetry().
    uint64_t fps_window_start_ = 0;
    uint32_t fps_window_frames_ = 0;

    // Rolling window behind psxe_host_stats() — see publishFrameStats(). Everything here is
    // untouched (and the counters unarmed) while the performance overlay is off.
    struct StatsAccumulator {
        uint64_t emu_ticks = 0;
        uint64_t present_ticks = 0;
        uint64_t frame_ticks = 0;
        psx_perf_counters_t counters{};
    };

    StatsAccumulator stats_accum_{};
    uint64_t stats_window_start_ = 0;
    uint64_t stats_prev_frame_begin_ = 0;
    uint64_t stats_worst_frame_ticks_ = 0;
    uint32_t stats_window_frames_ = 0;
};

std::string ModuleNameFromPath(const char* path) {
    if (!path || !path[0]) {
        return "(unknown)";
    }

    return std::filesystem::path(path).filename().string();
}

void WriteNativeStackTraceImpl() {
#if defined(_WIN32)
    void* frames[64] = {};
    const USHORT frame_count = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    psxe_diag_logf("crash", "Native stack trace (%u frames):", static_cast<unsigned int>(frame_count));

#if !defined(UWP_TARGET)
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
#endif

    for (USHORT index = 0; index < frame_count; index++) {
        const DWORD64 address = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(frames[index]));
        HMODULE module = nullptr;
        char module_path[MAX_PATH] = {};
        DWORD64 module_offset = 0;

        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(frames[index]),
                &module) != 0) {
            GetModuleFileNameA(module, module_path, static_cast<DWORD>(std::size(module_path)));
            module_offset = address - static_cast<DWORD64>(reinterpret_cast<uintptr_t>(module));
        }

#if !defined(UWP_TARGET)
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol) != 0) {
            psxe_diag_logf(
                "crash",
                "  #%u %p %s!%s+0x%llx module+0x%llx",
                static_cast<unsigned int>(index),
                frames[index],
                module_path[0] ? ModuleNameFromPath(module_path).c_str() : "(unknown)",
                symbol->Name,
                static_cast<unsigned long long>(displacement),
                static_cast<unsigned long long>(module_offset)
            );
            continue;
        }
#endif

        psxe_diag_logf(
            "crash",
            "  #%u %p %s+0x%llx",
            static_cast<unsigned int>(index),
            frames[index],
            module_path[0] ? ModuleNameFromPath(module_path).c_str() : "(unknown)",
            static_cast<unsigned long long>(module_offset)
        );
    }

#if !defined(UWP_TARGET)
    SymCleanup(process);
#endif
#elif defined(PSXE_HAS_EXECINFO)
    void* frames[64] = {};
    const int frame_count = backtrace(frames, static_cast<int>(std::size(frames)));
    psxe_diag_logf("crash", "Native stack trace (%d frames):", frame_count);

    for (int index = 0; index < frame_count; index++) {
        Dl_info info{};
        if (dladdr(frames[index], &info) != 0 && info.dli_fname) {
            const uintptr_t symbol_offset =
                info.dli_saddr
                    ? (reinterpret_cast<uintptr_t>(frames[index]) - reinterpret_cast<uintptr_t>(info.dli_saddr))
                    : 0u;
            psxe_diag_logf(
                "crash",
                "  #%d %p %s %s+0x%zx",
                index,
                frames[index],
                ModuleNameFromPath(info.dli_fname).c_str(),
                info.dli_sname ? info.dli_sname : "(unknown)",
                symbol_offset
            );
        } else {
            psxe_diag_logf("crash", "  #%d %p", index, frames[index]);
        }
    }
#else
    psxe_diag_logf("crash", "Native stack trace unavailable on this platform build.");
#endif
}

[[noreturn]] void ReportNativeCrash(const char* reason) {
    if (g_crash_reporting.exchange(true)) {
        std::_Exit(1);
    }

    psxe_diag_logf("crash", "Native crash captured: %s", reason ? reason : "(unknown)");

    if (g_active_app) {
        g_active_app->writeCrashContext(reason);
    } else {
        psxe_diag_logf("crash", "No active app context available.");
        psxe_diag_dump_breadcrumbs();
    }

    WriteNativeStackTraceImpl();
    psxe_diag_shutdown();
    std::_Exit(1);
}

} // namespace

extern "C" void psxe_diag_write_native_stacktrace(void) {
    WriteNativeStackTraceImpl();
}

extern "C" int psxe_run(int argc, const char* argv[], void* external_window, void* external_renderer) {
    ArmsxApp app(argc, argv, external_window, external_renderer);
    return app.run();
}

extern "C" PSXE_API void psxe_enqueue_launch_argument(const char* argument) {
    if (!argument || !*argument) {
        return;
    }

    EnqueuePendingLaunchArgument(argument);
}

// ---------------------------------------------------------------------------
// Embedded-host control API (see the g_host_* block near the top of this file).
//
// Every entry point below is safe to call from any thread at any time — including
// before the emulation loop starts and after it has exited. Requests are parked under
// g_host_control_lock and applied by ArmsxApp::applyHostControlRequests() on the
// emulation thread. frontend/android_jni.cpp is the first consumer.
// ---------------------------------------------------------------------------

namespace {

// The embedder's button code space (inherited from the Android/PS2 front-end: Android
// KEYCODE_* values for the pad, plus 200 for the DualShock analog-mode button) mapped onto
// the PS1 pad's PSXI_SW_SDA_* bits.
uint32_t HostPadMaskForCode(int code) {
    switch (code) {
        case 19:  return PSXI_SW_SDA_PAD_UP;      // KEYCODE_DPAD_UP
        case 20:  return PSXI_SW_SDA_PAD_DOWN;    // KEYCODE_DPAD_DOWN
        case 21:  return PSXI_SW_SDA_PAD_LEFT;    // KEYCODE_DPAD_LEFT
        case 22:  return PSXI_SW_SDA_PAD_RIGHT;   // KEYCODE_DPAD_RIGHT
        case 96:  return PSXI_SW_SDA_CROSS;       // KEYCODE_BUTTON_A
        case 97:  return PSXI_SW_SDA_CIRCLE;      // KEYCODE_BUTTON_B
        case 99:  return PSXI_SW_SDA_SQUARE;      // KEYCODE_BUTTON_X
        case 100: return PSXI_SW_SDA_TRIANGLE;    // KEYCODE_BUTTON_Y
        case 102: return PSXI_SW_SDA_L1;          // KEYCODE_BUTTON_L1
        case 103: return PSXI_SW_SDA_R1;          // KEYCODE_BUTTON_R1
        case 104: return PSXI_SW_SDA_L2;          // KEYCODE_BUTTON_L2
        case 105: return PSXI_SW_SDA_R2;          // KEYCODE_BUTTON_R2
        case 106: return PSXI_SW_SDA_L3;          // KEYCODE_BUTTON_THUMBL
        case 107: return PSXI_SW_SDA_R3;          // KEYCODE_BUTTON_THUMBR
        case 108: return PSXI_SW_SDA_START;       // KEYCODE_BUTTON_START
        case 109: return PSXI_SW_SDA_SELECT;      // KEYCODE_BUTTON_SELECT
        case 200: return PSXI_SW_SDA_ANALOG;      // DualShock analog-mode button
        default:  return 0;
    }
}

// Codes 110-113 (left stick) and 120-123 (right stick) arrive as four independent
// per-direction magnitudes in 0..32767. The PS1 pad wants two 0x00..0xFF axes centred on
// 0x80, so the magnitudes are accumulated here and collapsed into one axis pair.
constexpr int kHostStickFullRange = 32767;
// [player][stick][up,right,down,left]. Per player because each pad behind a multitap has
// its own two sticks; sharing one accumulator made player 2's stick move player 1's.
int g_host_stick_dir[kHostMaxPlayers][2][4] = {};

bool HostStickSlotForCode(int code, int* stick, int* slot) {
    switch (code) {
        case 110: *stick = 0; *slot = 0; return true; // left  up
        case 111: *stick = 0; *slot = 1; return true; // left  right
        case 112: *stick = 0; *slot = 2; return true; // left  down
        case 113: *stick = 0; *slot = 3; return true; // left  left
        case 120: *stick = 1; *slot = 0; return true; // right up
        case 121: *stick = 1; *slot = 1; return true; // right right
        case 122: *stick = 1; *slot = 2; return true; // right down
        case 123: *stick = 1; *slot = 3; return true; // right left
        default: return false;
    }
}

int HostAxisByte(int positive, int negative) {
    const int delta = std::clamp(positive, 0, kHostStickFullRange) - std::clamp(negative, 0, kHostStickFullRange);
    const int scaled = (delta * 127) / kHostStickFullRange;
    return std::clamp(0x80 + scaled, 0x00, 0xFF);
}

} // namespace

extern "C" PSXE_API void psxe_host_set_embedded(int enabled) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_embedded = enabled != 0;
}

extern "C" PSXE_API void psxe_host_set_present_callback(void (*callback)(void*), void* user) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_present_callback = callback;
    g_host_present_user = user;
}

extern "C" PSXE_API void psxe_host_set_paused(int paused) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_pause_pending = true;
    g_host_pause_value = paused != 0;
}

// The app went off-screen (backgrounded, or the screen was switched off) / came back.
//
// Closes the SDL audio device outright, which is the only thing that actually stops the stream
// on Android — see ArmsxSession::closeAudioDevice(). Separate from psxe_host_set_paused() because the two are
// genuinely different states: the pause menu freezes the VM with the app still on screen, this
// is the app not being on screen at all. The host front-end normally asks for both.
//
// Honoured (or deliberately ignored) in applyHostControlRequests(), where [audio]
// background_playback is in scope.
extern "C" PSXE_API void psxe_host_set_audio_suspended(int suspended) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_audio_suspend_pending = true;
    g_host_audio_suspend_value = suspended != 0;
}

// 0 = classic 4:3, 1 = square 1:1, 2 = wide 16:9; anything negative drops back to
// settings.toml's [video] display_aspect. Applies on the next presented frame — no VM
// restart, because nothing in the VM depends on it.
// App off-screen: stop drawing and posting frames entirely. Separate from
// psxe_host_set_paused() (the pause menu is on-screen and must keep presenting) and from
// psxe_host_set_audio_suspended() ([audio] background_playback can keep audio alive with the
// screen off). Cheap and immediate — read once per loop tick, gates nothing but presentation.
extern "C" PSXE_API void psxe_host_set_presentation_suspended(int suspended) {
    g_host_presentation_suspended.store(suspended != 0, std::memory_order_release);
}

// Live custom ratio (width/height) used while the mode is 3. <= 0 clears the override and
// falls back to settings.toml's [video] display_aspect_custom. Applies on the next presented
// frame, like the mode itself.
// Live integer-scaling override; negative clears it back to settings.toml. Applies on the
// next presented frame — it only changes the destination rect.
extern "C" PSXE_API void psxe_host_set_portrait_render_top(int enabled) {
    g_host_portrait_top.store(enabled ? 1 : 0, std::memory_order_release);
}

extern "C" PSXE_API void psxe_host_set_portrait_render_top_inset(int pixels) {
    g_host_portrait_top_inset.store(pixels > 0 ? pixels : 0, std::memory_order_release);
}

extern "C" PSXE_API void psxe_host_set_integer_scaling(int enabled) {
    g_host_integer_scaling.store(enabled < 0 ? -1 : (enabled != 0 ? 1 : 0), std::memory_order_release);
}

extern "C" PSXE_API void psxe_host_set_display_aspect_custom(float ratio) {
    g_host_display_aspect_custom.store(ratio > 0.0f ? ratio : 0.0f, std::memory_order_release);
}

extern "C" PSXE_API void psxe_host_set_display_aspect(int mode) {
    g_host_display_aspect.store(mode >= 0 && mode <= 3 ? mode : -1, std::memory_order_release);
}

// 1 = fill the window and ignore the aspect, 0 = letterbox/pillarbox to it, negative =
// defer to settings.toml's [video] stretch_mode.
extern "C" PSXE_API void psxe_host_set_stretch_mode(int enabled) {
    g_host_stretch_mode.store(enabled < 0 ? -1 : (enabled != 0 ? 1 : 0), std::memory_order_release);
}

/* ---- [video] display features, live ------------------------------------------------------
   Each takes a negative value to mean "clear the override and go back to settings.toml".
   All three only affect how the finished frame is presented, so they apply on the very next
   presented frame with no VM interaction at all — safe with no session running. */
extern "C" PSXE_API void psxe_host_set_deinterlace(int mode) {
    g_host_deinterlace.store(mode >= 0 && mode <= 2 ? mode : -1, std::memory_order_release);
}

extern "C" PSXE_API void psxe_host_set_overscan_crop(int mode) {
    g_host_overscan_crop.store(mode >= 0 && mode <= 2 ? mode : -1, std::memory_order_release);
}

/* Degrees (0/90/180/270), matching settings.toml, converted to quarter turns here so every
   caller speaks one language. Anything that is not a right angle clears the override. */
extern "C" PSXE_API void psxe_host_set_display_rotation(int degrees) {
    int turns = -1;

    switch (degrees) {
        case 0:   turns = 0; break;
        case 90:  turns = 1; break;
        case 180: turns = 2; break;
        case 270: turns = 3; break;
        default:  turns = -1; break;
    }

    g_host_display_rotation.store(turns, std::memory_order_release);
}

/* The GTE widescreen hack is CORE state, not presentation: it changes the vertices the
   machine produces, so it is applied straight to psx/cpu.c's global rather than parked as a
   present-time override. Safe while the VM runs — the next projected vertex simply uses the
   new scale, exactly like PGXP's toggle. */
extern "C" PSXE_API void psxe_host_set_widescreen_hack(int enabled) {
    psx_cpu_set_widescreen_hack(enabled ? 1 : 0);
}

/* Texture dumping / replacement (psx/texrep.h). PARKED, not applied: the reconfigure frees
   decoded replacement images and the rasterizers hold pointers into them, so it has to happen
   on the emulation thread. ArmsxApp::applyPendingTextureOptions() drains this once a frame. */
extern "C" PSXE_API void psxe_host_set_texture_options(int dump, int replace, const char* dir) {
    {
        std::lock_guard<std::mutex> lock(g_host_texture_lock);
        g_host_texture_dir = (dir && *dir) ? dir : "";
    }

    g_host_texture_dump.store(dump ? 1 : 0, std::memory_order_release);
    g_host_texture_replace.store(replace ? 1 : 0, std::memory_order_release);
    g_host_texture_pending.store(true, std::memory_order_release);
}

/* GLES rasterizer options. Forwarded verbatim; the backend clamps and owns the semantics. */
extern "C" PSXE_API void psxe_host_set_gl_video_options(int texture_filter, int downsample,
                                                        int line_detect) {
#ifdef USE_HARDWARE
    armsx_hw_gl_set_video_options(texture_filter, downsample, line_detect);
#else
    (void)texture_filter;
    (void)downsample;
    (void)line_detect;
#endif
}

extern "C" PSXE_API void psxe_host_request_shutdown(void) {
    {
        std::lock_guard<std::mutex> lock(g_host_control_lock);
        g_host_shutdown_pending = true;
        // A shutdown must never be blocked behind a stale pause.
        g_host_pause_pending = true;
        g_host_pause_value = false;
        // ...nor behind a stale background-suspend. Shutting down straight out of the background
        // (swipe-kill, Close Game from the pause menu after a screen-off) has to leave the stream
        // un-parked so the teardown below can close it cleanly.
        g_host_audio_suspend_pending = true;
        g_host_audio_suspend_value = false;
    }

    // SDL_PushEvent is thread-safe; this also breaks a loop parked in SDL_PollEvent.
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

extern "C" PSXE_API void psxe_host_request_reset(void) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_reset_pending = true;
}

extern "C" PSXE_API void psxe_host_set_fast_forward(int enabled) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_fast_forward_pending = true;
    g_host_fast_forward_value = enabled != 0;
}

// The whole frame-pacing policy in one call, so the deferred pump needs one flag instead of
// four and a host that changes two of them at once can never have them applied a frame apart.
//   frame_limit         0 runs unthrottled (the front-end's "Frame limit" switch)
//   speed_percent       10..1000, % of the game's own rate ("Emulation speed")
//   fps_limit           extra absolute ceiling in fps, 0 = off ("Frame rate cap")
//   fast_forward_speed  multiplier used instead while fast-forward is engaged, 0 = uncapped
//   frame_skip          presents to DROP, not speed: 0 off, 1..5 fixed, -1 adaptive ("Frame skip")
extern "C" PSXE_API void psxe_host_set_speed_limits(int frame_limit,
                                                    int speed_percent,
                                                    int fps_limit,
                                                    float fast_forward_speed,
                                                    int frame_skip) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_speed_limits_pending = true;
    g_host_speed_limit_frame_limit = frame_limit != 0;
    g_host_speed_limit_percent = speed_percent;
    g_host_speed_limit_fps = fps_limit;
    g_host_speed_limit_fast_forward = static_cast<double>(fast_forward_speed);
    g_host_speed_limit_frame_skip = frame_skip;
}

extern "C" PSXE_API void psxe_host_request_disc_swap(const char* path) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_disc_swap_pending = path && *path;
    g_host_disc_swap_path = (path && *path) ? path : "";
}

extern "C" PSXE_API void psxe_host_request_screenshot(const char* path) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_screenshot_pending = true;
    g_host_screenshot_path = path && *path ? path : "";
}

// Declared by an embedded host once SDLAudioManager's JNI glue AND its static Context are in
// place. Without both, only openslES is safe in-process; see applyAudioDriverSetting().
extern "C" PSXE_API void psxe_host_set_audio_backends_ready(int ready) {
    g_host_audio_backends_ready.store(ready != 0, std::memory_order_release);
}

extern "C" PSXE_API int psxe_host_vm_active(void) {
    return g_host_vm_active.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" PSXE_API int psxe_host_loop_running(void) {
    return g_host_loop_running.load(std::memory_order_acquire) ? 1 : 0;
}

// Session telemetry. Pure reads of what the emulation loop published on its last frame — safe
// from any thread, and 0 whenever nothing is running. See publishSessionTelemetry().
extern "C" PSXE_API float psxe_host_measured_fps(void) {
    return g_host_measured_fps.load(std::memory_order_relaxed);
}

extern "C" PSXE_API float psxe_host_nominal_frame_rate(void) {
    return g_host_nominal_fps.load(std::memory_order_relaxed);
}

extern "C" PSXE_API unsigned int psxe_host_presented_frames(void) {
    return g_host_presented_frames.load(std::memory_order_relaxed);
}

// Arm/disarm the full performance overlay. Also arms psx/perf.c's counters, so switching it
// off really does remove the instrumentation from the core rather than merely hiding the
// numbers. Safe from any thread: psx_perf_set_enabled() only writes a plain int and zeroes a
// counter block the emulation thread will overwrite on its next frame anyway.
//
// IDEMPOTENT, and that is load-bearing rather than tidiness: psx_perf_set_enabled() zeroes the
// counter block on every call and the disable path zeroes the snapshot, so a caller that simply
// re-asserted the state it wanted would keep wiping live measurements. Because it could not,
// the UI had to arm this on an EDGE (a Compose DisposableEffect keyed on "in a game" + "a
// statistics row is on"), while GameOsd.reset() disarms it unconditionally on every VM stop —
// including the stop half of a RESTART. When the STOPPED/RUNNING pair lands inside one
// recomposition the keys never change, the edge never fires, and the counters stay disarmed for
// the rest of the session: every row reads 0 forever while the game plays normally. With this
// a no-op, the overlay can re-assert its arm on the poll tick and the state converges.
extern "C" PSXE_API void psxe_host_set_stats_enabled(int enabled) {
    const bool on = enabled != 0;

    if (g_host_stats_enabled.exchange(on, std::memory_order_relaxed) == on) {
        return;
    }

    psx_perf_set_enabled(on ? 1 : 0);

    if (!on) {
        std::lock_guard<std::mutex> lock(g_host_stats_lock);
        std::memset(g_host_stats, 0, sizeof(g_host_stats));
        g_host_stats_published = false;
    }
}

// Copy the latest snapshot into [out]. Returns the number of doubles written (0 when the
// overlay has never been armed, or before the first window closed). Layout: PSXE_HOST_STAT_*.
extern "C" PSXE_API unsigned int psxe_host_stats(double* out, unsigned int count) {
    if (!out || count == 0) {
        return 0;
    }

    const unsigned int n = count < (unsigned int)PSXE_HOST_STAT_COUNT
        ? count
        : (unsigned int)PSXE_HOST_STAT_COUNT;

    std::lock_guard<std::mutex> lock(g_host_stats_lock);

    /* The documented "0 until the first window closed" — it was documented but never
       implemented, so an unpublished (all-zero) snapshot was handed over as if it were a
       reading, and the UI's own "not ready yet, print --" state could never be reached. */
    if (!g_host_stats_published) {
        return 0;
    }

    std::memcpy(out, g_host_stats, n * sizeof(double));

    return n;
}

extern "C" PSXE_API void psxe_host_pad_button_player(int player, int code, int range, int pressed) {
    const int target = std::clamp(player, 0, kHostMaxPlayers - 1);

    std::lock_guard<std::mutex> lock(g_host_control_lock);

    int stick = 0;
    int slot = 0;
    if (HostStickSlotForCode(code, &stick, &slot)) {
        // range is the direction's magnitude; a release (pressed == 0) zeroes it.
        g_host_stick_dir[target][stick][slot] = pressed ? std::clamp(range, 0, kHostStickFullRange) : 0;

        HostPadCommand command{};
        command.kind = HostPadCommandKind::Analog;
        command.stick = stick;
        command.player = target;
        command.x = HostAxisByte(g_host_stick_dir[target][stick][1], g_host_stick_dir[target][stick][3]);
        command.y = HostAxisByte(g_host_stick_dir[target][stick][2], g_host_stick_dir[target][stick][0]);
        g_host_pad_queue.push_back(command);
        return;
    }

    const uint32_t mask = HostPadMaskForCode(code);
    if (!mask) {
        return;
    }

    HostPadCommand command{};
    command.kind = HostPadCommandKind::Digital;
    command.mask = mask;
    command.pressed = pressed != 0;
    command.player = target;
    g_host_pad_queue.push_back(command);
}

extern "C" PSXE_API void psxe_host_pad_button(int code, int range, int pressed) {
    psxe_host_pad_button_player(0, code, range, pressed);
}

extern "C" PSXE_API void psxe_host_pad_analog_player(int player, int stick, int x, int y) {
    const int target = std::clamp(player, 0, kHostMaxPlayers - 1);

    std::lock_guard<std::mutex> lock(g_host_control_lock);

    HostPadCommand command{};
    command.kind = HostPadCommandKind::Analog;
    command.stick = stick != 0 ? 1 : 0;
    command.player = target;
    command.x = std::clamp(x, 0x00, 0xFF);
    command.y = std::clamp(y, 0x00, 0xFF);
    g_host_pad_queue.push_back(command);
}

extern "C" PSXE_API void psxe_host_pad_analog(int stick, int x, int y) {
    psxe_host_pad_analog_player(0, stick, x, y);
}

extern "C" PSXE_API void psxe_host_reset_pad_state(void) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_pad_queue.clear();
    std::memset(g_host_stick_dir, 0, sizeof(g_host_stick_dir));
}

/* ---- [input] multitap, [emulation] rewind / runahead ------------------------------------
   The first three park a request for the emulation thread (they allocate, free, or rebuild
   the device the SIO reads through). The last two are the hold-to-rewind engagement, which
   is read once per frame straight out of an atomic — see the note by g_host_rewind_active. */

extern "C" PSXE_API void psxe_host_set_multitap(int enabled) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_multitap_pending = true;
    g_host_multitap_value = enabled != 0;
}

extern "C" PSXE_API void psxe_host_set_rewind(int enabled, int seconds, int frequency) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_rewind_config_pending = true;
    g_host_rewind_config_enabled = enabled != 0;
    g_host_rewind_config_seconds = seconds;
    g_host_rewind_config_frequency = frequency;
}

extern "C" PSXE_API void psxe_host_set_runahead(int frames) {
    std::lock_guard<std::mutex> lock(g_host_control_lock);
    g_host_runahead_pending = true;
    g_host_runahead_value = frames;
}

extern "C" PSXE_API void psxe_host_set_rewind_active(int active) {
    g_host_rewind_active.store(active != 0, std::memory_order_release);
}

extern "C" PSXE_API void psxe_host_rewind_step(void) {
    g_host_rewind_step_requests.fetch_add(1, std::memory_order_acq_rel);
}

/* Bytes ONE snapshot takes: measured once a game is running, an estimate before that. The
   front-end multiplies by seconds x frequency to show what the control actually costs. */
extern "C" PSXE_API unsigned int psxe_host_rewind_snapshot_bytes(void) {
    return static_cast<unsigned int>(psx_rewind_snapshot_bytes());
}

/* Bytes the ring is holding right now, so a UI can show the real figure rather than the
   worst case. 0 when rewind is off — nothing is allocated then. */
extern "C" PSXE_API unsigned int psxe_host_rewind_bytes_used(void) {
    return static_cast<unsigned int>(psx_rewind_bytes_used());
}

extern "C" PSXE_API void psxe_wasm_on_file(const char* path) {
    if (!path || !*path) {
        return;
    }

    psxe_enqueue_launch_argument(path);
}

extern "C" PSXE_API void psxe_wasm_on_error(const char* message) {
    const char* text = message && *message ? message : "Unknown browser file error.";
    psxe_diag_logf("web", "Browser file access failed: %s", text);
    EnqueueWebError(text);
}

#if defined(__ANDROID__)
extern "C" JNIEXPORT void JNICALL Java_com_nanodata_armsx_EmulatorActivity_nativeEnqueueLaunchArgument(
    JNIEnv* env,
    jclass,
    jstring argument
) {
    if (!env || !argument) {
        return;
    }

    const char* utf = env->GetStringUTFChars(argument, nullptr);
    if (!utf) {
        return;
    }

    psxe_enqueue_launch_argument(utf);
    env->ReleaseStringUTFChars(argument, utf);
}
#endif

// ---------------------------------------------------------------------------
// Process entry points.
//
// `external_main` MUST keep the plain `int(int, char**)` main signature. SDL's Android
// glue (SDLActivity.nativeRunMain -> the symbol named by getMainFunction(), which
// com.nanodata.armsx.EmulatorActivity sets to "external_main") calls it through a
// SDL_main_func function pointer of exactly that shape. When this used to be the 4-arg
// form, parameters 3 and 4 arrived as whatever happened to be in x2/x3 — garbage that
// initializeWindowAndRenderer() then adopted as an SDL_Window/SDL_Renderer, so
// ArmsxApp::run() bailed out with 1 and the process vanished with no crash and no log.
//
// Hosts that really do own the window/renderer (PSVita, and any future embedder) call
// `external_main_ex` instead.
// ---------------------------------------------------------------------------

extern "C" PSXE_API int external_main_ex(int argc, const char* argv[], void* external_window, void* external_renderer) {
    return psxe_run(argc, argv, external_window, external_renderer);
}

extern "C" PSXE_API int external_main(int argc, const char* argv[]) {
    return psxe_run(argc, argv, nullptr, nullptr);
}

#ifndef __DLL_BUILD
int main(int argc, const char* argv[]) {
    return psxe_run(argc, argv, nullptr, nullptr);
}
#endif
