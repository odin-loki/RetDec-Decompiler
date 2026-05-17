/**
* @file include/retdec/llvmir2hll/utils/loop_optimizer.h
* @brief Utilities for optimizers.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_UTILS_LOOP_OPTIMIZER_H
#define RETDEC_LLVMIR2HLL_UTILS_LOOP_OPTIMIZER_H

#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"

namespace retdec {
namespace llvmir2hll {

/**
* @brief A representation of a "while true" loop.
*
* Consider a general "while true" loop:
* @code
* while True:
*     // Statements (1)
*     if cond: // Loop end (2)
*         either nothing or a variable assignment
*         break or return
*     // Statements (3)
* @endcode
*
* This class represents a loop splitted into the parts (1) through (3).
*/
struct SplittedWhileTrueLoop {
	/// Statements before the loop's end -- corresponds to (1) in the class
	/// description.
	ShPtr<Statement> beforeLoopEndStmts;

	/// The loop's end -- corresponds to (2) in the class description.
	ShPtr<IfStmt> loopEnd;

	/// Statements after the loop's end -- corresponds to (3) in the class
	/// description.
	ShPtr<Statement> afterLoopEndStmts;
};

/**
* @brief Information about the induction variable of a "while true" loop.
*
* Consider a "while true" loop that can be optimized into a for loop:
* @code
* ...
* i = 0 // (1)
* ...
* while True:
*     ...
*     if cond: // (2)
*         break or return
*     i = i + 1 // (3)
* @endcode
*/
struct IndVarInfo {
	IndVarInfo(
			ShPtr<Statement> initStmt,
			ShPtr<Variable> indVar,
			ShPtr<Expression> exitCond,
			ShPtr<Statement> updateStmt,
			bool updateBeforeExit)
			: initStmt(initStmt)
			, indVar(indVar)
			, exitCond(exitCond)
			, updateStmt(updateStmt)
			, updateBeforeExit(updateBeforeExit)
	{}

	/// Initialization of the induction variable (either a definition or an
	/// assignment) -- corresponds to (1) in the class description.
	ShPtr<Statement> initStmt;

	/// Induction variable -- corresponds to (1) in the class description.
	ShPtr<Variable> indVar;

	/// Exit condition -- corresponds to (2) in the class description.
	ShPtr<Expression> exitCond;

	/// Update of the induction variable -- corresponds to (3) in the class
	/// description.
	ShPtr<Statement> updateStmt;

	/// Is an update statement before exit condition?
	bool updateBeforeExit;
};

bool isLoopEnd(ShPtr<Statement> stmt);
ShPtr<Expression> getExitCondition(ShPtr<Statement> loopEnd);
ShPtr<SplittedWhileTrueLoop> splitWhileTrueLoop(
		ShPtr<WhileLoopStmt> stmt,
		ShPtr<IndVarInfo> indVarInfo = nullptr);
ShPtr<IndVarInfo> getIndVarInfo(ShPtr<WhileLoopStmt> stmt);

/// @name Do-while detection
/// @{

/**
* @brief Returns @c true if @a stmt is a "while true" loop whose exit check
*        is the last meaningful statement in the body (a do-while pattern).
*
* A do-while pattern:
* @code
* while True:
*     // body statements
*     if cond:   // exit check — always last
*         break or return
* @endcode
* maps directly to:
* @code
* do {
*     // body statements
* } while (!cond);
* @endcode
*/
bool isDoWhileLoop(ShPtr<WhileLoopStmt> stmt);

/**
* @brief Returns the continuation condition for the do-while loop equivalent
*        of @a stmt.
*
* The exit condition of the inner @c if is negated so it expresses
* "keep looping" rather than "exit".  Returns the null pointer if @a stmt is
* not a do-while pattern.
*/
ShPtr<Expression> getDoWhileCondition(ShPtr<WhileLoopStmt> stmt);

/// @}

} // namespace llvmir2hll
} // namespace retdec

#endif
