// Write once, emit anywhere: translate a script in FORGE's portable subset into a standalone native
// script for one shell (docs/scripting.md). The core here does all the language work: type inference
// (int / string / bool / process result), hoisting of calls and short-circuit operators into
// statements, `for(;;)` step handling around `continue`, and the mapping of the portable OS surface
// (os.env, fs.*, forge::sh, forge::shell). Dialects (shell_backend::emitter, one per shell file)
// only spell the constructs. Anything outside the subset is an emit_error naming the line.
#include <forge/script/script.hpp>
#include "parser.hpp"
#include "shells/shell.hpp"
#include "shells/emit_dialect.hpp"
#include <map>
#include <set>
#include <cctype>
#include <cstdlib>
#include <forge/os/process.hpp>

namespace forge::script {

namespace {
const char* nk_name[] = {"Nil","True","False","Int","Float","Str","Interp","Var","List","Map","Unary","Binary","And","Or","Coalesce","Call","Index","Field","OptField","Lambda","Shell","Is","Spawn","Await","Self","Super","Ternary","Move","Copy","Block","Let","Assign","ExprStmt","If","While","For","FnStmt","Return","Break","Continue","Throw","Try","ClassStmt","Using","Defer","Import"};

void dump(const Node* n, int d, std::string& o) {
    o += std::string(d * 2, ' ');
    if (!n) { o += "null\n"; return; }
    o += nk_name[int(n->k)]; o += " s='" + n->s + "' i=" + std::to_string(n->i) + (n->flag ? " flag" : "") + " var=" + n->var.name + "/" + std::to_string(n->var.depth) + ":" + std::to_string(n->var.slot) + (n->type ? " type=" + n->type->str() : "") + " L" + std::to_string(n->line) + "\n";
    for (auto& k : n->kids) dump(k.get(), d + 1, o);
    if (n->extra) { o += std::string(d * 2 + 2, ' ') + "<extra>\n"; dump(n->extra.get(), d + 2, o); }
    if (n->fn) { o += std::string(d * 2 + 2, ' ') + "<fn " + n->fn->name + " params:"; for (auto& p : n->fn->params) o += " " + p.name + (p.type ? ":" + p.type->str() : ""); o += ">\n"; dump(n->fn->body.get(), d + 2, o); }
}

// Names that are special in at least one target shell; user names that collide get a trailing '_'.
const std::set<std::string>& reserved() {
    static const std::set<std::string> r = {"path", "status", "argv", "argc", "home", "user", "term", "shell", "prompt", "cwd", "pid", "history",
        "version", "fpath", "cdpath", "options", "pipestatus", "signals", "funcstack", "fish_pid", "umask", "echo", "time", "watch", "mail",
        "e", "env", "nu", "in", "it", "args", "true", "false", "null", "input", "host", "pwd", "error", "matches", "this", "_", "value", "event",
        "home", "owd", "dirstack", "cdpath", "noclobber", "ignoreeof", "histchars", "savehist", "child", "fignore", "list", "set", "math", "str",
        "re", "os", "fs", "type", "test", "local", "global", "def", "fn", "var", "let", "if", "else", "while", "for", "return", "break",
        "continue", "function", "end", "begin", "and", "or", "not", "exit", "print", "len", "str", "int", "float", "bool", "dict", "list",
        "range", "open", "file", "id", "hash", "max", "min", "sum", "all", "any", "chr", "ord", "next", "iter", "object", "exec", "eval",
        "lambda", "pass", "with", "yield", "class", "import", "from", "as", "try", "except", "raise", "del", "assert", "is", "nonlocal", "path_",
        "ifs", "ps1", "ps2", "random", "seconds", "lineno", "optarg", "optind", "ppid", "ifs", "shlvl", "hostname", "oldpwd", "cmd"};
    return r;
}
std::string mangle(const std::string& n) {
    std::string s; for (char c : n) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    bool upper = false; for (char c : s) if (std::isupper(static_cast<unsigned char>(c))) upper = true;
    if (upper || reserved().count(s) || s.starts_with("_")) s += "_";
    return s;
}

struct fninfo { std::vector<ety> params; ety ret = ety::Unknown; std::string name; };

struct Emitter {
    emit_dialect& D; emit_ctx& C;
    std::map<std::string, ety> globals, locals;
    std::map<std::string, fninfo> fns;
    const fninfo* cur_fn = nullptr; std::string cur_fn_name;
    std::vector<const Node*> loop_steps;   // per loop: step to run before `continue` (null for none)

    [[noreturn]] void fail(const Node& n, const std::string& m) { throw emit_error(n.line, m); }
    [[noreturn]] void cant(const Node& n, const std::string& what) { throw emit_error(n.line, what + " can't be expressed in " + C.target + " (not in the portable emit subset; see docs/scripting.md \"Write once, emit anywhere\")"); }
    void at(const Node& n) { C.line = n.line; }

