#include <SDL.h>
#include <SDL_gamecontroller.h>
#include <SDL_render.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
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
#endif

extern "C" {
#include "../psx/psx.h"
#include "../psx/dev/cdrom/cdrom.h"
#include "../psx/dev/gpu.h"
#include "../psx/dev/input.h"
#include "../psx/dev/pad.h"
#include "../psx/dev/timer.h"
#include "../psx/input/sda.h"
#include "common.h"
#include "config.h"
#include "toml.h"
}

#include "archive.h"
#include "IconsFontAwesome5.h"
#ifdef USE_HARDWARE
#include "gpu_hw.h"
#endif
#include "fsui/backend_sdl.hpp"
#include "fsui/fsui.hpp"
#include "fsui/imgui_fullscreen.hpp"
#include "imgui.h"

#undef main

namespace {

constexpr Uint32 kPauseChordGraceMs = 120;
constexpr std::string_view kCustomFsuiAppIconPath = "icons/FsuiAppIcon.png";

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
enum class GpuBackend {
    Software = 0,
    SDLAccelerated = 1,
};
#endif

class ArmsxApp;

ArmsxApp* g_active_app = nullptr;
std::atomic_bool g_crash_reporting{false};
std::mutex g_pending_launch_lock;
std::vector<std::string> g_pending_launch_arguments;
std::vector<std::string> g_pending_web_errors;

constexpr double kUiFrameRate = 60.0;

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
#endif
    bool texture_scale_mode = false;
    bool debug_panel = false;
    bool stretch_mode = false;
    int display_aspect = 0;
    int upscale_height = 480;
    fsui::UiState ui_state{
        .theme = "Dark",
        .prompt_icon_pack = fsui::PromptIconPack::Auto,
        .background_image_path = {},
        .default_game_view = 0,
        .game_sort = 2,
        .game_sort_reverse = false,
        .game_list_paths = {},
        .game_list_recursive_paths = {},
        .covers_path = {},
        .show_inputs_overlay = false,
        .show_settings_overlay = false,
        .show_performance_overlay = false,
    };
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

bool ReadSdlFileBytes(std::string_view path, std::vector<unsigned char>& bytes) {
    SDL_RWops* handle = SDL_RWFromFile(std::string(path).c_str(), "rb");
    if (!handle) {
        return false;
    }

    bytes.clear();
    std::array<unsigned char, 16 * 1024> buffer{};
    for (;;) {
        const size_t read = SDL_RWread(handle, buffer.data(), 1, buffer.size());
        if (read == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    }

    SDL_RWclose(handle);
    return !bytes.empty();
}

std::string MaterializeBundledFsuiAppIcon() {
    const std::filesystem::path destination = DefaultBrowseDirectory() / kCustomFsuiAppIconPath;
    std::error_code ec;

    if (std::filesystem::exists(destination, ec) && std::filesystem::is_regular_file(destination, ec)) {
        return destination.string();
    }

    std::vector<unsigned char> bytes;
    if (!ReadSdlFileBytes(kCustomFsuiAppIconPath, bytes)) {
        return {};
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        return {};
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return {};
    }

    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output.good()) {
        return {};
    }

    return destination.string();
}

std::string ResolveFsuiAppIconPath() {
    const std::filesystem::path relative(kCustomFsuiAppIconPath);
    std::error_code ec;

    auto try_path = [&](const std::filesystem::path& candidate) -> std::string {
        if (candidate.empty()) {
            return {};
        }
        ec.clear();
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.lexically_normal().string();
        }
        return {};
    };

#ifdef PSVITA_TARGET
    const char* vita_base = "app0:/";
    struct VitaBaseDeleter { void operator()(const char*) const {} };
    std::unique_ptr<const char, VitaBaseDeleter> base_path(vita_base);
#else
    std::unique_ptr<char, decltype(&SDL_free)> base_path(SDL_GetBasePath(), &SDL_free);
#endif
    if (base_path) {
        const std::filesystem::path base(base_path.get());

        if (std::string resolved = try_path(base / relative); !resolved.empty()) {
            return resolved;
        }
        if (std::string resolved = try_path((base / ".." / "Resources" / relative).lexically_normal()); !resolved.empty()) {
            return resolved;
        }
    }

    if (std::string resolved = try_path(relative); !resolved.empty()) {
        return resolved;
    }
    if (std::string resolved = try_path(std::filesystem::current_path(ec) / relative); !resolved.empty()) {
        return resolved;
    }
    if (std::string resolved = MaterializeBundledFsuiAppIcon(); !resolved.empty()) {
        return resolved;
    }

    return std::string(kCustomFsuiAppIconPath);
}

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

