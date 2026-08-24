/**
* @file tests/bin2llvmir/optimizations/entry_alloca/entry_alloca_tests.cpp
* @brief Tests for the @c EntryAlloca pass.
* @copyright (c) 2026 Odin Loch trading as Imortek
*/

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/entry_alloca/entry_alloca.h"
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class EntryAllocaTests: public LlvmIrTests
{
	protected:
		EntryAlloca pass;
};

TEST_F(EntryAllocaTests, pointerSelectAttachesPointeeMetadata)
{
	parseInput(R"(
		define i32* @fnc(i1 %c, i32* %x, i32* %y) {
			%s = select i1 %c, i32* %x, i32* %y
			ret i32* %s
		}
	)");

	bool ret = pass.runOnModule(*module);
	EXPECT_TRUE(ret);

	auto* sel = getNthInstruction<SelectInst>();
	ASSERT_NE(nullptr, sel);
	auto* i32 = Type::getInt32Ty(context);
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(sel));
	EXPECT_EQ(i32, llvm_utils::pointeeType(sel));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
