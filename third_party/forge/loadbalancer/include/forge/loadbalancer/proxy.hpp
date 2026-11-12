#pragma once
// L4 (TCP) and L7 (HTTP/1.1) reverse proxy over a loadbalancer::pool whose backends have address "host:port".
//   L4: each client connection leases one backend for its lifetime; bytes are relayed both ways.
//   L7: each request is parsed (request line, headers, Content-Length or chunked body), routed with the
//       affinity key from a header (default X-Session-Id), the path prefix routes, or the client address,
//       then forwarded with retries on a fresh backend if the upstream cannot be reached or answers 502/503.
//       Responses are relayed until the upstream closes or Content-Length is satisfied. Keep-alive on the
//       client side; one upstream connection per request (Connection: close).
// Threads: one acceptor plus one thread per client connection.
#include <forge/loadbalancer/loadbalancer.hpp>
#include <forge/os/os.hpp>
#include <atomic>
#include <cctype>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace forge::loadbalancer {

inline bool split_hostport(std::string_view a, std::string& host, int& port) {
    auto c = a.rfind(':'); if (c == std::string_view::npos) return false;
    host = std::string(a.substr(0, c)); if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    port = std::atoi(std::string(a.substr(c + 1)).c_str()); return port > 0 && port < 65536;
}
inline os::result<os::socket> dial(const backend_spec& b, int timeout_ms) {
    std::string h; int p; if (!split_hostport(b.address, h, p)) return {{}, {-1, "bad backend address " + b.address}};
    return os::socket::connect(h, p, timeout_ms);
}
// TCP health probe: can we connect?
inline std::function<bool(const backend_spec&)> tcp_probe(int timeout_ms = 500) {
    return [timeout_ms](const backend_spec& b) { return bool(dial(b, timeout_ms)); };
}

struct route { std::string path_prefix; std::shared_ptr<pool> target; };
struct proxy_config {
    enum mode_t { l4, l7 } mode = l4;
    std::string listen_host = "127.0.0.1"; int listen_port = 0;
    int connect_timeout_ms = 2000, idle_timeout_ms = 60000;
    unsigned retries = 3;
    std::uint64_t queue_wait_ns = 1'000'000'000;   // backpressure wait when every backend is saturated
    std::string affinity_header = "x-session-id"; // lower-case
    std::size_t max_header_bytes = 64 * 1024;
    std::vector<route> routes;                     // L7 path routing; empty / no match = default pool
};

