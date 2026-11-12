// NeuralVFX (Part F.2): `streaming` session + audio module, Philox RNG for reproducible generation.
#include <forge/inference/streaming.hpp>
#include <forge/rand/philox.hpp>
#include <forge/signal/audio.hpp>
#include <forge/signal/fft.hpp>
#include <cmath>
#include <cstdio>

using namespace forge;

static std::vector<float> render(std::uint64_t seed, std::size_t n) {
    rand::philox4x32 rng{{std::uint32_t(seed), std::uint32_t(seed >> 32)}};
    std::vector<float> noise(n);
    for (std::size_t i = 0; i < n; ++i) noise[i] = rand::philox4x32::to_unit(rng(i / 4)[i % 4]) * 2 - 1;
    auto lp = signal::biquad::make(signal::biquad::lowpass, 48000, 2000);
    std::vector<float> out(n), frame_energy;
    inference::streaming::options o; o.window = 1024; o.hop = 512; o.deadline = std::chrono::microseconds(2000);
    inference::streaming::session s(o, [&](const float* fr, std::size_t len, std::size_t idx) {
        auto spec = signal::rfft(fr, len); double e = 0; for (auto& c : spec) e += std::norm(c);
        frame_energy.push_back(float(e)); (void)idx;
    });
    for (std::size_t i = 0; i < n; i += 480) {                        // 10 ms audio callbacks
        std::size_t k = std::min<std::size_t>(480, n - i);
        for (std::size_t j = 0; j < k; ++j) out[i + j] = float(lp.step(noise[i + j]));
        s.feed(out.data() + i, k); s.pump();
    }
    std::printf("neuralvfx: %zu frames, %zu dropped, %zu overruns, worst %.3f ms\n", s.stat().frames, s.stat().dropped, s.stat().overruns, s.stat().worst_ms);
    return frame_energy;
}

int main() {
    auto a = render(42, 48000), b = render(42, 48000);
    bool same = a == b;
    std::printf("neuralvfx: reproducible across runs: %s\n", same ? "yes" : "NO");
    return same ? 0 : 1;
}
