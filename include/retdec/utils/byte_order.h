/**
 * @file include/retdec/utils/byte_order.h
 * @brief Turning a byte range from an untrusted file into a fixed-width integer.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Fourteen surveys of this tree found the same primitive under nine names:
 * ByteValueStorage::createValueFromBytes/createBytesFromValue,
 * DynamicBuffer::readImpl/writeImpl, PeReader::read16/32/64, CLIReader's
 * readSignedLE, MetadataTables::RowReader::u8/u16/u32, JVM BinaryReader::u2/u4/u8,
 * DEX readEncodedBits/signExtendEncoded, and MarshalReader::readU16LE/readS32LE.
 * All of them do one thing. Most of them get the shift bound wrong in at least
 * one direction, and three are undefined behaviour today:
 *
 *   - src/utils/byte_value_storage.cpp:958 shifts a uint64_t by
 *     `getByteLength() * i`. `realSize` is `data.size() - offset`, so a caller
 *     handing it a nine-byte span reaches `1 << 64` on the last iteration.
 *   - src/dex_parser/dex_class_parser.cpp:442 accumulates into a `uint32_t`
 *     while shifting by `b * 8` for b up to `argBits`, which the file supplies
 *     as three bits. argBits = 4 gives `uint32_t << 32`; argBits = 7 gives
 *     `uint32_t << 56`.
 *   - src/cli_parser/cli_reader.cpp:710 indexes `b[n - 1]` after a loop that
 *     was careful to stop at `b.size()`. ESBMC returns n = 4, size = 2: the
 *     loop reads b[0] and b[1] and stops, then the sign test reads b[3], one
 *     past the end of a two-byte constant blob.
 *
 * bounds.h owns whether the range fits. Nothing owned what happens after it
 * does: the shift bound, the byte order, and the extension direction. The last
 * of those is a silent wrong-answer bug rather than a crash, and it is live:
 * dex_class_parser.cpp:95 decodes a shortened VALUE_FLOAT by assembling the
 * supplied bytes at the LOW end, but the DEX encoding says they are the HIGH
 * bytes -- "zero-extended to the right". The single byte 0x3F means the pattern
 * 0x3F000000, which is 0.5f; assembled low it is 0x0000003F = 63, a denormal of
 * about 8.8e-44. dex_class_parser.cpp:92 routes VALUE_CHAR -- unsigned by the
 * DEX specification -- through the sign-extending path, so a one-byte 0x80
 * becomes -128 rather than 128.
 *
 * The mask comes from leb128::maskFrom, so there is exactly one mask
 * implementation in the tree rather than two that can drift apart.
 *
 * Header-only, constexpr where it can be, no allocation, no exceptions, raw
 * pointers and sizes rather than containers, and free of everything but
 * <cstddef>/<cstdint> plus the two kernels above -- so ESBMC can reason about
 * it directly. Proved in tests/verification/byte_order_proof.cpp.
 *
 * Do not re-derive any of this at a call site; call it.
 */

#ifndef RETDEC_UTILS_BYTE_ORDER_H
#define RETDEC_UTILS_BYTE_ORDER_H

#include "retdec/utils/bounds.h"
#include "retdec/utils/leb128.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace byteorder {

/// Bits in one addressable byte of a std::uint8_t buffer.
constexpr unsigned kBitsPerByte = 8;

/// Bits in the accumulator every read produces and every write consumes.
constexpr unsigned kAccumulatorBits = 64;

/// Widest read or write this kernel performs: 64 bits at 8 bits per byte.
///
/// A caller asking for more is refused rather than truncated, because a
/// truncated answer is indistinguishable from a correct one at the call site.
///
/// Written as a literal rather than as kAccumulatorBits / kBitsPerByte because
/// ESBMC emits an arithmetic-overflow check for unsigned division even at
/// namespace scope, and the bitvector backends this harness pins report that
/// check as a violation. The static_assert keeps the two in step.
constexpr unsigned kMaxBytes = 8;
static_assert(kMaxBytes * kBitsPerByte == kAccumulatorBits,
              "kMaxBytes must be exactly the accumulator width in bytes");

/// Low @p bits bits set, every bit above them clear.
///
/// Built from leb128::maskFrom by complement so the tree has one mask, not two.
/// maskFrom(b) is every bit at or above b, so its complement is every bit below
/// b -- exact at both ends: maskFrom(0) is all-ones so lowMask(0) is 0, and
/// maskFrom(64) is 0 by its own definition so lowMask(64) is all-ones. Neither
/// end needs a special case here, which is the point of reusing it.
constexpr std::uint64_t lowMask(unsigned bits) noexcept
{
	return ~leb128::maskFrom(bits);
}

