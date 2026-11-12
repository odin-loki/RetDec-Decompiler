#include "check.hpp"
#include <forge/gpu/prims.hpp>
#include <forge/linalg/linalg.hpp>
#include <forge/mem/arena.hpp>
#include <forge/prof/prof.hpp>
#include <forge/signal/fft.hpp>
#include <forge/vision/vision.hpp>
#include <forge/json/json.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <random>

using namespace forge;

TEST(arena_alignment_and_scope) {
    mem::arena a(256);
    for (std::size_t al : {1, 8, 64, 128}) { auto* p = a.alloc(37, al); CHECK(reinterpret_cast<std::uintptr_t>(p) % al == 0); }
    auto u = a.used(); { mem::scope s(a); a.alloc(1000); CHECK(a.used() > u); } CHECK(a.used() == u);
    a.reset(); CHECK(a.used() == 0);
    auto* q = a.make_array<int>(10); for (int i = 0; i < 10; ++i) CHECK(q[i] == 0);
}
TEST(linalg_lu_chol_qr_cg) {
    std::mt19937 g(3); std::normal_distribution<double> d; const std::size_t n = 12;
    linalg::mat<double> M(n, n); for (auto& v : M.a) v = d(g);
    linalg::mat<double> A(n, n);                              // SPD: M M^T + n I
    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) { double s = i == j ? double(n) : 0; for (std::size_t k = 0; k < n; ++k) s += M(i, k) * M(j, k); A(i, j) = s; }
    std::vector<double> x0(n), b(n); for (auto& v : x0) v = d(g);
    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) b[i] += A(i, j) * x0[j];
    auto LU = M; auto bm = b; std::vector<double> bM(n); for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) bM[i] += M(i, j) * x0[j];
    auto p = linalg::lu(LU); auto x = linalg::lu_solve(LU, p, bM); for (std::size_t i = 0; i < n; ++i) CHECK(std::fabs(x[i] - x0[i]) < 1e-9);
    auto L = linalg::cholesky(A); double e = 0;
    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) { double s = 0; for (std::size_t k = 0; k < n; ++k) s += L(i, k) * L(j, k); e = std::max(e, std::fabs(s - A(i, j))); }
    CHECK(e < 1e-10);
    linalg::mat<double> R0(9, 5); for (auto& v : R0.a) v = d(g); linalg::mat<double> Q, R; linalg::qr(R0, Q, R);
    auto QR = linalg::matmul(Q, R); e = 0; for (std::size_t i = 0; i < R0.a.size(); ++i) e = std::max(e, std::fabs(QR.a[i] - R0.a[i])); CHECK(e < 1e-12);
    for (std::size_t i = 0; i < 9; ++i) for (std::size_t j = 0; j < 5 && j < i; ++j) CHECK(std::fabs(R(i, j)) < 1e-12);
    auto QtQ = linalg::matmul(Q, Q); (void)QtQ; e = 0;
    for (std::size_t i = 0; i < 9; ++i) for (std::size_t j = 0; j < 9; ++j) { double s = 0; for (std::size_t k = 0; k < 9; ++k) s += Q(k, i) * Q(k, j); e = std::max(e, std::fabs(s - (i == j))); }
    CHECK(e < 1e-12);
    // CG on a sparse 1-D Laplacian.
    const std::size_t m = 50; linalg::mat<double> T(m, m); for (std::size_t i = 0; i < m; ++i) { T(i, i) = 2; if (i) T(i, i - 1) = -1; if (i + 1 < m) T(i, i + 1) = -1; }
    auto S = linalg::to_csr(T); CHECK(S.val.size() == 3 * m - 2);
    std::vector<double> bb(m, 1.0), xs; int it = linalg::cg(S, bb, xs, 1e-12); CHECK(it <= int(m));
    std::vector<double> r(m); linalg::spmv(S, xs.data(), r.data()); for (std::size_t i = 0; i < m; ++i) CHECK(std::fabs(r[i] - 1) < 1e-8);
    linalg::mat<double> Z(2, 2); CHECK_THROWS(linalg::lu(Z)); CHECK_THROWS(linalg::cholesky(Z));
}
TEST(fft_matches_dft_any_length) {
    std::mt19937 g(5); std::normal_distribution<double> d;
    for (std::size_t n : {1, 2, 8, 64, 3, 5, 12, 100, 257}) {
        std::vector<signal::cd> a(n); for (auto& v : a) v = {d(g), d(g)}; auto X = a; signal::fft(X);
        double e = 0, sc = 0;
        for (std::size_t k = 0; k < n; ++k) { signal::cd s = 0; for (std::size_t t = 0; t < n; ++t) s += a[t] * std::polar(1.0, -2 * M_PI * double(k * t % n) / double(n)); e = std::max(e, std::abs(s - X[k])); sc = std::max(sc, std::abs(s)); }
        CHECK_MSG(e < 1e-9 * std::max(1.0, sc), "n=%zu err %g", n, e);
        signal::fft(X, true); e = 0; for (std::size_t i = 0; i < n; ++i) e = std::max(e, std::abs(X[i] - a[i])); CHECK(e < 1e-10);
    }
}
TEST(stft_and_mel) {
    const double sr = 16000; std::vector<float> x(4000); for (std::size_t i = 0; i < x.size(); ++i) x[i] = float(std::sin(2 * M_PI * 1000.0 * double(i) / sr));
    auto S = signal::stft_power(x, 512, 160); CHECK(S.size() == 1 + x.size() / 160); CHECK(S[0].size() == 257);
    auto& f = S[S.size() / 2]; CHECK(std::max_element(f.begin(), f.end()) - f.begin() == 32);   // 1000 Hz = bin 32
    auto F = signal::mel_filters(sr, 512, 80); CHECK(F.size() == 80);
    for (auto& row : F) CHECK(*std::max_element(row.begin(), row.end()) > 0);           // no empty filters
    CHECK(std::fabs(signal::mel_to_hz(signal::hz_to_mel(3210.0)) - 3210.0) < 1e-9);
    CHECK(std::fabs(signal::hz_to_mel(1000.0) - 15.0) < 1e-12);                          // Slaney break point
}
TEST(vision_resize_colour_nms) {
    vision::image im{4, 4, 1, {}}; for (std::size_t i = 0; i < 16; ++i) im.px.push_back(float(i % 4));
    auto d = vision::resize_bilinear(im, 8, 8); CHECK(std::fabs(d.at(0, 0, 0) - 0.f) < 1e-6); CHECK(std::fabs(d.at(3, 3, 0) - 1.25f) < 1e-6);
    auto same = vision::resize_bilinear(im, 4, 4); CHECK(same.px == im.px);
    float rgb[6] = {0.2f, 0.5f, 0.9f, 1, 0, 0}, yuv[6], back[6]; vision::rgb_to_yuv(rgb, yuv, 2); vision::yuv_to_rgb(yuv, back, 2);
    for (int i = 0; i < 6; ++i) CHECK(std::fabs(back[i] - rgb[i]) < 1e-5);
    std::vector<vision::box> b = {{0, 0, 10, 10}, {1, 1, 11, 11}, {20, 20, 30, 30}, {0, 0, 10, 9}};
    auto k = vision::nms(b, {0.9f, 0.8f, 0.7f, 0.95f}, 0.5f); CHECK((k == std::vector<std::size_t>{3, 2}));
}
TEST(prof_chrome_trace) {
    auto& r = prof::recorder::get(); r.clear(); { FORGE_ZONE("off"); } CHECK(r.ev.empty());
    r.on = true; { FORGE_ZONE("outer"); { FORGE_ZONE("in\"ner"); } } r.on = false;
    CHECK(r.ev.size() == 2);
    auto j = json::parse(r.chrome_json()); CHECK(j["traceEvents"].arr().size() == 2);
    CHECK(j["traceEvents"][0]["name"].str() == "in\"ner"); CHECK(j["traceEvents"][1]["ph"].str() == "X");
}
TEST(prims_segmented_bitonic_hash) {
    std::vector<int> x = {1, 2, 3, 4, 5, 6}, o(6); std::vector<std::uint8_t> st = {1, 0, 0, 1, 0, 1};
    gpu::prims::segmented_scan<int>(x, st, o); CHECK((o == std::vector<int>{1, 3, 6, 4, 9, 6}));
    std::vector<std::uint32_t> off = {0, 3, 5, 6}; std::vector<int> r(3); gpu::prims::segmented_reduce<int>(x, off, r); CHECK((r == std::vector<int>{6, 9, 6}));
    std::mt19937 g(9); std::uniform_real_distribution<float> u(-5, 5);
    for (std::size_t n : {0, 1, 7, 64, 100}) { std::vector<float> v(n); for (auto& e : v) e = u(g); auto w = v; std::sort(w.begin(), w.end()); gpu::prims::bitonic_sort(v); CHECK(v == w); }
    gpu::prims::hash_map h; std::map<std::uint64_t, std::uint32_t> ref;
    for (int i = 0; i < 5000; ++i) { std::uint64_t k = g() % 3000; h.insert(k, std::uint32_t(i)); ref[k] = std::uint32_t(i); }
    CHECK(h.size() == ref.size()); for (auto& [k, v] : ref) { auto* p = h.find(k); CHECK(p && *p == v); } CHECK(!h.find(99999));
}
