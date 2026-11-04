#pragma once
#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <utility>
#include <cstdint>

struct PsxVulkanContext {
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    uint32_t family=0;
    void (*lockQueue)()=nullptr;
    void (*unlockQueue)()=nullptr;
    void (*retireImage)(void*,VkImage)=nullptr;
    void* owner=nullptr;
    bool orderedColor=false;
};

// Original integer raster rules, native Vulkan resources and commands only.
// The GLES renderer is a separate backend and does not participate in these draws.
class NativeVulkanGpu {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    NativeVulkanGpu(PsxVulkanContext context,int scale);
    ~NativeVulkanGpu();
    NativeVulkanGpu(const NativeVulkanGpu&)=delete;
    NativeVulkanGpu& operator=(const NativeVulkanGpu&)=delete;
    void batch(const int* words,int length);
    void read(int* pixels);
    void set(const int* pixels);
    void fill(int color,int xy,int wh);
    void copy(int from,int to,int wh,int mask);
    void upload(const int* words,int count,int start,int x,int y,int width,int total,int mask);
    std::pair<int,int> scanout(const int* state,bool nativeMovie=false);
    void capture(const int* state,uint32_t* argb);
    void submit();
    void wait();
    VkImage outputImage()const;
    std::string description()const;
    void statistics(int64_t* out)const;
    void drawCounts(int64_t* out)const;
    uint64_t revision()const;
    bool fastHd()const;
};
