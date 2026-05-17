/**
* @file tests/llvmir2hll/optimizer/optimizers/loop_last_continue_optimizer_tests.cpp
* @brief Tests for the @c loop_last_continue_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/loop_last_continue_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class LastLoopContinueOptimizerTests: public TestsWithModule {};

TEST_F(LastLoopContinueOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<LoopLastContinueOptimizer> optimizer(
		new LoopLastContinueOptimizer(module));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(LastLoopContinueOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	Optimizer::optimize<LoopLastContinueOptimizer>(module);

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody())) <<
		"expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got " <<
		testFunc->getBody()->getSuccessor();
}

TEST_F(LastLoopContinueOptimizerTests,
WhileLoopWithTrailingContinueIsRemoved) {
	// while (true) { i = i + 1; continue; }
	// → while (true) { i = i + 1; }
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<ContinueStmt> cont(ContinueStmt::create());
	ShPtr<AssignStmt> incrI(AssignStmt::create(varI,
		ConstInt::create(1, 32), cont));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), incrI));
	testFunc->setBody(whileLoop);

	Optimizer::optimize<LoopLastContinueOptimizer>(module);

	// The body's last statement should no longer be a ContinueStmt.
	auto lastStmt = Statement::getLastStatement(whileLoop->getBody());
	EXPECT_FALSE(isa<ContinueStmt>(lastStmt)) <<
		"trailing continue should have been removed";
}

TEST_F(LastLoopContinueOptimizerTests,
WhileLoopWithOnlyContinueIsNotOptimized) {
	// while (true) { continue; }  — continue is the only statement, must stay.
	ShPtr<ContinueStmt> cont(ContinueStmt::create());
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), cont));
	testFunc->setBody(whileLoop);

	Optimizer::optimize<LoopLastContinueOptimizer>(module);

	// Body should still be the ContinueStmt.
	EXPECT_TRUE(isa<ContinueStmt>(whileLoop->getBody())) <<
		"continue-only while loop body should remain unchanged";
}

TEST_F(LastLoopContinueOptimizerTests,
WhileLoopWithoutTrailingContinueIsNotOptimized) {
	// while (true) { i = i + 1; }  — no trailing continue.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<AssignStmt> incrI(AssignStmt::create(varI, ConstInt::create(1, 32)));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), incrI));
	testFunc->setBody(whileLoop);

	Optimizer::optimize<LoopLastContinueOptimizer>(module);

	// Body should still end with the assignment (no continue removed).
	auto lastStmt = Statement::getLastStatement(whileLoop->getBody());
	EXPECT_TRUE(isa<AssignStmt>(lastStmt)) <<
		"loop without continue should not be changed";
}

TEST_F(LastLoopContinueOptimizerTests,
ForLoopWithTrailingContinueIsRemoved) {
	// for (i = 0; i < 10; i = i + 1) { j = j + 1; continue; }
	// → for (i = 0; i < 10; i = i + 1) { j = j + 1; }
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	ShPtr<Variable> varJ(Variable::create("j", IntType::create(32)));
	testFunc->addLocalVar(varI);
	testFunc->addLocalVar(varJ);
	ShPtr<ContinueStmt> cont(ContinueStmt::create());
	ShPtr<AssignStmt> incrJ(AssignStmt::create(varJ,
		ConstInt::create(1, 32), cont));
	// Create a for loop: for (i = 0; i < 10; i + 1)
	ShPtr<ForLoopStmt> forLoop(ForLoopStmt::create(
		varI, ConstInt::create(0, 32),
		ConstInt::create(10, 32),
		ConstInt::create(1, 32), incrJ));
	testFunc->setBody(forLoop);

	Optimizer::optimize<LoopLastContinueOptimizer>(module);

	// The last statement in the for body should not be ContinueStmt.
	auto lastStmt = Statement::getLastStatement(forLoop->getBody());
	EXPECT_FALSE(isa<ContinueStmt>(lastStmt)) <<
		"trailing continue in for loop should have been removed";
}

TEST_F(LastLoopContinueOptimizerTests,
ForLoopWithOnlyContinueIsNotOptimized) {
	// for (i = 0; ...) { continue; }  — continue is the only statement, must stay.
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<ContinueStmt> cont(ContinueStmt::create());
	ShPtr<ForLoopStmt> forLoop(ForLoopStmt::create(
		varI, ConstInt::create(0, 32),
		ConstInt::create(10, 32),
		ConstInt::create(1, 32), cont));
	testFunc->setBody(forLoop);

	Optimizer::optimize<LoopLastContinueOptimizer>(module);

	EXPECT_TRUE(isa<ContinueStmt>(forLoop->getBody())) <<
		"continue-only for loop body should remain unchanged";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
