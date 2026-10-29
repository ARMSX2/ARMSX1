// SPDX-License-Identifier: GPL-3.0-or-later
// Independently authored libretro Vulkan/Android presentation. No legacy engine code.
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include "vulkan_host.h"
#include <dlfcn.h>
#include <adrenotools/driver.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <cstring>
#include <android/log.h>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <sstream>
#include "shader_api.h"
#include "vulkan_surface.h"
#include "driver_environment.h"
#include "frame_slots.h"
#include "presentation_feedback.h"
#include "presentation_order.h"
#include <cstdlib>
namespace {
void* loader=nullptr;
PFN_vkGetInstanceProcAddr entry=nullptr;
PFN_vkGetDeviceProcAddr device_entry=nullptr;
std::mutex queue_mutex;
std::string driver_dir,driver_name,redirect_dir,hook_dir;
SwanDriverEnvironment::Values driver_variables;
SwanDriverEnvironment::Environment driver_environment;
std::optional<std::string> ReadEnvironment(const std::string& key) {
  const auto* value=std::getenv(key.c_str());return value?std::optional<std::string>(value):std::nullopt;
}
bool WriteEnvironment(const std::string& key,const std::optional<std::string>& value) {
  return (value?setenv(key.c_str(),value->c_str(),1):unsetenv(key.c_str()))==0;
}
SwanPresentation::Options presentation;
SwanShaders::Handle shader_chain=nullptr;
uint64_t shader_generation=0;size_t shader_counter=0;
std::atomic<bool> shader_active{false};
VkFormat swap_format=VK_FORMAT_UNDEFINED;
bool swap_color_attachment=false;
bool negotiated=false,rotation_chain=false;
ANativeWindow* retained_window=nullptr;
VkInstance instance=VK_NULL_HANDLE;
retro_vulkan_context core{};
retro_hw_render_interface_vulkan api{};
retro_hw_render_context_negotiation_interface_vulkan negotiation{};
VkSurfaceKHR surface=VK_NULL_HANDLE;
VkSwapchainKHR swapchain=VK_NULL_HANDLE;
VkExtent2D extent{};
std::vector<VkImage> swap_images;
VkCommandBuffer command=VK_NULL_HANDLE;
struct Frame {VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};VkSemaphore acquired{};};
std::array<Frame,3> frames{};
SwanFrames::Slots slots;
unsigned requested_frames=2;
bool requested_vsync=true,requested_matched=true;
std::atomic<int> active_present_mode{int(SwanFramePacing::PresentMode::Fifo)};
bool synchronization_failed=false;
double last_wait_ms=0;
unsigned shader_width=0,shader_height=0;
std::vector<VkSemaphore> present_complete;
VkSemaphore signal_semaphore=VK_NULL_HANDLE;
retro_vulkan_image image{};
bool has_image=false;
std::vector<VkSemaphore> waits;
std::vector<VkCommandBuffer> core_commands;
PFN_vkGetPastPresentationTimingGOOGLE past_timing=nullptr;
PFN_vkGetRefreshCycleDurationGOOGLE refresh_timing=nullptr;
std::atomic<bool> timing_supported{false};
std::atomic<uint64_t> refresh_ns{0},timed_presents{0};
SwanFramePacing::Feedback display_feedback;
uint32_t present_id=0;
#define INSTANCE_FUNCTIONS(X) X(vkDestroyInstance) X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkEnumeratePhysicalDevices) X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)
#define DEVICE_FUNCTIONS(X) X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkQueueSubmit) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCmdCopyImageToBuffer)
#define DECLARE(name) PFN_##name name=nullptr;
INSTANCE_FUNCTIONS(DECLARE) DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
void Error(const char* text) {__android_log_write(ANDROID_LOG_ERROR,"SwanVulkan",text);}
int64_t TimeNs(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void Barrier(VkImage target,VkImageLayout before,VkImageLayout after,VkAccessFlags source,VkAccessFlags destination) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask=source;barrier.dstAccessMask=destination;barrier.oldLayout=before;barrier.newLayout=after;
  barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=target;
  barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
}
bool Begin() {
  auto& frame=frames[slots.Current()];
  if(slots.Pending(slots.Current())) {
    if(vkWaitForFences(core.device,1,&frame.fence,VK_TRUE,2000000000ull)!=VK_SUCCESS)return false;
    slots.Retired(slots.Current());
  }
  command=frame.command;
  if(vkResetCommandPool(core.device,frame.pool,0)!=VK_SUCCESS)return false;
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return vkBeginCommandBuffer(command,&info)==VK_SUCCESS;
}
bool Submit(bool with_window,uint32_t image_index=0) {
  if(vkEndCommandBuffer(command)!=VK_SUCCESS)return false;
  auto& frame=frames[slots.Current()];
  std::vector<VkSemaphore> wait=waits;waits.clear();if(with_window)wait.push_back(frame.acquired);
  std::vector<VkPipelineStageFlags> stages(wait.size(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  std::vector<VkSemaphore> signals;if(with_window)signals.push_back(present_complete.at(image_index));if(signal_semaphore)signals.push_back(signal_semaphore);signal_semaphore=VK_NULL_HANDLE;
  auto commands=core_commands;core_commands.clear();commands.push_back(command);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=wait.size();submit.pWaitSemaphores=wait.data();submit.pWaitDstStageMask=stages.data();
  submit.commandBufferCount=commands.size();submit.pCommandBuffers=commands.data();submit.signalSemaphoreCount=signals.size();submit.pSignalSemaphores=signals.data();
  if(vkResetFences(core.device,1,&frame.fence)!=VK_SUCCESS)return false;
  if(vkQueueSubmit(core.queue,1,&submit,frame.fence)!=VK_SUCCESS)return false;
  slots.Submitted();return true;
}
void DestroySwapchain() {
  if(!core.device)return;
  if(vkDeviceWaitIdle)vkDeviceWaitIdle(core.device);
  for(const auto semaphore:present_complete)if(semaphore)vkDestroySemaphore(core.device,semaphore,nullptr);
  present_complete.clear();
  if(shader_chain){auto* api=SwanShaders::Functions();if(api)SwanShaders::Check(api->free(&shader_chain));shader_chain=nullptr;}
  shader_generation=0;shader_counter=0;shader_active=false;
  shader_width=shader_height=0;
  refresh_ns=0;SwanVulkan::ResetTimingWindow();
  if(swapchain)vkDestroySwapchainKHR(core.device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;swap_images.clear();
  if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;
}
void CollectDisplayTiming() {
  if(!timing_supported.load()||!swapchain||!past_timing)return;
  if(refresh_timing){VkRefreshCycleDurationGOOGLE cycle{};
    if(refresh_timing(core.device,swapchain,&cycle)==VK_SUCCESS && cycle.refreshDuration>=2000000 && cycle.refreshDuration<=40000000)
      refresh_ns=cycle.refreshDuration;}
  uint32_t count=0;
  if(past_timing(core.device,swapchain,&count,nullptr)!=VK_SUCCESS||!count||count>1024)return;
  std::vector<VkPastPresentationTimingGOOGLE> values(count);
  const auto result=past_timing(core.device,swapchain,&count,values.data());
  if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE)return;
  count=std::min(count,uint32_t(values.size()));
  std::sort(values.begin(),values.begin()+count,[](const auto& a,const auto& b){return int32_t(a.presentID-b.presentID)<0;});
  for(uint32_t i=0;i<count;++i){const auto& value=values[i];
    display_feedback.Record(value.presentID,value.actualPresentTime,value.desiredPresentTime,refresh_ns.load());
    SwanFramePacing::PresentationRecovery().Observe(value.presentID,int64_t(value.actualPresentTime));}
}
void SetImage(void*,const retro_vulkan_image* value,uint32_t count,const VkSemaphore* semaphores,uint32_t family) {
  has_image=value && (family==VK_QUEUE_FAMILY_IGNORED || family==core.queue_family_index);
  if(has_image)image=*value;
  waits.clear();if(count&&semaphores)waits.assign(semaphores,semaphores+count);
}
uint32_t SyncIndex(void*) {return uint32_t(slots.Current());}
uint32_t SyncMask(void*) {return (1u<<slots.Count())-1;}
void WaitIndex(void*) {
  // Used by checked core hooks before its single shared output image is
  // replaced or destroyed. A submit fence covers image reads, not scanout.
  for(size_t i=0;i<slots.Count();++i)if(slots.Pending(i)) {
    const auto result=vkWaitForFences(core.device,1,&frames[i].fence,VK_TRUE,UINT64_MAX);
    if(result!=VK_SUCCESS){Error("Presentation fence wait failed");synchronization_failed=true;vkDeviceWaitIdle(core.device);}
    slots.Retired(i);
  }
}
void Commands(void*,uint32_t count,const VkCommandBuffer* buffers){core_commands.clear();if(count&&buffers)core_commands.assign(buffers,buffers+count);}
void Lock(void*){queue_mutex.lock();}
void Unlock(void*){queue_mutex.unlock();}
void Signal(void*,VkSemaphore semaphore){signal_semaphore=semaphore;}
}
bool SwanVulkan::Negotiate(const retro_hw_render_context_negotiation_interface_vulkan* requested,ANativeWindow* window) {
  if(!requested || requested->interface_type!=RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN || !requested->create_device)return false;
  negotiation=*requested;
  // Never publish a partially created device. Clean core-side helper objects
  // before releasing the frontend's device on a failed negotiation.
  struct Rollback {bool committed=false;~Rollback(){if(!committed){
    static void* core_library=dlopen("libswanstation.so",RTLD_NOW|RTLD_LOCAL);
    auto abort=reinterpret_cast<void(*)()>(core_library?dlsym(core_library,"swan_vulkan_abort"):nullptr);if(abort)abort();SwanVulkan::Close();}}} rollback;
  negotiated=false;synchronization_failed=false;
  timing_supported=false;past_timing=nullptr;refresh_timing=nullptr;present_id=0;
  refresh_ns=0;SwanVulkan::ResetTimingWindow();
#define CLEAR_FUNCTION(name) name=nullptr;
  INSTANCE_FUNCTIONS(CLEAR_FUNCTION) DEVICE_FUNCTIONS(CLEAR_FUNCTION)
#undef CLEAR_FUNCTION
  // Always resolve the newly selected driver; never reuse the last session's selection.
  loader=nullptr;entry=nullptr;device_entry=nullptr;
  const bool custom=!driver_dir.empty()&&!driver_name.empty();
  if(!driver_environment.Restore(WriteEnvironment)){Error("Could not restore driver environment.");return false;}
  if(custom){
    const auto name=std::filesystem::path(driver_name);
    std::error_code error;const auto folder=std::filesystem::canonical(driver_dir,error);
    const auto file=std::filesystem::canonical(folder/name,error);
    const bool safe=!error&&name.filename()==name&&file.parent_path()==folder&&
      (folder.string().rfind("/data/user/",0)==0||folder.string().rfind("/data/data/",0)==0);
    if(safe){
      if(!driver_environment.Apply(driver_variables,ReadEnvironment,WriteEnvironment)){Error("Could not apply driver environment.");return false;}
      const int flags=ADRENOTOOLS_DRIVER_CUSTOM|(redirect_dir.empty()?0:ADRENOTOOLS_DRIVER_FILE_REDIRECT);
      loader=adrenotools_open_libvulkan(RTLD_NOW,flags,nullptr,hook_dir.c_str(),driver_dir.c_str(),driver_name.c_str(),redirect_dir.empty()?nullptr:redirect_dir.c_str(),nullptr);}
    if(!loader)Error("Custom Vulkan loader unavailable; using system Vulkan for this boot.");
  }
  if(!loader){
    if(!driver_environment.Restore(WriteEnvironment)){Error("Could not restore system driver environment.");return false;}
    loader=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
  }
  if(!loader)return false;
  entry=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));if(!entry)return false;
  const char* extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
  VkApplicationInfo fallback{VK_STRUCTURE_TYPE_APPLICATION_INFO};fallback.pApplicationName="EmuCorePSX";fallback.apiVersion=VK_API_VERSION_1_0;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=requested->get_application_info?requested->get_application_info():&fallback;
  info.enabledExtensionCount=2;info.ppEnabledExtensionNames=extensions;
  auto create=reinterpret_cast<PFN_vkCreateInstance>(entry(VK_NULL_HANDLE,"vkCreateInstance"));
  if(!create||create(&info,nullptr,&instance)!=VK_SUCCESS)return false;
