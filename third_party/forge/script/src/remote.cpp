// forge::remote(addr, source[, opts]) -> {ok, output, error_type, error}
// Runs a FORGE script on a `forge rpc-serve --allow-scripts` node (docs/rpc.md). The connection is authenticated
// (token or Ed25519 key) and optionally TLS; credentials come from opts or the FORGE_RPC_* environment.
//   opts: {"args", list} {"timeout_ms", n} {"token", s} {"key", secret-key-file} {"tls", bool} {"ca", file}
//         {"fingerprint", sha256-hex} {"server_name", s} {"tls_cert", file} {"tls_key", file} (mTLS client certificate)
//         {"name", script-name}
//
// forge::remote_call(addr, fn[, args-list[, opts]]) -> fn(args...) evaluated on the node
// Ships a script function (lambda or free function) as source plus the values it captures, runs it through the
// same remote-script path (so the node's sandbox, timeout and memory cap apply) and returns its result. Captured
// values and arguments must be plain data (nil, bool, numbers, strings, lists, maps); captured script functions
// are shipped the same way. Handles, objects, classes, tasks and natives are refused with a TypeError. Errors
// raised by the function come back with their type; its print output is replayed locally.
// Values travel in a typed encoding (forge::__marshal / forge::__unmarshal) so 2 and 2.0 stay distinct.
#include "interp.hpp"
#include "ast.hpp"
#include <forge/graph/rpc.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <map>

