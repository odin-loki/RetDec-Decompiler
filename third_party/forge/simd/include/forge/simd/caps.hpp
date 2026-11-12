#pragma once
// Capabilities, not hierarchies: a target is a *set* of features; implementations require subsets.
#include <type_traits>

namespace forge::simd {

// ---- feature tags ----
// (FMA3 is tagged fma3, not fma: MSVC rejects a class and the simd::fma function template sharing a name.)
#define FORGE_FEATURES(X) \
    X(sse2) X(sse4_2) X(popcnt) X(avx) X(avx2) X(fma3) X(f16c) X(bmi2) \
    X(avx512f) X(avx512bw) X(avx512dq) X(avx512vl) X(avx512cd) X(avx512vnni) X(avx512bf16) \
    X(avx512fp16) X(avx512ifma) X(avx512vbmi) X(avx512vbmi2) X(avx512bitalg) X(avx512vpopcntdq) \
    X(avxvnni) X(avx10_1) X(amx_tile) X(amx_int8) X(amx_bf16) X(amx_fp16) \
    X(neon) X(dotprod) X(i8mm) X(arm_bf16) X(arm_fp16) X(sve) X(sve2) X(sme) X(sme2) \
    X(rvv) X(zvfh) X(zvbb) X(vsx) X(wasm_simd128) X(lsx) X(lasx)

#define FORGE_TAG(n) struct n { static constexpr const char* name = #n; };
FORGE_FEATURES(FORGE_TAG)
#undef FORGE_TAG

template <class... F> struct caps {
    template <class G> static constexpr bool has = (std::is_same_v<F, G> || ...);
    using is_target = void;
};

template <class Base, class... Extra> struct caps_extend_t;
template <class... B, class... E> struct caps_extend_t<caps<B...>, E...> { using type = caps<B..., E...>; };
template <class Base, class... E> struct caps_extend : caps_extend_t<typename Base::caps_type, E...>::type {
};

template <class T> concept Target = requires { typename T::is_target; };
template <class T, class F> concept Has = Target<T> && T::template has<F>;

// ---- presets ----
struct scalar_t : caps<> { using caps_type = caps<>; static constexpr const char* name = "scalar"; };
struct x86_64_v2 : caps<sse2, sse4_2, popcnt> {
    using caps_type = caps<sse2, sse4_2, popcnt>; static constexpr const char* name = "x86-64-v2"; };
struct x86_64_v3 : caps<sse2, sse4_2, popcnt, avx, avx2, fma3, f16c, bmi2> {
    using caps_type = caps<sse2, sse4_2, popcnt, avx, avx2, fma3, f16c, bmi2>; static constexpr const char* name = "x86-64-v3"; };
struct x86_64_v4 : caps_extend<x86_64_v3, avx512f, avx512bw, avx512dq, avx512vl, avx512cd> {
    using caps_type = caps_extend_t<x86_64_v3::caps_type, avx512f, avx512bw, avx512dq, avx512vl, avx512cd>::type;
    static constexpr const char* name = "x86-64-v4"; };
struct zen4 : caps_extend<x86_64_v4, avx512vnni, avx512bf16, avx512ifma, avx512vbmi, avx512vbmi2, avx512bitalg, avx512vpopcntdq> {
    static constexpr const char* name = "zen4"; };
struct sapphire : caps_extend<x86_64_v4, avx512vnni, avx512bf16, avx512fp16, avx512ifma, avx512vbmi, avx512vbmi2,
                              avx512bitalg, avx512vpopcntdq, avxvnni, amx_tile, amx_int8, amx_bf16> {
    static constexpr const char* name = "sapphire-rapids"; };
struct neoverse2 : caps<neon, dotprod, i8mm, arm_bf16, arm_fp16, sve, sve2> {
    using caps_type = caps<neon, dotprod, i8mm, arm_bf16, arm_fp16, sve, sve2>; static constexpr const char* name = "neoverse-v2"; };
struct apple_m : caps<neon, dotprod, i8mm, arm_bf16, arm_fp16> {
    using caps_type = caps<neon, dotprod, i8mm, arm_bf16, arm_fp16>; static constexpr const char* name = "apple-m"; };
struct rvv1 : caps<rvv, zvfh> { using caps_type = caps<rvv, zvfh>; static constexpr const char* name = "rvv1"; };
// Scalable-vector targets at a fixed register length (bits); one kernel table is compiled per length.
template <int Bits> struct sve_vl : caps<neon, dotprod, i8mm, arm_bf16, arm_fp16, sve, sve2> {
    using caps_type = caps<neon, dotprod, i8mm, arm_bf16, arm_fp16, sve, sve2>; static constexpr int vl = Bits; static constexpr const char* name = "sve"; };
template <int Bits> struct rvv_vl : caps<rvv, zvfh> {
    using caps_type = caps<rvv, zvfh>; static constexpr int vl = Bits; static constexpr const char* name = "rvv"; };
template <class T> constexpr int vl_bits() { if constexpr (requires { T::vl; }) return T::vl; else return 256; }

// Widest native f32 register width (in lanes) implied by a target.
template <Target T> constexpr int native_f32_lanes() {
    if constexpr (Has<T, avx512f>) return 16;
    else if constexpr (Has<T, avx2>) return 8;
    else if constexpr (Has<T, sve>) return vl_bits<T>() / 32;   // SVE / RVV: fixed length per tag (default 256)
    else if constexpr (Has<T, rvv>) return vl_bits<T>() / 32;
    else if constexpr (Has<T, neon>) return 4;
    else return 4; // scalar emulation, 4-wide for ILP
}

} // namespace forge::simd
