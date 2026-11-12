#include "check.hpp"
#include <forge/simd/dispatch.hpp>
#include <forge/simd/caps.hpp>
#include <forge/verify/oracle.hpp>
#include <cmath>
#include <forge/simd/math.hpp>
#include <random>

using namespace forge;
using namespace forge::simd;

// ---- concept subsumption: most constrained overload wins, no manual ordering ----
template <Target T> requires Has<T, avx2> constexpr int pick() { return 1; }
template <Target T> requires Has<T, avx2> && Has<T, avx512f> constexpr int pick() { return 2; }
template <Target T> constexpr int pick() { return 0; }
static_assert(pick<scalar_t>() == 0);
static_assert(pick<x86_64_v3>() == 1);
static_assert(pick<x86_64_v4>() == 2);
static_assert(pick<sapphire>() == 2);
static_assert(Has<sapphire, amx_int8> && !Has<zen4, amx_int8> && Has<zen4, avx512vnni>);
static_assert(!Has<neoverse2, avx2> && Has<neoverse2, sve2>);

static std::vector<const kernels*> tables() {
    std::vector<const kernels*> v;
    for (const char* t : {"scalar", "avx2", "avx512", "neon", "sve", "rvv"}) if (auto* k = by_name(t)) v.push_back(k);
    return v;
}

TEST(detection_reports) {
    auto& f = detect();
    std::printf("  cpu: %s\n  active: %s\n", f.summary().c_str(), active().target);
#if defined(__x86_64__)
    CHECK(f.sse2);
#elif defined(__aarch64__)
    CHECK(f.neon);
#endif
}

// strip/masks: every length 0..100, results must match scalar and never touch past the end.
TEST(strip_tails_all_lengths) {
    for (auto* k : tables()) {
        for (std::size_t n = 0; n <= 100; ++n) {
            std::vector<float> x(n + 32), y(n + 32, 7.f);
            for (std::size_t i = 0; i < n; ++i) { x[i] = float(i) * 0.5f; y[i] = 1.f; }
            k->saxpy(2.f, x.data(), y.data(), n);
            for (std::size_t i = 0; i < n; ++i) CHECK(y[i] == 1.f + float(i));
            for (std::size_t i = n; i < n + 32; ++i) CHECK_MSG(y[i] == 7.f, "%s wrote past end n=%zu", k->target, n);
            float s = k->sum(x.data(), n), e = 0; for (std::size_t i = 0; i < n; ++i) e += x[i];
            CHECK(std::fabs(s - e) <= 1e-4f * std::max(1.f, e));
        }
    }
}

// ULP harness for math functions over sampled ranges plus hard cases.
template <class F, class R>
static void math_sweep(const char* name, F kernel_of, R ref, float lo, float hi, double max_ulp, double abs_floor) {
    std::vector<float> x;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> d(lo, hi);
    for (int i = 0; i < 200000; ++i) x.push_back(d(rng));
    for (float h : {0.f, -0.f, 1e-30f, -1e-30f, 1.f, -1.f, lo, hi}) x.push_back(h);
    std::vector<float> y(x.size());
    for (auto* k : tables()) {
        kernel_of(*k)(x.data(), y.data(), x.size());
        std::int64_t worst = 0; double worst_x = 0;
        bool ok = true;
        for (std::size_t i = 0; i < x.size(); ++i) {
            float e = float(ref(double(x[i])));
            auto u = verify::ulp_distance(y[i], e);
            if (std::fabs(double(y[i]) - e) <= abs_floor) continue;
            if (u > worst) { worst = u; worst_x = x[i]; }
            if (double(u) > max_ulp) ok = false;
        }
        std::printf("  %-8s %-7s max %lld ulp (x=%g)\n", name, k->target, (long long)worst, worst_x);
        CHECK_MSG(ok, "%s on %s exceeds %g ulp", name, k->target, max_ulp);
    }
}
TEST(math_ulp_harness) {
    math_sweep("exp", [](const kernels& k) { return k.exp; }, [](double x) { return std::exp(x); }, -87.f, 88.f, 2, 0);
    math_sweep("exp-sub", [](const kernels& k) { return k.exp; }, [](double x) { return std::exp(x); }, -103.f, -87.f, 2, 1e-44);
    math_sweep("log", [](const kernels& k) { return k.log; }, [](double x) { return std::log(x); }, 1e-30f, 1e30f, 2, 0);
    math_sweep("log01", [](const kernels& k) { return k.log; }, [](double x) { return std::log(x); }, 0.5f, 2.f, 2, 1e-7);
    math_sweep("tanh", [](const kernels& k) { return k.tanh; }, [](double x) { return std::tanh(x); }, -10.f, 10.f, 4, 1e-7);
    math_sweep("sigmoid", [](const kernels& k) { return k.sigmoid; }, [](double x) { return 1 / (1 + std::exp(-x)); }, -30.f, 30.f, 4, 1e-7);
    math_sweep("silu", [](const kernels& k) { return k.silu; }, [](double x) { return x / (1 + std::exp(-x)); }, -30.f, 30.f, 4, 1e-6);
    math_sweep("gelu", [](const kernels& k) { return k.gelu; },
               [](double x) { return 0.5 * x * (1 + std::tanh(0.7978845608028654 * (x + 0.044715 * x * x * x))); }, -10.f, 10.f, 8, 1e-6);
}

