/**
 * @file tests/retdec/managed_decompiler_test.cpp
 * @brief Tests for the managed-format router over hostile bytes.
 *
 * detectManagedFormatFromBytes() decides, from the first bytes of a file
 * nobody vouched for, which parser gets to look at it. It lives in
 * src/retdec-decompiler/, whose other translation units are the LLVM-backed
 * CLI front end, so nothing in the fast gate reached it until this suite;
 * scripts/standalone_check.sh now compiles that one file through
 * EXTRA_SOURCES.
 *
 * The cases below are all rejections. That is the point: each one used to be
 * a rejection that read past the end of the buffer on the way to deciding.
 */

#include "../../src/retdec-decompiler/managed_decompiler.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

/// Runs the router on an exactly-sized heap buffer, so a sanitizer sees any
/// read past the end. A std::vector would too, but only past its capacity.
ManagedFormat routeExactly(const std::vector<std::uint8_t>& image)
{
	std::vector<std::uint8_t> exact(image.begin(), image.end());
	exact.shrink_to_fit();
	return detectManagedFormatFromBytes(exact.data(), exact.size());
}

std::vector<std::uint8_t> mzStub(std::uint32_t e_lfanew)
{
	std::vector<std::uint8_t> img(0x40, 0);
	img[0] = 'M';
	img[1] = 'Z';
	img[0x3C] = static_cast<std::uint8_t>(e_lfanew & 0xFF);
	img[0x3D] = static_cast<std::uint8_t>((e_lfanew >> 8) & 0xFF);
	img[0x3E] = static_cast<std::uint8_t>((e_lfanew >> 16) & 0xFF);
	img[0x3F] = static_cast<std::uint8_t>((e_lfanew >> 24) & 0xFF);
	return img;
}

} // namespace

// probeCliAssembly() guarded its PE header read with `peOff + 24 > size`.
// peOff is what the file says at 0x3C; held as a uint32_t that sum is 32-bit,
// so 0xFFFFFFFF + 24 is 23, the guard passes, and the memcmp that follows
// reads about 4 GiB past a 64-byte buffer. Under ASan this test is a SEGV in
// __memcmp_evex_movbe; without one it is a wild read that usually is not.
//
// The four values below are every e_lfanew whose sum wraps.
TEST(ManagedFormatRouter, PeOffsetNearFourGibDoesNotReadPastTheBuffer)
{
	for (std::uint32_t e_lfanew: {0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu})
	{
		EXPECT_EQ(routeExactly(mzStub(e_lfanew)), ManagedFormat::Unknown) << "e_lfanew=" << e_lfanew;
	}
}

// The offsets derived from peOff -- opt, ddStart and comOff -- were uint32_t
// too, so each carried the same wrap. An e_lfanew just under the top makes
// opt = peOff + 24 wrap on its own.
TEST(ManagedFormatRouter, DerivedPeOffsetsDoNotWrap)
{
	for (std::uint32_t e_lfanew: {0x80000000u, 0xFFFFFF00u, 0xFFFFFFA0u})
	{
		EXPECT_EQ(routeExactly(mzStub(e_lfanew)), ManagedFormat::Unknown) << "e_lfanew=" << e_lfanew;
	}
}

// An e_lfanew that points inside the stub but leaves no PE signature is the
// ordinary rejection, and must stay one.
TEST(ManagedFormatRouter, PlainMzStubIsNotManaged)
{
	EXPECT_EQ(routeExactly(mzStub(0x40)), ManagedFormat::Unknown);
	EXPECT_EQ(routeExactly(mzStub(0x00)), ManagedFormat::Unknown);
}

// The router is reached with anything at all; a truncated buffer must not be
// read past either.
TEST(ManagedFormatRouter, ShortBuffersAreRejected)
{
	for (std::size_t n = 0; n <= 0x40; ++n)
	{
		std::vector<std::uint8_t> img(n, 0);
		if (n >= 2)
		{
			img[0] = 'M';
			img[1] = 'Z';
		}
		EXPECT_EQ(routeExactly(img), ManagedFormat::Unknown) << "size=" << n;
	}
}
