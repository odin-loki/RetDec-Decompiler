/**
 * @file src/utils/dynamic_buffer.cpp
 * @brief Implementation of class for buffered data mainpulation.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/utils/dynamic_buffer.h"

#include "retdec/utils/bounds.h"

using namespace retdec::utils;

namespace retdec {
namespace utils {

/**
 * Creates the empty DynamicBuffer object with no capacity and
 * specified endianness.
 *
 * @param endianness Endianness of the bytes in the buffer.
 */
DynamicBuffer::DynamicBuffer(Endianness endianness): _data(), _endianness(endianness), _capacity(0) {}

/**
 * Creates the DynamicBuffer object with specified capacity and endianness.
 *
 * @param capacity Capacity of the buffer.
 * @param endianness Endianness of the bytes in the buffer.
 */
DynamicBuffer::DynamicBuffer(uint32_t capacity, Endianness endianness):
	_data(), _endianness(endianness), _capacity(capacity)
{
	_data.reserve(capacity);
}

/**
 * Creates the DynamicBuffer object and fills it with specified data with
 * specified endianness.
 *
 * @param data The bytes to initialize the buffer with.
 * @param endianness Endiannes of the bytes in the buffer.
 */
DynamicBuffer::DynamicBuffer(const std::vector<uint8_t>& data, Endianness endianness):
	_data(data), _endianness(endianness), _capacity(static_cast<uint32_t>(data.size()))
{}

/**
 * Creates the copy of the DynamicBuffer object.
 *
 * @param dynamicBuffer Buffer to copy.
 */
DynamicBuffer::DynamicBuffer(const DynamicBuffer& dynamicBuffer):
	_data(dynamicBuffer._data), _endianness(dynamicBuffer._endianness), _capacity(dynamicBuffer._capacity)
{}

/**
 * Creates the copy of the DynamicBuffer object, but only the
 * specified subbuffer.
 *
 * @param dynamicBuffer Buffer to copy.
 * @param startPos Starting position in the specified buffer where to
 *        start the copying.
 * @param amount Number of bytes from startPos to copy.
 */
DynamicBuffer::DynamicBuffer(const DynamicBuffer& dynamicBuffer, uint32_t startPos, uint32_t amount)
{
	// This had no bounds check at all: both iterators were formed from
	// startPos and amount straight away, so a startPos past the end of the
	// source is undefined before the vector is even constructed. Every caller
	// is unpacker code taking both from the packed file -- pe_upx_stub.cpp
	// reaches here four times with offsets it read out of the input.
	const std::vector<uint8_t> tmpBuffer = dynamicBuffer.getBuffer();
	const std::size_t start = bounds::clamp(static_cast<std::size_t>(startPos), tmpBuffer.size());
	const std::size_t take =
		bounds::clamp(static_cast<std::size_t>(amount), bounds::remaining(start, tmpBuffer.size()));
	std::vector<uint8_t> buffer(tmpBuffer.begin() + start, tmpBuffer.begin() + start + take);

	_data = buffer;
	_endianness = dynamicBuffer._endianness;
	_capacity = static_cast<uint32_t>(buffer.size());
}

/**
 * Assign operator, creates the copy of the DynamicBuffer.
 *
 * @param rhs Right hand side of the operator.
 *
 * @return The new DynamicBuffer object.
 */
DynamicBuffer& DynamicBuffer::operator=(DynamicBuffer rhs)
{
	std::swap(_data, rhs._data);
	std::swap(_endianness, rhs._endianness);
	std::swap(_capacity, rhs._capacity);
	return *this;
}

/**
 * Sets the capacity of the buffer.
 *
 * @param capacity The new capacity to set to the buffer.
 */
void DynamicBuffer::setCapacity(uint32_t capacity)
{
	_capacity = capacity;
	_data.reserve(_capacity);
}

/**
 * Gets the actual capacity of the buffer.
 *
 * @return The capacity of the buffer.
 */
uint32_t DynamicBuffer::getCapacity() const
{
	return _capacity;
}

/**
 * Sets the endianness of the bytes in the buffer. It doesn't result in any
 * changes to the actual bytes in the buffer. It reflects only when reading
 * from or writing to the buffer.
 *
 * @param endianness The endianness to set.
 */
void DynamicBuffer::setEndianness(Endianness endianness)
{
	_endianness = endianness;
}

/**
 * Gets the current endianness of the buffer.
 *
 * @return The endianness of the bytes in the buffer.
 */
