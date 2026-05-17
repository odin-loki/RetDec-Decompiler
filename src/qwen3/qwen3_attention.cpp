/**
 * @file src/qwen3/qwen3_attention.cpp
 * @brief FlashAttention-2 tiled attention kernel — CPU reference + OpenCL GPU.
 *
 * CPU reference
 * ─────────────
 * Implements Algorithm 2 from "FlashAttention-2: Faster Attention with Better
 * Parallelism and Work Partitioning" (Dao, 2023) in plain C++.
 * Processes the decode step (one query token, seqLen KV tokens).
 *
 * Per-query-head, we tile over the KV dimension in chunks of FLASH_BC:
 *   - Maintain running max (m), running sum (l), and accumulated output (O)
 *   - For each KV tile: compute scores, update online softmax, accumulate O
 *   - After all tiles: normalise O /= l
 *
 * OpenCL GPU kernel
 * ─────────────────
 * The same algorithm is expressed as an OpenCL C kernel string embedded here.
 * Each work-group handles ONE query head. Work-items within the group
 * collaboratively load KV tiles into local memory and compute scores.
 *
 * Key design decisions:
 *   - Local memory: K tile [Bc × headDim] + V tile [Bc × headDim]
 *   - Work-items: one per output dimension (headDim work-items per head)
 *   - Score reduction: simple parallel reduction within work-group
 *   - Causal mask: if all KV positions in a tile > queryPos, skip the tile
 *
 * GQA:  kvHead = qHead / kvRepeat   is computed in the kernel.
 */

#include "retdec/qwen3/qwen3_attention.h"
#include "retdec/qwen3/qwen3_config.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>


