// Minimal headless Vulkan harness for fuse_ui_vk gates: instance, first device with a compute queue, an RGBA8
// storage image, upload / readback. Returns false (skip) when no device is available.
#pragma once

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" // partial designated Vk*CreateInfo initialisers
#endif

#include "fuse/ui/draw_list.hpp"

#include <vulkan/vulkan.h>
#if defined(FUSE_HAS_VULKAN_RHI)
// In-tree with fuse_rhi every vk* name is a volk pointer: register raw instances / devices so volk loads them.
#include <fuse/renderer/vk/loader.hpp>
#define FUSE_UI_TEST_VOLK(x) x
#else
#define FUSE_UI_TEST_VOLK(x)
#endif

#include <cstdio>
#include <cstring>
#include <vector>

namespace fuse::ui::test {

struct VkHeadless {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingPtr = nullptr;
    u32 w = 0, h = 0;
    char deviceName[256] = {};

    u32 memType(u32 bits, VkMemoryPropertyFlags want) const {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (u32 i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return ~0u;
    }

    bool init(u32 width, u32 height) {
        w = width;
        h = height;
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return false;
        FUSE_UI_TEST_VOLK(fuse::renderer::vkloader::registerInstance(instance));
        u32 n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (!n) return false;
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());
        for (VkPhysicalDevice d : devs) {
            u32 qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
            for (u32 q = 0; q < qn; ++q)
                if (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    phys = d;
                    family = q;
                    break;
                }
            if (phys) break;
        }
        if (!phys) return false;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys, &props);
        std::snprintf(deviceName, sizeof deviceName, "%s", props.deviceName);
        const float prio = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return false;
        FUSE_UI_TEST_VOLK(fuse::renderer::vkloader::registerDevice(device, instance));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        vkCreateCommandPool(device, &pci, nullptr, &pool);

        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {w, h, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(device, &ii, nullptr, &image) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == ~0u) mai.memoryTypeIndex = memType(req.memoryTypeBits, 0);
        if (vkAllocateMemory(device, &mai, nullptr, &imageMem) != VK_SUCCESS) return false;
        vkBindImageMemory(device, image, imageMem, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vci, nullptr, &view) != VK_SUCCESS) return false;

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(w) * h * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device, &bci, nullptr, &staging);
        vkGetBufferMemoryRequirements(device, staging, &req);
        mai.allocationSize = req.size;
        mai.memoryTypeIndex =
            memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device, &mai, nullptr, &stagingMem) != VK_SUCCESS) return false;
        vkBindBufferMemory(device, staging, stagingMem, 0);
        vkMapMemory(device, stagingMem, 0, VK_WHOLE_SIZE, 0, &stagingPtr);
        return true;
    }

    /// Uploads `init` into the image, runs `fn(cmd)` with the image in GENERAL, reads the result into `out`.
    template <class F>
    void run(const Image& init, Image& out, F&& fn) {
        std::memcpy(stagingPtr, init.pixels.data(), std::size_t(w) * h * 4);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device, &cai, &cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                           VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = src;
            b.dstAccessMask = dst;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, s0, s1, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        fn(cmd);
        barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        out.resize(int(w), int(h));
        std::memcpy(out.pixels.data(), stagingPtr, std::size_t(w) * h * 4);
    }

    ~VkHeadless() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (staging) vkDestroyBuffer(device, staging, nullptr);
            if (stagingMem) vkFreeMemory(device, stagingMem, nullptr);
            if (view) vkDestroyImageView(device, view, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (imageMem) vkFreeMemory(device, imageMem, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            FUSE_UI_TEST_VOLK(fuse::renderer::vkloader::unregisterDevice(device));
            vkDestroyDevice(device, nullptr);
        }
        if (instance) {
            FUSE_UI_TEST_VOLK(fuse::renderer::vkloader::unregisterInstance(instance));
            vkDestroyInstance(instance, nullptr);
        }
    }
};

} // namespace fuse::ui::test
