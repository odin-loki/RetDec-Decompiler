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
		define i32 @fnc(i1 %c) {
			%x = alloca i32
			%y = alloca i32
			%s = select i1 %c, ptr %x, ptr %y
			%v = load i32, ptr %s
			ret i32 %v
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

TEST_F(EntryAllocaTests, pointerBitCastAttachesPointeeMetadata)
{
	parseInput(R"(
		define i32 @fnc() {
			%a = alloca i32
			%q = bitcast ptr %a to ptr
			%v = load i32, ptr %q
			ret i32 %v
		}
	)");

	bool ret = pass.runOnModule(*module);
	EXPECT_TRUE(ret);

	auto* bc = getNthInstruction<BitCastInst>();
	ASSERT_NE(nullptr, bc);
	auto* i32 = Type::getInt32Ty(context);
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(bc));
	EXPECT_EQ(i32, llvm_utils::pointeeType(bc));
}

TEST_F(EntryAllocaTests, intToPtrAttachesPointeeMetadata)
{
	parseInput(R"(
		define i32 @fnc(i32 %a) {
			%p = inttoptr i32 %a to ptr
			%v = load i32, ptr %p
			ret i32 %v
		}
	)");

	bool ret = pass.runOnModule(*module);
	EXPECT_TRUE(ret);

	auto* i2p = getNthInstruction<IntToPtrInst>();
	ASSERT_NE(nullptr, i2p);
	auto* i32 = Type::getInt32Ty(context);
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(i2p));
	EXPECT_EQ(i32, llvm_utils::pointeeType(i2p));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
