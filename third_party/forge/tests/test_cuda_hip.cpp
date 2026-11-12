// CUDA / HIP runtime. `test_cuda_hip images` checks the embedded kernel images (runs everywhere);
// `test_cuda_hip cuda|hip` runs every kernel on the device against the reference (exit 77 = skip, no device).
#include <forge/graph/graph.hpp>
#include <forge/gpu/cuda.hpp>
#include <forge/graph/vulkan.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <string>

using namespace forge;
namespace cuda_hip = forge::gpu::cuda_hip;

static const char* kernel_ops[] = {"neg", "abs", "sqrt", "rsqrt", "recip", "square", "exp", "exp2", "log", "log2", "tanh", "sigmoid",
    "silu", "relu", "softplus", "gelu_tanh", "add", "sub", "mul", "div", "maximum", "minimum", "silu_mul", "scale", "softmax",
    "rms_norm", "matmul_t", "rope", "gather", "slice", "attention", "qmatmul_t"};

static bool has(std::string_view hay, const std::string& needle) { return hay.find(needle) != std::string_view::npos; }

static int images() {
    int fails = 0;
    auto ptx = cuda_hip::module_images(cuda_hip::api::cuda);
    auto hip = cuda_hip::module_images(cuda_hip::api::hip);
    if (ptx.empty()) { std::puts("SKIP images: built without a GPU-capable clang"); return 77; }
    for (auto& [arch, img] : ptx) {
        const bool sm80 = arch == "ptx sm_80";   // the sm_80 image adds the int8 tensor-core prefill matmul
        bool ok = has(img, ".version") && has(img, sm80 ? ".target sm_80" : ".target sm_70");
        for (auto op : kernel_ops) if (!has(img, std::string(".entry forge_") + op + "(")) { std::printf("FAIL %s: no entry forge_%s\n", arch.c_str(), op); ok = false; }
        if (sm80) for (auto op : {"qmatmul_mma_q8_0", "qmatmul_mma_q4_k", "qmatmul_mma_q6_k", "quant_q8r", "qmatmul_glu_q4_k", "qmatmul_glu128_q6_k", "glu_q81"}) if (!has(img, std::string(".entry forge_") + op + "(")) { std::printf("FAIL %s: no entry forge_%s\n", arch.c_str(), op); ok = false; }
        std::printf("%s %s: %zu bytes, %zu kernels\n", ok ? "ok  " : "FAIL", arch.c_str(), img.size(), std::size(kernel_ops)); fails += !ok;
    }
    for (auto& [arch, img] : hip) {
        bool elf = img.size() > 64 && std::memcmp(img.data(), "\x7f" "ELF", 4) == 0;
        std::uint16_t machine = 0; if (elf) std::memcpy(&machine, img.data() + 18, 2);
        bool ok = elf && machine == 224;   // EM_AMDGPU
        for (auto op : kernel_ops) if (!has(img, std::string("forge_") + op + ".kd")) { std::printf("FAIL %s: no kernel descriptor forge_%s\n", arch.c_str(), op); ok = false; }
        std::printf("%s hsaco %s: %zu bytes, ELF machine %u\n", ok ? "ok  " : "FAIL", arch.c_str(), img.size(), unsigned(machine)); fails += !ok;
    }
    if (hip.empty()) std::puts("note: no HIP code objects (no lld at build time)");
    for (auto a : {cuda_hip::api::cuda, cuda_hip::api::hip}) {   // without a driver this must fail cleanly, not crash
        std::string why; auto rt = cuda_hip::runtime::open(a, &why);
        std::printf("     %s runtime: %s\n", cuda_hip::api_name(a), rt ? ("device " + rt->device_name()).c_str() : why.c_str());
    }
    std::string why; auto reg = graph::device_registry("cuda", &why);
    if (!reg && why.empty()) { std::puts("FAIL device_registry(cuda) gave no reason for the fallback"); ++fails; }
    return fails ? 1 : 0;
}

// Quantised matmul_t kept encoded on the device vs the CPU quantised kernel (graph::qweights::gemm_nt).
// mma_runs: how many prefill matmuls ran on the int8 tensor-core kernel so far (it quantises x like the MMVQ path).
template<class RT> static int qmatmul_check(graph::registry& reg, const std::string& api, const std::function<std::size_t()>& mma_runs) {
    if (!reg.device_qgemm) { std::printf("note %s: no device quantised matmul\n", api.c_str()); return 0; }
    int fails = 0, n = 0;
    for (std::string cn : {"q8_0", "q4_k", "q5_k", "q6_k"}) for (std::size_t rows : {1, 3, 6, 16, 70, 130}) {   // >= 16 rows: the tiled MMQ kernel
        const auto* c = graph::codecs::find(cn);
        const std::size_t k = 512, cols = 37, rb = c->bytes(k);
        std::vector<float> wf(cols * k), x(rows * k);
        for (std::size_t i = 0; i < wf.size(); ++i) wf[i] = std::sin(float(i) * 0.731f + float(cn.size())) * (1.f + float(i % 97) / 50.f);
        for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::cos(float(i) * 0.217f);
        std::vector<std::uint8_t> enc(cols * rb);
        for (std::size_t j = 0; j < cols; ++j) c->encode(wf.data() + j * k, k, enc.data() + j * rb);
        tensor_view xv; xv.data = x.data(); xv.shape = {i64(rows), i64(k)};
        tensor_view wv; wv.shape = {i64(cols), i64(k)}; wv.qdata = enc.data(); wv.qcodec = c;
        std::vector<float> yg(rows * cols, -1.f), yc(rows * cols);
        tensor_view og; og.data = yg.data(); og.shape = {i64(rows), i64(cols)};
        tensor_view oc; oc.data = yc.data(); oc.shape = og.shape;
        std::vector<tensor_view> ins{xv, wv};
        const std::size_t mma0 = mma_runs();
        reg.device_qgemm->fn(ins, og, {});
        const bool mma = mma_runs() != mma0;
        // rows < 16 on q8_0/q4_k/q6_k: the MMVQ path quantises x to q8_1 (32-value blocks, d = amax/127) like llama.cpp,
        // so the reference uses the same rounded activations.
        const bool mmvq = mma || (rows < 16 && (cn == "q8_0" || cn == "q4_k" || cn == "q6_k") && !(std::getenv("FORGE_MMVQ") && std::string(std::getenv("FORGE_MMVQ")) == "0"));
        std::vector<float> xr(x);
        if (mmvq) for (std::size_t b = 0; b < xr.size(); b += 32) {
            float amax = 0; for (int i = 0; i < 32; ++i) amax = std::max(amax, std::fabs(xr[b + i]));
            const float d = amax / 127.f, id = d > 0 ? 1.f / d : 0.f;
            for (int i = 0; i < 32; ++i) xr[b + i] = d * float(int(std::nearbyint(xr[b + i] * id)));
        }
        std::vector<tensor_view> insr{xv, wv}; insr[0].data = xr.data();
        graph::qweights::gemm_nt(insr, oc, {});
        double worst = 0;
        for (std::size_t i = 0; i < yc.size(); ++i) worst = std::max(worst, std::fabs(double(yg[i]) - yc[i]) / (1.0 + std::fabs(double(yc[i]))));
        ++n; if (worst > (mmvq ? 2e-3 : 1e-4)) { std::printf("FAIL %s.qmatmul_t %s rows %zu: worst rel error %.3g\n", api.c_str(), cn.c_str(), rows, worst); ++fails; }
    }
    std::printf("%s %d/%d %s quantised matmul_t cases match the CPU kernel\n", fails ? "FAIL" : "ok  ", n - fails, n, api.c_str());
    return fails;
}

