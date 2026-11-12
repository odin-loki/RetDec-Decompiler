#include "check.hpp"
#include <forge/core/dtypes.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <random>

using namespace forge;

// Exhaustive: every f16 pattern decodes then re-encodes to itself (NaNs stay NaN).
TEST(f16_roundtrip_exhaustive) {
    for (u32 b = 0; b < 0x10000; ++b) {
        float f = float(f16::from_bits(u16(b)));
        u16 r = f16(f).bits;
        if (std::isnan(f)) CHECK(std::isnan(float(f16::from_bits(r))));
        else CHECK_MSG(r == b, "bits %04x -> %f -> %04x", b, f, r);
    }
}
TEST(f16_known_values) {
    CHECK(f16(1.0f).bits == 0x3c00); CHECK(f16(-2.0f).bits == 0xc000); CHECK(f16(65504.f).bits == 0x7bff);
    CHECK(f16(65520.f).bits == 0x7c00);            // rounds to inf
    CHECK(f16(5.9604645e-8f).bits == 0x0001);      // smallest subnormal
    CHECK(f16(2.98e-8f).bits == 0x0000);           // below half of smallest -> 0
    CHECK(f16(1e9f, rounding::nearest_even, overflow::saturate).bits == 0x7bff);
}
// Exhaustive nearest-even check vs brute-force nearest over all f16 values, on a dense f32 sample.
TEST(f16_rounding_matches_bruteforce) {
    std::vector<float> vals; for (u32 b = 0; b < 0x7c00; ++b) vals.push_back(float(f16::from_bits(u16(b))));
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> d(0.f, 65504.f);
    for (int it = 0; it < 200000; ++it) {
        float x = it < 100000 ? d(rng) : std::ldexp(d(rng) / 65504.f, -int(rng() % 26));
        auto it2 = std::lower_bound(vals.begin(), vals.end(), x);
        std::size_t hi = std::size_t(it2 - vals.begin()); std::size_t lo = hi ? hi - 1 : 0;
        if (hi >= vals.size()) hi = vals.size() - 1;
        double dl = std::fabs(double(x) - vals[lo]), dh = std::fabs(double(vals[hi]) - x);
        u16 expect = dl < dh ? u16(lo) : (dh < dl ? u16(hi) : ((lo & 1) ? u16(hi) : u16(lo)));
        u16 got = f16(x).bits;
        CHECK_MSG(got == expect, "x=%.9g got %04x expect %04x", x, got, expect);
        if (got != expect) break;
    }
}
TEST(bf16_roundtrip_exhaustive) {
    for (u32 b = 0; b < 0x10000; ++b) {
        float f = float(bf16::from_bits(u16(b)));
        if (std::isnan(f)) CHECK(std::isnan(float(bf16(f))));
        else CHECK(bf16(f).bits == b);
    }
    CHECK(bf16(1.00390625f).bits == 0x3f80);  // tie -> even
    CHECK(bf16(1.01171875f).bits == 0x3f82);  // tie -> even (up)
}
TEST(fp8_e4m3_exhaustive) {
    for (u32 b = 0; b < 256; ++b) {
        float f = float(fp8_e4m3::from_bits(u8(b)));
        if (std::isnan(f)) { CHECK((b & 0x7f) == 0x7f); continue; }
        CHECK_MSG(fp8_e4m3(f).bits == b, "e4m3 %02x", b);
    }
    CHECK(float(fp8_e4m3::from_bits(0x7e)) == 448.f);
    CHECK(fp8_e4m3(1000.f).bits == 0x7e);          // saturate
    CHECK(float(fp8_e4m3::from_bits(0x01)) == std::ldexp(1.f, -9));
}
TEST(fp8_e5m2_exhaustive) {
    for (u32 b = 0; b < 256; ++b) {
        float f = float(fp8_e5m2::from_bits(u8(b)));
        if (std::isnan(f)) continue;
        if (std::isinf(f)) { CHECK(fp8_e5m2(f).bits == b); continue; }
        CHECK_MSG(fp8_e5m2(f).bits == b, "e5m2 %02x", b);
    }
    CHECK(float(fp8_e5m2::from_bits(0x7b)) == 57344.f);
    CHECK(fp8_e5m2(1e6f).bits == 0x7b);
    CHECK(fp8_e5m2(1e6f, rounding::nearest_even, overflow::to_inf).bits == 0x7c);
}
TEST(int4_packing_exhaustive) {
    for (int a = -8; a < 8; ++a) for (int b = -8; b < 8; ++b) {
        auto p = i4x2::pack(a, b); CHECK(p.get(0) == a && p.get(1) == b);
    }
    for (unsigned a = 0; a < 16; ++a) for (unsigned b = 0; b < 16; ++b) {
        auto p = u4x2::pack(a, b); CHECK(p.get(0) == a && p.get(1) == b);
    }
    CHECK(i4x2::pack(100, -100).get(0) == 7 && i4x2::pack(100, -100).get(1) == -8);
}
TEST(mx_element_formats_exhaustive) {
    // every code round-trips; known maxima; RNE ties go to the even code; saturation
    auto all = []<class T>(T, float mx, int n) {
        for (int b = 0; b < n; ++b) { float f = float(T::from_bits(u8(b))); if (f == 0 && b) continue; CHECK_MSG(T(f).bits == b, "code %d", b); }
        CHECK(T::max_value() == mx); CHECK(float(T(1e9f)) == mx); CHECK(float(T(-1e9f)) == -mx);
    };
    all(fp4_e2m1{}, 6.f, 16); all(fp6_e2m3{}, 7.5f, 64); all(fp6_e3m2{}, 28.f, 64);
    CHECK(float(fp4_e2m1(2.5f)) == 2.f);   // tie between 2 (even code) and 3
    CHECK(float(fp4_e2m1(5.f)) == 4.f);    // tie between 4 (even code) and 6
    CHECK(float(fp4_e2m1(0.25f)) == 0.f);
    CHECK(float(fp4_e2m1(1.75f)) == 2.f);
}
TEST(stochastic_rounding_unbiased) {
    // E[stochastic_round(x)] == x: the mean over many draws converges to x, unlike RNE
    u32 st = 12345;
    auto rnd = [&] { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
    for (float x : {1.0001f, 3.3f, -0.77f, 100.3f}) {
        double sum = 0; int n = 200000;
        for (int i = 0; i < n; ++i) sum += float(stochastic_round<bf16>(x, rnd()));
        double ulp = std::fabs(float(bf16(x, rounding::toward_zero)) - float(bf16::from_bits(u16(bf16(x, rounding::toward_zero).bits + 1))));
        CHECK_MSG(std::fabs(sum / n - x) < ulp * 0.02, "bf16 sr mean %g vs %g", sum / n, double(x));
        sum = 0;
        for (int i = 0; i < n; ++i) sum += float(stochastic_round<f16>(x, rnd()));
        CHECK_MSG(std::fabs(sum / n - x) < std::fabs(x) * 2e-5, "f16 sr mean %g vs %g", sum / n, double(x));
    }
    CHECK(float(stochastic_round<fp8_e4m3>(1e6f, 0)) == 448.f);   // saturates
    CHECK(float(stochastic_round<f16>(1.5f, 0xffffffffu)) == 1.5f); // exact stays exact
}
TEST(dtype_names) {
    for (int i = 0; i < int(dtype::invalid); ++i) CHECK(dtype_from_name(name(dtype(i))) == dtype(i));
}
