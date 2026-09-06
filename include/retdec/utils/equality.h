/**
* @file include/retdec/utils/equality.h
* @brief Equality-related utilities.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_UTILS_EQUALITY_H
#define RETDEC_UTILS_EQUALITY_H

#include "retdec/utils/float_predicate.h"

namespace retdec {
namespace utils {

namespace {

/**
* @brief Checks if @a x is equal to @a y (differing only by @a epsilon).
*
* This function is meant to be used ONLY in floating-point specializations of
* areEqual<> below.
*
* One line now, because the rule is stated and proved once in
* retdec/utils/float_predicate.h and this is a call to it. What it used to be,
* and what each part of it did wrong:
*
*     if (std::isnan(x)) return std::isnan(y);
*     else if (std::isnan(y)) return false;
*     else if (std::isinf(x)) return std::isinf(x) == std::isinf(y);
*     else if (std::isinf(y)) return false;
*     return std::abs(x - y) <= epsilon * std::abs(x);
*
* - `std::isinf(x) == std::isinf(y)` compares two bools, so it is true whenever
*   both are infinite REGARDLESS OF SIGN: areEqual(+inf, -inf) returned true.
* - `x - y` is an overflow for x = DBL_MAX, y = -DBL_MAX. ESBMC reports it as
*   "arithmetic overflow on floating-point ieee_sub"; the result is an infinity
*   and the comparison against a finite tolerance then says "not equal", which
*   is the right answer reached by undefined means.
* - `epsilon * std::abs(x)` scales by |x| alone, so the predicate is not
*   symmetric -- the doc comment used to admit this as a known limitation. It
*   is now symmetric by construction, and proved so
*   (proof_nearly_equal_is_symmetric).
*
* The epsilons below are all in [0, 1], which is the range nearlyEqual accepts;
* a tolerance outside it is refused rather than applied.
*/
template<typename T>
inline bool areEqualFPWithEpsilon(const T &x, const T &y, const T &epsilon) {
	// nearlyEqualT rather than the nearlyEqual overloads, because this template
	// is also instantiated at long double. Everything it uses is generic --
	// std::numeric_limits<T> for the classification, T arithmetic for the
	// tolerance -- so the code is the same at every width; the PROOFS cover
	// double and float, which are the two widths ESBMC models exactly.
	return fpred::nearlyEqualT<T>(x, y, epsilon);
}

} // anonymous namespace

/// @name Equality of Values
/// @{

/**
* @brief Returns @c true if @a x is equal to @a y, @c false otherwise.
*
* @tparam T Type of @a x and @a y.
*
* By default, it returns <tt>x == y</tt>.
*/
template<typename T>
inline bool areEqual(const T &x, const T &y) {
	return x == y;
}

// Specialization for floats.
template<>
inline bool areEqual<float>(const float &x, const float &y) {
	return areEqualFPWithEpsilon(x, y, 1e-5f);
}

// Specialization for doubles.
template<>
inline bool areEqual<double>(const double &x, const double &y) {
	return areEqualFPWithEpsilon(x, y, 1e-10);
}

// Specialization for long doubles.
template<>
inline bool areEqual<long double>(const long double &x, const long double &y) {
	return areEqualFPWithEpsilon(x, y, 1e-15L);
}

/// @}

} // namespace utils
} // namespace retdec

#endif
