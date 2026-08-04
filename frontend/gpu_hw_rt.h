#ifndef ARMSX_GPU_HW_RT_H
#define ARMSX_GPU_HW_RT_H

/*
    ARMSX — internal-resolution ("hardware") rasterizer backend.

    Implements the psx/dev/gpu_backend.h ABI by rasterizing every primitive a second time
    into a render target of 1024*S x 512*S BGR555 pixels, S being the internal-resolution
    multiplier from [video] internal_scale. The emulated console's own 1024x512 VRAM stays
    authoritative and untouched (PSX_GPU_BACKEND_SOFTWARE_SHADOW), so texture fetches,
    GPUREAD drains and save states all keep working with no coherency layer or readback stall.

    At S == 1 the render target is required to be BYTE-IDENTICAL to gpu->vram. That is the
    correctness gate for the coordinate model: scaling must be a no-op at 1x.
    tests/gpu_renderer_parity.c enforces it.

    This backend is deliberately graphics-API-independent: it validates the ABI, the
    scaling contract and the setting plumbing without needing a GL context on the
    emulation thread. A GLES/Vulkan backend replaces the rasterizing half behind the same ABI.
*/

#include <stdint.h>

#include "../psx/dev/gpu.h"

#ifdef USE_HARDWARE

#include "../psx/dev/gpu_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound on internal_scale. 8 puts the render target at 8192x4096x2 = 64 MB, which
   is already past what a phone should be asked for; the config layer clamps to this. */
#define ARMSX_HW_RT_MAX_SCALE 8

/* Creates the backend at the given scale (clamped to 1..ARMSX_HW_RT_MAX_SCALE) and seeds
   its render target from `gpu`'s current VRAM. Returns NULL if allocation fails, in which
   case the caller stays on the software rasterizer. The GPU does not take ownership. */
psx_gpu_backend_t* armsx_hw_rt_create(psx_gpu_t* gpu, int scale);

/* Destroys a backend from armsx_hw_rt_create(). Call psx_gpu_set_backend(gpu, NULL)
   first. NULL-safe. */
void armsx_hw_rt_destroy(psx_gpu_backend_t* backend);

#ifdef __cplusplus
}
#endif

#endif

#endif