static void set_env(const char* k, const std::string& v);
// Persistent stream-K tensor-core matmul (FORGE_MMA_STREAMK) vs the one-tile-per-block launch on ragged shapes: tiles
// split mid-k across blocks (fixup slots and flags), data-parallel waves before the stream-K share (small
// FORGE_MMA_SK_MIN shrinks the grid), 64- and 128-token tiles; then the same shapes against the CPU kernel.
template<class RT> static int streamk_check(graph::registry& reg, RT& rt, const std::string& api) {
    if (!reg.device_qgemm) return 0;
    auto disp = [&](const char* key) -> std::size_t { auto d = rt.dispatches(); auto it = d.find(key); return it == d.end() ? 0 : it->second; };
    int fails = 0, n = 0, took = 0;
    struct cs { const char* cn; std::size_t rows, cols, k; const char* skmin; const char* bt; };
    for (auto [cn, rows, cols, k, skmin, bt] : {cs{"q4_k", 130, 300, 2560, "4", "64"}, cs{"q4_k", 70, 1100, 1024, "8", "64"}, cs{"q6_k", 200, 520, 1536, "1", "128"},
                                                 cs{"q8_0", 130, 1100, 1024, "8", "128"}, cs{"q6_k", 64, 1100, 768, "3", "64"}, cs{"q8_0", 17, 260, 2048, "2", "64"},
                                                 cs{"q4_k", 16, 256, 512, "1", "64"}, cs{"q4_k", 130, 256, 512, "1", "64"}, cs{"q4_k", 130, 40, 256, "1", "64"}}) {
        const auto* c = graph::codecs::find(cn);
        const std::size_t rb = c->bytes(k);
        std::vector<float> wf(cols * k), x(rows * k);
        for (std::size_t i = 0; i < wf.size(); ++i) wf[i] = std::sin(float(i) * 0.731f + float(rows)) * (1.f + float(i % 97) / 50.f);
        for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::cos(float(i) * 0.217f);
        // kept alive: device weights are cached by host address, so a freed buffer's address must not come back with other contents
        static std::deque<std::vector<std::uint8_t>> keep;
        auto& enc = keep.emplace_back(cols * rb);
        for (std::size_t j = 0; j < cols; ++j) c->encode(wf.data() + j * k, k, enc.data() + j * rb);
        tensor_view xv; xv.data = x.data(); xv.shape = {i64(rows), i64(k)};
        tensor_view wv; wv.shape = {i64(cols), i64(k)}; wv.qdata = enc.data(); wv.qcodec = c;
        std::vector<float> ys(rows * cols, -1.f), yp(rows * cols, -2.f), yc(rows * cols);
        auto run = [&](std::vector<float>& y) { tensor_view o; o.data = y.data(); o.shape = {i64(rows), i64(cols)}; std::vector<tensor_view> ins{xv, wv}; reg.device_qgemm->fn(ins, o, {}); };
        set_env("FORGE_MMA_SK_MIN", skmin); set_env("FORGE_MMA_BT", bt);
        const std::size_t s0 = disp("qmatmul_mma_streamk");
        set_env("FORGE_MMA_STREAMK", "2");   // every ragged grid, however small
        run(ys); run(ys);   // twice: the flags must be back to zero after a launch
        took += disp("qmatmul_mma_streamk") > s0;
        set_env("FORGE_MMA_STREAMK", "0"); run(yp); set_env("FORGE_MMA_STREAMK", "");
        set_env("FORGE_MMA_SK_MIN", ""); set_env("FORGE_MMA_BT", "");
        std::vector<float> xr(x);
        for (std::size_t b = 0; b < xr.size(); b += 32) {
            float amax = 0; for (int i = 0; i < 32; ++i) amax = std::max(amax, std::fabs(xr[b + i]));
            const float d = amax / 127.f, id = d > 0 ? 1.f / d : 0.f;
            for (int i = 0; i < 32; ++i) xr[b + i] = d * float(int(std::nearbyint(xr[b + i] * id)));
        }
        std::vector<tensor_view> insr{xv, wv}; insr[0].data = xr.data();
        tensor_view oc; oc.data = yc.data(); oc.shape = {i64(rows), i64(cols)};
        graph::qweights::gemm_nt(insr, oc, {});
        double wp = 0, wc = 0;
        for (std::size_t i = 0; i < yc.size(); ++i) { wp = std::max(wp, std::fabs(double(ys[i]) - yp[i]) / (1.0 + std::fabs(double(yp[i]))));
                                                     wc = std::max(wc, std::fabs(double(ys[i]) - yc[i]) / (1.0 + std::fabs(double(yc[i])))); }
        // vs the CPU only as a sanity bound: its q4_k / q6_k kernels quantise x again (q8_K), which drifts with long k
        ++n; if (wp > 1e-4 || wc > 5e-2) { std::printf("FAIL %s stream-K %s %zux%zux%zu: rel vs one-tile-per-block %.3g, vs CPU %.3g\n", api.c_str(), cn, rows, cols, k, wp, wc); ++fails; }
    }
    std::printf("%s %d/%d %s stream-K matmul cases match the one-tile-per-block launch and the CPU (%d took stream-K)\n", fails ? "FAIL" : "ok  ", n - fails, n, api.c_str(), took);
    return fails;
}

static int state_ops_check_one(graph::registry& reg, const std::string& api, const i64 dk, const i64 dv);
// Gated DeltaNet state ops on the device (run_state) vs the CPU executor: 3-token prefill, then token by token,
// then reset() and again (the resident state must restart from zeros).
static int state_ops_check(graph::registry& reg, const std::string& api) {
    if (!reg.hooks || !reg.hooks->state) { std::printf("note %s: no device state ops\n", api.c_str()); return 0; }
    int fails = 0;
    for (auto [dk, dv] : {std::pair<i64, i64>{40, 136}, {128, 128}, {64, 32}}) fails += state_ops_check_one(reg, api, dk, dv);
    return fails;
}
// One shape: dk 40 / dv 136 takes the per-head kernel (dv > 128 and C > 128: multi-column threads / blocks); dk = dv = 128
// (Qwen3.5) and dk 64 / dv 32 take the register-state kernels forge_delta_rule_p16 / _p8 (_r16 / _r8 with FORGE_DELTA_RULE_R).
static int state_ops_check_one(graph::registry& reg, const std::string& api, const i64 dk, const i64 dv) {
    using namespace graph;
    const i64 S = 7, Hk = 2, Hv = 4, C = 150, K = 4;
    auto rnd = [](std::size_t n, float ph) { std::vector<float> v(n); for (std::size_t i = 0; i < n; ++i) v[i] = std::sin(float(i) * 0.37f + ph); return v; };
    auto q = rnd(S * Hk * dk, 1), k = rnd(S * Hk * dk, 2), v = rnd(S * Hv * dv, 3), gl = rnd(S * Hv, 4), bl = rnd(S * Hv, 5), xc = rnd(S * C, 6), wc = rnd(C * K, 7);
    for (auto& x : gl) x = -std::fabs(x) * 0.5f;
    for (auto& x : bl) x = 1.f / (1.f + std::exp(-x));
    auto build = [&](i64 s) {
        tensor_graph g;
        auto iq = g.input("q", {s, Hk, dk}), ik = g.input("k", {s, Hk, dk}), iv = g.input("v", {s, Hv, dv});
        auto ig = g.input("g", {s, Hv}), ib = g.input("b", {s, Hv}), ix = g.input("x", {s, C});
        auto w = g.constant(tensor({C, K}, 0.f)); g.at(w).constant->data = wc;
        g.output("o", g.state_op("ssm", "delta_rule", {iq, ik, iv, ig, ib}));
        g.output("c", g.state_op("conv", "causal_conv", {ix, w}));
        return g;
    };
    auto run = [&](registry* r) {
        std::vector<float> out;
        state_store st; pass_options po; po.lowerings = r;
        auto p3 = compile(build(3), &st, po), p1 = compile(build(1), &st, po);
        executor e3(*p3, &st), e1(*p1, &st);
        auto feed = [&](executor& e, i64 t0, i64 s) {
            auto sl = [&](const std::vector<float>& a, i64 row) { return std::span<const float>(a.data() + t0 * row, std::size_t(s * row)); };
            e.set_input("q", sl(q, Hk * dk)); e.set_input("k", sl(k, Hk * dk)); e.set_input("v", sl(v, Hv * dv));
            e.set_input("g", sl(gl, Hv)); e.set_input("b", sl(bl, Hv)); e.set_input("x", sl(xc, C)); e.run();
            for (auto nm : {"o", "c"}) { auto t = e.output(nm); out.insert(out.end(), t.data.begin(), t.data.end()); }
        };
        for (int pass = 0; pass < 2; ++pass) { feed(e3, 0, 3); for (i64 t = 3; t < S; ++t) feed(e1, t, 1); st.reset(); }
        return out;
    };
    auto dev = run(&reg), cpu = run(nullptr);
    double worst = dev.size() == cpu.size() ? 0 : 1e9;
    for (std::size_t i = 0; i < std::min(dev.size(), cpu.size()); ++i) worst = std::max(worst, std::fabs(double(dev[i]) - cpu[i]) / (1.0 + std::fabs(double(cpu[i]))));
    bool ok = worst < 1e-4;
    std::printf("%s %s delta_rule/causal_conv (dk %lld, dv %lld) on the device match the CPU (worst rel %.3g)\n", ok ? "ok  " : "FAIL", api.c_str(),
                (long long)dk, (long long)dv, worst);
    return ok ? 0 : 1;
}

