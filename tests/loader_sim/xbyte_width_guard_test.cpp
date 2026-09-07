/**
 * @file tests/loader_sim/xbyte_width_guard_test.cpp
 * @brief The bounds arithmetic behind the byte readers in FileFormat and Image.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * What is under test
 * ------------------
 * The readers in src/fileformat/file_format/file_format.cpp and
 * src/loader/loader/image.cpp bounded caller-supplied offsets and widths with
 * expressions that form the sum or the product first:
 *
 *     x * getByteLength() > sizeof(res) * CHAR_BIT     (getXByte, setXByte)
 *     offset + x > getLoadedFileLength()               (getXByteOffset, ...)
 *     offset + numberOfBytes > getLoadedFileLength()   (getBytes)
 *     secStart + sec->getSizeInFile()                  (isObjectStretched...)
 *     item->getOffset() + item->getSizeInFile()        (getDeclaredFileLength)
 *
 * Every operand comes out of a file this process did not write, so a wrapped
 * value compares as small and the guard admits exactly the request it exists to
 * refuse. That arithmetic now lives in named functions -- the `bounds_kernels`
 * namespace of each of those two files -- and this suite calls them.
 *
 * How this suite reaches the real code
 * ------------------------------------
 * Neither source can be compiled here as a whole: both reach
 * retdec/fileformat/types/sec_seg/sec_seg.h, which includes
 * <llvm/ADT/StringRef.h>, and deps/llvm in this repository is a download stub
 * holding only CMakeLists.txt and a config file.
 *
 *     $ g++ -std=c++17 -Iinclude -fsyntax-only src/loader/loader/image.cpp
 *     include/retdec/fileformat/types/sec_seg/sec_seg.h:14:10: fatal error:
 *     llvm/ADT/StringRef.h: No such file or directory
 *
 * So each file puts its bounds arithmetic above a
 * `#ifndef RETDEC_..._BOUNDS_KERNELS_ONLY` guard that fences off everything
 * needing LLVM, and this file includes both sources with that macro defined.
 * The functions called below are therefore the ones the shipped readers call --
 * the same definitions, compiled from the same lines, not a copy and not a
 * description of them. An earlier version of this suite searched those files
 * for token strings instead; that could not tell a correct guard from one with
 * its polarity inverted, and it is gone.
 *
 * What this does and does not cover
 * ---------------------------------
 * Covered, by execution: every bound the readers apply, including the copy in
 * FileFormat::getBytes, which moved into the kernel region for exactly this
 * reason and runs here against a real heap buffer. Widening its clamp by eight
 * bytes -- too small a change for any size assertion elsewhere to notice --
 * gives "AddressSanitizer: heap-buffer-overflow ... in memmove" under
 * EXTRA_CXXFLAGS="-fsanitize=address,undefined". Restoring the wrapping clamp
 * outright is caught earlier and more cheaply, by the expected length: the
 * suite reports 18446744073709551611 where it wanted 502.
 *
 * Not covered: that each FileFormat/Image method still calls its kernel. That
 * is a property of code no compiler in this container can build. Deleting a
 * call and re-inlining the wrapping expression would leave this suite green,
 * though deleting the kernel it calls would not -- this file would then fail to
 * compile, and the suite fails to build, which is what a plain `git checkout`
 * of either source produces today.
 */

// Both sources are included for their bounds arithmetic only; the macros fence
// off the parts that need an LLVM tree. Defined before the includes, and never
// undefined, because the two guards are independent and each source is included
// exactly once.
#define RETDEC_FILEFORMAT_BOUNDS_KERNELS_ONLY 1
#define RETDEC_LOADER_BOUNDS_KERNELS_ONLY 1

#include "../../src/fileformat/file_format/file_format.cpp"
#include "../../src/loader/loader/image.cpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

namespace ff = retdec::fileformat::bounds_kernels;
namespace ld = retdec::loader::bounds_kernels;

/// ESBMC's counterexample for `x * getByteLength() > sizeof(res) * CHAR_BIT`.
constexpr std::uint64_t kWidthWitness = 0x2000000000000002ULL; // 2305843009213693954

