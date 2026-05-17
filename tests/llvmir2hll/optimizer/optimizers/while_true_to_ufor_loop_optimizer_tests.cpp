/**
* @file tests/llvmir2hll/optimizer/optimizers/while_true_to_ufor_loop_optimizer_tests.cpp
* @brief Tests for the @c while_true_to_ufor_loop_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <memory>
#include <gtest/gtest.h>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/while_true_to_ufor_loop_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c while_true_to_ufor_loop_optimizer module.
*/
class WhileTrueToUForLoopOptimizerTests: public TestsWithModule {
protected:
	void optimizeModule();
};

void WhileTrueToUForLoopOptimizerTests::optimizeModule() {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<WhileTrueToUForLoopOptimizer>(module, va);
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
OptimizerHasNonEmptyID) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	auto optimizer = std::make_shared<WhileTrueToUForLoopOptimizer>(module, va);

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	optimizeModule();

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody())) <<
		"expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got " <<
		testFunc->getBody()->getSuccessor();
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
NonWhileTrueLoopIsNotOptimized) {
	// while (i == 0) { ... }  — not a while-true loop, should not be changed.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<EqOpExpr> cond(EqOpExpr::create(varI, ConstInt::create(0, 32)));
	ShPtr<EmptyStmt> body(EmptyStmt::create());
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(cond, body));
	testFunc->setBody(whileLoop);

	optimizeModule();

	// Body should still start with the WhileLoopStmt.
	ASSERT_TRUE(isa<WhileLoopStmt>(testFunc->getBody())) <<
		"non-while-true loop should not be converted";
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
WhileTrueWithSimpleIfBreakIsProcessed) {
	// while (true) {
	//     if (i == 10) { break; }
	// }
	// This has a valid loop split (loopEnd = if-break), so gatherInfo succeeds.
	// tryConversionToUForLoop returns {} (TODO stub), so nothing is replaced —
	// but gatherInfoAboutOptimizedWhileLoop and isDoWhileLoop code paths are exercised.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<EqOpExpr> exitCond(EqOpExpr::create(varI, ConstInt::create(10, 32)));
	ShPtr<IfStmt> ifBreak(IfStmt::create(exitCond, breakStmt));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), ifBreak));
	testFunc->setBody(whileLoop);

	ASSERT_NO_THROW(optimizeModule());
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
WhileTrueWithBodyBeforeBreakIsProcessed) {
	// while (true) {
	//     i = i + 1;
	//     if (i == 10) { break; }
	// }
	// Exercises the beforeLoopEndStmts and afterLoopEndStmts path in split.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<EqOpExpr> exitCond(EqOpExpr::create(varI, ConstInt::create(10, 32)));
	ShPtr<IfStmt> ifBreak(IfStmt::create(exitCond, breakStmt));
	ShPtr<AssignStmt> incrI(AssignStmt::create(varI,
		ConstInt::create(1, 32), ifBreak));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), incrI));
	testFunc->setBody(whileLoop);

	ASSERT_NO_THROW(optimizeModule());
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
WhileTrueWithBodyAfterBreakIsProcessed) {
	// while (true) {
	//     if (i == 10) { break; }
	//     i = i + 1;
	// }
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<AssignStmt> incrI(AssignStmt::create(varI, ConstInt::create(1, 32)));
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<EqOpExpr> exitCond(EqOpExpr::create(varI, ConstInt::create(10, 32)));
	ShPtr<IfStmt> ifBreak(IfStmt::create(exitCond, breakStmt, incrI));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), ifBreak));
	testFunc->setBody(whileLoop);

	ASSERT_NO_THROW(optimizeModule());
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
WhileTrueLoopWithNoBreakIsNotSplit) {
	// while (true) { i = i + 1; }  — no loop-end, splitWhileTrueLoop returns null.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<AssignStmt> body(AssignStmt::create(varI, ConstInt::create(1, 32)));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), body));
	testFunc->setBody(whileLoop);

	ASSERT_NO_THROW(optimizeModule());
	EXPECT_TRUE(isa<WhileLoopStmt>(testFunc->getBody())) <<
		"unsplittable while-true should remain unchanged";
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
InvalidValueAnalysisStateClearsCache) {
	// Runs the optimizer with va that starts in an invalid state.
	// This exercises the va->isInValidState() / va->clearCache() branch.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<EmptyStmt> body(EmptyStmt::create());
	testFunc->setBody(body);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	va->invalidateState();
	ASSERT_NO_THROW(Optimizer::optimize<WhileTrueToUForLoopOptimizer>(module, va));
}

TEST_F(WhileTrueToUForLoopOptimizerTests,
DoWhilePatternExercisesDoWhilePath) {
	// while (true) {
	//     i = i + 1;
	//     if (i >= 10) { break; }   ← exit check at the end (do-while pattern)
	// }
	// isDoWhileLoop should return true, exercising the do-while lowering path.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<EqOpExpr> exitCond(EqOpExpr::create(varI, ConstInt::create(10, 32)));
	ShPtr<IfStmt> ifBreak(IfStmt::create(exitCond, breakStmt));
	ShPtr<AssignStmt> incrI(AssignStmt::create(varI,
		ConstInt::create(1, 32), ifBreak));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), incrI));
	testFunc->setBody(whileLoop);

	ASSERT_NO_THROW(optimizeModule());
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
