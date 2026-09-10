/**
 * @file include/retdec/llvmir2hll/analysis/loop_bound_jump_analysis.h
 * @brief Analysis of a break or continue bound to the loop around a statement.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_LLVMIR2HLL_ANALYSIS_LOOP_BOUND_JUMP_ANALYSIS_H
#define RETDEC_LLVMIR2HLL_ANALYSIS_LOOP_BOUND_JUMP_ANALYSIS_H

#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/types.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"
#include "retdec/utils/non_copyable.h"

namespace retdec {
namespace llvmir2hll {

class Statement;

/**
 * @brief Analysis of a @c break or @c continue bound to the loop around a
 *        statement chain.
 *
 * Two of the three passes that call @c splitWhileTrueLoop use the body prefix
 * it returns *outside* the loop as well as inside: WhileTrueToWhileCondOptimizer
 * prepends a clone of it before the loop, and WhileTrueToUForLoopOptimizer's
 * do-while lowering rewrites
 *
 *     while true { BODY; if (exit) break; }
 *
 * as
 *
 *     BODY; while (!exit) { BODY; }
 *
 * with the first BODY at the loop's old position. Both are correct only because
 * a "while true" body always runs at least once -- and both are wrong if that
 * body holds a @c break, because the copy outside the loop then has nothing to
 * break out of. A C compiler says `break statement not within loop or switch`;
 * CC-01 reported it on hash_table across four builds.
 *
 * This answers the question those two passes have to ask first. A @c break
 * nested inside a loop or a @c switch *within* the prefix is a different
 * statement: it binds to that inner construct, which travels with it and still
 * encloses it afterwards. So only the ones at depth zero count, and a loop whose
 * prefix merely contains a nested loop is still optimized.
 *
 * BreakInIfAnalysis is the neighbouring question -- whether one @c IfStmt holds
 * a break at all -- and does not track depth.
 *
 * This class implements the "static helper" (or "library") design pattern (it
 * has just static functions and no instances can be created).
 */
class LoopBoundJumpAnalysis : private OrderedAllVisitor, private retdec::utils::NonCopyable {
public:
	static bool hasJumpBoundToEnclosingLoop(ShPtr<Statement> stmts);

private:
	LoopBoundJumpAnalysis();

	template <typename T>
	void descendInto(ShPtr<T> stmt);

	/// @name Visitor Interface
	/// @{
	using OrderedAllVisitor::visit;
	virtual void visit(ShPtr<WhileLoopStmt> stmt) override;
	virtual void visit(ShPtr<ForLoopStmt> stmt) override;
	virtual void visit(ShPtr<UForLoopStmt> stmt) override;
	virtual void visit(ShPtr<SwitchStmt> stmt) override;
	virtual void visit(ShPtr<BreakStmt> stmt) override;
	virtual void visit(ShPtr<ContinueStmt> stmt) override;
	virtual void visit(ShPtr<GotoStmt> stmt) override;
	/// @}

private:
	/// How many loops or switches enclose the statement being visited.
	std::size_t enclosingLoops;

	/// Whether a break or continue was found outside all of them.
	bool foundJump;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
