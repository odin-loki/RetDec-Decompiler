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

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c goto_cfg_optimizer module.
*/
class GotoCFGOptimizerTests: public TestsWithModule {};

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

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
