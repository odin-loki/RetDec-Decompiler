#pragma once
// Level Zero host runtime (Intel GPUs: Arc, Data Center GPU Max/Flex, integrated Xe; also the backend SYCL/
// oneAPI programs run on). libze_loader is dlopened; types come from the vendored MIT ze_api.h. Kernels are
// the generated OpenCL C compiled offline to SPIR-V (clang -> LLVM IR -> llvm-spirv) at build time; builds
// without llvm-spirv have no image and open() reports that (use FORGE_DEVICE=opencl on the same GPU instead).
//
// HARDWARE-UNTESTED beyond the CPU mock (tests/mock_ze.c). Same contract as gpu/cuda.hpp: run_op, the
// residency protocol, recorded graphs (replayed by re-appending to the immediate command list), multi-device
// by ordinal (all devices of all drivers), host-staged copies between runtimes.
#include <forge/gpu/cuda.hpp>
#include <level-zero/ze_api.h>
#include <forge/core/dl.hpp>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace forge::gpu::ze {

using cuda_hip::sig;
class runtime {
public:
    static int device_count() { runtime p; if (!p.load_lib().empty() || p.f_.init(0) != 0) return 0; return int(p.all_devices().size()); }
    static std::unique_ptr<runtime> open(std::string* why = nullptr, int ordinal = 0) {
        std::unique_ptr<runtime> r(new runtime);
        r->ordinal_ = ordinal;
        std::string e = r->init();
        if (!e.empty()) { if (why) *why = "level_zero: " + e; return nullptr; }
        return r;
    }
    ~runtime() {
        dm_.detach();
        try { std::lock_guard lk(mu_); for (auto& [k, e] : res_) f_.free_(ctx_, e.d); res_.clear(); } catch (...) {}
        for (auto p : keep_) f_.free_(ctx_, p);
        for (auto& [n, k] : kern_) f_.kernel_destroy(k);
        if (mod_) f_.module_destroy(mod_);
        if (list_) f_.list_destroy(list_);
        if (ctx_) f_.ctx_destroy(ctx_);
        if (lib_) dlclose(lib_);
    }
    const char* backend() const { return "level_zero"; }
    int ordinal() const { return ordinal_; }
    const std::string& device_name() const { return name_; }
    const std::string& image_arch() const { static const std::string s = "spirv"; return s; }
    bool has_graphs() const { return true; }
    bool has_streams() const { return true; }
    std::map<std::string, std::size_t> dispatches() { std::lock_guard lk(mu_); return dispatches_; }
    struct mem_info { std::size_t free = 0, total = 0; };
    mem_info memory() { mem_info m; m.total = total_; m.free = total_ > stats_.resident_bytes ? total_ - stats_.resident_bytes : 0; return m; }
    using residency_stats = cuda_hip::runtime::residency_stats;
    residency_stats residency() { std::lock_guard lk(mu_); auto s = stats_; s.resident = res_.size(); return s; }
    mem::budget* memory_pool() { return dm_.pool(); }   // "level_zero:N" (gpu/device_memory.hpp)
    std::uint64_t charged_bytes() { std::lock_guard lk(mu_); return dm_.charged(); }

