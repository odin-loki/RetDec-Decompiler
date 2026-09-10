/**
* @file tests/llvmir2hll/ir/statement_tests.cpp
* @brief Tests for the @c statement module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "llvmir2hll/ir/tests_with_module.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c statement module.
*/
class StatementTests: public TestsWithModule {};

//
// setSuccessor()
//

TEST_F(StatementTests, SettingANewSuccessorDropsTheStatementFromTheOldSuccessorsPredecessors)
{
	auto a = EmptyStmt::create();
	auto b = EmptyStmt::create();
	auto c = EmptyStmt::create();

	a->setSuccessor(b);
	a->setSuccessor(c);

	ASSERT_EQ(c, a->getSuccessor());
	ASSERT_FALSE(b->hasPredecessors()) << "nothing reaches b any more, but a is still recorded as reaching it";
}

TEST_F(StatementTests, AStatementThatNoLongerReachesAnotherIsNotItsUniquePredecessor)
{
	// getUniquePredecessor() is what pre_while_true_loop_conv_optimizer and
	// copy_propagation_optimizer walk backwards through, so a stale entry is
	// not merely an extra name in a set -- it is the wrong statement, and they
	// rewrite around it.
	auto a = EmptyStmt::create();
	auto b = EmptyStmt::create();
	auto c = EmptyStmt::create();

	a->setSuccessor(b);
	a->setSuccessor(c);

	ASSERT_EQ(ShPtr<Statement>(), b->getUniquePredecessor());
}

TEST_F(StatementTests, RemovingTheSuccessorDropsTheStatementFromItsPredecessors)
{
	auto a = EmptyStmt::create();
	auto b = EmptyStmt::create();

	a->setSuccessor(b);
	a->removeSuccessor();

	ASSERT_FALSE(b->hasPredecessors());
}

//
// hasLabel()
//

TEST_F(StatementTests,
HasLabelReturnsFalseWhenStatementDoesNotHaveLabelSet) {
	auto stmt = EmptyStmt::create();

	ASSERT_FALSE(stmt->hasLabel());
}

TEST_F(StatementTests,
HasLabelReturnsTrueWhenStatementHasLabelSet) {
	auto stmt = EmptyStmt::create();
	stmt->setLabel("my_label");

	ASSERT_TRUE(stmt->hasLabel());
}

//
// removeLabel()
//

TEST_F(StatementTests,
RemoveLabelWorksCorrectlyWhenNoLabelWasAssigned) {
	auto stmt = EmptyStmt::create();
	stmt->removeLabel();
	ASSERT_FALSE(stmt->hasLabel());
}

TEST_F(StatementTests,
RemoveLabelWorksCorrectlyWhenLabelWasAssigned) {
	auto stmt = EmptyStmt::create();
	stmt->setLabel("my_label");
	stmt->removeLabel();
	ASSERT_FALSE(stmt->hasLabel());
}

//
// transferLabelFrom()
//

TEST_F(StatementTests,
TransferLabelFromTransfersLabelFromOtherStatementWhenItHasLabel) {
	auto stmt1 = EmptyStmt::create();
	auto stmt2 = EmptyStmt::create();
	stmt2->setLabel("my_label");

	stmt1->transferLabelFrom(stmt2);

	ASSERT_FALSE(stmt2->hasLabel());
	ASSERT_EQ("my_label", stmt1->getLabel());
}

TEST_F(StatementTests,
TransferLabelFromDoesNothingWhenStatementHasNoLabel) {
	auto stmt1 = EmptyStmt::create();
	auto stmt2 = EmptyStmt::create();

	stmt1->transferLabelFrom(stmt2);

	ASSERT_FALSE(stmt1->hasLabel());
	ASSERT_FALSE(stmt2->hasLabel());
}

TEST_F(StatementTests, TransferLabelFromKeepsOwnLabelWhenThereIsNothingToTransfer)
{
	// "Does nothing" was only ever tested with both statements unlabelled,
	// which cannot tell a no-op from an assignment of the empty string.
	auto stmt1 = EmptyStmt::create();
	stmt1->setLabel("my_label");
	auto stmt2 = EmptyStmt::create();

	stmt1->transferLabelFrom(stmt2);

	ASSERT_EQ("my_label", stmt1->getLabel());
}

//
// transferLabelTo()
//

