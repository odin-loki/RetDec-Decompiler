// Intel AMX tile GEMM. Built with -mamx-tile -mamx-bf16 -mamx-int8 on x86_64; only called after amx_available().
#include <forge/simd/tile.hpp>
#include <cstring>
#include <vector>
#if defined(__x86_64__) && defined(FORGE_HAVE_AMX)
#include <immintrin.h>
#include <cpuid.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace forge::simd::tile {
namespace {
// GCC's _tile_loadd/_tile_stored are asm without a "memory" clobber, so the compiler may reorder
// plain loads/stores of the packed buffers and c[] across them. Fence explicitly.
inline void fence() { __asm__ __volatile__("" ::: "memory"); }
struct alignas(64) tilecfg { std::uint8_t palette, start; std::uint8_t rsv[14]; std::uint16_t colsb[16]; std::uint8_t rows[16]; };
// tmm0 = C (16 x 16 x 4B), tmm1 = A (16 rows x 64B), tmm2 = B (16 rows x 64B)
void config() {
    tilecfg c{}; c.palette = 1;
    for (int t = 0; t < 3; ++t) { c.colsb[t] = 64; c.rows[t] = 16; }
    fence(); _tile_loadconfig(&c);
}
std::uint16_t bf16(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); if ((u & 0x7fffffff) > 0x7f800000) return std::uint16_t((u >> 16) | 0x40); u += 0x7fff + ((u >> 16) & 1); return std::uint16_t(u >> 16); }
}
bool amx_available() {
    static const bool ok = [] {
        unsigned a, b, c, d;
        if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
        bool hw = (d >> 24 & 1) && (d >> 22 & 1) && (d >> 25 & 1);   // AMX-TILE, AMX-BF16, AMX-INT8
        if (!hw) return false;
#if defined(__linux__)
        constexpr long ARCH_REQ_XCOMP_PERM = 0x1023, XFEATURE_XTILEDATA = 18;
        if (syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_PERM, XFEATURE_XTILEDATA) != 0) return false;
#endif
        return true;
    }();
    return ok;
}
bool amx_gemm_bf16(const float* A, const float* B, float* C, std::size_t m, std::size_t n, std::size_t k) {
    if (!amx_available()) return false;
    const std::size_t K = (k + 31) / 32 * 32, M = (m + 15) / 16 * 16, N = (n + 15) / 16 * 16;
    // A packed row-major bf16 [M, K]; B packed VNNI per 16-col block: [N/16][K/2][16 cols][2]
    static thread_local std::vector<std::uint16_t> a, b; a.assign(M * K, 0); b.assign(N * K, 0);
    for (std::size_t i = 0; i < m; ++i) for (std::size_t p = 0; p < k; ++p) a[i * K + p] = bf16(A[i * k + p]);
    for (std::size_t j = 0; j < n; ++j) for (std::size_t p = 0; p < k; ++p)
        b[(j / 16) * (K * 16) + (p / 2) * 32 + (j % 16) * 2 + (p % 2)] = bf16(B[j * k + p]);
    fence(); config();
    alignas(64) float c[16 * 16];
    for (std::size_t i0 = 0; i0 < M; i0 += 16)
        for (std::size_t j0 = 0; j0 < N; j0 += 16) {
            _tile_zero(0);
            for (std::size_t p0 = 0; p0 < K; p0 += 32) {
                _tile_loadd(1, a.data() + i0 * K + p0, int(K * 2));
                _tile_loadd(2, b.data() + (j0 / 16) * (K * 16) + (p0 / 2) * 32, 64);
                _tile_dpbf16ps(0, 1, 2);
            }
            _tile_stored(0, c, 64); fence();
            for (std::size_t i = i0; i < std::min(m, i0 + 16); ++i)
                for (std::size_t j = j0; j < std::min(n, j0 + 16); ++j) C[i * n + j] = c[(i - i0) * 16 + (j - j0)];
        }
    _tile_release();
    return true;
}
bool amx_gemm_s8(const std::int8_t* A, const std::int8_t* B, std::int32_t* C, std::size_t m, std::size_t n, std::size_t k) {
    if (!amx_available()) return false;
    const std::size_t K = (k + 63) / 64 * 64, M = (m + 15) / 16 * 16, N = (n + 15) / 16 * 16;
    static thread_local std::vector<std::int8_t> a, b; a.assign(M * K, 0); b.assign(N * K, 0);
    for (std::size_t i = 0; i < m; ++i) std::memcpy(&a[i * K], A + i * k, k);
    for (std::size_t j = 0; j < n; ++j) for (std::size_t p = 0; p < k; ++p)
        b[(j / 16) * (K * 16) + (p / 4) * 64 + (j % 16) * 4 + (p % 4)] = B[j * k + p];
    fence(); config();
    alignas(64) std::int32_t c[16 * 16];
    for (std::size_t i0 = 0; i0 < M; i0 += 16)
        for (std::size_t j0 = 0; j0 < N; j0 += 16) {
            _tile_zero(0);
            for (std::size_t p0 = 0; p0 < K; p0 += 64) {
                _tile_loadd(1, a.data() + i0 * K + p0, int(K));
                _tile_loadd(2, b.data() + (j0 / 16) * (K * 16) + (p0 / 4) * 64, 64);
                _tile_dpbssd(0, 1, 2);
            }
            _tile_stored(0, c, 64); fence();
            for (std::size_t i = i0; i < std::min(m, i0 + 16); ++i)
                for (std::size_t j = j0; j < std::min(n, j0 + 16); ++j) C[i * n + j] = c[(i - i0) * 16 + (j - j0)];
        }
    _tile_release();
    return true;
}
} // namespace forge::simd::tile
#else
namespace forge::simd::tile {
bool amx_available() { return false; }
bool amx_gemm_bf16(const float*, const float*, float*, std::size_t, std::size_t, std::size_t) { return false; }
bool amx_gemm_s8(const std::int8_t*, const std::int8_t*, std::int32_t*, std::size_t, std::size_t, std::size_t) { return false; }
} // namespace forge::simd::tile
#endif
