#pragma once
// audio: windowed-sinc rational resampler and RBJ biquad filters (ring buffers live in inference/streaming.hpp).
#include <cmath>
#include <complex>
#include <cstddef>
#include <numeric>
#include <vector>

namespace forge::signal {

// Resample by out_rate/in_rate with a Kaiser-windowed sinc (cutoff at the lower Nyquist). `taps` per side.
inline std::vector<float> resample(const std::vector<float>& x, long in_rate, long out_rate, int taps = 32, double beta = 8.6) {
    long g = std::gcd(in_rate, out_rate); long up = out_rate / g, down = in_rate / g;
    double cutoff = std::min(1.0, double(out_rate) / double(in_rate));
    auto i0 = [](double v) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (v / 2) / k; s += t * t; } return s; };
    double ib = i0(beta);
    std::size_t n = std::size_t((x.size() * std::size_t(up) + std::size_t(down) - 1) / std::size_t(down));
    std::vector<float> y(n);
    for (std::size_t m = 0; m < n; ++m) {
        double t = double(m) * double(down) / double(up);           // position in input samples
        long c = long(std::floor(t)); double acc = 0, wsum = 0;
        for (long k = c - taps + 1; k <= c + taps; ++k) {
            double d = t - double(k); double r = d / taps; if (std::fabs(r) > 1) continue;
            double sinc = d == 0 ? 1.0 : std::sin(M_PI * d * cutoff) / (M_PI * d * cutoff);
            double w = i0(beta * std::sqrt(1 - r * r)) / ib, h = cutoff * sinc * w;
            wsum += h;
            if (k >= 0 && std::size_t(k) < x.size()) acc += h * x[std::size_t(k)];
        }
        y[m] = float(wsum != 0 ? acc / wsum * 1.0 : 0.0);
    }
    return y;
}

struct biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    enum kind { lowpass, highpass, bandpass, notch };
    static biquad make(kind k, double sr, double f0, double q = 0.70710678) {
        double w = 2 * M_PI * f0 / sr, cw = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al; biquad b;
        switch (k) {
            case lowpass:  b.b0 = (1 - cw) / 2; b.b1 = 1 - cw; b.b2 = (1 - cw) / 2; break;
            case highpass: b.b0 = (1 + cw) / 2; b.b1 = -(1 + cw); b.b2 = (1 + cw) / 2; break;
            case bandpass: b.b0 = al; b.b1 = 0; b.b2 = -al; break;
            case notch:    b.b0 = 1; b.b1 = -2 * cw; b.b2 = 1; break;
        }
        b.a1 = -2 * cw; b.a2 = 1 - al;
        b.b0 /= a0; b.b1 /= a0; b.b2 /= a0; b.a1 /= a0; b.a2 /= a0;
        return b;
    }
    float step(float x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return float(y); }   // transposed DF-II
    void process(float* p, std::size_t n) { for (std::size_t i = 0; i < n; ++i) p[i] = step(p[i]); }
    double gain(double sr, double f) const {   // |H(e^jw)|
        double w = 2 * M_PI * f / sr; std::complex<double> z = std::polar(1.0, -w);
        return std::abs((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z));
    }
};

} // namespace forge::signal
