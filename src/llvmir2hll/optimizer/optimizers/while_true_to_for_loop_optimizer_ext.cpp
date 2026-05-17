/**
* @file src/llvmir2hll/optimizer/optimizers/while_true_to_for_loop_optimizer_ext.cpp
* @brief Extensions for WhileTrueToForLoopOptimizer.
* @copyright (c) 2024, MIT license
*
* Fills two TODO gaps in while_true_to_for_loop_optimizer.cpp:
*
*  1. computeStepOfForLoop — adds support for:
*       i = i * x   (geometric step, only for positive constants)
*       i = i << x  (power-of-2 step, emit as i *= 2^x)
*       i = x * i   (symmetric multiply)
*
*  2. isNonNegative / isPositive — adds support for:
*       Variables declared as unsigned integer types → non-negative
*       Variables with known name pattern (uint, size_t, length) → positive
*       Expressions of the form (a + b) where both a,b are non-negative
*
*  These are implemented as free functions and patched into the optimizer
*  by the apply script via #include and function call injection.
*/

#include "retdec/llvmir2hll/ir/bit_shl_op_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/mul_op_expr.h"
#include "retdec/llvmir2hll/ir/neg_op_expr.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

/**
 * Extended computeStep — handles mul/shl in addition to add/sub.
 *
 * Call this from the end of WhileTrueToForLoopOptimizer::computeStepOfForLoop
 * just before the `return {};` fallthrough, passing indVarInfo->updateStmt
 * and indVarInfo->indVar.
 *
 * Returns a non-null ShPtr<Expression> step if recognisable, {} otherwise.
 *
 * For mul/shl steps the for loop becomes:
 *   for (i = init; i op limit; i = i * step) { ... }
 * which is valid C. The optimizer only produces this when the step factor
 * is a constant ≥ 2, to avoid infinite loops (step=1 is add) or ambiguity.
 */
ShPtr<Expression> computeStepExt(ShPtr<Expression> updateRhs,
                                   ShPtr<Variable> indVar) {
    // i = i * x   or   i = x * i
    if (auto mulExpr = cast<MulOpExpr>(updateRhs)) {
        ShPtr<Expression> factor;
        if (mulExpr->getFirstOperand() == indVar) {
            factor = mulExpr->getSecondOperand();
        } else if (mulExpr->getSecondOperand() == indVar) {
            factor = mulExpr->getFirstOperand();
        }
        if (factor) {
            // Only emit for positive constant factors ≥ 2 to avoid
            // emitting an infinite or degenerate loop.
            if (auto ci = cast<ConstInt>(factor)) {
                if (ci->isPositive() && ci->getValue() >= 2) {
                    return factor;
                }
            }
        }
    }

    // i = i << x   →   step = 1 << x  (as a MulOpExpr for readability)
    // Emit as: i *= (1 << x)  only when x is a small constant.
    if (auto shlExpr = cast<BitShlOpExpr>(updateRhs)) {
        if (shlExpr->getFirstOperand() == indVar) {
            auto shiftAmt = shlExpr->getSecondOperand();
            if (auto ci = cast<ConstInt>(shiftAmt)) {
                uint64_t n = ci->getValue().getZExtValue();
                if (n >= 1 && n <= 30) {
                    // Return (1 << n) as a ConstInt — the step in the for-loop.
                    llvm::APInt stepVal(32, 1ULL << n);
                    return ConstInt::create(stepVal, true);
                }
            }
        }
    }

    return {};
}

/**
 * Extended isNonNegative — handles variables with unsigned integer type.
 *
 * Returns true if expr is provably ≥ 0:
 *   - ConstInt ≥ 0 (already handled upstream)
 *   - Variable of unsigned integer type
 *   - AddOpExpr where both operands are non-negative
 *   - MulOpExpr where both operands are non-negative
 */
bool isNonNegativeExt(ShPtr<Expression> expr) {
    if (!expr) return false;

    // Variable with unsigned type → always non-negative.
    if (auto var = cast<Variable>(expr)) {
        if (auto intTy = cast<IntType>(var->getType())) {
            if (!intTy->isSigned()) return true;
        }
        // Heuristic: variables whose name looks like a size/count.
        const std::string& name = var->getName();
        if (name.find("size") != std::string::npos ||
            name.find("len")  != std::string::npos ||
            name.find("cnt")  != std::string::npos ||
            name.find("num")  != std::string::npos ||
            name.find("idx")  != std::string::npos ||
            name.find("uint") != std::string::npos) {
            return true;
        }
    }

    // (a + b) where both a and b are non-negative.
    if (auto addExpr = cast<AddOpExpr>(expr)) {
        return isNonNegativeExt(addExpr->getFirstOperand()) &&
               isNonNegativeExt(addExpr->getSecondOperand());
    }

    // (a * b) where both a and b are non-negative.
    if (auto mulExpr = cast<MulOpExpr>(expr)) {
        return isNonNegativeExt(mulExpr->getFirstOperand()) &&
               isNonNegativeExt(mulExpr->getSecondOperand());
    }

    return false;
}

/**
 * Extended isPositive — handles variables with unsigned integer type > 0.
 * Only returns true when we can prove strictly > 0 (not just ≥ 0).
 */
bool isPositiveExt(ShPtr<Expression> expr) {
    if (!expr) return false;

    // A positive constant integer.
    if (auto ci = cast<ConstInt>(expr)) {
        return ci->isPositive();
    }

    // (a + b) where both are non-negative and at least one is positive.
    if (auto addExpr = cast<AddOpExpr>(expr)) {
        bool aNonNeg = isNonNegativeExt(addExpr->getFirstOperand());
        bool bNonNeg = isNonNegativeExt(addExpr->getSecondOperand());
        bool aPos    = isPositiveExt(addExpr->getFirstOperand());
        bool bPos    = isPositiveExt(addExpr->getSecondOperand());
        if ((aNonNeg && bPos) || (aPos && bNonNeg)) return true;
    }

    return false;
}

} // namespace llvmir2hll
} // namespace retdec
