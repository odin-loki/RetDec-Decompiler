#pragma once
// The oracle: every lowering is differential-tested against the spec's scalar reference.
#include <forge/ops/ops.gen.hpp>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>

namespace forge::verify {

inline std::int64_t ulp_distance(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) return (std::isnan(a) && std::isnan(b)) ? 0 : INT64_MAX;
    if (a == b) return 0;
    auto key = [](float f) { std::int32_t i = std::bit_cast<std::int32_t>(f); return i < 0 ? std::int64_t(INT32_MIN) - i : std::int64_t(i); };
    return std::llabs(key(a) - key(b));
}

struct compare_result {
    bool ok = true;
    double max_abs = 0, max_rel = 0;
    std::int64_t max_ulp = 0;
    std::size_t worst = 0;
    std::string message;
};

inline compare_result compare(const tensor& got, const tensor& ref, ops::tolerance tol) {
    compare_result r;
    if (got.shape != ref.shape) { r.ok = false; r.message = "shape " + shape_str(got.shape) + " vs " + shape_str(ref.shape); return r; }
    for (std::size_t i = 0; i < ref.size(); ++i) {
        float g = got.data[i], e = ref.data[i];
        if (std::isnan(e) || std::isnan(g)) { if (std::isnan(e) != std::isnan(g)) { r.ok = false; r.worst = i; } continue; }
        if (std::isinf(e) || std::isinf(g)) { if (g != e) { r.ok = false; r.worst = i; } continue; }
        double ae = std::fabs(double(g) - e), re = ae / std::max(1e-30, std::fabs(double(e)));
        auto u = ulp_distance(g, e);
        bool pass = true;
        switch (tol.kind) {
            case ops::tol_kind::exact: pass = u == 0; break;
            case ops::tol_kind::ulp: pass = u <= std::int64_t(tol.value); break;
            case ops::tol_kind::rel: pass = re <= tol.value || ae <= tol.value; break;  // abs floor near 0
            case ops::tol_kind::abs: pass = ae <= tol.value; break;
        }
        if (ae > r.max_abs) { r.max_abs = ae; if (!pass) r.worst = i; }
        r.max_rel = std::max(r.max_rel, re); r.max_ulp = std::max(r.max_ulp, u);
        if (!pass) r.ok = false;
    }
    if (!r.ok) r.message = "mismatch at " + std::to_string(r.worst) + ": got " + std::to_string(got.data[r.worst]) +
                           " ref " + std::to_string(ref.data[r.worst]);
    return r;
}

// Domain-aware random inputs for an op.
inline tensor random_tensor(shape_t s, std::mt19937& rng, float lo, float hi) {
    tensor t(std::move(s));
    std::uniform_real_distribution<float> d(lo, hi);
    for (auto& x : t.data) x = d(rng);
    return t;
}
inline std::pair<float, float> domain(std::string_view op) {
    if (op == "log" || op == "log2" || op == "sqrt" || op == "rsqrt") return {1e-3f, 100.f};
    if (op == "log1p") return {-0.9f, 50.f};
    if (op == "asin" || op == "acos") return {-0.99f, 0.99f};
    if (op == "pow") return {0.1f, 3.f};
    if (op == "exp" || op == "exp2" || op == "expm1" || op == "sinh" || op == "cosh") return {-20.f, 20.f};
    if (op == "tan") return {-1.2f, 1.2f};
    if (op == "div" || op == "recip") return {0.5f, 4.f};
    return {-6.f, 6.f};
}

// Build a valid random input set (shapes + attrs) for any spec op.
struct case_t { std::vector<tensor> in; ops::attrs a; };
inline case_t make_case(ops::op o, std::mt19937& rng, i64 n = 37) {
    const auto& inf = ops::info(o);
    auto [lo, hi] = domain(inf.name);
    case_t c;
    auto rt = [&](shape_t s) { return random_tensor(std::move(s), rng, lo, hi); };
    std::string_view nm = inf.name;
    switch (inf.shape) {
    case ops::shape_rule::same:
    case ops::shape_rule::broadcast:
        if (nm == "rms_norm") { c.in = {rt({3, n}), rt({n})}; c.a.set("eps", 1e-5); }
        else if (nm == "layer_norm") { c.in = {rt({3, n}), rt({n}), rt({n})}; c.a.set("eps", 1e-5); }
        else if (nm == "rope") {
            tensor pos({5}); for (int i = 0; i < 5; ++i) pos.data[std::size_t(i)] = float(i * 3);
            c.in = {rt({5, 2, 16}), pos}; c.a.set("base", 10000.0); c.a.set("neox", 1.0);
        } else {
            for (int i = 0; i < inf.inputs; ++i) c.in.push_back(rt({3, n}));
            if (nm == "select") for (auto& x : c.in[2].data) x = x > 0 ? 1.f : 0.f;
            if (nm == "scale") c.a.set("alpha", 0.37);
            if (nm == "convert") c.a.set("to", std::string("bf16"));
            if (nm == "cumsum") c.a.set("axis", -1.0);
            if (nm == "moe_gate") { c.in = {rt({5, 8})}; c.a.set("k", 2.0); c.a.set("norm", 1.0); }
            if (nm == "moe_ffn") {   // h [t,d], logits [t,e], gate/up [e,f,d], down [e,d,f]; small values keep the products O(1)
                i64 t = n > 16 ? 5 : 3, d = 8, f = 6, e = 4;
                c.in = {rt({t, d}), rt({t, e}), rt({e, f, d}), rt({e, f, d}), rt({e, d, f})};
                for (std::size_t i = 2; i < 5; ++i) for (auto& x : c.in[i].data) x *= 0.1f;
                c.a.set("k", 2.0); c.a.set("norm", 1.0);
            }
        }
        break;
    case ops::shape_rule::reduce: c.in = {rt({4, n})}; c.a.set("axis", -1.0); break;
    case ops::shape_rule::matmul: c.in = {rt({2, 5, n}), rt({n, 11})}; break;
    case ops::shape_rule::matmul_t: c.in = {rt({2, 5, n}), rt({11, n})}; break;
    case ops::shape_rule::attention:
        c.in = {rt({3, 4, 16}), rt({7, 2, 16}), rt({7, 2, 12})}; c.a.set("causal", 1.0); break;
    case ops::shape_rule::transpose: c.in = {rt({3, 4, 5})}; c.a.set("perm", std::vector<i64>{2, 0, 1}); break;
    case ops::shape_rule::reshape: c.in = {rt({3, 4, 5})}; c.a.set("shape", std::vector<i64>{-1, 10}); break;
    case ops::shape_rule::gather: {
        tensor idx({2, 3}); for (std::size_t i = 0; i < 6; ++i) idx.data[i] = float(rng() % 9);
        c.in = {rt({9, n}), idx}; break; }
    case ops::shape_rule::concat: c.in = {rt({3, 4}), rt({3, 6})}; c.a.set("axis", 1.0); break;
    case ops::shape_rule::expand: c.in = {rt({3, 1, 5})}; c.a.set("axis", 1.0); c.a.set("n", 4.0); break;
    case ops::shape_rule::slice: c.in = {rt({6, 5})}; c.a.set("axis", 0.0); c.a.set("start", 1.0); c.a.set("end", 4.0); break;
    }
    return c;
}

} // namespace forge::verify
