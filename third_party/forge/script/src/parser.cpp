// forge::script front end: lexer, recursive-descent parser and name resolver for the FORGE script
// language, an interpreted, memory-safe subset of ISO C++23 (see docs/scripting.md).
//
// The parser lowers C++ constructs onto the runtime's small AST (ast.hpp):
//   - declarations `T x = e;` become Let nodes with a runtime type check / conversion,
//   - copy-initialisation from an lvalue is wrapped in a Copy node (value semantics),
//   - std::move(x) becomes a Move node (use-after-move raises BorrowError),
//   - class members referenced without `this->` are resolved to fields / methods of `this`,
//   - qualified names (std::println, forge::sh, Color::Red) are single identifiers looked up at run time.
// Raw pointers, new/delete, pointer arithmetic and unsafe casts are rejected with a SyntaxError.
#include "parser.hpp"
#include <cstdlib>
#include <cctype>
#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace forge::script {
namespace {

enum class T { Ident, Int, Float, Str, Op, Kw, End };
struct Tok { T t; std::string s; int line = 0; int col = 0; std::int64_t i = 0; double d = 0; std::size_t off = 0; std::size_t end = 0; };   // off/end: byte span in the source (0/0 for synthesised tokens)

const std::unordered_set<std::string> kKeywords = {
    "auto", "const", "constexpr", "consteval", "constinit", "static", "inline", "virtual", "override", "final",
    "explicit", "mutable", "noexcept", "return", "if", "else", "while", "for", "do", "break", "continue", "true",
    "false", "nullptr", "try", "catch", "throw", "class", "struct", "enum", "public", "private", "protected",
    "this", "template", "typename", "using", "namespace", "import", "void", "operator", "friend", "default",
    "delete", "new", "switch", "case", "static_cast", "const_cast", "reinterpret_cast", "dynamic_cast", "sizeof",
    "goto", "volatile", "extern", "register", "union", "decltype", "static_assert", "co_await", "co_return",
    "co_yield", "asm", "thread_local", "typedef", "concept", "requires", "export", "alignas", "alignof"};

class Lexer {
public:
    Lexer(std::string_view s, std::string file) : s_(s), file_(std::move(file)) {}
    std::vector<Tok> run() {
        std::vector<Tok> out; bool bol = true;
        for (;;) {
            for (;;) {   // whitespace, comments, preprocessor lines
                if (p_ >= s_.size()) break;
                char c = s_[p_];
                if (c == '\n') { ++line_; ++p_; bol = true; continue; }
                if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++p_; continue; }
                if (c == '/' && peek(1) == '/') { while (p_ < s_.size() && s_[p_] != '\n') ++p_; continue; }
                if (c == '/' && peek(1) == '*') {
                    int l0 = line_; p_ += 2;
                    while (p_ < s_.size() && !(s_[p_] == '*' && peek(1) == '/')) { if (s_[p_] == '\n') ++line_; ++p_; }
                    if (p_ >= s_.size()) throw syntax_error(l0, where(l0) + "unterminated /* comment", true);
                    p_ += 2; continue;
                }
                if (c == '#' && bol) { preprocessor(out); continue; }
                break;
            }
            if (p_ >= s_.size()) { out.push_back({T::End, "", line_}); return out; }
            bol = false;
            { auto nl = p_ ? s_.rfind('\n', p_ - 1) : std::string_view::npos; int col = int(p_ - (nl == std::string_view::npos ? 0 : nl + 1)) + 1; std::size_t b = p_; out.push_back(next()); out.back().col = col; out.back().off = b; out.back().end = p_; }
        }
    }
private:
    std::string where(int l) const { return file_ + ":" + std::to_string(l) + ": "; }
    char peek(std::size_t k = 0) const { return p_ + k < s_.size() ? s_[p_ + k] : '\0'; }
    // `#include "x.fs"` imports a script module; other directives (#include <...>, #pragma, ...) are ignored.
    void preprocessor(std::vector<Tok>& out) {
        std::size_t b = p_; while (p_ < s_.size() && s_[p_] != '\n') ++p_;
        std::string ln(s_.substr(b, p_ - b));
        std::size_t q = 1; while (q < ln.size() && (ln[q] == ' ' || ln[q] == '\t')) ++q;
        if (ln.compare(q, 7, "include") == 0) {
            auto a = ln.find('"', q);
            if (a != std::string::npos) {
                auto e = ln.find('"', a + 1);
                if (e == std::string::npos) throw syntax_error(line_, where(line_) + "bad #include");
                out.push_back({T::Kw, "import", line_}); out.push_back({T::Str, ln.substr(a + 1, e - a - 1), line_});
                out.push_back({T::Op, "*", line_}); out.push_back({T::Op, ";", line_});   // `*`: splice the names in (C++ #include)
            }
        }
    }
    static bool idstart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
    static bool idchar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
    std::string word() { std::size_t b = p_; while (p_ < s_.size() && idchar(s_[p_])) ++p_; return std::string(s_.substr(b, p_ - b)); }
    std::string escape(int line) {
        char e = s_[p_++];
        switch (e) {
        case 'n': return "\n"; case 't': return "\t"; case 'r': return "\r"; case '0': return std::string(1, '\0');
        case 'a': return "\a"; case 'b': return "\b"; case 'f': return "\f"; case 'v': return "\v";
        case 'x': {
            std::size_t b = p_; while (p_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[p_])) && p_ - b < 2) ++p_;
            if (p_ == b) throw syntax_error(line, where(line) + "bad \\x escape");
            return std::string(1, char(std::stoi(std::string(s_.substr(b, p_ - b)), nullptr, 16)));
        }
        default: return std::string(1, e);
        }
    }
    Tok next() {
        int line = line_; char c = s_[p_];
        if (c == 'R' && peek(1) == '"') { ++p_; return raw_string(line); }
        if (c == 'u' && peek(1) == '8' && peek(2) == 'R' && peek(3) == '"') { p_ += 3; return raw_string(line); }
        if (c == 'u' && peek(1) == '8' && peek(2) == '"') { p_ += 2; return string_lit(line); }
        if ((c == 'L' || c == 'u' || c == 'U') && peek(1) == '"') { ++p_; return string_lit(line); }
        // identifiers; `a::b::c` is one token, and `::name` after a template id (numeric_limits<int>::max) too
        if (idstart(c) || (c == ':' && peek(1) == ':' && idstart(peek(2)))) {
            std::string w;
            bool lead = c == ':';
            if (lead) { p_ += 2; w = "::"; }
            w += word();
            while (peek() == ':' && peek(1) == ':' && idstart(peek(2))) { p_ += 2; w += "::" + word(); }
            if (lead && !prev_close_angle_) w = w.substr(2);   // ::name -> name (global qualifier)
            prev_close_angle_ = false;
            return {kKeywords.count(w) ? T::Kw : T::Ident, w, line};
        }
        prev_close_angle_ = false;
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) return number(line);
        if (c == '"') return string_lit(line);
        if (c == '\'') {
            ++p_; std::string o;
            while (true) {
                if (p_ >= s_.size() || s_[p_] == '\n') throw syntax_error(line, where(line) + "unterminated character literal");
                char ch = s_[p_++];
                if (ch == '\'') break;
                if (ch == '\\') { o += escape(line); continue; }
                o += ch;
            }
            return {T::Str, o, line};
        }
        static const char* ops[] = {"<<=", ">>=", "...", "<=>", "->*", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                                    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "::",
                                    "(", ")", "[", "]", "{", "}", ",", ".", ":", ";", "+", "-", "*", "/", "%", "<", ">", "=",
                                    "!", "?", "&", "|", "^", "~"};
        for (const char* o : ops) {
            std::string_view ov(o);
            if (s_.substr(p_, ov.size()) == ov) { p_ += ov.size(); prev_close_angle_ = ov == ">" || ov == ">>"; return {T::Op, std::string(ov), line}; }
        }
        throw syntax_error(line, where(line) + "unexpected character '" + std::string(1, c) + "'");
    }
    Tok number(int line) {
        std::size_t b = p_; bool flt = false; int base = 10;
        std::string num;
        if (s_[p_] == '0' && (peek(1) == 'x' || peek(1) == 'X')) { base = 16; p_ += 2; }
        else if (s_[p_] == '0' && (peek(1) == 'b' || peek(1) == 'B')) { base = 2; p_ += 2; }
        auto digit = [&](char ch) { return base == 16 ? std::isxdigit(static_cast<unsigned char>(ch)) != 0 : base == 2 ? (ch == '0' || ch == '1') : std::isdigit(static_cast<unsigned char>(ch)) != 0; };
        for (;;) {
            char ch = peek();
            if (digit(ch)) { num += ch; ++p_; continue; }
            if (ch == '\'' && digit(peek(1))) { ++p_; continue; }   // digit separator 1'000'000
            if (base == 10 && ch == '.' && !flt) { flt = true; num += ch; ++p_; continue; }
            if (base == 10 && (ch == 'e' || ch == 'E')) {
                flt = true; num += ch; ++p_; if (peek() == '+' || peek() == '-') { num += peek(); ++p_; }
                while (std::isdigit(static_cast<unsigned char>(peek()))) { num += peek(); ++p_; }
                continue;
            }
            break;
        }
        bool fsuf = false;   // suffixes: u, l, ul, ll, ull, z, f
        while (peek() == 'u' || peek() == 'U' || peek() == 'l' || peek() == 'L' || peek() == 'z' || peek() == 'Z' || ((peek() == 'f' || peek() == 'F') && base == 10)) {
            if (peek() == 'f' || peek() == 'F') fsuf = true;
            ++p_;
        }
        if (idchar(peek())) throw syntax_error(line, where(line) + "bad numeric literal '" + std::string(s_.substr(b, p_ - b + 1)) + "'");
        Tok t{flt || fsuf ? T::Float : T::Int, std::string(s_.substr(b, p_ - b)), line};
        if (t.t == T::Float) {   // strtod, not stod: "1e" / "1e999" are diagnostics or inf, never a stray std::exception
            char* end = nullptr; t.d = std::strtod(num.c_str(), &end);
            if (num.empty() || end != num.c_str() + num.size()) throw syntax_error(line, where(line) + "bad numeric literal '" + t.s + "'");
            return t;
        }
        std::uint64_t u = 0;
        auto r = std::from_chars(num.data(), num.data() + num.size(), u, base);
        if (r.ec != std::errc{} || u > std::uint64_t(INT64_MAX)) throw syntax_error(line, where(line) + "integer literal out of range: " + t.s);
        t.i = std::int64_t(u);
        return t;
    }
    Tok string_lit(int line) {
        ++p_; std::string o;
        while (true) {
            if (p_ >= s_.size() || s_[p_] == '\n') throw syntax_error(line, where(line) + "unterminated string", p_ >= s_.size());
            char ch = s_[p_++];
            if (ch == '"') break;
            if (ch == '\\' && p_ < s_.size()) { o += escape(line); continue; }
            o += ch;
        }
        return {T::Str, o, line};
    }
    Tok raw_string(int line) {   // at '"' of R"delim( ... )delim"
        ++p_; std::size_t b = p_;
        while (p_ < s_.size() && s_[p_] != '(') ++p_;
        if (p_ >= s_.size()) throw syntax_error(line, where(line) + "bad raw string", true);
        std::string close = ")" + std::string(s_.substr(b, p_ - b)) + "\"";
        ++p_;
        auto e = s_.find(close, p_);
        if (e == std::string_view::npos) throw syntax_error(line, where(line) + "unterminated raw string", true);
        std::string o(s_.substr(p_, e - p_));
        for (char ch : o) if (ch == '\n') ++line_;
        p_ = e + close.size();
        return {T::Str, o, line};
    }
    std::string_view s_; std::string file_; std::size_t p_ = 0; int line_ = 1; bool prev_close_angle_ = false;
};

// ---------------------------------------------------------------- types
enum class TK { Auto, Void, Int, Float, Bool, String, List, Map, Set, Pair, Fn, Any, Class, Weak, Task, Channel, Shared, Enum };
struct TI {
    TK k = TK::Auto;
    std::string name;          // class / enum name
    std::vector<TI> args;
    bool sorted = false;       // std::map / std::set
    bool nullable = false;     // std::optional<T>, smart pointers
    int smart = 0;             // 1 shared_ptr, 2 unique_ptr
    bool ref = false, cnst = false, is_static = false;
    std::shared_ptr<TypeAnn> ann() const {
        auto t = std::make_shared<TypeAnn>();
        switch (k) {
        case TK::Auto: case TK::Void: case TK::Any: return nullptr;
        case TK::Int: case TK::Enum: t->name = "int"; break;
        case TK::Float: t->name = "float"; break;
        case TK::Bool: t->name = "bool"; break;
        case TK::String: t->name = "string"; break;
        case TK::List: case TK::Pair: t->name = "list"; break;
        case TK::Map: case TK::Set: t->name = "map"; break;
        case TK::Fn: t->name = "fn"; break;
        case TK::Class: t->name = name; break;
        case TK::Weak: t->name = "weak"; break;
        case TK::Task: t->name = "task"; break;
        case TK::Channel: t->name = "channel"; break;
        case TK::Shared: t->name = "shared"; break;
        }
        t->nullable = nullable;
        return t;
    }
};

struct FnScope {
    std::vector<std::unordered_map<std::string, int>> blocks;
    std::vector<std::vector<int>> block_slots;
    int nslots = 0; bool top = false;
    std::unordered_set<std::string> consts;
    std::string cls;            // enclosing class for member functions
    bool is_static = false;     // static member function: no `this`
    std::shared_ptr<TI> ret;    // declared return type (brace-init returns)
    bool limit_stmt = false;    // body of a `limit {...} { }` statement: `return` would only leave the block
};

struct ClassInfo {
    std::shared_ptr<ClassDecl> decl;
    std::string base;
    std::set<std::string> members;     // fields + methods (own), statics included
    std::set<std::string> statics;
    int ctors = 0;
};

class Parser {
public:
    Parser(std::vector<Tok> toks, std::string file, std::shared_ptr<const std::string> src = {}) : t_(std::move(toks)), file_(std::move(file)), src_(std::move(src)) { prescan(); }

