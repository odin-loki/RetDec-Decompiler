/**
 * @file include/retdec/qwen3/qwen3_ops.h
 * @brief Low-level tensor operations for Qwen3 CPU inference.
 *
 * All activations are float32.  Weights stay in their native GGUF
 * quantization format and are dequantized on-the-fly during GEMV.
 *
 * Supported weight dtypes for GEMV:
 *   F32 · F16 · BF16 · Q8_0 · Q4_0 · Q4_1 · Q4_K_M · Q6_K
 *
 * Naming conventions (sizes are in floats unless noted):
 *   n  — sequence length (number of tokens processed so far)
 *   d  — hidden dimension (hiddenSize)
 *   dh — per-head dimension (headDim)
 *   nh — number of query heads
 *   nk — number of key-value heads
 *   di — intermediate (FFN) dimension
 */

#ifndef RETDEC_QWEN3_OPS_H
#define RETDEC_QWEN3_OPS_H

#include "retdec/qwen3/qwen3_weights.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace retdec::qwen3 { class Qwen3CUDA; }

namespace retdec::qwen3::ops {

// ─── CUDA GPU backend ─────────────────────────────────────────────────────────

/** Attach a CUDA device manager.  Pass nullptr to revert to CPU-only. */
void setCUDA(Qwen3CUDA* cuda);
Qwen3CUDA* getCUDA();

/**
 * @brief Hybrid GEMV with OpenCL key (for weight buffer caching).
 *
 * Same as gemv() but accepts an explicit GPU buffer cache key.
 */
void gemvKeyed(const uint8_t* wData, const void* wKey,
               GgufDtype dtype, int rows, int cols,
               const float* x, float* y);

// ─── Matrix-vector multiply ───────────────────────────────────────────────────

/**
 * @brief y = W * x   where W is [rows × cols] in native quantization format.
 *
 * Dispatches to the correct dequantizing kernel based on dtype.
 * y must be pre-allocated to [rows] floats.
 * x must be [cols] floats.
 *
 * @param wData  Raw weight bytes (GGUF quantized format)
 * @param wDtype GgufDtype of the weight matrix
 * @param rows   Number of output neurons
 * @param cols   Number of input features (= hidden size)
 * @param x      Input vector [cols]
 * @param y      Output vector [rows]
 */
void gemv(const uint8_t* wData, GgufDtype wDtype,
          int rows, int cols,
          const float* x, float* y);

/// Convenience overload taking a std::vector<uint8_t> weight buffer.
void gemv(const std::vector<uint8_t>& wData, GgufDtype wDtype,
          int rows, int cols,
          const float* x, float* y);

// Specialized kernels (public for benchmarking / testing)
void gemvF32 (const float*   W, int rows, int cols, const float* x, float* y);
void gemvF16 (const uint8_t* W, int rows, int cols, const float* x, float* y);
void gemvBF16(const uint8_t* W, int rows, int cols, const float* x, float* y);
void gemvQ8_0(const uint8_t* W, int rows, int cols, const float* x, float* y);
void gemvQ4_0(const uint8_t* W, int rows, int cols, const float* x, float* y);
void gemvQ4_1(const uint8_t* W, int rows, int cols, const float* x, float* y);
void gemvQ4_K(const uint8_t* W, int rows, int cols, const float* x, float* y);
void gemvQ6_K(const uint8_t* W, int rows, int cols, const float* x, float* y);

// ─── Normalization ────────────────────────────────────────────────────────────

/**
 * @brief In-place RMSNorm: x = x / rms(x) * weight.
 * @param x      In/out vector [dim]
 * @param weight Scale vector [dim] (F32)
 * @param dim    Vector length
 * @param eps    Epsilon for numerical stability (default 1e-6)
 */
void rmsnorm(float* x, const float* weight, int dim, float eps = 1e-6f);

// ─── Activation ───────────────────────────────────────────────────────────────

/**
 * @brief In-place SiLU × Hadamard product: gate[i] = silu(gate[i]) * up[i].
 *
 * SwiGLU computes:  FFN(x) = W_down( SiLU(W_gate * x) ⊙ W_up * x )
 * This function applies the element-wise SiLU to `gate` and multiplies by `up`
 * in place.  The caller stores the result back in `gate`.
 *
 * @param gate  Gate vector [dim], modified in place
 * @param up    Up vector [dim], read-only
 * @param dim   Vector length
 */
void siluHadamard(float* gate, const float* up, int dim);

// ─── Attention ────────────────────────────────────────────────────────────────

/**
 * @brief Rotary Position Embedding (RoPE) applied in-place to Q and K.
 *
 * Applies the standard RoPE rotation:
 *   q[2i], q[2i+1] = q[2i]*cos - q[2i+1]*sin, q[2i]*sin + q[2i+1]*cos
 *
 * @param q      Query vectors [nh × headDim], modified in place
 * @param k      Key vectors   [nk × headDim], modified in place
 * @param pos    Current token position (0-based)
 * @param headDim Per-head dimension
 * @param nh     Number of query heads
 * @param nk     Number of key-value heads
 * @param theta  RoPE base frequency (default 1e6 for Qwen3)
 */
void rope(float* q, float* k,
          int pos, int headDim, int nh, int nk,
          float theta = 1000000.0f);

/**
 * @brief Grouped-Query Attention (GQA) forward pass.
 *
 * Computes:
 *   scores[h][j] = Q[h] · K_cache[h/kvRepeat][j] / sqrt(headDim)   j∈[0,seqLen]
 *   scores[h]    = softmax(scores[h])
 *   out[h]       = sum_j scores[h][j] * V_cache[h/kvRepeat][j]
 *
 * @param q        Query [nh × headDim]
 * @param kCache   Key cache   [maxSeq × nk × headDim]
 * @param vCache   Value cache [maxSeq × nk × headDim]
 * @param seqLen   Number of tokens in the KV cache (including current)
 * @param nh       Number of query heads
 * @param nk       Number of KV heads
 * @param headDim  Per-head dimension
 * @param out      Output [nh × headDim]
 * @param scratch  Temporary [maxSeq] floats for softmax scores
 */
void gqaAttention(const float* q,
                   const float* kCache, const float* vCache,
                   int seqLen, int nh, int nk, int headDim,
                   float* out, float* scratch);

// ─── Softmax / argmax ─────────────────────────────────────────────────────────

/// In-place softmax over [n] elements.
void softmax(float* x, int n);

/// Returns the index of the maximum element in [x, x+n).
int argmax(const float* x, int n);

// ─── Vector helpers ───────────────────────────────────────────────────────────

/// y += x  (in-place add)
void addVec(float* y, const float* x, int n);

/// y = x (copy)
void copyVec(float* dst, const float* src, int n);

} // namespace retdec::qwen3::ops

#endif // RETDEC_QWEN3_OPS_H
