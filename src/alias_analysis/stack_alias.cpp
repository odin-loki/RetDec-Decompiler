/**
 * @file src/alias_analysis/stack_alias.cpp
 * @brief Exact stack frame alias analysis.
 *
 * ## Correctness argument
 *
 * Every stack access within one activation record has a unique frame-relative
 * offset.  Two accesses to [off_A, off_A + sz_A) and [off_B, off_B + sz_B)
 * alias precisely when those byte intervals overlap.  This is exact
 * (not conservative) because:
 *
 *   1. Stack frames are not shared between distinct activations: two calls to
 *      the same function get distinct frames.
 *
 *   2. We only run this analysis within a single function's activation record.
 *      Intra-procedural accesses with different offsets provably refer to
 *      different bytes.
 *
 *   3. When the address of a stack slot is taken (LEA/ADDROF), EscapeAnalysis
 *      marks that slot as potentially escaping.  Such slots are excluded from
 *      the stack analysis and handed to the Steensgaard tier.
 *
 * ## Three-way result
 *
 *   NoAlias   — byte ranges are completely disjoint:
 *                 [offA + szA ≤ offB] OR [offB + szB ≤ offA]
 *
 *   MustAlias — byte ranges are identical:
 *                 offA == offB  AND  szA == szB
 *
 *   MayAlias  — ranges overlap but are not identical:
 *                 Example: 4-byte access to [RBP-8] and 8-byte access to
 *                 [RBP-8] overlap but don't cover the same bytes.
 */

#include "retdec/alias_analysis/alias_analysis.h"

namespace retdec {
namespace alias_analysis {

bool StackAliasAnalysis::byteRangesOverlap(
    int64_t offA, uint8_t szA, int64_t offB, uint8_t szB) noexcept {

    int64_t endA = offA + (int64_t)szA;
    int64_t endB = offB + (int64_t)szB;
    return offA < endB && offB < endA;
}

AliasResult StackAliasAnalysis::alias(const MemLoc& a,
                                        const MemLoc& b) const noexcept {
    // Caller is responsible for ensuring both are Stack kind.
    if (!byteRangesOverlap(a.offset, a.size, b.offset, b.size))
        return AliasResult::NoAlias;
    if (a.offset == b.offset && a.size == b.size)
        return AliasResult::MustAlias;
    return AliasResult::MayAlias;
}

// ─── GlobalAliasAnalysis ──────────────────────────────────────────────────────

void GlobalAliasAnalysis::addAccess(uint64_t addr, uint8_t size, uint32_t ssaId) {
    accesses_.push_back({addr, size, ssaId});
}

AliasResult GlobalAliasAnalysis::alias(const MemLoc& a,
                                         const MemLoc& b) const noexcept {
    // Both must be Global kind; offsets are absolute addresses.
    uint64_t addrA = (uint64_t)a.offset;
    uint64_t addrB = (uint64_t)b.offset;

    uint64_t endA = addrA + a.size;
    uint64_t endB = addrB + b.size;

    bool overlaps = addrA < endB && addrB < endA;
    if (!overlaps) return AliasResult::NoAlias;
    if (addrA == addrB && a.size == b.size) return AliasResult::MustAlias;
    return AliasResult::MayAlias;
}

} // namespace alias_analysis
} // namespace retdec
