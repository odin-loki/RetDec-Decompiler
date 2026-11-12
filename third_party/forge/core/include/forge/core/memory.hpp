#pragma once
// Memory governor: one process-wide view of how much memory FORGE may use, who is using it, and what to drop
// when it runs short. See docs/memory.md.
//
//   budget      a named pool ("host", "cuda:0", ...) with a byte limit, soft/hard watermarks and per-tag usage.
//               reserve() returns an RAII lease; when a reservation would cross the limit the governor first
//               asks registered caches to shrink (lowest priority, least recently used first), then waits up
//               to a timeout, then fails with errc::out_of_memory instead of letting the kernel OOM-kill us.
//   cache       anything droppable (prefix KV sessions, decoded weights, pipeline caches) registers a size
//               probe and an evict(bytes) callback.
//   resource    a std::pmr::memory_resource that charges a budget, for containers that should be accounted.
//   spill_store a directory of numbered files for evicted blobs that are cheaper to reload than recompute.
//
// The host limit comes from, in order: governor::set_limit / --mem-limit, FORGE_MEM_LIMIT, then
// FORGE_MEM_FRACTION (default 0.85) of the detected limit = min(physical RAM, cgroup v2 memory.max,
// cgroup v1 memory.limit_in_bytes, RLIMIT_AS). Device budgets come from FORGE_MEM_DEVICE_LIMIT or set_limit.
#include <forge/core/error.hpp>
#include <forge/os/os.hpp>
#include <forge/os/private_files.hpp>
#include <forge/os/memory_pressure.hpp>
#include <forge/os/process.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace forge::mem {

// "512M", "4G", "1.5GiB", "800000", "75%" (of `whole`). Returns nullopt on junk.
inline std::optional<std::uint64_t> parse_size(std::string_view s, std::uint64_t whole = 0) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return std::nullopt;
    std::string t(s); char* end = nullptr; double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || v < 0) return std::nullopt;
    std::string u(end); while (!u.empty() && u.front() == ' ') u.erase(0, 1);
    for (auto& c : u) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (u == "%") { if (!whole) return std::nullopt; return std::uint64_t(double(whole) * v / 100.0); }
    if (u.ends_with("ib")) u.erase(u.size() - 2); else if (u.size() > 1 && u.back() == 'b') u.pop_back();
    double m = u.empty() || u == "b" ? 1 : u == "k" ? 1024.0 : u == "m" ? 1048576.0 : u == "g" ? 1073741824.0 : u == "t" ? 1099511627776.0 : -1;
    if (m < 0) return std::nullopt;
    return std::uint64_t(v * m);
}
inline std::string format_size(std::uint64_t b) {
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"}; double v = double(b); int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    return i ? std::format("{:.1f} {}", v, u[i]) : std::format("{} B", b);
}

