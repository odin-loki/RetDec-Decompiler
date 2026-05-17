/**
* @file src/llvmir2hll/optimizer/optimizers/while_true_to_ufor_loop_optimizer.cpp
* @brief Implementation of WhileTrueToUForLoopOptimizer.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/optimizer/optimizers/while_true_to_ufor_loop_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/support/expression_negater.h"
#include "retdec/llvmir2hll/utils/ir.h"
#include "retdec/llvmir2hll/utils/loop_optimizer.h"

namespace retdec {
namespace llvmir2hll {

/**
* @brief Constructs a new optimizer.
*
* @param[in] module Module to be optimized.
* @param[in] va Analysis of values.
*
* @par Preconditions
*  - @a module and @a va are non-null
*/
WhileTrueToUForLoopOptimizer::WhileTrueToUForLoopOptimizer(ShPtr<Module> module,
		ShPtr<ValueAnalysis> va):
	FuncOptimizer(module), va(va), canBeOptimized(false) {
		PRECONDITION_NON_NULL(module);
		PRECONDITION_NON_NULL(va);
	}

void WhileTrueToUForLoopOptimizer::doOptimization() {
	if (!va->isInValidState()) {
		va->clearCache();
	}
	FuncOptimizer::doOptimization();

	// Currently, we do not update the used analysis of values (va) during this
	// optimization, so here, at the end of the optimization, we have to put it
	// into an invalid state.
	va->invalidateState();
}

/**
* @brief Tries to replace the given while loop with a universal for loop or,
*        when the exit check is the last statement in the body, a do-while loop.
*/
void WhileTrueToUForLoopOptimizer::tryReplacementWithUForLoop(
		ShPtr<WhileLoopStmt> whileLoop) {
	initializeReplacement(whileLoop);
	bool infoGathered = gatherInfoAboutOptimizedWhileLoop();
	if (!infoGathered) {
		return;
	}

	// Do-while lowering.
	//
	// The IR in this branch does not have a dedicated do-while node, so lower
	//   while true { BODY; if (exit) break; }
	// into
	//   BODY;
	//   while (!exit) { BODY; }
	// which preserves semantics while still simplifying the original shape.
	if (isDoWhileLoop(whileLoop)) {
		auto continueCond = getDoWhileCondition(whileLoop);
		auto firstBody = splittedLoop->beforeLoopEndStmts;
		if (continueCond && firstBody) {
			auto loopBody = cast<Statement>(firstBody->clone());
			if (loopBody) {
				auto loweredLoop = WhileLoopStmt::create(
					continueCond,
					loopBody,
					nullptr,
					whileLoop->getAddress());

				Statement::mergeStatements(firstBody, loweredLoop);
				Statement::replaceStatement(whileLoop, firstBody);
				return;
			}
		}
	}

	// -----------------------------------------------------------------------
	// Universal for-loop fallback (stub kept for future implementation).
	// -----------------------------------------------------------------------

	// Store the last statement of the original loop for later use. Usually,
	// the last empty statement in a "while true" loop contains metadata of the
	// form "continue -> bb". To preserve this piece of information, we store
	// it and use it after the transformation if finished.
	auto lastLoopStmt = getLastEmptyStatement(splittedLoop->afterLoopEndStmts);

	// Perform the conversion and replacement.
	auto forLoop = tryConversionToUForLoop();
	if (!forLoop) {
		return;
	}
	performReplacement(forLoop);

	// Put lastLoopStmt to the end of the new loop.
	if (lastLoopStmt && lastLoopStmt->hasMetadata()) {
		Statement::mergeStatements(forLoop->getBody(), lastLoopStmt);
	}

	removeUselessSucessors(forLoop);
}

/**
* @brief Initialize a new replacement.
*/
void WhileTrueToUForLoopOptimizer::initializeReplacement(ShPtr<WhileLoopStmt> stmt) {
	whileLoop = stmt;
	splittedLoop.reset();
	canBeOptimized = true;
	toRemoveStmts.clear();
}

/**
* @brief Gathers information about the loop.
*
* @return @c true if the information was gathered successfully, @c false otherwise.
*/
bool WhileTrueToUForLoopOptimizer::gatherInfoAboutOptimizedWhileLoop() {
	// We are able to optimize only "while true" loops.
	if (!isWhileTrueLoop(whileLoop)) {
		return false;
	}

	// We have to be enable to split the loop.
	splittedLoop = splitWhileTrueLoop(whileLoop);
	if (!splittedLoop) {
		return false;
	}

	return true;
}

