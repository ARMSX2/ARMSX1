/*
    ARMSX — mobile GPU identification and per-driver behaviour profile.

    WHY THIS EXISTS
    ---------------
    Every mobile GPU quirk worth knowing is a property of a *driver*, not of a vendor. ARMSX2
    learned that expensively: a disable applied across all of Qualcomm to work around a stall
    measured on Mesa/Turnip also caught the Snapdragon 8 Elite's proprietary driver, where the
    workaround was pure overhead and produced a reported busy-scene regression. The fix was to
    key the gate on the driver ID rather than the vendor ID.

    So: identify precisely, gate narrowly, and never widen a gate past the hardware it was
    actually observed on.

    WHAT IS AND IS NOT ENCODED HERE
    -------------------------------
    Every flag below records something *observed on real hardware*. None of it is inferred from
    a spec, and none of it is a guess about a device nobody has run. Where a rule rests on an
    unconfirmed constant (the Xclipse vendor ID in particular) it is called out at the flag and
    is inert until a real device proves it fires — which is what `armsx_gpu_profile_override`
    is for: a tester can force a profile and tell us whether it helped.

    This module deliberately has NO dependencies — not on SDL, not on the renderer, not on the
    logger. It takes strings and integers in, and answers questions. That keeps it callable
    from the GL backend, the Vulkan backend and the hardware rasteriser alike, all of which
    learn about the GPU at different moments and through different APIs.
*/

#ifndef ARMSX_GPU_PROFILE_H
#define ARMSX_GPU_PROFILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan vendor IDs (PCI-style, as reported by VkPhysicalDeviceProperties::vendorID). */
#define ARMSX_GPU_VENDOR_ID_ARM        0x13B5u  /* Mali / Immortalis */
#define ARMSX_GPU_VENDOR_ID_QUALCOMM   0x5143u  /* Adreno */
#define ARMSX_GPU_VENDOR_ID_IMGTEC     0x1010u  /* PowerVR */
#define ARMSX_GPU_VENDOR_ID_SAMSUNG    0x144Du  /* Xclipse (Exynos, AMD RDNA2) — see note below */

typedef enum {
    ARMSX_GPU_VENDOR_UNKNOWN = 0,
    ARMSX_GPU_VENDOR_ADRENO,
    ARMSX_GPU_VENDOR_MALI,
    ARMSX_GPU_VENDOR_POWERVR,
    ARMSX_GPU_VENDOR_XCLIPSE,
} armsx_gpu_vendor_t;

/*
    Mirrors the subset of VkDriverId we actually distinguish. Kept as our own enum so the GL
    backend — which has no VkDriverId — can populate the same field from driver strings, and so
    nothing outside render_vk.cpp needs the Vulkan headers.
*/
typedef enum {
    ARMSX_GPU_DRIVER_UNKNOWN = 0,
    ARMSX_GPU_DRIVER_QUALCOMM_PROPRIETARY,
    ARMSX_GPU_DRIVER_MESA_TURNIP,
    ARMSX_GPU_DRIVER_ARM_PROPRIETARY,
    ARMSX_GPU_DRIVER_SAMSUNG_PROPRIETARY,
    ARMSX_GPU_DRIVER_IMGTEC_PROPRIETARY,
} armsx_gpu_driver_t;