namespace forge::script {
namespace {
using Args = std::vector<Value>;
Value strv(std::string s) { return obj(make<String>(std::move(s))); }
const Value* kv(const Args& a, std::size_t i, const std::string& k) {
    if (i >= a.size()) return nullptr; const Value* r = nullptr;
    if (auto* m = as<Map>(a[i])) r = m->find_str(k);
    else if (auto* l = as<List>(a[i])) for (auto& e : l->items) if (auto* p = as<List>(e); p && p->items.size() == 2) if (auto* s = as<String>(p->items[0]); s && s->s == k) r = &p->items[1];
    return r && !is_nil(*r) ? r : nullptr;
}

// ---- connection from (addr, opts)
struct Conn { std::shared_ptr<graph::rpc::client> c; std::vector<std::string> argv; std::uint32_t tmo = 0; std::string name = "<remote>", sig; };
Conn connect(Interp& in, const std::string& what, std::string addr, const Args& a, std::size_t oi) {
    if (!in.caps.net) in.raise("PermissionError", "network access is disabled in this sandbox");
    if (addr.starts_with("rpc:")) addr = addr.substr(4);
    auto colon = addr.rfind(':'); if (colon == std::string::npos) in.raise("ValueError", what + ": want host:port");
    auto host = addr.substr(0, colon), port = addr.substr(colon + 1);
    if (host.size() > 1 && host.front() == '[') host = host.substr(1, host.size() - 2);
    enforce_scope(in, SecScope::net_connect, (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + port);
    graph::rpc::client_security sec;
    try { sec = graph::rpc::client::env_security(); } catch (const std::exception& e) { in.raise("ValueError", what + ": " + e.what()); }
    if (auto* v = kv(a, oi, "token")) sec.token = in.str(*v);
    try { if (auto* v = kv(a, oi, "key")) sec.key = std::make_shared<graph::rpc::identity>(graph::rpc::identity_from_seed_hex(graph::rpc::read_key_file(in.str(*v)))); }
    catch (const std::exception& e) { in.raise("ValueError", what + ": " + e.what()); }
    if (auto* v = kv(a, oi, "ca")) { sec.tls.ca_file = in.str(*v); sec.tls.enabled = true; }
    if (auto* v = kv(a, oi, "fingerprint")) { sec.tls.fingerprint_sha256 = in.str(*v); sec.tls.enabled = true; }
    if (auto* v = kv(a, oi, "server_name")) sec.tls.server_name = in.str(*v);
    if (auto* v = kv(a, oi, "tls_cert")) { sec.tls.cert_file = in.str(*v); sec.tls.enabled = true; }
    if (auto* v = kv(a, oi, "tls_key")) sec.tls.key_file = in.str(*v);
    if (auto* v = kv(a, oi, "tls")) sec.tls.enabled = Interp::truthy(*v);
    Conn c;
    if (auto* v = kv(a, oi, "args")) if (auto* l = as<List>(*v)) for (auto& x : l->items) c.argv.push_back(in.str(x));
    if (auto* v = kv(a, oi, "timeout_ms")) if (auto* i = std::get_if<std::int64_t>(v)) c.tmo = std::uint32_t(std::max<std::int64_t>(0, *i));
    if (auto* v = kv(a, oi, "name")) c.name = in.str(*v);
    if (auto* v = kv(a, oi, "signature")) c.sig = in.str(*v);   // detached .sig text for `name` (FSS INT-2)
    std::string why;
    c.c = graph::rpc::client::connect(host, port, &why, sec);
    if (!c.c) in.raise(why.find("auth") != std::string::npos ? "PermissionError" : "IOError", why);
    return c;
}
graph::rpc::client::script_reply run_remote(Interp& in, Conn& c, const std::string& src) {
    try { return c.c->run_script(src, c.name, c.argv, c.tmo, c.sig); }
    catch (const std::exception& e) { std::string m = e.what(); in.raise(m.find("disabled") != std::string::npos || m.find("permission denied") != std::string::npos ? "PermissionError" : "IOError", m); }
}

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

// ---- typed value encoding: n t f i<int>; d<hexfloat>; s<len>:<bytes> l<n>:<items> m<flags><n>:[dflt]<k v>...
const char* plain_hint = "only nil, bool, numbers, strings, lists and maps (and script functions over them) can be sent to a remote node";
void marshal(Interp& in, const Value& v, std::string& o, const std::string& what, int depth = 0) {
    if (depth > 256) in.raise("ValueError", what + ": nesting too deep (cycle?)");
    if (is_nil(v)) { o += 'n'; return; }
    if (auto* b = std::get_if<bool>(&v)) { o += *b ? 't' : 'f'; return; }
    if (auto* i = std::get_if<std::int64_t>(&v)) { o += 'i'; o += std::to_string(*i); o += ';'; return; }
    if (auto* d = std::get_if<double>(&v)) {
        char buf[64]; auto r = std::to_chars(buf, buf + sizeof buf, *d, std::chars_format::hex);
        o += 'd'; o.append(buf, r.ptr); o += ';'; return;
    }
    if (auto* s = as<String>(v)) { o += 's'; o += std::to_string(s->s.size()); o += ':'; o += s->s; return; }
    if (auto* l = as<List>(v)) { o += 'l'; o += std::to_string(l->items.size()); o += ':'; for (auto& x : l->items) marshal(in, x, o, what, depth + 1); return; }
    if (auto* m = as<Map>(v)) {
        bool dflt = m->has_dflt && !is_nil(m->dflt);   // std::map default: a factory; send one value it makes
        o += 'm'; o += char('0' + (m->sorted ? 1 : 0) + (m->is_set ? 2 : 0) + (dflt ? 4 : 0));
        o += std::to_string(m->entries.size()); o += ':';
        if (dflt) { std::vector<Value> none; marshal(in, in.call(m->dflt, none), o, what, depth + 1); }
        for (auto& [k, x] : m->entries) { marshal(in, k, o, what, depth + 1); marshal(in, x, o, what, depth + 1); }
        return;
    }
    in.raise("TypeError", what + " is a " + Interp::type_of(v) + "; " + plain_hint);
}
struct Unmarshal {
    Interp& in; std::string_view s; std::size_t p = 0;
    [[noreturn]] void bad() { in.raise("ValueError", "forge::__unmarshal: malformed value"); }
    std::string_view upto(char c) { auto e = s.find(c, p); if (e == std::string_view::npos) bad(); auto r = s.substr(p, e - p); p = e + 1; return r; }
    std::size_t count() { auto t = upto(':'); std::size_t n = 0; auto r = std::from_chars(t.data(), t.data() + t.size(), n); if (r.ec != std::errc() || r.ptr != t.data() + t.size() || n > s.size()) bad(); return n; }
    Value val(int depth = 0) {
        if (depth > 256 || p >= s.size()) bad();
        switch (s[p++]) {
        case 'n': return Nil{};
        case 't': return true;
        case 'f': return false;
        case 'i': { auto t = upto(';'); std::int64_t x = 0; auto r = std::from_chars(t.data(), t.data() + t.size(), x); if (r.ec != std::errc() || r.ptr != t.data() + t.size()) bad(); return x; }
        case 'd': { auto t = upto(';'); double x = 0; if (!parse_double(t, x, true)) bad(); return x; }
        case 's': { auto n = count(); if (s.size() - p < n) bad(); auto r = strv(std::string(s.substr(p, n))); p += n; return r; }
        case 'l': { auto n = count(); auto l = make<List>(); for (std::size_t i = 0; i < n; ++i) l->push(val(depth + 1)); return obj(l); }
        case 'm': {
            if (p >= s.size()) bad();
            int fl = s[p++] - '0'; if (fl < 0 || fl > 7) bad();
            auto n = count(); auto m = make<Map>(); m->sorted = fl & 1; m->is_set = fl & 2;
            if (fl & 4) {   // rebuild the default factory: each call decodes a fresh copy
                auto b = p; (void)val(depth + 1); std::string enc(s.substr(b, p - b));
                auto f = make<Native>("<map default>", [enc](Interp& in2, std::vector<Value>&) -> Value { Unmarshal u{in2, enc}; return u.val(); }, 0, 0);
                f->frozen = true; m->has_dflt = true; m->dflt = obj(f);
            }
            for (std::size_t i = 0; i < n; ++i) { Value k = val(depth + 1); m->set(k, val(depth + 1)); }
            return obj(m);
        }
        default: bad();
        }
    }
};
std::string hex(std::string_view s) { static const char* d = "0123456789abcdef"; std::string o; o.reserve(s.size() * 2); for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; } return o; }
bool unhex(std::string_view h, std::string& o) {
    if (h.size() % 2) return false;
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    o.clear(); o.reserve(h.size() / 2);
    for (std::size_t i = 0; i < h.size(); i += 2) { int a = nib(h[i]), b = nib(h[i + 1]); if (a < 0 || b < 0) return false; o += char(a * 16 + b); }
    return true;
}

// ---- function shipping: source + free variables -> a standalone program
constexpr const char* kResultMark = "\x1e" "FORGE-REMOTE-RESULT:";
struct Shipper {
    Interp& in;
    std::map<std::string, const void*> bound;                  // name -> identity (object pointer, or the Value for data)
    std::map<const Obj*, std::string> fn_names;                // function -> first name it was emitted under
    std::vector<std::string> data, fns, aliases;
    std::map<std::string, std::string> data_enc;               // data name -> its encoding

