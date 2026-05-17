/**
 * @file src/dce/dead_propagation.cpp
 * @brief Backward SSA def-use liveness propagation from C-semantic live roots.
 *
 * Algorithm (worklist-based, backward through SSA def-use chains):
 *
 *   1. Seed the worklist with all live root InstrIds.
 *   2. For each live instruction I in the worklist:
 *      a. For each USE operand V of I:
 *         - Find the defining instruction D of V (via V->defInstr).
 *         - If D is not in the live set AND D is not an ABI artifact → enqueue.
 *      b. For phi nodes: also mark the instructions that define each
 *         incoming operand of any phi whose result is used by I.
 *   3. Mark ALL control-flow instructions (Branch, CondBranch, Ret) live
 *      unconditionally — they are structural and must be emitted.
 *   4. Any instruction not in the live set after convergence is dead.
 *
 * Note: we do *not* propagate through ABI artifact instructions, even if
 * their output is transitively used.  The ABI marker has already confirmed
 * they are safe to eliminate.
 */

#include "retdec/dce/dce.h"
#include "retdec/ssa/ssa.h"

#include <queue>

namespace retdec {
namespace dce {

std::unordered_set<InstrId> DeadPropagation::run(
        const ssa::SSAFunction& fn,
        const std::vector<LiveRoot>& roots,
        const std::unordered_set<InstrId>& abiArtifactInstrs) const {

    std::unordered_set<InstrId> live;
    std::queue<InstrId> worklist;

    auto enqueue = [&](InstrId id) {
        if (id == UINT32_MAX) return;
        if (abiArtifactInstrs.count(id)) return;  // never propagate through artifacts
        if (live.insert(id).second) worklist.push(id);
    };

    // Seed from live roots.
    for (const auto& r : roots) enqueue(r.instrId);

    // All control-flow instructions are unconditionally live (structural).
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Branch    ||
                instr->op == ssa::IrInstr::Op::CondBranch ||
                instr->op == ssa::IrInstr::Op::Ret) {
                enqueue(instr->id);
            }
        }
    }

    // Build a fast lookup: PhiNode result value → phi node ptr.
    std::unordered_map<ssa::ValueId, const ssa::PhiNode*> phiByResult;
    for (const auto& phi : fn.phis()) {
        if (phi) phiByResult[phi->result] = phi.get();
    }

    // Backward propagation.
    while (!worklist.empty()) {
        InstrId cur = worklist.front();
        worklist.pop();

        const ssa::IrInstr* instr = fn.instr(cur);
        if (!instr) continue;

        // Propagate through all USE operands.
        for (const auto& use : instr->uses) {
            ssa::ValueId vid = use.valueId;
            const ssa::IrValue* val = fn.value(vid);
            if (!val) continue;

            // Case 1: defined by an instruction.
            if (val->defInstr) {
                enqueue(val->defInstr->id);
            }

            // Case 2: defined by a phi node → mark all incoming definitions.
            if (val->defPhi) {
                for (const auto& [pred, incomingVid] : val->defPhi->operands) {
                    const ssa::IrValue* incVal = fn.value(incomingVid);
                    if (incVal && incVal->defInstr) {
                        enqueue(incVal->defInstr->id);
                    }
                }
            }

            // Case 3: the value's id is a phi result → mark its operands live.
            auto phiIt = phiByResult.find(vid);
            if (phiIt != phiByResult.end()) {
                const ssa::PhiNode* phi = phiIt->second;
                for (const auto& [pred, incomingVid] : phi->operands) {
                    const ssa::IrValue* incVal = fn.value(incomingVid);
                    if (incVal && incVal->defInstr) {
                        enqueue(incVal->defInstr->id);
                    }
                }
            }
        }

        // Also process flag bundle inputs (for flag-consuming instructions).
        if (instr->readsFlagBundle && instr->flagBundleInput != ssa::kInvalidValue) {
            const ssa::IrValue* flagVal = fn.value(instr->flagBundleInput);
            if (flagVal && flagVal->defInstr) {
                enqueue(flagVal->defInstr->id);
            }
        }
    }

    return live;
}

} // namespace dce
} // namespace retdec
