// forge::script standard library. Thin bindings onto existing FORGE components:
// forge::json (json.*), forge::os (fs.*, os.*, path.*, time.*), forge::os::process (proc.*, shell blocks),
// forge::os::file_watcher (fs.watch), forge::hash::blake3 (hash.*), forge::task::pool (parallel),
// forge::mem::host_budget (mem.*, heap accounting).
#include <forge/os/oserror.hpp>
#include "interp.hpp"
#include "vm.hpp"
#include "parser.hpp"
#include <forge/hash/blake3.hpp>
#include <forge/script/plugin.h>
#include <forge/json/json.hpp>
#include <forge/os/clipboard.hpp>
#include <forge/os/console.hpp>
#include <forge/os/devices.hpp>
#include <forge/os/file_watch.hpp>
#include <forge/os/file_watch_tree.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/net.hpp>
#include <forge/os/service_control.hpp>
#include <forge/os/sysinfo.hpp>
#include <forge/os/time.hpp>
#include <forge/os/users.hpp>
#include <forge/os/os.hpp>
#include <forge/os/process.hpp>
#include <forge/os/signal.hpp>
#include <forge/os/sandbox.hpp>
#include <forge/os/process_pool.hpp>
#include <forge/script/script.hpp>
#include <forge/security/enforce.hpp>
#include <forge/security/plugin.hpp>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <forge/task/pool.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <format>
#include <regex>

namespace forge::script {
Value make_range(std::int64_t a, std::int64_t b, std::int64_t s);

void enforce_scope(Interp& in, SecScope s, std::string_view target) {
    auto p = security::active_policy();
    if (!p->active()) return;   // no FSS profile: unchanged behaviour
    using C = security::capability;
    C c = C::fs_read;
    switch (s) {
    case SecScope::fs_read: c = C::fs_read; break; case SecScope::fs_write: c = C::fs_write; break;
    case SecScope::net_connect: c = C::net_connect; break; case SecScope::net_listen: c = C::net_listen; break;
    case SecScope::exec: c = C::exec; break; case SecScope::env: c = C::env; break; case SecScope::plugin_load: c = C::plugin_load; break;
    }
    if (auto st = security::try_enforce(*p, c, target); !st) in.raise("PermissionError", st.err.what);
}

namespace {

using Args = std::vector<Value>;
using Fn = Native::fn_t;

Value native(const std::string& name, int lo, int hi, Fn f) { auto n = make<Native>(name, std::move(f), lo, hi); n->frozen = true; return obj(n); }

struct Lib {
    Ref<Map> m = make<Map>();
    std::string prefix;
    explicit Lib(std::string p) : prefix(std::move(p)) {}
    Lib& def(const std::string& n, int lo, int hi, Fn f) { m->set_str(n, native(prefix + "." + n, lo, hi, std::move(f))); return *this; }
    Lib& val(const std::string& n, Value v) { m->set_str(n, std::move(v)); return *this; }
    Value done() { m->frozen = true; return obj(m); }
};

std::string& s_of(Interp& in, const Value& v, const char* what) { auto* s = as<String>(v); if (!s) in.raise("TypeError", std::string(what) + " expects a string, got " + Interp::type_of(v)); return s->s; }
// A path argument checked against the active security policy (fs.read / fs.write roots; see enforce_scope).
const std::string& p_rd(Interp& in, const Value& v, const char* what) { auto& s = s_of(in, v, what); enforce_scope(in, SecScope::fs_read, s); return s; }
const std::string& p_wr(Interp& in, const Value& v, const char* what) { auto& s = s_of(in, v, what); enforce_scope(in, SecScope::fs_write, s); return s; }
std::int64_t i_of(Interp& in, const Value& v, const char* what) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return *i;
    if (auto* d = std::get_if<double>(&v); d && *d == std::floor(*d)) return std::int64_t(*d);
    in.raise("TypeError", std::string(what) + " expects an int, got " + Interp::type_of(v));
}
double f_of(Interp& in, const Value& v, const char* what) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return double(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    in.raise("TypeError", std::string(what) + " expects a number, got " + Interp::type_of(v));
}
List& l_of(Interp& in, const Value& v, const char* what) { auto* l = as<List>(v); if (!l) in.raise("TypeError", std::string(what) + " expects a list, got " + Interp::type_of(v)); return *l; }
Map& m_of(Interp& in, const Value& v, const char* what) { auto* m = as<Map>(v); if (!m) in.raise("TypeError", std::string(what) + " expects a map, got " + Interp::type_of(v)); return *m; }
const Value* opt(const Args& a, std::size_t i) { return i < a.size() && !is_nil(a[i]) ? &a[i] : nullptr; }
// Option bags: a map, or a braced list of {"key", value} pairs (what `{{"k", v}, ...}` evaluates to).
const Value* kv_find(const Value& bag, const std::string& k) {
    if (auto* m = as<Map>(bag)) return m->find_str(k);
    if (auto* l = as<List>(bag)) for (auto& e : l->items) if (auto* p = as<List>(e); p && p->items.size() == 2) if (auto* s = as<String>(p->items[0]); s && s->s == k) return &p->items[1];
    return nullptr;
}
const Value* opt_key(const Args& a, std::size_t i, const std::string& k) { if (i < a.size()) if (auto* v = kv_find(a[i], k)) return is_nil(*v) ? nullptr : v; return nullptr; }
void mut(Interp& in, const Obj& o) { if (o.frozen) in.raise("TypeError", std::string(o.type_name()) + " is frozen"); }
Value list_of(std::vector<Value> v) { return obj(make<List>(std::move(v))); }
Value strv(std::string s) { return obj(make<String>(std::move(s))); }
// Also records which kind of OS operation follows (for EPERM -> SandboxViolationError only when the active
// OS sandbox policy denies that kind; see os::os_op). Reset by ok()/raise_os().
void need(Interp& in, const bool& cap, const char* what) {
    os::detail::t_op = &cap == &in.caps.net ? os::os_op::net : &cap == &in.caps.proc ? os::os_op::exec
                     : &cap == &in.caps.fs ? os::os_op::fs_write : os::os_op::any;
    if (!cap) in.raise("PermissionError", std::string(what) + " is disabled in this sandbox");
}
// OS failures raise a typed ForgeScript exception (FileNotFoundError, SandboxViolationError, ...; see
// forge/os/oserror.hpp); `type` is the fallback for errors that don't classify more precisely.
[[noreturn]] void raise_os(Interp& in, const os::error& e, const char* type) {
    auto k = os::classify(e); os::detail::t_op = os::os_op::any;
    in.raise(k == os::os_errc::other || k == os::os_errc::io ? type : os::script_error_type(k), e.what);
}
template <class T> T ok(Interp& in, os::result<T> r, const char* type = "IOError") { if (!r) raise_os(in, r.err, type); os::detail::t_op = os::os_op::any; return std::move(r.value); }
void ok(Interp& in, const os::status& s, const char* type = "IOError") { if (!s) raise_os(in, s.err, type); os::detail::t_op = os::os_op::any; }
// errno / GetLastError() right now -> typed exception
[[noreturn]] void raise_last_os(Interp& in, const std::string& what) { int c = os::last_errno(); raise_os(in, os::error{c ? c : -1, what + ": " + os::error_text(c)}, "OSError"); }

// ---- json <-> values (forge::json)
json::value to_json(Interp& in, const Value& v, int depth = 0) {
    if (depth > 256) in.raise("ValueError", "json: nesting too deep (cycle?)");
    if (is_nil(v)) return nullptr;
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* i = std::get_if<std::int64_t>(&v)) return double(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* s = as<String>(v)) return s->s;
    if (auto* l = as<List>(v)) { json::array a; for (auto& x : l->items) a.push_back(to_json(in, x, depth + 1)); return a; }
    if (auto* m = as<Map>(v)) { json::object o; for (auto& [k, x] : m->entries) o.emplace_back(as<String>(k) ? as<String>(k)->s : in.str(k), to_json(in, x, depth + 1)); return o; }
    if (auto* i = as<Instance>(v)) {
        json::object o;
        std::vector<const Class*> chain; for (const Class* c = i->cls.get(); c; c = c->base.get()) chain.push_back(c);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) for (auto& f : (*it)->fields) if (auto x = i->fields.find(f); x != i->fields.end()) o.emplace_back(f, to_json(in, x->second, depth + 1));
        if (i->cls->is_dynamic) for (auto& [k, x] : i->fields) if (!i->cls->has_field(k)) o.emplace_back(k, to_json(in, x, depth + 1));
        return o;
    }
    in.raise("TypeError", "cannot convert " + Interp::type_of(v) + " to json");
}
Value from_json(const json::value& j) {
    switch (j.v.index()) {
    case 0: return Nil{};
    case 1: return j.b();
    case 2: { double d = j.num(); if (d == std::floor(d) && std::fabs(d) < 9.0e15) return std::int64_t(d); return d; }
    case 3: return strv(j.str());
    case 4: { auto l = make<List>(); for (auto& x : j.arr()) l->push(from_json(x)); return obj(l); }
    default: { auto m = make<Map>(); for (auto& [k, x] : j.obj()) m->set_str(k, from_json(x)); return obj(m); }
    }
}
void pretty(std::string& o, const json::value& v, int ind) {
    std::string pad(std::size_t(ind + 2), ' '), end(std::size_t(ind), ' ');
    if (v.is_arr() && !v.arr().empty()) { o += "[\n"; bool f = true; for (auto& x : v.arr()) { if (!f) o += ",\n"; f = false; o += pad; pretty(o, x, ind + 2); } o += "\n" + end + "]"; }
    else if (v.is_obj() && !v.obj().empty()) { o += "{\n"; bool f = true; for (auto& [k, x] : v.obj()) { if (!f) o += ",\n"; f = false; o += pad; o += json::dump(json::value(k)); o += ": "; pretty(o, x, ind + 2); } o += "\n" + end + "}"; }
    else o += json::dump(v);
}

// ---- sorting with script comparisons
bool less(Interp& in, const Value& a, const Value& b) { return Interp::truthy(in.binary(Op::Lt, a, b)); }

// An OS-resource handle. Inside limit({handles: N}) it counts against the quota until disposed; over the
// quota the resource is closed again and TooManyFilesError (the EMFILE error) is raised.
Value make_handle(Interp& in, std::string kind, std::shared_ptr<void> payload, std::function<void()> closer) {
    if (const Quota* full = in.handle_quota ? Quota::acquire(in.handle_quota) : nullptr) {
        if (closer) { try { closer(); } catch (...) {} }
        payload.reset();
        in.raise("TooManyFilesError", "open-handle limit of " + std::to_string(full->max) + " reached opening a " + kind + " handle");
    }
    auto h = make<Handle>(std::move(kind)); h->payload = std::move(payload); h->closer = std::move(closer); h->quota = in.handle_quota; return obj(h);
}
Handle& h_of(Interp& in, const Value& v, const std::string& kind) {
    auto* h = as<Handle>(v);
    if (!h || h->kind != kind) in.raise("TypeError", "expected a " + kind + " handle");
    if (h->disposed) in.raise("ObjectDisposedError", kind + " handle has been disposed");
    return *h;
}

Value run_process(Interp& in, Args& a) {
    need(in, in.caps.proc, "running processes");
    os::spawn_spec sp;
    if (auto* l = as<List>(a[0])) { for (auto& x : l->items) sp.argv.push_back(in.str(x)); }
    else {   // a command string: split on whitespace (no shell); use sh { } for shell syntax
        std::string s = s_of(in, a[0], "proc.run"), cur; bool q = false; char qc = 0;
        for (char c : s) { if (q) { if (c == qc) q = false; else cur += c; } else if (c == '"' || c == '\'') { q = true; qc = c; } else if (c == ' ' || c == '\t') { if (!cur.empty()) sp.argv.push_back(std::move(cur)), cur.clear(); } else cur += c; }
        if (!cur.empty()) sp.argv.push_back(cur);
    }
    if (sp.argv.empty()) in.raise("ValueError", "proc.run: empty command");
    enforce_scope(in, SecScope::exec, sp.argv[0]);
    in.charge_process(sp.argv[0]);
    sp.max_output = in.sh->limits.max_output;
    sp.limits.memory_bytes = in.child_mem; sp.limits.cpu_seconds = in.child_cpu_s; sp.timeout_ms = in.child_timeout_ms;
    if (auto* v = opt_key(a, 1, "cwd")) sp.cwd = in.str(*v);
    if (auto* v = opt_key(a, 1, "stdin")) sp.stdin_data = in.str(*v);
    if (auto* v = opt_key(a, 1, "timeout")) sp.timeout_ms = std::uint64_t(i_of(in, *v, "timeout"));
    if (auto* v = opt_key(a, 1, "merge")) sp.merge_stderr = Interp::truthy(*v);
    if (auto* v = opt_key(a, 1, "env")) for (auto& [k, x] : m_of(in, *v, "env").entries) sp.env_set[in.str(k)] = in.str(x);
    if (in.deadline_ns) { auto now = os::monotonic_ns(); std::uint64_t left = in.deadline_ns > now ? (in.deadline_ns - now) / 1000000 + 1 : 1; sp.timeout_ms = sp.timeout_ms ? std::min(sp.timeout_ms, left) : left; }
    auto r = os::spawn_wait(sp);
    if (!r) raise_os(in, r.err, "OSError");
    std::string cmd; for (auto& s : sp.argv) { if (!cmd.empty()) cmd += ' '; cmd += s; }
    Value res = in.proc_result(cmd, r->exit_code, std::move(r->out), std::move(r->err), r->wall_ms, r->timed_out);
    if (auto* v = opt_key(a, 1, "check"); v && Interp::truthy(*v)) {
        auto* i = as<Instance>(res);
        if (!Interp::truthy(i->fields["ok"])) in.raise("ShellError", cmd + " failed with exit code " + std::to_string(r->exit_code) + ": " + in.str(i->fields["err"]));
    }
    return res;
}

