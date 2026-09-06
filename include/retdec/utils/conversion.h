/**
* @file include/retdec/utils/conversion.h
* @brief Conversion utilities.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_UTILS_CONVERSION_H
#define RETDEC_UTILS_CONVERSION_H

#include <cstdint>
#include <iomanip>
#include <ios>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "retdec/utils/bounds.h"
#include "retdec/utils/text_transcode.h"

namespace retdec {
namespace utils {

/// @name Conversions
/// @{

char* byteToHexString(uint8_t b, bool uppercase = true);

/**
 * Converts the given array of numbers into a hexadecimal string representation
 * @param data Array to be converted into a hexadecimal string
 * @param dataSize Size of array
 * @param result Into this parameter the result is stored
 * @param offset First byte from @a data which will be converted
 * @param size Number of bytes from @a data for conversion
 *    (0 means all bytes from @a offset)
 * @param uppercase @c true if hex letters (A-F) should be uppercase
 * @param spacing insert ' ' between every byte
 */
template<typename N> void bytesToHexString(
		const N *data,
		std::size_t dataSize,
		std::string &result,
		std::size_t offset = 0,
		std::size_t size = 0,
		bool uppercase = true,
		bool spacing = false)
{
	if (data == nullptr || offset >= dataSize)
	{
		// Clear rather than return silently: `result` is an output parameter,
		// and leaving the caller's previous value in it means a rejected
		// request looks like a successful one. bytesToString below already
		// clears on the same path; the two disagreed.
		result.clear();
		return;
	}

	// `offset + size > dataSize` forms the sum first: at offset 1 and size
	// SIZE_MAX it wraps to 0, the test is false, and `size` survives the clamp
	// unchanged. rangeFits asks the same question without forming it.
	size = (size == 0 || !bounds::rangeFits(offset, dataSize, size))
			? bounds::remaining(offset, dataSize)
			: size;

	std::size_t hexIndex = 0;

	// Two characters per byte, three with spacing. Both products are formed
	// from a length that came from a file, and `result.resize(wrapped)` followed
	// by a loop writing 2*size characters is a heap overflow rather than a
	// short string. Refuse rather than truncate: a caller asking for a
	// rendering that cannot be represented has asked for nothing.
	const std::size_t perByte = spacing ? 3 : 2;
	if (!bounds::mulFits(size, perByte))
	{
		result.clear();
		return;
	}
	std::size_t sz = spacing ? (size * 3 - 1) : (size * 2);
	result.resize(sz);

	for (std::size_t i = 0; i < size; ++i)
	{
		if (spacing && hexIndex > 0)
		{
			result[hexIndex++] = ' ';
		}
		auto res = byteToHexString(data[offset + i], uppercase);
		result[hexIndex++] = res[0];
		result[hexIndex++] = res[1];
	}
}

/**
 * Converts the given vector of numbers into a hexadecimal string representation
 * @param bytes Vector to be converted into a hexadecimal string
 * @param result Into this parameter the result is stored
 * @param offset First byte from @a bytes which will be converted
 * @param size Number of bytes from @a bytes for conversion
 *    (0 means all bytes from @a offset)
 * @param uppercase @c true if hex letters (A-F) should be uppercase
 * @param spacing insert ' ' between every byte
 */
template<typename N> void bytesToHexString(
		const std::vector<N> &bytes,
		std::string &result,
		std::size_t offset = 0,
		std::size_t size = 0,
		bool uppercase = true,
		bool spacing = false)
{
	bytesToHexString(
			bytes.data(),
			bytes.size(),
			result,
			offset,
			size,
			uppercase,
			spacing
	);
}

/**
* @brief Converts the given integer into its hexadecimal representation.
*
* @param[in] w Number to be converted.
* @param[in] addBase Prepends "0x" before the result.
* @param[in] fillToN If needed, prepends "0" before the result to get at least
*                    @c fillToN characters long string.
*
* All letters in the result are lowercase.
*/
template<typename I>
std::string intToHexString(I w, bool addBase = false, unsigned fillToN = 0)
{
	static const char* digits = "0123456789abcdef";

	size_t hex_len = sizeof(I)<<1;

	std::string rc(hex_len,'0');
	for (size_t i = 0, j = (hex_len-1)*4 ; i < hex_len; ++i, j -= 4)
	{
		rc[i] = digits[(w>>j) & 0x0f];
	}

	bool started = false;
	std::string res;
	size_t j = 0;
	if (addBase)
	{
		res.resize(rc.size() + 2);
		res[0] = '0';
		res[1] = 'x';
		j = 2;
	}
	else
	{
		res.resize(rc.size());
	}
	for (size_t i = 0; i < rc.size(); ++i)
	{
		if (started)
		{
			res[j++] = rc[i];
		}
		else if (rc[i] != '0' || (rc.size() - i <= fillToN) || (i == rc.size() - 1))
		{
			res[j++] = rc[i];
			started = true;
		}
	}
	res.resize(j);

	return res;
}

std::vector<uint8_t> hexStringToBytes(const std::string& hexIn);

