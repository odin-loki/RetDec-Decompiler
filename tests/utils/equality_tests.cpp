/**
 * @file tests/utils/equality_tests.cpp
 * @brief Tests for the @c equality module.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * areEqual<> for floating-point types had no tests at all, and its own doc
 * comment recorded one of its defects as a known limitation. It is now the
 * proved predicate in retdec/utils/float_predicate.h; these are the properties
 * that were wrong before the change, each written from what the old body did.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "retdec/utils/equality.h"

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {

class EqualityTests : public Test {};

/// `std::isinf(x) == std::isinf(y)` compares two BOOLS, so it was true whenever
/// both operands were infinite regardless of sign -- areEqual(+inf, -inf)
/// returned true. Two constants as far apart as floating point can express were
/// reported equal.
TEST_F(EqualityTests, OppositeInfinitiesAreNotEqual)
{
	const double pinf = std::numeric_limits<double>::infinity();
	const float pinff = std::numeric_limits<float>::infinity();

	EXPECT_FALSE(areEqual(pinf, -pinf));
	EXPECT_FALSE(areEqual(-pinf, pinf));
	EXPECT_FALSE(areEqual(pinff, -pinff));

	// The same infinity still compares equal to itself.
	EXPECT_TRUE(areEqual(pinf, pinf));
	EXPECT_TRUE(areEqual(-pinf, -pinf));
	EXPECT_TRUE(areEqual(pinff, pinff));

	// An infinity is not equal to any finite value, in either order.
	EXPECT_FALSE(areEqual(pinf, 1.0));
	EXPECT_FALSE(areEqual(1.0, pinf));
	EXPECT_FALSE(areEqual(-pinf, std::numeric_limits<double>::max()));
}

/// `std::abs(x - y)` is an overflow for x = DBL_MAX, y = -DBL_MAX: the
/// subtraction is infinite, and the comparison against a finite tolerance then
/// answered "not equal" by undefined means. The answer is right; the way it was
/// reached is not, and ESBMC reports it as "arithmetic overflow on
/// floating-point ieee_sub". The predicate never forms the difference now.
TEST_F(EqualityTests, TheWidestFinitePairDoesNotOverflow)
{
	const double dmax = std::numeric_limits<double>::max();
	const float fmax = std::numeric_limits<float>::max();

	EXPECT_FALSE(areEqual(dmax, -dmax));
	EXPECT_FALSE(areEqual(-dmax, dmax));
	EXPECT_FALSE(areEqual(fmax, -fmax));

	EXPECT_TRUE(areEqual(dmax, dmax));
	EXPECT_TRUE(areEqual(-dmax, -dmax));
}

/// `epsilon * std::abs(x)` scales by the FIRST operand alone, so the predicate
/// was not symmetric -- the header used to say so outright: "it is possible
/// that areEqual(x, y) returns a different value from areEqual(y, x)". A
/// comparison that depends on argument order is not an equality, and callers
/// (src/cpdetect/search.cpp:528, cpdetect.cpp:96) sort by it.
TEST_F(EqualityTests, EqualityIsSymmetric)
{
	// The asymmetric band: y within 1e-10 of x relative to |y| but not to |x|.
	// Scaling by the larger magnitude makes the two directions agree.
	const double values[] = {
		0.0,
		-0.0,
		1.0,
		-1.0,
		1e-300,
		-1e-300,
		1e300,
		-1e300,
		1.0000000000001,
		0.9999999999999,
		std::numeric_limits<double>::min(),
		std::numeric_limits<double>::denorm_min(),
		std::numeric_limits<double>::max()};

	for (auto x: values)
		for (auto y: values)
			EXPECT_EQ(areEqual(x, y), areEqual(y, x)) << "asymmetric at x=" << x << " y=" << y;
}

/// Reflexivity, including for the values where `x == y` is false or the
/// arithmetic would not have got that far.
TEST_F(EqualityTests, EqualityIsReflexive)
{
	const double nan = std::numeric_limits<double>::quiet_NaN();

	EXPECT_TRUE(areEqual(nan, nan));
	EXPECT_TRUE(areEqual(0.0, -0.0));
	EXPECT_TRUE(areEqual(std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::denorm_min()));

	// A NaN is equal to nothing else.
	EXPECT_FALSE(areEqual(nan, 0.0));
	EXPECT_FALSE(areEqual(0.0, nan));
	EXPECT_FALSE(areEqual(nan, std::numeric_limits<double>::infinity()));
}

/// The ordinary cases, so the change is not a silent loosening: values that
/// were equal before still are, and values that were not still are not.
TEST_F(EqualityTests, OrdinaryComparisonsAreUnchanged)
{
	EXPECT_TRUE(areEqual(1.0, 1.0));
	EXPECT_TRUE(areEqual(1.0, 1.0 + 1e-15));
	EXPECT_FALSE(areEqual(1.0, 1.5));
	EXPECT_FALSE(areEqual(1.0, 2.0));
	EXPECT_FALSE(areEqual(0.0, 1e-9));

	EXPECT_TRUE(areEqual(1.0f, 1.0f));
	EXPECT_FALSE(areEqual(1.0f, 1.5f));

	// Non-floating-point types still go through the plain `==` overload.
	EXPECT_TRUE(areEqual(3, 3));
	EXPECT_FALSE(areEqual(3, 4));
}

} // namespace tests
} // namespace utils
} // namespace retdec
