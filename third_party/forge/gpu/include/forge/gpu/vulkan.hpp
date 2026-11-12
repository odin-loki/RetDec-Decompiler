#pragma once
// gpu Vulkan backend: loads libvulkan at run time (no link dependency), runs the compute kernels that
// ops/gen/gpu_gen.py generates from spec/ops (embedded as SPIR-V). Tested on Mesa lavapipe in CI.
// Available when <vulkan/vulkan.h> and the generated SPIR-V header are both present at build time.
#if __has_include(<vulkan/vulkan.h>) && __has_include(<forge/gpu/spirv.gen.hpp>) && (defined(__linux__) || defined(__APPLE__) || defined(_WIN32))
#define FORGE_GPU_VULKAN 1
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <forge/gpu/spirv.gen.hpp>
#include <forge/core/tensor.hpp>
#include <forge/core/dl.hpp>
#include <forge/gpu/device_memory.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <forge/ops/ops.gen.hpp>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace forge::gpu::vk {
// GLSL sin/cos are not correctly rounded; the Vulkan spec bounds them at 2^-11 absolute error on [-pi,pi],
// which SwiftShader (Android emulator) actually uses. Ops built on them get that bound.
inline double transcendental_tol(const std::string& n) { return (n == "sin" || n == "cos" || n == "rope") ? 5e-4 : 0.0; }

struct push { std::uint32_t n = 0, rows = 0, cols = 0, k = 0; float alpha = 0; std::uint32_t a = 0, b = 0, c = 0, d = 0; float beta = 0; };
struct hbuf { float* data; std::size_t count; };

class runtime {
public:
    // Returns nullptr (with reason) when no loader / no compute-capable device is present.
    static std::unique_ptr<runtime> open(std::string* why = nullptr) {
        auto r = std::unique_ptr<runtime>(new runtime);
        std::string e = r->init();
        if (!e.empty()) { if (why) *why = e; return nullptr; }
        return r;
    }
    ~runtime() {
        dm_.detach();
        if (dev_) {
            vkDeviceWaitIdle(dev_);
            if (cb_) vkFreeCommandBuffers(dev_, pool_, 1, &cb_);
            for (auto dp : pending_pools_) vkDestroyDescriptorPool(dev_, dp, nullptr);
            for (auto& [k, e] : res_) free_entry(e);
            for (auto& [n, p] : pipes_) { vkDestroyPipeline(dev_, p.pipe, nullptr); vkDestroyPipelineLayout(dev_, p.layout, nullptr);
                                          vkDestroyDescriptorSetLayout(dev_, p.dsl, nullptr); vkDestroyShaderModule(dev_, p.mod, nullptr); }
            if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
            if (pcache_) { save_pipeline_cache(); vkDestroyPipelineCache(dev_, pcache_, nullptr); }
            vkDestroyDevice(dev_, nullptr);
        }
        if (inst_) vkDestroyInstance(inst_, nullptr);
        if (lib_) dlclose(lib_);
    }
    const std::string& device_name() const { return name_; }
    // On-disk pipeline cache: keyed by vendor/device/driver version, the pipelineCacheUUID and a hash of
    // the embedded SPIR-V, under $FORGE_CACHE_DIR (else $XDG_CACHE_HOME/forge, else ~/.cache/forge).
    // FORGE_VK_PIPELINE_CACHE=0 disables it. Written atomically at destruction.
    const std::string& pipeline_cache_path() const { return pcache_path_; }
    std::size_t pipeline_cache_loaded_bytes() const { return pcache_loaded_; }
    void save_pipeline_cache() {
        if (!pcache_ || pcache_path_.empty()) return;
        std::size_t n = 0; if (vkGetPipelineCacheData(dev_, pcache_, &n, nullptr) != VK_SUCCESS || !n) return;
        std::vector<char> d(n); if (vkGetPipelineCacheData(dev_, pcache_, &n, d.data()) != VK_SUCCESS) return;
        std::error_code ec; std::filesystem::create_directories(std::filesystem::path(pcache_path_).parent_path(), ec);
        std::string tmp = pcache_path_ + ".tmp" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        { std::ofstream o(tmp, std::ios::binary); if (!o) return; o.write(d.data(), std::streamsize(n)); if (!o) return; }
        std::filesystem::rename(tmp, pcache_path_, ec); if (ec) std::filesystem::remove(tmp, ec);
    }
    std::map<std::string, std::size_t> dispatches() { std::lock_guard lk(mu_); return dispatches_; }

    static bool has_kernel(const std::string& n) { for (auto& b : spv::table) if (n == b.name) return true; return false; }