    void begin_run() { std::lock_guard lk(mu_); in_run_ = true; }
    void end_run() {
        std::lock_guard lk(mu_);
        for (auto it = res_.begin(); it != res_.end();) {
            auto& e = it->second; if (!e.host_valid) download(e);
            if (e.pinned) { ++it; continue; }
            sync(); drop(e); it = res_.erase(it);
        }
        sync(); in_run_ = false;
    }
    bool resident() { std::lock_guard lk(mu_); return in_run_; }
    void pin(const float* p, std::size_t n) { std::lock_guard lk(mu_); pinned_.push_back({p, n}); }
    void to_host(const float* p, std::size_t n) { std::lock_guard lk(mu_); each_overlap(p, n, [&](entry& e) { if (!e.host_valid) download(e); }); sync(); }
    void host_wrote(const float* p, std::size_t n) {
        std::lock_guard lk(mu_);
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + n * 4;
        each_overlap(p, n, [&](entry& e) {
            auto el = reinterpret_cast<std::uintptr_t>(e.host), eh = el + e.count * 4;
            auto a = std::max(lo, el), b = std::min(hi, eh);
            copy(static_cast<char*>(e.d) + (a - el), reinterpret_cast<const void*>(a), b - a);
        });
        sync();
    }
    void release(const float* p, std::size_t n) {
        std::lock_guard lk(mu_);
        std::vector<std::uintptr_t> ks;
        each_overlap(p, n, [&](entry& e) { if (!e.host_valid) download(e); ks.push_back(reinterpret_cast<std::uintptr_t>(e.host)); });
        sync(); for (auto k : ks) { drop(res_[k]); res_.erase(k); }
    }
    void copy_resident(float* src, std::size_t n, float* dst_base, std::size_t dst_count, std::size_t off) {
        std::lock_guard lk(mu_); if (!n) return;
        begin_op();
        acquire(dst_base, dst_count, false);
        void* s = acquire(src, n, false).ptr;
        auto d = acquire(dst_base, dst_count, false);
        d.e->host_valid = false;
        copy(static_cast<char*>(d.ptr) + off * 4, s, n * 4);
    }

    void capture_begin() { std::lock_guard lk(mu_); capturing_ = true; cur_.clear(); }
    std::size_t capture_end() { std::lock_guard lk(mu_); capturing_ = false; graphs_.push_back(std::move(cur_)); cur_.clear(); return graphs_.size() - 1; }
    void graph_launch(std::size_t id) { std::lock_guard lk(mu_); for (auto& l : graphs_.at(id)) enqueue(l); sync(); }

    std::uint64_t device_alloc(std::size_t bytes) { std::lock_guard lk(mu_); begin_op(); return reinterpret_cast<std::uintptr_t>(alloc(bytes, "raw")); }
    void device_free(std::uint64_t p) { std::lock_guard lk(mu_); dfree(reinterpret_cast<void*>(p)); }
    void upload(std::uint64_t d, const void* h, std::size_t bytes) { std::lock_guard lk(mu_); copy(reinterpret_cast<void*>(d), h, bytes); sync(); }
    void download(void* h, std::uint64_t d, std::size_t bytes) { std::lock_guard lk(mu_); copy(h, reinterpret_cast<void*>(d), bytes); sync(); }
    void copy_to_peer(runtime& dst, std::uint64_t dst_ptr, std::uint64_t src_ptr, std::size_t bytes) {
        std::vector<char> t(bytes); download(t.data(), src_ptr, bytes); dst.upload(dst_ptr, t.data(), bytes);
    }

