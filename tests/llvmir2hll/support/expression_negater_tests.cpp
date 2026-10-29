/**
* @file tests/llvmir2hll/support/expression_negater_tests.cpp
* @brief Tests for the @c expression_negater module.
* @copyright (c) 2026 Odin Loch trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/gt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_op_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/support/expression_negater.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class ExpressionNegaterTests: public Test {
protected:
	ShPtr<Variable> a = Variable::create("a", IntType::create(32, false));
	ShPtr<ConstInt> zero = ConstInt::create(0, 32);
};

// !(a <s 0) is a >=s 0; an unsigned a >= 0 would always be true.
TEST_F(ExpressionNegaterTests, NegatingASignedLessThanKeepsItSigned) {
	auto neg = cast<GtEqOpExpr>(ExpressionNegater::negate(
		LtOpExpr::create(a, zero, LtOpExpr::Variant::SCmp)));
	ASSERT_TRUE(neg);
	EXPECT_EQ(GtEqOpExpr::Variant::SCmp, neg->getVariant());
}

TEST_F(ExpressionNegaterTests, NegatingASignedGreaterOrEqualKeepsItSigned) {
	auto neg = cast<LtOpExpr>(ExpressionNegater::negate(
		GtEqOpExpr::create(a, zero, GtEqOpExpr::Variant::SCmp)));
	ASSERT_TRUE(neg);
	EXPECT_EQ(LtOpExpr::Variant::SCmp, neg->getVariant());
}

TEST_F(ExpressionNegaterTests, NegatingASignedGreaterThanKeepsItSigned) {
	auto neg = cast<LtEqOpExpr>(ExpressionNegater::negate(
		GtOpExpr::create(a, zero, GtOpExpr::Variant::SCmp)));
	ASSERT_TRUE(neg);
	EXPECT_EQ(LtEqOpExpr::Variant::SCmp, neg->getVariant());
}

TEST_F(ExpressionNegaterTests, NegatingAnUnsignedLessOrEqualStaysUnsigned) {
	auto neg = cast<GtOpExpr>(ExpressionNegater::negate(
		LtEqOpExpr::create(a, zero, LtEqOpExpr::Variant::UCmp)));
	ASSERT_TRUE(neg);
	EXPECT_EQ(GtOpExpr::Variant::UCmp, neg->getVariant());
}

// Optimizers clone compares; a clone must not turn a signed compare unsigned.
TEST_F(ExpressionNegaterTests, CloningASignedCompareKeepsItSigned) {
	auto lt = LtOpExpr::create(a, zero, LtOpExpr::Variant::SCmp);
	auto clone = cast<LtOpExpr>(lt->clone());
	EXPECT_EQ(LtOpExpr::Variant::SCmp, clone->getVariant());
	EXPECT_FALSE(lt->isEqualTo(LtOpExpr::create(a, zero)));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
