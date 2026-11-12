#pragma once
// CPU feature detection (what the OS enables, not just CPUID) + dispatch table resolved once.
#include <cstddef>
#include <cstdint>
#include <string>

namespace forge::simd {

struct cpu_features {
    bool sse2 = false, sse4_2 = false, popcnt = false, avx = false, avx2 = false, fma = false, f16c = false, bmi2 = false;
    bool avx512f = false, avx512bw = false, avx512dq = false, avx512vl = false, avx512cd = false;
    bool avx512vnni = false, avx512bf16 = false, avx512fp16 = false, avx512ifma = false, avx512vbmi = false;
    bool avx512vbmi2 = false, avx512bitalg = false, avx512vpopcntdq = false, avxvnni = false, avx10 = false;
    int avx10_version = 0;
    bool amx_tile = false, amx_int8 = false, amx_bf16 = false, amx_fp16 = false;
    bool amx_permitted = false;          // Linux arch_prctl(ARCH_REQ_XCOMP_PERM) succeeded
    bool os_avx = false, os_avx512 = false; // XGETBV state enabled
    bool neon = false, sve = false, sve2 = false; int sve_bytes = 0, sve_hw_bytes = 0;
    bool rvv = false; int rvv_bytes = 0;
    bool hybrid = false;                 // P/E-core part
    std::string vendor, brand;
    std::string summary() const;
};

const cpu_features& detect();          // cached after first call

// Dispatch table: resolved exactly once into function pointers.
struct kernels {
    const char* target;
    void (*saxpy)(float, const float*, float*, std::size_t);
    float (*dot)(const float*, const float*, std::size_t);
    float (*sum)(const float*, std::size_t);
    void (*softmax_row)(const float*, float*, std::size_t);
    void (*rms_norm_row)(const float*, const float*, float*, std::size_t, float);
    void (*exp)(const float*, float*, std::size_t);
    void (*log)(const float*, float*, std::size_t);
    void (*tanh)(const float*, float*, std::size_t);
    void (*sigmoid)(const float*, float*, std::size_t);
    void (*silu)(const float*, float*, std::size_t);
    void (*gelu)(const float*, float*, std::size_t);
    void (*silu_mul)(const float*, const float*, float*, std::size_t);
    void (*add)(const float*, const float*, float*, std::size_t);
    void (*sub)(const float*, const float*, float*, std::size_t);
    void (*mul)(const float*, const float*, float*, std::size_t);
    void (*gemm_nt)(const float*, const float*, float*, std::size_t, std::size_t, std::size_t);
    void (*gemm_nn)(const float*, const float*, float*, std::size_t, std::size_t, std::size_t);
    void (*math1)(int fn, const float*, float*, std::size_t);              // simd::fn1 ids
    void (*math2)(int fn, const float*, const float*, float*, std::size_t); // simd::fn2 ids
    float (*logsumexp)(const float*, std::size_t);
    // quantised rows (lane/qdot.hpp): fmt is a simd::qfmt, fallback the codec's scalar decoder
    void (*qdecode)(int, const std::uint8_t*, std::size_t, float*, void (*)(const std::uint8_t*, std::size_t, float*));
    float (*qdot)(int, const std::uint8_t*, const float*, std::size_t, void (*)(const std::uint8_t*, std::size_t, float*), std::size_t, std::size_t);
};

const kernels& active();                // best target for this machine (FORGE_SIMD_TARGET overrides)
const kernels* by_name(const char* t);  // "scalar", "avx2", "avx512"; nullptr if not built/usable

} // namespace forge::simd

#define FORGE_SIMD_EXPORT_TABLE(NS, TG, NAME)                                                       \
    namespace forge::simd::NS {                                                                     \
    extern const kernels table;                                                                     \
    const kernels table = {NAME,                                                                    \
        &algo::saxpy<TG>, &algo::dot<TG>, &algo::sum<TG>, &algo::softmax_row<TG>, &algo::rms_norm_row<TG>, \
        &algo::exp_n<TG>, &algo::log_n<TG>, &algo::tanh_n<TG>, &algo::sigmoid_n<TG>, &algo::silu_n<TG>, \
        &algo::gelu_n<TG>, &algo::silu_mul_n<TG>, &algo::add_n<TG>, &algo::sub_n<TG>, &algo::mul_n<TG>, \
        &algo::gemm_nt<TG>, &algo::gemm_nn<TG>, &algo::math1_n<TG>, &algo::math2_n<TG>, &algo::logsumexp_row<TG>, \
        &algo::qdecode<TG>, &algo::qdot<TG>};                                                    \
    }