/// The request that makes `offset + numberOfBytes > length` skip its own clamp:
/// at offset 10 the sum is 5, which is inside any file of six bytes or more.
constexpr std::uint64_t kWrappingRequest = 0xFFFFFFFFFFFFFFFBULL; // 2^64 - 5

/// A width that survives truncation to unsigned but cannot fit an accumulator.
constexpr std::uint64_t kNarrowingWidth = 0x100000002ULL;

/// Does @a x units of @a unitBits bits fit a 64-bit accumulator?
///
/// The oracle is written as a division so that it shares no arithmetic with the
/// guarded multiplication under test; for unitBits >= 1 the two are the same
/// predicate, and the division cannot wrap for any input.
bool widthFitsOracle(std::uint64_t x, std::uint64_t unitBits)
{
	if (x == 0 || unitBits == 0 || unitBits > 64)
	{
		return false;
	}
	return x <= 64ULL / unitBits;
}

/// Does the half-open range [@a offset, @a offset + @a len) lie inside a buffer
/// of @a size bytes?
///
/// Counts one byte at a time rather than comparing endpoints, so it shares no
/// arithmetic with the closed form under test. Only usable for small lengths,
/// which is the point: the wrapping cases are checked one by one below.
bool rangeFitsCountingOracle(std::uint64_t offset, std::uint64_t size, std::uint64_t len)
{
	if (offset > size)
	{
		return false;
	}
	std::uint64_t pos = offset;
	for (std::uint64_t i = 0; i < len; ++i)
	{
		if (pos >= size)
		{
			return false;
		}
		++pos;
	}
	return true;
}

