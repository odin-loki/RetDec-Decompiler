/**
* @file tests/llvmir2hll/llvm/llvmir2bir_converter/functions_tests.cpp
* @brief Tests for functions conversion in @c LLVMIR2BIRConverter.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <gtest/gtest.h>

#include "llvmir2hll/llvm/llvmir2bir_converter_tests/base_tests.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/llvm/llvmir2bir_converter.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/unknown_type.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/void_type.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"

using namespace ::testing;
using namespace std::string_literals;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for functions conversion in @c LLVMIR2BIRConverter.
*/
class LLVMIR2BIRConverterFunctionsTests: public LLVMIR2BIRConverterBaseTests {};

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionDeclarationIsCorrectlyAddedToModuleAsDeclaration) {
	auto module = convertLLVMIR2BIR(R"(
		declare i32 @function()
	)");

	auto f = module->getFuncByName("function");
	ASSERT_TRUE(f);
	ASSERT_TRUE(f->isDeclaration());
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionDefinitionIsCorrectlyAddedToModuleAsDefinition) {
	auto module = convertLLVMIR2BIR(R"(
		define i32 @function() {
			ret i32 0
		}
	)");

	auto f = module->getFuncByName("function");
	ASSERT_TRUE(f);
	ASSERT_TRUE(f->isDefinition());
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
AvailableExternallyLinkageFunctionIsNotAddedToModule) {
	auto module = convertLLVMIR2BIR(R"(
		define available_externally i32 @function() {
			ret i32 0
		}
	)");

	ASSERT_FALSE(module->hasFuncWithName(("function")));
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionReturnValueIsConvertedCorrectly) {
	auto module = convertLLVMIR2BIR(R"(
		declare void @function()
	)");

	auto f = module->getFuncByName("function");
	ASSERT_TRUE(f);
	ASSERT_TRUE(isa<VoidType>(f->getRetType()));
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionDefinitionParametersAreConvertedCorrectly) {
	auto module = convertLLVMIR2BIR(R"(
		define i32 @function(i32 %num, i8* %str) {
			ret i32 0
		}
	)");

	auto f = module->getFuncByName("function");
	ASSERT_TRUE(f);
	ASSERT_EQ(2, f->getNumOfParams());
	auto param1 = f->getParam(1);
	ASSERT_EQ("num"s, param1->getName());
	auto param1Type = cast<IntType>(param1->getType());
	ASSERT_TRUE(param1Type);
	ASSERT_EQ(32, param1Type->getSize());
	auto param2 = f->getParam(2);
	ASSERT_EQ("str"s, param2->getName());
	auto param2Type = cast<PointerType>(param2->getType());
	ASSERT_TRUE(param2Type);
	ASSERT_TRUE(isa<UnknownType>(param2Type->getContainedType()));
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionDeclarationParametersAreConvertedCorrectly) {
	auto module = convertLLVMIR2BIR(R"(
		declare void @function(i32, i8*)
	)");

	auto f = module->getFuncByName("function");
	ASSERT_TRUE(f);
	ASSERT_EQ(2, f->getNumOfParams());
	auto param1 = f->getParam(1);
	ASSERT_TRUE(param1->hasName());
	ASSERT_TRUE(isa<IntType>(param1->getType()));
	auto param2 = f->getParam(2);
	ASSERT_TRUE(param2->hasName());
	ASSERT_TRUE(isa<PointerType>(param2->getType()));
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionWithoutVariableArgumentsIsConvertedCorrectly) {
	auto module = convertLLVMIR2BIR(R"(
		declare i32 @func1(i32)
	)");

	auto f = module->getFuncByName("func1");
	ASSERT_TRUE(f);
	ASSERT_FALSE(f->isVarArg());
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionWithVariableArgumentsIsConvertedCorrectly) {
	auto module = convertLLVMIR2BIR(R"(
		declare i32 @func2(i32, ...)
	)");

	auto f = module->getFuncByName("func2");
	ASSERT_TRUE(f);
	ASSERT_TRUE(f->isVarArg());
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionWithInvalidIdentifierNameHasValidIdentifierNameAfterConversion) {
	auto module = convertLLVMIR2BIR(R"(
		declare i32 @$example.func1()
	)");

	auto f = *(module->func_begin());
	ASSERT_TRUE(f);
	ASSERT_EQ("_24_example_func1", f->getName());
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
FunctionParameterWithInvalidIdentifierNameHasValidIdentifierNameAfterConversion) {
	auto module = convertLLVMIR2BIR(R"(
		define void @function(i32 %$example_param1) {
			ret void
		}
	)");

	auto f = module->getFuncByName("function");
	ASSERT_TRUE(f);
	auto param = f->getParam(1);
	ASSERT_EQ("_24_example_param1", param->getName());
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
LocalVariableWithInvalidIdentifierNameHasValidIdentifierNameAfterConversion) {
	auto module = convertLLVMIR2BIR(R"(
		declare i32 @test()

		define i32 @function() {
			%$example.variable1 = call i32 @test()
			ret i32 %$example.variable1
		}
	)");

	auto f = module->getFuncByName("function");
	ASSERT_TRUE(f);
	auto varSet = f->getLocalVars();
	auto var = *(varSet.begin());
	ASSERT_EQ("_24_example_variable1", var->getName());
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
PairedIntegerLoadLanesAreLoweredToShiftsOfTheScalar) {
	// gcc -O2 bubblesort: movq/pshufd/movd reads two adjacent ints as one
	// i64; lane 1 must become the high half, not lane 0 again.
	auto m = parseLLVMIR(R"(
		define i64 @f(i64 %x) {
			%z = zext i64 %x to i128
			%v = bitcast i128 %z to <4 x i32>
			%hi = extractelement <4 x i32> %v, i64 1
			%s = shufflevector <4 x i32> %v, <4 x i32> poison, <2 x i32> <i32 1, i32 0>
			%sw = bitcast <2 x i32> %s to i64
			%h = zext i32 %hi to i64
			%r = add i64 %h, %sw
			ret i64 %r
		}
	)");
	LLVMIR2BIRConverter::lowerIntegerVectorLanes(*m);
	unsigned shr32 = 0, shl32 = 0;
	for (auto &i : llvm::instructions(*m->getFunction("f"))) {
		ASSERT_FALSE(llvm::isa<llvm::ExtractElementInst>(i));
		ASSERT_FALSE(i.getType()->isVectorTy());
		if (i.getOpcode() == llvm::Instruction::LShr) {
			auto *c = llvm::cast<llvm::ConstantInt>(i.getOperand(1));
			shr32 += c->getZExtValue() == 32;
		}
		if (i.getOpcode() == llvm::Instruction::Shl) {
			auto *c = llvm::cast<llvm::ConstantInt>(i.getOperand(1));
			shl32 += c->getZExtValue() == 32;
		}
	}
	EXPECT_EQ(2u, shr32);
	EXPECT_EQ(1u, shl32);
}

