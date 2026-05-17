/**
 * @file include/retdec/qwen3/qwen3_model.h
 * @brief Qwen3 transformer model — load and run.
 *
 * ## Quick start
 *
 *   Qwen3Model model;
 *   if (!model.loadGguf("qwen3-1.7b-q4_k_m.gguf")) {
 *       std::cerr << model.lastError() << "\n";
 *       return 1;
 *   }
 *
 *   // The vocabulary is embedded in the GGUF file — no extra files needed.
 *   auto result = model.generate("What is the main() function?",
 *                                {.maxNewTokens = 256,
 *                                 .temperature  = 0.7f,
 *                                 .topP         = 0.9f});
 *   std::cout << result.text << "\n";
 *
 * ## Architecture (Qwen3 dense)
 *
 *   x = embed(token_ids)            // [seq, hidden]
 *   for layer in 0..n_layers:
 *     h = rmsnorm(x, attn_norm)
 *     q, k, v = linear(Wq,h), linear(Wk,h), linear(Wv,h)
 *     rope(q, k, pos)
 *     kv_cache[layer].append(k, v)
 *     attn = gqa_attention(q, kv_cache[layer])
 *     x = x + linear(Wo, attn)
 *     h = rmsnorm(x, ffn_norm)
 *     x = x + linear(Wd, silu(linear(Wg,h)) * linear(Wu,h))
 *   logits = linear(lm_head, rmsnorm(x, output_norm))
 */

#ifndef RETDEC_QWEN3_MODEL_H
#define RETDEC_QWEN3_MODEL_H

#include <memory>
#include "retdec/qwen3/qwen3_config.h"
#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_tokenizer.h"
#include "retdec/qwen3/qwen3_weights.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace retdec::qwen3 {

// ─── GenerateOptions ──────────────────────────────────────────────────────────

struct GenerateOptions {
    int   maxNewTokens  = 512;    ///< Maximum tokens to generate
    float temperature   = 0.6f;   ///< Sampling temperature (0 = greedy)
    float topP          = 0.9f;   ///< Nucleus sampling probability
    int   topK          = 0;      ///< Top-K sampling (0 = disabled)
    float repetitionPenalty = 1.1f; ///< Penalise recently generated tokens
    int   seed          = 42;     ///< RNG seed for reproducibility
    bool  enableThinking = false; ///< Qwen3 reasoning mode (<think>…</think>)
    std::vector<TokenId> stopTokens; ///< Extra stop token IDs

    /// Called with each generated token ID (for streaming output).
    std::function<void(TokenId)> tokenCallback;
    /// Called with each decoded token string (for streaming text).
    std::function<void(const std::string&)> textCallback;
};

// ─── GenerateResult ───────────────────────────────────────────────────────────

struct GenerateResult {
    std::string      text;             ///< Full generated text
    std::vector<TokenId> tokenIds;     ///< Raw token IDs generated
    int              promptTokens = 0; ///< Tokens in the prompt
    int              newTokens    = 0; ///< Tokens generated
    double           tokensPerSec = 0; ///< Generation throughput
    bool             hitEos       = false; ///< Ended on EOS token
};

// ─── KvCache ─────────────────────────────────────────────────────────────────

/**
 * @brief Per-layer KV cache.  Stores keys and values for all past tokens.
 */
struct KvLayer {
    std::vector<float> keys;   ///< [maxSeq × nk × headDim]
    std::vector<float> values; ///< [maxSeq × nk × headDim]
    int                seqLen = 0;
};

// ─── LayerWeights ─────────────────────────────────────────────────────────────

/**
 * @brief All weight tensors for a single transformer block.
 * Weights are stored in their native GGUF byte format.
 */
struct LayerWeights {
    std::vector<uint8_t> attnNorm;  GgufDtype attnNormDtype = GgufDtype::F32;
    std::vector<uint8_t> wq;        GgufDtype wqDtype  = GgufDtype::F32;
    std::vector<uint8_t> wk;        GgufDtype wkDtype  = GgufDtype::F32;
    std::vector<uint8_t> wv;        GgufDtype wvDtype  = GgufDtype::F32;
    std::vector<uint8_t> wo;        GgufDtype woDtype  = GgufDtype::F32;
    std::vector<uint8_t> ffnNorm;   GgufDtype ffnNormDtype = GgufDtype::F32;
    std::vector<uint8_t> wGate;     GgufDtype wGateDtype = GgufDtype::F32;
    std::vector<uint8_t> wUp;       GgufDtype wUpDtype  = GgufDtype::F32;
    std::vector<uint8_t> wDown;     GgufDtype wDownDtype = GgufDtype::F32;
};

// ─── Qwen3Model ───────────────────────────────────────────────────────────────

class Qwen3Model {
public:
    Qwen3Model();
    ~Qwen3Model();

    // ── CUDA ─────────────────────────────────────────────────────────────────