    std::shared_ptr<const FnDecl> program() {
        auto fd = std::make_shared<FnDecl>(); fd->name = "<main>"; fd->is_top = true; fd->file = file_; fd->line = 1;
        fns_.push_back({}); fns_.back().top = true; fns_.back().blocks.emplace_back(); fns_.back().block_slots.emplace_back();
        auto body = mk(NK::Block, 1);
        while (!at(T::End)) { if (auto s = statement()) body->kids.push_back(s); flush_extra(*body); }
        fd->body = body; fd->nslots = fns_.back().nslots; fns_.pop_back();
        fd->has_main = has_main_;
        mark_ref_calls(*fd);
        return fd;
    }
    // Calls that may bind an lvalue argument to a non-const `T&` parameter are flagged so the runtime
    // writes the parameter's final value back to the caller's variable/element (C++ reference semantics
    // for values held by value: ints, doubles, strings rebound by assignment, ...).
    static std::string last_seg(const std::string& s) { auto p = s.rfind("::"); return p == std::string::npos ? s : s.substr(p + 2); }
    static bool lvalue(const Node& n) { return n.k == NK::Var || n.k == NK::Field || n.k == NK::Index; }
    template <class F> static void walk_fn(const FnDecl& f, F&& fn) {
        for (auto& p : f.params) if (p.dflt) walk(*p.dflt, fn);
        if (f.body) walk(*f.body, fn);
    }
    template <class F> static void walk(const Node& cn, F&& fn) {
        Node& n = const_cast<Node&>(cn);   // every Node is created non-const by mk(); the tree is still being finalised
        fn(n);
        for (auto& k : n.kids) if (k) walk(*k, fn);
        if (n.extra) walk(*n.extra, fn);
        if (n.fn) walk_fn(*n.fn, fn);
        if (n.cls) { for (auto& [nm, d] : n.cls->fields) if (d) walk(*d, fn); for (auto& m : n.cls->methods) walk_fn(*m, fn); }
    }
    static void mark_ref_calls(FnDecl& top) {
        std::set<std::string> names;
        auto note = [&](const FnDecl& f) { for (auto& p : f.params) if (p.out) { const_cast<FnDecl&>(f).has_out = true; names.insert(last_seg(f.name)); break; } };
        note(top);
        walk_fn(top, [&](Node& n) {
            if (n.fn) note(*n.fn);
            if (n.fn && n.fn->has_out && (n.k == NK::FnStmt)) names.insert(last_seg(n.var.name.empty() ? n.fn->name : n.var.name));
            if (n.k == NK::Let && !n.kids.empty() && n.kids[0]->k == NK::Lambda && n.kids[0]->fn) { note(*n.kids[0]->fn); if (n.kids[0]->fn->has_out) names.insert(n.s); }
            if (n.cls) for (auto& m : n.cls->methods) { note(*m); if (m->has_out) { auto nm = m->name; if (auto p = nm.find('/'); p != std::string::npos) nm = nm.substr(0, p); names.insert(last_seg(nm)); } }
        });
        if (names.empty()) return;
        walk_fn(top, [&](Node& n) {
            if (n.k != NK::Call || n.kids.empty()) return;
            const Node& c = *n.kids[0];
            std::string nm = c.k == NK::Var ? last_seg(c.s) : c.k == NK::Field ? c.s : std::string();
            if (nm.empty() || !names.count(nm)) return;
            for (std::size_t i = 1; i < n.kids.size(); ++i) if (lvalue(*n.kids[i])) { n.wb = true; return; }
        });
    }
private:
    // ---- token helpers
    const Tok& cur() const { return t_[std::min(i_, t_.size() - 1)]; }
    const Tok& ahead(std::size_t k) const { return t_[std::min(i_ + k, t_.size() - 1)]; }
    bool at(T t) const { return cur().t == t; }
    bool is_op(const char* o) const { return cur().t == T::Op && cur().s == o; }
    bool is_kw(const char* k) const { return cur().t == T::Kw && cur().s == k; }
    bool ahead_op(std::size_t k, const char* o) const { return ahead(k).t == T::Op && ahead(k).s == o; }
    Tok take() { Tok t = cur(); t_line_ = t.line; t_col_ = t.col; if (i_ < t_.size() - 1) ++i_; return t; }
    [[noreturn]] void fail(const std::string& m) const { throw syntax_error(cur().line, file_ + ":" + std::to_string(cur().line) + ": " + m, at(T::End)); }
    std::int64_t get_index(const std::string& n, int line) const {
        std::int64_t v = 0; auto r = std::from_chars(n.data(), n.data() + n.size(), v);
        if (r.ec != std::errc{} || r.ptr != n.data() + n.size() || v < 0) fail_at(line, "std::get index must be a non-negative integer, got '" + n + "'");
        return v;
    }
    [[noreturn]] void fail_at(int line, const std::string& m) const { throw syntax_error(line, file_ + ":" + std::to_string(line) + ": " + m); }
    void expect_op(const char* o) { if (!is_op(o)) fail(std::string("expected '") + o + "', got '" + (at(T::End) ? std::string("end of input") : cur().s) + "'"); take(); }
    bool accept_op(const char* o) { if (is_op(o)) { take(); return true; } return false; }
    bool accept_kw(const char* k) { if (is_kw(k)) { take(); return true; } return false; }
    std::string ident() { if (!at(T::Ident)) fail("expected a name, got '" + cur().s + "'"); return take().s; }
    void semi() { if ((file_ == "<repl>" || file_ == "<type>") && at(T::End)) return; expect_op(";"); }   // the REPL takes `x + 2` without ';' 
    void flush_extra(Node& b) { for (auto& x : extra_) b.kids.push_back(x); extra_.clear(); }
    // `>` closing a template argument list; splits `>>` (vector<vector<int>>)
    void close_angle() {
        if (is_op(">>")) { undo_.emplace_back(i_, t_[i_].s); t_[i_].s = ">"; return; }
        if (is_op(">=")) { undo_.emplace_back(i_, t_[i_].s); t_[i_].s = "="; return; }
        if (is_op(">>=")) { undo_.emplace_back(i_, t_[i_].s); t_[i_].s = ">="; return; }
        expect_op(">");
    }
    void skip_balanced(const char* open, const char* close) {
        expect_op(open); int depth = 1;
        while (depth) { if (at(T::End)) fail(std::string("unbalanced '") + open + "'"); if (is_op(open)) ++depth; else if (is_op(close)) --depth; take(); }
    }
    // [[a, b(1)]] ... : each attribute's text is appended to pending_attrs_ (reflection: forge::reflect::attributes).
    std::vector<std::string> pending_attrs_;
    void skip_attributes() {
        while (is_op("[") && ahead_op(1, "[")) {
            take(); take(); int depth = 0; std::string cur_a;
            auto flush = [&] { if (!cur_a.empty()) pending_attrs_.push_back(cur_a); cur_a.clear(); };
            for (;;) {
                if (at(T::End)) fail("unbalanced '[['");
                if (depth == 0 && is_op("]") && ahead_op(1, "]")) { take(); take(); break; }
                if (depth == 0 && is_op(",")) { take(); flush(); continue; }
                if (is_op("(") || is_op("[")) ++depth; else if (is_op(")") || is_op("]")) --depth;
                const Tok& k = take();
                std::string txt = k.t == T::Str ? "\"" + k.s + "\"" : k.t == T::Int && k.s.empty() ? std::to_string(k.i) : k.s;
                bool glue = cur_a.empty() || txt == "(" || txt == ")" || txt == "," || txt == "::" || cur_a.back() == '(' || (cur_a.size() > 1 && cur_a.ends_with("::"));
                if (!glue) cur_a += ' ';
                cur_a += txt; if (txt == ",") cur_a += ' ';
            }
            flush();
        }
    }
    std::vector<std::string> take_attrs() { return std::exchange(pending_attrs_, {}); }
    static std::string tname(const TI& t) {
        std::string b;
        switch (t.k) {
        case TK::Auto: b = "auto"; break; case TK::Void: b = "void"; break; case TK::Int: b = "int"; break; case TK::Float: b = "double"; break;
        case TK::Bool: b = "bool"; break; case TK::String: b = "std::string"; break; case TK::List: b = "std::vector"; break;
        case TK::Map: b = t.sorted ? "std::map" : "std::unordered_map"; break; case TK::Set: b = t.sorted ? "std::set" : "std::unordered_set"; break;
        case TK::Pair: b = "std::pair"; break; case TK::Fn: b = "std::function"; break; case TK::Any: b = "std::any"; break;
        case TK::Class: case TK::Enum: b = t.name; break; case TK::Weak: b = "std::weak_ptr"; break; case TK::Task: b = "std::future"; break;
        case TK::Channel: b = "forge::channel"; break; case TK::Shared: b = "forge::shared"; break;
        }
        if (!t.args.empty()) { b += "<"; for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) b += ", "; b += tname(t.args[i]); } b += ">"; }
        if (t.nullable && !t.smart) b = "std::optional<" + b + ">";
        if (t.smart == 1) b = "std::shared_ptr<" + b + ">"; else if (t.smart == 2) b = "std::unique_ptr<" + b + ">";
        if (t.cnst) b = "const " + b;
        if (t.ref) b += "&";
        return b;
    }

    static inline thread_local int t_line_ = 0, t_col_ = 0;   // last consumed token: gives nodes a column
    static std::shared_ptr<Node> mk(NK k, int line) { auto n = std::make_shared<Node>(); n->k = k; n->line = line; n->col = line == t_line_ ? t_col_ : 0; return n; }
    std::shared_ptr<Node> var_node(const std::string& name, int line) { auto n = mk(NK::Var, line); n->s = name; n->var = lookup(name); return n; }
    std::shared_ptr<Node> str_node(const std::string& s, int line) { auto n = mk(NK::Str, line); n->s = s; return n; }
    std::shared_ptr<Node> int_node(std::int64_t v, int line) { auto n = mk(NK::Int, line); n->i = v; return n; }
    std::shared_ptr<Node> call_node(NodeP f, std::vector<NodeP> args, int line) { auto c = mk(NK::Call, line); c->kids.push_back(std::move(f)); for (auto& a : args) c->kids.push_back(std::move(a)); return c; }
    std::shared_ptr<Node> global_var(const std::string& name, int line) { auto n = mk(NK::Var, line); n->s = name; n->var.name = name; return n; }
    std::shared_ptr<Node> builtin_call(const std::string& name, std::vector<NodeP> args, int line) { return call_node(global_var(name, line), std::move(args), line); }
    std::shared_ptr<Node> assign_node(NodeP target, NodeP value, int line) { auto a = mk(NK::Assign, line); a->s = "="; a->i = std::int64_t(Op::None); a->kids = {std::move(target), std::move(value)}; return a; }
    NodeP expr_stmt_of(NodeP e, int line) { auto n = mk(NK::ExprStmt, line); n->kids.push_back(std::move(e)); return n; }
    static bool is_lvalue(const NodeP& e) { return e->k == NK::Var || e->k == NK::Field || e->k == NK::Index; }
    NodeP copy_of(NodeP e) {   // copy-initialisation from an lvalue copies value types
        if (!is_lvalue(e)) return e;
        auto c = mk(NK::Copy, e->line); c->kids.push_back(std::move(e)); return c;
    }
    // A compiler-generated local (switch subject, swap temporary, structured binding source).
    VarRef hidden(const std::string& what, int line) {
        if (top_level_global()) { VarRef v; v.name = "<" + what + ":" + std::to_string(line) + ":" + std::to_string(hidden_n_++) + ">"; return v; }
        return VarRef{"<" + what + ">", 0, fns_.back().nslots++};
    }
    NodeP hidden_ref(const VarRef& v, int line) { auto n = mk(NK::Var, line); n->var = v; n->s = v.name; return n; }

    // ---- pre-scan: every class / struct / enum name in the file is a known type
    void prescan() {
        for (std::size_t k = 0; k + 1 < t_.size(); ++k) {
            const Tok& a = t_[k];
            if (a.t == T::Kw && (a.s == "class" || a.s == "struct" || a.s == "enum")) {
                if (k > 0 && t_[k - 1].t == T::Kw && (t_[k - 1].s == "template" || t_[k - 1].s == "typename")) continue;
                if (k > 0 && t_[k - 1].t == T::Op && (t_[k - 1].s == "<" || t_[k - 1].s == ",")) continue;   // template <class T>
                std::size_t j = k + 1;
                if (a.s == "enum" && j < t_.size() && t_[j].t == T::Kw && (t_[j].s == "class" || t_[j].s == "struct")) ++j;
                if (a.s != "enum" && j + 1 < t_.size() && t_[j].t == T::Op && t_[j].s == "[" && t_[j + 1].t == T::Op && t_[j + 1].s == "[") {
                    int d = 0;   // struct [[attr]] Name
                    for (; j < t_.size(); ++j) { if (t_[j].t == T::Op && t_[j].s == "[") ++d; else if (t_[j].t == T::Op && t_[j].s == "]" && --d == 0) { ++j; break; } }
                }
                if (j < t_.size() && t_[j].t == T::Ident) { types_.insert(t_[j].s); if (a.s == "enum") enums_.insert(t_[j].s); }
            }
        }
    }

    // ---- resolution
    bool top_level_global() const { return fns_.back().top && fns_.back().blocks.size() == 1; }
    std::string ns_name(const std::string& n) const { return ns_.empty() ? n : ns_ + "::" + n; }
    VarRef declare(const std::string& name, bool is_const) {
        auto& f = fns_.back();
        if (top_level_global()) {
            VarRef v; v.name = ns_name(name);
            if (!ns_.empty()) ns_members_[ns_].insert(name);
            if (is_const) f.consts.insert(v.name); else f.consts.erase(v.name);
            return v;
        }
        int slot = f.nslots++; f.blocks.back()[name] = slot; f.block_slots.back().push_back(slot);
        if (is_const) f.consts.insert(name + "#" + std::to_string(slot)); else f.consts.erase(name + "#" + std::to_string(slot));
        return VarRef{name, 0, slot};
    }
    VarRef lookup(const std::string& name) const {
        int depth = 0;
        for (auto f = fns_.rbegin(); f != fns_.rend(); ++f, ++depth)
            for (auto b = f->blocks.rbegin(); b != f->blocks.rend(); ++b)
                if (auto it = b->find(name); it != b->end()) return VarRef{name, depth, it->second};
        VarRef v; v.name = name;
        if (auto a = aliases_.find(name); a != aliases_.end()) v.name = a->second;
        for (std::string ns = ns_; !ns.empty();) {   // names declared earlier in an enclosing namespace
            if (auto it = ns_members_.find(ns); it != ns_members_.end() && it->second.count(name)) { v.name = ns + "::" + name; break; }
            auto p = ns.rfind("::"); ns = p == std::string::npos ? "" : ns.substr(0, p);
        }
        return v;
    }
    bool is_local(const std::string& name) const { return !lookup(name).global(); }
    bool is_const_var(const VarRef& v) const {
        if (v.global()) return fns_.front().consts.count(v.name) > 0;
        auto& f = fns_[fns_.size() - 1 - std::size_t(v.depth)];
        return f.consts.count(v.name + "#" + std::to_string(v.slot)) > 0;
    }
    struct BlockGuard {
        Parser& p; std::vector<int>* out; std::size_t depth;
        explicit BlockGuard(Parser& x, std::vector<int>* o = nullptr) : p(x), out(o), depth(x.fns_.size()) { p.fns_.back().blocks.emplace_back(); p.fns_.back().block_slots.emplace_back(); }
        ~BlockGuard() {
            // A syntax error thrown inside a nested function (lambda, local class method) unwinds past its
            // pop_fn(); drop those scopes so this guard pops its own block, not an inner function's.
            while (p.fns_.size() > depth) p.pop_fn();
            if (p.fns_.size() < depth || p.fns_.back().block_slots.empty()) return;
            if (out) *out = p.fns_.back().block_slots.back(); p.fns_.back().blocks.pop_back(); p.fns_.back().block_slots.pop_back();
        }
    };
    void push_fn(const std::string& cls = "", bool is_static = false) {
        fns_.push_back({}); fns_.back().blocks.emplace_back(); fns_.back().block_slots.emplace_back();
        fns_.back().cls = cls; fns_.back().is_static = is_static;
        saved_loops_.push_back({loops_, switches_}); loops_ = 0; switches_ = 0;
    }
    void pop_fn() { fns_.pop_back(); loops_ = saved_loops_.back().first; switches_ = saved_loops_.back().second; saved_loops_.pop_back(); }
    const FnScope* method_scope() const {   // innermost enclosing function that belongs to a class
        for (auto f = fns_.rbegin(); f != fns_.rend(); ++f) if (!f->cls.empty()) return &*f;
        return nullptr;
    }
    bool class_has(const std::string& cls, const std::string& m) const {
        for (std::string c = cls; !c.empty();) {
            auto it = classes_.find(c); if (it == classes_.end()) return false;
            if (it->second.members.count(m)) return true;
            c = it->second.base;
        }
        return false;
    }
    bool class_has_static(const std::string& cls, const std::string& m) const {
        for (std::string c = cls; !c.empty();) {
            auto it = classes_.find(c); if (it == classes_.end()) return false;
            if (it->second.statics.count(m)) return true;
            c = it->second.base;
        }
        return false;
    }
    NodeP self_node(int line) {
        auto v = lookup("self");
        if (v.global()) fail_at(line, "'this' used outside a non-static member function");
        auto n = mk(NK::Var, line); n->var = v; n->s = "this"; return n;
    }

    // ---- types
    static const std::unordered_set<std::string>& builtin_types() {
        static const std::unordered_set<std::string> b = {
            "int", "long", "short", "unsigned", "signed", "char", "bool", "double", "float", "size_t", "ptrdiff_t",
            "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "intptr_t", "uintptr_t",
            "wchar_t", "char8_t", "char16_t", "char32_t", "ssize_t",
            "std::size_t", "std::ptrdiff_t", "std::int8_t", "std::int16_t", "std::int32_t", "std::int64_t", "std::uint8_t",
            "std::uint16_t", "std::uint32_t", "std::uint64_t", "std::intmax_t", "std::uintmax_t",
            "std::string", "std::string_view", "std::vector", "std::array", "std::deque", "std::list", "std::span",
            "std::map", "std::unordered_map", "std::multimap", "std::set", "std::unordered_set", "std::pair", "std::tuple",
            "std::optional", "std::shared_ptr", "std::unique_ptr", "std::weak_ptr", "std::function", "std::any",
            "std::future", "std::thread", "std::jthread", "std::exception", "std::runtime_error", "std::logic_error",
            "std::invalid_argument", "std::out_of_range", "std::overflow_error", "std::bad_alloc", "std::ostream",
            "std::filesystem::path", "std::chrono::milliseconds", "std::chrono::seconds",
            "forge::result", "forge::proc_result", "forge::channel", "forge::task", "forge::shared", "forge::handle",
            "forge::value", "forge::error"};
        return b;
    }
    std::string canon_type_name(std::string w) const {
        if (auto a = aliases_.find(w); a != aliases_.end()) w = a->second;
        if (using_std_ && w.find("::") == std::string::npos && !types_.count(w) && !tparams_.count(w) && !typedefs_.count(w) && builtin_types().count("std::" + w)) w = "std::" + w;
        return w;
    }
    bool is_type_word(const std::string& w0) const {
        std::string w = canon_type_name(w0);
        return builtin_types().count(w) || types_.count(w) || tparams_.count(w) || typedefs_.count(w);
    }
    bool starts_type() const {
        if (at(T::Kw)) { const auto& s = cur().s; return s == "auto" || s == "const" || s == "constexpr" || s == "static" || s == "inline" || s == "void" || s == "volatile" || s == "mutable" || s == "thread_local" || s == "constinit" || s == "typename" || s == "decltype"; }
        return at(T::Ident) && is_type_word(cur().s);
    }
    static std::string strip_std(const std::string& n) { return n.rfind("std::", 0) == 0 ? n.substr(5) : n; }
    static std::string exception_class(const std::string& w) {
        static const std::unordered_map<std::string, std::string> m = {
            {"std::exception", "Error"}, {"std::runtime_error", "RuntimeError"}, {"std::logic_error", "LogicError"},
            {"std::invalid_argument", "ValueError"}, {"std::out_of_range", "IndexError"}, {"std::overflow_error", "OverflowError"},
            {"std::range_error", "ValueError"}, {"std::length_error", "ValueError"}, {"std::domain_error", "ValueError"},
            {"std::bad_alloc", "OutOfMemoryError"}, {"std::bad_optional_access", "ValueError"}, {"std::system_error", "OSError"},
            {"std::filesystem::filesystem_error", "IOError"}, {"forge::error", "Error"}};
        if (auto it = m.find(w); it != m.end()) return it->second;
        if (w.rfind("forge::", 0) == 0) return w.substr(7);
        return w;
    }

    // Parses a declaration type: specifiers, a (possibly templated) type name and trailing & / &&.
    // Raw pointers are rejected: the language is memory safe.
    TI parse_type() {
        TI ti;
        for (;;) {
            if (accept_kw("const") || accept_kw("constexpr") || accept_kw("constinit") || accept_kw("consteval")) { ti.cnst = true; continue; }
            if (accept_kw("static")) { ti.is_static = true; continue; }
            if (accept_kw("inline") || accept_kw("volatile") || accept_kw("mutable") || accept_kw("thread_local") || accept_kw("typename") || accept_kw("extern") || accept_kw("virtual") || accept_kw("explicit")) continue;
            if (is_op("[") && ahead_op(1, "[")) { skip_attributes(); continue; }
            break;
        }
        if (accept_kw("auto")) ti.k = TK::Auto;
        else if (accept_kw("void")) ti.k = TK::Void;
        else if (is_kw("decltype")) { take(); skip_balanced("(", ")"); ti.k = TK::Auto; }
        else {
            if (!at(T::Ident)) fail("expected a type, got '" + cur().s + "'");
            std::string w = canon_type_name(take().s);
            // multi-word builtins: unsigned long long, long double, ...
            static const std::unordered_set<std::string> mw = {"unsigned", "signed", "long", "short"};
            if (mw.count(w)) { while (at(T::Ident) && (mw.count(cur().s) || cur().s == "int" || cur().s == "char" || cur().s == "double")) { if (cur().s == "double") w = "double"; take(); } }
            std::vector<TI> args;
            if (is_op("<")) {
                take();
                while (!is_op(">") && !is_op(">>") && !is_op(">=") && !is_op(">>=")) {
                    if (at(T::Int)) { take(); args.push_back(TI{TK::Int}); }   // std::array<int, 3>
                    else args.push_back(parse_type());
                    if (is_op("(")) skip_balanced("(", ")");   // function type std::function<R(Args...)>: erased
                    if (!accept_op(",")) break;
                }
                close_angle();
            }
            ti.args = args;
            static const std::unordered_map<std::string, TK> km = {
                {"int", TK::Int}, {"long", TK::Int}, {"short", TK::Int}, {"unsigned", TK::Int}, {"signed", TK::Int}, {"size_t", TK::Int},
                {"ptrdiff_t", TK::Int}, {"ssize_t", TK::Int}, {"int8_t", TK::Int}, {"int16_t", TK::Int}, {"int32_t", TK::Int}, {"int64_t", TK::Int},
                {"uint8_t", TK::Int}, {"uint16_t", TK::Int}, {"uint32_t", TK::Int}, {"uint64_t", TK::Int}, {"intptr_t", TK::Int}, {"uintptr_t", TK::Int},
                {"std::size_t", TK::Int}, {"std::ptrdiff_t", TK::Int}, {"std::int8_t", TK::Int}, {"std::int16_t", TK::Int}, {"std::int32_t", TK::Int},
                {"std::int64_t", TK::Int}, {"std::uint8_t", TK::Int}, {"std::uint16_t", TK::Int}, {"std::uint32_t", TK::Int}, {"std::uint64_t", TK::Int},
                {"std::intmax_t", TK::Int}, {"std::uintmax_t", TK::Int},
                {"double", TK::Float}, {"float", TK::Float}, {"bool", TK::Bool},
                {"char", TK::String}, {"wchar_t", TK::String}, {"char8_t", TK::String}, {"char16_t", TK::String}, {"char32_t", TK::String},
                {"std::string", TK::String}, {"std::string_view", TK::String}, {"std::filesystem::path", TK::String},
                {"std::vector", TK::List}, {"std::array", TK::List}, {"std::deque", TK::List}, {"std::list", TK::List}, {"std::span", TK::List},
                {"std::map", TK::Map}, {"std::unordered_map", TK::Map}, {"std::multimap", TK::Map},
                {"std::set", TK::Set}, {"std::unordered_set", TK::Set},
                {"std::pair", TK::Pair}, {"std::tuple", TK::Pair}, {"std::function", TK::Fn}, {"std::any", TK::Any},
                {"std::future", TK::Task}, {"std::thread", TK::Task}, {"std::jthread", TK::Task}, {"forge::task", TK::Task},
                {"forge::channel", TK::Channel}, {"forge::shared", TK::Shared}, {"std::weak_ptr", TK::Weak},
                {"std::chrono::milliseconds", TK::Int}, {"std::chrono::seconds", TK::Int}};
            if (auto it = km.find(w); it != km.end()) ti.k = it->second;
            else if (w == "std::optional") { ti = args.empty() ? TI{TK::Any} : args[0]; ti.nullable = true; }
            else if (w == "std::shared_ptr" || w == "std::unique_ptr") { ti = args.empty() ? TI{TK::Any} : args[0]; ti.nullable = true; ti.smart = w == "std::shared_ptr" ? 1 : 2; }
            else if (auto td = typedefs_.find(w); td != typedefs_.end()) { bool c = ti.cnst, s = ti.is_static; ti = td->second; ti.cnst |= c; ti.is_static |= s; }
            else if (tparams_.count(w)) ti.k = TK::Any;
            else if (enums_.count(w)) { ti.k = TK::Enum; ti.name = w; }
            else if (types_.count(w)) {
                if (exception_class(w) != w) { ti.k = TK::Class; ti.name = exception_class(w); }
                else { ti.k = TK::Class; ti.name = w; }
            }
            else if (exception_class(w) != w) { ti.k = TK::Class; ti.name = exception_class(w); }
            else ti.k = TK::Any;   // forge::result and friends: dynamically checked
            if (ti.k == TK::Fn || ti.k == TK::Any) ti.args.clear();
            if (ti.k == TK::List || ti.k == TK::Map || ti.k == TK::Set) ti.args = args;
            if (w == "std::map" || w == "std::set" || w == "std::multimap") ti.sorted = true;
        }
        while (accept_kw("const") || accept_kw("volatile")) ti.cnst = true;
        if (is_op("*")) fail("raw pointers are not allowed in FORGE scripts; use std::shared_ptr / std::unique_ptr or a reference");
        if (accept_op("&")) ti.ref = true; else if (accept_op("&&")) ti.ref = false;
        while (accept_kw("const")) ti.cnst = true;
        return ti;
    }

    // Default-initialised value of a type (`int x;` is 0 here: no indeterminate values).
    NodeP default_value(const TI& ti, int line) {
        if (ti.nullable) return mk(NK::Nil, line);
        switch (ti.k) {
        case TK::Int: case TK::Enum: return int_node(0, line);
        case TK::Float: { auto n = mk(NK::Float, line); n->d = 0; return n; }
        case TK::Bool: return mk(NK::False, line);
        case TK::String: return str_node("", line);
        case TK::List: case TK::Pair: {
            if (ti.k == TK::Pair) { auto l = mk(NK::List, line); for (auto& a : ti.args) l->kids.push_back(default_value(a, line)); return l; }
            return mk(NK::List, line);
        }
        case TK::Map: case TK::Set: return make_map_node(ti, {}, line);
        case TK::Class: {
            if (!classes_.count(ti.name) && !types_.count(ti.name)) return builtin_call(ti.name, {}, line);
            return call_node(var_node(ti.name, line), {}, line);
        }
        case TK::Channel: return builtin_call("forge::channel", {}, line);
        case TK::Shared: return builtin_call("forge::shared", {ti.args.empty() ? mk(NK::Nil, line) : default_value(ti.args[0], line)}, line);
        default: return mk(NK::Nil, line);
        }
    }
    // forge::__make_map(kind, default, [k, v, k, v...]); kind: 0 unordered map, 1 map, 2 unordered set, 3 set
    NodeP make_map_node(const TI& ti, std::vector<NodeP> pairs, int line) {
        int kind = (ti.k == TK::Set ? 2 : 0) + (ti.sorted ? 1 : 0);
        NodeP dflt = ti.k == TK::Map && ti.args.size() >= 2 ? default_value(ti.args[1], line) : mk(NK::Nil, line);
        auto l = mk(NK::List, line); for (auto& p : pairs) l->kids.push_back(p);
        auto lam = dflt;   // default value expression is evaluated per inserted key: pass a factory
        auto fd = std::make_shared<FnDecl>(); fd->name = "<default>"; fd->line = line; fd->file = file_;
        auto body = mk(NK::Block, line); auto r = mk(NK::Return, line); r->kids.push_back(lam); body->kids.push_back(r);
        fd->body = body; fd->nslots = 0;
        auto f = mk(NK::Lambda, line); f->fn = fd;
        return builtin_call("forge::__make_map", {int_node(kind, line), ti.k == TK::Map && ti.args.size() >= 2 ? NodeP(f) : mk(NK::Nil, line), l}, line);
    }
    // Brace / paren initialisation of a typed declaration.
    NodeP construct(const TI& ti, std::vector<NodeP> args, bool brace, int line) {
        switch (ti.k) {
        case TK::List:
            if (!brace && !args.empty()) {   // vector<T>(n) / vector<T>(n, fill)
                NodeP fill = args.size() > 1 ? args[1] : (ti.args.empty() ? NodeP(mk(NK::Nil, line)) : default_value(ti.args[0], line));
                return builtin_call("std::vector", {args[0], fill}, line);
            }
            {   // vector<Job>{{"a", 1}, ...}: nested braces construct the element type
                auto l = mk(NK::List, line);
                bool typed = !ti.args.empty() && (ti.args[0].k == TK::Class || ti.args[0].k == TK::List || ti.args[0].k == TK::Map || ti.args[0].k == TK::Set);
                for (auto& a : args) {
                    if (typed && brace && a->k == NK::List) { std::vector<NodeP> ks(a->kids.begin(), a->kids.end()); l->kids.push_back(construct(ti.args[0], ks, true, line)); }
                    else l->kids.push_back(a);
                }
                return l;
            }
        case TK::Pair: { auto l = mk(NK::List, line); for (auto& a : args) l->kids.push_back(a); return l; }
        case TK::Map: case TK::Set: {
            std::vector<NodeP> flat;
            for (auto& a : args) {
                if (ti.k == TK::Set) { flat.push_back(a); flat.push_back(mk(NK::True, line)); continue; }
                if (a->k != NK::List || a->kids.size() != 2) fail_at(line, "map initialiser entries must be {key, value}");
                flat.push_back(a->kids[0]); flat.push_back(a->kids[1]);
            }
            return make_map_node(ti, flat, line);
        }
        case TK::String:
            if (args.empty()) return str_node("", line);
            if (args.size() == 1) return copy_of(args[0]);
            return builtin_call("std::string", args, line);   // string(n, ch)
        case TK::Class:
            if (ti.smart) {
                if (args.empty()) return mk(NK::Nil, line);
                return args[0];
            }
            return call_node(classes_.count(ti.name) || types_.count(ti.name) ? var_node(ti.name, line) : global_var(ti.name, line), args, line);
        case TK::Task:
            if (args.empty()) return mk(NK::Nil, line);
            return builtin_call("forge::spawn", args, line);   // std::thread t(f, args...)
        case TK::Channel: return builtin_call("forge::channel", args, line);
        case TK::Shared: {   // forge::shared<T>(init...) / forge::shared<T> s{...}: T is built from the arguments
            NodeP inner = ti.args.empty() || (ti.args[0].smart && args.size() == 1) ? (args.empty() ? mk(NK::Nil, line) : args[0]) : construct(ti.args[0], args, brace, line);
            return builtin_call("forge::shared", {inner}, line);
        }
        case TK::Weak:
            if (args.empty()) return mk(NK::Nil, line);
            return builtin_call("forge::__weak", {args[0]}, line);
        case TK::Int: case TK::Float: case TK::Bool: case TK::Enum:
            if (args.empty()) return default_value(ti, line);
            return builtin_call("forge::__cast", {str_node(ti.k == TK::Float ? "float" : ti.k == TK::Bool ? "bool" : "int", line), args[0]}, line);
        default:
            if (args.empty()) return default_value(ti, line);
            return copy_of(args[0]);
        }
    }
    // `= expr` initialiser: braces become a typed construction, lvalues are copied.
    NodeP initializer(const TI& ti, int line) {
        if (is_op("{")) return construct(ti, brace_list(), true, line);
        NodeP e = expr();
        if (ti.k == TK::Weak) return builtin_call("forge::__weak", {e}, line);
        if (ti.k == TK::Int && e->k == NK::Float) fail_at(line, "narrowing conversion from double to int; use static_cast<int>(...)");
        if (ti.k == TK::Float && e->k == NK::Int) { auto f = mk(NK::Float, line); f->d = double(e->i); return f; }
        if ((ti.k == TK::Int || ti.k == TK::Float) && e->k != NK::Int && e->k != NK::Float)
            return builtin_call("forge::__cast", {str_node(ti.k == TK::Float ? "float" : "int", line), e}, line);
        return copy_of(e);
    }
    std::vector<NodeP> brace_list() {
        expect_op("{"); std::vector<NodeP> v;
        while (!is_op("}")) {
            if (is_op("{")) { auto l = mk(NK::List, cur().line); for (auto& x : brace_list()) l->kids.push_back(x); v.push_back(l); }
            else v.push_back(copy_of(assign_expr()));
            if (!accept_op(",")) break;
        }
        expect_op("}");
        return v;
    }
    std::vector<NodeP> paren_args() {
        expect_op("("); std::vector<NodeP> v;
        while (!is_op(")")) { v.push_back(arg_expr()); if (!accept_op(",")) break; }
        expect_op(")");
        return v;
    }
    NodeP arg_expr() { if (is_op("{")) { int l = cur().line; auto x = mk(NK::List, l); for (auto& k : brace_list()) x->kids.push_back(k); return x; } return assign_expr(); }

    // ---------------------------------------------------------------- statements
    NodeP block_body(int line, bool new_scope = true) {
        auto b = mk(NK::Block, line);
        expect_op("{");
        if (new_scope) {
            BlockGuard g(*this, &b->locals);
            while (!is_op("}")) { if (at(T::End)) fail("expected '}'"); if (auto s = statement()) b->kids.push_back(s); flush_extra(*b); }
        } else {
            while (!is_op("}")) { if (at(T::End)) fail("expected '}'"); if (auto s = statement()) b->kids.push_back(s); flush_extra(*b); }
        }
        expect_op("}");
        return b;
    }
    // A sub-statement (if / loop body): always a Block with its own scope.
    NodeP sub_statement() {
        int line = cur().line;
        if (is_op("{")) return block_body(line);
        auto b = mk(NK::Block, line);
        BlockGuard g(*this, &b->locals);
        if (auto s = statement()) b->kids.push_back(s);
        flush_extra(*b);
        return b;
    }
    // Try to parse a declaration type; restores the position and returns nullopt if this is an expression.
    std::optional<TI> try_decl_type() {
        if (!starts_type()) return std::nullopt;
        std::size_t save = i_, mark = undo_.size();
        try {
            TI ti = parse_type();
            if (at(T::Ident) || (is_op("[") && !ahead_op(1, "[")) || is_kw("operator")) return ti;
        } catch (const syntax_error&) {}
        rollback(save, mark);
        return std::nullopt;
    }
    void rollback(std::size_t pos, std::size_t mark) {
        while (undo_.size() > mark) { t_[undo_.back().first].s = undo_.back().second; undo_.pop_back(); }
        i_ = pos;
    }

    // statement start column: the first token of the statement (Node::col for statement nodes)
    NodeP statement() {
        int l0 = cur().line, c0 = cur().col;
        NodeP n = statement_body();
        if (n && n->line == l0 && n->k != NK::Block) const_cast<Node&>(*n).col = c0;
        return n;
    }
    NodeP statement_body() {
        int line = cur().line;
        pending_attrs_.clear();
        skip_attributes();
        if (accept_op(";")) return nullptr;
        if (is_op("{")) return block_body(line);
        if (at(T::Kw)) {
            const std::string k = cur().s;
            if (k == "if") return if_stmt();
            if (k == "while") return while_stmt();
            if (k == "do") return do_stmt();
            if (k == "for") return for_stmt();
            if (k == "switch") return switch_stmt();
            if (k == "return") return return_stmt();
            if (k == "break") { take(); semi(); if (brk_.empty()) fail_at(line, "break outside a loop or switch"); return mk(NK::Break, line); }
            if (k == "continue") {
                take(); semi();
                bool in_loop = false;
                for (auto it = brk_.rbegin(); it != brk_.rend(); ++it) { if (*it == 's') fail_at(line, "continue inside a switch is not supported; restructure the loop"); in_loop = true; break; }
                if (!in_loop) fail_at(line, "continue outside a loop");
                return mk(NK::Continue, line);
            }
            if (k == "throw") {
                take(); auto n = mk(NK::Throw, line);
                if (accept_op(";")) {
                    if (catch_vars_.empty()) fail_at(line, "'throw;' outside a catch block");
                    n->kids.push_back(hidden_ref(catch_vars_.back(), line)); return n;
                }
                n->kids.push_back(expr()); semi(); return n;
            }
            if (k == "try") return try_stmt();
            if (k == "class" || k == "struct") {
                if (ahead_op(1, "[") && ahead_op(2, "[")) return class_stmt();
                if (ahead(1).t == T::Ident && (ahead_op(2, "{") || ahead_op(2, ":") || ahead_op(2, ";") || (ahead(2).t == T::Kw && ahead(2).s == "final"))) return class_stmt();
            }
            if (k == "enum") return enum_stmt();
            if (k == "namespace") return namespace_stmt();
            if (k == "using") return using_stmt();
            if (k == "typedef") { take(); TI ti = parse_type(); typedefs_[ident()] = ti; semi(); return nullptr; }
            if (k == "template") { take(); template_params(); return statement_templated(); }
            if (k == "import") { take(); return import_stmt(line); }
            if (k == "static_assert") { take(); auto a = paren_args(); semi(); return expr_stmt_of(builtin_call("forge::assert", a, line), line); }
            if (k == "export") { take(); return statement(); }
            if (k == "goto" || k == "asm" || k == "union" || k == "co_await" || k == "co_return" || k == "co_yield")
                fail_at(line, "'" + k + "' is not supported in FORGE scripts");
            if (k == "new" || k == "delete") fail_at(line, "new/delete are not allowed; use std::make_unique / std::make_shared");
        }
        if (limit_block_ahead()) { auto e = limit_block(line, true); accept_op(";"); return expr_stmt_of(std::move(e), line); }
        if (at(T::Ident) && ahead_op(1, "(")) {   // Cls::Cls(...) defined out of line
            const std::string& q = cur().s; auto p = q.rfind("::");
            if (p != std::string::npos && classes_.count(q.substr(0, p)) && short_name(q.substr(0, p)) == q.substr(p + 2)) { std::string qn = take().s; return out_of_line(TI{TK::Void}, qn, line); }
        }
        if (auto ti = try_decl_type()) return declaration(*ti, line);
        auto e = expr();
        semi();
        return expr_stmt(std::move(e), line);
    }
    NodeP statement_templated() { auto s = statement(); tparams_.clear(); return s; }
    void template_params() {
        expect_op("<");
        while (!is_op(">")) {
            if (accept_kw("typename") || accept_kw("class")) { accept_op("..."); if (at(T::Ident)) tparams_.insert(take().s); }
            else if (at(T::Ident) && (cur().s == "auto" || is_type_word(cur().s))) { parse_type(); if (at(T::Ident)) take(); }
            else if (accept_kw("auto")) { if (at(T::Ident)) take(); }
            else if (at(T::Ident)) { take(); accept_op("..."); if (at(T::Ident)) tparams_.insert(take().s); }   // concept-constrained: std::integral T
            else fail("bad template parameter list");
            if (accept_op("=")) { int depth = 0; while (!(depth == 0 && (is_op(",") || is_op(">")))) { if (is_op("<")) ++depth; if (is_op(">")) --depth; take(); } }
            if (!accept_op(",")) break;
        }
        close_angle();
        if (accept_kw("requires")) { while (!is_kw("auto") && !starts_type() && !is_kw("class") && !is_kw("struct")) take(); }
    }
    NodeP expr_stmt(NodeP e, int line) {
        if (e->k == NK::Assign) return e;
        if (e->k == NK::Call && e->kids[0]->k == NK::Var && (e->kids[0]->s == "std::swap" || e->kids[0]->s == "std::ranges::swap") && e->kids.size() == 3) {
            NodeP a = e->kids[1], b = e->kids[2];
            if (!is_lvalue(a) || !is_lvalue(b)) fail_at(line, "std::swap needs two assignable operands");
            VarRef tmp = hidden("swap", line);
            auto blk = mk(NK::Block, line);
            auto let = mk(NK::Let, line); let->var = tmp; let->s = tmp.name; let->kids.push_back(a);
            blk->kids = {let, assign_node(a, b, line), assign_node(b, hidden_ref(tmp, line), line)};
            return blk;
        }   // assignments are statements in the AST
        return expr_stmt_of(std::move(e), line);
    }
    NodeP import_stmt(int line) {
        std::string path;
        if (at(T::Str)) path = take().s;
        else { path = ident(); while (accept_op(".")) path += "/" + ident(); }
        bool splice = accept_op("*");
        std::string alias;
        if (!splice && at(T::Ident) && cur().s == "as") { take(); alias = ident(); }
        semi();
        if (splice) { auto n = mk(NK::Import, line); n->s = path; n->flag = true; return n; }
        std::string name = path;
        if (auto p = name.find_last_of('/'); p != std::string::npos) name = name.substr(p + 1);
        if (auto p = name.find('.'); p != std::string::npos) name = name.substr(0, p);
        if (name.rfind("lib", 0) == 0 && name.size() > 3 && (path.ends_with(".so") || path.ends_with(".dylib") || path.ends_with(".dll"))) name = name.substr(3);
        if (!alias.empty()) name = alias;
        auto n = mk(NK::Import, line); n->s = path; n->var = declare(name, true);
        return n;
    }

    // Declarations: variables (several declarators), structured bindings and function definitions.
    NodeP declaration(const TI& ti, int line) {
        if (is_op("[")) return structured_binding(ti, line);
        if (is_kw("operator")) fail_at(line, "operator overloads must be class members");
        std::string name = ident();
        if (auto p = name.rfind("::"); p != std::string::npos && is_op("(") && classes_.count(name.substr(0, p))) return out_of_line(ti, name, line);
        if (is_op("(") && (fns_.back().top && fns_.back().blocks.size() == 1) && looks_like_function()) return function_def(ti, name, line, "");
        auto out = mk(NK::Block, line);
        std::vector<NodeP> decls;
        for (;;) {
            decls.push_back(declarator(ti, name, line));
            if (!accept_op(",")) break;
            name = ident();
        }
        semi();
        for (std::size_t k = 1; k < decls.size(); ++k) extra_.push_back(decls[k]);
        return decls[0];
    }
    // At `(` after `T name`: a function definition / declaration rather than a direct initialisation?
    bool looks_like_function() const {
        std::size_t k = i_ + 1;
        if (t_[k].t == T::Op && t_[k].s == ")") return true;
        if (t_[k].t == T::Kw && (t_[k].s == "const" || t_[k].s == "auto" || t_[k].s == "void")) return true;
        if (t_[k].t == T::Ident && is_type_word(t_[k].s)) return true;
        return false;
    }
    NodeP declarator(const TI& ti0, const std::string& name, int line) {
        TI ti = ti0;
        while (accept_op("[")) {   // C array: int a[3] = {...}  -> std::array
            NodeP n = is_op("]") ? nullptr : expr(); expect_op("]");
            TI el = ti; ti = TI{TK::List}; ti.args = {el}; ti.cnst = el.cnst;
            if (n && !is_op("=") && !is_op("{")) { auto v = declare(name, ti.cnst); auto let = mk(NK::Let, line); let->s = name; let->var = v;
                let->kids.push_back(builtin_call("std::vector", {n, default_value(el, line)}, line)); return let; }
        }
        NodeP init;
        if (accept_op("=")) init = initializer(ti, line);
        else if (is_op("{")) init = construct(ti, brace_list(), true, line);
        else if (is_op("(")) init = construct(ti, paren_args(), false, line);
        else {
            if (ti.k == TK::Auto) fail_at(line, "'auto " + name + "' needs an initialiser");
            if (ti.ref) fail_at(line, "reference '" + name + "' must be initialised");
            init = default_value(ti, line);
        }
        if (ti.ref && !ti.cnst && init->k == NK::Copy) init = init->kids[0];   // T& r = x: alias (objects share; scalars copy)
        auto let = mk(NK::Let, line);
        let->s = name; let->flag = ti.cnst; let->ref = ti.ref;
        let->kids.push_back(init);
        if (ti.k != TK::Auto && ti.k != TK::Any && ti.k != TK::Fn && ti.k != TK::Class && ti.k != TK::Enum) let->type = ti.ann();
        let->var = declare(name, ti.cnst);   // declared after the initialiser: `int x = x;` refers to an outer x
        return let;
    }
    NodeP structured_binding(const TI& ti, int line) {
        expect_op("["); std::vector<std::string> names;
        while (!is_op("]")) { names.push_back(ident()); if (!accept_op(",")) break; }
        expect_op("]");
        if (names.empty()) fail_at(line, "structured binding needs at least one name");
        NodeP src;
        if (accept_op("=")) src = expr(); else if (is_op("{")) { auto l = brace_list(); if (l.size() != 1) fail("bad structured binding"); src = l[0]; } else fail("structured binding needs an initialiser");
        semi();
        VarRef tmp = hidden("binding", line);
        auto let = mk(NK::Let, line); let->var = tmp; let->s = tmp.name; let->kids.push_back(src);
        for (std::size_t k = 0; k < names.size(); ++k) {
            auto d = mk(NK::Let, line); d->s = names[k]; d->flag = ti.cnst;
            auto g = builtin_call("forge::__get", {hidden_ref(tmp, line), int_node(std::int64_t(k), line)}, line);
            d->kids.push_back(ti.ref ? g : copy_of_call(g));
            d->var = declare(names[k], ti.cnst);
            extra_.push_back(d);
        }
        return let;
    }
    NodeP copy_of_call(NodeP e) { auto c = mk(NK::Copy, e->line); c->kids.push_back(std::move(e)); return c; }

    std::vector<Param> params(std::vector<TI>* types = nullptr) {
        std::vector<Param> ps;
        expect_op("(");
        if (is_kw("void") && ahead_op(1, ")")) take();
        while (!is_op(")")) {
            if (accept_op("...")) { Param p; p.name = "..."; ps.push_back(p); break; }
            TI ti = parse_type();
            Param p;
            accept_op("...");
            p.name = at(T::Ident) ? take().s : "_" + std::to_string(ps.size());
            while (accept_op("[")) { if (!is_op("]")) expr(); expect_op("]"); ti = TI{TK::List}; ti.ref = true; }
            if (!ti.ref && (ti.k == TK::Int || ti.k == TK::Float)) p.conv = ti.k == TK::Int ? 'i' : 'f';
            p.by_ref = ti.ref;   // const T& binds without copying (const-ness is not enforced at run time)
            p.out = ti.ref && !ti.cnst;
            if (ti.k != TK::Auto && ti.k != TK::Any && ti.k != TK::Fn && ti.k != TK::Class && ti.k != TK::Enum && ti.k != TK::Float && ti.k != TK::Int) p.type = ti.ann();
            if (accept_op("=")) p.dflt = is_op("{") ? construct(ti, brace_list(), true, cur().line) : assign_expr();
            p.tname = tname(ti); p.cnst = ti.cnst;
            if (types) types->push_back(ti);
            ps.push_back(std::move(p));
            if (!accept_op(",")) break;
        }
        expect_op(")");
        return ps;
    }
    // After the parameter list: cv / ref qualifiers, noexcept, override, final, trailing return.
    void fn_qualifiers(TI* ret, bool* is_const = nullptr, bool* pure = nullptr) {
        for (;;) {
            if (accept_kw("const")) { if (is_const) *is_const = true; continue; }
            if (accept_kw("override") || accept_kw("final") || accept_kw("volatile")) continue;
            if (is_op("&") || is_op("&&")) { take(); continue; }
            if (accept_kw("noexcept")) { if (is_op("(")) skip_balanced("(", ")"); continue; }
            if (accept_op("->")) { TI t = parse_type(); if (ret) *ret = t; continue; }
            if (accept_kw("requires")) { while (!is_op("{") && !is_op(";")) take(); continue; }
            break;
        }
        if (is_op("=") && ahead(1).t == T::Int && ahead(1).i == 0) { take(); take(); if (pure) *pure = true; }
    }
    // Builds a function: `cls` non-empty for member functions (slot 0 is `this`).
    std::shared_ptr<FnDecl> function_body(const std::string& name, int line, const std::string& cls, bool is_static, const TI& ret,
                                          std::vector<Param>* pre_params = nullptr, const std::vector<std::pair<std::string, NodeP>>* = nullptr) {
        auto fd = std::make_shared<FnDecl>(); fd->name = cls.empty() ? name : cls + "::" + name; fd->line = line; fd->file = file_;
        fd->attrs = take_attrs(); fd->is_static = is_static;
        push_fn(cls, is_static);
        fns_.back().ret = std::make_shared<TI>(ret);
        if (!cls.empty() && !is_static) { fd->is_method = true; declare("self", false); }
        std::vector<Param> ps = pre_params ? std::move(*pre_params) : params();
        for (auto& p : ps) { if (p.name == "...") { p.name = "__va"; } p.slot = declare(p.name, false).slot; }
        fd->params = std::move(ps);
        TI r = ret; bool cq = false;
        fn_qualifiers(&r, &cq);
        fd->ret_name = tname(r); fd->is_const = cq;
        fns_.back().ret = std::make_shared<TI>(r);
        fd->body = block_body(line, false);
        fd->nslots = fns_.back().nslots;
        pop_fn();
        return fd;
    }
    NodeP function_def(const TI& ret, const std::string& name, int line, const std::string& cls) {
        // Forward declaration `int f(int);` -> nothing to do.
        std::size_t save = i_;
        { skip_balanced("(", ")"); TI dummy; bool pure = false; fn_qualifiers(&dummy, nullptr, &pure); if (accept_op(";")) return nullptr; i_ = save; }
        VarRef v = declare(name, false);   // declared first: recursion
        std::size_t sb = cur().off;
        auto fd = function_body(name, line, cls, false, ret);
        keep_source(*fd, sb);
        if (name == "main" && fns_.back().top && ns_.empty()) {
            if (!fd->params.empty()) fail_at(line, "main must take no parameters in a script; use forge::args()");
            has_main_ = true;
        }
        auto n = mk(NK::FnStmt, line); n->s = name; n->var = v; n->fn = fd;
        return n;
    }
    NodeP return_stmt() {
        int line = take().line; auto n = mk(NK::Return, line);
        if (fns_.back().limit_stmt) fail_at(line, "'return' inside a limit block statement would only leave the block; write `auto r = limit {...} { return x; };` to get a value out");
        if (accept_op(";")) return n;
        std::shared_ptr<TI> fret = fns_.back().ret;   // copy: parsing a lambda below may grow fns_
        if (is_op("{")) {
            if (!fret) fail_at(line, "brace-initialised return needs a declared return type");
            n->kids.push_back(construct(*fret, brace_list(), true, line));
        } else {
            NodeP e = expr();
            if (fret && fret->k == TK::Float && e->k == NK::Int) { auto d = mk(NK::Float, line); d->d = double(e->i); e = d; }
            if (fret && fret->k == TK::Weak) e = builtin_call("forge::__weak", {e}, line);
            n->kids.push_back(e->k == NK::Var && !e->var.global() ? e : copy_of(e));   // returning a local moves it (NRVO)
        }
        semi();
        return n;
    }
    NodeP condition() { expect_op("("); auto c = expr(); expect_op(")"); return c; }
    NodeP if_stmt() {
        int line = take().line;
        accept_kw("constexpr");
        expect_op("(");
        auto outer = mk(NK::Block, line);
        BlockGuard g(*this, &outer->locals);
        NodeP c;
        if (auto ti = try_decl_type()) {   // if (init; cond) / if (T x = e)
            std::string name = ident();
            auto d = declarator(*ti, name, line);
            outer->kids.push_back(d); flush_extra(*outer);
            if (accept_op(";")) c = expr(); else c = var_node(name, line);
        } else {
            c = expr();
            if (accept_op(";")) { outer->kids.push_back(expr_stmt(c, line)); c = expr(); }
        }
        expect_op(")");
        auto n = mk(NK::If, line); n->kids.push_back(c); n->kids.push_back(sub_statement());
        if (accept_kw("else")) n->kids.push_back(sub_statement());
        if (outer->kids.empty()) return n;
        outer->kids.push_back(n);
        return outer;
    }
    NodeP loop_body() { brk_.push_back('l'); ++loops_; auto b = sub_statement(); --loops_; brk_.pop_back(); return b; }
    NodeP while_stmt() {
        int line = take().line; auto n = mk(NK::While, line);
        n->kids.push_back(condition()); n->kids.push_back(loop_body());
        return n;
    }
    NodeP do_stmt() {
        int line = take().line; auto n = mk(NK::While, line);
        n->kids.push_back(mk(NK::True, line));
        n->kids.push_back(loop_body());
        if (!accept_kw("while")) fail("expected 'while' after do body");
        auto c = condition(); semi();
        auto neg = mk(NK::Unary, line); neg->s = "not"; neg->kids.push_back(c);
        auto stop = mk(NK::If, line); stop->kids.push_back(neg);
        auto bb = mk(NK::Block, line); bb->kids.push_back(mk(NK::Break, line)); stop->kids.push_back(bb);
        auto step = mk(NK::Block, line); step->kids.push_back(stop);
        n->extra = step;
        return n;
    }
    bool range_for_ahead() const {
        int depth = 0;
        for (std::size_t k = i_; k < t_.size(); ++k) {
            const Tok& t = t_[k];
            if (t.t == T::Op) {
                if (t.s == "(" || t.s == "[" || t.s == "{") ++depth;
                else if (t.s == ")" || t.s == "]" || t.s == "}") { if (depth == 0) return false; --depth; }
                else if (depth == 0 && t.s == ";") return false;
                else if (depth == 0 && t.s == ":") return true;
            }
        }
        return false;
    }
    NodeP for_stmt() {
        int line = take().line;
        expect_op("(");
        auto outer = mk(NK::Block, line);
        BlockGuard g(*this, &outer->locals);
        if (range_for_ahead()) {
            TI ti = parse_type();
            std::vector<std::string> names;
            if (accept_op("[")) { while (!is_op("]")) { names.push_back(ident()); if (!accept_op(",")) break; } expect_op("]"); }
            else names.push_back(ident());
            expect_op(":");
            auto src = expr();
            expect_op(")");
            if (names.empty()) fail_at(line, "range-for binding needs a name");
            if (names.size() > 2) fail_at(line, "range-for bindings support at most two names");
            auto n = mk(NK::For, line);
            n->kids.push_back(src);
            n->s = ti.ref && !ti.cnst ? "ref" : (ti.ref ? "" : "copy");
            n->var = declare(names[0], ti.cnst);
            if (names.size() == 2) { n->flag = true; n->i = declare(names[1], ti.cnst).slot; }
            n->kids.push_back(loop_body());
            outer->kids.push_back(n);
            return outer;
        }
        if (!accept_op(";")) {
            if (auto ti = try_decl_type()) { outer->kids.push_back(declaration(*ti, line)); flush_extra(*outer); }
            else { auto e = expr(); semi(); outer->kids.push_back(expr_stmt(e, line)); while (false) {} }
        }
        NodeP c = is_op(";") ? NodeP(mk(NK::True, line)) : expr();
        semi();
        NodeP step;
        if (!is_op(")")) {
            auto sb = mk(NK::Block, line);
            for (;;) { auto e = assign_expr(); sb->kids.push_back(expr_stmt(e, line)); if (!accept_op(",")) break; }
            step = sb;
        }
        expect_op(")");
        auto n = mk(NK::While, line);
        n->kids.push_back(c); n->kids.push_back(loop_body()); n->extra = step;
        outer->kids.push_back(n);
        return outer;
    }
    // switch: `st` = index of the first matching case (or default, or -1); section i runs when 0 <= st <= i
    // (fall-through), all inside a one-shot loop so `break` leaves the switch.
    NodeP switch_stmt() {
        int line = take().line;
        expect_op("(");
        auto outer = mk(NK::Block, line);
        BlockGuard g(*this, &outer->locals);
        NodeP subj = expr();
        expect_op(")");
        VarRef sv = hidden("switch", line), st = hidden("case", line);
        auto ls = mk(NK::Let, line); ls->var = sv; ls->s = sv.name; ls->kids.push_back(subj); outer->kids.push_back(ls);
        expect_op("{");
        brk_.push_back('s');
        std::vector<std::vector<NodeP>> labels; std::vector<NodeP> bodies; int dflt = -1;
        while (!is_op("}")) {
            if (at(T::End)) fail("expected '}'");
            if (is_kw("case") || is_kw("default")) {
                std::vector<NodeP> labs; bool is_d = false;
                while (is_kw("case") || is_kw("default")) {
                    if (accept_kw("default")) { is_d = true; expect_op(":"); continue; }
                    take(); labs.push_back(ternary()); expect_op(":");
                }
                if (is_d) dflt = int(labels.size());
                labels.push_back(labs);
                auto b = mk(NK::Block, cur().line);
                BlockGuard g2(*this, &b->locals);
                while (!is_op("}") && !is_kw("case") && !is_kw("default")) { if (auto s = statement()) b->kids.push_back(s); flush_extra(*b); }
                bodies.push_back(b);
                continue;
            }
            fail("expected 'case' or 'default' in switch");
        }
        expect_op("}");
        brk_.pop_back();
        // st = (v == a || v == b) ? 0 : (v == c) ? 1 : dflt
        NodeP sel = int_node(dflt, line);
        for (int k = int(labels.size()) - 1; k >= 0; --k) {
            if (labels[std::size_t(k)].empty()) continue;
            NodeP c;
            for (auto& l : labels[std::size_t(k)]) {
                auto eq = mk(NK::Binary, line); eq->s = "=="; eq->i = std::int64_t(Op::Eq); eq->kids = {hidden_ref(sv, line), l};
                if (!c) c = eq; else { auto o = mk(NK::Or, line); o->kids = {c, eq}; c = o; }
            }
            auto t = mk(NK::Ternary, line); t->kids = {c, int_node(k, line), sel}; sel = t;
        }
        auto lst = mk(NK::Let, line); lst->var = st; lst->s = st.name; lst->kids.push_back(sel); outer->kids.push_back(lst);
        auto loop = mk(NK::While, line); loop->kids.push_back(mk(NK::True, line));
        auto body = mk(NK::Block, line);
        for (std::size_t k = 0; k < bodies.size(); ++k) {
            auto ge = mk(NK::Binary, line); ge->s = ">="; ge->i = std::int64_t(Op::Ge); ge->kids = {hidden_ref(st, line), int_node(0, line)};
            auto le = mk(NK::Binary, line); le->s = "<="; le->i = std::int64_t(Op::Le); le->kids = {hidden_ref(st, line), int_node(std::int64_t(k), line)};
            auto a = mk(NK::And, line); a->kids = {ge, le};
            auto i = mk(NK::If, line); i->kids = {a, bodies[k]};
            body->kids.push_back(i);
        }
        body->kids.push_back(mk(NK::Break, line));
        loop->kids.push_back(body);
        outer->kids.push_back(loop);
        return outer;
    }
    NodeP try_stmt() {
        int line = take().line; auto n = mk(NK::Try, line);
        n->kids.push_back(block_body(line));
        if (!is_kw("catch")) fail("expected 'catch' after try block");
        while (accept_kw("catch")) {
            int cl = cur().line;
            expect_op("(");
            auto c = mk(NK::Block, cl);
            BlockGuard g(*this, &c->locals);
            std::string vname;
            if (accept_op("...")) c->s = "";
            else {
                TI ti = parse_type();
                c->s = ti.k == TK::Class ? ti.name : ti.k == TK::String ? "String" : "";
                if (at(T::Ident)) vname = take().s;
            }
            expect_op(")");
            c->var = vname.empty() ? hidden("caught", cl) : declare(vname, false);
            catch_vars_.push_back(c->var);
            c->kids.push_back(block_body(cl, false));
            catch_vars_.pop_back();
            n->kids.push_back(c);
        }
        return n;
    }

    // ---------------------------------------------------------------- classes
    // Pass 1 over the class body: member names (so method bodies can refer to later members) and statics.
    void scan_members(ClassInfo& ci, const std::string& cls) {
        int depth = 0, paren = 0; bool stmt_static = false; bool after_tilde = false, after_operator = false;
        for (std::size_t k = i_; k < t_.size(); ++k) {
            const Tok& t = t_[k];
            if (t.t == T::End) break;
            if (t.t == T::Op) {
                if (t.s == "{") { ++depth; continue; }
                if (t.s == "}") { if (--depth == 0) break; if (depth == 1 && paren == 0) stmt_static = false; continue; }
                if (t.s == "(") ++paren; else if (t.s == ")") --paren;
                if (depth == 1 && paren == 0 && t.s == ";") stmt_static = false;
                after_tilde = t.s == "~";
                continue;
            }
            if (depth != 1 || paren != 0) continue;
            if (t.t == T::Kw) { if (t.s == "static") stmt_static = true; after_operator = t.s == "operator"; if (t.s == "public" || t.s == "private" || t.s == "protected") stmt_static = false; continue; }
            if (t.t != T::Ident) continue;
            const Tok& nx = t_[k + 1];
            bool member = nx.t == T::Op && (nx.s == "(" || nx.s == "=" || nx.s == ";" || nx.s == "{" || nx.s == "," || nx.s == "[");
            if (member && t.s != cls && !after_tilde && !after_operator && !is_type_word(t.s)) {
                ci.members.insert(t.s);
                if (stmt_static) ci.statics.insert(t.s);
            }
            after_tilde = false; after_operator = false;
        }
    }
    NodeP class_stmt() {
        int line = cur().line;
        bool is_struct = take().s == "struct";
        skip_attributes();   // struct [[deprecated]] S
        auto class_attrs = take_attrs();
        std::string name = ident();
        std::string full = ns_name(name);
        types_.insert(name); types_.insert(full);
        auto cd = std::make_shared<ClassDecl>(); cd->name = full; cd->is_struct = true;
        cd->meta->attrs = std::move(class_attrs); cd->meta->file = file_; cd->meta->line = line;
        accept_kw("final");
        if (accept_op(";")) return nullptr;   // forward declaration
        if (accept_op(":")) {
            for (;;) {
                accept_kw("public"); accept_kw("private"); accept_kw("protected"); accept_kw("virtual");
                std::string b = canon_type_name(ident());
                if (is_op("<")) skip_balanced("<", ">");
                b = exception_class(b);
                if (cd->base.empty()) cd->base = b; else cd->interfaces.push_back(b);
                if (!accept_op(",")) break;
            }
        }
        ClassInfo& ci = classes_[full]; ci.decl = cd; ci.base = cd->base;
        if (full != name) classes_[name] = ci;
        scan_members(ci, name);
        if (full != name) classes_[name] = ci;
        VarRef cv = declare(name, true);
        expect_op("{");
        std::string access = is_struct ? "public" : "private";
        std::vector<NodeP> post;   // static member initialisers, run after the class is defined
        std::map<std::string, int> ctor_count;
        std::vector<std::shared_ptr<FnDecl>> ctors;
        while (!accept_op("}")) {
            if (at(T::End)) fail("expected '}' at end of class");
            int ml = cur().line;
            pending_attrs_.clear();
            skip_attributes();
            if (accept_op(";")) continue;
            if ((is_kw("public") || is_kw("private") || is_kw("protected")) && ahead_op(1, ":")) { access = take().s; take(); continue; }
            if (is_kw("using") || is_kw("typedef")) { auto s = statement(); (void)s; continue; }
            if (accept_kw("friend")) {
                if (is_kw("class") || is_kw("struct")) { while (!accept_op(";")) take(); continue; }
                TI ret = parse_type();
                std::string fname = is_kw("operator") ? operator_name() : ident();
                VarRef saved_top_ns;
                // friend functions are free functions at namespace scope
                auto saved = std::move(fns_); fns_.clear(); fns_.push_back(saved.front());
                auto fs = function_def(ret, fname, ml, "");
                saved.front() = fns_.front(); fns_ = std::move(saved);
                if (fs) extra_after_class_.push_back(fs);
                continue;
            }
            if (is_kw("class") || is_kw("struct") || is_kw("enum")) fail_at(ml, "nested types are not supported; declare them at namespace scope");
            if (is_kw("template")) { take(); template_params(); }
            if (is_kw("virtual") && ahead_op(1, "~")) take();
            if (is_op("~")) {   // destructor
                take(); std::string dn = ident(); if (dn != name) fail_at(ml, "destructor name must match the class");
                expect_op("("); expect_op(")"); fn_qualifiers(nullptr);
                if (accept_op("=")) { take(); semi(); continue; }
                if (accept_op(";")) continue;
                std::vector<Param> none;
                // function_body parses params itself; feed it an empty list by rewinding is simpler: build manually
                auto fd = std::make_shared<FnDecl>(); fd->name = name + "::~" + name; fd->line = ml; fd->file = file_; fd->is_method = true;
                push_fn(full, false); declare("self", false);
                fd->body = block_body(ml, false); fd->nslots = fns_.back().nslots; pop_fn();
                fd->name = "~";
                auto fdn = std::make_shared<FnDecl>(*fd); fdn->name = "~";
                cd->methods.push_back(fdn);
                continue;
            }
            bool explicit_ctor = false;
            while (accept_kw("explicit") || accept_kw("constexpr") || accept_kw("inline")) explicit_ctor = true;
            (void)explicit_ctor;
            if (at(T::Ident) && cur().s == name && ahead_op(1, "(")) {   // constructor
                take();
                auto ca = take_attrs();
                auto fd = constructor(full, ml);
                if (!fd) continue;
                fd->attrs = std::move(ca); fd->access = access; fd->ret_name = "void";
                ctors.push_back(fd);
                continue;
            }
            bool is_virtual = is_kw("virtual");
            TI ti = parse_type();
            std::string mname = is_kw("operator") ? operator_name() : ident();
            if (is_op("(")) {   // method
                bool pure = false;
                std::size_t save = i_;
                skip_balanced("(", ")");
                TI d; fn_qualifiers(&d, nullptr, &pure);
                if (pure) { semi(); cd->required.push_back(mname); continue; }
                if (accept_op(";")) { continue; }   // declared here, defined out of line
                if (is_op("=")) { take(); take(); semi(); continue; }   // = default / = delete
                i_ = save;
                auto fd = function_body(mname, ml, full, ti.is_static, ti);
                fd->name = mname; fd->access = access; fd->is_virtual = is_virtual;
                if (ti.is_static) {   // static member function: kept on the class as a plain function
                    auto lam = mk(NK::Lambda, ml); lam->fn = fd;
                    auto f = mk(NK::Field, ml); f->s = mname; f->kids.push_back(hidden_ref(cv, ml));
                    post.push_back(assign_node(f, lam, ml));
                    cd->statics.push_back(mname);
                } else cd->methods.push_back(fd);
                continue;
            }
            // data members: T a = 1, b{2}, c;
            auto mattrs = take_attrs();
            for (;;) {
                { MemberMeta mm; mm.name = mname; TI bare = ti; bare.is_static = false; mm.type = tname(bare); mm.ann = bare.ann(); mm.is_const = ti.cnst;
                  mm.is_static = ti.is_static; mm.access = access; mm.attrs = mattrs; mm.line = ml; cd->meta->fields.push_back(std::move(mm)); }
                NodeP init;
                if (accept_op("=")) init = initializer(ti, ml);
                else if (is_op("{")) init = construct(ti, brace_list(), true, ml);
                else init = default_value(ti, ml);
                if (ti.is_static) {
                    auto f = mk(NK::Field, ml); f->s = mname; f->kids.push_back(hidden_ref(cv, ml));
                    post.push_back(assign_node(f, init, ml));
                    cd->statics.push_back(mname);
                } else cd->fields.emplace_back(mname, init);
                if (!accept_op(",")) break;
                mname = ident();
            }
            semi();
        }
        semi();
        if (ctors.size() == 1) ctors[0]->name = "init";
        else for (auto& c : ctors) c->name = "init/" + std::to_string(c->params.size());
        for (auto& c : ctors) cd->methods.push_back(c);
        if (!cd->required.empty() || cd->base.empty()) {}
        cd->var = cv;
        classes_[full].decl = cd; if (full != name) classes_[name].decl = cd;
        auto n = mk(NK::ClassStmt, line); n->s = full; n->var = cv; n->cls = cd;
        for (auto& p : post) extra_.push_back(p);
        for (auto& f : extra_after_class_) extra_.push_back(f);
        extra_after_class_.clear();
        return n;
    }
    std::string operator_name() {
        expect_kw_operator();
        if (accept_op("(")) { expect_op(")"); return "operator()"; }
        if (accept_op("[")) { expect_op("]"); return "operator[]"; }
        if (at(T::Op)) return "operator" + take().s;
        if (is_kw("bool") || at(T::Ident)) { TI t = parse_type(); (void)t; return "operator bool"; }
        fail("bad operator name");
    }
    void expect_kw_operator() { if (!accept_kw("operator")) fail("expected 'operator'"); }
    // Constructor after `Name`: params, member-initialiser list, body.
    std::shared_ptr<FnDecl> constructor(const std::string& cls, int line) {
        std::size_t save = i_;
        skip_balanced("(", ")");
        fn_qualifiers(nullptr);
        if (accept_op(";")) return nullptr;
        if (is_op("=")) { take(); take(); semi(); return nullptr; }   // = default / = delete
        i_ = save;
        auto fd = std::make_shared<FnDecl>(); fd->name = "init"; fd->line = line; fd->file = file_; fd->is_method = true;
        push_fn(cls, false);
        fns_.back().ret = std::make_shared<TI>(TI{TK::Void});
        declare("self", false);
        auto ps = params();
        for (auto& p : ps) { if (p.name == "...") p.name = "__va"; p.slot = declare(p.name, false).slot; }
        fd->params = std::move(ps);
        fn_qualifiers(nullptr);
        std::vector<NodeP> inits;
        if (accept_op(":")) {
            for (;;) {
                int il = cur().line;
                std::string m = canon_type_name(ident());
                if (is_op("<")) skip_balanced("<", ">");
                std::vector<NodeP> args;
                bool brace = is_op("{");
                if (brace) args = brace_list(); else args = paren_args();
                auto& ci = classes_[cls];
                std::string em = exception_class(m);
                if (em == ci.base || m == ci.base) {   // base-class initialiser
                    auto s = mk(NK::Super, il); s->s = "init"; s->var = lookup("self");
                    s->type = std::make_shared<TypeAnn>(); s->type->name = cls;
                    inits.push_back(expr_stmt_of(call_node(s, args, il), il));
                } else {
                    auto f = mk(NK::Field, il); f->s = m; f->kids.push_back(self_node(il));
                    NodeP v = args.empty() ? NodeP(mk(NK::Nil, il)) : args.size() == 1 && !brace ? copy_of(args[0]) : NodeP();
                    if (!v) { auto l = mk(NK::List, il); for (auto& a : args) l->kids.push_back(a); v = l; }
                    if (brace && args.size() == 1) v = copy_of(args[0]);
                    if (brace && args.empty()) v = mk(NK::Nil, il);
                    inits.push_back(assign_node(f, v, il));
                }
                if (!accept_op(",")) break;
            }
        }
        auto body = block_body(line, false);
        auto full = mk(NK::Block, line);
        for (auto& x : inits) full->kids.push_back(x);
        for (auto& x : body->kids) full->kids.push_back(x);
        fd->body = full;
        fd->nslots = fns_.back().nslots;
        pop_fn();
        return fd;
    }
    // Out-of-line member definition `T Cls::name(...) { ... }` / `Cls::Cls(...) : ... { }`.
    NodeP out_of_line(const TI& ret, const std::string& qname, int line) {
        auto p = qname.rfind("::");
        std::string cls = qname.substr(0, p), m = qname.substr(p + 2);
        auto it = classes_.find(cls);
        if (it == classes_.end() || !it->second.decl) fail_at(line, "unknown class '" + cls + "'");
        auto cd = it->second.decl;
        std::string short_cls = cls.substr(cls.rfind(':') == std::string::npos ? 0 : cls.rfind(':') + 1);
        if (m == short_cls) {
            auto fd = constructor(cd->name, line);
            if (fd) {
                int n = 0; for (auto& x : cd->methods) if (x->name.rfind("init", 0) == 0) ++n;
                fd->name = "init/" + std::to_string(fd->params.size());
                cd->methods.push_back(fd);
                // a single constructor is also reachable as plain "init"
                bool has_plain = false; for (auto& x : cd->methods) if (x->name == "init") has_plain = true;
                if (!has_plain && n == 0) { auto c = std::make_shared<FnDecl>(*fd); c->name = "init"; cd->methods.push_back(c); }
            }
            return nullptr;
        }
        bool is_static = it->second.statics.count(m) > 0;
        auto fd = function_body(m, line, cd->name, is_static, ret);
        fd->name = m;
        if (is_static) {
            auto lam = mk(NK::Lambda, line); lam->fn = fd;
            auto f = mk(NK::Field, line); f->s = m; f->kids.push_back(hidden_ref(cd->var, line));
            cd->statics.push_back(m);
            return assign_node(f, lam, line);
        }
        cd->methods.push_back(fd);
        return nullptr;
    }
    NodeP enum_stmt() {
        int line = take().line;
        bool scoped = accept_kw("class") || accept_kw("struct");
        std::string name = at(T::Ident) ? take().s : "<enum" + std::to_string(line) + ">";
        if (accept_op(":")) parse_type();
        if (accept_op(";")) return nullptr;
        enums_.insert(name); types_.insert(name);
        expect_op("{");
        auto m = mk(NK::Map, line);
        std::int64_t next = 0;
        std::vector<std::pair<std::string, NodeP>> vals;
        while (!is_op("}")) {
            std::string e = ident();
            NodeP v;
            if (accept_op("=")) { v = ternary(); if (v->k == NK::Int) next = v->i; }
            if (!v || v->k == NK::Int) { v = int_node(next, line); }
            ++next;
            m->kids.push_back(str_node(e, line)); m->kids.push_back(v);
            vals.emplace_back(e, v);
            if (!accept_op(",")) break;
        }
        expect_op("}"); semi();
        auto let = mk(NK::Let, line); let->s = name; let->flag = true;
        let->kids.push_back(builtin_call("forge::__enum", {str_node(name, line), m}, line));
        let->var = declare(name, true);
        if (!scoped) for (auto& [e, v] : vals) { auto d = mk(NK::Let, line); d->s = e; d->flag = true; d->kids.push_back(v); d->var = declare(e, true); extra_.push_back(d); }
        return let;
    }
    NodeP namespace_stmt() {
        int line = take().line;
        std::string name = at(T::Ident) ? take().s : "";
        if (accept_op("=")) { std::string target = ident(); semi(); aliases_ns_[name] = target; return nullptr; }
        if (!fns_.back().top || fns_.back().blocks.size() != 1) fail_at(line, "namespaces must be declared at file scope");
        std::string saved = ns_;
        if (!name.empty()) ns_ = ns_name(name);
        expect_op("{");
        auto b = mk(NK::Block, line);
        b->s = "namespace";
        while (!is_op("}")) { if (at(T::End)) fail("expected '}'"); if (auto s = statement()) b->kids.push_back(s); flush_extra(*b); }
        expect_op("}");
        ns_ = saved;
        // namespace members are globals: splice the statements into the enclosing block (no new scope)
        for (auto& k : b->kids) extra_.push_back(k);
        return nullptr;
    }
    NodeP using_stmt() {
        int line = take().line;
        if (accept_kw("namespace")) {
            std::string ns = ident(); semi();
            if (ns == "std") using_std_ = true;
            using_ns_.push_back(ns);
            return expr_stmt_of(builtin_call("forge::__using_namespace", {str_node(ns, line)}, line), line);
        }
        std::string name = ident();
        if (accept_op("=")) { TI ti = parse_type(); typedefs_[name] = ti; semi(); return nullptr; }
        semi();   // using std::cout;
        auto p = name.rfind("::");
        if (p != std::string::npos) { aliases_[name.substr(p + 2)] = name; if (builtin_types().count(name)) typedefs_[name.substr(p + 2)] = TI{}; }
        if (p != std::string::npos && builtin_types().count(name)) { typedefs_.erase(name.substr(p + 2)); aliases_[name.substr(p + 2)] = name; }
        return nullptr;
    }

    // ---------------------------------------------------------------- expressions
    NodeP expr() { return assign_expr(); }
    void no_assign(const NodeP& e) const {
        if (e && e->k == NK::Assign) fail_at(e->line, "assignment / increment used as a value is not supported; make it a separate statement");
    }
    NodeP assign_expr() {
        int line = cur().line;
        NodeP lhs = ternary();
        static const char* aops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
        for (const char* o : aops) {
            if (is_op(o)) {
                take();
                if (!is_lvalue(lhs)) fail_at(line, "left side of '" + std::string(o) + "' is not assignable");
                check_assignable(lhs, line);
                NodeP rhs;
                if (is_op("{")) { auto l = mk(NK::List, line); for (auto& x : brace_list()) l->kids.push_back(x); rhs = l; }
                else rhs = assign_expr();
                no_assign(rhs);
                auto a = mk(NK::Assign, line); a->s = o;
                a->i = std::int64_t(std::string(o) == "=" ? Op::None : op_of(o));
                a->kids = {lhs, std::string(o) == "=" ? copy_of(rhs) : rhs};
                return a;
            }
        }
        return lhs;
    }
    void check_assignable(const NodeP& t, int line) const {
        if (t->k == NK::Var && is_const_var(t->var)) fail_at(line, "cannot assign to const '" + t->s + "'");
    }
    NodeP ternary() {
        NodeP c = binary(0);
        if (!is_op("?")) return c;
        int line = take().line;
        NodeP a = assign_expr(); expect_op(":"); NodeP b = assign_expr();
        no_assign(c); no_assign(a); no_assign(b);
        auto t = mk(NK::Ternary, line); t->kids = {c, a, b};
        return t;
    }
    static int prec(const std::string& o) {
        static const std::unordered_map<std::string, int> p = {
            {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6},
            {"<", 7}, {"<=", 7}, {">", 7}, {">=", 7}, {"<=>", 8}, {"<<", 9}, {">>", 9},
            {"+", 10}, {"-", 10}, {"*", 11}, {"/", 11}, {"%", 11}};
        auto it = p.find(o); return it == p.end() ? -1 : it->second;
    }
    NodeP binary(int min) {
        NodeP lhs = unary();
        for (;;) {
            if (!at(T::Op)) { if (is_kw("and")) {} break; }
            std::string o = cur().s; int p = prec(o);
            if (p < 0 || p <= min - 1 || p < min) break;
            int line = take().line;
            NodeP rhs = binary(p + 1);
            no_assign(lhs); no_assign(rhs);
            if (o == "&&" || o == "||") { auto n = mk(o == "&&" ? NK::And : NK::Or, line); n->kids = {lhs, rhs}; lhs = n; continue; }
            if (o == "<=>") { lhs = builtin_call("forge::__cmp3", {lhs, rhs}, line); continue; }
            auto n = mk(NK::Binary, line); n->s = o; n->i = std::int64_t(op_of(o)); n->kids = {lhs, rhs};
            lhs = n;
        }
        return lhs;
    }
    NodeP incdec(NodeP target, const std::string& o, int line) {
        if (!is_lvalue(target)) fail_at(line, "operand of " + o + " is not assignable");
        check_assignable(target, line);
        auto a = mk(NK::Assign, line); a->s = o == "++" ? "+=" : "-="; a->i = std::int64_t(o == "++" ? Op::Add : Op::Sub);
        a->kids = {target, int_node(1, line)};
        return a;
    }
    NodeP unary() {
        int line = cur().line;
        if (is_op("!") || (at(T::Ident) && cur().s == "not")) { take(); auto n = mk(NK::Unary, line); n->s = "not"; n->kids.push_back(unary()); no_assign(n->kids[0]); return n; }
        if (accept_op("-")) {
            auto x = unary(); no_assign(x);
            if (x->k == NK::Int) { auto n = mk(NK::Int, line); n->i = -x->i; return n; }
            if (x->k == NK::Float) { auto n = mk(NK::Float, line); n->d = -x->d; return n; }
            auto n = mk(NK::Unary, line); n->s = "-"; n->kids.push_back(x); return n;
        }
        if (accept_op("+")) return unary();
        if (accept_op("~")) { auto n = mk(NK::Unary, line); n->s = "~"; n->kids.push_back(unary()); return n; }
        if (is_op("++") || is_op("--")) { std::string o = take().s; return incdec(unary(), o, line); }
        if (accept_op("*")) { auto x = unary(); return builtin_call("forge::__deref", {x}, line); }
        if (is_op("&")) fail_at(line, "taking the address of a value is not allowed in FORGE scripts; use a reference or a smart pointer");
        if (is_kw("new") || is_kw("delete")) fail_at(line, "new/delete are not allowed; use std::make_unique / std::make_shared");
        if (is_kw("sizeof") || is_kw("alignof")) fail_at(line, "sizeof is not available in FORGE scripts");
        if (is_kw("co_await")) fail_at(line, "co_await is not supported; use forge::await(task)");
        return postfix(primary());
    }
    NodeP deref_target(NodeP e) {   // *p as an assignment target is p itself
        if (e->k == NK::Call && e->kids[0]->k == NK::Var && e->kids[0]->s == "forge::__deref") return e->kids[1];
        return e;
    }
    NodeP postfix(NodeP e) {
        for (;;) {
            int line = cur().line;
            if (is_op("(")) { auto args = paren_args(); e = call_node(e, args, line); continue; }
            if (accept_op("[")) { auto i = expr(); expect_op("]"); auto n = mk(NK::Index, line); n->kids = {deref_target(e), i}; e = n; continue; }
            if (accept_op(".") || accept_op("->")) {
                accept_kw("template");
                std::string m;
                if (is_kw("operator")) m = operator_name();
                else if (at(T::Op) && is_op("~")) fail_at(line, "explicit destructor calls are not allowed");
                else m = ident();
                auto n = mk(NK::Field, line); n->s = m; n->kids.push_back(deref_target(e));
                e = n; continue;
            }
            if (is_op("++") || is_op("--")) { std::string o = take().s; e = incdec(e, o, line); continue; }
            break;
        }
        return e;
    }
    // Template arguments after a name: `<int, std::string>`; backtracks if this is a comparison.
    std::optional<std::vector<TI>> try_template_args() {
        if (!is_op("<")) return std::nullopt;
        std::size_t save = i_, mark = undo_.size();
        try {
            take(); std::vector<TI> args;
            while (!is_op(">") && !is_op(">>") && !is_op(">=") && !is_op(">>=")) {
                if (at(T::Int)) { TI t{TK::Int}; t.name = std::to_string(cur().i); take(); args.push_back(t); }
                else args.push_back(parse_type());
                if (!accept_op(",")) break;
            }
            close_angle();
            if (is_op("(") || is_op("{") || (at(T::Ident) && cur().s.rfind("::", 0) == 0)) return args;
        } catch (const syntax_error&) {}
        rollback(save, mark);
        return std::nullopt;
    }
    std::string canon_fn_name(const std::string& w) const {
        if (auto a = aliases_.find(w); a != aliases_.end()) return a->second;
        for (auto& [k, v] : aliases_ns_) if (w.rfind(k + "::", 0) == 0) return v + w.substr(k.size());
        return w;
    }
    NodeP primary() {
        int line = cur().line;
        const Tok& t = cur();
        switch (t.t) {
        case T::Int: { take(); return int_node(t.i, line); }
        case T::Float: { take(); auto n = mk(NK::Float, line); n->d = t.d; return n; }
        case T::Str: {
            std::string s = take().s;
            while (at(T::Str)) s += take().s;   // adjacent literals concatenate
            if (at(T::Ident) && (cur().s == "s" || cur().s == "sv")) take();   // "..."s
            return str_node(s, line);
        }
        case T::End: fail("unexpected end of input");
        default: break;
        }
        if (accept_kw("true")) return mk(NK::True, line);
        if (accept_kw("false")) return mk(NK::False, line);
        if (accept_kw("nullptr")) return mk(NK::Nil, line);
        if (accept_kw("this")) return self_node(line);
        if (is_op("(")) { take(); auto e = expr(); expect_op(")"); return e; }
        if (is_op("[")) return lambda();
        if (is_op("{")) { auto l = mk(NK::List, line); for (auto& x : brace_list()) l->kids.push_back(x); return l; }
        if (is_kw("static_cast") || is_kw("dynamic_cast")) {
            bool dyn = take().s == "dynamic_cast";
            expect_op("<"); TI ti = parse_type(); close_angle();
            expect_op("("); auto e = expr(); expect_op(")");
            if (dyn) return builtin_call("forge::__downcast", {ti.k == TK::Class ? var_node(ti.name, line) : NodeP(mk(NK::Nil, line)), e}, line);
            return cast_to(ti, e, line);
        }
        if (is_kw("reinterpret_cast") || is_kw("const_cast")) fail_at(line, "'" + cur().s + "' is not allowed in FORGE scripts (memory safety)");
        if (is_kw("auto") || is_kw("void") || is_kw("typename") || is_kw("const")) fail_at(line, "unexpected '" + cur().s + "'");
        if (at(T::Kw)) fail_at(line, "unexpected '" + cur().s + "'");
        if (at(T::Op)) fail_at(line, "unexpected '" + cur().s + "'");
        std::string w = canon_fn_name(take().s);
        if (using_std_ && w.find("::") == std::string::npos && !is_local(w) && lookup(w).name == w && !classes_.count(w) && !ns_has(w) && std_function_names().count(w)) w = "std::" + w;
        // special forms
        if (w == "std::move") { expect_op("("); auto e = expr(); expect_op(")"); if (!is_lvalue(e)) return e; auto m = mk(NK::Move, line); m->kids.push_back(e); return m; }
        if (w == "std::forward") { try_template_args(); expect_op("("); auto e = expr(); expect_op(")"); return e; }
        if (w == "std::make_shared" || w == "std::make_unique") {
            auto targs = try_template_args();
            if (!targs || targs->size() != 1) fail_at(line, w + " needs a type argument");
            auto args = paren_args();
            NodeP v = construct_value((*targs)[0], args, line);
            return builtin_call("forge::__own", {int_node(w == "std::make_shared" ? 1 : 2, line), v}, line);
        }
        if (w == "std::get") {
            auto targs = try_template_args();
            if (!targs || targs->size() != 1) fail_at(line, "std::get needs an index");
            auto args = paren_args();
            if (args.size() != 1) fail_at(line, "std::get takes one argument");
            const TI& a = (*targs)[0];
            return builtin_call("forge::__get", {args[0], a.name.empty() ? str_node("<type>", line) : int_node(get_index(a.name, line), line)}, line);
        }
        if (w == "std::numeric_limits") {
            auto targs = try_template_args();
            if (!targs || targs->size() != 1 || !at(T::Ident)) fail_at(line, "bad std::numeric_limits use");
            std::string m = take().s;   // ::max
            std::string k = (*targs)[0].k == TK::Float ? "float" : "int";
            return global_var("std::numeric_limits<" + k + ">" + m, line);
        }
        if (w == "std::dynamic_pointer_cast" || w == "std::static_pointer_cast") {
            auto targs = try_template_args(); auto args = paren_args();
            if (!targs || targs->size() != 1 || args.size() != 1) fail_at(line, "bad " + w);
            const TI& ti = (*targs)[0];
            return builtin_call("forge::__downcast", {ti.k == TK::Class ? var_node(ti.name, line) : NodeP(mk(NK::Nil, line)), args[0]}, line);
        }
        if (w == "std::make_pair" || w == "std::make_tuple" || w == "std::tie" || w == "std::forward_as_tuple") {
            auto args = paren_args(); auto l = mk(NK::List, line); for (auto& a : args) l->kids.push_back(copy_of(a)); return l;
        }
        // local variables shadow everything else
        if (is_local(w)) return var_node(w, line);
        if (limit_word(w) && limit_block_after(i_)) return limit_block(line, false, true);
        // members of the enclosing class (fields, methods, statics), including inherited ones
        if (const FnScope* ms = method_scope(); ms && w.find("::") == std::string::npos) {
            if (class_has_static(ms->cls, w)) { auto f = mk(NK::Field, line); f->s = w; f->kids.push_back(var_node(short_name(ms->cls), line)); return f; }
            if (class_has(ms->cls, w)) {
                if (ms->is_static) fail_at(line, "'" + w + "' is an instance member; static member functions have no 'this'");
                auto f = mk(NK::Field, line); f->s = w; f->kids.push_back(self_node(line)); return f;
            }
        }
        // qualified static member Cls::x
        if (auto p = w.rfind("::"); p != std::string::npos) {
            std::string head = w.substr(0, p), m = w.substr(p + 2);
            if (classes_.count(head) && !(is_op("(") && m == short_name(head))) {
                if (const FnScope* ms = method_scope(); ms && is_op("(") && derives_from(ms->cls, head) && !class_has_static(head, m)) {
                    auto s = mk(NK::Super, line); s->s = m; s->var = lookup("self");   // Base::method(...) from a derived method
                    s->type = std::make_shared<TypeAnn>(); s->type->name = parent_before(ms->cls, head);
                    return s;
                }
                auto f = mk(NK::Field, line); f->s = m; f->kids.push_back(var_node(head, line)); return f;
            }
        }
        // types used as values: construction / conversion / functional cast
        if (is_type_word(w) && !is_local(w)) {
            std::string cw = canon_type_name(w);
            std::size_t save = i_, mark = undo_.size();
            i_ -= 1; undo_.emplace_back(i_, t_[i_].s); t_[i_].s = w;
            TI ti;
            try { ti = parse_type(); } catch (const syntax_error&) { rollback(save, mark); ti = TI{TK::Any}; }
            if (is_op("(")) { auto args = paren_args(); return construct_value(ti, args, line); }
            if (is_op("{")) { auto args = brace_list(); return construct(ti, args, true, line); }
            if (enums_.count(cw)) return var_node(cw, line);
            if (ti.k == TK::Class) return var_node(ti.name, line);
            return global_var(cw, line);
        }
        if (auto targs = try_template_args()) {   // f<T>(...) : template arguments are erased
            (void)targs;
        }
        return var_node(w, line);
    }
    bool ns_has(const std::string& w) const {
        for (std::string ns = ns_; !ns.empty();) {
            if (auto it = ns_members_.find(ns); it != ns_members_.end() && it->second.count(w)) return true;
            auto p = ns.rfind("::"); ns = p == std::string::npos ? "" : ns.substr(0, p);
        }
        return fns_.front().consts.count(w) > 0 || declared_globals_.count(w) > 0;
    }
    static const std::unordered_set<std::string>& std_function_names() {
        static const std::unordered_set<std::string> s = {
            "move", "forward", "make_shared", "make_unique", "get", "numeric_limits", "make_pair", "make_tuple", "tie",
            "println", "print", "cout", "cerr", "endl", "format", "to_string", "stoi", "stol", "stoll", "stod", "stof",
            "abs", "min", "max", "clamp", "sqrt", "pow", "floor", "ceil", "round", "sort", "reverse", "find", "count",
            "accumulate", "swap", "getline", "cin", "async", "thread", "jthread", "boolalpha", "fixed", "setprecision",
            "dynamic_pointer_cast", "static_pointer_cast", "nullopt", "any_of", "all_of", "none_of", "transform", "iota",
            "runtime_error", "logic_error", "invalid_argument", "out_of_range", "exception", "overflow_error", "flush"};
        return s;
    }
    static std::string short_name(const std::string& c) { auto p = c.rfind("::"); return p == std::string::npos ? c : c.substr(p + 2); }
    bool derives_from(const std::string& cls, const std::string& base) const {
        for (std::string c = cls; !c.empty();) { if (c == base) return true; auto it = classes_.find(c); if (it == classes_.end()) return false; c = it->second.base; }
        return false;
    }
    // For Base::m() inside a derived method: Super resolves from the class just below `base` in the chain.
    std::string parent_before(const std::string& cls, const std::string& base) const {
        std::string prev = cls;
        for (std::string c = cls; !c.empty();) { auto it = classes_.find(c); if (it == classes_.end()) break; if (it->second.base == base) return c; c = it->second.base; }
        return prev;
    }
    NodeP cast_to(const TI& ti, NodeP e, int line) {
        switch (ti.k) {
        case TK::Int: case TK::Enum: return builtin_call("forge::__cast", {str_node("int", line), e}, line);
        case TK::Float: return builtin_call("forge::__cast", {str_node("float", line), e}, line);
        case TK::Bool: return builtin_call("forge::__cast", {str_node("bool", line), e}, line);
        case TK::String: return builtin_call("forge::__cast", {str_node("string", line), e}, line);
        default: return e;
        }
    }
    // T(args): a functional cast for scalars, a construction otherwise.
    NodeP construct_value(const TI& ti, std::vector<NodeP> args, int line) {
        if ((ti.k == TK::Int || ti.k == TK::Float || ti.k == TK::Bool || ti.k == TK::Enum) && args.size() == 1) return cast_to(ti, args[0], line);
        if (ti.k == TK::String && args.size() == 1) return cast_to(ti, args[0], line);
        if (ti.k == TK::Class && !ti.smart) {
            if (classes_.count(ti.name)) return call_node(var_node(ti.name, line), args, line);
            return builtin_call(ti.name, args, line);   // exceptions and other built-in classes
        }
        if (ti.k == TK::Any && !ti.name.empty()) return builtin_call(ti.name, args, line);
        return construct(ti, std::move(args), false, line);
    }
    // ---- limit blocks: `limit {opts} { body }` / `limit (opts_expr) { body }` (docs/scripting.md "Resource limits").
    // Sugar for forge::limit(opts, [&] { body }). opts entries are `{"key", value}` pairs or `.key = value`.
    bool limit_word(const std::string& w) const { return (w == "limit" || w == "forge::limit") && !is_local(w) && !types_.count(w); }
    // Is the token at index k a `{...}` or `(...)` group directly followed by `{`?
    bool limit_block_after(std::size_t k) const {
        if (k >= t_.size() || t_[k].t != T::Op || (t_[k].s != "{" && t_[k].s != "(")) return false;
        const std::string open = t_[k].s, close = open == "{" ? "}" : ")";
        int depth = 0;
        for (std::size_t j = k; j < t_.size() && t_[j].t != T::End; ++j) {
            if (t_[j].t != T::Op) continue;
            if (t_[j].s == open) ++depth;
            else if (t_[j].s == close && --depth == 0) return j + 1 < t_.size() && t_[j + 1].t == T::Op && t_[j + 1].s == "{";
        }
        return false;
    }
    bool limit_block_ahead() const { return at(T::Ident) && limit_word(canon_fn_name(cur().s)) && limit_block_after(i_ + 1); }
    // stmt: statement form (value discarded, `return` refused). taken: the `limit` word was already consumed.
    NodeP limit_block(int line, bool stmt, bool taken = false) {
        if (!taken) take();
        NodeP opts;
        if (accept_op("(")) { opts = expr(); expect_op(")"); }
        else {
            auto m = mk(NK::Map, line);
            std::set<std::string> keys;
            expect_op("{");
            while (!is_op("}")) {
                std::string k; NodeP v;
                if (accept_op(".")) { k = ident(); expect_op("="); v = copy_of(assign_expr()); }
                else {
                    expect_op("{");
                    if (!at(T::Str)) fail("a limit option name must be a string literal, e.g. {\"timeout\", 500}");
                    k = take().s; expect_op(","); v = copy_of(assign_expr()); expect_op("}");
                }
                static const std::set<std::string> known = {"timeout", "cpu_s", "mem", "proc_mem", "proc_cpu", "allocs", "handles", "procs"};
                if (!known.count(k)) fail_at(line, "unknown limit option '" + k + "' (timeout, cpu_s, mem, proc_mem, proc_cpu, allocs, handles, procs)");
                if (!keys.insert(k).second) fail_at(line, "limit option '" + k + "' given twice");
                m->kids.push_back(str_node(k, line)); m->kids.push_back(std::move(v));
                if (!accept_op(",")) break;
            }
            expect_op("}");
            opts = m;
        }
        auto fd = std::make_shared<FnDecl>(); fd->name = "<limit>"; fd->line = line; fd->file = file_;
        auto saved_brk = std::exchange(brk_, {});   // break / continue cannot leave the block
        push_fn("", false);
        fns_.back().cls.clear(); fns_.back().limit_stmt = stmt;
        TI r; r.k = TK::Auto; fns_.back().ret = std::make_shared<TI>(r);
        try { fd->body = block_body(line, false); } catch (...) { brk_ = std::move(saved_brk); throw; }
        brk_ = std::move(saved_brk);
        fd->nslots = fns_.back().nslots;
        pop_fn();
        auto lam = mk(NK::Lambda, line); lam->fn = fd;
        return builtin_call("forge::limit", {opts, lam}, line);
    }

    NodeP lambda() {
        int line = cur().line;
        expect_op("[");
        std::vector<std::pair<std::string, NodeP>> copies;   // by-value captures: name, initial value
        while (!is_op("]")) {
            if (accept_op("=") || accept_kw("this")) {}
            else if (is_op("*") && ahead(1).t == T::Kw && ahead(1).s == "this") { take(); take(); }
            else if (accept_op("&")) { if (at(T::Ident)) { std::string n = take().s; if (accept_op("=")) fail_at(line, "reference init-captures are not supported"); (void)n; } }
            else {
                std::string n = ident();
                accept_op("...");
                if (accept_op("=")) copies.emplace_back(n, copy_of(assign_expr()));
                else if (is_op("{")) { auto l = brace_list(); copies.emplace_back(n, l.empty() ? NodeP(mk(NK::Nil, line)) : l[0]); }
                else copies.emplace_back(n, copy_of(var_node(n, line)));
            }
            if (!accept_op(",")) break;
        }
        expect_op("]");
        std::size_t sb = cur().off;   // source after the capture list (forge::remote_call re-captures by name)
        if (is_op("<")) template_params();
        // copy captures: an immediately-invoked outer function binds the copies, the lambda closes over them
        std::shared_ptr<FnDecl> outer;
        if (!copies.empty()) {
            outer = std::make_shared<FnDecl>(); outer->name = "<captures>"; outer->line = line; outer->file = file_;
            push_fn(method_scope() ? method_scope()->cls : "", method_scope() ? method_scope()->is_static : false);
            fns_.back().cls.clear();
            for (auto& [n, v] : copies) { Param p; p.name = n; p.slot = declare(n, false).slot; outer->params.push_back(p); }
        }
        auto fd = std::make_shared<FnDecl>(); fd->name = "<lambda>"; fd->line = line; fd->file = file_;
        push_fn("", false);
        fns_.back().cls.clear();
        if (is_op("(")) {
            auto ps = params();
            for (auto& p : ps) { if (p.name == "...") p.name = "__va"; p.slot = declare(p.name, false).slot; }
            fd->params = std::move(ps);
        }
        TI r; r.k = TK::Auto;
        while (accept_kw("mutable") || accept_kw("constexpr") || accept_kw("static")) {}
        fn_qualifiers(&r);
        fns_.back().ret = std::make_shared<TI>(r);
        fd->body = block_body(line, false);
        fd->nslots = fns_.back().nslots;
        pop_fn();
        keep_source(*fd, sb);
        auto lam = mk(NK::Lambda, line); lam->fn = fd;
        if (!outer) return lam;
        auto body = mk(NK::Block, line); auto ret = mk(NK::Return, line); ret->kids.push_back(lam); body->kids.push_back(ret);
        outer->body = body; outer->nslots = fns_.back().nslots;
        pop_fn();
        auto ol = mk(NK::Lambda, line); ol->fn = outer;
        std::vector<NodeP> args; for (auto& [n, v] : copies) args.push_back(v);
        return call_node(ol, args, line);
    }

    // Source text of a function from byte `b` (its parameter list) to the end of the last token consumed.
    void keep_source(FnDecl& fd, std::size_t b) const {
        if (!src_ || i_ == 0) return;
        std::size_t e = t_[i_ - 1].end;
        if (b < e && e <= src_->size()) { fd.src = src_; fd.src_b = b; fd.src_e = e; }
    }

    // ---- state
    std::vector<Tok> t_; std::size_t i_ = 0; std::string file_; std::shared_ptr<const std::string> src_;
    std::vector<std::pair<std::size_t, std::string>> undo_;   // token splits made by close_angle(), for backtracking
    std::vector<FnScope> fns_;
    std::vector<NodeP> extra_, extra_after_class_;
    std::vector<char> brk_;
    std::vector<VarRef> catch_vars_;
    std::vector<std::pair<int, int>> saved_loops_;
    int loops_ = 0, switches_ = 0, hidden_n_ = 0;
    bool has_main_ = false, using_std_ = false;
    std::string ns_;
    std::unordered_set<std::string> types_, enums_, tparams_, declared_globals_;
    std::unordered_map<std::string, TI> typedefs_;
    std::unordered_map<std::string, std::string> aliases_, aliases_ns_;
    std::unordered_map<std::string, std::set<std::string>> ns_members_;
    std::vector<std::string> using_ns_;
    std::unordered_map<std::string, ClassInfo> classes_;
};

} // namespace

std::shared_ptr<const FnDecl> parse_program(std::string_view src, const std::string& file) {
    Lexer lx(src, file);
    Parser p(lx.run(), file, std::make_shared<const std::string>(src));
    return p.program();
}

} // namespace forge::script
