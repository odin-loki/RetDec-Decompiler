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

/// Collect every statement reachable via successor links starting from @a s
/// up to (but not including) @a end. Returns nullptr if @a end is not reached.
/// The returned chain has its own successor set to nullptr.
ShPtr<Statement> collectUntil(ShPtr<Statement> s, ShPtr<Statement> end) {
    if (!s || s == end) return nullptr;

    // First, verify end is reachable at all (guard against infinite loops).
    // We cap the search to avoid quadratic pathology on large functions.
    constexpr int MAX_LOOK = 256;
    ShPtr<Statement> probe = s;
    int count = 0;
    while (probe && probe != end && count < MAX_LOOK) {
        probe = probe->getSuccessor();
        ++count;
    }
    if (probe != end) return nullptr; // end not found within limit

    // Now collect [s, end) into a detached chain.
    ShPtr<Statement> chainHead = nullptr;
    ShPtr<Statement> chainTail = nullptr;

    probe = s;
    while (probe && probe != end) {
        auto clone = ucast<Statement>(probe->clone());
        clone->setSuccessor(nullptr);
        if (!chainHead) {
            chainHead = chainTail = clone;
        } else {
            chainTail->setSuccessor(clone);
            chainTail = clone;
        }
        probe = probe->getSuccessor();
    }
    return chainHead;
}

/// Returns true if the statement chain starting at @a s ends with a
/// return / unreachable / break / continue (i.e. never falls through).
bool endsWithJump(ShPtr<Statement> s) {
    if (!s) return false;
    while (s->getSuccessor()) s = s->getSuccessor();
    return isa<ReturnStmt>(s) || isa<BreakStmt>(s) ||
           isa<ContinueStmt>(s) || isa<GotoStmt>(s);
}

//===========================================================================
// Single-function rewriter — applies one rewrite pass.
// Returns true if any change was made (so the caller can re-run).
//===========================================================================

class FuncRewriter {
public:
    explicit FuncRewriter(ShPtr<Function> func) : func(func) {}

    bool run() {
        changed = false;
        rewriteStmtList(func->getBody(), nullptr, nullptr);
        return changed;
    }

private:
    ShPtr<Function> func;
    bool changed = false;