    // Run kernel `name` over the given host buffers (bound 0..N-1 in order; the last is written back).
    void run(const std::string& name, const std::vector<hbuf>& bufs, const push& pc, std::uint32_t invocations) {
        std::lock_guard lk(mu_); begin_op();
        ++dispatches_[name];
        auto& p = pipeline(name, std::uint32_t(bufs.size()));
        struct dbuf { VkBuffer b; VkDeviceMemory m; VkDeviceSize sz; };
        std::vector<dbuf> d;
        for (auto& h : bufs) {
            VkDeviceSize sz = std::max<VkDeviceSize>(4, h.count * 4);
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = sz; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VkBuffer b; ck(vkCreateBuffer(dev_, &bi, nullptr, &b), "vkCreateBuffer");
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev_, b, &mr);
            VkDeviceMemory m;
            try { m = vk_alloc(mr.size, memtype(mr.memoryTypeBits), "scratch"); } catch (...) { vkDestroyBuffer(dev_, b, nullptr); for (auto& x : d) { vkDestroyBuffer(dev_, x.b, nullptr); vk_free(x.m); } throw; }
            ck(vkBindBufferMemory(dev_, b, m, 0), "vkBindBufferMemory");
            void* ptr; ck(vkMapMemory(dev_, m, 0, sz, 0, &ptr), "vkMapMemory");
            std::memcpy(ptr, h.data, h.count * 4); vkUnmapMemory(dev_, m);
            d.push_back({b, m, sz});
        }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, std::uint32_t(bufs.size())};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VkDescriptorPool dp; ck(vkCreateDescriptorPool(dev_, &pi, nullptr, &dp), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sa.descriptorPool = dp; sa.descriptorSetCount = 1; sa.pSetLayouts = &p.dsl;
        VkDescriptorSet ds; ck(vkAllocateDescriptorSets(dev_, &sa, &ds), "vkAllocateDescriptorSets");
        std::vector<VkDescriptorBufferInfo> dbi; std::vector<VkWriteDescriptorSet> w;
        dbi.reserve(d.size());
        for (std::size_t i = 0; i < d.size(); ++i) {
            dbi.push_back({d[i].b, 0, VK_WHOLE_SIZE});
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = ds; x.dstBinding = std::uint32_t(i); x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x.pBufferInfo = &dbi.back(); w.push_back(x);
        }
        vkUpdateDescriptorSets(dev_, std::uint32_t(w.size()), w.data(), 0, nullptr);
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool_; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        VkCommandBuffer cb; ck(vkAllocateCommandBuffers(dev_, &ca, &cb), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ck(vkBeginCommandBuffer(cb, &bb), "vkBeginCommandBuffer");
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cb, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &pc);
        vkCmdDispatch(cb, (invocations + 63) / 64, 1, 1);
        ck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; ck(vkCreateFence(dev_, &fi, nullptr, &f), "vkCreateFence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        ck(vkQueueSubmit(q_, 1, &si, f), "vkQueueSubmit");
        ck(vkWaitForFences(dev_, 1, &f, VK_TRUE, ~0ull), "vkWaitForFences");
        auto& out = d.back(); void* ptr; ck(vkMapMemory(dev_, out.m, 0, out.sz, 0, &ptr), "vkMapMemory");
        std::memcpy(bufs.back().data, ptr, bufs.back().count * 4); vkUnmapMemory(dev_, out.m);
        vkDestroyFence(dev_, f, nullptr); vkFreeCommandBuffers(dev_, pool_, 1, &cb); vkDestroyDescriptorPool(dev_, dp, nullptr);
        for (auto& x : d) { vkDestroyBuffer(dev_, x.b, nullptr); vk_free(x.m); }
    }

    // ---- Resident mode -------------------------------------------------------------------------------
    // Between begin_run() and end_run() (one executor run), run_resident() keeps every buffer it touches on
    // the device. Buffers are keyed by host address. Outputs stay on the device (the host copy is stale) and
    // dispatches are recorded into one command buffer, which is only submitted when the host needs data
    // (to_host / host_wrote / end_run). Pinned ranges (weights) survive end_run. Everything else is flushed
    // back to the host and dropped, so memory reused between runs can never alias a stale device copy.
    void begin_run() { std::lock_guard lk(mu_); in_run_ = true; }
    void end_run() {
        std::lock_guard lk(mu_);
        submit_locked();
        for (auto it = res_.begin(); it != res_.end();) {
            auto& e = it->second;
            if (!e.host_valid) { std::memcpy(e.host, e.map, e.count * 4); e.host_valid = true; ++stats_.downloads; }
            if (e.pinned) { ++it; continue; }
            free_entry(e); it = res_.erase(it);
        }
        in_run_ = false;
    }
    bool resident() { std::lock_guard lk(mu_); return in_run_; }
    void pin(const float* p, std::size_t n) { std::lock_guard lk(mu_); pinned_.push_back({p, n}); }
    // The host is about to read [p, p+n): bring any device-only data back.
    void to_host(const float* p, std::size_t n) {
        std::lock_guard lk(mu_);
        if (!overlaps_any(p, n)) return;
        submit_locked();
        for_overlapping(p, n, [&](entry& e) { if (!e.host_valid) { std::memcpy(e.host, e.map, e.count * 4); e.host_valid = true; ++stats_.downloads; } });
    }
    // The host has written [p, p+n) (after a to_host of the same range): patch the overlapping device copies.
    void host_wrote(const float* p, std::size_t n) {
        std::lock_guard lk(mu_);
        if (!overlaps_any(p, n)) return;
        submit_locked();
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + n * 4;
        for_overlapping(p, n, [&](entry& e) {
            auto el = reinterpret_cast<std::uintptr_t>(e.host), eh = el + e.count * 4;
            auto a = std::max(lo, el), b = std::min(hi, eh);
            std::memcpy(static_cast<char*>(e.map) + (a - el), reinterpret_cast<const void*>(a), b - a);
            stats_.upload_bytes += b - a;
        });
    }
    // [p, p+n) is about to be freed by the host: sync it back and drop every overlapping device copy, so a
    // later allocation at the same address can never pick up this buffer's device data.
    void release(const float* p, std::size_t n) {
        std::lock_guard lk(mu_);
        if (!overlaps_any(p, n)) return;
        submit_locked();
        std::vector<std::uintptr_t> ks;
        for_overlapping(p, n, [&](entry& e) {
            if (!e.host_valid) { std::memcpy(e.host, e.map, e.count * 4); ++stats_.downloads; }
            ks.push_back(reinterpret_cast<std::uintptr_t>(e.host)); });
        for (auto k : ks) { free_entry(res_[k]); res_.erase(k); }
    }
    // evictions / evicted_bytes: resident buffers dropped to fit the device budget (gpu/device_memory.hpp)
    struct residency_stats { std::size_t uploads = 0, downloads = 0, submits = 0, upload_bytes = 0, resident = 0, resident_bytes = 0, peak_resident_bytes = 0,
                             evictions = 0, evicted_bytes = 0; };
    residency_stats residency() { std::lock_guard lk(mu_); auto s = stats_; s.resident = res_.size(); return s; }
    mem::budget* memory_pool() { return dm_.pool(); }   // "vulkan:0"
    std::uint64_t charged_bytes() { std::lock_guard lk(mu_); return dm_.charged(); }

    // Record kernel `name` over resident buffers (bound 0..N-1; the last is the output).
    void run_resident(const std::string& name, const std::vector<hbuf>& bufs, const push& pc, std::uint32_t invocations) {
        {   // an output that partially overlaps an input can't share one resident buffer: run it via the host
            auto& o = bufs.back(); bool clash = false;
            for (std::size_t i = 0; i + 1 < bufs.size(); ++i) {
                auto& b = bufs[i];
                bool ov = b.data < o.data + o.count && o.data < b.data + b.count;
                if (ov && !(b.data == o.data && b.count == o.count)) clash = true;
            }
            if (clash) {
                for (auto& b : bufs) to_host(b.data, b.count);
                run(name, bufs, pc, invocations);
                host_wrote(o.data, o.count);
                return;
            }
        }
        std::unique_lock lk(mu_); begin_op();
        std::vector<bound> es;
        for (std::size_t i = 0; i < bufs.size(); ++i) es.push_back(acquire(bufs[i], i + 1 == bufs.size()));
        // a later acquisition may have dropped an earlier binding (partially overlapping views): re-check
        bool stable = true;
        for (std::size_t i = 0; i < bufs.size(); ++i) { auto b = acquire(bufs[i], i + 1 == bufs.size()); if ((b.parent ? b.parent : b.e) != (es[i].parent ? es[i].parent : es[i].e) || b.off + b.poff != es[i].off + es[i].poff) stable = false; }
        if (!stable) {
            lk.unlock();
            for (auto& b : bufs) release(b.data, b.count);
            run(name, bufs, pc, invocations);
            return;
        }
        ++dispatches_[name];
        auto& p = pipeline(name, std::uint32_t(bufs.size()));
        es.back().e->host_valid = false;
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, std::uint32_t(bufs.size())};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VkDescriptorPool dp; ck(vkCreateDescriptorPool(dev_, &pi, nullptr, &dp), "vkCreateDescriptorPool");
        pending_pools_.push_back(dp);
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sa.descriptorPool = dp; sa.descriptorSetCount = 1; sa.pSetLayouts = &p.dsl;
        VkDescriptorSet ds; ck(vkAllocateDescriptorSets(dev_, &sa, &ds), "vkAllocateDescriptorSets");
        std::vector<VkDescriptorBufferInfo> dbi(es.size()); std::vector<VkWriteDescriptorSet> w;
        for (std::size_t i = 0; i < es.size(); ++i) {
            dbi[i] = {es[i].e->b, es[i].off, es[i].range};
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; x.dstSet = ds; x.dstBinding = std::uint32_t(i); x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x.pBufferInfo = &dbi[i]; w.push_back(x);
        }
        vkUpdateDescriptorSets(dev_, std::uint32_t(w.size()), w.data(), 0, nullptr);
        open_cb();
        vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cb_, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &pc);
        vkCmdDispatch(cb_, (invocations + 63) / 64, 1, 1);
        ++recorded_;
        if (auto& o = es.back(); o.parent) {   // staged output: copy the result back into its parent buffer
            open_cb(); VkBufferCopy r{0, o.poff, o.range}; vkCmdCopyBuffer(cb_, o.e->b, o.parent->b, 1, &r); ++recorded_;
            o.parent->host_valid = false;
        }
    }
    // Device-side copy of n floats from src into dst_base[off..] (dst_base spans dst_count floats, e.g. a
    // state slot). Both stay resident; nothing touches the host.
    void copy_resident(float* src, std::size_t n, float* dst_base, std::size_t dst_count, std::size_t off) {
        std::lock_guard lk(mu_); begin_op();
        if (!n) return;
        auto d = acquire({dst_base, dst_count}, false);
        auto sb = acquire({src, n}, false);
        d = acquire({dst_base, dst_count}, false);   // src acquisition can't drop dst (disjoint), re-find anyway
        open_cb();
        VkBufferCopy r{sb.off, d.off + off * 4, n * 4};
        vkCmdCopyBuffer(cb_, sb.e->b, d.e->b, 1, &r);
        d.e->host_valid = false;
        ++recorded_;
    }
