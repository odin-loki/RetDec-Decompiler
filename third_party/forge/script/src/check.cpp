// forge::script static checker. See check.hpp and docs/scripting.md "Static checks".
#include <forge/script/check.hpp>
#include "ast.hpp"
#include "parser.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace forge::script {
namespace {

using L = diagnostic::level;

// Built-in exception hierarchy (builtins.cpp): child -> parent. Builtin classes are frozen globals.
const std::map<std::string, std::string>& err_parent() {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> p;
        for (auto n : {"IOError", "OSError", "TypeError", "IndexError", "KeyError", "OverflowError", "ObjectDisposedError", "ShellError",
                       "LimitExceededError", "NameError", "AttributeError", "ValueError", "ArithmeticError", "SyntaxError", "ImportError",
                       "RuntimeError", "LogicError", "BorrowError", "NullPointerError", "FrozenError", "RecursionError", "SystemExit",
                       "RaceError", "PipelineError", "PluginError", "AIError", "AccessError"}) p[n] = "Error";
        for (auto n : {"PermissionError", "TimeoutError", "OutOfMemoryError", "CancelledError", "DeadlockError", "FileNotFoundError",
                       "FileExistsError", "NotSupportedError", "InterruptedError", "ConnectionError", "SignalError", "BlockingIOError",
                       "NotADirectoryError", "IsADirectoryError", "ProcessLookupError", "NoSpaceError", "TooManyFilesError", "BusyError",
                       "ReadOnlyFileSystemError"}) p[n] = "OSError";
        for (auto n : {"ConnectionRefusedError", "ConnectionResetError", "BrokenPipeError"}) p[n] = "ConnectionError";
        p["SandboxViolationError"] = "PermissionError";
        p["SecurityError"] = "OSError";
        p["IntegrityError"] = "SecurityError"; p["PolicyViolation"] = "SecurityError";
        p["ZeroDivisionError"] = "ArithmeticError";
        return p;
    }();
    return m;
}
bool err_is_a(std::string c, const std::string& anc) {
    if (anc.empty() || anc == "..." || anc == "Error" ) return err_parent().count(c) || c == "Error";
    for (int g = 0; g < 8 && !c.empty(); ++g) { if (c == anc) return true; auto it = err_parent().find(c); if (it == err_parent().end()) return false; c = it->second; }
    return false;
}

// Capability of a builtin library call (builtins.cpp `need(...)`).
const char* cap_of(const std::string& lib, const std::string& fn) {
    if (lib == "fs") return "fs";
    if (lib == "net") return "net";
    if (lib == "proc" || lib == "service") return "proc";
    if (lib == "os") {
        if (fn == "env" || fn == "setenv" || fn == "unsetenv" || fn == "environ") return "env";
        if (fn == "chdir") return "fs";
        if (fn == "processes" || fn == "kill" || fn == "signal") return "proc";
    }
    return nullptr;
}

struct Caps { bool fs = true, net = true, proc = true, env = true;
    bool get(const std::string& c) const { return c == "fs" ? fs : c == "net" ? net : c == "proc" ? proc : env; } };

bool is_builtin_var(const Node& n, const char* name) { return n.k == NK::Var && n.var.global() && n.s == name; }
const Node* callee(const Node& n) { return n.k == NK::Call && !n.kids.empty() ? n.kids[0].get() : nullptr; }

enum : int { NONE = 0, MAYBE = 1, DEF = 2 };
using State = std::map<int, int>;   // slot -> moved state, for the current function's own locals
State merge(const State& a, const State& b) {
    State r;
    auto get = [](const State& m, int k) { auto it = m.find(k); return it == m.end() ? int(NONE) : it->second; };
    for (auto* m : {&a, &b}) for (auto& [k, v] : *m) { (void)v; int x = get(a, k), y = get(b, k); r[k] = (x == DEF && y == DEF) ? DEF : (x || y) ? MAYBE : NONE; }
    return r;
}

struct Binding { const FnDecl* lam = nullptr; bool param = false; bool container_cap = false; std::string alias; const char* cap = nullptr; bool chan = false; char lit = 0; std::string name; int line = 0; bool used = false; bool report = false; bool unique = false; bool list = false; bool cnst = false; };

struct Fn {
    const FnDecl* decl = nullptr;
    std::map<int, Binding*> slot;                      // current binding per slot
    std::vector<std::unique_ptr<Binding>> all;
    std::vector<Binding*> order;                       // every binding declared here, locals and globals, in order
    std::vector<std::set<std::string>> scopes;         // names per block, for shadowing
    // Move-only: MSVC's vector<Fn> reallocation otherwise picks the (ill-formed) copy,
    // because std::map's move constructor is not noexcept there.
    Fn() = default;
    Fn(const Fn&) = delete;
    Fn& operator=(const Fn&) = delete;
    Fn(Fn&&) = default;
    Fn& operator=(Fn&&) = default;
};

struct Checker {
    std::string_view src; std::string file; check_options opt;
    std::vector<diagnostic> out; std::set<std::tuple<int, std::string>> seen;
    std::vector<Fn> fns;
    Caps caps;
    std::vector<std::string> iter_stack;               // containers borrowed by enclosing range-for (by name+slot key)
    std::vector<std::pair<std::string, bool>> iter_list; // parallel: known list/map?
    std::vector<std::string> held;                     // forge::shared values whose with() is active
    std::map<std::string, std::set<std::string>> lock_edges; std::map<std::pair<std::string, std::string>, int> edge_line;
    std::map<std::string, const FnDecl*> globals_fn; std::map<std::string, int> global_assigns;
    bool quiet = false;                                // second loop pass: dedupe only
    std::vector<int> loop_depth_breaks;

    int col_of(int line, const std::string& needle) {
        if (line <= 0) return 0;
        std::size_t p = 0; for (int l = 1; l < line && p != std::string_view::npos; ++l) { p = src.find('\n', p); if (p != std::string_view::npos) ++p; }
        if (p == std::string_view::npos) return 0;
        auto e = src.find('\n', p); auto ln = src.substr(p, e == std::string_view::npos ? src.size() - p : e - p);
        if (!needle.empty()) { auto q = ln.find(needle); if (q != std::string_view::npos) return int(q) + 1; }
        auto q = ln.find_first_not_of(" \t"); return q == std::string_view::npos ? 1 : int(q) + 1;
    }
    void diag(L sev, const char* id, int line, std::string msg, const std::string& needle = {}) {
        if (sev == L::warning && !opt.warnings) return;
        if (!seen.emplace(line, std::string(id) + msg).second) return;
        out.push_back({sev, id, std::move(msg), file, line, col_of(line, needle)});
    }

