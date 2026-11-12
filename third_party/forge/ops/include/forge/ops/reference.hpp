#pragma once
// Scalar reference implementations — the oracle. Clarity over speed; computed in double where it matters.
#include <forge/core/fma.hpp>
#include <forge/ops/op.hpp>
#include <cmath>
#include <limits>
#include <numbers>

namespace forge::ops::ref {

using tv = std::vector<tensor>;

namespace detail {
inline std::vector<i64> strides(const shape_t& s) {
    std::vector<i64> st(s.size(), 1);
    for (int i = int(s.size()) - 2; i >= 0; --i) st[std::size_t(i)] = st[std::size_t(i) + 1] * s[std::size_t(i) + 1];
    return st;
}
// index into a (possibly lower-rank, broadcast) input for flat output index
inline i64 bindex(i64 flat, const shape_t& out, const shape_t& in) {
    i64 idx = 0, mul = 1;
    for (int oi = int(out.size()) - 1, ii = int(in.size()) - 1; oi >= 0; --oi, --ii) {
        i64 c = flat % out[std::size_t(oi)]; flat /= out[std::size_t(oi)];
        if (ii >= 0) { if (in[std::size_t(ii)] != 1) idx += c * mul; mul *= in[std::size_t(ii)]; }
    }
    return idx;
}
template <class F> tensor map_n(const tv& in, F f) {
    std::vector<shape_t> ss; for (auto& t : in) ss.push_back(t.shape);
    tensor o(broadcast_shapes(ss));
    for (i64 i = 0; i < i64(o.size()); ++i) {
        double a[3] = {0, 0, 0};
        for (std::size_t j = 0; j < in.size() && j < 3; ++j) a[j] = in[j].data[std::size_t(bindex(i, o.shape, in[j].shape))];
        o.data[std::size_t(i)] = float(f(a[0], a[1], a[2]));
    }
    return o;
}
template <class F> tensor map1(const tv& in, F f) {
    tensor o(in.at(0).shape);
    for (std::size_t i = 0; i < o.size(); ++i) o.data[i] = float(f(double(in[0].data[i])));
    return o;
}
// iterate over (outer, axis, inner) decomposition
struct axis_view { i64 outer, n, inner; };
inline axis_view split_axis(const shape_t& s, i64 ax) {
    axis_view v{1, s[std::size_t(ax)], 1};
    for (i64 i = 0; i < ax; ++i) v.outer *= s[std::size_t(i)];
    for (std::size_t i = std::size_t(ax) + 1; i < s.size(); ++i) v.inner *= s[i];
    return v;
}
template <class F> tensor reduce(const tv& in, const attrs& a, F f) {
    const auto& x = in.at(0);
    i64 ax = norm_axis(i64(a.num("axis", -1)), x.shape.size());
    auto v = split_axis(x.shape, ax);
    tensor o(infer_shape(shape_rule::reduce, {x.shape}, a));
    std::vector<double> buf(std::size_t(v.n));
    for (i64 o_ = 0; o_ < v.outer; ++o_)
        for (i64 in_ = 0; in_ < v.inner; ++in_) {
            for (i64 k = 0; k < v.n; ++k) buf[std::size_t(k)] = x.data[std::size_t((o_ * v.n + k) * v.inner + in_)];
            o.data[std::size_t(o_ * v.inner + in_)] = float(f(buf));
        }
    return o;
}
inline double sigm(double x) { return 1.0 / (1.0 + std::exp(-x)); }
} // namespace detail

#define FORGE_REF_BIN(name, expr) inline tensor name(const tv& in, const attrs&) { \
    return detail::map_n(in, [](double a, double b, double c) { (void)a; (void)b; (void)c; return (expr); }); }
#define FORGE_REF_UN(name, expr) inline tensor name(const tv& in, const attrs&) { \
    return detail::map1(in, [](double x) { return (expr); }); }