// run fn(args) with narrowed limits, restoring them afterwards (RAII)
struct Scope {
    Interp& in; Caps caps; std::uint64_t mem, cpu, to, dl, cdl, cap, acap; std::shared_ptr<Quota> hq, pq;
    explicit Scope(Interp& i) : in(i), caps(i.caps), mem(i.child_mem), cpu(i.child_cpu_s), to(i.child_timeout_ms), dl(i.deadline_ns), cdl(i.cpu_deadline_ns), cap(Heap::instance().soft_cap()),
                                acap(AllocQuota::cap), hq(i.handle_quota), pq(i.proc_quota) {}
    ~Scope() {
        in.caps = caps; in.child_mem = mem; in.child_cpu_s = cpu; in.child_timeout_ms = to; in.deadline_ns = dl; in.cpu_deadline_ns = cdl; Heap::instance().set_soft_cap(cap);
        AllocQuota::cap = acap; in.handle_quota = std::move(hq); in.proc_quota = std::move(pq);
    }
};

// Parse a whole string as a double (hex: digits as written by std::to_chars(..., chars_format::hex), no "0x").
// Older libc++ (FreeBSD 14 base, LLVM 18) has no floating-point std::from_chars, so fall back to strtod there.
[[maybe_unused]] static bool parse_double(std::string_view t, double& out, bool hex = false) {
    if (t.empty()) return false;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    auto r = hex ? std::from_chars(t.data(), t.data() + t.size(), out, std::chars_format::hex) : std::from_chars(t.data(), t.data() + t.size(), out);
    return r.ec == std::errc{} && r.ptr == t.data() + t.size();
#else
    if (t[0] == '+' || std::isspace(static_cast<unsigned char>(t[0]))) return false;
    std::string s(t);
    if (hex) { bool neg = s[0] == '-'; s = (neg ? "-0x" : "0x") + s.substr(neg ? 1 : 0); }
    char* end = nullptr; errno = 0; out = std::strtod(s.c_str(), &end);
    return errno == 0 && end == s.c_str() + s.size();
#endif
}

// ---- limit option values. Sizes: a number of bytes or a string "64K", "512M", "2G", "1T" (powers of 1024; "KB",
// "KiB", "MB", ... and a trailing "B" are accepted). Durations: a number (ms for timeout, seconds for cpu_s /
// proc_cpu) or a string with a unit "250us", "200ms", "5s", "2m", "1h".
std::pair<double, std::string> num_unit(Interp& in, const std::string& s, const char* key) {
    std::size_t i = 0; while (i < s.size() && s[i] == ' ') ++i;
    std::size_t b = i; while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    double v = 0; std::string num = s.substr(b, i - b);
    if (!parse_double(num, v)) in.raise("ValueError", std::string("limit: bad ") + key + " value \"" + s + "\"");
    while (i < s.size() && s[i] == ' ') ++i;
    std::string u; for (; i < s.size(); ++i) if (s[i] != ' ') u += char(std::tolower(static_cast<unsigned char>(s[i])));
    return {v, u};
}
std::uint64_t size_opt(Interp& in, const Value& v, const char* key) {
    if (auto* s = as<String>(v)) {
        auto [n, u] = num_unit(in, s->s, key);
        if (u.size() > 1 && u.back() == 'b') u.pop_back();             // KB, MiB -> k, mi
        if (u.size() == 2 && u.back() == 'i') u.pop_back();
        static const std::pair<const char*, double> units[] = {{"", 1}, {"k", 1024.0}, {"m", 1048576.0}, {"g", 1073741824.0}, {"t", 1099511627776.0}};
        for (auto& [name, mul] : units) if (u == name || (u == "b" && !*name)) {
            double bytes = n * mul;
            if (bytes >= 1.8e19) in.raise("ValueError", std::string("limit: ") + key + " \"" + s->s + "\" is too large");
            return std::uint64_t(bytes);
        }
        in.raise("ValueError", std::string("limit: unknown size unit in ") + key + " \"" + s->s + "\" (use K, M, G or T)");
    }
    double d = f_of(in, v, key);
    if (d < 0) in.raise("ValueError", std::string("limit: ") + key + " must not be negative");
    return std::uint64_t(d);
}
// duration in units of `unit_s` seconds (1e-3 for ms); bare numbers are already in that unit
double duration_opt(Interp& in, const Value& v, const char* key, double unit_s) {
    double out;
    if (auto* s = as<String>(v)) {
        auto [n, u] = num_unit(in, s->s, key);
        double secs;
        if (u.empty()) return n;
        if (u == "ns") secs = n * 1e-9; else if (u == "us") secs = n * 1e-6; else if (u == "ms") secs = n * 1e-3;
        else if (u == "s") secs = n; else if (u == "m" || u == "min") secs = n * 60; else if (u == "h") secs = n * 3600;
        else in.raise("ValueError", std::string("limit: unknown time unit in ") + key + " \"" + s->s + "\" (use ms, s, m or h)");
        out = secs / unit_s;
    } else out = f_of(in, v, key);
    if (out < 0) in.raise("ValueError", std::string("limit: ") + key + " must not be negative");
    return out;
}
std::int64_t count_opt(Interp& in, const Value& v, const char* key) {
    std::int64_t n = i_of(in, v, key);
    if (n < 0) in.raise("ValueError", std::string("limit: ") + key + " must not be negative");
    return n;
}

} // namespace

// ---- ProcessPool (forge::os::process_pool): script tasks in worker processes.
// Payload (JSON): {"src": source defining task(arg), "arg": <json>, "fs","net","proc","env": bools, "timeout_ms": n}.
// The worker runs src in a fresh engine with those capabilities (the pool already applied the matching OS
// sandbox to the worker process), calls task(arg) and answers {"ok":true,"value":<json>} or
// {"ok":false,"type":"<ErrorType>","msg":"..."}; script errors keep their exact type across the process boundary.
std::string script_process_task(std::string payload) {
    json::value p = json::parse(payload);
    auto get = [&](const char* k) -> const json::value* { for (auto& [n, v] : p.obj()) if (n == k) return &v; return nullptr; };
    auto flag = [&](const char* k) { auto* v = get(k); return v && v->v.index() == 1 && v->b(); };
    options o;
    o.allow_fs = flag("fs"); o.allow_net = flag("net"); o.allow_proc = flag("proc"); o.allow_env = flag("env");
    o.os_sandbox = options::os_sandbox_mode::off;   // the pool sandboxes the worker process itself
    o.cancel_signals = false;
    if (auto* t = get("timeout_ms"); t && t->v.index() == 2) o.timeout_ms = std::uint64_t(t->num());
    std::string out_text; o.out = [&out_text](std::string_view t) { if (out_text.size() < (1u << 20)) out_text += t; };
    std::string src = get("src") && get("src")->v.index() == 3 ? get("src")->str() : std::string();
    std::string arg = get("arg") ? json::dump(*get("arg")) : std::string("null");
    std::string result = "null"; bool got = false;
    engine e(o);
    e.define("__pp_arg", 0, 0, [&](const std::string&) { return arg; });
    e.define("__pp_result", 1, 1, [&](const std::string& a) {
        auto v = json::parse(a); result = v.arr().empty() ? std::string("null") : json::dump(v.arr()[0]); got = true; return std::string("null"); });
    auto r = e.run(src + "\n__pp_result(task(__pp_arg()));\n", "<process-task>");
    json::object res;
    if (r.ok && got) { res.emplace_back("ok", true); res.emplace_back("value", json::parse(result)); }
    else {
        res.emplace_back("ok", false);
        res.emplace_back("type", r.error_type.empty() ? std::string("RuntimeError") : r.error_type);
        std::string m = r.error; if (auto c = m.find(": "); c != std::string::npos && m.compare(0, c, r.error_type) == 0) m = m.substr(c + 2);
        res.emplace_back("msg", r.ok ? std::string("task(arg) was not called; define task(arg) in the source") : m);
    }
    res.emplace_back("out", out_text);
    return json::dump(json::value(std::move(res)));
}
void register_script_process_task() { static bool once = os::register_process_task("forge.script", script_process_task); (void)once; }
struct PoolBox { std::unique_ptr<os::process_pool> pool; bool fs = false, net = false, proc = false, env = false; std::uint64_t timeout_ms = 0; };
// Graceful shutdown, but never past `deadline_ns` (the script's or limit block's): workers still busy then are killed.
static void close_pool(PoolBox& b, std::uint64_t deadline_ns) {
    if (!b.pool) return;
    if (deadline_ns) {
        auto now = os::monotonic_ns();
        if (now >= deadline_ns || !b.pool->wait_all_for((deadline_ns - now) / 1000000 + 1)) { b.pool->terminate(); b.pool.reset(); return; }
    }
    b.pool->shutdown(); b.pool.reset();
}
struct FutureBox { std::future<std::string> f; std::optional<std::string> got; std::exception_ptr err; };

