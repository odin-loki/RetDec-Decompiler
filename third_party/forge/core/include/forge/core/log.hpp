#pragma once
// Leveled, thread-safe logging with module tags and pluggable sinks.
//
//   forge::log::info("loaded {} tensors", n);              // module "forge"
//   forge::log::logger L{"inference"}; L.debug("kv {}", b);     // module "inference"
//
// Threshold: FORGE_LOG="warn" or "info,inference=debug,graph=trace" (first letter of each level counts:
// t[race] d[ebug] i[nfo] w[arn] e[rror] o[ff]; default warn). set_level()/set_module_level() override it.
// Compile-time strip: define FORGE_LOG_MIN (or FORGE_LOG_MIN_LEVEL) to 0..6 (trace..fatal, off) and calls
// below it compile to nothing. The FORGE_LOG_TRACE(...)..FORGE_LOG_FATAL(...) macros also skip evaluating
// their arguments when the level is stripped or disabled at run time; the functions always evaluate them.
// The CMake cache variable FORGE_LOG_MIN_LEVEL (trace..off, default none) defines it for every target
// linking forge_core.
// Sinks: stderr (default), append-to-file, syslog (POSIX), Windows event log, or any callback. Each record
// is formatted first and written as one line under a lock, so concurrent lines never interleave. Format:
// text (default) or JSON lines (FORGE_LOG_FORMAT=json or set_format()).
// Async: add_async_sink(make_file_sink(...)) wraps any sink in a bounded queue drained by a background
// thread. A full queue drops the line (counted, and reported to the sink as a "log" warning) instead of
// blocking the caller. flush() waits for every async sink; remove_sink and process exit drain and stop it.
// A sink must not log itself (the sink lock is held while it runs; an async sink would feed its own queue).
#include <forge/os/os.hpp>
#include <forge/os/time.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <syslog.h>
#endif

#if !defined(FORGE_LOG_MIN) && defined(FORGE_LOG_MIN_LEVEL)
#define FORGE_LOG_MIN FORGE_LOG_MIN_LEVEL
#endif
#ifndef FORGE_LOG_MIN
#define FORGE_LOG_MIN 0
#endif

namespace forge::log {
enum class level : int { trace, debug, info, warn, error, fatal, off };
enum class format { text, json };

struct record {
    level lvl;
    std::string_view module;
    std::string_view message;
    std::int64_t unix_ns;
    std::uint64_t thread;
};
using sink = std::function<void(const record&, std::string_view line)>;   // line = formatted, no newline

inline const char* level_name(level l) {
    static const char* n[] = {"trace", "debug", "info", "warn", "error", "fatal", "off"};
    return n[int(l) < 0 || int(l) > 6 ? 6 : int(l)];
}
inline level parse_level(std::string_view s, level dflt = level::warn) {
    if (s.empty()) return dflt;
    switch (s[0] | 0x20) {
        case 't': return level::trace; case 'd': return level::debug; case 'i': return level::info;
        case 'w': return level::warn; case 'e': return level::error; case 'f': return level::fatal; case 'o': return level::off;
        default: return dflt;
    }
}

namespace detail {
struct async_queue;
inline void async_stop(async_queue& q);
struct state {
    std::mutex m;                                          // guards everything below except the atomics
    std::atomic<int> global{int(level::warn)};
    std::atomic<int> floor{int(level::warn)};              // min over global and module overrides (fast reject)
    std::vector<std::pair<std::string, level>> modules;
    std::vector<std::pair<int, sink>> sinks;
    int next_id = 1;
    bool use_stderr = true;
    std::atomic<int> fmt{int(format::text)};
    std::vector<std::pair<int, std::shared_ptr<async_queue>>> asyncs;   // async sinks (their pushers are in `sinks`)
    state() {
        if (const char* e = std::getenv("FORGE_LOG")) parse_spec(e);
        if (const char* f = std::getenv("FORGE_LOG_FORMAT"); f && (f[0] | 0x20) == 'j') fmt = int(format::json);
    }
    ~state() {   // process exit: drain every async sink and join its thread
        std::vector<std::pair<int, std::shared_ptr<async_queue>>> a;
        { std::lock_guard g(m); a.swap(asyncs); sinks.clear(); }
        for (auto& p : a) async_stop(*p.second);
    }
    void recompute() {
        int f = global.load();
        for (auto& [_, l] : modules) f = std::min(f, int(l));
        floor.store(f);
    }
    void parse_spec(std::string_view s) {
        while (!s.empty()) {
            auto c = s.find(','); auto part = s.substr(0, c);
            s = c == std::string_view::npos ? std::string_view{} : s.substr(c + 1);
            auto eq = part.find('=');
            if (eq == std::string_view::npos) global = int(parse_level(part));
            else modules.emplace_back(std::string(part.substr(0, eq)), parse_level(part.substr(eq + 1)));
        }
        recompute();
    }
    level threshold_for(std::string_view mod) {
        std::lock_guard l(m);
        for (auto& [n, lv] : modules) if (n == mod) return lv;
        return level(global.load());
    }
};
inline state& st() { static state s; return s; }

inline void json_escape(std::string& o, std::string_view s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default: if (c < 0x20) o += std::format("\\u{:04x}", c); else o += char(c);
        }
    }
}
inline std::string format_line(const record& r, format f) {
    static const char tag[] = {'T', 'D', 'I', 'W', 'E', 'F', 'O'};
    std::string o;
    if (f == format::json) {
        o = std::format("{{\"ts\":\"{}\",\"level\":\"{}\",\"module\":\"", os::format_iso8601(r.unix_ns), level_name(r.lvl));
        json_escape(o, r.module); o += "\",\"thread\":" + std::to_string(r.thread) + ",\"msg\":\"";
        json_escape(o, r.message); o += "\"}";
    } else {
        o = r.module == "forge" ? std::format("[forge {}] ", tag[int(r.lvl)]) : std::format("[forge:{} {}] ", r.module, tag[int(r.lvl)]);
        o += r.message;
    }
    return o;
}

