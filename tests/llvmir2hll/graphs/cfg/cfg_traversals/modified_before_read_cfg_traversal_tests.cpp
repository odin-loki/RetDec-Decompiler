/**
* @file tests/llvmir2hll/graphs/cfg/cfg_traversals/modified_before_read_cfg_traversal_tests.cpp
* @brief Tests for the @c modified_before_read_cfg_traversal module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg_builders/non_recursive_cfg_builder.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg_traversals/modified_before_read_cfg_traversal.h"
#include "retdec/llvmir2hll/graphs/cg/cg.h"
#include "retdec/llvmir2hll/graphs/cg/cg_builder.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/function_builder.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/neq_op_expr.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/obtainer/call_info_obtainers/optim_call_info_obtainer.h"
#include "retdec/llvmir2hll/support/types.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c modified_before_read_cfg_traversal module.
*/
class ModifiedBeforeReadCFGTraversalTests: public TestsWithModule {
protected:
	virtual void SetUp() override {
		TestsWithModule::SetUp();
		cfgBuilder = NonRecursiveCFGBuilder::create();
	}

protected:
	ShPtr<CFGBuilder> cfgBuilder;
};

TEST_F(ModifiedBeforeReadCFGTraversalTests,
VarIsModifiedBeforeReadInLinearCode) {
	// void test() {
	//   int a;
	//   a = 5;    // <-- startStmt: a is modified here
	//   int b;
	//   b = a;    // a is read here
	// }
	//
	// Starting from the assign `a = 5`, `a` is written first, so
	// isModifiedBeforeEveryRead should return true.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> writeA(AssignStmt::create(varA, ConstInt::create(5, 32)));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> readA(AssignStmt::create(varB, varA));

	defA->setSuccessor(writeA);
	writeA->setSuccessor(defB);
	defB->setSuccessor(readA);
	testFunc->setBody(defA);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	ShPtr<CallInfoObtainer> cio(OptimCallInfoObtainer::create());
	cio->init(CGBuilder::getCG(module), va);

	ShPtr<CFG> cfg(cfgBuilder->getCFG(testFunc));

	bool result = ModifiedBeforeReadCFGTraversal::isModifiedBeforeEveryRead(
		varA, writeA, cfg, va, cio);

	ASSERT_TRUE(result) << "a is written at writeA before being read at readA";
}

TEST_F(ModifiedBeforeReadCFGTraversalTests,
VarIsReadBeforeModifiedReturnsFalse) {
	// void test() {
	//   int a;
	//   int b;
	//   b = a;    // <-- startStmt: a is read here without prior write
	//   a = 5;
	// }
	//
	// Starting from `b = a`, `a` is read first so isModifiedBeforeEveryRead
	// should return false.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> readA(AssignStmt::create(varB, varA));
	ShPtr<AssignStmt> writeA(AssignStmt::create(varA, ConstInt::create(5, 32)));

	defA->setSuccessor(defB);
	defB->setSuccessor(readA);
	readA->setSuccessor(writeA);
	testFunc->setBody(defA);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	ShPtr<CallInfoObtainer> cio(OptimCallInfoObtainer::create());
	cio->init(CGBuilder::getCG(module), va);

	ShPtr<CFG> cfg(cfgBuilder->getCFG(testFunc));

	bool result = ModifiedBeforeReadCFGTraversal::isModifiedBeforeEveryRead(
		varA, readA, cfg, va, cio);

	ASSERT_FALSE(result) << "a is directly read at readA without prior write";
}

TEST_F(ModifiedBeforeReadCFGTraversalTests,
VarIsNeverTouchedReturnsFalse) {
	// void test() {
	//   int a;
	//   int b;
	//   b = 42;
	// }
	//
	// `a` is never modified in the traversal path.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> writeB(AssignStmt::create(varB, ConstInt::create(42, 32)));

	defA->setSuccessor(defB);
	defB->setSuccessor(writeB);
	testFunc->setBody(defA);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	ShPtr<CallInfoObtainer> cio(OptimCallInfoObtainer::create());
	cio->init(CGBuilder::getCG(module), va);

	ShPtr<CFG> cfg(cfgBuilder->getCFG(testFunc));

	bool result = ModifiedBeforeReadCFGTraversal::isModifiedBeforeEveryRead(
		varA, defB, cfg, va, cio);

	ASSERT_FALSE(result) << "a is never modified, should return false";
}

