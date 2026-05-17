/**
 * @file include/retdec/qwen3/qwen3_sampler.h
 * @brief Token sampling strategies for Qwen3 generation.
 *
 * Sampling pipeline applied in order:
 *   1. Repetition penalty  — lower logits for recently seen tokens
 *   2. Temperature         — scale logits by 1/T
 *   3. Top-K filtering     — keep only K highest-probability tokens
 *   4. Top-P (nucleus)     — keep smallest set with cumulative prob ≥ P
 *   5. Sample              — draw from the remaining distribution
 *                            (or argmax if temperature == 0)
 */

#ifndef RETDEC_QWEN3_SAMPLER_H
#define RETDEC_QWEN3_SAMPLER_H

#include "retdec/qwen3/qwen3_tokenizer.h"

#include <cstdint>
#include <random>
#include <vector>

namespace retdec::qwen3 {

struct SamplerConfig {
    float temperature       = 0.6f;
    float topP              = 0.9f;
    int   topK              = 0;      ///< 0 = disabled
    float repetitionPenalty = 1.1f;   ///< 1.0 = no penalty
    int   repetitionWindow  = 64;     ///< Tokens to look back
    int   seed              = 42;
};

class Qwen3Sampler {
public:
    explicit Qwen3Sampler(const SamplerConfig& cfg = SamplerConfig{});

    /**
     * @brief Sample the next token from logits.
     *
     * @param logits      Raw logits [vocabSize] from the model forward pass.
     *                    Modified in place (repetition penalty, temperature,
     *                    top-k/top-p filtering).
     * @param vocabSize   Number of vocabulary entries.
     * @param history     Tokens generated so far (for repetition penalty).
     * @return            Sampled token ID.
     */
    TokenId sample(float* logits, int vocabSize,
                   const std::vector<TokenId>& history);

    /// Greedy decode (argmax) — ignores temperature/top-p/top-k.
    static TokenId greedy(const float* logits, int vocabSize);

    void reseed(int seed);

    const SamplerConfig& config() const { return cfg_; }

private:
    void applyRepetitionPenalty(float* logits, int vocabSize,
                                const std::vector<TokenId>& history) const;
    void applyTemperature(float* logits, int n) const;
    void applyTopK(std::vector<std::pair<float,int>>& pairs, int k) const;
    TokenId sampleFromPairs(std::vector<std::pair<float,int>>& pairs,
                             float topP);

    SamplerConfig cfg_;
    std::mt19937  rng_;
};

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_SAMPLER_H
