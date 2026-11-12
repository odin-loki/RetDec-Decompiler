// (1) every generated GPU kernel vs the spec reference on the Vulkan backend;
// (2) a whole tiny Llama run end-to-end with FORGE's Vulkan lowerings vs the CPU path.
// Exit 77 (ctest SKIP) when no Vulkan device is available.
#include <forge/gpu/vulkan.hpp>
#include <cstdio>
#ifndef FORGE_GPU_VULKAN
int main() { std::puts("vulkan backend not built (no vulkan headers / glslangValidator)"); return 77; }
#else
#include "tiny_model.hpp"
#include <forge/inference/inference.hpp>
#include <cmath>
#include <filesystem>
#if !defined(_WIN32)
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif
#include <random>
using namespace forge;

static int kernels(gpu::vk::runtime& rt) {
    std::mt19937 g(7);
    auto rnd = [&](shape_t s, float lo, float hi) { tensor t(s); std::uniform_real_distribution<float> d(lo, hi); for (auto& v : t.data) v = d(g); return t; };
    int fails = 0, n = 0;
    for (auto& b : gpu::spv::table) {
        std::string name = b.name;
        auto o = ops::op_from_name(name);
        if (!o) { std::printf("FAIL %s not in spec\n", b.name); ++fails; continue; }
        auto& info = ops::info(*o);
        bool positive = name == "sqrt" || name == "rsqrt" || name == "log" || name == "log2" || name == "recip";
        std::vector<tensor> in;
        ops::attrs a;
        if (name == "matmul_t") in = {rnd({3, 5, 17}, -1, 1), rnd({9, 17}, -1, 1)};
        else if (name == "rms_norm") { in = {rnd({7, 33}, -2, 2), rnd({33}, -1, 1)}; a = {{"eps", 1e-5}}; }
        else if (name == "rope") { in = {rnd({6, 3, 16}, -1, 1), tensor({6})}; for (int i = 0; i < 6; ++i) in[1].data[std::size_t(i)] = float(i * 7); a = {{"base", 10000.0}, {"neox", 1.0}}; }
        else if (name == "gather") { in = {rnd({11, 8}, -1, 1), tensor({5})}; for (int i = 0; i < 5; ++i) in[1].data[std::size_t(i)] = float((i * 4) % 11); }
        else if (name == "slice") { in = {rnd({3, 10, 4}, -1, 1)}; a = {{"axis", 1.0}, {"start", 2.0}, {"end", 7.0}}; }
        else if (name == "attention") { in = {rnd({4, 4, 8}, -1, 1), rnd({9, 2, 8}, -1, 1), rnd({9, 2, 6}, -1, 1)}; a = {{"causal", 1.0}}; }
        else { in.push_back(rnd({4, 129}, positive ? 0.1f : -4.f, 4.f)); if (info.inputs == 2) in.push_back(rnd({4, 129}, 0.5f, 3.f)); }
        if (name == "scale") a = {{"alpha", 1.75}};
        tensor ref = ops::reference(*o)(in, a), got;
        if (!gpu::vk::run_op(rt, name, in, a, got)) { std::printf("FAIL %s: no lowering\n", b.name); ++fails; continue; }
        // GLSL float precision: transcendental builtins are not correctly rounded, so use a relaxed bound.
        double worst = 0;
        for (std::size_t i = 0; i < ref.data.size(); ++i) {
            double e = std::fabs(double(got.data[i]) - ref.data[i]) / std::max(1.0, std::fabs(double(ref.data[i])));
            if (!(e <= worst)) worst = std::isnan(e) ? 1e30 : e;
        }
        bool ok = got.shape == ref.shape && worst <= std::max(2e-5, gpu::vk::transcendental_tol(name));
        std::printf("%s %-10s max err %.2e\n", ok ? "ok  " : "FAIL", b.name, worst);
        fails += !ok; ++n;
    }
    std::printf("%d kernels, %d failures\n", n, fails);
    return fails;
}

