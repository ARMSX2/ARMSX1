/*
    ARMSX — RetroArch (.slangp) shader chains, via librashader. See render_shaders.h for
    the contract; this file is the implementation and the reasoning.

    ================================================================================
    HOW LIBRASHADER IS LINKED: IT IS NOT. IT IS dlopen()ED.
    ================================================================================
    third_party/librashader/prebuilt/arm64-v8a/liblibrashader_capi.so is staged into
    jniLibs next to libSDL2.so, and opened by SONAME the first time anything here needs
    it. `libarmsx.so` has NO DT_NEEDED entry for it. That is deliberate:

      * A hard DT_NEEDED on a 13 MB optional feature means a missing or corrupt
        librashader makes LIBARMSX ITSELF fail to load — every JNI method vanishes and the
        app dies at NativeApp.initialize with an error that points nowhere near shaders.
      * The static archive is 79 MB and this Makefile puts -flto in BASE_CFLAGS, so
        linking it would pull every Rust object into the LTO link.
      * dlopen costs ~15 function pointers and gives the degrade path this feature is
        required to have: no librashader -> log it once -> present normally.

    ================================================================================
    WHERE THE CHAIN SITS RELATIVE TO INTERNAL-RESOLUTION UPSCALING
    ================================================================================
    This is the decision that most often ships this feature broken, so it is written down
    rather than left implicit.

    The pipeline is: psx/dev/gpu.c rasterises (optionally through an internal-resolution
    backend at scale S, [video] internal_scale 1..8), frontend/main.cpp hands the present
    layer a frame of nativeW*S x nativeH*S, and render_vk.cpp blits that into an
    aspect-corrected letterbox rect computed by armsx_render_compute_dst().

    Two facts decide where the chain goes:

    1. A CRT shader's scanline count comes from SourceSize.y. Feed it a 2x frame and it
       draws 480 scanlines instead of 240; feed it 4x and it draws 960. Those land on a
       fractional number of screen pixels and smear into moire. This is not a matter of
       taste — a CRT chain is WRONG on an already-upscaled image, which is why RetroArch's
       own answer is "use 1x with CRT shaders".

    2. A CRT shader's mask/scanline geometry must be generated at DISPLAY pixel density,
       not at source density, or the presenter's own scale-up smears it.

    So: **the chain reads the frame at NATIVE PlayStation resolution and writes at the
    aspect-corrected DISPLAY size.**

      frame_image (nativeW*S x nativeH*S)
        --(linear downsample by S, only when S > 1)-->  chain input (nativeW x nativeH)
        --(librashader, viewport = the letterbox rect)-->  chain output (dst_w x dst_h)
        --(1:1 blit)-->  swapchain at (dst_x, dst_y)

    What this means for each renderer setting:

      * [video] renderer = "software"  (S == 1): the downsample is the identity and is
        skipped entirely. The chain reads frame_image directly. Zero added cost.
      * [video] renderer = "hardware" at 2x/3x: the frame is downsampled to native first.
        This is NOT throwing the upscale away — a 3x render box-filtered to 1x is
        SUPERSAMPLING, so the CRT shader gets a cleaner, anti-aliased native image than the
        software rasteriser could ever hand it. It is, however, a poor CPU trade on the CPU
        rasteriser, so the log says so explicitly and names internal_scale = 1 as the
        cheaper way to the same picture.

    Deliberately NOT offered: a "run the chain on the upscaled frame" mode. It is only
    right for sharpen/FSR-style chains, it is the setting people mis-set and then report a
    CRT preset as broken, and a `[video]` key with no Kotlin field behind it is silently
    dropped by Ps1SettingsStore the first time the user saves anything (three settings have
    already been lost that way in this port). If it is ever wanted it needs all four
    places, not just this file.

    ================================================================================
    WHY THE CHAIN OWNS ITS OWN OUTPUT IMAGE
    ================================================================================
    librashader needs a SAMPLED input and a COLOR_ATTACHMENT output. frame_image gained
    VK_IMAGE_USAGE_SAMPLED_BIT (universally supported for R8G8B8A8_UNORM optimal tiling)
    so the common S == 1 path samples it in place with no copy. The swapchain, though, is
    created TRANSFER_DST only, and whether COLOR_ATTACHMENT is even in
    VkSurfaceCapabilitiesKHR::supportedUsageFlags is driver-dependent — so the chain
    renders into an image it owns, sized exactly to the letterbox rect, and that gets
    blitted 1:1 into the swapchain. One extra full-rect copy per frame (~1% of memory
    bandwidth at 1080p on the test device), in exchange for one code path that cannot be
    defeated by a surface's usage flags. This is also what ARMSX2 does.
*/

/* A shader chain needs a Vulkan present path to hook, so ARMSX_ENABLE_SHADERS without
   ARMSX_ENABLE_VULKAN is a contradiction. The Makefile already refuses to set it that way;
   enforcing it here too means this file still compiles if someone defines it by hand, and
   keeps the guard in exactly one place for every declaration below. */
#if defined(ARMSX_ENABLE_SHADERS) && !defined(ARMSX_ENABLE_VULKAN)
#undef ARMSX_ENABLE_SHADERS
#endif

#include "render_shaders.h"

#include "render_internal.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

/* The Vulkan presenter serializes queue recording with the shader seam even when
   librashader is compiled out. Keep the guard available to the always-built
   armsx_shader_vk_queue_lock/unlock entry points below; defining it only inside
   ARMSX_ENABLE_SHADERS leaves Vulkan/no-shader builds with an undefined symbol. */
namespace {
std::mutex g_vk_queue_lock;
}

