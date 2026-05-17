/**
 * @file include/retdec/qwen3/qwen3_attention.h
 * @brief FlashAttention-2 tiled attention kernel for Qwen3 inference.
 *
 * ## Algorithm overview
 *
 *   Standard scaled dot-product attention requires O(N²) HBM reads/writes for
 *   the attention score matrix.  FlashAttention-2 (Dao et al., 2023) fuses the
 *   softmax and weighted-sum into a single tiled kernel, keeping intermediate
 *   scores in on-chip SRAM/local memory.  HBM traffic falls to O(N).
 *
 *   Tile sizes:
 *     Br = work-group size in the Q direction  (query tile rows)
 *     Bc = KV tile columns
 *
 *   Per-tile computation:
 *     1. Load Q tile (Br × headDim) from global → registers
 *     2. For each KV tile j:
 *        a. Load K_j (Bc × headDim) into local memory
 *        b. Compute S_j = Q · K_j^T * scale   (Br × Bc)
 *        c. Apply causal mask (skip tiles where all cols > row)
 *        d. Online softmax update:
 *             m_new  = max(m_prev, rowmax(S_j))
 *             l_new  = exp(m_prev - m_new)*l_prev + rowsum(exp(S_j - m_new))
 *             O_new  = O_prev * exp(m_prev - m_new) + P_j · V_j
 *        e. Load V_j (Bc × headDim) into local memory, accumulate O
 *     3. Normalise:  O /= l
 *
 * ## Grouped-Query Attention (GQA)
 *
 *   Qwen3 uses separate head counts for Q (numHeads) and KV (numKvHeads).
 *   Each KV head serves (numHeads / numKvHeads) query heads.
 *   kvHead = qHead / kvRepeat
 *
 * ## KV Cache
 *
 *   Dense cache:
 *     shape  [maxSeqLen × numKvHeads × headDim]
 *     layout row-major (position major)
 *
 *   Paged cache (PagedKvCache):
 *     Fixed-size blocks of PAGE_SIZE tokens.
 *     Block table maps logical page indices to physical page slots.
 *     Avoids contiguous allocation for long sequences.
 *
 * ## RoPE (Rotary Position Embedding)
 *
 *   Applied to Q and K before attention.  For position p, dimension i:
 *     theta_i = p / (base^(2i / headDim))
 *     [x_2i, x_2i+1] = [x_2i*cos - x_2i+1*sin, x_2i*sin + x_2i+1*cos]
 *
 * ## GPU dispatch
 *
 *   FlashAttention runs on CPU.  GEMV projections (Wq, Wk, Wv, Wo) are
 *   dispatched through ops::gemvKeyed() which routes to CUDA when available.
 *
 * ## Thread safety
 *
 *   Qwen3Attention is NOT thread-safe.  Create one instance per inference
 *   thread or protect with an external mutex.
 */

#ifndef RETDEC_QWEN3_ATTENTION_H
#define RETDEC_QWEN3_ATTENTION_H

#include "retdec/qwen3/qwen3_config.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec::qwen3 {

// ─── Tile size constants ──────────────────────────────────────────────────────

/// Q-tile rows (work-group size along the query dimension)
inline constexpr int FLASH_BR = 64;
/// KV-tile columns (tile size along key/value sequence dimension)
inline constexpr int FLASH_BC = 64;

// ─── KV Cache ─────────────────────────────────────────────────────────────────

/**
 * @brief Dense KV cache for a single transformer layer.
 *
 * Layout: [maxSeqLen × numKvHeads × headDim]  (row-major, position-major)
 *
 * Append new K/V at position `pos` via appendKv(), then attend over
 * positions [0..seqLen) via the attention kernel.
 */
struct KvCacheLayer {
    std::vector<float> k;       ///< [maxSeqLen × numKvHeads × headDim]
    std::vector<float> v;       ///< [maxSeqLen × numKvHeads × headDim]
    int                seqLen = 0;     ///< Tokens appended so far
    int                maxSeqLen = 0;
    int                numKvHeads = 0;
    int                headDim = 0;

    void allocate(int maxSeq, int nkv, int hd);
    void reset();

    /**
     * @brief Append one token's K/V projections into the cache.
     * @param kNew  [numKvHeads × headDim] K vectors for the new token
     * @param vNew  [numKvHeads × headDim] V vectors for the new token
     * @param pos   Position index to write (must be < maxSeqLen)
     */
    void appendKv(const float* kNew, const float* vNew, int pos);
};

// ─── Paged KV cache ──────────────────────────────────────────────────────────

inline constexpr int PAGE_SIZE = 16;  ///< Tokens per KV cache page

/**
 * @brief Variable-length KV cache using a paged block-table.
 *
 * Avoids large contiguous allocations.  Each page holds PAGE_SIZE tokens.
 * The block table maps logical pages to physical page slots.
 */
struct PagedKvCache {
    /// Physical storage: [numPages × PAGE_SIZE × numKvHeads × headDim]
    std::vector<float> kStorage;
    std::vector<float> vStorage;

