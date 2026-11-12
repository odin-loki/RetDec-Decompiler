#include "check.hpp"
#include "tiny_model.hpp"
#include <forge/core/memory.hpp>
#include <forge/inference/inference.hpp>
#include <cstring>
#include <filesystem>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace forge;
static std::vector<int> greedy(inference::session& s, std::vector<int> p, int n) { inference::sampler g; s.reset(); return s.generate(p, n, g); }

TEST(parse_and_format_sizes) {
    CHECK(mem::parse_size("512") == 512u);
    CHECK(mem::parse_size("4k") == 4096u);
    CHECK(mem::parse_size("1.5G") == 1610612736u);
    CHECK(mem::parse_size("2GiB") == 2147483648u);
    CHECK(mem::parse_size("8 MB") == 8u << 20);
    CHECK(mem::parse_size("50%", 1000) == 500u);
    CHECK(!mem::parse_size("50%"));
    CHECK(!mem::parse_size("lots"));
    CHECK(!mem::parse_size("-1G"));
    CHECK(mem::format_size(1536) == "1.5 KiB");
}
TEST(host_limits_detected) {
    auto h = mem::detect_host_limits();
    CHECK(h.physical > 0);
    CHECK(h.effective() > 0 && h.effective() <= h.physical);
    std::printf("  physical=%s available=%s cgroup=%s\n", mem::format_size(h.physical).c_str(), mem::format_size(h.available).c_str(), mem::format_size(h.cgroup).c_str());
}
TEST(budget_reserve_release_and_tags) {
    mem::budget b("t", 1000);
    { auto a = b.reserve(400, "weights"); auto c = b.try_reserve(500, "kv");
      CHECK(a && c); CHECK(b.used() == 900); CHECK(b.stats().by_tag["weights"] == 400);
      CHECK(!b.try_reserve(200)); CHECK(b.stats().denials == 1);
      CHECK_THROWS(b.reserve(200, "x"));
      CHECK(c.resize(550)); CHECK(b.used() == 950); CHECK(!c.resize(700)); CHECK(c.resize(100)); CHECK(b.used() == 500);
      auto moved = std::move(a); CHECK(b.used() == 500); }
    CHECK(b.used() == 0); CHECK(b.stats().peak == 950);
    CHECK_THROWS(b.reserve(5000));
    try { b.reserve(5000); } catch (const forge_exception& e) { CHECK(e.code == errc::out_of_memory); }
}
TEST(budget_evicts_caches_lowest_priority_lru_first) {
    mem::budget b("t", 1000);
    std::uint64_t a = 300, c = 300; std::vector<std::string> order;
    int ia = b.add_cache("a", [&] { return a; }, [&](std::uint64_t) { order.push_back("a"); auto f = a; a = 0; return f; }, 0);
    b.add_cache("c", [&] { return c; }, [&](std::uint64_t) { order.push_back("c"); auto f = c; c = 0; return f; }, 0);
    b.touch_cache(ia);   // a is now most recently used
    auto l = b.reserve(600, "work");
    CHECK(b.cache_bytes() == 600);
    // Reserve treats caches as outside used_, so they only get evicted when asked or under trim().
    CHECK(b.under_pressure());
    auto freed = b.trim(); CHECK(freed == 300); CHECK(order.size() == 1 && order[0] == "c");
    CHECK(b.evict(0) == 300); CHECK(order.size() == 2 && order[1] == "a");
    CHECK(b.stats().evictions == 2 && b.stats().evicted_bytes == 600);
}
TEST(budget_try_reserve_evicts_to_fit) {
    mem::budget b("t", 1000);
    auto held = b.reserve(500, "a");
    mem::lease spare = b.reserve(400, "cache");
    b.add_cache("c", [&] { return spare.bytes(); }, [&](std::uint64_t) { auto n = spare.bytes(); spare.release(); return n; });
    auto l = b.try_reserve(300, "new");   // only fits once the cache gives its lease back
    CHECK(l); CHECK(b.used() == 800);
}
TEST(budget_reserve_waits_for_release) {
    mem::budget b("t", 1000);
    auto a = b.reserve(900);
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); a.release(); });
    auto l = b.reserve(500, "late", std::chrono::milliseconds(5000));
    t.join(); CHECK(l); CHECK(b.stats().waits == 1);
}
TEST(budget_set_usage_and_prometheus) {
    mem::budget b("host", 1 << 20);
    b.set_usage("kv", 4096); CHECK(b.used() == 4096);
    b.set_usage("kv", 1024); CHECK(b.used() == 1024);
    auto p = b.prometheus();
    CHECK(p.find("forge_mem_limit_bytes{pool=\"host\"} 1048576") != std::string::npos);
    CHECK(p.find("forge_mem_used_bytes_by_tag{pool=\"host\",tag=\"kv\"} 1024") != std::string::npos);
    b.set_usage("kv", 0); CHECK(b.used() == 0);
}
TEST(pmr_resource_charges_budget) {
    mem::budget b("t", 64 << 10);
    mem::budget_resource r(b, "vec");
    { std::pmr::vector<char> v(&r); v.resize(16 << 10); CHECK(b.used() >= (16u << 10)); CHECK_THROWS(v.resize(128 << 10)); }
    CHECK(b.used() == 0);
}
TEST(governor_env_and_pressure) {
    ::setenv("FORGE_MEM_DEVICE_LIMIT", "1M", 1);
    auto& g = mem::governor::instance();
    CHECK(g.pool("testdev:0").limit() == (1u << 20));
    CHECK(g.host().limit() > 0);
    std::uint64_t c = 100; g.pool("testdev:0").add_cache("c", [&] { return c; }, [&](std::uint64_t) { auto f = c; c = 0; return f; });
    g.on_pressure(os::pressure_level::critical);
    CHECK(c == 0); CHECK(g.pressure_events() >= 1);
    CHECK(g.prometheus().find("forge_mem_pressure_events_total") != std::string::npos);
    CHECK(g.describe().find("budget   host") != std::string::npos);
}
TEST(spill_roundtrip_and_cap) {
    auto dir = std::filesystem::temp_directory_path() / ("forge_spill_test_" + std::to_string(::getpid()));
    {
        mem::spill_store s(dir);
        std::vector<int> v(1000); for (int i = 0; i < 1000; ++i) v[std::size_t(i)] = i * 3;
        auto k = s.put(v.data(), v.size() * sizeof(int)); CHECK(k); CHECK(s.bytes() == 4000);
        auto back = s.take(k); CHECK(back && back->size() == 4000);
        CHECK(std::memcmp(back->data(), v.data(), 4000) == 0); CHECK(s.bytes() == 0); CHECK(!s.take(k));
    }
    CHECK(!std::filesystem::exists(dir));
}
TEST(scheduler_reports_kv_and_defers_under_budget) {
    auto path = (std::filesystem::temp_directory_path() / ("forge_test_mem_" + std::to_string(::getpid()) + ".fpk")).string();
    forge_test::make_tiny_model(path);
    {   // scope: the model's mapping must be gone before the file is deleted (Windows)
    auto m = inference::model::open(path);
    mem::budget b("kvtest", 0);
    std::vector<std::vector<int>> outs(4), want(4);
    for (int i = 0; i < 4; ++i) { inference::session s(m); want[std::size_t(i)] = greedy(s, {256, i + 1, 2}, 6); }
    {   // unlimited: behaves exactly like before but reports KV usage
        inference::batch_scheduler s(m, {.max_active = 4, .prefix_cache = 2, .budget = &b});
        for (int i = 0; i < 4; ++i) s.submit({.prompt = {256, i + 1, 2}, .max_new = 6, .on_done = [&, i](const std::vector<int>& o, const std::string&) { outs[std::size_t(i)] = o; }});
        s.run_until_idle();
        CHECK(s.stats().peak_active == 4); CHECK(s.stats().deferred == 0);
        CHECK(b.cache_bytes() > 0);
    }
    for (int i = 0; i < 4; ++i) CHECK(outs[std::size_t(i)] == want[std::size_t(i)]);
    CHECK(b.used() == 0); CHECK(b.cache_bytes() == 0);
    b.set_limit(1);   // absurdly tight: requests must run one at a time but still all finish, same output
    std::fill(outs.begin(), outs.end(), std::vector<int>{});
    inference::batch_scheduler s(m, {.max_active = 4, .prefix_cache = 2, .budget = &b});
    for (int i = 0; i < 4; ++i) s.submit({.prompt = {256, i + 1, 2}, .max_new = 6, .on_done = [&, i](const std::vector<int>& o, const std::string&) { outs[std::size_t(i)] = o; }});
    s.run_until_idle();
    CHECK(s.stats().peak_active == 1); CHECK(s.stats().deferred > 0); CHECK(s.stats().completed == 4);
    for (int i = 0; i < 4; ++i) CHECK(outs[std::size_t(i)] == want[std::size_t(i)]);
    }
    std::filesystem::remove(path);
}

