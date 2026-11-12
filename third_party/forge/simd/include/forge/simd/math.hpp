#pragma once
// Vectorised libm with accuracy policies. Errors are measured by tests/test_simd_math.cpp.
#include <forge/simd/vec.hpp>
#include <forge/simd/math_coeffs.hpp>

namespace forge::simd {

// Function ids for the dispatched extended-math entry points (kernels::math1 / math2).
enum class fn1 : int { exp2, expm1, log2, log10, log1p, sqrt, rsqrt, cbrt, sin, cos, tan, asin, acos, atan, sinh, cosh, erf, erfc, gelu_erf, softplus, lgamma, count };
enum class fn2 : int { pow, atan2, hypot, count };

struct precise {}; // target <= 1-2 ULP over normal range
struct fast {};    // target <= 4 ULP
struct ml {};      // relative error ~1e-4..1e-3

template <class P> concept Policy = std::is_same_v<P, precise> || std::is_same_v<P, fast> || std::is_same_v<P, ml>;

template <Target Tg> inline vec<float, Tg> clamp(vec<float, Tg> x, float lo, float hi) {
    return min(max(x, splat<Tg>(lo)), splat<Tg>(hi));
}

template <Policy P = precise, Target Tg> inline vec<float, Tg> exp(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    auto nan = isnan(x);
    auto over = lt(splat<Tg>(88.7228394f), x);
    auto under = lt(x, splat<Tg>(-104.0f));
    V x0 = x;
    x = clamp(x, -103.9f, 88.72f);
    V n = round_nearest(x * splat<Tg>(1.44269504088896341f));
    V r = fma(n, splat<Tg>(-0.693359375f), x);
    r = fma(n, splat<Tg>(2.12194440e-4f), r);
    V p;
    if constexpr (std::is_same_v<P, ml>) {
        p = splat<Tg>(4.16666716e-2f);
        p = fma(p, r, splat<Tg>(1.66666672e-1f));
        p = fma(p, r, splat<Tg>(0.5f));
        p = fma(p, r, splat<Tg>(1.0f));
        p = fma(p, r, splat<Tg>(1.0f));
    } else {
        p = splat<Tg>(1.9875691500e-4f);
        p = fma(p, r, splat<Tg>(1.3981999507e-3f));
        p = fma(p, r, splat<Tg>(8.3334519073e-3f));
        p = fma(p, r, splat<Tg>(4.1665795894e-2f));
        p = fma(p, r, splat<Tg>(1.6666665459e-1f));
        p = fma(p, r, splat<Tg>(5.0000001201e-1f));
        p = fma(p, r * r, r + splat<Tg>(1.0f));
    }
    // split scaling so n down to -150 stays representable through subnormals
    V half = round_nearest(n * splat<Tg>(0.5f));
    V y = ldexp_int(ldexp_int(p, half), n - half);
    y = select(over, splat<Tg>(INFINITY), y);
    y = select(under, splat<Tg>(0.0f), y);
    return select(nan, x0, y);
}

// frexp-style split: x = m * 2^e with m in [sqrt(1/2), sqrt(2)). x must be positive and normal.
template <Target Tg> inline void split_exp(vec<float, Tg> x, vec<float, Tg>& m, vec<float, Tg>& e) {
    if constexpr (vec<float, Tg>::K == 2) {
        FORGE_IF512(
        __m512 mm = _mm512_getmant_ps(x.r, _MM_MANT_NORM_1_2, _MM_MANT_SIGN_src);
        __m512 ee = _mm512_getexp_ps(x.r);
        __mmask16 big = _mm512_cmp_ps_mask(mm, _mm512_set1_ps(1.41421356f), _CMP_GT_OQ);
        mm = _mm512_mask_mul_ps(mm, big, mm, _mm512_set1_ps(0.5f));
        ee = _mm512_mask_add_ps(ee, big, ee, _mm512_set1_ps(1.0f));
        m.r = mm; e.r = ee;)
    } else if constexpr (vec<float, Tg>::K == 1) {
        FORGE_IF256(
        __m256i xi = _mm256_castps_si256(x.r);
        __m256i ei = _mm256_sub_epi32(_mm256_srli_epi32(xi, 23), _mm256_set1_epi32(127));
        __m256 mm = _mm256_castsi256_ps(_mm256_or_si256(_mm256_and_si256(xi, _mm256_set1_epi32(0x007fffff)), _mm256_set1_epi32(0x3f800000)));
        __m256 ee = _mm256_cvtepi32_ps(ei);
        __m256 big = _mm256_cmp_ps(mm, _mm256_set1_ps(1.41421356f), _CMP_GT_OQ);
        mm = _mm256_blendv_ps(mm, _mm256_mul_ps(mm, _mm256_set1_ps(0.5f)), big);
        ee = _mm256_blendv_ps(ee, _mm256_add_ps(ee, _mm256_set1_ps(1.0f)), big);
        m.r = mm; e.r = ee;)
    } else if constexpr (vec<float, Tg>::K == 4) {
        FORGE_IFSVE(
        auto pt = svptrue_b32();
        svuint32_t xi = svreinterpret_u32_f32(x.r);
        svfloat32_t ee = svcvt_f32_s32_x(pt, svsub_n_s32_x(pt, svreinterpret_s32_u32(svlsr_n_u32_x(pt, xi, 23)), 127));
        svfloat32_t mm = svreinterpret_f32_u32(svorr_n_u32_x(pt, svand_n_u32_x(pt, xi, 0x007fffff), 0x3f800000));
        svbool_t big = svcmpgt_n_f32(pt, mm, 1.41421356f);
        mm = svsel_f32(big, svmul_n_f32_x(pt, mm, 0.5f), mm);
        ee = svsel_f32(big, svadd_n_f32_x(pt, ee, 1.0f), ee);
        m.r = mm; e.r = ee;)
    } else if constexpr (vec<float, Tg>::K == 5) {
        FORGE_IFRVV(
        vuint32m1_t xi = __riscv_vreinterpret_v_f32m1_u32m1(x.r);
        vfloat32m1_t ee = __riscv_vfcvt_f_x_v_f32m1(__riscv_vsub_vx_i32m1(__riscv_vreinterpret_v_u32m1_i32m1(__riscv_vsrl_vx_u32m1(xi, 23, FORGE_RVV_VL)), 127, FORGE_RVV_VL), FORGE_RVV_VL);
        vfloat32m1_t mm = __riscv_vreinterpret_v_u32m1_f32m1(__riscv_vor_vx_u32m1(__riscv_vand_vx_u32m1(xi, 0x007fffff, FORGE_RVV_VL), 0x3f800000, FORGE_RVV_VL));
        vbool32_t big = __riscv_vmfgt_vf_f32m1_b32(mm, 1.41421356f, FORGE_RVV_VL);
        mm = __riscv_vmerge_vvm_f32m1(mm, __riscv_vfmul_vf_f32m1(mm, 0.5f, FORGE_RVV_VL), big, FORGE_RVV_VL);
        ee = __riscv_vmerge_vvm_f32m1(ee, __riscv_vfadd_vf_f32m1(ee, 1.0f, FORGE_RVV_VL), big, FORGE_RVV_VL);
        m.r = mm; e.r = ee;)
    } else if constexpr (vec<float, Tg>::K == 3) {
        FORGE_IFNEON(
        uint32x4_t xi = vreinterpretq_u32_f32(x.r);
        float32x4_t ee = vcvtq_f32_s32(vsubq_s32(vreinterpretq_s32_u32(vshrq_n_u32(xi, 23)), vdupq_n_s32(127)));
        float32x4_t mm = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(xi, vdupq_n_u32(0x007fffff)), vdupq_n_u32(0x3f800000)));
        uint32x4_t big = vcgtq_f32(mm, vdupq_n_f32(1.41421356f));
        mm = vbslq_f32(big, vmulq_f32(mm, vdupq_n_f32(0.5f)), mm);
        ee = vbslq_f32(big, vaddq_f32(ee, vdupq_n_f32(1.0f)), ee);
        m.r = mm; e.r = ee;)
    } else {
        for (int i = 0; i < vec<float, Tg>::N; ++i) {
            int ex; float mm = std::frexp(x.r.v[i], &ex); mm *= 2; --ex;
            if (mm > 1.41421356f) { mm *= 0.5f; ++ex; }
            m.r.v[i] = mm; e.r.v[i] = float(ex);
        }
    }
}