    static std::string source_of(const FnDecl& d) { return d.src && d.src_b < d.src_e && d.src_e <= d.src->size() ? d.src->substr(d.src_b, d.src_e - d.src_b) : std::string(); }

    // Free variables of a function body: (name, hops above the function's captured env, slot) or globals (hop -1).
    struct Free { std::string name; int hop; int slot; };
    void walk(const Node& n, int level, bool in_class, std::vector<Free>& out) {
        if (n.k == NK::Self && !in_class) in.raise("TypeError", "forge::remote_call: the function uses 'this'; object members cannot be sent to a remote node");
        if (!n.var.name.empty()) {
            if (n.var.global()) out.push_back({n.var.name, -1, -1});
            else if (n.var.depth > level) out.push_back({n.var.name, n.var.depth - level - 1, n.var.slot});
        }
        for (auto& k : n.kids) if (k) walk(*k, level, in_class, out);
        if (n.extra) walk(*n.extra, level, in_class, out);
        if (n.fn) walk_fn(*n.fn, level + 1, in_class, out);
        if (n.cls) {
            for (auto& [nm, d] : n.cls->fields) if (d) walk(*d, level + 1, true, out);
            for (auto& m : n.cls->methods) walk_fn(*m, level + 1, true, out);
        }
    }
    void walk_fn(const FnDecl& f, int level, bool in_class, std::vector<Free>& out) {
        for (auto& p : f.params) if (p.dflt) walk(*p.dflt, level, in_class, out);
        if (f.body) walk(*f.body, level, in_class, out);
    }