// Extended libm (A.6): every function measured against the double libm on every built lowering.
static void ext_sweep(const char* name, int fn, double (*ref)(double), float lo, float hi, double max_ulp, double abs_floor, bool logspace = false) {
    std::vector<float> x;
    std::mt19937 rng(43);
    std::uniform_real_distribution<double> d(logspace ? std::log(double(lo)) : lo, logspace ? std::log(double(hi)) : hi);
    for (int i = 0; i < 100000; ++i) x.push_back(float(logspace ? std::exp(d(rng)) : d(rng)));
    x.push_back(lo); x.push_back(hi);
    std::vector<float> y(x.size());
    for (auto* k : tables()) {
        k->math1(fn, x.data(), y.data(), x.size());
        std::int64_t worst = 0; double worst_x = 0; bool ok = true;
        for (std::size_t i = 0; i < x.size(); ++i) {
            double e = ref(double(x[i]));
            auto u = verify::ulp_distance(y[i], float(e));
            if (std::fabs(double(y[i]) - e) <= abs_floor) continue;
            if (u > worst) { worst = u; worst_x = x[i]; }
            if (double(u) > max_ulp) ok = false;
        }
        std::printf("  %-9s %-7s max %lld ulp (x=%g)\n", name, k->target, (long long)worst, worst_x);
        CHECK_MSG(ok, "%s on %s exceeds %g ulp", name, k->target, max_ulp);
    }
}
static void ext_sweep2(const char* name, int fn, double (*ref)(double, double), float lo1, float hi1, float lo2, float hi2, double max_ulp, double abs_floor) {
    std::vector<float> a, b;
    std::mt19937 rng(44);
    std::uniform_real_distribution<float> d1(lo1, hi1), d2(lo2, hi2);
    for (int i = 0; i < 100000; ++i) { a.push_back(d1(rng)); b.push_back(d2(rng)); }
    std::vector<float> y(a.size());
    for (auto* k : tables()) {
        k->math2(fn, a.data(), b.data(), y.data(), a.size());
        std::int64_t worst = 0; bool ok = true;
        for (std::size_t i = 0; i < a.size(); ++i) {
            double e = ref(double(a[i]), double(b[i]));
            auto u = verify::ulp_distance(y[i], float(e));
            if (std::fabs(double(y[i]) - e) <= abs_floor) continue;
            worst = std::max(worst, std::int64_t(u));
            if (double(u) > max_ulp) ok = false;
        }
        std::printf("  %-9s %-7s max %lld ulp\n", name, k->target, (long long)worst);
        CHECK_MSG(ok, "%s on %s exceeds %g ulp", name, k->target, max_ulp);
    }
}
static double gelu_erf_ref(double x) { return 0.5 * x * std::erfc(-x / std::sqrt(2.0)); }
static double softplus_ref(double x) { return x > 30 ? x : std::log1p(std::exp(x)); }
static double rsqrt_ref(double x) { return 1 / std::sqrt(x); }
static double exp2_ref(double x) { return std::exp2(x); }
TEST(math_extended_ulp) {
    using simd::fn1;
    auto F = [](fn1 f) { return int(f); };
    ext_sweep("exp2", F(fn1::exp2), exp2_ref, -126, 127, 2, 0);
    ext_sweep("expm1", F(fn1::expm1), [](double x) { return std::expm1(x); }, -20, 80, 3, 0);
    ext_sweep("expm1-0", F(fn1::expm1), [](double x) { return std::expm1(x); }, -0.5f, 0.5f, 3, 0);
    ext_sweep("log2", F(fn1::log2), [](double x) { return std::log2(x); }, 1e-30f, 1e30f, 3, 0, true);
    ext_sweep("log10", F(fn1::log10), [](double x) { return std::log10(x); }, 1e-30f, 1e30f, 3, 0, true);
    ext_sweep("log1p", F(fn1::log1p), [](double x) { return std::log1p(x); }, -0.999f, 1e6f, 3, 0);
    ext_sweep("log1p-0", F(fn1::log1p), [](double x) { return std::log1p(x); }, -1e-3f, 1e-3f, 3, 0);
    ext_sweep("sqrt", F(fn1::sqrt), [](double x) { return std::sqrt(x); }, 0, 1e30f, 0, 0);
    ext_sweep("rsqrt", F(fn1::rsqrt), rsqrt_ref, 1e-30f, 1e30f, 1, 0, true);
    ext_sweep("cbrt", F(fn1::cbrt), [](double x) { return std::cbrt(x); }, -1e30f, 1e30f, 2, 0);
    ext_sweep("cbrt-sub", F(fn1::cbrt), [](double x) { return std::cbrt(x); }, 1e-44f, 1e-30f, 2, 0, true);
    ext_sweep("sin", F(fn1::sin), [](double x) { return std::sin(x); }, -1e4f, 1e4f, 2, 1e-7);
    ext_sweep("cos", F(fn1::cos), [](double x) { return std::cos(x); }, -1e4f, 1e4f, 2, 1e-7);
    ext_sweep("sin-0", F(fn1::sin), [](double x) { return std::sin(x); }, -3.2f, 3.2f, 2, 0);
    ext_sweep("tan", F(fn1::tan), [](double x) { return std::tan(x); }, -1e3f, 1e3f, 4, 1e-7);
    ext_sweep("asin", F(fn1::asin), [](double x) { return std::asin(x); }, -1, 1, 2, 0);
    ext_sweep("acos", F(fn1::acos), [](double x) { return std::acos(x); }, -1, 1, 2, 0);
    ext_sweep("atan", F(fn1::atan), [](double x) { return std::atan(x); }, -1e4f, 1e4f, 2, 0);
    ext_sweep("atan-0", F(fn1::atan), [](double x) { return std::atan(x); }, -3, 3, 3, 0);
    ext_sweep("sinh", F(fn1::sinh), [](double x) { return std::sinh(x); }, -88, 88, 3, 0);
    ext_sweep("cosh", F(fn1::cosh), [](double x) { return std::cosh(x); }, -88, 88, 3, 0);
    ext_sweep("erf", F(fn1::erf), [](double x) { return std::erf(x); }, -5, 5, 3, 0);
    ext_sweep("erfc", F(fn1::erfc), [](double x) { return std::erfc(x); }, -3, 10, 4, 0);
    ext_sweep("gelu_erf", F(fn1::gelu_erf), gelu_erf_ref, -10, 10, 4, 1e-30);
    ext_sweep("softplus", F(fn1::softplus), softplus_ref, -80, 80, 3, 0);
    ext_sweep("lgamma", F(fn1::lgamma), [](double x) { return std::lgamma(x); }, 1e-3f, 1e3f, 4, 1e-7);
    ext_sweep("lgamma-l", F(fn1::lgamma), [](double x) { return std::lgamma(x); }, 1e3f, 1e30f, 4, 0, true);
    ext_sweep("lgamma-n", F(fn1::lgamma), [](double x) { return std::lgamma(x); }, -20.f, -0.01f, 64, 1e-5);
    using simd::fn2;
    ext_sweep2("pow", int(fn2::pow), [](double a, double b) { return std::pow(a, b); }, 0.01f, 10, -8, 8, 4, 0);
    ext_sweep2("atan2", int(fn2::atan2), [](double a, double b) { return std::atan2(a, b); }, -10, 10, -10, 10, 3, 0);
    ext_sweep2("hypot", int(fn2::hypot), [](double a, double b) { return std::hypot(a, b); }, -1e30f, 1e30f, -1e30f, 1e30f, 2, 0);
    // special values and the fused logsumexp row
    float sv[5] = {0.f, INFINITY, -INFINITY, NAN, -0.f}, so[5];
    for (auto* k : tables()) {
        k->math1(int(fn1::sin), sv, so, 5); CHECK(so[0] == 0.f && std::isnan(so[1]) && std::isnan(so[3]));
        k->math1(int(fn1::erf), sv, so, 5); CHECK(so[0] == 0.f && so[1] == 1.f && so[2] == -1.f && std::isnan(so[3]));
        k->math1(int(fn1::cbrt), sv, so, 5); CHECK(so[0] == 0.f && so[1] == INFINITY && so[2] == -INFINITY);
        k->math1(int(fn1::lgamma), sv, so, 5); CHECK(so[1] == INFINITY);
        std::vector<float> row(77); for (std::size_t i = 0; i < row.size(); ++i) row[i] = float(i) * 0.37f - 9.f;
        double ref = 0, mx = row.back(); for (float v : row) ref += std::exp(double(v) - mx); ref = mx + std::log(ref);
        CHECK_MSG(std::fabs(k->logsumexp(row.data(), row.size()) - ref) < 1e-5, "logsumexp %s", k->target);
    }
}

