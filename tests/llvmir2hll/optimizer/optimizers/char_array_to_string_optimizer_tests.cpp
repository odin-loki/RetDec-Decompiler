/**
* @file tests/llvmir2hll/optimizer/optimizers/char_array_to_string_optimizer_tests.cpp
* @brief Tests for the @c char_array_to_string_optimizer module.
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/array_type.h"
#include "retdec/llvmir2hll/ir/const_array.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/const_string.h"
#include "retdec/llvmir2hll/ir/global_var_def.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/char_array_to_string_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class CharArrayToStringOptimizerTests: public TestsWithModule {};

// Helper: build an initialized ConstArray from a vector of int values (i8 type).
static ShPtr<ConstArray> makeCharArray(const std::vector<int>& vals) {
	ConstArray::ArrayValue arrVals;
	for (int v : vals) {
		llvm::APInt ap(8, static_cast<uint64_t>(v), false);
		arrVals.push_back(ConstInt::create(ap, false));
	}
	auto arrType = ArrayType::create(IntType::create(8), {vals.size()});
	return ConstArray::create(arrVals, arrType);
}

TEST_F(CharArrayToStringOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<CharArrayToStringOptimizer> optimizer(
		new CharArrayToStringOptimizer(module));

	EXPECT_TRUE(!optimizer->getId().empty());
}

TEST_F(CharArrayToStringOptimizerTests,
EmptyModuleRunsWithoutCrash) {
	Optimizer::optimize<CharArrayToStringOptimizer>(module);
	// No global vars → nothing should change.
}

TEST_F(CharArrayToStringOptimizerTests,
GlobalCharArrayWithHelloIsConvertedToString) {
	// "Hello" = {72, 101, 108, 108, 111, 0}
	auto arr = makeCharArray({72, 101, 108, 108, 111, 0});
	auto var = Variable::create("g_str", arr->getType());
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	// After optimization the initializer should be a ConstString.
	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto init = (*it)->getInitializer();
	EXPECT_TRUE(isa<ConstString>(init)) <<
		"expected ConstString, got " << init;
}

// A ConstString is bytes. An int32_t[] table whose elements all happen to be
// printable ASCII is not a string: promoting it prints
// `int32_t g[6] = "Hello";`, which is not valid C, and describes six bytes
// where the binary holds twenty-four. Every existing test in this file builds
// its array from IntType::create(8), so the missing width check was invisible.
static ShPtr<ConstArray> makeWideArray(const std::vector<int>& vals, unsigned width)
{
	ConstArray::ArrayValue arrVals;
	for (int v: vals)
	{
		llvm::APInt ap(width, static_cast<uint64_t>(v), false);
		arrVals.push_back(ConstInt::create(ap, false));
	}
	auto arrType = ArrayType::create(IntType::create(width), {vals.size()});
	return ConstArray::create(arrVals, arrType);
}

TEST_F(CharArrayToStringOptimizerTests, GlobalArrayOfThirtyTwoBitElementsIsNotConvertedToAString)
{
	auto arr = makeWideArray({72, 101, 108, 108, 111, 0}, 32);
	auto var = Variable::create("g_wide", arr->getType());
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	EXPECT_FALSE(isa<ConstString>((*it)->getInitializer())) << "an int32_t[] table was promoted to a byte string";
}

TEST_F(CharArrayToStringOptimizerTests, GlobalArrayOfSixteenBitElementsIsNotConvertedToAString)
{
	auto arr = makeWideArray({72, 105, 0}, 16);
	auto var = Variable::create("g_wide16", arr->getType());
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	EXPECT_FALSE(isa<ConstString>((*it)->getInitializer()));
}

TEST_F(CharArrayToStringOptimizerTests,
GlobalCharArrayWithoutNullTerminatorIsNotConverted) {
	// "Hi" without null terminator → should not be converted.
	auto arr = makeCharArray({72, 105});
	auto var = Variable::create("g_no_null", arr->getType());
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto init = (*it)->getInitializer();
	EXPECT_FALSE(isa<ConstString>(init)) <<
		"should not have converted array without null terminator";
}

TEST_F(CharArrayToStringOptimizerTests,
ArrayWithMostlyNonPrintableCharsIsNotConverted) {
	// {1, 2, 3, 0} — 3 non-printable + null → fails 50% printable check.
	auto arr = makeCharArray({1, 2, 3, 0});
	auto var = Variable::create("g_binary", arr->getType());
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto init = (*it)->getInitializer();
	EXPECT_FALSE(isa<ConstString>(init));
}

TEST_F(CharArrayToStringOptimizerTests,
SingleNullByteArrayIsNotConverted) {
	// Array of length 1 ({0}) → n < 2, rejected.
	auto arr = makeCharArray({0});
	auto var = Variable::create("g_single", arr->getType());
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto init = (*it)->getInitializer();
	EXPECT_FALSE(isa<ConstString>(init));
}

TEST_F(CharArrayToStringOptimizerTests,
UninitializedArrayIsNotConverted) {
	auto arrType = ArrayType::create(IntType::create(8), {5});
	auto arr = ConstArray::createUninitialized(arrType);
	auto var = Variable::create("g_uninit", arrType);
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto init = (*it)->getInitializer();
	EXPECT_FALSE(isa<ConstString>(init));
}

TEST_F(CharArrayToStringOptimizerTests,
NullInitializerIsNotConverted) {
	auto var = Variable::create("g_null", IntType::create(32));
	module->addGlobalVar(var, nullptr);

	// Should not crash when initializer is null.
	Optimizer::optimize<CharArrayToStringOptimizer>(module);
}

TEST_F(CharArrayToStringOptimizerTests,
ExtendedASCIIValueAbove127IsNotConverted) {
	// {128, 0} — value > 127 → rejected.
	ConstArray::ArrayValue vals;
	vals.push_back(ConstInt::create(llvm::APInt(8, 128, false), false));
	vals.push_back(ConstInt::create(llvm::APInt(8, 0, false), false));
	auto arrType = ArrayType::create(IntType::create(8), {2});
	auto arr = ConstArray::create(vals, arrType);
	auto var = Variable::create("g_ext", arrType);
	module->addGlobalVar(var, arr);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto init = (*it)->getInitializer();
	EXPECT_FALSE(isa<ConstString>(init));
}

TEST_F(CharArrayToStringOptimizerTests,
MultipleGlobalVarsConvertsOnlyEligibleOnes) {
	// g_hello = "Hi\0" → should be converted.
	auto arr1 = makeCharArray({72, 105, 0});
	auto var1 = Variable::create("g_hello", arr1->getType());
	module->addGlobalVar(var1, arr1);

	// g_binary = {1, 2, 0} → should NOT be converted (non-printable).
	auto arr2 = makeCharArray({1, 2, 0});
	auto var2 = Variable::create("g_binary2", arr2->getType());
	module->addGlobalVar(var2, arr2);

	Optimizer::optimize<CharArrayToStringOptimizer>(module);

	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	// First var (g_hello) should be converted.
	EXPECT_TRUE(isa<ConstString>((*it)->getInitializer()));
	++it;
	ASSERT_NE(it, module->global_var_end());
	// Second var (g_binary2) should remain an array.
	EXPECT_FALSE(isa<ConstString>((*it)->getInitializer()));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
