/**
* @file tests/llvmir2hll/optimizer/optimizers/var_def_for_loop_optimizer_tests.cpp
* @brief Tests for the @c var_def_for_loop_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/var_def_for_loop_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c var_def_for_loop_optimizer module.
*/
class VarDefLoopOptimizerTests: public TestsWithModule {};

TEST_F(VarDefLoopOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<VarDefForLoopOptimizer> optimizer(
		new VarDefForLoopOptimizer(module));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(VarDefLoopOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	// Optimize the module.
	Optimizer::optimize<VarDefForLoopOptimizer>(module);

	// Check that the output is correct.
	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody())) <<
		"expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got " <<
		testFunc->getBody()->getSuccessor();
}

//
// Helper: build a minimal ForLoopStmt for variable varI, 0..9.
//
static ShPtr<ForLoopStmt> makeForLoop(ShPtr<Variable> varI,
		ShPtr<Statement> successor = nullptr) {
	return ForLoopStmt::create(
		varI,
		ConstInt::create(0, 32),
		LtOpExpr::create(varI, ConstInt::create(10, 32)),
		ConstInt::create(1, 32),
		EmptyStmt::create(),
		successor
	);
}

//
// Core optimization tests
//

TEST_F(VarDefLoopOptimizerTests,
VarDefStmtForIndVarIsRemovedWhenForLoopPresent) {
	// Construct:
	//   int i;               ← VarDefStmt with NO initializer, at body start
	//   for (i = 0; i < 10; i = i + 1) {}
	//
	// The VarDefStmt for `i` should be removed because `i` is the for loop's
	// induction variable.

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	testFunc->addLocalVar(varI);

	ShPtr<ForLoopStmt> forLoop(makeForLoop(varI));
	// VarDefStmt with NO initializer, successor = forLoop
	ShPtr<VarDefStmt> varDef(VarDefStmt::create(varI, nullptr, forLoop));
	testFunc->setBody(varDef);

	Optimizer::optimize<VarDefForLoopOptimizer>(module);

	// After optimization, VarDefStmt should be gone.
	// The body should start directly with the ForLoopStmt.
	ASSERT_TRUE(testFunc->getBody()) << "function body must not be null";
	EXPECT_TRUE(isa<ForLoopStmt>(testFunc->getBody())) <<
		"VarDefStmt for the for-loop induction variable should have been removed; "
		"expected ForLoopStmt as new body start, got " << testFunc->getBody();
}

TEST_F(VarDefLoopOptimizerTests,
VarDefStmtWithInitializerIsNotRemoved) {
	// Construct:
	//   int i = 0;           ← VarDefStmt WITH initializer — must NOT be removed
	//   for (i = 0; i < 10; i = i + 1) {}

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	testFunc->addLocalVar(varI);

	ShPtr<ForLoopStmt> forLoop(makeForLoop(varI));
	// VarDefStmt WITH initializer
	ShPtr<VarDefStmt> varDef(VarDefStmt::create(
		varI, ConstInt::create(0, 32), forLoop));
	testFunc->setBody(varDef);

	Optimizer::optimize<VarDefForLoopOptimizer>(module);

	// The VarDefStmt with an initializer must remain.
	EXPECT_TRUE(isa<VarDefStmt>(testFunc->getBody())) <<
		"VarDefStmt with an initializer should NOT be removed";
}

TEST_F(VarDefLoopOptimizerTests,
VarDefForUnrelatedVariableIsNotRemoved) {
	// Construct:
	//   int j;               ← VarDefStmt for `j` (not the for-loop ind. var)
	//   for (i = 0; i < 10; i = i + 1) {}

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	ShPtr<Variable> varJ(Variable::create("j", IntType::create(32, true)));
	testFunc->addLocalVar(varI);
	testFunc->addLocalVar(varJ);

	ShPtr<ForLoopStmt> forLoop(makeForLoop(varI));
	// VarDefStmt for `j`, not for `i`
	ShPtr<VarDefStmt> varDefJ(VarDefStmt::create(varJ, nullptr, forLoop));
	testFunc->setBody(varDefJ);

	Optimizer::optimize<VarDefForLoopOptimizer>(module);

	// `j`'s VarDefStmt should remain because `j` is not a for-loop ind. var.
	EXPECT_TRUE(isa<VarDefStmt>(testFunc->getBody())) <<
		"VarDefStmt for an unrelated variable should not be removed";
}

TEST_F(VarDefLoopOptimizerTests,
MultipleVarDefsBeforeForLoopAreRemovedForIndVarsOnly) {
	// Construct:
	//   int i;               ← ind. var of for loop → removed
	//   int j;               ← not an ind. var → kept (but optimizer stops here)
	//   for (i = 0; i < 10; i = i + 1) {}
	//
	// The optimizer removes no-init VarDefStmts at the start until it hits
	// one that is not an induction variable or has an initializer.

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	ShPtr<Variable> varJ(Variable::create("j", IntType::create(32, true)));
	testFunc->addLocalVar(varI);
	testFunc->addLocalVar(varJ);

	ShPtr<ForLoopStmt> forLoop(makeForLoop(varI));
	// chain: varI_def → varJ_def → forLoop
	ShPtr<VarDefStmt> varDefJ(VarDefStmt::create(varJ, nullptr, forLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, nullptr, varDefJ));
	testFunc->setBody(varDefI);

	Optimizer::optimize<VarDefForLoopOptimizer>(module);

	// varI's VarDefStmt should be removed.
	// The body should now start with varJ's VarDefStmt (optimizer stops there).
	auto body = testFunc->getBody();
	EXPECT_TRUE(isa<VarDefStmt>(body)) <<
		"body should start with VarDefStmt for j after i's def is removed";
	if (isa<VarDefStmt>(body)) {
		auto vds = cast<VarDefStmt>(body);
		EXPECT_EQ(varJ, vds->getVar()) <<
			"remaining VarDefStmt should be for variable j";
	}
}

TEST_F(VarDefLoopOptimizerTests,
ForLoopWithoutPrecedingVarDefIsNotChanged) {
	// A for loop with no preceding VarDefStmt: nothing to optimize.
	//   for (i = 0; i < 10; i = i + 1) {}

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	testFunc->addLocalVar(varI);

	ShPtr<ForLoopStmt> forLoop(makeForLoop(varI));
	testFunc->setBody(forLoop);

	Optimizer::optimize<VarDefForLoopOptimizer>(module);

	// Body should still be the for loop.
	EXPECT_TRUE(isa<ForLoopStmt>(testFunc->getBody())) <<
		"for loop without preceding VarDef should not be changed";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
