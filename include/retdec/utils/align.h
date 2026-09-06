/**
 * @file include/retdec/utils/align.h
 * @brief Rounding a file-controlled value to a file-controlled alignment.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Alignment rounding appears in six places in this tree in three spellings, and
 * three of them are wrong. The rule is one line of arithmetic, so it is written
 * once here and proved once in tests/verification/align_proof.cpp.
 *
 * What goes wrong, in the order it was found:
 *
 *   - `alignDown(value + (alignment - 1), alignment)` -- src/utils/alignment.cpp:59,
 *     the tree's public helper -- forms the sum first. For a value within
 *     `alignment - 1` of UINT64_MAX the sum wraps and the result is SMALLER than
 *     the input. ESBMC returns value = 18446744073709551440, alignment = 512:
 *     the true rounded value 18446744073709551616 is not representable, the sum
 *     wrapped to 335, the mask took that to 256, and alignUp returned a number
 *     2^64 below its input. Confirmed by execution.
 *
 *     Its callers pass it a value they have already bounded --
 *     src/fileformat/types/resource_table/resource_table.cpp:783 rounds
 *     `origOffset + str.length`, where origOffset is inside a std::vector and
 *     str.length is a uint16 -- so the wrap is not reachable from there today.
 *     What makes it worth removing anyway is that nothing at the call site says
 *     so: the bound lives in the caller and the helper is public.
 *
 *   - The same helper accepts `alignment == 0`. `alignment - 1` is UINT64_MAX,
 *     the mask in alignDown is 0, and the answer is 0 for every input. A PE
 *     FileAlignment of 0 is a legal-looking field, and a cursor that was
 *     supposed to advance resets to the start of the buffer instead.
 *
 *   - `(nameLen + 4) & ~3u` -- src/cli_parser/pe_reader.cpp:284 -- rounds a
 *     std::size_t with a mask that is an `unsigned int`. `~3u` is 0xFFFFFFFC,
 *     which zero-extends to 0x00000000FFFFFFFC, so the mask clears bits 32-63
 *     as well as bits 0-1. Confirmed by execution: nameLen = 0x100000001 gives
 *     paddedNameLen = 4, and `streamHdrOff = nameOff + 4` walks the stream
 *     header table backwards.
 *
 *   - `isAligned(value, alignment, remainder)` -- src/utils/alignment.cpp:31 --
 *     masks with `alignment - 1` without asking whether that is a mask, and the
 *     answer is then wrong in both directions. ESBMC returns value = 128,
 *     alignment = 9223372036854775811: the mask is 9223372036854775810, the AND
 *     is 0, and it reports the value ALIGNED with a remainder of 0 -- while
 *     128 mod 9223372036854775811 is 128. With alignment = 0 the mask is
 *     UINT64_MAX instead and every non-zero value is reported unaligned.
 *     include/retdec/fileformat/file_format/pe/pe_format_parser.h:123 feeds it a
 *     raw PE FileAlignment and reports the answer as a header anomaly, so a
 *     file chooses whether its own anomaly is noticed.
 *
 *   - `number && !(number & (number - 1))` -- include/retdec/utils/math.h:22 --
 *     is a template with no constraint on N, so it instantiates at signed types
 *     too, and `number - 1` at INT64_MIN is signed overflow -- undefined
 *     behaviour, not a wrong answer. ESBMC reports it as an overflow on sub at
 *     n = -9223372036854775808 before any answer exists. The one alignment
 *     caller in the tree, src/fileformat/types/sec_seg/elf_section.cpp:42,
 *     instantiates it at `unsigned long long`, where the short-circuit on zero
 *     keeps it safe; the signed hazard is in the template, not at that site.
 *
 * Every function here is total: defined for every input, with no precondition a
 * caller can violate. The ones that can fail say so with a bool and leave their
 * out-parameter equal to the input, so a caller that ignores the return value
 * still never moves a cursor backwards -- which is the failure mode of every bug
 * above.
 *
 * This is deliberately NOT next to bounds::pageCount. pageCount rounds a
 * *count* -- how many pages hold n bytes -- and these round a *value*. Its
 * docstring records that conflating the two is exactly what mini_emu got wrong,
 * so the two live apart.
 *
 * Header-only, constexpr, no allocation, no exceptions, nothing included but
 * bounds.h and <cstddef>/<cstdint>, so ESBMC can reason about it directly. Do
 * not copy this arithmetic into a call site; call it.
 */

