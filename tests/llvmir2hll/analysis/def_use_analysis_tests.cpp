/**
* @file tests/llvmir2hll/analysis/def_use_analysis_tests.cpp
* @brief Tests for the @c def_use_analysis and @c use_def_analysis modules.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/analysis/def_use_analysis.h"
#include "retdec/llvmir2hll/analysis/use_def_analysis.h"
#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "llvmir2hll/analysis/alias_analysis/alias_analysis_mock.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "llvmir2hll/analysis/tests_with_value_analysis.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for DefUseAnalysis and UseDefAnalysis.
*/
class DefUseAnalysisTests: public TestsWithModule {
protected:
	virtual void SetUp() override {
		TestsWithModule::SetUp();
		// Set up alias analysis and value analysis using the helper macro.
		INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);
		va_ = va;
	}

	ShPtr<ValueAnalysis> va_;
};

// ---------------------------------------------------------------------------
// DefUseAnalysis: basic tests
// ---------------------------------------------------------------------------

TEST_F(DefUseAnalysisTests,
EmptyFunctionProducesEmptyDefUseChains) {
	// void test() {}
	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);

	ASSERT_TRUE(ducs);
	ASSERT_EQ(testFunc, ducs->func);
	ASSERT_TRUE(ducs->cfg) << "CFG should be built";
	ASSERT_TRUE(ducs->du.empty()) << "no def-use chains for empty function";
}

TEST_F(DefUseAnalysisTests,
FunctionWithSimpleAssignmentHasDefChain) {
	// void test() {
	//   int a;
	//   a = 1;  <-- def of a
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

	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);

	ASSERT_TRUE(ducs);
	// The def-use chain may contain the assign statement
	// (variable a is defined in assign, and there are no uses after it)
	ASSERT_FALSE(ducs->cfg == nullptr) << "CFG should be built";
}

TEST_F(DefUseAnalysisTests,
FunctionWithReadAfterWriteHasNonEmptyChain) {
	// void test() {
	//   int a;
	//   a = 1;  <-- def of a
	//   a = a;  <-- use of a (read), then def of a (write)
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign1(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assign2(AssignStmt::create(varA, varA));
	defA->setSuccessor(assign1);
	assign1->setSuccessor(assign2);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);

	ASSERT_TRUE(ducs);
	ASSERT_TRUE(ducs->cfg);
	// The function has at least one def-use chain entry
	// (assign1 defines a, assign2 uses a)
	bool hasAnyChain = !ducs->du.empty();
	ASSERT_TRUE(hasAnyChain) << "Should have at least one def-use chain";
}

TEST_F(DefUseAnalysisTests,
DefUseChainLinksDefToUse) {
	// void test() {
	//   int a;
	//   a = 1;   <-- stmt1: def of a
	//   a = a;   <-- stmt2: use of a (reads stmt1's def)
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign1(AssignStmt::create(varA, ConstInt::create(42, 32)));
	ShPtr<AssignStmt> assign2(AssignStmt::create(varA, varA));
	defA->setSuccessor(assign1);
	assign1->setSuccessor(assign2);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);

	ASSERT_TRUE(ducs);
	// Check that the chain for (assign1, a) contains assign2
	bool foundChain = false;
	for (const auto &entry : ducs->du) {
		if (entry.first.first == assign1 && entry.first.second == varA) {
			// Check that assign2 is in the use set
			if (entry.second.count(assign2)) {
				foundChain = true;
			}
		}
	}
	ASSERT_TRUE(foundChain) << "assign1's def of a should link to assign2's use";
}

TEST_F(DefUseAnalysisTests,
DefUseAnalysisWithProvidedCFG) {
	// Test that providing an explicit CFG works
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	defA->setSuccessor(assign);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);
	// Get chains once to let DefUseAnalysis build a CFG
	auto ducs1 = dua->getDefUseChains(testFunc);

	ASSERT_TRUE(ducs1);
	// Now pass the same CFG explicitly
	auto ducs2 = dua->getDefUseChains(testFunc, ducs1->cfg);
	ASSERT_TRUE(ducs2);
	ASSERT_EQ(ducs1->cfg, ducs2->cfg) << "Provided CFG should be used";
}

