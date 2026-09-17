/**
 * @file tests/llvmir2hll/analysis/loop_bound_jump_analysis_tests.cpp
 * @brief Tests for @c LoopBoundJumpAnalysis.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * This analysis decides whether a statement chain may be copied or moved
 * outside the loop that encloses it. Two passes ask it before doing exactly
 * that -- WhileTrueToWhileCondOptimizer prepends a clone of the loop's body
 * prefix before the loop, and WhileTrueToUForLoopOptimizer's do-while lowering
 * puts the prefix at the loop's old position -- so a wrong "no" here becomes a
 * `break statement not within loop or switch` in the emitted C.
 *
 * It had no tests.
 */

#include <gtest/gtest.h>

#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/analysis/loop_bound_jump_analysis.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class LoopBoundJumpAnalysisTests : public TestsWithModule {
protected:
	ShPtr<Variable> x;

	void SetUp() override
	{
		TestsWithModule::SetUp();
		x = Variable::create("x", IntType::create(32));
	}

	static ShPtr<ConstInt> num(int64_t v)
	{
		return ConstInt::create(v, 32);
	}

	ShPtr<AssignStmt> bump(ShPtr<Statement> succ = nullptr)
	{
		return AssignStmt::create(x, num(1), succ);
	}

	/// while (x < 9) { x = 1; }
	ShPtr<WhileLoopStmt> nestedLoop(ShPtr<Statement> succ = nullptr)
	{
		auto loop = WhileLoopStmt::create(LtOpExpr::create(x, num(9)), bump());
		if (succ)
		{
			loop->setSuccessor(succ);
		}
		return loop;
	}
};

TEST_F(LoopBoundJumpAnalysisTests, NullChainHoldsNothing)
{
	EXPECT_FALSE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(nullptr));
}

TEST_F(LoopBoundJumpAnalysisTests, AChainWithNoJumpAtAll)
{
	EXPECT_FALSE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(bump()));
}

TEST_F(LoopBoundJumpAnalysisTests, ABareBreakBindsToTheEnclosingLoop)
{
	EXPECT_TRUE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(BreakStmt::create()));
}

TEST_F(LoopBoundJumpAnalysisTests, ABareContinueBindsToTheEnclosingLoop)
{
	EXPECT_TRUE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(ContinueStmt::create()));
}

TEST_F(LoopBoundJumpAnalysisTests, ABreakInsideAnIfStillBindsToTheEnclosingLoop)
{
	// An `if` is not a jump target, so this is the same as a bare break -- and
	// it is the shape that actually occurs.
	EXPECT_TRUE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(
		IfStmt::create(LtOpExpr::create(x, num(3)), BreakStmt::create())));
}

TEST_F(LoopBoundJumpAnalysisTests, ABreakInsideANestedLoopDoesNot)
{
	// It binds to the nested loop, which travels with it.
	auto inner = WhileLoopStmt::create(LtOpExpr::create(x, num(9)), BreakStmt::create());
	EXPECT_FALSE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(inner));
}

TEST_F(LoopBoundJumpAnalysisTests, ABreakAFTERANestedLoopStillBinds)
{
	// The defect this test exists for. descendInto used to call
	// OrderedAllVisitor::visit, which walks the statement's SUCCESSOR as well
	// as its body, with the enclosing count still raised. So everything after
	// a nested loop at the same level read as being inside it, and this
	// answered "no jump" -- the answer that lets a caller hoist the break out
	// of the loop it belongs to.
	auto chain = nestedLoop(IfStmt::create(LtOpExpr::create(x, num(3)), BreakStmt::create()));
	EXPECT_TRUE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(chain))
		<< "a break after a nested loop binds to the OUTER loop";
}

TEST_F(LoopBoundJumpAnalysisTests, AContinueAFTERANestedLoopStillBinds)
{
	auto chain = nestedLoop(IfStmt::create(LtOpExpr::create(x, num(3)), ContinueStmt::create()));
	EXPECT_TRUE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(chain));
}

TEST_F(LoopBoundJumpAnalysisTests, ABreakInsideASwitchBindsToTheSwitch)
{
	auto sw = SwitchStmt::create(x);
	sw->addClause(num(1), BreakStmt::create());
	EXPECT_FALSE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(sw)) << "C captures break in a switch";
}

TEST_F(LoopBoundJumpAnalysisTests, AContinueInsideASwitchBindsToTheLoop)
{
	// The second defect. A switch was counted as enclosing a `continue` as
	// well as a `break`. C does not capture continue in a switch: a continue
	// inside a switch inside a loop binds to the loop, so a prefix holding one
	// must not be moved out of that loop.
	auto sw = SwitchStmt::create(x);
	sw->addClause(num(1), ContinueStmt::create());
	EXPECT_TRUE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(sw)) << "C does not capture continue in a switch";
}

TEST_F(LoopBoundJumpAnalysisTests, ABreakAfterASwitchStillBinds)
{
	auto sw = SwitchStmt::create(x);
	sw->addClause(num(1), bump());
	sw->setSuccessor(BreakStmt::create());
	EXPECT_TRUE(LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(sw));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
