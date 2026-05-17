/**
 * @file src/qwen3/qwen3_ops.cpp
 * @brief Low-level tensor operations for Qwen3 CPU inference.
 *
 * All quantized GEMV kernels follow the GGUF block layout exactly.
 * Block sizes (elements per block / bytes per block):
 *   Q8_0  :  32 / 34   (f16 scale + 32 × i8)
 *   Q4_0  :  32 / 18   (f16 scale + 16 bytes of 4-bit pairs)
 *   Q4_1  :  32 / 20   (f16 scale + f16 min + 16 bytes of 4-bit pairs)
 *   Q4_K_M: 256 / 144  (f16 super-d + f16 super-min + 12-byte scales + 128 bytes qs)
 *   Q6_K  : 256 / 210  (128-byte ql + 64-byte qh + 16-byte scales(i8) + f16 d)
 */

#include "retdec/qwen3/qwen3_ops.h"
#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_weights.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace retdec::qwen3::ops {

// ─── CUDA GPU backend ─────────────────────────────────────────────────────────

static Qwen3CUDA* gCuda = nullptr;

void setCUDA(Qwen3CUDA* cuda) { gCuda = cuda; }
Qwen3CUDA* getCUDA()           { return gCuda; }

void gemvKeyed(const uint8_t* wData, const void* wKey,
               GgufDtype dtype, int rows, int cols,
               const float* x, float* y) {
    if (gCuda && gCuda->isReady()) {
        gCuda->gemv(wData, wKey, dtype, rows, cols, x, y);
    } else {
        gemv(wData, dtype, rows, cols, x, y);
    }
}

// ─── FP conversion helpers ───────────────────────────────────────────────────

