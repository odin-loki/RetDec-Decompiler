/**
 * @file src/loader/loader/image.cpp
 * @brief Implementation of loadable image class.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

// ─── the bounds arithmetic Image's byte readers share ───────────────────────
//
// Same two wrapping shapes as src/fileformat/file_format/file_format.cpp, in
// the same reader family, over a sink that repeats the defect twice more:
// Segment::getBytes clamps with `addressOffset + size >= getSize()` *after*
// SegmentDataSource::loadData has already been called with the unclamped size,
// and loadData clamps with `loadOffset + loadSize >= getDataSize()` immediately
// before `std::copy(_data.data() + loadOffset, _data.data() + loadOffset +
// loadSize, ...)`. Both of those sums wrap, so the width has to be refused here,
// before either is reached.
//
// The arithmetic lives up here, outside the
// RETDEC_LOADER_BOUNDS_KERNELS_ONLY guard, because the rest of this translation
// unit cannot be compiled without an LLVM source tree -- it reaches
// retdec/fileformat/types/sec_seg/sec_seg.h, which includes
// <llvm/ADT/StringRef.h>, and deps/llvm here is a download stub:
//
//     $ g++ -std=c++17 -Iinclude -fsyntax-only src/loader/loader/image.cpp
//     include/retdec/fileformat/types/sec_seg/sec_seg.h:14:10: fatal error:
//     llvm/ADT/StringRef.h: No such file or directory
//
// so a guard written inside an Image method is a guard no test in this
// repository can execute. tests/loader_sim/xbyte_width_guard_test.cpp defines
// RETDEC_LOADER_BOUNDS_KERNELS_ONLY, includes this file, and calls the two
// functions below on the counterexamples they exist to refuse. Only <cstdint>
// and the two dependency-free headers below are reachable from here.

#include <cstddef>
#include <cstdint>

#include "retdec/utils/bounds.h"
#include "retdec/utils/byte_order.h"

namespace retdec {
namespace loader {
namespace bounds_kernels {

/**
 * Decide whether @a x units of @a unitBits bits each fit in a 64-bit result.
 *
 * The guard this replaces -- at Image::getXByte and Image::setXByte, and in the
 * same two shapes at src/fileformat/file_format/file_format.cpp -- was spelled
 *
 *     x * getByteLength() > sizeof(res) * CHAR_BIT
 *
 * which forms the product before comparing it. x is a std::uint64_t the caller
 * supplies, so the product is not bounded by anything. ESBMC's witness is
 * x = 2305843009213693954 (0x2000000000000002) with getByteLength() == 8: the
 * true product 0x10000000000000010 wraps to 16, `16 > 64` is false, and the
 * guard admits a width of 2.3e18 units. It is reported as "arithmetic overflow
 * on mul, !overflow(\"*\", x, byteLength)" (CWE-190/191).
 *
 * byteorder::widthFits is the same test with the product formed only once both
 * factors are known to be at most 64, proved equivalent over the whole 64-bit
 * domain in tests/verification/byte_order_proof.cpp.
 *
 * The two narrowings on the way in are refused rather than cast away. The unit
 * width reaches widthFits as an unsigned, so on a host with 64-bit std::size_t
 * a width of 0x100000008 would truncate to 8 and be accepted; x reaches it as a
 * std::size_t, which is narrower on a 32-bit host, so x = 0x100000002 would
 * truncate to 2. Neither can fit a 64-bit accumulator at any unit width, so
 * both lose here.
 *
 * Zero units is not this function's case: widthFits refuses n == 0 by design,
 * while the getXByte family answered x == 0 through the byte fetch below it.
 * The call sites keep that behaviour by testing x separately.
 */
inline bool xWidthFitsAccumulator(std::uint64_t x, std::uint64_t unitBits)
{
	if (x > retdec::utils::byteorder::kAccumulatorBits || unitBits > retdec::utils::byteorder::kAccumulatorBits)
	{
		return false;
	}

	return retdec::utils::byteorder::widthFits(static_cast<std::size_t>(x), static_cast<unsigned>(unitBits));
}