// attention against the reference: the split-K decode kernels' shapes (forge_attention_dec + _comb: head dims a
// multiple of 32 up to 256, rows x heads <= 256) and prefill shapes for the tiled kernel; GQA, causal and not.
template<class RT> static int attention_dec_check(RT& rt, const std::string& api) {
    int fails = 0, n = 0;
    struct cfg { i64 s, t, hq, hk, d; bool causal; i64 window = 0; };
    for (auto c : {cfg{1, 1, 4, 2, 32, true}, cfg{1, 37, 4, 2, 32, true}, cfg{6, 6, 4, 2, 32, true}, cfg{5, 12, 8, 2, 64, true},
                   cfg{1, 300, 16, 4, 128, true}, cfg{3, 9, 4, 4, 32, false}, cfg{16, 16, 8, 8, 32, true},
                   // prefill shapes (rows x heads > 256: the tiled forge_attention_fa2_* kernels on the sm_80 image)
                   cfg{100, 100, 8, 2, 128, true}, cfg{70, 90, 4, 4, 256, true}, cfg{40, 40, 8, 4, 64, false}, cfg{130, 130, 4, 1, 96, true},
                   cfg{50, 120, 8, 2, 128, true}, cfg{150, 150, 4, 2, 128, true, 40}, cfg{90, 90, 4, 2, 256, true, 33}}) {
        auto rnd = [](std::size_t m, float ph) { std::vector<float> v(m); for (std::size_t i = 0; i < m; ++i) v[i] = std::sin(float(i) * 0.613f + ph); return v; };
        tensor q({c.s, c.hq, c.d}), k({c.t, c.hk, c.d}), v({c.t, c.hk, c.d}), y({c.s, c.hq, c.d});
        q.data = rnd(q.data.size(), 1); k.data = rnd(k.data.size(), 2); v.data = rnd(v.data.size(), 3);
        ops::attrs a{{"causal", c.causal ? 1.0 : 0.0}, {"window", double(c.window)}};
        ++n;
        auto tc_runs = [&]() -> std::size_t { auto dd = rt.dispatches(); auto it = dd.find("attention_fa2tc"); return it == dd.end() ? 0 : it->second; };
        const std::size_t tc0 = tc_runs();
        if (!rt.run_op("attention", {{q.data.data(), q.shape}, {k.data.data(), k.shape}, {v.data.data(), v.shape}}, a, {y.data.data(), y.shape})) {
            std::printf("FAIL %s.attention s %lld t %lld d %lld: run_op refused the shape\n", api.c_str(), (long long)c.s, (long long)c.t, (long long)c.d); ++fails; continue;
        }
        auto want = ops::ref::attention({q, k, v}, a);
        double worst = 0; for (std::size_t i = 0; i < y.data.size(); ++i) worst = std::max(worst, std::fabs(double(y.data[i]) - want.data[i]));
        // fp16 tensor-core kernel (forge_attention_fa2tc_*): Q, K, V and P are rounded to fp16 (relative 2^-11) before
        // the f32-accumulated products; outputs here carry ~4e-4 absolute error (bound 2e-3).
        const bool tc = tc_runs() > tc0; const double tol = tc ? 2e-3 : 1e-4;
        if (!(worst < tol)) { std::printf("FAIL %s.attention s %lld t %lld hq %lld hk %lld d %lld: worst abs error %.3g\n", api.c_str(),
                                                (long long)c.s, (long long)c.t, (long long)c.hq, (long long)c.hk, (long long)c.d, worst); ++fails; }
        else if (tc) std::printf("ok   %s.attention s %lld t %lld d %lld on fp16 tensor cores: worst abs error %.3g\n", api.c_str(),
                                 (long long)c.s, (long long)c.t, (long long)c.d, worst);
    }
    std::printf("%s %d/%d %s attention shapes match the reference\n", fails ? "FAIL" : "ok  ", n - fails, n, api.c_str());
    return fails;
}