#if defined(ARMSX_ENABLE_SHADERS)

#include <dlfcn.h>

#include "../third_party/librashader/include/librashader_min.h"

namespace {

/* ---- librashader entry points ----------------------------------------------------- */

struct LibraApi {
    void* handle = nullptr;
    bool  usable = false;

    PFN_libra_preset_create              preset_create = nullptr;
    PFN_libra_preset_free                preset_free = nullptr;
    PFN_libra_preset_get_runtime_params  preset_get_runtime_params = nullptr;
    PFN_libra_preset_free_runtime_params preset_free_runtime_params = nullptr;

    PFN_libra_vk_filter_chain_create    chain_create = nullptr;
    PFN_libra_vk_filter_chain_frame     chain_frame = nullptr;
    PFN_libra_vk_filter_chain_set_param chain_set_param = nullptr;
    PFN_libra_vk_filter_chain_free      chain_free = nullptr;

    PFN_libra_error_errno       error_errno = nullptr;
    PFN_libra_error_write       error_write = nullptr;
    PFN_libra_error_free_string error_free_string = nullptr;
    PFN_libra_error_free        error_free = nullptr;

    PFN_libra_instance_abi_version abi_version = nullptr;
    PFN_libra_instance_api_version api_version = nullptr;
};

LibraApi g_libra;
std::mutex g_libra_lock;
bool g_libra_tried = false;

template <typename Fn>
bool Bind(void* handle, const char* name, Fn* out) {
    void* symbol = dlsym(handle, name);
    *out = reinterpret_cast<Fn>(symbol);
    if (!symbol) {
        armsx_render_log("shaders", "librashader: entry point '%s' is missing.", name);
        return false;
    }
    return true;
}

/* Opened by SONAME. On Android the APK's jniLibs directory is on the app's linker
   namespace search path, exactly as it is for libSDL2.so, so the bare name resolves; the
   absolute-path candidates are for desktop/dev runs where it sits beside the binary. */
const LibraApi* EnsureLibra() {
    std::lock_guard<std::mutex> lock(g_libra_lock);
    if (g_libra_tried) {
        return g_libra.usable ? &g_libra : nullptr;
    }
    g_libra_tried = true;

    static const char* const kCandidates[] = {
        "liblibrashader_capi.so",
        "./liblibrashader_capi.so",
        "liblibrashader_capi.dylib",
    };
    for (const char* name : kCandidates) {
        g_libra.handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (g_libra.handle) {
            break;
        }
    }
    if (!g_libra.handle) {
        /* Not an error in itself — a build without the .so staged simply has no shader
           chains. It must still be visible, because "the toggle does nothing" is the
           defect shape this port keeps producing. */
        armsx_render_log("shaders",
                         "librashader is not available (dlopen failed: %s). Shader chains are "
                         "disabled; presentation is unaffected.",
                         dlerror() ? dlerror() : "unknown");
        return nullptr;
    }

    const bool bound =
        Bind(g_libra.handle, "libra_instance_abi_version", &g_libra.abi_version) &&
        Bind(g_libra.handle, "libra_instance_api_version", &g_libra.api_version) &&
        Bind(g_libra.handle, "libra_preset_create", &g_libra.preset_create) &&
        Bind(g_libra.handle, "libra_preset_free", &g_libra.preset_free) &&
        Bind(g_libra.handle, "libra_preset_get_runtime_params", &g_libra.preset_get_runtime_params) &&
        Bind(g_libra.handle, "libra_preset_free_runtime_params", &g_libra.preset_free_runtime_params) &&
        Bind(g_libra.handle, "libra_vk_filter_chain_create", &g_libra.chain_create) &&
        Bind(g_libra.handle, "libra_vk_filter_chain_frame", &g_libra.chain_frame) &&
        Bind(g_libra.handle, "libra_vk_filter_chain_set_param", &g_libra.chain_set_param) &&
        Bind(g_libra.handle, "libra_vk_filter_chain_free", &g_libra.chain_free) &&
        Bind(g_libra.handle, "libra_error_errno", &g_libra.error_errno) &&
        Bind(g_libra.handle, "libra_error_write", &g_libra.error_write) &&
        Bind(g_libra.handle, "libra_error_free_string", &g_libra.error_free_string) &&
        Bind(g_libra.handle, "libra_error_free", &g_libra.error_free);

    if (!bound) {
        dlclose(g_libra.handle);
        g_libra.handle = nullptr;
        return nullptr;
    }

    /* ABI is the STRUCT LAYOUT version and is not backwards compatible. Reading a
       differently-shaped libra_preset_param_t is silent memory corruption, not a clean
       failure, so refuse rather than hope. */
    const size_t abi = g_libra.abi_version();
    if (abi != LIBRASHADER_CURRENT_ABI) {
        armsx_render_log("shaders",
                         "librashader ABI %zu does not match the %zu this build was written "
                         "against; refusing to use it.",
                         abi, LIBRASHADER_CURRENT_ABI);
        dlclose(g_libra.handle);
        g_libra.handle = nullptr;
        return nullptr;
    }

    g_libra.usable = true;
    armsx_render_log("shaders", "librashader loaded (ABI %zu, API %zu).", abi,
                     g_libra.api_version());
    return &g_libra;
}

/* Consumes and frees `error`, returning its message. Empty when there was no error. */
std::string TakeError(const LibraApi* api, libra_error_t error) {
    if (!error) {
        return std::string();
    }
    std::string text;
    char* message = nullptr;
    if (api->error_write(error, &message) == 0 && message) {
        text.assign(message);
        api->error_free_string(&message);
    } else {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "librashader error %d", (int)api->error_errno(error));
        text.assign(buffer);
    }
    api->error_free(&error);
    return text;
}