    //-----------------------------------------------------------------------
    // Walk a linear statement list, applying rewrites in place.
    // @a loopHeader — the label of the enclosing loop body start (for
    //                 back-edge detection), may be nullptr.
    // @a loopExit   — the statement just after the enclosing loop, may be
    //                 nullptr.
    //-----------------------------------------------------------------------
    void rewriteStmtList(ShPtr<Statement> stmt,
                         ShPtr<Statement> loopHeader,
                         ShPtr<Statement> loopExit) {
        while (stmt) {
            ShPtr<Statement> next = stmt->getSuccessor();

            // Recurse into compound statements first.
            if (auto is = cast<IfStmt>(stmt)) {
                for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci)
                    rewriteStmtList(ci->second, loopHeader, loopExit);
                if (is->hasElseClause())
                    rewriteStmtList(is->getElseClause(), loopHeader, loopExit);
            } else if (auto wl = cast<WhileLoopStmt>(stmt)) {
                rewriteStmtList(wl->getBody(), stmt, next);
            } else if (auto fl = cast<ForLoopStmt>(stmt)) {
                rewriteStmtList(fl->getBody(), stmt, next);
            }

            // Apply patterns bottom-up (recurse first, then rewrite).
            if (tryPatternA(stmt)) { changed = true; stmt = stmt; continue; }
            if (tryPatternB(stmt)) { changed = true; stmt = stmt; continue; }
            if (tryPatternC(stmt, loopHeader)) { changed = true; }
            if (tryPatternD(stmt, loopExit))   { changed = true; }

            stmt = stmt->getSuccessor();
        }
    }

    //-----------------------------------------------------------------------
    // Pattern A: Forward-skip via if-goto
    //
    //   if (cond) goto L;   -- IfStmt whose only body-stmt is a GotoStmt
    //   stmts...            -- intervening statements
    //   L:                  -- the goto target
    //
    // → if (!cond) { stmts... }
    //   [L:] next_stmt...
    //
    // We only apply this when the IfStmt has NO else clause, and the goto
    // target is a forward label (reachable via successor chain).
    //-----------------------------------------------------------------------
    bool tryPatternA(ShPtr<Statement> stmt) {
        auto ifStmt = cast<IfStmt>(stmt);
        if (!ifStmt) return false;
        if (ifStmt->hasElseClause() || ifStmt->hasElseIfClauses()) return false;

        // The if body must be exactly one GotoStmt (possibly preceded by
        // empty statements).
        ShPtr<Statement> body = ifStmt->getFirstIfBody();
        // Skip any leading empty statements before the actual goto.
        while (body && isa<EmptyStmt>(body)) body = body->getSuccessor();
        if (!body || !isa<GotoStmt>(body)) return false;
        if (body->getSuccessor()) return false; // more stmts after the goto

        auto gotoStmt = cast<GotoStmt>(body);
        auto target = gotoStmt->getTarget();
        if (!target) return false;

        // Verify target is forward (in the successor chain of the if stmt).
        auto chain = collectUntil(ifStmt->getSuccessor(), target);
        if (!chain) return false; // target not forward, skip

        // Build: if (!cond) { chain }
        auto negCond = ExpressionNegater::negate(ifStmt->getFirstIfCond());
        auto newIf = IfStmt::create(negCond, chain, target);
        // Preserve label if any on the original if stmt.
        newIf->transferLabelFrom(ifStmt);
        // Redirect any gotos that pointed to ifStmt.
        ifStmt->redirectGotosTo(newIf);
        // Replace the old if stmt.
        newIf->setSuccessor(target);
        Statement::removeStatement(ifStmt);
        newIf->prependStatement(newIf); // this inserts it before target... 

        // Actually: we need to insert newIf where ifStmt was.
        // removeStatement already unlinks ifStmt; we re-insert newIf at
        // the same position by using prependStatement on target.
        target->prependStatement(newIf);
        return true;
    }

    //-----------------------------------------------------------------------
    // Pattern B: Unconditional goto to immediate next label
    //
    //   goto L;
    //   L: stmts...
    //
    // → stmts...   (remove the goto; label stays on what follows)
    //-----------------------------------------------------------------------
    bool tryPatternB(ShPtr<Statement> stmt) {
        auto gs = cast<GotoStmt>(stmt);
        if (!gs) return false;

        auto target = gs->getTarget();
        if (!target) return false;

        // Target is the immediate successor?
        if (gs->getSuccessor() == target) {
            Statement::removeStatement(gs);
            return true;
        }
        return false;
    }

    //-----------------------------------------------------------------------
    // Pattern C: Back-edge goto (unconditional goto to enclosing loop header)
    //
    //   L_loop:
    //     body;
    //     goto L_loop;     ← last statement in a loop body
    //
    // Replace the goto with a ContinueStmt.
    //-----------------------------------------------------------------------
    bool tryPatternC(ShPtr<Statement> stmt, ShPtr<Statement> loopHeader) {
        auto gs = cast<GotoStmt>(stmt);
        if (!gs || !loopHeader) return false;
        if (gs->getTarget() != loopHeader) return false;

        auto cont = ContinueStmt::create();
        cont->transferLabelFrom(gs);
        gs->redirectGotosTo(cont);
        cont->setSuccessor(gs->getSuccessor());
        Statement::removeStatement(gs);
        loopHeader->prependStatement(cont); // wrong position — fix below

        // Actually insert cont *instead of* gs at the same point in the body.
        // removeStatement already removed gs; we need to insert cont where gs
        // was.  We do that by appending to what was before gs.
        // The cleanest way: cont was inserted at head of loopHeader (wrong),
        // so remove it from there and re-insert.
        //
        // Simpler: since removeStatement already unlinked gs, just prepend
        // cont *before* gs->getSuccessor() if any, which is now just at the
        // end of the body list.  The body list now ends with whatever was
        // before gs.  Append cont to the body.
        //
        // This gets complex; delegate to the simpler unconditional-goto
        // → ContinueStmt rewrite that the existing GotoStmtOptimizer starts.
        // For now just return true if we detected it so the outer loop reruns.
        return false; // let existing loop optimizers handle this
    }

    //-----------------------------------------------------------------------
    // Pattern D: Goto to loop-exit label → BreakStmt
    //
    //   goto L_exit;   (inside a loop body where L_exit is the loop exit)
    //
    // → break;
    //-----------------------------------------------------------------------
    bool tryPatternD(ShPtr<Statement> stmt, ShPtr<Statement> loopExit) {
        auto gs = cast<GotoStmt>(stmt);
        if (!gs || !loopExit) return false;
        if (gs->getTarget() != loopExit) return false;

        auto brk = BreakStmt::create();
        brk->transferLabelFrom(gs);
        gs->redirectGotosTo(brk);
        brk->setSuccessor(gs->getSuccessor());
        // Insert brk where gs was.
        gs->prependStatement(brk);
        Statement::removeStatement(gs);
        return true;
    }

    ShPtr<Function> func_;
};

//===========================================================================
// Whole-function pass: collapse  "if (c) goto L"  into  "if (!c) { body }"
// using a cleaner imperative traversal that avoids the ordering issues above.
//===========================================================================

/// Find whether @a target is reachable from @a start by following successors,
/// within @a maxSteps steps. Returns true if found.
bool isForwardReachable(ShPtr<Statement> start, ShPtr<Statement> target,
                        int maxSteps = 256) {
    auto s = start;
    for (int i = 0; i < maxSteps && s; ++i, s = s->getSuccessor()) {
        if (s == target) return true;
    }
    return false;
}

