#include <forge/simd/algo.hpp>
#include <forge/simd/qdot.hpp>
#include <forge/simd/dispatch.hpp>
static_assert(forge::simd::vec<float, forge::simd::x86_64_v4>::K == 2, "avx512 TU must build native AVX-512 kernels");
FORGE_SIMD_EXPORT_TABLE(mv_avx512, x86_64_v4, "avx512")
