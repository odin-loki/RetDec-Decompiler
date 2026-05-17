/**
 * @file src/pattern_detect/raii_detect.cpp
 * @brief RAII idiom detector — constructor acquires, destructor releases.
 *
 * ## Structural invariant
 *
 * A compiled RAII type has (across two functions — ctor + dtor):
 *   1. Constructor: a Call to a resource-acquire function, with the result
 *      stored to a struct field.
 *   2. Destructor: a matching Call to the paired resource-release function
 *      on the same struct field.
 *   3. No other significant virtual methods beyond ctor/dtor.
 *
 * ## Acquire/release pairs
 *
 *   Acquire                    Release
 *   fopen                    → fclose
 *   open (syscall)           → close
 *   malloc / calloc          → free
 *   operator new             → operator delete
 *   pthread_mutex_lock       → pthread_mutex_unlock
 *   pthread_rwlock_rdlock/wrlock → pthread_rwlock_unlock
 *   CreateFile               → CloseHandle
 *   CreateMutex              → ReleaseMutex
 *   socket                   → closesocket
 *   regcomp                  → regfree
 *   curl_easy_init           → curl_easy_cleanup
 *   SDL_Init                 → SDL_Quit
 *   dlopen                   → dlclose
 *
 * ## Single-function mode
 *
 * When only one function is given (intra-procedural), we check whether both
 * an acquire and a matching release call appear in the same function — this
 * covers scope-guard patterns that acquire and release within the same scope.
 *
 * ## Confidence scoring (group mode)
 *
 *   acquire call in ctor   +0.45
 *   release call in dtor   +0.45
 *   matched pair           +0.10
 *
 * ## Confidence scoring (single-function mode)
 *
 *   acquire + matched release in same fn  → 0.70
 */

#include "retdec/pattern_detect/pattern_detect.h"
#include "retdec/ssa/ssa.h"

#include <unordered_map>

namespace retdec {
namespace pattern_detect {

namespace {

static const std::unordered_map<std::string, std::string> kAcquireReleasePairs = {
    { "fopen",               "fclose"                 },
    { "open",                "close"                  },
    { "malloc",              "free"                   },
    { "calloc",              "free"                   },
    { "_Znwm",               "_ZdlPv"                 },
    { "operator new",        "operator delete"        },
    { "pthread_mutex_lock",  "pthread_mutex_unlock"   },
    { "pthread_rwlock_rdlock","pthread_rwlock_unlock" },
    { "pthread_rwlock_wrlock","pthread_rwlock_unlock" },
    { "CreateFile",          "CloseHandle"            },
    { "CreateMutex",         "ReleaseMutex"           },
    { "socket",              "closesocket"            },
    { "regcomp",             "regfree"                },
    { "curl_easy_init",      "curl_easy_cleanup"      },
    { "SDL_Init",            "SDL_Quit"               },
    { "dlopen",              "dlclose"                },
    { "mmap",                "munmap"                 },
    { "sem_open",            "sem_close"              },
};

} // anonymous namespace

bool RAIIDetector::isAcquireCall(const std::string& callee) const {
    return kAcquireReleasePairs.count(callee) > 0;
}

bool RAIIDetector::isReleaseCall(const std::string& callee) const {
    for (const auto& [acq, rel] : kAcquireReleasePairs)
        if (rel == callee) return true;
    return false;
}

std::string RAIIDetector::matchingRelease(const std::string& acquire) const {
    auto it = kAcquireReleasePairs.find(acquire);
    return it != kAcquireReleasePairs.end() ? it->second : "";
}

RAIIEvidence RAIIDetector::analyse(const ssa::SSAFunction& fn) const {
    RAIIEvidence ev;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            if (isAcquireCall(i->calleeName)) {
                ev.hasAcquireInCtor = true;
                ev.acquireName = i->calleeName;
            }
            if (isReleaseCall(i->calleeName)) {
                ev.hasReleaseInDtor = true;
                ev.releaseName = i->calleeName;
            }
        }
    }
    if (!ev.acquireName.empty() && !ev.releaseName.empty()) {
        auto expected = matchingRelease(ev.acquireName);
        ev.hasMatchingPair = (expected == ev.releaseName);
    }
    ev.found = ev.hasAcquireInCtor || ev.hasReleaseInDtor;
    ev.confidence = score(ev);
    return ev;
}

float RAIIDetector::score(const RAIIEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasAcquireInCtor) s += 0.45f;
    if (ev.hasReleaseInDtor) s += 0.45f;
    if (ev.hasMatchingPair)  s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

PatternResult RAIIDetector::detect(const ssa::SSAFunction& fn) const {
    PatternResult r;
    r.kind = PatternKind::RAII;
    auto ev = analyse(fn);
    r.confidence = ev.confidence;
    if (ev.confidence >= 0.45f) {
        std::string acq = ev.acquireName.empty() ? "acquire()" : ev.acquireName + "()";
        std::string rel = ev.releaseName.empty() ? "release()" : ev.releaseName + "()";
        r.emittedForm =
            "struct RAIIHandle {\n"
            "    Resource* res_;\n"
            "    RAIIHandle() { res_ = " + acq + "; }\n"
            "    ~RAIIHandle() { " + rel + "; }\n"
            "    RAIIHandle(const RAIIHandle&) = delete;\n"
            "    RAIIHandle& operator=(const RAIIHandle&) = delete;\n"
            "};";
        r.comment = "// Design pattern: RAII (" + acq + " / " + rel + ")";
    }
    return r;
}

PatternResult RAIIDetector::detectGroup(
        const std::vector<const ssa::SSAFunction*>& fns) const {
    PatternResult r;
    r.kind = PatternKind::RAII;
    RAIIEvidence combined;
    for (const auto* fn : fns) {
        if (!fn) continue;
        auto ev = analyse(*fn);
        if (ev.hasAcquireInCtor && combined.acquireName.empty()) {
            combined.hasAcquireInCtor = true;
            combined.acquireName = ev.acquireName;
        }
        if (ev.hasReleaseInDtor && combined.releaseName.empty()) {
            combined.hasReleaseInDtor = true;
            combined.releaseName = ev.releaseName;
        }
    }
    if (!combined.acquireName.empty() && !combined.releaseName.empty()) {
        auto expected = matchingRelease(combined.acquireName);
        combined.hasMatchingPair = (expected == combined.releaseName);
    }
    r.confidence = score(combined);
    if (r.confidence >= 0.45f) {
        std::string acq = combined.acquireName.empty() ? "acquire()" : combined.acquireName + "()";
        std::string rel = combined.releaseName.empty() ? "release()" : combined.releaseName + "()";
        r.emittedForm =
            "struct RAIIHandle {\n"
            "    Resource* res_;\n"
            "    RAIIHandle() { res_ = " + acq + "; }\n"
            "    ~RAIIHandle() { " + rel + "; }\n"
            "    RAIIHandle(const RAIIHandle&) = delete;\n"
            "    RAIIHandle& operator=(const RAIIHandle&) = delete;\n"
            "};";
        r.comment = "// Design pattern: RAII (" + acq + " / " + rel + ")";
    }
    return r;
}

} // namespace pattern_detect
} // namespace retdec