// Elementwise fusions (cuda.hpp run_fused, the rms_norm q8r output): bit-identical to the unfused kernels inside a run,
// and through the executor (plan.hpp fuse_steps) within the oracle tolerance of the CPU.
template<class RT> static int fusion_check(RT& rt, const std::string& api) {
    if constexpr (!requires { rt.run_fused(std::string(), std::vector<tensor_view>(), std::vector<tensor_view>(), ops::attrs()); }) return 0;
    else {
    auto off = [](const char* v) { auto* e = std::getenv(v); return e && std::string(e) == "0"; };
    if (off("FORGE_FUSE_ADD_NORM") || off("FORGE_FUSE_QK_ROPE")) { std::printf("note %s: elementwise fusions switched off\n", api.c_str()); return 0; }
    int fails = 0, n = 0;
    auto rnd = [](std::size_t m, float ph) { std::vector<float> v(m); for (std::size_t i = 0; i < m; ++i) v[i] = std::sin(float(i) * 0.613f + ph) * (1.f + float(i % 7) / 3.f); return v; };
    auto disp = [&](const char* k) -> std::size_t { auto d = rt.dispatches(); auto it = d.find(k); return it == d.end() ? 0 : it->second; };
    // add + rms_norm: decode rows (q8_1 reduction; 1024 threads for one wide row), prefill rows (tree; q8r output when
    // cols % 256 == 0), in place (s = a) and out of place
    struct an { i64 rows, cols; bool inplace; };
    for (auto c : {an{1, 4096, true}, an{3, 256, false}, an{2, 96, false}, an{40, 512, true}, an{20, 160, false}, an{70, 1024, false}, an{33, 2048, true}}) {
        const std::size_t N = std::size_t(c.rows * c.cols);
        auto a = rnd(N, 1), b = rnd(N, 2), w = rnd(std::size_t(c.cols), 3);
        std::vector<float> a2 = a, s1(N), y1(N), s2(N), y2(N);
        shape_t sh{c.rows, c.cols}, ws{c.cols};
        ops::attrs at{{"eps", 1e-5}};
        rt.begin_run();   // unfused: add, rms_norm
        float* sp = c.inplace ? a.data() : s1.data();
        rt.run_op("add", {{a.data(), sh}, {b.data(), sh}}, {}, {sp, sh});
        rt.run_op("rms_norm", {{sp, sh}, {w.data(), ws}}, at, {y1.data(), sh});
        rt.to_host(sp, N); rt.to_host(y1.data(), N); rt.end_run();
        if (c.inplace) s1 = a;
        rt.begin_run();
        const std::size_t f0 = disp("fused_add_rms_norm");
        float* sp2 = c.inplace ? a2.data() : s2.data();
        bool ok = rt.run_fused("add_rms_norm", {{a2.data(), sh}, {b.data(), sh}, {w.data(), ws}}, {{sp2, sh}, {y2.data(), sh}}, at);
        rt.to_host(sp2, N); rt.to_host(y2.data(), N); rt.end_run();
        if (c.inplace) s2 = a2;
        ++n;
        if (!ok || disp("fused_add_rms_norm") != f0 + 1 || s1 != s2 || y1 != y2) {
            std::printf("FAIL %s add_rms_norm rows %lld cols %lld: %s\n", api.c_str(), (long long)c.rows, (long long)c.cols, ok ? "differs from add + rms_norm" : "refused"); ++fails; }
    }
    // rms_norm + rope (QK-norm): decode (64 heads: tree; 8 heads: q8_1 reduction), prefill, neox and interleaved, in place
    struct rr { i64 s, h, d; int neox; bool inplace; };
    for (auto c : {rr{1, 64, 128, 1, false}, rr{1, 8, 128, 1, true}, rr{37, 4, 64, 0, false}, rr{5, 3, 256, 1, false}, rr{40, 64, 128, 1, true}, rr{3, 5, 32, 0, true}}) {
        const std::size_t N = std::size_t(c.s * c.h * c.d);
        auto x = rnd(N, 4), w = rnd(std::size_t(c.d), 5); std::vector<float> pos(std::size_t(c.s)), x2 = x, t1(N), y1(N), y2(N);
        for (std::size_t i = 0; i < pos.size(); ++i) pos[i] = float(i * 37 + 3);
        shape_t sh{c.s, c.h, c.d}, ws{c.d}, ps{c.s};
        ops::attrs ar{{"base", 1000000.0}, {"neox", double(c.neox)}}, an_{{"eps", 1e-6}}, af = ar; af.v["eps"] = 1e-6;
        rt.begin_run();
        rt.run_op("rms_norm", {{x.data(), sh}, {w.data(), ws}}, an_, {t1.data(), sh});
        rt.run_op("rope", {{t1.data(), sh}, {pos.data(), ps}}, ar, {y1.data(), sh});
        rt.to_host(y1.data(), N); rt.end_run();
        rt.begin_run();
        float* yp = c.inplace ? x2.data() : y2.data();
        bool ok = rt.run_fused("rms_rope", {{x2.data(), sh}, {w.data(), ws}, {pos.data(), ps}}, {{yp, sh}}, af);
        rt.to_host(yp, N); rt.end_run();
        if (c.inplace) y2 = x2;
        ++n;
        double worst = 0; for (std::size_t i = 0; i < N; ++i) worst = std::max(worst, std::fabs(double(y1[i]) - y2[i]));
        if (!ok || y1 != y2) { std::printf("FAIL %s rms_rope s %lld heads %lld d %lld: %s (worst %.3g)\n", api.c_str(), (long long)c.s, (long long)c.h, (long long)c.d,
                                           ok ? "differs from rms_norm + rope" : "refused", worst); ++fails; }
    }
    // a prefill rms_norm writes its q8r rows; the next two matmuls on its output reuse them (no forge_quant_q8r) and give
    // the same bits as a matmul that quantises the same input itself
    if (std::getenv("FORGE_FUSE_Q8R") == nullptr && rt.has_function("forge_quant_q8r")) {
        const auto* c4 = graph::codecs::find("q4_k");
        const std::size_t R = 48, K = 1024, C = 40, rb = c4->bytes(K);
        auto x = rnd(R * K, 6), w = rnd(K, 7), wf = rnd(C * K, 8); std::vector<float> h(R * K), o1(R * C), o2(R * C), o3(R * C);
        std::vector<std::uint8_t> enc(C * rb); for (std::size_t j = 0; j < C; ++j) c4->encode(wf.data() + j * K, K, enc.data() + j * rb);
        rt.begin_run();   // uploads the weight (an upload drops the q8r tracking)
        rt.run_qmatmul({x.data(), {i64(R), i64(K)}}, enc.data(), rb, std::uint32_t(C), 2, {o1.data(), {i64(R), i64(C)}});
        rt.end_run();
        const std::size_t r0 = disp("quant_q8r_reused"), q0 = disp("fused_q8r");
        rt.begin_run();
        rt.run_op("rms_norm", {{x.data(), {i64(R), i64(K)}}, {w.data(), {i64(K)}}}, {{"eps", 1e-6}}, {h.data(), {i64(R), i64(K)}});
        rt.run_qmatmul({h.data(), {i64(R), i64(K)}}, enc.data(), rb, std::uint32_t(C), 2, {o1.data(), {i64(R), i64(C)}});
        rt.run_qmatmul({h.data(), {i64(R), i64(K)}}, enc.data(), rb, std::uint32_t(C), 2, {o2.data(), {i64(R), i64(C)}});
        rt.to_host(h.data(), h.size()); rt.to_host(o1.data(), o1.size()); rt.to_host(o2.data(), o2.size()); rt.end_run();
        std::vector<float> hc = h;
        rt.begin_run(); rt.run_qmatmul({hc.data(), {i64(R), i64(K)}}, enc.data(), rb, std::uint32_t(C), 2, {o3.data(), {i64(R), i64(C)}}); rt.to_host(o3.data(), o3.size()); rt.end_run();
        const bool used = disp("fused_q8r") == q0 + 1, reused = disp("quant_q8r_reused") == r0 + 2;
        ++n;
        if (disp("qmatmul_mma") && (!used || !reused || o1 != o3 || o2 != o3)) {
            std::printf("FAIL %s rms_norm q8r output: emitted %d reused %d same %d\n", api.c_str(), int(used), int(reused), int(o1 == o3 && o2 == o3)); ++fails; }
    }
    // decode attention: forge_attention_comb_q81 writes the output projection's q8_1 input; the matmul reuses it
    {
        const i64 T = 37, HQ = 4, HK = 2, D = 128; const std::size_t K = std::size_t(HQ * D), C = 24;
        const auto* c8 = graph::codecs::find("q8_0"); const std::size_t rb = c8->bytes(K);
        auto q = rnd(std::size_t(HQ * D), 9), k = rnd(std::size_t(T * HK * D), 10), v = rnd(std::size_t(T * HK * D), 11), wf = rnd(C * K, 12);
        std::vector<float> y1(K), y2(K), o1(C), o2(C);
        std::vector<std::uint8_t> enc(C * rb); for (std::size_t j = 0; j < C; ++j) c8->encode(wf.data() + j * K, K, enc.data() + j * rb);
        ops::attrs at{{"causal", 1.0}};
        auto attn = [&](std::vector<float>& y) { return rt.run_op("attention", {{q.data(), {1, HQ, D}}, {k.data(), {T, HK, D}}, {v.data(), {T, HK, D}}}, at, {y.data(), {1, HQ, D}}); };
        attn(y2);   // outside a run: no fusion
        rt.begin_run(); rt.run_qmatmul({y2.data(), {1, i64(K)}}, enc.data(), rb, std::uint32_t(C), 1, {o2.data(), {1, i64(C)}}); rt.to_host(o2.data(), C); rt.end_run();
        const std::size_t r0 = disp("quant_q81_reused");
        rt.begin_run(); attn(y1); rt.run_qmatmul({y1.data(), {1, i64(K)}}, enc.data(), rb, std::uint32_t(C), 1, {o1.data(), {1, i64(C)}});
        rt.to_host(y1.data(), K); rt.to_host(o1.data(), C); rt.end_run();
        ++n;
        if (disp("attention_dec") && (y1 != y2 || o1 != o2 || disp("quant_q81_reused") != r0 + 1)) {
            std::printf("FAIL %s attention_comb_q81: y same %d, matmul same %d, reused %d\n", api.c_str(), int(y1 == y2), int(o1 == o2), int(disp("quant_q81_reused") - r0)); ++fails; }
    }
    std::printf("%s %d/%d %s elementwise fusions match the unfused kernels\n", fails ? "FAIL" : "ok  ", n - fails, n, api.c_str());
    return fails;
    }
}

// The executor's fused step pairs (plan.hpp fuse_steps): residual add + rms_norm and QK-norm + rope on the device
// against the CPU executor.
static int fusion_plan_check(graph::registry& reg, const std::string& api, const std::function<std::size_t(const char*)>& disp) {
    if (!reg.hooks || !reg.hooks->fused) return 0;
    for (auto v : {"FORGE_FUSE_ADD_NORM", "FORGE_FUSE_QK_ROPE"}) if (auto* e = std::getenv(v); e && std::string(e) == "0") return 0;
    using namespace graph; using ops::op;
    const i64 S = 20, D = 256, H = 4, d = 64;
    auto build = [&] {
        tensor_graph g;
        auto a = g.input("a", {S, D}), b = g.input("b", {S, D}), w = g.input("w", {D}), q = g.input("q", {S, H, d}), qw = g.input("qw", {d}), pos = g.input("pos", {S});
        auto s = g.add(a, b);
        auto hn = g.apply(op::rms_norm, {s, w}, {{"eps", 1e-5}});
        auto r = g.apply(op::rope, {g.apply(op::rms_norm, {q, qw}, {{"eps", 1e-6}}), pos}, {{"base", 10000.0}, {"neox", 1.0}});
        g.output("s", s); g.output("h", hn); g.output("r", r);
        return g;
    };
    auto rnd = [](std::size_t m, float ph) { std::vector<float> v(m); for (std::size_t i = 0; i < m; ++i) v[i] = std::sin(float(i) * 0.37f + ph); return v; };
    std::vector<float> pos(S); for (std::size_t i = 0; i < pos.size(); ++i) pos[i] = float(i);
    auto run = [&](registry* r) {
        pass_options po; po.lowerings = r; auto p = compile(build(), nullptr, po); executor e(*p);
        e.set_input("a", rnd(S * D, 1)); e.set_input("b", rnd(S * D, 2)); e.set_input("w", rnd(D, 3)); e.set_input("q", rnd(S * H * d, 4));
        e.set_input("qw", rnd(d, 5)); e.set_input("pos", pos); e.run();
        std::vector<float> out; for (auto nm : {"s", "h", "r"}) { auto t = e.output(nm); out.insert(out.end(), t.data.begin(), t.data.end()); }
        return out;
    };
    const std::size_t f1 = disp("fused_add_rms_norm"), f2 = disp("fused_rms_rope");
    auto dev = run(&reg), cpu = run(nullptr);
    double worst = dev.size() == cpu.size() ? 0 : 1e9;
    for (std::size_t i = 0; i < std::min(dev.size(), cpu.size()); ++i) worst = std::max(worst, std::fabs(double(dev[i]) - cpu[i]) / (1.0 + std::fabs(double(cpu[i]))));
    const bool fused = disp("fused_add_rms_norm") > f1 && disp("fused_rms_rope") > f2, ok = fused && worst < 1e-4;
    std::printf("%s %s executor runs add+rms_norm and rms_norm+rope fused (%d) and matches the CPU (worst rel %.3g)\n", ok ? "ok  " : "FAIL", api.c_str(), int(fused), worst);
    return ok ? 0 : 1;
}

