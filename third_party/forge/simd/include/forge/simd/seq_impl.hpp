#pragma once
// Implementation of the seq_kernels tables (seq.hpp). Included only by simd/src/seq_*.cpp: each defines
// FORGE_SEQ_NS (its namespace) and FORGE_SEQ_AVX512, FORGE_SEQ_AVX2 or neither (portable scalar), and is
// compiled with -ffp-contract=off so no multiply-add is fused unless written as fma.
//
// The algorithm is written once against V, 16 float lanes. Every backend implements V's operations with the
// same IEEE rounding, its horizontal sum with the tree ((v[i] + v[i+8]) -> + [i+4] -> + [i+2] -> + [i+1]) and
// reduce16 (16 vectors -> one vector of their 16 sums) with that same tree per vector, so all backends agree
// bit for bit.
#include <forge/simd/seq.hpp>
#include <forge/core/fma.hpp>   // MinGW's fmaf rounds twice: the scalar table must fuse exactly like the vector ones
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(FORGE_SEQ_AVX512) || defined(FORGE_SEQ_AVX2)
#include <immintrin.h>
#endif

#ifndef FORGE_SEQ_NS
#error "define FORGE_SEQ_NS before including seq_impl.hpp"
#endif

namespace forge::simd::FORGE_SEQ_NS {

constexpr float k_ninf = -std::numeric_limits<float>::infinity();

// ---- the one exp: range clamp, n = round(x log2 e), Cody-Waite reduction, degree-6 polynomial, scale by 2^n ----
inline float exp_s(float x) {
    x = std::min(std::max(x, -87.0f), 88.0f);
    float t = x * 1.44269504088896341f;
    float n = t + 12582912.0f;
    n = n - 12582912.0f;
    float r = forge::fmaf_exact(n, -0.693359375f, x);
    r = forge::fmaf_exact(n, 2.12194440e-4f, r);
    float p = 1.9875691500e-4f;
    p = forge::fmaf_exact(p, r, 1.3981999507e-3f);
    p = forge::fmaf_exact(p, r, 8.3334519073e-3f);
    p = forge::fmaf_exact(p, r, 4.1665795894e-2f);
    p = forge::fmaf_exact(p, r, 1.6666665459e-1f);
    p = forge::fmaf_exact(p, r, 5.0000001201e-1f);
    float r2 = r * r;
    float y = forge::fmaf_exact(p, r2, r);
    y = y + 1.0f;
    std::uint32_t bits = std::uint32_t(int(n) + 127) << 23;
    float s; std::memcpy(&s, &bits, 4);
    return y * s;
}

#if defined(FORGE_SEQ_AVX512)
// ------------------------------------------------ AVX-512 ------------------------------------------------
struct V { __m512 v; };
inline V zero() { return {_mm512_setzero_ps()}; }
inline V splat(float a) { return {_mm512_set1_ps(a)}; }
inline V load(const float* p) { return {_mm512_loadu_ps(p)}; }
inline V loadn(const float* p, std::size_t n) { return {_mm512_maskz_loadu_ps(__mmask16((1u << n) - 1u), p)}; }
inline void store(float* p, V a) { _mm512_storeu_ps(p, a.v); }
inline void storen(float* p, V a, std::size_t n) { _mm512_mask_storeu_ps(p, __mmask16((1u << n) - 1u), a.v); }
inline V load_i8(const std::int8_t* p) { return {_mm512_cvtepi32_ps(_mm512_cvtepi8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))))}; }
inline V load_i8n(const std::int8_t* p, std::size_t n) {
    return {_mm512_cvtepi32_ps(_mm512_cvtepi8_epi32(_mm_maskz_loadu_epi8(__mmask16((1u << n) - 1u), p)))};
}
inline V add(V a, V b) { return {_mm512_add_ps(a.v, b.v)}; }
inline V sub(V a, V b) { return {_mm512_sub_ps(a.v, b.v)}; }
inline V mul(V a, V b) { return {_mm512_mul_ps(a.v, b.v)}; }
inline V fma(V a, V b, V c) { return {_mm512_fmadd_ps(a.v, b.v, c.v)}; }
inline V vmax(V a, V b) { return {_mm512_max_ps(a.v, b.v)}; }
inline float hmax(V a) { return _mm512_reduce_max_ps(a.v); }
inline float hsum(V a) {
    __m256 lo = _mm512_castps512_ps256(a.v), hi = _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(a.v), 1));
    __m256 b = _mm256_add_ps(lo, hi);
    __m128 c = _mm_add_ps(_mm256_castps256_ps128(b), _mm256_extractf128_ps(b, 1));
    __m128 d = _mm_add_ps(c, _mm_movehl_ps(c, c));
    return _mm_cvtss_f32(_mm_add_ss(d, _mm_movehdup_ps(d)));
}
// lane p of the result = hsum(a[p]), same tree: halves, quarters, pairs, neighbours
inline V reduce16(const V* a) {
    __m512 x[8], y[4], z[2];
    for (int j = 0; j < 8; ++j)
        x[j] = _mm512_add_ps(_mm512_shuffle_f32x4(a[2 * j].v, a[2 * j + 1].v, 0x44), _mm512_shuffle_f32x4(a[2 * j].v, a[2 * j + 1].v, 0xEE));
    for (int k = 0; k < 4; ++k)
        y[k] = _mm512_add_ps(_mm512_shuffle_f32x4(x[2 * k], x[2 * k + 1], 0x88), _mm512_shuffle_f32x4(x[2 * k], x[2 * k + 1], 0xDD));
    for (int m = 0; m < 2; ++m)
        z[m] = _mm512_add_ps(_mm512_shuffle_ps(y[2 * m], y[2 * m + 1], 0x44), _mm512_shuffle_ps(y[2 * m], y[2 * m + 1], 0xEE));
    __m512 f = _mm512_add_ps(_mm512_shuffle_ps(z[0], z[1], 0x88), _mm512_shuffle_ps(z[0], z[1], 0xDD));
    // element 4L + t holds position 4t + L
    const __m512i idx = _mm512_setr_epi32(0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
    return {_mm512_permutexvar_ps(idx, f)};
}
// lanes outside [lo, hi) (lane indices, clipped to 0..16) become `fill`
inline V keep_range(V a, std::size_t lo, std::size_t hi, float fill) {
    unsigned m = (hi >= 16 ? 0xffffu : ((1u << hi) - 1u)) & ~(lo >= 16 ? 0xffffu : ((1u << lo) - 1u));
    return {_mm512_mask_blend_ps(__mmask16(m), _mm512_set1_ps(fill), a.v)};
}
inline V exp_v(V x) {
    __m512 v = _mm512_min_ps(_mm512_max_ps(x.v, _mm512_set1_ps(-87.0f)), _mm512_set1_ps(88.0f));
    __m512 t = _mm512_mul_ps(v, _mm512_set1_ps(1.44269504088896341f));
    __m512 n = _mm512_add_ps(t, _mm512_set1_ps(12582912.0f));
    n = _mm512_sub_ps(n, _mm512_set1_ps(12582912.0f));
    __m512 r = _mm512_fmadd_ps(n, _mm512_set1_ps(-0.693359375f), v);
    r = _mm512_fmadd_ps(n, _mm512_set1_ps(2.12194440e-4f), r);
    __m512 p = _mm512_set1_ps(1.9875691500e-4f);
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.3981999507e-3f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(8.3334519073e-3f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(4.1665795894e-2f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.6666665459e-1f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(5.0000001201e-1f));
    __m512 r2 = _mm512_mul_ps(r, r);
    __m512 y = _mm512_fmadd_ps(p, r2, r);
    y = _mm512_add_ps(y, _mm512_set1_ps(1.0f));
    __m512i e = _mm512_slli_epi32(_mm512_add_epi32(_mm512_cvtps_epi32(n), _mm512_set1_epi32(127)), 23);
    return {_mm512_mul_ps(y, _mm512_castsi512_ps(e))};
}
// exp(x) with lanes x < -80 (including -inf) set to 0
inline V exp_cut(V x) {
    __mmask16 small = _mm512_cmp_ps_mask(x.v, _mm512_set1_ps(-80.0f), _CMP_LT_OQ);
    return {_mm512_mask_blend_ps(small, exp_v(x).v, _mm512_setzero_ps())};
}
constexpr int k_vgroup = 8;   // value accumulators per pass (slots)
#elif defined(FORGE_SEQ_AVX2)
// ------------------------------------------------ AVX2 + FMA (two 8-lane halves) ------------------------------------------------
struct V { __m256 lo, hi; };
inline V zero() { return {_mm256_setzero_ps(), _mm256_setzero_ps()}; }
inline V splat(float a) { return {_mm256_set1_ps(a), _mm256_set1_ps(a)}; }
inline V load(const float* p) { return {_mm256_loadu_ps(p), _mm256_loadu_ps(p + 8)}; }
inline V loadn(const float* p, std::size_t n) { alignas(32) float t[16] = {}; std::memcpy(t, p, n * 4); return load(t); }
inline void store(float* p, V a) { _mm256_storeu_ps(p, a.lo); _mm256_storeu_ps(p + 8, a.hi); }
inline void storen(float* p, V a, std::size_t n) { alignas(32) float t[16]; store(t, a); std::memcpy(p, t, n * 4); }
inline V load_i8(const std::int8_t* p) {
    return {_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)))),
            _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + 8))))};
}
inline V load_i8n(const std::int8_t* p, std::size_t n) { alignas(16) std::int8_t t[16] = {}; std::memcpy(t, p, n); return load_i8(t); }
inline V add(V a, V b) { return {_mm256_add_ps(a.lo, b.lo), _mm256_add_ps(a.hi, b.hi)}; }
inline V sub(V a, V b) { return {_mm256_sub_ps(a.lo, b.lo), _mm256_sub_ps(a.hi, b.hi)}; }
inline V mul(V a, V b) { return {_mm256_mul_ps(a.lo, b.lo), _mm256_mul_ps(a.hi, b.hi)}; }
inline V fma(V a, V b, V c) { return {_mm256_fmadd_ps(a.lo, b.lo, c.lo), _mm256_fmadd_ps(a.hi, b.hi, c.hi)}; }
inline V vmax(V a, V b) { return {_mm256_max_ps(a.lo, b.lo), _mm256_max_ps(a.hi, b.hi)}; }
inline float hmax(V a) { alignas(32) float t[16]; store(t, a); float m = t[0]; for (int i = 1; i < 16; ++i) m = std::max(m, t[i]); return m; }
inline float hsum(V a) {
    __m256 b = _mm256_add_ps(a.lo, a.hi);
    __m128 c = _mm_add_ps(_mm256_castps256_ps128(b), _mm256_extractf128_ps(b, 1));
    __m128 d = _mm_add_ps(c, _mm_movehl_ps(c, c));
    return _mm_cvtss_f32(_mm_add_ss(d, _mm_movehdup_ps(d)));
}
inline V reduce16(const V* a) { alignas(32) float t[16]; for (int p = 0; p < 16; ++p) t[p] = hsum(a[p]); return load(t); }
inline V keep_range(V a, std::size_t lo, std::size_t hi, float fill) {
    alignas(32) float t[16]; store(t, a);
    for (std::size_t p = 0; p < 16; ++p) if (p < lo || p >= hi) t[p] = fill;
    return load(t);
}
inline __m256 exp8(__m256 x) {
    __m256 v = _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(-87.0f)), _mm256_set1_ps(88.0f));
    __m256 t = _mm256_mul_ps(v, _mm256_set1_ps(1.44269504088896341f));
    __m256 n = _mm256_add_ps(t, _mm256_set1_ps(12582912.0f));
    n = _mm256_sub_ps(n, _mm256_set1_ps(12582912.0f));
    __m256 r = _mm256_fmadd_ps(n, _mm256_set1_ps(-0.693359375f), v);
    r = _mm256_fmadd_ps(n, _mm256_set1_ps(2.12194440e-4f), r);
    __m256 p = _mm256_set1_ps(1.9875691500e-4f);
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.3981999507e-3f));
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(8.3334519073e-3f));
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(4.1665795894e-2f));
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.6666665459e-1f));
    p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(5.0000001201e-1f));
    __m256 r2 = _mm256_mul_ps(r, r);
    __m256 y = _mm256_fmadd_ps(p, r2, r);
    y = _mm256_add_ps(y, _mm256_set1_ps(1.0f));
    __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(n), _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(e));
}
inline V exp_v(V x) { return {exp8(x.lo), exp8(x.hi)}; }
inline V exp_cut(V x) {
    __m256 c = _mm256_set1_ps(-80.0f), z = _mm256_setzero_ps();
    return {_mm256_blendv_ps(exp8(x.lo), z, _mm256_cmp_ps(x.lo, c, _CMP_LT_OQ)), _mm256_blendv_ps(exp8(x.hi), z, _mm256_cmp_ps(x.hi, c, _CMP_LT_OQ))};
}
constexpr int k_vgroup = 4;
#else
// ------------------------------------------------ portable scalar ------------------------------------------------
struct V { float v[16]; };
inline V zero() { V r; for (auto& x : r.v) x = 0.f; return r; }
inline V splat(float a) { V r; for (auto& x : r.v) x = a; return r; }
inline V load(const float* p) { V r; std::memcpy(r.v, p, 64); return r; }
inline V loadn(const float* p, std::size_t n) { V r = zero(); std::memcpy(r.v, p, n * 4); return r; }
inline void store(float* p, V a) { std::memcpy(p, a.v, 64); }
inline void storen(float* p, V a, std::size_t n) { std::memcpy(p, a.v, n * 4); }
inline V load_i8(const std::int8_t* p) { V r; for (int i = 0; i < 16; ++i) r.v[i] = float(p[i]); return r; }
inline V load_i8n(const std::int8_t* p, std::size_t n) { V r = zero(); for (std::size_t i = 0; i < n; ++i) r.v[i] = float(p[i]); return r; }
inline V add(V a, V b) { for (int i = 0; i < 16; ++i) a.v[i] = a.v[i] + b.v[i]; return a; }
inline V sub(V a, V b) { for (int i = 0; i < 16; ++i) a.v[i] = a.v[i] - b.v[i]; return a; }
inline V mul(V a, V b) { for (int i = 0; i < 16; ++i) a.v[i] = a.v[i] * b.v[i]; return a; }
inline V fma(V a, V b, V c) { for (int i = 0; i < 16; ++i) c.v[i] = forge::fmaf_exact(a.v[i], b.v[i], c.v[i]); return c; }
inline V vmax(V a, V b) { for (int i = 0; i < 16; ++i) a.v[i] = std::max(a.v[i], b.v[i]); return a; }
inline float hmax(V a) { float m = a.v[0]; for (int i = 1; i < 16; ++i) m = std::max(m, a.v[i]); return m; }
inline float hsum(V a) {
    float b[8], c[4], d[2];
    for (int i = 0; i < 8; ++i) b[i] = a.v[i] + a.v[i + 8];
    for (int i = 0; i < 4; ++i) c[i] = b[i] + b[i + 4];
    for (int i = 0; i < 2; ++i) d[i] = c[i] + c[i + 2];
    return d[0] + d[1];
}
inline V reduce16(const V* a) { V r; for (int p = 0; p < 16; ++p) r.v[p] = hsum(a[p]); return r; }
inline V keep_range(V a, std::size_t lo, std::size_t hi, float fill) {
    for (std::size_t p = 0; p < 16; ++p) if (p < lo || p >= hi) a.v[p] = fill;
    return a;
}
inline V exp_v(V x) { for (auto& e : x.v) e = exp_s(e); return x; }
inline V exp_cut(V x) { for (auto& e : x.v) e = e < -80.0f ? 0.0f : exp_s(e); return x; }
constexpr int k_vgroup = 4;
#endif

