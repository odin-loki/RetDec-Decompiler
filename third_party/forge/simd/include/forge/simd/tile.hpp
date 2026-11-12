#pragma once
// Matrix tile units: Intel AMX (bf16 / int8 dot-product tiles) and Arm SME (f32 outer-product ZA tiles).
// C = A * B^T with A [m,k], B [n,k] row-major (same convention as gemm_nt). All functions return false when the
// unit is absent (or the OS refuses tile state) so the caller falls back to the vector GEMM.
//   amx_gemm_bf16: inputs rounded to bf16 (RNE), f32 accumulate -> relative error ~ 2^-8 * sqrt(k).
//   amx_gemm_s8:   int8 x int8 -> int32 (exact).
//   sme_gemm_f32:  f32 FMOPA, f32 accumulate -> same accuracy class as gemm_nt.
#include <cstddef>
#include <cstdint>

namespace forge::simd::tile {
bool amx_available();   // CPUID AMX-TILE+BF16+INT8 and the Linux XTILEDATA permission granted
bool sme_available();   // HWCAP2_SME (Linux) / FEAT_SME (Apple M4)
bool amx_gemm_bf16(const float* A, const float* B, float* C, std::size_t m, std::size_t n, std::size_t k);
bool amx_gemm_s8(const std::int8_t* A, const std::int8_t* B, std::int32_t* C, std::size_t m, std::size_t n, std::size_t k);
bool sme_gemm_f32(const float* A, const float* B, float* C, std::size_t m, std::size_t n, std::size_t k);
std::size_t sme_vl_bytes();  // streaming vector length (0 when absent)
} // namespace forge::simd::tile
