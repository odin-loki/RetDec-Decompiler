/**
 * @file include/retdec/utils/float_predicate.h
 * @brief Decisions taken on floating-point values that came out of a binary.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Four decisions in this tree are taken on doubles derived from an input file,
 * and in three of them a NaN or an out-of-range operand silently answers "no"
 * instead of being refused:
 *
 *   include/retdec/utils/equality.h:45   areEqualFPWithEpsilon
 *   src/utils/string.cpp:1019            isNiceString
 *   src/utils/string.cpp:1046            isNiceAsciiWideString
 *   src/utils/gpu_scanner_cpu.cpp        GpuScanner::fileEntropy
 *
 * A false "no" is not harmless here. isNiceString decides whether a run of
 * bytes in a data section becomes a string literal in the output; fileEntropy
 * drives packer detection; areEqualFPWithEpsilon decides whether two recovered
 * constants are the same value. Each of them answers a question the operator
 * will act on, and each of them currently has an input for which the answer is
 * an artefact of IEEE-754 rather than a measurement.
 *
 * The concrete faults these replace, each one an ESBMC refutation of the
 * expression as written and not a reading of it:
 *
 *   - equality.h's infinity arm is `std::isinf(x) == std::isinf(y)`. In C++
 *     std::isinf returns bool, so that is `true == true` for a positive and a
 *     negative infinity, and areEqual(+inf, -inf) is TRUE. Two constants as
 *     far apart as doubles get compare equal. Refuted directly, with
 *     px = +INFINITY, py = -INFINITY.
 *   - equality.h forms `x - y` with nothing bounding either operand, so for
 *     x = DBL_MAX, y = -DBL_MAX it is an "arithmetic overflow on
 *     floating-point ieee_sub". At the epsilons the tree instantiates it with
 *     the resulting infinity still compares false, so the answer survives; the
 *     overflow does not, and nothing keeps a future epsilon from making
 *     `epsilon * |x|` infinite too and turning `inf <= inf` into "equal".
 *   - string.cpp compares an integer count against `s.size() * minRatio` with
 *     minRatio range-checked only by an `assert` that disappears under NDEBUG.
 *     A negative ratio makes every comparison true: ESBMC returns size = 48,
 *     minRatio = -1.0, niceCharCount = 0, so a 48-byte run without one
 *     printable character is emitted as a string literal. A NaN makes every
 *     comparison false: size = 1, count = 1, and a run in which every byte is
 *     printable is not nice. Monotonicity in the ratio fails on the same NaN.
 *   - GpuScanner::fileEntropy computed `sz = hi - lo + 1` before anything
 *     establishes lo <= hi. On a ten-byte file with startOffset = 10 and
 *     stopOffset = SIZE_MAX, hi is 9 and lo is 10: the subtraction wraps
 *     ("arithmetic overflow on sub"), sz comes out 0, the histogram loop runs
 *     zero times, and the function returns 0.0 -- the answer a perfectly
 *     uniform region also gives, with nothing in the return type to tell them
 *     apart.
 *
 * One claim that was NOT confirmed, and is recorded because the code below was
 * shaped around it before it was checked: areEqualFPWithEpsilon is asymmetric
 * in principle -- it scales by |x| alone, so it admits a pair when
 * |x - y| lands in (epsilon*|y|, epsilon*|x|] -- but that window is
 * epsilon^2*|x| wide while |x - y| moves in steps of ulp(x), about 2^-52*|x|.
 * A witness therefore needs epsilon^2 > 2^-52, and the three epsilons the tree
 * instantiates it with (1e-5f, 1e-10, 1e-15L) are all far below that. ESBMC
 * confirms the symmetric case at 1e-10 and refutes it at 1e-3, with
 * x = 1.0, y = 0.9990000000000001: |x - y| is 0.0009999999999998899, which is
 * inside 1e-3*|x| = 0.001 and outside 1e-3*|y| = 0.000999. The documented
 * caveat is real for a caller who picks a larger tolerance and unreachable for
 * the ones in the tree. Symmetry below is by construction rather than by
 * arithmetic luck, which is the difference worth having.
 *
 * The rules are stated once here and proved in
 * tests/verification/float_predicate_proof.cpp. Three constraints shaped the
 * code and are worth stating, because they are not obvious from reading it:
 *
 *  1. ESBMC's --overflow-check covers FLOATING-POINT operations, not just
 *     integer ones: an ieee_sub whose result is infinite is reported as
 *     "arithmetic overflow on floating-point ieee_sub". So no function here may
 *     form a value that overflows, for ANY input. That is why nearlyEqual
 *     never computes x - y (which is infinite for x = DBL_MAX, y = -DBL_MAX)
 *     and never computes epsilon * |x| for an unbounded epsilon.
 *  2. --nan-check likewise. Every NaN is decided by classification, before any
 *     arithmetic runs, rather than by letting a NaN propagate through a
 *     subtraction and produce the answer "false" as a side effect.
 *  3. The logarithm is a template parameter, not a call to <cmath>. ESBMC has
 *     an operational model of std::log2 and it does not discharge -- a single
 *     query over one symbolic call did not return in 180s. Passing it in means
 *     the proof holds for EVERY behaviour of the logarithm, including a wrong
 *     one, which is a stronger statement than assuming a correct libm; the
 *     guards below are what make that true. entropyBits() instantiates it with
 *     std::log2 and is the entry point callers use.
 *
 * Header-only, no allocation, no exceptions, raw pointers and sizes.
 */

