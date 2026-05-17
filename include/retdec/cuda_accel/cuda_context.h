/**
 * @file include/retdec/cuda_accel/cuda_context.h
 * @brief CUDA device context — replaces OCLContext.
 *
 * CUDAContext owns a CUDA stream and the selected device. It provides the
 * same high-level interface as OCLContext so callers can be updated with
 * minimal changes.
 *
 * When RETDEC_HAS_CUDA is not defined a stub is compiled that always reports
 * no GPU and falls back to the CPU paths in each accelerated module.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef RETDEC_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace retdec::cuda_accel {

/// Device summary (mirrors OCLDeviceInfo).
struct CUDADeviceInfo {
    int         id{-1};
    std::string name;
    std::string vendor{"NVIDIA"};
    std::string driverVersion;
    std::size_t globalMemBytes{0};
    std::size_t sharedMemPerBlock{0};
    int         maxThreadsPerBlock{0};
    int         multiProcessorCount{0};
    int         computeMajor{0};
    int         computeMinor{0};
    bool        available{false};

    int score() const noexcept {
        if (!available) return -1;
        return computeMajor * 100 + computeMinor + multiProcessorCount;
    }
};

/// Kernel launch descriptor.
struct CUDAKernelLaunch {
    std::string kernelName;
    std::size_t globalSize{0};
    std::size_t blockSize{256};
    std::function<void()> launch; ///< callable that performs the actual <<<>>> call
};

/**
 * CUDAContext — singleton-friendly RAII wrapper around a CUDA device + stream.
 *
 * Thread safety: one context per thread is recommended. The default
 * implementation uses stream 0 (the default stream).
 */
class CUDAContext {
public:
    CUDAContext();
    ~CUDAContext();

    CUDAContext(const CUDAContext&)            = delete;
    CUDAContext& operator=(const CUDAContext&) = delete;
    CUDAContext(CUDAContext&&)                 = delete;
    CUDAContext& operator=(CUDAContext&&)      = delete;

    /// Initialise: pick the best GPU. Returns false if no CUDA device found.
    bool initialize();

    /// Release all resources.
    void reset();

    bool isReady()  const noexcept { return ready_; }
    int  deviceId() const noexcept { return devId_; }

    const CUDADeviceInfo&              primaryDevice() const noexcept { return primary_; }
    const std::vector<CUDADeviceInfo>& allDevices()    const noexcept { return devices_; }

    /// Allocate device memory; returns nullptr on failure.
    void* createBuffer(std::size_t bytes, int* errOut = nullptr);

    /// Copy host → device.
    int writeBuffer(void* devPtr, const void* src, std::size_t bytes);

    /// Copy device → host.
    int readBuffer(void* devPtr, void* dst, std::size_t bytes);

    /// Free device memory.
    void freeBuffer(void* devPtr);

    /// Block until all GPU work on the internal stream is done.
    int synchronize();

    const std::string& lastError() const noexcept { return lastError_; }

#ifdef RETDEC_HAS_CUDA
    cudaStream_t stream()   const noexcept { return stream_; }
#endif

private:
    bool        ready_{false};
    int         devId_{-1};
    std::string lastError_;
    CUDADeviceInfo              primary_;
    std::vector<CUDADeviceInfo> devices_;

#ifdef RETDEC_HAS_CUDA
    cudaStream_t stream_{nullptr};
#endif
};

} // namespace retdec::cuda_accel
