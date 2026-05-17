/**
 * @file src/ssa/ssa_rename.cpp
 * @brief SSA renaming pass (Cytron et al. algorithm with FlagBundle and MemRef).
 *
 * ## Algorithm
 *
 * After phi placement, every variable's uses and definitions are renamed to
 * versioned SSA values.  We use the recursive DFS algorithm from Cytron et
 * al. §4, adapted for our three special value kinds:
 *
 *   FlagBundle  — flag-writing instructions define a FlagBundle SSA value.
 *                 Flag-reading instructions receive that bundle as input and
 *                 extract the specific flag bit they need.
 *
 *   MemRef      — stack slot accesses (based on a SP/BP register + constant
 *                 offset) are allocated as MemRef values.  They are NOT
 *                 renamed through phi functions — each access is a distinct
 *                 MemRef.  Promotion to SSA scalar happens later (Stage 18).
 *
 *   Undef       — if a variable is used before any reaching definition
 *                 (e.g. a register that is live-in to the entry block but
 *                 has no explicit assignment), an UndefValue is allocated.
 *
 * ## Pass structure
 *
 *   1. For each block B in dominator-tree pre-order:
 *      a. Rename each phi result (the LHS of phi nodes in B).
 *      b. Rename each instruction in B:
 *         - Replace each use (VarId) with the top of the def stack.
 *         - Rename the definition (allocate new SSA value, push to stack).
 *      c. For each successor S of B:
 *         Fill in phi operands for B in S's phi nodes.
 *      d. Recurse into dominator-tree children.
 *      e. Pop all defs pushed in this block.
 *
 * ## Def stacks
 *
 * We maintain one stack per VarId: stacks[varId] = [v_current, ...].
 * The top of the stack is the current reaching definition.
 *
 * At entry to the entry block we push one UndefValue per variable to
 * ensure the stack is never empty during renaming.
 */

#include "retdec/ssa/ssa.h"
#include <cassert>
#include <functional>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace ssa {

static constexpr VarId kFlagsVarId = UINT32_MAX - 1;

// ─── Stack helpers ────────────────────────────────────────────────────────────
// Uses unordered_map to avoid allocating UINT32_MAX entries when kFlagsVarId
// (= UINT32_MAX-1) is used as a key.

ValueId SSARename::currentDef(const std::unordered_map<VarId, std::vector<ValueId>>& stacks,
                                VarId var) const {
    if (var == kInvalidVar) return kInvalidValue;
    auto it = stacks.find(var);
    if (it == stacks.end() || it->second.empty()) return kInvalidValue;
    return it->second.back();
}

void SSARename::pushDef(std::unordered_map<VarId, std::vector<ValueId>>& stacks,
                         VarId var, ValueId val) {
    if (var == kInvalidVar) return;
    stacks[var].push_back(val);
}

void SSARename::popDefs(std::unordered_map<VarId, std::vector<ValueId>>& stacks,
                          const std::vector<std::pair<VarId,ValueId>>& pushed) {
    for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) {
        VarId var = it->first;
        if (var == kInvalidVar) continue;
        auto sit = stacks.find(var);
        if (sit != stacks.end() && !sit->second.empty())
            sit->second.pop_back();
    }
}

// ─── Fill phi operands ────────────────────────────────────────────────────────

void SSARename::fillPhiOperands(SSAFunction& fn,
                                  BlockId blk,
                                  std::unordered_map<VarId, std::vector<ValueId>>& stacks) {
    // For each successor of blk, fill in our contribution to its phi nodes
    BasicBlock* b = fn.block(blk);
    if (!b) return;

    for (BlockId succId : b->succs) {
        BasicBlock* succ = fn.block(succId);
        if (!succ) continue;
        for (PhiNode* phi : succ->phis) {
            // Find if blk is already registered as a predecessor operand
            bool found = false;
            for (auto& [predId, _] : phi->operands)
                if (predId == blk) { found = true; break; }
            if (found) continue;

            ValueId reaching = currentDef(stacks, phi->varId);
            if (reaching == kInvalidValue) {
                // No definition reaches this phi from this predecessor.
                // Allocate an Undef value.
                IrValue* undef = fn.allocValue(ValueKind::Undef, phi->varId);
                reaching = undef->id;
            }
            phi->addOperand(blk, reaching);
        }
    }
}

// ─── Per-block renaming ───────────────────────────────────────────────────────

