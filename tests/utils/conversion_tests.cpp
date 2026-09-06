/**
* @file tests/utils/conversion_tests.cpp
* @brief Tests for the @c conversion module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <gtest/gtest.h>

#include <limits>

#include "retdec/utils/conversion.h"

using namespace ::testing;

namespace retdec {
namespace utils {
namespace tests {

/**
* @brief Tests for the @c conversion module.
*/
class ConversionTests: public Test {};

//
// intToHexString()
//

TEST_F(ConversionTests,
ToHexCorrectConversionNoBase) {
	EXPECT_EQ("0", intToHexString(0x0, false));
	EXPECT_EQ("1", intToHexString(0x1, false));
	EXPECT_EQ("f", intToHexString(0xf, false));
	EXPECT_EQ("400", intToHexString(0x400, false));
	EXPECT_EQ("ffff", intToHexString(0xffff, false));
}

TEST_F(ConversionTests,
ToHexCorrectConversionWithBase) {
	EXPECT_EQ("0x0", intToHexString(0x0, true));
	EXPECT_EQ("0x1", intToHexString(0x1, true));
	EXPECT_EQ("0xf", intToHexString(0xf, true));
	EXPECT_EQ("0x400", intToHexString(0x400, true));
	EXPECT_EQ("0xffff", intToHexString(0xffff, true));
}

TEST_F(ConversionTests,
ToHexCorrectConversionWithFill) {
	EXPECT_EQ("0x0", intToHexString(0x0, true, 0));
	EXPECT_EQ("0", intToHexString(0x0, false, 0));
	EXPECT_EQ("0x0000", intToHexString(0x0, true, 4));
	EXPECT_EQ("0000", intToHexString(0x0, false, 4));
	EXPECT_EQ("0x1234", intToHexString(0x1234, true, 2));
	EXPECT_EQ("1234", intToHexString(0x1234, false, 2));
	EXPECT_EQ("0x1234", intToHexString(0x1234, true, 4));
	EXPECT_EQ("1234", intToHexString(0x1234, false, 4));
	EXPECT_EQ("0x00001234", intToHexString(0x1234, true, 8));
	EXPECT_EQ("00001234", intToHexString(0x1234, false, 8));
}

//
// strToNum()
//

TEST_F(ConversionTests,
StrToNumIntDecimalSuccess) {
	int out = 0;
	EXPECT_TRUE(strToNum("-100", out, std::dec));
	EXPECT_EQ(-100, out);

	out = 0;
	EXPECT_TRUE(strToNum("-1", out, std::dec));
	EXPECT_EQ(-1, out);

	out = 0;
	EXPECT_TRUE(strToNum("0", out, std::dec));
	EXPECT_EQ(0, out);

	out = 0;
	EXPECT_TRUE(strToNum("1", out, std::dec));
	EXPECT_EQ(1, out);

	out = 0;
	EXPECT_TRUE(strToNum("100", out, std::dec));
	EXPECT_EQ(100, out);

	out = 0;
	EXPECT_TRUE(strToNum("0000", out, std::dec));
	EXPECT_EQ(0, out);

	out = 0;
	EXPECT_TRUE(strToNum("0005", out, std::dec));
	EXPECT_EQ(5, out);

	// TODO How to test std::numeric_limit<int>::max?
}

TEST_F(ConversionTests,
StrToNumIntDecimalFailure) {
	int out = -1;
	EXPECT_FALSE(strToNum("", out, std::dec));
	EXPECT_EQ(-1, out);

	out = -1;
	EXPECT_FALSE(strToNum("xx", out, std::dec));
	EXPECT_EQ(-1, out);

	out = -1;
	EXPECT_FALSE(strToNum("12 bbb", out, std::dec));
	EXPECT_EQ(-1, out);

	out = -1;
	EXPECT_FALSE(strToNum("12bbb", out, std::dec));
	EXPECT_EQ(-1, out);

	// TODO How to test overflow?
}

