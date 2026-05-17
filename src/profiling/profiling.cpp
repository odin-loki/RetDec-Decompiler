/**
 * @file src/profiling/profiling.cpp
 * @brief Performance Profiling Harness implementation.
 */

#include "retdec/profiling/profiling.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

#if defined(_WIN32)
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#  include <sys/resource.h>
#else
#  include <sys/resource.h>
#  include <fstream>
#endif

namespace retdec::profiling {

// ─── Profiler singleton ───────────────────────────────────────────────────────

Profiler& Profiler::instance() {
    static Profiler p;
    return p;
}

// ─── Profiler::record (internal, called with lock held) ──────────────────────

void Profiler::record(const std::string& stageName, Nanos elapsedNs) {
    auto& rec = stages_[stageName];
    rec.name = stageName;
    rec.totalNs   += elapsedNs;
    rec.callCount++;
    if (elapsedNs < rec.minNs) rec.minNs = elapsedNs;
    if (elapsedNs > rec.maxNs) rec.maxNs = elapsedNs;
    totalWallNs_  += elapsedNs;
}

// ─── Profiler::measure ───────────────────────────────────────────────────────

ScopeTimer Profiler::measure(const std::string& stageName) {
    return ScopeTimer(stageName, *this);
}

TimePoint Profiler::start(const std::string&) {
    return Clock::now();
}

void Profiler::stop(const std::string& stageName, TimePoint t0) {
    if (!enabled_) return;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - t0).count();
    std::lock_guard<std::mutex> lk(mutex_);
    record(stageName, static_cast<Nanos>(ns));
}

// ─── Profiler::recordFunction ────────────────────────────────────────────────

void Profiler::recordFunction(const std::string& key, Nanos elapsedNs) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(mutex_);
    funcSamples_.push_back({key, elapsedNs});
}

// ─── Profiler::recordKernel ───────────────────────────────────────────────────

void Profiler::recordKernel(const std::string& kernelName, Nanos elapsedNs) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(mutex_);
    auto& rec = kernels_[kernelName];
    rec.name      = kernelName;
    rec.totalNs  += elapsedNs;
    rec.launches++;
}

// ─── Profiler::sampleRss ─────────────────────────────────────────────────────

void Profiler::sampleRss() {
    int64_t rss = RssTracker::peakRssBytes();
    std::lock_guard<std::mutex> lk(mutex_);
    lastRssBytes_ = rss;
}

// ─── Profiler::report ────────────────────────────────────────────────────────

ProfilingReport Profiler::report(bool resetAfter) {
    std::lock_guard<std::mutex> lk(mutex_);
    ProfilingReport r;
    r.totalWallNs  = totalWallNs_;
    r.peakRssBytes = lastRssBytes_;
    for (const auto& [_, s] : stages_) r.stages.push_back(s);
    for (const auto& [_, k] : kernels_) r.kernels.push_back(k);
    r.functionSamples = funcSamples_;

    // Sort stages by total time descending
    std::sort(r.stages.begin(), r.stages.end(),
        [](const StageRecord& a, const StageRecord& b){ return a.totalNs > b.totalNs; });
    std::sort(r.kernels.begin(), r.kernels.end(),
        [](const KernelRecord& a, const KernelRecord& b){ return a.totalNs > b.totalNs; });

    if (resetAfter) {
        stages_.clear();
        kernels_.clear();
        funcSamples_.clear();
        totalWallNs_  = 0;
        lastRssBytes_ = 0;
    }
    return r;
}

void Profiler::reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    stages_.clear();
    kernels_.clear();
    funcSamples_.clear();
    totalWallNs_  = 0;
    lastRssBytes_ = 0;
}

// ─── ProfilingReport::toText ─────────────────────────────────────────────────