static void set_env(const char* k, const std::string& v);
// FFN gate/up/SiLU fusion (plan.hpp fuse_glu + run_qglu): y = (silu(x Wg^T) * (x Wu^T)) Wd^T as a compiled plan on the
// device, fused vs compiled with FORGE_FUSE_GLU=0 vs the CPU executor. Rows < 16: decode (forge_qmatmul_q81 over the
// interleaved weight + forge_glu_q81, whose q8_1 the down projection reuses); >= 16: forge_qmatmul_glu_* (tensor cores),
// whose q8r output the down projection reuses. q5_k has no fused kernel: the plan must stay unfused.
template<class RT> static int glu_check(graph::registry& reg, RT& rt, const std::string& api) {
    if (!reg.device_qglu) { std::printf("note %s: no fused FFN gate/up\n", api.c_str()); return 0; }
    using namespace graph;
    int fails = 0, n = 0;
    // static: device weights are cached by host address, and glu_check runs more than once per process
    static std::deque<std::vector<std::uint8_t>> keep;
    auto disp = [&](const char* key) -> std::size_t { auto d = rt.dispatches(); auto it = d.find(key); return it == d.end() ? 0 : it->second; };
    for (std::string cn : {"q8_0", "q4_k", "q6_k", "q5_k"}) for (i64 rows : {1, 3, 16, 70, 130}) {
        const auto* c = codecs::find(cn);
        const i64 k = 512, ff = 256, od = 40;
        auto enc = [&](i64 r, i64 kk, float ph) {
            std::vector<float> w(std::size_t(r * kk)); std::vector<std::uint8_t> e(std::size_t(r) * c->bytes(std::size_t(kk)));
            for (std::size_t i = 0; i < w.size(); ++i) w[i] = std::sin(float(i) * 0.731f + ph) * (0.05f + float(i % 89) / 900.f);
            for (i64 j = 0; j < r; ++j) c->encode(w.data() + j * kk, std::size_t(kk), e.data() + std::size_t(j) * c->bytes(std::size_t(kk)));
            return e; };
        // kept alive for the whole check: device weights are cached by host address, so a freed buffer's address must
        // not come back with other contents
        auto& eg = keep.emplace_back(enc(ff, k, 1.f)); auto& eu = keep.emplace_back(enc(ff, k, 2.f)); auto& ed = keep.emplace_back(enc(od, ff, 3.f));
        std::vector<float> x(std::size_t(rows * k)); for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::cos(float(i) * 0.217f);
        auto build = [&] {
            tensor_graph g; auto xi = g.input("x", {rows, k});
            auto gt = g.apply(ops::op::matmul_t, {xi, g.qweight("wg", {ff, k}, eg.data(), cn)});
            auto up = g.apply(ops::op::matmul_t, {xi, g.qweight("wu", {ff, k}, eu.data(), cn)});
            g.output("y", g.apply(ops::op::matmul_t, {g.mul(g.apply(ops::op::silu, {gt}), up), g.qweight("wd", {od, ff}, ed.data(), cn)}));
            return g; };
        int fused_steps = 0;
        auto run = [&](registry* r) {
            pass_options po; po.lowerings = r; auto p = compile(build(), nullptr, po);
            for (auto& s : p->steps) if (s.low && s.low->name.ends_with(".qglu")) ++fused_steps;
            executor e(*p); e.set_input("x", x); e.run(); return e.output("y").data; };
        run(&reg);   // warm-up: the first run uploads the weights (an upload drops the quantised-activation reuse)
        fused_steps = 0;
        const std::size_t glu0 = disp("qmatmul_glu"), re0 = disp("quant_q8r_reused") + disp("quant_q81_reused");
        auto fused = run(&reg);
        const int fs = fused_steps;
        const bool took = disp("qmatmul_glu") > glu0, reused = disp("quant_q8r_reused") + disp("quant_q81_reused") > re0;
        set_env("FORGE_FUSE_GLU", "0"); fused_steps = 0;
        auto plain = run(&reg);
        set_env("FORGE_FUSE_GLU", "");
        const int ps = fused_steps;
        auto cpu = run(nullptr);
        double mx = 0, dp = 0, dc = 0;
        for (auto v : cpu) mx = std::max(mx, double(std::fabs(v)));
        for (std::size_t i = 0; i < cpu.size() && fused.size() == cpu.size() && plain.size() == cpu.size(); ++i) {
            dp = std::max(dp, std::fabs(double(fused[i]) - plain[i])); dc = std::max(dc, std::fabs(double(fused[i]) - cpu[i])); }
        const bool want = cn != "q5_k";
        bool ok = fused.size() == cpu.size() && dp <= 2e-3 * (1 + mx) && dc <= 2e-2 * (1 + mx) && ps == 0 && fs == (want ? 1 : 0);
        // where the runtime takes it (sm_80 image / mock), the fused kernel ran and the down projection reused its quantised output
        if (want && took && !reused) ok = false;
        ++n;
        if (!ok) { std::printf("FAIL %s.qglu %s rows %lld: fused steps %d/%d, ran %d, reused %d, |fused-unfused| %.3g, |fused-cpu| %.3g (max %.3g)\n", api.c_str(),
                               cn.c_str(), (long long)rows, fs, ps, int(took), int(reused), dp, dc, mx); ++fails; }
    }
    std::printf("%s %d/%d %s fused FFN gate/up cases match the unfused plan and the CPU\n", fails ? "FAIL" : "ok  ", n - fails, n, api.c_str());
    return fails;
}

// Every runtime with the cuda.hpp contract: open(why, ordinal) via the Open functor.
template<class RT, class Open> static int device_t(Open open) {
    std::string why; auto rt = open(&why, 0);
    if (!rt) { std::printf("SKIP: %s\n", why.c_str()); return 77; }
    const std::string api = rt->backend();
    std::printf("%s device: %s (image %s)\n", api.c_str(), rt->device_name().c_str(), rt->image_arch().c_str());
    int dec_fails = 0;
    if constexpr (requires { rt->run_op("attention", {}, ops::attrs{}, tensor_view{}); })
        if (api == "cuda" || api == "hip") { dec_fails = attention_dec_check(*rt, api); dec_fails += fusion_check(*rt, api); }
    auto shared = std::shared_ptr<RT>(std::move(rt));
    auto reg = graph::gpu_registry(shared);
    auto mma_runs = [&]() -> std::size_t { auto d = shared->dispatches(); auto it = d.find("qmatmul_mma"); return it == d.end() ? 0 : it->second; };
    int fails = 0, n = 0;
    for (std::size_t i = 0; i < ops::op_count; ++i) for (auto& l : reg->lowerings(ops::op(i))) {
        if (l.name.rfind(api + ".", 0) != 0) continue;
        std::string msg; ++n;
        if (!graph::registry::oracle_check(l, &msg)) { std::printf("FAIL %s: %s\n", l.name.c_str(), msg.c_str()); ++fails; }
    }
    std::printf("%s %d/%d %s lowerings pass the oracle\n", fails ? "FAIL" : "ok  ", n - fails, n, api.c_str());
    fails += dec_fails;
    fails += qmatmul_check<RT>(*reg, api, mma_runs);
    if constexpr (requires { shared->run_qglu(tensor_view{}, nullptr, nullptr, std::size_t(0), 0u, 0u, tensor_view{}); }) {
        fails += glu_check(*reg, *shared, api);
        fails += streamk_check(*reg, *shared, api);
        // the fused GLU kernel with stream-K over single 256-value chunks (every tile split across blocks)
        set_env("FORGE_MMA_SK_MIN", "1"); set_env("FORGE_MMA_STREAMK", "2"); fails += glu_check(*reg, *shared, api);
        set_env("FORGE_MMA_SK_MIN", ""); set_env("FORGE_MMA_STREAMK", "");
    }
    fails += state_ops_check(*reg, api);
    if constexpr (requires { shared->run_fused(std::string(), std::vector<tensor_view>(), std::vector<tensor_view>(), ops::attrs()); })
        fails += fusion_plan_check(*reg, api, [&](const char* k) -> std::size_t { auto d = shared->dispatches(); auto it = d.find(k); return it == d.end() ? 0 : it->second; });
    return fails ? 1 : 0;
}