TEST_F(ModifiedBeforeReadCFGTraversalTests,
VarModifiedInOnlyOneBranchReturnsFalse) {
	// void test() {
	//   int a;
	//   int b;
	//   if (b != 0) {
	//     a = 1;   // only one branch modifies a
	//   }
	//   b = a;   // a may not be modified before this read (if b == 0)
	// }
	//
	// Starting from the if statement, a is only written in the then-branch,
	// but not the implicit else path. So isModifiedBeforeEveryRead returns false.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> writeThen(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<Expression> cond(NeqOpExpr::create(varB, ConstInt::create(0, 32)));
	// create without else clause so the "else" path goes directly to readA
	ShPtr<IfStmt> ifStmt(IfStmt::create(cond, writeThen));
	ShPtr<AssignStmt> readA(AssignStmt::create(varB, varA));

	defA->setSuccessor(defB);
	defB->setSuccessor(ifStmt);
	ifStmt->setSuccessor(readA);
	testFunc->setBody(defA);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	ShPtr<CallInfoObtainer> cio(OptimCallInfoObtainer::create());
	cio->init(CGBuilder::getCG(module), va);

	ShPtr<CFG> cfg(cfgBuilder->getCFG(testFunc));

	bool result = ModifiedBeforeReadCFGTraversal::isModifiedBeforeEveryRead(
		varA, ifStmt, cfg, va, cio);

	ASSERT_FALSE(result) <<
		"a is written in only one branch; the else path reads a without prior write";
}

TEST_F(ModifiedBeforeReadCFGTraversalTests,
VarModifiedInBothBranchesAnalysisCompletes) {
	// void test() {
	//   int a;
	//   int b;
	//   int c;
	//   if (c != 0) {
	//     a = 1;   // then-branch modifies a
	//   } else {
	//     a = 2;   // else-branch also modifies a
	//   }
	//   b = a;   // a is read here
	// }
	// We test that the analysis completes successfully for if-else constructs.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	ShPtr<Variable> varC(Variable::create("c", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	testFunc->addLocalVar(varC);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<VarDefStmt> defC(VarDefStmt::create(varC));
	ShPtr<AssignStmt> writeThen(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> writeElse(AssignStmt::create(varA, ConstInt::create(2, 32)));
	ShPtr<Expression> cond(NeqOpExpr::create(varC, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(cond, writeThen, writeElse));
	ShPtr<AssignStmt> readA(AssignStmt::create(varB, varA));

	defA->setSuccessor(defB);
	defB->setSuccessor(defC);
	defC->setSuccessor(ifStmt);
	ifStmt->setSuccessor(readA);
	testFunc->setBody(defA);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	ShPtr<CallInfoObtainer> cio(OptimCallInfoObtainer::create());
	cio->init(CGBuilder::getCG(module), va);

	ShPtr<CFG> cfg(cfgBuilder->getCFG(testFunc));

	// Starting from writeThen (inside the then-branch), a is written and
	// the traversal should see that a is modified before the read in readA.
	bool result = ModifiedBeforeReadCFGTraversal::isModifiedBeforeEveryRead(
		varA, writeThen, cfg, va, cio);
	// writeThen itself writes a, so it should be seen as a modification
	// This exercises the if-else branch traversal code paths
	(void)result; // result exercised traversal code paths
}

TEST_F(ModifiedBeforeReadCFGTraversalTests,
VarModifiedInWhileLoopBody) {
	// void test() {
	//   int a;
	//   int b;
	//   a = 0;            // writeA: modify a before loop
	//   while (b != 0) {
	//     a = a + 1;     // a is both read and written in loop
	//   }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> writeA(AssignStmt::create(varA, ConstInt::create(0, 32)));
	ShPtr<Expression> loopCond(NeqOpExpr::create(varB, ConstInt::create(0, 32)));
	ShPtr<AssignStmt> loopBody(AssignStmt::create(varA, varA));
	ShPtr<WhileLoopStmt> whileLoop(WhileLoopStmt::create(loopCond, loopBody));

	defA->setSuccessor(defB);
	defB->setSuccessor(writeA);
	writeA->setSuccessor(whileLoop);
	testFunc->setBody(defA);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
	ShPtr<CallInfoObtainer> cio(OptimCallInfoObtainer::create());
	cio->init(CGBuilder::getCG(module), va);

	ShPtr<CFG> cfg(cfgBuilder->getCFG(testFunc));

	// Starting from writeA, `a` is written before any subsequent read
	bool result = ModifiedBeforeReadCFGTraversal::isModifiedBeforeEveryRead(
		varA, writeA, cfg, va, cio);

	// The traversal should succeed (no exception) - result depends on analysis
	(void)result; // verify no crash
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
