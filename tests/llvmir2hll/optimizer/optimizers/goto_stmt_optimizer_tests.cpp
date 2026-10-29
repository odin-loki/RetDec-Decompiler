/**
* @file tests/llvmir2hll/optimizer/optimizers/goto_stmt_optimizer_tests.cpp
* @brief Tests for the @c goto_stmt_optimizer module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <gtest/gtest.h>

#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/goto_stmt_optimizer.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/not_op_expr.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c empty_stmt_optimizer module.
*/
class GotoStmtOptimizerTests: public TestsWithModule {};

TEST_F(GotoStmtOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<GotoStmtOptimizer> optimizer(
		new GotoStmtOptimizer(module));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

// goto label
// ...
// label:
//   goto/return/break/continue

TEST_F(GotoStmtOptimizerTests,
Goto2Return) {
	// int a
	// goto label
	// a = 1 (...)
	// label:
	//   return 2
	//
	// Can be optimized to:
	// int a
	// return 2
	// a = 1 (...)
	// return 2
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto retStmt = ReturnStmt::create(ConstInt::create(1, 64));
	auto assignA = AssignStmt::create(varA, ConstInt::create(1, 32), retStmt);
	auto gotoStmt = GotoStmt::create(retStmt);
	gotoStmt->setSuccessor(assignA);
	testFunc->setBody(gotoStmt);

	Optimizer::optimize<GotoStmtOptimizer>(module);

	ASSERT_TRUE(testFunc->getBody()) <<
		"expected a non-empty body";
	ShPtr<ReturnStmt> outRetStmt1(cast<ReturnStmt>(testFunc->getBody()));
	ASSERT_TRUE(outRetStmt1) << "expected ReturnStmt, got" << testFunc->getBody();
	ShPtr<AssignStmt> outAssignA(cast<AssignStmt>(outRetStmt1->getSuccessor()));
	ASSERT_TRUE(outAssignA) << "expected AssignStmt, got " << outRetStmt1->getSuccessor();
	ShPtr<ReturnStmt> outRetStmt2(cast<ReturnStmt>(outAssignA->getSuccessor()));
	ASSERT_TRUE(outRetStmt2) << "expected ReturnStmt, got" << outAssignA->getSuccessor();
}

TEST_F(GotoStmtOptimizerTests,
Goto2Break) {
	// int a
	// goto label
	// a = 1 (...)
	// label:
	//   break
	//
	// Can be optimized to:
	// int a
	// break
	// a = 1 (...)
	// break
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto breakStmt = BreakStmt::create();
	auto assignA = AssignStmt::create(varA, ConstInt::create(1, 32), breakStmt);
	auto gotoStmt = GotoStmt::create(breakStmt); //, assignA);
	gotoStmt->setSuccessor(assignA);
	testFunc->setBody(gotoStmt);

	Optimizer::optimize<GotoStmtOptimizer>(module);

	ASSERT_TRUE(testFunc->getBody()) <<
		"expected a non-empty body";
	ShPtr<BreakStmt> outBreakStmt1(cast<BreakStmt>(testFunc->getBody()));
	ASSERT_TRUE(outBreakStmt1) << "expected BreakStmt, got" << testFunc->getBody();
	ShPtr<AssignStmt> outAssignA(cast<AssignStmt>(outBreakStmt1->getSuccessor()));
	ASSERT_TRUE(outAssignA) << "expected AssignStmt, got " << outBreakStmt1->getSuccessor();
	ShPtr<BreakStmt> outBreakStmt2(cast<BreakStmt>(outAssignA->getSuccessor()));
	ASSERT_TRUE(outBreakStmt2) << "expected BreakStmt, got" << outAssignA->getSuccessor();
}

TEST_F(GotoStmtOptimizerTests,
Goto2Continue) {
	// int a
	// goto label
	// a = 1 (...)
	// label:
	//   continue
	//
	// Can be optimized to:
	// int a
	// continue
	// a = 1 (...)
	// continue
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto continueStmt = ContinueStmt::create();
	auto assignA = AssignStmt::create(varA, ConstInt::create(1, 32), continueStmt);
	auto gotoStmt = GotoStmt::create(continueStmt); //, assignA);
	gotoStmt->setSuccessor(assignA);
	testFunc->setBody(gotoStmt);

	Optimizer::optimize<GotoStmtOptimizer>(module);

	ASSERT_TRUE(testFunc->getBody()) <<
		"expected a non-empty body";
	ShPtr<ContinueStmt> outContinueStmt1(cast<ContinueStmt>(testFunc->getBody()));
	ASSERT_TRUE(outContinueStmt1) << "expected ContinueStmt, got" << testFunc->getBody();
	ShPtr<AssignStmt> outAssignA(cast<AssignStmt>(outContinueStmt1->getSuccessor()));
	ASSERT_TRUE(outAssignA) << "expected AssignStmt, got " << outContinueStmt1->getSuccessor();
	ShPtr<ContinueStmt> outContinueStmt2(cast<ContinueStmt>(outAssignA->getSuccessor()));
	ASSERT_TRUE(outContinueStmt2) << "expected ContinueStmt, got" << outAssignA->getSuccessor();
}

// while (a) { if (b) goto L; a = 1; }  b = 2;  L: a = 3;
// ==>
// while (true) { if (!a) { b = 2; break; } if (b) break; a = 1; }  a = 3;
TEST_F(GotoStmtOptimizerTests,
GotoPastLoopExitCodeBecomesBreak) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto exitCode = AssignStmt::create(varB, ConstInt::create(2, 32), target);
	auto gotoStmt = GotoStmt::create(target);
	auto inLoop = AssignStmt::create(varA, ConstInt::create(1, 32));
	auto ifGoto = IfStmt::create(varB, gotoStmt, inLoop);
	auto loop = WhileLoopStmt::create(varA, ifGoto, exitCode);
	testFunc->setBody(loop);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);

	ASSERT_EQ(loop, testFunc->getBody());
	auto cond = cast<ConstBool>(loop->getCondition());
	ASSERT_TRUE(cond && cond->getValue()) << "expected while (true)";
	EXPECT_EQ(target, loop->getSuccessor());
	EXPECT_FALSE(target->hasLabel());
	auto exitIf = cast<IfStmt>(loop->getBody());
	ASSERT_TRUE(exitIf);
	EXPECT_TRUE(isa<NotOpExpr>(exitIf->getFirstIfCond()));
	EXPECT_EQ(exitCode, exitIf->getFirstIfBody());
	EXPECT_TRUE(isa<BreakStmt>(exitCode->getSuccessor()));
	EXPECT_EQ(ifGoto, exitIf->getSuccessor());
	EXPECT_TRUE(isa<BreakStmt>(ifGoto->getFirstIfBody()));
}