static int end_to_end() {
    auto path = (std::filesystem::temp_directory_path() / ("forge_test_vk_e2e_" + std::to_string(::getpid()) + ".fpk")).string();
    forge_test::make_tiny_model(path);
    auto cpu = inference::model::open(path), gpu = inference::model::open(path);
    cpu->set_device("cpu");
    if (gpu->set_device("vulkan") != "vulkan") { std::puts("FAIL e2e: vulkan device not selected"); return 1; }
    {   // every vk lowering must pass the same oracle gate the registry applies
        auto reg = graph::vulkan_registry(graph::last_vulkan_runtime());
        for (std::size_t i = 0; i < ops::op_count; ++i) for (auto& l : reg->lowerings(ops::op(i))) {
            if (l.name.rfind("vk.", 0) != 0) continue;
            std::string msg;
            if (!graph::registry::oracle_check(l, &msg)) { std::printf("FAIL oracle %s: %s\n", l.name.c_str(), msg.c_str()); return 1; }
        }
    }
    std::vector<int> prompt = {1, 5, 9, 33, 70, 101, 7};
    inference::session a(cpu), b(gpu);
    auto la = a.forward(prompt), lb = b.forward(prompt);
    float worst = 0;
    auto diff = [&](const std::vector<float>& x, const std::vector<float>& y) { for (std::size_t i = 0; i < x.size(); ++i) worst = std::max(worst, std::fabs(x[i] - y[i])); };
    diff(la, lb);
    inference::sampler g1, g2;
    a.reset(); b.reset();
    auto ta = a.generate(prompt, 12, g1), tb = b.generate(prompt, 12, g2);
    std::size_t vk_ops = 0;
    for (auto& [k, v] : graph::last_vulkan_runtime()->dispatches()) vk_ops += v;
    // Which lowering every step of the decode plan uses: all compute should be on the device.
    auto& plan = gpu->plan_for(1, false, const_cast<graph::state_store*>(&b.state()));
    std::printf("%s", plan.describe().c_str());
    int cpu_compute = 0;
    for (auto& st : plan.steps) if (st.low && st.low->name.rfind("vk.", 0) != 0) {
        ++cpu_compute; std::printf("  on cpu: %s (%s)\n", std::string(ops::info(st.low->op).name).c_str(), st.low->name.c_str()); }
    // Residency: weights upload once, and a decode step is one submit, not one per op.
    auto rs = graph::last_vulkan_runtime()->residency();
    std::printf("  residency: %zu uploads (%.1f KiB), %zu downloads, %zu submits for %zu dispatches, %zu pinned buffers\n",
                rs.uploads, double(rs.upload_bytes) / 1024, rs.downloads, rs.submits, vk_ops, rs.resident);
    bool batched = rs.submits * 4 < vk_ops;
    if (!batched) std::puts("FAIL residency: dispatches are not batched");
    bool ok = la.size() == lb.size() && worst < 1e-3f && ta == tb && vk_ops > 0 && cpu_compute == 0 && batched;
    std::printf("%s e2e tiny llama: prefill logits max|err| %.2e, 12 greedy tokens %s, %zu vulkan dispatches, %d CPU compute steps\n",
                ok ? "ok  " : "FAIL", double(worst), ta == tb ? "identical" : "DIFFER", vk_ops, cpu_compute);
    std::error_code ec; std::filesystem::remove(path, ec);
    return ok ? 0 : 1;
}

