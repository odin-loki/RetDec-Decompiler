#pragma once
// Tier P: portable fixed-width vectors and masks over native registers.
// Native code paths are only instantiated in TUs compiled with the matching -m flags (see simd/src).
#include <forge/core/dtypes.hpp>
#include <forge/core/fma.hpp>
#include <forge/simd/caps.hpp>
#include <cmath>
#include <cstddef>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define FORGE_IFNEON(...) __VA_ARGS__
#else
#define FORGE_IFNEON(...)
#endif

// Scalable vectors are used at a fixed length per translation unit (-msve-vector-bits=B / -mrvv-vector-bits=B,
// B in 128..1024) so they can live in structs. One kernel table is built per length (tags sve_vl<B>, rvv_vl<B>)
// and the dispatcher picks the table matching the hardware VL/VLEN at run time.
#if defined(__ARM_FEATURE_SVE) && defined(__ARM_FEATURE_SVE_BITS) && __ARM_FEATURE_SVE_BITS >= 128 && __ARM_FEATURE_SVE_BITS <= 1024
#include <arm_sve.h>
#define FORGE_HAVE_SVE_FIXED __ARM_FEATURE_SVE_BITS
#define FORGE_IFSVE(...) __VA_ARGS__
typedef svfloat32_t forge_sve_f __attribute__((arm_sve_vector_bits(__ARM_FEATURE_SVE_BITS)));
typedef svbool_t forge_sve_b __attribute__((arm_sve_vector_bits(__ARM_FEATURE_SVE_BITS)));
#else
#define FORGE_IFSVE(...)
#endif
#if defined(__riscv_v) && defined(__riscv_v_fixed_vlen) && __riscv_v_fixed_vlen >= 128 && __riscv_v_fixed_vlen <= 1024
#include <riscv_vector.h>
#define FORGE_HAVE_RVV_FIXED __riscv_v_fixed_vlen
#define FORGE_RVV_VL (__riscv_v_fixed_vlen / 32)
#define FORGE_IFRVV(...) __VA_ARGS__
typedef vfloat32m1_t forge_rvv_f __attribute__((riscv_rvv_vector_bits(__riscv_v_fixed_vlen)));
typedef vbool32_t forge_rvv_b __attribute__((riscv_rvv_vector_bits(__riscv_v_fixed_vlen / 32)));
#else
#define FORGE_IFRVV(...)
#endif