#ifndef RETDEC_UTILS_ALIGN_H
#define RETDEC_UTILS_ALIGN_H

#include "retdec/utils/bounds.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace align {

/// True when @p a is a power of two, and not zero.
///
/// Alignment arithmetic is undefined for anything else: `v & ~(a - 1)` with
/// a = 3 has the mask 0xFFFF...FD and clears bit 1 of the value while leaving
/// bit 0 alone, which is not rounding at all. Every entry point below asks this
/// first, because the alignment came out of a file.
constexpr bool isPowerOfTwo(std::uint64_t a) noexcept
{
	return a != 0 && (a & (a - 1)) == 0;
}

/// True when @p a is zero or a power of two.
///
/// Zero is what an ELF section header means by "no alignment constraint"
/// (sh_addralign 0 and 1 are equivalent per the gABI), so it is admissible
/// where a PE FileAlignment of 0 is not. The two questions are separate
/// functions rather than a flag so a call site cannot answer the wrong one.
constexpr bool isPowerOfTwoOrZero(std::uint64_t a) noexcept
{
	return a == 0 || isPowerOfTwo(a);
}

/// isPowerOfTwo for a value that arrived in a signed field.
///
/// The template at math.h:22 computes `number - 1` in the argument's own type.
/// At INT64_MIN that is signed overflow -- undefined behaviour, not a wrong
/// answer. Rejecting negatives before any arithmetic happens is total: a
/// negative alignment is not a power of two under any reading, so there is
/// nothing to compute, and -9223372036854775808 does not become a power of two
/// by being read as a bit pattern.
constexpr bool isPowerOfTwoSigned(std::int64_t n) noexcept
{
	return n > 0
		&& (static_cast<std::uint64_t>(n) & (static_cast<std::uint64_t>(n) - 1)) == 0;
}

/// isPowerOfTwoOrZero for a value that arrived in a signed field.
constexpr bool isPowerOfTwoOrZeroSigned(std::int64_t n) noexcept
{
	return n == 0 || isPowerOfTwoSigned(n);
}

/// Bytes of padding between @p v and the next multiple of @p a, or 0.
///
/// Total and always less than @p a, because it is computed from the remainder
/// and never forms the rounded value. This is the shape dex_header.cpp:542
/// writes by hand for the try_item pad (`if (triesSize > 0 && (insnsSize & 1))
/// read a padding u2`) -- a special case of padTo(pos, 4) that is correct only
/// because the DEX code_item header happens to be 4-aligned already.
///
/// A non-power-of-two @p a -- zero included -- has no next multiple worth
/// computing, and 0 is the answer that leaves a cursor where it is.
constexpr std::uint64_t padTo(std::uint64_t v, std::uint64_t a) noexcept
{
	if (!isPowerOfTwo(a)) return 0;
	// a is a power of two, so a - 1 is its low-bit mask and this is v mod a.
	const std::uint64_t rem = v & (a - 1);
	// rem is in [1, a-1] here, so the subtraction is in range and pad < a.
	return rem == 0 ? 0 : a - rem;
}

/// Round @p v up to the next multiple of @p a, or refuse and leave @p out at
/// @p v.
///
/// Refuses exactly when @p a is not a power of two, or when the rounded value
/// is not representable in 64 bits. On success out >= v, out - v < a, and
/// out is a multiple of a.
///
/// The sum is never formed before it is known to fit. `v + (a - 1)` -- the
/// spelling at alignment.cpp:59, at cil_lifter.cpp:326 and 408 as
/// `(sectStart + 3) & ~3ULL`, and at resource_table.cpp:783 through the helper
/// -- wraps and rounds *down*: v = 18446744073709551614 with a = 4 gives 0, run
/// and confirmed. Comparing the padding against the room left cannot wrap.
///
/// On refusal out is set to v rather than to 0. A caller that ignores the bool
/// then gets an unrounded cursor instead of a cursor reset to the start of the
/// buffer, which is the difference between a mis-parse and an out-of-bounds
/// read at every one of the sites above.
constexpr bool alignUp(std::uint64_t v, std::uint64_t a, std::uint64_t& out) noexcept
{
	out = v;
	if (!isPowerOfTwo(a)) return false;
	const std::uint64_t pad = padTo(v, a);
	// The subtraction is bounds::addFits stated at 64-bit width. addFits itself
	// takes std::size_t, and casting a uint64 through size_t would be a silent
	// truncation on a 32-bit host -- the exact shape of the pe_reader.cpp:284
	// bug -- so the check is written out rather than narrowed to reuse it.
	if (pad > UINT64_MAX - v) return false;
	out = v + pad;
	return true;
}

