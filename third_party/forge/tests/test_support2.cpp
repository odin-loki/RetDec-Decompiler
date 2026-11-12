#include "check.hpp"
#include <forge/linalg/linalg.hpp>
#include <forge/mem/tlsf.hpp>
#include <forge/rand/philox.hpp>
#include <forge/reflect/reflect.hpp>
#include <forge/signal/audio.hpp>
#include <forge/vision/vision.hpp>
#include <cmath>
#include <random>

using namespace forge;

TEST(threefry_kat) {
    // Random123 known-answer vectors for threefry4x32_20.
    rand::threefry4x32 z{{0, 0, 0, 0}};
    auto r = z.block({0, 0, 0, 0}, {0, 0, 0, 0});
    CHECK((r == std::array<std::uint32_t, 4>{0x9c6ca96au, 0xe17eae66u, 0xfc10ecd4u, 0x5256a7d8u}));
    auto f = rand::threefry4x32::block({0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}, {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu});
    CHECK((f == std::array<std::uint32_t, 4>{0x2a881696u, 0x57012287u, 0xf6c7446eu, 0xa16a6732u}));
    auto p = rand::threefry4x32::block({0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u}, {0xa4093822u, 0x299f31d0u, 0x082efa98u, 0xec4e6c89u});
    CHECK((p == std::array<std::uint32_t, 4>{0x59cd1dbbu, 0xb8879579u, 0x86b5d00cu, 0xac8b6d84u}));
}
TEST(linalg_eig_svd_gmres) {
    std::mt19937 g(5); std::normal_distribution<double> d; const std::size_t n = 9, m = 13;
    linalg::mat<double> M(n, n); for (auto& v : M.a) v = d(g);
    linalg::mat<double> S(n, n); for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) S(i, j) = M(i, j) + M(j, i);
    std::vector<double> w; linalg::mat<double> V; linalg::sym_eig(S, w, V);
    double err = 0;
    for (std::size_t k = 0; k < n; ++k) for (std::size_t i = 0; i < n; ++i) { double s = 0; for (std::size_t j = 0; j < n; ++j) s += S(i, j) * V(j, k); err = std::max(err, std::fabs(s - w[k] * V(i, k))); }
    CHECK_MSG(err < 1e-10, "eig residual %g", err);
    for (std::size_t k = 1; k < n; ++k) CHECK(w[k] >= w[k - 1]);
    linalg::mat<double> A(m, n); for (auto& v : A.a) v = d(g);
    linalg::mat<double> U, W; std::vector<double> s; linalg::svd(A, U, s, W);
    err = 0;
    for (std::size_t i = 0; i < m; ++i) for (std::size_t j = 0; j < n; ++j) { double r = 0; for (std::size_t k = 0; k < n; ++k) r += U(i, k) * s[k] * W(j, k); err = std::max(err, std::fabs(r - A(i, j))); }
    CHECK_MSG(err < 1e-10, "svd reconstruction %g", err);
    for (std::size_t k = 1; k < n; ++k) CHECK(s[k] <= s[k - 1]);
    double orth = 0; for (std::size_t a = 0; a < n; ++a) for (std::size_t b = 0; b < n; ++b) { double t = 0; for (std::size_t i = 0; i < m; ++i) t += U(i, a) * U(i, b); orth = std::max(orth, std::fabs(t - (a == b))); }
    CHECK(orth < 1e-10);
    // Non-symmetric, diagonally dominant sparse system.
    const std::size_t N = 60; linalg::mat<double> B(N, N);
    for (std::size_t i = 0; i < N; ++i) { B(i, i) = 4; if (i + 1 < N) B(i, i + 1) = -1.3; if (i) B(i, i - 1) = -0.4; if (i + 7 < N) B(i, i + 7) = 0.5; }
    std::vector<double> x0(N), b(N), x; for (auto& v : x0) v = d(g);
    for (std::size_t i = 0; i < N; ++i) for (std::size_t j = 0; j < N; ++j) b[i] += B(i, j) * x0[j];
    int it = linalg::gmres(linalg::to_csr(B), b, x, 20, 1e-12, 500);
    CHECK(it > 0); err = 0; for (std::size_t i = 0; i < N; ++i) err = std::max(err, std::fabs(x[i] - x0[i]));
    CHECK_MSG(err < 1e-9, "gmres err %g it %d", err, it);
}
TEST(tlsf_random_ops_and_leaks) {
    std::vector<std::uint8_t> region(1 << 20);
    mem::tlsf t(region.data(), region.size());
    auto s0 = t.stat();
    std::mt19937 g(11); std::vector<std::pair<std::uint8_t*, std::size_t>> live;
    for (int i = 0; i < 20000; ++i) {
        if (live.empty() || g() % 3) {
            std::size_t n = g() % 4 ? g() % 200 + 1 : g() % 20000 + 1;
            auto* p = static_cast<std::uint8_t*>(t.malloc(n));
            if (!p) continue;
            CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
            std::memset(p, int(n & 0xff), n); live.push_back({p, n});
        } else {
            std::size_t k = g() % live.size(); auto [p, n] = live[k];
            bool intact = true; for (std::size_t q = 0; q < n; ++q) intact &= p[q] == std::uint8_t(n & 0xff);
            CHECK(intact); t.free(p); live[k] = live.back(); live.pop_back();
        }
        if (i % 1000 == 0) t.check();
    }
    CHECK(t.stat().live_allocs == live.size());
    CHECK(t.leak_report().size() == live.size());
    CHECK(t.stat().peak_bytes >= t.stat().live_bytes);
    for (auto [p, n] : live) t.free(p);
    t.check();
    CHECK(t.leak_report().empty());
    CHECK(t.stat().free_bytes == s0.free_bytes && t.stat().largest_free == s0.free_bytes);   // fully coalesced
    CHECK_THROWS(t.free(region.data() + 64));
    mem::pool pl(24, 4); void* a[4]; for (auto& x : a) { x = pl.alloc(); CHECK(x); } CHECK(!pl.alloc()); pl.free(a[2]); CHECK(pl.alloc() == a[2]); CHECK(pl.live() == 4);
}
struct inner { float x = 0; std::vector<int> ids; };
FORGE_REFLECT(inner, x, ids)
enum class mode { a = 3, b = 7 };
struct save { int level = 1; std::string name; bool hard = false; mode m = mode::a; std::vector<inner> parts; std::map<std::string, double> stats; };
FORGE_REFLECT(save, level, name, hard, m, parts, stats)
TEST(reflect_round_trip) {
    save s{42, "forge", true, mode::b, {{1.5f, {1, 2}}, {-2.f, {}}}, {{"hp", 9.5}}};
    auto j = reflect::to_json(s);
    auto txt = json::dump(j);
    auto r = reflect::from_json<save>(json::parse(txt));
    CHECK(r.level == 42 && r.name == "forge" && r.hard && r.m == mode::b && r.parts.size() == 2 && r.parts[0].ids[1] == 2 && r.parts[1].x == -2.f && r.stats["hp"] == 9.5);
    auto partial = reflect::from_json<save>(json::parse(R"({"name":"x"})"));
    CHECK(partial.level == 1 && partial.name == "x");      // missing fields keep defaults
}
TEST(audio_resample_biquad) {
    const long sr = 48000; std::vector<float> x(4800);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = float(std::sin(2 * M_PI * 440.0 * double(i) / sr));
    auto y = signal::resample(x, sr, 16000);
    CHECK(y.size() == 1600);
    double err = 0; for (std::size_t i = 100; i < 1500; ++i) err = std::max(err, std::fabs(y[i] - std::sin(2 * M_PI * 440.0 * double(i) / 16000)));
    CHECK_MSG(err < 2e-3, "resample err %g", err);
    auto up = signal::resample(y, 16000, 44100);
    CHECK(up.size() == 4410);
    auto lp = signal::biquad::make(signal::biquad::lowpass, sr, 1000);
    CHECK(std::fabs(lp.gain(sr, 1) - 1) < 1e-4); CHECK(std::fabs(lp.gain(sr, 1000) - std::sqrt(0.5)) < 1e-6); CHECK(lp.gain(sr, 10000) < 0.02);
    std::vector<float> hi(4800); for (std::size_t i = 0; i < hi.size(); ++i) hi[i] = float(std::sin(2 * M_PI * 12000.0 * double(i) / sr));
    lp.process(hi.data(), hi.size());
    double pk = 0; for (std::size_t i = 2000; i < hi.size(); ++i) pk = std::max(pk, double(std::fabs(hi[i])));
    CHECK(pk < 0.02);
    auto nt = signal::biquad::make(signal::biquad::notch, sr, 3000, 5); CHECK(nt.gain(sr, 3000) < 1e-9);
}
TEST(vision_roi_align) {
    vision::image im{8, 8, 1, std::vector<float>(64)};
    for (std::size_t y = 0; y < 8; ++y) for (std::size_t x = 0; x < 8; ++x) im.at(y, x, 0) = float(2 * y + 3 * x);   // linear: bilinear is exact
    auto o = vision::roi_align(im, {1.f, 2.f, 5.f, 6.f}, 2, 2, 2);
    // bin (i,j) centre: y = 2 + 2i + 1, x = 1 + 2j + 1 (box is x0=1,y0=2,x1=5,y1=6)
    for (std::size_t i = 0; i < 2; ++i) for (std::size_t j = 0; j < 2; ++j)
        CHECK(std::fabs(o.at(i, j, 0) - (2.f * (3.f + 2.f * float(i)) + 3.f * (2.f + 2.f * float(j)))) < 1e-5);
}
