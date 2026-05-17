/**
* @file src/llvmir2hll/optimizer/optimizers/if_to_switch_optimizer.cpp
* @brief Implementation of IfToSwitchOptimizer.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include "retdec/llvmir2hll/analysis/break_in_if_analysis.h"
#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_op_expr.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "retdec/llvmir2hll/optimizer/optimizers/if_to_switch_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/support/types.h"

#include <cstdint>
#include <iterator>
#include <limits>
#include <set>

namespace retdec {
namespace llvmir2hll {

namespace {

/// Narrow @c APSInt to @c int64_t for compare-tree / hoist matching (conservative).
bool apsIntToInt64(const llvm::APSInt &v, int64_t *out) {
	if (v.getBitWidth() > 64) {
		return false;
	}
	if (v.isSigned()) {
		*out = v.getSExtValue();
	} else {
		const uint64_t z = v.getZExtValue();
		if (z > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			return false;
		}
		*out = static_cast<int64_t>(z);
	}
	return true;
}

} // namespace

/**
* @brief Constructs a new optimizer.
*
* @param[in] module Module to be optimized.
* @param[in] va Analysis of values.
*
* @par Preconditions
*  - @a module and @a va are non-null
*/
IfToSwitchOptimizer::IfToSwitchOptimizer(ShPtr<Module> module,
		ShPtr<ValueAnalysis> va): FuncOptimizer(module), va(va) {
	PRECONDITION_NON_NULL(module);
	PRECONDITION_NON_NULL(va);
}

void IfToSwitchOptimizer::visit(ShPtr<IfStmt> stmt) {
	// First, try to convert an else-if chain rooted at this statement.
	ShPtr<Expression> controlExpr(getControlExprIfConvertibleToSwitch(stmt));
	if (controlExpr) {
		convertIfStmtToSwitchStmt(stmt, controlExpr);
		return;
	}

	// Else-if chain: v < 1, v < 2, … v < n  →  switch (v) with cases 0..n-1
	// (common lowered form for small dense integer dispatch / compare trees).
	ShPtr<Expression> ltControl(
		getControlExprIfConvertibleToLtUpperBoundSwitch(stmt));
	if (ltControl) {
		convertDenseIntegerPartitionIfChainToSwitchStmt(stmt, ltControl);
		return;
	}

	// Else-if chain: v <= 0, v <= 1, … v <= n-1  →  same switch shape as above.
	ShPtr<Expression> leControl(
		getControlExprIfConvertibleToLeUpperBoundSwitch(stmt));
	if (leControl) {
		convertDenseIntegerPartitionIfChainToSwitchStmt(stmt, leControl);
		return;
	}

	// Else-if: v >= k, v >= k-1, … v >= 1  +  else  →  cases k..0
	ShPtr<Expression> geControl(
		getControlExprIfConvertibleToGeLowerBoundSwitch(stmt));
	if (geControl) {
		convertDescendingIntegerCaseIfChainToSwitchStmt(stmt, geControl);
		return;
	}

	// Else-if: v > k-1, … v > 0  +  else  →  same case mapping (strict >)
	ShPtr<Expression> gtControl(
		getControlExprIfConvertibleToGtLowerBoundSwitch(stmt));
	if (gtControl) {
		convertDescendingIntegerCaseIfChainToSwitchStmt(stmt, gtControl);
		return;
	}

	// if (v < n) { if (v==0)... else if (v==n-1)... }  →  switch (v)
	if (tryConvertOuterLtWithInnerDenseEqChain(stmt)) {
		return;
	}

	// if (v <= n-1) { ... same inner ... }  →  switch (v)
	if (tryConvertOuterLeWithInnerDenseEqChain(stmt)) {
		return;
	}

	// if (v > L) { v==L+1 … }  /  if (v >= F) { v==F … }  →  switch (v)
	if (tryConvertOuterGtWithInnerDenseEqChain(stmt)) {
		return;
	}
	if (tryConvertOuterGeWithInnerDenseEqChain(stmt)) {
		return;
	}

	// Six-level nested compare trees before five-level.
	if (tryConvertLtWithSixLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertLeWithSixLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGtWithSixLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGeWithSixLevelNestedSplitInThen(stmt)) {
		return;
	}

	// Five-level nested compare trees before four-level.
	if (tryConvertLtWithFiveLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertLeWithFiveLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGtWithFiveLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGeWithFiveLevelNestedSplitInThen(stmt)) {
		return;
	}

	// Four-level nested compare trees before three-level.
	if (tryConvertLtWithFourLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertLeWithFourLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGtWithFourLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGeWithFourLevelNestedSplitInThen(stmt)) {
		return;
	}

	// Three-level nested Lt (compare tree) before two-level.
	if (tryConvertLtWithThreeLevelNestedSplitInThen(stmt)) {
		return;
	}

	// Three-level nested Le / Gt / Ge before two-level.
	if (tryConvertLeWithThreeLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGtWithThreeLevelNestedSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGeWithThreeLevelNestedSplitInThen(stmt)) {
		return;
	}

	// Three-level with mixed innermost comparator (Lt/Le and Gt/Ge).
	if (tryConvertLtWithThreeLevelNestedInnerLeSplitInThen(stmt)) {
		return;
	}
	if (tryConvertLeWithThreeLevelNestedInnerLtSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGtWithThreeLevelNestedInnerGeSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGeWithThreeLevelNestedInnerGtSplitInThen(stmt)) {
		return;
	}

	// if (v < M) { if (v < m) { … } else { … } } else { … }  (nested Lt)
	if (tryConvertLtWithNestedLtSplitInThen(stmt)) {
		return;
	}

	// if (v <= U) { if (v <= u) { … } else { … } } else { … }  (nested Le)
	if (tryConvertLeWithNestedLeSplitInThen(stmt)) {
		return;
	}

	// Mixed: outer Lt with inner Le, outer Le with inner Lt.
	if (tryConvertLtWithNestedLeSplitInThen(stmt)) {
		return;
	}
	if (tryConvertLeWithNestedLtSplitInThen(stmt)) {
		return;
	}

	// if (v > LO) { if (v > LI) { … } else { … } } else { … }  (nested Gt)
	if (tryConvertGtWithNestedGtSplitInThen(stmt)) {
		return;
	}

	// if (v >= LO) { if (v >= H) { … } else { … } } else { … }  (nested Ge)
	if (tryConvertGeWithNestedGeSplitInThen(stmt)) {
		return;
	}

	// Mixed: outer Gt with inner Ge, outer Ge with inner Gt.
	if (tryConvertGtWithNestedGeSplitInThen(stmt)) {
		return;
	}
	if (tryConvertGeWithNestedGtSplitInThen(stmt)) {
		return;
	}

	// if (v < mid) { … } else { … }  /  <= /  >  one-level BST slices
	if (tryConvertLtSplitTwoConsecutiveInnerEqChains(stmt)) {
		return;
	}
	if (tryConvertLeSplitTwoConsecutiveInnerEqChains(stmt)) {
		return;
	}
	if (tryConvertGtSplitTwoConsecutiveInnerEqChains(stmt)) {
		return;
	}
	if (tryConvertGeSplitTwoConsecutiveInnerEqChains(stmt)) {
		return;
	}

	// Next, try to convert a sequential chain of single-clause if
	// statements on the same variable that immediately follow each other.
	// This pattern arises from compiled jump tables:
	//   if (v == 0) { ... }
	//   if (v == 1) { ... }
	//   if (v == 2) { ... }
	tryConvertSequentialIfChainToSwitch(stmt);
}

/**
* @brief Analyze if IfStmt can be optimized to SwitchStmt and if yes,
*        return control expression.
*
* @param[in] ifStmt IfStmt to analyse.
*
* @return Control expression if @a stmt can be optimized, otherwise the null
*         pointer
*/
ShPtr<Expression> IfToSwitchOptimizer::getControlExprIfConvertibleToSwitch(
		ShPtr<IfStmt> ifStmt) {
	if (!ifStmt->hasElseIfClauses()) {
		// Don't optimize simple if statements without else if clauses.
		return ShPtr<Expression>();
	}

	if (BreakInIfAnalysis::hasBreakStmt(ifStmt)) {
		return ShPtr<Expression>();
	}

	ShPtr<Expression> controlExpr;
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e; ++i) {
		ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
		if (!eqOpExpr) {
			return ShPtr<Expression>();
		}

		ShPtr<Expression> exprToCompareWithControlExpr(
			getNextOpIfSecondOneIsConstInt(eqOpExpr));
		if (!exprToCompareWithControlExpr) {
			return ShPtr<Expression>();
		}

		if (!controlExpr) {
			controlExpr = exprToCompareWithControlExpr;
		} else if (!controlExpr->isEqualTo(exprToCompareWithControlExpr)) {
			return ShPtr<Expression>();
		}

		ShPtr<ValueData> exprData(va->getValueData(exprToCompareWithControlExpr));
		if (exprData->hasCalls() || exprData->hasArrayAccesses() ||
				exprData->hasDerefs()) {
			return ShPtr<Expression>();
		}
	}

	return controlExpr;
}

ShPtr<Expression> IfToSwitchOptimizer::getControlExprIfConvertibleToLtUpperBoundSwitch(
		ShPtr<IfStmt> ifStmt) {
	if (!ifStmt->hasElseIfClauses()) {
		return ShPtr<Expression>();
	}
	if (BreakInIfAnalysis::hasBreakStmt(ifStmt)) {
		return ShPtr<Expression>();
	}

	ShPtr<Expression> controlExpr;
	int clauseIdx = 0;
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e;
			++i, ++clauseIdx) {
		ShPtr<LtOpExpr> lt(cast<LtOpExpr>(i->first));
		if (!lt) {
			return ShPtr<Expression>();
		}
		// Require (control < ConstInt); not (ConstInt < control).
		if (isa<ConstInt>(lt->getFirstOperand())) {
			return ShPtr<Expression>();
		}
		ShPtr<ConstInt> bound(cast<ConstInt>(lt->getSecondOperand()));
		if (!bound) {
			return ShPtr<Expression>();
		}
		ShPtr<Expression> lhs(lt->getFirstOperand());

		const llvm::APSInt &bv(bound->getValue());
		const int64_t expected = static_cast<int64_t>(clauseIdx) + 1;
		if (bv.getBitWidth() > 64) {
			return ShPtr<Expression>();
		}
		if (bv.isSigned()) {
			if (bv.getSExtValue() != expected) {
				return ShPtr<Expression>();
			}
		} else if (bv.getZExtValue() != static_cast<uint64_t>(expected)) {
			return ShPtr<Expression>();
		}

		if (!controlExpr) {
			controlExpr = lhs;
		} else if (!controlExpr->isEqualTo(lhs)) {
			return ShPtr<Expression>();
		}

		ShPtr<ValueData> exprData(va->getValueData(controlExpr));
		if (exprData->hasCalls() || exprData->hasArrayAccesses() ||
				exprData->hasDerefs()) {
			return ShPtr<Expression>();
		}
	}

	return controlExpr;
}

ShPtr<Expression> IfToSwitchOptimizer::getControlExprIfConvertibleToLeUpperBoundSwitch(
		ShPtr<IfStmt> ifStmt) {
	if (!ifStmt->hasElseIfClauses()) {
		return ShPtr<Expression>();
	}
	if (BreakInIfAnalysis::hasBreakStmt(ifStmt)) {
		return ShPtr<Expression>();
	}

	ShPtr<Expression> controlExpr;
	bool haveVariant = false;
	LtEqOpExpr::Variant cmpVariant = LtEqOpExpr::Variant::SCmp;
	int clauseIdx = 0;
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e;
			++i, ++clauseIdx) {
		ShPtr<LtEqOpExpr> le(cast<LtEqOpExpr>(i->first));
		if (!le) {
			return ShPtr<Expression>();
		}
		if (!haveVariant) {
			cmpVariant = le->getVariant();
			haveVariant = true;
		} else if (le->getVariant() != cmpVariant) {
			return ShPtr<Expression>();
		}
		if (isa<ConstInt>(le->getFirstOperand())) {
			return ShPtr<Expression>();
		}
		ShPtr<ConstInt> bound(cast<ConstInt>(le->getSecondOperand()));
		if (!bound) {
			return ShPtr<Expression>();
		}
		ShPtr<Expression> lhs(le->getFirstOperand());

		const llvm::APSInt &bv(bound->getValue());
		const int64_t expected = static_cast<int64_t>(clauseIdx);
		if (bv.getBitWidth() > 64) {
			return ShPtr<Expression>();
		}
		if (bv.isSigned()) {
			if (bv.getSExtValue() != expected) {
				return ShPtr<Expression>();
			}
		} else if (bv.getZExtValue() != static_cast<uint64_t>(expected)) {
			return ShPtr<Expression>();
		}

		if (!controlExpr) {
			controlExpr = lhs;
		} else if (!controlExpr->isEqualTo(lhs)) {
			return ShPtr<Expression>();
		}

		ShPtr<ValueData> exprData(va->getValueData(controlExpr));
		if (exprData->hasCalls() || exprData->hasArrayAccesses() ||
				exprData->hasDerefs()) {
			return ShPtr<Expression>();
		}
	}

	return controlExpr;
}

