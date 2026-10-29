/**
 * @file tests/bin2llvmir/optimizations/stack/stack_tests.cpp
 * @brief Tests for merging pointer-walked stack locals into one frame object.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 */

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/stack/stack.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class StackTests : public LlvmIrTests {
protected:
	std::unique_ptr<Config> config;

	void run()
	{
		auto c = config::Config::fromJsonString(R"({
			"architecture" : { "bitSize" : 64, "endian" : "little", "name" : "x86" }
		})");
		config = std::make_unique<Config>(Config::fromConfig(module.get(), c));
		auto* abi = AbiProvider::addAbi(module.get(), config.get());
		StackAnalysis pass;
		pass.runOnModuleCustom(*module, config.get(), abi, nullptr);
	}

	unsigned countAllocas()
	{
		unsigned n = 0;
		for (auto& i : module->getFunction("fnc")->front())
			n += isa<AllocaInst>(i);
		return n;
	}
};

// The array's first element is written to a slot nothing loads; it is read
// only by walking from the address of the next slot.  Both must become one
// object or the write is dead to LLVM.
TEST_F(StackTests, aWriteOnlySlotBetweenTakenAddressesJoinsTheFrame)
{
	parseInput(R"(
		define i64 @fnc() {
			%"stack_var_-24" = alloca i64
			%"stack_var_-16" = alloca i64
			%"stack_var_-8" = alloca i64
			store i64 1, ptr %"stack_var_-16"
			%a = ptrtoint ptr %"stack_var_-24" to i64
			%b = ptrtoint ptr %"stack_var_-8" to i64
			%d = sub i64 %b, %a
			ret i64 %d
		}
	)");

	run();

	auto* f = module->getFunction("fnc");
	EXPECT_EQ(1u, countAllocas());
	auto* frame = dyn_cast<AllocaInst>(&f->front().front());
	ASSERT_NE(nullptr, frame);
	EXPECT_EQ("stack_frame_24", frame->getName());
	EXPECT_EQ(24u, module->getDataLayout().getTypeAllocSize(frame->getAllocatedType()));
}

// Taken addresses alone (scanf(&a, &b)) keep their own variables.
TEST_F(StackTests, takenAddressesWithoutAWriteOnlySlotStaySeparate)
{
	parseInput(R"(
		define i64 @fnc() {
			%"stack_var_-16" = alloca i64
			%"stack_var_-8" = alloca i64
			%a = ptrtoint ptr %"stack_var_-16" to i64
			%b = ptrtoint ptr %"stack_var_-8" to i64
			%d = sub i64 %b, %a
			ret i64 %d
		}
	)");

	run();

	EXPECT_EQ(2u, countAllocas());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
