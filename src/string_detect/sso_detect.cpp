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
    struct Entry { int64_t imm; SSOImpl impl; uint32_t threshold; };
    static constexpr Entry table[] = {
        { 15, SSOImpl::LibStdCpp,     15 },  // GCC/MSVC  CMP N, 15 + JBE
        { 16, SSOImpl::LibStdCpp,     15 },  // GCC/MSVC  CMP N, 16 + JB
        { 15, SSOImpl::MsvcStl,       15 },
        { 16, SSOImpl::MsvcStl,       15 },
        { 22, SSOImpl::LibCpp,        22 },  // libc++    CMP N, 22 + JBE
        { 23, SSOImpl::LibCpp,        22 },  // libc++    CMP N, 23 + JB
        { 23, SSOImpl::FollyFBString, 23 },
        { 24, SSOImpl::FollyFBString, 23 },
    };

    // Try most specific first: prefer libc++ over libstdc++ for ambiguous imm
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
