/**
 * @file tests/fileformat/asn1_test.cpp
 * @brief Unit tests for the ASN.1 / DER parser.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * src/fileformat/utils/asn1.cpp is a DER decoder over a caller-supplied byte
 * vector. It links no LLVM, and until now nothing in this tree compiled it in
 * the dependency-free path or asserted anything about it.
 *
 * It also has no caller: pe_format.cpp includes asn1.h and references no Asn1
 * symbol, and the Authenticode path goes through deps/authenticode-parser
 * instead. So what follows is a shipped, installed, reachable-by-API decoder
 * whose two defects nothing in the tree could trip -- which is exactly the
 * class that stays broken.
 *
 * The lengths below are DER long-form: the low seven bits of the second byte
 * say how many bytes the length occupies, and those bytes are the length.
 */

#include "retdec/fileformat/utils/asn1.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using namespace retdec::fileformat;

namespace {

/// Tag byte for a universal primitive OCTET STRING.
constexpr std::uint8_t kOctetString = Asn1Tag_OctetString;

} // namespace

TEST(Asn1Tests, ALengthByteCountThatReachesTheLastByteDoesNotReadPastIt)
{
	// _data[1] = 0x81 declares one length byte, which lives at _data[2]. The
	// guard was `1 + lengthBytes > _data.size()`, so at size 2 it passed --
	// while the loop reads _data[2 + i] and so touches _data[2], one past the
	// end. Measured before the fix:
	//
	//   ERROR: AddressSanitizer: heap-buffer-overflow
	//   READ of size 1 ... Asn1Item::init() asn1.cpp:133
	const std::vector<std::uint8_t> data = {kOctetString, 0x81};

	Asn1OctetString item(data);

	EXPECT_EQ(0u, item.getContentLength());
	EXPECT_TRUE(item.getContentData().empty());
}

TEST(Asn1Tests, ADeclaredLengthThatWrapsIsRefused)
{
	// Eight length bytes of 0xFF make _contentLength SIZE_MAX. The old
	// `_data.resize(2 + lengthBytes + _contentLength)` then evaluates to
	// resize(9) -- the vector SHRINKS -- while _contentBegin is formed as
	// begin() + 10, an iterator past the end, and getContentData() builds a
	// vector from [that, that + SIZE_MAX).
	std::vector<std::uint8_t> data = {kOctetString, 0x88};
	data.insert(data.end(), 8, 0xFF);
	ASSERT_EQ(10u, data.size());

	Asn1OctetString item(data);

	EXPECT_EQ(0u, item.getContentLength());
	EXPECT_TRUE(item.getContentData().empty());
}

TEST(Asn1Tests, ADeclaredLengthLongerThanTheInputIsRefusedRatherThanZeroFilled)
{
	// Three length bytes saying 0xFFFFFF: no wrap, but the resize honours it
	// verbatim and allocates 16 MiB of zeros from a six-byte input, then hands
	// them back as the item's content. Fabricated bytes are worse than no
	// bytes in a signature decoder.
	const std::vector<std::uint8_t> data = {kOctetString, 0x83, 0xFF, 0xFF, 0xFF, 0x00};

	Asn1OctetString item(data);

	EXPECT_EQ(0u, item.getContentLength());
	EXPECT_TRUE(item.getContentData().empty());
	EXPECT_LE(item.getData().size(), data.size());
}

TEST(Asn1Tests, MoreLengthBytesThanASizeCanHoldIsRefused)
{
	// The accumulation is `_contentLength = (_contentLength << 8) | byte` with
	// no cap on the iteration count, so nine or more length bytes silently
	// discard the high ones and decode to something the file did not say.
	std::vector<std::uint8_t> data = {kOctetString, 0x89};
	data.insert(data.end(), 9, 0x01);
	data.push_back(0x00);

	Asn1OctetString item(data);

	EXPECT_EQ(0u, item.getContentLength());
	EXPECT_TRUE(item.getContentData().empty());
}

// ─── and the ordinary cases, so a fix that refuses everything does not pass ──

TEST(Asn1Tests, AShortFormItemIsDecoded)
{
	const std::vector<std::uint8_t> data = {kOctetString, 0x03, 0xAA, 0xBB, 0xCC};

	Asn1OctetString item(data);

	EXPECT_EQ(3u, item.getContentLength());
	ASSERT_EQ(3u, item.getContentData().size());
	EXPECT_EQ(0xAA, item.getContentData()[0]);
	EXPECT_EQ(0xCC, item.getContentData()[2]);
	EXPECT_EQ("AABBCC", item.getString());
}

TEST(Asn1Tests, ALongFormItemIsDecoded)
{
	// 0x81 = one length byte, which is 4.
	const std::vector<std::uint8_t> data = {kOctetString, 0x81, 0x04, 0x01, 0x02, 0x03, 0x04};

	Asn1OctetString item(data);

	EXPECT_EQ(4u, item.getContentLength());
	ASSERT_EQ(4u, item.getContentData().size());
	EXPECT_EQ(0x01, item.getContentData()[0]);
	EXPECT_EQ(0x04, item.getContentData()[3]);
}

TEST(Asn1Tests, AnItemShorterThanTheInputIsTruncatedToItsDeclaredExtent)
{
	// Two content bytes declared, four supplied: the trailing two belong to
	// whatever comes next, and the item must not claim them.
	const std::vector<std::uint8_t> data = {kOctetString, 0x02, 0x11, 0x22, 0x33, 0x44};

	Asn1OctetString item(data);

	EXPECT_EQ(2u, item.getContentLength());
	ASSERT_EQ(2u, item.getContentData().size());
	EXPECT_EQ(0x11, item.getContentData()[0]);
	EXPECT_EQ(0x22, item.getContentData()[1]);
	EXPECT_EQ(4u, item.getData().size());
}

TEST(Asn1Tests, AnObjectIdentifierIsDecoded)
{
	// 1.2.840.113549.2.5 -- the MD5 digest algorithm OID, which asn1.h names.
	const std::vector<std::uint8_t> data = {Asn1Tag_Object, 0x08, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x02, 0x05};

	Asn1Object item(data);

	EXPECT_EQ(DigestAlgorithmOID_Md5, item.getIdentifier());
}

TEST(Asn1Tests, TooShortAnInputIsRefused)
{
	EXPECT_TRUE(Asn1Item::parse({}) == nullptr);
	EXPECT_TRUE(Asn1Item::parse({kOctetString}) == nullptr);

	Asn1OctetString empty(std::vector<std::uint8_t>{});
	EXPECT_EQ(0u, empty.getContentLength());
	EXPECT_TRUE(empty.getContentData().empty());
}
