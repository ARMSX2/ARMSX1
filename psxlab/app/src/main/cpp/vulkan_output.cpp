#include "display_layout.h"
// Original Vulkan display backend. Rasterization remains in our software core.
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
#include <jni.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <memory>
#include <atomic>

namespace {
void checked(VkResult r,const char* operation){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" (Vulkan "+std::to_string(r)+")");}
class Output {
    double outputAspect=4.0/3.0;bool outputStretch=false,outputInteger=false;

    VkInstance instance=VK_NULL_HANDLE;VkPhysicalDevice physical=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;VkQueue queue=VK_NULL_HANDLE;uint32_t family=0;
    VkSurfaceKHR surface=VK_NULL_HANDLE;ANativeWindow* window=nullptr;VkSwapchainKHR chain=VK_NULL_HANDLE;VkExtent2D extent{};VkSurfaceCapabilitiesKHR chainCapabilities{};std::vector<VkImage> images;std::vector<VkSemaphore> hdFinished;
    VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;VkSemaphore acquired=VK_NULL_HANDLE,rendered=VK_NULL_HANDLE;
    VkBuffer staging=VK_NULL_HANDLE;VkDeviceMemory stagingMemory=VK_NULL_HANDLE;void* mapped=nullptr;const int maxPixels;
    VkImage source=VK_NULL_HANDLE;VkDeviceMemory sourceMemory=VK_NULL_HANDLE;int sourceW=0,sourceH=0;bool sourceReady=false,linear=true;
    bool sharedSupported=false,sharedAsync=false;VkSemaphore producerReady=VK_NULL_HANDLE;PFN_vkImportSemaphoreFdKHR importReadyFd=nullptr;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getHardwareProperties=nullptr;
    struct SharedAllocation{AHardwareBuffer* buffer;VkImage image;VkDeviceMemory memory;};std::vector<SharedAllocation> sharedAllocations;AHardwareBuffer* sharedBuffer=nullptr;VkImage sharedImage=VK_NULL_HANDLE;VkDeviceMemory sharedMemory=VK_NULL_HANDLE;
    jlong previousJavaToken=0;int previousJavaWindowW=0,previousJavaWindowH=0;bool previousJavaFilter=false;
    std::atomic<uint64_t> presented{0},reused{0},javaUploadBytes{0},chainBuilds{0},suboptimalPresents{0},outdatedPresents{0};
    void dropShared(){for(auto& allocation:sharedAllocations){vkDestroyImage(device,allocation.image,nullptr);vkFreeMemory(device,allocation.memory,nullptr);AHardwareBuffer_release(allocation.buffer);}sharedAllocations.clear();sharedImage=VK_NULL_HANDLE;sharedMemory=VK_NULL_HANDLE;sharedBuffer=nullptr;}
    void importShared(AHardwareBuffer* buffer,int w,int h){
        if(sharedBuffer==buffer)return;
        for(const auto& allocation:sharedAllocations)if(allocation.buffer==buffer){sharedBuffer=buffer;sharedImage=allocation.image;sharedMemory=allocation.memory;return;}
        if(sharedAllocations.size()>=4){idle();dropShared();}
        VkAndroidHardwareBufferFormatPropertiesANDROID format{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};properties.pNext=&format;checked(getHardwareProperties(device,buffer,&properties),"Shared buffer properties");
        if(format.format==VK_FORMAT_UNDEFINED||!(format.formatFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))throw std::runtime_error("Shared RGBA buffer cannot be presented");
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.pNext=&external;image.imageType=VK_IMAGE_TYPE_2D;image.format=format.format;image.extent={uint32_t(w),uint32_t(h),1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VkImage importedImage=VK_NULL_HANDLE;VkDeviceMemory importedMemory=VK_NULL_HANDLE;
        try{checked(vkCreateImage(device,&image,nullptr,&importedImage),"Import shared image");
            VkImportAndroidHardwareBufferInfoANDROID imported{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};imported.buffer=buffer;VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=importedImage;dedicated.pNext=&imported;
            VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memory.pNext=&dedicated;memory.allocationSize=properties.allocationSize;memory.memoryTypeIndex=memoryType(properties.memoryTypeBits,0);checked(vkAllocateMemory(device,&memory,nullptr,&importedMemory),"Import shared memory");checked(vkBindImageMemory(device,importedImage,importedMemory,0),"Bind shared memory");
        }catch(...){if(importedImage)vkDestroyImage(device,importedImage,nullptr);if(importedMemory)vkFreeMemory(device,importedMemory,nullptr);throw;}
        AHardwareBuffer_acquire(buffer);sharedAllocations.push_back({buffer,importedImage,importedMemory});sharedBuffer=buffer;sharedImage=importedImage;sharedMemory=importedMemory;
    }
    void sharedBarrier(bool acquire){
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=sharedImage;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.srcQueueFamilyIndex=acquire?VK_QUEUE_FAMILY_FOREIGN_EXT:family;barrier.dstQueueFamilyIndex=acquire?family:VK_QUEUE_FAMILY_FOREIGN_EXT;barrier.oldLayout=acquire?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=acquire?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;barrier.srcAccessMask=acquire?0:VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=acquire?VK_ACCESS_TRANSFER_READ_BIT:0;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(physical,&p);for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("Vulkan memory type unavailable");}
    void idle(){if(device)vkDeviceWaitIdle(device);}
    void dropChain(){previousJavaToken=0;idle();for(VkSemaphore s:hdFinished)if(s)vkDestroySemaphore(device,s,nullptr);hdFinished.clear();if(chain)vkDestroySwapchainKHR(device,chain,nullptr);chain=VK_NULL_HANDLE;images.clear();}
    void detach(){dropChain();if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;if(window)ANativeWindow_release(window);window=nullptr;}
    void dropSource(){previousJavaToken=0;if(source)vkDestroyImage(device,source,nullptr);if(sourceMemory)vkFreeMemory(device,sourceMemory,nullptr);source=VK_NULL_HANDLE;sourceMemory=VK_NULL_HANDLE;sourceReady=false;}
    void imageBarrier(VkImage image,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=from;b.newLayout=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);}
    void begin(){checked(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"Display fence");checked(vkResetFences(device,1,&fence),"Reset fence");checked(vkResetCommandBuffer(command,0),"Reset command buffer");VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;checked(vkBeginCommandBuffer(command,&info),"Begin display commands");}
    void submit(bool present,VkSemaphore finished=VK_NULL_HANDLE,VkSemaphore producer=VK_NULL_HANDLE){if(!finished)finished=rendered;checked(vkEndCommandBuffer(command),"End display commands");VkPipelineStageFlags stages[2]={VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};VkSemaphore waits[2]={acquired,producer};VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;if(present){info.waitSemaphoreCount=producer?2:1;info.signalSemaphoreCount=1;info.pWaitSemaphores=waits;info.pWaitDstStageMask=stages;info.pSignalSemaphores=&finished;}checked(vkQueueSubmit(queue,1,&info,fence),"Submit display");}
    void upload(const jint* pixels,int w,int h,bool rgba=false){if(w<1||h<1||int64_t(w)*h>maxPixels)throw std::runtime_error("Vulkan frame exceeds its upload buffer");
        // Upload callers have waited the submission fence; presentation owns swapchain images, not this source.
        if(!source || w!=sourceW || h!=sourceH){dropSource();VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R8G8B8A8_UNORM;image.extent={uint32_t(w),uint32_t(h),1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vkCreateImage(device,&image,nullptr,&source),"Create source image");VkMemoryRequirements r{};vkGetImageMemoryRequirements(device,source,&r);VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=r.size;alloc.memoryTypeIndex=memoryType(r.memoryTypeBits,0);checked(vkAllocateMemory(device,&alloc,nullptr,&sourceMemory),"Allocate source image");checked(vkBindImageMemory(device,source,sourceMemory,0),"Bind source image");sourceW=w;sourceH=h;}
        if(!pixels)return;
        if(rgba){std::memcpy(mapped,pixels,size_t(w)*h*4);return;}auto* out=static_cast<uint32_t*>(mapped);for(int i=0;i<w*h;i++){uint32_t v=uint32_t(pixels[i]);out[i]=(v&0xff00ff00u)|((v&255u)<<16)|((v>>16)&255u);}
    }
    // The caller has waited for the previous submission's fence. Copy Java pixels
    // into our persistent owned staging allocation, never pinning Java memory over a wait.
    void uploadJava(JNIEnv* env,jintArray pixels,int w,int h){
        upload(nullptr,w,h);env->GetIntArrayRegion(pixels,0,w*h,static_cast<jint*>(mapped));
        if(env->ExceptionCheck())return;javaUploadBytes+=uint64_t(w)*h*4;
        auto* out=static_cast<uint32_t*>(mapped);for(int i=0;i<w*h;i++){uint32_t v=out[i];out[i]=(v&0xff00ff00u)|((v&255u)<<16)|((v>>16)&255u);}
    }
    void copyUpload(){imageBarrier(source,sourceReady?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,sourceReady?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={uint32_t(sourceW),uint32_t(sourceH),1};vkCmdCopyBufferToImage(command,staging,source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);imageBarrier(source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);sourceReady=true;}
    void createChain(){
        VkSurfaceCapabilitiesKHR caps{};checked(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"Surface capabilities");if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Vulkan surface cannot receive display transfers");uint32_t count=0;checked(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"Surface formats");std::vector<VkSurfaceFormatKHR> formats(count);checked(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"Surface formats");if(formats.empty())throw std::runtime_error("No Vulkan surface format");auto format=formats[0];if(format.format==VK_FORMAT_UNDEFINED)format.format=VK_FORMAT_R8G8B8A8_UNORM;for(auto f:formats)if(f.format==VK_FORMAT_R8G8B8A8_UNORM || f.format==VK_FORMAT_B8G8R8A8_UNORM){format=f;break;}
        VkFormatProperties destination{},src{};vkGetPhysicalDeviceFormatProperties(physical,format.format,&destination);vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&src);if(!(destination.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)||!(src.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))throw std::runtime_error("Vulkan image scaling unsupported");linear=(src.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)!=0;
        extent=caps.currentExtent;if(extent.width==UINT32_MAX){extent.width=std::clamp(uint32_t(std::max(1,ANativeWindow_getWidth(window))),caps.minImageExtent.width,caps.maxImageExtent.width);extent.height=std::clamp(uint32_t(std::max(1,ANativeWindow_getHeight(window))),caps.minImageExtent.height,caps.maxImageExtent.height);}
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};info.surface=surface;info.minImageCount=std::max(2u,caps.minImageCount);if(caps.maxImageCount)info.minImageCount=std::min(info.minImageCount,caps.maxImageCount);info.imageFormat=format.format;info.imageColorSpace=format.colorSpace;info.imageExtent=extent;info.imageArrayLayers=1;info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;if(!(caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))throw std::runtime_error("Vulkan upright surface transform unavailable");info.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;info.compositeAlpha=(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)?VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR:VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));info.presentMode=VK_PRESENT_MODE_FIFO_KHR;info.clipped=VK_TRUE;checked(vkCreateSwapchainKHR(device,&info,nullptr,&chain),"Create display swapchain");chainCapabilities=caps;chainBuilds++;checked(vkGetSwapchainImagesKHR(device,chain,&count,nullptr),"Swapchain images");images.resize(count);checked(vkGetSwapchainImagesKHR(device,chain,&count,images.data()),"Swapchain images");
    }
    bool surfaceChanged(){
        VkSurfaceCapabilitiesKHR caps{};checked(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"Updated surface capabilities");
        VkExtent2D desired=caps.currentExtent;if(desired.width==UINT32_MAX){desired.width=std::clamp(uint32_t(std::max(1,ANativeWindow_getWidth(window))),caps.minImageExtent.width,caps.maxImageExtent.width);desired.height=std::clamp(uint32_t(std::max(1,ANativeWindow_getHeight(window))),caps.minImageExtent.height,caps.maxImageExtent.height);}
        return desired.width!=extent.width||desired.height!=extent.height||caps.currentTransform!=chainCapabilities.currentTransform||caps.supportedTransforms!=chainCapabilities.supportedTransforms||caps.minImageCount!=chainCapabilities.minImageCount||caps.maxImageCount!=chainCapabilities.maxImageCount||caps.supportedUsageFlags!=chainCapabilities.supportedUsageFlags||caps.supportedCompositeAlpha!=chainCapabilities.supportedCompositeAlpha;
    }
    bool finishPresent(VkResult result,const char* operation){
        if(result==VK_ERROR_OUT_OF_DATE_KHR){outdatedPresents++;dropChain();return false;}
        if(result==VK_SUBOPTIMAL_KHR){
            // Presentation succeeded. Android can repeat this for a stable identity
            // transform: rebuilding the same swapchain every field achieves nothing.
            suboptimalPresents++;presented++;if(surfaceChanged())dropChain();return true;
        }
        checked(result,operation);presented++;return true;
    }
