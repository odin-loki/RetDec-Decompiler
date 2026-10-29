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
* function-call result across intervening statements, provided every
* intervening statement is "transparent" to the call (see isTransparentFor()):
* a call-free, memory-access-free assignment that touches neither the LHS
* variable nor anything the call could observe, and the LHS variable appears
* exactly once in the use statement.
*
* Usage: call canPropagateThroughStmts() from CopyPropagationOptimizer
* before the `if (stmtSucc != use) return;` guard, and skip that guard
* when this function returns true.
*/

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/expression.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer_ext.h"

namespace retdec {
namespace llvmir2hll {

/**
 * Returns true if the effects summarised by @a data cannot interact with the
 * call in `lhsVar = <call>` (whose own effects are @a stmtData) when the call
 * is moved past them.
 *
 * That requires no call of any kind (external calls have side effects too,
 * and their order relative to the moved call would swap); no dereference,
 * array, or struct access (the call may read the memory such an access
 * writes); no access to @a lhsVar; and no write to a variable the call
 * statement reads, to a global, or to a variable a pointer may hold the
 * address of (the callee could read any of those).
 */
static bool effectsAreIndependent(ShPtr<ValueData> data,
		ShPtr<Variable> lhsVar, ShPtr<ValueData> stmtData,
		ShPtr<Module> module, ShPtr<ValueAnalysis> va) {
	if (!data) return false;
	if (data->hasCalls() || data->hasDerefs() || data->hasArrayAccesses() ||
			data->hasStructAccesses()) {
		return false;
	}
	if (data->isDirAccessed(lhsVar)) return false;
	for (auto it = data->dir_written_begin(); it != data->dir_written_end(); ++it) {
		const auto &var = *it;
		if (stmtData->isDirRead(var)) return false;
		if (module && module->isGlobalVar(var)) return false;
		if (va->mayBePointed(var)) return false;
	}
	return true;
}

static bool isTransparentFor(ShPtr<Statement> cur, ShPtr<Variable> lhsVar,
		ShPtr<ValueData> stmtData, ShPtr<Module> module,
		ShPtr<ValueAnalysis> va, unsigned depth);

/**
 * Returns true if every statement in the chain starting at @a first is
 * transparent (see isTransparentFor()).
 */
static bool isTransparentChain(ShPtr<Statement> first, ShPtr<Variable> lhsVar,
		ShPtr<ValueData> stmtData, ShPtr<Module> module,
		ShPtr<ValueAnalysis> va, unsigned depth) {
	for (auto cur = first; cur; cur = cur->getSuccessor()) {
		if (!isTransparentFor(cur, lhsVar, stmtData, module, va, depth)) {
			return false;
		}
	}
	return true;
}

/**
 * Returns true if `lhsVar = <call>` may be moved past @a cur without changing
 * what the call computes, whether it runs, or the order of side effects.
 *
 * Plain assignments, variable definitions, and empty statements qualify when
 * their effects are independent of the call. An @c if qualifies when its
 * conditions do and every statement in its clauses does, recursively.
 * Anything else -- loops, switches, returns, breaks, gotos -- is rejected:
 * value analysis of a compound statement covers only its head, and a jump
 * could skip the use and with it the moved call.
 */
static bool isTransparentFor(ShPtr<Statement> cur, ShPtr<Variable> lhsVar,
		ShPtr<ValueData> stmtData, ShPtr<Module> module,
		ShPtr<ValueAnalysis> va, unsigned depth) {
	const unsigned MAX_DEPTH = 4;
	if (isa<EmptyStmt>(cur)) return true;
	if (isa<AssignStmt>(cur) || isa<VarDefStmt>(cur)) {
		return effectsAreIndependent(va->getValueData(cur), lhsVar, stmtData,
			module, va);
	}
	if (auto ifStmt = cast<IfStmt>(cur)) {
		if (depth >= MAX_DEPTH) return false;
		for (auto it = ifStmt->clause_begin(); it != ifStmt->clause_end(); ++it) {
			if (!effectsAreIndependent(va->getValueData(it->first), lhsVar,
					stmtData, module, va)) {
				return false;
			}
			if (!isTransparentChain(it->second, lhsVar, stmtData, module, va,
					depth + 1)) {
				return false;
			}
		}
		return !ifStmt->hasElseClause() ||
			isTransparentChain(ifStmt->getElseClause(), lhsVar, stmtData,
				module, va, depth + 1);
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
 * @param module The module (for global-variable lookups).
 * @param va     Value analysis.
 *
 * @return true if it is safe to propagate.
 */
bool canPropagateThroughStmts(ShPtr<Statement> stmt,
                               ShPtr<Variable> lhsVar,
                               ShPtr<Statement> use,
                               ShPtr<Module> module,
                               ShPtr<ValueAnalysis> va) {
	if (!stmt || !lhsVar || !use || !va) return false;
	auto stmtData = va->getValueData(stmt);
	if (!stmtData) return false;

	// Walk from stmt's successor to use, checking each intervening statement.
	ShPtr<Statement> cur = stmt->getSuccessor();
	unsigned steps = 0;
	const unsigned MAX_STEPS = 16; // Don't walk too far.

	while (cur && cur != use && steps < MAX_STEPS) {
		if (!isTransparentFor(cur, lhsVar, stmtData, module, va, 0)) return false;
		cur = cur->getSuccessor();
		++steps;
	}

	// Did we actually reach `use`?
	if (cur != use) return false;

	// Verify: lhsVar used exactly once in use (same guard as original code).
	auto useData = va->getValueData(use);
	if (!useData) return false;
	return useData->getDirNumOfUses(lhsVar) == 1;
}

} // namespace llvmir2hll
} // namespace retdec
