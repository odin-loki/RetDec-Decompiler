// KV-cache quantisation (int8 / fp8 e4m3) and host offload, checked against the f32 KV path on CPU and Vulkan.
#include "check.hpp"
#include "tiny_model.hpp"
#include <forge/inference/inference.hpp>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <random>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace forge;
using namespace forge::inference;
using forge::graph::kv_codec;

static std::string tmp(const std::string& n) { return (std::filesystem::temp_directory_path() / ("forge_kvq_" + std::to_string(::getpid()) + "_" + n)).string(); }
static const std::string& model_a() { static std::string p = forge_test::make_tiny_model(tmp("a.fpk")); return p; }
static float maxabs(const std::vector<float>& a) { float m = 0; for (float x : a) m = std::max(m, std::abs(x)); return m; }
static float maxdiff(const std::vector<float>& a, const std::vector<float>& b) { float m = 0; for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i])); return m; }

// logits for a prompt then `steps` single-token decode steps (fixed tokens, so paths are comparable)
static std::vector<std::vector<float>> trace(inference::session& s, int steps) {
    std::vector<int> p = {1, 5, 9, 33, 70, 101, 7, 12, 3};
    std::vector<std::vector<float>> out; s.reset();
    out.push_back(s.forward(p));
    for (int i = 0; i < steps; ++i) { int t = (i * 37 + 11) % 100 + 1; out.push_back(s.forward(std::span<const int>(&t, 1))); }
    return out;
}

TEST(codec_roundtrip_bounds) {
    std::mt19937 g(3); std::normal_distribution<float> nd(0, 2);
    for (i64 d : {1, 3, 4, 7, 16, 64, 129}) {
        std::vector<float> x(static_cast<std::size_t>(d)), y(static_cast<std::size_t>(d)), buf(static_cast<std::size_t>(graph::kv_head_words(d)));
        for (auto& v : x) v = nd(g);
        x[0] = 9.5f;   // amax
        for (auto c : {kv_codec::int8, kv_codec::fp8}) {
            graph::kv_encode_head(x.data(), d, c, buf.data());
            graph::kv_decode_head(buf.data(), d, c, y.data());
            for (i64 e = 0; e < d; ++e) {
                float err = std::abs(x[e] - y[e]);
                float bound = c == kv_codec::int8 ? 9.5f / 254.f * 1.0001f : std::abs(x[e]) / 16.f + 9.5f / 448.f * std::ldexp(1.f, -9);
                CHECK_MSG(err <= bound + 1e-7f, "codec %d d %lld e %lld x %g y %g", int(c), (long long)d, (long long)e, x[e], y[e]);
            }
            CHECK(std::abs(y[0] - 9.5f) < 1e-5f);   // the amax element is exact up to scale rounding
        }
    }
    std::vector<float> z(8, 0.f), b(3), o(8, 1.f);
    graph::kv_encode_head(z.data(), 8, kv_codec::fp8, b.data()); graph::kv_decode_head(b.data(), 8, kv_codec::fp8, o.data());
    for (float v : o) CHECK(v == 0.f);
    // the fp8 decode formula matches core's E4M3 decoder on every non-NaN code
    for (std::uint32_t c = 0; c < 256; ++c) if ((c & 0x7f) != 0x7f) CHECK(graph::kv_fp8_decode(c) == float(fp8_e4m3::from_bits(std::uint8_t(c))));
}

static void accuracy_vs_f32(const std::string& dev) {
    auto mf = model::open(model_a()), m8 = model::open(model_a()), mq = model::open(model_a());
    if (!dev.empty()) { mf->set_device(dev); m8->set_device(dev); mq->set_device(dev); }
    inference::session sf(mf), s8(m8, {.kv_codec = "int8"}), sq(mq, {.kv_codec = "fp8"});
    auto rf = trace(sf, 24), r8 = trace(s8, 24), rq = trace(sq, 24);
    float w8 = 0, wq = 0;
    for (std::size_t i = 0; i < rf.size(); ++i) {
        float sc = maxabs(rf[i]);
        w8 = std::max(w8, maxdiff(rf[i], r8[i]) / sc); wq = std::max(wq, maxdiff(rf[i], rq[i]) / sc);
    }
    std::printf("  %s: max logit error vs f32 KV: int8 %.2e, fp8 %.2e (relative to max |logit|)\n", dev.empty() ? "cpu" : dev.c_str(), w8, wq);
    CHECK(w8 > 0 && w8 < 2e-2f);
    CHECK(wq > 0 && wq < 8e-2f);
    // memory: each (row, head) is one scale word + ceil(d/4) words instead of d words
    for (auto& [k, sl] : s8.state().slots) {
        CHECK(sl.codec == kv_codec::int8);
        auto& fs = sf.state().slots.at(k);
        i64 d = fs.buf.shape.back();
        CHECK(sl.buf.shape.back() == graph::kv_head_words(d));
    }
    CHECK(s8.state().bytes() < sf.state().bytes());
    // greedy decoding agrees with f32 KV
    sampler g1, g2; std::vector<int> p = {1, 5, 9, 33};
    sf.reset(); s8.reset();
    auto a = sf.generate(p, 12, g1), b = s8.generate(p, 12, g2);
    // (random tiny model: logits are near-tied, so require only the leading tokens to agree)
    std::size_t same = 0; while (same < a.size() && a[same] == b[same]) ++same;
    std::printf("  greedy tokens identical to f32 KV: %zu/%zu\n", same, a.size());
    CHECK(same >= 3);
}

