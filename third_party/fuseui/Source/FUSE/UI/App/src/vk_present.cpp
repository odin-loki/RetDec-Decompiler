// Vulkan swapchain presenter for fuse_app native windows (see native.hpp).
//
// Per window: VkSurfaceKHR + swapchain (TRANSFER_DST), one RGBA8 "canvas" image (STORAGE | TRANSFER_SRC | TRANSFER_DST)
// kept in GENERAL layout, two frames in flight (command buffer, fence, acquire semaphore, persistently mapped upload
// buffer), one render-done semaphore per swapchain image. A frame: wait the slot fence, acquire, then either
//   GpuDraw:   clear canvas, fuse_ui_vk::record (the "ui.draw_list" kernel over the packed draw list), or
//   CpuUpload: CPU-raster the damaged rect, copy those rows into the canvas,
// then blit canvas -> swapchain image (format conversion RGBA -> BGRA included) and present. Out-of-date / suboptimal
// swapchains and size changes recreate the swapchain + canvas and force a full redraw.
#include "native_internal.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#else
// vulkan_xlib.h needs only these two Xlib types; the X11 headers are not required to build.
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>
#endif

#if defined(FUSE_HAS_VULKAN_RHI)
// In-tree with fuse_rhi every vk* name is a volk function pointer: raw instances / devices must be registered so
// volk loads their entry points (fuse/renderer/vk/loader.hpp).
#include <fuse/renderer/vk/loader.hpp>
#define FUSE_APP_VOLK(x) x
#else
#define FUSE_APP_VOLK(x)
#endif

#if defined(FUSE_APP_HAS_UI_VK)
#include "fuse/ui/render/gpu_draw_data.hpp"
#include "fuse/ui/render/ui_vk.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace fuse::app::detail {
using namespace fuse::ui;

struct VkCtx {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 family = ~0u;
    VkCommandPool pool = VK_NULL_HANDLE;
    SurfaceSource::Kind kind = SurfaceSource::Xlib;
    char name[256] = {};
    u32 windows = 0;
    bool adopted = false; ///< instance + device belong to the caller (NativeOptions::adoptDevice)
};

