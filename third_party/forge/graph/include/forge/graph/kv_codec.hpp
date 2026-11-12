#pragma once
// KV-cache codecs. A quantised state slot stores each (row, head) as one float32 scale word followed by
// ceil(d/4) words holding d packed bytes (little-endian, byte e of the head in word e/4, bits 8*(e%4)).
// The slot's physical view is therefore [len, heads, 1 + ceil(d/4)] floats; attention kernels (CPU and
// Vulkan) decode on the fly. int8: symmetric, scale = amax/127. fp8: E4M3 (saturating), scale = amax/448.
//
// TurboQuant (arXiv 2504.19874; llama.cpp tbq3_0/tbq4_0): tbq4 / tbq3. Each head (padded with zeros to
// dp = next power of two >= d) is rotated by R = H*D -- D a fixed pseudo-random +-1 diagonal (kv_tbq_sign),
// H the orthonormal Walsh-Hadamard transform -- which makes every rotated coordinate ~ N(0, |x|^2/dp).
// Each coordinate is then scalar-quantised with the Lloyd-Max codebook for N(0,1) (16 or 8 levels) scaled
// by one f32 gain per head (least-squares fit after picking codes). Layout per (row, head): the gain word,
// then dp codes, 8 per word (tbq4, 4 bits each) or 10 per word (tbq3, 3 bits each, top 2 bits zero).
// Since R is orthonormal, q.k = (Rq).(Rk): GPU attention rotates q once and dots it against the codebook
// values directly, and accumulates V in the rotated domain, applying R^T once per output row.
// The optional 1-bit QJL residual from the paper is not implemented (MSE-optimal codes only).
// Mixed "k8v4": keys int8, values tbq4 (kv_codec_pair_from_name).
#include <forge/core/dtypes.hpp>
#include <forge/core/error.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace forge::graph {

// tbq4p / tbq3p: TurboQuant_prod - a (b-1)-bit MSE stage plus a 1-bit QJL sketch of the residual (CPU only).
enum class kv_codec : int { f32 = 0, int8 = 1, fp8 = 2, tbq4 = 3, tbq3 = 4, tbq4p = 5, tbq3p = 6 };
inline bool kv_is_qjl(kv_codec c) { return c == kv_codec::tbq4p || c == kv_codec::tbq3p; }
inline bool kv_is_tbq(kv_codec c) { return c == kv_codec::tbq4 || c == kv_codec::tbq3 || kv_is_qjl(c); }

