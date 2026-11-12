#pragma once
// CUDA / HIP host runtime. The driver library (libcuda / libamdhip64) is dlopened at run time, so FORGE has
// no link or build dependency on either SDK. Kernels come from the same generated single source as the
// compile checks (ops/gen/gpu_gen.py -> kernels.cu), built at FORGE build time into PTX (CUDA, JIT-compiled
// by the driver for the installed GPU) and an AMDGPU code object (HIP), and embedded below.
// Without a driver or a device, open() fails with a reason and callers fall back to the CPU.
//
// Features (HARDWARE-UNTESTED beyond the CPU mock driver in tests/mock_cuda.c):
//   * every generated kernel (elementwise, softmax, rms_norm, matmul_t, rope, gather, slice, attention with
//     int8/fp8 KV codecs);
//   * one stream per runtime, async copies, events for timing;
//   * residency protocol (same contract as the Vulkan backend): between begin_run()/end_run() buffers stay
//     on the device keyed by host address, pinned ranges (weights) survive across runs;
//   * stream capture into a graph and replay (`capture_begin/capture_end/graph_launch`) where the driver
//     exports the graph API;
//   * multiple devices (`open(api, ordinal)`, `device_count`) and peer copies between runtimes;
//   * device memory accounting (gpu/device_memory.hpp): every allocation is charged to the pool "cuda:N" / "hip:N",
//     and resident buffers are evicted least recently used first when an allocation would not fit.
#include <cstdlib>
#include <forge/gpu/gpu_images.gen.hpp>
#include <forge/core/tensor.hpp>
#include <forge/ops/ops.gen.hpp>
#include <forge/core/dl.hpp>
#include <forge/gpu/device_memory.hpp>
#include <algorithm>
#include <atomic>
#include <set>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace forge::gpu::cuda_hip {

enum class api { cuda, hip };

// Process-wide driver call counters for the benchmark harness (forge bench, tools/bench_llm.sh): how many
// kernels are issued (directly or recorded into a captured graph), graphs replayed, copies and syncs. Relaxed
// atomics, so the cost is one uncontended increment per call.
struct call_counters {
    std::atomic<std::uint64_t> launches{0}, captured{0}, graph_launches{0}, dtoh{0}, htod{0}, dtod{0}, syncs{0}, instantiates{0}, node_updates{0}, replayed_runs{0};
    struct snapshot { std::uint64_t launches = 0, captured = 0, graph_launches = 0, dtoh = 0, htod = 0, dtod = 0, syncs = 0, instantiates = 0, node_updates = 0, replayed_runs = 0; };
    snapshot read() const {
        auto r = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
        return {r(launches), r(captured), r(graph_launches), r(dtoh), r(htod), r(dtod), r(syncs), r(instantiates), r(node_updates), r(replayed_runs)};
    }
};
inline call_counters& counters() { static call_counters c; return c; }
inline void count(std::atomic<std::uint64_t>& a) { a.fetch_add(1, std::memory_order_relaxed); }
inline const char* api_name(api a) { return a == api::cuda ? "cuda" : "hip"; }

// Kernels in kernels.cu and how their arguments are laid out.
enum class sig { unary, binary, scale, rows, rms, matmul, rope, gather, slice, attention };
inline bool kernel_sig(const std::string& op, sig& s) {
    static const std::map<std::string, sig> m = [] {
        std::map<std::string, sig> r;
        for (auto n : {"neg", "abs", "sqrt", "rsqrt", "recip", "square", "exp", "exp2", "log", "log2", "tanh", "sigmoid",
                       "silu", "relu", "softplus", "gelu_tanh"}) r[n] = sig::unary;
        for (auto n : {"add", "sub", "mul", "div", "maximum", "minimum", "silu_mul"}) r[n] = sig::binary;
        r["scale"] = sig::scale; r["softmax"] = sig::rows; r["rms_norm"] = sig::rms; r["matmul_t"] = sig::matmul;
        r["rope"] = sig::rope; r["gather"] = sig::gather; r["slice"] = sig::slice; r["attention"] = sig::attention;
        return r; }();
    auto it = m.find(op); if (it == m.end()) return false; s = it->second; return true;
}

inline std::vector<std::pair<std::string, std::string_view>> module_images(api a) {
    std::vector<std::pair<std::string, std::string_view>> r;
    if (a == api::cuda) {   // sm_80 first (tensor-core kernels); a device that rejects it falls back to sm_70
        if (images::ptx_sm80_size) r.push_back({"ptx sm_80", {images::ptx_sm80, images::ptx_sm80_size}});
        if (images::ptx_size) r.push_back({"ptx", {images::ptx, images::ptx_size}});
        if (const char* e = std::getenv("FORGE_CUDA_IMAGE"); e && *e)   // e.g. "ptx" forces the sm_70 image
            std::erase_if(r, [&](const auto& im) { return im.first != e; });
    }
    else for (std::size_t i = 0; i < images::hsaco_count; ++i)
        r.push_back({images::hsaco[i].arch, {reinterpret_cast<const char*>(images::hsaco[i].data), images::hsaco[i].size}});
    return r;
}

// Shape rules for the kernels above (mirrors run_op); false = no CUDA/HIP lowering, use the CPU.
inline bool supports(const std::string& op, const std::vector<shape_t>& in, const ops::attrs& a) {
    sig s; if (!kernel_sig(op, s) || in.empty()) return false;
    auto& x = in[0];
    switch (s) {
    case sig::unary: case sig::scale: return in.size() == 1;
    case sig::binary: return in.size() == 2 && (in[1] == x || numel(in[1]) == numel(x));   // [n] op [1, n]: same elements
    case sig::rows: return !x.empty() && (a.num("axis", -1) == -1 || a.num("axis", -1) == double(x.size()) - 1);
    case sig::rms: return in.size() == 2 && !x.empty() && in[1].size() == 1 && in[1][0] == x.back();
    case sig::matmul: return in.size() == 2 && x.size() >= 2 && in[1].size() == 2 && in[1][1] == x.back();
    case sig::rope: return in.size() == 2 && x.size() == 3 && x[2] % 2 == 0 && numel(in[1]) == x[0];
    case sig::gather: return in.size() == 2 && x.size() == 2;
    case sig::slice: return in.size() == 1 && !x.empty() && a.num("step", 1) == 1;
    case sig::attention: return in.size() == 3 && x.size() == 3 && in[1].size() == 3 && in[2].size() == 3 && in[1][1] > 0 && x[1] % in[1][1] == 0;
    }
    return false;
}

// Broadcast binaries the CUDA/HIP runtime runs with forge_bcast: the smaller input's shape (leading 1s dropped)
// is a suffix of the larger one's. Other GPU backends keep these on the CPU.
inline int bcast_op(const std::string& op, const std::vector<shape_t>& in) {
    int o = op == "add" ? 0 : op == "sub" ? 1 : op == "mul" ? 2 : op == "div" ? 3 : -1;
    if (o < 0 || in.size() != 2 || numel(in[0]) == numel(in[1])) return -1;
    auto strip = [](shape_t s) { while (s.size() > 1 && s[0] == 1) s.erase(s.begin()); return s; };
    shape_t a = strip(in[0]), b = strip(in[1]);
    const shape_t& big = numel(in[0]) > numel(in[1]) ? a : b; const shape_t& sm = numel(in[0]) > numel(in[1]) ? b : a;
    if (sm.size() > big.size() || numel(sm) == 0) return -1;
    for (std::size_t i = 0; i < sm.size(); ++i) if (sm[sm.size() - 1 - i] != big[big.size() - 1 - i]) return -1;
    return o;
}

// Scalar arguments and launch size for a kernel, shared by the OpenCL and Level Zero runtimes (cuda.hpp's own
// run_op packs the same values). Buffers come first in kernel order (inputs, the rope table after pos, output).
struct scalar { bool is_float; std::uint32_t u; float f; };
struct launch_args { sig s{}; std::uint32_t threads = 0; std::vector<scalar> scalars; std::vector<float> rope_inv; };
inline launch_args kernel_args(const std::string& op, const std::vector<tensor_view>& in, const ops::attrs& a, const tensor_view& out) {
    launch_args L; kernel_sig(op, L.s);
    auto& x = in[0];
    std::uint32_t n = std::uint32_t(out.size()); L.threads = n;
    auto U = [&](std::uint32_t v) { L.scalars.push_back({false, v, 0.f}); };
    auto F = [&](float v) { L.scalars.push_back({true, 0, v}); };
    switch (L.s) {
    case sig::unary: case sig::binary: U(n); break;
    case sig::scale: U(n); F(float(a.num("alpha", 1.0))); break;
    case sig::rows: case sig::rms: {
        std::uint32_t cols = std::uint32_t(x.shape.back()), rows = cols ? std::uint32_t(x.size()) / cols : 0;
        L.threads = rows; U(rows); U(cols); if (L.s == sig::rms) F(float(a.num("eps", 1e-6))); break; }
    case sig::matmul: {
        std::uint32_t k = std::uint32_t(x.shape.back()), rows = std::uint32_t(x.size()) / k, cols = std::uint32_t(in[1].shape[0]);
        L.threads = rows * cols; U(rows); U(cols); U(k); break; }
    case sig::rope: {
        std::uint32_t rows = std::uint32_t(x.shape[0]), cols = std::uint32_t(x.shape[1]), k = std::uint32_t(x.shape[2]);
        double base = a.num("base", 10000.0); L.rope_inv.resize(k / 2);
        for (std::uint32_t j = 0; j < k / 2; ++j) L.rope_inv[j] = float(std::pow(base, -2.0 * double(j) / double(k)));
        L.threads = rows * cols * (k / 2); U(rows); U(cols); U(k); U(a.num("neox", 1) != 0); break; }
    case sig::gather: U(n); U(std::uint32_t(x.shape[0])); U(std::uint32_t(x.shape[1])); break;
    case sig::slice: {
        auto ax = std::size_t(ops::norm_axis(i64(a.num("axis", 0)), x.shape.size()));
        i64 inner = 1; for (std::size_t d = ax + 1; d < x.shape.size(); ++d) inner *= x.shape[d];
        i64 st = i64(a.num("start", 0)); if (st < 0) st += x.shape[ax];
        U(n); U(std::uint32_t(x.shape[ax])); U(std::uint32_t(inner)); U(std::uint32_t(st)); U(std::uint32_t(out.shape[ax])); break; }
    case sig::attention: {
        auto kc = std::uint32_t(a.num("kv_codec_k", 0)), vc = std::uint32_t(a.num("kv_codec_v", 0));
        if (std::uint32_t dd = std::uint32_t(x.shape[2]), dvv = std::uint32_t(a.num("kv_dv", 0)); (kc >= 3u && dd > 256u) || (vc >= 3u && dvv > 256u)) raise(errc::unsupported, "TurboQuant KV on the GPU supports head dims up to 256");
        if (kc >= 5u || vc >= 5u) raise(errc::unsupported, "TurboQuant QJL KV types (tbq4p, tbq3p) run on the CPU only");
        std::uint32_t s = std::uint32_t(x.shape[0]), hq = std::uint32_t(x.shape[1]), d = std::uint32_t(x.shape[2]);
        U(s); U(hq); U(d); U(std::uint32_t(in[1].shape[0])); U(std::uint32_t(in[1].shape[1]));
        U(vc ? std::uint32_t(a.num("kv_dv", 0)) : std::uint32_t(in[2].shape[2]));
        U((a.num("causal", 1) != 0 ? 1u : 0u) | (kc << 1) | (vc << 4) | (std::uint32_t(a.num("window", 0)) << 7));
        F(float(a.num("scale", 1.0 / std::sqrt(double(d))))); L.threads = s * hq; break; }
    }
    return L;
}

class runtime {
public:
    static int device_count(api a) {
        runtime probe(a);
        if (!probe.load_lib().empty()) return 0;
        if (probe.f_.init(0) != 0) return 0;
        int n = 0; return probe.f_.count(&n) == 0 ? n : 0;
    }
    static std::unique_ptr<runtime> open(api a, std::string* why = nullptr, int ordinal = 0) {
        std::unique_ptr<runtime> r(new runtime(a));
        r->ordinal_ = ordinal;
        std::string e = r->init();
        if (!e.empty()) { if (why) *why = std::string(api_name(a)) + ": " + e; return nullptr; }
        return r;
    }
    ~runtime() {
        dm_.detach();
        try { std::lock_guard lk(mu_); for (auto& [k, e] : res_) f_.free_(e.d); res_.clear(); for (auto& [k, q] : qres_) if (q.d) f_.free_(q.d); qres_.clear();
            for (auto& [k, d] : pool_) f_.free_(d); pool_.clear(); for (auto& [k, d] : rope_) f_.free_(d); rope_.clear(); if (q81_) f_.free_(q81_); if (q8r_) f_.free_(q8r_); if (sk_) f_.free_(sk_); if (am_) f_.free_(am_); if (att_) f_.free_(att_);
            if (g8_) f_.free_(g8_); if (gs_) f_.free_(gs_); } catch (...) {}
        for (auto& c : segc_) drop_seg(c);
        for (auto& [e, g] : graphs_) { if (f_.gexec_destroy) f_.gexec_destroy(e); if (f_.graph_destroy) f_.graph_destroy(g); }
        if (stage_ && f_.host_free) f_.host_free(stage_);
        if (stream_ && f_.stream_destroy) f_.stream_destroy(stream_);
        if (mod_ && f_.unload) f_.unload(mod_);
        if (ctx_ && f_.ctx_destroy) { f_.ctx_destroy(ctx_); ctx_epoch().fetch_add(1, std::memory_order_acq_rel); }
        if (lib_) dlclose(lib_);
    }
    api which() const { return api_; }
    const char* backend() const { return api_name(api_); }
    int ordinal() const { return ordinal_; }
    const std::string& device_name() const { return name_; }
    const std::string& image_arch() const { return arch_; }
    bool has_graphs() const { return f_.capture_begin && f_.capture_end && f_.graph_inst && f_.graph_launch; }
    bool has_streams() const { return stream_ != nullptr; }
    std::map<std::string, std::size_t> dispatches() { std::lock_guard lk(mu_); return dispatches_; }
    struct mem_info { std::size_t free = 0, total = 0; };
    mem_info memory() { mem_info m; if (f_.mem_info) { bind(); f_.mem_info(&m.free, &m.total); } return m; }
    // pooled_bytes: freed buffers kept for reuse; evictions / evicted_bytes: buffers dropped to fit the device budget
    struct residency_stats { std::size_t uploads = 0, downloads = 0, resident = 0, resident_bytes = 0, peak_resident_bytes = 0, qweight_bytes = 0,
                             pooled_bytes = 0, evictions = 0, evicted_bytes = 0, zero_fills = 0; };   // zero_fills: uploads of all-zero data done as device clears
    residency_stats residency() { std::lock_guard lk(mu_); auto s = stats_; s.resident = res_.size(); s.pooled_bytes = pool_bytes_; return s; }
    // The device memory pool this runtime charges ("cuda:0", ...) and the bytes it has charged to it.
    mem::budget* memory_pool() { return dm_.pool(); }
    std::uint64_t charged_bytes() { std::lock_guard lk(mu_); return dm_.charged(); }