#define LOAD_INSTANCE(name) name=reinterpret_cast<PFN_##name>(entry(instance,#name));if(!name)return false;
  INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
  if(window){VkAndroidSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};surface_info.window=window;
    if(vkCreateAndroidSurfaceKHR(instance,&surface_info,nullptr,&surface)!=VK_SUCCESS)return false;}
  // Select the same first device as the pinned core, then enable the timing
  // extension only when that actual system/custom driver advertises it.
  uint32_t gpu_count=0;
  if(vkEnumeratePhysicalDevices(instance,&gpu_count,nullptr)!=VK_SUCCESS||!gpu_count)return false;
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  if(vkEnumeratePhysicalDevices(instance,&gpu_count,gpus.data())!=VK_SUCCESS)return false;
  const auto selected_gpu=gpus.front();
  std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  uint32_t extension_count=0;
  if(vkEnumerateDeviceExtensionProperties(selected_gpu,nullptr,&extension_count,nullptr)==VK_SUCCESS){
    std::vector<VkExtensionProperties> available(extension_count);
    if(vkEnumerateDeviceExtensionProperties(selected_gpu,nullptr,&extension_count,available.data())==VK_SUCCESS)
      for(const auto& extension:available)if(std::strcmp(extension.extensionName,VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)==0){
        device_extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);break;}
  }
  // The pinned core owns its helper resources; the returned device belongs to this host.
  if(!requested->create_device(&core,instance,selected_gpu,surface,entry,device_extensions.data(),uint32_t(device_extensions.size()),nullptr,0,nullptr))return false;
  device_entry=reinterpret_cast<PFN_vkGetDeviceProcAddr>(entry(instance,"vkGetDeviceProcAddr"));if(!device_entry)return false;
