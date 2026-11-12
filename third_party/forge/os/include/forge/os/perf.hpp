#pragma once
// High-resolution timing and hardware performance counters.
//   cycle_counter(): rdtsc (x86, GCC/Clang and MSVC) / mach_absolute_time (Apple) / cntvct_el0 (other arm64) /
//   monotonic_ns elsewhere; counter_hz() its frequency (mach timebase, cntfrq_el0, or TSC calibrated against the
//   monotonic clock).  Apple: mach_absolute_time, not a raw cntvct_el0 read - in Virtualization.framework guests
//   (GitHub's macOS runners) the raw register did not advance across a short loop.
//   precise_sleep_ns(): Windows high-resolution waitable timer; POSIX absolute clock_nanosleep + short spin.
//   perf_counters: Linux/Android perf_event_open (cycles, instructions, cache refs/misses, branch misses; obeys
//   perf_event_paranoid).  Intel hybrid CPUs get one counter group per core PMU (cpu_core, cpu_atom) and the
//   counts are summed; a group the kernel multiplexed is scaled by time_enabled/time_running and the reading is
//   flagged `scaled`.  macOS: kperf (kpc_* fixed counters: cycles, instructions; needs root), else
//   proc_pid_rusage(RUSAGE_INFO_V4) ri_cycles/ri_instructions without root (process-wide, not per thread).
//   FreeBSD: libpmc (dlopen; needs the hwpmc module) process-virtual counters, else TSC "cycles".  Windows:
//   QueryThreadCycleTime (cycles only).  iOS: CNTVCT "cycles" only, the rest unsupported.
#include <forge/os/detail.hpp>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#if defined(FORGE_OS_MACOS)
#include <libproc.h>
#include <sys/resource.h>
#endif
#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#endif
#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
#include <x86intrin.h>
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace forge::os {
inline std::uint64_t cycle_counter() {
#if defined(__APPLE__) && defined(__aarch64__)
    return mach_absolute_time();
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t v; asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    return monotonic_ns();
#endif
}
inline std::uint64_t counter_hz() {
#if defined(__APPLE__) && defined(__aarch64__)
    static const std::uint64_t hz = [] { mach_timebase_info_data_t tb{}; mach_timebase_info(&tb);
                                          return tb.numer ? 1000000000ull * tb.denom / tb.numer : 24000000ull; }();
    return hz;
#elif defined(__aarch64__)
    std::uint64_t f; asm volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f;
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    static const std::uint64_t hz = [] {
        std::uint64_t t0 = monotonic_ns(), c0 = cycle_counter(); while (monotonic_ns() - t0 < 20000000ull) {}
        std::uint64_t t1 = monotonic_ns(), c1 = cycle_counter(); return (c1 - c0) * 1000000000ull / (t1 - t0);
    }();
    return hz;
#else
    return 1000000000ull;
#endif
}
inline std::uint64_t timer_resolution_ns() {
#if defined(FORGE_OS_WINDOWS)
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); return std::uint64_t(1000000000ull / std::uint64_t(f.QuadPart ? f.QuadPart : 1)) + 1;
#else
    timespec r{}; ::clock_getres(CLOCK_MONOTONIC, &r); return std::uint64_t(r.tv_sec) * 1000000000ull + std::uint64_t(r.tv_nsec);
#endif
}
// Sleep for ns with sub-millisecond accuracy (spins for the final stretch).
inline void precise_sleep_ns(std::uint64_t ns) {
    std::uint64_t end = monotonic_ns() + ns; const std::uint64_t spin = 200000;   // 200us
    if (ns > spin) {
#if defined(FORGE_OS_WINDOWS)
        struct timer { HANDLE h; ~timer() { if (h) CloseHandle(h); } };   // one per thread, closed at thread exit
        static thread_local timer t{[] { HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS); return h ? h : CreateWaitableTimerW(nullptr, TRUE, nullptr); }()};
        LARGE_INTEGER due; due.QuadPart = -std::int64_t((ns - spin) / 100);
        if (t.h && SetWaitableTimer(t.h, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(t.h, INFINITE); else sleep_ns(ns - spin);
#elif defined(__APPLE__)
        sleep_ns(ns - spin);
#else
        timespec ts; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        std::uint64_t a = std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec) + ns - spin;
        timespec abs{time_t(a / 1000000000ull), long(a % 1000000000ull)};
        while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, nullptr) == EINTR) {}
