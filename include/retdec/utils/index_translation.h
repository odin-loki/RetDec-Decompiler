/**
 * @file include/retdec/utils/index_translation.h
 * @brief Turning a number a file supplied into a subscript.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * A file hands over an index or a packed token and it is about to become a
 * subscript. Three surveys of this tree named the same primitive three
 * different ways -- "index translation", "coded token split", "row addressing"
 * -- and it is one question with two halves.
 *
 * ## The 1-based half
 *
 * .NET metadata, and every table addressed the way ECMA-335 addresses one,
 * numbers rows from 1. Row 0 is the null token. So every read is
 * `container[n - 1]`, and `n` came out of the file:
 *
 *   - `n == 0` makes `n - 1` be SIZE_MAX. `include/retdec/utils/container.h:76`
 *     and `:91` are guarded against that only by an `assert`, which is compiled
 *     out under NDEBUG, and `container[SIZE_MAX]` is what is left.
 *   - `(n - 1) * width` done in the width the file chose wraps.
 *     `src/cli_parser/cli_heaps.cpp:128` computes `(index - 1) * 16` entirely
 *     in uint32. ESBMC returns index = 268435457 (0x10000001): the true offset
 *     is 4294967296, it wraps to 0, `off + 16 > data_.size()` is satisfied, and
 *     GuidHeap::get hands back the GUID at index 1 -- a module's MVID -- for an
 *     index that addresses nothing. It reports success, so no caller can tell.
 *
 * `bounds::arrayFits` already answers whether row i is readable. It does not
 * produce the offset, and it knows nothing about a 1-based domain, which is
 * exactly where the wrong answer above lives. @ref slotFor1Based and
 * @ref rowAt1Based are that missing step, and they are the only two spellings
 * of it, so `idx >= size()` (which lets the last row through) and
 * `idx > size() - 1` (which underflows on an empty table) cannot both be
 * written down again.
 *
 * ## The tag/payload half
 *
 * A coded token packs a small table selector into the low bits of an index
 * (ECMA-335 II.24.2.6). A DEX `encoded_value` header packs the same way -- the
 * value_type in the low 5 bits, the value_arg above it -- which is why
 * `src/dex_parser/dex_class_parser.cpp:434` and `:439` read the one byte twice,
 * as `(va & 0x1F)` and `(ev >> 5) & 0x7`; that is @ref splitTag at tagBits 5,
 * once. A metadata token packs the other way round, the table id in the top 8
 * bits of a uint32, which is @ref splitHigh. All of them go wrong the same two
 * ways: a shift count that reaches the width of the type,
 * which is undefined; and a mask that does not match the shift, which silently
 * drops an index bit and aliases two rows onto one. @ref splitTag and
 * @ref splitHigh state the split once, and state it losslessly -- the two
 * halves put back together are the original word, with nothing dropped in
 * between -- so a transposed mask (0xFFFFFF against 0xFFFFFFF) is a failed
 * proof rather than a wrong name on a hostile assembly.
 *
 * @ref tagIndexes is then the only path from a tag to a lookup-table subscript.
 * `src/cli_parser/cli_tables.cpp:858-928` has eleven decoders that each pair a
 * mask width with a table length by inspection; seven check the pairing and
 * four do not.
 *
 * Header-only, constexpr, allocation-free and exception-free, over raw pointers
 * and sizes, so tests/verification/index_translation_proof.cpp can reason about
 * the code that actually runs. Do not re-derive any of this at a call site;
 * call it.
 */

#ifndef RETDEC_UTILS_INDEX_TRANSLATION_H
#define RETDEC_UTILS_INDEX_TRANSLATION_H

#include "retdec/utils/bounds.h"
#include "retdec/utils/leb128.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace idxmap {

/// SIZE_MAX as a 64-bit value.
///
/// Indices arrive as uint64 (or as a uint32 widened to one) because the whole
/// point is that the arithmetic is not done in the width the file chose. This
/// is the boundary where such a value becomes a subscript.
inline constexpr std::uint64_t kMaxOffset = static_cast<std::uint64_t>(SIZE_MAX);

// ─── 1-based indices ─────────────────────────────────────────────────────────

/// Translate a 1-based @p idx into a 0-based @p slot in a container of
/// @p count elements.
///
/// Succeeds exactly when `1 <= idx <= count`; on success `slot == idx - 1` and
/// `slot < count`. Both boundary cases are the ones that go wrong in this tree:
/// `idx == 0` is the null token and must be refused rather than wrapped to
/// SIZE_MAX, and `count == 0` refuses every index, which is what an empty or
/// never-parsed table needs.
///
/// @p slot is left untouched on failure, so a caller that ignores the result
/// gets whatever it had rather than a value derived from a bad index.
constexpr bool slotFor1Based(std::uint64_t idx, std::size_t count, std::size_t& slot) noexcept
{
	// Written as two refusals rather than as `idx - 1 < count`, which is the
	// same predicate only because the subtraction wraps -- correct by accident,
	// and it does not survive being copied to a place where the types differ.
	if (idx == 0) return false;
	if (idx > static_cast<std::uint64_t>(count)) return false;
	slot = static_cast<std::size_t>(idx - 1);
	return true;
}