void install_builtins(Shared& sh) {
    auto& B = sh.builtins;
    auto& M = sh.methods;

    // ------------------------------------------------ error hierarchy
    auto mkclass = [&](const std::string& name, const Ref<Class>& base) {
        auto c = make<Class>(); c->name = name; c->base = base; c->is_dynamic = true;
        if (!base) {
            c->fields = {"message", "trace", "line"}; c->field_defaults = {strv(""), strv(""), std::int64_t(0)};
            c->methods["init"] = native("Error.init", 1, 2, [](Interp& in, Args& a) -> Value {
                if (a.size() > 1) in.set_field(a[0], "message", is_nil(a[1]) ? strv("") : strv(in.str(a[1])));
                in.set_field(a[0], "trace", strv(in.trace_text())); in.set_field(a[0], "line", std::int64_t(in.line));
                return Nil{};
            });
            c->methods["to_string"] = native("Error.to_string", 1, 1, [](Interp& in, Args& a) -> Value { return strv(in.error_message(a[0])); });
        }
        c->frozen = true;
        sh.errors[name] = c; B[name] = obj(c);
        return c;
    };
    auto err = mkclass("Error", {});
    for (auto n : {"IOError", "OSError", "TypeError", "IndexError", "KeyError", "OverflowError",
                   "ObjectDisposedError", "ShellError", "LimitExceededError", "NameError",
                   "AttributeError", "ValueError", "ArithmeticError", "SyntaxError", "ImportError", "RuntimeError", "LogicError",
                   "BorrowError", "NullPointerError", "FrozenError", "RecursionError", "SystemExit", "RaceError", "PipelineError", "PluginError", "AIError", "AccessError"})
        mkclass(n, err);
    // As in Python, OS-level conditions are OSError subclasses: catch (const OSError& e) gets all of them.
    for (auto n : {"PermissionError", "TimeoutError", "OutOfMemoryError", "CancelledError", "DeadlockError"}) mkclass(n, sh.errors["OSError"]);
    // typed OS errors (forge/os/oserror.hpp): catch OSError to get all of them
    for (auto n : {"FileNotFoundError", "FileExistsError", "NotSupportedError", "InterruptedError", "ConnectionError", "SignalError",
                   "BlockingIOError", "NotADirectoryError", "IsADirectoryError", "ProcessLookupError", "NoSpaceError",
                   "TooManyFilesError", "BusyError", "ReadOnlyFileSystemError"})
        if (!sh.errors.count(n)) mkclass(n, sh.errors["OSError"]);
    for (auto n : {"ConnectionRefusedError", "ConnectionResetError", "BrokenPipeError"}) mkclass(n, sh.errors["ConnectionError"]);
    mkclass("SandboxViolationError", sh.errors["PermissionError"]);   // denied by the OS sandbox, not the interpreter
    // FSS-ERR-1 (forge::security): security::integrity_error / policy_violation. permission_denied and sandbox_error keep
    // the existing PermissionError / SandboxViolationError classes.
    mkclass("SecurityError", sh.errors["OSError"]);
    for (auto n : {"IntegrityError", "PolicyViolation"}) mkclass(n, sh.errors["SecurityError"]);
    sh.errors["ZeroDivisionError"] = sh.errors["ArithmeticError"];

    // ------------------------------------------------ ProcResult
    {
        auto c = make<Class>(); c->name = "ProcResult";
        c->fields = {"cmd", "code", "ok", "out", "err", "ms", "timed_out"};
        c->field_defaults = {strv(""), std::int64_t(0), false, strv(""), strv(""), std::int64_t(0), false};
        c->methods["get_json"] = native("ProcResult.json", 1, 1, [](Interp& in, Args& a) -> Value {
            std::string s = in.str(as<Instance>(a[0])->fields["out"]);
            try { return from_json(json::parse(s)); } catch (const std::exception& e) { in.raise("ValueError", std::string("output is not json: ") + e.what()); }
        });
        c->methods["get_lines"] = native("ProcResult.lines", 1, 1, [](Interp& in, Args& a) -> Value {
            std::string s = in.str(as<Instance>(a[0])->fields["out"]); auto l = make<List>(); std::size_t i = 0;
            while (i < s.size()) { auto j = s.find('\n', i); if (j == std::string::npos) j = s.size(); std::string ln = s.substr(i, j - i); if (!ln.empty() && ln.back() == '\r') ln.pop_back(); l->push(strv(ln)); i = j + 1; }
            return obj(l);
        });
        c->methods["check"] = native("ProcResult.check", 1, 1, [](Interp& in, Args& a) -> Value {
            auto* i = as<Instance>(a[0]);
            if (!Interp::truthy(i->fields["ok"])) in.raise("ShellError", in.str(i->fields["cmd"]) + " failed with exit code " + in.str(i->fields["code"]) + (in.str(i->fields["err"]).empty() ? "" : ": " + in.str(i->fields["err"])));
            return a[0];
        });
        c->methods["to_string"] = native("ProcResult.to_string", 1, 1, [](Interp& in, Args& a) -> Value { return strv(in.str(as<Instance>(a[0])->fields["out"])); });
        c->frozen = true;
        sh.classes["ProcResult"] = c; B["ProcResult"] = obj(c);
    }
    { auto c = make<Class>(); c->name = "Disposable"; c->is_interface = true; c->required = {"dispose"}; c->frozen = true; B["Disposable"] = obj(c); }

    // ------------------------------------------------ core functions
    B["print"] = native("print", 0, -1, [](Interp& in, Args& a) -> Value {
        std::string s; for (std::size_t i = 0; i < a.size(); ++i) { if (i) s += ' '; s += in.str(a[i]); } s += '\n';
        in.sh->out(s); return Nil{};
    });
    B["write"] = native("write", 0, -1, [](Interp& in, Args& a) -> Value { std::string s; for (auto& x : a) s += in.str(x); in.sh->out(s); return Nil{}; });
    B["str"] = native("str", 1, 1, [](Interp& in, Args& a) -> Value { return strv(in.str(a[0])); });
    B["repr"] = native("repr", 1, 1, [](Interp& in, Args& a) -> Value { return strv(in.repr(a[0])); });
    B["type"] = native("type", 1, 1, [](Interp&, Args& a) -> Value {
        if (auto* i = as<Instance>(a[0])) return strv(i->cls->name);
        if (as<Function>(a[0]) || as<Native>(a[0]) || as<Bound>(a[0])) return strv("fn");
        return strv(Interp::type_of(a[0]));
    });
    B["len"] = native("len", 1, 1, [](Interp& in, Args& a) -> Value {
        if (auto* s = as<String>(a[0])) return std::int64_t(s->s.size());
        if (auto* l = as<List>(a[0])) return std::int64_t(l->items.size());
        if (auto* m = as<Map>(a[0])) return std::int64_t(m->entries.size());
        in.raise("TypeError", "len() of " + Interp::type_of(a[0]));
    });
    B["int"] = native("int", 1, 2, [](Interp& in, Args& a) -> Value {
        if (auto* i = std::get_if<std::int64_t>(&a[0])) return *i;
        if (auto* d = std::get_if<double>(&a[0])) { if (!std::isfinite(*d) || std::fabs(*d) >= 9.2e18) in.raise("OverflowError", "float out of int range"); return std::int64_t(*d); }
        if (auto* b = std::get_if<bool>(&a[0])) return std::int64_t(*b);
        if (auto* s = as<String>(a[0])) {
            int base = a.size() > 1 ? int(i_of(in, a[1], "int base")) : 10;
            std::string t = s->s; std::erase(t, '_');
            std::size_t pos = 0; long long v = 0;
            try { v = std::stoll(t, &pos, base); } catch (...) { pos = 0; }
            if (t.empty() || pos != t.size()) { if (opt(a, 1) && false) {} in.raise("ValueError", "invalid int: " + in.repr(a[0])); }
            return std::int64_t(v);
        }
        in.raise("TypeError", "int() of " + Interp::type_of(a[0]));
    });
    B["float"] = native("float", 1, 1, [](Interp& in, Args& a) -> Value {
        if (auto* s = as<String>(a[0])) { std::size_t pos = 0; double d = 0; try { d = std::stod(s->s, &pos); } catch (...) { pos = 0; } if (s->s.empty() || pos != s->s.size()) in.raise("ValueError", "invalid float: " + in.repr(a[0])); return d; }
        return f_of(in, a[0], "float()");
    });
    B["bool"] = native("bool", 1, 1, [](Interp&, Args& a) -> Value { return Interp::truthy(a[0]); });
    B["range"] = native("range", 1, 3, [](Interp& in, Args& a) -> Value {
        std::int64_t lo = 0, hi, st = 1;
        if (a.size() == 1) hi = i_of(in, a[0], "range"); else { lo = i_of(in, a[0], "range"); hi = i_of(in, a[1], "range"); }
        if (a.size() == 3) st = i_of(in, a[2], "range");
        if (st == 0) in.raise("ValueError", "range step must not be 0");
        return make_range(lo, hi, st);
    });
    B["abs"] = native("abs", 1, 1, [](Interp& in, Args& a) -> Value { if (auto* i = std::get_if<std::int64_t>(&a[0])) { if (*i == INT64_MIN) in.raise("OverflowError", "abs overflow"); return *i < 0 ? -*i : *i; } return std::fabs(f_of(in, a[0], "abs")); });
    auto minmax = [](bool mx) {
        return [mx](Interp& in, Args& a) -> Value {
            const std::vector<Value>* src = &a; if (a.size() == 1) src = &l_of(in, a[0], mx ? "max" : "min").items;
            if (src->empty()) in.raise("ValueError", "empty sequence");
            Value best = (*src)[0];
            for (auto& x : *src) if (mx ? less(in, best, x) : less(in, x, best)) best = x;
            return best;
        };
    };
    B["min"] = native("min", 1, -1, minmax(false));
    B["max"] = native("max", 1, -1, minmax(true));
    B["assert"] = native("assert", 1, 2, [](Interp& in, Args& a) -> Value { if (!Interp::truthy(a[0])) in.raise("Error", a.size() > 1 ? in.str(a[1]) : "assertion failed"); return Nil{}; });
    B["weak"] = native("weak", 1, 1, [](Interp& in, Args& a) -> Value {
        auto* r = std::get_if<Ref<Obj>>(&a[0]); if (!r || !*r) in.raise("TypeError", "weak() needs an object");
        Obj& o = **r; if (!o.weak) { o.weak = std::make_shared<WeakCell>(); o.weak->target = &o; }
        return obj(make<WeakRef>(o.weak));
    });
    B["freeze"] = native("freeze", 1, 1, [](Interp&, Args& a) -> Value {
        std::function<void(const Value&, int)> fr = [&](const Value& v, int d) {
            auto* r = std::get_if<Ref<Obj>>(&v); if (!r || !*r || d > 256) return;
            (*r)->frozen = true;
            if (auto* l = as<List>(v)) for (auto& x : l->items) fr(x, d + 1);
            if (auto* m = as<Map>(v)) for (auto& [k, x] : m->entries) fr(x, d + 1);
            if (auto* i = as<Instance>(v)) for (auto& [k, x] : i->fields) fr(x, d + 1);
        };
        fr(a[0], 0); return a[0];
    });
    B["is_frozen"] = native("is_frozen", 1, 1, [](Interp&, Args& a) -> Value { auto* r = std::get_if<Ref<Obj>>(&a[0]); return !r || !*r || (*r)->frozen || as<String>(a[0]); });
    B["channel"] = native("channel", 0, 1, [](Interp& in, Args& a) -> Value { return obj(make<Channel>(a.empty() ? 0 : std::size_t(i_of(in, a[0], "channel")))); });
    B["dispose"] = native("dispose", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
    B["sleep"] = native("sleep", 1, 1, [](Interp& in, Args& a) -> Value {
        double ms = f_of(in, a[0], "sleep"); auto end = os::monotonic_ns() + std::uint64_t(std::max(0.0, ms) * 1e6);
        while (os::monotonic_ns() < end) { os::sleep_ns(std::min<std::uint64_t>(end - os::monotonic_ns(), 10'000'000)); in.step(); if (in.sh->interrupt.load()) in.step(); }
        return Nil{};
    });
    B["eval"] = native("eval", 1, 1, [](Interp& in, Args& a) -> Value {
        std::string src = s_of(in, a[0], "eval");
        auto top = parse_program(src, "<eval>");
        in.sh->keep.push_back(top);
        return in.run_top(top, false);
    });
    B["sorted"] = native("sorted", 1, 2, [](Interp& in, Args& a) -> Value {
        std::vector<Value> v = l_of(in, a[0], "sorted").items;
        if (a.size() > 1) { Value key = a[1]; std::stable_sort(v.begin(), v.end(), [&](const Value& x, const Value& y) { Args ax{x}, ay{y}; return less(in, in.call(key, ax), in.call(key, ay)); }); }
        else std::stable_sort(v.begin(), v.end(), [&](const Value& x, const Value& y) { return less(in, x, y); });
        return list_of(std::move(v));
    });

    // parallel(list, fn): map over a list on the forge::task pool. Each item runs in its own interpreter
    // with deep-copied inputs (no shared mutable state), results come back in order.
    B["parallel"] = native("parallel", 2, 3, [](Interp& in, Args& a) -> Value {
        auto items = l_of(in, a[0], "parallel").items;
        Value fn = a[1];
        std::size_t n = items.size();
        std::vector<Value> out(n);
        std::vector<std::string> errs(n), etypes(n);
        std::unordered_map<const Obj*, Value> memo;
        Value g0 = deep_copy(in, obj(in.globals), memo);
        Value f0 = deep_copy(in, fn, memo);
        std::vector<Value> in_items; for (auto& x : items) in_items.push_back(deep_copy(in, x, memo));
        memo.clear();
        std::mutex copy_m;   // deep copies of the shared function per chunk
        Heap::instance().active_tasks.fetch_add(1);
        struct Dec { ~Dec() { Heap::instance().active_tasks.fetch_sub(1); } } dec;
        auto shared = in.sh; Caps caps = in.caps; auto dl = in.deadline_ns;
        task::parallel_for(n, 1, [&](std::size_t b, std::size_t e) {
            Value g, f;
            { std::lock_guard lk(copy_m); std::unordered_map<const Obj*, Value> mm; g = deep_copy(in, g0, mm); f = deep_copy(in, f0, mm); }
            Interp child(shared, as_ref<Globals>(g)); child.caps = caps; child.deadline_ns = dl; child.file = in.file;
            child.child_mem = in.child_mem; child.child_cpu_s = in.child_cpu_s; child.child_timeout_ms = in.child_timeout_ms;
            child.handle_quota = in.handle_quota; child.proc_quota = in.proc_quota;
            for (std::size_t i = b; i < e; ++i) {
                try { Args x{in_items[i], std::int64_t(i)}; auto* fnp = as<Function>(f); if (fnp && fnp->decl->params.size() < 2) x.pop_back(); out[i] = child.call(f, x); }
                catch (...) { auto se = child.from_cpp(std::current_exception()); auto* inst = as<Instance>(se.v); etypes[i] = inst ? inst->cls->name : "Error"; errs[i] = se.msg; }
            }
        });
        for (std::size_t i = 0; i < n; ++i) if (!errs[i].empty()) in.raise(in.sh->errors.count(etypes[i]) ? etypes[i] : "Error", "parallel item " + std::to_string(i) + ": " + errs[i]);
        return list_of(std::move(out));
    });

    // limit(opts) { ... } and sandbox(opts) { ... }: run the trailing block with narrower limits / capabilities
    B["limit"] = native("limit", 2, 2, [](Interp& in, Args& a) -> Value {
        Scope s(in);
        if (!as<Map>(a[0]) && !as<List>(a[0])) in.raise("TypeError", "limit expects a map of options, got " + Interp::type_of(a[0]));
        if (auto* l = as<List>(a[0])) for (auto& e : l->items) { auto* p = as<List>(e); if (!p || p->items.size() != 2 || !as<String>(p->items[0])) in.raise("TypeError", "limit options must be {\"key\", value} pairs"); }
        static const char* known[] = {"timeout", "cpu_s", "mem", "proc_mem", "proc_cpu", "allocs", "handles", "procs"};
        auto check_keys = [&](const Value& k) {
            std::string ks = in.str(k);
            if (std::find_if(std::begin(known), std::end(known), [&](const char* n) { return ks == n; }) == std::end(known))
                in.raise("ValueError", "limit: unknown option '" + ks + "' (timeout, cpu_s, mem, proc_mem, proc_cpu, allocs, handles, procs)");
        };
        if (auto* m = as<Map>(a[0])) for (auto& [k, x] : m->entries) check_keys(k);
        else for (auto& e : as<List>(a[0])->items) check_keys(as<List>(e)->items[0]);
        auto get = [&](const char* k) { return opt_key(a, 0, k); };
        // Read every option before applying any, so a bad value leaves the limits unchanged.
        const Value *to = get("timeout"), *mem = get("mem"), *cpu = get("cpu_s"), *pm = get("proc_mem"), *pc = get("proc_cpu"), *al = get("allocs"), *hd = get("handles"), *pr = get("procs");
        double to_ms = to ? duration_opt(in, *to, "timeout", 1e-3) : 0, cpu_s = cpu ? duration_opt(in, *cpu, "cpu_s", 1.0) : 0, pc_s = pc ? duration_opt(in, *pc, "proc_cpu", 1.0) : 0;
        std::uint64_t mem_b = mem ? size_opt(in, *mem, "mem") : 0, pm_b = pm ? size_opt(in, *pm, "proc_mem") : 0;
        std::int64_t n_al = al ? count_opt(in, *al, "allocs") : 0, n_hd = hd ? count_opt(in, *hd, "handles") : 0, n_pr = pr ? count_opt(in, *pr, "procs") : 0;
        if (to) { auto d = os::monotonic_ns() + std::uint64_t(to_ms * 1e6); in.deadline_ns = in.deadline_ns ? std::min(in.deadline_ns, d) : d; in.child_timeout_ms = std::uint64_t(std::ceil(to_ms)); }
        if (mem) { auto cap = Heap::instance().live_bytes() + mem_b; auto old = Heap::instance().soft_cap(); Heap::instance().set_soft_cap(old ? std::min(old, cap) : cap); }
        if (cpu) { auto d = os::thread_cpu_time_ns() + std::uint64_t(cpu_s * 1e9); in.cpu_deadline_ns = in.cpu_deadline_ns ? std::min(in.cpu_deadline_ns, d) : d; }
        if (pm) in.child_mem = in.child_mem ? std::min(in.child_mem, pm_b) : pm_b;
        if (pc) { auto c = std::uint64_t(std::ceil(pc_s)); in.child_cpu_s = in.child_cpu_s ? std::min(in.child_cpu_s, c) : c; }
        if (al) {   // allocations on this thread from now on (AllocQuota in value.hpp)
            if (!AllocQuota::cap) AllocQuota::count = 0;
            auto c = AllocQuota::count + std::uint64_t(n_al);
            AllocQuota::cap = AllocQuota::cap ? std::min(AllocQuota::cap, c) : c;
            if (!AllocQuota::cap) { AllocQuota::count = 1; AllocQuota::cap = 1; }   // allocs: 0 (cap 0 means "none")
        }
        if (hd) in.handle_quota = std::make_shared<Quota>(n_hd, in.handle_quota);
        if (pr) in.proc_quota = std::make_shared<Quota>(n_pr, in.proc_quota);
        Args none; return in.call(a[1], none);
    });
    B["sandbox"] = native("sandbox", 2, 2, [](Interp& in, Args& a) -> Value {
        Scope s(in);
        auto& o = m_of(in, a[0], "sandbox");
        auto flag = [&](const char* k, bool& cap) { if (auto* v = o.find_str(k)) cap = cap && Interp::truthy(*v); };   // can only narrow
        Caps before = in.caps;
        flag("fs", in.caps.fs); flag("net", in.caps.net); flag("proc", in.caps.proc); flag("env", in.caps.env); flag("reflect", in.caps.reflect);
        const Value* iso = o.find_str("isolate");
        if (!iso || !Interp::truthy(*iso)) { Args none; return in.call(a[1], none); }
        // isolate: true — run the block in a forked child under a narrower OS sandbox (docs/scripting.md
        // "Per-block OS sandbox"). The child gets a copy of the heap: changes to captured variables, globals and
        // objects stay in the child; the block's return value (JSON-representable values only) and its print()
        // output come back over a pipe, and an exception is re-raised here with its type and message.
        os::sandbox_policy pol;
        pol.deny_network = before.net && !in.caps.net; pol.deny_exec = before.proc && !in.caps.proc; pol.deny_fs_write = before.fs && !in.caps.fs;
#if defined(_WIN32) || defined(FORGE_OS_IOS)
        in.raise("PermissionError", "sandbox({isolate: true}) needs fork(), which this platform does not have; the block was not run");
        return Value{};
#else
        int fd[2]; if (::pipe(fd) != 0) raise_last_os(in, "sandbox isolate: pipe");
        std::fflush(stdout); std::fflush(stderr);
        pid_t pid = forge::os::serialized_fork();
        if (pid < 0) { int e = errno; ::close(fd[0]); ::close(fd[1]); errno = e; raise_last_os(in, "sandbox isolate: fork"); }
        if (pid == 0) {
            ::close(fd[0]);
            json::object res; std::string out;
            in.sh->out = [&out](std::string_view t) { if (out.size() < (16u << 20)) out += t; };
            auto st = os::apply_sandbox(pol);
            if (!st) { res.emplace_back("ok", false); res.emplace_back("type", std::string("SandboxViolationError")); res.emplace_back("msg", "OS sandbox could not be applied in the isolated block: " + st.err.what); }
            else try { Args none; Value v = in.call(a[1], none); res.emplace_back("ok", true); res.emplace_back("value", to_json(in, v)); }
            catch (...) { auto se = in.from_cpp(std::current_exception()); auto* inst = as<Instance>(se.v); res.emplace_back("ok", false); res.emplace_back("type", std::string(inst ? inst->cls->name : "Error")); res.emplace_back("msg", se.msg); }
            res.emplace_back("out", out);
            std::string bytes; try { bytes = json::dump(json::value(std::move(res))); } catch (...) { bytes = "{\"ok\":false,\"type\":\"ValueError\",\"msg\":\"isolated block result is not JSON-representable\"}"; }
            for (std::size_t w = 0; w < bytes.size();) { auto n = ::write(fd[1], bytes.data() + w, bytes.size() - w); if (n <= 0) { if (errno == EINTR) continue; break; } w += std::size_t(n); }
            ::_exit(0);
        }
        ::close(fd[1]);
        std::string buf; char tmp[65536];
        for (;;) { auto n = ::read(fd[0], tmp, sizeof tmp); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; buf.append(tmp, std::size_t(n)); }
        ::close(fd[0]);
        int wst = 0; while (::waitpid(pid, &wst, 0) < 0 && errno == EINTR) {}
        if (buf.empty()) in.raise("SandboxViolationError", WIFSIGNALED(wst) ? "isolated block killed by signal " + std::to_string(WTERMSIG(wst)) : std::string("isolated block exited without a result"));
        json::value r; try { r = json::parse(buf); } catch (const std::exception& e) { in.raise("RuntimeError", std::string("sandbox isolate: bad child reply: ") + e.what()); }
        if (!r.is_obj()) in.raise("RuntimeError", "sandbox isolate: bad child reply");
        if (auto o2 = r.str_or("out", ""); !o2.empty()) in.sh->out(o2);
        auto* okp = r.find("ok");
        if (!okp || !okp->is_bool() || !okp->b()) { std::string ty = r.str_or("type", "Error"); in.raise(in.sh->errors.count(ty) ? ty : "Error", r.str_or("msg", "")); }
        auto* vp = r.find("value"); return vp ? from_json(*vp) : Value{};
#endif
    });

    // ------------------------------------------------ gc / mem (forge::mem)
    B["gc"] = Lib("gc")
        .def("collect", 0, 0, [](Interp&, Args&) -> Value { return std::int64_t(Heap::instance().collect()); })
        .def("stats", 0, 0, [](Interp&, Args&) -> Value {
            auto m = make<Map>(); auto& h = Heap::instance();
            m->set_str("live_bytes", std::int64_t(h.live_bytes())); m->set_str("live_objects", std::int64_t(h.live_objects()));
            m->set_str("collections", std::int64_t(h.collections())); m->set_str("collected", std::int64_t(h.collected()));
            m->set_str("soft_cap", std::int64_t(h.soft_cap()));
            return obj(m);
        }).done();
    B["mem"] = Lib("mem")
        .def("stats", 0, 0, [](Interp&, Args&) -> Value {
            auto& b = mem::host_budget(); auto m = make<Map>();
            m->set_str("limit", std::int64_t(b.limit())); m->set_str("used", std::int64_t(b.used()));
            m->set_str("script_heap", std::int64_t(Heap::instance().live_bytes())); m->set_str("total_ram", std::int64_t(os::total_memory()));
            return obj(m);
        }).done();

    // ------------------------------------------------ fs (forge::os)
    B["fs"] = Lib("fs")
        .def("read", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); return strv(ok(in, os::read_file(p_rd(in, a[0], "fs.read")))); })
        .def("write", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); auto d = in.str(a[1]); ok(in, os::write_file(p_wr(in, a[0], "fs.write"), d)); return std::int64_t(d.size()); })
        .def("append", 2, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access"); auto d = in.str(a[1]);
            auto f = ok(in, os::file::open(p_wr(in, a[0], "fs.append"), os::wr | os::create | os::append)); ok(in, f.write_all(d.data(), d.size())); return std::int64_t(d.size());
        })
        .def("exists", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); return os::exists(p_rd(in, a[0], "fs.exists")); })
        .def("is_dir", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); return os::stat(p_rd(in, a[0], "fs.is_dir")).is_dir; })
        .def("stat", 1, 1, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access"); auto st = os::stat(p_rd(in, a[0], "fs.stat")); auto m = make<Map>();
            m->set_str("exists", st.exists); m->set_str("is_dir", st.is_dir); m->set_str("is_file", st.is_file); m->set_str("is_link", st.is_link);
            m->set_str("size", std::int64_t(st.size)); m->set_str("mtime", double(st.mtime_ns) / 1e9); return obj(m);
        })
        .def("list", 0, 1, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access"); auto es = ok(in, os::list_dir(a.empty() ? (enforce_scope(in, SecScope::fs_read, "."), std::string(".")) : p_rd(in, a[0], "fs.list")));
            std::sort(es.begin(), es.end(), [](auto& x, auto& y) { return x.name < y.name; });
            auto l = make<List>(); for (auto& e : es) l->push(strv(e.name + (e.is_dir ? "/" : ""))); return obj(l);
        })
        .def("mkdir", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::mkdirs(p_wr(in, a[0], "fs.mkdir"))); return Nil{}; })
        .def("remove", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::remove_all(p_wr(in, a[0], "fs.remove"))); return Nil{}; })
        .def("rename", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::rename(p_wr(in, a[0], "fs.rename"), p_wr(in, a[1], "fs.rename"))); return Nil{}; })
        .def("temp_dir", 0, 0, [](Interp&, Args&) -> Value { return strv(os::temp_dir()); })
        .def("open", 1, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access");
            std::string mode = a.size() > 1 ? s_of(in, a[1], "fs.open mode") : "r";
            unsigned fl = mode == "r" ? os::rd : mode == "w" ? (os::wr | os::create | os::truncate) : mode == "a" ? (os::wr | os::create | os::append) : mode == "rw" ? (os::rdwr | os::create) : 0u;
            if (!fl) in.raise("ValueError", "fs.open mode must be r, w, a or rw");
            auto f = std::make_shared<os::file>(ok(in, os::file::open(mode == "r" ? p_rd(in, a[0], "fs.open") : p_wr(in, a[0], "fs.open"), fl)));
            Value h = make_handle(in, "file", f, [f]() { *f = os::file(); });
            as<Handle>(h)->props["path"] = a[0];
            return h;
        })
        .def("copy", 2, 3, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access"); bool ow = a.size() > 2 && in.truthy(a[2]);
            ok(in, os::fs::copy(p_rd(in, a[0], "fs.copy"), p_wr(in, a[1], "fs.copy"), {ow, true, false, !ow})); return Nil{};
        })
        .def("move", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::fs::move(p_wr(in, a[0], "fs.move"), p_wr(in, a[1], "fs.move"))); return Nil{}; })
        .def("glob", 1, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access");
            const std::string& pat = s_of(in, a[0], "fs.glob"); std::string base = a.size() > 1 ? p_rd(in, a[1], "fs.glob base") : std::string(".");
            if (a.size() <= 1) enforce_scope(in, SecScope::fs_read, base);
            // the pattern's literal directory prefix ("/etc/x*" or "../../*") must lie inside the read roots too
            std::size_t cut = std::string::npos;
            for (std::size_t i = 0; i < pat.size() && pat[i] != '*' && pat[i] != '?' && pat[i] != '['; ++i) if (pat[i] == '/' || pat[i] == '\\') cut = i;
            if (cut != std::string::npos) { auto d = pat.substr(0, cut ? cut : 1); enforce_scope(in, SecScope::fs_read, std::filesystem::path(d).is_absolute() || d.front() == '/' ? d : os::join(base, d)); }
            auto r = ok(in, os::fs::glob(pat, base));
            auto l = make<List>(); for (auto& p : r) l->push(strv(p)); return obj(l);
        })
        .def("walk", 1, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access"); os::fs::walk_opts o; if (a.size() > 1) o.max_depth = int(i_of(in, a[1], "fs.walk depth"));
            auto r = ok(in, os::fs::walk(p_rd(in, a[0], "fs.walk"), o)); auto l = make<List>();
            for (auto& e : r) { auto m = make<Map>(); m->set_str("path", strv(e.path)); m->set_str("is_dir", e.is_dir); m->set_str("is_link", e.is_link); m->set_str("size", std::int64_t(e.size)); m->set_str("depth", std::int64_t(e.depth)); l->push(obj(m)); }
            return obj(l);
        })
        .def("match", 2, 2, [](Interp& in, Args& a) -> Value { return os::fs::glob_match(s_of(in, a[0], "fs.match"), s_of(in, a[1], "fs.match")); })
        .def("symlink", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::fs::symlink(p_rd(in, a[0], "fs.symlink"), p_wr(in, a[1], "fs.symlink"))); return Nil{}; })
        .def("readlink", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); return strv(ok(in, os::fs::read_link(p_rd(in, a[0], "fs.readlink")))); })
        .def("chmod", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::fs::chmod(p_wr(in, a[0], "fs.chmod"), unsigned(i_of(in, a[1], "fs.chmod mode")))); return Nil{}; })
        .def("mode", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); return std::int64_t(ok(in, os::fs::mode(p_rd(in, a[0], "fs.mode")))); })
        .def("touch", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::fs::touch(p_wr(in, a[0], "fs.touch"))); return Nil{}; })
        .def("set_mtime", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::fs::set_mtime_ns(p_wr(in, a[0], "fs.set_mtime"), std::int64_t(f_of(in, a[1], "mtime") * 1e9))); return Nil{}; })
        .def("write_atomic", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); auto d = in.str(a[1]); ok(in, os::fs::write_file_atomic(p_wr(in, a[0], "fs.write_atomic"), d)); return std::int64_t(d.size()); })
        .def("temp_file", 0, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); enforce_scope(in, SecScope::fs_write, os::temp_dir()); return strv(ok(in, os::fs::make_temp_file(a.empty() ? "forge-" : s_of(in, a[0], "fs.temp_file")))); })
        .def("make_temp_dir", 0, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); enforce_scope(in, SecScope::fs_write, os::temp_dir()); return strv(ok(in, os::fs::make_temp_dir(a.empty() ? "forge-" : s_of(in, a[0], "fs.make_temp_dir")))); })
        .def("disk_usage", 1, 1, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access"); auto d = ok(in, os::fs::disk_usage(p_rd(in, a[0], "fs.disk_usage"))); auto m = make<Map>();
            m->set_str("total", std::int64_t(d.total)); m->set_str("free", std::int64_t(d.free)); m->set_str("available", std::int64_t(d.available)); return obj(m);
        })
        .def("lock", 1, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access"); bool wait = a.size() < 2 || in.truthy(a[1]);
            auto r = os::fs::file_lock::acquire(p_wr(in, a[0], "fs.lock"), true, wait);
            if (!r) { if (!wait) return Nil{}; in.raise("IOError", r.err.what); }
            auto l = std::make_shared<os::fs::file_lock>(std::move(r.value));
            return make_handle(in, "lock", l, [l]() { l->unlock(); });
        })
        .def("watch_tree", 1, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access");
            os::tree_watch_opts o;
            if (auto* v = opt_key(a, 1, "debounce_ms")) o.debounce_ms = int(i_of(in, *v, "debounce_ms"));
            if (auto* v = opt_key(a, 1, "polling")) o.force_polling = Interp::truthy(*v);
            if (auto* v = opt_key(a, 1, "hidden")) o.include_hidden = Interp::truthy(*v);
            if (auto* v = opt_key(a, 1, "ignore")) if (auto* l = as<List>(*v)) for (auto& x : l->items) o.ignore.push_back(in.str(x));
            auto w = ok(in, os::tree_watcher::open(p_rd(in, a[0], "fs.watch_tree"), o));
            std::shared_ptr<os::tree_watcher> sp(std::move(w));
            return make_handle(in, "treewatcher", sp, {});
        })
        .def("watch", 1, 1, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.fs, "file access");
            auto w = std::make_shared<os::file_watcher>();
            if (auto* l = as<List>(a[0])) { for (auto& x : l->items) { auto px = in.str(x); enforce_scope(in, SecScope::fs_read, px); ok(in, w->add(px)); } } else ok(in, w->add(p_rd(in, a[0], "fs.watch")));
            return make_handle(in, "watcher", w, {});
        }).done();
    M["handle:file"]["read"] = native("file.read", 1, 2, [](Interp& in, Args& a) -> Value {
        need(in, in.caps.fs, "file access");   // a handle opened before a sandbox({fs: false}) block is not usable inside it
        auto f = h_of(in, a[0], "file").payload_as<os::file>();
        std::size_t lim = a.size() > 1 ? std::size_t(i_of(in, a[1], "read")) : SIZE_MAX;
        std::string s; char buf[65536];
        while (s.size() < lim) { auto r = f->read(buf, std::min(sizeof buf, lim - s.size())); if (!r) in.raise("IOError", r.err.what); if (!r.value) break; s.append(buf, r.value); }
        return strv(std::move(s));
    });
    M["handle:file"]["write"] = native("file.write", 2, 2, [](Interp& in, Args& a) -> Value {
        need(in, in.caps.fs, "file access");
        auto f = h_of(in, a[0], "file").payload_as<os::file>(); auto d = in.str(a[1]); ok(in, f->write_all(d.data(), d.size())); return std::int64_t(d.size());
    });
    M["handle:file"]["close"] = native("file.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
    M["handle:watcher"]["poll"] = native("watcher.poll", 1, 2, [](Interp& in, Args& a) -> Value {
        auto w = h_of(in, a[0], "watcher").payload_as<os::file_watcher>();
        int to = a.size() > 1 ? int(i_of(in, a[1], "poll timeout")) : 0;
        auto l = make<List>();
        for (auto& e : w->poll(to)) { auto m = make<Map>(); m->set_str("path", strv(e.path)); m->set_str("kind", strv(os::watch_kind_name(e.kind))); l->push(obj(m)); }
        return obj(l);
    });
    M["handle:treewatcher"]["poll"] = native("treewatcher.poll", 1, 2, [](Interp& in, Args& a) -> Value {
        auto w = h_of(in, a[0], "treewatcher").payload_as<os::tree_watcher>();
        int to = a.size() > 1 ? int(i_of(in, a[1], "poll timeout")) : 0;
        auto l = make<List>();
        for (auto& e : w->poll(to)) { auto m = make<Map>(); m->set_str("path", strv(e.path)); m->set_str("kind", strv(os::watch_kind_name(e.kind))); l->push(obj(m)); }
        return obj(l);
    });
    M["handle:treewatcher"]["close"] = native("treewatcher.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
    M["handle:treewatcher"]["backend"] = native("treewatcher.backend", 1, 1, [](Interp& in, Args& a) -> Value { return strv(h_of(in, a[0], "treewatcher").payload_as<os::tree_watcher>()->backend()); });
    M["handle:lock"]["unlock"] = native("lock.unlock", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
    M["handle"]["dispose"] = native("handle.dispose", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
    M["handle"]["get_disposed"] = native("handle.disposed", 1, 1, [](Interp&, Args& a) -> Value { return as<Handle>(a[0])->disposed; });
    M["handle"]["get_kind"] = native("handle.kind", 1, 1, [](Interp&, Args& a) -> Value { return strv(as<Handle>(a[0])->kind); });

    // ------------------------------------------------ path / os / time
    B["path"] = Lib("path")
        .def("join", 1, -1, [](Interp& in, Args& a) -> Value { std::string p = in.str(a[0]); for (std::size_t i = 1; i < a.size(); ++i) p = os::join(p, in.str(a[i])); return strv(p); })
        .def("base", 1, 1, [](Interp& in, Args& a) -> Value { return strv(os::basename(s_of(in, a[0], "path.base"))); })
        .def("dir", 1, 1, [](Interp& in, Args& a) -> Value { return strv(os::dirname(s_of(in, a[0], "path.dir"))); })
        .def("ext", 1, 1, [](Interp& in, Args& a) -> Value { auto b = os::basename(s_of(in, a[0], "path.ext")); auto i = b.rfind('.'); return strv(i == std::string::npos || i == 0 ? "" : b.substr(i)); })
        .def("abs", 1, 1, [](Interp& in, Args& a) -> Value { return strv(os::absolute(s_of(in, a[0], "path.abs"))); }).done();
    B["os"] = Lib("os")
        .val("platform", strv(os::platform_name()))
        .def("pid", 0, 0, [](Interp&, Args&) -> Value { return std::int64_t(os::pid()); })
        .def("cwd", 0, 0, [](Interp&, Args&) -> Value { return strv(os::cwd()); })
        .def("chdir", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.fs, "file access"); ok(in, os::chdir(p_rd(in, a[0], "os.chdir"))); return Nil{}; })
        .def("env", 1, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.env, "environment access"); auto k = s_of(in, a[0], "os.env"); enforce_scope(in, SecScope::env, k);
            if (!os::has_env(k)) return a.size() > 1 ? a[1] : Value(Nil{});
            return strv(os::getenv(k));
        })
        .def("setenv", 2, 2, [](Interp& in, Args& a) -> Value { need(in, in.caps.env, "environment access"); enforce_scope(in, SecScope::env, s_of(in, a[0], "os.setenv")); return os::setenv(s_of(in, a[0], "os.setenv"), in.str(a[1])); })
        .def("cpu_count", 0, 0, [](Interp&, Args&) -> Value { return std::int64_t(os::cpu_count()); })
        .def("total_memory", 0, 0, [](Interp&, Args&) -> Value { return std::int64_t(os::total_memory()); })
        .def("home", 0, 0, [](Interp&, Args&) -> Value { return strv(os::home_dir()); })
        .def("caps", 0, 0, [](Interp& in, Args&) -> Value {
            auto l = make<List>(); for (auto& c : os::process_caps()) l->push(strv(c));
            auto m = make<Map>(); m->set_str("platform", strv(os::platform_name())); m->set_str("process", obj(l));
            m->set_str("fs", in.caps.fs); m->set_str("net", in.caps.net); m->set_str("proc", in.caps.proc); m->set_str("env", in.caps.env);
            return obj(m);
        })
        .def("processes", 0, 0, [](Interp& in, Args&) -> Value {
            need(in, in.caps.proc, "process listing");
            auto ps = os::list_processes(); auto l = make<List>();
            if (!ps) raise_os(in, ps.err, "OSError");
            for (auto& p : ps.value) { auto m = make<Map>(); m->set_str("pid", std::int64_t(p.pid)); m->set_str("ppid", std::int64_t(p.ppid)); m->set_str("name", strv(p.name)); m->set_str("rss", std::int64_t(p.rss_bytes)); l->push(obj(m)); }
            return obj(l);
        })
        .def("user", 0, 0, [](Interp&, Args&) -> Value {
            auto u = os::current_user(); auto m = make<Map>();
            m->set_str("name", strv(u.name)); m->set_str("home", strv(u.home)); m->set_str("shell", strv(u.shell)); m->set_str("uid", std::int64_t(u.uid)); m->set_str("gid", std::int64_t(u.gid));
            auto g = make<List>(); for (auto& x : u.groups) g->push(strv(x)); m->set_str("groups", obj(g)); m->set_str("elevated", os::is_elevated());
            return obj(m);
        })
        .def("dir", 1, 1, [](Interp& in, Args& a) -> Value {
            auto k = s_of(in, a[0], "os.dir"); using D = os::user_dir;
            static const std::pair<const char*, D> t[] = {{"config", D::config}, {"cache", D::cache}, {"data", D::data}, {"state", D::state}, {"runtime", D::runtime}, {"documents", D::documents}, {"downloads", D::downloads}, {"desktop", D::desktop}};
            for (auto& [n, d] : t) if (k == n) return strv(os::user_directory(d));
            in.raise("ValueError", "os.dir: unknown directory '" + k + "' (config, cache, data, state, runtime, documents, downloads, desktop)"); return Nil{};
        })
        .def("release", 0, 0, [](Interp&, Args&) -> Value {
            auto r = os::release_info(); auto m = make<Map>();
            m->set_str("name", strv(r.name)); m->set_str("version", strv(r.version)); m->set_str("id", strv(r.id)); m->set_str("kernel", strv(r.kernel)); m->set_str("kernel_version", strv(r.kernel_version)); m->set_str("arch", strv(r.arch)); m->set_str("pretty", strv(r.pretty));
            return obj(m);
        })
        .def("uptime", 0, 0, [](Interp& in, Args&) -> Value { return ok(in, os::uptime_seconds()); })
        .def("loadavg", 0, 0, [](Interp& in, Args&) -> Value { auto l = ok(in, os::load_average()); auto v = make<List>(); v->push(l.one); v->push(l.five); v->push(l.fifteen); return obj(v); })
        .def("memory", 0, 0, [](Interp&, Args&) -> Value {
            auto x = os::memory(); auto m = make<Map>();
            m->set_str("total", std::int64_t(x.total)); m->set_str("available", std::int64_t(x.available)); m->set_str("free", std::int64_t(x.free));
            m->set_str("swap_total", std::int64_t(x.swap_total)); m->set_str("swap_free", std::int64_t(x.swap_free)); m->set_str("cgroup_limit", std::int64_t(x.cgroup_limit));
            return obj(m);
        })
        .def("is_tty", 0, 1, [](Interp& in, Args& a) -> Value { int s = a.empty() ? 1 : int(i_of(in, a[0], "os.is_tty")); return os::console::is_tty(os::console::stream(s < 0 || s > 2 ? 1 : s)); })
        .def("term_size", 0, 0, [](Interp&, Args&) -> Value { auto s = os::console::size(); auto l = make<List>(); l->push(std::int64_t(s.value.cols)); l->push(std::int64_t(s.value.rows)); return obj(l); })
        .def("devices", 1, 1, [](Interp& in, Args& a) -> Value {
            auto k = s_of(in, a[0], "os.devices"); auto l = make<List>();
            if (k == "block") for (auto& d : ok(in, os::devices::block(), "OSError")) {
                auto m = make<Map>(); m->set_str("name", strv(d.name)); m->set_str("model", strv(d.model)); m->set_str("size", std::int64_t(d.size_bytes));
                m->set_str("removable", d.removable); m->set_str("rotational", d.rotational); auto ps = make<List>(); for (auto& p : d.partitions) ps->push(strv(p)); m->set_str("partitions", obj(ps)); l->push(obj(m));
            }
            else if (k == "pci") for (auto& d : ok(in, os::devices::pci(), "OSError")) {
                auto m = make<Map>(); m->set_str("address", strv(d.address)); m->set_str("vendor", strv(d.vendor_id)); m->set_str("device", strv(d.device_id)); m->set_str("class", strv(d.class_id)); m->set_str("driver", strv(d.driver)); l->push(obj(m));
            }
            else if (k == "usb") for (auto& d : ok(in, os::devices::usb(), "OSError")) {
                auto m = make<Map>(); m->set_str("vendor", strv(d.vendor_id)); m->set_str("product_id", strv(d.product_id)); m->set_str("manufacturer", strv(d.manufacturer)); m->set_str("product", strv(d.product)); m->set_str("bus", std::int64_t(d.bus)); l->push(obj(m));
            }
            else in.raise("ValueError", "os.devices: kind must be block, pci or usb");
            return obj(l);
        })
        .def("clipboard", 0, 1, [](Interp& in, Args& a) -> Value {
            if (a.empty()) return strv(ok(in, os::clipboard::get(), "OSError"));
            ok(in, os::clipboard::set(in.str(a[0])), "OSError"); return Nil{};
        })
        .def("signal", 2, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.proc, "signals");
            os::signal_id sg{}; if (!os::sig_from_name(s_of(in, a[1], "os.signal"), sg)) in.raise("ValueError", "unknown signal " + in.str(a[1]));
            ok(in, os::send_signal(std::uint64_t(i_of(in, a[0], "pid")), sg, false), "OSError"); return Nil{};
        }).done();
    B["time"] = Lib("time")
        .def("now", 0, 0, [](Interp&, Args&) -> Value { return double(os::wall_ns()) / 1e9; })
        .def("ms", 0, 0, [](Interp&, Args&) -> Value { return double(os::monotonic_ns()) / 1e6; })
        .def("iso", 0, 2, [](Interp& in, Args& a) -> Value {
            std::int64_t ns = a.empty() ? os::unix_time_ns() : std::int64_t(f_of(in, a[0], "time") * 1e9);
            bool local = a.size() > 1 && in.truthy(a[1]); return strv(local ? os::format_local_iso8601(ns) : os::format_iso8601(ns));
        })
        .def("parse", 1, 1, [](Interp& in, Args& a) -> Value { auto r = os::parse_iso8601(s_of(in, a[0], "time.parse")); if (!r) in.raise("ValueError", r.err.what); return double(r.value) / 1e9; })
        .def("utc_offset", 0, 0, [](Interp&, Args&) -> Value { return std::int64_t(os::utc_offset_seconds()); })
        .def("zone", 0, 0, [](Interp&, Args&) -> Value { return strv(os::timezone_name()); })
        .def("ns", 0, 0, [](Interp&, Args&) -> Value { return std::int64_t(os::monotonic_ns()); }).done();

    // ------------------------------------------------ net (forge::os::net) and service (forge::os::service)
    B["net"] = Lib("net")
        .def("resolve", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.net, "network access"); enforce_scope(in, SecScope::net_connect, s_of(in, a[0], "net.resolve")); auto r = ok(in, os::net::resolve(s_of(in, a[0], "net.resolve")), "NetError"); auto l = make<List>(); for (auto& x : r) l->push(strv(x.ip)); return obj(l); })
        .def("interfaces", 0, 0, [](Interp& in, Args&) -> Value {
            need(in, in.caps.net, "network access"); auto r = ok(in, os::net::interfaces(), "NetError"); auto l = make<List>();
            for (auto& i : r) { auto m = make<Map>(); m->set_str("name", strv(i.name)); m->set_str("up", i.up); m->set_str("loopback", i.loopback); auto ad = make<List>(); for (auto& x : i.addresses) ad->push(strv(x.ip)); m->set_str("addresses", obj(ad)); l->push(obj(m)); }
            return obj(l);
        })
        .def("port_open", 2, 3, [](Interp& in, Args& a) -> Value { need(in, in.caps.net, "network access");
            auto h = s_of(in, a[0], "net.port_open"); int port = int(i_of(in, a[1], "port"));
            enforce_scope(in, SecScope::net_connect, (h.find(':') != std::string::npos ? "[" + h + "]" : h) + ":" + std::to_string(port));
            return os::net::port_open(h, port, a.size() > 2 ? int(i_of(in, a[2], "timeout")) : 1000);
        })
        .def("http", 1, 2, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.net, "network access");
            os::net::http_request rq; rq.url = s_of(in, a[0], "net.http");
            if (auto u = os::net::parse_url(rq.url)) enforce_scope(in, SecScope::net_connect, (u->host.find(':') != std::string::npos ? "[" + u->host + "]" : u->host) + ":" + std::to_string(u->port ? u->port : u->scheme == "https" ? 443 : 80));
            if (auto* v = opt_key(a, 1, "method")) rq.method = in.str(*v);
            if (auto* v = opt_key(a, 1, "body")) rq.body = in.str(*v);
            if (auto* v = opt_key(a, 1, "timeout")) rq.timeout_ms = int(i_of(in, *v, "timeout"));
            if (auto* v = opt_key(a, 1, "headers")) if (auto* hm = as<Map>(*v)) for (auto& [k, x] : hm->entries) rq.headers[in.str(k)] = in.str(x);
            auto r = ok(in, os::net::http(rq), "NetError");
            auto m = make<Map>(); m->set_str("status", std::int64_t(r.status)); m->set_str("reason", strv(r.reason)); m->set_str("body", strv(r.body));
            auto h = make<Map>(); for (auto& [k, v] : r.headers) h->set_str(k, strv(v)); m->set_str("headers", obj(h));
            return obj(m);
        })
        .def("parse_url", 1, 1, [](Interp& in, Args& a) -> Value {
            auto u = ok(in, os::net::parse_url(s_of(in, a[0], "net.parse_url")), "ValueError"); auto m = make<Map>();
            m->set_str("scheme", strv(u.scheme)); m->set_str("host", strv(u.host)); m->set_str("port", std::int64_t(u.port)); m->set_str("path", strv(u.path)); m->set_str("query", strv(u.query));
            return obj(m);
        }).done();
    auto svc_spec = [](Interp& in, const Value& v) {
        os::service::service_spec s; if (!as<Map>(v) && !as<List>(v)) in.raise("TypeError", "service: expected a spec {name, argv, ...}");
        if (auto* x = kv_find(v, "name")) s.name = in.str(*x);
        if (auto* x = kv_find(v, "description")) s.description = in.str(*x);
        if (auto* x = kv_find(v, "argv")) if (auto* l = as<List>(*x)) for (auto& i : l->items) s.argv.push_back(in.str(i));
        if (auto* x = kv_find(v, "cwd")) s.working_dir = in.str(*x);
        if (auto* x = kv_find(v, "dir")) s.dir = in.str(*x);
        if (auto* x = kv_find(v, "restart")) s.restart_on_failure = Interp::truthy(*x);
        if (auto* x = kv_find(v, "at_login")) s.start_at_login = Interp::truthy(*x);
        if (auto* x = kv_find(v, "env")) if (auto* e = as<Map>(*x)) for (auto& [k, val] : e->entries) s.env[in.str(k)] = in.str(val);
        return s;
    };
    B["service"] = Lib("service")
        .def("unit", 1, 1, [svc_spec](Interp& in, Args& a) -> Value {
            auto s = svc_spec(in, a[0]);
#if defined(__APPLE__)
            return strv(os::service::render_launchd_plist(s));
#else
            return strv(os::service::render_systemd_unit(s));
#endif
        })
        .def("install", 1, 1, [svc_spec](Interp& in, Args& a) -> Value { need(in, in.caps.proc, "service control"); ok(in, os::service::install(svc_spec(in, a[0])), "OSError"); return Nil{}; })
        .def("uninstall", 1, 1, [svc_spec](Interp& in, Args& a) -> Value { need(in, in.caps.proc, "service control"); ok(in, os::service::uninstall(svc_spec(in, a[0])), "OSError"); return Nil{}; })
        .def("start", 1, 1, [svc_spec](Interp& in, Args& a) -> Value { need(in, in.caps.proc, "service control"); ok(in, os::service::start(svc_spec(in, a[0])), "OSError"); return Nil{}; })
        .def("stop", 1, 1, [svc_spec](Interp& in, Args& a) -> Value { need(in, in.caps.proc, "service control"); ok(in, os::service::stop(svc_spec(in, a[0])), "OSError"); return Nil{}; })
        .def("status", 1, 1, [svc_spec](Interp& in, Args& a) -> Value { return strv(os::service::state_name(os::service::query(svc_spec(in, a[0])))); }).done();

    // ------------------------------------------------ processes (forge::os::process)
    B["run"] = native("run", 1, 2, run_process);
    B["proc"] = Lib("proc")
        .def("run", 1, 2, run_process)
        .def("which", 1, 1, [](Interp& in, Args& a) -> Value { auto p = os::find_executable(s_of(in, a[0], "proc.which")); return p.empty() ? Value(Nil{}) : strv(p); })
        .def("start", 1, 1, [](Interp& in, Args& a) -> Value {
            need(in, in.caps.proc, "running processes");
            std::vector<std::string> argv; for (auto& x : l_of(in, a[0], "proc.start").items) argv.push_back(in.str(x));
            enforce_scope(in, SecScope::exec, argv.empty() ? std::string() : argv[0]);
            in.charge_process(argv.empty() ? std::string("proc.start") : argv[0]);
            auto c = std::make_shared<os::child>(ok(in, os::child::start(argv, true), "OSError"));
            Value h = make_handle(in, "process", c, [c]() { if (c->running()) { c->kill(); c->wait(1000); } });
            as<Handle>(h)->props["pid"] = std::int64_t(c->pid());
            return h;
        }).done();
    M["handle:process"]["read"] = native("process.read", 1, 2, [](Interp& in, Args& a) -> Value { auto c = h_of(in, a[0], "process").payload_as<os::child>(); return strv(c->read(65536, a.size() > 1 ? std::uint64_t(i_of(in, a[1], "timeout")) : 0)); });
    M["handle:process"]["wait"] = native("process.wait", 1, 2, [](Interp& in, Args& a) -> Value { auto c = h_of(in, a[0], "process").payload_as<os::child>(); return std::int64_t(c->wait(a.size() > 1 ? std::uint64_t(i_of(in, a[1], "timeout")) : 0)); });
    M["handle:process"]["kill"] = native("process.kill", 1, 1, [](Interp& in, Args& a) -> Value { need(in, in.caps.proc, "process control"); return h_of(in, a[0], "process").payload_as<os::child>()->kill(); });
    M["handle:process"]["get_running"] = native("process.running", 1, 1, [](Interp& in, Args& a) -> Value { return h_of(in, a[0], "process").payload_as<os::child>()->running(); });

    // ------------------------------------------------ ProcessPool
    //   auto pp = ProcessPool(4, {net: false, fs: false, timeout: 5000});   // workers default to no fs/net/proc/env
    //   auto f = pp.submit("auto task(auto x) { return x * x; }", 7);     f.get() == 49
    //   std::vector<int> xs = {1, 2, 3}; pp.map(src, xs); pp.size(); pp.shutdown();
    B["ProcessPool"] = native("ProcessPool", 0, 2, [](Interp& in, Args& a) -> Value {
        register_script_process_task();
        enforce_scope(in, SecScope::exec, "ProcessPool workers");
        auto box = std::make_shared<PoolBox>();
        // a worker never gets a capability its parent script lacks
        auto cap = [&](const char* k, bool parent) { auto* v = opt_key(a, 1, k); return parent && v && Interp::truthy(*v); };
        box->fs = cap("fs", in.caps.fs); box->net = cap("net", in.caps.net); box->proc = cap("proc", in.caps.proc); box->env = cap("env", in.caps.env);
        if (auto* v = opt_key(a, 1, "timeout")) box->timeout_ms = std::uint64_t(i_of(in, *v, "timeout"));
        os::process_pool::options po;
        po.workers = a.empty() || is_nil(a[0]) ? 0u : unsigned(std::max<std::int64_t>(1, i_of(in, a[0], "workers")));
        os::sandbox_policy sp; sp.deny_network = !box->net; sp.deny_exec = !box->proc; sp.deny_fs_write = !box->fs;
        po.sandbox = sp;
        try { box->pool = std::make_unique<os::process_pool>(std::move(po)); }
        catch (const os::os_exception& e) { in.raise(e.script_type(), e.what()); }
        if (in.proc_quota) {   // each worker is a child process (limit {procs})
            try { for (std::size_t i = 0; i < box->pool->size(); ++i) in.charge_process("ProcessPool worker"); }
            catch (...) { box->pool->shutdown(); throw; }
        }
        Value h = make_handle(in, "processpool", box, [box, dl = in.deadline_ns]() { close_pool(*box, dl); });
        as<Handle>(h)->props["workers"] = std::int64_t(box->pool->size());
        return h;
    });
    auto pp_submit = [](Interp& in, PoolBox& box, const Value& src, const Value& arg) -> Value {
        if (!box.pool) in.raise("ObjectDisposedError", "ProcessPool is shut down");
        json::object p;
        p.emplace_back("src", in.str(src)); p.emplace_back("arg", to_json(in, arg));
        p.emplace_back("fs", box.fs); p.emplace_back("net", box.net); p.emplace_back("proc", box.proc); p.emplace_back("env", box.env);
        if (box.timeout_ms) p.emplace_back("timeout_ms", double(box.timeout_ms));
        auto fb = std::make_shared<FutureBox>();
        fb->f = box.pool->submit("forge.script", json::dump(json::value(std::move(p))));
        return make_handle(in, "processfuture", fb, [] {});
    };
    auto pp_get = [](Interp& in, FutureBox& fb, std::uint64_t timeout_ms) -> Value {
        if (!fb.got && !fb.err) {
            auto end = timeout_ms ? os::monotonic_ns() + timeout_ms * 1000000 : 0;
            while (fb.f.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
                in.step();   // CancelledError / limits while waiting
                // step() reads the clock only every 256 calls, which is 5 s or more of these waits
                auto now = os::monotonic_ns();
                if (in.deadline_ns && now > in.deadline_ns) { in.deadline_ns = 0; in.raise("TimeoutError", "time limit reached"); }
                if (end && now > end) in.raise("TimeoutError", "ProcessPool future: timed out waiting for the worker");
            }
            try { fb.got = fb.f.get(); } catch (...) { fb.err = std::current_exception(); }
        }
        if (fb.err) {
            try { std::rethrow_exception(fb.err); }
            catch (const os::os_exception& e) { in.raise(e.script_type(), std::string("process worker: ") + e.what()); }
            catch (const std::exception& e) { in.raise("RuntimeError", std::string("process worker: ") + e.what()); }
        }
        json::value r;
        try { r = json::parse(*fb.got); } catch (const std::exception& e) { in.raise("ValueError", std::string("process worker: bad reply: ") + e.what()); }
        bool okv = false; const json::value* val = nullptr; std::string type = "RuntimeError", msg, out;
        for (auto& [k, v] : r.obj()) {
            if (k == "ok") okv = v.v.index() == 1 && v.b(); else if (k == "value") val = &v;
            else if (k == "type") type = v.str(); else if (k == "msg") msg = v.str(); else if (k == "out") out = v.str();
        }
        if (!out.empty() && in.sh->out) in.sh->out(out);
        if (!okv) in.raise(in.sh->errors.count(type) ? type : std::string("RuntimeError"), msg);
        return val ? from_json(*val) : Value(Nil{});
    };
    M["handle:processpool"]["submit"] = native("ProcessPool.submit", 2, 3, [pp_submit](Interp& in, Args& a) -> Value {
        return pp_submit(in, *h_of(in, a[0], "processpool").payload_as<PoolBox>(), a[1], a.size() > 2 ? a[2] : Value(Nil{}));
    });
    M["handle:processpool"]["map"] = native("ProcessPool.map", 3, 3, [pp_submit, pp_get](Interp& in, Args& a) -> Value {
        auto& box = *h_of(in, a[0], "processpool").payload_as<PoolBox>();
        std::vector<Value> fs; for (auto& x : l_of(in, a[2], "ProcessPool.map").items) fs.push_back(pp_submit(in, box, a[1], x));
        auto l = make<List>(); for (auto& f : fs) l->push(pp_get(in, *h_of(in, f, "processfuture").payload_as<FutureBox>(), 0)); return obj(l);
    });
    M["handle:processpool"]["size"] = native("ProcessPool.size", 1, 1, [](Interp& in, Args& a) -> Value {
        auto& b = *h_of(in, a[0], "processpool").payload_as<PoolBox>(); return std::int64_t(b.pool ? b.pool->size() : 0); });
    M["handle:processpool"]["resize"] = native("ProcessPool.resize", 2, 2, [](Interp& in, Args& a) -> Value {
        auto& b = *h_of(in, a[0], "processpool").payload_as<PoolBox>(); if (b.pool) b.pool->set_workers(unsigned(std::max<std::int64_t>(1, i_of(in, a[1], "workers")))); return Nil{}; });
    M["handle:processpool"]["shutdown"] = native("ProcessPool.shutdown", 1, 1, [](Interp& in, Args& a) -> Value {
        close_pool(*h_of(in, a[0], "processpool").payload_as<PoolBox>(), in.deadline_ns); return Nil{}; });
    M["handle:processfuture"]["get"] = native("ProcessFuture.get", 1, 2, [pp_get](Interp& in, Args& a) -> Value {
        return pp_get(in, *h_of(in, a[0], "processfuture").payload_as<FutureBox>(), a.size() > 1 ? std::uint64_t(i_of(in, a[1], "timeout")) : 0); });
    M["handle:processfuture"]["done"] = native("ProcessFuture.done", 1, 1, [](Interp& in, Args& a) -> Value {
        auto& fb = *h_of(in, a[0], "processfuture").payload_as<FutureBox>();
        return fb.got || fb.err || fb.f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });

    // ------------------------------------------------ json (forge::json) / hash (forge::hash::blake3) / re
    B["json"] = Lib("json")
        .def("parse", 1, 1, [](Interp& in, Args& a) -> Value { try { return from_json(json::parse(s_of(in, a[0], "json.parse"))); } catch (const forge_exception& e) { in.raise("ValueError", e.what()); } })
        .def("dump", 1, 2, [](Interp& in, Args& a) -> Value {
            auto j = to_json(in, a[0]);
            if (a.size() > 1 && Interp::truthy(a[1])) { std::string o; pretty(o, j, 0); return strv(o); }
            return strv(json::dump(j));
        }).done();
    B["hash"] = Lib("hash")
        .def("blake3", 1, 1, [](Interp& in, Args& a) -> Value {
            auto s = in.str(a[0]);
            return strv(hash::blake3::hex(hash::blake3::of(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()))));
        }).done();
    B["re"] = Lib("re")
        .def("match", 2, 2, [](Interp& in, Args& a) -> Value {
            try { std::regex r(s_of(in, a[0], "re.match")); std::smatch m; const std::string& s = s_of(in, a[1], "re.match");
                if (!std::regex_search(s, m, r)) return Nil{};
                auto l = make<List>(); for (auto& g : m) l->push(strv(g.str())); return obj(l);
            } catch (const std::regex_error& e) { in.raise("ValueError", std::string("bad regex: ") + e.what()); }
        })
        .def("find_all", 2, 2, [](Interp& in, Args& a) -> Value {
            try { std::regex r(s_of(in, a[0], "re.find_all")); const std::string& s = s_of(in, a[1], "re.find_all"); auto l = make<List>();
                for (auto it = std::sregex_iterator(s.begin(), s.end(), r); it != std::sregex_iterator(); ++it) l->push(strv(it->str())); return obj(l);
            } catch (const std::regex_error& e) { in.raise("ValueError", std::string("bad regex: ") + e.what()); }
        })
        .def("replace", 3, 3, [](Interp& in, Args& a) -> Value {
            try { return strv(std::regex_replace(s_of(in, a[1], "re.replace"), std::regex(s_of(in, a[0], "re.replace")), s_of(in, a[2], "re.replace"))); }
            catch (const std::regex_error& e) { in.raise("ValueError", std::string("bad regex: ") + e.what()); }
        }).done();
    B["math"] = Lib("math")
        .val("pi", 3.14159265358979323846).val("e", 2.71828182845904523536)
        .def("sqrt", 1, 1, [](Interp& in, Args& a) -> Value { return std::sqrt(f_of(in, a[0], "sqrt")); })
        .def("floor", 1, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(std::floor(f_of(in, a[0], "floor"))); })
        .def("ceil", 1, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(std::ceil(f_of(in, a[0], "ceil"))); })
        .def("round", 1, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(std::llround(f_of(in, a[0], "round"))); })
        .def("pow", 2, 2, [](Interp& in, Args& a) -> Value { return std::pow(f_of(in, a[0], "pow"), f_of(in, a[1], "pow")); })
        .def("log", 1, 1, [](Interp& in, Args& a) -> Value { return std::log(f_of(in, a[0], "log")); }).done();

    // ------------------------------------------------ string methods
    auto& S = M["string"];
    auto sm = [&](const std::string& n, int lo, int hi, Fn f) { S[n] = native("string." + n, lo, hi, std::move(f)); };
    sm("get_len", 1, 1, [](Interp&, Args& a) -> Value { return std::int64_t(as<String>(a[0])->s.size()); });
    sm("upper", 1, 1, [](Interp&, Args& a) -> Value { auto s = as<String>(a[0])->s; for (auto& c : s) c = char(std::toupper((unsigned char)c)); return strv(s); });
    sm("lower", 1, 1, [](Interp&, Args& a) -> Value { auto s = as<String>(a[0])->s; for (auto& c : s) c = char(std::tolower((unsigned char)c)); return strv(s); });
    sm("trim", 1, 1, [](Interp&, Args& a) -> Value { auto& s = as<String>(a[0])->s; auto b = s.find_first_not_of(" \t\r\n"); if (b == std::string::npos) return strv(""); auto e = s.find_last_not_of(" \t\r\n"); return strv(s.substr(b, e - b + 1)); });
    sm("split", 1, 2, [](Interp& in, Args& a) -> Value {
        auto& s = as<String>(a[0])->s; auto l = make<List>();
        if (a.size() < 2 || is_nil(a[1])) { std::size_t i = 0; while (i < s.size()) { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; std::size_t j = i; while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j; if (j > i) l->push(strv(s.substr(i, j - i))); i = j; } return obj(l); }
        auto& d = s_of(in, a[1], "split"); if (d.empty()) in.raise("ValueError", "empty separator");
        std::size_t i = 0; for (;;) { auto j = s.find(d, i); if (j == std::string::npos) { l->push(strv(s.substr(i))); break; } l->push(strv(s.substr(i, j - i))); i = j + d.size(); }
        return obj(l);
    });
    sm("lines", 1, 1, [](Interp&, Args& a) -> Value { auto& s = as<String>(a[0])->s; auto l = make<List>(); std::size_t i = 0; while (i < s.size()) { auto j = s.find('\n', i); if (j == std::string::npos) j = s.size(); auto ln = s.substr(i, j - i); if (!ln.empty() && ln.back() == '\r') ln.pop_back(); l->push(strv(ln)); i = j + 1; } return obj(l); });
    sm("starts_with", 2, 2, [](Interp& in, Args& a) -> Value { return as<String>(a[0])->s.starts_with(s_of(in, a[1], "starts_with")); });
    sm("ends_with", 2, 2, [](Interp& in, Args& a) -> Value { return as<String>(a[0])->s.ends_with(s_of(in, a[1], "ends_with")); });
    sm("contains", 2, 2, [](Interp& in, Args& a) -> Value { return as<String>(a[0])->s.find(s_of(in, a[1], "contains")) != std::string::npos; });
    sm("find", 2, 2, [](Interp& in, Args& a) -> Value { auto i = as<String>(a[0])->s.find(s_of(in, a[1], "find")); return i == std::string::npos ? std::int64_t(-1) : std::int64_t(i); });
    sm("replace", 3, 3, [](Interp& in, Args& a) -> Value {
        std::string s = as<String>(a[0])->s; auto& f = s_of(in, a[1], "replace"); auto& t = s_of(in, a[2], "replace"); if (f.empty()) return a[0];
        std::string o; std::size_t i = 0; for (;;) { auto j = s.find(f, i); if (j == std::string::npos) { o += s.substr(i); break; } o += s.substr(i, j - i); o += t; i = j + f.size(); }
        return strv(o);
    });
    sm("slice", 2, 3, [](Interp& in, Args& a) -> Value {
        auto& s = as<String>(a[0])->s; auto n = std::int64_t(s.size());
        auto b = i_of(in, a[1], "slice"), e = a.size() > 2 && !is_nil(a[2]) ? i_of(in, a[2], "slice") : n;
        if (b < 0) b += n; if (e < 0) e += n; b = std::clamp<std::int64_t>(b, 0, n); e = std::clamp<std::int64_t>(e, b, n);
        return strv(s.substr(std::size_t(b), std::size_t(e - b)));
    });
    sm("repeat", 2, 2, [](Interp& in, Args& a) -> Value { auto k = i_of(in, a[1], "repeat"); auto& s = as<String>(a[0])->s; if (k < 0 || std::uint64_t(k) * s.size() > (256u << 20)) in.raise("ValueError", "bad repeat count"); std::string o; for (std::int64_t i = 0; i < k; ++i) o += s; return strv(o); });
    sm("to_int", 1, 1, [](Interp& in, Args& a) -> Value { Args x{a[0]}; return in.call(in.sh->builtins.at("int"), x); });
    sm("to_float", 1, 1, [](Interp& in, Args& a) -> Value { Args x{a[0]}; return in.call(in.sh->builtins.at("float"), x); });
    sm("chars", 1, 1, [](Interp& in, Args& a) -> Value { auto l = make<List>(); auto it = in.iterate(a[0]); Value v; while (in.iter_next(*it, v, nullptr)) l->push(v); return obj(l); });
    sm("pad_left", 2, 3, [](Interp& in, Args& a) -> Value { auto s = as<String>(a[0])->s; auto w = std::size_t(std::max<std::int64_t>(0, i_of(in, a[1], "pad"))); char c = a.size() > 2 ? s_of(in, a[2], "pad").at(0) : ' '; if (s.size() < w) s.insert(0, w - s.size(), c); return strv(s); });
    sm("pad_right", 2, 3, [](Interp& in, Args& a) -> Value { auto s = as<String>(a[0])->s; auto w = std::size_t(std::max<std::int64_t>(0, i_of(in, a[1], "pad"))); char c = a.size() > 2 ? s_of(in, a[2], "pad").at(0) : ' '; if (s.size() < w) s.append(w - s.size(), c); return strv(s); });

    // ------------------------------------------------ list methods
    auto& L = M["list"];
    auto lm = [&](const std::string& n, int lo, int hi, Fn f) { L[n] = native("list." + n, lo, hi, std::move(f)); };
    lm("get_len", 1, 1, [](Interp&, Args& a) -> Value { return std::int64_t(as<List>(a[0])->items.size()); });
    lm("push", 2, -1, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); for (std::size_t i = 1; i < a.size(); ++i) l->push(in.share(a[i])); return a[0]; });
    lm("pop", 1, 1, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); if (l->items.empty()) in.raise("IndexError", "pop from empty list"); Value v = l->items.back(); l->items.pop_back(); return v; });
    lm("insert", 3, 3, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); auto i = i_of(in, a[1], "insert"); auto n = std::int64_t(l->items.size()); if (i < 0) i += n; if (i < 0 || i > n) in.raise("IndexError", "insert index out of range"); l->items.insert(l->items.begin() + i, in.share(a[2])); return a[0]; });
    lm("remove_at", 2, 2, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); auto i = i_of(in, a[1], "remove_at"); auto n = std::int64_t(l->items.size()); if (i < 0) i += n; if (i < 0 || i >= n) in.raise("IndexError", "remove_at index out of range"); Value v = l->items[std::size_t(i)]; l->items.erase(l->items.begin() + i); return v; });
    lm("clear", 1, 1, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); l->items.clear(); return a[0]; });
    lm("extend", 2, 2, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); auto src = l_of(in, a[1], "extend").items; for (auto& x : src) l->push(x); return a[0]; });
    lm("contains", 2, 2, [](Interp& in, Args& a) -> Value { for (auto& x : as<List>(a[0])->items) if (in.equal(x, a[1])) return true; return false; });
    lm("index_of", 2, 2, [](Interp& in, Args& a) -> Value { auto& v = as<List>(a[0])->items; for (std::size_t i = 0; i < v.size(); ++i) if (in.equal(v[i], a[1])) return std::int64_t(i); return std::int64_t(-1); });
    lm("map", 2, 2, [](Interp& in, Args& a) -> Value { auto src = as<List>(a[0])->items; auto l = make<List>(); for (auto& x : src) { Args y{x}; l->push(in.call(a[1], y)); } return obj(l); });
    lm("filter", 2, 2, [](Interp& in, Args& a) -> Value { auto src = as<List>(a[0])->items; auto l = make<List>(); for (auto& x : src) { Args y{x}; if (Interp::truthy(in.call(a[1], y))) l->push(x); } return obj(l); });
    lm("each", 2, 2, [](Interp& in, Args& a) -> Value { auto src = as<List>(a[0])->items; for (auto& x : src) { Args y{x}; in.call(a[1], y); } return Nil{}; });
    lm("reduce", 3, 3, [](Interp& in, Args& a) -> Value { auto src = as<List>(a[0])->items; Value acc = a[2]; for (auto& x : src) { Args y{acc, x}; acc = in.call(a[1], y); } return acc; });
    lm("any", 2, 2, [](Interp& in, Args& a) -> Value { auto src = as<List>(a[0])->items; for (auto& x : src) { Args y{x}; if (Interp::truthy(in.call(a[1], y))) return true; } return false; });
    lm("all", 2, 2, [](Interp& in, Args& a) -> Value { auto src = as<List>(a[0])->items; for (auto& x : src) { Args y{x}; if (!Interp::truthy(in.call(a[1], y))) return false; } return true; });
    lm("sort", 1, 2, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); Args x{a[0]}; if (a.size() > 1) x.push_back(a[1]); auto r = in.call(in.sh->builtins.at("sorted"), x); l->items = as<List>(r)->items; return a[0]; });
    lm("reverse", 1, 1, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mut(in, *l); std::reverse(l->items.begin(), l->items.end()); return a[0]; });
    lm("slice", 2, 3, [](Interp& in, Args& a) -> Value {
        auto& v = as<List>(a[0])->items; auto n = std::int64_t(v.size());
        auto b = i_of(in, a[1], "slice"), e = a.size() > 2 && !is_nil(a[2]) ? i_of(in, a[2], "slice") : n;
        if (b < 0) b += n; if (e < 0) e += n; b = std::clamp<std::int64_t>(b, 0, n); e = std::clamp<std::int64_t>(e, b, n);
        return list_of(std::vector<Value>(v.begin() + b, v.begin() + e));
    });
    lm("join", 1, 2, [](Interp& in, Args& a) -> Value { std::string sep = a.size() > 1 ? s_of(in, a[1], "join") : ""; std::string o; bool f = true; for (auto& x : as<List>(a[0])->items) { if (!f) o += sep; f = false; o += in.str(x); } return strv(o); });
    lm("sum", 1, 1, [](Interp& in, Args& a) -> Value { Value acc = std::int64_t(0); for (auto& x : as<List>(a[0])->items) acc = in.binary(Op::Add, acc, x); return acc; });
    lm("copy", 1, 1, [](Interp&, Args& a) -> Value { return list_of(as<List>(a[0])->items); });
    lm("first", 1, 1, [](Interp&, Args& a) -> Value { auto& v = as<List>(a[0])->items; return v.empty() ? Value(Nil{}) : v.front(); });
    lm("last", 1, 1, [](Interp&, Args& a) -> Value { auto& v = as<List>(a[0])->items; return v.empty() ? Value(Nil{}) : v.back(); });

    // ------------------------------------------------ map methods
    auto& MM = M["map"];
    auto mm = [&](const std::string& n, int lo, int hi, Fn f) { MM[n] = native("map." + n, lo, hi, std::move(f)); };
    mm("len", 1, 1, [](Interp&, Args& a) -> Value { return std::int64_t(as<Map>(a[0])->entries.size()); });
    mm("keys", 1, 1, [](Interp&, Args& a) -> Value { auto l = make<List>(); for (auto& [k, v] : as<Map>(a[0])->entries) l->push(k); return obj(l); });
    mm("values", 1, 1, [](Interp&, Args& a) -> Value { auto l = make<List>(); for (auto& [k, v] : as<Map>(a[0])->entries) l->push(v); return obj(l); });
    mm("items", 1, 1, [](Interp&, Args& a) -> Value { auto l = make<List>(); for (auto& [k, v] : as<Map>(a[0])->entries) l->push(list_of({k, v})); return obj(l); });
    mm("has", 2, 2, [](Interp&, Args& a) -> Value { return as<Map>(a[0])->find(a[1]) != nullptr; });
    mm("get", 2, 3, [](Interp&, Args& a) -> Value { if (auto* v = as<Map>(a[0])->find(a[1])) return *v; return a.size() > 2 ? a[2] : Value(Nil{}); });
    mm("set", 3, 3, [](Interp& in, Args& a) -> Value { in.set_index(a[0], a[1], a[2]); return a[0]; });
    mm("remove", 2, 2, [](Interp& in, Args& a) -> Value { auto* m = as<Map>(a[0]); mut(in, *m); return m->erase(a[1]); });
    mm("copy", 1, 1, [](Interp&, Args& a) -> Value { auto n = make<Map>(); for (auto& [k, v] : as<Map>(a[0])->entries) n->set(k, v); return obj(n); });

    // ------------------------------------------------ tasks, channels, weak refs
    M["task"]["cancel"] = native("task.cancel", 1, 1, [](Interp&, Args& a) -> Value { as<Task>(a[0])->st->cancel = true; return Nil{}; });
    M["task"]["get_done"] = native("task.done", 1, 1, [](Interp&, Args& a) -> Value { auto* t = as<Task>(a[0]); std::lock_guard g(t->st->m); return t->st->done; });
    M["task"]["wait"] = native("task.wait", 1, 1, [](Interp& in, Args& a) -> Value { return in.await(a[0]); });
    auto& C = M["channel"];
    C["send"] = native("channel.send", 2, 2, [](Interp& in, Args& a) -> Value {
        auto st = as<Channel>(a[0])->st;
        std::unordered_map<const Obj*, Value> memo; Value v = deep_copy(in, a[1], memo);
        std::unique_lock g(st->m);
        { debug::Blocked w(in, "channel send");
          while (!st->closed && st->cap && st->q.size() >= st->cap) { st->cv.wait_for(g, std::chrono::milliseconds(20)); g.unlock(); in.step(); g.lock(); if (st->q.size() >= st->cap) w.poll(); } }
        if (st->closed) in.raise("ValueError", "send on a closed channel");
        st->q.push_back(std::move(v)); st->cv.notify_all(); return Nil{};
    });
    C["recv"] = native("channel.recv", 1, 2, [](Interp& in, Args& a) -> Value {
        auto st = as<Channel>(a[0])->st;
        std::uint64_t end = a.size() > 1 && !is_nil(a[1]) ? os::monotonic_ns() + std::uint64_t(f_of(in, a[1], "timeout") * 1e6) : 0;
        std::unique_lock g(st->m);
        std::optional<debug::Blocked> w; if (!end) w.emplace(in, "channel receive");
        while (st->q.empty() && !st->closed) {
            if (end && os::monotonic_ns() >= end) return Nil{};
            st->cv.wait_for(g, std::chrono::milliseconds(20)); g.unlock(); in.step(); g.lock();
            if (w && st->q.empty() && !st->closed) w->poll();
        }
        if (st->q.empty()) return Nil{};
        Value v = std::move(st->q.front()); st->q.pop_front(); st->cv.notify_all(); debug::adopt(v); return v;
    });
    C["close"] = native("channel.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
    C["get_len"] = native("channel.len", 1, 1, [](Interp&, Args& a) -> Value { auto st = as<Channel>(a[0])->st; std::lock_guard g(st->m); return std::int64_t(st->q.size()); });
    M["weak"]["get"] = native("weak.get", 1, 1, [](Interp&, Args& a) -> Value { return as<WeakRef>(a[0])->get(); });
    M["weak"]["get_alive"] = native("weak.alive", 1, 1, [](Interp&, Args& a) -> Value { return !is_nil(as<WeakRef>(a[0])->get()); });
    M["fn"]["get_name"] = native("fn.name", 1, 1, [](Interp&, Args& a) -> Value {
        if (auto* f = as<Function>(a[0])) return strv(f->decl->name); if (auto* n = as<Native>(a[0])) return strv(n->name); return strv("<bound>");
    });


    // ------------------------------------------------ native plugins (script/include/forge/script/plugin.h) and modules
    sh.load_plugin = [](Interp& in, const std::string& path) -> Value {
        if (!(in.caps.fs && in.caps.net && in.caps.proc && in.caps.env)) in.raise("PermissionError", "native plugins are refused in sandboxed runs");
        // FSS CAP-1 / INT-1..3: fs.read here; security::open_plugin checks plugin_load, then integrity.plugins (the
        // detached <path>.sig) and os_code_signing on the bytes it loads (security_error -> PermissionError / IntegrityError)
        enforce_scope(in, SecScope::fs_read, path);
        {
            std::lock_guard g(in.sh->mod_m);
            if (auto it = in.sh->plugins.find(path); it != in.sh->plugins.end()) return it->second;
        }
        auto lib = security::open_plugin(path);
        if (!lib) in.raise("PluginError", "cannot load '" + path + "': " + lib.err.what);
        auto L = std::make_shared<os::library>(std::move(lib.value));
        using init_t = int (*)(const forge_plugin_api*);
        auto init = reinterpret_cast<init_t>(L->symbol("forge_script_plugin_init"));
        if (!init) in.raise("PluginError", "'" + path + "' does not export forge_script_plugin_init");
        struct Reg { std::shared_ptr<os::library> lib; Ref<Map> m; std::string base, err; std::shared_ptr<forge_plugin_api> api; };
        std::string base = std::filesystem::path(path).stem().string(); if (base.rfind("lib", 0) == 0 && base.size() > 3) base = base.substr(3);
        // The api table outlives init (plugins may keep the pointer for alloc); natives keep it alive.
        auto apip = std::make_shared<forge_plugin_api>(); auto& api = *apip;
        Reg reg{L, make<Map>(), base, {}, apip};
        api.abi = FORGE_SCRIPT_PLUGIN_ABI; api.reg = &reg;
        api.alloc = [](std::size_t n) -> char* { return static_cast<char*>(std::malloc(n ? n : 1)); };
        api.def = [](void* r, const char* name, int lo, int hi, forge_plugin_fn fn, void* user) {
            if (!r) return;
            auto& R = *static_cast<Reg*>(r);
            if (!name || !fn) { R.err = "def() with a null name or function"; return; }
            std::string full = R.base + "." + name; auto keep = std::make_pair(R.lib, R.api);
            R.m->set_str(name, native(full, std::max(0, lo), hi < 0 ? -1 : hi, [fn, user, full, keep](Interp& in, Args& a) -> Value {
                json::array arr; for (auto& x : a) arr.push_back(to_json(in, x));
                auto args = json::dump(json::value(std::move(arr)));
                char* out = nullptr; int rc = fn(args.c_str(), &out, user);
                std::string res = out ? std::string(out) : std::string(); std::free(out);
                if (rc != 0) in.raise("PluginError", full + ": " + (res.empty() ? "failed (" + std::to_string(rc) + ")" : res));
                if (res.empty()) return Nil{};
                try { return from_json(json::parse(res)); }
                catch (const forge_exception& e) { in.raise("PluginError", full + ": bad JSON result: " + e.what()); }
            }));
        };
        api.constant = [](void* r, const char* name, const char* js) {
            if (!r || !name) return;
            auto& R = *static_cast<Reg*>(r);
            try { R.m->set_str(name, from_json(json::parse(js ? js : "null"))); } catch (const forge_exception& e) { R.err = std::string("constant ") + name + ": " + e.what(); }
        };
        int rc = init(&api);
        api.reg = nullptr;   // def()/constant() are only valid during init
        if (rc != 0) in.raise("PluginError", "'" + path + "' refused to load (init returned " + std::to_string(rc) + ")");
        if (!reg.err.empty()) in.raise("PluginError", path + ": " + reg.err);
        reg.m->frozen = true;
        Value v = obj(reg.m);
        std::lock_guard g(in.sh->mod_m); in.sh->plugins[path] = v; return v;
    };
    B["forge::plugin"] = native("forge::plugin", 1, 1, [](Interp& in, Args& a) -> Value {
        auto p = s_of(in, a[0], "forge::plugin");
        if (!is_native_module(p)) in.raise("ValueError", "forge::plugin: '" + p + "' is not a .so/.dylib/.dll");
        // Refuse before resolving the path, so a sandboxed run gets PermissionError whether or not the file exists.
        if (!(in.caps.fs && in.caps.net && in.caps.proc && in.caps.env)) in.raise("PermissionError", "native plugins are refused in sandboxed runs");
        return in.import_module(p);
    });
    B["forge::import"] = native("forge::import", 1, 1, [](Interp& in, Args& a) -> Value { return in.import_module(s_of(in, a[0], "forge::import")); });
    B["forge::module_path"] = native("forge::module_path", 0, 0, [](Interp& in, Args&) -> Value { auto l = make<List>(); for (auto& d : module_search_path(in)) l->push(strv(d)); return obj(l); });
    B["forge::modules"] = native("forge::modules", 0, 0, [](Interp& in, Args&) -> Value {
        std::vector<std::string> v; { std::lock_guard g(in.sh->mod_m); for (auto& [k, m] : in.sh->modules) if (m) v.push_back(k); for (auto& [k, m] : in.sh->plugins) v.push_back(k); }
        std::sort(v.begin(), v.end()); auto l = make<List>(); for (auto& x : v) l->push(strv(x)); return obj(l);
    });
    install_cpp_std(sh); install_automation(sh); install_ai(sh); install_remote(sh); install_mobile(sh); install_text(sh); install_reflect(sh);
    for (auto& [k, v] : B) if (auto* r = std::get_if<Ref<Obj>>(&v)) if (*r) (*r)->frozen = true;
}