    void claim(const std::string& name, const void* id) {
        auto [it, fresh] = bound.emplace(name, id);
        if (!fresh && it->second != id) in.raise("ValueError", "forge::remote_call: two different values are captured under the name '" + name + "'");
    }
    // Emit `name` bound to v on the remote side.
    void bind(const std::string& name, const Value& v) {
        if (name.find("::") != std::string::npos) in.raise("TypeError", "forge::remote_call: '" + name + "' is namespaced; only unqualified names can be sent to a remote node");
        if (auto* f = as<Function>(v)) { function(name, *f); return; }
        std::string enc; marshal(in, v, enc, "forge::remote_call: captured '" + name + "'");
        if (auto it = data_enc.find(name); it != data_enc.end()) {   // the same name seen again (e.g. from two functions)
            if (it->second != enc) in.raise("ValueError", "forge::remote_call: two different values are captured under the name '" + name + "'");
            return;
        }
        claim(name, &data_enc);   // data names: a function under the same name is a conflict
        data_enc[name] = enc;
        data.push_back("auto " + name + " = forge::__unmarshal(\"" + hex(enc) + "\");\n");
    }
    void function(const std::string& name, const Function& f) {
        if (auto it = fn_names.find(&f); it != fn_names.end()) {
            claim(name, &f);
            if (it->second != name && !std::count(aliases.begin(), aliases.end(), "auto " + name + " = " + it->second + ";\n")) aliases.push_back("auto " + name + " = " + it->second + ";\n");
            return;
        }
        claim(name, &f);
        fn_names[&f] = name;
        const FnDecl& d = *f.decl;
        if (d.is_method || f.owner) in.raise("TypeError", "forge::remote_call: '" + name + "' is a method; only free functions and lambdas can be sent to a remote node");
        auto src = source_of(d);
        if (src.empty()) in.raise("ValueError", "forge::remote_call: the source of '" + name + "' is not available");
        std::vector<Free> free; walk_fn(d, 0, false, free);
        for (auto& v : free) {
            if (v.hop < 0) { global(v.name, f); continue; }
            Env* e = f.env.get();
            for (int h = v.hop; h > 0 && e; --h) e = e->parent.get();
            if (!e || v.slot < 0 || std::size_t(v.slot) >= e->slots.size()) in.raise("ValueError", "forge::remote_call: cannot resolve captured '" + v.name + "'");
            bind(v.name, e->slots[std::size_t(v.slot)]);
        }
        bool lambda = d.name == "<lambda>" || d.name != name;
        if (lambda) fns.push_back("auto " + name + " = [&]" + src + ";\n");
        else fns.push_back((d.ret_name.empty() ? std::string("auto") : d.ret_name) + " " + name + src + "\n");
    }
    void global(const std::string& name, const Function& f) {
        if (!f.globals) return;
        auto& g = f.globals->vars;
        if (auto it = g.find(name); it != g.end()) {
            if (as<Class>(it->second)) in.raise("TypeError", "forge::remote_call: '" + name + "' is a class; classes cannot be sent to a remote node");
            bind(name, it->second); return;
        }
        if (in.sh->builtins.count(name)) return;   // the node has the same built-ins
        if (auto p = name.find("::"); p != std::string::npos) {   // Enum::Value, alias::member: ship the head
            auto head = name.substr(0, p);
            if (auto it = g.find(head); it != g.end() && as<Map>(it->second)) bind(head, it->second);
        }
        // anything else is unknown here too; the node raises NameError when the function runs
    }
};
} // namespace

void install_remote(Shared& sh) {
    auto n = make<Native>("forge::remote", [](Interp& in, Args& a) -> Value {
        std::string addr = in.str(a[0]), src = in.str(a[1]);
        auto c = connect(in, "forge::remote", addr, a, 2);
        auto r = run_remote(in, c, src);
        auto m = make<Map>(); m->set_str("ok", r.ok); m->set_str("output", strv(r.output)); m->set_str("error_type", strv(r.error_type)); m->set_str("error", strv(r.error));
        return obj(m);
    }, 2, 3);
    n->frozen = true; sh.builtins["forge::remote"] = obj(n);

    auto rc = make<Native>("forge::remote_call", [](Interp& in, Args& a) -> Value {
        std::string addr = in.str(a[0]);
        auto* f = as<Function>(a[1]);
        if (!f) in.raise("TypeError", "forge::remote_call: expected a script function or lambda, got " + Interp::type_of(a[1]));
        Shipper sp{in};
        // a named free function keeps its declaration (and declared return type); a lambda gets a fresh name
        const auto& dn = f->decl->name;
        std::string root = dn != "<lambda>" && dn != "main" && !dn.empty() && dn.find_first_of(":<>/ ") == std::string::npos ? dn : "__forge_remote_fn";
        sp.function(root, *f);
        std::string call = root + "(";
        if (a.size() > 2 && !is_nil(a[2])) {
            auto* l = as<List>(a[2]);
            if (!l) in.raise("TypeError", "forge::remote_call: arguments must be a list, got " + Interp::type_of(a[2]));
            for (std::size_t i = 0; i < l->items.size(); ++i) {
                std::string enc; marshal(in, l->items[i], enc, "forge::remote_call: argument " + std::to_string(i + 1));
                call += (i ? ", " : "") + std::string("forge::__unmarshal(\"") + hex(enc) + "\")";
            }
        }
        call += ")";
        std::string prog = "// forge::remote_call\n";
        for (auto& s : sp.data) prog += s;
        for (auto& s : sp.fns) prog += s;
        for (auto& s : sp.aliases) prog += s;
        prog += "std::print(\"\\x1e" "FORGE-REMOTE-RESULT:{}\", forge::__marshal(" + call + "));\n";
        auto c = connect(in, "forge::remote_call", addr, a, 3);
        if (c.name == "<remote>") c.name = "<remote_call>";
        auto r = run_remote(in, c, prog);
        std::string out = r.output, payload; bool have = false;
        if (auto p = out.rfind(kResultMark); p != std::string::npos) { payload = out.substr(p + std::char_traits<char>::length(kResultMark)); out.resize(p); have = true; }
        if (!out.empty() && in.sh->out) in.sh->out(out);
        if (!r.ok) {
            std::string type = r.error_type.empty() ? "RuntimeError" : r.error_type, msg = r.error;
            if (auto p = msg.find(type + ": "); p != std::string::npos) msg = msg.substr(p + type.size() + 2);
            if (auto p = msg.find("\n  at "); p != std::string::npos) msg.resize(p);   // the node's traceback is in generated code
            in.raise(in.sh->errors.count(type) ? type : std::string("RuntimeError"), "remote: " + msg);
        }
        std::string raw;
        if (!have || !unhex(payload, raw)) in.raise("IOError", "forge::remote_call: the node returned no result");
        Unmarshal u{in, raw}; auto v = u.val(); if (u.p != raw.size()) u.bad();
        return v;
    }, 2, 4);
    rc->frozen = true; sh.builtins["forge::remote_call"] = obj(rc);

    auto ms = make<Native>("forge::__marshal", [](Interp& in, Args& a) -> Value {
        std::string enc; marshal(in, a[0], enc, "remote_call result"); return strv(hex(enc));
    }, 1, 1);
    ms->frozen = true; sh.builtins["forge::__marshal"] = obj(ms);
    auto um = make<Native>("forge::__unmarshal", [](Interp& in, Args& a) -> Value {
        std::string raw; if (!unhex(in.str(a[0]), raw)) in.raise("ValueError", "forge::__unmarshal: malformed value");
        Unmarshal u{in, raw}; auto v = u.val(); if (u.p != raw.size()) u.bad(); return v;
    }, 1, 1);
    um->frozen = true; sh.builtins["forge::__unmarshal"] = obj(um);
}
} // namespace forge::script