TEST_F(StatementTests,
TransferLabelToTransfersLabelToOtherStatementWhenStatementHasLabel) {
	auto stmt1 = EmptyStmt::create();
	stmt1->setLabel("my_label");
	auto stmt2 = EmptyStmt::create();

	stmt1->transferLabelTo(stmt2);

	ASSERT_FALSE(stmt1->hasLabel());
	ASSERT_EQ("my_label", stmt2->getLabel());
}

TEST_F(StatementTests,
TransferLabelToDoesNothingWhenStatementHasNoLabel) {
	auto stmt1 = EmptyStmt::create();
	auto stmt2 = EmptyStmt::create();

	stmt1->transferLabelTo(stmt2);

	ASSERT_FALSE(stmt1->hasLabel());
	ASSERT_FALSE(stmt2->hasLabel());
}

TEST_F(StatementTests, TransferLabelToKeepsTheDestinationLabelWhenThereIsNothingToTransfer)
{
	auto stmt1 = EmptyStmt::create();
	auto stmt2 = EmptyStmt::create();
	stmt2->setLabel("my_label");

	stmt1->transferLabelTo(stmt2);

	ASSERT_EQ("my_label", stmt2->getLabel()) << "an unlabelled statement took away the label the destination had,"
												" so every goto aimed at the destination lost its definition";
}

//
// redirectGotosTo()
//

TEST_F(StatementTests,
RedirectGotosToDoesNothingWhenStatementIsNotGotoTarget) {
	auto stmt1 = EmptyStmt::create();
	auto stmt2 = EmptyStmt::create();

	stmt1->redirectGotosTo(stmt2);

	ASSERT_FALSE(stmt1->isGotoTarget());
	ASSERT_FALSE(stmt2->isGotoTarget());
}

TEST_F(StatementTests,
RedirectGotosToRedirectsGotos) {
	auto origTarget = EmptyStmt::create();
	auto newTarget = EmptyStmt::create();
	auto gotoStmt = GotoStmt::create(origTarget);

	origTarget->redirectGotosTo(newTarget);

	ASSERT_TRUE(newTarget->isGotoTarget());
	ASSERT_TRUE(newTarget->hasPredecessors());
	ASSERT_FALSE(origTarget->isGotoTarget());
	ASSERT_FALSE(origTarget->hasPredecessors());
}

TEST_F(StatementTests, RedirectGotosToKeepsTheNewTargetsLabelWhenTheOldTargetHasNone)
{
	// The whole point of redirecting is that the gotos keep resolving; they
	// resolve through the new target's label, so wiping it strands them.
	auto origTarget = EmptyStmt::create();
	auto newTarget = EmptyStmt::create();
	newTarget->setLabel("my_label");
	auto gotoStmt = GotoStmt::create(origTarget);

	origTarget->redirectGotosTo(newTarget);

	ASSERT_TRUE(newTarget->isGotoTarget());
	ASSERT_EQ("my_label", newTarget->getLabel());
}

TEST_F(StatementTests,
RedirectGotosToTransfersLabels) {
	auto origTarget = EmptyStmt::create();
	origTarget->setLabel("my_label");
	auto newTarget = EmptyStmt::create();
	auto gotoStmt = GotoStmt::create(origTarget);

	origTarget->redirectGotosTo(newTarget);

	ASSERT_EQ("my_label", newTarget->getLabel());
	ASSERT_FALSE(origTarget->hasLabel());
}

//
// removeStatement()
//

TEST_F(StatementTests, RemovingAGotoSendsTheJumpsIntoItToItsTarget)
{
	// X: goto L;
	//    a;
	// L: b;
	//
	// A jump to X means L. Sending it to `a` -- the statement the goto
	// existed to skip -- is a different program.
	auto atL = EmptyStmt::create();
	auto a = EmptyStmt::create(atL);
	auto removed = GotoStmt::create(atL);
	removed->setSuccessor(a);
	auto jumpIn = GotoStmt::create(removed);

	Statement::removeStatement(removed);

	ASSERT_EQ(atL, jumpIn->getTarget()) << "the jump landed on the statement the removed goto skipped";
}

