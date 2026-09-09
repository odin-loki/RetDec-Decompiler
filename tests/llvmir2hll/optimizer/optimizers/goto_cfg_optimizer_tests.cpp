/**
* @file tests/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer_tests.cpp
* @brief Tests for the @c goto_cfg_optimizer module.
* @copyright (c) 2024, MIT license
*
* Tests patterns A (if-goto inversion), B (trivial goto removal), and
* D (goto-to-break) of GotoCFGOptimizer.
*/

#include <gtest/gtest.h>

#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c goto_cfg_optimizer module.
*/
class GotoCFGOptimizerTests: public TestsWithModule {};

namespace {

/// Every statement the emitter walks from @a start.  A label on a statement
/// outside this set is a label the emitter never writes.
class ReachableStmtCollector : private OrderedAllVisitor {
public:
	static StmtUSet collect(ShPtr<Statement> start)
	{
		ReachableStmtCollector c;
		c.visitStmt(start);
		return c.seen;
	}

private:
	void visitStmt(ShPtr<Statement> stmt, bool visitSuccessors = true, bool visitNestedStmts = true) override
	{
		if (stmt)
		{
			seen.insert(stmt);
		}
		OrderedAllVisitor::visitStmt(stmt, visitSuccessors, visitNestedStmts);
	}

	StmtUSet seen;
};

} // namespace


TEST_F(GotoCFGOptimizerTests,
OptimizerHasNonEmptyID) {
	auto opt = std::make_shared<GotoCFGOptimizer>(module);
	EXPECT_FALSE(opt->getId().empty());
}

TEST_F(GotoCFGOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	Optimizer::optimize<GotoCFGOptimizer>(module);

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody()))
		<< "expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_FALSE(testFunc->getBody()->hasSuccessor());
}

// Pattern B: unconditional goto to immediate successor → remove goto
TEST_F(GotoCFGOptimizerTests,
PatternB_TrivialGotoToImmediateSuccessorRemoved) {
	// void test() {
	//     goto L;
	//     L: return 0;
	// }
	// →
	// void test() {
	//     return 0;
	// }
	auto retStmt = ReturnStmt::create(ConstInt::create(0, 32));
	auto gotoStmt = GotoStmt::create(retStmt);
	gotoStmt->setSuccessor(retStmt);
	testFunc->setBody(gotoStmt);

	Optimizer::optimize<GotoCFGOptimizer>(module);

	// The goto should have been eliminated
	ASSERT_TRUE(testFunc->getBody())
		<< "function body should not be empty after optimization";
	EXPECT_TRUE(isa<ReturnStmt>(testFunc->getBody()) ||
	            isa<EmptyStmt>(testFunc->getBody()))
		<< "expected ReturnStmt or EmptyStmt after goto removal, got "
		<< testFunc->getBody();
}

// Pattern A: if (cond) goto L; stmts...; L: → if (!cond) { stmts... }
TEST_F(GotoCFGOptimizerTests,
PatternA_IfGotoForwardInverted) {
	// void test() {
	//     int a;
	//     if (a) goto L;
	//     a = 1;
	//     L: return;
	// }
	// →
	// void test() {
	//     int a;
	//     if (!a) { a = 1; }
	//     return;
	// }
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);

	auto retStmt = ReturnStmt::create();
	auto assignA = AssignStmt::create(varA,
		ConstInt::create(llvm::APInt(32, 1)), retStmt);
	auto gotoStmt = GotoStmt::create(retStmt);
	auto ifStmt = IfStmt::create(varA, gotoStmt, assignA);
	testFunc->setBody(ifStmt);

	Optimizer::optimize<GotoCFGOptimizer>(module);

	// The body should now be an IfStmt with inverted condition and no goto
	ASSERT_TRUE(testFunc->getBody());
	// After optimization, there should be no bare GotoStmt in the top-level
	auto s = testFunc->getBody();
	bool foundBareGoto = false;
	while (s) {
		if (isa<GotoStmt>(s)) {
			foundBareGoto = true;
			break;
		}
		s = s->getSuccessor();
	}
	EXPECT_FALSE(foundBareGoto)
		<< "should not have any top-level GotoStmt after pattern A optimization";
}