// One async sink. Producers copy the record into `q` and never wait for space; the worker swaps the queue
// out and runs `inner` without holding the lock. `queued`/`done` count accepted lines so flush() waits only
// for what was queued before it was called.
struct async_queue {
    struct item { level lvl; std::string module, message, line; std::int64_t unix_ns; std::uint64_t thread; };
    sink inner;
    std::size_t capacity;
    const std::atomic<int>* fmt;   // format for the drop report
    std::mutex m;
    std::condition_variable work, idle;
    std::vector<item> q;
    std::uint64_t queued = 0, done = 0, dropped = 0, unreported = 0;
    bool stopping = false;
    os::thread worker;
    async_queue(sink k, std::size_t cap, const std::atomic<int>* f) : inner(std::move(k)), capacity(cap ? cap : 1), fmt(f) {}
    void push(const record& r, std::string_view line) {
        std::lock_guard g(m);
        if (stopping || q.size() >= capacity) { ++dropped; ++unreported; return; }
        q.push_back(item{r.lvl, std::string(r.module), std::string(r.message), std::string(line), r.unix_ns, r.thread});
        ++queued;
        if (q.size() == 1) work.notify_one();
    }
    void run() {
        std::vector<item> batch;
        std::unique_lock l(m);
        for (;;) {
            work.wait(l, [&] { return stopping || !q.empty() || unreported; });
            if (q.empty() && !unreported) break;   // stopping and drained
            batch.swap(q);
            std::uint64_t lost = std::exchange(unreported, 0);
            l.unlock();
            for (auto& it : batch) {
                record r{it.lvl, it.module, it.message, it.unix_ns, it.thread};
                try { inner(r, it.line); } catch (...) {}   // a throwing sink must not end the drain thread
            }
            if (lost) {
                std::string msg = std::format("dropped {} log line(s): async queue full (capacity {})", lost, capacity);
                record r{level::warn, "log", msg, os::unix_time_ns(), os::thread_id()};
                try { inner(r, format_line(r, format(fmt->load()))); } catch (...) {}
            }
            std::size_t n = batch.size(); batch.clear();
            l.lock();
            done += n;
            idle.notify_all();
        }
    }
    void wait_until(std::uint64_t target) { std::unique_lock l(m); idle.wait(l, [&] { return done >= target; }); }
};
inline void async_stop(async_queue& q) {
    { std::lock_guard g(q.m); q.stopping = true; q.work.notify_one(); }
    q.worker.join();   // the worker writes out everything queued before it exits
}
} // namespace detail

