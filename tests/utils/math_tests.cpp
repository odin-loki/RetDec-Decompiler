/**
 * @file tests/utils/math_tests.cpp
 * @brief Tests for the @c math module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "retdec/utils/math.h"

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {

/**
 * @brief Tests for the @c math module.
 */
class MathTests : public Test {};

//
// countBits()
//

TEST_F(MathTests, countBitsCountsOK)
{
	EXPECT_EQ(0, countBits(0));
	EXPECT_EQ(1, countBits(1));
	EXPECT_EQ(1, countBits(2));
	EXPECT_EQ(2, countBits(3));
	EXPECT_EQ(2, countBits(10));
	EXPECT_EQ(6, countBits(123));
	EXPECT_EQ(6, countBits(123456));
	EXPECT_EQ(16, countBits(123456789));
	EXPECT_EQ(23, countBits(1234567890123));
}

//
// bitSizeOfNumber()
//

TEST_F(MathTests, bitSizeOfNumberCountsOK)
{
	EXPECT_EQ(1, bitSizeOfNumber(0));
	EXPECT_EQ(1, bitSizeOfNumber(1));
	EXPECT_EQ(2, bitSizeOfNumber(2));
	EXPECT_EQ(3, bitSizeOfNumber(4));
	EXPECT_EQ(4, bitSizeOfNumber(8));
}

//
// isPowerOfTwo() / isPowerOfTwoOrZero()
//
// The body used to be `number && !(number & (number - 1))` with no constraint
// on N, so it instantiated at signed types. At INT64_MIN `number - 1` is signed
// integer overflow -- undefined behaviour, not a wrong answer. ESBMC reports
// "arithmetic overflow on sub" with the witness
// n = -9223372036854775807 - 1 (0x8000000000000000) and marks the rest of the
// expression NOT CHECKED, because the UB is reached first.
//
// The value assertions below pin the answer; the UB itself is what
// -fsanitize=undefined -fno-sanitize-recover=undefined catches, and it aborts
// on the INT64_MIN case with the old body.

TEST_F(MathTests, IsPowerOfTwoAtInt64MinIsFalseAndNotUndefined)
{
	// The bit pattern 0x8000000000000000 has exactly one bit set, so a caller
	// reading it as unsigned would say "power of two". Read as int64_t it is
	// negative, and a negative alignment is not a power of two under any
	// reading -- align::isPowerOfTwoSigned refuses it before any arithmetic.
	EXPECT_FALSE(isPowerOfTwo(std::numeric_limits<std::int64_t>::min()));
	EXPECT_FALSE(isPowerOfTwoOrZero(std::numeric_limits<std::int64_t>::min()));
}

TEST_F(MathTests, IsPowerOfTwoRefusesEveryNegativeValue)
{
	// A signed minimum is what discriminates here, not the small negatives.
	// `number && !(number & (number - 1))` already answers false for -1, -2
	// and -8: after promotion those have more than one bit set, so the AND is
	// non-zero -- which means those three assertions pass against the old body
	// and prove nothing on their own. A minimum that is a minimum of the
	// PROMOTED type is the exception: its pattern has exactly one bit set and
	// `number - 1` overflows to the complementary pattern, so the AND is zero
	// and the answer comes back true. Measured against the old body at -O1:
	// isPowerOfTwo(INT32_MIN) and isPowerOfTwo(INT64_MIN) both answered TRUE.
	//
	// INT16_MIN and INT8_MIN do not discriminate -- they promote to int, where
	// the subtraction is exact and the AND is non-zero -- and they are here to
	// say the answer is the same at every width, not to catch the regression.
	EXPECT_FALSE(isPowerOfTwo(std::numeric_limits<std::int32_t>::min()));
	EXPECT_FALSE(isPowerOfTwoOrZero(std::numeric_limits<std::int32_t>::min()));
	EXPECT_FALSE(isPowerOfTwo(std::numeric_limits<std::int16_t>::min()));
	EXPECT_FALSE(isPowerOfTwo(std::numeric_limits<std::int8_t>::min()));

	EXPECT_FALSE(isPowerOfTwo(std::int64_t(-1)));
	EXPECT_FALSE(isPowerOfTwo(std::int64_t(-2)));
	EXPECT_FALSE(isPowerOfTwo(std::int32_t(-8)));
	EXPECT_FALSE(isPowerOfTwoOrZero(std::int32_t(-8)));
	// -1 is all-ones, the value most likely to arrive from a truncated field.
	EXPECT_FALSE(isPowerOfTwoOrZero(std::int64_t(-1)));
}

TEST_F(MathTests, IsPowerOfTwoStillAnswersTheOrdinaryCases)
{
	EXPECT_FALSE(isPowerOfTwo(0u));
	EXPECT_TRUE(isPowerOfTwo(1u));
	EXPECT_TRUE(isPowerOfTwo(2u));
	EXPECT_FALSE(isPowerOfTwo(3u));
	EXPECT_TRUE(isPowerOfTwo(std::uint64_t(1) << 63));
	EXPECT_FALSE(isPowerOfTwo(std::numeric_limits<std::uint64_t>::max()));

	EXPECT_TRUE(isPowerOfTwoOrZero(0u));
	EXPECT_TRUE(isPowerOfTwoOrZero(4u));
	EXPECT_FALSE(isPowerOfTwoOrZero(6u));

	// Positive signed values answer the same as their unsigned counterparts.
	EXPECT_TRUE(isPowerOfTwo(std::int64_t(1) << 62));
	EXPECT_FALSE(isPowerOfTwo(std::int64_t(6)));
	EXPECT_TRUE(isPowerOfTwoOrZero(std::int64_t(0)));
}

} // namespace tests
} // namespace utils
} // namespace retdec