// ─── element offsets ─────────────────────────────────────────────────────────

/// Offset of element @p i of @p width bytes, in a buffer of @p size bytes whose
/// elements start at @p base.
///
/// Succeeds exactly when the whole element lies inside the buffer:
/// `mulFits(i, width) && addFits(i * width, width) &&
///  rangeFits(base, size, i * width + width)`. On success
/// `off == base + i * width` and `off + width <= size`.
///
/// The `addFits` conjunct is not decoration. Without it the predicate is
/// satisfied by i = SIZE_MAX, width = 1, base = 0, size = 0: the product is
/// representable, `i * width + width` wraps to 0, and `rangeFits(0, 0, 0)` is
/// true -- so the unguarded spelling reports that an element at offset
/// 18446744073709551615 fits in an empty buffer. Implemented below as
/// `bounds::arrayFits(base, size, i + 1, width)` -- the same predicate with the
/// wrap ruled out -- and the sum is never formed until it is known to be inside
/// the buffer.
///
/// @p width of 0 succeeds whenever `base <= size`, with `off == base`. A
/// zero-width element occupies nothing, so there is nothing to bound; callers
/// for which a zero width means "this table was never parsed" want
/// @ref rowAt1Based, which refuses it.
constexpr bool elementAt(
		std::size_t base,
		std::size_t size,
		std::uint64_t i,
		std::size_t width,
		std::size_t& off) noexcept
{
	if (width == 0) {
		// No multiplication at all: an element of no bytes is readable wherever
		// base is a position in the buffer, however large i is.
		if (!bounds::rangeFits(base, size, 0)) return false;
		off = base;
		return true;
	}

	// The element count the read needs is i + 1, and i is file-controlled, so
	// the increment is checked before it is made. With width >= 1 an i at or
	// above SIZE_MAX would put the element at or past SIZE_MAX and still need
	// width more bytes, which no buffer can supply -- so refusing here loses
	// nothing the bound below would have allowed.
	if (i >= kMaxOffset) return false;

	const std::size_t count = static_cast<std::size_t>(i) + 1;

	// This is bounds::arrayFits(base, size, count, width) with the operands of
	// the overflow check written the other way round. mulFits divides by its
	// FIRST argument, and width is the operand a caller knows statically while
	// count comes out of the file; passing width first keeps the divisor
	// concrete. Multiplication is commutative, so the predicate is identical --
	// but a symbolic 64-bit divide alongside a symbolic 64-bit multiply is
	// nonlinear bitvector arithmetic that no solver here discharges, and
	// docs/VERIFICATION.md already records that limit. This ordering is what
	// makes the per-width proofs in tests/verification/ possible at all.
	if (!bounds::mulFits(width, count)) return false;
	if (!bounds::rangeFits(base, size, count * width)) return false;

	// Both products are representable and the larger of them is inside the
	// buffer, so this sum cannot wrap.
	off = base + static_cast<std::size_t>(i) * width;
	return true;
}

/// Offset of 1-based row @p idx of @p width bytes in a table of @p size bytes.
///
/// The composition of the two halves above, and the direct replacement for
/// `off = (index - 1) * width; if (off + width > size) fail;`.
///
/// Refuses `width == 0`, so a table that was never parsed -- rowSize is 0 until
/// parseTable sets it -- cannot alias one that was; and refuses `idx == 0`, so
/// a 1-based off-by-one cannot address the row before the buffer.
constexpr bool rowAt1Based(
		std::size_t size,
		std::uint64_t idx,
		std::size_t width,
		std::size_t& off) noexcept
{
	if (width == 0) return false;
	if (idx == 0) return false;
	// idx >= 1, so idx - 1 cannot wrap; and the multiplication that follows is
	// done at 64 bits by elementAt rather than in the file's 32.
	return elementAt(0, size, idx - 1, width, off);
}

// ─── tag / payload splits ────────────────────────────────────────────────────

