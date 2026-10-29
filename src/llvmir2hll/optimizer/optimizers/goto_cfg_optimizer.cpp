/**
 * @file src/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.cpp
 * @brief Structural elimination of spurious goto statements.
 * @copyright (c) 2024, MIT license
 *
 * Implements patterns A–D described in the header, plus E (a trailing forward
 * goto moves the skipped run into the other clause) and F (a backward sibling
 * goto becomes a while loop). Runs iteratively on each
 * function until no further rewrites are possible.
 */

#include "retdec/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.h"

#include <set>

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/support/expression_negater.h"

namespace retdec {
namespace llvmir2hll {

namespace {

//===========================================================================
// Utilities
//===========================================================================



//===========================================================================
// Whole-function pass: collapse  "if (c) goto L"  into  "if (!c) { body }".
//
// A 213-line `FuncRewriter` class used to sit above this, described here as
// the thing this traversal replaced. It was never constructed, and it had not
// been through a reviewer either: two of its loop advances were `stmt = stmt;`,
// one of its inserts was `newIf->prependStatement(newIf)` -- a statement
// inserted before itself -- with the line after it saying "Actually: we need
// to insert newIf where ifStmt was", and it carried two members for the same
// function, `func` and `func_`. It has been removed, along with the
// `endsWithJump` helper written for it, which nothing called either.
//===========================================================================

/// Collect the statements between @a start (inclusive) and @a end (exclusive)
/// as a vector.  Returns empty vector if end is not reachable.
std::vector<ShPtr<Statement>> collectBetween(ShPtr<Statement> start, ShPtr<Statement> end, int maxSteps = 256)
{
	std::vector<ShPtr<Statement>> result;
	auto s = start;
	for (int i = 0; i < maxSteps && s && s != end; ++i, s = s->getSuccessor())
		result.push_back(s);
	if (s != end) return {}; // end not found
	return result;
}

/// Move the run @a between -- which must be exactly the statements from
/// @c is->getSuccessor() up to but excluding @a target -- out of the main
/// successor chain and into the first if-clause body of @a is, whose
/// successor becomes @a target.
///
/// The run is *moved*, not copied.  Copying it is what this did, and it lost
/// every label nested inside a compound statement: IfStmt::clone(),
/// WhileLoopStmt::clone() and ForLoopStmt::clone() deep-copy their bodies
/// through Statement::cloneStatements(), and no clone() carries a label.
/// Redirecting the gotos aimed at the run's top-level statements -- which is
/// all a walk over @a between can reach -- is not enough, because the
/// statements nested inside those bodies are dropped along with their
/// containers, and a goto elsewhere in the function still names the label one
/// of them carried.  That is `goto lab_0x112c;` with no `lab_0x112c:`
/// anywhere, which is what CC-01 measured on generated_shell_sort-gcc-O2.
///
/// Moving has nothing to fix up: every statement in the run keeps its
/// identity, so its label, the gotos aimed at it, and everything nested
/// inside it come along untouched.
void moveRunIntoIfBody(
	const ShPtr<IfStmt>& is, const std::vector<ShPtr<Statement>>& between, const ShPtr<Statement>& target)
{
	// The body being replaced is `[empty...] goto target`, so reaching any of
	// it meant reaching `target`; anything aimed at it is aimed at `target`.
	// Without this the same symptom appears one statement over.
	for (auto s = is->getFirstIfBody(); s; s = s->getSuccessor())
	{
		s->redirectGotosTo(target);
	}

	// setFirstIfBody() first, while `is` is still the run's predecessor in the
	// main chain: it prunes the predecessors that reach the new body by
	// falling through, which is exactly the edge from `is` that the move
	// replaces, and a clause body is not supposed to keep one.
	auto last = between.back();
	is->setFirstIfBody(between.front());
	is->setSuccessor(target);
	last->setSuccessor(nullptr);
}


/// Whether the chain starting at @a stmt contains a break or continue that
/// would bind to a loop or switch enclosing the chain (rather than one nested
/// inside it).  @a inSwitch is set while scanning a switch nested in the
/// chain: a break there binds to that switch, a continue still escapes.
bool hasEscapingBreakOrContinue(ShPtr<Statement> stmt, bool inSwitch = false,
		bool single = false)
{
	for (int i = 0; stmt && i < 4096; ++i, stmt = single ? nullptr : stmt->getSuccessor())
	{
		if (isa<BreakStmt>(stmt) && !inSwitch) return true;
		if (isa<ContinueStmt>(stmt)) return true;
		if (auto is = cast<IfStmt>(stmt))
		{
			for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci)
				if (hasEscapingBreakOrContinue(ci->second, inSwitch)) return true;
			if (is->hasElseClause()
					&& hasEscapingBreakOrContinue(is->getElseClause(), inSwitch))
				return true;
		}
		else if (auto ss = cast<SwitchStmt>(stmt))
		{
			for (auto ci = ss->clause_begin(); ci != ss->clause_end(); ++ci)
				if (hasEscapingBreakOrContinue(ci->second, true)) return true;
		}
		// Loops: their breaks and continues bind to themselves.
	}
	return false;
}

/// The goto ending the chain @a body (skipping nothing), or null.  Sets
/// @a prev to the statement before it (null when the goto is the only one).
ShPtr<GotoStmt> trailingGoto(ShPtr<Statement> body, ShPtr<Statement>& prev)
{
	prev = nullptr;
	for (auto s = body; s; s = s->getSuccessor())
	{
		if (!s->getSuccessor())
			return cast<GotoStmt>(s);
		prev = s;
	}
	return nullptr;
}

/// Adds every statement of the chain from @a stmt, nested ones included, to
/// @a out.  @a single limits it to @a stmt and what it contains.
void collectStmts(ShPtr<Statement> stmt, std::set<ShPtr<Statement>>& out,
		bool single = false)
{
	for (int i = 0; stmt && i < 8192; ++i, stmt = single ? nullptr : stmt->getSuccessor())
	{
		if (!out.insert(stmt).second) return;
		if (auto is = cast<IfStmt>(stmt))
		{
			for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci)
				collectStmts(ci->second, out);
			if (is->hasElseClause()) collectStmts(is->getElseClause(), out);
		}
		else if (auto ss = cast<SwitchStmt>(stmt))
		{
			for (auto ci = ss->clause_begin(); ci != ss->clause_end(); ++ci)
				collectStmts(ci->second, out);
		}
		else if (auto wl = cast<WhileLoopStmt>(stmt))
			collectStmts(wl->getBody(), out);
		else if (auto fl = cast<ForLoopStmt>(stmt))
			collectStmts(fl->getBody(), out);
		else if (auto ul = cast<UForLoopStmt>(stmt))
			collectStmts(ul->getBody(), out);
	}
}

/// Whether some goto outside @a region (and not @a ignore) targets a
/// statement inside it.  Moving such a region into a new loop or clause makes
/// that goto jump into the middle of a compound statement, which later
/// passes (variable definitions, copy propagation) do not model.
bool isEnteredFromOutside(const std::set<ShPtr<Statement>>& region,
		const std::set<ShPtr<Statement>>& allowed)
{
	for (auto &st: region)
	{
		for (auto pi = st->predecessor_begin(); pi != st->predecessor_end(); ++pi)
		{
			auto g = cast<GotoStmt>(*pi);
			if (g && g->getTarget() == st && !region.count(g) && !allowed.count(g))
				return true;
		}
	}
	return false;
}

/// Pattern E: a clause of @a is ends with `goto T`, T a later sibling of
/// @a is.  The statements between @a is and T run only when the other clause
/// was taken, so they move to its end and the goto goes:
///
///     if (c) { A; goto T; } [else { B; }]  R;  T: ...
///  => if (c) { A; } else { [B;] R; }  T: ...
bool tryTrailingForwardGoto(const ShPtr<IfStmt>& is)
{
	if (is->hasElseIfClauses() || !is->getSuccessor()) return false;

	for (int which = 0; which < 2; ++which)
	{
		bool inElse = which == 1;
		if (inElse && !is->hasElseClause()) break;
		auto body = inElse ? is->getElseClause() : is->getFirstIfBody();
		ShPtr<Statement> prev;
		auto g = trailingGoto(body, prev);
		if (!g || g->isGotoTarget() || !g->getTarget()) continue;
		auto target = g->getTarget();
		// The sole-goto then-clause without an else is Pattern A's.
		if (!prev && !inElse && !is->hasElseClause()) continue;
		if (target == is->getSuccessor() && !prev) continue; // Pattern B-like; leave
		std::vector<ShPtr<Statement>> run;
		if (target != is->getSuccessor())
		{
			run = collectBetween(is->getSuccessor(), target);
			if (run.empty()) continue;
		}

		// A goto from elsewhere into the run now enters a clause of `is`.
		// That is what Pattern A has always produced, and the CFG builders
		// model it; a jump into a loop body (Patterns F and G) is refused.

		// Detach the run from the main chain.
		if (!run.empty())
		{
			is->setSuccessor(target);
			run.back()->setSuccessor(nullptr);
		}

		// Drop the goto.
		if (prev)
		{
			Statement::removeStatement(g);
		}
		else if (inElse)
		{
			is->removeElseClause();
		}
		else
		{
			// `if (c) goto T; else { B; }`: becomes `if (!c) { B; }`.
			auto elseBody = is->getElseClause();
			target->removeObserver(g);
			target->removePredecessor(g);
			is->removeElseClause();
			is->setFirstIfCond(ExpressionNegater::negate(is->getFirstIfCond()));
			is->setFirstIfBody(elseBody);
			inElse = true; // the "other" clause is now the then-clause
			if (!run.empty())
			{
				Statement::getLastStatement(elseBody)->setSuccessor(run.front());
			}
			return true;
		}

		if (run.empty()) return true;

		// Append the run to the other clause.
		if (inElse)
		{
			Statement::getLastStatement(is->getFirstIfBody())->setSuccessor(run.front());
		}
		else if (is->hasElseClause())
		{
			Statement::getLastStatement(is->getElseClause())->setSuccessor(run.front());
		}
		else
		{
			is->setElseClause(run.front());
		}
		return true;
	}
	return false;
}

/// Pattern F: a backward goto to an earlier sibling becomes a loop.
///
///     L: S...;  if (c) goto L;  R    =>  while (true) { S...; if (!c) break; }  R
///     L: S...;  goto L;         R    =>  while (true) { S...; }  R
///
/// Not done when S holds a break or continue bound outside it, which the new
/// loop would capture.
bool tryBackwardSiblingGoto(const ShPtr<Statement>& stmt)
{
	ShPtr<GotoStmt> g;
	auto is = cast<IfStmt>(stmt);
	if (is)
	{
		if (is->hasElseClause() || is->hasElseIfClauses()) return false;
		auto b = is->getFirstIfBody();
		while (b && isa<EmptyStmt>(b)) b = b->getSuccessor();
		if (!b || b->getSuccessor()) return false;
		g = cast<GotoStmt>(b);
	}
	else
	{
		g = cast<GotoStmt>(stmt);
	}
	if (!g || !g->getTarget() || g->isGotoTarget()) return false;
	auto target = g->getTarget();
	if (target == stmt) return false;
	auto run = collectBetween(target, stmt, 1024);
	if (run.empty()) return false;
	// Only a loop entered at its top reads as one: when the statement before
	// L never falls through to it, every entry is a jump into the middle.
	for (auto pi = target->predecessor_begin(); pi != target->predecessor_end(); ++pi)
	{
		auto p = *pi;
		if (p->getSuccessor() == target && (isa<ReturnStmt>(p) || isa<BreakStmt>(p)
				|| isa<ContinueStmt>(p) || isa<GotoStmt>(p)))
			return false;
	}
	for (auto &s: run)
		if (hasEscapingBreakOrContinue(s, false, /*single=*/true)) return false;

	{
		std::set<ShPtr<Statement>> region, none;
		for (auto &r: run) collectStmts(r, region, /*single=*/true);
		collectStmts(stmt, region, /*single=*/true);
		region.erase(target); // L itself: a jump to it enters the loop at its top
		if (isEnteredFromOutside(region, none)) return false;
	}

	auto after = stmt->getSuccessor();
	auto loop = WhileLoopStmt::create(ConstBool::create(true),
		EmptyStmt::create(), nullptr, target->getAddress());
	target->prependStatement(loop);
	// Jumping to the loop's first statement is entering the loop.
	loop->transferLabelFrom(target);
	target->redirectGotosTo(loop);
	// `stmt` no longer falls through to `after`; the loop does.
	stmt->setSuccessor(nullptr);
	loop->setSuccessor(after);
	loop->setBody(target);
	if (is)
	{
		is->setFirstIfCond(ExpressionNegater::negate(is->getFirstIfCond()));
		is->setFirstIfBody(BreakStmt::create());
	}
	else
	{
		Statement::removeStatement(g);
	}
	return true;
}

/// Whether @a cond is a constant true (a `while (true)` condition).
bool isConstTrue(const ShPtr<Expression>& cond)
{
	if (auto cb = cast<ConstBool>(cond)) return cb->isTrue();
	if (auto ci = cast<ConstInt>(cond)) return !ci->isZero();
	return false;
}

/// Whether the chain from @a stmt holds a break bound outside it.  A
/// continue inside a `while (true)` body binds to that loop, so only breaks
/// count here.
bool hasEscapingBreak(ShPtr<Statement> stmt, bool single = false)
{
	for (int i = 0; stmt && i < 4096; ++i, stmt = single ? nullptr : stmt->getSuccessor())
	{
		if (isa<BreakStmt>(stmt)) return true;
		if (auto is = cast<IfStmt>(stmt))
		{
			for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci)
				if (hasEscapingBreak(ci->second)) return true;
			if (is->hasElseClause() && hasEscapingBreak(is->getElseClause()))
				return true;
		}
		// Breaks in a nested switch or loop bind to it.
	}
	return false;
}