Endianness DynamicBuffer::getEndianness() const
{
	return _endianness;
}

/**
 * Gets the size of the data that are actually written to the buffer.
 * This cannot be greater than the capacity of the buffer.
 *
 * @return The size of the written data to the buffer.
 */
uint32_t DynamicBuffer::getRealDataSize() const
{
	return static_cast<uint32_t>(_data.size());
}

/**
 * Erases the bytes from the buffer. Also reduces the capacity of the buffer.
 *
 * @param startPos The starting position where to start erasing.
 * @param amount Number of bytes from the startPos including to erase.
 */
void DynamicBuffer::erase(uint32_t startPos, uint32_t amount)
{
	if (startPos >= _data.size()) return;

	// `startPos + amount > _data.size()` is a uint32 sum: at startPos 100 and
	// amount 0xFFFFFFFF it wraps to 99, the test is false, amount survives
	// unclamped and the second iterator is formed 4 GB past the end.
	amount = static_cast<uint32_t>(
		bounds::clamp(static_cast<std::size_t>(amount), bounds::remaining(startPos, _data.size())));
	_data.erase(_data.begin() + startPos, _data.begin() + startPos + static_cast<std::size_t>(amount));
}

/**
 * Gets the buffer as the vector of bytes.
 *
 * @return The vector with the bytes.
 */
std::vector<uint8_t> DynamicBuffer::getBuffer() const
{
	return _data;
}

/**
 * Gets the raw pointer to the bytes in the buffer.
 *
 * @return The pointer to the bytes in the buffer.
 */
const uint8_t* DynamicBuffer::getRawBuffer() const
{
	return _data.data();
}

/**
 * Runs the specified function for every single byte in the DynamicBuffer.
 *
 * @param func Function to run for every byte.
 */
void DynamicBuffer::forEach(const std::function<void(uint8_t&)>& func)
{
	for (uint8_t& byte: _data)
		func(byte);
}

/**
 * Runs the specified function for every single byte in the DynamicBuffer
 * in the reverse order.
 *
 * @param func Function to run for every byte.
 */
void DynamicBuffer::forEachReverse(const std::function<void(uint8_t&)>& func)
{
	for (std::vector<uint8_t>::reverse_iterator itr = _data.rbegin(); itr != _data.rend(); ++itr)
	{
		uint8_t& byte = *itr;
		func(byte);
	}
}

/**
 * Reads the null or length terminated string from the buffer.
 *
 * @param pos The poisition in the buffer where to start reading.
 * @param maxLength The maximal length of the string that is read. If this is 0,
 *        the length limit is ignored and the string is read up to the next
 *        0 byte.
 *
 * @return String read from buffer.
 */
std::string DynamicBuffer::readString(uint32_t pos, uint32_t maxLength) const
{
	std::string str;
	char ch;

	while (((ch = read<char>(pos++)) != 0) && (!maxLength || str.length() < maxLength))
		str += ch;

	return str;
}

/**
 * Writes the single byte into the buffer for repeating amount of times.
 *
 * @param byte The byte to write into the buffer.
 * @param pos The position where to start writing the byte.
 * @param repeatAmount The number of times the byte is written into the
 *        buffer starting from pos including.
 */
void DynamicBuffer::writeRepeatingByte(uint8_t byte, uint32_t pos, uint32_t repeatAmount)
{
	// `pos + repeatAmount > _capacity` then `repeatAmount = _capacity - pos`
	// is wrong twice over once pos is past the capacity: the sum is uint32 and
	// wraps, and the subtraction underflows. At capacity 100, pos 200 and
	// repeatAmount 1 the clamp produced 4294967196; the resize test then formed
	// 200 + 4294967196, wrapped back to 100, found it not greater than the
	// hundred bytes already there and did nothing; and the memset wrote about
	// four gigabytes starting a hundred bytes past a hundred-byte vector.
	// &_data[pos] was already undefined before it ran.
	//
	// remaining() saturates at zero instead of underflowing and clamp() never
	// forms the sum, which is the whole reason they are in a proved header.
	repeatAmount =
		static_cast<uint32_t>(bounds::clamp(static_cast<std::size_t>(repeatAmount), bounds::remaining(pos, _capacity)));
	if (repeatAmount == 0) return;

	const std::size_t end = static_cast<std::size_t>(pos) + repeatAmount;
	if (end > _data.size()) _data.resize(end);

	memset(&_data[pos], byte, repeatAmount);
}

} // namespace utils
} // namespace retdec
