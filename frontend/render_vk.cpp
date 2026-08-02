/*
    ARMSX — Vulkan presentation backend.

    ============================ STATUS: EXPERIMENTAL ============================
    This backend is COMPILE-VERIFIED ONLY. It has not been executed on a device by
    the author of this file. It is never selected unless the user explicitly asks for
    `gpu_backend = "vulkan"`, and every failure path returns NULL so the frontend's
    fallback ladder (vulkan -> opengl -> sdl-accelerated -> software, see
    ArmsxApp::createManagedRenderer) keeps the emulator bootable. Treat any claim of
    "Vulkan works" as unproven until it has been run.
    ==============================================================================

    DESIGN
    ------
    Presentation only — no graphics pipeline, no render pass, no SPIR-V. The frame is moved
    with transfer commands alone:

        host staging VkBuffer  --vkCmdCopyBufferToImage-->  R8G8B8A8_UNORM VkImage
                               --vkCmdBlitImage----------->  swapchain image

    vkCmdClearColorImage paints the letterbox bars first, and vkCmdBlitImage does the scale
    with VK_FILTER_LINEAR / VK_FILTER_NEAREST, which is exactly the filtering choice the
    settings expose. Avoiding a pipeline means no shader toolchain in the Makefile and no
    hand-written SPIR-V, at the cost of one CPU-side format conversion.

    WHY THE CPU CONVERSION
    ----------------------
    The core's 16-bit format is A1B5G5R5 (mask in bit 15, then B, G, R). Core Vulkan has no
    matching packed format (VK_FORMAT_A1B5G5R5_UNORM_PACK16 only arrived with
    maintenance5/1.4), and VK_FORMAT_R8G8B8_UNORM is very rarely blit-source capable. So the
    dirty rows are unpacked to RGBA8 on the CPU at native PlayStation resolution
    (320x240-ish, ~0.3 MB/frame) rather than at output resolution. That is a small fraction
    of the full-resolution surface blit this backend replaces. If a shader-based decode is
    wanted later, it drops in as a render pass between the staging copy and the present.

    CUSTOM DRIVER (adrenotools)
    ---------------------------
    libvulkan is never linked. It is opened through a replaceable function pointer,
    armsx_render_set_vulkan_loader() (frontend/render.h). Handing it a function that calls
    adrenotools_open_libvulkan() is the whole integration — see the REPORT notes and
    pcsx2/GS/Renderers/Vulkan/VKLoader.cpp in the PS2 tree for the reference implementation.
*/

#include "render_internal.h"
#include "gpu_profile.h"

#if defined(ARMSX_ENABLE_VULKAN)

#define VK_NO_PROTOTYPES
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <SDL_vulkan.h>

/* AFTER <vulkan/vulkan.h>, deliberately: render_shaders.h includes it too, but without
   VK_USE_PLATFORM_ANDROID_KHR. Letting it get there first would leave this file without
   VkAndroidSurfaceCreateInfoKHR. */
#include "render_shaders.h"

namespace {
/* RAII over the shader module's VkQueue guard. Scoped so an early return inside a submit block
   can never leave the queue locked against the builder thread. */
struct ShaderQueueGuard {
    ShaderQueueGuard() { armsx_shader_vk_queue_lock(); }
    ~ShaderQueueGuard() { armsx_shader_vk_queue_unlock(); }
    ShaderQueueGuard(const ShaderQueueGuard&) = delete;
    ShaderQueueGuard& operator=(const ShaderQueueGuard&) = delete;
};
}  // namespace

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_loader_lock;
armsx_vk_library_open_fn g_loader_fn = nullptr;
void* g_loader_user = nullptr;

/* Whether the handle we are running on came from the caller-supplied (custom driver) loader
   or from the system Vulkan loader. A custom driver that was configured but failed to open
   falls back silently at the dlopen level, so this is the only place the difference survives
   — and it is what stops the driver manager from claiming a driver is active when it is not. */
bool g_custom_loader_active = false;

void* OpenVulkanLibrary() {
    armsx_vk_library_open_fn custom = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_loader_lock);
        custom = g_loader_fn;
        user = g_loader_user;
    }

    g_custom_loader_active = false;

    if (custom) {
        if (void* handle = custom(user)) {
            armsx_render_log("renderer", "Vulkan: using the caller-supplied loader handle.");
            g_custom_loader_active = true;
            return handle;
        }
        armsx_render_log("renderer",
                         "Vulkan: the custom driver could NOT be opened; falling back to the "
                         "system loader. The reported renderer will not claim a custom driver.");
    }

#if defined(_WIN32)
    return nullptr; /* Not wired: this backend targets Android/Linux. */
#else
    static const char* const kCandidates[] = {
        "libvulkan.so",
        "libvulkan.so.1",
        "libvulkan.1.dylib",
        "libMoltenVK.dylib",
    };
    for (const char* name : kCandidates) {
        if (void* handle = dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
            return handle;
        }
    }
    return nullptr;
#endif
}

void CloseVulkanLibrary(void* handle) {
#if !defined(_WIN32)
    if (handle) {
        dlclose(handle);
    }
#else
    (void)handle;
#endif
}

/* ---- entry point tables --------------------------------------------------------------- */

#define ARMSX_VK_INSTANCE_FUNCS(X)          \
    X(vkDestroyInstance)                    \
    X(vkEnumeratePhysicalDevices)           \
    X(vkGetPhysicalDeviceProperties)        \
    X(vkGetPhysicalDeviceMemoryProperties)  \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties)  \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkDestroySurfaceKHR)                  \
    X(vkCreateDevice)                       \
    X(vkGetDeviceProcAddr)

#define ARMSX_VK_DEVICE_FUNCS(X)   \
    X(vkDestroyDevice)             \
    X(vkGetDeviceQueue)            \
    X(vkDeviceWaitIdle)            \
    X(vkQueueWaitIdle)             \
    X(vkQueueSubmit)               \
    X(vkCreateSwapchainKHR)        \
    X(vkDestroySwapchainKHR)       \
    X(vkGetSwapchainImagesKHR)     \
    X(vkAcquireNextImageKHR)       \
    X(vkQueuePresentKHR)           \
    X(vkCreateCommandPool)         \
    X(vkDestroyCommandPool)        \
    X(vkAllocateCommandBuffers)    \
    X(vkBeginCommandBuffer)        \
    X(vkEndCommandBuffer)          \
    X(vkResetCommandBuffer)        \
    X(vkCreateSemaphore)           \
    X(vkDestroySemaphore)          \
    X(vkCreateFence)               \
    X(vkDestroyFence)              \
    X(vkWaitForFences)             \
    X(vkResetFences)               \
    X(vkCreateBuffer)              \
    X(vkDestroyBuffer)             \
    X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory)            \
    X(vkFreeMemory)                \
    X(vkBindBufferMemory)          \
    X(vkMapMemory)                 \
    X(vkUnmapMemory)               \
    X(vkCreateImage)               \
    X(vkDestroyImage)              \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory)           \
    X(vkCmdPipelineBarrier)        \
    X(vkCmdCopyBufferToImage)      \
    X(vkCmdBlitImage)              \
    X(vkCmdClearColorImage)