TEST_F(StatementTests, RemovingAGotoPutsItsLabelWhereItsJumpsNowGo)
{
	// The label and the gotos that resolve through it have to end up on the
	// same statement, or the emitter writes one of them twice and the other
	// not at all.
	auto atL = EmptyStmt::create();
	auto a = EmptyStmt::create(atL);
	auto removed = GotoStmt::create(atL);
	removed->setLabel("lab_x");
	removed->setSuccessor(a);
	auto jumpIn = GotoStmt::create(removed);

	Statement::removeStatement(removed);

	ASSERT_EQ(atL, jumpIn->getTarget());
	ASSERT_EQ("lab_x", atL->getLabel());
	ASSERT_FALSE(a->hasLabel()) << "the label was copied onto the successor as well, so two reachable"
								   " statements carry it";
}

TEST_F(StatementTests, RemovingAGotoWithNoSuccessorStillSendsTheJumpsToItsTarget)
{
	// A goto is the last statement of its block, which is the ordinary case.
	// The placeholder EmptyStmt that a targeted, successorless statement is
	// replaced by has nowhere to fall through to; the goto's target does.
	auto atL = EmptyStmt::create();
	auto removed = GotoStmt::create(atL);
	auto jumpIn = GotoStmt::create(removed);

	Statement::removeStatement(removed);

	ASSERT_EQ(atL, jumpIn->getTarget());
}

TEST_F(StatementTests, RemovingAnOrdinaryStatementStillSendsTheJumpsIntoItToItsSuccessor)
{
	// The other half of the rule, so the goto case cannot be widened by
	// accident: an ordinary statement does fall through.
	auto succ = EmptyStmt::create();
	auto removed = EmptyStmt::create(succ);
	auto jumpIn = GotoStmt::create(removed);

	Statement::removeStatement(removed);

	ASSERT_EQ(succ, jumpIn->getTarget());
}

//
// removeLastStatement()
//

TEST_F(StatementTests,
RemoveLastStatementWorksCorrectlyWhenStmtsIsJustSingleStatement) {
	testFunc->setBody(BreakStmt::create());

	Statement::removeLastStatement(testFunc->getBody());

	EXPECT_TRUE(isa<EmptyStmt>(testFunc->getBody()));
}

TEST_F(StatementTests,
RemoveLastStatementWorksCorrectlyWhenStmtsHasMoreThanOneStatement) {
	ShPtr<Statement> stmt1(BreakStmt::create());
	ShPtr<Statement> stmt2(EmptyStmt::create(stmt1));
	ShPtr<Statement> stmt3(EmptyStmt::create(stmt2));

	Statement::removeLastStatement(stmt1);

	EXPECT_FALSE(stmt2->hasSuccessor());
}

#if DEATH_TESTS_ENABLED
TEST_F(StatementTests,
RemoveLastStatementViolatedPreconditionNullStmts) {
	EXPECT_DEATH(Statement::removeLastStatement(ShPtr<Statement>()),
		".*removeLastStatement.*Precondition.*failed.*");
}
#endif

//
// isStatementInStatements()
//

TEST_F(StatementTests,
IsStatementInStatementsReturnsTrueWhenStatementIsInBlockOfStatements) {
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<ReturnStmt> returnStmt(ReturnStmt::create());
	ShPtr<EmptyStmt> emptyStmt(EmptyStmt::create());
	breakStmt->setSuccessor(returnStmt);
	returnStmt->setSuccessor(emptyStmt);

	EXPECT_TRUE(Statement::isStatementInStatements(returnStmt, breakStmt));
}

TEST_F(StatementTests,
IsStatementInStatementsReturnsFalseWhenStatementIsNotInBlockOfStatements) {
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<ReturnStmt> returnStmt(ReturnStmt::create());
	ShPtr<EmptyStmt> emptyStmt(EmptyStmt::create());
	ShPtr<ContinueStmt> continueStmt(ContinueStmt::create());
	breakStmt->setSuccessor(returnStmt);
	returnStmt->setSuccessor(emptyStmt);

	EXPECT_FALSE(Statement::isStatementInStatements(continueStmt, breakStmt));
}

TEST_F(StatementTests,
IsStatementInStatementsReturnsFalseWhenStatementIsInNestedBlock) {
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<ReturnStmt> returnStmt(ReturnStmt::create());
	ShPtr<EmptyStmt> emptyStmt(EmptyStmt::create());
	ShPtr<IfStmt> ifStmt(IfStmt::create(ConstBool::create(true), emptyStmt));
	breakStmt->setSuccessor(returnStmt);
	returnStmt->setSuccessor(ifStmt);

	EXPECT_FALSE(Statement::isStatementInStatements(emptyStmt, breakStmt));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
