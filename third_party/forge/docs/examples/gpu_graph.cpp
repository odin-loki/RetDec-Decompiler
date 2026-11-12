// docs/gpu.md example: record a graph once, compile it, replay it; then use the prims.
#include <forge/gpu/prims.hpp>
#include <cstdio>
#include <vector>
using namespace forge;

struct Scale {
    float a;
    FORGE_KERNEL void operator()(gpu::item<3> it, gpu::span<float> y) const {
        if (auto i = it.global(0); i < i64(y.size())) y[std::size_t(i)] *= a;
    }
};

int main() {
    for (auto& p : gpu::probe_backends())
        std::printf("%-10s %s\n", gpu::name(p.kind), p.available ? "available" : "absent");
    auto dev = gpu::select();
    std::size_t n = 1024;
    std::vector<float> host(n, 1.f), back(n, 0.f);
    auto buf = dev.alloc<float>(n);
    gpu::graph g;
    auto up = g.copy(gpu::span<const float>(host), buf);
    auto k = g.launch(Scale{3.f}, gpu::extent{i64(n)}, buf); k.after(up);
    g.copy(buf, gpu::span<float>(back)).after(k);
    auto exe = g.compile(dev);
    exe.run();                                   // replay with exe.run() as often as needed
    std::vector<int> x{3, 1, 4, 1, 5}, y(5);
    gpu::prims::scan<int>(x, y, true);          // inclusive prefix sum: 3 4 8 9 14
    std::printf("back[0]=%g scan[4]=%d\n", back[0], y[4]);
    return back[0] == 3.f && y[4] == 14 ? 0 : 1;
}