struct VkApi {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;

#define ARMSX_VK_DECL(name) PFN_##name name = nullptr;
    ARMSX_VK_INSTANCE_FUNCS(ARMSX_VK_DECL)
    ARMSX_VK_DEVICE_FUNCS(ARMSX_VK_DECL)
#undef ARMSX_VK_DECL

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR = nullptr;
#endif

    /* OPTIONAL — deliberately outside ARMSX_VK_INSTANCE_FUNCS, whose loader treats a missing
       entry point as fatal. vkGetPhysicalDeviceProperties2 is core in Vulkan 1.1 and an
       extension before it, so on a 1.0 instance it is legitimately absent and must not stop
       the renderer coming up. It is how we read VkPhysicalDeviceDriverProperties, i.e. the
       driver ID — the only signal precise enough to gate a workaround on (see gpu_profile.h). */
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
};

/* ---- backend state ---------------------------------------------------------------------- */

struct VkRenderer {
    armsx_renderer_t base;

    /* The apiVersion the instance was actually created with. Gates every core-1.1 entry point:
       calling one on a 1.0 instance is undefined and Mesa/Turnip enforces it by dying. */
    uint32_t instance_api_version = VK_API_VERSION_1_0;

    void* library = nullptr;
    VkApi api{};

    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    /* Window-surface rebuild state — see armsx_render_native_window_generation. Vulkan needs
       BOTH the VkSurfaceKHR and the swapchain rebuilt: the surface wraps the ANativeWindow that
       Android destroyed on background, and OUT_OF_DATE handling alone cannot rescue a swapchain
       whose underlying surface is gone. */
    unsigned long window_generation = 0;
    long          surface_rebuilds = 0;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    ANativeWindow* native_window = nullptr;
#endif

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent{0, 0};
    std::vector<VkImage> swapchain_images;
    bool swapchain_valid = false;
    bool vsync = true;

    /* Bring-up trace: the surface's reported rotation, and a handful of fully-logged
       presents so the letterbox can be checked against real numbers from logcat. */
    VkSurfaceTransformFlagBitsKHR surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    int present_traces = 0;
    /* [video] display_rotation: this backend cannot do it; warn once, never per frame. */
    bool rotation_warned = false;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    VkSemaphore release_semaphore = VK_NULL_HANDLE;
    VkFence frame_fence = VK_NULL_HANDLE;

    /* Staging (host visible) + device-local source image, both RGBA8. */
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* staging_mapped = nullptr;
    VkDeviceSize staging_size = 0;

    VkImage frame_image = VK_NULL_HANDLE;
    VkDeviceMemory frame_memory = VK_NULL_HANDLE;
    int frame_width = 0;
    int frame_height = 0;
    bool frame_image_initialized = false;

    Uint32 source_format = SDL_PIXELFORMAT_UNKNOWN;
    int dirty_first = 0;
    int dirty_last = -1;
    bool has_frame = false;

    std::string driver_name;
};

VkRenderer* Self(armsx_renderer_t* base) {
    return static_cast<VkRenderer*>(base->impl);
}

bool Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        armsx_render_log("renderer", "Vulkan: %s failed (VkResult %d)", what, (int)result);
        return false;
    }
    return true;
}

uint32_t FindMemoryType(VkRenderer* self, uint32_t type_bits, VkMemoryPropertyFlags wanted) {
    for (uint32_t index = 0; index < self->memory_properties.memoryTypeCount; ++index) {
        if ((type_bits & (1u << index)) == 0) {
            continue;
        }
        if ((self->memory_properties.memoryTypes[index].propertyFlags & wanted) == wanted) {
            return index;
        }
    }
    return UINT32_MAX;
}

/* ---- CPU format conversion --------------------------------------------------------------- */

/* A1B5G5R5 (bit 15 = mask, 14:10 = B, 9:5 = G, 4:0 = R) -> RGBA8. */
void Convert555Rows(const uint8_t* source, int pitch, int width, int first_row, int rows,
                    uint8_t* dst, size_t dst_pitch) {
    for (int row = 0; row < rows; ++row) {
        const auto* src = reinterpret_cast<const uint16_t*>(source + (static_cast<size_t>(first_row + row) *
                                                                     static_cast<size_t>(pitch)));
        uint8_t* out = dst + (static_cast<size_t>(row) * dst_pitch);
        for (int x = 0; x < width; ++x) {
            const uint16_t value = src[x];
            const uint32_t r = value & 0x1Fu;
            const uint32_t g = (value >> 5) & 0x1Fu;
            const uint32_t b = (value >> 10) & 0x1Fu;
            /* 5 -> 8 bits with the standard replicate-high-bits expansion. */
            out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
            out[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
            out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
            out[3] = 0xFF;
            out += 4;
        }
    }
}

void ConvertRgb24Rows(const uint8_t* source, int pitch, int width, int first_row, int rows,
                      uint8_t* dst, size_t dst_pitch) {
    for (int row = 0; row < rows; ++row) {
        const uint8_t* src = source + (static_cast<size_t>(first_row + row) * static_cast<size_t>(pitch));
        uint8_t* out = dst + (static_cast<size_t>(row) * dst_pitch);
        for (int x = 0; x < width; ++x) {
            out[0] = src[0];
            out[1] = src[1];
            out[2] = src[2];
            out[3] = 0xFF;
            src += 3;
            out += 4;
        }
    }
}

/* ---- setup ------------------------------------------------------------------------------- */

bool LoadLoaderFunctions(VkRenderer* self) {
#if defined(_WIN32)
    return false;
#else
    auto get = [&](const char* name) { return dlsym(self->library, name); };
    self->api.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get("vkGetInstanceProcAddr"));
    if (!self->api.vkGetInstanceProcAddr) {
        armsx_render_log("renderer", "Vulkan: vkGetInstanceProcAddr not found in the loader.");
        return false;
    }

    self->api.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        self->api.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    self->api.vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        self->api.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));

    return self->api.vkCreateInstance != nullptr;
#endif
}

