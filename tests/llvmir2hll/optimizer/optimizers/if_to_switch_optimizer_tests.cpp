/**
* @file tests/llvmir2hll/optimizer/optimizers/if_to_switch_optimizer_tests.cpp
* @brief Tests for the @c if_to_switch_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include <vector>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/array_index_op_expr.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/deref_op_expr.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_op_expr.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/if_to_switch_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c if_to_switch_optimizer module.
*/
class IfToSwitchOptimizerTests: public TestsWithModule {
protected:
	void checkCorrectConvertIfToSwitch(ShPtr<IfStmt> ifStmt, ShPtr<Statement>
		stmt, bool isBreakNeeded);
};

/**
* @brief Check if @a ifStmt was correctly transformed to @c SwitchStmt.
*
* @param[in] ifStmt If statement to check.
* @param[in] stmt Optimized statement to compare.
* @param[in] isBreakNeeded Set control to if break is in case clauses needed.
*/
void IfToSwitchOptimizerTests::checkCorrectConvertIfToSwitch(ShPtr<IfStmt> ifStmt,
		ShPtr<Statement> stmt, bool isBreakNeeded) {
	ShPtr<SwitchStmt> outSwitchStmt(cast<SwitchStmt>(stmt));
	ASSERT_TRUE(outSwitchStmt) <<
		"expected `SwitchStmt`, "
		"got `" << ifStmt << "`";

	// Check control expression.
	ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(ifStmt->getFirstIfCond()));
	ShPtr<Expression> controlExpr;
	if (isa<ConstInt>(eqOpExpr->getFirstOperand())) {
		controlExpr = eqOpExpr->getSecondOperand();
	} else {
		controlExpr = eqOpExpr->getFirstOperand();
	}
	ASSERT_EQ(controlExpr, outSwitchStmt->getControlExpr()) <<
		"expected `" << controlExpr << "`, "
		"got `" << outSwitchStmt->getControlExpr() << "`";

	// Check correctness transform if clauses to switch clauses.
	auto switchIt = outSwitchStmt->clause_begin();
	for (auto i = ifStmt->clause_begin(), e = ifStmt->clause_end(); i != e; ++i) {
		ShPtr<EqOpExpr> eqOpExpr(cast<EqOpExpr>(i->first));
		ShPtr<ConstInt> constant(cast<ConstInt>(eqOpExpr->getFirstOperand()));
		if (!constant) {
			constant = cast<ConstInt>(eqOpExpr->getSecondOperand());
		}
		ASSERT_EQ(constant, switchIt->first) <<
			"expected `" << constant << "`, "
			"got `" << switchIt->first << "`";

		ShPtr<BreakStmt> outBreakStmt(cast<BreakStmt>(Statement::
			getLastStatement(i->second)));
		if (isBreakNeeded) {
			ASSERT_TRUE(outBreakStmt) <<
				"expected `BreakStmt`";
		} else {
			ASSERT_FALSE(outBreakStmt) <<
				"not expected `BreakStmt`";
		}
		switchIt++;
	}

	if (ifStmt->hasElseClause()) {
		// Check correctness transform of else clause.
		ASSERT_TRUE(outSwitchStmt->hasDefaultClause()) <<
			"expected that `" << outSwitchStmt <<
			"` has default clause";
		ShPtr<BreakStmt> outBreakStmt(cast<BreakStmt>(Statement::getLastStatement(
			outSwitchStmt->getDefaultClauseBody())));
		ASSERT_TRUE(outBreakStmt) <<
			"expected `BreakStmt`";
	}
}

