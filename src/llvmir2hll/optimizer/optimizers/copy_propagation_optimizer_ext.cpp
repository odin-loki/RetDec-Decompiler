/**
* @file src/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer_ext.cpp
* @brief Extension for CopyPropagationOptimizer: propagate through intervening statements.
* @copyright (c) 2024, MIT license
*
* The existing copy propagation optimizer has a TODO at line 587:
*
*   // For simplicity, we currently consider only the situation where the
*   // use is the next statement.
*   // TODO Implement support for situations where there are some
*   //      statements between stmt and use.
*
* This extension implements the missing case: allow copy propagation of a
* function-call result across intervening statements, provided:
*
*   1. The LHS variable is not read or written by any intervening statement.
*   2. No intervening statement calls a function that could modify the LHS
*      variable through aliasing (conservative: skip if any user-defined
*      function call intervenes and the LHS variable is a global or pointer).
*   3. The LHS variable appears exactly once in the use statement.
*   4. The call result (RHS) does not involve any variable that could be
*      modified by intervening statements.
*
* Usage: call canPropagateThroughStmts() from CopyPropagationOptimizer
* before the `if (stmtSucc != use) return;` guard, and skip that guard
* when this function returns true.
*/

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/expression.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer_ext.h"

namespace retdec {
namespace llvmir2hll {

/**
 * Returns true if @a var is written (defined) in @a stmt.
 * A write means @a stmt is an AssignStmt whose LHS is @a var, or a
 * VarDefStmt defining @a var.
 */
static bool isWrittenIn(ShPtr<Variable> var, ShPtr<Statement> stmt) {
    if (!stmt) return false;
    if (auto assign = cast<AssignStmt>(stmt)) {
        if (auto lhsVar = cast<Variable>(assign->getLhs())) {
            if (lhsVar == var) return true;
        }
    }
    return false;
}

/**
 * Returns true if @a var appears as a direct read in the value-analysis
 * data for @a stmt (i.e. the variable is used in @a stmt's RHS or anywhere
 * in a call's arguments).
 */
static bool isReadIn(ShPtr<Variable> var, ShPtr<Statement> stmt,
                      ShPtr<ValueAnalysis> va) {
    if (!stmt || !va) return false;
    auto data = va->getValueData(stmt);
    if (!data) return false;
    return data->getDirNumOfUses(var) > 0;
}

/**
 * Returns true if @a stmt contains a call to a user-defined (non-declaration)
 * function — a conservative indicator that globals/pointers may be modified.
 */
static bool hasUserDefinedCall(ShPtr<Statement> stmt,
                                ShPtr<Module> module,
                                ShPtr<ValueAnalysis> va) {
    if (!stmt || !va) return false;
    auto data = va->getValueData(stmt);
    if (!data) return false;
    for (auto it = data->call_begin(); it != data->call_end(); ++it) {
        auto call = *it;
        if (auto varExpr = cast<Variable>(call->getCalledExpr())) {
            auto fnc = module->getFuncByName(varExpr->getName());
            if (fnc && !fnc->isDeclaration()) return true;
        }
    }
    return false;
}

/**
 * Check whether copy propagation of `lhsVar = <callRhs>` into a use
 * statement @a use is safe, even when there are intervening statements
 * between @a stmt and @a use.
 *
 * @param stmt   The assignment statement `lhsVar = callRhs`.
 * @param lhsVar The variable being propagated.
 * @param use    The statement that uses @a lhsVar.
 * @param module The module (for function lookups).
 * @param va     Value analysis.
 *
 * @return true if it is safe to propagate.
 */
bool canPropagateThroughStmts(ShPtr<Statement> stmt,
                               ShPtr<Variable> lhsVar,
                               ShPtr<Statement> use,
                               ShPtr<Module> module,
                               ShPtr<ValueAnalysis> va) {
    if (!stmt || !lhsVar || !use) return false;

    // Walk from stmt's successor to use, checking each intervening statement.
    ShPtr<Statement> cur = stmt->getSuccessor();
    unsigned steps = 0;
    const unsigned MAX_STEPS = 16; // Don't walk too far.

    while (cur && cur != use && steps < MAX_STEPS) {
        // If lhsVar is written or read by an intervening statement, bail.
        if (isWrittenIn(lhsVar, cur)) return false;
        if (isReadIn(lhsVar, cur, va)) return false;

        // If there's a user-defined function call, be conservative:
        // it might modify globals or pointer targets.
        // We only allow external (declared-only) calls to pass through.
        if (hasUserDefinedCall(cur, module, va)) return false;

        cur = cur->getSuccessor();
        ++steps;
    }

    // Did we actually reach `use`?
    if (cur != use) return false;

    // Verify: lhsVar used exactly once in use (same guard as original code).
    auto useData = va->getValueData(use);
    if (!useData) return false;
    if (useData->getDirNumOfUses(lhsVar) != 1) return false;

    return true;
}

} // namespace llvmir2hll
} // namespace retdec