namespace retdec::qwen3 {

// ─── KvCacheLayer ─────────────────────────────────────────────────────────────

void KvCacheLayer::allocate(int maxSeq, int nkv, int hd) {
    maxSeqLen  = maxSeq;
    numKvHeads = nkv;
    headDim    = hd;
    k.assign(static_cast<std::size_t>(maxSeq) * nkv * hd, 0.0f);
    v.assign(static_cast<std::size_t>(maxSeq) * nkv * hd, 0.0f);
    seqLen = 0;
}

void KvCacheLayer::reset() {
    seqLen = 0;
}

void KvCacheLayer::appendKv(const float* kNew, const float* vNew, int pos) {
    if (pos < 0 || pos >= maxSeqLen) {
        throw std::runtime_error(
            "KvCacheLayer::appendKv: position " + std::to_string(pos) +
            " out of bounds [0, " + std::to_string(maxSeqLen) + ")");
    }
    int stride = numKvHeads * headDim;
    std::memcpy(k.data() + static_cast<std::ptrdiff_t>(pos) * stride,
                kNew, static_cast<std::size_t>(stride) * sizeof(float));
    std::memcpy(v.data() + static_cast<std::ptrdiff_t>(pos) * stride,
                vNew, static_cast<std::size_t>(stride) * sizeof(float));
    if (pos + 1 > seqLen) seqLen = pos + 1;
}

// ─── PagedKvCache ─────────────────────────────────────────────────────────────

void PagedKvCache::allocate(int maxPages, int nkv, int hd) {
    numPages   = maxPages;
    numKvHeads = nkv;
    headDim    = hd;
    std::size_t pageSlots = static_cast<std::size_t>(maxPages) * PAGE_SIZE * nkv * hd;
    kStorage.assign(pageSlots, 0.0f);
    vStorage.assign(pageSlots, 0.0f);
    blockTable.assign(static_cast<std::size_t>((maxPages + 1) & ~1), -1);
    usedPages = 0;
    seqLen    = 0;
}

void PagedKvCache::reset() {
    usedPages = 0;
    seqLen    = 0;
    std::fill(blockTable.begin(), blockTable.end(), -1);
}

int PagedKvCache::allocPage() {
    if (usedPages >= numPages)
        throw std::runtime_error("PagedKvCache: out of pages");
    return usedPages++;
}

void PagedKvCache::appendKv(const float* kNew, const float* vNew) {
    int logPage  = seqLen / PAGE_SIZE;
    int pageOff  = seqLen % PAGE_SIZE;

    // Ensure the logical page is mapped to a physical page
    if (logPage >= static_cast<int>(blockTable.size())) {
        blockTable.resize(static_cast<std::size_t>(logPage + 1), -1);
    }
    if (blockTable[logPage] < 0) {
        blockTable[logPage] = allocPage();
    }
    int physPage = blockTable[logPage];

    // Write K and V
    std::size_t tokenOffset = (static_cast<std::size_t>(physPage) * PAGE_SIZE + pageOff)
                              * numKvHeads * headDim;
    std::memcpy(kStorage.data() + tokenOffset, kNew,
                static_cast<std::size_t>(numKvHeads * headDim) * sizeof(float));
    std::memcpy(vStorage.data() + tokenOffset, vNew,
                static_cast<std::size_t>(numKvHeads * headDim) * sizeof(float));
    ++seqLen;
}

const float* PagedKvCache::kAt(int head, int pos) const {
    int logPage  = pos / PAGE_SIZE;
    int pageOff  = pos % PAGE_SIZE;
    int physPage = blockTable[logPage];
    std::size_t off = (static_cast<std::size_t>(physPage) * PAGE_SIZE + pageOff)
                      * numKvHeads * headDim + head * headDim;
    return kStorage.data() + off;
}

const float* PagedKvCache::vAt(int head, int pos) const {
    int logPage  = pos / PAGE_SIZE;
    int pageOff  = pos % PAGE_SIZE;
    int physPage = blockTable[logPage];
    std::size_t off = (static_cast<std::size_t>(physPage) * PAGE_SIZE + pageOff)
                      * numKvHeads * headDim + head * headDim;
    return vStorage.data() + off;
}

// ─── RoPE helpers ─────────────────────────────────────────────────────────────

void ropeApplyHead(float* x, int pos, int headDim, float theta) {
    int half = headDim / 2;
    for (int i = 0; i < half; ++i) {
        float freq  = 1.0f / std::pow(theta, (2.0f * i) / headDim);
        float angle = static_cast<float>(pos) * freq;
        float c = std::cos(angle);
        float s = std::sin(angle);
        float x0 = x[i];
        float x1 = x[i + half];
        x[i]        = x0 * c - x1 * s;
        x[i + half] = x0 * s + x1 * c;
    }
}

void ropeApplyAll(float* q, float* k,
                  int pos, const FlashAttnParams& params,
                  float theta) {
    for (int h = 0; h < params.numHeads; ++h)
        ropeApplyHead(q + h * params.headDim, pos, params.headDim, theta);
    for (int h = 0; h < params.numKvHeads; ++h)
        ropeApplyHead(k + h * params.headDim, pos, params.headDim, theta);
}

// ─── CPU reference: FlashAttention-2 (dense KV cache) ────────────────────────

void flashAttnCpu(const FlashAttnParams& params,
                  const float* q,
                  const float* kCache,
                  const float* vCache,
                  float*       out) {
    const int nh  = params.numHeads;
    const int nkv = params.numKvHeads;
    const int dh  = params.headDim;
    const int N   = params.seqLen;
    const int kvR = params.kvRepeat();
    const float scale = (params.scale > 0.0f) ? params.scale
                                               : 1.0f / std::sqrt(static_cast<float>(dh));

    // Per-query-head processing
    for (int h = 0; h < nh; ++h) {
        const int kvh      = h / kvR;
        const float* qHead = q + h * dh;
        float*       oHead = out + h * dh;

        float m = -1e38f;   // running max
        float l = 0.0f;     // running normaliser
        // Accumulator for output (initialised to zero)
        std::vector<float> acc(dh, 0.0f);

        // Tile over KV dimension
        for (int tileStart = 0; tileStart < N; tileStart += FLASH_BC) {
            const int tileEnd = std::min(tileStart + FLASH_BC, N);

            // Causal mask optimisation: if entire tile is beyond queryPos, skip
            if (params.causalMask && tileStart > params.queryPos) break;

            // ── Compute scores for this tile ──────────────────────────────────
            std::vector<float> scores(tileEnd - tileStart);
            for (int j = tileStart; j < tileEnd; ++j) {
                // Causal: only attend to positions <= queryPos
                if (params.causalMask && j > params.queryPos) {
                    scores[j - tileStart] = -1e38f;
                    continue;
                }
                // K[j, kvh, :] offset in dense cache
                const float* kj = kCache + (static_cast<std::ptrdiff_t>(j) * nkv + kvh) * dh;
                float dot = 0.0f;
                for (int d = 0; d < dh; ++d) dot += qHead[d] * kj[d];
                scores[j - tileStart] = dot * scale;
            }

            // ── Online softmax update ─────────────────────────────────────────
            // m_new = max(m, max(scores))
            float m_tile = *std::max_element(scores.begin(), scores.end());
            float m_new  = std::max(m, m_tile);

            // exp(scores - m_new)
            float l_tile = 0.0f;
            std::vector<float> expS(scores.size());
            for (int j = 0; j < static_cast<int>(scores.size()); ++j) {
                expS[j]  = std::exp(scores[j] - m_new);
                l_tile  += expS[j];
            }

            float decay = std::exp(m - m_new);  // rescale old accumulator
            float l_new = decay * l + l_tile;

            // O = O * decay + V_tile^T * expS
            for (int d = 0; d < dh; ++d) acc[d] *= decay;

            for (int j = tileStart; j < tileEnd; ++j) {
                float w = expS[j - tileStart];
                if (w == 0.0f) continue;
                const float* vj = vCache + (static_cast<std::ptrdiff_t>(j) * nkv + kvh) * dh;
                for (int d = 0; d < dh; ++d) acc[d] += w * vj[d];
            }

            m = m_new;
            l = l_new;
        }

        // Normalise
        float invL = (l > 0.0f) ? 1.0f / l : 0.0f;
        for (int d = 0; d < dh; ++d) oHead[d] = acc[d] * invL;
    }
}

// ─── CPU reference: FlashAttention-2 (paged KV cache) ────────────────────────

void flashAttnCpuPaged(const FlashAttnParams& params,
                       const float*       q,
                       const PagedKvCache& kv,
                       float*             out) {
    const int nh  = params.numHeads;
    const int dh  = params.headDim;
    const int N   = kv.seqLen;
    const int kvR = params.kvRepeat();
    const float scale = (params.scale > 0.0f) ? params.scale
                                               : 1.0f / std::sqrt(static_cast<float>(dh));

    for (int h = 0; h < nh; ++h) {
        const int kvh      = h / kvR;
        const float* qHead = q + h * dh;
        float*       oHead = out + h * dh;

        float m = -1e38f;
        float l = 0.0f;
        std::vector<float> acc(dh, 0.0f);

        for (int tileStart = 0; tileStart < N; tileStart += FLASH_BC) {
            if (params.causalMask && tileStart > params.queryPos) break;
            const int tileEnd = std::min(tileStart + FLASH_BC, N);

            std::vector<float> scores(tileEnd - tileStart);
            for (int j = tileStart; j < tileEnd; ++j) {
                if (params.causalMask && j > params.queryPos) {
                    scores[j - tileStart] = -1e38f;
                    continue;
                }
                const float* kj = kv.kAt(kvh, j);
                float dot = 0.0f;
                for (int d = 0; d < dh; ++d) dot += qHead[d] * kj[d];
                scores[j - tileStart] = dot * scale;
            }

            float m_tile = *std::max_element(scores.begin(), scores.end());
            float m_new  = std::max(m, m_tile);

            float l_tile = 0.0f;
            std::vector<float> expS(scores.size());
            for (int j = 0; j < static_cast<int>(scores.size()); ++j) {
                expS[j]  = std::exp(scores[j] - m_new);
                l_tile  += expS[j];
            }

            float decay = std::exp(m - m_new);
            float l_new = decay * l + l_tile;

            for (int d = 0; d < dh; ++d) acc[d] *= decay;

            for (int j = tileStart; j < tileEnd; ++j) {
                float w = expS[j - tileStart];
                if (w == 0.0f) continue;
                const float* vj = kv.vAt(kvh, j);
                for (int d = 0; d < dh; ++d) acc[d] += w * vj[d];
            }

            m = m_new;
            l = l_new;
        }

        float invL = (l > 0.0f) ? 1.0f / l : 0.0f;
        for (int d = 0; d < dh; ++d) oHead[d] = acc[d] * invL;
    }
}

// (OpenCL kernel source removed — CUDA GEMV path used instead)

#if 0 // ── Legacy OpenCL kernel source (kept for reference only) ──────────────
static const char* kFlashAttnKernelSrc = R"CL(
// ── FlashAttention-2 decode kernel ──────────────────────────────────────────
// Compile-time constants expected (passed as -D flags):
//   FLASH_BC    — KV tile size (e.g. 64)
//   HEAD_DIM    — per-head dimension (e.g. 128)
//
// Arguments:
//   0  q        [numHeads * headDim]       float*  (read-only)
//   1  kCache   [seqLen * numKvHeads * headDim]  float*
//   2  vCache   [seqLen * numKvHeads * headDim]  float*
//   3  out      [numHeads * headDim]       float*  (write)
//   4  seqLen   int
//   5  kvRepeat int   (= numHeads / numKvHeads)
//   6  queryPos int   (current token position for causal mask)
//   7  scale    float
//   8  numKvHeads int
//
// Local memory: 2 * FLASH_BC * HEAD_DIM + FLASH_BC + HEAD_DIM floats

__kernel __attribute__((reqd_work_group_size(HEAD_DIM, 1, 1)))
void flash_attn_decode(
    __global const float* q,
    __global const float* kCache,
    __global const float* vCache,
    __global       float* out,
    int seqLen,
    int kvRepeat,
    int queryPos,
    float scale,
    int numKvHeads,
    __local float* lMem)
{
    const int qHead = get_group_id(0);   // which query head
    const int lid   = get_local_id(0);   // which output dimension [0, headDim)
    const int headDim = HEAD_DIM;
    const int kvHead  = qHead / kvRepeat;

    // ── Partition local memory ────────────────────────────────────────────────
    __local float* kTile   = lMem;                              // [Bc * headDim]
    __local float* vTile   = lMem + FLASH_BC * headDim;         // [Bc * headDim]
    __local float* scores  = lMem + 2 * FLASH_BC * headDim;     // [Bc]
    __local float* scratch = lMem + 2 * FLASH_BC * headDim + FLASH_BC; // [headDim]

    // ── Load query vector for this head ──────────────────────────────────────
    float qVal = q[qHead * headDim + lid];

    // ── Running online-softmax state ─────────────────────────────────────────
    float m   = -1e38f;   // running max
    float l   = 0.0f;     // running sum
    float acc = 0.0f;     // per-dimension output accumulator

    // ── Tile loop over KV sequence ───────────────────────────────────────────
    for (int tileStart = 0; tileStart < seqLen; tileStart += FLASH_BC) {
        // Causal early exit: entire tile is beyond queryPos
        if (tileStart > queryPos) break;

        int tileEnd = min(tileStart + FLASH_BC, seqLen);
        int tileLen = tileEnd - tileStart;

        // ── Load K tile into local memory ─────────────────────────────────────
        // Each work-item (lid) loads column `lid` for all FLASH_BC rows
        for (int bc = 0; bc < tileLen; ++bc) {
            int pos = tileStart + bc;
            int src = (pos * numKvHeads + kvHead) * headDim + lid;
            kTile[bc * headDim + lid] = kCache[src];
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        // ── Compute dot products → scores[bc] ─────────────────────────────────
        // Work-item lid contributes its dimension to the partial dot product.
        // We accumulate into local scratch[bc] then reduce.
        for (int bc = 0; bc < tileLen; ++bc) {
            scratch[lid] = qVal * kTile[bc * headDim + lid];
        }
        // Parallel reduction to compute full dot product per bc token
        // (simpler: each bc uses a separate reduction — but expensive for large Bc)
        // Instead: reduce across headDim for each bc sequentially.
        // Work-item 0 aggregates; others wait.
        for (int bc = 0; bc < tileLen; ++bc) {
            scratch[lid] = qVal * kTile[bc * headDim + lid];
            barrier(CLK_LOCAL_MEM_FENCE);
            // Tree reduction
            for (int stride = headDim >> 1; stride > 0; stride >>= 1) {
                if (lid < stride)
                    scratch[lid] += scratch[lid + stride];
                barrier(CLK_LOCAL_MEM_FENCE);
            }
            if (lid == 0) {
                int pos = tileStart + bc;
                float s = (pos <= queryPos) ? scratch[0] * scale : -1e38f;
                scores[bc] = s;
            }
            barrier(CLK_LOCAL_MEM_FENCE);
        }

        // ── Tile max → m_tile ─────────────────────────────────────────────────
        // Reduce scores[0..tileLen) to find maximum
        scratch[lid] = (lid < tileLen) ? scores[lid] : -1e38f;
        barrier(CLK_LOCAL_MEM_FENCE);
        for (int stride = headDim >> 1; stride > 0; stride >>= 1) {
            if (lid < stride && lid + stride < tileLen)
                scratch[lid] = max(scratch[lid], scratch[lid + stride]);
            else if (lid < stride)
                ; // keep scratch[lid]
            barrier(CLK_LOCAL_MEM_FENCE);
        }
        float m_tile = scratch[0];
        barrier(CLK_LOCAL_MEM_FENCE);

        float m_new = max(m, m_tile);

        // ── Load V tile into local memory ─────────────────────────────────────
        for (int bc = 0; bc < tileLen; ++bc) {
            int pos = tileStart + bc;
            int src = (pos * numKvHeads + kvHead) * headDim + lid;
            vTile[bc * headDim + lid] = vCache[src];
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        // ── Accumulate: O = O * exp(m - m_new) + sum_j(exp(s_j - m_new) * V_j[lid]) ──
        float decay  = exp(m - m_new);
        acc *= decay;

        float l_tile = 0.0f;
        for (int bc = 0; bc < tileLen; ++bc) {
            float w   = exp(scores[bc] - m_new);
            acc      += w * vTile[bc * headDim + lid];
            if (lid == 0) l_tile += w;   // sum only once per tile
        }

        // Broadcast l_tile to all work-items via scratch[0]
        if (lid == 0) scratch[0] = l_tile;
        barrier(CLK_LOCAL_MEM_FENCE);
        l_tile = scratch[0];

        l = decay * l + l_tile;
        m = m_new;
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    // ── Normalise and write output ────────────────────────────────────────────
    float invL = (l > 0.0f) ? (1.0f / l) : 0.0f;
    out[qHead * headDim + lid] = acc * invL;
}
)CL";
#endif // Legacy OpenCL kernel source

// ─── Qwen3FlashAttn ───────────────────────────────────────────────────────────

Qwen3FlashAttn::Qwen3FlashAttn()  = default;
Qwen3FlashAttn::~Qwen3FlashAttn() { shutdown(); }

bool Qwen3FlashAttn::init(const Qwen3Config&) { return true; }
void Qwen3FlashAttn::shutdown() { gpuReady_ = false; }

void Qwen3FlashAttn::forward(const FlashAttnParams& params,
                              const float* q,
                              const float* kCache,
                              const float* vCache,
                              float*       out) {
    flashAttnCpu(params, q, kCache, vCache, out);
}

void Qwen3FlashAttn::forwardPaged(const FlashAttnParams& params,
                                   const float*      q,
                                   const PagedKvCache& kv,
                                   float*            out) {
    flashAttnCpuPaged(params, q, kv, out);
}

#if 0 // ── removed OpenCL code below ──────────────────────────────────────────

// ── Helper: release a cl_mem if non-null ──────────────────────────────────────
static void safeFree(cl_mem& m) {
    if (m) { clReleaseMemObject(m); m = nullptr; }
}

bool Qwen3FlashAttn::buildKernel(const Qwen3Config& cfg) {
    // Build compiler options: tile/head-dim constants
    std::string opts = "-DFLASH_BC=" + std::to_string(FLASH_BC)
                     + " -DHEAD_DIM=" + std::to_string(cfg.headDim)
                     + " -cl-fast-relaxed-math";

    cl_int err;
    const char* src = kFlashAttnKernelSrc;
    std::size_t srcLen = std::strlen(src);

    program_ = clCreateProgramWithSource(ctx_, 1, &src, &srcLen, &err);
    if (err != CL_SUCCESS) {
        lastError_ = "clCreateProgramWithSource failed: " + std::to_string(err);
        return false;
    }

    // Retrieve the device from the context
    cl_device_id dev;
    clGetCommandQueueInfo(queue_, CL_QUEUE_DEVICE, sizeof(dev), &dev, nullptr);

    err = clBuildProgram(program_, 1, &dev, opts.c_str(), nullptr, nullptr);
    if (err != CL_SUCCESS) {
        std::size_t logLen = 0;
        clGetProgramBuildInfo(program_, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logLen);
        std::string log(logLen, '\0');
        clGetProgramBuildInfo(program_, dev, CL_PROGRAM_BUILD_LOG, logLen, log.data(), nullptr);
        lastError_ = "clBuildProgram failed:\n" + log;
        return false;
    }

    kernel_ = clCreateKernel(program_, "flash_attn_decode", &err);
    if (err != CL_SUCCESS) {
        lastError_ = "clCreateKernel failed: " + std::to_string(err);
        return false;
    }
    return true;
}

bool Qwen3FlashAttn::init(const Qwen3Config& cfg) {
    if (gpuReady_) return true;

    numHeads_   = static_cast<int>(cfg.numHeads);
    numKvHeads_ = static_cast<int>(cfg.numKvHeads);
    headDim_    = static_cast<int>(cfg.headDim);
    maxSeqLen_  = static_cast<int>(cfg.maxPositionEmbeddings);

    // Select the first GPU device across all platforms
    cl_uint numPlatforms = 0;
    clGetPlatformIDs(0, nullptr, &numPlatforms);
    if (numPlatforms == 0) {
        lastError_ = "No OpenCL platforms found";
        return false;
    }
    std::vector<cl_platform_id> platforms(numPlatforms);
    clGetPlatformIDs(numPlatforms, platforms.data(), nullptr);

    cl_device_id chosen = nullptr;
    for (auto& plat : platforms) {
        cl_uint nd = 0;
        if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 0, nullptr, &nd) != CL_SUCCESS) continue;
        if (nd == 0) continue;
        std::vector<cl_device_id> devs(nd);
        clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, nd, devs.data(), nullptr);
        chosen = devs[0];
        break;
    }
    if (!chosen) {
        lastError_ = "No GPU OpenCL device found — using CPU fallback";
        return false;
    }

