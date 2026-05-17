/**
 * @file include/retdec/profiling/profiling.h
 * @brief Performance Profiling Harness — Stage 58.
 *
 * Instruments all pipeline stages with:
 *   - Wall-clock time   (std::chrono::steady_clock, nanosecond resolution)
 *   - Peak RSS tracking (platform-specific: /proc/self/status, getrusage, PSAPI)
 *   - Call count        (invocations per stage)
 *   - Function-level time histogram (user-supplied key → elapsed_ns)
 *   - OpenCL kernel timing (cl_event profiling when CL_QUEUE_PROFILING_ENABLE
 *                           is set; stub-compatible when OpenCL not present)
 *
 * ## Usage — RAII scope timer
 *
 *   {
 *     auto guard = Profiler::instance().measure("type_inference");
 *     runTypeInference();
 *   } // guard destructor records elapsed time
 *
 * ## Usage — manual timer
 *
 *   auto t0 = Profiler::instance().start("structuring");
 *   // ... work ...
 *   Profiler::instance().stop(t0);
 *
 * ## Report
 *
 *   auto report = Profiler::instance().report();
 *   std::cout << report.toText();
 *   report.toCsv("profile.csv");
 *
 * ## Thread safety
 *
 *   All public methods are thread-safe via a shared mutex.
 *   Each recording is atomic at the update level; concurrent stages are
 *   supported (wall-clock time is the elapsed real time for each stage, not
 *   the sum of all threads).
 */

#ifndef RETDEC_PROFILING_PROFILING_H
#define RETDEC_PROFILING_PROFILING_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec::profiling {

using Clock    = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Nanos    = std::int64_t;  ///< nanoseconds

// ─── StageRecord ─────────────────────────────────────────────────────────────

/**
 * @brief Accumulated timing data for one pipeline stage.
 */
struct StageRecord {
    std::string name;
    Nanos       totalNs    = 0;
    Nanos       minNs      = INT64_MAX;
    Nanos       maxNs      = 0;
    int64_t     callCount  = 0;

    double totalMs()   const { return static_cast<double>(totalNs)  / 1e6; }
    double averageMs() const {
        return callCount > 0 ? static_cast<double>(totalNs) / callCount / 1e6 : 0.0;
    }
    double minMs() const { return static_cast<double>(minNs) / 1e6; }
    double maxMs() const { return static_cast<double>(maxNs) / 1e6; }
};

// ─── FunctionSample ──────────────────────────────────────────────────────────

/**
 * @brief A single timing sample for a named function.
 */
struct FunctionSample {
    std::string key;
    Nanos       elapsedNs = 0;
};

// ─── OpenCL kernel record ─────────────────────────────────────────────────────

/**
 * @brief Accumulated kernel execution timing from cl_event profiling.
 */
struct KernelRecord {
    std::string name;
    Nanos       totalNs   = 0;
    int64_t     launches  = 0;
    double      totalMs() const { return static_cast<double>(totalNs) / 1e6; }
    double      avgMs()   const {
        return launches > 0 ? static_cast<double>(totalNs) / launches / 1e6 : 0.0;
    }
};

// ─── ProfilingReport ─────────────────────────────────────────────────────────

struct ProfilingReport {
    std::vector<StageRecord>    stages;
    std::vector<KernelRecord>   kernels;
    std::vector<FunctionSample> functionSamples;
    Nanos     totalWallNs  = 0;
    int64_t   peakRssBytes = 0;

    /**
     * @brief Format as a human-readable text table.
     */
    std::string toText() const;

    /**
     * @brief Write as CSV to a file.
     *        Returns false on I/O error.
     */
    bool toCsv(const std::string& path) const;

    /**
     * @brief Format as JSON.
     */
    std::string toJson() const;

    /**
     * @brief Fraction of total wall time taken by a named stage (0.0–1.0).
     */
    double stageFraction(const std::string& stageName) const;
};

// ─── ScopeTimer (RAII) ───────────────────────────────────────────────────────

/**
 * @brief RAII scope timer.  Records elapsed time into the Profiler on destruction.
 */
class ScopeTimer {
public:
    ScopeTimer(std::string stageName, class Profiler& profiler);
    ~ScopeTimer();

    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;
    ScopeTimer(ScopeTimer&&) noexcept;

private:
    std::string  stageName_;
    TimePoint    start_;
    Profiler*    profiler_;
    bool         active_ = true;
};

