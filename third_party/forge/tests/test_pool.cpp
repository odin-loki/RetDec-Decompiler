#include "check.hpp"
#include <forge/task/pool.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace forge;
using namespace std::chrono_literals;

TEST(submit_returns_values_and_exceptions) {
    task::pool p(4);
    auto a = p.submit([] { return 6 * 7; });
    auto b = p.submit([] { return std::string("ok"); });
    auto c = p.submit([]() -> int { throw std::runtime_error("boom"); });
    auto d = p.submit([] {});
    CHECK(a.get() == 42); CHECK(b.get() == "ok"); CHECK_THROWS(c.get()); d.get();
    std::atomic<int> n{0}; std::vector<std::future<void>> fs;
    for (int i = 0; i < 500; ++i) fs.push_back(p.submit([&] { ++n; }));
    for (auto& f : fs) f.get();
    CHECK(n == 500);
}

TEST(priority_order_with_one_worker) {
    task::pool p(2);   // one worker thread
    std::mutex m; std::vector<int> order;
    std::atomic<bool> go{false};
    auto blocker = p.submit([&] { while (!go) std::this_thread::sleep_for(1ms); });
    std::this_thread::sleep_for(20ms);   // the worker is now busy; the rest queue up
    std::vector<std::future<void>> fs;
    for (int pr : {0, 5, 1, 5, -3}) fs.push_back(p.submit([&, pr] { std::lock_guard l(m); order.push_back(pr); }, {.priority = pr}));
    go = true; blocker.get(); for (auto& f : fs) f.get();
    CHECK((order == std::vector<int>{5, 5, 1, 0, -3}));
}

TEST(cancellation_before_and_during) {
    task::pool p(2);
    std::atomic<bool> go{false};
    auto blocker = p.submit([&] { while (!go) std::this_thread::sleep_for(1ms); });
    std::stop_source src; std::atomic<bool> ran{false};
    auto f = p.submit([&] { ran = true; }, {.stop = src.get_token()});
    src.request_stop(); go = true; blocker.get();
    bool threw = false; try { f.get(); } catch (const task::cancelled&) { threw = true; }
    CHECK(threw); CHECK(!ran);
    std::stop_source s2; std::atomic<bool> started{false};
    auto g = p.submit([&](std::stop_token st) { started = true; int spins = 0; while (!st.stop_requested() && spins < 5000) { std::this_thread::sleep_for(1ms); ++spins; } return spins; }, {.stop = s2.get_token()});
    while (!started) std::this_thread::sleep_for(1ms);
    s2.request_stop(); CHECK(g.get() < 5000);
}

TEST(single_thread_pool_runs_inline_and_wait_idle) {
    task::pool one(1);
    auto f = one.submit([] { return 1; }); CHECK(f.wait_for(0s) == std::future_status::ready); CHECK(f.get() == 1);
    task::pool p(3); std::atomic<int> n{0};
    for (int i = 0; i < 50; ++i) (void)p.submit([&] { std::this_thread::sleep_for(1ms); ++n; });
    p.wait_idle(); CHECK(n == 50); CHECK(p.pending() == 0);
}

TEST(parallel_for_still_works_alongside_tasks) {
    task::pool p(4); std::atomic<long> sum{0};
    auto t = p.submit([] { std::this_thread::sleep_for(30ms); return 1; });
    p.parallel_for(10000, 64, [&](std::size_t b, std::size_t e) { long s = 0; for (auto i = b; i < e; ++i) s += long(i); sum += s; });
    CHECK(sum == 10000L * 9999 / 2); CHECK(t.get() == 1);
    CHECK(task::submit([] { return 3; }).get() == 3);
}

TEST(parallel_for_claims_cover_every_item_once) {
    task::pool p(6);
    for (std::size_t n : {std::size_t(1), std::size_t(7), std::size_t(1000), std::size_t(100003)}) {
        for (auto [lo, hi] : {std::pair<std::size_t, std::size_t>{1, std::size_t(-1)}, {16, 256}, {16, 16}, {1000, 1000}}) {
            std::vector<std::atomic<int>> hit(n);
            std::atomic<std::size_t> maxc{0};
            for (int rep = 0; rep < 3; ++rep)
                p.parallel_for(n, lo, hi, [&](std::size_t b, std::size_t e) {
                    std::size_t c = e - b, m = maxc.load(); while (c > m && !maxc.compare_exchange_weak(m, c)) {}
                    for (auto i = b; i < e; ++i) ++hit[i];
                });
            bool ok = true; for (auto& h : hit) ok &= h == 3;
            CHECK_MSG(ok, "n=%zu lo=%zu hi=%zu", n, lo, hi);
            CHECK(n <= lo || maxc <= hi);
        }
    }
}

TEST(parallel_for_nested_and_concurrent_and_throwing) {
    task::pool p(4);
    std::atomic<long> sum{0};
    p.parallel_for(64, 1, [&](std::size_t b, std::size_t e) {   // nested calls run inline: no deadlock
        for (auto i = b; i < e; ++i) p.parallel_for(100, 1, [&](std::size_t b2, std::size_t e2) { sum += long(e2 - b2); });
    });
    CHECK(sum == 6400);
    std::vector<std::thread> ts; std::atomic<long> tot{0};   // several callers at once queue for the pool
    for (int t = 0; t < 4; ++t) ts.emplace_back([&] { for (int i = 0; i < 200; ++i) p.parallel_for(1000, 8, [&](std::size_t b, std::size_t e) { tot += long(e - b); }); });
    for (auto& t : ts) t.join();
    CHECK(tot == 4L * 200 * 1000);
    bool threw = false;
    try { p.parallel_for(10000, 1, [&](std::size_t b, std::size_t) { if (b > 5000) throw std::runtime_error("x"); }); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    sum = 0; p.parallel_for(10000, 1, [&](std::size_t b, std::size_t e) { sum += long(e - b); });   // still usable
    CHECK(sum == 10000);
}

TEST(parallel_for_wakes_sleeping_workers) {
    task::pool p(4);
    std::this_thread::sleep_for(20ms);   // well past the spin time (FORGE_SPIN_US, 200 us): the workers are asleep
    std::mutex m; std::set<std::thread::id> ids;
    for (int i = 0; i < 50 && ids.size() < 2; ++i)
        p.parallel_for(4000, 1, [&](std::size_t, std::size_t) { std::this_thread::sleep_for(100us); std::lock_guard l(m); ids.insert(std::this_thread::get_id()); });
    CHECK(ids.size() >= 2);
}

TEST(destructor_drains_queue) {
    std::atomic<int> n{0};
    { task::pool p(2); for (int i = 0; i < 20; ++i) (void)p.submit([&] { ++n; }); }
    CHECK(n == 20);
}