/// True when a shift of `index * bitsPerUnit` is defined for an operand of
/// @p accumulatorBits bits -- that is, when the count is strictly below the
/// width.
///
/// The product is never formed until both factors are known to be small. That
/// is not pedantry: the call sites this replaces form `x * getByteLength()`
/// with x straight out of a file (file_format.cpp:1998, image.cpp:378), and a
/// wrapped product compares as small and lets the shift through.
///
/// Written as a guarded multiplication rather than as
/// `index <= (accumulatorBits - 1) / bitsPerUnit` so that it has the same shape
/// as the expression it replaces. A call site converting to this reads as
/// "the product I was forming, checked", not as a different test it has to
/// convince itself is equivalent.
constexpr bool shiftFits(std::size_t index, unsigned bitsPerUnit, unsigned accumulatorBits) noexcept
{
	// A zero-width accumulator admits no shift at all, not even by zero.
	if (accumulatorBits == 0) return false;
	// A zero-width unit means every shift count is 0, which fits.
	if (bitsPerUnit == 0) return true;
	// One unit already covers the accumulator, so only index 0 shifts by less
	// than the width. Returning here keeps the factors below accumulatorBits.
	if (bitsPerUnit >= accumulatorBits) return index == 0;
	// bitsPerUnit >= 1 from here, so index >= accumulatorBits already loses.
	if (index >= accumulatorBits) return false;
	// Both factors are now strictly below accumulatorBits <= UINT_MAX, so the
	// product is below 2^64 and cannot wrap in this width.
	return static_cast<std::uint64_t>(index) * bitsPerUnit < accumulatorBits;
}

/// True when @p n units of @p bitsPerUnit bits each fit in the accumulator.
///
/// This is the `n * bitsPerUnit <= 64` test the two getXByte call sites need
/// (file_format.cpp:1998, image.cpp:378), with the product formed only once
/// both factors are known small enough that it cannot wrap. Equivalent to
/// `n <= 64 / bitsPerUnit` for bitsPerUnit >= 1 -- both forms were run under
/// z3, boolector and bitwuzla and discharged clean -- and stated as the product
/// so that a call site replacing `x * getByteLength() > 64` with `widthFits(x,
/// getByteLength())` is reading the same test, not a rearranged one.
constexpr bool widthFits(std::size_t n, unsigned bitsPerUnit) noexcept
{
	if (n == 0 || bitsPerUnit == 0) return false;
	// n >= 1, so a unit wider than the accumulator can never fit.
	if (bitsPerUnit > kAccumulatorBits) return false;
	// bitsPerUnit >= 1, so n above the accumulator width can never fit.
	if (n > kAccumulatorBits) return false;
	// Both factors are at most 64 now; the product is at most 4096.
	return n * bitsPerUnit <= kAccumulatorBits;
}

// ─── reads ───────────────────────────────────────────────────────────────────

/// Read @p n little-endian bytes at @p pos from a buffer of @p size bytes.
///
/// Returns false, leaving @p out untouched, unless n is in 1..8 AND the whole
/// range is inside the buffer. Both halves matter. Refusing n = 0 is not
/// fussiness: `createValueFromBytes` treats a zero size as "all of it", which
/// is how a caller who meant to read nothing ends up reading a whole section.
/// Refusing n > 8 is what byte_value_storage.cpp:958 does not do.
///
/// @p out is written only on success, so a caller that ignores the return value
/// keeps whatever it had rather than a half-assembled value.
inline bool readLE(const std::uint8_t* data, std::size_t size, std::size_t pos,
                   unsigned n, std::uint64_t& out) noexcept
{
	if (data == nullptr) return false;
	if (n < 1 || n > kMaxBytes) return false;
	if (!bounds::rangeFits(pos, size, n)) return false;

	std::uint64_t v = 0;
	for (unsigned i = 0; i < n; ++i)
	{
		// n <= 8 was established above, so i <= 7 and the count is at most 56.
		// The bound is the guard, not a comment: reaching 64 here is exactly
		// the undefined shift at byte_value_storage.cpp:958.
		v |= static_cast<std::uint64_t>(data[pos + i]) << (kBitsPerByte * i);
	}
	out = v;
	return true;
}

