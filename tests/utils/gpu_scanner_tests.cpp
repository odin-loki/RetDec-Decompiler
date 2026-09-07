/**
* @file tests/utils/gpu_scanner_tests.cpp
* @brief Tests for the @c GpuScanner module.
* @copyright (c) 2025-2026 Odin Loch trading as Imortek
*
* These cover the two public entry points that take numbers a caller supplied
* and turn them into buffer arithmetic: uploadFile, which takes a raw pointer
* and a length as two independent arguments, and batchMatch, which turns a byte
* range into a nibble range by doubling both ends.
*
* Which implementation they are testing matters here. src/utils/CMakeLists.txt
* builds retdec-gpu-scanner from src/utils/gpu_scanner.cu when a CUDA compiler
* is found and from src/utils/gpu_scanner_cpu.cpp when one is not, and for a
* while the two files carried separate copies of that arithmetic: a fix landed
* in the CPU file, the CUDA file kept the original wrong answers, and no test
* in the tree could see it, because a machine without nvcc cannot build a .cu
* at all. The host-side arithmetic now lives once, in gpu_scanner.cu behind
* RETDEC_GPU_SCANNER_HOST_ONLY, and gpu_scanner_cpu.cpp includes it. The
* include below is the same one, so the assertions in this file are about the
* text a CUDA build compiles and not about a copy of it.
*/

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retdec/utils/gpu_scanner.h"

#define RETDEC_GPU_SCANNER_HOST_ONLY 1
#include "../../src/utils/gpu_scanner.cu"

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {

class GpuScannerTests : public Test {};

/// A file that really was uploaded still scans, so the guards below are not
/// just refusing everything.
TEST_F(GpuScannerTests,
UploadedFileIsScannable) {
	const std::vector<std::uint8_t> bytes = {0xDE, 0xAD, 0xBE, 0xEF};

	GpuScanner scanner;
	scanner.uploadFile(bytes.data(), bytes.size());

	// "dead" is the nibble string of the first two bytes, so it matches at
	// offset 0 with every nibble in agreement.
	const std::vector<std::string> patterns = {"dead"};
	const auto results = scanner.batchMatch(patterns);

	ASSERT_EQ(1u, results.size());
	EXPECT_TRUE(results[0].matched);
	EXPECT_EQ(0u, results[0].offset);
	EXPECT_EQ(results[0].totalNibs, results[0].sameNibs);

	EXPECT_TRUE(scanner.fileEntropy() > 0.0);

	const std::vector<std::uint8_t> needle = {0xBE, 0xEF};
	const auto found = scanner.findAll(needle);
	ASSERT_EQ(1u, found.size());
	EXPECT_EQ(2u, found[0]);
}

/// uploadFile used to run `impl_->h_fileBytes.assign(data, data + size)` and
/// `impl_->h_fileNibs.resize(size * 2)` on whatever it was handed.
///
/// ESBMC witness: size = 18446744073709551615. The nibble buffer is two hex
/// characters per byte, so `size * 2` wraps to 18446744073709551614 -- one
/// below the byte count, where it should have been twice it. resize() gets
/// fewer nibbles than there are bytes while the loop writes at i*2 and i*2+1
/// for every i < size, so it leaves the string almost at once. And
/// `assign(data, data + size)` on a four-byte array asks for a range of
/// 18446744073709551615 elements, which throws std::length_error out of a void
/// function that no caller wraps in a try block. Both are refusals the
/// function is now able to make: bounds::mulFits is asked before anything is
/// touched.
TEST_F(GpuScannerTests,
UploadRefusesASizeWhoseNibbleCountWouldWrap) {
	const std::uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};

	GpuScanner scanner;
	EXPECT_NO_THROW(scanner.uploadFile(bytes, std::numeric_limits<std::size_t>::max()));

	// A refused upload leaves an empty scanner, which is what every caller
	// already handles for a file it could not read.
	const std::vector<std::string> patterns = {"dead"};
	const auto results = scanner.batchMatch(patterns);
	ASSERT_EQ(1u, results.size());
	EXPECT_FALSE(results[0].matched);
	EXPECT_EQ(0.0, scanner.fileEntropy());

	// SIZE_MAX / 2 + 1 is the smallest size whose doubling wraps, so it is the
	// boundary the check has to sit exactly on.
	const std::size_t firstWrappingSize =
			std::numeric_limits<std::size_t>::max() / 2 + 1;
	EXPECT_NO_THROW(scanner.uploadFile(bytes, firstWrappingSize));
	EXPECT_EQ(0.0, scanner.fileEntropy());
}