bool CreateInstance(VkRenderer* self) {
    std::vector<const char*> extensions;
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    const bool use_android_surface = armsx_render_native_window() != nullptr;
    if (use_android_surface) {
        extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }
#else
    const bool use_android_surface = false;
#endif

    if (!use_android_surface) {
        if (!self->window) {
            armsx_render_log("renderer", "Vulkan: no window and no native surface to present to.");
            return false;
        }
        unsigned int count = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(self->window, &count, nullptr)) {
            armsx_render_log("renderer", "Vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
            return false;
        }
        std::vector<const char*> sdl_extensions(count);
        if (count && !SDL_Vulkan_GetInstanceExtensions(self->window, &count, sdl_extensions.data())) {
            armsx_render_log("renderer", "Vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
            return false;
        }
        extensions.assign(sdl_extensions.begin(), sdl_extensions.end());
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ARMSX";
    app.applicationVersion = 1;
    app.pEngineName = "ARMSX";
    app.engineVersion = 1;
    /*
        1.1 when the loader supports it, 1.0 otherwise.

        vkGetPhysicalDeviceProperties2 — which is how the driver ID is read — is CORE 1.1.
        Calling it on a 1.0 instance is undefined, and the two drivers disagree about what
        undefined means: the Qualcomm blob returns something and carries on, while Mesa/Turnip
        takes the process down with it. That is the whole "custom drivers crash the app" bug —
        the driver loaded correctly and then this call killed it.

        vkEnumerateInstanceVersion is itself 1.1-only, so its absence IS the 1.0 answer.
    */
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            self->api.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"))) {
        if (enumerate_version(&loader_version) != VK_SUCCESS) {
            loader_version = VK_API_VERSION_1_0;
        }
    }

    self->instance_api_version =
        (loader_version >= VK_API_VERSION_1_1) ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    app.apiVersion = self->instance_api_version;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (!Check(self->api.vkCreateInstance(&info, nullptr, &self->instance), "vkCreateInstance")) {
        return false;
    }

#define ARMSX_VK_LOAD_INSTANCE(name)                                                      \
    self->api.name = reinterpret_cast<PFN_##name>(self->api.vkGetInstanceProcAddr(self->instance, #name)); \
    if (!self->api.name) {                                                                \
        armsx_render_log("renderer", "Vulkan: missing instance entry point %s", #name);   \
        return false;                                                                     \
    }
    ARMSX_VK_INSTANCE_FUNCS(ARMSX_VK_LOAD_INSTANCE)
#undef ARMSX_VK_LOAD_INSTANCE

    /* Core name first, then the pre-1.1 extension name. Absence is not an error: it only means
       we identify the GPU by vendor ID and device name and leave the driver "unknown", which
       every gate already treats as the conservative case. */
    self->api.vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        self->api.vkGetInstanceProcAddr(self->instance, "vkGetPhysicalDeviceProperties2"));
    if (!self->api.vkGetPhysicalDeviceProperties2) {
        self->api.vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            self->api.vkGetInstanceProcAddr(self->instance, "vkGetPhysicalDeviceProperties2KHR"));
    }

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    if (use_android_surface) {
        self->api.vkCreateAndroidSurfaceKHR = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            self->api.vkGetInstanceProcAddr(self->instance, "vkCreateAndroidSurfaceKHR"));
        if (!self->api.vkCreateAndroidSurfaceKHR) {
            armsx_render_log("renderer", "Vulkan: vkCreateAndroidSurfaceKHR unavailable.");
            return false;
        }
    }
#endif

    return true;
}

