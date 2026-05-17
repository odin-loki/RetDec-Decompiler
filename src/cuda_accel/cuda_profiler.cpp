/**
 * @file src/cuda_accel/cuda_profiler.cpp
 * @brief CUDA kernel timing singleton.
 */
#include "retdec/cuda_accel/cuda_profiler.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace retdec::cuda_accel {

// ---------------------------------------------------------------------------

struct CUDAProfiler::Impl {
    mutable std::mutex mu;
    std::unordered_map<std::string, KernelStats> stats;
};

static CUDAProfiler::Impl g_impl;

CUDAProfiler& CUDAProfiler::instance() {
    static CUDAProfiler inst;
    if (!inst.impl_) inst.impl_ = &g_impl;
    return inst;
}

void CUDAProfiler::record(const char* kernelName, std::uint64_t ns) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& s = impl_->stats[kernelName];
    s.totalNs += ns;
    s.minNs    = std::min(s.minNs, ns);
    s.maxNs    = std::max(s.maxNs, ns);
    ++s.launches;
}

void CUDAProfiler::recordNs(const char* kernelName,
                            std::uint64_t startNs,
                            std::uint64_t endNs) {
    if (endNs > startNs) record(kernelName, endNs - startNs);
}

void CUDAProfiler::reset() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->stats.clear();
}

std::size_t CUDAProfiler::totalLaunches() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::size_t n = 0;
    for (auto& [k, v] : impl_->stats) n += v.launches;
    return n;
}

KernelStats CUDAProfiler::statsFor(const std::string& kernelName) const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->stats.find(kernelName);
    return it != impl_->stats.end() ? it->second : KernelStats{};
}

std::vector<std::pair<std::string, KernelStats>> CUDAProfiler::report() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<std::pair<std::string, KernelStats>> out(impl_->stats.begin(),
                                                          impl_->stats.end());
    std::sort(out.begin(), out.end(), [](auto& a, auto& b){
        return a.second.totalNs > b.second.totalNs;
    });
    return out;
}

void CUDAProfiler::printReport() const {
    auto r = report();
    std::printf("%-40s  %8s  %8s  %8s  %10s\n",
                "Kernel", "Launches", "MinMs", "MaxMs", "TotalMs");
    std::printf("%-40s  %8s  %8s  %8s  %10s\n",
                "------", "--------", "-----", "-----", "-------");
    for (auto& [name, s] : r) {
        std::printf("%-40s  %8llu  %8.3f  %8.3f  %10.3f\n",
                    name.c_str(),
                    static_cast<unsigned long long>(s.launches),
                    s.minNs / 1.0e6,
                    s.maxNs / 1.0e6,
                    s.totalNs / 1.0e6);
    }
}

} // namespace retdec::cuda_accel