/// The same call had no null check at all -- unlike conversion.h's
/// bytesToHexString, which does. `assign(nullptr, nullptr + 8)` allocated eight
/// bytes and then copied from address 0.
TEST_F(GpuScannerTests,
UploadRefusesANullPointerWithANonZeroSize) {
	GpuScanner scanner;
	scanner.uploadFile(nullptr, 8);

	const std::vector<std::string> patterns = {"dead"};
	const auto results = scanner.batchMatch(patterns);
	ASSERT_EQ(1u, results.size());
	EXPECT_FALSE(results[0].matched);
	EXPECT_EQ(0.0, scanner.fileEntropy());
	EXPECT_TRUE(scanner.findAll({0xDE}).empty());
}

/// An empty upload is legitimate -- a zero-byte file -- and must not be turned
/// into a refusal by the null check.
TEST_F(GpuScannerTests,
UploadOfAnEmptyFileIsNotAnError) {
	GpuScanner scanner;
	scanner.uploadFile(nullptr, 0);
	EXPECT_EQ(0.0, scanner.fileEntropy());

	const std::vector<std::uint8_t> empty;
	scanner.uploadFile(empty.data(), empty.size());
	EXPECT_EQ(0.0, scanner.fileEntropy());
}

/// A refused upload must not leave the bytes of the previous one behind, or a
/// caller that checks the results of the second upload would be reading the
/// first file.
TEST_F(GpuScannerTests,
RefusedUploadDiscardsThePreviousFile) {
	const std::vector<std::uint8_t> bytes = {0xDE, 0xAD, 0xBE, 0xEF};

	GpuScanner scanner;
	scanner.uploadFile(bytes.data(), bytes.size());
	ASSERT_TRUE(scanner.fileEntropy() > 0.0);

	scanner.uploadFile(nullptr, 8);
	EXPECT_EQ(0.0, scanner.fileEntropy());

	const std::vector<std::string> patterns = {"dead"};
	const auto results = scanner.batchMatch(patterns);
	ASSERT_EQ(1u, results.size());
	EXPECT_FALSE(results[0].matched);
}


/// fileEntropy computed `sz = hi - lo + 1` before anything checked that lo was
/// at or below hi. For a startOffset past the end of the file the subtraction
/// underflows to a number near SIZE_MAX, the histogram loop does not run, and
/// every `hist[b] / sz` is 0 -- so the function returned 0.0.
///
/// 0.0 is not a neutral answer here. It is the strongest possible statement
/// about a range: perfectly uniform, one byte value repeated. An invalid range
/// was making it, and entropy is what decides whether a region looks packed.
/// The range check and fpred::entropyBits' refusal of a histogram that does not
/// sum to its declared total are the two halves of the fix.
///
/// Which half this test pins, precisely: fpred::entropyBits' refusal. Removing
/// gpuscan::byteWindow's `startOffset > stop` check on its own leaves every
/// assertion below passing, because the histogram is empty either way and
/// entropyBits then rejects the total it was handed. The window check is pinned
/// instead by TheByteWindowRefusesARangeThatSelectsNothing, which calls it
/// directly -- and it is not redundant, because gpu_scanner.cu's fileEntropy
/// divides by that length itself rather than going through entropyBits.
TEST_F(GpuScannerTests,
EntropyOfAnOutOfRangeWindowIsNotAMeasurement) {
	const std::vector<std::uint8_t> bytes = {0xDE, 0xAD, 0xBE, 0xEF};

	GpuScanner scanner;
	scanner.uploadFile(bytes.data(), bytes.size());

	// A four-byte file with four distinct values: two bits of entropy exactly.
	ASSERT_TRUE(scanner.fileEntropy() > 1.9);

	// Every window that starts past the end. The old body took the underflow
	// branch for each of these and answered 0.0.
	EXPECT_EQ(0.0, scanner.fileEntropy(4));
	EXPECT_EQ(0.0, scanner.fileEntropy(5));
	EXPECT_EQ(0.0, scanner.fileEntropy(1000));
	EXPECT_EQ(0.0, scanner.fileEntropy(std::numeric_limits<std::size_t>::max() - 1));

	// And a window that starts after the stop offset, which underflows the
	// same way without leaving the file.
	EXPECT_EQ(0.0, scanner.fileEntropy(3, 1));

	// A real single-byte window is genuinely zero-entropy, and must still be
	// answerable -- the fix must not turn a valid measurement into a refusal.
	EXPECT_EQ(0.0, scanner.fileEntropy(0, 0));

	// A valid two-byte window of two distinct values is exactly one bit.
	EXPECT_NEAR(1.0, scanner.fileEntropy(0, 1), 1e-12);
}


