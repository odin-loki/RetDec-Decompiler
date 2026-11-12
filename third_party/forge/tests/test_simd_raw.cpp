// Tier R: generated wrappers are feature-gated at compile time.
#include "check.hpp"
#include <forge/simd/dispatch.hpp>
#include <forge/simd/raw/x86.gen.hpp>

using namespace forge::simd;
namespace r = forge::simd::raw::x86;

template <class T> concept can_avx512_add = requires(__m512 a) { r::avx512f::mm512_add_ps<T>(a, a); };
template <class T> concept can_avx2_add = requires(__m256i a) { r::avx2::mm256_add_epi32<T>(a, a); };
template <class T> concept can_vnni = requires(__m512i a) { r::avx512vnni::mm512_dpbusd_epi32<T>(a, a, a); };
static_assert(can_avx512_add<x86_64_v4> && !can_avx512_add<x86_64_v3>);
static_assert(can_avx2_add<x86_64_v3> && !can_avx2_add<scalar_t>);
static_assert(can_vnni<sapphire> && !can_vnni<x86_64_v4>);

TEST(raw_generated_count) { CHECK(r::intrinsic_count > 200); }
TEST(raw_avx2_executes) {
    if (!detect().avx2 || !detect().fma) return;
    __m256i a = _mm256_set1_epi32(20), b = _mm256_set1_epi32(22);
    __m256i c = r::avx2::mm256_add_epi32<x86_64_v3>(a, b);
    alignas(32) int out[8]; _mm256_store_si256((__m256i*)out, c);
    for (int v : out) CHECK(v == 42);
    __m256 f = r::fma::mm256_fmadd_ps<x86_64_v3>(_mm256_set1_ps(2), _mm256_set1_ps(3), _mm256_set1_ps(1));
    CHECK(_mm256_cvtss_f32(f) == 7.f);
}