FORGE_REF_BIN(add, a + b)
FORGE_REF_BIN(sub, a - b)
FORGE_REF_BIN(mul, a * b)
FORGE_REF_BIN(div, a / b)
FORGE_REF_BIN(maximum, std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::max(a, b))
FORGE_REF_BIN(minimum, std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::min(a, b))
FORGE_REF_BIN(pow, std::pow(a, b))
FORGE_REF_BIN(fma, double(forge::fmaf_exact(float(a), float(b), float(c))))
FORGE_REF_BIN(select, c != 0 ? a : b)
FORGE_REF_BIN(silu_mul, a * detail::sigm(a) * b)
FORGE_REF_UN(neg, -x)
FORGE_REF_UN(abs, std::fabs(x))
FORGE_REF_UN(sqrt, std::sqrt(x))
FORGE_REF_UN(rsqrt, 1.0 / std::sqrt(x))
FORGE_REF_UN(recip, 1.0 / x)
FORGE_REF_UN(square, x * x)
FORGE_REF_UN(exp, std::exp(x))
FORGE_REF_UN(exp2, std::exp2(x))
FORGE_REF_UN(expm1, std::expm1(x))
FORGE_REF_UN(log, std::log(x))
FORGE_REF_UN(log2, std::log2(x))
FORGE_REF_UN(log1p, std::log1p(x))
FORGE_REF_UN(sin, std::sin(x))
FORGE_REF_UN(cos, std::cos(x))
FORGE_REF_UN(tan, std::tan(x))
FORGE_REF_UN(tanh, std::tanh(x))
FORGE_REF_UN(sinh, std::sinh(x))
FORGE_REF_UN(cosh, std::cosh(x))
FORGE_REF_UN(asin, std::asin(x))
FORGE_REF_UN(acos, std::acos(x))
FORGE_REF_UN(atan, std::atan(x))
FORGE_REF_UN(erf, std::erf(x))
FORGE_REF_UN(erfc, std::erfc(x))
FORGE_REF_UN(sigmoid, detail::sigm(x))
FORGE_REF_UN(silu, x * detail::sigm(x))
FORGE_REF_UN(gelu, 0.5 * x * (1.0 + std::erf(x / std::numbers::sqrt2)))
FORGE_REF_UN(gelu_tanh, 0.5 * x * (1.0 + std::tanh(std::sqrt(2.0 / std::numbers::pi) * (x + 0.044715 * x * x * x))))
FORGE_REF_UN(relu, x > 0 ? x : 0.0)
FORGE_REF_UN(softplus, x > 20 ? x : std::log1p(std::exp(x)))
#undef FORGE_REF_BIN
#undef FORGE_REF_UN

inline tensor scale(const tv& in, const attrs& a) {
    double al = a.num("alpha", 1.0);
    return detail::map1(in, [al](double x) { return float(al) * float(x); });
}
inline tensor convert(const tv& in, const attrs& a) {
    auto to = dtype_from_name(a.str("to", "f32"));
    return detail::map1(in, [to](double xd) -> double {
        float x = float(xd);
        switch (to) {
            case dtype::f16: return float(f16(x));
            case dtype::bf16: return float(bf16(x));
            case dtype::fp8_e4m3: return float(fp8_e4m3(x));
            case dtype::fp8_e5m2: return float(fp8_e5m2(x));
            default: return x;
        }
    });
}

inline tensor reduce_sum(const tv& in, const attrs& a) {
    return detail::reduce(in, a, [](auto& b) { double s = 0; for (double x : b) s += x; return s; });
}
inline tensor reduce_max(const tv& in, const attrs& a) {
    return detail::reduce(in, a, [](auto& b) { double m = -INFINITY; for (double x : b) m = std::max(m, x); return m; });
}
inline tensor reduce_min(const tv& in, const attrs& a) {
    return detail::reduce(in, a, [](auto& b) { double m = INFINITY; for (double x : b) m = std::min(m, x); return m; });
}
inline tensor reduce_mean(const tv& in, const attrs& a) {
    return detail::reduce(in, a, [](auto& b) { double s = 0; for (double x : b) s += x; return s / double(b.size()); });
}
inline tensor argmax(const tv& in, const attrs& a) {
    return detail::reduce(in, a, [](auto& b) { std::size_t m = 0; for (std::size_t i = 1; i < b.size(); ++i) if (b[i] > b[m]) m = i; return double(m); });
}
inline tensor logsumexp(const tv& in, const attrs& a) {
    return detail::reduce(in, a, [](auto& b) {
        double m = -INFINITY; for (double x : b) m = std::max(m, x);
        if (std::isinf(m)) return m;
        double s = 0; for (double x : b) s += std::exp(x - m); return m + std::log(s); });
}

