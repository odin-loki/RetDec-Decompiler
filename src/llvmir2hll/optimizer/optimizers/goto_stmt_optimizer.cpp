/**
* @file src/llvmir2hll/optimizer/optimizers/goto_stmt_optimizer.cpp
* @brief Replace goto statements when possible.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <set>
#include <sstream>

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/not_op_expr.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/optimizer/optimizers/goto_stmt_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

namespace {

bool isLoopOrSwitch(const ShPtr<Statement> &stmt) {
	return isa<WhileLoopStmt>(stmt) || isa<ForLoopStmt>(stmt) ||
		isa<UForLoopStmt>(stmt) || isa<SwitchStmt>(stmt);
}

/// The innermost loop or switch around @a stmt, or null.
ShPtr<Statement> innermostBreakScope(ShPtr<Statement> stmt) {
	for (auto p = stmt->getParent(); p; p = p->getParent()) {
		if (isLoopOrSwitch(p)) {
			return p;
		}
	}
	return nullptr;
}

/// A statement that can move into a loop without changing what any break,
/// continue or goto inside it refers to.
bool isMovable(const ShPtr<Statement> &stmt) {
	return !stmt->hasLabel() && !stmt->isGotoTarget() &&
		(isa<AssignStmt>(stmt) || isa<VarDefStmt>(stmt) ||
		 isa<CallStmt>(stmt) || isa<EmptyStmt>(stmt));
}

/// Adds every goto in the statement chain starting at @a stmt to @a out.
void collectGotos(ShPtr<Statement> stmt, std::set<ShPtr<GotoStmt>> &out,
		std::set<ShPtr<Statement>> &seen) {
	for (; stmt && seen.insert(stmt).second; stmt = stmt->getSuccessor()) {
		if (auto g = cast<GotoStmt>(stmt)) {
			out.insert(g);
		} else if (auto ifStmt = cast<IfStmt>(stmt)) {
			for (auto i = ifStmt->clause_begin(); i != ifStmt->clause_end(); ++i) {
				collectGotos(i->second, out, seen);
			}
			if (ifStmt->hasElseClause()) {
				collectGotos(ifStmt->getElseClause(), out, seen);
			}
		} else if (auto sw = cast<SwitchStmt>(stmt)) {
			for (auto i = sw->clause_begin(); i != sw->clause_end(); ++i) {
				collectGotos(i->second, out, seen);
			}
		} else if (auto w = cast<WhileLoopStmt>(stmt)) {
			collectGotos(w->getBody(), out, seen);
		} else if (auto f = cast<ForLoopStmt>(stmt)) {
			collectGotos(f->getBody(), out, seen);
		} else if (auto u = cast<UForLoopStmt>(stmt)) {
			collectGotos(u->getBody(), out, seen);
		}
	}
}

} // anonymous namespace

/**
* @brief Turns gotos that leave a while loop to just past its exit code into
*        breaks.
*
* @code
* while (c) {        while (true) {
*     ...                if (!c) { S; break; }
*     goto L;    ==>     ...
* }                      break;
* S;                 }
* L: T               T
* @endcode
*
* Only when every goto to L is directly in that loop (not in a nested loop or
* switch, which a break would leave instead), and S is plain statements.
*
* @return @c true if @a stmt was replaced.
*/
bool GotoStmtOptimizer::tryBreakOutOfLoop(ShPtr<GotoStmt> stmt) {
	auto target = stmt->getTarget();
	auto loop = cast<WhileLoopStmt>(innermostBreakScope(stmt));
	if (!loop || !target) {
		return false;
	}

	// S: the plain statements between the loop and the target.
	std::vector<ShPtr<Statement>> exitCode;
	auto cur = loop->getSuccessor();
	while (cur && cur != target) {
		if (!isMovable(cur) || exitCode.size() > 8) {
			return false;
		}
		exitCode.push_back(cur);
		cur = cur->getSuccessor();
	}
	if (cur != target) {
		return false;
	}

	// Every goto to the target must be one this rewrite turns into a break.
	// Predecessor sets can keep gotos that earlier passes removed, so look at
	// the gotos still in the function.
	std::set<ShPtr<GotoStmt>> live;
	std::set<ShPtr<Statement>> seen;
	collectGotos(currFunc->getBody(), live, seen);
	std::vector<ShPtr<GotoStmt>> gotos;
	for (const auto &g : live) {
		if (g->getTarget() != target) {
			continue;
		}
		if (innermostBreakScope(g) != loop) {
			return false;
		}
		gotos.push_back(g);
	}
	if (gotos.empty()) {
		return false;
	}

	if (!exitCode.empty()) {
		// Move S into the loop as its normal exit.
		auto first = exitCode.front();
		auto last = exitCode.back();
		loop->setSuccessor(target);
		last->setSuccessor(BreakStmt::create());
		auto exitIf = IfStmt::create(NotOpExpr::create(loop->getCondition()),
			first, loop->getBody());
		loop->setBody(exitIf);
		loop->setCondition(ConstBool::create(true));
	}

	for (auto &g : gotos) {
		Statement::replaceStatement(g, BreakStmt::create(g->getAddress()));
	}
	// Every live goto to the target is gone now.
	target->removeLabel();
	return true;
}

/**
* @brief Constructs a new optimizer.
*
* @param[in] module Module to be optimized.
*
* @par Preconditions
*  - @a module is non-null
*/
GotoStmtOptimizer::GotoStmtOptimizer(ShPtr<Module> module, bool breakOutOfLoops):
	FuncOptimizer(module), breakOutOfLoops(breakOutOfLoops) {
		PRECONDITION_NON_NULL(module);
	}

/**
* @brief Optimize goto statement.
*/
void GotoStmtOptimizer::visit(ShPtr<GotoStmt> stmt) {
	auto target = stmt->getTarget();

	// goto label
	// ...
	// label:
	//   goto/return/break/continue
	//
	// ==>
	//
	// cloned goto/return/break/continue
	// ...
	// label: (possibly remove if no goto label left)
	//   goto/return/break/continue
	//
	if (isa<GotoStmt>(target)
			|| isa<ReturnStmt>(target)
			|| isa<BreakStmt>(target)
			|| isa<ContinueStmt>(target)) {
		auto c = ucast<Statement>(target->clone());
		c->setMetadata("");

		stmt->prependStatement(c);
		Statement::removeStatement(stmt);

		if (!target->isGotoTarget()) {
			target->removeLabel();
		}

		return;
	}

	if (breakOutOfLoops) {
		tryBreakOutOfLoop(stmt);
	}
}

} // namespace llvmir2hll
} // namespace retdec
