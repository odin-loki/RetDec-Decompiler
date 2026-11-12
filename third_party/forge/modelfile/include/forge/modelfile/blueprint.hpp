#pragma once
// Blueprint language: a small declarative graph language (FORGE_PLAN Part E.2).
//
//   blueprint "decoder-lm" opset 1
//   params { d_model: 64  n_layers: 2  rope: { base: 10000 } }
//   state kv[n_layers] : kv_cache(heads: n_kv, dim: head_dim)
//   input tokens : i32[seq]
//   graph {
//     x = embed(W.tok_emb, tokens)
//     repeat l in 0..n_layers { h = rms_norm(x, W.layer[l].attn_norm, eps)  ... }
//     logits = linear(x, W.lm_head)
//   }
//   output logits : f32[seq, vocab]
//   session generative_sequence
//
// Expressions: numbers, lists [a, b], params (dotted), arithmetic + - * /, calls with positional and
// keyword args, `W.<path>` weight references, `state[i].append(k, v)`, fixed-size state ops `state[i].conv(x, w)` and
// `state[i].delta(q, k, v, g, beta)`, tuple assignment `a, b = f(..)`.
// Any spec op is callable by name; positional numeric/list args bind to the op's declared attrs in order.
// Built-in composites: linear(x, W) = x @ W^T, embed(W, ids) = gather, heads(x, n) / merge(x) reshapes.
#include <forge/graph/ir.hpp>
#include <forge/ops/ops.gen.hpp>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace forge::modelfile {

struct bp_token { enum kind_t { k_ident, k_num, k_str, k_punct, k_end } kind; std::string text; double num = 0; int line = 0; };

inline std::vector<bp_token> bp_lex(std::string_view s) {
    std::vector<bp_token> t; int line = 1; std::size_t p = 0;
    while (p < s.size()) {
        char c = s[p];
        if (c == '\n') { ++line; ++p; continue; }
        if (std::isspace((unsigned char)c)) { ++p; continue; }
        if (c == '#' || (c == '/' && p + 1 < s.size() && s[p + 1] == '/')) { while (p < s.size() && s[p] != '\n') ++p; continue; }
        if (std::isalpha((unsigned char)c) || c == '_') {
            std::size_t q = p; while (q < s.size() && (std::isalnum((unsigned char)s[q]) || s[q] == '_')) ++q;
            t.push_back({bp_token::k_ident, std::string(s.substr(p, q - p)), 0, line}); p = q; continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && p + 1 < s.size() && std::isdigit((unsigned char)s[p + 1]) && !(p > 0 && s[p - 1] == '.'))) {
            std::size_t q = p;
            while (q < s.size() && (std::isdigit((unsigned char)s[q]) || (s[q] == '.' && !(q + 1 < s.size() && s[q + 1] == '.')) ||
                                    ((s[q] == 'e' || s[q] == 'E') && q + 1 < s.size() && (std::isdigit((unsigned char)s[q + 1]) || s[q + 1] == '-' || s[q + 1] == '+')) ||
                                    ((s[q] == '-' || s[q] == '+') && (s[q - 1] == 'e' || s[q - 1] == 'E'))))
                ++q;
            auto txt = std::string(s.substr(p, q - p));
            char* end = nullptr; double v = std::strtod(txt.c_str(), &end);
            if (end != txt.c_str() + txt.size() || !std::isfinite(v)) raise(errc::parse_error, "blueprint:{}: bad number '{}'", line, txt);
            t.push_back({bp_token::k_num, txt, v, line}); p = q; continue;
        }
        if (c == '"') {
            std::size_t q = p + 1; while (q < s.size() && s[q] != '"') ++q;
            if (q >= s.size()) raise(errc::parse_error, "blueprint:{}: unterminated string", line);
            t.push_back({bp_token::k_str, std::string(s.substr(p + 1, q - p - 1)), 0, line}); p = q + 1; continue;
        }
        if (c == '.' && p + 1 < s.size() && s[p + 1] == '.') { t.push_back({bp_token::k_punct, "..", 0, line}); p += 2; continue; }
        if (std::string_view("{}()[],:=.+-*/").find(c) != std::string_view::npos) { t.push_back({bp_token::k_punct, std::string(1, c), 0, line}); ++p; continue; }
        raise(errc::parse_error, "blueprint:{}: unexpected character '{}'", line, c);
    }
    t.push_back({bp_token::k_end, "<eof>", 0, line});
    return t;
}