/// batchMatch doubled its start offset without asking whether the doubling
/// fit: `const std::size_t startNib = startOffset * 2;`, in the same file as
/// the NIBBLES_PER_BYTE constant that had just been introduced to stop exactly
/// that, and fifty-two lines below it.
///
/// Measured against the real library before the fix, on this four-byte file
/// with the pattern "dead": batchMatch(pats, 100, SIZE_MAX) correctly reported
/// matched=0, but batchMatch(pats, 2^63, SIZE_MAX) reported matched=1 at
/// offset=0. A start offset 2^63 bytes past the end of a four-byte file wrapped
/// to nibble 0 and produced a full-confidence signature match at the front of
/// it. It is not memory-unsafe -- the scan still stops at the end of the
/// nibble string -- it is a false positive, and a signature match is what names
/// a packer, a compiler or a crypto constant.
TEST_F(GpuScannerTests,
AStartOffsetPastTheEndOfTheFileDoesNotMatchAtItsStart) {
	const std::vector<std::uint8_t> bytes = {0xDE, 0xAD, 0xBE, 0xEF};

	GpuScanner scanner;
	scanner.uploadFile(bytes.data(), bytes.size());
	const std::vector<std::string> patterns = {"dead"};

	// The control: a start past the end whose doubling fits was always right.
	EXPECT_FALSE(scanner.batchMatch(patterns, 100, SIZE_MAX)[0].matched);

	// SIZE_MAX / 2 + 1 is the smallest start offset whose doubling does not
	// fit -- 2^63 where std::size_t is 64 bits, which is the offset the wrong
	// answer above was measured at -- so it is the boundary the check sits on.
	const std::size_t firstWrapping =
			std::numeric_limits<std::size_t>::max() / 2 + 1;

	const auto wrapped = scanner.batchMatch(patterns, firstWrapping, SIZE_MAX);
	ASSERT_EQ(1u, wrapped.size());
	EXPECT_FALSE(wrapped[0].matched);
	EXPECT_EQ(0u, wrapped[0].totalNibs);
	EXPECT_EQ(0.0, wrapped[0].bestRatio);

	// Either side of that boundary, and the largest offset there is.
	EXPECT_FALSE(scanner.batchMatch(patterns, firstWrapping - 1, SIZE_MAX)[0].matched);
	EXPECT_FALSE(scanner.batchMatch(
			patterns, std::numeric_limits<std::size_t>::max(), SIZE_MAX)[0].matched);

	// The first offset that is genuinely outside a four-byte file, where no
	// wrapping is involved at all.
	EXPECT_FALSE(scanner.batchMatch(patterns, 4, SIZE_MAX)[0].matched);

	// And a start offset that really is inside the file still matches, so the
	// refusal above is not just refusing everything.
	const auto inside = scanner.batchMatch({"beef"}, 2, SIZE_MAX);
	ASSERT_EQ(1u, inside.size());
	EXPECT_TRUE(inside[0].matched);
	EXPECT_EQ(2u, inside[0].offset);
}

