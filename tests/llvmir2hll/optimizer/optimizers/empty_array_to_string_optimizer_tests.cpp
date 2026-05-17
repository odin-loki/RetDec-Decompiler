/**
* @file tests/llvmir2hll/optimizer/optimizers/empty_array_to_string_optimizer_tests.cpp
* @brief Tests for the @c empty_array_to_string_optimizer module.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/ir/array_type.h"
#include "retdec/llvmir2hll/ir/const_array.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/const_string.h"
#include "retdec/llvmir2hll/ir/global_var_def.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "llvmir2hll/ir/tests_with_module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/empty_array_to_string_optimizer.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

class EmptyArrayToStringOptimizerTests: public TestsWithModule {
protected:
	/// Create an empty ConstArray (uninitialized with empty dimensions).
	/// isEmpty() returns true for such arrays.
	ShPtr<ConstArray> makeEmptyArray() {
		// Empty dimensions → hasEmptyDimensions() → isEmpty() returns true.
		auto innerType = ArrayType::create(IntType::create(8), {});
		return ConstArray::createUninitialized(innerType);
	}
};

TEST_F(EmptyArrayToStringOptimizerTests,
OptimizerHasNonEmptyID) {
	ShPtr<EmptyArrayToStringOptimizer> optimizer(
		new EmptyArrayToStringOptimizer(module));

	EXPECT_TRUE(!optimizer->getId().empty()) <<
		"the optimizer should have a non-empty ID";
}

TEST_F(EmptyArrayToStringOptimizerTests,
EmptyModuleRunsWithoutCrash) {
	// No global vars → doOptimization does nothing.
	ASSERT_NO_THROW(Optimizer::optimize<EmptyArrayToStringOptimizer>(module));
}

TEST_F(EmptyArrayToStringOptimizerTests,
GlobalVarWithNullInitializerIsSkipped) {
	auto var = Variable::create("g", IntType::create(32));
	module->addGlobalVar(var, nullptr);

	ASSERT_NO_THROW(Optimizer::optimize<EmptyArrayToStringOptimizer>(module));
}

TEST_F(EmptyArrayToStringOptimizerTests,
GlobalVarWithNonArrayInitializerIsSkipped) {
	auto var = Variable::create("g", IntType::create(32));
	module->addGlobalVar(var, ConstInt::create(0, 32));

	ASSERT_NO_THROW(Optimizer::optimize<EmptyArrayToStringOptimizer>(module));
}

TEST_F(EmptyArrayToStringOptimizerTests,
UninitializedArrayIsSkipped) {
	auto arrType = ArrayType::create(IntType::create(8), {3});
	auto uninit = ConstArray::createUninitialized(arrType);
	auto var = Variable::create("g", arrType);
	module->addGlobalVar(var, uninit);

	ASSERT_NO_THROW(Optimizer::optimize<EmptyArrayToStringOptimizer>(module));
	// Should remain uninitialized (unchanged).
	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto arr = cast<ConstArray>((*it)->getInitializer());
	ASSERT_NE(nullptr, arr);
	EXPECT_FALSE(arr->isInitialized());
}

TEST_F(EmptyArrayToStringOptimizerTests,
ArrayWithOnlyConstIntIsNotArrayOfStrings) {
	// Array {1, 2, 3} — ConstInts are not strings → isArrayOfStrings returns false.
	ConstArray::ArrayValue vals;
	vals.push_back(ConstInt::create(1, 8));
	vals.push_back(ConstInt::create(2, 8));
	auto arrType = ArrayType::create(IntType::create(8), {2});
	auto arr = ConstArray::create(vals, arrType);
	auto var = Variable::create("g", arrType);
	module->addGlobalVar(var, arr);

	ASSERT_NO_THROW(Optimizer::optimize<EmptyArrayToStringOptimizer>(module));
	// Array should still have ConstInt elements (not converted).
	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto result = cast<ConstArray>((*it)->getInitializer());
	ASSERT_NE(nullptr, result);
	// First element should still be ConstInt.
	EXPECT_TRUE(isa<ConstInt>(*result->init_begin()));
}

TEST_F(EmptyArrayToStringOptimizerTests,
ArrayWithStringAndEmptySubarrayIsOptimized) {
	// Outer array: {"hello", {}, "world", {}}
	// The empty arrays {} should be replaced by ConstString("").
	auto emptyArr1 = makeEmptyArray();
	auto emptyArr2 = makeEmptyArray();
	auto strHello = ConstString::create("hello");
	auto strWorld = ConstString::create("world");

	// Use a type for the outer array.
	auto outerArrType = ArrayType::create(IntType::create(8), {4});
	ConstArray::ArrayValue vals;
	vals.push_back(strHello);
	vals.push_back(emptyArr1);
	vals.push_back(strWorld);
	vals.push_back(emptyArr2);
	auto outerArr = ConstArray::create(vals, outerArrType);

	auto var = Variable::create("g", outerArrType);
	module->addGlobalVar(var, outerArr);

	Optimizer::optimize<EmptyArrayToStringOptimizer>(module);

	// After optimization, the empty arrays should be replaced by ConstString("").
	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto result = cast<ConstArray>((*it)->getInitializer());
	ASSERT_NE(nullptr, result);

	// Count ConstString elements (should be all 4 now).
	int stringCount = 0;
	for (auto jt = result->init_begin(); jt != result->init_end(); ++jt) {
		if (isa<ConstString>(*jt)) {
			++stringCount;
		}
	}
	EXPECT_EQ(4, stringCount) <<
		"expected all 4 elements to be ConstString after optimization";
}

TEST_F(EmptyArrayToStringOptimizerTests,
ArrayWithOnlyConstStringsIsNotModified) {
	// Outer array: {"hello", "world"} — no empty arrays to replace.
	auto outerArrType = ArrayType::create(IntType::create(8), {2});
	ConstArray::ArrayValue vals;
	vals.push_back(ConstString::create("hello"));
	vals.push_back(ConstString::create("world"));
	auto outerArr = ConstArray::create(vals, outerArrType);

	auto var = Variable::create("g", outerArrType);
	module->addGlobalVar(var, outerArr);

	Optimizer::optimize<EmptyArrayToStringOptimizer>(module);

	// Should still have 2 ConstString elements.
	auto it = module->global_var_begin();
	ASSERT_NE(it, module->global_var_end());
	auto result = cast<ConstArray>((*it)->getInitializer());
	ASSERT_NE(nullptr, result);
	int stringCount = 0;
	for (auto jt = result->init_begin(); jt != result->init_end(); ++jt) {
		if (isa<ConstString>(*jt)) {
			++stringCount;
		}
	}
	EXPECT_EQ(2, stringCount);
}

TEST_F(EmptyArrayToStringOptimizerTests,
ArrayMixedWithNonEmptySubarrayIsNotOptimized) {
	// Outer array contains a non-empty ConstArray → isArrayOfStrings returns false.
	ConstArray::ArrayValue innerVals;
	innerVals.push_back(ConstInt::create(1, 8));
	auto innerArrType = ArrayType::create(IntType::create(8), {1});
	auto nonEmptyArr = ConstArray::create(innerVals, innerArrType);

	auto outerArrType = ArrayType::create(IntType::create(8), {2});
	ConstArray::ArrayValue vals;
	vals.push_back(ConstString::create("hello"));
	vals.push_back(nonEmptyArr);
	auto outerArr = ConstArray::create(vals, outerArrType);

	auto var = Variable::create("g", outerArrType);
	module->addGlobalVar(var, outerArr);

	ASSERT_NO_THROW(Optimizer::optimize<EmptyArrayToStringOptimizer>(module));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
