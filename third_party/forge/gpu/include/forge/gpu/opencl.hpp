#pragma once
// OpenCL host runtime: Intel (incl. the GPUs Level Zero/SYCL target), AMD, NVIDIA, Arm Mali, Qualcomm Adreno,
// Apple (deprecated) and CPU runtimes such as PoCL. libOpenCL is dlopened, so there is no SDK dependency. The
// kernels are the generated OpenCL C (ops/gen/gpu_gen.py -> kernels.cl, same bodies as kernels.cu), embedded
// and built by the driver at open().
//
// HARDWARE-UNTESTED beyond the CPU mock (tests/mock_opencl.c, which compiles kernels.cl as C and runs it).
// Same contract as gpu/cuda.hpp: run_op over host views, the residency protocol (begin_run/end_run/pin/
// to_host/host_wrote/release/copy_resident), recorded "graphs" (captured enqueues replayed with the same
// arguments; OpenCL has no portable graph object), multiple devices by ordinal, copies between runtimes.
#include <forge/gpu/cuda.hpp>   // kernel_sig / supports / images
#include <forge/core/dl.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace forge::gpu::ocl {

using cuda_hip::sig;
class runtime {
public:
    static int device_count() { runtime p; if (!p.load_lib().empty()) return 0; return int(p.all_devices().size()); }
    static std::unique_ptr<runtime> open(std::string* why = nullptr, int ordinal = 0) {
        std::unique_ptr<runtime> r(new runtime);
        r->ordinal_ = ordinal;
        std::string e = r->init();
        if (!e.empty()) { if (why) *why = "opencl: " + e; return nullptr; }
        return r;
    }
    ~runtime() {
        dm_.detach();
        try { std::lock_guard lk(mu_); for (auto& [k, e] : res_) drop(e); res_.clear(); } catch (...) {}
        for (auto m : keep_) f_.rel_mem(m);
        for (auto& [n, k] : kern_) f_.rel_kernel(k);
        if (prog_) f_.rel_program(prog_);
        if (q_) f_.rel_queue(q_);
        if (ctx_) f_.rel_context(ctx_);
        if (lib_) dlclose(lib_);
    }
    const char* backend() const { return "opencl"; }
    int ordinal() const { return ordinal_; }
    const std::string& device_name() const { return name_; }
    const std::string& image_arch() const { static const std::string s = "opencl-c"; return s; }
    bool has_graphs() const { return true; }
    bool has_streams() const { return true; }
    std::map<std::string, std::size_t> dispatches() { std::lock_guard lk(mu_); return dispatches_; }
    struct mem_info { std::size_t free = 0, total = 0; };
    mem_info memory() { mem_info m; m.total = total_mem_; m.free = total_mem_ > stats_.resident_bytes ? total_mem_ - stats_.resident_bytes : 0; return m; }
    using residency_stats = cuda_hip::runtime::residency_stats;
    residency_stats residency() { std::lock_guard lk(mu_); auto s = stats_; s.resident = res_.size(); return s; }
    mem::budget* memory_pool() { return dm_.pool(); }   // "opencl:N" (gpu/device_memory.hpp)
    std::uint64_t charged_bytes() { std::lock_guard lk(mu_); return dm_.charged(); }

