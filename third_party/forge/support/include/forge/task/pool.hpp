#pragma once
// task: a small shared work pool (one scheduler for every layer; no oversubscription).
//   parallel_for(n, grain, f)          data-parallel loop; the caller participates. Threads claim ranges of
//                                      max(grain, remaining / (2 * threads)) items, so early claims are long
//                                      (streaming) and the last ones short (no thread waits on a slow one).
//   parallel_for(n, lo, hi, f)         the same with claims clamped to [lo, hi] items.
//   submit(f, {priority, stop_token})  queued task -> std::future. Higher priority runs first, FIFO within a
//                                      priority. A task whose stop_token is triggered before it starts is
//                                      skipped and its future throws task::cancelled; a running task sees the
//                                      token if f takes a std::stop_token. parallel_for jobs take precedence.
// Dispatch is lock-free while workers are awake: a parallel_for publishes its job and bumps a ticket the
// workers spin on (FORGE_SPIN_US, default 200 us, after their last job), and only threads that have gone to
// sleep are woken through the mutex and condition variable. A parallel_for called from inside another one
// (or from a task running on a worker) runs inline on the calling thread.
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__linux__)
#include <cstdio>
#include <sched.h>
#endif

namespace forge::task {

struct cancelled : std::runtime_error { cancelled() : std::runtime_error("task cancelled before it started") {} };
struct options { int priority = 0; std::stop_token stop{}; };

namespace detail {
inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#elif (defined(__aarch64__) || defined(__arm__)) && defined(__GNUC__)
    __asm__ volatile("yield");
#endif
}
// Physical cores this process may run on (SMT siblings counted once); 0 if unknown.
inline unsigned physical_cores() {
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) return 0;
    std::set<std::pair<long, long>> cores;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &set)) continue;
        auto rd = [&](const char* f) -> long {
            char path[128]; std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/%s", c, f);
            std::FILE* fp = std::fopen(path, "r"); if (!fp) return -1;
            long v = -1; if (std::fscanf(fp, "%ld", &v) != 1) v = -1; std::fclose(fp); return v;
        };
        long core = rd("core_id"), pkg = rd("physical_package_id");
        if (core < 0) return unsigned(CPU_COUNT(&set));   // no topology: every allowed CPU counts
        cores.insert({pkg, core});
    }
    return unsigned(cores.size());
#else
    return 0;
#endif
}
} // namespace detail