template <class F> tensor last_axis_rows(const tensor& x, F f) {
    tensor o(x.shape);
    i64 n = x.shape.empty() ? 1 : x.shape.back();
    for (i64 r = 0; r < i64(x.size()) / n; ++r) f(&x.data[std::size_t(r * n)], &o.data[std::size_t(r * n)], n);
    return o;
}
inline tensor softmax(const tv& in, const attrs&) {
    return last_axis_rows(in.at(0), [](const float* x, float* y, i64 n) {
        double m = -INFINITY; for (i64 i = 0; i < n; ++i) m = std::max(m, double(x[i]));
        double s = 0; for (i64 i = 0; i < n; ++i) s += std::exp(double(x[i]) - m);
        for (i64 i = 0; i < n; ++i) y[i] = float(std::exp(double(x[i]) - m) / s);
    });
}
inline tensor rms_norm(const tv& in, const attrs& a) {
    double eps = a.num("eps", 1e-6); const auto& w = in.at(1);
    return last_axis_rows(in.at(0), [&](const float* x, float* y, i64 n) {
        double s = 0; for (i64 i = 0; i < n; ++i) s += double(x[i]) * x[i];
        double r = 1.0 / std::sqrt(s / double(n) + eps);
        for (i64 i = 0; i < n; ++i) y[i] = float(double(x[i]) * r * w.data[std::size_t(i)]);
    });
}
inline tensor layer_norm(const tv& in, const attrs& a) {
    double eps = a.num("eps", 1e-5); const auto& w = in.at(1); const auto& b = in.at(2);
    return last_axis_rows(in.at(0), [&](const float* x, float* y, i64 n) {
        double m = 0; for (i64 i = 0; i < n; ++i) m += x[i]; m /= double(n);
        double v = 0; for (i64 i = 0; i < n; ++i) v += (x[i] - m) * (x[i] - m); v /= double(n);
        double r = 1.0 / std::sqrt(v + eps);
        for (i64 i = 0; i < n; ++i) y[i] = float((x[i] - m) * r * w.data[std::size_t(i)] + b.data[std::size_t(i)]);
    });
}
// Mixture-of-experts router: softmax over experts, keep the top-k per row, optionally renormalise.
// Ties break toward the lower expert index. Output has the same shape; dropped experts are 0.
inline tensor moe_gate(const tv& in, const attrs& a) {
    i64 k = i64(a.num("k", 2)); bool renorm = a.num("norm", 1) != 0;
    return last_axis_rows(in.at(0), [&](const float* x, float* y, i64 n) {
        double m = -INFINITY; for (i64 i = 0; i < n; ++i) m = std::max(m, double(x[i]));
        std::vector<double> p(static_cast<std::size_t>(n)); double s = 0;
        for (i64 i = 0; i < n; ++i) { p[std::size_t(i)] = std::exp(double(x[i]) - m); s += p[std::size_t(i)]; }
        std::vector<i64> idx(static_cast<std::size_t>(n)); for (i64 i = 0; i < n; ++i) idx[std::size_t(i)] = i;
        i64 kk = std::min(k, n);
        std::partial_sort(idx.begin(), idx.begin() + kk, idx.end(), [&](i64 a1, i64 b1) {
            return x[a1] != x[b1] ? x[a1] > x[b1] : a1 < b1; });
        double ks = 0; for (i64 j = 0; j < kk; ++j) ks += p[std::size_t(idx[std::size_t(j)])];
        for (i64 i = 0; i < n; ++i) y[i] = 0;
        for (i64 j = 0; j < kk; ++j) { auto e = idx[std::size_t(j)]; y[e] = float(p[std::size_t(e)] / (renorm ? ks : s)); }
    });
}
inline tensor cumsum(const tv& in, const attrs& a) {
    const auto& x = in.at(0);
    auto v = detail::split_axis(x.shape, norm_axis(i64(a.num("axis", -1)), x.shape.size()));
    tensor o(x.shape);
    for (i64 p = 0; p < v.outer; ++p) for (i64 q = 0; q < v.inner; ++q) {
        double s = 0;
        for (i64 k = 0; k < v.n; ++k) { auto i = std::size_t((p * v.n + k) * v.inner + q); s += x.data[i]; o.data[i] = float(s); }
    }
    return o;
}

