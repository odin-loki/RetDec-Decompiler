#pragma once
// forge::loadbalancer: a general-purpose load balancer. One pool type routes anything addressable: devices,
// sessions, RPC nodes, inference backends, network upstreams.
//   strategies : round_robin, weighted (smooth WRR), least_connections, least_latency (EWMA), consistent_hash
//                (ring + virtual nodes, bounded load), power_of_two (P2C on load and latency), prefix_affinity
//                (KV-cache / prompt-prefix locality with load spill).
//   resilience : passive and active health checks, per-backend circuit breakers (closed/open/half-open),
//                retries with exponential backoff and jitter on a different backend, per-backend concurrency
//                limits plus a bounded wait queue with deadlines (backpressure), outlier ejection.
// Thread-safe. Header-only. Time comes from forge::os::monotonic_ns.
#include <forge/os/os.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace forge::loadbalancer {

inline std::uint64_t hash64(std::string_view s, std::uint64_t seed = 0xcbf29ce484222325ull) {   // FNV-1a + fmix
    std::uint64_t h = seed;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
    return h;
}

enum class strategy { round_robin, weighted, least_connections, least_latency, consistent_hash, power_of_two, prefix_affinity };
inline const char* strategy_name(strategy s) {
    constexpr const char* n[] = {"round_robin", "weighted", "least_connections", "least_latency", "consistent_hash", "power_of_two", "prefix_affinity"};
    return n[int(s)];
}
inline std::optional<strategy> parse_strategy(std::string_view s) {
    for (int i = 0; i <= int(strategy::prefix_affinity); ++i) if (s == strategy_name(strategy(i))) return strategy(i);
    if (s == "rr") return strategy::round_robin; if (s == "wrr") return strategy::weighted; if (s == "lc") return strategy::least_connections;
    if (s == "ewma") return strategy::least_latency; if (s == "hash" || s == "ring") return strategy::consistent_hash; if (s == "p2c") return strategy::power_of_two;
    if (s == "prefix" || s == "kv") return strategy::prefix_affinity;
    return std::nullopt;
}

enum class circuit { closed, open, half_open };

struct backend_spec {
    std::string id;            // unique: "gpu0", "10.0.0.5:7000", "session-node-3"
    std::string address;       // free-form target (host:port, device name, ...)
    double weight = 1.0;
    unsigned max_inflight = 0; // 0 = unlimited
    void* user = nullptr;      // caller payload (device handle, rpc client, ...)
};

struct config {
    strategy strat = strategy::power_of_two;
    unsigned vnodes = 160;                 // consistent hash points per unit weight
    double hash_load_factor = 1.25;        // bounded-load consistent hashing: max load = ceil(avg * factor)
    double ewma_alpha = 0.3;               // latency smoothing
    double ewma_decay_ns = 10e9;           // stale latency estimates decay toward the pool mean
    unsigned failure_threshold = 5;        // consecutive failures that open the circuit
    double failure_rate_threshold = 0.5;   // or error rate over the window (with >= min_requests)
    unsigned window = 20, min_requests = 10;
    std::uint64_t open_ns = 5'000'000'000; // open -> half-open after this
    unsigned half_open_probes = 1;         // concurrent trial requests while half-open
    unsigned healthy_after = 2, unhealthy_after = 3;   // active health check hysteresis
    std::size_t prefix_block = 64;         // bytes per prefix block for affinity hashing
    std::size_t prefix_table = 65536;      // remembered prefix blocks
    double prefix_spill = 1.5;             // leave the affine backend once its load exceeds avg * spill + 1
    std::size_t queue_limit = 1024;        // waiting requests when every backend is at max_inflight
};

struct backend_stats {
    std::string id; bool healthy = true; circuit state = circuit::closed;
    unsigned inflight = 0; std::uint64_t requests = 0, failures = 0; double ewma_ms = 0, weight = 1;
};

