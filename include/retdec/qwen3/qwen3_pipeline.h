/**
 * @file include/retdec/qwen3/qwen3_pipeline.h
 * @brief Full Qwen3 inference pipeline — FlashAttention-2 + MoE + generation loop.
 *
 * ## Overview
 *
 *   Qwen3Pipeline is the top-level inference engine.  It ties together:
 *     - Qwen3FlashAttn   — tiled FlashAttention-2 (GPU or CPU)
 *     - Qwen3MoeLayer    — top-K expert routing + dispatch
 *     - KvCacheLayer     — per-layer dense KV cache
 *     - PagedKvCache     — optional paged KV cache for long contexts
 *     - Qwen3Sampler     — repetition-penalised temperature / top-P / top-K
 *     - Qwen3Tokenizer   — BPE tokenizer (loaded from GGUF metadata)
 *     - Qwen3OpenCL      — optional GPU+CPU hybrid GEMV
 *
 *   The pipeline is the only class a user needs to interact with for
 *   end-to-end text generation.
 *
 * ## Forward pass (one decode step)
 *
 *   1. x = embed(token_id)
 *   2. For layer l in [0, numLayers):
 *        a. h = rmsnorm(x, attn_norm_l)
 *        b. q  = gemv(Wq, h)   k  = gemv(Wk, h)   v  = gemv(Wv, h)
 *        c. rope(q, k, pos, headDim, theta)
 *        d. kvCache_l.appendKv(k, v, pos)
 *        e. attn.forward(params, q, kvCache_l.k, kvCache_l.v, attnOut)
 *        f. x += gemv(Wo, attnOut)
 *        g. h = rmsnorm(x, ffn_norm_l)
 *        h. dense model:  x += ffn(h)          (SwiGLU)
 *           MoE model:    x += moeLayers_l(h)  (router + experts)
 *   3. logits = gemv(lm_head, rmsnorm(x, output_norm))
 *
 * ## Prefill vs decode
 *
 *   Prefill  — processes all prompt tokens, building the KV cache.
 *              Returns the log-probability distribution over the next token.
 *   Decode   — advances one step, samples a token, appends to context.
 *
 * ## Paged KV cache
 *
 *   For long contexts, use setPagedKvCache(true) before loading the model.
 *   The paged cache avoids large contiguous allocations.
 *
 * ## Thread safety
 *
 *   Qwen3Pipeline is NOT thread-safe.  Create one pipeline per thread, or
 *   protect all methods with an external lock.
 *
 * ## Quick start
 *
 * @code
 *   Qwen3Pipeline pipe;
 *   pipe.enableOpenCL();
 *   if (!pipe.load("qwen3-1.7b-q4_k_m.gguf")) {
 *       std::cerr << pipe.lastError() << "\n"; return 1;
 *   }
 *   auto result = pipe.generate("Explain decompilation in one sentence.");
 *   std::cout << result.text << "\n";
 * @endcode
 */

#ifndef RETDEC_QWEN3_PIPELINE_H
#define RETDEC_QWEN3_PIPELINE_H

#include "retdec/qwen3/qwen3_attention.h"
#include "retdec/qwen3/qwen3_config.h"
#include "retdec/qwen3/qwen3_moe.h"
#include "retdec/qwen3/qwen3_sampler.h"
#include "retdec/qwen3/qwen3_tokenizer.h"
#include "retdec/qwen3/qwen3_weights.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec::qwen3 {

class Qwen3CUDA;

// ─── PipelineLayerWeights ────────────────────────────────────────────────────

/**
 * @brief Per-layer weight tensor views (raw byte pointers + dtypes).
 *
 * Pointers reference into the raw GGUF memory-mapped region owned by
 * `Qwen3Pipeline::weights_`.  They are valid for the lifetime of the pipeline.
 *
 * For MoE layers, `wGate`/`wUp`/`wDown` are null; `moeGate` and `experts`
 * hold the routing gate and per-expert weight views respectively.
 */
struct PipelineLayerWeights {
    // ── Attention norms ───────────────────────────────────────────────────────
    const float* attnNorm = nullptr;   ///< RMSNorm pre-attention   [hidden]
    const float* ffnNorm  = nullptr;   ///< RMSNorm pre-FFN         [hidden]

    // ── Attention projections ─────────────────────────────────────────────────
    const uint8_t* wq    = nullptr;   GgufDtype wqDtype  = GgufDtype::F32;
    const uint8_t* wk    = nullptr;   GgufDtype wkDtype  = GgufDtype::F32;
    const uint8_t* wv    = nullptr;   GgufDtype wvDtype  = GgufDtype::F32;
    const uint8_t* wo    = nullptr;   GgufDtype woDtype  = GgufDtype::F32;