template <Policy P = precise, Target Tg> inline vec<float, Tg> log(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V m, e;
    auto bad = lt(x, splat<Tg>(1.17549435e-38f));  // <= 0, denormal handled below
    bad = bad | lt(splat<Tg>(3.40282347e38f), x) | isnan(x);
    V xs = select(bad, splat<Tg>(1.0f), x);
    split_exp(xs, m, e);
    V f = m - splat<Tg>(1.0f);
    // log(1+f) = 2 atanh(s), s = f / (2 + f)
    V s = f / (f + splat<Tg>(2.0f));
    V z = s * s;
    V p;
    if constexpr (std::is_same_v<P, ml>) {
        p = fma(z, splat<Tg>(0.4f), splat<Tg>(0.66666667f));
    } else {
        p = splat<Tg>(0.24279078841f);
        p = fma(p, z, splat<Tg>(0.28498786688f));
        p = fma(p, z, splat<Tg>(0.40000972152f));
        p = fma(p, z, splat<Tg>(0.66666662693f));
    }
    V r = fma(s * z, p, s + s); // 2s + 2s^3/3 + ...
    V y = fma(e, splat<Tg>(0.693147180559945f), r);
    if (bad.any()) {
        // slow path for non-normal lanes keeps semantics identical to std::log
        float tx[V::N], ty[V::N]; store(tx, x); store(ty, y);
        for (int i = 0; i < V::N; ++i) if ((bad.bits() >> i) & 1 || !(tx[i] == tx[i])) ty[i] = std::log(tx[i]);
        y = load<Tg>(ty);
    }
    return y;
}