namespace forge::simd {

template <class T, Target Tg> struct vec;
template <class T, Target Tg> struct mask;

// ---------------- scalar emulation (any target) ----------------
template <int N> struct femu { float v[N]; };
template <int N> struct iemu { i32 v[N]; };

namespace impl {
template <Target Tg> constexpr int kind() {   // 5 = rvv (fixed VLEN), 4 = sve (fixed VL), 3 = neon, 2 = avx512, 1 = avx2, 0 = emulated
#if defined(FORGE_HAVE_SVE_FIXED)
    if constexpr (Has<Tg, sve> && vl_bits<Tg>() == FORGE_HAVE_SVE_FIXED) return 4;
#endif
#if defined(FORGE_HAVE_RVV_FIXED)
    if constexpr (Has<Tg, rvv> && vl_bits<Tg>() == FORGE_HAVE_RVV_FIXED) return 5;
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
    if constexpr (Has<Tg, neon>) return 3;
#endif
#if defined(__AVX512F__)
    if constexpr (Has<Tg, avx512f>) return 2;
#endif
#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))   // MSVC /arch:AVX2 implies FMA3 but never defines __FMA__
    if constexpr (Has<Tg, avx2> && Has<Tg, fma3>) return 1;
#endif
    return 0;
}
} // namespace impl

template <Target Tg> struct mask<float, Tg> {
    static constexpr int K = impl::kind<Tg>();
    static constexpr int N = native_f32_lanes<Tg>() > 4 && K == 0 ? 4 : native_f32_lanes<Tg>();
#if defined(__AVX512F__)
    using native_t = std::conditional_t<K == 2, __mmask16,
#else
    using native_t = std::conditional_t<false, int,
#endif
#if defined(__AVX2__)
                     std::conditional_t<K == 1, __m256, u32>>;
#elif defined(FORGE_HAVE_SVE_FIXED)
                     std::conditional_t<K == 4, forge_sve_b, std::conditional_t<K == 3, uint32x4_t, u32>>>;
#elif defined(__aarch64__) && defined(__ARM_NEON)
                     std::conditional_t<K == 3, uint32x4_t, u32>>;
#elif defined(FORGE_HAVE_RVV_FIXED)
                     std::conditional_t<K == 5, forge_rvv_b, u32>>;
#else
                     u32>;
#endif
    native_t m;
    static mask first_n(std::size_t n) {
        mask r;
        if constexpr (K == 2) {
#if defined(__AVX512F__)
            r.m = __mmask16(n >= 16 ? 0xffffu : ((1u << n) - 1u));
#endif
        } else if constexpr (K == 1) {
#if defined(__AVX2__)
            __m256i idx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
            r.m = _mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_set1_epi32(int(n > 8 ? 8 : n)), idx));
#endif
        } else if constexpr (K == 4) {
            FORGE_IFSVE(r.m = svwhilelt_b32_u64(0, std::uint64_t(n));)
        } else if constexpr (K == 5) {
            FORGE_IFRVV(r.m = __riscv_vmsltu_vx_u32m1_b32(__riscv_vid_v_u32m1(FORGE_RVV_VL), unsigned(n > FORGE_RVV_VL ? FORGE_RVV_VL : n), FORGE_RVV_VL);)
        } else if constexpr (K == 3) {
            FORGE_IFNEON(const uint32_t idx[4] = {0, 1, 2, 3}; r.m = vcltq_u32(vld1q_u32(idx), vdupq_n_u32(uint32_t(n > 4 ? 4 : n)));)
        } else {
            r.m = n >= u32(N) ? ((1u << N) - 1u) : ((1u << n) - 1u);
        }
        return r;
    }
    static mask all() { return first_n(N); }
    unsigned bits() const {
        if constexpr (K == 2) return unsigned(m);
#if defined(__AVX2__)
        else if constexpr (K == 1) return unsigned(_mm256_movemask_ps(m));
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
        else if constexpr (K == 3) { const uint32_t w[4] = {1, 2, 4, 8}; return unsigned(vaddvq_u32(vandq_u32(m, vld1q_u32(w)))); }
#endif
#if defined(FORGE_HAVE_SVE_FIXED)
        else if constexpr (K == 4) return unsigned(svaddv_u32(m, svlsl_u32_x(svptrue_b32(), svdup_n_u32(1), svindex_u32(0, 1))));
#endif
#if defined(FORGE_HAVE_RVV_FIXED)
        else if constexpr (K == 5) {
            auto w = __riscv_vsll_vv_u32m1(__riscv_vmv_v_x_u32m1(1, FORGE_RVV_VL), __riscv_vid_v_u32m1(FORGE_RVV_VL), FORGE_RVV_VL);
            auto z = __riscv_vmv_s_x_u32m1(0, 1);
            return unsigned(__riscv_vmv_x_s_u32m1_u32(__riscv_vredsum_vs_u32m1_u32m1_m(m, w, z, FORGE_RVV_VL)));
        }
#endif
        else return unsigned(m);
    }
    bool any() const { return bits() != 0; }
    bool all_set() const { return bits() == (N == 32 ? ~0u : ((1u << N) - 1u)); }
    bool none() const { return bits() == 0; }
    int count() const { return __builtin_popcount(bits()); }
    int first() const { auto b = bits(); return b ? __builtin_ctz(b) : -1; }
};

template <Target Tg> struct vec<float, Tg> {
    static constexpr int K = impl::kind<Tg>();
    static constexpr int N = mask<float, Tg>::N;
    using mask_t = mask<float, Tg>;
#if defined(__AVX512F__)
    using native_t = std::conditional_t<K == 2, __m512,
#else
    using native_t = std::conditional_t<false, int,
#endif
#if defined(__AVX2__)
                     std::conditional_t<K == 1, __m256, femu<N>>>;
#elif defined(FORGE_HAVE_SVE_FIXED)
                     std::conditional_t<K == 4, forge_sve_f, std::conditional_t<K == 3, float32x4_t, femu<N>>>>;
#elif defined(__aarch64__) && defined(__ARM_NEON)
                     std::conditional_t<K == 3, float32x4_t, femu<N>>>;
#elif defined(FORGE_HAVE_RVV_FIXED)
                     std::conditional_t<K == 5, forge_rvv_f, femu<N>>>;
#else
                     femu<N>>;
#endif
    native_t r;
    native_t native() const { return r; }
    static vec from_native(native_t n) { vec v; v.r = n; return v; }
    static constexpr int size() { return N; }
};

