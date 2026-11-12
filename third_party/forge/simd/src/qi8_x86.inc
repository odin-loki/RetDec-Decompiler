// x86 integer matvec kernels (qdot_i8.hpp), written once and compiled per ISA. The including TU defines
//   QI8_NS                 a namespace unique to the TU (the bodies differ per ISA: no ODR clashes)
//   QI8_DPWSSD(acc, a, b)  acc + sum of int16 pairs a*b into int32 lanes
//   QI8_DPBUSD(acc, u, s)  acc + sum of 4 u8*s8 products into int32 lanes
// All vectors are 256 bits (AVX-512 VNNI uses the EVEX ymm forms: no 512-bit frequency licence on decode).
// Every kernel forms the reference's exact int32 sums and then the reference's float steps (qdot_i8.hpp).
#include <forge/simd/qdot_i8.hpp>
#include <immintrin.h>
#include <algorithm>

namespace forge::simd::qi8::QI8_NS {

static inline __m256i ld(const void* p) { return _mm256_loadu_si256(static_cast<const __m256i*>(p)); }
static inline std::int32_t hsum(__m256i v) {
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
}
static inline std::int32_t hsum128(__m128i s) {
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
}
static inline float h2f_(const u8* p) { std::uint16_t b; std::memcpy(&b, p, 2); return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(b))); }
static inline void pf(const u8* p, std::size_t bytes) {
    const int d = qi8_prefetch_bytes;
    if (d > 0) for (std::size_t o = 0; o < bytes; o += 64) _mm_prefetch(reinterpret_cast<const char*>(p + d + o), _MM_HINT_T0);
}

// ---- activation quantisation (the reference's rounding: x * (1/d), nearest-even, clamp to +-127) ----
static inline float amax_n(const float* x, int n) {
    const __m256 sign = _mm256_set1_ps(-0.f);
    __m256 m = _mm256_setzero_ps();
    for (int i = 0; i < n; i += 8) m = _mm256_max_ps(m, _mm256_andnot_ps(sign, _mm256_loadu_ps(x + i)));
    __m128 s = _mm_max_ps(_mm256_castps256_ps128(m), _mm256_extractf128_ps(m, 1));
    s = _mm_max_ps(s, _mm_movehl_ps(s, s));
    s = _mm_max_ss(s, _mm_movehdup_ps(s));
    return _mm_cvtss_f32(s);
}
// 32 floats -> 32 int8 (in order)
static inline __m256i q32(const float* x, __m256 id) {
    __m256i a = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x), id));
    __m256i b = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + 8), id));
    __m256i c = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + 16), id));
    __m256i d = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + 24), id));
    __m256i ab = _mm256_packs_epi32(a, b), cd = _mm256_packs_epi32(c, d);   // lanes interleaved per 128
    __m256i r = _mm256_packs_epi16(ab, cd);
    r = _mm256_permutevar8x32_epi32(r, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
    return _mm256_max_epi8(r, _mm256_set1_epi8(-127));
}
static void quantize(int fmt, const float* x, std::size_t k, void* out) {
    if (qfmt(fmt) == qfmt::q8_0) {
        auto* o = static_cast<act_32*>(out);
        for (std::size_t b = 0; b < k / 32; ++b, x += 32, ++o) {
            float d = amax_n(x, 32) / 127.f, id = d != 0.f ? 1.f / d : 0.f;
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(o->q), q32(x, _mm256_set1_ps(id)));
            o->d = d;
        }
        return;
    }
    auto* o = static_cast<act_k*>(out);
    const __m256i ones = _mm256_set1_epi8(1);
    for (std::size_t b = 0; b < k / 256; ++b, x += 256, ++o) {
        float d = amax_n(x, 256) / 127.f, id = d != 0.f ? 1.f / d : 0.f;
        __m256 vid = _mm256_set1_ps(id);
        alignas(32) std::int16_t s16[16];
        for (int j = 0; j < 8; ++j) {
            __m256i q = q32(x + 32 * j, vid);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(o->q + 32 * j), q);
            // sums of 16: pairs (maddubs with ones), then 8 int16 -> two sums of 8 pairs
            __m256i p = _mm256_maddubs_epi16(ones, q);                 // 16 int16 = pair sums
            __m256i w = _mm256_madd_epi16(p, _mm256_set1_epi16(1));    // 8 int32 = sums of 4
            __m128i lo = _mm256_castsi256_si128(w), hi = _mm256_extracti128_si256(w, 1);
            s16[2 * j] = std::int16_t(hsum128(lo)); s16[2 * j + 1] = std::int16_t(hsum128(hi));
        }
        std::memcpy(o->bsum16, s16, sizeof s16);
        for (int s = 0; s < 8; ++s) o->bsum32[s] = std::int16_t(s16[2 * s] + s16[2 * s + 1]);
        o->d = d;
    }
}