private:
    void open_cb() {
        if (!cb_) {
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool_; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
            ck(vkAllocateCommandBuffers(dev_, &ca, &cb_), "vkAllocateCommandBuffers");
            VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ck(vkBeginCommandBuffer(cb_, &bb), "vkBeginCommandBuffer");
        } else {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            auto st = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            vkCmdPipelineBarrier(cb_, st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
        }
    }
public:

private:
    runtime() = default;
    struct pipe { VkShaderModule mod{}; VkDescriptorSetLayout dsl{}; VkPipelineLayout layout{}; VkPipeline pipe{}; };
    static void ck(VkResult r, const char* what) { if (r != VK_SUCCESS) throw std::runtime_error(std::string("vulkan: ") + what + " failed (" + std::to_string(int(r)) + ")"); }

#define FORGE_VK_FNS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define FORGE_VK_DEV_FNS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCreateFence) X(vkDestroyFence) X(vkQueueSubmit) X(vkWaitForFences) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData)
#define FORGE_VK_DECL(f) PFN_##f f = nullptr;
    FORGE_VK_FNS(FORGE_VK_DECL)
    FORGE_VK_DEV_FNS(FORGE_VK_DECL)
#undef FORGE_VK_DECL

    std::string init() {
        for (const char* so : {"libvulkan.so.1", "libvulkan.so", "vulkan-1.dll", "libvulkan.1.dylib", "libMoltenVK.dylib"})
            if ((lib_ = dlopen(so, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib_) return "vulkan loader not found";
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib_, "vkGetInstanceProcAddr"));
        if (!gipa) return "vkGetInstanceProcAddr missing";
        auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "forge"; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
        if (!create || create(&ici, nullptr, &inst_) != VK_SUCCESS) { inst_ = nullptr; return "vkCreateInstance failed"; }
#define FORGE_VK_LOAD(f) f = reinterpret_cast<PFN_##f>(gipa(inst_, #f)); if (!f) return "missing " #f;
        FORGE_VK_FNS(FORGE_VK_LOAD)
#undef FORGE_VK_LOAD
        std::uint32_t n = 0; vkEnumeratePhysicalDevices(inst_, &n, nullptr);
        if (!n) return "no vulkan physical devices";
        std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst_, &n, pds.data());
        int best = -1; std::uint32_t fam = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qf.data());
            for (std::uint32_t j = 0; j < qn; ++j) if (qf[j].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pds[i], &pr);
                bool discrete = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
                if (best < 0 || discrete) { best = int(i); fam = j; name_ = pr.deviceName; }
                break;
            }
        }
        if (best < 0) return "no compute queue";
        phys_ = pds[std::size_t(best)];
        { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys_, &pr); align_ = std::max<VkDeviceSize>(4, pr.limits.minStorageBufferOffsetAlignment);
          if (const char* e = std::getenv("FORGE_VK_SSBO_ALIGN")) align_ = std::max<VkDeviceSize>(align_, std::strtoull(e, nullptr, 10)); }   // emulate stricter drivers (SwiftShader: 256)
        float prio = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = fam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(phys_, &dci, nullptr, &dev_) != VK_SUCCESS) { dev_ = nullptr; return "vkCreateDevice failed"; }
