// Graphics interop: export a Vulkan buffer as an OS handle, import it back (same device) and check the bytes are shared.
// Plays the role of an application renderer creating its own VkDevice. CUDA import runs only with a CUDA driver.
#include "check.hpp"
#include <forge/gpu/interop.hpp>
#include <cstdio>
#include <cstring>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <vector>
using namespace forge::gpu::interop;

#if defined(FORGE_GPU_VULKAN)
TEST(vk_export_import_roundtrip) {
    void* lib = dlopen(FORGE_DLNAME("libvulkan.so.1", "vulkan-1.dll"), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::puts("  skip: no Vulkan loader"); return; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "interop", 1, "forge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkInstance inst; if (create(&ici, nullptr, &inst) != VK_SUCCESS) { std::puts("  skip: vkCreateInstance failed"); return; }
    auto enumpd = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto enumext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(inst, "vkEnumerateDeviceExtensionProperties"));
    auto qfp = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto mkdev = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst, "vkCreateDevice"));
    auto dinst = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    std::uint32_t n = 0; enumpd(inst, &n, nullptr);
    if (!n) { std::puts("  skip: no Vulkan device"); dinst(inst, nullptr); return; }
    std::vector<VkPhysicalDevice> pds(n); enumpd(inst, &n, pds.data()); auto pd = pds[0];
    std::uint32_t ne = 0; enumext(pd, nullptr, &ne, nullptr); std::vector<VkExtensionProperties> ex(ne); enumext(pd, nullptr, &ne, ex.data());
    std::uint32_t nreq = 0; auto req = vk_external::required_device_extensions(nreq);
    for (std::uint32_t i = 0; i < nreq; ++i) {
        bool have = false; for (auto& e : ex) if (!std::strcmp(e.extensionName, req[i])) have = true;
        if (!have) { std::printf("  skip: device lacks %s\n", req[i]); dinst(inst, nullptr); return; }
    }
    std::vector<const char*> en(req, req + nreq);
    std::uint32_t ns = 0; auto sx = vk_external::semaphore_extensions(ns); bool sem = true;
    for (std::uint32_t i = 0; i < ns; ++i) { bool have = false; for (auto& e : ex) if (!std::strcmp(e.extensionName, sx[i])) have = true; sem &= have; }
    if (sem) en.insert(en.end(), sx, sx + ns); else std::puts("  (no external semaphores on this device)");
    std::uint32_t nq = 0; qfp(pd, &nq, nullptr);
    float pr = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &pr};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, std::uint32_t(en.size()), en.data(), nullptr};
    VkDevice dev; CHECK(mkdev(pd, &dci, nullptr, &dev) == VK_SUCCESS);
    {
        vk_external x(pd, dev, gipa, inst); CHECK(x.ok());
        auto a = x.create(4096, true);
        auto* p = static_cast<std::uint32_t*>(x.map(a)); for (int i = 0; i < 1024; ++i) p[i] = 0xC0DE0000u + std::uint32_t(i); x.unmap(a);
        auto h = x.export_handle(a); CHECK(h != no_handle);
        auto b = x.create(4096, true, h);   // import: the same memory under a second VkBuffer
        auto* q = static_cast<std::uint32_t*>(x.map(b)); bool same = true; for (int i = 0; i < 1024; ++i) same &= q[i] == 0xC0DE0000u + std::uint32_t(i);
        q[7] = 42; x.unmap(b);
        CHECK(same);
        p = static_cast<std::uint32_t*>(x.map(a)); CHECK(p[7] == 42); x.unmap(a);   // writes through the import are visible to the exporter
        if (sem) {
            auto s = x.create_semaphore(); auto sh = x.export_semaphore(s); CHECK(sh != no_handle);
#if !defined(_WIN32)
            ::close(sh);
#endif
            x.destroy_semaphore(s);
        } x.destroy(b); x.destroy(a);
        cuda_import cu; std::string why;
        if (!cu.open(&why)) std::printf("  cuda import: skip (%s)\n", why.c_str());
    }
    reinterpret_cast<PFN_vkDestroyDevice>(gipa(inst, "vkDestroyDevice"))(dev, nullptr);
    dinst(inst, nullptr);
}
#else
TEST(vk_not_built) { std::puts("  skip: Vulkan not built"); }
#endif