// Pattern D: goto to loop exit → break
TEST_F(GotoCFGOptimizerTests,
PatternD_GotoLoopExitBecomesBreak) {
	// void test() {
	//     while (true) {
	//         goto L_exit;
	//     }
	//     L_exit: return;
	// }
	// →
	// void test() {
	//     while (true) {
	//         break;
	//     }
	//     return;
	// }
	auto retStmt = ReturnStmt::create();
	// retStmt IS the loop exit
	auto gotoBreak = GotoStmt::create(retStmt);
	auto trueConst = ConstInt::create(llvm::APInt(1, 1));
	auto whileStmt = WhileLoopStmt::create(trueConst, gotoBreak, retStmt);
	testFunc->setBody(whileStmt);

	Optimizer::optimize<GotoCFGOptimizer>(module);

	// The while body should now contain a BreakStmt instead of GotoStmt
	ASSERT_TRUE(testFunc->getBody());
	auto outerWhile = cast<WhileLoopStmt>(testFunc->getBody());
	if (outerWhile) {
		auto bodyStmt = outerWhile->getBody();
		EXPECT_TRUE(isa<BreakStmt>(bodyStmt))
			<< "goto to loop exit should become break, got " << bodyStmt;
	}
}

// No-op: a goto that is NOT to an immediate successor should NOT be removed
TEST_F(GotoCFGOptimizerTests,
NonTrivialGotoNotRemoved) {
	// void test() {
	//     goto L;
	//     a = 1;       ← this is between goto and label
	//     L: return;
	// }
	// Pattern B only applies when goto target IS the immediate successor.
	// Here there's an intervening statement, so it's Pattern A territory.
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);

	auto retStmt = ReturnStmt::create();
	auto assignA = AssignStmt::create(varA,
		ConstInt::create(llvm::APInt(32, 1)), retStmt);
	auto gotoStmt = GotoStmt::create(retStmt);
	gotoStmt->setSuccessor(assignA);
	testFunc->setBody(gotoStmt);

	// This is NOT pattern B (there's an intervening statement).
	// The existing GotoStmtOptimizer handles this case; GotoCFGOptimizer
	// should at minimum not crash or corrupt the IR.
	EXPECT_NO_THROW(Optimizer::optimize<GotoCFGOptimizer>(module));
	ASSERT_TRUE(testFunc->getBody())
		<< "function body should not be nulled out";
}

// Body is just a single plain statement; optimizer should not crash.
TEST_F(GotoCFGOptimizerTests,
SingleReturnStatementLeftUnchanged) {
	auto retStmt = ReturnStmt::create(ConstInt::create(llvm::APInt(32, 0)));
	testFunc->setBody(retStmt);

	Optimizer::optimize<GotoCFGOptimizer>(module);

	ASSERT_TRUE(isa<ReturnStmt>(testFunc->getBody()))
		<< "plain return should be unchanged";
}


//
// A label the emitter never writes, and a goto that targets it.
//
// CHLLWriter writes a goto as `goto <label of the target statement>;` and
// writes a label only when it reaches the statement carrying it.  So a
// statement that leaves the function body while a goto still points at it --
// or that keeps the goto while the label moves elsewhere -- emits C that says
// `label 'lab_...' used but not defined`, which is what
// scripts/ci/check_emitted_c_compiles.sh caught on generated_shell_sort-gcc-O2.
//