    // ── Dense FFN (non-MoE layers) ────────────────────────────────────────────
    const uint8_t* wGate = nullptr;   GgufDtype wGateDtype = GgufDtype::F32;
    const uint8_t* wUp   = nullptr;   GgufDtype wUpDtype   = GgufDtype::F32;
    const uint8_t* wDown = nullptr;   GgufDtype wDownDtype = GgufDtype::F32;

    // ── MoE (Mixture-of-Experts) fields ──────────────────────────────────────
    bool isMoE = false;
    const uint8_t* moeGate = nullptr;   GgufDtype moeGateDtype = GgufDtype::F32;
    std::vector<ExpertWeights> experts;  ///< [numExperts] expert weight views
    ExpertWeights sharedExpert;          ///< always-on shared expert (optional)
};

// ─── PipelineGenerateOptions ─────────────────────────────────────────────────

struct PipelineGenerateOptions {
    int   maxNewTokens      = 512;
    float temperature       = 0.6f;
    float topP              = 0.9f;
    int   topK              = 0;         ///< 0 = disabled
    float repetitionPenalty = 1.1f;
    int   seed              = 42;
    bool  enableThinking    = false;     ///< Emit <think>…</think> block

    /// Optional system message (ChatML). If empty, a default assistant prompt is used unless
    /// \ref skipSystemPrompt is true.
    std::string systemPrompt;
    /// If true, omit the ChatML system turn (user + assistant only).
    bool skipSystemPrompt = false;

    std::vector<int32_t> stopTokenIds;   ///< Extra stop token IDs

    /// Called with each generated token ID and its decoded string piece.
    std::function<void(int32_t tokenId, const std::string& piece)> tokenCallback;
    /// Called after each decoded text fragment (for streaming).
    std::function<bool(const std::string&)> streamCallback;
};

// ─── PipelineResult ───────────────────────────────────────────────────────────

struct PipelineResult {
    std::string         text;
    std::vector<int32_t> tokenIds;
    int   promptTokens  = 0;
    int   newTokens     = 0;
    bool  hitEos        = false;
    double prefillMs    = 0.0;   ///< Time for prefill phase
    double decodeMs     = 0.0;   ///< Time for decode phase
    double tokPerSec    = 0.0;
};

// ─── Qwen3Pipeline ────────────────────────────────────────────────────────────

class Qwen3Pipeline {
public:
    Qwen3Pipeline();
    ~Qwen3Pipeline();

    Qwen3Pipeline(const Qwen3Pipeline&)            = delete;
    Qwen3Pipeline& operator=(const Qwen3Pipeline&) = delete;

    // ── Configuration ─────────────────────────────────────────────────────────

    /**
     * @brief Activate paged KV cache for long-context generation.
     *
     * Must be called before load().
     */
    void setPagedKvCache(bool enable) { usePagedKv_ = enable; }

    /**
     * @brief Set the maximum context length used by the KV cache.
     *
     * The model's `maxPositionEmbeddings` (e.g. 131072 for Qwen3-30B) would
     * require ~51 GB of KV cache at full size, which crashes most machines.
     * This method caps the KV cache to a manageable size.  Must be called
     * before load().
     *
     * @param n  Maximum number of tokens to keep in the KV cache.
     *           Defaults to 4096 (good for interactive chat).
     *           Set to 0 to use the model's full maxPositionEmbeddings.
     */
    void setMaxContextLen(int n) { maxContextLen_ = n; }
    int  maxContextLen()   const { return maxContextLen_; }

    /**
     * @brief Initialise a CUDA device (driver check / future use).
     *
     * Matmuls in this pipeline use CPU @c ops::gemv(); this does not register
     * @c ops::setCUDA (MoE CUDA hybrid was unstable with Qt on the same GPU).
     *
     * Safe to call before or after load().
     */
    bool enableCUDA(float gpuFraction = 0.80f, int deviceIndex = -1);
    void disableCUDA();
    bool isCUDAEnabled() const;


    // ── Loading ───────────────────────────────────────────────────────────────

    /**
     * @brief Load a Qwen3 GGUF model file.
     *
     * Parses the GGUF binary, extracts model config, weight tensors, and
     * the embedded BPE vocabulary.  Initialises the FlashAttention-2 kernel
     * (GPU if available, CPU otherwise) and allocates per-layer KV caches.
     *
     * @return true on success.
     */
    bool load(const std::string& ggufPath);

    /**
     * @brief Direct initialisation from pre-loaded config and weights.
     *
     * Used primarily for testing with synthetic weights without a GGUF file.
     *
     * @param cfg      Model configuration
     * @param weights  Raw byte buffers for each layer (ownership transferred)
     * @param tok      Tokenizer to use
     */
    void loadDirect(const Qwen3Config&                      cfg,
                    std::vector<PipelineLayerWeights>        layers,
                    std::vector<uint8_t>                     embedBuf,
                    GgufDtype                                embedDtype,
                    std::vector<uint8_t>                     lmHeadBuf,
                    GgufDtype                                lmHeadDtype,
                    std::vector<float>                       outputNorm,
                    Qwen3Tokenizer                           tok);