struct retry_policy { unsigned attempts = 3; std::uint64_t base_backoff_ns = 2'000'000, max_backoff_ns = 200'000'000; std::uint64_t wait_ns = 0; };
class pool;
// RAII lease on a backend: report the outcome (or it counts as a failure if dropped un-reported).
class lease {
public:
    lease() = default;
    lease(lease&& o) noexcept { *this = std::move(o); }
    lease& operator=(lease&& o) noexcept { finish_(false, 0, true); p_ = std::exchange(o.p_, nullptr); idx_ = o.idx_; t0_ = o.t0_; probe_ = o.probe_; return *this; }
    ~lease() { finish_(false, 0, true); }
    explicit operator bool() const { return p_ != nullptr; }
    const backend_spec& backend() const;
    std::size_t index() const { return idx_; }
    void success() { finish_(true, os::monotonic_ns() - t0_, false); }
    void failure() { finish_(false, os::monotonic_ns() - t0_, false); }
    void success_with_latency(std::uint64_t ns) { finish_(true, ns, false); }
private:
    friend class pool;
    lease(pool* p, std::size_t i, bool probe) : p_(p), idx_(i), t0_(os::monotonic_ns()), probe_(probe) {}
    void finish_(bool ok, std::uint64_t ns, bool dropped);
    pool* p_ = nullptr; std::size_t idx_ = 0; std::uint64_t t0_ = 0; bool probe_ = false;
};

struct pick_options {
    std::string_view key;                    // session / user / request id for hashing strategies
    std::string_view prefix;                 // prompt tokens or bytes for prefix_affinity
    const std::vector<std::size_t>* exclude = nullptr;   // already-tried backends (retries)
    std::uint64_t wait_ns = 0;               // backpressure: wait this long for capacity (0 = fail fast)
};

class pool {
public:
    explicit pool(config c = {}) : cfg_(c) {}
    pool(const pool&) = delete; pool& operator=(const pool&) = delete;

    const config& cfg() const { return cfg_; }
    void set_strategy(strategy s) { std::lock_guard g(m_); cfg_.strat = s; }

    std::size_t add(backend_spec s) {
        std::lock_guard g(m_);
        for (std::size_t i = 0; i < b_.size(); ++i) if (b_[i].spec.id == s.id && b_[i].removed) { b_[i] = node{std::move(s)}; rebuild_ring_(); return i; }
        b_.push_back(node{std::move(s)}); rebuild_ring_(); return b_.size() - 1;
    }
    bool remove(std::string_view id) {   // drains: in-flight leases finish normally
        std::lock_guard g(m_);
        for (auto& n : b_) if (n.spec.id == id && !n.removed) { n.removed = true; rebuild_ring_(); cv_.notify_all(); return true; }
        return false;
    }
    bool set_weight(std::string_view id, double w) {
        std::lock_guard g(m_);
        for (auto& n : b_) if (n.spec.id == id) { n.spec.weight = std::max(0.0, w); rebuild_ring_(); return true; }
        return false;
    }
    void set_healthy(std::string_view id, bool h) {   // administrative / active-check override
        std::lock_guard g(m_);
        for (auto& n : b_) if (n.spec.id == id) { n.healthy = h; n.hc_ok = n.hc_bad = 0; }
        cv_.notify_all();
    }
    std::size_t size() const { std::lock_guard g(m_); std::size_t k = 0; for (auto& n : b_) k += !n.removed; return k; }

    // Choose a backend. Empty lease when none is available (all down / excluded / saturated past wait).
    lease acquire(const pick_options& o = {}) {
        std::unique_lock g(m_);
        std::uint64_t deadline = o.wait_ns ? os::monotonic_ns() + o.wait_ns : 0;
        bool queued = false;
        for (;;) {
            auto now = os::monotonic_ns();
            auto [idx, probe] = pick_(o, now);
            if (idx != npos) {
                if (queued) --waiting_;
                auto& n = b_[idx]; ++n.inflight; ++n.requests; if (probe) ++n.probes;
                return lease(this, idx, probe);
            }
            if (!deadline || now >= deadline || !any_could_serve_(o, now)) { if (queued) --waiting_; ++rejected_; return {}; }
            if (!queued) { if (waiting_ >= cfg_.queue_limit) { ++rejected_; return {}; } ++waiting_; queued = true; }
            cv_.wait_for(g, std::chrono::nanoseconds(std::min<std::uint64_t>(deadline - now, 50'000'000)));
        }
    }

