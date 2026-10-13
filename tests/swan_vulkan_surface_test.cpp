// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/vulkan_surface.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
  using namespace SwanVulkanSurface;
  // Thor's sideways physical panel must not rotate otherwise-upright frames.
  for (auto rotation : {VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR,
      VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR}) {
    assert(UprightTransform(rotation | VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
      == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
  }
  assert(UprightTransform(VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) == 0);
  assert(UprightTransform(0) == 0);
  puts("Vulkan logical-window orientation: 6 cases passed");
}
