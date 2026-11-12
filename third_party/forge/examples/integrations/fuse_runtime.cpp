// FUSE engine (Part F.2): task/mem/prof as runtime services, lane for GUI CPU paths, inference through the C ABI.
#include <forge/simd/dispatch.hpp>
#include <forge/task/pool.hpp>
#include <cstdio>
#include <vector>

using namespace forge;

int main() {
    task::pool workers;                                             // one scheduler for frames + inference
    auto& k = simd::active();
    const std::size_t W = 640, H = 360;
    std::vector<float> dst(W * H, 0.25f), src(W * H, 1.f);
    for (int frame = 0; frame < 3; ++frame) {
        workers.parallel_for(H, 16, [&](std::size_t y0, std::size_t y1) {   // alpha blend rows: dst += a*src
            for (std::size_t y = y0; y < y1; ++y) k.saxpy(0.5f, src.data() + y * W, dst.data() + y * W, W);
        });
    }
    double sum = 0; for (float v : dst) sum += v;
    std::printf("fuse: %u workers, lane %s, blended mean %.3f\n", workers.size(), k.target, sum / double(dst.size()));
    return sum / double(dst.size()) > 1.74 && sum / double(dst.size()) < 1.76 ? 0 : 1;
}