#define FORGE_VOP(name, a512, a256, aneon, scalar_expr)                                   \
    template <Target Tg> inline vec<float, Tg> name(vec<float, Tg> a, vec<float, Tg> b) { \
        vec<float, Tg> o;                                                                 \
        if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = forge_sve_##name(a.r, b.r);) } \
        else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = forge_rvv_##name(a.r, b.r);) } \
        else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = aneon(a.r, b.r);) }    \
        else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = a512(a.r, b.r);) } \
        else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = a256(a.r, b.r);) } \
        else for (int i = 0; i < vec<float, Tg>::N; ++i) { float x = a.r.v[i], y = b.r.v[i]; o.r.v[i] = (scalar_expr); } \
        return o;                                                                         \
    }
#if defined(__AVX512F__)
#define FORGE_IF512(...) __VA_ARGS__
#else
#define FORGE_IF512(...)
#endif
#if defined(__AVX2__)
#define FORGE_IF256(...) __VA_ARGS__
#else
#define FORGE_IF256(...)
#endif

#if defined(__aarch64__) && defined(__ARM_NEON)
// x86 max/min semantics (return the second operand when unordered), so every target agrees with the oracle
inline float32x4_t forge_neon_max(float32x4_t a, float32x4_t b) { return vbslq_f32(vcgtq_f32(a, b), a, b); }
inline float32x4_t forge_neon_min(float32x4_t a, float32x4_t b) { return vbslq_f32(vcltq_f32(a, b), a, b); }
#endif
#if defined(FORGE_HAVE_SVE_FIXED)
#define FORGE_SVE_PT svptrue_b32()
inline forge_sve_f forge_sve_add(forge_sve_f a, forge_sve_f b) { return svadd_f32_x(FORGE_SVE_PT, a, b); }
inline forge_sve_f forge_sve_sub(forge_sve_f a, forge_sve_f b) { return svsub_f32_x(FORGE_SVE_PT, a, b); }
inline forge_sve_f forge_sve_mul(forge_sve_f a, forge_sve_f b) { return svmul_f32_x(FORGE_SVE_PT, a, b); }
inline forge_sve_f forge_sve_div(forge_sve_f a, forge_sve_f b) { return svdiv_f32_x(FORGE_SVE_PT, a, b); }
inline forge_sve_f forge_sve_max(forge_sve_f a, forge_sve_f b) { return svsel_f32(svcmpgt_f32(FORGE_SVE_PT, a, b), a, b); }
inline forge_sve_f forge_sve_min(forge_sve_f a, forge_sve_f b) { return svsel_f32(svcmplt_f32(FORGE_SVE_PT, a, b), a, b); }
#endif
#if defined(FORGE_HAVE_RVV_FIXED)
inline forge_rvv_f forge_rvv_add(forge_rvv_f a, forge_rvv_f b) { return __riscv_vfadd_vv_f32m1(a, b, FORGE_RVV_VL); }
inline forge_rvv_f forge_rvv_sub(forge_rvv_f a, forge_rvv_f b) { return __riscv_vfsub_vv_f32m1(a, b, FORGE_RVV_VL); }
inline forge_rvv_f forge_rvv_mul(forge_rvv_f a, forge_rvv_f b) { return __riscv_vfmul_vv_f32m1(a, b, FORGE_RVV_VL); }
inline forge_rvv_f forge_rvv_div(forge_rvv_f a, forge_rvv_f b) { return __riscv_vfdiv_vv_f32m1(a, b, FORGE_RVV_VL); }
inline forge_rvv_f forge_rvv_max(forge_rvv_f a, forge_rvv_f b) { return __riscv_vmerge_vvm_f32m1(b, a, __riscv_vmfgt_vv_f32m1_b32(a, b, FORGE_RVV_VL), FORGE_RVV_VL); }
inline forge_rvv_f forge_rvv_min(forge_rvv_f a, forge_rvv_f b) { return __riscv_vmerge_vvm_f32m1(b, a, __riscv_vmflt_vv_f32m1_b32(a, b, FORGE_RVV_VL), FORGE_RVV_VL); }
#endif
FORGE_VOP(add, _mm512_add_ps, _mm256_add_ps, vaddq_f32, x + y)
FORGE_VOP(sub, _mm512_sub_ps, _mm256_sub_ps, vsubq_f32, x - y)
FORGE_VOP(mul, _mm512_mul_ps, _mm256_mul_ps, vmulq_f32, x * y)
FORGE_VOP(div, _mm512_div_ps, _mm256_div_ps, vdivq_f32, x / y)
FORGE_VOP(max, _mm512_max_ps, _mm256_max_ps, forge_neon_max, x > y ? x : y)
FORGE_VOP(min, _mm512_min_ps, _mm256_min_ps, forge_neon_min, x < y ? x : y)

