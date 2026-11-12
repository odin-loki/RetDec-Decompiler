#pragma once
// forge::core dtypes: f16, bf16, fp8 (E4M3/E5M2), packed int4 — scalar, exhaustively tested.
#include <bit>
#include <complex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace forge {

using i8 = std::int8_t;   using u8 = std::uint8_t;
using i16 = std::int16_t; using u16 = std::uint16_t;
using i32 = std::int32_t; using u32 = std::uint32_t;
using i64 = std::int64_t; using u64 = std::uint64_t;
using f32 = float;        using f64 = double;

enum class rounding : u8 { nearest_even, toward_zero };
enum class overflow : u8 { saturate, to_inf };

namespace detail {
// Generic float32 -> small-float encoder. EXP/MAN bits, BIAS, has_inf (IEEE-like) or not (E4M3 style).
template <int EXP, int MAN, bool HAS_INF>
constexpr u32 encode_small(float f, rounding rm, overflow ov) {
    constexpr int bias = (1 << (EXP - 1)) - 1;
    constexpr u32 sign_shift = EXP + MAN;
    u32 x = std::bit_cast<u32>(f);
    u32 sign = (x >> 31) << sign_shift;
    u32 ax = x & 0x7fffffffu;
    // max finite magnitude encoding
    constexpr u32 max_exp_field = HAS_INF ? ((1u << EXP) - 2) : ((1u << EXP) - 1);
    constexpr u32 max_man = HAS_INF ? ((1u << MAN) - 1) : ((1u << MAN) - 2);
    constexpr u32 max_enc = (max_exp_field << MAN) | max_man;
    constexpr u32 nan_enc = HAS_INF ? (((1u << EXP) - 1) << MAN) | (1u << (MAN - 1)) : ((1u << (EXP + MAN)) - 1);
    if (ax > 0x7f800000u) return sign | nan_enc;
    if (ax == 0x7f800000u) {
        if constexpr (HAS_INF) return sign | (((1u << EXP) - 1) << MAN);
        else return ov == overflow::saturate ? sign | max_enc : sign | nan_enc;
    }
    int e = int(ax >> 23) - 127;      // unbiased
    u32 m = ax & 0x7fffffu;
    if ((ax >> 23) == 0) return sign; // f32 denormal -> 0 (far below any small-float range)
    m |= 0x800000u;                   // implicit 1, 24-bit significand
    int te = e + bias;                // target biased exponent
    int shift;                        // bits to drop from 24-bit significand
    if (te >= 1) shift = 23 - MAN;
    else { shift = 23 - MAN + (1 - te); te = 0; }
    if (shift > 31) return sign;
    u32 q = m >> shift;
    u32 rem = m & ((1u << shift) - 1);
    u32 half = 1u << (shift - 1);
    if (rm == rounding::nearest_even) {
        if (rem > half || (rem == half && (q & 1))) ++q;
    }
    // compose: if normal, q includes implicit bit at position MAN
    u32 enc;
    if (te == 0) enc = q; // subnormal (q may round up into normal: bits carry into exponent correctly)
    else {
        if (q >> (MAN + 1)) { q >>= 1; ++te; }
        enc = (u32(te) << MAN) | (q & ((1u << MAN) - 1));
    }
    if (enc > max_enc) {
        if constexpr (HAS_INF) {
            if (ov == overflow::to_inf) return sign | (((1u << EXP) - 1) << MAN);
        } else {
            if (ov == overflow::to_inf) return sign | nan_enc;
        }
        return sign | max_enc;
    }
    return sign | enc;
}

template <int EXP, int MAN, bool HAS_INF>
constexpr float decode_small(u32 v) {
    constexpr int bias = (1 << (EXP - 1)) - 1;
    u32 sign = (v >> (EXP + MAN)) & 1u;
    u32 e = (v >> MAN) & ((1u << EXP) - 1);
    u32 m = v & ((1u << MAN) - 1);
    float r;
    if (HAS_INF && e == (1u << EXP) - 1) {
        r = m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    } else if (!HAS_INF && e == (1u << EXP) - 1 && m == (1u << MAN) - 1) {
        r = std::numeric_limits<float>::quiet_NaN();
    } else if (e == 0) {
        r = std::ldexp(float(m), 1 - bias - MAN);
    } else {
        r = std::ldexp(float(m | (1u << MAN)), int(e) - bias - MAN);
    }
    return sign ? -r : r;
}
} // namespace detail

