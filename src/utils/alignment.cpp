/**
 * @file src/utils/alignment.cpp
 * @brief Definition of aligning operations.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 *
 * The arithmetic these three functions used to spell out by hand now lives in
 * include/retdec/utils/align.h, which is proved over the whole 64-bit domain in
 * tests/verification/align_proof.cpp. This file is the old signature kept for
 * its callers; the body is a call, not a second copy of the rule.
 */

#include "retdec/utils/align.h"
#include "retdec/utils/alignment.h"

namespace retdec {
namespace utils {

/**
 * Checks whether given value is aligned based on alignment value.
 *
 * @param value Value to be checked.
 * @param alignment Alignment to check.
 * @param remainder Output value that is zero if @c value is aligned and
 *   @c value modulo @c alignment otherwise. When @c alignment is not a power
 *   of two -- zero included -- there is no such modulus to report and the
 *   remainder is set to @c value, which is non-zero whenever @c value is, so a
 *   caller that reads only the remainder still concludes "not aligned".
 *
 * @return True if value is aligned to given alignment. False when it is not,
 *   and false when @c alignment is not a power of two.
 *
 * The body used to be `(remainder = (value & (alignment - 1))) == 0`, which
 * masks with `alignment - 1` without asking whether that is a mask, and is then
 * wrong in both directions. ESBMC returned value = 128,
 * alignment = 9223372036854775811 (0x8000000000000003): the mask is
 * 0x8000000000000002, 128 AND that is 0, so it answered ALIGNED with a
 * remainder of 0 -- while 128 mod 9223372036854775811 is 128. Run and
 * confirmed. With alignment = 0 the mask is UINT64_MAX instead and every
 * non-zero value came back unaligned, without ever saying that the alignment
 * was the problem.
 *
 * That false-positive direction is the dangerous one:
 * include/retdec/fileformat/file_format/pe/pe_format_parser.h:123 feeds this a
 * raw PE FileAlignment -- a file-controlled field -- and turns the answer into
 * a header-anomaly verdict, so a malformed file got to choose whether its own
 * anomaly was noticed. align::isAlignedTo refuses the alignment instead of
 * answering about it, so a FileAlignment of 0 or 3 now reads as "not a multiple
 * of the file alignment" rather than as a clean header.
 */
bool isAligned(
		std::uint64_t value,
		std::uint64_t alignment,
		std::uint64_t& remainder)
{
	return align::isAlignedTo(value, alignment, remainder);
}

/**
 * Aligns given value down by specified alignment.
 *
 * @param value Value to align.
 * @param alignment Alignment to use.
 *
 * @return Value aligned down, or @c value unchanged when @c alignment is not a
 *   power of two.
 *
 * The body used to be `value & ~(alignment - 1)` unconditionally. At
 * alignment = 0 that is `value & ~UINT64_MAX`, i.e. `value & 0`, so every input
 * collapsed to 0 -- a cursor that was meant to be rounded reset to the start of
 * the buffer instead. align::alignDown leaves the value where it is instead,
 * which is the answer that cannot move a cursor backwards.
 */
std::uint64_t alignDown(std::uint64_t value, std::uint64_t alignment)
{
	// alignDown sets out to value before it inspects the alignment, so the
	// refusal path needs no separate handling here.
	std::uint64_t out;
	align::alignDown(value, alignment, out);
	return out;
}

/**
 * Aligns given value up by specified alignment.
 *
 * @param value Value to align.
 * @param alignment Alignment to use.
 *
 * @return Value aligned up. When @c alignment is not a power of two, or when
 *   the rounded value is not representable in 64 bits, @c value is returned
 *   unchanged. The result is never smaller than @c value.
 *
 * The body used to be `alignDown(value + (alignment - 1), alignment)`, which
 * forms the sum before knowing it fits. ESBMC returned
 * value = 18446744073709551440 (0xFFFFFFFFFFFFFF50), alignment = 512: the true
 * rounded value 18446744073709551616 is not representable, `value + 511`
 * wrapped to 335, the mask ~511 took that to 0, and alignUp returned 0 for an
 * input of 18446744073709551440. Run and confirmed;
 * alignUp(18446744073709551614, 4) came back 0 the same way.
 *
 * alignment = 0 was the second way in: `alignment - 1` is UINT64_MAX, the mask
 * in alignDown is 0, and the answer was 0 for every input.
 * src/unpackertool/plugins/upx/pe/pe_upx_stub.cpp:476 passes
 * getSectionAlignment() -- a PE header field a file can simply set to 0 --
 * straight in, and used it as the size of the section it was about to write.
 *
 * align::alignUp compares the padding against the room left instead of forming
 * the sum, so neither case can produce a result below the input.
 */
std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment)
{
	// alignUp sets out to value before it inspects the alignment or the room
	// left, so both refusal paths already leave out == value.
	std::uint64_t out;
	align::alignUp(value, alignment, out);
	return out;
}

} // namespace utils
} // namespace retdec