struct bp_state_decl { std::string name; i64 count = 1; std::string kind; std::map<std::string, double> args; };
struct bp_io_decl { std::string name, dtype; std::vector<std::string> dims; };

struct blueprint {
    std::string name;
    int opset = 1;
    std::map<std::string, double> params;          // dotted keys: "rope.base"
    std::map<std::string, std::string> str_params;
    std::vector<bp_state_decl> states;
    std::vector<bp_io_decl> inputs, outputs;
    std::string session = "one_shot";
    std::vector<bp_token> toks;
    std::size_t graph_begin = 0, graph_end = 0;     // token range of the graph body
    // Composite ops: `op name(a, b) { t = ...  return expr }`. A spec op of the same name wins when the
    // engine has one, so files keep working on old engines and pick up fused kernels on new ones.
    struct composite { std::vector<std::string> args; std::size_t body_begin = 0, body_end = 0; };
    std::map<std::string, composite> composites;
    double param(const std::string& k) const {
        auto it = params.find(k);
        if (it == params.end()) raise(errc::not_found, "blueprint '{}': no param '{}'", name, k);
        return it->second;
    }
    double param_or(const std::string& k, double d) const { auto it = params.find(k); return it == params.end() ? d : it->second; }
};

struct build_options {
    std::map<std::string, i64> dims;  // symbolic dims, e.g. {"seq", 7}
    bool all_rows = false;            // `last(x, n)` keeps every row (speculative verification, scoring)
    // resolves a weight name (e.g. "layer.0.attn_norm") to shape + f32 data
    std::function<std::pair<shape_t, const float*>(const std::string&)> weight;
    // optional: resolves a weight to its encoded bytes + codec name instead (null data = use `weight`)
    struct encoded { shape_t shape; const std::uint8_t* data = nullptr; std::string codec; };
    std::function<encoded(const std::string&)> qweight;
};

namespace detail {

struct bp_val {
    enum kind_t { none, num, list, tensor, tuple, wpath, state } k = none;
    double n = 0;
    std::vector<double> l;
    graph::value_id v = graph::no_value;
    std::vector<graph::value_id> tup;
    std::string path;  // weight path or state name
};

class bp_parser {
public:
    bp_parser(const std::vector<bp_token>& t, std::size_t p = 0) : t_(t), p_(p) {}
    const bp_token& cur() const { return t_[p_]; }
    const bp_token& next() { const auto& t = t_[p_]; if (t.kind != bp_token::k_end) ++p_; return t; }
    double number() { if (cur().kind != bp_token::k_num) fail("expected number"); return next().num; }
    bool is(std::string_view s) const { return (cur().kind == bp_token::k_punct || cur().kind == bp_token::k_ident) && cur().text == s; }
    bool accept(std::string_view s) { if (is(s)) { ++p_; return true; } return false; }
    [[noreturn]] void fail(const std::string& m) const { raise(errc::parse_error, "blueprint:{}: {} (near '{}')", cur().line, m, cur().text); }
    void expect(std::string_view s) { if (!accept(s)) fail(std::format("expected '{}'", s)); }
    std::string ident() { if (cur().kind != bp_token::k_ident) fail("expected identifier"); return next().text; }
    std::size_t pos() const { return p_; }
    void seek(std::size_t p) { p_ = p; }
    void skip_block() {  // assumes current token is '{'
        int depth = 0;
        do { if (is("{")) ++depth; else if (is("}")) --depth; else if (cur().kind == bp_token::k_end) fail("unbalanced braces"); ++p_; } while (depth > 0);
    }
private:
    const std::vector<bp_token>& t_;
    std::size_t p_;
};

class bp_builder {
public:
    bp_builder(const blueprint& bp, const build_options& o, graph::tensor_graph& g) : bp_(bp), o_(o), g_(g), ps_(bp.toks, bp.graph_begin) {}