    cl_int err;
    ctx_   = clCreateContext(nullptr, 1, &chosen, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) { lastError_ = "clCreateContext failed"; return false; }

    queue_ = clCreateCommandQueue(ctx_, chosen, 0, &err);
    if (err != CL_SUCCESS) { lastError_ = "clCreateCommandQueue failed"; return false; }

    if (!buildKernel(cfg)) return false;

    gpuReady_ = true;
    return true;
}

void Qwen3FlashAttn::shutdown() {
    safeFree(qBuf_);
    safeFree(kBuf_);
    safeFree(vBuf_);
    safeFree(outBuf_);
    if (kernel_)  { clReleaseKernel(kernel_);   kernel_  = nullptr; }
    if (program_) { clReleaseProgram(program_);  program_ = nullptr; }
    if (queue_)   { clReleaseCommandQueue(queue_); queue_ = nullptr; }
    if (ctx_)     { clReleaseContext(ctx_);       ctx_    = nullptr; }
    gpuReady_ = false;
}

// Ensure GPU scratch buffers are at least as large as needed
static cl_mem ensureBuf(cl_context ctx, cl_mem& buf, std::size_t& currentBytes,
                         std::size_t needed, cl_mem_flags flags) {
    if (currentBytes >= needed && buf) return buf;
    if (buf) { clReleaseMemObject(buf); buf = nullptr; }
    cl_int err;
    buf = clCreateBuffer(ctx, flags, needed, nullptr, &err);
    currentBytes = (err == CL_SUCCESS) ? needed : 0;
    return buf;
}