// ---- configuration --------------------------------------------------------------------------------
inline void set_level(level l) { auto& s = detail::st(); std::lock_guard g(s.m); s.global = int(l); s.recompute(); }
inline void set_module_level(std::string_view mod, level l) {
    auto& s = detail::st(); std::lock_guard g(s.m);
    for (auto& [n, lv] : s.modules) if (n == mod) { lv = l; s.recompute(); return; }
    s.modules.emplace_back(std::string(mod), l); s.recompute();
}
inline void configure(std::string_view spec) {   // same syntax as FORGE_LOG; resets module overrides
    auto& s = detail::st(); std::lock_guard g(s.m); s.modules.clear(); s.parse_spec(spec);
}
inline level threshold(std::string_view mod = "forge") { return detail::st().threshold_for(mod); }
inline void set_format(format f) { detail::st().fmt = int(f); }
inline bool enabled(level l, std::string_view mod = "forge") {
    if (int(l) < FORGE_LOG_MIN || l == level::off) return false;
    auto& s = detail::st();
    if (int(l) < s.floor.load(std::memory_order_relaxed)) return false;
    return int(l) >= int(s.threshold_for(mod));
}

// Sinks return an id for remove_sink. The built-in stderr sink stays on unless set_stderr(false).
inline int add_sink(sink k) { auto& s = detail::st(); std::lock_guard g(s.m); int id = s.next_id++; s.sinks.emplace_back(id, std::move(k)); return id; }
// Removing an async sink writes out its queue and joins its thread before returning.
inline void remove_sink(int id) {
    auto& s = detail::st(); std::shared_ptr<detail::async_queue> q;
    {
        std::lock_guard g(s.m); std::erase_if(s.sinks, [&](auto& p) { return p.first == id; });
        for (auto it = s.asyncs.begin(); it != s.asyncs.end(); ++it)
            if (it->first == id) { q = std::move(it->second); s.asyncs.erase(it); break; }
    }
    if (q) detail::async_stop(*q);
}
inline void set_stderr(bool on) { auto& s = detail::st(); std::lock_guard g(s.m); s.use_stderr = on; }
// make_*_sink return a sink for add_sink or add_async_sink (an empty one on failure); add_*_sink register
// it directly and return its id (0 on failure).
inline sink make_stderr_sink() {
    return [](const record&, std::string_view line) {
        std::string nl = std::string(line) + '\n'; std::fwrite(nl.data(), 1, nl.size(), stderr); std::fflush(stderr);
    };
}
// Appends each line to `path` (created if missing) and flushes it. With max_bytes > 0 the file is rotated
// when it would grow past that size: path -> path.1 -> ... -> path.<keep> (the oldest is dropped).
inline sink make_file_sink(const std::string& path, std::uint64_t max_bytes = 0, unsigned keep = 3) {
    struct file_state { std::FILE* f = nullptr; std::uint64_t size = 0; ~file_state() { if (f) std::fclose(f); } };
    auto st = std::make_shared<file_state>();
    st->f = std::fopen(path.c_str(), "ab");
    if (!st->f) return {};
    std::fseek(st->f, 0, SEEK_END); st->size = std::uint64_t(std::max(0L, std::ftell(st->f)));
    return [st, path, max_bytes, keep](const record&, std::string_view line) {
        if (max_bytes && st->size && st->size + line.size() + 1 > max_bytes) {
            std::fclose(st->f); st->f = nullptr;
            if (keep == 0) std::remove(path.c_str());
            for (unsigned i = keep; i >= 1; --i) {
                std::string from = i == 1 ? path : path + "." + std::to_string(i - 1), to = path + "." + std::to_string(i);
                (void)os::rename(from, to);
            }
            st->f = std::fopen(path.c_str(), "ab"); st->size = 0;
            if (!st->f) return;
        }
        if (!st->f) return;
        std::fwrite(line.data(), 1, line.size(), st->f); std::fputc('\n', st->f); std::fflush(st->f);
        st->size += line.size() + 1;
    };
}
inline int add_file_sink(const std::string& path, std::uint64_t max_bytes = 0, unsigned keep = 3) {
    auto k = make_file_sink(path, max_bytes, keep);
    return k ? add_sink(std::move(k)) : 0;
}
#if defined(__unix__) || defined(__APPLE__)
// Sends each record to syslog(3) (journald picks it up on systemd hosts) with the matching priority.
inline sink make_syslog_sink(const char* ident = "forge") {
    ::openlog(ident, LOG_PID, LOG_USER);
    return [](const record& r, std::string_view line) {
        static const int pri[] = {LOG_DEBUG, LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR, LOG_CRIT, LOG_DEBUG};
        ::syslog(pri[int(r.lvl)], "%.*s", int(line.size()), line.data());
    };
}
inline int add_syslog_sink(const char* ident = "forge") { return add_sink(make_syslog_sink(ident)); }
#endif
#ifdef _WIN32
// Reports each record to the Windows event log (Application log, event source `source`) with ReportEventW:
// error/fatal -> EVENTLOG_ERROR_TYPE, warn -> EVENTLOG_WARNING_TYPE, others -> EVENTLOG_INFORMATION_TYPE.
// Event id 0, no category; the formatted line is the only insertion string. Without a message file
// registered for the source, Event Viewer shows the line after a "description ... cannot be found" note.
inline sink make_eventlog_sink(const char* source = "forge") {
    HANDLE h = ::RegisterEventSourceW(nullptr, os::widen(source).c_str());
    if (!h) return {};
    struct source_handle { HANDLE h; ~source_handle() { ::DeregisterEventSource(h); } };
    auto src = std::make_shared<source_handle>(h);
    return [src](const record& r, std::string_view line) {
        WORD type = r.lvl >= level::error ? WORD(EVENTLOG_ERROR_TYPE)
                  : r.lvl == level::warn  ? WORD(EVENTLOG_WARNING_TYPE) : WORD(EVENTLOG_INFORMATION_TYPE);
        std::wstring w = os::widen(line);
        LPCWSTR strings[] = {w.c_str()};
        ::ReportEventW(src->h, type, 0, 0, nullptr, 1, 0, strings, nullptr);
    };
}
inline int add_eventlog_sink(const char* source = "forge") {
    auto k = make_eventlog_sink(source);
    return k ? add_sink(std::move(k)) : 0;
}
#endif

