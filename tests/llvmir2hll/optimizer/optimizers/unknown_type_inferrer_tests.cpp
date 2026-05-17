/**
* @file tests/llvmir2hll/optimizer/optimizers/unknown_type_inferrer_tests.cpp
* @brief Tests for the @c unknown_type_inferrer module.
* @copyright (c) 2024, MIT license
*
* Verifies that variables with UnknownType are assigned concrete types based on
* their usage context:
*  - assignment propagation (LHS gets type of RHS and vice-versa)
*  - dereference context implies pointer type
*  - array-index base implies pointer, index implies int
*  - NULL comparison implies pointer
*/

#include <gtest/gtest.h>

#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/array_index_op_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/const_null_pointer.h"
#include "retdec/llvmir2hll/ir/deref_op_expr.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/unknown_type.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/void_type.h"
#include "retdec/llvmir2hll/optimizer/optimizers/unknown_type_inferrer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c unknown_type_inferrer module.
*/
class UnknownTypeInferrerTests: public TestsWithModule {};

TEST_F(UnknownTypeInferrerTests,
OptimizerHasNonEmptyID) {
	auto opt = std::make_shared<UnknownTypeInferrer>(module);
	EXPECT_FALSE(opt->getId().empty());
}

TEST_F(UnknownTypeInferrerTests,
InEmptyBodyThereIsNothingToOptimize) {
	Optimizer::optimize<UnknownTypeInferrer>(module);

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody()))
		<< "expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_FALSE(testFunc->getBody()->hasSuccessor());
}

// Variables with concrete types should not be changed.
TEST_F(UnknownTypeInferrerTests,
ConcreteTypeVariableUnchanged) {
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);
	auto assign = AssignStmt::create(varA,
		ConstInt::create(llvm::APInt(32, 5)));
	testFunc->setBody(assign);

	Optimizer::optimize<UnknownTypeInferrer>(module);

	EXPECT_TRUE(isa<IntType>(varA->getType()))
		<< "concrete-type variable should remain int32";
}

// Assignment propagation: unknown_var = known_typed_expr → var gets known type.
TEST_F(UnknownTypeInferrerTests,
AssignmentPropagatesTypeFromRhsToUnknownLhs) {
	// void test() { unknown_var = (int32) 0; }
	// → unknown_var should be inferred as int32.
	auto varU = Variable::create("u", UnknownType::create());
	testFunc->addLocalVar(varU);
	auto c0 = ConstInt::create(llvm::APInt(32, 0));
	auto assign = AssignStmt::create(varU, c0);
	testFunc->setBody(assign);

	Optimizer::optimize<UnknownTypeInferrer>(module);

	// After inference, the variable's type should be concrete (int32 or similar)
	EXPECT_FALSE(isa<UnknownType>(varU->getType()))
		<< "type should have been inferred from int32 RHS";
}

// VarDef propagation: int32 vd = unknown_var → unknown_var gets int32.
TEST_F(UnknownTypeInferrerTests,
VarDefInitializerPropagatesTypeToUnknownVar) {
	// void test() { int32 a = u; }   (u has UnknownType)
	// → u should be inferred as int32.
	auto varU = Variable::create("u", UnknownType::create());
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varU);
	testFunc->addLocalVar(varA);
	auto vd = VarDefStmt::create(varA, varU);
	testFunc->setBody(vd);

	Optimizer::optimize<UnknownTypeInferrer>(module);

	EXPECT_FALSE(isa<UnknownType>(varU->getType()))
		<< "u should be inferred as int32 from typed LHS";
}

// Dereference context: *u → u should become void* (or similar pointer type).
TEST_F(UnknownTypeInferrerTests,
DerefContextImpliesPointerType) {
	// void test() { int32 a = *u; }   (u has UnknownType)
	// → u should become a pointer type.
	auto varU = Variable::create("u", UnknownType::create());
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varU);
	testFunc->addLocalVar(varA);
	auto deref = DerefOpExpr::create(varU);
	auto assign = AssignStmt::create(varA, deref);
	testFunc->setBody(assign);

	Optimizer::optimize<UnknownTypeInferrer>(module);

	EXPECT_TRUE(isa<PointerType>(varU->getType()))
		<< "variable used in dereference should become pointer type";
}

// NULL comparison: if (u == NULL) → u becomes void*.
TEST_F(UnknownTypeInferrerTests,
NullComparisonImpliesPointerType) {
	// void test() { if (u == nullptr) {} }
	auto varU = Variable::create("u", UnknownType::create());
	testFunc->addLocalVar(varU);
	auto nullPtr = ConstNullPointer::create(PointerType::create(VoidType::create()));
	auto eqExpr = EqOpExpr::create(varU, nullPtr);
	auto emptyBody = EmptyStmt::create();
	auto ifStmt = IfStmt::create(eqExpr, emptyBody);
	testFunc->setBody(ifStmt);

	Optimizer::optimize<UnknownTypeInferrer>(module);

	EXPECT_TRUE(isa<PointerType>(varU->getType()))
		<< "variable compared to NULL should become pointer type";
}

// Array-index base: u[i] → u should become a pointer type.
TEST_F(UnknownTypeInferrerTests,
ArrayIndexBaseImpliesPointerType) {
	// void test() { int32 a = u[0]; }
	auto varU = Variable::create("u", UnknownType::create());
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varU);
	testFunc->addLocalVar(varA);
	auto idx = ConstInt::create(llvm::APInt(32, 0));
	auto aiExpr = ArrayIndexOpExpr::create(varU, idx);
	auto assign = AssignStmt::create(varA, aiExpr);
	testFunc->setBody(assign);

	Optimizer::optimize<UnknownTypeInferrer>(module);

	EXPECT_TRUE(isa<PointerType>(varU->getType()))
		<< "array-index base should become pointer type";
}

// Multiple passes: chained unknowns are resolved transitively.
TEST_F(UnknownTypeInferrerTests,
TransitivePropagationResolvesChainedUnknowns) {
	// void test() {
	//     unknown u1 = 42_int32;   ← u1 gets int32
	//     unknown u2 = u1;         ← u2 should get int32 via u1
	// }
	auto varU1 = Variable::create("u1", UnknownType::create());
	auto varU2 = Variable::create("u2", UnknownType::create());
	testFunc->addLocalVar(varU1);
	testFunc->addLocalVar(varU2);
	auto c42 = ConstInt::create(llvm::APInt(32, 42));
	auto assign2 = AssignStmt::create(varU2, varU1);
	auto assign1 = AssignStmt::create(varU1, c42, assign2);
	testFunc->setBody(assign1);

	Optimizer::optimize<UnknownTypeInferrer>(module);

	// Both should be concrete (not UnknownType) after transitive propagation.
	EXPECT_FALSE(isa<UnknownType>(varU1->getType()))
		<< "u1 should have been inferred from int32 constant";
	EXPECT_FALSE(isa<UnknownType>(varU2->getType()))
		<< "u2 should have been inferred transitively from u1";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
