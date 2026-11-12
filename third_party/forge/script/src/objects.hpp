#pragma once
// Script object types. Each maps directly onto a standard or FORGE C++ type: string -> std::string,
// list -> std::vector<Value>, map -> insertion-ordered vector + hash index, handle -> an RAII C++ owner.
#include "value.hpp"
#include <condition_variable>
#include <deque>
#include <optional>
#include <thread>

namespace forge::script {

struct Node;     // AST (ast.hpp)
struct FnDecl;
struct Chunk;    // bytecode (vm.hpp)
struct ClassMeta;   // reflection metadata (ast.hpp)
struct Interp;

struct String final : Obj {
    std::string s;
    explicit String(std::string v) : s(std::move(v)) { Heap::instance().charge(*this, s.size()); }
    const char* type_name() const override { return "string"; }
};

struct List final : Obj {
    std::vector<Value> items;
    List() { Heap::instance().track(*this); }
    explicit List(std::vector<Value> v) : items(std::move(v)) { Heap::instance().track(*this); Heap::instance().charge(*this, items.size() * sizeof(Value)); }
    ~List() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "list"; }
    void push(Value v) { Heap::instance().charge(*this, sizeof(Value)); items.push_back(std::move(v)); }
    void trace(const Visit& f) const override { for (auto& v : items) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); }
    void clear() override { items.clear(); }
};

// Map keys: nil/bool/int/float/string; encoded to a string for the hash index.
std::optional<std::string> key_of(const Value& v);

struct Map final : Obj {
    std::vector<std::pair<Value, Value>> entries;
    std::unordered_map<std::string, std::size_t> index;
    bool sorted = false;                 // std::map (ordered by key) vs std::unordered_map (insertion order)
    bool has_dflt = false; Value dflt;
    std::string enum_name;               // non-empty: the frozen value table of `enum [class] enum_name`
    bool is_set = false;                 // std::set / unordered_set: iteration yields keys   // operator[] inserts a copy of dflt for a missing key (std::map semantics)
    Map() { Heap::instance().track(*this); }
    ~Map() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "map"; }
    const Value* find(const Value& k) const;
    void set(const Value& k, Value v);
    bool erase(const Value& k);
    void set_str(const std::string& k, Value v);
    const Value* find_str(const std::string& k) const;
    void trace(const Visit& f) const override {
        for (auto& [k, v] : entries) { if (auto* r = std::get_if<Ref<Obj>>(&k)) if (*r) f(**r); if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); }
        if (auto* r = std::get_if<Ref<Obj>>(&dflt)) if (*r) f(**r);
    }
    void clear() override { entries.clear(); index.clear(); dflt = Nil{}; }
};

// Global variables of one program/module/REPL session.
struct Globals final : Obj {
    std::unordered_map<std::string, Value> vars;
    std::unordered_set<std::string> consts;
    std::vector<std::string> using_ns;   // `using namespace X;` prefixes tried for unqualified names
    Globals() { Heap::instance().track(*this); }
    ~Globals() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "globals"; }
    void trace(const Visit& f) const override { for (auto& [k, v] : vars) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); }
    void clear() override { vars.clear(); }
};

// One activation's local slots; parent = the enclosing function's activation (closures).
struct Env final : Obj {
    std::vector<Value> slots;
    Ref<Env> parent;
    explicit Env(std::size_t n, Ref<Env> p) : slots(n), parent(std::move(p)) { Heap::instance().track(*this); Heap::instance().charge(*this, n * sizeof(Value)); }
    ~Env() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "env"; }
    void trace(const Visit& f) const override { for (auto& v : slots) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); if (parent) f(*parent); }
    void clear() override { slots.clear(); parent.reset(); }
};

struct Class;

struct Function final : Obj {
    std::shared_ptr<const FnDecl> decl;     // immutable AST, shared across tasks
    Ref<Env> env;                           // captured activation
    Ref<Globals> globals;
    Ref<Class> owner;                       // for methods (super lookup)
    mutable std::shared_ptr<const Chunk> chunk;   // compiled lazily
    mutable std::atomic<int> calls{0};
    Function(std::shared_ptr<const FnDecl> d, Ref<Env> e, Ref<Globals> g) : decl(std::move(d)), env(std::move(e)), globals(std::move(g)) { Heap::instance().track(*this); }
    ~Function() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "fn"; }
    void trace(const Visit& f) const override;
    void clear() override;
};