struct f16 {
    u16 bits = 0;
    constexpr f16() = default;
    static constexpr f16 from_bits(u16 b) { f16 h; h.bits = b; return h; }
    explicit f16(float f, rounding rm = rounding::nearest_even, overflow ov = overflow::to_inf)
        : bits(u16(detail::encode_small<5, 10, true>(f, rm, ov))) {}
    explicit operator float() const { return detail::decode_small<5, 10, true>(bits); }
    float to_float() const { return float(*this); }
};

struct bf16 {
    u16 bits = 0;
    constexpr bf16() = default;
    static constexpr bf16 from_bits(u16 b) { bf16 h; h.bits = b; return h; }
    explicit bf16(float f, rounding rm = rounding::nearest_even) {
        u32 x = std::bit_cast<u32>(f);
        if ((x & 0x7fffffffu) > 0x7f800000u) { bits = u16((x >> 16) | 0x40); return; }
        if (rm == rounding::nearest_even) x += 0x7fffu + ((x >> 16) & 1u);
        bits = u16(x >> 16);
    }
    explicit operator float() const { return std::bit_cast<float>(u32(bits) << 16); }
    float to_float() const { return float(*this); }
};

// OCP FP8 E4M3 (no inf, max 448, single NaN pattern S.1111.111) and E5M2 (IEEE-like).
struct fp8_e4m3 {
    u8 bits = 0;
    static constexpr fp8_e4m3 from_bits(u8 b) { fp8_e4m3 h; h.bits = b; return h; }
    constexpr fp8_e4m3() = default;
    explicit fp8_e4m3(float f, rounding rm = rounding::nearest_even, overflow ov = overflow::saturate)
        : bits(u8(detail::encode_small<4, 3, false>(f, rm, ov))) {}
    explicit operator float() const { return detail::decode_small<4, 3, false>(bits); }
};
struct fp8_e5m2 {
    u8 bits = 0;
    static constexpr fp8_e5m2 from_bits(u8 b) { fp8_e5m2 h; h.bits = b; return h; }
    constexpr fp8_e5m2() = default;
    explicit fp8_e5m2(float f, rounding rm = rounding::nearest_even, overflow ov = overflow::saturate)
        : bits(u8(detail::encode_small<5, 2, true>(f, rm, ov))) {}
    explicit operator float() const { return detail::decode_small<5, 2, true>(bits); }
};

// Packed 4-bit pair: low nibble = element 0, high nibble = element 1.
struct i4x2 {
    u8 bits = 0;
    static constexpr int lo(u8 b) { return int(i8(u8(b << 4)) >> 4); }
    static constexpr int hi(u8 b) { return int(i8(b) >> 4); }
    static constexpr i4x2 pack(int a, int b) {
        auto sat = [](int v) { return v < -8 ? -8 : (v > 7 ? 7 : v); };
        return i4x2{u8((u8(sat(a)) & 0xf) | (u8(sat(b)) << 4))};
    }
    constexpr int get(int i) const { return i ? hi(bits) : lo(bits); }
};
struct u4x2 {
    u8 bits = 0;
    static constexpr u4x2 pack(unsigned a, unsigned b) {
        auto sat = [](unsigned v) { return v > 15 ? 15u : v; };
        return u4x2{u8(sat(a) | (sat(b) << 4))};
    }
    constexpr unsigned get(int i) const { return i ? (bits >> 4) : (bits & 0xf); }
};