    void D(L sev, const char* id, const Node& n, std::string msg, const std::string& needle = {}) {
        const Node* at = &n; if (n.k == NK::Call && !n.kids.empty() && n.kids[0]->col) at = n.kids[0].get();
        if (sev == L::warning && !opt.warnings) return;
        if (!seen.emplace(n.line, std::string(id) + msg).second) return;
        int sc = start_col(*at, n.line);
        out.push_back({sev, id, std::move(msg), file, n.line, sc ? sc : col_of(n.line, needle)});
    }
    // start column: leftmost recorded column among the node and its sub-expressions on the same line
    static int start_col(const Node& n, int line) {
        int c = n.line == line ? n.col : 0;
        if (n.k == NK::Block || n.k == NK::Lambda) return c;
        for (auto& k : n.kids) if (k) { int x = start_col(*k, line); if (x && (!c || x < c)) c = x; }
        return c;
    }
    Fn& cur() { return fns.back(); }
    std::map<std::string, Binding*> gbind;             // globals declared by this script's top level
    std::vector<std::unique_ptr<Binding>> gall;         // owns global bindings: a global can be declared inside a nested function
    std::set<std::string> gread, fn_assigned;          // globals read anywhere / assigned inside some function
    std::map<std::string, int> ids; std::map<std::string, std::vector<int>> field_ids;
    int gid(const std::string& k) { auto it = ids.find(k); if (it != ids.end()) return it->second; int id = -int(ids.size()) - 1; ids[k] = id; auto dot = k.find('.'); if (dot != std::string::npos) field_ids[k.substr(0, dot)].push_back(id); return id; }
    bool at_top() const { return !fns.empty() && fns.back().decl && fns.back().decl->is_top; }
    // graph key of an lvalue: own local "l<slot>", script global "g<name>" (top level only), plus ".field" chains
    std::string lkey(const Node& n) {
        if (n.k == NK::Var) {
            if (!n.var.global()) return n.var.depth == 0 ? "l" + std::to_string(n.var.slot) : std::string();
            return at_top() && gbind.count(n.s) ? "g" + n.s : std::string();
        }
        if (n.k == NK::Field && !n.kids.empty()) { auto b = lkey(*n.kids[0]); return b.empty() ? b : b + "." + n.s; }
        return {};
    }
    int skey(const std::string& k) { return k.size() > 1 && k[0] == 'l' && k.find('.') == std::string::npos ? std::stoi(k.substr(1)) : gid(k); }
    void kill(State& st, const std::string& k) { st.erase(skey(k)); for (auto id : field_ids[k]) st.erase(id); auto it = field_ids.find(k); (void)it; for (auto& [fk, v] : ids) if (fk.size() > k.size() && fk.compare(0, k.size(), k) == 0 && fk[k.size()] == '.') st.erase(v); }
    void after_call(State& st) {   // a script function may reassign a global it writes
        for (auto& g : fn_assigned) { auto it = ids.find("g" + g); if (it != ids.end()) if (auto s2 = st.find(it->second); s2 != st.end() && s2->second == DEF) s2->second = MAYBE; }
    }
    Binding* bind_of(const VarRef& v) {
        if (v.global()) { auto it = gbind.find(v.name); return it == gbind.end() ? nullptr : it->second; }
        if (v.depth >= int(fns.size())) return nullptr;
        auto& f = fns[fns.size() - 1 - v.depth]; auto it = f.slot.find(v.slot); return it == f.slot.end() ? nullptr : it->second;
    }
    static bool internal_name(const std::string& n) { return n.empty() || n[0] == '_' || n.find_first_of("$#.: <>") != std::string::npos || !(std::isalpha((unsigned char)n[0])); }
    void declare(const VarRef& v, const std::string& name, int line, bool report, bool cnst) {
        auto& f = cur();
        if (!internal_name(name) && !f.scopes.empty()) {
            for (std::size_t i = 0; i + 1 < f.scopes.size(); ++i) if (f.scopes[i].count(name)) { diag(L::warning, "shadow", line, "'" + name + "' shadows a variable declared in an enclosing block", name); break; }
            f.scopes.back().insert(name);
        }
        auto b = std::make_unique<Binding>(); b->name = name; b->line = line; b->report = report && !internal_name(name); b->cnst = cnst;
        f.order.push_back(b.get());
        if (v.global()) { if (gread.count(name)) b->report = false; gbind[name] = b.get(); gall.push_back(std::move(b)); return; }
        f.slot[v.slot] = b.get();
        f.all.push_back(std::move(b));
    }
    void end_fn() {
        for (auto* b : cur().order) if (b->report && !b->used) diag(L::warning, "unused-variable", b->line, "variable '" + b->name + "' is never read", b->name);
        fns.pop_back();
    }

    // ---------------------------------------------------------------- expressions
    void read_var(const Node& n, State& st) {
        auto* b = bind_of(n.var); if (b) b->used = true;
        check_key(n, lkey(n), st);
        if (b && !b->alias.empty()) if (auto it = st.find(skey(b->alias)); it != st.end() && it->second)
            D(L::warning, "use-after-move", n, "reference '" + n.s + "' refers to a value that may have been moved", n.s);
    }
    void check_key(const Node& n, const std::string& k, State& st) {
        if (k.empty()) return;
        auto it = st.find(skey(k)); if (it == st.end() || !it->second) return;
        if (it->second == DEF && !quiet) D(L::error, "use-after-move", n, "'" + n.s + "' is used after std::move (raises BorrowError)", n.s);
        else D(L::warning, "use-after-move", n, "'" + n.s + "' may be used after std::move on some path", n.s);
    }
    std::string key(const Node& n) { return n.k == NK::Var ? (n.var.global() ? "g:" + n.s : n.s + "@" + std::to_string(fns.size() - 1 - std::max(0, n.var.depth)) + ":" + std::to_string(n.var.slot)) : std::string(); }

    void lambda(const Node& n, bool thread_ctx) {
        if (!n.fn || !n.fn->body) return;
        if (thread_ctx) thread_captures(*n.fn->body, 1);
        auto saved_iter = iter_stack; auto saved_list = iter_list; iter_stack.clear(); iter_list.clear();
        fn(*n.fn);
        iter_stack = saved_iter; iter_list = saved_list;
    }
    // captured (depth >= 1) mutable locals read or written inside a thread lambda
    void thread_captures(const Node& n, int depth) {
        if (n.k == NK::Lambda) { if (n.fn && n.fn->body) thread_captures(*n.fn->body, depth + 1); return; }
        if (n.k == NK::Var && !n.var.global() && n.var.depth >= depth) {
            int up = n.var.depth - depth;   // hops above the thread lambda's definer
            if (up < int(fns.size())) { auto& f = fns[fns.size() - 1 - up]; auto it = f.slot.find(n.var.slot);
                if (it != f.slot.end() && !it->second->cnst) D(L::warning, "thread-capture", n, "thread body captures mutable local '" + n.s + "'; move, freeze or wrap it in forge::shared", n.s); }
        }
        for (auto& k : n.kids) if (k) thread_captures(*k, depth);
        if (n.extra) thread_captures(*n.extra, depth);
    }
    bool sendable_arg(const Node& a, State& st) {
        switch (a.k) {
        case NK::Nil: case NK::True: case NK::False: case NK::Int: case NK::Float: case NK::Str: case NK::Move: case NK::Lambda: return true;
        case NK::Call: { auto* c = callee(a); return c && (is_builtin_var(*c, "freeze") || is_builtin_var(*c, "forge::shared")); }
        case NK::Var: { auto* b = bind_of(a.var); (void)st; return b && b->cnst; }
        default: return false;
        }
    }
    static bool json_literal(const Node& a) {
        switch (a.k) {
        case NK::Nil: case NK::True: case NK::False: case NK::Int: case NK::Float: case NK::Str: return true;
        case NK::List: case NK::Map: for (auto& k : a.kids) if (!json_literal(*k)) return false; return true;
        default: return false;
        }
    }
    static bool non_json_literal(const Node& a) { return a.k == NK::Lambda || a.k == NK::Shell; }

