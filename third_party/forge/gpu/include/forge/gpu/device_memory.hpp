#pragma once
// Device memory accounting shared by the GPU runtimes (docs/memory.md, "GPU memory").
//
//   * Every driver allocation a runtime makes is charged to the governor pool "<backend>:<ordinal>" ("cuda:0",
//     "vulkan:0", ...; limit from FORGE_MEM_DEVICE_LIMIT or budget::set_limit) with one lease per allocation,
//     released when the allocation is freed. With no limit the pool only keeps accounts.
//   * The runtime registers itself with that pool as a cache. When an allocation (its own, or another runtime's
//     on the same pool) would cross the limit, or the driver itself reports out of memory, the runtime evicts
//     device copies: pooled free buffers first, then resident tensors least recently used first. A resident tensor
//     is synced back to the host before it is freed, so its next use uploads it again: FORGE_GPU_NO_RESIDENCY
//     behaviour, applied on demand to just enough buffers. Buffers bound by the op in progress are never evicted.
//     Only when nothing more can be evicted does the allocation fail, with errc::out_of_memory.
//
// Locking: the runtime calls allocate()/release() with its own mutex held. The pool's evict callback runs either
// on that same thread inside allocate() (the lock is already held: evict directly) or on another thread (pressure
// response, another runtime on the same pool): then it only try_locks the runtime, so two runtimes evicting each
// other can never deadlock; a busy runtime is skipped.
#include <forge/core/memory.hpp>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace forge::gpu {

class device_memory {
public:
    // evict(want, external): free about `want` bytes of device memory (0 = everything evictable), with the
    // runtime's lock held; returns the bytes freed. `external` is true when no op of this runtime is in progress
    // (another thread's request), so every buffer may go.
    using evict_fn = std::function<std::uint64_t(std::uint64_t want, bool external)>;

    device_memory() = default;
    device_memory(const device_memory&) = delete; device_memory& operator=(const device_memory&) = delete;
    ~device_memory() { detach(); }

    // Start charging pool `name` (priority: lower is evicted before other caches of the same pool).
    void attach(const std::string& name, std::mutex& mu, evict_fn ev, int priority = 5) {
        detach();
        pool_ = &mem::governor::instance().pool(name);
        st_ = std::make_shared<state>(); st_->mu = &mu; st_->ev = std::move(ev); st_->self = this;
        std::weak_ptr<state> w = st_;
        id_ = pool_->add_cache(name + " resident", nullptr, [w](std::uint64_t want) -> std::uint64_t {
            auto s = w.lock(); if (!s) return 0;
            std::lock_guard g(s->m);
            if (!s->alive) return 0;
            if (owner() == s->self) return s->ev(want, false);   // inside our own allocate(): lock already held
            std::unique_lock lk(*s->mu, std::try_to_lock);
            if (!lk.owns_lock()) return 0;                         // busy: skip rather than risk a lock cycle
            return s->ev(want, true);
        }, priority);
    }
    // Stop charging and unregister; releases every lease (call before freeing everything in a destructor).
    void detach() {
        if (st_) { std::lock_guard g(st_->m); st_->alive = false; }
        if (pool_ && id_) pool_->remove_cache(id_);
        id_ = 0; st_.reset(); leases_.clear();
    }
    mem::budget* pool() const { return pool_; }
    // Bytes this runtime has charged (live driver allocations).
    std::uint64_t charged() const { std::uint64_t s = 0; for (auto& [k, l] : leases_) s += l.bytes(); return s; }

    // Allocate `bytes` through `try_alloc` (returns std::optional<handle>: empty = the driver is out of memory).
    // Charges the pool first (evicting to make room), and on a driver failure evicts this runtime's buffers and
    // retries. Throws errc::out_of_memory when neither helps. The handle keys the charge for release().
    template<class F> auto allocate(std::uint64_t bytes, const char* tag, F&& try_alloc) -> typename std::invoke_result_t<F&>::value_type {
        mem::lease l;
        if (pool_) {
            scope sc(this);
            l = pool_->try_reserve(bytes, tag);   // evicts caches of this pool (ours LRU-first) to fit
            if (!l) {
                auto s = pool_->stats();
                raise(errc::out_of_memory, "{}: cannot allocate {} for {} ({} of {} in use after evicting resident buffers; raise FORGE_MEM_DEVICE_LIMIT)",
                      pool_->name(), mem::format_size(bytes), tag, mem::format_size(s.used), mem::format_size(s.limit));
            }
        }
        for (;;) {
            if (auto h = try_alloc()) { if (l) leases_.insert_or_assign(key(*h), std::move(l)); return *h; }
            scope sc(this);
            if (!st_ || !st_->ev || !st_->ev(bytes, false)) break;   // the driver is full: evict our own buffers, retry
        }
        raise(errc::out_of_memory, "{}: the device is out of memory allocating {} for {} (nothing left to evict)",
              pool_ ? pool_->name() : std::string("gpu"), mem::format_size(bytes), tag);
    }
    // The allocation keyed by `h` was freed.
    template<class H> void release(H h) { leases_.erase(key(h)); }

private:
    struct state { std::mutex m; std::mutex* mu = nullptr; evict_fn ev; const device_memory* self = nullptr; bool alive = true; };
    static const device_memory*& owner() { thread_local const device_memory* o = nullptr; return o; }
    struct scope {   // marks this thread as inside allocate() of `d` (its runtime lock is held)
        const device_memory* prev;
        explicit scope(const device_memory* d) : prev(std::exchange(owner(), d)) {}
        ~scope() { owner() = prev; }
    };
    template<class H> static std::uint64_t key(H h) {
        if constexpr (std::is_pointer_v<H>) return std::uint64_t(reinterpret_cast<std::uintptr_t>(h));
        else return std::uint64_t(h);
    }
    mem::budget* pool_ = nullptr; int id_ = 0;
    std::shared_ptr<state> st_;
    std::unordered_map<std::uint64_t, mem::lease> leases_;
};

} // namespace forge::gpu
