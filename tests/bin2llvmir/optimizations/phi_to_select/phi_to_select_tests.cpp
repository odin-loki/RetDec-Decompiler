/**
* @file tests/bin2llvmir/optimizations/phi_to_select/phi_to_select_tests.cpp
* @brief Tests for the @c PhiToSelect pass.
* @copyright (c) 2026 Odin Loch trading as Imortek
*/

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/phi_to_select/phi_to_select.h"
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class PhiToSelectTests: public LlvmIrTests
{
	protected:
		PhiToSelect pass;
};

TEST_F(PhiToSelectTests, pointerSelectAttachesPointeeMetadata)
{
	parseInput(R"(
		define i32* @fnc(i1 %c, i32* %x, i32* %y) {
		entry:
			br i1 %c, label %a, label %b
		a:
			br label %join
		b:
			br label %join
		join:
			%p = phi i32* [ %x, %a ], [ %y, %b ]
			ret i32* %p
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
