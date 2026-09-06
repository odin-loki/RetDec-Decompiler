/**
* @file tests/utils/byte_value_storage_kernel_tests.cpp
* @brief Regression tests for the ByteValueStorage defects that ESBMC refuted.
* @copyright (c) 2026 Odin Loch trading as Imortek
*
* These live apart from byte_value_storage_tests.cpp because that file includes
* <gmock/gmock.h>, which the standalone shim in tests/standalone/gtest does not
* provide, so it never runs in the fast path
* (scripts/standalone_check.sh, EXCLUDED_TEST_SOURCES). A regression test that
* only runs in the slow CMake build is a regression test nobody watches, so the
* fake below is hand-written and this file uses nothing but <gtest/gtest.h>.
*/

#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "retdec/utils/byte_value_storage.h"

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {

namespace {

/**
* @brief A ByteValueStorage whose getXByte is scripted and, crucially, BOUNDED.
*
* The walks under test used to run forever when handed a zero width. A fake
* that answers forever would turn those regressions into hangs rather than
* failures, and a test nobody can watch fail is not a regression test -- so
* getXByte here refuses after @c maxCalls answers. With the defect present the
* walk takes all of them; with it fixed the walk stops itself.
*/
class FakeStorage : public ByteValueStorage
{
public:
	Endianness endianness = Endianness::LITTLE;
	std::size_t byteLength = 8;

	/// Answers given before getXByte starts refusing.
	std::size_t maxCalls = 64;
	/// Answers given before getXByte starts returning the NUL terminator.
	std::size_t nonZeroCalls = 64;
	/// The non-terminator element; 'A', which isNiceAsciiWideCharacter accepts.
	std::uint64_t element = 0x41;

	mutable std::size_t calls = 0;
	mutable std::vector<std::uint64_t> addressesSeen;

	Endianness getEndianness() const override { return endianness; }
	std::size_t getNibbleLength() const override { return 4; }
	std::size_t getByteLength() const override { return byteLength; }
	std::size_t getWordLength() const override { return 32; }
	std::size_t getBytesPerWord() const override { return 4; }
	std::size_t getNumberOfNibblesInByte() const override { return 2; }
	bool hasMixedEndianForDouble() const override { return false; }

	bool getXByte(
			std::uint64_t address,
			std::uint64_t,
			std::uint64_t& res,
			Endianness) const override
	{
		if (calls >= maxCalls)
		{
			return false;
		}
		addressesSeen.push_back(address);
		res = calls < nonZeroCalls ? element : 0;
		++calls;
		return true;
	}

	bool getXBytes(
			std::uint64_t,
			std::uint64_t,
			std::vector<std::uint8_t>&) const override
	{
		return false;
	}

	bool setXByte(
			std::uint64_t,
			std::uint64_t,
			std::uint64_t,
			Endianness) override
	{
		return false;
	}

	bool setXBytes(std::uint64_t, const std::vector<std::uint8_t>&) override
	{
		return false;
	}

	// createValueFromBytes and createBytesFromValue are protected; the whole
	// point of the fixes is what they refuse, so expose them.
	using ByteValueStorage::createBytesFromValue;
	using ByteValueStorage::createValueFromBytes;
};

} // anonymous namespace

class ByteValueStorageKernelTests : public Test {};

// --- createValueFromBytes: the shift bound ---
//
// The loop was
//     value += data[offset + i]
//              << (getByteLength() * (endian == LITTLE ? i : realSize - i - 1));
// on a std::uint64_t accumulator, and realSize is `data.size() - offset` when
// the caller passes size 0 ("all bytes"). Nothing bounded it at 8. ESBMC's
// witness is dataSize = 10, offset = 0, size = 0, byteLength = 8: at i = 8 the
// expression is `(uint64)data[8] << 64`, reported as "arithmetic overflow on
// shl, !overflow("shl", (unsigned long int)(data[offset + i]), byteLength * i)".
// The witness sets data[8] = data[9] = 1, so the bits that fall off the end are
// real bits and not a discarded zero.

TEST_F(ByteValueStorageKernelTests,
CreateValueFromBytesRefusesMoreBytesThanTheAccumulatorHolds)
{
	FakeStorage storage;

	// Exactly the ESBMC witness: ten bytes, offset 0, size 0 meaning "all".
	std::vector<std::uint8_t> data(10, 0);
	data[8] = 1;
	data[9] = 1;

	std::uint64_t value = 0xDEADBEEF;
	EXPECT_FALSE(storage.createValueFromBytes(
			data, value, Endianness::LITTLE, 0, 0));
	// Refused means untouched, not half-assembled.
	EXPECT_EQ(std::uint64_t(0xDEADBEEF), value);

	EXPECT_FALSE(storage.createValueFromBytes(
			data, value, Endianness::BIG, 0, 0));
	// Nine bytes is already one too many for a 64-bit accumulator.
	EXPECT_FALSE(storage.createValueFromBytes(
			data, value, Endianness::LITTLE, 1, 9));
}

TEST_F(ByteValueStorageKernelTests,
CreateValueFromBytesStillDecodesTheOrdinaryWidths)
{
	FakeStorage storage;
	const std::vector<std::uint8_t> data = {
			0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

	std::uint64_t value = 0;
	ASSERT_TRUE(storage.createValueFromBytes(
			data, value, Endianness::LITTLE, 0, 0));
	EXPECT_EQ(std::uint64_t(0x8877665544332211), value);

	ASSERT_TRUE(storage.createValueFromBytes(
			data, value, Endianness::BIG, 0, 0));
	EXPECT_EQ(std::uint64_t(0x1122334455667788), value);

	// A sub-range, and an offset that is not zero.
	ASSERT_TRUE(storage.createValueFromBytes(
			data, value, Endianness::LITTLE, 2, 2));
	EXPECT_EQ(std::uint64_t(0x4433), value);
}

TEST_F(ByteValueStorageKernelTests,
CreateValueFromBytesRefusesASizeThatWrapsTheOffsetSum)
{
	FakeStorage storage;
	const std::vector<std::uint8_t> data = {0x01, 0x02, 0x03, 0x04};

	// `offset + size > data.size()` formed the sum first: at offset 1 and size
	// SIZE_MAX it wraps to 0, the test is false, `size` survives as the real
	// size, and the loop then indexes data[1 + i] for SIZE_MAX iterations.
	std::uint64_t value = 0;
	EXPECT_FALSE(storage.createValueFromBytes(
			data,
			value,
			Endianness::LITTLE,
			1,
			std::numeric_limits<std::uint64_t>::max()));
}

TEST_F(ByteValueStorageKernelTests,
CreateValueFromBytesRefusesAnOffsetPastTheEnd)
{
	FakeStorage storage;
	const std::vector<std::uint8_t> data = {0x01, 0x02};

	std::uint64_t value = 0;
	EXPECT_FALSE(storage.createValueFromBytes(
			data, value, Endianness::LITTLE, 2, 0));
	EXPECT_FALSE(storage.createValueFromBytes(
			data, value, Endianness::LITTLE, 64, 1));
}

TEST_F(ByteValueStorageKernelTests,
CreateValueFromBytesRefusesAnUnknownEndianness)
{
	FakeStorage storage;
	storage.endianness = Endianness::UNKNOWN;
	const std::vector<std::uint8_t> data = {0x01, 0x02};

	std::uint64_t value = 0;
	EXPECT_FALSE(storage.createValueFromBytes(
			data, value, Endianness::UNKNOWN, 0, 0));
}

// --- createBytesFromValue: the counter and the shift bound ---
//
// `for (std::uint8_t i = 0; i < x; ++i)` counted a std::uint64_t width the
// caller supplies. The counter cannot represent any index at or above 256.
// Asked whether the last index the loop must reach, x - 1, is representable in
// the counter's type, ESBMC answers x = 0x8000000000000008
// (9223372036854775816), for which x - 1 = 9223372036854775815 > 255:
// "assertion x - 1 <= 255u" violated.
//
// Every x above 8 is undefined for a second reason as well:
// `data >> (getByteLength() * i)` reaches a shift count of 64 at i = 8.

TEST_F(ByteValueStorageKernelTests,
CreateBytesFromValueRefusesAWidthWiderThanTheAccumulator)
{
	FakeStorage storage;
	std::vector<std::uint8_t> out;

	// Nine bytes is the first width whose last shift count is 64.
	EXPECT_FALSE(storage.createBytesFromValue(
			0x1122334455667788ull, 9, out, Endianness::LITTLE));
	EXPECT_TRUE(out.empty());

	EXPECT_FALSE(storage.createBytesFromValue(
			0x1122334455667788ull, 16, out, Endianness::BIG));
	EXPECT_TRUE(out.empty());

	// 255 is the largest width the old std::uint8_t counter could represent at
	// all, and it is still eight bytes past the end of the accumulator.
	EXPECT_FALSE(storage.createBytesFromValue(
			0x1122334455667788ull, 255, out, Endianness::LITTLE));
	EXPECT_TRUE(out.empty());
}

TEST_F(ByteValueStorageKernelTests,
CreateBytesFromValueRefusesAWidthThatWrapsTheCounter)
{
	// NOTE: with the defect present this test does not fail, it HANGS -- at
	// x = 256 the std::uint8_t counter wraps 255 -> 0 and the loop never
	// terminates. That is the defect. The width-wider-than-the-accumulator
	// test above is the one that fails cleanly on a revert; this one pins the
	// exact ESBMC witness.
	FakeStorage storage;
	std::vector<std::uint8_t> out;

	EXPECT_FALSE(storage.createBytesFromValue(
			0x1122334455667788ull, 256, out, Endianness::LITTLE));
	EXPECT_TRUE(out.empty());

	EXPECT_FALSE(storage.createBytesFromValue(
			0x1122334455667788ull,
			0x8000000000000008ull,
			out,
			Endianness::LITTLE));
	EXPECT_TRUE(out.empty());
}

TEST_F(ByteValueStorageKernelTests,
CreateBytesFromValueStillEmitsTheOrdinaryWidths)
{
	FakeStorage storage;
	std::vector<std::uint8_t> out;

	ASSERT_TRUE(storage.createBytesFromValue(
			0x1122334455667788ull, 8, out, Endianness::LITTLE));
	const std::vector<std::uint8_t> le = {
			0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
	EXPECT_EQ(le, out);

	ASSERT_TRUE(storage.createBytesFromValue(
			0x1122334455667788ull, 8, out, Endianness::BIG));
	const std::vector<std::uint8_t> be = {
			0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
	EXPECT_EQ(be, out);

	ASSERT_TRUE(storage.createBytesFromValue(
			0x1122334455667788ull, 2, out, Endianness::LITTLE));
	const std::vector<std::uint8_t> two = {0x88, 0x77};
	EXPECT_EQ(two, out);

	// A zero width emits nothing and always did; the old loop simply never ran.
	ASSERT_TRUE(storage.createBytesFromValue(
			0x1122334455667788ull, 0, out, Endianness::LITTLE));
	EXPECT_TRUE(out.empty());
}

// --- getNTWS / getNTWSNice: the walk must advance ---
//
// The loops ended with `address += width` and nothing rejected width == 0. A
// caller asking for a zero-width wide string re-reads the same non-zero element
// forever and pushes it into `tmp` on EVERY iteration -- an unbounded vector,
// not just a spin. ESBMC refutes `address + width > address` for the expression
// as written; the reachable case is simply width = 0, where address = 0x1000
// gives next = 0x1000, unchanged, and the wrapping case it also finds is
// width = 0x8000020C00200910 at address = 0x8000000405200103.
//
// The fake refuses after maxCalls answers, so the defect shows up here as a
// walk that collected maxCalls elements instead of stopping at one.

TEST_F(ByteValueStorageKernelTests, GetNTWSRefusesAZeroWidthWalk)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 64;

	std::vector<std::uint64_t> res;
	EXPECT_FALSE(storage.getNTWS(0x1000, 0, res));
	// One element read, then the refusal to advance stops the walk.
	EXPECT_EQ(std::size_t(1), res.size());
	EXPECT_EQ(std::size_t(1), storage.calls);
	// And the cursor never moved off the address it was given.
	ASSERT_EQ(std::size_t(1), storage.addressesSeen.size());
	EXPECT_EQ(std::uint64_t(0x1000), storage.addressesSeen[0]);
}

TEST_F(ByteValueStorageKernelTests,
GetNTWSDoesNotWalkPastTheTopOfTheAddressSpace)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 64;

	// `address += width` here wraps to 0x0FF and the walk carries on reading
	// from the bottom of the address space as though nothing happened.
	std::vector<std::uint64_t> res;
	EXPECT_FALSE(storage.getNTWS(
			std::numeric_limits<std::uint64_t>::max() - 0xFF, 0x1000, res));
	EXPECT_EQ(std::size_t(1), res.size());
	EXPECT_EQ(std::size_t(1), storage.calls);
}

TEST_F(ByteValueStorageKernelTests, GetNTWSStillWalksAndTerminatesOnNul)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 3;

	std::vector<std::uint64_t> res;
	ASSERT_TRUE(storage.getNTWS(0x1000, 2, res));
	const std::vector<std::uint64_t> expected = {0x41, 0x41, 0x41, 0x00};
	EXPECT_EQ(expected, res);
	// Each step advanced by exactly the width.
	const std::vector<std::uint64_t> addresses = {0x1000, 0x1002, 0x1004, 0x1006};
	EXPECT_EQ(addresses, storage.addressesSeen);
}

TEST_F(ByteValueStorageKernelTests, GetNTWSNiceRefusesAZeroWidthWalk)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 64;

	std::vector<std::uint64_t> res;
	// One element is not a string either way; what matters is that the walk
	// stopped instead of collecting all 64 answers.
	EXPECT_FALSE(storage.getNTWSNice(0x1000, 0, res));
	EXPECT_EQ(std::size_t(1), res.size());
	EXPECT_EQ(std::size_t(1), storage.calls);
}

TEST_F(ByteValueStorageKernelTests, GetNTWSNiceStillWalksAndTerminatesOnNul)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 3;

	std::vector<std::uint64_t> res;
	ASSERT_TRUE(storage.getNTWSNice(0x1000, 2, res));
	const std::vector<std::uint64_t> expected = {0x41, 0x41, 0x41, 0x00};
	EXPECT_EQ(expected, res);
}

TEST_F(ByteValueStorageKernelTests, GetNTWSNiceStopsAtACharacterThatIsNotNice)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 64;
	storage.element = 0x110000; // far above 0xFF, so not a nice ASCII wide char

	std::vector<std::uint64_t> res;
	EXPECT_FALSE(storage.getNTWSNice(0x1000, 2, res));
	EXPECT_EQ(std::size_t(1), storage.calls);
}

} // namespace tests
} // namespace utils
} // namespace retdec