/// The other end of the same window: `std::min(stopOffset * 2 + 1, size - 1)`,
/// where both the doubling and the +1 could wrap.
///
/// Measured against the real library before the fix, same file and pattern:
/// batchMatch(pats, 0, 1000) correctly reported matched=1, but
/// batchMatch(pats, 0, 2^63) reported matched=0. The doubling wrapped to 0, the
/// +1 made endNib = 1, and a caller asking to scan a whole file got the window
/// collapsed to a single nibble. A false negative -- the opposite direction
/// from the start-offset wrap, from the same missing question.
TEST_F(GpuScannerTests,
AStopOffsetPastTheEndOfTheFileStillScansTheWholeFile) {
	const std::vector<std::uint8_t> bytes = {0xDE, 0xAD, 0xBE, 0xEF};

	GpuScanner scanner;
	scanner.uploadFile(bytes.data(), bytes.size());
	const std::vector<std::string> patterns = {"dead"};

	// The control: a stop past the end whose doubling fits was always right.
	EXPECT_TRUE(scanner.batchMatch(patterns, 0, 1000)[0].matched);

	const std::size_t firstWrapping =
			std::numeric_limits<std::size_t>::max() / 2 + 1;

	const auto wrapped = scanner.batchMatch(patterns, 0, firstWrapping);
	ASSERT_EQ(1u, wrapped.size());
	EXPECT_TRUE(wrapped[0].matched);
	EXPECT_EQ(0u, wrapped[0].offset);
	EXPECT_EQ(4u, wrapped[0].totalNibs);
	EXPECT_EQ(wrapped[0].totalNibs, wrapped[0].sameNibs);

	// Either side of the boundary, and the value the header documents as
	// "scan to end of file", which must keep meaning that.
	EXPECT_TRUE(scanner.batchMatch(patterns, 0, firstWrapping - 1)[0].matched);
	EXPECT_TRUE(scanner.batchMatch(
			patterns, 0, std::numeric_limits<std::size_t>::max())[0].matched);
	EXPECT_TRUE(scanner.batchMatch(patterns, 0, SIZE_MAX)[0].matched);

	// A stop offset that really does narrow the window still narrows it: byte 0
	// on its own is two nibbles and cannot hold the four-nibble "beef", which
	// lives at byte 2. Saturating must not turn into ignoring.
	EXPECT_FALSE(scanner.batchMatch({"beef"}, 0, 0)[0].matched);
}

/// The last position in the window was off by one in the other direction:
/// `if (patLen == 0 || endNib < patLen) continue;` throws away the pattern that
/// exactly fills the window, because such a pattern has patLen == endNib + 1.
///
/// Measured against the real library before the fix: the two-byte file DE AD
/// scanned for "dead" -- four nibbles against a four-nibble file, an exact
/// whole-file match -- reported matched=0, bestRatio=0 and totalNibs=0, as
/// though the pattern had never been considered. The device kernel in
/// gpu_scanner.cu computes the same quantity as `endPos + 1 - patLen` guarded
/// by `endPos + 1 >= patLen`, which is correct, so the CPU and GPU halves of
/// one class disagreed about the last window that fits.
TEST_F(GpuScannerTests,
AnExactWholeFileMatchIsFound) {
	const std::vector<std::uint8_t> two = {0xDE, 0xAD};

	GpuScanner scanner;
	scanner.uploadFile(two.data(), two.size());

	const auto whole = scanner.batchMatch({"dead"});
	ASSERT_EQ(1u, whole.size());
	EXPECT_TRUE(whole[0].matched);
	EXPECT_EQ(0u, whole[0].offset);
	EXPECT_EQ(4u, whole[0].totalNibs);
	EXPECT_EQ(4u, whole[0].sameNibs);

	// The same window reached through a stop offset on a longer file.
	const std::vector<std::uint8_t> four = {0xDE, 0xAD, 0xBE, 0xEF};
	GpuScanner longer;
	longer.uploadFile(four.data(), four.size());

	const auto capped = longer.batchMatch({"dead"}, 0, 1);
	ASSERT_EQ(1u, capped.size());
	EXPECT_TRUE(capped[0].matched);
	EXPECT_EQ(0u, capped[0].offset);

	// A pattern one nibble longer than the window still does not fit, so the
	// correction is one position and not a removed bound.
	EXPECT_FALSE(longer.batchMatch({"deadb"}, 0, 1)[0].matched);
}


// ---------------------------------------------------------------------------
// The shared host-side arithmetic, called directly.
//
// These reach the boundaries the public interface cannot: a nibble string
// longer than any file this test could upload, and a window whose ends are
// chosen rather than derived. They are the same functions the four tests above
// exercise through GpuScanner, defined in src/utils/gpu_scanner.cu.
// ---------------------------------------------------------------------------

