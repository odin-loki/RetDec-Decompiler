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
LoopBoundJumpAnalysis::LoopBoundJumpAnalysis():
	OrderedAllVisitor(), enclosingLoops(0), enclosingBreakTargets(0), foundJump(false)
{}

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
 * @brief Visits @a stmt's body with the enclosing counts raised, then its
 *        successor with them restored.
 *
 * This used to be @c OrderedAllVisitor::visit(stmt) between an increment and a
 * decrement, and that visits the statement's SUCCESSOR as well as its body. So
 * everything after a nested loop, at the same level, was counted as still
 * inside it:
 *
 *     while true {
 *         while (x) { ... }    // the counter goes up here
 *         if (c) break;        // and was still up here, so this was missed
 *         ...
 *     }
 *
 * The @c break binds to the OUTER loop and the analysis answered "no jump
 * bound to the enclosing loop", which is exactly the answer that lets the
 * do-while lowering hoist it out of the loop it belongs to.
 */
template <typename T>
void LoopBoundJumpAnalysis::descendInto(ShPtr<T> stmt, bool isLoop)
{
	if (isLoop)
	{
		++enclosingLoops;
	}
	++enclosingBreakTargets;
	if (visitNestedStmts)
	{
		visitStmt(stmt->getBody());
	}
	--enclosingBreakTargets;
	if (isLoop)
	{
		--enclosingLoops;
	}

	if (visitSuccessors && stmt->hasSuccessor())
	{
		visitStmt(stmt->getSuccessor());
	}
}

void LoopBoundJumpAnalysis::visit(ShPtr<WhileLoopStmt> stmt)
{
	descendInto(stmt, true);
}

void LoopBoundJumpAnalysis::visit(ShPtr<ForLoopStmt> stmt)
{
	descendInto(stmt, true);
}

void LoopBoundJumpAnalysis::visit(ShPtr<UForLoopStmt> stmt)
{
	descendInto(stmt, true);
}

void LoopBoundJumpAnalysis::visit(ShPtr<SwitchStmt> stmt)
{
	// A switch is a break target and is NOT a continue target: `continue`
	// inside a switch inside a loop binds to the loop. It used to be counted
	// as both, so such a continue was reported as bound to the switch and the
	// prefix holding it was hoisted out of its loop.
	//
	// SwitchStmt has clauses rather than one body, so this does not go through
	// descendInto.
	++enclosingBreakTargets;
	if (visitNestedStmts)
	{
		for (auto i = stmt->clause_begin(), e = stmt->clause_end(); i != e; ++i)
		{
			visitStmt(i->second);
		}
	}
	--enclosingBreakTargets;

	if (visitSuccessors && stmt->hasSuccessor())
	{
		visitStmt(stmt->getSuccessor());
	}
}

void LoopBoundJumpAnalysis::visit(ShPtr<BreakStmt> stmt)
{
	foundJump = foundJump || enclosingBreakTargets == 0;
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
