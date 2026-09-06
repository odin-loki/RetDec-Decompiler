/**
 * @file include/retdec/utils/alignment.h
 * @brief Declaration of aligning operations.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 *
 * These three are the tree's long-standing alignment helpers. They used to
 * carry the precondition "alignment must be a power of 2" and to return an
 * undefined value when it was not -- which is not a precondition a caller can
 * honour when the alignment came out of a PE header. They are now total: every
 * input has a defined answer, and the arithmetic is the proved kernel in
 * retdec/utils/align.h rather than a second copy of it here.
 *
 * New code should call retdec::utils::align directly, because it can say
 * whether it succeeded. These keep the single-return-value shape their existing
 * callers rely on, and answer a refusal with the input unchanged so that no
 * caller can be pushed backwards by one.
 */

#ifndef RETDEC_UTILS_ALIGNMENT_H
#define RETDEC_UTILS_ALIGNMENT_H

#include <cstdint>

namespace retdec {
namespace utils {

/// True when @p value is a multiple of @p alignment, with @p remainder set to
/// `value mod alignment`.
///
/// False when @p alignment is not a power of two -- zero included -- with
/// @p remainder set to @p value. There is no modulus to report in that case,
/// and @p value is non-zero exactly when the "aligned" answer would have been
/// wrong, so a caller reading only the remainder still concludes "not aligned".
bool isAligned(
		std::uint64_t value,
		std::uint64_t alignment,
		std::uint64_t& remainder);

/// @p value rounded down to a multiple of @p alignment.
///
/// Returns @p value unchanged when @p alignment is not a power of two. Never
/// larger than @p value. Rounding down cannot overflow, so that is the only
/// refusal.
std::uint64_t alignDown(std::uint64_t value, std::uint64_t alignment);

/// @p value rounded up to a multiple of @p alignment.
///
/// Returns @p value unchanged when @p alignment is not a power of two, and when
/// the rounded value would not fit in 64 bits. Never smaller than @p value --
/// which the old `alignDown(value + (alignment - 1), alignment)` was for any
/// value within `alignment - 1` of UINT64_MAX: it returned the true rounded
/// value minus 2^64, so at 0xFFFFFFFFFFFFFF50 with an alignment of 512 it
/// returned 0, short by 2^64 - 176 rather than by 2^64.
std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment);

} // namespace utils
} // namespace retdec

#endif
