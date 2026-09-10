/**
 * @file src/loader/loader/segment_data_source.cpp
 * @brief Definition of segment data source class.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <algorithm>
#include <iterator>

#include "retdec/loader/loader/segment_data_source.h"

namespace retdec {
namespace loader {

SegmentDataSource::SegmentDataSource() : _data(nullptr, 0)
{
}

SegmentDataSource::SegmentDataSource(const llvm::StringRef& data)
	: _data(data.data(), data.size())
{
}

SegmentDataSource::SegmentDataSource(const SegmentDataSource& dataSource)
	: _data(dataSource._data.data(), dataSource._data.size())
{
}

bool SegmentDataSource::isDataSet() const
{
	return !_data.empty();
}

const std::uint8_t* SegmentDataSource::getData() const
{
	return _data.bytes_begin();
}

std::uint64_t SegmentDataSource::getDataSize() const
{
	return _data.size();
}

void SegmentDataSource::resize(std::uint64_t newSize)
{
	_data = llvm::StringRef(_data.data(), std::min(getDataSize(), newSize));
}

bool SegmentDataSource::shrink(std::uint64_t newOffset, std::uint64_t newSize)
{
	if (newSize > getDataSize())
		return false;

	if (newOffset >= getDataSize())
	{
		_data = llvm::StringRef(nullptr, 0);
	}
	else if (newOffset + newSize > getDataSize())
	{
		_data = llvm::StringRef(_data.data() + newOffset, getDataSize() - newOffset);
	}
	else
	{
		_data = llvm::StringRef(_data.data() + newOffset, std::min(getDataSize(), newSize));
	}

	return true;
}

bool SegmentDataSource::loadData(std::uint64_t loadOffset, std::uint64_t loadSize, std::vector<std::uint8_t>& data) const
{
	data.clear();

	if (!isDataSet())
		return false;

	if (loadOffset >= getDataSize())
		return false;

	// `loadOffset + loadSize >= getDataSize()` was the clamp, and it is a
	// wrapping sum of two values the caller supplies: at loadOffset 10 with
	// loadSize 0xFFFFFFFFFFFFFFFB it computes 5, decides 5 is inside the
	// buffer, and leaves loadSize at 18446744073709551611 -- which the
	// std::copy below then reads, out of a buffer that may hold a few hundred
	// bytes.
	//
	// `loadOffset < getDataSize()` is established two lines up, so the space
	// that remains cannot itself wrap. Comparing against that asks the same
	// question without the sum, and gives the same answer for every request
	// that did not wrap: where the old test clamped on equality, so does this.
	const std::uint64_t available = getDataSize() - loadOffset;

	loadSize = loadSize >= available ? available : loadSize;
	std::copy(_data.data() + loadOffset, _data.data() + loadOffset + loadSize, std::back_inserter(data));
	return true;
}

bool SegmentDataSource::saveData(std::uint64_t saveOffset, std::uint64_t saveSize, const std::vector<std::uint8_t>& data)
{
	if (!isDataSet())
		return false;

	if (saveOffset >= getDataSize())
		return false;

	// The same wrapping sum, with the same fix. `saveOffset < getDataSize()`
	// is established two lines up.
	const std::uint64_t available = getDataSize() - saveOffset;

	saveSize = saveSize > available ? available : saveSize;

	// And a second bound the clamp above never had: saveSize is checked
	// against the destination and nothing checks it against the source, so
	// `saveData(0, 100, aVectorOfTen)` read ninety bytes past the end of the
	// caller's vector. Every caller in this tree passes a vector at least as
	// long as the size it asks for, which is why this has never been seen;
	// none of them states it, and the read is out of bounds when one stops.
	if (saveSize > data.size())
	{
		saveSize = data.size();
	}

	std::copy(data.data(), data.data() + saveSize, const_cast<char*>(_data.data()) + saveOffset);
	return true;
}

} // namespace loader
} // namespace retdec
