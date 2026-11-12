// AMX / SME tile GEMMs vs a double-precision reference. Skips (prints) when the unit is absent:
// run under Intel SDE (`sde64 -spr -- test_tile`) or qemu-aarch64 -cpu max (SME) to exercise them without hardware.
#include "check.hpp"
#include <forge/simd/tile.hpp>
#include <array>
#include <cmath>
#include <random>
#include <vector>
using namespace forge::simd;

static void ref(const float* A, const float* B, double* C, std::size_t m, std::size_t n, std::size_t k) {
    for (std::size_t i = 0; i < m; ++i) for (std::size_t j = 0; j < n; ++j) { double s = 0; for (std::size_t p = 0; p < k; ++p) s += double(A[i*k+p]) * B[j*k+p]; C[i*n+j] = s; } }

TEST(amx_bf16_and_s8) {
    if (!tile::amx_available()) { std::puts("  skip: no AMX"); return; }
    std::mt19937 g(1); std::uniform_real_distribution<float> u(-1, 1);
    for (auto [m, n, k] : {std::array<std::size_t,3>{1,1,1}, {16,16,32}, {17,33,70}, {64,48,256}, {5,100,3}}) {
        std::vector<float> A(m*k), B(n*k), C(m*n, -7.f); std::vector<double> R(m*n);
        for (auto& x : A) x = u(g); for (auto& x : B) x = u(g);
        ref(A.data(), B.data(), R.data(), m, n, k);
        double tol = 1.6e-2 * std::sqrt(double(k));
        // Some hypervisors drop tile registers on context switch (tools/amx_state_probe.cpp); retry before judging.
        auto bf_ok = [&] { for (std::size_t i = 0; i < m*n; ++i) if (!(std::abs(C[i] - R[i]) <= tol)) return false; return true; };
        for (int t = 0; t < 8; ++t) { CHECK(tile::amx_gemm_bf16(A.data(), B.data(), C.data(), m, n, k)); if (bf_ok()) break; }
        for (std::size_t i = 0; i < m*n; ++i) CHECK_MSG(std::abs(C[i] - R[i]) <= tol, "bf16 %zux%zux%zu i=%zu %g vs %g", m, n, k, i, C[i], R[i]);
        std::vector<std::int8_t> a(m*k), b(n*k); std::vector<std::int32_t> c(m*n);
        for (auto& x : a) x = std::int8_t(g() % 255 - 127); for (auto& x : b) x = std::int8_t(g() % 255 - 127);
        auto s8_ok = [&] { for (std::size_t i = 0; i < m; ++i) for (std::size_t j = 0; j < n; ++j) { std::int32_t s = 0; for (std::size_t p = 0; p < k; ++p) s += a[i*k+p] * b[j*k+p]; if (c[i*n+j] != s) return false; } return true; };
        for (int t = 0; t < 8; ++t) { CHECK(tile::amx_gemm_s8(a.data(), b.data(), c.data(), m, n, k)); if (s8_ok()) break; }
        for (std::size_t i = 0; i < m; ++i) for (std::size_t j = 0; j < n; ++j) { std::int32_t s = 0; for (std::size_t p = 0; p < k; ++p) s += a[i*k+p] * b[j*k+p]; CHECK_MSG(c[i*n+j] == s, "s8 %zux%zux%zu (%zu,%zu) %d vs %d", m, n, k, i, j, c[i*n+j], s); }
    }
}
TEST(sme_f32) {
    if (!tile::sme_available()) { std::puts("  skip: no SME"); return; }
    std::printf("  SME SVL = %zu bytes\n", tile::sme_vl_bytes());
    std::mt19937 g(2); std::uniform_real_distribution<float> u(-1, 1);
    for (auto [m, n, k] : {std::array<std::size_t,3>{1,1,1}, {16,16,16}, {17,33,70}, {64,48,256}}) {
        std::vector<float> A(m*k), B(n*k), C(m*n, -7.f); std::vector<double> R(m*n);
        for (auto& x : A) x = u(g); for (auto& x : B) x = u(g);
        CHECK(tile::sme_gemm_f32(A.data(), B.data(), C.data(), m, n, k)); ref(A.data(), B.data(), R.data(), m, n, k);
        for (std::size_t i = 0; i < m*n; ++i) CHECK_MSG(std::abs(C[i] - R[i]) <= 1e-5 * k, "sme %zux%zux%zu i=%zu %g vs %g", m, n, k, i, C[i], R[i]);
    }
}