TEST_F(IfToSwitchOptimizerTests,
OptimizerHasNonEmptyID) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	ShPtr<IfToSwitchOptimizer> optimizer(new IfToSwitchOptimizer(module, va));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(IfToSwitchOptimizerTests,
NotSameControlExprInElseIfClausesNotOptimize) {
	// if (a == 5) {
	//     b = b + 3;
	// } else if (b + 3 == 6) {
	//     b = b + 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(16)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			varA,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
OnlyIfConditionNotOptimize) {
	// if (a == 5) {
	//     b = b + 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(16)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			varA,
			ConstInt::create(5, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
SameControlExprButNoEqOpExprNotOptimize) {
	// if (b + 3) {
	//     b = b + 3;
	// } else if (b + 3) {
	//     b = b + 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(addOpExpr, assignStmt));
	ifStmt->addClause(addOpExpr, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
SameControlExprButNotConstIntOperandNotOptimize) {
	// if (a == a) {
	//     b = b + 3;
	// } else if (b + 3 == b) {
	//     b = b + 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(16)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			varA,
			varA
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			addOpExpr,
			varB
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
SameControlExprButBreakInIfStmtNotOptimize) {
	// if (b + 3 == 2) {
	//     break;
	// } else if (b + 3 == 3) {
	//     b = b + 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(16)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(2, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(3, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, BreakStmt::create()));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
SameControlExprInElseIfClausesButDerefOpExprInControlExprNotOptimize) {
	// if (*a == 5) {
	//     a = 3;
	// } else if (*a == 6) {
	//     a = 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(16)));
	ShPtr<DerefOpExpr> derefOpExpr(DerefOpExpr::create(varA));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			derefOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			derefOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varA,
			ConstInt::create(3, 64)
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
SameControlExprInElseIfClausesButArrayIndexOpExprInControlExprNotOptimize) {
	// if (a[3] == 5) {
	//     a = 3;
	// } else if (a[3] == 6) {
	//     a = 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(16)));
	ShPtr<ArrayIndexOpExpr> arrayIndexOpExpr(ArrayIndexOpExpr::create(varA,
		ConstInt::create(3, 64)));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			arrayIndexOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			arrayIndexOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varA,
			ConstInt::create(3, 64)
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
SameControlExprInElseIfClausesButFunctionCallInControlExprNotOptimize) {
	// if (func() == 5) {
	//     a = 3;
	// } else if (func() == 6) {
	//     a = 3;
	// }
	//
	// Not optimized
	//
	ShPtr<Variable> varA(Variable::create("a", IntType::create(16)));
	ExprVector args;
	args.push_back(varA);
	ShPtr<CallExpr> callExpr(CallExpr::create(varA, args));
	ShPtr<ArrayIndexOpExpr> arrayIndexOpExpr(ArrayIndexOpExpr::create(varA,
		ConstInt::create(3, 64)));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			callExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			callExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varA,
			ConstInt::create(3, 64)
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ASSERT_TRUE(testFunc->getBody()) << "expected a non-empty body";
	ShPtr<IfStmt> outIfStmt(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(outIfStmt) <<
		"expected `IfStmt`, "
		"got `" << testFunc->getBody() << "`";
}

TEST_F(IfToSwitchOptimizerTests,
SimpleSubstituteIfToSwitchOptimize) {
	// if (b + 3 == 5) {
	//     b = b + 3;
	// } else if (b + 3 == 6) {
	//     b = b + 3;
	// }
	//
	// Optimized to:
	// switch(b + 3) {
	//     case 5: b = b + 3; break;
	//     case 6: b = b + 3; break;
	// }
	//
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<AddOpExpr> addOpExpr2(
		AddOpExpr::create(
			varB,
			ConstInt::create(4, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<AssignStmt> assignStmt2(
		AssignStmt::create(
			varB,
			addOpExpr2
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt2);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	checkCorrectConvertIfToSwitch(ifStmt, testFunc->getBody(), true);
}

TEST_F(IfToSwitchOptimizerTests,
MoreComplicatedSubstituteIfToSwitchOptimize) {
	// if (b + 3 == 5) {
	// } else if (b + 3 == 6) {
	//     b = b + 3;
	// } else if (b + 3 == 8) {
	//     b = b + 3;
	// } else if (b + 3 == 12) {
	//     b = b + 3;
	// }
	//
	// Optimized to:
	// switch(b + 3) {
	//     case 5: b = b + 3; break;
	//     case 6: b = b + 3; break;
	//     case 8: b = b + 3; break;
	//     case 12: b = b + 3; break;
	// }
	//
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf1(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf2(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(8, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf3(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(12, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf1, assignStmt);
	ifStmt->addClause(eqOpExprElseIf2, assignStmt);
	ifStmt->addClause(eqOpExprElseIf3, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	checkCorrectConvertIfToSwitch(ifStmt, testFunc->getBody(), true);
}

TEST_F(IfToSwitchOptimizerTests,
SimpleSubstituteIfToSwitchWithElseClauseOptimize) {
	// if (b + 3 == 5) {
	//     b = b + 3;
	// } else if (b + 3 == 6) {
	//     b = b + 3;
	// } else {
	//     b = b + 3;
	// }
	//
	// Optimized to:
	// switch(b + 3) {
	//     case 5: b = b + 3; break;
	//     case 6: b = b + 3; break;
	//     default: b = b + 3; break;
	// }
	//
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	ifStmt->setElseClause(assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	checkCorrectConvertIfToSwitch(ifStmt, testFunc->getBody(), true);
}

TEST_F(IfToSwitchOptimizerTests,
SimpleSubstituteIfToSwitchControlExprIsOnNotSameSidesOptimize) {
	// if (b + 3 == 5) {
	//     b = b + 3;
	// } else if (6 == b + 3) {
	//     b = b + 3;
	// }
	//
	// Optimized to:
	// switch(b + 3) {
	//     case 5: b = b + 3; break;
	//     case 6: b = b + 3; break;
	// }
	//
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			ConstInt::create(6, 64),
			addOpExpr
	));
	ShPtr<AssignStmt> assignStmt(
		AssignStmt::create(
			varB,
			addOpExpr
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, assignStmt));
	ifStmt->addClause(eqOpExprElseIf, assignStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	checkCorrectConvertIfToSwitch(ifStmt, testFunc->getBody(), true);
}

TEST_F(IfToSwitchOptimizerTests,
SimpleSubstituteIfToSwitchLastStmtIsReturnStmtOptimize) {
	// if (b + 3 == 5) {
	//     return 2;
	// } else if (b + 3 == 6) {
	//     return 2;
	// }
	//
	// Optimized to:
	// switch(b + 3) {
	//     case 5: return 2;
	//     case 6: return 2;
	// }
	//
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<ReturnStmt> returnStmt(ReturnStmt::create(ConstInt::create(2, 64)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, returnStmt));
	ifStmt->addClause(eqOpExprElseIf, returnStmt);
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	checkCorrectConvertIfToSwitch(ifStmt, testFunc->getBody(), false);
}

TEST_F(IfToSwitchOptimizerTests,
SimpleSubstituteIfToSwitchLastStmtIsContinueStmtOptimize) {
	// if (b + 3 == 5) {
	//     continue;
	// } else if (b + 3 == 6) {
	//     continue;
	// }
	//
	// Optimized to:
	// switch(b + 3) {
	//     case 5: continue;
	//     case 6: continue;
	// }
	//
	ShPtr<Variable> varB(Variable::create("b", IntType::create(16)));
	ShPtr<AddOpExpr> addOpExpr(
		AddOpExpr::create(
			varB,
			ConstInt::create(3, 64)
	));
	ShPtr<EqOpExpr> eqOpExprIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(5, 64)
	));
	ShPtr<EqOpExpr> eqOpExprElseIf(
		EqOpExpr::create(
			addOpExpr,
			ConstInt::create(6, 64)
	));
	ShPtr<IfStmt> ifStmt(IfStmt::create(eqOpExprIf, ContinueStmt::create()));
	ifStmt->addClause(eqOpExprElseIf, ContinueStmt::create());
	testFunc->setBody(ifStmt);

	// Optimize the module.
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	checkCorrectConvertIfToSwitch(ifStmt, testFunc->getBody(), false);
}

TEST_F(IfToSwitchOptimizerTests,
SequentialTwoSingleClauseIfsSameVarConvertToSwitch) {
	// if (v == 0) { b = 1; }
	// if (v == 2) { b = 2; }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k0(ConstInt::create(0, 64));
	ShPtr<ConstInt> k2(ConstInt::create(2, 64));
	ShPtr<AssignStmt> body0(
		AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> body1(
		AssignStmt::create(varB, ConstInt::create(2, 32)));
	ShPtr<IfStmt> if0(IfStmt::create(EqOpExpr::create(varV, k0), body0));
	ShPtr<IfStmt> if1(IfStmt::create(EqOpExpr::create(varV, k2), body1));
	if0->setSuccessor(if1);
	testFunc->setBody(if0);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ASSERT_EQ(k0, it->first);
	ASSERT_EQ(body0, it->second);
	++it;
	ASSERT_EQ(k2, it->first);
	ASSERT_EQ(body1, it->second);
}

TEST_F(IfToSwitchOptimizerTests,
ElseIfLtUpperBoundChainConvertsToSwitch) {
	// if (v < 1) b = 10; else if (v < 2) b = 20; else b = 30;
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> b10(
		AssignStmt::create(varB, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> b20(
		AssignStmt::create(varB, ConstInt::create(20, 32)));
	ShPtr<AssignStmt> b30(
		AssignStmt::create(varB, ConstInt::create(30, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(1, 64)), b10));
	ifStmt->addClause(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), b20);
	ifStmt->setElseClause(b30);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ShPtr<ConstInt> c0(cast<ConstInt>(it->first));
	ASSERT_TRUE(c0);
	EXPECT_TRUE(c0->isEqualTo(ConstInt::create(0, 32)));
	ASSERT_EQ(b10, it->second);
	++it;
	ShPtr<ConstInt> c1(cast<ConstInt>(it->first));
	ASSERT_TRUE(c1);
	EXPECT_TRUE(c1->isEqualTo(ConstInt::create(1, 32)));
	ASSERT_EQ(b20, it->second);
	ASSERT_TRUE(sw->hasDefaultClause());
	ASSERT_EQ(b30, sw->getDefaultClauseBody());
}

TEST_F(IfToSwitchOptimizerTests,
ElseIfLeUpperBoundChainConvertsToSwitch) {
	// if (v <= 0) b = 1; else if (v <= 1) b = 2; else b = 3;
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> b1(
		AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> b2(
		AssignStmt::create(varB, ConstInt::create(2, 32)));
	ShPtr<AssignStmt> b3(
		AssignStmt::create(varB, ConstInt::create(3, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(0, 64),
			LtEqOpExpr::Variant::SCmp),
		b1));
	ifStmt->addClause(
		LtEqOpExpr::create(varV, ConstInt::create(1, 64),
			LtEqOpExpr::Variant::SCmp),
		b2);
	ifStmt->setElseClause(b3);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ShPtr<ConstInt> c0(cast<ConstInt>(it->first));
	ASSERT_TRUE(c0);
	EXPECT_TRUE(c0->isEqualTo(ConstInt::create(0, 32)));
	ASSERT_EQ(b1, it->second);
	++it;
	ShPtr<ConstInt> c1(cast<ConstInt>(it->first));
	ASSERT_TRUE(c1);
	EXPECT_TRUE(c1->isEqualTo(ConstInt::create(1, 32)));
	ASSERT_EQ(b2, it->second);
	ASSERT_TRUE(sw->hasDefaultClause());
	ASSERT_EQ(b3, sw->getDefaultClauseBody());
}

TEST_F(IfToSwitchOptimizerTests,
ElseIfGeLowerBoundChainConvertsToSwitch) {
	// if (v >= 2) b = 2; else if (v >= 1) b = 1; else b = 0;
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> b2(
		AssignStmt::create(varB, ConstInt::create(2, 32)));
	ShPtr<AssignStmt> b1(
		AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> b0(
		AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		b2));
	ifStmt->addClause(
		GtEqOpExpr::create(varV, ConstInt::create(1, 64),
			GtEqOpExpr::Variant::SCmp),
		b1);
	ifStmt->setElseClause(b0);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ShPtr<ConstInt> c2(cast<ConstInt>(it->first));
	ASSERT_TRUE(c2);
	EXPECT_TRUE(c2->isEqualTo(ConstInt::create(2, 32)));
	ASSERT_EQ(b2, it->second);
	++it;
	ShPtr<ConstInt> c1(cast<ConstInt>(it->first));
	ASSERT_TRUE(c1);
	EXPECT_TRUE(c1->isEqualTo(ConstInt::create(1, 32)));
	ASSERT_EQ(b1, it->second);
	ASSERT_TRUE(sw->hasDefaultClause());
	ASSERT_EQ(b0, sw->getDefaultClauseBody());
}

TEST_F(IfToSwitchOptimizerTests,
SingleIfGeLowerBoundWithElseConvertsToSwitch) {
	// if (v >= 1) b = 1; else b = 0;
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> b1(
		AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> b0(
		AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(1, 64),
			GtEqOpExpr::Variant::SCmp),
		b1));
	ifStmt->setElseClause(b0);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	auto it = sw->clause_begin();
	ShPtr<ConstInt> c1(cast<ConstInt>(it->first));
	ASSERT_TRUE(c1);
	EXPECT_TRUE(c1->isEqualTo(ConstInt::create(1, 32)));
	ASSERT_EQ(b1, it->second);
	ASSERT_EQ(b0, sw->getDefaultClauseBody());
}

TEST_F(IfToSwitchOptimizerTests,
ElseIfGtLowerBoundChainConvertsToSwitch) {
	// if (v > 1) b = 2; else if (v > 0) b = 1; else b = 0;
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> b2(
		AssignStmt::create(varB, ConstInt::create(2, 32)));
	ShPtr<AssignStmt> b1(
		AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> b0(
		AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64),
			GtOpExpr::Variant::SCmp),
		b2));
	ifStmt->addClause(
		GtOpExpr::create(varV, ConstInt::create(0, 64),
			GtOpExpr::Variant::SCmp),
		b1);
	ifStmt->setElseClause(b0);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ShPtr<ConstInt> c2(cast<ConstInt>(it->first));
	ASSERT_TRUE(c2);
	EXPECT_TRUE(c2->isEqualTo(ConstInt::create(2, 32)));
	ASSERT_EQ(b2, it->second);
	++it;
	ShPtr<ConstInt> c1(cast<ConstInt>(it->first));
	ASSERT_TRUE(c1);
	EXPECT_TRUE(c1->isEqualTo(ConstInt::create(1, 32)));
	ASSERT_EQ(b1, it->second);
	ASSERT_TRUE(sw->hasDefaultClause());
	ASSERT_EQ(b0, sw->getDefaultClauseBody());
}

TEST_F(IfToSwitchOptimizerTests,
SingleIfGtLowerBoundWithElseConvertsToSwitch) {
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> b1(
		AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> b0(
		AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(0, 64),
			GtOpExpr::Variant::SCmp),
		b1));
	ifStmt->setElseClause(b0);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	auto it = sw->clause_begin();
	ShPtr<ConstInt> c1(cast<ConstInt>(it->first));
	ASSERT_TRUE(c1);
	EXPECT_TRUE(c1->isEqualTo(ConstInt::create(1, 32)));
	ASSERT_EQ(b1, it->second);
	ASSERT_EQ(b0, sw->getDefaultClauseBody());
}

TEST_F(IfToSwitchOptimizerTests,
ElseIfGeLowerBoundChainWrongBoundNotOptimized) {
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> body(AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		body));
	ifStmt->addClause(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		body);
	ifStmt->setElseClause(body);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<IfStmt> out(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(out);
}

TEST_F(IfToSwitchOptimizerTests,
ElseIfLeUpperBoundChainWrongBoundNotOptimized) {
	// if (v <= 0) ... else if (v <= 2) ...  (skips v == 1)
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> body(AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(0, 64),
			LtEqOpExpr::Variant::SCmp),
		body));
	ifStmt->addClause(
		LtEqOpExpr::create(varV, ConstInt::create(2, 64),
			LtEqOpExpr::Variant::SCmp),
		body);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<IfStmt> out(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(out);
}

TEST_F(IfToSwitchOptimizerTests,
ElseIfLtUpperBoundChainWrongBoundNotOptimized) {
	// if (v < 1) ... else if (v < 3) ...  (skips v == 1 partition)
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> body(AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(1, 64)), body));
	ifStmt->addClause(
		LtOpExpr::create(varV, ConstInt::create(3, 64)), body);
	testFunc->setBody(ifStmt);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<IfStmt> out(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(out);
}

TEST_F(IfToSwitchOptimizerTests,
OuterLtWithInnerDenseEqChainHoistsToSwitch) {
	// if (v < 2) { if (v == 0) ... else if (v == 1) ... }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k0(ConstInt::create(0, 64));
	ShPtr<ConstInt> k1(ConstInt::create(1, 64));
	ShPtr<AssignStmt> b0(AssignStmt::create(varB, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> b1(AssignStmt::create(varB, ConstInt::create(11, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(EqOpExpr::create(varV, k0), b0));
	inner->addClause(EqOpExpr::create(varV, k1), b1);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ASSERT_EQ(k0, it->first);
	ASSERT_EQ(b0, it->second);
	++it;
	ASSERT_EQ(k1, it->first);
	ASSERT_EQ(b1, it->second);
}

TEST_F(IfToSwitchOptimizerTests,
OuterLtInnerClauseCountMismatchNotOptimized) {
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> body(AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(
		EqOpExpr::create(varV, ConstInt::create(0, 64)), body));
	inner->addClause(
		EqOpExpr::create(varV, ConstInt::create(1, 64)), body);
	// Bound says n=3 but inner only has 2 == branches
	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(3, 64)), inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<IfStmt> out(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(out);
}

TEST_F(IfToSwitchOptimizerTests,
OuterLeWithInnerDenseEqChainHoistsToSwitch) {
	// if (v <= 1) { if (v == 0) ... else if (v == 1) ... }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k0(ConstInt::create(0, 64));
	ShPtr<ConstInt> k1(ConstInt::create(1, 64));
	ShPtr<AssignStmt> b0(AssignStmt::create(varB, ConstInt::create(20, 32)));
	ShPtr<AssignStmt> b1(AssignStmt::create(varB, ConstInt::create(21, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(EqOpExpr::create(varV, k0), b0));
	inner->addClause(EqOpExpr::create(varV, k1), b1);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(1, 64),
			LtEqOpExpr::Variant::SCmp),
		inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ASSERT_EQ(k0, it->first);
	ASSERT_EQ(b0, it->second);
	++it;
	ASSERT_EQ(k1, it->first);
	ASSERT_EQ(b1, it->second);
}

TEST_F(IfToSwitchOptimizerTests,
OuterLeInnerClauseCountMismatchNotOptimized) {
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> body(AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(
		EqOpExpr::create(varV, ConstInt::create(0, 64)), body));
	inner->addClause(
		EqOpExpr::create(varV, ConstInt::create(1, 64)), body);
	// v <= 2  ⇒  expect 3 dense cases; inner has only 2
	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(2, 64),
			LtEqOpExpr::Variant::SCmp),
		inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<IfStmt> out(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(out);
}

TEST_F(IfToSwitchOptimizerTests,
OuterLtWithInnerConsecutiveNonZeroHoistsToSwitch) {
	// if (v < 7) { if (v==5)... else if (v==6)... }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k5(ConstInt::create(5, 64));
	ShPtr<ConstInt> k6(ConstInt::create(6, 64));
	ShPtr<AssignStmt> b5(AssignStmt::create(varB, ConstInt::create(50, 32)));
	ShPtr<AssignStmt> b6(AssignStmt::create(varB, ConstInt::create(60, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(EqOpExpr::create(varV, k5), b5));
	inner->addClause(EqOpExpr::create(varV, k6), b6);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(7, 64)), inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ASSERT_EQ(k5, it->first);
	ASSERT_EQ(b5, it->second);
	++it;
	ASSERT_EQ(k6, it->first);
	ASSERT_EQ(b6, it->second);
}

TEST_F(IfToSwitchOptimizerTests,
OuterLtInnerConsecutiveWrongUpperBoundNotOptimized) {
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> body(AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(
		EqOpExpr::create(varV, ConstInt::create(5, 64)), body));
	inner->addClause(
		EqOpExpr::create(varV, ConstInt::create(6, 64)), body);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(8, 64)), inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<IfStmt> out(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(out);
}

TEST_F(IfToSwitchOptimizerTests,
OuterGtWithInnerConsecutiveHoistsToSwitch) {
	// if (v > 0) { if (v==1)... else if (v==2)... }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k1(ConstInt::create(1, 64));
	ShPtr<ConstInt> k2(ConstInt::create(2, 64));
	ShPtr<AssignStmt> b1(AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> b2(AssignStmt::create(varB, ConstInt::create(2, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(EqOpExpr::create(varV, k1), b1));
	inner->addClause(EqOpExpr::create(varV, k2), b2);
	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(0, 64),
			GtOpExpr::Variant::SCmp),
		inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
}

TEST_F(IfToSwitchOptimizerTests,
OuterGeWithInnerConsecutiveHoistsToSwitch) {
	// if (v >= 10) { if (v==10)... else if (v==11)... }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k10(ConstInt::create(10, 64));
	ShPtr<ConstInt> k11(ConstInt::create(11, 64));
	ShPtr<AssignStmt> b10(AssignStmt::create(varB, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> b11(AssignStmt::create(varB, ConstInt::create(11, 32)));
	ShPtr<IfStmt> inner(IfStmt::create(EqOpExpr::create(varV, k10), b10));
	inner->addClause(EqOpExpr::create(varV, k11), b11);
	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(10, 64),
			GtEqOpExpr::Variant::SCmp),
		inner));
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
}

TEST_F(IfToSwitchOptimizerTests,
LtFourLevelNestedSplitInThenMergesToSwitch) {
	// Nested v<8,6,4,2 → bands 0-1, 2-3, 4-5, 6-7, 8-9
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 9; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(900 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), chainA));
	layer3->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(4, 64)), layer3));
	layer2->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(6, 64)), layer2));
	layer1->setElseClause(chainD);

	ShPtr<IfStmt> tailE(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	tailE->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(8, 64)), layer1));
	outer->setElseClause(tailE);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 9u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(10u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LtFiveLevelNestedSplitInThenMergesToSwitch) {
	// Nested v<10,8,6,4,2 → six bands of two cases each (0-11)
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 11; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(920 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), chainA));
	layer4->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(4, 64)), layer4));
	layer3->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(6, 64)), layer3));
	layer2->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainE->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(8, 64)), layer2));
	layer1->setElseClause(chainE);

	ShPtr<IfStmt> tailF(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	tailF->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(10, 64)), layer1));
	outer->setElseClause(tailF);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 11u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(12u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LtSixLevelNestedSplitInThenMergesToSwitch) {
	// Nested v<12,10,8,6,4,2 → seven bands of two cases (0-13)
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(921 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer5(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), chainA));
	layer5->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(4, 64)), layer5));
	layer4->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(6, 64)), layer4));
	layer3->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainE->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(8, 64)), layer3));
	layer2->setElseClause(chainE);

	ShPtr<IfStmt> chainF(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	chainF->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(10, 64)), layer2));
	layer1->setElseClause(chainF);

	ShPtr<IfStmt> tailG(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	tailG->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(12, 64)), layer1));
	outer->setElseClause(tailG);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LtThreeLevelNestedInnerLeSplitInThenMergesToSwitch) {
	// v<14, v<10, v<=3 → 0..3, 4..9, 10..13, 14..15
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 15; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(930 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 3; ++k) {
		chainA->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	for (int k = 5; k <= 9; ++k) {
		chainB->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midInner(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(3, 64),
			LtEqOpExpr::Variant::SCmp),
		chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	for (int k = 11; k <= 13; ++k) {
		chainC->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midOuter(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(10, 64)), midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[14]), bodies[14]));
	tailD->addClause(EqOpExpr::create(varV, keys[15]), bodies[15]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(14, 64)), midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 15u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(16u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LeThreeLevelNestedInnerLtSplitInThenMergesToSwitch) {
	// v<=14, v<=10, v<4 → 0..3, 4..10 (inner else under v<=10), 11..14 (v>10), 15..16
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 16; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(931 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 3; ++k) {
		chainA->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	for (int k = 5; k <= 10; ++k) {
		chainB->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midInner(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(4, 64)), chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[11]), bodies[11]));
	for (int k = 12; k <= 14; ++k) {
		chainC->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midOuter(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(10, 64),
			LtEqOpExpr::Variant::SCmp),
		midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[15]), bodies[15]));
	tailD->addClause(EqOpExpr::create(varV, keys[16]), bodies[16]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(14, 64),
			LtEqOpExpr::Variant::SCmp),
		midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 16u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(17u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LeFourLevelNestedSplitInThenMergesToSwitch) {
	// v<=7,5,3,1 → 0-1 … 8-9
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 9; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(910 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(1, 64),
			LtEqOpExpr::Variant::SCmp),
		chainA));
	layer3->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(3, 64),
			LtEqOpExpr::Variant::SCmp),
		layer3));
	layer2->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(5, 64),
			LtEqOpExpr::Variant::SCmp),
		layer2));
	layer1->setElseClause(chainD);

	ShPtr<IfStmt> tailE(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	tailE->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(7, 64),
			LtEqOpExpr::Variant::SCmp),
		layer1));
	outer->setElseClause(tailE);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 9u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(10u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LeFiveLevelNestedSplitInThenMergesToSwitch) {
	// v<=9,7,5,3,1 → six pairs 0-1 … 10-11
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 11; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(940 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(1, 64),
			LtEqOpExpr::Variant::SCmp),
		chainA));
	layer4->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(3, 64),
			LtEqOpExpr::Variant::SCmp),
		layer4));
	layer3->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(5, 64),
			LtEqOpExpr::Variant::SCmp),
		layer3));
	layer2->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainE->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(7, 64),
			LtEqOpExpr::Variant::SCmp),
		layer2));
	layer1->setElseClause(chainE);

	ShPtr<IfStmt> tailF(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	tailF->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(9, 64),
			LtEqOpExpr::Variant::SCmp),
		layer1));
	outer->setElseClause(tailF);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 11u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(12u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LeSixLevelNestedSplitInThenMergesToSwitch) {
	// v<=11,9,7,5,3,1 → seven pairs 0-1 … 12-13
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(941 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer5(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(1, 64),
			LtEqOpExpr::Variant::SCmp),
		chainA));
	layer5->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(3, 64),
			LtEqOpExpr::Variant::SCmp),
		layer5));
	layer4->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(5, 64),
			LtEqOpExpr::Variant::SCmp),
		layer4));
	layer3->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainE->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(7, 64),
			LtEqOpExpr::Variant::SCmp),
		layer3));
	layer2->setElseClause(chainE);

	ShPtr<IfStmt> chainF(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	chainF->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(9, 64),
			LtEqOpExpr::Variant::SCmp),
		layer2));
	layer1->setElseClause(chainF);

	ShPtr<IfStmt> tailG(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	tailG->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(11, 64),
			LtEqOpExpr::Variant::SCmp),
		layer1));
	outer->setElseClause(tailG);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GtFiveLevelNestedSplitInThenMergesToSwitch) {
	// v>1,3,5,7,9 → tail 0-1 … A 10-11
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 11; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(950 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	chainA->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainB->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(9, 64)), chainA));
	layer4->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainC->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(7, 64)), layer4));
	layer3->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainD->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(5, 64)), layer3));
	layer2->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainE->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(3, 64)), layer2));
	layer1->setElseClause(chainE);

	ShPtr<IfStmt> tailF(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailF->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64)), layer1));
	outer->setElseClause(tailF);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 11u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(12u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GtSixLevelNestedSplitInThenMergesToSwitch) {
	// v>1,3,5,7,9,11 → tail 0-1 … A 12-13
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(951 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	chainA->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	chainB->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);
	ShPtr<IfStmt> layer5(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(11, 64)), chainA));
	layer5->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainC->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(9, 64)), layer5));
	layer4->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(7, 64)), layer4));
	layer3->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainE->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(5, 64)), layer3));
	layer2->setElseClause(chainE);

	ShPtr<IfStmt> chainF(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainF->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(3, 64)), layer2));
	layer1->setElseClause(chainF);

	ShPtr<IfStmt> tailG(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailG->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64)), layer1));
	outer->setElseClause(tailG);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GeFiveLevelNestedSplitInThenMergesToSwitch) {
	// v>=2,4,6,8,10 → tail 0-1 … A 10-11
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 11; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(960 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	chainA->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainB->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(10, 64),
			GtEqOpExpr::Variant::SCmp),
		chainA));
	layer4->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainC->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(8, 64),
			GtEqOpExpr::Variant::SCmp),
		layer4));
	layer3->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainD->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		layer3));
	layer2->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainE->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(4, 64),
			GtEqOpExpr::Variant::SCmp),
		layer2));
	layer1->setElseClause(chainE);

	ShPtr<IfStmt> tailF(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailF->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		layer1));
	outer->setElseClause(tailF);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 11u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(12u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GeSixLevelNestedSplitInThenMergesToSwitch) {
	// v>=2,4,6,8,10,12 → tail 0-1 … A 12-13
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(961 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	chainA->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	chainB->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);
	ShPtr<IfStmt> layer5(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(12, 64),
			GtEqOpExpr::Variant::SCmp),
		chainA));
	layer5->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainC->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> layer4(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(10, 64),
			GtEqOpExpr::Variant::SCmp),
		layer5));
	layer4->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(8, 64),
			GtEqOpExpr::Variant::SCmp),
		layer4));
	layer3->setElseClause(chainD);

	ShPtr<IfStmt> chainE(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainE->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		layer3));
	layer2->setElseClause(chainE);

	ShPtr<IfStmt> chainF(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainF->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(4, 64),
			GtEqOpExpr::Variant::SCmp),
		layer2));
	layer1->setElseClause(chainF);

	ShPtr<IfStmt> tailG(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailG->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		layer1));
	outer->setElseClause(tailG);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GtFourLevelNestedSplitInThenMergesToSwitch) {
	// v>1,3,5,7 → E 0-1 … A 8-9
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 9; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(920 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainA->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainB->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(7, 64)), chainA));
	layer3->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(5, 64)), layer3));
	layer2->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainD->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(3, 64)), layer2));
	layer1->setElseClause(chainD);

	ShPtr<IfStmt> tailE(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailE->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64)), layer1));
	outer->setElseClause(tailE);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 9u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(10u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GeFourLevelNestedSplitInThenMergesToSwitch) {
	// v>=2,4,6,8 → E 0-1 … A 8-9
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 9; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(930 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainA->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainB->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> layer3(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(8, 64),
			GtEqOpExpr::Variant::SCmp),
		chainA));
	layer3->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> layer2(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		layer3));
	layer2->setElseClause(chainC);

	ShPtr<IfStmt> chainD(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainD->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> layer1(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(4, 64),
			GtEqOpExpr::Variant::SCmp),
		layer2));
	layer1->setElseClause(chainD);

	ShPtr<IfStmt> tailE(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailE->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		layer1));
	outer->setElseClause(tailE);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 9u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(10u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LtThreeLevelNestedSplitInThenMergesToSwitch) {
	// if (v < 6) { if (v < 4) { if (v < 2) { 0,1 } else { 2,3 } } else { 4,5 } } else { 6,7 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 7; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(500 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> midInner(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> midOuter(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(4, 64)), midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	tailD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(6, 64)), midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 7u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(8u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LeThreeLevelNestedSplitInThenMergesToSwitch) {
	// if (v<=5){ if (v<=3){ if (v<=1){0,1} else {2,3} } else {4,5} } else {6,7}
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 7; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(600 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	chainA->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainB->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> midInner(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(1, 64),
			LtEqOpExpr::Variant::SCmp),
		chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainC->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> midOuter(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(3, 64),
			LtEqOpExpr::Variant::SCmp),
		midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	tailD->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(5, 64),
			LtEqOpExpr::Variant::SCmp),
		midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 7u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(8u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GtThreeLevelNestedSplitInThenMergesToSwitch) {
	// if (v>1){ if (v>3){ if (v>5){6,7} else {4,5} } else {2,3} } else {0,1}
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 7; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(700 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainA->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainB->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> midInner(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(5, 64)), chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainC->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> midOuter(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(3, 64)), midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailD->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64)), midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 7u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(8u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GtThreeLevelNestedInnerGeSplitInThenMergesToSwitch) {
	// v>1, v>4, v>=8 → else 0-1, 2-4, 5-7, then 8-10
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 10; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(710 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[8]), bodies[8]));
	chainA->addClause(EqOpExpr::create(varV, keys[9]), bodies[9]);
	chainA->addClause(EqOpExpr::create(varV, keys[10]), bodies[10]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[5]), bodies[5]));
	chainB->addClause(EqOpExpr::create(varV, keys[6]), bodies[6]);
	chainB->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> midInner(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(8, 64),
			GtEqOpExpr::Variant::SCmp),
		chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainC->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	chainC->addClause(EqOpExpr::create(varV, keys[4]), bodies[4]);
	ShPtr<IfStmt> midOuter(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(4, 64)), midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailD->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64)), midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 10u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(11u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GeThreeLevelNestedSplitInThenMergesToSwitch) {
	// if (v>=2){ if (v>=4){ if (v>=6){6,7} else {4,5} } else {2,3} } else {0,1}
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 7; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(800 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	chainA->addClause(EqOpExpr::create(varV, keys[7]), bodies[7]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[4]), bodies[4]));
	chainB->addClause(EqOpExpr::create(varV, keys[5]), bodies[5]);
	ShPtr<IfStmt> midInner(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	chainC->addClause(EqOpExpr::create(varV, keys[3]), bodies[3]);
	ShPtr<IfStmt> midOuter(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(4, 64),
			GtEqOpExpr::Variant::SCmp),
		midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailD->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 7u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(8u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GeThreeLevelNestedInnerGtSplitInThenMergesToSwitch) {
	// v>=2, v>=6, v>9 → else 0-1, 2-5, 6-9, then 10-12
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 12; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(820 + k, 32)));
	}
	ShPtr<IfStmt> chainA(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	chainA->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);
	chainA->addClause(EqOpExpr::create(varV, keys[12]), bodies[12]);
	ShPtr<IfStmt> chainB(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	for (int k = 7; k <= 9; ++k) {
		chainB->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midInner(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(9, 64)), chainA));
	midInner->setElseClause(chainB);

	ShPtr<IfStmt> chainC(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	for (int k = 3; k <= 5; ++k) {
		chainC->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midOuter(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		midInner));
	midOuter->setElseClause(chainC);

	ShPtr<IfStmt> tailD(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailD->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(2, 64),
			GtEqOpExpr::Variant::SCmp),
		midOuter));
	outer->setElseClause(tailD);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 12u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(13u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LtNestedSplitInThenMergesToSwitch) {
	// if (v < 10) { if (v < 5) { 0..4 } else { 5..9 } } else { 10,11 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 11; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(100 + k, 32)));
	}
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 4; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[5]), bodies[5]));
	for (int k = 6; k <= 9; ++k) {
		highInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(5, 64)), lowInner));
	midIf->setElseClause(highInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	tailRight->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(10, 64)), midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 11u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(12u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LeNestedSplitInThenMergesToSwitch) {
	// if (v <= 11) { if (v <= 5) { 0..5 } else { 6..11 } } else { 12,13 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(200 + k, 32)));
	}
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 5; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	for (int k = 7; k <= 11; ++k) {
		highInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(5, 64),
			LtEqOpExpr::Variant::SCmp),
		lowInner));
	midIf->setElseClause(highInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	tailRight->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(11, 64),
			LtEqOpExpr::Variant::SCmp),
		midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LtWithNestedLeSplitInThenMergesToSwitch) {
	// if (v < 10) { if (v <= 4) { 0..4 } else { 5..9 } } else { 10,11 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 11; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(500 + k, 32)));
	}
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 4; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[5]), bodies[5]));
	for (int k = 6; k <= 9; ++k) {
		highInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(4, 64),
			LtEqOpExpr::Variant::SCmp),
		lowInner));
	midIf->setElseClause(highInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[10]), bodies[10]));
	tailRight->addClause(EqOpExpr::create(varV, keys[11]), bodies[11]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(10, 64)), midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 11u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(12u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LeWithNestedLtSplitInThenMergesToSwitch) {
	// if (v <= 11) { if (v < 6) { 0..5 } else { 6..11 } } else { 12,13 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(600 + k, 32)));
	}
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 5; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	for (int k = 7; k <= 11; ++k) {
		highInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(6, 64)), lowInner));
	midIf->setElseClause(highInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	tailRight->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);

	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(11, 64),
			LtEqOpExpr::Variant::SCmp),
		midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GtNestedSplitInThenMergesToSwitch) {
	// if (v > 1) { if (v > 5) { 6..9 } else { 2..5 } } else { 0,1 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 9; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(300 + k, 32)));
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	for (int k = 7; k <= 9; ++k) {
		highInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	for (int k = 3; k <= 5; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(5, 64)), highInner));
	midIf->setElseClause(lowInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailRight->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64)), midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 9u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(10u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GeNestedSplitInThenMergesToSwitch) {
	// if (v >= 6) { if (v >= 12) { 12,13 } else { 6..11 } } else { 0..5 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(400 + k, 32)));
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	highInner->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	for (int k = 7; k <= 11; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(12, 64),
			GtEqOpExpr::Variant::SCmp),
		highInner));
	midIf->setElseClause(lowInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 5; ++k) {
		tailRight->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}

	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GtWithNestedGeSplitInThenMergesToSwitch) {
	// if (v > 1) { if (v >= 6) { 6..9 } else { 2..5 } } else { 0,1 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 9; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(700 + k, 32)));
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	for (int k = 7; k <= 9; ++k) {
		highInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[2]), bodies[2]));
	for (int k = 3; k <= 5; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		highInner));
	midIf->setElseClause(lowInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	tailRight->addClause(EqOpExpr::create(varV, keys[1]), bodies[1]);

	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(1, 64)), midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 9u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(10u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
GeWithNestedGtSplitInThenMergesToSwitch) {
	// if (v >= 6) { if (v > 11) { 12,13 } else { 6..11 } } else { 0..5 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	std::vector<ShPtr<ConstInt>> keys;
	std::vector<ShPtr<AssignStmt>> bodies;
	for (int k = 0; k <= 13; ++k) {
		keys.push_back(ConstInt::create(k, 64));
		bodies.push_back(
			AssignStmt::create(varB, ConstInt::create(800 + k, 32)));
	}
	ShPtr<IfStmt> highInner(IfStmt::create(
		EqOpExpr::create(varV, keys[12]), bodies[12]));
	highInner->addClause(EqOpExpr::create(varV, keys[13]), bodies[13]);
	ShPtr<IfStmt> lowInner(IfStmt::create(
		EqOpExpr::create(varV, keys[6]), bodies[6]));
	for (int k = 7; k <= 11; ++k) {
		lowInner->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}
	ShPtr<IfStmt> midIf(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(11, 64)), highInner));
	midIf->setElseClause(lowInner);

	ShPtr<IfStmt> tailRight(IfStmt::create(
		EqOpExpr::create(varV, keys[0]), bodies[0]));
	for (int k = 1; k <= 5; ++k) {
		tailRight->addClause(EqOpExpr::create(varV, keys[k]), bodies[k]);
	}

	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(6, 64),
			GtEqOpExpr::Variant::SCmp),
		midIf));
	outer->setElseClause(tailRight);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	unsigned ki = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ki) {
		ASSERT_LE(ki, 13u);
		ASSERT_EQ(keys[ki], it->first);
	}
	ASSERT_EQ(14u, ki);
}

