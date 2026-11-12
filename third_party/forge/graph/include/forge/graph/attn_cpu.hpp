#pragma once
// CPU attention over a long context, flash-decoding style (simd/seq.hpp does the arithmetic):
//   * positions are cut into absolute 256-position chunks; a work item is (KV head, block of query tokens,
//     chunk) and serves every query head of the KV head's GQA group, so each K/V row is read and converted
//     once per item instead of once per query head;
//   * each item leaves per (token, head) a partial (max, sum of exponentials, weighted V); partials merge in
//     chunk order. Few query rows (decode, short verify batches): items are (head, token block, chunk) and a
//     second pass merges. Many rows (prefill): an item is (head, token block) and walks its chunks merging as
//     it goes. Both orders are the same arithmetic, so a token gets the same bits alone or inside a block.
// Keys and values may each be f32 or int8 (kv_codec int8: one f32 scale per position and head, then d bytes).
// FORGE_CPU_ATTN=old selects the previous per-(token, head) kernel.
#include <forge/graph/kv_codec.hpp>
#include <forge/simd/seq.hpp>
#include <forge/task/pool.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace forge::graph::attn_cpu {

// FORGE_CPU_ATTN: unset = this kernel for an int8 KV cache only (where it measured faster); "new" = also for
// f32 KV (its f32 prefill measured slower than the old kernel on one noisy run, not yet explained); "old" = never.
inline int mode() {
    static const int m = [] { const char* e = std::getenv("FORGE_CPU_ATTN");
        return !e ? 1 : std::string(e) == "old" ? 0 : std::string(e) == "new" ? 2 : 1; }();
    return m;
}
inline bool enabled() { return mode() != 0; }
inline bool supports(kv_codec kc, kv_codec vc) {
    const bool ok = (kc == kv_codec::f32 || kc == kv_codec::int8) && (vc == kv_codec::f32 || vc == kv_codec::int8);
    return ok && (mode() == 2 || kc == kv_codec::int8 || vc == kv_codec::int8);
}