    void begin_run() { std::lock_guard lk(mu_); in_run_ = true; }
    void end_run() {
        std::lock_guard lk(mu_);
        for (auto it = res_.begin(); it != res_.end();) {
            auto& e = it->second; if (!e.host_valid) download(e);
            if (e.pinned) { ++it; continue; }
            drop(e); it = res_.erase(it);
        }
        finish(); in_run_ = false;
    }
    bool resident() { std::lock_guard lk(mu_); return in_run_; }
    void pin(const float* p, std::size_t n) { std::lock_guard lk(mu_); pinned_.push_back({p, n}); }
    void to_host(const float* p, std::size_t n) { std::lock_guard lk(mu_); each_overlap(p, n, [&](entry& e) { if (!e.host_valid) download(e); }); finish(); }
    void host_wrote(const float* p, std::size_t n) {
        std::lock_guard lk(mu_);
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + n * 4;
        each_overlap(p, n, [&](entry& e) {
            auto el = reinterpret_cast<std::uintptr_t>(e.host), eh = el + e.count * 4;
            auto a = std::max(lo, el), b = std::min(hi, eh);
            ck(f_.write(q_, e.mem, 1, a - el, b - a, reinterpret_cast<const void*>(a), 0, nullptr, nullptr), "clEnqueueWriteBuffer");
        });
    }
    void release(const float* p, std::size_t n) {
        std::lock_guard lk(mu_);
        std::vector<std::uintptr_t> ks;
        each_overlap(p, n, [&](entry& e) { if (!e.host_valid) download(e); ks.push_back(reinterpret_cast<std::uintptr_t>(e.host)); });
        finish(); for (auto k : ks) { drop(res_[k]); res_.erase(k); }
    }
    void copy_resident(float* src, std::size_t n, float* dst_base, std::size_t dst_count, std::size_t off) {
        std::lock_guard lk(mu_); if (!n) return;
        begin_op();
        acquire(dst_base, dst_count, false);
        auto s = acquire(src, n, false);
        auto d = acquire(dst_base, dst_count, false);
        d.e->host_valid = false;
        ck(f_.copy(q_, s.e->mem, d.e->mem, s.off, d.off + off * 4, n * 4, 0, nullptr, nullptr), "clEnqueueCopyBuffer");
    }

    // ---- recorded graphs ----
    void capture_begin() { std::lock_guard lk(mu_); capturing_ = true; cur_.clear(); }
    std::size_t capture_end() { std::lock_guard lk(mu_); capturing_ = false; graphs_.push_back(std::move(cur_)); cur_.clear(); return graphs_.size() - 1; }
    void graph_launch(std::size_t id) {
        std::lock_guard lk(mu_);
        for (auto& l : graphs_.at(id)) enqueue(l);
        finish();
    }

    // ---- raw device memory (multi-device / pipeline split) ----
    std::uint64_t device_alloc(std::size_t bytes) {
        std::lock_guard lk(mu_); begin_op();
        return reinterpret_cast<std::uintptr_t>(dalloc(bytes, "raw"));
    }
    void device_free(std::uint64_t p) { std::lock_guard lk(mu_); dfree(reinterpret_cast<void*>(p)); }
    void upload(std::uint64_t d, const void* h, std::size_t bytes) { std::lock_guard lk(mu_); ck(f_.write(q_, reinterpret_cast<void*>(d), 1, 0, bytes, h, 0, nullptr, nullptr), "clEnqueueWriteBuffer"); }
    void download(void* h, std::uint64_t d, std::size_t bytes) { std::lock_guard lk(mu_); ck(f_.read(q_, reinterpret_cast<void*>(d), 1, 0, bytes, h, 0, nullptr, nullptr), "clEnqueueReadBuffer"); }
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
        auto free_temps = [&] { for (auto m : temps) dfree(m); };
        try {
            void* outm = nullptr;
            struct { void* tmp; void* dst; std::size_t off, bytes; } writeback{nullptr, nullptr, 0, 0};
            auto buf = [&](const float* h, std::size_t cnt, bool is_out) {
                void* m;
                if (in_run_) {
                    auto b = acquire(const_cast<float*>(h), cnt, is_out); if (is_out) b.e->host_valid = false;
                    if (b.off % align_ == 0) m = b.off ? sub(b) : b.e->mem;
                    else {   // unaligned sub-range: stage through a temporary, copied device-side (and back for the output)
                        m = dalloc(b.bytes, "scratch"); temps.push_back(m);
                        if (!is_out) ck(f_.copy(q_, b.e->mem, m, b.off, 0, b.bytes, 0, nullptr, nullptr), "clEnqueueCopyBuffer");
                        else writeback = {m, b.e->mem, b.off, b.bytes};
                    }
                } else {
                    m = dalloc(cnt * 4, "scratch"); temps.push_back(m);
                    if (!is_out) ck(f_.write(q_, m, 0, 0, cnt * 4, h, 0, nullptr, nullptr), "clEnqueueWriteBuffer");
                }
                l.args.push_back(arg::mem(m)); return m;
            };
            for (std::size_t i = 0; i < in.size(); ++i) {
                buf(in[i].data, in[i].size(), false);
                if (L.s == sig::rope && i == 1) {   // inverse-frequency table goes after pos
                    void* m = dalloc(L.rope_inv.size() * 4, "scratch", 0x20 /*COPY_HOST_PTR*/, L.rope_inv.data());
                    (capturing_ ? keep_ : temps).push_back(m); l.args.push_back(arg::mem(m));
                }
            }
            outm = buf(out.data, out.size(), true);
            for (auto& sc : L.scalars) l.args.push_back(sc.is_float ? arg::f32(sc.f) : arg::u32(sc.u));
            if (L.threads) enqueue(l);
            if (capturing_) cur_.push_back(l);
            if (writeback.tmp) ck(f_.copy(q_, writeback.tmp, writeback.dst, 0, writeback.off, writeback.bytes, 0, nullptr, nullptr), "clEnqueueCopyBuffer");
            if (!in_run_) ck(f_.read(q_, outm, 1, 0, out.size() * 4, out.data, 0, nullptr, nullptr), "clEnqueueReadBuffer");
        } catch (...) { finish(); free_temps(); throw; }
        if (!temps.empty()) { finish(); free_temps(); }
        return true;
    }