TEST_F(IfToSwitchOptimizerTests,
LtSplitTwoConsecutiveInnerChainsMergesToSwitch) {
	// if (v < 2) { if (v==0)... else if (v==1)... } else { if (v==2)... else if (v==3)... }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k0(ConstInt::create(0, 64));
	ShPtr<ConstInt> k1(ConstInt::create(1, 64));
	ShPtr<ConstInt> k2(ConstInt::create(2, 64));
	ShPtr<ConstInt> k3(ConstInt::create(3, 64));
	ShPtr<AssignStmt> b0(AssignStmt::create(varB, ConstInt::create(100, 32)));
	ShPtr<AssignStmt> b1(AssignStmt::create(varB, ConstInt::create(101, 32)));
	ShPtr<AssignStmt> b2(AssignStmt::create(varB, ConstInt::create(102, 32)));
	ShPtr<AssignStmt> b3(AssignStmt::create(varB, ConstInt::create(103, 32)));
	ShPtr<IfStmt> leftInner(IfStmt::create(EqOpExpr::create(varV, k0), b0));
	leftInner->addClause(EqOpExpr::create(varV, k1), b1);
	ShPtr<IfStmt> rightInner(IfStmt::create(EqOpExpr::create(varV, k2), b2));
	rightInner->addClause(EqOpExpr::create(varV, k3), b3);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), leftInner));
	outer->setElseClause(rightInner);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	auto it = sw->clause_begin();
	ASSERT_EQ(k0, it->first);
	ASSERT_EQ(b0, it->second);
	++it;
	ASSERT_EQ(k1, it->first);
	ASSERT_EQ(b1, it->second);
	++it;
	ASSERT_EQ(k2, it->first);
	ASSERT_EQ(b2, it->second);
	++it;
	ASSERT_EQ(k3, it->first);
	ASSERT_EQ(b3, it->second);
}

