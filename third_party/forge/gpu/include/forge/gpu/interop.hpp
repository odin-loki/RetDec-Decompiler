#pragma once
// Graphics interop (Part B.5 / F.2 FUSE): share GPU memory between an application's renderer and FORGE.
//   vk_external: on the application's VkDevice (which must enable VK_KHR_external_memory_fd, or _win32 on Windows),
//                allocate exportable buffers, export them as an OS handle and import handles back. Semaphores likewise.
//   cuda_import: map an exported Vulkan buffer / semaphore into CUDA (cuImportExternalMemory) so CUDA kernels
//                read/write the renderer's buffer without a copy.
// Tested: Vulkan export -> import round trip on lavapipe. CUDA import: HARDWARE-UNTESTED.
#include <forge/core/dl.hpp>
#include <forge/os/os.hpp>
#include <forge/core/error.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <forge/gpu/vulkan.hpp>   // defines FORGE_GPU_VULKAN when Vulkan is available


namespace forge::gpu::interop {
#if defined(_WIN32)
using os_handle = void*;          // HANDLE (NT handle; close with CloseHandle)
inline constexpr os_handle no_handle = nullptr;
#else
using os_handle = int;            // POSIX fd (ownership passes to whoever imports it)
inline constexpr os_handle no_handle = -1;
#endif

#if defined(FORGE_GPU_VULKAN)
// Device functions are resolved through the app's vkGetDeviceProcAddr; nothing here owns the device.
class vk_external {
public:
    struct buffer { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; VkDeviceSize size = 0; std::uint32_t type = 0; };
    vk_external(VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa, VkInstance inst) : pd_(pd), dev_(dev) {
        auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));
        mp_ = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
#define D(f) f##_ = reinterpret_cast<PFN_##f>(gdpa(dev, #f));
        D(vkCreateBuffer) D(vkDestroyBuffer) D(vkGetBufferMemoryRequirements) D(vkAllocateMemory) D(vkFreeMemory) D(vkBindBufferMemory)
        D(vkMapMemory) D(vkUnmapMemory) D(vkCreateSemaphore) D(vkDestroySemaphore)
#if defined(_WIN32)
        D(vkGetMemoryWin32HandleKHR) D(vkGetSemaphoreWin32HandleKHR)
#else
        D(vkGetMemoryFdKHR) D(vkGetSemaphoreFdKHR) D(vkImportSemaphoreFdKHR)
#endif
#undef D
    }
    bool ok() const {
#if defined(_WIN32)
        return vkGetMemoryWin32HandleKHR_ != nullptr;
#else
        return vkGetMemoryFdKHR_ != nullptr;
#endif
    }
    static constexpr VkExternalMemoryHandleTypeFlagBits mem_type =
#if defined(_WIN32)
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    // host_visible: also map-able (for tests / staging); otherwise device-local.
    buffer create(VkDeviceSize size, bool host_visible, os_handle import = no_handle) {
        buffer r; r.size = size;
        VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr, VkExternalMemoryHandleTypeFlags(mem_type)};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
        if (vkCreateBuffer_(dev_, &bi, nullptr, &r.b) != VK_SUCCESS) raise(errc::io_error, "interop: vkCreateBuffer failed");
        VkMemoryRequirements req; vkGetBufferMemoryRequirements_(dev_, r.b, &req);
        VkPhysicalDeviceMemoryProperties mp; mp_(pd_, &mp);
        VkMemoryPropertyFlags want = host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        std::uint32_t t = UINT32_MAX;
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((req.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) { t = i; break; }
        if (t == UINT32_MAX) raise(errc::unsupported, "interop: no suitable memory type");
        r.type = t;
        VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VkExternalMemoryHandleTypeFlags(mem_type)};
#if defined(_WIN32)
        VkImportMemoryWin32HandleInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, nullptr, mem_type, import, nullptr};
#else
        VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr, mem_type, import};
#endif
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, VK_NULL_HANDLE, r.b};
        if (import != no_handle) { im.pNext = &ded; } else { ex.pNext = &ded; }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, import != no_handle ? static_cast<const void*>(&im) : &ex, req.size, t};
        if (vkAllocateMemory_(dev_, &ai, nullptr, &r.m) != VK_SUCCESS) { vkDestroyBuffer_(dev_, r.b, nullptr); raise(errc::io_error, "interop: vkAllocateMemory ({}) failed", import != no_handle ? "import" : "export"); }
        vkBindBufferMemory_(dev_, r.b, r.m, 0);
        return r;
    }
    os_handle export_handle(const buffer& b) {
        os_handle h = no_handle;
#if defined(_WIN32)
        VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, b.m, mem_type};
        if (vkGetMemoryWin32HandleKHR_(dev_, &gi, &h) != VK_SUCCESS) raise(errc::io_error, "interop: vkGetMemoryWin32HandleKHR failed");
