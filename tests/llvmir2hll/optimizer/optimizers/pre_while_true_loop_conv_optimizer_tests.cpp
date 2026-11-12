/**
* @file tests/llvmir2hll/optimizer/optimizers/pre_while_true_loop_conv_optimizer_tests.cpp
* @brief Tests for the @c pre_while_true_loop_conv_optimizer module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <gtest/gtest.h>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/binary_op_expr.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/gt_eq_op_expr.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/or_op_expr.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/optimizer/optimizers/pre_while_true_loop_conv_optimizer.h"
#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluators/strict_arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/hll/hll_writers/c_hll_writer.h"
#include "retdec/llvmir2hll/obtainer/call_info_obtainers/pessim_call_info_obtainer.h"
#include "retdec/llvmir2hll/optimizer/optimizer_manager.h"

#include <llvm/Support/raw_ostream.h>

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c pre_while_true_loop_conv_optimizer module.
*/
class PreWhileTrueLoopConvOptimizerTests: public TestsWithModule {};

TEST_F(PreWhileTrueLoopConvOptimizerTests,
OptimizerHasNonEmptyID) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	ShPtr<PreWhileTrueLoopConvOptimizer> optimizer(new PreWhileTrueLoopConvOptimizer(
		module, va));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(PreWhileTrueLoopConvOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	// Optimize the module.
	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	// Check that the output is correct.
	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody())) <<
		"expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got " <<
		testFunc->getBody()->getSuccessor();
}

TEST_F(PreWhileTrueLoopConvOptimizerTests,
OptimizeCase1Simplest) {
	// Set-up the module.
	//
	// void test() {
	//     int tmp;
	//     int i;
	//     i = 1;
	//     while (true) {
	//         tmp = i + 1;
	//         if (tmp >= 1) {
	//             break;
	//         }
	//         i = tmp;
	//     }
	// }
	//
	// is optimized to
	//
	// void test() {
	//     int i;
	//     i = 1;
	//     while (true) {
	//         if (i + 1 >= 1) {
	//             break;
	//         }
	//         i = i + 1;
	//     }
	// }
	//
	ShPtr<Variable> varTmp(Variable::create("tmp", IntType::create(32)));
	testFunc->addLocalVar(varTmp);
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<AssignStmt> assignITmp(AssignStmt::create(varI, varTmp));
	ShPtr<BreakStmt> breakIf(BreakStmt::create());
	ShPtr<Expression> ifCond(GtEqOpExpr::create(varTmp, ConstInt::create(1, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, breakIf, assignITmp));
	ShPtr<AssignStmt> assignTmpI1(AssignStmt::create(varTmp,
		AddOpExpr::create(varI, ConstInt::create(1, 32)), ifStmt));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), assignTmpI1));
	ShPtr<AssignStmt> assignI1(AssignStmt::create(varI, ConstInt::create(1, 32), whileLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), assignI1));
	ShPtr<VarDefStmt> varDefTmp(VarDefStmt::create(varTmp, ShPtr<Expression>(), varDefI));
	testFunc->setBody(varDefTmp);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	// Optimize the module.
	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	// Check that the output is correct.
	EXPECT_EQ(varDefI, testFunc->getBody()) <<
		"expected `" << varDefI << "`, got `" << testFunc->getBody() << "`";
	EXPECT_EQ(ifStmt, whileLoop->getBody()) <<
		"expected `" << ifStmt << "`, got `" << whileLoop->getBody() << "`";
	ShPtr<AssignStmt> afterIfStmt(cast<AssignStmt>(ifStmt->getSuccessor()));
	EXPECT_EQ(varI, afterIfStmt->getLhs()) <<
		"expected `" << varI << "`, got `" << afterIfStmt->getLhs() << "`";
	ShPtr<AddOpExpr> afterIfStmtRhs(cast<AddOpExpr>(afterIfStmt->getRhs()));
	ASSERT_TRUE(afterIfStmtRhs) <<
		"expected `" << afterIfStmtRhs << "`, got `" << afterIfStmt->getRhs() << "`";
	EXPECT_EQ(varI, afterIfStmtRhs->getFirstOperand()) <<
		"expected `" << varI << "`, got `" << afterIfStmtRhs->getFirstOperand() << "`";
	ShPtr<GtEqOpExpr> newIfCond(cast<GtEqOpExpr>(ifStmt->getFirstIfCond()));
	ASSERT_TRUE(newIfCond) <<
		"expected `" << newIfCond << "`, got `" << ifStmt->getFirstIfCond() << "`";
	EXPECT_TRUE(isa<AddOpExpr>(newIfCond->getFirstOperand())) <<
		"expected AddOpExpr, got `" << newIfCond->getFirstOperand() << "`";
}