// What the OS will actually let this process have.
struct host_limits {
    std::uint64_t physical = 0, available = 0, cgroup = 0, rlimit = 0, cgroup_usage = 0;
    std::uint64_t effective() const {
        std::uint64_t e = physical ? physical : UINT64_MAX;
        if (cgroup) e = std::min(e, cgroup);
        if (rlimit) e = std::min(e, rlimit);
        return e == UINT64_MAX ? 0 : e;
    }
    // Free memory we could still grow into: the tighter of system-available and cgroup headroom.
    std::uint64_t headroom() const {
        std::uint64_t h = available ? available : UINT64_MAX;
        if (cgroup) h = std::min(h, cgroup > cgroup_usage ? cgroup - cgroup_usage : 0);
        return h == UINT64_MAX ? 0 : h;
    }
};
namespace detail {
inline std::uint64_t read_u64(const std::filesystem::path& p) {
    std::ifstream f(p); std::string s; if (!(f >> s) || s == "max") return 0;
    auto v = std::strtoull(s.c_str(), nullptr, 10); return v >= (1ull << 60) ? 0 : v;   // v1 "unlimited" is ~2^63
}
// The cgroup this process lives in, from /proc/self/cgroup ("0::/path" for v2, "N:memory:/path" for v1).
inline std::pair<std::filesystem::path, bool> cgroup_dir(const std::string& root = "/sys/fs/cgroup") {
    std::ifstream f("/proc/self/cgroup"); std::string line; std::filesystem::path v1;
    while (std::getline(f, line)) {
        auto a = line.find(':'), b = line.find(':', a + 1); if (a == std::string::npos || b == std::string::npos) continue;
        auto ctrl = line.substr(a + 1, b - a - 1), rel = line.substr(b + 1);
        if (ctrl.empty()) return {std::filesystem::path(root) / rel.substr(rel.starts_with('/') ? 1 : 0), true};
        if (ctrl.find("memory") != std::string::npos) v1 = std::filesystem::path(root) / "memory" / rel.substr(rel.starts_with('/') ? 1 : 0);
    }
    return {v1, false};
}
}
// Walk up the cgroup hierarchy: the tightest ancestor limit wins (a container sets it on a parent).
inline host_limits detect_host_limits() {
    host_limits h; h.physical = os::total_memory(); h.available = os::available_memory();
#if defined(__linux__)
    auto [dir, v2] = detail::cgroup_dir();
    std::error_code ec;
    for (auto d = dir; !d.empty() && d.string().starts_with("/sys/fs/cgroup"); d = d.parent_path()) {
        auto lim = detail::read_u64(d / (v2 ? "memory.max" : "memory.limit_in_bytes"));
        if (lim && (!h.cgroup || lim < h.cgroup)) {
            h.cgroup = lim; h.cgroup_usage = detail::read_u64(d / (v2 ? "memory.current" : "memory.usage_in_bytes"));
        }
        if (d == d.parent_path() || !std::filesystem::exists(d, ec)) break;
    }
#endif
#if defined(__unix__) || defined(__APPLE__)
    rlimit r{}; if (::getrlimit(RLIMIT_AS, &r) == 0 && r.rlim_cur != RLIM_INFINITY) h.rlimit = std::uint64_t(r.rlim_cur);
#endif
    return h;
}

class budget;

// RAII hold on `bytes` of a budget. Movable; resize() grows (may evict/fail) or shrinks in place.
class lease {
public:
    lease() = default;
    lease(budget* b, std::string tag, std::uint64_t n) : b_(b), tag_(std::move(tag)), n_(n) {}
    lease(lease&& o) noexcept : b_(std::exchange(o.b_, nullptr)), tag_(std::move(o.tag_)), n_(std::exchange(o.n_, 0)) {}
    lease& operator=(lease&& o) noexcept { if (this != &o) { release(); b_ = std::exchange(o.b_, nullptr); tag_ = std::move(o.tag_); n_ = std::exchange(o.n_, 0); } return *this; }
    lease(const lease&) = delete; lease& operator=(const lease&) = delete;
    ~lease() { release(); }
    std::uint64_t bytes() const { return n_; }
    explicit operator bool() const { return b_ != nullptr; }
    bool resize(std::uint64_t n);
    void release();
private:
    budget* b_ = nullptr; std::string tag_; std::uint64_t n_ = 0;
};

struct budget_stats {
    std::uint64_t limit = 0, used = 0, peak = 0, reservations = 0, denials = 0, waits = 0, evicted_bytes = 0, evictions = 0;
    std::map<std::string, std::uint64_t> by_tag;
};

class budget {
public:
    using size_fn = std::function<std::uint64_t()>;
    using evict_fn = std::function<std::uint64_t(std::uint64_t want)>;   // free about `want` bytes, return freed

    explicit budget(std::string name, std::uint64_t limit = 0) : name_(std::move(name)), limit_(limit) {}
    budget(const budget&) = delete; budget& operator=(const budget&) = delete;