#else
        VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, b.m, mem_type};
        if (vkGetMemoryFdKHR_(dev_, &gi, &h) != VK_SUCCESS) raise(errc::io_error, "interop: vkGetMemoryFdKHR failed");
#endif
        return h;
    }
    void* map(const buffer& b) { void* p = nullptr; if (vkMapMemory_(dev_, b.m, 0, b.size, 0, &p) != VK_SUCCESS) raise(errc::io_error, "interop: map failed"); return p; }
    void unmap(const buffer& b) { vkUnmapMemory_(dev_, b.m); }
    void destroy(buffer& b) { if (b.b) vkDestroyBuffer_(dev_, b.b, nullptr); if (b.m) vkFreeMemory_(dev_, b.m, nullptr); b = {}; }
    // Exportable binary semaphore (for renderer <-> compute ordering).
    VkSemaphore create_semaphore() {
        VkExportSemaphoreCreateInfo ex{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr,
#if defined(_WIN32)
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
#else
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
#endif
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ex, 0};
        VkSemaphore s = VK_NULL_HANDLE; if (vkCreateSemaphore_(dev_, &ci, nullptr, &s) != VK_SUCCESS) raise(errc::io_error, "interop: vkCreateSemaphore failed");
        return s;
    }
    os_handle export_semaphore(VkSemaphore s) {
        os_handle h = no_handle;
#if defined(_WIN32)
        VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, s, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        if (!vkGetSemaphoreWin32HandleKHR_ || vkGetSemaphoreWin32HandleKHR_(dev_, &gi, &h) != VK_SUCCESS) raise(errc::io_error, "interop: semaphore export failed");
#else
        VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr, s, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        if (!vkGetSemaphoreFdKHR_ || vkGetSemaphoreFdKHR_(dev_, &gi, &h) != VK_SUCCESS) raise(errc::io_error, "interop: semaphore export failed");
#endif
        return h;
    }
    void destroy_semaphore(VkSemaphore s) { vkDestroySemaphore_(dev_, s, nullptr); }
    // Optional: needed for create_semaphore/export_semaphore.
    static const char* const* semaphore_extensions(std::uint32_t& n) {
#if defined(_WIN32)
        static const char* e[] = {"VK_KHR_external_semaphore", "VK_KHR_external_semaphore_win32"};
#else
        static const char* e[] = {"VK_KHR_external_semaphore", "VK_KHR_external_semaphore_fd"};
#endif
        n = 2; return e;
    }
    bool has_semaphores() const {
#if defined(_WIN32)
        return vkGetSemaphoreWin32HandleKHR_ != nullptr;
#else
        return vkGetSemaphoreFdKHR_ != nullptr;
#endif
    }
    // Device extensions the application must enable for buffer sharing.
    static const char* const* required_device_extensions(std::uint32_t& n) {
#if defined(_WIN32)
        static const char* e[] = {"VK_KHR_external_memory", "VK_KHR_external_memory_win32", "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2"};
#else
        static const char* e[] = {"VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2"};
#endif
        n = std::uint32_t(sizeof e / sizeof *e); return e;
    }
private:
    VkPhysicalDevice pd_; VkDevice dev_;
    PFN_vkGetPhysicalDeviceMemoryProperties mp_ = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer_ = nullptr; PFN_vkDestroyBuffer vkDestroyBuffer_ = nullptr; PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements_ = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory_ = nullptr; PFN_vkFreeMemory vkFreeMemory_ = nullptr; PFN_vkBindBufferMemory vkBindBufferMemory_ = nullptr;
    PFN_vkMapMemory vkMapMemory_ = nullptr; PFN_vkUnmapMemory vkUnmapMemory_ = nullptr;
    PFN_vkCreateSemaphore vkCreateSemaphore_ = nullptr; PFN_vkDestroySemaphore vkDestroySemaphore_ = nullptr;
#if defined(_WIN32)
    PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR_ = nullptr; PFN_vkGetSemaphoreWin32HandleKHR vkGetSemaphoreWin32HandleKHR_ = nullptr;
#else
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR_ = nullptr; PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR_ = nullptr; PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR_ = nullptr;
#endif
};
#endif // FORGE_GPU_VULKAN