#define FORGE_VK_LOAD(f) f = reinterpret_cast<PFN_##f>(vkGetDeviceProcAddr(dev_, #f)); if (!f) f = reinterpret_cast<PFN_##f>(gipa(inst_, #f)); if (!f) return "missing " #f;
        FORGE_VK_DEV_FNS(FORGE_VK_LOAD)
#undef FORGE_VK_LOAD
        vkGetDeviceQueue(dev_, fam, 0, &q_);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex = fam; cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev_, &cp, nullptr, &pool_) != VK_SUCCESS) return "vkCreateCommandPool failed";
        init_pipeline_cache();
        dm_.attach("vulkan:0", mu_, [this](std::uint64_t w, bool ext) { return evict_device(w, ext); });
        return {};
    }
    void init_pipeline_cache() {
        const char* off = std::getenv("FORGE_VK_PIPELINE_CACHE");
        std::vector<char> data;
        if (!(off && std::string(off) == "0")) {
            std::string dir;
            if (const char* e = std::getenv("FORGE_CACHE_DIR"); e && *e) dir = e;
            else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) dir = std::string(x) + "/forge";
            else if (const char* h = std::getenv("HOME"); h && *h) dir = std::string(h) + "/.cache/forge";
            if (!dir.empty()) {
                VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys_, &pr);
                std::uint64_t hsh = 1469598103934665603ull;   // FNV-1a over every embedded SPIR-V blob
                for (auto& b : spv::table) { for (const char* c = b.name; *c; ++c) hsh = (hsh ^ std::uint8_t(*c)) * 1099511628211ull;
                    auto* by = reinterpret_cast<const std::uint8_t*>(b.words);
                    for (std::size_t i = 0; i < b.count * 4; ++i) hsh = (hsh ^ by[i]) * 1099511628211ull; }
                char buf[160]; std::string uuid;
                for (auto u : pr.pipelineCacheUUID) { std::snprintf(buf, sizeof buf, "%02x", u); uuid += buf; }
                std::snprintf(buf, sizeof buf, "/vk-pipelines-%04x-%04x-%08x-%s-%016llx.bin", pr.vendorID, pr.deviceID,
                              pr.driverVersion, uuid.c_str(), static_cast<unsigned long long>(hsh));
                pcache_path_ = dir + buf;
                std::ifstream in(pcache_path_, std::ios::binary);
                if (in) data.assign(std::istreambuf_iterator<char>(in), {});
                // Header check (VkPipelineCacheHeaderVersionOne): the driver also validates, but a stale file
                // from another device must never be handed over.
                if (data.size() >= 32) {
                    std::uint32_t hl, ver, ven, devid; std::memcpy(&hl, data.data(), 4); std::memcpy(&ver, data.data() + 4, 4);
                    std::memcpy(&ven, data.data() + 8, 4); std::memcpy(&devid, data.data() + 12, 4);
                    if (hl < 32 || ver != 1 || ven != pr.vendorID || devid != pr.deviceID ||
                        std::memcmp(data.data() + 16, pr.pipelineCacheUUID, 16) != 0) data.clear();
                } else data.clear();
            }
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size(); ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(dev_, &ci, nullptr, &pcache_) != VK_SUCCESS) {
            ci.initialDataSize = 0; ci.pInitialData = nullptr; data.clear();
            if (vkCreatePipelineCache(dev_, &ci, nullptr, &pcache_) != VK_SUCCESS) pcache_ = VK_NULL_HANDLE;
        }
        pcache_loaded_ = data.size();
    }
    std::uint32_t memtype(std::uint32_t bits, bool prefer_local = false) {
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
        auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (prefer_local)   // resizable-BAR / UMA memory: device-local and still mappable
            for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) == (want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        throw std::runtime_error("vulkan: no host-visible coherent memory");
    }
    pipe& pipeline(const std::string& name, std::uint32_t nbuf) {
        auto it = pipes_.find(name); if (it != pipes_.end()) return it->second;
        const spv::blob* b = nullptr; for (auto& x : spv::table) if (name == x.name) b = &x;
        if (!b) throw std::runtime_error("vulkan: no kernel '" + name + "'");
        pipe p;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = b->count * 4; mi.pCode = b->words;
        ck(vkCreateShaderModule(dev_, &mi, nullptr, &p.mod), "vkCreateShaderModule");
        std::vector<VkDescriptorSetLayoutBinding> lb(nbuf);
        for (std::uint32_t i = 0; i < nbuf; ++i) lb[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = nbuf; li.pBindings = lb.data();
        ck(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &p.dsl), "vkCreateDescriptorSetLayout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &p.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
        ck(vkCreatePipelineLayout(dev_, &pl, nullptr, &p.layout), "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, p.mod, "main", nullptr};
        ci.layout = p.layout;
        ck(vkCreateComputePipelines(dev_, pcache_, 1, &ci, nullptr, &p.pipe), "vkCreateComputePipelines");
        return pipes_.emplace(name, p).first->second;
    }

    struct entry { float* host; std::size_t count; VkBuffer b{}; VkDeviceMemory m{}; void* map = nullptr; bool host_valid = true, pinned = false; std::uint64_t last = 0; };
    std::map<std::uintptr_t, entry> res_;   // keyed by host start address; entries never overlap
    std::vector<std::pair<const float*, std::size_t>> pinned_;
    std::vector<VkDescriptorPool> pending_pools_;
    VkCommandBuffer cb_ = nullptr; std::size_t recorded_ = 0;
    bool in_run_ = false;
    residency_stats stats_;
    void free_entry(entry& e) { stats_.resident_bytes -= e.count * 4; vkUnmapMemory(dev_, e.m); vkDestroyBuffer(dev_, e.b, nullptr); vk_free(e.m); }
    // ---- device memory accounting (gpu/device_memory.hpp): pool "vulkan:0" ----
    device_memory dm_;
    std::uint64_t clock_ = 0, op_mark_ = 0;   // buffers touched after begin_op() belong to the op in progress
    std::size_t op_scratch_ = 0;              // scratch_ entries that predate the op in progress
    void begin_op() { op_mark_ = clock_; op_scratch_ = scratch_.size(); }
    VkDeviceMemory vk_alloc(VkDeviceSize size, std::uint32_t type, const char* tag) {
        return dm_.allocate(size, tag, [&]() -> std::optional<VkDeviceMemory> {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = size; ai.memoryTypeIndex = type;
            VkDeviceMemory m{}; auto r = vkAllocateMemory(dev_, &ai, nullptr, &m);
            if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) return std::nullopt;
            ck(r, "vkAllocateMemory"); return m; });
    }
    void vk_free(VkDeviceMemory m) { dm_.release(m); vkFreeMemory(dev_, m, nullptr); }
    // device_memory's evict callback (mu_ held): resident buffers least recently used first. Recorded work is
    // submitted first (it may read them), device-only data is copied back to the host, then the buffer goes.
    // Not while the op in progress holds staged scratch buffers (a submit frees those).
    std::uint64_t evict_device(std::uint64_t want, bool external) {
        if (external) begin_op();
        else if (scratch_.size() > op_scratch_) return 0;
        std::vector<std::pair<std::uint64_t, std::uintptr_t>> vs;
        for (auto& [k, e] : res_) if (e.last <= op_mark_) vs.push_back({e.last, k});
        std::sort(vs.begin(), vs.end());
        std::size_t n = 0; std::uint64_t got = 0;
        while (n < vs.size() && (!want || got < want)) got += std::max<std::size_t>(4, res_[vs[n++].second].count * 4);
        if (!n) return 0;
        submit_locked();
        for (std::size_t i = 0; i < n; ++i) {
            auto& e = res_[vs[i].second];
            if (!e.host_valid) { std::memcpy(e.host, e.map, e.count * 4); ++stats_.downloads; }
            free_entry(e); res_.erase(vs[i].second);
        }
        stats_.evictions += n; stats_.evicted_bytes += got;
        return got;
    }
    bool overlaps_any(const float* p, std::size_t n) {
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + n * 4;
        auto it = res_.lower_bound(hi);
        if (it == res_.begin()) return false;
        --it; return it->first + it->second.count * 4 > lo;
    }
    template<class F> void for_overlapping(const float* p, std::size_t n, F f) {
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + n * 4;
        auto it = res_.lower_bound(lo);
        if (it != res_.begin()) { auto pr = std::prev(it); if (pr->first + pr->second.count * 4 > lo) it = pr; }
        for (; it != res_.end() && it->first < hi; ++it) f(it->second);
    }
    void submit_locked() {
        if (!cb_) return;
        ck(vkEndCommandBuffer(cb_), "vkEndCommandBuffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; ck(vkCreateFence(dev_, &fi, nullptr, &f), "vkCreateFence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb_;
        ck(vkQueueSubmit(q_, 1, &si, f), "vkQueueSubmit");
        ck(vkWaitForFences(dev_, 1, &f, VK_TRUE, ~0ull), "vkWaitForFences");
        vkDestroyFence(dev_, f, nullptr); vkFreeCommandBuffers(dev_, pool_, 1, &cb_); cb_ = nullptr;
        for (auto dp : pending_pools_) vkDestroyDescriptorPool(dev_, dp, nullptr);
        pending_pools_.clear(); ++stats_.submits; recorded_ = 0;
        for (auto& e : scratch_) free_entry(e); scratch_.clear(); op_scratch_ = 0;
    }
    // Find or create the resident buffer for exactly [h.data, h.data+h.count). An overlapping entry of a
    // different extent (arena slot reuse, a view into a bigger buffer) is synced to the host and dropped first.
    // parent/poff: a staged copy of a sub-range whose offset breaks minStorageBufferOffsetAlignment
    struct bound { entry* e; VkDeviceSize off, range; entry* parent = nullptr; VkDeviceSize poff = 0; };
    bound acquire(const hbuf& h, bool is_out) {
        auto key = reinterpret_cast<std::uintptr_t>(h.data);
        VkDeviceSize range = std::max<VkDeviceSize>(4, h.count * 4);
        auto it = res_.find(key);
        if (it != res_.end() && it->second.count == h.count) { it->second.last = ++clock_; return {&it->second, 0, range}; }
        // a sub-range of a bigger resident buffer (e.g. the live prefix of a KV-cache slot): bind at an offset
        it = res_.upper_bound(key);
        if (it != res_.begin()) {
            auto& c = std::prev(it)->second;
            auto cl = reinterpret_cast<std::uintptr_t>(c.host);
            if (cl + c.count * 4 >= key + h.count * 4 && h.count) {
                c.last = ++clock_;
                if ((key - cl) % align_ == 0) return {&c, key - cl, range};
                // Misaligned view (e.g. 256-byte SwiftShader limit): stage it through a scratch buffer with
                // device-side copies instead of dropping the parent, which would force a submit + host sync.
                scratch_.push_back(make_buffer_(h.count));
                auto* sc = &scratch_.back();
                if (c.host_valid) { std::memcpy(sc->map, h.data, h.count * 4); stats_.upload_bytes += h.count * 4; }
                else { open_cb(); VkBufferCopy r{key - cl, 0, h.count * 4}; vkCmdCopyBuffer(cb_, c.b, sc->b, 1, &r); ++recorded_; }
                return {sc, 0, range, &c, key - cl};
            }
        }
        bool stale = false;
        for_overlapping(h.data, std::max<std::size_t>(h.count, 1), [&](entry& e) { if (!e.host_valid) stale = true; });
        if (stale) submit_locked();
        for (;;) {
            bool any = false;
            for_overlapping(h.data, std::max<std::size_t>(h.count, 1), [&](entry& e) {
                if (!any) { if (!e.host_valid) { std::memcpy(e.host, e.map, e.count * 4); ++stats_.downloads; } any = true; key_ = reinterpret_cast<std::uintptr_t>(e.host); } });
            if (!any) break;
            if (cb_) submit_locked();   // the dropped buffer may still be referenced by recorded work
            free_entry(res_[key_]); res_.erase(key_);
        }
        entry e{h.data, h.count};
        VkDeviceSize sz = std::max<VkDeviceSize>(4, h.count * 4);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = sz; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        ck(vkCreateBuffer(dev_, &bi, nullptr, &e.b), "vkCreateBuffer");
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev_, e.b, &mr);
        try { e.m = vk_alloc(mr.size, memtype(mr.memoryTypeBits, true), "resident"); } catch (...) { vkDestroyBuffer(dev_, e.b, nullptr); throw; }
        ck(vkBindBufferMemory(dev_, e.b, e.m, 0), "vkBindBufferMemory");
        ck(vkMapMemory(dev_, e.m, 0, sz, 0, &e.map), "vkMapMemory");
        e.last = ++clock_;
        for (auto [pp, pn] : pinned_) if (h.data >= pp && h.data + h.count <= pp + pn) e.pinned = true;
        (void)is_out; { std::memcpy(e.map, h.data, h.count * 4); ++stats_.uploads; stats_.upload_bytes += h.count * 4; }
        stats_.resident_bytes += h.count * 4; stats_.peak_resident_bytes = std::max(stats_.peak_resident_bytes, stats_.resident_bytes);
        return {&res_.emplace(reinterpret_cast<std::uintptr_t>(h.data), e).first->second, 0, range};
    }
    entry make_buffer_(std::size_t count) {
        entry e{nullptr, count};
        VkDeviceSize sz = std::max<VkDeviceSize>(4, count * 4);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = sz; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        ck(vkCreateBuffer(dev_, &bi, nullptr, &e.b), "vkCreateBuffer");
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev_, e.b, &mr);
        try { e.m = vk_alloc(mr.size, memtype(mr.memoryTypeBits, true), "scratch"); } catch (...) { vkDestroyBuffer(dev_, e.b, nullptr); throw; }
        ck(vkBindBufferMemory(dev_, e.b, e.m, 0), "vkBindBufferMemory");
        ck(vkMapMemory(dev_, e.m, 0, sz, 0, &e.map), "vkMapMemory");
        stats_.resident_bytes += count * 4;
        return e;
    }
    std::deque<entry> scratch_;   // staged misaligned views; freed after the submit that used them
    std::uintptr_t key_ = 0;
    VkDeviceSize align_ = 256;
    void* lib_ = nullptr;
    VkInstance inst_ = nullptr; VkPhysicalDevice phys_ = nullptr; VkDevice dev_ = nullptr; VkQueue q_ = nullptr; VkCommandPool pool_{};
    std::string name_;
    VkPipelineCache pcache_ = VK_NULL_HANDLE; std::string pcache_path_; std::size_t pcache_loaded_ = 0;
    std::mutex mu_;
    std::map<std::string, std::size_t> dispatches_;
    std::map<std::string, pipe> pipes_;
};

