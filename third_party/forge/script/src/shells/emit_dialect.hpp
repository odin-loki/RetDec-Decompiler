#pragma once
// Emitter dialect interface ("write once, emit anywhere", docs/scripting.md).
// The core (script/src/emit.cpp) walks the parsed script, infers int/string/bool types, hoists
// anything that needs statements, and calls these hooks; a dialect only spells each construct in its
// shell. A shell backend opts in by setting shell_backend::emitter (see posix.cpp, fish.cpp, ...).
// A hook that the shell can't express calls no(...), which becomes a clear emit_error at the
// offending script line.
#include <forge/script/script.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace forge::script {

enum class ety { Int, Str, Bool, Cond, Res, Void, Unknown };

struct emit_ctx {
    std::vector<std::string> lines;
    int indent = 0, line = 0, tmp_n = 0;
    std::string target;                 // backend name (bash, pwsh, ...)
    std::string exe;                    // its first executable name
    bool in_fn = false;
    std::string ind = "    ";
    void put(const std::string& s) { std::string p; for (int i = 0; i < indent; ++i) p += ind; lines.push_back(p + s); }
    std::string tmp() { return "_t" + std::to_string(++tmp_n); }
};

struct emit_dialect {
    emit_ctx* cx = nullptr;
    bool has_functions = true;    // false: only `int main()` is allowed and is inlined at top level
    virtual ~emit_dialect() = default;
    void L(const std::string& s) { cx->put(s); }
    void in() { ++cx->indent; }
    void out() { --cx->indent; }
    [[noreturn]] void no(const std::string& what) { throw emit_error(cx->line, what + " can't be expressed in " + cx->target); }

    virtual std::string header() = 0;           // shebang + helpers
    virtual std::string footer() { return ""; }
    virtual std::string ident(const std::string& n) { return n; }   // after core-level mangling

    // ---- values (fragments that evaluate to a scalar in this shell)
    virtual std::string s_lit(const std::string& s) = 0;
    virtual std::string i_lit(long long v) { return std::to_string(v); }
    virtual std::string ref(const std::string& n, ety t, bool global) = 0;
    virtual std::string arith(char op, const std::string& a, const std::string& b) = 0;   // + - * / % (C semantics, truncating)
    virtual std::string neg(const std::string& a) { return arith('-', i_lit(0), a); }
    virtual std::string concat(const std::vector<std::string>& parts) = 0;
    virtual std::string i2s(const std::string& i) = 0;
    virtual std::string s2i(const std::string& s) = 0;
    virtual std::string slen(const std::string& s) = 0;

    // ---- conditions
    virtual std::string cmp(const std::string& op, const std::string& a, const std::string& b, bool ints) = 0;
    virtual std::string c_and(const std::string& a, const std::string& b) = 0;   // "" = core lowers to nested ifs
    virtual std::string c_or(const std::string& a, const std::string& b) = 0;
    virtual std::string c_not(const std::string& a) = 0;
    virtual std::string c_bool(bool v) = 0;
    virtual std::string truthy(const std::string& v) { return cmp("==", v, s_lit("true"), false); }

    // ---- statements
    virtual void let(const std::string& n, const std::string& v, ety t, bool global, bool decl) = 0;
    virtual void print(const std::string& v, bool nl, bool err) = 0;
    virtual void if_(const std::string& c) = 0;
    virtual void else_() = 0;
    virtual void end_if() = 0;
    virtual void while_(const std::string& c) = 0;
    virtual void end_while() = 0;
    virtual void for_(const std::string& var, const std::vector<std::string>& items) = 0;
    virtual void end_for() = 0;
    virtual void brk() = 0;
    virtual void cont() = 0;
    virtual void fn_(const std::string& name, const std::vector<std::string>& params) = 0;
    virtual void end_fn() = 0;
    virtual void ret(const std::string& v) = 0;                // v empty = void return
    // A call; returns a value fragment (may emit the call as statements first and return a temp).
    virtual std::string call(const std::string& name, const std::vector<std::string>& args, bool want, ety ret) = 0;
    virtual void exit_(const std::string& code) = 0;

    // ---- portable OS surface
    virtual std::string env(const std::string& n, const std::string& dflt) = 0;
    virtual void setenv(const std::string& n, const std::string& v) = 0;
    virtual std::string test_path(const std::string& p, char kind) = 0;   // 'e' exists, 'd' dir, 'f' file -> condition
    virtual void mkdir(const std::string& p) = 0;
    virtual void remove(const std::string& p) = 0;
    virtual void write(const std::string& p, const std::string& s, bool append) = 0;
    virtual std::string read(const std::string& p) = 0;
    virtual std::string cwd() = 0;
    virtual std::string platform() = 0;
    // Run `cmd` (a string) and capture stdout exactly plus the exit code into out_var / code_var.
    // host=false: forge::sh, i.e. POSIX sh. host=true: forge::shell, i.e. this script's own shell.
    virtual void run(const std::string& cmd, const std::string& out_var, const std::string& code_var, bool host) = 0;
    // Quote a string value for POSIX sh at run time (forge::sh "{}" arguments).
    virtual std::string sh_quote(const std::string& v) = 0;
};

// Helper base: indentation plus a no-op for empty bodies (shells that reject `if x; then fi`).
struct block_dialect : emit_dialect {
    std::vector<std::size_t> opens;
    std::string noop;                                 // "" = empty bodies are fine
    void open(const std::string& s) { L(s); opens.push_back(cx->lines.size()); in(); }
    void close(const std::string& s) { if (!noop.empty() && cx->lines.size() == opens.back()) L(noop); opens.pop_back(); out(); L(s); }
    void mid(const std::string& s) { if (!noop.empty() && cx->lines.size() == opens.back()) L(noop); out(); L(s); in(); opens.back() = cx->lines.size(); }
};
inline bool lit_name(const std::string& w, char q, std::string& out) {   // 'NAME' / "NAME" literal -> NAME
    if (w.size() < 3 || w.front() != q || w.back() != q) return false;
    out = w.substr(1, w.size() - 2);
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) return false;
    for (char c : out) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// Shared helper: the POSIX wrapper every dialect uses to run forge::sh commands. It runs the command
// through `sh -c` and appends "\x1c<code>", so stdout is captured byte-exactly (no newline trimming)
// and the exit code travels with it.
inline const char* sh_wrapper() { return "(eval \"$1\"); printf '\\034%d' \"$?\""; }

} // namespace forge::script
