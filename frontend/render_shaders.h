/*
    ARMSX — RetroArch (.slangp) shader chains, via librashader.

    This is the whole seam between the presentation backends and librashader. Nothing else
    in the tree knows librashader exists; frontend/render_vk.cpp calls the three Vulkan
    entry points below and frontend/android_jni.cpp calls the four control ones.

    BACKEND: VULKAN ONLY. librashader's runtimes are Vulkan / desktop-OpenGL / D3D9-12 /
    Metal / wgpu. Its GL runtime emits DESKTOP GLSL and uses desktop-only entry points;
    frontend/render_gl.cpp deliberately targets GLES 3.0 (`#version 300 es`), and ANGLE
    advertising GLES 3.1 does not close that gap — the gap is desktop-vs-ES, not a minor
    version. Vulkan is also the backend verified working on the test device, and its
    present path is a single vkCmdBlitImage, i.e. a two-call seam. With any other present
    backend the chain is inert and says so once in the log.

    DEGRADATION IS ALWAYS TO PLAIN PRESENTATION, NEVER TO A BLACK SCREEN. Every failure
    path — no librashader .so, wrong ABI, unreadable preset, a preset that fails to
    compile, a Vulkan error mid-frame — makes armsx_shader_vk_render() return false, and
    the caller's ordinary blit runs instead. Every one of them is logged; none is silent.

    THREADING. librashader chains are single-threaded. Only the presenting thread may
    touch a chain, so everything the UI sets goes through a queue here and is applied at
    the top of the next armsx_shader_vk_render(). armsx_shader_preset_params() is the one
    exception and touches no chain at all: it parses a preset file into a throwaway
    handle, which is why it works with no VM and no renderer up.
*/

#ifndef ARMSX_RENDER_SHADERS_H
#define ARMSX_RENDER_SHADERS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- control surface (any thread) ---------------------------------------------------- */

/* Select the chain. `preset_path` may be NULL or "" for none. Cheap and non-blocking: it
   records the request and the presenting thread reconciles on its next frame, because
   building a chain compiles shaders and must not happen on the caller's thread.
   Idempotent — re-setting the same (enabled, path) does nothing. */
void armsx_shader_set_chain(bool enabled, const char* preset_path);

/* Queue parameter assignments for `preset_path`'s chain. Applied by the presenting thread
   on its next frame, and only if the running chain is still that preset — so a stale set
   from a screen the user has left cannot land on a chain that has moved on.

   This is a set of ASSIGNMENTS, not the chain's whole state: a parameter left out keeps
   whatever the chain has. Resetting one therefore means sending its initial value, not
   omitting it (com.armsx2.ShaderParams.pushEffective is the caller that gets this right).
   Safe with no VM, no renderer and no librashader — the values just sit here. */
void armsx_shader_queue_params(const char* preset_path,
                               const char* const* names,
                               const float* values,
                               int count);

/* The parameters `preset_path` declares, as a JSON ARRAY, in declaration order.

   Order is load-bearing: a parameter whose maximum equals its minimum is a CAPTION
   introducing the run that follows it, and com.armsx2.ShaderParams relies on the
   ordering to keep captions attached to what they caption.

     [{"name":"…","description":"…","initial":0,"minimum":0,"maximum":1,"step":0.01}]

   NEVER returns NULL, and never returns something JSONArray() would throw on: every
   failure path yields "[]". The caller owns the buffer and frees it with
   armsx_shader_free_string(). Blocking file IO + parse; call it off the UI thread. */
char* armsx_shader_preset_params(const char* preset_path);
void armsx_shader_free_string(char* text);

/* The rasterizer's internal-resolution scale (1..8), so the chain can be run at NATIVE
   PlayStation resolution rather than at the upscaled one. See the long comment in
   frontend/render_shaders.cpp for why that is the correct place for a CRT chain to sit.
   Called from frontend/config.c when [video] renderer/internal_scale are resolved; the
   default of 1 is the identity, so a caller that never calls this is simply un-upscaled. */
void armsx_shader_set_source_scale(int scale);

/* Which present backend actually came up (armsx_render_backend_t, passed as int to keep
   this header free of render.h). Purely so "you enabled a shader chain on a backend that
   cannot run one" is said ONCE, out loud, instead of the feature silently doing nothing —
   which is the single most common way this port has broken a lifted control. */
