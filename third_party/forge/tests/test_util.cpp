// stopwatch/deadline, durable atomic writes and the settings registry.
#include "check.hpp"
#include <forge/os/fs.hpp>
#include <forge/os/settings.hpp>
#include <forge/os/time.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace forge;
namespace S = forge::os::settings;

TEST(stopwatch_and_deadline) {
    os::stopwatch w; os::sleep_ns(20000000);
    CHECK(w.elapsed_ms() >= 20); CHECK(w.elapsed_s() >= 0.02);
    auto l1 = w.lap_ns(); os::sleep_ns(5000000); auto l2 = w.lap_ns(); CHECK(l1 >= 20000000ull); CHECK(l2 >= 5000000ull && l2 < l1 + 1000000000ull);
    w.restart(); CHECK(w.elapsed_ms() < 20);
    auto n = os::deadline::never(); CHECK(n.is_never()); CHECK(!n.expired()); CHECK(n.remaining_ms() == ~0ull);
    auto d = os::deadline::after_ms(60);
    CHECK(!d.expired()); CHECK(d.remaining_ms() > 0 && d.remaining_ms() <= 60);
    auto c = d.child(10'000); CHECK(c.at() == d.at());           // a child never outlives its parent
    auto e = d.child(10); CHECK(e.at() < d.at());
    CHECK(e.sleep()); CHECK(e.expired()); CHECK(e.remaining_ns() == 0);
    CHECK(!d.sleep(1000000) || d.expired());
    CHECK(d.sleep()); CHECK(d.expired());
    CHECK(os::deadline::after_ns(~0ull - 5).is_never());          // overflow saturates
    CHECK(n.earlier(d).at() == d.at());
}

TEST(atomic_write_replaces_whole_file) {
    auto dir = std::filesystem::temp_directory_path() / ("forge_util_" + std::to_string(os::pid()));
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    auto p = (dir / "f.txt").string();
    CHECK(!os::fs::write_file_atomic(p, "first").err);
    CHECK(!os::fs::write_file_atomic(p, std::string(100000, 'z')).err);
    { std::ifstream f(p, std::ios::binary); std::string s((std::istreambuf_iterator<char>(f)), {});   // closed before remove_all (Windows)
      CHECK(s == std::string(100000, 'z')); }
    int files = 0; for (auto& e : std::filesystem::directory_iterator(dir)) { (void)e; ++files; }
    CHECK(files == 1);                                             // no temp file left behind
    CHECK(os::fs::write_file_atomic((dir / "missing" / "x").string(), "x").err);
    std::filesystem::remove_all(dir);
}

TEST(settings_registry) {
    static const auto kInt = S::define<std::int64_t>("FORGE_TST_INT", 7, "An int");
    static const auto kSize = S::define<std::uint64_t>("FORGE_TST_SIZE", 1024, "A size");
    static const auto kFlag = S::define<bool>("FORGE_TST_FLAG", false, "A flag");
    static const auto kName = S::define("FORGE_TST_NAME", "dflt", "A string");
    static const auto kReal = S::define<double>("FORGE_TST_REAL", 0.5, "A real");
    forge_test::set_env("FORGE_TST_INT", nullptr); forge_test::set_env("FORGE_TST_SIZE", "2G");
    forge_test::set_env("FORGE_TST_FLAG", "yes"); forge_test::set_env("FORGE_TST_NAME", "abc"); forge_test::set_env("FORGE_TST_REAL", "x");
    S::reload();
    CHECK(kInt.get() == 7); CHECK(!kInt.is_set());
    CHECK(kSize.get() == (2ull << 30)); CHECK(kFlag.get()); CHECK(kName.get() == "abc"); CHECK(kReal.get() == 0.5);
    CHECK(S::problems().size() == 1);
    forge_test::set_env("FORGE_TST_INT", "0x10"); CHECK(kInt.get() == 16);  // a changed variable is re-read
    forge_test::set_env("FORGE_TST_INT", nullptr); CHECK(kInt.get() == 7);  // unset: the default again
    forge_test::set_env("FORGE_TST_INT", "0x10"); S::reload(); CHECK(kInt.get() == 16);
    CHECK(S::get_raw("FORGE_TST_NAME").value_or("") == "abc");
    CHECK_THROWS(S::get_raw("FORGE_TST_NEVER_DEFINED"));
    CHECK_THROWS(S::define<std::int64_t>("FORGE_TST_INT", 8, "conflict"));
    (void)S::define<std::int64_t>("FORGE_TST_INT", 7, "same again is fine");
    forge_test::set_env("FORGE_TST_TYPO", "1");
    auto u = S::undocumented("FORGE_TST_"); CHECK(u.size() == 1 && u[0] == "FORGE_TST_TYPO");
    auto md = S::markdown("FORGE_TST_");
    CHECK(md.find("| `FORGE_TST_SIZE` | size | `1024` | A size |") != std::string::npos);
    CHECK(md.find("FORGE_TST_TYPO") == std::string::npos);
    for (auto* k : {"FORGE_TST_INT", "FORGE_TST_SIZE", "FORGE_TST_FLAG", "FORGE_TST_NAME", "FORGE_TST_REAL", "FORGE_TST_TYPO"}) forge_test::set_env(k, nullptr);
}
TEST(settings_integers_and_unicode) {
    static const auto kInt = S::define<std::int64_t>("FORGE_TST2_INT", 7, "An int");
    static const auto kI32 = S::define<std::int32_t>("FORGE_TST2_I32", 3, "A 32-bit int");
    static const auto kName = S::define("FORGE_TST2_NAME", "dflt", "A string");
#if defined(_WIN32)   // the process (UTF-16) environment, as a parent or any wide API would set it
    auto put = [](const char* k, const char* v) { SetEnvironmentVariableW(os::widen(k).c_str(), v ? os::widen(v).c_str() : nullptr); };
#else
    auto put = [](const char* k, const char* v) { if (v) os::setenv(k, v); else os::unsetenv(k); };
#endif
    put("FORGE_TST2_INT", "010"); S::reload(); CHECK(kInt.get() == 10);   // decimal, not octal
    put("FORGE_TST2_INT", "-0x10"); CHECK(kInt.get() == -16); put("FORGE_TST2_INT", "+12"); CHECK(kInt.get() == 12);
    put("FORGE_TST2_INT", "08"); CHECK(kInt.get() == 8); put("FORGE_TST2_INT", " 9"); CHECK(kInt.get() == 9);
    for (auto* bad : {"0x", "12 ", "1e3", "0b101", "x1", "99999999999999999999"}) { put("FORGE_TST2_INT", bad); S::reload(); CHECK_MSG(kInt.get() == 7, "'%s'", bad); CHECK(S::problems().size() == 1); }
    put("FORGE_TST2_I32", "5000000000"); S::reload(); CHECK(kI32.get() == 3); CHECK(S::problems().size() == 1);
    put("FORGE_TST2_I32", "-2147483648"); CHECK(kI32.get() == INT32_MIN);
    // UTF-8 values and names round-trip through the (UTF-16 on Windows) environment
    put("FORGE_TST2_NAME", "caf\xc3\xa9 \xe2\x9c\x93"); CHECK(kName.is_set()); CHECK(kName.get() == "caf\xc3\xa9 \xe2\x9c\x93");
    CHECK(S::get_raw("FORGE_TST2_NAME").value_or("") == "caf\xc3\xa9 \xe2\x9c\x93");
    put("FORGE_TST2_\xc3\x9c" "BER", "1"); auto u = S::undocumented("FORGE_TST2_"); CHECK(u.size() == 1 && u[0] == "FORGE_TST2_\xc3\x9c" "BER");
    for (auto* k : {"FORGE_TST2_INT", "FORGE_TST2_I32", "FORGE_TST2_NAME", "FORGE_TST2_\xc3\x9c" "BER"}) put(k, nullptr);
    CHECK(!kName.is_set()); CHECK(S::undocumented("FORGE_TST2_").empty());
}