TEST_F(ConversionTests,
StrToNumIntHexSuccess) {
	int out = 0;
	EXPECT_TRUE(strToNum("-0xFA", out, std::hex));
	EXPECT_EQ(-0xFA, out);

	out = 0;
	EXPECT_TRUE(strToNum("-0x1", out, std::hex));
	EXPECT_EQ(-0x1, out);

	out = 0;
	EXPECT_TRUE(strToNum("0x0", out, std::hex));
	EXPECT_EQ(0x0, out);

	out = 0;
	EXPECT_TRUE(strToNum("0x1", out, std::hex));
	EXPECT_EQ(0x1, out);

	out = 0;
	EXPECT_TRUE(strToNum("0xFA", out, std::hex));
	EXPECT_EQ(0xFA, out);

	out = 0;
	EXPECT_TRUE(strToNum("0x00F", out, std::hex));
	EXPECT_EQ(0x00F, out);
}

TEST_F(ConversionTests,
StrToNumIntHexFailure) {
	int out = -1;
	EXPECT_FALSE(strToNum("", out, std::hex));
	EXPECT_EQ(-1, out);

	out = -1;
	EXPECT_FALSE(strToNum("0x", out, std::hex));
	EXPECT_EQ(-1, out);

	out = -1;
	EXPECT_FALSE(strToNum("xx", out, std::hex));
	EXPECT_EQ(-1, out);

	out = -1;
	EXPECT_FALSE(strToNum("0xF bbb", out, std::hex));
	EXPECT_EQ(-1, out);

	out = -1;
	EXPECT_FALSE(strToNum("0xFwww", out, std::hex));
	EXPECT_EQ(-1, out);
}

TEST_F(ConversionTests,
StrToNumConversionFailsWhenConvertingNegativeNumberIntoUnsignedInt) {
	unsigned out = 0;
	EXPECT_FALSE(strToNum("-1", out, std::dec));
	EXPECT_EQ(0, out);

	out = 0;
	EXPECT_FALSE(strToNum("+-1", out, std::dec));
	EXPECT_EQ(0, out);
}

//
// bytesToBits()
//

TEST_F(ConversionTests,
BytesToBits) {
	std::vector<std::uint8_t> vec;
	EXPECT_EQ(bytesToBits(vec.data(), vec.size()), "");

	vec = { 0xAB };
	EXPECT_EQ(bytesToBits(vec.data(), vec.size()), "10101011");

	vec = { 0x11, 0x55, 0xFF };
	EXPECT_EQ(bytesToBits(vec.data(), vec.size()), "000100010101010111111111");

	std::vector<std::uint16_t> u16vec = { 0xDEAD, 0xBEEF };
	EXPECT_EQ(bytesToBits(u16vec), "1010110111101111");
}

//
// double10toDouble8()
//

TEST_F(ConversionTests,
double10ToDouble8Success) {
	std::vector<unsigned char> dest;
	std::vector<unsigned char> src = {0x60, 0xe5, 0xd0, 0x22, 0xdb, 0xf9, 0x7e, 0xf2, 0x00, 0x40}; // 80-bit double for 3.789
	std::vector<unsigned char> ok = {0x1c, 0x5a, 0x64, 0x3b, 0xdf, 0x4f, 0x0e, 0x40}; // 64-bit double for 3.789

	double10ToDouble8(dest, src);

	EXPECT_TRUE(dest == ok);
}

//
// byteSwap16()
//

TEST_F(ConversionTests,
byteSwap16Success) {
	EXPECT_EQ(0x0, byteSwap16(0x0));
	EXPECT_EQ(0x1200, byteSwap16(0x0012));
	EXPECT_EQ(0x0012, byteSwap16(0x1200));
	EXPECT_EQ(0x3412, byteSwap16(0x1234));
}

//
// byteSwap32()
//

TEST_F(ConversionTests,
byteSwap32Success) {
	EXPECT_EQ(0x0, byteSwap32(0x0));
	EXPECT_EQ(0x12000000, byteSwap32(0x00000012));
	EXPECT_EQ(0x12340000, byteSwap32(0x00003412));
	EXPECT_EQ(0x12345600, byteSwap32(0x00563412));
	EXPECT_EQ(0x12345678, byteSwap32(0x78563412));
}

//
// byteSwap16()
//

