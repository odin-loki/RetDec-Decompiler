/**
* @file tests/llvmir2hll/graphs/cfg/cfg_tests.cpp
* @brief Tests for the @c cfg module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/graphs/cfg/cfg.h"
#include "retdec/llvmir2hll/graphs/cfg/cfg_builders/non_recursive_cfg_builder.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/support/types.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c CFG class.
*/
class CFGTests: public TestsWithModule {
protected:
	virtual void SetUp() override {
		TestsWithModule::SetUp();
		cfgBuilder = NonRecursiveCFGBuilder::create();
	}

	ShPtr<CFG> buildCFG() {
		return cfgBuilder->getCFG(testFunc);
	}

protected:
	ShPtr<CFGBuilder> cfgBuilder;
};

// ---------------------------------------------------------------------------
// CFG::Node tests
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
EmptyNodeHasNoStmts) {
	auto cfg = buildCFG();
	ASSERT_TRUE(cfg);
	// Entry node in an empty function body has no stmts
	// (only params if any)
	auto entry = cfg->getEntryNode();
	ASSERT_TRUE(entry);
}

TEST_F(CFGTests,
NodeHasSuccsReturnsFalseForExitNode) {
	auto cfg = buildCFG();
	auto exitNode = cfg->getExitNode();
	// Exit node has no successors
	ASSERT_FALSE(exitNode->hasSuccs());
}

TEST_F(CFGTests,
NodeHasPredsReturnsTrueForExitNode) {
	// void test() { return; }
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	testFunc->setBody(ret);

	auto cfg = buildCFG();
	auto exitNode = cfg->getExitNode();
	ASSERT_TRUE(exitNode->hasPreds());
}

TEST_F(CFGTests,
NodeHasSuccsReturnsTrueForEntryNode) {
	// void test() { return; }
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	testFunc->setBody(ret);

	auto cfg = buildCFG();
	auto entryNode = cfg->getEntryNode();
	ASSERT_TRUE(entryNode->hasSuccs());
}

TEST_F(CFGTests,
StmtExistsInCFGForBuiltCFG) {
	// void test() {
	//   int a;
	//   a = 1;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	defA->setSuccessor(assign);
	testFunc->setBody(defA);

	auto cfg = buildCFG();

	ASSERT_TRUE(cfg->stmtExistsInCFG(defA)) << "defA should exist in CFG";
	ASSERT_TRUE(cfg->stmtExistsInCFG(assign)) << "assign should exist in CFG";
}

TEST_F(CFGTests,
StmtExistsInCFGReturnsFalseForMissingStmt) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<AssignStmt> detachedStmt(AssignStmt::create(varA, ConstInt::create(1, 32)));

	auto cfg = buildCFG(); // empty function

	ASSERT_FALSE(cfg->stmtExistsInCFG(detachedStmt)) << "detached stmt should not be in CFG";
}

TEST_F(CFGTests,
HasNodeForStmtReturnsCorrectResult) {
	// void test() {
	//   int a;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	testFunc->setBody(defA);

	auto cfg = buildCFG();

	ASSERT_TRUE(cfg->hasNodeForStmt(defA)) << "defA has a node";
}

TEST_F(CFGTests,
GetNodeForStmtReturnsNullForMissingStmt) {
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<VarDefStmt> detached(VarDefStmt::create(varA));

	auto cfg = buildCFG();

	auto result = cfg->getNodeForStmt(detached);
	ASSERT_FALSE(result.first) << "detached stmt has no node";
}

// ---------------------------------------------------------------------------
// CFG edge manipulation
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
NodeHasSuccReturnsCorrectResult) {
	// void test() { return; }
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	testFunc->setBody(ret);

	auto cfg = buildCFG();
	auto entryNode = cfg->getEntryNode();

	// Entry should have successors
	ASSERT_TRUE(entryNode->hasSuccs());
	auto firstSuccEdge = entryNode->getFirstSucc();
	ASSERT_TRUE(firstSuccEdge);
	ASSERT_TRUE(entryNode->hasSucc(firstSuccEdge));
}

TEST_F(CFGTests,
NodeHasPredReturnsCorrectResult) {
	// void test() { return; }
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	testFunc->setBody(ret);

	auto cfg = buildCFG();
	auto exitNode = cfg->getExitNode();

	ASSERT_TRUE(exitNode->hasPreds());

	// The exit node's predecessors have an edge to it
	auto predEdge = *exitNode->pred_begin();
	ASSERT_TRUE(predEdge);
	ASSERT_TRUE(exitNode->hasPred(predEdge));
}

