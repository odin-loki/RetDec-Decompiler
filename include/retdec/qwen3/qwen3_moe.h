/**
 * @file include/retdec/qwen3/qwen3_moe.h
 * @brief Mixture-of-Experts (MoE) routing and dispatch for Qwen3.
 *
 * ## Architecture
 *
 *   Qwen3 MoE variants (Qwen3-30B-A3B, Qwen3-235B-A22B) replace the dense
 *   FFN in every transformer layer with a mixture of N independent SwiGLU FFN
 *   experts.  For each input token, a small gating network (a single linear
 *   layer + softmax) scores all experts and the top-K highest-scoring experts
 *   process the token.  The outputs are aggregated with a weighted sum.
 *
 *   Model shapes:
 *     Qwen3-30B-A3B   numExperts=128, numExpertsPerTok=8, moeIntermSize=768
 *     Qwen3-235B-A22B numExperts=128, numExpertsPerTok=8, moeIntermSize=1536
 *
 * ## Components
 *
 *   MoeConfig         — hyper-parameters
 *   ExpertWeights     — thin view over GGUF-quantized expert FFN weight tensors
 *   MoeRouter         — gating linear → top-K selection → weight normalisation
 *   MoeDispatcher     — dispatch hidden state to selected experts, weighted sum
 *   Qwen3MoeLayer     — full MoE FFN replacement (router + dispatcher + shared
 *                       expert if present)
 *
 * ## Expert FFN layout
 *
 *   Each expert is a SwiGLU FFN identical to the dense model:
 *     gate_proj: [moeIntermSize × hiddenSize]
 *     up_proj:   [moeIntermSize × hiddenSize]
 *     down_proj: [hiddenSize    × moeIntermSize]
 *   FFN(x) = down_proj( SiLU(gate_proj(x)) ⊙ up_proj(x) )
 *
 * ## Routing algorithm (inference)
 *
 *   1. logits = hidden @ gate_weight^T          [numExperts]
 *   2. routing_probs = softmax(logits)
 *   3. (top_ids, top_weights) = top-K(routing_probs, K)
 *   4. if normalizeWeights: top_weights /= sum(top_weights)
 *   5. output = sum_i( top_weights[i] * Expert_i(hidden) )
 *
 * ## Shared expert
 *
 *   Some Qwen3 MoE configs include one permanently-active "shared expert"
 *   that is always applied in addition to the routed experts.
 *   If sharedExpertIntermSize > 0, the shared expert output is added to the
 *   final result before returning.
 *
 * ## OpenCL acceleration
 *
 *   When a Qwen3CUDA instance is attached via ops::setCUDA(), the gating GEMV
 *   and all per-expert gate/up/down
 *   GEMVs are dispatched through the existing hybrid GPU+CPU kernel path.
 *   The K expert FFNs are serialised on the CPU; a future version may
 *   dispatch them as independent work-groups.
 */

#ifndef RETDEC_QWEN3_MOE_H
#define RETDEC_QWEN3_MOE_H

#include "retdec/qwen3/qwen3_config.h"
#include "retdec/qwen3/qwen3_weights.h"  // GgufDtype

#include <cstddef>
#include <cstdint>
#include <vector>

namespace retdec::qwen3 {

// ─── MoeConfig ────────────────────────────────────────────────────────────────

struct MoeConfig {
    int numExperts            = 0;  ///< Total number of expert FFNs
    int numExpertsPerTok      = 0;  ///< Top-K experts activated per token
    int hiddenSize            = 0;  ///< Input / output hidden dimension
    int moeIntermSize         = 0;  ///< Per-expert intermediate (gate/up) dim
    int sharedExpertIntermSize= 0;  ///< Shared always-on expert (0 = none)
    bool normalizeWeights     = true; ///< Re-normalise routing weights after top-K

    bool isMoE() const { return numExperts > 0 && numExpertsPerTok > 0; }

    /// Build from a Qwen3Config (fills MoE fields, returns false for dense).
    static MoeConfig fromQwen3Config(const Qwen3Config& cfg);
};

// ─── ExpertWeights ────────────────────────────────────────────────────────────

/**
 * @brief Lightweight view of one expert's quantized weight tensors.
 *
 * Does NOT own the memory — the caller must ensure the pointed-to GGUF
 * buffers outlive this struct.
 *
 * gate_proj, up_proj: [moeIntermSize × hiddenSize]
 * down_proj:          [hiddenSize    × moeIntermSize]
 */
struct ExpertWeights {
    const uint8_t* gateW = nullptr;
    const uint8_t* upW   = nullptr;
    const uint8_t* downW = nullptr;
    GgufDtype      dtype = GgufDtype::F32;
};

// ─── Routing result ───────────────────────────────────────────────────────────

struct MoeRouteResult {
    std::vector<int>   expertIds;    ///< [numExpertsPerTok] selected expert indices
    std::vector<float> weights;      ///< [numExpertsPerTok] routing weights (sum ≤ 1)
};

// ─── MoeRouter ────────────────────────────────────────────────────────────────

/**
 * @brief Gating network: linear projection + softmax + top-K selection.
 *
 * The gate weight matrix is a single [numExperts × hiddenSize] projection.
 * It is stored in the model's GGUF file, quantized in the same format as
 * all other weight matrices.
 */
class MoeRouter {
public:
    explicit MoeRouter(const MoeConfig& cfg);