namespace {
constexpr u32 kFrames = 2;

/// WSI entry points resolved through vkGetInstanceProcAddr: import libraries / loaders do not all export the KHR
/// surface and swapchain functions (a MinGW vulkan-1 import library without vkCreateWin32SurfaceKHR crashed under Wine).
struct Khr {
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
#if defined(_WIN32)
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
#endif
#if !defined(_WIN32)
    PFN_vkCreateXlibSurfaceKHR CreateXlibSurfaceKHR = nullptr;
#endif
    bool load(VkInstance inst) {
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(vkGetInstanceProcAddr(inst, name));
            all = all && fn;
        };
        get(DestroySurfaceKHR, "vkDestroySurfaceKHR");
        get(GetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR");
        get(GetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        get(GetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        get(GetPhysicalDeviceSurfacePresentModesKHR, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        get(CreateSwapchainKHR, "vkCreateSwapchainKHR");
        get(DestroySwapchainKHR, "vkDestroySwapchainKHR");
        get(GetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
        get(AcquireNextImageKHR, "vkAcquireNextImageKHR");
        get(QueuePresentKHR, "vkQueuePresentKHR");
#if defined(_WIN32)
        get(CreateWin32SurfaceKHR, "vkCreateWin32SurfaceKHR");
#endif
#if !defined(_WIN32)
        get(CreateXlibSurfaceKHR, "vkCreateXlibSurfaceKHR");
#endif
        return all;
    }
};
Khr K; // one Vulkan instance per process (VkCtx); its WSI functions


bool ok(VkResult r, const char* what, std::string& err) {
    if (r == VK_SUCCESS) return true;
    err = std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")";
    return false;
}
u32 memType(VkPhysicalDevice p, u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
VkImageMemoryBarrier imageBarrier(VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
} // namespace

VkCtx* vkCtxCreate(SurfaceSource::Kind kind, std::string& err) {
    auto* c = new VkCtx{};
    c->kind = kind;
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
                          VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#else
                          VK_KHR_XLIB_SURFACE_EXTENSION_NAME
#endif
    };
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "fuse_app";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    if (!ok(vkCreateInstance(&ici, nullptr, &c->instance), "vkCreateInstance (surface extensions)", err)) {
        delete c;
        return nullptr;
    }
    FUSE_APP_VOLK(fuse::renderer::vkloader::registerInstance(c->instance));
    if (!K.load(c->instance)) {
        err = "the Vulkan loader lacks the surface / swapchain entry points";
        FUSE_APP_VOLK(fuse::renderer::vkloader::unregisterInstance(c->instance));
        vkDestroyInstance(c->instance, nullptr);
        delete c;
        return nullptr;
    }
    return c;
}

VkCtx* vkCtxAdopt(SurfaceSource::Kind kind, const VulkanDeviceHandles& h, std::string& err) {
    if (!h.instance || !h.physicalDevice || !h.device || !h.queue) {
        err = "adoptDevice needs instance, physical device, device and queue";
        return nullptr;
    }
    auto* c = new VkCtx{};
    c->kind = kind;
    c->adopted = true;
    c->instance = static_cast<VkInstance>(h.instance);
    c->phys = static_cast<VkPhysicalDevice>(h.physicalDevice);
    c->device = static_cast<VkDevice>(h.device);
    c->queue = static_cast<VkQueue>(h.queue);
    c->family = h.queueFamily;
    // the owner registered both with the loader (fuse_rhi's VulkanInstance / VulkanDevice); entry points are loaded
    if (!K.load(c->instance)) {
        err = "the adopted instance lacks the surface / swapchain entry points";
        delete c;
        return nullptr;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(c->phys, &props);
    std::snprintf(c->name, sizeof c->name, "%s", props.deviceName);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c->family;
    if (!ok(vkCreateCommandPool(c->device, &pci, nullptr, &c->pool), "vkCreateCommandPool", err)) {
        delete c;
        return nullptr;
    }
    return c;
}

void vkCtxDestroy(VkCtx* c) {
    if (!c) return;
    if (c->adopted) {
        if (c->device) {
            vkDeviceWaitIdle(c->device);
            if (c->pool) vkDestroyCommandPool(c->device, c->pool, nullptr);
        }
        delete c;
        return;
    }
    if (c->device) {
        vkDeviceWaitIdle(c->device);
        if (c->pool) vkDestroyCommandPool(c->device, c->pool, nullptr);
        FUSE_APP_VOLK(fuse::renderer::vkloader::unregisterDevice(c->device));
        vkDestroyDevice(c->device, nullptr);
    }
    if (c->instance) {
        FUSE_APP_VOLK(fuse::renderer::vkloader::unregisterInstance(c->instance));
        vkDestroyInstance(c->instance, nullptr);
    }
    delete c;
}
const char* vkCtxDeviceName(const VkCtx* c) { return c ? c->name : ""; }

namespace {
/// First device + queue family with graphics or compute that can present to `surface` and has VK_KHR_swapchain.
bool pickDevice(VkCtx& c, VkSurfaceKHR surface, std::string& err) {
    u32 n = 0;
    vkEnumeratePhysicalDevices(c.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(c.instance, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        u32 en = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        if (en) vkEnumerateDeviceExtensionProperties(d, nullptr, &en, ep.data());
        if (std::none_of(ep.begin(), ep.end(), [](const VkExtensionProperties& e) {
                return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            }))
            continue;
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (u32 q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            K.GetPhysicalDeviceSurfaceSupportKHR(d, q, surface, &present);
            if (present && (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                c.phys = d;
                c.family = q;
                break;
            }
        }
        if (c.phys) break;
    }
    if (!c.phys) {
        err = "no Vulkan device can present to this window";
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(c.phys, &props);
    std::snprintf(c.name, sizeof c.name, "%s", props.deviceName);
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = c.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    if (!ok(vkCreateDevice(c.phys, &dci, nullptr, &c.device), "vkCreateDevice", err)) return false;
    FUSE_APP_VOLK(fuse::renderer::vkloader::registerDevice(c.device, c.instance));
    vkGetDeviceQueue(c.device, c.family, 0, &c.queue);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c.family;
    return ok(vkCreateCommandPool(c.device, &pci, nullptr, &c.pool), "vkCreateCommandPool", err);
}
} // namespace

struct VkWin {
    VkCtx* ctx = nullptr;
    NativeOptions opts;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> renderDone; ///< per swapchain image
    // canvas
    VkImage canvas = VK_NULL_HANDLE;
    VkDeviceMemory canvasMem = VK_NULL_HANDLE;
    VkImageView canvasView = VK_NULL_HANDLE;
    bool canvasFresh = true; ///< UNDEFINED layout, contents invalid
    // frames
    VkCommandBuffer cmd[kFrames]{};
    VkFence fence[kFrames]{};
    VkSemaphore acquired[kFrames]{};
    VkBuffer upload[kFrames]{};
    VkDeviceMemory uploadMem[kFrames]{};
    u8* uploadPtr[kFrames]{};
    VkDeviceSize uploadSize = 0;
    u32 frame = 0;
    bool needFull = true;
    bool gpuDraw = false;
    ExternalTexture external[4]{};
#if defined(FUSE_APP_HAS_UI_VK)
    fuse::ui::render::UiVkRenderer gpu;
    fuse::ui::render::GpuDrawData data;
#endif

    void destroySwapchain() {
        VkDevice d = ctx->device;
        vkDeviceWaitIdle(d);
        for (VkSemaphore s : renderDone) vkDestroySemaphore(d, s, nullptr);
        renderDone.clear();
        images.clear();
        if (canvasView) vkDestroyImageView(d, canvasView, nullptr);
        if (canvas) vkDestroyImage(d, canvas, nullptr);
        if (canvasMem) vkFreeMemory(d, canvasMem, nullptr);
        canvasView = VK_NULL_HANDLE;
        canvas = VK_NULL_HANDLE;
        canvasMem = VK_NULL_HANDLE;
        for (u32 i = 0; i < kFrames; ++i) {
            if (upload[i]) vkDestroyBuffer(d, upload[i], nullptr);
            if (uploadMem[i]) vkFreeMemory(d, uploadMem[i], nullptr);
            upload[i] = VK_NULL_HANDLE;
            uploadMem[i] = VK_NULL_HANDLE;
            uploadPtr[i] = nullptr;
        }
        uploadSize = 0;
    }

    bool createSwapchain(u32 wantW, u32 wantH, std::string& err) {
        VkDevice d = ctx->device;
        VkSwapchainKHR old = swapchain;
        if (old) destroySwapchain();
        VkSurfaceCapabilitiesKHR caps;
        if (!ok(K.GetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phys, surface, &caps), "surface capabilities", err))
            return false;
        extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{wantW, wantH};
        extent.width = std::clamp(extent.width, std::max(1u, caps.minImageExtent.width), std::max(1u, caps.maxImageExtent.width));
        extent.height = std::clamp(extent.height, std::max(1u, caps.minImageExtent.height), std::max(1u, caps.maxImageExtent.height));
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            err = "swapchain images cannot be blit targets";
            return false;
        }
        u32 fn = 0;
        K.GetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, surface, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn);
        if (fn) K.GetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, surface, &fn, fmts.data());
        if (fmts.empty()) {
            err = "surface has no formats";
            return false;
        }
        VkSurfaceFormatKHR sf = fmts[0];
        for (const VkSurfaceFormatKHR& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
                sf = f;
                break;
            }
        format = sf.format;
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!opts.vsync) {
            u32 pn = 0;
            K.GetPhysicalDeviceSurfacePresentModesKHR(ctx->phys, surface, &pn, nullptr);
            std::vector<VkPresentModeKHR> pms(pn);
            if (pn) K.GetPhysicalDeviceSurfacePresentModesKHR(ctx->phys, surface, &pn, pms.data());
            for (VkPresentModeKHR m : pms)
                if (m == VK_PRESENT_MODE_MAILBOX_KHR || (m == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR))
                    mode = m;
        }
        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = surface;
        sci.minImageCount = std::max(caps.minImageCount, 2u);
        if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
        sci.imageFormat = sf.format;
        sci.imageColorSpace = sf.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                              : caps.currentTransform;
        sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                 ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                 : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        sci.presentMode = mode;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = old;
        VkResult r = K.CreateSwapchainKHR(d, &sci, nullptr, &swapchain);
        if (old) K.DestroySwapchainKHR(d, old, nullptr);
        if (!ok(r, "vkCreateSwapchainKHR", err)) {
            swapchain = VK_NULL_HANDLE;
            return false;
        }
        u32 n = 0;
        K.GetSwapchainImagesKHR(d, swapchain, &n, nullptr);
        images.resize(n);
        K.GetSwapchainImagesKHR(d, swapchain, &n, images.data());
        renderDone.resize(n);
        VkSemaphoreCreateInfo semi{};
        semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkSemaphore& s : renderDone)
            if (!ok(vkCreateSemaphore(d, &semi, nullptr, &s), "vkCreateSemaphore", err)) return false;
        // canvas: RGBA8, the kernel's image format and the CPU raster's byte order
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {extent.width, extent.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (gpuDraw ? VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT) : VkImageUsageFlags(0));
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!ok(vkCreateImage(d, &ii, nullptr, &canvas), "vkCreateImage canvas", err)) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(d, canvas, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = memType(ctx->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == ~0u) mai.memoryTypeIndex = memType(ctx->phys, mr.memoryTypeBits, 0);
        if (!ok(vkAllocateMemory(d, &mai, nullptr, &canvasMem), "canvas memory", err)) return false;
        vkBindImageMemory(d, canvas, canvasMem, 0);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = canvas;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!ok(vkCreateImageView(d, &vi, nullptr, &canvasView), "canvas view", err)) return false;
        canvasFresh = true;
        if (!gpuDraw) {
            uploadSize = VkDeviceSize(extent.width) * extent.height * 4;
            for (u32 i = 0; i < kFrames; ++i) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = uploadSize;
                bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                if (!ok(vkCreateBuffer(d, &bi, nullptr, &upload[i]), "upload buffer", err)) return false;
                VkMemoryRequirements br;
                vkGetBufferMemoryRequirements(d, upload[i], &br);
                VkMemoryAllocateInfo bai{};
                bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                bai.allocationSize = br.size;
                bai.memoryTypeIndex = memType(ctx->phys, br.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (!ok(vkAllocateMemory(d, &bai, nullptr, &uploadMem[i]), "upload memory", err)) return false;
                vkBindBufferMemory(d, upload[i], uploadMem[i], 0);
                void* p = nullptr;
                vkMapMemory(d, uploadMem[i], 0, uploadSize, 0, &p);
                uploadPtr[i] = static_cast<u8*>(p);
            }
        }
        needFull = true;
        return true;
    }
};

VkWin* vkWinCreate(VkCtx* c, const SurfaceSource& s, const NativeOptions& o, std::string& err) {
    if (!c) return nullptr;
    auto* w = new VkWin{};
    w->ctx = c;
    w->opts = o;
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR si{};
    si.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    si.hinstance = static_cast<HINSTANCE>(s.hinstance);
    si.hwnd = static_cast<HWND>(s.hwnd);
    const VkResult sr = K.CreateWin32SurfaceKHR(c->instance, &si, nullptr, &w->surface);
#else
    VkXlibSurfaceCreateInfoKHR si{};
    si.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    si.dpy = static_cast<Display*>(s.display);
    si.window = s.xwindow;
    const VkResult sr = K.CreateXlibSurfaceKHR(c->instance, &si, nullptr, &w->surface);
#endif
    if (!ok(sr, "vkCreate*SurfaceKHR", err)) {
        delete w;
        return nullptr;
    }
    if (!c->device) {
        if (!pickDevice(*c, w->surface, err)) {
            K.DestroySurfaceKHR(c->instance, w->surface, nullptr);
            delete w;
            return nullptr;
        }
    } else {
        VkBool32 present = VK_FALSE;
        K.GetPhysicalDeviceSurfaceSupportKHR(c->phys, c->family, w->surface, &present);
        if (!present) {
            err = "the shared Vulkan queue cannot present to this window";
            K.DestroySurfaceKHR(c->instance, w->surface, nullptr);
            delete w;
            return nullptr;
        }
    }
    ++c->windows;
#if defined(FUSE_APP_HAS_UI_VK)
    if (o.drawMode != VkDrawMode::CpuUpload && fuse::ui::render::UiVkRenderer::kernelAvailable()) {
        fuse::ui::render::UiVkDesc gd;
        gd.physicalDevice = c->phys;
        gd.device = c->device;
        gd.framesInFlight = kFrames;
        w->gpuDraw = w->gpu.init(gd);
    }
#endif
    if (o.drawMode == VkDrawMode::GpuDraw && !w->gpuDraw) err = "GPU draw requested but the ui.draw_list kernel is unavailable; using CPU upload";
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = c->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFrames;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    bool good = ok(vkAllocateCommandBuffers(c->device, &cai, w->cmd), "command buffers", err);
    for (u32 i = 0; good && i < kFrames; ++i)
        good = ok(vkCreateFence(c->device, &fi, nullptr, &w->fence[i]), "fence", err) &&
               ok(vkCreateSemaphore(c->device, &semi, nullptr, &w->acquired[i]), "semaphore", err);
    if (!good) {
        vkWinDestroy(w);
        return nullptr;
    }
    return w;
}

bool vkWinGpuDraws(const VkWin* w) { return w && w->gpuDraw; }

bool vkWinSetExternal(VkWin* w, unsigned slot, const ExternalTexture& t) {
    if (!w || !w->gpuDraw || slot >= 4) return false;
    w->external[slot] = t;
    return true;
}

void vkWinDestroy(VkWin* w) {
    if (!w) return;
    VkCtx* c = w->ctx;
    if (c->device) {
        vkDeviceWaitIdle(c->device);
        w->destroySwapchain();
        if (w->swapchain) K.DestroySwapchainKHR(c->device, w->swapchain, nullptr);
        for (u32 i = 0; i < kFrames; ++i) {
            if (w->fence[i]) vkDestroyFence(c->device, w->fence[i], nullptr);
            if (w->acquired[i]) vkDestroySemaphore(c->device, w->acquired[i], nullptr);
        }
        if (w->cmd[0]) vkFreeCommandBuffers(c->device, c->pool, kFrames, w->cmd);
#if defined(FUSE_APP_HAS_UI_VK)
        w->gpu.destroy();
#endif
    }
    if (w->surface) K.DestroySurfaceKHR(c->instance, w->surface, nullptr);
    if (c->windows) --c->windows;
    delete w;
}

bool vkWinPresent(VkWin* w, AppWindow& win, Rect damage) {
    VkCtx& c = *w->ctx;
    const Size vp = win.size();
    const u32 vw = u32(std::max(1.f, std::round(vp.w))), vh = u32(std::max(1.f, std::round(vp.h)));
    std::string err;
    if (!w->swapchain || w->extent.width != vw || w->extent.height != vh) {
        if (!w->createSwapchain(vw, vh, err)) {
            std::fprintf(stderr, "fuse_app: %s\n", err.c_str());
            return false;
        }
        // the surface decides the size (X11 / Win32 report the client area): follow it
        if (w->extent.width != vw || w->extent.height != vh) {
            win.resize(w->extent.width, w->extent.height); // redraws at the new size next iteration
            return false;
        }
    }
    const u32 f = w->frame;
    vkWaitForFences(c.device, 1, &w->fence[f], VK_TRUE, UINT64_MAX);
    u32 index = 0;
    VkResult r = K.AcquireNextImageKHR(c.device, w->swapchain, UINT64_MAX, w->acquired[f], VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        w->createSwapchain(vw, vh, err);
        win.requestRedraw();
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    vkResetFences(c.device, 1, &w->fence[f]);

    const u32 W = w->extent.width, H = w->extent.height;
    const bool full = w->needFull || w->canvasFresh || damage.empty();
    Rect dmg = full ? Rect{0.f, 0.f, f32(W), f32(H)} : damage;
    int x0 = std::max(0, int(std::floor(dmg.x))), y0 = std::max(0, int(std::floor(dmg.y)));
    int x1 = std::min(int(W), int(std::ceil(dmg.x + dmg.w))), y1 = std::min(int(H), int(std::ceil(dmg.y + dmg.h)));
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = int(W); y1 = int(H); }

#if defined(FUSE_APP_HAS_UI_VK)
    if (w->gpuDraw) {
        UiContext& ui = win.ui();
        const CpuRasterizer& raster = win.rasterizer();
        fuse::ui::render::packDrawList(ui.drawList(), W, H, w->data, ui.atlas().pixels(), u32(ui.atlas().size()),
                                       raster.images(), raster.imageCount());
        if (!w->gpu.upload(f, w->data)) return false;
    }
#endif
    if (!w->gpuDraw) {
        const Image& img = win.rasterize(full ? Rect{} : dmg);
        if (img.width != int(W) || img.height != int(H)) return false;
        for (int y = y0; y < y1; ++y)
            std::memcpy(w->uploadPtr[f] + (std::size_t(y) * W + x0) * 4, img.pixels.data() + std::size_t(y) * W + x0,
                        std::size_t(x1 - x0) * 4);
    }

    VkCommandBuffer cmd = w->cmd[f];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    // canvas: previous blit read (or nothing) -> transfer write
    VkImageMemoryBarrier b0 = imageBarrier(w->canvas, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                           w->canvasFresh ? VK_IMAGE_LAYOUT_UNDEFINED : G, G);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b0);
    w->canvasFresh = false;
    if (w->gpuDraw) {
#if defined(FUSE_APP_HAS_UI_VK)
        VkClearColorValue black{};
        black.float32[3] = 1.f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, w->canvas, G, &black, 1, &range);
        VkImageMemoryBarrier b1 = imageBarrier(w->canvas, VK_ACCESS_TRANSFER_WRITE_BIT,
                                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, G, G);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b1);
        // external textures (engine frames on this device): their writer's layout -> sampled for the kernel and back.
        // ALL_COMMANDS / MEMORY_WRITE orders after earlier submissions on this queue (same-queue producer).
        VkImageMemoryBarrier eb[4];
        u32 en = 0;
        for (u32 e = 0; e < 4; ++e) {
            const ExternalTexture& x = w->external[e];
            const bool bound = x.image && x.view && x.layout != VK_IMAGE_LAYOUT_UNDEFINED;
            w->gpu.setExternalImage(e, bound ? x.view : nullptr);
            if (!bound) continue;
            eb[en++] = imageBarrier(static_cast<VkImage>(x.image), VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VkImageLayout(x.layout), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        if (en)
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                                 0, nullptr, en, eb);
        w->gpu.record(cmd, w->canvasView, f);
        if (en) {
            for (u32 i = 0; i < en; ++i) {
                std::swap(eb[i].oldLayout, eb[i].newLayout);
                eb[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                eb[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                                 0, nullptr, en, eb);
        }
        VkImageMemoryBarrier b2 = imageBarrier(w->canvas, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, G, G);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b2);
#endif
    } else {
        VkBufferImageCopy cp{};
        cp.bufferOffset = (VkDeviceSize(y0) * W + VkDeviceSize(x0)) * 4;
        cp.bufferRowLength = W;
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageOffset = {x0, y0, 0};
        cp.imageExtent = {u32(x1 - x0), u32(y1 - y0), 1};
        vkCmdCopyBufferToImage(cmd, w->upload[f], w->canvas, G, 1, &cp);
        VkImageMemoryBarrier b2 = imageBarrier(w->canvas, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, G, G);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
    }
    VkImage sw = w->images[index];
    VkImageMemoryBarrier s0 = imageBarrier(sw, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &s0);
    // swapchain contents are undefined after acquire: always blit the whole canvas (the canvas keeps the history)
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {int(W), int(H), 1};
    blit.dstOffsets[1] = {int(W), int(H), 1};
    vkCmdBlitImage(cmd, w->canvas, G, sw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    VkImageMemoryBarrier s1 = imageBarrier(sw, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &s1);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &w->acquired[f];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &w->renderDone[index];
    if (vkQueueSubmit(c.queue, 1, &si, w->fence[f]) != VK_SUCCESS) return false;
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &w->renderDone[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &w->swapchain;
    pi.pImageIndices = &index;
    r = K.QueuePresentKHR(c.queue, &pi);
    w->frame = (f + 1) % kFrames;
    w->needFull = false;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        w->createSwapchain(vw, vh, err);
        win.requestRedraw();
    }
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR;
}

bool vkWinReadback(VkWin* w, Image& out) {
    if (!w || !w->canvas || w->canvasFresh) return false;
    VkCtx& c = *w->ctx;
    vkDeviceWaitIdle(c.device);
    const u32 W = w->extent.width, H = w->extent.height;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = VkDeviceSize(W) * H * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkCreateBuffer(c.device, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c.device, buf, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType(c.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    bool good = vkAllocateMemory(c.device, &mai, nullptr, &mem) == VK_SUCCESS;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (good) {
        vkBindBufferMemory(c.device, buf, mem, 0);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = c.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        good = vkAllocateCommandBuffers(c.device, &cai, &cmd) == VK_SUCCESS;
    }
    if (good) {
        VkCommandBufferBeginInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cb);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, w->canvas, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &cp);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        good = vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(c.queue) == VK_SUCCESS;
    }
    void* p = nullptr;
    if (good && vkMapMemory(c.device, mem, 0, bi.size, 0, &p) == VK_SUCCESS) {
        out.resize(int(W), int(H));
        std::memcpy(out.pixels.data(), p, std::size_t(bi.size));
        for (u32& px : out.pixels) px |= 0xFF000000u;
        vkUnmapMemory(c.device, mem);
    } else {
        good = false;
    }
    if (cmd) vkFreeCommandBuffers(c.device, c.pool, 1, &cmd);
    if (mem) vkFreeMemory(c.device, mem, nullptr);
    vkDestroyBuffer(c.device, buf, nullptr);
    return good;
}

} // namespace fuse::app::detail
