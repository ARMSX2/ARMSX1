// Desktop test for frame generation: synthetic scenes with a known true in-between picture.
//
//   g++ -O2 -std=c++17 -I<ndk>/sysroot/usr/include -Ipsxlab/app/src/main/cpp \
//       psxlab/tools/framegen/framegen_test.cpp C:/Windows/System32/vulkan-1.dll -o framegen_test
//   framegen_test [--cpu-only]
//
// Checks, per scene: the CPU result is much closer to the truth than a plain blend; static
// pictures and scene cuts come back unchanged; the Vulkan compute result matches the CPU one.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include "frame_generation_vk.h"

using psxfg::CpuFrameGen;

static uint32_t hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
static uint32_t rgb(int r, int g, int b) { return 0xff000000u | uint32_t(std::clamp(r, 0, 255)) | uint32_t(std::clamp(g, 0, 255)) << 8 | uint32_t(std::clamp(b, 0, 255)) << 16; }
// Smooth, textured background at world position (x, y): enough detail to match, no flat areas.
static uint32_t background(float x, float y) {
    int r = int(128 + 60 * std::sin(x * 0.11f) + 40 * std::cos(y * 0.07f + x * 0.03f));
    int g = int(110 + 70 * std::sin(y * 0.13f + 1.0f));
    int b = int(90 + 50 * std::cos((x + y) * 0.05f) + 30 * std::sin(x * 0.31f));
    return rgb(r, g, b);
}
static uint32_t sprite(float x, float y) {
    int checker = ((int(std::floor(x / 6)) + int(std::floor(y / 6))) & 1);
    return checker ? rgb(240, 40, 40) : rgb(30, 30, 220);
}
using Scene = std::function<uint32_t(int x, int y, float time)>;  // time 0 = previous, 1 = newest
static std::vector<uint32_t> render(const Scene& scene, int w, int h, float time) {
    std::vector<uint32_t> f(size_t(w) * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) f[size_t(y) * w + x] = scene(x, y, time);
    return f;
}
static double meanError(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int w, int h, int margin) {
    double sum = 0; long n = 0;
    for (int y = margin; y < h - margin; y++)
        for (int x = margin; x < w - margin; x++) {
            uint32_t p = a[size_t(y) * w + x], q = b[size_t(y) * w + x];
            for (int k = 0; k < 3; k++) sum += std::abs(int((p >> (8 * k)) & 255) - int((q >> (8 * k)) & 255));
            n += 3;
        }
    return sum / n;
}
static std::vector<uint32_t> blend(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, float t) {
    std::vector<uint32_t> out(a.size());
    for (size_t i = 0; i < a.size(); i++) {
        uint32_t p = 0xff000000u;
        for (int k = 0; k < 3; k++) { float v = ((a[i] >> (8 * k)) & 255) * (1 - t) + ((b[i] >> (8 * k)) & 255) * t; p |= uint32_t(std::lround(v)) << (8 * k); }
        out[i] = p;
    }
    return out;
}

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice physical = VK_NULL_HANDLE; VkDevice device = VK_NULL_HANDLE; VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    std::string name;
    psxfg::VkFrameGen gen;
    static void ok(VkResult r, const char* what) { if (r != VK_SUCCESS) { std::fprintf(stderr, "%s failed: %d\n", what, r); std::exit(2); } }
    bool open() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return false;
        uint32_t count = 0; vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance, &count, devices.data());
        uint32_t family = 0;
        for (auto p : devices) {
            uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(p, &n, nullptr);
            std::vector<VkQueueFamilyProperties> f(n); vkGetPhysicalDeviceQueueFamilyProperties(p, &n, f.data());
            for (uint32_t i = 0; i < n; i++) if ((f[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (f[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { physical = p; family = i; break; }
            if (physical) break;
        }
        if (!physical) return false;
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical, &props); name = props.deviceName;
        float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex = family; q.queueCount = 1; q.pQueuePriorities = &priority;
        VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; d.queueCreateInfoCount = 1; d.pQueueCreateInfos = &q;
        ok(vkCreateDevice(physical, &d, nullptr, &device), "device");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; p.queueFamilyIndex = family; p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ok(vkCreateCommandPool(device, &p, nullptr, &pool), "pool");
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; a.commandPool = pool; a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount = 1;
        ok(vkAllocateCommandBuffers(device, &a, &cmd), "command buffer");
        VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; ok(vkCreateFence(device, &f, nullptr, &fence), "fence");
        gen.attach(physical, device);
        return true;
    }
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; i++) if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::exit(3);
    }
    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory bufferMemory = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize bufferSize = 0;
    VkImage input = VK_NULL_HANDLE; VkDeviceMemory inputMemory = VK_NULL_HANDLE; int inputW = 0, inputH = 0;
    void ensure(int w, int h) {
        VkDeviceSize need = VkDeviceSize(w) * h * 4;
        if (need > bufferSize) {
            if (buffer) { vkUnmapMemory(device, bufferMemory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, bufferMemory, nullptr); }
            VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; b.size = need; b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            ok(vkCreateBuffer(device, &b, nullptr, &buffer), "buffer");
            VkMemoryRequirements r{}; vkGetBufferMemoryRequirements(device, buffer, &r);
            VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; m.allocationSize = r.size; m.memoryTypeIndex = memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ok(vkAllocateMemory(device, &m, nullptr, &bufferMemory), "buffer memory"); vkBindBufferMemory(device, buffer, bufferMemory, 0);
            vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped); bufferSize = need;
        }
        if (w != inputW || h != inputH) {
            if (input) { vkDestroyImage(device, input, nullptr); vkFreeMemory(device, inputMemory, nullptr); }
            VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; i.imageType = VK_IMAGE_TYPE_2D; i.format = VK_FORMAT_R8G8B8A8_UNORM; i.extent = {uint32_t(w), uint32_t(h), 1};
            i.mipLevels = i.arrayLayers = 1; i.samples = VK_SAMPLE_COUNT_1_BIT; i.tiling = VK_IMAGE_TILING_OPTIMAL; i.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ok(vkCreateImage(device, &i, nullptr, &input), "input");
            VkMemoryRequirements r{}; vkGetImageMemoryRequirements(device, input, &r);
            VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; m.allocationSize = r.size; m.memoryTypeIndex = memoryType(r.memoryTypeBits, 0);
            ok(vkAllocateMemory(device, &m, nullptr, &inputMemory), "input memory"); vkBindImageMemory(device, input, inputMemory, 0);
            inputW = w; inputH = h;
        }
    }
    void begin() { vkResetFences(device, 1, &fence); vkResetCommandBuffer(cmd, 0); VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cmd, &b); }
    void end() { vkEndCommandBuffer(cmd); VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd; ok(vkQueueSubmit(queue, 1, &s, fence), "submit"); ok(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait"); }
    void barrier(VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void capture(const std::vector<uint32_t>& frame, int w, int h, bool estimate) {
        ensure(w, h); gen.resize(w, h);
        std::memcpy(mapped, frame.data(), frame.size() * 4);
        begin();
        barrier(input, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {uint32_t(w), uint32_t(h), 1};
        vkCmdCopyBufferToImage(cmd, buffer, input, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(input, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        gen.recordCapture(cmd, input, estimate);
        end();
    }
    std::vector<uint32_t> interpolate(float t, bool nearest, double* milliseconds) {
        int w = gen.width(), h = gen.height();
        begin();
        gen.recordInterpolate(cmd, t, nearest);
        gen.presentable(cmd, gen.output());
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {uint32_t(w), uint32_t(h), 1};
        vkCmdCopyImageToBuffer(cmd, gen.output(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        gen.restore(cmd, gen.output());
        auto start = std::chrono::steady_clock::now();
        end();
        if (milliseconds) *milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::vector<uint32_t> out(size_t(w) * h);
        std::memcpy(out.data(), mapped, out.size() * 4);
        return out;
    }
};

static int failures = 0;
static void expect(bool condition, const std::string& what) { std::printf("  %s %s\n", condition ? "ok  " : "FAIL", what.c_str()); if (!condition) failures++; }

int main(int argc, char** argv) {
    bool cpuOnly = argc > 1 && std::string(argv[1]) == "--cpu-only";
    Gpu gpu;
    bool haveGpu = !cpuOnly && gpu.open();
    std::printf("GPU: %s\n", haveGpu ? gpu.name.c_str() : "none (CPU only)");
    struct Case { const char* name; int w, h; Scene scene; bool cut; bool still; };
    std::vector<Case> cases;
    // A sprite moving right/down over a background panning left: two motions and occlusion edges.
    auto moving = [](float scale) {
        return Scene([scale](int x, int y, float time) {
            float px = x / scale, py = y / scale;
            float sx = 100 + 14 * time, sy = 80 + 6 * time;
            if (px >= sx && px < sx + 60 && py >= sy && py < sy + 48) return sprite(px - sx, py - sy);
            return background(px + 10 * time, py + 3 * time);
        });
    };
    cases.push_back({"sprite + pan 320x240", 320, 240, moving(1), false, false});
    cases.push_back({"sprite + pan 640x480 (2x)", 640, 480, moving(2), false, false});
    cases.push_back({"sprite + pan 368x240", 368, 240, moving(1), false, false});
    cases.push_back({"fast pan 320x240 (26 px/frame)", 320, 240, Scene([](int x, int y, float time) { return background(x + 26 * time, y - 8 * time); }), false, false});
    cases.push_back({"still picture", 320, 240, Scene([](int x, int y, float) { return background(x, y); }), false, true});
    cases.push_back({"scene cut", 320, 240, Scene([](int x, int y, float time) { return time < 0.5f ? background(x, y) : rgb(hash(x * 7 + y * 131) & 255, hash(x + y * 977) & 255, hash(x * 31 + y) & 255); }), true, false});
    for (const auto& c : cases) {
        std::printf("%s\n", c.name);
        auto prev = render(c.scene, c.w, c.h, 0), curr = render(c.scene, c.w, c.h, 1);
        CpuFrameGen cpu, cpu4;
        cpu4.threads = 4;
        cpu4.capture(prev.data(), c.w, c.h, c.w, false);
        auto threadedStart = std::chrono::steady_clock::now();
        cpu4.capture(curr.data(), c.w, c.h, c.w, true);
        double threadedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - threadedStart).count();
        cpu.capture(prev.data(), c.w, c.h, c.w, false);
        auto captureStart = std::chrono::steady_clock::now();
        cpu.capture(curr.data(), c.w, c.h, c.w, true);
        double captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count();
        expect(cpu.canInterpolate(), "motion estimated (cpu capture + motion " + std::to_string(captureMs) + " ms, 4 threads " + std::to_string(threadedMs) + " ms)");
        bool same = cpu4.errorSum == cpu.errorSum && cpu4.fineMv.size() == cpu.fineMv.size();
        for (size_t i = 0; same && i < cpu.fineMv.size(); i++) same = cpu4.fineMv[i].x == cpu.fineMv[i].x && cpu4.fineMv[i].y == cpu.fineMv[i].y;
        expect(same, "threaded motion search identical");
        if (haveGpu) { gpu.gen.reset(); gpu.capture(prev, c.w, c.h, false); gpu.capture(curr, c.w, c.h, true); }
        for (float t : {0.5f, 0.25f, 0.75f}) {
            auto truth = render(c.scene, c.w, c.h, t);
            std::vector<uint32_t> out(size_t(c.w) * c.h);
            auto start = std::chrono::steady_clock::now();
            cpu.interpolate(t, false, out.data(), c.w);
            double cpuMs1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::vector<uint32_t> threaded(out.size());
            start = std::chrono::steady_clock::now();
            CpuFrameGen many = cpu; many.threads = 4;
            many.interpolate(t, false, threaded.data(), c.w);
            expect(threaded == out, "threaded cpu result identical (1 thread " + std::to_string(cpuMs1) + " ms)");
            double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            int margin = 12 * c.h / 240;
            double gen = meanError(out, truth, c.w, c.h, margin), mixed = meanError(blend(prev, curr, t), truth, c.w, c.h, margin);
            char line[256];
            std::snprintf(line, sizeof line, "t=%.2f cpu error %.2f vs blend %.2f (cpu 4 threads %.1f ms)", t, gen, mixed, cpuMs);
            if (c.still) expect(meanError(out, curr, c.w, c.h, 0) < 0.01, std::string(line) + ": still picture unchanged");
            else if (c.cut) expect(meanError(out, t < 0.5f ? prev : curr, c.w, c.h, 0) == 0.0, std::string(line) + ": cut shows the nearer frame");
            else expect(gen < mixed * 0.5 && gen < 6.0, std::string(line) + ": much closer to the truth than blending");
            if (haveGpu) {
                double ms = 0;
                auto g = gpu.interpolate(t, false, &ms);
                double diff = meanError(g, out, c.w, c.h, 0);
                long far = 0;
                for (size_t i = 0; i < g.size(); i++) for (int k = 0; k < 3; k++) if (std::abs(int((g[i] >> (8 * k)) & 255) - int((out[i] >> (8 * k)) & 255)) > 8) { far++; break; }
                std::snprintf(line, sizeof line, "t=%.2f vulkan vs cpu: mean %.3f, %.3f%% pixels off by >8, gpu %.2f ms incl. readback", t, diff, 100.0 * far / g.size(), ms);
                expect(diff < 1.0 && far * 1000 < long(g.size()) * 5, line);
            }
        }
        if (haveGpu) {
            uint32_t errorSum = 0, blocks = 0; gpu.gen.readStats(&errorSum, &blocks);
            expect(blocks == cpu.blocks && std::abs(int64_t(errorSum) - int64_t(cpu.errorSum)) <= int64_t(blocks), "vulkan block statistics match cpu (" + std::to_string(errorSum) + "/" + std::to_string(blocks) + " vs " + std::to_string(cpu.errorSum) + "/" + std::to_string(cpu.blocks) + ")");
        }
    }
    // Nearest sampling keeps pixel art crisp: output colours must all exist in an input frame
    // wherever the two frames agree.
    {
        std::printf("nearest sampling\n");
        auto scene = cases[0].scene;
        auto prev = render(scene, 320, 240, 0), curr = render(scene, 320, 240, 1);
        CpuFrameGen cpu; cpu.capture(prev.data(), 320, 240, 320, false); cpu.capture(curr.data(), 320, 240, 320, true);
        std::vector<uint32_t> out(320 * 240); cpu.interpolate(0.5f, true, out.data(), 320);
        auto truth = render(scene, 320, 240, 0.5f);
        double gen = meanError(out, truth, 320, 240, 12), mixed = meanError(blend(prev, curr, 0.5f), truth, 320, 240, 12);
        expect(gen < mixed * 0.5, "nearest: error " + std::to_string(gen) + " vs blend " + std::to_string(mixed));
        if (haveGpu) {
            gpu.gen.reset(); gpu.capture(prev, 320, 240, false); gpu.capture(curr, 320, 240, true);
            auto g = gpu.interpolate(0.5f, true, nullptr);
            expect(meanError(g, out, 320, 240, 0) < 0.5, "nearest: vulkan matches cpu (" + std::to_string(meanError(g, out, 320, 240, 0)) + ")");
        }
    }
    // A size change must drop the history rather than interpolate across sizes.
    {
        std::printf("size change\n");
        auto a = render(cases[0].scene, 320, 240, 0), b = render(cases[1].scene, 640, 480, 1);
        CpuFrameGen cpu; cpu.capture(a.data(), 320, 240, 320, false); cpu.capture(b.data(), 640, 480, 640, true);
        expect(!cpu.canInterpolate(), "cpu refuses to interpolate across a size change");
        if (haveGpu) { gpu.gen.reset(); gpu.capture(a, 320, 240, false); gpu.capture(b, 640, 480, true); expect(!gpu.gen.canInterpolate(), "vulkan refuses to interpolate across a size change"); }
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
