/**
* @file tests/utils/gpu_scanner_tests.cpp
* @brief Tests for the CPU fallback of the @c GpuScanner module.
* @copyright (c) 2025-2026 Odin Loch trading as Imortek
*
* These cover GpuScanner::uploadFile, which is a public entry point taking a
* raw pointer and a length as two independent arguments and, until the fix in
* src/utils/gpu_scanner_cpu.cpp, trusted both without asking anything.
*/

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retdec/utils/gpu_scanner.h"

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
/// characters per byte, so `size * 2` wraps to 18446744073709551614 -- resize()
/// gets a length two BELOW the byte count while the loop writes at i*2 and
/// i*2+1 for every i < size, past the end of the string. Before that can even
/// happen, `data + size` is pointer arithmetic 2^64 past a four-byte array and
/// assign() throws std::length_error out of a void function that no caller
/// wraps in a try block. Both are refusals the function is now able to make:
/// bounds::mulFits is asked before anything is touched.
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

} // namespace tests
} // namespace utils
} // namespace retdec
