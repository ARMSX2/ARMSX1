#pragma once
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>

// Poll before producing another shared picture. A busy display must not hold up
// the worker which also consumes the guest's GPU command stream. Do not reset
// the fence or recycle any presentation resource until it actually signals.
inline bool psxVulkanFrameReady(VkDevice device,VkFence fence){
    VkResult result=vkGetFenceStatus(device,fence);
    if(result==VK_NOT_READY)return false;
    if(result!=VK_SUCCESS)throw std::runtime_error("Check shared display fence (Vulkan "+std::to_string(result)+")");
    return true;
}
