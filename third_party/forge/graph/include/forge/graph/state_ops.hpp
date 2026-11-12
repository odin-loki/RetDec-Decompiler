#pragma once
// Fixed-size stateful ops (graph node kind state_op): kernels that read and update a per-session slot in
// place on every run, for recurrent / linear-attention layers (Gated DeltaNet in Qwen3.5 / Qwen3-Next).
//
//   causal_conv(x[s, C], w[C, K])                      state [K-1, C]   depthwise causal conv1d, no bias
//   delta_rule(q[s,Hk,dk], k[s,Hk,dk], v[s,Hv,dv],
//              g[s,Hv], beta[s,Hv])                    state [Hv, dk, dv]   gated delta rule recurrence
//
// delta_rule per step t and value head h (key head h / (Hv/Hk), i.e. repeat_interleave), with
// l2norm(x) = x / sqrt(sum x^2 + 1e-6) on q and k when attr l2norm != 0 and q scaled by `scale`
// (default 1/sqrt(dk)):
//   S *= exp(g);  delta = (v - S^T k) * beta;  S += k delta^T;  out = S^T q
// A slot whose length is 0 (fresh session, reset(), truncate(0)) starts from zeros.
#include <forge/task/pool.hpp>
#include <forge/core/error.hpp>
#include <forge/ops/ops.gen.hpp>
#include <forge/simd/seq.hpp>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