#define LOAD_DEVICE(name) name=reinterpret_cast<PFN_##name>(device_entry(core.device,#name));if(!name)return false;
  DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
  if(device_extensions.size()>1){
    past_timing=reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(device_entry(core.device,"vkGetPastPresentationTimingGOOGLE"));
    refresh_timing=reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(device_entry(core.device,"vkGetRefreshCycleDurationGOOGLE"));
    timing_supported=past_timing&&refresh_timing;
  }
  slots.Reset(requested_frames);
  for(size_t i=0;i<slots.Count();++i) {
    auto& frame=frames[i];
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=core.queue_family_index;pool_info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if(vkCreateCommandPool(core.device,&pool_info,nullptr,&frame.pool)!=VK_SUCCESS)return false;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=frame.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
    if(vkAllocateCommandBuffers(core.device,&allocate,&frame.command)!=VK_SUCCESS)return false;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence_info.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    if(vkCreateFence(core.device,&fence_info,nullptr,&frame.fence)!=VK_SUCCESS)return false;
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if(vkCreateSemaphore(core.device,&semaphore_info,nullptr,&frame.acquired)!=VK_SUCCESS)return false;
  }
  api.interface_type=RETRO_HW_RENDER_INTERFACE_VULKAN;api.interface_version=RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;api.handle=nullptr;
  api.instance=instance;api.gpu=core.gpu;api.device=core.device;api.queue=core.queue;api.queue_index=core.queue_family_index;
  api.get_instance_proc_addr=entry;api.get_device_proc_addr=device_entry;api.set_image=SetImage;api.get_sync_index=SyncIndex;api.get_sync_index_mask=SyncMask;
  api.set_command_buffers=Commands;api.wait_sync_index=WaitIndex;api.lock_queue=Lock;api.unlock_queue=Unlock;api.set_signal_semaphore=Signal;
  if(!Window(window))return false;negotiated=true;rollback.committed=true;return true;
}
bool SwanVulkan::Window(ANativeWindow* window) {
  if(!core.device)return false;
  if(window)ANativeWindow_acquire(window);if(retained_window)ANativeWindow_release(retained_window);retained_window=window;
  DestroySwapchain();if(!window)return true;
  VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};info.window=window;
  if(vkCreateAndroidSurfaceKHR(instance,&info,nullptr,&surface)!=VK_SUCCESS)return false;
  VkBool32 supported=VK_FALSE;
  if(vkGetPhysicalDeviceSurfaceSupportKHR(core.gpu,core.queue_family_index,surface,&supported)!=VK_SUCCESS || !supported)return false;
  VkSurfaceCapabilitiesKHR capabilities{};if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(core.gpu,surface,&capabilities)!=VK_SUCCESS)return false;
  if(!(capabilities.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))return false;
  uint32_t count=0;vkGetPhysicalDeviceSurfaceFormatsKHR(core.gpu,surface,&count,nullptr);std::vector<VkSurfaceFormatKHR> formats(count);
  if(!count || vkGetPhysicalDeviceSurfaceFormatsKHR(core.gpu,surface,&count,formats.data())!=VK_SUCCESS)return false;
  auto format=formats.front();for(auto value:formats)if(value.format==VK_FORMAT_R8G8B8A8_UNORM){format=value;break;}
  if(format.format==VK_FORMAT_UNDEFINED)format.format=VK_FORMAT_R8G8B8A8_UNORM;
  swap_format=format.format;swap_color_attachment=capabilities.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  extent=capabilities.currentExtent;if(extent.width==UINT32_MAX)extent={
    std::clamp<uint32_t>(ANativeWindow_getWidth(window),capabilities.minImageExtent.width,capabilities.maxImageExtent.width),
    std::clamp<uint32_t>(ANativeWindow_getHeight(window),capabilities.minImageExtent.height,capabilities.maxImageExtent.height)};
  VkSwapchainCreateInfoKHR chain{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};chain.surface=surface;chain.minImageCount=std::max(2u,capabilities.minImageCount);
  if(capabilities.maxImageCount)chain.minImageCount=std::min(chain.minImageCount,capabilities.maxImageCount);
  chain.imageFormat=format.format;chain.imageColorSpace=format.colorSpace;chain.imageExtent=extent;chain.imageArrayLayers=1;chain.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if(swap_color_attachment)chain.imageUsage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  chain.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
  chain.preTransform=SwanVulkanSurface::UprightTransform(capabilities.supportedTransforms);
  if(!chain.preTransform){Error("Vulkan surface cannot present logical window coordinates without pre-rotation.");return false;}
  __android_log_print(ANDROID_LOG_INFO,"SwanVulkan","Window %ux%u: panel transform %u, presentation transform %u",
    extent.width,extent.height,capabilities.currentTransform,chain.preTransform);
  chain.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if(!(capabilities.supportedCompositeAlpha&chain.compositeAlpha))chain.compositeAlpha=static_cast<VkCompositeAlphaFlagBitsKHR>(capabilities.supportedCompositeAlpha&-capabilities.supportedCompositeAlpha);
  chain.presentMode=VK_PRESENT_MODE_FIFO_KHR;chain.clipped=VK_TRUE;
  if(!requested_vsync||!requested_matched) {
    uint32_t modes_count=0;vkGetPhysicalDeviceSurfacePresentModesKHR(core.gpu,surface,&modes_count,nullptr);
    std::vector<VkPresentModeKHR> modes(modes_count);
    if(vkGetPhysicalDeviceSurfacePresentModesKHR(core.gpu,surface,&modes_count,modes.data())==VK_SUCCESS) {
      const auto mode=SwanFramePacing::ChoosePresentMode(requested_vsync,requested_matched,
        std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=modes.end(),
        std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_IMMEDIATE_KHR)!=modes.end());
      if(mode==SwanFramePacing::PresentMode::Mailbox)chain.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
      else if(mode==SwanFramePacing::PresentMode::Immediate)chain.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }
  if(vkCreateSwapchainKHR(core.device,&chain,nullptr,&swapchain)!=VK_SUCCESS)return false;
  active_present_mode=int(chain.presentMode==VK_PRESENT_MODE_MAILBOX_KHR?SwanFramePacing::PresentMode::Mailbox:
    chain.presentMode==VK_PRESENT_MODE_IMMEDIATE_KHR?SwanFramePacing::PresentMode::Immediate:SwanFramePacing::PresentMode::Fifo);
  vkGetSwapchainImagesKHR(core.device,swapchain,&count,nullptr);swap_images.resize(count);
  if(vkGetSwapchainImagesKHR(core.device,swapchain,&count,swap_images.data())!=VK_SUCCESS)return false;
  present_complete.resize(count,VK_NULL_HANDLE);
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for(auto& semaphore:present_complete)if(vkCreateSemaphore(core.device,&semaphore_info,nullptr,&semaphore)!=VK_SUCCESS)return false;
  CollectDisplayTiming();
  return true;
}
const retro_hw_render_interface_vulkan* SwanVulkan::Interface(){return negotiated?&api:nullptr;}
bool SwanVulkan::Ready(){return negotiated;}
bool SwanVulkan::Present(unsigned width,unsigned height,float aspect,bool suspended,bool black,unsigned subframe,unsigned total,float nominal,int64_t desired_time,int64_t submit_time,void (*wait)(int64_t),SwanFramePacing::PresentRequest recovery) {
  if(!core.device||synchronization_failed)return false;std::lock_guard lock(queue_mutex);
  last_wait_ms=0;
  auto& control=SwanFramePacing::PresentationRecovery();control.Policy(recovery);
  CollectDisplayTiming();
  const bool skip=swapchain&&!suspended&&control.Skip(recovery,TimeNs());
  // Even a recovery drop submits the core's waits/commands/signals below. It
  // does not acquire a window image or strand a presentation semaphore.
  const bool draw=swapchain&&!suspended&&!skip;
  const auto selection=SwanShaders::Selected();
  const bool rotated=presentation.rotation!=0;
  if(has_image&&(selection.generation!=shader_generation||rotated!=rotation_chain)){vkDeviceWaitIdle(core.device);
    if(shader_chain){auto* api=SwanShaders::Functions();if(api)SwanShaders::Check(api->free(&shader_chain));shader_chain=nullptr;}
    shader_generation=selection.generation;rotation_chain=rotated;shader_active=false;
    const auto path=selection.enabled&&!selection.path.empty()?selection.path:rotated?SwanShaders::PresentationPreset():std::string();
    if(swap_color_attachment&&!path.empty()){
      auto* api=SwanShaders::Functions();SwanShaders::Handle preset=nullptr;
      if(api&&SwanShaders::Check(api->preset_create(path.c_str(),&preset))){
        SwanShaders::Options options{5,uint32_t(slots.Count()),true,false,true};SwanShaders::Device device{core.gpu,instance,core.device,core.queue,entry};
        if(SwanShaders::Check(api->create(&preset,device,&options,&shader_chain))&&selection.enabled)for(const auto& parameter:selection.parameters)SwanShaders::Check(api->parameter(&shader_chain,parameter.first.c_str(),parameter.second));
        if(preset)SwanShaders::Check(api->preset_free(&preset));
      }
      if(!shader_chain)Error("Vulkan preset failed; keeping plain presentation.");
    }
  }
  uint32_t index=0;
  // Subframe shader calls reuse the same librashader frame index. Retire them
  // explicitly rather than reusing descriptor/feedback resources in flight.
  if(shader_chain && (total>1 || shader_width!=width || shader_height!=height))WaitIndex(nullptr);
  if(synchronization_failed)return false;
  shader_width=width;shader_height=height;
  if(!Begin())return false;
  if(draw){const auto acquire_start=std::chrono::steady_clock::now();
    auto result=vkAcquireNextImageKHR(core.device,swapchain,2000000000ull,frames[slots.Current()].acquired,VK_NULL_HANDLE,&index);
    last_wait_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-acquire_start).count();
    if(result==VK_ERROR_OUT_OF_DATE_KHR){if(!Window(retained_window)||!swapchain)return false;
      if(!Begin())return false;
      result=vkAcquireNextImageKHR(core.device,swapchain,2000000000ull,frames[slots.Current()].acquired,VK_NULL_HANDLE,&index);}
    if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR)return false;}
  if(draw){const auto target=swap_images[index];
    Barrier(target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue clear_black{{0,0,0,1}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(command,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear_black,1,&range);
    const bool filtered=!black&&has_image&&width&&height&&shader_chain;
    if(filtered){
      Barrier(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      Barrier(image.create_info.image,image.image_layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
      const auto rect=SwanPresentation::Fit(extent.width,extent.height,width,height,aspect,presentation);
      SwanShaders::Viewport viewport{float(rect.x),float(rect.y),uint32_t(rect.width),uint32_t(rect.height)};
      auto* api=SwanShaders::Functions();
      const SwanShaders::Image source{image.create_info.image,image.create_info.format,width,height},destination{target,swap_format,extent.width,extent.height};
      const auto matrix=SwanPresentation::Matrix(presentation.rotation);
      SwanShaders::FrameOptions frame;frame.total_subframes=total;frame.current_subframe=subframe;
      frame.frames_per_second=nominal;frame.frametime_delta=uint32_t(1000.f/nominal);
      if(!SwanShaders::Check(api->frame(&shader_chain,command,shader_counter,source,destination,&viewport,matrix.data(),&frame))){Error("Vulkan shader frame failed.");return false;}
      if(subframe==total)shader_counter++;
      shader_active=selection.enabled;
      Barrier(image.create_info.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image.image_layout,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    }else if(!black&&has_image && width&&height) {
      Barrier(image.create_info.image,image.image_layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT);
      const auto rect=SwanPresentation::Fit(extent.width,extent.height,width,height,aspect,presentation);
      VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;
      blit.srcOffsets[1]={int(width),int(height),1};blit.dstOffsets[0]={rect.x,rect.y,0};
      blit.dstOffsets[1]={rect.x+rect.width,rect.y+rect.height,1};
      vkCmdBlitImage(command,image.create_info.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
      Barrier(image.create_info.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image.image_layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    }
    Barrier(target,filtered?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,filtered?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT,0);
  }
  return SwanFramePacing::PresentPreparedFrame([&]{
    if(!Submit(draw,index))return false;
    slots.Advance();return true;
  },[&]{if(draw&&wait)wait(submit_time);},[&]{
    if(!draw)return true;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&present_complete[index];present.swapchainCount=1;present.pSwapchains=&swapchain;present.pImageIndices=&index;
    VkPresentTimeGOOGLE time{++present_id,desired_time>0?uint64_t(desired_time):0};
    VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
    if(timing_supported.load()){
      times.swapchainCount=1;times.pTimes=&time;present.pNext=&times;
      if(time.desiredPresentTime)++timed_presents;
    }
    const auto present_start=std::chrono::steady_clock::now();
    if(timing_supported.load())control.Submitted(time.presentID,recovery,TimeNs());
    const auto result=vkQueuePresentKHR(core.queue,&present);
    if(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR)control.Presented(recovery,TimeNs());
    last_wait_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-present_start).count();
    // Feedback is collected before the next acquire, once per frame, so
    // recovery decisions see the newest available short-history samples.
    // Submission resources retire on their own fence. Presentation semaphore
    // reuse is protected by reacquiring this swapchain image, not that fence.
    if(result==VK_ERROR_OUT_OF_DATE_KHR)return Window(retained_window);
    // Android may report SUBOPTIMAL every frame when identity differs from the
    // panel transform. This is intentional compositor rotation, not a stale
    // swapchain. Real resize/recreation is handled by Window/OUT_OF_DATE; do not
    // stall and recreate shader resources on every otherwise-successful frame.
    return result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR;
  });
}
void SwanVulkan::Blank(){has_image=false;if(core.device)Present(0,0,4.f/3,false);}
void SwanVulkan::Close(){
  if(core.device && vkDeviceWaitIdle)vkDeviceWaitIdle(core.device);
  if(core.device && vkDestroySwapchainKHR)DestroySwapchain();
  if(core.device)for(auto& frame:frames){if(frame.acquired&&vkDestroySemaphore)vkDestroySemaphore(core.device,frame.acquired,nullptr);
    if(frame.fence&&vkDestroyFence)vkDestroyFence(core.device,frame.fence,nullptr);if(frame.pool&&vkDestroyCommandPool)vkDestroyCommandPool(core.device,frame.pool,nullptr);}
  if(negotiation.destroy_device)negotiation.destroy_device();
  if(core.device&&vkDestroyDevice)vkDestroyDevice(core.device,nullptr);
  if(surface&&vkDestroySurfaceKHR)vkDestroySurfaceKHR(instance,surface,nullptr);
  if(instance&&vkDestroyInstance)vkDestroyInstance(instance,nullptr);
  core={};api={};negotiation={};image={};instance=VK_NULL_HANDLE;surface=VK_NULL_HANDLE;swapchain=VK_NULL_HANDLE;
  if(retained_window)ANativeWindow_release(retained_window);retained_window=nullptr;negotiated=false;rotation_chain=false;
  frames={};slots.Reset(requested_frames);signal_semaphore=VK_NULL_HANDLE;command=VK_NULL_HANDLE;has_image=false;waits.clear();core_commands.clear();
  timing_supported=false;refresh_ns=0;past_timing=nullptr;refresh_timing=nullptr;
  // Keep the loader resident: the pinned core may retain proc-address trampolines.
  if(!driver_environment.Restore(WriteEnvironment))Error("Could not restore driver environment after shutdown.");
}
std::vector<unsigned char> SwanVulkan::Pixels(unsigned width,unsigned height) {
  if(!core.device||!has_image||!width||!height||width>8192||height>8192)return {};
  std::lock_guard lock(queue_mutex);if(!Begin())return {};
  VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
  const VkDeviceSize size=VkDeviceSize(width)*height*4;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=size;info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(vkCreateBuffer(core.device,&info,nullptr,&buffer)!=VK_SUCCESS)return {};
  VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(core.device,buffer,&requirements);
  VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(core.gpu,&properties);
  uint32_t type=properties.memoryTypeCount;
  for(uint32_t i=0;i<properties.memoryTypeCount;i++)if((requirements.memoryTypeBits&(1u<<i)) &&
    (properties.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
  std::vector<unsigned char> output;
  if(type<properties.memoryTypeCount){VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=requirements.size;allocate.memoryTypeIndex=type;
    if(vkAllocateMemory(core.device,&allocate,nullptr,&memory)==VK_SUCCESS && vkBindBufferMemory(core.device,buffer,memory,0)==VK_SUCCESS){
      Barrier(image.create_info.image,image.image_layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
      VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
      vkCmdCopyImageToBuffer(command,image.create_info.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
      Barrier(image.create_info.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image.image_layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
      if(Submit(false)&&vkWaitForFences(core.device,1,&frames[slots.Current()].fence,VK_TRUE,2000000000ull)==VK_SUCCESS){void* mapped=nullptr;
        if(vkMapMemory(core.device,memory,0,size,0,&mapped)==VK_SUCCESS){output.resize(size);std::memcpy(output.data(),mapped,size);vkUnmapMemory(core.device,memory);
          if(image.create_info.format==VK_FORMAT_B8G8R8A8_UNORM)for(size_t i=0;i<output.size();i+=4)std::swap(output[i],output[i+2]);}
      }
    }
  }
  vkDeviceWaitIdle(core.device);slots.Advance();vkDestroyBuffer(core.device,buffer,nullptr);if(memory)vkFreeMemory(core.device,memory,nullptr);return output;
}
void SwanVulkan::ConfigurePacing(bool vsync,unsigned queued_frames) {
  // Session configuration only, before negotiation; no live resource resize.
  if(core.device)return;
  requested_vsync=vsync;requested_matched=true;requested_frames=std::clamp(queued_frames,1u,3u);
}
double SwanVulkan::LastWaitMs(){return last_wait_ms;}
bool SwanVulkan::Vsync(bool enabled,bool matched) {
  if(requested_vsync==enabled&&requested_matched==matched)return true;
  requested_vsync=enabled;requested_matched=matched;
  // Speed/fast-forward transitions are infrequent. Recreate through the same
  // retirement path so FIFO cannot silently cap an unthrottled session.
  return !core.device || Window(retained_window);
}
void SwanVulkan::ResetTimingWindow(){
  const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  display_feedback.Reset(uint64_t(now));timed_presents=0;
  SwanFramePacing::PresentationRecovery().Reset(now);
}
int64_t SwanVulkan::LastDisplayTime(){return int64_t(display_feedback.Last());}
float SwanVulkan::RefreshHz(){const auto ns=refresh_ns.load();return ns?float(1e9/double(ns)):0;}
std::string SwanVulkan::TimingSnapshot(){
  const auto feedback=display_feedback.Read();
  std::ostringstream out;out<<"Vulkan display timing: supported="<<timing_supported.load()
    <<" timestamped="<<timed_presents.load()<<" feedback_frames="<<feedback.frames
    <<" mean_present_ms="<<(feedback.gaps?double(feedback.sum)/feedback.gaps/1e6:0)
    <<" worst_present_ms="<<double(feedback.maximum)/1e6<<" unexpected_late="<<feedback.late
    <<" feedback_intervals="<<feedback.gaps<<" missing_feedback="<<feedback.missing
    <<" coalesced_feedback="<<feedback.coalesced<<" present_mode="
    <<(active_present_mode==int(SwanFramePacing::PresentMode::Mailbox)?"mailbox":
       active_present_mode==int(SwanFramePacing::PresentMode::Immediate)?"immediate":"fifo");
  return out.str();
}
void SwanVulkan::Driver(std::string directory,std::string name,std::string redirect,std::string hooks){
  driver_dir=std::move(directory);driver_name=std::move(name);redirect_dir=std::move(redirect);hook_dir=std::move(hooks);
}
void SwanVulkan::Presentation(SwanPresentation::Options options){presentation=options;}
bool SwanVulkan::ShaderActive(){return shader_active;}
bool SwanVulkan::DriverVariables(const std::string& variables){
  SwanDriverEnvironment::Values parsed;if(!SwanDriverEnvironment::Parse(variables,parsed))return false;
  driver_variables=std::move(parsed);return true;
}