/**
 * bounds::rangeFits for a 64-bit offset, region size and length.
 *
 * Image::getXBytes handed a caller-supplied x straight to Segment::getBytes,
 * whose two clamps are the wrapping sums quoted above. At an address offset of
 * 10 with x = 0xFFFFFFFFFFFFFFFB the sum is 5, so for any segment longer than
 * five bytes neither clamp fires and SegmentDataSource::loadData copies
 * 0xFFFFFFFFFFFFFFFB bytes out of the segment's data. bounds::rangeFits compares against the bytes that remain and
 * never forms the sum; it is proved in tests/verification/bounds_proof.cpp.
 *
 * @a offset and @a len above SIZE_MAX are refused rather than cast, because on
 * a host where std::size_t is narrower the cast is the bug it is meant to
 * prevent. A @a size above SIZE_MAX is capped instead, since no buffer that
 * large can exist in this process.
 */
inline bool rangeFitsWide(std::uint64_t offset, std::uint64_t size, std::uint64_t len)
{
	if (offset > static_cast<std::uint64_t>(SIZE_MAX) || len > static_cast<std::uint64_t>(SIZE_MAX))
	{
		return false;
	}

	const std::size_t cappedSize =
		size > static_cast<std::uint64_t>(SIZE_MAX) ? SIZE_MAX : static_cast<std::size_t>(size);

	return retdec::utils::bounds::rangeFits(
		static_cast<std::size_t>(offset), cappedSize, static_cast<std::size_t>(len));
}

} // namespace bounds_kernels
} // namespace loader
} // namespace retdec

#ifndef RETDEC_LOADER_BOUNDS_KERNELS_ONLY

#include <memory>
#include <climits>
#include <cstring>

#include "retdec/utils/conversion.h"
#include "retdec/utils/string.h"
#include "retdec/utils/system.h"
#include "retdec/loader/loader/image.h"

using namespace retdec::utils;