public:
    std::string name;
    Output(int capacity=1024*512):maxPixels(capacity){if(capacity<1||capacity>1024*512*25)throw std::runtime_error("Invalid Vulkan frame capacity");try{
        const char* extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="PSX Lab";uint32_t loaderVersion=VK_API_VERSION_1_0;auto enumerateVersion=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr,"vkEnumerateInstanceVersion"));if(enumerateVersion)checked(enumerateVersion(&loaderVersion),"Vulkan loader version");app.apiVersion=capacity>1024*512&&loaderVersion>=VK_API_VERSION_1_1?VK_API_VERSION_1_1:VK_API_VERSION_1_0;VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;create.enabledExtensionCount=2;create.ppEnabledExtensionNames=extensions;checked(vkCreateInstance(&create,nullptr,&instance),"Create Vulkan instance");uint32_t count=0;checked(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate Vulkan devices");std::vector<VkPhysicalDevice> candidates(count);checked(vkEnumeratePhysicalDevices(instance,&count,candidates.data()),"Enumerate Vulkan devices");for(VkPhysicalDevice p:candidates){uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(p,&n,families.data());for(uint32_t i=0;i<n;i++)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){physical=p;family=i;break;}if(physical)break;}if(!physical)throw std::runtime_error("No Vulkan graphics device");VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);name=properties.deviceName;
        float priority=1;VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueFamilyIndex=family;q.queueCount=1;q.pQueuePriorities=&priority;const char* swapchain=VK_KHR_SWAPCHAIN_EXTENSION_NAME;VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};d.queueCreateInfoCount=1;d.pQueueCreateInfos=&q;std::vector<const char*> deviceExtensions{swapchain};
        if(app.apiVersion>=VK_API_VERSION_1_1&&properties.apiVersion>=VK_API_VERSION_1_1){uint32_t count=0;checked(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr),"Vulkan extension count");std::vector<VkExtensionProperties> available(count);checked(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,available.data()),"Vulkan extensions");auto has=[&](const char* name){return std::any_of(available.begin(),available.end(),[&](const auto& e){return std::strcmp(e.extensionName,name)==0;});};sharedSupported=has(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)&&has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);if(sharedSupported){deviceExtensions.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);deviceExtensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);sharedAsync=has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);if(sharedAsync)deviceExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);}}
        d.enabledExtensionCount=deviceExtensions.size();d.ppEnabledExtensionNames=deviceExtensions.data();checked(vkCreateDevice(physical,&d,nullptr,&device),"Create Vulkan device");vkGetDeviceQueue(device,family,0,&queue);getHardwareProperties=reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(vkGetDeviceProcAddr(device,"vkGetAndroidHardwareBufferPropertiesANDROID"));sharedSupported=sharedSupported&&getHardwareProperties;importReadyFd=reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(device,"vkImportSemaphoreFdKHR"));sharedAsync=sharedSupported&&sharedAsync&&importReadyFd;
        if(sharedAsync){auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceExternalSemaphoreProperties"));VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};info.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;VkExternalSemaphoreProperties result{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};if(query)query(physical,&info,&result);sharedAsync=query&&(result.externalSemaphoreFeatures&VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);}

        VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};p.queueFamilyIndex=family;p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;checked(vkCreateCommandPool(device,&p,nullptr,&pool),"Create command pool");VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;checked(vkAllocateCommandBuffers(device,&a,&command),"Allocate command buffer");VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};f.flags=VK_FENCE_CREATE_SIGNALED_BIT;checked(vkCreateFence(device,&f,nullptr,&fence),"Create fence");VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};checked(vkCreateSemaphore(device,&s,nullptr,&acquired),"Create acquire semaphore");checked(vkCreateSemaphore(device,&s,nullptr,&rendered),"Create present semaphore");if(sharedAsync)checked(vkCreateSemaphore(device,&s,nullptr,&producerReady),"Create shared producer semaphore");
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=VkDeviceSize(maxPixels)*4;b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;checked(vkCreateBuffer(device,&b,nullptr,&staging),"Create upload buffer");VkMemoryRequirements r{};vkGetBufferMemoryRequirements(device,staging,&r);VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};m.allocationSize=r.size;m.memoryTypeIndex=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);checked(vkAllocateMemory(device,&m,nullptr,&stagingMemory),"Allocate upload buffer");checked(vkBindBufferMemory(device,staging,stagingMemory,0),"Bind upload buffer");checked(vkMapMemory(device,stagingMemory,0,VK_WHOLE_SIZE,0,&mapped),"Map upload buffer");
    }catch(...){destroy();throw;}}
    void configureDisplay(float aspect,bool stretch,bool integerScale){
        double next=std::isfinite(aspect)&&aspect>=0.1f&&aspect<=10.f?aspect:4.0/3.0;
        if(outputAspect==next&&outputStretch==stretch&&outputInteger==integerScale)return;
        outputAspect=next;outputStretch=stretch;outputInteger=integerScale;previousJavaToken=0;
    }
    ~Output(){destroy();}
    void destroy(){idle();detach();if(device){dropShared();dropSource();if(mapped)vkUnmapMemory(device,stagingMemory);if(staging)vkDestroyBuffer(device,staging,nullptr);if(stagingMemory)vkFreeMemory(device,stagingMemory,nullptr);if(producerReady)vkDestroySemaphore(device,producerReady,nullptr);if(acquired)vkDestroySemaphore(device,acquired,nullptr);if(rendered)vkDestroySemaphore(device,rendered,nullptr);if(fence)vkDestroyFence(device,fence,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;}if(instance){vkDestroyInstance(instance,nullptr);instance=VK_NULL_HANDLE;}}
    bool present(JNIEnv* env,jobject target,const jint* pixels,int w,int h,bool filter,bool rgba=false,jintArray javaPixels=nullptr,jlong frameToken=0){
        // Android may resize a newly attached surface between creation and acquisition.
        for(int attempt=0;attempt<2;attempt++)if(presentOnce(env,target,pixels,w,h,filter,rgba,javaPixels,frameToken))return true;
        return false;
    }
    bool presentOnce(JNIEnv* env,jobject target,const jint* pixels,int w,int h,bool filter,bool rgba,jintArray javaPixels,jlong frameToken){
        std::unique_ptr<ANativeWindow,decltype(&ANativeWindow_release)> requested(ANativeWindow_fromSurface(env,target),ANativeWindow_release);if(!requested)return false;
        int targetW=ANativeWindow_getWidth(requested.get()),targetH=ANativeWindow_getHeight(requested.get());
        // An unchanged picture remains in the compositor. Do not upload or submit it again.
        // Retain window identity, dimensions and filter in the key so recreation/resize redraws.
        if(javaPixels&&frameToken>0&&frameToken==previousJavaToken&&requested.get()==window&&chain&&sourceReady&&sourceW==w&&sourceH==h&&targetW==previousJavaWindowW&&targetH==previousJavaWindowH&&filter==previousJavaFilter){reused++;return true;}
        checked(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"Display fence");
        if(requested.get()!=window){detach();window=requested.release();VkAndroidSurfaceCreateInfoKHR s{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};s.window=window;checked(vkCreateAndroidSurfaceKHR(instance,&s,nullptr,&surface),"Create Android Vulkan surface");VkBool32 supported=VK_FALSE;checked(vkGetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported),"Presentation support");if(!supported)throw std::runtime_error("Vulkan queue cannot present this surface");}else requested.reset();
        if(!chain)createChain();uint32_t index=0;VkResult r=vkAcquireNextImageKHR(device,chain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index);if(r==VK_ERROR_OUT_OF_DATE_KHR){dropChain();return false;}if(r!=VK_SUBOPTIMAL_KHR)checked(r,"Acquire display image");
        VkSemaphore finished=VK_NULL_HANDLE;
        // Per-image semaphores remain owned by presentation until that image is acquired again.
        {if(hdFinished.empty()){hdFinished.resize(images.size(),VK_NULL_HANDLE);VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};for(auto& semaphore:hdFinished)checked(vkCreateSemaphore(device,&info,nullptr,&semaphore),"HD presentation semaphore");}finished=hdFinished[index];}
        if(javaPixels){uploadJava(env,javaPixels,w,h);if(env->ExceptionCheck())return false;}else upload(pixels,w,h,rgba);begin();copyUpload();imageBarrier(images[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);VkClearColorValue black{{0,0,0,1}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        VkMemoryBarrier order{VK_STRUCTURE_TYPE_MEMORY_BARRIER};order.srcAccessMask=order.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&order,0,nullptr,0,nullptr);
        auto rect=psxDisplayRect(int(extent.width),int(extent.height),w,h,outputAspect,outputStretch,outputInteger);int dw=rect.w,dh=rect.h,x=rect.x,y=rect.y;VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[1]={w,h,1};blit.dstOffsets[0]={x,y,0};blit.dstOffsets[1]={x+dw,y+dh,1};vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter&&linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);imageBarrier(images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0);submit(true,finished);
        VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};p.waitSemaphoreCount=1;p.pWaitSemaphores=&finished;p.swapchainCount=1;p.pSwapchains=&chain;p.pImageIndices=&index;r=vkQueuePresentKHR(queue,&p);if(!finishPresent(r,"Present display image"))return false;previousJavaToken=javaPixels&&chain?frameToken:0;previousJavaWindowW=targetW;previousJavaWindowH=targetH;previousJavaFilter=filter;return true;
    }
    bool supportsShared()const{return sharedSupported;}
    bool supportsSharedAsync()const{return sharedAsync;}
    bool presentShared(JNIEnv* env,jobject target,AHardwareBuffer* buffer,int w,int h,bool filter,int readyFd){struct OwnedFd{int value;~OwnedFd(){if(value>=0)close(value);}} ready{readyFd};for(int attempt=0;attempt<2;attempt++)if(presentSharedOnce(env,target,buffer,w,h,filter,ready.value))return true;return false;}
    bool presentSharedOnce(JNIEnv* env,jobject target,AHardwareBuffer* buffer,int w,int h,bool filter,int& readyFd){
        previousJavaToken=0;checked(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"Shared display fence");importShared(buffer,w,h);
        ANativeWindow* requested=ANativeWindow_fromSurface(env,target);if(!requested)return false;
        if(requested!=window){detach();window=requested;VkAndroidSurfaceCreateInfoKHR s{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};s.window=window;checked(vkCreateAndroidSurfaceKHR(instance,&s,nullptr,&surface),"Create shared presentation surface");VkBool32 supported=VK_FALSE;checked(vkGetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported),"Shared presentation support");if(!supported)throw std::runtime_error("Shared presentation queue unavailable");}else ANativeWindow_release(requested);
        if(!chain)createChain();uint32_t index=0;VkResult r=vkAcquireNextImageKHR(device,chain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index);if(r==VK_ERROR_OUT_OF_DATE_KHR){dropChain();return false;}if(r!=VK_SUBOPTIMAL_KHR)checked(r,"Acquire shared presentation image");
        if(hdFinished.empty()){hdFinished.resize(images.size(),VK_NULL_HANDLE);VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};for(auto& semaphore:hdFinished)checked(vkCreateSemaphore(device,&info,nullptr,&semaphore),"Shared presentation semaphore");}VkSemaphore finished=hdFinished[index];
        VkSemaphore producer=VK_NULL_HANDLE;if(readyFd>=0){VkImportSemaphoreFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};imported.semaphore=producerReady;imported.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;imported.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;imported.fd=readyFd;checked(importReadyFd(device,&imported),"Import shared producer fence");readyFd=-1;producer=producerReady;}
        begin();sharedBarrier(true);imageBarrier(images[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);VkClearColorValue black{{0,0,0,1}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        VkMemoryBarrier order{VK_STRUCTURE_TYPE_MEMORY_BARRIER};order.srcAccessMask=order.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&order,0,nullptr,0,nullptr);
        auto rect=psxDisplayRect(int(extent.width),int(extent.height),w,h,outputAspect,outputStretch,outputInteger);int dw=rect.w,dh=rect.h,x=rect.x,y=rect.y;VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[1]={w,h,1};blit.dstOffsets[0]={x,y,0};blit.dstOffsets[1]={x+dw,y+dh,1};vkCmdBlitImage(command,sharedImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter&&linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
        sharedBarrier(false);imageBarrier(images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0);submit(true,finished,producer);
        VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};p.waitSemaphoreCount=1;p.pWaitSemaphores=&finished;p.swapchainCount=1;p.pSwapchains=&chain;p.pImageIndices=&index;r=vkQueuePresentKHR(queue,&p);
        // The next call waits this fence; ping-pong allocations let GL render concurrently.
        return finishPresent(r,"Present shared image");
    }
    void statistics(jlong* values)const{values[0]=presented.load();values[1]=reused.load();values[2]=javaUploadBytes.load();values[3]=chainBuilds.load();values[4]=suboptimalPresents.load();values[5]=outdatedPresents.load();}
    void verify(JNIEnv* env){
        for(bool javaPath:{false,true})for(auto dimensions:{std::pair<int,int>{32,24},{320,240},{17,31}}){
            int w=dimensions.first,h=dimensions.second;std::vector<jint> pixels(w*h);
            for(size_t i=0;i<pixels.size();i++)pixels[i]=int(uint32_t(i*7919)^0xa57129c3u);
            checked(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"Verify upload fence");
            if(javaPath){jintArray array=env->NewIntArray(w*h);if(!array)throw std::runtime_error("Java upload fixture allocation");env->SetIntArrayRegion(array,0,w*h,pixels.data());uploadJava(env,array,w,h);env->DeleteLocalRef(array);if(env->ExceptionCheck())throw std::runtime_error("Java upload fixture failed");}
            else upload(pixels.data(),w,h);
            begin();copyUpload();VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={uint32_t(w),uint32_t(h),1};vkCmdCopyImageToBuffer(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging,1,&copy);VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);submit(false);checked(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"Verify display transfer");
            auto* out=static_cast<uint32_t*>(mapped);for(size_t i=0;i<pixels.size();i++){uint32_t v=uint32_t(pixels[i]),expected=(v&0xff00ff00u)|((v&255)<<16)|((v>>16)&255);if(out[i]!=expected)throw std::runtime_error("Vulkan source image readback differs");}
        }
    }
};
void failure(JNIEnv* e,const std::exception& error){e->ThrowNew(e->FindClass("java/lang/IllegalStateException"),error.what());}
}
extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_VulkanPresenter_create(JNIEnv* e,jclass,jint capacity){try{return reinterpret_cast<jlong>(new Output(capacity));}catch(const std::exception& x){failure(e,x);return 0;}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_VulkanPresenter_destroy(JNIEnv*,jclass,jlong h){delete reinterpret_cast<Output*>(h);}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_psxlab_core_VulkanPresenter_presentNative(JNIEnv* e,jclass,jlong handle,jobject surface,jintArray data,jint w,jint h,jboolean filter,jlong token){try{if(w<1||w>5120||h<1||h>2560||e->GetArrayLength(data)!=w*h)throw std::runtime_error("Invalid Vulkan frame");return reinterpret_cast<Output*>(handle)->present(e,surface,nullptr,w,h,filter,false,data,token);}catch(const std::exception& x){failure(e,x);return false;}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_VulkanPresenter_statisticsNative(JNIEnv* e,jclass,jlong handle,jlongArray out){if(e->GetArrayLength(out)!=6){e->ThrowNew(e->FindClass("java/lang/IllegalArgumentException"),"Vulkan statistics length");return;}jlong values[6];reinterpret_cast<Output*>(handle)->statistics(values);e->SetLongArrayRegion(out,0,6,values);}
extern "C" JNIEXPORT jstring JNICALL Java_dev_psxlab_core_VulkanPresenter_verifyNative(JNIEnv* e,jclass){try{Output output;output.verify(e);std::string text="Vulkan native/Java staging upload/readback: six fixtures with source resizing match on "+output.name+".\n";return e->NewStringUTF(text.c_str());}catch(const std::exception& x){failure(e,x);return nullptr;}}

bool psxVulkanPresentRgba(JNIEnv* env,jlong handle,jobject surface,const uint32_t* pixels,int w,int h,bool filter){return reinterpret_cast<Output*>(handle)->present(env,surface,reinterpret_cast<const jint*>(pixels),w,h,filter,true);}

bool psxVulkanSharedSupported(jlong handle){return reinterpret_cast<Output*>(handle)->supportsShared();}
bool psxVulkanPresentShared(JNIEnv* env,jlong handle,jobject surface,AHardwareBuffer* buffer,int w,int h,bool filter,int readyFd){return reinterpret_cast<Output*>(handle)->presentShared(env,surface,buffer,w,h,filter,readyFd);}
bool psxVulkanSharedAsyncSupported(jlong handle){return reinterpret_cast<Output*>(handle)->supportsSharedAsync();}

extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_VulkanPresenter_configureDisplayNative(JNIEnv*,jclass,jlong h,jfloat aspect,jboolean stretch,jboolean integerScale){reinterpret_cast<Output*>(h)->configureDisplay(aspect,stretch,integerScale);}
