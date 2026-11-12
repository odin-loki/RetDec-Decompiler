#include "check.hpp"
#include <forge/core/log.hpp>
#include <forge/os/os.hpp>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <thread>
namespace L = forge::log;

#if FORGE_LOG_MIN > 0
// Built with -DFORGE_LOG_MIN_LEVEL=...: these tests need every level (test_log_strip covers stripping).
TEST(skip_when_stripped) { std::printf("skip: built with FORGE_LOG_MIN=%d\n", FORGE_LOG_MIN); std::exit(77); }
#endif

struct capture {
    std::vector<std::string> lines; std::vector<std::string> mods; int id;
    capture() { id = L::add_sink([this](const L::record& r, std::string_view s) { lines.emplace_back(s); mods.emplace_back(r.module); }); }
    ~capture() { L::remove_sink(id); }
};

TEST(levels_and_threshold) {
    L::set_stderr(false); L::configure("warn");
    capture c;
    L::info("hidden {}", 1); L::warn("shown {}", 2); L::error("e");
    CHECK(c.lines.size() == 2);
    CHECK(c.lines[0] == "[forge W] shown 2");
    L::set_level(L::level::trace); L::trace("t"); L::debug("d");
    CHECK(c.lines.size() == 4);
    L::set_level(L::level::off); L::error("x"); CHECK(c.lines.size() == 4);
    CHECK(L::parse_level("DEBUG") == L::level::debug);
}

TEST(module_tags_and_overrides) {
    L::configure("error,mind=debug");
    capture c; L::logger m{"mind"}, f{"flow"};
    m.debug("kv {}", 3); f.warn("no"); L::warn("no");
    CHECK(c.lines.size() == 1);
    CHECK(c.lines[0] == "[forge:mind D] kv 3");
    CHECK(c.mods[0] == "mind");
    CHECK(m.enabled(L::level::debug) && !f.enabled(L::level::warn));
    L::set_module_level("flow", L::level::info); f.info("yes"); CHECK(c.lines.size() == 2);
}

TEST(json_format_escapes) {
    L::configure("info"); L::set_format(L::format::json);
    capture c; L::logger{"a\"b"}.info("line\n\"q\"");
    L::set_format(L::format::text);
    CHECK(c.lines.size() == 1);
    CHECK(c.lines[0].find("\"level\":\"info\"") != std::string::npos);
    CHECK(c.lines[0].find("\"module\":\"a\\\"b\"") != std::string::npos);
    CHECK(c.lines[0].find("\"msg\":\"line\\n\\\"q\\\"\"") != std::string::npos);
    CHECK(c.lines[0].find('\n') == std::string::npos);
}

TEST(file_sink_and_threads_do_not_interleave) {
    L::configure("info");
    auto p = (std::filesystem::temp_directory_path() / ("forge_log_" + std::to_string(forge::os::pid()) + ".txt")).string();
    std::filesystem::remove(p);
    int id = L::add_file_sink(p); CHECK(id > 0);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([t] { L::logger g{"th"}; for (int i = 0; i < 200; ++i) g.info("t{}-i{}-{}", t, i, std::string(40, char('a' + t))); });
    for (auto& t : ts) t.join();
    L::remove_sink(id);
    std::ifstream in(p); std::string s; int n = 0; std::set<std::string> seen;
    while (std::getline(in, s)) {
        ++n; CHECK_MSG(s.rfind("[forge:th I] t", 0) == 0, "bad line %s", s.c_str());
        auto tail = s.substr(s.size() - 40); CHECK(tail == std::string(40, tail[0]));
        seen.insert(s);
    }
    CHECK(n == 800); CHECK(seen.size() == 800);
    in.close(); std::filesystem::remove(p);
    CHECK(L::add_file_sink("/nonexistent-dir/x/y.log") == 0);
}

TEST(env_spec_parsing) {
    L::configure("i,mind=t,flow=o");
    CHECK(L::threshold() == L::level::info);
    CHECK(L::threshold("mind") == L::level::trace);
    CHECK(!L::enabled(L::level::error, "flow"));
    CHECK(L::enabled(L::level::trace, "mind"));
    L::configure("warn");
}

