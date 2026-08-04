/* Host-side unit test for the profile's detection rules. Runs on the Mac, so the cases nobody
   here owns hardware for (Mali, MediaTek, ANGLE, Turnip, Xclipse) are still exercised. */
#include "frontend/gpu_profile.h"
#include <stdio.h>
#include <string.h>
static int fails = 0;
static void check(const char* what, int got, int want) {
    if (got != want) { printf("  FAIL %s: got %d want %d\n", what, got, want); fails++; }
}
int main(void) {
    const armsx_gpu_profile_t* p;

    /* 1. Adreno on the proprietary blob — the RP6. Must NOT disable push descriptors. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_gl("Qualcomm", "Adreno (TM) 740", "OpenGL ES 3.2 V@0676.53 (GIT@69e13)");
    p = armsx_gpu_profile_get();
    printf("adreno-proprietary\n");
    check("vendor", p->vendor, ARMSX_GPU_VENDOR_ADRENO);
    check("model", p->model, 740);
    check("driver", p->driver, ARMSX_GPU_DRIVER_QUALCOMM_PROPRIETARY);
    check("push-desc kept", p->avoid_push_descriptors, 0);
    check("fbfetch gl", p->fbfetch_gl, 1);
    check("fbfetch vk roaa off", p->fbfetch_vk_roaa, 0);
    check("single attachment", p->single_fbfetch_attachment, 1);

    /* 2. Same GPU on Turnip — opposite conclusions on BOTH driver-keyed gates. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_gl("Mesa", "Turnip Adreno (TM) 740", "OpenGL ES 3.2 Mesa 24.0");
    p = armsx_gpu_profile_get();
    printf("adreno-turnip\n");
    check("driver", p->driver, ARMSX_GPU_DRIVER_MESA_TURNIP);
    check("push-desc disabled", p->avoid_push_descriptors, 1);
    check("fbfetch vk roaa on", p->fbfetch_vk_roaa, 1);

    /* 3. Adreno 650 — the model-specific buffer teardown, and only the 650. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_gl("Qualcomm", "Adreno (TM) 650", "OpenGL ES 3.2 V@0490");
    p = armsx_gpu_profile_get();
    printf("adreno-650\n");
    check("no persistent map", p->avoid_persistent_buffer_map, 1);
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_gl("Qualcomm", "Adreno (TM) 660", "OpenGL ES 3.2 V@0490");
    check("660 keeps persistent map", armsx_gpu_profile_get()->avoid_persistent_buffer_map, 0);

    /* 4. ANGLE over Mali — detection must see THROUGH ANGLE to the real GPU. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_gl("Google Inc.", "ANGLE (ARM, Vulkan 1.1.177 (Mali-G77 MC9), ...)",
                              "OpenGL ES 3.1");
    p = armsx_gpu_profile_get();
    printf("angle-over-mali\n");
    check("sees Mali", p->vendor, ARMSX_GPU_VENDOR_MALI);
    check("angle flagged", p->is_angle, 1);
    check("es31 fallback", p->needs_es31_fallback, 1);
    check("no dual-source", p->dual_source_blend, 0);

    /* 5. MediaTek Mali via Vulkan — fbfetch must be off in both APIs. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_vk(0x13B5u, 9, "Mali-G610 MC4", "ARM proprietary", "r44p1");
    armsx_gpu_profile_note_host_hint("mt6877");
    p = armsx_gpu_profile_get();
    printf("mediatek-mali\n");
    check("mediatek", p->is_mediatek, 1);
    check("fbfetch gl off", p->fbfetch_gl, 0);
    check("fbfetch vk off", p->fbfetch_vk_roaa, 0);
    check("driverInfo kept", strcmp(p->driver_info, "r44p1"), 0);

    /* Android knows MediaTek from Build.SOC_* before EGL exists. The later GL strings supply
       Mali. This is the real SDL-present + GLES-rasterizer discovery order. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_host_hint("OPPO CPHxxxx MediaTek MT6877");
    armsx_gpu_profile_note_gl("ARM", "Mali-G610 MC4", "OpenGL ES 3.2");
    p = armsx_gpu_profile_get();
    printf("mediatek-host-then-gl\n");
    check("host hint retained", p->is_mediatek, 1);
    check("later GL sees Mali", p->vendor, ARMSX_GPU_VENDOR_MALI);
    check("host+GL fbfetch off", p->fbfetch_gl, 0);

    /* A software-only Android session has no renderer context of its own. The startup pbuffer
       probe still provides a real system GL_RENDERER, which must identify the physical GPU
       without inventing a driver. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_host_hint("Mali-G715");
    p = armsx_gpu_profile_get();
    printf("software-host-gpu-probe\n");
    check("probe sees Mali", p->vendor, ARMSX_GPU_VENDOR_MALI);
    check("probe sees model", p->model, 715);
    check("probe leaves driver unknown", p->driver, ARMSX_GPU_DRIVER_UNKNOWN);
    check("Mali dual-source disabled", p->dual_source_blend, 0);

    /* 6. THE OVERRIDE MUST NOT DISCARD FACTS: force "mali" on a MediaTek device. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_override("mali");
    armsx_gpu_profile_note_gl("ARM", "Mali-G610 MC4 mt6877", "OpenGL ES 3.2");
    p = armsx_gpu_profile_get();
    printf("forced-mali-on-mediatek\n");
    check("forced", p->forced, 1);
    check("mediatek still seen", p->is_mediatek, 1);
    check("fbfetch off (not yes)", p->fbfetch_gl, 0);

    /* 7. Forcing must pin the vendor against contrary evidence. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_override("mali");
    armsx_gpu_profile_note_gl("Qualcomm", "Adreno (TM) 740", "OpenGL ES 3.2 V@0676");
    p = armsx_gpu_profile_get();
    printf("forced-mali-on-adreno\n");
    check("vendor pinned", p->vendor, ARMSX_GPU_VENDOR_MALI);
    check("no dual-source", p->dual_source_blend, 0);

    /* 8. Unknown GPU: never pessimise hardware nobody characterised. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_gl("Acme", "Frobnicator 9000", "OpenGL ES 3.2");
    p = armsx_gpu_profile_get();
    printf("unknown-gpu\n");
    check("vendor unknown", p->vendor, ARMSX_GPU_VENDOR_UNKNOWN);
    check("fbfetch gl assumed ok", p->fbfetch_gl, 1);
    check("push-desc allowed", p->avoid_push_descriptors, 0);
    check("macros always on", p->shader_helpers_must_be_macros, 1);

    /* 9. force_fbfetch must overrule the profile in BOTH directions and survive re-detection. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_gl("Qualcomm", "Adreno (TM) 740", "OpenGL ES 3.2 V@0676");
    check("vk roaa off by default", armsx_gpu_profile_get()->fbfetch_vk_roaa, 0);
    armsx_gpu_profile_force_fbfetch("on");
    printf("force-fbfetch-on\n");
    check("vk roaa forced on", armsx_gpu_profile_get()->fbfetch_vk_roaa, 1);
    /* Re-notifying must not silently undo the user's choice. */
    armsx_gpu_profile_note_gl("Qualcomm", "Adreno (TM) 740", "OpenGL ES 3.2 V@0676");
    check("survives re-detection", armsx_gpu_profile_get()->fbfetch_vk_roaa, 1);
    armsx_gpu_profile_force_fbfetch("off");
    printf("force-fbfetch-off\n");
    check("gl forced off", armsx_gpu_profile_get()->fbfetch_gl, 0);
    armsx_gpu_profile_force_fbfetch("auto");
    check("auto restores profile", armsx_gpu_profile_get()->fbfetch_gl, 1);
    check("garbage value rejected", armsx_gpu_profile_force_fbfetch("banana"), 0);

    /* 10. A custom driver that reports driverID 0 must still be identified by name. This is the
       adrenotools/Turnip case: real device, real driver, no driverID. */
    armsx_gpu_profile_reset();
    armsx_gpu_profile_note_vk(0x5143u, 0, "Turnip Adreno (TM) 740", "", "");
    p = armsx_gpu_profile_get();
    printf("turnip-no-driverid\n");
    check("identified as Turnip", p->driver, ARMSX_GPU_DRIVER_MESA_TURNIP);
    check("vk roaa trusted on turnip", p->fbfetch_vk_roaa, 1);
    check("push-desc disabled on turnip", p->avoid_push_descriptors, 1);

    printf(fails ? "\nGPU_PROFILE %d check(s) FAILED\n" : "\nGPU_PROFILE all checks passed\n", fails);
    return fails ? 1 : 0;
}
