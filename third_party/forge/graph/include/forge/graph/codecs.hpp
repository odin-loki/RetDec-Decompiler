#pragma once
// Quantisation codecs: block layout + pack + unpack. Verified against dequantise-then-compute.
#include <forge/core/dtypes.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace forge::graph::codecs {

struct codec {
    std::string_view name;
    dtype tag;
    std::size_t block;        // elements per block
    std::size_t block_bytes;  // bytes per block
    void (*encode)(const float*, std::size_t, u8*);
    void (*decode)(const u8*, std::size_t, float*);
    std::size_t bytes(std::size_t n) const { return (n + block - 1) / block * block_bytes; }
};

namespace detail {
inline void enc_f32(const float* x, std::size_t n, u8* o) { std::memcpy(o, x, n * 4); }
inline void dec_f32(const u8* i, std::size_t n, float* x) { std::memcpy(x, i, n * 4); }
inline void enc_f16(const float* x, std::size_t n, u8* o) { for (std::size_t i = 0; i < n; ++i) { u16 b = f16(x[i]).bits; std::memcpy(o + 2 * i, &b, 2); } }
inline void dec_f16(const u8* in, std::size_t n, float* x) { for (std::size_t i = 0; i < n; ++i) { u16 b; std::memcpy(&b, in + 2 * i, 2); x[i] = float(f16::from_bits(b)); } }
inline void enc_bf16(const float* x, std::size_t n, u8* o) { for (std::size_t i = 0; i < n; ++i) { u16 b = bf16(x[i]).bits; std::memcpy(o + 2 * i, &b, 2); } }
inline void dec_bf16(const u8* in, std::size_t n, float* x) { for (std::size_t i = 0; i < n; ++i) { u16 b; std::memcpy(&b, in + 2 * i, 2); x[i] = float(bf16::from_bits(b)); } }
inline void enc_e4m3(const float* x, std::size_t n, u8* o) { for (std::size_t i = 0; i < n; ++i) o[i] = fp8_e4m3(x[i]).bits; }
inline void dec_e4m3(const u8* in, std::size_t n, float* x) { for (std::size_t i = 0; i < n; ++i) x[i] = float(fp8_e4m3::from_bits(in[i])); }

// Q8_0 (GGUF-compatible): 32 values, f16 scale d + 32 x int8. x = d * q
inline void enc_q8_0(const float* x, std::size_t n, u8* o) {
    for (std::size_t b = 0; b < n; b += 32, o += 34) {
        float amax = 0; std::size_t m = std::min<std::size_t>(32, n - b);
        for (std::size_t i = 0; i < m; ++i) amax = std::max(amax, std::fabs(x[b + i]));
        float d = amax / 127.f; float id = d ? 1.f / d : 0.f;
        u16 hb = f16(d).bits; std::memcpy(o, &hb, 2);
        for (std::size_t i = 0; i < 32; ++i) o[2 + i] = u8(i8(i < m ? std::lround(x[b + i] * id) : 0));
    }
}
inline void dec_q8_0(const u8* in, std::size_t n, float* x) {
    for (std::size_t b = 0; b < n; b += 32, in += 34) {
        u16 hb; std::memcpy(&hb, in, 2); float d = float(f16::from_bits(hb));
        for (std::size_t i = 0; i < 32 && b + i < n; ++i) x[b + i] = d * float(i8(in[2 + i]));
    }
}

// Q4_K (GGUF-compatible layout): super-block of 256 = 8 sub-blocks of 32.
// struct { f16 d; f16 dmin; u8 scales[12]; u8 qs[128]; }  x = d*sc*q - dmin*m
inline void get_scale_min_k4(int j, const u8* q, u8& d, u8& m) {
    if (j < 4) { d = q[j] & 63; m = q[j + 4] & 63; }
    else { d = u8((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4)); m = u8((q[j + 4] >> 4) | ((q[j] >> 6) << 4)); }
}
inline void enc_q4_k(const float* x, std::size_t n, u8* o) {
    for (std::size_t b = 0; b < n; b += 256, o += 144) {
        float blk[256] = {};
        std::copy_n(x + b, std::min<std::size_t>(256, n - b), blk);
        float sc[8], mn[8];
        for (int s = 0; s < 8; ++s) {
            float lo = 0, hi = 0;   // include 0 so min >= 0 offset is representable
            for (int i = 0; i < 32; ++i) { lo = std::min(lo, blk[s * 32 + i]); hi = std::max(hi, blk[s * 32 + i]); }
            sc[s] = (hi - lo) / 15.f; mn[s] = -lo;
        }
        float msc = *std::max_element(sc, sc + 8), mmn = *std::max_element(mn, mn + 8);
        float d = msc / 63.f, dmin = mmn / 63.f;
        u16 hd = f16(d).bits, hm = f16(dmin).bits; std::memcpy(o, &hd, 2); std::memcpy(o + 2, &hm, 2);
        d = float(f16::from_bits(hd)); dmin = float(f16::from_bits(hm));
        u8 ls[8], lm[8];
        for (int s = 0; s < 8; ++s) {
            ls[s] = u8(std::clamp(d ? int(std::lround(sc[s] / d)) : 0, 0, 63));
            lm[s] = u8(std::clamp(dmin ? int(std::lround(mn[s] / dmin)) : 0, 0, 63));
            // make sure the scale covers the range after rounding
            if (d && ls[s] < 63 && ls[s] * d * 15.f < (sc[s] * 15.f) - 1e-6f) ++ls[s];
        }
        u8* scales = o + 4;
        std::memset(scales, 0, 12);
        for (int j = 0; j < 4; ++j) { scales[j] = u8(ls[j] | ((ls[j + 4] >> 4) << 6)); scales[j + 4] = u8(lm[j] | ((lm[j + 4] >> 4) << 6)); }
        for (int j = 4; j < 8; ++j) scales[j + 4] = u8((ls[j] & 0xF) | ((lm[j] & 0xF) << 4));
        u8* qs = o + 16;
        for (int j = 0; j < 256; j += 64) {
            int is = j / 32;
            u8 s1, m1, s2, m2; get_scale_min_k4(is, scales, s1, m1); get_scale_min_k4(is + 1, scales, s2, m2);
            float d1 = d * s1, mm1 = dmin * m1, d2 = d * s2, mm2 = dmin * m2;
            for (int l = 0; l < 32; ++l) {
                int q1 = d1 ? int(std::lround((blk[j + l] + mm1) / d1)) : 0;
                int q2 = d2 ? int(std::lround((blk[j + 32 + l] + mm2) / d2)) : 0;
                qs[j / 2 + l] = u8(std::clamp(q1, 0, 15) | (std::clamp(q2, 0, 15) << 4));
            }
        }
    }
}
inline void dec_q4_k(const u8* in, std::size_t n, float* x) {
    for (std::size_t b = 0; b < n; b += 256, in += 144) {
        u16 hd, hm; std::memcpy(&hd, in, 2); std::memcpy(&hm, in + 2, 2);
        float d = float(f16::from_bits(hd)), dmin = float(f16::from_bits(hm));
        const u8* scales = in + 4; const u8* q = in + 16;
        float out[256];
        int is = 0;
        for (int j = 0; j < 256; j += 64, q += 32, is += 2) {
            u8 sc, m; get_scale_min_k4(is, scales, sc, m);
            float d1 = d * sc, m1 = dmin * m;
            get_scale_min_k4(is + 1, scales, sc, m);
            float d2 = d * sc, m2 = dmin * m;
            for (int l = 0; l < 32; ++l) out[j + l] = d1 * float(q[l] & 0xF) - m1;
            for (int l = 0; l < 32; ++l) out[j + 32 + l] = d2 * float(q[l] >> 4) - m2;
        }
        std::copy_n(out, std::min<std::size_t>(256, n - b), x + b);
    }
}

// FP8 E5M2 per element
inline void enc_e5m2(const float* x, std::size_t n, u8* o) { for (std::size_t i = 0; i < n; ++i) o[i] = fp8_e5m2(x[i]).bits; }
inline void dec_e5m2(const u8* in, std::size_t n, float* x) { for (std::size_t i = 0; i < n; ++i) x[i] = float(fp8_e5m2::from_bits(in[i])); }

inline void put16(u8* o, float v) { u16 b = f16(v).bits; std::memcpy(o, &b, 2); }
inline float get16(const u8* i) { u16 b; std::memcpy(&b, i, 2); return float(f16::from_bits(b)); }

// Group-wise asymmetric weight-only quantisation (AWQ / GPTQ style), group 32:
// { f16 scale; f16 min; q[32] } with x = q * scale + min, q in [0, 2^bits - 1].
template <int BITS> inline void enc_qg(const float* x, std::size_t n, u8* o) {
    constexpr int L = (1 << BITS) - 1;
    constexpr std::size_t BB = 4 + 32 * BITS / 8;
    for (std::size_t b = 0; b < n; b += 32, o += BB) {
        std::size_t m = std::min<std::size_t>(32, n - b);
        float lo = x[b], hi = x[b];
        for (std::size_t i = 0; i < m; ++i) { lo = std::min(lo, x[b + i]); hi = std::max(hi, x[b + i]); }
        float sc = (hi - lo) / float(L);
        float mn = float(f16(lo));                          // the stored min is what decode sees
        float scs = float(f16(sc));
        if (scs * float(L) + mn < hi) scs = float(f16(sc * 1.001f + 1e-7f));
        put16(o, scs); put16(o + 2, mn);
        float id = scs > 0 ? 1.f / scs : 0.f;
        std::memset(o + 4, 0, BB - 4);
        for (std::size_t i = 0; i < m; ++i) {
            int q = int(std::lround((x[b + i] - mn) * id)); q = std::clamp(q, 0, L);
            if constexpr (BITS == 8) o[4 + i] = u8(q);
            else o[4 + i / 2] |= u8(q << (4 * (i & 1)));
        }
    }
}
template <int BITS> inline void dec_qg(const u8* in, std::size_t n, float* x) {
    constexpr std::size_t BB = 4 + 32 * BITS / 8;
    for (std::size_t b = 0; b < n; b += 32, in += BB) {
        float sc = get16(in), mn = get16(in + 2);
        for (std::size_t i = 0; i < 32 && b + i < n; ++i) {
            int q = BITS == 8 ? in[4 + i] : (in[4 + i / 2] >> (4 * (i & 1))) & 0xf;
            x[b + i] = float(q) * sc + mn;
        }
    }
}

// OCP Microscaling (MX v1.0): blocks of 32 elements share an E8M0 scale X (a power of two, bias 127).
// X = floor(log2(amax)) - emax_elem; elements store x / 2^X in the element format.
inline int e8m0_exp(float amax, int emax) {
    if (!(amax > 0) || std::isinf(amax)) return 0;
    int e; std::frexp(amax, &e);                   // amax = f * 2^e, f in [0.5, 1)
    return std::clamp(e - 1 - emax, -127, 127);
}
template <class EL, int EMAX, int ELBITS> inline void enc_mx(const float* x, std::size_t n, u8* o) {
    constexpr std::size_t BB = 1 + 32 * ELBITS / 8;
    for (std::size_t b = 0; b < n; b += 32, o += BB) {
        std::size_t m = std::min<std::size_t>(32, n - b);
        float amax = 0; for (std::size_t i = 0; i < m; ++i) amax = std::max(amax, std::fabs(x[b + i]));
        int X = e8m0_exp(amax, EMAX);
        o[0] = u8(X + 127);
        std::memset(o + 1, 0, BB - 1);
        for (std::size_t i = 0; i < m; ++i) {
            float v = std::ldexp(x[b + i], -X);
            u32 code;
            if constexpr (std::is_same_v<EL, i8>) code = u8(i8(std::clamp<long>(std::lround(v * 64.f), -127, 127)));   // MXINT8: 1.6 fixed point
            else code = EL(v).bits;
            std::size_t bit = i * ELBITS;
            for (int k = 0; k < ELBITS; ++k) if ((code >> k) & 1) o[1 + (bit + k) / 8] |= u8(1u << ((bit + k) % 8));
        }
    }
}
template <class EL, int EMAX, int ELBITS> inline void dec_mx(const u8* in, std::size_t n, float* x) {
    constexpr std::size_t BB = 1 + 32 * ELBITS / 8;
    for (std::size_t b = 0; b < n; b += 32, in += BB) {
        int X = int(in[0]) - 127;
        for (std::size_t i = 0; i < 32 && b + i < n; ++i) {
            u32 code = 0; std::size_t bit = i * ELBITS;
            for (int k = 0; k < ELBITS; ++k) code |= u32((in[1 + (bit + k) / 8] >> ((bit + k) % 8)) & 1) << k;
            float v;
            if constexpr (std::is_same_v<EL, i8>) v = float(i8(u8(code))) / 64.f;
            else v = float(EL::from_bits(u8(code)));
            x[b + i] = std::ldexp(v, X);
        }
    }
}

// NVFP4: blocks of 16 E2M1 elements with an FP8 E4M3 block scale (amax / 6).
// (The optional per-tensor f32 second-level scale is folded to 1 here.)
inline void enc_nvfp4(const float* x, std::size_t n, u8* o) {
    for (std::size_t b = 0; b < n; b += 16, o += 9) {
        std::size_t m = std::min<std::size_t>(16, n - b);
        float amax = 0; for (std::size_t i = 0; i < m; ++i) amax = std::max(amax, std::fabs(x[b + i]));
        fp8_e4m3 sc(amax / 6.f);
        if (float(sc) * 6.f < amax && sc.bits < 0x7e) sc = fp8_e4m3::from_bits(u8(sc.bits + 1));
        o[0] = sc.bits;
        float s = float(sc), is = s > 0 ? 1.f / s : 0.f;
        std::memset(o + 1, 0, 8);
        for (std::size_t i = 0; i < m; ++i) o[1 + i / 2] |= u8(fp4_e2m1(x[b + i] * is).bits << (4 * (i & 1)));
    }
}
inline void dec_nvfp4(const u8* in, std::size_t n, float* x) {
    for (std::size_t b = 0; b < n; b += 16, in += 9) {
        float s = float(fp8_e4m3::from_bits(in[0]));
        for (std::size_t i = 0; i < 16 && b + i < n; ++i) x[b + i] = s * float(fp4_e2m1::from_bits(u8((in[1 + i / 2] >> (4 * (i & 1))) & 0xf)));
    }
}

// Ternary (BitNet b1.58 style): blocks of 64, f16 absmean scale, values in {-1, 0, +1} packed 2 bits each.
inline void enc_tq2(const float* x, std::size_t n, u8* o) {
    for (std::size_t b = 0; b < n; b += 64, o += 18) {
        std::size_t m = std::min<std::size_t>(64, n - b);
        double sa = 0; for (std::size_t i = 0; i < m; ++i) sa += std::fabs(x[b + i]);
        float g0 = float(sa / double(m)), ig = g0 > 0 ? 1.f / g0 : 0.f;
        int q[64] = {};
        double nz = 0, cnt = 0;
        for (std::size_t i = 0; i < m; ++i) {
            q[i] = std::clamp(int(std::lround(x[b + i] * ig)), -1, 1);   // absmean quantisation (BitNet b1.58)
            if (q[i]) { nz += std::fabs(x[b + i]); cnt += 1; }
        }
        put16(o, float(cnt > 0 ? nz / cnt : 0.0));                       // least-squares scale for the chosen signs
        std::memset(o + 2, 0, 16);
        for (std::size_t i = 0; i < m; ++i) o[2 + i / 4] |= u8(u8(q[i] + 1) << (2 * (i & 3)));
    }
}
inline void dec_tq2(const u8* in, std::size_t n, float* x) {
    for (std::size_t b = 0; b < n; b += 64, in += 18) {
        float g = get16(in);
        for (std::size_t i = 0; i < 64 && b + i < n; ++i) x[b + i] = g * float(int((in[2 + i / 4] >> (2 * (i & 3))) & 3) - 1);
    }
}

// Q6_K (GGUF-compatible layout): super-block of 256 = 16 sub-blocks of 16.
// struct { u8 ql[128]; u8 qh[64]; i8 scales[16]; f16 d; }  x = d * scales[s] * (q - 32), q in [0, 63]
inline void dec_q6_k(const u8* in, std::size_t n, float* x) {
    for (std::size_t b = 0; b < n; b += 256, in += 210) {
        const u8* ql = in; const u8* qh = in + 128; const i8* sc = reinterpret_cast<const i8*>(in + 192);
        float d = get16(in + 208), out[256];
        for (int h = 0; h < 2; ++h, ql += 64, qh += 32, sc += 8) {
            float* y = out + 128 * h;
            for (int l = 0; l < 32; ++l) {
                int is = l / 16;
                int q1 = int((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = int((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = int((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = int((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                y[l] = d * sc[is] * q1; y[l + 32] = d * sc[is + 2] * q2;
                y[l + 64] = d * sc[is + 4] * q3; y[l + 96] = d * sc[is + 6] * q4;
            }
        }
        std::copy_n(out, std::min<std::size_t>(256, n - b), x + b);
    }
}
inline void enc_q6_k(const float* x, std::size_t n, u8* o) {
    for (std::size_t b = 0; b < n; b += 256, o += 210) {
        float blk[256] = {};
        std::copy_n(x + b, std::min<std::size_t>(256, n - b), blk);
        float ss[16], amax = 0;
        for (int s = 0; s < 16; ++s) {
            float m = 0; for (int i = 0; i < 16; ++i) m = std::max(m, std::fabs(blk[s * 16 + i]));
            ss[s] = m / 31.f; amax = std::max(amax, ss[s]);
        }
        float d = amax / 127.f; put16(o + 208, d); d = get16(o + 208);
        i8 sc[16]; int q[256];
        for (int s = 0; s < 16; ++s) {
            int v = d ? int(std::lround(ss[s] / d)) : 0;
            if (d && v < 127 && v * d * 31.f < ss[s] * 31.f - 1e-6f) ++v;   // cover the range after rounding
            sc[s] = i8(std::clamp(v, 0, 127));
            float ds = d * sc[s];
            for (int i = 0; i < 16; ++i) q[s * 16 + i] = std::clamp(ds ? int(std::lround(blk[s * 16 + i] / ds)) : 0, -32, 31) + 32;
        }
        std::memcpy(o + 192, sc, 16);
        u8* ql = o; u8* qh = o + 128; std::memset(o, 0, 192);
        for (int h = 0; h < 2; ++h) {
            const int* qq = q + 128 * h; u8* L = ql + 64 * h; u8* H = qh + 32 * h;
            for (int l = 0; l < 32; ++l) {
                L[l] = u8((qq[l] & 0xF) | ((qq[l + 64] & 0xF) << 4));
                L[l + 32] = u8((qq[l + 32] & 0xF) | ((qq[l + 96] & 0xF) << 4));
                H[l] = u8((qq[l] >> 4) | ((qq[l + 32] >> 4) << 2) | ((qq[l + 64] >> 4) << 4) | ((qq[l + 96] >> 4) << 6));
            }
        }
    }
}

// Q5_K (GGUF-compatible layout): super-block of 256 = 8 sub-blocks of 32.
// struct { f16 d; f16 dmin; u8 scales[12]; u8 qh[32]; u8 qs[128]; }  x = d*sc*q - dmin*m, q in [0, 31]
inline void dec_q5_k(const u8* in, std::size_t n, float* x) {
    for (std::size_t b = 0; b < n; b += 256, in += 176) {
        float d = get16(in), dmin = get16(in + 2);
        const u8* scales = in + 4; const u8* qh = in + 16; const u8* ql = in + 48;
        float out[256]; int is = 0; u8 u1 = 1, u2 = 2;
        for (int j = 0; j < 256; j += 64, ql += 32, is += 2, u1 <<= 2, u2 <<= 2) {
            u8 sc, m; get_scale_min_k4(is, scales, sc, m);
            float d1 = d * sc, m1 = dmin * m;
            get_scale_min_k4(is + 1, scales, sc, m);
            float d2 = d * sc, m2 = dmin * m;
            for (int l = 0; l < 32; ++l) out[j + l] = d1 * float((ql[l] & 0xF) + (qh[l] & u1 ? 16 : 0)) - m1;
            for (int l = 0; l < 32; ++l) out[j + 32 + l] = d2 * float((ql[l] >> 4) + (qh[l] & u2 ? 16 : 0)) - m2;
        }
        std::copy_n(out, std::min<std::size_t>(256, n - b), x + b);
    }
}
inline void enc_q5_k(const float* x, std::size_t n, u8* o) {
    for (std::size_t b = 0; b < n; b += 256, o += 176) {
        float blk[256] = {};
        std::copy_n(x + b, std::min<std::size_t>(256, n - b), blk);
        float sc[8], mn[8];
        for (int s = 0; s < 8; ++s) {
            float lo = 0, hi = 0;
            for (int i = 0; i < 32; ++i) { lo = std::min(lo, blk[s * 32 + i]); hi = std::max(hi, blk[s * 32 + i]); }
            sc[s] = (hi - lo) / 31.f; mn[s] = -lo;
        }
        float d = *std::max_element(sc, sc + 8) / 63.f, dmin = *std::max_element(mn, mn + 8) / 63.f;
        put16(o, d); put16(o + 2, dmin); d = get16(o); dmin = get16(o + 2);
        u8 ls[8], lm[8];
        for (int s = 0; s < 8; ++s) {
            ls[s] = u8(std::clamp(d ? int(std::lround(sc[s] / d)) : 0, 0, 63));
            lm[s] = u8(std::clamp(dmin ? int(std::lround(mn[s] / dmin)) : 0, 0, 63));
            if (d && ls[s] < 63 && ls[s] * d * 31.f < (sc[s] * 31.f) - 1e-6f) ++ls[s];
        }
        u8* scales = o + 4; std::memset(scales, 0, 12);
        for (int j = 0; j < 4; ++j) { scales[j] = u8(ls[j] | ((ls[j + 4] >> 4) << 6)); scales[j + 4] = u8(lm[j] | ((lm[j + 4] >> 4) << 6)); }
        for (int j = 4; j < 8; ++j) scales[j + 4] = u8((ls[j] & 0xF) | ((lm[j] & 0xF) << 4));
        u8* qh = o + 16; u8* qs = o + 48; std::memset(qh, 0, 32);
        u8 u1 = 1, u2 = 2;
        for (int j = 0, is = 0; j < 256; j += 64, is += 2, u1 <<= 2, u2 <<= 2) {
            u8 s1, m1, s2, m2; get_scale_min_k4(is, scales, s1, m1); get_scale_min_k4(is + 1, scales, s2, m2);
            float d1 = d * s1, mm1 = dmin * m1, d2 = d * s2, mm2 = dmin * m2;
            for (int l = 0; l < 32; ++l) {
                int q1 = std::clamp(d1 ? int(std::lround((blk[j + l] + mm1) / d1)) : 0, 0, 31);
                int q2 = std::clamp(d2 ? int(std::lround((blk[j + 32 + l] + mm2) / d2)) : 0, 0, 31);
                qs[j / 2 + l] = u8((q1 & 0xF) | ((q2 & 0xF) << 4));
                if (q1 & 16) qh[l] |= u1;
                if (q2 & 16) qh[l] |= u2;
            }
        }
    }
}
} // namespace detail

inline const std::vector<codec>& all() {
    static const std::vector<codec> c = {
        {"f32", dtype::f32, 1, 4, detail::enc_f32, detail::dec_f32},
        {"f16", dtype::f16, 1, 2, detail::enc_f16, detail::dec_f16},
        {"bf16", dtype::bf16, 1, 2, detail::enc_bf16, detail::dec_bf16},
        {"fp8_e4m3", dtype::fp8_e4m3, 1, 1, detail::enc_e4m3, detail::dec_e4m3},
        {"q8_0", dtype::q8_0, 32, 34, detail::enc_q8_0, detail::dec_q8_0},
        {"q4_k", dtype::q4_k, 256, 144, detail::enc_q4_k, detail::dec_q4_k},
        {"fp8_e5m2", dtype::fp8_e5m2, 1, 1, detail::enc_e5m2, detail::dec_e5m2},
        {"q4_g32", dtype::q4_g32, 32, 20, detail::enc_qg<4>, detail::dec_qg<4>},
        {"q8_g32", dtype::q8_g32, 32, 36, detail::enc_qg<8>, detail::dec_qg<8>},
        {"mxfp8", dtype::mxfp8, 32, 33, detail::enc_mx<fp8_e4m3, 8, 8>, detail::dec_mx<fp8_e4m3, 8, 8>},
        {"mxfp6", dtype::mxfp6, 32, 25, detail::enc_mx<fp6_e2m3, 2, 6>, detail::dec_mx<fp6_e2m3, 2, 6>},
        {"mxfp4", dtype::mxfp4, 32, 17, detail::enc_mx<fp4_e2m1, 2, 4>, detail::dec_mx<fp4_e2m1, 2, 4>},
        {"mxint8", dtype::mxint8, 32, 33, detail::enc_mx<i8, 0, 8>, detail::dec_mx<i8, 0, 8>},
        {"nvfp4", dtype::nvfp4, 16, 9, detail::enc_nvfp4, detail::dec_nvfp4},
        {"tq2", dtype::tq2, 64, 18, detail::enc_tq2, detail::dec_tq2},
        {"q6_k", dtype::q6_k, 256, 210, detail::enc_q6_k, detail::dec_q6_k},
        {"q5_k", dtype::q5_k, 256, 176, detail::enc_q5_k, detail::dec_q5_k},
    };
    return c;
}
inline const codec* find(std::string_view n) {
    for (auto& c : all()) if (c.name == n) return &c;
    return nullptr;
}
inline const codec* find(dtype t) {
    for (auto& c : all()) if (c.tag == t) return &c;
    return nullptr;
}

} // namespace forge::graph::codecs
