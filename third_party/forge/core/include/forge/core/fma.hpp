#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace forge {
// Correctly rounded float fma without trusting libm: some C runtimes (mingw/msvcrt under wine) implement
// fmaf as an unfused a*b+c. The product of two floats is exact in double; the sum is made exact-or-sticky
// with TwoSum and round-to-odd, so the final double->float rounding is correct.
inline float fmaf_exact(float a, float b, float c) {
#if defined(__FMA__) || defined(__ARM_FEATURE_FMA)
    return __builtin_fmaf(a, b, c);
#else
    double p = double(a) * double(b), cd = double(c), s = p + cd;
    if (!std::isfinite(s)) return float(s);
    double bv = s - p, err = (p - (s - bv)) + (cd - bv);
    if (err != 0) {
        std::uint64_t u; std::memcpy(&u, &s, 8);
        if (!(u & 1)) { if ((err > 0) == (s > 0)) ++u; else --u; std::memcpy(&s, &u, 8); }
    }
    return float(s);
#endif
}
} // namespace forge