    const std::string& name() const { return name_; }
    std::uint64_t limit() const { std::lock_guard g(m_); return limit_; }
    void set_limit(std::uint64_t n) { { std::lock_guard g(m_); limit_ = n; } cv_.notify_all(); }
    // Fraction of the limit above which caches are trimmed proactively and new work should be deferred.
    void set_soft_fraction(double f) { std::lock_guard g(m_); soft_ = std::clamp(f, 0.1, 1.0); }
    std::uint64_t soft_limit() const { std::lock_guard g(m_); return soft_limit_(); }
    std::uint64_t used() const { std::lock_guard g(m_); return used_; }
    std::uint64_t available() const { std::lock_guard g(m_); return limit_ ? (limit_ > used_ ? limit_ - used_ : 0) : UINT64_MAX; }
    // Above the soft watermark: callers doing admission control should hold back new work.
    // `extra` is the projected cost of the work about to start.
    bool under_pressure(std::uint64_t extra = 0) const { std::lock_guard g(m_); return limit_ && used_ + extra + cache_bytes_locked_() > soft_limit_(); }

    // Non-blocking: evicts caches if that makes room, else returns an empty lease.
    lease try_reserve(std::uint64_t n, std::string tag = "other") {
        std::unique_lock g(m_);
        if (!fits_(n)) { g.unlock(); evict(need_(n)); g.lock(); }
        if (!fits_(n)) { ++st_.denials; return {}; }
        charge_(n, tag); return lease(this, std::move(tag), n);
    }
    // Blocking: evicts, then waits up to `timeout` for leases to be released; throws errc::out_of_memory.
    lease reserve(std::uint64_t n, std::string tag = "other", std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        if (auto l = try_reserve(n, tag)) return l;
        std::unique_lock g(m_);
        if (limit_ && n > limit_) { raise(errc::out_of_memory, "{}: {} needs {} but the whole budget is {} (raise FORGE_MEM_LIMIT or --mem-limit)", name_, tag, format_size(n), format_size(limit_)); }
        if (timeout.count() > 0) {
            ++st_.waits;
            auto dl = std::chrono::steady_clock::now() + timeout;
            while (!fits_(n)) {
                if (cv_.wait_until(g, dl) == std::cv_status::timeout && !fits_(n)) break;
                if (!fits_(n)) { g.unlock(); evict(need_(n)); g.lock(); }
            }
            if (fits_(n)) { charge_(n, tag); return lease(this, std::move(tag), n); }
        }
        ++st_.denials;
        raise(errc::out_of_memory, "{}: cannot reserve {} for {} ({} of {} in use; set FORGE_MEM_LIMIT or free caches)", name_, format_size(n), tag, format_size(used_), format_size(limit_));
    }
    // Externally sized consumers (a KV cache that grows by itself) report their current size here; the
    // governor counts it but never blocks on it. Use set_usage(tag, 0) to drop.
    void set_usage(const std::string& tag, std::uint64_t n) {
        std::lock_guard g(m_); auto& cur = ext_[tag]; used_ = used_ - cur + n; cur = n;
        st_.by_tag[tag] = n; st_.peak = std::max(st_.peak, used_);
        if (!n) { ext_.erase(tag); st_.by_tag.erase(tag); }
    }

    // Register something droppable. Lower priority is evicted first; ties go least-recently-touched first.
    int add_cache(std::string name, size_fn size, evict_fn evict, int priority = 0) {
        std::lock_guard g(m_); int id = ++next_;
        caches_[id] = {std::move(name), std::move(size), std::move(evict), priority, ++clock_}; return id;
    }
    void remove_cache(int id) { std::lock_guard g(m_); caches_.erase(id); }
    void touch_cache(int id) { std::lock_guard g(m_); if (auto it = caches_.find(id); it != caches_.end()) it->second.last = ++clock_; }
    std::uint64_t cache_bytes() const { std::lock_guard g(m_); return cache_bytes_locked_(); }

