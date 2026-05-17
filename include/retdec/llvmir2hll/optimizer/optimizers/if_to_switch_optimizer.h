/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/if_to_switch_optimizer.h
* @brief Optimizes if statements to switch statements.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_IF_TO_SWITCH_OPTIMIZER_H
#define RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_IF_TO_SWITCH_OPTIMIZER_H

#include <cstdint>

#include "retdec/llvmir2hll/optimizer/func_optimizer.h"

namespace retdec {
namespace llvmir2hll {

class ValueAnalysis;

/**
* @brief Optimizes if statements to switch statements.
*
* For example,
* @code
* if (a == 5) {
*     c = 5;
* } else if (a == 6) {
*     c = 6;
* } else {
*     c = 3;
* }
* @endcode
* can be optimized to
* @code
* switch (a) {
*     case 5: c = 5; break;
*     case 6: c = 6; break;
*     default: c = 3; break;
* }
* @endcode
*
* In the following cases, this optimization is not possible:
*   -# When the if statement is a simple if without else if clauses.
*   -# When the control expression is not the same in all else if clauses.
*   -# When EqOpExpr is not in clauses conditions.
*   -# When at least one operand of EqOpExpr is not ConstInt.
*   -# When the control expression contains a function call, dereference, or
*      array.
*   -# When if statement has break statement in his body don't optimize because
*      this break can break out some loop and after place it into switch this
*      break jump out only switch statement. In some special cases like this:
*      @code
*      if (a == 5) {
*          while (true) {
*              break;
*          }
*      else if (a == 6) {
*          statement;
*      }
*      @endcode
*      This break don't cause this problem. But for simplification of
*      optimization is this don't optimized.
* Instances of this class have reference object semantics.
*
* This is a concrete optimizer which should not be subclassed.
*/
class IfToSwitchOptimizer final: public FuncOptimizer {
public:
	IfToSwitchOptimizer(ShPtr<Module> module, ShPtr<ValueAnalysis> va);

	virtual std::string getId() const override { return "IfToSwitch"; }

private:
	/// @name Visitor Interface
	/// @{
	using OrderedAllVisitor::visit;
	virtual void visit(ShPtr<IfStmt> stmt) override;

