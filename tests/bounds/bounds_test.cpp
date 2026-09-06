/**
 * @file tests/bounds/bounds_test.cpp
 * @brief Unit tests for the verified helpers in retdec/utils.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 *
 * These headers are proved with ESBMC (tests/verification/, docs/VERIFICATION.md),
 * which covers the safety properties for all inputs. What a proof does not do is
 * pin down the *values* a decoder produces, so this suite carries the published
 * LEB128 vectors and the concrete boundary cases that the fuzzer originally hit.
 *
 * Proofs and tests answer different questions here. Keep both.
 */

#include "retdec/utils/bounds.h"
#include "retdec/utils/c_source_scan.h"
#include "retdec/utils/leb128.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace retdec::utils;

// ─── bounds ──────────────────────────────────────────────────────────────────

TEST(Bounds, RemainingSaturatesInsteadOfWrapping)
{
	EXPECT_EQ(bounds::remaining(2, 10), 8u);
	EXPECT_EQ(bounds::remaining(10, 10), 0u);
	// The case that matters: a cursor already past the end must not report a
	// nearly-infinite amount of input left.
	EXPECT_EQ(bounds::remaining(11, 10), 0u);
	EXPECT_EQ(bounds::remaining(SIZE_MAX, 10), 0u);
}

TEST(Bounds, AddFitsRejectsExactlyTheWrappingSums)
{
	EXPECT_TRUE(bounds::addFits(0, SIZE_MAX));
	EXPECT_TRUE(bounds::addFits(SIZE_MAX, 0));
	EXPECT_FALSE(bounds::addFits(SIZE_MAX, 1));
	EXPECT_FALSE(bounds::addFits(SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1));
}

TEST(Bounds, MulFitsRejectsExactlyTheOverflowingProducts)
{
	EXPECT_TRUE(bounds::mulFits(0, SIZE_MAX));
	EXPECT_TRUE(bounds::mulFits(SIZE_MAX, 1));
	EXPECT_FALSE(bounds::mulFits(SIZE_MAX, 2));
	EXPECT_TRUE(bounds::mulFits(SIZE_MAX / 2, 2));
}

TEST(Bounds, RangeFitsRefusesWrappingRanges)
{
	EXPECT_TRUE(bounds::rangeFits(4, 8, 4));
	EXPECT_FALSE(bounds::rangeFits(4, 8, 5));
	// `pos + len <= size` would accept this; the sum wraps to a small number.
	EXPECT_FALSE(bounds::rangeFits(SIZE_MAX, 8, 10));
	EXPECT_FALSE(bounds::rangeFits(9, 8, 0));
}

TEST(Bounds, CountFitsIsBoundedByTheInput)
{
	// The .pyc bug: five header bytes claiming 2^31 elements.
	EXPECT_FALSE(bounds::countFits(0, 5, 1u << 31));
	EXPECT_TRUE(bounds::countFits(0, 5, 5));
	EXPECT_FALSE(bounds::countFits(0, 5, 6));
	// Wider elements need proportionally more input.
	EXPECT_TRUE(bounds::countFits(0, 16, 4, 4));
	EXPECT_FALSE(bounds::countFits(0, 16, 5, 4));
}

TEST(Bounds, CountFitsRejectsAPositionPastTheEnd)
{
	// The corner ESBMC found: without the `pos <= size` check this answered
	// true and stopped composing with rangeFits.
	EXPECT_FALSE(bounds::countFits(SIZE_MAX - 34, 0, 0));
	EXPECT_FALSE(bounds::rangeFits(SIZE_MAX - 34, 0, 0));
}

TEST(Bounds, CountFitsTreatsZeroWidthAsOne)
{
	EXPECT_EQ(bounds::countFits(0, 8, 8, 0), bounds::countFits(0, 8, 8, 1));
	EXPECT_FALSE(bounds::countFits(0, 8, 9, 0));
}

TEST(Bounds, SignedCountFitsRejectsNegativesBeforeConverting)
{
	EXPECT_FALSE(bounds::signedCountFits(-1, 0, SIZE_MAX));
	EXPECT_FALSE(bounds::signedCountFits(INT64_MIN, 0, SIZE_MAX));
	EXPECT_TRUE(bounds::signedCountFits(4, 0, 8));
	EXPECT_FALSE(bounds::signedCountFits(9, 0, 8));
}

TEST(Bounds, PageCountRoundsThePageCountNotTheByteCount)
{
	// The mini_emu bug: 3 bytes must map one page, not round the length to 4096.
	EXPECT_EQ(bounds::pageCount(3, 4096), 1u);
	EXPECT_EQ(bounds::pageCount(0, 4096), 1u);
	EXPECT_EQ(bounds::pageCount(4096, 4096), 1u);
	EXPECT_EQ(bounds::pageCount(4097, 4096), 2u);
	EXPECT_EQ(bounds::pageCount(4096, 0), 0u);
}

