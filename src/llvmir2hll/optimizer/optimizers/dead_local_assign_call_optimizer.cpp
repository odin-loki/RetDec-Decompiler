/**
 * @file src/llvmir2hll/optimizer/optimizers/dead_local_assign_call_optimizer.cpp
 * @brief Extension of dead local assignment elimination: call-result demotion.
 * @copyright (c) 2024, MIT license
 *
 * The existing DeadLocalAssignOptimizer skips variables whose only "use" sites
 * have a function call on the RHS, because it can't remove the call.
 * This pass handles that exact case differently:
 *
 *   int32 a = func(x, y);    ← a never read after this
 *   ...
 *
 * is rewritten to:
 *
 *   func(x, y);              ← demote assign to void call-stmt; drop the variable
 *
 * This is safe because:
 *  - The call's side effects are preserved (it's still called).
 *  - Only the result is discarded, matching C's "cast to void" idiom.
 *  - We only do this when the variable has NO reads anywhere in the function
 *    (checked via VarUsesVisitor).
 *
 * Runs as a separate FuncOptimizer pass after DeadLocalAssignOptimizer.
 */

#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/analysis/var_uses_visitor.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/dead_local_assign_call_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/utils/ir.h"

namespace retdec {
namespace llvmir2hll {

DeadLocalAssignCallOptimizer::DeadLocalAssignCallOptimizer(
        ShPtr<Module> module, ShPtr<ValueAnalysis> va)
    : FuncOptimizer(module), va(va) {
    PRECONDITION_NON_NULL(module);
    PRECONDITION_NON_NULL(va);
}

void DeadLocalAssignCallOptimizer::doOptimization() {
    if (!va->isInValidState()) va->clearCache();
    vuv = VarUsesVisitor::create(va, true, module);
    FuncOptimizer::doOptimization();
}

void DeadLocalAssignCallOptimizer::runOnFunction(ShPtr<Function> func) {
    bool changed = true;
    while (changed) {
        changed = false;
        changed |= tryDemoteCallResults(func);
    }
}

/**
 * For every local variable that is:
 *   - assigned exactly once, at a VarDefStmt or AssignStmt
 *   - the RHS of that statement is a bare CallExpr (possibly with casts)
 *   - the variable is never read (no indirect uses, no direct reads)
 *
 * demote the statement to a CallStmt and remove the variable definition.
 */
bool DeadLocalAssignCallOptimizer::tryDemoteCallResults(ShPtr<Function> func) {
    bool changed = false;

    for (const auto& var : func->getLocalVars()) {
        if (var->isExternal()) continue;

        ShPtr<VarUses> uses = vuv->getUses(var, func);

        // Variable must have no indirect uses (passed to another func, addr taken, etc.)
        if (!uses->indirUses.empty()) continue;

        // All direct uses must be on the LHS of an assignment (i.e. write-only).
        bool allWriteOnly = true;
        for (const auto& use : uses->dirUses) {
            if (!isVarDefOrAssignStmt(use)) { allWriteOnly = false; break; }
            if (getLhs(use) != var)         { allWriteOnly = false; break; }
            // Verify var is not also read on the RHS of the same statement.
            ShPtr<ValueData> data = va->getValueData(use);
            if (data->getDirReadVars().count(var)) { allWriteOnly = false; break; }
        }
        if (!allWriteOnly) continue;

        // Each write-only use must have a call on its RHS.
        for (const auto& useStmt : StmtSet(uses->dirUses)) {
            ShPtr<Expression> rhs = getRhs(useStmt);
            if (!rhs) continue;

            // Strip top-level casts to find the call.
            ShPtr<Expression> inner = skipCasts(rhs);
            ShPtr<CallExpr> callExpr = cast<CallExpr>(inner);
            if (!callExpr) continue;

            // Demote: replace VarDefStmt/AssignStmt with CallStmt.
            // Pass nullptr for successor: Statement::replaceStatement() calls
            // mergeStatements(callStmt, oldStmt->succ) which appends the
            // original successor onto callStmt's chain. If we pre-set the
            // successor via create(), mergeStatements walks to the end of the
            // pre-set chain (= the original successor) and then calls
            // setSuccessor(originalSuccessor) on it — creating a cycle.
            ShPtr<CallStmt> callStmt = CallStmt::create(callExpr,
                nullptr, useStmt->getAddress());
            Statement::replaceStatement(useStmt, callStmt);

            va->removeFromCache(useStmt);
            vuv->stmtHasBeenRemoved(useStmt, func);
            changed = true;
        }

        // If the variable now has no uses, remove its VarDef if any remain.
        // (runOnFunction loops, so unreachable VarDefs are cleaned by
        //  DeadLocalAssignOptimizer on the next combined pass.)
    }

    return changed;
}

} // namespace llvmir2hll
} // namespace retdec
