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
 * @brief Convert 80-bit (10-byte) <tt>long double</tt> binary data (byte array)
 *        into 64-bit (8-byte) <tt>double</tt> binary data.
 *
 * @param[out] dest 64-bit double to create, or left EMPTY when @a src is too
 *             short to hold an 80-bit datum.
 * @param[in] src 80-bit long double to convert; must be at least
 *            @c kExtendedBytes bytes.
 */
void double10ToDouble8(std::vector<unsigned char>& dest, const std::vector<unsigned char>& src)
{
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
	if (!bounds::rangeFits(0, src.size(), kExtendedBytes))
	{
		return;
	}

	dest.resize(8, 0);

	int expo, i, sign;
	// exponents 15 -> 11 bits
	sign = src[9] & 0x80;
	expo = (src[9] & 0x7f) << 8 | src[8];
	if (expo == 0)
	{
	nul:
		if (sign) dest[7] |= 0x80;
		return;
	}
	expo -= 16383; // - bias long double
	expo += 1023;  // + bias for double
	if (expo <= 0) // underflow
		goto nul;
	if (expo > 0x7ff)
	{ // inf/nan
		dest[7] = 0x7f;
		dest[6] = src[7] == 0xc0 ? 0xf8 : 0xf0;
		goto nul;
	}
	expo <<= 4;
	dest[6] = expo & 0xff;
	dest[7] = (expo & 0x7f00) >> 8;
	if (sign) dest[7] |= 0x80;
	// long double frac 63 bits => 52 bits src[7] &= 0x7f; reset intbit 63.
	for (i = 0; i < 6; ++i)
	{
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
unsigned short byteSwap16(unsigned short val)
{
	return (0xFF00 & val) >> 8 | (0xFF & val) << 8;
}

/**
 * @brief Swap bytes for Intel x86 32-bit little-endian immediate.
 *
 * @param val Original value.
 *
 * @return Value with swapped bytes
 */
unsigned int byteSwap32(unsigned int val)
{
	return (0xFF000000 & val) >> 24 | (0xFF0000 & val) >> 8 | (0xFF00 & val) << 8 | (0xFF & val) << 24;
}

/**
 * @brief Swap bytes for Intel x86 16-bit little-endian immediate.
 *
 * @param val Original value.
 *
 * @return Value with swapped bytes or original value if its size is not 16.
 */
std::string byteSwap16(const std::string& val)
{
	if (val.length() != 16) return val;

	return val.substr(8, 8) + val.substr(0, 8);
}

/**
 * @brief Swap bytes for Intel x86 32-bit little-endian immediate.
 *
 * @param val Original value.
 *
 * @return Value with swapped bytes or original value if its size is not 32.
 */
std::string byteSwap32(const std::string& val)
{
	if (val.length() != 32) return val;

	return val.substr(24, 8) + val.substr(16, 8) + val.substr(8, 8) + val.substr(0, 8);
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
