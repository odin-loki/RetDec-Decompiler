/**
 * @file tests/utils/memory_tests.cpp
 * @brief Tests for the @c memory module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <gtest/gtest.h>

#include "retdec/utils/memory.h"
#include "retdec/utils/os.h"

using namespace ::testing;

/// True when this translation unit was built with a sanitizer that reserves a
/// large virtual mapping up front.
///
/// AddressSanitizer maps roughly 20 TB of shadow at startup, and its allocator
/// keeps mmap'ing as the process runs. Every test below sets RLIMIT_AS to the
/// machine's PHYSICAL memory, which is four orders of magnitude smaller, so the
/// next allocation ASan attempts fails and the process dies with
///
///   ERROR: AddressSanitizer failed to allocate 0x1f000 bytes ... (errno: 12)
///
/// The failure surfaces in whatever test runs next -- it appeared as
/// StringTests.TrimNothingToTrim, because string_tests.cpp links after this
/// file -- which is why it read as a flaky sanitizer rather than as this.
/// Measured: the same 87 MB binary runs 298 tests to a clean exit under
/// --gtest_filter='*-MemoryTests.*'.
///
/// So the whole utils suite was un-sanitizable, and the ASan/UBSan gate that
/// every fix in this tree is supposed to pass could not be run on it at all.
/// The tests are skipped rather than deleted: what they check is real on an
/// ordinary build, and it is the interaction with the sanitizer's own
/// reservation that is not.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define RETDEC_TESTS_RLIMIT_IS_UNSAFE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define RETDEC_TESTS_RLIMIT_IS_UNSAFE 1
#endif
#endif

namespace {

constexpr bool rlimitIsUnsafeHere()
{
#ifdef RETDEC_TESTS_RLIMIT_IS_UNSAFE
	return true;
#else
	return false;
#endif
}

} // anonymous namespace

/// Skip the test that follows when setting RLIMIT_AS would kill the process.
///
/// It goes at the top of each BODY. GTEST_SKIP is a `return`, and this suite
/// runs against the shim in tests/standalone/gtest/, whose SetUp cannot stop a
/// body from running -- so a skip placed in SetUp compiles, reports nothing,
/// and lets the test run anyway. That was the first attempt and it did not
/// work.
#define RETDEC_SKIP_IF_RLIMIT_UNSAFE()                                        \
	do                                                                        \
	{                                                                         \
		if (rlimitIsUnsafeHere())                                             \
		{                                                                     \
			GTEST_SKIP() << "capping RLIMIT_AS at physical memory makes the " \
							"sanitizer's own mmap fail; see the note at the " \
							"top of this file";                               \
		}                                                                     \
	}                                                                         \
	while (false)

namespace retdec {
namespace utils {
namespace tests {

/**
 * @brief Tests for the @c memory module.
 */
class MemoryTests : public Test {
protected:
	virtual void SetUp() override
	{
		// The skip is in each test body, not here: GTEST_SKIP expands to a
		// `return`, and returning early from this shim's SetUp does not stop
		// the body from running afterwards. Measured -- the first version of
		// this guard put the skip here, the macro was correctly defined, and
		// the tests ran anyway and killed the process.
		if (rlimitIsUnsafeHere()) return;
		// Several tests have side effects, so we need to store the original
		// total memory so we can restore it after each test.
		totalSystemMemory = getTotalSystemMemory();
	}

	virtual void TearDown() override
	{
		if (rlimitIsUnsafeHere()) return;
		limitSystemMemory(totalSystemMemory);
	}

private:
	/// Original total memory in the system.
	std::size_t totalSystemMemory = 0;
};

TEST_F(MemoryTests, GetTotalSystemMemoryReturnsNonZeroSize)
{
	RETDEC_SKIP_IF_RLIMIT_UNSAFE();

	auto size = getTotalSystemMemory();

	ASSERT_GT(size, 0);
}

TEST_F(MemoryTests, LimitSystemMemoryReturnsTrueWhenLimitingTotalSystemMemoryToNonZeroSize)
{
	RETDEC_SKIP_IF_RLIMIT_UNSAFE();

	auto totalSize = getTotalSystemMemory();

	// This has a side effect, but the system's memory is set back to the
	// original value in TearDown().
	ASSERT_TRUE(limitSystemMemory(totalSize)) << "failed to limit system memory to " << totalSize;
}

TEST_F(MemoryTests, LimitSystemMemoryReturnsFalseWhenLimitIsZero)
{
	RETDEC_SKIP_IF_RLIMIT_UNSAFE();

	ASSERT_FALSE(limitSystemMemory(0));
}

#ifdef OS_WINDOWS
TEST_F(MemoryTests, LimitSystemMemoryReturnsFalseOnWindowsWhenLimitIsBelowPageSize)
{
	RETDEC_SKIP_IF_RLIMIT_UNSAFE();

	// SetInformationJobObject() requires the limit to be at least page size
	// (e.g. 4 kB = 4096 bytes). If the limit is lower, it will fail.
	ASSERT_FALSE(limitSystemMemory(100 /*bytes*/));
}
#endif

TEST_F(MemoryTests, LimitSystemMemoryToHalfOfTotalSystemMemoryReturnsTrue)
{
	RETDEC_SKIP_IF_RLIMIT_UNSAFE();

	// This has a side effect, but the system's memory is set back to the
	// original value in TearDown().
	ASSERT_TRUE(limitSystemMemoryToHalfOfTotalSystemMemory());
}

} // namespace tests
} // namespace utils
} // namespace retdec
