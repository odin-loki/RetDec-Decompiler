#pragma once
// Session kind `diffusion_loop`: noise scheduler + step plan + classifier-free guidance + latent cache.
// Model-agnostic: the denoiser is any callable (x_t, t, cond) -> prediction, so a blueprint plan, a UNet/DiT
// session or a test oracle all plug in. Noise comes from Philox, so a (seed, plan) pair gives the same
// trajectory on every backend.
#include <forge/rand/philox.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace forge::inference::diffusion {

using latent = std::vector<float>;

enum class beta_schedule { linear, scaled_linear, cosine };
enum class prediction { epsilon, v, sample };          // what the denoiser outputs
enum class sampler_kind { ddim, ddpm, euler_flow };      // euler_flow: rectified graph / graph matching (velocity)

struct noise_scheduler {
    std::vector<double> alphas_cumprod;                  // abar_t, t = 0..T-1
    static noise_scheduler make(int T = 1000, beta_schedule s = beta_schedule::scaled_linear, double b0 = 0.00085, double b1 = 0.012) {
        noise_scheduler n; n.alphas_cumprod.resize(std::size_t(T));
        double prod = 1;
        for (int t = 0; t < T; ++t) {
            double beta;
            if (s == beta_schedule::linear) beta = b0 + (b1 - b0) * t / std::max(1, T - 1);
            else if (s == beta_schedule::scaled_linear) { double a = std::sqrt(b0) + (std::sqrt(b1) - std::sqrt(b0)) * t / std::max(1, T - 1); beta = a * a; }
            else {
                auto f = [&](double u) { double c = std::cos((u / T + 0.008) / 1.008 * M_PI / 2); return c * c; };
                beta = std::min(0.999, 1 - f(t + 1) / f(t));
            }
            prod *= 1 - beta; n.alphas_cumprod[std::size_t(t)] = prod;
        }
        return n;
    }
    int steps() const { return int(alphas_cumprod.size()); }
};

// Step plan: which timesteps to visit, high noise first ("leading"/"trailing" spacing as in common samplers).
inline std::vector<int> step_plan(int T, int n, bool trailing = true) {
    if (n <= 0 || n > T) throw std::invalid_argument("diffusion: bad step count");
    std::vector<int> ts;
    for (int i = 0; i < n; ++i) ts.push_back(trailing ? int(std::lround(T - 1 - double(i) * T / n)) : int((n - 1 - i) * (T / n)));
    return ts;
}

struct options {
    int steps = 20;
    sampler_kind kind = sampler_kind::ddim;
    prediction pred = prediction::epsilon;
    float guidance = 1.f;                                // classifier-free guidance scale; 1 = conditional only
    double eta = 0.0;                                    // DDIM stochasticity (0 = deterministic); DDPM uses 1
    std::uint64_t seed = 0;
    std::vector<int> cache_at;                           // step indices whose latents are kept in the cache
    int start_step = 0;                                  // resume from a cached step (img2img / interruption)
};

// denoiser(x, t, cond): cond = 1 for the conditional branch, 0 for unconditional. For euler_flow, t is in
// [0, 1] scaled by 1000 and the model predicts velocity v = noise - x0.
using denoiser = std::function<latent(const latent&, double t, int cond)>;

class session {
public:
    session(noise_scheduler s, denoiser d) : sch_(std::move(s)), den_(std::move(d)) {}

    latent initial_noise(std::size_t n, std::uint64_t seed, std::uint64_t stream = 0) const { return gaussian(n, seed, stream); }

    // Runs the plan from `x` (pure noise for start_step == 0, or a cached latent). Returns x0.
    latent run(latent x, const options& o) {
        cache_.clear();
        std::vector<int> ts = o.kind == sampler_kind::euler_flow ? std::vector<int>{} : step_plan(sch_.steps(), o.steps);
        for (int i = o.start_step; i < o.steps; ++i) {
            if (o.kind == sampler_kind::euler_flow) {
                double t = 1.0 - double(i) / o.steps, tn = 1.0 - double(i + 1) / o.steps;
                latent v = guided(x, t * 1000, o);
                for (std::size_t k = 0; k < x.size(); ++k) x[k] += float((tn - t) * v[k]);
            } else {
                int t = ts[std::size_t(i)], tp = i + 1 < o.steps ? ts[std::size_t(i + 1)] : -1;
                double ab = sch_.alphas_cumprod[std::size_t(t)], abp = tp >= 0 ? sch_.alphas_cumprod[std::size_t(tp)] : 1.0;
                latent m = guided(x, t, o);
                latent eps(x.size()), x0(x.size());
                for (std::size_t k = 0; k < x.size(); ++k) {
                    double xt = x[k], e, z;
                    if (o.pred == prediction::epsilon) { e = m[k]; z = (xt - std::sqrt(1 - ab) * e) / std::sqrt(ab); }
                    else if (o.pred == prediction::sample) { z = m[k]; e = (xt - std::sqrt(ab) * z) / std::sqrt(1 - ab); }
                    else { z = std::sqrt(ab) * xt - std::sqrt(1 - ab) * m[k]; e = std::sqrt(1 - ab) * xt + std::sqrt(ab) * m[k]; }
                    eps[k] = float(e); x0[k] = float(z);
                }
                double eta = o.kind == sampler_kind::ddpm ? 1.0 : o.eta;
                double sigma = tp >= 0 ? eta * std::sqrt((1 - abp) / (1 - ab) * (1 - ab / abp)) : 0.0;
                latent nz = sigma > 0 ? gaussian(x.size(), o.seed, std::uint64_t(i) + 1) : latent{};
                double dir = std::sqrt(std::max(0.0, 1 - abp - sigma * sigma));
                for (std::size_t k = 0; k < x.size(); ++k)
                    x[k] = float(std::sqrt(abp) * x0[k] + dir * eps[k] + (sigma > 0 ? sigma * nz[k] : 0.0));
            }
            for (int c : o.cache_at) if (c == i + 1) cache_[i + 1] = x;
        }
        return x;
    }
    // Latent cache: step index (number of completed steps) -> latent. Feed one back with start_step to resume.
    const std::map<int, latent>& cache() const { return cache_; }
    std::size_t model_calls() const { return calls_; }

private:
    latent guided(const latent& x, double t, const options& o) {
        if (o.guidance == 1.f) { ++calls_; return den_(x, t, 1); }
        latent c = den_(x, t, 1), u = den_(x, t, 0); calls_ += 2;
        for (std::size_t k = 0; k < c.size(); ++k) c[k] = u[k] + o.guidance * (c[k] - u[k]);
        return c;
    }
    static latent gaussian(std::size_t n, std::uint64_t seed, std::uint64_t stream) {
        rand::philox4x32 g{{std::uint32_t(seed), std::uint32_t(seed >> 32)}};
        latent out(n);
        for (std::size_t i = 0; i < n; i += 2) {              // Box-Muller on Philox lanes
            auto r = g(i / 2, stream);
            double u1 = (double(r[0] >> 8) + 0.5) / 16777216.0, u2 = double(r[1] >> 8) / 16777216.0;
            double rad = std::sqrt(-2 * std::log(u1));
            out[i] = float(rad * std::cos(2 * M_PI * u2));
            if (i + 1 < n) out[i + 1] = float(rad * std::sin(2 * M_PI * u2));
        }
        return out;
    }
    noise_scheduler sch_; denoiser den_; std::map<int, latent> cache_; std::size_t calls_ = 0;
};

} // namespace forge::inference::diffusion