#endif
    }
    while (monotonic_ns() < end) {}
}

enum class perf_event { cycles, instructions, cache_references, cache_misses, branch_misses };
inline const char* perf_event_name(perf_event e) {
    switch (e) { case perf_event::cycles: return "cycles"; case perf_event::instructions: return "instructions"; case perf_event::cache_references: return "cache-references";
                 case perf_event::cache_misses: return "cache-misses"; default: return "branch-misses"; }
}
// Counts for the calling thread between start() and stop().
class perf_counters {
public:
    // scaled: the kernel multiplexed the counter, value is extrapolated from the fraction of time it ran.
    struct reading { perf_event event; std::uint64_t value = 0; bool valid = false, scaled = false; };
    perf_counters() = default;
    perf_counters(const perf_counters&) = delete; perf_counters& operator=(const perf_counters&) = delete;
    ~perf_counters() { close_(); }
    status open(const std::vector<perf_event>& evs) {
        close_(); evs_ = evs; backend_ = "none"; if (evs.empty()) return {{}, {22, "perf_counters: no events"}};
        bool cycles_only = evs.size() == 1 && evs[0] == perf_event::cycles;
#if defined(__linux__)
        std::vector<std::uint64_t> types;   // Intel hybrid: one group per core PMU, selected by the extended type (5.13+)
        for (const char* pmu : {"cpu_core", "cpu_atom"}) { auto t = detail::slurp_ll(std::string("/sys/bus/event_source/devices/") + pmu + "/type", -1); if (t > 0) types.push_back(std::uint64_t(t)); }
        if (types.size() < 2) types = {0};
        error er;
        for (;;) {
            if (!(er = open_groups_(types))) { backend_ = types[0] ? "perf_event (hybrid)" : "perf_event"; return {}; }
            close_(); if (!types[0]) break; types = {0};   // kernel without extended types: one legacy group
        }
        if (cycles_only) { backend_ = "cycle_counter"; return {}; }   // perf_event locked down: TSC/CNTVCT
        return {{}, er};
#elif defined(FORGE_OS_MACOS)
        for (auto e : evs) if (e != perf_event::cycles && e != perf_event::instructions) return {{}, unsupported(std::string("macOS perf event ") + perf_event_name(e))};
        if ((kp_ = ::dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY))) {
            auto set_counting = reinterpret_cast<int (*)(std::uint32_t)>(::dlsym(kp_, "kpc_set_counting"));
            auto set_thread = reinterpret_cast<int (*)(std::uint32_t)>(::dlsym(kp_, "kpc_set_thread_counting"));
            get_thread_ = reinterpret_cast<int (*)(int, unsigned, void*)>(::dlsym(kp_, "kpc_get_thread_counters"));
            if (set_counting && set_thread && get_thread_ && !set_counting(1) && !set_thread(1)) { backend_ = "kperf"; return {}; }   // root only
        }
        if (auto r = rusage_(); r.size() == 2 && (r[0] || r[1])) { backend_ = "proc_pid_rusage"; return {}; }
        if (cycles_only) { backend_ = "cycle_counter"; return {}; }
        return {{}, {1, "perf counters: kpc needs root and proc_pid_rusage reports none"}};
#elif defined(FORGE_OS_WINDOWS)
        for (auto e : evs) if (e != perf_event::cycles) return {{}, unsupported(std::string("windows perf event ") + perf_event_name(e))};
        { ULONG64 c = 0; backend_ = QueryThreadCycleTime(GetCurrentThread(), &c) && c ? "QueryThreadCycleTime" : "cycle_counter"; }   // wine reports 0
        (void)cycles_only; return {};
#elif defined(FORGE_OS_FREEBSD)
        if (auto er = pmc_open_(); !er) { backend_ = "libpmc"; return {}; }
        else if (!cycles_only) return {{}, er};
        backend_ = "cycle_counter"; return {};
#else
        if (!cycles_only) for (auto e : evs) if (e != perf_event::cycles) return {{}, unsupported(std::string("perf event ") + perf_event_name(e))};
        backend_ = "cycle_counter"; return {};
#endif
    }
    const char* backend() const { return backend_; }
    void start() {
#if defined(__linux__)
        if (!groups_.empty()) { for (auto& g : groups_) { ::ioctl(g[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP); ::ioctl(g[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP); } return; }
#endif
        base_ = snapshot_();
    }
    std::vector<reading> stop() {
        std::vector<reading> r; for (auto e : evs_) r.push_back({e, 0, false, false});
#if defined(__linux__)
        if (!groups_.empty()) {
            for (auto& g : groups_) ::ioctl(g[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
            for (auto& g : groups_) {   // PERF_FORMAT_GROUP | TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING: nr, enabled, running, values[nr]
                std::vector<std::uint64_t> buf(3 + g.size());
                if (::read(g[0], buf.data(), buf.size() * 8) <= 0) continue;
                std::uint64_t en = buf[1], run = buf[2];
                for (std::size_t i = 0; i < r.size() && i < buf[0]; ++i) {
                    std::uint64_t v = buf[3 + i];
                    if (run && run < en) { v = std::uint64_t(static_cast<long double>(v) * static_cast<long double>(en) / static_cast<long double>(run)); r[i].scaled = true; }
                    r[i].value += v; r[i].valid = true;   // run == 0: this PMU never counted us (thread stayed on the other core type)
                }
            }
            return r;
        }
        if (std::string(backend_) != "cycle_counter") return r;
#endif
        {
        auto now = snapshot_();
        bool by_pos = std::string(backend_) == "libpmc";   // libpmc snapshots follow evs_; the others are {cycles, instructions}
        for (std::size_t i = 0; i < r.size(); ++i) {
            std::size_t k = by_pos ? i : r[i].event == perf_event::instructions ? 1 : 0;
            if (k < now.size() && k < base_.size()) { r[i].value = now[k] - base_[k]; r[i].valid = true; }
        }
        }
        return r;
    }
private:
#if defined(__linux__)
    error open_groups_(const std::vector<std::uint64_t>& types) {
        for (auto ty : types) {
            std::vector<int> g;
            for (auto e : evs_) {
                perf_event_attr a{}; a.size = sizeof a; a.type = PERF_TYPE_HARDWARE; a.disabled = g.empty() ? 1 : 0; a.exclude_kernel = 1; a.exclude_hv = 1;
                a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
                a.config = (e == perf_event::cycles ? PERF_COUNT_HW_CPU_CYCLES : e == perf_event::instructions ? PERF_COUNT_HW_INSTRUCTIONS : e == perf_event::cache_references ? PERF_COUNT_HW_CACHE_REFERENCES
                         : e == perf_event::cache_misses ? PERF_COUNT_HW_CACHE_MISSES : PERF_COUNT_HW_BRANCH_MISSES) | (ty << 32);
                int fd = int(::syscall(SYS_perf_event_open, &a, 0, -1, g.empty() ? -1 : g[0], 8UL /*PERF_FLAG_FD_CLOEXEC*/));
                if (fd < 0) {
                    auto er = last_error(std::string("perf_event_open ") + perf_event_name(e) + " (perf_event_paranoid=" + detail::trim(detail::slurp("/proc/sys/kernel/perf_event_paranoid")) + ")");
                    for (int f : g) ::close(f);
                    return er;
                }
                g.push_back(fd);
            }
            groups_.push_back(std::move(g));
        }
        return {};
    }
#endif
#if defined(FORGE_OS_MACOS)
    static std::vector<std::uint64_t> rusage_() {
        rusage_info_v4 ri{};
        if (::proc_pid_rusage(::getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri))) return {};
        return {ri.ri_cycles, ri.ri_instructions};
    }
#endif
#if defined(FORGE_OS_FREEBSD)
    // libpmc, late-bound: process-virtual counting PMCs (PMC_MODE_TC) attached to ourselves.
    struct pmc_api {
        int (*init)() = nullptr; int (*alloc)(const char*, int, std::uint32_t, int, std::uint32_t*, std::uint64_t) = nullptr;
        int (*attach)(std::uint32_t, pid_t) = nullptr; int (*start)(std::uint32_t) = nullptr; int (*stop)(std::uint32_t) = nullptr;
        int (*read)(std::uint32_t, std::uint64_t*) = nullptr; int (*release)(std::uint32_t) = nullptr; bool ok = false;
        static const pmc_api& get() {
            static const pmc_api a = [] {
                pmc_api p; void* h = ::dlopen("libpmc.so.5", RTLD_LAZY | RTLD_LOCAL); if (!h) h = ::dlopen("libpmc.so", RTLD_LAZY | RTLD_LOCAL); if (!h) return p;
                auto f = [&](auto& fp, const char* n) { fp = reinterpret_cast<std::remove_reference_t<decltype(fp)>>(::dlsym(h, n)); };
                f(p.init, "pmc_init"); f(p.alloc, "pmc_allocate"); f(p.attach, "pmc_attach"); f(p.start, "pmc_start"); f(p.stop, "pmc_stop"); f(p.read, "pmc_read"); f(p.release, "pmc_release");
                p.ok = p.init && p.alloc && p.attach && p.start && p.stop && p.read && p.release && p.init() == 0;   // fails without hwpmc.ko
                return p;
            }();
            return a;
        }
    };
    error pmc_open_() {
        auto& p = pmc_api::get(); if (!p.ok) return unsupported("libpmc (hwpmc module not loaded)");
        for (auto e : evs_) {
            const char* spec = e == perf_event::cycles ? "unhalted-cycles" : e == perf_event::instructions ? "instructions" : e == perf_event::branch_misses ? "branch-mispredicts"
                             : e == perf_event::cache_misses ? "dc-misses" : nullptr;
            if (!spec) { close_(); return unsupported(std::string("libpmc event ") + perf_event_name(e)); }
            std::uint32_t id = 0;
            if (p.alloc(spec, 3 /*PMC_MODE_TC*/, 0, -1 /*PMC_CPU_ANY*/, &id, 0)) { auto er = last_error(std::string("pmc_allocate ") + spec); close_(); return er; }
            pmcs_.push_back(id);
            if (p.attach(id, 0) || p.start(id)) { auto er = last_error(std::string("pmc_attach/start ") + spec); close_(); return er; }
        }
        return {};
    }
#endif
    void close_() {
#if defined(__linux__)
        for (auto& g : groups_) for (int f : g) ::close(f);
        groups_.clear();
#endif
#if defined(FORGE_OS_FREEBSD)
        if (!pmcs_.empty()) { auto& p = pmc_api::get(); for (auto id : pmcs_) { p.stop(id); p.release(id); } pmcs_.clear(); }
#endif
    }
    std::vector<std::uint64_t> snapshot_() {
#if defined(FORGE_OS_MACOS)
        if (std::string(backend_) == "proc_pid_rusage") return rusage_();
        if (std::string(backend_) == "cycle_counter") return {cycle_counter()};
        std::uint64_t c[32] = {}; if (get_thread_ && get_thread_(0, 32, c) == 0) return {c[0], c[1]}; return {};
#elif defined(FORGE_OS_WINDOWS)
        if (std::string(backend_) == "cycle_counter") return {cycle_counter()};
        ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return {c};
#elif defined(FORGE_OS_FREEBSD)
        if (pmcs_.empty()) return {cycle_counter()};
        std::vector<std::uint64_t> v; auto& p = pmc_api::get();
        for (auto id : pmcs_) { std::uint64_t x = 0; if (p.read(id, &x)) return {}; v.push_back(x); }
        return v;
#else
        return {cycle_counter()};
#endif
    }
    std::vector<std::uint64_t> base_;
#if defined(__linux__)
    std::vector<std::vector<int>> groups_;
#endif
#if defined(FORGE_OS_MACOS)
    void* kp_ = nullptr; int (*get_thread_)(int, unsigned, void*) = nullptr;
#endif
#if defined(FORGE_OS_FREEBSD)
    std::vector<std::uint32_t> pmcs_;
#endif
    std::vector<perf_event> evs_; const char* backend_ = "none";
};
} // namespace forge::os
