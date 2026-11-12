#pragma once
// RPC device: run lowerings on a remote FORGE process over TCP ("rpc:host:port").
// The server (`forge rpc-serve`, or rpc::serve()) executes each op with its own device registry (CPU, Vulkan, CUDA...),
// so a machine without a GPU can borrow one over the network. Every op is still oracle-gated on the client.
// Wire format (little endian): request  = "FRPC" u32 op-name-len name u32 nattrs {key, tag, value} u32 nin {u32 rank, i64 dims, f32 data} u32 rank i64 out-dims
//                              response = u32 status(0 ok) then f32 out data, or u32 len + error text.
// Not optimised: weights travel with every op (no remote residency yet).
// Security (rpc_secure.hpp, docs/rpc.md): optional TLS + token / Ed25519 key auth before the first frame.
// Remote scripts:  request  = "FRPS" str source, str name, u32 argc {str}, u32 timeout_ms
//                  or "FRPZ" (signed) with the same fields then str signature (the script's detached .sig text, FSS
//                  INT-2; servers older than FRPZ close the connection)
//                  response = u32 status(0 ok / 1 refused + str error) then u32 ok, str error_type, str error, str output
#include <forge/graph/kernels.hpp>
#include <forge/graph/rpc_secure.hpp>
#include <functional>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <forge/os/os.hpp>