/**
* @brief Converts the given string into a number.
*
* @param[in] str String to be converted into a number.
* @param[out] number Into this parameter the resulting number is stored.
* @param[in] format Number format (e.g. std::dec, std::hex).
*
* @return @c true if the conversion went ok, @c false otherwise.
*
* If the conversion fails, @a number is left unchanged.
*/
template<typename N>
inline bool strToNum(const std::string &str, N &number,
		std::ios_base &(* format)(std::ios_base &) = std::dec) {
	std::istringstream strStream(str);
	N convNumber = 0;
	strStream >> format >> convNumber;
	if (strStream.fail() || !strStream.eof()) {
		return false;
	}

	// The above checks do not detect conversion of a negative number into an
	// unsigned integer. We have to perform an additional check here.
	if (std::is_unsigned<N>::value && str[0] == '-') {
		return false;
	}

	number = convNumber;
	return true;
}

namespace
{
	const std::size_t BITS_IN_BYTE = 8;
}

/**
 * @brief Converts the given array of numbers into a bits.
 *
 * @param[in] data Array of numbers.
 * @param[in] dataSize Size of array.
 *
 * @return Resulting string.
 */
template<typename N>
std::string bytesToBits(const N *data, std::size_t dataSize) {
	if(!data) {
		dataSize = 0;
	}

	// `result.reserve(dataSize * BITS_IN_BYTE)` formed the product first, so a
	// dataSize above SIZE_MAX/8 wrapped to a small reservation while the loop
	// still appended eight characters per element. bitsCapacity refuses the
	// rendering outright when 8*n is not representable, which is the only
	// honest answer: a caller asking for a string longer than the address space
	// has asked for nothing.
	const std::size_t need = txt::bitsCapacity(dataSize);
	if (need == 0) {
		return std::string();
	}

	std::string result(need, '0');

	for (std::size_t i = 0; i < dataSize; ++i) {
		// The old body was `((item << j) & 0x80)`, and this template is
		// instantiated for std::int8_t. For any element with the top bit set,
		// `item` promotes to a NEGATIVE int and `item << j` left-shifts a
		// negative value, which is undefined behaviour in C++17 -- not a wrong
		// answer, no answer. ESBMC's witness is item = -96 (the byte 0xA0) with
		// j = 7: "undefined behavior on shift operation shl".
		//
		// Only bits 0..7 of the promoted value were ever inspected (`& 0x80`
		// after a left shift of at most 7), so converting the element to
		// std::uint8_t first reproduces the intended rendering bit for bit --
		// the same low byte, most significant bit first -- and hands it to
		// txt::bytesToBits, which shifts an unsigned value RIGHT and
		// so cannot be undefined at any element value.
		const std::uint8_t byte = static_cast<std::uint8_t>(data[i]);
		// need == dataSize * 8 exactly, so this window is inside the string.
		txt::bytesToBits(
				&byte, 1, &result[i * BITS_IN_BYTE], BITS_IN_BYTE);
	}

	return result;
}

/**
 * @brief Converts the given vector of numbers into a bits.
 *
 * @param[in] bytes Vector to be converted into a bits.
 *
 * @return Resulting string.
 */
template<typename N>
std::string bytesToBits(const std::vector<N> &bytes) {
	return bytesToBits(bytes.data(), bytes.size());
}
/**
 * Converts the given array of numbers into a string
 * @param data Array to be converted into a string
 * @param dataSize Size of array
 * @param result Into this parameter the result is stored
 * @param offset First byte from @a data which will be converted to string
 * @param size Number of bytes from @a data for conversion
 *    (0 means all bytes from @a offset)
 */
template<typename N> void bytesToString(
		const N *data,
		std::size_t dataSize,
		std::string &result,
		std::size_t offset = 0,
		std::size_t size = 0)
{
	if(!data)
	{
		dataSize = 0;
	}

	if(offset >= dataSize)
	{
		size = 0;
	}
	else
	{
		// Same wrapping sum as bytesToHexString above; same fix. Here the
		// consequence is `std::string(data + offset, size)` reading `size`
		// bytes from a buffer that has fewer.
		size = (size == 0 || !bounds::rangeFits(offset, dataSize, size))
				? bounds::remaining(offset, dataSize)
				: size;
	}

	result.clear();
	result.reserve(size);
	result = std::string(reinterpret_cast<const char*>(data + offset), size);
}

/**
 * Converts the given vector of numbers into a string
 * @param bytes Vector to be converted into a string
 * @param result Into this parameter the result is stored
 * @param offset First byte from @a bytes which will be converted to string
 * @param size Number of bytes from @a bytes for conversion
 *    (0 means all bytes from @a offset)
 */
template<typename N> void bytesToString(
		const std::vector<N> &bytes,
		std::string &result,
		std::size_t offset = 0,
		std::size_t size = 0)
{
	bytesToString(bytes.data(), bytes.size(), result, offset, size);
}

void double10ToDouble8(std::vector<unsigned char> &dest,
	const std::vector<unsigned char> &src);

unsigned short byteSwap16(unsigned short val);
unsigned int byteSwap32(unsigned int val);
std::string byteSwap16(const std::string &val);
std::string byteSwap32(const std::string &val);

/// @}

} // namespace utils
} // namespace retdec

#endif
