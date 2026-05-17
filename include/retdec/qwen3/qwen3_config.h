/**
 * @file include/retdec/qwen3/qwen3_config.h
 * @brief Qwen3 model configuration — hyper-parameters parsed from
 *        `config.json` (Hugging Face format) or from GGUF metadata.
 *
 * Supported model sizes:
 *   Qwen3-0.6B   Qwen3-1.7B   Qwen3-4B   Qwen3-8B
 *   Qwen3-14B    Qwen3-32B    Qwen3-30B-A3B (MoE)
 *   Qwen3-235B-A22B (MoE)
 *
 * Architecture: Grouped-Query Attention (GQA), RoPE, SwiGLU FFN.
 * MoE variants add expert routing metadata.
 */

#ifndef RETDEC_QWEN3_CONFIG_H
#define RETDEC_QWEN3_CONFIG_H

#include <cstdint>
#include <string>

namespace retdec::qwen3 {

// ─── Qwen3Config ─────────────────────────────────────────────────────────────

struct Qwen3Config {
    // ── Vocabulary ────────────────────────────────────────────────────────────
    uint32_t vocabSize     = 151936; ///< Token vocabulary size
    uint32_t padTokenId    = 0;
    uint32_t eosTokenId    = 151643; ///< <|endoftext|>
    uint32_t imStartId     = 151644; ///< <|im_start|>
    uint32_t imEndId       = 151645; ///< <|im_end|>
    uint32_t thinkStartId  = 151646; ///< <|think|>   (Qwen3 reasoning tokens)
    uint32_t thinkEndId    = 151647; ///< </|think|>

    // ── Transformer dimensions ────────────────────────────────────────────────
    uint32_t hiddenSize    = 2048;   ///< Embedding / hidden dimension
    uint32_t numLayers     = 28;     ///< Number of transformer blocks
    uint32_t numHeads      = 16;     ///< Number of query attention heads
    uint32_t numKvHeads    = 8;      ///< GQA key/value heads (< numHeads = GQA)
    uint32_t headDim       = 128;    ///< Per-head dimension (hiddenSize / numHeads)
    uint32_t intermediateSize = 11008; ///< FFN intermediate dimension (SwiGLU gate)

    // ── Context and generation ────────────────────────────────────────────────
    uint32_t maxPositionEmbeddings = 40960; ///< Maximum sequence length (40K)
    float    ropeTheta             = 1000000.0f; ///< RoPE base frequency

    // ── Quantisation hint (from GGUF) ─────────────────────────────────────────
    std::string fileType; ///< e.g. "Q4_K_M", "F16", "BF16"

    // ── MoE (Mixture of Experts) — zero for dense models ─────────────────────
    uint32_t numExperts        = 0;  ///< Total routable experts (0 = dense)
    uint32_t numExpertsPerTok  = 0;  ///< Top-K experts activated per token

    // ── Model identity ────────────────────────────────────────────────────────
    std::string modelType   = "qwen3";
    std::string archName;            ///< e.g. "Qwen3ForCausalLM"

    // ── Derived helpers ───────────────────────────────────────────────────────

    bool isMoE()       const { return numExperts > 0; }
    bool isGQA()       const { return numKvHeads > 0 && numKvHeads < numHeads; }
    bool hasMHA()      const { return numKvHeads == numHeads; }
    uint32_t kvRepeat() const {
        if (numKvHeads == 0) return 1;
        return numHeads / numKvHeads;
    }
    uint64_t estimatedParamsM() const {
        // Very rough: embedding + n_layers * (attn + ffn) parameters in millions
        uint64_t emb = static_cast<uint64_t>(vocabSize) * hiddenSize * 2; // embed + lm_head
        uint64_t attn = static_cast<uint64_t>(numHeads + numKvHeads * 2 + numHeads) *
                        headDim * hiddenSize;
        uint64_t ffn  = static_cast<uint64_t>(intermediateSize) * hiddenSize * 3;
        return (emb + numLayers * (attn + ffn)) / 1'000'000;
    }
};

// ─── Well-known model presets ─────────────────────────────────────────────────

namespace presets {

inline Qwen3Config qwen3_0_6B() {
    Qwen3Config c;
    c.hiddenSize       = 1024;
    c.numLayers        = 28;
    c.numHeads         = 16;
    c.numKvHeads       = 8;
    c.headDim          = 64;
    c.intermediateSize = 3072;
    return c;
}

inline Qwen3Config qwen3_1_7B() {
    Qwen3Config c;
    c.hiddenSize       = 2048;
    c.numLayers        = 28;
    c.numHeads         = 16;
    c.numKvHeads       = 8;
    c.headDim          = 128;
    c.intermediateSize = 11008;
    return c;
}

inline Qwen3Config qwen3_4B() {
    Qwen3Config c;
    c.hiddenSize       = 2560;
    c.numLayers        = 36;
    c.numHeads         = 32;
    c.numKvHeads       = 8;
    c.headDim          = 128;
    c.intermediateSize = 9728;  // approximate
    return c;
}

inline Qwen3Config qwen3_8B() {
    Qwen3Config c;
    c.hiddenSize       = 4096;
    c.numLayers        = 36;
    c.numHeads         = 32;
    c.numKvHeads       = 8;
    c.headDim          = 128;
    c.intermediateSize = 22016;
    return c;
}

inline Qwen3Config qwen3_14B() {
    Qwen3Config c;
    c.hiddenSize       = 5120;
    c.numLayers        = 40;
    c.numHeads         = 40;
    c.numKvHeads       = 8;
    c.headDim          = 128;
    c.intermediateSize = 17920;
    return c;
}

inline Qwen3Config qwen3_32B() {
    Qwen3Config c;
    c.hiddenSize       = 5120;
    c.numLayers        = 64;
    c.numHeads         = 64;
    c.numKvHeads       = 8;
    c.headDim          = 128;
    c.intermediateSize = 25600;
    return c;
}

/// Qwen3-30B-A3B (MoE): 30.5 B total params, 3.3 B activated per token.
/// 128 routable experts, top-8 per token, 128 K context window.
inline Qwen3Config qwen3_30B_A3B() {
    Qwen3Config c;
    c.hiddenSize              = 2048;
    c.numLayers               = 48;
    c.numHeads                = 16;
    c.numKvHeads              = 8;
    c.headDim                 = 128;
    c.intermediateSize        = 1024; ///< Per-expert FFN dim (SwiGLU gate width)
    c.numExperts              = 128;  ///< Total routable experts
    c.numExpertsPerTok        = 8;    ///< Top-K experts activated per token
    c.maxPositionEmbeddings   = 131072; ///< 128 K context window
    c.ropeTheta               = 1000000.0f;
    return c;
}

} // namespace presets
} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_CONFIG_H