    /// block_table[logicalPage] = physicalPage (-1 = not allocated)
    std::vector<int>   blockTable;

    int numPages    = 0;
    int numKvHeads  = 0;
    int headDim     = 0;
    int usedPages   = 0;
    int seqLen      = 0;

    /**
     * @brief Allocate physical storage for up to maxPages pages.
     */
    void allocate(int maxPages, int nkv, int hd);
    void reset();

    /**
     * @brief Append one token to the paged cache, allocating a new page if needed.
     */
    void appendKv(const float* kNew, const float* vNew);

    /**
     * @brief Read K[headIdx][pos] — returns pointer into storage (read-only).
     */
    const float* kAt(int head, int pos) const;
    const float* vAt(int head, int pos) const;

private:
    int allocPage();
};

// ─── Attention parameters ────────────────────────────────────────────────────

struct FlashAttnParams {
    int  numHeads   = 16;
    int  numKvHeads = 8;
    int  headDim    = 128;
    int  seqLen     = 0;      ///< Sequence length (tokens in KV cache)
    int  queryPos   = 0;      ///< Position index of the current query token
    float scale     = 0.0f;   ///< Attention scale (1/sqrt(headDim) if 0)
    bool causalMask = true;   ///< Apply lower-triangular causal mask

    int kvRepeat() const { return (numKvHeads > 0) ? numHeads / numKvHeads : 1; }
};

// ─── CPU reference implementation ────────────────────────────────────────────

/**
 * @brief Pure-C++ reference FlashAttention-2 for a single query token.
 *
 * Processes one token at a time (decode step).  Uses the tiled online-softmax
 * algorithm to avoid materialising the full score matrix.
 *
 * @param params     Attention configuration
 * @param q          Query projections  [numHeads × headDim]
 * @param kCache     Key cache          [seqLen × numKvHeads × headDim]
 * @param vCache     Value cache        [seqLen × numKvHeads × headDim]
 * @param out        Output             [numHeads × headDim]
 */
void flashAttnCpu(const FlashAttnParams& params,
                  const float* q,
                  const float* kCache,
                  const float* vCache,
                  float*       out);

/**
 * @brief Same as flashAttnCpu but reads from a PagedKvCache.
 */
void flashAttnCpuPaged(const FlashAttnParams& params,
                       const float*      q,
                       const PagedKvCache& kv,
                       float*            out);

// ─── FlashAttention kernel (CPU) ─────────────────────────────────────────────

/**
 * @brief FlashAttention-2 for Qwen3 — CPU implementation.
 *
 * GEMV projections (Wq/Wk/Wv/Wo) are dispatched via ops::gemvKeyed() which
 * routes to the CUDA backend when available.  The attention dot-product itself
 * runs on CPU using tiled FlashAttention-2 with online softmax.
 */
class Qwen3FlashAttn {
public:
    Qwen3FlashAttn();
    ~Qwen3FlashAttn();

    Qwen3FlashAttn(const Qwen3FlashAttn&)            = delete;
    Qwen3FlashAttn& operator=(const Qwen3FlashAttn&) = delete;

    /** @brief Initialise (no-op — kept for API compatibility). */
    bool init(const Qwen3Config& cfg);

    /** @brief Tear down resources. */
    void shutdown();

    /**
     * @brief Run FlashAttention for one decode step.
     *
     * Dense KV cache variant.
     *
     * @param params    Attention parameters (seqLen, queryPos, etc.)
     * @param q         Query  [numHeads  × headDim]  (after RoPE)
     * @param kCache    Key    [seqLen × numKvHeads × headDim]
     * @param vCache    Value  [seqLen × numKvHeads × headDim]
     * @param out       Output [numHeads × headDim]
     */
    void forward(const FlashAttnParams& params,
                 const float* q,
                 const float* kCache,
                 const float* vCache,
                 float*       out);

    /**
     * @brief Paged KV cache variant.
     */
    void forwardPaged(const FlashAttnParams& params,
                      const float*      q,
                      const PagedKvCache& kv,
                      float*            out);

    bool isGpuReady() const { return gpuReady_; }
    const std::string& lastError() const { return lastError_; }

private:
    bool        gpuReady_   = false;
    mutable std::string lastError_;
};

// ─── RoPE helpers ─────────────────────────────────────────────────────────────

/**
 * @brief Apply RoPE in-place to a single head vector.
 *
 * @param x       Head vector [headDim], modified in place
 * @param pos     Token position
 * @param headDim Per-head dimension
 * @param theta   RoPE base frequency (Qwen3 default: 1,000,000)
 */
void ropeApplyHead(float* x, int pos, int headDim, float theta = 1000000.0f);

/**
 * @brief Apply RoPE to all Q and K head vectors.
 *
 * @param q       [numHeads  × headDim]
 * @param k       [numKvHeads × headDim]
 * @param pos     Current token position
 * @param params  Attention config (numHeads, numKvHeads, headDim, ropeTheta)
 */
void ropeApplyAll(float* q, float* k,
                  int pos, const FlashAttnParams& params,
                  float theta = 1000000.0f);

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_ATTENTION_H
