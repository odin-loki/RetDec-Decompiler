/**
 * @file tests/loader_sim/xbyte_width_guard_test.cpp
 * @brief The width guard on the getXByte family, and the kernel it routes to.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * What is under test
 * ------------------
 * FileFormat::getXByte, FileFormat::getXByteOffset, Image::getXByte and
 * Image::setXByte all bounded the requested width with
 *
 *     x * getByteLength() > sizeof(res) * CHAR_BIT
 *
 * which forms the product before comparing it. ESBMC's witness for that
 * expression is x = 2305843009213693954 (0x2000000000000002) with a byte length
 * of 8: the true product 0x10000000000000010 does not fit in 64 bits, wraps to
 * 16, and `16 > 64` is false -- so the guard admitted a width of 2.3e18 units.
 * Reported as "arithmetic overflow on mul, !overflow(\"*\", x, byteLength)".
 * The same family spelled containment as `offset + x > getLoadedFileLength()`,
 * which wraps the same way; in getXBytesOffset the wrapped sum is followed
 * immediately by `loadedBytes->begin() + offset`.
 *
 * Why these tests are shaped this way
 * -----------------------------------
 * src/fileformat and src/loader cannot be linked here. Both pull in
 * retdec/fileformat/types/sec_seg/sec_seg.h, which includes
 * <llvm/ADT/StringRef.h>, so neither module is in the standalone fast path
 * (scripts/standalone_check.sh MODULES) and neither compiles at all without an
 * LLVM tree:
 *
 *     $ g++ -std=c++17 -Iinclude -fsyntax-only src/loader/loader/image.cpp
 *     include/retdec/fileformat/types/sec_seg/sec_seg.h:14:10: fatal error:
 *     llvm/ADT/StringRef.h: No such file or directory
 *
 * So the guards themselves are checked as a source invariant -- each of the
 * four call sites must no longer form the product, and must call the helper
 * that delegates to the proved kernel -- and the kernel's answer to the exact
 * counterexample is checked by calling it, since retdec/utils/byte_order.h is
 * header-only and dependency-free. The source-invariant half is what fails when
 * the fix is reverted; the kernel half is what says the replacement is right.
 *
 * These are string checks over real source files, so they are deliberately
 * anchored on one distinctive token each rather than on whitespace or line
 * numbers, and they fail loudly rather than skip if the tree cannot be found.
 */

#include "retdec/utils/bounds.h"
#include "retdec/utils/byte_order.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace {

/// The two sources under test, relative to the repository root.
constexpr const char* kFileFormatSrc = "src/fileformat/file_format/file_format.cpp";
constexpr const char* kImageSrc      = "src/loader/loader/image.cpp";

bool fileExists(const std::string& path)
{
	std::ifstream f(path);
	return f.good();
}

/// Find the repository root by walking up from the working directory.
///
/// scripts/standalone_check.sh runs the suite binaries from the root, while a
/// CMake build runs them from somewhere under build/; both are covered by
/// walking up until file_format.cpp appears. The returned prefix always ends in
/// a separator and is never empty on success -- "./" at the root itself -- so
/// that an empty result means only "not found", which the tests report as a
/// failure rather than a skip. A source invariant that silently stops being
/// checked is worse than none.
std::string repoRoot()
{
	std::string prefix = "./";
	for (int up = 0; up < 12; ++up)
	{
		if (fileExists(prefix + kFileFormatSrc))
		{
			return prefix;
		}
		prefix += "../";
	}
	return {};
}

std::string readSource(const char* relative)
{
	const std::string root = repoRoot();
	if (root.empty())
	{
		return {};
	}

	std::ifstream in(root + relative);
	std::ostringstream buf;
	buf << in.rdbuf();
	return buf.str();
}