// ─── Profiler ────────────────────────────────────────────────────────────────

/**
 * @brief Main profiling singleton.
 *
 * Designed to be low-overhead in the common case (a single mutex lock per
 * stage begin/end).  Disabled entirely if `enabled_ == false`.
 */
class Profiler {
public:
    static Profiler& instance();

    /**
     * @brief Enable or disable all profiling.  When disabled, all record()
     *        calls are no-ops and ScopeTimer destructors do nothing.
     */
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled()        const { return enabled_; }

    /**
     * @brief Begin a named stage measurement.  Returns a ScopeTimer that
     *        automatically stops on destruction.
     */
    ScopeTimer measure(const std::string& stageName);

    /**
     * @brief Manual start.  Returns a token TimePoint to pass to stop().
     */
    TimePoint start(const std::string& stageName);

    /**
     * @brief Record elapsed time for a stage started at t0.
     */
    void stop(const std::string& stageName, TimePoint t0);

    /**
     * @brief Record a single function-level timing sample.
     */
    void recordFunction(const std::string& key, Nanos elapsedNs);

    /**
     * @brief Record an OpenCL kernel execution time.
     */
    void recordKernel(const std::string& kernelName, Nanos elapsedNs);

    /**
     * @brief Sample peak RSS immediately.
     *        Stores result in lastRssBytes_.
     */
    void sampleRss();

    /**
     * @brief Return the current profiling report and optionally reset counters.
     */
    ProfilingReport report(bool resetAfter = false);

    /**
     * @brief Reset all accumulated data.
     */
    void reset();

    /**
     * @brief Run the given callable and return its wall-clock duration in ms.
     */
    template<typename Fn>
    double time(const std::string& stageName, Fn&& fn) {
        auto t0 = Clock::now();
        std::forward<Fn>(fn)();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - t0).count();
        if (enabled_) {
            std::lock_guard<std::mutex> lk(mutex_);
            record(stageName, static_cast<Nanos>(ns));
        }
        return static_cast<double>(ns) / 1e6;
    }

private:
    Profiler() = default;

    void record(const std::string& stageName, Nanos elapsedNs);

    mutable std::mutex mutex_;
    bool               enabled_       = true;
    Nanos              totalWallNs_   = 0;
    int64_t            lastRssBytes_  = 0;

    std::unordered_map<std::string, StageRecord>  stages_;
    std::unordered_map<std::string, KernelRecord> kernels_;
    std::vector<FunctionSample>                   funcSamples_;
};

// ─── RssTracker ──────────────────────────────────────────────────────────────

/**
 * @brief Cross-platform peak RSS tracker.
 *
 *   Linux  : reads /proc/self/status VmPeak line
 *   macOS  : uses getrusage RUSAGE_SELF ru_maxrss (bytes)
 *   Windows: uses PSAPI GetProcessMemoryInfo
 */
class RssTracker {
public:
    /**
     * @brief Returns current peak RSS in bytes.
     *        Returns 0 if unavailable on this platform.
     */
    static int64_t peakRssBytes();

    /**
     * @brief Returns current RSS (resident set size) in bytes.
     */
    static int64_t currentRssBytes();
};

// ─── FunctionHistogram ────────────────────────────────────────────────────────

/**
 * @brief Accumulates timing samples into a histogram.
 *
 * Bucket boundaries are set at construction (logarithmic by default).
 * Call add(ns) for each sample; query() returns the bucket counts.
 */
class FunctionHistogram {
public:
    /**
     * @brief Construct with N logarithmically spaced buckets from minNs to maxNs.
     */
    FunctionHistogram(Nanos minNs = 1'000,          // 1 µs
                       Nanos maxNs = 1'000'000'000,  // 1 s
                       int   buckets = 20);

    void add(Nanos ns);

    struct Bucket {
        Nanos  lo, hi;
        int64_t count = 0;
    };
    const std::vector<Bucket>& buckets() const { return buckets_; }
    int64_t totalSamples() const { return totalSamples_; }
    Nanos   percentile(double p) const; ///< 0.0–1.0

    std::string format() const;

private:
    std::vector<Bucket> buckets_;
    int64_t             totalSamples_ = 0;
    // sorted raw samples for percentile computation (up to 10000)
    std::vector<Nanos>  rawSamples_;
    static const int    kMaxRaw = 10000;
};

} // namespace retdec::profiling

#endif // RETDEC_PROFILING_PROFILING_H