    // ---- residency protocol (see graph::device_hooks) ----
    void begin_run() { std::lock_guard lk(mu_); q81_drop(); in_run_ = true; seg_ = 0; dirty_ = false; }
    void wait() { std::lock_guard lk(mu_); bind(); sync(); }   // block until queued device work finishes (timing)
    // ---- whole-run replay (graph::device_hooks::replay / ran) ----
    // A run whose work was all graph-recorded kernels (no transfers, syncs or host ops inside it) can be replayed
    // without the executor: every segment's exec is relaunched with its kernel arguments advanced by the change
    // seen between the last two runs (decode: the KV write row and the attention length move by one row per
    // token; everything else -- weights, the resident arena, state slots -- sits at fixed addresses). A run only
    // becomes replayable once such a prediction has been confirmed by a real run, and the executor re-checks one
    // real run in every $FORGE_REPLAY_VERIFY; a wrong prediction disables replay for that plan.
    void ran(const void* key, bool clean) {
        std::lock_guard lk(mu_); bind(); flush();
        bool hit = clean && !dirty_ && replay_ok() && gmode() && seg_ > 0 && seg_ <= segc_.size();
        for (std::size_t s = 0; hit && s < seg_; ++s) hit = segc_[s].replay && segc_[s].hit;
        if (key != run_key_) { run_key_ = key; streak_ = 0; }
        if (!hit && replayed_ && !replay_bad_.count(key)) {   // a replayed run predicted something a real run did not do
            replay_bad_.insert(key);
            std::fprintf(stderr, "forge: %s: whole-run replay mispredicted; disabled for this plan\n", api_name(api_));
        }
        streak_ = hit ? streak_ + 1 : 0; replayed_ = false; nseg_ = seg_;
        for (std::size_t s = 0; s < seg_ && s < segc_.size(); ++s) predict(segc_[s]);
    }
    bool replay(const void* key) {
        std::lock_guard lk(mu_);
        if (!(key == run_key_ && streak_ >= 2 && !replay_bad_.count(key) && replay_ok() && gmode() && nseg_ && nseg_ <= segc_.size())) return false;
        bind(); flush();
        std::vector<void*> ap;
        for (std::size_t s = 0; s < nseg_; ++s) {
            auto& c = segc_[s];
            if (!c.replay || !c.exec || c.pred.size() != c.bytes.size()) { streak_ = 0; return false; }
        }
        // Patch and launch segment by segment, so the device starts on the first segment while the host patches the
        // next. A failed patch before the first launch falls back to a real run; after it (work already queued that
        // a real run would repeat) it is an error.
        static const bool eager = [] { auto* e = std::getenv("FORGE_REPLAY_EAGER"); return !(e && std::string(e) == "0"); }();
        auto patch = [&](std::size_t s) {
            auto& c = segc_[s];
            for (std::size_t i = 0; i < c.recs.size(); ++i) {
                auto& r = c.recs[i];
                if (!std::memcmp(c.pred.data() + r.off, c.bytes.data() + r.off, r.len)) continue;
                arg_ptrs(r, c.pred.data(), ap);
                struct { void* fn; unsigned d[7]; void** kp; void** ex; } kp{r.fn, {r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6]}, ap.data(), nullptr};
                if (f_.kn_set(c.exec, c.nodes[i], &kp) != 0) return false;
                cuda_hip::count(counters().node_updates);
            }
            return true;
        };
        auto launch = [&](std::size_t s) {
            auto& c = segc_[s];
            c.prev.swap(c.bytes); c.bytes.swap(c.pred); predict(c);
            cuda_hip::count(counters().graph_launches); ck(f_.graph_launch(c.exec, stream_), "graph launch");
        };
        const std::size_t first = eager ? 1 : nseg_;   // segments patched before anything is launched
        for (std::size_t s = 0; s < first; ++s)
            if (!patch(s)) { for (std::size_t t = 0; t <= s; ++t) drop_seg(segc_[t]); streak_ = 0; return false; }
        for (std::size_t s = 0; s < nseg_; ++s) {
            if (s >= first && !patch(s)) { streak_ = 0; replay_bad_.insert(key); throw std::runtime_error(std::string(api_name(api_)) + ": graph node update failed during replay"); }
            launch(s);
        }
        cuda_hip::count(counters().replayed_runs); replayed_ = true;
        return true;
    }
    void end_run() {
        std::lock_guard lk(mu_); bind();
        for (auto it = res_.begin(); it != res_.end();) {
            auto& e = it->second;
            if (!e.host_valid) download(e);
            if (e.pinned) { ++it; continue; }
            drop(e); it = res_.erase(it);
        }
        sync(); in_run_ = false;
    }
    void end_run_discard() {   // like end_run, but non-pinned intermediates go back to the pool without a download
        std::lock_guard lk(mu_); bind();
        for (auto it = res_.begin(); it != res_.end();) {
            auto& e = it->second;
            if (e.kept) { ++it; continue; }   // stays resident, host copy fetched on demand
            if (e.pinned) { if (!e.host_valid) download(e); ++it; continue; }
            drop(e); it = res_.erase(it);
        }
        sync(); in_run_ = false;
    }
    void keep(const float* p, std::size_t n) { std::lock_guard lk(mu_); each_overlap(p, n, [&](entry& e) { e.kept = true; }); }
    // a plan arena held as one device buffer across runs (graph::device_hooks::arena): intermediates are sub-ranges
    // of it, so they keep their device addresses run after run
    void arena(float* h, std::size_t n) {
        std::lock_guard lk(mu_); bind(); begin_op();
        auto key = reinterpret_cast<std::uintptr_t>(h);
        if (auto it = res_.find(key); it != res_.end() && it->second.count == n && it->second.arena) { it->second.last = ++clock_; return; }
        auto b = acquire(h, n, false); b.e->kept = true; b.e->arena = true;
    }
    void forget(const float* p, std::size_t n) {   // drop device copies without fetching them
        std::lock_guard lk(mu_); bind(); flush();
        std::vector<std::uintptr_t> ks;
        each_overlap(p, n, [&](entry& e) { ks.push_back(reinterpret_cast<std::uintptr_t>(e.host)); });
        for (auto k : ks) { drop(res_[k]); res_.erase(k); }
        std::erase_if(pinned_, [&](const auto& r) { return r.first >= p && r.first + r.second <= p + n; });   // the range is going away
    }
    bool resident() { std::lock_guard lk(mu_); return in_run_; }
    void pin(const float* p, std::size_t n) { std::lock_guard lk(mu_); pinned_.push_back({p, n}); }
    void to_host(const float* p, std::size_t n) {
        std::lock_guard lk(mu_); bind();
        bool any = false;
        each_overlap(p, n, [&](entry& e) {
            if (e.host_valid) return;
            if (!e.arena) { download(e); any = true; return; }
            auto el = reinterpret_cast<std::uintptr_t>(e.host), lo = std::max(el, reinterpret_cast<std::uintptr_t>(p)),
                 hi = std::min(el + e.count * 4, reinterpret_cast<std::uintptr_t>(p) + n * 4);   // just the asked range
            if (hi > lo) { ck(dtoh_s(reinterpret_cast<void*>(lo), e.d + (lo - el), hi - lo, stream_), "memcpy DtoH"); ++stats_.downloads; any = true; }
        });
        if (any) sync();
    }
    // Greedy decode: the argmax of a buffer whose newest copy is on the device, read back as one int (instead of the
    // whole buffer). -1 when it is not wholly inside one device-newer entry, or the image has no forge_argmax.
    long long argmax_resident(const float* p, std::size_t n) {
        std::lock_guard lk(mu_); bind();
        if (!n || n > 0xFFFFFFFFull || !has_fn_locked("forge_argmax")) return -1;
        const auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + n * 4;
        std::uint64_t dp = 0;
        each_overlap(p, n, [&](entry& e) {
            auto el = reinterpret_cast<std::uintptr_t>(e.host);
            if (!e.host_valid && el <= lo && el + e.count * 4 >= hi) dp = e.d + (lo - el); });
        if (!dp) return -1;
        // up to 128 blocks of 256 threads (a lone block took ~30 us over a 248k vocabulary); the blocks leave partials
        // in am_ and the last one merges them (am_[1] is its ticket counter, zero between launches)
        if (!am_) { static const std::uint32_t zero[1024] = {}; am_ = dmalloc(sizeof zero, "scratch"); ck(htod_s(am_, zero, sizeof zero, stream_), "memcpy HtoD"); }
        std::uint32_t n32 = std::uint32_t(n), nb = std::uint32_t(std::min<std::size_t>(128, (n + 2047) / 2048)); void* args[] = {&dp, &n32, &am_, &nb};
        ck(launch_s(function("forge_argmax"), nb, 1, 1, 256, 1, 1, 0, stream_, args, nullptr), "launch");
        ++dispatches_["argmax"];
        int r = -1; void* pr = stage(4, true);   // page-locked: no driver staging on the read-back
        ck(dtoh_s(pr ? pr : &r, am_, 4, stream_), "memcpy DtoH"); sync();
        if (pr) std::memcpy(&r, pr, 4);
        return r;
    }
    void host_wrote(const float* p, std::size_t n) {
        std::lock_guard lk(mu_); bind();
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + n * 4;
        each_overlap(p, n, [&](entry& e) {
            auto el = reinterpret_cast<std::uintptr_t>(e.host), eh = el + e.count * 4;
            auto a = std::max(lo, el), b = std::min(hi, eh);
            ck(htod_or_zero(e.d + (a - el), reinterpret_cast<const void*>(a), b - a, stream_), "memcpy HtoD");
        });
        // No sync: the copy is stream-ordered before later kernels, and a pageable source may be reused once the
        // call returns (the driver has staged it). Only launch any recorded kernels so ordering is kept.
        flush();
    }
    void release(const float* p, std::size_t n) {
        std::lock_guard lk(mu_); bind();
        std::vector<std::uintptr_t> ks;
        each_overlap(p, n, [&](entry& e) { if (!e.host_valid) download(e); ks.push_back(reinterpret_cast<std::uintptr_t>(e.host)); });
        if (!ks.empty()) sync();
        for (auto k : ks) { drop(res_[k]); res_.erase(k); }
    }
    static constexpr bool has_bcast = true;
    bool run_bcast(int op, const std::vector<tensor_view>& in, tensor_view out) {
        std::uint32_t n = std::uint32_t(out.size()), ma = std::uint32_t(in[0].size()), mb = std::uint32_t(in[1].size()), o = std::uint32_t(op);
        if (!ma || !mb || n % ma || n % mb) return false;
        std::lock_guard lk(mu_); bind(); begin_op();
        if (!has_fn_locked("forge_bcast")) return false;
        if (!in_run_) {   // outside a run the views are host memory
            for (std::uint32_t i = 0; i < n; ++i) { float x = in[0].data[i % ma], z = in[1].data[i % mb];
                out.data[i] = o == 0 ? x + z : o == 1 ? x - z : o == 2 ? x * z : x / z; }
            return true;
        }
        ++dispatches_["bcast"];
        auto ad = acquire(in[0].data, in[0].size(), false).ptr;
        auto bd = acquire(in[1].data, in[1].size(), false).ptr;
        ad = acquire(in[0].data, in[0].size(), false).ptr;
        auto y = acquire(out.data, out.size(), true); y.e->host_valid = false; std::uint64_t yd = y.ptr;
        void* args[] = {&ad, &bd, &yd, &n, &ma, &mb, &o};
        writes(yd, std::size_t(n) * 4);
        if (n) ck(launch_s(function("forge_bcast"), (n + 255) / 256, 1, 1, 256, 1, 1, 0, stream_, args, nullptr), "launch");
        return true;
    }
    // dst_base[off + r * ds + c] = src[r * w + c] on the device (concat)
    bool copy2d_resident(float* src, std::size_t sn, float* dst_base, std::size_t dst_count, std::size_t rows, std::size_t w, std::size_t ds, std::size_t off) {
        std::lock_guard lk(mu_); bind(); begin_op();
        if (!has_fn_locked("forge_copy2d")) return false;
        if (!rows || !w) return true;
        ++dispatches_["copy2d"];
        // A write that covers the whole destination (a first prefill filling a KV slot) needs none of its old contents:
        // allocate without uploading the host copy.
        const bool whole = off == 0 && ds == w && rows * w >= dst_count;
        auto d = acquire(dst_base, dst_count, whole);
        auto s = acquire(src, sn, false);
        d = acquire(dst_base, dst_count, false);
        d.e->host_valid = false;
        std::uint64_t sp = s.ptr, dp = d.ptr + off * 4; std::uint32_t r = std::uint32_t(rows), ww = std::uint32_t(w), dss = std::uint32_t(ds);
        void* args[] = {&sp, &dp, &r, &ww, &dss};
        writes(d.ptr, dst_count * 4);
        ck(launch_s(function("forge_copy2d"), unsigned((rows * w + 255) / 256), 1, 1, 256, 1, 1, 0, stream_, args, nullptr), "launch");
        return true;
    }
    void copy_resident(float* src, std::size_t n, float* dst_base, std::size_t dst_count, std::size_t off) {
        std::lock_guard lk(mu_); bind(); begin_op();
        if (!n) return;
        auto d = acquire(dst_base, dst_count, off == 0 && n >= dst_count);   // whole destination overwritten: no upload
        auto s = acquire(src, n, false);
        d = acquire(dst_base, dst_count, false);
        d.e->host_valid = false;
        ck(dtod_s(d.ptr + off * 4, s.ptr, n * 4), "memcpy DtoD");
    }

    // ---- graphs: capture the stream's work between capture_begin/capture_end and replay it ----
    void capture_begin() { std::lock_guard lk(mu_); need_graphs(); bind(); flush(); user_cap_ = true; ck(f_.capture_begin(stream_, 2 /* RELAXED: run_op may allocate an output buffer while capturing */), "stream begin capture"); }
    std::size_t capture_end() { user_cap_ = false;
        std::lock_guard lk(mu_); need_graphs(); bind();
        void* g = nullptr; ck(f_.capture_end(stream_, &g), "stream end capture");
        void* e = nullptr; ck(f_.graph_inst(&e, g, 0), "graph instantiate");
        graphs_.push_back({e, g}); return graphs_.size() - 1;
    }
    void graph_launch(std::size_t id) { std::lock_guard lk(mu_); need_graphs(); bind(); cuda_hip::count(counters().graph_launches); ck(f_.graph_launch(graphs_.at(id).first, stream_), "graph launch"); sync(); }

    // ---- multi-device: copy device memory to another runtime (peer copy where available, else via host) ----
    void copy_to_peer(runtime& dst, std::uint64_t dst_ptr, std::uint64_t src_ptr, std::size_t bytes) {
        if (f_.peer && dst.api_ == api_) {
            if (api_ == api::cuda) ck(f_.peer(dst_ptr, reinterpret_cast<std::uintptr_t>(dst.ctx_), src_ptr, reinterpret_cast<std::uintptr_t>(ctx_), bytes), "memcpy peer");
            else ck(f_.peer(dst_ptr, std::uintptr_t(dst.ordinal_), src_ptr, std::uintptr_t(ordinal_), bytes), "memcpy peer");
            return;
        }
        std::vector<char> tmp(bytes);
        { std::lock_guard lk(mu_); bind(); ck(dtoh_s(tmp.data(), src_ptr, bytes, stream_), "memcpy DtoH"); sync(); }
        std::lock_guard lk(dst.mu_); dst.bind(); dst.ck(dst.htod_s(dst_ptr, tmp.data(), bytes, dst.stream_), "memcpy HtoD"); dst.sync();
    }
    std::uint64_t device_alloc(std::size_t bytes) { std::lock_guard lk(mu_); bind(); begin_op(); return dmalloc(bytes, "raw"); }
    void device_free(std::uint64_t p) { std::lock_guard lk(mu_); bind(); dfree(p); }
    void upload(std::uint64_t d, const void* h, std::size_t bytes) { std::lock_guard lk(mu_); bind(); ck(htod_s(d, h, bytes, stream_), "memcpy HtoD"); sync(); }
    void download(void* h, std::uint64_t d, std::size_t bytes) { std::lock_guard lk(mu_); bind(); ck(dtoh_s(h, d, bytes, stream_), "memcpy DtoH"); sync(); }

    // Quantised matmul_t (forge_qmatmul_t): y[rows, cols] = x[rows, k] . W[cols, k]^T with W in its encoded rows
    // (fmt 1 q8_0, 2 q4_k, 3 q6_k, 4 q5_k; rb bytes per row). The encoded weight is uploaded once and stays on the
    // device for the runtime's lifetime (keyed by host address and size); x/out follow the residency protocol.
    bool has_function(const std::string& n) {
        std::lock_guard lk(mu_); bind();
        if (fn_.count(n)) return true;
        void* f = nullptr; if (f_.get_fn(&f, mod_, n.c_str()) != 0 || !f) return false;
        fn_[n] = f; return true;
    }
    // rows at or above which run_qmatmul uses the tiled MMQ kernel ($FORGE_MMQ_MIN_ROWS, default 16; 0 disables)
    static std::uint32_t mmq_min_rows() {
        static const std::uint32_t v = [] { const char* e = std::getenv("FORGE_MMQ_MIN_ROWS"); if (!e) return 16u; auto n = std::uint32_t(std::strtoul(e, nullptr, 10)); return n ? n : 0xffffffffu; }();
        return v;
    }
    bool has_fn_locked(const std::string& n) {   // caller holds mu_
        if (fn_.count(n)) return true;
        void* f = nullptr; if (f_.get_fn(&f, mod_, n.c_str()) != 0 || !f) return false;
        fn_[n] = f; return true;
    }
    // Embedding gather over an encoded table (forge_qgather); the table is uploaded once, like run_qmatmul's weights.
    bool run_qgather(const tensor_view& idx, const std::uint8_t* q, std::size_t rb, std::uint32_t nrows, std::uint32_t d, std::uint32_t fmt, tensor_view out) {
        if (!q || fmt < 1 || fmt > 4 || !d || d % 32 || out.size() != idx.size() * d) return false;
        std::lock_guard lk(mu_); bind(); begin_op();
        if (!has_fn_locked("forge_qgather")) return false;
        ++dispatches_["qgather"];
        std::size_t qbytes = rb * nrows;
        auto& qe = qres_[{reinterpret_cast<std::uintptr_t>(q), qbytes}]; qe.last = ++clock_;
        if (!qe.d) {
            std::uint64_t dp = dmalloc(qbytes, "weights");
            qe.d = dp; ck(htod_s(qe.d, q, qbytes, stream_), "memcpy HtoD"); ++stats_.uploads; stats_.qweight_bytes += qbytes;
        }
        const std::uint64_t qd = qe.d;
        std::vector<std::uint64_t> temps;
        auto free_temps = [&] { for (auto p : temps) dfree(p); };
        std::uint32_t n = std::uint32_t(idx.size());
        try {
            std::uint64_t xd = 0, yd = 0;
            if (in_run_) {
                xd = acquire(idx.data, idx.size(), false).ptr;
                auto b = acquire(out.data, out.size(), true); b.e->host_valid = false; yd = b.ptr;
            } else {
                xd = dmalloc(idx.size() * 4, "scratch"); temps.push_back(xd);
                ck(htod_s(xd, idx.data, idx.size() * 4, stream_), "memcpy HtoD");
                yd = dmalloc(out.size() * 4, "scratch"); temps.push_back(yd);
            }
            std::uint64_t wd = qd; std::uint32_t rb32 = std::uint32_t(rb);
            void* args[] = {&xd, &wd, &yd, &n, &d, &fmt, &rb32, &nrows};
            if (n) ck(launch_s(function("forge_qgather"), n, 1, 1, 256, 1, 1, 0, stream_, args, nullptr), "launch");
            if (!in_run_) { ck(dtoh_s(out.data, yd, out.size() * 4, stream_), "memcpy DtoH"); sync(); }
        } catch (...) { if (!in_run_) sync(); free_temps(); throw; }
        if (!temps.empty()) { sync(); free_temps(); }
        return true;
    }
    // Prefill on int8 tensor cores (forge_qmatmul_mma, sm_80+ image) for q8_0/q4_k/q6_k ($FORGE_MMA=0 off).
    static bool use_mma() { static const bool on = [] { auto* e = std::getenv("FORGE_MMA"); return !(e && std::string(e) == "0"); }(); return on; }
    static bool use_mmvq() { static const bool on = [] { auto* e = std::getenv("FORGE_MMVQ"); return !(e && std::string(e) == "0"); }(); return on; }
    // Split-K decode attention ($FORGE_ATTN_DEC=0 off) and its partials scratch.
    static bool fa_tc() { static const bool on = [] { auto* e = std::getenv("FORGE_FA_TC"); return !(e && std::string(e) == "0"); }(); return on; }
    static bool attn_dec() { static const bool on = [] { auto* e = std::getenv("FORGE_ATTN_DEC"); return !(e && std::string(e) == "0"); }(); return on; }
    std::uint64_t att_ = 0; std::size_t att_cap_ = 0;
    std::set<void*> mma_attr_;   // kernels (qmatmul_mma_*, attention_fa2_*) whose dynamic shared memory limit was raised
    static std::string mma_fn(std::uint32_t fmt, unsigned bt = 64) {
        return std::string(bt == 128 ? "forge_qmatmul_mma128_" : "forge_qmatmul_mma_") + (fmt == 1 ? "q8_0" : fmt == 6 ? "q8_0g" : fmt == 2 ? "q4_k" : "q6_k"); }
    static unsigned mma_ns() { static const unsigned v = [] { auto* e = std::getenv("FORGE_MMA_STAGES"); return e && std::string(e) == "2" ? 2u : 1u; }(); return v; }   // $FORGE_MMA_STAGES=2: double-buffered q4_k (opt-in; no gain on an RTX 3090)
    static unsigned mma_bt() {   // $FORGE_MMA_BT: 64 or 128 tokens per block; 0 (unset) = per format
        auto* e = std::getenv("FORGE_MMA_BT"); return !e ? 0u : std::string(e) == "64" ? 64u : std::string(e) == "128" ? 128u : 0u; }
    static bool use_splitk() { static const bool on = [] { auto* e = std::getenv("FORGE_MMA_SPLITK"); return !(e && std::string(e) == "0"); }(); return on; }
    // $FORGE_MMA_STREAMK: off by default (greedy output on Qwen3.5 not yet verified against the default path); 1 on for
    // grids of 1/4 to 4 waves, 2 for every grid that does not fill whole waves (tests). Read per launch, so tests and
    // benches compare the paths in one process.
    static unsigned streamk_mode() { auto* e = std::getenv("FORGE_MMA_STREAMK"); return !e || !*e ? 0u : std::string(e) == "1" ? 1u : std::string(e) == "2" ? 2u : 0u; }
    // Stream-K needs >= this many 256-value k chunks per block ($FORGE_MMA_SK_MIN, default 4): fewer blocks otherwise.
    static unsigned sk_min() { auto* e = std::getenv("FORGE_MMA_SK_MIN"); int n = e && *e ? std::atoi(e) : 4; return unsigned(n < 1 ? 1 : n); }
    std::uint64_t skfx_ = 0; std::size_t skfx_cap_ = 0;   // stream-K fixup slots (one bt x 128 float tile per block)
    std::uint64_t skfl_ = 0; static constexpr unsigned skfl_n_ = 4096;   // stream-K flags, zero between launches (never reallocated)
    std::map<void*, unsigned> occ_;   // resident blocks per SM of the mma kernels
    // Persistent stream-K launch for the mma kernels (see mma_body): grid G (<= resident blocks), dp data-parallel
    // tiles. false: the plain one-tile-per-block launch (tiles fill whole waves, stream-K off, or no occupancy figure).
    bool streamk_plan(void* fn, unsigned smem, unsigned tiles, unsigned nck, unsigned bt, unsigned& G, unsigned& dp, std::uint64_t& fx, std::uint64_t& fl) {
        const unsigned mode = streamk_mode();
        if (!mode || !tiles || !has_fn_locked("forge_sum_splits")) return false;   // sum_splits: an sm_80 image of this revision
        auto it = occ_.find(fn);
        if (it == occ_.end()) { int nb = 0; if (!f_.occupancy || f_.occupancy(&nb, fn, 256, smem) != 0) nb = 0; it = occ_.emplace(fn, unsigned(std::max(nb, 0))).first; }
        const unsigned slots = sms() * it->second;
        if (!slots || tiles % slots == 0) return false;
        // RTX 3090, 512 tokens: below 1/4 wave (e.g. 8 tiles, Qwen3.5's 32-column DeltaNet a/b) split-K's parallel sum
        // beats the serial fixup (53.6 vs 63.6 us); from 4 waves on the ragged tail costs about what the stream-K
        // share does (25600x5120 q4_k: 1847 vs 1896 us), so both keep the one-tile-per-block launch.
        if (mode == 1 && (tiles * 4 < slots || tiles >= 4 * slots)) return false;
        const std::uint64_t tot = std::uint64_t(tiles) * nck;
        G = unsigned(std::min<std::uint64_t>(slots, std::max<std::uint64_t>(1, tot / sk_min())));
        if (G > skfl_n_) return false;
        // whole waves but the last one or two stay data-parallel; the rest (G..2G tiles) is shared out by k chunks
        dp = tiles >= 2 * G ? (tiles / G - 1) * G : 0;
        const std::size_t need = std::size_t(G) * bt * 128 * 4;
        if (skfx_cap_ < need) { if (skfx_) free_s(skfx_); skfx_ = 0; skfx_cap_ = 0; std::size_t n = std::max<std::size_t>(need, std::size_t(sms()) << 16); skfx_ = dmalloc(n, "scratch"); skfx_cap_ = n; }
        if (!skfl_) { skfl_ = dmalloc(skfl_n_ * 4, "scratch"); std::vector<std::uint32_t> z(skfl_n_, 0); ck(htod_s(skfl_, z.data(), z.size() * 4, stream_), "memcpy HtoD"); }
        fx = skfx_; fl = skfl_;
        return true;
    }
    std::uint64_t am_ = 0;   // forge_argmax result (one int)
    std::uint64_t sk_ = 0; std::size_t sk_cap_ = 0;   // split-K partial products (stream-ordered reuse)
    void sk_reserve(std::size_t need) {
        if (sk_cap_ >= need) return;
        if (sk_) free_s(sk_); sk_ = 0; sk_cap_ = 0;
        std::size_t n = std::max<std::size_t>(need, 4 << 20); sk_ = dmalloc(n, "scratch"); sk_cap_ = n;
    }
    std::uint64_t q8r_ = 0; std::size_t q8r_cap_ = 0;   // int8 activation rows + per-group (d, d*sum) for the mma path
    void q8r_reserve(std::size_t need) {
        if (q8r_cap_ >= need) return;
        if (q8r_) free_s(q8r_); q8r_ = 0; q8r_cap_ = 0;
        std::size_t n = std::max<std::size_t>(need, 4 << 20); q8r_ = dmalloc(n, "scratch"); q8r_cap_ = n;
    }
    std::uint64_t q81_ = 0; std::size_t q81_cap_ = 0;   // q8_1 activation scratch (stream-ordered reuse)
    // The scratch holds the q8_1 form of device buffer q81_src_ ([q81_n_] blocks). Consecutive decode matmuls that
    // read the same activation (qkv/z/a/b, gate/up) reuse it instead of re-quantising; any other kernel, copy or free
    // may change that buffer, so launch_s/htod_s/dtod/free_s drop it (q81_keep_ marks run_qmatmul's own launches).
    std::uint64_t q81_src_ = 0; std::uint32_t q81_n_ = 0; bool q81_keep_ = false;
    // The q8r scratch (prefill MMA activations) likewise holds the int8 rows of device buffer q8r_src_ ([q8r_n_] floats,
    // rows x k): written by forge_quant_q8r or by an rms_norm that emits it (forge_rms_norm_f), reused by the next
    // matmuls on the same input (q/k/v, gate/up) and dropped by the same rules as q81_src_ ($FORGE_FUSE_Q8R=0 off).
    // (run_qglu's own hand-off to the down projection, g8_*, is separate: see there.)
    std::uint64_t q8r_src_ = 0; std::size_t q8r_n_ = 0;
    static bool fuse_q8r() { static const bool on = [] { auto* e = std::getenv("FORGE_FUSE_Q8R"); return !(e && std::string(e) == "0"); }(); return on; }
    void q81_drop() { q81_src_ = 0; q8r_src_ = 0; g8_src_ = 0; }
    // Device ranges the next launch writes (set by callers that know them). launch_s keeps the q8_1 scratch when
    // none of them overlaps its source, so e.g. the a/b/z matmuls of a DeltaNet layer reuse the quantised input of
    // in_proj_qkv across the conv/slice kernels in between; a launch with no declared writes drops it.
    std::uint64_t wr_[3][2] = {}; unsigned nwr_ = 0;
    void writes(std::uint64_t p, std::size_t bytes) { if (nwr_ < 3) { wr_[nwr_][0] = p; wr_[nwr_][1] = p + bytes; } ++nwr_; }
    void q81_reserve(std::size_t need) {
        if (q81_cap_ >= need) return;
        q81_drop(); if (q81_) free_s(q81_); q81_ = 0; q81_cap_ = 0;
        q81_ = dmalloc(std::max<std::size_t>(need, 1 << 20), "scratch"); q81_cap_ = std::max<std::size_t>(need, 1 << 20);
    }
    // decode matvec: output columns per 128-thread block (1, 2 or 4; the block's warps split between them).
    // $FORGE_MMVQ_CPB forces one for every shape.
    static std::uint32_t mmvq_cpb(std::uint32_t fmt, std::uint32_t k) {
        static const std::uint32_t forced = [] { auto* e = std::getenv("FORGE_MMVQ_CPB"); unsigned v = e ? unsigned(std::atoi(e)) : 0u; return v == 1 || v == 2 || v == 4 ? v : 0u; }();
        if (forced) return forced;
        // q4_k with up to 24 super-blocks per column: a 4-warp block takes 16 per step, so k = 4096 / 5120 leave
        // most lanes idle on the second step; two columns per block (2 warps each, 8 per step) balance it
        // (25600x5120: 93 -> 87 us, 12288x4096: 39 -> 35.5 us). q6_k, q8_0 and longer k: no gain.
        return fmt == 2 && k <= 6144 ? 2 : 1;
    }
    // q8_0 decode (regrouped weights): 16-byte loads per lane; $FORGE_MMVQ_Q8V=0 keeps the 4-byte-per-lane loop
    static bool mmvq_q8v() { static const bool on = [] { auto* e = std::getenv("FORGE_MMVQ_Q8V"); return !(e && std::string(e) == "0"); }(); return on; }
    // decode matvec weight loads through ld.global.nc.L1::no_allocate ($FORGE_MMVQ_NC=1; measured no faster on a 3090)
    static bool mmvq_nc() { static const bool on = [] { auto* e = std::getenv("FORGE_MMVQ_NC"); return e && std::string(e) == "1"; }(); return on; }
    // Decode rms_norm / silu_mul also emit the q8_1 form of their output for the next matmul ($FORGE_FUSE_Q81=0 off).
    static bool fuse_q81() { static const bool on = [] { auto* e = std::getenv("FORGE_FUSE_Q81"); return !(e && std::string(e) == "0"); }(); return on; }
    bool run_qmatmul(const tensor_view& x, const std::uint8_t* q, std::size_t rb, std::uint32_t cols, std::uint32_t fmt, tensor_view out) {
        if (x.shape.empty() || !q || fmt < 1 || fmt > 4) return false;
        std::uint32_t k = std::uint32_t(x.shape.back()), rows = k ? std::uint32_t(x.size() / k) : 0;
        if (!k || k % 32 || out.size() != std::size_t(rows) * cols) return false;
        std::lock_guard lk(mu_); bind(); begin_op();
        ++dispatches_["qmatmul_t"];
        void* fn = function("forge_qmatmul_t");
        // q6_k on the sm_80 image: blocks padded from 210 to 224 bytes on upload (fmt 5), so both the tensor-core and
        // the decode kernel read them with aligned 16-byte loads. Cached under the padded size, apart from raw copies.
        const bool pad6 = fmt == 3 && k % 256 == 0 && use_mma() && use_mmvq() && has_fn_locked("forge_qmatmul_mma_q6_k") && has_fn_locked("forge_qmatmul_q81");
        // q8_0 likewise regrouped (fmt 6): each 8 blocks become 8 fp16 scales then 256 int8, the same 272 bytes, aligned.
        // Same size as the raw copy, so the cache key's low address bit tells them apart (block data is >= 2-aligned).
        const bool grp8 = fmt == 1 && k % 256 == 0 && use_mma() && use_mmvq() && has_fn_locked("forge_qmatmul_mma_q8_0g") && has_fn_locked("forge_qmatmul_q81");
        const std::size_t drb = pad6 ? rb / 210 * 224 : rb, qbytes = drb * cols;
        auto& qe = qres_[{reinterpret_cast<std::uintptr_t>(q) | (grp8 ? 1u : 0u), qbytes}]; qe.last = ++clock_;
        if (!qe.d) {
            std::uint64_t d = dmalloc(qbytes, "weights");
            qe.d = d;
            if (pad6) {   // pageable source: the call returns once the staging copy is taken, so tmp may go
                std::vector<std::uint8_t> tmp(qbytes, 0);
                for (std::size_t c = 0, nb = rb / 210; c < cols; ++c)
                    for (std::size_t b = 0; b < nb; ++b) std::memcpy(tmp.data() + c * drb + b * 224, q + c * rb + b * 210, 210);
                ck(htod_s(qe.d, tmp.data(), qbytes, stream_), "memcpy HtoD");
            } else if (grp8) {
                std::vector<std::uint8_t> tmp(qbytes);
                for (std::size_t c = 0, ng = rb / 272; c < cols; ++c)
                    for (std::size_t g = 0; g < ng; ++g) {
                        std::uint8_t* o = tmp.data() + c * rb + g * 272; const std::uint8_t* b = q + c * rb + g * 272;
                        for (int j = 0; j < 8; ++j) { std::memcpy(o + 2 * j, b + 34 * j, 2); std::memcpy(o + 16 + 32 * j, b + 34 * j + 2, 32); }
                    }
                ck(htod_s(qe.d, tmp.data(), qbytes, stream_), "memcpy HtoD");
            } else ck(htod_s(qe.d, q, qbytes, stream_), "memcpy HtoD");
            ++stats_.uploads; stats_.qweight_bytes += qbytes;
        }
        if (pad6) { fmt = 5; rb = drb; }
        if (grp8) fmt = 6;
        const std::uint64_t qd = qe.d;
        std::vector<std::uint64_t> temps;
        auto free_temps = [&] { for (auto p : temps) dfree(p); };
        try {
            std::uint64_t xd = 0, yd = 0;
            if (in_run_) {
                xd = acquire(x.data, x.size(), false).ptr;
                auto b = acquire(out.data, out.size(), true); b.e->host_valid = false; yd = b.ptr;
            } else {
                xd = dmalloc(x.size() * 4, "scratch"); temps.push_back(xd);
                ck(htod_s(xd, x.data, x.size() * 4, stream_), "memcpy HtoD");
                yd = dmalloc(out.size() * 4, "scratch"); temps.push_back(yd);
            }
            std::uint64_t wd = qd; std::uint32_t rb32 = std::uint32_t(rb);
            void* args[] = {&xd, &wd, &yd, &rows, &cols, &k, &fmt, &rb32};
            if (rows >= mmq_min_rows() && (fmt == 1 || fmt == 2 || fmt == 5 || fmt == 6) && k % 256 == 0 && use_mma() && has_fn_locked(mma_fn(fmt)) && has_fn_locked("forge_quant_q8r")) {
                ++dispatches_["qmatmul_mma"];   // batched prefill on int8 tensor cores
                std::uint32_t nb = rows * (k / 32);
                std::size_t xbytes = (std::size_t(rows) * k + 255) / 256 * 256;
                std::uint64_t xq = q8r_, xds = q8r_ + xbytes;
                // [q8r reuse] skip the quantisation when x's q8r form is already on the device: run_qglu's epilogue
                // wrote it (g8_, the down projection), or the q8r scratch holds it (an rms_norm emitted it, or the
                // previous matmul read the same x); this matmul's own launches below keep the tracking (q81_keep_)
                q81_keep_ = true;
                if (g8_hit(xd, nb)) { xq = g8_; xds = g8_ + xbytes; ++dispatches_["quant_q8r_reused"]; }   // x came from run_qglu
                else if (fuse_q8r() && q8r_src_ == xd && q8r_n_ == std::size_t(rows) * k && q8r_cap_ >= xbytes + std::size_t(nb) * 8) {
                    xq = q8r_; xds = q8r_ + xbytes; ++dispatches_["quant_q8r_reused"]; }
                else {
                    q8r_reserve(xbytes + std::size_t(nb) * 8); xq = q8r_; xds = q8r_ + xbytes;
                    void* qa[] = {&xd, &xq, &xds, &nb};
                    ck(launch_s(function("forge_quant_q8r"), (nb + 7) / 8, 1, 1, 256, 1, 1, 0, stream_, qa, nullptr), "launch");
                    if (fuse_q8r()) { q8r_src_ = xd; q8r_n_ = std::size_t(rows) * k; }
                }
                struct q8r_end { runtime* r; std::uint64_t y; std::size_t n; ~q8r_end() { r->q81_keep_ = false;   // y overwrote a tracked source
                    auto ov = [&](std::uint64_t s, std::size_t b) { return s && y < s + b && s < y + n; };
                    if (ov(r->q8r_src_, r->q8r_n_ * 4)) r->q8r_src_ = 0; if (ov(r->q81_src_, std::size_t(r->q81_n_) * 128)) r->q81_src_ = 0; } } q8r_guard{this, yd, out.size() * 4};
                // [/q8r reuse]
                std::uint32_t ksplit = 0, sk = 0, dp = 0; std::uint64_t fx = 0, fl = 0;
                void* a2[] = {&xq, &xds, &wd, &yd, &rows, &cols, &k, &fmt, &rb32, &ksplit, &fx, &fl, &sk, &dp};
                // 128 weight rows x bt tokens per 256-thread block (bt 128 for big batches); shared memory sized per format
                // q4_k keeps 64 tokens (47 KB of shared memory: two blocks per SM); q6_k / q8_0 use one block per SM anyway,
                // so 128 tokens give each warp twice the work per weight fragment ($FORGE_MMA_BT=64|128 forces one)
                const unsigned want = mma_bt() ? mma_bt() : fmt == 2 ? 64u : 128u;
                const unsigned bt = rows >= 128 && want == 128 && has_fn_locked(mma_fn(fmt, 128)) ? 128u : 64u;
                // q4_k at 64 tokens: the double-buffered kernel (copies the next k-chunk while computing this one)
                const bool ns2 = fmt == 2 && bt == 64 && mma_ns() == 2 && has_fn_locked("forge_qmatmul_mma2_q4_k");
                const unsigned smem = (ns2 ? 2u : 1u) * (bt * 336 + (fmt == 2 ? 144u : fmt == 5 ? 240u : 272u) * 128) + 8192;
                void* mf = function(ns2 ? std::string("forge_qmatmul_mma2_q4_k") : mma_fn(fmt, bt));
                if (!mma_attr_.count(mf) && f_.func_attr) { ck(f_.func_attr(mf, 8 /* MAX_DYNAMIC_SHARED_SIZE_BYTES */, int(smem)), "cuFuncSetAttribute"); mma_attr_.insert(mf); }
                unsigned grid = ((rows + bt - 1) / bt) * ((cols + 127) / 128);
                // Split-K when the grid cannot fill the device (k/v projections, DeltaNet a/b): S slices of k, each
                // written to a partial buffer, then summed in order ($FORGE_MMA_SPLITK=0 off).
                // S is chosen by wave efficiency: the time of S slices is ~ ceil(grid*S / slots) waves of 1/S the work,
                // so pick the S (up to 8, each slice >= 2 chunks of k) with the fewest waves per unit of work; ties go
                // to the smaller S (less partial traffic). slots: blocks resident at once (64-token kernels fit 2 per SM).
                const unsigned nch = k / 256, slots = sms() * (bt == 64 && !ns2 ? 2u : 1u);
                unsigned splits = 1, G = 0;
                const bool skp = !ns2 && streamk_plan(mf, smem, grid, nch, bt, G, dp, fx, fl);
                // Grids of a few waves with a ragged last one (e.g. 256 blocks on 164 slots) also split, when that saves
                // >= 10% of the waves; past 4 waves the tail costs little.
                if (!skp && grid && grid < 4 * slots && use_splitk() && has_fn_locked("forge_sum_splits")) {
                    double best = double((grid + slots - 1) / slots); const double need = grid < slots ? 0.97 : 0.9;
                    // a grid that already fills the device only splits long k (>= 16 chunks per slice): shorter slices
                    // lose more to the partial sums than the ragged wave costs (512x8192x4096 q4_k: 528 -> 638 us)
                    const unsigned min_ch = grid < slots ? 2u : 16u;
                    for (unsigned s = 2; s <= 8 && s <= nch / min_ch; ++s) {
                        double t = double((grid * s + slots - 1) / slots) / s;
                        if (t < best * need) { best = t; splits = s; }
                    }
                }
                if (grid && splits >= 2) {
                    ksplit = (nch + splits - 1) / splits;
                    const unsigned s = (nch + ksplit - 1) / ksplit;   // slices actually used
                    const std::size_t n = std::size_t(rows) * cols;
                    sk_reserve(n * s * 4);
                    std::uint64_t part = sk_; void* a3[] = {&xq, &xds, &wd, &part, &rows, &cols, &k, &fmt, &rb32, &ksplit, &fx, &fl, &sk, &dp};
                    ck(launch_s(mf, grid, s, 1, 256, 1, 1, smem, stream_, a3, nullptr), "launch");
                    std::uint32_t n32 = std::uint32_t(n), s32 = s; void* a4[] = {&part, &yd, &n32, &s32};
                    ck(launch_s(function("forge_sum_splits"), (n32 + 255) / 256, 1, 1, 256, 1, 1, 0, stream_, a4, nullptr), "launch");
                    ++dispatches_["qmatmul_mma_splitk"];
                } else if (skp) {
                    sk = 1; ck(launch_s(mf, G, 1, 1, 256, 1, 1, smem, stream_, a2, nullptr), "launch");
                    ++dispatches_["qmatmul_mma_streamk"];
                } else if (grid) ck(launch_s(mf, grid, 1, 1, 256, 1, 1, smem, stream_, a2, nullptr), "launch");
            } else if (rows >= mmq_min_rows() && has_fn_locked("forge_qmatmul_mmq")) {   // batched prefill: 64x64 tiles
                ++dispatches_["qmatmul_mmq"];
                unsigned grid = ((cols + 63) / 64) * ((rows + 63) / 64);
                if (grid) ck(launch_s(function("forge_qmatmul_mmq"), grid, 1, 1, 256, 1, 1, 0, stream_, args, nullptr), "launch");
            } else if ((fmt <= 3 || fmt == 5 || fmt == 6) && use_mmvq() && has_fn_locked("forge_qmatmul_q81") && has_fn_locked("forge_quant_q81")) {
                ++dispatches_["qmatmul_q81"];   // decode: q8_1 activations + dp4a (llama.cpp mmvq)
                std::uint32_t nb = rows * (k / 32); std::size_t need = std::size_t(nb) * 40;
                q81_reserve(need);
                std::uint64_t qx = q81_;
                q81_keep_ = true;
                if (q81_src_ != xd || q81_n_ != nb) {
                    void* qa[] = {&xd, &qx, &nb};
                    ck(launch_s(function("forge_quant_q81"), (nb + 7) / 8, 1, 1, 256, 1, 1, 0, stream_, qa, nullptr), "launch");
                    q81_src_ = xd; q81_n_ = nb;
                } else ++dispatches_["quant_q81_reused"];
                const std::uint32_t cpb = mmvq_cpb(fmt, k); std::uint32_t fc = fmt | (cpb << 8) | (fmt == 6 && !mmvq_q8v() ? 0x800u : 0u) | (mmvq_nc() ? 0x1000u : 0u);
                void* a2[] = {&qx, &wd, &yd, &rows, &cols, &k, &fc, &rb32};
                unsigned grid = (cols + cpb - 1) / cpb * ((rows + 3) / 4);   // a 4-warp block per cpb columns (and 4 activation rows)
                if (grid) ck(launch_s(function("forge_qmatmul_q81"), grid, 1, 1, 128, 1, 1, 0, stream_, a2, nullptr), "launch");
                q81_keep_ = false;
                if (yd < xd + std::uint64_t(nb) * 128 && xd < yd + std::uint64_t(out.size()) * 4) q81_drop();   // y overwrote the source
            } else {
                unsigned grid = cols * ((rows + 3) / 4);
                if (grid) ck(launch_s(fn, grid, 1, 1, 128, 1, 1, 0, stream_, args, nullptr), "launch");
            }
            if (!in_run_) { ck(dtoh_s(out.data, yd, out.size() * 4, stream_), "memcpy DtoH"); sync(); }
        } catch (...) { q81_keep_ = false; q81_drop(); if (!in_run_) sync(); free_temps(); throw; }
        if (!temps.empty()) { sync(); free_temps(); }
        return true;
    }

    // ---- FFN gate/up/SiLU fused (graph::compile's GLU pass; $FORGE_FUSE_GLU=0 off) ----
    // y[rows, n] = silu(x . Wg^T) * (x . Wu^T). The two weights (same codec, [n, k] each) are uploaded once as one [2n, k]
    // weight that interleaves them in groups of 16 rows (gate rows 8i..8i+7, then up rows 8i..8i+7), repacked like
    // run_qmatmul's (q6_k padded, q8_0 regrouped), in place of the two separate copies. Prefill (rows >= the MMA
    // threshold): forge_qmatmul_glu_* -- the int8 tensor-core kernel with a SiLU epilogue, whose block's 128 rows give 64
    // whole output columns; it also writes the q8r form of y, which the down projection's run_qmatmul then uses instead
    // of quantising y again (g8_*). Decode: forge_qmatmul_q81 over the 2n rows into scratch, then forge_glu_q81 (y and its
    // q8_1 blocks for the down projection, like forge_silu_mul_q81). Only inside a run, k % 256 == 0, n % 64 == 0, and
    // only where both the prefill and the decode kernels exist (so one process never holds both weight layouts).
    // false: not taken; the caller runs the two matmuls and silu_mul instead.
    static bool use_glu() { static const bool on = [] { auto* e = std::getenv("FORGE_FUSE_GLU"); return !(e && std::string(e) == "0"); }(); return on; }
    static std::string glu_fn(std::uint32_t efmt, unsigned bt) {
        return std::string(bt == 128 ? "forge_qmatmul_glu128_" : "forge_qmatmul_glu_") + (efmt == 6 ? "q8_0g" : efmt == 2 ? "q4_k" : "q6_k"); }
    std::uint64_t g8_ = 0; std::size_t g8_cap_ = 0;   // q8r form of the last forge_qmatmul_glu output (device buffer g8_src_)
    std::uint64_t g8_src_ = 0, g8_stamp_ = 0; std::uint32_t g8_n_ = 0;
    std::uint64_t gs_ = 0; std::size_t gs_cap_ = 0;   // decode GLU: the [rows, 2n] gate/up product
    static std::uint64_t launch_stamp() { auto& c = counters(); return c.launches.load(std::memory_order_relaxed) + c.captured.load(std::memory_order_relaxed); }
    // The q8r form in g8_ is that of x (device pointer, nb groups) when nothing was launched, copied or freed since.
    bool g8_hit(std::uint64_t xd, std::uint32_t nb) const { return g8_src_ && g8_src_ == xd && g8_n_ == nb && g8_stamp_ == launch_stamp(); }
    void scratch_reserve(std::uint64_t& p, std::size_t& cap, std::size_t need) {
        if (cap >= need) return;
        if (p) free_s(p); p = 0; cap = 0;
        std::size_t n = std::max<std::size_t>(need, 1 << 20); p = dmalloc(n, "scratch"); cap = n;
    }
    bool run_qglu(const tensor_view& x, const std::uint8_t* qg, const std::uint8_t* qu, std::size_t rb, std::uint32_t n, std::uint32_t fmt, tensor_view out) {
        if (x.shape.empty() || !qg || !qu || !use_glu() || (fmt != 1 && fmt != 2 && fmt != 3) || (reinterpret_cast<std::uintptr_t>(qg) & 3u)) return false;
        const std::uint32_t k = std::uint32_t(x.shape.back()), rows = k ? std::uint32_t(x.size() / k) : 0;
        if (!k || k % 256 || !n || n % 64 || !rows || out.size() != std::size_t(rows) * n) return false;
        std::lock_guard lk(mu_); bind();
        if (!in_run_ || !use_mma() || !use_mmvq()) return false;
        const std::uint32_t ef = fmt == 1 ? 6 : fmt == 3 ? 5 : 2;   // the layouts run_qmatmul repacks to on this image
        for (const char* f : {"forge_quant_q8r", "forge_quant_q81", "forge_qmatmul_q81", "forge_glu_q81"}) if (!has_fn_locked(f)) return false;
        if (!has_fn_locked(glu_fn(ef, 64)) || !has_fn_locked(mma_fn(ef))) return false;
        begin_op();
        ++dispatches_["qmatmul_glu"];
        const std::uint32_t cols = 2 * n;
        const std::size_t drb = fmt == 3 ? rb / 210 * 224 : rb, qbytes = drb * cols;
        auto& qe = qres_[{reinterpret_cast<std::uintptr_t>(qg) | 2u, qbytes}]; qe.last = ++clock_;
        if (!qe.d) {
            std::uint64_t d = dmalloc(qbytes, "weights"); qe.d = d;
            std::vector<std::uint8_t> tmp(qbytes, 0);
            for (std::size_t c = 0; c < cols; ++c) {
                const std::uint8_t* s = ((c & 8) ? qu : qg) + ((c >> 4) * 8 + (c & 7)) * rb;
                std::uint8_t* o = tmp.data() + c * drb;
                if (fmt == 3) for (std::size_t b = 0, nb = rb / 210; b < nb; ++b) std::memcpy(o + b * 224, s + b * 210, 210);
                else if (fmt == 1) for (std::size_t g = 0, ng = rb / 272; g < ng; ++g)
                    for (int j = 0; j < 8; ++j) { std::memcpy(o + g * 272 + 2 * j, s + g * 272 + 34 * j, 2); std::memcpy(o + g * 272 + 16 + 32 * j, s + g * 272 + 34 * j + 2, 32); }
                else std::memcpy(o, s, rb);
            }
            ck(htod_s(qe.d, tmp.data(), qbytes, stream_), "memcpy HtoD");   // pageable: staged before the call returns
            ++stats_.uploads; stats_.qweight_bytes += qbytes;
        }
        std::uint64_t wd = qe.d, xd = acquire(x.data, x.size(), false).ptr;
        auto yb = acquire(out.data, out.size(), true); yb.e->host_valid = false; std::uint64_t yd = yb.ptr;
        xd = acquire(x.data, x.size(), false).ptr;   // final once a pass hits the cache (out may have replaced an overlapping x)
        std::uint32_t fm = ef, rb32 = std::uint32_t(drb), kk = k, nn = n, rr = rows, cc = cols;
        try {
            if (rows >= mmq_min_rows()) {
                std::uint32_t nb = rows * (k / 32), nbo = rows * (n / 32);
                std::size_t xbytes = (std::size_t(rows) * k + 255) / 256 * 256, obytes = (std::size_t(rows) * n + 255) / 256 * 256;
                q8r_reserve(xbytes + std::size_t(nb) * 8);
                scratch_reserve(g8_, g8_cap_, obytes + std::size_t(nbo) * 8);
                std::uint64_t xq = q8r_, xds = q8r_ + xbytes, gq = g8_, gds = g8_ + obytes;
                void* qa[] = {&xd, &xq, &xds, &nb};
                // x is usually the ffn rms_norm's output, which already wrote its q8r form (FORGE_FUSE_Q8R)
                if (fuse_q8r() && q8r_src_ == xd && q8r_n_ == std::size_t(rows) * k) ++dispatches_["quant_q8r_reused"];
                else {
                    ck(launch_s(function("forge_quant_q8r"), (nb + 7) / 8, 1, 1, 256, 1, 1, 0, stream_, qa, nullptr), "launch");
                    if (fuse_q8r()) { q8r_src_ = xd; q8r_n_ = std::size_t(rows) * k; }
                }
                const unsigned want = mma_bt() ? mma_bt() : ef == 2 ? 64u : 128u;
                const unsigned bt = rows >= 128 && want == 128 && has_fn_locked(glu_fn(ef, 128)) ? 128u : 64u;
                const unsigned smem = bt * 336 + (ef == 2 ? 144u : ef == 5 ? 240u : 272u) * 128 + 8192;
                void* mf = function(glu_fn(ef, bt));
                if (!mma_attr_.count(mf) && f_.func_attr) { ck(f_.func_attr(mf, 8 /* MAX_DYNAMIC_SHARED_SIZE_BYTES */, int(smem)), "cuFuncSetAttribute"); mma_attr_.insert(mf); }
                std::uint32_t sk = 0, dp = 0; std::uint64_t fx = 0, fl = 0; unsigned G = 0;
                const unsigned tiles = ((rows + bt - 1) / bt) * (cols / 128);
                if (streamk_plan(mf, smem, tiles, k / 256, bt, G, dp, fx, fl)) { sk = 1; ++dispatches_["qmatmul_glu_streamk"]; }
                void* a2[] = {&xq, &xds, &wd, &yd, &rr, &cc, &kk, &fm, &rb32, &gq, &gds, &fx, &fl, &sk, &dp};
                writes(yd, out.size() * 4);
                ck(launch_s(mf, sk ? G : tiles, 1, 1, 256, 1, 1, smem, stream_, a2, nullptr), "launch");
                g8_src_ = yd; g8_n_ = nbo; g8_stamp_ = launch_stamp();
            } else {   // decode
                std::uint32_t nb = rows * (k / 32), nbo = rows * (n / 32);
                q81_reserve(std::size_t(std::max(nb, nbo)) * 40);
                scratch_reserve(gs_, gs_cap_, std::size_t(rows) * cols * 4);
                std::uint64_t qx = q81_, hs = gs_;
                q81_keep_ = true;
                if (q81_src_ != xd || q81_n_ != nb) {
                    void* qa[] = {&xd, &qx, &nb};
                    ck(launch_s(function("forge_quant_q81"), (nb + 7) / 8, 1, 1, 256, 1, 1, 0, stream_, qa, nullptr), "launch");
                    q81_src_ = xd; q81_n_ = nb;
                } else ++dispatches_["quant_q81_reused"];
                void* a2[] = {&qx, &wd, &hs, &rr, &cc, &kk, &fm, &rb32};
                ck(launch_s(function("forge_qmatmul_q81"), cols * ((rows + 3) / 4), 1, 1, 128, 1, 1, 0, stream_, a2, nullptr), "launch");
                q81_keep_ = false;
                void* a3[] = {&hs, &yd, &rr, &nn, &qx};
                writes(yd, out.size() * 4); writes(qx, std::size_t(nbo) * 40);
                ck(launch_s(function("forge_glu_q81"), (nbo + 7) / 8, 1, 1, 256, 1, 1, 0, stream_, a3, nullptr), "launch");
                // the scratch now holds y's q8_1 blocks: the down projection reuses them ($FORGE_FUSE_Q81=0: not)
                if (fuse_q81()) { q81_src_ = yd; q81_n_ = nbo; } else q81_drop();
            }
        } catch (...) { q81_keep_ = false; q81_drop(); throw; }
        return true;
    }

    // Gated DeltaNet state ops (graph/state_ops.hpp) on the device. The state slot S is pinned, so it stays resident
    // across runs (tokens) like the weights; inputs/output follow the residency protocol. Only inside a run.
    bool run_state(const std::string& fn, const std::vector<tensor_view>& in, float* S, std::size_t sn, tensor_view out, const ops::attrs& a) {
        std::lock_guard lk(mu_); bind(); begin_op();
        if (!in_run_ || !has_fn_locked("forge_" + fn)) return false;
        std::uint32_t u[6] = {}; std::uint32_t grid = 0; float sc = 0; std::string kfn = "forge_" + fn; unsigned block = 128;
        if (fn == "delta_rule" && in.size() == 5) {
            u[0] = std::uint32_t(in[0].shape[0]); u[1] = std::uint32_t(in[0].shape[1]); u[2] = std::uint32_t(in[0].shape[2]);
            u[3] = std::uint32_t(in[2].shape[1]); u[4] = std::uint32_t(in[2].shape[2]); u[5] = a.num("l2norm", 1) != 0;
            if (u[2] > 256u || !u[1] || u[3] % u[1]) return false;
            sc = float(a.num("scale", 1.0 / std::sqrt(double(u[2])))); grid = u[3];
            // state in registers, dk = 8 * {8, 16, 32}: by default the staged kernels (a 64-thread block per
            // (value head, 8 columns)); FORGE_DELTA_RULE_R the earlier ones (a 256-thread block per (head, 32 columns))
            if ((u[2] == 64 || u[2] == 128 || u[2] == 256) && !std::getenv("FORGE_DELTA_RULE_OLD")) {
                bool rk = std::getenv("FORGE_DELTA_RULE_R") != nullptr; unsigned cols = rk ? 32u : 8u;
                std::string rn = std::string(rk ? "forge_delta_rule_r" : "forge_delta_rule_p") + std::to_string(u[2] / 8);
                if (u[4] % cols == 0 && has_fn_locked(rn)) { kfn = rn; grid = u[3] * (u[4] / cols); block = rk ? 256 : 64; } }
        } else if (fn == "causal_conv" && in.size() == 2) {
            u[0] = std::uint32_t(in[0].shape[0]); u[1] = std::uint32_t(in[0].shape[1]); u[2] = std::uint32_t(in[1].shape[1]);
            if (u[2] < 1u || u[2] > 9u) return false;
            grid = (u[1] + 127u) / 128u;
            // token-parallel: a thread per (channel, 8 tokens); FORGE_CAUSAL_CONV_OLD the per-channel kernel
            if (!std::getenv("FORGE_CAUSAL_CONV_OLD") && has_fn_locked("forge_causal_conv_t")) { kfn = "forge_causal_conv_t"; grid *= (u[0] + 7u) / 8u; }
        } else return false;
        bool known = false; for (auto& [p, n] : pinned_) if (p == S && n == sn) known = true;
        if (!known) pinned_.push_back({S, sn});
        // the kernels are not in-place safe: an output overlapping an input or the state goes to the host path
        auto overlaps = [](const float* a, std::size_t an, const float* b, std::size_t bn) { return a < b + bn && b < a + an; };
        for (auto& v : in) if (overlaps(out.data, out.size(), v.data, v.size())) return false;
        if (overlaps(out.data, out.size(), S, sn)) return false;
        // acquire twice: a later acquire may replace (and free) a resident buffer an earlier one returned when the
        // ranges overlap, so the pointers are only final once a full pass hits the cache
        std::vector<std::uint64_t> ptrs;
        for (int pass = 0; pass < 2; ++pass) {
            ptrs.clear();
            for (auto& v : in) ptrs.push_back(acquire(v.data, v.size(), false).ptr);
            auto sb = acquire(S, sn, false); sb.e->pinned = true; ptrs.push_back(sb.ptr);
            ptrs.push_back(acquire(out.data, out.size(), pass == 0).ptr);
        }
        for (auto& [k, e] : res_) { auto kp = reinterpret_cast<const float*>(k); if (overlaps(kp, e.count, S, sn) || overlaps(kp, e.count, out.data, out.size())) e.host_valid = false; }
        std::vector<void*> args; for (auto& p : ptrs) args.push_back(&p);
        if (fn == "delta_rule") { for (int i = 0; i < 6; ++i) args.push_back(&u[i]); args.push_back(&sc); }
        else for (int i = 0; i < 3; ++i) args.push_back(&u[i]);
        ++dispatches_[fn];
        writes(ptrs.back(), out.size() * 4); writes(ptrs[ptrs.size() - 2], sn * 4);
        if (grid) ck(launch_s(function(kfn), grid, 1, 1, block, 1, 1, 0, stream_, args.data(), nullptr), "launch");
        return true;
    }

    // Run one op over host views; false if there is no kernel for this op/shape. Inside a run (begin_run)
    // buffers stay resident; outside, every buffer is uploaded and the output downloaded.
    bool run_op(const std::string& op, const std::vector<tensor_view>& in, const ops::attrs& a, tensor_view out) {
        std::vector<shape_t> ss; for (auto& v : in) ss.push_back(v.shape);
        if (!supports(op, ss, a)) return false;
        sig s; kernel_sig(op, s);
        auto& x = in[0];
        std::uint32_t n = std::uint32_t(out.size()), rows = 0, cols = 0, k = 0, threads = n, u4 = 0, u5 = 0, u6 = 0, u7 = 0;
        float f0 = 0;
        std::vector<float> extra;   // rope: inverse frequencies (computed in double on the host, like the CPU path)
        switch (s) {
        case sig::unary: case sig::binary: break;
        case sig::scale: f0 = float(a.num("alpha", 1.0)); break;
        case sig::rows: case sig::rms:
            cols = std::uint32_t(x.shape.back()); rows = cols ? std::uint32_t(x.size()) / cols : 0; threads = rows; f0 = float(a.num("eps", 1e-6)); break;
        case sig::matmul:
            k = std::uint32_t(x.shape.back()); rows = std::uint32_t(x.size()) / k; cols = std::uint32_t(in[1].shape[0]); threads = rows * cols; break;
        case sig::rope: {
            rows = std::uint32_t(x.shape[0]); cols = std::uint32_t(x.shape[1]); k = std::uint32_t(x.shape[2]); u4 = a.num("neox", 1) != 0;
            double base = a.num("base", 10000.0); extra.resize(k / 2);
            for (std::uint32_t j = 0; j < k / 2; ++j) extra[j] = float(std::pow(base, -2.0 * double(j) / double(k)));
            threads = rows * cols * (k / 2); break; }
        case sig::gather: rows = std::uint32_t(x.shape[0]); cols = std::uint32_t(x.shape[1]); break;
        case sig::slice: {
            auto ax = std::size_t(ops::norm_axis(i64(a.num("axis", 0)), x.shape.size()));
            i64 inner = 1; for (std::size_t d = ax + 1; d < x.shape.size(); ++d) inner *= x.shape[d];
            i64 st = i64(a.num("start", 0)); if (st < 0) st += x.shape[ax];
            rows = std::uint32_t(x.shape[ax]); cols = std::uint32_t(inner); k = std::uint32_t(st); u4 = std::uint32_t(out.shape[ax]); break; }
        case sig::attention: {
            auto kc = std::uint32_t(a.num("kv_codec_k", 0)), vc = std::uint32_t(a.num("kv_codec_v", 0));
        if (std::uint32_t dd = std::uint32_t(x.shape[2]), dvv = std::uint32_t(a.num("kv_dv", 0)); (kc >= 3u && dd > 256u) || (vc >= 3u && dvv > 256u)) raise(errc::unsupported, "TurboQuant KV on the GPU supports head dims up to 256");
        if (kc >= 5u || vc >= 5u) raise(errc::unsupported, "TurboQuant QJL KV types (tbq4p, tbq3p) run on the CPU only");
            rows = std::uint32_t(x.shape[0]); cols = std::uint32_t(x.shape[1]); k = std::uint32_t(x.shape[2]);
            u4 = std::uint32_t(in[1].shape[0]); u5 = std::uint32_t(in[1].shape[1]);
            u6 = vc ? std::uint32_t(a.num("kv_dv", 0)) : std::uint32_t(in[2].shape[2]);
            u7 = (a.num("causal", 1) != 0 ? 1u : 0u) | (kc << 1) | (vc << 4) | (std::uint32_t(a.num("window", 0)) << 7);
            f0 = float(a.num("scale", 1.0 / std::sqrt(double(k)))); threads = rows * cols; break; }
        }
        std::lock_guard lk(mu_); bind(); begin_op();
        ++dispatches_[op];
        void* fn = function("forge_" + op);
        std::vector<std::uint64_t> ptrs, temps;
        auto free_temps = [&] { for (auto p : temps) dfree(p); };
        try {
            auto buf = [&](const float* h, std::size_t cnt, bool is_out) {
                if (in_run_) { auto b = acquire(const_cast<float*>(h), cnt, is_out); if (is_out) b.e->host_valid = false; return b.ptr; }
                std::uint64_t d = dmalloc(cnt * 4, "scratch"); temps.push_back(d);
                if (!is_out) ck(htod_s(d, h, cnt * 4, stream_), "memcpy HtoD");
                return d;
            };
            for (std::size_t i = 0; i < in.size(); ++i) {
                ptrs.push_back(buf(in[i].data, in[i].size(), false));
                if (s == sig::rope && i == 1) {   // the inverse-frequency table goes after pos
                    auto& d = rope_[extra];   // the table depends only on (base, dim): upload once
                    if (!d) { d = dmalloc(extra.size() * 4, "scratch");
                              ck(htod_s(d, extra.data(), extra.size() * 4, stream_), "memcpy HtoD"); }
                    ptrs.push_back(d);
                }
            }
            ptrs.push_back(buf(out.data, out.size(), true));
            std::vector<void*> args; for (auto& p : ptrs) args.push_back(&p);
            switch (s) {
            case sig::unary: case sig::binary: args.push_back(&n); break;
            case sig::scale: args.push_back(&n); args.push_back(&f0); break;
            case sig::rows: args.push_back(&rows); args.push_back(&cols); break;
            case sig::rms: args.push_back(&rows); args.push_back(&cols); args.push_back(&f0); break;
            case sig::matmul: args.push_back(&rows); args.push_back(&cols); args.push_back(&k); break;
            case sig::rope: args.push_back(&rows); args.push_back(&cols); args.push_back(&k); args.push_back(&u4); break;
            case sig::gather: args.push_back(&n); args.push_back(&rows); args.push_back(&cols); break;
            case sig::slice: args.push_back(&n); args.push_back(&rows); args.push_back(&cols); args.push_back(&k); args.push_back(&u4); break;
            case sig::attention:
                for (auto* p : {&rows, &cols, &k, &u4, &u5, &u6, &u7}) args.push_back(p);
                args.push_back(&f0); break;
            }
            unsigned block = 64, grid = (threads + block - 1) / block;
            if (s == sig::attention && !std::getenv("FORGE_GPU_NO_FA") && has_fn_locked("forge_attention_fa")) {   // block per (row, head)
                fn = function("forge_attention_fa"); block = 128; grid = rows * cols; }
            std::uint32_t ns = 16, nrow = rows * cols; std::uint64_t pa = 0; bool dec = false;   // outlive the block: the comb launch below reads them via args
            if (s == sig::attention && rows * cols <= 256 && (u7 & 0x7Eu) == 0 && k % 32 == 0 && k <= 256 && u6 % 32 == 0 && u6 <= 256 &&
                ptrs.size() == 4 && attn_dec() && has_fn_locked("forge_attention_dec") && has_fn_locked("forge_attention_comb")) {
                std::size_t need = std::size_t(nrow) * ns * (u6 + 2) * 4;
                if (att_cap_ < need) { if (att_) free_s(att_); att_ = 0; att_cap_ = 0; att_ = dmalloc(need, "scratch"); att_cap_ = need; }
                pa = att_;
                void* a1[] = {&ptrs[0], &ptrs[1], &ptrs[2], &pa, &rows, &cols, &k, &u4, &u5, &u6, &u7, &f0, &ns};
                writes(pa, need);
                ck(launch_s(function("forge_attention_dec"), nrow * ns, 1, 1, 128, 1, 1, 0, stream_, a1, nullptr), "launch");
                fn = function("forge_attention_comb"); args = {&pa, &ptrs[3], &nrow, &ns, &u6}; block = 128; grid = nrow;
                ++dispatches_["attention_dec"]; dec = true; }
            unsigned smem = 0;
            // prefill: tiled attention (forge_attention_fa2_*, sm_80 image); K/V tiles shared by 64 (32) query rows
            if (s == sig::attention && !dec && rows >= 16 && (u7 & 0x7Eu) == 0 && k % 4 == 0 && u6 % 4 == 0 && k <= 256 && u6 <= 256 &&
                ptrs.size() == 4 && !std::getenv("FORGE_GPU_NO_FA") && has_fn_locked(k <= 128 && u6 <= 128 ? "forge_attention_fa2_d128" : "forge_attention_fa2_d256")) {
                const bool d128 = k <= 128 && u6 <= 128; unsigned bq = d128 ? 64 : 32;
                // fp16 tensor-core variant (forge_attention_fa2tc_*): 64 query rows per 128-thread block, K/V tiles in fp16
                const unsigned tcd = k <= 64 && u6 <= 64 ? 64 : d128 ? 128 : 256;
                const std::string tcn = "forge_attention_fa2tc_d" + std::to_string(tcd);
                if (fa_tc() && has_fn_locked(tcn)) {
                    fn = function(tcn); bq = 64; block = 128; smem = (tcd == 64 ? 128 : 64) * (tcd + 8) * 2;   // max(2 BK, 64) rows (BK 64, 32, 16)
                    ++dispatches_["attention_fa2tc"];
                } else {
                    fn = function(d128 ? "forge_attention_fa2_d128" : "forge_attention_fa2_d256"); block = 256;
                    smem = d128 ? (64 * 132 + 32 * 132 + 32 * 128 + 64 * 33) * 4 : (32 * 260 + 16 * 260 + 16 * 256 + 32 * 17) * 4; }
                if (!mma_attr_.count(fn) && f_.func_attr) { ck(f_.func_attr(fn, 8 /* MAX_DYNAMIC_SHARED_SIZE_BYTES */, int(smem)), "cuFuncSetAttribute"); mma_attr_.insert(fn); }
                grid = ((rows + bq - 1) / bq) * cols; ++dispatches_["attention_fa2"]; }
            if (s == sig::rms && has_fn_locked("forge_rms_norm_b")) { fn = function("forge_rms_norm_b"); block = 256; grid = rows; }
            std::uint32_t fnb = 0; std::uint64_t qx = 0;
            if (in_run_ && fuse_q81() && use_mmvq() && rows < mmq_min_rows()) {
                if (s == sig::rms && cols % 32 == 0 && has_fn_locked("forge_rms_norm_q81")) {
                    fn = function("forge_rms_norm_q81"); fnb = rows * (cols / 32);
                    if (rows <= 4 && cols >= 2048) block = 1024; }   // one wide row: a 1024-thread block
                else if (op == "silu_mul" && n % 32 == 0 && n <= (1u << 18) && has_fn_locked("forge_silu_mul_q81")) {
                    fn = function("forge_silu_mul_q81"); fnb = n / 32; block = 256; grid = (fnb + 7) / 8; }
                else if (dec && u6 % 32 == 0 && has_fn_locked("forge_attention_comb_q81")) {   // the output projection's input
                    fn = function("forge_attention_comb_q81"); fnb = nrow * (u6 / 32); }
                if (fnb) { q81_reserve(std::size_t(fnb) * 40); qx = q81_; args.push_back(&qx); ++dispatches_["fused_q81"]; }
            }
            // prefill rms_norm also writes y's q8r rows for the next tensor-core matmul (forge_rms_norm_f, same reduction
            // as forge_rms_norm_b), which then skips forge_quant_q8r ($FORGE_FUSE_Q8R=0 off)
            std::uint64_t q8x = 0, q8d = 0, nul = 0; std::uint32_t q8m = 4; bool q8 = false;
            if (s == sig::rms && !fnb && q8r_emit(rows, cols) && fn == function("forge_rms_norm_b")) {
                auto [xq, xd2] = q8r_slot(rows, cols); q8x = xq; q8d = xd2; q8 = true;
                fn = function("forge_rms_norm_f"); args = {&ptrs[0], &nul, &nul, &ptrs[1], &ptrs[2], &rows, &cols, &f0, &q8m, &q8x, &q8d};
                ++dispatches_["fused_q8r"];
            }
            writes(ptrs.back(), out.size() * 4);
            if (grid) ck(launch_s(fn, grid, 1, 1, block, 1, 1, smem, stream_, args.data(), nullptr), "launch");
            if (fnb) { q81_src_ = ptrs.back(); q81_n_ = fnb; }
            if (q8) { q8r_src_ = ptrs.back(); q8r_n_ = std::size_t(rows) * cols; }
            if (!in_run_) { ck(dtoh_s(out.data, ptrs.back(), out.size() * 4, stream_), "memcpy DtoH"); sync(); }
        } catch (...) { if (!in_run_) sync(); free_temps(); throw; }
        if (!temps.empty()) { sync(); free_temps(); }
        return true;
    }

    // ---- elementwise fusions (graph::device_hooks::fused): two adjacent plan steps as one kernel, inside a run ----
    //   "add_rms_norm": in {a, b, w}, out {s = a + b, y = rms_norm(s, w)}            ($FORGE_FUSE_ADD_NORM=0 off)
    //   "rms_rope":     in {x [s, heads, d], w [d], pos [s]}, out {rope(rms_norm(x, w), pos)}   ($FORGE_FUSE_QK_ROPE=0 off)
    // attrs: the rms_norm's eps (and the rope's base / neox). The norm reduces as the kernel run_op would pick for the
    // same rows (forge_rms_norm_b or forge_rms_norm_q81), and add_rms_norm also writes the q8_1 / q8r form of y when
    // run_op's rms_norm would, so results are bit-identical to the two steps. false = not handled here (the executor
    // runs the steps one by one): outside a run, a missing kernel, unsupported shapes, or outputs that partly overlap
    // the inputs (in place is fine).
    bool run_fused(const std::string& kind, const std::vector<tensor_view>& in, const std::vector<tensor_view>& out, const ops::attrs& a) {
        auto on = [](const char* v) { auto* e = std::getenv(v); return !(e && std::string(e) == "0"); };
        static const bool add_norm = on("FORGE_FUSE_ADD_NORM"), qk_rope = on("FORGE_FUSE_QK_ROPE");
        auto ov = [](const tensor_view& p, const tensor_view& q) { return p.data < q.data + q.size() && q.data < p.data + p.size(); };
        auto same_or_apart = [&](const tensor_view& p, const tensor_view& q) { return !ov(p, q) || (p.data == q.data && p.size() == q.size()); };
        for (auto& v : in) if (!v.data) return false;
        for (auto& v : out) if (!v.data) return false;
        std::lock_guard lk(mu_); bind(); begin_op();
        if (!in_run_ || !has_fn_locked("forge_rms_norm_b")) return false;
        // device pointers: inputs, then outputs (acquired twice: a later acquire may replace an earlier overlapping one)
        auto bind_all = [&](std::vector<std::uint64_t>& p) {
            for (int pass = 0; pass < 2; ++pass) {
                p.clear();
                for (auto& v : in) p.push_back(acquire(v.data, v.size(), false).ptr);
                for (auto& v : out) { auto b = acquire(v.data, v.size(), pass == 0); b.e->host_valid = false; p.push_back(b.ptr); }
            } };
        const float eps = float(a.num("eps", 1e-6));
        if (kind == "add_rms_norm" && add_norm && in.size() == 3 && out.size() == 2 && has_fn_locked("forge_rms_norm_f")) {
            const tensor_view &A = in[0], &B = in[1], &W = in[2], &S = out[0], &Y = out[1];
            const std::size_t n = S.size(), c = W.size();
            if (!c || c % 32 || A.size() != n || B.size() != n || Y.size() != n || n % c || n / c > 0xffffffffu || c > 0xffffffffu) return false;
            if (!same_or_apart(S, A) || !same_or_apart(S, B) || ov(S, Y) || !same_or_apart(Y, A) || !same_or_apart(Y, B) || ov(W, S) || ov(W, Y)) return false;
            std::uint32_t rows = std::uint32_t(n / c), cols = std::uint32_t(c); unsigned block = 256;
            std::uint32_t mode = 8u | rms_mode(rows, cols, &block);
            std::uint64_t xq = 0, xds = 0;
            if (mode & 1u) { mode |= 2u; q81_reserve(std::size_t(rows) * (cols / 32) * 40); xq = q81_; }
            else if (q8r_emit(rows, cols)) { mode |= 4u; std::tie(xq, xds) = q8r_slot(rows, cols); }
            std::vector<std::uint64_t> p; bind_all(p);   // a, b, w, s, y
            void* args[] = {&p[0], &p[1], &p[3], &p[2], &p[4], &rows, &cols, const_cast<float*>(&eps), &mode, &xq, &xds};
            writes(p[3], n * 4); writes(p[4], n * 4);
            if (rows) ck(launch_s(function("forge_rms_norm_f"), rows, 1, 1, block, 1, 1, 0, stream_, args, nullptr), "launch");
            if (mode & 2u) { q81_src_ = p[4]; q81_n_ = rows * (cols / 32); }
            if (mode & 4u) { q8r_src_ = p[4]; q8r_n_ = n; }
            ++dispatches_["fused_add_rms_norm"];
            return true;
        }
        if (kind == "rms_rope" && qk_rope && in.size() == 3 && out.size() == 1 && has_fn_locked("forge_rms_rope")) {
            const tensor_view &X = in[0], &W = in[1], &Pp = in[2], &Y = out[0];
            if (X.shape.size() != 3) return false;
            const i64 s = X.shape[0], H = X.shape[1], d = X.shape[2];
            if (s <= 0 || H <= 0 || d < 32 || d > 256 || (d & (d - 1)) || i64(W.size()) != d || i64(Pp.size()) != s || Y.size() != X.size() || s * H > 0xffffffffll) return false;
            if (!same_or_apart(Y, X) || ov(Y, W) || ov(Y, Pp)) return false;
            unsigned block = 256;
            std::uint32_t mode = rms_mode(std::uint32_t(s * H), std::uint32_t(d), &block);
            if (block != 256) return false;
            std::vector<float> invf(std::size_t(d / 2)); const double base = a.num("base", 10000.0);
            for (std::size_t j = 0; j < invf.size(); ++j) invf[j] = float(std::pow(base, -2.0 * double(j) / double(d)));   // as run_op's rope
            auto& tb = rope_[invf];
            if (!tb) { tb = dmalloc(invf.size() * 4, "scratch"); ck(htod_s(tb, invf.data(), invf.size() * 4, stream_), "memcpy HtoD"); }
            std::uint64_t tbl = tb;
            // heads per block: a prefill block takes up to 16 of a token's heads (its cos/sin computed once); decode spreads them
            static const unsigned hpb_env = [] { auto* e = std::getenv("FORGE_ROPE_HPB"); return e ? unsigned(std::strtoul(e, nullptr, 10)) : 0u; }();
            std::uint32_t hpb = hpb_env ? hpb_env : s >= 32 ? 16u : std::uint32_t(256 / d);   // 16: fastest for 512 x 64 x 128 on an RTX 3090
            hpb = std::min<std::uint32_t>(hpb, std::uint32_t(H));
            std::uint32_t su = std::uint32_t(s), hu = std::uint32_t(H), du = std::uint32_t(d), neox = a.num("neox", 1) != 0;
            const unsigned grid = su * ((hu + hpb - 1) / hpb);
            std::vector<std::uint64_t> p; bind_all(p);   // x, w, pos, y
            void* args[] = {&p[0], &p[1], &p[2], &tbl, &p[3], &su, &hu, &du, const_cast<float*>(&eps), &neox, &hpb, &mode};
            writes(p[3], Y.size() * 4);
            ck(launch_s(function("forge_rms_rope"), grid, 1, 1, 256, 1, 1, 0, stream_, args, nullptr), "launch");
            ++dispatches_["fused_rms_rope"];
            return true;
        }
        return false;
    }

