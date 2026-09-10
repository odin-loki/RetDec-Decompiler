/**
 * @file src/llvmir2hll/analysis/loop_bound_jump_analysis.cpp
 * @brief Implementation of LoopBoundJumpAnalysis.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "retdec/llvmir2hll/analysis/loop_bound_jump_analysis.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"

namespace retdec {
namespace llvmir2hll {

/**
 * @brief Constructs a new analysis.
 */
LoopBoundJumpAnalysis::LoopBoundJumpAnalysis(): OrderedAllVisitor(), enclosingLoops(0), foundJump(false) {}

/**
 * @brief Returns @c true if @a stmts holds a @c break or @c continue that binds
 *        to the loop around @a stmts, @c false otherwise.
 *
 * Nested statements and successors are both searched. A @c null @a stmts holds
 * nothing, so the answer is @c false.
 */
bool LoopBoundJumpAnalysis::hasJumpBoundToEnclosingLoop(ShPtr<Statement> stmts)
{
	if (!stmts)
	{
		return false;
	}

	ShPtr<LoopBoundJumpAnalysis> analysis(new LoopBoundJumpAnalysis());
	analysis->visitStmt(stmts);
	return analysis->foundJump;
}

/**
 * @brief Visits @a stmt with the enclosing-construct count raised by one.
 */
template <typename T>
void LoopBoundJumpAnalysis::descendInto(ShPtr<T> stmt)
{
	++enclosingLoops;
	OrderedAllVisitor::visit(stmt);
	--enclosingLoops;
}

void LoopBoundJumpAnalysis::visit(ShPtr<WhileLoopStmt> stmt)
{
	descendInto(stmt);
}

void LoopBoundJumpAnalysis::visit(ShPtr<ForLoopStmt> stmt)
{
	descendInto(stmt);
}

void LoopBoundJumpAnalysis::visit(ShPtr<UForLoopStmt> stmt)
{
	descendInto(stmt);
}

void LoopBoundJumpAnalysis::visit(ShPtr<SwitchStmt> stmt)
{
	descendInto(stmt);
}

void LoopBoundJumpAnalysis::visit(ShPtr<BreakStmt> stmt)
{
	foundJump = foundJump || enclosingLoops == 0;
	OrderedAllVisitor::visit(stmt);
}

void LoopBoundJumpAnalysis::visit(ShPtr<ContinueStmt> stmt)
{
	foundJump = foundJump || enclosingLoops == 0;
	OrderedAllVisitor::visit(stmt);
}

void LoopBoundJumpAnalysis::visit(ShPtr<GotoStmt> stmt)
{
	// Follow the successor but not the target: the target is elsewhere in the
	// function, and a break reached through it is not in this chain.
	// BreakInIfAnalysis declines to follow it for the same reason.
	OrderedAllVisitor::visitStmt(stmt->getSuccessor());
}

} // namespace llvmir2hll
} // namespace retdec
