/*
    librashader — minimal C API surface, hand-written.

    ============================== WHY HAND-WRITTEN ==============================
    Upstream generates `librashader.h` with cbindgen from `build.rs`. cbindgen is not
    installed here and the generated header is NOT part of the published crate, so there
    is nothing to vendor. Every declaration below was read directly out of
    librashader-capi-0.12.0's own source (`src/ctypes.rs`, `src/error.rs`,
    `src/version.rs`, `src/presets.rs`, `src/runtime/vk/filter_chain.rs`) — all the
    structs are `#[repr(C)]`, so the layouts here are the ABI.

    This covers ONLY the entry points frontend/render_shaders.cpp calls: preset loading,
    parameter enumeration, and the Vulkan filter chain. The D3D/Metal/OpenGL/wgpu runtimes
    are not built into our .so at all (`--no-default-features --features runtime-vulkan`).

    ================================ LICENCE =====================================
    librashader is "MPL-2.0 OR GPL-3.0-only". ARMSX takes the MPL-2.0 arm. MPL is
    FILE-level copyleft: it permits linking into a larger work under other terms as long
    as librashader's own source stays available and the notice is preserved. See
    ../NOTICE and ../source/.

    =============================== ABI PINNING ==================================
    LIBRASHADER_CURRENT_ABI is the *struct layout* version and is what
    `libra_instance_abi_version()` reports. Load-time check in render_shaders.cpp refuses
    anything else rather than reading a differently-shaped struct — a mismatch there is a
    silent memory-corruption bug, not a clean failure.

    LIBRASHADER_CURRENT_VERSION is the *option-struct* version: every `*_opt_t` carries a
    `version` field, and librashader only reads the fields that version introduced. Set it
    to the version the header was written against (5) and the fields below are exactly the
    ones that get read.
*/

#ifndef ARMSX_LIBRASHADER_MIN_H
#define ARMSX_LIBRASHADER_MIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBRASHADER_CURRENT_VERSION ((size_t)5)
#define LIBRASHADER_CURRENT_ABI     ((size_t)2)

/* Opaque handles. Each is `Option<NonNull<T>>` on the Rust side, i.e. a nullable pointer,
   so a plain pointer here is layout-correct. They are passed BY POINTER to the functions
   that consume or mutate them (`libra_shader_preset_t*`), which is why the typedefs are
   the pointer itself and not the pointee. */
typedef void* libra_shader_preset_t;
typedef void* libra_vk_filter_chain_t;

/* NULL == success. Anything else is an owned error object; free it with
   libra_error_free(). */
typedef void* libra_error_t;

typedef struct libra_image_vk_t {
    VkImage  handle;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} libra_image_vk_t;

typedef struct libra_viewport_t {
    float    x;
    float    y;
    uint32_t width;
    uint32_t height;
} libra_viewport_t;

typedef struct libra_device_vk_t {
    VkPhysicalDevice          physical_device;
    VkInstance                instance;
    VkDevice                  device;
    VkQueue                   queue;   /* NULL = librashader picks a graphics queue */
    PFN_vkGetInstanceProcAddr entry;
} libra_device_vk_t;

/* One `#pragma parameter` line, resolved across the preset's whole chain.
   `maximum == minimum` means the author declared a CAPTION, not a control. */
typedef struct libra_preset_param_t {
    const char* name;
    const char* description;
    float       initial;
    float       minimum;
    float       maximum;
    float       step;
} libra_preset_param_t;

typedef struct libra_preset_param_list_t {
    const libra_preset_param_t* parameters;
    uint64_t                    length;
} libra_preset_param_list_t;

/* filter_chain_vk_opt_t. `version` MUST be set (0 would mean "read nothing"). */
typedef struct filter_chain_vk_opt_t {
    size_t   version;
    uint32_t frames_in_flight;      /* 0 -> librashader default of 3 */
    bool     force_no_mipmaps;
    bool     use_dynamic_rendering; /* MUST stay false: our VkDevice has no VK_KHR_dynamic_rendering */
    bool     disable_cache;
} filter_chain_vk_opt_t;

/* ---- preset ---------------------------------------------------------------------- */

/* Parse a .slangp. `out` receives the handle. */
typedef libra_error_t (*PFN_libra_preset_create)(const char* filename, libra_shader_preset_t* out);

/* Free a preset. NOTE: libra_vk_filter_chain_create CONSUMES the preset — do not free it
   afterwards. Only free a preset that was never handed to a chain. */
typedef libra_error_t (*PFN_libra_preset_free)(libra_shader_preset_t* preset);

/* Enumerate parameters. Does NOT consume the preset. The returned list borrows from the
   preset and must be released with libra_preset_free_runtime_params. */
typedef libra_error_t (*PFN_libra_preset_get_runtime_params)(const libra_shader_preset_t* preset,
                                                             libra_preset_param_list_t* out);
typedef libra_error_t (*PFN_libra_preset_free_runtime_params)(libra_preset_param_list_t list);

/* ---- Vulkan filter chain --------------------------------------------------------- */

/* CONSUMES `preset`. Does GPU-side work (LUT uploads) on `vulkan.queue` internally, so it
   must not race another submit on that queue. */
typedef libra_error_t (*PFN_libra_vk_filter_chain_create)(libra_shader_preset_t* preset,
                                                          libra_device_vk_t vulkan,
                                                          const filter_chain_vk_opt_t* options,
                                                          libra_vk_filter_chain_t* out);

/* Records the chain into `command_buffer`.
     * `image` must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
     * `out` must be in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, and STAYS there —
       librashader emits no barrier for the final pass; the caller transitions it.
     * MUST NOT be called inside a render pass.
     * Single-threaded: one thread at a time, and the same thread as set_param. */
typedef libra_error_t (*PFN_libra_vk_filter_chain_frame)(libra_vk_filter_chain_t* chain,
                                                         VkCommandBuffer command_buffer,
                                                         size_t frame_count,
                                                         libra_image_vk_t image,
                                                         libra_image_vk_t out,
                                                         const libra_viewport_t* viewport,
                                                         const float* mvp,
                                                         const void* options);

typedef libra_error_t (*PFN_libra_vk_filter_chain_set_param)(libra_vk_filter_chain_t* chain,
                                                             const char* param_name,
                                                             float value);

typedef libra_error_t (*PFN_libra_vk_filter_chain_free)(libra_vk_filter_chain_t* chain);

/* ---- errors ----------------------------------------------------------------------- */

typedef int32_t LIBRA_ERRNO;

typedef LIBRA_ERRNO (*PFN_libra_error_errno)(libra_error_t error);
/* Writes a NUL-terminated message into a librashader-owned buffer; release with
   libra_error_free_string. Returns 0 on success. */
typedef int32_t (*PFN_libra_error_write)(libra_error_t error, char** out);
typedef int32_t (*PFN_libra_error_free_string)(char** out);
typedef int32_t (*PFN_libra_error_free)(libra_error_t* error);

/* ---- version ---------------------------------------------------------------------- */

typedef size_t (*PFN_libra_instance_abi_version)(void);
typedef size_t (*PFN_libra_instance_api_version)(void);

#ifdef __cplusplus
}
#endif

#endif /* ARMSX_LIBRASHADER_MIN_H */
