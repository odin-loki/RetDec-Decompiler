/**
 * @file src/opencl/ocl_profiler.cpp
 * @brief OCLProfiler implementation: cl_event timing, dynamic stats table, report.
 */

#include "retdec/opencl/ocl_profiler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace retdec {
namespace opencl {

OCLProfiler& OCLProfiler::instance()
{
    static OCLProfiler p;
    return p;
}

void OCLProfiler::record(const char* kernelName, std::uint64_t nanoseconds)
{
    if (!kernelName || !kernelName[0]) {
        kernelName = "(unknown)";
    }

    std::lock_guard<std::mutex> lock(_mtx);

    auto& s = _stats[std::string(kernelName)];
    s.totalNs  += nanoseconds;
    s.launches += 1;
    if (nanoseconds < s.minNs) { s.minNs = nanoseconds; }
    if (nanoseconds > s.maxNs) { s.maxNs = nanoseconds; }
}

void OCLProfiler::recordEvent(const char* kernelName, cl_event ev)
{
    if (!ev) { return; }

    cl_ulong start = 0, end = 0;
    cl_int rc1 = clGetEventProfilingInfo(
        ev, CL_PROFILING_COMMAND_START, sizeof(start), &start, nullptr);
    cl_int rc2 = clGetEventProfilingInfo(
        ev, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr);

    if (rc1 != CL_SUCCESS || rc2 != CL_SUCCESS) {
        return; // Profiling not enabled on the queue, or event invalid.
    }

    if (end > start) {
        record(kernelName, static_cast<std::uint64_t>(end - start));
    }
}

void OCLProfiler::reset()
{
    std::lock_guard<std::mutex> lock(_mtx);
    _stats.clear();
}

std::size_t OCLProfiler::totalLaunches() const
{
    std::lock_guard<std::mutex> lock(_mtx);
    std::size_t n = 0;
    for (const auto& [name, s] : _stats) {
        n += s.launches;
    }
    return n;
}

KernelStats OCLProfiler::statsFor(const std::string& kernelName) const
{
    std::lock_guard<std::mutex> lock(_mtx);
    auto it = _stats.find(kernelName);
    if (it == _stats.end()) { return {}; }
    return it->second;
}

std::vector<std::pair<std::string, KernelStats>> OCLProfiler::report() const
{
    std::lock_guard<std::mutex> lock(_mtx);

    std::vector<std::pair<std::string, KernelStats>> result;
    result.reserve(_stats.size());
    for (const auto& [name, s] : _stats) {
        result.emplace_back(name, s);
    }

    // Sort by total time descending (hottest kernels first).
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) {
            return a.second.totalNs > b.second.totalNs;
        });

    return result;
}

void OCLProfiler::printReport() const
{
    auto rows = report();
    if (rows.empty()) {
        std::fprintf(stderr, "[OCLProfiler] No kernel timing data recorded.\n");
        return;
    }

    std::fprintf(stderr,
        "[OCLProfiler] Kernel timing report (%zu entries)\n"
        "  %-40s  %8s  %10s  %10s  %10s\n",
        rows.size(),
        "Kernel", "Launches", "Total(ms)", "Avg(ms)", "Max(ms)");
    std::fprintf(stderr, "  %s\n", std::string(86, '-').c_str());

    for (const auto& [name, s] : rows) {
        double totalMs = static_cast<double>(s.totalNs) / 1e6;
        double avgMs   = s.launches ? totalMs / s.launches : 0.0;
        double maxMs   = static_cast<double>(s.maxNs) / 1e6;
        std::fprintf(stderr, "  %-40s  %8u  %10.3f  %10.3f  %10.3f\n",
            name.c_str(), s.launches, totalMs, avgMs, maxMs);
    }
}

} // namespace opencl
} // namespace retdec