TEST_F(LLVMIR2BIRConverterFunctionsTests,
LoopCarriedScalarSseLaneIsReadFromItsOwnSlot) {
	// gcc -O2 float_mean_variance: addsd accumulates into lane 0 of an xmm
	// register that reg2mem keeps in a <2 x double> slot. The next iteration
	// must read the sum just stored, not the slot's stale base vector.
	auto m = parseLLVMIR(R"(
		define double @f(ptr %p, i1 %c) {
		entry:
			%slot = alloca <2 x double>
			store <2 x double> zeroinitializer, ptr %slot
			br label %loop
		loop:
			%v = load <2 x double>, ptr %slot
			%a = extractelement <2 x double> %v, i64 0
			%x = load double, ptr %p
			%s = fadd double %a, %x
			%n = insertelement <2 x double> %v, double %s, i64 0
			store <2 x double> %n, ptr %slot
			br i1 %c, label %loop, label %exit
		exit:
			%w = load <2 x double>, ptr %slot
			%r = extractelement <2 x double> %w, i64 0
			ret double %r
		}
	)");
	LLVMIR2BIRConverter::lowerIntegerVectorLanes(*m);
	bool storesSum = false;
	llvm::Value *sum = nullptr;
	for (auto &i : llvm::instructions(*m->getFunction("f"))) {
		ASSERT_FALSE(llvm::isa<llvm::ExtractElementInst>(i)) << i.getName().str();
		if (i.getOpcode() == llvm::Instruction::FAdd) {
			sum = &i;
			// The addend is a scalar lane load, not a lane of a vector.
			ASSERT_TRUE(llvm::isa<llvm::LoadInst>(i.getOperand(0)));
		}
		if (auto *st = llvm::dyn_cast<llvm::StoreInst>(&i)) {
			ASSERT_FALSE(st->getValueOperand()->getType()->isVectorTy());
			storesSum |= sum && st->getValueOperand() == sum;
		}
	}
	EXPECT_TRUE(storesSum);
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
