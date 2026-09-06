/**
 * @file include/retdec/utils/leb128.h
 * @brief LEB128 decoding that cannot shift out of range or read past the end.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * LEB128 appears wherever this tree reads a container format: DWARF and the EH
 * tables, Python 3.11 line tables and exception tables, WebAssembly, DEX. It is
 * a continuation-bit encoding with no length prefix, so the decoder decides for
 * itself when to stop — and a hand-written one usually decides wrong in the same
 * two ways:
 *
 *   - The shift is not bounded. A run of bytes with the continuation bit set
 *     drives `shift` past the width of the accumulator, and `x << shift` with
 *     `shift >= width` is undefined behaviour. Signed accumulators are worse:
 *     `(int64_t)0x7F << 57` overflows, which is undefined before the shift
 *     count ever becomes the problem.
 *   - The cursor is not bounded, so a stream that never clears the continuation
 *     bit walks off the end of the buffer.
 *
 * These decoders bound both. They accumulate in an unsigned type, where a shift
 * is defined for every count below the width and overflow wraps rather than
 * being undefined, and convert to signed only at the end.
 *
 * Header-only, constexpr, and free of everything but <cstddef>/<cstdint>, so
 * the proofs in tests/verification/ can reason about them directly. Do not
 * re-implement this in a call site; call it.
 */

#ifndef RETDEC_UTILS_LEB128_H
#define RETDEC_UTILS_LEB128_H

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace leb128 {

/// Bits carried by one LEB128 byte.
constexpr unsigned kBitsPerByte = 7;

/// Most bytes a 64-bit value can occupy: ceil(64 / 7).
constexpr std::size_t kMaxBytes = 10;

/// One byte's payload, reduced to the bits that still fit above @p shift.
///
/// The last byte of a maximal encoding carries more payload bits than remain in
/// a 64-bit word. Letting the shift discard them is defined for an unsigned
/// type, but it is discarding information silently; masking first says so, and
/// keeps the shift lossless so a verifier flagging lossy shifts is flagging a
/// real mistake rather than this one.
constexpr std::uint64_t payloadFitting(std::uint8_t byte, unsigned shift) noexcept
{
	const std::uint64_t payload = static_cast<std::uint64_t>(byte & 0x7F);
	if (shift >= 64) return 0;
	const unsigned room = 64 - shift;
	if (room >= kBitsPerByte) return payload;
	return payload & ((std::uint64_t{1} << room) - 1);
}

/// Mask of every bit at or above @p shift, built without a lossy shift.
///
/// `~uint64_t{0} << shift` is the obvious spelling and throws away the top
/// `shift` bits on the way; `1 << shift` never discards a set bit for
/// shift < 64, so this form is exact.
constexpr std::uint64_t maskFrom(unsigned shift) noexcept
{
	if (shift >= 64) return 0;
	return ~((std::uint64_t{1} << shift) - 1);
}

/// Outcome of a decode.
struct Result {
	std::uint64_t value = 0;     ///< Decoded value; 0 when !ok.
	std::size_t   bytesRead = 0; ///< Bytes consumed, 0 when !ok.
	bool          ok = false;    ///< False on truncation or an over-long encoding.
};

/// Decode an unsigned LEB128 at @p pos in a buffer of @p size bytes.
///
/// Fails rather than guessing when the buffer ends before a terminating byte,
/// or when the encoding runs past kMaxBytes -- an encoding longer than that
/// cannot denote a 64-bit value, so accepting it would mean silently discarding
/// the bits that did not fit.
///
/// @p pos is not advanced; the caller adds Result::bytesRead, so a failed
/// decode cannot leave a cursor somewhere undefined.
constexpr Result decodeUnsigned(const std::uint8_t* data, std::size_t size, std::size_t pos) noexcept
{
	if (data == nullptr || pos >= size) return Result{};

	std::uint64_t value = 0;
	std::size_t   used = 0;
	unsigned      shift = 0;

	while (pos + used < size && used < kMaxBytes)
	{
		const std::uint8_t byte = data[pos + used];
		++used;

		// Bits at or above 64 cannot be represented, so they are dropped rather
		// than shifted in: `x << shift` with shift >= 64 is undefined, and this
		// is the branch a hostile encoding is trying to reach.
		if (shift < 64)
		{
			value |= payloadFitting(byte, shift) << shift;
		}

		if ((byte & 0x80) == 0) return Result{value, used, true};

		shift += kBitsPerByte;
	}

	// Ran out of input, or the encoding was longer than any 64-bit value needs.
	return Result{};
}

/// Decode a signed LEB128 at @p pos in a buffer of @p size bytes.
///
/// Accumulates unsigned and sign-extends at the end. Building the value in an
/// int64_t instead is undefined twice over: the shift overflows the signed
/// range long before the shift count reaches the width.
constexpr Result decodeSigned(const std::uint8_t* data, std::size_t size, std::size_t pos) noexcept
{
	if (data == nullptr || pos >= size) return Result{};

	std::uint64_t value = 0;
	std::size_t   used = 0;
	unsigned      shift = 0;
	std::uint8_t  last = 0;

	while (pos + used < size && used < kMaxBytes)
	{
		const std::uint8_t byte = data[pos + used];
		++used;
		last = byte;

		if (shift < 64)
		{
			value |= payloadFitting(byte, shift) << shift;
		}

		if ((byte & 0x80) == 0)
		{
			shift += kBitsPerByte;
			// Sign-extend from the last payload bit, in unsigned arithmetic.
			// Only meaningful while some bits above the sign remain; at or past
			// 64 the value already occupies the whole word.
			if (shift < 64 && (last & 0x40) != 0)
			{
				value |= maskFrom(shift);
			}
			return Result{value, used, true};
		}

		shift += kBitsPerByte;
	}

	return Result{};
}

/// decodeSigned, reinterpreted as a two's-complement signed value.
constexpr std::int64_t toSigned(std::uint64_t value) noexcept
{
	// Defined for every input, unlike a cast that assumes the value is in range:
	// values above INT64_MAX map onto the negative half, which is exactly the
	// two's-complement reading the encoding intends.
	return value <= static_cast<std::uint64_t>(INT64_MAX)
		? static_cast<std::int64_t>(value)
		: -static_cast<std::int64_t>(~value) - 1;
}

} // namespace leb128
} // namespace utils
} // namespace retdec

#endif