    // Ask caches to free `want` bytes (0 = everything). Returns bytes freed. Callbacks run without the lock.
    std::uint64_t evict(std::uint64_t want) {
        std::vector<std::pair<int, cache_entry>> order;
        { std::lock_guard g(m_); for (auto& [id, c] : caches_) order.push_back({id, c}); }
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second.priority != b.second.priority ? a.second.priority < b.second.priority : a.second.last < b.second.last; });
        std::uint64_t freed = 0;
        for (auto& [id, c] : order) {
            if (want && freed >= want) break;
            if (c.size && !c.size()) continue;   // nothing to give
            std::uint64_t f = c.evict ? c.evict(want ? want - freed : 0) : 0;
            if (f) { freed += f; std::lock_guard g(m_); st_.evicted_bytes += f; ++st_.evictions; }
        }
        if (freed) cv_.notify_all();
        return freed;
    }
    // Trim caches down so used + cached sits under the soft watermark (called on pressure and by maintenance).
    std::uint64_t trim() {
        std::uint64_t over;
        { std::lock_guard g(m_); auto tot = used_ + cache_bytes_locked_(), soft = soft_limit_(); if (!limit_ || tot <= soft) return 0; over = tot - soft; }
        return evict(over);
    }

    budget_stats stats() const { std::lock_guard g(m_); auto s = st_; s.limit = limit_; s.used = used_; return s; }
    // Prometheus text exposition for this budget.
    std::string prometheus() const {
        auto s = stats(); auto l = std::format("{{pool=\"{}\"}}", name_); std::string o;
        o += std::format("forge_mem_limit_bytes{} {}\nforge_mem_used_bytes{} {}\nforge_mem_peak_bytes{} {}\nforge_mem_cache_bytes{} {}\n", l, s.limit, l, s.used, l, s.peak, l, cache_bytes());
        o += std::format("forge_mem_reservations_total{} {}\nforge_mem_denials_total{} {}\nforge_mem_waits_total{} {}\nforge_mem_evictions_total{} {}\nforge_mem_evicted_bytes_total{} {}\n",
                         l, s.reservations, l, s.denials, l, s.waits, l, s.evictions, l, s.evicted_bytes);
        for (auto& [t, n] : s.by_tag) o += std::format("forge_mem_used_bytes_by_tag{{pool=\"{}\",tag=\"{}\"}} {}\n", name_, t, n);
        return o;
    }

private:
    friend class lease;
    struct cache_entry { std::string name; size_fn size; evict_fn evict; int priority = 0; std::uint64_t last = 0; };
    bool fits_(std::uint64_t n) const { return !limit_ || used_ + n <= limit_; }
    std::uint64_t need_(std::uint64_t n) const { return limit_ && used_ + n > limit_ ? used_ + n - limit_ : 0; }
    std::uint64_t soft_limit_() const { return std::uint64_t(double(limit_) * soft_); }
    std::uint64_t cache_bytes_locked_() const { std::uint64_t s = 0; for (auto& [_, c] : caches_) if (c.size) s += c.size(); return s; }
    void charge_(std::uint64_t n, const std::string& tag) { used_ += n; st_.by_tag[tag] += n; ++st_.reservations; st_.peak = std::max(st_.peak, used_); }
    void uncharge_(std::uint64_t n, const std::string& tag) {
        { std::lock_guard g(m_); used_ -= std::min(used_, n); auto& t = st_.by_tag[tag]; t -= std::min(t, n); if (!t) st_.by_tag.erase(tag); }
        cv_.notify_all();
    }
    bool grow_(std::uint64_t by, const std::string& tag) {
        std::unique_lock g(m_);
        if (!fits_(by)) { g.unlock(); evict(need_(by)); g.lock(); }
        if (!fits_(by)) { ++st_.denials; return false; }
        used_ += by; st_.by_tag[tag] += by; st_.peak = std::max(st_.peak, used_); return true;
    }
    std::string name_;
    mutable std::mutex m_; std::condition_variable cv_;
    std::uint64_t limit_ = 0, used_ = 0, clock_ = 0; double soft_ = 0.9; int next_ = 0;
    std::map<int, cache_entry> caches_; std::map<std::string, std::uint64_t> ext_;
    budget_stats st_;
};

