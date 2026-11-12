#include <filesystem>
#include "forge/io/aio.hpp"
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace forge::io;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static void run(bool uring, const std::string& path, const std::vector<std::uint8_t>& ref) {
    async_reader r(16, uring);
    std::printf("backend=%s\n", r.kind() == async_reader::backend::io_uring ? "io_uring" : "threads");
    auto d = r.read_file(path, 65536 + 13);           // odd chunk, more chunks than queue depth
    CHECK(d == ref);
    // scattered reads, including one past EOF
    int fd = ::open(path.c_str(), O_RDONLY);
    std::vector<std::vector<std::uint8_t>> bufs(40, std::vector<std::uint8_t>(4096));
    std::mt19937 g(1);
    std::vector<std::uint64_t> offs(40);
    for (int i = 0; i < 40; ++i) { offs[i] = g() % (ref.size() - 4096); r.read(fd, offs[i], bufs[i].data(), 4096, i); }
    std::vector<std::uint8_t> tail(100); r.read(fd, ref.size() - 10, tail.data(), 100, 99);
    std::size_t got = 0;
    while (got < 41) for (auto& c : r.wait(1)) {
        ++got;
        if (c.user == 99) { CHECK(c.result == 10); CHECK(std::equal(tail.begin(), tail.begin() + 10, ref.end() - 10)); continue; }
        CHECK(c.result == 4096);
        CHECK(std::equal(bufs[c.user].begin(), bufs[c.user].end(), ref.begin() + long(offs[c.user])));
    }
    CHECK(r.inflight() == 0);
    ::close(fd);
}
int main() {
    std::string path = (std::filesystem::temp_directory_path() / "forge_aio_test.bin").string();
    std::vector<std::uint8_t> ref(3 * 1024 * 1024 + 777);
    std::mt19937 g(7); for (auto& b : ref) b = std::uint8_t(g());
    FILE* f = std::fopen(path.c_str(), "wb"); std::fwrite(ref.data(), 1, ref.size(), f); std::fclose(f);
    run(true, path, ref); run(false, path, ref);
    std::remove(path.c_str());
    if (fails) return 1; std::puts("aio ok"); return 0;
}
