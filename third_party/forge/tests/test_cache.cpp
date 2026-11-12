#include "check.hpp"
#include <forge/cache/cache.hpp>
#include <forge/os/os.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace forge;
namespace fs = std::filesystem;

static fs::path tmpdir(const char* tag) {
    auto d = fs::temp_directory_path() / (std::string("forge_cache_") + tag + "_" + std::to_string(os::pid()));
    fs::remove_all(d); return d;
}

TEST(content_address_roundtrip) {
    auto d = tmpdir("rt");
    { cache::store c(d);
      auto k = c.put("hello");
      CHECK(k == cache::key_of("hello"));
      CHECK(cache::to_hex(k) == "ea8f163db38682925e4491c5e58d4bb3506ef8c14eb78a86e908c5624a67200f");   // BLAKE3("hello")
      CHECK(c.contains(k)); CHECK(c.get(k).value_or("") == "hello");
      CHECK(!c.get(cache::key_of("other")));
      auto s = c.stats(); CHECK(s.entries == 1); CHECK(s.hits == 1); CHECK(s.misses == 1); CHECK(s.bytes == 32 + 5);
      CHECK(fs::exists(c.path_of(k)));
      CHECK(cache::from_hex(cache::to_hex(k)) == k); CHECK(!cache::from_hex("zz")); }
    { cache::store again(d); CHECK(again.stats().entries == 1); CHECK(again.get(cache::key_of("hello")).value_or("") == "hello"); }
    fs::remove_all(d);
}

TEST(memo_keys_and_overwrite_and_erase) {
    auto d = tmpdir("memo"); cache::store c(d);
    auto k1 = cache::key_of({"ab", "c"}), k2 = cache::key_of({"a", "bc"});
    CHECK(k1 != k2);
    CHECK(c.put(k1, "r1")); CHECK(c.put(k1, "result-two")); CHECK(c.get(k1).value_or("") == "result-two");
    CHECK(c.stats().entries == 1); CHECK(c.stats().bytes == 32 + 10);
    CHECK(c.put(k2, std::string(1000, '\0'))); CHECK(c.get(k2)->size() == 1000);
    CHECK(c.erase(k1)); CHECK(!c.erase(k1)); CHECK(!c.get(k1)); CHECK(c.stats().entries == 1);
    c.clear(); CHECK(c.stats().entries == 0); CHECK(c.stats().bytes == 0);
    fs::remove_all(d);
}

TEST(corrupt_entry_is_dropped) {
    auto d = tmpdir("bad"); cache::store c(d);
    auto k = c.put("precious data");
    { std::fstream f(c.path_of(k), std::ios::in | std::ios::out | std::ios::binary); f.seekp(40); f.put('X'); }
    CHECK(!c.get(k)); CHECK(c.stats().corrupt == 1); CHECK(!c.contains(k));
    fs::remove_all(d);
}

TEST(lru_eviction_keeps_recent) {
    auto d = tmpdir("lru"); cache::store c(d, {.max_bytes = 3 * (32 + 100)});
    std::string v(100, 'v');
    auto a = cache::key_of("a"), b = cache::key_of("b"), e = cache::key_of("e"), x = cache::key_of("x");
    c.put(a, v); std::this_thread::sleep_for(std::chrono::milliseconds(20));
    c.put(b, v); std::this_thread::sleep_for(std::chrono::milliseconds(20));
    c.put(e, v); std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(c.get(a)); std::this_thread::sleep_for(std::chrono::milliseconds(20));   // a is now the most recent
    c.put(x, v);
    CHECK(c.contains(a)); CHECK(!c.contains(b)); CHECK(c.contains(e)); CHECK(c.contains(x));
    CHECK(c.stats().evictions == 1); CHECK(c.stats().bytes <= 3 * 132);
    CHECK(c.prune(132) >= 2 * 132); CHECK(c.stats().entries == 1);
    fs::remove_all(d);
}

TEST(concurrent_writers) {
    auto d = tmpdir("mt"); cache::store c(d);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([&, t] { for (int i = 0; i < 50; ++i) { auto s = std::to_string(i % 25) + "-shared"; c.put(s); (void)c.get(cache::key_of(s)); (void)t; } });
    for (auto& t : ts) t.join();
    c.refresh(); CHECK(c.stats().entries == 25);
    for (int i = 0; i < 25; ++i) CHECK(c.get(cache::key_of(std::to_string(i) + "-shared")).has_value());
    fs::remove_all(d);
}