Value host_native(Shared& sh, const std::string& name, int lo, int hi, std::function<std::string(const std::string&)> fn) {
    Shared* shp = &sh;
    return native(name, std::max(0, lo), hi < 0 ? -1 : hi, [shp, name, fn = std::move(fn)](Interp& in, Args& a) -> Value {
        // Script callables anywhere in the arguments (also inside lists / maps) become {"$callback": id}.
        std::function<json::value(const Value&, int)> conv = [&](const Value& x, int depth) -> json::value {
            if (depth > 256) in.raise("ValueError", name + ": arguments nested too deep (cycle?)");
            if (as<Function>(x) || as<Native>(x) || as<Bound>(x)) {
                std::uint64_t id;
                { std::lock_guard g(shp->cb_m); id = shp->cb_next++; shp->callbacks[id] = x; }
                json::object o; o.emplace_back("$callback", double(id)); return json::value(std::move(o));
            }
            if (auto* l = as<List>(x)) { json::array r; for (auto& e : l->items) r.push_back(conv(e, depth + 1)); return json::value(std::move(r)); }
            if (auto* m = as<Map>(x)) { json::object o; for (auto& [k, e] : m->entries) o.emplace_back(as<String>(k) ? as<String>(k)->s : in.str(k), conv(e, depth + 1)); return json::value(std::move(o)); }
            return to_json(in, x);
        };
        json::array arr;
        for (auto& x : a) arr.push_back(conv(x, 0));
        std::string res;
        try { res = fn(json::dump(json::value(std::move(arr)))); }
        catch (const ScriptError&) { throw; }
        catch (const std::exception& e) { in.raise("HostError", name + ": " + e.what()); }
        if (res.empty()) return Nil{};
        try { return from_json(json::parse(res)); }
        catch (const forge_exception& e) { in.raise("HostError", name + ": bad JSON result: " + e.what()); }
    });
}
Value host_args(const std::string& js) {
    Value v = from_json(json::parse(js.empty() ? "[]" : js));
    if (!as<List>(v)) { auto l = make<List>(); l->push(v); return obj(l); }
    return v;
}

bool run_process_worker_if_requested(int argc, char** argv) {
    register_script_process_task();
    return os::process_pool::run_worker_if_requested(argc, argv);
}
} // namespace forge::script