/**
* @brief Tries to convert the "while true" loop into a universal for loop.
*
* The conversion succeeds when `getIndVarInfo` can identify an induction
* variable pattern:
*
*   init_stmt          (before the loop)
*   while True:
*       body
*       if exit_cond:
*           break/return
*       update_stmt
*
* → for (init_expr; !exit_cond; update_expr) { body }
*
* Unlike `WhileTrueToForLoopOptimizer` (which requires an integer `ForLoopStmt`
* with simple ±1 increments), this optimizer emits a `UForLoopStmt` that
* accepts arbitrary init/cond/step expressions.
*
* Returns the null pointer when no conversion is possible.
*/
ShPtr<UForLoopStmt> WhileTrueToUForLoopOptimizer::tryConversionToUForLoop() {
	// Try to detect an induction-variable pattern in the current while loop.
	auto indVarInfo = getIndVarInfo(whileLoop);
	if (!indVarInfo) {
		return {};
	}

	// Extract the init expression from the init statement.
	// The init statement is either an AssignStmt or a VarDefStmt.
	ShPtr<Expression> initExpr;
	if (auto assignInit = cast<AssignStmt>(indVarInfo->initStmt)) {
		initExpr = assignInit->asExpression();
	} else if (auto defInit = cast<VarDefStmt>(indVarInfo->initStmt)) {
		initExpr = defInit->asExpression();
	}
	if (!initExpr) {
		return {};
	}

	// The loop condition is the negation of the exit condition.
	if (!indVarInfo->exitCond) {
		return {};
	}
	auto condExpr = ExpressionNegater::negate(indVarInfo->exitCond);

	// Extract the step expression from the update statement.
	ShPtr<Expression> stepExpr;
	if (auto assignStep = cast<AssignStmt>(indVarInfo->updateStmt)) {
		stepExpr = assignStep->asExpression();
	} else if (auto defStep = cast<VarDefStmt>(indVarInfo->updateStmt)) {
		stepExpr = defStep->asExpression();
	}
	if (!stepExpr) {
		return {};
	}

	// The body is everything before the loop-end check.
	auto body = splittedLoop->beforeLoopEndStmts;
	if (!body) {
		body = EmptyStmt::create(nullptr, whileLoop->getAddress());
	}

	// Remove the init statement from before the loop and the update statement
	// from the loop body (they move into the for-loop header).
	Statement::removeStatement(indVarInfo->initStmt);
	if (indVarInfo->updateBeforeExit) {
		Statement::removeStatement(indVarInfo->updateStmt);
	}
	// Schedule removal of the update statement if it appears after the exit check.
	if (!indVarInfo->updateBeforeExit) {
		toRemoveStmts.insert(indVarInfo->updateStmt);
	}

	return UForLoopStmt::create(
		initExpr, condExpr, stepExpr, body, nullptr, whileLoop->getAddress());
}

/**
* @brief Returns the last empty statement in the given statements.
*/
ShPtr<EmptyStmt> WhileTrueToUForLoopOptimizer::getLastEmptyStatement(
		ShPtr<Statement> stmts) const {
	return cast<EmptyStmt>(Statement::getLastStatement(stmts));
}

/**
* @brief Removes useless successors (if any) of the given loop statement.
*
* If the successors are two consecutive empty statements with metadata, removes
* the first one because it usually contains just an end label of the original
* while loop.
*/
void WhileTrueToUForLoopOptimizer::removeUselessSucessors(
		ShPtr<UForLoopStmt> forLoop) {
	if (!forLoop) {
		return;
	}
	if (auto succ = forLoop->getSuccessor()) {
		if (isa<EmptyStmt>(succ) && succ->hasMetadata()) {
			if (auto succSucc = succ->getSuccessor()) {
				if (isa<EmptyStmt>(succSucc) && succSucc->hasMetadata()) {
					Statement::removeStatement(succ);
				}
			}
		}
	}
}

/**
* @brief Performs the replacement.
*/
void WhileTrueToUForLoopOptimizer::performReplacement(ShPtr<UForLoopStmt> forLoop) {
	Statement::replaceStatement(whileLoop, forLoop);
	removeStatementsToBeRemoved();
}

/**
* @brief Removes statements that are to be removed after a successful
*        optimization.
*/
void WhileTrueToUForLoopOptimizer::removeStatementsToBeRemoved() {
	for (auto stmt : toRemoveStmts) {
		Statement::removeStatement(stmt);
	}
}

void WhileTrueToUForLoopOptimizer::visit(ShPtr<WhileLoopStmt> stmt) {
	visitNestedAndSuccessorStatements(stmt);
	tryReplacementWithUForLoop(stmt);
}

} // namespace llvmir2hll
} // namespace retdec