// q [s, hq, d]; k/v hold t positions of [hk, row] (row = d / dv floats, or kv_head_words for int8);
// out [s, hq, dv]. Query token i sits at position t - s + i.
inline void attention(const float* q, std::size_t s, std::size_t hq, std::size_t d, const float* k, const float* v,
                      std::size_t t, std::size_t hk, std::size_t dv, kv_codec kc, kv_codec vc, bool causal,
                      std::size_t window, float scale, float* out) {
    const auto& K = simd::seq_active();
    constexpr std::size_t CH = simd::seq_attn_chunk;
    const std::size_t grp = hq / hk, nch = (t + CH - 1) / CH;
    const std::size_t kw = kc == kv_codec::f32 ? d : std::size_t(kv_head_words(i64(d))),
                      vw = vc == kv_codec::f32 ? dv : std::size_t(kv_head_words(i64(dv)));
    auto kv_of = [&](std::size_t kh) {
        return std::pair{simd::seq_kv{k + kh * kw, hk * kw, kc == kv_codec::int8}, simd::seq_kv{v + kh * vw, hk * vw, vc == kv_codec::int8}};
    };
    auto lo_of = [&](std::size_t i) { std::size_t pos = t - s + i; return window && pos + 1 > window ? pos + 1 - window : 0; };
    auto hi_of = [&](std::size_t i) { return causal ? std::min(t, t - s + i + 1) : t; };
    // query tokens per item: enough slots to amortise the K/V conversion, few enough for the score buffer
    const std::size_t tb = std::max<std::size_t>(1, std::min<std::size_t>(s, std::max<std::size_t>(1, 64 / grp)));
    const std::size_t ntb = (s + tb - 1) / tb;
    auto chunks_of = [&](std::size_t b) {   // chunk range touched by token block b
        std::size_t i0 = b * tb, i1 = std::min(s, i0 + tb), lo = t, hi = 0;
        for (std::size_t i = i0; i < i1; ++i) { lo = std::min(lo, lo_of(i)); hi = std::max(hi, hi_of(i)); }
        return lo < hi ? std::pair{lo / CH, (hi + CH - 1) / CH} : std::pair{std::size_t(0), std::size_t(0)};
    };
    struct slots { std::vector<const float*> q; std::vector<std::size_t> lo, hi, row; };   // query rows of one (KV head, token block)
    auto fill = [&](slots& sl, std::size_t i0, std::size_t i1, std::size_t kh) {
        sl.q.clear(); sl.lo.clear(); sl.hi.clear(); sl.row.clear();
        for (std::size_t i = i0; i < i1; ++i)
            for (std::size_t h = kh * grp; h < (kh + 1) * grp; ++h) {
                sl.q.push_back(q + (i * hq + h) * d); sl.lo.push_back(lo_of(i)); sl.hi.push_back(hi_of(i)); sl.row.push_back(i * hq + h);
            }
    };
    const std::size_t threads = std::max<std::size_t>(1, task::pool::global().size());
    const std::size_t part_floats = s * hq * nch * (dv + 2);
    const bool split = nch > 1 && hk * ntb < 2 * threads && part_floats <= (std::size_t(64) << 20);
    if (split) {
        struct item { std::size_t kh, b, c; };
        std::vector<item> items;
        for (std::size_t b = 0; b < ntb; ++b) {
            auto [c0, c1] = chunks_of(b);
            for (std::size_t kh = 0; kh < hk; ++kh) for (std::size_t c = c0; c < c1; ++c) items.push_back({kh, b, c});
        }
        const std::size_t nrow = s * hq;
        std::vector<float> pm(nrow * nch, -std::numeric_limits<float>::infinity()), pl(nrow * nch, 0.f), pa(nrow * nch * dv);
        task::parallel_for(items.size(), 1, [&](std::size_t ib, std::size_t ie) {
            slots sl; std::vector<float> scratch, m, l, acc;
            for (std::size_t it = ib; it < ie; ++it) {
                auto [kh, b, c] = items[it];
                fill(sl, b * tb, std::min(s, b * tb + tb), kh);
                const std::size_t ns = sl.q.size();
                scratch.resize(K.attn_scratch(ns, d, dv)); m.resize(ns); l.resize(ns); acc.resize(ns * dv);
                auto [kk, vv] = kv_of(kh);
                K.attn_chunk(kk, vv, d, dv, scale, c * CH, std::min(t, c * CH + CH), ns, sl.q.data(), sl.lo.data(), sl.hi.data(),
                             m.data(), l.data(), acc.data(), scratch.data());
                for (std::size_t j = 0; j < ns; ++j) {
                    std::size_t r = sl.row[j] * nch + c;
                    pm[r] = m[j]; pl[r] = l[j]; std::memcpy(pa.data() + r * dv, acc.data() + j * dv, dv * 4);
                }
            }
        });
        task::parallel_for(nrow, 4, [&](std::size_t rb, std::size_t re) {
            std::vector<float> A(dv);
            for (std::size_t r = rb; r < re; ++r) {
                float M = -std::numeric_limits<float>::infinity(), L = 0.f;
                for (std::size_t c = 0; c < nch; ++c) K.attn_merge(&M, &L, A.data(), pm[r * nch + c], pl[r * nch + c], pa.data() + (r * nch + c) * dv, dv);
                if (M == -std::numeric_limits<float>::infinity()) std::fill(out + r * dv, out + (r + 1) * dv, 0.f);
                else K.attn_finish(A.data(), L, out + r * dv, dv);
            }
        });
        return;
    }
    task::parallel_for(hk * ntb, 1, [&](std::size_t ib, std::size_t ie) {
        slots sl; std::vector<float> scratch, m, l, acc, M, L, A;
        for (std::size_t it = ib; it < ie; ++it) {
            // heavy (late, causal) token blocks first: item order runs the blocks backwards
            const std::size_t b = ntb - 1 - it / hk, kh = it % hk;
            fill(sl, b * tb, std::min(s, b * tb + tb), kh);
            const std::size_t ns = sl.q.size();
            scratch.resize(K.attn_scratch(ns, d, dv)); m.resize(ns); l.resize(ns); acc.resize(ns * dv);
            M.assign(ns, -std::numeric_limits<float>::infinity()); L.assign(ns, 0.f); A.resize(ns * dv);
            auto [kk, vv] = kv_of(kh);
            auto [c0, c1] = chunks_of(b);
            for (std::size_t c = c0; c < c1; ++c) {
                K.attn_chunk(kk, vv, d, dv, scale, c * CH, std::min(t, c * CH + CH), ns, sl.q.data(), sl.lo.data(), sl.hi.data(),
                             m.data(), l.data(), acc.data(), scratch.data());
                for (std::size_t j = 0; j < ns; ++j) K.attn_merge(&M[j], &L[j], A.data() + j * dv, m[j], l[j], acc.data() + j * dv, dv);
            }
            for (std::size_t j = 0; j < ns; ++j) {
                float* o = out + sl.row[j] * dv;
                if (M[j] == -std::numeric_limits<float>::infinity()) std::fill(o, o + dv, 0.f);
                else K.attn_finish(A.data() + j * dv, L[j], o, dv);
            }
        }
    });
}

} // namespace forge::graph::attn_cpu
