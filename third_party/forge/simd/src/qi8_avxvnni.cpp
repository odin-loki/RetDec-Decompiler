// Integer matvec kernels, AVX-VNNI (VEX vpdpwssd / vpdpbusd on ymm: Alder Lake and later client cores, Zen 5).
// CMake defines FORGE_QI8_HAVE_AVXVNNI when the compiler accepts -mavxvnni.
#if defined(FORGE_QI8_HAVE_AVXVNNI)
#define QI8_NS avxvnni
#define QI8_DPWSSD(acc, a, b) _mm256_dpwssd_avx_epi32((acc), (a), (b))
#define QI8_DPBUSD(acc, u, s) _mm256_dpbusd_avx_epi32((acc), (u), (s))
#include "qi8_x86.inc"
namespace forge::simd::qi8_tables {
extern const qi8_kernels avxvnni;
const qi8_kernels avxvnni = {"avxvnni", &qi8::avxvnni::quantize, &qi8::avxvnni::matvec};
}
#endif