TEST(Bounds, ReserveForNeverCommitsMoreThanTheCap)
{
	EXPECT_EQ(bounds::reserveFor(1u << 30, 64), 64u);
	EXPECT_EQ(bounds::reserveFor(7, 64), 7u);
	EXPECT_EQ(bounds::reserveFor(0, 64), 0u);
}

TEST(Bounds, ArrayFitsCatchesTheOverflowingProduct)
{
	EXPECT_TRUE(bounds::arrayFits(0, 64, 8, 8));
	EXPECT_FALSE(bounds::arrayFits(0, 64, 9, 8));
	// count * elementSize would wrap; the check must not be fooled by the
	// small wrapped value.
	EXPECT_FALSE(bounds::arrayFits(0, 64, SIZE_MAX / 2 + 1, 4));
}

// ─── LEB128 ──────────────────────────────────────────────────────────────────

namespace {

leb128::Result decodeU(const std::vector<std::uint8_t>& b)
{
	return leb128::decodeUnsigned(b.data(), b.size(), 0);
}

std::int64_t decodeS(const std::vector<std::uint8_t>& b)
{
	return leb128::toSigned(leb128::decodeSigned(b.data(), b.size(), 0).value);
}

} // namespace

// The vectors from the DWARF standard's LEB128 appendix.
TEST(Leb128, DecodesTheStandardUnsignedVectors)
{
	EXPECT_EQ(decodeU({0x00}).value, 0u);
	EXPECT_EQ(decodeU({0x02}).value, 2u);
	EXPECT_EQ(decodeU({0x7F}).value, 127u);
	EXPECT_EQ(decodeU({0x80, 0x01}).value, 128u);
	EXPECT_EQ(decodeU({0x81, 0x01}).value, 129u);
	EXPECT_EQ(decodeU({0x82, 0x01}).value, 130u);
	EXPECT_EQ(decodeU({0xB9, 0x64}).value, 12857u);
	EXPECT_EQ(decodeU({0xE5, 0x8E, 0x26}).value, 624485u);
}

TEST(Leb128, DecodesTheStandardSignedVectors)
{
	EXPECT_EQ(decodeS({0x00}), 0);
	EXPECT_EQ(decodeS({0x02}), 2);
	EXPECT_EQ(decodeS({0x7E}), -2);
	EXPECT_EQ(decodeS({0x7F}), -1);
	EXPECT_EQ(decodeS({0xFF, 0x00}), 127);
	EXPECT_EQ(decodeS({0x81, 0x7F}), -127);
	EXPECT_EQ(decodeS({0x80, 0x01}), 128);
	EXPECT_EQ(decodeS({0x80, 0x7F}), -128);
	EXPECT_EQ(decodeS({0x9B, 0xF1, 0x59}), -624485);
}

TEST(Leb128, DecodesTheWidestRepresentableValues)
{
	EXPECT_EQ(decodeU({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}).value,
			  UINT64_MAX);
	EXPECT_EQ(decodeS({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}), -1);
}

TEST(Leb128, ReportsBytesConsumed)
{
	EXPECT_EQ(decodeU({0x7F}).bytesRead, 1u);
	EXPECT_EQ(decodeU({0xE5, 0x8E, 0x26}).bytesRead, 3u);
	// Trailing bytes are not consumed.
	EXPECT_EQ(decodeU({0x01, 0xFF, 0xFF}).bytesRead, 1u);
}

TEST(Leb128, RefusesTruncatedInput)
{
	EXPECT_FALSE(decodeU({}).ok);
	EXPECT_FALSE(decodeU({0x80}).ok);
	EXPECT_FALSE(decodeU({0x80, 0x80, 0x80}).ok);
	// A failed decode reports no progress, so a cursor advanced by bytesRead
	// cannot end up somewhere undefined.
	EXPECT_EQ(decodeU({0x80}).bytesRead, 0u);
	EXPECT_EQ(decodeU({0x80}).value, 0u);
}

// The bug this header exists to prevent: a run of continuation bytes used to
// drive the shift past the width of the accumulator, which is undefined
// behaviour, and walk the cursor off the end.
TEST(Leb128, RefusesAnOverLongEncoding)
{
	std::vector<std::uint8_t> tooLong(32, 0x80);
	tooLong.back() = 0x01;
	EXPECT_FALSE(leb128::decodeUnsigned(tooLong.data(), tooLong.size(), 0).ok);
	EXPECT_FALSE(leb128::decodeSigned(tooLong.data(), tooLong.size(), 0).ok);

	std::vector<std::uint8_t> allContinuation(64, 0xFF);
	EXPECT_FALSE(
		leb128::decodeUnsigned(allContinuation.data(), allContinuation.size(), 0).ok);
}