struct Native final : Obj {
    using fn_t = std::function<Value(Interp&, std::vector<Value>&)>;
    std::string name; fn_t fn; int min_args = 0, max_args = -1;
    Native(std::string n, fn_t f, int lo, int hi) : name(std::move(n)), fn(std::move(f)), min_args(lo), max_args(hi) {}
    const char* type_name() const override { return "fn"; }
};

struct Class final : Obj {
    std::string name;
    Ref<Class> base;
    std::vector<std::string> fields;                  // declared fields in order
    std::vector<Value> field_defaults;                // evaluated when the class is defined (literals)
    std::vector<const struct Node*> field_inits;      // non-literal default member initialisers: evaluated per object
    Ref<Env> def_env;
    std::unordered_map<std::string, Value> methods;   // name -> Function
    std::unordered_map<std::string, Value> statics;
    std::vector<std::string> interfaces;
    bool is_struct = false, is_dynamic = false, is_interface = false;
    std::vector<std::string> required;                // interface: method names
    std::shared_ptr<const ClassMeta> meta;            // fields' types, const, access, attributes (reflection)
    Class() { Heap::instance().track(*this); }
    ~Class() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return is_interface ? "interface" : "class"; }
    const Value* find_method(const std::string& n) const {
        for (const Class* c = this; c; c = c->base.get()) { auto it = c->methods.find(n); if (it != c->methods.end()) return &it->second; }
        return nullptr;
    }
    bool has_field(const std::string& n) const {
        for (const Class* c = this; c; c = c->base.get()) for (auto& f : c->fields) if (f == n) return true;
        return false;
    }
    bool derives(const std::string& n) const {
        for (const Class* c = this; c; c = c->base.get()) { if (c->name == n) return true; for (auto& i : c->interfaces) if (i == n) return true; }
        return false;
    }
    void trace(const Visit& f) const override {
        if (base) f(*base);
        if (def_env) f(*def_env);
        for (auto& v : field_defaults) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r);
        for (auto& [k, v] : methods) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r);
        for (auto& [k, v] : statics) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r);
    }
    void clear() override { base.reset(); def_env.reset(); methods.clear(); statics.clear(); field_defaults.clear(); }
};

struct Instance final : Obj {
    Ref<Class> cls;
    std::unordered_map<std::string, Value> fields;
    bool disposed = false;
    explicit Instance(Ref<Class> c) : cls(std::move(c)) { Heap::instance().track(*this); }
    ~Instance() override;
    const char* type_name() const override { return cls->is_struct ? "struct" : "object"; }
    void trace(const Visit& f) const override { if (cls) f(*cls); for (auto& [k, v] : fields) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); }
    void clear() override { fields.clear(); }
};

struct Bound final : Obj {
    Value self, fn;
    Bound(Value s, Value f) : self(std::move(s)), fn(std::move(f)) { Heap::instance().track(*this); }
    ~Bound() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "fn"; }
    void trace(const Visit& f) const override { for (auto* v : {&self, &fn}) if (auto* r = std::get_if<Ref<Obj>>(v)) if (*r) f(**r); }
    void clear() override { self = Nil{}; fn = Nil{}; }
};

struct WeakRef final : Obj {
    std::shared_ptr<WeakCell> cell;
    explicit WeakRef(std::shared_ptr<WeakCell> c) : cell(std::move(c)) {}
    const char* type_name() const override { return "weak"; }
    Value get() const;
};

// limit {handles: N} / limit {procs: N}: a counter shared by every interpreter (task) running inside one limit
// block. Nested blocks chain to the enclosing block's quota, so an acquire counts against all of them.
struct Quota {
    std::atomic<std::int64_t> used{0};
    std::int64_t max = 0;
    std::shared_ptr<Quota> parent;
    Quota(std::int64_t m, std::shared_ptr<Quota> p) : max(m), parent(std::move(p)) {}
    // Counts one use against every quota in the chain. Returns the full quota (and counts nothing) on failure.
    static const Quota* acquire(const std::shared_ptr<Quota>& q) {
        for (Quota* p = q.get(); p; p = p->parent.get()) {
            if (p->used.fetch_add(1) + 1 > p->max) {
                for (Quota* r = q.get(); ; r = r->parent.get()) { r->used.fetch_sub(1); if (r == p) break; }
                return p;
            }
        }
        return nullptr;
    }
    static void release(const std::shared_ptr<Quota>& q) noexcept { for (Quota* p = q.get(); p; p = p->parent.get()) p->used.fetch_sub(1); }
};

