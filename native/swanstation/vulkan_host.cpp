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
#include "shader_api.h"
#include "vulkan_surface.h"
namespace {
void* loader=nullptr;
PFN_vkGetInstanceProcAddr entry=nullptr;
PFN_vkGetDeviceProcAddr device_entry=nullptr;
std::mutex queue_mutex;
std::string driver_dir,driver_name,redirect_dir,hook_dir;
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
VkCommandPool pool=VK_NULL_HANDLE;
VkCommandBuffer command=VK_NULL_HANDLE;
VkFence fence=VK_NULL_HANDLE;
VkSemaphore acquired=VK_NULL_HANDLE,complete=VK_NULL_HANDLE,signal_semaphore=VK_NULL_HANDLE;
retro_vulkan_image image{};
bool has_image=false;
std::vector<VkSemaphore> waits;
std::vector<VkCommandBuffer> core_commands;
#define INSTANCE_FUNCTIONS(X) X(vkDestroyInstance) X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)
#define DEVICE_FUNCTIONS(X) X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkQueueSubmit) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCmdCopyImageToBuffer)
#define DECLARE(name) PFN_##name name=nullptr;
INSTANCE_FUNCTIONS(DECLARE) DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
void Error(const char* text) {__android_log_write(ANDROID_LOG_ERROR,"SwanVulkan",text);}
void Barrier(VkImage target,VkImageLayout before,VkImageLayout after,VkAccessFlags source,VkAccessFlags destination) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask=source;barrier.dstAccessMask=destination;barrier.oldLayout=before;barrier.newLayout=after;
  barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=target;
  barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
}
bool Begin() {
  if(vkWaitForFences(core.device,1,&fence,VK_TRUE,2000000000ull)!=VK_SUCCESS)return false;
  if(vkResetCommandPool(core.device,pool,0)!=VK_SUCCESS)return false;
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return vkBeginCommandBuffer(command,&info)==VK_SUCCESS;
}
bool Submit(bool with_window) {
  if(vkEndCommandBuffer(command)!=VK_SUCCESS)return false;
  std::vector<VkSemaphore> wait=waits;waits.clear();if(with_window)wait.push_back(acquired);
  std::vector<VkPipelineStageFlags> stages(wait.size(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  std::vector<VkSemaphore> signals;if(with_window)signals.push_back(complete);if(signal_semaphore)signals.push_back(signal_semaphore);signal_semaphore=VK_NULL_HANDLE;
  auto commands=core_commands;core_commands.clear();commands.push_back(command);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=wait.size();submit.pWaitSemaphores=wait.data();submit.pWaitDstStageMask=stages.data();
  submit.commandBufferCount=commands.size();submit.pCommandBuffers=commands.data();submit.signalSemaphoreCount=signals.size();submit.pSignalSemaphores=signals.data();
  if(vkResetFences(core.device,1,&fence)!=VK_SUCCESS)return false;
  return vkQueueSubmit(core.queue,1,&submit,fence)==VK_SUCCESS;
}
void DestroySwapchain() {
  if(!core.device)return;
  if(vkDeviceWaitIdle)vkDeviceWaitIdle(core.device);
  if(shader_chain){auto* api=SwanShaders::Functions();if(api)SwanShaders::Check(api->free(&shader_chain));shader_chain=nullptr;}
  shader_generation=0;shader_counter=0;shader_active=false;
  if(swapchain)vkDestroySwapchainKHR(core.device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;swap_images.clear();
  if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;
}
void SetImage(void*,const retro_vulkan_image* value,uint32_t count,const VkSemaphore* semaphores,uint32_t family) {
  has_image=value && (family==VK_QUEUE_FAMILY_IGNORED || family==core.queue_family_index);
  if(has_image)image=*value;
  waits.clear();if(count&&semaphores)waits.assign(semaphores,semaphores+count);
}
uint32_t SyncIndex(void*) {return 0;}
uint32_t SyncMask(void*) {return 1;}
void WaitIndex(void*) {if(fence)vkWaitForFences(core.device,1,&fence,VK_TRUE,2000000000ull);}
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
  negotiated=false;
#define CLEAR_FUNCTION(name) name=nullptr;
  INSTANCE_FUNCTIONS(CLEAR_FUNCTION) DEVICE_FUNCTIONS(CLEAR_FUNCTION)
#undef CLEAR_FUNCTION
  // Always resolve the newly selected driver; never reuse the last session's selection.
  loader=nullptr;entry=nullptr;device_entry=nullptr;
  const bool custom=!driver_dir.empty()&&!driver_name.empty();
  if(custom){
    const auto name=std::filesystem::path(driver_name);
    std::error_code error;const auto folder=std::filesystem::canonical(driver_dir,error);
    const auto file=std::filesystem::canonical(folder/name,error);
    const bool safe=!error&&name.filename()==name&&file.parent_path()==folder&&
      (folder.string().rfind("/data/user/",0)==0||folder.string().rfind("/data/data/",0)==0);
    if(safe){const int flags=ADRENOTOOLS_DRIVER_CUSTOM|(redirect_dir.empty()?0:ADRENOTOOLS_DRIVER_FILE_REDIRECT);
      loader=adrenotools_open_libvulkan(RTLD_NOW,flags,nullptr,hook_dir.c_str(),driver_dir.c_str(),driver_name.c_str(),redirect_dir.empty()?nullptr:redirect_dir.c_str(),nullptr);}
    if(!loader)Error("Custom Vulkan loader unavailable; using system Vulkan for this boot.");
  }
  if(!loader)loader=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
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
  const char* device_extensions[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  // The pinned core owns its helper resources; the returned device belongs to this host.
  if(!requested->create_device(&core,instance,VK_NULL_HANDLE,surface,entry,device_extensions,1,nullptr,0,nullptr))return false;
  device_entry=reinterpret_cast<PFN_vkGetDeviceProcAddr>(entry(instance,"vkGetDeviceProcAddr"));if(!device_entry)return false;
#define LOAD_DEVICE(name) name=reinterpret_cast<PFN_##name>(device_entry(core.device,#name));if(!name)return false;
  DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=core.queue_family_index;pool_info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if(vkCreateCommandPool(core.device,&pool_info,nullptr,&pool)!=VK_SUCCESS)return false;
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
  if(vkAllocateCommandBuffers(core.device,&allocate,&command)!=VK_SUCCESS)return false;
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence_info.flags=VK_FENCE_CREATE_SIGNALED_BIT;
  if(vkCreateFence(core.device,&fence_info,nullptr,&fence)!=VK_SUCCESS)return false;
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  if(vkCreateSemaphore(core.device,&semaphore_info,nullptr,&acquired)!=VK_SUCCESS || vkCreateSemaphore(core.device,&semaphore_info,nullptr,&complete)!=VK_SUCCESS)return false;
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
  if(vkCreateSwapchainKHR(core.device,&chain,nullptr,&swapchain)!=VK_SUCCESS)return false;
  vkGetSwapchainImagesKHR(core.device,swapchain,&count,nullptr);swap_images.resize(count);
  return vkGetSwapchainImagesKHR(core.device,swapchain,&count,swap_images.data())==VK_SUCCESS;
}
const retro_hw_render_interface_vulkan* SwanVulkan::Interface(){return negotiated?&api:nullptr;}
bool SwanVulkan::Ready(){return negotiated;}
bool SwanVulkan::Present(unsigned width,unsigned height,float aspect,bool suspended) {
  if(!core.device)return false;std::lock_guard lock(queue_mutex);
  const bool draw=swapchain&&!suspended;
  const auto selection=SwanShaders::Selected();
  const bool rotated=presentation.rotation!=0;
  if(has_image&&(selection.generation!=shader_generation||rotated!=rotation_chain)){vkDeviceWaitIdle(core.device);
    if(shader_chain){auto* api=SwanShaders::Functions();if(api)SwanShaders::Check(api->free(&shader_chain));shader_chain=nullptr;}
    shader_generation=selection.generation;rotation_chain=rotated;shader_active=false;
    const auto path=selection.enabled&&!selection.path.empty()?selection.path:rotated?SwanShaders::PresentationPreset():std::string();
    if(swap_color_attachment&&!path.empty()){
      auto* api=SwanShaders::Functions();SwanShaders::Handle preset=nullptr;
      if(api&&SwanShaders::Check(api->preset_create(path.c_str(),&preset))){
        SwanShaders::Options options{5,1,true,false,true};SwanShaders::Device device{core.gpu,instance,core.device,core.queue,entry};
        if(SwanShaders::Check(api->create(&preset,device,&options,&shader_chain))&&selection.enabled)for(const auto& parameter:selection.parameters)SwanShaders::Check(api->parameter(&shader_chain,parameter.first.c_str(),parameter.second));
        if(preset)SwanShaders::Check(api->preset_free(&preset));
      }
      if(!shader_chain)Error("Vulkan preset failed; keeping plain presentation.");
    }
  }
  uint32_t index=0;
  if(draw){auto result=vkAcquireNextImageKHR(core.device,swapchain,2000000000ull,acquired,VK_NULL_HANDLE,&index);
    if(result==VK_ERROR_OUT_OF_DATE_KHR){if(!Window(retained_window)||!swapchain)return false;
      result=vkAcquireNextImageKHR(core.device,swapchain,2000000000ull,acquired,VK_NULL_HANDLE,&index);}
    if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR)return false;}
  if(!Begin())return false;
  if(draw){const auto target=swap_images[index];
    Barrier(target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black{{0,0,0,1}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(command,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
    const bool filtered=has_image&&width&&height&&shader_chain;
    if(filtered){
      Barrier(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      Barrier(image.create_info.image,image.image_layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
      const auto rect=SwanPresentation::Fit(extent.width,extent.height,width,height,aspect,presentation);
      SwanShaders::Viewport viewport{float(rect.x),float(rect.y),uint32_t(rect.width),uint32_t(rect.height)};
      auto* api=SwanShaders::Functions();
      const SwanShaders::Image source{image.create_info.image,image.create_info.format,width,height},destination{target,swap_format,extent.width,extent.height};
      const auto matrix=SwanPresentation::Matrix(presentation.rotation);
      if(!SwanShaders::Check(api->frame(&shader_chain,command,shader_counter++,source,destination,&viewport,matrix.data(),nullptr))){Error("Vulkan shader frame failed.");return false;}
      shader_active=selection.enabled;
      Barrier(image.create_info.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image.image_layout,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    }else if(has_image && width&&height) {
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
  if(!Submit(draw))return false;
  if(draw){VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&complete;present.swapchainCount=1;present.pSwapchains=&swapchain;present.pImageIndices=&index;
    const auto result=vkQueuePresentKHR(core.queue,&present);
    // One in-flight frame: safe reuse of the core image and binary presentation semaphore.
    if(vkQueueWaitIdle(core.queue)!=VK_SUCCESS)return false;
    if(result==VK_ERROR_OUT_OF_DATE_KHR)return Window(retained_window);
    // Android may report SUBOPTIMAL every frame when identity differs from the
    // panel transform. This is intentional compositor rotation, not a stale
    // swapchain. Real resize/recreation is handled by Window/OUT_OF_DATE; do not
    // stall and recreate shader resources on every otherwise-successful frame.
    return result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR;}
  return true;
}
void SwanVulkan::Blank(){has_image=false;if(core.device)Present(0,0,4.f/3,false);}
void SwanVulkan::Close(){
  if(core.device && vkDeviceWaitIdle)vkDeviceWaitIdle(core.device);
  if(core.device && vkDestroySwapchainKHR)DestroySwapchain();
  if(core.device){if(acquired&&vkDestroySemaphore)vkDestroySemaphore(core.device,acquired,nullptr);if(complete&&vkDestroySemaphore)vkDestroySemaphore(core.device,complete,nullptr);
    if(fence&&vkDestroyFence)vkDestroyFence(core.device,fence,nullptr);if(pool&&vkDestroyCommandPool)vkDestroyCommandPool(core.device,pool,nullptr);}
  if(negotiation.destroy_device)negotiation.destroy_device();
  if(core.device&&vkDestroyDevice)vkDestroyDevice(core.device,nullptr);
  if(surface&&vkDestroySurfaceKHR)vkDestroySurfaceKHR(instance,surface,nullptr);
  if(instance&&vkDestroyInstance)vkDestroyInstance(instance,nullptr);
  core={};api={};negotiation={};image={};instance=VK_NULL_HANDLE;surface=VK_NULL_HANDLE;swapchain=VK_NULL_HANDLE;
  if(retained_window)ANativeWindow_release(retained_window);retained_window=nullptr;negotiated=false;rotation_chain=false;
  acquired=complete=signal_semaphore=VK_NULL_HANDLE;fence=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;command=VK_NULL_HANDLE;has_image=false;waits.clear();core_commands.clear();
  // Keep the loader resident: the pinned core may retain proc-address trampolines.
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
      if(Submit(false)&&vkWaitForFences(core.device,1,&fence,VK_TRUE,2000000000ull)==VK_SUCCESS){void* mapped=nullptr;
        if(vkMapMemory(core.device,memory,0,size,0,&mapped)==VK_SUCCESS){output.resize(size);std::memcpy(output.data(),mapped,size);vkUnmapMemory(core.device,memory);
          if(image.create_info.format==VK_FORMAT_B8G8R8A8_UNORM)for(size_t i=0;i<output.size();i+=4)std::swap(output[i],output[i+2]);}
      }
    }
  }
  vkDeviceWaitIdle(core.device);vkDestroyBuffer(core.device,buffer,nullptr);if(memory)vkFreeMemory(core.device,memory,nullptr);return output;
}
void SwanVulkan::Driver(std::string directory,std::string name,std::string redirect,std::string hooks){
  driver_dir=std::move(directory);driver_name=std::move(name);redirect_dir=std::move(redirect);hook_dir=std::move(hooks);
}
void SwanVulkan::Presentation(SwanPresentation::Options options){presentation=options;}
bool SwanVulkan::ShaderActive(){return shader_active;}