TEST(fatal_level_and_macros_skip_arguments) {
    L::configure("warn"); capture c; int evals = 0;
    auto bump = [&] { return ++evals; };
    FORGE_LOG_DEBUG("m", "x {}", bump());   // disabled at run time: argument not evaluated
    CHECK(evals == 0);
    FORGE_LOG_WARN("m", "y {}", bump()); CHECK(evals == 1);
    L::fatal("down"); CHECK(c.lines.size() == 2); CHECK(c.lines[1] == "[forge F] down");
    CHECK(L::parse_level("fatal") == L::level::fatal);
}

TEST(file_sink_rotation) {
    L::configure("info");
    auto p = (std::filesystem::temp_directory_path() / ("forge_rot_" + std::to_string(forge::os::pid()) + ".log")).string();
    for (auto s : {"", ".1", ".2", ".3"}) std::filesystem::remove(p + s);
    int id = L::add_file_sink(p, 200, 2);
    for (int i = 0; i < 30; ++i) L::info("line {:02} {}", i, std::string(20, 'x'));   // 36 bytes each
    L::remove_sink(id);
    CHECK(std::filesystem::exists(p)); CHECK(std::filesystem::exists(p + ".1")); CHECK(std::filesystem::exists(p + ".2"));
    CHECK(!std::filesystem::exists(p + ".3"));
    for (auto s : {"", ".1", ".2"}) CHECK(std::filesystem::file_size(p + s) <= 200);
    { std::ifstream in(p); std::string last, s; while (std::getline(in, s)) last = s;   // closed before remove (Windows)
      CHECK(last.find("line 29") != std::string::npos); }
    for (auto s : {"", ".1", ".2"}) std::filesystem::remove(p + s);
}

// ---- async sink -----------------------------------------------------------------------------------
static std::string tmp_log(const char* tag) {
    return (std::filesystem::temp_directory_path() / (std::string(tag) + std::to_string(forge::os::pid()) + ".log")).string();
}

TEST(async_file_sink_threads_and_flush) {
    L::set_stderr(false); L::configure("info");
    auto p = tmp_log("forge_async_"); std::filesystem::remove(p);
    int id = L::add_async_sink(L::make_file_sink(p), 4096); CHECK(id > 0);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([t] { L::logger g{"th"}; for (int i = 0; i < 200; ++i) g.info("t{}-i{}-{}", t, i, std::string(40, char('a' + t))); });
    for (auto& t : ts) t.join();
    L::flush();
    auto st = L::async_sink_stats(id);
    CHECK(st.written == 800); CHECK(st.dropped == 0); CHECK(st.pending == 0);
    {
        std::ifstream in(p); std::string s; int n = 0; std::set<std::string> seen;
        while (std::getline(in, s)) {
            ++n; CHECK_MSG(s.rfind("[forge:th I] t", 0) == 0, "bad line %s", s.c_str());
            auto tail = s.substr(s.size() - 40); CHECK(tail == std::string(40, tail[0]));
            seen.insert(s);
        }
        CHECK(n == 800); CHECK(seen.size() == 800);
    }
    L::remove_sink(id);
    CHECK(L::async_sink_stats(id).written == 0);   // unknown id after removal
    std::filesystem::remove(p);
}