std::string ProfilingReport::toText() const {
    std::ostringstream os;
    os << "┌─────────────────────────────────────────────────────────────────────┐\n";
    os << "│                    RetDec Pipeline Profile Report                   │\n";
    os << "├──────────────────────────┬────────┬────────┬────────┬────────┬──────┤\n";
    os << "│ Stage                    │ Total  │ Avg    │ Min    │ Max    │Calls │\n";
    os << "├──────────────────────────┼────────┼────────┼────────┼────────┼──────┤\n";

    double totalMs = static_cast<double>(totalWallNs) / 1e6;

    for (const auto& s : stages) {
        double frac = totalMs > 0.0 ? s.totalMs() / totalMs * 100.0 : 0.0;
        os << "│ " << std::left << std::setw(24) << s.name.substr(0, 24)
           << " │" << std::right << std::setw(7) << std::fixed << std::setprecision(1)
           << s.totalMs() << " │"
           << std::setw(7) << s.averageMs() << " │"
           << std::setw(7) << (s.minNs == INT64_MAX ? 0.0 : s.minMs()) << " │"
           << std::setw(7) << s.maxMs() << " │"
           << std::setw(5) << s.callCount << " │"
           << "  " << std::setprecision(1) << frac << "%\n";
    }

    os << "├──────────────────────────┴────────┴────────┴────────┴────────┴──────┤\n";
    os << "│ Total wall time: " << std::fixed << std::setprecision(1)
       << totalMs << " ms";
    if (peakRssBytes > 0) {
        os << "   Peak RSS: " << peakRssBytes / (1024 * 1024) << " MB";
    }
    os << "\n└─────────────────────────────────────────────────────────────────────┘\n";

    if (!kernels.empty()) {
        os << "\n OpenCL Kernel Timings:\n";
        os << " ──────────────────────────────────────────────\n";
        for (const auto& k : kernels) {
            os << "  " << std::left << std::setw(30) << k.name.substr(0, 30)
               << "  total=" << std::setw(8) << std::fixed << std::setprecision(2) << k.totalMs()
               << " ms  launches=" << k.launches
               << "  avg=" << std::setprecision(3) << k.avgMs() << " ms\n";
        }
    }

    return os.str();
}

// ─── ProfilingReport::toCsv ──────────────────────────────────────────────────

bool ProfilingReport::toCsv(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << "stage,total_ms,avg_ms,min_ms,max_ms,calls,fraction_pct\n";
    double totalMs = static_cast<double>(totalWallNs) / 1e6;
    for (const auto& s : stages) {
        double frac = totalMs > 0.0 ? s.totalMs() / totalMs * 100.0 : 0.0;
        f << '"' << s.name << '"' << ','
          << std::fixed << std::setprecision(3)
          << s.totalMs() << ',' << s.averageMs() << ','
          << (s.minNs == INT64_MAX ? 0.0 : s.minMs()) << ','
          << s.maxMs() << ',' << s.callCount << ','
          << frac << '\n';
    }
    if (!kernels.empty()) {
        f << "\nkernel,total_ms,avg_ms,launches\n";
        for (const auto& k : kernels)
            f << '"' << k.name << '"' << ','
              << std::fixed << std::setprecision(3)
              << k.totalMs() << ',' << k.avgMs() << ',' << k.launches << '\n';
    }
    return true;
}

// ─── ProfilingReport::toJson ─────────────────────────────────────────────────

std::string ProfilingReport::toJson() const {
    std::ostringstream os;
    os << "{\n  \"total_wall_ms\": " << std::fixed << std::setprecision(3)
       << static_cast<double>(totalWallNs) / 1e6;
    if (peakRssBytes > 0)
        os << ",\n  \"peak_rss_bytes\": " << peakRssBytes;
    os << ",\n  \"stages\": [\n";
    double totalMs = static_cast<double>(totalWallNs) / 1e6;
    for (size_t i = 0; i < stages.size(); ++i) {
        const auto& s = stages[i];
        double frac = totalMs > 0.0 ? s.totalMs() / totalMs * 100.0 : 0.0;
        os << "    {\"name\":\"" << s.name << "\","
           << "\"total_ms\":" << s.totalMs() << ","
           << "\"avg_ms\":" << s.averageMs() << ","
           << "\"calls\":" << s.callCount << ","
           << "\"fraction_pct\":" << frac << "}";
        if (i + 1 < stages.size()) os << ",";
        os << "\n";
    }
    os << "  ]";
    if (!kernels.empty()) {
        os << ",\n  \"kernels\": [\n";
        for (size_t i = 0; i < kernels.size(); ++i) {
            const auto& k = kernels[i];
            os << "    {\"name\":\"" << k.name << "\","
               << "\"total_ms\":" << k.totalMs() << ","
               << "\"avg_ms\":" << k.avgMs() << ","
               << "\"launches\":" << k.launches << "}";
            if (i + 1 < kernels.size()) os << ",";
            os << "\n";
        }
        os << "  ]";
    }
    os << "\n}\n";
    return os.str();
}