// Run one spec op on the device over host views (out.shape must already be the op's output shape).
// Elementwise ops need same-shape inputs; softmax/rms_norm act on the last axis; matmul_t is a[..., m, k] @ b[n, k]^T.
// Returns false if this op/shape/attrs combination has no Vulkan lowering (the caller falls back to the CPU).
inline bool supports(const std::string& op, const std::vector<shape_t>& in, const ops::attrs& a) {
    if (!runtime::has_kernel(op) || in.empty()) return false;
    auto& x = in[0];
    if (op == "softmax") return !x.empty() && (a.num("axis", -1) == -1 || a.num("axis", -1) == double(x.size()) - 1);
    if (op == "rms_norm") return in.size() == 2 && !x.empty() && in[1].size() == 1 && in[1][0] == x.back();
    if (op == "matmul_t") return in.size() == 2 && x.size() >= 2 && in[1].size() == 2 && in[1][1] == x.back();
    if (op == "rope") return in.size() == 2 && x.size() == 3 && x[2] % 2 == 0 && numel(in[1]) == x[0];
    if (op == "gather") return in.size() == 2 && x.size() == 2;
    if (op == "slice") return in.size() == 1 && !x.empty() && a.num("step", 1) == 1;
    if (op == "attention") return in.size() == 3 && x.size() == 3 && in[1].size() == 3 && in[2].size() == 3 && in[1][1] > 0 && x[1] % in[1][1] == 0;
    for (auto& t : in) if (t != x) return false;
    return true;
}
inline bool run_op(runtime& rt, const std::string& op, const std::vector<tensor_view>& in, const ops::attrs& a, tensor_view out) {
    std::vector<shape_t> ss; for (auto& v : in) ss.push_back(v.shape);
    if (!supports(op, ss, a)) return false;
    auto& x = in[0];
    push pc; pc.n = std::uint32_t(out.size());
    std::uint32_t inv = pc.n;
    if (op == "scale") pc.alpha = float(a.num("alpha", 1.0));
    else if (op == "softmax" || op == "rms_norm") {
        pc.cols = std::uint32_t(x.shape.back()); pc.rows = pc.cols ? std::uint32_t(x.size()) / pc.cols : 0; inv = pc.rows;
        if (op == "rms_norm") pc.alpha = float(a.num("eps", 1e-6));
    } else if (op == "matmul_t") {
        pc.k = std::uint32_t(x.shape.back()); pc.rows = std::uint32_t(x.size()) / pc.k; pc.cols = std::uint32_t(in[1].shape[0]); inv = pc.rows * pc.cols;
    } else if (op == "rope") {
        pc.rows = std::uint32_t(x.shape[0]); pc.cols = std::uint32_t(x.shape[1]); pc.k = std::uint32_t(x.shape[2]);
        pc.alpha = float(a.num("base", 10000.0)); pc.a = a.num("neox", 1) != 0; inv = pc.rows * pc.cols * (pc.k / 2);
    } else if (op == "slice") {
        auto ax = std::size_t(ops::norm_axis(i64(a.num("axis", 0)), x.shape.size()));
        i64 inner = 1; for (std::size_t d = ax + 1; d < x.shape.size(); ++d) inner *= x.shape[d];
        i64 st = i64(a.num("start", 0)); if (st < 0) st += x.shape[ax];
        pc.cols = std::uint32_t(x.shape[ax]); pc.k = std::uint32_t(inner); pc.a = std::uint32_t(st); pc.b = std::uint32_t(out.shape[ax]);
    } else if (op == "gather") {
        pc.rows = std::uint32_t(x.shape[0]); pc.cols = std::uint32_t(x.shape[1]);
    } else if (op == "attention") {
        pc.rows = std::uint32_t(x.shape[0]); pc.cols = std::uint32_t(x.shape[1]); pc.k = std::uint32_t(x.shape[2]);
        auto kc = std::uint32_t(a.num("kv_codec_k", 0)), vc = std::uint32_t(a.num("kv_codec_v", 0));
        if (std::uint32_t dd = std::uint32_t(x.shape[2]), dvv = std::uint32_t(a.num("kv_dv", 0)); (kc >= 3u && dd > 256u) || (vc >= 3u && dvv > 256u)) raise(errc::unsupported, "TurboQuant KV on the GPU supports head dims up to 256");
        if (kc >= 5u || vc >= 5u) raise(errc::unsupported, "TurboQuant QJL KV types (tbq4p, tbq3p) run on the CPU only");
        pc.a = std::uint32_t(in[1].shape[0]); pc.b = std::uint32_t(in[1].shape[1]);
        pc.c = vc ? std::uint32_t(a.num("kv_dv", 0)) : std::uint32_t(in[2].shape[2]);   // quantised V: physical last dim is packed
        pc.d = (a.num("causal", 1) != 0 ? 1u : 0u) | (kc << 1) | (vc << 4) | (std::uint32_t(a.num("window", 0)) << 7);
        pc.alpha = float(a.num("scale", 1.0 / std::sqrt(double(pc.k)))); inv = pc.rows * pc.cols;
    }
    std::vector<hbuf> b; for (auto& v : in) b.push_back({v.data, v.size()}); b.push_back({out.data, out.size()});
    if (rt.resident()) rt.run_resident(op, b, pc, inv); else rt.run(op, b, pc, inv);
    return true;
}
inline bool run_op(runtime& rt, const std::string& op, const std::vector<forge::tensor>& in, const ops::attrs& a, forge::tensor& out) {
    auto o = ops::op_from_name(op);
    if (!o) return false;
    std::vector<shape_t> ss; std::vector<tensor_view> vs;
    for (auto& t : in) { ss.push_back(t.shape); vs.push_back({const_cast<float*>(t.data.data()), t.shape}); }
    if (!supports(op, ss, a)) return false;
    out = forge::tensor(ops::infer_shape(ops::info(*o).shape, ss, a));
    return run_op(rt, op, vs, a, out.view());
}

} // namespace forge::gpu::vk
#endif