// Streams, residency, graphs, multi-device and peer copies (runs on hardware or the mock drivers).
template<class Open, class Count> static int features_t(Open open, Count device_count) {
    std::string why; auto rt = open(&why, 0);
    if (!rt) { std::printf("SKIP: %s\n", why.c_str()); return 77; }
    int fails = 0;
    auto chk = [&](bool c, const char* what) { std::printf("%s %s\n", c ? "ok  " : "FAIL", what); fails += !c; };
    std::printf("%s: %d device(s), streams %d, graphs %d\n", rt->backend(), device_count(), rt->has_streams(), rt->has_graphs());
    // residency: a chain of ops inside one run touches the host only at the end
    std::vector<float> x(1000), y(1000), z(1000);
    for (int i = 0; i < 1000; ++i) x[std::size_t(i)] = float(i % 17) * 0.25f - 2;
    shape_t sh{1000};
    rt->begin_run();
    rt->run_op("exp", {{x.data(), sh}}, {}, {y.data(), sh});
    rt->run_op("mul", {{y.data(), sh}, {x.data(), sh}}, {}, {z.data(), sh});
    auto st = rt->residency();
    rt->end_run();
    bool ok = true; for (int i = 0; i < 1000; ++i) ok &= std::abs(z[std::size_t(i)] - std::exp(x[std::size_t(i)]) * x[std::size_t(i)]) <= 1e-5f * (1 + std::abs(z[std::size_t(i)]));
    chk(ok, "resident exp*x chain matches the host");
    chk(st.uploads == 1 && st.downloads == 0, "residency: one upload, no downloads mid-run");
    chk(rt->residency().resident == 0, "end_run frees unpinned buffers");
    // device-side copy into a slot (KV append path)
    std::vector<float> slot(64, 0.f), row(16, 3.f);
    rt->begin_run(); rt->copy_resident(row.data(), 16, slot.data(), 64, 16); rt->to_host(slot.data(), 64);
    chk(slot[16] == 3.f && slot[31] == 3.f && slot[15] == 0.f && slot[32] == 0.f, "copy_resident writes the slot on the device"); rt->end_run();
    // greedy argmax on a device-newer buffer (multi-block over a vocabulary-sized row; ties: the lowest index), twice
    // per size so the blocks' ticket counter is seen to reset
    if constexpr (requires { rt->argmax_resident((const float*)nullptr, std::size_t(0)); }) {
        bool am_ok = true;
        for (std::size_t n : {std::size_t(1000), std::size_t(248320), std::size_t(3)})
            for (std::size_t at : {n - 1, n / 3}) {
                std::vector<float> lv(n), lo(n);
                for (std::size_t i = 0; i < n; ++i) lv[i] = float((i * 7919) % 1000) * 1e-3f;
                lv[at] = 5.f; if (at + 1 < n) lv[n - 1] = 5.f;   // a tie after `at`
                shape_t ls{i64(n)};
                rt->begin_run(); rt->run_op("scale", {{lv.data(), ls}}, {{"alpha", 1.0}}, {lo.data(), ls});
                const long long r = rt->argmax_resident(lo.data(), n); rt->end_run();
                if (r != (long long)at) { am_ok = false; std::printf("     argmax n %zu: %lld, want %zu\n", n, r, at); }
            }
        chk(am_ok, "argmax_resident finds the first largest value");
    }
    // graphs
    if (rt->has_graphs()) {
        // warm run makes the buffers resident, then capture the same op and replay it on new input
        rt->begin_run();
        rt->run_op("exp", {{x.data(), sh}}, {}, {y.data(), sh});
        rt->capture_begin();
        rt->run_op("scale", {{y.data(), sh}}, {{"alpha", 2.0}}, {z.data(), sh});
        auto g = rt->capture_end();
        std::fill(y.begin(), y.end(), 1.5f); rt->host_wrote(y.data(), y.size());
        rt->graph_launch(g); rt->to_host(z.data(), z.size());
        chk(z[0] == 3.f && z[999] == 3.f, "captured graph replays on updated input");
        rt->end_run();
    } else std::puts("note: driver has no graph API");
    // multi-device + peer copy
    if (device_count() > 1) {
        auto r1 = open(&why, 1);
        chk(r1 != nullptr, "open device 1");
        if (r1) {
            auto s0 = rt->device_alloc(4000), s1 = r1->device_alloc(4000);
            rt->upload(s0, x.data(), 4000); rt->copy_to_peer(*r1, s1, s0, 4000);
            std::vector<float> back(1000); r1->download(back.data(), s1, 4000);
            chk(back == x, "peer copy device 0 -> 1");
            rt->device_free(s0); r1->device_free(s1);
        }
    }
    chk(!open(&why, 99) && !why.empty(), "an out-of-range ordinal fails with a reason");
    auto m = rt->memory(); std::printf("     memory: %zu / %zu bytes free\n", m.free, m.total);
    return fails ? 1 : 0;
}

static void set_env(const char* k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
}