// OS resource owned by a C++ RAII object. dispose() is deterministic; destruction also releases it.
struct Handle final : Obj {
    std::string kind;
    std::function<void()> closer;        // releases the OS resource exactly once
    std::shared_ptr<void> payload;       // the C++ owner (type-erased, typed access via payload_as)
    std::unordered_map<std::string, Value> props;
    bool disposed = false;
    std::shared_ptr<Quota> quota;        // limit {handles: N} this handle counts against (released on dispose)
    explicit Handle(std::string k) : kind(std::move(k)) { Heap::instance().track(*this); }
    ~Handle() override { dispose(); Heap::instance().untrack(*this); }
    const char* type_name() const override { return "handle"; }
    void dispose() noexcept {
        if (!disposed) { disposed = true; if (closer) { try { closer(); } catch (...) {} } closer = nullptr; payload.reset(); }
        if (quota) { Quota::release(quota); quota.reset(); }
    }
    template <class T> std::shared_ptr<T> payload_as() const { return std::static_pointer_cast<T>(payload); }
    // Script values a handle keeps (callbacks of pipelines / schedulers) live in props, visible to the cycle collector.
    void trace(const Visit& f) const override { for (auto& [k, v] : props) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); }
    void clear() override { props.clear(); }
};

// Result slot shared between a task thread and its awaiting owner. Values crossing threads are deep-copied.
struct TaskState {
    std::mutex m; std::condition_variable cv;
    bool done = false, failed = false;
    Value result; std::string error_type, error_msg;
    std::atomic<bool> cancel{false};
};
struct Task final : Obj {
    std::shared_ptr<TaskState> st;
    std::thread th;
    Task() : st(std::make_shared<TaskState>()) {}
    ~Task() override { if (th.joinable()) { st->cancel = true; th.join(); } }
    const char* type_name() const override { return "task"; }
};

// An iterator position (v.begin(), v.end(), m.find(k)): the container plus an index. No raw pointers.
struct PosIt final : Obj {
    Value c; std::int64_t i = 0;
    PosIt(Value cont, std::int64_t idx) : c(std::move(cont)), i(idx) { Heap::instance().track(*this); }
    ~PosIt() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "iterator"; }
    void trace(const Visit& f) const override { if (auto* r = std::get_if<Ref<Obj>>(&c)) if (*r) f(**r); }
    void clear() override { c = Nil{}; }
};

// A range adaptor (std::views::filter(f), transform, take, ...): applied eagerly by `range | adaptor`.
struct Adaptor final : Obj {
    std::string kind; std::vector<Value> args;
    Adaptor(std::string k, std::vector<Value> a) : kind(std::move(k)), args(std::move(a)) { Heap::instance().track(*this); }
    ~Adaptor() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "view"; }
    void trace(const Visit& f) const override { for (auto& v : args) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); }
    void clear() override { args.clear(); }
};

// std::cout / std::cerr: `os << x` formats x and writes it.
struct Ostream final : Obj {
    bool err = false, boolalpha = false, fixed = false; int precision = -1, width = 0;
    explicit Ostream(bool e) : err(e) {}
    const char* type_name() const override { return "ostream"; }
};

// The value left behind by std::move(x): reading x again raises BorrowError (Rust-style use-after-move).
struct Moved final : Obj { const char* type_name() const override { return "moved-from"; } };
Obj* moved_sentinel();
inline bool is_moved(const Value& v) { auto* r = std::get_if<Ref<Obj>>(&v); return r && r->get() == moved_sentinel(); }

// forge::shared<T>(value): the one kind of mutable object several threads may hold at once. Every
// access takes its lock automatically; in debug mode lock order is tracked to report deadlocks.
struct SharedBox final : Obj {
    std::recursive_timed_mutex m;
    const std::uint64_t id = [] { static std::atomic<std::uint64_t> n{0}; return ++n; }();   // lock-order graph key
    Value v;
    explicit SharedBox(Value x) : v(std::move(x)) { own = Own::Shared; Heap::instance().track(*this); }
    ~SharedBox() override { Heap::instance().untrack(*this); }
    const char* type_name() const override { return "shared"; }
    void trace(const Visit& f) const override { if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) f(**r); }
    void clear() override { v = Nil{}; }
};

struct ChannelState {
    std::mutex m; std::condition_variable cv;
    std::deque<Value> q; std::size_t cap = 0; bool closed = false;
};
struct Channel final : Obj {
    std::shared_ptr<ChannelState> st;
    explicit Channel(std::size_t cap) : st(std::make_shared<ChannelState>()) { st->cap = cap; }
    const char* type_name() const override { return "channel"; }
};

} // namespace forge::script