/// Pattern G: code after a `while (true)` loop that has no break is reached
/// only by gotos; when it ends by jumping back to the loop's head it is the
/// tail of the loop body, and moves there:
///
///     while (true) { L: S; }  R;  goto L;   =>  while (true) { L: S; [continue;] R; }
///
/// `continue;` goes in when S can fall off its end, which loops back to L
/// and must keep doing so.
bool tryTailBackIntoLoop(const ShPtr<WhileLoopStmt>& wl)
{
	if (!isConstTrue(wl->getCondition())) return false;
	auto head = wl->getBody();
	if (!head || hasEscapingBreak(head)) return false;

	std::vector<ShPtr<Statement>> run;
	ShPtr<GotoStmt> back;
	for (auto s = wl->getSuccessor(); s && run.size() < 256; s = s->getSuccessor())
	{
		if (auto g = cast<GotoStmt>(s))
		{
			if (g->getTarget() == head || g->getTarget() == wl)
			{
				back = g;
				break;
			}
		}
		if (hasEscapingBreakOrContinue(s, false, /*single=*/true)) return false;
		run.push_back(s);
	}
	if (!back || run.empty()) return false;
	{
		std::set<ShPtr<Statement>> region, allowed;
		for (auto &r: run) collectStmts(r, region, /*single=*/true);
		collectStmts(wl, allowed, /*single=*/true);
		if (isEnteredFromOutside(region, allowed)) return false;
	}

	auto after = back->getSuccessor();
	wl->setSuccessor(after);
	back->setSuccessor(nullptr);
	Statement::removeStatement(back);
	run.back()->setSuccessor(nullptr);

	auto last = Statement::getLastStatement(head);
	if (!isa<GotoStmt>(last) && !isa<ReturnStmt>(last) && !isa<ContinueStmt>(last))
	{
		auto cont = ContinueStmt::create();
		last->setSuccessor(cont);
		last = cont;
	}
	last->setSuccessor(run.front());
	return true;
}

