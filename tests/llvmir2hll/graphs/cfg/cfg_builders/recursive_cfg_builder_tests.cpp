/**
* @file tests/llvmir2hll/graphs/cfg/cfg_builders/recursive_cfg_builder_tests.cpp
* @brief Tests for the @c recursive_cfg_builder module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/graphs/cfg/cfg.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg_builders/recursive_cfg_builder.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/break_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/const_bool.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/continue_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/ir/function_builder.h"
#include "retdec/llvmir2hll/ir/goto_stmt.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/lt_op_expr.h"
#include "retdec/llvmir2hll/ir/neq_op_expr.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/switch_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/unreachable_stmt.h"
#include "retdec/llvmir2hll/ir/ufor_loop_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/ir/void_type.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c recursive_cfg_builder module.
*/
class RecursiveCFGBuilderTests: public TestsWithModule {
protected:
	virtual void SetUp() override {
		TestsWithModule::SetUp();
		cfgBuilder = RecursiveCFGBuilder::create();
	}

	ShPtr<CFG> buildCFG() {
		return cfgBuilder->getCFG(testFunc);
	}

protected:
	ShPtr<CFGBuilder> cfgBuilder;
};

//
// Basic statement coverage tests.
//

TEST_F(RecursiveCFGBuilderTests,
EmptyFunctionHasEntryAndExitNodes) {
	// void test() {}
	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getEntryNode());
	ASSERT_TRUE(cfg->getExitNode());
}

TEST_F(RecursiveCFGBuilderTests,
SimpleVarDefAndAssignStmts) {
	// void test() {
	//   int a;
	//   int b;
	//   a = b;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, varB));
	defA->setSuccessor(defB);
	defB->setSuccessor(assign);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	// entry -> body node -> exit: 3 nodes minimum
	ASSERT_GE(cfg->getNumberOfNodes(), 3u);
	ASSERT_TRUE(cfg->getNodeForStmt(defA).first);
	ASSERT_TRUE(cfg->getNodeForStmt(defB).first);
	ASSERT_TRUE(cfg->getNodeForStmt(assign).first);
}

TEST_F(RecursiveCFGBuilderTests,
ReturnStmtCreatesEdgeToExit) {
	// void test() {
	//   return;
	// }
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	testFunc->setBody(ret);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	auto nodeForRet = cfg->getNodeForStmt(ret);
	ASSERT_TRUE(nodeForRet.first);

	// The return node should have the exit node as successor.
	bool hasExitSucc = false;
	for (auto i = nodeForRet.first->succ_begin(),
			e = nodeForRet.first->succ_end(); i != e; ++i) {
		if ((*i)->getDst() == cfg->getExitNode()) {
			hasExitSucc = true;
		}
	}
	ASSERT_TRUE(hasExitSucc) << "Return statement should create edge to exit node";
}

TEST_F(RecursiveCFGBuilderTests,
EmptyStmtIsIncluded) {
	// void test() {
	//   ; (EmptyStmt)
	// }
	ShPtr<EmptyStmt> empty(EmptyStmt::create());
	testFunc->setBody(empty);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
}

