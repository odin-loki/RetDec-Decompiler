/**
* @file src/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/negative_operand_sub_optimizer.cpp
* @brief Implementation of NegativeOperandSubOptimizer.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/const_float.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/neg_op_expr.h"
#include "retdec/llvmir2hll/ir/sub_op_expr.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/negative_operand_sub_optimizer.h"

namespace retdec {
namespace llvmir2hll {

REGISTER_AT_FACTORY("NegativeOperand", NEGATIVE_OPERAND_SUB_OPTIMIZER_ID,
	SubOptimizerFactory, NegativeOperandSubOptimizer::create);

/**
* @brief Constructs the NegativeOperandSubOptimizer.
*
* @param[in] arithmExprEvaluator @a The used evaluator of arithmetical
*            expressions.
*/
NegativeOperandSubOptimizer::NegativeOperandSubOptimizer(
		ShPtr<ArithmExprEvaluator> arithmExprEvaluator):
			SubOptimizer(arithmExprEvaluator) {}

/**
* @brief Creates a new NegativeOperandSubOptimizer.
*
* @param[in] arithmExprEvaluator @a The used evaluator of arithmetical
*            expressions.
*/
ShPtr<SubOptimizer> NegativeOperandSubOptimizer::create(
		ShPtr<ArithmExprEvaluator> arithmExprEvaluator) {
	return ShPtr<SubOptimizer>(new NegativeOperandSubOptimizer(
		arithmExprEvaluator));
}

std::string NegativeOperandSubOptimizer::getId() const {
	return NEGATIVE_OPERAND_SUB_OPTIMIZER_ID;
}

void NegativeOperandSubOptimizer::visit(ShPtr<AddOpExpr> expr) {
	OrderedAllVisitor::visit(expr);

	// -------
	// First negative operand optimization.
	//
	ShPtr<ConstInt> firstOpNegConstInt(
		ifNegativeConstIntReturnIt(expr->getFirstOperand()));
	ShPtr<ConstFloat> firstOpNegConstFloat(
		ifNegativeConstFloatReturnIt(expr->getFirstOperand()));
	ShPtr<NegOpExpr> firstOpNegOpExpr(cast<NegOpExpr>(expr->getFirstOperand()));

	// Optimization like "-3 + a" -> optimized to "a - 3".
	if (firstOpNegConstInt && !firstOpNegConstInt->isMinSigned()) {
		// We can't optimize expressions like -128 + a when -128 is only
		// on 8 bits because in this case, .abs() does not invert this
		// number to positive.
		firstOpNegConstInt->flipSign();
		ShPtr<SubOpExpr> sub(SubOpExpr::create(expr->getSecondOperand(),
			firstOpNegConstInt));
		optimizeExpr(expr, sub);
		return;
	}

	// Optimization like "-3.0 + a" -> optimized to "a - 3.0".
	if (firstOpNegConstFloat) {
		firstOpNegConstFloat->flipSign();
		ShPtr<SubOpExpr> sub(SubOpExpr::create(expr->getSecondOperand(),
			firstOpNegConstFloat));
		optimizeExpr(expr, sub);
		return;
	}

	// Optimization like "(NegOpExpr)a + 3" -> optimized to "3 - a".
	if (firstOpNegOpExpr) {
		ShPtr<SubOpExpr> sub(SubOpExpr::create(expr->getSecondOperand(),
			firstOpNegOpExpr->getOperand()));
		optimizeExpr(expr, sub);
		return;
	}
	//
	// -------

	// -------
	// Second negative operand optimization.
	//
	ShPtr<ConstInt> secOpNegConstInt(
		ifNegativeConstIntReturnIt(expr->getSecondOperand()));
	ShPtr<ConstFloat> secOpNegConstFloat(
		ifNegativeConstFloatReturnIt(expr->getSecondOperand()));
	ShPtr<NegOpExpr> secOpNegOpExpr(cast<NegOpExpr>(expr->getSecondOperand()));

	// Optimization like "a + -2" -> optimized to "a - 2".
	if (secOpNegConstInt && !secOpNegConstInt->isMinSigned()) {
		secOpNegConstInt->flipSign();
		// We can't optimize expressions like a + -128 when -128 is only
		// on 8 bits because in this case, .abs() does not invert this
		// number to positive.
		ShPtr<SubOpExpr> sub(SubOpExpr::create(expr->getFirstOperand(),
			secOpNegConstInt));
		optimizeExpr(expr, sub);
		return;
	}

	// Optimization like "a + -2.0" -> optimized to "a - 2.0".
	if (secOpNegConstFloat) {
		secOpNegConstFloat->flipSign();
		ShPtr<SubOpExpr> sub(SubOpExpr::create(expr->getFirstOperand(),
			secOpNegConstFloat));
		optimizeExpr(expr, sub);
		return;
	}

	// Optimization like "2 + (NegOpExpr)a" -> optimized to "2 - a".
	if (secOpNegOpExpr) {
		ShPtr<SubOpExpr> sub(SubOpExpr::create(expr->getFirstOperand(),
			secOpNegOpExpr->getOperand()));
		optimizeExpr(expr, sub);
		return;
	}

	// Optimization for unsigned constants that represent negative values in
	// two's complement for their bit width:  a + 0xffffffff(unsigned64)  →
	// a - 1  when the constant's high bit is set (i.e. it is >= 2^(N-1)).
	// This is safe because modular integer arithmetic is identical for signed
	// and unsigned representations.  We only do this for small k (k <= 256)
	// to avoid turning large positive adds into awkward subtracts.
	// Optimization for unsigned constants whose bit pattern represents a small
	// negative number in two's complement for their native bit width:
	//   a + 0xffffffff(uint32)  →  a - 1
	//   a + 0xfffffffe(uint32)  →  a - 2
	// We check isSignBitSet() (not isNegative()) because the value is stored
	// as unsigned and APSInt::isNegative() returns false for unsigned APSInts.
	// This covers 8/16/32-bit unsigned add operands; the 64-bit masked case is
	// already handled in ThreeOperandsSubOptimizer.
	if (!secOpNegConstInt) {
		ShPtr<ConstInt> secConst(cast<ConstInt>(expr->getSecondOperand()));
		if (secConst && secConst->isUnsigned()) {
			const llvm::APSInt &val = secConst->getValue();
			const unsigned bits     = val.getBitWidth();
			if (bits >= 8 && bits <= 32 && val.isSignBitSet()) {
				// High bit set → interpret as negative two's complement.
				// Compute k = 2^bits - val  (the positive subtract amount).
				// APSInt extends APInt; operator-() negates the bit pattern.
				llvm::APSInt negated = -val;
				uint64_t k = negated.getZExtValue();
				if (k >= 1 && k <= 256) {
					auto kConst = ConstInt::create(static_cast<std::int64_t>(k),
					                               bits, /* isSigned */ true);
					ShPtr<SubOpExpr> sub(SubOpExpr::create(
					    expr->getFirstOperand(), kConst));
					optimizeExpr(expr, sub);
					return;
				}
			}
		}
	}
	//
	// -------
}