    void call(const Node& n, State& st) {
        const Node* c = callee(n);
        std::size_t argc = n.kids.size() - 1;
        // ---- capability
        if (c && c->k == NK::Field && !c->kids.empty() && c->kids[0]->k == NK::Var && c->kids[0]->var.global()) {
            const std::string& lib = c->kids[0]->s;
            if (auto* cp = cap_of(lib, c->s); cp && !caps.get(cp))
                D(L::error, "capability", n, lib + "." + c->s + " needs the '" + cp + "' capability, which this run denies (raises PermissionError)", c->s);
        }
        if (c && (c->k == NK::Index || c->k == NK::Call || (c->k == NK::Field && !(c->kids.size() && c->kids[0]->k == NK::Var && c->kids[0]->var.global() && !gbind.count(c->kids[0]->s))))) if (auto t = taint(*c); t.first && !t.second && !caps.get(t.first))
            D(L::warning, "capability", n, std::string("this call may invoke a builtin that needs the '") + t.first + "' capability, which this run denies");
        // push_back & co. of a builtin taints the container
        if (c && c->k == NK::Field && !c->kids.empty() && c->kids[0]->k == NK::Var && (c->s == "push_back" || c->s == "emplace_back" || c->s == "push_front" || c->s == "insert" || c->s == "append" || c->s == "emplace"))
            if (auto* b = bind_of(c->kids[0]->var); b && !b->chan) for (std::size_t i = 1; i < n.kids.size(); ++i) if (auto t = taint(*n.kids[i]); t.first) { b->cap = t.first; b->container_cap = true; }
        if (c && c->k == NK::Var) if (auto* b = bind_of(c->var); b && b->cap && !b->container_cap && !caps.get(b->cap))
            D(b->cnst && !b->param ? L::error : L::warning, "capability", n, "'" + c->s + "' holds a builtin that needs the '" + b->cap + "' capability, which this run denies", c->s);
        if (c && (is_builtin_var(*c, "eval") || is_builtin_var(*c, "forge::eval")) && argc == 1 && n.kids[1]->k == NK::Str) sub_check(n, n.kids[1]->s, "eval");
        if (c) deprecated(n, *c);
        if (c && (is_builtin_var(*c, "forge::limit") || is_builtin_var(*c, "limit")) && argc == 2 && n.kids[1]->k == NK::Map) limit_options(*n.kids[1]);
        if (c && (is_builtin_var(*c, "forge::plugin") || is_builtin_var(*c, "forge::import")) && argc == 1 && n.kids[1]->k == NK::Str) {
            Node fake; fake.k = NK::Import; fake.s = n.kids[1]->s; fake.line = n.line; fake.col = start_col(*c, n.line); import_check(fake);
        }
        if (c && is_builtin_var(*c, "run") && !caps.proc) D(L::error, "capability", n, "run() needs the 'proc' capability, which this run denies (raises PermissionError)", "run");
        // ---- format strings
        if (c && c->k == NK::Var && c->var.global() && (c->s == "std::format" || c->s == "std::println" || c->s == "std::print") && argc >= 1 && n.kids[1]->k == NK::Str)
            format_check(n.kids[1]->s, argc - 1, n.line);
        // ---- known script functions
        if (c && c->k == NK::Var && c->var.global() && globals_fn.count(c->s) && !global_assigns.count(c->s)) {
            auto* f = globals_fn[c->s]; std::size_t req = 0; for (auto& p : f->params) if (!p.dflt) ++req;
            if (argc < req || argc > f->params.size())
                D(L::warning, "arg-count", n, c->s + "() takes " + (req == f->params.size() ? std::to_string(req) : std::to_string(req) + ".." + std::to_string(f->params.size())) + " argument(s), called with " + std::to_string(argc), c->s);
        }
        // ---- thread send
        bool thread = c && c->k == NK::Var && c->var.global() && (c->s == "forge::spawn" || c->s == "std::async" || c->s == "forge::async");
        if (thread) for (std::size_t i = 1; i < n.kids.size(); ++i) {
            auto& a = *n.kids[i];
            if (a.k == NK::Lambda) continue;
            if (!sendable_arg(a, st)) D(L::warning, "thread-send", a, "value passed to a thread is not moved, frozen, const or forge::shared; it is deep-copied or raises RaceError on shared access", a.k == NK::Var ? a.s : std::string());
        }
        bool chan_send = c && c->k == NK::Field && (c->s == "send" || c->s == "push") && !c->kids.empty() && c->kids[0]->k == NK::Var && argc == 1;
        if (chan_send) if (auto* b = bind_of(c->kids[0]->var); b && b->chan && n.kids[1]->k != NK::Lambda && !sendable_arg(*n.kids[1], st))
            D(L::warning, "channel-send", *n.kids[1], "value sent on a channel is not moved, frozen, const or forge::shared; the receiver gets a deep copy or RaceError on shared access", n.kids[1]->k == NK::Var ? n.kids[1]->s : std::string());
        // ---- range-for structural mutation
        if (c && c->k == NK::Field && !c->kids.empty() && c->kids[0]->k == NK::Var) {
            static const std::set<std::string> mut = {"push_back", "emplace_back", "pop_back", "push_front", "pop_front", "insert", "emplace", "erase", "clear", "resize", "append", "extend", "remove", "pop", "push", "assign", "swap"};
            if (mut.count(c->s)) { auto k = key(*c->kids[0]); for (std::size_t i = 0; i < iter_stack.size(); ++i) if (!k.empty() && iter_stack[i] == k)
                D(iter_list[i].second ? L::error : L::warning, "iter-mutation", n, "'" + c->kids[0]->s + "." + c->s + "' changes the size of the container a range-for is iterating" + (iter_list[i].second ? std::string(" (raises BorrowError)") : std::string()), c->s); }
        }
        // ---- lock order: x.with(fn)
        bool with = c && c->k == NK::Field && c->s == "with" && !c->kids.empty() && c->kids[0]->k == NK::Var && argc == 1;
        // ---- ProcessPool
        if (c && c->k == NK::Field && (c->s == "submit" || c->s == "map") && argc >= 1) pool_task(n);
        // ---- sandbox(opts, fn)
        bool sb = c && is_builtin_var(*c, "sandbox") && argc == 2 && n.kids[1]->k == NK::Map && n.kids[2]->k == NK::Lambda;
        // evaluate
        if (c) expr(*c, st);
        for (std::size_t i = 1; i < n.kids.size(); ++i) {
            auto& a = *n.kids[i];
            if (a.k == NK::Lambda) {
                if (with && i == 1) {
                    auto k = c->kids[0]->s;
                    for (auto& h : held) if (h != k) { lock_edges[h].insert(k); edge_line.emplace(std::pair{h, k}, n.line); }
                    held.push_back(k); lambda(a, false); held.pop_back(); continue;
                }
                if (sb && i == 2) {
                    Caps saved = caps; auto& m = *n.kids[1];
                    for (std::size_t j = 0; j + 1 < m.kids.size(); j += 2) if (m.kids[j]->k == NK::Str && m.kids[j + 1]->k == NK::False) {
                        auto& s = m.kids[j]->s; if (s == "fs") caps.fs = false; else if (s == "net") caps.net = false; else if (s == "proc") caps.proc = false; else if (s == "env") caps.env = false; }
                    lambda(a, false); caps = saved; continue;
                }
                lambda(a, thread); continue;
            }
            expr(a, st);
        }
        if (at_top() && !(c && c->k == NK::Field && c->kids.size() && c->kids[0]->k == NK::Var && c->kids[0]->var.global() && !gbind.count(c->kids[0]->s))) after_call(st);
    }
    std::map<std::string, const char*> ret_caps;   // script fn -> builtin capability its return value may carry
    std::map<const FnDecl*, const char*> lambda_ret; // any function body (lambdas included) -> returned capability
    std::map<std::string, const char*> field_caps; // object field name -> builtin stored there (any object; coarse)
    std::map<const FnDecl*, int> dummy_;   // script fn -> builtin capability its return value may carry
    static bool ctor_name(const std::string& n) {
        static const std::set<std::string> c = {"std::vector", "std::deque", "std::list", "std::forward_list", "std::map", "std::unordered_map", "std::multimap",
            "std::set", "std::unordered_set", "std::multiset", "std::array", "std::pair", "std::tuple", "std::make_pair", "std::make_tuple", "std::optional"};
        return c.count(n) > 0;
    }
    // Does this value carry a capability-needing builtin? {cap, held inside a container}
    std::pair<const char*, bool> taint(const Node& e, int d = 0) {
        if (d > 8) return {nullptr, false};
        switch (e.k) {
        case NK::Copy: case NK::Move: return e.kids.empty() ? std::pair<const char*, bool>{nullptr, false} : taint(*e.kids[0], d + 1);
        case NK::Field: {
            if (!e.kids.empty() && e.kids[0]->k == NK::Var && e.kids[0]->var.global() && !gbind.count(e.kids[0]->s)) { if (auto* c = cap_of(e.kids[0]->s, e.s)) return {c, false}; }
            if (auto it = field_caps.find(e.s); it != field_caps.end() && !(e.kids.size() && e.kids[0]->k == NK::Var && e.kids[0]->var.global() && !gbind.count(e.kids[0]->s))) return {it->second, false};
            return {nullptr, false};
        }
        case NK::Var: if (auto* b = bind_of(e.var); b && b->cap) return {b->cap, b->container_cap}; return {nullptr, false};
        case NK::Index: if (!e.kids.empty()) { auto t = taint(*e.kids[0], d + 1); if (t.first && t.second) return {t.first, false}; } return {nullptr, false};
        case NK::Ternary: for (std::size_t i = 1; i < e.kids.size(); ++i) { auto t = taint(*e.kids[i], d + 1); if (t.first) return t; } return {nullptr, false};
        case NK::List: case NK::Map: for (auto& k : e.kids) if (k) { auto t = taint(*k, d + 1); if (t.first) return {t.first, true}; } return {nullptr, false};
        case NK::Call: {
            auto* c = callee(e); if (!c) return {nullptr, false};
            if (c->k == NK::Lambda && c->fn) { if (auto it = lambda_ret.find(c->fn.get()); it != lambda_ret.end()) return {it->second, false}; return {nullptr, false}; }
            if (c->k == NK::Field) { if (auto it = ret_caps.find("." + c->s); it != ret_caps.end()) return {it->second, false}; return {nullptr, false}; }
            if (c->k == NK::Var && !c->var.global()) { if (auto* b = bind_of(c->var); b && b->lam) if (auto it = lambda_ret.find(b->lam); it != lambda_ret.end()) return {it->second, false}; return {nullptr, false}; }
            if (c->k != NK::Var) return {nullptr, false};
            if (auto it = ret_caps.find(c->s); it != ret_caps.end()) return {it->second, false};
            if (ctor_name(c->s)) for (std::size_t i = 1; i < e.kids.size(); ++i) { auto t = taint(*e.kids[i], d + 1); if (t.first) return {t.first, true}; }
            return {nullptr, false};
        }
        default: return {nullptr, false};
        }
    }
    std::set<std::string> imported;
    void import_check(const Node& n) {
        bool native = n.s.ends_with(".so") || n.s.ends_with(".dylib") || n.s.ends_with(".dll");
        if (native) {   // load_plugin: native code is refused unless every capability is granted
            if (!(caps.fs && caps.net && caps.proc && caps.env)) D(L::error, "capability", n, "native module '" + n.s + "' needs every capability (fs, net, proc, env); this run denies some (raises PermissionError)", "import");
            return;
        }
        if (!caps.fs) { D(L::error, "capability", n, "import needs the 'fs' capability, which this run denies (raises PermissionError)", "import"); return; }
        // module_search_path (runtime.cpp): script dir, FORGE_SCRIPT_PATH, ~/.forge/modules, cwd
        std::vector<std::string> dirs;
        { auto sl = file.find_last_of('/'); dirs.push_back(sl == std::string::npos ? std::string(".") : file.substr(0, sl)); }
        for (auto& d : opt.module_dirs) dirs.push_back(d);
        if (const char* mp = std::getenv("FORGE_SCRIPT_PATH")) { std::string m = mp; std::size_t i = 0;
            while (i <= m.size()) { auto j = m.find_first_of(":;", i); if (j == std::string::npos) j = m.size(); if (j > i) dirs.push_back(m.substr(i, j - i)); i = j + 1; } }
        if (const char* h = std::getenv("HOME")) dirs.push_back(std::string(h) + "/.forge/modules");
        dirs.push_back(".");
        std::vector<std::string> forms;
        auto slash = n.s.find_last_of('/'); auto base = slash == std::string::npos ? n.s : n.s.substr(slash + 1);
        if (base.find('.') == std::string::npos) forms = {n.s + ".fs", n.s + ".cpp", n.s + "/mod.fs", n.s + "/mod.cpp"}; else forms = {n.s};
        std::vector<std::string> cand;
        if (!n.s.empty() && n.s[0] == '/') cand = forms; else for (auto& d : dirs) for (auto& f : forms) cand.push_back(d + "/" + f);
        for (auto& path : cand) {
            std::FILE* f = std::fopen(path.c_str(), "rb"); if (!f) continue;
            std::string code; char buf[4096]; std::size_t r; while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) code.append(buf, r); std::fclose(f);
            if (code.empty()) continue;   // directory or empty module: nothing to check
            if (!imported.insert(path).second) return;
            sub_check(n, code, "import");
            return;
        }
    }
    void sub_check(const Node& at, const std::string& code, const char* what) {
        std::shared_ptr<const FnDecl> t;
        try { t = parse_program(code, std::string("<") + what + ">"); } catch (const std::exception& e) { D(L::warning, "syntax", at, std::string(what) + " source does not parse: " + e.what()); return; }
        Checker sub; sub.src = code; sub.file = file; sub.opt = opt; sub.opt.warnings = false; sub.caps = caps; sub.imported = imported;
        if (t->body) sub.collect(*t->body, false);
        sub.fn(*t);
        imported.insert(sub.imported.begin(), sub.imported.end());
        for (auto& d : sub.out) if (d.severity == L::error) D(L::error, d.id.c_str(), at, std::string("in ") + what + " source: " + d.message);
    }
    // Deprecated APIs: built-in table plus script functions marked [[deprecated]] / [[deprecated("why")]].
    void deprecated(const Node& n, const Node& c) {
        static const std::map<std::string, std::string> table = {
            {"std::random_shuffle", "removed in C++17; use std::shuffle"}, {"std::auto_ptr", "removed in C++17; use std::unique_ptr"},
            {"std::bind1st", "removed in C++17; use a lambda"}, {"std::bind2nd", "removed in C++17; use a lambda"},
            {"std::ptr_fun", "removed in C++17; use a lambda"}, {"std::gets", "removed in C++14; use std::getline"},
            {"std::aligned_storage", "deprecated in C++23; use alignas storage"}, {"std::aligned_union", "deprecated in C++23"},
        };
        if (c.k == NK::Field && !c.kids.empty() && !(c.kids[0]->k == NK::Var && c.kids[0]->var.global() && !gbind.count(c.kids[0]->s) && !globals_fn.count(c.kids[0]->s)))
            if (auto it = method_dep.find(c.s); it != method_dep.end() && it->second.first > 0) {
                bool all = it->second.first == it->second.second;
                D(L::warning, "deprecated", n, "method " + c.s + "() is marked [[deprecated]]" + (all ? std::string() : std::string(" in some classes that define it")), c.s);
                return; }
        std::string name = c.k == NK::Var ? c.s : std::string();
        if (name.empty()) return;
        if (auto it = table.find(name); it != table.end()) { D(L::warning, "deprecated", n, name + " is deprecated: " + it->second, name); return; }
        if (c.var.global()) if (auto f = globals_fn.find(name); f != globals_fn.end())
            for (auto& a : f->second->attrs) if (a.rfind("deprecated", 0) == 0) { D(L::warning, "deprecated", n, name + "() is marked [[" + a + "]]", name); return; }
    }
    // Warning-level type inference: literal operands, and const locals initialised from a literal.
    char lit_of(const Node& e) {
        if (e.k == NK::Int) return 'i'; if (e.k == NK::Float) return 'f'; if (e.k == NK::Str) return 's';
        if (e.k == NK::Var) if (auto* b = bind_of(e.var)) return b->lit;
        return 0;
    }
    void type_check(const Node& n) {
        if (n.kids.size() != 2) return;
        char a = lit_of(*n.kids[0]), b = lit_of(*n.kids[1]); if (!a || !b) return;
        Op o = Op(n.i);
        bool num_a = a != 's', num_b = b != 's';
        bool bad = false;
        if (o == Op::Sub || o == Op::Div || o == Op::Mod || o == Op::BitAnd || o == Op::BitOr || o == Op::BitXor || o == Op::Shl || o == Op::Shr) bad = !num_a || !num_b;
        if (o == Op::Add) bad = num_a != num_b;
        if (o == Op::Mul) bad = !num_a && !num_b;
        if ((o == Op::Lt || o == Op::Le || o == Op::Gt || o == Op::Ge) && num_a != num_b) bad = true;
        if (bad) D(L::warning, "type", n, std::string("operator ") + op_name(o) + " on " + (num_a ? "number" : "string") + " and " + (num_b ? "number" : "string") + " likely raises TypeError");
    }
    // limit {...} / forge::limit(<map literal>, fn): unknown keys and malformed size / duration strings raise ValueError.
    void limit_options(const Node& m) {
        static const std::set<std::string> sizes = {"mem", "proc_mem"}, times = {"timeout", "cpu_s", "proc_cpu"}, counts = {"allocs", "handles", "procs"};
        auto split = [](const std::string& s, std::string& unit) {
            std::size_t i = 0; while (i < s.size() && s[i] == ' ') ++i;
            std::size_t b = i; bool dot = false; while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || (s[i] == '.' && !dot))) { dot = dot || s[i] == '.'; ++i; }
            if (i == b || (i == b + 1 && s[b] == '.')) return false;
            for (; i < s.size(); ++i) if (s[i] != ' ') unit += char(std::tolower(static_cast<unsigned char>(s[i])));
            return true;
        };
        for (std::size_t j = 0; j + 1 < m.kids.size(); j += 2) {
            if (m.kids[j]->k != NK::Str) continue;
            const std::string& k = m.kids[j]->s; const Node& v = *m.kids[j + 1];
            if (!sizes.count(k) && !times.count(k) && !counts.count(k)) { D(L::error, "limit-option", v, "unknown limit option '" + k + "' (raises ValueError)", k); continue; }
            if (v.k != NK::Str) continue;
            if (counts.count(k)) { D(L::error, "limit-option", v, "limit option '" + k + "' is a count and needs an integer (raises TypeError)", v.s); continue; }
            std::string u;
            bool ok = split(v.s, u);
            if (ok && sizes.count(k)) { static const std::set<std::string> su = {"", "b", "k", "kb", "kib", "m", "mb", "mib", "g", "gb", "gib", "t", "tb", "tib"}; ok = su.count(u) > 0; }
            if (ok && times.count(k)) { static const std::set<std::string> tu = {"", "ns", "us", "ms", "s", "m", "min", "h"}; ok = tu.count(u) > 0; }
            if (!ok) D(L::error, "limit-option", v, "bad value \"" + v.s + "\" for limit option '" + k + "' (raises ValueError)", v.s);
        }
    }
    void pool_task(const Node& n) {
        auto& s = *n.kids[1];
        if (s.k == NK::Str) {
            std::shared_ptr<const FnDecl> t;
            try { t = parse_program(s.s, "<task>"); } catch (const std::exception& e) { D(L::error, "pool-task", n, std::string("ProcessPool task source does not parse: ") + e.what()); return; }
            const FnDecl* task = nullptr; bool reassigned = false;
            std::function<void(const Node&)> walk = [&](const Node& x) {
                if (x.k == NK::FnStmt && x.fn && x.fn->name == "task") task = x.fn.get();
                if (x.k == NK::Assign && !x.kids.empty() && x.kids[0]->k == NK::Var && x.kids[0]->s == "task") reassigned = true;
                if (x.k == NK::Block) for (auto& k : x.kids) if (k) walk(*k);
            };
            if (t && t->body) walk(*t->body);
            if (!task && !reassigned) D(L::error, "pool-task", n, "ProcessPool task source does not define task(arg)");
            else if (task) { std::size_t req = 0; for (auto& p : task->params) if (!p.dflt) ++req;
                if (req > 1 || task->params.empty()) D(L::error, "pool-task", n, "ProcessPool task(arg) must take exactly one argument (it takes " + std::to_string(task->params.size()) + ")"); }
        }
        if (n.kids.size() > 2) {
            auto& a = *n.kids[2];
            if (non_json_literal(a)) D(L::error, "pool-json", a, "ProcessPool arguments must be JSON-representable; a function or shell value is not");
            else if ((a.k == NK::List || a.k == NK::Map) && !json_literal(a)) {
                bool bad = false; for (auto& k : a.kids) if (non_json_literal(*k)) bad = true;
                if (bad) D(L::error, "pool-json", a, "ProcessPool arguments must be JSON-representable; this literal contains a function");
            }
        }
    }
    void format_check(const std::string& f, std::size_t nargs, int line) {
        std::size_t autos = 0; int maxi = -1; bool bad = false;
        for (std::size_t i = 0; i < f.size(); ++i) {
            if (f[i] == '{') { if (i + 1 < f.size() && f[i + 1] == '{') { ++i; continue; }
                auto e = f.find('}', i); if (e == std::string::npos) { bad = true; break; }
                std::string in = f.substr(i + 1, e - i - 1); auto colon = in.find(':'); std::string id = in.substr(0, colon);
                if (id.empty()) ++autos; else if (std::all_of(id.begin(), id.end(), ::isdigit)) maxi = std::max(maxi, std::stoi(id)); else bad = true;
                i = e;
            } else if (f[i] == '}') { if (i + 1 < f.size() && f[i + 1] == '}') ++i; else bad = true; }
        }
        if (bad) { diag(L::warning, "format", line, "malformed format string"); return; }
        std::size_t need = std::max<std::size_t>(autos, std::size_t(maxi + 1));
        if (need > nargs) diag(L::warning, "format", line, "format string needs " + std::to_string(need) + " argument(s), " + std::to_string(nargs) + " given");
        else if (need < nargs) diag(L::warning, "format", line, "format string uses " + std::to_string(need) + " argument(s), " + std::to_string(nargs) + " given; the rest are ignored");
    }

    void expr(const Node& n, State& st) {
        switch (n.k) {
        case NK::Var: read_var(n, st); return;
        case NK::Field: if (!n.kids.empty() && (n.kids[0]->k == NK::Var || n.kids[0]->k == NK::Field)) { expr(*n.kids[0], st); check_key(n, lkey(n), st); return; } break;
        case NK::Binary: type_check(n); break;
        case NK::Move:
            if (!n.kids.empty() && (n.kids[0]->k == NK::Var || n.kids[0]->k == NK::Field)) { auto& v = *n.kids[0]; expr(v, st); auto k = lkey(v); if (!k.empty()) st[skey(k)] = DEF;
                if (v.k == NK::Var) if (auto* b = bind_of(v.var); b && !b->alias.empty()) { auto& t = st[skey(b->alias)]; t = std::max(t, int(MAYBE)); }
                return; }
            break;
        case NK::Copy:
            if (!n.kids.empty() && n.kids[0]->k == NK::Var) if (auto* b = bind_of(n.kids[0]->var); b && b->unique)
                D(L::error, "unique-copy", n, "'" + n.kids[0]->s + "' is a unique_ptr and cannot be copied (raises BorrowError); use std::move", n.kids[0]->s);
            break;
        case NK::Lambda: lambda(n, false); return;
        case NK::Call: call(n, st); return;
        case NK::Shell: if (!caps.proc) D(L::error, "capability", n, "shell blocks need the 'proc' capability, which this run denies (raises PermissionError)"); break;
        case NK::And: case NK::Or: case NK::Coalesce:
            if (n.kids.size() == 2) { expr(*n.kids[0], st); State b = st; expr(*n.kids[1], b); st = merge(st, b); return; }
            break;
        case NK::Ternary:
            if (n.kids.size() == 3) { expr(*n.kids[0], st); State a = st, b = st; expr(*n.kids[1], a); expr(*n.kids[2], b); st = merge(a, b); return; }
            break;
        default: break;
        }
        for (auto& k : n.kids) if (k) expr(*k, st);
    }

    // ---------------------------------------------------------------- statements. Returns false when control cannot fall through.
    bool block(const Node& n, State& st) {
        cur().scopes.emplace_back();
        bool live = true; bool warned = false;
        for (auto& k : n.kids) {
            if (!k) continue;
            if (!live && !warned && k->k != NK::FnStmt && k->k != NK::ClassStmt) { diag(L::warning, "unreachable", k->line, "unreachable code"); warned = true; }
            if (!live) { State dead = st; stmt(*k, dead); continue; }
            live = stmt(*k, st);
        }
        cur().scopes.pop_back();
        return live;
    }
    static bool has_exit(const Node& n, int loops) {   // a break of this loop, a return or a throw, not inside a nested lambda
        if (n.k == NK::Lambda || n.k == NK::FnStmt) return false;
        if (n.k == NK::Return || n.k == NK::Throw) return true;
        if (n.k == NK::Break && loops == 0) return true;
        if (n.k == NK::Call) { auto* c = callee(n); if (c && c->k == NK::Var && c->var.global() && (c->s == "std::exit" || c->s == "exit" || c->s == "std::abort")) return true; }
        int inner = loops + ((n.k == NK::While || n.k == NK::For) ? 1 : 0);
        for (auto& k : n.kids) if (k && has_exit(*k, n.k == NK::While || n.k == NK::For ? inner : loops)) return true;
        if (n.extra && has_exit(*n.extra, loops)) return true;
        return false;
    }
    bool stmt(const Node& n, State& st) {
        switch (n.k) {
        case NK::Block: return block(n, st);
        case NK::Let: {
            bool uniq = false, list = false, report = false;
            if (!n.kids.empty() && n.kids[0]) {
                auto& i = *n.kids[0];
                expr(i, st);
                if (auto* c = callee(i); c && is_builtin_var(*c, "forge::__own") && n.kids[0]->kids.size() > 1 && n.kids[0]->kids[1]->k == NK::Int && n.kids[0]->kids[1]->i == 2) uniq = true;
                if (i.k == NK::Move && !i.kids.empty() && i.kids[0]->k == NK::Var) if (auto* b = bind_of(i.kids[0]->var); b && b->unique) uniq = true;
                list = i.k == NK::List || i.k == NK::Map;
                report = i.k == NK::Nil || i.k == NK::Int || i.k == NK::Float || i.k == NK::Str || i.k == NK::True || i.k == NK::False || i.k == NK::List || i.k == NK::Map;
            }
            if (n.type) { static const std::set<std::string> cont = {"list", "map", "set", "dict", "vector", "deque", "std::vector", "std::deque", "std::list",
                "std::forward_list", "std::map", "std::unordered_map", "std::multimap", "std::set", "std::unordered_set", "std::multiset", "std::array"};
                if (cont.count(n.type->name)) list = true; }
            const char* cap = nullptr; bool chan = false, ctr = false, direct_ok = true; char lit = 0;
            if (!n.kids.empty() && n.kids[0]) { auto& i = n.kids[0]->k == NK::Copy && !n.kids[0]->kids.empty() ? *n.kids[0]->kids[0] : *n.kids[0];
                { auto t = taint(i); cap = t.first; ctr = t.second; if (cap && !(i.k == NK::Field)) direct_ok = false; }
                if (auto* c = callee(i); c && (is_builtin_var(*c, "forge::channel") || is_builtin_var(*c, "channel"))) chan = true;
                if (auto* c = callee(i); c && c->k == NK::Var && c->var.global() && (c->s == "std::vector" || c->s == "std::deque" || c->s == "std::list" || c->s == "std::map" || c->s == "std::unordered_map" || c->s == "std::set" || c->s == "std::unordered_set")) list = true;
                lit = i.k == NK::Int ? 'i' : i.k == NK::Float ? 'f' : i.k == NK::Str ? 's' : 0;
                if (!cap && (i.k == NK::List || i.k == NK::Map || (i.k == NK::Call && list))) for (auto& e : i.kids) if (e && e->k == NK::Field && !e->kids.empty() && e->kids[0]->k == NK::Var && e->kids[0]->var.global()) if (auto* c2 = cap_of(e->kids[0]->s, e->s)) { cap = c2; ctr = true; } }
            std::string alias = n.ref && !n.kids.empty() && n.kids[0] && (n.kids[0]->k == NK::Var || n.kids[0]->k == NK::Field) ? lkey(*n.kids[0]) : std::string();
            if (n.var.global() && !at_top()) return true;
            declare(n.var, n.s, n.line, report, n.flag);
            if (auto* b = bind_of(n.var.global() ? VarRef{n.s, -1, -1} : VarRef{n.s, 0, n.var.slot})) { b->unique = uniq; b->list = list; b->cap = cap; b->chan = chan; b->lit = n.flag ? lit : 0; b->alias = alias; b->container_cap = ctr; if (!n.kids.empty() && n.kids[0] && n.kids[0]->k == NK::Lambda) b->lam = n.kids[0]->fn.get(); if (!direct_ok) b->param = true; }
            { auto k = n.var.global() ? "g" + n.s : "l" + std::to_string(n.var.slot); kill(st, k); }
            return true;
        }
        case NK::Assign:
            if (n.kids.size() == 2) {
                expr(*n.kids[1], st);
                auto& t = *n.kids[0];
                if (t.k == NK::Var) { if (n.s != "=") read_var(t, st); auto k = lkey(t); if (!k.empty()) kill(st, k); if (auto* b = bind_of(t.var)) { b->cap = nullptr; b->chan = false; b->lit = 0; b->unique = false; } }
                else if (t.k == NK::Field && !t.kids.empty()) { expr(*t.kids[0], st); auto k = lkey(t); if (!k.empty()) kill(st, k); if (auto tt = taint(*n.kids[1]); tt.first && !tt.second) field_caps[t.s] = tt.first; }
                else expr(t, st);
            }
            return true;
        case NK::ExprStmt: for (auto& k : n.kids) if (k) expr(*k, st); return true;
        case NK::If: {
            expr(*n.kids[0], st);
            State a = st, b = st;
            bool la = n.kids.size() > 1 && n.kids[1] ? stmt(*n.kids[1], a) : true;
            bool lb = n.kids.size() > 2 && n.kids[2] ? stmt(*n.kids[2], b) : true;
            if (la && lb) st = merge(a, b); else if (la) st = a; else if (lb) st = b;
            return la || lb;
        }
        case NK::While: {
            bool infinite = !n.kids.empty() && n.kids[0] && n.kids[0]->k == NK::True;
            if (infinite && !(n.kids.size() > 1 && n.kids[1] && has_exit(*n.kids[1], 0)) && !(n.extra && has_exit(*n.extra, 0)))
                D(L::warning, "infinite-loop", n, "loop has no break, return or throw; it only ends on a timeout, step limit or cancellation");
            loop(n, st, n.kids[0].get(), n.kids.size() > 1 ? n.kids[1].get() : nullptr, nullptr);
            return !infinite || has_exit(n, 0);
        }
        case NK::For: {
            expr(*n.kids[0], st);
            auto& srcn = *n.kids[0];
            std::string k = key(srcn); bool lst = false;
            if (srcn.k == NK::Var) if (auto* b = bind_of(srcn.var)) lst = b->list;
            declare(n.var, n.var.name, n.line, false, false);
            iter_stack.push_back(k); iter_list.push_back({k, lst});
            loop(n, st, nullptr, n.kids.size() > 1 ? n.kids[1].get() : nullptr, nullptr);
            iter_stack.pop_back(); iter_list.pop_back();
            return true;
        }
        case NK::FnStmt: if (n.fn) { fn(*n.fn); } return true;
        case NK::Return:
            for (auto& k : n.kids) if (k) { expr(*k, st); if (auto t = taint(*k); t.first && !t.second && cur().decl && !cur().decl->is_top) {
                if (!cur().decl->name.empty() && cur().decl->name != "<lambda>") ret_caps[(cur().decl->is_method ? "." : "") + cur().decl->name] = t.first;
                lambda_ret[cur().decl] = t.first; } }
            return false;
        case NK::ClassStmt:
            if (n.cls) for (auto& m : n.cls->methods) if (m) fn(*m);
            return true;
        case NK::Throw: for (auto& k : n.kids) if (k) expr(*k, st); return false;
        case NK::Break: case NK::Continue: return false;
        case NK::Try: {
            State entry = st; bool live = n.kids[0] ? stmt(*n.kids[0], st) : true;
            State after = live ? st : State{}; bool any = live;
            std::vector<std::string> prior;
            for (std::size_t i = 1; i < n.kids.size(); ++i) {
                auto& c = *n.kids[i]; std::string t = c.s;
                for (auto& p : prior) if (p.empty() || p == "..." || (err_is_a(t, p) && err_parent().count(t)))
                    { D(L::warning, "catch-order", c, "catch (" + (t.empty() ? std::string("...") : t) + ") is unreachable: an earlier catch (" + (p.empty() ? std::string("...") : p) + ") already handles it", t); break; }
                prior.push_back(t);
                State cs = merge(entry, st);
                if (!c.var.name.empty() && !c.var.global()) declare(c.var, c.var.name, c.line, false, false);
                bool lc = stmt(c, cs);
                if (lc) { after = any ? merge(after, cs) : cs; any = true; }
            }
            if (n.extra) { State fs = any ? after : merge(entry, st); stmt(*n.extra, fs); if (any) after = fs; }
            st = any ? after : st;
            return any;
        }
        case NK::Import: import_check(n); return true;
        case NK::Defer: for (auto& k : n.kids) if (k) { State d = st; stmt(*k, d); } return true;
        default:
            for (auto& k : n.kids) if (k) expr(*k, st);
            return true;
        }
    }
    void loop(const Node& n, State& st, const Node* cond, const Node* body, const Node*) {
        State entry = st;
        State s1 = st; if (cond) expr(*cond, s1);
        State before_body = s1;
        if (body) stmt(*body, s1);
        if (n.extra) stmt(*n.extra, s1);
        // second pass: state carried round the back edge, reported as warnings at most
        State s2 = merge(before_body, s1); for (auto& [k, v] : s2) if (v == DEF && !(before_body.count(k) && before_body[k] == DEF)) v = MAYBE;
        bool q = quiet; quiet = true;
        if (cond) expr(*cond, s2);
        if (body) stmt(*body, s2);
        if (n.extra) stmt(*n.extra, s2);
        quiet = q;
        st = merge(entry, merge(s1, s2));
        for (auto& [k, v] : st) if (v == DEF && !(entry.count(k) && entry[k] == DEF)) v = MAYBE;
    }
    static bool always_returns(const Node& n) {
        switch (n.k) {
        case NK::Return: case NK::Throw: return true;
        case NK::Block: for (auto& k : n.kids) if (k && always_returns(*k)) return true; return false;
        case NK::If: return n.kids.size() > 2 && n.kids[1] && n.kids[2] && always_returns(*n.kids[1]) && always_returns(*n.kids[2]);
        case NK::While: return !n.kids.empty() && n.kids[0] && n.kids[0]->k == NK::True && !has_exit_break(*n.kids[1]);
        case NK::Try: { if (!n.kids[0] || !always_returns(*n.kids[0])) return false; for (std::size_t i = 1; i < n.kids.size(); ++i) if (!always_returns(*n.kids[i])) return false; return true; }
        case NK::ExprStmt: if (!n.kids.empty()) if (auto* c = callee(*n.kids[0]); c && c->k == NK::Var && c->var.global() && (c->s == "std::exit" || c->s == "std::abort")) return true; return false;
        default: return false;
        }
    }
    static bool has_exit_break(const Node& n) {
        if (n.k == NK::Lambda || n.k == NK::FnStmt || n.k == NK::While || n.k == NK::For) return false;
        if (n.k == NK::Break) return true;
        for (auto& k : n.kids) if (k && has_exit_break(*k)) return true;
        return false;
    }
    void fn(const FnDecl& f) {
        fns.emplace_back(); cur().decl = &f; cur().scopes.emplace_back();
        for (std::size_t pi = 0; pi < f.params.size(); ++pi) { auto& p = f.params[pi]; declare(VarRef{p.name, 0, p.slot}, p.name, f.line, false, p.cnst);
            if (auto it = param_caps.find({(f.is_method ? "." : "") + f.name, pi}); it != param_caps.end() && !f.is_top) if (auto* b = cur().slot[p.slot]) { b->cap = it->second; b->param = true; } }
        State st;
        if (f.body) stmt(*f.body, st);
        static const std::set<std::string> no_value = {"", "void", "auto", "any"};
        if (!f.is_top && f.body && !no_value.count(f.ret_name) && f.name != "main" && !always_returns(*f.body))
            diag(L::warning, "missing-return", f.line, "function '" + f.name + "' is declared to return " + f.ret_name + " but can reach its end without returning (returns nil)", f.name);
        end_fn();
    }
    std::map<std::pair<std::string, std::size_t>, const char*> param_caps;   // script fn, param index -> builtin capability passed there
    std::map<std::string, std::pair<int, int>> method_dep;                   // method name -> (deprecated defs, all defs)
    void collect(const Node& n, bool in_fn) {
        if (n.k == NK::Call && n.kids.size() > 1 && n.kids[0]->k == NK::Var && n.kids[0]->var.global())
            for (std::size_t i = 1; i < n.kids.size(); ++i) { auto& e = *n.kids[i];
                if (e.k == NK::Field && !e.kids.empty() && e.kids[0]->k == NK::Var && e.kids[0]->var.global()) if (auto* cp = cap_of(e.kids[0]->s, e.s)) param_caps[{n.kids[0]->s, i - 1}] = cp; }
        if (n.k == NK::Call && n.kids.size() > 1 && n.kids[0]->k == NK::Field)
            for (std::size_t i = 1; i < n.kids.size(); ++i) { const Node* e = n.kids[i].get(); if (e->k == NK::Copy && !e->kids.empty()) e = e->kids[0].get();
                if (e->k == NK::Field && !e->kids.empty() && e->kids[0]->k == NK::Var && e->kids[0]->var.global()) if (auto* cp = cap_of(e->kids[0]->s, e->s)) param_caps[{"." + n.kids[0]->s, i - 1}] = cp; }
        if (n.k == NK::ClassStmt && n.cls) for (auto& m : n.cls->methods) if (m) {
            auto& d = method_dep[m->name]; ++d.second;
            for (auto& a : m->attrs) if (a.rfind("deprecated", 0) == 0) { ++d.first; break; }
            if (m->body) collect(*m->body, true); }
        if (n.k == NK::Var && n.var.global()) gread.insert(n.s);
        if (in_fn && n.k == NK::Assign && !n.kids.empty() && n.kids[0]->k == NK::Var && n.kids[0]->var.global()) fn_assigned.insert(n.kids[0]->s);
        if (in_fn && n.k == NK::Move && !n.kids.empty() && n.kids[0]->k == NK::Var && n.kids[0]->var.global()) fn_assigned.insert(n.kids[0]->s);
        if (n.k == NK::FnStmt && n.fn && n.var.global() && !n.fn->is_method) { if (globals_fn.count(n.fn->name)) global_assigns[n.fn->name]++; globals_fn[n.fn->name] = n.fn.get(); }
        if (n.k == NK::Assign && !n.kids.empty() && n.kids[0]->k == NK::Var && n.kids[0]->var.global()) global_assigns[n.kids[0]->s]++;
        if (n.k == NK::Let && n.var.global()) global_assigns[n.s]++;
        for (auto& k : n.kids) if (k) collect(*k, in_fn);
        if (n.extra) collect(*n.extra, in_fn);
        if (n.fn && n.fn->body && n.k == NK::Lambda) collect(*n.fn->body, true);
        if (n.k == NK::FnStmt && n.fn && n.fn->body) collect(*n.fn->body, true);
    }
    void lock_cycles() {
        std::map<std::string, int> idx, low; std::vector<std::string> stk; std::set<std::string> on; int counter = 0;
        std::function<void(const std::string&)> sc = [&](const std::string& v) {
            idx[v] = low[v] = counter++; stk.push_back(v); on.insert(v);
            for (auto& w : lock_edges[v]) { if (!idx.count(w)) { sc(w); low[v] = std::min(low[v], low[w]); } else if (on.count(w)) low[v] = std::min(low[v], idx[w]); }
            if (low[v] == idx[v]) {
                std::vector<std::string> comp; std::string w;
                do { w = stk.back(); stk.pop_back(); on.erase(w); comp.push_back(w); } while (w != v);
                if (comp.size() < 2) return;
                std::sort(comp.begin(), comp.end());
                std::string names, lines; int last = 0;
                for (auto& x : comp) names += (names.empty() ? "'" : ", '") + x + "'";
                for (auto& [e, l] : edge_line) if (std::find(comp.begin(), comp.end(), e.first) != comp.end() && std::find(comp.begin(), comp.end(), e.second) != comp.end()) { lines += (lines.empty() ? "" : ", ") + std::to_string(l); last = std::max(last, l); }
                diag(L::warning, "lock-order", last, "forge::shared values " + names + " are locked in a cyclic order (nested with() at lines " + lines + "); this can deadlock (DeadlockError under --debug)");
            }
        };
        std::vector<std::string> keys; for (auto& [k, v] : lock_edges) keys.push_back(k);
        for (auto& k : keys) if (!idx.count(k)) sc(k);
    }
    void suppress() {
        // `// forge-check: ignore` or `// forge-check: ignore[id,id]` on the flagged line or the line above;
        // `// forge-check: ignore-file[id]` anywhere disables those ids for the file.
        std::vector<std::string> lines; std::size_t p = 0;
        while (p <= src.size()) { auto e = src.find('\n', p); lines.emplace_back(src.substr(p, e == std::string_view::npos ? std::string_view::npos : e - p)); if (e == std::string_view::npos) break; p = e + 1; }
        auto ids_of = [](const std::string& l, const char* tag, bool& all) -> std::set<std::string> {
            std::set<std::string> r; auto q = l.find(tag); if (q == std::string::npos) return r;
            q += std::string(tag).size();
            if (q < l.size() && l[q] == '[') { auto e = l.find(']', q); std::string s = l.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1); std::size_t b = 0;
                while (b <= s.size()) { auto c = s.find(',', b); auto id = s.substr(b, c == std::string::npos ? std::string::npos : c - b); id.erase(0, id.find_first_not_of(' ')); id.erase(id.find_last_not_of(' ') + 1); if (!id.empty()) r.insert(id); if (c == std::string::npos) break; b = c + 1; } }
            else all = true;
            r.insert("\x01"); return r;
        };
        std::set<std::string> file_ids; bool file_all = false;
        for (auto& l : lines) { bool a = false; auto s = ids_of(l, "forge-check: ignore-file", a); if (!s.empty()) { file_ids.insert(s.begin(), s.end()); file_all |= a; } }
        std::erase_if(out, [&](const diagnostic& d) {
            if (d.id == "syntax") return false;
            if (file_all || file_ids.count(d.id)) return true;
            for (int ln : {d.line, d.line - 1}) if (ln >= 1 && ln <= int(lines.size())) {
                auto& l = lines[ln - 1]; if (l.find("forge-check: ignore-file") != std::string::npos) continue;
                bool all = false; auto s = ids_of(l, "forge-check: ignore", all); if (s.empty()) continue;
                if (all || s.count(d.id)) return true;
            }
            return false;
        });
    }
};