    static std::string callee(const Node& n) {   // Call -> qualified name ("std::println", "fs.exists", "r.out")
        const Node& f = *n.kids[0];
        if (f.k == NK::Var) return f.s;
        if (f.k == NK::Field && f.kids[0]->k == NK::Var) return f.kids[0]->s + "." + f.s;
        return {};
    }
    static std::string norm(std::string s) {      // spelling variants of the portable surface
        for (auto [a, b] : std::initializer_list<std::pair<const char*, const char*>>{{"forge::os::", "os."}, {"forge::fs::", "fs."}, {"std::filesystem::", "fs."}, {"forge::fs.", "fs."}}) if (s.starts_with(a)) s = b + s.substr(std::string(a).size());
        if (s == "fs.create_directories" || s == "fs.create_directory") return "fs.mkdir";
        if (s == "fs.remove_all") return "fs.remove";
        if (s == "fs.is_directory") return "fs.is_dir";
        if (s == "fs.is_regular_file") return "fs.is_file";
        if (s == "fs.current_path") return "os.cwd";
        if (s == "std::getenv" || s == "getenv") return "os.env";
        if (s == "std::exit" || s == "exit" || s == "forge::exit") return "os.exit";
        return s;
    }

    ety var_type(const Node& v) { auto n = mangle(v.s); if (auto it = locals.find(n); it != locals.end() && C.in_fn) return it->second; if (auto it = globals.find(n); it != globals.end()) return it->second; return ety::Unknown; }
    bool is_global(const std::string& m) { return !C.in_fn || (!locals.count(m) && globals.count(m)); }

    // ---- static typing (for return-type inference and Let without a type)
    ety ann_type(const TypeAnn* t) {
        if (!t) return ety::Unknown;
        auto& n = t->name;
        if (n == "int" || n == "long" || n == "long long" || n == "short" || n == "size_t" || n == "std::size_t" || n == "int64_t" || n == "std::int64_t" || n == "unsigned") return ety::Int;
        if (n == "string" || n == "std::string" || n == "str" || n == "std::string_view" || n == "string_view") return ety::Str;
        if (n == "bool") return ety::Bool;
        return ety::Unknown;
    }
    ety type_of(const Node& n, std::map<std::string, ety>* env = nullptr) {
        auto look = [&](const std::string& m) -> ety { if (env) if (auto it = env->find(m); it != env->end()) return it->second; if (C.in_fn) if (auto it = locals.find(m); it != locals.end()) return it->second; if (auto it = globals.find(m); it != globals.end()) return it->second; return ety::Unknown; };
        switch (n.k) {
        case NK::Int: return ety::Int;
        case NK::Str: case NK::Interp: return ety::Str;
        case NK::True: case NK::False: return ety::Bool;
        case NK::Var: return look(mangle(n.s));
        case NK::Unary: return n.s == "not" ? ety::Bool : type_of(*n.kids[0], env);
        case NK::And: case NK::Or: return ety::Bool;
        case NK::Binary: {
            Op o = op_of(n.s);
            if (o == Op::Eq || o == Op::Ne || o == Op::Lt || o == Op::Le || o == Op::Gt || o == Op::Ge) return ety::Bool;
            auto a = type_of(*n.kids[0], env), b = type_of(*n.kids[1], env);
            if (a == ety::Str || b == ety::Str) return ety::Str;
            if (a == ety::Int || b == ety::Int) return ety::Int;
            return ety::Unknown;
        }
        case NK::Ternary: { auto a = type_of(*n.kids[1], env); return a != ety::Unknown ? a : type_of(*n.kids[2], env); }
        case NK::Field: {
            if (n.kids[0]->k == NK::Var && look(mangle(n.kids[0]->s)) == ety::Res) return n.s == "out" ? ety::Str : n.s == "code" ? ety::Int : n.s == "ok" ? ety::Bool : ety::Unknown;
            if (n.s == "platform") return ety::Str;
            return ety::Unknown;
        }
        case NK::Call: {
            auto c = norm(callee(n));
            if (c == "os.env" || c == "fs.read" || c == "os.cwd" || c == "std::to_string" || c == "std::format") return ety::Str;
            if (c == "fs.exists" || c == "fs.is_dir" || c == "fs.is_file") return ety::Bool;
            if (c == "std::stoi" || c == "std::stol" || c == "std::stoll") return ety::Int;
            if (c == "forge::sh" || c == "forge::shell") return ety::Res;
            if (n.kids[0]->k == NK::Field && (n.kids[0]->s == "size" || n.kids[0]->s == "length")) return ety::Int;
            if (n.kids[0]->k == NK::Field && n.kids[0]->s == "empty") return ety::Bool;
            if (auto it = fns.find(c); it != fns.end()) return it->second.ret;
            return ety::Unknown;
        }
        default: return ety::Unknown;
        }
    }
    void collect_returns(const Node* n, std::map<std::string, ety>& env, ety& r) {
        if (!n) return;
        if (n->k == NK::Let && !n->kids.empty()) { auto t = ann_type(n->type.get()); if (t == ety::Unknown) t = type_of(*n->kids[0], &env); env[mangle(n->s)] = t; }
        if (n->k == NK::Return && !n->kids.empty() && r == ety::Unknown) r = type_of(*n->kids[0], &env);
        if (n->k == NK::Return && n->kids.empty() && r == ety::Unknown) r = ety::Void;
        for (auto& k : n->kids) collect_returns(k.get(), env, r);
        collect_returns(n->extra.get(), env, r);
    }