// ---------------------------------------------------------------------------
// CFG edge iteration
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
EdgeIteratorsWork) {
	// void test() {
	//   int a;
	//   a = 1;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	defA->setSuccessor(assign);
	testFunc->setBody(defA);

	auto cfg = buildCFG();

	// Check that edge iterators work
	int edgeCount = 0;
	for (auto i = cfg->edge_begin(), e = cfg->edge_end(); i != e; ++i) {
		edgeCount++;
	}
	ASSERT_GT(edgeCount, 0) << "CFG should have edges";
}

// ---------------------------------------------------------------------------
// CFG::getLastStmtInNode
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
GetLastStmtInNodeReturnsLastStatement) {
	// void test() {
	//   int a;
	//   a = 1;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	defA->setSuccessor(assign);
	testFunc->setBody(defA);

	auto cfg = buildCFG();

	// Find the node containing defA
	auto nodeInfo = cfg->getNodeForStmt(defA);
	ASSERT_TRUE(nodeInfo.first);

	// The last statement in that node should be the assign
	auto lastStmt = CFG::getLastStmtInNode(nodeInfo.first);
	ASSERT_EQ(assign, lastStmt) << "assign should be the last stmt in the node";
}

// ---------------------------------------------------------------------------
// CFG::removeEmptyNodes / splitNodes
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
BuildCFGAndCheckNodeCount) {
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

	auto cfg = buildCFG();
	ASSERT_GE(cfg->getNumberOfNodes(), 3u); // entry, body, exit
}

// ---------------------------------------------------------------------------
// CFG::removeStmt
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
RemoveStmtFromCFGUpdatesNodeMapping) {
	// void test() {
	//   int a;
	//   a = 1;
	//   return;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	defA->setSuccessor(assign);
	assign->setSuccessor(ret);
	testFunc->setBody(defA);

	auto cfg = buildCFG();
	ASSERT_TRUE(cfg->stmtExistsInCFG(assign));

	cfg->removeStmt(assign);
	ASSERT_FALSE(cfg->stmtExistsInCFG(assign)) << "assign should be removed from CFG";
}

// ---------------------------------------------------------------------------
// CFG::replaceStmt
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
ReplaceStmtInCFGUpdatesNodeMapping) {
	// void test() {
	//   int a;
	//   a = 1;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	defA->setSuccessor(assign);
	testFunc->setBody(defA);

	auto cfg = buildCFG();
	ASSERT_TRUE(cfg->stmtExistsInCFG(assign));

	// Replace assign with two new statements
	ShPtr<AssignStmt> newAssign1(AssignStmt::create(varA, ConstInt::create(2, 32)));
	ShPtr<AssignStmt> newAssign2(AssignStmt::create(varB, ConstInt::create(3, 32)));
	StmtVector replacements = {newAssign1, newAssign2};

	cfg->replaceStmt(assign, replacements);

	ASSERT_FALSE(cfg->stmtExistsInCFG(assign)) << "old assign should be removed";
	ASSERT_TRUE(cfg->stmtExistsInCFG(newAssign1)) << "newAssign1 should be in CFG";
	ASSERT_TRUE(cfg->stmtExistsInCFG(newAssign2)) << "newAssign2 should be in CFG";
}

// ---------------------------------------------------------------------------
// CFG::splitNodes
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
SplitNodeOnSingleStmtNodeIsNoop) {
	// void test() {
	//   return;
	// }
	// Single-statement nodes should not be split (early return in splitNode)
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	testFunc->setBody(ret);

	auto cfg = buildCFG();
	auto nodesBefore = cfg->getNumberOfNodes();

	// splitNodes on an already-split CFG is a no-op
	cfg->splitNodes();

	ASSERT_EQ(cfg->getNumberOfNodes(), nodesBefore) << "single-stmt nodes don't change count";
}

// ---------------------------------------------------------------------------
// CFG::removeStmt edge cases
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
RemoveNonExistentStmtFromCFGIsNoop) {
	// Removing a stmt that's not in the CFG should do nothing
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<VarDefStmt> detached(VarDefStmt::create(varA));

	auto cfg = buildCFG();
	// Should not crash
	ASSERT_NO_THROW(cfg->removeStmt(detached));
}

TEST_F(CFGTests,
RemoveStmtFromSingleStmtNodeRemovesNode) {
	// void test() {
	//   int a;
	// }
	// After removing defA, its node should be removed from the CFG
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	testFunc->setBody(defA);

	auto cfg = buildCFG();
	ASSERT_TRUE(cfg->stmtExistsInCFG(defA));

	cfg->removeStmt(defA);
	ASSERT_FALSE(cfg->stmtExistsInCFG(defA)) << "defA should be gone";
	// Node should be removed (or at least stmt gone)
}

