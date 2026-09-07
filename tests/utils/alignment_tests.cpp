/**
 * @file tests/utils/alignment_tests.cpp
 * @brief Tests for the @c alignment module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <gtest/gtest.h>

#include "retdec/utils/alignment.h"

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {

class AlignmentTests : public Test {};

TEST_F(AlignmentTests, IsAlignedWorks)
{
	std::uint64_t remainder;

	EXPECT_TRUE(isAligned(0x2000, 0x1000, remainder));
	EXPECT_EQ(0, remainder);

	EXPECT_FALSE(isAligned(0x2010, 0x1000, remainder));
	EXPECT_EQ(0x10, remainder);
}

TEST_F(AlignmentTests, AlignDownWorks)
{
	EXPECT_EQ(0x2000, alignDown(0x2FFF, 0x1000));
	EXPECT_EQ(0x2000, alignDown(0x2000, 0x1000));
}

TEST_F(AlignmentTests, AlignUpWorks)
{
	EXPECT_EQ(0x3000, alignUp(0x2FFF, 0x1000));
	EXPECT_EQ(0x3000, alignUp(0x3000, 0x1000));
}

//
// Regression tests for the three defects confirmed by ESBMC against the old
// bodies of these functions. Each name records the witness it was written from.
//

/// alignUp used to be `alignDown(value + (alignment - 1), alignment)`, which
/// forms the sum before knowing it fits.
///
/// ESBMC witness: value = 18446744073709551440 (0xFFFFFFFFFFFFFF50),
/// alignment = 512. The true rounded value 2^64 is not representable, the sum
/// wrapped to 335, the mask ~511 took that to 0, and the old alignUp returned 0
/// for an input of 18446744073709551440. Every caller that uses alignUp to
/// advance a cursor moved the cursor to the start of the buffer instead.
TEST_F(AlignmentTests, AlignUpNeverReturnsLessThanItsInput)
{
	// The ESBMC witness itself.
	EXPECT_EQ(0xFFFFFFFFFFFFFF50ULL, alignUp(0xFFFFFFFFFFFFFF50ULL, 512));

	// The second witness from the same proof, at a different alignment: the old
	// body returned 0 here.
	EXPECT_EQ(0xFFFFFFFFFFFFFFFEULL, alignUp(0xFFFFFFFFFFFFFFFEULL, 4));
	EXPECT_EQ(0xFFFFFFFFFFFFFFFFULL, alignUp(0xFFFFFFFFFFFFFFFFULL, 2));

	// The whole top-of-range band for one alignment, since the wrap is a
	// property of the band and not of the single witness.
	for (std::uint64_t v = 0xFFFFFFFFFFFFF000ULL; v != 0; ++v)
	{
		EXPECT_TRUE(alignUp(v, 0x1000) >= v);
	}

	// The largest value that still rounds up without wrapping must still round.
	EXPECT_EQ(0xFFFFFFFFFFFFF000ULL, alignUp(0xFFFFFFFFFFFFEFFFULL, 0x1000));
}

/// alignUp and alignDown used to accept alignment == 0 and answer 0 for every
/// input: `alignment - 1` is UINT64_MAX, alignDown's mask ~UINT64_MAX is 0, and
/// the AND annihilates the value.
///
/// ESBMC witness: value = 15, alignment = 0 -- the old helper returned 0.
/// A PE FileAlignment or SectionAlignment of 0 is a field a file can simply
/// set, and src/unpackertool/plugins/upx/pe/pe_upx_stub.cpp:476 passes
/// getSectionAlignment() straight in.
TEST_F(AlignmentTests, ZeroAlignmentLeavesTheValueWhereItIs)
{
	EXPECT_EQ(15, alignUp(15, 0));
	EXPECT_EQ(15, alignDown(15, 0));
	EXPECT_EQ(0x2FFF, alignUp(0x2FFF, 0));
	EXPECT_EQ(0x2FFF, alignDown(0x2FFF, 0));

	std::uint64_t remainder = 0;
	// Not aligned, and the remainder is left non-zero so a caller that reads
	// only the remainder still concludes "not aligned".
	EXPECT_FALSE(isAligned(15, 0, remainder));
	EXPECT_EQ(15, remainder);
}

/// isAligned used to mask with `alignment - 1` without asking whether that is a
/// mask, and was then wrong in both directions for a non-power-of-two.
///
/// ESBMC witness: value = 128, alignment = 9223372036854775811
/// (0x8000000000000003). The mask is 0x8000000000000002, 128 AND that is 0, so
/// the old body reported the value ALIGNED with a remainder of 0 -- while
/// 128 mod 9223372036854775811 is 128. That false positive is the dangerous
/// direction: include/retdec/fileformat/file_format/pe/pe_format_parser.h:123
/// feeds this a raw PE FileAlignment and turns the answer into a
/// header-anomaly verdict, so a malformed file chose whether its own anomaly
/// was noticed.
TEST_F(AlignmentTests, IsAlignedDoesNotReportAlignedForANonPowerOfTwo)
{
	std::uint64_t remainder = 0;

	const std::uint64_t witnessAlignment = 0x8000000000000003ULL;
	EXPECT_EQ(128, 128 % witnessAlignment); // the true modulus
	EXPECT_FALSE(isAligned(128, witnessAlignment, remainder));
	EXPECT_NE(0, remainder);

	// Small non-powers of two, where the old body was wrong in the other
	// direction too: with alignment 3 the mask 2 clears bit 1 and leaves bit 0.
	EXPECT_FALSE(isAligned(4, 3, remainder));
	EXPECT_NE(0, remainder);
	EXPECT_FALSE(isAligned(9, 3, remainder));
	EXPECT_NE(0, remainder);

	// A power of two still answers exactly the modulus.
	EXPECT_TRUE(isAligned(0x3000, 0x1000, remainder));
	EXPECT_EQ(0, remainder);
	EXPECT_FALSE(isAligned(0x3001, 0x1000, remainder));
	EXPECT_EQ(1, remainder);
}

/// The one property every caller in the tree depends on, stated directly:
/// alignUp never moves a cursor backwards and alignDown never moves it
/// forwards, for any alignment at all.
TEST_F(AlignmentTests, RoundingNeverMovesACursorTheWrongWay)
{
	const std::uint64_t values[] = {
		0,
		1,
		15,
		0x1000,
		0x2FFF,
		0x7FFFFFFFFFFFFFFFULL,
		0xFFFFFFFFFFFFFF50ULL,
		0xFFFFFFFFFFFFFFFEULL,
		0xFFFFFFFFFFFFFFFFULL};
	const std::uint64_t alignments[] = {
		0, 1, 2, 3, 4, 7, 0x1000, 0x8000000000000000ULL, 0x8000000000000003ULL, 0xFFFFFFFFFFFFFFFFULL};

	for (auto v: values)
	{
		for (auto a: alignments)
		{
			EXPECT_TRUE(alignUp(v, a) >= v);
			EXPECT_TRUE(alignDown(v, a) <= v);
		}
	}
}

} // namespace tests
} // namespace utils
} // namespace retdec
