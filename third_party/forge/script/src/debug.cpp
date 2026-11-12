// forge::script debug-mode concurrency checks (options::debug / `forge script --debug`):
//  * race detector  - every mutable heap object has an owning thread; a write from any other thread that
//                     does not hold a forge::shared<T> lock raises RaceError. Values that legally cross
//                     threads (task arguments, channel messages, task results) are adopted by the receiver.
//  * lock order     - forge::shared<T>::with() records "held A, then took B" edges; taking locks in an order
//                     that closes a cycle raises DeadlockError before the threads can actually deadlock.
//  * global stall   - when every script thread is blocked (channel send/recv, await, lock) for longer than
//                     the stall window, the waiter raises DeadlockError instead of hanging forever.
#include "interp.hpp"
#include "objects.hpp"
#include <chrono>
#include <map>
#include <set>
#include <unordered_set>

namespace forge::script::debug {

namespace {
struct State {
    std::mutex m;
    std::map<std::uint64_t, std::set<std::uint64_t>> edges;   // lock-order graph by SharedBox id
    std::atomic<int> threads{1}, blocked{0};
    std::atomic<std::uint64_t> all_blocked_since{0};
};
State& st() { static State s; return s; }
thread_local std::vector<std::uint64_t> t_held;
thread_local int t_locked = 0;
std::uint64_t now_ms() { return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
bool reach(std::uint64_t from, std::uint64_t to, std::set<std::uint64_t>& seen) {
    if (from == to) return true;
    if (!seen.insert(from).second) return false;
    auto it = st().edges.find(from);
    if (it == st().edges.end()) return false;
    for (auto n : it->second) if (reach(n, to, seen)) return true;
    return false;
}
}

bool on() { return Heap::instance().debug.load(std::memory_order_relaxed); }

void check_write(Interp& in, const Obj* o) {
    if (!o || !on() || o->frozen || t_locked > 0) return;
    if (dynamic_cast<const SharedBox*>(o)) return;
    if (o->owner != std::this_thread::get_id())
        in.raise("RaceError", std::string("data race: ") + o->type_name() + " owned by another thread was modified without forge::shared<T> (move it, send it over a channel, or wrap it in forge::shared)");
}

void adopt(const Value& v) {
    if (!on()) return;
    auto* r = std::get_if<Ref<Obj>>(&v); if (!r || !*r) return;
    std::unordered_set<const Obj*> seen; std::vector<Obj*> work{r->get()};
    auto me = std::this_thread::get_id();
    while (!work.empty()) {
        Obj* o = work.back(); work.pop_back();
        if (!seen.insert(o).second || o->frozen || dynamic_cast<SharedBox*>(o)) continue;
        o->owner = me;
        o->trace([&](const Obj& c) { work.push_back(const_cast<Obj*>(&c)); });   // NOLINT: owner tag only
    }
}

void lock_acquire(Interp& in, std::uint64_t lock) {
    if (on()) {
        std::lock_guard g(st().m);
        for (auto h : t_held) {
            if (h == lock) continue;   // recursive
            std::set<std::uint64_t> seen;
            if (reach(lock, h, seen))
                in.raise("DeadlockError", "potential deadlock: forge::shared locks taken in inconsistent order (lock-order inversion)");
        }
        for (auto h : t_held) if (h != lock) st().edges[h].insert(lock);
    }
    t_held.push_back(lock); ++t_locked;
}
void lock_release(std::uint64_t lock) noexcept {
    for (auto it = t_held.rbegin(); it != t_held.rend(); ++it) if (*it == lock) { t_held.erase(std::next(it).base()); break; }
    --t_locked;
}
void thread_started() noexcept { st().threads.fetch_add(1); }
void thread_finished() noexcept { st().threads.fetch_sub(1); st().all_blocked_since = 0; }

Blocked::Blocked(Interp& i, const char* w) : in(i), what(w) { st().blocked.fetch_add(1); }
Blocked::~Blocked() { st().blocked.fetch_sub(1); st().all_blocked_since = 0; }
void Blocked::poll() {
    if (!on()) return;
    auto& s = st();
    if (s.blocked.load() < s.threads.load()) { s.all_blocked_since = 0; return; }
    std::uint64_t t = now_ms(), since = s.all_blocked_since.load();
    if (since == 0) { s.all_blocked_since.compare_exchange_strong(since, t); return; }
    if (t - since >= stall_ms) {
        s.all_blocked_since = 0;
        in.raise("DeadlockError", std::string("deadlock: all ") + std::to_string(s.threads.load()) + " script thread(s) are blocked (this one in " + what + ")");
    }
}

} // namespace forge::script::debug
