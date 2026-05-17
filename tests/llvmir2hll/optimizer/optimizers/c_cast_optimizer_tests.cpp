/**
* @file tests/llvmir2hll/optimizer/optimizers/c_cast_optimizer_tests.cpp
* @brief Tests for the @c c_cast_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*
* Verifies that redundant C casts (those that are implicit in C) are removed
* from assignments, variable definitions, and return statements, while
* non-implicit casts (e.g., sign-changing casts) are preserved.
*/

#include <gtest/gtest.h>

#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/ext_cast_expr.h"
#include "retdec/llvmir2hll/ir/float_type.h"
#include "retdec/llvmir2hll/ir/fp_to_int_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_to_fp_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/trunc_cast_expr.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/c_cast_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c c_cast_optimizer module.
*/
class CCastOptimizerTests: public TestsWithModule {};

TEST_F(CCastOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<CCastOptimizer> optimizer(new CCastOptimizer(module));
	EXPECT_FALSE(optimizer->getId().empty())
		<< "the optimizer should have a non-empty ID";
}

TEST_F(CCastOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	Optimizer::optimize<CCastOptimizer>(module);

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody()))
		<< "expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_FALSE(testFunc->getBody()->hasSuccessor());
}

// int32_t b = (int32_t)a  where a is int16_t → cast removed (int-to-int, same sign).
TEST_F(CCastOptimizerTests,
SignedIntToSignedIntExtCastInAssignIsRemoved) {
	// int16_t a; int32_t b; b = (int32_t)a;
	auto varA = Variable::create("a", IntType::create(16, /*isSigned=*/true));
	auto varB = Variable::create("b", IntType::create(32, /*isSigned=*/true));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto castExpr = ExtCastExpr::create(varA, IntType::create(32, true),
		ExtCastExpr::Variant::SExt);
	auto assign = AssignStmt::create(varB, castExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	// After optimization the cast should be stripped: b = a.
	EXPECT_EQ(varA, assign->getRhs())
		<< "implicit signed int-to-int cast should be removed";
}

// int32_t b = (int32_t)a  where a is uint16_t and b is int32_t (ZExt to same sign) → removed.
TEST_F(CCastOptimizerTests,
ZeroExtCastToSameSignedIntIsRemoved) {
	// uint16_t a; int32_t b; b = (int32_t)a; (ZExt: both int, both signed? no — dst signed, cast signed)
	// The cast has dst type int32_t (signed); src = ZExt(a, int32_t) has type int32_t (signed).
	// Operand a has uint16_t — cast<IntType> succeeds → cast removed.
	auto varA = Variable::create("a", IntType::create(16, /*isSigned=*/false));
	auto varB = Variable::create("b", IntType::create(32, /*isSigned=*/true));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto castExpr = ExtCastExpr::create(varA, IntType::create(32, true),
		ExtCastExpr::Variant::ZExt);
	auto assign = AssignStmt::create(varB, castExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varA, assign->getRhs())
		<< "int-to-int cast with int32_t result matching dst should be removed";
}

// int32_t b = (uint32_t)a  where signedness differs → cast preserved.
TEST_F(CCastOptimizerTests,
SignednessMismatchCastIsPreserved) {
	// int32_t b = (uint32_t)a;  dst is signed, cast result is unsigned → not removed.
	auto varA = Variable::create("a", IntType::create(32, true));
	auto varB = Variable::create("b", IntType::create(32, true));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	// BitCast (identity) but with differing signedness on result vs destination.
	auto castExpr = ExtCastExpr::create(varA, IntType::create(32, /*isSigned=*/false),
		ExtCastExpr::Variant::ZExt);
	auto assign = AssignStmt::create(varB, castExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	// Cast should remain because dst (signed) != cast result type (unsigned).
	EXPECT_TRUE(isa<ExtCastExpr>(assign->getRhs()))
		<< "signed/unsigned mismatch cast must not be removed";
}

// int32_t b = (int32_t)f  where f is float → FpToInt cast removed (float-to-int implicit).
TEST_F(CCastOptimizerTests,
FloatToSignedIntCastInAssignIsRemoved) {
	auto varF = Variable::create("f", FloatType::create(32));
	auto varB = Variable::create("b", IntType::create(32, true));
	testFunc->addLocalVar(varF);
	testFunc->addLocalVar(varB);
	auto castExpr = FPToIntCastExpr::create(varF, IntType::create(32, true));
	auto assign = AssignStmt::create(varB, castExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varF, assign->getRhs())
		<< "float-to-int cast should be removed (implicit in C)";
}

// float32_t b = (float32_t)a  where a is int32_t → IntToFp cast removed.
TEST_F(CCastOptimizerTests,
IntToFloatCastInAssignIsRemoved) {
	auto varA = Variable::create("a", IntType::create(32, true));
	auto varB = Variable::create("b", FloatType::create(32));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto castExpr = IntToFPCastExpr::create(varA, FloatType::create(32));
	auto assign = AssignStmt::create(varB, castExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varA, assign->getRhs())
		<< "int-to-float cast should be removed (implicit in C)";
}

// float32_t b = (float32_t)f16  where f16 is float16_t → float-to-float removed.
TEST_F(CCastOptimizerTests,
FloatToFloatCastInAssignIsRemoved) {
	auto varF16 = Variable::create("f16", FloatType::create(16));
	auto varB   = Variable::create("b",   FloatType::create(32));
	testFunc->addLocalVar(varF16);
	testFunc->addLocalVar(varB);
	auto castExpr = ExtCastExpr::create(varF16, FloatType::create(32),
		ExtCastExpr::Variant::FPExt);
	auto assign = AssignStmt::create(varB, castExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varF16, assign->getRhs())
		<< "float-to-float cast should be removed (implicit in C)";
}

// VarDefStmt: int32_t b = (int32_t)a  → cast stripped.
TEST_F(CCastOptimizerTests,
IntToIntCastInVarDefStmtIsRemoved) {
	auto varA = Variable::create("a", IntType::create(16, true));
	auto varB = Variable::create("b", IntType::create(32, true));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto castExpr = ExtCastExpr::create(varA, IntType::create(32, true),
		ExtCastExpr::Variant::SExt);
	auto vd = VarDefStmt::create(varB, castExpr);
	testFunc->setBody(vd);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varA, vd->getInitializer())
		<< "int-to-int cast in VarDefStmt initializer should be removed";
}

// ReturnStmt: return (int32_t)a  where func returns int32_t → cast stripped.
TEST_F(CCastOptimizerTests,
IntToIntCastInReturnStmtIsRemoved) {
	// Ensure the function has a non-void return type.
	testFunc->setRetType(IntType::create(32, true));
	auto varA = Variable::create("a", IntType::create(16, true));
	testFunc->addLocalVar(varA);
	auto castExpr = ExtCastExpr::create(varA, IntType::create(32, true),
		ExtCastExpr::Variant::SExt);
	auto retStmt = ReturnStmt::create(castExpr);
	testFunc->setBody(retStmt);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varA, retStmt->getRetVal())
		<< "int-to-int cast in ReturnStmt should be removed";
}

// Non-cast RHS should be left unchanged.
TEST_F(CCastOptimizerTests,
NonCastRhsIsNotTouched) {
	auto varA = Variable::create("a", IntType::create(32, true));
	auto varB = Variable::create("b", IntType::create(32, true));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto assign = AssignStmt::create(varB, varA);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varA, assign->getRhs())
		<< "plain variable RHS should not be modified";
}

// TruncCast: int16_t b = (int16_t)a  where a is int32_t → removed (int-to-int).
TEST_F(CCastOptimizerTests,
TruncCastIntToIntIsRemoved) {
	auto varA = Variable::create("a", IntType::create(32, true));
	auto varB = Variable::create("b", IntType::create(16, true));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto castExpr = TruncCastExpr::create(varA, IntType::create(16, true));
	auto assign = AssignStmt::create(varB, castExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<CCastOptimizer>(module);

	EXPECT_EQ(varA, assign->getRhs())
		<< "int-to-int truncation cast should be removed";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
