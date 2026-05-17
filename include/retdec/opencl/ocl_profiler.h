/**
 * @file include/retdec/opencl/ocl_profiler.h
 * @brief OpenCL kernel timing accumulator (host-side).
 *
 * Records cl_event-based nanosecond timing per kernel name.
 * Thread-safe via a simple mutex.
 */

#ifndef RETDEC_OPENCL_OCL_PROFILER_H
#define RETDEC_OPENCL_OCL_PROFILER_H

#include <CL/cl.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace opencl {

// ─── Per-kernel statistics ────────────────────────────────────────────────────
struct KernelStats {
    std::uint64_t totalNs   = 0;  ///< Sum of all launch durations in nanoseconds.
    std::uint64_t minNs     = UINT64_MAX; ///< Fastest single launch.
    std::uint64_t maxNs     = 0;  ///< Slowest single launch.
    std::uint32_t launches  = 0;  ///< Total launch count.

    double avgMs() const noexcept
    {
        return launches ? static_cast<double>(totalNs) / launches / 1e6 : 0.0;
    }
};

// ─── OCLProfiler singleton ────────────────────────────────────────────────────
class OCLProfiler {
public:
    static OCLProfiler& instance();

    /// Record a kernel launch duration given explicit nanosecond count.
    void record(const char* kernelName, std::uint64_t nanoseconds);

    /// Record timing from a profiling-enabled cl_event (blocking query).
    /// Silently ignores events where timing is unavailable.
    void recordEvent(const char* kernelName, cl_event ev);

    /// Reset all accumulated statistics.
    void reset();

    /// Total number of individual kernel launches recorded.
    std::size_t totalLaunches() const;

    /// Return stats for a specific kernel (returns default if not found).
    KernelStats statsFor(const std::string& kernelName) const;

    /// Snapshot of all kernel stats (sorted by total time descending).
    std::vector<std::pair<std::string, KernelStats>> report() const;

    /// Print a formatted report to stderr.
    void printReport() const;

private:
    OCLProfiler() = default;

    mutable std::mutex _mtx;
    std::unordered_map<std::string, KernelStats> _stats;
};

} // namespace opencl
} // namespace retdec

#endif // RETDEC_OPENCL_OCL_PROFILER_H
