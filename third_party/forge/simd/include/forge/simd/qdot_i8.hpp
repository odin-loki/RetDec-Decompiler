#pragma once
// Quantised matrix-vector products on int8 activations (integer dot products).
//
// The activation vector is quantised once per product into blocks of int8 values with one f32 scale each
// (act_k: 256 values, for the 256-weight K-quant super-blocks; act_32: 32 values, for q8_0). For every weight
// block the kernels form EXACT int32 sums (the K-quant 6-bit sub-scales folded in), and then run the same
// one or two float steps per block, in the same order, on every ISA:
//     acc  = fma(d_w * d_a, float(isum), acc)       accm = fma(dmin_w * d_a, float(imin), accm)
//     y    = acc - accm
// Integer sums do not depend on the order they were formed in, so the scalar reference and the AVX2, AVX-VNNI
// and AVX-512 VNNI kernels give identical bits, and a vector's result does not depend on how many other
// vectors were computed with it (nv = 1..N).
//
// qi8_kernels is a dispatch table separate from simd::kernels: it is chosen once from the CPU's integer
// dot-product support (avx512vnni > avxvnni > avx2 > scalar), FORGE_QI8_TARGET overrides.
#include <forge/simd/qdot.hpp>
#include <forge/core/fma.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace forge::simd {

