/**
 * @file src/string_detect/sso_detect.cpp
 * @brief SSO (Short String Optimisation) branch recognition.
 *
 * ## Short String Optimisation review
 *
 * Every major C++ standard library implements SSO for std::string.
 * When a string is short enough, its character data is stored directly
 * in the string object itself rather than on the heap.  The exact threshold
 * differs by implementation:
 *
 * | Implementation      | SSO threshold | Object size (64-bit) |
 * |---------------------|---------------|----------------------|
 * | libstdc++ (GCC)     | 15 chars      | 32 bytes             |
 * | MSVC STL            | 15 chars      | 32 bytes             |
 * | libc++ (Clang/LLVM) | 22 chars      | 24 bytes             |
 * | Folly fbstring      | 23 chars      | 24 bytes             |
 *
 * The compiler emits a conditional branch around the heap allocation:
 *
 *   libstdc++ / MSVC:
 *     CMP len, 15
 *     JBE inline_path      ; unsigned ≤ 15: use inline buffer
 *
 *   libc++:
 *     CMP len, 22
 *     JBE inline_path
 *
 *   Sometimes the comparison is offset by 1 (JB vs JBE semantics):
 *     CMP len, 16   ; with JB  → effective threshold 15
 *
 * We match on the IMMEDIATE value of the comparison (15, 16, 22, 23, 24)
 * and record which implementation it implies.
 *
 * ## What we emit
 *
 * A SSOBranchInfo records the threshold and both branch targets.  The caller
 * uses this to annotate the inline path as "string stored inline, no pointer
 * to .rodata" — which prevents the false variable-extraction of the inline
 * buffer as a raw pointer.
 */

#include "retdec/string_detect/string_detect.h"
#include <optional>

namespace retdec {
namespace string_detect {

uint32_t ssoThreshold(SSOImpl impl) noexcept {
    switch (impl) {
    case SSOImpl::LibStdCpp:     return 15;
    case SSOImpl::MsvcStl:       return 15;
    case SSOImpl::LibCpp:        return 22;
    case SSOImpl::FollyFBString: return 23;
    }
    return 15;
}

std::optional<SSOBranchInfo> detectSSOBranch(int64_t  compareImm,
                                               uint64_t branchVma,
                                               uint64_t inlinePath,
                                               uint64_t heapPath) noexcept
{
    // Map immediate → (impl, effective threshold)
    // We accept both JBE form (CMP len, N → JBE) and JB form (CMP len, N+1 → JB)
    // The lookup is first-match, so a second row for an immediate already
    // listed can never be reached. Two such rows were here -- { 15, MsvcStl }
    // and { 16, MsvcStl }, both shadowed by the LibStdCpp rows above them --
    // which made SSOImpl::MsvcStl unreturnable. That is not a table to repair
    // by reordering: libstdc++ and the MSVC STL both inline 15 characters, so
    // the compare immediate genuinely cannot tell them apart, and one of the
    // two has to be the answer. The threshold, which is what callers act on,
    // is 15 either way.
    //
    // 23 is a real ambiguity of the same kind, between libc++'s JB form and
    // folly's JBE form; libc++ wins it, which is what the note about
    // preferring libc++ meant. It never applied to libstdc++, whose
    // immediates (15, 16) do not collide with libc++'s (22, 23).
    struct Entry { int64_t imm; SSOImpl impl; uint32_t threshold; };
    static constexpr Entry table[] = {
        { 15, SSOImpl::LibStdCpp,     15 },  // GCC/MSVC  CMP N, 15 + JBE
        { 16, SSOImpl::LibStdCpp,     15 },  // GCC/MSVC  CMP N, 16 + JB
        { 22, SSOImpl::LibCpp,        22 },  // libc++    CMP N, 22 + JBE
        { 23, SSOImpl::LibCpp,        22 },  // libc++    CMP N, 23 + JB, wins over folly
        { 24, SSOImpl::FollyFBString, 23 },
    };

    for (auto& e : table) {
        if (e.imm == compareImm) {
            SSOBranchInfo info;
            info.impl       = e.impl;
            info.branchVma  = branchVma;
            info.inlinePath = inlinePath;
            info.heapPath   = heapPath;
            info.threshold  = e.threshold;
            return info;
        }
    }
    return std::nullopt;
}

} // namespace string_detect
} // namespace retdec