/// Neither end of the window may wrap: a start that cannot be expressed in
/// nibbles is past the end of any file, and a stop that cannot be expressed in
/// nibbles means the end of this one.
TEST_F(GpuScannerTests,
TheNibbleWindowSaturatesAndRefusesRatherThanWrapping) {
	// Eight nibbles is a four-byte file.
	std::size_t startNib = 99;
	std::size_t endNib = 99;

	EXPECT_TRUE(gpuscan::nibbleWindow(0, SIZE_MAX, 8, startNib, endNib));
	EXPECT_EQ(0u, startNib);
	EXPECT_EQ(7u, endNib);

	// Byte 1 through byte 2 is nibbles 2 through 5: a stop offset selects the
	// second nibble of its byte, which is where the +1 came from.
	EXPECT_TRUE(gpuscan::nibbleWindow(1, 2, 8, startNib, endNib));
	EXPECT_EQ(2u, startNib);
	EXPECT_EQ(5u, endNib);

	const std::size_t firstWrapping =
			std::numeric_limits<std::size_t>::max() / 2 + 1;

	// Starts: past the end, whether by wrapping or not.
	EXPECT_FALSE(gpuscan::nibbleWindow(4, SIZE_MAX, 8, startNib, endNib));
	EXPECT_FALSE(gpuscan::nibbleWindow(firstWrapping, SIZE_MAX, 8, startNib, endNib));
	EXPECT_FALSE(gpuscan::nibbleWindow(firstWrapping - 1, SIZE_MAX, 8, startNib, endNib));
	EXPECT_FALSE(gpuscan::nibbleWindow(
			std::numeric_limits<std::size_t>::max(), SIZE_MAX, 8, startNib, endNib));

	// Stops: every one of these means "to the end of the file".
	EXPECT_TRUE(gpuscan::nibbleWindow(0, firstWrapping, 8, startNib, endNib));
	EXPECT_EQ(7u, endNib);
	EXPECT_TRUE(gpuscan::nibbleWindow(0, firstWrapping - 1, 8, startNib, endNib));
	EXPECT_EQ(7u, endNib);
	EXPECT_TRUE(gpuscan::nibbleWindow(
			0, std::numeric_limits<std::size_t>::max(), 8, startNib, endNib));
	EXPECT_EQ(7u, endNib);

	// A window big enough that the doubling is the only thing that could have
	// gone wrong: the stop offset here doubles to just under SIZE_MAX.
	const std::size_t hugeNibLen = std::numeric_limits<std::size_t>::max();
	EXPECT_TRUE(gpuscan::nibbleWindow(
			2, firstWrapping - 1, hugeNibLen, startNib, endNib));
	EXPECT_EQ(4u, startNib);
	EXPECT_EQ(std::numeric_limits<std::size_t>::max() - 1, endNib);

	// An empty nibble string has no window, and a refusal must not leave a
	// usable-looking one behind for a caller that ignores the result.
	EXPECT_FALSE(gpuscan::nibbleWindow(0, SIZE_MAX, 0, startNib, endNib));
	EXPECT_EQ(0u, startNib);
	EXPECT_EQ(0u, endNib);
}

/// The byte window fileEntropy measures over: an empty selection is refused
/// rather than answered with an underflowed length.
TEST_F(GpuScannerTests,
TheByteWindowRefusesARangeThatSelectsNothing) {
	std::size_t lo = 99;
	std::size_t hi = 99;

	EXPECT_TRUE(gpuscan::byteWindow(0, SIZE_MAX, 4, lo, hi));
	EXPECT_EQ(0u, lo);
	EXPECT_EQ(3u, hi);

	EXPECT_TRUE(gpuscan::byteWindow(1, 2, 4, lo, hi));
	EXPECT_EQ(1u, lo);
	EXPECT_EQ(2u, hi);

	// A single byte is a real window; it is zero-entropy, not no measurement.
	EXPECT_TRUE(gpuscan::byteWindow(3, 3, 4, lo, hi));
	EXPECT_EQ(3u, lo);
	EXPECT_EQ(3u, hi);

	// Past the end, backwards, and empty.
	EXPECT_FALSE(gpuscan::byteWindow(4, SIZE_MAX, 4, lo, hi));
	EXPECT_FALSE(gpuscan::byteWindow(
			std::numeric_limits<std::size_t>::max(), SIZE_MAX, 4, lo, hi));
	EXPECT_FALSE(gpuscan::byteWindow(3, 1, 4, lo, hi));
	EXPECT_FALSE(gpuscan::byteWindow(0, SIZE_MAX, 0, lo, hi));
	EXPECT_EQ(0u, lo);
	EXPECT_EQ(0u, hi);
}