    // ---- expressions
    struct X { std::string c; ety t; };
    std::string cond(const Node& n) {
        auto x = expr(n);
        if (x.t == ety::Cond) return x.c;
        if (x.t == ety::Bool) return D.truthy(x.c);
        if (x.t == ety::Int) return D.cmp("!=", x.c, D.i_lit(0), true);
        cant(n, "using a string or result as a condition");
    }
    std::string val(const Node& n, ety* t = nullptr) {
        auto x = expr(n);
        if (x.t == ety::Cond) x = {bool_of(x.c), ety::Bool};
        if (x.t == ety::Res) cant(n, "a process result used as a value (use .out, .code or .ok)");
        if (x.t == ety::Void) fail(n, "a void call has no value");
        if (t) *t = x.t;
        return x.c;
    }
    std::string str(const Node& n) {
        ety t; auto v = val(n, &t);
        return t == ety::Int ? D.i2s(v) : v;
    }
    std::string intv(const Node& n) {
        ety t; auto v = val(n, &t);
        if (t == ety::Str) cant(n, "string used where an integer is needed (use std::stoi)");
        return v;
    }
    std::string bool_of(const std::string& c) {   // condition -> "true"/"false" value via a temp
        auto t = C.tmp();
        D.let(t, D.s_lit("false"), ety::Bool, !C.in_fn, true);
        D.if_(c); D.let(t, D.s_lit("true"), ety::Bool, !C.in_fn, false); D.end_if();
        return D.ref(t, ety::Bool, !C.in_fn);
    }
    // Format string with {} placeholders -> string value.
    std::string format(const Node& call, std::size_t first) {
        const Node& f = *call.kids[first];
        if (f.k != NK::Str) cant(f, "a non-literal format string");
        std::vector<std::string> parts; std::string lit; std::size_t ai = first + 1;
        const std::string& s = f.s;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '{') { lit += '{'; ++i; continue; }
            if (s[i] == '}' && i + 1 < s.size() && s[i + 1] == '}') { lit += '}'; ++i; continue; }
            if (s[i] == '{') {
                auto e = s.find('}', i);
                if (e == std::string::npos) fail(f, "unterminated {} in format string");
                if (e != i + 1) cant(f, "format spec '" + s.substr(i, e - i + 1) + "' (only plain {} is portable)");
                if (ai >= call.kids.size()) fail(f, "format string has more {} than arguments");
                if (!lit.empty()) { parts.push_back(D.s_lit(lit)); lit.clear(); }
                parts.push_back(str(*call.kids[ai++]));
                i = e; continue;
            }
            lit += s[i];
        }
        if (!lit.empty() || parts.empty()) parts.push_back(D.s_lit(lit));
        return parts.size() == 1 ? parts[0] : D.concat(parts);
    }
    // forge::sh("tmpl {}", args...) -> command string with each {} quoted for sh at run time.
    std::string sh_template(const Node& call) {
        const Node& f = *call.kids[1];
        if (f.k != NK::Str) return str(f);
        std::vector<std::string> parts; std::string lit; std::size_t ai = 2;
        for (std::size_t i = 0; i < f.s.size(); ++i) {
            if (f.s[i] == '{' && i + 1 < f.s.size() && f.s[i + 1] == '}' && ai < call.kids.size()) {
                if (!lit.empty()) { parts.push_back(D.s_lit(lit)); lit.clear(); }
                parts.push_back(D.sh_quote(str(*call.kids[ai++]))); ++i; continue;
            }
            lit += f.s[i];
        }
        if (!lit.empty() || parts.empty()) parts.push_back(D.s_lit(lit));
        return parts.size() == 1 ? parts[0] : D.concat(parts);
    }