TEST_F(IfToSwitchOptimizerTests,
LtSplitNonZeroBaseMergesToSwitch) {
	// if (v < 10) { 7,8,9 } else { 10,11 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k7(ConstInt::create(7, 64));
	ShPtr<ConstInt> k8(ConstInt::create(8, 64));
	ShPtr<ConstInt> k9(ConstInt::create(9, 64));
	ShPtr<ConstInt> k10(ConstInt::create(10, 64));
	ShPtr<ConstInt> k11(ConstInt::create(11, 64));
	ShPtr<AssignStmt> b7(AssignStmt::create(varB, ConstInt::create(7, 32)));
	ShPtr<AssignStmt> b8(AssignStmt::create(varB, ConstInt::create(8, 32)));
	ShPtr<AssignStmt> b9(AssignStmt::create(varB, ConstInt::create(9, 32)));
	ShPtr<AssignStmt> b10(AssignStmt::create(varB, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> b11(AssignStmt::create(varB, ConstInt::create(11, 32)));
	ShPtr<IfStmt> leftInner(IfStmt::create(EqOpExpr::create(varV, k7), b7));
	leftInner->addClause(EqOpExpr::create(varV, k8), b8);
	leftInner->addClause(EqOpExpr::create(varV, k9), b9);
	ShPtr<IfStmt> rightInner(IfStmt::create(EqOpExpr::create(varV, k10), b10));
	rightInner->addClause(EqOpExpr::create(varV, k11), b11);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(10, 64)), leftInner));
	outer->setElseClause(rightInner);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
}

