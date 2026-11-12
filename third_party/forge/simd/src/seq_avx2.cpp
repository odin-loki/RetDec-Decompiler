// seq_kernels table: AVX2 + FMA, compiled with -ffp-contract=off (see seq_impl.hpp).
#if defined(__x86_64__) || defined(_M_X64)
#define FORGE_SEQ_NS seq_avx2
#define FORGE_SEQ_AVX2 1
#include <forge/simd/seq_impl.hpp>
FORGE_SEQ_EXPORT_TABLE("avx2")
#endif