    X expr(const Node& n) {
        at(n);
        switch (n.k) {
        case NK::Int: return {D.i_lit(n.i), ety::Int};
        case NK::Str: return {D.s_lit(n.s), ety::Str};
        case NK::True: return {D.s_lit("true"), ety::Bool};
        case NK::False: return {D.s_lit("false"), ety::Bool};
        case NK::Var: {
            if (n.s == "os.platform") return {D.platform(), ety::Str};
            auto m = mangle(n.s); auto t = var_type(n);
            if (t == ety::Unknown) fail(n, "unknown name '" + n.s + "' (the emitter only knows script variables and the portable surface)");
            if (t == ety::Res) return {m, ety::Res};
            return {D.ref(m, t, is_global(m)), t};
        }
        case NK::Field: {
            if (n.kids[0]->k == NK::Var && n.kids[0]->s == "os" && n.s == "platform") return {D.platform(), ety::Str};
            if (n.kids[0]->k == NK::Var && var_type(*n.kids[0]) == ety::Res) {
                auto b = mangle(n.kids[0]->s); bool g = is_global(b + "_out");
                if (n.s == "out") return {D.ref(b + "_out", ety::Str, g), ety::Str};
                if (n.s == "code") return {D.ref(b + "_code", ety::Int, g), ety::Int};
                if (n.s == "ok") return {D.cmp("==", D.ref(b + "_code", ety::Int, g), D.i_lit(0), true), ety::Cond};
                cant(n, "result field ." + n.s + " (portable fields: out, code, ok)");
            }
            cant(n, "field access ." + n.s);
        }
        case NK::Unary: {
            if (n.s == "not") return {D.c_not(cond(*n.kids[0])), ety::Cond};
            if (n.s == "-") return {D.neg(intv(*n.kids[0])), ety::Int};
            if (n.s == "+") return {intv(*n.kids[0]), ety::Int};
            cant(n, "unary operator " + n.s);
        }
        case NK::And: case NK::Or: {
            auto a = cond(*n.kids[0]);
            // the right side must only run when needed: if it emits statements, lower to ifs
            auto mark = C.lines.size();
            auto b = cond(*n.kids[1]);
            if (C.lines.size() == mark) {
                auto c = n.k == NK::And ? D.c_and(a, b) : D.c_or(a, b);
                if (!c.empty()) return {c, ety::Cond};
            }
            std::vector<std::string> pre(C.lines.begin() + mark, C.lines.end()); C.lines.resize(mark);
            auto t = C.tmp(); bool g = !C.in_fn;
            D.let(t, D.s_lit(n.k == NK::And ? "false" : "true"), ety::Bool, g, true);
            D.if_(n.k == NK::And ? a : D.c_not(a));
            int ind = C.indent; for (auto& l : pre) C.lines.push_back(std::string(C.ind) + l);   // re-indent the hoisted rhs into the if
            (void)ind;
            D.if_(n.k == NK::And ? b : D.c_not(b)); D.let(t, D.s_lit(n.k == NK::And ? "true" : "false"), ety::Bool, g, false); D.end_if();
            D.end_if();
            return {D.truthy(D.ref(t, ety::Bool, g)), ety::Cond};
        }
        case NK::Binary: {
            Op o = op_of(n.s);
            if (n.s == "<<" && n.kids[0]->k != NK::Int) cant(n, "operator << on a stream outside a statement");
            if (o == Op::Eq || o == Op::Ne || o == Op::Lt || o == Op::Le || o == Op::Gt || o == Op::Ge) {
                ety ta, tb; auto a = val(*n.kids[0], &ta); auto b = val(*n.kids[1], &tb);
                bool ints = ta == ety::Int && tb == ety::Int;
                if (!ints && (ta == ety::Int || tb == ety::Int)) cant(n, "comparing an integer with a string");
                if (!ints && ta == ety::Bool && o != Op::Eq && o != Op::Ne) cant(n, "ordering booleans");
                return {D.cmp(op_name(o), a, b, ints), ety::Cond};
            }
            if (o == Op::Add) {
                auto ta = type_of(*n.kids[0]), tb = type_of(*n.kids[1]);
                if (ta == ety::Str || tb == ety::Str) {
                    std::vector<std::string> parts; flatten_concat(n, parts);
                    return {D.concat(parts), ety::Str};
                }
            }
            if (o == Op::Add || o == Op::Sub || o == Op::Mul || o == Op::Div || o == Op::Mod) {
                auto a = intv(*n.kids[0]); auto b = intv(*n.kids[1]);
                return {D.arith(op_name(o)[0], a, b), ety::Int};
            }
            cant(n, std::string("operator ") + n.s);
        }
        case NK::Ternary: {
            auto c = cond(*n.kids[0]);
            auto t = C.tmp(); bool g = !C.in_fn; ety ty = type_of(*n.kids[1]);
            if (ty == ety::Unknown) ty = type_of(*n.kids[2]);
            if (ty != ety::Int && ty != ety::Str && ty != ety::Bool) cant(n, "a ternary whose type can't be inferred");
            D.let(t, ty == ety::Int ? D.i_lit(0) : D.s_lit(""), ty, g, true);
            D.if_(c);
            { auto v = ty == ety::Int ? intv(*n.kids[1]) : ty == ety::Str ? str(*n.kids[1]) : val(*n.kids[1]); D.let(t, v, ty, g, false); }
            D.else_();
            { auto v = ty == ety::Int ? intv(*n.kids[2]) : ty == ety::Str ? str(*n.kids[2]) : val(*n.kids[2]); D.let(t, v, ty, g, false); }
            D.end_if();
            if (C.in_fn) locals[t] = ty; else globals[t] = ty;
            return {D.ref(t, ty, g), ty};
        }
        case NK::Call: return call(n, true);
        case NK::Nil: cant(n, "nullptr/nil");
        case NK::Float: cant(n, "floating point (shell arithmetic is integer-only)");
        case NK::List: cant(n, "a list value outside a range-for");
        case NK::Map: cant(n, "maps");
        case NK::Lambda: cant(n, "lambdas");
        case NK::Index: cant(n, "indexing");
        case NK::Shell: cant(n, "shell blocks (use forge::sh / forge::shell)");
        default: cant(n, std::string("expression kind ") + nk_name[int(n.k)]);
        }
    }
    void flatten_concat(const Node& n, std::vector<std::string>& parts) {
        if (n.k == NK::Binary && op_of(n.s) == Op::Add && (type_of(*n.kids[0]) == ety::Str || type_of(*n.kids[1]) == ety::Str)) {
            flatten_concat(*n.kids[0], parts); flatten_concat(*n.kids[1], parts); return;
        }
        auto t = type_of(n);
        if (t == ety::Int) cant(n, "string + integer (use std::to_string)");
        parts.push_back(str(n));
    }