// ------------------------------------------------ shared algorithms ------------------------------------------------
constexpr std::size_t CH = seq_attn_chunk;
inline std::size_t round16(std::size_t n) { return (n + 15) & ~std::size_t(15); }

inline std::size_t attn_scratch(std::size_t ns, std::size_t d, std::size_t) {
    std::size_t d16 = round16(d);
    return 16 * d16 + d16 + ns * d16 + ns * CH + CH + 16;
}

inline void attn_chunk(const seq_kv& K, const seq_kv& Vv, std::size_t d, std::size_t dv, float scale, std::size_t c0, std::size_t c1,
                       std::size_t ns, const float* const* q, const std::size_t* lo, const std::size_t* hi,
                       float* m, float* l, float* acc, float* scratch) {
    const std::size_t d16 = round16(d), n = c1 - c0, nb = (n + 15) / 16;
    float* Kf = scratch;                 // 16 converted key rows
    float* zrow = Kf + 16 * d16;         // zeros (rows past the chunk end)
    float* qp = zrow + d16;              // queries, zero padded to d16
    float* sco = qp + ns * d16;          // [ns][CH] scores, then weights
    float* vsc = sco + ns * CH;          // value scales of the chunk (int8)
    float* ks = vsc + CH;                // key scales of the current block
    std::fill(zrow, zrow + d16, 0.f);
    auto lr = [&](std::size_t s) { return std::min(std::max(lo[s], c0), c1) - c0; };
    auto hr = [&](std::size_t s) { return std::min(std::max(hi[s], c0), c1) - c0; };
    for (std::size_t s = 0; s < ns; ++s) {
        std::memcpy(qp + s * d16, q[s], d * 4);
        std::fill(qp + s * d16 + d, qp + (s + 1) * d16, 0.f);
    }
    const bool kdirect = !K.int8 && d % 16 == 0;
    const float* kp[16];
    const V vscale = splat(scale);
    for (std::size_t b = 0; b < nb; ++b) {
        const std::size_t p0 = b * 16, np = std::min<std::size_t>(16, n - p0);
        bool any = false;
        for (std::size_t s = 0; s < ns && !any; ++s) any = std::max(lr(s), p0) < std::min(hr(s), p0 + np);
        if (!any) continue;
        for (std::size_t p = 0; p < 16; ++p) {
            if (p >= np) { kp[p] = zrow; ks[p] = 0.f; continue; }
            const float* row = K.base + (c0 + p0 + p) * K.stride;
            if (kdirect) { kp[p] = row; continue; }
            float* dst = Kf + p * d16;
            if (K.int8) {
                ks[p] = row[0];
                const auto* by = reinterpret_cast<const std::int8_t*>(row + 1);
                std::size_t e = 0;
                for (; e + 16 <= d; e += 16) store(dst + e, load_i8(by + e));
                if (e < d) store(dst + e, load_i8n(by + e, d - e));
            } else {
                std::memcpy(dst, row, d * 4);
                std::fill(dst + d, dst + d16, 0.f);
            }
            kp[p] = dst;
        }
        const V ksv = K.int8 ? load(ks) : splat(1.f);
        for (std::size_t s = 0; s < ns; ++s) {
            std::size_t a0 = std::max(lr(s), p0), a1 = std::min(hr(s), p0 + np);
            if (a0 >= a1) continue;
            V a[16];
            for (auto& x : a) x = zero();
            const float* qs = qp + s * d16;
            for (std::size_t e = 0; e < d16; e += 16) {
                V qv = load(qs + e);
                for (int p = 0; p < 16; ++p) a[p] = fma(qv, load(kp[p] + e), a[p]);
            }
            V r = reduce16(a);
            if (K.int8) r = mul(r, ksv);
            r = mul(r, vscale);
            store(sco + s * CH + p0, keep_range(r, a0 - p0, a1 - p0, k_ninf));
        }
    }
    // chunk softmax statistics; weights stay in sco (times the value scale for int8 values)
    if (Vv.int8) for (std::size_t p = 0; p < n; ++p) vsc[p] = (Vv.base + (c0 + p) * Vv.stride)[0];
    for (std::size_t s = 0; s < ns; ++s) {
        std::size_t a0 = lr(s), a1 = hr(s);
        float* w = sco + s * CH;
        if (a0 >= a1) { m[s] = k_ninf; l[s] = 0.f; std::fill(w, w + nb * 16, 0.f); continue; }
        std::size_t bl = a0 / 16, bh = (a1 + 15) / 16;
        V mx = splat(k_ninf);
        for (std::size_t b = bl; b < bh; ++b) mx = vmax(mx, load(w + b * 16));
        float M = hmax(mx);
        V ls = zero(), vm = splat(M);
        for (std::size_t b = bl; b < bh; ++b) {
            V e = exp_cut(sub(load(w + b * 16), vm));
            ls = add(ls, e);
            if (Vv.int8) e = mul(e, b * 16 + 16 <= n ? load(vsc + b * 16) : loadn(vsc + b * 16, n - b * 16));
            store(w + b * 16, e);
        }
        m[s] = M; l[s] = hsum(ls);
        std::fill(w, w + bl * 16, 0.f);
        std::fill(w + bh * 16, w + nb * 16, 0.f);
    }
    // weighted values: one value row conversion feeds k_vgroup slots
    for (std::size_t s0 = 0; s0 < ns; s0 += k_vgroup) {
        const std::size_t g = std::min<std::size_t>(k_vgroup, ns - s0);
        std::size_t j0 = n, j1 = 0;
        for (std::size_t t = 0; t < g; ++t) {
            std::size_t a0 = lr(s0 + t), a1 = hr(s0 + t);
            if (a0 < a1) { j0 = std::min(j0, a0); j1 = std::max(j1, a1); }
        }
        for (std::size_t e = 0; e < dv; e += 16) {
            const std::size_t ne = std::min<std::size_t>(16, dv - e);
            V a[k_vgroup];
            for (auto& x : a) x = zero();
            for (std::size_t j = j0; j < j1; ++j) {
                const float* row = Vv.base + (c0 + j) * Vv.stride;
                V vv;
                if (Vv.int8) { const auto* by = reinterpret_cast<const std::int8_t*>(row + 1) + e; vv = ne == 16 ? load_i8(by) : load_i8n(by, ne); }
                else vv = ne == 16 ? load(row + e) : loadn(row + e, ne);
                for (std::size_t t = 0; t < g; ++t) a[t] = fma(splat(sco[(s0 + t) * CH + j]), vv, a[t]);
            }
            for (std::size_t t = 0; t < g; ++t) {
                if (ne == 16) store(acc + (s0 + t) * dv + e, a[t]);
                else storen(acc + (s0 + t) * dv + e, a[t], ne);
            }
        }
    }
}

