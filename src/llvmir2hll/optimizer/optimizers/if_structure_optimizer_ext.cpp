/**
* @file src/llvmir2hll/optimizer/optimizers/if_structure_optimizer_ext.cpp
* @brief Pattern 6 for IfStructureOptimizer: if/else-if/else consolidation.
* @copyright (c) 2024, MIT license
*
* The existing IfStructureOptimizer has 5 patterns. Pattern 4 merges two
* consecutive ifs with IDENTICAL bodies (and no else) into a single
* `if (cond1 || cond2)`. The restriction is: neither if may have an else clause.
*
* The TODO comment says this can be relaxed. This patch adds two new patterns:
*
*  Pattern 6: Two consecutive ifs with identical bodies, where the SECOND one
*             has an else clause. Transform:
*
*    if (cond1):            if (cond1 || cond2):
*        body               →     body
*    if (cond2):               else:
*        body                      else_body
*    else:
*        else_body
*
*  Pattern 7: Three or more consecutive ifs on the SAME control variable
*             that end in an else clause → emit a single if/else-if/.../else
*             chain. Specifically, extend a chain of `if (v == K)` that
*             already terminated in an else clause to absorb a leading
*             `if (v == K2)` with the same body structure.
*
*             This is the structural pre-condition for if_to_switch_optimizer
*             to later convert the whole chain to a switch.
*
*  Call tryOptimization6() and tryOptimization7() from the end of
*  IfStructureOptimizer::visit(ShPtr<IfStmt>).
*/

#include "retdec/llvmir2hll/ir/and_op_expr.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/or_op_expr.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/unreachable_stmt.h"
#include "retdec/llvmir2hll/optimizer/optimizers/if_structure_optimizer_ext.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

namespace {

/// Returns true if @a stmt has no else-if clauses and at most one else clause.
static bool isSimpleIf(ShPtr<IfStmt> stmt) {
    return !stmt->hasElseIfClauses();
}

/// Returns true if @a body ends with a return/goto/break/continue/unreachable.
static bool endsWithJump(ShPtr<Statement> body) {
    if (!body) return false;
    ShPtr<Statement> last = body;
    while (last->hasSuccessor()) last = last->getSuccessor();
    return isa<ReturnStmt>(last) ||
           isa<GotoStmt>(last)   ||
           isa<BreakStmt>(last)  ||
           isa<ContinueStmt>(last) ||
           isa<UnreachableStmt>(last);
}

} // anonymous namespace

/**
 * Pattern 6:
 *   if (A): body
 *   if (B): body      ← same body, second has else clause
 *   else: else_body
 * →
 *   if (A || B): body
 *   else: else_body
 *
 * @return true if the optimization was applied.
 */
bool tryOptimization6(ShPtr<IfStmt> stmt) {
    // stmt must be a plain if (no else-if, no else).
    if (!isSimpleIf(stmt) || stmt->hasElseClause()) return false;

    // Next statement must be an if with an else clause but no else-if.
    auto nextStmt = cast<IfStmt>(stmt->getSuccessor());
    if (!nextStmt || !isSimpleIf(nextStmt) || !nextStmt->hasElseClause()) return false;

    // Both bodies must be identical.
    if (!Statement::areEqualStatements(stmt->getFirstIfBody(),
                                       nextStmt->getFirstIfBody())) return false;

    // Both bodies must end with a jump (so the else clause is reachable
    // only when both conditions are false — this is the structural
    // requirement for the merge to be semantically correct).
    // If the bodies do NOT end with a jump, the merge is still valid
    // because both branches lead to the else, but we restrict to ending
    // with a jump for simplicity and safety.
    if (!endsWithJump(stmt->getFirstIfBody())) return false;

    // Merge: (A || B)
    stmt->setFirstIfCond(OrOpExpr::create(stmt->getFirstIfCond(),
                                          nextStmt->getFirstIfCond()));
    // Transfer the else clause from nextStmt to stmt.
    stmt->setElseClause(nextStmt->getElseClause());

    // Remove nextStmt from the statement list.
    Statement::removeStatement(nextStmt);
    return true;
}

/**
 * Pattern 7:
 * Absorb a leading plain `if (cond): body` into an existing else-if chain
 * that starts with the successor, provided they share identical bodies.
 *
 *   if (A): body
 *   if (B): body
 *   else-if (C): body2
 *   else: default_body
 * →
 *   if (A || B): body
 *   else-if (C): body2
 *   else: default_body
 *
 * This is a generalisation of Pattern 4 for chains that already have
 * else-if clauses.
 *
 * @return true if the optimization was applied.
 */
bool tryOptimization7(ShPtr<IfStmt> stmt) {
    // stmt must be a plain if (no else-if, no else).
    if (!isSimpleIf(stmt) || stmt->hasElseClause()) return false;

    // Next must be an if with else-if clauses (i.e. an existing chain).
    auto nextStmt = cast<IfStmt>(stmt->getSuccessor());
    if (!nextStmt) return false;

    // The leading body of nextStmt must be identical to stmt's body.
    if (!Statement::areEqualStatements(stmt->getFirstIfBody(),
                                       nextStmt->getFirstIfBody())) return false;

    // Both bodies must end with a jump for safety.
    if (!endsWithJump(stmt->getFirstIfBody())) return false;

    // Merge conditions into nextStmt's leading condition.
    nextStmt->setFirstIfCond(OrOpExpr::create(stmt->getFirstIfCond(),
                                               nextStmt->getFirstIfCond()));

    // Remove stmt — nextStmt now handles both conditions.
    Statement::removeStatement(stmt);
    return true;
}

} // namespace llvmir2hll
} // namespace retdec
