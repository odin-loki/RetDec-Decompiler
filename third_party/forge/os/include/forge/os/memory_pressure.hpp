#pragma once
// Memory-pressure notifications, one subscription API:
//   Linux: PSI triggers, on the process's own cgroup v2 memory.pressure when it has one (containers: the limit
//   that matters), else /proc/pressure/memory (poll POLLPRI); Android: the same where readable, plus the
//   onTrimMemory hook (call memory_pressure::on_trim_memory(level) from ComponentCallbacks2 via JNI);
//   iOS/macOS: DISPATCH_SOURCE_TYPE_MEMORYPRESSURE (also what UIApplicationDidReceiveMemoryWarning is built on;
//   app code may forward warnings with notify());  Windows: CreateMemoryResourceNotification;
//   everywhere else (FreeBSD, PSI-less kernels): a 1s poll of available memory.
// stop() tears the monitor down completely (thread joined, wake pipe / event closed, dispatch source cancelled
// and released); the next subscribe() starts a fresh one.
#include <forge/os/detail.hpp>
#include <forge/os/sysinfo.hpp>
#include <atomic>
#include <condition_variable>

namespace forge::os {
enum class pressure_level { normal = 0, moderate = 1, critical = 2 };
inline const char* pressure_name(pressure_level l) { return l == pressure_level::critical ? "critical" : l == pressure_level::moderate ? "moderate" : "normal"; }

class memory_pressure {
public:
    using callback = std::function<void(pressure_level)>;
    static memory_pressure& instance() { static memory_pressure m; return m; }
    // Subscribe; the monitor starts on first subscription. Callbacks run on the monitor thread (or the hook's caller).
    int subscribe(callback cb) {
        std::lock_guard<std::mutex> g(m_); int id = ++next_; subs_[id] = std::move(cb); if (!running_) start_locked_(); return id;
    }
    void unsubscribe(int id) { std::lock_guard<std::mutex> g(m_); subs_.erase(id); }
    // Deliver a level from anywhere (tests, platform glue, iOS app delegate).
    void notify(pressure_level l) {
        std::vector<callback> cbs; { std::lock_guard<std::mutex> g(m_); last_ = l; ++events_; for (auto& kv : subs_) cbs.push_back(kv.second); }
        for (auto& c : cbs) c(l);
    }
    // Android ComponentCallbacks2.onTrimMemory levels: RUNNING_MODERATE=5, RUNNING_LOW=10, RUNNING_CRITICAL=15,
    // UI_HIDDEN=20, BACKGROUND=40, MODERATE=60, COMPLETE=80.
    static void on_trim_memory(int level) {
        instance().notify(level >= 60 || level == 15 ? pressure_level::critical : level >= 5 && level != 20 ? pressure_level::moderate : pressure_level::normal);
    }
    pressure_level last() const { std::lock_guard<std::mutex> g(m_); return last_; }
    std::uint64_t events() const { std::lock_guard<std::mutex> g(m_); return events_; }
    std::string source() const { std::lock_guard<std::mutex> g(m_); return source_; }
    // True while a monitor is live: the thread is in its wait loop, or the dispatch source is armed.
    bool monitoring() const { return live_.load(); }
    // Current level by sampling (independent of the monitor).
    static pressure_level sample() {
        std::uint64_t tot = total_memory(), av = available_memory(); if (!tot) return pressure_level::normal;
        double f = double(av) / double(tot); return f < 0.05 ? pressure_level::critical : f < 0.10 ? pressure_level::moderate : pressure_level::normal;
    }
    void stop() {
        { std::lock_guard<std::mutex> g(m_); if (!running_) return; stop_ = true; }
#if defined(__APPLE__)
        stop_dispatch_();
#endif
#if defined(FORGE_OS_WINDOWS)
        if (stop_ev_) SetEvent(stop_ev_);
#elif defined(FORGE_OS_POSIX)
        if (wake_[1] >= 0) { char c = 1; (void)!::write(wake_[1], &c, 1); }
#endif
        if (th_.joinable()) th_.join();
#if defined(FORGE_OS_WINDOWS)
        if (stop_ev_) { CloseHandle(stop_ev_); stop_ev_ = nullptr; }
#elif defined(FORGE_OS_POSIX)
        for (int& f : wake_) if (f >= 0) { ::close(f); f = -1; }   // the wake byte goes with it: a restart gets a clean pipe
#endif
        std::lock_guard<std::mutex> g(m_); running_ = false; stop_ = false; live_ = false;
    }
    ~memory_pressure() { stop(); }
private:
    memory_pressure() = default;
    void start_locked_() {
        running_ = true;
#if defined(__APPLE__)
        if (start_dispatch_()) { live_ = true; return; }
#endif
#if defined(FORGE_OS_WINDOWS)
        stop_ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#elif defined(FORGE_OS_POSIX)
        if (::pipe(wake_)) wake_[0] = wake_[1] = -1;
        else for (int f : wake_) ::fcntl(f, F_SETFD, FD_CLOEXEC);
#endif
        th_ = std::thread([this] { live_ = true; run_(); live_ = false; });
    }
    bool stopping_() { std::lock_guard<std::mutex> g(m_); return stop_; }
    void set_source_(const char* s) { std::lock_guard<std::mutex> g(m_); source_ = s; }
#if defined(__linux__)
    // PSI trigger fds (some, full) on the cgroup's memory.pressure (cgroup v2), else the system-wide file.
    static bool psi_open_(int& some, int& full, const char*& src) {
        std::string cg = detail::cgroup2_dir(); std::vector<std::pair<std::string, const char*>> files;
        if (!cg.empty() && cg != "/sys/fs/cgroup") files.push_back({cg + "/memory.pressure", "psi-cgroup"});
        files.push_back({"/proc/pressure/memory", "psi"});
        for (auto& [f, s] : files) {
            some = ::open(f.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC); full = ::open(f.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            // Unprivileged PSI triggers need a 2s-multiple window.
            if (some >= 0 && full >= 0 && ::write(some, "some 150000 2000000", 20) > 0 && ::write(full, "full 100000 2000000", 20) > 0) { src = s; return true; }
            if (some >= 0) ::close(some); if (full >= 0) ::close(full);
            some = full = -1;
        }
        return false;
    }
#endif
    void run_() {
#if defined(__linux__)
        int some = -1, full = -1; const char* src = "psi";
        if (psi_open_(some, full, src)) {
            set_source_(src);
            pollfd f[3] = {{some, POLLPRI, 0}, {full, POLLPRI, 0}, {wake_[0], POLLIN, 0}};
            while (!stopping_()) {
                int n = ::poll(f, 3, 2000); if (n < 0 && errno != EINTR) break;
                if (f[2].revents) break;
                if (f[1].revents & POLLPRI) notify(pressure_level::critical);
                else if (f[0].revents & POLLPRI) notify(pressure_level::moderate);
                else if (n == 0 && last() != pressure_level::normal && sample() == pressure_level::normal) notify(pressure_level::normal);
                if ((f[0].revents | f[1].revents) & (POLLERR | POLLNVAL)) break;
            }
            ::close(some); ::close(full); return;
        }
#elif defined(FORGE_OS_WINDOWS)
        HANDLE lo = CreateMemoryResourceNotification(LowMemoryResourceNotification), hi = CreateMemoryResourceNotification(HighMemoryResourceNotification);
        if (lo && hi) {
            set_source_("memory_resource_notification");
            bool low = false;
            while (!stopping_()) {
                HANDLE hs[2] = {stop_ev_, lo};
                DWORD r = WaitForMultipleObjects(2, hs, FALSE, low ? 1000 : INFINITE);
                if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
                BOOL st = FALSE; QueryMemoryResourceNotification(lo, &st);
                if (st && !low) { low = true; notify(sample() == pressure_level::critical ? pressure_level::critical : pressure_level::moderate); }
                BOOL h = FALSE; QueryMemoryResourceNotification(hi, &h);
                if (low && !st && h) { low = false; notify(pressure_level::normal); }
            }
            CloseHandle(lo); CloseHandle(hi); return;
        }
        if (lo) CloseHandle(lo); if (hi) CloseHandle(hi);
#endif
        set_source_("poll");
        pressure_level cur = pressure_level::normal;
        while (!stopping_()) {
            auto l = sample(); if (l != cur) { cur = l; notify(l); }
#if defined(FORGE_OS_WINDOWS)
            if (WaitForSingleObject(stop_ev_, 1000) != WAIT_TIMEOUT) break;
#else
            pollfd f{wake_[0], POLLIN, 0}; if (::poll(&f, 1, 1000) > 0) break;
#endif
        }
    }
#if defined(__APPLE__)
    // libdispatch, late-bound. stop() cancels the source and waits for its cancel handler, which libdispatch runs
    // only after any event handler in flight has returned; then releases it.
    bool start_dispatch_() {
        auto& d = disp_();
        if (!d.ok) return false;
        src_ = d.create(d.type, 0, 0x1 | 0x2 | 0x4 /*NORMAL|WARN|CRITICAL*/, d.gq(0, 0));
        if (!src_) return false;
        d.ctx(src_, this);
        d.seth(src_, [](void* c) {
            auto* self = static_cast<memory_pressure*>(c); in_handler_() = true;
            auto v = disp_().data(self->src_);
            self->notify(v & 0x4 ? pressure_level::critical : v & 0x2 ? pressure_level::moderate : pressure_level::normal);
            in_handler_() = false;
        });
        d.setc(src_, [](void* c) { auto* self = static_cast<memory_pressure*>(c); std::lock_guard<std::mutex> g(self->cm_); self->cancelled_ = true; self->cv_.notify_all(); });
        cancelled_ = false; d.res(src_); source_ = "dispatch_source"; return true;
    }
    void stop_dispatch_() {
        if (!src_) return;
        auto& d = disp_(); d.cancel(src_);
        if (!in_handler_()) { std::unique_lock<std::mutex> l(cm_); cv_.wait_for(l, std::chrono::seconds(5), [&] { return cancelled_; }); }
        d.release(src_); src_ = nullptr;
    }
    struct dispatch_api {
        void* (*create)(const void*, std::uintptr_t, std::uintptr_t, void*) = nullptr; const void* type = nullptr;
        void* (*gq)(long, std::uintptr_t) = nullptr; void (*seth)(void*, void (*)(void*)) = nullptr; void (*setc)(void*, void (*)(void*)) = nullptr;
        void (*ctx)(void*, void*) = nullptr; void (*res)(void*) = nullptr; void (*cancel)(void*) = nullptr; void (*release)(void*) = nullptr;
        std::uintptr_t (*data)(void*) = nullptr; bool ok = false;
    };
    static const dispatch_api& disp_() {
        static const dispatch_api a = [] {
            dispatch_api d;
            auto f = [](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(::dlsym(RTLD_DEFAULT, n)); };
            f(d.create, "dispatch_source_create"); d.type = ::dlsym(RTLD_DEFAULT, "_dispatch_source_type_memorypressure"); f(d.gq, "dispatch_get_global_queue");
            f(d.seth, "dispatch_source_set_event_handler_f"); f(d.setc, "dispatch_source_set_cancel_handler_f"); f(d.ctx, "dispatch_set_context");
            f(d.res, "dispatch_resume"); f(d.cancel, "dispatch_source_cancel"); f(d.release, "dispatch_release"); f(d.data, "dispatch_source_get_data");
            d.ok = d.create && d.type && d.gq && d.seth && d.setc && d.ctx && d.res && d.cancel && d.release && d.data;
            return d;
        }();
        return a;
    }
    static bool& in_handler_() { static thread_local bool b = false; return b; }
    void* src_ = nullptr; std::mutex cm_; std::condition_variable cv_; bool cancelled_ = false;
#endif
    mutable std::mutex m_; std::map<int, callback> subs_; int next_ = 0; bool running_ = false, stop_ = false;
    pressure_level last_ = pressure_level::normal; std::uint64_t events_ = 0; std::string source_ = "none"; std::thread th_;
    std::atomic<bool> live_{false};
#if defined(FORGE_OS_WINDOWS)
    HANDLE stop_ev_ = nullptr;
#elif defined(FORGE_OS_POSIX)
    int wake_[2] = {-1, -1};
#endif
};
} // namespace forge::os