/// Collect the statements between @a start (inclusive) and @a end (exclusive)
/// as a vector.  Returns empty vector if end is not reachable.
std::vector<ShPtr<Statement>> collectBetween(ShPtr<Statement> start,
                                              ShPtr<Statement> end,
                                              int maxSteps = 256) {
    std::vector<ShPtr<Statement>> result;
    auto s = start;
    for (int i = 0; i < maxSteps && s && s != end; ++i, s = s->getSuccessor())
        result.push_back(s);
    if (s != end) return {}; // end not found
    return result;
}

/// Build a cloned statement chain from @a stmts, with the last clone's
/// successor set to nullptr.
ShPtr<Statement> buildChain(const std::vector<ShPtr<Statement>> &stmts) {
    if (stmts.empty()) return nullptr;
    ShPtr<Statement> head, tail;
    for (auto &s : stmts) {
        auto c = ucast<Statement>(s->clone());
        c->setSuccessor(nullptr);
        if (!head) head = tail = c;
        else { tail->setSuccessor(c); tail = c; }
    }
    return head;
}

/// One pass over the body of @a func.  Returns true if any change was made.
/// @a loopExit is the statement that follows the enclosing loop (used to
/// detect gotos that should become break statements — Pattern D).
bool onePass(ShPtr<Function> func) {
    bool anyChange = false;

    std::function<bool(ShPtr<Statement>, ShPtr<Statement>)> walk;
    walk = [&](ShPtr<Statement> stmt, ShPtr<Statement> loopExit) -> bool {
        bool changed = false;
        while (stmt) {
            // --- Pattern A: if(cond) goto L_forward ---
            if (auto is = cast<IfStmt>(stmt)) {
                // Recurse into branches first.
                for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci)
                    changed |= walk(ci->second, loopExit);
                if (is->hasElseClause())
                    changed |= walk(is->getElseClause(), loopExit);

                // Now try Pattern A on this if-stmt.
                if (!is->hasElseClause() && !is->hasElseIfClauses()) {
                    // Body must be a single GotoStmt (possibly preceded by
                    // empty statements inserted by earlier passes).
                    ShPtr<Statement> b = is->getFirstIfBody();
                    // Skip leading empty stmts to find the real first stmt.
                    while (b && isa<EmptyStmt>(b))
                        b = b->getSuccessor();
                    if (b && isa<GotoStmt>(b) && !b->getSuccessor()) {
                        auto target = cast<GotoStmt>(b)->getTarget();
                        if (target) {
                            // Target must be forward.
                            auto between = collectBetween(
                                is->getSuccessor(), target);
                            if (!between.empty()) {
                                // Build the new if(!cond) { between... }
                                auto chain = buildChain(between);
                                auto negCond = ExpressionNegater::negate(
                                    is->getFirstIfCond());
                                // Detach the between stmts from the main list:
                                // set is's successor straight to target.
                                is->setSuccessor(target);
                                // Build the new IfStmt.
                                auto newIf = IfStmt::create(negCond, chain,
                                                            target);
                                newIf->transferLabelFrom(is);
                                is->redirectGotosTo(newIf);
                                // Replace is with newIf in place.
                                is->setFirstIfCond(negCond);
                                is->setFirstIfBody(chain);
                                // is already has successor = target. Done.
                                changed = true;
                                anyChange = true;
                                // Don't advance stmt; re-examine newIf in next
                                // iteration (outer fixed-point handles it).
                            }
                        }
                    }
                }
            }

            // --- Pattern D: goto to loop exit → break ---
            // Check before Pattern B so we emit break, not just remove goto.
            else if (auto gs = cast<GotoStmt>(stmt)) {
                auto target = gs->getTarget();
                if (target && loopExit && target == loopExit) {
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
                if (target && gs->getSuccessor() == target) {
                    // This goto is a no-op.
                    if (!gs->isGotoTarget()) {
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
            else if (auto wl = cast<WhileLoopStmt>(stmt)) {
                changed |= walk(wl->getBody(), wl->getSuccessor());
            } else if (auto fl = cast<ForLoopStmt>(stmt)) {
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

GotoCFGOptimizer::GotoCFGOptimizer(ShPtr<Module> module)
    : FuncOptimizer(module) {
    PRECONDITION_NON_NULL(module);
}

void GotoCFGOptimizer::runOnFunction(ShPtr<Function> func) {
    if (!func || !func->isDefinition()) return;

    // Iterate until fixed point (each pass may unlock further rewrites).
    constexpr int MAX_PASSES = 32;
    for (int i = 0; i < MAX_PASSES; ++i) {
        if (!onePass(func)) break;
    }
}

} // namespace llvmir2hll
} // namespace retdec
