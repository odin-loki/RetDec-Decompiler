#pragma once
// Vulkan lowerings for the graph executor: every spec op with a generated GPU kernel gets a "vk.<op>" lowering
// (priority above the CPU ones). Like every lowering it must pass the oracle before it is selected, and any
// op/shape without a GPU kernel runs on the CPU, so a whole model runs with the GPU doing what it can.
#include <forge/graph/kernels.hpp>
#include <forge/graph/codecs.hpp>
#include <forge/gpu/vulkan.hpp>
#include <forge/gpu/cuda.hpp>
#include <forge/gpu/opencl.hpp>
#include <forge/gpu/levelzero.hpp>
#include <forge/gpu/metal.hpp>
#include <forge/graph/rpc.hpp>

namespace forge::graph {
#ifdef FORGE_GPU_VULKAN
// Last runtime opened by device_registry (for reporting dispatch counts).
inline std::shared_ptr<gpu::vk::runtime>& last_vulkan_runtime() { static std::shared_ptr<gpu::vk::runtime> r; return r; }
inline std::unique_ptr<registry> vulkan_registry(std::shared_ptr<gpu::vk::runtime> rt) {
    last_vulkan_runtime() = rt;
    auto r = registry::make();
    auto h = std::make_shared<device_hooks>();
    gpu::vk::runtime* raw = rt.get();
    h->begin_run = [raw] { raw->begin_run(); };
    h->end_run = [raw] { raw->end_run(); };
    h->pin = [raw](const float* p, std::size_t n) { raw->pin(p, n); };
    h->to_host = [raw](const float* p, std::size_t n) { raw->to_host(p, n); };
    h->host_wrote = [raw](const float* p, std::size_t n) { raw->host_wrote(p, n); };
    h->release = [raw](const float* p, std::size_t n) { raw->release(p, n); };
    h->copy = [raw](float* s, std::size_t n, float* d, std::size_t dn, std::size_t off) { raw->copy_resident(s, n, d, dn, off); };
    r->hooks = h;
    for (std::size_t i = 0; i < ops::op_count; ++i) {
        auto o = ops::op(i);
        std::string name(ops::info(o).name);
        if (!gpu::vk::runtime::has_kernel(name)) continue;
        r->add({"vk." + name, o, 20,
                [name](std::span<const shape_t> in, const shape_t&, const ops::attrs& a) {
                    bool ok = gpu::vk::supports(name, std::vector<shape_t>(in.begin(), in.end()), a); if (!ok && std::getenv("FORGE_VK_DEBUG")) { std::string s; for (auto& x : in) { for (auto d : x) s += std::to_string(d) + ","; s += "| "; } log::warn("vk: {} unsupported for {}", name, s); } return ok; },
                [rt, name](std::span<const tensor_view> in, tensor_view out, const ops::attrs& a) {
                    if (!gpu::vk::run_op(*rt, name, std::vector<tensor_view>(in.begin(), in.end()), a, out))
                        raise(errc::unsupported, "vulkan: {} rejected its inputs", name); }, -1, true,
                gpu::vk::transcendental_tol(name)});
    }
    // reshape on the device: a no-op when the planner aliased it in place, else a copy (scale by 1 is exact).
    r->add({"vk.reshape", ops::op::reshape, 20, nullptr,
            [rt](std::span<const tensor_view> in, tensor_view out, const ops::attrs&) {
                if (in[0].data == out.data) return;
                gpu::vk::push pc; pc.n = std::uint32_t(out.size()); pc.alpha = 1.f;
                std::vector<gpu::vk::hbuf> b{{in[0].data, in[0].size()}, {out.data, out.size()}};
                if (rt->resident()) rt->run_resident("scale", b, pc, pc.n); else rt->run("scale", b, pc, pc.n); }, -1, true});
    return r;
}
#endif
// CUDA / HIP lowerings ("cuda.<op>" / "hip.<op>"), oracle-gated. With the residency hooks, buffers stay on the
// device for a whole executor run (weights across runs), like the Vulkan backend.
// The same registry serves every runtime with the cuda.hpp contract: CUDA, HIP, OpenCL, Level Zero.
template<class RT> std::unique_ptr<registry> gpu_registry(std::shared_ptr<RT> rt, bool resident = true) {
    auto r = registry::make();
    std::string pre = rt->backend();
    resident = resident && !std::getenv("FORGE_GPU_NO_RESIDENCY");
    if (resident) {
        auto h = std::make_shared<device_hooks>();
        RT* raw = rt.get();
        h->begin_run = [raw] { raw->begin_run(); };
        h->end_run = [raw] { raw->end_run(); };
        if constexpr (requires { raw->end_run_discard(); })
            if (!std::getenv("FORGE_GPU_EAGER_SYNC")) {
                h->end_run_discard = [raw] { raw->end_run_discard(); };
                h->keep = [raw](const float* p, std::size_t n) { raw->keep(p, n); };
                if constexpr (requires { raw->arena((float*)nullptr, std::size_t(0)); raw->forget((const float*)nullptr, std::size_t(0)); }) {
                    h->arena = [rt](float* p, std::size_t n) { rt->arena(p, n); };      // owning: plans may outlive the registry
                    h->forget = [rt](const float* p, std::size_t n) { rt->forget(p, n); };
                }
                if constexpr (requires { raw->ran((const void*)nullptr, true); raw->replay((const void*)nullptr); })
                    if (std::getenv("FORGE_GPU_REPLAY")) {   // opt-in while experimental (no measured gain yet)
                        h->ran = [raw](const void* k, bool c) { raw->ran(k, c); };
                        h->replay = [raw](const void* k) { return raw->replay(k); };
                    }
            }
        h->pin = [raw](const float* p, std::size_t n) { raw->pin(p, n); };
        h->to_host = [raw](const float* p, std::size_t n) { raw->to_host(p, n); };
        h->host_wrote = [raw](const float* p, std::size_t n) { raw->host_wrote(p, n); };
        h->release = [raw](const float* p, std::size_t n) { raw->release(p, n); };
        h->copy = [raw](float* s, std::size_t n, float* d, std::size_t dn, std::size_t off) { raw->copy_resident(s, n, d, dn, off); };
        if constexpr (requires { raw->copy2d_resident((float*)nullptr, std::size_t(0), (float*)nullptr, std::size_t(0), std::size_t(0), std::size_t(0), std::size_t(0), std::size_t(0)); })
            h->copy2d = [raw](float* s, std::size_t sn, float* d, std::size_t dn, std::size_t rows, std::size_t w, std::size_t ds, std::size_t off) {
                return raw->copy2d_resident(s, sn, d, dn, rows, w, ds, off); };
        if constexpr (requires { raw->run_state(std::string(), std::vector<tensor_view>(), (float*)nullptr, std::size_t(0), tensor_view(), ops::attrs()); })
            if (!std::getenv("FORGE_GPU_NO_STATE_OPS"))
                h->state = [raw](const std::string& fn, std::span<const tensor_view> in, float* S, std::size_t sn, tensor_view out, const ops::attrs& a) {
                    return raw->run_state(fn, std::vector<tensor_view>(in.begin(), in.end()), S, sn, out, a); };
        if constexpr (requires { raw->argmax_resident((const float*)nullptr, std::size_t(0)); })
            h->argmax = [raw](const float* p, std::size_t n) { return raw->argmax_resident(p, n); };
        if constexpr (requires { raw->run_fused(std::string(), std::vector<tensor_view>(), std::vector<tensor_view>(), ops::attrs()); })
            h->fused = [raw](const std::string& k, std::span<const tensor_view> in, std::span<const tensor_view> out, const ops::attrs& a) {
                return raw->run_fused(k, std::vector<tensor_view>(in.begin(), in.end()), std::vector<tensor_view>(out.begin(), out.end()), a); };
        r->hooks = h;
    }
    for (std::size_t i = 0; i < ops::op_count; ++i) {
        auto o = ops::op(i);
        std::string name(ops::info(o).name);
        gpu::cuda_hip::sig sg; if (!gpu::cuda_hip::kernel_sig(name, sg)) continue;
        r->add({pre + "." + name, o, 20,
                [name](std::span<const shape_t> in, const shape_t&, const ops::attrs& a) {
                    std::vector<shape_t> v(in.begin(), in.end());
                    if constexpr (requires { RT::has_bcast; }) if (gpu::cuda_hip::bcast_op(name, v) >= 0) return true;
                    return gpu::cuda_hip::supports(name, v, a); },
                [rt, name](std::span<const tensor_view> in, tensor_view out, const ops::attrs& a) {
                    if constexpr (requires { RT::has_bcast; }) {
                        std::vector<shape_t> v; for (auto& t : in) v.push_back(t.shape);
                        if (int bo = gpu::cuda_hip::bcast_op(name, v); bo >= 0 && numel(out.shape) == std::max(numel(v[0]), numel(v[1]))) {
                            if (!rt->run_bcast(bo, std::vector<tensor_view>(in.begin(), in.end()), out))
                                raise(errc::unsupported, "{}: broadcast {} rejected its inputs", rt->backend(), name);
                            return; }
                    }
                    if (!rt->run_op(name, std::vector<tensor_view>(in.begin(), in.end()), a, out))
                        raise(errc::unsupported, "{}: {} rejected its inputs", rt->backend(), name); }, -1, resident});
    }
    // Quantised matmul_t with the weight kept encoded on the device (CUDA/HIP forge_qmatmul_t); FORGE_GPU_QMATMUL=0
    // falls back to the f32 path (decoded weights within FORGE_GPU_WEIGHT_BUDGET, the rest on the CPU).
    if constexpr (requires(RT& t, const tensor_view& v, tensor_view o) { t.run_qmatmul(v, nullptr, std::size_t(0), 0u, 0u, o); t.has_function(""); }) {
        auto* qe = std::getenv("FORGE_GPU_QMATMUL");
        if (!(qe && std::string(qe) == "0") && rt->has_function("forge_qmatmul_t")) {
            r->device_qgemm = lowering{pre + ".qmatmul_t", ops::op::matmul_t, 30, [](auto, auto&, auto&) { return true; },
                [rt](std::span<const tensor_view> in, tensor_view out, const ops::attrs&) {
                    const auto* c = static_cast<const codecs::codec*>(in[1].qcodec);
                    std::uint32_t fmt = c->name == "q8_0" ? 1 : c->name == "q4_k" ? 2 : c->name == "q6_k" ? 3 : c->name == "q5_k" ? 4 : 0;
                    std::size_t k = std::size_t(in[0].shape.back());
                    if (!rt->run_qmatmul(in[0], in[1].qdata, c->bytes(k), std::uint32_t(in[1].shape[0]), fmt, out))
                        raise(errc::unsupported, "{}: qmatmul_t rejected a {} weight", rt->backend(), c->name); },
                1, resident};
            r->device_qgemm_codecs = {"q8_0", "q4_k", "q5_k", "q6_k"};
            // FFN gate/up/SiLU in one step (CUDA run_qglu; $FORGE_FUSE_GLU=0 off): inputs (x, Wg, Wu, gate, up). When the
            // runtime does not take it, the two matmuls and silu_mul run as before, into the gate/up buffers.
            if constexpr (requires(RT& t, const tensor_view& v, tensor_view o) { t.run_qglu(v, nullptr, nullptr, std::size_t(0), 0u, 0u, o); }) {
                auto* fe = std::getenv("FORGE_FUSE_GLU");
                if (resident && !(fe && std::string(fe) == "0")) {
                    r->device_qglu = lowering{pre + ".qglu", ops::op::silu_mul, 30, [](auto, auto&, auto&) { return true; },
                        [rt](std::span<const tensor_view> in, tensor_view out, const ops::attrs& a) {
                            const auto* c = static_cast<const codecs::codec*>(in[1].qcodec);
                            std::uint32_t fmt = c->name == "q8_0" ? 1 : c->name == "q4_k" ? 2 : c->name == "q6_k" ? 3 : c->name == "q5_k" ? 4 : 0;
                            std::size_t rb = c->bytes(std::size_t(in[0].shape.back()));
                            std::uint32_t n = std::uint32_t(in[1].shape[0]);
                            if (rt->run_qglu(in[0], in[1].qdata, in[2].qdata, rb, n, fmt, out)) return;
                            if (!rt->run_qmatmul(in[0], in[1].qdata, rb, n, fmt, in[3]) || !rt->run_qmatmul(in[0], in[2].qdata, rb, n, fmt, in[4]) ||
                                !rt->run_op("silu_mul", {in[3], in[4]}, a, out))
                                raise(errc::unsupported, "{}: qglu rejected a {} weight", rt->backend(), c->name); },
                        1, true};
                    r->device_qglu_codecs = {"q8_0", "q4_k", "q6_k"};
                }
            }
            if constexpr (requires(RT& t, const tensor_view& v, tensor_view o) { t.run_qgather(v, nullptr, std::size_t(0), 0u, 0u, 0u, o); }) {
                auto* ge = std::getenv("FORGE_GPU_QGATHER");
                if (!(ge && std::string(ge) == "0") && rt->has_function("forge_qgather"))
                    r->device_qgather = lowering{pre + ".qgather", ops::op::gather, 30, [](auto, auto&, auto&) { return true; },
                        [rt](std::span<const tensor_view> in, tensor_view out, const ops::attrs&) {
                            const auto* c = static_cast<const codecs::codec*>(in[0].qcodec);
                            std::uint32_t fmt = c->name == "q8_0" ? 1 : c->name == "q4_k" ? 2 : c->name == "q6_k" ? 3 : c->name == "q5_k" ? 4 : 0;
                            std::size_t d = std::size_t(in[0].shape[1]);
                            if (!rt->run_qgather(in[1], in[0].qdata, c->bytes(d), std::uint32_t(in[0].shape[0]), std::uint32_t(d), fmt, out))
                                raise(errc::unsupported, "{}: qgather rejected a {} table", rt->backend(), c->name); },
                        1, resident};
            }
        }
    }
    return r;
}
// Registry for a device name ("cpu", "vulkan", "cuda[:N]", "hip[:N]", "opencl[:N]", "level_zero[:N]"); null = CPU. `why` explains a fallback.
inline std::unique_ptr<registry> device_registry(const std::string& dev, std::string* why = nullptr, bool resident = true);
// "pipe:<dev>[*w],<dev>[*w],..." e.g. "pipe:cuda:0,cuda:1" or "pipe:vulkan*3,cpu" or "pipe:rpc:host:7000,cpu".
inline std::unique_ptr<registry> pipeline_registry(const std::string& list, std::string* why) {
    auto r = registry::make();
    std::size_t i = 0;
    while (i <= list.size()) {
        // device names contain ':' but never ',' -> split on ','
        auto j = list.find(',', i); if (j == std::string::npos) j = list.size();
        auto d = list.substr(i, j - i); double w = 1;
        if (auto st = d.rfind('*'); st != std::string::npos) { w = std::atof(d.c_str() + st + 1); d = d.substr(0, st); }
        if (d.empty()) { if (why) *why = "pipe: empty stage"; return nullptr; }
        std::string e; auto sr = device_registry(d, &e, false);
        if (!sr && d != "cpu") { if (why) *why = "pipe stage '" + d + "': " + e; return nullptr; }
        r->stages.push_back(std::move(sr)); r->stage_weight.push_back(w > 0 ? w : 1);
        i = j + 1;
    }
    if (r->stages.size() < 2 && why) *why = "pipe: single stage";
    return r;
}
#ifdef FORGE_GPU_VULKAN
// Non-resident Vulkan (pipeline stages / RPC servers): every op round-trips host memory.
inline std::unique_ptr<registry> vulkan_registry_flat(std::shared_ptr<gpu::vk::runtime> rt) {
    auto r = vulkan_registry(rt); r->hooks.reset();
    for (std::size_t i = 0; i < ops::op_count; ++i) for (auto& l : r->lowerings(ops::op(i))) if (l.on_device) {
        auto fn = l.fn; l.on_device = false;
        l.fn = [rt, fn](std::span<const tensor_view> in, tensor_view out, const ops::attrs& a) {
            rt->begin_run(); try { fn(in, out, a); rt->to_host(out.data, out.size()); rt->end_run(); } catch (...) { rt->end_run(); throw; } };
    }
    return r;
}
#endif
inline std::unique_ptr<registry> device_registry(const std::string& dev, std::string* why, bool resident) {
    if (dev.empty() || dev == "cpu") return nullptr;
    if (dev.rfind("pipe:", 0) == 0) return pipeline_registry(dev.substr(5), why);
    if (dev.rfind("rpc:", 0) == 0) return rpc::open(dev.substr(4), why);
#ifdef FORGE_GPU_VULKAN
    if (dev == "vulkan") {
        std::string e;
        auto rt = gpu::vk::runtime::open(&e);
        if (rt) return resident ? vulkan_registry(std::shared_ptr<gpu::vk::runtime>(std::move(rt))) : vulkan_registry_flat(std::shared_ptr<gpu::vk::runtime>(std::move(rt)));
        if (why) *why = "vulkan unavailable: " + e;
        return nullptr;
    }
#endif
    auto base = dev.substr(0, dev.find(':'));   // "cuda:1" selects device ordinal 1
    if (base == "cuda" || base == "hip") {
        std::string e; int ord = dev.size() > base.size() ? std::atoi(dev.c_str() + base.size() + 1) : 0;
        auto rt = gpu::cuda_hip::runtime::open(base == "cuda" ? gpu::cuda_hip::api::cuda : gpu::cuda_hip::api::hip, &e, ord);
        if (rt) return gpu_registry(std::shared_ptr<gpu::cuda_hip::runtime>(std::move(rt)), resident);
        if (why) *why = e;
        return nullptr;
    }
    if (base == "opencl") {   // "opencl:N" = Nth device over all platforms (GPUs first)
        std::string e; int ord = dev.size() > base.size() ? std::atoi(dev.c_str() + base.size() + 1) : 0;
        auto rt = gpu::ocl::runtime::open(&e, ord);
        if (rt) return gpu_registry(std::shared_ptr<gpu::ocl::runtime>(std::move(rt)), resident);
        if (why) *why = e;
        return nullptr;
    }
    if (base == "metal") {
        std::string e; int ord = dev.size() > base.size() ? std::atoi(dev.c_str() + base.size() + 1) : 0;
        auto rt = gpu::metal::runtime::open(&e, ord);
        if (rt) return gpu_registry(std::shared_ptr<gpu::metal::runtime>(std::move(rt)), resident);
        if (why) *why = e;
        return nullptr;
    }
    if (base == "level_zero" || base == "ze") {
        std::string e; int ord = dev.size() > base.size() ? std::atoi(dev.c_str() + base.size() + 1) : 0;
        auto rt = gpu::ze::runtime::open(&e, ord);
        if (rt) return gpu_registry(std::shared_ptr<gpu::ze::runtime>(std::move(rt)), resident);
        if (why) *why = e;
        return nullptr;
    }
    if (why) *why = "device '" + dev + "' not available in this build";
    return nullptr;
}
} // namespace forge::graph
