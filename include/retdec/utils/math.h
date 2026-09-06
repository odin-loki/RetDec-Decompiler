/**
* @file include/retdec/utils/math.h
* @brief Mathematical utilities.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_UTILS_MATH_H
#define RETDEC_UTILS_MATH_H

#include <cstdint>
#include <type_traits>

#include "retdec/utils/align.h"

namespace retdec {
namespace utils {

/**
* @brief Check if @a number is power of two.
*
* @param[in] number Value which will be checked.
*
* @tparam N Type of @a number.
*/
template<typename N>
bool isPowerOfTwo(N number) {
	static_assert(std::is_integral<N>::value,
		"isPowerOfTwo is defined for integral types only; a power of two is a "
		"property of an integer bit pattern and of nothing else");

	// The body used to be `number && !(number & (number - 1))`, which computes
	// `number - 1` in N's own type. The template constrains N to nothing, so it
	// instantiates at signed types, and at INT64_MIN the subtraction is signed
	// integer overflow: ESBMC reports "arithmetic overflow on sub" with the
	// witness n = -9223372036854775807 - 1 (0x8000000000000000) and marks the
	// rest of the expression NOT CHECKED, because the undefined behaviour is
	// reached before any answer is computed. That is not a wrong answer, it is
	// no answer -- the compiler may assume it never happens.
	//
	// align::isPowerOfTwoSigned refuses every n <= 0 before any arithmetic
	// happens (a negative alignment is not a power of two under any reading),
	// and align::isPowerOfTwo does the unsigned case. Both are proved over the
	// whole 64-bit domain against an independent bit count in
	// tests/verification/align_proof.cpp, so this does not re-derive the test.
	if constexpr (std::is_signed<N>::value) {
		return align::isPowerOfTwoSigned(static_cast<std::int64_t>(number));
	} else {
		return align::isPowerOfTwo(static_cast<std::uint64_t>(number));
	}
}

/**
* @brief Check if @a number is power of two or zero.
*
* @param[in] number Value which will be checked.
*
* @tparam N Type of @a number.
*/
template<typename N>
bool isPowerOfTwoOrZero(N number) {
	return !number || isPowerOfTwo(number);
}

unsigned countBits(unsigned long long n);
unsigned bitSizeOfNumber(unsigned long long v);

} // namespace utils
} // namespace retdec

#endif