template <Policy P = precise, Target Tg> inline vec<float, Tg> sigmoid(vec<float, Tg> x) {
    auto one = splat<Tg>(1.0f);
    return one / (one + exp<P>(splat<Tg>(0.0f) - x));
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> silu(vec<float, Tg> x) { return x * sigmoid<P>(x); }

template <Policy P = precise, Target Tg> inline vec<float, Tg> tanh(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    // small |x|: odd polynomial (avoids cancellation); else 1 - 2/(e^{2x}+1)
    V ax = max(x, splat<Tg>(0.0f) - x);
    V e2 = exp<P>(clamp(x + x, -88.f, 88.f));
    V big = splat<Tg>(1.0f) - splat<Tg>(2.0f) / (e2 + splat<Tg>(1.0f));
    V z = x * x;
    V p = splat<Tg>(-5.70498872745e-3f);
    p = fma(p, z, splat<Tg>(2.06390887954e-2f));
    p = fma(p, z, splat<Tg>(-5.37397155531e-2f));
    p = fma(p, z, splat<Tg>(1.33314422036e-1f));
    p = fma(p, z, splat<Tg>(-3.33332819422e-1f));
    V lo = fma(p * z, x, x);   // not "small": <rpcndr.h> defines small as char
    return select(lt(ax, splat<Tg>(0.625f)), lo, big);
}

template <Policy P = precise, Target Tg> inline vec<float, Tg> gelu(vec<float, Tg> x) {
    // tanh form is used for ml/fast; precise uses tanh form too but with precise tanh
    auto c = splat<Tg>(0.7978845608028654f);
    auto inner = c * fma(splat<Tg>(0.044715f) * x, x * x, x);
    return splat<Tg>(0.5f) * x * (splat<Tg>(1.0f) + tanh<P>(inner));
}


// ---- extended libm (A.6). All written on the portable vec ops, so every lowering gets them. ----
namespace detail {
template <Target Tg, std::size_t K> inline vec<float, Tg> horner(const float (&c)[K], vec<float, Tg> t) {
    vec<float, Tg> y = splat<Tg>(c[0]);
    for (std::size_t i = 1; i < K; ++i) y = fma(y, t, splat<Tg>(c[i]));
    return y;
}
} // namespace detail

template <Policy P = precise, Target Tg> inline vec<float, Tg> rsqrt(vec<float, Tg> x) { return splat<Tg>(1.0f) / sqrt(x); }

// 2^x: exact split x = n + f, then e^(f ln2) through the exp polynomial
template <Policy P = precise, Target Tg> inline vec<float, Tg> exp2(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    auto nan = isnan(x);
    auto over = lt(splat<Tg>(128.0f), x), under = lt(x, splat<Tg>(-150.0f));
    V xc = clamp(x, -150.f, 128.f);
    V n = round_nearest(xc), f = xc - n;
    V r = f * splat<Tg>(0.693147180559945f);
    V p = splat<Tg>(1.9875691500e-4f);
    p = fma(p, r, splat<Tg>(1.3981999507e-3f));
    p = fma(p, r, splat<Tg>(8.3334519073e-3f));
    p = fma(p, r, splat<Tg>(4.1665795894e-2f));
    p = fma(p, r, splat<Tg>(1.6666665459e-1f));
    p = fma(p, r, splat<Tg>(5.0000001201e-1f));
    p = fma(p, r * r, r + splat<Tg>(1.0f));
    V half = round_nearest(n * splat<Tg>(0.5f));
    V y = ldexp_int(ldexp_int(p, half), n - half);
    y = select(over, splat<Tg>(INFINITY), select(under, splat<Tg>(0.0f), y));
    return select(nan, x, y);
}

// e^x - 1 without cancellation near 0
template <Policy P = precise, Target Tg> inline vec<float, Tg> expm1(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V p = splat<Tg>(2.50521084e-8f);                 // 1/11!
    p = fma(p, x, splat<Tg>(2.75573192e-7f));
    p = fma(p, x, splat<Tg>(2.75573192e-6f));
    p = fma(p, x, splat<Tg>(2.48015873e-5f));
    p = fma(p, x, splat<Tg>(1.98412698e-4f));
    p = fma(p, x, splat<Tg>(1.38888889e-3f));
    p = fma(p, x, splat<Tg>(8.33333333e-3f));
    p = fma(p, x, splat<Tg>(4.16666667e-2f));
    p = fma(p, x, splat<Tg>(1.66666667e-1f));
    p = fma(p, x, splat<Tg>(0.5f));
    V lo = fma(p * x, x, x);
    V big = exp<P>(x) - splat<Tg>(1.0f);
    return select(lt(abs(x), splat<Tg>(0.75f)), lo, big);
}

template <Policy P = precise, Target Tg> inline vec<float, Tg> log2(vec<float, Tg> x) { return log<P>(x) * splat<Tg>(1.44269504088896341f); }
template <Policy P = precise, Target Tg> inline vec<float, Tg> log10(vec<float, Tg> x) { return log<P>(x) * splat<Tg>(0.434294481903251828f); }

// log(1+x) with the (u-1)-x correction so tiny x stays exact
template <Policy P = precise, Target Tg> inline vec<float, Tg> log1p(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V u = x + splat<Tg>(1.0f);
    V d = u - splat<Tg>(1.0f);
    V corr = (x - d) / u;
    auto exact = lt(abs(x), splat<Tg>(1e-30f));
    V safe_u = select(lt(splat<Tg>(0.0f), u), u, splat<Tg>(1.0f));
    V y = log<P>(safe_u) + select(lt(splat<Tg>(0.0f), u), corr, splat<Tg>(0.0f));
    y = select(lt(splat<Tg>(0.0f), u), y, log<P>(u));   // u <= 0 / nan: std semantics via log
    return select(exact, x, y);
}

namespace detail {
// log(x) as hi + lo (double-float) for positive normal x; |error| ~ 2^-40 relative.
template <Target Tg> inline void log_df(vec<float, Tg> x, vec<float, Tg>& hi, vec<float, Tg>& lo) {
    using V = vec<float, Tg>;
    V m, e;
    split_exp(x, m, e);
    V f = m - splat<Tg>(1.0f);                       // exact (Sterbenz)
    V s = f / (f + splat<Tg>(2.0f));
    V z = s * s;
    V R = z * fma(fma(fma(splat<Tg>(0.24279078841f), z, splat<Tg>(0.28498786688f)), z, splat<Tg>(0.40000972152f)), z, splat<Tg>(0.66666662693f));
    V hf = splat<Tg>(0.5f) * f;
    V hfsq = hf * f, hfsq_lo = fma(hf, f, -hfsq);    // 0.5 f^2 exactly
    // log(1+f) = f - hfsq + s*(hfsq + R)
    V h1 = f - hfsq;
    V l1 = (f - h1) - hfsq;
    l1 = l1 - hfsq_lo + s * (hfsq + R);
    // + e*ln2 (ln2_hi has 16 significant bits, so e*ln2_hi is exact)
    V eh = e * splat<Tg>(0.693145751953125f);
    V h2 = eh + h1;
    V bb = h2 - eh;
    V l2 = (eh - (h2 - bb)) + (h1 - bb);             // two-sum
    V lsum = l2 + l1 + e * splat<Tg>(1.42860682030941723212e-6f);
    hi = h2 + lsum;                                  // renormalise so |lo| <= ulp(hi)/2
    lo = lsum - (hi - h2);
}
} // namespace detail

// pow(x, y): log in double-float, product split with fma, exp with a first-order lo correction.
// Non-positive / subnormal / non-finite x take std::pow per lane so edge semantics match libm.
template <Policy P = precise, Target Tg> inline vec<float, Tg> pow(vec<float, Tg> x, vec<float, Tg> y) {
    using V = vec<float, Tg>;
    auto bad = lt(x, splat<Tg>(1.17549435e-38f)) | lt(splat<Tg>(3.40282347e38f), x) | isnan(x) | isnan(y) | lt(splat<Tg>(3.40282347e38f), abs(y));
    V xs = select(bad, splat<Tg>(1.0f), x);
    V lh, ll;
    detail::log_df(xs, lh, ll);
    V ph = y * lh;
    V pl = fma(y, lh, -ph) + y * ll;
    V r = exp<P>(ph);
    r = fma(r, pl, r);
    r = select(lt(splat<Tg>(88.72f), ph), splat<Tg>(INFINITY), r);
    if (bad.any()) {
        float tx[V::N], ty[V::N], tr[V::N]; store(tx, x); store(ty, y); store(tr, r);
        for (int i = 0; i < V::N; ++i) if ((bad.bits() >> i) & 1) tr[i] = std::pow(tx[i], ty[i]);
        r = load<Tg>(tr);
    }
    return r;
}

// cbrt: exp/log seed + one Newton step in float; subnormals scaled by 2^48 first
template <Policy P = precise, Target Tg> inline vec<float, Tg> cbrt(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V a = abs(x);
    auto zero = lt(a, splat<Tg>(1e-45f));
    auto tiny = lt(a, splat<Tg>(1.17549435e-38f));
    V as = select(tiny, a * splat<Tg>(281474976710656.0f), a);
    as = select(zero, splat<Tg>(1.0f), as);
    V y = exp<P>(log<P>(as) * splat<Tg>(1.0f / 3.0f));
    y = y - (y * y * y - as) / (splat<Tg>(3.0f) * y * y);
    y = select(tiny, y * splat<Tg>(1.0f / 65536.0f), y);
    y = select(zero, a, y);
    y = select(lt(splat<Tg>(3.40282347e38f), a) | isnan(x), a, y);
    return select(lt(x, splat<Tg>(0.0f)), -y, y);
}

template <Policy P = precise, Target Tg> inline vec<float, Tg> hypot(vec<float, Tg> x, vec<float, Tg> y) {
    using V = vec<float, Tg>;
    V a = abs(x), b = abs(y);
    V m = max(a, b), n = min(a, b);
    auto mz = lt(m, splat<Tg>(1e-45f));
    V r = n / select(mz, splat<Tg>(1.0f), m);
    V h = m * sqrt(fma(r, r, splat<Tg>(1.0f)));
    h = select(mz, splat<Tg>(0.0f), h);
    auto inf = lt(splat<Tg>(3.40282347e38f), m);
    return select(inf, splat<Tg>(INFINITY), select(isnan(x) | isnan(y), x + y, h));
}

namespace detail {
// x = n*pi/2 + r, |r| <= pi/4; q = n mod 4 in {0,1,2,3}. Accurate for |x| < ~1e5.
template <Target Tg> inline void reduce_pi2(vec<float, Tg> x, vec<float, Tg>& r, vec<float, Tg>& q) {
    using V = vec<float, Tg>;
    V n = round_nearest(x * splat<Tg>(0.636619772367581343f));
    r = fma(n, splat<Tg>(-1.5703125f), x);
    r = fma(n, splat<Tg>(-4.837512969970703125e-4f), r);
    r = fma(n, splat<Tg>(-7.54978995489188216e-8f), r);
    q = n - splat<Tg>(4.0f) * round_nearest(n * splat<Tg>(0.25f) - splat<Tg>(0.375f));
}
template <Target Tg> inline vec<float, Tg> sin_poly(vec<float, Tg> r) {
    auto z = r * r;
    auto p = fma(fma(splat<Tg>(-1.9515295891e-4f), z, splat<Tg>(8.3321608736e-3f)), z, splat<Tg>(-1.6666654611e-1f));
    return fma(p * z, r, r);
}
template <Target Tg> inline vec<float, Tg> cos_poly(vec<float, Tg> r) {
    auto z = r * r;
    auto p = fma(fma(splat<Tg>(2.443315711809948e-5f), z, splat<Tg>(-1.388731625493765e-3f)), z, splat<Tg>(4.166664568298827e-2f));
    return fma(p * z, z, fma(splat<Tg>(-0.5f), z, splat<Tg>(1.0f)));
}
} // namespace detail

template <Policy P = precise, Target Tg> inline void sincos(vec<float, Tg> x, vec<float, Tg>& s, vec<float, Tg>& c) {
    using V = vec<float, Tg>;
    V r, q;
    detail::reduce_pi2(x, r, q);
    V sp = detail::sin_poly(r), cp = detail::cos_poly(r);
    V qc = q + splat<Tg>(1.0f);
    qc = select(lt(splat<Tg>(3.5f), qc), qc - splat<Tg>(4.0f), qc);
    auto odd_s = lt(splat<Tg>(0.5f), q) & lt(q, splat<Tg>(1.5f)), odd_s2 = lt(splat<Tg>(2.5f), q);
    auto odd_c = lt(splat<Tg>(0.5f), qc) & lt(qc, splat<Tg>(1.5f)), odd_c2 = lt(splat<Tg>(2.5f), qc);
    V sv = select(odd_s | odd_s2, cp, sp);
    s = select(lt(splat<Tg>(1.5f), q), -sv, sv);
    V cv = select(odd_c | odd_c2, cp, sp);
    c = select(lt(splat<Tg>(1.5f), qc), -cv, cv);
    auto bad = isnan(x) | lt(splat<Tg>(3.4e38f), abs(x));
    s = select(bad, splat<Tg>(NAN), s);
    c = select(bad, splat<Tg>(NAN), c);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> sin(vec<float, Tg> x) { vec<float, Tg> s, c; sincos<P>(x, s, c); return s; }
template <Policy P = precise, Target Tg> inline vec<float, Tg> cos(vec<float, Tg> x) { vec<float, Tg> s, c; sincos<P>(x, s, c); return c; }
template <Policy P = precise, Target Tg> inline vec<float, Tg> tan(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V r, q;
    detail::reduce_pi2(x, r, q);
    V sp = detail::sin_poly(r), cp = detail::cos_poly(r);
    auto odd = (lt(splat<Tg>(0.5f), q) & lt(q, splat<Tg>(1.5f))) | lt(splat<Tg>(2.5f), q);
    V t = select(odd, -cp / sp, sp / cp);
    return select(isnan(x) | lt(splat<Tg>(3.4e38f), abs(x)), splat<Tg>(NAN), t);
}

namespace detail {
// atan on |x| with cephes range reduction
template <Target Tg> inline vec<float, Tg> atan_pos(vec<float, Tg> a) {
    using V = vec<float, Tg>;
    auto big = lt(splat<Tg>(2.414213562373095f), a);
    auto mid = lt(splat<Tg>(0.4142135623730950f), a);
    V base = select(big, splat<Tg>(1.57079632679489662f), select(mid, splat<Tg>(0.785398163397448310f), splat<Tg>(0.0f)));
    V xr = select(big, splat<Tg>(-1.0f) / a, select(mid, (a - splat<Tg>(1.0f)) / (a + splat<Tg>(1.0f)), a));
    V z = xr * xr;
    V p = fma(fma(fma(splat<Tg>(8.05374449538e-2f), z, splat<Tg>(-1.38776856032e-1f)), z, splat<Tg>(1.99777106478e-1f)), z, splat<Tg>(-3.33329491539e-1f));
    return base + fma(p * z, xr, xr);
}
// asin core for t in [0, 0.5]
template <Target Tg> inline vec<float, Tg> asin_poly(vec<float, Tg> t) {
    auto z = t * t;
    auto p = fma(fma(fma(fma(splat<Tg>(4.2163199048e-2f), z, splat<Tg>(2.4181311049e-2f)), z, splat<Tg>(4.5470025998e-2f)), z, splat<Tg>(7.4953002686e-2f)), z, splat<Tg>(1.6666752422e-1f));
    return fma(p * z, t, t);
}
} // namespace detail

template <Policy P = precise, Target Tg> inline vec<float, Tg> atan(vec<float, Tg> x) {
    auto y = detail::atan_pos(abs(x));
    y = select(lt(splat<Tg>(3.4e38f), abs(x)), splat<Tg>(1.57079632679489662f), y);
    y = select(isnan(x), x, y);
    return select(lt(x, splat<Tg>(0.0f)), -y, y);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> atan2(vec<float, Tg> y, vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V ax = abs(x), ay = abs(y);
    auto swap = lt(ax, ay);
    V num = select(swap, ax, ay), den = select(swap, ay, ax);
    auto dz = lt(den, splat<Tg>(1e-45f));
    V t = detail::atan_pos(num / select(dz, splat<Tg>(1.0f), den));
    t = select(dz, splat<Tg>(0.0f), t);
    t = select(swap, splat<Tg>(1.57079632679489662f) - t, t);
    t = select(lt(x, splat<Tg>(0.0f)), splat<Tg>(3.14159265358979324f) - t, t);
    t = select(lt(y, splat<Tg>(0.0f)), -t, t);
    return select(isnan(x) | isnan(y), x + y, t);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> asin(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V a = abs(x);
    auto hi = lt(splat<Tg>(0.5f), a);
    V t = select(hi, sqrt(splat<Tg>(0.5f) * (splat<Tg>(1.0f) - a)), a);
    V p = detail::asin_poly(t);
    p = select(hi, splat<Tg>(1.57079632679489662f) - (p + p), p);
    p = select(lt(splat<Tg>(1.0f), a) | isnan(x), splat<Tg>(NAN), p);
    return select(lt(x, splat<Tg>(0.0f)), -p, p);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> acos(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    auto pos = lt(splat<Tg>(0.5f), x), neg = lt(x, splat<Tg>(-0.5f));
    V tp = detail::asin_poly(sqrt(splat<Tg>(0.5f) * (splat<Tg>(1.0f) - x)));
    V tn = detail::asin_poly(sqrt(splat<Tg>(0.5f) * (splat<Tg>(1.0f) + x)));
    V mid = splat<Tg>(1.57079632679489662f) - detail::asin_poly(x);
    V y = select(pos, tp + tp, select(neg, splat<Tg>(3.14159265358979324f) - (tn + tn), mid));
    return select(lt(splat<Tg>(1.0f), abs(x)) | isnan(x), splat<Tg>(NAN), y);
}

namespace detail {
// e^a / 2 without overflow up to a ~ 89.4
template <Policy P, Target Tg> inline vec<float, Tg> half_exp(vec<float, Tg> a) {
    auto h = exp<P>(a * splat<Tg>(0.5f));
    return select(lt(a, splat<Tg>(88.0f)), exp<P>(a) * splat<Tg>(0.5f), (h * splat<Tg>(0.5f)) * h);
}
} // namespace detail
template <Policy P = precise, Target Tg> inline vec<float, Tg> sinh(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V a = abs(x);
    V em = expm1<P>(a);
    V lo = splat<Tg>(0.5f) * (em + em / (em + splat<Tg>(1.0f)));
    V e = detail::half_exp<P>(a);
    V big = e - splat<Tg>(0.25f) / e;
    V y = select(lt(a, splat<Tg>(1.0f)), lo, big);
    return select(lt(x, splat<Tg>(0.0f)), -y, y);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> cosh(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V e = detail::half_exp<P>(abs(x));
    return e + splat<Tg>(0.25f) / e;
}

template <Policy P = precise, Target Tg> inline vec<float, Tg> erfc(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V a = abs(x);
    V a2 = a * a, a2lo = fma(a, a, -a2);                 // a^2 = a2 + a2lo exactly
    V g = exp<P>(-a2) * (splat<Tg>(1.0f) - a2lo);
    V r = select(lt(a, splat<Tg>(2.0f)), detail::horner(coeffs::erfc_0, a - splat<Tg>(1.25f)),
          select(lt(a, splat<Tg>(3.5f)), detail::horner(coeffs::erfc_1, a - splat<Tg>(2.75f)),
          select(lt(a, splat<Tg>(6.0f)), detail::horner(coeffs::erfc_2, a - splat<Tg>(4.75f)),
                                         detail::horner(coeffs::erfc_3, a - splat<Tg>(8.0f)))));
    V tail = g * r;
    V lo = splat<Tg>(1.0f) - a * detail::horner(coeffs::erf_small, a2);
    V y = select(lt(a, splat<Tg>(0.5f)), lo, tail);
    y = select(lt(splat<Tg>(10.1f), a), splat<Tg>(0.0f), y);
    y = select(lt(x, splat<Tg>(0.0f)), splat<Tg>(2.0f) - y, y);
    return select(isnan(x), x, y);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> erf(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V a = abs(x);
    V lo = a * detail::horner(coeffs::erf_small, a * a);
    V y = select(lt(a, splat<Tg>(1.0f)), lo, splat<Tg>(1.0f) - erfc<P>(a));
    y = select(isnan(x), x, y);
    return select(lt(x, splat<Tg>(0.0f)), -y, y);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> gelu_erf(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    // negative side through erfc with a first-order correction for rounding x/sqrt(2)
    V t = x * splat<Tg>(0.70710678118654752f);
    V d = fma(x, splat<Tg>(0.70710678118654752f), -t) + x * splat<Tg>(1.21016175e-8f);
    V u = -t;
    V ec = erfc<P>(u) + d * splat<Tg>(1.12837916709551257f) * exp<P>(-(u * u));
    V pos = splat<Tg>(1.0f) + erf<P>(t);
    return splat<Tg>(0.5f) * x * select(lt(x, splat<Tg>(0.0f)), ec, pos);
}
template <Policy P = precise, Target Tg> inline vec<float, Tg> softplus(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    V y = log1p<P>(exp<P>(min(x, splat<Tg>(20.0f))));
    return select(lt(splat<Tg>(20.0f), x), x, y);
}

// lgamma for x > 0 (reflection for x < 0): [1,3) by a fitted polynomial with the roots factored out,
// (0,1) by lgamma(x+1) - log x, [3,8) shifted up by a product, >= 8 Stirling.
template <Policy P = precise, Target Tg> inline vec<float, Tg> lgamma(vec<float, Tg> x) {
    using V = vec<float, Tg>;
    auto negx = lt(x, splat<Tg>(0.0f));
    V xp = select(negx, splat<Tg>(1.0f) - x, x);
    auto lt1 = lt(xp, splat<Tg>(1.0f));
    V z = select(lt1, xp + splat<Tg>(1.0f), xp);
    auto mid = lt(z, splat<Tg>(3.0f));
    V t = select(mid, z, splat<Tg>(2.0f));
    V lm = (t - splat<Tg>(1.0f)) * (t - splat<Tg>(2.0f)) * detail::horner(coeffs::lgamma_12, t - splat<Tg>(2.0f));
    // [3,8): shift down into [2,3) -> lgamma(t) + log((z-1)...(t)); >= 8: Stirling
    auto midr = lt(z, splat<Tg>(8.0f));
    V t2 = select(midr, z, splat<Tg>(2.5f)), pr = splat<Tg>(1.0f);
    for (int i = 0; i < 5; ++i) {
        auto m = lt(splat<Tg>(3.0f), t2) | (lt(splat<Tg>(2.999999f), t2) & lt(t2, splat<Tg>(3.000001f)));
        t2 = select(m, t2 - splat<Tg>(1.0f), t2);
        pr = select(m, pr * t2, pr);
    }
    V ld = (t2 - splat<Tg>(1.0f)) * (t2 - splat<Tg>(2.0f)) * detail::horner(coeffs::lgamma_12, t2 - splat<Tg>(2.0f)) + log<P>(pr);
    V s = select(midr, splat<Tg>(8.0f), z);
    V ls = log<P>(s);
    V is = splat<Tg>(1.0f) / s, is2 = is * is;
    V corr = is * fma(fma(is2, splat<Tg>(1.0f / 1260), splat<Tg>(-1.0f / 360)), is2, splat<Tg>(1.0f / 12));
    V st = fma(s - splat<Tg>(0.5f), ls, splat<Tg>(0.918938533204672742f) - s) + corr;
    st = select(midr, ld, st);
    V y = select(mid, lm, st);
    y = select(lt1, y - log<P>(select(lt1, xp, splat<Tg>(1.0f))), y);
    if (negx.any()) {
        // |sin(pi x)| from the exact fractional part r = x - round(x)
        V r = abs(x - round_nearest(x));
        V pr2 = r * splat<Tg>(3.14159265358979324f);
        V sp = select(lt(r, splat<Tg>(0.25f)), detail::sin_poly(pr2), detail::cos_poly((splat<Tg>(0.5f) - r) * splat<Tg>(3.14159265358979324f)));
        auto pole = lt(r, splat<Tg>(1e-45f));
        V refl = log<P>(splat<Tg>(3.14159265358979324f) / select(pole, splat<Tg>(1.0f), sp)) - y;
        y = select(negx, select(pole, splat<Tg>(INFINITY), refl), y);
    }
    y = select(lt(splat<Tg>(3.4e38f), abs(x)), splat<Tg>(INFINITY), y);
    return select(isnan(x), x, y);
}

} // namespace forge::simd