    // ── Inference ─────────────────────────────────────────────────────────────

    /**
     * @brief Process a token and update all internal state.
     *
     * Runs one transformer forward pass:
     *   - Uses Qwen3FlashAttn for attention (GPU when available)
     *   - Uses Qwen3MoeLayer for MoE layers
     *   - Updates the per-layer KV cache
     *
     * @param tokenId  Input token ID
     * @param pos      Position in the sequence (0-based)
     */
    void forward(int32_t tokenId, int pos);

    /**
     * @brief Run forward on a batch of tokens (prefill phase).
     *
     * Each token is processed at positions [startPos, startPos + n).
     * Fills logits_ with the distribution after the last token.
     */
    void prefill(const std::vector<int32_t>& tokenIds, int startPos = 0);

    /**
     * @brief Sample the next token from the current logit distribution.
     */
    int32_t sampleNext(const SamplerConfig& cfg,
                       const std::vector<int32_t>& history);

    /**
     * @brief High-level text generation.
     *
     * Applies the Qwen3 ChatML template, runs prefill, then decodes until
     * EOS or maxNewTokens.
     *
     * @param prompt  User text
     * @param opts    Generation options
     */
    PipelineResult generate(const std::string& prompt,
                             const PipelineGenerateOptions& opts = PipelineGenerateOptions{});

    /**
     * @brief Generate from pre-tokenized IDs.
     */
    PipelineResult generateFromIds(const std::vector<int32_t>& promptIds,
                                    const PipelineGenerateOptions& opts = PipelineGenerateOptions{});

    /**
     * @brief Reset the KV cache for a new conversation.
     */
    void resetKvCache();

    /**
     * @brief Unload the model and free all weight memory.
     */
    void unloadModel();

    // ── Accessors ─────────────────────────────────────────────────────────────

    const Qwen3Config&          config()    const { return cfg_; }
    const Qwen3Tokenizer&       tokenizer() const { return tok_; }
    const std::vector<float>&   logits()    const { return logits_; }
    bool                        isLoaded()  const { return loaded_; }
    const std::string&          lastError() const { return lastError_; }
    int                         currentPos() const { return currentPos_; }
    bool                        hasPagedKv() const { return usePagedKv_; }

    /// Access the FlashAttention instance (for configuration / monitoring).
    Qwen3FlashAttn&       flashAttn()       { return flashAttn_; }
    const Qwen3FlashAttn& flashAttn() const { return flashAttn_; }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    void initKvCaches();
    void initFlashAttn();
    void initMoeLayers();

    void runLayer(int layer, float* x, int pos);

    bool loadGgufInternal(const std::string& path);

    // ── Weights ───────────────────────────────────────────────────────────────

    std::vector<PipelineLayerWeights> layers_;

    // Storage for weights loaded from GGUF (layers_ point into these)
    std::vector<std::vector<uint8_t>> layerStorage_;
    std::vector<uint8_t>              embedBuf_;
    GgufDtype                         embedDtype_ = GgufDtype::F32;
    /// Shape/dtype from GGUF for `embedBuf_` — required for correct quantized row strides.
    TensorInfo                        embedTensorInfo_{};
    std::vector<uint8_t>              lmHeadBuf_;
    GgufDtype                         lmHeadDtype_ = GgufDtype::F32;
    TensorInfo                        lmHeadTensorInfo_{};
    std::vector<float>                outputNorm_;

    // ── KV caches ─────────────────────────────────────────────────────────────

    std::vector<KvCacheLayer>    denseCaches_;
    std::vector<PagedKvCache>    pagedCaches_;
    bool                         usePagedKv_  = false;

    // ── Components ────────────────────────────────────────────────────────────

    Qwen3FlashAttn                         flashAttn_;
    std::vector<std::unique_ptr<Qwen3MoeLayer>> moeLayers_;
    std::unique_ptr<Qwen3CUDA>             cuda_;   ///< CUDA GPU backend

    // ── Scratch buffers ───────────────────────────────────────────────────────

    std::vector<float> x_;       // hidden state  [hidden]
    std::vector<float> h_;       // norm output   [hidden]
    std::vector<float> q_;       // query         [nh × headDim]
    std::vector<float> k_;       // key           [nk × headDim]
    std::vector<float> v_;       // value         [nk × headDim]
    std::vector<float> attnOut_; // attn output   [nh × headDim]
    std::vector<float> gate_;    // FFN gate      [intermediate]
    std::vector<float> up_;      // FFN up        [intermediate]
    std::vector<float> logits_;  // output logits [vocabSize]

    // ── State ─────────────────────────────────────────────────────────────────

    Qwen3Config    cfg_;
    Qwen3Tokenizer tok_;
    bool           loaded_     = false;
    int            currentPos_ = 0;
    int            maxContextLen_ = 4096;  ///< Capped KV context window (0 = model max)
    mutable std::string lastError_;
};

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_PIPELINE_H