    bool run_op(const std::string& op, const std::vector<tensor_view>& in, const ops::attrs& a, tensor_view out) {
        std::vector<shape_t> ss; for (auto& v : in) ss.push_back(v.shape);
        if (!cuda_hip::supports(op, ss, a)) return false;
        auto L = cuda_hip::kernel_args(op, in, a, out);
        std::lock_guard lk(mu_); begin_op();
        ++dispatches_[op];
        launch l; l.kernel = kernel("forge_" + op); l.global = L.threads;
        std::vector<void*> temps;
        auto free_temps = [&] { for (auto p : temps) dfree(p); };
        try {
            auto buf = [&](const float* h, std::size_t cnt, bool is_out) {
                void* d;
                if (in_run_) { auto b = acquire(const_cast<float*>(h), cnt, is_out); if (is_out) b.e->host_valid = false; d = b.ptr; }
                else { d = alloc(cnt * 4, "scratch"); temps.push_back(d); if (!is_out) copy(d, h, cnt * 4); }
                l.args.push_back({d, 0, false}); return d;
            };
            for (std::size_t i = 0; i < in.size(); ++i) {
                buf(in[i].data, in[i].size(), false);
                if (L.s == sig::rope && i == 1) {
                    void* d = alloc(L.rope_inv.size() * 4, "scratch"); (capturing_ ? keep_ : temps).push_back(d);
                    copy(d, L.rope_inv.data(), L.rope_inv.size() * 4); l.args.push_back({d, 0, false});
                }
            }
            void* od = buf(out.data, out.size(), true);
            for (auto& sc : L.scalars) { arg g{nullptr, sc.u, true}; if (sc.is_float) std::memcpy(&g.v, &sc.f, 4); l.args.push_back(g); }
            if (L.threads) enqueue(l);
            if (capturing_) cur_.push_back(l);
            if (!in_run_) { copy(out.data, od, out.size() * 4); sync(); }
        } catch (...) { sync(); free_temps(); throw; }
        if (!temps.empty()) { sync(); free_temps(); }
        return true;
    }

private:
    runtime() = default;
    struct arg { void* ptr; std::uint32_t v; bool scalar; };
    struct launch { ze_kernel_handle_t kernel = nullptr; std::uint32_t global = 0; std::vector<arg> args; };
    struct entry { float* host; std::size_t count; void* d; bool host_valid = true, pinned = false; std::uint64_t last = 0; };
    struct bound { entry* e; void* ptr; };
    struct fns {
        decltype(&::zeInit) init = nullptr;
        decltype(&::zeDriverGet) drivers = nullptr;
        decltype(&::zeDeviceGet) devices = nullptr;
        decltype(&::zeDeviceGetProperties) props = nullptr;
        decltype(&::zeDeviceGetMemoryProperties) mem_props = nullptr;
        decltype(&::zeContextCreate) ctx_create = nullptr;
        decltype(&::zeContextDestroy) ctx_destroy = nullptr;
        decltype(&::zeCommandListCreateImmediate) list_create = nullptr;
        decltype(&::zeCommandListDestroy) list_destroy = nullptr;
        decltype(&::zeCommandListHostSynchronize) list_sync = nullptr;
        decltype(&::zeCommandListAppendMemoryCopy) memcpy_ = nullptr;
        decltype(&::zeCommandListAppendBarrier) barrier = nullptr;
        decltype(&::zeCommandListAppendLaunchKernel) launch = nullptr;
        decltype(&::zeMemAllocDevice) alloc = nullptr;
        decltype(&::zeMemFree) free_ = nullptr;
        decltype(&::zeModuleCreate) module_create = nullptr;
        decltype(&::zeModuleDestroy) module_destroy = nullptr;
        decltype(&::zeModuleBuildLogGetString) log_get = nullptr;
        decltype(&::zeModuleBuildLogDestroy) log_destroy = nullptr;
        decltype(&::zeKernelCreate) kernel_create = nullptr;
        decltype(&::zeKernelDestroy) kernel_destroy = nullptr;
        decltype(&::zeKernelSetGroupSize) group_size = nullptr;
        decltype(&::zeKernelSetArgumentValue) set_arg = nullptr;
    };
    template<class F> bool sym(F& f, const char* n) { f = reinterpret_cast<F>(dlsym(lib_, n)); return f != nullptr; }
    void ck(ze_result_t r, const char* what) { if (r != ZE_RESULT_SUCCESS) throw std::runtime_error(std::string("level_zero: ") + what + " failed (" + std::to_string(int(r)) + ")"); }
    void sync() { ck(f_.list_sync(list_, UINT64_MAX), "zeCommandListHostSynchronize"); }
    void copy(void* dst, const void* src, std::size_t n) {
        ck(f_.memcpy_(list_, dst, src, n, nullptr, 0, nullptr), "zeCommandListAppendMemoryCopy");
        ck(f_.barrier(list_, nullptr, 0, nullptr), "zeCommandListAppendBarrier");
    }
    // ---- device memory accounting (gpu/device_memory.hpp): pool "level_zero:N" ----
    device_memory dm_;
    std::uint64_t clock_ = 0, op_mark_ = 0;   // buffers touched after begin_op() belong to the op in progress
    void begin_op() { op_mark_ = clock_; }
    void* alloc(std::size_t bytes, const char* tag) {
        bytes = std::max<std::size_t>(4, bytes);
        return dm_.allocate(bytes, tag, [&]() -> std::optional<void*> {
            ze_device_mem_alloc_desc_t d{}; d.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
            void* p = nullptr; auto r = f_.alloc(ctx_, &d, bytes, 64, dev_, &p);
            if (r == ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY || r == ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY) return std::nullopt;
            ck(r, "zeMemAllocDevice"); return p; });
    }
    void dfree(void* p) { dm_.release(p); f_.free_(ctx_, p); }
    // device_memory's evict callback (mu_ held): resident buffers least recently used first, synced to the host
    // first; never one the op in progress has bound, and nothing while a graph is being recorded.
    std::uint64_t evict_device(std::uint64_t want, bool external) {
        if (capturing_) return 0;
        if (external) op_mark_ = clock_;
        std::vector<std::pair<std::uint64_t, std::uintptr_t>> vs;
        for (auto& [k, e] : res_) if (e.last <= op_mark_) vs.push_back({e.last, k});
        std::sort(vs.begin(), vs.end());
        std::size_t n = 0; std::uint64_t got = 0;
        while (n < vs.size() && (!want || got < want)) got += std::max<std::size_t>(4, res_[vs[n++].second].count * 4);
        if (!n) return 0;
        for (std::size_t i = 0; i < n; ++i) { auto& e = res_[vs[i].second]; if (!e.host_valid) download(e); }
        sync();
        for (std::size_t i = 0; i < n; ++i) { drop(res_[vs[i].second]); res_.erase(vs[i].second); }
        stats_.evictions += n; stats_.evicted_bytes += got;
        return got;
    }
    void enqueue(const launch& l) {
        for (std::uint32_t i = 0; i < l.args.size(); ++i) {
            auto& g = l.args[i];
            if (g.scalar) ck(f_.set_arg(l.kernel, i, 4, &g.v), "zeKernelSetArgumentValue");
            else ck(f_.set_arg(l.kernel, i, sizeof(void*), &g.ptr), "zeKernelSetArgumentValue");
        }
        ze_group_count_t gc{(l.global + 63) / 64, 1, 1};
        ck(f_.launch(list_, l.kernel, &gc, nullptr, 0, nullptr), "zeCommandListAppendLaunchKernel");
        ck(f_.barrier(list_, nullptr, 0, nullptr), "zeCommandListAppendBarrier");
    }
    ze_kernel_handle_t kernel(const std::string& n) {
        auto it = kern_.find(n); if (it != kern_.end()) return it->second;
        ze_kernel_desc_t d{}; d.stype = ZE_STRUCTURE_TYPE_KERNEL_DESC; d.pKernelName = n.c_str();
        ze_kernel_handle_t k = nullptr; ck(f_.kernel_create(mod_, &d, &k), ("zeKernelCreate " + n).c_str());
        ck(f_.group_size(k, 64, 1, 1), "zeKernelSetGroupSize");
        return kern_[n] = k;
    }
    template<class F> void each_overlap(const float* p, std::size_t n, F&& f) {
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + std::max<std::size_t>(n, 1) * 4;
        for (auto& [k, e] : res_) { auto el = k, eh = k + e.count * 4; if (el < hi && lo < eh) f(e); }
    }
    void download(entry& e) { copy(e.host, e.d, e.count * 4); e.host_valid = true; ++stats_.downloads; }
    void drop(entry& e) { dfree(e.d); stats_.resident_bytes -= e.count * 4; }
    bound acquire(float* h, std::size_t count, bool is_out) {
        auto key = reinterpret_cast<std::uintptr_t>(h);
        if (auto it = res_.find(key); it != res_.end() && it->second.count == count) { it->second.last = ++clock_; return {&it->second, it->second.d}; }
        auto it = res_.upper_bound(key);
        if (it != res_.begin()) {
            auto& c = std::prev(it)->second; auto cl = std::prev(it)->first;
            if (cl + c.count * 4 >= key + count * 4 && count) { c.last = ++clock_; return {&c, static_cast<char*>(c.d) + (key - cl)}; }
        }
        std::vector<std::uintptr_t> ks;
        each_overlap(h, count, [&](entry& e) { if (!e.host_valid) download(e); ks.push_back(reinterpret_cast<std::uintptr_t>(e.host)); });
        if (!ks.empty()) sync();
        for (auto k : ks) { drop(res_[k]); res_.erase(k); }
        entry e{h, count, alloc(count * 4, "resident")}; e.last = ++clock_;
        for (auto& [p, n] : pinned_) if (p == h && n == count) e.pinned = true;
        if (!is_out) { copy(e.d, h, count * 4); ++stats_.uploads; }
        stats_.resident_bytes += count * 4; stats_.peak_resident_bytes = std::max(stats_.peak_resident_bytes, stats_.resident_bytes);
        auto& r = res_.emplace(key, e).first->second; return {&r, r.d};
    }
    std::string load_lib() {
        for (auto p : {"libze_loader.so.1", "libze_loader.so", "ze_loader.dll"}) if ((lib_ = dlopen(p, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib_) return "libze_loader not found (no Level Zero runtime)";
        bool ok = sym(f_.init, "zeInit") && sym(f_.drivers, "zeDriverGet") && sym(f_.devices, "zeDeviceGet") &&
                  sym(f_.props, "zeDeviceGetProperties") && sym(f_.mem_props, "zeDeviceGetMemoryProperties") &&
                  sym(f_.ctx_create, "zeContextCreate") && sym(f_.ctx_destroy, "zeContextDestroy") &&
                  sym(f_.list_create, "zeCommandListCreateImmediate") && sym(f_.list_destroy, "zeCommandListDestroy") &&
                  sym(f_.list_sync, "zeCommandListHostSynchronize") && sym(f_.memcpy_, "zeCommandListAppendMemoryCopy") &&
                  sym(f_.barrier, "zeCommandListAppendBarrier") && sym(f_.launch, "zeCommandListAppendLaunchKernel") &&
                  sym(f_.alloc, "zeMemAllocDevice") && sym(f_.free_, "zeMemFree") && sym(f_.module_create, "zeModuleCreate") &&
                  sym(f_.module_destroy, "zeModuleDestroy") && sym(f_.log_get, "zeModuleBuildLogGetString") &&
                  sym(f_.log_destroy, "zeModuleBuildLogDestroy") && sym(f_.kernel_create, "zeKernelCreate") &&
                  sym(f_.kernel_destroy, "zeKernelDestroy") && sym(f_.group_size, "zeKernelSetGroupSize") &&
                  sym(f_.set_arg, "zeKernelSetArgumentValue");
        return ok ? std::string{} : "libze_loader is missing entry points";
    }
    std::vector<std::pair<ze_driver_handle_t, ze_device_handle_t>> all_devices() {
        std::uint32_t nd = 0; if (f_.drivers(&nd, nullptr) != ZE_RESULT_SUCCESS || !nd) return {};
        std::vector<ze_driver_handle_t> ds(nd); f_.drivers(&nd, ds.data());
        std::vector<std::pair<ze_driver_handle_t, ze_device_handle_t>> r;
        for (auto d : ds) {
            std::uint32_t n = 0; if (f_.devices(d, &n, nullptr) != ZE_RESULT_SUCCESS || !n) continue;
            std::vector<ze_device_handle_t> v(n); f_.devices(d, &n, v.data());
            for (auto x : v) r.push_back({d, x});
        }
        return r;
    }
    std::string init() {
        // FORGE_ZE_TEST_IMAGE=opencl-c hands the OpenCL C source to zeModuleCreate instead (only the CPU mock accepts it).
        const char* tst = std::getenv("FORGE_ZE_TEST_IMAGE");
        bool src = tst && std::string(tst) == "opencl-c";
        const void* img = src ? static_cast<const void*>(cuda_hip::images::opencl_c) : cuda_hip::images::spirv_l0;
        std::size_t img_size = src ? cuda_hip::images::opencl_c_size : cuda_hip::images::spirv_l0_size;
        if (!img_size) return "this build has no SPIR-V kernel image (llvm-spirv was not found at build time)";
        if (auto e = load_lib(); !e.empty()) return e;
        if (f_.init(ZE_INIT_FLAG_GPU_ONLY) != ZE_RESULT_SUCCESS) return "zeInit failed";
        auto ds = all_devices();
        if (ds.empty()) return "no devices";
        if (ordinal_ < 0 || ordinal_ >= int(ds.size())) return "device ordinal " + std::to_string(ordinal_) + " out of range (" + std::to_string(ds.size()) + " devices)";
        auto [drv, dev] = ds[std::size_t(ordinal_)]; dev_ = dev;
        ze_device_properties_t p{}; p.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
        if (f_.props(dev_, &p) == ZE_RESULT_SUCCESS) name_ = p.name;
        std::uint32_t nm = 1; ze_device_memory_properties_t mp{}; mp.stype = ZE_STRUCTURE_TYPE_DEVICE_MEMORY_PROPERTIES;
        if (f_.mem_props(dev_, &nm, &mp) == ZE_RESULT_SUCCESS && nm) total_ = mp.totalSize;
        ze_context_desc_t cd{}; cd.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
        if (f_.ctx_create(drv, &cd, &ctx_) != ZE_RESULT_SUCCESS) return "zeContextCreate failed";
        ze_command_queue_desc_t qd{}; qd.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC; qd.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
        if (f_.list_create(ctx_, dev_, &qd, &list_) != ZE_RESULT_SUCCESS) return "zeCommandListCreateImmediate failed";
        ze_module_desc_t md{}; md.stype = ZE_STRUCTURE_TYPE_MODULE_DESC; md.format = ZE_MODULE_FORMAT_IL_SPIRV;
        md.inputSize = img_size; md.pInputModule = static_cast<const std::uint8_t*>(img); md.pBuildFlags = "";
        ze_module_build_log_handle_t log = nullptr;
        if (f_.module_create(ctx_, dev_, &md, &mod_, &log) != ZE_RESULT_SUCCESS) {
            std::string s; std::size_t n = 0;
            if (log && f_.log_get(log, &n, nullptr) == ZE_RESULT_SUCCESS && n) { s.resize(n); f_.log_get(log, &n, s.data()); }
            if (log) f_.log_destroy(log);
            mod_ = nullptr; return "zeModuleCreate failed: " + s;
        }
        if (log) f_.log_destroy(log);
        dm_.attach("level_zero:" + std::to_string(ordinal_), mu_, [this](std::uint64_t w, bool ext) { return evict_device(w, ext); });
        return {};
    }
    int ordinal_ = 0;
    void* lib_ = nullptr;
    ze_device_handle_t dev_ = nullptr; ze_context_handle_t ctx_ = nullptr; ze_command_list_handle_t list_ = nullptr; ze_module_handle_t mod_ = nullptr;
    fns f_;
    std::string name_;
    std::size_t total_ = 0;
    std::mutex mu_;
    bool in_run_ = false, capturing_ = false;
    std::map<std::uintptr_t, entry> res_;
    std::vector<std::pair<const float*, std::size_t>> pinned_;
    std::vector<launch> cur_;
    std::vector<std::vector<launch>> graphs_;
    std::vector<void*> keep_;
    std::map<std::string, ze_kernel_handle_t> kern_;
    std::map<std::string, std::size_t> dispatches_;
    residency_stats stats_;
};

} // namespace forge::gpu::ze
