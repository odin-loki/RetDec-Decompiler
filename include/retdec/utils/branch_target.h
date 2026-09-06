/**
 * @file include/retdec/utils/branch_target.h
 * @brief Resolving a branch displacement to an offset inside the code.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Every bytecode lifter in this tree asks the same question: an instruction at
 * some position carries a signed displacement read out of the file, so where
 * does it branch to, and is that inside the method at all? Three of them answer
 * it three different ways, and two of the three are wrong.
 *
 *   src/cli_parser/cil_lifter.cpp:519,533  widens to int64 -- so the addition
 *       itself is right -- and then truncates the result back to uint32 with no
 *       range check. `pos = 2`, `delta = -128` gives the int64 sum -126, which
 *       truncates to 0xFFFFFF82, and addBlock() records that as a basic-block
 *       leader. buildCFG then creates a block at 0xFFFFFF82 that no instruction
 *       falls into and wires real predecessors' successor edges to it.
 *
 *   src/jvm_parser/jvm_lifter.cpp:275,281,293,297,310,317,324,331  writes
 *       `static_cast<uint32_t>(pc + off)` with `pc` a size_t and `off` an
 *       int32_t. The int32_t converts to uint64 first, so `pc = 0, off = -1`
 *       is 0xFFFFFFFFFFFFFFFF, and the narrowing cast makes the leader
 *       0xFFFFFFFF. There is no range check on any of the eight.
 *
 *   src/jvm_parser/jvm_lifter.cpp:495  is worse than wrong, it is undefined:
 *       `static_cast<int32_t>(instrPc) + offset` adds two int32_t values, both
 *       derived from the file. A JVM `code_length` is a u4, so instrPc really
 *       can exceed INT32_MAX, and `goto_w` really does carry a full int32
 *       displacement.
 *
 *   src/dex_parser/dex_lifter.cpp:222,251  does it correctly: form the sum in
 *       int64, then `target >= 0 && (uint64)target < total` before use. So does
 *       src/pyc_parser/pyc_reader.cpp:612. This header is that rule, written
 *       once, made total, and proved.
 *
 * The contract every function here shares:
 *
 *   - the out parameter is written ONLY on success, so a caller that ignores
 *     the return value cannot silently record an out-of-range leader. That is
 *     precisely the failure mode above: it is not that the arithmetic is
 *     unchecked, it is that the unchecked answer reaches the CFG;
 *   - on success the answer is inside [0, codeSize), which is what makes it
 *     safe to use as an index or a map key;
 *   - total: defined for every input, including a `base` past 2^63 and a
 *     `delta` of INT64_MIN, with no precondition a caller can violate.
 *
 * Proved in tests/verification/branch_target_proof.cpp.
 */

#ifndef RETDEC_UTILS_BRANCH_TARGET_H
#define RETDEC_UTILS_BRANCH_TARGET_H

#include "retdec/utils/bounds.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace btgt {

// bounds.h is written in std::size_t and these offsets are uint64_t, because a
// lifter's positions must not be done in the width the file chose. The casts
// between them below are exact only where the two types are the same width. On
// a 32-bit host they would silently truncate a 64-bit offset down into the
// checker, which is the class of bug this header exists to remove, so say so at
// compile time rather than discover it in a cross build.
static_assert(sizeof(std::size_t) == sizeof(std::uint64_t),
              "branch_target.h narrows uint64_t offsets into bounds.h's size_t");