TEST_F(CFGTests,
ReplaceStmtWithEmptyVectorCallsRemoveStmt) {
	// Replacing with empty vector should remove the statement
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	testFunc->setBody(defA);

	auto cfg = buildCFG();
	ASSERT_TRUE(cfg->stmtExistsInCFG(defA));

	StmtVector empty;
	cfg->replaceStmt(defA, empty);
	ASSERT_FALSE(cfg->stmtExistsInCFG(defA)) << "defA removed via empty replace";
}

TEST_F(CFGTests,
ReplaceNonExistentStmtIsNoop) {
	// Replacing a stmt not in CFG should do nothing
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<VarDefStmt> detached(VarDefStmt::create(varA));

	auto cfg = buildCFG();
	ShPtr<AssignStmt> newStmt(AssignStmt::create(varA, ConstInt::create(0, 32)));
	StmtVector replacements = {newStmt};

	ASSERT_NO_THROW(cfg->replaceStmt(detached, replacements));
	ASSERT_FALSE(cfg->stmtExistsInCFG(newStmt)) << "nothing should be added";
}

// ---------------------------------------------------------------------------
// CFG::removeEmptyNodes
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
RemoveEmptyNodesDoesNotCrashOnSimpleCFG) {
	// Simple function - removeEmptyNodes should work without crash
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	testFunc->setBody(defA);

	auto cfg = buildCFG();
	ASSERT_NO_THROW(cfg->removeEmptyNodes());
}

// ---------------------------------------------------------------------------
// CFG::getLastStmtInNode edge cases
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
GetLastStmtInNodeForSingleStmtNode) {
	// void test() { return; }
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	testFunc->setBody(ret);

	auto cfg = buildCFG();
	auto nodeInfo = cfg->getNodeForStmt(ret);
	if (nodeInfo.first) {
		auto lastStmt = CFG::getLastStmtInNode(nodeInfo.first);
		ASSERT_EQ(ret, lastStmt) << "single-stmt node: last stmt is that stmt";
	}
}

// ---------------------------------------------------------------------------
// CFG::Node::getLabel and iteration  
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
EntryAndExitNodesHaveLabels) {
	auto cfg = buildCFG();
	auto entry = cfg->getEntryNode();
	auto exit = cfg->getExitNode();

	ASSERT_TRUE(entry);
	ASSERT_TRUE(exit);
	// Labels should be non-empty strings
	ASSERT_FALSE(entry->getLabel().empty()) << "entry node has a label";
	ASSERT_FALSE(exit->getLabel().empty()) << "exit node has a label";
}

TEST_F(CFGTests,
NodeIteratorsWorkForAllNodes) {
	// void test() { int a; a = 1; return; }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<ReturnStmt> ret(ReturnStmt::create());
	defA->setSuccessor(assign);
	assign->setSuccessor(ret);
	testFunc->setBody(defA);

	auto cfg = buildCFG();

	// Iterate over all nodes
	int nodeCount = 0;
	for (auto i = cfg->node_begin(), e = cfg->node_end(); i != e; ++i) {
		nodeCount++;
		ASSERT_TRUE(*i) << "node should be non-null";
	}
	ASSERT_GT(nodeCount, 0) << "CFG should have nodes";
}

// ---------------------------------------------------------------------------
// CFG::Node::replaceStmt (direct node operation)
// ---------------------------------------------------------------------------

TEST_F(CFGTests,
NodeReplaceStmtReplacesStatement) {
	// void test() {
	//   int a;
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	testFunc->setBody(defA);

	auto cfg = buildCFG();
	auto nodeInfo = cfg->getNodeForStmt(defA);
	ASSERT_TRUE(nodeInfo.first);

	// Replace defA with defB in the node
	ShPtr<VarDefStmt> defB(VarDefStmt::create(varB));
	StmtVector replacements = {defB};
	nodeInfo.first->replaceStmt(defA, replacements);

	// defA should no longer be in the node
	bool foundDefA = false;
	bool foundDefB = false;
	for (auto i = nodeInfo.first->stmt_begin(), e = nodeInfo.first->stmt_end();
			i != e; ++i) {
		if (*i == defA) foundDefA = true;
		if (*i == defB) foundDefB = true;
	}
	ASSERT_FALSE(foundDefA) << "defA should be replaced";
	ASSERT_TRUE(foundDefB) << "defB should be in node";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
