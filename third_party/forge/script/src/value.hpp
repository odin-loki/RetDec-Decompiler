#pragma once
// forge::script managed heap: intrusive atomic reference counting (ARC), weak cells, heap accounting
// against forge::mem, and the backup cycle collector. Safe ISO C++23: the only ownership transfer between
// std::unique_ptr and the intrusive count lives in make<T>() and release_obj() below.
#include <forge/core/memory.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace forge::script {

struct Obj;
class Heap;

// Non-owning visitor over an object's outgoing strong references (for the cycle collector).
using Visit = std::function<void(Obj&)>;

struct WeakCell {
    std::mutex m;
    Obj* target = nullptr;   // non-owning; cleared under m before the target is destroyed
};

// Ownership mode of a heap object, mirroring the C++ type the script declared:
//   Value  - std::string / std::vector / std::map / class or struct by value: copied on copy-initialisation
//   Shared - std::shared_ptr<T> (std::make_shared): copies share the object (ARC)
//   Unique - std::unique_ptr<T> (std::make_unique): move-only; a copy raises BorrowError
enum class Own : std::uint8_t { Value, Shared, Unique };

struct Obj {
    std::atomic<std::uint32_t> rc{0};
    std::size_t charged = 0;            // bytes counted against the heap
    bool tracked = false;               // participates in cycle collection
    bool frozen = false;                // deep-immutable, shareable across tasks
    Own own = Own::Value;
    bool finalize = false;              // has a script destructor (~T) still to run
    std::atomic<std::uint32_t> borrows{0};   // active range-for borrows (Rust-style: no structural mutation while > 0)
    std::thread::id owner;              // creating / adopting thread (debug race detector)
    std::shared_ptr<WeakCell> weak;     // created on first weak()
    Obj() = default;
    Obj(const Obj&) = delete; Obj& operator=(const Obj&) = delete;
    virtual ~Obj();
    virtual const char* type_name() const = 0;
    virtual void trace(const Visit&) const {}   // strong children
    virtual void clear() {}                      // drop strong children (cycle collector)
};

void release_obj(Obj* o) noexcept;

// Intrusive strong reference. Copy = retain, destroy = release.
template <class T> class Ref {
public:
    Ref() = default;
    Ref(std::nullptr_t) {}
    explicit Ref(T* p) : p_(p) { if (p_) p_->rc.fetch_add(1, std::memory_order_relaxed); }
    Ref(const Ref& o) : Ref(o.p_) {}
    Ref(Ref&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    template <class U> requires std::is_base_of_v<T, U> Ref(const Ref<U>& o) : Ref(o.get()) {}
    Ref& operator=(Ref o) noexcept { std::swap(p_, o.p_); return *this; }
    ~Ref() { if (p_) release_obj(p_); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T& operator*() const { return *p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(const Ref& o) const { return p_ == o.p_; }
    void reset() { Ref().swap_(*this); }
private:
    void swap_(Ref& o) { std::swap(p_, o.p_); }
    T* p_ = nullptr;
};

// Objects whose script destructor must run are parked here by release_obj() instead of being freed,
// and the interpreter runs ~T() at the next statement boundary on the same thread (deterministic RAII).
bool finalizers_pending() noexcept;
std::vector<Ref<Obj>> take_finalizers();

// Thrown by the heap when the script's memory budget is exhausted; mapped to OutOfMemoryError.
struct heap_exhausted : std::runtime_error { using std::runtime_error::runtime_error; };

// Process-wide managed heap. Charges forge::mem::host_budget() in chunks and keeps the registry of
// container objects for the cycle collector.
class Heap {
public:
    static Heap& instance() { static Heap h; return h; }
    void charge(Obj& o, std::size_t n);
    void uncharge(std::size_t n) noexcept;
    void track(Obj& o) { std::lock_guard g(m_); tracked_.insert(&o); o.tracked = true; }
    void untrack(Obj& o) noexcept { std::lock_guard g(m_); tracked_.erase(&o); }
    std::uint64_t live_bytes() const { return bytes_.load(); }
    std::uint64_t live_objects() const { return objects_.load(); }
    std::uint64_t collections() const { return collections_.load(); }
    std::uint64_t collected() const { return collected_.load(); }
    // Script-level cap on top of the budget (limit {mem: ...}); 0 = none.
    void set_soft_cap(std::uint64_t n) { cap_ = n; }
    std::uint64_t soft_cap() const { return cap_; }
    // Run the cycle collector. Only safe when no other task thread is mutating the heap.
    std::size_t collect();
    std::atomic<int> active_tasks{0};
    void note_object(int d) { objects_.fetch_add(std::uint64_t(std::int64_t(d))); }
    std::function<void(Obj&)> on_cycle_garbage;   // dispose hook (runs script dispose())
    // Debug checks (race detector, lock-order / deadlock detector). Off by default; options::debug.
    std::atomic<bool> debug{false};
private:
    Heap() { (void)mem::host_budget(); }   // construct the budget first so it is destroyed after the heap
    void grow_lease(std::uint64_t want);
    std::mutex m_;
    std::unordered_set<Obj*> tracked_;   // non-owning
    std::atomic<std::uint64_t> bytes_{0}, objects_{0}, collections_{0}, collected_{0};
    std::atomic<std::uint64_t> cap_{0};
    std::mutex lm_;
    mem::lease lease_;
    std::atomic<bool> collecting_{false};
};

// limit {allocs: N}: per-thread count of managed-object allocations. `count` only advances while a cap is
// set; `cap` is the absolute count at which the next allocation fails (0 = no cap). The cap is cleared when
// it fires, so the OutOfMemoryError and its handlers can still allocate; the limit scope restores it.
struct AllocQuota {
    static inline thread_local std::uint64_t count = 0, cap = 0;
    static void charge() {
        if (!cap) return;
        if (++count > cap) { cap = 0; throw heap_exhausted("allocation count limit reached"); }
    }
};

template <class T, class... A> Ref<T> make(A&&... a) {
    AllocQuota::charge();
    auto u = std::make_unique<T>(std::forward<A>(a)...);
    static_cast<Obj*>(u.get())->owner = std::this_thread::get_id();
    Heap::instance().note_object(1);
    Ref<T> r(u.release());   // ownership moves into the intrusive count; freed by release_obj()
    Heap::instance().charge(*r, sizeof(T));
    return r;
}

// ---------------------------------------------------------------- values
struct Nil { bool operator==(const Nil&) const = default; };
using Value = std::variant<Nil, bool, std::int64_t, double, Ref<Obj>>;

inline bool is_nil(const Value& v) { return std::holds_alternative<Nil>(v); }
template <class T> T* as(const Value& v) {
    if (auto* r = std::get_if<Ref<Obj>>(&v)) return dynamic_cast<T*>(r->get());
    return nullptr;
}
template <class T> Ref<T> as_ref(const Value& v) { return Ref<T>(as<T>(v)); }
inline Value obj(Ref<Obj> r) { return Value(std::move(r)); }
template <class T> Value obj(const Ref<T>& r) { return Value(Ref<Obj>(r)); }

} // namespace forge::script