TEST_F(IfToSwitchOptimizerTests,
LeSplitTwoConsecutiveInnerChainsMergesToSwitch) {
	// if (v <= 9) { 7,8,9 } else { 10,11 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k7(ConstInt::create(7, 64));
	ShPtr<ConstInt> k8(ConstInt::create(8, 64));
	ShPtr<ConstInt> k9(ConstInt::create(9, 64));
	ShPtr<ConstInt> k10(ConstInt::create(10, 64));
	ShPtr<ConstInt> k11(ConstInt::create(11, 64));
	ShPtr<AssignStmt> b7(AssignStmt::create(varB, ConstInt::create(7, 32)));
	ShPtr<AssignStmt> b8(AssignStmt::create(varB, ConstInt::create(8, 32)));
	ShPtr<AssignStmt> b9(AssignStmt::create(varB, ConstInt::create(9, 32)));
	ShPtr<AssignStmt> b10(AssignStmt::create(varB, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> b11(AssignStmt::create(varB, ConstInt::create(11, 32)));
	ShPtr<IfStmt> leftInner(IfStmt::create(EqOpExpr::create(varV, k7), b7));
	leftInner->addClause(EqOpExpr::create(varV, k8), b8);
	leftInner->addClause(EqOpExpr::create(varV, k9), b9);
	ShPtr<IfStmt> rightInner(IfStmt::create(EqOpExpr::create(varV, k10), b10));
	rightInner->addClause(EqOpExpr::create(varV, k11), b11);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtEqOpExpr::create(varV, ConstInt::create(9, 64),
			LtEqOpExpr::Variant::SCmp),
		leftInner));
	outer->setElseClause(rightInner);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
}

