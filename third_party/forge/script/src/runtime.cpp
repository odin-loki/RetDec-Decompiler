#include <forge/os/oserror.hpp>
// forge::script runtime operations shared by the tree-walker and the VM.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>   // _AddressOfReturnAddress
#endif
#include "interp.hpp"
#include "shells/shell.hpp"
#include "parser.hpp"
#include "vm.hpp"
#include <forge/core/error.hpp>
#include <forge/os/os.hpp>
#include <forge/os/process.hpp>
#include <forge/security/errors.hpp>
#include <forge/security/enforce.hpp>
#include <forge/security/integrity.hpp>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>

namespace forge::script {

// ---------------------------------------------------------------- objects
std::optional<std::string> key_of(const Value& v) {
    if (is_nil(v)) return std::string("n");
    if (auto* b = std::get_if<bool>(&v)) return std::string(*b ? "bt" : "bf");
    if (auto* i = std::get_if<std::int64_t>(&v)) return "i" + std::to_string(*i);
    if (auto* d = std::get_if<double>(&v)) { if (*d == std::floor(*d) && std::abs(*d) < 9e18) return "i" + std::to_string(std::int64_t(*d)); return std::format("f{}", *d); }
    if (auto* s = as<String>(v)) return "s" + s->s;
    return std::nullopt;
}
const Value* Map::find(const Value& k) const { auto key = key_of(k); if (!key) return nullptr; auto it = index.find(*key); return it == index.end() ? nullptr : &entries[it->second].second; }
const Value* Map::find_str(const std::string& k) const { auto it = index.find("s" + k); return it == index.end() ? nullptr : &entries[it->second].second; }
void Map::set(const Value& k, Value v) {
    auto key = key_of(k);
    if (!key) throw std::invalid_argument("map keys must be nil, bool, numbers or strings");
    if (auto it = index.find(*key); it != index.end()) { entries[it->second].second = std::move(v); return; }
    Heap::instance().charge(*this, 2 * sizeof(Value) + key->size() + 32);
    index[*key] = entries.size(); entries.emplace_back(k, std::move(v));
}
void Map::set_str(const std::string& k, Value v) {
    if (auto it = index.find("s" + k); it != index.end()) { entries[it->second].second = std::move(v); return; }
    Value kv(Ref<Obj>(make<String>(k)));
    Heap::instance().charge(*this, 2 * sizeof(Value) + k.size() + 32);
    index["s" + k] = entries.size(); entries.emplace_back(std::move(kv), std::move(v));
}
bool Map::erase(const Value& k) {
    auto key = key_of(k); if (!key) return false;
    auto it = index.find(*key); if (it == index.end()) return false;
    entries.erase(entries.begin() + std::ptrdiff_t(it->second));
    index.clear();
    for (std::size_t i = 0; i < entries.size(); ++i) index[*key_of(entries[i].first)] = i;
    return true;
}
void Function::trace(const Visit& f) const { if (env) f(*env); if (globals) f(*globals); if (owner) f(*owner); }
void Function::clear() { env.reset(); globals.reset(); owner.reset(); }
Instance::~Instance() { Heap::instance().untrack(*this); }

// ---------------------------------------------------------------- iterators
namespace {
struct Iter final : Obj {
    Value src; std::size_t i = 0; std::int64_t a = 0, b = 0, stp = 1; std::size_t last = 0; int kind = 0;   // 0 list, 1 map, 2 string, 3 range, 4 channel
    std::vector<Value> keys;
    const char* type_name() const override { return "iterator"; }
    Iter() { Heap::instance().track(*this); }
    ~Iter() override { if (auto* r = std::get_if<Ref<Obj>>(&src); r && *r && kind <= 2) (*r)->borrows.fetch_sub(1); Heap::instance().untrack(*this); }
    void trace(const Visit& f) const override { if (auto* r = std::get_if<Ref<Obj>>(&src)) if (*r) f(**r); for (auto& k : keys) if (auto* r = std::get_if<Ref<Obj>>(&k)) if (*r) f(**r); }
    void clear() override { src = Nil{}; keys.clear(); }
};
}
struct Range final : Obj { std::int64_t a, b, step; Range(std::int64_t x, std::int64_t y, std::int64_t s) : a(x), b(y), step(s) {} const char* type_name() const override { return "range"; } };
Value make_range(std::int64_t a, std::int64_t b, std::int64_t s) { return obj(make<Range>(a, b, s)); }

Ref<Obj> Interp::iterate(const Value& v) {
    auto it = make<Iter>();
    if (as<List>(v)) { it->kind = 0; it->src = v; }
    else if (auto* m = as<Map>(v)) { it->kind = 1; it->src = v; for (auto& [k, x] : m->entries) it->keys.push_back(k); if (m->sorted) sort_keys(it->keys); }
    else if (as<String>(v)) { it->kind = 2; it->src = v; }
    else if (auto* r = as<Range>(v)) { it->kind = 3; it->a = r->a; it->b = r->b; it->stp = r->step; }
    else if (as<Channel>(v)) { it->kind = 4; it->src = v; }
    else if (auto* g = as<Globals>(v)) { it->kind = 1; auto m = make<Map>(); for (auto& [k, x] : g->vars) m->set_str(k, x); it->src = obj(m); for (auto& [k, x] : m->entries) it->keys.push_back(k); }
    else raise("TypeError", "cannot iterate over " + type_of(v));
    if (auto* r = std::get_if<Ref<Obj>>(&it->src); r && *r && it->kind <= 2) (*r)->borrows.fetch_add(1);   // range-for borrow
    return Ref<Obj>(it);
}
void Interp::iter_writeback(Obj& o, const Value& a, const Value* b) {
    auto* it = dynamic_cast<Iter*>(&o);
    if (!it || it->i == 0) return;
    if (it->kind == 0) {
        auto* l = as<List>(it->src); if (!l || it->last >= l->items.size()) return;
        if (b) { if (auto* p = as<List>(l->items[it->last]); p && p->items.size() >= 2) { p->items[0] = a; p->items[1] = *b; } }
        else l->items[it->last] = a;
    } else if (it->kind == 1) {
        auto* m = as<Map>(it->src); if (!m || m->is_set) return;
        const Value& k = it->keys[it->i - 1];
        if (b) { if (m->find(k)) m->set(k, *b); }
        else if (auto* p = as<List>(a); p && p->items.size() >= 2 && m->find(k)) m->set(k, p->items[1]);
    } else if (it->kind == 2) {
        auto* s = as<String>(it->src); auto* c = as<String>(b ? *b : a);
        if (s && c && it->last < s->s.size() && !c->s.empty()) s->s[it->last] = c->s[0];
        else if (s && it->last < s->s.size()) if (auto* x = std::get_if<std::int64_t>(b ? b : &a)) s->s[it->last] = char(*x);
    }
}
bool Interp::iter_next(Obj& o, Value& out, Value* out2) {
    auto* it = dynamic_cast<Iter*>(&o);
    if (!it) raise("TypeError", "not an iterator");
    switch (it->kind) {
    case 0: { auto* l = as<List>(it->src); if (!l || it->i >= l->items.size()) return false;
              if (out2) {   // for (auto [a, b] : pairs) destructures each element
                  auto* p = as<List>(l->items[it->i]);
                  if (!p || p->items.size() < 2) raise("TypeError", "structured binding needs a pair/tuple element, got " + type_of(l->items[it->i]));
                  out = p->items[0]; *out2 = p->items[1];
              } else out = l->items[it->i];
              it->last = it->i; ++it->i; return true; }
    case 1: { auto* m = as<Map>(it->src);
              while (it->i < it->keys.size()) {
                  auto& k = it->keys[it->i++];
                  if (auto* x = m->find(k)) {
                      if (out2) { out = k; *out2 = *x; }
                      else if (m->is_set) out = k;
                      else { auto p = make<List>(); p->push(k); p->push(*x); out = obj(p); }   // std::pair<const K, V>
                      return true;
                  }
              }
              return false; }
    case 2: { auto* s = as<String>(it->src); if (!s || it->i >= s->s.size()) return false;
              std::size_t n = 1; unsigned char c = static_cast<unsigned char>(s->s[it->i]);
              if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
              n = std::min(n, s->s.size() - it->i);
              Value ch = new_string(s->s.substr(it->i, n)); it->last = it->i;
              if (out2) { out = std::int64_t(it->i); *out2 = ch; } else out = ch;
              it->i += n; return true; }
    case 3: { if ((it->stp > 0 && it->a >= it->b) || (it->stp < 0 && it->a <= it->b)) return false;
              if (out2) { out = std::int64_t(it->i); *out2 = it->a; } else out = it->a; it->a += it->stp; ++it->i; return true; }
    case 4: { auto* ch = as<Channel>(it->src); auto& st = *ch->st;
              std::unique_lock g(st.m);
              { debug::Blocked w(*this, "channel receive");
                while (st.q.empty() && !st.closed) { st.cv.wait_for(g, std::chrono::milliseconds(50)); if (sh->interrupt) { g.unlock(); step(); g.lock(); } if (st.q.empty() && !st.closed) w.poll(); } }
              if (st.q.empty()) return false;
              Value x = std::move(st.q.front()); st.q.pop_front(); st.cv.notify_all(); debug::adopt(x);
              if (out2) { out = std::int64_t(it->i); *out2 = std::move(x); } else out = std::move(x); ++it->i; return true; }
    }
    return false;
}

// ---------------------------------------------------------------- strings
std::string Interp::type_of(const Value& v) {
    switch (v.index()) { case 0: return "nil"; case 1: return "bool"; case 2: return "int"; case 3: return "float"; }
    auto& r = std::get<Ref<Obj>>(v);
    if (!r) return "nil";
    if (auto* i = dynamic_cast<Instance*>(r.get())) return i->cls->name;
    return r->type_name();
}
static std::string fmt_float(double d) {   // std::format("{}", d): shortest round-trip, no forced ".0"
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    return std::format("{}", d);
}
std::string Interp::repr(const Value& v, int depth) {
    if (auto* s = as<String>(v)) {
        std::string o = "\"";
        for (char c : s->s) { if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t"; else o += c; }
        return o + "\"";
    }
    return str(v, depth);
}
std::string Interp::str(const Value& v, int depth) {
    if (depth > 32) return "...";
    switch (v.index()) {
    case 0: return "nil";
    case 1: return std::get<bool>(v) ? "true" : "false";
    case 2: return std::to_string(std::get<std::int64_t>(v));
    case 3: return fmt_float(std::get<double>(v));
    }
    auto& r = std::get<Ref<Obj>>(v);
    if (!r) return "nil";
    Obj* o = r.get();
    if (auto* s = dynamic_cast<String*>(o)) return s->s;
    if (auto* l = dynamic_cast<List*>(o)) {
        std::string s = "["; bool first = true;
        for (auto& x : l->items) { if (!first) s += ", "; first = false; s += repr(x, depth + 1); }
        return s + "]";
    }
    if (auto* m = dynamic_cast<Map*>(o)) {
        std::string s = "{"; bool first = true;
        for (auto& [k, x] : m->entries) { if (!first) s += ", "; first = false; s += as<String>(k) ? str(k) : "[" + repr(k) + "]"; s += ": "; s += repr(x, depth + 1); }
        return s + "}";
    }
    if (auto* f = dynamic_cast<Function*>(o)) return "<fn " + f->decl->name + ">";
    if (auto* n = dynamic_cast<Native*>(o)) return "<native " + n->name + ">";
    if (auto* b = dynamic_cast<Bound*>(o)) { (void)b; return "<bound method>"; }
    if (auto* c = dynamic_cast<Class*>(o)) return std::string("<") + c->type_name() + " " + c->name + ">";
    if (auto* i = dynamic_cast<Instance*>(o)) {
        if (i->cls->derives("Error")) return error_message(v);
        if (auto* m = i->cls->find_method("str")) { std::vector<Value> a; Value self = v; Value bm = obj(make<Bound>(self, *m)); return str(call(bm, a), depth + 1); }
        std::string s = i->cls->name + "{"; bool first = true;
        for (const Class* c = i->cls.get(); c; c = c->base.get()) for (auto& f : c->fields) {
            auto it = i->fields.find(f); if (it == i->fields.end()) continue;
            if (!first) s += ", "; first = false; s += f + ": " + repr(it->second, depth + 1);
        }
        return s + "}";
    }
    if (auto* h = dynamic_cast<Handle*>(o)) return "<" + h->kind + (h->disposed ? " (disposed)>" : ">");
    if (auto* g = dynamic_cast<Range*>(o)) return std::format("range({}, {}, {})", g->a, g->b, g->step);
    if (dynamic_cast<Globals*>(o)) return "<module>";
    return std::string("<") + o->type_name() + ">";
}

// ---------------------------------------------------------------- errors
Value Interp::make_error(const std::string& type, const std::string& msg) {
    Ref<Class> c;
    if (auto it = sh->errors.find(type); it != sh->errors.end()) c = it->second; else c = sh->errors.at("Error");
    auto e = make<Instance>(c);
    e->fields["message"] = new_string(msg);
    e->fields["trace"] = new_string(trace_text());
    e->fields["line"] = std::int64_t(line);
    return obj(e);
}
void Interp::raise(const std::string& type, const std::string& msg) {
    Value e = make_error(type, msg);
    throw ScriptError(e, type + ": " + msg);
}
std::string Interp::error_message(const Value& err) {
    if (auto* i = as<Instance>(err)) {
        std::string m; if (auto it = i->fields.find("message"); it != i->fields.end()) m = str(it->second);
        return i->cls->name + (m.empty() ? "" : ": " + m);
    }
    return str(err);
}
std::string Interp::trace_text() const {
    std::string s;
    for (auto it = frames.rbegin(); it != frames.rend() && s.size() < 4000; ++it) s += "  at " + it->first + " (" + file + ":" + std::to_string(it->second) + ")\n";
    return s;
}
ScriptError Interp::from_cpp(std::exception_ptr ep) {
    auto mk = [&](const std::string& t, const std::string& m) { Value e = make_error(t, m); return ScriptError(e, t + ": " + m); };
    try { std::rethrow_exception(ep); }
    catch (const ScriptError& e) { return e; }
    catch (const heap_exhausted& e) { return mk("OutOfMemoryError", e.what()); }
    catch (const std::bad_alloc&) { return mk("OutOfMemoryError", "out of memory"); }
    catch (const syntax_error& e) { return mk("SyntaxError", e.what()); }
    catch (const security::security_error& e) {   // FSS-ERR-1
        switch (e.sc) {
        case security::sec_code::integrity: return mk("IntegrityError", e.what());
        case security::sec_code::policy: return mk("PolicyViolation", e.what());
        case security::sec_code::permission: return mk("PermissionError", e.what());
        case security::sec_code::sandbox: return mk("SandboxViolationError", e.what());
        }
        return mk("SecurityError", e.what());
    }
    catch (const os::os_exception& e) { return mk(e.script_type(), e.what()); }   // typed OS errors, faults, crashed workers
    catch (const forge_exception& e) {
        switch (e.code) {
        case errc::out_of_memory: return mk("OutOfMemoryError", e.what());
        case errc::io_error: case errc::not_found: return mk("IOError", e.what());
        case errc::invalid_argument: case errc::parse_error: case errc::format_error: return mk("ValueError", e.what());
        case errc::unsupported: return mk("NotSupportedError", e.what());
        default: return mk("Error", e.what());
        }
    }
    catch (const std::out_of_range& e) { return mk("IndexError", e.what()); }
    catch (const std::invalid_argument& e) { return mk("ValueError", e.what()); }
    catch (const std::exception& e) { return mk("Error", e.what()); }
    catch (...) { return mk("Error", "unknown native error"); }
}

// ---------------------------------------------------------------- limits
void Interp::step() {
    if (finalizers_pending()) run_finalizers();
    auto n = sh->steps.fetch_add(1, std::memory_order_relaxed);
    if (sh->limits.max_steps && n >= sh->limits.max_steps) raise("LimitExceededError", "step limit of " + std::to_string(sh->limits.max_steps) + " reached");
    if ((n & 255) == 0) {
        if (sh->interrupt.load()) {
            sh->interrupt = false;
            int sig = os::take_cancel_signal();
            raise("CancelledError", sig ? os::cancellation_exception(sig).what() : std::string("interrupted"));
        }
        if (task && task->cancel.load()) raise("CancelledError", "task cancelled");
        if (deadline_ns && os::monotonic_ns() > deadline_ns) { deadline_ns = 0; raise("TimeoutError", "time limit reached"); }
        if (cpu_deadline_ns && os::thread_cpu_time_ns() > cpu_deadline_ns) { cpu_deadline_ns = 0; raise("LimitExceededError", "CPU time limit reached"); }
    }
}
void Interp::charge_process(const std::string& what) {
    if (!proc_quota) return;
    if (const Quota* full = Quota::acquire(proc_quota))
        raise("LimitExceededError", "child-process limit of " + std::to_string(full->max) + " reached (not starting " + what + ")");
}

// ---------------------------------------------------------------- variables
Value Interp::load(const VarRef& v, const Ref<Env>& env) {
    if (v.global()) {
        if (auto it = globals->vars.find(v.name); it != globals->vars.end()) { if (is_moved(it->second)) raise("BorrowError", "use of moved-from '" + v.name + "'"); return it->second; }
        if (auto it = sh->builtins.find(v.name); it != sh->builtins.end()) return it->second;
        return load_qualified(v.name);
    }
    Env* e = env.get();
    for (int d = v.depth; d > 0 && e; --d) e = e->parent.get();
    if (!e) raise("NameError", "bad scope for '" + v.name + "'");
    const Value& x = e->slots.at(std::size_t(v.slot));
    if (is_moved(x)) raise("BorrowError", "use of moved-from '" + v.name + "'");
    return x;
}
// Unresolved global: `using namespace` prefixes, then `head::member` (namespace, class static, enum, module).
Value Interp::load_qualified(const std::string& name) {
    if (name.find("::") == std::string::npos) {
        for (auto& ns : globals->using_ns) {
            std::string q = ns + "::" + name;
            if (auto it = globals->vars.find(q); it != globals->vars.end()) return it->second;
            if (auto it = sh->builtins.find(q); it != sh->builtins.end()) return it->second;
        }
    } else {
        std::string n = name.rfind("::", 0) == 0 ? name.substr(2) : name;
        if (n != name) { VarRef r; r.name = n; return load(r, Ref<Env>()); }
        if (n.rfind("forge::", 0) == 0) {   // forge::fs::read -> builtin module fs, member read
            std::string rest = n.substr(7);
            if (auto it = sh->builtins.find(rest); it != sh->builtins.end()) return it->second;
            auto p = rest.find("::");
            if (p != std::string::npos) if (auto it = sh->builtins.find(rest.substr(0, p)); it != sh->builtins.end()) {
                Value cur = it->second; std::string tail = rest.substr(p + 2);
                for (;;) { auto q = tail.find("::"); cur = get_field(cur, q == std::string::npos ? tail : tail.substr(0, q)); if (q == std::string::npos) return cur; tail = tail.substr(q + 2); }
            }
        }
        auto p = n.rfind("::");
        std::string head = n.substr(0, p), member = n.substr(p + 2);
        Value h;
        if (auto it = globals->vars.find(head); it != globals->vars.end()) h = it->second;
        else if (auto b = sh->builtins.find(head); b != sh->builtins.end()) h = b->second;
        else if (head.find("::") != std::string::npos || !globals->using_ns.empty()) { try { VarRef r; r.name = head; h = load(r, Ref<Env>()); } catch (const ScriptError&) {} }
        if (!is_nil(h)) {
            if (auto* m = as<Map>(h)) { if (auto* x = m->find_str(member)) return *x; raise("NameError", "'" + member + "' is not a member of " + head); }
            return get_field(h, member);
        }
        for (auto& ns : globals->using_ns) {
            std::string q = ns + "::" + n;
            if (auto it = sh->builtins.find(q); it != sh->builtins.end()) return it->second;
            if (auto it = globals->vars.find(q); it != globals->vars.end()) return it->second;
        }
    }
    raise("NameError", "'" + name + "' was not declared in this scope");
}
void Interp::store(const VarRef& v, const Ref<Env>& env, Value x, bool define, bool is_const) {
    if (v.global()) {
        debug::check_write(*this, globals.get());
        if (define) { globals->vars[v.name] = share(x); if (is_const) globals->consts.insert(v.name); else globals->consts.erase(v.name); return; }
        auto it = globals->vars.find(v.name);
        if (it == globals->vars.end()) raise("NameError", "'" + v.name + "' is not defined (declare it with let)");
        if (globals->consts.count(v.name)) raise("TypeError", "cannot assign to const '" + v.name + "'");
        it->second = share(x); return;
    }
    Env* e = env.get();
    for (int d = v.depth; d > 0 && e; --d) e = e->parent.get();
    if (!e) raise("NameError", "bad scope for '" + v.name + "'");
    e->slots.at(std::size_t(v.slot)) = share(x);
}
Value Interp::share(const Value& v) {
    auto* i = as<Instance>(v);
    if (!i || !i->cls->is_struct || i->own != Own::Value) return v;
    if (i->rc.load(std::memory_order_acquire) <= 1) return v;   // sole reference: a temporary, elide the copy (C++17 guaranteed elision)
    return copy_value(v);
}

// ---------------------------------------------------------------- operators
static bool num(const Value& v, double& d) {
    if (auto* i = std::get_if<std::int64_t>(&v)) { d = double(*i); return true; }
    if (auto* f = std::get_if<double>(&v)) { d = *f; return true; }
    return false;
}
bool Interp::equal(const Value& a, const Value& b, int depth) {
    if (depth > 64) return false;
    double x, y;
    if (num(a, x) && num(b, y)) {
        if (a.index() == 2 && b.index() == 2) return std::get<2>(a) == std::get<2>(b);
        return x == y;
    }
    if (a.index() != b.index()) return false;
    switch (a.index()) { case 0: return true; case 1: return std::get<1>(a) == std::get<1>(b); }
    Obj* p = std::get<Ref<Obj>>(a).get(); Obj* q = std::get<Ref<Obj>>(b).get();
    if (p == q) return true;
    if (!p || !q) return false;
    if (auto* s = dynamic_cast<String*>(p)) { auto* t = dynamic_cast<String*>(q); return t && s->s == t->s; }
    if (auto* pi = dynamic_cast<PosIt*>(p)) {   // iterators: same container, same position
        auto* qi = dynamic_cast<PosIt*>(q); if (!qi || pi->i != qi->i) return false;
        auto* x = std::get_if<Ref<Obj>>(&pi->c); auto* y = std::get_if<Ref<Obj>>(&qi->c);
        return x && y && x->get() == y->get();
    }
    if (auto* l = dynamic_cast<List*>(p)) {
        auto* m = dynamic_cast<List*>(q); if (!m || l->items.size() != m->items.size()) return false;
        for (std::size_t i = 0; i < l->items.size(); ++i) if (!equal(l->items[i], m->items[i], depth + 1)) return false;
        return true;
    }
    if (auto* l = dynamic_cast<Map*>(p)) {
        auto* m = dynamic_cast<Map*>(q); if (!m || l->entries.size() != m->entries.size()) return false;
        for (auto& [k, v] : l->entries) { auto* w = m->find(k); if (!w || !equal(v, *w, depth + 1)) return false; }
        return true;
    }
    if (auto* i = dynamic_cast<Instance*>(p)) {
        auto* j = dynamic_cast<Instance*>(q);
        if (!j || !i->cls->is_struct || i->cls != j->cls || i->fields.size() != j->fields.size()) return false;
        for (auto& [k, v] : i->fields) { auto it = j->fields.find(k); if (it == j->fields.end() || !equal(v, it->second, depth + 1)) return false; }
        return true;
    }
    return false;
}

Value Interp::binary(Op op, const Value& a, const Value& b) {
    if (a.index() == 4 || b.index() == 4) {
        if (auto* os = as<Ostream>(a); os && op == Op::Shl) { stream_write(*os, b); return a; }
        if (auto* pi = as<PosIt>(a)) {   // iterator arithmetic
            if (auto* n = std::get_if<std::int64_t>(&b); n && (op == Op::Add || op == Op::Sub)) return obj(make<PosIt>(pi->c, op == Op::Add ? pi->i + *n : pi->i - *n));
            if (auto* qi = as<PosIt>(b); qi && op == Op::Sub) return pi->i - qi->i;
            if (auto* qi = as<PosIt>(b); qi && (op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge))
                return op == Op::Lt ? pi->i < qi->i : op == Op::Le ? pi->i <= qi->i : op == Op::Gt ? pi->i > qi->i : pi->i >= qi->i;
        }
        if (as<Instance>(a) || as<Instance>(b)) {   // operator overloading: member, then reversed comparisons, then <=>, then free functions
            bool found = false;
            Value r;
            if (as<Instance>(a)) { r = call_operator(a, std::string("operator") + op_name(op), {b}, found); if (found) return r; }
            if (as<Instance>(a) && op == Op::Ne) { r = call_operator(a, "operator==", {b}, found); if (found) return !truthy(r); }
            if (as<Instance>(b) && op == Op::Gt) { r = call_operator(b, "operator<", {a}, found); if (found) return r; }
            if (as<Instance>(a) && op == Op::Ge) { r = call_operator(a, "operator<", {b}, found); if (found) return !truthy(r); }
            if (as<Instance>(b) && op == Op::Le) { r = call_operator(b, "operator<", {a}, found); if (found) return !truthy(r); }
            if (as<Instance>(a)) {
                r = call_operator(a, "operator<=>", {b}, found);
                if (found) if (auto* c = std::get_if<std::int64_t>(&r)) switch (op) {
                    case Op::Lt: return *c < 0; case Op::Le: return *c <= 0; case Op::Gt: return *c > 0; case Op::Ge: return *c >= 0;
                    case Op::Eq: return *c == 0; case Op::Ne: return *c != 0; default: break; }
            }
            if (auto it = globals->vars.find(std::string("operator") + op_name(op)); it != globals->vars.end()) { std::vector<Value> args{a, b}; return call(it->second, args); }
        }
    }
    const auto* ia = std::get_if<std::int64_t>(&a); const auto* ib = std::get_if<std::int64_t>(&b);
    constexpr auto MAX = std::numeric_limits<std::int64_t>::max(), MIN = std::numeric_limits<std::int64_t>::min();
    switch (op) {
    case Op::Eq: return equal(a, b);
    case Op::Ne: return !equal(a, b);
    case Op::In: {
        if (auto* l = as<List>(b)) { for (auto& x : l->items) if (equal(a, x)) return true; return false; }
        if (auto* m = as<Map>(b)) return m->find(a) != nullptr;
        if (auto* s = as<String>(b)) { auto* t = as<String>(a); if (!t) raise("TypeError", "'in' on a string needs a string"); return s->s.find(t->s) != std::string::npos; }
        raise("TypeError", "'in' needs a list, map or string, got " + type_of(b));
    }
    default: break;
    }
    if (ia && ib) {
        std::int64_t x = *ia, y = *ib;
        switch (op) {
        case Op::Add: if ((y > 0 && x > MAX - y) || (y < 0 && x < MIN - y)) raise("OverflowError", "integer overflow in +"); return x + y;
        case Op::Sub: if ((y < 0 && x > MAX + y) || (y > 0 && x < MIN + y)) raise("OverflowError", "integer overflow in -"); return x - y;
        case Op::Mul: {
            if (x == 0 || y == 0) return std::int64_t(0);
            if ((x == -1 && y == MIN) || (y == -1 && x == MIN)) raise("OverflowError", "integer overflow in *");
            std::int64_t r = x * (y == 0 ? 1 : 1);   // computed below with a division check
            (void)r;
            if ((x > 0 && y > 0 && x > MAX / y) || (x < 0 && y < 0 && x < MAX / y) || (x > 0 && y < 0 && y < MIN / x) || (x < 0 && y > 0 && x < MIN / y))
                raise("OverflowError", "integer overflow in *");
            return x * y;
        }
        case Op::Div: if (y == 0) raise("ArithmeticError", "integer division by zero"); if (x == MIN && y == -1) raise("OverflowError", "integer overflow in /"); return x / y;   // truncates, as in C++
        case Op::Mod: if (y == 0) raise("ArithmeticError", "integer modulo by zero"); if (y == -1) return std::int64_t(0); return x % y;
        case Op::BitAnd: return x & y; case Op::BitOr: return x | y; case Op::BitXor: return x ^ y;
        case Op::Shl: if (y < 0 || y > 63) raise("ArithmeticError", "shift count out of range"); return std::int64_t(std::uint64_t(x) << y);
        case Op::Shr: if (y < 0 || y > 63) raise("ArithmeticError", "shift count out of range"); return x >> y;
        case Op::Lt: return x < y; case Op::Le: return x <= y; case Op::Gt: return x > y; case Op::Ge: return x >= y;
        default: break;
        }
    }
    double x, y;
    if (num(a, x) && num(b, y)) {
        switch (op) {
        case Op::Add: return x + y; case Op::Sub: return x - y; case Op::Mul: return x * y;
        case Op::Div: return x / y;   // IEEE: inf / nan, as in C++
        case Op::Mod: return std::fmod(x, y);
        case Op::Lt: return x < y; case Op::Le: return x <= y; case Op::Gt: return x > y; case Op::Ge: return x >= y;
        default: break;
        }
    }
    auto* sa = as<String>(a); auto* sb = as<String>(b);
    if (sa && sb) {
        switch (op) {
        case Op::Add: return new_string(sa->s + sb->s);
        case Op::Lt: return sa->s < sb->s; case Op::Le: return sa->s <= sb->s; case Op::Gt: return sa->s > sb->s; case Op::Ge: return sa->s >= sb->s;
        default: break;
        }
    }
    if (sa && ib && op == Op::Mul) {
        if (*ib < 0 || std::uint64_t(*ib) * sa->s.size() > (256ull << 20)) raise("ValueError", "string repeat count out of range");
        std::string o; o.reserve(sa->s.size() * std::size_t(*ib)); for (std::int64_t k = 0; k < *ib; ++k) o += sa->s; return new_string(std::move(o));
    }
    if (op == Op::Add) {
        auto* la = as<List>(a); auto* lb = as<List>(b);
        if (la && lb) { auto l = make<List>(); for (auto& v : la->items) l->push(v); for (auto& v : lb->items) l->push(v); return obj(l); }
        if (auto* ma = as<Map>(a)) if (auto* mb = as<Map>(b)) { auto m = make<Map>(); for (auto& [k, v] : ma->entries) m->set(k, v); for (auto& [k, v] : mb->entries) m->set(k, v); return obj(m); }
        if (sa || sb) raise("TypeError", "cannot add " + type_of(a) + " and " + type_of(b) + " (use str() or \"${...}\")");
    }
    if (op == Op::Add && sa && b.index() == 2) raise("TypeError", "cannot add string and int (use std::to_string)");
    raise("TypeError", std::string("no operator") + op_name(op) + " for " + type_of(a) + " and " + type_of(b));
}
Value Interp::unary(const std::string& op, const Value& a) {
    if (op == "not") return !truthy(a);
    if (op == "bool") return truthy(a);
    if (op == "~") { if (auto* i = std::get_if<std::int64_t>(&a)) return ~*i; raise("TypeError", "operator~ needs an int, got " + type_of(a)); }
    if (as<Instance>(a)) { bool f = false; Value r = call_operator(a, "operator-", {}, f); if (f) return r; }
    if (auto* i = std::get_if<std::int64_t>(&a)) { if (*i == std::numeric_limits<std::int64_t>::min()) raise("OverflowError", "integer overflow in -"); return -*i; }
    if (auto* d = std::get_if<double>(&a)) return -*d;
    raise("TypeError", "cannot negate " + type_of(a));
}

// ---------------------------------------------------------------- members
static Value* find_method_in(Shared& sh, const std::string& table, const std::string& name) {
    auto t = sh.methods.find(table); if (t == sh.methods.end()) return nullptr;
    auto m = t->second.find(name); return m == t->second.end() ? nullptr : &m->second;
}
Value Interp::get_field(const Value& o, const std::string& name, bool optional) {
    if (optional && is_nil(o)) return Nil{};
    auto bind = [&](const Value& f) { return obj(make<Bound>(o, f)); };
    if (auto* i = as<Instance>(o)) {
        if (auto it = i->fields.find(name); it != i->fields.end()) return it->second;
        if (auto* m = i->cls->find_method(name)) return bind(*m);
        if (auto* g = i->cls->find_method("get_" + name)) { std::vector<Value> a; return call(bind(*g), a); }
        for (const Class* c = i->cls.get(); c; c = c->base.get()) if (auto it = c->statics.find(name); it != c->statics.end()) return it->second;
        if (i->cls->has_field(name)) return Nil{};
        if (optional) return Nil{};
        raise("AttributeError", i->cls->name + " has no member '" + name + "'");
    }
    if (auto* pi = as<PosIt>(o)) {   // it->first, it->member
        auto* r = std::get_if<Ref<Obj>>(&pi->c); std::int64_t n = 0;
        if (r && *r) { if (auto* l = dynamic_cast<List*>(r->get())) n = std::int64_t(l->items.size()); else if (auto* mp = dynamic_cast<Map*>(r->get())) n = std::int64_t(mp->entries.size()); }
        if (pi->i < 0 || pi->i >= n) raise("IndexError", "dereferencing an end() iterator");
        Value e;
        if (auto* l = dynamic_cast<List*>(r->get())) e = l->items[std::size_t(pi->i)];
        else { auto& [k, v] = dynamic_cast<Map*>(r->get())->entries[std::size_t(pi->i)]; if (name == "first") return k; if (name == "second") return v; e = v; }
        return get_field(e, name, optional);
    }
    if (auto* m = as<Map>(o)) {
        // Library maps (frozen, holding natives) resolve their own members first: fs.remove is the library call.
        if (m->frozen) if (auto* v = m->find_str(name); v && as<Native>(*v)) return *v;
        if (auto* f = find_method_in(*sh, "map", name)) return bind(*f);
        if (auto* v = m->find_str(name)) return *v;
        if (auto* f = find_method_in(*sh, "any", name)) return bind(*f);
        if (optional) return Nil{};
        raise("AttributeError", "map has no member '" + name + "'");
    }
    if (auto* l = as<List>(o); l && (name == "first" || name == "second")) {
        std::size_t k = name == "first" ? 0 : 1;
        if (l->items.size() <= k) raise("IndexError", "pair has no ." + name);
        return l->items[k];
    }
    if (auto* c = as<Class>(o)) {
        if (name == "name") return new_string(c->name);
        for (const Class* k = c; k; k = k->base.get()) if (auto it = k->statics.find(name); it != k->statics.end()) return it->second;
        if (auto* m = c->find_method(name)) return *m;
        if (optional) return Nil{};
        raise("AttributeError", c->name + " has no static member '" + name + "'");
    }
    if (auto* g = as<Globals>(o)) {
        if (auto it = g->vars.find(name); it != g->vars.end()) return it->second;
        if (optional) return Nil{};
        raise("AttributeError", "module has no member '" + name + "'");
    }
    if (auto* h = as<Handle>(o)) {
        if (auto it = h->props.find(name); it != h->props.end()) return it->second;
        if (auto* f = find_method_in(*sh, "handle:" + h->kind, name)) return bind(*f);
        if (auto* f = find_method_in(*sh, "handle", name)) return bind(*f);
        if (optional) return Nil{};
        raise("AttributeError", h->kind + " has no member '" + name + "'");
    }
    std::string t = type_of(o);
    if (as<Function>(o) || as<Native>(o) || as<Bound>(o)) t = "fn";
    if (auto* f = find_method_in(*sh, t, name)) return bind(*f);
    if (auto* f = find_method_in(*sh, "any", name)) return bind(*f);
    if (optional) return Nil{};
    raise("AttributeError", t + " has no member '" + name + "'");
}
void Interp::set_field(const Value& o, const std::string& name, Value v) {
    if (auto* r = std::get_if<Ref<Obj>>(&o)) debug::check_write(*this, r->get());
    if (auto* i = as<Instance>(o)) {
        if (i->frozen) raise("TypeError", "object is frozen");
        if (!i->cls->is_dynamic && !i->cls->has_field(name)) raise("AttributeError", i->cls->name + " has no field '" + name + "' (declare it in the class, or mark the class dynamic)");
        i->fields[name] = share(v); return;
    }
    if (auto* m = as<Map>(o)) { if (m->frozen) raise("TypeError", "map is frozen"); m->set_str(name, share(v)); return; }
    if (auto* c = as<Class>(o)) { c->statics[name] = share(v); return; }
    if (auto* h = as<Handle>(o)) { h->props[name] = v; return; }
    raise("AttributeError", "cannot set '" + name + "' on " + type_of(o));
}
static std::size_t norm_index(Interp& in, std::int64_t i, std::size_t n) {
    std::int64_t k = i < 0 ? i + std::int64_t(n) : i;
    if (k < 0 || std::uint64_t(k) >= n) in.raise("IndexError", "index " + std::to_string(i) + " out of range for length " + std::to_string(n));
    return std::size_t(k);
}
Value Interp::get_index(const Value& o, const Value& i) {
    if (auto* l = as<List>(o)) { auto* k = std::get_if<std::int64_t>(&i); if (!k) raise("TypeError", "list index must be int"); return l->items.at(norm_index(*this, *k, l->items.size())); }
    if (auto* m = as<Map>(o)) {
        if (auto* v = m->find(i)) return *v;
        if (m->has_dflt && !m->frozen) {   // std::map::operator[] inserts a value-initialised element
            std::vector<Value> none; Value d = is_nil(m->dflt) ? Value(Nil{}) : call(m->dflt, none);
            if (!key_of(i)) raise("TypeError", "unsupported map key type " + type_of(i));
            if (m->borrows.load() > 0) raise("BorrowError", "operator[] would insert a key into a map while it is being iterated");
            m->set(i, d); return *m->find(i);
        }
        raise("KeyError", "key " + repr(i) + " not found");
    }
    if (auto* s = as<String>(o)) { auto* k = std::get_if<std::int64_t>(&i); if (!k) raise("TypeError", "string index must be int"); return new_string(std::string(1, s->s.at(norm_index(*this, *k, s->s.size())))); }
    if (auto* g = as<Globals>(o)) { if (auto* s = as<String>(i)) return get_field(o, s->s); }
    raise("TypeError", "cannot index " + type_of(o));
}
void Interp::set_index(const Value& o, const Value& i, Value v) {
    if (auto* r = std::get_if<Ref<Obj>>(&o)) debug::check_write(*this, r->get());
    if (auto* l = as<List>(o)) { if (l->frozen) raise("TypeError", "list is frozen"); auto* k = std::get_if<std::int64_t>(&i); if (!k) raise("TypeError", "list index must be int"); l->items.at(norm_index(*this, *k, l->items.size())) = share(v); return; }
    if (auto* m = as<Map>(o)) { if (m->frozen) raise("TypeError", "map is frozen");
        if (m->borrows.load() > 0 && !m->find(i)) raise("BorrowError", "operator[] would insert a key into a map while it is being iterated");
        try { m->set(i, share(v)); } catch (const std::invalid_argument& e) { raise("TypeError", e.what()); } return; }
    if (auto* s = as<String>(o)) {   // s[i] = 'c'
        if (s->frozen) raise("TypeError", "string is frozen");
        auto* k = std::get_if<std::int64_t>(&i); if (!k) raise("TypeError", "string index must be int");
        auto* c = as<String>(v); if (!c || c->s.size() != 1) raise("TypeError", "s[i] = needs a single char");
        s->s.at(norm_index(*this, *k, s->s.size())) = c->s[0]; return;
    }
    raise("TypeError", "cannot assign by index on " + type_of(o));
}

// ---------------------------------------------------------------- types
bool Interp::conforms(const Value& v, const TypeAnn& t) {
    const std::string& n = t.name;
    if (n == "any") return true;
    if (is_nil(v)) return t.nullable || n == "nil";
    if (n == "int") return v.index() == 2;
    if (n == "float") return v.index() == 3 || v.index() == 2;
    if (n == "number") return v.index() == 2 || v.index() == 3;
    if (n == "bool") return v.index() == 1;
    if (n == "string" || n == "str") return as<String>(v) != nullptr;
    if (n == "list") return as<List>(v) != nullptr;
    if (n == "map") return as<Map>(v) != nullptr;
    if (n == "fn") return as<Function>(v) || as<Native>(v) || as<Bound>(v) || as<Class>(v);
    if (n == "task") return as<Task>(v) != nullptr;
    if (n == "channel") return as<Channel>(v) != nullptr;
    if (n == "handle") return as<Handle>(v) != nullptr;
    if (n == "weak") return as<WeakRef>(v) != nullptr;
    if (n == "shared") return as<SharedBox>(v) != nullptr;
    if (n == "class") return as<Class>(v) != nullptr;
    if (n == "Disposable") { if (as<Handle>(v)) return true; if (auto* i = as<Instance>(v)) return i->cls->find_method("dispose") || i->cls->derives("Disposable"); return false; }
    if (auto* i = as<Instance>(v)) {
        if (i->cls->derives(n)) return true;
        // structural check for interfaces declared in scope
        Value iv; if (auto it = globals->vars.find(n); it != globals->vars.end()) iv = it->second; else if (auto b = sh->builtins.find(n); b != sh->builtins.end()) iv = b->second;
        if (auto* c = as<Class>(iv); c && c->is_interface) { for (auto& r : c->required) if (!i->cls->find_method(r)) return false; return true; }
        return false;
    }
    if (auto* h = as<Handle>(v)) return h->kind == n;
    return false;
}
void Interp::check_type(const Value& v, const TypeAnn& t, const std::string& what) {
    if (!conforms(v, t)) raise("TypeError", what + " expects " + t.str() + ", got " + type_of(v));
}

// ---------------------------------------------------------------- calls
namespace {
// Native stack guard: recursion depth alone is not enough when frames are large (sanitizers, deep
// expressions), so the guard also measures how much C++ stack the current thread has used.
thread_local std::uintptr_t t_stack_base = 0;
constexpr std::uintptr_t kStackBudget = 3u << 20;   // well inside the 8 MiB main / task thread stacks
struct DepthGuard {
    Interp& in;
    bool outer = false;
    DepthGuard(Interp& i, const std::string& name, int line) : in(i) {
#if defined(_MSC_VER) && !defined(__clang__)
        auto here = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
        auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
        if (!t_stack_base) { t_stack_base = here; outer = true; }
        else if (t_stack_base > here && t_stack_base - here > kStackBudget) in.raise("LimitExceededError", "native stack limit reached (infinite recursion?)");
        if (++in.depth > in.sh->limits.max_depth) { --in.depth; if (outer) t_stack_base = 0; in.raise("LimitExceededError", "call depth limit of " + std::to_string(in.sh->limits.max_depth) + " reached (infinite recursion?)"); }
        in.frames.emplace_back(name, line);
    }
    ~DepthGuard() { --in.depth; in.frames.pop_back(); if (outer) t_stack_base = 0; }
};
}
Value Interp::call(const Value& f, std::vector<Value>& args, int call_line) {
    if (call_line) line = call_line;
    if (auto* fn = as<Function>(f)) return call_function(Ref<Function>(fn), args, nullptr);
    if (auto* inst = as<Instance>(f)) if (auto* m = inst->cls->find_method("operator()")) { Value bnd = obj(make<Bound>(f, *m)); return call(bnd, args); }
    if (auto* b = as<Bound>(f)) {
        if (auto* fn = as<Function>(b->fn)) return call_function(Ref<Function>(fn), args, &b->self);
        std::vector<Value> a; a.reserve(args.size() + 1); a.push_back(b->self); for (auto& x : args) a.push_back(std::move(x));
        return call(b->fn, a);
    }
    if (auto* n = as<Native>(f)) {
        int k = int(args.size());
        if (k < n->min_args || (n->max_args >= 0 && k > n->max_args))
            raise("TypeError", n->name + " expects " + (n->max_args == n->min_args ? std::to_string(n->min_args) : std::to_string(n->min_args) + (n->max_args < 0 ? "+" : ".." + std::to_string(n->max_args))) + " argument(s), got " + std::to_string(k));
        DepthGuard g(*this, n->name, line);
        return n->fn(*this, args);
    }
    if (auto* c = as<Class>(f)) return instantiate(Ref<Class>(c), args);
    raise("TypeError", type_of(f) + " is not callable");
}
Value Interp::call_function(const Ref<Function>& f, std::vector<Value>& args, const Value* self) {
    const FnDecl& d = *f->decl;
    int saved_line = line;
    DepthGuard g(*this, d.name, line);
    step();
    if (args.size() > d.params.size()) raise("TypeError", d.name + "() takes " + std::to_string(d.params.size()) + " argument(s), got " + std::to_string(args.size()));
    auto env = make<Env>(std::size_t(d.nslots), f->env);
    if (d.is_method) { if (!self) raise("TypeError", d.name + "() is a method; call it on an object"); env->slots.at(0) = *self; }
    Ref<Globals> saved_g = globals;
    globals = f->globals ? f->globals : globals;
    struct Restore { Interp& in; Ref<Globals> g; int line; ~Restore() { in.globals = g; in.line = line; } } restore{*this, saved_g, saved_line};
    for (std::size_t i = 0; i < d.params.size(); ++i) {
        auto& p = d.params[i];
        Value v;
        if (i < args.size()) v = std::move(args[i]);
        else if (p.dflt) v = eval(*p.dflt, env);
        else raise("TypeError", d.name + "() missing argument '" + p.name + "'");
        if (p.type) check_type(v, *p.type, d.name + "() argument '" + p.name + "'");
        if (p.conv == 'i' && !std::holds_alternative<std::int64_t>(v)) {
            if (auto* x = std::get_if<double>(&v)) v = std::int64_t(*x);
            else if (auto* b = std::get_if<bool>(&v)) v = std::int64_t(*b);
            else if (auto* s = as<String>(v); s && s->s.size() == 1) v = std::int64_t(static_cast<signed char>(s->s[0]));
            else raise("TypeError", d.name + "() argument '" + p.name + "' expects int, got " + type_of(v));
        } else if (p.conv == 'f' && !std::holds_alternative<double>(v)) {
            if (auto* x = std::get_if<std::int64_t>(&v)) v = double(*x);
            else if (auto* b = std::get_if<bool>(&v)) v = double(*b);
            else raise("TypeError", d.name + "() argument '" + p.name + "' expects double, got " + type_of(v));
        }
        env->slots.at(std::size_t(p.slot)) = p.by_ref ? std::move(v) : copy_value(v);
    }
    Value result;
    if (!f->chunk && sh->use_vm && f->calls.fetch_add(1) + 1 >= sh->jit_threshold) f->chunk = compile(d);
    if (f->chunk) result = vm_run(*this, *f->chunk, env);
    else {
        ret = Nil{};
        Flow fl = exec_block(*d.body, env);
        result = fl == Flow::Return ? std::move(ret) : Value(Nil{});
        ret = Nil{};
    }
    if (d.ret) check_type(result, *d.ret, d.name + "() return value");
    if (d.has_out) {
        ref_back.clear(); ref_back_fn = f.get();
        for (std::size_t i = 0; i < d.params.size() && i < args.size(); ++i)
            if (d.params[i].out) ref_back.emplace_back(i, env->slots.at(std::size_t(d.params[i].slot)));
    }
    return result;
}
Value Interp::make_closure(const std::shared_ptr<const FnDecl>& fd, const Ref<Env>& env) {
    return obj(make<Function>(fd, env, globals));
}
Value Interp::run_top(const std::shared_ptr<const FnDecl>& top, bool vm) {
    auto env = make<Env>(std::size_t(top->nslots), Ref<Env>());
    file = top->file;
    if (vm) {
        auto c = compile(*top);
        Value r = vm_run(*this, *c, env);
        return top->has_main ? run_main() : r;
    }
    ret = Nil{};
    Value last = Nil{};
    // REPL/eval path: evaluate statements directly and keep the value of a trailing expression.
    const Node& b = *top->body;
    for (std::size_t i = 0; i < b.kids.size(); ++i) {
        auto& s = *b.kids[i];
        if (s.k == NK::ExprStmt && i + 1 == b.kids.size()) { line = s.line; step(); last = eval(*s.kids[0], env); break; }
        Flow fl = exec(s, env);
        if (fl != Flow::Normal) break;
    }
    if (top->has_main) return run_main();
    return last;
}
// `int main()`: called after the top-level declarations; its return value is the exit status.
Value Interp::run_main() {
    auto it = globals->vars.find("main");
    if (it == globals->vars.end()) raise("NameError", "main is not defined");
    std::vector<Value> none;
    Value r = call(it->second, none);
    return is_nil(r) ? Value(std::int64_t(0)) : r;
}

// ---------------------------------------------------------------- classes
Value Interp::define_class(const ClassDecl& cd, const Ref<Env>& env) {
    auto c = make<Class>();
    c->name = cd.name; c->is_struct = cd.is_struct; c->is_dynamic = cd.is_dynamic; c->is_interface = cd.is_interface;
    c->required = cd.required; c->interfaces = cd.interfaces; c->meta = cd.meta;
    auto lookup_name = [&](const std::string& n) -> Value {
        if (auto it = globals->vars.find(n); it != globals->vars.end()) return it->second;
        if (auto it = sh->builtins.find(n); it != sh->builtins.end()) return it->second;
        // local classes: walk the current activation chain by name is not possible (slots are anonymous) -> require global
        raise("NameError", "class '" + n + "' is not defined");
    };
    if (!cd.base.empty()) {
        auto* b = as<Class>(lookup_name(cd.base));
        if (!b || b->is_interface) raise("TypeError", cd.name + ": base '" + cd.base + "' is not a class");
        c->base = Ref<Class>(b);
        if (b->is_dynamic) c->is_dynamic = true;
    }
    c->def_env = env;
    for (auto& [n, d] : cd.fields) {
        c->fields.push_back(n);
        bool lit = !d || d->k == NK::Nil || d->k == NK::True || d->k == NK::False || d->k == NK::Int || d->k == NK::Float || d->k == NK::Str;
        c->field_defaults.push_back(lit && d ? eval(*d, env) : Value(Nil{}));
        c->field_inits.push_back(lit ? nullptr : d.get());   // `G g{"x"};` constructs a fresh member for every object
    }
    for (auto& m : cd.methods) {
        auto fn = make<Function>(m, env, globals); fn->owner = c;
        c->methods[m->name] = obj(fn);
    }
    for (auto& iname : cd.interfaces) {
        if (iname == "Disposable") { if (!c->find_method("dispose")) raise("TypeError", cd.name + " implements Disposable but has no dispose()"); continue; }
        auto* ic = as<Class>(lookup_name(iname));
        if (!ic || !ic->is_interface) raise("TypeError", iname + " is not an interface");
        for (auto& r : ic->required) if (!c->find_method(r)) raise("TypeError", cd.name + " implements " + iname + " but has no method '" + r + "'");
    }
    return obj(c);
}
Value Interp::instantiate(const Ref<Class>& c, std::vector<Value>& args) {
    if (c->is_interface) raise("TypeError", "cannot instantiate interface " + c->name);
    for (const Class* k = c.get(); k; k = k->base.get())
        for (auto& r : k->required) if (!c->find_method(r)) raise("TypeError", "cannot instantiate abstract class " + c->name + " (pure virtual '" + r + "' is not overridden)");
    auto inst = make<Instance>(c);
    std::vector<const Class*> chain; for (const Class* k = c.get(); k; k = k->base.get()) chain.push_back(k);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (std::size_t i = 0; i < (*it)->fields.size(); ++i) {
            if (i < (*it)->field_inits.size() && (*it)->field_inits[i]) { inst->fields[(*it)->fields[i]] = share(eval(*(*it)->field_inits[i], (*it)->def_env)); continue; }
            Value d = (*it)->field_defaults[i];
            if (auto* l = as<List>(d)) d = obj(make<List>(l->items));
            else if (auto* m = as<Map>(d)) { auto n = make<Map>(); for (auto& [k, v] : m->entries) n->set(k, v); d = obj(n); }
            inst->fields[(*it)->fields[i]] = share(d);
        }
    Value self = obj(inst);
    const Value* init = nullptr;
    for (const Class* k = c.get(); k && !init; k = k->base.get()) {
        bool any_ctor = false;
        if (auto it = k->methods.find("init/" + std::to_string(args.size())); it != k->methods.end()) init = &it->second;
        else if (auto it2 = k->methods.find("init"); it2 != k->methods.end()) init = &it2->second;
        else for (auto& [mk, mv] : k->methods) if (mk.rfind("init/", 0) == 0) any_ctor = true;
        if (!init && any_ctor) raise("TypeError", "no constructor of " + k->name + " takes " + std::to_string(args.size()) + " argument(s)");
    }
    if (init) {
        Value b = obj(make<Bound>(self, *init));
        call(b, args);   // a constructor that throws leaves no object to destroy (finalize is set below)
    } else if (!args.empty()) {
        std::vector<std::string> all; for (auto it = chain.rbegin(); it != chain.rend(); ++it) for (auto& f : (*it)->fields) all.push_back(f);
        if (args.size() > all.size()) raise("TypeError", c->name + "() takes at most " + std::to_string(all.size()) + " argument(s)");
        for (std::size_t i = 0; i < args.size(); ++i) inst->fields[all[i]] = share(args[i]);
    }
    inst->finalize = c->find_method("~") != nullptr;
    return self;
}
// Run script destructors (~T) of objects whose last reference just went away. Most-derived first, then
// bases; members are released afterwards (which may queue more destructors, handled by the loop).
void Interp::run_finalizers() {
    if (in_finalizers_) return;
    in_finalizers_ = true;
    struct Reset { bool& b; ~Reset() { b = false; } } reset{in_finalizers_};
    while (finalizers_pending()) {
        auto objs = take_finalizers();
        for (auto& o : objs) {
            auto* i = dynamic_cast<Instance*>(o.get());
            if (!i) continue;
            Value self = obj(o);
            for (const Class* k = i->cls.get(); k; k = k->base.get()) {
                auto m = k->methods.find("~");
                if (m == k->methods.end()) continue;
                std::vector<Value> a; Value b = obj(make<Bound>(self, m->second));
                call(b, a);
            }
        }
    }
}
void Interp::dispose(const Value& v) {
    if (is_nil(v)) return;
    if (auto* h = as<Handle>(v)) { h->dispose(); return; }
    if (auto* i = as<Instance>(v)) {
        if (i->disposed) return;
        i->disposed = true;
        if (auto* m = i->cls->find_method("dispose")) { std::vector<Value> a; Value b = obj(make<Bound>(v, *m)); call(b, a); }
        return;
    }
    if (auto* t = as<Task>(v)) { t->st->cancel = true; return; }
    if (auto* c = as<Channel>(v)) { std::lock_guard g(c->st->m); c->st->closed = true; c->st->cv.notify_all(); return; }
    raise("TypeError", type_of(v) + " is not disposable");
}

Value Interp::interpolate(const std::vector<Value>& parts) {
    std::string s; for (auto& p : parts) s += str(p);
    return new_string(std::move(s));
}

// ---------------------------------------------------------------- processes
Value Interp::proc_result(const std::string& cmd, int code, std::string out, std::string err, std::uint64_t ms, bool timed_out) {
    auto r = make<Instance>(sh->classes.at("ProcResult"));
    r->fields["cmd"] = new_string(cmd);
    r->fields["code"] = std::int64_t(code);
    r->fields["ok"] = code == 0 && !timed_out;
    r->fields["out"] = new_string(std::move(out));
    r->fields["err"] = new_string(std::move(err));
    r->fields["ms"] = std::int64_t(ms);
    r->fields["timed_out"] = timed_out;
    return obj(r);
}
static std::string quote_for(const shell_backend& b, const std::string& s) { return b.quote(s); }
Value Interp::shell(const Node& n, const std::vector<Value>& parts) {
    if (!caps.proc) raise("PermissionError", "running processes is disabled in this sandbox");
    const std::string& kind = n.s;
    const shell_backend* be = shells().find(kind);
    if (!be) raise("ValueError", "unknown shell " + kind + " (see forge::shells())");
    // kids[0] is a Str (no interpolation) or an Interp node whose Str parts are literal
    std::string script;
    const Node& body = *n.kids[0];
    if (body.k == NK::Str) script = body.s;
    else for (std::size_t i = 0; i < body.kids.size() && i < parts.size(); ++i)
        script += body.kids[i]->k == NK::Str ? str(parts[i]) : quote_for(*be, str(parts[i]));
    std::string exe = shell_exe(*be);
    if (exe.empty()) raise("ShellError", kind + " is not installed (looked for " + [&] { std::string l; for (auto& e : be->exes) l += (l.empty() ? "'" : ", '") + e + "'"; return l; }() + " on PATH)");
    std::vector<std::string> argv = be->argv(exe, script);
    enforce_scope(*this, SecScope::exec, exe);
    charge_process(kind);
    os::spawn_spec sp; sp.argv = argv; sp.max_output = sh->limits.max_output;
    sp.limits.memory_bytes = child_mem; sp.limits.cpu_seconds = child_cpu_s;
    sp.timeout_ms = child_timeout_ms;
    if (deadline_ns) { auto now = os::monotonic_ns(); std::uint64_t left = deadline_ns > now ? (deadline_ns - now) / 1000000 + 1 : 1; sp.timeout_ms = sp.timeout_ms ? std::min(sp.timeout_ms, left) : left; }
    auto r = os::spawn_wait(sp);
    if (!r) raise("ShellError", r.err.what);
    return proc_result(kind + " { " + script + " }", r->exit_code, std::move(r->out), std::move(r->err), r->wall_ms, r->timed_out);
}

// ---------------------------------------------------------------- tasks
Value deep_copy(Interp& in, const Value& v, std::unordered_map<const Obj*, Value>& memo) {
    auto* rp = std::get_if<Ref<Obj>>(&v);
    if (!rp || !*rp) return v;
    const Obj* o = rp->get();
    if (auto it = memo.find(o); it != memo.end()) return it->second;
    if (auto* s = dynamic_cast<const String*>(o); s && !o->frozen) { Value c = in.new_string(s->s); memo[o] = c; return c; }
    if (o->frozen || dynamic_cast<const String*>(o) || dynamic_cast<const Native*>(o) || dynamic_cast<const Range*>(o)) return v;
    if (auto* l = dynamic_cast<const List*>(o)) { auto n = make<List>(); memo[o] = obj(n); for (auto& x : l->items) n->push(deep_copy(in, x, memo)); return obj(n); }
    if (auto* m = dynamic_cast<const Map*>(o)) { auto n = make<Map>(); memo[o] = obj(n); for (auto& [k, x] : m->entries) n->set(deep_copy(in, k, memo), deep_copy(in, x, memo)); return obj(n); }
    if (auto* e = dynamic_cast<const Env*>(o)) {
        auto n = make<Env>(e->slots.size(), Ref<Env>()); memo[o] = obj(n);
        for (std::size_t i = 0; i < e->slots.size(); ++i) {
            // A closure's frame may also hold thread-bound handles it never uses (e.g. the std::future of a
            // sibling task). They are not copied; touching one from the new thread raises BorrowError.
            if (as<Task>(e->slots[i]) || as<Handle>(e->slots[i])) { n->slots[i] = Value(Ref<Obj>(moved_sentinel())); continue; }
            n->slots[i] = deep_copy(in, e->slots[i], memo);
        }
        if (e->parent) n->parent = as_ref<Env>(deep_copy(in, obj(e->parent), memo));
        return obj(n);
    }
    if (auto* g = dynamic_cast<const Globals*>(o)) {
        auto n = make<Globals>(); memo[o] = obj(n); n->consts = g->consts;
        for (auto& [k, x] : g->vars) {
            // Same rule as closure frames: thread-bound globals (handles, tasks) stay behind.
            if (as<Task>(x) || as<Handle>(x)) { n->vars[k] = Value(Ref<Obj>(moved_sentinel())); continue; }
            n->vars[k] = deep_copy(in, x, memo);
        }
        return obj(n);
    }
    if (auto* f = dynamic_cast<const Function*>(o)) {
        auto n = make<Function>(f->decl, Ref<Env>(), Ref<Globals>()); memo[o] = obj(n);
        n->chunk = f->chunk;
        if (f->env) n->env = as_ref<Env>(deep_copy(in, obj(f->env), memo));
        if (f->globals) n->globals = as_ref<Globals>(deep_copy(in, obj(f->globals), memo));
        if (f->owner) n->owner = as_ref<Class>(deep_copy(in, obj(f->owner), memo));
        return obj(n);
    }
    if (auto* c = dynamic_cast<const Class*>(o)) {
        auto n = make<Class>(); memo[o] = obj(n);
        n->name = c->name; n->fields = c->fields; n->interfaces = c->interfaces; n->is_struct = c->is_struct; n->is_dynamic = c->is_dynamic;
        n->is_interface = c->is_interface; n->required = c->required;
        if (c->base) n->base = as_ref<Class>(deep_copy(in, obj(c->base), memo));
        for (auto& d : c->field_defaults) n->field_defaults.push_back(deep_copy(in, d, memo));
        n->field_inits = c->field_inits; if (c->def_env) n->def_env = as_ref<Env>(deep_copy(in, obj(c->def_env), memo));
        for (auto& [k, x] : c->methods) n->methods[k] = deep_copy(in, x, memo);
        for (auto& [k, x] : c->statics) n->statics[k] = deep_copy(in, x, memo);
        return obj(n);
    }
    if (auto* i = dynamic_cast<const Instance*>(o)) {
        auto cls = as_ref<Class>(deep_copy(in, obj(i->cls), memo));
        auto n = make<Instance>(cls); memo[o] = obj(n);
        for (auto& [k, x] : i->fields) n->fields[k] = deep_copy(in, x, memo);
        return obj(n);
    }
    if (auto* b = dynamic_cast<const Bound*>(o)) { auto n = make<Bound>(deep_copy(in, b->self, memo), deep_copy(in, b->fn, memo)); memo[o] = obj(n); return obj(n); }
    if (dynamic_cast<const SharedBox*>(o) && o->own == Own::Shared) return v;   // forge::shared<T>: one locked object for all tasks
    if (auto* c = dynamic_cast<const Channel*>(o)) { auto n = make<Channel>(0); n->st = c->st; memo[o] = obj(n); return obj(n); }
    if (dynamic_cast<const WeakRef*>(o)) return Nil{};
    in.raise("TypeError", std::string("a ") + o->type_name() + " cannot be sent to another task (only data, functions and channels can)");
}

Value Interp::spawn(const Value& f, std::vector<Value> args) {
    std::unordered_map<const Obj*, Value> memo;
    Value g = deep_copy(*this, obj(globals), memo);
    Value fc = deep_copy(*this, f, memo);
    std::vector<Value> ac; for (auto& a : args) ac.push_back(deep_copy(*this, a, memo));
    memo.clear();
    auto t = make<Task>();
    auto st = t->st; auto shared = sh;
    Heap::instance().active_tasks.fetch_add(1);
    Caps c = caps; auto cm = child_mem, cc = child_cpu_s, ct = child_timeout_ms; auto dl = deadline_ns; std::string fl = file;
    auto hq = handle_quota, pq = proc_quota;
    struct Payload { Value g, f; std::vector<Value> a; };
    auto pl = std::make_shared<Payload>(Payload{std::move(g), std::move(fc), std::move(ac)});
    t->th = std::thread([st, shared, pl, c, cm, cc, ct, dl, fl, hq, pq]() mutable {
        {
            debug::thread_started();
            debug::adopt(pl->g); debug::adopt(pl->f); for (auto& x : pl->a) debug::adopt(x);
            Interp child(shared, as_ref<Globals>(pl->g));
            child.task = st; child.caps = c; child.child_mem = cm; child.child_cpu_s = cc; child.child_timeout_ms = ct; child.deadline_ns = dl; child.file = fl;
            child.handle_quota = std::move(hq); child.proc_quota = std::move(pq);
            Value result; bool failed = false; std::string et, em;
            try { result = child.call(pl->f, pl->a); }
            catch (...) {
                auto e = child.from_cpp(std::current_exception());
                failed = true;
                if (auto* i = as<Instance>(e.v)) { et = i->cls->name; auto it = i->fields.find("message"); em = it != i->fields.end() ? child.str(it->second) : ""; }
                else { et = "Error"; em = child.str(e.v); }
            }
            pl.reset();
            std::lock_guard lk(st->m);
            st->result = std::move(result); st->failed = failed; st->error_type = et; st->error_msg = em; st->done = true;
        }
        debug::thread_finished();
        Heap::instance().active_tasks.fetch_sub(1);
        st->cv.notify_all();
    });
    return obj(t);
}
Value Interp::await(const Value& tv) {
    auto* t = as<Task>(tv);
    if (!t) { if (auto* l = as<List>(tv)) { auto out = make<List>(); for (auto& x : l->items) out->push(await(x)); return obj(out); } return tv; }
    {
        std::unique_lock g(t->st->m);
        debug::Blocked w(*this, "await");
        while (!t->st->done) { t->st->cv.wait_for(g, std::chrono::milliseconds(20)); if (sh->interrupt.load() || (task && task->cancel.load())) { t->st->cancel = true; } if (!t->st->done) w.poll(); }
    }
    debug::adopt(t->st->result);
    if (t->th.joinable()) t->th.join();
    if (t->st->failed) {
        std::string ty = sh->errors.count(t->st->error_type) ? t->st->error_type : "Error";
        raise(ty, t->st->error_msg);
    }
    return t->st->result;
}

// ---------------------------------------------------------------- modules
bool is_native_module(const std::string& p) {
    auto e = std::filesystem::path(p).extension().string();
    return e == ".so" || e == ".dylib" || e == ".dll";
}

std::vector<std::string> module_search_path(const Interp& in) {
    std::vector<std::string> out;
    if (!in.file.empty()) out.push_back(std::filesystem::path(in.file).parent_path().string());
    if (!in.sh->base_dir.empty()) out.push_back(in.sh->base_dir);
    auto mp = os::getenv("FORGE_SCRIPT_PATH");
    std::size_t i = 0;
#if defined(_WIN32)
    const char* seps = ";";
#else
    const char* seps = ":;";
#endif
    while (!mp.empty() && i <= mp.size()) { auto j = mp.find_first_of(seps, i); if (j == std::string::npos) j = mp.size(); if (j > i) out.push_back(mp.substr(i, j - i)); i = j + 1; }
    auto home = os::getenv("HOME"); if (home.empty()) home = os::getenv("USERPROFILE");
    if (!home.empty()) out.push_back((std::filesystem::path(home) / ".forge" / "modules").string());
    std::error_code ec; out.push_back(std::filesystem::current_path(ec).string());
    return out;
}

Value Interp::import_module(const std::string& path) {
    namespace fs = std::filesystem;
    if (!caps.fs) raise("PermissionError", "import needs file access, which this sandbox disables");
    fs::path p(path);
    // Names without an extension try `name.fs`, `name.cpp`, then a package directory `name/mod.fs|mod.cpp`.
    std::vector<fs::path> forms;
    if (p.extension().empty()) for (auto f : {p.string() + ".fs", p.string() + ".cpp", (p / "mod.fs").string(), (p / "mod.cpp").string()}) forms.emplace_back(f);
    else forms.push_back(p);
    std::vector<fs::path> cand;
    if (p.is_absolute()) cand = forms;
    else for (auto& dir : module_search_path(*this)) for (auto& f : forms) cand.push_back(fs::path(dir) / f);
    fs::path found;
    for (auto& c : cand) { std::error_code ec; if (fs::is_regular_file(c, ec)) { found = fs::weakly_canonical(c, ec); break; } }
    if (found.empty()) raise("ImportError", "module '" + path + "' not found (search path: FORGE_SCRIPT_PATH, script dir, ~/.forge/modules, cwd)");
    std::string key = found.string();
    enforce_scope(*this, SecScope::fs_read, key);
    if (is_native_module(key)) {
        if (!sh->load_plugin) raise("ImportError", "native plugins are not available in this build");
        return sh->load_plugin(*this, key);
    }
    {
        std::lock_guard g(sh->mod_m);
        if (auto it = sh->modules.find(key); it != sh->modules.end()) {
            if (!it->second) raise("ImportError", "import cycle through '" + path + "'");
            return obj(it->second);
        }
        sh->modules[key] = Ref<Globals>();
    }
    auto src = os::read_file(key);
    if (!src) { std::lock_guard lk(sh->mod_m); sh->modules.erase(key); raise("IOError", src.err.what); }
    // FSS INT-1/INT-2: integrity.scripts applies to imported modules too, checked on the buffer that gets parsed
    try { security::verify_artefact(*security::active_policy(), security::artefact::script, key, {reinterpret_cast<const std::uint8_t*>(src.value.data()), src.value.size()}); }
    catch (...) { std::lock_guard lk(sh->mod_m); sh->modules.erase(key); throw; }
    std::shared_ptr<const FnDecl> top;
    try { top = parse_program(src.value, key); }
    catch (const syntax_error& e) { std::lock_guard g(sh->mod_m); sh->modules.erase(key); raise("SyntaxError", e.what()); }
    auto g = make<Globals>();
    Interp sub(sh, g); sub.caps = caps; sub.depth = depth; sub.task = task; sub.deadline_ns = deadline_ns;
    sub.cpu_deadline_ns = cpu_deadline_ns; sub.child_mem = child_mem; sub.child_cpu_s = child_cpu_s; sub.child_timeout_ms = child_timeout_ms;
    sub.handle_quota = handle_quota; sub.proc_quota = proc_quota;
    try { sub.run_top(top, sh->use_vm); }
    catch (...) { std::lock_guard lk(sh->mod_m); sh->modules.erase(key); throw; }
    { std::lock_guard lk(sh->mod_m); sh->modules[key] = g; sh->keep.push_back(top); }
    return obj(g);
}

} // namespace forge::script
