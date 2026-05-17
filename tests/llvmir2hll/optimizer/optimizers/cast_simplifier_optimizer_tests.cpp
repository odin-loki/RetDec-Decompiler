/**
* @file tests/llvmir2hll/optimizer/optimizers/cast_simplifier_optimizer_tests.cpp
* @brief Tests for the @c cast_simplifier_optimizer module.
* @copyright (c) 2024, MIT license
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/bit_and_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_cast_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
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
#include "retdec/llvmir2hll/optimizer/optimizers/cast_simplifier_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class CastSimplifierOptimizerTests: public TestsWithModule {};

TEST_F(CastSimplifierOptimizerTests,
OptimizerHasNonEmptyID) {
	auto opt = std::make_shared<CastSimplifierOptimizer>(module);
	EXPECT_FALSE(opt->getId().empty());
}

TEST_F(CastSimplifierOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	Optimizer::optimize<CastSimplifierOptimizer>(module);

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody()));
	EXPECT_FALSE(testFunc->getBody()->hasSuccessor());
}

// Case 3: Identity cast cast<T>(x) where type(x)==T → x
TEST_F(CastSimplifierOptimizerTests,
Case3_IdentityBitCastEliminated) {
	// a = (int32)(int32_var)  →  a = int32_var
	auto varA = Variable::create("a", IntType::create(32));
	auto varB = Variable::create("b", IntType::create(32));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto cast = BitCastExpr::create(varB, IntType::create(32));
	auto assign = AssignStmt::create(varA, cast);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	EXPECT_EQ(varB, assign->getRhs())
		<< "identity BitCast should have been removed";
}

TEST_F(CastSimplifierOptimizerTests,
Case3_IdentityExtCastEliminated) {
	// a = (int32)(int32_var)  via ExtCast (same type → identity)
	auto varA = Variable::create("a", IntType::create(32));
	auto varB = Variable::create("b", IntType::create(32));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto cast = ExtCastExpr::create(varB, IntType::create(32),
		ExtCastExpr::Variant::ZExt);
	auto assign = AssignStmt::create(varA, cast);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	EXPECT_EQ(varB, assign->getRhs())
		<< "identity ExtCast (same type) should have been removed";
}

TEST_F(CastSimplifierOptimizerTests,
Case3_IdentityTruncCastEliminated) {
	auto varA = Variable::create("a", IntType::create(16));
	auto varB = Variable::create("b", IntType::create(16));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto cast = TruncCastExpr::create(varB, IntType::create(16));
	auto assign = AssignStmt::create(varA, cast);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	EXPECT_EQ(varB, assign->getRhs())
		<< "identity TruncCast should have been removed";
}

// Case 2: Idempotent chain cast<T>(cast<T>(x)) → cast<T>(x)
TEST_F(CastSimplifierOptimizerTests,
Case2_IdempotentBitCastChainCollapsed) {
	// a = (int32)((int32)(b))  →  a = (int32)(b)
	auto varA = Variable::create("a", IntType::create(32));
	auto varB = Variable::create("b", IntType::create(64));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto inner = BitCastExpr::create(varB, IntType::create(32));
	auto outer = BitCastExpr::create(inner, IntType::create(32));
	auto assign = AssignStmt::create(varA, outer);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	// outer should have been eliminated, inner should remain
	EXPECT_EQ(inner, assign->getRhs())
		<< "outer idempotent BitCast should have been removed";
}

TEST_F(CastSimplifierOptimizerTests,
Case2_IdempotentExtCastChainCollapsed) {
	auto varA = Variable::create("a", IntType::create(32));
	auto varB = Variable::create("b", IntType::create(8));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto inner = ExtCastExpr::create(varB, IntType::create(32),
		ExtCastExpr::Variant::ZExt);
	auto outer = ExtCastExpr::create(inner, IntType::create(32),
		ExtCastExpr::Variant::ZExt);
	auto assign = AssignStmt::create(varA, outer);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	EXPECT_EQ(inner, assign->getRhs())
		<< "idempotent ExtCast chain should collapse to inner cast";
}

// Case 4: inttoptr(ptrtoint(p)) → p  (pointer round-trip)
TEST_F(CastSimplifierOptimizerTests,
Case4_PointerRoundTripEliminated) {
	// a = (int*)(uintptr)(p)  →  a = p
	auto ptrType = PointerType::create(IntType::create(32));
	auto varA = Variable::create("a", ptrType);
	auto varP = Variable::create("p", ptrType);
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varP);
	auto ptrToInt = PtrToIntCastExpr::create(varP, IntType::create(64));
	auto intToPtr = IntToPtrCastExpr::create(ptrToInt, ptrType);
	auto assign = AssignStmt::create(varA, intToPtr);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	EXPECT_EQ(varP, assign->getRhs())
		<< "ptr→int→ptr round-trip should collapse to original pointer";
}

TEST_F(CastSimplifierOptimizerTests,
Case4_PointerRoundTripDifferentTypeNotEliminated) {
	// Different pointer types — should NOT be collapsed
	auto srcPtrType = PointerType::create(IntType::create(32));
	auto dstPtrType = PointerType::create(IntType::create(8));
	auto varA = Variable::create("a", dstPtrType);
	auto varP = Variable::create("p", srcPtrType);
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varP);
	auto ptrToInt = PtrToIntCastExpr::create(varP, IntType::create(64));
	auto intToPtr = IntToPtrCastExpr::create(ptrToInt, dstPtrType);
	auto assign = AssignStmt::create(varA, intToPtr);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	// Should NOT have been simplified (different pointer types)
	EXPECT_FALSE(isa<Variable>(assign->getRhs()))
		<< "mismatched pointer round-trip should not be simplified";
}

// Case 5: ZExt(Trunc(x, N)) where outer width == original → x & mask
TEST_F(CastSimplifierOptimizerTests,
Case5_ZextTruncToMask) {
	// zext<32>(trunc<8>(x))  where x is int32 → x & 0xFF
	auto varA = Variable::create("a", IntType::create(32));
	auto varX = Variable::create("x", IntType::create(32));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varX);
	auto trunc = TruncCastExpr::create(varX, IntType::create(8));
	auto zext  = ExtCastExpr::create(trunc, IntType::create(32),
		ExtCastExpr::Variant::ZExt);
	auto assign = AssignStmt::create(varA, zext);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	// Result should be x & 0xFF (BitAndOpExpr)
	EXPECT_TRUE(isa<BitAndOpExpr>(assign->getRhs()))
		<< "zext(trunc(x,8)) should become x & 0xFF";
}

// Case 6: cast<T1>(cast<T2>(x)) where sizeof(T1)==sizeof(x) → x
TEST_F(CastSimplifierOptimizerTests,
Case6_DownThenUpCastCancels) {
	// (int32)(int64)(int32_var)  →  int32_var
	auto varA = Variable::create("a", IntType::create(32));
	auto varX = Variable::create("x", IntType::create(32));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varX);
	auto widen = ExtCastExpr::create(varX, IntType::create(64),
		ExtCastExpr::Variant::SExt);
	auto narrow = TruncCastExpr::create(widen, IntType::create(32));
	auto assign = AssignStmt::create(varA, narrow);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	EXPECT_EQ(varX, assign->getRhs())
		<< "widen-then-narrow-to-original-size should cancel";
}

// Non-cast expression: should not be touched
TEST_F(CastSimplifierOptimizerTests,
NonCastExpressionUnchanged) {
	auto varA = Variable::create("a", IntType::create(32));
	auto varB = Variable::create("b", IntType::create(32));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto assign = AssignStmt::create(varA, varB);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	EXPECT_EQ(varB, assign->getRhs())
		<< "non-cast assignment should be unchanged";
}

// Widening ExtCast (different sizes) should NOT be eliminated
TEST_F(CastSimplifierOptimizerTests,
NontrivialWidenCastNotEliminated) {
	auto varA = Variable::create("a", IntType::create(32));
	auto varB = Variable::create("b", IntType::create(8));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto cast = ExtCastExpr::create(varB, IntType::create(32),
		ExtCastExpr::Variant::ZExt);
	auto assign = AssignStmt::create(varA, cast);
	testFunc->setBody(assign);

	Optimizer::optimize<CastSimplifierOptimizer>(module);

	// Different sizes — should remain a cast (case 3 doesn't apply)
	EXPECT_TRUE(isa<ExtCastExpr>(assign->getRhs()))
		<< "widening cast (int8 → int32) should not be eliminated";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
