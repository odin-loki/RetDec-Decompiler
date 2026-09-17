/**
 * @file tests/bin2llvmir/optimizations/strength_reduction/strength_reduction_tests.cpp
 * @brief Tests for the @c StrengthReduction pass.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * The pass is in the shipped pipeline (retdec-strength-reduction, see
 * src/retdec-decompiler/decompiler-config.json) and had no tests at all.
 */

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/strength_reduction/strength_reduction.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class StrengthReductionTests : public LlvmIrTests {
protected:
	StrengthReduction pass;
};

//
// lshr (shl x, N), N
//
// Shifting left by N drops the top N bits and shifting back right refills them
// with zeros, so what survives is the LOW w - N bits. The mask used to be the
// exact complement of that.
//

TEST_F(StrengthReductionTests, shlThenLshrKeepsTheLowBits)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%a = shl i32 %x, 4
			%b = lshr i32 %a, 4
			ret i32 %b
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		define i32 @fnc(i32 %x) {
			%sr_mask = and i32 %x, 268435455
			ret i32 %sr_mask
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

// 3 is not a power of two. The reduction used to gate on isPow2Const of the
// SHIFT AMOUNT, so it only ever fired for amounts 1, 2, 4, 8 and 16.
TEST_F(StrengthReductionTests, shlThenLshrByANonPowerOfTwoAmount)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%a = shl i32 %x, 3
			%b = lshr i32 %a, 3
			ret i32 %b
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		define i32 @fnc(i32 %x) {
			%sr_mask = and i32 %x, 536870911
			ret i32 %sr_mask
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(StrengthReductionTests, shlThenLshrByDifferentAmountsIsNotAMask)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%a = shl i32 %x, 4
			%b = lshr i32 %a, 8
			ret i32 %b
		}
	)");

	bool ret = pass.runOnModule(*module);

	EXPECT_FALSE(ret);
}

//
// mul / udiv / urem by a power of two
//

TEST_F(StrengthReductionTests, mulByPowerOfTwoBecomesAShift)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%r = mul i32 %x, 8
			ret i32 %r
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		define i32 @fnc(i32 %x) {
			%sr_shl = shl i32 %x, 3
			ret i32 %sr_shl
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(StrengthReductionTests, uremByPowerOfTwoBecomesAMask)
{
	parseInput(R"(
		define i32 @fnc(i32 %x) {
			%r = urem i32 %x, 16
			ret i32 %r
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		define i32 @fnc(i32 %x) {
			%sr_and = and i32 %x, 15
			ret i32 %sr_and
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

// isPow2Const used to call APInt::getZExtValue() with no width guard, which
// asserts above 64 active bits. The MIPS DSP flag registers are i128, so this
// is not a hypothetical width.
TEST_F(StrengthReductionTests, aConstantWiderThanSixtyFourBitsDoesNotAbort)
{
	parseInput(R"(
		define i128 @fnc(i128 %x) {
			%r = mul i128 %x, 18446744073709551616
			ret i128 %r
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		define i128 @fnc(i128 %x) {
			%sr_shl = shl i128 %x, 64
			ret i128 %sr_shl
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
