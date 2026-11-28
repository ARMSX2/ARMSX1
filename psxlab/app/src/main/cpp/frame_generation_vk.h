#pragma once
// Vulkan frame generation (see frame_generation.h for the algorithm). Records into the caller's
// command buffer; the caller owns submission and waits its fence before resize()/capture.
// Every image this class owns stays in VK_IMAGE_LAYOUT_GENERAL between calls. presentable()
// hands one out in TRANSFER_SRC_OPTIMAL for blits and shader chains, restore() takes it back.
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>
#include <initializer_list>
#include "frame_generation.h"
#include "frame_generation_spirv.h"

namespace psxfg {

class VkFrameGen {
public:
    ~VkFrameGen() { destroy(); }
    void attach(VkPhysicalDevice physicalDevice, VkDevice logicalDevice) {
        if (device == logicalDevice) return;
        destroy();
        physical = physicalDevice; device = logicalDevice;
        try { createPipelines(); } catch (...) { destroy(); throw; }
    }
    bool attached() const { return device != VK_NULL_HANDLE && pipelines[0] != VK_NULL_HANDLE; }
    void destroy() {
        if (!device) return;
        dropImages();
        for (auto& p : pipelines) { if (p) vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; }
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        layout = VK_NULL_HANDLE; setLayout = VK_NULL_HANDLE; pool = VK_NULL_HANDLE; sampler = VK_NULL_HANDLE;
        sets[0] = sets[1] = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
    }
    /** Size the history for w x h frames. Call only when no submitted work uses these images. */
    void resize(int w, int h) {
        Dims next = dims(w, h);
        if (imagesReady && next == d) return;
        dropImages();
        d = next;
        try { createImages(); } catch (...) { dropImages(); throw; }
    }
    bool sized(int w, int h) const { return imagesReady && d.w == w && d.h == h; }
    /** Forget the history, e.g. after a pause or a size change elsewhere. */
    void reset() { havePrevious = false; motionValid = false; }

    /**
     * Copy `input` (in TRANSFER_SRC_OPTIMAL, exactly w x h after resize) into the history as the
     * newest frame and, when asked and possible, estimate motion from the previous one.
     */
    void recordCapture(VkCommandBuffer cmd, VkImage input, bool estimate) {
        prepare(cmd);
        current ^= 1;
        barrier(cmd);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {d.w, d.h, 1};
        vkCmdBlitImage(cmd, input, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color[current].image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
        barrier(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[current], 0, nullptr);
        dispatch(cmd, LUMA, {d.w, d.h, d.s, 0}, {d.mw, d.mh, 0, 0}, 0, d.mw, d.mh);
        barrier(cmd);
        dispatch(cmd, COARSE, {d.mw, d.mh, 0, 0}, {d.cw, d.ch, 0, 0}, 0, d.cw, d.ch);
        barrier(cmd);
        motionValid = false;
        if (estimate && havePrevious) {
            dispatch(cmd, COARSE_MOTION, {d.cw, d.ch, 0, 0}, {d.cbw, d.cbh, 0, 0}, 0, d.cbw, d.cbh);
            vkCmdFillBuffer(cmd, stats.buffer, 0, VK_WHOLE_SIZE, 0);
            barrier(cmd);
            dispatch(cmd, FINE_MOTION, {d.mw, d.mh, d.fbw, d.fbh}, {d.cbw, d.cbh, 0, 0}, 0, d.fbw, d.fbh);
            barrier(cmd);
            motionValid = true;
        }
        havePrevious = true;
    }
    bool canInterpolate() const { return motionValid; }
    /** Write the picture at time t between the previous and newest frame into output(). */
    void recordInterpolate(VkCommandBuffer cmd, float t, bool nearest) {
        prepare(cmd);
        barrier(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[current], 0, nullptr);
        dispatch(cmd, INTERPOLATE, {d.w, d.h, d.s, nearest ? 1 : 0}, {d.fbw, d.fbh, 0, 0}, t, d.w, d.h);
        barrier(cmd);
    }
    VkImage output() const { return out.image; }
    VkImage newest() const { return color[current].image; }
    int width() const { return d.w; }
    int height() const { return d.h; }
    void presentable(VkCommandBuffer cmd, VkImage image) { transition(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); }
    void restore(VkCommandBuffer cmd, VkImage image) { transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL); }

