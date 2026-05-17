/**
 * @file src/bin2llvmir/optimizations/phi_to_select/phi_to_select.cpp
 * @brief Replace trivial 2-input PHI nodes with SelectInst.
 * @copyright (c) 2024, MIT license
 *
 * Converts:
 *   %cond_br BB_true, BB_false
 *   BB_true:   (empty or single value)  → BB_merge
 *   BB_false:  (empty or single value)  → BB_merge
 *   BB_merge:  %x = phi T [%a, BB_true], [%b, BB_false]
 *
 * Into:
 *   BB_merge:  %x = select i1 cond, %a, %b
 *
 * Enables inst_opt_ext::selectSame to then collapse `select cond, v, v` → v.
 */

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include "retdec/bin2llvmir/optimizations/phi_to_select/phi_to_select.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

char PhiToSelect::ID = 0;

static RegisterPass<PhiToSelect> X(
    "retdec-phi-to-select",
    "Replace trivial 2-input PHI nodes with SelectInst",
    false, false);

PhiToSelect::PhiToSelect() : ModulePass(ID) {}

bool PhiToSelect::runOnModule(Module& M) {
    bool changed = false;
    for (auto& F : M)
        changed |= runOnFunction(F);
    return changed;
}

bool PhiToSelect::runOnFunction(Function& F) {
    bool changed = true, any = false;
    while (changed) {
        changed = false;
        SmallVector<PHINode*, 16> phis;
        for (auto& BB : F)
            for (auto& I : BB)
                if (auto* phi = dyn_cast<PHINode>(&I))
                    phis.push_back(phi);
        for (auto* phi : phis)
            if (tryConvert(phi)) { changed = any = true; }
    }
    return any;
}

// Returns true if BB contains no instructions other than an unconditional
// branch (i.e. it's a pure pass-through block for the diamond).
static bool isPassThrough(BasicBlock* bb) {
    for (auto& I : *bb)
        if (!I.isTerminator()) return false;
    auto* br = dyn_cast<BranchInst>(bb->getTerminator());
    return br && br->isUnconditional();
}

bool PhiToSelect::tryConvert(PHINode* phi) {
    if (phi->getNumIncomingValues() != 2) return false;

    BasicBlock* merge = phi->getParent();
    BasicBlock* predA = phi->getIncomingBlock(0);
    BasicBlock* predB = phi->getIncomingBlock(1);
    Value*      valA  = phi->getIncomingValue(0);
    Value*      valB  = phi->getIncomingValue(1);

    // No self-loops (loop headers).
    for (auto* p : predecessors(merge))
        if (p == merge) return false;

    // Both preds must each have exactly one successor (the merge block).
    // That means both branch unconditionally to merge.
    if (!isPassThrough(predA) || !isPassThrough(predB)) return false;

    // Find the conditional branch that leads into predA and predB.
    // Both predA and predB must have exactly one predecessor each,
    // and that predecessor must be the same block with a conditional branch.
    if (std::distance(pred_begin(predA), pred_end(predA)) != 1) return false;
    if (std::distance(pred_begin(predB), pred_end(predB)) != 1) return false;

    BasicBlock* idomA = *pred_begin(predA);
    BasicBlock* idomB = *pred_begin(predB);
    if (idomA != idomB) return false;
    BasicBlock* idom = idomA;

    auto* condBr = dyn_cast<BranchInst>(idom->getTerminator());
    if (!condBr || !condBr->isConditional()) return false;
    if (condBr->getNumSuccessors() != 2) return false;

    bool trueIsPredA = (condBr->getSuccessor(0) == predA &&
                        condBr->getSuccessor(1) == predB);
    bool trueIsPredB = (condBr->getSuccessor(0) == predB &&
                        condBr->getSuccessor(1) == predA);
    if (!trueIsPredA && !trueIsPredB) return false;

    // The incoming values must be available at the merge point.
    // Since predA/predB are pass-through, values must be defined at or before idom.
    auto isAvailableAtMerge = [&](Value* v) -> bool {
        if (isa<Constant>(v) || isa<Argument>(v)) return true;
        if (auto* I = dyn_cast<Instruction>(v))
            return I->getParent() == idom ||
                   I->getParent()->getParent() == merge->getParent();
        return false;
    };
    if (!isAvailableAtMerge(valA) || !isAvailableAtMerge(valB)) return false;

    // Build the select at the top of the merge block.
    IRBuilder<> irb(&merge->front());
    Value* cond     = condBr->getCondition();
    Value* trueVal  = trueIsPredA ? valA : valB;
    Value* falseVal = trueIsPredA ? valB : valA;

    // LLVM requires select operands to match the select result type.  After
    // load/store type fixes (e.g. entry_alloca Pass 3) or other passes, the
    // incoming values may have been truncated/extended to a different type than
    // the original phi.  Coerce them to match.
    Type* phiTy = phi->getType();
    if (trueVal->getType() != phiTy) {
        if (phiTy->isIntegerTy() && trueVal->getType()->isIntegerTy())
            trueVal = irb.CreateIntCast(trueVal, phiTy, false);
        else if (phiTy->isPointerTy() && trueVal->getType()->isPointerTy())
            trueVal = irb.CreatePointerCast(trueVal, phiTy);
        else
            return false;  // Cannot safely coerce.
    }
    if (falseVal->getType() != phiTy) {
        if (phiTy->isIntegerTy() && falseVal->getType()->isIntegerTy())
            falseVal = irb.CreateIntCast(falseVal, phiTy, false);
        else if (phiTy->isPointerTy() && falseVal->getType()->isPointerTy())
            falseVal = irb.CreatePointerCast(falseVal, phiTy);
        else
            return false;
    }

    Value* sel = irb.CreateSelect(cond, trueVal, falseVal, phi->getName() + ".sel");
    phi->replaceAllUsesWith(sel);
    phi->eraseFromParent();
    return true;
}

} // namespace bin2llvmir
} // namespace retdec