void NegativeOperandSubOptimizer::visit(ShPtr<SubOpExpr> expr) {
	OrderedAllVisitor::visit(expr);

	// -------
	// Second negative operand optimization.
	//
	ShPtr<ConstInt> secOpNegConstInt(
		ifNegativeConstIntReturnIt(expr->getSecondOperand()));
	ShPtr<ConstFloat> secOpNegConstFloat(
		ifNegativeConstFloatReturnIt(expr->getSecondOperand()));
	ShPtr<NegOpExpr> secOpNegOpExpr(cast<NegOpExpr>(expr->getSecondOperand()));

	// Optimization like "a - -2" -> optimized to "a + 2".
	if (secOpNegConstInt && !secOpNegConstInt->isMinSigned()) {
		// We can't optimize expressions like a - -128 when -128 is only
		// on 8 bits because in this case, .abs() does not invert this
		// number to positive.
		secOpNegConstInt->flipSign();
		ShPtr<AddOpExpr> add(AddOpExpr::create(expr->getFirstOperand(),
			secOpNegConstInt));
		optimizeExpr(expr, add);
		return;
	}

	// Optimization like "a - -2.0" -> optimized to "a + 2.0".
	if (secOpNegConstFloat) {
		secOpNegConstFloat->flipSign();
		ShPtr<AddOpExpr> add(AddOpExpr::create(expr->getFirstOperand(),
			secOpNegConstFloat));
		optimizeExpr(expr, add);
		return;
	}

	// Optimization like "2 - (NegOpExpr)a" -> optimized to "2 + a".
	if (secOpNegOpExpr) {
		ShPtr<AddOpExpr> add(AddOpExpr::create(expr->getFirstOperand(),
			secOpNegOpExpr->getOperand()));
		optimizeExpr(expr, add);
		return;
	}
	//
	// -------
}

/**
* @brief If @a expr is negative @c ConstInt, return it.
*
* @param[in] expr An expression to check.
*
* @return @c ConstInt if @a expr is negative @c ConstInt, otherwise the null
*         pointer.
*/
ShPtr<ConstInt> NegativeOperandSubOptimizer::ifNegativeConstIntReturnIt(
		ShPtr<Expression> expr) const {
	ShPtr<ConstInt> constInt(cast<ConstInt>(expr));
	if (constInt && constInt->isNegative()) {
		return constInt;
	} else {
		return ShPtr<ConstInt>();
	}
}

/**
* @brief If @a expr is negative @c ConstFloat, return it.
*
* @param[in] expr An expression to check.
*
* @return @c ConstFloat if @a expr is negative @c ConstFloat, otherwise the
*         null pointer.
*/
ShPtr<ConstFloat> NegativeOperandSubOptimizer::ifNegativeConstFloatReturnIt(
		ShPtr<Expression> expr) const {
	ShPtr<ConstFloat> constFloat(cast<ConstFloat>(expr));
	if (constFloat && constFloat->isNegative()) {
		return constFloat;
	} else {
		return ShPtr<ConstFloat>();
	}
}

} // namespace llvmir2hll
} // namespace retdec
