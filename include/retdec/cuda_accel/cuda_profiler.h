/**
 * @file include/retdec/cuda_accel/cuda_profiler.h
 * @brief CUDA kernel timing — replaces OCLProfiler.
 *
 * Singleton that records per-kernel statistics using CUDA events.
 * Falls back to std::chrono timing when RETDEC_HAS_CUDA is not defined.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace retdec::cuda_accel {

struct KernelStats {
    std::uint64_t totalNs{0};
    std::uint64_t minNs{UINT64_MAX};
    std::uint64_t maxNs{0};
    std::uint64_t launches{0};

    double avgMs() const noexcept {
        return launches ? static_cast<double>(totalNs) / (launches * 1.0e6) : 0.0;
    }
};

class CUDAProfiler {
public:
    // Impl is public so it can be defined and instantiated in the .cpp file.
    struct Impl;

    static CUDAProfiler& instance();

    /// Record a timing in nanoseconds.
    void record(const char* kernelName, std::uint64_t nanoseconds);

    /// Record timing from a host-side timer (std::chrono) start/end pair in ns.
    void recordNs(const char* kernelName, std::uint64_t startNs, std::uint64_t endNs);

    void reset();

    std::size_t totalLaunches() const;

    KernelStats statsFor(const std::string& kernelName) const;

    std::vector<std::pair<std::string, KernelStats>> report() const;

    void printReport() const;

private:
    CUDAProfiler() = default;

    Impl* impl_ = nullptr;
};

} // namespace retdec::cuda_accel
