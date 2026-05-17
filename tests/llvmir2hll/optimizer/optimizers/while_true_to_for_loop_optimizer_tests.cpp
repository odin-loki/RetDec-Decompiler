/**
* @file tests/llvmir2hll/optimizer/optimizers/while_true_to_for_loop_optimizer_tests.cpp
* @brief Tests for the @c while_true_to_for_loop_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluators/strict_arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/gt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/while_true_to_for_loop_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c while_true_to_for_loop_optimizer module.
*/
class WhileTrueToForLoopOptimizerTests: public TestsWithModule {
protected:
	WhileTrueToForLoopOptimizerTests();

protected:
	/// Evaluator of expressions to be used in tests.
	ShPtr<ArithmExprEvaluator> arithmExprEvaluator;
};

WhileTrueToForLoopOptimizerTests::WhileTrueToForLoopOptimizerTests():
	arithmExprEvaluator(StrictArithmExprEvaluator::create()) {}

TEST_F(WhileTrueToForLoopOptimizerTests,
OptimizerHasNonEmptyID) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	ShPtr<WhileTrueToForLoopOptimizer> optimizer(new WhileTrueToForLoopOptimizer(
		module, va, arithmExprEvaluator));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(WhileTrueToForLoopOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	// Optimize the module.
	Optimizer::optimize<WhileTrueToForLoopOptimizer>(module, va, arithmExprEvaluator);

	// Check that the output is correct.
	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody())) <<
		"expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got " <<
		testFunc->getBody()->getSuccessor();
}

//
// Core optimization: while(true) → for loop
//

TEST_F(WhileTrueToForLoopOptimizerTests,
SimpleWhileTrueWithInductionVarIsConvertedToForLoop) {
	// Construct the following code:
	//
	//   i = 0;
	//   while (true) {
	//       if (i >= 9) break;
	//       i = i + 1;
	//   }
	//
	// The exit condition `i >= 9` should produce:
	//   negated → i < 9,  +1 for updateAfterExit → i < 10
	//   for (i = 0; i < 10; i = i + 1) {}

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	testFunc->addLocalVar(varI);

	// i = i + 1  (update — must be the last stmt in the loop body)
	ShPtr<AssignStmt> updateI(AssignStmt::create(
		varI,
		AddOpExpr::create(varI, ConstInt::create(1, 32))
	));

	// if (i >= 9) break;  with successor = updateI (3rd param is successor)
	ShPtr<IfStmt> exitIf(IfStmt::create(
		GtEqOpExpr::create(varI, ConstInt::create(9, 32)),
		BreakStmt::create(),
		updateI
	));

	// while (true) { exitIf; updateI; }
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true),
		exitIf
	));

	// i = 0;  with successor = whileLoop
	ShPtr<AssignStmt> initI(AssignStmt::create(
		varI,
		ConstInt::create(0, 32),
		whileLoop
	));

	testFunc->setBody(initI);

	// Run the optimizer.
	Optimizer::optimize<WhileTrueToForLoopOptimizer>(module, va, arithmExprEvaluator);

	// The function body should now start with a ForLoopStmt.
	auto funcBody = testFunc->getBody();
	ASSERT_TRUE(isa<ForLoopStmt>(funcBody)) <<
		"expected ForLoopStmt after optimization, got " << funcBody;

	auto forLoop = cast<ForLoopStmt>(funcBody);
	EXPECT_EQ(varI, forLoop->getIndVar()) <<
		"for loop induction variable should be `i`";

	// Start value must be 0.
	auto startVal = cast<ConstInt>(forLoop->getStartValue());
	ASSERT_TRUE(startVal) << "start value should be a ConstInt";
	EXPECT_EQ(0u, startVal->getValue().getZExtValue()) <<
		"start value should be 0";

	// End condition must be a less-than comparison.
	EXPECT_TRUE(isa<LtOpExpr>(forLoop->getEndCond())) <<
		"end condition should be LtOpExpr (i < n), got " << forLoop->getEndCond();

	// Step must be 1.
	auto stepVal = cast<ConstInt>(forLoop->getStep());
	ASSERT_TRUE(stepVal) << "step should be a ConstInt";
	EXPECT_EQ(1u, stepVal->getValue().getZExtValue()) <<
		"step should be 1";
}

