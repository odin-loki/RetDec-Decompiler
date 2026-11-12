// RVV kernel table at one fixed VLEN. CMake compiles this file once per length
// (-mrvv-vector-bits=128/256/512/1024); each copy exports forge::simd::mv_rvv<B>::table.
#include <forge/simd/algo.hpp>
#include <forge/simd/qdot.hpp>
#include <forge/simd/dispatch.hpp>
#if defined(__riscv)
#if !defined(FORGE_HAVE_RVV_FIXED)
#error "kernels_rvv.cpp must be compiled with V and -mrvv-vector-bits=<128..1024>"
#endif
#define FORGE_CAT2(a, b) a##b
#define FORGE_CAT(a, b) FORGE_CAT2(a, b)
using forge_rvv_tag = forge::simd::rvv_vl<FORGE_HAVE_RVV_FIXED>;
static_assert(forge::simd::vec<float, forge_rvv_tag>::K == 5 && forge::simd::vec<float, forge_rvv_tag>::N == FORGE_HAVE_RVV_FIXED / 32);
FORGE_SIMD_EXPORT_TABLE(FORGE_CAT(mv_rvv, __riscv_v_fixed_vlen), forge_rvv_tag, "rvv")
#endif