TEST(kv_quant_cpu_accuracy) { accuracy_vs_f32(""); }

TEST(kv_quant_prefix_copy_and_rewind) {
    auto m = model::open(model_a());
    inference::session a(m, {.kv_codec = "int8"}), b(m, {.kv_codec = "int8"}), c(m);
    std::vector<int> p = {4, 8, 15, 16, 23, 42};
    a.reset(); a.forward(p);
    b.copy_prefix(a, 4);
    int t = 9;
    auto lb = b.forward(std::span<const int>(&p[4], 2));
    a.rewind(4); auto la = a.forward(std::span<const int>(&p[4], 2));
    CHECK(maxdiff(la, lb) == 0.f);
    (void)t;
    CHECK_THROWS(c.copy_prefix(a, 2));
}

TEST(kv_quant_vulkan) {
#ifdef FORGE_GPU_VULKAN
    std::string why;
    if (!graph::device_registry("vulkan", &why)) { std::printf("  vulkan unavailable (%s): skipped\n", why.c_str()); return; }
    accuracy_vs_f32("vulkan");
    // Vulkan int8 KV matches CPU int8 KV (same decode, f32 accumulation differences only)
    auto mc = model::open(model_a()), mv = model::open(model_a()); mv->set_device("vulkan");
    inference::session sc(mc, {.kv_codec = "int8"}), sv(mv, {.kv_codec = "int8"});
    auto rc = trace(sc, 8), rv = trace(sv, 8);
    float w = 0; for (std::size_t i = 0; i < rc.size(); ++i) w = std::max(w, maxdiff(rc[i], rv[i]) / maxabs(rc[i]));
    std::printf("  vulkan int8 vs cpu int8: %.2e\n", w);
    CHECK(w < 1e-4f);
    // host offload: identical results, lower peak device memory
    auto mo = model::open(model_a()); mo->set_device("vulkan");
    auto rt = graph::last_vulkan_runtime();
    inference::session so(mo, {.kv_offload = 1});
    auto mn = model::open(model_a()); mn->set_device("vulkan"); auto rt2 = graph::last_vulkan_runtime();
    inference::session sn(mn, {.kv_offload = 0});
    std::vector<int> p = {1, 5, 9, 33, 70, 101, 7, 12, 3, 44, 55, 66, 77, 88, 99, 10};
    so.reset(); auto lo = so.forward(p); sn.reset(); auto ln = sn.forward(p);
    CHECK(maxdiff(lo, ln) == 0.f);
    auto po = rt->residency().peak_resident_bytes, pn = rt2->residency().peak_resident_bytes;
    std::printf("  peak device bytes: offload %zu, resident %zu\n", po, pn);
    CHECK(po < pn);
#else
    std::puts("  vulkan not built: skipped");
#endif
}