// ---- CUDA side: import an exported allocation / semaphore (driver API, dlopened). ----
class cuda_import {
public:
    // Mirrors of the CUDA driver structs (cuda.h, CUDA 10+; layout is part of the stable driver ABI).
    struct ext_mem_desc { int type; int pad_; union { int fd; struct { void* handle; const void* name; } win32; const void* nvSciBufObject; } handle; unsigned long long size; unsigned flags; unsigned reserved[16]; };
    struct ext_buf_desc { unsigned long long offset, size; unsigned flags; unsigned reserved[16]; };
    struct ext_sem_desc { int type; int pad_; union { int fd; struct { void* handle; const void* name; } win32; const void* nvSciSyncObj; } handle; unsigned flags; unsigned reserved[16]; };
    struct sem_wait_params { struct { struct { unsigned long long value; } fence; union { void* fence; unsigned long long reserved; } nvSciSync; struct { unsigned long long key; unsigned timeoutMs; } keyedMutex; unsigned reserved[10]; } params; unsigned flags; unsigned reserved[16]; };
    static constexpr int CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD = 1, CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32 = 2;
    static constexpr int CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD = 1, CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32 = 2;

    bool open(std::string* why) {
        lib_ = std::move(os::library::open(FORGE_DLNAME("libcuda.so.1", "nvcuda.dll")).value);
        if (!lib_.valid()) { if (why) *why = "interop: libcuda not found"; return false; }
        auto s = [&](auto& f, const char* n) { f = lib_.get<std::remove_reference_t<decltype(f)>>(n); return f != nullptr; };
        if (!(s(init_, "cuInit") && s(imp_, "cuImportExternalMemory") && s(map_, "cuExternalMemoryGetMappedBuffer") && s(destroy_, "cuDestroyExternalMemory")
              && s(simp_, "cuImportExternalSemaphore") && s(swait_, "cuWaitExternalSemaphoresAsync") && s(ssig_, "cuSignalExternalSemaphoresAsync") && s(sdestroy_, "cuDestroyExternalSemaphore"))) {
            if (why) *why = "interop: libcuda lacks external memory API"; return false; }
        return init_(0) == 0;
    }
    // Requires a current CUDA context (e.g. the one gpu::cuda_hip::runtime created). Returns a CUdeviceptr.
    unsigned long long import_buffer(os_handle h, unsigned long long size, void** ext_out) {
        ext_mem_desc d{}; d.size = size; d.flags = 1;   // CUDA_EXTERNAL_MEMORY_DEDICATED
#if defined(_WIN32)
        d.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32; d.handle.win32.handle = h;
#else
        d.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD; d.handle.fd = h;
#endif
        void* ext = nullptr; if (imp_(&ext, &d) != 0) raise(errc::io_error, "interop: cuImportExternalMemory failed");
        ext_buf_desc bd{}; bd.size = size; unsigned long long p = 0;
        if (map_(&p, ext, &bd) != 0) { destroy_(ext); raise(errc::io_error, "interop: cuExternalMemoryGetMappedBuffer failed"); }
        *ext_out = ext; return p;
    }
    void release(void* ext) { if (ext) destroy_(ext); }
    void* import_semaphore(os_handle h) {
        ext_sem_desc d{};
#if defined(_WIN32)
        d.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32; d.handle.win32.handle = h;
#else
        d.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD; d.handle.fd = h;
#endif
        void* s = nullptr; if (simp_(&s, &d) != 0) raise(errc::io_error, "interop: cuImportExternalSemaphore failed");
        return s;
    }
    void wait(void* sem, void* stream) { sem_wait_params p{}; swait_(&sem, &p, 1, stream); }
    void signal(void* sem, void* stream) { sem_wait_params p{}; ssig_(&sem, &p, 1, stream); }
    void release_semaphore(void* s) { if (s) sdestroy_(s); }
private:
    os::library lib_;
    int (*init_)(unsigned) = nullptr;
    int (*imp_)(void**, const ext_mem_desc*) = nullptr;
    int (*map_)(unsigned long long*, void*, const ext_buf_desc*) = nullptr;
    int (*destroy_)(void*) = nullptr;
    int (*simp_)(void**, const ext_sem_desc*) = nullptr;
    int (*swait_)(void* const*, const sem_wait_params*, unsigned, void*) = nullptr;
    int (*ssig_)(void* const*, const sem_wait_params*, unsigned, void*) = nullptr;
    int (*sdestroy_)(void*) = nullptr;
};
} // namespace forge::gpu::interop