/// Resolve a signed displacement @p delta from @p base into @p target.
///
/// Succeeds exactly when the mathematical sum `base + delta` -- not the sum in
/// any particular width -- lands in [0, @p codeSize). @p target is written only
/// then, and holds that sum exactly: no truncation to 32 bits, which is what
/// cil_lifter.cpp:523 does after getting the addition right.
///
/// The sum is never formed in int64 either, because that would be a precondition
/// on @p base: `(int64)base + delta` overflows for base >= 2^63, and a function
/// whose contract only holds below some bound is a function every caller has to
/// re-check. Splitting on the sign of @p delta makes both halves exact over the
/// whole 64-bit domain.
constexpr bool relative(std::uint64_t base,
                        std::int64_t delta,
                        std::uint64_t codeSize,
                        std::uint64_t& target) noexcept
{
	if (delta >= 0) {
		const std::uint64_t forward = static_cast<std::uint64_t>(delta);
		// Representability first: the sum is not formed until it is known to
		// exist, the same discipline bounds::rangeFits keeps.
		if (!bounds::addFits(static_cast<std::size_t>(base),
		                     static_cast<std::size_t>(forward)))
			return false;
		const std::uint64_t t = base + forward;
		if (t >= codeSize) return false;
		target = t;
		return true;
	}

	// The magnitude of a negative delta. Written as -(delta + 1) + 1 because
	// -delta is undefined for INT64_MIN -- the one input a plain negation gets
	// wrong, and the one a fuzzer reaches immediately since a displacement is a
	// file-controlled signed field. Here delta + 1 is in [INT64_MIN+1, 0], its
	// negation is in [0, INT64_MAX], and the +1 happens in uint64 where 2^63 is
	// representable.
	const std::uint64_t back = static_cast<std::uint64_t>(-(delta + 1)) + 1;
	// A branch to before the start of the code. jvm_lifter.cpp:275 has no such
	// test: pc = 0 with def = -1 wraps to 0xFFFFFFFFFFFFFFFF there.
	if (back > base) return false;
	const std::uint64_t t = base - back;
	if (t >= codeSize) return false;
	target = t;
	return true;
}

/// Accept an already-absolute target @p t as an offset into the code.
///
/// The trivial half of the same rule, kept as a function so a lifter with both
/// forms of branch -- Python before 3.11 has absolute jumps and after it has
/// relative ones, in the same switch at pyc_reader.cpp:535-543 -- gets the same
/// verdict and the same write-only-on-success discipline from both.
constexpr bool absolute(std::uint64_t t,
                        std::uint64_t codeSize,
                        std::uint64_t& target) noexcept
{
	if (t >= codeSize) return false;
	target = t;
	return true;
}

/// End of the half-open region [@p start, @p start + @p len) inside the code.
///
/// For an exception-handler try block or handler extent: a start and a length,
/// both read out of the file, that a consumer will subtract. Succeeds exactly
/// when the region fits, and then @p start <= @p end <= @p codeSize.
///
/// cil_lifter.cpp:779 writes `clause.tryOffset + clause.tryLength` with both
/// operands uint32 and the destination uint32. tryOffset = 0xFFFFFFFF with
/// tryLength = 2 gives endOffset = 1: a protected region that ends before it
/// begins, whose consumers compute `end - start` as 0xFFFFFFFE bytes.
constexpr bool region(std::uint64_t start,
                      std::uint64_t len,
                      std::uint64_t codeSize,
                      std::uint64_t& end) noexcept
{
	if (!bounds::rangeFits(static_cast<std::size_t>(start),
	                       static_cast<std::size_t>(codeSize),
	                       static_cast<std::size_t>(len)))
		return false;
	// Only now is the sum formed, and rangeFits has already established that it
	// neither wraps nor exceeds codeSize.
	end = start + len;
	return true;
}

/// End of an inline table of @p n entries of @p width bytes starting at @p pos.
///
/// A switch instruction's case labels follow the opcode, and the instruction --
/// and therefore the base every case target is measured from -- ends after
/// them. Both @p n and @p width come from the file: CIL InlineSwitch reads a
/// raw uint32 count at cil_lifter.cpp:540, and a JVM tableswitch derives its
/// count from `hi - lo + 1` at jvm_lifter.cpp:277, which is 2^32 for the
/// (lo, hi) pair (INT32_MIN, INT32_MAX).
///
/// So the product is checked for representability before it is formed, and the
/// span is then checked to fit the code. cil_lifter.cpp:549 forms `n * 4` in
/// 64 bits correctly and then truncates `afterSwitch` back to uint32, which
/// puts the base for every one of that switch's case targets outside the method.
constexpr bool tableEnd(std::uint64_t pos,
                        std::uint64_t n,
                        std::uint64_t width,
                        std::uint64_t codeSize,
                        std::uint64_t& end) noexcept
{
	if (!bounds::mulFits(static_cast<std::size_t>(n), static_cast<std::size_t>(width)))
		return false;
	const std::uint64_t span = n * width;
	if (!bounds::rangeFits(static_cast<std::size_t>(pos),
	                       static_cast<std::size_t>(codeSize),
	                       static_cast<std::size_t>(span)))
		return false;
	end = pos + span;
	return true;
}

} // namespace btgt
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_BRANCH_TARGET_H