bool CreateSurface(VkRenderer* self) {
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    if (auto* native = static_cast<ANativeWindow*>(armsx_render_native_window())) {
        VkAndroidSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        info.window = native;
        if (!Check(self->api.vkCreateAndroidSurfaceKHR(self->instance, &info, nullptr, &self->surface),
                   "vkCreateAndroidSurfaceKHR")) {
            return false;
        }
        /* Hold our own reference for the lifetime of the VkSurfaceKHR. */
        ANativeWindow_acquire(native);
        self->native_window = native;
        self->window_generation = armsx_render_native_window_generation();
        armsx_render_set_native_window_claimed(true);
        armsx_render_log("renderer", "Vulkan: Android surface on ANativeWindow %p (%dx%d)",
                         static_cast<void*>(native), ANativeWindow_getWidth(native),
                         ANativeWindow_getHeight(native));
        return true;
    }
#endif

    if (!self->window) {
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(self->window, self->instance, &self->surface)) {
        armsx_render_log("renderer", "Vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool PickPhysicalDevice(VkRenderer* self) {
    uint32_t count = 0;
    if (!Check(self->api.vkEnumeratePhysicalDevices(self->instance, &count, nullptr), "vkEnumeratePhysicalDevices") ||
        count == 0) {
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    if (!Check(self->api.vkEnumeratePhysicalDevices(self->instance, &count, devices.data()),
               "vkEnumeratePhysicalDevices")) {
        return false;
    }

    for (VkPhysicalDevice candidate : devices) {
        uint32_t family_count = 0;
        self->api.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        self->api.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        for (uint32_t family = 0; family < family_count; ++family) {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 present_supported = VK_FALSE;
            self->api.vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, self->surface, &present_supported);
            if (present_supported != VK_TRUE) {
                continue;
            }

            self->physical_device = candidate;
            self->queue_family = family;
            self->api.vkGetPhysicalDeviceMemoryProperties(candidate, &self->memory_properties);

            VkPhysicalDeviceProperties properties{};
            self->api.vkGetPhysicalDeviceProperties(candidate, &properties);
            self->driver_name = properties.deviceName;
            armsx_render_log("renderer",
                             "Vulkan: device=%s api=%u.%u.%u driver_version=0x%08x queue_family=%u",
                             properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
                             VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion),
                             properties.driverVersion, family);

            /* Identify the driver, not just the vendor. driverInfo is the human-readable version
               ("r44p1" on Mali), which is what a bug report can be matched against — the packed
               driverVersion above is unreadable and vendor-specific in its encoding. */
            VkPhysicalDeviceDriverProperties driver_properties{};
            driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            if (self->api.vkGetPhysicalDeviceProperties2 &&
                self->instance_api_version >= VK_API_VERSION_1_1) {
                VkPhysicalDeviceProperties2 properties2{};
                properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties2.pNext = &driver_properties;
                self->api.vkGetPhysicalDeviceProperties2(candidate, &properties2);
            }
            armsx_gpu_profile_note_vk(properties.vendorID, driver_properties.driverID,
                                      properties.deviceName, driver_properties.driverName,
                                      driver_properties.driverInfo);

            char profile_line[320];
            armsx_gpu_profile_describe(profile_line, sizeof(profile_line));
            armsx_render_log("renderer", "GPU profile: %s", profile_line);
            return true;
        }
    }

    armsx_render_log("renderer", "Vulkan: no physical device with a present-capable graphics queue.");
    return false;
}

bool CreateDeviceAndQueue(VkRenderer* self) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = self->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = device_extensions;

    if (!Check(self->api.vkCreateDevice(self->physical_device, &info, nullptr, &self->device), "vkCreateDevice")) {
        return false;
    }

#define ARMSX_VK_LOAD_DEVICE(name)                                                       \
    self->api.name = reinterpret_cast<PFN_##name>(self->api.vkGetDeviceProcAddr(self->device, #name)); \
    if (!self->api.name) {                                                               \
        armsx_render_log("renderer", "Vulkan: missing device entry point %s", #name);    \
        return false;                                                                    \
    }
    ARMSX_VK_DEVICE_FUNCS(ARMSX_VK_LOAD_DEVICE)
#undef ARMSX_VK_LOAD_DEVICE

    self->api.vkGetDeviceQueue(self->device, self->queue_family, 0, &self->queue);
    return self->queue != VK_NULL_HANDLE;
}

bool CreateFrameResources(VkRenderer* self) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = self->queue_family;
    if (!Check(self->api.vkCreateCommandPool(self->device, &pool_info, nullptr, &self->command_pool),
               "vkCreateCommandPool")) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = self->command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (!Check(self->api.vkAllocateCommandBuffers(self->device, &alloc, &self->command_buffer),
               "vkAllocateCommandBuffers")) {
        return false;
    }

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (!Check(self->api.vkCreateSemaphore(self->device, &semaphore_info, nullptr, &self->acquire_semaphore),
               "vkCreateSemaphore") ||
        !Check(self->api.vkCreateSemaphore(self->device, &semaphore_info, nullptr, &self->release_semaphore),
               "vkCreateSemaphore")) {
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return Check(self->api.vkCreateFence(self->device, &fence_info, nullptr, &self->frame_fence), "vkCreateFence");
}

void DestroySwapchain(VkRenderer* self) {
    if (self->device && self->swapchain != VK_NULL_HANDLE) {
        self->api.vkDestroySwapchainKHR(self->device, self->swapchain, nullptr);
    }
    self->swapchain = VK_NULL_HANDLE;
    self->swapchain_images.clear();
    self->swapchain_valid = false;
}

bool CreateSwapchain(VkRenderer* self) {
    VkSurfaceCapabilitiesKHR capabilities{};
    if (!Check(self->api.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(self->physical_device, self->surface,
                                                                   &capabilities),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }

    /* The blit target must accept transfer writes and clears. */
    const VkImageUsageFlags wanted_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((capabilities.supportedUsageFlags & wanted_usage) != wanted_usage) {
        armsx_render_log("renderer",
                         "Vulkan: the surface does not support TRANSFER_DST swapchain images "
                         "(usage 0x%08x); this transfer-only present path cannot run here.",
                         capabilities.supportedUsageFlags);
        return false;
    }

    uint32_t format_count = 0;
    self->api.vkGetPhysicalDeviceSurfaceFormatsKHR(self->physical_device, self->surface, &format_count, nullptr);
    if (format_count == 0) {
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    self->api.vkGetPhysicalDeviceSurfaceFormatsKHR(self->physical_device, self->surface, &format_count,
                                                   formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM || candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = candidate;
            break;
        }
    }

    uint32_t present_mode_count = 0;
    self->api.vkGetPhysicalDeviceSurfacePresentModesKHR(self->physical_device, self->surface,
                                                        &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    if (present_mode_count) {
        self->api.vkGetPhysicalDeviceSurfacePresentModesKHR(self->physical_device, self->surface,
                                                            &present_mode_count, present_modes.data());
    }

    /* FIFO is always available and is the vsync-on mode. Without vsync prefer IMMEDIATE,
       then MAILBOX. */
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!self->vsync) {
        for (VkPresentModeKHR candidate : present_modes) {
            if (candidate == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                present_mode = candidate;
                break;
            }
            if (candidate == VK_PRESENT_MODE_MAILBOX_KHR) {
                present_mode = candidate;
            }
        }
    }

    /* ---- surface transform ---------------------------------------------------------
       A landscape-locked Android app on a portrait-native panel gets
       currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR. Passing that straight
       through as preTransform (which is what this backend used to do) is a PROMISE to the
       presentation engine that the app already rotated its content by 90 degrees, so the
       compositor counter-rotates on scan-out. This backend blits an upright frame, so the
       result was a picture turned 90 degrees on screen.

       We ask for IDENTITY instead. Android's loader always advertises it
       (supportedTransforms = 0x1ff on this device) and it makes the swapchain behave
       exactly like the software blit bridge: buffers in window space, no buffer transform,
       SurfaceFlinger applies the display rotation. Costs one composition pass.

       See the imageExtent block below for the other half of that trade. */
    VkSurfaceTransformFlagBitsKHR pre_transform = capabilities.currentTransform;
    if ((capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
        pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    self->surface_transform = capabilities.currentTransform;

    armsx_render_log("renderer",
                     "Vulkan: surface caps current_extent=%ux%u min=%ux%u max=%ux%u "
                     "current_transform=0x%02x supported_transforms=0x%04x pre_transform=0x%02x "
                     "min_image_count=%u usage=0x%08x",
                     capabilities.currentExtent.width, capabilities.currentExtent.height,
                     capabilities.minImageExtent.width, capabilities.minImageExtent.height,
                     capabilities.maxImageExtent.width, capabilities.maxImageExtent.height,
                     (unsigned)capabilities.currentTransform, (unsigned)capabilities.supportedTransforms,
                     (unsigned)pre_transform, capabilities.minImageCount,
                     capabilities.supportedUsageFlags);

    /* ---- imageExtent ----------------------------------------------------------------
       preTransform = IDENTITY means the presentation engine maps the swapchain image onto
       the Surface with no buffer transform, so the image's pixels ARE the Surface's pixels
       and armsx_render_compute_dst() can letterbox straight against them. That equivalence
       only holds while the extent we create matches the Surface's own geometry.

       currentExtent is not reliable for that on Android. It is whatever the last producer
       asked of the ANativeWindow (the host's CPU blit bridge sets a downscaled framebuffer
       size with ANativeWindow_setBuffersGeometry before we ever get here), and whether it
       is expressed in window space or in pre-transform space is driver-dependent once
       currentTransform is ROTATE_90/270. A portrait extent on a landscape Surface is
       exactly the reported bug: the letterbox gets computed for a 1080x1920 rect — full
       width, bars top and bottom — and SurfaceFlinger then squashes that rect into the
       1920x1080 window, so the bars survive the trip and the picture comes out stretched.

       So prefer the geometry the host published with the Surface itself, which is in the
       app's own (orientation-locked) space; currentExtent stays as the fallback, and as the
       retry if the driver refuses our extent. */
    VkExtent2D reported = capabilities.currentExtent;
    if (reported.width == 0xFFFFFFFFu || reported.height == 0xFFFFFFFFu) {
        int w = 0;
        int h = 0;
        if (self->window) {
            SDL_Vulkan_GetDrawableSize(self->window, &w, &h);
        }
        reported.width = static_cast<uint32_t>(std::max(w, 0));
        reported.height = static_cast<uint32_t>(std::max(h, 0));
    }

    if (reported.width == 0 || reported.height == 0) {
        /* Surface is not presentable right now (backgrounded / zero-sized). */
        return false;
    }

    auto clamp_extent = [&capabilities](VkExtent2D value) {
        value.width = std::clamp(value.width, capabilities.minImageExtent.width,
                                 capabilities.maxImageExtent.width);
        value.height = std::clamp(value.height, capabilities.minImageExtent.height,
                                  capabilities.maxImageExtent.height);
        return value;
    };

    reported = clamp_extent(reported);

    /* Candidates, best first. Duplicates are dropped so the common case is one attempt. */
    std::vector<VkExtent2D> candidates;
    int host_width = 0;
    int host_height = 0;
    if (armsx_render_native_window_size(&host_width, &host_height)) {
        const VkExtent2D host = clamp_extent({static_cast<uint32_t>(host_width),
                                              static_cast<uint32_t>(host_height)});
        if (host.width != reported.width || host.height != reported.height) {
            armsx_render_log("renderer",
                             "Vulkan: host Surface is %dx%d but currentExtent is %ux%u; "
                             "presenting at the host geometry so the letterbox is computed in "
                             "the same space it is displayed in.",
                             host_width, host_height, reported.width, reported.height);
        }
        candidates.push_back(host);
    }
    if (candidates.empty() || candidates.front().width != reported.width ||
        candidates.front().height != reported.height) {
        candidates.push_back(reported);
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkSwapchainKHR old_swapchain = self->swapchain;

    VkExtent2D extent{0, 0};
    VkSwapchainKHR created = VK_NULL_HANDLE;
    /* A failed vkCreateSwapchainKHR still retires oldSwapchain, and a retired swapchain may
       not be offered as oldSwapchain again — so only the first attempt gets it. */
    VkSwapchainKHR reusable = old_swapchain;
    for (const VkExtent2D& candidate : candidates) {
        if (candidate.width == 0 || candidate.height == 0) {
            continue;
        }

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = self->surface;
        info.minImageCount = image_count;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = candidate;
        info.imageArrayLayers = 1;
        info.imageUsage = wanted_usage;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = pre_transform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = present_mode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = reusable;
        reusable = VK_NULL_HANDLE;

        const VkResult result = self->api.vkCreateSwapchainKHR(self->device, &info, nullptr, &created);
        if (result == VK_SUCCESS) {
            extent = candidate;
            break;
        }

        armsx_render_log("renderer", "Vulkan: vkCreateSwapchainKHR(%ux%u) failed (VkResult %d)",
                         candidate.width, candidate.height, (int)result);
        created = VK_NULL_HANDLE;
    }

    if (created == VK_NULL_HANDLE) {
        /* Surface is not presentable right now (backgrounded / zero-sized), or the driver
           refused every geometry we could offer it. Drop the old swapchain rather than leave
           a retired handle behind for the next attempt to offer back — after the in-flight
           frame that may still reference its images has retired. */
        if (old_swapchain != VK_NULL_HANDLE && self->frame_fence != VK_NULL_HANDLE) {
            self->api.vkWaitForFences(self->device, 1, &self->frame_fence, VK_TRUE, UINT64_MAX);
        }
        DestroySwapchain(self);
        return false;
    }

    if (old_swapchain != VK_NULL_HANDLE) {
        self->api.vkDestroySwapchainKHR(self->device, old_swapchain, nullptr);
    }

    self->swapchain = created;
    self->swapchain_format = chosen.format;
    self->swapchain_extent = extent;

    uint32_t actual = 0;
    self->api.vkGetSwapchainImagesKHR(self->device, self->swapchain, &actual, nullptr);
    self->swapchain_images.resize(actual);
    self->api.vkGetSwapchainImagesKHR(self->device, self->swapchain, &actual, self->swapchain_images.data());

    self->swapchain_valid = true;
    self->present_traces = 0;
    armsx_render_log("renderer",
                     "Vulkan: swapchain %ux%u format=%d images=%u present_mode=%d "
                     "pre_transform=0x%02x current_transform=0x%02x",
                     extent.width, extent.height, (int)chosen.format, actual, (int)present_mode,
                     (unsigned)pre_transform, (unsigned)capabilities.currentTransform);
    return true;
}

void DestroyStaging(VkRenderer* self) {
    if (self->staging_mapped) {
        self->api.vkUnmapMemory(self->device, self->staging_memory);
        self->staging_mapped = nullptr;
    }
    if (self->staging_buffer != VK_NULL_HANDLE) {
        self->api.vkDestroyBuffer(self->device, self->staging_buffer, nullptr);
        self->staging_buffer = VK_NULL_HANDLE;
    }
    if (self->staging_memory != VK_NULL_HANDLE) {
        self->api.vkFreeMemory(self->device, self->staging_memory, nullptr);
        self->staging_memory = VK_NULL_HANDLE;
    }
    self->staging_size = 0;
}

void DestroyFrameImage(VkRenderer* self) {
    if (self->frame_image != VK_NULL_HANDLE) {
        self->api.vkDestroyImage(self->device, self->frame_image, nullptr);
        self->frame_image = VK_NULL_HANDLE;
    }
    if (self->frame_memory != VK_NULL_HANDLE) {
        self->api.vkFreeMemory(self->device, self->frame_memory, nullptr);
        self->frame_memory = VK_NULL_HANDLE;
    }
    self->frame_width = 0;
    self->frame_height = 0;
    self->frame_image_initialized = false;
}

bool EnsureStaging(VkRenderer* self, VkDeviceSize size) {
    if (self->staging_size >= size && self->staging_buffer != VK_NULL_HANDLE) {
        return true;
    }

    DestroyStaging(self);

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(self->api.vkCreateBuffer(self->device, &info, nullptr, &self->staging_buffer), "vkCreateBuffer")) {
        return false;
    }

    VkMemoryRequirements requirements{};
    self->api.vkGetBufferMemoryRequirements(self->device, self->staging_buffer, &requirements);

    const uint32_t type_index = FindMemoryType(self, requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type_index == UINT32_MAX) {
        armsx_render_log("renderer", "Vulkan: no host-visible coherent memory type for the staging buffer.");
        return false;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = type_index;
    if (!Check(self->api.vkAllocateMemory(self->device, &alloc, nullptr, &self->staging_memory), "vkAllocateMemory") ||
        !Check(self->api.vkBindBufferMemory(self->device, self->staging_buffer, self->staging_memory, 0),
               "vkBindBufferMemory") ||
        !Check(self->api.vkMapMemory(self->device, self->staging_memory, 0, VK_WHOLE_SIZE, 0, &self->staging_mapped),
               "vkMapMemory")) {
        return false;
    }

    self->staging_size = size;
    return true;
}

bool EnsureFrameImage(VkRenderer* self, int width, int height) {
    if (self->frame_image != VK_NULL_HANDLE && self->frame_width == width && self->frame_height == height) {
        return true;
    }

    self->api.vkDeviceWaitIdle(self->device);
    DestroyFrameImage(self);

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* SAMPLED is for the shader chain (frontend/render_shaders.cpp), which samples this
       image directly at internal_scale 1 rather than copying it first. Vulkan's mandatory
       format table guarantees optimal-tiling SAMPLED_IMAGE for R8G8B8A8_UNORM on every
       implementation, so this cannot narrow device support, and an unused usage bit costs
       nothing when no chain is active. */
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (!Check(self->api.vkCreateImage(self->device, &info, nullptr, &self->frame_image), "vkCreateImage")) {
        return false;
    }

    VkMemoryRequirements requirements{};
    self->api.vkGetImageMemoryRequirements(self->device, self->frame_image, &requirements);
    const uint32_t type_index = FindMemoryType(self, requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_index == UINT32_MAX) {
        armsx_render_log("renderer", "Vulkan: no device-local memory type for the frame image.");
        return false;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = type_index;
    if (!Check(self->api.vkAllocateMemory(self->device, &alloc, nullptr, &self->frame_memory), "vkAllocateMemory") ||
        !Check(self->api.vkBindImageMemory(self->device, self->frame_image, self->frame_memory, 0),
               "vkBindImageMemory")) {
        return false;
    }

    self->frame_width = width;
    self->frame_height = height;
    self->frame_image_initialized = false;
    return true;
}

void ImageBarrier(VkRenderer* self, VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                  VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
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
    self->api.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

/* ---- ops --------------------------------------------------------------------------------- */

const char* OpDriverName(armsx_renderer_t* base) {
    return Self(base)->driver_name.c_str();
}

bool OpIsAccelerated(armsx_renderer_t*) {
    return true;
}

SDL_Renderer* OpSdlHandle(armsx_renderer_t*) {
    return nullptr;
}

void OpResize(armsx_renderer_t* base, int, int) {
    Self(base)->swapchain_valid = false;
}

void OpOutputSize(armsx_renderer_t* base, int* width, int* height) {
    VkRenderer* self = Self(base);
    if (width) {
        *width = static_cast<int>(self->swapchain_extent.width);
    }
    if (height) {
        *height = static_cast<int>(self->swapchain_extent.height);
    }
}

void OpSetVsync(armsx_renderer_t* base, bool enabled) {
    VkRenderer* self = Self(base);
    if (self->vsync == enabled) {
        return;
    }
    self->vsync = enabled;
    self->swapchain_valid = false; /* present mode is baked into the swapchain */
}

bool OpUploadFrame(armsx_renderer_t* base,
                   const void* pixels,
                   int width,
                   int height,
                   int pitch,
                   Uint32 sdl_format,
                   int dirty_first_row,
                   int dirty_last_row) {
    VkRenderer* self = Self(base);
    if (!pixels || width <= 0 || height <= 0 || pitch <= 0) {
        return false;
    }
    if (sdl_format != SDL_PIXELFORMAT_BGR555 && sdl_format != SDL_PIXELFORMAT_RGB24) {
        armsx_render_log("renderer", "Vulkan backend cannot upload %s", SDL_GetPixelFormatName(sdl_format));
        return false;
    }

    if (!EnsureFrameImage(self, width, height)) {
        return false;
    }

    const size_t dst_pitch = static_cast<size_t>(width) * 4u;
    if (!EnsureStaging(self, static_cast<VkDeviceSize>(dst_pitch) * static_cast<VkDeviceSize>(height))) {
        return false;
    }

    if (!self->frame_image_initialized) {
        /* Undefined image contents: the whole thing has to be written this frame. */
        dirty_first_row = 0;
        dirty_last_row = height - 1;
    }

    if (dirty_last_row < dirty_first_row) {
        self->dirty_last = -1;
        self->source_format = sdl_format;
        return true;
    }

    dirty_first_row = std::max(dirty_first_row, 0);
    dirty_last_row = std::min(dirty_last_row, height - 1);
    const int rows = dirty_last_row - dirty_first_row + 1;

    auto* staging = static_cast<uint8_t*>(self->staging_mapped) +
                    (static_cast<size_t>(dirty_first_row) * dst_pitch);
    const auto* source = static_cast<const uint8_t*>(pixels);

    if (sdl_format == SDL_PIXELFORMAT_BGR555) {
        Convert555Rows(source, pitch, width, dirty_first_row, rows, staging, dst_pitch);
    } else {
        ConvertRgb24Rows(source, pitch, width, dirty_first_row, rows, staging, dst_pitch);
    }

    self->source_format = sdl_format;
    self->dirty_first = dirty_first_row;
    self->dirty_last = dirty_last_row;
    self->has_frame = true;
    return true;
}

bool BeginFrame(VkRenderer* self, uint32_t* image_index) {
    /* A new ANativeWindow means our VkSurfaceKHR wraps a destroyed object. Tear down the
       swapchain FIRST (it references the surface), then the surface, then rebuild both. The
       device, pipelines and all resident images survive, so a resume costs one surface plus
       one swapchain — not a renderer restart. */
    {
        const unsigned long generation = armsx_render_native_window_generation();

        if (generation != self->window_generation) {
            void* const window = armsx_render_native_window();

            self->api.vkDeviceWaitIdle(self->device);
            DestroySwapchain(self);
            self->swapchain_valid = false;

            if (self->surface != VK_NULL_HANDLE) {
                self->api.vkDestroySurfaceKHR(self->instance, self->surface, nullptr);
                self->surface = VK_NULL_HANDLE;
            }

            /* Our own reference goes with the surface it was taken for. Dropped only AFTER
               vkDestroySurfaceKHR, and cleared here rather than reassigned so the early return
               below cannot leave OpShutdown holding a window this renderer no longer uses. */
            if (self->native_window) {
                ANativeWindow_release(self->native_window);
                self->native_window = nullptr;
            }

            /* Backgrounded: no window to build on. Retry on a later frame and deliberately do
               NOT consume the generation, so the eventual resume still triggers a rebuild. */
            if (!window) {
                return false;
            }

            VkAndroidSurfaceCreateInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
            info.window = static_cast<ANativeWindow*>(window);

            if (!Check(self->api.vkCreateAndroidSurfaceKHR(self->instance, &info, nullptr,
                                                           &self->surface),
                       "vkCreateAndroidSurfaceKHR (rebuild)")) {
                self->surface = VK_NULL_HANDLE;
                return false;
            }

            /* Same contract as CreateSurface(): hold a reference for the lifetime of the
               VkSurfaceKHR, and re-raise the claim so the host's CPU blit bridge stays parked.
               (It is the backend's flag to hold; nothing else re-raises it after a resume.) */
            ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
            self->native_window = static_cast<ANativeWindow*>(window);
            armsx_render_set_native_window_claimed(true);

            self->window_generation = generation;
            armsx_render_log("renderer", "Vulkan: surface rebuilt after resume (#%ld) window=%p",
                             ++self->surface_rebuilds, window);
        }
    }

    if (!self->swapchain_valid && !CreateSwapchain(self)) {
        return false;
    }

    self->api.vkWaitForFences(self->device, 1, &self->frame_fence, VK_TRUE, UINT64_MAX);

    VkResult result = self->api.vkAcquireNextImageKHR(self->device, self->swapchain, UINT64_MAX,
                                                      self->acquire_semaphore, VK_NULL_HANDLE, image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        self->swapchain_valid = false;
        if (!CreateSwapchain(self)) {
            return false;
        }
        result = self->api.vkAcquireNextImageKHR(self->device, self->swapchain, UINT64_MAX,
                                                 self->acquire_semaphore, VK_NULL_HANDLE, image_index);
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        armsx_render_log("renderer", "Vulkan: vkAcquireNextImageKHR failed (VkResult %d)", (int)result);
        self->swapchain_valid = false;
        return false;
    }

    self->api.vkResetFences(self->device, 1, &self->frame_fence);
    return true;
}

void OpPresent(armsx_renderer_t* base, const armsx_render_frame_params_t* params) {
    VkRenderer* self = Self(base);
    if (!self->has_frame || self->frame_image == VK_NULL_HANDLE) {
        return;
    }
    if (!self->frame_image_initialized && self->dirty_last < self->dirty_first) {
        /* Nothing has ever been written into the frame image; blitting from an UNDEFINED
           layout would be undefined behaviour. */
        return;
    }

    uint32_t image_index = 0;
    if (!BeginFrame(self, &image_index) || image_index >= self->swapchain_images.size()) {
        return;
    }

    VkCommandBuffer cmd = self->command_buffer;
    self->api.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(self->api.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer")) {
        return;
    }

    /* 1. Staging -> frame image (dirty rows only). */
    const bool needs_copy = self->dirty_last >= self->dirty_first;
    if (needs_copy) {
        ImageBarrier(self, cmd, self->frame_image,
                     self->frame_image_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                   : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     self->frame_image_initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        const int rows = self->dirty_last - self->dirty_first + 1;
        VkBufferImageCopy copy{};
        copy.bufferOffset = static_cast<VkDeviceSize>(self->dirty_first) *
                            static_cast<VkDeviceSize>(self->frame_width) * 4u;
        copy.bufferRowLength = static_cast<uint32_t>(self->frame_width);
        copy.bufferImageHeight = static_cast<uint32_t>(rows);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {0, self->dirty_first, 0};
        copy.imageExtent = {static_cast<uint32_t>(self->frame_width), static_cast<uint32_t>(rows), 1};
        self->api.vkCmdCopyBufferToImage(cmd, self->staging_buffer, self->frame_image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        ImageBarrier(self, cmd, self->frame_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        self->frame_image_initialized = true;
    }

    VkImage target = self->swapchain_images[image_index];

    /* 2. Swapchain image -> TRANSFER_DST, clear the letterbox bars. */
    ImageBarrier(self, cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkClearColorValue clear{};
    clear.float32[0] = 0.0f;
    clear.float32[1] = 0.0f;
    clear.float32[2] = 0.0f;
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    self->api.vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    /* 3. Scale-blit into the letterboxed destination rect. */
    SDL_Rect dst{0, 0, 0, 0};
    armsx_render_compute_dst(static_cast<int>(self->swapchain_extent.width),
                             static_cast<int>(self->swapchain_extent.height), self->frame_width,
                             self->frame_height, params, &dst);

    if (self->present_traces < 3) {
        self->present_traces++;
        int host_width = 0;
        int host_height = 0;
        const bool have_host = armsx_render_native_window_size(&host_width, &host_height);
        armsx_render_log("renderer",
                         "Vulkan present #%d surface=%dx%d swapchain=%ux%u current_transform=0x%02x "
                         "source=%dx%d aspect=%.4f stretch=%s dst=%d,%d %dx%d",
                         self->present_traces, have_host ? host_width : -1,
                         have_host ? host_height : -1, self->swapchain_extent.width,
                         self->swapchain_extent.height, (unsigned)self->surface_transform,
                         self->frame_width, self->frame_height, params ? params->aspect : 0.0f,
                         (params && params->stretch) ? "true" : "false", dst.x, dst.y, dst.w, dst.h);
    }

    if (dst.w > 0 && dst.h > 0) {
        /* A RetroArch (.slangp) chain gets first refusal. It returns false whenever there
           is no chain, the preset failed to compile, or librashader is unavailable — and
           then the ordinary blit below runs, so a broken preset degrades to normal
           presentation rather than a black screen. It is layout-transparent: both images
           come back in the layouts they went in, so the barrier to PRESENT_SRC_KHR after
           this block is correct either way. */
        armsx_shader_vk_frame_t chain_frame{};
        chain_frame.cmd = cmd;
        chain_frame.source = self->frame_image;
        chain_frame.source_format = VK_FORMAT_R8G8B8A8_UNORM;
        chain_frame.source_width = self->frame_width;
        chain_frame.source_height = self->frame_height;
        chain_frame.target = target;
        chain_frame.target_width = static_cast<int>(self->swapchain_extent.width);
        chain_frame.target_height = static_cast<int>(self->swapchain_extent.height);
        chain_frame.dst_x = dst.x;
        chain_frame.dst_y = dst.y;
        chain_frame.dst_w = dst.w;
        chain_frame.dst_h = dst.h;

        if (!armsx_shader_vk_render(&chain_frame)) {
            /* [video] overscan_crop maps straight onto srcOffsets. [video] display_rotation
               does NOT: vkCmdBlitImage cannot rotate and this backend deliberately hands the
               compositor an upright frame (see the preTransform note above). Say so once and
               present upright rather than present something wrong — the OpenGL and SDL
               backends both do rotate. */
            int sx0 = 0, sy0 = 0, sx1 = self->frame_width, sy1 = self->frame_height;

            if (params && params->crop_w > 0 && params->crop_h > 0) {
                sx0 = params->crop_x < 0 ? 0 : params->crop_x;
                sy0 = params->crop_y < 0 ? 0 : params->crop_y;
                if (sx0 > self->frame_width - 1) sx0 = self->frame_width - 1;
                if (sy0 > self->frame_height - 1) sy0 = self->frame_height - 1;
                sx1 = sx0 + params->crop_w;
                sy1 = sy0 + params->crop_h;
                if (sx1 > self->frame_width) sx1 = self->frame_width;
                if (sy1 > self->frame_height) sy1 = self->frame_height;
            }

            if (params && (params->rotation & 3) && !self->rotation_warned) {
                self->rotation_warned = true;
                armsx_render_log("renderer",
                                 "Vulkan present: display rotation is not supported by the "
                                 "blit path; presenting upright. Use the OpenGL backend.");
            }

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {sx0, sy0, 0};
            blit.srcOffsets[1] = {sx1, sy1, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {dst.x, dst.y, 0};
            blit.dstOffsets[1] = {dst.x + dst.w, dst.y + dst.h, 1};

            self->api.vkCmdBlitImage(cmd, self->frame_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                     (params && params->linear_filter) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
    }

    ImageBarrier(self, cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (!Check(self->api.vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) {
        return;
    }

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &self->acquire_semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &self->release_semaphore;

    bool submitted;
    {
        /* The shader chain is built on a worker thread which uploads its LUTs through THIS
           queue. A VkQueue may not be used from two threads at once. Uncontended when nothing
           is compiling, which is almost always. */
        ShaderQueueGuard guard;
        submitted = Check(self->api.vkQueueSubmit(self->queue, 1, &submit, self->frame_fence), "vkQueueSubmit");
    }
    if (!submitted) {
        return;
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &self->release_semaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &self->swapchain;
    present.pImageIndices = &image_index;

    /* VK_SUBOPTIMAL_KHR is the PERMANENT steady state here, not a transient one: asking for
       an IDENTITY preTransform on a surface whose currentTransform is ROTATE_90 is exactly
       what "suboptimal" means, and it is the trade we chose above. Recreating the swapchain
       on it rebuilds every single frame and never converges (that thrash is itself a black
       screen). Only OUT_OF_DATE — a genuinely stale surface — forces a rebuild. */
    VkResult result;
    {
        ShaderQueueGuard guard;
        result = self->api.vkQueuePresentKHR(self->queue, &present);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        self->swapchain_valid = false;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        armsx_render_log("renderer", "Vulkan: vkQueuePresentKHR failed (VkResult %d)", (int)result);
        self->swapchain_valid = false;
    }

    /* The dirty span has been consumed. */
    self->dirty_last = -1;
    self->dirty_first = 0;
}

void OpPresentBlank(armsx_renderer_t* base) {
    VkRenderer* self = Self(base);

    uint32_t image_index = 0;
    if (!BeginFrame(self, &image_index) || image_index >= self->swapchain_images.size()) {
        return;
    }

    VkCommandBuffer cmd = self->command_buffer;
    self->api.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(self->api.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer")) {
        return;
    }

    VkImage target = self->swapchain_images[image_index];
    ImageBarrier(self, cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkClearColorValue clear{};
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    self->api.vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    ImageBarrier(self, cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (!Check(self->api.vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) {
        return;
    }

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &self->acquire_semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &self->release_semaphore;
    bool submitted;
    {
        /* The shader chain is built on a worker thread which uploads its LUTs through THIS
           queue. A VkQueue may not be used from two threads at once. Uncontended when nothing
           is compiling, which is almost always. */
        ShaderQueueGuard guard;
        submitted = Check(self->api.vkQueueSubmit(self->queue, 1, &submit, self->frame_fence), "vkQueueSubmit");
    }
    if (!submitted) {
        return;
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &self->release_semaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &self->swapchain;
    present.pImageIndices = &image_index;
    VkResult result;
    {
        ShaderQueueGuard guard;
        result = self->api.vkQueuePresentKHR(self->queue, &present);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        self->swapchain_valid = false;
    }
}

void OpShutdown(armsx_renderer_t* base) {
    VkRenderer* self = Self(base);

    /* BEFORE vkDestroyDevice, and before anything else is torn down: the shader chain owns
       images, views, framebuffers, pipelines and descriptor pools created against this
       VkDevice. Safe to call unconditionally — it is a no-op when nothing ever attached,
       which includes every failure path out of armsx_render_create_vk(). */
    armsx_shader_vk_detach();

    if (self->device != VK_NULL_HANDLE && self->api.vkDeviceWaitIdle) {
        self->api.vkDeviceWaitIdle(self->device);

        DestroyStaging(self);
        DestroyFrameImage(self);
        DestroySwapchain(self);

        if (self->frame_fence != VK_NULL_HANDLE) {
            self->api.vkDestroyFence(self->device, self->frame_fence, nullptr);
        }
        if (self->acquire_semaphore != VK_NULL_HANDLE) {
            self->api.vkDestroySemaphore(self->device, self->acquire_semaphore, nullptr);
        }
        if (self->release_semaphore != VK_NULL_HANDLE) {
            self->api.vkDestroySemaphore(self->device, self->release_semaphore, nullptr);
        }
        if (self->command_pool != VK_NULL_HANDLE) {
            self->api.vkDestroyCommandPool(self->device, self->command_pool, nullptr);
        }
        self->api.vkDestroyDevice(self->device, nullptr);
    }

    if (self->instance != VK_NULL_HANDLE) {
        if (self->surface != VK_NULL_HANDLE && self->api.vkDestroySurfaceKHR) {
            self->api.vkDestroySurfaceKHR(self->instance, self->surface, nullptr);
        }
        if (self->api.vkDestroyInstance) {
            self->api.vkDestroyInstance(self->instance, nullptr);
        }
    }

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    if (self->native_window) {
        ANativeWindow_release(self->native_window);
        self->native_window = nullptr;
    }
#endif

    armsx_render_set_native_window_claimed(false);
    armsx_render_set_active_name("");
    CloseVulkanLibrary(self->library);
    delete self;
}

const armsx_render_ops_t kVkOps = {
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
};

} // namespace

extern "C" {

armsx_renderer_t* armsx_render_create_vk(SDL_Window* window, const armsx_render_config_t* config) {
    auto* self = new VkRenderer();
    self->base.ops = &kVkOps;
    self->base.impl = self;
    self->base.backend = ARMSX_RENDER_BACKEND_VULKAN;
    self->window = window;
    self->vsync = config ? config->vsync : true;
    self->driver_name = "Vulkan";

    self->library = OpenVulkanLibrary();
    if (!self->library) {
        armsx_render_log("renderer", "Vulkan: no loader could be opened.");
        delete self;
        return nullptr;
    }

    if (!LoadLoaderFunctions(self) || !CreateInstance(self) || !CreateSurface(self) ||
        !PickPhysicalDevice(self) || !CreateDeviceAndQueue(self) || !CreateFrameResources(self) ||
        !CreateSwapchain(self)) {
        OpShutdown(&self->base);
        return nullptr;
    }

    /* Hand the shader layer the live device. Everything it needs is explicit here because
       VkRenderer is namespace-private to this file; nothing about librashader leaks in.
       vkGetInstanceProcAddr rather than a linked entry point is what keeps an adrenotools
       custom driver substitutable all the way through the chain. */
    armsx_shader_vk_device_t shader_device{};
    shader_device.get_instance_proc_addr = self->api.vkGetInstanceProcAddr;
    shader_device.instance = self->instance;
    shader_device.physical_device = self->physical_device;
    shader_device.device = self->device;
    shader_device.queue = self->queue;
    shader_device.queue_family = self->queue_family;
    armsx_shader_vk_attach(&shader_device);

    /* The name the OSD and the driver manager show. "custom driver" only ever appears when a
       caller-supplied loader handle is what we are genuinely running on. */
    const std::string name = "Vulkan (" + self->driver_name +
                             (g_custom_loader_active ? ", custom driver)" : ")");
    armsx_render_set_active_name(name.c_str());

    armsx_render_log("renderer", "Vulkan backend created: %s", name.c_str());
    return &self->base;
}

void armsx_render_set_vulkan_loader(armsx_vk_library_open_fn open_fn, void* user) {
    std::lock_guard<std::mutex> lock(g_loader_lock);
    g_loader_fn = open_fn;
    g_loader_user = user;
}

} // extern "C"

#else /* !ARMSX_ENABLE_VULKAN */

extern "C" {

armsx_renderer_t* armsx_render_create_vk(SDL_Window*, const armsx_render_config_t*) {
    return nullptr;
}

void armsx_render_set_vulkan_loader(armsx_vk_library_open_fn, void*) {}

} // extern "C"

#endif