void Qwen3FlashAttn::forward(const FlashAttnParams& params,
                              const float* q,
                              const float* kCache,
                              const float* vCache,
                              float*       out) {
    if (!gpuReady_) {
        flashAttnCpu(params, q, kCache, vCache, out);
        return;
    }

    const int nh  = params.numHeads;
    const int nkv = params.numKvHeads;
    const int dh  = params.headDim;
    const int N   = params.seqLen;

    std::size_t qBytes   = static_cast<std::size_t>(nh  * dh) * sizeof(float);
    std::size_t kvBytes  = static_cast<std::size_t>(N   * nkv * dh) * sizeof(float);
    std::size_t outBytes = qBytes;

    cl_int err;
    auto qBuf  = ensureBuf(ctx_, qBuf_,  qBufBytes_,  qBytes,  CL_MEM_READ_ONLY);
    auto kBuf  = ensureBuf(ctx_, kBuf_,  kvBufBytes_, kvBytes, CL_MEM_READ_ONLY);
    auto vBuf  = ensureBuf(ctx_, vBuf_,  kvBufBytes_, kvBytes, CL_MEM_READ_ONLY);
    auto oBuf  = ensureBuf(ctx_, outBuf_,qBufBytes_,  outBytes,CL_MEM_WRITE_ONLY);

    if (!qBuf || !kBuf || !vBuf || !oBuf) {
        lastError_ = "Buffer allocation failed — falling back to CPU";
        flashAttnCpu(params, q, kCache, vCache, out);
        return;
    }

    // Upload
    clEnqueueWriteBuffer(queue_, qBuf, CL_FALSE, 0, qBytes,  q,      0, nullptr, nullptr);
    clEnqueueWriteBuffer(queue_, kBuf, CL_FALSE, 0, kvBytes, kCache, 0, nullptr, nullptr);
    clEnqueueWriteBuffer(queue_, vBuf, CL_FALSE, 0, kvBytes, vCache, 0, nullptr, nullptr);

    // Set kernel args
    int kvRepeat = params.kvRepeat();
    int qPos     = params.queryPos;
    float scale  = (params.scale > 0.0f) ? params.scale
                                         : 1.0f / std::sqrt(static_cast<float>(dh));
    int numKvH   = nkv;

    clSetKernelArg(kernel_, 0, sizeof(cl_mem), &qBuf);
    clSetKernelArg(kernel_, 1, sizeof(cl_mem), &kBuf);
    clSetKernelArg(kernel_, 2, sizeof(cl_mem), &vBuf);
    clSetKernelArg(kernel_, 3, sizeof(cl_mem), &oBuf);
    clSetKernelArg(kernel_, 4, sizeof(int),    &N);
    clSetKernelArg(kernel_, 5, sizeof(int),    &kvRepeat);
    clSetKernelArg(kernel_, 6, sizeof(int),    &qPos);
    clSetKernelArg(kernel_, 7, sizeof(float),  &scale);
    clSetKernelArg(kernel_, 8, sizeof(int),    &numKvH);

    // Local memory: kTile + vTile + scores + scratch
    std::size_t lmem = (2 * FLASH_BC * dh + FLASH_BC + dh) * sizeof(float);
    clSetKernelArg(kernel_, 9, lmem, nullptr);

    // Dispatch: one work-group per query head, headDim work-items per group
    std::size_t global[2] = { static_cast<std::size_t>(nh * dh),
                               static_cast<std::size_t>(1) };
    std::size_t local[2]  = { static_cast<std::size_t>(dh),
                               static_cast<std::size_t>(1) };
    err = clEnqueueNDRangeKernel(queue_, kernel_, 2, nullptr, global, local, 0, nullptr, nullptr);

    if (err != CL_SUCCESS) {
        lastError_ = "clEnqueueNDRangeKernel failed: " + std::to_string(err)
                   + " — falling back to CPU";
        flashAttnCpu(params, q, kCache, vCache, out);
        return;
    }

    // Readback
    clEnqueueReadBuffer(queue_, oBuf, CL_TRUE, 0, outBytes, out, 0, nullptr, nullptr);
}

