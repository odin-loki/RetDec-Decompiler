/**
 * @file include/retdec/qwen3/qwen3_cuda.h
 * @brief CUDA acceleration for Qwen3 inference — hybrid CPU + GPU GEMV.
 *
 * Drop-in replacement for Qwen3OpenCL.  Same public interface; same hybrid
 * split (configurable GPU fraction, default 80 %).  Uses CUDA streams instead
 * of OpenCL command queues so the GPU and CPU portions overlap.
 *
 * ## Supported GEMV dtypes (on GPU)
 *
 *   F32 · F16 · BF16 · Q8_0 · Q4_0 · Q4_1 · Q4_K_M/S · Q6_K
 *
 * Everything else falls back to CPU automatically.
 *
 * ## Usage
 *
 *   Qwen3CUDA cuda;
 *   if (cuda.init()) {
 *       cuda.setGpuFraction(0.80f);
 *       retdec::qwen3::ops::setCUDA(&cuda);
 *   }
 *
 * When built without CUDA (RETDEC_HAS_CUDA not defined) all methods are
 * no-ops so callers need no conditional compilation.
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_QWEN3_CUDA_H
#define RETDEC_QWEN3_CUDA_H

#include "retdec/qwen3/qwen3_weights.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec::qwen3 {

// ─── CudaDeviceInfo ───────────────────────────────────────────────────────────

struct CudaDeviceInfo {
    int         index          = -1;
    std::string name;
    std::string computeCapability;
    uint64_t    totalMemBytes  = 0;
    uint64_t    freeMemBytes   = 0;
    int         multiProcessors = 0;
    int         maxThreadsPerBlock = 0;
    bool        unifiedMemory  = false;
    bool        fp16           = false;  ///< compute capability >= 5.3
};

// ─── Qwen3CUDA ────────────────────────────────────────────────────────────────

class Qwen3CUDA {
public:
    Qwen3CUDA();
    ~Qwen3CUDA();

    Qwen3CUDA(const Qwen3CUDA&)            = delete;
    Qwen3CUDA& operator=(const Qwen3CUDA&) = delete;

    // ── Initialisation ────────────────────────────────────────────────────────

    /**
     * @brief Select the best CUDA device and create streams.
     *
     * @param gpuFraction  Fraction of rows handled by the GPU [0.0, 1.0].
     * @param deviceIndex  Preferred device index (-1 = auto-select by VRAM).
     * @return true if a CUDA device was successfully initialised.
     */
    bool init(float gpuFraction = 0.80f, int deviceIndex = -1);

    void setGpuFraction(float f);
    void shutdown();

    bool isReady()   const { return ready_; }
    bool hasGpu()    const { return ready_; }

    // ── Device info ───────────────────────────────────────────────────────────

    const CudaDeviceInfo& deviceInfo() const { return devInfo_; }
    static std::vector<CudaDeviceInfo> enumDevices();

    // ── Weight buffer management ──────────────────────────────────────────────

    /**
     * @brief Upload weight bytes to GPU memory (idempotent — caches by pointer).
     */
    bool uploadWeight(const void* key,
                      const uint8_t* data, std::size_t bytes);
    void releaseWeight(const void* key);
    void releaseAllWeights();

    std::size_t weightBytesOnGpu() const { return weightBytesOnGpu_; }

    // ── GEMV dispatch ─────────────────────────────────────────────────────────

    /**
     * @brief Hybrid CPU+GPU GEMV:  y = W * x.
     *
     * GPU handles [0, gpuRows), CPU handles [gpuRows, rows) in parallel.
     *
     * @return true if any GPU work was dispatched.
     */
    bool gemv(const uint8_t* wData, const void* wKey,
              GgufDtype dtype,
              int rows, int cols,
              const float* x, float* y);

    // ── Tuning ────────────────────────────────────────────────────────────────

    void setMinGpuRows(int n)            { minGpuRows_    = n; }
    void setMaxWeightBytes(std::size_t b){ maxWeightBytes_ = b; }

    const std::string& lastError() const { return lastError_; }

private:
#ifdef RETDEC_HAS_CUDA
    struct DeviceBuf { void* ptr; std::size_t bytes; };
    std::unordered_map<const void*, DeviceBuf> weightCache_;

    void* computeStream_ = nullptr;  // cudaStream_t
    void* xDevBuf_       = nullptr;  // persistent device buffer for x
    std::size_t xDevCap_ = 0;        // capacity of xDevBuf_ in floats
    int  deviceId_       = 0;
#endif

    CudaDeviceInfo devInfo_;
    float          gpuFraction_     = 0.80f;
    bool           ready_           = false;
    int            minGpuRows_      = 64;
    std::size_t    maxWeightBytes_  = std::size_t(20) << 30; // 20 GiB (fits RTX 3090)
    std::size_t    weightBytesOnGpu_ = 0;
    mutable std::string lastError_;
};

} // namespace retdec::qwen3

#endif // RETDEC_QWEN3_CUDA_H
