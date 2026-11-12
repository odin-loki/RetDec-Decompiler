// FORGE_LOG_MIN=3 (warn): trace/debug/info calls compile to nothing even when the runtime level allows them.
#define FORGE_LOG_MIN 3
#include "check.hpp"
#include <forge/core/log.hpp>
namespace L = forge::log;
TEST(compile_time_strip) {
    L::set_stderr(false); L::configure("trace");
    std::vector<std::string> got; int id = L::add_sink([&](const L::record&, std::string_view s) { got.emplace_back(s); });
    L::trace("a"); L::debug("b"); L::info("c"); L::logger{"m"}.info("d");
    L::warn("w"); L::logger{"m"}.error("e");
    L::remove_sink(id);
    CHECK(got.size() == 2);
    CHECK(!L::enabled(L::level::info));
    int evals = 0; FORGE_LOG_INFO("m", "{}", ++evals); CHECK(evals == 0);   // stripped: not evaluated
}