    X call(const Node& n, bool want) {
        std::string c = norm(callee(n));
        auto args = [&](std::size_t from) { std::vector<const Node*> v; for (std::size_t i = from; i < n.kids.size(); ++i) v.push_back(n.kids[i].get()); return v; };
        auto nargs = n.kids.size() - 1;
        if (c == "std::println" || c == "std::print") {
            std::size_t first = 1; bool err = false;
            if (nargs >= 1 && n.kids[1]->k == NK::Var && (n.kids[1]->s == "stderr" || n.kids[1]->s == "std::cerr")) { first = 2; err = true; }
            std::string v = n.kids.size() > first ? format(n, first) : D.s_lit("");
            D.print(v, c == "std::println", err); return {"", ety::Void};
        }
        if (c == "std::format") return {format(n, 1), ety::Str};
        if (c == "std::to_string") return {str(*n.kids[1]), ety::Str};
        if (c == "std::stoi" || c == "std::stol" || c == "std::stoll") return {D.s2i(str(*n.kids[1])), ety::Int};
        if (c == "os.env") {
            auto k = str(*n.kids[1]);
            return {D.env(k, nargs > 1 ? str(*n.kids[2]) : D.s_lit("")), ety::Str};
        }
        if (c == "os.setenv") { D.setenv(str(*n.kids[1]), str(*n.kids[2])); return {"", ety::Void}; }
        if (c == "os.cwd") return {D.cwd(), ety::Str};
        if (c == "os.exit") { D.exit_(nargs ? intv(*n.kids[1]) : D.i_lit(0)); return {"", ety::Void}; }
        if (c == "fs.exists") return {D.test_path(str(*n.kids[1]), 'e'), ety::Cond};
        if (c == "fs.is_dir") return {D.test_path(str(*n.kids[1]), 'd'), ety::Cond};
        if (c == "fs.is_file") return {D.test_path(str(*n.kids[1]), 'f'), ety::Cond};
        if (c == "fs.mkdir") { D.mkdir(str(*n.kids[1])); return {"", ety::Void}; }
        if (c == "fs.remove") { D.remove(str(*n.kids[1])); return {"", ety::Void}; }
        if (c == "fs.write" || c == "fs.append") { auto p = str(*n.kids[1]); D.write(p, str(*n.kids[2]), c == "fs.append"); return {"", ety::Void}; }
        if (c == "fs.read") return {D.read(str(*n.kids[1])), ety::Str};
        if (n.kids[0]->k == NK::Field && (n.kids[0]->s == "size" || n.kids[0]->s == "length") && nargs == 0) {
            auto t = type_of(*n.kids[0]->kids[0]); if (t != ety::Str) cant(n, ".size() on a non-string");
            return {D.slen(str(*n.kids[0]->kids[0])), ety::Int};
        }
        if (n.kids[0]->k == NK::Field && n.kids[0]->s == "empty" && nargs == 0) {
            auto t = type_of(*n.kids[0]->kids[0]); if (t != ety::Str) cant(n, ".empty() on a non-string");
            return {D.cmp("==", str(*n.kids[0]->kids[0]), D.s_lit(""), false), ety::Cond};
        }
        if (c == "forge::sh" || c == "forge::shell") {
            if (want) cant(n, std::string(c) + "(...) inside an expression (assign it to a variable first: auto r = " + c + "(...);)");
            fail(n, "internal: run handled by let");
        }
        if (auto it = fns.find(c); it != fns.end()) {
            if (nargs != it->second.params.size()) fail(n, "wrong number of arguments to " + c);
            std::vector<std::string> a;
            for (std::size_t i = 0; i < nargs; ++i) a.push_back(it->second.params[i] == ety::Int ? intv(*n.kids[i + 1]) : str(*n.kids[i + 1]));
            auto r = it->second.ret;
            bool wv = want && r != ety::Void && r != ety::Unknown;
            auto v = D.call(mangle(c), a, wv, r);
            return {wv ? v : "", wv ? r : ety::Void};
        }
        (void)args;
        if (c.empty()) cant(n, "calling a computed function");
        cant(n, "'" + c + "' (not in the portable surface)");
    }

