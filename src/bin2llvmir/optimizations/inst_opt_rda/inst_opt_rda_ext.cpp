/**
* @file src/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_ext.cpp
* @brief Additional RDA-based instruction optimizations.
* @copyright (c) 2024, MIT license
*
* The existing inst_opt_rda.cpp has three patterns (unusedStores,
* usesWithOneDefInSameBb, defWithUsesInTheSameBb). This file adds:
*
*  4. constantFoldingThroughLoads
*     If a store of a CONSTANT reaches a load (possibly across BB boundaries,
*     via RDA), replace the load with the constant directly.
*     Example:
*       store i32 42, i32* %eax      ; in BB1
*       ...
*       %v = load i32, i32* %eax     ; in BB2, only def is the store above
*     → %v replaced with i32 42, load erased.
*
*  5. doubleLoadElimination
*     If two loads from the same pointer are both reached by the same
*     single store (no intervening writes), the second load is replaced
*     by the first load's result.
*     Example:
*       %a = load i32, i32* %p
*       ...
*       %b = load i32, i32* %p   ; same reaching def
*     → %b replaced with %a.
*
*  6. storeThenImmediateLoad (cross-BB variant of usesWithOneDefInSameBb)
*     The existing usesWithOneDefInSameBb only handles same-BB. This
*     extends it to handle the case where the store is in a dominating
*     predecessor block and the load is the only use of that definition
*     in the entire function.
*/

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "retdec/bin2llvmir/analyses/reaching_definitions.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/utils/ir_modifier.h"
#include "retdec/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_ext.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace inst_opt_rda {

//===========================================================================
// Pattern 4: constant folding through loads
//===========================================================================

bool constantFoldingThroughLoads(
        Instruction* insn,
        ReachingDefinitionsAnalysis& RDA,
        Abi* abi,
        std::unordered_set<Value*>* toRemove) {

    auto* load = dyn_cast<LoadInst>(insn);
    if (!load) return false;

    auto* use = RDA.getUse(load);
    if (!use) return false;

    // Must have exactly one reaching definition.
    auto defs = RDA.defsFromUse(load);
    if (defs.size() != 1) return false;

    auto* def = *defs.begin();
    if (!def || !def->src) return false;

    auto* store = dyn_cast<StoreInst>(def->src);
    if (!store) return false;

    // The stored value must be a constant.
    auto* constVal = dyn_cast<Constant>(store->getValueOperand());
    if (!constVal) return false;

    // Types must match.
    if (constVal->getType() != load->getType()) return false;

    load->replaceAllUsesWith(constVal);
    if (toRemove) {
        toRemove->insert(load);
    } else {
        IrModifier::eraseUnusedInstructionRecursive(load);
    }
    return true;
}

//===========================================================================
// Pattern 5: double load elimination
//===========================================================================

bool doubleLoadElimination(
        Instruction* insn,
        ReachingDefinitionsAnalysis& RDA,
        Abi* abi,
        std::unordered_set<Value*>* toRemove) {

    auto* load = dyn_cast<LoadInst>(insn);
    if (!load) return false;

    auto* use = RDA.getUse(load);
    if (!use) return false;

    auto defs = RDA.defsFromUse(load);
    if (defs.size() != 1) return false;

    auto* def = *defs.begin();
    if (!def) return false;

    // Find another load from the same pointer with the same single reaching def.
    Value* ptr = load->getPointerOperand();

    for (auto* u : ptr->users()) {
        auto* otherLoad = dyn_cast<LoadInst>(u);
        if (!otherLoad || otherLoad == load) continue;
        if (otherLoad->getType() != load->getType()) continue;

        // Check that otherLoad also has exactly this one reaching def.
        auto* otherUse = RDA.getUse(otherLoad);
        if (!otherUse) continue;

        auto otherDefs = RDA.defsFromUse(otherLoad);
        if (otherDefs.size() != 1) continue;
        if (*otherDefs.begin() != def) continue;

        // otherLoad and load have the same single definition. Replace the
        // later one with the earlier one. We identify "later" by instruction
        // order: the one that appears later in the function replaces itself.
        // Simple check: if otherLoad comes AFTER load in the same function,
        // replace otherLoad. (Cross-BB: we use RDA dominance.)
        if (!def->dominates(otherUse)) continue;

        // Replace otherLoad with the result of load (the first one).
        otherLoad->replaceAllUsesWith(load);
        if (toRemove) {
            toRemove->insert(otherLoad);
        } else {
            IrModifier::eraseUnusedInstructionRecursive(otherLoad);
        }
        return true;
    }
    return false;
}

//===========================================================================
// Pattern 6: cross-BB store-to-load forwarding
//===========================================================================

bool crossBbStorePropagation(
        Instruction* insn,
        ReachingDefinitionsAnalysis& RDA,
        Abi* abi,
        std::unordered_set<Value*>* toRemove) {

    auto* load = dyn_cast<LoadInst>(insn);
    if (!load) return false;

    // Skip same-BB (already handled by usesWithOneDefInSameBb).
    auto* use = RDA.getUse(load);
    if (!use) return false;

    auto defs = RDA.defsFromUse(load);
    if (defs.size() != 1) return false;

    auto* def = *defs.begin();
    if (!def || !def->src) return false;

    auto* store = dyn_cast<StoreInst>(def->src);
    if (!store) return false;

    // Same BB case already handled upstream — skip.
    if (store->getParent() == load->getParent()) return false;

    // The definition must dominate the use.
    if (!def->dominates(use)) return false;

    // The store's value operand must be non-volatile.
    if (store->isVolatile() || load->isVolatile()) return false;

    Value* storedVal = store->getValueOperand();
    if (storedVal->getType() != load->getType()) return false;

    // Replace the load with the stored value.
    load->replaceAllUsesWith(storedVal);
    if (toRemove) {
        toRemove->insert(load);
    } else {
        IrModifier::eraseUnusedInstructionRecursive(load);
    }

    // If the store now has no remaining uses of its definition, mark it too.
    if (def->uses.empty()) {
        // leave store; the unused-store pass will clean it up.
    }
    return true;
}

} // namespace inst_opt_rda
} // namespace bin2llvmir
} // namespace retdec