// Wraps `inner` in a bounded queue of at most `capacity` pending lines, drained by a background thread
// named "forge-log". Logging threads only copy the record and never wait: when the queue is full the line
// is dropped and counted, and the worker later reports "dropped N log line(s)" to `inner` as a warn record
// of module "log". `inner` runs on that one thread, one line at a time, outside the logger lock.
// Returns the sink id (0 if `inner` is empty). If the thread cannot be started the sink is synchronous.
inline int add_async_sink(sink inner, std::size_t capacity = 8192) {
    if (!inner) return 0;
    auto& s = detail::st();
    auto q = std::make_shared<detail::async_queue>(std::move(inner), capacity, &s.fmt);
    q->worker = os::thread([raw = q.get()] { raw->run(); }, "forge-log");
    if (!q->worker.joinable()) return add_sink(std::move(q->inner));
    std::lock_guard g(s.m);
    int id = s.next_id++;
    s.sinks.emplace_back(id, [q](const record& r, std::string_view line) { q->push(r, line); });
    s.asyncs.emplace_back(id, q);
    return id;
}
// Counters of one async sink (all zero for an unknown id): lines written, lines dropped, lines pending.
struct async_stats { std::uint64_t written = 0, dropped = 0, pending = 0; };
inline async_stats async_sink_stats(int id) {
    std::shared_ptr<detail::async_queue> q;
    { auto& s = detail::st(); std::lock_guard g(s.m); for (auto& [i, a] : s.asyncs) if (i == id) q = a; }
    if (!q) return {};
    std::lock_guard g(q->m);
    return {q->done, q->dropped, q->queued - q->done};
}
// Blocks until every line queued to an async sink before the call has been written. Synchronous sinks
// write before the log call returns and need no flush. Must not be called from inside a sink.
inline void flush() {
    std::vector<std::pair<std::shared_ptr<detail::async_queue>, std::uint64_t>> w;
    {
        auto& s = detail::st(); std::lock_guard g(s.m);   // emit holds s.m too, so no line is half-queued
        for (auto& [_, q] : s.asyncs) { std::lock_guard qg(q->m); w.emplace_back(q, q->queued); }
    }
    for (auto& [q, target] : w) q->wait_until(target);
}