/* ---- Vulkan entry points we need ourselves ----------------------------------------- */

/* Only what the input/output plumbing uses. Everything is resolved from the caller's
   vkGetInstanceProcAddr, so libvulkan never becomes a link-time dependency and an
   adrenotools custom driver stays substitutable. */
struct VkFns {
    PFN_vkGetDeviceProcAddr                 GetDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateImage                       CreateImage = nullptr;
    PFN_vkDestroyImage                      DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements        GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory                   BindImageMemory = nullptr;
    PFN_vkAllocateMemory                    AllocateMemory = nullptr;
    PFN_vkFreeMemory                        FreeMemory = nullptr;
    PFN_vkCmdPipelineBarrier                CmdPipelineBarrier = nullptr;
    PFN_vkCmdBlitImage                      CmdBlitImage = nullptr;
    PFN_vkDeviceWaitIdle                    DeviceWaitIdle = nullptr;
};

/* An image this layer owns, plus its memory. */
struct OwnedImage {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int            width = 0;
    int            height = 0;
    bool           initialized = false; /* false => its layout is still UNDEFINED */
};

/* ---- state -------------------------------------------------------------------------- */

/* Everything the UI writes. Read by the presenting thread once per frame. */
struct Request {
    std::mutex  lock;
    bool        enabled = false;
    std::string preset;

    /* Parameter assignments waiting for the presenting thread, and the preset they were
       read off. A queue keyed by preset is what stops a stale set from a screen the user
       has left landing on a chain that has moved on. */
    std::string             param_preset;
    std::map<std::string, float> params;
    bool                    params_dirty = false;
};

Request g_request;

int g_source_scale = 1;                /* [video] internal_scale, 1 when the rasterizer is off */
int g_backend = -1;                    /* armsx_render_backend_t of the live present backend */
bool g_backend_warned = false;

/*
    The single VkQueue guard. Held by the present path around its submits/presents and by the
    builder thread across chain_create(), which uploads librashader's lookup textures through
    that same queue. A VkDevice is safe to use from several threads; a VkQueue is not.
*/
/*
    Off-thread chain build.

    A CRT preset takes ~110 ms to compile. On the presenting thread that is a visible hitch every
    time a preset is selected, and on a driver that compiles synchronously it can be far worse —
    ARMSX2 has a documented Mali case where a synchronous shader compile stalls the frame outright.
    So the compile happens here, and the presenting thread adopts the finished chain on whatever
    frame it is ready.

    Ownership is deliberately one-way: the worker writes `result` and then sets `done`; the
    presenting thread reads `done` and only then touches `result`. Nothing else is shared, so
    there is no lock on this struct — the release/acquire pair on `done` is the handoff.
*/
struct Builder {
    std::thread       thread;
    std::atomic<bool> done{false};
    bool              busy = false;       /* presenting-thread only */
    std::string       path;               /* what the worker is building */
    libra_vk_filter_chain_t result = nullptr;
    bool              failed = false;
    double            compile_ms = 0.0;
    std::string       error;
};

Builder g_builder;

/* Presenting-thread-owned. No lock: only armsx_shader_vk_render/attach/detach touch it. */
struct ChainState {
    bool                    attached = false;
    armsx_shader_vk_device_t dev{};
    VkFns                   fns{};
    VkPhysicalDeviceMemoryProperties memory{};

    libra_vk_filter_chain_t chain = nullptr;
    std::string             active;      /* the preset the live chain was built from */
    std::string             desired;     /* what the UI last asked for; "" == none */
    bool                    failed = false; /* `desired` failed to build; do not retry it */
    size_t                  frame_count = 0;

    OwnedImage input;   /* only allocated when the frame has to be downsampled */
    OwnedImage output;  /* always: the chain's COLOR_ATTACHMENT render target */
    int        logged_input_w = 0;
    int        logged_input_h = 0;
};

ChainState g_state;

/* ---- helpers ------------------------------------------------------------------------ */

void WarnBackendOnce() {
    if (g_backend_warned || g_backend < 0) {
        return;
    }
    bool wants = false;
    {
        std::lock_guard<std::mutex> lock(g_request.lock);
        wants = g_request.enabled && !g_request.preset.empty();
    }
    if (!wants || g_backend == ARMSX_RENDER_BACKEND_VULKAN) {
        return;
    }
    g_backend_warned = true;
    armsx_render_log("shaders",
                     "A shader chain is enabled, but the active present backend is %s. "
                     "librashader has no GLES runtime, so chains need the Vulkan backend "
                     "([video] gpu_backend = \"vulkan\"). Presenting normally.",
                     armsx_render_backend_name((armsx_render_backend_t)g_backend));
}

uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags wanted) {
    for (uint32_t index = 0; index < g_state.memory.memoryTypeCount; ++index) {
        if ((type_bits & (1u << index)) == 0) {
            continue;
        }
        if ((g_state.memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
            return index;
        }
    }
    return UINT32_MAX;
}

void DestroyImage(OwnedImage* img) {
    if (img->image != VK_NULL_HANDLE) {
        g_state.fns.DestroyImage(g_state.dev.device, img->image, nullptr);
    }
    if (img->memory != VK_NULL_HANDLE) {
        g_state.fns.FreeMemory(g_state.dev.device, img->memory, nullptr);
    }
    *img = OwnedImage{};
}

/* R8G8B8A8_UNORM only. Vulkan's mandatory format table guarantees optimal-tiling
   SAMPLED_IMAGE / COLOR_ATTACHMENT / BLIT_SRC / BLIT_DST for it, so no capability probe
   is needed and no device can refuse this. */
bool EnsureImage(OwnedImage* img, int width, int height, VkImageUsageFlags usage) {
    if (img->image != VK_NULL_HANDLE && img->width == width && img->height == height) {
        return true;
    }
    DestroyImage(img);
    if (width <= 0 || height <= 0) {
        return false;
    }

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {(uint32_t)width, (uint32_t)height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (g_state.fns.CreateImage(g_state.dev.device, &info, nullptr, &img->image) != VK_SUCCESS) {
        armsx_render_log("shaders", "vkCreateImage failed for a %dx%d chain image.", width, height);
        *img = OwnedImage{};
        return false;
    }

    VkMemoryRequirements requirements{};
    g_state.fns.GetImageMemoryRequirements(g_state.dev.device, img->image, &requirements);
    const uint32_t type_index =
        FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_index == UINT32_MAX) {
        armsx_render_log("shaders", "no device-local memory type for a %dx%d chain image.", width,
                         height);
        DestroyImage(img);
        return false;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = type_index;
    if (g_state.fns.AllocateMemory(g_state.dev.device, &alloc, nullptr, &img->memory) != VK_SUCCESS ||
        g_state.fns.BindImageMemory(g_state.dev.device, img->image, img->memory, 0) != VK_SUCCESS) {
        armsx_render_log("shaders", "could not back a %dx%d chain image with memory.", width, height);
        DestroyImage(img);
        return false;
    }

    img->width = width;
    img->height = height;
    img->initialized = false;
    return true;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
             VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
             VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_state.fns.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void BlitFull(VkCommandBuffer cmd, VkImage src, int src_w, int src_h, VkImage dst, int dst_x,
              int dst_y, int dst_w, int dst_h, VkFilter filter) {
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {src_w, src_h, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {dst_x, dst_y, 0};
    blit.dstOffsets[1] = {dst_x + dst_w, dst_y + dst_h, 1};
    g_state.fns.CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
}

void DestroyChain() {
    if (!g_state.chain) {
        return;
    }
    const LibraApi* api = EnsureLibra();
    if (g_state.fns.DeviceWaitIdle && g_state.dev.device) {
        g_state.fns.DeviceWaitIdle(g_state.dev.device);
    }
    if (api) {
        api->chain_free(&g_state.chain);
    }
    g_state.chain = nullptr;
    g_state.active.clear();
    g_state.frame_count = 0;
}

/* The divisor that turns the uploaded frame back into a native-resolution one.

   The hint comes from [video] internal_scale via armsx_shader_set_source_scale(), which
   is the CONFIGURED scale — the rasterizer can have failed to install and left the frame
   at 1x, and config.c cannot know that. So the hint is walked DOWN until it divides the
   frame exactly and leaves a plausible PlayStation display size behind. Being wrong here
   is cosmetic (a chunkier scanline pitch) rather than corrupting, and the chosen divisor
   is logged. */
int SourceDivisor(int width, int height) {
    int divisor = g_source_scale;
    if (divisor < 1) {
        divisor = 1;
    }
    while (divisor > 1) {
        const bool exact = (width % divisor) == 0 && (height % divisor) == 0;
        if (exact && (width / divisor) >= 128 && (height / divisor) >= 100) {
            break;
        }
        --divisor;
    }
    return divisor;
}

void ApplyQueuedParams(const LibraApi* api) {
    std::map<std::string, float> values;
    {
        std::lock_guard<std::mutex> lock(g_request.lock);
        if (!g_request.params_dirty || g_request.param_preset != g_state.active) {
            return;
        }
        values = g_request.params;
        g_request.params_dirty = false;
    }
    int rejected = 0;
    for (const auto& entry : values) {
        libra_error_t error = api->chain_set_param(&g_state.chain, entry.first.c_str(), entry.second);
        if (error) {
            /* A name the chain does not know is the normal case when a preset is switched
               while an editor is open; count them rather than spamming a line each. */
            ++rejected;
            TakeError(api, error);
        }
    }
    if (rejected) {
        armsx_render_log("shaders", "%d of %zu queued parameters were not known to the chain.",
                         rejected, values.size());
    }
}

/*
    Builds a chain. Runs on the BUILDER THREAD — it must not touch g_state beyond the immutable
    device handles copied in at attach, and must take the queue guard around chain_create()
    because librashader uploads its lookup textures through that queue.
*/
void BuildChainWorker(const LibraApi* api, std::string path) {
    const auto started = std::chrono::steady_clock::now();

    libra_shader_preset_t preset = nullptr;
    if (libra_error_t error = api->preset_create(path.c_str(), &preset)) {
        armsx_render_log("shaders", "preset '%s' could not be read: %s. Presenting normally.",
                         path.c_str(), TakeError(api, error).c_str());
        g_builder.failed = true;
        g_builder.done.store(true, std::memory_order_release);
        return;
    }

    filter_chain_vk_opt_t options{};
    options.version = LIBRASHADER_CURRENT_VERSION;
    /* Our present path has exactly one command buffer and waits its fence before
       recording, so one frame is ever in flight. librashader's default of 3 keeps its
       per-frame residual ring comfortably ahead of us, which is the safe direction: an
       undersized ring frees intermediate image views while the GPU still reads them. */
    options.frames_in_flight = 3;
    options.force_no_mipmaps = false;
    /* MUST stay false. render_vk.cpp creates its VkDevice with VK_KHR_swapchain and
       nothing else, so the dynamic-rendering path does not exist here. */
    options.use_dynamic_rendering = false;
    /* librashader's compile cache resolves its directory through AppDirs ($XDG_CACHE_HOME
       / $HOME/.cache), neither of which is meaningful for an Android app — it would
       either fail or scribble somewhere outside the app's sandbox. Compiling is a
       one-off per preset load, so take the deterministic option. */
    options.disable_cache = true;

    libra_device_vk_t device{};
    device.physical_device = g_state.dev.physical_device;
    device.instance = g_state.dev.instance;
    device.device = g_state.dev.device;
    device.queue = g_state.dev.queue;
    device.entry = g_state.dev.get_instance_proc_addr;

    /* CONSUMES `preset`, on success and on failure alike (the Rust side takes the handle
       out before doing anything). Never free it after this call. */
    libra_vk_filter_chain_t built = nullptr;
    libra_error_t error;
    {
        /* chain_create() uploads LUTs through `device.queue`. Held for the whole call rather
           than guessing which part submits. */
        std::lock_guard<std::mutex> lock(g_vk_queue_lock);
        error = api->chain_create(&preset, device, &options, &built);
    }

    if (error || !built) {
        armsx_render_log("shaders", "preset '%s' failed to build: %s. Presenting normally.",
                         path.c_str(), TakeError(api, error).c_str());
        g_builder.result = nullptr;
        g_builder.failed = true;
        g_builder.done.store(true, std::memory_order_release);
        return;
    }

    g_builder.result = built;
    g_builder.failed = false;
    /* RELEASE: everything written above must be visible to the presenting thread before it can
       observe done == true. */
    g_builder.compile_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started)
                               .count();

    /* RELEASE, and the LAST thing the worker does. Everything the presenting thread will read
       is written above it; nothing below it exists. Touching g_state here — as this code did
       when it ran on the presenting thread — would be a plain data race, because the presenting
       thread owns g_state and is using it every frame. */
    g_builder.done.store(true, std::memory_order_release);
}

/*
    Presenting thread. Takes ownership of whatever the worker produced and does all the g_state
    work the old synchronous build used to do inline.
*/
void AdoptBuiltChain(const LibraApi* api) {
    if (!g_builder.busy || !g_builder.done.load(std::memory_order_acquire)) {
        return;
    }

    if (g_builder.thread.joinable()) {
        g_builder.thread.join();
    }

    g_builder.busy = false;
    g_builder.done.store(false, std::memory_order_relaxed);

    const std::string path = g_builder.path;

    /* The user may have changed preset while this was compiling. Throw the result away rather
       than installing a chain nobody asked for any more. */
    if (path != g_state.desired) {
        if (g_builder.result) {
            api->chain_free(&g_builder.result);
        }
        g_builder.result = nullptr;
        armsx_render_log("shaders", "discarded '%s': the preset changed while it compiled.",
                         path.c_str());
        return;
    }

    if (g_builder.failed || !g_builder.result) {
        g_state.failed = true;
        g_builder.result = nullptr;
        return;
    }

    g_state.chain = g_builder.result;
    g_builder.result = nullptr;

    g_state.active = path;
    g_state.frame_count = 0;
    g_state.logged_input_w = 0;
    g_state.logged_input_h = 0;
    armsx_render_log("shaders", "chain loaded: %s (compiled in %.0f ms off-thread)",
                     path.c_str(), g_builder.compile_ms);

    /* A fresh chain sits at the preset's own initial values, so whatever the UI queued
       before it existed still has to land. */
    {
        std::lock_guard<std::mutex> lock(g_request.lock);
        if (g_request.param_preset == path && !g_request.params.empty()) {
            g_request.params_dirty = true;
        }
    }
    ApplyQueuedParams(api);
}

/* Presenting thread. Kicks a build if one is wanted and none is running. */
void StartChainBuild(const LibraApi* api) {
    if (g_builder.busy || g_state.desired.empty()) {
        return;
    }

    g_builder.busy = true;
    g_builder.failed = false;
    g_builder.result = nullptr;
    g_builder.compile_ms = 0.0;
    g_builder.path = g_state.desired;
    g_builder.done.store(false, std::memory_order_relaxed);

    g_builder.thread = std::thread(BuildChainWorker, api, g_builder.path);
}

/* Presenting thread. Blocks until any in-flight build finishes and frees its result. Used by
   detach and by a preset change: a worker still holding the queue guard must not outlive the
   device it was handed. */
void CancelChainBuild(const LibraApi* api) {
    if (!g_builder.busy) {
        return;
    }

    if (g_builder.thread.joinable()) {
        g_builder.thread.join();
    }

    if (g_builder.result && api) {
        api->chain_free(&g_builder.result);
    }

    g_builder.result = nullptr;
    g_builder.busy = false;
    g_builder.failed = false;
    g_builder.done.store(false, std::memory_order_relaxed);
}

/* Reconciles the live chain with what the UI asked for. Runs BEFORE anything is recorded
   into the caller's command buffer: building a chain submits LUT uploads and waits on the
   queue, and tearing one down destroys images that recorded commands could still name. */
void Reconcile(const LibraApi* api) {
    std::string desired;
    {
        std::lock_guard<std::mutex> lock(g_request.lock);
        desired = g_request.enabled ? g_request.preset : std::string();
    }

    if (desired != g_state.desired) {
        /* Cancel BEFORE moving `desired`: AdoptBuiltChain compares against g_state.desired to
           decide whether a finished build is still wanted, and a worker mid-compile against the
           old preset would otherwise be adopted or leaked. */
        CancelChainBuild(api);
        g_state.desired = desired;
        g_state.failed = false;
        if (g_state.chain) {
            DestroyChain();
        }
    }

    if (g_state.desired.empty()) {
        if (g_state.input.image != VK_NULL_HANDLE || g_state.output.image != VK_NULL_HANDLE) {
            if (g_state.fns.DeviceWaitIdle) {
                g_state.fns.DeviceWaitIdle(g_state.dev.device);
            }
            DestroyImage(&g_state.input);
            DestroyImage(&g_state.output);
        }
        return;
    }

    /* Adopt first, then kick: a build that finished during the last frame becomes the live
       chain on this one, and only if nothing is pending do we start another. */
    AdoptBuiltChain(api);

    if (!g_state.chain && !g_state.failed) {
        StartChainBuild(api);
    }
}

/* ---- JSON ---------------------------------------------------------------------------- */

void AppendJsonString(std::string* out, const char* text) {
    out->push_back('"');
    for (const char* p = text ? text : ""; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\b': out->append("\\b");  break;
            case '\f': out->append("\\f");  break;
            case '\n': out->append("\\n");  break;
            case '\r': out->append("\\r");  break;
            case '\t': out->append("\\t");  break;
            default:
                if (c < 0x20) {
                    char escape[8];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                    out->append(escape);
                } else {
                    out->push_back((char)c);
                }
                break;
        }
    }
    out->push_back('"');
}

/* Shader authors write these; NaN/inf would produce JSON that JSONArray() rejects, and a
   throw there is exactly the crash-shaped failure this API is required not to have. */
void AppendJsonNumber(std::string* out, float value) {
    if (!std::isfinite(value)) {
        out->append("0");
        return;
    }
    char buffer[40];
    /* %g in the C locale: never a comma decimal separator. The Kotlin side reads these
       with JSONObject.optDouble, so plain decimal is what it wants. */
    std::snprintf(buffer, sizeof(buffer), "%.6g", (double)value);
    out->append(buffer);
}

char* Duplicate(const std::string& text) {
    char* copy = (char*)std::malloc(text.size() + 1);
    if (!copy) {
        return nullptr;
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    return copy;
}

} // namespace

#endif /* ARMSX_ENABLE_SHADERS */

namespace {

/* A freshly malloc'd "[]" — the ONLY thing armsx_shader_preset_params() may return on any
   failure path. Deliberately outside the ARMSX_ENABLE_SHADERS guard: the shaders-off build
   needs it too, and having exactly one definition is what stops the failure paths drifting
   apart into a "" or a NULL, either of which throws in the Kotlin caller's JSONArray(). */
char* EmptyParamJson() {
    static const char kEmpty[] = "[]";
    char* copy = (char*)std::malloc(sizeof(kEmpty));
    if (copy) {
        std::memcpy(copy, kEmpty, sizeof(kEmpty));
    }
    return copy;
}

} // namespace

/* ======================================================================================
   Public surface. Everything below is present in EVERY build — android_jni.cpp calls
   these unconditionally, and a build without librashader must link and behave, not fail.
   ====================================================================================== */

extern "C" {

void armsx_shader_set_chain(bool enabled, const char* preset_path) {
#if defined(ARMSX_ENABLE_SHADERS)
    const std::string path = preset_path ? preset_path : "";
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_request.lock);
        changed = (g_request.enabled != enabled) || (g_request.preset != path);
        g_request.enabled = enabled;
        g_request.preset = path;
    }
    if (changed) {
        armsx_render_log("shaders", "chain request: %s%s%s", enabled ? "on" : "off",
                         path.empty() ? "" : " preset=", path.c_str());
        g_backend_warned = false;
        WarnBackendOnce();
    }
#else
    (void)enabled;
    (void)preset_path;
#endif
}

void armsx_shader_queue_params(const char* preset_path, const char* const* names,
                               const float* values, int count) {
#if defined(ARMSX_ENABLE_SHADERS)
    if (!preset_path || !*preset_path) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_request.lock);
    if (g_request.param_preset != preset_path) {
        g_request.param_preset = preset_path;
        g_request.params.clear();
    }
    for (int i = 0; i < count; ++i) {
        if (names && names[i] && values) {
            g_request.params[names[i]] = values[i];
        }
    }
    g_request.params_dirty = true;
#else
    (void)preset_path;
    (void)names;
    (void)values;
    (void)count;
#endif
}