    // ---- statements
    void block(const Node& n) { if (n.k == NK::Block) for (auto& k : n.kids) stmt(*k); else stmt(n); }
    void declare(const std::string& m, ety t) { if (C.in_fn) locals[m] = t; else globals[m] = t; }
    bool known(const std::string& m) { return C.in_fn ? locals.count(m) > 0 : globals.count(m) > 0; }

    void run_into(const Node& call, const std::string& m) {
        std::string c = norm(callee(call));
        bool host = c == "forge::shell";
        std::string cmd = host ? str(*call.kids[1]) : sh_template(call);
        bool g = !C.in_fn;
        bool decl = !known(m + "_out");
        declare(m, ety::Res); declare(m + "_out", ety::Str); declare(m + "_code", ety::Int);
        if (decl) { D.let(m + "_out", D.s_lit(""), ety::Str, g, true); D.let(m + "_code", D.i_lit(0), ety::Int, g, true); }
        D.run(cmd, m + "_out", m + "_code", host);
    }
    void stream(const Node& n) {   // std::cout << a << b << std::endl
        std::vector<const Node*> items; const Node* p = &n;
        while (p->k == NK::Binary && p->s == "<<") { items.push_back(p->kids[1].get()); p = p->kids[0].get(); }
        bool err = p->k == NK::Var && (p->s == "std::cerr" || p->s == "std::clog");
        if (!(p->k == NK::Var && (p->s == "std::cout" || err))) cant(n, "operator <<");
        std::vector<std::string> parts; std::string lit;
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            const Node& x = **it;
            if (x.k == NK::Var && x.s == "std::endl") { lit += "\n"; continue; }
            if (x.k == NK::Var && (x.s == "std::boolalpha" || x.s == "std::flush")) continue;
            if (x.k == NK::Str) { lit += x.s; continue; }
            if (!lit.empty()) { parts.push_back(D.s_lit(lit)); lit.clear(); }
            parts.push_back(str(x));
        }
        // print without newline unless the stream ends with one we can peel off
        bool nl = !lit.empty() && lit.back() == '\n';
        if (nl) lit.pop_back();
        if (!lit.empty()) parts.push_back(D.s_lit(lit));
        if (parts.empty()) { if (nl) D.print(D.s_lit(""), true, err); return; }
        D.print(parts.size() == 1 ? parts[0] : D.concat(parts), nl, err);
    }
    void assign(const Node& n) {
        const Node& lhs = *n.kids[0];
        if (lhs.k != NK::Var) cant(n, "assigning to a field or index");
        auto m = mangle(lhs.s); auto t = var_type(lhs);
        if (t == ety::Unknown) fail(n, "assignment to unknown variable " + lhs.s);
        bool g = is_global(m);
        if (n.s == "=") {
            const Node& r = *n.kids[1];
            if (r.k == NK::Call && (norm(callee(r)) == "forge::sh" || norm(callee(r)) == "forge::shell")) { run_into(r, m); return; }
            D.let(m, t == ety::Int ? intv(r) : t == ety::Str ? str(r) : val(r), t, g, false); return;
        }
        Op o = op_of(n.s);
        if (t == ety::Str && o == Op::Add) { auto a = D.ref(m, t, g); auto b = str(*n.kids[1]); D.let(m, D.concat({a, b}), t, g, false); return; }
        if (t != ety::Int) cant(n, "compound assignment " + n.s + " on a non-integer");
        if (o != Op::Add && o != Op::Sub && o != Op::Mul && o != Op::Div && o != Op::Mod) cant(n, "operator " + n.s);
        auto b = intv(*n.kids[1]);
        D.let(m, D.arith(op_name(o)[0], D.ref(m, t, g), b), t, g, false);
    }
    void stmt(const Node& n) {
        at(n);
        switch (n.k) {
        case NK::Block: for (auto& k : n.kids) stmt(*k); return;
        case NK::Let: {
            auto m = mangle(n.s);
            if (n.kids.empty()) {
                auto t = ann_type(n.type.get()); if (t == ety::Unknown) cant(n, "a declaration without an initialiser of type " + (n.type ? n.type->str() : std::string("auto")));
                declare(m, t); D.let(m, t == ety::Int ? D.i_lit(0) : D.s_lit(t == ety::Bool ? "false" : ""), t, !C.in_fn, true); return;
            }
            const Node& r = *n.kids[0];
            if (r.k == NK::Call && (norm(callee(r)) == "forge::sh" || norm(callee(r)) == "forge::shell")) { run_into(r, m); return; }
            auto t = ann_type(n.type.get());
            if (n.type && t == ety::Unknown && n.type->name != "auto") cant(n, "variables of type " + n.type->str());
            if (t == ety::Unknown) t = type_of(r);
            if (t == ety::Cond) t = ety::Bool;
            if (t == ety::Unknown || t == ety::Void || t == ety::Res) cant(n, "inferring the type of '" + n.s + "'");
            std::string v = t == ety::Int ? intv(r) : t == ety::Str ? str(r) : val(r);
            bool decl = !known(m);
            declare(m, t);
            D.let(m, v, t, !C.in_fn, decl);
            return;
        }
        case NK::Assign: assign(n); return;
        case NK::ExprStmt: {
            const Node& e = *n.kids[0];
            if (e.k == NK::Binary && e.s == "<<") { stream(e); return; }
            if (e.k == NK::Call) {
                auto c = norm(callee(e));
                if (c == "forge::sh" || c == "forge::shell") { run_into(e, C.tmp()); return; }
                call(e, false); return;
            }
            if (e.k == NK::Assign) { assign(e); return; }
            cant(e, "an expression statement of kind " + std::string(nk_name[int(e.k)]));
        }
        case NK::If: {
            auto c = cond(*n.kids[0]);
            D.if_(c); block(*n.kids[1]);
            if (n.kids.size() > 2 && n.kids[2]) { D.else_(); block(*n.kids[2]); }
            D.end_if(); return;
        }
        case NK::While: {
            // conditions that need statements are re-evaluated each iteration: while true { pre; if !c break; ... }
            auto mark = C.lines.size(); auto c = cond(*n.kids[0]);
            if (C.lines.size() != mark || n.s == "do") {
                std::vector<std::string> pre(C.lines.begin() + mark, C.lines.end()); C.lines.resize(mark);
                if (n.s == "do") fail(n, "internal: do-while");
                D.while_(D.c_bool(true));
                for (auto& l : pre) C.lines.push_back(C.ind + l);
                D.if_(D.c_not(c)); D.brk(); D.end_if();
            } else D.while_(c);
            loop_steps.push_back(n.extra.get());
            block(*n.kids[1]);
            if (n.extra) block(*n.extra);
            loop_steps.pop_back();
            D.end_while(); return;
        }
        case NK::For: {
            const Node& src = *n.kids[0];
            if (src.k != NK::List) cant(n, "range-for over anything but a braced list of literals");
            std::vector<std::string> items; ety t = ety::Unknown;
            for (auto& k : src.kids) {
                auto kt = type_of(*k);
                if (k->k != NK::Str && k->k != NK::Int) cant(*k, "range-for items that aren't literals");
                if (t != ety::Unknown && kt != t) cant(*k, "a list mixing strings and integers");
                t = kt; items.push_back(k->k == NK::Str ? D.s_lit(k->s) : D.i_lit(k->i));
            }
            auto m = mangle(n.var.name); declare(m, t == ety::Unknown ? ety::Str : t);
            D.for_(m, items);
            loop_steps.push_back(nullptr);
            block(*n.kids[1]);
            loop_steps.pop_back();
            D.end_for(); return;
        }
        case NK::Break: D.brk(); return;
        case NK::Continue: { if (!loop_steps.empty() && loop_steps.back()) block(*loop_steps.back()); D.cont(); return; }
        case NK::Return: {
            if (inline_main) { D.exit_(n.kids.empty() ? D.i_lit(0) : intv(*n.kids[0])); return; }
            if (!C.in_fn) cant(n, "return at top level");
            if (n.kids.empty()) { D.ret(""); return; }
            auto r = cur_fn ? cur_fn->ret : ety::Unknown;
            D.ret(r == ety::Int ? intv(*n.kids[0]) : r == ety::Str ? str(*n.kids[0]) : val(*n.kids[0])); return;
        }
        case NK::FnStmt: fail(n, "functions must be defined at top level");
        case NK::Using: case NK::Import: return;
        case NK::Try: cant(n, "try/catch");
        case NK::Throw: cant(n, "throw");
        case NK::ClassStmt: cant(n, "classes and structs");
        case NK::Defer: cant(n, "defer");
        default: cant(n, std::string("statement kind ") + nk_name[int(n.k)]);
        }
    }
    bool inline_main = false, main_inlined = false;
    void function(const Node& n) {
        auto& f = *n.fn; at(n);
        if (!D.has_functions) {
            if (f.name != "main" || !f.params.empty()) cant(n, "functions (" + C.target + " has none; only int main() is inlined)");
            inline_main = true; block(*f.body); inline_main = false; main_inlined = true; return;
        }
        auto m = mangle(f.name);
        fninfo fi; fi.name = m;
        std::map<std::string, ety> env;
        std::vector<std::string> pn;
        for (auto& p : f.params) {
            ety t = p.conv == 'i' ? ety::Int : ann_type(p.type.get());
            if (p.conv == 'f') cant(n, "double parameters");
            if (t == ety::Unknown) t = ety::Str;
            if (p.by_ref && p.out) cant(n, "non-const reference parameters");
            if (p.dflt) cant(n, "default arguments");
            fi.params.push_back(t); env[mangle(p.name)] = t; pn.push_back(mangle(p.name));
        }
        collect_returns(f.body.get(), env, fi.ret);
        if (fi.ret == ety::Unknown) {    // a recursive return expression: retry with the function known as Int
            fns[f.name] = fi; fns[f.name].ret = ety::Int; ety r = ety::Unknown; auto env2 = env;
            collect_returns(f.body.get(), env2, r);
            fi.ret = r == ety::Unknown ? ety::Void : r;
        }
        if (fi.ret == ety::Cond) fi.ret = ety::Bool;
        fns[f.name] = fi;
        C.in_fn = true; locals.clear(); cur_fn = &fns[f.name];
        for (std::size_t i = 0; i < pn.size(); ++i) locals[pn[i]] = fi.params[i];
        D.fn_(m, pn);
        block(*f.body);
        D.end_fn();
        C.in_fn = false; locals.clear(); cur_fn = nullptr;
    }
    void program(const FnDecl& top) {
        const Node& body = *top.body;
        for (auto& k : body.kids) { if (k->k == NK::FnStmt) function(*k); else stmt(*k); }   // source order (lexically scoped shells need it)
        if (top.has_main && main_inlined) return;
        if (top.has_main) {
            auto it = fns.find("main"); if (it == fns.end()) fail(body, "has main but no main()");
            C.line = 0;
            if (it->second.ret == ety::Int) { auto v = D.call("main", {}, true, ety::Int); D.exit_(v); }
            else D.call("main", {}, false, ety::Void);
        }
    }
};
} // namespace