    // Run fn on a backend with retries on other backends. fn returns true on success.
    template <class F> bool call(const pick_options& o, F&& fn, retry_policy rp = {}) {
        std::vector<std::size_t> tried; pick_options po = o; po.wait_ns = rp.wait_ns;
        std::uint64_t backoff = rp.base_backoff_ns;
        for (unsigned a = 0; a < std::max(1u, rp.attempts); ++a) {
            po.exclude = &tried;
            lease l = acquire(po);
            if (!l) { if (tried.empty()) return false; tried.clear(); l = acquire(po); if (!l) return false; }
            tried.push_back(l.index());
            bool ok = false;
            try { ok = fn(l.backend()); } catch (...) { ok = false; }
            if (ok) { l.success(); return true; }
            l.failure();
            if (a + 1 < rp.attempts) {
                std::uint64_t j = backoff / 2 + (rng_() % (backoff / 2 + 1));
                os::sleep_ns(j); backoff = std::min(rp.max_backoff_ns, backoff * 2);
            }
        }
        return false;
    }

    // Active health checking: probe(backend) -> healthy?  Call tick() periodically (or run_health_checks()).
    void set_health_check(std::function<bool(const backend_spec&)> probe) { std::lock_guard g(m_); probe_ = std::move(probe); }
    void run_health_checks() {
        std::vector<std::pair<std::size_t, backend_spec>> todo;
        std::function<bool(const backend_spec&)> pr;
        { std::lock_guard g(m_); pr = probe_; for (std::size_t i = 0; i < b_.size(); ++i) if (!b_[i].removed) todo.push_back({i, b_[i].spec}); }
        if (!pr) return;
        for (auto& [i, s] : todo) {
            bool ok = false; try { ok = pr(s); } catch (...) {}
            std::lock_guard g(m_);
            auto& n = b_[i];
            if (ok) { n.hc_bad = 0; if (!n.healthy && ++n.hc_ok >= cfg_.healthy_after) { n.healthy = true; n.hc_ok = 0; } }
            else { n.hc_ok = 0; if (n.healthy && ++n.hc_bad >= cfg_.unhealthy_after) { n.healthy = false; n.hc_bad = 0; } }
        }
        cv_.notify_all();
    }