/// Read @p n big-endian bytes at @p pos from a buffer of @p size bytes.
///
/// Same contract as readLE, most significant byte first. This is the order the
/// JVM constant pool and every DEX-adjacent big-endian container uses.
inline bool readBE(const std::uint8_t* data, std::size_t size, std::size_t pos,
                   unsigned n, std::uint64_t& out) noexcept
{
	if (data == nullptr) return false;
	if (n < 1 || n > kMaxBytes) return false;
	if (!bounds::rangeFits(pos, size, n)) return false;

	std::uint64_t v = 0;
	for (unsigned i = 0; i < n; ++i)
	{
		// n - 1 - i runs from n-1 down to 0, so the count is at most 56 again.
		v |= static_cast<std::uint64_t>(data[pos + i]) << (kBitsPerByte * (n - 1 - i));
	}
	out = v;
	return true;
}

/// Read @p n units of @p bitsPerUnit bits each, as getXByte means it.
///
/// This is the shape ByteValueStorage::createValueFromBytes has: a byte length
/// that the format supplies (FileFormat says 8; RawDataFormat says whatever the
/// config said) and a count that the caller supplies. The two call sites that
/// bound it -- file_format.cpp:1998 and image.cpp:378 -- both write
/// `x * getByteLength() > sizeof(res) * CHAR_BIT`, forming a product of two
/// file-influenced values before comparing it. This refuses instead, via
/// widthFits, which never forms a product that could wrap.
///
/// @p bitsPerUnit below 8 is refused. The source is an array of std::uint8_t,
/// so a unit narrower than one element cannot be addressed by it at all, and
/// accepting one would mean silently overlapping adjacent units.
///
/// Each unit's payload is masked to @p bitsPerUnit bits before it is placed, so
/// nothing is discarded by the shift; the accumulate is `|=` rather than the
/// `+=` at byte_value_storage.cpp:959, which lets overlapping units carry.
inline bool readWidened(const std::uint8_t* data, std::size_t size, std::size_t pos,
                        std::size_t n, unsigned bitsPerUnit, bool bigEndian,
                        std::uint64_t& out) noexcept
{
	if (data == nullptr) return false;
	if (bitsPerUnit < kBitsPerByte) return false;
	if (!widthFits(n, bitsPerUnit)) return false;
	if (!bounds::rangeFits(pos, size, n)) return false;

	std::uint64_t v = 0;
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::size_t unit = bigEndian ? (n - 1 - i) : i;
		// widthFits(n, bitsPerUnit) means n * bitsPerUnit <= 64 and unit <= n-1,
		// so the count is at most 64 - bitsPerUnit <= 56.
		const unsigned shift = static_cast<unsigned>(unit) * bitsPerUnit;
		const std::uint64_t payload =
				static_cast<std::uint64_t>(data[pos + i]) & lowMask(bitsPerUnit);
		v |= payload << shift;
	}
	out = v;
	return true;
}

// ─── writes ──────────────────────────────────────────────────────────────────

/// Write the low @p n bytes of @p value little-endian into @p out.
///
/// Refuses unless n is in 1..8 and n <= @p outCap, and on refusal writes
/// nothing at all -- not a prefix. The counterpart at
/// byte_value_storage.cpp:1000 does neither: it resizes to a file-supplied x
/// and then runs `for (std::uint8_t i = 0; i < x; ++i)`, whose counter cannot
/// represent any index at or above 256. For x = 256 the counter wraps to 0 and
/// the loop never terminates; for x > 256 it never terminates either, and every
/// byte from 256 up is left as the resize wrote it.
inline bool writeLE(std::uint64_t value, std::size_t n,
                    std::uint8_t* out, std::size_t outCap) noexcept
{
	if (out == nullptr) return false;
	if (n < 1 || n > kMaxBytes) return false;
	if (n > outCap) return false;

	for (std::size_t i = 0; i < n; ++i)
	{
		// std::size_t counter against a std::size_t bound, so every index the
		// loop reaches is representable. n <= 8 keeps the count at most 56.
		const unsigned shift = static_cast<unsigned>(i) * kBitsPerByte;
		out[i] = static_cast<std::uint8_t>((value >> shift) & 0xFF);
	}
	return true;
}

