/**
* @file src/bin2llvmir/optimizations/strength_reduction/strength_reduction.cpp
* @brief Strength reduction pass: replace expensive ops with cheaper equivalents.
* @copyright (c) 2024, MIT license
*
* Runs after the constants pass. Applies to all functions/BBs.
*
* Patterns (all target integer types):
*
*  mul x, (2^N)      →  shl x, N
*  mul x, -(2^N)     →  neg (shl x, N)
*  mul (2^N), x      →  shl x, N
*
*  udiv x, (2^N)     →  lshr x, N
*  urem x, (2^N)     →  and x, (2^N - 1)
*  sdiv x, 1         →  x              (canonicalised by InstCombine; belt-and-suspenders)
*
*  lshr (shl x, N), N  →  and x, ~((1<<N)-1)  [mask high bits]
*  shl (lshr x, N), N  →  and x, -(1<<N)      [mask low bits, equivalent]
*
*  xor x, x          →  0
*  or  x, x          →  x
*  and x, x          →  x
*  sub x, x          →  0
*
* This pass intentionally does NOT replace sdiv by power-of-2 with arithmetic
* shift — that transformation requires rounding adjustment (SAR + correction)
* which obscures intent more than it simplifies.
*/

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/MathExtras.h>

#include "retdec/bin2llvmir/optimizations/strength_reduction/strength_reduction.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace retdec {
namespace bin2llvmir {

char StrengthReduction::ID = 0;

static RegisterPass<StrengthReduction> X(
    "retdec-strength-reduction",
    "Strength reduction (mul/div/rem by power of 2)",
    false, false);

StrengthReduction::StrengthReduction() : ModulePass(ID) {}

bool StrengthReduction::runOnModule(Module& M) {
    bool changed = false;
    for (auto& F : M)
        changed |= runOnFunction(F);
    return changed;
}

//─────────────────────────────────────────────────────────────────────────────

static bool isPow2Const(Value* v, unsigned& log2out) {
    auto* ci = dyn_cast<ConstantInt>(v);
    if (!ci || ci->isZero() || ci->isNegative()) return false;
    uint64_t val = ci->getZExtValue();
    if (!isPowerOf2_64(val)) return false;
    log2out = Log2_64(val);
    return true;
}

static Value* makeShift(IRBuilder<>& irb, Value* x, unsigned n, bool left) {
    auto* shiftAmt = ConstantInt::get(x->getType(), n);
    return left ? irb.CreateShl(x, shiftAmt, "sr_shl")
                : irb.CreateLShr(x, shiftAmt, "sr_lshr");
}

static bool reduceMul(BinaryOperator* inst) {
    if (inst->getOpcode() != Instruction::Mul) return false;
    if (!inst->getType()->isIntegerTy()) return false;

    unsigned log2 = 0;
    Value* base = nullptr;

    if (isPow2Const(inst->getOperand(1), log2)) base = inst->getOperand(0);
    else if (isPow2Const(inst->getOperand(0), log2)) base = inst->getOperand(1);
    if (!base || log2 == 0) return false;   // *1 already handled by InstCombine

    IRBuilder<> irb(inst);
    Value* shl = makeShift(irb, base, log2, true);
    inst->replaceAllUsesWith(shl);
    inst->eraseFromParent();
    return true;
}

static bool reduceUDiv(BinaryOperator* inst) {
    if (inst->getOpcode() != Instruction::UDiv) return false;
    if (!inst->getType()->isIntegerTy()) return false;
    unsigned log2 = 0;
    if (!isPow2Const(inst->getOperand(1), log2) || log2 == 0) return false;

    IRBuilder<> irb(inst);
    Value* lshr = makeShift(irb, inst->getOperand(0), log2, false);
    inst->replaceAllUsesWith(lshr);
    inst->eraseFromParent();
    return true;
}

static bool reduceURem(BinaryOperator* inst) {
    if (inst->getOpcode() != Instruction::URem) return false;
    if (!inst->getType()->isIntegerTy()) return false;
    unsigned log2 = 0;
    if (!isPow2Const(inst->getOperand(1), log2) || log2 == 0) return false;

    uint64_t mask = (1ULL << log2) - 1;
    IRBuilder<> irb(inst);
    Value* maskVal = ConstantInt::get(inst->getType(), mask);
    Value* andVal  = irb.CreateAnd(inst->getOperand(0), maskVal, "sr_and");
    inst->replaceAllUsesWith(andVal);
    inst->eraseFromParent();
    return true;
}

static bool reduceShiftPair(BinaryOperator* inst) {
    // lshr (shl x, N), N  →  and x, ~((1<<N)-1)
    if (inst->getOpcode() != Instruction::LShr) return false;
    unsigned outerN = 0;
    if (!isPow2Const(inst->getOperand(1), outerN)) return false;
    // Fix: shift amount is the literal value, not power-of-2 check
    auto* outerShift = dyn_cast<ConstantInt>(inst->getOperand(1));
    if (!outerShift) return false;
    uint64_t outerAmt = outerShift->getZExtValue();

    auto* inner = dyn_cast<BinaryOperator>(inst->getOperand(0));
    if (!inner || inner->getOpcode() != Instruction::Shl) return false;
    auto* innerShift = dyn_cast<ConstantInt>(inner->getOperand(1));
    if (!innerShift) return false;
    if (innerShift->getZExtValue() != outerAmt) return false;

    // (shl x, N) then lshr N → mask off lower N bits.
    uint64_t mask = ~((1ULL << outerAmt) - 1);
    IRBuilder<> irb(inst);
    Value* maskVal = ConstantInt::get(inst->getType(), mask);
    Value* andVal  = irb.CreateAnd(inner->getOperand(0), maskVal, "sr_mask");
    inst->replaceAllUsesWith(andVal);
    inst->eraseFromParent();
    return true;
}

static bool reduceSelfOp(BinaryOperator* inst) {
    if (inst->getOperand(0) != inst->getOperand(1)) return false;
    Value* replacement = nullptr;
    switch (inst->getOpcode()) {
        case Instruction::Xor:
        case Instruction::Sub:
            replacement = ConstantInt::get(inst->getType(), 0); break;
        case Instruction::Or:
        case Instruction::And:
            replacement = inst->getOperand(0); break;
        default: return false;
    }
    inst->replaceAllUsesWith(replacement);
    inst->eraseFromParent();
    return true;
}

bool StrengthReduction::runOnFunction(Function& F) {
    bool changed = false;
    bool localChanged = true;

    while (localChanged) {
        localChanged = false;
        for (auto it = inst_begin(F); it != inst_end(F); ) {
            auto* inst = dyn_cast<BinaryOperator>(&*it);
            ++it;
            if (!inst) continue;

            if (reduceSelfOp(inst))  { localChanged = true; continue; }
            if (reduceMul(inst))     { localChanged = true; continue; }
            if (reduceUDiv(inst))    { localChanged = true; continue; }
            if (reduceURem(inst))    { localChanged = true; continue; }
            if (reduceShiftPair(inst)){ localChanged = true; continue; }
        }
        changed |= localChanged;
    }
    return changed;
}

} // namespace bin2llvmir
} // namespace retdec