// Device memory accounting (gpu/device_memory.hpp, docs/memory.md): every allocation is charged to the pool
// "<backend>:0"; resident buffers are evicted least recently used first (synced to the host) when an allocation
// would cross the pool's limit or the driver runs out; an op that cannot fit even then fails with out_of_memory.
template<class Open> static int memory_t(Open open) {
    std::string why; auto rt = open(&why, 0);
    if (!rt) { std::printf("SKIP: %s\n", why.c_str()); return 77; }
    int fails = 0;
    auto chk = [&](bool c, const std::string& what) { std::printf("%s %s\n", c ? "ok  " : "FAIL", what.c_str()); fails += !c; };
    auto* pool = rt->memory_pool();
    chk(pool && pool->name() == std::string(rt->backend()) + ":0", std::string("runtime charges the pool ") + (pool ? pool->name() : "?"));
    if (!pool) return 1;
    pool->set_limit(0);
    const std::size_t N = 1000, B = N * 4;
    shape_t sh{i64(N)};
    std::vector<float> w0(N), w1(N), w2(N), y(N), z(N);
    for (std::size_t i = 0; i < N; ++i) { w0[i] = float(i % 13) * 0.5f - 3; w1[i] = float(i % 7) + 1; w2[i] = float(i % 5) - 2; }
    for (auto* w : {&w0, &w1, &w2}) rt->pin(w->data(), N);   // weights: resident across runs
    auto same = [&](const std::vector<float>& v, auto f) { for (std::size_t i = 0; i < N; ++i) if (std::abs(v[i] - f(i)) > 1e-5f) return false; return true; };
    auto pooled = [&] { if constexpr (requires { rt->residency().pooled_bytes; }) return std::uint64_t(rt->residency().pooled_bytes); else return std::uint64_t(0); };
    auto base = pool->used();

    // 1. accounting: a resident op charges its buffers; after the run the pinned weights (and pooled buffers) stay charged
    rt->begin_run();
    rt->run_op("add", {{w0.data(), sh}, {w1.data(), sh}}, {}, {y.data(), sh});
    chk(pool->used() - base == 3 * B && rt->charged_bytes() == 3 * B, std::format("add(w0, w1) charges 3 x {} bytes (used {})", B, pool->used() - base));
    chk(pool->stats().by_tag["resident"] >= 3 * B, "charged under the tag \"resident\"");
    rt->end_run();
    chk(same(y, [&](std::size_t i) { return w0[i] + w1[i]; }), "add result");
    chk(pool->used() - base == 2 * B + pooled(), std::format("after end_run: 2 pinned weights + {} pooled bytes stay charged (used {})", pooled(), pool->used() - base));

    // 2. a limit of 3 buffers: the next op needs 4, so the least recently used weight not bound by the op goes
    pool->set_limit(base + 3 * B);
    auto ev0 = rt->residency().evictions, up0 = rt->residency().uploads;
    rt->begin_run();
    rt->run_op("mul", {{w2.data(), sh}, {w0.data(), sh}}, {}, {z.data(), sh});
    rt->end_run();
    chk(same(z, [&](std::size_t i) { return w2[i] * w0[i]; }), "mul result under a 3-buffer budget");
    chk(rt->residency().evictions > ev0 && pool->stats().evictions > 0, std::format("an allocation over the budget evicted ({} evictions)", rt->residency().evictions - ev0));
    chk(pool->stats().peak <= base + 3 * B && pool->used() <= base + 3 * B, std::format("usage never crossed the limit (peak {})", pool->stats().peak - base));
    // w1 was evicted (LRU, not bound by mul): using it again uploads it; w0 stayed resident
    up0 = rt->residency().uploads;
    rt->begin_run(); rt->run_op("scale", {{w0.data(), sh}}, {{"alpha", 2.0}}, {y.data(), sh}); rt->end_run();
    chk(rt->residency().uploads == up0 && same(y, [&](std::size_t i) { return 2 * w0[i]; }), "w0 (used most recently) stayed resident");
    rt->begin_run(); rt->run_op("scale", {{w1.data(), sh}}, {{"alpha", 2.0}}, {y.data(), sh}); rt->end_run();
    chk(rt->residency().uploads == up0 + 1 && same(y, [&](std::size_t i) { return 2 * w1[i]; }), "w1 (least recently used) was evicted and is uploaded again");

    // 3. eviction requested from outside (pressure response): least recently used first
    ev0 = rt->residency().evictions;
    pool->evict(pooled() + 1);   // the pool's free buffers, then one resident weight: w0 (used before w1)
    up0 = rt->residency().uploads;
    rt->begin_run(); rt->run_op("scale", {{w1.data(), sh}}, {{"alpha", 3.0}}, {y.data(), sh}); rt->end_run();
    chk(rt->residency().evictions > ev0 && rt->residency().uploads == up0, "budget::evict drops the LRU weight and keeps the recent one");

    // 4. an op that cannot fit even after evicting everything fails with out_of_memory, and leaves the runtime usable
    pool->set_limit(base + 2 * B);
    auto den = pool->stats().denials; bool oom = false;
    rt->begin_run();
    try { rt->run_op("add", {{w0.data(), sh}, {w1.data(), sh}}, {}, {y.data(), sh}); }
    catch (const forge_exception& e) { oom = e.code == errc::out_of_memory; std::printf("     %s\n", e.what()); }
    rt->end_run();
    chk(oom && pool->stats().denials > den, "an op larger than the budget fails with errc::out_of_memory");
    pool->set_limit(0);
    rt->begin_run(); rt->run_op("add", {{w0.data(), sh}, {w1.data(), sh}}, {}, {y.data(), sh}); rt->end_run();
    chk(same(y, [&](std::size_t i) { return w0[i] + w1[i]; }), "the runtime recovers once the limit is lifted");

#if !defined(_WIN32)   // the mock reads MOCK_MEM_LIMIT through its own C runtime
    // 5. (mock driver) the driver runs out with no budget set: evict resident buffers, including an unsynced output, and retry
    if (rt->device_name().find("forge mock") != std::string::npos && (std::string(rt->backend()) == "cuda" || std::string(rt->backend()) == "hip")) {
        pool->evict(0);
        const std::size_t M = 1500;   // a size the pool has no buffer for
        shape_t sm{i64(M)};
        std::vector<float> a(M), b(M), c(M), d(M);
        for (std::size_t i = 0; i < M; ++i) { a[i] = float(i % 11) * 0.1f; c[i] = float(i % 3) * 0.2f; }
        auto m = rt->memory(); std::size_t live = m.total - m.free;
        set_env("MOCK_MEM_LIMIT", std::to_string(live + 2 * M * 4));
        ev0 = rt->residency().evictions;
        rt->begin_run();
        rt->run_op("exp", {{a.data(), sm}}, {}, {b.data(), sm});   // a, b: the driver is now full
        rt->run_op("exp", {{c.data(), sm}}, {}, {d.data(), sm});   // c evicts a, d evicts b (downloaded first)
        rt->end_run();
        set_env("MOCK_MEM_LIMIT", "");
        bool ok = true; for (std::size_t i = 0; i < M; ++i) ok &= std::abs(b[i] - std::exp(a[i])) < 1e-5f && std::abs(d[i] - std::exp(c[i])) < 1e-5f;
        chk(ok && rt->residency().evictions >= ev0 + 2, std::format("driver out of memory: {} evictions, results intact", rt->residency().evictions - ev0));
    }
#endif
    rt.reset();
    chk(pool->used() == base, std::format("destroying the runtime returns every byte (used {})", pool->used() - base));
    return fails ? 1 : 0;
}

// test_cuda_hip bench <codec> <rows> <cols> <k> [reps] [VAR=a,b,...]: time one quantised matmul on the device (x and y stay
// resident inside a run, so only the kernels are timed). Not part of ctest; for kernel work (e.g. q4_k 512 25600 5120).
// With VAR=a,b,...: batches alternate between the values of environment variable VAR (read per launch, e.g.
// FORGE_MMA_STREAMK=0,1 or FORGE_MMA_BT=64,128), one line each: an A/B on a shared GPU in one process.
static int bench_qmm(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: test_cuda_hip bench <codec> <rows> <cols> <k> [reps] [VAR=a,b,...]\n"); return 2; }
    std::string why; auto rt = cuda_hip::runtime::open(cuda_hip::api::cuda, &why, 0);
    if (!rt) { std::printf("SKIP: %s\n", why.c_str()); return 77; }
    const auto* c = graph::codecs::find(argv[2]);
    if (!c) { std::fprintf(stderr, "unknown codec %s\n", argv[2]); return 2; }
    const std::size_t rows = std::stoul(argv[3]), cols = std::stoul(argv[4]), k = std::stoul(argv[5]), rb = c->bytes(k);
    const int reps = argc > 6 ? std::stoi(argv[6]) : 20;
    std::vector<float> wrow(k), x(rows * k), y(rows * cols);
    std::vector<std::uint8_t> enc(cols * rb);
    for (std::size_t j = 0; j < cols; ++j) {
        for (std::size_t i = 0; i < k; ++i) wrow[i] = std::sin(float(j * k + i) * 0.731f) * 0.05f;
        c->encode(wrow.data(), k, enc.data() + j * rb);
    }
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::cos(float(i) * 0.217f);
    std::uint32_t fmt = c->name == "q8_0" ? 1 : c->name == "q4_k" ? 2 : c->name == "q6_k" ? 3 : c->name == "q5_k" ? 4 : 0;
    tensor_view xv{x.data(), {i64(rows), i64(k)}}, yv{y.data(), {i64(rows), i64(cols)}};
    rt->begin_run();
    rt->run_qmatmul(xv, enc.data(), rb, std::uint32_t(cols), fmt, yv);   // upload + warm up
    rt->wait();
    // batches of `reps` back-to-back calls, one wait each; the fastest batch is kept (the GPU may be shared, and
    // the minimum best approximates an idle device). Per-call time = batch time / reps.
    std::string var; std::vector<std::string> vals{""};
    if (argc > 7) {
        const std::string a = argv[7]; const auto eq = a.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "expected VAR=a,b,...\n"); return 2; }
        var = a.substr(0, eq); vals.clear();
        for (std::size_t p = eq + 1;;) { auto q = a.find(',', p); vals.push_back(a.substr(p, q == std::string::npos ? q : q - p)); if (q == std::string::npos) break; p = q + 1; }
    }
    std::vector<std::vector<double>> ts(vals.size());
    for (int b = 0; b < 7; ++b) for (std::size_t v = 0; v < vals.size(); ++v) {
        if (!var.empty()) { set_env(var.c_str(), vals[v]); rt->run_qmatmul(xv, enc.data(), rb, std::uint32_t(cols), fmt, yv); rt->wait(); }
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) rt->run_qmatmul(xv, enc.data(), rb, std::uint32_t(cols), fmt, yv);
        rt->wait();
        ts[v].push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps);
    }
    rt->end_run();
    auto d = rt->dispatches(); std::string path;
    for (const char* p : {"qmatmul_mma", "qmatmul_mmq", "qmatmul_q81"}) if (d.count(p)) path = p;
    for (std::size_t v = 0; v < vals.size(); ++v) {
        std::sort(ts[v].begin(), ts[v].end()); const double us = ts[v].front(), med = ts[v][ts[v].size() / 2];
        std::printf("%s rows %zu cols %zu k %zu%s%s%s%s: min %.1f us (median %.1f), %.1f TOPS, %.0f GB/s weights (%s)\n", argv[2], rows, cols, k,
                    var.empty() ? "" : " ", var.c_str(), var.empty() ? "" : "=", vals[v].c_str(), us, med,
                    2.0 * double(rows) * double(cols) * double(k) / us / 1e6, double(enc.size()) / us / 1e3, path.c_str());
    }
    return 0;
}

