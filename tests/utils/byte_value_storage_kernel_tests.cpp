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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retdec/utils/byte_value_storage.h"
#include "retdec/utils/conversion.h"
#include "retdec/utils/system.h"

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
	/// getBytesPerWord/getNumberOfNibblesInByte are virtual, and hexToBig
	/// multiplies them together, so a fake has to be able to report a pair
	/// whose product is not representable.
	std::size_t bytesPerWord = 4;
	std::size_t nibblesInByte = 2;

	/// Answers given before getXByte starts refusing.
	std::size_t maxCalls = 64;
	/// Answers given before getXByte starts returning the NUL terminator.
	std::size_t nonZeroCalls = 64;
	/// The non-terminator element; 'A', which isNiceAsciiWideCharacter accepts.
	std::uint64_t element = 0x41;

	/// What getXBytes answers, and whether it answers at all. getXBytes is
	/// pure virtual, so the number of bytes a format hands back is entirely up
	/// to the format: the two implementations in this tree enforce
	/// res.size() == x, but nothing in the interface obliges them to, and the
	/// decoders that consume the result have to say so themselves.
	bool xBytesOk = false;
	std::vector<std::uint8_t> xBytesAnswer;

	mutable std::size_t calls = 0;
	mutable std::vector<std::uint64_t> addressesSeen;

	Endianness getEndianness() const override { return endianness; }
	std::size_t getNibbleLength() const override { return 4; }
	std::size_t getByteLength() const override { return byteLength; }
	std::size_t getWordLength() const override { return 32; }
	std::size_t getBytesPerWord() const override { return bytesPerWord; }
	std::size_t getNumberOfNibblesInByte() const override
	{
		return nibblesInByte;
	}
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

	/// Every address getXBytes was asked for, in order.
	///
	/// getFloat, getDouble and get10Byte all read through getXBytes rather than
	/// getXByte, so the array walkers built on them are invisible in
	/// addressesSeen. Recording here is what makes the stride and the wrap
	/// observable -- and a walker's step IS its address sequence, so a test
	/// that cannot see the sequence cannot test the step.
	mutable std::vector<std::uint64_t> xBytesAddressesSeen;
	/// Answers given before getXBytes starts refusing, so a walk that does not
	/// terminate fails the test instead of hanging it.
	std::size_t maxXBytesCalls = 64;

	bool getXBytes(
			std::uint64_t address,
			std::uint64_t x,
			std::vector<std::uint8_t>& res) const override
	{
		if (xBytesAddressesSeen.size() >= maxXBytesCalls)
		{
			return false;
		}
		xBytesAddressesSeen.push_back(address);
		if (!xBytesOk)
		{
			return false;
		}
		// The canned answer, verbatim, whatever width it is. Substituting a
		// correctly-sized one here would be helpful and wrong: a format is not
		// obliged to return x bytes, several tests below exist precisely to
		// check that a caller notices when it does not, and a fake that
		// silently corrects the width would pass them against any caller at
		// all. `x` is unused for that reason.
		(void) x;
		res = xBytesAnswer;
		return true;
	}

	bool setXByte(
			std::uint64_t,
			std::uint64_t,
			std::uint64_t,
			Endianness) override
	{
		return false;
	}

	/// Whether setXBytes accepts, and where it records what it was handed.
	/// set10Byte's whole contract is which bytes reach the format, so a fake
	/// that discards them cannot test it.
	bool setXBytesOk = false;
	std::vector<std::uint8_t>* setXBytesSeen = nullptr;

	bool setXBytes(
			std::uint64_t,
			const std::vector<std::uint8_t>& data) override
	{
		if (!setXBytesOk)
		{
			return false;
		}
		if (setXBytesSeen != nullptr)
		{
			*setXBytesSeen = data;
		}
		return true;
	}

	// createValueFromBytes and createBytesFromValue are protected; the whole
	// point of the fixes is what they refuse, so expose them.
	using ByteValueStorage::createBytesFromValue;
	using ByteValueStorage::extendedBytesFor;
	using ByteValueStorage::createValueFromBytes;
};

