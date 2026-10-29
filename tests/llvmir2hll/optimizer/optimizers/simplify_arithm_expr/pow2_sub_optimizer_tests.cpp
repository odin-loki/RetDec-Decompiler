/**
* @file tests/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/pow2_sub_optimizer_tests.cpp
* @brief Tests for the @c pow2_sub_optimizer module.
* @copyright (c) 2026 Odin Loch trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluators/strict_arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/ir/bit_and_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shr_op_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/div_op_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/mod_op_expr.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/pow2_sub_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class Pow2SubOptimizerTests: public TestsWithModule {
protected:
	virtual void SetUp() override {
		TestsWithModule::SetUp();
		optimizer = ShPtr<Pow2SubOptimizer>(new Pow2SubOptimizer(
			StrictArithmExprEvaluator::create()));
	}

	ShPtr<Pow2SubOptimizer> optimizer;
};

TEST_F(Pow2SubOptimizerTests,
UnsignedDivisionByFourBecomesALogicalShift) {
	auto a = Variable::create("a", IntType::create(32, false));
	auto ret = ReturnStmt::create(DivOpExpr::create(a, ConstInt::create(4, 32),
		DivOpExpr::Variant::UDiv));
	optimizer->tryOptimize(ret->getRetVal());
	auto shr = cast<BitShrOpExpr>(ret->getRetVal());
	ASSERT_TRUE(shr) << ret;
	EXPECT_TRUE(shr->isLogical());
}

// -7 / 4 is -1 but -7 >> 2 is -2, whatever C type the operand was given.
TEST_F(Pow2SubOptimizerTests,
SignedDivisionOfAnUnsignedTypedValueIsKept) {
	auto a = Variable::create("a", IntType::create(32, false));
	auto ret = ReturnStmt::create(DivOpExpr::create(a, ConstInt::create(4, 32),
		DivOpExpr::Variant::SDiv));
	optimizer->tryOptimize(ret->getRetVal());
	EXPECT_TRUE(isa<DivOpExpr>(ret->getRetVal())) << ret;
}

// -7 % 4 is -3 but -7 & 3 is 1.
TEST_F(Pow2SubOptimizerTests,
SignedRemainderIsNotAMask) {
	auto a = Variable::create("a", IntType::create(32, false));
	auto ret = ReturnStmt::create(ModOpExpr::create(a, ConstInt::create(4, 32),
		ModOpExpr::Variant::SMod));
	optimizer->tryOptimize(ret->getRetVal());
	EXPECT_TRUE(isa<ModOpExpr>(ret->getRetVal())) << ret;
}

TEST_F(Pow2SubOptimizerTests,
UnsignedRemainderBecomesAMask) {
	auto a = Variable::create("a", IntType::create(32, false));
	auto ret = ReturnStmt::create(ModOpExpr::create(a, ConstInt::create(4, 32),
		ModOpExpr::Variant::UMod));
	optimizer->tryOptimize(ret->getRetVal());
	EXPECT_TRUE(isa<BitAndOpExpr>(ret->getRetVal())) << ret;
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