// ---- TurboQuant (arXiv 2504.19874): random-sign FWHT rotation + Lloyd-Max scalar codes ----
TEST(tbq_roundtrip_mse) {
    std::mt19937 g(11); std::normal_distribution<float> nd(0, 1);
    for (i64 d : {8, 64, 96, 128, 256}) {
        for (auto c : {kv_codec::tbq4, kv_codec::tbq3}) {
            double num = 0, den = 0;
            std::vector<float> x(static_cast<std::size_t>(d)), y(static_cast<std::size_t>(d)), buf(static_cast<std::size_t>(graph::kv_head_words(d, c)));
            for (int r = 0; r < 200; ++r) {
                for (auto& v : x) v = nd(g) * (r % 3 == 0 ? 5.f : 1.f);
                x[std::size_t(r) % std::size_t(d)] += 12.f;   // an outlier channel: the rotation spreads it
                graph::kv_encode_head(x.data(), d, c, buf.data());
                graph::kv_decode_head(buf.data(), d, c, y.data());
                for (i64 e = 0; e < d; ++e) { num += double(x[e] - y[e]) * (x[e] - y[e]); den += double(x[e]) * x[e]; }
            }
            double rel = num / den;
            std::printf("  %s d %lld: relative MSE %.4f, words/head %lld (f32 %lld)\n", c == kv_codec::tbq4 ? "tbq4" : "tbq3", (long long)d, rel,
                        (long long)graph::kv_head_words(d, c), (long long)d);
            // Lloyd-Max on N(0,1): 4-bit 0.0095, 3-bit 0.0345 (paper Thm 1); small d is a touch worse
            CHECK_MSG(rel < (c == kv_codec::tbq4 ? (d < 64 ? 0.03 : 0.013) : (d < 64 ? 0.07 : 0.045)), "rel %g", rel);
        }
    }
    std::vector<float> z(64, 0.f), b(std::size_t(graph::kv_head_words(64, kv_codec::tbq4))), o(64, 1.f);
    graph::kv_encode_head(z.data(), 64, kv_codec::tbq4, b.data()); graph::kv_decode_head(b.data(), 64, kv_codec::tbq4, o.data());
    for (float v : o) CHECK(v == 0.f);
    auto p = graph::kv_codec_pair_from_name("k8v4");
    CHECK(p.k == kv_codec::int8 && p.v == kv_codec::tbq4);
    CHECK(graph::kv_codec_pair_from_name("tbq3").k == kv_codec::tbq3);
    CHECK_THROWS(graph::kv_codec_pair_from_name("tbq9"));
}

// TurboQuant_prod: (b-1)-bit MSE codes + 1-bit QJL residual sketch. Inner products should be unbiased.
TEST(tbq_qjl_inner_product) {
    std::mt19937 g(5); std::normal_distribution<float> nd(0, 1);
    for (i64 d : {64, 128}) {
        std::vector<float> q(static_cast<std::size_t>(d)); for (auto& v : q) v = nd(g);
        for (auto c : {kv_codec::tbq4, kv_codec::tbq4p, kv_codec::tbq3, kv_codec::tbq3p}) {
            std::vector<float> k(static_cast<std::size_t>(d)), y(static_cast<std::size_t>(d)), buf(static_cast<std::size_t>(graph::kv_head_words(d, c)));
            double bias = 0, se = 0, ref2 = 0, rel = 0, xx = 0; const int n = 4000;
            for (int r = 0; r < n; ++r) {
                for (auto& v : k) v = nd(g);
                // keys aligned with q: shrinkage bias of a pure MSE quantizer shows up here
                for (i64 e = 0; e < d; ++e) k[e] += 0.5f * q[e];
                graph::kv_encode_head(k.data(), d, c, buf.data());
                graph::kv_decode_head(buf.data(), d, c, y.data());
                double a = 0, b = 0;
                for (i64 e = 0; e < d; ++e) { a += double(q[e]) * k[e]; b += double(q[e]) * y[e]; rel += double(k[e] - y[e]) * (k[e] - y[e]); xx += double(k[e]) * k[e]; }
                bias += b - a; se += (b - a) * (b - a); ref2 += a * a;
            }
            bias /= n; double rmse = std::sqrt(se / n), rms = std::sqrt(ref2 / n);
            std::printf("  %s d %lld: <q,k> bias %+.4f (%.2e of rms), rmse %.4f, vector rel MSE %.4f, words %lld\n", std::string(graph::kv_codec_name(c)).c_str(), (long long)d,
                        bias, bias / rms, rmse / rms, rel / xx, (long long)graph::kv_head_words(d, c));
            if (graph::kv_is_qjl(c)) {
                CHECK_MSG(std::fabs(bias / rms) < 6e-3,   // MSE-only: 8e-3 (tbq4), 3e-2 (tbq3); sampling noise ~1e-3 (stdlib RNGs differ)
                          "bias %g", bias / rms);
                CHECK_MSG(rel / xx < (c == kv_codec::tbq4p ? 0.06 : 0.2), "rel %g", rel / xx);
                CHECK(graph::kv_head_words(d, c) <= graph::kv_head_words(d, c == kv_codec::tbq4p ? kv_codec::tbq4 : kv_codec::tbq3) + 2);
            }
        }
    }
    CHECK(graph::kv_codec_pair_from_name("tbq4p").k == kv_codec::tbq4p);
    CHECK(graph::kv_codec_pair_from_name("tbq3_qjl").v == kv_codec::tbq3p);
}

static double kld(const std::vector<float>& p, const std::vector<float>& q) {
    auto sm = [](const std::vector<float>& l) { float m = *std::max_element(l.begin(), l.end()); std::vector<double> o(l.size()); double s = 0;
        for (std::size_t i = 0; i < l.size(); ++i) s += o[i] = std::exp(double(l[i] - m)); for (auto& v : o) v /= s; return o; };
    auto a = sm(p), b = sm(q); double k = 0;
    for (std::size_t i = 0; i < a.size(); ++i) if (a[i] > 0) k += a[i] * std::log(a[i] / std::max(b[i], 1e-300));
    return k;
}
static std::size_t argmax(const std::vector<float>& v) { return std::size_t(std::max_element(v.begin(), v.end()) - v.begin()); }

