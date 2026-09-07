/**
 * @file tests/utils/gtest_shim_conformance_tests.cpp
 * @brief The shim in tests/standalone/gtest/ must behave like GoogleTest.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * These suites are built two ways: against real GoogleTest under CMake, and
 * against the dependency-free shim under scripts/standalone_check.sh. Every
 * other file here tests the product; this one tests that the two harnesses
 * agree, because a shim that quietly differs turns every suite it runs into
 * evidence about nothing.
 *
 * It exists because one such difference shipped. GTEST_SKIP() expands to a
 * `return`, so calling it in SetUp() left SetUp early and set the skipped flag
 * -- and the shim then ran the test body anyway, against exactly the
 * precondition the fixture had just declared unusable. It compiled, the flag
 * was set, and the report said SKIPPED, so the skip looked effective from every
 * angle except the one that mattered.
 *
 * The case that found it: tests/utils/memory_tests.cpp skips itself under a
 * sanitizer, because it sets RLIMIT_AS to physical memory and AddressSanitizer
 * needs far more address space than that. The skip was inert, the tests ran,
 * and the process died -- in whatever test happened to link next, which is why
 * it read for a long time as a flaky sanitizer.
 *
 * The same shape turned up twice more and is pinned below: a fatal ASSERT_* in
 * SetUp() did not stop the body either, and EXPECT_NEAR passed on NaN because
 * `!(diff > tol)` is true when diff is not a number.
 */

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <limits>

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {
namespace {

/// Set by the body below. Namespace scope rather than a member, because the
/// point is to observe it from a test that is not the one that would set it.
bool skippedFixtureBodyRan = false;

class GtestShimSkipInSetUp : public Test {
protected:
	void SetUp() override
	{
		GTEST_SKIP() << "deliberate: this fixture exists to be skipped";
	}
};

/// If this body runs, the harness is wrong -- but a failure here is only half
/// the evidence, because a harness that skipped correctly would not run it at
/// all and the failure would be invisible. The observation is next door.
TEST_F(GtestShimSkipInSetUp, ABodyDoesNotRunWhenSetUpSkips)
{
	skippedFixtureBodyRan = true;
	FAIL() << "the body ran after SetUp() called GTEST_SKIP()";
}

/// Declared after the fixture above so it observes the flag afterwards: both
/// GoogleTest and the shim register tests in declaration order within a
/// translation unit.
TEST(GtestShimConformance, ASkippedSetUpReallyStopsTheBody)
{
	EXPECT_FALSE(skippedFixtureBodyRan) << "GTEST_SKIP() in SetUp() must stop the test body; the shim used to "
										   "set the skipped flag and run it anyway";
}

} // anonymous namespace
} // namespace tests
} // namespace utils
} // namespace retdec

/// A fatal ASSERT_* in SetUp() must stop the test body. GoogleTest does that by
/// definition -- ASSERT_* expands to a `return` and the framework then checks
/// whether that return left a fatal failure behind -- so there is nothing to
/// pin on that side, and this is guarded to the shim, which checked only the
/// skip flag and ran the body against the state SetUp had just declared broken.
///
/// It cannot be written as an ordinary TEST_F the way the skip case above is:
/// a skipped test is reported SKIPPED and keeps the suite green, while a
/// fixture that deliberately fails its SetUp is reported FAILED and would turn
/// this suite red for doing its job. So the fixture is driven by hand inside
/// captureFailures(), which is the shim's gtest-spi equivalent.
#ifdef RETDEC_GTEST_LITE

bool assertedFixtureBodyRan = false;

class AssertingSetUpFixture : public Test {
public:
	void SetUp() override
	{
		ASSERT_EQ(1, 2) << "deliberate: this fixture exists to fail its SetUp";
	}

	static void body()
	{
		assertedFixtureBodyRan = true;
	}
};

TEST(GtestShimConformance, AFailedAssertInSetUpReallyStopsTheBody)
{
	assertedFixtureBodyRan = false;

	AssertingSetUpFixture fixture;
	const std::vector<std::string> produced =
		::testing::lite::captureFailures([&fixture]() { fixture.gtlRun(&AssertingSetUpFixture::body); });

	EXPECT_FALSE(assertedFixtureBodyRan) << "a fatal ASSERT_* in SetUp() must stop the test body; the shim used "
											"to run it, because it checked the skip flag and not the fatal one";
	EXPECT_FALSE(produced.empty()) << "the ASSERT_EQ in SetUp() should have recorded a failure";
}

#endif // RETDEC_GTEST_LITE

/// Every comparison with NaN is false, so `!(diff > tol)` -- the natural way to
/// write "within tolerance" -- is true for NaN and passes. GoogleTest fails.
/// A shim that passes here turns any test comparing a computed double into
/// evidence about nothing whenever that computation produces NaN.
TEST(GtestShimConformance, NearComparisonsFailOnNaN)
{
	const double nan = std::numeric_limits<double>::quiet_NaN();

	EXPECT_NONFATAL_FAILURE(EXPECT_NEAR(nan, 1.0, 0.5), "not a number");
	EXPECT_NONFATAL_FAILURE(EXPECT_NEAR(1.0, nan, 0.5), "not a number");

	// And the ordinary contract still holds.
	EXPECT_NEAR(1.0, 1.4, 0.5);
	EXPECT_NEAR(-1.0, -1.0, 0.0);
}