#ifndef RETDEC_UTILS_FLOAT_PREDICATE_H
#define RETDEC_UTILS_FLOAT_PREDICATE_H

#include "retdec/utils/bounds.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace retdec {
namespace utils {
namespace fpred {

/// Bits in the significand of an IEEE-754 binary64, counting the implicit one.
///
/// Two things follow from it and both are used below: every integer in
/// [0, 2^53] converts to double exactly, and no probability formed as a ratio
/// of two such integers is smaller than 2^-53, so no binary logarithm of one
/// is below -53.
inline constexpr int kExactBits = 53;

/// The largest integer this header will convert to double.
///
/// Above it the conversion rounds, and `count >= total * ratio` stops being
/// the comparison the caller asked for. Nothing in this tree measures a run of
/// bytes anywhere near 2^53, so refusing is free.
inline constexpr std::size_t kExactInteger = std::size_t{1} << kExactBits;

/// One histogram bucket per distinct byte value.
inline constexpr std::size_t kByteValues = 256;

/// log2(kByteValues): a byte cannot carry more than eight bits of entropy.
inline constexpr double kByteEntropyMax = 8.0;

// ─── classification ──────────────────────────────────────────────────────────
//
// Written as comparisons rather than as calls to std::isnan/std::isinf. A
// comparison neither produces a NaN nor overflows, so these are the only
// operations that may be applied to an operand whose class is not yet known --
// and, unlike the <cmath> predicates, they say the same thing under
// -ffast-math, which this tree does not use but its dependents might.

/// True when @p x is a NaN. NaN is the only value not equal to itself.
template <typename T>
constexpr bool isNaN(T x) noexcept
{
	return !(x == x);
}

/// True when @p x is an infinity of either sign.
///
/// False for a NaN: every comparison against a NaN is false, which is the
/// answer wanted here.
template <typename T>
constexpr bool isInf(T x) noexcept
{
	return x > (std::numeric_limits<T>::max)() || x < -(std::numeric_limits<T>::max)();
}

/// True when @p x is neither NaN nor infinite.
template <typename T>
constexpr bool isFinite(T x) noexcept
{
	return x >= -(std::numeric_limits<T>::max)() && x <= (std::numeric_limits<T>::max)();
}

/// True when @p x and @p y are in the same IEEE class: both NaN, the same
/// infinity, or both finite.
///
/// The infinity arm is `x == y` and not `isInf(x) == isInf(y)`, which is the
/// bug in equality.h:41 -- there std::isinf returns bool, so a positive and a
/// negative infinity compare as the same class and then as equal.
template <typename T>
constexpr bool sameClassT(T x, T y) noexcept
{
	if (isNaN(x) || isNaN(y)) return isNaN(x) && isNaN(y);
	if (isInf(x) || isInf(y)) return x == y;
	return true;
}

constexpr bool sameClass(double x, double y) noexcept
{
	return sameClassT(x, y);
}
constexpr bool sameClass(float x, float y) noexcept
{
	return sameClassT(x, y);
}

// ─── nearlyEqual ─────────────────────────────────────────────────────────────

/// True when @p x and @p y are the same value to within a relative tolerance
/// of @p epsilon.
///
/// Reflexive for every x, including NaN and both infinities, and symmetric for
/// every pair, by construction rather than because the numbers happen to work
/// out -- see the note on areEqualFPWithEpsilon at the top of this file.
///
/// @p epsilon is a RELATIVE tolerance and must lie in [0, 1]. A tolerance of 1
/// already calls every pair of same-signed values equal, so nothing outside
/// that range is a tolerance; NaN, a negative value and a value above 1 are all
/// refused, and a refused tolerance admits nothing beyond exact equality
/// (`x == y`, or two NaNs). That is the deliberate difference from the code
/// this replaces, which has no range check at all and silently answers "not
/// equal" for a NaN epsilon and "equal" for a negative one.
///
/// Neither `x - y` nor `epsilon * |x|` is ever formed, and that is not
/// stylistic. `x - y` is infinite for x = DBL_MAX, y = -DBL_MAX; with epsilon
/// unbounded, `epsilon * |x|` is infinite for any epsilon above
/// DBL_MAX / |x|. Both are overflows on a path with no input validation in
/// front of it. The predicate below is exactly `|x - y| <= epsilon *
/// max(|x|,|y|)` -- proved so in proof_nearly_equal_matches_the_definition --
/// but every intermediate it forms is bounded by max(|x|,|y|).
template <typename T>
inline bool nearlyEqualT(T x, T y, T epsilon) noexcept
{
	// Class first, so no NaN and no infinity reaches the arithmetic below.
	if (isNaN(x) || isNaN(y)) return isNaN(x) && isNaN(y);
	if (isInf(x) || isInf(y)) return x == y;

	// Both finite. Exact equality is decided before the tolerance is even
	// looked at, which is what makes the predicate reflexive for every
	// epsilon; it also settles +0.0 against -0.0.
	if (x == y) return true;

	// `!(0 <= epsilon <= 1)` rather than `epsilon < 0 || epsilon > 1`, so a
	// NaN tolerance is refused by the same test instead of falling through
	// both comparisons.
	if (!(epsilon >= T(0) && epsilon <= T(1))) return false;

	const T a = x < T(0) ? -x : x;
	const T b = y < T(0) ? -y : y;
	const T hi = a > b ? a : b;
	const T lo = a > b ? b : a;

	// hi > 0 here (x != y, so they are not both zero), and epsilon <= 1, so
	// the product is in [0, hi] and cannot overflow.
	const T tol = epsilon * hi;

	if ((x < T(0)) == (y < T(0)))
	{
		// Same sign: |x - y| is the difference of the magnitudes, which lies
		// in [0, hi] and rounds exactly as x - y would have.
		return hi - lo <= tol;
	}

	// Opposite signs: |x - y| is hi + lo, the one sum that can overflow. It is
	// never formed. `hi + lo <= tol` is rearranged to `lo <= tol - hi`, whose
	// right-hand side lies in [-hi, 0]. The two are equivalent and not merely
	// close: lo >= 0 and tol - hi <= 0, so the test can only pass when lo is 0
	// and tol - hi is 0, and a subtraction of two equal finite values is
	// exact. That is precisely the case hi + lo <= tol admits.
	return lo <= tol - hi;
}

inline bool nearlyEqual(double x, double y, double epsilon) noexcept
{
	return nearlyEqualT(x, y, epsilon);
}

inline bool nearlyEqual(float x, float y, float epsilon) noexcept
{
	return nearlyEqualT(x, y, epsilon);
}

// ─── ratioAtLeast ────────────────────────────────────────────────────────────

/// True when @p ratio is a ratio: finite and in [0, 1].
///
/// False for a NaN, because every comparison against a NaN is false. This is
/// the check string.cpp performs with an `assert`, which is compiled out under
/// NDEBUG -- so in a release build the only thing standing between a malformed
/// ratio and the answer is IEEE-754's treatment of NaN comparisons.
constexpr bool ratioIsValid(double ratio) noexcept
{
	return ratio >= 0.0 && ratio <= 1.0;
}

/// Is @p count at least @p ratio of @p total?
///
/// Returns whether the QUESTION was well formed; the answer goes in
/// @p atLeast. That separation is the point of this function: `false` from a
/// bool-returning predicate cannot distinguish "the run is not nice" from "the
/// ratio was NaN so nothing is ever nice", and those need different handling
/// by a caller. @p atLeast is written on every path, so a caller that ignores
/// the return value still gets a defined answer rather than an uninitialised
/// one.
///
/// Refused: a @p ratio that is not in [0, 1]; a @p count larger than @p total,
/// which no caller can produce honestly since the count is taken over the same
/// range as the total; and a @p total above 2^53, where the conversion to
/// double would round.
///
/// A @p total of 0 is well formed and answers false -- an empty run is never
/// nice. Both call sites in string.cpp already say `!s.empty() && ...`, so
/// that rule is theirs, hoisted here where it also keeps the zero out of the
/// multiplication.
inline bool checkedRatioAtLeast(std::size_t count, std::size_t total, double ratio, bool& atLeast) noexcept
{
	atLeast = false;
	if (!ratioIsValid(ratio)) return false;
	if (count > total) return false;
	if (total > kExactInteger) return false;
	if (total == 0) return true;

	// Both conversions are exact: count <= total <= 2^53. The product is in
	// [0, total] because ratio is in [0, 1], so it is neither infinite nor a
	// NaN -- total is at least 1 here, so not even the 0 * inf case arises.
	const double t = static_cast<double>(total);
	const double c = static_cast<double>(count);
	atLeast = c >= t * ratio;
	return true;
}

/// checkedRatioAtLeast as a plain predicate, for a caller whose @p ratio is a
/// compile-time constant it already knows to be valid.
///
/// A malformed request answers false. That is the SAFE direction for the
/// string scanners -- a run that is not emitted as a literal loses information
/// rather than inventing it -- and it is the direction string.cpp gets wrong
/// today for a negative minRatio, where `count >= size * negative` is true for
/// every run and every run of bytes becomes a string.
inline bool ratioAtLeast(std::size_t count, std::size_t total, double ratio) noexcept
{
	bool atLeast = false;
	return checkedRatioAtLeast(count, total, ratio, atLeast) && atLeast;
}

// ─── entropyBits ─────────────────────────────────────────────────────────────

/// Sum of @p buckets counts, refused if the sum would wrap.
///
/// @p total is written on every path. The addition is guarded by
/// bounds::addFits rather than checked after the fact, which is the same rule
/// every count in this tree goes through.
constexpr bool histogramTotal(const std::uint32_t* histogram, std::size_t buckets, std::size_t& total) noexcept
{
	total = 0;
	if (histogram == nullptr) return false;
	std::size_t sum = 0;
	for (std::size_t i = 0; i < buckets; ++i)
	{
		if (!bounds::addFits(sum, histogram[i])) return false;
		sum += histogram[i];
	}
	total = sum;
	return true;
}

/// Shannon entropy of @p histogram, in bits, written to @p out.
///
/// Returns false, and leaves @p out at 0.0, when the measurement cannot be
/// made: a null histogram, a @p total of 0, a @p total the histogram does not
/// actually sum to, a @p total too large to convert exactly, or a @p log2fn
/// that returns something that is not a binary logarithm of a probability.
///
/// The return value is the whole point. GpuScanner::fileEntropy returns 0.0
/// for an invalid range, which is indistinguishable from a genuinely uniform
/// -- zero-entropy -- region, and entropy is what decides whether a file looks
/// packed. A caller here can tell the two apart.
///
/// @p log2fn is the binary logarithm. It is a template parameter so that the
/// proof can instantiate it with a function returning an unconstrained double:
/// what is proved is then that NO behaviour of the logarithm can make this
/// function overflow, produce a NaN, divide by zero, or return an @p out
/// outside [0, @p maxBits]. std::log2 is one such behaviour.
///
/// @p maxBits is the largest entropy @p buckets symbols can carry, log2 of the
/// bucket count -- 8.0 for a byte histogram.
template <typename Log2Fn>
inline bool entropyBitsWith(
	const std::uint32_t* histogram,
	std::size_t buckets,
	std::size_t total,
	double maxBits,
	double& out,
	Log2Fn log2fn) noexcept
{
	out = 0.0;
	if (histogram == nullptr) return false;
	// The property this function exists for: an empty range is refused, not
	// reported as zero entropy.
	if (total == 0) return false;
	if (total > kExactInteger) return false;
	// `!(maxBits >= 0)` also refuses a NaN bound, which would make the clamp
	// below a no-op and let any value out.
	if (!(maxBits >= 0.0)) return false;

	std::size_t sum = 0;
	if (!histogramTotal(histogram, buckets, sum)) return false;
	// The histogram and the total are two separate statements about the same
	// range; if they disagree, one of them was computed from a length that
	// underflowed, which is exactly the fault GpuScanner::fileEntropy had.
	if (sum != total) return false;

	// Exact, and at least 1: the divisor cannot be zero.
	const double t = static_cast<double>(total);

	double e = 0.0;
	for (std::size_t i = 0; i < buckets; ++i)
	{
		const std::uint32_t n = histogram[i];
		if (n == 0) continue;

		// n <= sum == total <= 2^53, so both conversions are exact and the
		// quotient is a genuine probability in (0, 1].
		const double c = static_cast<double>(n);
		const double p = c / t;
		const double lg = log2fn(p);

		// A logarithm that is not one must not reach the arithmetic. For p in
		// (0, 1] a binary logarithm is in [-53, 0]: it is at most 0 because p
		// is at most 1, and at least -53 because p is at least 1/total and
		// total is at most 2^53. Written as `!(in range)` so a NaN from a
		// broken libm is refused here rather than multiplied.
		if (!(lg <= 0.0 && lg >= -static_cast<double>(kExactBits))) return false;

		// p is in (0, 1] and -lg is in [0, 53], so each term is in [0, 53] and
		// the running sum is bounded by buckets * 53. No term and no partial
		// sum can overflow or be a NaN.
		e += p * (-lg);
	}

	// Clamped, not refused. The exact maximum is maxBits, attained by a
	// perfectly uniform histogram -- and that is the case a packer detector
	// cares about most, so it is also the case that must not be rejected when
	// rounding across the accumulation lands a few ulps above the bound.
	if (e > maxBits) e = maxBits;
	out = e;
	return true;
}

/// std::log2 as a callable, for the production instantiation below.
struct StdLog2
{
	double operator()(double p) const noexcept
	{
		return std::log2(p);
	}
};

/// Entropy of a 256-bucket byte histogram, in bits, in [0, 8].
inline bool entropyBits(const std::uint32_t* histogram256, std::size_t total, double& out) noexcept
{
	return entropyBitsWith(histogram256, kByteValues, total, kByteEntropyMax, out, StdLog2{});
}

} // namespace fpred
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_FLOAT_PREDICATE_H
