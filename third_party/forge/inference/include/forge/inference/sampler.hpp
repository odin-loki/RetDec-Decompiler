#pragma once
// Composable sampler chain: repetition penalty -> temperature -> top-k -> top-p -> min-p -> typical -> draw,
// or Mirostat v2 (adaptive surprise truncation) in place of the truncation steps.
#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace forge::inference {

struct sampler_config {
    float temperature = 0.f;   // 0 = greedy
    int top_k = 0;             // 0 = off
    float top_p = 1.f;
    float min_p = 0.f;
    float typical_p = 1.f;     // locally typical sampling (Meister et al.); 1 = off
    float mirostat_tau = 0.f;  // Mirostat v2 target surprise in bits; 0 = off
    float mirostat_eta = 0.1f;
    float repetition_penalty = 1.f;
    float presence_penalty = 0.f;   // OpenAI-style: subtract once if the token appeared in the window
    float frequency_penalty = 0.f;  // OpenAI-style: subtract per occurrence in the window
    int repetition_window = 64;
    std::uint64_t seed = 0;
};

class sampler {
public:
    explicit sampler(sampler_config c = {}) : c_(c), rng_(c.seed) {}
    const sampler_config& config() const { return c_; }
    // Optional logit filter run after the repetition penalty (grammar constraints set disallowed logits to -inf).
    std::function<void(std::span<float>)> filter;

    // Full next-token distribution after the whole chain (one-hot when greedy).
    std::vector<double> probs(std::span<const float> logits_in, std::span<const int> history = {}) {
        auto l = prepare(logits_in, history);
        std::vector<double> out(l.size(), 0.0);
        if (c_.temperature <= 0.f) { out[std::size_t(std::max_element(l.begin(), l.end()) - l.begin())] = 1; return out; }
        std::vector<int> idx; std::vector<double> p; truncate(l, idx, p);
        double tot = 0; for (auto x : p) tot += x;
        for (std::size_t i = 0; i < p.size(); ++i) out[std::size_t(idx[i])] = p[i] / tot;
        return out;
    }
    // Draw from an explicit distribution (speculative decoding residuals).
    int draw(std::span<const double> p) {
        double tot = 0; for (auto x : p) tot += x;
        if (tot <= 0) return int(std::max_element(p.begin(), p.end()) - p.begin());
        double u = std::uniform_real_distribution<double>(0, tot)(rng_);
        for (std::size_t i = 0; i < p.size(); ++i) { u -= p[i]; if (u <= 0 && p[i] > 0) return int(i); }
        for (std::size_t i = p.size(); i-- > 0;) if (p[i] > 0) return int(i);
        return 0;
    }
    double mirostat_mu() const { return mu_; }
    double uniform() { return std::uniform_real_distribution<double>(0, 1)(rng_); }