TEST_F(IfToSwitchOptimizerTests,
GtSplitTwoConsecutiveInnerChainsMergesToSwitch) {
	// if (v > 9) { 10,11 } else { 7,8,9 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k7(ConstInt::create(7, 64));
	ShPtr<ConstInt> k8(ConstInt::create(8, 64));
	ShPtr<ConstInt> k9(ConstInt::create(9, 64));
	ShPtr<ConstInt> k10(ConstInt::create(10, 64));
	ShPtr<ConstInt> k11(ConstInt::create(11, 64));
	ShPtr<AssignStmt> b7(AssignStmt::create(varB, ConstInt::create(7, 32)));
	ShPtr<AssignStmt> b8(AssignStmt::create(varB, ConstInt::create(8, 32)));
	ShPtr<AssignStmt> b9(AssignStmt::create(varB, ConstInt::create(9, 32)));
	ShPtr<AssignStmt> b10(AssignStmt::create(varB, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> b11(AssignStmt::create(varB, ConstInt::create(11, 32)));
	ShPtr<IfStmt> elseInner(IfStmt::create(EqOpExpr::create(varV, k7), b7));
	elseInner->addClause(EqOpExpr::create(varV, k8), b8);
	elseInner->addClause(EqOpExpr::create(varV, k9), b9);
	ShPtr<IfStmt> thenInner(IfStmt::create(EqOpExpr::create(varV, k10), b10));
	thenInner->addClause(EqOpExpr::create(varV, k11), b11);
	ShPtr<IfStmt> outer(IfStmt::create(
		GtOpExpr::create(varV, ConstInt::create(9, 64),
			GtOpExpr::Variant::SCmp),
		thenInner));
	outer->setElseClause(elseInner);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
	std::vector<ShPtr<ConstInt>> expectKeys = {k7, k8, k9, k10, k11};
	unsigned ei = 0;
	for (auto it = sw->clause_begin(); it != sw->clause_end(); ++it, ++ei) {
		ASSERT_LT(ei, expectKeys.size());
		ASSERT_EQ(expectKeys[ei], it->first);
	}
	ASSERT_EQ(expectKeys.size(), ei);
}

TEST_F(IfToSwitchOptimizerTests,
GeSplitTwoConsecutiveInnerChainsMergesToSwitch) {
	// if (v >= 10) { 10,11 } else { 7,8,9 }
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<ConstInt> k7(ConstInt::create(7, 64));
	ShPtr<ConstInt> k8(ConstInt::create(8, 64));
	ShPtr<ConstInt> k9(ConstInt::create(9, 64));
	ShPtr<ConstInt> k10(ConstInt::create(10, 64));
	ShPtr<ConstInt> k11(ConstInt::create(11, 64));
	ShPtr<AssignStmt> b7(AssignStmt::create(varB, ConstInt::create(7, 32)));
	ShPtr<AssignStmt> b8(AssignStmt::create(varB, ConstInt::create(8, 32)));
	ShPtr<AssignStmt> b9(AssignStmt::create(varB, ConstInt::create(9, 32)));
	ShPtr<AssignStmt> b10(AssignStmt::create(varB, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> b11(AssignStmt::create(varB, ConstInt::create(11, 32)));
	ShPtr<IfStmt> elseInner(IfStmt::create(EqOpExpr::create(varV, k7), b7));
	elseInner->addClause(EqOpExpr::create(varV, k8), b8);
	elseInner->addClause(EqOpExpr::create(varV, k9), b9);
	ShPtr<IfStmt> thenInner(IfStmt::create(EqOpExpr::create(varV, k10), b10));
	thenInner->addClause(EqOpExpr::create(varV, k11), b11);
	ShPtr<IfStmt> outer(IfStmt::create(
		GtEqOpExpr::create(varV, ConstInt::create(10, 64),
			GtEqOpExpr::Variant::SCmp),
		thenInner));
	outer->setElseClause(elseInner);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<SwitchStmt> sw(cast<SwitchStmt>(testFunc->getBody()));
	ASSERT_TRUE(sw);
	ASSERT_EQ(varV, sw->getControlExpr());
}

TEST_F(IfToSwitchOptimizerTests,
LtSplitWrongRightStartNotOptimized) {
	ShPtr<Variable> varV(Variable::create("v", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<AssignStmt> body(AssignStmt::create(varB, ConstInt::create(0, 32)));
	ShPtr<IfStmt> leftInner(IfStmt::create(
		EqOpExpr::create(varV, ConstInt::create(0, 64)), body));
	leftInner->addClause(
		EqOpExpr::create(varV, ConstInt::create(1, 64)), body);
	// Should start at v==2 but uses v==3 — gap
	ShPtr<IfStmt> rightInner(IfStmt::create(
		EqOpExpr::create(varV, ConstInt::create(3, 64)), body));
	rightInner->addClause(
		EqOpExpr::create(varV, ConstInt::create(4, 64)), body);
	ShPtr<IfStmt> outer(IfStmt::create(
		LtOpExpr::create(varV, ConstInt::create(2, 64)), leftInner));
	outer->setElseClause(rightInner);
	testFunc->setBody(outer);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	Optimizer::optimize<IfToSwitchOptimizer>(module, va);

	ShPtr<IfStmt> out(cast<IfStmt>(testFunc->getBody()));
	ASSERT_TRUE(out);
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
