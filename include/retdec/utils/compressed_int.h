/**
 * @file include/retdec/utils/compressed_int.h
 * @brief ECMA-335 II.23.2 compressed integers, decoded from a blob nobody vouched for.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Every signature, every blob-heap entry and every user string in a .NET image
 * begins with one of these. src/cli_parser reads them at sixteen call sites --
 * thirteen in cli_sig.cpp, three in cli_heaps.cpp, one of those last being
 * decodeCompressedInt calling the unsigned form -- and each one is reading
 * bytes the file chose.
 *
 * They are not LEB128 and do not belong in leb128.h. LEB128 is a
 * continuation-bit encoding: the decoder discovers the width one byte at a
 * time and the failure mode is a run that never terminates. This is a
 * prefix-length encoding: the high bits of the FIRST byte select the width
 * outright, from ECMA-335 II.23.2:
 *
 *     0xxxxxxx                              1 byte,   7 payload bits
 *     10xxxxxx xxxxxxxx                     2 bytes, 14 payload bits
 *     110xxxxx xxxxxxxx xxxxxxxx xxxxxxxx   4 bytes, 29 payload bits
 *     111xxxxx                              not a compressed integer
 *
 * so the width is known before the second byte is fetched, and the failure
 * mode is a declared width the blob is too short to supply. Same contract as
 * leb128.h -- a refusal consumes nothing -- different decoder.
 *
 * ## The signed form, and the bug this kernel exists to stop
 *
 * A signed compressed integer is the unsigned one with the sign bit ROTATED
 * into the low bit of the payload, so small negative values stay short. Decode
 * is therefore: shift right by one, put the low bit back at the top of the
 * payload, sign-extend from there. The width of "the payload" is fixed by how
 * many bytes were consumed and by nothing else.
 *
 * src/cli_parser/cli_heaps.cpp:48 picks it from the decoded magnitude instead:
 *
 *     if      (*uval <= 0x3F)   val |= 0xFFFFFFC0u;
 *     else if (*uval <= 0x3FFF) val |= 0xFFFFE000u;
 *     else                      val |= 0xF0000000u;
 *
 * The magnitude and the width are not the same thing. A one-byte encoding
 * spans 0x00..0x7F, so every odd byte above 0x3F -- all 32 of them -- takes
 * the two-byte branch. ECMA-335 II.23.2 gives 0x7F as the encoding of -1;
 * this returns 63 | 0xFFFFE000 = 0xFFFFE03F = -8129. The same for 0x41 (-32,
 * returned as -8160) through 0x7D (-2, returned as -8130).
 *
 * The error is not confined to the one-byte form, which is the part worth
 * being exact about because it is easy to assume the other two branches are
 * the right ones misapplied. They are not: they are the same mistake in the
 * other direction. ESBMC refutes all three widths against the II.23.2
 * definition, since a value that is small FOR ITS WIDTH takes a branch
 * narrower than the encoding it arrived in --
 *
 *     0x7F                 means         -1, returned as      -8129
 *     0x80 0x3F            means      -8161, returned as        -33
 *     0xC0 0x00 0x24 0x7F  means -268430785, returned as      -3521
 *
 * -- and no single test on the magnitude can be right for more than one width,
 * because the same magnitude is a legal payload at all three.
 *
 * Those values are array lower bounds. cli_sig.cpp:132 feeds every one of them
 * into decodeArrayShape's loBounds, so a multi-dimensional array declared
 * `[-1..n]` decompiles as `[-8129..n]`.
 *
 * ## Contract
 *
 * Header-only, constexpr, no allocation, no exceptions, raw pointer and size
 * rather than a span. Failure returns a zeroed Result: bytesRead 0, value 0,
 * ok false, and the caller's cursor is its own to advance -- the leb128.h
 * convention, so a refusal is retryable and cannot desynchronise the rest of
 * the blob.
 *
 * Whether the range fits comes from bounds.h; the masks and the extension come
 * from byte_order.h and leb128.h, so this file introduces no second copy of
 * either. Proved in tests/verification/compressed_int_proof.cpp.
 *
 * Do not re-derive any of this at a call site; call it.
 */

#ifndef RETDEC_UTILS_COMPRESSED_INT_H
#define RETDEC_UTILS_COMPRESSED_INT_H