    void run() {
        for (auto& in : bp_.inputs) {
            shape_t s; for (auto& d : in.dims) s.push_back(dim(d));
            bp_val v; v.k = bp_val::tensor; v.v = g_.input(in.name, s); env_[in.name] = v;
        }
        for (auto& st : bp_.states) { bp_val v; v.k = bp_val::state; v.path = st.name; env_[st.name] = v; }
        while (ps_.pos() < bp_.graph_end) stmt();
        for (auto& out : bp_.outputs) {
            auto it = env_.find(out.name);
            if (it == env_.end() || it->second.k != bp_val::tensor) raise(errc::invalid_argument, "blueprint: output '{}' never assigned", out.name);
            g_.output(out.name, it->second.v);
        }
    }

private:
    i64 dim(const std::string& d) {
        if (auto it = o_.dims.find(d); it != o_.dims.end()) return it->second;
        if (auto it = bp_.params.find(d); it != bp_.params.end()) return i64(it->second);
        if (!d.empty() && std::isdigit((unsigned char)d[0])) {
            i64 v = 0; auto [e, ec] = std::from_chars(d.data(), d.data() + d.size(), v);
            if (ec == std::errc() && e == d.data() + d.size()) return v;
        }
        raise(errc::invalid_argument, "blueprint: unbound dimension '{}'", d);
    }
    void stmt() {
        if (ps_.accept("repeat")) {
            auto var = ps_.ident(); ps_.expect("in");
            auto lo = expect_num(expr()); ps_.expect(".."); auto hi = expect_num(expr());
            if (!ps_.is("{")) ps_.fail("expected '{' after repeat range");
            std::size_t body = ps_.pos();
            ps_.skip_block();
            std::size_t after = ps_.pos();
            auto saved = env_.count(var) ? std::optional<bp_val>(env_[var]) : std::nullopt;
            for (i64 i = i64(lo); i < i64(hi); ++i) {
                bp_val iv; iv.k = bp_val::num; iv.n = double(i); env_[var] = iv;
                ps_.seek(body + 1);
                while (!ps_.is("}")) stmt();
            }
            if (saved) env_[var] = *saved; else env_.erase(var);
            ps_.seek(after);
            return;
        }
        std::vector<std::string> names{ps_.ident()};
        while (ps_.accept(",")) names.push_back(ps_.ident());
        ps_.expect("=");
        auto v = expr();
        if (names.size() == 1) { env_[names[0]] = v; return; }
        if (v.k != bp_val::tuple || v.tup.size() != names.size())
            ps_.fail(std::format("cannot unpack into {} names", names.size()));
        for (std::size_t i = 0; i < names.size(); ++i) { bp_val t; t.k = bp_val::tensor; t.v = v.tup[i]; env_[names[i]] = t; }
    }
    double expect_num(const bp_val& v) { if (v.k != bp_val::num) ps_.fail("expected a number"); return v.n; }
    graph::value_id as_tensor(const bp_val& v) {
        if (v.k == bp_val::tensor) return v.v;
        if (v.k == bp_val::num) return g_.scalar(float(v.n));
        if (v.k == bp_val::wpath) return weight(v.path);
        ps_.fail("expected a tensor");
    }
    graph::value_id weight(const std::string& name) {
        if (auto it = wcache_.find(name); it != wcache_.end()) return it->second;
        if (!o_.weight) raise(errc::invalid_argument, "blueprint: no weight resolver for '{}'", name);
        if (o_.qweight) if (auto e = o_.qweight(name); e.data) return wcache_[name] = g_.qweight(name, e.shape, e.data, e.codec);
        auto [s, p] = o_.weight(name);
        return wcache_[name] = g_.weight(name, s, p);
    }
    bp_val binop(char op, bp_val a, bp_val b) {
        if (a.k == bp_val::num && b.k == bp_val::num) {
            bp_val r; r.k = bp_val::num;
            r.n = op == '+' ? a.n + b.n : op == '-' ? a.n - b.n : op == '*' ? a.n * b.n : a.n / b.n; return r;
        }
        if (a.k == bp_val::list && b.k == bp_val::num && op == '*') { for (auto& x : a.l) x *= b.n; return a; }
        auto x = as_tensor(a), y = as_tensor(b);
        ops::op o = op == '+' ? ops::op::add : op == '-' ? ops::op::sub : op == '*' ? ops::op::mul : ops::op::div;
        bp_val r; r.k = bp_val::tensor; r.v = g_.apply(o, {x, y}); return r;
    }
    bp_val expr() {
        auto a = term();
        while (ps_.is("+") || ps_.is("-")) { char op = ps_.next().text[0]; a = binop(op, a, term()); }
        return a;
    }
    bp_val term() {
        auto a = unary();
        while (ps_.is("*") || ps_.is("/")) { char op = ps_.next().text[0]; a = binop(op, a, unary()); }
        return a;
    }
    bp_val unary() {
        if (ps_.accept("-")) {
            auto v = unary();
            if (v.k == bp_val::num) { v.n = -v.n; return v; }
            bp_val r; r.k = bp_val::tensor; r.v = g_.apply(ops::op::neg, {as_tensor(v)}); return r;
        }
        return postfix(primary());
    }
    bp_val postfix(bp_val v) {
        while (true) {
            if (ps_.is(".") ) {
                ps_.next(); auto m = ps_.ident();
                if (v.k == bp_val::wpath) { v.path += (v.path.empty() ? "" : ".") + m; continue; }
                if (v.k == bp_val::state && m == "append") {
                    ps_.expect("(");
                    std::vector<graph::value_id> xs;
                    if (!ps_.is(")")) { xs.push_back(as_tensor(expr())); while (ps_.accept(",")) xs.push_back(as_tensor(expr())); }
                    ps_.expect(")");
                    static const char* sfx[] = {"k", "v", "s2", "s3"};
                    bp_val r; r.k = xs.size() == 1 ? bp_val::tensor : bp_val::tuple;
                    for (std::size_t i = 0; i < xs.size(); ++i) {
                        auto id = g_.state_append(v.path + (xs.size() == 1 ? "" : std::string(".") + sfx[i]), xs[i]);
                        if (xs.size() == 1) r.v = id; else r.tup.push_back(id);
                    }
                    v = r; continue;
                }
                if (v.k == bp_val::state && (m == "conv" || m == "delta")) {   // fixed-size state ops (state_ops.hpp)
                    ps_.expect("(");
                    std::vector<graph::value_id> xs;
                    if (!ps_.is(")")) { xs.push_back(as_tensor(expr())); while (ps_.accept(",")) xs.push_back(as_tensor(expr())); }
                    ps_.expect(")");
                    if (xs.size() != (m == "conv" ? 2u : 5u)) ps_.fail(m == "conv" ? "conv(x, w) takes 2 tensors" : "delta(q, k, v, g, beta) takes 5 tensors");
                    bp_val r; r.k = bp_val::tensor; r.v = g_.state_op(v.path, m == "conv" ? "causal_conv" : "delta_rule", xs);
                    v = r; continue;
                }
                ps_.fail("unknown member '" + m + "'");
            }
            if (ps_.is("[") && (v.k == bp_val::wpath || v.k == bp_val::state)) {
                ps_.next(); auto i = expect_num(expr()); ps_.expect("]");
                v.path += "." + std::to_string(i64(i)); continue;
            }
            return v;
        }
    }
    bp_val primary() {
        const auto& t = ps_.cur();
        if (t.kind == bp_token::k_num) { ps_.next(); bp_val v; v.k = bp_val::num; v.n = t.num; return v; }
        if (ps_.accept("(")) { auto v = expr(); ps_.expect(")"); return v; }
        if (ps_.accept("[")) {
            bp_val v; v.k = bp_val::list;
            if (!ps_.is("]")) { v.l.push_back(expect_num(expr())); while (ps_.accept(",")) v.l.push_back(expect_num(expr())); }
            ps_.expect("]"); return v;
        }
        auto id = ps_.ident();
        if (id == "true" || id == "false") { bp_val v; v.k = bp_val::num; v.n = id == "true"; return v; }
        if (id == "W") { bp_val v; v.k = bp_val::wpath; return v; }
        if (ps_.is("(")) return call(id);
        if (auto it = env_.find(id); it != env_.end()) return it->second;
        // dotted params: rope.base
        std::string key = id;
        while (ps_.is(".") && !bp_.params.count(key)) {
            auto save = ps_.pos(); ps_.next();
            if (ps_.cur().kind != bp_token::k_ident) { ps_.seek(save); break; }
            key += "." + ps_.ident();
        }
        if (auto it = bp_.params.find(key); it != bp_.params.end()) { bp_val v; v.k = bp_val::num; v.n = it->second; return v; }
        if (auto it = o_.dims.find(key); it != o_.dims.end()) { bp_val v; v.k = bp_val::num; v.n = double(it->second); return v; }
        ps_.fail("undefined name '" + key + "'");
    }
    bp_val call(const std::string& fn) {
        int line = ps_.cur().line;
        ps_.expect("(");
        std::vector<bp_val> pos; std::vector<std::pair<std::string, bp_val>> kw;
        while (!ps_.is(")")) {
            if (ps_.cur().kind == bp_token::k_ident && ps_.pos() + 1 < bp_.graph_end && bp_.toks[ps_.pos() + 1].text == ":") {
                auto k = ps_.ident(); ps_.expect(":"); kw.emplace_back(k, expr());
            } else pos.push_back(expr());
            if (!ps_.accept(",")) break;
        }
        ps_.expect(")");
        auto T = [&](graph::value_id id) { bp_val r; r.k = bp_val::tensor; r.v = id; return r; };
        // built-in composites
        if (fn == "linear") return T(g_.apply(ops::op::matmul_t, {as_tensor(pos.at(0)), as_tensor(pos.at(1))}));
        if (fn == "embed") return T(g_.apply(ops::op::gather, {as_tensor(pos.at(0)), as_tensor(pos.at(1))}));
        if (fn == "heads") return T(g_.apply(ops::op::reshape, {as_tensor(pos.at(0))},
                                             {{"shape", std::vector<i64>{-1, i64(expect_num(pos.at(1))), i64(expect_num(pos.at(2)))}}}));
        if (fn == "merge") return T(g_.apply(ops::op::reshape, {as_tensor(pos.at(0))}, {{"shape", std::vector<i64>{-1, i64(expect_num(pos.at(1)))}}}));
        if (fn == "last") {  // last row along axis 0 (needs the symbolic length)
            i64 n = i64(expect_num(pos.at(1)));
            if (o_.all_rows) return pos.at(0);
            return T(g_.apply(ops::op::slice, {as_tensor(pos.at(0))}, {{"axis", 0.0}, {"start", double(n - 1)}, {"end", double(n)}}));
        }
        auto o = ops::op_from_name(fn);
        if (!o) {
            auto cit = bp_.composites.find(fn);
            if (cit == bp_.composites.end()) raise(errc::parse_error, "blueprint:{}: unknown function '{}'", line, fn);
            return expand_composite(fn, cit->second, pos, line);
        }
        const auto& inf = ops::info(*o);
        std::vector<std::string> anames;
        for (std::size_t s = 0, e; s < inf.attrs.size(); s = e + 1) {
            e = inf.attrs.find(',', s); if (e == std::string_view::npos) e = inf.attrs.size();
            anames.emplace_back(inf.attrs.substr(s, e - s));
        }
        std::vector<graph::value_id> ins; ops::attrs a; std::size_t ai = 0;
        for (auto& p : pos) {
            if (int(ins.size()) < inf.inputs && (p.k == bp_val::tensor || p.k == bp_val::wpath || (p.k == bp_val::num && ai == 0 && anames.empty())))
                ins.push_back(as_tensor(p));
            else {
                if (ai >= anames.size()) ps_.fail(std::format("too many arguments to {}", fn));
                if (p.k == bp_val::list) { std::vector<i64> l; for (auto x : p.l) l.push_back(i64(x)); a.set(anames[ai++], l); }
                else a.set(anames[ai++], expect_num(p));
            }
        }
        for (auto& [k, v] : kw) {
            if (v.k == bp_val::list) { std::vector<i64> l; for (auto x : v.l) l.push_back(i64(x)); a.set(k, l); }
            else a.set(k, expect_num(v));
        }
        if (int(ins.size()) != inf.inputs) ps_.fail(std::format("{} expects {} tensor inputs, got {}", fn, inf.inputs, ins.size()));
        return T(g_.apply(*o, ins, a));
    }