TEST_F(WhileTrueToForLoopOptimizerTests,
WhileTrueWithBodyBeforeExitIsConvertedToForLoop) {
	// Construct:
	//   i = 0;
	//   while (true) {
	//       x = 5;           // body executed before the exit check
	//       if (i >= 9) break;
	//       i = i + 1;
	//   }
	//
	// The body statement `x = 5` becomes the for loop body.

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	ShPtr<Variable> varX(Variable::create("x", IntType::create(32, true)));
	testFunc->addLocalVar(varI);
	testFunc->addLocalVar(varX);

	// i = i + 1  (update — last stmt)
	ShPtr<AssignStmt> updateI(AssignStmt::create(
		varI,
		AddOpExpr::create(varI, ConstInt::create(1, 32))
	));

	// if (i >= 9) break;  with successor = updateI
	ShPtr<IfStmt> exitIf(IfStmt::create(
		GtEqOpExpr::create(varI, ConstInt::create(9, 32)),
		BreakStmt::create(),
		updateI
	));

	// x = 5;  (body before exit)
	ShPtr<AssignStmt> bodyX(AssignStmt::create(
		varX,
		ConstInt::create(5, 32),
		exitIf
	));

	// while (true) { x=5; exitIf; updateI; }
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true),
		bodyX
	));

	// i = 0;
	ShPtr<AssignStmt> initI(AssignStmt::create(
		varI,
		ConstInt::create(0, 32),
		whileLoop
	));

	testFunc->setBody(initI);

	// Run the optimizer.
	Optimizer::optimize<WhileTrueToForLoopOptimizer>(module, va, arithmExprEvaluator);

	// Should start with a ForLoopStmt.
	auto funcBody = testFunc->getBody();
	ASSERT_TRUE(isa<ForLoopStmt>(funcBody)) <<
		"expected ForLoopStmt after optimization, got " << funcBody;

	auto forLoop = cast<ForLoopStmt>(funcBody);
	EXPECT_EQ(varI, forLoop->getIndVar());

	// The for loop body should contain the body statement (x = 5).
	ASSERT_TRUE(forLoop->getBody()) << "for loop body should not be null";
	EXPECT_TRUE(isa<AssignStmt>(forLoop->getBody())) <<
		"expected AssignStmt as for loop body (x = 5), got " << forLoop->getBody();
}

TEST_F(WhileTrueToForLoopOptimizerTests,
PlainWhileCondLoopIsNotOptimized) {
	// A while loop with an explicit condition (not while(true)) should not be
	// converted to a for loop by this optimizer.
	//
	//   while (i < 10) { i = i + 1; }

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	testFunc->addLocalVar(varI);

	ShPtr<AssignStmt> updateI(AssignStmt::create(
		varI,
		AddOpExpr::create(varI, ConstInt::create(1, 32))
	));

	// Not while(true) — use `i < 10` as the condition.
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		LtOpExpr::create(varI, ConstInt::create(10, 32)),
		updateI
	));
	testFunc->setBody(whileLoop);

	Optimizer::optimize<WhileTrueToForLoopOptimizer>(module, va, arithmExprEvaluator);

	// The loop should remain a WhileLoopStmt.
	EXPECT_TRUE(isa<WhileLoopStmt>(testFunc->getBody())) <<
		"a non-while-true loop should not be converted to a for loop";
}

TEST_F(WhileTrueToForLoopOptimizerTests,
WhileTrueWithNoUpdateStmtIsNotOptimized) {
	// A while(true) loop that has no update statement after the exit condition
	// cannot be converted to a for loop.
	//
	//   while (true) {
	//       if (i >= 9) break;
	//       // no i++
	//   }

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	ShPtr<Variable> varI(Variable::create("i", IntType::create(32, true)));
	testFunc->addLocalVar(varI);

	// if (i >= 9) break;  with no successor (no update)
	ShPtr<IfStmt> exitIf(IfStmt::create(
		GtEqOpExpr::create(varI, ConstInt::create(9, 32)),
		BreakStmt::create()
	));

	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true),
		exitIf
	));
	ShPtr<AssignStmt> initI(AssignStmt::create(
		varI, ConstInt::create(0, 32), whileLoop));
	testFunc->setBody(initI);

	Optimizer::optimize<WhileTrueToForLoopOptimizer>(module, va, arithmExprEvaluator);

	// Loop should not have been converted (still a WhileLoopStmt).
	// The init + while should still be present.
	EXPECT_FALSE(isa<ForLoopStmt>(testFunc->getBody())) <<
		"a while(true) without update stmt should not be converted to for loop";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