static int pipeline_cache() {
    // A second runtime must pick up the cache file written by the first one.
    std::string path;
    { auto a = gpu::vk::runtime::open(); if (!a) return 1;
      path = a->pipeline_cache_path();
      std::vector<float> x(64, 1.f), y(64, 2.f), o(64);
      if (gpu::vk::runtime::has_kernel("add")) a->run("add", {{x.data(), 64}, {y.data(), 64}, {o.data(), 64}}, {}, 64); }
    if (path.empty() || !std::filesystem::exists(path)) { std::printf("FAIL pipeline cache not written (%s)\n", path.c_str()); return 1; }
    auto b = gpu::vk::runtime::open(); if (!b) return 1;
    if (b->pipeline_cache_path() != path || b->pipeline_cache_loaded_bytes() == 0) { std::printf("FAIL pipeline cache not reloaded\n"); return 1; }
    std::printf("pipeline cache: %s (%zu bytes reloaded)\n", path.c_str(), b->pipeline_cache_loaded_bytes());
    return 0;
}

// Device memory pool "vulkan:0" (gpu/device_memory.hpp): allocations are charged, and a resident weight not used by
// the op in progress is evicted (least recently used first) when the next allocation would cross the limit.
static int device_memory() {
    auto rt = gpu::vk::runtime::open(); if (!rt) return 1;
    auto* pool = rt->memory_pool();
    if (!pool || pool->name() != "vulkan:0") { std::puts("FAIL vulkan runtime charges no pool vulkan:0"); return 1; }
    pool->set_limit(0);
    pool->evict(0);   // runtimes from the earlier checks share the pool: empty their resident buffers first
    const std::size_t N = 1000; auto base = pool->used();
    std::vector<float> w0(N), w1(N), w2(N), y(N), z(N);
    for (std::size_t i = 0; i < N; ++i) { w0[i] = float(i % 13) - 6; w1[i] = float(i % 7); w2[i] = float(i % 5) - 2; }
    for (auto* w : {&w0, &w1, &w2}) rt->pin(w->data(), N);
    rt->begin_run(); rt->run_resident("add", {{w0.data(), N}, {w1.data(), N}, {y.data(), N}}, {.n = std::uint32_t(N)}, std::uint32_t(N)); rt->end_run();
    auto two = pool->used() - base, one = two / 2;   // the two pinned weights stay charged
    bool ok = two >= 2 * N * 4 && rt->charged_bytes() == two;
    for (std::size_t i = 0; i < N; ++i) ok &= y[i] == w0[i] + w1[i];
    pool->set_limit(base + 3 * one);   // mul(w2, w0) needs a 4th buffer: w1 must go
    rt->begin_run(); rt->run_resident("mul", {{w2.data(), N}, {w0.data(), N}, {z.data(), N}}, {.n = std::uint32_t(N)}, std::uint32_t(N)); rt->end_run();
    for (std::size_t i = 0; i < N; ++i) ok &= z[i] == w2[i] * w0[i];
    std::size_t ev = rt->residency().evictions;
    if (!ok) std::puts("FAIL vulkan: wrong results or charges before the eviction step");
    ok &= ev >= 1 && pool->used() <= base + 3 * one;
    pool->set_limit(0); rt.reset();
    ok &= pool->used() == base;
    std::printf("%s vulkan device memory: %llu bytes per weight, eviction under a 3-buffer budget (%zu evictions, %llu bytes left charged)\n", ok ? "ok  " : "FAIL",
                static_cast<unsigned long long>(one), ev, static_cast<unsigned long long>(pool->used() - base));
    return ok ? 0 : 1;
}

int main() {
    std::string why;
    auto tmp = std::filesystem::temp_directory_path() / ("forge-vkcache-" + std::to_string(::getpid()));
    
#if defined(_WIN32)
    _putenv_s("FORGE_CACHE_DIR", tmp.c_str());
#else
    ::setenv("FORGE_CACHE_DIR", tmp.c_str(), 1);
#endif
    auto rt = gpu::vk::runtime::open(&why);
    if (!rt) { std::printf("SKIP: %s\n", why.c_str()); return 77; }
    std::printf("vulkan device: %s\n", rt->device_name().c_str());
    int f = kernels(*rt);
    f += end_to_end();
    rt.reset();
    f += pipeline_cache();
    f += device_memory();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    return f ? 1 : 0;
}
#endif