TEST_F(PreWhileTrueLoopConvOptimizerTests,
OptimizeCase2Simplest) {
	// Set-up the module.
	//
	// void test() {
	//     int tmp;
	//     int i;
	//     i = 1;
	//     while (true) {
	//         tmp = i;
	//         i = tmp + 1;
	//         if (tmp == 100) {
	//             break;
	//         }
	//     }
	// }
	//
	// is optimized to
	//
	// void test() {
	//     int i;
	//     i = 1;
	//     while (true) {
	//         if (i == 100) {
	//             break;
	//         }
	//         i = i + 1
	//     }
	// }
	//
	ShPtr<Variable> varTmp(Variable::create("tmp", IntType::create(32)));
	testFunc->addLocalVar(varTmp);
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakIf(BreakStmt::create());
	ShPtr<Expression> ifCond(EqOpExpr::create(varTmp, ConstInt::create(100, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, breakIf));
	ShPtr<AssignStmt> assignITmp1(AssignStmt::create(varI,
		AddOpExpr::create(varTmp, ConstInt::create(1, 32)), ifStmt));
	ShPtr<AssignStmt> assignTmpI(AssignStmt::create(varTmp, varI, assignITmp1));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), assignTmpI));
	ShPtr<AssignStmt> assignI1(AssignStmt::create(varI, ConstInt::create(1, 32), whileLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), assignI1));
	ShPtr<VarDefStmt> varDefTmp(VarDefStmt::create(varTmp, ShPtr<Expression>(), varDefI));
	testFunc->setBody(varDefTmp);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	// Optimize the module.
	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	// Check that the output is correct.
	EXPECT_EQ(varDefI, testFunc->getBody()) <<
		"expected `" << varDefI << "`, got `" << testFunc->getBody() << "`";
	EXPECT_EQ(ifStmt, whileLoop->getBody()) <<
		"expected `" << ifStmt << "`, got `" << whileLoop->getBody() << "`";
	ShPtr<AssignStmt> afterIfStmt(cast<AssignStmt>(ifStmt->getSuccessor()));
	ASSERT_TRUE(afterIfStmt) <<
		"expected a successor of `" << ifStmt << "`";
	EXPECT_EQ(varI, afterIfStmt->getLhs()) <<
		"expected `" << varI << "`, got `" << afterIfStmt->getLhs() << "`";
	ShPtr<AddOpExpr> afterIfStmtRhs(cast<AddOpExpr>(afterIfStmt->getRhs()));
	ASSERT_TRUE(afterIfStmtRhs) <<
		"expected `" << afterIfStmtRhs << "`, got `" << afterIfStmt->getRhs() << "`";
	EXPECT_EQ(varI, afterIfStmtRhs->getFirstOperand()) <<
		"expected `" << varI << "`, got `" << afterIfStmtRhs->getFirstOperand() << "`";
	ShPtr<EqOpExpr> newIfCond(cast<EqOpExpr>(ifStmt->getFirstIfCond()));
	ASSERT_TRUE(newIfCond) <<
		"expected `" << newIfCond << "`, got `" << ifStmt->getFirstIfCond() << "`";
	EXPECT_EQ(varI, newIfCond->getFirstOperand()) <<
		"expected `" << varI << "`, got `" << newIfCond->getFirstOperand() << "`";
}