/**
* @brief Run @a body on another thread; answer whether it returned in time.
*
* FakeStorage's maxCalls ceiling makes every walk-shaped defect in this file
* show up as a wrong answer rather than a hang, because a walk has to ask the
* fake for the next element. createBytesFromValue's counter defect is the one
* shape that ceiling cannot reach: the loop calls nothing, so with the defect
* present it spins inside the function under test with no way out. A test that
* hangs is not a regression test -- it turns a returning defect into a CI
* timeout with no message instead of a named failure -- so the call is made
* somewhere it can be abandoned.
*
* Everything the body touches is heap-allocated and owned by the closure, so a
* body that never returns keeps writing to memory that is still alive rather
* than to a destroyed test frame. The thread is detached and never joined:
* returning from main() calls exit(), which does not wait for detached threads,
* so a stuck body costs the run one spinning thread and nothing else.
*/
template<typename Fn>
bool completesWithin(std::chrono::milliseconds timeout, Fn body)
{
	auto done = std::make_shared<std::atomic<bool>>(false);
	auto owned = std::make_shared<Fn>(std::move(body));
	std::thread([done, owned]() {
		(*owned)();
		done->store(true);
	}).detach();

	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!done->load() && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return done->load();
}

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

// x = 256 is the first width the std::uint8_t counter cannot represent: it
// wraps 255 -> 0 and the loop never terminates. With the defect present a
// direct call therefore never comes back, so it is made on a thread this test
// can walk away from -- see completesWithin above. The failure the revert
// produces is a named one ("did not return within ..."), which is the whole
// point: a hang in a suite that a human or CI runs is a timeout with no
// message attached to it.
TEST_F(ByteValueStorageKernelTests,
CreateBytesFromValueRefusesAWidthThatWrapsTheCounter)
{
	// Owned by the closure rather than by this frame: if the width counter
	// wraps again, the abandoned thread goes on writing into `out` long after
	// this test has returned.
	auto storage = std::make_shared<FakeStorage>();
	auto out = std::make_shared<std::vector<std::uint8_t>>();
	auto accepted = std::make_shared<std::atomic<bool>>(false);

	const auto attempt = [storage, out, accepted](std::uint64_t x) {
		return [storage, out, accepted, x]() {
			accepted->store(storage->createBytesFromValue(
					0x1122334455667788ull, x, *out, Endianness::LITTLE));
		};
	};

	ASSERT_TRUE(completesWithin(std::chrono::seconds(5), attempt(256)))
			<< "createBytesFromValue did not return for a width of 256: the "
			   "width counter cannot represent 256, so it wrapped to 0 and the "
			   "loop never reached its bound";
	EXPECT_FALSE(accepted->load());
	EXPECT_TRUE(out->empty());

	// The exact ESBMC witness: x - 1 = 9223372036854775815, which is not
	// representable in the counter's type either.
	ASSERT_TRUE(completesWithin(
			std::chrono::seconds(5), attempt(0x8000000000000008ull)))
			<< "createBytesFromValue did not return for a width of "
			   "0x8000000000000008";
	EXPECT_FALSE(accepted->load());
	EXPECT_TRUE(out->empty());
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

// --- getXByteArray: the third walk, and the only one that is public API ---
//
// It ended with `address += x`, the same expression the two NTWS walks used to
// end with, and x is a parameter its callers choose. Unlike those two it is
// bounded by `size`, so a zero step is not an unbounded vector -- it is `size`
// reads of one address reported as an array of `size` distinct elements.

TEST_F(ByteValueStorageKernelTests, GetXByteArrayRefusesAZeroWidthStep)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 64;

	std::vector<std::uint64_t> res;
	// With `address += 0` this returned true having read address 0x1000 four
	// times and pushed the same element four times.
	EXPECT_FALSE(storage.getXByteArray(0x1000, 0, res, 4));
	EXPECT_EQ(std::size_t(1), storage.calls);
	const std::vector<std::uint64_t> addresses = {0x1000};
	EXPECT_EQ(addresses, storage.addressesSeen);
}

TEST_F(ByteValueStorageKernelTests,
GetXByteArrayDoesNotWalkPastTheTopOfTheAddressSpace)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 64;

	// Measured against the unguarded loop: it returned true having read
	// 0xffffffffffffff00, 0xf00, 0x1f00 and 0x2f00 -- three of the four reads
	// came from the bottom of the address space after the sum wrapped.
	std::vector<std::uint64_t> res;
	EXPECT_FALSE(storage.getXByteArray(
			std::numeric_limits<std::uint64_t>::max() - 0xFF, 0x1000, res, 4));
	const std::vector<std::uint64_t> addresses = {
			std::numeric_limits<std::uint64_t>::max() - 0xFF};
	EXPECT_EQ(addresses, storage.addressesSeen);
}