const char* UpscaleToString(int height) {
    switch (height) {
        case 720: return "720p";
        case 1080: return "1080p";
        case 1440: return "1440p";
        case 2160: return "2160p";
        default: return "480p";
    }
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

const char* GpuBackendTitle(GpuBackend backend) {
    switch (backend) {
        case GpuBackend::SDLAccelerated:
            return "SDL accelerated";
        case GpuBackend::Software:
        default:
            return "SDL software";
    }
}

const char* GpuBackendSettingToken(GpuBackend backend) {
    switch (backend) {
        case GpuBackend::SDLAccelerated:
            return "sdl-accelerated";
        case GpuBackend::Software:
        default:
            return "software";
    }
}

std::optional<GpuBackend> ParseGpuBackendOverride(const char* value) {
    if (!value || !value[0]) {
        return std::nullopt;
    }

    const std::string lowered = ToLower(value);
    if (lowered == "sdl-accelerated" || lowered == "hardware" || lowered == "hardware (experimental)" || lowered == "hw" || lowered == "hw-renderer") {
        return GpuBackend::SDLAccelerated;
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

std::vector<fsui::SettingsChoiceOption> BuildChoices(const std::vector<std::string>& titles, int selected_index) {
    std::vector<fsui::SettingsChoiceOption> options;
    options.reserve(titles.size());

    for (size_t index = 0; index < titles.size(); index++) {
        fsui::SettingsChoiceOption option;
        option.title = titles[index];
        option.selected = (static_cast<int>(index) == selected_index);
        options.push_back(std::move(option));
    }

    return options;
}

std::vector<fsui::SettingsChoiceOption> BuildCpuEngineChoices(psx_cpu_execution_mode_t mode) {
    return BuildChoices(
        {"Cached interpreter", "Reference interpreter"},
        mode == PSX_CPU_INTERPRETER ? 1 : 0
    );
}

#ifdef USE_HARDWARE
std::vector<fsui::SettingsChoiceOption> BuildGpuBackendChoices(GpuBackend current_backend) {
    return BuildChoices(
        {"SDL software", "SDL accelerated"},
        static_cast<int>(current_backend == GpuBackend::SDLAccelerated ? 1 : 0)
    );
}
#endif

std::vector<fsui::SettingsChoiceOption> BuildAspectChoices(int current) {
    return BuildChoices({"Classic", "Square", "Wide 16:9"}, current);
}

std::vector<fsui::SettingsChoiceOption> BuildUpscaleChoices(int current_height) {
    const std::vector<int> heights = {480, 720, 1080, 1440, 2160};
    const std::vector<std::string> labels = {"480p", "720p", "1080p", "1440p", "2160p"};
    int selected = 0;

    for (size_t index = 0; index < heights.size(); index++) {
        if (heights[index] == current_height) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildScaleChoices(int current_scale) {
    const std::vector<int> scales = {1, 2, 3, 4, 5, 6};
    std::vector<std::string> labels;
    labels.reserve(scales.size());

    int selected = 0;
    for (size_t index = 0; index < scales.size(); index++) {
        labels.push_back(std::to_string(scales[index]) + "x");
        if (scales[index] == current_scale) {
            selected = static_cast<int>(index);
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildRegionChoices(const std::string& current_region) {
    const std::vector<std::string> values = {"auto", "ntsc", "pal"};
    const std::vector<std::string> labels = {"Auto", "NTSC", "PAL"};
    const std::string current = ToLower(current_region);
    int selected = 0;

    for (size_t index = 0; index < values.size(); index++) {
        if (values[index] == current) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<std::string> ModelChoices() {
    return {
        "scph1000", "scph1001", "scph1002", "scph3000", "scph3500", "scph5000", "scph5500",
        "scph5501", "scph5502", "scph5552", "scph7000", "scph7001", "scph7002", "scph7003",
        "scph7501", "scph7502", "scph9002", "scph100", "scph101", "scph102a", "scph102b", "scph102c",
    };
}

std::vector<fsui::SettingsChoiceOption> BuildModelChoices(const std::string& current_model) {
    const std::vector<std::string> models = ModelChoices();
    const std::string current = NormalizeModel(current_model);
    int selected = 0;

    for (size_t index = 0; index < models.size(); index++) {
        if (NormalizeModel(models[index]) == current) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(models, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildLogLevelChoices(int current_level) {
    const std::vector<std::string> labels = {"Trace", "Debug", "Info", "Warn", "Error", "Fatal"};
    const int clamped = std::clamp(current_level, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
    return BuildChoices(labels, clamped);
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
            } else {
                settings.display_aspect = 0;
            }
            free(display_aspect.u.s);
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

    if (toml_table_t* library = toml_table_in(root, "library")) {
        CopyTomlArrayStrings(toml_array_in(library, "folders"), settings.ui_state.game_list_paths);
        CopyTomlArrayStrings(toml_array_in(library, "recursive_folders"), settings.ui_state.game_list_recursive_paths);
    }

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
    settings.gpu_backend = cfg && cfg->gpu_backend ? GpuBackend::SDLAccelerated : GpuBackend::Software;
    if (const char* env_backend = std::getenv("ARMSX_GPU_BACKEND")) {
        env_gpu_backend = ParseGpuBackendOverride(env_backend);
    }
#endif
    settings.texture_scale_mode = cfg ? (cfg->texture_scale_mode != 0) : false;
    settings.debug_panel = cfg ? (cfg->debug_panel != 0) : false;
    settings.stretch_mode = cfg ? (cfg->stretch_mode != 0) : false;
    settings.display_aspect = cfg ? cfg->display_aspect : 0;
    settings.upscale_height = cfg ? cfg->upscale_height : 480;
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
    settings.ui_state.show_settings_overlay = settings.debug_panel;
    settings.ui_state.show_performance_overlay = settings.debug_panel;

    if (settings.ui_state.covers_path.empty()) {
        settings.ui_state.covers_path = DefaultBrowseDirectory() / "covers";
    }

    return settings;
}

bool SaveSettings(const FrontendSettings& settings) {
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
        << "    quiet = " << (settings.logging_enabled ? "false" : "true") << "\n\n"
        << "[paths]\n"
        << "    expansion_rom = \"" << EscapeTomlString(settings.exp_path) << "\"\n"
        << "    default_psx_exe = \"" << EscapeTomlString(settings.default_exe_path) << "\"\n\n"
        << "[video]\n"
        << "    vsync = " << (settings.vsync_enabled ? "true" : "false") << "\n"
#ifdef USE_HARDWARE
        << "    gpu_backend = \"" << GpuBackendSettingToken(settings.gpu_backend) << "\"\n"
#endif
        << "    texture_scale_mode = " << (settings.texture_scale_mode ? "true" : "false") << "\n"
        << "    debug_panel = " << (settings.debug_panel ? "true" : "false") << "\n"
        << "    stretch_mode = " << (settings.stretch_mode ? "true" : "false") << "\n"
        << "    display_aspect = \"" << AspectToString(settings.display_aspect) << "\"\n"
        << "    wide_upscale = \"" << UpscaleToString(settings.upscale_height) << "\"\n\n"
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
        const uint32_t value = psx_spu_get_sample(spu);
        const int16_t left = static_cast<int16_t>(value & 0xffff);
        const int16_t right = static_cast<int16_t>(value >> 16);

        *reinterpret_cast<int16_t*>(&buffer[(sample << 2) + 0]) += left;
        *reinterpret_cast<int16_t*>(&buffer[(sample << 2) + 2]) += right;
    }
}

class ArmsxSession {
  public:
    ~ArmsxSession() {
        destroy();
    }

    bool create(SDL_Renderer* renderer, const FrontendSettings& settings, const LaunchRequest& request, std::string& error) {
        destroy();

        if (!renderer) {
            error = "Display is not ready.";
            return false;
        }

        renderer_ = renderer;
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
        psx_gpu_set_event_callback(gpu, GPU_EVENT_DMODE, nullptr);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK, SessionVblankEvent);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK, psxe_gpu_hblank_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK_END, psxe_gpu_vblank_end_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK_END, psxe_gpu_hblank_end_event_cb);
        psx_gpu_set_udata(gpu, 0, this);
        psx_gpu_set_udata(gpu, 1, psx_->timer);
        psx_gpu_set_udata(gpu, 2, nullptr);

#ifdef USE_HARDWARE
        hardware_backend_active_ = settings.gpu_backend == GpuBackend::SDLAccelerated;
        if (hardware_backend_active_) {
            SDL_RendererInfo renderer_info{};
            if (SDL_GetRendererInfo(renderer_, &renderer_info) != 0 ||
                !(renderer_info.flags & SDL_RENDERER_ACCELERATED)) {
                psxe_diag_logf("renderer", "SDL acceleration requested but unavailable; using SDL software presentation.");
                hardware_backend_active_ = false;
            } else {
                psxe_diag_logf(
                    "renderer",
                    "SDL accelerated presentation enabled driver=%s; PlayStation VRAM remains software-authoritative.",
                    renderer_info.name ? renderer_info.name : "(unknown)"
                );
            }
        }
#endif

        psx_cdrom_set_region(psx_get_cdrom(psx_), RegionFromSettings(settings));

        input_ = psx_input_create();
        if (input_) {
            psx_input_init(input_);
        }
        pad_device_ = psxi_sda_create();

        if (!input_ || !pad_device_) {
            error = "Failed to create the controller input bridge.";
            destroy();
            return false;
        }

        psxi_sda_init(pad_device_, SDA_MODEL_DIGITAL);
        psxi_sda_init_input(pad_device_, input_);
        psx_pad_attach_joy(psx_->pad, 0, input_);
        pad_device_ = nullptr;

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
            SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);
            psxe_diag_logf(
                "hw",
                "session-create kind=%s title=%s renderer=%p output=%dx%d texture=%dx%d timing=%s target_fps=%.3f display_mode=0x%08x gpustat=0x%08x",
                LaunchKindTitle(request.kind),
                title_.c_str(),
                (void*)renderer_,
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

        SDL_AudioSpec desired{};
        SDL_AudioSpec obtained{};
        desired.freq = 44100;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = CD_SECTOR_SIZE >> 2;
        desired.callback = AudioUpdate;
        desired.userdata = this;

        audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        paused_ = false;
        fast_forward_enabled_ = false;
        debug_view_ = false;
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        if (audio_dev_) {
            audio_sample_accumulator_ = 0.0;
            audio_queue_.clear();
            audio_queue_read_offset_ = 0;
            updateAudioPlaybackState();
        }
        updateTexture(settings);

        return true;
    }

    void destroy() {
        if (psx_) {
            psxe_diag_breadcrumbf(
                "Session destroyed kind=%s title=%s path=%s",
                LaunchKindTitle(launch_kind_),
                title_.empty() ? "(none)" : title_.c_str(),
                disc_path_.empty() ? (exe_path_.empty() ? "(none)" : exe_path_.string().c_str()) : disc_path_.string().c_str()
            );
        }

        if (audio_dev_) {
            SDL_LockAudioDevice(audio_dev_);
            resetAudioQueueLocked();
            SDL_UnlockAudioDevice(audio_dev_);
            SDL_PauseAudioDevice(audio_dev_, 1);
            SDL_CloseAudioDevice(audio_dev_);
            audio_dev_ = 0;
        }

        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        texture_snapshot_.clear();

#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_destroy(hw_renderer_);
            hw_renderer_ = nullptr;
        }
#endif

        const bool input_attached = psx_ && psx_->pad && (psx_->pad->joy_slot[0] == input_);

        if (psx_) {
            psx_destroy(psx_);
            psx_ = nullptr;
        }

        if (input_ && !input_attached) {
            psx_input_destroy(input_);
        }

        if (pad_device_) {
            psxi_sda_destroy(pad_device_);
            pad_device_ = nullptr;
        }

        input_ = nullptr;
        renderer_ = nullptr;
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

    void rebindRenderer(SDL_Renderer* renderer, const FrontendSettings& settings) {
        renderer_ = renderer;
#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_set_renderer(hw_renderer_, renderer_);
            if (texture_width_ > 0 && texture_height_ > 0) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
        }
#endif
        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        texture_snapshot_.clear();
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        updateTexture(settings);
    }

    void setPaused(bool paused) {
        paused_ = paused;
        if (audio_dev_) {
            if (paused || fast_forward_enabled_) {
                SDL_LockAudioDevice(audio_dev_);
                resetAudioQueueLocked();
                SDL_UnlockAudioDevice(audio_dev_);
            }
            updateAudioPlaybackState();
        }
    }

    bool paused() const {
        return paused_;
    }

    void setFastForwardEnabled(bool enabled) {
        if (fast_forward_enabled_ == enabled) {
            return;
        }

        fast_forward_enabled_ = enabled;
        clearQueuedAudio();

        if (audio_dev_) {
            updateAudioPlaybackState();
        }
    }

    bool fastForwardEnabled() const {
        return fast_forward_enabled_;
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

        const std::uint64_t start_vblank = vblank_counter_;
        std::uint32_t steps = 0;

        while (vblank_counter_ == start_vblank) {
            psx_update(psx_);
            steps++;

            if (steps >= kMaxFrameSteps) {
                psxe_diag_logf("timing", "Session frame advance exceeded vblank step budget title=%s", title_.empty() ? "(none)" : title_.c_str());
#if defined(HW_DEBUG)
                traceHardwareFrameState("frame-budget-exceeded", steps);
#endif
                return steps;
            }
        }

        queueAudioForFrame();

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
        disc_path_ = path;
        launch_kind_ = LaunchKind::Disc;
        title_ = StemToTitle(path);
        psxe_diag_breadcrumbf("Disc swapped path=%s", path.string().c_str());
        psx_soft_reset(psx_);
        return true;
    }

    fsui::CurrentGameInfo currentGameInfo() const {
        fsui::CurrentGameInfo info;

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

    std::vector<fsui::OverlayTextLine> settingsOverlayLines(const FrontendSettings& settings) const {
        std::vector<fsui::OverlayTextLine> lines;
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Aspect: ") + AspectTitle(settings.display_aspect)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Video: ") + RendererValueTitle(settings)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("VSync: ") + BoolTitle(settings.vsync_enabled)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Debug View: ") + BoolTitle(debug_view_)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("Fast Forward: ") + (fast_forward_enabled_ ? "2x" : "Off")});
        return lines;
    }

    std::vector<fsui::OverlayTextLine> performanceOverlayLines(const FrontendSettings& settings) const {
        (void)settings;
        std::vector<fsui::OverlayTextLine> lines;
        char fps[64] = {};
        char video[64] = {};
        std::snprintf(fps, sizeof(fps), "FPS: %.1f", ImGui::GetIO().Framerate);
        std::snprintf(video, sizeof(video), "Video: %dx%d", texture_width_, texture_height_);
        lines.push_back(fsui::OverlayTextLine{.text = fps});
        lines.push_back(fsui::OverlayTextLine{.text = video});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("State: ") + (paused_ ? "Paused" : "Running")});
        if (fast_forward_enabled_) {
            lines.push_back(fsui::OverlayTextLine{.text = "Speed: 2x"});
        }
        return lines;
    }

    void updateTexture(const FrontendSettings& settings) {
        if (!psx_ || !renderer_) {
            return;
        }

        int next_width = debug_view_ ? PSX_GPU_FB_WIDTH : static_cast<int>(psx_get_display_width(psx_));
        int next_height = debug_view_ ? PSX_GPU_FB_HEIGHT : static_cast<int>(psx_get_display_height(psx_));
        if (next_width <= 0) {
            next_width = texture_width_ > 0 ? texture_width_ : 320;
        }
        if (next_height <= 0) {
            next_height = texture_height_ > 0 ? texture_height_ : 240;
        }
        const Uint32 next_format = debug_view_ || !psx_get_display_format(psx_) ? SDL_PIXELFORMAT_BGR555 : SDL_PIXELFORMAT_RGB24;
        const bool use_vram_source = debug_view_ || !psx_ || !psx_->gpu ? false : ((psx_->gpu->disp_y + next_height) > PSX_GPU_FB_HEIGHT);

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            int output_width = 0;
            int output_height = 0;
            SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);
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

        if ((next_width != texture_width_) || (next_height != texture_height_) || (next_format != texture_format_) || !texture_) {
            if (texture_) {
                SDL_DestroyTexture(texture_);
                texture_ = nullptr;
            }

            texture_ = SDL_CreateTexture(renderer_, next_format, SDL_TEXTUREACCESS_STREAMING, next_width, next_height);
            texture_width_ = next_width;
            texture_height_ = next_height;
            texture_format_ = next_format;
            texture_snapshot_.clear();

#ifdef USE_HARDWARE
            if (hw_renderer_) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
#endif
        }

        if (!texture_) {
            return;
        }

        if (settings.texture_scale_mode) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
#endif
        } else {
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeNearest);
#endif
        }

        void* display_buffer = use_vram_source ? psx_get_vram(psx_) : psx_get_display_buffer(psx_);
        const int bytes_per_pixel = SDL_BYTESPERPIXEL(texture_format_);
        const size_t row_bytes = static_cast<size_t>(texture_width_) * static_cast<size_t>(bytes_per_pixel);
        const size_t snapshot_size = static_cast<size_t>(PSX_GPU_FB_STRIDE) * static_cast<size_t>(texture_height_);
        const auto* source = static_cast<const uint8_t*>(display_buffer);

        int first_dirty_row = 0;
        int last_dirty_row = texture_height_ - 1;
        if (texture_snapshot_.size() == snapshot_size) {
            first_dirty_row = texture_height_;
            last_dirty_row = -1;
            for (int row = 0; row < texture_height_; ++row) {
                const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
                if (std::memcmp(source + offset, texture_snapshot_.data() + offset, row_bytes) != 0) {
                    first_dirty_row = std::min(first_dirty_row, row);
                    last_dirty_row = row;
                }
            }
        } else {
            texture_snapshot_.assign(snapshot_size, 0);
        }

        if (last_dirty_row >= first_dirty_row) {
            SDL_Rect dirty_rect{
                0,
                first_dirty_row,
                texture_width_,
                last_dirty_row - first_dirty_row + 1,
            };
            const size_t first_offset = static_cast<size_t>(first_dirty_row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
            SDL_UpdateTexture(texture_, &dirty_rect, source + first_offset, PSX_GPU_FB_STRIDE);
            for (int row = first_dirty_row; row <= last_dirty_row; ++row) {
                const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
                std::memcpy(texture_snapshot_.data() + offset, source + offset, row_bytes);
            }
        }
    }

    void draw(const FrontendSettings& settings) {
        if (!texture_) {
            return;
        }

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
        const float display_width = std::max(display.x, 1.0f);
        const float display_height = std::max(display.y, 1.0f);

        float aspect = 4.0f / 3.0f;
        if (debug_view_) {
            aspect = static_cast<float>(texture_width_) / static_cast<float>(std::max(texture_height_, 1));
        } else if (settings.display_aspect == 2) {
            aspect = 16.0f / 9.0f;
        } else if (settings.display_aspect == 1) {
            aspect = 1.0f;
        } else {
            aspect = static_cast<float>(psx_get_display_aspect(psx_));
        }

        float target_width = display_width;
        float target_height = display_height;
        float offset_x = 0.0f;
        float offset_y = 0.0f;

        if (!settings.stretch_mode) {
            target_width = display_width;
            target_height = target_width / aspect;

            if (target_height > display_height) {
                target_height = display_height;
                target_width = target_height * aspect;
            }

            offset_x = (display_width - target_width) * 0.5f;
            offset_y = (display_height - target_height) * 0.5f;
        }

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        psxe_diag_logf(
            "ui",
            "session-draw frame=%llu display=(%.1f,%.1f) fb_scale=(%.2f,%.2f) texture=%dx%d aspect=%.3f stretch=%s debug=%s target=(%.1f,%.1f) offset=(%.1f,%.1f) hardware=%s timing=%s",
            static_cast<unsigned long long>(vblank_counter_),
            display.x,
            display.y,
            framebuffer_scale.x,
            framebuffer_scale.y,
            texture_width_,
            texture_height_,
            aspect,
            settings.stretch_mode ? "true" : "false",
            debug_view_ ? "true" : "false",
            target_width,
            target_height,
            offset_x,
            offset_y,
#ifdef USE_HARDWARE
            hardware_backend_active_ ? "true" : "false",
#else
            "false",
#endif
            timingModeTitle()
        );
#endif
        draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 255));
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(texture_),
            ImVec2(offset_x, offset_y),
            ImVec2(offset_x + target_width, offset_y + target_height)
        );

#ifdef USE_HARDWARE
        if (!debug_view_ && hw_renderer_) {
            if (SDL_Texture* overlay = armsx_hw_renderer_overlay_texture(hw_renderer_)) {
                draw_list->AddImage(
                    reinterpret_cast<ImTextureID>(overlay),
                    ImVec2(offset_x, offset_y),
                    ImVec2(offset_x + target_width, offset_y + target_height)
                );
            }
        }
#endif
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
            (void*)renderer_
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
        void* source = debug_view_ ? psx_get_vram(psx_) : psx_get_display_buffer(psx_);

        if (!debug_view_ && ((psx_->gpu->disp_y + texture_height_) > PSX_GPU_FB_HEIGHT)) {
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
            std::memcpy(dst + (static_cast<size_t>(row) * static_cast<size_t>(surface->pitch)), src + (static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE)), row_size);
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

    double targetFrameRate() const {
        return frameRate() * (fast_forward_enabled_ ? 2.0 : 1.0);
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

        SDL_PauseAudioDevice(audio_dev_, (paused_ || fast_forward_enabled_) ? 1 : 0);
    }

    void queueAudioForFrame() {
        if (!psx_ || !audio_dev_ || fast_forward_enabled_) {
            return;
        }

        audio_sample_accumulator_ += static_cast<double>(kAudioMixRate) / frameRate();
        const int sample_count = static_cast<int>(audio_sample_accumulator_);
        if (sample_count <= 0) {
            return;
        }

        audio_sample_accumulator_ -= static_cast<double>(sample_count);

        const size_t byte_count = static_cast<size_t>(sample_count) * sizeof(int16_t) * 2;
        if (byte_count == 0) {
            return;
        }

        const size_t queue_before = audio_queue_.size() - audio_queue_read_offset_;
        std::vector<uint8_t> frame_audio(byte_count);
        MixPsxAudio(psx_, frame_audio.data(), static_cast<int>(frame_audio.size()));

        SDL_LockAudioDevice(audio_dev_);
        compactAudioQueueLocked();
        if ((audio_queue_.size() - audio_queue_read_offset_) > kMaxQueuedAudioBytes) {
            resetAudioQueueLocked();
        }
        audio_queue_.insert(audio_queue_.end(), frame_audio.begin(), frame_audio.end());
        const size_t queue_after = audio_queue_.size() - audio_queue_read_offset_;
        SDL_UnlockAudioDevice(audio_dev_);

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            psxe_diag_logf(
                "audio",
                "frame-audio frame=%llu samples=%d bytes=%zu queue_before=%zu queue_after=%zu accumulator=%.3f frame_rate=%.3f fast_forward=%s paused=%s",
                static_cast<unsigned long long>(vblank_counter_),
                sample_count,
                byte_count,
                queue_before,
                queue_after,
                audio_sample_accumulator_,
                frameRate(),
                fast_forward_enabled_ ? "true" : "false",
                paused_ ? "true" : "false"
            );
        }
#endif
    }

    void consumeQueuedAudio(uint8_t* buffer, size_t size) {
        const size_t available = audio_queue_.size() - audio_queue_read_offset_;
        const size_t to_copy = std::min(size, available);

        if (to_copy > 0) {
            std::memcpy(buffer, audio_queue_.data() + audio_queue_read_offset_, to_copy);
            audio_queue_read_offset_ += to_copy;
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
    static constexpr size_t kMaxQueuedAudioBytes = static_cast<size_t>(kAudioMixRate * sizeof(int16_t) * 2 / 2);
    static constexpr size_t kAudioQueueCompactThreshold = 4096;
    static constexpr std::uint32_t kMaxFrameSteps = PSX_CPU_CPS / 8u;

    SDL_Renderer* renderer_ = nullptr;
    psx_t* psx_ = nullptr;
    psx_input_t* input_ = nullptr;
    psxi_sda_t* pad_device_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    std::vector<uint8_t> texture_snapshot_;
#ifdef USE_HARDWARE
    armsx_hw_renderer_t* hw_renderer_ = nullptr;
#endif
    SDL_AudioDeviceID audio_dev_ = 0;
    std::vector<uint8_t> audio_queue_;
    size_t audio_queue_read_offset_ = 0;
    double audio_sample_accumulator_ = 0.0;
    std::filesystem::path disc_path_;
    std::filesystem::path exe_path_;
    std::string title_;
    LaunchKind launch_kind_ = LaunchKind::None;
    bool paused_ = false;
    bool fast_forward_enabled_ = false;
    bool debug_view_ = false;
#ifdef USE_HARDWARE
    bool hardware_backend_active_ = false;
#endif
    std::uint64_t vblank_counter_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
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

    std::vector<fsui::InputOverlayDeviceState> buildInputOverlay() const {
        fsui::InputOverlayDeviceState device;
        device.title = "P1";
        device.bindings = {
            fsui::InputOverlayBinding{.label = "Up", .glyph = "^", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_UP) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Down", .glyph = "v", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_DOWN) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Left", .glyph = "<", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_LEFT) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Right", .glyph = ">", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_RIGHT) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Start", .glyph = "S", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_START) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Select", .glyph = "=", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_SELECT) ? 1.0f : 0.0f},
        };
        return {device};
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
        psxe_diag_initialize(psxe_cfg_get_pref_path());
        psxe_diag_breadcrumbf("ARMSX startup argc=%d", argc_);
        psxe_diag_logf("diag", "Pref path: %s", psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "(none)");

        static bool log_callback_installed = false;
        if (!log_callback_installed) {
            log_add_callback(StructuredLogCallback, nullptr, LOG_TRACE);
            SDL_LogSetOutputFunction(SdlLogOutput, nullptr);
            log_callback_installed = true;
        }

        psxe_config_t* cfg = psxe_cfg_create();
        if (!cfg) {
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

        installCrashHandlers();

        if (!initializeSdl() || !initializeWindowAndRenderer() || !initializeFsui()) {
            shutdown();
            return 1;
        }

        logRendererBootstrap("frontend-init", kUiFrameRate);
        refreshGameList(true);

        g_active_app = this;
        initializeWebLaunchSupport();
        consumePendingLaunchArguments();

        if (cli_.has_boot_request) {
            if (pending_cli_launch_.has_value()) {
                launchSession(*pending_cli_launch_, false);
            } else if (!pending_cli_argument_.empty()) {
                pending_error_dialog_ = "Unsupported launch path or URI.";
            }
        }

        consumePendingLaunchArguments();

        if (!session_.valid()) {
            showLandingWindow();
        }

        fsui::SdlMainLoopCallbacks loop;
        loop.handle_event = [this](const SDL_Event& event) { handleEvent(event); };
        loop.run_frame = [this]() { runFrame(); };
        loop.should_continue = [this]() { return running_; };
        loop.on_exit = [this]() { shutdown(); };
        fsui::RunSdlMainLoop(loop);
        g_active_app = nullptr;

        return 0;
    }

    void writeCrashContext(const char* reason) const {
        const fsui::CurrentGameInfo info = session_.currentGameInfo();
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

    Uint32 managedRendererFlags(bool vsync_enabled) const {
        Uint32 renderer_flags = settings_.gpu_backend == GpuBackend::SDLAccelerated
            ? SDL_RENDERER_ACCELERATED
            : SDL_RENDERER_SOFTWARE;
#if !defined(__EMSCRIPTEN__)
        if (vsync_enabled) {
            renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
        }
#endif
        return renderer_flags;
    }

#ifdef USE_HARDWARE
    bool hardwareRendererAvailable() const {
        if (!owns_renderer_ || external_renderer_ || !renderer_) {
            return false;
        }
        return SupportsHardwareGpuBackend();
    }
#endif

    bool createManagedRenderer(bool vsync_enabled) {
        renderer_ = SDL_CreateRenderer(window_, -1, managedRendererFlags(vsync_enabled));
        owns_renderer_ = renderer_ != nullptr;

        if (!renderer_ && settings_.gpu_backend == GpuBackend::SDLAccelerated) {
            psxe_diag_logf(
                "renderer",
                "SDL accelerated renderer unavailable error=%s; retrying software renderer.",
                SDL_GetError()
            );
            Uint32 fallback_flags = SDL_RENDERER_SOFTWARE;
#if !defined(__EMSCRIPTEN__)
            if (vsync_enabled) {
                fallback_flags |= SDL_RENDERER_PRESENTVSYNC;
            }
#endif
            renderer_ = SDL_CreateRenderer(window_, -1, fallback_flags);
            owns_renderer_ = renderer_ != nullptr;
        }

        if (!renderer_) {
            psxe_diag_logf(
                "renderer",
                "Renderer initialization failed requested_vsync=%s error=%s",
                vsync_enabled ? "true" : "false",
                SDL_GetError()
            );
            return false;
        }

        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer_, &info) == 0) {
            psxe_diag_logf(
                "renderer",
                "Selected SDL renderer driver=%s flags=%s",
                info.name ? info.name : "(unknown)",
                RendererFlagsTitle(info.flags).c_str()
            );
        }

        managed_renderer_vsync_ = vsync_enabled;
        return true;
    }

    void shutdownFsuiFrontend() {
        if (fsui_initialized_) {
            fsui::Shutdown(true);
            fsui_initialized_ = false;
        }

        ImGuiFullscreen::ClearFileSelectorExtraRoots();

        imgui_backend_.shutdown();

        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
    }

    void refreshFileSelectorRoots() {
        std::vector<ImGuiFullscreen::FileSelectorRootEntry> roots{
            ImGuiFullscreen::FileSelectorRootEntry{
                .title = "App Folder",
                .path = DefaultBrowseDirectory().string(),
            },
        };

        ImGuiFullscreen::SetFileSelectorExtraRoots(std::move(roots));
    }

    void showLandingWindow() {
        ui_window_state_ = FsuiWindowState::Landing;
        fsui::ShowLandingWindow();
    }

    void showStartGameWindow() {
        ui_window_state_ = FsuiWindowState::StartGame;
        fsui::ShowStartGameWindow();
    }

    void showExitWindow() {
        ui_window_state_ = FsuiWindowState::Exit;
        fsui::ShowExitWindow();
    }

    void showGameListWindow() {
        ui_window_state_ = FsuiWindowState::GameList;
        fsui::ShowGameListWindow();
    }

    void switchToSettingsWindow() {
        ui_window_state_ = FsuiWindowState::Settings;
        fsui::SwitchToSettings();
    }

    void returnToMainWindow() {
        ui_window_state_ = session_.valid() ? FsuiWindowState::None : FsuiWindowState::Landing;
        fsui::ReturnToMainWindow();
    }

    void showPauseMenuWindow() {
        ui_window_state_ = FsuiWindowState::PauseMenu;
        fsui::OpenPauseMenu();
    }

    void restoreUiWindow() {
        switch (ui_window_state_) {
            case FsuiWindowState::Landing:
                showLandingWindow();
                break;
            case FsuiWindowState::StartGame:
                showStartGameWindow();
                break;
            case FsuiWindowState::Exit:
                showExitWindow();
                break;
            case FsuiWindowState::GameList:
                showGameListWindow();
                break;
            case FsuiWindowState::Settings:
                switchToSettingsWindow();
                break;
            case FsuiWindowState::PauseMenu:
                showPauseMenuWindow();
                break;
            case FsuiWindowState::None:
            default:
                break;
        }
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

            if (renderer_) {
                SDL_DestroyRenderer(renderer_);
                renderer_ = nullptr;
            }
            owns_renderer_ = false;

            if (!createManagedRenderer(previous_vsync) || !initializeFsui()) {
                psxe_diag_logf("renderer", "Renderer recovery failed phase=%s error=%s", phase ? phase : "(none)", SDL_GetError());
                running_ = false;
                return false;
            }

            if (had_session) {
                session_.rebindRenderer(renderer_, settings_);
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

        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        owns_renderer_ = false;

        if (!createManagedRenderer(desired_vsync)) {
            return restore_previous_renderer("renderer-recreate-recover", "Failed to apply the requested VSync mode; restored the previous renderer.");
        }

        if (!initializeFsui()) {
            return restore_previous_renderer("renderer-recreate-recover", "Failed to rebuild the UI after changing VSync; restored the previous renderer.");
        }

        if (had_session) {
            session_.rebindRenderer(renderer_, settings_);
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
        if (reason && reason[0]) {
            psxe_diag_breadcrumbf("Frame pacing reset reason=%s", reason);
        }
#else
        (void)reason;
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
        if (!renderer_) {
            return;
        }

        SDL_RendererInfo info{};
        SDL_GetRendererInfo(renderer_, &info);

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);

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
            info.name ? info.name : "(unknown)",
            RendererFlagsTitle(info.flags).c_str(),
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
        if (ImGui::GetCurrentContext()) {
            const ImGuiIO& io = ImGui::GetIO();
            imgui_display_x = io.DisplaySize.x;
            imgui_display_y = io.DisplaySize.y;
            imgui_scale_x = io.DisplayFramebufferScale.x;
            imgui_scale_y = io.DisplayFramebufferScale.y;
            imgui_font_scale = io.FontGlobalScale;
            imgui_backend_flags = static_cast<int>(io.BackendFlags);
            imgui_config_flags = static_cast<int>(io.ConfigFlags);
        }

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

    bool initializeSdl() {
        const Uint32 required = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;
        psxe_diag_breadcrumbf("Initializing SDL subsystems flags=0x%x", required);

        if (SDL_WasInit(0) == 0) {
            owns_sdl_ = true;
            if (SDL_Init(required) != 0) {
                psxe_diag_logf("sdl", "SDL_Init failed: %s", SDL_GetError());
                return false;
            }
        } else if (SDL_InitSubSystem(required) != 0) {
            psxe_diag_logf("sdl", "SDL_InitSubSystem failed: %s", SDL_GetError());
            return false;
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
            window_ = SDL_CreateWindow(
                "ARMSX",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                window_width,
                window_height,
                flags
            );
            owns_window_ = window_ != nullptr;
        }

        if (!window_) {
            psxe_diag_logf("renderer", "Window initialization failed: %s", SDL_GetError());
            return false;
        }

        if (external_renderer_) {
            renderer_ = external_renderer_;
            owns_renderer_ = false;
        } else {
            renderer_ = nullptr;
            if (!createManagedRenderer(settings_.vsync_enabled)) {
                return false;
            }
        }

        if (!renderer_) {
            psxe_diag_logf("renderer", "Renderer initialization failed: %s", SDL_GetError());
        }

        return renderer_ != nullptr;
    }

    bool initializeFsui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;

        fsui::SdlImGuiBackendConfig backend_config;
        backend_config.window = window_;
        backend_config.sdl_renderer = renderer_;
        backend_config.renderer_backend = fsui::RendererBackend::SDLRenderer;
#if defined(__EMSCRIPTEN__)
        // SDL_Renderer on Emscripten renders correctly when the canvas backing store
        // matches the CSS size instead of a DPR-scaled framebuffer.
        backend_config.web_canvas.max_device_pixel_ratio = 1.0;
#endif

        if (!imgui_backend_.initialize(backend_config)) {
            return false;
        }

#if defined(__EMSCRIPTEN__)
        fsui::SyncSdl2WebCanvasLayout(window_, backend_config.web_canvas, true);
#endif

        fsui::FontStack fonts;
        if (!fsui::BuildDefaultFontStack(fsui::DescribeSdl2FontBootstrap(window_), fonts)) {
            return false;
        }

        fsui::UiContext context;
        context.window = window_;
        context.fonts = fonts;
        context.app_title = "ARMSX";
        context.app_icon_path = ResolveFsuiAppIconPath();
        context.platform_backend = fsui::PlatformBackendKind::SDL2;
        context.renderer_backend = fsui::RendererBackend::SDLRenderer;

        fsui::Host host;
        host.ui_state = &settings_.ui_state;
        host.has_running_game = [this]() { return session_.valid(); };
        host.get_current_game = [this]() { return session_.currentGameInfo(); };
        host.get_game_list = [this]() { return game_list_; };
        host.refresh_game_list = [this](bool full_rescan) { refreshGameList(full_rescan); };
        host.persist_ui_state = [this](bool reload) {
            if (reload) {
                refreshGameList(true);
            }
            SaveSettings(settings_);
        };
        host.set_paused = [this](bool paused) {
            if (!session_.valid()) {
                return;
            }
            session_.setPaused(paused);
            resetFramePacing(paused ? "fsui-pause" : "fsui-unpause");
            if (paused) {
                input_router_.onFsuiOpened();
            }
        };
        host.resume_game = [this]() {
            if (session_.valid()) {
                session_.setPaused(false);
                resetFramePacing("fsui-resume");
            }
        };
        host.reset_system = [this]() {
            if (session_.valid()) {
                session_.reset();
                resetFramePacing("fsui-reset");
            }
        };
        host.exit_to_library = [this]() { exitToLibrary(); };
        host.request_quit = [this]() { running_ = false; };
        host.request_classic_ui = []() {};
        host.close_selector = []() {};
        host.detect_prompt_icon_pack = []() { return fsui::DetectPromptIconPackFromSDL(); };
        host.detect_swap_north_west_gamepad_buttons = []() { return false; };
        host.runtime_overlay_options = fsui::RuntimeOverlayOptions{
            .show_inputs = true,
            .show_settings = true,
            .show_performance = true,
        };
        host.get_input_overlay_devices = [this]() { return input_router_.buildInputOverlay(); };
        host.get_settings_overlay_lines = [this]() { return session_.settingsOverlayLines(settings_); };
        host.get_performance_overlay_lines = [this]() { return session_.performanceOverlayLines(settings_); };
        host.get_landing_items = [this]() { return buildLandingItems(); };
        host.get_start_items = [this]() { return buildStartItems(); };
        host.get_exit_items = [this]() { return buildExitItems(); };
        host.get_pause_items = [this]() { return buildPauseItems(); };
        host.get_game_launch_options = [this](const fsui::GameEntry& entry) { return buildGameLaunchOptions(entry); };
        host.get_settings_pages = [this](fsui::SettingsScope scope) { return buildSettingsPages(scope); };

        context.host = std::move(host);
        refreshFileSelectorRoots();

        if (!fsui::Initialize(context)) {
            return false;
        }

        fsui_initialized_ = true;
        return true;
    }

    void shutdown() {
        psxe_diag_breadcrumbf("Frontend shutdown");
        input_router_.detach();
        session_.destroy();

        shutdownFsuiFrontend();

        if (owns_renderer_ && renderer_) {
            SDL_DestroyRenderer(renderer_);
        }
        if (owns_window_ && window_) {
            SDL_DestroyWindow(window_);
        }

        renderer_ = nullptr;
        window_ = nullptr;

        if (owns_sdl_) {
            SDL_Quit();
        }

        psxe_diag_shutdown();
    }

    void handleEvent(const SDL_Event& event) {
        imgui_backend_.processEvent(event);

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

        const bool fsui_active = fsui::HasActiveWindow();
        input_router_.processEvent(event, session_.valid() ? &session_ : nullptr, fsui_active);

        if (event.type == SDL_KEYDOWN && !fsui_active && session_.valid() && event.key.repeat == 0 && event.key.keysym.sym == SDLK_RETURN) {
            psx_exp2_atcons_put(session_.psx()->exp2, 13);
        }

        if (session_.valid() && !fsui_active && input_router_.takePauseRequest()) {
            openPauseMenu();
        }
    }

    void runFrame() {
        const bool fsui_was_active = fsui::HasActiveWindow();
        input_router_.tick(fsui_was_active);

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

        waitForFrameDeadline(currentTargetFrameRate());

        std::uint32_t session_steps = 0;
        if (session_.valid() && !session_.paused()) {
            session_steps = session_.runFrame();
            session_.updateTexture(settings_);
            session_.finishHardwareFrame();
        }

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-pre-imgui");
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
        imgui_backend_.newFrame();
#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-post-imgui-backend");
        }
#endif
        ImGui::NewFrame();

        if (session_.valid()) {
            session_.draw(settings_);
        }

        if (pending_error_dialog_.has_value()) {
            ImGuiFullscreen::OpenInfoMessageDialog("ARMSX", *pending_error_dialog_);
            pending_error_dialog_.reset();
        }

        fsui::Render();

        for (const fsui::Command& command : fsui::ConsumeCommands()) {
            applyCommand(command);
        }

        applyDeferredActions();

        ImGui::Render();
        imgui_backend_.renderDrawData(ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-post-imgui-render");
        }
#endif

        const bool fsui_is_active = fsui::HasActiveWindow();

        if (fsui_was_active && !fsui_is_active) {
            ui_window_state_ = FsuiWindowState::None;
            if (session_.valid()) {
                input_router_.onFsuiClosed();
            }
        } else if (!fsui_was_active && fsui_is_active && session_.valid()) {
            input_router_.onFsuiOpened();
        }

        advanceFrameDeadline();
    }

    void applyCommand(const fsui::Command& command) {
        psxe_diag_breadcrumbf("Frontend command type=%d", static_cast<int>(command.type));

        switch (command.type) {
            case fsui::CommandType::LaunchPath:
                queueLaunchRequest(LaunchForPath(command.path), true);
                break;

            case fsui::CommandType::RequestQuit:
                if (session_.valid()) {
                    exitToLibrary();
                } else {
                    running_ = false;
                }
                break;

            case fsui::CommandType::Resume:
                if (session_.valid()) {
                    ui_window_state_ = FsuiWindowState::None;
                    session_.setPaused(false);
                    resetFramePacing("command-resume");
                }
                break;

            case fsui::CommandType::Reset:
                if (session_.valid()) {
                    session_.reset();
                    session_.setPaused(false);
                    resetFramePacing("command-reset");
                }
                break;

            case fsui::CommandType::ExitToLibrary:
                exitToLibrary();
                break;

            case fsui::CommandType::CloseSelector:
            case fsui::CommandType::ExitFullscreenUI:
            case fsui::CommandType::SwitchShell:
            case fsui::CommandType::Custom:
            default:
                break;
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
                returnToMainWindow();
            }
        }

        if (deferred_screenshot_) {
            deferred_screenshot_ = false;

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

        if (deferred_fast_forward_toggle_) {
            deferred_fast_forward_toggle_ = false;
            if (session_.valid()) {
                session_.setFastForwardEnabled(!session_.fastForwardEnabled());
                session_.setPaused(false);
                resetFramePacing("toggle-fast-forward");
                returnToMainWindow();
            }
        }
    }

    bool launchSession(const LaunchRequest& request, bool close_ui) {
        std::string error;
        psxe_diag_breadcrumbf("Launching session kind=%s path=%s close_ui=%s",
            LaunchKindTitle(request.kind),
            request.path.empty() ? "(none)" : request.path.string().c_str(),
            close_ui ? "true" : "false");

        if (!session_.create(renderer_, settings_, request, error)) {
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

        if (close_ui) {
            returnToMainWindow();
        }

        return true;
    }

    void exitToLibrary() {
        psxe_diag_breadcrumbf("Exit to library requested");
        input_router_.detach();
        session_.destroy();
        resetFramePacing("exit-to-library");
        showLandingWindow();
    }

    void openPauseMenu() {
        if (!session_.valid()) {
            return;
        }

        session_.setPaused(true);
        resetFramePacing("open-pause-menu");
        input_router_.onFsuiOpened();
        showPauseMenuWindow();
    }

    void refreshGameList(bool full_rescan) {
        (void)full_rescan;
        game_list_.clear();
        std::vector<std::filesystem::path> seen_game_paths;
        std::vector<std::pair<std::filesystem::path, bool>> scan_roots;

        auto add_scan_root = [&](const std::filesystem::path& root, bool recursive) {
            if (root.empty()) {
                return;
            }

            for (auto& entry : scan_roots) {
                if (PathsMatch(entry.first, root)) {
                    entry.second = entry.second || recursive;
                    return;
                }
            }

            scan_roots.emplace_back(root, recursive);
        };

        auto scan_root = [&](const std::filesystem::path& root, bool recursive) {
            try {
                if (!std::filesystem::exists(root)) {
                    return;
                }

                auto append_entry = [&](const std::filesystem::directory_entry& item) {
                    if (!item.is_regular_file()) {
                        return;
                    }

                    const std::filesystem::path path = item.path();
                    if (!IsDiscPath(path) && !IsExePath(path)) {
                        return;
                    }
                    if (IsLikelyBiosImagePath(path, settings_)) {
                        return;
                    }
                    if (ToLower(path.extension().string()) == ".bin" && CueDirectoryReferencesImage(path)) {
                        return;
                    }
                    if (PathListContains(seen_game_paths, path)) {
                        return;
                    }
                    seen_game_paths.push_back(path);

                    fsui::GameEntry entry;
                    entry.path = path;
                    entry.type = IsExePath(path) ? fsui::GameEntryType::Homebrew : fsui::GameEntryType::Application;
                    entry.title = StemToTitle(path);
                    entry.title_sort = ToLower(entry.title);
                    entry.english_title = entry.title;
                    entry.title_id = NormalizeModel(entry.title);
                    entry.file_size = item.file_size();
                    entry.modified_time = FileTimeToTimeT(item.last_write_time());

                    const int region = RegionFromSettings(settings_);
                    if (region == CDR_REGION_EUROPE) {
                        entry.region = "Europe";
                        entry.region_flag_path = "icons/flags/eu.png";
                    } else if (region == CDR_REGION_JAPAN) {
                        entry.region = "Japan";
                        entry.region_flag_path = "icons/flags/jp.png";
                    } else {
                        entry.region = "North America";
                        entry.region_flag_path = "icons/flags/us.png";
                    }

                    game_list_.push_back(std::move(entry));
                };

                if (recursive) {
                    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
                        append_entry(item);
                    }
                } else {
                    for (const auto& item : std::filesystem::directory_iterator(root)) {
                        append_entry(item);
                    }
                }
            } catch (...) {
            }
        };

        for (const auto& root : settings_.ui_state.game_list_paths) {
            add_scan_root(root, false);
        }

        for (const auto& root : settings_.ui_state.game_list_recursive_paths) {
            add_scan_root(root, true);
        }

#if defined(UWP_TARGET)
        add_scan_root(DefaultBrowseDirectory(), true);
#endif

        for (const auto& [root, recursive] : scan_roots) {
            scan_root(root, recursive);
        }

        std::sort(game_list_.begin(), game_list_.end(), [](const fsui::GameEntry& left, const fsui::GameEntry& right) {
            if (left.title_sort == right.title_sort) {
                return left.path < right.path;
            }
            return left.title_sort < right.title_sort;
        });
    }

    std::vector<fsui::MenuItemDescriptor> buildLandingItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor game_list;
        game_list.id = "game-list";
        game_list.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::GameList);
        game_list.title = "Game List";
        game_list.summary = "Browse your library folders and open a game.";
        game_list.on_activate = [this]() { showGameListWindow(); };
        items.push_back(std::move(game_list));

        fsui::MenuItemDescriptor start;
        start.id = "start-game";
        start.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartGame);
        start.title = "Start Game";
        start.summary = "Boot a disc, PS-X EXE, or the BIOS directly.";
        start.on_activate = [this]() { showStartGameWindow(); };
        items.push_back(std::move(start));

        fsui::MenuItemDescriptor settings;
        settings.id = "settings";
        settings.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Settings);
        settings.title = "Settings";
        settings.summary = "Adjust ARMSX, BIOS, video, and library settings.";
        settings.on_activate = [this]() { switchToSettingsWindow(); };
        items.push_back(std::move(settings));

        fsui::MenuItemDescriptor exit;
        exit.id = "exit";
        exit.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Exit);
        exit.title = "Exit";
        exit.summary = "Quit ARMSX.";
        exit.on_activate = [this]() { showExitWindow(); };
        items.push_back(std::move(exit));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildStartItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        auto default_dir = initialBrowseDirectory();

        fsui::MenuItemDescriptor start_file;
        start_file.id = "start-file";
        start_file.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
        start_file.title = "Start File";
        start_file.summary = "Choose a disc image or PS-X EXE from your files.";
        start_file.on_activate = [this, default_dir]() {
#if defined(__EMSCRIPTEN__)
            requestWebFiles(false);
#else
            ImGuiFullscreen::OpenFileSelector(
                "Start File",
                false,
                [this](const std::string& path) {
                    queueLaunchSelection(path);
                },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                 ".exe", ".ps-exe", ".psexe"},
                default_dir.string()
            );
