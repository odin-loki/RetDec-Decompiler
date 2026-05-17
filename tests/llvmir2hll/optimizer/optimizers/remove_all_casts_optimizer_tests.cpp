/**
* @file tests/llvmir2hll/optimizer/optimizers/remove_all_casts_optimizer_tests.cpp
* @brief Tests for the @c remove_all_casts_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/bit_cast_expr.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/ext_cast_expr.h"
#include "retdec/llvmir2hll/ir/float_type.h"
#include "retdec/llvmir2hll/ir/fp_to_int_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_to_fp_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_to_ptr_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/ptr_to_int_cast_expr.h"
#include "retdec/llvmir2hll/ir/trunc_cast_expr.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/remove_all_casts_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class RemoveAllCastsOptimizerTests: public TestsWithModule {};

TEST_F(RemoveAllCastsOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<RemoveAllCastsOptimizer> optimizer(
		new RemoveAllCastsOptimizer(module));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(RemoveAllCastsOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody())) <<
		"expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got " <<
		testFunc->getBody()->getSuccessor();
}

// Helper: set up a function body with `a = (TargetType)b` and return the stmt.
// The cast expression wraps varB.

TEST_F(RemoveAllCastsOptimizerTests,
BitCastExprIsRemoved) {
	// void test() { int32_t a; int32_t b; a = (int32_t)b; }
	// → a = b  (BitCast stripped)
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varB);
	ShPtr<BitCastExpr> castExpr(BitCastExpr::create(varB, IntType::create(32)));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, castExpr));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected BitCast to be removed, got " << assignStmt->getRhs();
}

TEST_F(RemoveAllCastsOptimizerTests,
ExtCastExprIsRemoved) {
	// a = (int32_t)b  where b is int8_t → ext cast stripped.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", IntType::create(8)));
	testFunc->addLocalVar(varB);
	ShPtr<ExtCastExpr> castExpr(ExtCastExpr::create(varB, IntType::create(32)));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, castExpr));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected ExtCast to be removed, got " << assignStmt->getRhs();
}

TEST_F(RemoveAllCastsOptimizerTests,
TruncCastExprIsRemoved) {
	// a = (int8_t)b  where b is int32_t → trunc cast stripped.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(8)));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varB);
	ShPtr<TruncCastExpr> castExpr(TruncCastExpr::create(varB, IntType::create(8)));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, castExpr));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected TruncCast to be removed, got " << assignStmt->getRhs();
}

TEST_F(RemoveAllCastsOptimizerTests,
FPToIntCastExprIsRemoved) {
	// a = (int32_t)fp_b  → fp-to-int cast stripped.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", FloatType::create(32)));
	testFunc->addLocalVar(varB);
	ShPtr<FPToIntCastExpr> castExpr(FPToIntCastExpr::create(varB, IntType::create(32)));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, castExpr));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected FPToIntCast to be removed, got " << assignStmt->getRhs();
}

TEST_F(RemoveAllCastsOptimizerTests,
IntToFPCastExprIsRemoved) {
	// a = (float)int_b  → int-to-fp cast stripped.
	ShPtr<Variable> varA(Variable::create("a", FloatType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varB);
	ShPtr<IntToFPCastExpr> castExpr(IntToFPCastExpr::create(varB, FloatType::create(32)));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, castExpr));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected IntToFPCast to be removed, got " << assignStmt->getRhs();
}

TEST_F(RemoveAllCastsOptimizerTests,
IntToPtrCastExprIsRemoved) {
	// a = (ptr)int_b  → int-to-ptr cast stripped.
	auto ptrType = PointerType::create(IntType::create(32));
	ShPtr<Variable> varA(Variable::create("a", ptrType));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varB);
	ShPtr<IntToPtrCastExpr> castExpr(IntToPtrCastExpr::create(varB, ptrType));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, castExpr));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected IntToPtrCast to be removed, got " << assignStmt->getRhs();
}

TEST_F(RemoveAllCastsOptimizerTests,
PtrToIntCastExprIsRemoved) {
	// a = (int32_t)ptr_b  → ptr-to-int cast stripped.
	auto ptrType = PointerType::create(IntType::create(32));
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", ptrType));
	testFunc->addLocalVar(varB);
	ShPtr<PtrToIntCastExpr> castExpr(PtrToIntCastExpr::create(varB, IntType::create(32)));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, castExpr));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected PtrToIntCast to be removed, got " << assignStmt->getRhs();
}

TEST_F(RemoveAllCastsOptimizerTests,
NestedCastsAreAllStripped) {
	// a = (int32_t)((int8_t)b)  → both casts stripped → a = b.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varB);
	// Inner: trunc to i8
	ShPtr<TruncCastExpr> innerCast(TruncCastExpr::create(varB, IntType::create(8)));
	// Outer: ext back to i32
	ShPtr<ExtCastExpr> outerCast(ExtCastExpr::create(innerCast, IntType::create(32)));
	ShPtr<AssignStmt> assignStmt(AssignStmt::create(varA, outerCast));
	testFunc->setBody(assignStmt);

	Optimizer::optimize<RemoveAllCastsOptimizer>(module);

	// After stripping all casts, RHS should be varB.
	EXPECT_EQ(varB, assignStmt->getRhs()) <<
		"expected nested casts to be stripped, got " << assignStmt->getRhs();
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