void SSARename::renameBlock(SSAFunction& fn,
                              BlockId blkId,
                              std::unordered_map<VarId, std::vector<ValueId>>& stacks) {
    BasicBlock* blk = fn.block(blkId);
    if (!blk) return;

    // Track all definitions pushed in this block so we can pop them later
    std::vector<std::pair<VarId,ValueId>> pushed;

    // (a) Rename phi node results (LHS)
    for (PhiNode* phi : blk->phis) {
        IrValue* val = fn.allocValue(ValueKind::Phi, phi->varId);
        val->defPhi   = phi;
        phi->result   = val->id;
        pushDef(stacks, phi->varId, val->id);
        pushed.push_back({phi->varId, val->id});
    }

    // (b) Rename each instruction
    for (IrInstr* instr : blk->instrs) {
        // Rename uses
        for (Use& use : instr->uses) {
            // Use carries a pre-SSA ValueId whose varId we look up
            IrValue* old = fn.value(use.valueId);
            if (!old) continue;
            VarId var = old->varId;

            if (old->kind == ValueKind::MemRef) {
                // MemRef: keep as-is, no renaming through phi
                continue;
            }
            if (old->kind == ValueKind::Immediate) {
                // Immediate constants are not variables; keep as-is.
                continue;
            }

            ValueId reaching = currentDef(stacks, var);
            if (reaching == kInvalidValue) {
                IrValue* undef = fn.allocValue(ValueKind::Undef, var);
                reaching = undef->id;
                pushDef(stacks, var, reaching);
                pushed.push_back({var, reaching});
            }
            use.valueId = reaching;
        }

        // Special: flag-reading instruction uses the current flag bundle
        if (instr->readsFlagBundle) {
            ValueId flagReach = currentDef(stacks, kFlagsVarId);
            if (flagReach == kInvalidValue) {
                IrValue* undef = fn.allocValue(ValueKind::Undef, kFlagsVarId);
                undef->kind = ValueKind::FlagBundle;
                flagReach = undef->id;
                pushDef(stacks, kFlagsVarId, flagReach);
                pushed.push_back({kFlagsVarId, flagReach});
            }
            instr->flagBundleInput = flagReach;
        }

        // Rename definition
        if (instr->defVar != kInvalidVar && instr->op != IrInstr::Op::Phi) {
            ValueKind kind = ValueKind::VirtualReg;
            if (instr->writesFlagBundle) kind = ValueKind::FlagBundle;

            IrValue* def = fn.allocValue(kind, instr->defVar);
            def->defInstr = instr;
            instr->defValue = def->id;

            if (kind == ValueKind::FlagBundle) {
                def->definedFlags = instr->flagMask;
                instr->flagBundleValue = def->id;
                pushDef(stacks, kFlagsVarId, def->id);
                pushed.push_back({kFlagsVarId, def->id});
            } else {
                pushDef(stacks, instr->defVar, def->id);
                pushed.push_back({instr->defVar, def->id});
            }
        }

        // MemRef store: allocate a MemRef value representing the new slot state
        if (instr->op == IrInstr::Op::Store) {
            // Find the MemRef use and allocate a def MemRef
            for (auto& use : instr->uses) {
                IrValue* v = fn.value(use.valueId);
                if (v && v->kind == ValueKind::MemRef) {
                    IrValue* newRef = fn.allocValue(ValueKind::MemRef, v->varId);
                    newRef->memBaseReg = v->memBaseReg;
                    newRef->memOffset  = v->memOffset;
                    newRef->memWidth   = v->memWidth;
                    newRef->memIsStack = v->memIsStack;
                    newRef->defInstr   = instr;
                    instr->defValue = newRef->id;
                    pushDef(stacks, v->varId, newRef->id);
                    pushed.push_back({v->varId, newRef->id});
                    break;
                }
            }
        }
    }

    // (c) Fill phi operands in successors
    fillPhiOperands(fn, blkId, stacks);

    // (d) Recurse into dominator-tree children
    for (BlockId child : blk->domChildren) {
        renameBlock(fn, child, stacks);
    }

    // (e) Pop all defs pushed in this block
    popDefs(stacks, pushed);
}

// ─── Main entry ───────────────────────────────────────────────────────────────

void SSARename::run(SSAFunction& fn) {
    // Use unordered_map to avoid flat-vector allocation for kFlagsVarId
    // (UINT32_MAX-1), which would require allocating ~4 billion entries.
    std::unordered_map<VarId, std::vector<ValueId>> stacks;

    // Seed stacks: for each variable, push one Undef at entry so the stack
    // is never empty (handles live-in variables with no explicit definition).
    for (VarId v = 0; v < fn.varCount(); ++v) {
        IrValue* undef = fn.allocValue(ValueKind::Undef, v);
        stacks[v].push_back(undef->id);
    }
    // Seed flags pseudo-variable
    {
        IrValue* undef = fn.allocValue(ValueKind::FlagBundle, kFlagsVarId);
        undef->kind = ValueKind::Undef;
        stacks[kFlagsVarId].push_back(undef->id);
    }

    renameBlock(fn, fn.entryId(), stacks);
}

} // namespace ssa
} // namespace retdec