    bp_val expand_composite(const std::string& fn, const blueprint::composite& c, const std::vector<bp_val>& pos, int line) {
        if (pos.size() != c.args.size()) raise(errc::parse_error, "blueprint:{}: {} expects {} arguments, got {}", line, fn, c.args.size(), pos.size());
        if (++expansions_ > 100000) raise(errc::parse_error, "blueprint:{}: too many composite expansions", line);
        if (++depth_ > 64) raise(errc::parse_error, "blueprint:{}: composite '{}' recursion too deep", line, fn);
        auto saved_env = env_; auto back = ps_.pos();
        for (std::size_t i = 0; i < pos.size(); ++i) env_[c.args[i]] = pos[i];   // lexical: args shadow outer names
        ps_.seek(c.body_begin);
        bp_val r; bool got = false;
        while (ps_.pos() < c.body_end) {
            if (ps_.accept("return")) { r = expr(); got = true; break; }
            stmt();
        }
        if (!got) raise(errc::parse_error, "blueprint:{}: composite '{}' has no return", line, fn);
        env_ = std::move(saved_env); ps_.seek(back); --depth_;
        return r;
    }

    int depth_ = 0; std::size_t expansions_ = 0;
    const blueprint& bp_;
    const build_options& o_;
    graph::tensor_graph& g_;
    bp_parser ps_;
    std::map<std::string, bp_val> env_;
    std::map<std::string, graph::value_id> wcache_;
};

inline void parse_params(bp_parser& ps, blueprint& bp, const std::string& prefix) {
    ps.expect("{");
    while (!ps.accept("}")) {
        auto k = prefix + ps.ident(); ps.expect(":");
        if (ps.is("{")) { parse_params(ps, bp, k + "."); continue; }
        bool neg = ps.accept("-");
        auto& t = ps.cur();
        if (t.kind == bp_token::k_num) { bp.params[k] = neg ? -t.num : t.num; ps.next(); }
        else if (t.kind == bp_token::k_str) { bp.str_params[k] = t.text; ps.next(); }
        else if (t.kind == bp_token::k_ident) {
            auto v = ps.ident();
            if (v == "true" || v == "false") { bp.params[k] = v == "true"; }
            else if (ps.is("(")) {  // e.g. yarn(factor: 8, orig_ctx: 8192) -> k = "yarn", k.factor = 8
                bp.str_params[k] = v; ps.next();
                while (!ps.accept(")")) { auto a = ps.ident(); ps.expect(":"); bp.params[k + "." + a] = ps.number(); ps.accept(","); }
            } else bp.str_params[k] = v;
        } else ps.fail("bad param value");
        ps.accept(",");
    }
}

inline bp_io_decl parse_io(bp_parser& ps) {
    bp_io_decl d; d.name = ps.ident(); ps.expect(":"); d.dtype = ps.ident(); ps.expect("[");
    while (!ps.accept("]")) {
        if (ps.cur().kind == bp_token::k_num) d.dims.push_back(std::to_string(i64(ps.next().num)));
        else d.dims.push_back(ps.ident());
        ps.accept(",");
    }
    return d;
}

} // namespace detail

