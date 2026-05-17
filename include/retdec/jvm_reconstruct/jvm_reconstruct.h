/**
 * @file include/retdec/jvm_reconstruct/jvm_reconstruct.h
 * @brief JVM stack-to-variable reconstruction pipeline — top-level entry point.
 *
 * ## Pipeline
 *
 *   BcCFG (from JvmLifter, stack-based)
 *       │
 *       ▼  JvmStackSim
 *   StackSimResult (named stack slots, type annotations)
 *       │
 *       ▼  SlotCoalescer
 *   CoalesceResult (which slots inline, which survive as locals)
 *       │
 *       ▼  LocalRebuilder + ExceptionVarIntroducer
 *   LocalRebuildResult (BcLocalVar list, slot→local map)
 *       │
 *       ▼  PatternLifter
 *   PatternLiftResult (string concat, lambda, for-each annotations)
 *       │
 *       ▼  BcCFG rewriter
 *   BcCFG (variable-based, fully typed, ready for Java emitter)
 *
 * The final rewriting pass converts stack-based instructions to
 * variable-based ones:
 *   - Every PushInt / LoadLocal / etc. that feeds into a surviving slot
 *     becomes an explicit assignment to the slot's local variable.
 *   - Every pop (the consuming instruction's input) becomes a load of
 *     that local variable.
 *   - Coalesced slots are inlined: the expression is moved directly to
 *     the use site.
 *
 * ## Usage
 *
 *   JvmReconstructor rec;
 *   auto result = rec.reconstruct(method, lvtEntries);
 *   // method.cfg is now variable-based
 *   // result.patterns carries lambda / for-each annotations
 */

#ifndef RETDEC_JVM_RECONSTRUCT_JVM_RECONSTRUCT_H
#define RETDEC_JVM_RECONSTRUCT_JVM_RECONSTRUCT_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/jvm_reconstruct/local_rebuild.h"
#include "retdec/jvm_reconstruct/pattern_lift.h"
#include "retdec/jvm_reconstruct/slot_coalesce.h"
#include "retdec/jvm_reconstruct/stack_sim.h"

#include <string>
#include <vector>

namespace retdec {
namespace jvm_reconstruct {

// ─── Pipeline options ─────────────────────────────────────────────────────────

struct ReconstructOptions {
    StackSimOptions   stackSim;
    LocalRebuildOptions locals;
    bool detectPatterns    = true; ///< Run PatternLifter
    bool rewriteCFG        = true; ///< Rewrite BcCFG to variable form in-place
    bool insertPhiNodes    = false;///< Insert explicit φ-functions (for SSA output)
};

// ─── Full result ──────────────────────────────────────────────────────────────

struct ReconstructResult {
    enum Status { OK, PartialError, Error };
    Status status = OK;
    std::string error;
    std::vector<std::string> warnings;

    StackSimResult     stackSim;
    CoalesceResult     coalesce;
    LocalRebuildResult locals;
    PatternLiftResult  patterns;
};

// ─── Pipeline orchestrator ────────────────────────────────────────────────────

/**
 * @brief Runs the full JVM reconstruction pipeline on one method.
 *
 * After successful reconstruction, `method.cfg` contains variable-based
 * instructions and `method.locals` is populated.
 */
class JvmReconstructor {
public:
    explicit JvmReconstructor(ReconstructOptions opts = ReconstructOptions{});

    /**
     * @brief Reconstruct one BcMethod in place.
     *
     * @param method     The method to reconstruct (its `cfg` is rewritten).
     * @param lvtEntries  LocalVariableTable entries (may be empty).
     */
    ReconstructResult reconstruct(BcMethod& method,
                                   const std::vector<LVTEntry>& lvtEntries = {});

    /**
     * @brief Reconstruct all methods in a BcModule.
     *
     * Returns the total number of reconstruction errors.
     */
    int reconstructModule(BcModule& module);

private:
    ReconstructOptions opts_;

    // Phase 5: rewrite BcCFG from stack to variable form.
    void rewriteCFG(BcCFG& cfg,
                    BcMethod& method,
                    const StackSimResult& sim,
                    const CoalesceResult& coalesce,
                    const LocalRebuildResult& locals);

    // Create a synthetic StoreLocal instruction for a slot assignment.
    BcInstruction makeStore(uint32_t localIdx, uint32_t slotId,
                            const StackSimResult& sim,
                            uint32_t instrOffset) const;

    // Create a synthetic LoadLocal instruction for a slot use.
    BcInstruction makeLoad(uint32_t localIdx,
                           const BcType& type,
                           uint32_t instrOffset) const;
};

} // namespace jvm_reconstruct
} // namespace retdec

#endif // RETDEC_JVM_RECONSTRUCT_JVM_RECONSTRUCT_H
