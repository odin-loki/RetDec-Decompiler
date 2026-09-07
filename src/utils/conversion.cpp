/**
* @file src/utils/conversion.cpp
* @brief Implementation of the conversion utilities.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <bitset>
#include <cstring>

#include "retdec/utils/conversion.h"
#include "retdec/utils/string.h"

namespace retdec {
namespace utils {

/**
 * From https://johnnylee-sde.github.io/Fast-unsigned-integer-to-hex-string/
 */
char* byteToHexString(uint8_t b, bool uppercase)
{
	static char result[3] = {'\0', '\0', '\0'};
	static const char digits[513] =
		"000102030405060708090A0B0C0D0E0F"
		"101112131415161718191A1B1C1D1E1F"
		"202122232425262728292A2B2C2D2E2F"
		"303132333435363738393A3B3C3D3E3F"
		"404142434445464748494A4B4C4D4E4F"
		"505152535455565758595A5B5C5D5E5F"
		"606162636465666768696A6B6C6D6E6F"
		"707172737475767778797A7B7C7D7E7F"
		"808182838485868788898A8B8C8D8E8F"
		"909192939495969798999A9B9C9D9E9F"
		"A0A1A2A3A4A5A6A7A8A9AAABACADAEAF"
		"B0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF"
		"C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF"
		"D0D1D2D3D4D5D6D7D8D9DADBDCDDDEDF"
		"E0E1E2E3E4E5E6E7E8E9EAEBECEDEEEF"
		"F0F1F2F3F4F5F6F7F8F9FAFBFCFDFEFF";
	static const char digitsLowerAlpha[513] =
		"000102030405060708090a0b0c0d0e0f"
		"101112131415161718191a1b1c1d1e1f"
		"202122232425262728292a2b2c2d2e2f"
		"303132333435363738393a3b3c3d3e3f"
		"404142434445464748494a4b4c4d4e4f"
		"505152535455565758595a5b5c5d5e5f"
		"606162636465666768696a6b6c6d6e6f"
		"707172737475767778797a7b7c7d7e7f"
		"808182838485868788898a8b8c8d8e8f"
		"909192939495969798999a9b9c9d9e9f"
		"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
		"b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
		"c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
		"d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
		"e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
		"f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

	const char* lut = uppercase ? digits : digitsLowerAlpha;
	std::size_t pos = b * 2;
	result[0] = lut[pos];
	result[1] = lut[pos+1];

	return &(result[0]);
}

/**
* @brief Convert 80-bit (10-byte) <tt>long double</tt> binary data (byte array)
*        into 64-bit (8-byte) <tt>double</tt> binary data.
*
* @param[out] dest 64-bit double to create, or left EMPTY when @a src is too
*             short to hold an 80-bit datum.
* @param[in] src 80-bit long double to convert; must be at least
*            @c kExtendedBytes bytes.
*/
void double10ToDouble8(std::vector<unsigned char> &dest,
		const std::vector<unsigned char> &src) {
	// Taken from:
	// http://blogs.perl.org/users/rurban/2012/09/reading-binary-floating-point-numbers-numbers-part2.html
	dest.clear();

	// The body below subscripts src at 1 and at 7, 8 and 9 unconditionally, and
	// at 2..7 in the fraction loop, so it needs kExtendedBytes = 10 elements.
	// Nothing used to check that, and this is public API declared in
	// include/retdec/utils/conversion.h with no precondition a caller could
	// have read. On a four-byte src, ASan reports
	// "heap-buffer-overflow ... READ of size 1 ... in
	// retdec::utils::double10ToDouble8" at the `src[9] & 0x80` below -- five
	// bytes past a four-byte region.
	//
	// bounds::rangeFits asks whether the 10-byte datum lies inside the vector
	// without forming `0 + 10`; it is the same kernel bytesToString and
	// bytesToHexString in conversion.h use for the identical question.
	//
	// A refusal leaves dest EMPTY rather than eight zero bytes, so a caller can
	// tell "the input was too short" from "the input encoded +0.0" -- eight
	// zero bytes is a valid answer for the latter. ByteValueStorage::
	// get10ByteImpl is the one in-tree caller and it now checks dest.size().
	if (!bounds::rangeFits(0, src.size(), kExtendedBytes)) {
		return;
	}

	dest.resize(8, 0);

	int expo, i, sign;
	// exponents 15 -> 11 bits
	sign = src[9] & 0x80;
	expo = (src[9] & 0x7f)<< 8 | src[8];
	if (expo == 0) {
	nul:
		if (sign)
			dest[7] |= 0x80;
		return;
	}
	expo -= 16383;       // - bias long double
	expo += 1023;        // + bias for double
	if (expo <= 0)       // underflow
		goto nul;
	if (expo > 0x7ff) {  // inf/nan
		dest[7] = 0x7f;
		dest[6] = src[7] == 0xc0 ? 0xf8 : 0xf0 ;
		goto nul;
	}
	expo <<= 4;
	dest[6] = expo & 0xff;
	dest[7] = (expo & 0x7f00) >> 8;
	if (sign)
		dest[7] |= 0x80;
	// long double frac 63 bits => 52 bits src[7] &= 0x7f; reset intbit 63.
	for (i = 0; i < 6; ++i) {
		dest[i + 1] |= (i == 5 ? src[7] & 0x7f : src[i + 2]) >> 3;
		dest[i] |= (src[i + 2] & 0x1f) << 5;
	}
	dest[0] |= src[1] >> 3;
}