// (key codec, value codec) for a codec name; "k8v4" is int8 keys + tbq4 values, anything else is uniform.
struct kv_codec_pair { kv_codec k = kv_codec::f32, v = kv_codec::f32; };
inline kv_codec_pair kv_codec_pair_from_name(const std::string& s) {
    if (s.empty() || s == "f32" || s == "f16" || s == "none") return {};
    if (s == "int8" || s == "i8" || s == "q8") return {kv_codec::int8, kv_codec::int8};
    if (s == "fp8" || s == "fp8_e4m3" || s == "e4m3") return {kv_codec::fp8, kv_codec::fp8};
    if (s == "tbq4" || s == "tbq4_0" || s == "turbo4") return {kv_codec::tbq4, kv_codec::tbq4};
    if (s == "tbq3" || s == "tbq3_0" || s == "turbo3") return {kv_codec::tbq3, kv_codec::tbq3};
    if (s == "tbq4p" || s == "tbq4_qjl" || s == "turbo4p") return {kv_codec::tbq4p, kv_codec::tbq4p};
    if (s == "tbq3p" || s == "tbq3_qjl" || s == "turbo3p") return {kv_codec::tbq3p, kv_codec::tbq3p};
    if (s == "k8v4") return {kv_codec::int8, kv_codec::tbq4};
    raise(errc::invalid_argument, "unknown KV codec '{}' (f32, int8, fp8, tbq4, tbq3, tbq4p, tbq3p, k8v4)", s);
}
inline kv_codec kv_codec_from_name(const std::string& s) {
    auto p = kv_codec_pair_from_name(s);
    if (p.k != p.v) raise(errc::invalid_argument, "KV codec '{}' is mixed; use kv_codec_pair_from_name", s);
    return p.k;
}
inline const char* kv_codec_name(kv_codec c) {
    switch (c) { case kv_codec::int8: return "int8"; case kv_codec::fp8: return "fp8"; case kv_codec::tbq4: return "tbq4";
                 case kv_codec::tbq3: return "tbq3"; case kv_codec::tbq4p: return "tbq4p"; case kv_codec::tbq3p: return "tbq3p";
                 default: return "f32"; }
}
inline std::string kv_codec_pair_name(kv_codec_pair p) {
    if (p.k == kv_codec::int8 && p.v == kv_codec::tbq4) return "k8v4";
    return p.k == p.v ? kv_codec_name(p.k) : std::string(kv_codec_name(p.k)) + "+" + kv_codec_name(p.v);
}
inline i64 kv_tbq_dim(i64 d) { i64 p = 1; while (p < d) p <<= 1; return p; }
// words per (row, head) of a quantised slot; the one-argument form is the int8/fp8 layout
inline i64 kv_head_words(i64 d) { return 1 + (d + 3) / 4; }
inline i64 kv_head_words(i64 d, kv_codec c) {
    if (c == kv_codec::f32) return d;
    if (c == kv_codec::tbq4) return 1 + (kv_tbq_dim(d) + 7) / 8;
    if (c == kv_codec::tbq3) return 1 + (kv_tbq_dim(d) + 9) / 10;
    if (c == kv_codec::tbq4p) return 2 + (kv_tbq_dim(d) + 9) / 10 + (kv_tbq_dim(d) + 31) / 32;
    if (c == kv_codec::tbq3p) return 2 + (kv_tbq_dim(d) + 15) / 16 + (kv_tbq_dim(d) + 31) / 32;
    return kv_head_words(d);
}
// bits of the MSE (Lloyd-Max) stage
inline int kv_tbq_bits(kv_codec c) { return c == kv_codec::tbq4 ? 4 : c == kv_codec::tbq3 || c == kv_codec::tbq4p ? 3 : 2; }
inline int kv_tbq_per_word(kv_codec c) { int b = kv_tbq_bits(c); return b == 4 ? 8 : b == 3 ? 10 : 16; }
// Lloyd-Max reconstruction levels for N(0,1), ascending (code i -> level i). GPU kernels carry the same tables.
inline const float* kv_tbq_levels(kv_codec c) {
    static const float l4[16] = {-2.7326f, -2.0690f, -1.6181f, -1.2562f, -0.9424f, -0.6568f, -0.3881f, -0.1284f,
                                  0.1284f,  0.3881f,  0.6568f,  0.9424f,  1.2562f,  1.6181f,  2.0690f,  2.7326f};
    static const float l3[8] = {-2.1520f, -1.3439f, -0.7560f, -0.2451f, 0.2451f, 0.7560f, 1.3439f, 2.1520f};
    static const float l2[4] = {-1.5104f, -0.4528f, 0.4528f, 1.5104f};
    int b = kv_tbq_bits(c);
    return b == 4 ? l4 : b == 3 ? l3 : l2;
}
// fixed pseudo-random sign of coordinate i (integer hash; identical in every kernel language)
inline float kv_tbq_sign(std::uint32_t i) {
    std::uint32_t h = i * 0x9E3779B1u; h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (h & 1u) ? -1.f : 1.f;
}
// in-place orthonormal Walsh-Hadamard transform (n a power of two); self-inverse
inline void kv_fwht(float* a, i64 n) {
    for (i64 len = 1; len < n; len <<= 1)
        for (i64 i = 0; i < n; i += len << 1)
            for (i64 j = i; j < i + len; ++j) { float u = a[j], w = a[j + len]; a[j] = u + w; a[j + len] = u - w; }
    float r = 1.f / std::sqrt(float(n));
    for (i64 i = 0; i < n; ++i) a[i] *= r;
}
inline void kv_tbq_encode(const float* x, i64 d, kv_codec c, float* dst) {
    i64 dp = kv_tbq_dim(d); int L = 1 << kv_tbq_bits(c), bits = kv_tbq_bits(c), per = kv_tbq_per_word(c);
    const float* lv = kv_tbq_levels(c);
    float yb[512]; std::vector<float> yh; float* y = yb;
    if (dp > 512) { yh.resize(std::size_t(dp)); y = yh.data(); }
    for (i64 e = 0; e < dp; ++e) y[e] = e < d ? x[e] * kv_tbq_sign(std::uint32_t(e)) : 0.f;
    kv_fwht(y, dp);
    double r2 = 0; for (i64 e = 0; e < dp; ++e) r2 += double(y[e]) * y[e];
    float s0 = float(std::sqrt(r2 / double(dp)));
    const i64 h = kv_is_qjl(c) ? 2 : 1;
    i64 nw = kv_head_words(d, c) - h;
    auto* w = reinterpret_cast<std::uint32_t*>(dst + h);
    std::memset(w, 0, std::size_t(nw) * 4);
    std::vector<int> codes(kv_is_qjl(c) ? std::size_t(dp) : 0);
    double num = 0, den = 0;
    float inv = s0 > 0 ? 1.f / s0 : 0.f;
    for (i64 e = 0; e < dp; ++e) {
        float u = y[e] * inv;
        int lo = 0, hi = L - 1;   // nearest level: binary search on the midpoints
        while (lo < hi) { int mid = (lo + hi) / 2; if (u > 0.5f * (lv[mid] + lv[mid + 1])) lo = mid + 1; else hi = mid; }
        num += double(y[e]) * lv[lo]; den += double(lv[lo]) * lv[lo];
        w[e / per] |= std::uint32_t(lo) << (bits * (e % per));
        if (!codes.empty()) codes[std::size_t(e)] = lo;
    }
    float g = den > 0 ? float(num / den) : 0.f;
    std::memcpy(dst, &g, 4);
    if (kv_is_qjl(c)) {   // QJL (Zandieh et al.): sign bits of S r, S = H D2 (a second random-sign Hadamard), plus |r|
        std::vector<float> r(static_cast<std::size_t>(dp));
        double rn = 0;
        for (i64 e = 0; e < dp; ++e) { r[e] = (y[e] - g * lv[codes[e]]) * kv_tbq_sign(std::uint32_t(e) + 0x10000u); rn += double(r[e]) * r[e]; }
        kv_fwht(r.data(), dp);
        float rnf = float(std::sqrt(rn)); std::memcpy(dst + 1, &rnf, 4);
        auto* sb = w + (dp + per - 1) / per;
        for (i64 e = 0; e < dp; ++e) if (r[e] >= 0.f) sb[e / 32] |= 1u << (e % 32);
    }
}
inline void kv_tbq_decode(const float* src, i64 d, kv_codec c, float* out) {
    i64 dp = kv_tbq_dim(d); int bits = kv_tbq_bits(c), per = kv_tbq_per_word(c); std::uint32_t mask = (1u << bits) - 1u;
    const float* lv = kv_tbq_levels(c);
    float g; std::memcpy(&g, src, 4);
    const i64 h = kv_is_qjl(c) ? 2 : 1;
    auto* w = reinterpret_cast<const std::uint32_t*>(src + h);
    float zb[512]; std::vector<float> zh; float* z = zb;
    if (dp > 512) { zh.resize(std::size_t(dp)); z = zh.data(); }
    for (i64 e = 0; e < dp; ++e) z[e] = lv[(w[e / per] >> (bits * (e % per))) & mask] * g;
    if (kv_is_qjl(c)) {   // unbiased residual estimate: sqrt(pi/2) |r| / sqrt(n) * S^T sign(S r)
        float rn; std::memcpy(&rn, src + 1, 4);
        auto* sb = w + (dp + per - 1) / per;
        std::vector<float> t(static_cast<std::size_t>(dp));
        for (i64 e = 0; e < dp; ++e) t[e] = (sb[e / 32] >> (e % 32)) & 1u ? 1.f : -1.f;
        kv_fwht(t.data(), dp);
        float k = 1.2533141373155003f * rn / std::sqrt(float(dp));
        for (i64 e = 0; e < dp; ++e) z[e] += k * t[e] * kv_tbq_sign(std::uint32_t(e) + 0x10000u);
    }
    kv_fwht(z, dp);
    for (i64 e = 0; e < d; ++e) out[e] = z[e] * kv_tbq_sign(std::uint32_t(e));
}

