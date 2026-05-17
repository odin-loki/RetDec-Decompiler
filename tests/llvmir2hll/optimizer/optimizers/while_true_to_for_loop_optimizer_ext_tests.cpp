/**
* @file tests/llvmir2hll/optimizer/optimizers/while_true_to_for_loop_optimizer_ext_tests.cpp
* @brief Tests for while_true_to_for_loop_optimizer_ext.cpp extension functions.
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shl_op_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/mul_op_expr.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/while_true_to_for_loop_optimizer_ext.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class WhileTrueToForLoopOptimizerExtTests: public Test {};

//
// computeStepExt tests
//

TEST_F(WhileTrueToForLoopOptimizerExtTests,
ComputeStepExtReturnsMulFactorForITimesConst) {
	// i = i * 3  → step = 3
	auto varI = Variable::create("i", IntType::create(32, true));
	auto factor = ConstInt::create(llvm::APInt(32, 3, true), true);
	auto mulExpr = MulOpExpr::create(varI, factor);

	auto step = computeStepExt(mulExpr, varI);
	ASSERT_NE(nullptr, step) << "expected non-null step for i*3";
	auto ci = cast<ConstInt>(step);
	ASSERT_NE(nullptr, ci);
	EXPECT_EQ(3u, ci->getValue().getZExtValue());
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
ComputeStepExtReturnsMulFactorForConstTimesI) {
	// i = 4 * i  → step = 4 (symmetric case)
	auto varI = Variable::create("i", IntType::create(32, true));
	auto factor = ConstInt::create(llvm::APInt(32, 4, true), true);
	auto mulExpr = MulOpExpr::create(factor, varI);

	auto step = computeStepExt(mulExpr, varI);
	ASSERT_NE(nullptr, step) << "expected non-null step for 4*i";
	auto ci = cast<ConstInt>(step);
	ASSERT_NE(nullptr, ci);
	EXPECT_EQ(4u, ci->getValue().getZExtValue());
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
ComputeStepExtRejectsFactorLessThan2) {
	// i = i * 1  → step factor < 2, return {}
	auto varI = Variable::create("i", IntType::create(32, true));
	auto factor = ConstInt::create(llvm::APInt(32, 1, true), true);
	auto mulExpr = MulOpExpr::create(varI, factor);

	auto step = computeStepExt(mulExpr, varI);
	EXPECT_EQ(nullptr, step) << "should not compute step for factor=1";
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
ComputeStepExtReturnsPowerOf2ForShiftLeft) {
	// i = i << 2  → step = (1 << 2) = 4
	auto varI = Variable::create("i", IntType::create(32, true));
	auto shiftAmt = ConstInt::create(llvm::APInt(32, 2, true), true);
	auto shlExpr = BitShlOpExpr::create(varI, shiftAmt);

	auto step = computeStepExt(shlExpr, varI);
	ASSERT_NE(nullptr, step) << "expected non-null step for i<<2";
	auto ci = cast<ConstInt>(step);
	ASSERT_NE(nullptr, ci);
	EXPECT_EQ(4u, ci->getValue().getZExtValue());
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
ComputeStepExtRejectsShiftOf0) {
	// i = i << 0  → n=0, rejected (n >= 1 check)
	auto varI = Variable::create("i", IntType::create(32, true));
	auto shiftAmt = ConstInt::create(llvm::APInt(32, 0, true), true);
	auto shlExpr = BitShlOpExpr::create(varI, shiftAmt);

	auto step = computeStepExt(shlExpr, varI);
	EXPECT_EQ(nullptr, step) << "should not compute step for i<<0";
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
ComputeStepExtRejectsNonMatchingMul) {
	// i = j * k (neither operand is indVar) → return {}
	auto varI = Variable::create("i", IntType::create(32, true));
	auto varJ = Variable::create("j", IntType::create(32, true));
	auto varK = Variable::create("k", IntType::create(32, true));
	auto mulExpr = MulOpExpr::create(varJ, varK);

	auto step = computeStepExt(mulExpr, varI);
	EXPECT_EQ(nullptr, step);
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
ComputeStepExtReturnsNullForAddExpr) {
	// Simple add — not handled by ext, returns {}
	auto varI = Variable::create("i", IntType::create(32, true));
	auto one = ConstInt::create(llvm::APInt(32, 1, true), true);
	auto addExpr = AddOpExpr::create(varI, one);

	auto step = computeStepExt(addExpr, varI);
	EXPECT_EQ(nullptr, step);
}

//
// isNonNegativeExt tests
//

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsTrueForUnsignedVar) {
	// Variable of unsigned int type → non-negative.
	auto var = Variable::create("u", IntType::create(32, false));
	EXPECT_TRUE(isNonNegativeExt(var));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsFalseForSignedVar) {
	auto var = Variable::create("s", IntType::create(32, true));
	EXPECT_FALSE(isNonNegativeExt(var));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsTrueForSizeNamedVar) {
	auto var = Variable::create("size", IntType::create(32, true));
	EXPECT_TRUE(isNonNegativeExt(var));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsTrueForLenNamedVar) {
	auto var = Variable::create("len", IntType::create(32, true));
	EXPECT_TRUE(isNonNegativeExt(var));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsTrueForIdxNamedVar) {
	auto var = Variable::create("idx", IntType::create(32, true));
	EXPECT_TRUE(isNonNegativeExt(var));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsTrueForAddOfTwoUnsigned) {
	auto a = Variable::create("a", IntType::create(32, false));
	auto b = Variable::create("b", IntType::create(32, false));
	auto addExpr = AddOpExpr::create(a, b);
	EXPECT_TRUE(isNonNegativeExt(addExpr));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsFalseForAddWithSigned) {
	auto a = Variable::create("a", IntType::create(32, true));
	auto b = Variable::create("b", IntType::create(32, false));
	auto addExpr = AddOpExpr::create(a, b);
	EXPECT_FALSE(isNonNegativeExt(addExpr));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsTrueForMulOfTwoUnsigned) {
	auto a = Variable::create("a", IntType::create(32, false));
	auto b = Variable::create("b", IntType::create(32, false));
	auto mulExpr = MulOpExpr::create(a, b);
	EXPECT_TRUE(isNonNegativeExt(mulExpr));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsNonNegativeExtReturnsFalseForNull) {
	EXPECT_FALSE(isNonNegativeExt(nullptr));
}

//
// isPositiveExt tests
//

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsPositiveExtReturnsTrueForPositiveConstInt) {
	auto ci = ConstInt::create(llvm::APInt(32, 5, true), true);
	EXPECT_TRUE(isPositiveExt(ci));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsPositiveExtReturnsFalseForZeroConstInt) {
	auto ci = ConstInt::create(llvm::APInt(32, 0, true), true);
	EXPECT_FALSE(isPositiveExt(ci));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsPositiveExtReturnsTrueForAddOfNonNegAndPositive) {
	// (a_unsigned + 5) → a_unsigned is non-negative, 5 is positive → true
	auto a = Variable::create("a", IntType::create(32, false));
	auto five = ConstInt::create(llvm::APInt(32, 5, true), true);
	auto addExpr = AddOpExpr::create(a, five);
	EXPECT_TRUE(isPositiveExt(addExpr));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsPositiveExtReturnsFalseForNull) {
	EXPECT_FALSE(isPositiveExt(nullptr));
}

TEST_F(WhileTrueToForLoopOptimizerExtTests,
IsPositiveExtReturnsFalseForSignedVar) {
	auto var = Variable::create("s", IntType::create(32, true));
	EXPECT_FALSE(isPositiveExt(var));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