inline tensor matmul_impl(const tv& in, bool trans) {
    const auto& x = in.at(0); const auto& w = in.at(1);
    shape_t os = infer_shape(trans ? shape_rule::matmul_t : shape_rule::matmul, {x.shape, w.shape}, {});
    tensor o(os);
    i64 k = x.shape.back(), m = x.shape[x.shape.size() - 2];
    i64 n = os.back(), batch = i64(x.size()) / (m * k);
    bool wb = w.shape.size() > 2;
    for (i64 b = 0; b < batch; ++b) {
        const float* W = w.data.data() + (wb ? b * k * n : 0);
        for (i64 i = 0; i < m; ++i) for (i64 j = 0; j < n; ++j) {
            double s = 0;
            for (i64 p = 0; p < k; ++p)
                s += double(x.data[std::size_t((b * m + i) * k + p)]) * (trans ? W[j * k + p] : W[p * n + j]);
            o.data[std::size_t((b * m + i) * n + j)] = float(s);
        }
    }
    return o;
}
inline tensor matmul(const tv& in, const attrs&) { return matmul_impl(in, false); }
inline tensor matmul_t(const tv& in, const attrs&) { return matmul_impl(in, true); }
// Sparse MoE routing for one token (shared by the reference, the CPU lowering and the device tests): scores are
// softmax (gating 0) or sigmoid (gating 1) of the router logits; the k largest are kept (ties -> lower index; softmax
// and sigmoid are monotone, so the choice is made on the logits); weights = score / (sum of the k scores if norm,
// else 1 for sigmoid / the softmax total), times scale. ids/w get k entries in descending-score order.
inline void moe_route(const float* x, i64 n, i64 k, bool norm, double scale, int gating, i64* ids, float* w) {
    k = std::min(k, n);
    std::vector<i64> idx(static_cast<std::size_t>(n)); for (i64 i = 0; i < n; ++i) idx[std::size_t(i)] = i;
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(), [&](i64 a1, i64 b1) { return x[a1] != x[b1] ? x[a1] > x[b1] : a1 < b1; });
    double m = -INFINITY; for (i64 i = 0; i < n; ++i) m = std::max(m, double(x[i]));
    double tot = 0; if (gating == 0) for (i64 i = 0; i < n; ++i) tot += std::exp(double(x[i]) - m);
    auto score = [&](i64 e) { return gating == 0 ? std::exp(double(x[e]) - m) : 1.0 / (1.0 + std::exp(-double(x[e]))); };
    double ks = 0; for (i64 j = 0; j < k; ++j) ks += score(idx[std::size_t(j)]);
    double den = norm ? ks : (gating == 0 ? tot : 1.0);
    for (i64 j = 0; j < k; ++j) { ids[j] = idx[std::size_t(j)]; w[j] = float(score(idx[std::size_t(j)]) / den * scale); }
}
// Sparse mixture-of-experts SwiGLU FFN. in: h [t, d], router logits [t, e], gate [e, f, d], up [e, f, d], down [e, d, f].
// Only the k routed experts of each token are evaluated: y = sum_j w_j * down_j(silu(gate_j h) * up_j h).
inline tensor moe_ffn(const tv& in, const attrs& a) {
    const auto& h = in.at(0); const auto& lg = in.at(1); const auto& wg = in.at(2); const auto& wu = in.at(3); const auto& wd = in.at(4);
    if (h.shape.size() != 2 || lg.shape.size() != 2 || wg.shape.size() != 3 || wu.shape != wg.shape || wd.shape.size() != 3)
        raise(errc::shape_mismatch, "moe_ffn: h [t,d], logits [t,e], gate/up [e,f,d], down [e,d,f]");
    i64 t = h.shape[0], d = h.shape[1], ne = wg.shape[0], f = wg.shape[1];
    if (lg.shape[0] != t || lg.shape[1] != ne || wg.shape[2] != d || wd.shape != shape_t{ne, d, f})
        raise(errc::shape_mismatch, "moe_ffn: inconsistent shapes");
    i64 k = std::min(i64(a.num("k", 2)), ne);
    tensor o(h.shape);
    std::vector<i64> ids(static_cast<std::size_t>(k)); std::vector<float> w(static_cast<std::size_t>(k)); std::vector<double> hid(static_cast<std::size_t>(f));
    for (i64 r = 0; r < t; ++r) {
        moe_route(lg.data.data() + r * ne, ne, k, a.num("norm", 1) != 0, a.num("scale", 1.0), int(a.num("gating", 0)), ids.data(), w.data());
        const float* x = h.data.data() + r * d;
        std::vector<double> acc(std::size_t(d), 0.0);
        for (i64 j = 0; j < k; ++j) {
            i64 e = ids[std::size_t(j)];
            const float* G = wg.data.data() + e * f * d; const float* U = wu.data.data() + e * f * d; const float* D = wd.data.data() + e * d * f;
            for (i64 c = 0; c < f; ++c) {
                double g = 0, u = 0;
                for (i64 p = 0; p < d; ++p) { g += double(x[p]) * G[c * d + p]; u += double(x[p]) * U[c * d + p]; }
                hid[std::size_t(c)] = g / (1.0 + std::exp(-g)) * u;
            }
            for (i64 c = 0; c < d; ++c) { double s = 0; for (i64 p = 0; p < f; ++p) s += hid[std::size_t(p)] * D[c * f + p]; acc[std::size_t(c)] += double(w[std::size_t(j)]) * s; }
        }
        for (i64 c = 0; c < d; ++c) o.data[std::size_t(r * d + c)] = float(acc[std::size_t(c)]);
    }
    return o;
}