/// One pass over the body of @a func.  Returns true if any change was made.
/// @a loopExit is the statement that follows the enclosing loop (used to
/// detect gotos that should become break statements — Pattern D).
bool onePass(ShPtr<Function> func)
{
	bool anyChange = false;

	std::function<bool(ShPtr<Statement>, ShPtr<Statement>, ShPtr<Statement>)> walk;
	walk = [&](ShPtr<Statement> stmt, ShPtr<Statement> loopExit, ShPtr<Statement> loopHead) -> bool {
		bool changed = false;
		while (stmt)
		{
			if (tryBackwardSiblingGoto(stmt))
			{
				anyChange = true;
				return true;
			}
			// --- Pattern A: if(cond) goto L_forward ---
			if (auto is = cast<IfStmt>(stmt))
			{
				// Recurse into branches first.
				for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci)
					changed |= walk(ci->second, loopExit, loopHead);
				if (is->hasElseClause()) changed |= walk(is->getElseClause(), loopExit, loopHead);

				// Now try Pattern A on this if-stmt.
				if (!is->hasElseClause() && !is->hasElseIfClauses())
				{
					// Body must be a single GotoStmt (possibly preceded by
					// empty statements inserted by earlier passes).
					ShPtr<Statement> b = is->getFirstIfBody();
					// Skip leading empty stmts to find the real first stmt.
					while (b && isa<EmptyStmt>(b))
						b = b->getSuccessor();
					if (b && isa<GotoStmt>(b) && !b->getSuccessor())
					{
						auto target = cast<GotoStmt>(b)->getTarget();
						if (target)
						{
							// Target must be forward.
							auto between = collectBetween(is->getSuccessor(), target);
							if (!between.empty())
							{
								// Rewrite `is` in place: same statement
								// object, inverted condition, the collected
								// statements as its body.  Building a second
								// IfStmt here and moving `is`'s label and
								// inbound gotos onto it stranded both on a
								// statement that was then dropped on the
								// floor -- the label vanished from the output
								// while the gotos still named it.
								auto negCond = ExpressionNegater::negate(is->getFirstIfCond());
								moveRunIntoIfBody(is, between, target);
								is->setFirstIfCond(negCond);
								changed = true;
								anyChange = true;
								// Don't advance stmt; the rewritten `is` is
								// re-examined by the outer fixed point.
							}
						}
					}
				}
				if (!changed && tryTrailingForwardGoto(is))
				{
					changed = true;
					anyChange = true;
				}
			}

			// --- Pattern D: goto to loop exit → break ---
			// Check before Pattern B so we emit break, not just remove goto.
			else if (auto gs = cast<GotoStmt>(stmt))
			{
				auto target = gs->getTarget();
				if (target && loopExit && target == loopExit)
				{
					// This goto exits the enclosing loop: replace with break.
					auto brk = BreakStmt::create();
					brk->transferLabelFrom(gs);
					gs->redirectGotosTo(brk);
					// prependStatement() links the end of brk's chain to
					// gs, so brk must not have a successor yet; giving it
					// gs's successor first made a cycle.  removeStatement()
					// below links brk to gs's successor.
					gs->prependStatement(brk);
					Statement::removeStatement(gs);
					changed = true;
					anyChange = true;
					// brk is now at the same position; advance past it.
					stmt = brk->getSuccessor();
					continue;
				}

				// --- Pattern C: goto to the head of the enclosing
				// while (true) -> continue ---
				if (target && loopHead && !gs->isGotoTarget()
						&& (target == loopHead
							|| target == cast<WhileLoopStmt>(loopHead)->getBody()))
				{
					auto cont = ContinueStmt::create();
					gs->prependStatement(cont);
					Statement::removeStatement(gs);
					changed = true;
					anyChange = true;
					stmt = cont->getSuccessor();
					continue;
				}

				// --- Pattern B: unconditional goto to immediate successor ---
				if (target && gs->getSuccessor() == target)
				{
					// This goto is a no-op.
					if (!gs->isGotoTarget())
					{
						Statement::removeStatement(gs);
						changed = true;
						anyChange = true;
						// stmt was removed; let fixed-point re-run.
						break;
					}
				}
			}

			// --- WhileLoopStmt / ForLoopStmt: recurse into body ---
			// Pass the statement after the loop as the new loopExit.
			else if (auto wl = cast<WhileLoopStmt>(stmt))
			{
				if (tryTailBackIntoLoop(wl))
				{
					anyChange = true;
					return true;
				}
				changed |= walk(wl->getBody(), wl->getSuccessor(),
					isConstTrue(wl->getCondition()) ? ShPtr<Statement>(wl) : nullptr);
			}
			else if (auto fl = cast<ForLoopStmt>(stmt))
			{
				changed |= walk(fl->getBody(), fl->getSuccessor(), nullptr);
			}

			stmt = stmt->getSuccessor();
		}
		return changed;
	};

	return walk(func->getBody(), nullptr, nullptr);
}

} // anonymous namespace

//===========================================================================
// GotoCFGOptimizer
//===========================================================================

GotoCFGOptimizer::GotoCFGOptimizer(ShPtr<Module> module): FuncOptimizer(module)
{
	PRECONDITION_NON_NULL(module);
}

void GotoCFGOptimizer::runOnFunction(ShPtr<Function> func)
{
	if (!func || !func->isDefinition()) return;

	// Iterate until fixed point (each pass may unlock further rewrites).
	constexpr int MAX_PASSES = 32;
	for (int i = 0; i < MAX_PASSES; ++i)
	{
		if (!onePass(func)) break;
	}
}

} // namespace llvmir2hll
} // namespace retdec
