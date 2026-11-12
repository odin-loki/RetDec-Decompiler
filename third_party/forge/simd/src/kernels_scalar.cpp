#include <forge/simd/algo.hpp>
#include <forge/simd/qdot.hpp>
#include <forge/simd/dispatch.hpp>
// Each target lives in its own TU, own flags, own namespace — no ODR leakage across ISAs.
FORGE_SIMD_EXPORT_TABLE(mv_scalar, scalar_t, "scalar")
