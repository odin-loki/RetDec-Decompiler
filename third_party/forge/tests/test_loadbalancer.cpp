// forge::loadbalancer: strategies, circuit breaking, health checks, retries, backpressure, L4/L7 proxy.
#include "check.hpp"
#include <forge/loadbalancer/proxy.hpp>
#include <map>
#include <set>
using namespace forge;
using namespace forge::loadbalancer;

static std::shared_ptr<pool> make(strategy s, int n, config c = {}) {
    c.strat = s; auto p = std::make_shared<pool>(c);
    for (int i = 0; i < n; ++i) p->add({"b" + std::to_string(i), "", 1.0});
    return p;
}
static std::map<std::string, int> spread(pool& p, int n, std::function<pick_options(int)> f = {}) {
    std::map<std::string, int> m;
    for (int i = 0; i < n; ++i) { auto l = p.acquire(f ? f(i) : pick_options{}); if (l) { ++m[l.backend().id]; l.success_with_latency(1000); } }
    return m;
}

TEST(round_robin_and_weighted) {
    auto p = make(strategy::round_robin, 3); auto m = spread(*p, 300);
    CHECK(m["b0"] == 100 && m["b1"] == 100 && m["b2"] == 100);
    pool w({strategy::weighted}); w.add({"a", "", 5}); w.add({"b", "", 1}); w.add({"c", "", 1});
    auto mw = spread(w, 700); CHECK(mw["a"] == 500 && mw["b"] == 100 && mw["c"] == 100);
    // smooth WRR interleaves rather than bursting: no 5 consecutive "a" picks at the start
    std::string seq; for (int i = 0; i < 7; ++i) { auto l = w.acquire(); seq += l.backend().id; l.success(); }
    CHECK(seq.find("aaaaa") == std::string::npos);
    CHECK(parse_strategy("p2c") == strategy::power_of_two && parse_strategy("kv") == strategy::prefix_affinity && !parse_strategy("nope"));
}
TEST(least_connections) {
    auto p = make(strategy::least_connections, 3);
    auto a = p->acquire(), b = p->acquire(), c = p->acquire();
    std::set<std::string> ids{a.backend().id, b.backend().id, c.backend().id}; CHECK(ids.size() == 3);
    std::string held = a.backend().id; b.success(); c.success();
    for (int i = 0; i < 20; ++i) { auto l = p->acquire(); CHECK(l.backend().id != held); l.success(); }
}
TEST(least_latency_ewma) {
    auto p = make(strategy::least_latency, 3);
    for (int i = 0; i < 30; ++i) { auto l = p->acquire(); l.success_with_latency(l.backend().id == "b1" ? 1'000'000 : 50'000'000); }
    int fast = 0; for (int i = 0; i < 100; ++i) { auto l = p->acquire(); bool f = l.backend().id == "b1"; fast += f; l.success_with_latency(f ? 1'000'000 : 50'000'000); }
    CHECK(fast >= 90);
}
TEST(power_of_two_choices) {
    auto p = make(strategy::power_of_two, 4);
    std::vector<lease> held; for (int i = 0; i < 8; ++i) held.push_back(p->acquire());
    std::map<std::string, int> load; for (auto& l : held) ++load[l.backend().id];
    int mx = 0, mn = 100; for (auto& [k, v] : load) { mx = std::max(mx, v); mn = std::min(mn, v); }
    CHECK(load.size() == 4 && mx - mn <= 2);
}
TEST(consistent_hash_stability_and_bounded_load) {
    auto p = make(strategy::consistent_hash, 5);
    std::map<std::string, std::string> before;
    for (int i = 0; i < 1000; ++i) { auto k = "user" + std::to_string(i); auto l = p->acquire({k}); before[k] = l.backend().id; l.success(); }
    std::map<std::string, int> cnt; for (auto& [k, v] : before) ++cnt[v];
    for (auto& [k, v] : cnt) CHECK(v > 100 && v < 320);
    for (int i = 0; i < 1000; i += 7) { auto k = "user" + std::to_string(i); auto l = p->acquire({k}); CHECK(l.backend().id == before[k]); l.success(); }
    p->remove("b2"); int moved = 0;
    for (auto& [k, v] : before) { auto l = p->acquire({k}); if (v != "b2" && l.backend().id != v) ++moved; CHECK(l.backend().id != "b2"); l.success(); }
    CHECK(moved == 0);   // only keys of the removed node move
    // bounded load: one hot key held many times spills to other nodes
    std::vector<lease> hot; std::set<std::string> ids; for (int i = 0; i < 12; ++i) { hot.push_back(p->acquire({"hot"})); ids.insert(hot.back().backend().id); }
    CHECK(ids.size() >= 3);
}
TEST(prefix_affinity) {
    config c; c.prefix_block = 16; auto p = make(strategy::prefix_affinity, 4, c);
    std::string sys(64, 'S'), a = sys + "question one about cats....", b = sys + "a different follow-up.......";
    std::string first; { auto l = p->acquire({"", a}); first = l.backend().id; l.success(); }
    for (int i = 0; i < 10; ++i) { auto l = p->acquire({"", b}); CHECK(l.backend().id == first); l.success(); }   // shared prefix -> same KV holder
    std::vector<lease> busy; for (int i = 0; i < 6; ++i) { auto l = p->acquire({"", a}); busy.push_back(std::move(l)); }   // overload it: spills
    std::set<std::string> ids; for (auto& l : busy) ids.insert(l.backend().id); CHECK(ids.size() >= 2);
}
TEST(circuit_breaker) {
    config c; c.failure_threshold = 3; c.open_ns = 1'000'000'000; auto p = make(strategy::round_robin, 2, c);
    for (int i = 0; i < 6; ++i) { auto l = p->acquire(); if (l.backend().id == "b0") l.failure(); else l.success(); }
    auto st = p->stats(); CHECK(st[0].state == circuit::open);
    for (int i = 0; i < 10; ++i) { auto l = p->acquire(); CHECK(l.backend().id == "b1"); l.success(); }
    os::sleep_ns(1'100'000'000);
    CHECK(p->stats()[0].state == circuit::half_open);
    { auto l = p->acquire(); CHECK(l.backend().id == "b0"); auto l2 = p->acquire(); CHECK(l2.backend().id == "b1"); l2.success(); l.success(); }   // one probe
    CHECK(p->stats()[0].state == circuit::closed);
    { pool q(c); q.add({"x"}); for (int i = 0; i < 3; ++i) q.acquire().failure(); CHECK(!q.acquire()); os::sleep_ns(1'100'000'000);
      auto l = q.acquire(); CHECK(l); l.failure(); CHECK(q.stats()[0].state == circuit::open); }   // failed probe re-opens
}
TEST(health_checks) {
    config c; c.unhealthy_after = 2; c.healthy_after = 2; auto p = make(strategy::round_robin, 3, c);
    std::atomic<bool> b1_up{false};
    p->set_health_check([&](const backend_spec& b) { return b.id != "b1" || b1_up.load(); });
    p->run_health_checks(); CHECK(p->stats()[1].healthy); p->run_health_checks(); CHECK(!p->stats()[1].healthy);
    auto m = spread(*p, 30); CHECK(m.count("b1") == 0);
    b1_up = true; p->run_health_checks(); CHECK(!p->stats()[1].healthy); p->run_health_checks(); CHECK(p->stats()[1].healthy);
    { health_checker hc(*p, 5'000'000); b1_up = false; os::sleep_ns(100'000'000); } CHECK(!p->stats()[1].healthy);
    p->set_healthy("b1", true); CHECK(p->stats()[1].healthy);
}
TEST(retries_on_other_backends) {
    auto p = make(strategy::round_robin, 3);
    std::vector<std::string> seen;
    bool ok = p->call({}, [&](const backend_spec& b) { seen.push_back(b.id); return seen.size() == 3; }, {3, 1000, 10000});
    CHECK(ok); CHECK(seen.size() == 3); CHECK(std::set<std::string>(seen.begin(), seen.end()).size() == 3);
    int n = 0; CHECK(!p->call({}, [&](const backend_spec&) { ++n; throw 1; return true; }, {2, 1000, 1000})); CHECK(n == 2);
}
TEST(backpressure_queue) {
    config c; c.queue_limit = 1; pool p(c); p.add({"only", "", 1, 1});
    auto a = p.acquire(); CHECK(a); CHECK(!p.acquire()); CHECK(p.rejected() == 1);
    os::thread t([&] { os::sleep_ns(30'000'000); a.success(); });
    auto t0 = os::monotonic_ns(); auto b = p.acquire({{}, {}, nullptr, 2'000'000'000}); CHECK(b); CHECK(os::monotonic_ns() - t0 >= 20'000'000);
    t.join();
    std::atomic<int> got{0}; os::thread w([&] { auto x = p.acquire({{}, {}, nullptr, 500'000'000}); if (x) ++got; });   // queued
    os::sleep_ns(50'000'000);
    auto r = p.acquire({{}, {}, nullptr, 1'000'000}); CHECK(!r);   // queue full (limit 1) -> immediate reject
    b.success(); w.join(); CHECK(got == 1);
    auto t1 = os::monotonic_ns(); { auto h = p.acquire(); CHECK(!p.acquire({{}, {}, nullptr, 30'000'000})); CHECK(os::monotonic_ns() - t1 >= 25'000'000); }
}
TEST(dynamic_membership_and_concurrency) {
    auto p = make(strategy::power_of_two, 4);
    std::atomic<int> done{0};
    { std::vector<os::thread> ts; for (int t = 0; t < 4; ++t) ts.emplace_back([&] { for (int i = 0; i < 2000; ++i) { auto l = p->acquire(); if (l) { l.success_with_latency(1000 + i); ++done; } } });
      p->add({"late"}); p->remove("b0"); p->set_weight("b1", 3); }
    CHECK(done == 8000);
    for (auto& s : p->stats()) CHECK(s.inflight == 0);
}

// ---- proxy: tiny upstream servers
struct upstream {
    os::socket lis; int port = 0; std::string name; std::atomic<bool> stop{false}; std::atomic<int> hits{0}; int status = 200; os::thread t;
    explicit upstream(std::string n, bool http, int st = 200) : name(std::move(n)), status(st) {
        lis = std::move(os::socket::listen("127.0.0.1", 0).value); port = lis.local_port();
        t = os::thread([this, http] {
            while (!stop) {
                if (!lis.wait_readable(50)) continue;
                auto c = lis.accept(); if (!c) continue; ++hits;
                if (!http) { char b[256]; long n; while ((n = c->recv(b, sizeof b)) > 0) { std::string r = name + ":" + std::string(b, std::size_t(n)); c->send_all(r.data(), r.size()); } continue; }
                std::string req; char b[4096]; long n;
                while (req.find("\r\n\r\n") == std::string::npos && (n = c->recv(b, sizeof b)) > 0) req.append(b, std::size_t(n));
                auto cl = req.find("content-length: "); std::size_t len = cl == std::string::npos ? 0 : std::strtoull(req.c_str() + cl + 16, nullptr, 10);
                while (req.size() < req.find("\r\n\r\n") + 4 + len && (n = c->recv(b, sizeof b)) > 0) req.append(b, std::size_t(n));
                std::string body = name + " " + req.substr(0, req.find("\r\n")) + " body=" + req.substr(req.find("\r\n\r\n") + 4);
                std::string r = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(body.size()) + "\r\nX-Up: " + name + "\r\n\r\n" + body;
                c->send_all(r.data(), r.size());
            }
        });
    }
    ~upstream() { stop = true; t.join(); }
    std::string addr() const { return "127.0.0.1:" + std::to_string(port); }
};
static std::string http(int port, const std::string& req) {
    auto c = os::socket::connect("127.0.0.1", port); if (!c) return "";
    c->send_all(req.data(), req.size()); std::string r; char b[4096]; long n;
    while (c->wait_readable(3000) && (n = c->recv(b, sizeof b)) > 0) { r.append(b, std::size_t(n)); auto h = r.find("\r\n\r\n"); auto cl = r.find("Content-Length: ");
        if (h != std::string::npos && cl != std::string::npos && r.size() >= h + 4 + std::strtoull(r.c_str() + cl + 16, nullptr, 10)) break; }
    return r;
}
TEST(l4_proxy) {
    upstream u1("u1", false), u2("u2", false);
    auto p = std::make_shared<pool>(config{strategy::round_robin}); p->add({"u1", u1.addr()}); p->add({"u2", u2.addr()});
    p->add({"dead", "127.0.0.1:1"});   // refused: retried on a live backend, then its circuit opens
    proxy px(p, {proxy_config::l4}); CHECK(px.start());
    std::set<std::string> who;
    for (int i = 0; i < 9; ++i) {
        auto c = os::socket::connect("127.0.0.1", px.port()); CHECK(c);
        CHECK(c->send_all("hi", 2)); char b[16] = {}; CHECK(c->wait_readable(3000)); long n = c->recv(b, sizeof b); CHECK(n == 5);
        who.insert(std::string(b, 2));
    }
    CHECK(who.size() == 2 && who.count("u1") && who.count("u2"));
    CHECK(px.errors() == 0);
    p->set_health_check(tcp_probe(200)); p->run_health_checks(); p->run_health_checks(); p->run_health_checks();
    for (auto& s : p->stats()) if (s.id == "dead") CHECK(!s.healthy);
    px.stop();
}
TEST(l7_proxy) {
    upstream a("A", true), b("B", true), bad("BAD", true, 503), api("API", true);
    auto p = std::make_shared<pool>(config{strategy::consistent_hash}); p->add({"A", a.addr()}); p->add({"B", b.addr()}); p->add({"BAD", bad.addr()});
    auto apip = std::make_shared<pool>(config{strategy::round_robin}); apip->add({"API", api.addr()});
    proxy_config pc; pc.mode = proxy_config::l7; pc.routes = {{"/api/", apip}};
    pc.connect_timeout_ms = 15000;   // slow emulators (TCG): a timed-out dial would legitimately fail over and break stickiness
    proxy px(p, pc); CHECK(px.start());
    std::map<std::string, std::string> owner;
    for (int s = 0; s < 6; ++s) for (int k = 0; k < 3; ++k) {
        std::string r = http(px.port(), "GET /gen HTTP/1.1\r\nHost: x\r\nX-Session-Id: s" + std::to_string(s) + "\r\nConnection: close\r\n\r\n");
        CHECK(r.rfind("HTTP/1.1 200", 0) == 0);
        std::string up = r.substr(r.find("\r\n\r\n") + 4, 1);
        CHECK(up == "A" || up == "B");   // 503 upstream retried away
        if (!owner.count("s" + std::to_string(s))) owner["s" + std::to_string(s)] = up; else { bool same = owner["s" + std::to_string(s)] == up; if (!same) std::printf("  sticky miss s%d: %s then %s\n%s\n", s, owner["s" + std::to_string(s)].c_str(), up.c_str(), r.c_str()); CHECK(same); }   // sticky
    }
    auto r = http(px.port(), "POST /api/v1/infer HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    CHECK(r.find("API POST /api/v1/infer HTTP/1.1 body=hello") != std::string::npos);
    auto ch = http(px.port(), "POST /api/c HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    CHECK(ch.find("body=abcde") != std::string::npos);
    { // keep-alive: two requests on one client connection
        auto c = os::socket::connect("127.0.0.1", px.port()); CHECK(c);
        for (int i = 0; i < 2; ++i) {
            std::string q = "GET /api/k HTTP/1.1\r\nHost: x\r\n\r\n"; CHECK(c->send_all(q.data(), q.size()));
            std::string resp; char t[4096]; long n;
            while (c->wait_readable(3000) && (n = c->recv(t, sizeof t)) > 0) { resp.append(t, std::size_t(n)); if (resp.find("body=") != std::string::npos) break; }
            CHECK(resp.find("keep-alive") != std::string::npos && resp.find("API GET /api/k") != std::string::npos);
        }
    }
    CHECK(http(px.port(), "garbage\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
    auto down = std::make_shared<pool>(); proxy_config pd; pd.mode = proxy_config::l7; pd.queue_wait_ns = 0; proxy px2(down, pd); CHECK(px2.start());
    CHECK(http(px2.port(), "GET / HTTP/1.1\r\nConnection: close\r\n\r\n").rfind("HTTP/1.1 503", 0) == 0);
    px.stop(); px2.stop();
}