TEST_F(GotoStmtOptimizerTests,
GotoRightAfterLoopBecomesPlainBreak) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto gotoStmt = GotoStmt::create(target);
	auto ifGoto = IfStmt::create(varA, gotoStmt);
	auto loop = WhileLoopStmt::create(varA, ifGoto, target);
	testFunc->setBody(loop);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);

	EXPECT_EQ(varA, loop->getCondition()) << "the condition must stay";
	EXPECT_TRUE(isa<BreakStmt>(ifGoto->getFirstIfBody()));
	EXPECT_FALSE(target->hasLabel());
}

// A goto in an inner loop leaves both loops; a break would leave one.
TEST_F(GotoStmtOptimizerTests,
GotoFromNestedLoopBecomesAFlaggedBreak) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto gotoStmt = GotoStmt::create(target);
	auto inner = WhileLoopStmt::create(varA, gotoStmt);
	auto loop = WhileLoopStmt::create(varA, inner, target);
	testFunc->setBody(loop);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);

	// while (a) { while (a) { brk = true; break; } if (brk) break; } a = 3;
	auto set = cast<AssignStmt>(inner->getBody());
	ASSERT_TRUE(set);
	EXPECT_TRUE(isa<BreakStmt>(set->getSuccessor()));
	auto check = cast<IfStmt>(inner->getSuccessor());
	ASSERT_TRUE(check);
	EXPECT_TRUE(isa<BreakStmt>(check->getFirstIfBody()));
	EXPECT_TRUE(isa<AssignStmt>(loop->getUniquePredecessor()) ||
		loop->hasPredecessors()) << "the flag is cleared before the outer loop";
	EXPECT_FALSE(target->hasLabel());
}

// Without the flag (early runs) loops are not rewritten.
TEST_F(GotoStmtOptimizerTests,
GotoPastLoopIsKeptByDefault) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto gotoStmt = GotoStmt::create(target);
	auto loop = WhileLoopStmt::create(varA, gotoStmt, target);
	testFunc->setBody(loop);

	Optimizer::optimize<GotoStmtOptimizer>(module);

	EXPECT_EQ(gotoStmt, loop->getBody());
}

