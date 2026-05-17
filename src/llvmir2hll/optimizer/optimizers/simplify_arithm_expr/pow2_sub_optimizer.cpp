/**
* @file src/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/pow2_sub_optimizer.cpp
* @brief Power-of-2 arithmetic simplification sub-optimizer.
* @copyright (c) 2024, MIT license
*
* Simplifications:
*  x * (2^N)  →  x << N         (integer)
*  x / (2^N)  →  x >> N         (unsigned integer only)
*  x % (2^N)  →  x & (2^N - 1)  (unsigned integer only)
*  -(-(x))    →  x
*  ~(~(x)) encoded as (x^M)^M → x
*  !(!x)      →  x               (i1 only)
*/

#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/ir/bit_and_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shl_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shr_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_xor_op_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/div_op_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/mod_op_expr.h"
#include "retdec/llvmir2hll/ir/mul_op_expr.h"
#include "retdec/llvmir2hll/ir/neg_op_expr.h"
#include "retdec/llvmir2hll/ir/not_op_expr.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/pow2_sub_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

REGISTER_AT_FACTORY("Pow2", POW2_SUB_OPTIMIZER_ID, SubOptimizerFactory,
	Pow2SubOptimizer::create);

Pow2SubOptimizer::Pow2SubOptimizer(ShPtr<ArithmExprEvaluator> e)
    : SubOptimizer(e) {}

ShPtr<SubOptimizer> Pow2SubOptimizer::create(ShPtr<ArithmExprEvaluator> e) {
    return ShPtr<SubOptimizer>(new Pow2SubOptimizer(e));
}

std::string Pow2SubOptimizer::getId() const {
    return POW2_SUB_OPTIMIZER_ID;
}

namespace {

bool isPow2(ShPtr<ConstInt> ci, unsigned& log2out) {
    if (!ci || !ci->isPositive()) return false;
    uint64_t v = ci->getValue().getZExtValue();
    if (v == 0 || (v & (v - 1)) != 0) return false;
    log2out = 0;
    while ((v >>= 1)) ++log2out;
    return true;
}

bool isUnsigned(ShPtr<Expression> expr) {
    if (auto t = cast<IntType>(expr->getType()))
        return !t->isSigned();
    return false;
}

ShPtr<ConstInt> sameWidthConst(uint64_t val, ShPtr<Expression> ref) {
    unsigned bits = 32;
    if (auto t = cast<IntType>(ref->getType())) bits = t->getSize();
    return ConstInt::create(llvm::APInt(bits, val), false);
}

} // anonymous namespace

void Pow2SubOptimizer::visit(ShPtr<MulOpExpr> expr) {
    OrderedAllVisitor::visit(expr);
    unsigned log2 = 0;
    ShPtr<Expression> base;
    if (auto ci = cast<ConstInt>(expr->getSecondOperand())) {
        if (isPow2(ci, log2)) base = expr->getFirstOperand();
    }
    if (!base) {
        if (auto ci = cast<ConstInt>(expr->getFirstOperand())) {
            if (isPow2(ci, log2)) base = expr->getSecondOperand();
        }
    }
    if (!base || log2 == 0) return;
    optimizeExpr(expr, BitShlOpExpr::create(base, sameWidthConst(log2, base)));
}

void Pow2SubOptimizer::visit(ShPtr<DivOpExpr> expr) {
    OrderedAllVisitor::visit(expr);
    auto ci = cast<ConstInt>(expr->getSecondOperand());
    if (!ci || !isUnsigned(expr->getFirstOperand())) return;
    unsigned log2 = 0;
    if (!isPow2(ci, log2) || log2 == 0) return;
    auto base = expr->getFirstOperand();
    optimizeExpr(expr, BitShrOpExpr::create(base, sameWidthConst(log2, base),
                                             BitShrOpExpr::Variant::Logical));
}

void Pow2SubOptimizer::visit(ShPtr<ModOpExpr> expr) {
    OrderedAllVisitor::visit(expr);
    auto ci = cast<ConstInt>(expr->getSecondOperand());
    if (!ci || !isUnsigned(expr->getFirstOperand())) return;
    unsigned log2 = 0;
    if (!isPow2(ci, log2) || log2 == 0) return;
    auto base = expr->getFirstOperand();
    uint64_t mask = (1ULL << log2) - 1;
    optimizeExpr(expr, BitAndOpExpr::create(base, sameWidthConst(mask, base)));
}

void Pow2SubOptimizer::visit(ShPtr<NegOpExpr> expr) {
    OrderedAllVisitor::visit(expr);
    if (auto inner = cast<NegOpExpr>(expr->getOperand())) {
        optimizeExpr(expr, inner->getOperand());
    }
}

void Pow2SubOptimizer::visit(ShPtr<BitXorOpExpr> expr) {
    OrderedAllVisitor::visit(expr);
    // (x ^ M) ^ M  →  x
    auto outerRhs = cast<ConstInt>(expr->getSecondOperand());
    if (!outerRhs) return;
    auto inner = cast<BitXorOpExpr>(expr->getFirstOperand());
    if (!inner) return;
    auto innerRhs = cast<ConstInt>(inner->getSecondOperand());
    if (!innerRhs) return;
    if (outerRhs->getValue() == innerRhs->getValue()) {
        optimizeExpr(expr, inner->getFirstOperand());
    }
}

void Pow2SubOptimizer::visit(ShPtr<NotOpExpr> expr) {
    OrderedAllVisitor::visit(expr);
    auto inner = cast<NotOpExpr>(expr->getOperand());
    if (!inner) return;
    auto innerOp = inner->getOperand();
    if (auto t = cast<IntType>(innerOp->getType())) {
        if (t->getSize() == 1) optimizeExpr(expr, innerOp);
    }
}

} // namespace llvmir2hll
} // namespace retdec