#include "retdec/utils/bounds.h"
#include "retdec/utils/byte_order.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace cint {

/// Widest encoding ECMA-335 II.23.2 defines.
constexpr std::size_t kMaxBytes = 4;

/// Payload bits carried by an encoding of @p bytesConsumed bytes.
///
/// The three widths of II.23.2 and nothing else: 7, 14, 29. Note that these are
/// not 8 * bytes - tag bits by accident, they ARE that -- 8-1, 16-2, 32-3 --
/// but the table is the authority, so it is written out rather than computed
/// from a formula that would also happily answer for three bytes.
///
/// Returns 0 for any other count, which is the answer to a question with no
/// meaning. Every caller here checks it, so a 0 can never become a shift count.
constexpr unsigned payloadBits(std::size_t bytesConsumed) noexcept
{
	switch (bytesConsumed)
	{
	case 1: return 7;
	case 2: return 14;
	case 4: return 29;
	default: return 0;
	}
}

/// Largest unsigned value any compressed integer can denote: 2^29 - 1.
///
/// From payloadBits(kMaxBytes) rather than written out, so the two cannot
/// drift. byteorder::lowMask is the tree's one mask implementation.
constexpr std::uint32_t kMaxValue = static_cast<std::uint32_t>(byteorder::lowMask(payloadBits(kMaxBytes)));

/// Payload bits carried by the FIRST byte of a @p bytes-byte encoding: 7, 6, 5.
///
/// The rest of the payload is whole bytes, so this is what is left after them.
/// The decoder masks the first byte with exactly this many bits and the
/// encoder shifts exactly this far, which is why they are the same function.
constexpr unsigned firstByteBits(std::size_t bytes) noexcept
{
	const unsigned total = payloadBits(bytes);
	if (total == 0) return 0;
	// bytes is 1, 2 or 4 here, so the product is at most 24 and the difference
	// cannot go negative: 7-0, 14-8, 29-24.
	return total - byteorder::kBitsPerByte * static_cast<unsigned>(bytes - 1);
}

/// The prefix that selects a @p bytes-byte encoding: 0x00, 0x80, 0xC0.
///
/// Straight off the II.23.2 table quoted at the top of this file. It sits in
/// the bits above the first byte's payload, which is what makes
/// `(b0 & ~lowMask(firstByteBits)) == tagFor(bytes)` the classification test.
constexpr std::uint8_t tagFor(std::size_t bytes) noexcept
{
	switch (bytes)
	{
	case 1: return 0x00;  // 0xxxxxxx
	case 2: return 0x80;  // 10xxxxxx
	case 4: return 0xC0;  // 110xxxxx
	default: return 0xFF; // 111xxxxx is not a compressed integer
	}
}

/// How many bytes the encoding beginning with @p b0 claims, or 0 for none.
///
/// Order matters and is the one place the classification can go wrong: the
/// tests are nested prefixes, so the widest tag has to be excluded before the
/// narrower ones would match it. Written as a descending walk over the three
/// forms so that no test can accept a byte a wider tag already claimed.
constexpr std::size_t widthOf(std::uint8_t b0) noexcept
{
	if ((b0 & 0x80) == 0x00) return 1; // 0xxxxxxx
	if ((b0 & 0xC0) == 0x80) return 2; // 10xxxxxx
	if ((b0 & 0xE0) == 0xC0) return 4; // 110xxxxx
	return 0;                          // 111xxxxx
}

/// Outcome of an unsigned decode.
struct Result
{
	std::uint32_t value = 0;   ///< Decoded value, 0 when !ok; never above kMaxValue.
	std::size_t bytesRead = 0; ///< Bytes consumed, 0 when !ok.
	bool ok = false;           ///< False on a bad tag or a blob too short for it.
};