TEST_F(PreWhileTrueLoopConvOptimizerTests,
OptimizeCase3Simplest) {
	// Set-up the module.
	//
	// void test() {
	//     int x;
	//     int i;
	//     i = 0;
	//     if (i >= x) {
	//         return;
	//     }
	//     while (true) { }
	// }
	//
	// is optimized to
	//
	// void test() {
	//     int x;
	//     int i;
	//     if (0 >= x) {
	//         return;
	//     }
	//     i = 0;
	//     while (true) { }
	// }
	//
	ShPtr<Variable> varX(Variable::create("x", IntType::create(32)));
	testFunc->addLocalVar(varX);
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), EmptyStmt::create()));
	ShPtr<BinaryOpExpr> ifCond(GtEqOpExpr::create(varI, varX));
	ShPtr<ReturnStmt> returnStmt(ReturnStmt::create());
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, returnStmt, whileLoop));
	ShPtr<AssignStmt> assignI0(AssignStmt::create(varI, ConstInt::create(0, 32), ifStmt));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), assignI0));
	ShPtr<VarDefStmt> varDefX(VarDefStmt::create(varX, ShPtr<Expression>(), varDefI));
	testFunc->setBody(varDefX);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	// Optimize the module.
	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	// Check that the output is correct.
	EXPECT_EQ(assignI0, ifStmt->getSuccessor()) <<
		"expected `" << assignI0 << "`, got `" << ifStmt->getSuccessor() << "`";
	EXPECT_EQ(varDefI, ifStmt->getUniquePredecessor()) <<
		"expected `" << varDefI << "`, got `" << ifStmt->getUniquePredecessor() << "`";
	ASSERT_TRUE(isa<ConstInt>(ifCond->getFirstOperand())) <<
		"expected `ConstInt`, got `" << ifCond->getFirstOperand() << "`";
	EXPECT_TRUE(cast<ConstInt>(ifCond->getFirstOperand())->isZero()) <<
		"expected `0`, got `" << ifCond->getFirstOperand() << "`";
}

TEST_F(PreWhileTrueLoopConvOptimizerTests,
OptimizeCase4Simplest) {
	// Set-up the module.
	//
	// void test() {
	//     int tmp;
	//     int i;
	//     i = 1;
	//     while (true) {
	//         tmp = test();
	//         if (i >= tmp) {
	//             break;
	//         }
	//         i = i + 1;
	//     }
	// }
	//
	// is optimized to
	//
	// void test() {
	//     int i;
	//     i = 1;
	//     while (true) {
	//         if (i >= test()) {
	//             break;
	//         }
	//         i = i + 1;
	//     }
	// }
	//
	ShPtr<Variable> varTmp(Variable::create("tmp", IntType::create(32)));
	testFunc->addLocalVar(varTmp);
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<AssignStmt> assignII1(AssignStmt::create(varI,
		AddOpExpr::create(varI, ConstInt::create(1, 32))));
	ShPtr<BreakStmt> breakIf(BreakStmt::create());
	ShPtr<GtEqOpExpr> ifCond(GtEqOpExpr::create(varI, varTmp));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, breakIf, assignII1));
	ShPtr<CallExpr> callExpr(CallExpr::create(testFunc->getAsVar()));
	ShPtr<AssignStmt> assignTmpTest(AssignStmt::create(varTmp, callExpr, ifStmt));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), assignTmpTest));
	ShPtr<AssignStmt> assignI1(AssignStmt::create(varI, ConstInt::create(1, 32), whileLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), whileLoop));
	ShPtr<VarDefStmt> varDefTmp(VarDefStmt::create(varTmp, ShPtr<Expression>(), varDefI));
	testFunc->setBody(varDefTmp);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	// Optimize the module.
	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	// Check that the output is correct.
	EXPECT_EQ(varDefI, testFunc->getBody()) <<
		"expected `" << varDefI << "`, got `" << testFunc->getBody() << "`";
	EXPECT_EQ(ifStmt, whileLoop->getBody()) <<
		"expected `" << ifStmt << "`, got `" << whileLoop->getBody() << "`";
	EXPECT_EQ(callExpr, ifCond->getSecondOperand()) <<
		"expected `" << callExpr << "`, got `" << ifCond->getSecondOperand() << "`";
}