TEST(math_special_values) {
    float in[6] = {0.f, -1.f, INFINITY, -INFINITY, NAN, 1e-40f}, out[6];
    for (auto* k : tables()) {
        k->log(in, out, 6);
        CHECK(std::isinf(out[0]) && out[0] < 0); CHECK(std::isnan(out[1])); CHECK(std::isinf(out[2]));
        CHECK(std::isnan(out[3])); CHECK(std::isnan(out[4])); CHECK(std::fabs(out[5] - std::log(1e-40f)) < 1e-4f);
        k->exp(in, out, 6);
        CHECK(out[0] == 1.f); CHECK(std::isinf(out[2])); CHECK(out[3] == 0.f);
    }
}

TEST(kernels_vs_spec_oracle) {
    std::mt19937 rng(7);
    for (auto* k : tables()) {
        for (i64 n : {1, 7, 16, 33, 100, 257}) {
            auto x = verify::random_tensor({3, n}, rng, -5, 5);
            auto w = verify::random_tensor({n}, rng, -2, 2);
            tensor y(x.shape);
            for (int r = 0; r < 3; ++r) k->softmax_row(&x.data[std::size_t(r * n)], &y.data[std::size_t(r * n)], std::size_t(n));
            auto ref = ops::ref::softmax({x}, {});
            auto c = verify::compare(y, ref, ops::info(ops::op::softmax).tol);
            CHECK_MSG(c.ok, "softmax %s n=%lld %s", k->target, (long long)n, c.message.c_str());
            for (int r = 0; r < 3; ++r) k->rms_norm_row(&x.data[std::size_t(r * n)], w.data.data(), &y.data[std::size_t(r * n)], std::size_t(n), 1e-5f);
            c = verify::compare(y, ops::ref::rms_norm({x, w}, {{"eps", 1e-5}}), ops::info(ops::op::rms_norm).tol);
            CHECK_MSG(c.ok, "rms_norm %s %s", k->target, c.message.c_str());
            auto b = verify::random_tensor({3, n}, rng, -5, 5);
            k->silu_mul(x.data.data(), b.data.data(), y.data.data(), y.size());
            c = verify::compare(y, ops::ref::silu_mul({x, b}, {}), ops::info(ops::op::silu_mul).tol);
            CHECK_MSG(c.ok, "silu_mul %s %s", k->target, c.message.c_str());
        }
    }
}