std::string jesc(const std::string& s) {
    std::string r; for (unsigned char c : s) { if (c == '"' || c == '\\') { r += '\\'; r += char(c); } else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; } else r += char(c); }
    return r;
}

} // namespace

std::vector<diagnostic> check_parsed(const FnDecl& top, std::string_view src, const std::string& file, const check_options& o) {
    // Pass 1 summarises functions (returned builtins, lambda returns, field stores) so call order does not matter;
    // pass 2 reports with those summaries.
    Checker s1; s1.src = src; s1.file = file; s1.opt = o; s1.caps = {o.allow_fs, o.allow_net, o.allow_proc, o.allow_env};
    if (top.body) s1.collect(*top.body, false);
    for (int round = 0; round < 2; ++round) s1.fn(top);   // twice: summaries that depend on other summaries
    Checker c; c.src = src; c.file = file; c.opt = o; c.caps = s1.caps;
    if (top.body) c.collect(*top.body, false);
    c.ret_caps = s1.ret_caps; c.lambda_ret = s1.lambda_ret; c.field_caps = s1.field_caps;
    c.fn(top);
    c.lock_cycles();
    c.suppress();
    std::stable_sort(c.out.begin(), c.out.end(), [](auto& a, auto& b) { return a.line < b.line || (a.line == b.line && a.col < b.col); });
    return std::move(c.out);
}