class pool {
public:
    // n = 0: FORGE_THREADS, else the physical cores this process may use (Linux), else hardware_concurrency().
    explicit pool(unsigned n = 0) {
        if (!n) if (const char* e = std::getenv("FORGE_THREADS")) n = unsigned(std::strtoul(e, nullptr, 10));   // cap for constrained hosts/emulators
        if (!n) n = detail::physical_cores();
        if (!n) n = std::max(1u, std::thread::hardware_concurrency());
        if (const char* e = std::getenv("FORGE_SPIN_US")) spin_us_ = std::strtol(e, nullptr, 10);
        size_ = n;
        for (unsigned i = 1; i < n; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~pool() {   // queued tasks still run (or are skipped if cancelled) before the workers exit
        if (workers_.empty()) { std::unique_lock l(m_); drain_inline_(l); }
        { std::lock_guard l(m_); stop_ = true; stop_flag_.store(true); }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    unsigned size() const { return size_; }

    // Calls f(begin, end) over [0, n) in claims of at least `grain` items. Caller participates.
    template <class F> void parallel_for(std::size_t n, std::size_t grain, F&& f) {
        parallel_for(n, grain, std::size_t(-1), std::forward<F>(f));
    }
    // Claims clamped to [lo, hi] items (the last one may be shorter).
    template <class F> void parallel_for(std::size_t n, std::size_t lo, std::size_t hi, F&& f) {
        if (n == 0) return;
        lo = std::max<std::size_t>(lo, 1); hi = std::max(hi, lo);
        auto* fp = std::addressof(f);
        using FP = decltype(fp);
        if (size_ == 1 || n <= lo || in_parallel_()) { f(std::size_t(0), n); return; }
        run_(n, lo, hi, [](void* c, std::size_t b, std::size_t e) { (**static_cast<FP*>(c))(b, e); }, &fp);
    }

    // Queue f; returns its future. With a single-thread pool (no workers) the task runs inline at submit,
    // after any higher-or-equal priority tasks already queued.
    template <class F> auto submit(F&& f, options o = {}) {
        using R = std::conditional_t<std::is_invocable_v<F&, std::stop_token>, std::invoke_result<F&, std::stop_token>, std::invoke_result<F&>>::type;
        auto pr = std::make_shared<std::promise<R>>();
        std::future<R> fut = pr->get_future();
        item it{o.priority, 0, [fn = std::make_shared<std::decay_t<F>>(std::forward<F>(f)), pr, st = o.stop]() {
            if (st.stop_requested()) { pr->set_exception(std::make_exception_ptr(cancelled{})); return; }
            try {
                if constexpr (std::is_void_v<R>) {
                    if constexpr (std::is_invocable_v<F&, std::stop_token>) (*fn)(st); else (*fn)();
                    pr->set_value();
                } else {
                    if constexpr (std::is_invocable_v<F&, std::stop_token>) pr->set_value((*fn)(st)); else pr->set_value((*fn)());
                }
            } catch (...) { pr->set_exception(std::current_exception()); }
        }};
        {
            std::unique_lock l(m_);
            it.seq = seq_++; tasks_.push(std::move(it)); ++queued_; queued_flag_.store(queued_, std::memory_order_release);
            if (workers_.empty()) { drain_inline_(l); return fut; }
        }
        cv_.notify_one();
        return fut;
    }
    std::size_t pending() const { std::lock_guard l(m_); return queued_; }
    // Block until every task queued so far has finished (or been skipped).
    void wait_idle() { std::unique_lock l(m_); idle_cv_.wait(l, [&] { return queued_ == 0 && running_tasks_ == 0; }); }

    static pool& global() { static pool p; return p; }

private:
    using call_t = void (*)(void*, std::size_t, std::size_t);
    // The pools this thread is inside (running a parallel_for, or one of their workers): a nested call runs inline.
    struct inside_ {
        static const pool** slots() { thread_local const pool* s[8] = {}; return s; }
        static int& count() { thread_local int n = 0; return n; }
        explicit inside_(const pool* p) { if (count() < 8) slots()[count()] = p; ++count(); }
        ~inside_() { --count(); }
    };
    bool in_parallel_() const {
        for (int i = 0, n = std::min(inside_::count(), 8); i < n; ++i) if (inside_::slots()[i] == this) return true;
        return false;
    }

    // Claim and run ranges of the published job until none are left.
    void work_() noexcept {
        const std::size_t n = n_, lo = lo_, hi = hi_, t2 = 2 * std::size_t(size_);
        for (;;) {
            std::size_t cur = next_.load(std::memory_order_relaxed);
            if (cur >= n) return;
            std::size_t c = std::clamp((n - cur) / t2, lo, hi);
            std::size_t b = next_.fetch_add(c, std::memory_order_relaxed);
            if (b >= n) return;
            try { call_(ctx_, b, std::min(n, b + c)); }
            catch (...) {
                std::lock_guard l(err_mu_);
                if (!err_) err_ = std::current_exception();
                next_.store(n, std::memory_order_relaxed);   // stop handing out work
            }
        }
    }
    void run_(std::size_t n, std::size_t lo, std::size_t hi, call_t call, void* ctx) {
        std::lock_guard run(run_mu_);   // one data-parallel job at a time; other callers queue here
        inside_ in(this);
        call_ = call; ctx_ = ctx; n_ = n; lo_ = lo; hi_ = hi; err_ = nullptr;
        next_.store(0, std::memory_order_relaxed);
        open_.store(true);
        ticket_.fetch_add(1);
        if (sleepers_.load() > 0) { { std::lock_guard l(m_); } cv_.notify_all(); }
        work_();
        open_.store(false);
        while (active_.load() != 0) detail::cpu_relax();
        if (err_) { auto e = err_; err_ = nullptr; std::rethrow_exception(e); }
    }
    void join_(std::uint64_t t) {
        active_.fetch_add(1);
        if (open_.load() && ticket_.load() >= t) work_();
        active_.fetch_sub(1);
    }
    // Spin until a new ticket (true) or until a task is queued / the pool stops / the spin time runs out (false).
    bool spin_(std::uint64_t& seen) {
        auto t0 = std::chrono::steady_clock::now();
        for (unsigned i = 1;; ++i) {
            std::uint64_t t = ticket_.load(std::memory_order_acquire);
            if (t != seen) { seen = t; return true; }
            if (queued_flag_.load(std::memory_order_relaxed) || stop_flag_.load(std::memory_order_relaxed)) return false;
            if ((i & 255) == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(spin_us_)) return false;
            detail::cpu_relax();
        }
    }
    void loop() {
        inside_ in(this);
        std::uint64_t seen = ticket_.load();
        for (;;) {
            if (spin_(seen)) { join_(seen); continue; }
            std::unique_lock l(m_);
            sleepers_.fetch_add(1);
            cv_.wait(l, [&] { return stop_ || ticket_.load() != seen || !tasks_.empty(); });
            sleepers_.fetch_sub(1);
            if (ticket_.load() != seen) continue;   // a parallel job: picked up by spin_ at the top
            if (!tasks_.empty()) {                  // a queued task
                auto t = tasks_.top().fn; tasks_.pop(); --queued_; queued_flag_.store(queued_, std::memory_order_release); ++running_tasks_;
                l.unlock(); t(); l.lock();
                if (--running_tasks_ == 0 && queued_ == 0) idle_cv_.notify_all();
                continue;
            }
            if (stop_) return;
        }
    }
    struct item {
        int priority; std::uint64_t seq; std::function<void()> fn;
        bool operator<(const item& o) const { return priority != o.priority ? priority < o.priority : seq > o.seq; }
    };
    void drain_inline_(std::unique_lock<std::mutex>& l) {
        while (!tasks_.empty()) { auto t = tasks_.top().fn; tasks_.pop(); --queued_; queued_flag_.store(queued_); l.unlock(); t(); l.lock(); }
        idle_cv_.notify_all();
    }
    // the published job (written by the caller before open_/ticket_)
    call_t call_ = nullptr; void* ctx_ = nullptr; std::size_t n_ = 0, lo_ = 1, hi_ = 1;
    std::exception_ptr err_; std::mutex err_mu_;
    std::mutex run_mu_;
    alignas(64) std::atomic<std::uint64_t> ticket_{0};
    alignas(64) std::atomic<std::size_t> next_{0};
    alignas(64) std::atomic<int> active_{0};
    alignas(64) std::atomic<bool> open_{false};
    alignas(64) std::atomic<int> sleepers_{0};
    std::atomic<std::size_t> queued_flag_{0};
    std::atomic<bool> stop_flag_{false};
    long spin_us_ = 200;
    std::priority_queue<item> tasks_;
    std::uint64_t seq_ = 0;
    std::size_t queued_ = 0, running_tasks_ = 0;
    std::condition_variable idle_cv_;
    std::vector<std::thread> workers_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    unsigned size_ = 1;
};

template <class F> void parallel_for(std::size_t n, std::size_t grain, F&& f) { pool::global().parallel_for(n, grain, std::forward<F>(f)); }
template <class F> void parallel_for(std::size_t n, std::size_t lo, std::size_t hi, F&& f) { pool::global().parallel_for(n, lo, hi, std::forward<F>(f)); }
template <class F> auto submit(F&& f, options o = {}) { return pool::global().submit(std::forward<F>(f), std::move(o)); }

} // namespace forge::task