    std::vector<backend_stats> stats() const {
        std::lock_guard g(m_); std::vector<backend_stats> v; auto now = os::monotonic_ns();
        for (auto& n : b_) if (!n.removed) v.push_back({n.spec.id, n.healthy, state_(n, now), n.inflight, n.requests, n.failures, n.ewma_ns / 1e6, n.spec.weight});
        return v;
    }
    std::uint64_t rejected() const { std::lock_guard g(m_); return rejected_; }
    std::size_t waiting() const { std::lock_guard g(m_); return waiting_; }
    const backend_spec& spec(std::size_t i) const { std::lock_guard g(m_); return b_[i].spec; }

private:
    friend class lease;
    static constexpr std::size_t npos = ~std::size_t(0);
    struct node {
        backend_spec spec;
        bool removed = false, healthy = true;
        unsigned inflight = 0, probes = 0, consecutive_fail = 0, hc_ok = 0, hc_bad = 0;
        std::uint64_t requests = 0, failures = 0;
        double ewma_ns = 0; std::uint64_t ewma_at = 0;
        double wrr_current = 0;
        std::vector<bool> win; std::size_t win_pos = 0;   // recent outcomes (true = failure)
        std::uint64_t opened_at = 0; bool open = false;
    };
    circuit state_(const node& n, std::uint64_t now) const {
        if (!n.open) return circuit::closed;
        return now - n.opened_at >= cfg_.open_ns ? circuit::half_open : circuit::open;
    }
    // Usable now? probe = admitted as a half-open trial.
    bool usable_(const node& n, std::uint64_t now, bool& probe) const {
        probe = false;
        if (n.removed || !n.healthy || n.spec.weight <= 0) return false;
        if (n.spec.max_inflight && n.inflight >= n.spec.max_inflight) return false;
        auto st = state_(n, now);
        if (st == circuit::open) return false;
        if (st == circuit::half_open) { if (n.probes >= cfg_.half_open_probes) return false; probe = true; }
        return true;
    }
    bool excluded_(std::size_t i, const pick_options& o) const { return o.exclude && std::find(o.exclude->begin(), o.exclude->end(), i) != o.exclude->end(); }
    bool any_could_serve_(const pick_options& o, std::uint64_t now) const {   // up but saturated -> worth waiting
        for (std::size_t i = 0; i < b_.size(); ++i) { auto& n = b_[i]; if (!n.removed && n.healthy && n.spec.weight > 0 && !excluded_(i, o) && state_(n, now) != circuit::open) return true; }
        return false;
    }
    double latency_(const node& n, std::uint64_t now) const {   // decayed EWMA toward the pool mean; unmeasured = 0 so it gets explored
        double mean = 0; int k = 0; for (auto& x : b_) if (!x.removed && x.ewma_at) { mean += x.ewma_ns; ++k; }
        mean = k ? mean / k : 0;
        if (!n.ewma_at) return 0;
        double age = double(now - n.ewma_at), w = std::exp(-age / cfg_.ewma_decay_ns);
        return n.ewma_ns * w + mean * (1 - w);
    }
    double load_(const node& n) const { return (n.inflight + 1.0) / std::max(1e-9, n.spec.weight); }
    std::pair<std::size_t, bool> pick_(const pick_options& o, std::uint64_t now) {
        std::vector<std::size_t> c; std::vector<bool> probe;
        for (std::size_t i = 0; i < b_.size(); ++i) { bool p; if (!excluded_(i, o) && usable_(b_[i], now, p)) { c.push_back(i); probe.push_back(p); } }
        if (c.empty()) return {npos, false};
        // half-open trials take priority so a recovered backend gets its probe promptly
        for (std::size_t k = 0; k < c.size(); ++k) if (probe[k]) return {c[k], true};
        std::size_t r = npos;
        switch (cfg_.strat) {
        case strategy::round_robin: r = c[rr_++ % c.size()]; break;
        case strategy::weighted: {   // smooth weighted round-robin (nginx)
            double total = 0; node* best = nullptr; std::size_t bi = 0;
            for (auto i : c) { auto& n = b_[i]; n.wrr_current += n.spec.weight; total += n.spec.weight; if (!best || n.wrr_current > best->wrr_current) { best = &n; bi = i; } }
            best->wrr_current -= total; r = bi; break;
        }
        case strategy::least_connections: {
            r = c[0]; for (auto i : c) if (load_(b_[i]) < load_(b_[r]) || (load_(b_[i]) == load_(b_[r]) && b_[i].requests < b_[r].requests)) r = i;
            break;
        }
        case strategy::least_latency: {   // EWMA latency x (inflight + 1): the "peak EWMA" cost
            double best = 0; for (auto i : c) { double cost = (latency_(b_[i], now) + 1.0) * load_(b_[i]); if (r == npos || cost < best) { best = cost; r = i; } }
            break;
        }
        case strategy::power_of_two: {
            if (c.size() == 1) { r = c[0]; break; }
            std::size_t a = c[rng_() % c.size()], b = c[rng_() % (c.size() - 1)]; if (b == a) b = c.back();
            auto cost = [&](std::size_t i) { return (latency_(b_[i], now) + 1.0) * load_(b_[i]); };
            r = cost(a) <= cost(b) ? a : b; break;
        }
        case strategy::consistent_hash: r = ring_pick_(hash64(o.key), c); break;
        case strategy::prefix_affinity: r = prefix_pick_(o, c); break;
        }
        return {r, false};
    }
    void rebuild_ring_() {
        ring_.clear();
        for (std::size_t i = 0; i < b_.size(); ++i) {
            if (b_[i].removed || b_[i].spec.weight <= 0) continue;
            unsigned pts = std::max(1u, unsigned(std::lround(cfg_.vnodes * b_[i].spec.weight)));
            for (unsigned v = 0; v < pts; ++v) ring_.push_back({hash64(b_[i].spec.id + "#" + std::to_string(v)), i});
        }
        std::sort(ring_.begin(), ring_.end());
    }
    // Ring walk from h; skips candidates that are unavailable or over the bounded-load cap.
    std::size_t ring_pick_(std::uint64_t h, const std::vector<std::size_t>& c) {
        if (ring_.empty()) return c[0];
        std::vector<char> ok(b_.size(), 0); unsigned total = 1; for (auto i : c) { ok[i] = 1; total += b_[i].inflight; }
        double wsum = 0; for (auto i : c) wsum += b_[i].spec.weight;
        auto it = std::lower_bound(ring_.begin(), ring_.end(), std::pair<std::uint64_t, std::size_t>{h, 0});
        std::size_t first = npos;
        for (std::size_t k = 0; k < ring_.size(); ++k, ++it) {
            if (it == ring_.end()) it = ring_.begin();
            std::size_t i = it->second; if (!ok[i]) continue;
            if (first == npos) first = i;
            double cap = std::ceil(total * cfg_.hash_load_factor * b_[i].spec.weight / wsum);
            if (b_[i].inflight < cap) return i;
        }
        return first == npos ? c[0] : first;
    }
    // Longest remembered prefix block wins if its backend is not overloaded; else least-loaded. The chosen
    // backend is then recorded for every block of this prefix (it now holds that KV state).
    std::size_t prefix_pick_(const pick_options& o, const std::vector<std::size_t>& c) {
        std::vector<std::uint64_t> blocks;
        std::uint64_t h = 0x9e3779b97f4a7c15ull;
        for (std::size_t off = 0; off + cfg_.prefix_block <= o.prefix.size(); off += cfg_.prefix_block) { h = hash64(o.prefix.substr(off, cfg_.prefix_block), h); blocks.push_back(h); }
        double avg = 0; for (auto i : c) avg += b_[i].inflight; avg /= double(c.size());
        std::size_t r = npos;
        for (auto bi = blocks.rbegin(); bi != blocks.rend() && r == npos; ++bi) {
            auto f = prefix_.find(*bi); if (f == prefix_.end()) continue;
            std::size_t i = f->second;
            if (std::find(c.begin(), c.end(), i) != c.end() && b_[i].inflight <= avg * cfg_.prefix_spill + 1) r = i;
        }
        if (r == npos) {
            if (!o.key.empty() && blocks.empty()) r = ring_pick_(hash64(o.key), c);
            else { r = c[0]; for (auto i : c) if (load_(b_[i]) < load_(b_[r])) r = i; }
        }
        if (prefix_.size() + blocks.size() > cfg_.prefix_table) prefix_.clear();   // coarse eviction
        for (auto b : blocks) prefix_[b] = r;
        return r;
    }
    void finish_(std::size_t i, bool ok, std::uint64_t ns, bool probe) {
        std::lock_guard g(m_);
        auto& n = b_[i];
        if (n.inflight) --n.inflight; if (probe && n.probes) --n.probes;
        auto now = os::monotonic_ns();
        if (ok) {
            n.ewma_ns = n.ewma_at ? cfg_.ewma_alpha * double(ns) + (1 - cfg_.ewma_alpha) * n.ewma_ns : double(ns); n.ewma_at = now;
            n.consecutive_fail = 0;
            if (n.open && state_(n, now) == circuit::half_open) { n.open = false; n.win.assign(n.win.size(), false); }
        } else {
            ++n.failures; ++n.consecutive_fail;
            if (n.open) n.opened_at = now;   // failed probe: stay open for another period
        }
        if (n.win.size() != cfg_.window) n.win.assign(cfg_.window, false);
        n.win[n.win_pos++ % cfg_.window] = !ok;
        std::size_t seen = std::min<std::size_t>(n.requests, cfg_.window), bad = 0; for (bool b : n.win) bad += b;
        if (!n.open && (n.consecutive_fail >= cfg_.failure_threshold || (seen >= cfg_.min_requests && double(bad) / double(seen) >= cfg_.failure_rate_threshold))) {
            n.open = true; n.opened_at = now;
        }
        cv_.notify_all();
    }