TEST(async_full_queue_drops_and_reports_without_blocking) {
    L::set_stderr(false); L::configure("info");
    std::atomic<bool> gate{false}, entered{false};
    std::vector<std::string> lines, mods; std::vector<L::level> lvls;   // touched only by the drain thread until flush
    int id = L::add_async_sink([&](const L::record& r, std::string_view s) {
        entered = true;
        while (!gate.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lines.emplace_back(s); mods.emplace_back(r.module); lvls.push_back(r.lvl);
    }, 4);
    L::info("first");
    while (!entered.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // drain thread is now stuck in the sink
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) L::info("burst {}", i);   // 4 fit in the queue, 96 are dropped; none of these waits
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK_MSG(ms < 2000, "logging blocked for %lld ms", (long long)ms);
    auto st = L::async_sink_stats(id);
    CHECK(st.dropped == 96); CHECK(st.pending == 5); CHECK(st.written == 0);
    gate = true;
    L::flush();
    st = L::async_sink_stats(id);
    CHECK(st.written == 5); CHECK(st.dropped == 96); CHECK(st.pending == 0);
    CHECK(lines.size() == 6);
    if (lines.size() == 6) {
        CHECK(lines[0] == "[forge I] first");
        CHECK(lines[1] == "[forge I] burst 0"); CHECK(lines[4] == "[forge I] burst 3");
        CHECK_MSG(lines[5] == "[forge:log W] dropped 96 log line(s): async queue full (capacity 4)", "got %s", lines[5].c_str());
        CHECK(mods[5] == "log"); CHECK(lvls[5] == L::level::warn);
    }
    L::remove_sink(id);
}

TEST(async_remove_sink_drains_queue) {
    L::set_stderr(false); L::configure("info");
    std::atomic<int> got{0};
    int id = L::add_async_sink([&](const L::record&, std::string_view) { std::this_thread::sleep_for(std::chrono::microseconds(200)); ++got; });
    for (int i = 0; i < 50; ++i) L::warn("w {}", i);
    L::remove_sink(id);   // writes the remaining lines and joins the thread
    CHECK(got.load() == 50);
    L::warn("after");     // no longer delivered
    CHECK(got.load() == 50);
}

TEST(async_sink_factories) {
    CHECK(!L::make_file_sink("/nonexistent-dir/x/y.log"));
    CHECK(L::add_async_sink(L::sink{}) == 0);
    auto st = L::async_sink_stats(987654);
    CHECK(st.written == 0 && st.dropped == 0 && st.pending == 0);
    L::flush();   // nothing registered: returns at once
    CHECK(L::make_stderr_sink());
}

// Child half of async_drains_at_exit: does something only when the parent sets FORGE_TEST_LOG_EXIT_FILE.
TEST(async_exit_child) {
    const char* p = std::getenv("FORGE_TEST_LOG_EXIT_FILE");
    if (!p || !*p) return;
    L::set_stderr(false); L::configure("info");
    auto file = L::make_file_sink(p);
    L::add_async_sink([file](const L::record& r, std::string_view s) { std::this_thread::sleep_for(std::chrono::microseconds(300)); file(r, s); });
    for (int i = 0; i < 100; ++i) L::info("exit line {}", i);
    std::exit(0);   // static destruction must drain the queue
}

TEST(async_drains_at_exit) {
    auto p = tmp_log("forge_async_exit_"); std::filesystem::remove(p);
    auto exe = forge::os::exe_path(); CHECK(!exe.empty());
    forge_test::set_env("FORGE_TEST_LOG_EXIT_FILE", p.c_str());
    forge_test::set_env("FORGE_TEST_ONLY", "async_exit_child");
#if defined(_WIN32)
    int rc = std::system(("\"\"" + exe + "\"\"").c_str());
#else
    int rc = std::system(("'" + exe + "'").c_str());
#endif
    forge_test::set_env("FORGE_TEST_LOG_EXIT_FILE", nullptr);
    forge_test::set_env("FORGE_TEST_ONLY", nullptr);
    CHECK(rc == 0);
    int n = 0;
    { std::ifstream in(p); std::string s, last; while (std::getline(in, s)) { ++n; last = s; }   // closed before remove (Windows)
      CHECK(last == "[forge I] exit line 99"); }
    CHECK_MSG(n == 100, "child wrote %d of 100 lines before exit", n);
    std::filesystem::remove(p);
}

#ifdef _WIN32
TEST(eventlog_sink) {
    L::set_stderr(false); L::configure("info");
    CHECK(L::make_eventlog_sink("forge-test"));
    int id = L::add_eventlog_sink("forge-test"); CHECK(id > 0);
    L::warn("forge test_log eventlog_sink {}", 1); L::error("e"); L::info("i");
    L::remove_sink(id);
}
#endif
