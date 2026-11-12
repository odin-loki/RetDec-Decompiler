// Integer matvec kernels, AVX-512 VNNI in its 256-bit EVEX form (Cascade Lake, Ice Lake, Sapphire Rapids, Zen 4).
// Decode is memory-bound, so ymm keeps the core out of the AVX-512 frequency licence.
// CMake defines FORGE_QI8_HAVE_AVX512VNNI when the compiler accepts -mavx512vnni.
#if defined(FORGE_QI8_HAVE_AVX512VNNI)
#define QI8_NS avx512vnni
#define QI8_DPWSSD(acc, a, b) _mm256_dpwssd_epi32((acc), (a), (b))
#define QI8_DPBUSD(acc, u, s) _mm256_dpbusd_epi32((acc), (u), (s))
#include "qi8_x86.inc"
namespace forge::simd::qi8_tables {
extern const qi8_kernels avx512vnni;
const qi8_kernels avx512vnni = {"avx512vnni", &qi8::avx512vnni::quantize, &qi8::avx512vnni::matvec};
}
#endif