// ---- products: NV activation vectors per weight row (weights unpacked once per block for all of them) ----
template <int NV> static void rows_q4_k(const u8* w, std::size_t rb, std::size_t rows, const act_k* const* av, std::size_t nb, float* y, std::size_t ys) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    for (std::size_t r = 0; r < rows; ++r) {
        const u8* blk = w + r * rb;
        float acc[NV] = {}, accm[NV] = {};
        for (std::size_t b = 0; b < nb; ++b, blk += 144) {
            pf(blk, 144);
            u8 sc[8], mn[8]; unpack_scales(blk + 4, sc, mn);
            __m256i ai[NV];
            for (int v = 0; v < NV; ++v) ai[v] = _mm256_setzero_si256();
            for (int j = 0; j < 4; ++j) {
                __m256i qs = ld(blk + 16 + 32 * j);
                __m256i lo = _mm256_and_si256(qs, m4), hi = _mm256_and_si256(_mm256_srli_epi16(qs, 4), m4);
                __m256i slo = _mm256_set1_epi16(sc[2 * j]), shi = _mm256_set1_epi16(sc[2 * j + 1]);
                for (int v = 0; v < NV; ++v) {
                    const i8* aq = av[v][b].q + 64 * j;
                    ai[v] = QI8_DPWSSD(ai[v], _mm256_maddubs_epi16(lo, ld(aq)), slo);
                    ai[v] = QI8_DPWSSD(ai[v], _mm256_maddubs_epi16(hi, ld(aq + 32)), shi);
                }
            }
            __m128i mv = _mm_cvtepu8_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(mn)));
            const float d = h2f_(blk), dmin = h2f_(blk + 2);
            for (int v = 0; v < NV; ++v) {
                const act_k& a = av[v][b];
                std::int32_t imin = hsum128(_mm_madd_epi16(mv, _mm_loadu_si128(reinterpret_cast<const __m128i*>(a.bsum32))));
                acc[v] = fmac(d * a.d, float(hsum(ai[v])), acc[v]);
                accm[v] = fmac(dmin * a.d, float(imin), accm[v]);
            }
        }
        for (int v = 0; v < NV; ++v) y[std::size_t(v) * ys + r] = acc[v] - accm[v];
    }
}
template <int NV> static void rows_q5_k(const u8* w, std::size_t rb, std::size_t rows, const act_k* const* av, std::size_t nb, float* y, std::size_t ys) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), one = _mm256_set1_epi8(1);
    for (std::size_t r = 0; r < rows; ++r) {
        const u8* blk = w + r * rb;
        float acc[NV] = {}, accm[NV] = {};
        for (std::size_t b = 0; b < nb; ++b, blk += 176) {
            pf(blk, 176);
            u8 sc[8], mn[8]; unpack_scales(blk + 4, sc, mn);
            const __m256i qh = ld(blk + 16);
            __m256i ai[NV];
            for (int v = 0; v < NV; ++v) ai[v] = _mm256_setzero_si256();
            for (int j = 0; j < 4; ++j) {
                __m256i qs = ld(blk + 48 + 32 * j);
                __m256i hl = _mm256_slli_epi16(_mm256_and_si256(_mm256_srl_epi16(qh, _mm_cvtsi32_si128(2 * j)), one), 4);
                __m256i hh = _mm256_slli_epi16(_mm256_and_si256(_mm256_srl_epi16(qh, _mm_cvtsi32_si128(2 * j + 1)), one), 4);
                __m256i lo = _mm256_or_si256(_mm256_and_si256(qs, m4), hl);
                __m256i hi = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(qs, 4), m4), hh);
                __m256i slo = _mm256_set1_epi16(sc[2 * j]), shi = _mm256_set1_epi16(sc[2 * j + 1]);
                for (int v = 0; v < NV; ++v) {
                    const i8* aq = av[v][b].q + 64 * j;
                    ai[v] = QI8_DPWSSD(ai[v], _mm256_maddubs_epi16(lo, ld(aq)), slo);
                    ai[v] = QI8_DPWSSD(ai[v], _mm256_maddubs_epi16(hi, ld(aq + 32)), shi);
                }
            }
            __m128i mv = _mm_cvtepu8_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(mn)));
            const float d = h2f_(blk), dmin = h2f_(blk + 2);
            for (int v = 0; v < NV; ++v) {
                const act_k& a = av[v][b];
                std::int32_t imin = hsum128(_mm_madd_epi16(mv, _mm_loadu_si128(reinterpret_cast<const __m128i*>(a.bsum32))));
                acc[v] = fmac(d * a.d, float(hsum(ai[v])), acc[v]);
                accm[v] = fmac(dmin * a.d, float(imin), accm[v]);
            }
        }
        for (int v = 0; v < NV; ++v) y[std::size_t(v) * ys + r] = acc[v] - accm[v];
    }
}
template <int NV> static void rows_q6_k(const u8* w, std::size_t rb, std::size_t rows, const act_k* const* av, std::size_t nb, float* y, std::size_t ys) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m30 = _mm256_set1_epi8(0x30);
    for (std::size_t r = 0; r < rows; ++r) {
        const u8* blk = w + r * rb;
        float acc[NV] = {};
        for (std::size_t b = 0; b < nb; ++b, blk += 210) {
            pf(blk, 210);
            const auto* sc = reinterpret_cast<const i8*>(blk + 192);
            __m256i ai[NV];
            for (int v = 0; v < NV; ++v) ai[v] = _mm256_setzero_si256();
            for (int h = 0; h < 2; ++h) {
                const __m256i l0 = ld(blk + 64 * h), l1 = ld(blk + 64 * h + 32), qh = ld(blk + 128 + 32 * h);
                __m256i q[4];
                q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_and_si256(_mm256_slli_epi16(qh, 4), m30));
                q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_and_si256(_mm256_slli_epi16(qh, 2), m30));
                q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_and_si256(qh, m30));
                q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_and_si256(_mm256_srli_epi16(qh, 2), m30));
                for (int i = 0; i < 4; ++i) {
                    const int s = 8 * h + 2 * i;
                    __m256i sv = _mm256_set_m128i(_mm_set1_epi16(sc[s + 1]), _mm_set1_epi16(sc[s]));
                    for (int v = 0; v < NV; ++v)
                        ai[v] = QI8_DPWSSD(ai[v], _mm256_maddubs_epi16(q[i], ld(av[v][b].q + 128 * h + 32 * i)), sv);
                }
            }
            const __m256i s16 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(sc)));
            const float d = h2f_(blk + 208);
            for (int v = 0; v < NV; ++v) {
                const act_k& a = av[v][b];
                std::int32_t off = hsum(_mm256_madd_epi16(s16, ld(a.bsum16)));
                acc[v] = fmac(d * a.d, float(hsum(ai[v]) - 32 * off), acc[v]);
            }
        }
        for (int v = 0; v < NV; ++v) y[std::size_t(v) * ys + r] = acc[v];
    }
}
template <int NV> static void rows_q8_0(const u8* w, std::size_t rb, std::size_t rows, const act_32* const* av, std::size_t nb, float* y, std::size_t ys) {
    for (std::size_t r = 0; r < rows; ++r) {
        const u8* blk = w + r * rb;
        float acc[NV] = {};
        std::size_t b = 0;
        // four blocks at a time: one transposing reduction for their four sums
        for (; b + 4 <= nb; b += 4, blk += 136) {
            pf(blk, 136);
            __m256i wa[4], ws[4];
            for (int i = 0; i < 4; ++i) { ws[i] = ld(blk + 34 * i + 2); wa[i] = _mm256_sign_epi8(ws[i], ws[i]); }
            float dw[4]; for (int i = 0; i < 4; ++i) dw[i] = h2f_(blk + 34 * i);
            for (int v = 0; v < NV; ++v) {
                __m256i p[4];
                for (int i = 0; i < 4; ++i) p[i] = QI8_DPBUSD(_mm256_setzero_si256(), wa[i], _mm256_sign_epi8(ld(av[v][b + i].q), ws[i]));
                __m256i t = _mm256_hadd_epi32(_mm256_hadd_epi32(p[0], p[1]), _mm256_hadd_epi32(p[2], p[3]));
                __m128i s = _mm_add_epi32(_mm256_castsi256_si128(t), _mm256_extracti128_si256(t, 1));   // sums of blocks 0..3
                alignas(16) std::int32_t is[4]; _mm_store_si128(reinterpret_cast<__m128i*>(is), s);
                for (int i = 0; i < 4; ++i) acc[v] = fmac(dw[i] * av[v][b + i].d, float(is[i]), acc[v]);
            }
        }
        for (; b < nb; ++b, blk += 34) {
            __m256i ws = ld(blk + 2), wa = _mm256_sign_epi8(ws, ws);
            float dw = h2f_(blk);
            for (int v = 0; v < NV; ++v)
                acc[v] = fmac(dw * av[v][b].d, float(hsum(QI8_DPBUSD(_mm256_setzero_si256(), wa, _mm256_sign_epi8(ld(av[v][b].q), ws)))), acc[v]);
        }
        for (int v = 0; v < NV; ++v) y[std::size_t(v) * ys + r] = acc[v];
    }
}

