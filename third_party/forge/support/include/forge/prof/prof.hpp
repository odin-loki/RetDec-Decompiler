#pragma once
// Scoped profiling zones, recorded per thread and exported as Chrome trace-event JSON (chrome://tracing,
// Perfetto). Disabled zones cost one relaxed load.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace forge::prof {
struct event { const char* name; std::uint64_t t0_ns, dur_ns; std::uint32_t tid; };
struct recorder {
    std::atomic<bool> on{false}; std::mutex mu; std::vector<event> ev;
    static recorder& get() { static recorder r; return r; }
    static std::uint64_t now() { return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
    static std::uint32_t tid() { static std::atomic<std::uint32_t> n{0}; thread_local std::uint32_t t = n++; return t; }
    void add(const event& e) { std::lock_guard l(mu); ev.push_back(e); }
    std::string chrome_json() {
        std::lock_guard l(mu); std::string s = "{\"traceEvents\":[";
        char buf[256];
        for (std::size_t i = 0; i < ev.size(); ++i) {
            std::string nm; for (const char* p = ev[i].name; *p; ++p) { if (*p == '"' || *p == '\\') nm += '\\'; nm += *p; }
            std::snprintf(buf, sizeof buf, "%s{\"name\":\"%s\",\"ph\":\"X\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f}", i ? "," : "", nm.c_str(), ev[i].tid, double(ev[i].t0_ns) / 1e3, double(ev[i].dur_ns) / 1e3);
            s += buf;
        }
        return s + "],\"displayTimeUnit\":\"ns\"}";
    }
    void clear() { std::lock_guard l(mu); ev.clear(); }
};
struct zone {
    const char* name; std::uint64_t t0 = 0; bool live;
    explicit zone(const char* n) : name(n), live(recorder::get().on.load(std::memory_order_relaxed)) { if (live) t0 = recorder::now(); }
    ~zone() { if (live) recorder::get().add({name, t0, recorder::now() - t0, recorder::tid()}); }
};
}
#define FORGE_PROF_CAT2(a, b) a##b
#define FORGE_PROF_CAT(a, b) FORGE_PROF_CAT2(a, b)
#define FORGE_ZONE(name) ::forge::prof::zone FORGE_PROF_CAT(forge_zone_, __LINE__)(name)