/// Decode an unsigned compressed integer at @p pos in a buffer of @p size bytes.
///
/// Fails, rather than guessing, when the first byte carries the 111xxxxx tag
/// that II.23.2 does not define, or when the blob cannot supply the width that
/// tag declares. @p pos is not advanced; the caller adds Result::bytesRead, so
/// a failed decode cannot leave a cursor somewhere undefined.
///
/// The bound is bounds::rangeFits, which compares against the bytes remaining
/// and never forms pos + width. cli_heaps.cpp:35 writes it as
/// `pos + 3 >= blob.size()`: with pos within three of SIZE_MAX that sum wraps
/// to a small number, the guard passes, and the four reads that follow are off
/// the end of the blob.
inline constexpr Result decodeUnsigned(const std::uint8_t* data, std::size_t size, std::size_t pos) noexcept
{
	if (data == nullptr) return Result{};
	if (!bounds::rangeFits(pos, size, 1)) return Result{};

	const std::uint8_t b0 = data[pos];
	const std::size_t width = widthOf(b0);
	if (width == 0) return Result{};
	// The width is known before any further byte is fetched. Nothing below this
	// line reads until the blob has been shown to hold all of it.
	if (!bounds::rangeFits(pos, size, width)) return Result{};

	// The first byte's payload, with the tag masked off by the same width the
	// tag selected.
	std::uint32_t value = static_cast<std::uint32_t>(b0 & byteorder::lowMask(firstByteBits(width)));

	// Big-endian, most significant byte first -- II.23.2 stores the wide forms
	// the other way round from every little-endian field in the same file.
	for (std::size_t i = 1; i < width; ++i)
	{
		// rangeFits(pos, size, width) gives pos + width <= size, so pos + i is
		// below size for every i < width and the shift is at most 8 * 3 = 24.
		value = (value << byteorder::kBitsPerByte) | data[pos + i];
	}

	return Result{value, width, true};
}

/// Outcome of a signed decode.
struct SResult
{
	std::int32_t value = 0;    ///< Decoded value, 0 when !ok.
	std::size_t bytesRead = 0; ///< Bytes consumed, 0 when !ok.
	bool ok = false;           ///< False on a bad tag or a blob too short for it.
};

/// The rotate II.23.2 applies, undone: the low bit of @p u is the sign bit and
/// belongs at the top of a @p bits-wide payload.
///
/// This is the whole of the headline defect, stated as one function so that
/// there is one place where the width is chosen. @p bits comes from
/// payloadBits(bytesRead) and from nothing else; in particular it does not come
/// from how large @p u happens to be.
///
/// ECMA-335 II.23.2 gives the encodings to check it against: 0x7F is -1,
/// 0x01 is -64, 0x7B is -3. All three are one byte, so all three extend from
/// bit 6 -- and the last two have magnitudes a magnitude-driven test would put
/// in different buckets.
constexpr std::int32_t signFromWidth(std::uint32_t u, unsigned bits) noexcept
{
	if (bits == 0) return 0;
	// bits is 7, 14 or 29, so this shift count is at most 28.
	const std::uint64_t rotated =
		(static_cast<std::uint64_t>(u) >> 1) | ((static_cast<std::uint64_t>(u) & 1) << (bits - 1));
	// One sign-extension implementation in the tree, in byte_order.h, which
	// does it in unsigned arithmetic and converts once at the end.
	return static_cast<std::int32_t>(byteorder::signExtendFrom(rotated, bits));
}

/// Decode a signed compressed integer at @p pos in a buffer of @p size bytes.
///
/// Same bound and same refusal convention as decodeUnsigned; the only addition
/// is the rotate. The result lies in [-2^(n-1), 2^(n-1)-1] for the n payload
/// bits of the width consumed: [-64, 63], [-8192, 8191], [-2^28, 2^28-1].
inline constexpr SResult decodeSigned(const std::uint8_t* data, std::size_t size, std::size_t pos) noexcept
{
	const Result u = decodeUnsigned(data, size, pos);
	if (!u.ok) return SResult{};
	const unsigned bits = payloadBits(u.bytesRead);
	if (bits == 0) return SResult{}; // unreachable: bytesRead is 1, 2 or 4.
	return SResult{signFromWidth(u.value, bits), u.bytesRead, true};
}

/// The rotate II.23.2 applies: the sign bit of the @p bits-wide two's-complement
/// form of @p v moves to the low bit. The inverse of signFromWidth.
constexpr std::uint32_t signToLowBit(std::int32_t v, unsigned bits) noexcept
{
	if (bits == 0) return 0;
	// Conversion to an unsigned type is modular for every input, so this is
	// defined for negative v rather than implementation-defined.
	const std::uint64_t w = byteorder::zeroExtendFrom(static_cast<std::uint64_t>(static_cast<std::int64_t>(v)), bits);
	const std::uint64_t rotated = ((w << 1) | (w >> (bits - 1))) & byteorder::lowMask(bits);
	return static_cast<std::uint32_t>(rotated);
}