// OCP Microscaling element formats: finite-only (no inf, no NaN), sign-magnitude, bias 2^(E-1)-1.
// E2M1 (FP4, max 6), E2M3 (FP6, max 7.5), E3M2 (FP6, max 28). Round-to-nearest-even, saturating.
template <int EXP, int MAN> struct fn_float {
    static constexpr int bits_n = 1 + EXP + MAN;
    static constexpr int bias = (1 << (EXP - 1)) - 1;
    static constexpr u32 mag_codes = 1u << (EXP + MAN);
    u8 bits = 0;
    static constexpr fn_float from_bits(u8 b) { fn_float h; h.bits = b; return h; }
    static constexpr float pow2(int k) { float r = 1; for (; k > 0; --k) r *= 2; for (; k < 0; ++k) r *= 0.5f; return r; }
    static constexpr float magnitude(u32 c) {
        u32 e = c >> MAN, m = c & ((1u << MAN) - 1);
        return e == 0 ? float(m) * pow2(1 - bias - MAN) : float(m | (1u << MAN)) * pow2(int(e) - bias - MAN);
    }
    static constexpr float max_value() { return magnitude(mag_codes - 1); }
    constexpr fn_float() = default;
    explicit constexpr fn_float(float f) {
        u32 sign = std::bit_cast<u32>(f) >> 31;
        float a = f < 0 ? -f : f;
        if (!(a == a)) { bits = 0; return; }       // NaN has no encoding: map to +0
        u32 best = 0;
        if (a >= max_value()) best = mag_codes - 1;
        else {
            for (u32 c = 1; c < mag_codes; ++c) {
                float lo = magnitude(c - 1), hi = magnitude(c);
                if (a < hi) {
                    float mid = 0.5f * (lo + hi);
                    best = a < mid ? c - 1 : a > mid ? c : ((c - 1) % 2 == 0 ? c - 1 : c);  // ties to even code
                    break;
                }
            }
        }
        bits = u8((sign << (EXP + MAN)) | best);
    }
    explicit constexpr operator float() const {
        float m = magnitude(bits & (mag_codes - 1));
        return (bits >> (EXP + MAN)) & 1 ? -m : m;
    }
};
using fp4_e2m1 = fn_float<2, 1>;
using fp6_e2m3 = fn_float<2, 3>;
using fp6_e3m2 = fn_float<3, 2>;

// Stochastic rounding (A.4): rounds |f| down or up to the neighbouring representable values of T
// with probability proportional to proximity; `r` is a uniform 32-bit random draw (e.g. Philox).
// Unbiased in expectation; saturates at T's max finite value.
template <class T> inline T stochastic_round(float f, u32 r) {
    if (!(f == f)) return T(f);
    T lo(f, rounding::toward_zero);
    float l = float(lo);
    if (l == f || std::isinf(l)) return lo;
    T hi = T::from_bits(decltype(lo.bits)(lo.bits + 1));   // next magnitude (sign-magnitude formats)
    float h = float(hi);
    if (!(h == h) || std::isinf(h)) return lo;            // lo is already the max finite value
    double p = (double(f) - double(l)) / (double(h) - double(l));
    return double(r) * (1.0 / 4294967296.0) < p ? hi : lo;
}

using c32 = std::complex<float>;
using c64 = std::complex<double>;

// Runtime dtype tag used by graph/modelfile. New tags are appended before `invalid` so stored values stay stable.
enum class dtype : u8 { f32, f16, bf16, f64, i8, u8, i16, i32, i64, fp8_e4m3, fp8_e5m2, q8_0, q4_k,
                        q4_g32, q8_g32, mxfp8, mxfp6, mxfp4, mxint8, nvfp4, tq2, q6_k, q5_k, invalid };

constexpr std::string_view name(dtype d) {
    switch (d) {
        case dtype::f32: return "f32"; case dtype::f16: return "f16"; case dtype::bf16: return "bf16";
        case dtype::f64: return "f64"; case dtype::i8: return "i8"; case dtype::u8: return "u8";
        case dtype::i16: return "i16"; case dtype::i32: return "i32"; case dtype::i64: return "i64";
        case dtype::fp8_e4m3: return "fp8_e4m3"; case dtype::fp8_e5m2: return "fp8_e5m2";
        case dtype::q8_0: return "q8_0"; case dtype::q4_k: return "q4_k";
        case dtype::q4_g32: return "q4_g32"; case dtype::q8_g32: return "q8_g32"; case dtype::mxfp8: return "mxfp8";
        case dtype::mxfp6: return "mxfp6"; case dtype::mxfp4: return "mxfp4"; case dtype::mxint8: return "mxint8";
        case dtype::nvfp4: return "nvfp4"; case dtype::tq2: return "tq2";
        case dtype::q6_k: return "q6_k"; case dtype::q5_k: return "q5_k";
        default: return "invalid";
    }
}
constexpr dtype dtype_from_name(std::string_view s) {
    for (int i = 0; i < int(dtype::invalid); ++i)
        if (name(dtype(i)) == s) return dtype(i);
    return dtype::invalid;
}
// Bytes per element for plain dtypes (block codecs report 0; see graph/codecs).
constexpr std::size_t size_of(dtype d) {
    switch (d) {
        case dtype::f32: case dtype::i32: return 4; case dtype::f64: case dtype::i64: return 8;
        case dtype::f16: case dtype::bf16: case dtype::i16: return 2;
        case dtype::i8: case dtype::u8: case dtype::fp8_e4m3: case dtype::fp8_e5m2: return 1;
        default: return 0;
    }
}

} // namespace forge