TEST_F(ConversionTests,
byteSwap16SSuccess) {
	EXPECT_EQ("0000000000000000", byteSwap16("0000000000000000"));
	EXPECT_EQ("1010101000000000", byteSwap16("0000000010101010"));
	EXPECT_EQ("0000000010101010", byteSwap16("1010101000000000"));
	EXPECT_EQ("1111111110101010", byteSwap16("1010101011111111"));
}

//
// byteSwap32()
//

TEST_F(ConversionTests,
byteSwap32SSuccess) {
	EXPECT_EQ("00000000000000000000000000000000", byteSwap32("00000000000000000000000000000000"));
	EXPECT_EQ("11111111000000000000000000000000", byteSwap32("00000000000000000000000011111111"));
	EXPECT_EQ("00000000111111110000000000000000", byteSwap32("00000000000000001111111100000000"));
	EXPECT_EQ("00000000000000001111111100000000", byteSwap32("00000000111111110000000000000000"));
	EXPECT_EQ("00000000000000000000000011111111", byteSwap32("11111111000000000000000000000000"));
}

//
// hexStringToBytes()
//

TEST_F(ConversionTests,
hexStringToBytesSuccess) {
	EXPECT_EQ(hexStringToBytes("0b84d1a0806040"), hexStringToBytes("0b 84 d1 a0 80 60 40"));
	std::vector<uint8_t> vres = {0x0b, 0x84, 0xd1, 0xa0, 0x80, 0x60, 0x40};
	EXPECT_EQ(vres, hexStringToBytes("0b 84 d1 a0 80 60 40"));
}

//
// bytesToHexString()
//

TEST_F(ConversionTests,
bytesToHexStringSuccess) {
	std::vector<uint8_t> vres = {0x0b, 0x84, 0xd1, 0xa0, 0x80, 0x60, 0x40};
	std::string res;
	bytesToHexString(vres, res, 0, 0, false, true);
	EXPECT_EQ("0b 84 d1 a0 80 60 40", res);
}

// ─── wrapping length arithmetic ──────────────────────────────────────────────
//
// `offset + size > dataSize` forms the sum before testing it. At offset 1 and
// size SIZE_MAX it wraps to 0, the clamp does not fire, and the size survives
// unchanged into `size * 2` -- which wraps in turn, so `result.resize()` gets a
// small number and the loop then writes 2*SIZE_MAX characters into it.

TEST_F(ConversionTests, BytesToHexStringSizeThatWrapsIsClamped)
{
	const std::uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
	std::string result;
	bytesToHexString(data, sizeof(data), result, 1, std::numeric_limits<std::size_t>::max());
	// Three bytes remain from offset 1, so six hex characters.
	EXPECT_EQ("020304", result);
}

TEST_F(ConversionTests, BytesToHexStringOffsetPastEndProducesNothing)
{
	const std::uint8_t data[] = {0x01, 0x02};
	std::string result = "stale";
	bytesToHexString(data, sizeof(data), result, 64, 4);
	EXPECT_TRUE(result.empty());
}

TEST_F(ConversionTests, BytesToStringSizeThatWrapsIsClamped)
{
	const std::uint8_t data[] = {'a', 'b', 'c', 'd'};
	std::string result;
	bytesToString(data, sizeof(data), result, 1, std::numeric_limits<std::size_t>::max());
	EXPECT_EQ("bcd", result);
}

// --- hexStringToBytes parses whole bytes, or nothing ---
//
// The loop this replaces was
//     for (unsigned int i = 0; i < hex.length(); i += 2)
//         bytes.push_back(strtol(hex.substr(i, 2).c_str(), nullptr, 16));
// and it had three defects. The counter is `unsigned int` against a
// std::string::size_type bound (ESBMC: length = 0xFC0000007FFFFFFF, i =
// 4294967294, `i += 2` overflows back to 0 and the loop never terminates); an
// odd length is accepted (ESBMC refutes "push_back count * 2 == character
// count" at len = 7); and strtol returns a silent 0 for a non-hex substring
// (ESBMC: a = 'l', b = 'K', v = 0, indistinguishable from "00").
//
// The counter defect needs a 4-gigacharacter string to exercise and so is not
// unit-testable here; it is gone by construction, because txt::hexToBytes
// counts with a std::size_t against a std::size_t bound. The other two are
// below.

