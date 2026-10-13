// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan.h>

namespace SwanVulkanSurface {
// Frames are drawn in logical window coordinates, not pre-rotated into the
// panel's native orientation. Android must therefore perform panel rotation.
// Reporting currentTransform would incorrectly claim we already did that.
constexpr VkSurfaceTransformFlagBitsKHR UprightTransform(VkSurfaceTransformFlagsKHR supported) {
  return (supported & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
    ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : static_cast<VkSurfaceTransformFlagBitsKHR>(0);
}
}
