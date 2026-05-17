/**
 * @file src/ssa/liveness.cpp
 * @brief Backward dataflow liveness analysis.
 *
 * ## Algorithm
 *
 * Classic iterative backward dataflow (Aho, Lam, Sethi, Ullman §9.2):
 *
 *   GEN[B]  = {v : v is used in B before any definition of v in B}
 *   KILL[B] = {v : v is defined in B}
 *
 *   live_in[B]  = GEN[B] ∪ (live_out[B] \ KILL[B])
 *   live_out[B] = ∪ { live_in[S] : S ∈ succs(B) }
 *
 * Iteration order: we process blocks in reverse post-order (RPO) of the
 * *reverse* CFG — i.e. in post-order of the forward CFG.  This gives the
 * best convergence behaviour because information flows backward along
 * control-flow edges.
 *
 * ## Convergence
 *
 * For reducible CFGs (> 99% of compiler output), the algorithm converges
 * in depth_of_loop_nesting + 2 iterations.  In practice this is 3–5.
 * We iterate until no live set changes (fixed-point condition).
 *
 * ## Complexity
 *
 * O(n × k × d) where n = number of blocks, k = number of variables,
 * d = iterations until convergence.  Using bitsets for live sets would give
 * O(n × k/w × d) but we use unordered_set<VarId> for clarity.
 *
 * ## Flag variables
 *
 * EFLAGS is treated as a single pseudo-variable with a fixed reserved VarId
 * (`kFlagsVarId`).  Every flag-writing instruction defines it; every
 * flag-reading instruction uses it.  The FlagBundle analysis later
 * determines which specific bits are used/defined.
 */

#include "retdec/ssa/ssa.h"
#include <algorithm>
#include <cassert>
#include <queue>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa {

// ─── GEN / KILL computation ───────────────────────────────────────────────────

void LivenessAnalysis::computeGenKill(SSAFunction& fn) {
    for (auto& blkPtr : fn.blocks()) {
        BasicBlock& blk = *blkPtr;
        blk.gen.clear();
        blk.kill.clear();

        for (auto* instr : blk.instrs) {
            // Uses: if not already killed in this block, it's upward-exposed
            for (auto& use : instr->uses) {
                // We track VarId-level uses here; ValueId is not yet assigned
                // At this pre-SSA stage, uses carry a varId via the value table
                const IrValue* v = fn.value(use.valueId);
                if (v && v->varId != kInvalidVar) {
                    if (!blk.kill.count(v->varId))
                        blk.gen.insert(v->varId);
                }
            }

            // Flag uses
            if (instr->readsFlagBundle) {
                // Treat kFlagsVarId (UINT32_MAX-1) as the flags pseudo-variable
                constexpr VarId kFlagsVarId = UINT32_MAX - 1;
                if (!blk.kill.count(kFlagsVarId))
                    blk.gen.insert(kFlagsVarId);
            }

            // Definitions
            if (instr->defVar != kInvalidVar)
                blk.kill.insert(instr->defVar);
            if (instr->writesFlagBundle) {
                constexpr VarId kFlagsVarId = UINT32_MAX - 1;
                blk.kill.insert(kFlagsVarId);
            }
        }
    }
}

// ─── Iterative fixed-point ────────────────────────────────────────────────────

void LivenessAnalysis::run(SSAFunction& fn) {
    computeGenKill(fn);

    // Initialise live sets to empty
    for (auto& blkPtr : fn.blocks()) {
        blkPtr->liveIn.clear();
        blkPtr->liveOut.clear();
    }

    // Build a post-order traversal of blocks for the worklist.
    // We process blocks in post-order so that successor information is ready
    // before processing a block.
    std::vector<BlockId> postOrder;
    {
        std::vector<bool> visited(fn.blockCount(), false);
        std::function<void(BlockId)> dfs = [&](BlockId bid) {
            visited[bid] = true;
            if (auto* blk = fn.block(bid)) {
                for (BlockId s : blk->succs)
                    if (!visited[s]) dfs(s);
            }
            postOrder.push_back(bid);
        };
        if (fn.blockCount() > 0)
            dfs(fn.entryId());
        // Pick up any unreachable blocks
        for (std::size_t i = 0; i < fn.blockCount(); ++i)
            if (!visited[i]) dfs((BlockId)i);
    }

    bool changed = true;
    iterations_ = 0;

    while (changed) {
        changed = false;
        ++iterations_;

        // Process in post-order (successors before predecessors for backward flow)
        for (BlockId bid : postOrder) {
            BasicBlock* blk = fn.block(bid);
            if (!blk) continue;

            // live_out[B] = ∪ live_in[S] for S in succs(B)
            std::unordered_set<VarId> newOut;
            for (BlockId s : blk->succs) {
                BasicBlock* succ = fn.block(s);
                if (!succ) continue;
                for (VarId v : succ->liveIn)
                    newOut.insert(v);
            }

            // live_in[B] = GEN[B] ∪ (live_out[B] \ KILL[B])
            std::unordered_set<VarId> newIn = blk->gen;
            for (VarId v : newOut)
                if (!blk->kill.count(v))
                    newIn.insert(v);

            if (newOut != blk->liveOut || newIn != blk->liveIn) {
                blk->liveOut = std::move(newOut);
                blk->liveIn  = std::move(newIn);
                changed = true;
            }
        }
    }
}

bool LivenessAnalysis::isLiveIn(const SSAFunction& fn,
                                  BlockId bid, VarId var) const {
    const BasicBlock* blk = fn.block(bid);
    if (!blk) return false;
    return blk->liveIn.count(var) != 0;
}

} // namespace ssa
} // namespace retdec