namespace forge::graph::state_ops {

inline shape_t state_shape(const std::string& fn, const std::vector<shape_t>& in) {
    if (fn == "causal_conv") return {in.at(1).at(1) - 1, in.at(1).at(0)};
    if (fn == "delta_rule") return {in.at(2).at(1), in.at(0).at(2), in.at(2).at(2)};
    raise(errc::not_found, "unknown state op '{}'", fn);
}

inline void check(bool ok, const char* what) { if (!ok) raise(errc::shape_mismatch, "{}", what); }

inline shape_t out_shape(const std::string& fn, const std::vector<shape_t>& in) {
    if (fn == "causal_conv") {
        check(in.size() == 2 && in[0].size() == 2 && in[1].size() == 2, "causal_conv expects x[s,C], w[C,K]");
        check(in[0][1] == in[1][0] && in[1][1] >= 1, "causal_conv: channel mismatch");
        return in[0];
    }
    if (fn == "delta_rule") {
        check(in.size() == 5, "delta_rule expects q, k, v, g, beta");
        auto &q = in[0], &k = in[1], &v = in[2], &g = in[3], &b = in[4];
        check(q.size() == 3 && k == q && v.size() == 3 && v[0] == q[0], "delta_rule: q/k [s,Hk,dk], v [s,Hv,dv]");
        check(v[1] % q[1] == 0, "delta_rule: value heads must be a multiple of key heads");
        check(g == shape_t{v[0], v[1]} && b == g, "delta_rule: g and beta must be [s,Hv]");
        return v;
    }
    raise(errc::not_found, "unknown state op '{}'", fn);
}

inline void causal_conv(const float* x, const float* w, float* st, float* out, i64 s, i64 C, i64 K) {
    i64 P = K - 1;
    std::vector<float> win(std::size_t((P + s) * C));     // [state; x]
    std::copy_n(st, P * C, win.data());
    std::copy_n(x, s * C, win.data() + P * C);
    task::parallel_for(std::size_t(C), 64, [&](std::size_t b, std::size_t e) {
        for (i64 t = 0; t < s; ++t)
            for (std::size_t c = b; c < e; ++c) {
                float acc = 0;
                for (i64 j = 0; j < K; ++j) acc += w[i64(c) * K + j] * win[std::size_t((t + j) * C + i64(c))];
                out[t * C + i64(c)] = acc;
            }
    });
    std::copy_n(win.data() + s * C, P * C, st);
}

// One pass over the state per token (simd::seq delta_cols): a work item is (value head, block of 32 value
// columns); the block's [dk][32] slice of the state stays in L1 while the item walks all s tokens, and the
// decay, k-dot, update and q-dot of a column all happen in that one visit. q and k are normalised once per
// (token, key head) up front. Columns are independent, so the result does not depend on the split, and the
// tokens of one call give the same bits as the same tokens fed one call at a time.
inline void delta_rule_cols(const float* q, const float* k, const float* v, const float* g, const float* beta, float* S,
                            float* out, i64 s, i64 Hk, i64 dk, i64 Hv, i64 dv, bool l2, float scale) {
    const auto& K = simd::seq_active();
    const std::size_t rows = std::size_t(s * Hk), n = std::size_t(dk);
    std::vector<float> qn(rows * n), kn(rows * n);
    task::parallel_for(rows, 16, [&](std::size_t b, std::size_t e) {
        for (std::size_t r = b; r < e; ++r) {
            K.l2norm_scale(q + r * n, qn.data() + r * n, n, scale, l2);
            K.l2norm_scale(k + r * n, kn.data() + r * n, n, 1.f, l2);
        }
    });
    constexpr i64 cb = 32;
    const i64 rep = Hv / Hk, nbk = (dv + cb - 1) / cb;
    task::parallel_for(std::size_t(Hv * nbk), 1, [&](std::size_t b, std::size_t e) {
        for (std::size_t it = b; it < e; ++it) {
            i64 h = i64(it) / nbk, j0 = (i64(it) % nbk) * cb, kh = h / rep;
            K.delta_cols(qn.data() + kh * dk, kn.data() + kh * dk, std::size_t(Hk * dk), v + h * dv, std::size_t(Hv * dv),
                         g + h, beta + h, std::size_t(Hv), S + h * dk * dv, out + h * dv, std::size_t(Hv * dv),
                         std::size_t(s), std::size_t(dk), std::size_t(dv), std::size_t(j0), std::size_t(std::min(cb, dv - j0)));
        }
    });
}

// FORGE_CPU_DELTA=old: the previous two-pass kernel, parallel over value heads only.
inline bool delta_rule_old() {
    static const bool old = [] { const char* e = std::getenv("FORGE_CPU_DELTA"); return e && std::string(e) == "old"; }();
    return old;
}

inline void delta_rule(const float* q, const float* k, const float* v, const float* g, const float* beta, float* S,
                       float* out, i64 s, i64 Hk, i64 dk, i64 Hv, i64 dv, bool l2, float scale) {
    if (!delta_rule_old()) return delta_rule_cols(q, k, v, g, beta, S, out, s, Hk, dk, Hv, dv, l2, scale);
    i64 rep = Hv / Hk;
    task::parallel_for(std::size_t(Hv), 1, [&](std::size_t hb, std::size_t he) {
        std::vector<float> qn(static_cast<std::size_t>(dk)), kn(static_cast<std::size_t>(dk)), kv(static_cast<std::size_t>(dv));
        for (i64 h = i64(hb); h < i64(he); ++h) {
            float* Sh = S + h * dk * dv; i64 kh = h / rep;
            for (i64 t = 0; t < s; ++t) {
                const float* qt = q + (t * Hk + kh) * dk; const float* kt = k + (t * Hk + kh) * dk;
                const float* vt = v + (t * Hv + h) * dv;
                float qs = 0, ks = 0;
                for (i64 i = 0; i < dk; ++i) { qs += qt[i] * qt[i]; ks += kt[i] * kt[i]; }
                float qf = l2 ? scale / std::sqrt(qs + 1e-6f) : scale, kf = l2 ? 1.f / std::sqrt(ks + 1e-6f) : 1.f;
                for (i64 i = 0; i < dk; ++i) { qn[std::size_t(i)] = qt[i] * qf; kn[std::size_t(i)] = kt[i] * kf; }
                float decay = std::exp(g[t * Hv + h]), bt = beta[t * Hv + h];
                std::fill(kv.begin(), kv.end(), 0.f);
                for (i64 i = 0; i < dk; ++i) {
                    float* r = Sh + i * dv; float ki = kn[std::size_t(i)];
                    for (i64 j = 0; j < dv; ++j) { r[j] *= decay; kv[std::size_t(j)] += r[j] * ki; }
                }
                for (i64 j = 0; j < dv; ++j) kv[std::size_t(j)] = (vt[j] - kv[std::size_t(j)]) * bt;   // delta
                float* o = out + (t * Hv + h) * dv;
                std::fill(o, o + dv, 0.f);
                for (i64 i = 0; i < dk; ++i) {
                    float* r = Sh + i * dv; float ki = kn[std::size_t(i)], qi = qn[std::size_t(i)];
                    for (i64 j = 0; j < dv; ++j) { r[j] += ki * kv[std::size_t(j)]; o[j] += r[j] * qi; }
                }
            }
        }
    });
}

} // namespace forge::graph::state_ops

namespace forge::graph {
inline shape_t state_op_shape(const std::string& fn, const std::vector<shape_t>& in, const ops::attrs&) {
    return state_ops::out_shape(fn, in);
}
} // namespace forge::graph