namespace qi8 {
using u8 = std::uint8_t; using i8 = std::int8_t; using i16 = std::int16_t;
// 256 activations: int8 values, int16 sums of each 16 (q6_k offsets) and each 32 (q4_k / q5_k mins), one scale.
struct alignas(32) act_k { i8 q[256]; i16 bsum16[16]; i16 bsum32[8]; float d; };
// 32 activations (q8_0 weights: one scale per 32 so small values next to large ones keep their precision).
struct act_32 { float d; i8 q[32]; };
static_assert(sizeof(act_k) == 320 && sizeof(act_32) == 36);

inline bool supported(int fmt, std::size_t k) {
    switch (qfmt(fmt)) {
        case qfmt::q8_0: return k % 32 == 0 && k > 0;
        case qfmt::q4_k: case qfmt::q5_k: case qfmt::q6_k: return k % 256 == 0 && k > 0;
        default: return false;
    }
}
// bytes of one quantised activation vector of length k (fmt is the WEIGHT format it will be multiplied with)
inline std::size_t act_bytes(int fmt, std::size_t k) {
    return qfmt(fmt) == qfmt::q8_0 ? k / 32 * sizeof(act_32) : k / 256 * sizeof(act_k);
}
inline std::size_t block_bytes(int fmt) {
    switch (qfmt(fmt)) { case qfmt::q8_0: return 34; case qfmt::q4_k: return 144; case qfmt::q5_k: return 176; case qfmt::q6_k: return 210; default: return 0; }
}

// ---- scalar reference: the definition every SIMD path must match bit for bit ----
inline float h2f(const u8* p) { std::uint16_t b; std::memcpy(&b, p, 2); return float(f16::from_bits(b)); }
inline float fmac(float a, float b, float c) { return forge::fmaf_exact(a, b, c); }   // one rounding on every compiler (MinGW's std::fma rounds twice)
// round-to-nearest-even of x*id, clamped to [-127, 127] (NaN -> -127, the value cvtps2dq+packs gives)
inline i8 q8_round(float v) {
    float r = std::nearbyint(v);
    return r >= 127.f ? i8(127) : r > -127.f ? i8(int(r)) : i8(-127);
}
inline void quant_k_ref(const float* x, std::size_t k, act_k* o) {
    for (std::size_t b = 0; b < k / 256; ++b, x += 256, ++o) {
        float amax = 0; for (int i = 0; i < 256; ++i) amax = std::fmax(amax, std::fabs(x[i]));
        float d = amax / 127.f, id = d != 0.f ? 1.f / d : 0.f;
        for (int i = 0; i < 256; ++i) o->q[i] = q8_round(x[i] * id);
        for (int s = 0; s < 16; ++s) { int t = 0; for (int i = 0; i < 16; ++i) t += o->q[16 * s + i]; o->bsum16[s] = i16(t); }
        for (int s = 0; s < 8; ++s) o->bsum32[s] = i16(o->bsum16[2 * s] + o->bsum16[2 * s + 1]);
        o->d = d;
    }
}
inline void quant_32_ref(const float* x, std::size_t k, act_32* o) {
    for (std::size_t b = 0; b < k / 32; ++b, x += 32, ++o) {
        float amax = 0; for (int i = 0; i < 32; ++i) amax = std::fmax(amax, std::fabs(x[i]));
        float d = amax / 127.f, id = d != 0.f ? 1.f / d : 0.f;
        for (int i = 0; i < 32; ++i) o->q[i] = q8_round(x[i] * id);
        o->d = d;
    }
}
// K-quant 6-bit sub-scales and mins (q4_k / q5_k): 12 bytes -> sc[8], mn[8]
inline void unpack_scales(const u8* s, u8* sc, u8* mn) {
    std::uint32_t a, b, c; std::memcpy(&a, s, 4); std::memcpy(&b, s + 4, 4); std::memcpy(&c, s + 8, 4);
    std::uint32_t s0 = a & 0x3f3f3f3fu, m0 = b & 0x3f3f3f3fu;
    std::uint32_t s1 = (c & 0x0f0f0f0fu) | (((a >> 6) & 0x03030303u) << 4);
    std::uint32_t m1 = ((c >> 4) & 0x0f0f0f0fu) | (((b >> 6) & 0x03030303u) << 4);
    std::memcpy(sc, &s0, 4); std::memcpy(sc + 4, &s1, 4); std::memcpy(mn, &m0, 4); std::memcpy(mn + 4, &m1, 4);
}
inline float dot_q4_k_ref(const u8* w, const act_k* a, std::size_t nb) {
    float acc = 0, accm = 0;
    for (std::size_t b = 0; b < nb; ++b, w += 144, ++a) {
        u8 sc[8], mn[8]; unpack_scales(w + 4, sc, mn);
        const u8* q = w + 16; std::int32_t isum = 0, imin = 0;
        for (int j = 0; j < 4; ++j, q += 32) {
            std::int32_t lo = 0, hi = 0;
            for (int l = 0; l < 32; ++l) { lo += (q[l] & 0xF) * a->q[64 * j + l]; hi += (q[l] >> 4) * a->q[64 * j + 32 + l]; }
            isum += sc[2 * j] * lo + sc[2 * j + 1] * hi;
        }
        for (int s = 0; s < 8; ++s) imin += mn[s] * a->bsum32[s];
        acc = fmac(h2f(w) * a->d, float(isum), acc);
        accm = fmac(h2f(w + 2) * a->d, float(imin), accm);
    }
    return acc - accm;
}
inline float dot_q5_k_ref(const u8* w, const act_k* a, std::size_t nb) {
    float acc = 0, accm = 0;
    for (std::size_t b = 0; b < nb; ++b, w += 176, ++a) {
        u8 sc[8], mn[8]; unpack_scales(w + 4, sc, mn);
        const u8* qh = w + 16; const u8* q = w + 48; std::int32_t isum = 0, imin = 0;
        for (int j = 0; j < 4; ++j, q += 32) {
            std::int32_t lo = 0, hi = 0;
            for (int l = 0; l < 32; ++l) {
                lo += ((q[l] & 0xF) | (((qh[l] >> (2 * j)) & 1) << 4)) * a->q[64 * j + l];
                hi += ((q[l] >> 4) | (((qh[l] >> (2 * j + 1)) & 1) << 4)) * a->q[64 * j + 32 + l];
            }
            isum += sc[2 * j] * lo + sc[2 * j + 1] * hi;
        }
        for (int s = 0; s < 8; ++s) imin += mn[s] * a->bsum32[s];
        acc = fmac(h2f(w) * a->d, float(isum), acc);
        accm = fmac(h2f(w + 2) * a->d, float(imin), accm);
    }
    return acc - accm;
}
inline float dot_q6_k_ref(const u8* w, const act_k* a, std::size_t nb) {
    float acc = 0;
    for (std::size_t b = 0; b < nb; ++b, w += 210, ++a) {
        const u8* ql = w; const u8* qh = w + 128; const auto* sc = reinterpret_cast<const i8*>(w + 192);
        std::int32_t sub[16] = {};
        for (int h = 0; h < 2; ++h)
            for (int l = 0; l < 32; ++l) {
                const u8 L0 = ql[64 * h + l], L1 = ql[64 * h + 32 + l], H = qh[32 * h + l];
                const i8* aq = a->q + 128 * h;
                sub[8 * h + l / 16 + 0] += ((L0 & 0xF) | ((H & 3) << 4)) * aq[l];
                sub[8 * h + l / 16 + 2] += ((L1 & 0xF) | (((H >> 2) & 3) << 4)) * aq[32 + l];
                sub[8 * h + l / 16 + 4] += ((L0 >> 4) | (((H >> 4) & 3) << 4)) * aq[64 + l];
                sub[8 * h + l / 16 + 6] += ((L1 >> 4) | ((H >> 6) << 4)) * aq[96 + l];
            }
        std::int32_t isum = 0, off = 0;
        for (int s = 0; s < 16; ++s) { isum += sc[s] * sub[s]; off += sc[s] * a->bsum16[s]; }
        acc = fmac(h2f(w + 208) * a->d, float(isum - 32 * off), acc);
    }
    return acc;
}
inline float dot_q8_0_ref(const u8* w, const act_32* a, std::size_t nb) {
    float acc = 0;
    for (std::size_t b = 0; b < nb; ++b, w += 34, ++a) {
        std::int32_t isum = 0;
        for (int i = 0; i < 32; ++i) isum += i8(w[2 + i]) * a->q[i];
        acc = fmac(h2f(w) * a->d, float(isum), acc);
    }
    return acc;
}
} // namespace qi8

struct qi8_kernels {
    const char* target;
    // Quantise one activation vector of length k for products with `fmt` weights (qi8::act_bytes(fmt, k) bytes).
    void (*quantize)(int fmt, const float* x, std::size_t k, void* out);
    // y[v * y_stride + r] = row r . activation v, for r < rows and v < nv. Rows are `row_bytes` apart from w,
    // activation vectors `act_stride` bytes apart. Weights are read once per group of up to 8 vectors.
    void (*matvec)(int fmt, const std::uint8_t* w, std::size_t row_bytes, std::size_t rows, const void* act,
                   std::size_t act_stride, std::size_t k, std::size_t nv, float* y, std::size_t y_stride);
};
const qi8_kernels& qi8_active();                 // best for this CPU (FORGE_QI8_TARGET overrides)
const qi8_kernels* qi8_by_name(const char* t);   // "scalar", "avx2", "avxvnni", "avx512vnni"; nullptr if unusable
// Software prefetch distance of the matvec kernels in bytes (FORGE_QI8_PF, default 2048; 0 disables).
extern int qi8_prefetch_bytes;

} // namespace forge::simd