// q[s,hq,d], k/v[t,hk,d]. causal: query i (absolute position t - s + i) sees keys <= its position.
inline tensor attention(const tv& in, const attrs& a) {
    const auto& q = in.at(0); const auto& k = in.at(1); const auto& v = in.at(2);
    infer_shape(shape_rule::attention, {q.shape, k.shape, v.shape}, a);
    i64 s = q.shape[0], hq = q.shape[1], d = q.shape[2], t = k.shape[0], hk = k.shape[1];
    i64 grp = hq / hk;
    bool causal = a.num("causal", 1) != 0;
    double sc = a.num("scale", 1.0 / std::sqrt(double(d)));
    i64 window = i64(a.num("window", 0));
    i64 dv = v.shape[2];
    tensor o(shape_t{s, hq, dv});
    std::vector<double> sco(static_cast<std::size_t>(t));
    for (i64 i = 0; i < s; ++i) {
        i64 pos = t - s + i;
        for (i64 h = 0; h < hq; ++h) {
            i64 kh = h / grp; double mx = -INFINITY;
            for (i64 j = 0; j < t; ++j) {
                bool ok = !causal || j <= pos;
                if (window > 0 && pos - j >= window) ok = false;
                if (!ok) { sco[std::size_t(j)] = -INFINITY; continue; }
                double dot = 0;
                for (i64 e = 0; e < d; ++e) dot += double(q.data[std::size_t((i * hq + h) * d + e)]) * k.data[std::size_t((j * hk + kh) * d + e)];
                sco[std::size_t(j)] = dot * sc; mx = std::max(mx, sco[std::size_t(j)]);
            }
            double sum = 0; for (auto& x : sco) { x = std::isinf(x) ? 0 : std::exp(x - mx); sum += x; }
            for (i64 e = 0; e < dv; ++e) {
                double acc = 0;
                for (i64 j = 0; j < t; ++j) acc += sco[std::size_t(j)] * v.data[std::size_t((j * hk + kh) * dv + e)];
                o.data[std::size_t((i * hq + h) * dv + e)] = float(acc / sum);
            }
        }
    }
    return o;
}