	void appendBreakStmtIfNeeded(ShPtr<Statement> stmt);
	ShPtr<Expression> getControlExprIfConvertibleToSwitch(ShPtr<IfStmt> ifStmt);
	/// Else-if chain @c v < 1, @c v < 2, … @c v < n (same @c v) → @c switch(v) cases @c 0..n-1.
	ShPtr<Expression> getControlExprIfConvertibleToLtUpperBoundSwitch(
		ShPtr<IfStmt> ifStmt);
	/// Else-if chain @c v <= 0, @c v <= 1, … @c v <= n-1 (same @c v) → @c switch(v) cases @c 0..n-1.
	ShPtr<Expression> getControlExprIfConvertibleToLeUpperBoundSwitch(
		ShPtr<IfStmt> ifStmt);
	/// Else-if @c v >= k, @c v >= k-1, … @c v >= 1 (same @c v) + @c else → cases @c k..0.
	ShPtr<Expression> getControlExprIfConvertibleToGeLowerBoundSwitch(
		ShPtr<IfStmt> ifStmt);
	/// Else-if @c v > k-1, … @c v > 0 (same @c v) + @c else → cases @c k..0.
	ShPtr<Expression> getControlExprIfConvertibleToGtLowerBoundSwitch(
		ShPtr<IfStmt> ifStmt);
	ShPtr<Expression> getNextOpIfSecondOneIsConstInt(ShPtr<EqOpExpr>
		eqOpExpr);
	void convertIfStmtToSwitchStmt(ShPtr<IfStmt> ifStmt, ShPtr<Expression>
		controlExpr);
	/// Shared lowering for @c getControlExprIfConvertibleToLt/LeUpperBoundSwitch.
	void convertDenseIntegerPartitionIfChainToSwitchStmt(ShPtr<IfStmt> ifStmt,
		ShPtr<Expression> controlExpr);
	/// Dense descending cases @c k..1 + default (used by @c >= and @c > chains).
	void convertDescendingIntegerCaseIfChainToSwitchStmt(ShPtr<IfStmt> ifStmt,
		ShPtr<Expression> controlExpr);
	/// @c if (v < n) { dense @c v==0..n-1 chain }  →  @c switch(v) (hoist).
	bool tryConvertOuterLtWithInnerDenseEqChain(ShPtr<IfStmt> outerIf);
	/// @c if (v <= n-1) { dense @c v==0..n-1 chain }  →  @c switch(v) (hoist).
	bool tryConvertOuterLeWithInnerDenseEqChain(ShPtr<IfStmt> outerIf);
	/// @c if (v > L) { consecutive @c v==L+1 … }  →  @c switch(v) (hoist).
	bool tryConvertOuterGtWithInnerDenseEqChain(ShPtr<IfStmt> outerIf);
	/// @c if (v >= F) { consecutive @c v==F … }  →  @c switch(v) (hoist).
	bool tryConvertOuterGeWithInnerDenseEqChain(ShPtr<IfStmt> outerIf);
	/// Six-level nested compare trees (uniform comparator per tree).
	bool tryConvertLtWithSixLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertLeWithSixLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGtWithSixLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGeWithSixLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	/// Five-level nested compare trees (one deeper than four-level).
	bool tryConvertLtWithFiveLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertLeWithFiveLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGtWithFiveLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGeWithFiveLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	/// Four-level nested compare trees (same shape as three-level, one extra split).
	bool tryConvertLtWithFourLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertLeWithFourLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGtWithFourLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGeWithFourLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v<M0){ if (v<M1){ if (v<M2){ A } @c else { B } } @c else { C } } @c else { D }  →  @c switch(v).
	bool tryConvertLtWithThreeLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v < M) { if (v < m) { A.. } @c else { … M-1 } } @c else { M… }  →  @c switch(v).
	bool tryConvertLtWithNestedLtSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v<=U0){ if (v<=U1){ if (v<=U2){ A } @c else { B } } @c else { C } } @c else { D }  →  @c switch(v).
	bool tryConvertLeWithThreeLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v <= U) { if (v <= u) { … u } @c else { u+1..U } } @c else { U+1… }  →  @c switch(v).
	bool tryConvertLeWithNestedLeSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v < M) { if (v <= u) { … u } @c else { u+1..M-1 } } @c else { M… }  →  @c switch(v).
	bool tryConvertLtWithNestedLeSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v <= U) { if (v < m) { … m-1 } @c else { m..U } } @c else { U+1… }  →  @c switch(v).
	bool tryConvertLeWithNestedLtSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v>L0){ if (v>L1){ if (v>L2){ A } @c else { B } } @c else { C } } @c else { D }  →  @c switch(v).
	bool tryConvertGtWithThreeLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v > LO) { if (v > LI) { LI+1… } @c else { LO+1..LI } } @c else { … LO }  →  @c switch(v).
	bool tryConvertGtWithNestedGtSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v>=F0){ if (v>=F1){ if (v>=F2){ A } @c else { B } } @c else { C } } @c else { D }  →  @c switch(v).
	bool tryConvertGeWithThreeLevelNestedSplitInThen(ShPtr<IfStmt> outerIf);
	/// Three-level nested trees with mixed comparator at the innermost split only.
	bool tryConvertLtWithThreeLevelNestedInnerLeSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertLeWithThreeLevelNestedInnerLtSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGtWithThreeLevelNestedInnerGeSplitInThen(ShPtr<IfStmt> outerIf);
	bool tryConvertGeWithThreeLevelNestedInnerGtSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v >= LO) { if (v >= H) { H… } @c else { LO..H-1 } } @c else { … LO-1 }  →  @c switch(v).
	bool tryConvertGeWithNestedGeSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v > LO) { if (v >= H) { H… } @c else { LO+1..H-1 } } @c else { … LO }  →  @c switch(v).
	bool tryConvertGtWithNestedGeSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v >= LO) { if (v > H) { H+1… } @c else { LO..H } } @c else { … LO-1 }  →  @c switch(v).
	bool tryConvertGeWithNestedGtSplitInThen(ShPtr<IfStmt> outerIf);
	/// @c if (v < mid) { consecutive @c A.. } @c else { consecutive @c mid… }  →  @c switch(v) (@c mid == A+nL).
	bool tryConvertLtSplitTwoConsecutiveInnerEqChains(ShPtr<IfStmt> outerIf);
	/// @c if (v <= ub) { consecutive … @c ub } @c else { consecutive @c ub+1… }  →  @c switch(v).
	bool tryConvertLeSplitTwoConsecutiveInnerEqChains(ShPtr<IfStmt> outerIf);
	/// @c if (v > R) { consecutive @c R+1… } @c else { consecutive … @c R }  →  @c switch(v).
	bool tryConvertGtSplitTwoConsecutiveInnerEqChains(ShPtr<IfStmt> outerIf);
	/// @c if (v >= F) { consecutive @c F… } @c else { consecutive … @c F-1 }  →  @c switch(v).
	bool tryConvertGeSplitTwoConsecutiveInnerEqChains(ShPtr<IfStmt> outerIf);
	/// Consecutive @c if (v==k) (no @c else if) chain → @c switch(v).
	void tryConvertSequentialIfChainToSwitch(ShPtr<IfStmt> firstIf);
	/// Inner @c if is @c v==base, @c v==base+1, … in order, no @c else (int64 path).
	bool innerIsDenseEqChainConsecutive(ShPtr<IfStmt> inner,
		ShPtr<Expression> ctrl, int64_t &outFirst, unsigned &outN);
	/// Inner @c if is @c v==0 … @c v==n-1 in order, no @c else.
	bool innerIsDenseEqChainZeroToNMinus1(ShPtr<IfStmt> inner,
		ShPtr<Expression> ctrl, unsigned n);

	/// Analysis of values.
	ShPtr<ValueAnalysis> va;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
