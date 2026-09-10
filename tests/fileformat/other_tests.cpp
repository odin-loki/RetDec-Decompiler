/**
 * @file tests/fileformat/other_tests.cpp
 * @brief getRealSizeInRegion's clamp, against a requested size that wraps.
 *
 * This is the clamp every byte-range read in retdec::fileformat goes through:
 * SecSeg::getBytes and SecSeg::getString at src/fileformat/types/sec_seg/
 * sec_seg.cpp:235 and :402, Resource::getBytes and Resource::getString at
 * src/fileformat/types/resource_table/resource.cpp:93 and :218. Each of them
 * takes the result and indexes a std::vector with it.
 *
 * The offsets and sizes reaching it come from file headers, which are
 * attacker-controlled: a PE resource entry and an ELF section header both
 * carry a 64-bit size that nothing upstream bounds.
 */

#include "retdec/fileformat/utils/other.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>

using retdec::fileformat::getRealSizeInRegion;

namespace {

constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();

/// The case FileFormat::getXBytes already refuses at its own call site, left
/// live at the other four. `10 + (2^64 - 5)` wraps to 5, which is not greater
/// than 512, so the requested size was handed straight back: the clamp
/// returned 18446744073709551611 bytes out of a 512-byte region.
TEST(GetRealSizeInRegionTests, ARequestedSizeThatWrapsPastTheRegionIsClamped)
{
	EXPECT_EQ(502u, getRealSizeInRegion(10, kMax - 4, 512));
}

/// The same shape one byte from the end, where the wrap is to zero rather than
/// to a small number.
TEST(GetRealSizeInRegionTests, AWrapToZeroIsClampedToo)
{
	EXPECT_EQ(1u, getRealSizeInRegion(511, kMax, 512));
}

/// Every answer this function ever gave for an in-range request is the answer
/// it still gives. The clamp was narrowed to the wrapping cases; it was not
/// made stricter.
TEST(GetRealSizeInRegionTests, InRangeRequestsAreUnchanged)
{
	EXPECT_EQ(100u, getRealSizeInRegion(10, 100, 512));
	EXPECT_EQ(502u, getRealSizeInRegion(10, 502, 512));
	EXPECT_EQ(512u, getRealSizeInRegion(0, 512, 512));
	EXPECT_EQ(1u, getRealSizeInRegion(511, 1, 512));
}

/// A request that overruns the end without wrapping was always clamped, and
/// still is.
TEST(GetRealSizeInRegionTests, ARequestPastTheEndIsClampedToWhatIsLeft)
{
	EXPECT_EQ(502u, getRealSizeInRegion(10, 503, 512));
	EXPECT_EQ(512u, getRealSizeInRegion(0, kMax, 512));
}

/// Zero means "everything from the offset to the end", which is the one case
/// the caller cannot express with a size.
TEST(GetRealSizeInRegionTests, ZeroMeansTheRestOfTheRegion)
{
	EXPECT_EQ(502u, getRealSizeInRegion(10, 0, 512));
	EXPECT_EQ(512u, getRealSizeInRegion(0, 0, 512));
}

/// An offset at or past the end has nothing left, whatever was asked for.
TEST(GetRealSizeInRegionTests, AnOffsetOutsideTheRegionYieldsNothing)
{
	EXPECT_EQ(0u, getRealSizeInRegion(512, 4, 512));
	EXPECT_EQ(0u, getRealSizeInRegion(513, 0, 512));
	EXPECT_EQ(0u, getRealSizeInRegion(0, 0, 0));
	EXPECT_EQ(0u, getRealSizeInRegion(kMax, kMax, 512));
}

} // namespace