TEST_F(PreWhileTrueLoopConvOptimizerTests,
OptimizeCase5Simplest) {
	// Set-up the module.
	//
	// void test() {
	//     int i;
	//     i = 1;
	//     while (true) {
	//         i = i + 1;
	//         if (i == 100) {
	//             break;
	//         }
	//     }
	// }
	//
	// is optimized to
	//
	// void test() {
	//     int i;
	//     i = 1;
	//     while (true) {
	//         if (i + 1 == 100) {
	//             break;
	//         }
	//         i = i + 1
	//     }
	// }
	//
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakIf(BreakStmt::create());
	ShPtr<BinaryOpExpr> ifCond(EqOpExpr::create(varI, ConstInt::create(100, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, breakIf));
	ShPtr<AssignStmt> assignII1(AssignStmt::create(varI,
		AddOpExpr::create(varI, ConstInt::create(1, 32)), ifStmt));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(
		ConstBool::create(true), assignII1));
	ShPtr<AssignStmt> assignI1(AssignStmt::create(varI, ConstInt::create(1, 32), whileLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), assignI1));
	testFunc->setBody(varDefI);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	// Optimize the module.
	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	// Check that the output is correct.
	ASSERT_TRUE(ifStmt->getSuccessor()) <<
		"expected the if statement to have a successor";
	EXPECT_TRUE(isa<AddOpExpr>(ifCond->getFirstOperand())) <<
		"expected the left-hand side of the if condition to be an addition, " <<
		"got `" << ifStmt->getSuccessor() << "`";
}


/// The shape generated_float_newton_sqrt-gcc-O2 reached this pass in:
///
///     while (true) {
///         tmp = i;
///         i = tmp + 1;
///         if (i >= tmp) {      // reads the new i *and* the old one
///             break;
///         }
///     }
///
/// Case (2) would give `if (i >= i) break; i = i + 1;` -- the condition reads
/// the old i where it read the new one. (The real loop was Newton's method,
/// `if (fabs(next - guess) < eps) break`, and became `fabs(0.0)`.)
TEST_F(PreWhileTrueLoopConvOptimizerTests, Case2IsNotDoneWhenTheLoopEndReadsTheUpdatedVariable)
{
	ShPtr<Variable> varTmp(Variable::create("tmp", IntType::create(32)));
	testFunc->addLocalVar(varTmp);
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakIf(BreakStmt::create());
	ShPtr<Expression> ifCond(GtEqOpExpr::create(varI, varTmp));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, breakIf));
	ShPtr<AssignStmt> assignITmp1(AssignStmt::create(varI, AddOpExpr::create(varTmp, ConstInt::create(1, 32)), ifStmt));
	ShPtr<AssignStmt> assignTmpI(AssignStmt::create(varTmp, varI, assignITmp1));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(ConstBool::create(true), assignTmpI));
	ShPtr<AssignStmt> assignI1(AssignStmt::create(varI, ConstInt::create(1, 32), whileLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), assignI1));
	ShPtr<VarDefStmt> varDefTmp(VarDefStmt::create(varTmp, ShPtr<Expression>(), varDefI));
	testFunc->setBody(varDefTmp);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	EXPECT_EQ(assignTmpI, whileLoop->getBody())
		<< "expected the loop to be left alone, got `" << whileLoop->getBody() << "`";
	EXPECT_EQ(assignITmp1, assignTmpI->getSuccessor());
	EXPECT_EQ(ifStmt, assignITmp1->getSuccessor());
	EXPECT_EQ(varTmp, cast<GtEqOpExpr>(ifStmt->getFirstIfCond())->getSecondOperand())
		<< "got `" << ifStmt->getFirstIfCond() << "`";
}

/// The other half of the same defect:
///
///     while (true) {
///         tmp = i;
///         i = tmp + 1;
///         if (tmp == 100) {
///             break;
///         }
///     }
///     return i;
///
/// returns 101. After case (2) the update runs after the exit test, so it
/// would return 100.
TEST_F(PreWhileTrueLoopConvOptimizerTests, Case2IsNotDoneWhenTheUpdatedVariableIsReadAfterTheLoop)
{
	ShPtr<Variable> varTmp(Variable::create("tmp", IntType::create(32)));
	testFunc->addLocalVar(varTmp);
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakIf(BreakStmt::create());
	ShPtr<Expression> ifCond(EqOpExpr::create(varTmp, ConstInt::create(100, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, breakIf));
	ShPtr<AssignStmt> assignITmp1(AssignStmt::create(varI, AddOpExpr::create(varTmp, ConstInt::create(1, 32)), ifStmt));
	ShPtr<AssignStmt> assignTmpI(AssignStmt::create(varTmp, varI, assignITmp1));
	ShPtr<ReturnStmt> returnI(ReturnStmt::create(varI));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(ConstBool::create(true), assignTmpI, returnI));
	ShPtr<AssignStmt> assignI1(AssignStmt::create(varI, ConstInt::create(1, 32), whileLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), assignI1));
	ShPtr<VarDefStmt> varDefTmp(VarDefStmt::create(varTmp, ShPtr<Expression>(), varDefI));
	testFunc->setBody(varDefTmp);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<PreWhileTrueLoopConvOptimizer>(module, va);

	EXPECT_EQ(assignTmpI, whileLoop->getBody())
		<< "expected the loop to be left alone, got `" << whileLoop->getBody() << "`";
	EXPECT_EQ(assignITmp1, assignTmpI->getSuccessor());
	EXPECT_EQ(ifStmt, assignITmp1->getSuccessor());
	EXPECT_EQ(varDefTmp, testFunc->getBody());
}

