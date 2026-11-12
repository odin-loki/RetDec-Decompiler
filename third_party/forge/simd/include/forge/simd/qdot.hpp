#pragma once
// Quantised-row kernels: decode GGUF-style blocks (q8_0, q4_k, q6_k, q5_k) and dot them against f32
// activations without materialising the row. The AVX2 decoders produce exactly the same floats as the
// scalar codecs in graph/codecs.hpp (same operations, same order, no FMA contraction), and qdot keeps the
// accumulator layout of algo::dot, so qdot(row, x) == dot(x, decode(row)) bit for bit.
#include <forge/simd/math.hpp>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace forge::simd {
enum class qfmt : int { generic = 0, q8_0 = 1, q4_k = 2, q6_k = 3, q5_k = 4 };
using qdecode_fn = void (*)(const std::uint8_t*, std::size_t, float*);  // the codec's scalar decoder
}

namespace forge::simd::algo {
namespace qd {
using u8 = std::uint8_t;
inline float h2f(const u8* p) { std::uint16_t b; std::memcpy(&b, p, 2); return float(f16::from_bits(b)); }
inline void scale_min_k4(int j, const u8* q, u8& d, u8& m) {
    if (j < 4) { d = q[j] & 63; m = q[j + 4] & 63; }
    else { d = u8((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4)); m = u8((q[j + 4] >> 4) | ((q[j] >> 6) << 4)); }
}
#if defined(__AVX2__)
inline __m256i ld8(const u8* p) { return _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p))); }
inline void dec_q8_0(const u8* in, std::size_t n, float* x) {
    for (std::size_t b = 0; b < n; b += 32, in += 34) {
        __m256 d = _mm256_set1_ps(h2f(in));
        for (int i = 0; i < 32; i += 8) {
            __m256i q = _mm256_cvtepi8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(in + 2 + i)));
            _mm256_storeu_ps(x + b + i, _mm256_mul_ps(d, _mm256_cvtepi32_ps(q)));
        }
    }
}
inline void dec_q4_k(const u8* in, std::size_t n, float* x) {
    const __m256i lo = _mm256_set1_epi32(0xF);
    for (std::size_t b = 0; b < n; b += 256, in += 144) {
        float d = h2f(in), dmin = h2f(in + 2); const u8* sc = in + 4; const u8* q = in + 16; float* y = x + b;
        for (int j = 0, is = 0; j < 256; j += 64, q += 32, is += 2) {
            u8 s, m; scale_min_k4(is, sc, s, m); __m256 d1 = _mm256_set1_ps(d * s), m1 = _mm256_set1_ps(dmin * m);
            scale_min_k4(is + 1, sc, s, m); __m256 d2 = _mm256_set1_ps(d * s), m2 = _mm256_set1_ps(dmin * m);
            for (int l = 0; l < 32; l += 8) {
                __m256i v = ld8(q + l);
                _mm256_storeu_ps(y + j + l, _mm256_sub_ps(_mm256_mul_ps(d1, _mm256_cvtepi32_ps(_mm256_and_si256(v, lo))), m1));
                _mm256_storeu_ps(y + j + 32 + l, _mm256_sub_ps(_mm256_mul_ps(d2, _mm256_cvtepi32_ps(_mm256_srli_epi32(v, 4))), m2));
            }
        }
    }
}
inline void dec_q6_k(const u8* in, std::size_t n, float* x) {
    const __m256i f = _mm256_set1_epi32(0xF), t = _mm256_set1_epi32(3), o = _mm256_set1_epi32(32);
    for (std::size_t b = 0; b < n; b += 256, in += 210) {
        const u8* ql = in; const u8* qh = in + 128; const auto* sc = reinterpret_cast<const std::int8_t*>(in + 192);
        float d = h2f(in + 208);
        for (int h = 0; h < 2; ++h, ql += 64, qh += 32, sc += 8) {
            float* y = x + b + 128 * h;
            for (int l = 0; l < 32; l += 8) {
                int is = l / 16;
                __m256i a = ld8(ql + l), c = ld8(ql + l + 32), hh = ld8(qh + l);
                auto mk = [&](__m256i low, int sh) { return _mm256_sub_epi32(_mm256_or_si256(low, _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(hh, sh), t), 4)), o); };
                __m256i q1 = mk(_mm256_and_si256(a, f), 0), q2 = mk(_mm256_and_si256(c, f), 2);
                __m256i q3 = mk(_mm256_srli_epi32(a, 4), 4), q4 = mk(_mm256_srli_epi32(c, 4), 6);
                _mm256_storeu_ps(y + l, _mm256_mul_ps(_mm256_set1_ps(d * sc[is]), _mm256_cvtepi32_ps(q1)));
                _mm256_storeu_ps(y + l + 32, _mm256_mul_ps(_mm256_set1_ps(d * sc[is + 2]), _mm256_cvtepi32_ps(q2)));
                _mm256_storeu_ps(y + l + 64, _mm256_mul_ps(_mm256_set1_ps(d * sc[is + 4]), _mm256_cvtepi32_ps(q3)));
                _mm256_storeu_ps(y + l + 96, _mm256_mul_ps(_mm256_set1_ps(d * sc[is + 6]), _mm256_cvtepi32_ps(q4)));
            }
        }
    }
}
inline void dec_q5_k(const u8* in, std::size_t n, float* x) {
    const __m256i lo = _mm256_set1_epi32(0xF), z = _mm256_setzero_si256(), sixteen = _mm256_set1_epi32(16);
    for (std::size_t b = 0; b < n; b += 256, in += 176) {
        float d = h2f(in), dmin = h2f(in + 2); const u8* sc = in + 4; const u8* qh = in + 16; const u8* ql = in + 48; float* y = x + b;
        int u1 = 1, u2 = 2;
        for (int j = 0, is = 0; j < 256; j += 64, ql += 32, is += 2, u1 <<= 2, u2 <<= 2) {
            u8 s, m; scale_min_k4(is, sc, s, m); __m256 d1 = _mm256_set1_ps(d * s), m1 = _mm256_set1_ps(dmin * m);
            scale_min_k4(is + 1, sc, s, m); __m256 d2 = _mm256_set1_ps(d * s), m2 = _mm256_set1_ps(dmin * m);
            __m256i b1 = _mm256_set1_epi32(u1), b2 = _mm256_set1_epi32(u2);
            for (int l = 0; l < 32; l += 8) {
                __m256i v = ld8(ql + l), h = ld8(qh + l);
                __m256i h1 = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(h, b1), z), sixteen);
                __m256i h2 = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(h, b2), z), sixteen);
                _mm256_storeu_ps(y + j + l, _mm256_sub_ps(_mm256_mul_ps(d1, _mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_and_si256(v, lo), h1))), m1));
                _mm256_storeu_ps(y + j + 32 + l, _mm256_sub_ps(_mm256_mul_ps(d2, _mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_srli_epi32(v, 4), h2))), m2));
            }
        }
    }
}
#endif
template <Target Tg> constexpr bool simd_decoders() {
#if defined(__AVX2__)
    return true;   // this TU is compiled for AVX2 or better (the avx2 and avx512 kernel tables)
#else
    return false;
#endif
}
} // namespace qd

