/**
* @file tests/llvmir2hll/optimizer/optimizers/llvm_intrinsics_optimizer_tests.cpp
* @brief Tests for the @c llvm_intrinsics_optimizer module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/empty_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/optimizer/optimizers/llvm_intrinsics_optimizer.h"
#include "retdec/llvmir2hll/support/types.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c llvm_intrinsics_optimizer module.
*/
class LLVMIntrinsicsOptimizerTests: public TestsWithModule {};

TEST_F(LLVMIntrinsicsOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<LLVMIntrinsicsOptimizer> optimizer(new LLVMIntrinsicsOptimizer(module));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

// getCalledFunc answers null whenever the callee expression is not a Variable,
// or names no function in the module. Both are the ordinary shape of a
// decompiled indirect call, and the pass read the callee's name before testing
// for null. Every other test here calls a function registered in the module,
// so none of them reaches it.
TEST_F(LLVMIntrinsicsOptimizerTests, ACallThroughAFunctionPointerDoesNotCrash)
{
	// void test() {
	//     fp();      // fp is a local, not a module function
	// }
	auto fp = Variable::create("fp", IntType::create(32));
	testFunc->setBody(CallStmt::create(CallExpr::create(fp)));

	ASSERT_NO_FATAL_FAILURE(Optimizer::optimize<LLVMIntrinsicsOptimizer>(module));
}

TEST_F(LLVMIntrinsicsOptimizerTests, ACallWhoseCalleeIsNotAVariableDoesNotCrash)
{
	// void test() {
	//     (*(void (*)())1)();
	// }
	auto target = ConstInt::create(llvm::APInt(32, 1, false), false);
	testFunc->setBody(CallStmt::create(CallExpr::create(target)));

	ASSERT_NO_FATAL_FAILURE(Optimizer::optimize<LLVMIntrinsicsOptimizer>(module));
}

TEST_F(LLVMIntrinsicsOptimizerTests,
AllStandaloneCallsToLlvmCtpopFromAllFunctionsAreRemoved) {
	// Set-up the module.
	//
	// void test() {
	//     llvm.ctpop.i8();
	//     llvm.ctpop.i8();
	//     llvm.ctpop.i8();
	// }
	//
	// void test2() {
	//     llvm.ctpop.i8();
	//     llvm.ctpop.i8();
	// }
	//
	addFuncDecl("llvm.ctpop.i8");
	addCall("test", "llvm.ctpop.i8");
	addCall("test", "llvm.ctpop.i8");
	addCall("test", "llvm.ctpop.i8");
	addFuncDef("test2");
	addCall("test2", "llvm.ctpop.i8");
	addCall("test2", "llvm.ctpop.i8");

	// Optimize the module.
	Optimizer::optimize<LLVMIntrinsicsOptimizer>(module);

	// Check that the output is correct.
	ASSERT_TRUE(isa<EmptyStmt>(testFunc->getBody())) <<
		"expected EmptyStmt, got `" << testFunc->getBody() << "`";
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got `" <<
		testFunc->getBody()->getSuccessor() << "`";
	EXPECT_FALSE(module->getFuncByName("llvm.ctpop.i8")) <<
		"there should be no declaration of `llvm.ctpop.i8`";
}

TEST_F(LLVMIntrinsicsOptimizerTests,
IfThereIsANonStandaloneCallToLlvmCtpopLeftDoNotRemoveTheDeclaration) {
	// Set-up the module.
	//
	// void test() {
	//     int x = llvm.ctpop.i8();
	//     llvm.ctpop.i8();
	// }
	//
	addFuncDecl("llvm.ctpop.i8");
	ShPtr<Variable> varX(Variable::create("x", IntType::create(32)));
	ShPtr<CallExpr> callExpr(CallExpr::create(module->getFuncByName(
		"llvm.ctpop.i8")->getAsVar(), ExprVector()));
	ShPtr<VarDefStmt> varDefX(VarDefStmt::create(varX, callExpr));
	testFunc->setBody(varDefX);
	addCall("test", "llvm.ctpop.i8");

	// Optimize the module.
	Optimizer::optimize<LLVMIntrinsicsOptimizer>(module);

	// Check that the output is correct.
	ASSERT_TRUE(isa<VarDefStmt>(testFunc->getBody())) <<
		"expected VarDefStmt, got `" << testFunc->getBody() << "`";
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got `" <<
		testFunc->getBody()->getSuccessor() << "`";
	EXPECT_TRUE(module->getFuncByName("llvm.ctpop.i8")) <<
		"there should be a declaration of `llvm.ctpop.i8`";
}

TEST_F(LLVMIntrinsicsOptimizerTests,
KeepStandaloneCallsToNonLlvmCtpopFunctions) {
	// Set-up the module.
	//
	// void test() {
	//     printf();
	// }
	//
	addFuncDecl("printf");
	addCall("test", "printf");

	// Optimize the module.
	Optimizer::optimize<LLVMIntrinsicsOptimizer>(module);

	// Check that the output is correct.
	ASSERT_TRUE(isa<CallStmt>(testFunc->getBody())) <<
		"expected CallStmt, got `" << testFunc->getBody() << "`";
	EXPECT_TRUE(!testFunc->getBody()->hasSuccessor()) <<
		"expected no successors of the statement, but got `" <<
		testFunc->getBody()->getSuccessor() << "`";
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