namespace forge::graph::rpc {
#ifdef FORGE_HAVE_TLS
inline namespace tls_v1 {
#else
inline namespace plain_v1 {
#endif
namespace detail {
inline void wsa() { os::net_init(); }
inline bool send_all(channel& s, const void* p, std::size_t n) { return s.send_all(p, n); }
inline bool recv_all(channel& s, void* p, std::size_t n) { return s.recv_all(p, n); }
struct writer {
    std::vector<char> b;
    void raw(const void* p, std::size_t n) { auto* c = static_cast<const char*>(p); b.insert(b.end(), c, c + n); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void str(std::string_view s) { u32(std::uint32_t(s.size())); raw(s.data(), s.size()); }
    void shape(const shape_t& s) { u32(std::uint32_t(s.size())); for (auto d : s) i64(d); }
};
struct reader {
    channel& s; bool ok = true;
    void raw(void* p, std::size_t n) { if (ok) ok = recv_all(s, p, n); }
    std::uint32_t u32() { std::uint32_t v = 0; raw(&v, 4); return v; }
    std::int64_t i64() { std::int64_t v = 0; raw(&v, 8); return v; }
    std::string str(std::size_t cap = 1 << 20) { auto n = u32(); if (n > cap) { ok = false; return {}; } std::string r(n, '\0'); raw(r.data(), n); return r; }
    shape_t shape() { auto r = u32(); shape_t s; if (r > 16) { ok = false; return s; } for (std::uint32_t i = 0; i < r; ++i) s.push_back(i64()); return s; }
};
inline void put_attrs(writer& w, const ops::attrs& a) {
    w.u32(std::uint32_t(a.v.size()));
    for (auto& [k, v] : a.v) {
        w.str(k); w.u32(std::uint32_t(v.index()));
        if (auto* d = std::get_if<double>(&v)) w.raw(d, 8);
        else if (auto* iv = std::get_if<std::vector<i64>>(&v)) { w.u32(std::uint32_t(iv->size())); for (auto x : *iv) w.i64(x); }
        else w.str(std::get<std::string>(v));
    }
}
inline ops::attrs get_attrs(reader& r) {
    ops::attrs a; auto n = r.u32(); if (n > 4096) { r.ok = false; return a; }
    for (std::uint32_t i = 0; i < n && r.ok; ++i) {
        auto k = r.str(); auto tag = r.u32();
        if (tag == 0) { double d; r.raw(&d, 8); a.v[k] = d; }
        else if (tag == 1) { auto m = r.u32(); if (m > (1u << 20)) { r.ok = false; break; } std::vector<i64> iv(m); for (auto& x : iv) x = r.i64(); a.v[k] = iv; }
        else if (tag == 2) a.v[k] = r.str();
        else r.ok = false;
    }
    return a;
}
inline bool valid_shape(const shape_t& s) { i64 n = 1; for (auto d : s) { if (d < 0 || d > (i64(1) << 32)) return false; n *= std::max<i64>(d, 1); if (n > (i64(1) << 32)) return false; } return true; }
inline int op_index(std::string_view name) {
    for (std::size_t i = 0; i < ops::op_count; ++i) if (ops::info(ops::op(i)).name == name) return int(i);
    return -1;
}
} // namespace detail

// Connection to one server; serialises calls (one op in flight).
class client {
public:
    static std::shared_ptr<client> connect(const std::string& host, const std::string& port, std::string* why, const client_security& sec = env_security()) {
        detail::wsa();
        int pn = std::atoi(port.c_str());
        auto r = os::socket::connect(host, pn, 10000);
        if (!r) { if (why) *why = "rpc: cannot connect to " + host + ":" + port + " (" + r.err.what + ")"; return nullptr; }
        r->set_nodelay();
        auto c = std::shared_ptr<client>(new client); c->s_ = channel(std::move(r.value));
        if (sec.tls.enabled) {
#ifdef FORGE_HAVE_TLS
            if (!c->s_.connect_tls(sec.tls, host, why)) return nullptr;
#else
            if (why) *why = "rpc: TLS requested but this build has no TLS (rebuild with OpenSSL)"; return nullptr;
#endif
        }
        if (!client_auth(c->s_, sec, why)) return nullptr;
        return c;
    }
    // Credentials from the environment: FORGE_RPC_KEY (secret key file), FORGE_RPC_TOKEN, FORGE_RPC_TLS=1,
    // FORGE_RPC_CA, FORGE_RPC_FINGERPRINT (sha256 of the server cert), FORGE_RPC_SERVER_NAME,
    // FORGE_RPC_TLS_CERT + FORGE_RPC_TLS_KEY (client certificate for mTLS servers).
    static client_security env_security() {
        client_security s; auto env = [](const char* k) { const char* v = std::getenv(k); return std::string(v ? v : ""); };
        if (auto k = env("FORGE_RPC_KEY"); !k.empty()) s.key = std::make_shared<identity>(identity_from_seed_hex(read_key_file(k)));
        s.token = env("FORGE_RPC_TOKEN");
        s.tls.ca_file = env("FORGE_RPC_CA"); s.tls.fingerprint_sha256 = env("FORGE_RPC_FINGERPRINT"); s.tls.server_name = env("FORGE_RPC_SERVER_NAME");
        s.tls.cert_file = env("FORGE_RPC_TLS_CERT"); s.tls.key_file = env("FORGE_RPC_TLS_KEY");
        s.tls.enabled = env("FORGE_RPC_TLS") == "1" || !s.tls.ca_file.empty() || !s.tls.fingerprint_sha256.empty() || !s.tls.cert_file.empty();
        return s;
    }
    struct script_reply { bool ok = false; std::string error_type, error, output; };
    // Run a FORGE script on the server (needs an authenticated connection to a server started with scripts allowed).
    // `signature`: the script's detached signature (`forge sign script.fs` -> script.fs.sig text; `name` must be the
    // signed file name), checked by servers whose policy has integrity.scripts.
    script_reply run_script(std::string_view source, std::string_view name = "<remote>", const std::vector<std::string>& argv = {}, std::uint32_t timeout_ms = 0,
                            std::string_view signature = {}) {
        std::lock_guard g(m_);
        detail::writer w; w.raw(signature.empty() ? "FRPS" : "FRPZ", 4); w.str(source); w.str(name); w.u32(std::uint32_t(argv.size())); for (auto& a : argv) w.str(a); w.u32(timeout_ms);
        if (!signature.empty()) w.str(signature);
        if (!detail::send_all(s_, w.b.data(), w.b.size())) raise(errc::io_error, "rpc: send failed");
        detail::reader r{s_};
        auto st = r.u32(); if (!r.ok) raise(errc::io_error, "rpc: connection lost");
        if (st != 0) raise(errc::unsupported, "rpc server: {}", r.str());
        script_reply out; out.ok = r.u32() != 0; out.error_type = r.str(); out.error = r.str(); out.output = r.str(64u << 20);
        if (!r.ok) raise(errc::io_error, "rpc: short reply");
        return out;
    }
    std::uint64_t calls() const { return calls_; }
    void run(std::string_view op, std::span<const tensor_view> in, tensor_view out, const ops::attrs& a) {
        std::lock_guard g(m_);
        detail::writer w; w.raw("FRPC", 4); w.str(op); detail::put_attrs(w, a);
        w.u32(std::uint32_t(in.size()));
        for (auto& t : in) { w.shape(t.shape); w.raw(t.data, t.size() * 4); }
        w.shape(out.shape);
        if (!detail::send_all(s_, w.b.data(), w.b.size())) raise(errc::io_error, "rpc: send failed");
        detail::reader r{s_};
        auto st = r.u32();
        if (!r.ok) raise(errc::io_error, "rpc: connection lost");
        if (st != 0) raise(errc::unsupported, "rpc server: {}", r.str());
        r.raw(out.data, out.size() * 4);
        if (!r.ok) raise(errc::io_error, "rpc: short reply");
        ++calls_;
    }
private:
    client() = default;
    channel s_; std::mutex m_; std::uint64_t calls_ = 0;
};

// Registry whose every op runs remotely ("rpc.<op>", priority 20). Ops the server lacks a lowering for run there on its reference.
inline std::unique_ptr<registry> rpc_registry(std::shared_ptr<client> c) {
    auto r = registry::make();
    for (std::size_t i = 0; i < ops::op_count; ++i) {
        auto o = ops::op(i); std::string name(ops::info(o).name);
        r->add({"rpc." + name, o, 20, nullptr,
                [c, name](std::span<const tensor_view> in, tensor_view out, const ops::attrs& a) { c->run(name, in, out, a); }});
    }
    return r;
}
// "rpc:host:port" (host may be [v6]).
inline std::unique_ptr<registry> open(const std::string& addr, std::string* why) {
    auto hp = addr; auto colon = hp.rfind(':');
    if (colon == std::string::npos) { if (why) *why = "rpc: want rpc:host:port"; return nullptr; }
    auto host = hp.substr(0, colon), port = hp.substr(colon + 1);
    if (host.size() > 1 && host.front() == '[') host = host.substr(1, host.size() - 2);
    auto c = client::connect(host, port, why);
    return c ? rpc_registry(c) : nullptr;
}

// Server. `reg` (null = CPU) picks the lowering per op exactly like a local plan would.
class server {
public:
    // port 0 = ephemeral; see port(). bind = "127.0.0.1" by default (pass "0.0.0.0" to expose; there is no auth).
    struct script_result { bool ok = false; std::string error_type, error, output; };
    // Runs a remote script; set by the host (apps/forge.cpp wires forge::script). Null = scripts unsupported.
    std::function<script_result(const std::string& src, const std::string& name, const std::vector<std::string>& argv, std::uint32_t timeout_ms)> script_handler;
    // Same, with the client's detached signature ("" for an unsigned FRPS request). Preferred over script_handler when set.
    std::function<script_result(const std::string& src, const std::string& name, const std::vector<std::string>& argv, std::uint32_t timeout_ms,
                                const std::string& signature)> signed_script_handler;
    // Security log (optional): authentications with the principal and its permissions, refusals, permission denials.
    // Called from connection threads.
    std::function<void(const std::string&)> log;
    server(registry* reg, int port = 0, const std::string& bind = "127.0.0.1", server_security sec = {}) : reg_(reg ? reg : &registry::cpu()), sec_(std::move(sec)) {
        detail::wsa();
        if (!sec_.tls.client_ca_file.empty() && sec_.tls.cert_file.empty()) raise(errc::invalid_argument, "rpc: client certificates (mTLS) need TLS (a server certificate and key)");
        if (!sec_.tls.cert_file.empty()) {
#ifdef FORGE_HAVE_TLS
            tls_ = make_server_tls(sec_.tls);
#else
            raise(errc::unsupported, "rpc: TLS requested but this build has no TLS (rebuild with OpenSSL)");
#endif
        }
        if (!sec_.client_map.empty() && sec_.tls.client_ca_file.empty()) raise(errc::invalid_argument, "rpc: a client certificate map needs client certificates (mTLS: a client CA)");
        if (sec_.allow_scripts && !sec_.has_credentials()) raise(errc::invalid_argument, "rpc: remote scripts need authentication (tokens or authorized keys)");
        auto l = os::socket::listen(bind, port, 8);
        if (!l) raise(errc::io_error, "rpc: cannot listen on {}:{}: {}", bind, port, l.err.what);
        ls_ = std::move(l.value); port_ = ls_.local_port();
    }
    ~server() { stop(); }
    int port() const { return port_; }
    std::uint64_t ops() const { return ops_; }
    // Accepts connections until stop(); one thread per connection.
    void run() {
        while (!stop_) {
            // macOS/BSD: shutdown() on a listening socket does not wake a blocked accept() (Linux's does), so
            // wait for a connection in 200 ms slices and re-check stop_.
            if (!ls_.wait_readable(200)) continue;
            auto c = ls_.accept();
            if (!c) { if (stop_) break; continue; }
            auto ch = std::make_shared<channel>(std::move(c.value));
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::lock_guard g(m_);
            for (std::size_t i = 0; i < conns_.size();)   // reap finished connections
                if (conns_[i].done->load()) { if (conns_[i].th.joinable()) conns_[i].th.join(); conns_.erase(conns_.begin() + std::ptrdiff_t(i)); } else ++i;
            conns_.push_back({ch, done, std::thread([this, ch, done] { serve_conn(*ch); ch->shutdown_both(); done->store(true); })});
        }
    }
    void start() { th_ = std::thread([this] { run(); }); }
    void stop() {
        if (stop_.exchange(true)) return;
#if defined(_WIN32)
        ls_.close();                            // Winsock: shutdown() does not wake a blocked accept(); closesocket() does
        if (th_.joinable()) th_.join();
#else
        ls_.shutdown_both();                    // wakes accept() on Linux (run() polls elsewhere); close after the thread is gone
        if (th_.joinable()) th_.join();
        ls_.close();
#endif
        std::vector<conn> cs; { std::lock_guard g(m_); for (auto& c : conns_) c.ch->shutdown_both(); cs.swap(conns_); }
        for (auto& c : cs) if (c.th.joinable()) c.th.join();     // workers never outlive the server
    }
private:
    void serve_conn(channel& s) {
#ifdef FORGE_HAVE_TLS
        if (tls_) { s.sock().set_timeout_ms(15000); if (!s.accept_tls(tls_, nullptr)) return; s.sock().set_timeout_ms(0); }
#endif
        peer_info peer; bool first = true; auto conn_id = ++conn_seq_;
        auto say = [&](const std::string& m) { if (log) log("conn " + std::to_string(conn_id) + ": " + m); };
        for (;;) {
#if defined(_WIN32)
            // Winsock: shutdown() from stop() does not reliably wake a blocked recv(), so idle between requests in slices.
            while (!stop_ && !s.sock().wait_readable(200)) {}
            if (stop_) return;
#endif
            detail::reader r{s};
            char magic[4]; r.raw(magic, 4);
            if (!r.ok) return;
            if (first) {
                first = false;
                std::string why;
                if (!server_auth(s, sec_, magic, peer, &why)) { say("refused: " + (why.empty() ? std::string("connection lost during authentication") : why)); return; }
                if (peer.authed) say("authenticated " + peer.who + " allow=" + perms_string(peer.perms));
                else say("unauthenticated (no auth configured)");
                if (peer.handshake) continue;   // handshake consumed; next frame
            }
            if (std::memcmp(magic, "FRPS", 4) == 0 || std::memcmp(magic, "FRPZ", 4) == 0) { if (!serve_script(s, r, peer, say, magic[3] == 'Z')) return; continue; }
            if (std::memcmp(magic, "FRPC", 4) != 0) return;
            auto name = r.str(256); auto a = detail::get_attrs(r);
            auto n = r.u32(); if (n > 64) return;
            std::vector<tensor> ins; std::vector<tensor_view> views; std::vector<shape_t> shapes;
            for (std::uint32_t i = 0; i < n && r.ok; ++i) {
                auto sh = r.shape(); if (!detail::valid_shape(sh)) return;
                ins.emplace_back(sh); r.raw(ins.back().data.data(), ins.back().data.size() * 4);
            }
            auto os = r.shape(); if (!r.ok || !detail::valid_shape(os)) return;
            for (auto& t : ins) { views.push_back({t.data.data(), t.shape}); shapes.push_back(t.shape); }
            detail::writer w;
            int oi = detail::op_index(name);
            try {
                if (!(peer.perms & perm_device)) { say("denied device op " + name + " for " + peer.who); raise(errc::unsupported, "permission denied: {} may not run device ops (allow=)", peer.who); }
                if (oi < 0) raise(errc::unsupported, "unknown op '{}'", name);
                tensor out(os);
                auto& l = reg_->select(ops::op(oi), shapes, os, a);
                auto* h = l.on_device ? reg_->hooks.get() : nullptr;
                std::lock_guard dg(dev_m_);   // device runtimes are single-stream
                if (h) h->begin_run();
                try { l.fn(views, {out.data.data(), os}, a); if (h) { h->to_host(out.data.data(), out.data.size()); h->end_run(); } }
                catch (...) { if (h) h->end_run(); throw; }
                w.u32(0); w.raw(out.data.data(), out.data.size() * 4); ++ops_;
            } catch (const std::exception& e) { w.b.clear(); w.u32(1); w.str(e.what()); }
            if (!detail::send_all(s, w.b.data(), w.b.size())) return;
        }
    }
    template <class Log> bool serve_script(channel& s, detail::reader& r, const peer_info& peer, Log& say, bool with_sig = false) {
        auto src = r.str(16u << 20); auto name = r.str(4096); auto n = r.u32(); if (!r.ok || n > 4096) return false;
        std::vector<std::string> argv; for (std::uint32_t i = 0; i < n && r.ok; ++i) argv.push_back(r.str(1 << 20));
        auto tmo = r.u32(); if (!r.ok) return false;
        std::string sig; if (with_sig) { sig = r.str(64u << 10); if (!r.ok) return false; }
        detail::writer w;
        if (!sec_.allow_scripts || !peer.authed) { w.u32(1); w.str("remote scripts are disabled on this server"); }
        else if (!(peer.perms & perm_scripts)) { say("denied script for " + peer.who); w.u32(1); w.str("permission denied: " + peer.who + " may not run scripts (allow=)"); }
        else if (!script_handler && !signed_script_handler) { w.u32(1); w.str("this server has no script runtime"); }
        else {
            script_result res; say("script '" + name + "' for " + peer.who + (sig.empty() ? "" : " (signed)"));
            try { res = signed_script_handler ? signed_script_handler(src, name, argv, tmo, sig) : script_handler(src, name, argv, tmo); } catch (const std::exception& e) { res.ok = false; res.error_type = "RuntimeError"; res.error = e.what(); }
            w.u32(0); w.u32(res.ok ? 1 : 0); w.str(res.error_type); w.str(res.error); w.str(res.output); ++scripts_;
        }
        return detail::send_all(s, w.b.data(), w.b.size());
    }
public:
    std::uint64_t scripts() const { return scripts_; }
    bool tls() const {
#ifdef FORGE_HAVE_TLS
        return tls_ != nullptr;
#else
        return false;
#endif
    }
private:
    registry* reg_; server_security sec_; os::socket ls_; int port_ = 0; std::atomic<std::uint64_t> scripts_{0};
#ifdef FORGE_HAVE_TLS
    std::shared_ptr<SSL_CTX> tls_;
#endif
    std::atomic<bool> stop_{false}; std::atomic<std::uint64_t> ops_{0}, conn_seq_{0};
    std::thread th_; std::mutex m_, dev_m_; struct conn { std::shared_ptr<channel> ch; std::shared_ptr<std::atomic<bool>> done; std::thread th; };
    std::vector<conn> conns_;
};
} // inline namespace
} // namespace forge::graph::rpc