// Decode n elements (a whole number of blocks) of a quantised row.
template <Target Tg> void qdecode(int fmt, const std::uint8_t* row, std::size_t n, float* out, qdecode_fn fallback) {
#if defined(__AVX2__)
    if constexpr (qd::simd_decoders<Tg>()) {
        switch (qfmt(fmt)) {
            case qfmt::q8_0: return qd::dec_q8_0(row, n, out);
            case qfmt::q4_k: return qd::dec_q4_k(row, n, out);
            case qfmt::q6_k: return qd::dec_q6_k(row, n, out);
            case qfmt::q5_k: return qd::dec_q5_k(row, n, out);
            default: break;
        }
    }
#endif
    (void)fmt; fallback(row, n, out);
}

// dot(x, decode(row)) over k elements, bit-identical to algo::dot: 256-element tiles are decoded into L1 and fed
// to the same four accumulators in the same order. `block`/`block_bytes` describe the codec (256 % block == 0).
template <Target Tg> float qdot(int fmt, const std::uint8_t* row, const float* x, std::size_t k, qdecode_fn fallback,
                                std::size_t block, std::size_t block_bytes) {
    using V = vec<float, Tg>;
    constexpr std::size_t W = V::N, T = 256;
    static_assert(T % (4 * W) == 0);
    alignas(64) float buf[T];
    V a0 = splat<Tg>(0.f), a1 = a0, a2 = a0, a3 = a0;
    const std::size_t tile_bytes = T / block * block_bytes;
    std::size_t i = 0;
    for (; i < k; i += T, row += tile_bytes) {
        std::size_t n = std::min(T, k - i);
        qdecode<Tg>(fmt, row, n, buf, fallback);
        std::size_t j = 0;
        for (; j + 4 * W <= n; j += 4 * W) {
            a0 = fma(load<Tg>(x + i + j), load<Tg>(buf + j), a0);
            a1 = fma(load<Tg>(x + i + j + W), load<Tg>(buf + j + W), a1);
            a2 = fma(load<Tg>(x + i + j + 2 * W), load<Tg>(buf + j + 2 * W), a2);
            a3 = fma(load<Tg>(x + i + j + 3 * W), load<Tg>(buf + j + 3 * W), a3);
        }
        if (j < n) {   // only the final tile can be partial
            const float* xs = x + i + j; const float* bs = buf + j;
            strip<Tg, float>(n - j, [&](std::size_t jj, auto m) { a0 = fma(load(xs + jj, m), load(bs + jj, m), a0); });
        }
    }
    return reduce_add((a0 + a1) + (a2 + a3));
}

} // namespace forge::simd::algo
