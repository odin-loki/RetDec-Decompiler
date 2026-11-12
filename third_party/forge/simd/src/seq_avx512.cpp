// seq_kernels table: AVX-512 (F/BW/DQ/VL), compiled with -ffp-contract=off (see seq_impl.hpp).
#if defined(__x86_64__) || defined(_M_X64)
#define FORGE_SEQ_NS seq_avx512
#define FORGE_SEQ_AVX512 1
#include <forge/simd/seq_impl.hpp>
FORGE_SEQ_EXPORT_TABLE("avx512")
#endif
