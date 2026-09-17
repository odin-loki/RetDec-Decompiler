/**
 * @file src/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.cpp
 * @brief Structural elimination of spurious goto statements.
 * @copyright (c) 2024, MIT license
 *
 * Implements patterns A–D described in the header. Runs iteratively on each
 * function until no further rewrites are possible.
 */

#include "retdec/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.h"

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
#include "retdec/llvmir2hll/ir/return_stmt.h"
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

/// One pass over the body of @a func.  Returns true if any change was made.
/// @a loopExit is the statement that follows the enclosing loop (used to
/// detect gotos that should become break statements — Pattern D).
bool onePass(ShPtr<Function> func)
{
	bool anyChange = false;

	std::function<bool(ShPtr<Statement>, ShPtr<Statement>)> walk;
	walk = [&](ShPtr<Statement> stmt, ShPtr<Statement> loopExit) -> bool {
		bool changed = false;
		while (stmt)
		{
			// --- Pattern A: if(cond) goto L_forward ---
			if (auto is = cast<IfStmt>(stmt))
			{
				// Recurse into branches first.
				for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci)
					changed |= walk(ci->second, loopExit);
				if (is->hasElseClause()) changed |= walk(is->getElseClause(), loopExit);

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
					brk->setSuccessor(gs->getSuccessor());
					gs->prependStatement(brk);
					Statement::removeStatement(gs);
					changed = true;
					anyChange = true;
					// brk is now at the same position; advance past it.
					stmt = brk->getSuccessor();
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
				changed |= walk(wl->getBody(), wl->getSuccessor());
			}
			else if (auto fl = cast<ForLoopStmt>(stmt))
			{
				changed |= walk(fl->getBody(), fl->getSuccessor());
			}

			stmt = stmt->getSuccessor();
		}
		return changed;
	};

	return walk(func->getBody(), nullptr);
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