TEST_F(ByteValueStorageKernelTests, GetXByteArrayStillReadsAnOrdinaryArray)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 64;

	std::vector<std::uint64_t> res;
	ASSERT_TRUE(storage.getXByteArray(0x1000, 4, res, 3));
	const std::vector<std::uint64_t> expected = {0x41, 0x41, 0x41};
	EXPECT_EQ(expected, res);
	const std::vector<std::uint64_t> addresses = {0x1000, 0x1004, 0x1008};
	EXPECT_EQ(addresses, storage.addressesSeen);

	// A size of zero reads nothing and succeeds, as it always did.
	storage.addressesSeen.clear();
	res.clear();
	EXPECT_TRUE(storage.getXByteArray(0x1000, 0, res, 0));
	EXPECT_TRUE(res.empty());
	EXPECT_TRUE(storage.addressesSeen.empty());

	// A single element needs no step at all, so a read that ends exactly at the
	// top of the address space is still a legal read.
	storage.addressesSeen.clear();
	res.clear();
	EXPECT_TRUE(storage.getXByteArray(
			std::numeric_limits<std::uint64_t>::max(), 8, res, 1));
	EXPECT_EQ(std::size_t(1), res.size());
}

// --- getNTBS: the same walk with a step of one ---
//
// `get1ByteFn(++address, ...)` always advances, so this was never the zero-step
// spin, but it wraps: at UINT64_MAX the next read is at 0 and the scan carries
// on from the bottom of the address space.

TEST_F(ByteValueStorageKernelTests,
GetNTBSDoesNotWalkPastTheTopOfTheAddressSpace)
{
	FakeStorage storage;
	storage.maxCalls = 8;
	storage.nonZeroCalls = 8;

	std::string res;
	// One byte was read and it was not a terminator, so this is still a
	// non-empty result; what changed is that the walk stopped at the top of
	// the address space instead of continuing at 0, 1, 2 ... until the fake ran
	// out of answers.
	EXPECT_TRUE(storage.getNTBS(
			std::numeric_limits<std::uint64_t>::max(), res));
	EXPECT_EQ(std::string("A"), res);
	const std::vector<std::uint64_t> addresses = {
			std::numeric_limits<std::uint64_t>::max()};
	EXPECT_EQ(addresses, storage.addressesSeen);
}

TEST_F(ByteValueStorageKernelTests, GetNTBSStillWalksAndTerminatesOnNul)
{
	FakeStorage storage;
	storage.maxCalls = 64;
	storage.nonZeroCalls = 3;

	std::string res;
	ASSERT_TRUE(storage.getNTBS(0x1000, res));
	EXPECT_EQ(std::string("AAA"), res);
	const std::vector<std::uint64_t> addresses = {
			0x1000, 0x1001, 0x1002, 0x1003};
	EXPECT_EQ(addresses, storage.addressesSeen);

	// And the fixed-size form still stops after exactly `size` bytes.
	FakeStorage sized;
	sized.maxCalls = 64;
	sized.nonZeroCalls = 64;
	std::string two;
	ASSERT_TRUE(sized.getNTBS(0x2000, two, 2));
	EXPECT_EQ(std::string("AA"), two);
	EXPECT_EQ(std::size_t(2), sized.calls);
}

// --- bitsToBig / bitsToLittle: the bit-reversal mask ---
//
// The mask was built with `unsigned char a = 1; a <<= (sizeof(a) * items) - 1;`
// where items is getByteLength(). sizeof(a) is 1, so the count is items - 1 and
// `a` promotes to int: at getByteLength() = 64 that is a shift of 63 on a
// 32-bit int, which UBSan reports as "shift exponent 63 is too large for
// 32-bit type int". For 9..32 there is no undefined behaviour and no answer
// either -- the bit does not survive the narrowing back to unsigned char, so
// the reversal loop never runs and every element is silently set to 0.

