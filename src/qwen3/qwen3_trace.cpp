/**
 * @file src/qwen3/qwen3_trace.cpp
 */

#include "retdec/qwen3/qwen3_trace.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <string.h>
#else
#include <strings.h>
#endif

namespace retdec::qwen3 {
namespace {

#if defined(_WIN32)
int envIeq(const char* a, const char* b) { return _stricmp(a, b) == 0; }
#else
int envIeq(const char* a, const char* b) { return strcasecmp(a, b) == 0; }
#endif

bool envTruthy(const char* v) {
    if (!v || !v[0]) return false;
    return envIeq(v, "1") || envIeq(v, "true") || envIeq(v, "yes") || envIeq(v, "on");
}

int cacheFlag(const char* name) {
    const char* v = std::getenv(name);
    return envTruthy(v) ? 1 : 0;
}

} // namespace

bool traceEnabled() {
    static int s = -1;
    if (s < 0) s = cacheFlag("RETDEC_QWEN3_TRACE");
    return s != 0;
}

bool traceVerbose() {
    static int s = -1;
    if (s < 0) s = cacheFlag("RETDEC_QWEN3_TRACE_VERBOSE");
    return s != 0;
}

void tracef(const char* fmt, ...) {
    if (!traceEnabled()) return;

    using Clock = std::chrono::steady_clock;
    static Clock::time_point t0{};
    static bool              haveT0 = false;
    if (!haveT0) {
        t0     = Clock::now();
        haveT0 = true;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - t0)
                        .count();

    std::fprintf(stderr, "[retdec-qwen3 +%lld ms] ", static_cast<long long>(ms));

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);

    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

} // namespace retdec::qwen3
