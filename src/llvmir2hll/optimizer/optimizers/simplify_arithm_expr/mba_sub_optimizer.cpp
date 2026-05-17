/**
* @file src/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/mba_sub_optimizer.cpp
* @brief Mixed Boolean-Arithmetic (MBA) deobfuscation sub-optimizer.
* @copyright (c) 2024, MIT license
*
* Stage 24: Simplifies common MBA-obfuscated expressions:
*  (a | b) - (a & b)  →  a ^ b
*  (a | b) - (a ^ b)  →  a & b
*  (a | b) + (a & b)  →  a + b
*  (a ^ b) + 2*(a & b)  →  a + b
*  (a & b) + (a ^ b)  →  a | b
*  (a ^ b) ^ (a & b)  →  a | b
*/

#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_and_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_or_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_xor_op_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/mul_op_expr.h"
#include "retdec/llvmir2hll/ir/sub_op_expr.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/mba_sub_optimizer.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/sub_optimizer_factory.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

REGISTER_AT_FACTORY("Mba", MBA_SUB_OPTIMIZER_ID, SubOptimizerFactory,
	MbaSubOptimizer::create);

MbaSubOptimizer::MbaSubOptimizer(ShPtr<ArithmExprEvaluator> e)
	: SubOptimizer(e) {}

ShPtr<SubOptimizer> MbaSubOptimizer::create(ShPtr<ArithmExprEvaluator> e) {
	return ShPtr<SubOptimizer>(new MbaSubOptimizer(e));
}

std::string MbaSubOptimizer::getId() const {
	return MBA_SUB_OPTIMIZER_ID;
}

namespace {

bool sameVars(ShPtr<Expression> a, ShPtr<Expression> b, ShPtr<Expression> c,
		ShPtr<Expression> d) {
	return (a->isEqualTo(c) && b->isEqualTo(d))
		|| (a->isEqualTo(d) && b->isEqualTo(c));
}

bool isConstTwo(ShPtr<Expression> expr) {
	auto ci = cast<ConstInt>(expr);
	return ci && ci->getValue().getLimitedValue(64) == 2;
}

} // anonymous namespace

void MbaSubOptimizer::visit(ShPtr<SubOpExpr> expr) {
	OrderedAllVisitor::visit(expr);

	// (a | b) - (a & b)  →  a ^ b
	auto orExpr = cast<BitOrOpExpr>(expr->getFirstOperand());
	auto andExpr = cast<BitAndOpExpr>(expr->getSecondOperand());
	if (orExpr && andExpr) {
		auto a = orExpr->getFirstOperand();
		auto b = orExpr->getSecondOperand();
		auto c = andExpr->getFirstOperand();
		auto d = andExpr->getSecondOperand();
		if (sameVars(a, b, c, d)) {
			optimizeExpr(expr, BitXorOpExpr::create(a, b));
			return;
		}
	}

	// (a | b) - (a ^ b)  →  a & b
	orExpr = cast<BitOrOpExpr>(expr->getFirstOperand());
	auto xorExpr = cast<BitXorOpExpr>(expr->getSecondOperand());
	if (orExpr && xorExpr) {
		auto a = orExpr->getFirstOperand();
		auto b = orExpr->getSecondOperand();
		auto c = xorExpr->getFirstOperand();
		auto d = xorExpr->getSecondOperand();
		if (sameVars(a, b, c, d)) {
			optimizeExpr(expr, BitAndOpExpr::create(a, b));
		}
	}
}