/**
* @brief Swap bytes for Intel x86 16-bit little-endian immediate.
*
* @param val Original value.
*
* @return Value with swapped bytes
*/
unsigned short byteSwap16(unsigned short val) {
	return (0xFF00 & val) >> 8 | (0xFF & val) << 8;
}

/**
* @brief Swap bytes for Intel x86 32-bit little-endian immediate.
*
* @param val Original value.
*
* @return Value with swapped bytes
*/
unsigned int byteSwap32(unsigned int val) {
	return (0xFF000000 & val) >> 24 |
		(0xFF0000 & val) >> 8 |
		(0xFF00 & val) << 8 |
		(0xFF & val) << 24;
}

/**
* @brief Swap bytes for Intel x86 16-bit little-endian immediate.
*
* @param val Original value.
*
* @return Value with swapped bytes or original value if its size is not 16.
*/
std::string byteSwap16(const std::string &val) {
	if (val.length() != 16)
		return val;

	return val.substr(8, 8) + val.substr(0, 8);
}

/**
* @brief Swap bytes for Intel x86 32-bit little-endian immediate.
*
* @param val Original value.
*
* @return Value with swapped bytes or original value if its size is not 32.
*/
std::string byteSwap32(const std::string &val) {
	if (val.length() != 32)
		return val;

	return val.substr(24, 8) + val.substr(16, 8) +
		val.substr(8, 8) + val.substr(0, 8);
}

/**
 * Convert hexadecimal string @c hexIn string into bytes.
 * There might be whitespaces in the string, e.g. "0b 84 d1 a0 80 60 40" is
 * the same as "0b84d1a0806040".
 */
std::vector<uint8_t> hexStringToBytes(const std::string& hexIn)
{
	std::vector<uint8_t> bytes;

	auto hex = removeWhitespace(hexIn);

	// Three separate defects lived in the loop this replaces:
	//
	//     for (unsigned int i = 0; i < hex.length(); i += 2) {
	//         std::string byteString = hex.substr(i, 2);
	//         char byte = strtol(byteString.c_str(), nullptr, 16);
	//         bytes.push_back(byte);
	//     }
	//
	//  1. The counter is `unsigned int` against a std::string::size_type bound,
	//     so it cannot represent the indices it must reach. ESBMC's witness is
	//     length = 0xFC0000007FFFFFFF with i = 4294967294: `i += 2` overflows,
	//     the counter returns to 0, and the loop never terminates.
	//  2. An odd number of characters is accepted. "abc" steps to i = 2 and
	//     takes the one-character substring "c", which strtol parses as the
	//     whole byte 0x0c -- so "abc" becomes ab 0c, and the nibble the writer
	//     put in the HIGH half arrives in the LOW half. ESBMC refutes
	//     "push_back count * 2 == character count" at len = 7, where the loop
	//     produces four bytes from seven characters.
	//  3. strtol reports nothing the caller inspects. For the two characters
	//     'l' and 'K' it returns 0, indistinguishable from "00", so a corrupt
	//     hex dump silently becomes a run of NUL bytes that every caller
	//     believes it parsed.
	//
	// txt::hexToBytes counts with a std::size_t against a std::size_t bound,
	// refuses an odd length outright, and refuses the whole run on the first
	// character that is not a hex digit -- txt::hexValue returns -1 exactly
	// where strtol returns a silent 0.
	//
	// A malformed run now yields an empty vector rather than a plausible one.
	// That is the point: the two callers (src/capstone2llvmirtool and the
	// capstone2llvmir test harness) hand this a hand-written instruction
	// encoding, and "you typed something that is not hex" has to be
	// distinguishable from "you asked me to disassemble zero bytes".
	bytes.resize(hex.length() / txt::kHexCharsPerByte);

	std::size_t written = 0;
	if (!txt::hexToBytes(hex.data(), hex.length(), bytes.data(), bytes.size(), written))
	{
		bytes.clear();
		return bytes;
	}

	bytes.resize(written);
	return bytes;
}

} // namespace utils
} // namespace retdec