    config cfg_;
    mutable std::mutex m_; std::condition_variable cv_;
    std::vector<node> b_;
    std::vector<std::pair<std::uint64_t, std::size_t>> ring_;
    std::unordered_map<std::uint64_t, std::size_t> prefix_;
    std::size_t rr_ = 0, waiting_ = 0; std::uint64_t rejected_ = 0;
    std::mt19937_64 rng_{0x5eed};
    std::function<bool(const backend_spec&)> probe_;
};

inline const backend_spec& lease::backend() const { return p_->b_[idx_].spec; }
inline void lease::finish_(bool ok, std::uint64_t ns, bool dropped) {
    if (!p_) return;
    (void)dropped;
    p_->finish_(idx_, ok, ns, probe_);
    p_ = nullptr;
}

// Background health checker: runs pool.run_health_checks() every period on its own thread.
class health_checker {
public:
    health_checker(pool& p, std::uint64_t period_ns) : p_(p), period_(period_ns), t_([this] { loop_(); }, "forge-hc") {}
    ~health_checker() { stop_ = true; t_.join(); }
private:
    void loop_() { while (!stop_) { p_.run_health_checks(); for (std::uint64_t s = 0; s < period_ && !stop_; s += 10'000'000) os::sleep_ns(10'000'000); } }
    pool& p_; std::uint64_t period_; std::atomic<bool> stop_{false}; os::thread t_;
};

} // namespace forge::loadbalancer