/// Write the low @p n bytes of @p value big-endian into @p out.
inline bool writeBE(std::uint64_t value, std::size_t n,
                    std::uint8_t* out, std::size_t outCap) noexcept
{
	if (out == nullptr) return false;
	if (n < 1 || n > kMaxBytes) return false;
	if (n > outCap) return false;

	for (std::size_t i = 0; i < n; ++i)
	{
		const unsigned shift = static_cast<unsigned>(n - 1 - i) * kBitsPerByte;
		out[i] = static_cast<std::uint8_t>((value >> shift) & 0xFF);
	}
	return true;
}

// ─── extension ───────────────────────────────────────────────────────────────

/// Interpret the low @p bits bits of @p v as a two's-complement signed value.
///
/// The DEX version of this, dex_class_parser.cpp:79, is
///
///     return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
///
/// which is wrong twice over before the answer is even considered: the cast
/// converts an unsigned value above INT64_MAX to int64_t, which is
/// implementation-defined before C++20, and the `>>` is then a right shift of a
/// negative signed value, which is implementation-defined in every standard.
/// Both compile to the intended thing on the compilers this tree uses. Neither
/// is something to depend on, and neither is necessary.
///
/// This does it in unsigned arithmetic and converts once, at the end, through
/// leb128::toSigned, which is total. There is no signed shift on any path and
/// no out-of-range unsigned-to-signed conversion on any path.
///
/// @p bits of 0 yields 0: no bits were supplied, so no value was.
constexpr std::int64_t signExtendFrom(std::uint64_t v, unsigned bits) noexcept
{
	if (bits == 0) return 0;
	// The whole word is already the value; there is nothing above the sign bit
	// to fill, and `1 << 63` for the sign mask would be the last defined shift.
	if (bits >= kAccumulatorBits) return leb128::toSigned(v);

	const std::uint64_t low = v & lowMask(bits);
	// bits is in 1..63 here, so the count is at most 62.
	const std::uint64_t signBit = std::uint64_t{1} << (bits - 1);
	const std::uint64_t ext = (low & signBit) != 0 ? (low | leb128::maskFrom(bits)) : low;
	return leb128::toSigned(ext);
}

/// Place the @p suppliedBytes low bytes of @p v as the HIGH bytes of a
/// @p totalBytes-wide value, zero-filling below.
///
/// This is what a shortened DEX VALUE_FLOAT or VALUE_DOUBLE means: the encoding
/// drops the low-order zero bytes of the IEEE representation, so the bytes that
/// survive are the most significant ones and the missing ones are zeros on the
/// low side. dex_class_parser.cpp:95 and :103 assemble them at the low end
/// instead, which is wrong for every suppliedBytes below totalBytes: 1.0f is
/// 0x3F800000, so its encoding is the two bytes 80 3F (value_arg 1), and
/// assembling those at the low end gives 0x00003F80 -- a denormal of about
/// 2.3e-41 -- rather than 0x3F800000.
///
/// Returns 0 for a request that has no meaning -- totalBytes outside 1..8, or
/// suppliedBytes outside 1..totalBytes -- rather than shifting by a count it
/// cannot justify.
constexpr std::uint64_t extendHigh(std::uint64_t v, unsigned suppliedBytes,
                                   unsigned totalBytes) noexcept
{
	if (totalBytes == 0 || totalBytes > kMaxBytes) return 0;
	if (suppliedBytes == 0 || suppliedBytes > totalBytes) return 0;

	const unsigned suppliedBits = kBitsPerByte * suppliedBytes; // 8..64
	const unsigned shift = kBitsPerByte * (totalBytes - suppliedBytes); // 0..56
	// Anything above the supplied bytes is not part of the encoded value; drop
	// it here rather than letting the shift decide, so the shift is lossless.
	const std::uint64_t payload = v & lowMask(suppliedBits);
	return payload << shift;
}

/// Zero-extend the low @p bits bits of @p v: the unsigned reading.
///
/// The counterpart to signExtendFrom, present so that the choice between them
/// is a choice a caller makes by name. dex_class_parser.cpp:92 routes
/// VALUE_CHAR -- which the DEX specification defines as an unsigned 16-bit code
/// unit -- through the sign-extending path, so a one-byte 0x80 becomes -128
/// instead of 128, and every character above U+007F encoded in one byte comes
/// out negative.
constexpr std::uint64_t zeroExtendFrom(std::uint64_t v, unsigned bits) noexcept
{
	return v & lowMask(bits);
}

} // namespace byteorder
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_BYTE_ORDER_H