private:
    runtime() = default;
    struct arg {
        std::variant<void*, std::uint32_t, float> v;
        static arg mem(void* m) { return {m}; } static arg u32(std::uint32_t u) { return {u}; } static arg f32(float f) { return {f}; }
    };
    struct launch { void* kernel = nullptr; std::size_t global = 0; std::vector<arg> args; };
    struct entry { float* host; std::size_t count; void* mem; bool host_valid = true, pinned = false; std::uint64_t last = 0; };
    struct bound { entry* e; std::size_t off; std::size_t bytes; };
    struct fns {
        int (*platforms)(unsigned, void**, unsigned*) = nullptr;
        int (*devices)(void*, std::uint64_t, unsigned, void**, unsigned*) = nullptr;
        int (*dev_info)(void*, unsigned, std::size_t, void*, std::size_t*) = nullptr;
        void* (*create_context)(const std::intptr_t*, unsigned, void* const*, void*, void*, int*) = nullptr;
        void* (*create_queue)(void*, void*, std::uint64_t, int*) = nullptr;
        void* (*program_src)(void*, unsigned, const char**, const std::size_t*, int*) = nullptr;
        int (*build)(void*, unsigned, void* const*, const char*, void*, void*) = nullptr;
        int (*build_info)(void*, void*, unsigned, std::size_t, void*, std::size_t*) = nullptr;
        void* (*create_kernel)(void*, const char*, int*) = nullptr;
        int (*set_arg)(void*, unsigned, std::size_t, const void*) = nullptr;
        int (*ndrange)(void*, void*, unsigned, const std::size_t*, const std::size_t*, const std::size_t*, unsigned, const void*, void*) = nullptr;
        void* (*create_buffer)(void*, std::uint64_t, std::size_t, void*, int*) = nullptr;
        void* (*sub_buffer)(void*, std::uint64_t, unsigned, const void*, int*) = nullptr;
        int (*write)(void*, void*, unsigned, std::size_t, std::size_t, const void*, unsigned, const void*, void*) = nullptr;
        int (*read)(void*, void*, unsigned, std::size_t, std::size_t, void*, unsigned, const void*, void*) = nullptr;
        int (*copy)(void*, void*, void*, std::size_t, std::size_t, std::size_t, unsigned, const void*, void*) = nullptr;
        int (*finish)(void*) = nullptr;
        int (*rel_mem)(void*) = nullptr;
        int (*rel_kernel)(void*) = nullptr;
        int (*rel_program)(void*) = nullptr;
        int (*rel_queue)(void*) = nullptr;
        int (*rel_context)(void*) = nullptr;
    };
    template<class F> bool sym(F& f, const char* n) { f = reinterpret_cast<F>(dlsym(lib_, n)); return f != nullptr; }
    void ck(int r, const char* what) { if (r != 0) throw std::runtime_error(std::string("opencl: ") + what + " failed (" + std::to_string(r) + ")"); }
    void finish() { ck(f_.finish(q_), "clFinish"); }
    // ---- device memory accounting (gpu/device_memory.hpp): pool "opencl:N" ----
    device_memory dm_;
    std::uint64_t clock_ = 0, op_mark_ = 0;   // buffers touched after begin_op() belong to the op in progress
    void begin_op() { op_mark_ = clock_; }
    void* dalloc(std::size_t bytes, const char* tag, std::uint64_t flags = 0, void* host = nullptr) {
        bytes = std::max<std::size_t>(4, bytes);
        return dm_.allocate(bytes, tag, [&]() -> std::optional<void*> {
            int err = 0; void* m = f_.create_buffer(ctx_, 1 /*READ_WRITE*/ | flags, bytes, host, &err);
            if (err == -4 /*MEM_OBJECT_ALLOCATION_FAILURE*/ || err == -6 /*OUT_OF_HOST_MEMORY*/ || err == -5 /*OUT_OF_RESOURCES*/) return std::nullopt;
            ck(err, "clCreateBuffer"); return m; });
    }
    void dfree(void* m) { dm_.release(m); f_.rel_mem(m); }
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
        finish();
        for (std::size_t i = 0; i < n; ++i) { drop(res_[vs[i].second]); res_.erase(vs[i].second); }
        stats_.evictions += n; stats_.evicted_bytes += got;
        return got;
    }
    void enqueue(const launch& l) {
        for (unsigned i = 0; i < l.args.size(); ++i) {
            auto& v = l.args[i].v;
            if (auto m = std::get_if<void*>(&v)) ck(f_.set_arg(l.kernel, i, sizeof(void*), m), "clSetKernelArg");
            else if (auto u = std::get_if<std::uint32_t>(&v)) ck(f_.set_arg(l.kernel, i, 4, u), "clSetKernelArg");
            else ck(f_.set_arg(l.kernel, i, 4, &std::get<float>(v)), "clSetKernelArg");
        }
        std::size_t local = 64, global = (l.global + local - 1) / local * local;
        ck(f_.ndrange(q_, l.kernel, 1, nullptr, &global, &local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel");
    }
    void* kernel(const std::string& n) {
        auto it = kern_.find(n); if (it != kern_.end()) return it->second;
        int err = 0; void* k = f_.create_kernel(prog_, n.c_str(), &err); ck(err, ("clCreateKernel " + n).c_str());
        return kern_[n] = k;
    }
    void* sub(const bound& b) {   // a sub-range view of a resident buffer (origin must be base-aligned)
        auto key = std::make_pair(b.e->mem, b.off);
        if (auto it = subs_.find(key); it != subs_.end()) return it->second;
        std::size_t region[2] = {b.off, b.bytes}; int err = 0;
        void* m = f_.sub_buffer(b.e->mem, 1, 0x1220 /*CL_BUFFER_CREATE_TYPE_REGION*/, region, &err); ck(err, "clCreateSubBuffer");
        return subs_[key] = m;
    }
    template<class F> void each_overlap(const float* p, std::size_t n, F&& f) {
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + std::max<std::size_t>(n, 1) * 4;
        for (auto& [k, e] : res_) { auto el = k, eh = k + e.count * 4; if (el < hi && lo < eh) f(e); }
    }
    void download(entry& e) { ck(f_.read(q_, e.mem, 0, 0, e.count * 4, e.host, 0, nullptr, nullptr), "clEnqueueReadBuffer"); e.host_valid = true; ++stats_.downloads; }
    void drop(entry& e) {
        for (auto it = subs_.begin(); it != subs_.end();) if (it->first.first == e.mem) { f_.rel_mem(it->second); it = subs_.erase(it); } else ++it;
        dfree(e.mem); stats_.resident_bytes -= e.count * 4;
    }
    bound acquire(float* h, std::size_t count, bool is_out) {
        auto key = reinterpret_cast<std::uintptr_t>(h);
        if (auto it = res_.find(key); it != res_.end() && it->second.count == count) { it->second.last = ++clock_; return {&it->second, 0, count * 4}; }
        auto it = res_.upper_bound(key);
        if (it != res_.begin()) {
            auto& c = std::prev(it)->second; auto cl = std::prev(it)->first;
            if (cl + c.count * 4 >= key + count * 4 && count) { c.last = ++clock_; return {&c, key - cl, count * 4}; }
        }
        std::vector<std::uintptr_t> ks;
        each_overlap(h, count, [&](entry& e) { if (!e.host_valid) download(e); ks.push_back(reinterpret_cast<std::uintptr_t>(e.host)); });
        if (!ks.empty()) finish();
        for (auto k : ks) { drop(res_[k]); res_.erase(k); }
        entry e{h, count, dalloc(count * 4, "resident")}; e.last = ++clock_;
        for (auto& [p, n] : pinned_) if (p == h && n == count) e.pinned = true;
        if (!is_out) { ck(f_.write(q_, e.mem, 0, 0, count * 4, h, 0, nullptr, nullptr), "clEnqueueWriteBuffer"); ++stats_.uploads; }
        stats_.resident_bytes += count * 4; stats_.peak_resident_bytes = std::max(stats_.peak_resident_bytes, stats_.resident_bytes);
        auto& r = res_.emplace(key, e).first->second; return {&r, 0, count * 4};
    }
    std::string load_lib() {
        for (auto p : {"libOpenCL.so.1", "libOpenCL.so", "OpenCL.dll", "/System/Library/Frameworks/OpenCL.framework/OpenCL", "/system/vendor/lib64/libOpenCL.so", "/vendor/lib64/libOpenCL.so",
                       "/System/Library/Frameworks/OpenCL.framework/OpenCL", "OpenCL.dll"})
            if ((lib_ = dlopen(p, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib_) return "libOpenCL not found";
        bool ok = sym(f_.platforms, "clGetPlatformIDs") && sym(f_.devices, "clGetDeviceIDs") && sym(f_.dev_info, "clGetDeviceInfo") &&
                  sym(f_.create_context, "clCreateContext") && sym(f_.create_queue, "clCreateCommandQueue") &&
                  sym(f_.program_src, "clCreateProgramWithSource") && sym(f_.build, "clBuildProgram") && sym(f_.build_info, "clGetProgramBuildInfo") &&
                  sym(f_.create_kernel, "clCreateKernel") && sym(f_.set_arg, "clSetKernelArg") && sym(f_.ndrange, "clEnqueueNDRangeKernel") &&
                  sym(f_.create_buffer, "clCreateBuffer") && sym(f_.sub_buffer, "clCreateSubBuffer") && sym(f_.write, "clEnqueueWriteBuffer") &&
                  sym(f_.read, "clEnqueueReadBuffer") && sym(f_.copy, "clEnqueueCopyBuffer") && sym(f_.finish, "clFinish") &&
                  sym(f_.rel_mem, "clReleaseMemObject") && sym(f_.rel_kernel, "clReleaseKernel") && sym(f_.rel_program, "clReleaseProgram") &&
                  sym(f_.rel_queue, "clReleaseCommandQueue") && sym(f_.rel_context, "clReleaseContext");
        return ok ? std::string{} : "libOpenCL is missing entry points";
    }
    // Devices across all platforms; FORGE_OPENCL_TYPE=gpu|cpu|all (default all, GPUs listed first).
    std::vector<void*> all_devices() {
        unsigned np = 0; if (f_.platforms(0, nullptr, &np) != 0 || !np) return {};
        std::vector<void*> ps(np); f_.platforms(np, ps.data(), nullptr);
        const char* t = std::getenv("FORGE_OPENCL_TYPE"); std::string ty = t ? t : "all";
        std::vector<void*> r;
        for (std::uint64_t want : {std::uint64_t(4) /*GPU*/, std::uint64_t(2) /*CPU*/, std::uint64_t(8) /*ACCEL*/}) {
            if ((ty == "gpu" && want != 4) || (ty == "cpu" && want != 2)) continue;
            for (auto p : ps) {
                unsigned nd = 0; if (f_.devices(p, want, 0, nullptr, &nd) != 0 || !nd) continue;
                std::vector<void*> ds(nd); f_.devices(p, want, nd, ds.data(), nullptr); r.insert(r.end(), ds.begin(), ds.end());
            }
        }
        return r;
    }
    std::string init() {
        if (!cuda_hip::images::opencl_c_size) return "this build has no embedded OpenCL C";
        if (auto e = load_lib(); !e.empty()) return e;
        auto ds = all_devices();
        if (ds.empty()) return "no devices";
        if (ordinal_ < 0 || ordinal_ >= int(ds.size())) return "device ordinal " + std::to_string(ordinal_) + " out of range (" + std::to_string(ds.size()) + " devices)";
        dev_ = ds[std::size_t(ordinal_)];
        char nm[256] = {}; f_.dev_info(dev_, 0x102B /*NAME*/, sizeof nm - 1, nm, nullptr); name_ = nm;
        unsigned bits = 0; if (f_.dev_info(dev_, 0x1019 /*MEM_BASE_ADDR_ALIGN*/, 4, &bits, nullptr) == 0 && bits >= 8) align_ = bits / 8;
        if (const char* t = std::getenv("FORGE_OPENCL_TEST_ALIGN")) align_ = std::size_t(std::atoi(t));
        std::uint64_t tm = 0; if (f_.dev_info(dev_, 0x101F /*GLOBAL_MEM_SIZE*/, 8, &tm, nullptr) == 0) total_mem_ = tm;
        int err = 0;
        ctx_ = f_.create_context(nullptr, 1, &dev_, nullptr, nullptr, &err); if (err || !ctx_) return "clCreateContext failed";
        q_ = f_.create_queue(ctx_, dev_, 0, &err); if (err || !q_) return "clCreateCommandQueue failed";
        const char* src = cuda_hip::images::opencl_c; std::size_t len = cuda_hip::images::opencl_c_size;
        prog_ = f_.program_src(ctx_, 1, &src, &len, &err); if (err || !prog_) return "clCreateProgramWithSource failed";
        if (f_.build(prog_, 1, &dev_, "-cl-std=CL1.2", nullptr, nullptr) != 0) {
            std::size_t n = 0; f_.build_info(prog_, dev_, 0x1183 /*BUILD_LOG*/, 0, nullptr, &n);
            std::string log(n, '\0'); if (n) f_.build_info(prog_, dev_, 0x1183, n, log.data(), nullptr);
            return "clBuildProgram failed: " + log;
        }
        dm_.attach("opencl:" + std::to_string(ordinal_), mu_, [this](std::uint64_t w, bool ext) { return evict_device(w, ext); });
        return {};
    }
    int ordinal_ = 0;
    void* lib_ = nullptr; void* dev_ = nullptr; void* ctx_ = nullptr; void* q_ = nullptr; void* prog_ = nullptr;
    fns f_;
    std::string name_;
    std::size_t align_ = 128, total_mem_ = 0;
    std::mutex mu_;
    bool in_run_ = false, capturing_ = false;
    std::map<std::uintptr_t, entry> res_;
    std::map<std::pair<void*, std::size_t>, void*> subs_;
    std::vector<std::pair<const float*, std::size_t>> pinned_;
    std::vector<launch> cur_;
    std::vector<std::vector<launch>> graphs_;
    std::vector<void*> keep_;
    std::map<std::string, void*> kern_;
    std::map<std::string, std::size_t> dispatches_;
    residency_stats stats_;
};

} // namespace forge::gpu::ocl