// session::save_prefix / load_prefix: a prefix restored from a blob continues with bit-identical logits.
TEST(kv_prefix_blob_roundtrip_identical_logits) {
    auto path = (std::filesystem::temp_directory_path() / ("forge_test_kvblob_" + std::to_string(::getpid()) + ".fpk")).string();
    forge_test::make_tiny_model(path);
    {
    auto m = inference::model::open(path);
    const std::vector<int> p{256, 5, 9, 3, 7, 1};
    const int t = 11;
    inference::session a(m), b(m), fresh(m);
    a.forward(p);
    auto blob = a.save_prefix();
    CHECK(!blob.empty());
    b.load_prefix(blob);
    CHECK(b.position() == a.position());
    auto la = a.forward(std::span<const int>(&t, 1)), lb = b.forward(std::span<const int>(&t, 1));
    fresh.forward(p); auto lf = fresh.forward(std::span<const int>(&t, 1));
    CHECK(la == lb); CHECK(lb == lf);   // bitwise
    // a shorter prefix behaves exactly like copy_prefix
    inference::session c(m), d(m);
    c.copy_prefix(a, 3); d.load_prefix(a.save_prefix(3));
    CHECK(d.position() == 3);
    auto lc = c.forward(std::span<const int>(&t, 1)), ld = d.forward(std::span<const int>(&t, 1));
    CHECK(lc == ld);
    std::vector<std::byte> junk(blob.begin(), blob.begin() + 20);
    CHECK_THROWS(d.load_prefix(junk));
    }
    std::filesystem::remove(path);
}

