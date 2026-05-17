/**
 * @file src/qwen3/qwen3_sampler.cpp
 * @brief Token sampling strategies (temperature, top-p, top-k, repetition penalty).
 */

#include "retdec/qwen3/qwen3_sampler.h"
#include "retdec/qwen3/qwen3_ops.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace retdec::qwen3 {

Qwen3Sampler::Qwen3Sampler(const SamplerConfig& cfg)
    : cfg_(cfg), rng_(cfg.seed) {}

void Qwen3Sampler::reseed(int seed) {
    cfg_.seed = seed;
    rng_.seed(seed);
}

// ─── Repetition penalty ───────────────────────────────────────────────────────

void Qwen3Sampler::applyRepetitionPenalty(
        float* logits, int vocabSize,
        const std::vector<TokenId>& history) const {
    if (cfg_.repetitionPenalty <= 1.0f) return;

    int start = std::max(0, static_cast<int>(history.size())
                             - cfg_.repetitionWindow);
    for (int i = start; i < static_cast<int>(history.size()); ++i) {
        int id = history[i];
        if (id < 0 || id >= vocabSize) continue;
        if (logits[id] > 0.f)
            logits[id] /= cfg_.repetitionPenalty;
        else
            logits[id] *= cfg_.repetitionPenalty;
    }
}

// ─── Temperature ─────────────────────────────────────────────────────────────

void Qwen3Sampler::applyTemperature(float* logits, int n) const {
    if (cfg_.temperature <= 0.f || cfg_.temperature == 1.f) return;
    float inv = 1.f / cfg_.temperature;
    for (int i = 0; i < n; ++i) logits[i] *= inv;
}

// ─── Top-K ───────────────────────────────────────────────────────────────────

void Qwen3Sampler::applyTopK(std::vector<std::pair<float,int>>& pairs,
                              int k) const {
    if (k <= 0 || k >= static_cast<int>(pairs.size())) return;
    std::partial_sort(pairs.begin(), pairs.begin() + k, pairs.end(),
                      [](const auto& a, const auto& b){
                          return a.first > b.first;
                      });
    pairs.resize(k);
}

// ─── Sample from (prob, id) pairs with top-p ─────────────────────────────────

TokenId Qwen3Sampler::sampleFromPairs(
        std::vector<std::pair<float,int>>& pairs, float topP) {
    // Convert logits → probs
    float maxL = pairs[0].first;
    for (auto& [p,_] : pairs) p = std::exp(p - maxL);
    float sum = 0.f;
    for (auto& [p,_] : pairs) sum += p;
    for (auto& [p,_] : pairs) p /= sum;

    // Sort descending by prob for nucleus filtering
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    // Top-P cutoff
    if (topP < 1.f) {
        float cum = 0.f;
        int cutoff = static_cast<int>(pairs.size());
        for (int i = 0; i < static_cast<int>(pairs.size()); ++i) {
            cum += pairs[i].first;
            if (cum >= topP) { cutoff = i + 1; break; }
        }
        pairs.resize(cutoff);
    }

    // Sample
    std::uniform_real_distribution<float> dist(0.f, 1.f);
    float r = dist(rng_);
    float cum = 0.f;
    for (auto& [p, id] : pairs) {
        cum += p;
        if (r < cum) return id;
    }
    return pairs.back().second;
}

// ─── Main sample ─────────────────────────────────────────────────────────────

TokenId Qwen3Sampler::sample(float* logits, int vocabSize,
                              const std::vector<TokenId>& history) {
    applyRepetitionPenalty(logits, vocabSize, history);

    if (cfg_.temperature <= 0.f) {
        return greedy(logits, vocabSize);
    }

    applyTemperature(logits, vocabSize);

    // Build (logit, id) pairs
    std::vector<std::pair<float,int>> pairs(vocabSize);
    for (int i = 0; i < vocabSize; ++i) pairs[i] = {logits[i], i};

    applyTopK(pairs, cfg_.topK);
    return sampleFromPairs(pairs, cfg_.topP);
}

// ─── Greedy ───────────────────────────────────────────────────────────────────

TokenId Qwen3Sampler::greedy(const float* logits, int vocabSize) {
    return ops::argmax(logits, vocabSize);
}

} // namespace retdec::qwen3