double ProfilingReport::stageFraction(const std::string& stageName) const {
    if (totalWallNs == 0) return 0.0;
    for (const auto& s : stages)
        if (s.name == stageName)
            return static_cast<double>(s.totalNs) / totalWallNs;
    return 0.0;
}

// ─── ScopeTimer ──────────────────────────────────────────────────────────────

ScopeTimer::ScopeTimer(std::string stageName, Profiler& profiler)
    : stageName_(std::move(stageName)),
      start_(Clock::now()),
      profiler_(&profiler) {}

ScopeTimer::ScopeTimer(ScopeTimer&& other) noexcept
    : stageName_(std::move(other.stageName_)),
      start_(other.start_),
      profiler_(other.profiler_),
      active_(other.active_) {
    other.active_ = false;
}

ScopeTimer::~ScopeTimer() {
    if (!active_ || !profiler_) return;
    profiler_->stop(stageName_, start_);
}

// ─── RssTracker ──────────────────────────────────────────────────────────────

#if defined(_WIN32)
int64_t RssTracker::peakRssBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<int64_t>(pmc.PeakWorkingSetSize);
    return 0;
}
int64_t RssTracker::currentRssBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<int64_t>(pmc.WorkingSetSize);
    return 0;
}
#elif defined(__APPLE__)
int64_t RssTracker::peakRssBytes() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<int64_t>(ru.ru_maxrss);  // bytes on macOS
}
int64_t RssTracker::currentRssBytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<int64_t>(info.resident_size);
    return 0;
}
#else
int64_t RssTracker::peakRssBytes() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmPeak:", 0) == 0) {
            // VmPeak:   12345 kB
            int64_t kb = 0;
            sscanf(line.c_str() + 7, " %lld", &kb);
            return kb * 1024;
        }
    }
    return 0;
}
int64_t RssTracker::currentRssBytes() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            int64_t kb = 0;
            sscanf(line.c_str() + 6, " %lld", &kb);
            return kb * 1024;
        }
    }
    return 0;
}
#endif

// ─── FunctionHistogram ───────────────────────────────────────────────────────

FunctionHistogram::FunctionHistogram(Nanos minNs, Nanos maxNs, int buckets) {
    buckets_.resize(buckets);
    double logMin = std::log10(static_cast<double>(minNs));
    double logMax = std::log10(static_cast<double>(maxNs));
    double step   = (logMax - logMin) / buckets;
    for (int i = 0; i < buckets; ++i) {
        buckets_[i].lo = static_cast<Nanos>(std::pow(10.0, logMin + i * step));
        buckets_[i].hi = static_cast<Nanos>(std::pow(10.0, logMin + (i+1) * step));
    }
}

void FunctionHistogram::add(Nanos ns) {
    ++totalSamples_;
    bool placed = false;
    for (auto& b : buckets_) {
        if (ns >= b.lo && ns < b.hi) { ++b.count; placed = true; break; }
    }
    // Underflow/overflow: ensure every sample lands in some bucket.
    if (!placed && !buckets_.empty()) {
        if (ns < buckets_.front().lo)
            ++buckets_.front().count;
        else
            ++buckets_.back().count;
    }
    if (static_cast<int>(rawSamples_.size()) < kMaxRaw)
        rawSamples_.push_back(ns);
}

Nanos FunctionHistogram::percentile(double p) const {
    if (rawSamples_.empty()) return 0;
    std::vector<Nanos> sorted = rawSamples_;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
    return sorted[idx];
}

std::string FunctionHistogram::format() const {
    std::ostringstream os;
    os << "Function timing histogram (" << totalSamples_ << " samples):\n";
    int64_t maxCount = 0;
    for (const auto& b : buckets_) maxCount = std::max(maxCount, b.count);
    for (const auto& b : buckets_) {
        int barLen = maxCount > 0 ?
            static_cast<int>(40.0 * b.count / maxCount) : 0;
        os << std::setw(8) << static_cast<double>(b.lo) / 1e6 << "ms "
           << std::string(barLen, '█')
           << " " << b.count << "\n";
    }
    if (!rawSamples_.empty()) {
        os << "P50=" << percentile(0.5)  / 1e3 << "µs  "
           << "P90=" << percentile(0.9)  / 1e3 << "µs  "
           << "P99=" << percentile(0.99) / 1e3 << "µs\n";
    }
    return os.str();
}

} // namespace retdec::profiling
