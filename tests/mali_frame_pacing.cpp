#include "../frontend/mali_frame_pacing.h"

// Compile-time coverage of every vendor and the paused/library path. In particular,
// a detected Adreno must retain the original wait-before-emulation path.
static_assert(armsx_mali_frame_pacing_active(ARMSX_GPU_VENDOR_MALI, true));
static_assert(!armsx_mali_frame_pacing_active(ARMSX_GPU_VENDOR_MALI, false));
static_assert(!armsx_mali_frame_pacing_active(ARMSX_GPU_VENDOR_ADRENO, true));
static_assert(!armsx_mali_frame_pacing_active(ARMSX_GPU_VENDOR_ADRENO, false));
static_assert(!armsx_mali_frame_pacing_active(ARMSX_GPU_VENDOR_UNKNOWN, true));
static_assert(!armsx_mali_frame_pacing_active(ARMSX_GPU_VENDOR_POWERVR, true));
static_assert(!armsx_mali_frame_pacing_active(ARMSX_GPU_VENDOR_XCLIPSE, true));