// running (M, L, A) <- merge with a chunk partial (m, l, acc); M == -inf means "nothing yet"
inline void attn_merge(float* M, float* L, float* A, float m, float l, const float* acc, std::size_t dv) {
    if (m == k_ninf) return;
    if (*M == k_ninf) { *M = m; *L = l; std::memcpy(A, acc, dv * 4); return; }
    float Mn = std::max(*M, m);
    float a = exp_s(*M - Mn), b = exp_s(m - Mn);
    float lb = l * b;
    *L = forge::fmaf_exact(*L, a, lb);
    *M = Mn;
    V va = splat(a), vb = splat(b);
    std::size_t e = 0;
    for (; e + 16 <= dv; e += 16) store(A + e, fma(load(A + e), va, mul(load(acc + e), vb)));
    if (e < dv) storen(A + e, fma(loadn(A + e, dv - e), va, mul(loadn(acc + e, dv - e), vb)), dv - e);
}

inline void attn_finish(const float* A, float L, float* out, std::size_t dv) {
    float inv = 1.0f / L;
    V vi = splat(inv);
    std::size_t e = 0;
    for (; e + 16 <= dv; e += 16) store(out + e, mul(load(A + e), vi));
    if (e < dv) storen(out + e, mul(loadn(A + e, dv - e), vi), dv - e);
}