/// Round @p v down to a multiple of @p a, or refuse and leave @p out at @p v.
///
/// Refuses only for a non-power-of-two @p a; rounding down cannot overflow, so
/// there is no second failure mode. On success out <= v, v - out < a, out is a
/// multiple of a, and out == v - (v mod a).
constexpr bool alignDown(std::uint64_t v, std::uint64_t a, std::uint64_t& out) noexcept
{
	out = v;
	if (!isPowerOfTwo(a)) return false;
	// a - 1 is already the low-bit mask, so its complement is the high-bit mask
	// exactly. leb128::maskFrom builds the same mask from a shift count, and
	// using it here would mean deriving log2(a) first -- more arithmetic, and
	// one more place to be wrong, for an identical result.
	out = v & ~(a - 1);
	return true;
}

/// True when @p v is a multiple of @p a, with @p remainder set to v mod a.
///
/// Refuses -- returns false, with remainder set to v -- when @p a is not a
/// power of two. isAligned at alignment.cpp:31 answers anyway: with a = 0 the
/// mask is UINT64_MAX and remainder becomes v, so it reports every non-zero
/// value unaligned without ever saying that the alignment itself was the
/// problem. pe_format_parser.h:123 turns that into a header-anomaly verdict on
/// a file that has a different anomaly.
///
/// remainder is set to v on refusal for the same reason alignUp leaves out at
/// v: it is non-zero whenever v is, so a caller reading only the remainder
/// still concludes "not aligned" rather than "aligned".
constexpr bool isAlignedTo(std::uint64_t v, std::uint64_t a, std::uint64_t& remainder) noexcept
{
	remainder = v;
	if (!isPowerOfTwo(a)) return false;
	remainder = v & (a - 1);
	return remainder == 0;
}

/// @p n rounded up to a multiple of @p a, saturating at SIZE_MAX rather than
/// wrapping.
///
/// For the callers that have nowhere to put a failure -- a cursor update inside
/// a walk, pe_reader.cpp:284 and cil_lifter.cpp:326/408 -- the answer must be a
/// single value that is never less than the input and never decreases as the
/// input grows. That is exactly the property `(nameLen + 4) & ~3u` does not
/// have: 0x100000001 maps to 4 while 0x10 maps to 0x14, so a larger name gives
/// a smaller cursor and pe_reader.cpp:286 walks the stream-header table
/// backwards.
///
/// The saturation point is SIZE_MAX, and it is not the obvious choice. The
/// obvious choice is the largest a-aligned std::size_t, `SIZE_MAX & ~(a - 1)`,
/// so that the result is aligned unconditionally -- and ESBMC refuted it:
/// n = 18446744073709551615, a = 4 gives 18446744073709551612, which is three
/// bytes BELOW the input. That is the same class of backwards step this whole
/// header exists to stop, arrived at from the other direction, so the two
/// properties cannot both hold at the top of the range and "never backwards"
/// is the one that is load-bearing. The result is therefore aligned everywhere
/// except at the saturation point, where it is SIZE_MAX -- a value no bounds
/// check passes, so a walk that reaches it stops rather than continuing on a
/// plausible-looking wrong offset.
///
/// Even saturated the answer is within one alignment of the input: saturation
/// happens only when the padding exceeds the room left, so `SIZE_MAX - n` is
/// smaller than the padding, which is smaller than @p a.
///
/// A non-power-of-two @p a returns @p n unchanged, which is still monotone and
/// still never backwards.
constexpr std::size_t alignUpSaturating(std::size_t n, std::size_t a) noexcept
{
	if (!isPowerOfTwo(a)) return n;
	const std::size_t rem = n & (a - 1);
	if (rem == 0) return n;
	const std::size_t pad = a - rem;
	// bounds::addFits is the same subtraction, at the same width, already
	// proved -- both operands are genuinely std::size_t here, so there is no
	// cast to lose anything through.
	if (!bounds::addFits(n, pad)) return SIZE_MAX;
	return n + pad;
}

} // namespace align
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_ALIGN_H