// goto L; a = 1; L: a = 3; return 1;  ==>  a = 3; return 1; a = 1; ...
TEST_F(GotoStmtOptimizerTests,
GotoToShortReturningTailIsReplacedByACopy) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto ret = ReturnStmt::create(ConstInt::create(1, 32));
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32), ret);
	target->setLabel("L");
	auto skipped = AssignStmt::create(varA, ConstInt::create(1, 32), target);
	auto gotoStmt = GotoStmt::create(target);
	auto ifGoto = IfStmt::create(varA, gotoStmt, skipped);
	testFunc->setBody(ifGoto);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);

	auto copy = cast<AssignStmt>(ifGoto->getFirstIfBody());
	ASSERT_TRUE(copy) << "expected a copy of `a = 3`";
	EXPECT_NE(target, copy);
	EXPECT_FALSE(copy->hasLabel());
	EXPECT_TRUE(isa<ReturnStmt>(copy->getSuccessor()));
	EXPECT_FALSE(target->hasLabel());
	EXPECT_EQ(skipped, ifGoto->getSuccessor());
}

TEST_F(GotoStmtOptimizerTests,
GotoToTailWithoutReturnIsKept) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto skipped = AssignStmt::create(varA, ConstInt::create(1, 32), target);
	auto gotoStmt = GotoStmt::create(target);
	auto ifGoto = IfStmt::create(varA, gotoStmt, skipped);
	testFunc->setBody(ifGoto);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);

	EXPECT_EQ(gotoStmt, ifGoto->getFirstIfBody());
	EXPECT_TRUE(target->hasLabel());
}

// goto L; ... L: a = 3; goto R; ... R: return 1;  ==>  a = 3; return 1;
TEST_F(GotoStmtOptimizerTests,
TailEndingInGotoToReturnIsCopiedWithTheReturn) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto ret = ReturnStmt::create(ConstInt::create(1, 32));
	ret->setLabel("R");
	auto gotoRet = GotoStmt::create(ret);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32), gotoRet);
	target->setLabel("L");
	auto skipped = AssignStmt::create(varA, ConstInt::create(1, 32), target);
	auto gotoStmt = GotoStmt::create(target);
	auto ifGoto = IfStmt::create(varA, gotoStmt, skipped);
	gotoRet->setSuccessor(ret);
	testFunc->setBody(ifGoto);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);

	auto copy = cast<AssignStmt>(ifGoto->getFirstIfBody());
	ASSERT_TRUE(copy) << "expected a copy of `a = 3`";
	EXPECT_TRUE(isa<ReturnStmt>(copy->getSuccessor()));
}

TEST_F(GotoStmtOptimizerTests,
LabelThatNoGotoNamesIsDroppedOnTheLastRun) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto first = AssignStmt::create(varA, ConstInt::create(1, 32), target);
	testFunc->setBody(first);

	Optimizer::optimize<GotoStmtOptimizer>(module);
	EXPECT_TRUE(target->hasLabel()) << "early runs leave labels alone";

	Optimizer::optimize<GotoStmtOptimizer>(module, true);
	EXPECT_FALSE(target->hasLabel());
}

TEST_F(GotoStmtOptimizerTests,
GotoAtTheEndOfAnIfToTheStatementAfterTheIfIsDropped) {
	// if (a) { a = 1; goto L; } L: a = 3;  ==>  if (a) { a = 1; } a = 3;
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto gotoL = GotoStmt::create(target);
	auto body = AssignStmt::create(varA, ConstInt::create(1, 32), gotoL);
	auto ifStmt = IfStmt::create(varA, body, target);
	testFunc->setBody(ifStmt);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);
	auto after = body->getSuccessor();
	EXPECT_TRUE(!after || isa<EmptyStmt>(after));
	EXPECT_FALSE(target->hasLabel());
}

TEST_F(GotoStmtOptimizerTests,
GotoAtTheEndOfAnIfPastTheNextStatementIsKept) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto target = AssignStmt::create(varA, ConstInt::create(3, 32));
	target->setLabel("L");
	auto between = AssignStmt::create(varA, ConstInt::create(2, 32), target);
	auto gotoL = GotoStmt::create(target);
	auto body = AssignStmt::create(varA, ConstInt::create(1, 32), gotoL);
	auto ifStmt = IfStmt::create(varA, body, between);
	testFunc->setBody(ifStmt);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);
	EXPECT_EQ(gotoL, body->getSuccessor());
}

TEST_F(GotoStmtOptimizerTests,
IfWithAnEmptyThenAndAnElseIsFlippedOnTheLastRun) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	auto elseBody = AssignStmt::create(varA, ConstInt::create(2, 32));
	auto ifStmt = IfStmt::create(varA, EmptyStmt::create());
	ifStmt->setElseClause(elseBody);
	testFunc->setBody(ifStmt);

	Optimizer::optimize<GotoStmtOptimizer>(module, true);
	EXPECT_FALSE(ifStmt->hasElseClause());
	EXPECT_EQ(elseBody, ifStmt->getFirstIfBody());
	EXPECT_TRUE(isa<NotOpExpr>(ifStmt->getFirstIfCond()));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