template <int NV> static void group(int fmt, const u8* w, std::size_t rb, std::size_t rows, const u8* act, std::size_t as, std::size_t k, float* y, std::size_t ys) {
    switch (qfmt(fmt)) {
        case qfmt::q8_0: { const act_32* p[NV]; for (int v = 0; v < NV; ++v) p[v] = reinterpret_cast<const act_32*>(act + v * as); return rows_q8_0<NV>(w, rb, rows, p, k / 32, y, ys); }
        case qfmt::q4_k: case qfmt::q5_k: case qfmt::q6_k: {
            const act_k* p[NV]; for (int v = 0; v < NV; ++v) p[v] = reinterpret_cast<const act_k*>(act + v * as);
            if (qfmt(fmt) == qfmt::q4_k) return rows_q4_k<NV>(w, rb, rows, p, k / 256, y, ys);
            if (qfmt(fmt) == qfmt::q5_k) return rows_q5_k<NV>(w, rb, rows, p, k / 256, y, ys);
            return rows_q6_k<NV>(w, rb, rows, p, k / 256, y, ys);
        }
        default: return;
    }
}
static void matvec(int fmt, const u8* w, std::size_t rb, std::size_t rows, const void* act, std::size_t as, std::size_t k, std::size_t nv, float* y, std::size_t ys) {
    const u8* a = static_cast<const u8*>(act);
    while (nv) {
        std::size_t n = std::min<std::size_t>(nv, 8);
        switch (n) {
            case 1: group<1>(fmt, w, rb, rows, a, as, k, y, ys); break;
            case 2: group<2>(fmt, w, rb, rows, a, as, k, y, ys); break;
            case 3: group<3>(fmt, w, rb, rows, a, as, k, y, ys); break;
            case 4: group<4>(fmt, w, rb, rows, a, as, k, y, ys); break;
            case 5: group<5>(fmt, w, rb, rows, a, as, k, y, ys); break;
            case 6: group<6>(fmt, w, rb, rows, a, as, k, y, ys); break;
            case 7: group<7>(fmt, w, rb, rows, a, as, k, y, ys); break;
            default: group<8>(fmt, w, rb, rows, a, as, k, y, ys); break;
        }
        nv -= n; a += n * as; y += n * ys;
    }
}

} // namespace forge::simd::qi8::QI8_NS