ShPtr<Expression> IfToSwitchOptimizer::getControlExprIfConvertibleToGeLowerBoundSwitch(
		ShPtr<IfStmt> ifStmt) {
	if (!ifStmt->hasElseClause() || !ifStmt->hasIfClause()) {
		return ShPtr<Expression>();
	}
	if (BreakInIfAnalysis::hasBreakStmt(ifStmt)) {
		return ShPtr<Expression>();
	}

	const int k = static_cast<int>(
		std::distance(ifStmt->clause_begin(), ifStmt->clause_end()));
	if (k < 1) {
		return ShPtr<Expression>();
	}

	ShPtr<Expression> controlExpr;
	bool haveVariant = false;
	GtEqOpExpr::Variant cmpVariant = GtEqOpExpr::Variant::SCmp;
	int clauseIdx = 0;
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e;
			++i, ++clauseIdx) {
		ShPtr<GtEqOpExpr> ge(cast<GtEqOpExpr>(i->first));
		if (!ge) {
			return ShPtr<Expression>();
		}
		if (!haveVariant) {
			cmpVariant = ge->getVariant();
			haveVariant = true;
		} else if (ge->getVariant() != cmpVariant) {
			return ShPtr<Expression>();
		}
		if (isa<ConstInt>(ge->getFirstOperand())) {
			return ShPtr<Expression>();
		}
		ShPtr<ConstInt> bound(cast<ConstInt>(ge->getSecondOperand()));
		if (!bound) {
			return ShPtr<Expression>();
		}
		ShPtr<Expression> lhs(ge->getFirstOperand());

		const int64_t expected = static_cast<int64_t>(k - clauseIdx);
		const llvm::APSInt &bv(bound->getValue());
		if (bv.getBitWidth() > 64) {
			return ShPtr<Expression>();
		}
		if (bv.isSigned()) {
			if (bv.getSExtValue() != expected) {
				return ShPtr<Expression>();
			}
		} else if (bv.getZExtValue() != static_cast<uint64_t>(expected)) {
			return ShPtr<Expression>();
		}

		if (!controlExpr) {
			controlExpr = lhs;
		} else if (!controlExpr->isEqualTo(lhs)) {
			return ShPtr<Expression>();
		}

		ShPtr<ValueData> exprData(va->getValueData(controlExpr));
		if (exprData->hasCalls() || exprData->hasArrayAccesses() ||
				exprData->hasDerefs()) {
			return ShPtr<Expression>();
		}
	}

	return controlExpr;
}

ShPtr<Expression> IfToSwitchOptimizer::getControlExprIfConvertibleToGtLowerBoundSwitch(
		ShPtr<IfStmt> ifStmt) {
	if (!ifStmt->hasElseClause() || !ifStmt->hasIfClause()) {
		return ShPtr<Expression>();
	}
	if (BreakInIfAnalysis::hasBreakStmt(ifStmt)) {
		return ShPtr<Expression>();
	}

	const int k = static_cast<int>(
		std::distance(ifStmt->clause_begin(), ifStmt->clause_end()));
	if (k < 1) {
		return ShPtr<Expression>();
	}

	ShPtr<Expression> controlExpr;
	bool haveVariant = false;
	GtOpExpr::Variant cmpVariant = GtOpExpr::Variant::SCmp;
	int clauseIdx = 0;
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e;
			++i, ++clauseIdx) {
		ShPtr<GtOpExpr> gt(cast<GtOpExpr>(i->first));
		if (!gt) {
			return ShPtr<Expression>();
		}
		if (!haveVariant) {
			cmpVariant = gt->getVariant();
			haveVariant = true;
		} else if (gt->getVariant() != cmpVariant) {
			return ShPtr<Expression>();
		}
		if (isa<ConstInt>(gt->getFirstOperand())) {
			return ShPtr<Expression>();
		}
		ShPtr<ConstInt> bound(cast<ConstInt>(gt->getSecondOperand()));
		if (!bound) {
			return ShPtr<Expression>();
		}
		ShPtr<Expression> lhs(gt->getFirstOperand());

		const int64_t expected = static_cast<int64_t>(k - 1 - clauseIdx);
		const llvm::APSInt &bv(bound->getValue());
		if (bv.getBitWidth() > 64) {
			return ShPtr<Expression>();
		}
		if (bv.isSigned()) {
			if (bv.getSExtValue() != expected) {
				return ShPtr<Expression>();
			}
		} else if (bv.getZExtValue() != static_cast<uint64_t>(expected)) {
			return ShPtr<Expression>();
		}

		if (!controlExpr) {
			controlExpr = lhs;
		} else if (!controlExpr->isEqualTo(lhs)) {
			return ShPtr<Expression>();
		}

		ShPtr<ValueData> exprData(va->getValueData(controlExpr));
		if (exprData->hasCalls() || exprData->hasArrayAccesses() ||
				exprData->hasDerefs()) {
			return ShPtr<Expression>();
		}
	}

	return controlExpr;
}

void IfToSwitchOptimizer::convertDescendingIntegerCaseIfChainToSwitchStmt(
		ShPtr<IfStmt> ifStmt,
		ShPtr<Expression> controlExpr) {
	unsigned caseBitWidth = 64;
	if (ShPtr<IntType> it = cast<IntType>(controlExpr->getType())) {
		caseBitWidth = it->getSize();
	}

	const int k = static_cast<int>(
		std::distance(ifStmt->clause_begin(), ifStmt->clause_end()));

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(controlExpr, nullptr, ifStmt->getAddress()));

	int caseVal = k;
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e;
			++i, --caseVal) {
		appendBreakStmtIfNeeded(Statement::getLastStatement(i->second));
		switchStmt->addClause(ConstInt::create(caseVal, caseBitWidth), i->second);
	}

	switchStmt->addDefaultClause(ifStmt->getElseClause());
	appendBreakStmtIfNeeded(ifStmt->getElseClause());

	Statement::replaceStatement(ifStmt, switchStmt);
}

void IfToSwitchOptimizer::convertDenseIntegerPartitionIfChainToSwitchStmt(
		ShPtr<IfStmt> ifStmt,
		ShPtr<Expression> controlExpr) {
	unsigned caseBitWidth = 64;
	if (ShPtr<IntType> it = cast<IntType>(controlExpr->getType())) {
		caseBitWidth = it->getSize();
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(controlExpr, nullptr, ifStmt->getAddress()));

	int caseVal = 0;
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e;
			++i, ++caseVal) {
		appendBreakStmtIfNeeded(Statement::getLastStatement(i->second));
		switchStmt->addClause(ConstInt::create(caseVal, caseBitWidth), i->second);
	}

	if (ifStmt->hasElseClause()) {
		switchStmt->addDefaultClause(ifStmt->getElseClause());
		appendBreakStmtIfNeeded(ifStmt->getElseClause());
	}

	Statement::replaceStatement(ifStmt, switchStmt);
}

/**
* @brief Optimize @a ifStmt to SwitchStmt.
*
* @param[in] ifStmt IfStmt to optimize.
* @param[in] controlExpr Control expression of new SwitchStmt.
*/
void IfToSwitchOptimizer::convertIfStmtToSwitchStmt(ShPtr<IfStmt> ifStmt,
		ShPtr<Expression> controlExpr) {
	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(controlExpr, nullptr, ifStmt->getAddress()));
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e; ++i) {
		appendBreakStmtIfNeeded(Statement::getLastStatement(i->second));

		ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
		if (eqOpExpr->getFirstOperand()->isEqualTo(controlExpr)) {
			switchStmt->addClause(eqOpExpr->getSecondOperand(), i->second);
		} else {
			switchStmt->addClause(eqOpExpr->getFirstOperand(), i->second);
		}
	}

	if (ifStmt->hasElseClause()) {
		switchStmt->addDefaultClause(ifStmt->getElseClause());
		appendBreakStmtIfNeeded(ifStmt->getElseClause());
	}

	Statement::replaceStatement(ifStmt, switchStmt);
}

bool IfToSwitchOptimizer::innerIsDenseEqChainConsecutive(ShPtr<IfStmt> inner,
		ShPtr<Expression> ctrl,
		int64_t &outFirst,
		unsigned &outN) {
	if (inner->hasElseClause()) {
		return false;
	}
	outN = static_cast<unsigned>(
		std::distance(inner->clause_begin(), inner->clause_end()));
	if (outN < 2 || outN > 256) {
		return false;
	}

	bool haveFirst = false;
	unsigned idx = 0;
	for (auto i = inner->clause_begin(), e = inner->clause_end(); i != e;
			++i, ++idx) {
		ShPtr<EqOpExpr> eq(cast<EqOpExpr>(i->first));
		if (!eq) {
			return false;
		}
		ShPtr<ConstInt> caseVal;
		if (eq->getFirstOperand()->isEqualTo(ctrl)) {
			caseVal = cast<ConstInt>(eq->getSecondOperand());
		} else if (eq->getSecondOperand()->isEqualTo(ctrl)) {
			caseVal = cast<ConstInt>(eq->getFirstOperand());
		} else {
			return false;
		}
		if (!caseVal) {
			return false;
		}
		int64_t cvi = 0;
		if (!apsIntToInt64(caseVal->getValue(), &cvi)) {
			return false;
		}
		if (!haveFirst) {
			outFirst = cvi;
			haveFirst = true;
		} else {
			const int64_t expected = outFirst + static_cast<int64_t>(idx);
			if (expected != cvi) {
				return false;
			}
		}
	}
	return haveFirst;
}

bool IfToSwitchOptimizer::innerIsDenseEqChainZeroToNMinus1(ShPtr<IfStmt> inner,
		ShPtr<Expression> ctrl,
		unsigned n) {
	int64_t first = 0;
	unsigned nc = 0;
	if (!innerIsDenseEqChainConsecutive(inner, ctrl, first, nc) || nc != n) {
		return false;
	}
	return first == 0;
}

bool IfToSwitchOptimizer::tryConvertOuterLtWithInnerDenseEqChain(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt) {
		return false;
	}
	if (isa<ConstInt>(olt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> nConst(cast<ConstInt>(olt->getSecondOperand()));
	if (!nConst) {
		return false;
	}
	int64_t cBound = 0;
	if (!apsIntToInt64(nConst->getValue(), &cBound)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt->getFirstOperand());

	ShPtr<IfStmt> inner(cast<IfStmt>(outer->getFirstIfBody()));
	if (!inner) {
		return false;
	}

	ShPtr<Expression> innerCtrl(getControlExprIfConvertibleToSwitch(inner));
	if (!innerCtrl || !innerCtrl->isEqualTo(outerLhs)) {
		return false;
	}
	int64_t firstCase = 0;
	unsigned n = 0;
	if (!innerIsDenseEqChainConsecutive(inner, innerCtrl, firstCase, n)) {
		return false;
	}
	const int64_t lastCase = firstCase + static_cast<int64_t>(n) - 1;
	if (lastCase == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (cBound != lastCase + 1) {
		return false;
	}

	convertIfStmtToSwitchStmt(inner, innerCtrl);
	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(outer->getFirstIfBody()));
	if (!sw) {
		return false;
	}
	Statement::replaceStatement(outer, sw);
	return true;
}

bool IfToSwitchOptimizer::tryConvertOuterLeWithInnerDenseEqChain(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole) {
		return false;
	}
	if (isa<ConstInt>(ole->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubConst(cast<ConstInt>(ole->getSecondOperand()));
	if (!ubConst) {
		return false;
	}
	int64_t ub = 0;
	if (!apsIntToInt64(ubConst->getValue(), &ub)) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole->getFirstOperand());

	ShPtr<IfStmt> inner(cast<IfStmt>(outer->getFirstIfBody()));
	if (!inner) {
		return false;
	}

	ShPtr<Expression> innerCtrl(getControlExprIfConvertibleToSwitch(inner));
	if (!innerCtrl || !innerCtrl->isEqualTo(outerLhs)) {
		return false;
	}
	int64_t firstCase = 0;
	unsigned n = 0;
	if (!innerIsDenseEqChainConsecutive(inner, innerCtrl, firstCase, n)) {
		return false;
	}
	const int64_t lastCase = firstCase + static_cast<int64_t>(n) - 1;
	if (ub != lastCase) {
		return false;
	}

	convertIfStmtToSwitchStmt(inner, innerCtrl);
	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(outer->getFirstIfBody()));
	if (!sw) {
		return false;
	}
	Statement::replaceStatement(outer, sw);
	return true;
}

bool IfToSwitchOptimizer::tryConvertOuterGtWithInnerDenseEqChain(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt) {
		return false;
	}
	if (isa<ConstInt>(ogt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> lConst(cast<ConstInt>(ogt->getSecondOperand()));
	if (!lConst) {
		return false;
	}
	int64_t lBound = 0;
	if (!apsIntToInt64(lConst->getValue(), &lBound)) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt->getFirstOperand());

	ShPtr<IfStmt> inner(cast<IfStmt>(outer->getFirstIfBody()));
	if (!inner) {
		return false;
	}

	ShPtr<Expression> innerCtrl(getControlExprIfConvertibleToSwitch(inner));
	if (!innerCtrl || !innerCtrl->isEqualTo(outerLhs)) {
		return false;
	}
	int64_t firstCase = 0;
	unsigned n = 0;
	if (!innerIsDenseEqChainConsecutive(inner, innerCtrl, firstCase, n)) {
		return false;
	}
	if (firstCase == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	if (lBound != firstCase - 1) {
		return false;
	}

	convertIfStmtToSwitchStmt(inner, innerCtrl);
	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(outer->getFirstIfBody()));
	if (!sw) {
		return false;
	}
	Statement::replaceStatement(outer, sw);
	return true;
}