/// True when @p v fits a signed field of @p bits bits.
constexpr bool signedFits(std::int64_t v, unsigned bits) noexcept
{
	if (bits == 0 || bits >= byteorder::kAccumulatorBits) return false;
	const std::int64_t half = static_cast<std::int64_t>(std::uint64_t{1} << (bits - 1));
	return v >= -half && v <= half - 1;
}

/// Write @p v as exactly @p bytes bytes at @p out, returning @p bytes, or 0.
///
/// Returns 0 -- writing nothing -- when @p bytes is not one of the three
/// widths, when @p v needs more payload bits than that width carries, or when
/// @p outCap cannot hold it. A short buffer produces no partial encoding, so a
/// caller that ignores the return value has still not been written past.
inline std::size_t encodeUnsignedAs(std::uint32_t v, std::size_t bytes, std::uint8_t* out, std::size_t outCap) noexcept
{
	const unsigned bits = payloadBits(bytes);
	if (bits == 0 || out == nullptr) return 0;
	if (v > static_cast<std::uint32_t>(byteorder::lowMask(bits))) return 0;
	if (!bounds::rangeFits(0, outCap, bytes)) return 0;

	// Most significant byte first, tag folded into it. firstByteBits(bytes) is
	// 7, 6 or 5 and the tail is 0, 8 or 24 bits, so no shift here reaches 32.
	const unsigned tailBits = bits - firstByteBits(bytes);
	out[0] = static_cast<std::uint8_t>(tagFor(bytes) | (v >> tailBits));
	for (std::size_t i = 1; i < bytes; ++i)
	{
		const unsigned shift = byteorder::kBitsPerByte * static_cast<unsigned>(bytes - 1 - i);
		out[i] = static_cast<std::uint8_t>((v >> shift) & 0xFF);
	}
	return bytes;
}

/// Encode @p v in the narrowest of the three widths that holds it.
///
/// Returns 0 for a value above kMaxValue -- no compressed integer denotes it --
/// and 0 when @p outCap is too small, having written nothing.
inline std::size_t encodeUnsigned(std::uint32_t v, std::uint8_t* out, std::size_t outCap) noexcept
{
	// Ascending, so the narrowest width that holds v is the one chosen. The
	// width test is `v fits in payloadBits(n)`, which is the same test
	// encodeUnsignedAs applies, so a width accepted here is never refused there.
	if (v <= static_cast<std::uint32_t>(byteorder::lowMask(payloadBits(1)))) return encodeUnsignedAs(v, 1, out, outCap);
	if (v <= static_cast<std::uint32_t>(byteorder::lowMask(payloadBits(2)))) return encodeUnsignedAs(v, 2, out, outCap);
	if (v <= kMaxValue) return encodeUnsignedAs(v, 4, out, outCap);
	return 0;
}

/// Encode @p v in the narrowest width whose SIGNED range holds it.
///
/// The width cannot be chosen from the rotated value: II.23.2 encodes -8192 as
/// 0x80 0x01, whose rotated payload is 1, and encoding that 1 in one byte would
/// give 0x01 -- which is the encoding of -64. So the width is settled first,
/// from the signed range, and the rotate is then done at that width.
///
/// Returns 0 for a value outside [-2^28, 2^28-1], and 0 when @p outCap is too
/// small, having written nothing.
inline std::size_t encodeSigned(std::int32_t v, std::uint8_t* out, std::size_t outCap) noexcept
{
	const std::int64_t wide = v;
	if (signedFits(wide, payloadBits(1))) return encodeUnsignedAs(signToLowBit(v, payloadBits(1)), 1, out, outCap);
	if (signedFits(wide, payloadBits(2))) return encodeUnsignedAs(signToLowBit(v, payloadBits(2)), 2, out, outCap);
	if (signedFits(wide, payloadBits(kMaxBytes)))
		return encodeUnsignedAs(signToLowBit(v, payloadBits(kMaxBytes)), kMaxBytes, out, outCap);
	return 0;
}

} // namespace cint
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_COMPRESSED_INT_H
