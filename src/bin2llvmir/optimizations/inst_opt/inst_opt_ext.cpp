/**
 * @file src/bin2llvmir/optimizations/inst_opt/inst_opt_ext.cpp
 * @brief 8 additional inst_opt patterns.
 * @copyright (c) 2024, MIT license
 *
 * New patterns (all returning bool, same signature as existing patterns):
 *
 *  mulZero      x = mul y, 0       → x = 0
 *  orAllOnes    x = or y, -1       → x = -1   (all-ones)
 *  andZero      x = and y, 0       → x = 0
 *  subSelf      x = sub y, y       → x = 0    (same value subtracted)
 *  lshrZero     x = lshr y, 0      → x = y
 *  ashrZero     x = ashr y, 0      → x = y
 *  shlZero      x = shl y, 0       → x = y
 *  selectSame   x = select c, v, v → x = v    (both arms same value)
 *
 * Each is added to the optimizations vector in inst_opt.cpp by the apply script.
 */

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PatternMatch.h>

#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt_ext.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace retdec {
namespace bin2llvmir {
namespace inst_opt {

/**
 * x = mul y, 0   →   x = 0
 * x = mul 0, y   →   x = 0
 */
bool mulZero(llvm::Instruction* insn) {
    Value* val;
    uint64_t zero;
    if (!(match(insn, m_Mul(m_Value(val), m_ConstantInt(zero)))
       || match(insn, m_Mul(m_ConstantInt(zero), m_Value(val))))) return false;
    if (zero != 0) return false;
    insn->replaceAllUsesWith(ConstantInt::get(insn->getType(), 0));
    insn->eraseFromParent();
    return true;
}

/**
 * x = or y, -1   →   x = -1   (bitwise OR with all-ones = all-ones)
 * x = or -1, y   →   x = -1
 */
bool orAllOnes(llvm::Instruction* insn) {
    if (!insn->getType()->isIntegerTy()) return false;
    Value* val;
    if (!(match(insn, m_Or(m_Value(val), m_AllOnes()))
       || match(insn, m_Or(m_AllOnes(), m_Value(val))))) return false;
    insn->replaceAllUsesWith(Constant::getAllOnesValue(insn->getType()));
    insn->eraseFromParent();
    return true;
}

/**
 * x = and y, 0   →   x = 0
 * x = and 0, y   →   x = 0
 */
bool andZero(llvm::Instruction* insn) {
    Value* val;
    uint64_t zero;
    if (!(match(insn, m_And(m_Value(val), m_ConstantInt(zero)))
       || match(insn, m_And(m_ConstantInt(zero), m_Value(val))))) return false;
    if (zero != 0) return false;
    insn->replaceAllUsesWith(ConstantInt::get(insn->getType(), 0));
    insn->eraseFromParent();
    return true;
}

/**
 * x = sub y, y   →   x = 0   (same operand — already handled by subZero for
 *                              zero constant, but not for the same-value case)
 */
bool subSelf(llvm::Instruction* insn) {
    if (insn->getOpcode() != Instruction::Sub) return false;
    if (insn->getOperand(0) != insn->getOperand(1)) return false;
    insn->replaceAllUsesWith(ConstantInt::get(insn->getType(), 0));
    insn->eraseFromParent();
    return true;
}

/**
 * x = lshr y, 0   →   x = y
 * x = ashr y, 0   →   x = y
 * x = shl  y, 0   →   x = y
 */
bool shiftByZero(llvm::Instruction* insn) {
    uint64_t zero;
    Value* val;
    bool matched =
        match(insn, m_LShr(m_Value(val), m_ConstantInt(zero))) ||
        match(insn, m_AShr(m_Value(val), m_ConstantInt(zero))) ||
        match(insn, m_Shl (m_Value(val), m_ConstantInt(zero)));
    if (!matched || zero != 0) return false;
    insn->replaceAllUsesWith(val);
    insn->eraseFromParent();
    return true;
}

/**
 * x = select cond, v, v   →   x = v   (both arms identical)
 */
bool selectSame(llvm::Instruction* insn) {
    auto* sel = dyn_cast<SelectInst>(insn);
    if (!sel) return false;
    if (sel->getTrueValue() != sel->getFalseValue()) return false;
    insn->replaceAllUsesWith(sel->getTrueValue());
    insn->eraseFromParent();
    return true;
}

/**
 * x = or y, y    →   x = y     (already handled by orAndXX for load case,
 *                                but not the general register case)
 * x = and y, y   →   x = y
 */
bool orAndSelf(llvm::Instruction* insn) {
    if (insn->getOpcode() != Instruction::Or &&
        insn->getOpcode() != Instruction::And) return false;
    if (insn->getOperand(0) != insn->getOperand(1)) return false;
    insn->replaceAllUsesWith(insn->getOperand(0));
    insn->eraseFromParent();
    return true;
}

} // namespace inst_opt
} // namespace bin2llvmir
} // namespace retdec
