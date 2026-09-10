/**
 * @file tests/fileformat/bitmap_image_tests.cpp
 * @brief What bounds the allocations BitmapImage makes from a DIB header.
 *
 * Every count in BitmapImage::parseDib*Data comes from a field the file
 * supplies, and the products of them are large. At width 0xFFFFFFFF, height
 * 0xFFFFFFFF and bitCount 0xFFFF:
 *
 *     nBytesInRow = ((bitCount * width + 31) / 32) * 4 = 536862724
 *     nBytes      = nBytesInRow * (height / 2)         = 1152903920473874428
 *
 * 1.15 exabytes, and `std::vector<std::uint8_t>::max_size()` on a 64-bit host
 * is PTRDIFF_MAX -- larger. So `bytes.reserve(nBytes)` is not a
 * std::length_error that a caller might be expected to handle; it is an
 * allocation attempt, and it raises std::bad_alloc. Measured, with the clamp
 * removed:
 *
 *     C++ exception with description "std::bad_alloc" thrown in the test body.
 *
 * GoogleTest catches that and reports a failure. Nothing between BitmapImage
 * and main() does, so in the decompiler it is a std::terminate.
 *
 * docs/internal/UNFIXED_AUDIT_FINDINGS.md listed this as reachable. It is not,
 * through the only path that exists today: parseDibFormat is the sole caller
 * and runs parseDibHeader first, which refuses width > 512, height > 1024 and
 * bitCount > 32 -- so nBytes there is at most 2048 * 512, a megabyte.
 *
 * That bound is four calls away from the multiplication and nothing near it
 * says so, and every parseDib*Data is public. These call them the way anything
 * else in the tree could: directly, with a header parseDibHeader would have
 * refused.
 */

#include "retdec/fileformat/types/resource_table/bitmap_image.h"
#include "retdec/fileformat/types/resource_table/resource_icon.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace retdec {
namespace fileformat {
namespace tests {
namespace {

/// A DIB header stating the largest image its fields can describe.
BitmapInformationHeader hostileHeader(std::uint16_t bitCount)
{
	BitmapInformationHeader hdr;
	hdr.size = hdr.headerSize();
	hdr.width = 0xFFFFFFFFu;
	hdr.height = 0xFFFFFFFFu;
	hdr.planes = 1;
	hdr.bitCount = bitCount;
	hdr.compression = 0;
	hdr.bitmapSize = 0;
	hdr.horizontalRes = 0;
	hdr.verticalRes = 0;
	hdr.colorsUsed = 0;
	hdr.colorImportant = 0;
	return hdr;
}

//
// The two that reach the multiplication.
//
// 24 and 32 bpp have no palette, so nothing refuses before the reserves. These
// are the cases that raise std::bad_alloc without the clamp -- verified by
// removing it, when these two fail and the other four do not.
//

TEST(BitmapImageTests, ParseDib24DataOnAnEmptyIconWithAHostileHeaderRefuses)
{
	ResourceIcon icon;
	BitmapImage image;

	EXPECT_FALSE(image.parseDib24Data(icon, hostileHeader(24)));
	EXPECT_TRUE(image.getImage().empty());
}

TEST(BitmapImageTests, ParseDib32DataOnAnEmptyIconWithAHostileHeaderRefuses)
{
	ResourceIcon icon;
	BitmapImage image;

	EXPECT_FALSE(image.parseDib32Data(icon, hostileHeader(32)));
	EXPECT_TRUE(image.getImage().empty());
}

//
// The three that do not, and why they are still here.
//
// 1, 4 and 8 bpp read a palette first, and parseDibPalette refuses an empty
// icon before either reserve is reached -- so these pass with the clamp and
// without it, and they are NOT evidence for it. What they hold is the ordering:
// move the palette read after the reserves, or give these an icon with a
// palette in it, and the exabyte is reached here too. That is the regression
// they exist to catch, and it is a different one.
//

TEST(BitmapImageTests, ParseDib1DataRefusesBeforeReachingTheMultiplication)
{
	ResourceIcon icon;
	BitmapImage image;

	EXPECT_FALSE(image.parseDib1Data(icon, hostileHeader(1)));
}

TEST(BitmapImageTests, ParseDib4DataRefusesBeforeReachingTheMultiplication)
{
	ResourceIcon icon;
	BitmapImage image;

	EXPECT_FALSE(image.parseDib4Data(icon, hostileHeader(4)));
}

TEST(BitmapImageTests, ParseDib8DataRefusesBeforeReachingTheMultiplication)
{
	ResourceIcon icon;
	BitmapImage image;

	EXPECT_FALSE(image.parseDib8Data(icon, hostileHeader(8)));
}

/// Also not evidence for the clamp, and for a more interesting reason.
/// `std::size_t nBytes = nColors * 4` is a uint32 product: 0xFFFFFFFF colours
/// is 0xFFFFFFFC bytes, not 0x3FFFFFFFC. Four gigabytes is a reserve Linux will
/// often satisfy by overcommitting, so this one does not raise bad_alloc here
/// and cannot be made to fail by removing the clamp. It is kept because the
/// wrap is real and the number it produces is the one worth having written
/// down.
TEST(BitmapImageTests, ParseDibPaletteOnAnEmptyIconWithAHostileColourCountRefuses)
{
	ResourceIcon icon;
	BitmapImage image;
	std::vector<struct BitmapPixel> palette;

	EXPECT_FALSE(image.parseDibPalette(icon, palette, 0xFFFFFFFFu));
	EXPECT_TRUE(palette.empty());
}

} // namespace
} // namespace tests
} // namespace fileformat
} // namespace retdec