TEST_F(ConversionTests, HexStringToBytesRefusesAnOddNumberOfCharacters)
{
	// "abc" used to parse as ab 0c: the lone 'c' became a whole byte with the
	// nibble that was written as the HIGH half sitting in the LOW half.
	EXPECT_TRUE(hexStringToBytes("abc").empty());
	EXPECT_TRUE(hexStringToBytes("0b84d1a0806040f").empty());
	// Whitespace is removed first, so this is seven characters, not eight.
	EXPECT_TRUE(hexStringToBytes("0b 84 d1 a").empty());
}

TEST_F(ConversionTests, HexStringToBytesRefusesNonHexCharacters)
{
	// strtol returned 0 here with nothing the caller could inspect, so a
	// corrupt dump became a run of NUL bytes that every caller believed.
	EXPECT_TRUE(hexStringToBytes("lK").empty());
	EXPECT_TRUE(hexStringToBytes("zz").empty());
	// One bad byte refuses the whole run rather than contributing a zero.
	EXPECT_TRUE(hexStringToBytes("0b84zz").empty());
	EXPECT_TRUE(hexStringToBytes("0b 84 d1 a0 8g").empty());
}

TEST_F(ConversionTests, HexStringToBytesStillAcceptsWellFormedInput)
{
	const std::vector<uint8_t> expected = {0x0b, 0x84, 0xd1, 0xa0, 0x80, 0x60, 0x40};
	EXPECT_EQ(expected, hexStringToBytes("0b84d1a0806040"));
	EXPECT_EQ(expected, hexStringToBytes("0b 84 d1 a0 80 60 40"));
	EXPECT_EQ(expected, hexStringToBytes("0B84D1A0806040"));
	EXPECT_TRUE(hexStringToBytes("").empty());
}

// --- bytesToBits never left-shifts a negative value ---
//
// The body was `((item << j) & 0x80)` in a template instantiated for
// std::int8_t. For any element with the top bit set, `item` promotes to a
// negative int and `item << j` left-shifts a negative value -- undefined
// behaviour in C++17. ESBMC's witness is item = -96 (the byte 0xA0) with j = 7:
// "undefined behavior on shift operation shl". On x86 the answer happened to
// come out right, so this test pins the answer while
// -fsanitize=undefined -fno-sanitize-recover=undefined is what makes the old
// body abort: "runtime error: left shift of negative value -96".

TEST_F(ConversionTests, BytesToBitsRendersSignedBytesWithTheTopBitSet)
{
	const std::vector<std::int8_t> vec = {static_cast<std::int8_t>(0xA0)};
	EXPECT_EQ("10100000", bytesToBits(vec.data(), vec.size()));

	const std::vector<std::int8_t> all = {
		static_cast<std::int8_t>(0x80),
		static_cast<std::int8_t>(0xFF),
		static_cast<std::int8_t>(0x7F)};
	EXPECT_EQ("100000001111111101111111", bytesToBits(all.data(), all.size()));
}

TEST_F(ConversionTests, BytesToBitsRefusesALengthWhoseBitCountDoesNotFit)
{
	// `result.reserve(dataSize * BITS_IN_BYTE)` formed the product first, so a
	// dataSize above SIZE_MAX/8 wrapped to a small reservation while the loop
	// still appended eight characters per element. There is no answer to give:
	// the string would be longer than the address space. No element is read,
	// so the pointer is never dereferenced.
	const std::uint8_t one = 0x5A;
	EXPECT_TRUE(bytesToBits(&one, std::numeric_limits<std::size_t>::max()).empty());
	EXPECT_TRUE(
		bytesToBits(&one, std::numeric_limits<std::size_t>::max() / 8 + 1).empty());
	// One below the limit is representable, so it is not refused here -- it is
	// refused by the allocator, which is a different and honest failure.
	EXPECT_EQ("01011010", bytesToBits(&one, 1));
}

} // namespace tests
} // namespace utils

} // namespace retdec