/// Split a coded token into its low @p tagBits tag and its remaining payload.
///
/// This is the ECMA-335 II.24.2.6 coded index: the low bits select which table
/// the index refers to, the rest is the 1-based row number in that table.
///
/// Lossless by construction: `coded == (payload << tagBits) | tag` for every
/// uint32 and every tagBits below 32, and `tag < 2^tagBits`. Losing an index
/// bit here does not fail, it silently addresses a different row.
///
/// Refuses `tagBits >= 32`, which is the shift that is undefined rather than
/// merely wrong. `tagBits == 0` is accepted and means the whole word is the
/// payload.
constexpr bool splitTag(
		std::uint32_t coded,
		unsigned tagBits,
		std::uint32_t& tag,
		std::uint32_t& payload) noexcept
{
	if (tagBits >= 32) return false;

	// leb128::maskFrom(n) is every bit at or above n, built without a lossy
	// shift; its complement is the low-n mask this needs. Writing
	// `(1u << tagBits) - 1` here would be a second place for the mask and the
	// shift to drift apart, which is the bug.
	const std::uint32_t mask = static_cast<std::uint32_t>(~leb128::maskFrom(tagBits));

	tag     = coded & mask;
	payload = coded >> tagBits;
	return true;
}

/// Split a word into its top @p highBits and the bits below them.
///
/// At `highBits == 8` this is the metadata token split: the table id is the top
/// byte and the 1-based row index is the low 24 bits, so
/// `tok == (high << 24) | low` with `low < 2^24`. That identity is what refutes
/// a transposed mask -- 0xFFFFFF against 0xFFFFFFF -- which is invisible on a
/// well-formed assembly and misdirects every token on a hostile one.
///
/// `highBits` may be 0 (no high field) or 32 (no low field); both are defined
/// here and neither performs a 32-bit shift by 32. Refuses `highBits > 32`.
constexpr bool splitHigh(
		std::uint32_t tok,
		unsigned highBits,
		std::uint32_t& high,
		std::uint32_t& low) noexcept
{
	if (highBits > 32) return false;

	const unsigned lowBits = 32 - highBits;
	low = static_cast<std::uint32_t>(tok & ~leb128::maskFrom(lowBits));
	// `tok >> 32` is undefined. lowBits reaches 32 only when highBits is 0, and
	// then there is no high field to extract.
	high = lowBits >= 32 ? 0u : (tok >> lowBits);
	return true;
}

/// True when @p tag is a subscript into a table of @p tableLen entries.
///
/// The only path from a tag to a lookup-table read. It exists so the pairing of
/// a mask width with a table length is checked rather than inspected: a 3-bit
/// tag admits 8 values and `kCustomAttrType` has 5 entries, and nothing but
/// this call stands between the two.
constexpr bool tagIndexes(std::uint32_t tag, std::size_t tableLen) noexcept
{
	return static_cast<std::uint64_t>(tag) < static_cast<std::uint64_t>(tableLen);
}

/// Largest row count a 2-byte coded index with @p tagBits tag bits can address,
/// exclusive: 2^(16 - tagBits).
///
/// ECMA-335 II.24.2.6 gives a coded index 16 bits, of which tagBits are the
/// tag; a referenced table with that many rows or more no longer fits, and the
/// index becomes 4 bytes. Computed at 64 bits so the shift is defined for every
/// unsigned tagBits -- `1u << (16 - tagBits)` with tagBits above 16 shifts by a
/// wrapped unsigned, which is undefined, and tagBits is a caller constant that
/// a new coded-token kind could get wrong.
constexpr std::uint64_t wideThreshold(unsigned tagBits) noexcept
{
	const unsigned indexBits = tagBits >= 16 ? 0 : 16 - tagBits;
	return std::uint64_t{1} << indexBits;
}

/// True when a coded index over the tables whose row counts are @p rowCounts
/// must be stored in 4 bytes rather than 2.
///
/// @p rowCounts holds the counts of the referenced tables, in the order the
/// tag numbers them; @p n is how many there are.
///
/// Getting this wrong for one coded-token kind changes the width of every row
/// that contains one, so every table after it in the stream is decoded at the
/// wrong offset -- there is no length prefix to resynchronise against. The
/// comment at `src/cli_parser/cli_tables.cpp:465-478` describes exactly that
/// failure without anything checking for it.
///
/// Monotone in the counts: a table that grows never makes the token narrower.
constexpr bool codedTokenIsWide(
		const std::uint32_t* rowCounts,
		std::size_t n,
		unsigned tagBits) noexcept
{
	if (rowCounts == nullptr) return false;

	const std::uint64_t threshold = wideThreshold(tagBits);
	for (std::size_t i = 0; i < n; ++i) {
		// Widened before the comparison: at tagBits == 0 the threshold is
		// 65536, which is representable in 32 bits, but nothing here depends on
		// that staying true.
		if (static_cast<std::uint64_t>(rowCounts[i]) >= threshold) return true;
	}
	return false;
}

} // namespace idxmap
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_INDEX_TRANSLATION_H