std::vector<diagnostic> check(std::string_view src, const std::string& file, const check_options& o) {
    std::shared_ptr<const FnDecl> top;
    try { top = parse_program(src, file); }
    catch (const syntax_error& e) { return {diagnostic{L::error, "syntax", e.what(), file, e.line, 0}}; }
    return check_parsed(*top, src, file, o);
}

bool has_errors(const std::vector<diagnostic>& d) { return std::any_of(d.begin(), d.end(), [](auto& x) { return x.severity == L::error; }); }

std::string to_text(const std::vector<diagnostic>& d) {
    std::string r;
    for (auto& x : d) r += x.file + ":" + std::to_string(x.line) + ":" + std::to_string(x.col) + ": " + (x.severity == L::error ? "error" : "warning") + "[" + x.id + "]: " + x.message + "\n";
    return r;
}
std::string to_json(const std::vector<diagnostic>& d) {
    std::string r = "[";
    for (std::size_t i = 0; i < d.size(); ++i) {
        auto& x = d[i]; if (i) r += ",";
        r += "{\"file\":\"" + jesc(x.file) + "\",\"line\":" + std::to_string(x.line) + ",\"col\":" + std::to_string(x.col) + ",\"severity\":\"" + (x.severity == L::error ? "error" : "warning") + "\",\"id\":\"" + jesc(x.id) + "\",\"message\":\"" + jesc(x.message) + "\"}";
    }
    return r + "]";
}

} // namespace forge::script