    // Test access: stats buffer is host visible.
    void readStats(uint32_t* errorSum, uint32_t* blocks) const {
        void* data = nullptr;
        if (vkMapMemory(device, stats.memory, 0, 8, 0, &data) != VK_SUCCESS) throw std::runtime_error("Map frame generation stats");
        auto* words = static_cast<const uint32_t*>(data);
        *errorSum = words[0]; *blocks = words[1];
        vkUnmapMemory(device, stats.memory);
    }

private:
    struct Image { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; };
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipelines[PASS_COUNT] = {};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {};
    VkSampler sampler = VK_NULL_HANDLE;
    Image color[2], luma[2], coarse[2], coarseMv, fineMv, out;
    Buffer stats;
    Dims d;
    int current = 0;
    bool imagesReady = false, havePrevious = false, motionValid = false, fresh = false;

    static void checked(VkResult r, const char* what) { if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " (Vulkan " + std::to_string(r) + ")"); }
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; i++) if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("Frame generation memory type unavailable");
    }
    static void barrier(VkCommandBuffer cmd) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    struct Push { int32_t i0[4]; int32_t i1[4]; float f0[4]; };
    void dispatch(VkCommandBuffer cmd, int pass, std::initializer_list<int> i0, std::initializer_list<int> i1, float t, int w, int h) {
        Push push{};
        int k = 0; for (int v : i0) push.i0[k++] = v;
        k = 0; for (int v : i1) push.i1[k++] = v;
        push.f0[0] = t;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, uint32_t(divUp(w, 8)), uint32_t(divUp(h, 8)), 1);
    }
    void prepare(VkCommandBuffer cmd) { if (fresh) { initialiseLayouts(cmd); fresh = false; } }
    void initialiseLayouts(VkCommandBuffer cmd) {
        for (Image* image : {&color[0], &color[1], &luma[0], &luma[1], &coarse[0], &coarse[1], &coarseMv, &fineMv, &out})
            transition(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        // Never-written history must read as black, not as whatever the allocation held.
        VkClearColorValue black{{0, 0, 0, 1}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image* image : {&color[0], &color[1]}) vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        vkCmdFillBuffer(cmd, stats.buffer, 0, VK_WHOLE_SIZE, 0);
        barrier(cmd);
    }
    void createPipelines() {
        VkDescriptorSetLayoutBinding bindings[10]{};
        for (uint32_t i = 0; i < 10; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : i < 9 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 10; setInfo.pBindings = bindings;
        checked(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout), "Frame generation set layout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &range;
        checked(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "Frame generation pipeline layout");
        for (int pass = 0; pass < PASS_COUNT; pass++) {
            size_t bytes = 0;
            const uint32_t* code = passSpirv(pass, &bytes);
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = bytes; moduleInfo.pCode = code;
            VkShaderModule module = VK_NULL_HANDLE;
            checked(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "Frame generation shader");
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = module; info.stage.pName = "main";
            info.layout = layout;
            VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines[pass]);
            vkDestroyShaderModule(device, module, nullptr);
            checked(r, "Frame generation pipeline");
        }
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 14}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2; poolInfo.poolSizeCount = 3; poolInfo.pPoolSizes = sizes;
        checked(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "Frame generation descriptor pool");
        VkDescriptorSetLayout layouts[2] = {setLayout, setLayout};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool; allocate.descriptorSetCount = 2; allocate.pSetLayouts = layouts;
        checked(vkAllocateDescriptorSets(device, &allocate, sets), "Frame generation descriptor sets");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checked(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "Frame generation sampler");
    }
    Image createImage(VkFormat format, int w, int h, VkImageUsageFlags usage) {
        Image image;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {uint32_t(w), uint32_t(h), 1};
        info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        try {
            checked(vkCreateImage(device, &info, nullptr, &image.image), "Frame generation image");
            VkMemoryRequirements r{};
            vkGetImageMemoryRequirements(device, image.image, &r);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = r.size; alloc.memoryTypeIndex = memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            checked(vkAllocateMemory(device, &alloc, nullptr, &image.memory), "Frame generation image memory");
            checked(vkBindImageMemory(device, image.image, image.memory, 0), "Bind frame generation image");
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            checked(vkCreateImageView(device, &view, nullptr, &image.view), "Frame generation image view");
        } catch (...) { dropImage(image); throw; }
        return image;
    }
    void dropImage(Image& image) {
        if (image.view) vkDestroyImageView(device, image.view, nullptr);
        if (image.image) vkDestroyImage(device, image.image, nullptr);
        if (image.memory) vkFreeMemory(device, image.memory, nullptr);
        image = Image{};
    }
    void createImages() {
        const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (int i = 0; i < 2; i++) {
            color[i] = createImage(VK_FORMAT_R8G8B8A8_UNORM, d.w, d.h, colorUsage);
            luma[i] = createImage(VK_FORMAT_R32_SFLOAT, d.mw, d.mh, VK_IMAGE_USAGE_STORAGE_BIT);
            coarse[i] = createImage(VK_FORMAT_R32_SFLOAT, d.cw, d.ch, VK_IMAGE_USAGE_STORAGE_BIT);
        }
        coarseMv = createImage(VK_FORMAT_R32G32B32A32_SFLOAT, d.cbw, d.cbh, VK_IMAGE_USAGE_STORAGE_BIT);
        fineMv = createImage(VK_FORMAT_R32G32B32A32_SFLOAT, d.fbw, d.fbh, VK_IMAGE_USAGE_STORAGE_BIT);
        out = createImage(VK_FORMAT_R8G8B8A8_UNORM, d.w, d.h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 16; bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateBuffer(device, &bufferInfo, nullptr, &stats.buffer), "Frame generation stats");
        VkMemoryRequirements r{};
        vkGetBufferMemoryRequirements(device, stats.buffer, &r);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = r.size; alloc.memoryTypeIndex = memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checked(vkAllocateMemory(device, &alloc, nullptr, &stats.memory), "Frame generation stats memory");
        checked(vkBindBufferMemory(device, stats.buffer, stats.memory, 0), "Bind frame generation stats");
        for (int parity = 0; parity < 2; parity++) {
            int prev = parity ^ 1;
            VkDescriptorImageInfo images[9]{};
            images[0] = {sampler, color[prev].view, VK_IMAGE_LAYOUT_GENERAL};
            images[1] = {sampler, color[parity].view, VK_IMAGE_LAYOUT_GENERAL};
            images[2] = {VK_NULL_HANDLE, luma[prev].view, VK_IMAGE_LAYOUT_GENERAL};
            images[3] = {VK_NULL_HANDLE, luma[parity].view, VK_IMAGE_LAYOUT_GENERAL};
            images[4] = {VK_NULL_HANDLE, coarse[prev].view, VK_IMAGE_LAYOUT_GENERAL};
            images[5] = {VK_NULL_HANDLE, coarse[parity].view, VK_IMAGE_LAYOUT_GENERAL};
            images[6] = {VK_NULL_HANDLE, coarseMv.view, VK_IMAGE_LAYOUT_GENERAL};
            images[7] = {VK_NULL_HANDLE, fineMv.view, VK_IMAGE_LAYOUT_GENERAL};
            images[8] = {VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo buffer{stats.buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet writes[10]{};
            for (uint32_t i = 0; i < 10; i++) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = sets[parity]; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
                writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : i < 9 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                if (i < 9) writes[i].pImageInfo = &images[i]; else writes[i].pBufferInfo = &buffer;
            }
            vkUpdateDescriptorSets(device, 10, writes, 0, nullptr);
        }
        imagesReady = true; fresh = true; havePrevious = false; motionValid = false;
    }
    void dropImages() {
        if (!device) return;
        for (Image* image : {&color[0], &color[1], &luma[0], &luma[1], &coarse[0], &coarse[1], &coarseMv, &fineMv, &out}) dropImage(*image);
        if (stats.buffer) vkDestroyBuffer(device, stats.buffer, nullptr);
        if (stats.memory) vkFreeMemory(device, stats.memory, nullptr);
        stats = Buffer{};
        imagesReady = false; havePrevious = false; motionValid = false;
    }
};

}  // namespace psxfg
