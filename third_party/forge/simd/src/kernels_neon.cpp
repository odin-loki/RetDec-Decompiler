#include <forge/simd/algo.hpp>
#include <forge/simd/qdot.hpp>
#include <forge/simd/dispatch.hpp>
#if defined(__aarch64__)
static_assert(forge::simd::vec<float, forge::simd::apple_m>::K == 3, "neon TU must build native NEON kernels");
FORGE_SIMD_EXPORT_TABLE(mv_neon, apple_m, "neon")
#endif
