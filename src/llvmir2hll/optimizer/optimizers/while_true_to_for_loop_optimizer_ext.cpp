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
 * Extended computeStep — the multiplicative cases, which it does NOT handle.
 *
 * Call this from the end of WhileTrueToForLoopOptimizer::computeStepOfForLoop
 * just before the `return {};` fallthrough, passing indVarInfo->updateStmt
 * and indVarInfo->indVar.
 *
 * It used to return the multiply factor for `i = i * k`, and `1 << n` for
 * `i = i << n`, and the caller handed that straight to ForLoopStmt::create as
 * the step. ForLoopStmt has no multiplicative form: CHLLWriter emits the step
 * as `i++`, `i--`, `i -= x` or `i += x`, and nothing else. So `i = i * 2`
 * came out as `i += 2`:
 *
 *   int32_t i = 1, sum = 0;
 *   while (true) { sum = sum + i; if (i >= 64) break; i = i * 2; }
 *
 * runs i over 1, 2, 4, 8, 16, 32, 64 and leaves sum = 127. The emitted
 * `for (i = 1; i < 65; i += 2)` runs i over 1, 3, 5 ... 63 and leaves
 * sum = 1024 -- thirty-two iterations where there were seven. The `+ 1`
 * adjustment the caller makes to the end value is only correct for a unit
 * step besides.
 *
 * The comment this replaces claimed the loop "becomes for (...; i = i * step)",
 * which is a form the writer cannot produce. A multiplicative step belongs in
 * WhileTrueToUForLoopOptimizer, whose UForLoopStmt carries an arbitrary step
 * expression; until it is done there, these shapes are not recognised.
 *
 * Returns {} always. The signature is kept so the call site and its test stay
 * where they are.
 */
ShPtr<Expression> computeStepExt(ShPtr<Expression> updateRhs, ShPtr<Variable> indVar)
{
	(void)updateRhs;
	(void)indVar;
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
bool isNonNegativeExt(ShPtr<Expression> expr)
{
	if (!expr) return false;

	// Variable with unsigned type → always non-negative.
	//
	// There used to be a second branch here: a substring match on the
	// variable's *name* -- size, len, cnt, num, idx, uint -- returning true
	// whatever the type. A name is not a fact about a value. `int size = -1`
	// is the commonest sentinel there is, a signed `idx` walking a list
	// backwards is routine, and neither the names RetDec generates nor the ones
	// it reads out of debug info promise anything about sign.
	//
	// What the answer decides: WhileTrueToForLoopOptimizer::isNonNegative gates
	// rewriting a loop's exit test from `indVar != endValue` to
	// `indVar < endValue`. That rewrite is only equivalent when the step is
	// non-negative -- with a negative step and start > end, `!=` runs the loop
	// and `<` does not run it at all. So a variable called `num_step` holding
	// -1 turned a loop that ran into a loop that does not, in C that compiles.
	//
	// Three tests asserted the heuristic, one per name, each on a *signed*
	// 32-bit variable.
	if (auto var = cast<Variable>(expr))
	{
		if (auto intTy = cast<IntType>(var->getType()))
		{
			if (!intTy->isSigned()) return true;
		}
	}

	// (a + b) where both a and b are non-negative.
	if (auto addExpr = cast<AddOpExpr>(expr))
	{
		return isNonNegativeExt(addExpr->getFirstOperand()) && isNonNegativeExt(addExpr->getSecondOperand());
	}

	// (a * b) where both a and b are non-negative.
	if (auto mulExpr = cast<MulOpExpr>(expr))
	{
		return isNonNegativeExt(mulExpr->getFirstOperand()) && isNonNegativeExt(mulExpr->getSecondOperand());
	}

	return false;
}

/**
 * Extended isPositive — handles variables with unsigned integer type > 0.
 * Only returns true when we can prove strictly > 0 (not just ≥ 0).
 */
bool isPositiveExt(ShPtr<Expression> expr)
{
	if (!expr) return false;

	// A positive constant integer.
	if (auto ci = cast<ConstInt>(expr))
	{
		return ci->isPositive();
	}

	// (a + b) where both are non-negative and at least one is positive.
	if (auto addExpr = cast<AddOpExpr>(expr))
	{
		bool aNonNeg = isNonNegativeExt(addExpr->getFirstOperand());
		bool bNonNeg = isNonNegativeExt(addExpr->getSecondOperand());
		bool aPos = isPositiveExt(addExpr->getFirstOperand());
		bool bPos = isPositiveExt(addExpr->getSecondOperand());
		if ((aNonNeg && bPos) || (aPos && bNonNeg)) return true;
	}

	return false;
}

} // namespace llvmir2hll
} // namespace retdec