TEST(gemm_vs_oracle) {
    std::mt19937 rng(3);
    for (auto* k : tables()) {
        for (auto [m, n, kk] : {std::tuple<i64, i64, i64>{1, 1, 1}, {4, 4, 16}, {5, 7, 3}, {13, 29, 67}, {64, 48, 128}, {3, 100, 17}}) {
            auto A = verify::random_tensor({m, kk}, rng, -1, 1);
            auto Bt = verify::random_tensor({n, kk}, rng, -1, 1);
            auto B = verify::random_tensor({kk, n}, rng, -1, 1);
            tensor C({m, n});
            k->gemm_nt(A.data.data(), Bt.data.data(), C.data.data(), std::size_t(m), std::size_t(n), std::size_t(kk));
            auto c = verify::compare(C, ops::ref::matmul_t({A, Bt}, {}), ops::info(ops::op::matmul_t).tol);
            CHECK_MSG(c.ok, "gemm_nt %s %lldx%lldx%lld %s", k->target, (long long)m, (long long)n, (long long)kk, c.message.c_str());
            k->gemm_nn(A.data.data(), B.data.data(), C.data.data(), std::size_t(m), std::size_t(n), std::size_t(kk));
            c = verify::compare(C, ops::ref::matmul({A, B}, {}), ops::info(ops::op::matmul).tol);
            CHECK_MSG(c.ok, "gemm_nn %s %s", k->target, c.message.c_str());
        }
    }
}