void MbaSubOptimizer::visit(ShPtr<AddOpExpr> expr) {
	OrderedAllVisitor::visit(expr);

	// (a & b) + (a ^ b)  →  a | b
	ShPtr<BitAndOpExpr> andExpr = cast<BitAndOpExpr>(expr->getFirstOperand());
	ShPtr<BitXorOpExpr> xorExpr = cast<BitXorOpExpr>(expr->getSecondOperand());
	if (!andExpr || !xorExpr) {
		andExpr = cast<BitAndOpExpr>(expr->getSecondOperand());
		xorExpr = cast<BitXorOpExpr>(expr->getFirstOperand());
	}
	if (andExpr && xorExpr) {
		auto a = andExpr->getFirstOperand();
		auto b = andExpr->getSecondOperand();
		auto c = xorExpr->getFirstOperand();
		auto d = xorExpr->getSecondOperand();
		if (sameVars(a, b, c, d)) {
			optimizeExpr(expr, BitOrOpExpr::create(a, b));
			return;
		}
	}

	// (a | b) + (a & b)  →  a + b
	auto orExpr2 = cast<BitOrOpExpr>(expr->getFirstOperand());
	auto andExpr2nd = cast<BitAndOpExpr>(expr->getSecondOperand());
	if (orExpr2 && andExpr2nd) {
		auto a = orExpr2->getFirstOperand();
		auto b = orExpr2->getSecondOperand();
		auto c = andExpr2nd->getFirstOperand();
		auto d = andExpr2nd->getSecondOperand();
		if (sameVars(a, b, c, d)) {
			optimizeExpr(expr, AddOpExpr::create(a, b));
			return;
		}
	}
	if (!orExpr2 || !andExpr2nd) {
		orExpr2 = cast<BitOrOpExpr>(expr->getSecondOperand());
		andExpr2nd = cast<BitAndOpExpr>(expr->getFirstOperand());
		if (orExpr2 && andExpr2nd) {
			auto a = orExpr2->getFirstOperand();
			auto b = orExpr2->getSecondOperand();
			auto c = andExpr2nd->getFirstOperand();
			auto d = andExpr2nd->getSecondOperand();
			if (sameVars(a, b, c, d)) {
				optimizeExpr(expr, AddOpExpr::create(a, b));
				return;
			}
		}
	}

	// (a ^ b) + 2*(a & b)  →  a + b
	ShPtr<BitXorOpExpr> xorExpr2;
	ShPtr<MulOpExpr> mulExpr;

	if (auto x = cast<BitXorOpExpr>(expr->getFirstOperand())) {
		xorExpr2 = x;
		mulExpr = cast<MulOpExpr>(expr->getSecondOperand());
	}
	if (!xorExpr2 && (xorExpr2 = cast<BitXorOpExpr>(expr->getSecondOperand()))) {
		mulExpr = cast<MulOpExpr>(expr->getFirstOperand());
	}
	if (!xorExpr2 || !mulExpr) return;

	// mul must be 2 * (a & b) or (a & b) * 2
	ShPtr<BitAndOpExpr> andExpr2;
	if (isConstTwo(mulExpr->getFirstOperand())) {
		andExpr2 = cast<BitAndOpExpr>(mulExpr->getSecondOperand());
	} else if (isConstTwo(mulExpr->getSecondOperand())) {
		andExpr2 = cast<BitAndOpExpr>(mulExpr->getFirstOperand());
	}
	if (!andExpr2) return;

	auto a = xorExpr2->getFirstOperand();
	auto b = xorExpr2->getSecondOperand();
	auto c = andExpr2->getFirstOperand();
	auto d = andExpr2->getSecondOperand();

	if (!sameVars(a, b, c, d)) return;

	optimizeExpr(expr, AddOpExpr::create(a, b));
}

void MbaSubOptimizer::visit(ShPtr<BitXorOpExpr> expr) {
	OrderedAllVisitor::visit(expr);

	// (a ^ b) ^ (a & b)  →  a | b
	ShPtr<BitXorOpExpr> xorExpr = cast<BitXorOpExpr>(expr->getFirstOperand());
	ShPtr<BitAndOpExpr> andExpr = cast<BitAndOpExpr>(expr->getSecondOperand());
	if (!xorExpr || !andExpr) {
		xorExpr = cast<BitXorOpExpr>(expr->getSecondOperand());
		andExpr = cast<BitAndOpExpr>(expr->getFirstOperand());
	}
	if (xorExpr && andExpr) {
		auto a = xorExpr->getFirstOperand();
		auto b = xorExpr->getSecondOperand();
		auto c = andExpr->getFirstOperand();
		auto d = andExpr->getSecondOperand();
		if (sameVars(a, b, c, d)) {
			optimizeExpr(expr, BitOrOpExpr::create(a, b));
		}
	}
}

} // namespace llvmir2hll
} // namespace retdec