std::string host_shell() {
    if (const char* e = std::getenv("FORGE_SHELL"); e && *e) return e;
#if defined(_WIN32)
    for (const char* s : {"pwsh", "powershell"}) if (auto* b = shells().find(s); b && !shell_exe(*b).empty()) return s;
    return "cmd";
#elif defined(__ANDROID__)
    // /system/bin/sh is mksh, but there is usually no binary called mksh.
    return os::find_executable("mksh").empty() ? "sh" : "mksh";
#else
    return "sh";
#endif
}

std::string shell_exe(const shell_backend& b) {
#if defined(_WIN32)
    std::string path = os::getenv("PATH");
    for (auto& e : b.exes) {
        if (e.find_first_of("/\\") != std::string::npos) { if (auto p = os::find_executable(e); !p.empty()) return p; continue; }
        std::size_t i = 0;
        while (i <= path.size()) {
            auto j = path.find(';', i); if (j == std::string::npos) j = path.size();
            std::string d = path.substr(i, j - i); i = j + 1;
            if (d.empty()) continue;
            auto p = os::find_executable(d + "\\" + e);
            if (p.empty()) continue;
            std::string low = p; for (auto& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (e == "bash" && (low.find("\\system32\\") != std::string::npos || low.find("\\microsoft\\windowsapps\\") != std::string::npos)) continue;   // WSL relays
            if (e == "rc" && low.find("\\windows kits\\") != std::string::npos) continue;   // the Resource Compiler, not Plan 9 rc
            return p;
        }
    }
    return {};
#else
    for (auto& e : b.exes) {
        auto p = os::find_executable(e);
        if (p.empty()) continue;
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".exe") == 0) continue;   // WSL interop (cmd.exe): no Linux cwd
        return e;
    }
    return {};
