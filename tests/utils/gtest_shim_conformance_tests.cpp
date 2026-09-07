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
 */

#include <gtest/gtest.h>

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
