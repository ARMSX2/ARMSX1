// Exercise the production fence gate with a controllable Vulkan fence. This
// checks host-side ownership/error behaviour, not a driver's GPU performance.
#include "vulkan_frame_gate.h"
#include <cassert>
#include <cstdio>

static VkDevice expectedDevice=reinterpret_cast<VkDevice>(uintptr_t(1));
static VkFence expectedFence=reinterpret_cast<VkFence>(uintptr_t(2));
static VkResult status=VK_SUCCESS;
static unsigned polls=0;
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device,VkFence fence){
    assert(device==expectedDevice&&fence==expectedFence);polls++;return status;
}
int main(){
    assert(psxVulkanFrameReady(expectedDevice,expectedFence)); // initially signalled
    status=VK_NOT_READY;
    for(int i=0;i<10000;i++)assert(!psxVulkanFrameReady(expectedDevice,expectedFence));
    assert(polls==10001); // pending stays pending; no reset/wait/acquire/submit API is linked
    status=VK_SUCCESS;assert(psxVulkanFrameReady(expectedDevice,expectedFence));
    status=VK_NOT_READY;assert(!psxVulkanFrameReady(expectedDevice,expectedFence));
    status=VK_ERROR_DEVICE_LOST;
    bool threw=false;
    try{psxVulkanFrameReady(expectedDevice,expectedFence);}catch(const std::runtime_error& e){threw=std::string(e.what()).find("-4")!=std::string::npos;}
    assert(threw); // a lost device must reach error recovery, not silently drop forever
    std::puts("Vulkan fence gate: signalled, 10000 pending polls, reuse and device loss passed");
}