namespace retdec {
namespace loader {

Image::Image(const std::shared_ptr<retdec::fileformat::FileFormat>& fileFormat):
	_fileFormat(fileFormat), _segments(), _baseAddress(0), _namelessSegNameGen("seg", '0', 4), _statusMessage()
{}

Endianness Image::getEndianness() const
{
	return getFileFormat()->getEndianness();
}

std::size_t Image::getNibbleLength() const
{
	return getFileFormat()->getNibbleLength();
}

std::size_t Image::getByteLength() const
{
	return getFileFormat()->getByteLength();
}

std::size_t Image::getWordLength() const
{
	return getFileFormat()->getWordLength();
}

std::size_t Image::getBytesPerWord() const
{
	return getFileFormat()->getBytesPerWord();
}

std::size_t Image::getNumberOfNibblesInByte() const
{
	return getFileFormat()->getNumberOfNibblesInByte();
}

bool Image::hasMixedEndianForDouble() const
{
	return getFileFormat()->hasMixedEndianForDouble();
}

/**
 * Returns the retdec::fileformat::FileFormat object associated with the loaded image,
 * which contains static information about the file.
 *
 * @return File format object.
 */
retdec::fileformat::FileFormat* Image::getFileFormat()
{
	return _fileFormat.get();
}

/**
 * Returns the retdec::fileformat::FileFormat object associated with the loaded image,
 * which contains static information about the file.
 *
 * @return File format object.
 */
const retdec::fileformat::FileFormat* Image::getFileFormat() const
{
	return _fileFormat.get();
}

/**
 * Returns the retdec::fileformat::FileFormat object associated with the loaded image as weak pointer,
 * which containers static information about the file.
 *
 * @return Weak pointer to file format object.
 */
std::weak_ptr<retdec::fileformat::FileFormat> Image::getFileFormatWptr() const
{
	return std::weak_ptr<retdec::fileformat::FileFormat>(_fileFormat);
}

/**
 * Returns the number of the segments in the address space.
 *
 * @return The number of segments.
 */
std::size_t Image::getNumberOfSegments() const
{
	return _segments.size();
}

/**
 * Returns the all loaded segments in the image.
 *
 * @return Vector of loaded segments.
 */
const std::vector<std::unique_ptr<Segment>>& Image::getSegments() const
{
	return _segments;
}

/**
 * Returns a base address where the address space is loaded.
 *
 * @return Base address.
 */
std::uint64_t Image::getBaseAddress() const
{
	return _baseAddress;
}

/**
 * Sets a base address for the address space.
 *
 * @param baseAddress Address to set.
 */
void Image::setBaseAddress(std::uint64_t baseAddress)
{
	_baseAddress = baseAddress;
}

/**
 * Checks whether there are data on the provided address -- address must belong to some segment.
 *
 * @param address The address to check.
 *
 * @return True if data are present on the provided address, otherwise false.
 */
bool Image::hasDataOnAddress(std::uint64_t address) const
{
	auto seg = getSegmentFromAddress(address);
	return seg && seg->getSecSeg() && !seg->getSecSeg()->isDebug();
}

/**
 * Checks whether there are data on the provided address -- address must belong to some segment and it cannot be BSS
 * segment.
 *
 * @param address The address to check.
 *
 * @return True if data are present on the provided address, otherwise false.
 */
bool Image::hasDataInitializedOnAddress(std::uint64_t address) const
{
	auto seg = getSegmentFromAddress(address);
	return seg && seg->getSecSeg() && !seg->getSecSeg()->isBss() && !seg->getSecSeg()->isDebug();
}

/**
 * @brief Test if there are some read-only data on provided address -- address belongs
 * to some read-only section or segment
 *
 * @param address Address to test
 *
 * @return @c True if there are read-only data for address, @c false otherwise
 *
 * @note This will return false if address is in BSS or debug section.
 */
bool Image::hasReadOnlyDataOnAddress(std::uint64_t address) const
{
	auto* s = getSegmentFromAddress(address);
	return s && s->getSecSeg() && !s->getSecSeg()->isBss() && !s->getSecSeg()->isDebug()
		&& s->getSecSeg()->isReadOnly();
}

/**
 * Checks whether there is segment on the provided address -- whether address falls into some of the segments.
 *
 * @param address The address to check.
 *
 * @return True if segment is present, otherwise false.
 */
bool Image::hasSegmentOnAddress(std::uint64_t address) const
{
	return getSegmentFromAddress(address) != nullptr;
}

/**
 * Returns the segment at the given index, if any exists.
 *
 * @param index Index of the segment.
 *
 * @return Segment at given index, otherwise nullptr.
 */
Segment* Image::getSegment(std::size_t index)
{
	return const_cast<Segment*>(_getSegment(index));
}

/**
 * Returns the segment at the given index, if any exists.
 *
 * @param index Index of the segment.
 *
 * @return Segment at given index, otherwise nullptr.
 */
const Segment* Image::getSegment(std::size_t index) const
{
	return _getSegment(index);
}

/**
 * Returns the segment with the provided name, if any exists.
 *
 * @param name Name of the segment.
 *
 * @return Segment with provided name, otherwise nullptr.
 */
Segment* Image::getSegment(const std::string& name)
{
	return const_cast<Segment*>(_getSegment(name));
}

/**
 * Returns the segment with the provided name, if any exists.
 *
 * @param name Name of the segment.
 *
 * @return Segment with provided name, otherwise nullptr.
 */
const Segment* Image::getSegment(const std::string& name) const
{
	return _getSegment(name);
}

/**
 * Returns the segment created from section/segment in section/program headers at specified index, if any exists.
 *
 * @param index Index of the section/segment in section/program headers.
 *
 * @return Segment at the specified index, otherwise nullptr.
 */
Segment* Image::getSegmentWithIndex(std::size_t index)
{
	return const_cast<Segment*>(_getSegmentWithIndex(index));
}

/**
 * Returns the segment created from section/segment in section/program headers at specified index, if any exists.
 *
 * @param index Index of the section/segment in section/program headers.
 *
 * @return Segment at the specified index, otherwise nullptr.
 */
const Segment* Image::getSegmentWithIndex(std::size_t index) const
{
	return _getSegmentWithIndex(index);
}

/**
 * Returns the segment into which provided address falls, if any exists.
 *
 * @param address The address to check.
 *
 * @return Segment, otherwise nullptr.
 */
Segment* Image::getSegmentFromAddress(std::uint64_t address)
{
	return const_cast<Segment*>(_getSegmentFromAddress(address));
}

/**
 * Returns the segment into which provided address falls, if any exists.
 *
 * @param address The address to check.
 *
 * @return Segment, otherwise nullptr.
 */
const Segment* Image::getSegmentFromAddress(std::uint64_t address) const
{
	return _getSegmentFromAddress(address);
}

/**
 * Returns the segment into which entry points address falls, if any exists.
 *
 * @return Entry point segment, otherwise nullptr.
 */
const Segment* Image::getEpSegment()
{
	std::uint64_t epAddress;
	if (!getFileFormat()->getEpAddress(epAddress)) return nullptr;

	return getSegmentFromAddress(epAddress);
}

/**
 * Returns raw segment data together with its size. Caller should never access beyond
 * `pointer + size` (including). Size is calculated from the physical size of the segment.
 * Returns pair of null pointer and 0 in case of an error.
 *
 * @param address Address to start from.
 *
 * @return Raw data pointer and size.
 */
std::pair<const std::uint8_t*, std::uint64_t> Image::getRawSegmentData(std::uint64_t address) const
{
	auto segment = getSegmentFromAddress(address);
	if (!segment) return {nullptr, 0};

	auto offset = address - segment->getAddress();
	auto rawData = segment->getRawData();
	if (!rawData.first || offset > rawData.second) return {nullptr, 0};

	return {rawData.first + offset, rawData.second - offset};
}

/**
 * Get integer (@a x bytes) located at provided address using the specified endian or default file endian
 *
 * @param address Address to get integer from
 * @param x Number of bytes for conversion
 * @param res Result integer
 * @param e Endian - if specified it is forced, otherwise file's endian is used
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool Image::getXByte(std::uint64_t address, std::uint64_t x, std::uint64_t& res, Endianness e /* = UNKNOWN*/) const
{
	const auto* seg = getSegmentFromAddress(address);
	static_assert(
		sizeof(res) * CHAR_BIT == byteorder::kAccumulatorBits,
		"widthFits bounds the width against a 64-bit accumulator; res must be one");
	// x == 0 is still let past this guard, exactly as the product did (0 times
	// anything is 0, which is not greater than 64); what a zero-width read
	// means is then decided below by getBytes and createValueFromBytes, which
	// is where it was decided before. Only the wrapped widths are new here.
	if (!seg || (x != 0 && !bounds_kernels::xWidthFitsAccumulator(x, getByteLength())))
	{
		return false;
	}

	std::vector<std::uint8_t> data;
	if (!seg->getBytes(data, address - seg->getAddress(), x) || data.size() != x)
	{
		return false;
	}

	return createValueFromBytes(data, res, e);
}

/**
 * Get @a x bytes long byte array from specified address
 *
 * @param address Address to get array from
 * @param x       Number of bytes for get
 * @param res     Result array.
 *
 * @return Status of operation (@c true if all is OK, @c false otherwise)
 */
bool Image::getXBytes(std::uint64_t address, std::uint64_t x, std::vector<std::uint8_t>& res) const
{
	const auto* seg = getSegmentFromAddress(address);
	if (!seg)
	{
		return false;
	}

	res.clear();

	// x arrives from the caller with no bound at all, and every clamp between
	// here and the copy is a wrapping sum: Segment::getBytes shortens size with
	// `addressOffset + size >= getSize()` only after SegmentDataSource::loadData
	// has run with the unclamped size, and loadData shortens with
	// `loadOffset + loadSize >= getDataSize()` immediately before copying
	// loadSize bytes. At an address offset of 10 with x = 0xFFFFFFFFFFFFFFFB
	// both sums come to 5, so for any segment longer than five bytes neither
	// clamp fires and the copy runs off the end of the segment's data.
	// Refusing here changes no in-range answer: Segment::getBytes
	// zero-fills to `min(x, getSize() - addressOffset)`, so a read that runs
	// past the segment already failed the `res.size() != x` test below.
	const auto segOffset = address - seg->getAddress();
	if (!bounds_kernels::rangeFitsWide(segOffset, seg->getSize(), x))
	{
		return false;
	}

	if (!seg->getBytes(res, segOffset, x) || res.size() != x)
	{
		return false;
	}

	return true;
}

bool Image::setXByte(
	std::uint64_t address,
	std::uint64_t x,
	std::uint64_t val,
	retdec::utils::Endianness e /* = retdec::utils::Endianness::UNKNOWN*/)
{
	const auto* seg = getSegmentFromAddress(address);
	static_assert(
		sizeof(val) * CHAR_BIT == byteorder::kAccumulatorBits,
		"widthFits bounds the width against a 64-bit accumulator; val must be one");
	// Same guard as getXByte, and the same witness: x = 0x2000000000000002 with
	// an 8-bit byte length wrapped to 16, so `16 > 64` was false and the width
	// was let through. x == 0 is left to createBytesFromValue below, as before.
	if (!seg || (x != 0 && !bounds_kernels::xWidthFitsAccumulator(x, getByteLength())))
	{
		return false;
	}

	std::vector<std::uint8_t> data;
	if (!createBytesFromValue(val, x, data, e))
	{
		return false;
	}

	return setXBytes(address, data);
}

bool Image::setXBytes(std::uint64_t address, const std::vector<std::uint8_t>& val)
{
	auto* seg = getSegmentFromAddress(address);
	if (!seg)
	{
		return false;
	}

	return seg->setBytes(val, address - seg->getAddress());
}

/**
 * Find out, if there is a pointer (valid address) on the provided address
 * @param address Address to check
 * @param pointer If not @c nullptr, and there is a pointer on @p address, then
 *                set the pointer value to where this parameter points.
 * @return @c True if pointer on address, @c false otherwise
 */
bool Image::isPointer(std::uint64_t address, std::uint64_t* pointer) const
{
	std::uint64_t val = 0;
	if (getWord(address, val) && hasDataOnAddress(val))
	{
		if (pointer)
		{
			*pointer = val;
		}
		return true;
	}
	return false;
}

const std::string& Image::getStatusMessage() const
{
	return _statusMessage;
}

void Image::setStatusMessage(const std::string& message)
{
	_statusMessage = message;
}

const retdec::fileformat::LoaderErrorInfo& Image::getLoaderErrorInfo() const
{
	return getFileFormat()->getLoaderErrorInfo();
}

Segment* Image::insertSegment(std::unique_ptr<Segment> segment)
{
	_segments.push_back(std::move(segment));

	// We have used move constructor, segment is no longer valid pointer
	// Now give segment name
	Segment* retSegment = _segments.back().get();
	nameSegment(retSegment);
	return retSegment;
}

void Image::removeSegment(Segment* segment)
{
	for (auto itr = _segments.begin(); itr != _segments.end(); ++itr)
	{
		if (itr->get() == segment)
		{
			_segments.erase(itr);
			return;
		}
	}
}

void Image::nameSegment(Segment* segment)
{
	if (segment->getSecSeg() == nullptr || segment->getSecSeg()->getName().empty())
		segment->setName(_namelessSegNameGen.getNextName());
	else
		segment->setName(segment->getSecSeg()->getName());
}

void Image::sortSegments()
{
	std::stable_sort(
		_segments.begin(),
		_segments.end(),
		[](const std::unique_ptr<Segment>& seg1, const std::unique_ptr<Segment>& seg2) {
			return seg1->getAddress() < seg2->getAddress();
		});
}

const Segment* Image::_getSegment(std::size_t index) const
{
	if (index >= getNumberOfSegments()) return nullptr;

	return _segments[index].get();
}

const Segment* Image::_getSegment(const std::string& name) const
{
	for (const auto& segment: getSegments())
	{
		if (segment->getName() == name) return segment.get();
	}

	return nullptr;
}

const Segment* Image::_getSegmentWithIndex(std::size_t index) const
{
	for (const auto& seg: _segments)
	{
		if (seg->getSecSeg()->getIndex() == index) return seg.get();
	}

	return nullptr;
}

const Segment* Image::_getSegmentFromAddress(std::uint64_t address) const
{
	for (const auto& segment: getSegments())
	{
		if (segment->containsAddress(address)) return segment.get();
	}

	return nullptr;
}

} // namespace loader
} // namespace retdec

#endif // RETDEC_LOADER_BOUNDS_KERNELS_ONLY