inline tensor transpose(const tv& in, const attrs& a) {
    const auto& x = in.at(0);
    auto p = a.ints("perm");
    if (p.empty()) for (i64 i = i64(x.shape.size()) - 1; i >= 0; --i) p.push_back(i);
    tensor o(infer_shape(shape_rule::transpose, {x.shape}, a));
    auto ist = detail::strides(x.shape);
    for (i64 f = 0; f < i64(o.size()); ++f) {
        i64 rem = f, src = 0;
        for (int d = int(o.shape.size()) - 1; d >= 0; --d) {
            i64 c = rem % o.shape[std::size_t(d)]; rem /= o.shape[std::size_t(d)];
            src += c * ist[std::size_t(norm_axis(p[std::size_t(d)], x.shape.size()))];
        }
        o.data[std::size_t(f)] = x.data[std::size_t(src)];
    }
    return o;
}
inline tensor reshape(const tv& in, const attrs& a) {
    tensor o = in.at(0); o.shape = infer_shape(shape_rule::reshape, {o.shape}, a); return o;
}
inline tensor gather(const tv& in, const attrs& a) {
    const auto& t = in.at(0); const auto& idx = in.at(1);
    tensor o(infer_shape(shape_rule::gather, {t.shape, idx.shape}, a));
    i64 d = t.shape[1];
    for (std::size_t i = 0; i < idx.size(); ++i) {
        i64 r = i64(idx.data[i]);
        if (r < 0 || r >= t.shape[0]) raise(errc::invalid_argument, "gather index {} out of range", r);
        std::copy_n(&t.data[std::size_t(r * d)], d, &o.data[std::size_t(i64(i) * d)]);
    }
    return o;
}
inline tensor concat(const tv& in, const attrs& a) {
    std::vector<shape_t> ss; for (auto& t : in) ss.push_back(t.shape);
    tensor o(infer_shape(shape_rule::concat, ss, a));
    i64 ax = norm_axis(i64(a.num("axis", 0)), o.shape.size());
    auto ov = detail::split_axis(o.shape, ax);
    i64 off = 0;
    for (auto& t : in) {
        auto v = detail::split_axis(t.shape, ax);
        for (i64 p = 0; p < v.outer; ++p)
            std::copy_n(&t.data[std::size_t(p * v.n * v.inner)], v.n * v.inner, &o.data[std::size_t((p * ov.n + off) * ov.inner)]);
        off += v.n;
    }
    return o;
}
inline tensor slice(const tv& in, const attrs& a) {
    const auto& x = in.at(0);
    tensor o(infer_shape(shape_rule::slice, {x.shape}, a));
    i64 ax = norm_axis(i64(a.num("axis", 0)), x.shape.size());
    i64 st = i64(a.num("start", 0)); if (st < 0) st += x.shape[std::size_t(ax)];
    auto v = detail::split_axis(x.shape, ax); i64 n = o.shape[std::size_t(ax)];
    for (i64 p = 0; p < v.outer; ++p)
        std::copy_n(&x.data[std::size_t((p * v.n + st) * v.inner)], n * v.inner, &o.data[std::size_t(p * n * v.inner)]);
    return o;
}
// x[s, h, d], pos[s]. neox=1: rotate halves (HF Llama); neox=0: interleaved pairs (GPT-J / GGUF llama).
inline tensor rope(const tv& in, const attrs& a) {
    const auto& x = in.at(0); const auto& pos = in.at(1);
    double base = a.num("base", 10000.0); bool neox = a.num("neox", 1) != 0;
    if (x.shape.size() != 3) raise(errc::shape_mismatch, "rope expects [s,h,d]");
    i64 s = x.shape[0], h = x.shape[1], d = x.shape[2], half = d / 2;
    tensor o(x.shape);
    for (i64 i = 0; i < s; ++i) for (i64 hh = 0; hh < h; ++hh) {
        const float* src = &x.data[std::size_t((i * h + hh) * d)]; float* dst = &o.data[std::size_t((i * h + hh) * d)];
        for (i64 j = 0; j < half; ++j) {
            double th = double(pos.data[std::size_t(i)]) * std::pow(base, -2.0 * double(j) / double(d));
            double c = std::cos(th), sn = std::sin(th);
            i64 i0 = neox ? j : 2 * j, i1 = neox ? j + half : 2 * j + 1;
            dst[i0] = float(src[i0] * c - src[i1] * sn);
            dst[i1] = float(src[i0] * sn + src[i1] * c);
        }
    }
    return o;
}

} // namespace forge::ops::ref

namespace forge::ops::ref {
inline tensor expand(const tv& in, const attrs& a) {
    const auto& x = in.at(0);
    auto os = infer_shape(shape_rule::expand, {x.shape}, a);
    i64 ax = norm_axis(i64(a.num("axis", 0)), x.shape.size()), n = os[std::size_t(ax)];
    i64 outer = 1, inner = 1;
    for (i64 i = 0; i < ax; ++i) outer *= x.shape[std::size_t(i)];
    for (std::size_t i = std::size_t(ax) + 1; i < x.shape.size(); ++i) inner *= x.shape[i];
    tensor o(os);
    for (i64 p = 0; p < outer; ++p) for (i64 r = 0; r < n; ++r)
        std::copy_n(&x.data[std::size_t(p * inner)], inner, &o.data[std::size_t((p * n + r) * inner)]);
    return o;
}
} // namespace forge::ops::ref
