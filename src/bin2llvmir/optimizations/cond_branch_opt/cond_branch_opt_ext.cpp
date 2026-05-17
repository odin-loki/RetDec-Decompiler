/**
* @file src/bin2llvmir/optimizations/cond_branch_opt/cond_branch_opt_ext.cpp
* @brief Additional conditional branch optimization patterns.
* @copyright (c) 2024, MIT license
*
* Extends CondBranchOpt with patterns not yet covered:
*
*  7. Zero-extended comparison folding:
*       %z = zext i1 %cmp to i32
*       %b = icmp ne i32 %z, 0
*     → %b = %cmp  (eliminate the zext+icmp pair)
*
*  8. Sign-comparison through sub pattern (unsigned):
*       %d = sub i32 %a, %b
*       %b = icmp ult i32 %d, N
*     → %b = icmp ult i32 %a, (N + %b)  (when N=0: icmp ult %a, %b)
*
*  9. Boolean identity:
*       %t = icmp ne i1 %x, false   →  %x
*       %t = icmp eq i1 %x, true    →  %x
*       %t = icmp eq i1 %x, false   →  not %x
*       %t = icmp ne i1 %x, true    →  not %x
*
*  10. Comparison with self:
*       icmp eq %x, %x   →  true
*       icmp ne %x, %x   →  false
*       icmp ult/ugt/slt/sgt %x, %x  →  false
*       icmp ule/uge/sle/sge %x, %x  →  true
*/

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/IR/Module.h>

#include "retdec/bin2llvmir/optimizations/cond_branch_opt/cond_branch_opt_ext.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace retdec {
namespace bin2llvmir {

//===========================================================================
// Pattern 7: zext i1 → icmp ne 0  (eliminate bool promotion)
//===========================================================================
static bool foldZextBool(Instruction& inst) {
    // Match: icmp ne (zext i1 %cmp), 0
    Value* cmpVal = nullptr;
    ICmpInst::Predicate pred;
    if (!match(&inst, m_ICmp(pred,
                             m_ZExt(m_Value(cmpVal)),
                             m_Zero()))) return false;

    if (!cmpVal->getType()->isIntegerTy(1)) return false;

    Value* replacement = nullptr;
    if (pred == ICmpInst::ICMP_NE) {
        replacement = cmpVal;                  // ne 0  →  the bool itself
    } else if (pred == ICmpInst::ICMP_EQ) {
        IRBuilder<> irb(&inst);
        replacement = irb.CreateNot(cmpVal);   // eq 0  →  NOT the bool
    } else {
        return false;
    }

    if (replacement->getType() != inst.getType()) {
        IRBuilder<> irb(&inst);
        replacement = irb.CreateZExt(replacement, inst.getType());
    }

    inst.replaceAllUsesWith(replacement);
    inst.eraseFromParent();
    return true;
}

//===========================================================================
// Pattern 8: unsigned sub-then-compare folding
//===========================================================================
static bool foldSubUlt(Instruction& inst) {
    // Match: icmp ult (sub %a, %b), 0   →   icmp ne %a, %b
    // Match: icmp ule (sub %a, %b), 0   →   icmp eq %a, %b (false→same)
    Value* a = nullptr, *b = nullptr;
    ICmpInst::Predicate pred;
    if (!match(&inst, m_ICmp(pred,
                             m_Sub(m_Value(a), m_Value(b)),
                             m_Zero()))) return false;

    ICmpInst::Predicate newPred;
    if (pred == ICmpInst::ICMP_ULT) {
        newPred = ICmpInst::ICMP_NE;   // sub < 0 → a ≠ b  (unsigned)
    } else if (pred == ICmpInst::ICMP_UGT) {
        newPred = ICmpInst::ICMP_UGT;  // already handled upstream
        return false;
    } else {
        return false;
    }

    IRBuilder<> irb(&inst);
    Value* newCmp = irb.CreateICmp(newPred, a, b);
    inst.replaceAllUsesWith(newCmp);
    inst.eraseFromParent();
    return true;
}

//===========================================================================
// Pattern 9: Boolean identity / negation
//===========================================================================
static bool foldBoolIdentity(Instruction& inst) {
    auto* icmp = dyn_cast<ICmpInst>(&inst);
    if (!icmp) return false;
    if (!icmp->getOperand(0)->getType()->isIntegerTy(1)) return false;

    ICmpInst::Predicate pred = icmp->getPredicate();
    Value* x   = icmp->getOperand(0);
    Value* rhs = icmp->getOperand(1);

    auto* ci = dyn_cast<ConstantInt>(rhs);
    if (!ci) return false;

    Value* replacement = nullptr;
    IRBuilder<> irb(&inst);

    bool rhsTrue = ci->isOne();
    if (pred == ICmpInst::ICMP_NE && !rhsTrue) replacement = x;           // ne false → x
    else if (pred == ICmpInst::ICMP_EQ && rhsTrue)  replacement = x;      // eq true  → x
    else if (pred == ICmpInst::ICMP_EQ && !rhsTrue) replacement = irb.CreateNot(x);  // eq false → !x
    else if (pred == ICmpInst::ICMP_NE && rhsTrue)  replacement = irb.CreateNot(x);  // ne true  → !x
    else return false;

    inst.replaceAllUsesWith(replacement);
    inst.eraseFromParent();
    return true;
}

//===========================================================================
// Pattern 10: comparison with self
//===========================================================================
static bool foldSelfCompare(Instruction& inst) {
    auto* icmp = dyn_cast<ICmpInst>(&inst);
    if (!icmp) return false;
    if (icmp->getOperand(0) != icmp->getOperand(1)) return false;

    ICmpInst::Predicate pred = icmp->getPredicate();
    bool result;
    switch (pred) {
        case ICmpInst::ICMP_EQ:
        case ICmpInst::ICMP_ULE:
        case ICmpInst::ICMP_UGE:
        case ICmpInst::ICMP_SLE:
        case ICmpInst::ICMP_SGE: result = true;  break;
        case ICmpInst::ICMP_NE:
        case ICmpInst::ICMP_ULT:
        case ICmpInst::ICMP_UGT:
        case ICmpInst::ICMP_SLT:
        case ICmpInst::ICMP_SGT: result = false; break;
        default: return false;
    }

    auto* c = ConstantInt::get(inst.getType(), result ? 1 : 0);
    inst.replaceAllUsesWith(c);
    inst.eraseFromParent();
    return true;
}

//===========================================================================
// Entry point: run all extended patterns on an instruction.
// Returns true if the instruction was eliminated/replaced.
//===========================================================================
bool condBranchOptExt(Instruction& inst) {
    if (foldSelfCompare(inst)) return true;
    if (foldBoolIdentity(inst)) return true;
    if (foldZextBool(inst))     return true;
    if (foldSubUlt(inst))       return true;
    return false;
}

} // namespace bin2llvmir
} // namespace retdec