char* armsx_shader_preset_params(const char* preset_path) {
    /* The never-return-null contract is absolute: the Kotlin caller feeds this straight to
       JSONArray(), and both null and "" throw there. "[]" parses to an empty list. */
#if defined(ARMSX_ENABLE_SHADERS)
    if (!preset_path || !*preset_path) {
        return EmptyParamJson();
    }

    const LibraApi* api = EnsureLibra();
    if (!api) {
        return EmptyParamJson();
    }

    libra_shader_preset_t preset = nullptr;
    if (libra_error_t error = api->preset_create(preset_path, &preset)) {
        armsx_render_log("shaders", "cannot enumerate parameters of '%s': %s", preset_path,
                         TakeError(api, error).c_str());
        return EmptyParamJson();
    }

    std::string json = "[";
    libra_preset_param_list_t list{};
    if (libra_error_t error = api->preset_get_runtime_params(&preset, &list)) {
        armsx_render_log("shaders", "cannot enumerate parameters of '%s': %s", preset_path,
                         TakeError(api, error).c_str());
    } else {
        for (uint64_t i = 0; i < list.length; ++i) {
            const libra_preset_param_t& param = list.parameters[i];
            if (i) {
                json.push_back(',');
            }
            json.append("{\"name\":");
            AppendJsonString(&json, param.name);
            json.append(",\"description\":");
            AppendJsonString(&json, param.description);
            json.append(",\"initial\":");
            AppendJsonNumber(&json, param.initial);
            json.append(",\"minimum\":");
            AppendJsonNumber(&json, param.minimum);
            json.append(",\"maximum\":");
            AppendJsonNumber(&json, param.maximum);
            json.append(",\"step\":");
            AppendJsonNumber(&json, param.step);
            json.push_back('}');
        }
        api->preset_free_runtime_params(list);
    }
    json.push_back(']');

    /* get_runtime_params does NOT consume the preset (unlike chain_create), so this one
       is ours to free. */
    api->preset_free(&preset);

    if (char* copy = Duplicate(json)) {
        return copy;
    }
#else
    (void)preset_path;
#endif
    return EmptyParamJson();
}