    // Plain greedy: the next token is the argmax of the raw logits (no penalties or filter), so a device can pick it.
    bool plain_greedy() const {
        return c_.temperature <= 0.f && c_.repetition_penalty == 1.f && c_.presence_penalty == 0.f && c_.frequency_penalty == 0.f && !filter && !allow;
    }
    // Optional per-token form of `filter` (grammar_constraint::allows): sample() then tests the likeliest candidates
    // first and runs `filter` over the whole vocabulary only when they do not settle the draw (same result: the
    // draw is over the allowed tokens either way). Grammars mostly allow what the model wants anyway.
    std::function<bool(int)> allow;
    int sample(std::span<const float> logits_in, std::span<const int> history = {}) {
        // plain greedy: argmax over the logits as they are (no copy of a vocabulary-sized vector)
        if (plain_greedy() && !logits_in.empty())
            return int(std::max_element(logits_in.begin(), logits_in.end()) - logits_in.begin());
        if (allow && c_.mirostat_tau <= 0.f && !logits_in.empty()) if (int t = sample_lazy(logits_in, history); t >= 0) return t;
        auto l = prepare(logits_in, history);
        if (c_.temperature <= 0.f) return int(std::max_element(l.begin(), l.end()) - l.begin());
        std::vector<int> idx; std::vector<double> p;
        if (c_.mirostat_tau > 0.f) return mirostat(l);
        truncate(l, idx, p);
        double tot = 0; for (auto x : p) tot += x;
        double u = std::uniform_real_distribution<double>(0, tot)(rng_);
        for (std::size_t i = 0; i < p.size(); ++i) { u -= p[i]; if (u <= 0) return idx[i]; }
        return idx.back();
    }
private:
    // sample() with `allow`: the allowed candidates in logit order, testing at most `budget` tokens; -1 = undecided
    int sample_lazy(std::span<const float> logits_in, std::span<const int> history) {
        constexpr std::size_t budget = 256;
        auto l = prepare(logits_in, history, false);
        auto by_logit = [&](int a, int b) { return l[std::size_t(a)] > l[std::size_t(b)] || (l[std::size_t(a)] == l[std::size_t(b)] && a < b); };
        std::size_t tests = 0;
        if (c_.temperature <= 0.f) {
            std::vector<int> idx(l.size()); std::iota(idx.begin(), idx.end(), 0);
            std::size_t m = std::min(budget, idx.size());
            std::partial_sort(idx.begin(), idx.begin() + std::ptrdiff_t(m), idx.end(), by_logit);
            for (std::size_t i = 0; i < m; ++i) if (std::isfinite(l[std::size_t(idx[i])]) && allow(idx[i])) return idx[i];
            return -1;
        }
        std::vector<int> cand; candidates(l, cand, false);
        std::vector<int> ok;
        const std::size_t want = c_.top_k > 0 ? std::size_t(c_.top_k) : std::size_t(-1);
        for (int t : cand) {
            if (ok.size() >= want) break;
            if (++tests > budget) return -1;
            if (allow(t)) ok.push_back(t);
        }
        if (ok.empty()) return -1;
        // the window is 40 nats below the best *allowed* token: add the tokens between the two floors
        const float fa = l[std::size_t(ok[0])] - 40.f * c_.temperature, f1 = l[std::size_t(cand[0])] - 40.f * c_.temperature;
        if (ok.size() < want && fa < f1) {
            std::vector<int> more;
            for (std::size_t i = 0; i < l.size(); ++i) if (l[i] >= fa && l[i] < f1) more.push_back(int(i));
            std::sort(more.begin(), more.end(), by_logit);
            for (int t : more) {
                if (ok.size() >= want) break;
                if (++tests > budget) return -1;
                if (allow(t)) ok.push_back(t);
            }
        }
        std::vector<double> p; shape(l, ok, p);
        double tot = 0; for (auto x : p) tot += x;
        double u = std::uniform_real_distribution<double>(0, tot)(rng_);
        for (std::size_t i = 0; i < p.size(); ++i) { u -= p[i]; if (u <= 0) return ok[i]; }
        return ok.back();
    }
    std::vector<float> prepare(std::span<const float> logits_in, std::span<const int> history, bool use_filter = true) {
        std::vector<float> l(logits_in.begin(), logits_in.end());
        if (c_.repetition_penalty != 1.f) {
            std::unordered_set<int> seen;
            std::size_t from = history.size() > std::size_t(c_.repetition_window) ? history.size() - std::size_t(c_.repetition_window) : 0;
            for (std::size_t i = from; i < history.size(); ++i) seen.insert(history[i]);
            for (int t : seen) if (t >= 0 && std::size_t(t) < l.size()) l[std::size_t(t)] = l[std::size_t(t)] > 0 ? l[std::size_t(t)] / c_.repetition_penalty : l[std::size_t(t)] * c_.repetition_penalty;
        }
        if (c_.presence_penalty != 0.f || c_.frequency_penalty != 0.f) {
            std::unordered_map<int, int> cnt;
            std::size_t from = history.size() > std::size_t(c_.repetition_window) ? history.size() - std::size_t(c_.repetition_window) : 0;
            for (std::size_t i = from; i < history.size(); ++i) ++cnt[history[i]];
            for (auto [t, n] : cnt) if (t >= 0 && std::size_t(t) < l.size()) l[std::size_t(t)] -= c_.presence_penalty + c_.frequency_penalty * float(n);
        }
        if (filter && use_filter) filter(l);
        return l;
    }
    // sorted candidate indices + unnormalised probabilities after top-k / top-p / min-p
    void truncate(const std::vector<float>& l, std::vector<int>& idx, std::vector<double>& p) { candidates(l, idx, true); shape(l, idx, p); }
    // Candidates: tokens within 40 nats of the best (p / p_max >= e^-40 ~ 4e-18), sorted by logit (ties: lower id).
    // The probability sum in shape() starts at 1 and runs in descending order, so every term left out is under half
    // an ulp of it and would not have changed it: the result equals sorting the whole vocabulary, without sorting it
    // (one scan, then a sort of the few hundred or thousand candidates, or nth_element + sort of top_k).
    void candidates(const std::vector<float>& l, std::vector<int>& idx, bool cut_top_k) const {
        float mxl = -std::numeric_limits<float>::infinity();
        for (float x : l) if (x > mxl) mxl = x;
        const bool fin = std::isfinite(mxl);
        const float floor = fin ? mxl - 40.f * c_.temperature : 0.f;
        idx.clear();
        for (std::size_t i = 0; i < l.size(); ++i) if (!fin || l[i] >= floor) idx.push_back(int(i));
        auto by_logit = [&](int a, int b) { return l[std::size_t(a)] > l[std::size_t(b)] || (l[std::size_t(a)] == l[std::size_t(b)] && a < b); };
        if (cut_top_k && c_.top_k > 0 && idx.size() > std::size_t(c_.top_k)) {
            std::nth_element(idx.begin(), idx.begin() + c_.top_k, idx.end(), by_logit);
            idx.resize(std::size_t(c_.top_k));
        }
        std::sort(idx.begin(), idx.end(), by_logit);
    }
    void shape(const std::vector<float>& l, std::vector<int>& idx, std::vector<double>& p) {
        std::size_t keep = idx.size();
        if (c_.top_k > 0) keep = std::min(keep, std::size_t(c_.top_k));
        p.assign(keep, 0.0);
        double mx = l[std::size_t(idx[0])] / c_.temperature, sum = 0;
        for (std::size_t i = 0; i < keep; ++i) { p[i] = std::exp(double(l[std::size_t(idx[i])]) / c_.temperature - mx); sum += p[i]; }
        for (auto& x : p) x /= sum;
        if (c_.top_p < 1.f) {
            double cum = 0; std::size_t k = 0;
            while (k < keep) { cum += p[k++]; if (cum >= c_.top_p) break; }
            keep = std::max<std::size_t>(k, 1);
        }
        if (c_.min_p > 0.f) {
            std::size_t k = 1; while (k < keep && p[k] >= c_.min_p * p[0]) ++k;
            keep = k;
        }
        if (c_.typical_p < 1.f) {
            double tot = 0, H = 0; for (std::size_t i = 0; i < keep; ++i) tot += p[i];
            for (std::size_t i = 0; i < keep; ++i) if (p[i] > 0) H -= p[i] / tot * std::log(p[i] / tot);
            std::vector<std::size_t> o(keep); std::iota(o.begin(), o.end(), 0);
            auto dev = [&](std::size_t i) { return p[i] > 0 ? std::fabs(-std::log(p[i] / tot) - H) : 1e300; };
            std::stable_sort(o.begin(), o.end(), [&](auto a, auto b) { return dev(a) < dev(b); });
            double cum = 0; std::size_t k = 0; while (k < keep) { cum += p[o[k++]] / tot; if (cum >= c_.typical_p) break; }
            std::vector<int> i2; std::vector<double> p2; for (std::size_t j = 0; j < k; ++j) { i2.push_back(idx[o[j]]); p2.push_back(p[o[j]]); }
            std::copy(i2.begin(), i2.end(), idx.begin()); std::copy(p2.begin(), p2.end(), p.begin()); keep = k;
        }
        // drop -inf (filtered) candidates, keep at least one
        std::size_t k2 = keep; while (k2 > 1 && !(p[k2 - 1] > 0)) --k2;
        keep = k2;
        p.resize(keep); idx.resize(keep);
    }
    // Mirostat v2: keep tokens with surprise -log2 p <= mu, draw, then mu -= eta * (observed surprise - tau).
    int mirostat(const std::vector<float>& l) {
        if (mu_ < 0) mu_ = 2.0 * c_.mirostat_tau;
        double mx = *std::max_element(l.begin(), l.end()) / c_.temperature, s = 0;
        std::vector<double> p(l.size()); for (std::size_t i = 0; i < l.size(); ++i) { p[i] = std::exp(double(l[i]) / c_.temperature - mx); s += p[i]; }
        std::vector<double> q(p.size()); std::size_t best = 0;
        for (std::size_t i = 0; i < p.size(); ++i) { p[i] /= s; if (p[i] > p[best]) best = i; if (p[i] > 0 && -std::log2(p[i]) <= mu_) q[i] = p[i]; }
        q[best] = p[best];
        int t = draw(q);
        mu_ -= c_.mirostat_eta * (-std::log2(p[std::size_t(t)]) - c_.mirostat_tau);
        return t;
    }
    sampler_config c_;
    std::mt19937_64 rng_;
    double mu_ = -1;
};

} // namespace forge::inference