static inline float toF32_f16(uint16_t h) {
    // IEEE 754 half-precision → single-precision
    uint32_t sign = (h >> 15) & 1u;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign << 31; }
        else {
            // Denormal
            exp = 1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

static inline float toF32_bf16(uint16_t b) {
    uint32_t bits = static_cast<uint32_t>(b) << 16;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// ─── Q4_K scale/min extraction (matches ggml get_scale_min_k4) ───────────────

static inline void q4k_scale_min(int j, const uint8_t* sc,
                                  uint8_t& d, uint8_t& m) {
    if (j < 4) {
        d = sc[j]   & 0x3F;
        m = sc[j+4] & 0x3F;
    } else {
        d = (sc[j+4] & 0x0F) | ((sc[j-4] >> 6) << 4);
        m = (sc[j+4] >>  4) | ((sc[j-0] >> 6) << 4);
    }
}

// ─── GEMV kernels ─────────────────────────────────────────────────────────────

void gemvF32(const float* W, int rows, int cols,
             const float* x, float* y) {
    for (int r = 0; r < rows; ++r) {
        const float* row = W + r * cols;
        float s = 0.f;
        for (int c = 0; c < cols; ++c) s += row[c] * x[c];
        y[r] = s;
    }
}

void gemvF16(const uint8_t* W, int rows, int cols,
             const float* x, float* y) {
    for (int r = 0; r < rows; ++r) {
        const auto* row = reinterpret_cast<const uint16_t*>(W) + r * cols;
        float s = 0.f;
        for (int c = 0; c < cols; ++c) s += toF32_f16(row[c]) * x[c];
        y[r] = s;
    }
}

void gemvBF16(const uint8_t* W, int rows, int cols,
              const float* x, float* y) {
    for (int r = 0; r < rows; ++r) {
        const auto* row = reinterpret_cast<const uint16_t*>(W) + r * cols;
        float s = 0.f;
        for (int c = 0; c < cols; ++c) s += toF32_bf16(row[c]) * x[c];
        y[r] = s;
    }
}

// Q8_0: block of 32 elements → 2-byte f16 scale + 32 × i8
// Bytes per block: 34
void gemvQ8_0(const uint8_t* W, int rows, int cols,
              const float* x, float* y) {
    const int bpb = 34;
    const int nb  = (cols + 31) / 32; // ceil(cols/32); GGUF pads each row to full blocks
    for (int r = 0; r < rows; ++r) {
        const uint8_t* row = W + r * (nb * bpb);
        float s = 0.f;
        for (int b = 0; b < nb; ++b) {
            const int col0 = b * 32;
            if (col0 >= cols) break;
            const uint8_t*  blk   = row + b * bpb;
            uint16_t        scBits;
            std::memcpy(&scBits, blk, 2);
            float           scale = toF32_f16(scBits);
            const int8_t*   qs    = reinterpret_cast<const int8_t*>(blk + 2);
            const int       n     = std::min(32, cols - col0);
            float           bs    = 0.f;
            for (int i = 0; i < n; ++i) bs += qs[i] * x[col0 + i];
            s += scale * bs;
        }
        y[r] = s;
    }
}

// Q4_0: block of 32 elements → 2-byte f16 scale + 16 bytes of nibbles
// Bytes per block: 18
void gemvQ4_0(const uint8_t* W, int rows, int cols,
              const float* x, float* y) {
    const int bpb = 18;
    const int nb  = (cols + 31) / 32;
    for (int r = 0; r < rows; ++r) {
        const uint8_t* row = W + r * (nb * bpb);
        float s = 0.f;
        for (int b = 0; b < nb; ++b) {
            const int col0 = b * 32;
            if (col0 >= cols) break;
            const uint8_t*  blk   = row + b * bpb;
            uint16_t        scBits;
            std::memcpy(&scBits, blk, 2);
            float           scale = toF32_f16(scBits);
            const uint8_t*  qs    = blk + 2;
            float           bs    = 0.f;
            for (int i = 0; i < 16; ++i) {
                int lo = (int)(qs[i] & 0x0F) - 8;
                int hi = (int)(qs[i] >> 4)   - 8;
                int gi = col0 + i;
                if (gi < cols) bs += lo * x[gi];
                gi = col0 + i + 16;
                if (gi < cols) bs += hi * x[gi];
            }
            s += scale * bs;
        }
        y[r] = s;
    }
}

// Q4_1: block of 32 elements → f16 scale + f16 min + 16 nibble bytes
// Bytes per block: 20
void gemvQ4_1(const uint8_t* W, int rows, int cols,
              const float* x, float* y) {
    const int bpb = 20;
    const int nb  = (cols + 31) / 32;
    for (int r = 0; r < rows; ++r) {
        const uint8_t* row = W + r * (nb * bpb);
        float s = 0.f;
        for (int b = 0; b < nb; ++b) {
            const int col0 = b * 32;
            if (col0 >= cols) break;
            const uint8_t* blk = row + b * bpb;
            uint16_t scBits, mnBits;
            std::memcpy(&scBits, blk,     2);
            std::memcpy(&mnBits, blk + 2, 2);
            float      scale = toF32_f16(scBits);
            float      vmin  = toF32_f16(mnBits);
            const uint8_t* qs = blk + 4;
            float sumX = 0.f, sumQ = 0.f;
            for (int i = 0; i < 16; ++i) {
                int lo = qs[i] & 0x0F;
                int hi = qs[i] >> 4;
                int gi = col0 + i;
                if (gi < cols) {
                    sumQ += lo * x[gi];
                    sumX += x[gi];
                }
                gi = col0 + i + 16;
                if (gi < cols) {
                    sumQ += hi * x[gi];
                    sumX += x[gi];
                }
            }
            s += scale * sumQ + vmin * sumX;
        }
        y[r] = s;
    }
}

// Q4_K_M: super-block of 256 elements
//   Layout: f16 d | f16 dmin | uint8[12] scales | uint8[128] qs
//   Bytes per super-block: 144
//
// 8 sub-blocks of 32 elements.  Each sub-block j has:
//   scale[j] and min[j] stored as 6-bit values in the 12-byte scale array.
//   y[i] = scale * qs_nibble[i] - min
void gemvQ4_K(const uint8_t* W, int rows, int cols,
              const float* x, float* y) {
    const int bpb = 144;
    const int nb  = (cols + 255) / 256; // ceil(cols/256); MoE dims (e.g. 896) need this
    for (int r = 0; r < rows; ++r) {
        const uint8_t* row = W + r * (nb * bpb);
        float s = 0.f;
        for (int b = 0; b < nb; ++b) {
            const int colBase = b * 256;
            if (colBase >= cols) break;
            const uint8_t* blk = row + b * bpb;
            uint16_t dBits, dminBits;
            std::memcpy(&dBits,    blk,     2);
            std::memcpy(&dminBits, blk + 2, 2);
            float superD   = toF32_f16(dBits);
            float superMin = toF32_f16(dminBits);
            const uint8_t* sc = blk + 4;
            const uint8_t* qs = blk + 4 + 12;

            int is = 0;
            int qi = 0;
            for (int j = 0; j < 256; j += 64, is += 2) {
                uint8_t scA, mnA, scB, mnB;
                q4k_scale_min(is,     sc, scA, mnA);
                q4k_scale_min(is + 1, sc, scB, mnB);
                float d1 = superD   * scA, m1 = superMin * mnA;
                float d2 = superD   * scB, m2 = superMin * mnB;
                for (int i = 0; i < 32; ++i) {
                    const int gi = colBase + j + i;
                    if (gi < cols)
                        s += ((qs[qi + i] & 0x0F) * d1 - m1) * x[gi];
                }
                for (int i = 0; i < 32; ++i) {
                    const int gi = colBase + j + 32 + i;
                    if (gi < cols)
                        s += ((qs[qi + i] >> 4)   * d2 - m2) * x[gi];
                }
                qi += 32;
            }
        }
        y[r] = s;
    }
}

// Q6_K: super-block of 256 elements
//   Layout: uint8[128] ql | uint8[64] qh | int8[16] scales | f16 d
//   Bytes per super-block: 210
//   value[i] = d * scales[i/16] * (low4(ql[i]) | (2bit from qh) << 4) - 32
void gemvQ6_K(const uint8_t* W, int rows, int cols,
              const float* x, float* y) {
    const int bpb = 210;
    const int nb  = (cols + 255) / 256;
    for (int r = 0; r < rows; ++r) {
        const uint8_t* row = W + r * (nb * bpb);
        float s = 0.f;
        for (int b = 0; b < nb; ++b) {
            const int colBase = b * 256;
            if (colBase >= cols) break;
            const uint8_t* blk = row + b * bpb;
            const uint8_t*  ql  = blk;
            const uint8_t*  qh  = blk + 128;
            const int8_t*   sc  = reinterpret_cast<const int8_t*>(blk + 192);
            uint16_t dBits;
            std::memcpy(&dBits, blk + 208, 2);
            float d = toF32_f16(dBits);
            for (int j = 0; j < 256; ++j) {
                const int gi = colBase + j;
                if (gi >= cols) continue;
                int low4  = (ql[j / 2] >> (4 * (j & 1))) & 0x0F;
                int hi2   = (qh[j / 4] >> (2 * (j & 3))) & 0x03;
                int val6  = low4 | (hi2 << 4);
                float fv  = d * sc[j / 16] * (val6 - 32);
                s += fv * x[gi];
            }
        }
        y[r] = s;
    }
}

// ─── Dispatch ─────────────────────────────────────────────────────────────────

void gemv(const uint8_t* wData, GgufDtype dtype,
          int rows, int cols,
          const float* x, float* y) {
    switch (dtype) {
    case GgufDtype::F32:
        gemvF32(reinterpret_cast<const float*>(wData), rows, cols, x, y);
        break;
    case GgufDtype::F16:
        gemvF16(wData, rows, cols, x, y);
        break;
    case GgufDtype::BF16:
        gemvBF16(wData, rows, cols, x, y);
        break;
    case GgufDtype::Q8_0:
        gemvQ8_0(wData, rows, cols, x, y);
        break;
    case GgufDtype::Q4_0:
        gemvQ4_0(wData, rows, cols, x, y);
        break;
    case GgufDtype::Q4_1:
        gemvQ4_1(wData, rows, cols, x, y);
        break;
    case GgufDtype::Q4_K_S:
    case GgufDtype::Q4_K_M:
        gemvQ4_K(wData, rows, cols, x, y);
        break;
    case GgufDtype::Q6_K:
        gemvQ6_K(wData, rows, cols, x, y);
        break;
    default:
        // Fallback: dequantize the whole tensor first
        {
            TensorInfo tinfo;
            tinfo.dtype     = dtype;
            tinfo.nDims     = 2;
            tinfo.dims[0]   = static_cast<uint64_t>(cols);
            tinfo.dims[1]   = static_cast<uint64_t>(rows);
            tinfo.nElements = static_cast<uint64_t>(rows) * cols;
            tinfo.offset    = 0;
            tinfo.dataSizeBytes(); // ensure nElements is set
            TensorView tv(tinfo, wData, tinfo.dataSizeBytes());
            auto f32 = tv.dataF32();
            if (f32.has_value())
                gemvF32(f32->data(), rows, cols, x, y);
        }
        break;
    }
}

void gemv(const std::vector<uint8_t>& wData, GgufDtype dtype,
          int rows, int cols,
          const float* x, float* y) {
    gemv(wData.data(), dtype, rows, cols, x, y);
}

// ─── RMSNorm ──────────────────────────────────────────────────────────────────

void rmsnorm(float* x, const float* weight, int dim, float eps) {
    float ss = 0.f;
    for (int i = 0; i < dim; ++i) ss += x[i] * x[i];
    float scale = 1.f / std::sqrt(ss / dim + eps);
    for (int i = 0; i < dim; ++i) x[i] = x[i] * scale * weight[i];
}

// ─── SiLU × Hadamard ─────────────────────────────────────────────────────────

void siluHadamard(float* gate, const float* up, int dim) {
    for (int i = 0; i < dim; ++i) {
        float g = gate[i];
        // SiLU: x * sigmoid(x)
        gate[i] = g / (1.f + std::exp(-g)) * up[i];
    }
}

// ─── RoPE ─────────────────────────────────────────────────────────────────────

void rope(float* q, float* k,
          int pos, int headDim, int nh, int nk,
          float theta) {
    auto applyRope = [&](float* heads, int nHeads) {
        for (int h = 0; h < nHeads; ++h) {
            float* head = heads + h * headDim;
            for (int i = 0; i < headDim / 2; ++i) {
                float freq = 1.f / std::pow(theta,
                                 static_cast<float>(2 * i) / headDim);
                float ang  = pos * freq;
                float c    = std::cos(ang);
                float s    = std::sin(ang);
                float v0   = head[2 * i];
                float v1   = head[2 * i + 1];
                head[2 * i]     = v0 * c - v1 * s;
                head[2 * i + 1] = v0 * s + v1 * c;
            }
        }
    };
    applyRope(q, nh);
    applyRope(k, nk);
}

// ─── GQA Attention ───────────────────────────────────────────────────────────

void gqaAttention(const float* q,
                   const float* kCache, const float* vCache,
                   int seqLen, int nh, int nk, int headDim,
                   float* out, float* scratch) {
    float scale = 1.f / std::sqrt(static_cast<float>(headDim));
    int   kvRep = nh / nk;  // query heads per KV head

    for (int h = 0; h < nh; ++h) {
        int  kvHead = h / kvRep;
        const float* qHead  = q   + h * headDim;

        // Compute attention scores for this query head
        for (int t = 0; t < seqLen; ++t) {
            const float* kHead = kCache + (t * nk + kvHead) * headDim;
            float dot = 0.f;
            for (int d = 0; d < headDim; ++d) dot += qHead[d] * kHead[d];
            scratch[t] = dot * scale;
        }

        // Softmax over seqLen positions
        float maxV = *std::max_element(scratch, scratch + seqLen);
        float sumE = 0.f;
        for (int t = 0; t < seqLen; ++t) {
            scratch[t] = std::exp(scratch[t] - maxV);
            sumE += scratch[t];
        }
        for (int t = 0; t < seqLen; ++t) scratch[t] /= sumE;

        // Weighted sum of values
        float* outHead = out + h * headDim;
        std::fill(outHead, outHead + headDim, 0.f);
        for (int t = 0; t < seqLen; ++t) {
            const float* vHead = vCache + (t * nk + kvHead) * headDim;
            float        w     = scratch[t];
            for (int d = 0; d < headDim; ++d) outHead[d] += w * vHead[d];
        }
    }
}

// ─── Softmax / argmax ─────────────────────────────────────────────────────────

void softmax(float* x, int n) {
    float maxV = *std::max_element(x, x + n);
    float sumE = 0.f;
    for (int i = 0; i < n; ++i) { x[i] = std::exp(x[i] - maxV); sumE += x[i]; }
    for (int i = 0; i < n; ++i) x[i] /= sumE;
}

int argmax(const float* x, int n) {
    return static_cast<int>(std::max_element(x, x + n) - x);
}

// ─── Vector helpers ───────────────────────────────────────────────────────────

void addVec(float* y, const float* x, int n) {
    for (int i = 0; i < n; ++i) y[i] += x[i];
}

void copyVec(float* dst, const float* src, int n) {
    std::memcpy(dst, src, n * sizeof(float));
}

} // namespace retdec::qwen3::ops