typedef struct {
    armsx_gpu_vendor_t vendor;
    armsx_gpu_driver_t driver;

    /* Architecture generation: 6 for Adreno 6xx, 7 for 7xx, 8 for 8xx. For Mali this is the
       Valhall/Bifrost generation where it can be told from the model number, else 0. */
    int  generation;
    /* Model number as printed: 650, 740, 830 for Adreno; 57, 77, 615, 720 for Mali-G*. 0 if
       it could not be parsed — never assume 0 means "old". */
    int  model;

    int  is_mediatek;   /* MediaTek SoC (matters for Mali: see fbfetch_trustworthy) */
    int  is_angle;      /* running under ANGLE, which translates GLES onto Vulkan */
    int  forced;        /* profile came from the user override, not from detection */

    char name[128];         /* device name as reported */
    char driver_name[64];   /* VkPhysicalDeviceDriverProperties::driverName, when available */
    char driver_info[128];  /* ...::driverInfo, the human driver version (e.g. "r44p1") */

    /* ---------------------------------------------------------------------------------
       Observed-behaviour flags. Each is a fact someone measured, with the device noted.
       --------------------------------------------------------------------------------- */

    /* Mali's driver null-dereferences inside vkCmdPushDescriptorSetKHR on the first textured
       draw despite advertising VK_KHR_push_descriptor. On Adreno the extension works, but on
       Mesa/Turnip the per-draw texture-rebind path stalls; the proprietary Qualcomm driver
       (8-series flagships) is fine and is *penalised* by the descriptor-set fallback. Hence
       driver-keyed, not vendor-keyed. */
    int avoid_push_descriptors;

    /* Adreno 650 (Snapdragon 855/865-era driver) tears out a persistently-mapped buffer.
       Deliberately narrow: applying this across all Adreno was a large throughput regression,
       because every newer part keeps the fast persistent path. */
    int avoid_persistent_buffer_map;

    /* Mali forces a pipeline sync on glBufferSubData; orphaning via glBufferData is faster. */
    int prefer_buffer_orphaning;

    /* Destination-colour readback splits by API, and merging the two is a trap worth naming:
       the same GPU can be good at one and bad at the other.

       fbfetch_gl  — GL_EXT_shader_framebuffer_fetch under GLES.
       fbfetch_vk_roaa — raster-order attachment access under Vulkan.

       Adreno is the case that makes the split necessary. Its GLES framebuffer fetch works
       (subject to single_fbfetch_attachment below), while its proprietary Vulkan driver
       performs STALE ROAA reads — which is why ARMSX2 ships Vulkan fbfetch opt-in on Adreno
       yet uses the GL path freely. A single merged flag disables the working GLES path on the
       most common Android GPU, and does it silently, since the fallback still renders.

       Known-bad, all observed: MediaTek Mali across GPU generations and the Mali-G57 return
       zero or stale destination colour (black or intermittently missing textures). Samsung
       Xclipse has no working ROAA-based fetch. Mesa/Turnip does not exhibit the proprietary
       Adreno blob's stale reads and is trusted under Vulkan. Under ANGLE the GLES path is
       untrusted: ANGLE gives a GLES 3.1 context and framebuffer fetch there has been seen to
       crash outright rather than degrade. */
    int fbfetch_gl;
    int fbfetch_vk_roaa;

    /* Hardware exposes dual-source blending. Mali does not, and a renderer that assumes it
       silently produces wrong output rather than failing. */
    int dual_source_blend;

    /* ANGLE exposes a GLES *3.1* context, so every GLES 3.2 feature needs an ES-3.1 path or it
       fails at shader compile — not at feature-query time, which is what makes it bite late. */
    int needs_es31_fallback;

    /* Adreno's GLES driver rejects a shader declaring TWO framebuffer-fetch `inout` attachments;
       output is garbage rather than a compile error. One attachment only. */
    int single_fbfetch_attachment;

    /* Adreno's SPIR-V compiler crashes on OpFunctionCall, so shader helper functions must be
       preprocessor macros rather than real functions. Costs nothing to always obey. */
    int shader_helpers_must_be_macros;

    /* 0 = fbfetch follows the profile, 1 = user forced it on, -1 = user forced it off. */
    int fbfetch_forced;
} armsx_gpu_profile_t;

/* Resets to an unknown profile with conservative defaults. Called implicitly on first use. */
void armsx_gpu_profile_reset(void);

/*
    Feed in what the GL backend knows. Safe to call with NULLs. Detection is cumulative: calling
    this after the Vulkan variant refines rather than replaces, because ANGLE's GL strings name
    the *native* GPU that the Vulkan query would also have found.
*/
void armsx_gpu_profile_note_gl(const char* vendor, const char* renderer, const char* version);

/* Feed in what Vulkan knows. driver_id is a VkDriverId; pass 0 when the driver-properties
   extension is unavailable, and identification falls back to vendor_id plus the device name. */
void armsx_gpu_profile_note_vk(unsigned int vendor_id, unsigned int driver_id,
                               const char* device_name, const char* driver_name,
                               const char* driver_info);

/* Extra identification hints from the host (Android SoC properties), used for MediaTek. */
void armsx_gpu_profile_note_host_hint(const char* hint);

/* "auto" (default), "adreno", "mali", "powervr", "xclipse". Unknown values are ignored and
   leave detection alone. Returns non-zero if an override was applied. */
int armsx_gpu_profile_override(const char* value);

/*
    Force framebuffer fetch on or off, overruling what the profile concluded. "auto" (default),
    "on", "off". Returns non-zero if the value was understood.

    This exists because the profile's fbfetch defaults are CONSERVATIVE, not measured per device.
    The stale-ROAA behaviour that makes us distrust Vulkan fetch on the proprietary Adreno driver
    was observed on one generation and, in ARMSX2's own words, never confirmed on others — so a
    newer Adreno may well be fine and simply inherits the caution. Without a switch, the one
    person who could tell us has no way to try it, and the default becomes permanent by default.
*/
int armsx_gpu_profile_force_fbfetch(const char* value);

const armsx_gpu_profile_t* armsx_gpu_profile_get(void);

/* One line suitable for the log and the OSD, e.g.
   "Adreno 740 (gen 7, Qualcomm proprietary) fbfetch=yes push-desc=yes". */
void armsx_gpu_profile_describe(char* out, size_t out_size);

const char* armsx_gpu_profile_vendor_name(armsx_gpu_vendor_t vendor);
const char* armsx_gpu_profile_driver_name(armsx_gpu_driver_t driver);

#ifdef __cplusplus
}
#endif

#endif /* ARMSX_GPU_PROFILE_H */