inline void l2norm_scale(const float* x, float* y, std::size_t n, float scale, int l2) {
    float f = scale;
    if (l2) {
        V a = zero();
        std::size_t e = 0;
        for (; e + 16 <= n; e += 16) { V v = load(x + e); a = fma(v, v, a); }
        if (e < n) { V v = loadn(x + e, n - e); a = fma(v, v, a); }
        float ss = hsum(a) + 1e-6f;
        f = scale / std::sqrt(ss);
    }
    V vf = splat(f);
    std::size_t e = 0;
    for (; e + 16 <= n; e += 16) store(y + e, mul(load(x + e), vf));
    if (e < n) storen(y + e, mul(loadn(x + e, n - e), vf), n - e);
}

// columns [j0, j0 + nj), nj <= 32: two 16-column halves run interleaved (independent per lane)
inline void delta_cols(const float* q, const float* k, std::size_t qk_stride, const float* v, std::size_t v_stride,
                       const float* g, const float* beta, std::size_t gb_stride, float* S, float* out, std::size_t o_stride,
                       std::size_t s, std::size_t dk, std::size_t dv, std::size_t j0, std::size_t nj) {
    const std::size_t n0 = std::min<std::size_t>(nj, 16), n1 = nj - n0;
    auto ld = [](const float* p, std::size_t n) { return n == 16 ? load(p) : loadn(p, n); };
    auto st = [](float* p, V a, std::size_t n) { if (n == 16) store(p, a); else storen(p, a, n); };
    for (std::size_t t = 0; t < s; ++t) {
        const float* qt = q + t * qk_stride; const float* kt = k + t * qk_stride;
        const V dec = splat(exp_s(g[t * gb_stride])), bt = splat(beta[t * gb_stride]);
        V kv0 = zero(), kv1 = zero();
        for (std::size_t i = 0; i < dk; ++i) {
            float* row = S + i * dv + j0;
            V ki = splat(kt[i]);
            V r0 = mul(ld(row, n0), dec); st(row, r0, n0); kv0 = fma(r0, ki, kv0);
            if (n1) { V r1 = mul(ld(row + 16, n1), dec); st(row + 16, r1, n1); kv1 = fma(r1, ki, kv1); }
        }
        const float* vt = v + t * v_stride + j0;
        V d0 = mul(sub(ld(vt, n0), kv0), bt), d1 = n1 ? mul(sub(ld(vt + 16, n1), kv1), bt) : zero();
        V o0 = zero(), o1 = zero();
        for (std::size_t i = 0; i < dk; ++i) {
            float* row = S + i * dv + j0;
            V ki = splat(kt[i]), qi = splat(qt[i]);
            V r0 = fma(ki, d0, ld(row, n0)); st(row, r0, n0); o0 = fma(r0, qi, o0);
            if (n1) { V r1 = fma(ki, d1, ld(row + 16, n1)); st(row + 16, r1, n1); o1 = fma(r1, qi, o1); }
        }
        float* ot = out + t * o_stride + j0;
        st(ot, o0, n0);
        if (n1) st(ot + 16, o1, n1);
    }
}

} // namespace forge::simd::FORGE_SEQ_NS

#define FORGE_SEQ_EXPORT_TABLE(NAME)                                                                           \
    namespace forge::simd::FORGE_SEQ_NS {                                                                     \
    extern const seq_kernels table;                                                                           \
    const seq_kernels table = {NAME, &attn_scratch, &attn_chunk, &attn_merge, &attn_finish, &l2norm_scale,   \
                               &delta_cols, &exp_s};                                                          \
    }