void Qwen3FlashAttn::forwardPaged(const FlashAttnParams& params,
                                   const float*       q,
                                   const PagedKvCache& kv,
                                   float*             out) {
    // For paged cache: materialise a dense temporary and dispatch
    // (GPU paged dispatch can be added as future work)
    if (!gpuReady_) {
        flashAttnCpuPaged(params, q, kv, out);
        return;
    }

    const int nkv = params.numKvHeads;
    const int dh  = params.headDim;
    const int N   = kv.seqLen;

    std::vector<float> kDense(static_cast<std::size_t>(N * nkv * dh));
    std::vector<float> vDense(static_cast<std::size_t>(N * nkv * dh));
    for (int j = 0; j < N; ++j) {
        for (int h = 0; h < nkv; ++h) {
            std::memcpy(kDense.data() + (static_cast<std::ptrdiff_t>(j) * nkv + h) * dh,
                        kv.kAt(h, j), static_cast<std::size_t>(dh) * sizeof(float));
            std::memcpy(vDense.data() + (static_cast<std::ptrdiff_t>(j) * nkv + h) * dh,
                        kv.vAt(h, j), static_cast<std::size_t>(dh) * sizeof(float));
        }
    }
    FlashAttnParams p2 = params;
    p2.seqLen = N;
    forward(p2, q, kDense.data(), vDense.data(), out);
}

#endif // removed OpenCL code

} // namespace retdec::qwen3
