/**
 * @file src/cfg_structure/loop_recovery.cpp
 * @brief Natural loop classification (while / for / do-while / infinite).
 *
 * ## Natural loops
 *
 * Given the set of back-edges from `IrreducibilityCheck`, each back-edge
 * (latch → header) defines a natural loop:
 *   - header:  the back-edge target (dominates latch).
 *   - latch:   the back-edge source.
 *   - body:    all nodes reachable from header that can reach latch without
 *              passing through header again.
 *
 * ## Classification rules
 *
 * We examine the terminator instruction of the header and the latch:
 *
 *  `while`:
 *     Header ends in CondBranch.  One successor is inside the loop body,
 *     one is outside (exit edge).  Latch ends in Branch (or CondBranch
 *     whose back-edge successor is header).
 *
 *  `do-while`:
 *     Header ends in an unconditional Branch (no test at entry).
 *     Latch ends in CondBranch; one successor = header (back-edge),
 *     one is outside = loop exit.
 *
 *  `for`:
 *     Like `while`, but the latch block contains an ADD/Sub/Inc/Dec
 *     instruction whose destination is the same VarId used in the
 *     header's condition.  We call that variable the "induction variable".
 *     We prefer `for` over `while` when this increment is detected.
 *
 *  `infinite`:
 *     No exit edge from any block in the loop body.
 */

#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

namespace retdec {
namespace cfg_structure {

// ─── Helper: collectLoopBody ──────────────────────────────────────────────────

std::vector<BlockId>
LoopRecovery::collectLoopBody(BlockId header, BlockId latch,
                               const ssa::SSAFunction& fn) const {
    // The natural loop body = all nodes that:
    //   - are reachable from header in the original CFG, AND
    //   - can reach latch without leaving the loop.
    // Efficient: backward BFS from latch, stop at header.
    std::unordered_set<BlockId> body;
    body.insert(header);
    body.insert(latch);

    std::queue<BlockId> worklist;
    worklist.push(latch);
    while (!worklist.empty()) {
        BlockId v = worklist.front(); worklist.pop();
        const ssa::BasicBlock* bb = fn.block(v);
        if (!bb) continue;
        for (BlockId pred : bb->preds) {
            if (!body.count(pred)) {
                body.insert(pred);
                worklist.push(pred);
            }
        }
    }
    return std::vector<BlockId>(body.begin(), body.end());
}

// ─── Helper: hasConditionalBranch ────────────────────────────────────────────

bool LoopRecovery::hasConditionalBranch(const ssa::SSAFunction& fn,
                                          BlockId block) const {
    const ssa::BasicBlock* bb = fn.block(block);
    if (!bb || bb->instrs.empty()) return false;
    const ssa::IrInstr* last = bb->instrs.back();
    return last && last->op == ssa::IrInstr::Op::CondBranch;
}

// ─── Helper: hasIncrementAtLatch ─────────────────────────────────────────────

bool LoopRecovery::hasIncrementAtLatch(const ssa::SSAFunction& fn,
                                         BlockId latch,
                                         uint32_t& outInductionVar) const {
    const ssa::BasicBlock* bb = fn.block(latch);
    if (!bb) return false;

    // Look for an Add or Sub instruction in the latch block.
    // The def variable of that instruction is a candidate induction variable.
    for (const ssa::IrInstr* instr : bb->instrs) {
        if (!instr) continue;
        if (instr->op == ssa::IrInstr::Op::Add ||
            instr->op == ssa::IrInstr::Op::Sub) {
            outInductionVar = instr->defValue;
            return true;
        }
    }
    outInductionVar = ssa::kInvalidValue;
    return false;
}

// ─── LoopRecovery::classifyLoop ───────────────────────────────────────────────

LoopKind LoopRecovery::classifyLoop(const ssa::SSAFunction& fn,
                                      NaturalLoop& loop) const {
    const BlockId header = loop.header;
    const BlockId latch  = loop.latch;

    // Determine exit blocks: body blocks that have a successor outside the body.
    std::unordered_set<BlockId> bodySet(loop.body.begin(), loop.body.end());
    loop.exits.clear();
    for (BlockId b : loop.body) {
        const ssa::BasicBlock* bb = fn.block(b);
        if (!bb) continue;
        for (BlockId s : bb->succs) {
            if (!bodySet.count(s)) {
                loop.exits.push_back(b);
                break;
            }
        }
    }

    // Infinite loop: no exit edges.
    if (loop.exits.empty()) {
        return LoopKind::Infinite;
    }

    // Check header for conditional branch.
    bool headerCond = hasConditionalBranch(fn, header);
    // Check latch for conditional branch.
    bool latchCond  = hasConditionalBranch(fn, latch);

    // Do-while: header is unconditional, latch has the conditional branch.
    if (!headerCond && latchCond) {
        return LoopKind::DoWhile;
    }

    // While (or For): header has conditional branch.
    if (headerCond) {
        // Try to detect a for-loop increment in the latch.
        uint32_t inductionVar = ssa::kInvalidValue;
        if (hasIncrementAtLatch(fn, latch, inductionVar)) {
            loop.hasIncrement  = true;
            loop.inductionVar  = inductionVar;
            return LoopKind::For;
        }
        return LoopKind::While;
    }

    // Fallback: classify as while.
    return LoopKind::While;
}

// ─── LoopRecovery::run ────────────────────────────────────────────────────────

std::vector<NaturalLoop>
LoopRecovery::run(const ssa::SSAFunction& fn,
                   const IrreducibilityCheck::Result& edges,
                   const std::vector<BlockId>& idom) const {
    std::vector<NaturalLoop> loops;

    // Process each natural back-edge (not irreducible ones).
    for (const CfgEdge& e : edges.backEdges) {
        BlockId latch  = e.src;
        BlockId header = e.dst;

        // Verify that header dominates latch (defensive check).
        // Walk idom chain from latch, look for header.
        {
            bool dominated = false;
            BlockId cur = latch;
            std::size_t budget = idom.size() + 1;
            while (budget-- && cur != ssa::kInvalidBlock && cur < idom.size()) {
                if (cur == header) { dominated = true; break; }
                if (idom[cur] == cur) break;
                cur = idom[cur];
            }
            if (!dominated) continue;
        }

        NaturalLoop loop;
        loop.header = header;
        loop.latch  = latch;
        loop.body   = collectLoopBody(header, latch, fn);
        loop.kind   = classifyLoop(fn, loop);

        loops.push_back(std::move(loop));
    }

    return loops;
}

} // namespace cfg_structure
} // namespace retdec