inline void lease::release() { if (b_) { b_->uncharge_(n_, tag_); b_ = nullptr; n_ = 0; } }
inline bool lease::resize(std::uint64_t n) {
    if (!b_) return false;
    if (n > n_) { if (!b_->grow_(n - n_, tag_)) return false; }
    else if (n < n_) b_->uncharge_(n_ - n, tag_);
    n_ = n; return true;
}

// Process-wide registry of budgets. host() is created on first use from the environment.
class governor {
public:
    // Never destroyed on purpose: objects with static storage duration elsewhere (a model cache holding a GPU runtime) release
    // their budget leases in their own destructors at exit, possibly after a function-local static would already be gone.
    static governor& instance() { static governor* g = new governor; return *g; }
    budget& host() { return pool("host"); }
    budget& pool(const std::string& name) {
        std::lock_guard g(m_);
        auto& p = pools_[name];
        if (!p) { p = std::make_unique<budget>(name, default_limit_(name)); p->set_soft_fraction(soft_fraction_()); }
        return *p;
    }
    std::vector<budget*> pools() { std::lock_guard g(m_); std::vector<budget*> v; for (auto& [_, p] : pools_) v.push_back(p.get()); return v; }
    // Hook os::memory_pressure: moderate trims to the soft watermark, critical drops every cache.
    // Idempotent. FORGE_MEM_PRESSURE=0 disables it.
    void watch_pressure() {
        std::lock_guard g(m_); if (sub_ || os::getenv("FORGE_MEM_PRESSURE") == "0") return;
        sub_ = os::memory_pressure::instance().subscribe([this](os::pressure_level l) { on_pressure(l); });
    }
    void on_pressure(os::pressure_level l) {
        ++pressure_events_;
        if (l == os::pressure_level::normal) return;
        for (auto* p : pools()) { if (l == os::pressure_level::critical) p->evict(0); else p->trim(); }
    }
    std::uint64_t pressure_events() const { return pressure_events_.load(); }
    std::string prometheus() {
        std::string o; for (auto* p : pools()) o += p->prometheus();
        o += std::format("forge_mem_pressure_events_total {}\n", pressure_events());
        return o;
    }
    // Human-readable lines for `forge info`.
    std::string describe() {
        auto h = detect_host_limits(); std::string o;
        o += std::format("memory   physical {}, available {}", format_size(h.physical), format_size(h.available));
        if (h.cgroup) o += std::format(", cgroup limit {} ({} used)", format_size(h.cgroup), format_size(h.cgroup_usage));
        if (h.rlimit) o += std::format(", RLIMIT_AS {}", format_size(h.rlimit));
        o += std::format("\nbudget   host {} (soft {}), source {}\n", format_size(host().limit()), format_size(host().soft_limit()), limit_source_);
        return o;
    }
private:
    governor() = default;
    static double soft_fraction_() { auto e = os::getenv("FORGE_MEM_SOFT"); double f = e.empty() ? 0.9 : std::atof(e.c_str()); return f > 0 && f <= 1 ? f : 0.9; }
    std::uint64_t default_limit_(const std::string& name) {
        if (name != "host") { auto e = os::getenv("FORGE_MEM_DEVICE_LIMIT"); return e.empty() ? 0 : parse_size(e).value_or(0); }
        auto h = detect_host_limits(); auto eff = h.effective();
        if (auto e = os::getenv("FORGE_MEM_LIMIT"); !e.empty()) {
            if (e == "0" || e == "off" || e == "none") { limit_source_ = "disabled (FORGE_MEM_LIMIT)"; return 0; }
            if (auto v = parse_size(e, eff)) { limit_source_ = "FORGE_MEM_LIMIT"; return *v; }
        }
        auto fe = os::getenv("FORGE_MEM_FRACTION"); double f = fe.empty() ? 0.85 : std::atof(fe.c_str()); if (!(f > 0 && f <= 1)) f = 0.85;
        limit_source_ = std::format("{:.0f}% of {}", f * 100, h.cgroup && h.cgroup <= eff ? "cgroup limit" : h.rlimit && h.rlimit <= eff ? "RLIMIT_AS" : "physical RAM");
        return std::uint64_t(double(eff) * f);
    }
    std::mutex m_; std::map<std::string, std::unique_ptr<budget>> pools_; int sub_ = 0; std::string limit_source_ = "unset";
    std::atomic<std::uint64_t> pressure_events_{0};
};
inline budget& host_budget() { return governor::instance().host(); }

