#ifndef ARMSX_MALI_FRAME_PACING_H
#define ARMSX_MALI_FRAME_PACING_H
#include "gpu_profile.h"
// Experimental presentation scheduling, not a model-specific performance claim.
static constexpr bool armsx_mali_frame_pacing_active(armsx_gpu_vendor_t vendor,
                                                 bool running) {
    return running && vendor == ARMSX_GPU_VENDOR_MALI;
}
#endif