void armsx_shader_free_string(char* text) {
    std::free(text);
}

void armsx_shader_set_source_scale(int scale) {
#if defined(ARMSX_ENABLE_SHADERS)
    g_source_scale = (scale >= 1 && scale <= 8) ? scale : 1;
#else
    (void)scale;
#endif
}

void armsx_shader_note_backend(int backend) {
#if defined(ARMSX_ENABLE_SHADERS)
    g_backend = backend;
    WarnBackendOnce();
#else
    (void)backend;
#endif
}

/* The Vulkan seam exists only where render_vk.cpp does — see render_shaders.h. */
#if defined(ARMSX_ENABLE_VULKAN)

void armsx_shader_vk_attach(const armsx_shader_vk_device_t* device) {
#if defined(ARMSX_ENABLE_SHADERS)
    armsx_shader_vk_detach();
    if (!device || !device->device || !device->get_instance_proc_addr) {
        return;
    }

    g_state = ChainState{};
    g_state.dev = *device;

    PFN_vkGetInstanceProcAddr gipa = device->get_instance_proc_addr;
    auto instance_fn = [&](const char* name) { return gipa(device->instance, name); };

    g_state.fns.GetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)instance_fn("vkGetDeviceProcAddr");
    g_state.fns.GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)instance_fn("vkGetPhysicalDeviceMemoryProperties");
    if (!g_state.fns.GetDeviceProcAddr || !g_state.fns.GetPhysicalDeviceMemoryProperties) {
        armsx_render_log("shaders", "could not resolve the Vulkan entry points the chain needs.");
        return;
    }

    auto device_fn = [&](const char* name) {
        return g_state.fns.GetDeviceProcAddr(device->device, name);
    };
    g_state.fns.CreateImage = (PFN_vkCreateImage)device_fn("vkCreateImage");
    g_state.fns.DestroyImage = (PFN_vkDestroyImage)device_fn("vkDestroyImage");
    g_state.fns.GetImageMemoryRequirements =
        (PFN_vkGetImageMemoryRequirements)device_fn("vkGetImageMemoryRequirements");
    g_state.fns.BindImageMemory = (PFN_vkBindImageMemory)device_fn("vkBindImageMemory");
    g_state.fns.AllocateMemory = (PFN_vkAllocateMemory)device_fn("vkAllocateMemory");
    g_state.fns.FreeMemory = (PFN_vkFreeMemory)device_fn("vkFreeMemory");
    g_state.fns.CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)device_fn("vkCmdPipelineBarrier");
    g_state.fns.CmdBlitImage = (PFN_vkCmdBlitImage)device_fn("vkCmdBlitImage");
    g_state.fns.DeviceWaitIdle = (PFN_vkDeviceWaitIdle)device_fn("vkDeviceWaitIdle");

    if (!g_state.fns.CreateImage || !g_state.fns.DestroyImage ||
        !g_state.fns.GetImageMemoryRequirements || !g_state.fns.BindImageMemory ||
        !g_state.fns.AllocateMemory || !g_state.fns.FreeMemory ||
        !g_state.fns.CmdPipelineBarrier || !g_state.fns.CmdBlitImage ||
        !g_state.fns.DeviceWaitIdle) {
        armsx_render_log("shaders", "could not resolve the Vulkan entry points the chain needs.");
        g_state = ChainState{};
        return;
    }

    g_state.fns.GetPhysicalDeviceMemoryProperties(device->physical_device, &g_state.memory);
    g_state.attached = true;
