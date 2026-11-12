// forge::script managed heap: ARC release, weak cells, forge::mem accounting, cycle collector.
#include "objects.hpp"
#include <algorithm>

namespace forge::script {

Obj::~Obj() { Heap::instance().uncharge(charged); Heap::instance().note_object(-1); }

namespace {
// Each parked object holds one (resurrected) count. A thread that exits with parked objects frees them
// without running their destructors (the interpreter that owned them is gone).
struct FinQueue {
    std::vector<Obj*> v;
    ~FinQueue() { while (!v.empty()) { auto w = std::move(v); v.clear(); for (Obj* o : w) release_obj(o); } }
};
thread_local FinQueue t_fq;
std::vector<Obj*>& tfin() { return t_fq.v; }
}
bool finalizers_pending() noexcept { return !tfin().empty(); }
std::vector<Ref<Obj>> take_finalizers() {
    std::vector<Ref<Obj>> out; auto parked = std::move(tfin()); tfin().clear();
    out.reserve(parked.size());
    for (Obj* o : parked) { out.emplace_back(o); release_obj(o); }   // Ref takes +1, drop the parked count
    return out;
}

void release_obj(Obj* o) noexcept {
    if (o->rc.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (o->finalize) {   // resurrect once so the interpreter can run the script destructor
        o->finalize = false;
        o->rc.fetch_add(1, std::memory_order_relaxed);
        try { tfin().push_back(o); return; } catch (...) { o->rc.fetch_sub(1); }
    }
    if (o->weak) {
        std::lock_guard g(o->weak->m);
        if (o->rc.load(std::memory_order_acquire) != 0) return;   // a weak upgrade won the race
        o->weak->target = nullptr;
    }
    std::unique_ptr<Obj> owner(o);   // last reference: the intrusive count hands ownership back for deletion
}

Value WeakRef::get() const {
    if (!cell) return Nil{};
    std::lock_guard g(cell->m);
    Obj* t = cell->target;
    if (!t) return Nil{};
    std::uint32_t n = t->rc.load(std::memory_order_acquire);
    while (n != 0) { if (t->rc.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel)) break; }
    if (n == 0) return Nil{};
    Ref<Obj> r(t);                     // +1 for the Ref
    release_obj(t);                    // drop the temporary +1 from the CAS
    return Value(std::move(r));
}

namespace { constexpr std::uint64_t kChunk = 1ull << 20; thread_local bool t_grace = false; }

void Heap::grow_lease(std::uint64_t want) {
    std::lock_guard g(lm_);
    if (!lease_) lease_ = mem::host_budget().try_reserve(0, "script");
    if (!lease_) return;
    if (lease_.bytes() >= want) return;
    if (!lease_.resize(want + kChunk)) {
        if (!lease_.resize(want)) throw heap_exhausted("script heap: memory budget exhausted");
    }
}

void Heap::charge(Obj& o, std::size_t n) {
    std::uint64_t now = bytes_.load() + n;
    std::uint64_t cap = cap_.load();
    if (cap && now > cap && !t_grace) {
        if (active_tasks.load() == 0 && !collecting_) collect();
        now = bytes_.load() + n;
        if (now > cap) { t_grace = true; throw heap_exhausted("script heap: limit of " + std::to_string(cap) + " bytes exceeded"); }
    }
    if (cap && now < cap) t_grace = false;
    if (cap && t_grace && now > cap + (4ull << 20)) throw heap_exhausted("script heap: limit exceeded while handling OutOfMemoryError");
    try { grow_lease(now); }
    catch (const heap_exhausted&) {
        if (active_tasks.load() == 0 && !collecting_) { collect(); }
        grow_lease(bytes_.load() + n);   // throws again if still no room
    }
    bytes_.fetch_add(n); o.charged += n;
}

void Heap::uncharge(std::size_t n) noexcept {
    bytes_.fetch_sub(n);
    std::lock_guard g(lm_);
    if (lease_ && lease_.bytes() > bytes_.load() + 4 * kChunk) lease_.resize(bytes_.load() + kChunk);
}

std::size_t Heap::collect() {
    if (collecting_ || active_tasks.load() != 0) return 0;
    collecting_ = true;
    std::vector<Ref<Obj>> garbage;
    {
        std::lock_guard g(m_);
        std::unordered_map<Obj*, std::int64_t> gc;
        gc.reserve(tracked_.size());
        for (Obj* o : tracked_) { auto n = o->rc.load(); if (n) gc[o] = n; }   // rc 0 = being built or destroyed
        for (auto& [o, n] : gc) o->trace([&](Obj& c) { if (auto it = gc.find(&c); it != gc.end()) --it->second; });
        std::unordered_set<Obj*> live; std::vector<Obj*> work;
        for (auto& [o, n] : gc) if (n > 0) { live.insert(o); work.push_back(o); }
        while (!work.empty()) {
            Obj* o = work.back(); work.pop_back();
            o->trace([&](Obj& c) { if (gc.count(&c) && live.insert(&c).second) work.push_back(&c); });
        }
        for (auto& [o, n] : gc) if (!live.count(o)) garbage.emplace_back(o);
    }
    if (on_cycle_garbage) for (auto& o : garbage) { try { on_cycle_garbage(*o); } catch (...) {} }
    for (auto& o : garbage) o->clear();
    std::size_t n = garbage.size();
    garbage.clear();
    collections_.fetch_add(1); collected_.fetch_add(n);
    collecting_ = false;
    return n;
}

} // namespace forge::script