private:
    // How run_op's rms_norm reduces rows x cols inside a run: 1 = forge_rms_norm_q81 (warp shuffles; *block 256, or 1024
    // for a few wide rows), 0 = forge_rms_norm_b (tree over 256 threads). The fused kernels follow the same choice.
    unsigned rms_mode(std::uint32_t rows, std::uint32_t cols, unsigned* block) {
        *block = 256;
        if (fuse_q81() && use_mmvq() && rows < mmq_min_rows() && cols % 32 == 0 && has_fn_locked("forge_rms_norm_q81")) {
            if (rows <= 4 && cols >= 2048) *block = 1024;
            return 1;
        }
        return 0;
    }
    // a prefill rms_norm output that the next matmul quantises for the tensor cores (run_qmatmul's MMA conditions).
    // Only model-width rows (>= 1024): per-head norms (QK-norm, head dim <= 256) feed rope/attention, not a matmul, and
    // emitting for them would also evict the q/k/v projections' shared input from the scratch.
    bool q8r_emit(std::uint32_t rows, std::uint32_t cols) {
        return in_run_ && fuse_q8r() && use_mma() && rows >= mmq_min_rows() && cols % 256 == 0 && cols >= 1024 &&
               has_fn_locked("forge_quant_q8r") && has_fn_locked("forge_rms_norm_f");
    }
    // the q8r scratch laid out as run_qmatmul's MMA path expects: int8 rows (padded to 256 bytes), then (d, d*sum) pairs
    std::pair<std::uint64_t, std::uint64_t> q8r_slot(std::uint32_t rows, std::uint32_t k) {
        const std::size_t xbytes = (std::size_t(rows) * k + 255) / 256 * 256, nb = std::size_t(rows) * (k / 32);
        q8r_reserve(xbytes + nb * 8);
        return {q8r_, q8r_ + xbytes};
    }
    explicit runtime(api a) : api_(a) {}
    // Driver entry points, normalised: CUDA and HIP module APIs have the same shapes. Optional ones may be null.
    struct fns {
        int (*init)(unsigned) = nullptr;
        int (*count)(int*) = nullptr;
        int (*name)(char*, int, int) = nullptr;
        int (*device_get)(int*, int) = nullptr;               // CUDA
        int (*dev_attr)(int*, int, int) = nullptr;             // cuDeviceGetAttribute (multiprocessor count)
        int (*ctx_create)(void**, unsigned, int) = nullptr;   // CUDA
        int (*ctx_destroy)(void*) = nullptr;
        int (*ctx_set)(void*) = nullptr;                      // CUDA
        int (*set_device)(int) = nullptr;                      // HIP
        int (*load)(void**, const void*) = nullptr;
        int (*unload)(void*) = nullptr;
        int (*get_fn)(void**, void*, const char*) = nullptr;
        int (*malloc_)(std::uint64_t*, std::size_t) = nullptr;
        int (*free_)(std::uint64_t) = nullptr;
        int (*htod)(std::uint64_t, const void*, std::size_t) = nullptr;
        int (*dtoh)(void*, std::uint64_t, std::size_t) = nullptr;
        int (*htod_a)(std::uint64_t, const void*, std::size_t, void*) = nullptr;
        int (*memset32)(std::uint64_t, unsigned, std::size_t, void*) = nullptr;   // cuMemsetD32Async / hipMemsetD32Async
        int (*dtoh_a)(void*, std::uint64_t, std::size_t, void*) = nullptr;
        int (*dtod_a)(std::uint64_t, std::uint64_t, std::size_t, void*) = nullptr;
        int (*launch)(void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void*, void**, void**) = nullptr;
        int (*sync_)() = nullptr;
        int (*stream_create)(void**, unsigned) = nullptr;
        int (*stream_destroy)(void*) = nullptr;
        int (*stream_sync)(void*) = nullptr;
        int (*capture_begin)(void*, int) = nullptr;
        int (*capture_end)(void*, void**) = nullptr;
        int (*graph_inst)(void**, void*, unsigned long long) = nullptr;
        int (*graph_launch)(void*, void*) = nullptr;
        int (*gexec_destroy)(void*) = nullptr;
        int (*graph_destroy)(void*) = nullptr;
        int (*exec_update)(void*, void*, void*) = nullptr;   // cuGraphExecUpdate_v2 (CUDA only)
        int (*param_info)(void*, std::size_t, std::size_t*, std::size_t*) = nullptr;   // cuFuncGetParamInfo (CUDA 12.4+)
        int (*kn_set)(void*, void*, const void*) = nullptr;                             // cuGraphExecKernelNodeSetParams
        int (*g_roots)(void*, void**, std::size_t*) = nullptr;                          // cuGraphGetRootNodes
        int (*g_deps)(void*, void**, std::size_t*) = nullptr;                           // cuGraphNodeGetDependentNodes
        int (*g_type)(void*, int*) = nullptr;                                           // cuGraphNodeGetType
        int (*peer)(std::uint64_t, std::uintptr_t, std::uint64_t, std::uintptr_t, std::size_t) = nullptr;
        int (*mem_info)(std::size_t*, std::size_t*) = nullptr;
        int (*func_attr)(void*, int, int) = nullptr;   // cuFuncSetAttribute (dynamic shared memory above 48 KB)
        int (*host_alloc)(void**, std::size_t) = nullptr;   // cuMemAllocHost_v2 (page-locked staging; CUDA only)
        int (*host_free)(void*) = nullptr;                  // cuMemFreeHost
        int (*occupancy)(int*, void*, int, std::size_t) = nullptr;   // cuOccupancyMaxActiveBlocksPerMultiprocessor
        // wrappers that fall back to the synchronous copies when the async ones are missing
        int htod_async(std::uint64_t d, const void* h, std::size_t n, void* s) const { return htod_a ? htod_a(d, h, n, s) : htod(d, h, n); }
        int dtoh_async(void* h, std::uint64_t d, std::size_t n, void* s) const { return dtoh_a ? dtoh_a(h, d, n, s) : dtoh(h, d, n); }
        int dtod_async(std::uint64_t d, std::uint64_t s, std::size_t n, void* st) const {
            if (dtod_a) return dtod_a(d, s, n, st);
            std::vector<char> t(n); int r = dtoh(t.data(), s, n); return r ? r : htod(d, t.data(), n);
        }
    };
    struct entry { float* host; std::size_t count; std::uint64_t d; bool host_valid = true, pinned = false, kept = false, arena = false; std::uint64_t last = 0; };
    struct qbuf { std::uint64_t d = 0, last = 0; };   // an encoded weight kept on the device (run_qmatmul / run_qgather)
    struct bound { entry* e; std::uint64_t ptr; };

    template<class F> bool sym(F& f, const char* n) { f = reinterpret_cast<F>(dlsym(lib_, n)); return f != nullptr; }
    template<class F> void opt(F& f, const char* n) { f = reinterpret_cast<F>(dlsym(lib_, n)); }
    void ck(int r, const char* what) {
        if (r != 0) throw std::runtime_error(std::string(api_name(api_)) + ": " + what + " failed (" + std::to_string(r) + ")");
    }
    // ---- decode graphs (FORGE_GRAPHS=1, CUDA): inside a run, kernel launches are stream-captured; a host copy, a
    // free or a sync ends the segment, which is replayed through an exec graph cached per segment index and refreshed
    // with cuGraphExecUpdate (same topology every token, only pointers/scalars change), as llama.cpp's ggml-cuda does.
    bool gmode() const {
        // on by default (decode: one graph launch per segment instead of a launch per kernel); FORGE_GRAPHS=0 turns it off
        static const bool on = [] { auto* e = std::getenv("FORGE_GRAPHS"); return !(e && std::string(e) == "0"); }();
        return on && api_ == api::cuda && stream_ && has_graphs();
    }
    bool capturing_ = false, user_cap_ = false; std::size_t seg_ = 0;
    // A segment's kernels (function, launch shape, argument bytes). Once a segment has been captured and instantiated,
    // later tokens replay it without capturing: launches are only recorded, and if the sequence of functions and
    // shapes matches, flush() patches the kernel nodes whose arguments changed (cuGraphExecKernelNodeSetParams) and
    // relaunches the exec graph -- no capture, no cuGraphExecUpdate. A mismatch falls back to capturing the recorded
    // launches. Needs cuFuncGetParamInfo (CUDA 12.4+); without it every segment is captured and exec-updated.
    struct krec { void* fn; unsigned d[7]; std::uint32_t off, len; };
    struct segc { void* exec = nullptr; void* graph = nullptr; std::vector<void*> nodes; std::vector<krec> recs; std::vector<unsigned char> bytes, prev, pred; bool replay = false, hit = false; };
    // pred = bytes + (bytes - prev), per parameter (8-byte and 4-byte integers; other sizes word by word)
    void predict(segc& c) {
        c.pred = c.bytes;
        if (c.prev.size() != c.bytes.size()) { c.pred.clear(); return; }
        for (auto& r : c.recs) {
            std::size_t at = r.off;
            for (auto z : *params_of(r.fn)) {
                at = (at + 7) & ~std::size_t(7);
                if (z == 8) { std::uint64_t b, p; std::memcpy(&b, &c.bytes[at], 8); std::memcpy(&p, &c.prev[at], 8); b += b - p; std::memcpy(&c.pred[at], &b, 8); }
                else for (std::size_t w = 0; w + 4 <= z; w += 4) { std::uint32_t b, p; std::memcpy(&b, &c.bytes[at + w], 4); std::memcpy(&p, &c.prev[at + w], 4); b += b - p; std::memcpy(&c.pred[at + w], &b, 4); }
                at += z;
            }
        }
    }
    const void* run_key_ = nullptr; std::size_t streak_ = 0, nseg_ = 0; bool dirty_ = false, replayed_ = false;
    std::set<const void*> replay_bad_;
    std::vector<segc> segc_;
    std::vector<krec> prec_; std::vector<unsigned char> pbytes_;
    bool recording_ = false, deferring_ = false;
    std::unordered_map<void*, std::vector<std::uint32_t>> psize_;   // per function: parameter sizes
    bool replay_ok() const {
        static const bool on = [] { auto* e = std::getenv("FORGE_GRAPH_REPLAY"); return !(e && std::string(e) == "0"); }();
        return on && f_.param_info && f_.kn_set && f_.g_roots && f_.g_deps && f_.g_type;
    }
    const std::vector<std::uint32_t>* params_of(void* fn) {
        auto it = psize_.find(fn); if (it != psize_.end()) return &it->second;
        std::vector<std::uint32_t> v;
        for (std::size_t i = 0; i < 64; ++i) { std::size_t o = 0, z = 0; if (f_.param_info(fn, i, &o, &z) != 0) break; v.push_back(std::uint32_t(z)); }
        return &(psize_[fn] = std::move(v));
    }
    void record(void* fn, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz, unsigned sh, void** a) {
        krec r{fn, {gx, gy, gz, bx, by, bz, sh}, std::uint32_t(pbytes_.size()), 0};
        if (replay_ok()) for (auto z : *params_of(fn)) {
            std::size_t at = (pbytes_.size() + 7) & ~std::size_t(7); pbytes_.resize(at + z);
            std::memcpy(pbytes_.data() + at, *a++, z);
        }
        r.len = std::uint32_t(pbytes_.size() - r.off); prec_.push_back(r);
    }
    // the kernelParams array of record r, pointing into bytes
    void arg_ptrs(const krec& r, unsigned char* bytes, std::vector<void*>& out) {
        out.clear(); std::size_t at = r.off;
        for (auto z : *params_of(r.fn)) { at = (at + 7) & ~std::size_t(7); out.push_back(bytes + at); at += z; }
    }
    void reissue_captured() {   // begin capturing and launch every recorded kernel of the segment
        ck(f_.capture_begin(stream_, 2), "stream begin capture"); capturing_ = true; deferring_ = false;
        std::vector<void*> ap;
        for (auto& r : prec_) { arg_ptrs(r, pbytes_.data(), ap); ck(f_.launch(r.fn, r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6], stream_, ap.data(), nullptr), "launch"); }
    }
    int launch_s(void* fn, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz, unsigned sh, void* st, void** a, void** ex) {
        if (!q81_keep_ && q81_src_) {
            bool hit = nwr_ == 0 || nwr_ > 3; const std::uint64_t lo = q81_src_, hi = lo + std::uint64_t(q81_n_) * 128;
            for (unsigned i = 0; i < nwr_ && i < 3; ++i) if (wr_[i][0] < hi && lo < wr_[i][1]) hit = true;
            if (hit) q81_src_ = 0;
        }
        if (!q81_keep_ && q8r_src_) {   // the same rule for the q8r scratch's source
            bool hit = nwr_ == 0 || nwr_ > 3; const std::uint64_t lo = q8r_src_, hi = lo + std::uint64_t(q8r_n_) * 4;
            for (unsigned i = 0; i < nwr_ && i < 3; ++i) if (wr_[i][0] < hi && lo < wr_[i][1]) hit = true;
            if (hit) q8r_src_ = 0;
        }
        nwr_ = 0;
        if (!(in_run_ && !user_cap_ && gmode() && st == stream_ && !ex)) {
            count(user_cap_ ? counters().captured : counters().launches); if (in_run_) dirty_ = true;
            return f_.launch(fn, gx, gy, gz, bx, by, bz, sh, st, a, ex);
        }
        if (!recording_) {
            recording_ = true; prec_.clear(); pbytes_.clear();
            if (seg_ >= segc_.size()) segc_.resize(seg_ + 1);
            auto& c = segc_[seg_];
            deferring_ = replay_ok() && c.replay && c.exec;
            if (!deferring_) { ck(f_.capture_begin(stream_, 2), "stream begin capture"); capturing_ = true; }
        }
        record(fn, gx, gy, gz, bx, by, bz, sh, a);
        count(counters().captured);
        int r = 0;
        if (deferring_) {
            auto& c = segc_[seg_]; std::size_t i = prec_.size() - 1;
            if (i >= c.recs.size() || c.recs[i].fn != fn || std::memcmp(c.recs[i].d, prec_[i].d, sizeof prec_[i].d) || c.recs[i].len != prec_[i].len)
                reissue_captured();   // the segment changed shape: capture it after all
        } else r = f_.launch(fn, gx, gy, gz, bx, by, bz, sh, st, a, ex);
        // Split long segments so the GPU runs one while the host records the next ($FORGE_GRAPH_SEG kernels, 0 = off).
        // The first segments of a run are shorter ($FORGE_GRAPH_SEG0 kernels, doubling up to $FORGE_GRAPH_SEG): the
        // device idles from the previous run's end until the first launch, so that one goes out early.
        if (r == 0 && ++seg_kernels_ >= seg_limit()) flush();
        return r;
    }
    std::size_t seg_kernels_ = 0;
    std::size_t seg_limit() const {
        static const std::size_t s0 = [] { auto* e = std::getenv("FORGE_GRAPH_SEG0"); return std::size_t(e ? std::strtoul(e, nullptr, 10) : 8); }();
        const std::size_t full = graph_seg();
        if (!s0 || seg_ >= 16) return full;
        return std::min(full, s0 << seg_);
    }
    static std::size_t graph_seg() {
        static const std::size_t v = [] { auto* e = std::getenv("FORGE_GRAPH_SEG"); std::size_t n = e ? std::strtoul(e, nullptr, 10) : 64; return n ? n : ~std::size_t(0); }();
        return v;
    }
    // Upload, or clear on the device when the host bytes are all zero (a fresh KV slot or recurrent state): a large
    // pageable upload of zeros costs far more than scanning it. Small copies go straight through.
    static bool all_zero(const void* h, std::size_t n) {
        auto* p = static_cast<const unsigned char*>(h); std::size_t i = 0;
        for (; i + 8 <= n; i += 8) { std::uint64_t w; std::memcpy(&w, p + i, 8); if (w) return false; }
        for (; i < n; ++i) if (p[i]) return false;
        return true; }
    int htod_or_zero(std::uint64_t d, const void* h, std::size_t n, void* st) {
        if (f_.memset32 && n >= (64u << 10) && n % 4 == 0 && all_zero(h, n)) {
            flush(); q_written(d, n); dirty_ |= in_run_; ++stats_.zero_fills; return f_.memset32(d, 0u, n / 4, st); }
        return htod_s(d, h, n, st); }
    // a copy wrote device range [d, d + n): drop the quantised-activation scratches whose source it overlaps
    void q_written(std::uint64_t d, std::size_t n) {
        if (q81_src_ && d < q81_src_ + std::uint64_t(q81_n_) * 128 && q81_src_ < d + n) q81_src_ = 0;
        if (q8r_src_ && d < q8r_src_ + q8r_n_ * 4 && q8r_src_ < d + n) q8r_src_ = 0;
        if (g8_src_ && d < g8_src_ + std::uint64_t(g8_n_) * 128 && g8_src_ < d + n) g8_src_ = 0;
    }
    int htod_s(std::uint64_t d, const void* h, std::size_t n, void* st) {
        flush(); q_written(d, n); dirty_ |= in_run_; cuda_hip::count(counters().htod);
        if (void* p = st == stream_ ? stage(n) : nullptr) { std::memcpy(p, h, n); return f_.htod_async(d, p, n, st); }   // a real async copy
        return f_.htod_async(d, h, n, st); }
    // Small host-to-device copies (decode: the token id and position each run) go through a page-locked staging
    // ring: from pageable memory the driver stages them itself, synchronously (tens of us each under WSL). A slot is
    // reused only after the next sync(), which drains every copy issued before it; when the ring is full, or while a
    // stream capture is open (a memcpy node would keep pointing at a reused slot), the pageable path is taken.
    // $FORGE_GPU_STAGE=0 turns it off.
    static constexpr std::size_t stage_cap = 64 << 10, stage_max = 4096;
    void* stage_ = nullptr; std::size_t stage_off_ = 0; bool stage_tried_ = false;
    static bool stage_on() { static const bool on = [] { auto* e = std::getenv("FORGE_GPU_STAGE"); return !(e && std::string(e) == "0"); }(); return on; }
    void* stage(std::size_t n, bool reserve_tail = false) {
        if (n > stage_max || capturing_ || user_cap_ || !stage_on() || !f_.host_alloc || !f_.htod_a) return nullptr;
        if (!stage_tried_) { stage_tried_ = true; if (f_.host_alloc(&stage_, stage_cap) != 0) stage_ = nullptr; }
        if (!stage_) return nullptr;
        if (reserve_tail) return static_cast<char*>(stage_) + stage_cap - 64;   // the argmax read-back slot
        if (stage_off_ + n > stage_cap - 64) return nullptr;
        void* p = static_cast<char*>(stage_) + stage_off_; stage_off_ += (n + 63) & ~std::size_t(63); return p; }
    int dtoh_s(void* h, std::uint64_t d, std::size_t n, void* st) { flush(); dirty_ |= in_run_; cuda_hip::count(counters().dtoh); return f_.dtoh_async(h, d, n, st); }
    int free_s(std::uint64_t p) { flush(); q81_drop(); dirty_ |= in_run_; return dfree(p); }
    // ---- device memory accounting (gpu/device_memory.hpp) ----
    // Every driver allocation goes through dmalloc/dfree, so the pool "cuda:N" / "hip:N" sees it. A buffer
    // touched (acquired) after begin_op() belongs to the op in progress and is never evicted by it.
    device_memory dm_;
    std::uint64_t clock_ = 0, op_mark_ = 0;
    void begin_op() { op_mark_ = clock_; }
    std::uint64_t dmalloc(std::size_t bytes, const char* tag) {
        bytes = std::max<std::size_t>(4, bytes);
        return dm_.allocate(bytes, tag, [&]() -> std::optional<std::uint64_t> { std::uint64_t p = 0; if (f_.malloc_(&p, bytes) != 0 || !p) return std::nullopt; return p; });
    }
    int dfree(std::uint64_t p) { dm_.release(p); return f_.free_(p); }
    // Make room on the device (device_memory's evict callback; mu_ held): give back the pooled free buffers,
    // then evict resident buffers and encoded weights least recently used first, syncing device-only data to
    // the host first. The next op that needs one uploads it again (FORGE_GPU_NO_RESIDENCY behaviour on demand).
    std::uint64_t evict_device(std::uint64_t want, bool external) {
        if (user_cap_) return 0;   // a user stream capture is open: nothing may be freed or synced
        if (external) op_mark_ = clock_;   // no op in progress: every buffer may go
        bind(); flush();
        std::uint64_t freed = 0;
        for (auto& [b, p] : pool_) { free_s(p); freed += b; ++stats_.evictions; stats_.evicted_bytes += b; }
        pool_.clear(); pool_bytes_ = 0;
        if (want && freed >= want) return freed;
        struct victim { std::uint64_t last; bool q; std::uintptr_t key; std::pair<std::uintptr_t, std::size_t> qkey; std::size_t bytes; };
        std::vector<victim> vs;
        for (auto& [k, e] : res_) if (e.last <= op_mark_) vs.push_back({e.last, false, k, {}, std::max<std::size_t>(4, e.count * 4)});
        for (auto& [k, qb] : qres_) if (qb.d && qb.last <= op_mark_) vs.push_back({qb.last, true, 0, k, std::max<std::size_t>(4, k.second)});
        std::sort(vs.begin(), vs.end(), [](const victim& a, const victim& b) { return a.last < b.last; });
        std::size_t n = 0; std::uint64_t got = freed;
        while (n < vs.size() && (!want || got < want)) got += vs[n++].bytes;
        if (!n) return freed;
        for (std::size_t i = 0; i < n; ++i) if (!vs[i].q) { auto& e = res_[vs[i].key]; if (!e.host_valid) download(e); }
        sync();   // downloads done, and no queued kernel still reads a buffer about to be freed
        for (std::size_t i = 0; i < n; ++i) {
            auto& v = vs[i];
            if (v.q) { free_s(qres_[v.qkey].d); qres_.erase(v.qkey); }
            else { auto& e = res_[v.key]; stats_.resident_bytes -= e.count * 4; free_s(e.d); res_.erase(v.key); }
            freed += v.bytes; ++stats_.evictions; stats_.evicted_bytes += v.bytes;
        }
        q81_drop(); streak_ = 0;   // device addresses moved: no whole-run replay until runs are re-confirmed
        return freed;
    }
    // device-to-device copy on the stream; inside a graph-mode run it is a kernel (forge_copy_words), so segments hold
    // kernel nodes only and replay can patch them
    int dtod_s(std::uint64_t d, std::uint64_t s, std::size_t n) {
        cuda_hip::count(counters().dtod); q_written(d, n);
        if (in_run_ && gmode() && n % 4 == 0 && n / 4 < (1ull << 32) && has_fn_locked("forge_copy_words")) {
            std::uint32_t w = std::uint32_t(n / 4); void* args[] = {&s, &d, &w}; writes(d, n);
            return w ? launch_s(function("forge_copy_words"), (w + 255) / 256, 1, 1, 256, 1, 1, 0, stream_, args, nullptr) : 0;
        }
        if (deferring_) reissue_captured();   // a memcpy node: this segment is captured (and not replayed)
        dirty_ |= in_run_;
        return f_.dtod_async(d, s, n, stream_);
    }
    void drop_seg(segc& c) {
        if (c.exec && f_.gexec_destroy) f_.gexec_destroy(c.exec);
        if (c.graph && f_.graph_destroy) f_.graph_destroy(c.graph);
        c = segc{};
    }
    // the kernel nodes of a captured single-stream graph, in launch order (a chain from its one root)
    bool chain(void* g, std::vector<void*>& out) {
        out.clear(); std::size_t n = 1; void* node = nullptr;
        if (f_.g_roots(g, &node, &n) != 0 || n != 1) return false;
        while (node) {
            int t = -1; if (f_.g_type(node, &t) != 0 || t != 0 /* CU_GRAPH_NODE_TYPE_KERNEL */) return false;
            out.push_back(node);
            void* next = nullptr; std::size_t m = 0;
            if (f_.g_deps(node, nullptr, &m) != 0 || m > 1) return false;
            if (m == 1 && f_.g_deps(node, &next, &m) != 0) return false;
            node = m ? next : nullptr;
        }
        return true;
    }
    void flush() {
        if (!recording_) return;
        recording_ = false; seg_kernels_ = 0;
        auto& c = segc_[seg_++];
        if (deferring_) {
            deferring_ = false;
            bool ok = prec_.size() == c.recs.size();
            std::vector<void*> ap;
            for (std::size_t i = 0; ok && i < prec_.size(); ++i) {
                auto& r = prec_[i];
                if (!std::memcmp(pbytes_.data() + r.off, c.bytes.data() + c.recs[i].off, r.len)) continue;
                arg_ptrs(r, pbytes_.data(), ap);
                struct { void* fn; unsigned d[7]; void** kp; void** ex; } kp{r.fn, {r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6]}, ap.data(), nullptr};
                if (f_.kn_set(c.exec, c.nodes[i], &kp) != 0) ok = false; else cuda_hip::count(counters().node_updates);
            }
            if (ok) {
                c.hit = c.pred.size() == pbytes_.size() && !std::memcmp(c.pred.data(), pbytes_.data(), pbytes_.size());
                c.recs.swap(prec_); c.prev.swap(c.bytes); c.bytes.swap(pbytes_);
                cuda_hip::count(counters().graph_launches); ck(f_.graph_launch(c.exec, stream_), "graph launch");
                return;
            }
            reissue_captured();   // fewer kernels than last time, or a patch failed (exec state is suspect: rebuild)
            drop_seg(c);
        }
        capturing_ = false;
        void* g = nullptr; ck(f_.capture_end(stream_, &g), "stream end capture");
        void* done = nullptr;   // destroyed after the launch (a driver may share storage between graph and exec)
        if (replay_ok()) {   // keep the graph: replay patches its nodes
            drop_seg(c);
            ck(f_.graph_inst(&c.exec, g, 0), "graph instantiate"); cuda_hip::count(counters().instantiates);
            c.graph = g; c.replay = chain(g, c.nodes) && c.nodes.size() == prec_.size();
            c.recs.swap(prec_); c.bytes.swap(pbytes_);
        } else {
            alignas(8) char info[64] = {};
            if (c.exec && !(f_.exec_update && f_.exec_update(c.exec, g, info) == 0)) { f_.gexec_destroy(c.exec); c.exec = nullptr; }
            if (!c.exec) { ck(f_.graph_inst(&c.exec, g, 0), "graph instantiate"); cuda_hip::count(counters().instantiates); }
            done = g;
        }
        cuda_hip::count(counters().graph_launches); ck(f_.graph_launch(c.exec, stream_), "graph launch");
        if (done && f_.graph_destroy) f_.graph_destroy(done);
    }
    void need_graphs() { if (!has_graphs()) throw std::runtime_error(std::string(api_name(api_)) + ": driver has no graph API"); }
    // The context made current on this thread by the last bind() (any runtime): cuCtxSetCurrent is skipped when it is
    // already ours, instead of once per op.
    // already ours, instead of once per op. Destroying any context bumps the epoch, which invalidates every cache.
    struct bound_t { void* c = nullptr; unsigned long e = 0; };
    static bound_t& bound_ctx() { thread_local bound_t b; return b; }
    static std::atomic<unsigned long>& ctx_epoch() { static std::atomic<unsigned long> e{1}; return e; }
    void bind() {
        if (api_ == api::cuda) {
            auto& b = bound_ctx(); const unsigned long e = ctx_epoch().load(std::memory_order_acquire);
            if (f_.ctx_set && ctx_ && (b.c != ctx_ || b.e != e)) { f_.ctx_set(ctx_); b.c = ctx_; b.e = e; }
        }
        else if (f_.set_device) f_.set_device(ordinal_);
    }
    void sync() { flush(); dirty_ |= in_run_; cuda_hip::count(counters().syncs); if (stream_ && f_.stream_sync) ck(f_.stream_sync(stream_), "stream synchronize"); else ck(f_.sync_(), "synchronize"); stage_off_ = 0; }
    void* function(const std::string& n) {
        auto it = fn_.find(n); if (it != fn_.end()) return it->second;
        void* f = nullptr; ck(f_.get_fn(&f, mod_, n.c_str()), ("get function " + n).c_str());
        return fn_[n] = f;
    }
    template<class F> void each_overlap(const float* p, std::size_t n, F&& f) {
        auto lo = reinterpret_cast<std::uintptr_t>(p), hi = lo + std::max<std::size_t>(n, 1) * 4;
        // entries are disjoint host ranges (acquire splits or merges overlaps), so only the one starting before lo can
        // reach into the range: a log-time seek instead of a scan of every resident buffer (decode calls this per state
        // slot per token)
        auto it = res_.upper_bound(lo);
        if (it != res_.begin()) { auto pv = std::prev(it); if (pv->first + pv->second.count * 4 > lo) it = pv; }
        for (; it != res_.end() && it->first < hi; ++it) { auto el = it->first, eh = el + it->second.count * 4; if (el < hi && lo < eh) f(it->second); }
    }
    void download(entry& e) { ck(dtoh_s(e.host, e.d, e.count * 4, stream_), "memcpy DtoH"); e.host_valid = true; ++stats_.downloads; }
    // freed device buffers are pooled by size and reused: decode runs the same shapes every token
    void drop(entry& e) {
        stats_.resident_bytes -= e.count * 4;
        if (pool_bytes_ + e.count * 4 > pool_cap()) { free_s(e.d); return; }
        pool_.emplace(std::max<std::size_t>(4, e.count * 4), e.d); pool_bytes_ += std::max<std::size_t>(4, e.count * 4);
    }
    void dev_alloc(std::uint64_t* d, std::size_t bytes) {
        bytes = std::max<std::size_t>(4, bytes);
        if (auto it = pool_.find(bytes); it != pool_.end()) { *d = it->second; pool_.erase(it); pool_bytes_ -= bytes; return; }
        *d = dmalloc(bytes, "resident");   // out of memory: device_memory evicts (the pool first, then LRU) and retries
    }
    static std::size_t pool_cap() {
        static const std::size_t v = [] { const char* e = std::getenv("FORGE_GPU_POOL_MB"); return (e ? std::strtoull(e, nullptr, 10) : 4096ull) << 20; }();
        return v;
    }
    bound acquire(float* h, std::size_t count, bool is_out) {
        auto key = reinterpret_cast<std::uintptr_t>(h);
        if (auto it = res_.find(key); it != res_.end() && it->second.count == count) { it->second.last = ++clock_; return {&it->second, it->second.d}; }
        auto it = res_.upper_bound(key);
        if (it != res_.begin()) {   // a sub-range of a resident buffer (e.g. the live prefix of a KV slot)
            auto& c = std::prev(it)->second; auto cl = std::prev(it)->first;
            if (cl + c.count * 4 >= key + count * 4 && count) { c.last = ++clock_; return {&c, c.d + (key - cl)}; }
        }
        // Resident entries overlapping [h, h+count) (decode re-views the same host arenas with new extents every
        // token): the overlapping part moves device-to-device into the new buffer and any part sticking out stays
        // resident as its own entry, so nothing goes through the host and the stream never syncs. Pinned/kept
        // entries that stick out are fetched to the host as before.
        std::vector<std::uintptr_t> olds, fetched; bool dl = false;
        const auto lo = key, hi = key + std::max<std::size_t>(count, 1) * 4;
        each_overlap(h, count, [&](entry& o) {
            o.last = ++clock_;   // consumed below: not evictable while the new buffer is allocated
            auto ol = reinterpret_cast<std::uintptr_t>(o.host), oh = ol + o.count * 4;
            if ((o.pinned || o.kept) && !(ol >= lo && oh <= hi)) { if (!o.host_valid) { download(o); dl = true; } fetched.push_back(ol); }
            else olds.push_back(ol); });
        if (dl) sync();
        for (auto k : fetched) { drop(res_[k]); res_.erase(k); }
        entry e{h, count, 0};
        dev_alloc(&e.d, count * 4); e.last = ++clock_;
        for (auto& [p, n] : pinned_) if (p == h && n == count) e.pinned = true;
        std::sort(olds.begin(), olds.end());
        if (!is_out) {   // upload only the gaps the resident pieces do not cover
            std::uintptr_t at = lo;
            auto up = [&](std::uintptr_t x, std::uintptr_t y) { if (y > x) { ck(htod_or_zero(e.d + (x - lo), reinterpret_cast<const void*>(x), y - x, stream_), "memcpy HtoD"); ++stats_.uploads; } };
            for (auto k : olds) { up(at, std::max(k, lo)); at = std::max(at, std::min(hi, k + res_[k].count * 4)); }
            up(at, lo + count * 4);
        }
        std::vector<entry> rest;
        for (auto k : olds) {
            entry o = res_[k]; res_.erase(k);
            auto ol = k, oh = k + o.count * 4, a = std::max(ol, lo), z = std::min(oh, hi);
            if (!is_out && z > a) { ck(dtod_s(e.d + (a - lo), o.d + (a - ol), z - a), "memcpy DtoD"); }
            if (!o.host_valid) e.host_valid = false;
            if (o.kept) e.kept = true;
            if (!o.host_valid) for (auto [x, y] : {std::pair{ol, std::min(oh, lo)}, std::pair{std::max(ol, hi), oh}}) if (y > x) {
                entry r{reinterpret_cast<float*>(x), (y - x) / 4, 0}; r.host_valid = false;
                dev_alloc(&r.d, y - x); ck(dtod_s(r.d, o.d + (x - ol), y - x), "memcpy DtoD");
                stats_.resident_bytes += y - x; rest.push_back(r);
            }
            drop(o);
        }
        for (auto& r : rest) res_.emplace(reinterpret_cast<std::uintptr_t>(r.host), r);
        stats_.resident_bytes += count * 4; stats_.peak_resident_bytes = std::max(stats_.peak_resident_bytes, stats_.resident_bytes);
        auto& r = res_.emplace(key, e).first->second; return {&r, r.d};
    }
    std::string load_lib() {
        const char* libs_cuda[] = {"libcuda.so.1", "libcuda.so", "nvcuda.dll", nullptr};
        const char* libs_hip[] = {"libamdhip64.so.6", "libamdhip64.so.5", "libamdhip64.so", "amdhip64.dll", nullptr};
        for (auto p = api_ == api::cuda ? libs_cuda : libs_hip; *p; ++p) if ((lib_ = dlopen(*p, RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib_) return api_ == api::cuda ? "libcuda not found (no NVIDIA driver)" : "libamdhip64 not found (no ROCm runtime)";
        bool ok;
        if (api_ == api::cuda) {
            ok = sym(f_.init, "cuInit") && sym(f_.count, "cuDeviceGetCount") && sym(f_.name, "cuDeviceGetName") &&
                 sym(f_.ctx_create, "cuCtxCreate_v2") && sym(f_.ctx_destroy, "cuCtxDestroy_v2") && sym(f_.load, "cuModuleLoadData") &&
                 sym(f_.unload, "cuModuleUnload") && sym(f_.get_fn, "cuModuleGetFunction") && sym(f_.malloc_, "cuMemAlloc_v2") &&
                 sym(f_.free_, "cuMemFree_v2") && sym(f_.htod, "cuMemcpyHtoD_v2") && sym(f_.dtoh, "cuMemcpyDtoH_v2") &&
                 sym(f_.launch, "cuLaunchKernel") && sym(f_.sync_, "cuCtxSynchronize");
            opt(f_.device_get, "cuDeviceGet"); opt(f_.ctx_set, "cuCtxSetCurrent"); opt(f_.dev_attr, "cuDeviceGetAttribute");
            opt(f_.htod_a, "cuMemcpyHtoDAsync_v2"); opt(f_.memset32, "cuMemsetD32Async"); opt(f_.dtoh_a, "cuMemcpyDtoHAsync_v2"); opt(f_.dtod_a, "cuMemcpyDtoDAsync_v2");
            opt(f_.stream_create, "cuStreamCreate"); opt(f_.stream_destroy, "cuStreamDestroy_v2"); opt(f_.stream_sync, "cuStreamSynchronize");
            opt(f_.capture_begin, "cuStreamBeginCapture_v2"); opt(f_.capture_end, "cuStreamEndCapture");
            opt(f_.graph_inst, "cuGraphInstantiateWithFlags"); opt(f_.exec_update, "cuGraphExecUpdate_v2");
            opt(f_.param_info, "cuFuncGetParamInfo"); opt(f_.kn_set, "cuGraphExecKernelNodeSetParams");
            opt(f_.g_roots, "cuGraphGetRootNodes"); opt(f_.g_deps, "cuGraphNodeGetDependentNodes"); opt(f_.g_type, "cuGraphNodeGetType"); opt(f_.graph_launch, "cuGraphLaunch");
            opt(f_.gexec_destroy, "cuGraphExecDestroy"); opt(f_.graph_destroy, "cuGraphDestroy");
            opt(f_.peer, "cuMemcpyPeer"); opt(f_.mem_info, "cuMemGetInfo_v2"); opt(f_.func_attr, "cuFuncSetAttribute"); opt(f_.occupancy, "cuOccupancyMaxActiveBlocksPerMultiprocessor");
            opt(f_.host_alloc, "cuMemAllocHost_v2"); opt(f_.host_free, "cuMemFreeHost"); if (!f_.host_free) f_.host_alloc = nullptr;
        } else {
            ok = sym(f_.init, "hipInit") && sym(f_.count, "hipGetDeviceCount") && sym(f_.name, "hipDeviceGetName") &&
                 sym(f_.set_device, "hipSetDevice") && sym(f_.load, "hipModuleLoadData") && sym(f_.unload, "hipModuleUnload") &&
                 sym(f_.get_fn, "hipModuleGetFunction") && sym(f_.malloc_, "hipMalloc") && sym(f_.free_, "hipFree") &&
                 sym(f_.htod, "hipMemcpyHtoD") && sym(f_.dtoh, "hipMemcpyDtoH") && sym(f_.launch, "hipModuleLaunchKernel") &&
                 sym(f_.sync_, "hipDeviceSynchronize");
            opt(f_.htod_a, "hipMemcpyHtoDAsync"); opt(f_.memset32, "hipMemsetD32Async"); opt(f_.dtoh_a, "hipMemcpyDtoHAsync"); opt(f_.dtod_a, "hipMemcpyDtoDAsync");
            opt(f_.stream_create, "hipStreamCreateWithFlags"); opt(f_.stream_destroy, "hipStreamDestroy"); opt(f_.stream_sync, "hipStreamSynchronize");
            opt(f_.capture_begin, "hipStreamBeginCapture"); opt(f_.capture_end, "hipStreamEndCapture");
            opt(f_.graph_inst, "hipGraphInstantiateWithFlags"); opt(f_.graph_launch, "hipGraphLaunch");
            opt(f_.gexec_destroy, "hipGraphExecDestroy"); opt(f_.graph_destroy, "hipGraphDestroy");
            opt(f_.peer, "hipMemcpyPeer"); opt(f_.mem_info, "hipMemGetInfo");
        }
        return ok ? std::string{} : "driver library is missing entry points";
    }
    std::string init() {
        auto imgs = module_images(api_);
        if (imgs.empty()) return "this build has no embedded kernel image (no clang with the GPU target at build time)";
        if (auto e = load_lib(); !e.empty()) return e;
        if (f_.init(0) != 0) return "driver init failed";
        int n = 0; if (f_.count(&n) != 0 || n <= 0) return "no devices";
        if (ordinal_ < 0 || ordinal_ >= n) return "device ordinal " + std::to_string(ordinal_) + " out of range (" + std::to_string(n) + " devices)";
        int dev = ordinal_;
        if (api_ == api::cuda && f_.device_get && f_.device_get(&dev, ordinal_) != 0) return "cuDeviceGet failed";
        char buf[256] = {}; f_.name(buf, sizeof buf, dev); name_ = buf;
        if (int v = 0; api_ == api::cuda && f_.dev_attr && f_.dev_attr(&v, 16 /* MULTIPROCESSOR_COUNT */, dev) == 0 && v > 0) sms_ = unsigned(v);
        if (api_ == api::cuda) {
            // $FORGE_CUDA_SCHED: how a host thread waits on the device (auto | spin | yield | block): CU_CTX_SCHED_*
            const char* sc = std::getenv("FORGE_CUDA_SCHED"); const std::string s = sc ? sc : "";
            const unsigned fl = s == "spin" ? 1u : s == "yield" ? 2u : s == "block" ? 4u : 0u;
            if (f_.ctx_create(&ctx_, fl, dev) != 0) return "context creation failed";
        }
        else if (f_.set_device(ordinal_) != 0) return "hipSetDevice failed";
        if (f_.stream_create && f_.stream_create(&stream_, 1 /* non-blocking */) != 0) stream_ = nullptr;
        for (auto& [arch, img] : imgs) {
            std::string image(img);
            if (f_.load(&mod_, image.data()) == 0) {
                arch_ = arch;
                dm_.attach(std::string(api_name(api_)) + ":" + std::to_string(ordinal_), mu_, [this](std::uint64_t w, bool ext) { return evict_device(w, ext); });
                return {};
            }
            mod_ = nullptr;
        }
        return "module load failed (no embedded PTX/code object is accepted by this device)";
    }
    api api_;
    unsigned sms_ = 80;   // streaming multiprocessors (sizes split-K); 80 when the driver cannot say
    unsigned sms() const { return sms_; }
    int ordinal_ = 0;
    void* lib_ = nullptr; void* ctx_ = nullptr; void* mod_ = nullptr; void* stream_ = nullptr;
    fns f_;
    std::string name_, arch_;
    std::mutex mu_;
    bool in_run_ = false;
    std::map<std::uintptr_t, entry> res_;
    std::map<std::pair<std::uintptr_t, std::size_t>, qbuf> qres_;
    std::multimap<std::size_t, std::uint64_t> pool_; std::size_t pool_bytes_ = 0;
    std::map<std::vector<float>, std::uint64_t> rope_;   // encoded weights (run_qmatmul)
    std::vector<std::pair<const float*, std::size_t>> pinned_;
    std::vector<std::pair<void*, void*>> graphs_;
    std::map<std::string, void*> fn_;
    std::map<std::string, std::size_t> dispatches_;
    residency_stats stats_;
};

} // namespace forge::gpu::cuda_hip