/// Drop comments, so the invariants below are about code.
///
/// The fixed sites quote the expression they replaced in their comments -- that
/// is the point of the comment -- so a plain text search finds `offset + x` in
/// getXBytesOffset whether or not the code still forms the sum. Both comment
/// forms are handled; no source under test has a string literal containing a
/// comment opener, and none needs one for these checks to mean what they say.
std::string stripComments(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (std::size_t i = 0; i < text.size();)
	{
		if (text.compare(i, 2, "//") == 0)
		{
			const auto eol = text.find('\n', i);
			if (eol == std::string::npos)
			{
				break;
			}
			i = eol; // keep the newline, so line structure survives
		}
		else if (text.compare(i, 2, "/*") == 0)
		{
			const auto close = text.find("*/", i + 2);
			if (close == std::string::npos)
			{
				break;
			}
			i = close + 2;
		}
		else
		{
			out += text[i++];
		}
	}
	return out;
}

/// The text of one function definition, from its signature to the first line
/// that is a lone closing brace.
std::string functionBody(const std::string& source, const std::string& signature)
{
	const auto start = source.find(signature);
	if (start == std::string::npos)
	{
		return {};
	}

	const auto end = source.find("\n}\n", start);
	const std::string body = end == std::string::npos
			? source.substr(start)
			: source.substr(start, end - start);
	return stripComments(body);
}

bool contains(const std::string& haystack, const std::string& needle)
{
	return haystack.find(needle) != std::string::npos;
}

/// One call site: the file it lives in and the signature that opens it.
struct Site
{
	const char* source;
	const char* signature;
};

