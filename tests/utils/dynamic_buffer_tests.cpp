/**
 * @file tests/utils/dynamic_buffer_tests.cpp
 * @brief Tests for DynamicBuffer.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * This class had no tests at all, and the whole `utils` suite was excluded from
 * the dependency-free check for a gmock include in two unrelated files -- so
 * nothing here had ever been exercised by the fast path. Every caller is
 * unpacker code that takes its positions and lengths from the packed file.
 */

#include "retdec/utils/dynamic_buffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace retdec::utils;

namespace {

// ─── writeRepeatingByte ──────────────────────────────────────────────────────

TEST(DynamicBufferTests, WriteRepeatingByteInBounds)
{
	DynamicBuffer buf(16, Endianness::LITTLE);
	buf.writeRepeatingByte(0xAB, 4, 4);
	EXPECT_EQ(8u, buf.getRealDataSize());
	for (uint32_t i = 4; i < 8; ++i)
		EXPECT_EQ(0xAB, buf.read<uint8_t>(i));
}

TEST(DynamicBufferTests, WriteRepeatingByteIsClampedToCapacity)
{
	DynamicBuffer buf(8, Endianness::LITTLE);
	buf.writeRepeatingByte(0xCD, 6, 100);
	// Only the two bytes the capacity can hold.
	EXPECT_LE(buf.getRealDataSize(), 8u);
}

TEST(DynamicBufferTests, WriteRepeatingBytePastCapacityWritesNothing)
{
	// The heap overflow. `pos + repeatAmount > _capacity` then
	// `repeatAmount = _capacity - pos` underflows once pos is past the
	// capacity: at capacity 100, pos 200 and repeatAmount 1 the clamp produced
	// 4294967196, the resize test wrapped 200 + 4294967196 back to 100 and did
	// nothing, and the memset wrote about four gigabytes starting a hundred
	// bytes past a hundred-byte vector. Under ASan this test is a
	// heap-buffer-overflow without the fix; without a sanitizer it is a
	// segfault or a corrupted heap.
	DynamicBuffer buf(100, Endianness::LITTLE);
	buf.writeRepeatingByte(0xFF, 0, 100);
	ASSERT_EQ(100u, buf.getRealDataSize());

	buf.writeRepeatingByte(0xEE, 200, 1);
	EXPECT_EQ(100u, buf.getRealDataSize());
}

TEST(DynamicBufferTests, WriteRepeatingByteAtExactlyCapacityWritesNothing)
{
	DynamicBuffer buf(16, Endianness::LITTLE);
	buf.writeRepeatingByte(0x11, 16, 4);
	EXPECT_EQ(0u, buf.getRealDataSize());
}

// ─── erase ───────────────────────────────────────────────────────────────────

TEST(DynamicBufferTests, EraseInBounds)
{
	DynamicBuffer buf(8, Endianness::LITTLE);
	buf.writeRepeatingByte(0x22, 0, 8);
	buf.erase(2, 3);
	EXPECT_EQ(5u, buf.getRealDataSize());
}

TEST(DynamicBufferTests, EraseAmountThatWrapsIsClamped)
{
	// `startPos + amount > _data.size()` is a uint32 sum: at startPos 100 and
	// amount 0xFFFFFFFF it wraps to 99, the test is false, amount survives
	// unclamped, and the second iterator is formed 4 GB past the end.
	DynamicBuffer buf(200, Endianness::LITTLE);
	buf.writeRepeatingByte(0x33, 0, 200);
	ASSERT_EQ(200u, buf.getRealDataSize());

	buf.erase(100, 0xFFFFFFFFu);
	EXPECT_EQ(100u, buf.getRealDataSize());
}

TEST(DynamicBufferTests, EraseStartingPastTheEndIsANoOp)
{
	DynamicBuffer buf(8, Endianness::LITTLE);
	buf.writeRepeatingByte(0x44, 0, 8);
	buf.erase(64, 4);
	EXPECT_EQ(8u, buf.getRealDataSize());
}

// ─── the sub-buffer constructor ──────────────────────────────────────────────

TEST(DynamicBufferTests, SubBufferInBounds)
{
	DynamicBuffer src(8, Endianness::LITTLE);
	src.writeRepeatingByte(0x55, 0, 8);
	DynamicBuffer sub(src, 2, 4);
	EXPECT_EQ(4u, sub.getRealDataSize());
	EXPECT_EQ(0x55, sub.read<uint8_t>(0));
}

TEST(DynamicBufferTests, SubBufferStartingPastTheEndIsEmpty)
{
	// This constructor had no bounds check whatsoever: both iterators were
	// formed from startPos and amount immediately, so a startPos past the end
	// of the source is undefined before the vector is constructed. Reached from
	// pe_upx_stub.cpp four times with offsets read out of the packed file.
	DynamicBuffer src(8, Endianness::LITTLE);
	src.writeRepeatingByte(0x66, 0, 8);
	DynamicBuffer sub(src, 64, 4);
	EXPECT_EQ(0u, sub.getRealDataSize());
}

TEST(DynamicBufferTests, SubBufferAmountPastTheEndIsTruncated)
{
	DynamicBuffer src(8, Endianness::LITTLE);
	src.writeRepeatingByte(0x77, 0, 8);
	DynamicBuffer sub(src, 6, 0xFFFFFFFFu);
	EXPECT_EQ(2u, sub.getRealDataSize());
}

} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