// std::pmr resource that charges a budget for every allocation and throws errc::out_of_memory (a
// forge_exception, which is also a std::runtime_error) instead of handing out memory past the limit.
class budget_resource : public std::pmr::memory_resource {
public:
    explicit budget_resource(budget& b, std::string tag = "pmr", std::pmr::memory_resource* up = std::pmr::new_delete_resource())
        : b_(b), tag_(std::move(tag)), up_(up), l_(b.try_reserve(0, tag_)) {}
private:
    void* do_allocate(std::size_t n, std::size_t a) override {
        if (!l_.resize(l_.bytes() + n)) raise(errc::out_of_memory, "{}: allocation of {} for {} exceeds the budget", b_.name(), format_size(n), tag_);
        try { return up_->allocate(n, a); } catch (...) { l_.resize(l_.bytes() - n); throw; }
    }
    void do_deallocate(void* p, std::size_t n, std::size_t a) override { up_->deallocate(p, n, a); l_.resize(l_.bytes() - std::min<std::uint64_t>(n, l_.bytes())); }
    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override { return this == &o; }
    budget& b_; std::string tag_; std::pmr::memory_resource* up_; lease l_;
};

// Evicted blobs on disk. Directory: `dir`, else FORGE_MEM_SPILL_DIR, else a fresh <tmp>/forge-spill-<pid>-<random>;
// removed on destruction. Writes are size-capped by FORGE_MEM_SPILL_MAX (default unlimited).
// FSS: the directory is private (0700 / owner + SYSTEM + Administrators DACL; an existing one must be ours and not a
// symlink, and is tightened to 0700 on POSIX) and every spill file is created exclusively, 0600, without following
// symlinks (FIO-1). Under an active forge::security policy the directory must pass enforce(fs_write) (CAP-1, through
// os::check_access); a denial throws errc::io_error with the policy's reason.
class spill_store {
public:
    explicit spill_store(std::filesystem::path dir = {}) {
        if (dir.empty()) if (auto e = os::getenv("FORGE_MEM_SPILL_DIR"); !e.empty()) dir = std::filesystem::path(e);
        if (!dir.empty()) {
            auto u = dir.u8string(); std::string d(u.begin(), u.end());
            if (auto s = os::check_access(os::access_kind::write, d); !s) raise(errc::io_error, "spill directory {}: {}", d, s.err.what);
            if (auto s = os::ensure_private_dir(d); !s) raise(errc::io_error, "spill directory {}: {}", d, s.err.what);
            dir_ = std::move(dir);
        } else {
            if (auto s = os::check_access(os::access_kind::write, os::temp_dir()); !s) raise(errc::io_error, "spill directory {}: {}", os::temp_dir(), s.err.what);
            auto t = os::make_private_dir_in(os::temp_dir(), std::format("forge-spill-{}-", os::pid()));
            if (!t) raise(errc::io_error, "spill directory: {}", t.err.what);
            dir_ = std::filesystem::path(std::u8string(t.value.begin(), t.value.end()));
        }
        if (auto m = os::getenv("FORGE_MEM_SPILL_MAX"); !m.empty()) cap_ = parse_size(m).value_or(0);
    }
    ~spill_store() { std::error_code ec; for (auto& [k, _] : sizes_) std::filesystem::remove(path_(k), ec); std::filesystem::remove(dir_, ec); }
    spill_store(const spill_store&) = delete; spill_store& operator=(const spill_store&) = delete;
    // Returns a key, or 0 when the cap would be exceeded or the write failed.
    std::uint64_t put(const void* data, std::size_t n) {
        std::lock_guard g(m_);
        if (cap_ && bytes_ + n > cap_) return 0;
        auto k = ++next_;
        auto u = path_(k).u8string();
        if (!os::write_new_private_file(std::string(u.begin(), u.end()), std::string_view(static_cast<const char*>(data), n))) return 0;   // removes a partial file
        sizes_[k] = n; bytes_ += n; return k;
    }
    // Reads back and deletes. Empty optional if the key is unknown.
    std::optional<std::vector<std::byte>> take(std::uint64_t k) {
        std::lock_guard g(m_); auto it = sizes_.find(k); if (it == sizes_.end()) return std::nullopt;
        std::vector<std::byte> v(it->second); std::ifstream f(path_(k), std::ios::binary); f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size()));
        bool ok = bool(f); f.close(); bytes_ -= it->second; sizes_.erase(it); std::error_code ec; std::filesystem::remove(path_(k), ec);
        if (!ok) raise(errc::io_error, "spill file {} truncated", path_(k).string());
        return v;
    }
    void drop(std::uint64_t k) { std::lock_guard g(m_); if (auto it = sizes_.find(k); it != sizes_.end()) { bytes_ -= it->second; sizes_.erase(it); std::error_code ec; std::filesystem::remove(path_(k), ec); } }
    std::uint64_t bytes() const { std::lock_guard g(m_); return bytes_; }
    const std::filesystem::path& dir() const { return dir_; }