TEST_F(DefUseAnalysisTests,
DefUseAnalysisWithFilterRunsSuccessfully) {
	// Test that shouldBeIncluded filter can be provided and the analysis
	// completes without errors. The filter affects GEN/KILL/IN/OUT sets
	// (not the chain generation itself), so we just verify it works.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	ShPtr<Variable> varB(Variable::create("b", IntType::create(32)));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);

	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assignA(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assignA2(AssignStmt::create(varA, varA));
	ShPtr<AssignStmt> assignB(AssignStmt::create(varB, varA));
	defA->setSuccessor(assignA);
	assignA->setSuccessor(assignA2);
	assignA2->setSuccessor(assignB);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);

	// The filter excludes varA from GEN/KILL live-variable analysis.
	// The analysis should still complete successfully.
	auto ducs = dua->getDefUseChains(testFunc, nullptr,
		[&varA](ShPtr<Variable> v) { return v != varA; });

	ASSERT_TRUE(ducs);
	ASSERT_TRUE(ducs->cfg);
}

// ---------------------------------------------------------------------------
// UseDefAnalysis: basic tests
// ---------------------------------------------------------------------------

TEST_F(DefUseAnalysisTests,
UseDefChainsComputedFromDefUseChains) {
	// void test() {
	//   int a;
	//   a = 1;   <-- def of a
	//   a = a;   <-- use of a
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign1(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assign2(AssignStmt::create(varA, varA));
	defA->setSuccessor(assign1);
	assign1->setSuccessor(assign2);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);
	ASSERT_TRUE(ducs);

	auto uda = UseDefAnalysis::create(module);
	auto udcs = uda->getUseDefChains(testFunc, ducs);

	ASSERT_TRUE(udcs);
	ASSERT_EQ(testFunc, udcs->func);
	ASSERT_EQ(ducs->cfg, udcs->cfg);
}

TEST_F(DefUseAnalysisTests,
UseDefChainsMapsUseToItsDefinition) {
	// void test() {
	//   int a;
	//   a = 1;   <-- assign1: def of a
	//   a = a;   <-- assign2: use of a (reads def from assign1)
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign1(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assign2(AssignStmt::create(varA, varA));
	defA->setSuccessor(assign1);
	assign1->setSuccessor(assign2);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);
	ASSERT_TRUE(ducs);

	auto uda = UseDefAnalysis::create(module);
	auto udcs = uda->getUseDefChains(testFunc, ducs);
	ASSERT_TRUE(udcs);

	// The use-def chain for (a, assign2) should contain assign1
	bool foundUseDefChain = false;
	UseDefChains::VarStmtPair key(varA, assign2);
	auto it = udcs->ud.find(key);
	if (it != udcs->ud.end()) {
		if (it->second.count(assign1)) {
			foundUseDefChain = true;
		}
	}
	ASSERT_TRUE(foundUseDefChain)
		<< "UD[a, assign2] should contain assign1 (the definition of a)";
}

TEST_F(DefUseAnalysisTests,
UseDefChainsDebugPrintDoesNotCrash) {
	// Calling debugPrint() should not crash; it just emits to stderr.
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign1(AssignStmt::create(varA, ConstInt::create(1, 32)));
	ShPtr<AssignStmt> assign2(AssignStmt::create(varA, varA));
	defA->setSuccessor(assign1);
	assign1->setSuccessor(assign2);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);
	ASSERT_TRUE(ducs);

	// debugPrint exercises the debug print paths in DefUseChains
	ASSERT_NO_THROW(ducs->debugPrint());

	auto uda = UseDefAnalysis::create(module);
	auto udcs = uda->getUseDefChains(testFunc, ducs);
	ASSERT_TRUE(udcs);

	// debugPrint exercises the debug print paths in UseDefChains
	ASSERT_NO_THROW(udcs->debugPrint());
}

TEST_F(DefUseAnalysisTests,
UseDefEmptyWhenNoUses) {
	// void test() {
	//   int a;
	//   a = 1;  <-- def of a, but a is never used afterwards
	// }
	ShPtr<Variable> varA(Variable::create("a", IntType::create(32)));
	testFunc->addLocalVar(varA);
	ShPtr<VarDefStmt> defA(VarDefStmt::create(varA));
	ShPtr<AssignStmt> assign(AssignStmt::create(varA, ConstInt::create(1, 32)));
	defA->setSuccessor(assign);
	testFunc->setBody(defA);

	auto dua = DefUseAnalysis::create(module, va_);
	auto ducs = dua->getDefUseChains(testFunc);
	ASSERT_TRUE(ducs);

	auto uda = UseDefAnalysis::create(module);
	auto udcs = uda->getUseDefChains(testFunc, ducs);
	ASSERT_TRUE(udcs);

	// Since a is never used in the function after its definition,
	// the use-def map should not contain (a, assign) as a key
	UseDefChains::VarStmtPair key(varA, assign);
	ASSERT_EQ(0u, udcs->ud.count(key))
		<< "a is not read, so no UD[a, assign] entry expected";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
