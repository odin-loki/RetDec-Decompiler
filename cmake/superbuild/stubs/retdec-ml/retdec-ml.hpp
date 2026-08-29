#pragma once

// Parked ML stub (not src/qwen3; C-QWEN3-GPU withdrawn).
//
// Superbuild-only stub so existing consumers can still compile when
// RETDEC_SUPERBUILD_ENABLE_ML_FRAMEWORK is enabled. Not a product API
// and not an in-tree src/qwen3 stack (C-QWEN3-GPU withdrawn).

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::ml {

// ─── Streaming callback ───────────────────────────────────────────────────────
// Called once per generated token; return false to abort generation.
using TokenCallback = std::function<bool(int tokenId, std::string_view tokenText, float logprob)>;

// ─── Sampling parameters ──────────────────────────────────────────────────────
struct SamplingParams {
    float temperature    = 0.7f;
    float topP           = 0.95f;
    float repetitionPenalty = 1.1f;
    int   maxNewTokens   = 2048;
    bool  greedy         = false;
};

// ─── Model configuration (parsed from config.json + tokenizer.json) ───────────
struct ModelConfig {
    int         numLayers          = 0;
    int         hiddenSize         = 0;
    int         numHeads           = 0;
    int         numKVHeads         = 0;   // GQA
    int         numExperts         = 0;   // MoE total
    int         expertsPerToken    = 0;   // top-K active
    float       ropeTheta          = 10000.0f;
    int         vocabSize          = 0;
    int         maxContextLen      = 32768;
    std::string modelName;
};

// ─── MLModel — main inference object ─────────────────────────────────────────
class MLModel {
public:
    // Load weights + tokenizer from a directory or .gguf file.
    // Returns true on success. Sets errorMessage() on failure.
    bool loadFromPath(const char* path);

    bool isLoaded() const noexcept { return _loaded; }

    const ModelConfig& config() const noexcept { return _config; }

    const std::string& errorMessage() const noexcept { return _error; }

    // Tokenise a string into token IDs.
    std::vector<int> encode(std::string_view text) const;

    // Decode token IDs back to a string.
    std::string decode(const std::vector<int>& ids) const;

    // Run autoregressive generation. Calls callback once per token.
    // Returns the full generated text.
    std::string generate(std::string_view prompt,
                         const SamplingParams& params = {},
                         TokenCallback callback = {});

private:
    bool        _loaded = false;
    ModelConfig _config;
    std::string _error;
};

} // namespace retdec::ml

// Backwards-compatibility alias.
namespace retdec {
    using MLModel = ml::MLModel;
}