#endif
        };
        items.push_back(std::move(start_file));

        if (!settings_.default_exe_path.empty()) {
            fsui::MenuItemDescriptor start_saved_exe;
            start_saved_exe.id = "start-default-exe";
            start_saved_exe.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
            start_saved_exe.title = "Start Saved PS-X EXE";
            start_saved_exe.summary = settings_.default_exe_path;
            start_saved_exe.on_activate = [this]() {
                deferred_launch_ = LaunchRequest{
                    .kind = LaunchKind::Exe,
                    .path = std::filesystem::path(settings_.default_exe_path),
                    .label = StemToTitle(settings_.default_exe_path),
                };
                close_ui_after_launch_ = true;
            };
            items.push_back(std::move(start_saved_exe));
        }

        fsui::MenuItemDescriptor start_disc;
        start_disc.id = "start-disc";
        start_disc.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartDisc);
        start_disc.title = "Start Disc";
        start_disc.summary = "Pick a disc image directly.";
        start_disc.on_activate = [this, default_dir]() {
#if defined(__EMSCRIPTEN__)
            requestWebFiles(false);
#else
            ImGuiFullscreen::OpenFileSelector(
                "Start Disc",
                false,
                [this](const std::string& path) {
                    queueLaunchSelection(path, LaunchKind::Disc);
                },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                default_dir.string()
            );
#endif
        };
        items.push_back(std::move(start_disc));