class proxy {
public:
    proxy(std::shared_ptr<pool> p, proxy_config c = {}) : pool_(std::move(p)), cfg_(std::move(c)) {}
    ~proxy() { stop(); }
    os::status start() {
        os::ignore_sigpipe();
        auto l = os::socket::listen(cfg_.listen_host, cfg_.listen_port); if (!l) return {{}, l.err};
        lis_ = std::move(l.value); port_ = lis_.local_port(); running_ = true;
        acceptor_ = os::thread([this] { accept_loop_(); }, "forge-proxy");
        return {};
    }
    void stop() {
        if (!running_.exchange(false)) return;
        lis_.close(); acceptor_.join();
        std::list<conn> cs; std::list<std::atomic<bool>*> ds; { std::lock_guard g(m_); cs.swap(conns_); ds.swap(done_); }
        for (auto& c : cs) c.client->shutdown_both();
        for (auto& c : cs) c.t.join();
        for (auto* d : ds) delete d;
    }
    int port() const { return port_; }
    std::uint64_t served() const { return served_; }
    std::uint64_t errors() const { return errors_; }

private:
    struct conn { std::shared_ptr<os::socket> client; os::thread t; };
    void accept_loop_() {
        while (running_) {
            if (!lis_.wait_readable(100)) continue;
            auto c = lis_.accept(); if (!c) continue;
            auto cs = std::make_shared<os::socket>(std::move(c.value));
            std::lock_guard g(m_);
            reap_();
            conns_.push_back({cs, {}});
            auto& slot = conns_.back();
            auto* done = new std::atomic<bool>(false);
            done_.push_back(done);
            slot.t = os::thread([this, cs, done] { if (cfg_.mode == proxy_config::l4) serve_l4_(*cs); else serve_l7_(*cs); cs->close(); *done = true; }, "forge-proxy-c");
        }
    }
    void reap_() {   // join finished connection threads (called with m_ held)
        auto it = conns_.begin(); auto d = done_.begin();
        while (it != conns_.end()) { if (**d) { it->t.join(); delete *d; it = conns_.erase(it); d = done_.erase(d); } else { ++it; ++d; } }
    }
    // ---- L4
    void serve_l4_(os::socket& c) {
        std::string peer = std::to_string(std::uintptr_t(c.native()));
        for (unsigned a = 0; a < std::max(1u, cfg_.retries); ++a) {
            lease l = pool_->acquire({peer, {}, nullptr, cfg_.queue_wait_ns});
            if (!l) { ++errors_; return; }
            auto u = dial(l.backend(), cfg_.connect_timeout_ms);
            if (!u) { l.failure(); continue; }
            bool ok = relay_(c, *u);
            ok ? l.success() : l.failure(); ++served_; return;
        }
        ++errors_;
    }
    bool relay_(os::socket& a, os::socket& b) {
        os::poller p; p.add(a.native(), os::readable); p.add(b.native(), os::readable);
        char buf[65536]; bool a_open = true, b_open = true;
        while (a_open || b_open) {
            auto ev = p.wait(cfg_.idle_timeout_ms); if (ev.empty()) return true;
            for (auto& e : ev) {
                bool from_a = e.fd == a.native(); auto& src = from_a ? a : b; auto& dst = from_a ? b : a;
                long n = src.recv(buf, sizeof buf);
                if (n <= 0) { p.remove(src.native()); dst.shutdown_write(); (from_a ? a_open : b_open) = false; if (n < 0 && !from_a) return false; continue; }
                if (!dst.send_all(buf, std::size_t(n))) return !from_a;
            }
        }
        return true;
    }
    // ---- L7
    struct request { std::string method, target, version, head; std::vector<std::pair<std::string, std::string>> headers; std::string body; bool keep_alive = true; };
    static std::string lower(std::string s) { for (auto& ch : s) ch = char(std::tolower((unsigned char)ch)); return s; }
    static const std::string* header(const std::vector<std::pair<std::string, std::string>>& h, const std::string& k) { for (auto& [a, b] : h) if (a == k) return &b; return nullptr; }
    // Read until "\r\n\r\n"; leftover bytes stay in buf.
    bool read_head_(os::socket& s, std::string& buf, std::string& head) {
        for (;;) {
            auto e = buf.find("\r\n\r\n");
            if (e != std::string::npos) { head = buf.substr(0, e + 4); buf.erase(0, e + 4); return true; }
            if (buf.size() > cfg_.max_header_bytes) return false;
            if (!s.wait_readable(cfg_.idle_timeout_ms)) return false;
            char t[8192]; long n = s.recv(t, sizeof t); if (n <= 0) return false; buf.append(t, std::size_t(n));
        }
    }
    static void parse_headers(const std::string& head, std::string& first, std::vector<std::pair<std::string, std::string>>& out) {
        std::size_t p = head.find("\r\n"); first = head.substr(0, p); p += 2;
        while (p < head.size()) {
            auto e = head.find("\r\n", p); if (e == p || e == std::string::npos) break;
            auto line = head.substr(p, e - p); auto c = line.find(':');
            if (c != std::string::npos) { std::string v = line.substr(c + 1); while (!v.empty() && v.front() == ' ') v.erase(0, 1); out.push_back({lower(line.substr(0, c)), v}); }
            p = e + 2;
        }
    }
    // Body by Content-Length or chunked (de-chunked into body).
    bool read_body_(os::socket& s, std::string& buf, const std::vector<std::pair<std::string, std::string>>& h, std::string& body) {
        auto need = [&](std::size_t n) { while (buf.size() < n) { if (!s.wait_readable(cfg_.idle_timeout_ms)) return false; char t[65536]; long r = s.recv(t, sizeof t); if (r <= 0) return false; buf.append(t, std::size_t(r)); } return true; };
        if (auto te = header(h, "transfer-encoding"); te && lower(*te).find("chunked") != std::string::npos) {
            for (;;) {
                std::size_t e; while ((e = buf.find("\r\n")) == std::string::npos) if (!need(buf.size() + 1)) return false;
                std::size_t len = std::strtoull(buf.c_str(), nullptr, 16); buf.erase(0, e + 2);
                if (!need(len + 2)) return false; body.append(buf, 0, len); buf.erase(0, len + 2);
                if (!len) return true;
            }
        }
        std::size_t len = 0; if (auto cl = header(h, "content-length")) len = std::strtoull(cl->c_str(), nullptr, 10);
        if (!need(len)) return false; body = buf.substr(0, len); buf.erase(0, len); return true;
    }
    std::shared_ptr<pool> route_(const std::string& target) {
        std::shared_ptr<pool> best = pool_; std::size_t blen = 0;
        for (auto& r : cfg_.routes) if (target.compare(0, r.path_prefix.size(), r.path_prefix) == 0 && r.path_prefix.size() >= blen) { best = r.target; blen = r.path_prefix.size(); }
        return best;
    }
    static void reply(os::socket& c, int code, const char* msg) {
        std::string b = std::string(msg) + "\n";
        std::string r = "HTTP/1.1 " + std::to_string(code) + " " + msg + "\r\nContent-Length: " + std::to_string(b.size()) + "\r\nContent-Type: text/plain\r\n\r\n" + b;
        c.send_all(r.data(), r.size());
    }
    void serve_l7_(os::socket& c) {
        std::string buf;
        while (running_) {
            request rq; std::string first;
            if (!read_head_(c, buf, rq.head)) return;
            parse_headers(rq.head, first, rq.headers);
            auto s1 = first.find(' '), s2 = first.rfind(' ');
            if (s1 == std::string::npos || s2 == s1) { reply(c, 400, "Bad Request"); return; }
            rq.method = first.substr(0, s1); rq.target = first.substr(s1 + 1, s2 - s1 - 1); rq.version = first.substr(s2 + 1);
            if (!read_body_(c, buf, rq.headers, rq.body)) { reply(c, 400, "Bad Request"); return; }
            if (auto cn = header(rq.headers, "connection")) rq.keep_alive = lower(*cn) != "close"; else rq.keep_alive = rq.version != "HTTP/1.0";
            // forward head: same headers minus hop-by-hop, plus Connection: close and our body length
            std::string out = rq.method + " " + rq.target + " HTTP/1.1\r\n";
            for (auto& [k, v] : rq.headers) if (k != "connection" && k != "transfer-encoding" && k != "content-length" && k != "keep-alive") out += k + ": " + v + "\r\n";
            out += "Content-Length: " + std::to_string(rq.body.size()) + "\r\nConnection: close\r\nX-Forwarded-By: forge\r\n\r\n";
            out += rq.body;
            const std::string* aff = header(rq.headers, cfg_.affinity_header);
            std::string key = aff ? *aff : rq.target;
            auto target = route_(rq.target);
            bool sent = false;
            std::vector<std::size_t> tried;
            for (unsigned a = 0; a < std::max(1u, cfg_.retries) && !sent; ++a) {
                lease l = target->acquire({key, rq.body, &tried, cfg_.queue_wait_ns});
                if (!l) break;
                tried.push_back(l.index());
                auto u = dial(l.backend(), cfg_.connect_timeout_ms);
                if (!u || !u->send_all(out.data(), out.size())) { l.failure(); continue; }
                std::string ub, uhead;
                if (!read_head_(*u, ub, uhead)) { l.failure(); continue; }
                std::string ufirst; std::vector<std::pair<std::string, std::string>> uh; parse_headers(uhead, ufirst, uh);
                int code = ufirst.size() > 12 ? std::atoi(ufirst.c_str() + 9) : 0;
                bool idempotent = rq.method == "GET" || rq.method == "HEAD" || rq.method == "PUT" || rq.method == "DELETE" || rq.method == "OPTIONS";
                if ((code == 502 || code == 503) && a + 1 < cfg_.retries && (idempotent || header(rq.headers, "idempotency-key"))) { l.failure(); continue; }
                std::string body;
                bool has_len = header(uh, "content-length") || header(uh, "transfer-encoding");
                if (has_len) { if (!read_body_(*u, ub, uh, body)) { l.failure(); reply(c, 502, "Bad Gateway"); return; } }
                else { body = ub; char t[65536]; long n; while (u->wait_readable(cfg_.idle_timeout_ms) && (n = u->recv(t, sizeof t)) > 0) body.append(t, std::size_t(n)); }
                std::string resp = ufirst + "\r\n";
                for (auto& [k, v] : uh) if (k != "connection" && k != "transfer-encoding" && k != "content-length" && k != "keep-alive") resp += k + ": " + v + "\r\n";
                resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                resp += rq.keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
                if (rq.method != "HEAD") resp += body;
                code >= 500 ? l.failure() : l.success();
                sent = true; ++served_;
                if (!c.send_all(resp.data(), resp.size())) return;
            }
            if (!sent) { ++errors_; reply(c, 503, "Service Unavailable"); }
            if (!rq.keep_alive) return;
        }
    }

    std::shared_ptr<pool> pool_; proxy_config cfg_;
    os::socket lis_; int port_ = 0; std::atomic<bool> running_{false}; os::thread acceptor_;
    std::mutex m_; std::list<conn> conns_; std::list<std::atomic<bool>*> done_;
    std::atomic<std::uint64_t> served_{0}, errors_{0};
};

} // namespace forge::loadbalancer