void armsx_shader_note_backend(int backend);

/* ---- Vulkan seam (presenting thread only) --------------------------------------------

   Declared only where there is a Vulkan present path to hook. frontend/render_vk.cpp is
   itself entirely behind ARMSX_ENABLE_VULKAN, so its call sites and these declarations
   appear and disappear together — and a desktop build with no Vulkan headers installed
   never sees <vulkan/vulkan.h> from this header at all, which is what keeps `make` and
   `make test-gpu` compiling on a machine that has no Vulkan SDK.

   Keep libvulkan out of DT_NEEDED: nothing here ever calls a Vulkan entry point through
   the loader's exported symbols — every one is resolved from the caller's
   vkGetInstanceProcAddr — so VK_NO_PROTOTYPES removes even the opportunity for an
   accidental link against the system loader, which is what breaks adrenotools
   custom-driver injection.

   Define ARMSX_RENDER_SHADERS_NO_VK before including this to get only the control surface
   above. frontend/config.c does that: it calls armsx_shader_set_source_scale() and nothing
   else, and has no business pulling 10k lines of <vulkan/vulkan.h> into a C translation
   unit to do it. */
#if defined(ARMSX_ENABLE_VULKAN) && !defined(ARMSX_RENDER_SHADERS_NO_VK)

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

typedef struct armsx_shader_vk_device {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance                instance;
    VkPhysicalDevice          physical_device;
    VkDevice                  device;
    VkQueue                   queue;
    uint32_t                  queue_family;
} armsx_shader_vk_device_t;

/* Attach/detach the chain layer to a live VkDevice. detach MUST run before
   vkDestroyDevice: the chain owns images, views, framebuffers, pipelines and descriptor
   pools created against that device. Both are no-ops when librashader is unavailable. */
void armsx_shader_vk_attach(const armsx_shader_vk_device_t* device);
void armsx_shader_vk_detach(void);

/*
    Guards the single VkQueue.

    The shader chain is built on a WORKER thread so a ~110 ms compile does not land inside a
    frame, and librashader uploads its lookup textures through the queue it is handed during
    chain creation. A VkDevice may be used from several threads, but a VkQueue may NOT — so the
    present path must hold this around its submits and presents, or the two threads race and the
    result is intermittent corruption or a device-lost on hardware we do not own.

    Cheap when nothing is building: an uncontended mutex per present.
*/
void armsx_shader_vk_queue_lock(void);
void armsx_shader_vk_queue_unlock(void);

typedef struct armsx_shader_vk_frame {
    /* Already begun, and NOT inside a render pass (librashader forbids that). Exactly one
       vkQueueSubmit must follow a call that returned true — librashader recycles
       per-frame resources on a ring keyed to the frame counter, and recording a chain
       frame that is then never submitted desyncs that ring. */
    VkCommandBuffer cmd;

    /* The uploaded emulator frame, in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL on entry and
       left in it on exit. MUST have been created with VK_IMAGE_USAGE_SAMPLED_BIT. */
    VkImage  source;
    VkFormat source_format;
    int      source_width, source_height;

    /* The swapchain image, in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL on entry and left in it
       on exit. The caller's existing barrier to PRESENT_SRC_KHR is unaffected. */
    VkImage  target;
    int      target_width, target_height;

    /* The letterboxed destination rect from armsx_render_compute_dst(). The chain renders
       at exactly this size, which is the aspect-correct one — handing librashader the
       whole window instead is the classic way this feature ships stretched. */
    int dst_x, dst_y, dst_w, dst_h;
} armsx_shader_vk_frame_t;

/* Records the chain into frame->cmd. Returns true when it drew and the caller must NOT
   blit; false when there is no chain, it failed, or librashader is unavailable — then the
   caller's ordinary blit runs and presentation is exactly as it was before this file
   existed. Layout-transparent: both images come back in the layouts they went in. */
bool armsx_shader_vk_render(const armsx_shader_vk_frame_t* frame);

#endif /* ARMSX_ENABLE_VULKAN */

#ifdef __cplusplus
}
#endif

#endif /* ARMSX_RENDER_SHADERS_H */
