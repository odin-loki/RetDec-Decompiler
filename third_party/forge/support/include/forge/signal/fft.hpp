#pragma once
// FFT (iterative radix-2 for powers of two, Bluestein for any other length), real FFT, STFT with a Hann
// window and an HTK/Slaney-style mel filterbank (Slaney area normalisation, as in librosa defaults).
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace forge::signal {
using cd = std::complex<double>;
inline void fft_pow2(std::vector<cd>& a, bool inv) {
    std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) { std::size_t b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) std::swap(a[i], a[j]); }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * M_PI / double(len) * (inv ? 1 : -1);
        for (std::size_t i = 0; i < n; i += len)
            for (std::size_t k = 0; k < len / 2; ++k) {
                cd w = std::polar(1.0, ang * double(k)), u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
    }
}
// Unnormalised forward transform; the inverse divides by n.
inline void fft(std::vector<cd>& a, bool inv = false) {
    std::size_t n = a.size(); if (n <= 1) return;
    if ((n & (n - 1)) == 0) fft_pow2(a, inv);
    else {
        std::size_t m = 1; while (m < 2 * n - 1) m <<= 1;
        std::vector<cd> w(n), A(m), B(m);
        for (std::size_t k = 0; k < n; ++k) { double t = M_PI * double((k * k) % (2 * n)) / double(n); w[k] = std::polar(1.0, inv ? t : -t); }
        for (std::size_t k = 0; k < n; ++k) A[k] = a[k] * w[k];
        B[0] = std::conj(w[0]); for (std::size_t k = 1; k < n; ++k) B[k] = B[m - k] = std::conj(w[k]);
        fft_pow2(A, false); fft_pow2(B, false); for (std::size_t i = 0; i < m; ++i) A[i] *= B[i]; fft_pow2(A, true);
        for (std::size_t k = 0; k < n; ++k) a[k] = A[k] / double(m) * w[k];
    }
    if (inv) for (auto& x : a) x /= double(n);
}
inline std::vector<cd> rfft(const float* x, std::size_t n) {
    std::vector<cd> a(x, x + n); fft(a); a.resize(n / 2 + 1); return a;
}
inline std::vector<float> hann(std::size_t n) { std::vector<float> w(n); for (std::size_t i = 0; i < n; ++i) w[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(n))); return w; }
// Power spectrogram: frames x (n_fft/2+1), centred with reflect padding (librosa default).
inline std::vector<std::vector<float>> stft_power(const std::vector<float>& x, std::size_t n_fft, std::size_t hop) {
    std::size_t pad = n_fft / 2; if (x.size() <= pad) throw std::invalid_argument("stft: signal too short");
    std::vector<float> y(x.size() + 2 * pad);
    for (std::size_t i = 0; i < y.size(); ++i) {
        long j = long(i) - long(pad), n = long(x.size());
        if (j < 0) j = -j; if (j >= n) j = 2 * (n - 1) - j;
        y[i] = x[std::size_t(j)];
    }
    auto w = hann(n_fft); std::vector<std::vector<float>> S; std::vector<float> fr(n_fft);
    for (std::size_t s = 0; s + n_fft <= y.size(); s += hop) {
        for (std::size_t i = 0; i < n_fft; ++i) fr[i] = y[s + i] * w[i];
        auto X = rfft(fr.data(), n_fft); std::vector<float> p(X.size()); for (std::size_t k = 0; k < X.size(); ++k) p[k] = float(std::norm(X[k]));
        S.push_back(std::move(p));
    }
    return S;
}
inline double hz_to_mel(double f) { double fs = 200.0 / 3, mel = f / fs, minlog = 1000.0, ml = minlog / fs, step = std::log(6.4) / 27.0; return f >= minlog ? ml + std::log(f / minlog) / step : mel; }
inline double mel_to_hz(double m) { double fs = 200.0 / 3, minlog = 1000.0, ml = minlog / fs, step = std::log(6.4) / 27.0; return m >= ml ? minlog * std::exp(step * (m - ml)) : fs * m; }
// n_mels x (n_fft/2+1) Slaney mel filterbank.
inline std::vector<std::vector<float>> mel_filters(double sr, std::size_t n_fft, std::size_t n_mels, double fmin = 0, double fmax = -1) {
    if (fmax < 0) fmax = sr / 2; std::size_t nb = n_fft / 2 + 1;
    std::vector<double> pts(n_mels + 2); double a = hz_to_mel(fmin), b = hz_to_mel(fmax);
    for (std::size_t i = 0; i < pts.size(); ++i) pts[i] = mel_to_hz(a + (b - a) * double(i) / double(n_mels + 1));
    std::vector<std::vector<float>> F(n_mels, std::vector<float>(nb));
    for (std::size_t m = 0; m < n_mels; ++m) {
        double en = 2.0 / (pts[m + 2] - pts[m]);
        for (std::size_t k = 0; k < nb; ++k) {
            double f = sr * double(k) / double(n_fft), lo = (f - pts[m]) / (pts[m + 1] - pts[m]), hi = (pts[m + 2] - f) / (pts[m + 2] - pts[m + 1]);
            F[m][k] = float(std::max(0.0, std::min(lo, hi)) * en);
        }
    }
    return F;
}
}