/// A buffer whose bytes are distinguishable, on the heap so that a read past
/// its end is a heap-buffer-overflow under AddressSanitizer rather than a value
/// nobody notices.
std::vector<unsigned char> patternBuffer(std::size_t size)
{
	std::vector<unsigned char> data(size);
	for (std::size_t i = 0; i < size; ++i)
	{
		data[i] = static_cast<unsigned char>((i * 7 + 11) & 0xFF);
	}
	return data;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// The width guard: x units of getByteLength() bits into a 64-bit accumulator
// ═══════════════════════════════════════════════════════════════════════════════

TEST(XByteWidthGuard, RefusesTheWitnessTheWrappedProductAdmitted)
{
	// What the replaced expression computed, recomputed here rather than
	// asserted from a comment: the product wraps to 16, and `16 > 64` is false,
	// so a width of 2.3e18 units passed a guard meant to cap it at 8.
	EXPECT_EQ(static_cast<std::uint64_t>(kWidthWitness * 8ULL), 16ULL);
	EXPECT_FALSE(kWidthWitness * 8ULL > 64ULL);

	// What the shipped guards answer.
	EXPECT_FALSE(ff::xWidthFitsAccumulator(kWidthWitness, 8));
	EXPECT_FALSE(ld::xWidthFitsAccumulator(kWidthWitness, 8));
}

TEST(XByteWidthGuard, AcceptsExactlyTheWidthsAnEightBitByteAllows)
{
	// A guard whose polarity is inverted passes the witness above and fails
	// here, which is what this test exists for.
	for (std::uint64_t x = 1; x <= 8; ++x)
	{
		EXPECT_TRUE(ff::xWidthFitsAccumulator(x, 8)) << "fileformat, x = " << x;
		EXPECT_TRUE(ld::xWidthFitsAccumulator(x, 8)) << "loader, x = " << x;
	}
	for (std::uint64_t x = 9; x <= 72; ++x)
	{
		EXPECT_FALSE(ff::xWidthFitsAccumulator(x, 8)) << "fileformat, x = " << x;
		EXPECT_FALSE(ld::xWidthFitsAccumulator(x, 8)) << "loader, x = " << x;
	}

	// x == 0 is refused by the kernel; the call sites answer a zero-width read
	// with res = 0 through their own `x != 0` branch, so this is not a hole.
	EXPECT_FALSE(ff::xWidthFitsAccumulator(0, 8));
	EXPECT_FALSE(ld::xWidthFitsAccumulator(0, 8));
}

TEST(XByteWidthGuard, RefusesTheWidthsThatSurviveNarrowing)
{
	// 0x100000002 truncates to 2 in a 32-bit std::size_t, and 0x100000008 to 8
	// in a 32-bit unsigned; both would then be accepted as ordinary widths.
	EXPECT_FALSE(ff::xWidthFitsAccumulator(kNarrowingWidth, 8));
	EXPECT_FALSE(ld::xWidthFitsAccumulator(kNarrowingWidth, 8));
	EXPECT_FALSE(ff::xWidthFitsAccumulator(4, 0x100000008ULL));
	EXPECT_FALSE(ld::xWidthFitsAccumulator(4, 0x100000008ULL));
	EXPECT_FALSE(ff::xWidthFitsAccumulator(UINT64_MAX, 8));
	EXPECT_FALSE(ld::xWidthFitsAccumulator(UINT64_MAX, 8));
}

TEST(XByteWidthGuard, AgreesWithAnUnwrappedOracleOverEveryUnitWidth)
{
	const std::uint64_t units[] = {0, 1, 2, 3, 4, 7, 8, 9, 16, 31, 32, 63, 64, 65,
			128, 0x100000008ULL, UINT64_MAX};
	const std::uint64_t widths[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 32, 63, 64,
			65, 100, kNarrowingWidth, kWidthWitness, kWrappingRequest, UINT64_MAX};

	for (std::uint64_t unitBits : units)
	{
		for (std::uint64_t x : widths)
		{
			const bool want = widthFitsOracle(x, unitBits);
			EXPECT_EQ(ff::xWidthFitsAccumulator(x, unitBits), want)
					<< "fileformat, x = " << x << ", unitBits = " << unitBits;
			EXPECT_EQ(ld::xWidthFitsAccumulator(x, unitBits), want)
					<< "loader, x = " << x << ", unitBits = " << unitBits;
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// The containment guard: [offset, offset + len) inside a buffer
// ═══════════════════════════════════════════════════════════════════════════════

TEST(XByteRangeGuard, RefusesTheOffsetWhoseSumWraps)
{
	constexpr std::uint64_t kFileLength = 512;

	// Why the replaced expression passed: SIZE_MAX + 1 is 0, and 0 is inside
	// every file, so `offset + x > length` was false and the reader went on to
	// form `loadedBytes->begin() + SIZE_MAX`.
	EXPECT_EQ(static_cast<std::size_t>(SIZE_MAX + std::size_t(1)), std::size_t(0));
	EXPECT_TRUE(SIZE_MAX + std::size_t(1) <= kFileLength);

	EXPECT_FALSE(ff::rangeFitsWide(SIZE_MAX, kFileLength, 1));
	EXPECT_FALSE(ld::rangeFitsWide(SIZE_MAX, kFileLength, 1));
	EXPECT_FALSE(ff::rangeFitsWide(10, kFileLength, kWrappingRequest));
	EXPECT_FALSE(ld::rangeFitsWide(10, kFileLength, kWrappingRequest));
	EXPECT_FALSE(ff::rangeFitsWide(UINT64_MAX, kFileLength, UINT64_MAX));
	EXPECT_FALSE(ld::rangeFitsWide(UINT64_MAX, kFileLength, UINT64_MAX));
}

TEST(XByteRangeGuard, StillAdmitsEveryReadThatIsInsideTheBuffer)
{
	constexpr std::uint64_t kFileLength = 512;

	// A guard neutered to a constant false fails here; one inverted fails both
	// here and above.
	EXPECT_TRUE(ff::rangeFitsWide(0, kFileLength, kFileLength));
	EXPECT_TRUE(ff::rangeFitsWide(504, kFileLength, 8));
	EXPECT_TRUE(ff::rangeFitsWide(kFileLength, kFileLength, 0));
	EXPECT_FALSE(ff::rangeFitsWide(505, kFileLength, 8));
	EXPECT_FALSE(ff::rangeFitsWide(kFileLength + 1, kFileLength, 0));

	EXPECT_TRUE(ld::rangeFitsWide(0, kFileLength, kFileLength));
	EXPECT_TRUE(ld::rangeFitsWide(504, kFileLength, 8));
	EXPECT_TRUE(ld::rangeFitsWide(kFileLength, kFileLength, 0));
	EXPECT_FALSE(ld::rangeFitsWide(505, kFileLength, 8));
	EXPECT_FALSE(ld::rangeFitsWide(kFileLength + 1, kFileLength, 0));
}

TEST(XByteRangeGuard, AgreesWithACountingOracleOverSmallBuffers)
{
	for (std::uint64_t size = 0; size <= 12; ++size)
	{
		for (std::uint64_t offset = 0; offset <= 16; ++offset)
		{
			for (std::uint64_t len = 0; len <= 16; ++len)
			{
				const bool want = rangeFitsCountingOracle(offset, size, len);
				EXPECT_EQ(ff::rangeFitsWide(offset, size, len), want)
						<< "fileformat, offset = " << offset
						<< ", size = " << size << ", len = " << len;
				EXPECT_EQ(ld::rangeFitsWide(offset, size, len), want)
						<< "loader, offset = " << offset
						<< ", size = " << size << ", len = " << len;
			}
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// FileFormat::getBytes: the clamp, and the copy it is supposed to bound
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ClampedRead, ShortensTheRequestThatWrapsToTheBytesThatRemain)
{
	constexpr std::uint64_t kFileLength = 512;

	// The replaced expression, recomputed: the sum is 5, which is not greater
	// than 512, so the clamp did not fire and the length stayed at 2^64 - 5.
	EXPECT_EQ(static_cast<std::uint64_t>(10ULL + kWrappingRequest), 5ULL);
	EXPECT_FALSE(10ULL + kWrappingRequest > kFileLength);

	std::size_t start = 12345;
	std::size_t length = 12345;
	ASSERT_TRUE(ff::clampedReadLength(10, kFileLength, kWrappingRequest, start, length));
	EXPECT_EQ(start, std::size_t(10));
	EXPECT_EQ(length, std::size_t(502));

	// The same offset with a request that does not wrap has always clamped
	// correctly, which is what made the expression look right.
	ASSERT_TRUE(ff::clampedReadLength(10, kFileLength, 5000, start, length));
	EXPECT_EQ(length, std::size_t(502));
}

TEST(ClampedRead, NeverPermitsAReadPastTheEndOfTheBuffer)
{
	const std::uint64_t sizes[] = {0, 1, 2, 7, 512, 0x100000000ULL, UINT64_MAX};
	const std::uint64_t offsets[] = {0, 1, 6, 10, 511, 512, 513,
			kWrappingRequest, SIZE_MAX, UINT64_MAX};
	const std::uint64_t requests[] = {0, 1, 8, 502, 512, 5000,
			kWrappingRequest, SIZE_MAX, UINT64_MAX};

	for (std::uint64_t size : sizes)
	{
		for (std::uint64_t offset : offsets)
		{
			for (std::uint64_t requested : requests)
			{
				std::size_t start = 0;
				std::size_t length = 0;
				if (!ff::clampedReadLength(offset, size, requested, start, length))
				{
					continue;
				}

				// The postcondition the copy below relies on, stated without
				// forming start + length.
				ASSERT_LE(static_cast<std::uint64_t>(start), size)
						<< "size = " << size << ", offset = " << offset;
				ASSERT_LE(static_cast<std::uint64_t>(length), size - start)
						<< "size = " << size << ", offset = " << offset
						<< ", requested = " << requested;
				// And it never invents bytes the caller did not ask for.
				ASSERT_LE(static_cast<std::uint64_t>(length), requested)
						<< "size = " << size << ", offset = " << offset
						<< ", requested = " << requested;
			}
		}
	}
}

TEST(ClampedRead, RefusesAnOffsetAtOrPastTheEnd)
{
	std::size_t start = 7;
	std::size_t length = 7;
	EXPECT_FALSE(ff::clampedReadLength(512, 512, 1, start, length));
	EXPECT_FALSE(ff::clampedReadLength(513, 512, 1, start, length));
	EXPECT_FALSE(ff::clampedReadLength(UINT64_MAX, 512, 1, start, length));
	EXPECT_FALSE(ff::clampedReadLength(0, 0, 1, start, length));
	// The outputs are untouched when the read is refused.
	EXPECT_EQ(start, std::size_t(7));
	EXPECT_EQ(length, std::size_t(7));
}

TEST(ClampedRead, TheCopyStopsAtTheEndOfARealBuffer)
{
	// A heap buffer of exactly 512 bytes: if the clamp stops clamping, this is
	// a heap-buffer-overflow under AddressSanitizer and a wrong size otherwise.
	const std::vector<unsigned char> file = patternBuffer(512);
	std::vector<std::uint8_t> out;

	ASSERT_TRUE(ff::copyClampedRange(
			file.data(), file.size(), 10, kWrappingRequest, out));
	ASSERT_EQ(out.size(), std::size_t(502));
	EXPECT_EQ(out.front(), file[10]);
	EXPECT_EQ(out.back(), file[511]);

	// An ordinary read is unchanged.
	ASSERT_TRUE(ff::copyClampedRange(file.data(), file.size(), 10, 100, out));
	ASSERT_EQ(out.size(), std::size_t(100));
	EXPECT_EQ(out.front(), file[10]);
	EXPECT_EQ(out.back(), file[109]);

	// A read that merely runs off the end is still shortened, as before.
	ASSERT_TRUE(ff::copyClampedRange(file.data(), file.size(), 500, 5000, out));
	EXPECT_EQ(out.size(), std::size_t(12));

	// And one that starts past the end is refused, leaving the caller's vector
	// alone -- the behaviour getBytes has always had.
	EXPECT_FALSE(ff::copyClampedRange(file.data(), file.size(), 512, 1, out));
	EXPECT_EQ(out.size(), std::size_t(12));
}

TEST(ClampedRead, TheCopyIsExactForEveryOffsetOfASmallBuffer)
{
	const std::vector<unsigned char> file = patternBuffer(64);
	std::vector<std::uint8_t> out;

	for (std::uint64_t offset = 0; offset < 64; ++offset)
	{
		for (std::uint64_t requested : {std::uint64_t(0), std::uint64_t(1),
				std::uint64_t(31), std::uint64_t(64), std::uint64_t(1000),
				kWrappingRequest, UINT64_MAX})
		{
			ASSERT_TRUE(ff::copyClampedRange(
					file.data(), file.size(), offset, requested, out))
					<< "offset = " << offset << ", requested = " << requested;

			const std::size_t remaining = 64 - static_cast<std::size_t>(offset);
			const std::size_t want = requested < remaining
					? static_cast<std::size_t>(requested) : remaining;
			ASSERT_EQ(out.size(), want)
					<< "offset = " << offset << ", requested = " << requested;
			for (std::size_t i = 0; i < out.size(); ++i)
			{
				ASSERT_EQ(out[i], file[static_cast<std::size_t>(offset) + i])
						<< "offset = " << offset << ", i = " << i;
			}
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Declared region ends: getDeclaredFileLength and isObjectStretchedOverSections
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RegionEnd, SaturatesWhereTheSumWouldWrap)
{
	// A section header declaring offset 0x10 and size 2^64 - 0x10 made the end
	// 0, so getDeclaredFileLength took the maximum over a zero and reported a
	// declared length shorter than the section it just read.
	EXPECT_EQ(static_cast<std::uint64_t>(0x10ULL + 0xFFFFFFFFFFFFFFF0ULL), 0ULL);
	EXPECT_EQ(ff::regionEndSaturating(0x10, 0xFFFFFFFFFFFFFFF0ULL), SIZE_MAX);

	EXPECT_EQ(ff::regionEndSaturating(0, 0), std::size_t(0));
	EXPECT_EQ(ff::regionEndSaturating(0x40, 0x200), std::size_t(0x240));
	EXPECT_EQ(ff::regionEndSaturating(SIZE_MAX, 1), SIZE_MAX);
	EXPECT_EQ(ff::regionEndSaturating(SIZE_MAX, 0), SIZE_MAX);
	EXPECT_EQ(ff::regionEndSaturating(0, SIZE_MAX), SIZE_MAX);
	EXPECT_EQ(ff::regionEndSaturating(UINT64_MAX, UINT64_MAX), SIZE_MAX);
}

TEST(RegionEnd, ADeclaredLengthIsNeverShortenedByAWrappingSection)
{
	// getDeclaredFileLength folds the maximum of these ends. With the wrapped
	// term the malformed section contributed 0 and a 512-byte file was declared
	// to end at 0x240, so getOverlaySize answered about the wrong region.
	const std::size_t honest = ff::regionEndSaturating(0x40, 0x200);
	const std::size_t malformed = ff::regionEndSaturating(0x10, 0xFFFFFFFFFFFFFFF0ULL);
	EXPECT_GT(malformed, honest);
	EXPECT_EQ(std::max(honest, malformed), SIZE_MAX);
}

TEST(ObjectPlacement, FindsAnObjectInsideASectionWhoseEndWouldWrap)
{
	// secEnd wrapped to 0, so `addr < secEnd` was false for every address and
	// the loop walked straight past the section that contains the object.
	EXPECT_EQ(
			ff::placeObjectInRegion(0x10, 0xFFFFFFFFFFFFFFF0ULL, 0x100, 8),
			ff::ObjectPlacement::Contained);
}

TEST(ObjectPlacement, ReportsAnObjectWhoseOwnEndWouldWrapAsStretched)
{
	// addrEnd wrapped to 255, and `255 > 512` is false, so an object running
	// off the end of the address space was called contained.
	EXPECT_EQ(static_cast<std::uint64_t>(256ULL + UINT64_MAX), 255ULL);
	EXPECT_EQ(
			ff::placeObjectInRegion(0, 512, 256, UINT64_MAX),
			ff::ObjectPlacement::Stretched);
}

TEST(ObjectPlacement, ClassifiesOrdinarySectionsUnchanged)
{
	// Wholly inside.
	EXPECT_EQ(ff::placeObjectInRegion(0x100, 0x200, 0x180, 0x10),
			ff::ObjectPlacement::Contained);
	// Ends exactly at the section end.
	EXPECT_EQ(ff::placeObjectInRegion(0x100, 0x200, 0x2F0, 0x10),
			ff::ObjectPlacement::Contained);
	// One byte past it.
	EXPECT_EQ(ff::placeObjectInRegion(0x100, 0x200, 0x2F0, 0x11),
			ff::ObjectPlacement::Stretched);
	// Before the section, after it, and against an empty section.
	EXPECT_EQ(ff::placeObjectInRegion(0x100, 0x200, 0xFF, 4),
			ff::ObjectPlacement::Elsewhere);
	EXPECT_EQ(ff::placeObjectInRegion(0x100, 0x200, 0x300, 4),
			ff::ObjectPlacement::Elsewhere);
	EXPECT_EQ(ff::placeObjectInRegion(0x100, 0, 0x100, 4),
			ff::ObjectPlacement::Elsewhere);
}

// ═══════════════════════════════════════════════════════════════════════════════
// The two modules answer the same question the same way
// ═══════════════════════════════════════════════════════════════════════════════

TEST(BothModules, GiveIdenticalAnswersToIdenticalQuestions)
{
	// FileFormat::getXByte and Image::getXByte guard the same read of the same
	// bytes; a fix applied to one file and not the other is a fix that is not
	// finished.
	const std::uint64_t probes[] = {0, 1, 8, 9, 64, 65, kNarrowingWidth,
			kWidthWitness, kWrappingRequest, SIZE_MAX, UINT64_MAX};

	for (std::uint64_t a : probes)
	{
		EXPECT_EQ(ff::xWidthFitsAccumulator(a, 8), ld::xWidthFitsAccumulator(a, 8))
				<< "x = " << a;
		for (std::uint64_t b : probes)
		{
			EXPECT_EQ(ff::rangeFitsWide(a, 512, b), ld::rangeFitsWide(a, 512, b))
					<< "offset = " << a << ", len = " << b;
		}
	}
}
