/**
* @file tests/bin2llvmir/optimizations/phi_remover/phi_remover_tests.cpp
* @brief Tests for the @c PhiRemover pass.
* @copyright (c) 2026 Odin Loch trading as Imortek
*/

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/phi_remover/phi_remover.h"
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class PhiRemoverTests: public LlvmIrTests
{
	protected:
		PhiRemover pass;
};

TEST_F(PhiRemoverTests, demotePhiLoadStoreAttachesPointeeMetadata)
{
	parseInput(R"(
		define i32 @fnc(i1 %c) {
		entry:
			br i1 %c, label %a, label %b
		a:
			br label %join
		b:
			br label %join
		join:
			%p = phi i32 [ 1, %a ], [ 2, %b ]
			ret i32 %p
		}
	)");
	auto c = Config::empty(module.get());

	bool ret = pass.runOnModuleCustom(*module, &c);
	EXPECT_TRUE(ret);

	auto* i32 = Type::getInt32Ty(context);
	auto* s0 = getNthInstruction<StoreInst>();
	auto* s1 = getNthInstruction<StoreInst>(1u);
	auto* l = getNthInstruction<LoadInst>();
	ASSERT_NE(nullptr, s0);
	ASSERT_NE(nullptr, s1);
	ASSERT_NE(nullptr, l);
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(s0));
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(s1));
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(l));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