TEST_F(ByteValueStorageKernelTests,
BitsToBigRefusesAByteLengthWiderThanTheElement)
{
	FakeStorage storage;
	storage.endianness = Endianness::LITTLE; // so the swap is actually done
	storage.byteLength = 64;

	std::vector<unsigned char> values = {0xA0, 0x01};
	EXPECT_FALSE(storage.bitsToBig(values));
	// Refused means untouched. The old body left {0x00, 0x00} here.
	const std::vector<unsigned char> unchanged = {0xA0, 0x01};
	EXPECT_EQ(unchanged, values);

	// Nine is the first width past the element, and it is the silent-zero case
	// rather than the undefined-shift one.
	storage.byteLength = 9;
	values = {0xA0, 0x01};
	EXPECT_FALSE(storage.bitsToBig(values));
	EXPECT_EQ(unchanged, values);

	// bitsToLittle is the same swap reached from the other side: it converts
	// only when the storage is big endian, so a big-endian fake is what
	// exercises it.
	FakeStorage fromBig;
	fromBig.endianness = Endianness::BIG;
	fromBig.byteLength = 64;
	values = {0xA0, 0x01};
	EXPECT_FALSE(fromBig.bitsToLittle(values));
	EXPECT_EQ(unchanged, values);
}

TEST_F(ByteValueStorageKernelTests, BitsToBigStillReversesEachElement)
{
	FakeStorage storage;
	storage.endianness = Endianness::LITTLE;
	storage.byteLength = 8;

	// 0xA0 is 1010'0000; reversed it is 0000'0101 = 0x05.
	std::vector<unsigned char> values = {0xA0, 0x01};
	ASSERT_TRUE(storage.bitsToBig(values));
	const std::vector<unsigned char> reversed = {0x05, 0x80};
	EXPECT_EQ(reversed, values);

	// Already big endian: nothing to do, and nothing done.
	FakeStorage big;
	big.endianness = Endianness::BIG;
	std::vector<unsigned char> kept = {0xA0};
	ASSERT_TRUE(big.bitsToBig(kept));
	EXPECT_EQ(std::vector<unsigned char>({0xA0}), kept);
}

// --- hexToBig: the word size is a product of two virtual-call results ---
//
// `str.size() < items * length` formed the product before testing it, and the
// same wrapped value was then used as the modulus and as the loop stride. With
// getBytesPerWord() = 0x8000000000000001 and getNumberOfNibblesInByte() = 2 the
// product is 2: the guard passes on any string of two characters or more, and
// the inner loop then indexes str at `(items - j) * length - k - 1`, which for
// j = 1 is 0 - 1 = SIZE_MAX. Reverting the mulFits check does not make this
// test report a wrong value, it makes the suite fault -- which is what the
// defect is.

TEST_F(ByteValueStorageKernelTests, HexToBigRefusesAWordSizeWhoseProductWraps)
{
	FakeStorage storage;
	storage.endianness = Endianness::LITTLE; // so the swap is actually done
	storage.bytesPerWord = 0x8000000000000001ull;
	storage.nibblesInByte = 2;

	std::string str = "0123456789abcdef";
	EXPECT_FALSE(storage.hexToBig(str));
	EXPECT_EQ(std::string("0123456789abcdef"), str);

	// The other shape the wrap takes: a product of exactly zero, which the old
	// body then used as the modulus of `str.size() % (items * length)`.
	storage.bytesPerWord = 0x8000000000000000ull;
	storage.nibblesInByte = 2;
	std::string other = "0123456789abcdef";
	EXPECT_FALSE(storage.hexToBig(other));
	EXPECT_EQ(std::string("0123456789abcdef"), other);

	// hexToLittle is the same swap reached from the other side: it converts
	// only when the storage is big endian.
	FakeStorage fromBig;
	fromBig.endianness = Endianness::BIG;
	fromBig.bytesPerWord = 0x8000000000000001ull;
	fromBig.nibblesInByte = 2;
	std::string third = "0123456789abcdef";
	EXPECT_FALSE(fromBig.hexToLittle(third));
	EXPECT_EQ(std::string("0123456789abcdef"), third);
}

TEST_F(ByteValueStorageKernelTests, HexToBigStillSwapsAnOrdinaryWord)
{
	FakeStorage storage;
	storage.endianness = Endianness::LITTLE;
	storage.bytesPerWord = 4;
	storage.nibblesInByte = 2;

	// Four bytes of two nibbles each: "aabbccdd" becomes "ddccbbaa".
	std::string str = "aabbccdd";
	ASSERT_TRUE(storage.hexToBig(str));
	EXPECT_EQ(std::string("ddccbbaa"), str);
}

// --- get10Byte: the width getXBytes answered with is not the width asked for
//
// getXBytes is pure virtual. getFloatImpl and getDoubleImpl both refuse a data
// vector that is not exactly the width they decode; get10ByteImpl copied
// data.size() bytes into a long double and handed the same vector to
// double10ToDouble8, which subscripts it at 9. On a four-byte answer ASan
// reports "heap-buffer-overflow ... READ of size 1 ... 5 bytes after 4-byte
// region"; without a sanitizer it simply returns true with most of `res` never
// written.

