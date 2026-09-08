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

// ─── what a small input can cost ────────────────────────────────────────────
//
// Nesting is recursion (a SEQUENCE parses its content, which is another item)
// and every Asn1Item copies the buffer it was handed. So each level holds a
// copy of its parent's content while it descends, and a SEQUENCE's elements
// each keep a copy of the siblings that follow them. Both are quadratic in the
// input, measured on this container:
//
//   16384 nested SEQUENCEs, 65538 bytes in  -> 1058276 kB resident
//   20000 sibling NULLs,    40006 bytes in  ->  396680 kB resident
//
// and the deep one runs out of stack rather than memory on a larger input.

namespace {

/// A DER length in the long form that uses @a bytes length bytes.
void appendLongFormLength(std::vector<std::uint8_t>& out, std::size_t length, int bytes)
{
	out.push_back(static_cast<std::uint8_t>(0x80 | bytes));
	for (int i = bytes - 1; i >= 0; --i)
		out.push_back(static_cast<std::uint8_t>((length >> (8 * i)) & 0xFF));
}

/// @a levels SEQUENCEs wrapped around a NULL.
std::vector<std::uint8_t> nestedSequences(int levels)
{
	std::vector<std::uint8_t> buf = {Asn1Tag_Null, 0x00};
	for (int i = 0; i < levels; ++i)
	{
		std::vector<std::uint8_t> next;
		next.push_back(Asn1Tag_Sequence);
		appendLongFormLength(next, buf.size(), 2);
		next.insert(next.end(), buf.begin(), buf.end());
		buf = std::move(next);
	}
	return buf;
}

/// One SEQUENCE holding @a count NULLs.
std::vector<std::uint8_t> siblingNulls(int count)
{
	std::vector<std::uint8_t> body;
	for (int i = 0; i < count; ++i)
	{
		body.push_back(Asn1Tag_Null);
		body.push_back(0x00);
	}

	std::vector<std::uint8_t> buf;
	buf.push_back(Asn1Tag_Sequence);
	appendLongFormLength(buf, body.size(), 4);
	buf.insert(buf.end(), body.begin(), body.end());
	return buf;
}

/// How deep a parsed item actually goes.
std::size_t depthOf(const std::shared_ptr<Asn1Item>& item)
{
	std::size_t depth = 0;
	std::shared_ptr<Asn1Item> cur = item;
	while (cur)
	{
		++depth;
		if (auto seq = std::dynamic_pointer_cast<Asn1Sequence>(cur))
		{
			cur = seq->getNumberOfElements() ? seq->getElement(0) : nullptr;
			continue;
		}
		if (auto cs = std::dynamic_pointer_cast<Asn1ContextSpecific>(cur))
		{
			cur = cs->getItem();
			continue;
		}
		break;
	}
	return depth;
}

} // namespace

TEST(Asn1Tests, NestingStopsAtABoundedDepth)
{
	auto item = Asn1Item::parse(nestedSequences(16384));

	ASSERT_TRUE(item != nullptr);
	// The exact bound is the decoder's business; that there is one is not.
	EXPECT_LE(depthOf(item), 256u);
}

// A signature's own nesting is tens of levels, so the bound has to be well
// clear of anything real.
TEST(Asn1Tests, OrdinarySignatureNestingIsNotRefused)
{
	const int levels = 16;
	auto item = Asn1Item::parse(nestedSequences(levels));

	ASSERT_TRUE(item != nullptr);
	EXPECT_EQ(static_cast<std::size_t>(levels) + 1, depthOf(item));
}

// Every element is still there, and each holds only its own bytes rather than
// a copy of the siblings that follow it.
TEST(Asn1Tests, ASequenceOfManyElementsKeepsOnlyItsOwnBytes)
{
	const int count = 20000;
	auto item = Asn1Item::parse(siblingNulls(count));

	auto seq = std::dynamic_pointer_cast<Asn1Sequence>(item);
	ASSERT_TRUE(seq != nullptr);
	ASSERT_EQ(static_cast<std::size_t>(count), seq->getNumberOfElements());

	std::size_t total = 0;
	for (std::size_t i = 0; i < seq->getNumberOfElements(); ++i)
	{
		auto element = seq->getElement(i);
		ASSERT_TRUE(element != nullptr);
		EXPECT_EQ(2u, element->getLength()) << "element " << i;
		total += element->getData().capacity();
	}

	// Without the trim each element retained the rest of the sequence, so this
	// sum was quadratic: about 400 MB for a 40 kB input.
	EXPECT_LT(total, 16u * 1024 * 1024) << "elements retained " << total << " bytes";
}
