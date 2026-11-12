#include <forge/simd/algo.hpp>
#include <forge/simd/qdot.hpp>
#include <forge/simd/dispatch.hpp>
static_assert(forge::simd::vec<float, forge::simd::x86_64_v3>::K == 1, "avx2 TU must build native AVX2 kernels");
FORGE_SIMD_EXPORT_TABLE(mv_avx2, x86_64_v3, "avx2")