// test_cuda_hip benchfuse: time the elementwise fusions against the unfused kernels inside a run (Goedel/Qwen3-32B
// shapes: decode and 512-token prefill). Not part of ctest.
static int bench_fuse() {
    std::string why; auto rt = cuda_hip::runtime::open(cuda_hip::api::cuda, &why, 0);
    if (!rt) { std::printf("SKIP: %s\n", why.c_str()); return 77; }
    auto time = [&](auto&& f) {
        std::vector<double> ts;
        for (int b = 0; b < 7; ++b) { auto t0 = std::chrono::steady_clock::now(); for (int i = 0; i < 50; ++i) f(); rt->wait();
            ts.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 50); }
        std::sort(ts.begin(), ts.end()); return ts.front(); };
    for (i64 rows : {1, 512}) {
        const i64 D = 5120; const std::size_t N = std::size_t(rows * D);
        std::vector<float> a(N, 0.5f), b(N, 0.25f), s(N), y(N), w(std::size_t(D), 1.f);
        shape_t sh{rows, D}, ws{D}; ops::attrs at{{"eps", 1e-6}};
        rt->begin_run();
        double u = time([&] { rt->run_op("add", {{a.data(), sh}, {b.data(), sh}}, {}, {s.data(), sh}); rt->run_op("rms_norm", {{s.data(), sh}, {w.data(), ws}}, at, {y.data(), sh}); });
        double f = time([&] { rt->run_fused("add_rms_norm", {{a.data(), sh}, {b.data(), sh}, {w.data(), ws}}, {{s.data(), sh}, {y.data(), sh}}, at); });
        rt->end_run();
        std::printf("add+rms_norm rows %lld cols %lld: unfused %.1f us, fused %.1f us\n", (long long)rows, (long long)D, u, f);
    }
    for (auto [s_, h] : {std::pair<i64, i64>{1, 64}, {1, 8}, {512, 64}, {512, 8}}) {
        const i64 d = 128; const std::size_t N = std::size_t(s_ * h * d);
        std::vector<float> x(N, 0.3f), t(N), y(N), w(static_cast<std::size_t>(d), 1.f), pos(static_cast<std::size_t>(s_));
        for (std::size_t i = 0; i < pos.size(); ++i) pos[i] = float(i + 100);
        shape_t sh{s_, h, d}, ws{d}, ps{s_}; ops::attrs ar{{"base", 1000000.0}, {"neox", 1.0}}, an{{"eps", 1e-6}}, af = ar; af.v["eps"] = 1e-6;
        rt->begin_run();
        double u = time([&] { rt->run_op("rms_norm", {{x.data(), sh}, {w.data(), ws}}, an, {t.data(), sh}); rt->run_op("rope", {{t.data(), sh}, {pos.data(), ps}}, ar, {y.data(), sh}); });
        double f = time([&] { rt->run_fused("rms_rope", {{x.data(), sh}, {w.data(), ws}, {pos.data(), ps}}, {{y.data(), sh}}, af); });
        rt->end_run();
        std::printf("rms_norm+rope s %lld heads %lld d %lld: unfused %.1f us, fused %.1f us\n", (long long)s_, (long long)h, (long long)d, u, f);
    }
    return 0;
}

// test_cuda_hip benchglu <codec> <rows> <n> <k> [reps]: the FFN gate/up/SiLU step, fused (run_qglu) against the two
// matmuls + silu_mul it replaces, each followed by the down projection's activation quantisation path (a [rows, n] x
// [64, n] matmul, so the fused q8r hand-off is counted). Interleaved batches; the fastest of each is kept.
static int bench_glu(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: test_cuda_hip benchglu <codec> <rows> <n> <k> [reps]\n"); return 2; }
    std::string why; auto rt = cuda_hip::runtime::open(cuda_hip::api::cuda, &why, 0);
    if (!rt) { std::printf("SKIP: %s\n", why.c_str()); return 77; }
    const auto* c = graph::codecs::find(argv[2]);
    if (!c) { std::fprintf(stderr, "unknown codec %s\n", argv[2]); return 2; }
    const std::size_t rows = std::stoul(argv[3]), n = std::stoul(argv[4]), k = std::stoul(argv[5]), rb = c->bytes(k), rbd = c->bytes(n);
    const int reps = argc > 6 ? std::stoi(argv[6]) : 10;
    auto enc = [&](std::size_t r, std::size_t kk, std::size_t b, float ph) {
        std::vector<float> w(kk); std::vector<std::uint8_t> e(r * b);
        for (std::size_t j = 0; j < r; ++j) { for (std::size_t i = 0; i < kk; ++i) w[i] = std::sin(float(j * kk + i) * 0.731f + ph) * 0.05f; c->encode(w.data(), kk, e.data() + j * b); }
        return e; };
    auto eg = enc(n, k, rb, 1), eu = enc(n, k, rb, 2), ed = enc(64, n, rbd, 3);
    std::vector<float> x(rows * k), g(rows * n), u(rows * n), h(rows * n), y(rows * 64);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::cos(float(i) * 0.217f);
    std::uint32_t fmt = c->name == "q8_0" ? 1 : c->name == "q4_k" ? 2 : c->name == "q6_k" ? 3 : 4;
    tensor_view xv{x.data(), {i64(rows), i64(k)}}, gv{g.data(), {i64(rows), i64(n)}}, uv{u.data(), {i64(rows), i64(n)}}, hv{h.data(), {i64(rows), i64(n)}}, yv{y.data(), {i64(rows), 64}};
    auto unfused = [&] { rt->run_qmatmul(xv, eg.data(), rb, std::uint32_t(n), fmt, gv); rt->run_qmatmul(xv, eu.data(), rb, std::uint32_t(n), fmt, uv);
                         rt->run_op("silu_mul", {gv, uv}, {}, hv); rt->run_qmatmul(hv, ed.data(), rbd, 64, fmt, yv); };
    auto fused = [&] { if (!rt->run_qglu(xv, eg.data(), eu.data(), rb, std::uint32_t(n), fmt, hv)) std::abort(); rt->run_qmatmul(hv, ed.data(), rbd, 64, fmt, yv); };
    rt->begin_run(); unfused(); fused(); rt->wait();
    double best[2] = {1e30, 1e30};
    for (int b = 0; b < 7; ++b) for (int f = 0; f < 2; ++f) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) { if (f) fused(); else unfused(); }
        rt->wait();
        best[f] = std::min(best[f], std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps);
    }
    rt->end_run();
    std::printf("%s rows %zu n %zu k %zu (+ down to 64): unfused %.1f us, fused %.1f us (%.1f%%)\n", argv[2], rows, n, k, best[0], best[1], 100.0 * (best[0] - best[1]) / best[0]);
    return 0;
}

int main(int argc, char** argv) {
    // The device kernels are checked against the CPU's exact f32 products: the CPU's default integer path rounds
    // activations to int8 itself, which is a second approximation, not a reference (FORGE_CPU_INT8 set: kept).
    if (!std::getenv("FORGE_CPU_INT8")) forge::os::setenv("FORGE_CPU_INT8", "0");
    std::string mode = argc > 1 ? argv[1] : "images";
    if (mode == "images") return images();
    if (mode == "bench") return bench_qmm(argc, argv);
    if (mode == "benchfuse") return bench_fuse();
    if (mode == "benchglu") return bench_glu(argc, argv);
    auto cu = [](cuda_hip::api a) { return [a](std::string* w, int o) { return cuda_hip::runtime::open(a, w, o); }; };
    auto cl = [](std::string* w, int o) { return forge::gpu::ocl::runtime::open(w, o); };
    auto ze = [](std::string* w, int o) { return forge::gpu::ze::runtime::open(w, o); };
    if (mode == "cuda") return device_t<cuda_hip::runtime>(cu(cuda_hip::api::cuda));
    if (mode == "hip") return device_t<cuda_hip::runtime>(cu(cuda_hip::api::hip));
    if (mode == "opencl") return device_t<forge::gpu::ocl::runtime>(cl);
    if (mode == "ze") return device_t<forge::gpu::ze::runtime>(ze);
    if (mode == "metal") return device_t<forge::gpu::metal::runtime>([](std::string* w, int o) { return forge::gpu::metal::runtime::open(w, o); });
    if (mode == "features") {
        std::string w = argc > 2 ? argv[2] : "cuda";
        if (w == "opencl") return features_t(cl, [] { return forge::gpu::ocl::runtime::device_count(); });
        if (w == "ze") return features_t(ze, [] { return forge::gpu::ze::runtime::device_count(); });
        auto a = w == "hip" ? cuda_hip::api::hip : cuda_hip::api::cuda;
        return features_t(cu(a), [a] { return cuda_hip::runtime::device_count(a); });
    }
    if (mode == "memory") {
        std::string w = argc > 2 ? argv[2] : "cuda";
        if (w == "opencl") return memory_t(cl);
        if (w == "ze") return memory_t(ze);
        return memory_t(cu(w == "hip" ? cuda_hip::api::hip : cuda_hip::api::cuda));
    }
    std::fprintf(stderr, "usage: test_cuda_hip [images|cuda|hip|opencl|ze|features <api>]\n"); return 2;
}
