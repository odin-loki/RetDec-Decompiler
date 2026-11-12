// Minimal mutational fuzz driver (used when libFuzzer isn't available).
//   fuzz_x [--iters N] [--seconds S] [--seed K] <corpus files or dirs...>
// Replays every corpus input, then mutates them. On an unexpected exception it writes the input to
// ./crash-<target>-<n> and aborts, so a ctest run fails loudly with a reproducer on disk.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <csignal>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

static std::vector<std::uint8_t> g_cur;
static void dump_and_die(const char* why) {
    std::ofstream("crash-input.bin", std::ios::binary).write(reinterpret_cast<const char*>(g_cur.data()), std::streamsize(g_cur.size()));
    std::fprintf(stderr, "FINDING: %s (input of %zu bytes saved to crash-input.bin)\n", why, g_cur.size());
    std::abort();
}
static void run_one(const std::vector<std::uint8_t>& in) {
    g_cur = in;
    try { LLVMFuzzerTestOneInput(in.data(), in.size()); }
    catch (const std::exception& e) { std::string m = std::string("unexpected exception: ") + e.what(); dump_and_die(m.c_str()); }
    catch (...) { dump_and_die("unexpected non-std exception"); }
}

static void on_signal(int sig) {
    std::signal(sig, SIG_DFL);
    char why[64]; std::snprintf(why, sizeof why, "signal %d", sig);
    dump_and_die(why);
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    for (int s : {SIGSEGV, SIGFPE, SIGILL}) std::signal(s, on_signal);
#else
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) std::signal(s, on_signal);
    static char altstack[1 << 16]; stack_t ss{}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; sigaltstack(&ss, nullptr);
    struct sigaction sa{}; sa.sa_handler = on_signal; sa.sa_flags = SA_ONSTACK; sigaction(SIGSEGV, &sa, nullptr);
#endif
    long iters = 20000; double seconds = 0; unsigned seed = 1;
    std::vector<std::vector<std::uint8_t>> corpus;
    auto load = [&](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary); corpus.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--iters" && i + 1 < argc) iters = std::atol(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = unsigned(std::atoi(argv[++i]));
        else if (std::filesystem::is_directory(a)) { for (auto& e : std::filesystem::directory_iterator(a)) if (e.is_regular_file()) load(e.path()); }
        else if (std::filesystem::exists(a)) load(a);
    }
    if (corpus.empty()) corpus.push_back({});
    for (auto& c : corpus) run_one(c);
    std::mt19937_64 r(seed);
    static const std::uint64_t interesting[] = {0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0xffffffff, 0x8000000000000000ull, ~0ull, 64, 4096};
    auto t0 = std::chrono::steady_clock::now();
    long done = 0;
    for (; seconds > 0 ? std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds : done < iters; ++done) {
        auto x = corpus[r() % corpus.size()];
        int nm = 1 + int(r() % 8);
        for (int m = 0; m < nm; ++m) {
            switch (r() % 7) {
            case 0: if (!x.empty()) x[r() % x.size()] ^= std::uint8_t(1u << (r() % 8)); break;
            case 1: if (!x.empty()) x[r() % x.size()] = std::uint8_t(r()); break;
            case 2: x.insert(x.begin() + std::ptrdiff_t(x.empty() ? 0 : r() % x.size()), std::uint8_t(r())); break;
            case 3: if (!x.empty()) { auto p = r() % x.size(); x.erase(x.begin() + std::ptrdiff_t(p), x.begin() + std::ptrdiff_t(std::min<std::size_t>(x.size(), std::size_t(p + 1 + r() % 16)))); } break;
            case 4: if (x.size() >= 8) { auto v = interesting[r() % 13]; auto p = r() % (x.size() - 7); std::memcpy(x.data() + p, &v, 1 + r() % 8); } break;
            case 5: { auto& o = corpus[r() % corpus.size()]; if (!o.empty() && !x.empty()) { std::size_t p = std::size_t(r() % o.size()), n = std::min<std::size_t>(o.size() - p, 1 + r() % 64); auto q = r() % x.size(); x.insert(x.begin() + std::ptrdiff_t(q), o.begin() + std::ptrdiff_t(p), o.begin() + std::ptrdiff_t(p + n)); } } break;
            case 6: if (!x.empty()) x.resize(r() % x.size()); break;
            }
        }
        run_one(x);
    }
    std::printf("fuzz: %zu seeds + %ld mutated inputs, no findings\n", corpus.size(), done);
    return 0;
}