TEST_F(ByteValueStorageKernelTests, Get10ByteRefusesAShortAnswerFromGetXBytes)
{
	FakeStorage storage;
	storage.xBytesOk = true;
	storage.xBytesAnswer = {0x01, 0x02, 0x03, 0x04};

	long double res = 42.0L;
	EXPECT_FALSE(storage.get10Byte(0x1000, res));
	// Refused means untouched.
	EXPECT_TRUE(res == 42.0L);

	storage.xBytesAnswer.clear();
	EXPECT_FALSE(storage.get10Byte(0x1000, res));

	// Longer than ten is refused too: it is not the datum that was asked for.
	storage.xBytesAnswer.assign(16, 0x00);
	EXPECT_FALSE(storage.get10Byte(0x1000, res));
}

TEST_F(ByteValueStorageKernelTests, Get10ByteStillDecodesTenBytes)
{
	FakeStorage storage;
	storage.xBytesOk = true;
	// The 80-bit extended-precision encoding of 3.789, the same pattern
	// conversion_tests.cpp pins for double10ToDouble8.
	storage.xBytesAnswer = {
			0x60, 0xe5, 0xd0, 0x22, 0xdb, 0xf9, 0x7e, 0xf2, 0x00, 0x40};

	long double res = 0.0L;
	ASSERT_TRUE(storage.get10Byte(0x1000, res));
	EXPECT_NEAR(3.789, static_cast<double>(res), 1e-9);
}


// ─── the three array walks the first sweep missed ───────────────────────────
//
// getXByteArray, getNTWSImpl and getNTWSNiceImpl were given a scan::Cursor and
// the comment above getXByteArray said it was "the third of the three address
// walks in this file". It was not: get10ByteArray, getFloatArray and
// getDoubleArray are three more, all public API, and the adversarial verify
// pass measured every one of them walking off the top of the address space and
// returning true. getDoubleArray carried a second defect that is a plain wrong
// answer rather than a bound.

/// getDoubleArray stepped by `sizeof(float)` in a loop that reads DOUBLES.
///
/// Asked for four doubles at 0x1000 it read 0x1000, 0x1004, 0x1008 and 0x100c,
/// so every element after the first overlapped its predecessor by four bytes
/// and three of the four answers were assembled from the wrong bytes. It
/// returned true, so no caller could tell.
///
/// The addresses are the assertion. A step is an address sequence, and a test
/// that only inspected the returned values would pass against any stride that
/// happens to read in-range bytes.
TEST_F(ByteValueStorageKernelTests,
GetDoubleArrayStepsByTheWidthOfADouble) {
	FakeStorage storage;
	storage.xBytesOk = true;
	storage.xBytesAnswer.assign(sizeof(double), 0);

	std::vector<double> res;
	ASSERT_TRUE(storage.getDoubleArray(0x1000, res, 4));
	ASSERT_EQ(4u, res.size());

	ASSERT_EQ(4u, storage.xBytesAddressesSeen.size());
	EXPECT_EQ(0x1000u, storage.xBytesAddressesSeen[0]);
	EXPECT_EQ(0x1008u, storage.xBytesAddressesSeen[1]);
	EXPECT_EQ(0x1010u, storage.xBytesAddressesSeen[2]);
	EXPECT_EQ(0x1018u, storage.xBytesAddressesSeen[3]);
}

/// The float walk reads four bytes at a time, so its stride is genuinely
/// sizeof(float) -- stated so the fix above cannot be "corrected" into
/// striding both arrays by eight.
TEST_F(ByteValueStorageKernelTests,
GetFloatArrayStepsByTheWidthOfAFloat) {
	FakeStorage storage;
	storage.xBytesOk = true;
	storage.xBytesAnswer.assign(sizeof(float), 0);

	std::vector<float> res;
	ASSERT_TRUE(storage.getFloatArray(0x2000, res, 3));
	ASSERT_EQ(3u, storage.xBytesAddressesSeen.size());
	EXPECT_EQ(0x2000u, storage.xBytesAddressesSeen[0]);
	EXPECT_EQ(0x2004u, storage.xBytesAddressesSeen[1]);
	EXPECT_EQ(0x2008u, storage.xBytesAddressesSeen[2]);
}