const Site kWidthGuardSites[] = {
	{kFileFormatSrc, "bool FileFormat::getXByte("},
	{kFileFormatSrc, "bool FileFormat::getXByteOffset("},
	{kImageSrc,      "bool Image::getXByte("},
	{kImageSrc,      "bool Image::setXByte("},
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// The four guards: no product formed, and the proved kernel called
// ═══════════════════════════════════════════════════════════════════════════════

TEST(XByteWidthGuard, NoCallSiteFormsTheWrappingProduct)
{
	ASSERT_FALSE(repoRoot().empty())
			<< "cannot locate the repository root from the working directory; "
			   "these tests check a source invariant and must not be skipped";

	for (const auto& site : kWidthGuardSites)
	{
		const std::string body = functionBody(readSource(site.source), site.signature);
		ASSERT_FALSE(body.empty()) << site.source << " has no " << site.signature;

		// The witness above only exists because the product is formed first.
		EXPECT_FALSE(contains(body, "* getByteLength()"))
				<< site.signature << " in " << site.source
				<< " forms x * getByteLength() before comparing it; "
				   "x = 0x2000000000000002 with byte length 8 wraps to 16 and passes";
	}
}

TEST(XByteWidthGuard, EveryCallSiteRoutesThroughTheWidthKernel)
{
	ASSERT_FALSE(repoRoot().empty());

	for (const auto& site : kWidthGuardSites)
	{
		const std::string body = functionBody(readSource(site.source), site.signature);
		ASSERT_FALSE(body.empty()) << site.source << " has no " << site.signature;

		EXPECT_TRUE(contains(body, "xWidthFitsAccumulator("))
				<< site.signature << " in " << site.source
				<< " does not bound the width through the helper that calls "
				   "byteorder::widthFits";
	}
}

TEST(XByteWidthGuard, TheHelperDelegatesRatherThanRederiving)
{
	ASSERT_FALSE(repoRoot().empty());

	for (const char* src : {kFileFormatSrc, kImageSrc})
	{
		const std::string helper =
				functionBody(readSource(src), "bool xWidthFitsAccumulator(");
		ASSERT_FALSE(helper.empty()) << src << " has no xWidthFitsAccumulator";

		// The point of the helper is that it does no arithmetic of its own.
		EXPECT_TRUE(contains(helper, "byteorder::widthFits("))
				<< src << " re-derives the width test instead of calling the "
				          "proved kernel";
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// The offset readers: no sum formed before the buffer is indexed
// ═══════════════════════════════════════════════════════════════════════════════

TEST(XByteOffsetGuard, OffsetReadersDoNotFormTheWrappingSum)
{
	ASSERT_FALSE(repoRoot().empty());

	const std::string source = readSource(kFileFormatSrc);
	for (const char* signature : {"bool FileFormat::getXByteOffset(",
	                              "bool FileFormat::getXBytesOffset("})
	{
		const std::string body = functionBody(source, signature);
		ASSERT_FALSE(body.empty()) << kFileFormatSrc << " has no " << signature;

		// At offset SIZE_MAX and x = 1 the sum is 0, which is inside every
		// buffer -- and getXBytesOffset indexes loadedBytes with offset next.
		EXPECT_FALSE(contains(body, "offset + x"))
				<< signature << " compares offset + x against the file length; "
				   "the sum wraps and the comparison then passes";
		EXPECT_TRUE(contains(body, "rangeFitsWide("))
				<< signature << " does not bound the range through bounds::rangeFits";
	}

	const std::string helper = functionBody(source, "bool rangeFitsWide(");
	ASSERT_FALSE(helper.empty()) << kFileFormatSrc << " has no rangeFitsWide";
	EXPECT_TRUE(contains(helper, "bounds::rangeFits("))
			<< kFileFormatSrc << " re-derives the containment test instead of "
			                     "calling the proved kernel";
}

// ═══════════════════════════════════════════════════════════════════════════════
// The kernels, on the exact counterexamples
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WidthFitsKernel, RefusesTheEsbmcWitnessTheProductAdmitted)
{
	// ESBMC's witness, verbatim.
	constexpr std::uint64_t kWitness = 2305843009213693954ULL; // 0x2000000000000002
	constexpr unsigned kByteLength = 8;                        // FileFormat::getByteLength

	// What the old expression computed: the product wraps to 16, so the guard
	// asked `16 > 64` and let a width of 2.3e18 units through.
	EXPECT_EQ(static_cast<std::uint64_t>(kWitness * kByteLength), 16ULL);
	EXPECT_FALSE(kWitness * kByteLength > 64ULL);

	// What the kernel answers.
	EXPECT_FALSE(retdec::utils::byteorder::widthFits(
			static_cast<std::size_t>(kWitness), kByteLength));
}

TEST(WidthFitsKernel, AcceptsExactlyTheWidthsAnEightBitByteAllows)
{
	using retdec::utils::byteorder::widthFits;

	// A 64-bit accumulator at 8 bits per byte holds 1..8 bytes and no more.
	for (std::size_t n = 1; n <= 8; ++n)
	{
		EXPECT_TRUE(widthFits(n, 8)) << "n = " << n;
	}
	EXPECT_FALSE(widthFits(9, 8));
	EXPECT_FALSE(widthFits(SIZE_MAX, 8));

	// Zero is refused by the kernel, which is why the call sites keep their own
	// x == 0 branch: getXByte answers a zero-width request with res = 0.
	EXPECT_FALSE(widthFits(0, 8));
}

TEST(RangeFitsKernel, RefusesTheOffsetWhoseSumWraps)
{
	using retdec::utils::bounds::rangeFits;

	// The getXBytesOffset case: a 512-byte file, an offset of SIZE_MAX and one
	// byte. The sum is 0, so `offset + x <= length` passed and the next
	// statement formed loadedBytes->begin() + SIZE_MAX.
	constexpr std::size_t kFileLength = 512;
	EXPECT_EQ(static_cast<std::size_t>(SIZE_MAX + std::size_t(1)), std::size_t(0));
	EXPECT_TRUE(SIZE_MAX + std::size_t(1) <= kFileLength);
	EXPECT_FALSE(rangeFits(SIZE_MAX, kFileLength, 1));

	// And it still admits the reads that are genuinely inside the file.
	EXPECT_TRUE(rangeFits(504, kFileLength, 8));
	EXPECT_FALSE(rangeFits(505, kFileLength, 8));
}
