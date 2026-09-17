/**
 * @file tests/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_tests.cpp
 * @brief Tests for the RDA-driven instruction optimizations.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * ReachingDefinitionsAnalysis does not model calls: it records a call's pointer
 * arguments as uses and nothing else, so no call kills a definition. Measured
 * directly -- `store %x, %p; call @f(%p); %a = load %p` reports exactly one
 * reaching definition, the store, even though the callee was handed the
 * pointer. An alloca is saved from that by the users check in
 * defWithUsesInTheSameBb; a machine-register global is not, and a lifted callee
 * writes the registers as a matter of course.
 */

#include <capstone/x86.h>

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class InstOptRdaTests : public LlvmIrTests {
protected:
	/// A 32-bit x86 config with eax and ebx declared as machine registers.
	/// Config is not assignable, so it lives here for the fixture's lifetime.
	std::unique_ptr<Config> config;

	Abi* setUpAbi()
	{
		auto c = config::Config::fromJsonString(R"({
			"architecture" : {
				"bitSize" : 32,
				"endian" : "little",
				"name" : "x86"
			}
		})");
		config = std::make_unique<Config>(Config::fromConfig(module.get(), c));
		auto* abi = AbiProvider::addAbi(module.get(), config.get());
		abi->addRegister(X86_REG_EAX, getGlobalByName("eax"));
		abi->addRegister(X86_REG_EBX, getGlobalByName("ebx"));
		return abi;
	}

	/// What does @c fnc return after the rewrite? Passing a `toRemove` set
	/// defers the erase, so counting loads would not show whether the value
	/// was forwarded; the return operand does.
	Value* returnedValue()
	{
		auto* f = module->getFunction("fnc");
		return cast<ReturnInst>(f->front().getTerminator())->getOperand(0);
	}
};

// Nothing in between: the store's value may be carried to the load.
TEST_F(InstOptRdaTests, aStoreIsForwardedToALoadInTheSameBlock)
{
	parseInput(R"(
		@eax = global i32 0
		@ebx = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @eax
			%a = load i32, i32* @eax
			ret i32 %a
		}
	)");
	auto* abi = setUpAbi();

	ReachingDefinitionsAnalysis RDA;
	RDA.runOnModule(*module, abi, true);
	auto* store = getNthInstruction<StoreInst>();
	std::unordered_set<Value*> toRemove;

	bool ret = inst_opt_rda::optimize(store, RDA, abi, &toRemove);

	EXPECT_TRUE(ret);
	EXPECT_EQ(module->getFunction("fnc")->getArg(0), returnedValue())
		<< "the stored value should have been carried to the load";
}

// A call in between: the callee writes the machine registers, so the value in
// eax after it is not the value stored before it.
TEST_F(InstOptRdaTests, aStoreIsNotForwardedAcrossACall)
{
	parseInput(R"(
		@eax = global i32 0
		@ebx = global i32 0
		declare void @callee()
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @eax
			call void @callee()
			%a = load i32, i32* @eax
			ret i32 %a
		}
	)");
	auto* abi = setUpAbi();

	ReachingDefinitionsAnalysis RDA;
	RDA.runOnModule(*module, abi, true);
	auto* store = getNthInstruction<StoreInst>();
	std::unordered_set<Value*> toRemove;

	inst_opt_rda::optimize(store, RDA, abi, &toRemove);

	EXPECT_TRUE(isa<LoadInst>(returnedValue())) << "the load was answered with a value the callee may have overwritten";
}

// A store to another register in between is also a write, and without an alias
// analysis this pass cannot tell that it went somewhere else.
TEST_F(InstOptRdaTests, aStoreIsNotForwardedAcrossAnotherStore)
{
	parseInput(R"(
		@eax = global i32 0
		@ebx = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @eax
			store i32 7, i32* @ebx
			%a = load i32, i32* @eax
			ret i32 %a
		}
	)");
	auto* abi = setUpAbi();

	ReachingDefinitionsAnalysis RDA;
	RDA.runOnModule(*module, abi, true);
	auto* store = getNthInstruction<StoreInst>();
	std::unordered_set<Value*> toRemove;

	inst_opt_rda::optimize(store, RDA, abi, &toRemove);

	EXPECT_TRUE(isa<LoadInst>(returnedValue()));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