template <Target Tg> inline vec<float, Tg> operator+(vec<float, Tg> a, vec<float, Tg> b) { return add(a, b); }
template <Target Tg> inline vec<float, Tg> operator-(vec<float, Tg> a, vec<float, Tg> b) { return sub(a, b); }
template <Target Tg> inline vec<float, Tg> operator*(vec<float, Tg> a, vec<float, Tg> b) { return mul(a, b); }
template <Target Tg> inline vec<float, Tg> operator/(vec<float, Tg> a, vec<float, Tg> b) { return div(a, b); }

template <Target Tg> inline vec<float, Tg> splat(float s) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svdup_n_f32(s);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = __riscv_vfmv_v_f_f32m1(s, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = vdupq_n_f32(s);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_set1_ps(s);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = _mm256_set1_ps(s);) }
    else for (auto& x : o.r.v) x = s;
    return o;
}
template <Target Tg> inline vec<float, Tg> fma(vec<float, Tg> a, vec<float, Tg> b, vec<float, Tg> c) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svmla_f32_x(FORGE_SVE_PT, c.r, a.r, b.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = __riscv_vfmacc_vv_f32m1(c.r, a.r, b.r, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = vfmaq_f32(c.r, a.r, b.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_fmadd_ps(a.r, b.r, c.r);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = _mm256_fmadd_ps(a.r, b.r, c.r);) }
    else for (int i = 0; i < vec<float, Tg>::N; ++i) o.r.v[i] = forge::fmaf_exact(a.r.v[i], b.r.v[i], c.r.v[i]);
    return o;
}
template <Target Tg> inline vec<float, Tg> load(const float* p) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svld1_f32(FORGE_SVE_PT, p);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = __riscv_vle32_v_f32m1(p, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = vld1q_f32(p);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_loadu_ps(p);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = _mm256_loadu_ps(p);) }
    else std::memcpy(o.r.v, p, sizeof(o.r.v));
    return o;
}
// Masked load: lanes outside the mask read as 0 and never touch memory.
template <Target Tg> inline vec<float, Tg> load(const float* p, mask<float, Tg> m) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svld1_f32(m.m, p);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = __riscv_vle32_v_f32m1_mu(m.m, __riscv_vfmv_v_f_f32m1(0.f, FORGE_RVV_VL), p, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(for (int i = 0; i < 4; ++i) o.r[i] = (m.bits() >> i) & 1 ? p[i] : 0.f;) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_maskz_loadu_ps(m.m, p);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = _mm256_maskload_ps(p, _mm256_castps_si256(m.m));) }
    else for (int i = 0; i < vec<float, Tg>::N; ++i) o.r.v[i] = (m.m >> i) & 1 ? p[i] : 0.f;
    return o;
}
template <Target Tg> inline void store(float* p, vec<float, Tg> v) {
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(svst1_f32(FORGE_SVE_PT, p, v.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(__riscv_vse32_v_f32m1(p, v.r, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(vst1q_f32(p, v.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(_mm512_storeu_ps(p, v.r);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(_mm256_storeu_ps(p, v.r);) }
    else std::memcpy(p, v.r.v, sizeof(v.r.v));
}
template <Target Tg> inline void store(float* p, vec<float, Tg> v, mask<float, Tg> m) {
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(svst1_f32(m.m, p, v.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(__riscv_vse32_v_f32m1_m(m.m, p, v.r, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(for (int i = 0; i < 4; ++i) if ((m.bits() >> i) & 1) p[i] = v.r[i];) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(_mm512_mask_storeu_ps(p, m.m, v.r);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(_mm256_maskstore_ps(p, _mm256_castps_si256(m.m), v.r);) }
    else for (int i = 0; i < vec<float, Tg>::N; ++i) if ((m.m >> i) & 1) p[i] = v.r.v[i];
}
// select(m, a, b): m ? a : b
template <Target Tg> inline vec<float, Tg> select(mask<float, Tg> m, vec<float, Tg> a, vec<float, Tg> b) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svsel_f32(m.m, a.r, b.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = __riscv_vmerge_vvm_f32m1(b.r, a.r, m.m, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = vbslq_f32(m.m, a.r, b.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_mask_blend_ps(m.m, b.r, a.r);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = _mm256_blendv_ps(b.r, a.r, m.m);) }
    else for (int i = 0; i < vec<float, Tg>::N; ++i) o.r.v[i] = (m.m >> i) & 1 ? a.r.v[i] : b.r.v[i];
    return o;
}
template <Target Tg> inline mask<float, Tg> lt(vec<float, Tg> a, vec<float, Tg> b) {
    mask<float, Tg> m;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(m.m = svcmplt_f32(FORGE_SVE_PT, a.r, b.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(m.m = __riscv_vmflt_vv_f32m1_b32(a.r, b.r, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(m.m = vcltq_f32(a.r, b.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(m.m = _mm512_cmp_ps_mask(a.r, b.r, _CMP_LT_OQ);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(m.m = _mm256_cmp_ps(a.r, b.r, _CMP_LT_OQ);) }
    else { m.m = 0; for (int i = 0; i < vec<float, Tg>::N; ++i) m.m |= u32(a.r.v[i] < b.r.v[i]) << i; }
    return m;
}
template <Target Tg> inline mask<float, Tg> isnan(vec<float, Tg> a) {
    mask<float, Tg> m;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(m.m = svcmpuo_f32(FORGE_SVE_PT, a.r, a.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(m.m = __riscv_vmfne_vv_f32m1_b32(a.r, a.r, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(m.m = vmvnq_u32(vceqq_f32(a.r, a.r));) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(m.m = _mm512_cmp_ps_mask(a.r, a.r, _CMP_UNORD_Q);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(m.m = _mm256_cmp_ps(a.r, a.r, _CMP_UNORD_Q);) }
    else { m.m = 0; for (int i = 0; i < vec<float, Tg>::N; ++i) m.m |= u32(a.r.v[i] != a.r.v[i]) << i; }
    return m;
}
template <Target Tg> inline mask<float, Tg> operator|(mask<float, Tg> a, mask<float, Tg> b) {
    mask<float, Tg> m;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(m.m = svorr_b_z(FORGE_SVE_PT, a.m, b.m);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(m.m = __riscv_vmor_mm_b32(a.m, b.m, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(m.m = vorrq_u32(a.m, b.m);) }
    else if constexpr (vec<float, Tg>::K == 2) m.m = a.m | b.m;
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(m.m = _mm256_or_ps(a.m, b.m);) }
    else m.m = a.m | b.m;
    return m;
}
template <Target Tg> inline float reduce_add(vec<float, Tg> v) {
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(return svaddv_f32(FORGE_SVE_PT, v.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(return __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredusum_vs_f32m1_f32m1(v.r, __riscv_vfmv_s_f_f32m1(0.f, 1), FORGE_RVV_VL));) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(return vaddvq_f32(v.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(return _mm512_reduce_add_ps(v.r);) }
    else if constexpr (vec<float, Tg>::K == 1) {
        FORGE_IF256(__m128 lo = _mm_add_ps(_mm256_castps256_ps128(v.r), _mm256_extractf128_ps(v.r, 1));
                    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
                    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 1));
                    return _mm_cvtss_f32(lo);)
    }
    float s = 0; if constexpr (vec<float, Tg>::K == 0) for (float x : v.r.v) s += x;
    return s;
}
template <Target Tg> inline float reduce_max(vec<float, Tg> v) {
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(return svmaxv_f32(FORGE_SVE_PT, v.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(return __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredmax_vs_f32m1_f32m1(v.r, __riscv_vfmv_s_f_f32m1(-INFINITY, 1), FORGE_RVV_VL));) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(return vmaxvq_f32(v.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(return _mm512_reduce_max_ps(v.r);) }
    float tmp[vec<float, Tg>::N]; store(tmp, v);
    float m = tmp[0]; for (float x : tmp) m = x > m ? x : m; return m;
}

// Scale by 2^n for integer-valued float n (used by exp). Exact for normal range.
template <Target Tg> inline vec<float, Tg> ldexp_int(vec<float, Tg> x, vec<float, Tg> n) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svscale_f32_x(FORGE_SVE_PT, x.r, svcvt_s32_f32_x(FORGE_SVE_PT, svrintn_f32_x(FORGE_SVE_PT, n.r)));) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = __riscv_vfmul_vv_f32m1(x.r, __riscv_vreinterpret_v_i32m1_f32m1(__riscv_vsll_vx_i32m1(__riscv_vadd_vx_i32m1(__riscv_vfcvt_x_f_v_i32m1(n.r, FORGE_RVV_VL), 127, FORGE_RVV_VL), 23, FORGE_RVV_VL)), FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = vmulq_f32(x.r, vreinterpretq_f32_s32(vshlq_n_s32(vaddq_s32(vcvtnq_s32_f32(n.r), vdupq_n_s32(127)), 23)));) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_scalef_ps(x.r, n.r);) }
    else if constexpr (vec<float, Tg>::K == 1) {
        FORGE_IF256(__m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(n.r), _mm256_set1_epi32(127)), 23);
                    o.r = _mm256_mul_ps(x.r, _mm256_castsi256_ps(e));)
    } else for (int i = 0; i < vec<float, Tg>::N; ++i) o.r.v[i] = std::ldexp(x.r.v[i], int(n.r.v[i]));
    return o;
}
template <Target Tg> inline vec<float, Tg> round_nearest(vec<float, Tg> x) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svrintn_f32_x(FORGE_SVE_PT, x.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(auto r = __riscv_vfcvt_f_x_v_f32m1(__riscv_vfcvt_x_f_v_i32m1(x.r, FORGE_RVV_VL), FORGE_RVV_VL); o.r = __riscv_vmerge_vvm_f32m1(x.r, __riscv_vfsgnj_vv_f32m1(r, x.r, FORGE_RVV_VL), __riscv_vmflt_vf_f32m1_b32(__riscv_vfabs_v_f32m1(x.r, FORGE_RVV_VL), 8388608.f, FORGE_RVV_VL), FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = vrndnq_f32(x.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_roundscale_ps(x.r, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = _mm256_round_ps(x.r, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);) }
    else for (int i = 0; i < vec<float, Tg>::N; ++i) o.r.v[i] = std::nearbyint(x.r.v[i]);
    return o;
}

template <Target Tg> inline vec<float, Tg> sqrt(vec<float, Tg> x) {
    vec<float, Tg> o;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(o.r = svsqrt_f32_x(FORGE_SVE_PT, x.r);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(o.r = __riscv_vfsqrt_v_f32m1(x.r, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(o.r = vsqrtq_f32(x.r);) }
    else if constexpr (vec<float, Tg>::K == 2) { FORGE_IF512(o.r = _mm512_sqrt_ps(x.r);) }
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(o.r = _mm256_sqrt_ps(x.r);) }
    else for (int i = 0; i < vec<float, Tg>::N; ++i) o.r.v[i] = std::sqrt(x.r.v[i]);
    return o;
}
template <Target Tg> inline mask<float, Tg> operator&(mask<float, Tg> a, mask<float, Tg> b) {
    mask<float, Tg> m;
    if constexpr (vec<float, Tg>::K == 4) { FORGE_IFSVE(m.m = svand_b_z(FORGE_SVE_PT, a.m, b.m);) }
    else if constexpr (vec<float, Tg>::K == 5) { FORGE_IFRVV(m.m = __riscv_vmand_mm_b32(a.m, b.m, FORGE_RVV_VL);) }
    else if constexpr (vec<float, Tg>::K == 3) { FORGE_IFNEON(m.m = vandq_u32(a.m, b.m);) }
    else if constexpr (vec<float, Tg>::K == 2) m.m = a.m & b.m;
    else if constexpr (vec<float, Tg>::K == 1) { FORGE_IF256(m.m = _mm256_and_ps(a.m, b.m);) }
    else m.m = a.m & b.m;
    return m;
}
template <Target Tg> inline vec<float, Tg> abs(vec<float, Tg> x) { return select(lt(x, splat<Tg>(0.0f)), x * splat<Tg>(-1.0f), x); }
template <Target Tg> inline vec<float, Tg> operator-(vec<float, Tg> x) { return x * splat<Tg>(-1.0f); }

// strip: strip-mining loop hiding width and tail. f(i, mask) is called with a full mask for
// full vectors and a partial native predicate for the tail (no scalar epilogue).
template <Target Tg, class T, class F> inline void strip(std::size_t n, F&& f) {
    constexpr std::size_t W = std::size_t(vec<float, Tg>::N);
    std::size_t i = 0;
    auto full = mask<float, Tg>::all();
    for (; i + W <= n; i += W) f(i, full);
    if (i < n) f(i, mask<float, Tg>::first_n(n - i));
}

} // namespace forge::simd
