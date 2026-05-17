/**
 * @file include/retdec/jvm_reconstruct/slot_coalesce.h
 * @brief Stack-slot coalescing — eliminate unnecessary temporaries.
 *
 * ## Problem
 *
 * After stack simulation, every JVM push/pop pair introduces a fresh stack
 * slot temporary (e.g. `$s0 = x + y; return $s0`).  Many of these
 * temporaries are single-use: the defining instruction's push is immediately
 * consumed by the next instruction's pop.  They add noise to the output.
 *
 * ## Algorithm
 *
 * 1. Build a def-use graph over stack slots within each basic block.
 * 2. A slot is **coalesceable** if:
 *    - It has exactly one definition.
 *    - It has exactly one use.
 *    - The definition and use are in the same basic block.
 *    - No side-effecting instruction intervenes between def and use.
 * 3. Replace coalesceable slots by inlining the definition expression
 *    directly at the use site.
 * 4. After coalescing, slots that survive are promoted to BcLocalVar entries
 *    in the method (with synthetic names `$s0`, `$s1`, etc.) by the
 *    LocalRebuilder.
 *
 * ## Cross-block slots
 *
 * Slots that live across block boundaries (e.g. exception handler entries,
 * loop-carried values) cannot be coalesced and are always promoted to locals.
 */

#ifndef RETDEC_JVM_RECONSTRUCT_SLOT_COALESCE_H
#define RETDEC_JVM_RECONSTRUCT_SLOT_COALESCE_H

#include "retdec/jvm_reconstruct/stack_sim.h"

#include <unordered_set>
#include <vector>

namespace retdec {
namespace jvm_reconstruct {

// ─── Coalescing result ────────────────────────────────────────────────────────

struct CoalesceResult {
    /// Slots that were eliminated (inlined into their single use site).
    std::unordered_set<uint32_t> eliminatedSlots;

    /// Slots that survive and need promotion to BcLocalVar.
    std::vector<uint32_t> survivingSlots;

    /// Map from eliminated slot id → the instruction id that produces it,
    /// so the LocalRebuilder can reconstruct the inline expression.
    std::unordered_map<uint32_t, uint32_t> inlinedSlotToDef;
};

// ─── Coalescer ────────────────────────────────────────────────────────────────

class SlotCoalescer {
public:
    /**
     * @brief Run coalescing on the stack simulation result for one method.
     *
     * @param cfg    The BcCFG (not modified; coalescing is advisory).
     * @param simResult  The output of JvmStackSim::simulate().
     * @return CoalesceResult describing which slots were merged.
     */
    CoalesceResult coalesce(const BcCFG& cfg,
                             const StackSimResult& simResult);

private:
    // Count how many times each slot is used as an input.
    std::unordered_map<uint32_t, uint32_t>
        buildUseCount(const BcCFG& cfg,
                      const StackSimResult& simResult) const;

    // Count how many times each slot is defined (should always be 1 for SSA).
    std::unordered_map<uint32_t, uint32_t>
        buildDefCount(const BcCFG& cfg,
                      const StackSimResult& simResult) const;

    // True if slot id is live across any block boundary.
    bool isCrossBlock(uint32_t slotId,
                      const BcCFG& cfg,
                      const StackSimResult& simResult) const;
};

} // namespace jvm_reconstruct
} // namespace retdec

#endif // RETDEC_JVM_RECONSTRUCT_SLOT_COALESCE_H