#else
    (void)device;
#endif
}

void armsx_shader_vk_detach(void) {
#if defined(ARMSX_ENABLE_SHADERS)
    /* FIRST, and before any wait-idle: a builder thread may be inside chain_create() holding the
       queue guard and using the very device we are about to tear down. Joining it here is what
       stops the worker outliving its VkDevice — a use-after-free that would land on whichever
       unlucky frame the surface was destroyed. */
    CancelChainBuild(g_libra.handle ? &g_libra : nullptr);

    if (!g_state.attached) {
        g_state = ChainState{};
        return;
    }
    if (g_state.fns.DeviceWaitIdle && g_state.dev.device) {
        g_state.fns.DeviceWaitIdle(g_state.dev.device);
    }
    DestroyChain();
    DestroyImage(&g_state.input);
    DestroyImage(&g_state.output);
    g_state = ChainState{};
#endif
}

bool armsx_shader_vk_render(const armsx_shader_vk_frame_t* frame) {
#if defined(ARMSX_ENABLE_SHADERS)
    if (!frame || !g_state.attached || frame->dst_w <= 0 || frame->dst_h <= 0) {
        return false;
    }

    /* Nothing requested and nothing live: the overwhelmingly common case. Take it before
       touching dlopen, so a build with the chain off never pays for librashader at all. */
    {
        std::lock_guard<std::mutex> lock(g_request.lock);
        if ((!g_request.enabled || g_request.preset.empty()) && g_state.desired.empty() &&
            !g_state.chain) {
            return false;
        }
    }

    const LibraApi* api = EnsureLibra();
    if (!api) {
        /* EnsureLibra has already said why, once. Make sure a chain requested against a
           missing librashader does not keep any state around. */
        g_state.desired.clear();
        return false;
    }

    Reconcile(api);
    if (!g_state.chain) {
        return false;
    }
    ApplyQueuedParams(api);

    /* ---- input: native-resolution view of the uploaded frame --------------------- */
    const int divisor = SourceDivisor(frame->source_width, frame->source_height);
    const int input_w = frame->source_width / divisor;
    const int input_h = frame->source_height / divisor;

    if (input_w != g_state.logged_input_w || input_h != g_state.logged_input_h) {
        g_state.logged_input_w = input_w;
        g_state.logged_input_h = input_h;
        if (divisor > 1) {
            armsx_render_log("shaders",
                             "chain input %dx%d -> %dx%d (internal scale %dx downsampled to "
                             "native, so the shader's scanline pitch is the PlayStation's; set "
                             "[video] internal_scale = 1 for the same picture at lower cost), "
                             "output %dx%d",
                             frame->source_width, frame->source_height, input_w, input_h, divisor,
                             frame->dst_w, frame->dst_h);
        } else {
            armsx_render_log("shaders", "chain input %dx%d (native), output %dx%d", input_w,
                             input_h, frame->dst_w, frame->dst_h);
        }
    }

    VkImage input_image = frame->source;
    if (divisor > 1) {
        if (!EnsureImage(&g_state.input, input_w, input_h,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            return false;
        }
        input_image = g_state.input.image;
    }

    if (!EnsureImage(&g_state.output, frame->dst_w, frame->dst_h,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }

    VkCommandBuffer cmd = frame->cmd;

    if (divisor > 1) {
        Barrier(cmd, g_state.input.image,
                g_state.input.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                g_state.input.initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                g_state.input.initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        /* LINEAR: at an exact 2:1 this is a 2x2 box filter, i.e. supersampling. */
        BlitFull(cmd, frame->source, frame->source_width, frame->source_height,
                 g_state.input.image, 0, 0, input_w, input_h, VK_FILTER_LINEAR);
        Barrier(cmd, g_state.input.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        g_state.input.initialized = true;
    } else {
        /* Sample frame_image in place. The caller handed it over in TRANSFER_SRC_OPTIMAL
           and expects it back that way, so this is a there-and-back pair. */
        Barrier(cmd, frame->source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    /* ---- output: the chain's render target -------------------------------------- */
    /* UNDEFINED every frame: the chain overwrites the whole viewport, which IS the whole
       image here, so there is nothing to preserve and discarding is cheaper on a tiler. */
    Barrier(cmd, g_state.output.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g_state.output.initialized = true;

    libra_image_vk_t source{};
    source.handle = input_image;
    source.format = VK_FORMAT_R8G8B8A8_UNORM;
    source.width = (uint32_t)input_w;
    source.height = (uint32_t)input_h;

    libra_image_vk_t target{};
    target.handle = g_state.output.image;
    target.format = VK_FORMAT_R8G8B8A8_UNORM;
    target.width = (uint32_t)frame->dst_w;
    target.height = (uint32_t)frame->dst_h;

    /* The whole owned target. The aspect correction is already in its SIZE — handing
       librashader the full window rect instead is what stretches a 4:3 frame to 16:9. */
    libra_viewport_t viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (uint32_t)frame->dst_w;
    viewport.height = (uint32_t)frame->dst_h;

    libra_error_t error = api->chain_frame(&g_state.chain, cmd, g_state.frame_count, source, target,
                                           &viewport, nullptr, nullptr);
    if (error) {
        armsx_render_log("shaders", "chain '%s' failed mid-frame: %s. Falling back to plain "
                                    "presentation.",
                         g_state.active.c_str(), TakeError(api, error).c_str());
        /* Do NOT free anything here: commands naming these objects are already recorded
           into a command buffer the caller is about to submit. Mark it and let the next
           Reconcile(), which runs before any recording, do the teardown. */
        g_state.failed = true;
        g_state.desired.clear();
        /* Restore the layouts the caller expects, then let it blit over the top. */
        if (divisor <= 1) {
            Barrier(cmd, frame->source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        return false;
    }
    ++g_state.frame_count;

    /* ---- output -> swapchain ----------------------------------------------------- */
    /* librashader emits no barrier for the final pass; the output is still in
       COLOR_ATTACHMENT_OPTIMAL and transitioning it is documented as ours. */
    Barrier(cmd, g_state.output.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

    BlitFull(cmd, g_state.output.image, frame->dst_w, frame->dst_h, frame->target, frame->dst_x,
             frame->dst_y, frame->dst_w, frame->dst_h, VK_FILTER_NEAREST);

    if (divisor <= 1) {
        Barrier(cmd, frame->source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    return true;
#else
    (void)frame;
    return false;
#endif
}

#endif /* ARMSX_ENABLE_VULKAN */

} // extern "C"

extern "C" void armsx_shader_vk_queue_lock(void) {
    g_vk_queue_lock.lock();
}

extern "C" void armsx_shader_vk_queue_unlock(void) {
    g_vk_queue_lock.unlock();
}