/// The pattern that exactly fills the window has a position, and the pattern
/// one nibble longer has none.
TEST_F(GpuScannerTests,
TheLastStartPositionIncludesTheWindowThatExactlyFits) {
	std::size_t maxStart = 99;

	// Four nibbles in a four-nibble window: position 0, and only position 0.
	EXPECT_TRUE(gpuscan::lastStartFor(3, 4, maxStart));
	EXPECT_EQ(0u, maxStart);

	// Four nibbles in an eight-nibble window: positions 0 through 4.
	EXPECT_TRUE(gpuscan::lastStartFor(7, 4, maxStart));
	EXPECT_EQ(4u, maxStart);

	// One nibble in a one-nibble window.
	EXPECT_TRUE(gpuscan::lastStartFor(0, 1, maxStart));
	EXPECT_EQ(0u, maxStart);

	// One nibble too long, and a zero-length pattern, which has no position at
	// any window size.
	EXPECT_FALSE(gpuscan::lastStartFor(3, 5, maxStart));
	EXPECT_FALSE(gpuscan::lastStartFor(0, 2, maxStart));
	EXPECT_FALSE(gpuscan::lastStartFor(0, 0, maxStart));
	EXPECT_FALSE(gpuscan::lastStartFor(
			std::numeric_limits<std::size_t>::max(), 0, maxStart));
	EXPECT_EQ(0u, maxStart);

	// The largest windows there are, where the subtraction must not wrap. A
	// window ending at SIZE_MAX - 1 is SIZE_MAX nibbles long, so a pattern of
	// SIZE_MAX nibbles fills it exactly and has only position 0; one more
	// nibble of window is one more position.
	EXPECT_TRUE(gpuscan::lastStartFor(
			std::numeric_limits<std::size_t>::max() - 1,
			std::numeric_limits<std::size_t>::max(),
			maxStart));
	EXPECT_EQ(0u, maxStart);
	EXPECT_TRUE(gpuscan::lastStartFor(
			std::numeric_limits<std::size_t>::max(),
			std::numeric_limits<std::size_t>::max(),
			maxStart));
	EXPECT_EQ(1u, maxStart);
}

/// The byte-to-nibble expansion refuses what it cannot expand, and says so
/// before it has touched anything.
TEST_F(GpuScannerTests,
NibbleExpansionRefusesWhatItCannotExpand) {
	const std::uint8_t bytes[] = {0xDE, 0xAD};
	std::string out = "stale";

	EXPECT_TRUE(gpuscan::nibblesFor(bytes, 2, out));
	EXPECT_EQ(std::string("dead"), out);

	// A null pointer with a non-zero length: the two arguments disagree, and
	// the old body copied from address 0 rather than noticing.
	EXPECT_FALSE(gpuscan::nibblesFor(nullptr, 8, out));
	EXPECT_TRUE(out.empty());

	// A length whose nibble count is not representable. SIZE_MAX / 2 + 1 is the
	// smallest of them, so it is the boundary the check sits on.
	out = "stale";
	EXPECT_FALSE(gpuscan::nibblesFor(
			bytes, std::numeric_limits<std::size_t>::max(), out));
	EXPECT_TRUE(out.empty());
	EXPECT_FALSE(gpuscan::nibblesFor(
			bytes, std::numeric_limits<std::size_t>::max() / 2 + 1, out));
	EXPECT_TRUE(out.empty());

	// A zero-byte file is a legitimate input, not a refusal, even with a null
	// pointer -- that is what an empty std::vector's data() can be.
	out = "stale";
	EXPECT_TRUE(gpuscan::nibblesFor(nullptr, 0, out));
	EXPECT_TRUE(out.empty());
}

} // namespace tests
} // namespace utils
} // namespace retdec