// The batch scheduler spills evicted prefix KV to a spill_store and reloads it when a prompt hits it.
TEST(scheduler_spills_and_reloads_prefix_kv) {
    auto path = (std::filesystem::temp_directory_path() / ("forge_test_spill_" + std::to_string(::getpid()) + ".fpk")).string();
    auto dir = std::filesystem::temp_directory_path() / ("forge_spill_sched_" + std::to_string(::getpid()));
    forge_test::make_tiny_model(path);
    {
    auto m = inference::model::open(path);
    const std::vector<int> p1{256, 10, 11, 12, 13, 14}, p2{256, 20, 21, 22}, p3{256, 10, 11, 12, 13, 14, 15, 16};
    std::vector<int> want1, want2, want3;
    { inference::session s(m); want1 = greedy(s, p1, 5); want2 = greedy(s, p2, 5); want3 = greedy(s, p3, 5); }
    auto run = [&](inference::batch_scheduler& s, const std::vector<int>& p) {
        std::vector<int> out; s.submit({.prompt = p, .max_new = 5, .on_done = [&](const std::vector<int>& o, const std::string&) { out = o; }});
        s.run_until_idle(); return out;
    };
    mem::spill_store sp(dir);
    {
        inference::batch_scheduler s(m, {.max_active = 1, .prefix_cache = 1, .spill = &sp});
        CHECK(run(s, p1) == want1);
        CHECK(run(s, p2) == want2);   // p1's prefix leaves memory (capacity 1): spilled, not dropped
        CHECK(s.stats().prefix_spills == 1); CHECK(s.spilled() == 1); CHECK(sp.bytes() > 0);
        auto reused = s.stats().prefix_tokens_reused;
        CHECK(run(s, p3) == want3);   // shares p1's 6 tokens: read back from disk, rows copied, same tokens out
        CHECK(s.stats().prefix_reloads == 1);
        CHECK(s.stats().prefix_tokens_reused - reused >= p1.size());
        CHECK(s.stats().prefix_spills == 3);   // the reload pushed p2's prefix out, retiring p3 pushed p1's out
        CHECK(s.spilled() == 2);
        // memory pressure: every cached prefix goes to disk, nothing stays in memory, and a hit still works
        s.request_kv_eviction(0); s.step();
        CHECK(s.spilled() == 3);
        CHECK(run(s, p3) == want3);
        CHECK(s.stats().prefix_reloads == 2);
    }
    CHECK(sp.bytes() == 0);   // the scheduler drops its spilled prefixes when it goes away
    {   // without a spill store the evicted prefix is recomputed: same output, no hit
        inference::batch_scheduler s(m, {.max_active = 1, .prefix_cache = 1});
        run(s, p1); run(s, p2);
        auto hits = s.stats().prefix_tokens_reused;
        CHECK(run(s, p3) == want3);
        CHECK(s.stats().prefix_tokens_reused - hits < p1.size());
    }
    }
    std::filesystem::remove(path);
}

#if defined(FORGE_OS_POSIX) && !defined(FORGE_OS_IOS)
TEST(spawn_leased_charges_and_returns_cap) {
    forge::mem::budget b("children", 1ull << 30);
    forge::os::spawn_spec s; s.argv = {"sh", "-c", "sleep 0.1; exit 4"};
    auto c1 = forge::mem::spawn_leased(b, s, 600ull << 20, "z3");
    CHECK(c1); CHECK(b.used() == 600ull << 20); CHECK(c1->cap() == 600ull << 20);
    CHECK_THROWS(forge::mem::spawn_leased(b, s, 600ull << 20, "z3"));   // would over-promise the budget
    CHECK(b.used() == 600ull << 20);
    CHECK(c1->wait() == 4); CHECK(b.used() == 0);
    auto c2 = forge::mem::spawn_leased(b, s, 600ull << 20, "z3"); CHECK(c2);
    forge::os::child* w[] = {&c2->proc};
    CHECK(forge::os::wait_any(w)); c2->reap_if_done(); CHECK(b.used() == 0);
    forge::os::spawn_spec bad; bad.argv = {"/nonexistent/forge-nope"};
    CHECK(!forge::mem::spawn_leased(b, bad, 1 << 20)); CHECK(b.used() == 0);
    {   // waiting reservation: a second child gets room once the first exits
        auto a = forge::mem::spawn_leased(b, s, 900ull << 20);
        auto t0 = forge::os::monotonic_ns();
        std::thread t([&] { a->wait(); });
        auto d = forge::mem::spawn_leased(b, s, 900ull << 20, "child", std::chrono::milliseconds(5000));
        t.join(); CHECK(d); CHECK(forge::os::monotonic_ns() - t0 >= 50000000ull); d->wait();
    }
    CHECK(b.used() == 0);
}
#endif