#if defined(__EMSCRIPTEN__)
        fsui::MenuItemDescriptor start_folder;
        start_folder.id = "start-web-folder";
        start_folder.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
        start_folder.title = "Open Game Folder";
        start_folder.summary = "Grant temporary access to a folder and start a discovered game.";
        start_folder.on_activate = [this]() { requestWebFiles(true); };
        items.push_back(std::move(start_folder));
#endif

        fsui::MenuItemDescriptor start_bios;
        start_bios.id = "start-bios";
        start_bios.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartBios);
        start_bios.title = "Start BIOS";
        start_bios.summary = "Boot into the BIOS without media.";
        start_bios.on_activate = [this]() {
            deferred_launch_ = LaunchRequest{.kind = LaunchKind::Bios, .path = {}, .label = "PlayStation BIOS"};
            close_ui_after_launch_ = true;
        };
        items.push_back(std::move(start_bios));

        fsui::MenuItemDescriptor back;
        back.id = "back";
        back.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Back);
        back.title = "Back";
        back.summary = "Return to the main menu.";
        back.on_activate = [this]() { showLandingWindow(); };
        items.push_back(std::move(back));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildExitItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor quit;
        quit.id = "quit";
        quit.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Exit);
        quit.title = "Quit";
        quit.summary = "Close ARMSX.";
        quit.command = fsui::MakeRequestQuitCommand();
        items.push_back(std::move(quit));

        fsui::MenuItemDescriptor back;
        back.id = "back";
        back.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Back);
        back.title = "Back";
        back.summary = "Return to the main menu.";
        back.on_activate = [this]() { showLandingWindow(); };
        items.push_back(std::move(back));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildPauseItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor resume;
        resume.id = "resume";
        resume.title = ICON_FA_PLAY " Resume";
        resume.summary = "Return to gameplay.";
        resume.command = fsui::MakeResumeCommand();
        items.push_back(std::move(resume));

        fsui::MenuItemDescriptor reset;
        reset.id = "reset";
        reset.title = ICON_FA_SYNC " Reset";
        reset.summary = "Soft-reset the active session.";
        reset.command = fsui::MakeResetCommand();
        items.push_back(std::move(reset));

        fsui::MenuItemDescriptor change_disc;
        change_disc.id = "change-disc";
        change_disc.title = ICON_FA_COMPACT_DISC " Change Disc";
        change_disc.summary = "Swap to another disc image and resume.";
        change_disc.on_activate = [this]() {
            ImGuiFullscreen::OpenFileSelector(
                "Change Disc",
                false,
                [this](const std::string& path) { queueDiscSwapSelection(path); },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                initialBrowseDirectory().string()
            );
        };
        items.push_back(std::move(change_disc));

        fsui::MenuItemDescriptor screenshot;
        screenshot.id = "screenshot";
        screenshot.title = ICON_FA_CAMERA " Save Screenshot";
        screenshot.summary = "Save a BMP screenshot to the screenshot folder.";
        screenshot.on_activate = [this]() { deferred_screenshot_ = true; };
        items.push_back(std::move(screenshot));

        fsui::MenuItemDescriptor settings;
        settings.id = "settings";
        settings.title = ICON_FA_SLIDERS_H " Settings";
        settings.summary = "Open the in-game settings pages.";
        settings.on_activate = [this]() { switchToSettingsWindow(); };
        items.push_back(std::move(settings));

        fsui::MenuItemDescriptor fast_forward;
        fast_forward.id = "fast-forward";
        fast_forward.title = ICON_FA_FAST_FORWARD " Fast Forward";
        fast_forward.summary = session_.fastForwardEnabled()
            ? "Current speed is 2x. Click to return to normal speed and resume gameplay."
            : "Current speed is normal. Click to resume gameplay at 2x.";
        fast_forward.on_activate = [this]() { deferred_fast_forward_toggle_ = true; };
        items.push_back(std::move(fast_forward));

        fsui::MenuItemDescriptor quit;
        quit.id = "quit-game";
        quit.title = ICON_FA_POWER_OFF " Quit Game";
        quit.summary = "Close the current game and return to the main menu.";
        quit.command = fsui::MakeExitToLibraryCommand();
        items.push_back(std::move(quit));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildGameLaunchOptions(const fsui::GameEntry&) {
        return {};
    }

    std::vector<fsui::SettingsPageDescriptor> buildSettingsPages(fsui::SettingsScope scope) {
        std::vector<fsui::SettingsPageDescriptor> pages;

        if (scope == fsui::SettingsScope::PerGame) {
            pages = buildPerGameSettingsPages();
        } else {
            pages = buildGlobalSettingsPages();
        }

        if (pending_settings_page_restore_.has_value()) {
            const std::string restore_id = *pending_settings_page_restore_;
            const auto match = std::find_if(pages.begin(), pages.end(), [&](const fsui::SettingsPageDescriptor& page) {
                return page.id == restore_id;
            });

            if (match != pages.end()) {
                std::rotate(pages.begin(), match, match + 1);
            }

            pending_settings_page_restore_.reset();
        }

        return pages;
    }

    std::vector<fsui::SettingsPageDescriptor> buildGlobalSettingsPages() {
        std::vector<fsui::SettingsPageDescriptor> pages;

        fsui::SettingsPageDescriptor bios_page;
        bios_page.id = "BIOS";
        bios_page.scope = fsui::SettingsScope::Global;
        bios_page.built_in_page = fsui::BuiltInSettingsPage::BIOS;
        bios_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor model;
            model.kind = fsui::SettingsRowKind::Choice;
            model.id = "model";
            model.title = ICON_FA_MICROCHIP " Model";
            model.summary = "Choose which BIOS model to use when a BIOS folder contains more than one.";
            model.value = settings_.model;
            model.dialog_title = model.title;
            model.choices = BuildModelChoices(settings_.model);
            model.on_choice = [this](int index) {
                const auto models = ModelChoices();
                if (index >= 0 && index < static_cast<int>(models.size())) {
                    settings_.model = models[static_cast<size_t>(index)];
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(model));

            fsui::SettingsRowDescriptor folder;
            folder.kind = fsui::SettingsRowKind::Action;
            folder.id = "bios-folder";
            folder.title = ICON_FA_FOLDER " BIOS Folder";
            if (settings_.bios_search.empty()) {
                folder.summary = AppendBrowseRootHint("Choose a folder that contains BIOS files.");
            } else {
                folder.summary = settings_.bios_search;
            }
            folder.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "BIOS Folder",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.bios_search = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    browseDirectoryForPath(settings_.bios_search, true).string()
                );
            };
            rows.push_back(std::move(folder));

            fsui::SettingsRowDescriptor override;
            override.kind = fsui::SettingsRowKind::Action;
            override.id = "bios-override";
            override.title = ICON_FA_FILE " BIOS Override";
            override.summary = settings_.bios_override.empty() ? "Auto-select from the BIOS folder." : settings_.bios_override;
            override.on_activate = [this]() {
                const std::string browse_seed = settings_.bios_override.empty() ? settings_.bios_search : settings_.bios_override;
                ImGuiFullscreen::OpenFileSelector(
                    "BIOS Override",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.bios_override = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".bin", ".rom"},
                    browseDirectoryForPath(browse_seed, false).string()
                );
            };
            rows.push_back(std::move(override));

            fsui::SettingsRowDescriptor clear_override;
            clear_override.kind = fsui::SettingsRowKind::Action;
            clear_override.id = "clear-bios-override";
            clear_override.title = ICON_FA_TIMES " Clear BIOS Override";
            clear_override.summary = "Fall back to the BIOS folder + preferred model.";
            clear_override.enabled = !settings_.bios_override.empty();
            clear_override.on_activate = [this]() {
                settings_.bios_override.clear();
                SaveSettings(settings_);
            };
            rows.push_back(std::move(clear_override));

            return rows;
        };
        pages.push_back(std::move(bios_page));

        fsui::SettingsPageDescriptor emulation_page;
        emulation_page.id = "Emulation";
        emulation_page.scope = fsui::SettingsScope::Global;
        emulation_page.built_in_page = fsui::BuiltInSettingsPage::Emulation;
        emulation_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor cpu_engine;
            cpu_engine.kind = fsui::SettingsRowKind::Choice;
            cpu_engine.id = "cpu-engine";
            cpu_engine.title = ICON_FA_MICROCHIP " CPU Engine";
            cpu_engine.summary = "Use the cached interpreter for normal play or the reference interpreter for compatibility diagnostics. Applies on the next launch.";
            cpu_engine.value = CpuEngineTitle(settings_.cpu_engine);
            cpu_engine.dialog_title = cpu_engine.title;
            cpu_engine.choices = BuildCpuEngineChoices(settings_.cpu_engine);
            cpu_engine.on_choice = [this](int index) {
                settings_.cpu_engine = index == 1 ? PSX_CPU_INTERPRETER : PSX_CPU_CACHED_INTERPRETER;
                SaveSettings(settings_);
            };
            rows.push_back(std::move(cpu_engine));

            fsui::SettingsRowDescriptor region;
            region.kind = fsui::SettingsRowKind::Choice;
            region.id = "region";
            region.title = ICON_FA_GLOBE " Region";
            region.summary = "Choose which region the console should use when starting a disc.";
            region.value = ToLower(settings_.region);
            region.dialog_title = region.title;
            region.choices = BuildRegionChoices(settings_.region);
            region.on_choice = [this](int index) {
                static const std::array<const char*, 3> values = {"auto", "ntsc", "pal"};
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    settings_.region = values[static_cast<size_t>(index)];
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(region));

            fsui::SettingsRowDescriptor expansion;
            expansion.kind = fsui::SettingsRowKind::Action;
            expansion.id = "expansion-rom";
            expansion.title = ICON_FA_MEMORY " Expansion ROM";
            expansion.summary = settings_.exp_path.empty() ? "No expansion ROM selected." : settings_.exp_path;
            expansion.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Expansion ROM",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.exp_path = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".bin", ".rom"},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(expansion));

            fsui::SettingsRowDescriptor default_exe;
            default_exe.kind = fsui::SettingsRowKind::Action;
            default_exe.id = "default-exe";
            default_exe.title = ICON_FA_FILE_CODE " Default PS-X EXE";
            default_exe.summary = settings_.default_exe_path.empty() ? "No default executable selected." : settings_.default_exe_path;
            default_exe.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Default PS-X EXE",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.default_exe_path = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".exe", ".ps-exe", ".psexe"},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(default_exe));

            return rows;
        };
        pages.push_back(std::move(emulation_page));

        fsui::SettingsPageDescriptor graphics_page;
        graphics_page.id = "Graphics";
        graphics_page.scope = fsui::SettingsScope::Global;
        graphics_page.built_in_page = fsui::BuiltInSettingsPage::Graphics;
        graphics_page.build_rows = [this]() { return buildGraphicsRows(SupportsManagedWindowSizing()); };
        pages.push_back(std::move(graphics_page));

        fsui::SettingsPageDescriptor folders_page;
        folders_page.id = "Folders";
        folders_page.scope = fsui::SettingsScope::Global;
        folders_page.built_in_page = fsui::BuiltInSettingsPage::Folders;
        folders_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

