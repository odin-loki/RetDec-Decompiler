#include "check.hpp"
#include <forge/gpu/prims.hpp>
#include <random>

using namespace forge;

struct Scale {
    float a;
    FORGE_KERNEL void operator()(gpu::item<3> it, gpu::span<float> y) const {
        if (auto i = it.global(0); i < i64(y.size())) y[std::size_t(i)] *= a;
    }
};

TEST(graph_first_record_compile_replay) {
    auto dev = gpu::select(gpu::prefer::discrete);
    std::size_t n = 100000;
    std::vector<float> host(n, 1.f), back(n, 0.f);
    auto buf = dev.alloc<float>(n);
    gpu::graph g;
    auto up = g.copy(gpu::span<const float>(host), buf);
    auto k = g.launch(Scale{2.0f}, gpu::extent{i64(n)}, buf); k.after(up);
    g.copy(buf, gpu::span<float>(back)).after(k);
    auto exe = g.compile(dev);
    CHECK(exe.steps() == 3);
    exe.run();
    CHECK(back[0] == 2.f && back[n - 1] == 2.f);
    exe.run();  // replay: host -> 2x again (host unchanged)
    CHECK(back[12345] == 2.f);
}
TEST(backend_probe_and_caps) {
    auto pr = gpu::probe_backends();
    CHECK(pr.size() == 6 && pr[0].available);
    for (auto& p : pr) std::printf("  %-10s %s %s\n", gpu::name(p.kind), p.available ? "yes" : "no ", p.detail.c_str());
    auto d = gpu::select();
    CHECK(d.caps().compute_units >= 1);
}
TEST(prims_reduce_scan_compact) {
    std::vector<std::int64_t> x(1 << 18); std::mt19937 r(5);
    for (auto& v : x) v = std::int64_t(r() % 100);
    std::int64_t ref = 0; for (auto v : x) ref += v;
    CHECK(gpu::prims::reduce<std::int64_t>(x) == ref);
    std::vector<std::int64_t> y(x.size());
    gpu::prims::scan<std::int64_t>(x, y, true);
    std::int64_t s = 0; bool ok = true;
    for (std::size_t i = 0; i < x.size(); ++i) { s += x[i]; ok &= y[i] == s; }
    CHECK(ok);
    gpu::prims::scan<std::int64_t>(x, y, false);
    CHECK(y[0] == 0 && y[1] == x[0]);
    std::vector<std::int64_t> out(x.size());
    auto c = gpu::prims::compact<std::int64_t>(x, out, [](auto v) { return v < 10; });
    std::size_t e = 0; for (auto v : x) e += v < 10; CHECK(c == e);
    for (std::size_t i = 0; i < c; ++i) CHECK(out[i] < 10);
}
TEST(prims_radix_sort_topk) {
    std::vector<std::uint32_t> k(50000), v(50000); std::mt19937 r(9);
    for (std::size_t i = 0; i < k.size(); ++i) { k[i] = r(); v[i] = std::uint32_t(i); }
    auto orig = k;
    gpu::prims::radix_sort(k, v);
    CHECK(std::is_sorted(k.begin(), k.end()));
    bool ok = true; for (std::size_t i = 0; i < k.size(); ++i) ok &= orig[v[i]] == k[i];
    CHECK(ok);
    std::vector<float> f = {0.1f, 5.f, 3.f, 5.f, -1.f};
    auto t = gpu::prims::top_k(f, 3);
    CHECK(t.size() == 3 && t[0] == 1 && t[1] == 3 && t[2] == 2);
}
TEST(prims_gemm_and_philox) {
    std::mt19937 r(2); std::uniform_real_distribution<float> d(-1, 1);
    std::size_t m = 37, n = 129, k = 70;
    std::vector<float> A(m * k), B(n * k), C(m * n);
    for (auto& x : A) x = d(r); for (auto& x : B) x = d(r);
    gpu::prims::gemm_nt(A.data(), B.data(), C.data(), m, n, k);
    double maxe = 0;
    for (std::size_t i = 0; i < m; ++i) for (std::size_t j = 0; j < n; ++j) {
        double s = 0; for (std::size_t p = 0; p < k; ++p) s += double(A[i * k + p]) * B[j * k + p];
        maxe = std::max(maxe, std::fabs(s - C[i * n + j]));
    }
    CHECK(maxe < 1e-4);
    std::vector<float> a(10), b(10);
    gpu::prims::philox_uniform(a, 42); gpu::prims::philox_uniform(b, 42);
    CHECK(a == b);
    for (float x : a) CHECK(x >= 0.f && x < 1.f);
    // known-answer test: Philox4x32-10 with zero key/counter (Random123 KAT)
    auto kat = rand::philox4x32{{0, 0}}(0, 0);
    CHECK(kat[0] == 0x6627e8d5u && kat[1] == 0xe169c58du && kat[2] == 0xbc57ac4cu && kat[3] == 0x9b00dbd8u);
}