inline blueprint parse_blueprint(std::string_view src) {
    blueprint bp; bp.toks = bp_lex(src);
    detail::bp_parser ps(bp.toks);
    ps.expect("blueprint");
    if (ps.cur().kind != bp_token::k_str) ps.fail("expected blueprint name string");
    bp.name = ps.next().text;
    if (ps.accept("opset")) bp.opset = int(ps.number());
    bool have_graph = false;
    while (ps.cur().kind != bp_token::k_end) {
        if (ps.accept("params")) detail::parse_params(ps, bp, "");
        else if (ps.accept("state")) {
            bp_state_decl s; s.name = ps.ident();
            std::string count_name;
            if (ps.accept("[")) {
                if (ps.cur().kind == bp_token::k_num) s.count = i64(ps.next().num);
                else { auto n = ps.ident(); s.count = i64(bp.param(n)); }
                ps.expect("]");
            }
            ps.expect(":"); s.kind = ps.ident();
            if (ps.accept("(")) {
                while (!ps.accept(")")) {
                    auto a = ps.ident(); ps.expect(":");
                    if (ps.cur().kind == bp_token::k_num) s.args[a] = ps.next().num;
                    else { auto v = ps.ident(); if (bp.params.count(v)) s.args[a] = bp.params[v]; }
                    ps.accept(",");
                }
            }
            bp.states.push_back(s);
        } else if (ps.accept("input")) bp.inputs.push_back(detail::parse_io(ps));
        else if (ps.accept("output")) bp.outputs.push_back(detail::parse_io(ps));
        else if (ps.accept("session")) bp.session = ps.ident();
        else if (ps.accept("op")) {
            auto nm = ps.ident(); blueprint::composite c; ps.expect("(");
            while (!ps.accept(")")) { c.args.push_back(ps.ident()); ps.accept(","); }
            if (!ps.is("{")) ps.fail("expected '{' after op signature");
            c.body_begin = ps.pos() + 1; ps.skip_block(); c.body_end = ps.pos() - 1;
            if (!bp.composites.emplace(nm, c).second) ps.fail("op '" + nm + "' defined twice");
        }
        else if (ps.accept("graph")) {
            if (!ps.is("{")) ps.fail("expected '{'");
            bp.graph_begin = ps.pos() + 1;
            ps.skip_block();
            bp.graph_end = ps.pos() - 1;
            have_graph = true;
        } else ps.fail("expected params/state/input/graph/output/session/op");
    }
    if (!have_graph) raise(errc::parse_error, "blueprint '{}': no graph block", bp.name);
    if (bp.outputs.empty()) raise(errc::parse_error, "blueprint '{}': no outputs declared", bp.name);
    return bp;
}

inline graph::tensor_graph build_graph(const blueprint& bp, const build_options& o) {
    graph::tensor_graph g;
    detail::bp_builder b(bp, o, g);
    b.run();
    return g;
}

} // namespace forge::modelfile
