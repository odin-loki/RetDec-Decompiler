/**
 * @file src/qwen3/qwen3_cuda.cu
 * @brief CUDA acceleration — device init, weight upload, hybrid GEMV dispatch.
 *
 * Direct port of qwen3_opencl.cpp.  Same quantization math; CUDA syntax.
 * Each GEMV kernel launches one thread per output row.  Shared memory is used
 * for the x (input) vector to avoid repeated global reads across threads in
 * the same block.
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_ops.h"
#include "retdec/qwen3/qwen3_weights.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <future>
#include <string>
#include <vector>

// ── Stub when CUDA is not available ──────────────────────────────────────────
#ifndef RETDEC_HAS_CUDA

namespace retdec::qwen3 {
Qwen3CUDA::Qwen3CUDA()  = default;
Qwen3CUDA::~Qwen3CUDA() = default;
bool Qwen3CUDA::init(float, int)                              { return false; }
void Qwen3CUDA::setGpuFraction(float)                         {}
void Qwen3CUDA::shutdown()                                    {}
bool Qwen3CUDA::uploadWeight(const void*, const uint8_t*, std::size_t) { return false; }
void Qwen3CUDA::releaseWeight(const void*)                    {}
void Qwen3CUDA::releaseAllWeights()                           {}
bool Qwen3CUDA::gemv(const uint8_t* wData, const void*,
                     GgufDtype dtype, int rows, int cols,
                     const float* x, float* y) {
    ops::gemv(wData, dtype, rows, cols, x, y);
    return false;
}
std::vector<CudaDeviceInfo> Qwen3CUDA::enumDevices()          { return {}; }
} // namespace retdec::qwen3

#else // RETDEC_HAS_CUDA ────────────────────────────────────────────────────────

#include <cuda_runtime.h>
#include <cuda_fp16.h>

// ── Error helper ──────────────────────────────────────────────────────────────

#define CUDA_CHECK(expr)                                                     \
    do {                                                                     \
        cudaError_t _e = (expr);                                             \
        if (_e != cudaSuccess) {                                             \
            lastError_ = std::string(#expr) + " → " +                       \
                         cudaGetErrorString(_e);                             \
            return false;                                                    \
        }                                                                    \
    } while (0)

#define CUDA_CHECK_V(expr)                                                   \
    do {                                                                     \
        cudaError_t _e = (expr);                                             \
        if (_e != cudaSuccess) {                                             \
            lastError_ = std::string(#expr) + " → " +                       \
                         cudaGetErrorString(_e);                             \
            return;                                                          \
        }                                                                    \
    } while (0)

// ── FP decode device functions ────────────────────────────────────────────────

__device__ __forceinline__ float decode_f16(unsigned short h) {
    // Reinterpret as __half then convert — avoids manual bit twiddling
    __half hv;
    memcpy(&hv, &h, sizeof(hv));
    return __half2float(hv);
}

__device__ __forceinline__ float decode_bf16(unsigned short b) {
    unsigned int bits = static_cast<unsigned int>(b) << 16;
    return __int_as_float(static_cast<int>(bits));
}

__device__ __forceinline__ void q4k_scale_min(int j, const unsigned char* sc,
                                              unsigned char* d, unsigned char* m) {
    if (j < 4) {
        *d = sc[j]   & 0x3Fu;
        *m = sc[j+4] & 0x3Fu;
    } else {
        *d = (sc[j+4] & 0x0Fu) | ((sc[j-4] >> 6) << 4);
        *m = (sc[j+4] >> 4)    | ((sc[j-0] >> 6) << 4);
    }
}

// ── GEMV kernels ──────────────────────────────────────────────────────────────
//
// Each thread computes one output row.  The x vector is loaded into shared
// memory in tiles to reduce global memory traffic.
//
// Tile size: TILE elements of x are loaded per iteration.
// For large cols the loop iterates ceil(cols/TILE) times.

// TILE must equal BLOCK so every thread in the block can load exactly one x
// element per iteration.  If TILE > BLOCK, threads above threadIdx.x won't
// fill the upper xs[] slots, leaving them uninitialized.
static constexpr int TILE = 128;  // == BLOCK

// F32 ─────────────────────────────────────────────────────────────────────────
__global__ void gemv_f32(const float* __restrict__ W,
                          const float* __restrict__ x,
                          float* __restrict__ y,
                          int rows, int cols) {
    __shared__ float xs[TILE];
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    float s = 0.0f;
    for (int base = 0; base < cols; base += TILE) {
        int cnt = min(TILE, cols - base);
        if (threadIdx.x < cnt) xs[threadIdx.x] = x[base + threadIdx.x];
        __syncthreads();
        if (r < rows) {
            const float* row = W + r * cols + base;
            for (int c = 0; c < cnt; c++) s += row[c] * xs[c];
        }
        __syncthreads();
    }
    if (r < rows) y[r] = s;
}

// F16 ─────────────────────────────────────────────────────────────────────────
__global__ void gemv_f16(const unsigned short* __restrict__ W,
                          const float* __restrict__ x,
                          float* __restrict__ y,
                          int rows, int cols) {
    __shared__ float xs[TILE];
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    float s = 0.0f;
    for (int base = 0; base < cols; base += TILE) {
        int cnt = min(TILE, cols - base);
        if (threadIdx.x < cnt) xs[threadIdx.x] = x[base + threadIdx.x];
        __syncthreads();
        if (r < rows) {
            const unsigned short* row = W + r * cols + base;
            for (int c = 0; c < cnt; c++) s += decode_f16(row[c]) * xs[c];
        }
        __syncthreads();
    }
    if (r < rows) y[r] = s;
}

// BF16 ────────────────────────────────────────────────────────────────────────
__global__ void gemv_bf16(const unsigned short* __restrict__ W,
                           const float* __restrict__ x,
                           float* __restrict__ y,
                           int rows, int cols) {
    __shared__ float xs[TILE];
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    float s = 0.0f;
    for (int base = 0; base < cols; base += TILE) {
        int cnt = min(TILE, cols - base);
        if (threadIdx.x < cnt) xs[threadIdx.x] = x[base + threadIdx.x];
        __syncthreads();
        if (r < rows) {
            const unsigned short* row = W + r * cols + base;
            for (int c = 0; c < cnt; c++) s += decode_bf16(row[c]) * xs[c];
        }
        __syncthreads();
    }
    if (r < rows) y[r] = s;
}

// Q8_0: 32-elem block = [f16 scale | 32 × i8]  (34 bytes) ───────────────────
__global__ void gemv_q8_0(const unsigned char* __restrict__ W,
                           const float* __restrict__ x,
                           float* __restrict__ y,
                           int rows, int cols) {
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r >= rows) return;
    int nb  = (cols + 31) / 32;
    int bpb = 34;
    const unsigned char* row = W + r * nb * bpb;
    float s = 0.0f;
    for (int b = 0; b < nb; b++) {
        int col0 = b * 32;
        if (col0 >= cols) break;
        const unsigned char* blk = row + b * bpb;
        unsigned short sc_bits = (unsigned short)blk[0] | ((unsigned short)blk[1] << 8);
        float scale = decode_f16(sc_bits);
        const signed char* qs = (const signed char*)(blk + 2);
        int n = cols - col0;
        if (n > 32) n = 32;
        float bs = 0.0f;
        for (int i = 0; i < n; i++) bs += (float)qs[i] * x[col0 + i];
        s += scale * bs;
    }
    y[r] = s;
}

// Q4_0: 32-elem block = [f16 scale | 16 × nibble-pairs]  (18 bytes) ─────────
__global__ void gemv_q4_0(const unsigned char* __restrict__ W,
                           const float* __restrict__ x,
                           float* __restrict__ y,
                           int rows, int cols) {
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r >= rows) return;
    int nb  = (cols + 31) / 32;
    int bpb = 18;
    const unsigned char* row = W + r * nb * bpb;
    float s = 0.0f;
    for (int b = 0; b < nb; b++) {
        int col0 = b * 32;
        if (col0 >= cols) break;
        const unsigned char* blk = row + b * bpb;
        unsigned short sc_bits = (unsigned short)blk[0] | ((unsigned short)blk[1] << 8);
        float scale = decode_f16(sc_bits);
        const unsigned char* qs = blk + 2;
        float bs = 0.0f;
        for (int i = 0; i < 16; i++) {
            int lo = (int)(qs[i] & 0x0Fu) - 8;
            int hi = (int)(qs[i] >>   4u) - 8;
            int gi = col0 + i;
            if (gi < cols) bs += (float)lo * x[gi];
            gi = col0 + i + 16;
            if (gi < cols) bs += (float)hi * x[gi];
        }
        s += scale * bs;
    }
    y[r] = s;
}

// Q4_1: 32-elem block = [f16 scale | f16 min | 16 × nibble-pairs]  (20 bytes)
__global__ void gemv_q4_1(const unsigned char* __restrict__ W,
                           const float* __restrict__ x,
                           float* __restrict__ y,
                           int rows, int cols) {
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r >= rows) return;
    int nb  = (cols + 31) / 32;
    int bpb = 20;
    const unsigned char* row = W + r * nb * bpb;
    float s = 0.0f;
    for (int b = 0; b < nb; b++) {
        int col0 = b * 32;
        if (col0 >= cols) break;
        const unsigned char* blk = row + b * bpb;
        unsigned short sc_bits = (unsigned short)blk[0] | ((unsigned short)blk[1] << 8);
        unsigned short mn_bits = (unsigned short)blk[2] | ((unsigned short)blk[3] << 8);
        float scale = decode_f16(sc_bits);
        float vmin  = decode_f16(mn_bits);
        const unsigned char* qs = blk + 4;
        float sumQ = 0.0f, sumX = 0.0f;
        for (int i = 0; i < 16; i++) {
            int lo = qs[i] & 0x0Fu;
            int hi = qs[i] >>   4u;
            int gi = col0 + i;
            if (gi < cols) {
                sumQ += (float)lo * x[gi];
                sumX += x[gi];
            }
            gi = col0 + i + 16;
            if (gi < cols) {
                sumQ += (float)hi * x[gi];
                sumX += x[gi];
            }
        }
        s += scale * sumQ + vmin * sumX;
    }
    y[r] = s;
}

// Q4_K_M: 256-elem super-block = [f16 d | f16 dmin | 12-byte scales | 128-byte qs]
__global__ void gemv_q4k(const unsigned char* __restrict__ W,
                          const float* __restrict__ x,
                          float* __restrict__ y,
                          int rows, int cols) {
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r >= rows) return;
    int nb  = (cols + 255) / 256;
    int bpb = 144;
    const unsigned char* row = W + r * nb * bpb;
    float s = 0.0f;
    for (int b = 0; b < nb; b++) {
        int colBase = b * 256;
        if (colBase >= cols) break;
        const unsigned char* blk = row + b * bpb;
        unsigned short d_bits    = (unsigned short)blk[0] | ((unsigned short)blk[1] << 8);
        unsigned short dmin_bits = (unsigned short)blk[2] | ((unsigned short)blk[3] << 8);
        float super_d   = decode_f16(d_bits);
        float super_min = decode_f16(dmin_bits);
        const unsigned char* sc = blk + 4;
        const unsigned char* qs = blk + 16;
        int is = 0, qi = 0;
        for (int j = 0; j < 256; j += 64, is += 2) {
            unsigned char scA, mnA, scB, mnB;
            q4k_scale_min(is,     sc, &scA, &mnA);
            q4k_scale_min(is + 1, sc, &scB, &mnB);
            float d1 = super_d * (float)scA, m1 = super_min * (float)mnA;
            float d2 = super_d * (float)scB, m2 = super_min * (float)mnB;
            for (int i = 0; i < 32; i++) {
                int gi = colBase + j + i;
                if (gi < cols)
                    s += ((float)(qs[qi+i] & 0x0Fu) * d1 - m1) * x[gi];
            }
            for (int i = 0; i < 32; i++) {
                int gi = colBase + j + 32 + i;
                if (gi < cols)
                    s += ((float)(qs[qi+i] >>   4u) * d2 - m2) * x[gi];
            }
            qi += 32;
        }
    }
    y[r] = s;
}

// Q6_K: 256-elem super-block = [128 ql | 64 qh | 16 scales(i8) | f16 d]
__global__ void gemv_q6k(const unsigned char* __restrict__ W,
                          const float* __restrict__ x,
                          float* __restrict__ y,
                          int rows, int cols) {
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r >= rows) return;
    int nb  = (cols + 255) / 256;
    int bpb = 210;
    const unsigned char* row = W + r * nb * bpb;
    float s = 0.0f;
    for (int b = 0; b < nb; b++) {
        int colBase = b * 256;
        if (colBase >= cols) break;
        const unsigned char* blk = row + b * bpb;
        const unsigned char* ql = blk;
        const unsigned char* qh = blk + 128;
        const signed char*   sc = (const signed char*)(blk + 192);
        unsigned short d_bits   = (unsigned short)blk[208] | ((unsigned short)blk[209] << 8);
        float d = decode_f16(d_bits);
        for (int j = 0; j < 256; j++) {
            int gi = colBase + j;
            if (gi >= cols) continue;
            int low4 = (int)((ql[j/2] >> (4*(j&1))) & 0x0Fu);
            int hi2  = (int)((qh[j/4] >> (2*(j&3))) & 0x03u);
            int val6 = low4 | (hi2 << 4);
            float fv = d * (float)sc[j/16] * (float)(val6 - 32);
            s += fv * x[gi];
        }
    }
    y[r] = s;
}

// ── Helper: launch the right kernel ──────────────────────────────────────────

namespace retdec::qwen3 {

static void launchGemv(GgufDtype dtype,
                        const void* wDev, const float* xDev, float* yDev,
                        int rows, int cols,
                        cudaStream_t stream) {
    constexpr int BLOCK = 128;
    int grid = (rows + BLOCK - 1) / BLOCK;

    switch (dtype) {
    case GgufDtype::F32:
        gemv_f32<<<grid, BLOCK, 0, stream>>>(
            (const float*)wDev, xDev, yDev, rows, cols);
        break;
    case GgufDtype::F16:
        gemv_f16<<<grid, BLOCK, 0, stream>>>(
            (const unsigned short*)wDev, xDev, yDev, rows, cols);
        break;
    case GgufDtype::BF16:
        gemv_bf16<<<grid, BLOCK, 0, stream>>>(
            (const unsigned short*)wDev, xDev, yDev, rows, cols);
        break;
    case GgufDtype::Q8_0:
        gemv_q8_0<<<grid, BLOCK, 0, stream>>>(
            (const unsigned char*)wDev, xDev, yDev, rows, cols);
        break;
    case GgufDtype::Q4_0:
        gemv_q4_0<<<grid, BLOCK, 0, stream>>>(
            (const unsigned char*)wDev, xDev, yDev, rows, cols);
        break;
    case GgufDtype::Q4_1:
        gemv_q4_1<<<grid, BLOCK, 0, stream>>>(
            (const unsigned char*)wDev, xDev, yDev, rows, cols);
        break;
    case GgufDtype::Q4_K_S:
    case GgufDtype::Q4_K_M:
        gemv_q4k<<<grid, BLOCK, 0, stream>>>(
            (const unsigned char*)wDev, xDev, yDev, rows, cols);
        break;
    case GgufDtype::Q6_K:
        gemv_q6k<<<grid, BLOCK, 0, stream>>>(
            (const unsigned char*)wDev, xDev, yDev, rows, cols);
        break;
    default:
        break; // unsupported — caller falls back to CPU
    }
}

static bool dtypeSupported(GgufDtype d) {
    return d == GgufDtype::F32   || d == GgufDtype::F16   ||
           d == GgufDtype::BF16  || d == GgufDtype::Q8_0  ||
           d == GgufDtype::Q4_0  || d == GgufDtype::Q4_1  ||
           d == GgufDtype::Q4_K_S|| d == GgufDtype::Q4_K_M||
           d == GgufDtype::Q6_K;
}

// ── enumDevices ───────────────────────────────────────────────────────────────

std::vector<CudaDeviceInfo> Qwen3CUDA::enumDevices() {
    std::vector<CudaDeviceInfo> out;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess) return out;
    for (int i = 0; i < count; i++) {
        cudaDeviceProp p{};
        if (cudaGetDeviceProperties(&p, i) != cudaSuccess) continue;
        CudaDeviceInfo info;
        info.index           = i;
        info.name            = p.name;
        info.computeCapability =
            std::to_string(p.major) + "." + std::to_string(p.minor);
        info.totalMemBytes   = static_cast<uint64_t>(p.totalGlobalMem);
        info.multiProcessors = p.multiProcessorCount;
        info.maxThreadsPerBlock = p.maxThreadsPerBlock;
        info.unifiedMemory   = (p.unifiedAddressing != 0);
        info.fp16            = (p.major > 5 || (p.major == 5 && p.minor >= 3));
        // freeMemBytes filled at init time
        out.push_back(info);
    }
    return out;
}

// ── Ctor / Dtor ───────────────────────────────────────────────────────────────

Qwen3CUDA::Qwen3CUDA()  = default;
Qwen3CUDA::~Qwen3CUDA() { shutdown(); }

// ── init ──────────────────────────────────────────────────────────────────────

bool Qwen3CUDA::init(float gpuFraction, int preferredDevice) {
    gpuFraction_ = std::clamp(gpuFraction, 0.f, 1.f);

    int count = 0;
    CUDA_CHECK(cudaGetDeviceCount(&count));
    if (count == 0) {
        lastError_ = "No CUDA devices found";
        return false;
    }

    // Auto-select: pick the device with the most VRAM
    if (preferredDevice < 0 || preferredDevice >= count) {
        size_t bestMem = 0;
        for (int i = 0; i < count; i++) {
            cudaDeviceProp p{};
            cudaGetDeviceProperties(&p, i);
            if (p.totalGlobalMem > bestMem) {
                bestMem   = p.totalGlobalMem;
                deviceId_ = i;
            }
        }
    } else {
        deviceId_ = preferredDevice;
    }

    CUDA_CHECK(cudaSetDevice(deviceId_));

    cudaDeviceProp p{};
    CUDA_CHECK(cudaGetDeviceProperties(&p, deviceId_));

    devInfo_.index           = deviceId_;
    devInfo_.name            = p.name;
    devInfo_.computeCapability =
        std::to_string(p.major) + "." + std::to_string(p.minor);
    devInfo_.totalMemBytes   = static_cast<uint64_t>(p.totalGlobalMem);
    devInfo_.multiProcessors = p.multiProcessorCount;
    devInfo_.maxThreadsPerBlock = p.maxThreadsPerBlock;
    devInfo_.unifiedMemory   = (p.unifiedAddressing != 0);
    devInfo_.fp16            = (p.major > 5 || (p.major == 5 && p.minor >= 3));

    size_t free = 0, total = 0;
    cudaMemGetInfo(&free, &total);
    devInfo_.freeMemBytes = static_cast<uint64_t>(free);

    // Create a dedicated compute stream
    CUDA_CHECK(cudaStreamCreate(reinterpret_cast<cudaStream_t*>(&computeStream_)));

    ready_ = true;
    return true;
}

// ── shutdown ──────────────────────────────────────────────────────────────────

void Qwen3CUDA::shutdown() {
    if (!ready_) return;
    cudaSetDevice(deviceId_);
    releaseAllWeights();
    if (xDevBuf_) {
        cudaFree(xDevBuf_);
        xDevBuf_ = nullptr;
        xDevCap_ = 0;
    }
    if (computeStream_) {
        cudaStreamDestroy(reinterpret_cast<cudaStream_t>(computeStream_));
        computeStream_ = nullptr;
    }
    ready_ = false;
}

// ── setGpuFraction ────────────────────────────────────────────────────────────

void Qwen3CUDA::setGpuFraction(float f) {
    gpuFraction_ = std::clamp(f, 0.f, 1.f);
}

// ── Weight management ─────────────────────────────────────────────────────────

bool Qwen3CUDA::uploadWeight(const void* key,
                              const uint8_t* data, std::size_t bytes) {
    if (!ready_) { lastError_ = "CUDA not initialised"; return false; }
    if (weightCache_.count(key)) return true;
    if (bytes > maxWeightBytes_) {
        lastError_ = "Weight too large for GPU cache";
        return false;
    }
    void* devPtr = nullptr;
    cudaError_t e = cudaMalloc(&devPtr, bytes);
    if (e != cudaSuccess) {
        lastError_ = std::string("cudaMalloc(") + std::to_string(bytes) +
                     ") → " + cudaGetErrorString(e);
        return false;
    }
    e = cudaMemcpy(devPtr, data, bytes, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) {
        cudaFree(devPtr);
        lastError_ = std::string("cudaMemcpy(weight) → ") + cudaGetErrorString(e);
        return false;
    }
    weightCache_[key] = {devPtr, bytes};
    weightBytesOnGpu_ += bytes;
    return true;
}

void Qwen3CUDA::releaseWeight(const void* key) {
    auto it = weightCache_.find(key);
    if (it == weightCache_.end()) return;
    cudaFree(it->second.ptr);
    weightBytesOnGpu_ -= it->second.bytes;
    weightCache_.erase(it);
}

void Qwen3CUDA::releaseAllWeights() {
    for (auto& [k, b] : weightCache_) cudaFree(b.ptr);
    weightCache_.clear();
    weightBytesOnGpu_ = 0;
}

// ── Hybrid GEMV ───────────────────────────────────────────────────────────────

bool Qwen3CUDA::gemv(const uint8_t* wData, const void* wKey,
                      GgufDtype dtype,
                      int rows, int cols,
                      const float* x, float* y) {
    if (!ready_ || rows < minGpuRows_ || !dtypeSupported(dtype)) {
        ops::gemv(wData, dtype, rows, cols, x, y);
        return false;
    }

    // ── Compute total weight bytes ─────────────────────────────────────────────
    std::size_t wTotalBytes = [&] {
        auto bs = ggufDtypeBlockSize(dtype);
        auto be = ggufDtypeBlockElems(dtype);
        if (be == 1) return static_cast<std::size_t>(rows) * cols * bs;
        return ((static_cast<std::size_t>(rows) * cols + be - 1) / be) * bs;
    }();

    // Get or upload weight to GPU
    void* wDev = nullptr;
    {
        auto it = weightCache_.find(wKey);
        if (it != weightCache_.end()) {
            wDev = it->second.ptr;
        } else {
            if (!uploadWeight(wKey, wData, wTotalBytes)) {
                ops::gemv(wData, dtype, rows, cols, x, y);
                return false;
            }
            wDev = weightCache_.at(wKey).ptr;
        }
    }

    // ── Split rows: GPU [0, gpuRows), CPU [gpuRows, rows) ─────────────────────
    int gpuRows = static_cast<int>(rows * gpuFraction_);
    gpuRows = std::max(0, (gpuRows / 64) * 64);  // align to 64
    gpuRows = std::clamp(gpuRows, 0, rows);
    int cpuRows = rows - gpuRows;

    auto stream = reinterpret_cast<cudaStream_t>(computeStream_);

    // Ensure x device buffer is large enough
    if (static_cast<std::size_t>(cols) > xDevCap_) {
        if (xDevBuf_) cudaFree(xDevBuf_);
        cudaError_t e = cudaMalloc(&xDevBuf_,
                                    static_cast<std::size_t>(cols) * sizeof(float));
        if (e != cudaSuccess) {
            xDevBuf_ = nullptr; xDevCap_ = 0;
            ops::gemv(wData, dtype, rows, cols, x, y);
            return false;
        }
        xDevCap_ = static_cast<std::size_t>(cols);
    }

    // Upload x (async)
    cudaMemcpyAsync(xDevBuf_, x,
                    static_cast<std::size_t>(cols) * sizeof(float),
                    cudaMemcpyHostToDevice, stream);

    // GPU output buffer (only for GPU rows)
    void* yDev = nullptr;
    if (gpuRows > 0) {
        if (cudaMalloc(&yDev,
                       static_cast<std::size_t>(gpuRows) * sizeof(float))
            != cudaSuccess) {
            gpuRows = 0;
            cpuRows = rows;
        }
    }

    // ── Launch GPU (async, will overlap with CPU work below) ──────────────────
    if (gpuRows > 0 && yDev) {
        launchGemv(dtype, wDev,
                   reinterpret_cast<const float*>(xDevBuf_),
                   reinterpret_cast<float*>(yDev),
                   gpuRows, cols, stream);
    }

    // ── CPU portion (runs in parallel while GPU is busy) ──────────────────────
    auto cpuFuture = std::async(std::launch::async, [&] {
        if (cpuRows > 0) {
            std::size_t gpuRowBytes = wTotalBytes * gpuRows / rows;
            ops::gemv(wData + gpuRowBytes, dtype, cpuRows, cols,
                      x, y + gpuRows);
        }
    });

    // ── Wait for GPU, read back ───────────────────────────────────────────────
    if (gpuRows > 0 && yDev) {
        cudaStreamSynchronize(stream);
        cudaMemcpy(y, yDev,
                   static_cast<std::size_t>(gpuRows) * sizeof(float),
                   cudaMemcpyDeviceToHost);
        cudaFree(yDev);
    }

    cpuFuture.wait();
    return gpuRows > 0;
}

} // namespace retdec::qwen3

#endif // RETDEC_HAS_CUDA