// ---- emitting -------------------------------------------------------------------------------------
inline void emit(level l, std::string_view mod, std::string_view msg) {
    auto& s = detail::st();
    record r{l, mod, msg, os::unix_time_ns(), os::thread_id()};
    std::string line = detail::format_line(r, format(s.fmt.load()));
    std::lock_guard g(s.m);   // one writer at a time: lines never interleave
    if (s.use_stderr) { std::string nl = line + '\n'; std::fwrite(nl.data(), 1, nl.size(), stderr); std::fflush(stderr); }
    for (auto& [_, k] : s.sinks) k(r, line);
}
template <class... A> void write_module(level l, std::string_view mod, std::format_string<A...> f, A&&... a) {
    if (!enabled(l, mod)) return;
    emit(l, mod, std::format(f, std::forward<A>(a)...));
}
template <class... A> void write(level l, std::format_string<A...> f, A&&... a) { write_module(l, "forge", f, std::forward<A>(a)...); }

#define FORGE_LOG_FN_(name, L)                                                                           \
    template <class... A> void name(std::format_string<A...> f, A&&... a) {                              \
        if constexpr (int(L) >= FORGE_LOG_MIN) write_module(L, "forge", f, std::forward<A>(a)...); \
    }
FORGE_LOG_FN_(trace, level::trace)
FORGE_LOG_FN_(debug, level::debug)
FORGE_LOG_FN_(info, level::info)
FORGE_LOG_FN_(warn, level::warn)
FORGE_LOG_FN_(error, level::error)
FORGE_LOG_FN_(fatal, level::fatal)
#undef FORGE_LOG_FN_

// A module-tagged logger: logger L{"inference"}; L.info("..."). Cheap to copy; holds only the tag.
struct logger {
    std::string_view module = "forge";
#define FORGE_LOG_M_(name, L)                                                                            \
    template <class... A> void name(std::format_string<A...> f, A&&... a) const {                        \
        if constexpr (int(L) >= FORGE_LOG_MIN) write_module(L, module, f, std::forward<A>(a)...);               \
    }
    FORGE_LOG_M_(trace, level::trace)
    FORGE_LOG_M_(debug, level::debug)
    FORGE_LOG_M_(info, level::info)
    FORGE_LOG_M_(warn, level::warn)
    FORGE_LOG_M_(error, level::error)
    FORGE_LOG_M_(fatal, level::fatal)
#undef FORGE_LOG_M_
    bool enabled(level l) const { return log::enabled(l, module); }
};
} // namespace forge::log

// Macros that skip argument evaluation when the level is stripped or disabled:
//   FORGE_LOG_DEBUG("inference", "kv {}", expensive());
#define FORGE_LOG_AT_(L, mod, ...)                                                                        \
    do { if constexpr (int(L) >= FORGE_LOG_MIN) { if (::forge::log::enabled(L, mod)) ::forge::log::write_module(L, mod, __VA_ARGS__); } } while (0)
#define FORGE_LOG_TRACE(mod, ...) FORGE_LOG_AT_(::forge::log::level::trace, mod, __VA_ARGS__)
#define FORGE_LOG_DEBUG(mod, ...) FORGE_LOG_AT_(::forge::log::level::debug, mod, __VA_ARGS__)
#define FORGE_LOG_INFO(mod, ...) FORGE_LOG_AT_(::forge::log::level::info, mod, __VA_ARGS__)
#define FORGE_LOG_WARN(mod, ...) FORGE_LOG_AT_(::forge::log::level::warn, mod, __VA_ARGS__)
#define FORGE_LOG_ERROR(mod, ...) FORGE_LOG_AT_(::forge::log::level::error, mod, __VA_ARGS__)
#define FORGE_LOG_FATAL(mod, ...) FORGE_LOG_AT_(::forge::log::level::fatal, mod, __VA_ARGS__)