// Exact E4M3 decode by bit arithmetic (the GLSL kernel uses the same formula, so CPU and GPU agree bitwise).
inline float kv_fp8_decode(std::uint32_t b) {
    std::uint32_t e = (b >> 3) & 15u, m = b & 7u;
    float v = e == 0 ? std::ldexp(float(m), -9) : std::ldexp(float(8u + m), int(e) - 10);
    return (b & 0x80u) ? -v : v;
}

// Encode one head of d floats into kv_head_words(d) words at dst.
inline void kv_encode_head(const float* x, i64 d, kv_codec c, float* dst) {
    if (kv_is_tbq(c)) { kv_tbq_encode(x, d, c, dst); return; }
    float amax = 0; for (i64 e = 0; e < d; ++e) amax = std::max(amax, std::fabs(x[e]));
    float scale = amax > 0 ? amax / (c == kv_codec::int8 ? 127.f : 448.f) : 1.f;
    std::memcpy(dst, &scale, 4);
    auto* w = reinterpret_cast<std::uint32_t*>(dst + 1);
    std::memset(w, 0, std::size_t((d + 3) / 4) * 4);
    float inv = 1.f / scale;
    for (i64 e = 0; e < d; ++e) {
        std::uint32_t byte;
        if (c == kv_codec::int8) byte = std::uint32_t(std::uint8_t(std::int8_t(std::clamp(std::lround(x[e] * inv), -127l, 127l))));
        else { byte = fp8_e4m3(x[e] * inv).bits; if ((byte & 0x7fu) == 0x7fu) byte -= 1; }   // never emit NaN
        w[e / 4] |= byte << (8 * (e % 4));
    }
}
inline void kv_decode_head(const float* src, i64 d, kv_codec c, float* out) {
    if (kv_is_tbq(c)) { kv_tbq_decode(src, d, c, out); return; }
    float scale; std::memcpy(&scale, src, 4);
    auto* w = reinterpret_cast<const std::uint32_t*>(src + 1);
    for (i64 e = 0; e < d; ++e) {
        std::uint32_t b = (w[e / 4] >> (8 * (e % 4))) & 0xffu;
        out[e] = (c == kv_codec::int8 ? float(std::int8_t(b)) : kv_fp8_decode(b)) * scale;
    }
}

} // namespace forge::graph
