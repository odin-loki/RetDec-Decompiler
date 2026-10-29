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
#include "retdec/llvmir2hll/utils/ir.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/support/expression_negater.h"

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

/// A body of only empty statements that nothing jumps to.
bool isEmptyBody(ShPtr<Statement> stmt) {
	for (; stmt; stmt = stmt->getSuccessor()) {
		if (!isa<EmptyStmt>(stmt) || stmt->hasLabel() || stmt->isGotoTarget()) {
			return false;
		}
	}
	return true;
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

/// The statements from @a target to a return, if they are a short run of
/// plain statements; otherwise empty.
std::vector<ShPtr<Statement>> shortReturningTail(ShPtr<Statement> target) {
	std::vector<ShPtr<Statement>> tail;
	std::size_t plain = 0; // statements other than empty ones
	for (auto cur = target; cur; cur = cur->getSuccessor()) {
		if (plain >= 5 || (cur != target && cur->hasLabel())) {
			break;
		}
		// A goto straight to a return ends the tail as that return.
		if (auto g = cast<GotoStmt>(cur)) {
			if (isa<ReturnStmt>(g->getTarget())) {
				tail.push_back(g->getTarget());
				return tail;
			}
			break;
		}
		tail.push_back(cur);
		if (!isa<EmptyStmt>(cur)) {
			++plain;
		}
		if (isa<ReturnStmt>(cur)) {
			return tail;
		}
		if (!(isa<AssignStmt>(cur) || isa<CallStmt>(cur) ||
				isa<EmptyStmt>(cur))) {
			break;
		}
	}
	return {};
}

/// Drops gotos that are no longer in the function from @a target's
/// predecessors, so it stops counting as a goto target.
void forgetDeadGotos(const ShPtr<Statement> &target,
		const std::set<ShPtr<GotoStmt>> &live) {
	std::vector<ShPtr<Statement>> dead;
	for (auto i = target->predecessor_begin(); i != target->predecessor_end(); ++i) {
		if (auto g = cast<GotoStmt>(*i)) {
			if (!live.count(g)) {
				dead.push_back(g);
			}
		}
	}
	for (auto &g : dead) {
		target->removePredecessor(g);
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
/**
* @brief Replaces a goto to a short run of plain statements that ends in a
*        return with a copy of that run.
*
* @code
* goto L;          f(x);
* ...        ==>   return 0;
* L: f(x);         ...
* return 0;        L: f(x);
*                  return 0;
* @endcode
*/
bool GotoStmtOptimizer::tryCopyReturningTail(ShPtr<GotoStmt> stmt) {
	auto target = stmt->getTarget();
	auto tail = target ? shortReturningTail(target) : std::vector<ShPtr<Statement>>{};
	if (tail.empty()) {
		return false;
	}
	ShPtr<Statement> first;
	ShPtr<Statement> prev;
	for (const auto &t : tail) {
		auto c = ucast<Statement>(t->clone());
		c->setMetadata("");
		c->removeLabel();
		if (prev) {
			prev->setSuccessor(c);
		} else {
			first = c;
		}
		prev = c;
	}
	Statement::replaceStatement(stmt, first);

	std::set<ShPtr<GotoStmt>> live;
	std::set<ShPtr<Statement>> seen;
	collectGotos(currFunc->getBody(), live, seen);
	bool stillTargeted = false;
	for (const auto &g : live) {
		stillTargeted = stillTargeted || g->getTarget() == target;
	}
	if (!stillTargeted) {
		forgetDeadGotos(target, live);
		target->removeLabel();
	}
	return true;
}

/// `if (c) { ...; goto L; } L: ...`: a goto in tail position of if
/// statements whose next statement is its target falls through anyway.
bool GotoStmtOptimizer::tryDropFallThroughGoto(ShPtr<GotoStmt> stmt) {
	auto target = stmt->getTarget();
	if (!target) {
		return false;
	}
	ShPtr<Statement> s = stmt;
	while (!s->getSuccessor()) {
		auto p = s->getParent();
		if (!p || !isa<IfStmt>(p)) {
			return false;
		}
		s = p;
	}
	if (s == ShPtr<Statement>(stmt)) {
		return false; // A plain `goto L; L:` is someone else's job.
	}
	auto next = s->getSuccessor();
	while (next && next != target && isa<EmptyStmt>(next) &&
			!next->hasLabel() && !next->isGotoTarget()) {
		next = next->getSuccessor();
	}
	if (next != target) {
		return false;
	}
	// An empty statement in its place: the goto may be all a branch holds.
	target->removeObserver(stmt);
	target->removePredecessor(stmt);
	Statement::replaceStatement(stmt, EmptyStmt::create());
	return true;
}

/// A goto from inside nested loops (or switches) to the statement after one
/// of the outer ones: a flag carries the break out level by level.
///
///   while (a) { while (b) { goto L; } S; } L:
///
///   ==>
///
///   brk = false;
///   while (a) { while (b) { brk = true; break; } if (brk) break; S; }
bool GotoStmtOptimizer::tryBreakOutOfNestedLoops(ShPtr<GotoStmt> stmt) {
	auto target = stmt->getTarget();
	if (!target) {
		return false;
	}

	std::vector<ShPtr<Statement>> scopes;
	for (auto p = stmt->getParent(); p; p = p->getParent()) {
		if (isLoopOrSwitch(p)) {
			scopes.push_back(p);
		}
	}
	std::size_t k = 1;
	for (; k < scopes.size(); ++k) {
		auto next = scopes[k]->getSuccessor();
		while (next && next != target && isa<EmptyStmt>(next) &&
				!next->hasLabel() && !next->isGotoTarget()) {
			next = next->getSuccessor();
		}
		if (next == target) {
			break;
		}
	}
	if (k >= scopes.size()) {
		return false;
	}

	auto flag = Variable::create("brk" + std::to_string(++flagCount),
		IntType::create(1));
	addLocalVarToFunc(flag, currFunc);

	scopes[k]->prependStatement(AssignStmt::create(flag, ConstBool::create(false)));
	for (std::size_t i = 0; i < k; ++i) {
		scopes[i]->appendStatement(
			IfStmt::create(flag, BreakStmt::create()));
	}
	Statement::replaceStatement(stmt,
		AssignStmt::create(flag, ConstBool::create(true), BreakStmt::create()));
	return true;
}

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
	forgetDeadGotos(target, {});
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
* @brief Optimizes @a func, then, on the last run, drops labels that no goto
*        in the function names any more.
*
* Earlier passes remove gotos without telling their targets, and the C writer
* emits a label for every statement that still counts as a goto target.
*/
void GotoStmtOptimizer::runOnFunction(ShPtr<Function> func) {
	FuncOptimizer::runOnFunction(func);
	if (!breakOutOfLoops || !func->getBody()) {
		return;
	}

	std::set<ShPtr<GotoStmt>> live;
	std::set<ShPtr<Statement>> all;
	collectGotos(func->getBody(), live, all);
	std::set<ShPtr<Statement>> targeted;
	for (const auto &g : live) {
		targeted.insert(g->getTarget());
	}
	for (const auto &stmt : all) {
		auto ifStmt = cast<IfStmt>(stmt);
		if (ifStmt && !ifStmt->hasElseIfClauses() && ifStmt->hasElseClause() &&
				isEmptyBody(ifStmt->getFirstIfBody())) {
			// `if (c) {} else { S }` ==> `if (!c) { S }`; a dropped goto can
			// leave the first.
			ifStmt->setFirstIfCond(
				ExpressionNegater::negate(ifStmt->getFirstIfCond()));
			ifStmt->setFirstIfBody(ifStmt->getElseClause());
			ifStmt->removeElseClause();
		}
	}
	for (const auto &stmt : all) {
		if (!targeted.count(stmt) && (stmt->isGotoTarget() || stmt->hasLabel())) {
			forgetDeadGotos(stmt, live);
			stmt->removeLabel();
		}
	}
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

	if (breakOutOfLoops && !tryDropFallThroughGoto(stmt) &&
			!tryBreakOutOfLoop(stmt) && !tryBreakOutOfNestedLoops(stmt)) {
		tryCopyReturningTail(stmt);
	}
}

} // namespace llvmir2hll
} // namespace retdec
