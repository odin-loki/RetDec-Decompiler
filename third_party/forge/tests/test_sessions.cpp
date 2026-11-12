#include "check.hpp"
#include <forge/inference/diffusion.hpp>
#include <forge/inference/streaming.hpp>
#include <cmath>
#include <numeric>

using namespace forge::inference;

// Oracle denoiser for data concentrated at mu_c (cond) / mu_u (uncond): the exact epsilon is (x - sqrt(ab) mu)/sqrt(1-ab).
static diffusion::denoiser oracle(const diffusion::noise_scheduler& s, float mu_c, float mu_u, diffusion::prediction p) {
    return [&s, mu_c, mu_u, p](const diffusion::latent& x, double t, int cond) {
        double ab = s.alphas_cumprod[std::size_t(t)]; float mu = cond ? mu_c : mu_u;
        diffusion::latent o(x.size());
        for (std::size_t k = 0; k < x.size(); ++k) {
            double e = (x[k] - std::sqrt(ab) * mu) / std::sqrt(1 - ab);
            o[k] = float(p == diffusion::prediction::epsilon ? e : p == diffusion::prediction::sample ? mu : std::sqrt(ab) * e - std::sqrt(1 - ab) * mu);
        }
        return o;
    };
}
static double max_err(const diffusion::latent& x, float v) { double m = 0; for (float a : x) m = std::max(m, double(std::fabs(a - v))); return m; }

TEST(diffusion_ddim_converges_all_predictions) {
    auto s = diffusion::noise_scheduler::make();
    for (auto p : {diffusion::prediction::epsilon, diffusion::prediction::v, diffusion::prediction::sample}) {
        diffusion::session d(s, oracle(s, 0.7f, 0.f, p));
        diffusion::options o; o.steps = 10; o.pred = p;
        auto x = d.run(d.initial_noise(257, 42), o);
        CHECK_MSG(max_err(x, 0.7f) < 1e-3, "pred %d err %g", int(p), max_err(x, 0.7f));
        CHECK(d.model_calls() == 10);
    }
}
TEST(diffusion_guidance_and_cache_resume) {
    auto s = diffusion::noise_scheduler::make(1000, diffusion::beta_schedule::cosine);
    diffusion::session d(s, oracle(s, 1.f, -1.f, diffusion::prediction::epsilon));
    diffusion::options o; o.steps = 25; o.guidance = 3.f; o.cache_at = {10};
    auto n = d.initial_noise(64, 7);
    auto x = d.run(n, o);
    CHECK(max_err(x, -1.f + 3.f * 2.f) < 1e-2);           // CFG is linear in mu: mu_u + w (mu_c - mu_u) = 5
    CHECK(d.model_calls() == 50);
    CHECK(d.cache().count(10) == 1);
    auto o2 = o; o2.start_step = 10; o2.cache_at = {};
    auto lat = d.cache().at(10);
    auto y = d.run(lat, o2);
    CHECK(y == x);                                         // resuming from the cached latent is bit-identical
    CHECK(d.initial_noise(64, 7) == n && d.initial_noise(64, 8) != n);
    double mean = std::accumulate(n.begin(), n.end(), 0.0) / 64; CHECK(std::fabs(mean) < 0.5);
}
TEST(diffusion_ddpm_stochastic_but_seeded) {
    auto s = diffusion::noise_scheduler::make(1000, diffusion::beta_schedule::linear, 1e-4, 0.02);
    diffusion::session d(s, oracle(s, 0.25f, 0.f, diffusion::prediction::epsilon));
    diffusion::options o; o.steps = 50; o.kind = diffusion::sampler_kind::ddpm; o.seed = 3;
    auto n = d.initial_noise(128, 1);
    auto a = d.run(n, o), b = d.run(n, o);
    CHECK(a == b);
    CHECK(max_err(a, 0.25f) < 1e-2);                       // oracle data has zero variance: the posterior collapses
}
TEST(diffusion_euler_flow) {
    // Flow matching: x_t = (1-t) x0 + t noise, v = noise - x0. Oracle knows x0 = 2, so v = (x - 2)/t... use exact form.
    diffusion::session d(diffusion::noise_scheduler::make(10), [](const diffusion::latent& x, double t, int) {
        double tt = t / 1000; diffusion::latent v(x.size()); for (std::size_t k = 0; k < x.size(); ++k) v[k] = float((x[k] - 2.0) / tt); return v; });
    diffusion::options o; o.steps = 8; o.kind = diffusion::sampler_kind::euler_flow;
    auto x = d.run(d.initial_noise(32, 9), o);
    CHECK(max_err(x, 2.f) < 1e-4);
}
TEST(streaming_framing_matches_offline) {
    std::vector<float> sig(5003); for (std::size_t i = 0; i < sig.size(); ++i) sig[i] = float(i);
    streaming::options o; o.window = 64; o.hop = 24; o.max_lag_frames = 1000; o.capacity = 4096;
    std::vector<float> starts;
    streaming::session s(o, [&](const float* f, std::size_t len, std::size_t idx) {
        CHECK(len == 64); CHECK(f[0] == float(idx * 24)); CHECK(f[63] == f[0] + 63); starts.push_back(f[0]); });
    std::size_t pos = 0, chunk = 1;
    while (pos < sig.size()) { std::size_t k = std::min(chunk, sig.size() - pos); CHECK(s.feed(sig.data() + pos, k) == k); pos += k; chunk = chunk * 3 % 97 + 1; s.pump(); }
    s.pump();
    CHECK(starts.size() == (sig.size() - 64) / 24 + 1);
    CHECK(s.stat().dropped == 0 && s.stat().samples_in == sig.size());
}
TEST(streaming_latency_budget_drops_oldest) {
    streaming::options o; o.window = 10; o.hop = 10; o.max_lag_frames = 3; o.capacity = 1000;
    std::vector<std::size_t> seen;
    streaming::session s(o, [&](const float* f, std::size_t, std::size_t idx) { seen.push_back(idx); CHECK(f[0] == float(idx * 10)); });
    std::vector<float> sig(200); for (std::size_t i = 0; i < 200; ++i) sig[i] = float(i);
    s.feed(sig.data(), 200);                               // consumer stalled: 20 frames pending, budget 3
    CHECK(s.pending_frames() == 20);
    CHECK(s.pump() == 3);
    CHECK(s.stat().dropped == 17);
    CHECK((seen == std::vector<std::size_t>{17, 18, 19}));  // only the freshest frames survive
    streaming::ring<int> r(3); int v[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(r.capacity() == 3);
    CHECK(r.push(v, 8) == 3); int o2[3]; CHECK(r.peek(o2, 3) == 3 && o2[2] == 3); r.consume(2); CHECK(r.push(v + 5, 3) == 2);
    CHECK(r.peek(o2, 3) == 3 && o2[0] == 3 && o2[1] == 6 && o2[2] == 7);
}