    /**
     * @brief Route one token to its top-K experts.
     *
     * @param hidden     Input hidden vector  [hiddenSize]
     * @param gateW      Gate weight matrix   [numExperts × hiddenSize] (quantized)
     * @param gateKey    Cache key for GPU buffer (usually == gateW)
     * @param dtype      Quantization format of gateW
     * @return           Selected expert indices and normalised routing weights
     */
    MoeRouteResult route(const float*   hidden,
                         const uint8_t* gateW,
                         const void*    gateKey,
                         GgufDtype      dtype) const;

    /**
     * @brief Convenience overload that uses gateW pointer as key.
     */
    MoeRouteResult route(const float*   hidden,
                         const uint8_t* gateW,
                         GgufDtype      dtype) const;

    const MoeConfig& config() const { return cfg_; }

private:
    MoeConfig cfg_;
    mutable std::vector<float> logits_;  // scratch buffer [numExperts]
};

// ─── MoeDispatcher ────────────────────────────────────────────────────────────

/**
 * @brief Dispatch a token to selected experts and aggregate outputs.
 *
 * Evaluates each selected expert's SwiGLU FFN and accumulates the
 * weighted sum into `out`.
 */
class MoeDispatcher {
public:
    explicit MoeDispatcher(const MoeConfig& cfg);

    /**
     * @brief Compute the MoE FFN output for one token.
     *
     * @param hidden      Input hidden vector [hiddenSize]
     * @param route       Result from MoeRouter::route()
     * @param experts     All expert weight views (size >= numExperts)
     * @param out         Output vector [hiddenSize] — written (not accumulated)
     */
    void forward(const float*           hidden,
                 const MoeRouteResult&  route,
                 const std::vector<ExpertWeights>& experts,
                 float*                 out) const;

    /**
     * @brief Apply the shared expert (always active) and add to `out`.
     *
     * @param hidden       Input hidden vector [hiddenSize]
     * @param sharedExpert Shared expert weight view (sharedExpertIntermSize must > 0)
     * @param out          Output vector [hiddenSize] — result is *added* to out
     */
    void applySharedExpert(const float*          hidden,
                           const ExpertWeights&  sharedExpert,
                           float*                out) const;

    const MoeConfig& config() const { return cfg_; }

private:
    MoeConfig cfg_;
    mutable std::vector<float> gate_;  // scratch [moeIntermSize]
    mutable std::vector<float> up_;    // scratch [moeIntermSize]
    mutable std::vector<float> down_;  // scratch [hiddenSize]
};

// ─── Qwen3MoeLayer ────────────────────────────────────────────────────────────

/**
 * @brief Full MoE FFN replacement (router + dispatcher + optional shared expert).
 *
 * This is the top-level class used by the model forward pass.
 *
 * Usage:
 * @code
 *   Qwen3MoeLayer moe(MoeConfig::fromQwen3Config(cfg));
 *   moe.forward(hidden, gateW, gateKey, gateKey, experts, sharedExpert, out);
 * @endcode
 */
class Qwen3MoeLayer {
public:
    explicit Qwen3MoeLayer(const MoeConfig& cfg);

    /**
     * @brief Full forward pass: route + dispatch + shared expert.
     *
     * @param hidden        Input hidden vector [hiddenSize]
     * @param gateW         Gating projection weights [numExperts × hiddenSize]
     * @param gateKey       GPU buffer cache key for gating weights
     * @param gateDtype     Quantization format of gateW
     * @param experts       All expert weight views (size == numExperts)
     * @param sharedExpert  Shared expert (ignored if sharedExpertIntermSize == 0)
     * @param out           Output vector [hiddenSize]
     */
    void forward(const float*                       hidden,
                 const uint8_t*                     gateW,
                 const void*                        gateKey,
                 GgufDtype                          gateDtype,
                 const std::vector<ExpertWeights>&  experts,
                 const ExpertWeights&               sharedExpert,
                 float*                             out) const;

    /// Simpler overload (no explicit GPU key, no shared expert).
    void forward(const float*                       hidden,
                 const uint8_t*                     gateW,
                 GgufDtype                          gateDtype,
                 const std::vector<ExpertWeights>&  experts,
                 float*                             out) const;

    const MoeConfig&     config()     const { return router_.config(); }
    const MoeRouter&     router()     const { return router_; }
    const MoeDispatcher& dispatcher() const { return dispatcher_; }

    /// Return the routing statistics from the last forward() call (for load monitoring).
    const MoeRouteResult& lastRoute() const { return lastRoute_; }

private:
    MoeRouter           router_;
    MoeDispatcher       dispatcher_;
    mutable MoeRouteResult lastRoute_;
};

// ─── Load-balance diagnostics ─────────────────────────────────────────────────

/**
 * @brief Accumulate per-expert dispatch counts across many tokens.
 *
 * Useful for profiling and verifying that routing is approximately balanced
 * (each expert should receive ~(numExpertsPerTok / numExperts) of all tokens).
 */
class MoeLoadMonitor {
public:
    explicit MoeLoadMonitor(int numExperts);

    /// Record one routing decision.
    void record(const MoeRouteResult& route);

    /// Reset counters.
    void reset();

    /// Number of times each expert has been selected.
    const std::vector<int>& counts() const { return counts_; }

    /// Fraction of tokens routed to each expert [0, 1].
    std::vector<float> fractions() const;

    /// Total number of recorded routing events.
    int totalTokens() const { return totalTokens_; }

    /// Expert with the highest dispatch count (hot expert).
    int hotExpert() const;

    /// Expert with the lowest dispatch count (cold / underused expert).
    int coldExpert() const;

private:
    std::vector<int> counts_;
    int              totalTokens_ = 0;
};

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_MOE_H
