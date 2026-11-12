#pragma once
// Fuzz harness glue. Each fuzz_<target>.cpp defines LLVMFuzzerTestOneInput; it links either with
// libFuzzer (-DFORGE_LIBFUZZER=ON, clang -fsanitize=fuzzer) or with driver.cpp, a self-contained
// mutational fuzzer (seed replay + byte flips / inserts / deletes / splices / interesting values)
// used in CI so every target runs on every build. "Expected" failures are forge exceptions
// (format/parse errors); anything else -- a crash, a sanitizer report, an unexpected exception type,
// a hang -- is a finding.
#include <forge/core/error.hpp>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#if defined(_WIN32)
#include <process.h>
#define FORGE_FUZZ_GETPID _getpid
#else
#include <unistd.h>
#define FORGE_FUZZ_GETPID ::getpid
#endif

namespace forge::fuzz {
// readers that mmap a path: materialise the input in a per-process temp file
inline std::string as_file(const std::uint8_t* d, std::size_t n, const char* ext) {
    static std::string base = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/forge_fuzz_" + std::to_string(FORGE_FUZZ_GETPID());
    std::string p = base + ext;
    std::ofstream f(p, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(d), std::streamsize(n));
    return p;
}
template <class F> int guarded(F&& f) {
    try { f(); } catch (const forge::forge_exception&) {}  // rejected input: fine
    return 0;
}
} // namespace forge::fuzz

#define FORGE_FUZZ(...)                                                                    \
    extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {    \
        return forge::fuzz::guarded([&] { __VA_ARGS__ });                                          \
    }
