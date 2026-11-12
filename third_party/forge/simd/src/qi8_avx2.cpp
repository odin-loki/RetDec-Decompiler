// Integer matvec kernels, AVX2: maddubs then madd (qi8_x86.inc).
#define QI8_NS avx2
#define QI8_DPWSSD(acc, a, b) _mm256_add_epi32((acc), _mm256_madd_epi16((a), (b)))
#define QI8_DPBUSD(acc, u, s) _mm256_add_epi32((acc), _mm256_madd_epi16(_mm256_maddubs_epi16((u), (s)), _mm256_set1_epi16(1)))
#include "qi8_x86.inc"
namespace forge::simd::qi8_tables {
extern const qi8_kernels avx2;
const qi8_kernels avx2 = {"avx2", &qi8::avx2::quantize, &qi8::avx2::matvec};
}