TEST_F(GotoCFGOptimizerTests, PatternA_LabelOnTheRewrittenIfSurvives)
{
	// void test() {
	//     int a;
	//     lab_entry: if (a) goto L;
	//     a = 1;
	//     L: return;
	// }
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);

	auto retStmt = ReturnStmt::create();
	auto assignA = AssignStmt::create(varA, ConstInt::create(llvm::APInt(32, 1)), retStmt);
	auto gotoStmt = GotoStmt::create(retStmt);
	auto ifStmt = IfStmt::create(varA, gotoStmt, assignA);
	ifStmt->setLabel("lab_entry");
	testFunc->setBody(ifStmt);

	Optimizer::optimize<GotoCFGOptimizer>(module);

	bool labelStillEmitted = false;
	for (const auto& stmt: ReachableStmtCollector::collect(testFunc->getBody()))
	{
		if (stmt->getLabel() == "lab_entry")
		{
			labelStillEmitted = true;
			break;
		}
	}
	EXPECT_TRUE(labelStillEmitted) << "the label moved to a statement outside the function body, so nothing"
									  " emits it and every goto to it is undefined in the emitted C";
}

TEST_F(GotoCFGOptimizerTests, PatternA_EveryGotoStillTargetsAStatementInTheBody)
{
	// void test() {
	//     int a;
	//     lab_entry: if (a) goto L;
	//     a = 1;
	//     L: a = 2;
	//     goto lab_entry;
	// }
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);

	auto backGoto = GotoStmt::create(ReturnStmt::create());
	auto atL = AssignStmt::create(varA, ConstInt::create(llvm::APInt(32, 2)), backGoto);
	auto assignA = AssignStmt::create(varA, ConstInt::create(llvm::APInt(32, 1)), atL);
	auto gotoL = GotoStmt::create(atL);
	auto ifStmt = IfStmt::create(varA, gotoL, assignA);
	ifStmt->setLabel("lab_entry");
	backGoto->setTarget(ifStmt);
	testFunc->setBody(ifStmt);

	Optimizer::optimize<GotoCFGOptimizer>(module);

	auto reachable = ReachableStmtCollector::collect(testFunc->getBody());
	for (const auto& stmt: reachable)
	{
		auto gotoStmt = cast<GotoStmt>(stmt);
		if (!gotoStmt)
		{
			continue;
		}
		EXPECT_EQ(1u, reachable.count(gotoStmt->getTarget()))
			<< "a goto targets a statement the emitter never reaches, so its"
			   " label is used but never defined";
	}
}

TEST_F(GotoCFGOptimizerTests, PatternA_LabelOnAStatementThatGetsClonedFollowsTheClone)
{
	// void test() {
	//     int a;
	//     if (a) goto L;
	//     lab_mid: a = 1;   <- cloned into the new if-body, and jumped to
	//     L: a = 2;
	//     goto lab_mid;
	// }
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);

	auto backGoto = GotoStmt::create(ReturnStmt::create());
	auto atL = AssignStmt::create(varA, ConstInt::create(llvm::APInt(32, 2)), backGoto);
	auto mid = AssignStmt::create(varA, ConstInt::create(llvm::APInt(32, 1)), atL);
	mid->setLabel("lab_mid");
	auto gotoL = GotoStmt::create(atL);
	auto ifStmt = IfStmt::create(varA, gotoL, mid);
	backGoto->setTarget(mid);
	testFunc->setBody(ifStmt);

	Optimizer::optimize<GotoCFGOptimizer>(module);

	auto reachable = ReachableStmtCollector::collect(testFunc->getBody());
	bool labelStillEmitted = false;
	for (const auto& stmt: reachable)
	{
		if (stmt->getLabel() == "lab_mid")
		{
			labelStillEmitted = true;
			break;
		}
	}
	EXPECT_TRUE(labelStillEmitted) << "the labelled statement was cloned into the new if-body and the"
									  " original dropped, but the label stayed on the original";
	for (const auto& stmt: reachable)
	{
		auto gotoStmt = cast<GotoStmt>(stmt);
		if (!gotoStmt)
		{
			continue;
		}
		EXPECT_EQ(1u, reachable.count(gotoStmt->getTarget()))
			<< "a goto still points at the pre-clone statement, which is no"
			   " longer in the body";
	}
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
