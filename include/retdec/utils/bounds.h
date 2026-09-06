/**
 * @file include/retdec/utils/bounds.h
 * @brief Bounds arithmetic for values that came out of an untrusted file.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Every parser in this tree reads counts, lengths and offsets out of files it
 * did not write. Fuzzing found the same mistake in six of them: the declared
 * value was used to allocate, index or advance before anything checked that the
 * input could actually supply it. A five-byte header claiming 2^31 elements is
 * not a large file, it is a malformed one.
 *
 * These helpers are the single place that arithmetic lives, so it can be proved
 * once instead of re-argued at every call site. They are:
 *
 *   - header-only, constexpr, and free of anything but <cstddef>/<cstdint>, so
 *     ESBMC can reason about them without an operational model of the STL;
 *   - total: every function is defined for every input, with no precondition a
 *     caller can violate;
 *   - proved exhaustively over the whole 64-bit domain by the harnesses in
 *     tests/verification/, run by scripts/verify_esbmc.sh.
 *
 * The point of the proof is that it covers the code that actually runs. Do not
 * copy this logic into a call site; call it.
 */

#ifndef RETDEC_UTILS_BOUNDS_H
#define RETDEC_UTILS_BOUNDS_H

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace bounds {

/// Bytes still available in a buffer of @p size when positioned at @p pos.
///
/// Saturates at zero rather than wrapping, so a caller that has somehow run
/// past the end gets 0 and not SIZE_MAX.
constexpr std::size_t remaining(std::size_t pos, std::size_t size) noexcept
{
	return pos < size ? size - pos : 0;
}

/// True when @p a + @p b is representable, i.e. the addition does not wrap.
///
/// Written as a subtraction because computing the sum first is the bug this
/// exists to prevent.
constexpr bool addFits(std::size_t a, std::size_t b) noexcept
{
	return b <= SIZE_MAX - a;
}

/// True when @p a * @p b is representable.
constexpr bool mulFits(std::size_t a, std::size_t b) noexcept
{
	return a == 0 || b <= SIZE_MAX / a;
}

/// True when the half-open range [@p pos, @p pos + @p len) lies inside a buffer
/// of @p size.
///
/// The sum is never formed. `pos + len > size` is the natural way to write this
/// and is wrong: with a length read from a file, the addition wraps and the
/// check passes. Comparing against the remaining bytes cannot wrap.
constexpr bool rangeFits(std::size_t pos, std::size_t size, std::size_t len) noexcept
{
	return pos <= size && len <= size - pos;
}

/// True when a container declaring @p count elements could be satisfied by the
/// input left at @p pos.
///
/// @p minBytesPerElement is the smallest number of bytes one element can occupy
/// on the wire -- for a tagged format that is 1, the type byte. A count larger
/// than the remaining input divided by that is malformed by construction: no
/// well-formed continuation of the file could supply it. This is a bound the
/// data itself provides, which is why it is preferred over a fixed cap.
///
/// A @p minBytesPerElement of 0 is treated as 1, so a hostile header that
/// declares zero-width elements cannot make the bound vacuous.
constexpr bool countFits(
		std::size_t pos,
		std::size_t size,
		std::size_t count,
		std::size_t minBytesPerElement = 1) noexcept
{
	const std::size_t width = minBytesPerElement == 0 ? 1 : minBytesPerElement;
	// `pos <= size` is not redundant with the division: remaining() saturates,
	// so without it a caller already past the end of the buffer would be told
	// that a count of zero "fits", and countFits would stop implying rangeFits.
	// ESBMC found that corner (pos = SIZE_MAX-34, size = 0, count = 0).
	return pos <= size && count <= remaining(pos, size) / width;
}

/// countFits for a count that arrived as a signed field.
///
/// A negative count is rejected before the conversion, not after: converting
/// first turns -1 into SIZE_MAX, which is exactly the value that then gets
/// handed to reserve().
constexpr bool signedCountFits(
		std::int64_t count,
		std::size_t pos,
		std::size_t size,
		std::size_t minBytesPerElement = 1) noexcept
{
	return count >= 0
		&& countFits(pos, size, static_cast<std::size_t>(count), minBytesPerElement);
}

/// Pages of @p pageSize needed to hold @p bytes, never fewer than one.
///
/// Rounds the page *count* up, which is the distinction mini_emu got wrong:
/// rounding the byte count up instead makes a copy read past the end of the
/// caller's buffer. Returns 0 only for a @p pageSize of 0, which has no
/// meaningful answer.
///
/// The count covers the bytes -- `(n - 1) * pageSize < bytes <= n * pageSize`
/// -- but note that `n * pageSize` itself is not guaranteed to be
/// representable: for `bytes = SIZE_MAX` and `pageSize = 2` the count is 2^63
/// and the product wraps. Callers that need the byte span rather than the page
/// count must check `mulFits(pageCount(...), pageSize)` first. Nothing in this
/// tree does; every caller iterates pages.
constexpr std::size_t pageCount(std::size_t bytes, std::size_t pageSize) noexcept
{
	if (pageSize == 0) return 0;
	if (bytes == 0) return 1;
	return 1 + (bytes - 1) / pageSize;
}

/// @p value, capped at @p limit.
constexpr std::size_t clamp(std::size_t value, std::size_t limit) noexcept
{
	return value < limit ? value : limit;
}

/// How many elements to reserve up front for a container declaring @p declared.
///
/// Never more than the container will hold and never more than @p cap, so a
/// large declared count cannot commit memory before its elements have been
/// read. Growth covers the rest.
constexpr std::size_t reserveFor(std::size_t declared, std::size_t cap) noexcept
{
	return clamp(declared, cap);
}

/// True when reading @p count elements of @p elementSize bytes from @p pos
/// stays inside a buffer of @p size, with no overflow in the element-count
/// multiplication.
constexpr bool arrayFits(
		std::size_t pos,
		std::size_t size,
		std::size_t count,
		std::size_t elementSize) noexcept
{
	return mulFits(count, elementSize) && rangeFits(pos, size, count * elementSize);
}

/// True when a structure of @p len bytes at absolute file offset @p offset lies
/// inside a file of @p size.
///
/// Same shape as rangeFits, named for the offset/size pairs that appear in
/// every container header (DEX map lists, PE sections, ELF program headers).
constexpr bool offsetFits(std::size_t offset, std::size_t len, std::size_t size) noexcept
{
	return rangeFits(offset, size, len);
}

} // namespace bounds
} // namespace utils
} // namespace retdec

#endif
