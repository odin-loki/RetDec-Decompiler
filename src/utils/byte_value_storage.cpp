/**
 * @file src/utils/byte_value_storage.cpp
 * @brief Implementation of @c ByteValueStorage.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */
#include <cassert>
#include <cstring>

#include "retdec/utils/bounds.h"
#include "retdec/utils/byte_order.h"
#include "retdec/utils/byte_value_storage.h"
#include "retdec/utils/conversion.h"
#include "retdec/utils/scan_cursor.h"
#include "retdec/utils/string.h"
#include "retdec/utils/system.h"

namespace retdec {
namespace utils {

namespace {

/**
 * Convert endianness
 *
 * @param str String which will be converted
 * @param items Number of items in one word of converted string
 * @param length Length of one item in word
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool swapEndianness(std::string &str, std::size_t items, std::size_t length = 1)
{
	// `str.size() < items * length` formed the product before testing it, and
	// both factors are virtual-call results a subclass supplies: items is
	// getBytesPerWord() and length is getNumberOfNibblesInByte() at the
	// hexToBig/hexToLittle call sites. A product that wraps compares as small,
	// the guard passes, and the same wrapped value is then used again as the
	// modulus of `str.size() % (items * length)` and as the stride of the loop
	// below -- so the swap runs with a word size nothing in the string matches.
	//
	// bounds::mulFits answers "is items * length representable" without forming
	// it, which is the same kernel bytesToHexString and txt::bitsCapacity use.
	// Only once it says yes is the product computed, and then it is computed
	// once and reused rather than re-derived at each of the three sites.
	if (!length || !items || !bounds::mulFits(items, length))
	{
		return false;
	}

	const std::size_t wordLen = items * length;
	if (str.size() < wordLen)
	{
		return false;
	}

	const auto middleWordIndex = items / 2;
	const auto middleLengthIndex = length / 2;
	const auto wasteLen = str.size() % wordLen;
	str.erase(str.size() - wasteLen, wasteLen);

	for (std::size_t i = 0, e = str.size(); i < e; i += wordLen)
	{
		for (std::size_t j = 0; j < middleWordIndex; ++j)
		{
			for (std::size_t k = 0; k < length; ++k)
			{
				std::swap(
						str[i + j * length + k],
						str[i + (items - j) * length - k - 1]
				);
			}
		}

		if (middleWordIndex && middleLengthIndex)
		{
			for (std::size_t j = 0; j < items; ++j)
			{
				for (std::size_t k = 0; k < middleLengthIndex; ++k)
				{
					std::swap(
							str[i + j * length + k],
							str[i + (j + 1) * length - k - 1]
					);
				}
			}
		}
	}

	return true;
}

/**
 * Convert endianness
 *
 * @param values Values for conversion
 * @param items Number of bits in one value from @a values
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool swapEndianness(std::vector<unsigned char>& values, std::size_t items)
{
	// The mask this walks down from used to be built by
	// `unsigned char a = 1; a <<= (sizeof(a) * items) - 1;` with items =
	// getByteLength(), a virtual-call result no caller bounds. sizeof(a) is 1,
	// so the count is items - 1, and `a` promotes to int before the shift: at
	// getByteLength() = 64 that is a shift of 63 on a 32-bit int, which UBSan
	// reports as "shift exponent 63 is too large for 32-bit type int". Between
	// 9 and 32 there is no undefined behaviour and no answer either -- the bit
	// `1 << (items - 1)` sets does not survive the conversion back to unsigned
	// char, so `a` is 0, the while loop below never executes, and every element
	// is silently overwritten with 0.
	//
	// items counts the BITS of one value, and a value here is one unsigned
	// char, so a reversal is defined only for 1..kBitsPerByte. Anything wider
	// is refused, exactly as ByteValueStorage::createBytesFromValue refuses a
	// getByteLength() that is not kBitsPerByte: every getByteLength() in this
	// tree reports 8, and a silent wrong answer for the widths that do not is
	// worse than a refusal.
	if (!items || items > byteorder::kBitsPerByte)
	{
		return false;
	}

	// Built in unsigned int and narrowed once, so the shift count -- at most
	// kBitsPerByte - 1 = 7 -- is defined for the type it acts on. For every
	// items in 1..8 this is the value the old expression produced.
	const unsigned char top = static_cast<unsigned char>(1u << (items - 1));

	for (std::size_t i = 0, e = values.size(); i < e; ++i)
	{
		unsigned char a = top, b = 1, y = 0;

		while (a)
		{
			if (values[i] & b)
			{
				y |= a;
			}
			a >>= 1;
			b <<= 1;
		}

		values[i] = y;
	}

	return true;
}

} // anonymous namespace

/**
 * Get opposite endianness
 *
 * @return Endianness::LITTLE if input file is in big endian and vice versa
 * @retval Endianness::UNKNOWN if file endianness is unknown
 */
Endianness ByteValueStorage::getInverseEndianness() const
{
	switch (getEndianness())
	{
		case Endianness::LITTLE:
			return Endianness::BIG;
		case Endianness::BIG:
			return Endianness::LITTLE;
		case Endianness::UNKNOWN:
			return Endianness::UNKNOWN;
		default:
			assert(false && "Unexpected value of a switch expression");
			return Endianness::UNKNOWN;
	}
}

/**
 * Find out if endianness is little
 *
 * @return @c true if endianness is little, @c false otherwise
 */
bool ByteValueStorage::isLittleEndian() const
{
	return getEndianness() == Endianness::LITTLE;
}

/**
 * Find out if endianness is big
 *
 * @return @c true if endianness is big, @c false otherwise
 */
bool ByteValueStorage::isBigEndian() const
{
	return getEndianness() == Endianness::BIG;
}

/**
 * Find out if endianness is unknown
 *
 * @return @c true if endianness is unknown, @c false otherwise
 */
bool ByteValueStorage::isUnknownEndian() const
{
	return getEndianness() == Endianness::UNKNOWN;
}

/**
 * Convert hexadecimal string to big endian
 *
 * @param str String which will be converted
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::hexToBig(std::string& str) const
{
	if (isUnknownEndian())
	{
		return false;
	}

	return isBigEndian()
			? true
			: swapEndianness(
					str,
					getBytesPerWord(),
					getNumberOfNibblesInByte()
			);
}

/**
 * Convert hexadecimal string to little endian
 *
 * @param str String which will be converted
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::hexToLittle(std::string& str) const
{
	if (isUnknownEndian())
	{
		return false;
	}

	return isLittleEndian()
			? true
			: swapEndianness(
					str,
					getBytesPerWord(),
					getNumberOfNibblesInByte()
			);
}

/**
 * Convert bit string to big endian
 *
 * @param str String which will be converted
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::bitsToBig(std::string& str) const
{
	if (isUnknownEndian())
	{
		return false;
	}

	return isBigEndian() ? true : swapEndianness(str, getByteLength());
}

/**
 * Convert bit string to little endian
 *
 * @param str String which will be converted
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::bitsToLittle(std::string& str) const
{
	if (isUnknownEndian())
	{
		return false;
	}

	return isLittleEndian() ? true : swapEndianness(str, getByteLength());
}

/**
 * Convert bits to big endian
 *
 * @param values Bits for conversion stored as bytes
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::bitsToBig(std::vector<unsigned char>& values) const
{
	if (isUnknownEndian())
	{
		return false;
	}

	return isBigEndian() ? true : swapEndianness(values, getByteLength());
}

/**
 * Convert bits to little endian
 *
 * @param values Bits for conversion stored as bytes
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::bitsToLittle(std::vector<unsigned char>& values) const
{
	if (isUnknownEndian())
	{
		return false;
	}

	return isLittleEndian() ? true : swapEndianness(values, getByteLength());
}

/**
 * Get integer (1B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to get integer from
 * @param res Result integer
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get1Byte(
		std::uint64_t address,
		std::uint64_t& res,
		Endianness e) const
{
	return getXByte(address, 1, res, e);
}

/**
 * Get integer (2B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to get integer from
 * @param res Result integer
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get2Byte(
		std::uint64_t address,
		std::uint64_t& res,
		Endianness e) const
{
	return getXByte(address, 2, res, e);
}

/**
 * Get integer (4B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to get integer from
 * @param res Result integer
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get4Byte(
		std::uint64_t address,
		std::uint64_t& res,
		Endianness e) const
{
	return getXByte(address, 4, res, e);
}

/**
 * Get integer (8B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to get integer from
 * @param res Result integer
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get8Byte(
		std::uint64_t address,
		std::uint64_t& res,
		Endianness e) const
{
	return getXByte(address, 8, res, e);
}

/**
 * Get long double from the specified address.
 * If system has 80-bit (10-byte) long double, copy data directly.
 * Else convert 80-bit (10-byte) long double into 64-bit (8-byte) double.
 *
 * @param address Address to get double from
 * @param res Result double
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get10Byte(std::uint64_t address, long double& res) const
{
	std::vector<std::uint8_t> d10;
	if (!getXBytes(address, 10, d10))
	{
		return false;
	}

	if (!get10ByteImpl(d10, res))
	{
		return false;
	}

	return true;
}

/**
 * Get word located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to get integer from
 * @param res Result integer
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getWord(
		std::uint64_t address,
		std::uint64_t& res,
		Endianness e) const
{
	return getXByte(address, getBytesPerWord(), res, e);
}

/**
 * Get float from the specified address.
 *
 * @param address Address to get float from
 * @param res Result float
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getFloat(std::uint64_t address, float& res) const
{
	std::vector<std::uint8_t> d;
	if (!getXBytes(address, sizeof(float), d) || d.size() != sizeof(float))
	{
		return false;
	}

	memcpy(&res, d.data(), d.size());
	return true;
}

/**
 * Get double from the specified address.
 *
 * @param address Address to get double from
 * @param res Result double
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getDouble(std::uint64_t address, double& res) const
{
	std::vector<std::uint8_t> d;
	if (!getXBytes(address, sizeof(double), d) || d.size() != sizeof(double))
	{
		return false;
	}

	// 2.33 (0x4002a3d7 0a3d70a4) in data section as: d7a30240 a4703d0a
	// but only on old (pre version 5?) ARM architecture with ELF format.
	if (hasMixedEndianForDouble())
	{
		for (std::size_t i = 0; i < sizeof(double) / 2; ++i)
		{
			std::swap(d[i], d[i + 4]);
		}
	}
	// 2.33 (0x4002a3d7 0a3d70a4) in data section as: a4703d0a d7a30240.
	// New ARM compilers are also generating this kind of double constants.
	// We are not sure, what part of binary determines which kind of double
	// constants are used.
	// Currently we use new kind for ARMs > version 5.
	// To find relevant info, google: "ARM double mixed endian".

	memcpy(&res, d.data(), d.size());
	return true;
}

/**
 * Set integer (1B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to set integer at
 * @param val Integer to set
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::set1Byte(
		std::uint64_t address,
		std::uint64_t val,
		Endianness e)
{
	return setXByte(address, 1, val, e);
}

/**
 * Set integer (2B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to set integer at
 * @param val Integer to set
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::set2Byte(
		std::uint64_t address,
		std::uint64_t val,
		Endianness e)
{
	return setXByte(address, 2, val, e);
}

/**
 * Set integer (4B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to set integer at
 * @param val Integer to set
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::set4Byte(
		std::uint64_t address,
		std::uint64_t val,
		Endianness e)
{
	return setXByte(address, 4, val, e);
}

/**
 * Set integer (8B) located at provided address using the specified endian
 * or default file endian
 *
 * @param address Address to set integer at
 * @param val Integer to set
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::set8Byte(
		std::uint64_t address,
		std::uint64_t val,
		Endianness e)
{
	return setXByte(address, 8, val, e);
}

/**
 * Set long double at the specified address.
 * If system has 80-bit (10-byte) long double, copy data directly.
 * Else convert 80-bit (10-byte) long double into 64-bit (8-byte) double.
 *
 * @param address Address to set double at
 * @param val Double to set
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::set10Byte(std::uint64_t address, long double val)
{
	std::vector<std::uint8_t> bytes;
	if (systemHasLongDouble())
		bytes.resize(10);
	else
		bytes.resize(8);

	memcpy(bytes.data(), &val, bytes.size());
	return setXBytes(address, bytes);
}

/**
 * Set word located at provided address using the specified endian or default
 * file endian
 *
 * @param address Address to set integer at
 * @param val Integer to set
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::setWord(
		std::uint64_t address,
		std::uint64_t val,
		Endianness e)
{
	return setXByte(address, getBytesPerWord(), val, e);
}

/**
 * Set float at the specified address.
 *
 * @param address Address to set float at
 * @param val Float to set
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::setFloat(std::uint64_t address, float val)
{
	std::vector<std::uint8_t> bytes(sizeof(float));
	memcpy(bytes.data(), &val, sizeof(float));
	return setXBytes(address, bytes);
}

/**
 * Set double at the specified address.
 *
 * @param address Address to set double at
 * @param val Double to set
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::setDouble(std::uint64_t address, double val)
{
	std::vector<std::uint8_t> bytes(sizeof(double));
	memcpy(bytes.data(), &val, sizeof(double));

	if (hasMixedEndianForDouble())
	{
		for (std::size_t i = 0; i < sizeof(double) / 2; ++i)
		{
			std::swap(bytes[i], bytes[i + 4]);
		}
	}

	return setXBytes(address, bytes);
}

/**
 * Get NTBS (null-terminated byte string) from specified address
 *
 * @param address Address to get string from
 * @param res Result string
 * @param size Requested size of string (if @a size is zero,
 *             read until zero byte)
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getNTBS(
		std::uint64_t address,
		std::string& res,
		std::size_t size) const
{
	using namespace std::placeholders;

	GetNByteFn get1ByteFn = std::bind(
			&ByteValueStorage::get1Byte,
			this, _1, _2, _3
	);
	return getNTBSImpl(get1ByteFn, address, res, size);
}

/**
 * Get NTWS (null-terminated wide string) from the specified address
 *
 * @param address Address to get string from
 * @param width Byte width of one character
 * @param res Result character array
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 *
 * @note This will read items until it reaches zero (null terminator),
 *       it can potentially create huge non-nice vectors.
 *       Use this only if your are certain there is wide string on the address.
 *       See @c getNTWSNice() for a faster wide-string-probing method.
 */
bool ByteValueStorage::getNTWS(
		std::uint64_t address,
		std::size_t width,
		std::vector<std::uint64_t>& res) const
{
	using namespace std::placeholders;

	GetXByteFn getXByteFn = std::bind(
			&ByteValueStorage::getXByte,
			this, _1, _2, _3, _4
	);
	return getNTWSImpl(getXByteFn, address, width, res);
}

/**
 * Get nice NTWS (null-terminated wide string of ASCII characters) from the
 *
 * specified address
 * @param address Address to get string from
 * @param width Byte width of one character
 * @param res Result character array
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 *
 * @note This will read items until it reaches zero (null terminator) or
 *       non-ASCII character. Use this for fast wide string probing.
 *       See @c getNTWS() for a slower wide-string-forcing method.
 */
bool ByteValueStorage::getNTWSNice(
		std::uint64_t address,
		std::size_t width,
		std::vector<std::uint64_t>& res) const
{
	using namespace std::placeholders;

	GetXByteFn getXByteFn = std::bind(
			&ByteValueStorage::getXByte,
			this, _1, _2, _3, _4
	);
	return getNTWSNiceImpl(getXByteFn, address, width, res);
}

/**
 * Get integer (@a x bytes) array located at provided address using the
 * specified array size and endian (or default file endian)
 *
 * @param address Address to get integer array from
 * @param x Number of bytes for one array item
 * @param res Result integer array
 * @param size Integer array size (how many items are to be read)
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getXByteArray(
		std::uint64_t address,
		std::uint64_t x,
		std::vector<std::uint64_t>& res,
		std::size_t size,
		Endianness e) const
{
	std::uint64_t r = 0;

	// One of six address walks in this file -- the count was written as three
	// here and the adversarial verify pass found the other three, which is why
	// it is now a shared helper rather than a number in a comment. This is the
	// only one whose step comes straight from a caller parameter. It ended
	// with `address += x`, the same expression getNTWSImpl and getNTWSNiceImpl
	// below used to end with, and it has the same two failures. With x = 0 it re-reads one address `size` times and
	// reports success, so a caller asking for an array of 4 gets the same
	// element four times rather than a refusal. With a large x it wraps off the
	// top of the address space and carries on from the bottom: at
	// address = UINT64_MAX - 0xFF, x = 0x1000 and size = 4 it read
	// 0xffffffffffffff00, 0xf00, 0x1f00 and 0x2f00 and returned true.
	//
	// scan::advance refuses a zero step and refuses one that would leave the
	// buffer, and leaves the cursor bitwise unchanged on a refusal. The buffer
	// here is the address space itself -- getXByte is what knows where the
	// segments end -- so the cursor spans SIZE_MAX, exactly as in the two NTWS
	// walks. The last element needs no step after it, so advance is only asked
	// when another iteration follows; a walk that ends exactly at the top of
	// the address space is a legal walk.
	scan::Cursor cursor{static_cast<std::size_t>(address), SIZE_MAX};

	for (std::size_t i = 0; i < size; ++i)
	{
		if (!getXByte(cursor.pos, x, r, e))
		{
			return false;
		}
		res.push_back(r);
		if (i + 1 < size && !scan::advance(cursor, static_cast<std::size_t>(x)))
		{
			return false;
		}
	}

	return true;
}

/**
 * Get integer (1B) array located at provided address using the specified
 * array size and endian (or default file endian)
 *
 * @param address Address to get integer array from
 * @param res Result integer array
 * @param size Integer array size (how many items are to be read)
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get1ByteArray(
		std::uint64_t address,
		std::vector<std::uint64_t>& res,
		std::size_t size,
		Endianness e) const
{
	return getXByteArray(address, 1, res, size, e);
}

/**
 * Get integer (2B) array located at provided address using the specified
 * array size and endian (or default file endian)
 *
 * @param address Address to get integer array from
 * @param res Result integer array
 * @param size Integer array size (how many items are to be read)
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get2ByteArray(
		std::uint64_t address,
		std::vector<std::uint64_t>& res,
		std::size_t size,
		Endianness e) const
{
	return getXByteArray(address, 2, res, size, e);
}

/**
 * Get integer (4B) array located at provided address using the specified
 * array size and endian (or default file endian)
 *
 * @param address Address to get integer array from
 * @param res Result integer array
 * @param size Integer array size (how many items are to be read)
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get4ByteArray(
		std::uint64_t address,
		std::vector<std::uint64_t>& res,
		std::size_t size,
		Endianness e) const
{
	return getXByteArray(address, 4, res, size, e);
}

/**
 * Get integer (8B) array located at provided address using the specified
 * array size and endian (or default file endian)
 *
 * @param address Address to get integer array from
 * @param res Result integer array
 * @param size Integer array size (how many items are to be read)
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get8ByteArray(
		std::uint64_t address,
		std::vector<std::uint64_t>& res,
		std::size_t size,
		Endianness e) const
{
	return getXByteArray(address, 8, res, size, e);
}

/**
 * Get long double (10B) array located at provided address using the specified
 * array size
 *
 * @param address Address to get long double from
 * @param res Result long double array
 * @param size Array size (how many items are to be read)
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::get10ByteArray(
		std::uint64_t address,
		std::vector<long double>& res,
		std::size_t size) const
{
	long double r = 0;

	// The same unguarded walk as getXByteArray above, with a literal step. At
	// address = UINT64_MAX - 0x0F and size = 4 it read 0xfffffffffffffff0,
	// 0xfffffffffffffffa, 4 and 14 -- off the top of the address space and back
	// round from the bottom -- and returned true. Public API.
	//
	// kExtendedBytes rather than a bare 10: the width of the x87 extended
	// double is the reason for both the step and the name.
	scan::Cursor cursor{static_cast<std::size_t>(address), SIZE_MAX};

	for (std::size_t i = 0; i < size; ++i)
	{
		if (!get10Byte(cursor.pos, r))
		{
			return false;
		}
		res.push_back(r);
		if (i + 1 < size && !scan::advance(cursor, kExtendedBytes))
		{
			return false;
		}
	}

	return true;
}

/**
 * Get word array located at provided address using the specified size
 * and endian (or default file endian)
 *
 * @param address Address to get integer from
 * @param res Result integer
 * @param size Word array size (how many items are to be read)
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getWordArray(
		std::uint64_t address,
		std::vector<std::uint64_t>& res,
		std::size_t size,
		Endianness e) const
{
	return getXByteArray(address, getBytesPerWord(), res, size, e);
}

/**
 * Get float array located at provided address using the specified array size
 *
 * @param address Address to get float from
 * @param res Result float array
 * @param size Array size (how many items are to be read)
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getFloatArray(
		std::uint64_t address,
		std::vector<float>& res,
		std::size_t size) const
{
	float r = 0;

	// Same walk, same wrap: at address = UINT64_MAX - 0x07 and size = 4 it read
	// 0xfffffffffffffff8, 0xfffffffffffffffc, 0 and 4 and returned true.
	scan::Cursor cursor{static_cast<std::size_t>(address), SIZE_MAX};

	for (std::size_t i = 0; i < size; ++i)
	{
		if (!getFloat(cursor.pos, r))
		{
			return false;
		}
		res.push_back(r);
		if (i + 1 < size && !scan::advance(cursor, sizeof(float)))
		{
			return false;
		}
	}

	return true;
}

/**
 * Get double array located at provided address using the specified array size
 *
 * @param address Address to get double from
 * @param res Result double array
 * @param size Array size (how many items are to be read)
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool ByteValueStorage::getDoubleArray(
		std::uint64_t address,
		std::vector<double>& res,
		std::size_t size) const
{
	double r = 0;

	// Two defects, and the second is a plain wrong answer rather than a bound.
	//
	// The step was `address += sizeof(float)` -- four bytes -- in a loop that
	// reads DOUBLES. Asked for four doubles at 0x1000 it read 0x1000, 0x1004,
	// 0x1008 and 0x100c, so every element after the first overlapped its
	// predecessor by half and three of the four answers were assembled from the
	// wrong bytes. It returned true. Nothing in the tree tested it; the
	// adversarial verify pass measured it.
	//
	// And the walk is the unguarded one the rest of this file has now shed:
	// address += step with nothing stopping it wrapping off the top.
	scan::Cursor cursor{static_cast<std::size_t>(address), SIZE_MAX};

	for (std::size_t i = 0; i < size; ++i)
	{
		if (!getDouble(cursor.pos, r))
		{
			return false;
		}
		res.push_back(r);
		if (i + 1 < size && !scan::advance(cursor, sizeof(double)))
		{
			return false;
		}
	}

	return true;
}

/**
 * Create integer from vector of bytes
 *
 * @param data Vector of bytes
 * @param value Resulted value
 * @param endian Endian - if specified it is forced, otherwise file's endian
 *               is used
 * @param offset Offset of first byte from @a data which will be converted
 *    (0 means first offset from @a data)
 * @param size Number of bytes for conversion (0 means all bytes from @a offset
 *    to end of @a data)
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::createValueFromBytes(
		const std::vector<std::uint8_t>& data,
		std::uint64_t& value,
		Endianness endian,
		std::uint64_t offset,
		std::uint64_t size) const
{
	if (offset >= data.size())
	{
		return false;
	}

	// `offset + size > data.size()` formed the sum first, so at offset 1 and
	// size SIZE_MAX it wrapped to 0, the test was false, and `size` survived as
	// the real size. bounds::rangeFits asks the same question without forming
	// the sum, and bounds::remaining saturates instead of underflowing.
	const std::uint64_t realSize =
			(!size || !bounds::rangeFits(offset, data.size(), size))
					? bounds::remaining(offset, data.size())
					: size;
	if (size && realSize != size)
	{
		return false;
	}

	if (endian == Endianness::UNKNOWN && isLittleEndian())
	{
		endian = Endianness::LITTLE;
	}
	else if (endian == Endianness::UNKNOWN && isBigEndian())
	{
		endian = Endianness::BIG;
	}
	else if (endian == Endianness::UNKNOWN)
	{
		return false;
	}

	// The loop this replaces was
	//
	//     value += data[offset + i] << (getByteLength()
	//                 * (endian == LITTLE ? i : realSize - i - 1));
	//
	// on a std::uint64_t accumulator, and nothing bounded realSize at 8 --
	// a caller passing size 0 means "all bytes from offset", so realSize is
	// data.size() - offset and grows with the file. ESBMC's witness is
	// dataSize = 10, offset = 0, size = 0, byteLength = 8: at i = 8 the
	// expression is `(uint64)data[8] << 64`, reported as "arithmetic overflow
	// on shl, !overflow("shl", (unsigned long int)(data[offset + i]),
	// byteLength * i)". The witness sets data[8] = data[9] = 1, so the bits
	// that fall off the end are real bits and not a discarded zero.
	//
	// The `+=` was a second defect: with a byteLength below 8 the units
	// overlap, and an addition carries into the next place where an OR of the
	// masked payload does not.
	//
	// byteorder::readWidened refuses n * bitsPerUnit > 64 through widthFits,
	// which never forms the product, masks each unit to bitsPerUnit bits
	// before placing it, accumulates with |=, and writes `value` only on
	// success. It is proved over the whole 64-bit domain in
	// tests/verification/byte_order_proof.cpp.
	return byteorder::readWidened(
			data.data(),
			data.size(),
			static_cast<std::size_t>(offset),
			static_cast<std::size_t>(realSize),
			static_cast<unsigned>(getByteLength()),
			endian == Endianness::BIG,
			value);
}

/**
 * Create vector of bytes from integer
 *
 * @param data Integer
 * @param x Width of integer
 * @param value Resulted vector of bytes
 * @param endian Endian - if specified it is forced, otherwise file's
 *               endian is used
 *
 * @return @c true if conversion went OK, @c false otherwise
 */
bool ByteValueStorage::createBytesFromValue(
		std::uint64_t data,
		std::uint64_t x,
		std::vector<std::uint8_t>& value,
		Endianness endian) const
{
	if (endian == Endianness::UNKNOWN && isLittleEndian())
	{
		endian = Endianness::LITTLE;
	}
	else if (endian == Endianness::UNKNOWN && isBigEndian())
	{
		endian = Endianness::BIG;
	}
	else if (endian == Endianness::UNKNOWN)
	{
		return false;
	}

	value.clear();

	// A zero width emits nothing and always did: the old loop simply never
	// executed. Kept as it was, so a caller asking for zero bytes still sees
	// success rather than a new failure.
	if (x == 0)
	{
		return true;
	}

	// `for (std::uint8_t i = 0; i < x; ++i)` counted a std::uint64_t width the
	// caller supplies. The counter cannot represent any index at or above 256:
	// at x = 256 it wraps 255 -> 0 and the loop never terminates, and for any
	// larger x the same. Asked whether the last index the loop must reach,
	// x - 1, is representable in the counter's type, ESBMC answers
	// x = 0x8000000000000008 (9223372036854775816), for which
	// x - 1 = 9223372036854775815 > 255 -- "assertion x - 1 <= 255u" violated.
	// Everything from index 256 up was left as resize() wrote it.
	//
	// Every x above 8 is undefined for a second reason as well:
	// `data >> (getByteLength() * i)` reaches a shift count of 64 at i = 8 on
	// a 64-bit operand.
	//
	// The width has to be refused BEFORE resize(), not inside the writer:
	// resize() is what turns a bogus x into an allocation, and
	// value.resize(0x8000000000000008) throws length_error long before any
	// counter is involved. byteorder::kMaxBytes is the kernel's own bound --
	// the accumulator width in bytes -- not a number chosen here.
	if (x > byteorder::kMaxBytes)
	{
		return false;
	}

	// byteorder::writeLE/writeBE place one byte per 8 bits, which is what
	// every getByteLength() in this tree reports: FileFormat::getByteLength
	// returns a literal 8 and RawDataFormat's bytesLength field is 8 with no
	// caller of setBytesLength anywhere in src/ or include/. Refuse rather
	// than silently emit a different layout if that ever stops being true --
	// the old loop shifted by getByteLength() and then masked with 0xFF, which
	// drops bits for any width above 8 and overlaps units for any below it.
	if (getByteLength() != byteorder::kBitsPerByte)
	{
		return false;
	}

	value.resize(static_cast<std::size_t>(x));

	// byteorder::writeLE/writeBE count with a std::size_t against a
	// std::size_t bound, refuse n outside 1..8 and n > outCap, and write
	// nothing at all on a refusal rather than a prefix.
	const bool ok = endian == Endianness::LITTLE
			? byteorder::writeLE(data, value.size(), value.data(), value.size())
			: byteorder::writeBE(data, value.size(), value.data(), value.size());
	if (!ok)
	{
		value.clear();
	}
	return ok;
}

bool ByteValueStorage::get10ByteImpl(
		const std::vector<std::uint8_t>& data,
		long double& res) const
{
	// getFloatImpl and getDoubleImpl immediately below both refuse a data
	// vector that is not exactly the width they decode; this one copied
	// data.size() bytes into a long double and called double10ToDouble8 with
	// no check at all. getXBytes is virtual, so what get10Byte gets back is
	// whatever the format chose to return -- the two in-tree implementations
	// happen to enforce res.size() == x, but nothing in the interface says
	// they must. With a format that returns success and four bytes,
	// double10ToDouble8 read five bytes past the end of them (ASan:
	// "heap-buffer-overflow ... READ of size 1") and this returned true with
	// most of `res` never written.
	if (data.size() != kExtendedBytes)
	{
		return false;
	}

	if (systemHasLongDouble())
	{
		// systemHasLongDouble() is `sizeof(long double) >= 10`, so the ten
		// bytes checked above fit in the destination.
		memcpy(&res, data.data(), kExtendedBytes);
		return true;
	}

	std::vector<std::uint8_t> d8;
	double10ToDouble8(d8, data);
	// double10ToDouble8 leaves d8 empty when it refuses; the check above means
	// it cannot refuse here, and this says so rather than trusting it.
	if (d8.size() != sizeof(double))
	{
		return false;
	}

	// Without a 10-byte long double the decoded value is a double, and this is
	// the widening the original memcpy did by accident of size: copying eight
	// bytes into a narrower long double would have overrun it.
	static_assert(sizeof(long double) >= sizeof(double),
			"long double is never narrower than double");
	double d = 0.0;
	memcpy(&d, d8.data(), sizeof(double));
	res = d;
	return true;
}

bool ByteValueStorage::getFloatImpl(
		const std::vector<std::uint8_t>& data,
		float& res) const
{
	if (data.size() != sizeof(float))
	{
		return false;
	}

	memcpy(&res, data.data(), data.size());
	return true;
}

bool ByteValueStorage::getDoubleImpl(
		const std::vector<std::uint8_t>& data,
		double& res) const
{
	if (data.size() != sizeof(double))
	{
		return false;
	}

	memcpy(&res, data.data(), data.size());
	return true;
}

bool ByteValueStorage::getNTBSImpl(
		const GetNByteFn& get1ByteFn,
		std::uint64_t address,
		std::string& res, std::size_t size) const
{
	// The third address walk in this file, and it stepped with `++address`.
	// The step is a literal 1 so it always advances -- this is not the zero-step
	// spin getNTWSImpl had -- but it wraps: at address = UINT64_MAX the next
	// read is at 0, and with size = 0 a format that keeps answering non-zero
	// bytes sends the walk round the bottom of the address space and on. The
	// same cursor the other two walks use makes the top of the address space a
	// stop rather than a seam, so the walk terminates for every format: each
	// accepted step consumes at least one of the finitely many bytes left,
	// which is the bound scan::maxSteps states.
	scan::Cursor cursor{static_cast<std::size_t>(address), SIZE_MAX};

	std::uint64_t c = 0;
	auto suc = get1ByteFn(cursor.pos, c, getEndianness());
	res.clear();

	while (suc && (c || size))
	{
		res += c;
		if (size && res.length() == size)
		{
			break;
		}
		if (!scan::advance(cursor, 1))
		{
			break;
		}
		suc = get1ByteFn(cursor.pos, c, getEndianness());
	}

	return !res.empty();
}

bool ByteValueStorage::getNTWSImpl(
		const GetXByteFn& getXByteFn,
		std::uint64_t address,
		std::size_t width,
		std::vector<std::uint64_t>& res) const
{
	std::vector<std::uint64_t> tmp;
	std::uint64_t item = 0;
	res.clear();

	// The walk's buffer is the address space itself: getXByteFn is what knows
	// where the segments end, and it refuses anything outside them. What this
	// loop owns is that it ADVANCES and that it does not wrap off the top.
	//
	// `address += width` with nothing rejecting width == 0 does neither: a
	// caller asking for a zero-width wide string re-reads the same non-zero
	// element forever and pushes it into `tmp` on every iteration, so it is an
	// unbounded vector and not merely a spin. ESBMC refutes
	// `address + width > address` for the expression as written -- the
	// reachable case is simply width = 0, where address = 0x1000 gives
	// next = 0x1000, unchanged; the wrapping case it also finds is
	// width = 0x8000020C00200910 at address = 0x8000000405200103.
	//
	// scan::advance refuses a zero step outright and refuses a step that would
	// leave the buffer, and it leaves the cursor bitwise unchanged on a
	// refusal. tests/verification/scan_cursor_proof.cpp proves the walk
	// terminates.
	scan::Cursor cursor{static_cast<std::size_t>(address), SIZE_MAX};

	bool ret = false;
	while (getXByteFn(cursor.pos, width, item, getEndianness()))
	{
		tmp.push_back(item);
		if (!item)
		{
			ret = true;
			break;
		}
		else if (!scan::advance(cursor, width))
		{
			break;
		}
	}

	if (!tmp.empty())
	{
		res = tmp;
	}

	return ret;
}

bool ByteValueStorage::getNTWSNiceImpl(
		const GetXByteFn& getXByteFn,
		std::uint64_t address,
		std::size_t width,
		std::vector<std::uint64_t>& res) const
{
	std::vector<std::uint64_t> tmp;
	std::uint64_t item = 0;
	res.clear();

	// The same unbounded walk as getNTWSImpl above, with the same zero width
	// and the same wrap, so it gets the same cursor. Leaving one of two
	// identical loops guarded is worse than guarding neither, because the next
	// reader assumes the unguarded one was examined and found safe.
	scan::Cursor cursor{static_cast<std::size_t>(address), SIZE_MAX};

	while (getXByteFn(cursor.pos, width, item, getEndianness()))
	{
		tmp.push_back(item);
		if (!item)
		{
			break;
		}
		else if (!isNiceAsciiWideCharacter(item))
		{
			return false;
		}
		else if (!scan::advance(cursor, width))
		{
			break;
		}
	}

	if (!tmp.empty())
	{
		res = tmp;
	}

	// one char (trailing '0') is not enough
	return tmp.size() > 1;
}

} // namespace utils
} // namespace retdec