TEST_F(RecursiveCFGBuilderTests,
IfStmtWithNoElseCreatesBranch) {
	// void test() {
	//   int a;
	//   if (a != 0) { a = 1; }
	//   return;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assignOne(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<Expression> cond(NeqOpExpr::create(varA, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(cond, assignOne));
	ShPtr<ReturnStmt> ret(ReturnStmt::create());

	defA->setSuccessor(ifStmt);
	ifStmt->setSuccessor(ret);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_GE(cfg->getNumberOfNodes(), 4u);  // entry + def + if + body + ret/exit
	ASSERT_TRUE(cfg->getNodeForStmt(ifStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
IfStmtWithElseClauseBothBranchesCovered) {
	// void test() {
	//   int a;
	//   if (a != 0) { a = 1; } else { a = 2; }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> thenBranch(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> elseBranch(AssignStmt::create(varA, ConstInt::create(2, 32)));
	ShPtr<Expression> cond(NeqOpExpr::create(varA, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(cond, thenBranch, elseBranch));

	defA->setSuccessor(ifStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(ifStmt).first);
	ASSERT_TRUE(cfg->getNodeForStmt(thenBranch).first);
	ASSERT_TRUE(cfg->getNodeForStmt(elseBranch).first);
}

TEST_F(RecursiveCFGBuilderTests,
IfStmtWithElseIfChain) {
	// void test() {
	//   int a;
	//   if (a == 1) { a = 10; }
	//   else if (a == 2) { a = 20; }
	//   else { a = 30; }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<AssignStmt> body1(AssignStmt::create(varA, ConstInt::create(10, 32)));
	ShPtr<AssignStmt> body2(AssignStmt::create(varA, ConstInt::create(20, 32)));
	ShPtr<AssignStmt> bodyElse(AssignStmt::create(varA, ConstInt::create(30, 32)));

	ShPtr<Expression> cond1(NeqOpExpr::create(varA, ConstInt::create(1, 32)));
	ShPtr<Expression> cond2(NeqOpExpr::create(varA, ConstInt::create(2, 32)));

	ShPtr<IfStmt> ifStmt(IfStmt::create(cond1, body1));
	ifStmt->addClause(cond2, body2);
	ifStmt->setElseClause(bodyElse);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	defA->setSuccessor(ifStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(ifStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
WhileLoopCreatesLoopNode) {
	// void test() {
	//   int a;
	//   while (a != 0) { a = a - 1; }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> decA(AssignStmt::create(varA, ConstInt::create(0, 32)));
	ShPtr<Expression> cond(NeqOpExpr::create(varA, ConstInt::create(0, 32)));
	ShPtr<WhileLoopStmt> whileStmt(WhileLoopStmt::create(cond, decA));

	defA->setSuccessor(whileStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(whileStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
WhileTrueLoopHasNoFalseEdge) {
	// void test() {
	//   while (true) { int a; return; }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	defA->setSuccessor(ret);

	ShPtr<Expression> trueCond(ConstBool::create(true));
	ShPtr<WhileLoopStmt> whileStmt(WhileLoopStmt::create(trueCond, defA));

	testFunc->setBody(whileStmt);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(whileStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
ForLoopCreatesLoopStructure) {
	// void test() {
	//   for (int i = 0; i < 10; i++) { int a; }
	// }
	ShPtr<Variable> varI(Variable::create("i", IntType::create(32)));
	testFunc->addLocalVar(varI);
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<Expression> initI(ConstInt::create(0, 32));
	ShPtr<Expression> condI(LtOpExpr::create(varI, ConstInt::create(10, 32)));
	ShPtr<Expression> stepI(ConstInt::create(1, 32));

	ShPtr<ForLoopStmt> forStmt(ForLoopStmt::create(varI, initI, condI, stepI, defA));
	testFunc->setBody(forStmt);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(forStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
SwitchStmtWithClauses) {
	// void test() {
	//   int a;
	//   switch (a) {
	//     case 1: a = 10; break;
	//     case 2: a = 20;
	//   }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> case1Body(AssignStmt::create(varA, ConstInt::create(10, 32)));
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	case1Body->setSuccessor(breakStmt);

	ShPtr<AssignStmt> case2Body(AssignStmt::create(varA, ConstInt::create(20, 32)));

	ShPtr<SwitchStmt> switchStmt(SwitchStmt::create(varA));
	switchStmt->addClause(ConstInt::create(1, 32), case1Body);
	switchStmt->addClause(ConstInt::create(2, 32), case2Body);

	defA->setSuccessor(switchStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(switchStmt).first);
	ASSERT_TRUE(cfg->getNodeForStmt(case1Body).first);
	ASSERT_TRUE(cfg->getNodeForStmt(case2Body).first);
}

TEST_F(RecursiveCFGBuilderTests,
SwitchStmtWithDefaultClause) {
	// void test() {
	//   int a;
	//   switch (a) {
	//     case 1: return;
	//     default: a = 0;
	//   }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<ReturnStmt> case1Body(ReturnStmt::create());
	ShPtr<AssignStmt> defaultBody(AssignStmt::create(varA, ConstInt::create(0, 32)));

	ShPtr<SwitchStmt> switchStmt(SwitchStmt::create(varA));
	switchStmt->addClause(ConstInt::create(1, 32), case1Body);
	switchStmt->addDefaultClause(defaultBody);

	defA->setSuccessor(switchStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(switchStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
BreakStmtInWhileLoop) {
	// void test() {
	//   int a;
	//   while (true) {
	//     if (a == 0) { break; }
	//     a = a - 1;
	//   }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	ShPtr<Expression> cond(NeqOpExpr::create(varA, ConstInt::create(0, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(cond, breakStmt));
	ShPtr<AssignStmt> decA(AssignStmt::create(varA, ConstInt::create(0, 32)));
	ifStmt->setSuccessor(decA);

	ShPtr<Expression> trueCond(ConstBool::create(true));
	ShPtr<WhileLoopStmt> whileStmt(WhileLoopStmt::create(trueCond, ifStmt));

	defA->setSuccessor(whileStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(breakStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
ContinueStmtInWhileLoop) {
	// void test() {
	//   int a;
	//   while (a != 0) {
	//     if (a == 5) { continue; }
	//     a = a - 1;
	//   }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<ContinueStmt> contStmt(ContinueStmt::create());
	ShPtr<Expression> innerCond(NeqOpExpr::create(varA, ConstInt::create(5, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(innerCond, contStmt));
	ShPtr<AssignStmt> decA(AssignStmt::create(varA, ConstInt::create(0, 32)));
	ifStmt->setSuccessor(decA);

	ShPtr<Expression> outerCond(NeqOpExpr::create(varA, ConstInt::create(0, 32)));
	ShPtr<WhileLoopStmt> whileStmt(WhileLoopStmt::create(outerCond, ifStmt));

	defA->setSuccessor(whileStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(contStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
GotoStmtCreatesEdgeToTarget) {
	// void test() {
	//   int a;
	//   int b;
	//   b = 1;
	//   goto: a = 2;   (goto targets assignA)
	//   a = 2;
	// }
	//
	// The non-recursive builder handles gotos by detecting the back-reference.
	// We replicate the pattern from non_recursive_cfg_builder_tests.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> assignB(AssignStmt::create(varB, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assignA(AssignStmt::create(varA, ConstInt::create(2, 32)));

	// Create a goto statement that targets assignA (a backward jump).
	ShPtr<GotoStmt> gotoStmt(GotoStmt::create(assignA));

	defA->setSuccessor(defB);
	defB->setSuccessor(assignB);
	assignB->setSuccessor(assignA);
	assignA->setSuccessor(gotoStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(gotoStmt).first);
	ASSERT_TRUE(cfg->getNodeForStmt(assignA).first);
}

TEST_F(RecursiveCFGBuilderTests,
UnreachableStmtCreatesEdgeToExit) {
	// void test() {
	//   unreachable;
	// }
	ShPtr<UnreachableStmt> unreachable(UnreachableStmt::create());
	testFunc->setBody(unreachable);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	auto nodeForStmt = cfg->getNodeForStmt(unreachable);
	ASSERT_TRUE(nodeForStmt.first);

	bool hasExitSucc = false;
	for (auto i = nodeForStmt.first->succ_begin(),
			e = nodeForStmt.first->succ_end(); i != e; ++i) {
		if ((*i)->getDst() == cfg->getExitNode()) {
			hasExitSucc = true;
		}
	}
	ASSERT_TRUE(hasExitSucc) << "Unreachable statement should create edge to exit node";
}

TEST_F(RecursiveCFGBuilderTests,
CallStmtIsIncluded) {
	// void test() {
	//   foo();
	// }
	ShPtr<Variable> fooVar(Variable::create("foo", VoidType::create()));
	ShPtr<CallExpr> callExpr(CallExpr::create(fooVar));
	ShPtr<CallStmt> callStmt(CallStmt::create(callExpr));
	testFunc->setBody(callStmt);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(callStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
FunctionWithParametersHasParamsInEntryNode) {
	// int test(int x, int y) { return x; }
	ShPtr<Variable> paramX(Variable::create("x", IntType::create(32)));
	ShPtr<Variable> paramY(Variable::create("y", IntType::create(32)));
	testFunc->addParam(paramX);
	testFunc->addParam(paramY);

	ShPtr<ReturnStmt> ret(ReturnStmt::create(paramX));
	testFunc->setBody(ret);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getEntryNode());
	// The entry node should have parameter VarDef stmts.
	ASSERT_GE(cfg->getEntryNode()->getNumberOfStmts(), 2u);
}

TEST_F(RecursiveCFGBuilderTests,
UForLoopStmtCreatesLoopStructure) {
	// void test() {
	//   for ( ; ; ) { int a; break; }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<BreakStmt> breakStmt(BreakStmt::create());
	defA->setSuccessor(breakStmt);

	ShPtr<UForLoopStmt> uforStmt(UForLoopStmt::create(
		ShPtr<Expression>(),  // init
		ShPtr<Expression>(),  // cond
		ShPtr<Expression>(),  // step
		defA
	));
	testFunc->setBody(uforStmt);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(uforStmt).first);
}

TEST_F(RecursiveCFGBuilderTests,
NestedIfInWhileLoopCoversAllBranches) {
	// void test() {
	//   int a;
	//   while (a != 0) {
	//     if (a > 5) {
	//       a = 5;
	//     } else {
	//       a = a - 1;
	//     }
	//   }
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> setFive(AssignStmt::create(varA, ConstInt::create(5, 32)));
	ShPtr<AssignStmt> decA(AssignStmt::create(varA, ConstInt::create(0, 32)));

	ShPtr<Expression> ifCond(NeqOpExpr::create(varA, ConstInt::create(5, 32)));
	ShPtr<IfStmt> ifStmt(IfStmt::create(ifCond, setFive, decA));

	ShPtr<Expression> whileCond(NeqOpExpr::create(varA, ConstInt::create(0, 32)));
	ShPtr<WhileLoopStmt> whileStmt(WhileLoopStmt::create(whileCond, ifStmt));

	defA->setSuccessor(whileStmt);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);
	ASSERT_TRUE(cfg->getNodeForStmt(whileStmt).first);
	ASSERT_TRUE(cfg->getNodeForStmt(ifStmt).first);
	ASSERT_TRUE(cfg->getNodeForStmt(setFive).first);
	ASSERT_TRUE(cfg->getNodeForStmt(decA).first);
}

TEST_F(RecursiveCFGBuilderTests,
StmtNodeMappingIsCorrect) {
	// void test() {
	//   int a;
	//   int b;
	//   a = b;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, varB));
	defA->setSuccessor(defB);
	defB->setSuccessor(assign);
	testFunc->setBody(defA);

	ShPtr<CFG> cfg = buildCFG();
	ASSERT_TRUE(cfg);

	// All statements should be mapped to nodes.
	auto defANode = cfg->getNodeForStmt(defA);
	auto defBNode = cfg->getNodeForStmt(defB);
	auto assignNode = cfg->getNodeForStmt(assign);

	ASSERT_TRUE(defANode.first) << "defA should be in CFG";
	ASSERT_TRUE(defBNode.first) << "defB should be in CFG";
	ASSERT_TRUE(assignNode.first) << "assign should be in CFG";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
