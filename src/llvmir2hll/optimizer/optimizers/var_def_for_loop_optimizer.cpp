/**
* @file src/llvmir2hll/optimizer/optimizers/var_def_for_loop_optimizer.cpp
* @brief Implementation of VarDefForLoopOptimizer.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/expression.h"
#include "retdec/llvmir2hll/optimizer/optimizers/var_def_for_loop_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/utils/container.h"

#include <algorithm>

using retdec::utils::hasItem;

namespace retdec {
namespace llvmir2hll {

/**
* @brief Constructs a new optimizer.
*
* @param[in] module Module to be optimized.
*
* @par Preconditions
*  - @a module is non-null
*/
VarDefForLoopOptimizer::VarDefForLoopOptimizer(ShPtr<Module> module):
	FuncOptimizer(module), indVars() {
		PRECONDITION_NON_NULL(module);
	}

void VarDefForLoopOptimizer::runOnFunction(ShPtr<Function> func) {
	// Obtain all induction variables for func.
	indVars.clear();
	FuncOptimizer::runOnFunction(func);

	// Remove VarDefStmts for each induction variable from the beginning of
	// func.
	ShPtr<Statement> stmt(func->getBody());
	while (isa<VarDefStmt>(stmt)) {
		ShPtr<VarDefStmt> varDefStmt(cast<VarDefStmt>(stmt));
		if (varDefStmt->getInitializer()) {
			// We have reached a VarDefStmt with an initializer. This means we
			// are done as all VarDefStmts introduced to the beginning of a
			// function lack an initializer.
			return;
		}

		// We need to store the successor before removing the statement since
		// removeStatement() clears the successor.
		ShPtr<Statement> stmtSucc(stmt->getSuccessor());

		// The writer declares the induction variable in the loop header,
		// which ends its scope at the loop.  Keep the function-level
		// definition when the variable is also read outside every loop that
		// owns it, e.g. `return v2 + i;` after a counted loop (a64 -O1
		// f_rec emitted `'i' undeclared`).
		if (hasItem(indVars, varDefStmt->getVar())
				&& !hasItem(usedOutside, varDefStmt->getVar())) {
			// There should be no debug comments attached to VarDefStmts at the
			// beginning of func, so we can use directly
			// Statement::removeStatement() (not something from
			// optimizer utilities).
			Statement::removeStatement(stmt);
		}

		stmt = stmtSucc;
	}
}

void VarDefForLoopOptimizer::visit(ShPtr<ForLoopStmt> stmt) {
	indVars.insert(stmt->getIndVar());
	lastStmt = stmt;
	loopVars.push_back(stmt->getIndVar());
	stmt->getIndVar()->accept(this);
	stmt->getStartValue()->accept(this);
	stmt->getEndCond()->accept(this);
	stmt->getStep()->accept(this);
	visitStmt(stmt->getBody());
	loopVars.pop_back();
	if (stmt->hasSuccessor()) {
		visitStmt(stmt->getSuccessor());
	}
}

void VarDefForLoopOptimizer::visit(ShPtr<VarDefStmt> stmt) {
	// The definition itself is not a use.
	lastStmt = stmt;
	if (auto init = stmt->getInitializer()) {
		init->accept(this);
	}
	if (stmt->hasSuccessor()) {
		visitStmt(stmt->getSuccessor());
	}
}

void VarDefForLoopOptimizer::visit(ShPtr<Variable> var) {
	if (std::find(loopVars.begin(), loopVars.end(), var) == loopVars.end()) {
		usedOutside.insert(var);
	}
}

} // namespace llvmir2hll
} // namespace retdec
