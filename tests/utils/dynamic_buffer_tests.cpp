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

// ─── 32-bit wrap in the write and read templates ─────────────────────────────
//
// writeImpl and readImpl compute `pos + sizeof(T)` in uint32 arithmetic, twice
// each: once to clamp against the capacity and once to decide whether to grow.
// Both sums wrap when pos is within sizeof(T) of 2^32, which is reachable
// whenever the capacity is near 0xFFFFFFFF -- and the capacity comes from
// packed-file metadata. decompressor_lzma.cpp:241 and decompressor_nrv.cpp:228
// and :339 all call setCapacity(unpackedDataSize) with a value read from a UPX
// header, and pe_upx_stub.cpp:786 writes a uint32 at a position built from
// attacker-supplied relocation hints.
//
// erase() and writeRepeatingByte() in the matching .cpp were fixed for exactly
// this and say so in their comments; the two templates in the header were not.

// The arithmetic itself, at the positions that matter. Reaching them through
// write() means growing the buffer to 4 GiB by construction -- a write at index
// 0xFFFFFFFE needs 0xFFFFFFFF bytes of storage to be in bounds -- so the
// measurement that found this lives in the commit message and what is kept here
// is the expression. Before the fix the equivalent line was
// `if (pos + bytesToWrite > getCapacity()) bytesToWrite = getCapacity() - pos;`
// in uint32, and at pos 0xFFFFFFFE with width 4 it left the answer at 4:
//
//   ERROR: AddressSanitizer: heap-buffer-overflow
//   WRITE of size 1 at ... in DynamicBuffer::writeImpl<unsigned int>
//   0 bytes after 4294967295-byte region
TEST(DynamicBufferTests, WritableAtDoesNotWrapNearTheEndOfTheAddressSpace)
{
	// One byte left below the capacity, not four.
	EXPECT_EQ(1u, DynamicBuffer::writableAt(0xFFFFFFFEu, 0xFFFFFFFFu, 4));
	EXPECT_EQ(0u, DynamicBuffer::writableAt(0xFFFFFFFFu, 0xFFFFFFFFu, 4));
	EXPECT_EQ(2u, DynamicBuffer::writableAt(0xFFFFFFFDu, 0xFFFFFFFFu, 8));

	// And the ordinary answers, so a clamp that always returned zero would not
	// pass.
	EXPECT_EQ(4u, DynamicBuffer::writableAt(0, 64, 4));
	EXPECT_EQ(4u, DynamicBuffer::writableAt(60, 64, 4));
	EXPECT_EQ(2u, DynamicBuffer::writableAt(62, 64, 4));
	EXPECT_EQ(0u, DynamicBuffer::writableAt(64, 64, 4));
	EXPECT_EQ(0u, DynamicBuffer::writableAt(100, 64, 4));
}

TEST(DynamicBufferTests, ReadingPastTheDataIsRefusedRatherThanWrapping)
{
	// readImpl carries the identical arithmetic. Its first guard is
	// `pos >= _data.size()`, so the wrap is only reachable once the buffer is
	// itself 4 GiB; this checks the guard that keeps it that way.
	DynamicBuffer buffer(2);
	buffer.setCapacity(0xFFFFFFFFu);

	EXPECT_EQ(0u, buffer.read<std::uint32_t>(0xFFFFFFFEu));
	EXPECT_EQ(0u, buffer.read<std::uint32_t>(2));
}

TEST(DynamicBufferTests, SettingACapacityDoesNotAllocateIt)
{
	// setCapacity used to `_data.reserve(_capacity)`, and every caller passes a
	// size read out of the file being unpacked -- so a UPX header declaring
	// 0xFFFFFFFF made the unpacker allocate 4 GB before decompressing a byte.
	// The capacity is a limit on what may be written, not a promise that it
	// will be.
	DynamicBuffer buffer(2);
	buffer.setCapacity(0xFFFFFFFFu);

	EXPECT_EQ(0xFFFFFFFFu, buffer.getCapacity());
	EXPECT_EQ(0u, buffer.getRealDataSize());
}

TEST(DynamicBufferTests, AWriteStraddlingTheCapacityWritesOnlyWhatFits)
{
	// The ordinary case the clamp exists for, so a fix cannot be "return
	// early always".
	DynamicBuffer buffer(4);
	buffer.setCapacity(6);

	buffer.write<std::uint32_t>(0x11223344u, 4, retdec::utils::Endianness::LITTLE);

	ASSERT_EQ(6u, buffer.getRealDataSize());
	EXPECT_EQ(0x44, buffer.read<std::uint8_t>(4));
	EXPECT_EQ(0x33, buffer.read<std::uint8_t>(5));
}

TEST(DynamicBufferTests, AnOrdinaryWriteAndReadRoundTrip)
{
	DynamicBuffer buffer(0);
	buffer.setCapacity(64);

	buffer.write<std::uint32_t>(0xDEADBEEFu, 8, retdec::utils::Endianness::LITTLE);
	EXPECT_EQ(0xDEADBEEFu, buffer.read<std::uint32_t>(8, retdec::utils::Endianness::LITTLE));

	buffer.write<std::uint32_t>(0xDEADBEEFu, 16, retdec::utils::Endianness::BIG);
	EXPECT_EQ(0xDEADBEEFu, buffer.read<std::uint32_t>(16, retdec::utils::Endianness::BIG));
}

} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