static void tbq_vs_f32(const std::string& dev) {
    for (const char* name : {"tbq4", "tbq3", "k8v4", "tbq4p", "tbq3p"}) {
        if (!dev.empty() && name[4] == 'p') continue;   // QJL types are CPU only
        auto mf = model::open(model_a()), mq = model::open(model_a());
        if (!dev.empty()) { mf->set_device(dev); mq->set_device(dev); }
        inference::session sf(mf), sq(mq, {.kv_codec = name});
        auto rf = trace(sf, 24), rq = trace(sq, 24);
        double kl = 0; std::size_t agree = 0; float w = 0;
        for (std::size_t i = 0; i < rf.size(); ++i) { kl += kld(rf[i], rq[i]); agree += argmax(rf[i]) == argmax(rq[i]); w = std::max(w, maxdiff(rf[i], rq[i]) / maxabs(rf[i])); }
        kl /= double(rf.size());
        std::printf("  %s %s: mean KLD vs f32 KV %.2e, top-1 agreement %zu/%zu, max rel logit err %.2e, KV bytes %zu vs %zu\n", dev.empty() ? "cpu" : dev.c_str(), name, kl, agree,
                    rf.size(), w, std::size_t(sq.state().bytes()), std::size_t(sf.state().bytes()));
        CHECK(w > 0);
        // (random tiny model: near-tied logits make KLD large in absolute terms; tbq4 ~1.5e-2, tbq3 ~4e-2 here)
        const bool loose = std::string(name) == "tbq3" || std::string(name) == "tbq3p";
        CHECK_MSG(kl < (loose ? 1.2e-1 : std::string(name) == "tbq4p" ? 6e-2 : 3e-2), "kld %g", kl);
        CHECK(agree * 10 >= rf.size() * (loose ? 6 : std::string(name) == "tbq4p" ? 7 : 8));
        CHECK(sq.state().bytes() < sf.state().bytes());
        bool any_v = false, any_k = false;
        for (auto& [k, sl] : sq.state().slots) {
            i64 d = sf.state().slots.at(k).buf.shape.back();
            CHECK(sl.buf.shape.back() == graph::kv_head_words(d, sl.codec));
            if (std::string(name) == "k8v4") { any_k |= sl.codec == kv_codec::int8; any_v |= sl.codec == kv_codec::tbq4; }
        }
        if (std::string(name) == "k8v4") CHECK(any_k && any_v);
    }
}

TEST(kv_tbq_cpu_accuracy) { tbq_vs_f32(""); }

TEST(kv_tbq_cuda) {
    std::string why;
    if (!graph::device_registry("cuda", &why)) { std::printf("  cuda unavailable (%s): skipped\n", why.c_str()); return; }
    tbq_vs_f32("cuda");
    for (const char* name : {"tbq4", "tbq3", "k8v4"}) {   // CUDA fused-decode attention vs the CPU decode path, same codes
        auto mc = model::open(model_a()), mg = model::open(model_a()); mg->set_device("cuda");
        inference::session sc(mc, {.kv_codec = name}), sg(mg, {.kv_codec = name});
        auto rc = trace(sc, 8), rg = trace(sg, 8);
        float w = 0; for (std::size_t i = 0; i < rc.size(); ++i) w = std::max(w, maxdiff(rc[i], rg[i]) / maxabs(rc[i]));
        std::printf("  cuda %s vs cpu %s: %.2e\n", name, name, w);
        CHECK(w < 1e-3f);
    }
}

TEST(kv_tbq_vulkan) {
#ifdef FORGE_GPU_VULKAN
    std::string why;
    if (!graph::device_registry("vulkan", &why)) { std::printf("  vulkan unavailable (%s): skipped\n", why.c_str()); return; }
    for (const char* name : {"tbq4", "tbq3", "k8v4"}) {
        auto mc = model::open(model_a()), mv = model::open(model_a()); mv->set_device("vulkan");
        inference::session sc(mc, {.kv_codec = name}), sv(mv, {.kv_codec = name});
        auto rc = trace(sc, 8), rv = trace(sv, 8);
        float w = 0; for (std::size_t i = 0; i < rc.size(); ++i) w = std::max(w, maxdiff(rc[i], rv[i]) / maxabs(rc[i]));
        std::printf("  vulkan %s vs cpu %s: %.2e\n", name, name, w);
        CHECK(w < 1e-3f);
    }
#else
    std::puts("  vulkan not built: skipped");
#endif
}