#if defined(UWP_TARGET)
            fsui::SettingsRowDescriptor browse_notice;
            browse_notice.kind = fsui::SettingsRowKind::Notice;
            browse_notice.id = "uwp-filesystem-roots";
            browse_notice.title = ICON_FA_FOLDER_OPEN " Mounted Drives";
            browse_notice.summary = "Drive roots are available in the file picker. Use Filesystem Roots or Parent Directory to switch drives.";
            rows.push_back(std::move(browse_notice));
#endif

            fsui::SettingsRowDescriptor add_folder;
            add_folder.kind = fsui::SettingsRowKind::Action;
            add_folder.id = "add-folder";
            add_folder.title = ICON_FA_FOLDER_PLUS " Add Library Folder";
            add_folder.summary = AppendBrowseRootHint("Scan only the selected directory.");
            add_folder.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Add Library Folder",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.ui_state.game_list_paths.emplace_back(*selection);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(add_folder));

            fsui::SettingsRowDescriptor add_recursive;
            add_recursive.kind = fsui::SettingsRowKind::Action;
            add_recursive.id = "add-recursive-folder";
            add_recursive.title = ICON_FA_FOLDER_PLUS " Add Recursive Folder";
            add_recursive.summary = AppendBrowseRootHint("Scan the selected directory and its children.");
            add_recursive.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Add Recursive Library Folder",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.ui_state.game_list_recursive_paths.emplace_back(*selection);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(add_recursive));

            fsui::SettingsRowDescriptor refresh;
            refresh.kind = fsui::SettingsRowKind::Action;
            refresh.id = "refresh-library";
            refresh.title = ICON_FA_SYNC " Refresh Library";
            refresh.summary = "Rescan every configured library folder.";
            refresh.on_activate = [this]() { refreshGameList(true); };
            rows.push_back(std::move(refresh));

            auto append_folder_rows = [&](const std::vector<std::filesystem::path>& folders, bool recursive) {
                for (size_t index = 0; index < folders.size(); index++) {
                    fsui::SettingsRowDescriptor row;
                    row.kind = fsui::SettingsRowKind::Action;
                    row.id = std::string(recursive ? "recursive-" : "folder-") + std::to_string(index);
                    row.title = WithHiddenId(
                        recursive ? ICON_FA_FOLDER_OPEN " Recursive Folder" : ICON_FA_FOLDER " Library Folder",
                        row.id
                    );
                    row.summary = folders[index].string();
                    row.on_activate = [this, index, recursive]() {
                        auto& list = recursive ? settings_.ui_state.game_list_recursive_paths : settings_.ui_state.game_list_paths;
                        if (index < list.size()) {
                            list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    };
                    rows.push_back(std::move(row));
                }
            };

            append_folder_rows(settings_.ui_state.game_list_paths, false);
            append_folder_rows(settings_.ui_state.game_list_recursive_paths, true);

            return rows;
        };
        pages.push_back(std::move(folders_page));

        fsui::SettingsPageDescriptor advanced_page;
        advanced_page.id = "Advanced";
        advanced_page.scope = fsui::SettingsScope::Global;
        advanced_page.built_in_page = fsui::BuiltInSettingsPage::Advanced;
        advanced_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor logging_enabled;
            logging_enabled.kind = fsui::SettingsRowKind::Toggle;
            logging_enabled.id = "logging-enabled";
            logging_enabled.title = ICON_FA_BUG " Debug Logging";
            logging_enabled.summary =
                "Write diagnostic logs for troubleshooting only. Leave this off during normal play because it can reduce performance.";
            logging_enabled.toggle_value = settings_.logging_enabled;
            logging_enabled.on_toggle = [this](bool value) {
                settings_.logging_enabled = value;
                applyLoggingSettings("settings-toggle");
                SaveSettings(settings_);
            };
            rows.push_back(std::move(logging_enabled));

            if (settings_.logging_enabled) {
                fsui::SettingsRowDescriptor log_level;
                log_level.kind = fsui::SettingsRowKind::Choice;
                log_level.id = "log-level";
                log_level.title = ICON_FA_TERMINAL " Log Level";
                log_level.summary = "Choose how much detail ARMSX writes to the debug log.";
                log_level.value = log_level_string(settings_.log_level);
                log_level.dialog_title = log_level.title;
                log_level.choices = BuildLogLevelChoices(settings_.log_level);
                log_level.on_choice = [this](int index) {
                    settings_.log_level = std::clamp(index, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
                    applyLoggingSettings("settings-log-level");
                    SaveSettings(settings_);
                };
                rows.push_back(std::move(log_level));

                fsui::SettingsRowDescriptor log_path;
                log_path.kind = fsui::SettingsRowKind::Notice;
                log_path.id = "log-path";
                log_path.title = ICON_FA_FILE_ALT " Log File";
                log_path.summary = diagnosticsLogPath().string();
                rows.push_back(std::move(log_path));
            }

            fsui::SettingsRowDescriptor settings_path;
            settings_path.kind = fsui::SettingsRowKind::Notice;
            settings_path.id = "settings-path";
            settings_path.title = ICON_FA_FILE_ALT " Settings File";
            settings_path.summary = settings_.settings_path;
            rows.push_back(std::move(settings_path));

            return rows;
        };
        pages.push_back(std::move(advanced_page));

        return pages;
    }

    std::vector<fsui::SettingsPageDescriptor> buildPerGameSettingsPages() {
        std::vector<fsui::SettingsPageDescriptor> pages;

        fsui::SettingsPageDescriptor summary;
        summary.id = "Summary";
        summary.scope = fsui::SettingsScope::PerGame;
        summary.built_in_page = fsui::BuiltInSettingsPage::Summary;
        summary.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;
            const fsui::CurrentGameInfo info = session_.currentGameInfo();

            fsui::SettingsRowDescriptor title;
            title.kind = fsui::SettingsRowKind::Notice;
            title.id = "current-game";
            title.title = info.title;
            title.summary = info.path.empty() ? info.subtitle : info.path.string();
            rows.push_back(std::move(title));

            return rows;
        };
        pages.push_back(std::move(summary));

        fsui::SettingsPageDescriptor graphics;
        graphics.id = "Graphics";
        graphics.scope = fsui::SettingsScope::PerGame;
        graphics.built_in_page = fsui::BuiltInSettingsPage::Graphics;
        graphics.build_rows = [this]() { return buildGraphicsRows(false); };
        pages.push_back(std::move(graphics));

        fsui::SettingsPageDescriptor emulation;
        emulation.id = "Emulation";
        emulation.scope = fsui::SettingsScope::PerGame;
        emulation.built_in_page = fsui::BuiltInSettingsPage::Emulation;
        emulation.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor reset;
            reset.kind = fsui::SettingsRowKind::Action;
            reset.id = "reset-now";
            reset.title = ICON_FA_SYNC " Reset";
            reset.summary = "Soft-reset the active session.";
            reset.on_activate = [this]() { deferred_reset_ = true; };
            rows.push_back(std::move(reset));

            fsui::SettingsRowDescriptor disc;
            disc.kind = fsui::SettingsRowKind::Action;
            disc.id = "change-disc-now";
            disc.title = ICON_FA_COMPACT_DISC " Change Disc";
            disc.summary = "Swap to a different disc image.";
            disc.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "Change Disc",
                    false,
                [this](const std::string& path) { queueDiscSwapSelection(path); },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                initialBrowseDirectory().string()
            );
        };
            rows.push_back(std::move(disc));

            fsui::SettingsRowDescriptor screenshot;
            screenshot.kind = fsui::SettingsRowKind::Action;
            screenshot.id = "save-screenshot-now";
            screenshot.title = ICON_FA_CAMERA " Save Screenshot";
            screenshot.summary = "Write a BMP screenshot to the snapshot folder.";
            screenshot.on_activate = [this]() { deferred_screenshot_ = true; };
            rows.push_back(std::move(screenshot));

            fsui::SettingsRowDescriptor exit_library;
            exit_library.kind = fsui::SettingsRowKind::Action;
            exit_library.id = "exit-library-now";
            exit_library.title = ICON_FA_FOLDER_OPEN " Exit To Library";
            exit_library.summary = "Close the current game and return to the main menu.";
            exit_library.on_activate = [this]() { deferred_exit_to_library_ = true; };
            rows.push_back(std::move(exit_library));

            return rows;
        };
        pages.push_back(std::move(emulation));

        return pages;
    }

    std::vector<fsui::SettingsRowDescriptor> buildGraphicsRows(bool include_scale) {
        std::vector<fsui::SettingsRowDescriptor> rows;
#if defined(__EMSCRIPTEN__)
        const bool can_control_vsync = false;
#else
        const bool can_control_vsync = owns_renderer_ && !external_renderer_;
#endif

#ifdef USE_HARDWARE
        {
            fsui::SettingsRowDescriptor gpu_backend;
            gpu_backend.kind = fsui::SettingsRowKind::Choice;
            gpu_backend.id = "gpu-backend";
            gpu_backend.title = ICON_FA_TACHOMETER_ALT " GPU Backend";
            gpu_backend.summary = hardwareRendererAvailable()
                ? "Choose SDL software or accelerated presentation. PlayStation GPU rasterization remains accuracy-first in software. Applies on the next launch."
                : "SDL renderer selection is controlled by the embedding host in this display mode.";
            gpu_backend.value = GpuBackendTitle(settings_.gpu_backend);
            gpu_backend.dialog_title = gpu_backend.title;
            gpu_backend.choices = BuildGpuBackendChoices(settings_.gpu_backend);
            gpu_backend.enabled = hardwareRendererAvailable();
            if (!gpu_backend.enabled) {
                gpu_backend.availability = fsui::AvailabilityMode::Disabled;
                gpu_backend.unavailable_title = gpu_backend.title;
                gpu_backend.unavailable_message = "This embedding host owns the SDL renderer.";
            }
            gpu_backend.on_choice = [this](int index) {
                settings_.gpu_backend = (index == 1) ? GpuBackend::SDLAccelerated : GpuBackend::Software;
                SaveSettings(settings_);
            };
            rows.push_back(std::move(gpu_backend));
        }
#endif

        if (include_scale) {
            fsui::SettingsRowDescriptor scale;
            scale.kind = fsui::SettingsRowKind::Choice;
            scale.id = "display-scale";
            scale.title = ICON_FA_EXPAND " Display Scale";
            scale.summary = "Resize the app window.";
            scale.value = std::to_string(settings_.scale) + "x";
            scale.dialog_title = scale.title;
            scale.choices = BuildScaleChoices(settings_.scale);
            scale.on_choice = [this](int index) {
                static const std::array<int, 6> values = {1, 2, 3, 4, 5, 6};
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    settings_.scale = values[static_cast<size_t>(index)];
                    applyWindowMetrics();
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(scale));
        }

        fsui::SettingsRowDescriptor vsync;
        vsync.kind = fsui::SettingsRowKind::Toggle;
        vsync.id = "vsync";
        vsync.title = ICON_FA_SYNC " VSync";
#if defined(__EMSCRIPTEN__)
        vsync.summary = "Synchronize presentation to the display refresh rate. The browser controls this on web builds.";
#else
        vsync.summary = can_control_vsync
            ? "Synchronize presentation to the display refresh rate to reduce tearing. This applies immediately."
            : "Synchronize presentation to the display refresh rate. This renderer is host-controlled here.";
#endif
        vsync.toggle_value = settings_.vsync_enabled;
        vsync.enabled = can_control_vsync;
        vsync.on_toggle = [this](bool value) {
            settings_.vsync_enabled = value;
            pending_settings_page_restore_ = "Graphics";
            SaveSettings(settings_);
            deferred_vsync_ = value;
        };
        rows.push_back(std::move(vsync));

        fsui::SettingsRowDescriptor filter;
        filter.kind = fsui::SettingsRowKind::Toggle;
        filter.id = "texture-filter";
        filter.title = ICON_FA_ADJUST " Texture Scaling / Bilinear";
        filter.summary = "Use smoother filtering for the display output.";
        filter.toggle_value = settings_.texture_scale_mode;
        filter.on_toggle = [this](bool value) {
            settings_.texture_scale_mode = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(filter));

        fsui::SettingsRowDescriptor stretch;
        stretch.kind = fsui::SettingsRowKind::Toggle;
        stretch.id = "stretch";
        stretch.title = ICON_FA_EXPAND_ARROWS_ALT " Stretch";
        stretch.summary = "Fill the window instead of preserving the original aspect ratio.";
        stretch.toggle_value = settings_.stretch_mode;
        stretch.on_toggle = [this](bool value) {
            settings_.stretch_mode = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(stretch));

        fsui::SettingsRowDescriptor aspect;
        aspect.kind = fsui::SettingsRowKind::Choice;
        aspect.id = "aspect";
        aspect.title = ICON_FA_TV " Display Aspect";
        aspect.summary = "Choose the presentation aspect ratio.";
        aspect.value = AspectTitle(settings_.display_aspect);
        aspect.dialog_title = aspect.title;
        aspect.choices = BuildAspectChoices(settings_.display_aspect);
        aspect.on_choice = [this](int index) {
            settings_.display_aspect = std::clamp(index, 0, 2);
            applyWindowMetrics();
            SaveSettings(settings_);
        };
        rows.push_back(std::move(aspect));

        fsui::SettingsRowDescriptor upscale;
        upscale.kind = fsui::SettingsRowKind::Choice;
        upscale.id = "wide-upscale";
        upscale.title = ICON_FA_EXPAND " Wide Upscale";
        upscale.summary = "Choose the wide-mode output height.";
        upscale.value = UpscaleToString(settings_.upscale_height);
        upscale.dialog_title = upscale.title;
        upscale.choices = BuildUpscaleChoices(settings_.upscale_height);
        upscale.on_choice = [this](int index) {
            static const std::array<int, 5> heights = {480, 720, 1080, 1440, 2160};
            if (index >= 0 && index < static_cast<int>(heights.size())) {
                settings_.upscale_height = heights[static_cast<size_t>(index)];
                applyWindowMetrics();
                SaveSettings(settings_);
            }
        };
        rows.push_back(std::move(upscale));

        fsui::SettingsRowDescriptor debug_panel;
        debug_panel.kind = fsui::SettingsRowKind::Toggle;
        debug_panel.id = "debug-panel";
        debug_panel.title = ICON_FA_TACHOMETER_ALT " Debug Panel";
        debug_panel.summary = "Show the settings and performance overlays.";
        debug_panel.toggle_value = settings_.debug_panel;
        debug_panel.on_toggle = [this](bool value) {
            settings_.debug_panel = value;
            settings_.ui_state.show_settings_overlay = value;
            settings_.ui_state.show_performance_overlay = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(debug_panel));

        if (session_.valid()) {
            fsui::SettingsRowDescriptor debug_view;
            debug_view.kind = fsui::SettingsRowKind::Toggle;
            debug_view.id = "debug-view";
            debug_view.title = ICON_FA_BUG " VRAM Debug View";
            debug_view.summary = "Show raw VRAM instead of the game image.";
            debug_view.toggle_value = session_.debugView();
            debug_view.on_toggle = [this](bool value) {
                session_.setDebugView(value);
                applyWindowMetrics();
            };
            rows.push_back(std::move(debug_view));
        }

        return rows;
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
            const fsui::CurrentGameInfo info = session_.currentGameInfo();
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
    SDL_Renderer* renderer_ = nullptr;
    fsui::SdlImGuiBackend imgui_backend_{};
    ArmsxSession session_{};
    GameplayInputRouter input_router_{};
    std::vector<fsui::GameEntry> game_list_{};
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
    bool deferred_fast_forward_toggle_ = false;
    bool fsui_initialized_ = false;
    FsuiWindowState ui_window_state_ = FsuiWindowState::None;
    bool managed_renderer_vsync_ = DefaultVsyncEnabled();
    uint64_t next_frame_deadline_ = 0;
    uint64_t frame_period_ticks_ = 0;
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

extern "C" PSXE_API int external_main(int argc, const char* argv[], void* external_window, void* external_renderer) {
    return psxe_run(argc, argv, external_window, external_renderer);
}

#ifndef __DLL_BUILD
int main(int argc, const char* argv[]) {
    return external_main(argc, argv, nullptr, nullptr);
}
#endif