#endif
}

std::vector<std::string> emit_targets() {
    std::vector<std::string> v;
    for (auto& b : shells().all()) if (b.emitter) v.push_back(b.name);
    return v;
}

std::string emit(std::string_view src, const std::string& file, const std::string& target_in) {
    std::shared_ptr<const FnDecl> top;
    try { top = parse_program(src, file); }
    catch (const syntax_error& e) { throw emit_error(e.line, std::string("syntax error: ") + e.what()); }
    if (target_in == "ast") { std::string o = top->has_main ? "has_main\n" : ""; dump(top->body.get(), 0, o); return o; }
    std::string target = target_in == "auto" ? host_shell() : target_in;
    if (target == "android") target = "mksh";
    const shell_backend* be = shells().find(target);
    if (!be) throw emit_error(0, "unknown emit target '" + target + "' (see forge script --emit-targets)");
    if (!be->emitter) throw emit_error(0, be->name + " has no emitter yet (see forge script --emit-targets)");
    emit_ctx C; C.target = be->name; C.exe = be->exes.empty() ? be->name : be->exes.front();
    auto D = be->emitter(); D->cx = &C;
    Emitter E{*D, C};
    E.program(*top);
    std::string o = D->header();
    for (auto& l : C.lines) { o += l; o += "\n"; }
    o += D->footer();
    return o;
}

} // namespace forge::script