/// ValueAnalysis caches the variables each statement reads, and most passes
/// rewrite statements in place without telling it. IfStructureOptimizer
/// merging `if (a) break; if (b) break;` into one `if` is the case CC-01 hit:
/// the cache still held the reads of the condition before the merge.
///
/// Here the loop end starts as `if (tmp == 100)` -- which case (2) may
/// rewrite -- and its reads are cached; then the condition is rewritten in
/// place to `tmp == 100 || i == 7`, which case (2) must not touch because it
/// reads the new i. Run through OptimizerManager, the pass has to see the
/// rewritten condition.
TEST_F(PreWhileTrueLoopConvOptimizerTests, OptimizerManagerDoesNotHandThePassAStaleValueAnalysis)
{
	ShPtr<Variable> varTmp(Variable::create("tmp", IntType::create(32)));
	testFunc->addLocalVar(varTmp);
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<BreakStmt> breakIf(BreakStmt::create());
	ShPtr<Expression> ifCond(EqOpExpr::create(varTmp, ConstInt::create(100, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, breakIf));
	ShPtr<AssignStmt> assignITmp1(AssignStmt::create(varI, AddOpExpr::create(varTmp, ConstInt::create(1, 32)), ifStmt));
	ShPtr<AssignStmt> assignTmpI(AssignStmt::create(varTmp, varI, assignITmp1));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(ConstBool::create(true), assignTmpI));
	ShPtr<AssignStmt> assignI1(AssignStmt::create(varI, ConstInt::create(1, 32), whileLoop));
	ShPtr<VarDefStmt> varDefI(VarDefStmt::create(varI, ShPtr<Expression>(), assignI1));
	ShPtr<VarDefStmt> varDefTmp(VarDefStmt::create(varTmp, ShPtr<Expression>(), varDefI));
	testFunc->setBody(varDefTmp);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	ShPtr<ValueAnalysis> cachingVa(ValueAnalysis::create(aliasAnalysis, true));
	cachingVa->clearCache();

	// What an earlier pass would have left in the cache.
	ASSERT_FALSE(cachingVa->getValueData(ifStmt)->isDirRead(varI));

	// The in-place rewrite that pass does not report.
	ifStmt->setFirstIfCond(OrOpExpr::create(ifCond, EqOpExpr::create(varI, ConstInt::create(7, 32))));

	std::string sink;
	llvm::raw_string_ostream sinkStream(sink);
	OptimizerManager om(
		StringSet{"PreWhileTrueLoopConv"},
		StringSet(),
		CHLLWriter::create(sinkStream),
		cachingVa,
		PessimCallInfoObtainer::create(),
		StrictArithmExprEvaluator::create(),
		false);
	om.optimize(module);

	EXPECT_EQ(assignTmpI, whileLoop->getBody())
		<< "expected the loop to be left alone, got `" << whileLoop->getBody() << "`";
	EXPECT_EQ(ifStmt, assignITmp1->getSuccessor());
	auto cond = cast<OrOpExpr>(ifStmt->getFirstIfCond());
	ASSERT_TRUE(cond) << "got `" << ifStmt->getFirstIfCond() << "`";
	EXPECT_EQ(varTmp, cast<EqOpExpr>(cond->getFirstOperand())->getFirstOperand())
		<< "got `" << ifStmt->getFirstIfCond() << "`";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
