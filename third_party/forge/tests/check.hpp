#pragma once
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <vector>
namespace forge_test {
inline void set_env(const char* k, const char* v) {   // v = nullptr unsets
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) ::setenv(k, v, 1); else ::unsetenv(k);
#endif
}
}

namespace forge_test {
struct tcase { const char* name; std::function<void()> fn; };
inline std::vector<tcase>& registry() { static std::vector<tcase> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct reg { reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
// Binaries whose tests start a spawn-mode process pool (the only mode on Windows) re-launch themselves as
// workers; they register the serve function here and main() runs it before any test.
using worker_fn = bool (*)(int, char**);
inline worker_fn& worker_hook() { static worker_fn f = nullptr; return f; }
struct worker_reg { explicit worker_reg(worker_fn f) { worker_hook() = f; } };
}
#define TEST_PROCESS_WORKER(fn) static forge_test::worker_reg FT_CAT(wr_, __LINE__)(fn)
#define FT_CAT2(a, b) a##b
#define FT_CAT(a, b) FT_CAT2(a, b)
#define TEST(name) static void name(); static forge_test::reg FT_CAT(r_, name)(#name, name); static void name()
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); ++forge_test::failures(); } } while (0)
#define CHECK_MSG(c, ...) do { if (!(c)) { std::fprintf(stderr, "  CHECK failed %s:%d: %s :: ", __FILE__, __LINE__, #c); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); ++forge_test::failures(); } } while (0)
#define CHECK_THROWS(expr) do { bool t_ = false; try { (void)(expr); } catch (...) { t_ = true; } CHECK_MSG(t_, "expected exception"); } while (0)
int main(int argc, char** argv) {
    if (forge_test::worker_hook() && forge_test::worker_hook()(argc, argv)) return 0;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--forge-process-worker=", 23) == 0) {   // would re-run every test (and recurse)
            std::fprintf(stderr, "%s: started as a process-pool worker but has no TEST_PROCESS_WORKER hook\n", argv[0]);
            return 70;
        }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* only = std::getenv("FORGE_TEST_ONLY");   // substring filter for locating a hang
    for (auto& t : forge_test::registry()) {
        if (only && *only && !std::strstr(t.name, only)) continue;
        int before = forge_test::failures();
        try { t.fn(); } catch (const std::exception& e) { std::fprintf(stderr, "  exception: %s\n", e.what()); ++forge_test::failures(); }
        std::printf("[%s] %s\n", forge_test::failures() == before ? " ok " : "FAIL", t.name);
    }
    std::printf("%d failure(s)\n", forge_test::failures());
    return forge_test::failures() ? 1 : 0;
}

// POSIX shims so the tests also build for Windows (mingw) and run under wine.
#if defined(_WIN32)
#include <cstdlib>
#include <cstdio>
#include <process.h>
inline int setenv(const char* k, const char* v, int overwrite) { if (!overwrite && std::getenv(k)) return 0; return ::_putenv_s(k, v); }
inline int unsetenv(const char* k) { return ::_putenv_s(k, ""); }
#ifndef popen
#define popen _popen
#define pclose _pclose
#endif
#ifndef WIFEXITED
#define WIFEXITED(st) (true)
#define WEXITSTATUS(st) (st)
#endif
#ifndef getpid
#define getpid _getpid
#endif
#endif