bool IfToSwitchOptimizer::tryConvertOuterGeWithInnerDenseEqChain(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge) {
		return false;
	}
	if (isa<ConstInt>(oge->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> fConst(cast<ConstInt>(oge->getSecondOperand()));
	if (!fConst) {
		return false;
	}
	int64_t fBound = 0;
	if (!apsIntToInt64(fConst->getValue(), &fBound)) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge->getFirstOperand());

	ShPtr<IfStmt> inner(cast<IfStmt>(outer->getFirstIfBody()));
	if (!inner) {
		return false;
	}

	ShPtr<Expression> innerCtrl(getControlExprIfConvertibleToSwitch(inner));
	if (!innerCtrl || !innerCtrl->isEqualTo(outerLhs)) {
		return false;
	}
	int64_t firstCase = 0;
	unsigned n = 0;
	if (!innerIsDenseEqChainConsecutive(inner, innerCtrl, firstCase, n)) {
		return false;
	}
	if (fBound != firstCase) {
		return false;
	}

	convertIfStmtToSwitchStmt(inner, innerCtrl);
	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(outer->getFirstIfBody()));
	if (!sw) {
		return false;
	}
	Statement::replaceStatement(outer, sw);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtWithSixLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt0(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt0) {
		return false;
	}
	if (isa<ConstInt>(olt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b0Const(cast<ConstInt>(olt0->getSecondOperand()));
	if (!b0Const) {
		return false;
	}
	int64_t b0 = 0;
	if (!apsIntToInt64(b0Const->getValue(), &b0)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt0->getFirstOperand());

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailG(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailG) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailG)) {
		return false;
	}

	ShPtr<LtOpExpr> olt1(cast<LtOpExpr>(layer1->getFirstIfCond()));
	if (!olt1) {
		return false;
	}
	if (isa<ConstInt>(olt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b1Const(cast<ConstInt>(olt1->getSecondOperand()));
	if (!b1Const) {
		return false;
	}
	int64_t b1 = 0;
	if (!apsIntToInt64(b1Const->getValue(), &b1)) {
		return false;
	}
	if (!olt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainF(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainF) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainF)) {
		return false;
	}

	ShPtr<LtOpExpr> olt2(cast<LtOpExpr>(layer2->getFirstIfCond()));
	if (!olt2) {
		return false;
	}
	if (isa<ConstInt>(olt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b2Const(cast<ConstInt>(olt2->getSecondOperand()));
	if (!b2Const) {
		return false;
	}
	int64_t b2 = 0;
	if (!apsIntToInt64(b2Const->getValue(), &b2)) {
		return false;
	}
	if (!olt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainE) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<LtOpExpr> olt3(cast<LtOpExpr>(layer3->getFirstIfCond()));
	if (!olt3) {
		return false;
	}
	if (isa<ConstInt>(olt3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b3Const(cast<ConstInt>(olt3->getSecondOperand()));
	if (!b3Const) {
		return false;
	}
	int64_t b3 = 0;
	if (!apsIntToInt64(b3Const->getValue(), &b3)) {
		return false;
	}
	if (!olt3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainD) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<LtOpExpr> olt4(cast<LtOpExpr>(layer4->getFirstIfCond()));
	if (!olt4) {
		return false;
	}
	if (isa<ConstInt>(olt4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b4Const(cast<ConstInt>(olt4->getSecondOperand()));
	if (!b4Const) {
		return false;
	}
	int64_t b4 = 0;
	if (!apsIntToInt64(b4Const->getValue(), &b4)) {
		return false;
	}
	if (!olt4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer5(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer4->getElseClause()));
	if (!layer5 || !chainC) {
		return false;
	}
	if (layer5->hasElseIfClauses() || !layer5->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer5)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtOpExpr> olt5(cast<LtOpExpr>(layer5->getFirstIfCond()));
	if (!olt5) {
		return false;
	}
	if (isa<ConstInt>(olt5->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b5Const(cast<ConstInt>(olt5->getSecondOperand()));
	if (!b5Const) {
		return false;
	}
	int64_t b5 = 0;
	if (!apsIntToInt64(b5Const->getValue(), &b5)) {
		return false;
	}
	if (!olt5->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer5->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer5->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlF(getControlExprIfConvertibleToSwitch(chainF));
	if (!ctrlF || !ctrlF->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlG(getControlExprIfConvertibleToSwitch(tailG));
	if (!ctrlG || !ctrlG->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(chainF, ctrl, firstF, nF)) {
		return false;
	}
	int64_t firstG = 0;
	unsigned nG = 0;
	if (!innerIsDenseEqChainConsecutive(tailG, ctrl, firstG, nG)) {
		return false;
	}

	if (nA + nB + nC + nD + nE + nF + nG > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t split5 = firstA + static_cast<int64_t>(nA);
	if (firstB != split5 || b5 != split5) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t afterB = firstB + static_cast<int64_t>(nB);
	if (afterB != b4 || firstC != b4) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t afterC = firstC + static_cast<int64_t>(nC);
	if (afterC != b3 || firstD != b3) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t afterD = firstD + static_cast<int64_t>(nD);
	if (afterD != b2 || firstE != b2) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t afterE = firstE + static_cast<int64_t>(nE);
	if (afterE != b1 || firstF != b1) {
		return false;
	}
	if (firstF > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nF)) {
		return false;
	}
	const int64_t afterF = firstF + static_cast<int64_t>(nF);
	if (afterF != b0 || firstG != b0) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {
		chainA, chainB, chainC, chainD, chainE, chainF, tailG};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtWithFiveLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt0(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt0) {
		return false;
	}
	if (isa<ConstInt>(olt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b0Const(cast<ConstInt>(olt0->getSecondOperand()));
	if (!b0Const) {
		return false;
	}
	int64_t b0 = 0;
	if (!apsIntToInt64(b0Const->getValue(), &b0)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt0->getFirstOperand());

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailF(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailF) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailF)) {
		return false;
	}

	ShPtr<LtOpExpr> olt1(cast<LtOpExpr>(layer1->getFirstIfCond()));
	if (!olt1) {
		return false;
	}
	if (isa<ConstInt>(olt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b1Const(cast<ConstInt>(olt1->getSecondOperand()));
	if (!b1Const) {
		return false;
	}
	int64_t b1 = 0;
	if (!apsIntToInt64(b1Const->getValue(), &b1)) {
		return false;
	}
	if (!olt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainE) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<LtOpExpr> olt2(cast<LtOpExpr>(layer2->getFirstIfCond()));
	if (!olt2) {
		return false;
	}
	if (isa<ConstInt>(olt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b2Const(cast<ConstInt>(olt2->getSecondOperand()));
	if (!b2Const) {
		return false;
	}
	int64_t b2 = 0;
	if (!apsIntToInt64(b2Const->getValue(), &b2)) {
		return false;
	}
	if (!olt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainD) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<LtOpExpr> olt3(cast<LtOpExpr>(layer3->getFirstIfCond()));
	if (!olt3) {
		return false;
	}
	if (isa<ConstInt>(olt3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b3Const(cast<ConstInt>(olt3->getSecondOperand()));
	if (!b3Const) {
		return false;
	}
	int64_t b3 = 0;
	if (!apsIntToInt64(b3Const->getValue(), &b3)) {
		return false;
	}
	if (!olt3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainC) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtOpExpr> olt4(cast<LtOpExpr>(layer4->getFirstIfCond()));
	if (!olt4) {
		return false;
	}
	if (isa<ConstInt>(olt4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b4Const(cast<ConstInt>(olt4->getSecondOperand()));
	if (!b4Const) {
		return false;
	}
	int64_t b4 = 0;
	if (!apsIntToInt64(b4Const->getValue(), &b4)) {
		return false;
	}
	if (!olt4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer4->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlF(getControlExprIfConvertibleToSwitch(tailF));
	if (!ctrlF || !ctrlF->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(tailF, ctrl, firstF, nF)) {
		return false;
	}

	if (nA + nB + nC + nD + nE + nF > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t split4 = firstA + static_cast<int64_t>(nA);
	if (firstB != split4 || b4 != split4) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t afterB = firstB + static_cast<int64_t>(nB);
	if (afterB != b3 || firstC != b3) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t afterC = firstC + static_cast<int64_t>(nC);
	if (afterC != b2 || firstD != b2) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t afterD = firstD + static_cast<int64_t>(nD);
	if (afterD != b1 || firstE != b1) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t afterE = firstE + static_cast<int64_t>(nE);
	if (afterE != b0 || firstF != b0) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, chainD, chainE, tailF};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtWithFourLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt0(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt0) {
		return false;
	}
	if (isa<ConstInt>(olt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b0Const(cast<ConstInt>(olt0->getSecondOperand()));
	if (!b0Const) {
		return false;
	}
	int64_t b0 = 0;
	if (!apsIntToInt64(b0Const->getValue(), &b0)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt0->getFirstOperand());

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailE(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailE) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailE)) {
		return false;
	}

	ShPtr<LtOpExpr> olt1(cast<LtOpExpr>(layer1->getFirstIfCond()));
	if (!olt1) {
		return false;
	}
	if (isa<ConstInt>(olt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b1Const(cast<ConstInt>(olt1->getSecondOperand()));
	if (!b1Const) {
		return false;
	}
	int64_t b1 = 0;
	if (!apsIntToInt64(b1Const->getValue(), &b1)) {
		return false;
	}
	if (!olt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainD) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<LtOpExpr> olt2(cast<LtOpExpr>(layer2->getFirstIfCond()));
	if (!olt2) {
		return false;
	}
	if (isa<ConstInt>(olt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b2Const(cast<ConstInt>(olt2->getSecondOperand()));
	if (!b2Const) {
		return false;
	}
	int64_t b2 = 0;
	if (!apsIntToInt64(b2Const->getValue(), &b2)) {
		return false;
	}
	if (!olt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainC) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtOpExpr> olt3(cast<LtOpExpr>(layer3->getFirstIfCond()));
	if (!olt3) {
		return false;
	}
	if (isa<ConstInt>(olt3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> b3Const(cast<ConstInt>(olt3->getSecondOperand()));
	if (!b3Const) {
		return false;
	}
	int64_t b3 = 0;
	if (!apsIntToInt64(b3Const->getValue(), &b3)) {
		return false;
	}
	if (!olt3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer3->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(tailE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(tailE, ctrl, firstE, nE)) {
		return false;
	}

	if (nA + nB + nC + nD + nE > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t split3 = firstA + static_cast<int64_t>(nA);
	if (firstB != split3 || b3 != split3) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t afterB = firstB + static_cast<int64_t>(nB);
	if (afterB != b2 || firstC != b2) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t afterC = firstC + static_cast<int64_t>(nC);
	if (afterC != b1 || firstD != b1) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t afterD = firstD + static_cast<int64_t>(nD);
	if (afterD != b0 || firstE != b0) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, chainD, tailE};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtWithThreeLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt0(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt0) {
		return false;
	}
	if (isa<ConstInt>(olt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> midOConst(cast<ConstInt>(olt0->getSecondOperand()));
	if (!midOConst) {
		return false;
	}
	int64_t midO = 0;
	if (!apsIntToInt64(midOConst->getValue(), &midO)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt0->getFirstOperand());

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<LtOpExpr> olt1(cast<LtOpExpr>(midOuter->getFirstIfCond()));
	if (!olt1) {
		return false;
	}
	if (isa<ConstInt>(olt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> mid1Const(cast<ConstInt>(olt1->getSecondOperand()));
	if (!mid1Const) {
		return false;
	}
	int64_t mid1 = 0;
	if (!apsIntToInt64(mid1Const->getValue(), &mid1)) {
		return false;
	}
	if (!olt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtOpExpr> olt2(cast<LtOpExpr>(midInner->getFirstIfCond()));
	if (!olt2) {
		return false;
	}
	if (isa<ConstInt>(olt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> mid2Const(cast<ConstInt>(olt2->getSecondOperand()));
	if (!mid2Const) {
		return false;
	}
	int64_t mid2 = 0;
	if (!apsIntToInt64(mid2Const->getValue(), &mid2)) {
		return false;
	}
	if (!olt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}

	if (nA + nB + nC + nD > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t split2 = firstA + static_cast<int64_t>(nA);
	if (firstB != split2 || mid2 != split2) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t afterB = firstB + static_cast<int64_t>(nB);
	if (afterB != mid1 || firstC != mid1) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t afterC = firstC + static_cast<int64_t>(nC);
	if (afterC != midO || firstD != midO) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, tailD};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtWithThreeLevelNestedInnerLeSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt0(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt0) {
		return false;
	}
	if (isa<ConstInt>(olt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> midOConst(cast<ConstInt>(olt0->getSecondOperand()));
	if (!midOConst) {
		return false;
	}
	int64_t midO = 0;
	if (!apsIntToInt64(midOConst->getValue(), &midO)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt0->getFirstOperand());

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<LtOpExpr> olt1(cast<LtOpExpr>(midOuter->getFirstIfCond()));
	if (!olt1) {
		return false;
	}
	if (isa<ConstInt>(olt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> mid1Const(cast<ConstInt>(olt1->getSecondOperand()));
	if (!mid1Const) {
		return false;
	}
	int64_t mid1 = 0;
	if (!apsIntToInt64(mid1Const->getValue(), &mid1)) {
		return false;
	}
	if (!olt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole2(cast<LtEqOpExpr>(midInner->getFirstIfCond()));
	if (!ole2) {
		return false;
	}
	if (isa<ConstInt>(ole2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub2Const(cast<ConstInt>(ole2->getSecondOperand()));
	if (!ub2Const) {
		return false;
	}
	int64_t ub2 = 0;
	if (!apsIntToInt64(ub2Const->getValue(), &ub2)) {
		return false;
	}
	if (ub2 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}

	if (nA + nB + nC + nD > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t lastA = firstA + static_cast<int64_t>(nA) - 1;
	if (lastA != ub2) {
		return false;
	}
	const int64_t expectFirstB = firstA + static_cast<int64_t>(nA);
	if (firstB != ub2 + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t afterB = firstB + static_cast<int64_t>(nB);
	if (afterB != mid1 || firstC != mid1) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t afterC = firstC + static_cast<int64_t>(nC);
	if (afterC != midO || firstD != midO) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, tailD};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLeWithThreeLevelNestedInnerLtSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole0(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole0) {
		return false;
	}
	if (isa<ConstInt>(ole0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubOConst(cast<ConstInt>(ole0->getSecondOperand()));
	if (!ubOConst) {
		return false;
	}
	int64_t ubO = 0;
	if (!apsIntToInt64(ubOConst->getValue(), &ubO)) {
		return false;
	}
	if (ubO == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole0->getFirstOperand());
	const LtEqOpExpr::Variant outerVariant = ole0->getVariant();

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole1(cast<LtEqOpExpr>(midOuter->getFirstIfCond()));
	if (!ole1) {
		return false;
	}
	if (ole1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub1Const(cast<ConstInt>(ole1->getSecondOperand()));
	if (!ub1Const) {
		return false;
	}
	int64_t ub1 = 0;
	if (!apsIntToInt64(ub1Const->getValue(), &ub1)) {
		return false;
	}
	if (ub1 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtOpExpr> olt2(cast<LtOpExpr>(midInner->getFirstIfCond()));
	if (!olt2) {
		return false;
	}
	if (isa<ConstInt>(olt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> mid2Const(cast<ConstInt>(olt2->getSecondOperand()));
	if (!mid2Const) {
		return false;
	}
	int64_t mid2 = 0;
	if (!apsIntToInt64(mid2Const->getValue(), &mid2)) {
		return false;
	}
	if (!olt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}

	if (nA + nB + nC + nD > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t split2 = firstA + static_cast<int64_t>(nA);
	if (firstB != split2 || mid2 != split2) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != ub1) {
		return false;
	}
	const int64_t expectFirstC = firstB + static_cast<int64_t>(nB);
	if (firstC != ub1 + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != ubO) {
		return false;
	}
	const int64_t expectFirstD = firstC + static_cast<int64_t>(nC);
	if (firstD != ubO + 1 || firstD != expectFirstD) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, tailD};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGtWithThreeLevelNestedInnerGeSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt0(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt0) {
		return false;
	}
	if (isa<ConstInt>(ogt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l0Const(cast<ConstInt>(ogt0->getSecondOperand()));
	if (!l0Const) {
		return false;
	}
	int64_t l0Bound = 0;
	if (!apsIntToInt64(l0Const->getValue(), &l0Bound)) {
		return false;
	}
	if (l0Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt0->getFirstOperand());

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt1(cast<GtOpExpr>(midOuter->getFirstIfCond()));
	if (!ogt1) {
		return false;
	}
	if (isa<ConstInt>(ogt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l1Const(cast<ConstInt>(ogt1->getSecondOperand()));
	if (!l1Const) {
		return false;
	}
	int64_t l1Bound = 0;
	if (!apsIntToInt64(l1Const->getValue(), &l1Bound)) {
		return false;
	}
	if (l1Bound <= l0Bound || l1Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtEqOpExpr> ige2(cast<GtEqOpExpr>(midInner->getFirstIfCond()));
	if (!ige2) {
		return false;
	}
	if (isa<ConstInt>(ige2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> hConst(cast<ConstInt>(ige2->getSecondOperand()));
	if (!hConst) {
		return false;
	}
	int64_t hBound = 0;
	if (!apsIntToInt64(hConst->getValue(), &hBound)) {
		return false;
	}
	if (hBound <= l1Bound || hBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ige2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != l0Bound) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != l0Bound + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != l1Bound) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != l1Bound + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != hBound - 1) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != hBound || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeWithThreeLevelNestedInnerGtSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge0(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge0) {
		return false;
	}
	if (isa<ConstInt>(oge0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f0Const(cast<ConstInt>(oge0->getSecondOperand()));
	if (!f0Const) {
		return false;
	}
	int64_t f0Bound = 0;
	if (!apsIntToInt64(f0Const->getValue(), &f0Bound)) {
		return false;
	}
	if (f0Bound == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge0->getFirstOperand());
	const GtEqOpExpr::Variant outerVariant = oge0->getVariant();

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge1(cast<GtEqOpExpr>(midOuter->getFirstIfCond()));
	if (!oge1) {
		return false;
	}
	if (oge1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f1Const(cast<ConstInt>(oge1->getSecondOperand()));
	if (!f1Const) {
		return false;
	}
	int64_t f1Bound = 0;
	if (!apsIntToInt64(f1Const->getValue(), &f1Bound)) {
		return false;
	}
	if (f1Bound <= f0Bound) {
		return false;
	}
	if (!oge1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtOpExpr> igt2(cast<GtOpExpr>(midInner->getFirstIfCond()));
	if (!igt2) {
		return false;
	}
	if (isa<ConstInt>(igt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> hiConst(cast<ConstInt>(igt2->getSecondOperand()));
	if (!hiConst) {
		return false;
	}
	int64_t hiBound = 0;
	if (!apsIntToInt64(hiConst->getValue(), &hiBound)) {
		return false;
	}
	if (hiBound < f1Bound || hiBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!igt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != f0Bound - 1) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != f0Bound || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != f1Bound - 1) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != f1Bound || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != hiBound) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != hiBound + 1 || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtWithNestedLtSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt) {
		return false;
	}
	if (isa<ConstInt>(olt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> midOConst(cast<ConstInt>(olt->getSecondOperand()));
	if (!midOConst) {
		return false;
	}
	int64_t midO = 0;
	if (!apsIntToInt64(midOConst->getValue(), &midO)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt->getFirstOperand());

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	// Middle must be a single if/else (inner Lt split), not an else-if == chain.
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<LtOpExpr> ilt(cast<LtOpExpr>(midIf->getFirstIfCond()));
	if (!ilt) {
		return false;
	}
	if (isa<ConstInt>(ilt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> midIConst(cast<ConstInt>(ilt->getSecondOperand()));
	if (!midIConst) {
		return false;
	}
	int64_t midI = 0;
	if (!apsIntToInt64(midIConst->getValue(), &midI)) {
		return false;
	}
	if (!ilt->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getFirstIfBody()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getElseClause()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t splitI = firstLow + static_cast<int64_t>(nLow);
	if (firstHigh != splitI || midI != splitI) {
		return false;
	}
	if (firstHigh > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nHigh)) {
		return false;
	}
	const int64_t afterHigh = firstHigh + static_cast<int64_t>(nHigh);
	if (afterHigh != midO || firstTail != midO) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {lowInner, highInner, tailRight};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLeWithThreeLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole0(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole0) {
		return false;
	}
	if (isa<ConstInt>(ole0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubOConst(cast<ConstInt>(ole0->getSecondOperand()));
	if (!ubOConst) {
		return false;
	}
	int64_t ubO = 0;
	if (!apsIntToInt64(ubOConst->getValue(), &ubO)) {
		return false;
	}
	if (ubO == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole0->getFirstOperand());
	const LtEqOpExpr::Variant outerVariant = ole0->getVariant();

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole1(cast<LtEqOpExpr>(midOuter->getFirstIfCond()));
	if (!ole1) {
		return false;
	}
	if (ole1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub1Const(cast<ConstInt>(ole1->getSecondOperand()));
	if (!ub1Const) {
		return false;
	}
	int64_t ub1 = 0;
	if (!apsIntToInt64(ub1Const->getValue(), &ub1)) {
		return false;
	}
	if (ub1 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole2(cast<LtEqOpExpr>(midInner->getFirstIfCond()));
	if (!ole2) {
		return false;
	}
	if (ole2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub2Const(cast<ConstInt>(ole2->getSecondOperand()));
	if (!ub2Const) {
		return false;
	}
	int64_t ub2 = 0;
	if (!apsIntToInt64(ub2Const->getValue(), &ub2)) {
		return false;
	}
	if (ub2 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}

	if (nA + nB + nC + nD > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t lastA = firstA + static_cast<int64_t>(nA) - 1;
	if (lastA != ub2) {
		return false;
	}
	const int64_t expectFirstB = firstA + static_cast<int64_t>(nA);
	if (firstB != ub2 + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != ub1) {
		return false;
	}
	const int64_t expectFirstC = firstB + static_cast<int64_t>(nB);
	if (firstC != ub1 + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != ubO) {
		return false;
	}
	const int64_t expectFirstD = firstC + static_cast<int64_t>(nC);
	if (firstD != ubO + 1 || firstD != expectFirstD) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, tailD};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLeWithNestedLeSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole) {
		return false;
	}
	if (isa<ConstInt>(ole->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubOConst(cast<ConstInt>(ole->getSecondOperand()));
	if (!ubOConst) {
		return false;
	}
	int64_t ubO = 0;
	if (!apsIntToInt64(ubOConst->getValue(), &ubO)) {
		return false;
	}
	if (ubO == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole->getFirstOperand());
	const LtEqOpExpr::Variant outerVariant = ole->getVariant();

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ile(cast<LtEqOpExpr>(midIf->getFirstIfCond()));
	if (!ile) {
		return false;
	}
	if (ile->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ile->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubIConst(cast<ConstInt>(ile->getSecondOperand()));
	if (!ubIConst) {
		return false;
	}
	int64_t ubI = 0;
	if (!apsIntToInt64(ubIConst->getValue(), &ubI)) {
		return false;
	}
	if (!ile->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getFirstIfBody()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getElseClause()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t lastLow = firstLow + static_cast<int64_t>(nLow) - 1;
	if (lastLow != ubI) {
		return false;
	}
	if (ubI == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	const int64_t expectFirstHigh = firstLow + static_cast<int64_t>(nLow);
	if (firstHigh != ubI + 1 || firstHigh != expectFirstHigh) {
		return false;
	}
	if (firstHigh > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nHigh)) {
		return false;
	}
	const int64_t lastHigh = firstHigh + static_cast<int64_t>(nHigh) - 1;
	if (lastHigh != ubO) {
		return false;
	}
	if (firstTail != ubO + 1) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {lowInner, highInner, tailRight};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtWithNestedLeSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt) {
		return false;
	}
	if (isa<ConstInt>(olt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> midOConst(cast<ConstInt>(olt->getSecondOperand()));
	if (!midOConst) {
		return false;
	}
	int64_t midO = 0;
	if (!apsIntToInt64(midOConst->getValue(), &midO)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt->getFirstOperand());

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ile(cast<LtEqOpExpr>(midIf->getFirstIfCond()));
	if (!ile) {
		return false;
	}
	if (isa<ConstInt>(ile->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubIConst(cast<ConstInt>(ile->getSecondOperand()));
	if (!ubIConst) {
		return false;
	}
	int64_t ubI = 0;
	if (!apsIntToInt64(ubIConst->getValue(), &ubI)) {
		return false;
	}
	if (!ile->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getFirstIfBody()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getElseClause()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t lastLow = firstLow + static_cast<int64_t>(nLow) - 1;
	if (lastLow != ubI) {
		return false;
	}
	if (ubI == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	const int64_t expectFirstHigh = firstLow + static_cast<int64_t>(nLow);
	if (firstHigh != ubI + 1 || firstHigh != expectFirstHigh) {
		return false;
	}
	if (firstHigh > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nHigh)) {
		return false;
	}
	const int64_t afterHigh = firstHigh + static_cast<int64_t>(nHigh);
	if (afterHigh != midO || firstTail != midO) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {lowInner, highInner, tailRight};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLeWithNestedLtSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole) {
		return false;
	}
	if (isa<ConstInt>(ole->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubOConst(cast<ConstInt>(ole->getSecondOperand()));
	if (!ubOConst) {
		return false;
	}
	int64_t ubO = 0;
	if (!apsIntToInt64(ubOConst->getValue(), &ubO)) {
		return false;
	}
	if (ubO == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole->getFirstOperand());

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<LtOpExpr> ilt(cast<LtOpExpr>(midIf->getFirstIfCond()));
	if (!ilt) {
		return false;
	}
	if (isa<ConstInt>(ilt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> midIConst(cast<ConstInt>(ilt->getSecondOperand()));
	if (!midIConst) {
		return false;
	}
	int64_t midI = 0;
	if (!apsIntToInt64(midIConst->getValue(), &midI)) {
		return false;
	}
	if (!ilt->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getFirstIfBody()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getElseClause()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t splitI = firstLow + static_cast<int64_t>(nLow);
	if (firstHigh != splitI || midI != splitI) {
		return false;
	}
	if (firstHigh > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nHigh)) {
		return false;
	}
	const int64_t lastHigh = firstHigh + static_cast<int64_t>(nHigh) - 1;
	if (lastHigh != ubO) {
		return false;
	}
	if (firstTail != ubO + 1) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {lowInner, highInner, tailRight};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGtWithThreeLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt0(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt0) {
		return false;
	}
	if (isa<ConstInt>(ogt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l0Const(cast<ConstInt>(ogt0->getSecondOperand()));
	if (!l0Const) {
		return false;
	}
	int64_t l0Bound = 0;
	if (!apsIntToInt64(l0Const->getValue(), &l0Bound)) {
		return false;
	}
	if (l0Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt0->getFirstOperand());

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt1(cast<GtOpExpr>(midOuter->getFirstIfCond()));
	if (!ogt1) {
		return false;
	}
	if (isa<ConstInt>(ogt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l1Const(cast<ConstInt>(ogt1->getSecondOperand()));
	if (!l1Const) {
		return false;
	}
	int64_t l1Bound = 0;
	if (!apsIntToInt64(l1Const->getValue(), &l1Bound)) {
		return false;
	}
	if (l1Bound <= l0Bound || l1Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt2(cast<GtOpExpr>(midInner->getFirstIfCond()));
	if (!ogt2) {
		return false;
	}
	if (isa<ConstInt>(ogt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l2Const(cast<ConstInt>(ogt2->getSecondOperand()));
	if (!l2Const) {
		return false;
	}
	int64_t l2Bound = 0;
	if (!apsIntToInt64(l2Const->getValue(), &l2Bound)) {
		return false;
	}
	if (l2Bound <= l1Bound || l2Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != l0Bound) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != l0Bound + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != l1Bound) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != l1Bound + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != l2Bound) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != l2Bound + 1 || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeWithThreeLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge0(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge0) {
		return false;
	}
	if (isa<ConstInt>(oge0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f0Const(cast<ConstInt>(oge0->getSecondOperand()));
	if (!f0Const) {
		return false;
	}
	int64_t f0Bound = 0;
	if (!apsIntToInt64(f0Const->getValue(), &f0Bound)) {
		return false;
	}
	if (f0Bound == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge0->getFirstOperand());
	const GtEqOpExpr::Variant outerVariant = oge0->getVariant();

	ShPtr<IfStmt> midOuter(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailD(cast<IfStmt>(outer->getElseClause()));
	if (!midOuter || !tailD) {
		return false;
	}
	if (midOuter->hasElseIfClauses() || !midOuter->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midOuter)
			|| BreakInIfAnalysis::hasBreakStmt(tailD)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge1(cast<GtEqOpExpr>(midOuter->getFirstIfCond()));
	if (!oge1) {
		return false;
	}
	if (oge1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f1Const(cast<ConstInt>(oge1->getSecondOperand()));
	if (!f1Const) {
		return false;
	}
	int64_t f1Bound = 0;
	if (!apsIntToInt64(f1Const->getValue(), &f1Bound)) {
		return false;
	}
	if (f1Bound <= f0Bound) {
		return false;
	}
	if (!oge1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> midInner(cast<IfStmt>(midOuter->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(midOuter->getElseClause()));
	if (!midInner || !chainC) {
		return false;
	}
	if (midInner->hasElseIfClauses() || !midInner->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midInner)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge2(cast<GtEqOpExpr>(midInner->getFirstIfCond()));
	if (!oge2) {
		return false;
	}
	if (oge2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f2Const(cast<ConstInt>(oge2->getSecondOperand()));
	if (!f2Const) {
		return false;
	}
	int64_t f2Bound = 0;
	if (!apsIntToInt64(f2Const->getValue(), &f2Bound)) {
		return false;
	}
	if (f2Bound <= f1Bound) {
		return false;
	}
	if (!oge2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(midInner->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(midInner->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailD));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(tailD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != f0Bound - 1) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != f0Bound || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != f1Bound - 1) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != f1Bound || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != f2Bound - 1) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != f2Bound || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLeWithSixLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole0(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole0) {
		return false;
	}
	if (isa<ConstInt>(ole0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub0Const(cast<ConstInt>(ole0->getSecondOperand()));
	if (!ub0Const) {
		return false;
	}
	int64_t ub0 = 0;
	if (!apsIntToInt64(ub0Const->getValue(), &ub0)) {
		return false;
	}
	if (ub0 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole0->getFirstOperand());
	const LtEqOpExpr::Variant outerVariant = ole0->getVariant();

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailG(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailG) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailG)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole1(cast<LtEqOpExpr>(layer1->getFirstIfCond()));
	if (!ole1) {
		return false;
	}
	if (ole1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub1Const(cast<ConstInt>(ole1->getSecondOperand()));
	if (!ub1Const) {
		return false;
	}
	int64_t ub1 = 0;
	if (!apsIntToInt64(ub1Const->getValue(), &ub1)) {
		return false;
	}
	if (ub1 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainF(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainF) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainF)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole2(cast<LtEqOpExpr>(layer2->getFirstIfCond()));
	if (!ole2) {
		return false;
	}
	if (ole2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub2Const(cast<ConstInt>(ole2->getSecondOperand()));
	if (!ub2Const) {
		return false;
	}
	int64_t ub2 = 0;
	if (!apsIntToInt64(ub2Const->getValue(), &ub2)) {
		return false;
	}
	if (ub2 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainE) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole3(cast<LtEqOpExpr>(layer3->getFirstIfCond()));
	if (!ole3) {
		return false;
	}
	if (ole3->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub3Const(cast<ConstInt>(ole3->getSecondOperand()));
	if (!ub3Const) {
		return false;
	}
	int64_t ub3 = 0;
	if (!apsIntToInt64(ub3Const->getValue(), &ub3)) {
		return false;
	}
	if (ub3 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainD) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole4(cast<LtEqOpExpr>(layer4->getFirstIfCond()));
	if (!ole4) {
		return false;
	}
	if (ole4->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub4Const(cast<ConstInt>(ole4->getSecondOperand()));
	if (!ub4Const) {
		return false;
	}
	int64_t ub4 = 0;
	if (!apsIntToInt64(ub4Const->getValue(), &ub4)) {
		return false;
	}
	if (ub4 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer5(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer4->getElseClause()));
	if (!layer5 || !chainC) {
		return false;
	}
	if (layer5->hasElseIfClauses() || !layer5->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer5)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole5(cast<LtEqOpExpr>(layer5->getFirstIfCond()));
	if (!ole5) {
		return false;
	}
	if (ole5->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole5->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub5Const(cast<ConstInt>(ole5->getSecondOperand()));
	if (!ub5Const) {
		return false;
	}
	int64_t ub5 = 0;
	if (!apsIntToInt64(ub5Const->getValue(), &ub5)) {
		return false;
	}
	if (ub5 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole5->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer5->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer5->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlF(getControlExprIfConvertibleToSwitch(chainF));
	if (!ctrlF || !ctrlF->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlG(getControlExprIfConvertibleToSwitch(tailG));
	if (!ctrlG || !ctrlG->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(chainF, ctrl, firstF, nF)) {
		return false;
	}
	int64_t firstG = 0;
	unsigned nG = 0;
	if (!innerIsDenseEqChainConsecutive(tailG, ctrl, firstG, nG)) {
		return false;
	}

	if (nA + nB + nC + nD + nE + nF + nG > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t lastA = firstA + static_cast<int64_t>(nA) - 1;
	if (lastA != ub5) {
		return false;
	}
	const int64_t expectFirstB = firstA + static_cast<int64_t>(nA);
	if (firstB != ub5 + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != ub4) {
		return false;
	}
	const int64_t expectFirstC = firstB + static_cast<int64_t>(nB);
	if (firstC != ub4 + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != ub3) {
		return false;
	}
	const int64_t expectFirstD = firstC + static_cast<int64_t>(nC);
	if (firstD != ub3 + 1 || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != ub2) {
		return false;
	}
	const int64_t expectFirstE = firstD + static_cast<int64_t>(nD);
	if (firstE != ub2 + 1 || firstE != expectFirstE) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != ub1) {
		return false;
	}
	const int64_t expectFirstF = firstE + static_cast<int64_t>(nE);
	if (firstF != ub1 + 1 || firstF != expectFirstF) {
		return false;
	}
	if (firstF > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nF)) {
		return false;
	}
	const int64_t lastF = firstF + static_cast<int64_t>(nF) - 1;
	if (lastF != ub0) {
		return false;
	}
	const int64_t expectFirstG = firstF + static_cast<int64_t>(nF);
	if (firstG != ub0 + 1 || firstG != expectFirstG) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {
		chainA, chainB, chainC, chainD, chainE, chainF, tailG};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLeWithFiveLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole0(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole0) {
		return false;
	}
	if (isa<ConstInt>(ole0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub0Const(cast<ConstInt>(ole0->getSecondOperand()));
	if (!ub0Const) {
		return false;
	}
	int64_t ub0 = 0;
	if (!apsIntToInt64(ub0Const->getValue(), &ub0)) {
		return false;
	}
	if (ub0 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole0->getFirstOperand());
	const LtEqOpExpr::Variant outerVariant = ole0->getVariant();

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailF(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailF) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailF)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole1(cast<LtEqOpExpr>(layer1->getFirstIfCond()));
	if (!ole1) {
		return false;
	}
	if (ole1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub1Const(cast<ConstInt>(ole1->getSecondOperand()));
	if (!ub1Const) {
		return false;
	}
	int64_t ub1 = 0;
	if (!apsIntToInt64(ub1Const->getValue(), &ub1)) {
		return false;
	}
	if (ub1 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainE) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole2(cast<LtEqOpExpr>(layer2->getFirstIfCond()));
	if (!ole2) {
		return false;
	}
	if (ole2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub2Const(cast<ConstInt>(ole2->getSecondOperand()));
	if (!ub2Const) {
		return false;
	}
	int64_t ub2 = 0;
	if (!apsIntToInt64(ub2Const->getValue(), &ub2)) {
		return false;
	}
	if (ub2 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainD) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole3(cast<LtEqOpExpr>(layer3->getFirstIfCond()));
	if (!ole3) {
		return false;
	}
	if (ole3->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub3Const(cast<ConstInt>(ole3->getSecondOperand()));
	if (!ub3Const) {
		return false;
	}
	int64_t ub3 = 0;
	if (!apsIntToInt64(ub3Const->getValue(), &ub3)) {
		return false;
	}
	if (ub3 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainC) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole4(cast<LtEqOpExpr>(layer4->getFirstIfCond()));
	if (!ole4) {
		return false;
	}
	if (ole4->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub4Const(cast<ConstInt>(ole4->getSecondOperand()));
	if (!ub4Const) {
		return false;
	}
	int64_t ub4 = 0;
	if (!apsIntToInt64(ub4Const->getValue(), &ub4)) {
		return false;
	}
	if (ub4 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer4->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlF(getControlExprIfConvertibleToSwitch(tailF));
	if (!ctrlF || !ctrlF->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(tailF, ctrl, firstF, nF)) {
		return false;
	}

	if (nA + nB + nC + nD + nE + nF > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t lastA = firstA + static_cast<int64_t>(nA) - 1;
	if (lastA != ub4) {
		return false;
	}
	const int64_t expectFirstB = firstA + static_cast<int64_t>(nA);
	if (firstB != ub4 + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != ub3) {
		return false;
	}
	const int64_t expectFirstC = firstB + static_cast<int64_t>(nB);
	if (firstC != ub3 + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != ub2) {
		return false;
	}
	const int64_t expectFirstD = firstC + static_cast<int64_t>(nC);
	if (firstD != ub2 + 1 || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != ub1) {
		return false;
	}
	const int64_t expectFirstE = firstD + static_cast<int64_t>(nD);
	if (firstE != ub1 + 1 || firstE != expectFirstE) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != ub0) {
		return false;
	}
	const int64_t expectFirstF = firstE + static_cast<int64_t>(nE);
	if (firstF != ub0 + 1 || firstF != expectFirstF) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, chainD, chainE, tailF};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}


bool IfToSwitchOptimizer::tryConvertLeWithFourLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole0(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole0) {
		return false;
	}
	if (isa<ConstInt>(ole0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub0Const(cast<ConstInt>(ole0->getSecondOperand()));
	if (!ub0Const) {
		return false;
	}
	int64_t ub0 = 0;
	if (!apsIntToInt64(ub0Const->getValue(), &ub0)) {
		return false;
	}
	if (ub0 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole0->getFirstOperand());
	const LtEqOpExpr::Variant outerVariant = ole0->getVariant();

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailE(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailE) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailE)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole1(cast<LtEqOpExpr>(layer1->getFirstIfCond()));
	if (!ole1) {
		return false;
	}
	if (ole1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub1Const(cast<ConstInt>(ole1->getSecondOperand()));
	if (!ub1Const) {
		return false;
	}
	int64_t ub1 = 0;
	if (!apsIntToInt64(ub1Const->getValue(), &ub1)) {
		return false;
	}
	if (ub1 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainD) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole2(cast<LtEqOpExpr>(layer2->getFirstIfCond()));
	if (!ole2) {
		return false;
	}
	if (ole2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub2Const(cast<ConstInt>(ole2->getSecondOperand()));
	if (!ub2Const) {
		return false;
	}
	int64_t ub2 = 0;
	if (!apsIntToInt64(ub2Const->getValue(), &ub2)) {
		return false;
	}
	if (ub2 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainC) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole3(cast<LtEqOpExpr>(layer3->getFirstIfCond()));
	if (!ole3) {
		return false;
	}
	if (ole3->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ole3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ub3Const(cast<ConstInt>(ole3->getSecondOperand()));
	if (!ub3Const) {
		return false;
	}
	int64_t ub3 = 0;
	if (!apsIntToInt64(ub3Const->getValue(), &ub3)) {
		return false;
	}
	if (ub3 == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ole3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer3->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(tailE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(tailE, ctrl, firstE, nE)) {
		return false;
	}

	if (nA + nB + nC + nD + nE > 256) {
		return false;
	}
	if (firstA > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nA)) {
		return false;
	}
	const int64_t lastA = firstA + static_cast<int64_t>(nA) - 1;
	if (lastA != ub3) {
		return false;
	}
	const int64_t expectFirstB = firstA + static_cast<int64_t>(nA);
	if (firstB != ub3 + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != ub2) {
		return false;
	}
	const int64_t expectFirstC = firstB + static_cast<int64_t>(nB);
	if (firstC != ub2 + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != ub1) {
		return false;
	}
	const int64_t expectFirstD = firstC + static_cast<int64_t>(nC);
	if (firstD != ub1 + 1 || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != ub0) {
		return false;
	}
	const int64_t expectFirstE = firstD + static_cast<int64_t>(nD);
	if (firstE != ub0 + 1 || firstE != expectFirstE) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {chainA, chainB, chainC, chainD, tailE};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGtWithSixLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt0(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt0) {
		return false;
	}
	if (isa<ConstInt>(ogt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l0Const(cast<ConstInt>(ogt0->getSecondOperand()));
	if (!l0Const) {
		return false;
	}
	int64_t l0Bound = 0;
	if (!apsIntToInt64(l0Const->getValue(), &l0Bound)) {
		return false;
	}
	if (l0Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt0->getFirstOperand());

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailG(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailG) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailG)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt1(cast<GtOpExpr>(layer1->getFirstIfCond()));
	if (!ogt1) {
		return false;
	}
	if (isa<ConstInt>(ogt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l1Const(cast<ConstInt>(ogt1->getSecondOperand()));
	if (!l1Const) {
		return false;
	}
	int64_t l1Bound = 0;
	if (!apsIntToInt64(l1Const->getValue(), &l1Bound)) {
		return false;
	}
	if (l1Bound <= l0Bound || l1Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainF(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainF) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainF)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt2(cast<GtOpExpr>(layer2->getFirstIfCond()));
	if (!ogt2) {
		return false;
	}
	if (isa<ConstInt>(ogt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l2Const(cast<ConstInt>(ogt2->getSecondOperand()));
	if (!l2Const) {
		return false;
	}
	int64_t l2Bound = 0;
	if (!apsIntToInt64(l2Const->getValue(), &l2Bound)) {
		return false;
	}
	if (l2Bound <= l1Bound || l2Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainE) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt3(cast<GtOpExpr>(layer3->getFirstIfCond()));
	if (!ogt3) {
		return false;
	}
	if (isa<ConstInt>(ogt3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l3Const(cast<ConstInt>(ogt3->getSecondOperand()));
	if (!l3Const) {
		return false;
	}
	int64_t l3Bound = 0;
	if (!apsIntToInt64(l3Const->getValue(), &l3Bound)) {
		return false;
	}
	if (l3Bound <= l2Bound || l3Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainD) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt4(cast<GtOpExpr>(layer4->getFirstIfCond()));
	if (!ogt4) {
		return false;
	}
	if (isa<ConstInt>(ogt4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l4Const(cast<ConstInt>(ogt4->getSecondOperand()));
	if (!l4Const) {
		return false;
	}
	int64_t l4Bound = 0;
	if (!apsIntToInt64(l4Const->getValue(), &l4Bound)) {
		return false;
	}
	if (l4Bound <= l3Bound || l4Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer5(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer4->getElseClause()));
	if (!layer5 || !chainC) {
		return false;
	}
	if (layer5->hasElseIfClauses() || !layer5->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer5)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt5(cast<GtOpExpr>(layer5->getFirstIfCond()));
	if (!ogt5) {
		return false;
	}
	if (isa<ConstInt>(ogt5->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l5Const(cast<ConstInt>(ogt5->getSecondOperand()));
	if (!l5Const) {
		return false;
	}
	int64_t l5Bound = 0;
	if (!apsIntToInt64(l5Const->getValue(), &l5Bound)) {
		return false;
	}
	if (l5Bound <= l4Bound || l5Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt5->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer5->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer5->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailG));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlF(getControlExprIfConvertibleToSwitch(chainF));
	if (!ctrlF || !ctrlF->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstG = 0;
	unsigned nG = 0;
	if (!innerIsDenseEqChainConsecutive(tailG, ctrl, firstG, nG)) {
		return false;
	}
	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(chainF, ctrl, firstF, nF)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nG + nF + nE + nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstG > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nG)) {
		return false;
	}
	const int64_t lastG = firstG + static_cast<int64_t>(nG) - 1;
	if (lastG != l0Bound) {
		return false;
	}
	const int64_t expectFirstF = firstG + static_cast<int64_t>(nG);
	if (firstF != l0Bound + 1 || firstF != expectFirstF) {
		return false;
	}
	if (firstF > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nF)) {
		return false;
	}
	const int64_t lastF = firstF + static_cast<int64_t>(nF) - 1;
	if (lastF != l1Bound) {
		return false;
	}
	const int64_t expectFirstE = firstF + static_cast<int64_t>(nF);
	if (firstE != l1Bound + 1 || firstE != expectFirstE) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != l2Bound) {
		return false;
	}
	const int64_t expectFirstD = firstE + static_cast<int64_t>(nE);
	if (firstD != l2Bound + 1 || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != l3Bound) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != l3Bound + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != l4Bound) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != l4Bound + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != l5Bound) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != l5Bound + 1 || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {
		tailG, chainF, chainE, chainD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGtWithFiveLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt0(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt0) {
		return false;
	}
	if (isa<ConstInt>(ogt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l0Const(cast<ConstInt>(ogt0->getSecondOperand()));
	if (!l0Const) {
		return false;
	}
	int64_t l0Bound = 0;
	if (!apsIntToInt64(l0Const->getValue(), &l0Bound)) {
		return false;
	}
	if (l0Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt0->getFirstOperand());

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailF(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailF) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailF)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt1(cast<GtOpExpr>(layer1->getFirstIfCond()));
	if (!ogt1) {
		return false;
	}
	if (isa<ConstInt>(ogt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l1Const(cast<ConstInt>(ogt1->getSecondOperand()));
	if (!l1Const) {
		return false;
	}
	int64_t l1Bound = 0;
	if (!apsIntToInt64(l1Const->getValue(), &l1Bound)) {
		return false;
	}
	if (l1Bound <= l0Bound || l1Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainE) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt2(cast<GtOpExpr>(layer2->getFirstIfCond()));
	if (!ogt2) {
		return false;
	}
	if (isa<ConstInt>(ogt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l2Const(cast<ConstInt>(ogt2->getSecondOperand()));
	if (!l2Const) {
		return false;
	}
	int64_t l2Bound = 0;
	if (!apsIntToInt64(l2Const->getValue(), &l2Bound)) {
		return false;
	}
	if (l2Bound <= l1Bound || l2Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainD) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt3(cast<GtOpExpr>(layer3->getFirstIfCond()));
	if (!ogt3) {
		return false;
	}
	if (isa<ConstInt>(ogt3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l3Const(cast<ConstInt>(ogt3->getSecondOperand()));
	if (!l3Const) {
		return false;
	}
	int64_t l3Bound = 0;
	if (!apsIntToInt64(l3Const->getValue(), &l3Bound)) {
		return false;
	}
	if (l3Bound <= l2Bound || l3Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainC) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt4(cast<GtOpExpr>(layer4->getFirstIfCond()));
	if (!ogt4) {
		return false;
	}
	if (isa<ConstInt>(ogt4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l4Const(cast<ConstInt>(ogt4->getSecondOperand()));
	if (!l4Const) {
		return false;
	}
	int64_t l4Bound = 0;
	if (!apsIntToInt64(l4Const->getValue(), &l4Bound)) {
		return false;
	}
	if (l4Bound <= l3Bound || l4Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer4->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailF));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(tailF, ctrl, firstF, nF)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nF + nE + nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstF > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nF)) {
		return false;
	}
	const int64_t lastF = firstF + static_cast<int64_t>(nF) - 1;
	if (lastF != l0Bound) {
		return false;
	}
	const int64_t expectFirstE = firstF + static_cast<int64_t>(nF);
	if (firstE != l0Bound + 1 || firstE != expectFirstE) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != l1Bound) {
		return false;
	}
	const int64_t expectFirstD = firstE + static_cast<int64_t>(nE);
	if (firstD != l1Bound + 1 || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != l2Bound) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != l2Bound + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != l3Bound) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != l3Bound + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != l4Bound) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != l4Bound + 1 || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailF, chainE, chainD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGtWithFourLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt0(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt0) {
		return false;
	}
	if (isa<ConstInt>(ogt0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l0Const(cast<ConstInt>(ogt0->getSecondOperand()));
	if (!l0Const) {
		return false;
	}
	int64_t l0Bound = 0;
	if (!apsIntToInt64(l0Const->getValue(), &l0Bound)) {
		return false;
	}
	if (l0Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt0->getFirstOperand());

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailE(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailE) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailE)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt1(cast<GtOpExpr>(layer1->getFirstIfCond()));
	if (!ogt1) {
		return false;
	}
	if (isa<ConstInt>(ogt1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l1Const(cast<ConstInt>(ogt1->getSecondOperand()));
	if (!l1Const) {
		return false;
	}
	int64_t l1Bound = 0;
	if (!apsIntToInt64(l1Const->getValue(), &l1Bound)) {
		return false;
	}
	if (l1Bound <= l0Bound || l1Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainD) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt2(cast<GtOpExpr>(layer2->getFirstIfCond()));
	if (!ogt2) {
		return false;
	}
	if (isa<ConstInt>(ogt2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l2Const(cast<ConstInt>(ogt2->getSecondOperand()));
	if (!l2Const) {
		return false;
	}
	int64_t l2Bound = 0;
	if (!apsIntToInt64(l2Const->getValue(), &l2Bound)) {
		return false;
	}
	if (l2Bound <= l1Bound || l2Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainC) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt3(cast<GtOpExpr>(layer3->getFirstIfCond()));
	if (!ogt3) {
		return false;
	}
	if (isa<ConstInt>(ogt3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> l3Const(cast<ConstInt>(ogt3->getSecondOperand()));
	if (!l3Const) {
		return false;
	}
	int64_t l3Bound = 0;
	if (!apsIntToInt64(l3Const->getValue(), &l3Bound)) {
		return false;
	}
	if (l3Bound <= l2Bound || l3Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ogt3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer3->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailE));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(tailE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nE + nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != l0Bound) {
		return false;
	}
	const int64_t expectFirstD = firstE + static_cast<int64_t>(nE);
	if (firstD != l0Bound + 1 || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != l1Bound) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != l1Bound + 1 || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != l2Bound) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != l2Bound + 1 || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != l3Bound) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != l3Bound + 1 || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailE, chainD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeWithSixLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge0(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge0) {
		return false;
	}
	if (isa<ConstInt>(oge0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f0Const(cast<ConstInt>(oge0->getSecondOperand()));
	if (!f0Const) {
		return false;
	}
	int64_t f0Bound = 0;
	if (!apsIntToInt64(f0Const->getValue(), &f0Bound)) {
		return false;
	}
	if (f0Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge0->getFirstOperand());
	const GtEqOpExpr::Variant outerVariant = oge0->getVariant();

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailG(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailG) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailG)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge1(cast<GtEqOpExpr>(layer1->getFirstIfCond()));
	if (!oge1) {
		return false;
	}
	if (oge1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f1Const(cast<ConstInt>(oge1->getSecondOperand()));
	if (!f1Const) {
		return false;
	}
	int64_t f1Bound = 0;
	if (!apsIntToInt64(f1Const->getValue(), &f1Bound)) {
		return false;
	}
	if (f1Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!oge1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainF(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainF) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainF)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge2(cast<GtEqOpExpr>(layer2->getFirstIfCond()));
	if (!oge2) {
		return false;
	}
	if (oge2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f2Const(cast<ConstInt>(oge2->getSecondOperand()));
	if (!f2Const) {
		return false;
	}
	int64_t f2Bound = 0;
	if (!apsIntToInt64(f2Const->getValue(), &f2Bound)) {
		return false;
	}
	if (f2Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!oge2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainE) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge3(cast<GtEqOpExpr>(layer3->getFirstIfCond()));
	if (!oge3) {
		return false;
	}
	if (oge3->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f3Const(cast<ConstInt>(oge3->getSecondOperand()));
	if (!f3Const) {
		return false;
	}
	int64_t f3Bound = 0;
	if (!apsIntToInt64(f3Const->getValue(), &f3Bound)) {
		return false;
	}
	if (f3Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!oge3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainD) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge4(cast<GtEqOpExpr>(layer4->getFirstIfCond()));
	if (!oge4) {
		return false;
	}
	if (oge4->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f4Const(cast<ConstInt>(oge4->getSecondOperand()));
	if (!f4Const) {
		return false;
	}
	int64_t f4Bound = 0;
	if (!apsIntToInt64(f4Const->getValue(), &f4Bound)) {
		return false;
	}
	if (f4Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!oge4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer5(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer4->getElseClause()));
	if (!layer5 || !chainC) {
		return false;
	}
	if (layer5->hasElseIfClauses() || !layer5->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer5)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge5(cast<GtEqOpExpr>(layer5->getFirstIfCond()));
	if (!oge5) {
		return false;
	}
	if (oge5->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge5->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f5Const(cast<ConstInt>(oge5->getSecondOperand()));
	if (!f5Const) {
		return false;
	}
	int64_t f5Bound = 0;
	if (!apsIntToInt64(f5Const->getValue(), &f5Bound)) {
		return false;
	}
	if (f5Bound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!oge5->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer5->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer5->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailG));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlF(getControlExprIfConvertibleToSwitch(chainF));
	if (!ctrlF || !ctrlF->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstG = 0;
	unsigned nG = 0;
	if (!innerIsDenseEqChainConsecutive(tailG, ctrl, firstG, nG)) {
		return false;
	}
	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(chainF, ctrl, firstF, nF)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nG + nF + nE + nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstG > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nG)) {
		return false;
	}
	const int64_t lastG = firstG + static_cast<int64_t>(nG) - 1;
	if (lastG != f0Bound - 1) {
		return false;
	}
	const int64_t expectFirstF = firstG + static_cast<int64_t>(nG);
	if (firstF != f0Bound || firstF != expectFirstF) {
		return false;
	}
	if (firstF > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nF)) {
		return false;
	}
	const int64_t lastF = firstF + static_cast<int64_t>(nF) - 1;
	if (lastF != f1Bound - 1) {
		return false;
	}
	const int64_t expectFirstE = firstF + static_cast<int64_t>(nF);
	if (firstE != f1Bound || firstE != expectFirstE) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != f2Bound - 1) {
		return false;
	}
	const int64_t expectFirstD = firstE + static_cast<int64_t>(nE);
	if (firstD != f2Bound || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != f3Bound - 1) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != f3Bound || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != f4Bound - 1) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != f4Bound || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != f5Bound - 1) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != f5Bound || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {
		tailG, chainF, chainE, chainD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeWithFiveLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge0(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge0) {
		return false;
	}
	if (isa<ConstInt>(oge0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f0Const(cast<ConstInt>(oge0->getSecondOperand()));
	if (!f0Const) {
		return false;
	}
	int64_t f0Bound = 0;
	if (!apsIntToInt64(f0Const->getValue(), &f0Bound)) {
		return false;
	}
	if (f0Bound == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge0->getFirstOperand());
	const GtEqOpExpr::Variant outerVariant = oge0->getVariant();

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailF(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailF) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailF)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge1(cast<GtEqOpExpr>(layer1->getFirstIfCond()));
	if (!oge1) {
		return false;
	}
	if (oge1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f1Const(cast<ConstInt>(oge1->getSecondOperand()));
	if (!f1Const) {
		return false;
	}
	int64_t f1Bound = 0;
	if (!apsIntToInt64(f1Const->getValue(), &f1Bound)) {
		return false;
	}
	if (f1Bound <= f0Bound) {
		return false;
	}
	if (!oge1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainE(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainE) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainE)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge2(cast<GtEqOpExpr>(layer2->getFirstIfCond()));
	if (!oge2) {
		return false;
	}
	if (oge2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f2Const(cast<ConstInt>(oge2->getSecondOperand()));
	if (!f2Const) {
		return false;
	}
	int64_t f2Bound = 0;
	if (!apsIntToInt64(f2Const->getValue(), &f2Bound)) {
		return false;
	}
	if (f2Bound <= f1Bound) {
		return false;
	}
	if (!oge2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainD) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge3(cast<GtEqOpExpr>(layer3->getFirstIfCond()));
	if (!oge3) {
		return false;
	}
	if (oge3->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f3Const(cast<ConstInt>(oge3->getSecondOperand()));
	if (!f3Const) {
		return false;
	}
	int64_t f3Bound = 0;
	if (!apsIntToInt64(f3Const->getValue(), &f3Bound)) {
		return false;
	}
	if (f3Bound <= f2Bound) {
		return false;
	}
	if (!oge3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer4(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer3->getElseClause()));
	if (!layer4 || !chainC) {
		return false;
	}
	if (layer4->hasElseIfClauses() || !layer4->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer4)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge4(cast<GtEqOpExpr>(layer4->getFirstIfCond()));
	if (!oge4) {
		return false;
	}
	if (oge4->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge4->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f4Const(cast<ConstInt>(oge4->getSecondOperand()));
	if (!f4Const) {
		return false;
	}
	int64_t f4Bound = 0;
	if (!apsIntToInt64(f4Const->getValue(), &f4Bound)) {
		return false;
	}
	if (f4Bound <= f3Bound) {
		return false;
	}
	if (!oge4->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer4->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer4->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailF));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlE(getControlExprIfConvertibleToSwitch(chainE));
	if (!ctrlE || !ctrlE->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstF = 0;
	unsigned nF = 0;
	if (!innerIsDenseEqChainConsecutive(tailF, ctrl, firstF, nF)) {
		return false;
	}
	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(chainE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nF + nE + nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstF > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nF)) {
		return false;
	}
	const int64_t lastF = firstF + static_cast<int64_t>(nF) - 1;
	if (lastF != f0Bound - 1) {
		return false;
	}
	const int64_t expectFirstE = firstF + static_cast<int64_t>(nF);
	if (firstE != f0Bound || firstE != expectFirstE) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != f1Bound - 1) {
		return false;
	}
	const int64_t expectFirstD = firstE + static_cast<int64_t>(nE);
	if (firstD != f1Bound || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != f2Bound - 1) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != f2Bound || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != f3Bound - 1) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != f3Bound || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != f4Bound - 1) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != f4Bound || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailF, chainE, chainD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeWithFourLevelNestedSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge0(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge0) {
		return false;
	}
	if (isa<ConstInt>(oge0->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f0Const(cast<ConstInt>(oge0->getSecondOperand()));
	if (!f0Const) {
		return false;
	}
	int64_t f0Bound = 0;
	if (!apsIntToInt64(f0Const->getValue(), &f0Bound)) {
		return false;
	}
	if (f0Bound == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge0->getFirstOperand());
	const GtEqOpExpr::Variant outerVariant = oge0->getVariant();

	ShPtr<IfStmt> layer1(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailE(cast<IfStmt>(outer->getElseClause()));
	if (!layer1 || !tailE) {
		return false;
	}
	if (layer1->hasElseIfClauses() || !layer1->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer1)
			|| BreakInIfAnalysis::hasBreakStmt(tailE)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge1(cast<GtEqOpExpr>(layer1->getFirstIfCond()));
	if (!oge1) {
		return false;
	}
	if (oge1->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge1->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f1Const(cast<ConstInt>(oge1->getSecondOperand()));
	if (!f1Const) {
		return false;
	}
	int64_t f1Bound = 0;
	if (!apsIntToInt64(f1Const->getValue(), &f1Bound)) {
		return false;
	}
	if (f1Bound <= f0Bound) {
		return false;
	}
	if (!oge1->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer2(cast<IfStmt>(layer1->getFirstIfBody()));
	ShPtr<IfStmt> chainD(cast<IfStmt>(layer1->getElseClause()));
	if (!layer2 || !chainD) {
		return false;
	}
	if (layer2->hasElseIfClauses() || !layer2->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer2)
			|| BreakInIfAnalysis::hasBreakStmt(chainD)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge2(cast<GtEqOpExpr>(layer2->getFirstIfCond()));
	if (!oge2) {
		return false;
	}
	if (oge2->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge2->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f2Const(cast<ConstInt>(oge2->getSecondOperand()));
	if (!f2Const) {
		return false;
	}
	int64_t f2Bound = 0;
	if (!apsIntToInt64(f2Const->getValue(), &f2Bound)) {
		return false;
	}
	if (f2Bound <= f1Bound) {
		return false;
	}
	if (!oge2->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> layer3(cast<IfStmt>(layer2->getFirstIfBody()));
	ShPtr<IfStmt> chainC(cast<IfStmt>(layer2->getElseClause()));
	if (!layer3 || !chainC) {
		return false;
	}
	if (layer3->hasElseIfClauses() || !layer3->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(layer3)
			|| BreakInIfAnalysis::hasBreakStmt(chainC)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge3(cast<GtEqOpExpr>(layer3->getFirstIfCond()));
	if (!oge3) {
		return false;
	}
	if (oge3->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(oge3->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> f3Const(cast<ConstInt>(oge3->getSecondOperand()));
	if (!f3Const) {
		return false;
	}
	int64_t f3Bound = 0;
	if (!apsIntToInt64(f3Const->getValue(), &f3Bound)) {
		return false;
	}
	if (f3Bound <= f2Bound) {
		return false;
	}
	if (!oge3->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> chainA(cast<IfStmt>(layer3->getFirstIfBody()));
	ShPtr<IfStmt> chainB(cast<IfStmt>(layer3->getElseClause()));
	if (!chainA || !chainB) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(chainA)
			|| BreakInIfAnalysis::hasBreakStmt(chainB)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(tailE));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlD(getControlExprIfConvertibleToSwitch(chainD));
	if (!ctrlD || !ctrlD->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlC(getControlExprIfConvertibleToSwitch(chainC));
	if (!ctrlC || !ctrlC->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlB(getControlExprIfConvertibleToSwitch(chainB));
	if (!ctrlB || !ctrlB->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlA(getControlExprIfConvertibleToSwitch(chainA));
	if (!ctrlA || !ctrlA->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(tailE, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstD = 0;
	unsigned nD = 0;
	if (!innerIsDenseEqChainConsecutive(chainD, ctrl, firstD, nD)) {
		return false;
	}
	int64_t firstC = 0;
	unsigned nC = 0;
	if (!innerIsDenseEqChainConsecutive(chainC, ctrl, firstC, nC)) {
		return false;
	}
	int64_t firstB = 0;
	unsigned nB = 0;
	if (!innerIsDenseEqChainConsecutive(chainB, ctrl, firstB, nB)) {
		return false;
	}
	int64_t firstA = 0;
	unsigned nA = 0;
	if (!innerIsDenseEqChainConsecutive(chainA, ctrl, firstA, nA)) {
		return false;
	}

	if (nE + nD + nC + nB + nA > 256) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	if (lastE != f0Bound - 1) {
		return false;
	}
	const int64_t expectFirstD = firstE + static_cast<int64_t>(nE);
	if (firstD != f0Bound || firstD != expectFirstD) {
		return false;
	}
	if (firstD > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nD)) {
		return false;
	}
	const int64_t lastD = firstD + static_cast<int64_t>(nD) - 1;
	if (lastD != f1Bound - 1) {
		return false;
	}
	const int64_t expectFirstC = firstD + static_cast<int64_t>(nD);
	if (firstC != f1Bound || firstC != expectFirstC) {
		return false;
	}
	if (firstC > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nC)) {
		return false;
	}
	const int64_t lastC = firstC + static_cast<int64_t>(nC) - 1;
	if (lastC != f2Bound - 1) {
		return false;
	}
	const int64_t expectFirstB = firstC + static_cast<int64_t>(nC);
	if (firstB != f2Bound || firstB != expectFirstB) {
		return false;
	}
	if (firstB > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nB)) {
		return false;
	}
	const int64_t lastB = firstB + static_cast<int64_t>(nB) - 1;
	if (lastB != f3Bound - 1) {
		return false;
	}
	const int64_t expectFirstA = firstB + static_cast<int64_t>(nB);
	if (firstA != f3Bound || firstA != expectFirstA) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailE, chainD, chainC, chainB, chainA};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}
bool IfToSwitchOptimizer::tryConvertGtWithNestedGtSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt) {
		return false;
	}
	if (isa<ConstInt>(ogt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> loConst(cast<ConstInt>(ogt->getSecondOperand()));
	if (!loConst) {
		return false;
	}
	int64_t loBound = 0;
	if (!apsIntToInt64(loConst->getValue(), &loBound)) {
		return false;
	}
	if (loBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt->getFirstOperand());

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<GtOpExpr> igt(cast<GtOpExpr>(midIf->getFirstIfCond()));
	if (!igt) {
		return false;
	}
	if (isa<ConstInt>(igt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> liConst(cast<ConstInt>(igt->getSecondOperand()));
	if (!liConst) {
		return false;
	}
	int64_t liBound = 0;
	if (!apsIntToInt64(liConst->getValue(), &liBound)) {
		return false;
	}
	if (liBound <= loBound || liBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!igt->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getElseClause()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getFirstIfBody()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	const int64_t expectFirstLow = loBound + 1;
	if (firstLow != expectFirstLow) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t lastLow = firstLow + static_cast<int64_t>(nLow) - 1;
	if (lastLow != liBound) {
		return false;
	}
	const int64_t expectFirstHigh = liBound + 1;
	if (firstHigh != expectFirstHigh || firstHigh != firstLow + static_cast<int64_t>(nLow)) {
		return false;
	}
	if (firstTail + static_cast<int64_t>(nTail) - 1 != loBound) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailRight, lowInner, highInner};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeWithNestedGeSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge) {
		return false;
	}
	if (isa<ConstInt>(oge->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> loConst(cast<ConstInt>(oge->getSecondOperand()));
	if (!loConst) {
		return false;
	}
	int64_t loBound = 0;
	if (!apsIntToInt64(loConst->getValue(), &loBound)) {
		return false;
	}
	if (loBound == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge->getFirstOperand());
	const GtEqOpExpr::Variant outerVariant = oge->getVariant();

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<GtEqOpExpr> ige(cast<GtEqOpExpr>(midIf->getFirstIfCond()));
	if (!ige) {
		return false;
	}
	if (ige->getVariant() != outerVariant) {
		return false;
	}
	if (isa<ConstInt>(ige->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> hConst(cast<ConstInt>(ige->getSecondOperand()));
	if (!hConst) {
		return false;
	}
	int64_t hBound = 0;
	if (!apsIntToInt64(hConst->getValue(), &hBound)) {
		return false;
	}
	if (hBound <= loBound) {
		return false;
	}
	if (!ige->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getElseClause()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getFirstIfBody()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	if (firstLow != loBound) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t lastLow = firstLow + static_cast<int64_t>(nLow) - 1;
	if (lastLow != hBound - 1) {
		return false;
	}
	if (firstHigh != hBound || firstHigh != firstLow + static_cast<int64_t>(nLow)) {
		return false;
	}
	if (firstTail + static_cast<int64_t>(nTail) - 1 != loBound - 1) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailRight, lowInner, highInner};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGtWithNestedGeSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt) {
		return false;
	}
	if (isa<ConstInt>(ogt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> loConst(cast<ConstInt>(ogt->getSecondOperand()));
	if (!loConst) {
		return false;
	}
	int64_t loBound = 0;
	if (!apsIntToInt64(loConst->getValue(), &loBound)) {
		return false;
	}
	if (loBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt->getFirstOperand());

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<GtEqOpExpr> ige(cast<GtEqOpExpr>(midIf->getFirstIfCond()));
	if (!ige) {
		return false;
	}
	if (isa<ConstInt>(ige->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> hConst(cast<ConstInt>(ige->getSecondOperand()));
	if (!hConst) {
		return false;
	}
	int64_t hBound = 0;
	if (!apsIntToInt64(hConst->getValue(), &hBound)) {
		return false;
	}
	if (hBound <= loBound + 1
			|| hBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!ige->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getElseClause()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getFirstIfBody()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	const int64_t expectFirstLow = loBound + 1;
	if (firstLow != expectFirstLow) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t lastLow = firstLow + static_cast<int64_t>(nLow) - 1;
	if (lastLow != hBound - 1) {
		return false;
	}
	if (firstHigh != hBound || firstHigh != firstLow + static_cast<int64_t>(nLow)) {
		return false;
	}
	if (firstTail + static_cast<int64_t>(nTail) - 1 != loBound) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailRight, lowInner, highInner};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeWithNestedGtSplitInThen(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge) {
		return false;
	}
	if (isa<ConstInt>(oge->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> loConst(cast<ConstInt>(oge->getSecondOperand()));
	if (!loConst) {
		return false;
	}
	int64_t loBound = 0;
	if (!apsIntToInt64(loConst->getValue(), &loBound)) {
		return false;
	}
	if (loBound == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge->getFirstOperand());

	ShPtr<IfStmt> midIf(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> tailRight(cast<IfStmt>(outer->getElseClause()));
	if (!midIf || !tailRight) {
		return false;
	}
	if (midIf->hasElseIfClauses() || !midIf->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(midIf)
			|| BreakInIfAnalysis::hasBreakStmt(tailRight)) {
		return false;
	}

	ShPtr<GtOpExpr> igt(cast<GtOpExpr>(midIf->getFirstIfCond()));
	if (!igt) {
		return false;
	}
	if (isa<ConstInt>(igt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> hiConst(cast<ConstInt>(igt->getSecondOperand()));
	if (!hiConst) {
		return false;
	}
	int64_t hiBound = 0;
	if (!apsIntToInt64(hiConst->getValue(), &hiBound)) {
		return false;
	}
	if (hiBound < loBound || hiBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	if (!igt->getFirstOperand()->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<IfStmt> lowInner(cast<IfStmt>(midIf->getElseClause()));
	ShPtr<IfStmt> highInner(cast<IfStmt>(midIf->getFirstIfBody()));
	if (!lowInner || !highInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(lowInner)
			|| BreakInIfAnalysis::hasBreakStmt(highInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(lowInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlH(getControlExprIfConvertibleToSwitch(highInner));
	if (!ctrlH || !ctrlH->isEqualTo(ctrl)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(tailRight));
	if (!ctrlT || !ctrlT->isEqualTo(ctrl)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstLow = 0;
	unsigned nLow = 0;
	if (!innerIsDenseEqChainConsecutive(lowInner, ctrl, firstLow, nLow)) {
		return false;
	}
	int64_t firstHigh = 0;
	unsigned nHigh = 0;
	if (!innerIsDenseEqChainConsecutive(highInner, ctrl, firstHigh, nHigh)) {
		return false;
	}
	int64_t firstTail = 0;
	unsigned nTail = 0;
	if (!innerIsDenseEqChainConsecutive(tailRight, ctrl, firstTail, nTail)) {
		return false;
	}

	if (nLow + nHigh + nTail > 256) {
		return false;
	}
	if (firstLow != loBound) {
		return false;
	}
	if (firstLow > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nLow)) {
		return false;
	}
	const int64_t lastLow = firstLow + static_cast<int64_t>(nLow) - 1;
	if (lastLow != hiBound) {
		return false;
	}
	const int64_t expectFirstHigh = hiBound + 1;
	if (firstHigh != expectFirstHigh
			|| firstHigh != firstLow + static_cast<int64_t>(nLow)) {
		return false;
	}
	if (firstTail + static_cast<int64_t>(nTail) - 1 != loBound - 1) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> parts[] = {tailRight, lowInner, highInner};
	for (ShPtr<IfStmt> part : parts) {
		for (auto i = part->clause_begin(), e = part->clause_end(); i != e;
				++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLtSplitTwoConsecutiveInnerEqChains(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtOpExpr> olt(cast<LtOpExpr>(outer->getFirstIfCond()));
	if (!olt) {
		return false;
	}
	if (isa<ConstInt>(olt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> midConst(cast<ConstInt>(olt->getSecondOperand()));
	if (!midConst) {
		return false;
	}
	int64_t mid = 0;
	if (!apsIntToInt64(midConst->getValue(), &mid)) {
		return false;
	}
	ShPtr<Expression> outerLhs(olt->getFirstOperand());

	ShPtr<IfStmt> leftInner(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> rightInner(cast<IfStmt>(outer->getElseClause()));
	if (!leftInner || !rightInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(leftInner)
			|| BreakInIfAnalysis::hasBreakStmt(rightInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(leftInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlR(getControlExprIfConvertibleToSwitch(rightInner));
	if (!ctrlR || !ctrlR->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstL = 0;
	unsigned nL = 0;
	if (!innerIsDenseEqChainConsecutive(leftInner, ctrl, firstL, nL)) {
		return false;
	}
	int64_t firstR = 0;
	unsigned nR = 0;
	if (!innerIsDenseEqChainConsecutive(rightInner, ctrl, firstR, nR)) {
		return false;
	}
	if (nL + nR > 256) {
		return false;
	}
	if (firstL > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nL)) {
		return false;
	}
	const int64_t splitAt = firstL + static_cast<int64_t>(nL);
	if (firstR != splitAt || mid != splitAt) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> inners[] = {leftInner, rightInner};
	for (ShPtr<IfStmt> inner : inners) {
		for (auto i = inner->clause_begin(), e = inner->clause_end();
				i != e; ++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertLeSplitTwoConsecutiveInnerEqChains(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<LtEqOpExpr> ole(cast<LtEqOpExpr>(outer->getFirstIfCond()));
	if (!ole) {
		return false;
	}
	if (isa<ConstInt>(ole->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> ubConst(cast<ConstInt>(ole->getSecondOperand()));
	if (!ubConst) {
		return false;
	}
	int64_t ub = 0;
	if (!apsIntToInt64(ubConst->getValue(), &ub)) {
		return false;
	}
	if (ub == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ole->getFirstOperand());

	ShPtr<IfStmt> leftInner(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> rightInner(cast<IfStmt>(outer->getElseClause()));
	if (!leftInner || !rightInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(leftInner)
			|| BreakInIfAnalysis::hasBreakStmt(rightInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(leftInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlR(getControlExprIfConvertibleToSwitch(rightInner));
	if (!ctrlR || !ctrlR->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstL = 0;
	unsigned nL = 0;
	if (!innerIsDenseEqChainConsecutive(leftInner, ctrl, firstL, nL)) {
		return false;
	}
	int64_t firstR = 0;
	unsigned nR = 0;
	if (!innerIsDenseEqChainConsecutive(rightInner, ctrl, firstR, nR)) {
		return false;
	}
	if (nL + nR > 256) {
		return false;
	}
	if (firstL > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nL)) {
		return false;
	}
	const int64_t lastL = firstL + static_cast<int64_t>(nL) - 1;
	const int64_t expectFirstR = firstL + static_cast<int64_t>(nL);
	if (lastL != ub || firstR != ub + 1 || firstR != expectFirstR) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> inners[] = {leftInner, rightInner};
	for (ShPtr<IfStmt> inner : inners) {
		for (auto i = inner->clause_begin(), e = inner->clause_end();
				i != e; ++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGtSplitTwoConsecutiveInnerEqChains(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtOpExpr> ogt(cast<GtOpExpr>(outer->getFirstIfCond()));
	if (!ogt) {
		return false;
	}
	if (isa<ConstInt>(ogt->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> rConst(cast<ConstInt>(ogt->getSecondOperand()));
	if (!rConst) {
		return false;
	}
	int64_t rBound = 0;
	if (!apsIntToInt64(rConst->getValue(), &rBound)) {
		return false;
	}
	if (rBound == std::numeric_limits<int64_t>::max()) {
		return false;
	}
	ShPtr<Expression> outerLhs(ogt->getFirstOperand());

	ShPtr<IfStmt> thenInner(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> elseInner(cast<IfStmt>(outer->getElseClause()));
	if (!thenInner || !elseInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(thenInner)
			|| BreakInIfAnalysis::hasBreakStmt(elseInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(elseInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(thenInner));
	if (!ctrlT || !ctrlT->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(elseInner, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstT = 0;
	unsigned nT = 0;
	if (!innerIsDenseEqChainConsecutive(thenInner, ctrl, firstT, nT)) {
		return false;
	}
	if (nE + nT > 256) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)
			+ 1) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	const int64_t expectFirstT = firstE + static_cast<int64_t>(nE);
	if (lastE != rBound || firstT != rBound + 1 || firstT != expectFirstT) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> inners[] = {elseInner, thenInner};
	for (ShPtr<IfStmt> inner : inners) {
		for (auto i = inner->clause_begin(), e = inner->clause_end();
				i != e; ++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

bool IfToSwitchOptimizer::tryConvertGeSplitTwoConsecutiveInnerEqChains(
		ShPtr<IfStmt> outer) {
	if (outer->hasElseIfClauses() || !outer->hasElseClause()) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(outer)) {
		return false;
	}

	ShPtr<GtEqOpExpr> oge(cast<GtEqOpExpr>(outer->getFirstIfCond()));
	if (!oge) {
		return false;
	}
	if (isa<ConstInt>(oge->getFirstOperand())) {
		return false;
	}
	ShPtr<ConstInt> fConst(cast<ConstInt>(oge->getSecondOperand()));
	if (!fConst) {
		return false;
	}
	int64_t fBound = 0;
	if (!apsIntToInt64(fConst->getValue(), &fBound)) {
		return false;
	}
	if (fBound == std::numeric_limits<int64_t>::min()) {
		return false;
	}
	ShPtr<Expression> outerLhs(oge->getFirstOperand());

	ShPtr<IfStmt> thenInner(cast<IfStmt>(outer->getFirstIfBody()));
	ShPtr<IfStmt> elseInner(cast<IfStmt>(outer->getElseClause()));
	if (!thenInner || !elseInner) {
		return false;
	}
	if (BreakInIfAnalysis::hasBreakStmt(thenInner)
			|| BreakInIfAnalysis::hasBreakStmt(elseInner)) {
		return false;
	}

	ShPtr<Expression> ctrl(getControlExprIfConvertibleToSwitch(elseInner));
	if (!ctrl || !ctrl->isEqualTo(outerLhs)) {
		return false;
	}
	ShPtr<Expression> ctrlT(getControlExprIfConvertibleToSwitch(thenInner));
	if (!ctrlT || !ctrlT->isEqualTo(outerLhs)) {
		return false;
	}

	ShPtr<ValueData> ctrlData(va->getValueData(ctrl));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses()
			|| ctrlData->hasDerefs()) {
		return false;
	}

	int64_t firstE = 0;
	unsigned nE = 0;
	if (!innerIsDenseEqChainConsecutive(elseInner, ctrl, firstE, nE)) {
		return false;
	}
	int64_t firstT = 0;
	unsigned nT = 0;
	if (!innerIsDenseEqChainConsecutive(thenInner, ctrl, firstT, nT)) {
		return false;
	}
	if (nE + nT > 256) {
		return false;
	}
	if (firstE > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(nE)
			+ 1) {
		return false;
	}
	const int64_t lastE = firstE + static_cast<int64_t>(nE) - 1;
	const int64_t expectFirstT = firstE + static_cast<int64_t>(nE);
	if (lastE != fBound - 1 || firstT != fBound || firstT != expectFirstT) {
		return false;
	}

	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(ctrl, nullptr, outer->getAddress()));

	const ShPtr<IfStmt> inners[] = {elseInner, thenInner};
	for (ShPtr<IfStmt> inner : inners) {
		for (auto i = inner->clause_begin(), e = inner->clause_end();
				i != e; ++i) {
			ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
			ShPtr<Expression> caseVal;
			if (eqOpExpr->getFirstOperand()->isEqualTo(ctrl)) {
				caseVal = eqOpExpr->getSecondOperand();
			} else {
				caseVal = eqOpExpr->getFirstOperand();
			}
			ShPtr<Statement> body(i->second);
			appendBreakStmtIfNeeded(Statement::getLastStatement(body));
			switchStmt->addClause(caseVal, body);
		}
	}

	Statement::replaceStatement(outer, switchStmt);
	return true;
}

/**
* @brief Try to convert a sequential chain of single-clause if statements
*        into a switch statement.
*
* Handles the pattern produced by compiled jump tables:
*   if (v == 0) { stmts; }
*   if (v == 1) { stmts; }
*   ...
*
* Requirements:
*  - Each if must have exactly one clause (no else-if, no else).
*  - Each condition must be (same_var == ConstInt).
*  - The same variable must appear in all conditions.
*  - No break statements inside any if body (would escape the wrong loop).
*  - At least 2 consecutive ifs (heuristic: single if stays as if).
*
* @param[in] firstIf The first IfStmt in the potential chain.
*/
void IfToSwitchOptimizer::tryConvertSequentialIfChainToSwitch(
		ShPtr<IfStmt> firstIf) {
	// Must be a single-clause if (no else-if, no else).
	if (firstIf->hasElseIfClauses() || firstIf->hasElseClause()) {
		return;
	}
	if (BreakInIfAnalysis::hasBreakStmt(firstIf)) {
		return;
	}

	// Extract control expression from first if.
	ShPtr<EqOpExpr> firstEq(cast<EqOpExpr>(firstIf->getFirstIfCond()));
	if (!firstEq) {
		return;
	}
	ShPtr<Expression> controlExpr(getNextOpIfSecondOneIsConstInt(firstEq));
	if (!controlExpr) {
		return;
	}
	ShPtr<ValueData> ctrlData(va->getValueData(controlExpr));
	if (ctrlData->hasCalls() || ctrlData->hasArrayAccesses() ||
			ctrlData->hasDerefs()) {
		return;
	}

	// Collect the chain.
	std::set<llvm::APSInt> seenValues;
	std::vector<ShPtr<IfStmt>> chain;
	chain.push_back(firstIf);

	ShPtr<Statement> cur(firstIf->getSuccessor());
	while (cur) {
		ShPtr<IfStmt> nextIf(cast<IfStmt>(cur));
		if (!nextIf) break;
		if (nextIf->hasElseIfClauses() || nextIf->hasElseClause()) break;
		if (BreakInIfAnalysis::hasBreakStmt(nextIf)) break;

		ShPtr<EqOpExpr> eq(cast<EqOpExpr>(nextIf->getFirstIfCond()));
		if (!eq) break;

		ShPtr<Expression> cand(getNextOpIfSecondOneIsConstInt(eq));
		if (!cand || !cand->isEqualTo(controlExpr)) break;

		chain.push_back(nextIf);
		cur = nextIf->getSuccessor();
	}

	// Require at least 2 consecutive ifs (one if alone is not a switch).
	if (chain.size() < 2) {
		return;
	}

	// Build the switch statement replacing the entire chain.
	ShPtr<SwitchStmt> switchStmt(
		SwitchStmt::create(controlExpr, nullptr, firstIf->getAddress()));

	for (auto &ifStmt : chain) {
		ShPtr<EqOpExpr> eq(cast<EqOpExpr>(ifStmt->getFirstIfCond()));
		ShPtr<Expression> caseVal;
		if (eq->getFirstOperand()->isEqualTo(controlExpr)) {
			caseVal = eq->getSecondOperand();
		} else {
			caseVal = eq->getFirstOperand();
		}

		ShPtr<Statement> body(ifStmt->getFirstIfBody());
		appendBreakStmtIfNeeded(Statement::getLastStatement(body));
		switchStmt->addClause(caseVal, body);
	}

	// Replace the first if with the switch; remove remaining ifs in chain.
	Statement::replaceStatement(firstIf, switchStmt);
	for (std::size_t i = 1; i < chain.size(); ++i) {
		Statement::removeStatement(chain[i]);
	}
}

/**
* @brief Append BreakStmt after @a stmt when @a stmt is not a ContinueStmt
*        or ReturnStmt or GotoStmt.
*
* @param[in] stmt Statement on which is BreakStmt append.
*/
void IfToSwitchOptimizer::appendBreakStmtIfNeeded(ShPtr<Statement> stmt) {
	if (!isa<ContinueStmt>(stmt) && !isa<ReturnStmt>(stmt) &&
			!isa<GotoStmt>(stmt)) {
		stmt->setSuccessor(BreakStmt::create(stmt->getAddress()));
	}
}

/**
* @brief Check if one of the operands is a ConstInt. If so, then return the
*        next one operand.
*
* @param[in] eqOpExpr EqOpExpr to check.
*
* @return If one of the operands is a ConstInt, than return the second
*         operand. Otherwise return the null pointer.
*/
ShPtr<Expression> IfToSwitchOptimizer::getNextOpIfSecondOneIsConstInt(
		ShPtr<EqOpExpr> eqOpExpr) {
	if (isa<ConstInt>(eqOpExpr->getFirstOperand())) {
		return eqOpExpr->getSecondOperand();
	} else if (isa<ConstInt>(eqOpExpr->getSecondOperand())) {
		return eqOpExpr->getFirstOperand();
	} else {
		return ShPtr<Expression>();
	}
}

} // namespace llvmir2hll
} // namespace retdec