    /**
     * @brief Enable GPU+CPU hybrid inference via CUDA.
     *
     * Creates and initialises a Qwen3CUDA instance, then pre-uploads all
     * weight tensors to GPU memory.  Falls back gracefully if no GPU is found.
     *
     * @param gpuFraction  Fraction of each GEMV dispatched to the GPU [0,1].
     * @param deviceIndex  CUDA device index (-1 = auto).
     * @return true if CUDA was successfully initialised.
     */
    bool enableCUDA(float gpuFraction = 0.80f, int deviceIndex = -1);

    /** @brief Disable CUDA and revert to CPU-only inference. */
    void disableCUDA();

    bool isCUDAEnabled() const { return cuda_ && cuda_->isReady(); }
    Qwen3CUDA* cuda()           { return cuda_.get(); }
    const Qwen3CUDA* cuda() const { return cuda_.get(); }

    // ── Loading ───────────────────────────────────────────────────────────────

    /**
     * @brief Load a Qwen3 GGUF model file.
     *
     * Reads the model configuration and all weight tensors from the GGUF file.
     * Also extracts the vocabulary and BPE merges embedded in the GGUF metadata
     * to initialize the tokenizer — no separate tokenizer files are required.
     *
     * @param path  Path to the `.gguf` file.
     * @return true on success.
     */
    bool loadGguf(const std::string& path);

    /**
     * @brief Load tokenizer from a separate `tokenizer.json` file.
     *
     * Call after loadGguf() if you want to override the embedded vocabulary.
     */
    bool loadTokenizer(const std::string& tokenizerJsonPath);

    // ── Generation ────────────────────────────────────────────────────────────

    /**
     * @brief Generate text from a plain-text prompt.
     *
     * Applies the Qwen3 ChatML template internally:
     *   <|im_start|>user\n{prompt}<|im_end|>\n<|im_start|>assistant\n
     *
     * @param prompt  User prompt text.
     * @param opts    Generation options (temperature, max tokens, etc.)
     * @return GenerateResult containing the generated text and statistics.
     */
    GenerateResult generate(const std::string& prompt,
                             const GenerateOptions& opts = GenerateOptions{});

    /**
     * @brief Generate from pre-tokenized IDs (full control over the prompt).
     */
    GenerateResult generateFromIds(const TokenIds& promptIds,
                                    const GenerateOptions& opts = GenerateOptions{});

    /**
     * @brief Run the model on a single token at the given position.
     *
     * Fills `logits_` with unnormalized log-probabilities for the next token.
     *
     * @param tokenId  Current input token.
     * @param pos      Absolute position in the sequence (0-based).
     */
    void forward(TokenId tokenId, int pos);

    /**
     * @brief Reset the KV cache (start a new conversation).
     */
    void resetKvCache();

    // ── Accessors ─────────────────────────────────────────────────────────────

    const Qwen3Config&     config()    const { return cfg_; }
    const Qwen3Tokenizer&  tokenizer() const { return tok_; }
    Qwen3Tokenizer&        tokenizer()       { return tok_; }
    bool                   isLoaded()  const { return loaded_; }
    const std::string&     lastError() const { return lastError_; }

    /// Raw logit vector after the last forward() call (size = vocabSize).
    const std::vector<float>& logits() const { return logits_; }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    bool loadWeightsFromGguf(Qwen3Weights& w);
    bool loadVocabFromGguf(Qwen3Weights& w);
    bool extractTensor(Qwen3Weights& w, const std::string& name,
                       std::vector<uint8_t>& out, GgufDtype& dtype);

    void runLayer(int layer, float* x, int pos);

    // ── Weights ───────────────────────────────────────────────────────────────

    std::vector<uint8_t> embedWeight_;    GgufDtype embedDtype_ = GgufDtype::F32;
    std::vector<uint8_t> outputNorm_;     // F32 always
    std::vector<uint8_t> lmHead_;         GgufDtype lmHeadDtype_ = GgufDtype::F32;
    bool                 lmHeadTied_ = false; ///< lm_head shares embed weights

    std::vector<LayerWeights> layers_;

    // ── KV cache ──────────────────────────────────────────────────────────────

    std::vector<KvLayer> kvCache_;

    // ── Scratch buffers (reused across forward() calls) ───────────────────────

    std::vector<float> x_;       // hidden state [hidden]
    std::vector<float> h_;       // norm output  [hidden]
    std::vector<float> q_;       // query [nh*headDim]
    std::vector<float> k_;       // key   [nk*headDim]
    std::vector<float> v_;       // value [nk*headDim]
    std::vector<float> attnOut_; // attention output [nh*headDim]
    std::vector<float> gate_;    // FFN gate [intermediate]
    std::vector<float> up_;      // FFN up   [intermediate]
    std::vector<float> logits_;  // [vocabSize]
    std::vector<float> scratch_; // softmax scratch [maxSeq]

    Qwen3Config    cfg_;
    Qwen3Tokenizer tok_;
    bool           loaded_ = false;
    std::string    lastError_;

    std::unique_ptr<Qwen3CUDA> cuda_;
};

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_MODEL_H