TEST(Leb128, RefusesAPositionPastTheEnd)
{
	const std::uint8_t b[] = {0x01, 0x02};
	EXPECT_FALSE(leb128::decodeUnsigned(b, 2, 2).ok);
	EXPECT_FALSE(leb128::decodeUnsigned(b, 2, 99).ok);
	EXPECT_FALSE(leb128::decodeUnsigned(nullptr, 0, 0).ok);
}

TEST(Leb128, DecodesFromAnOffset)
{
	const std::uint8_t b[] = {0xFF, 0xFF, 0xE5, 0x8E, 0x26};
	const auto r = leb128::decodeUnsigned(b, sizeof(b), 2);
	ASSERT_TRUE(r.ok);
	EXPECT_EQ(r.value, 624485u);
	EXPECT_EQ(r.bytesRead, 3u);
}

TEST(Leb128, ToSignedIsAnExactRoundTrip)
{
	EXPECT_EQ(leb128::toSigned(0), 0);
	EXPECT_EQ(leb128::toSigned(UINT64_MAX), -1);
	EXPECT_EQ(leb128::toSigned(static_cast<std::uint64_t>(INT64_MAX)), INT64_MAX);
	EXPECT_EQ(leb128::toSigned(static_cast<std::uint64_t>(INT64_MAX) + 1), INT64_MIN);
}

// ─── C source scanning ───────────────────────────────────────────────────────

namespace {

std::string blanked(const std::string& in)
{
	std::string out(in.size(), '\0');
	if (!in.empty()) retdec::utils::source_scan::blankNonCode(in.data(), in.size(), &out[0]);
	return out;
}

} // namespace

TEST(SourceScan, BlanksLineCommentBodies)
{
	const std::string code = "int x; ";
	const std::string comment = "// if while";
	const std::string tail = "\nint y;";

	// Built rather than written out, so the expectation cannot be off by a
	// space: the comment becomes exactly as many blanks as it had characters.
	EXPECT_EQ(blanked(code + comment + tail),
			  code + std::string(comment.size(), ' ') + tail);
}

TEST(SourceScan, BlanksBlockCommentBodies)
{
	EXPECT_EQ(blanked("a/* if */b"), "a        b");
}

TEST(SourceScan, KeepsNewlinesInsideBlockComments)
{
	// Offsets and line numbers computed from the result have to still match.
	const std::string in = "a/*\n\n*/b";
	const std::string out = blanked(in);
	ASSERT_EQ(out.size(), in.size());
	EXPECT_EQ(out[3], '\n');
	EXPECT_EQ(out[4], '\n');
}

TEST(SourceScan, BlanksStringAndCharLiteralContents)
{
	// Delimiters are kept; only what is between them goes.
	EXPECT_EQ(blanked("s = \"if\";"), "s = \"  \";");
	EXPECT_EQ(blanked("c = 'x';"), "c = ' ';");
}

TEST(SourceScan, HandlesEscapedQuotesInsideLiterals)
{
	// Source text: "a\"b" x  -- the middle quote is escaped, so the literal
	// does not end there. Four characters of content become four blanks.
	const std::string in = "\"a\\\"b\" x";
	EXPECT_EQ(blanked(in), "\"    \" x");
	EXPECT_EQ(blanked(in).size(), in.size());
}

TEST(SourceScan, LeavesPlainCodeAlone)
{
	const std::string code = "if (x > 0) { return 1; }";
	EXPECT_EQ(blanked(code), code);
}

TEST(SourceScan, NeverInventsCharacters)
{
	const std::string in = "a\"b\"/*c*/'d'//e";
	const std::string out = blanked(in);
	ASSERT_EQ(out.size(), in.size());
	for (std::size_t i = 0; i < in.size(); ++i)
	{
		EXPECT_TRUE(out[i] == in[i] || out[i] == ' ') << "at " << i;
	}
}

TEST(SourceScan, HandlesTruncatedTokensAtTheEnd)
{
	// A lone '/' or an unterminated comment or literal at the very end is where
	// the one-character lookahead would run off the buffer.
	EXPECT_EQ(blanked("a/"), "a/");
	EXPECT_EQ(blanked("a/*"), "a  ");
	EXPECT_EQ(blanked("a//"), "a  ");
	EXPECT_EQ(blanked("a\""), "a\"");
	EXPECT_EQ(blanked("*"), "*");
	EXPECT_EQ(blanked(""), "");
}