/// And the ten-byte walk, whose step is the x87 extended width.
TEST_F(ByteValueStorageKernelTests,
Get10ByteArrayStepsByTheExtendedWidth) {
	FakeStorage storage;
	storage.xBytesOk = true;
	storage.xBytesAnswer.assign(10, 0);

	std::vector<long double> res;
	ASSERT_TRUE(storage.get10ByteArray(0x3000, res, 3));
	ASSERT_EQ(3u, storage.xBytesAddressesSeen.size());
	EXPECT_EQ(0x3000u, storage.xBytesAddressesSeen[0]);
	EXPECT_EQ(0x300Au, storage.xBytesAddressesSeen[1]);
	EXPECT_EQ(0x3014u, storage.xBytesAddressesSeen[2]);
}

/// None of the three may walk off the top of the address space and carry on
/// from the bottom. Measured before the fix: getFloatArray(UINT64_MAX - 0x07,
/// res, 4) returned TRUE having read 0xfffffffffffffff8, 0xfffffffffffffffc,
/// 0x0 and 0x4 -- the last two being the bottom of the address space, which is
/// a different part of the file entirely.
TEST_F(ByteValueStorageKernelTests,
ArrayWalksRefuseRatherThanWrapPastTheTopOfTheAddressSpace) {
	const std::uint64_t top = std::numeric_limits<std::uint64_t>::max();

	{
		FakeStorage storage;
		storage.xBytesOk = true;
		storage.xBytesAnswer.assign(sizeof(float), 0);
		std::vector<float> res;
		EXPECT_FALSE(storage.getFloatArray(top - 0x07, res, 4));
	}
	{
		FakeStorage storage;
		storage.xBytesOk = true;
		storage.xBytesAnswer.assign(sizeof(double), 0);
		std::vector<double> res;
		EXPECT_FALSE(storage.getDoubleArray(top - 0x0F, res, 4));
	}
	{
		FakeStorage storage;
		storage.xBytesOk = true;
		storage.xBytesAnswer.assign(10, 0);
		std::vector<long double> res;
		EXPECT_FALSE(storage.get10ByteArray(top - 0x0F, res, 4));
	}
}


/// set10Byte was the un-inverted twin of get10ByteImpl: on a host without a
/// 10-byte long double it wrote the first eight bytes of the long double's
/// OBJECT REPRESENTATION, which is not an encoding of anything. get10Byte on
/// such a host reads ten stored bytes as an x87 extended datum, so a write
/// followed by a read did not return the value written.
///
/// The host answer is a PARAMETER here, not a call to systemHasLongDouble().
/// That is the point of the test: `sizeof(long double) >= 10` is a compile-time
/// constant per host and is always true on x86-64, so with the decision inside
/// the function the broken path could not be reached from a test at all -- and
/// a first version of this test, written that way, passed against the old body.
TEST_F(ByteValueStorageKernelTests,
ExtendedBytesAreWrittenOnlyWhereTheHostHasTheDatum) {
	std::vector<std::uint8_t> out;

	// A host with the 80-bit datum: ten bytes of it, which is exactly what
	// get10ByteImpl memcpys back.
	EXPECT_TRUE(FakeStorage::extendedBytesFor(1.5L, true, out));
	EXPECT_EQ(10u, out.size());

	// A host without it: refused, and nothing written. The old body wrote eight
	// bytes here and returned whatever setXBytes said.
	out.assign(3, 0xAA);
	EXPECT_FALSE(FakeStorage::extendedBytesFor(1.5L, false, out));
	EXPECT_TRUE(out.empty());
}

/// And the round trip on this host, which is the property the encoding is for.
TEST_F(ByteValueStorageKernelTests,
Set10ByteRoundTripsThroughGet10Byte) {
	if (!systemHasLongDouble()) {
		GTEST_SKIP() << "this host has no 80-bit long double, so there is no "
		                "datum to round-trip; extendedBytesFor refuses and the "
		                "test above covers that";
	}

	FakeStorage storage;
	std::vector<std::uint8_t> written;
	storage.setXBytesOk = true;
	storage.setXBytesSeen = &written;

	const long double value = 1.5L;
	ASSERT_TRUE(storage.set10Byte(0x1000, value));
	ASSERT_EQ(10u, written.size());

	// Feed exactly those bytes back through the reader.
	storage.xBytesOk = true;
	storage.xBytesAnswer = written;
	long double back = 0.0L;
	ASSERT_TRUE(storage.get10Byte(0x1000, back));
	EXPECT_EQ(value, back);
}

} // namespace tests
} // namespace utils
} // namespace retdec