private:
    std::filesystem::path path_(std::uint64_t k) const { return dir_ / std::format("{}.bin", k); }
    std::filesystem::path dir_; mutable std::mutex m_; std::map<std::uint64_t, std::size_t> sizes_; std::uint64_t next_ = 0, bytes_ = 0, cap_ = 0;
};

// A child process whose address-space cap is a lease on a budget. The cap and the charge are the same
// number, so concurrent children can never promise more than the budget holds; the lease is returned the
// moment the child is reaped (wait/try_wait/wait_any through proc) or the leased_child is destroyed.
struct leased_child {
    os::child proc;
    lease mem;
    int wait(std::uint64_t timeout_ms = 0) { int c = proc.wait(timeout_ms); if (proc.exited()) mem.release(); return c; }
    int try_wait() { int c = proc.try_wait(); if (proc.exited()) mem.release(); return c; }
    void reap_if_done() { if (proc.exited()) mem.release(); }   // call after os::wait_any picked this child
    std::uint64_t cap() const { return mem.bytes(); }
};

// Reserve `bytes` from `b` (waiting up to `wait` for room; 0 = fail at once) and start the child with that
// many bytes as its memory cap. Throws errc::out_of_memory if the reservation cannot be made; returns the
// spawn error otherwise (the reservation is released on failure).
inline os::result<leased_child> spawn_leased(budget& b, os::spawn_spec sp, std::uint64_t bytes, std::string tag = "child",
                                             std::chrono::milliseconds wait = std::chrono::milliseconds(0)) {
    lease l = wait.count() > 0 ? b.reserve(bytes, tag, wait) : b.try_reserve(bytes, tag);
    if (!l) raise(errc::out_of_memory, "{}: cannot reserve {} for child {} ({} of {} used)", b.name(), format_size(bytes),
                  sp.argv.empty() ? std::string("?") : sp.argv[0], format_size(b.used()), format_size(b.limit()));
    sp.limits.memory_bytes = bytes;
    auto c = os::child::start(sp);
    if (!c) return {{}, c.err};
    return {leased_child{std::move(c.value), std::move(l)}, {}};
}
} // namespace forge::mem
