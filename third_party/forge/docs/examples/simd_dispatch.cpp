// docs/simd.md example: inspect CPU features and call a kernel through the dispatch table.
#include <forge/simd/dispatch.hpp>
#include <cstdio>
#include <vector>

int main() {
    std::printf("cpu: %s\n", forge::simd::detect().summary().c_str());
    const forge::simd::kernels& k = forge::simd::active();   // FORGE_SIMD_TARGET overrides
    std::printf("target: %s\n", k.target);
    std::vector<float> x(1000, 1.f), y(1000, 2.f);
    k.saxpy(3.f, x.data(), y.data(), x.size());               // y = 3*x + y
    float d = k.dot(x.data(), y.data(), x.size());
    std::printf("dot = %.1f\n", d);
    return d == 5000.f ? 0 : 1;
}
