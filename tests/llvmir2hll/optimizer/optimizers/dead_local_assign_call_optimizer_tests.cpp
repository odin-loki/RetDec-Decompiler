/**
* @file tests/llvmir2hll/optimizer/optimizers/dead_local_assign_call_optimizer_tests.cpp
* @brief Tests for the @c dead_local_assign_call_optimizer module.
* @copyright (c) 2024, MIT license
*
* Verifies that when a local variable is assigned the result of a function call
* and is never read afterwards, the assignment is demoted to a pure call
* statement (preserving side effects while discarding the unused return value).
*/

#include <gtest/gtest.h>

#include "llvmir2hll/analysis/tests_with_value_analysis.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/function_builder.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/void_type.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/dead_local_assign_call_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c dead_local_assign_call_optimizer module.
*/
class DeadLocalAssignCallOptimizerTests: public TestsWithModule {};

TEST_F(DeadLocalAssignCallOptimizerTests,
OptimizerHasNonEmptyID) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	auto opt = std::make_shared<DeadLocalAssignCallOptimizer>(module, va);
	EXPECT_FALSE(opt->getId().empty());
}

TEST_F(DeadLocalAssignCallOptimizerTests,
InEmptyBodyThereIsNothingToOptimize) {
	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<DeadLocalAssignCallOptimizer>(module, va);

	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody()))
		<< "expected EmptyStmt, got " << testFunc->getBody();
	EXPECT_FALSE(testFunc->getBody()->hasSuccessor());
}

TEST_F(DeadLocalAssignCallOptimizerTests,
AssignCallResultNeverReadDemotedToCallStmt) {
	// void test() {
	//     int32 a = foo();   ← a is never read
	// }
	// →
	// void test() {
	//     foo();
	// }
	auto fooFunc = addFuncDecl("foo");
	auto callExpr = CallExpr::create(fooFunc->getAsVar());

	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);
	auto varDef = VarDefStmt::create(varA, callExpr);
	testFunc->setBody(varDef);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<DeadLocalAssignCallOptimizer>(module, va);

	// The body should now start with a CallStmt, not VarDefStmt.
	ASSERT_TRUE(isa<CallStmt>(testFunc->getBody()))
		<< "expected CallStmt after demotion, got " << testFunc->getBody();
	auto callStmt = cast<CallStmt>(testFunc->getBody());
	EXPECT_EQ(callExpr, callStmt->getCall());
}

TEST_F(DeadLocalAssignCallOptimizerTests,
AssignCallResultThenReadNotOptimized) {
	// void test() {
	//     int32 a = foo();
	//     return a;           ← a IS read — must not be removed
	// }
	auto fooFunc = addFuncDecl("foo");
	auto callExpr = CallExpr::create(fooFunc->getAsVar());

	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);
	auto retStmt = ReturnStmt::create(varA);
	auto varDef = VarDefStmt::create(varA, callExpr, retStmt);
	testFunc->setBody(varDef);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<DeadLocalAssignCallOptimizer>(module, va);

	// VarDefStmt must remain because a is used.
	ASSERT_TRUE(isa<VarDefStmt>(testFunc->getBody()))
		<< "VarDef should remain when variable is read";
}

TEST_F(DeadLocalAssignCallOptimizerTests,
NonCallRhsNotOptimized) {
	// void test() {
	//     int32 a = 42;   ← not a call — should not be demoted
	// }
	auto varA = Variable::create("a", IntType::create(32));
	testFunc->addLocalVar(varA);
	auto c42 = ConstInt::create(llvm::APInt(32, 42));
	auto varDef = VarDefStmt::create(varA, c42);
	testFunc->setBody(varDef);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<DeadLocalAssignCallOptimizer>(module, va);

	// VarDef must remain (no call on RHS).
	ASSERT_TRUE(isa<VarDefStmt>(testFunc->getBody()))
		<< "VarDef with non-call RHS should not be demoted";
}

TEST_F(DeadLocalAssignCallOptimizerTests,
AssignCallResultToGlobalNotOptimized) {
	// Global variables should not be demoted (they may be read externally).
	auto fooFunc = addFuncDecl("foo");
	auto callExpr = CallExpr::create(fooFunc->getAsVar());

	auto varG = Variable::create("g", IntType::create(32));
	module->addGlobalVar(varG);
	auto assign = AssignStmt::create(varG, callExpr);
	testFunc->setBody(assign);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<DeadLocalAssignCallOptimizer>(module, va);

	// Global assign must remain.
	ASSERT_TRUE(isa<AssignStmt>(testFunc->getBody()))
		<< "global variable assignment should not be removed";
}

TEST_F(DeadLocalAssignCallOptimizerTests,
MultipleCallResultsUnusedAllDemoted) {
	// void test() {
	//     int32 a = foo();   ← unused
	//     int32 b = bar();   ← unused
	// }
	// →
	// void test() {
	//     foo();
	//     bar();
	// }
	auto fooFunc = addFuncDecl("foo");
	auto barFunc = addFuncDecl("bar");
	auto callFoo = CallExpr::create(fooFunc->getAsVar());
	auto callBar = CallExpr::create(barFunc->getAsVar());

	auto varA = Variable::create("a", IntType::create(32));
	auto varB = Variable::create("b", IntType::create(32));
	testFunc->addLocalVar(varA);
	testFunc->addLocalVar(varB);
	auto defB = VarDefStmt::create(varB, callBar);
	auto defA = VarDefStmt::create(varA, callFoo, defB);
	testFunc->setBody(defA);

	INSTANTIATE_ALIAS_ANALYSIS_AND_VALUE_ANALYSIS(module);

	Optimizer::optimize<DeadLocalAssignCallOptimizer>(module, va);

	EXPECT_TRUE(isa<CallStmt>(testFunc->getBody()))
		<< "first stmt should be CallStmt after demoting a";
	if (testFunc->getBody()->hasSuccessor()) {
		EXPECT_TRUE(isa<CallStmt>(testFunc->getBody()->getSuccessor()))
			<< "second stmt should be CallStmt after demoting b";
	}
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
