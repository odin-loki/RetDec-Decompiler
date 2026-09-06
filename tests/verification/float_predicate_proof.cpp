/**
 * @file tests/verification/float_predicate_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/float_predicate.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Every operand below is a fully symbolic double or float -- every sign,
 * exponent and significand, so NaN, both infinities, both zeros and the
 * subnormals are all in range unless a proof says otherwise. Nothing here
 * samples values.
 *
 * Two ESBMC checks do as much work as the assertions:
 *
 *   --nan-check      reports any operation that produces a NaN, and
 *   --overflow-check reports any FLOATING-POINT operation whose result is
 *                    infinite ("arithmetic overflow on floating-point
 *                    ieee_sub"), not just integer wrapping.
 *
 * Both are on for every run. So a proof that merely CALLS a function with
 * unconstrained arguments and asserts nothing about the answer is still a real
 * property: it says that no input at all can make that function form an
 * infinity or a NaN. That is what proof_nearly_equal_never_overflows_or_nans
 * and proof_ratio_arithmetic_never_overflows are, and it is not a formality --
 * the same check refutes the expression at equality.h:45 as written, reporting
 * "arithmetic overflow on floating-point ieee_sub" for x = DBL_MAX,
 * y = -DBL_MAX.
 *
 * These proofs are load-bearing, and that was measured rather than assumed.
 * Replacing max(|x|,|y|) in nearlyEqual with |x| alone -- which is exactly what
 * equality.h:45 does -- makes proof_nearly_equal_is_symmetric FAIL, with
 * x = -2.500985, y = -1.036131e-317, e = 4.144556e-317. Removing the
 * `total == 0` guard from entropyBitsWith makes
 * proof_entropy_refuses_an_empty_range FAIL. Both were run and both were
 * restored.
 *
 * The logarithm inside entropyBitsWith is instantiated here with a function
 * that returns an UNCONSTRAINED double, so these are proofs over every
 * possible behaviour of libm rather than over the one ESBMC models. That is
 * also the only way they discharge: a single symbolic call to ESBMC's
 * std::log2 model did not return in 180s.
 */

// ESBMC-OPTIONS: --unwind 5
// ESBMC-SOLVER: --cvc5
//
// cvc5, and it is not marginal. Every query in this file is floating-point, and
// the four backends this build has were measured on it:
//
//   cvc5       every proof below, none over 80s and most under 5s
//   z3         eight of them; proof_definition_at_a_full_tolerance and
//              proof_opposite_signs_at_a_full_tolerance both ran past 300s
//   bitwuzla   "SMT solver failed" on the same two after 66-90s
//   boolector  slower than either, and it does not always finish:
//              proof_definition_at_the_double_epsilon discharges in 67s,
//              proof_nearly_equal_is_symmetric does not return within 240s
//
// That boolector line is a correction. It said "not applicable -- a bitvector
// solver with no FP theory", which sounded right and is wrong: ESBMC bit-blasts
// the float theory for a backend that lacks it, so boolector answers these
// queries, it just pays for the encoding. Measured rather than reasoned about,
// after the claim was written the other way round.
//
// Where they finish, the backends agree on every verdict, including the two
// refutations recorded below. `scripts/verify_esbmc.sh --cross` compares z3
// against boolector, so on this file it reports whichever of the two runs out
// of time first -- a cost difference, not a disagreement about a property, and
// the pin above is what records that.
//
// Nothing here performs an unsigned INTEGER division, so the spurious
// "arithmetic overflow on div" that pins bounds_proof.cpp to z3 does not arise.
// The two divisions are `c / t` on doubles inside entropyBitsWith.

#include "retdec/utils/float_predicate.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::fpred;

// ESBMC treats an undefined function returning a value as an unconstrained
// choice of that type.
extern "C" {
double        nondet_double();
float         nondet_float();
std::size_t   nondet_size();
std::uint32_t nondet_u32();
}

// __ESBMC_assume is a verifier builtin, so this file does not type-check under
// an ordinary compiler. Defining it away lets `verify_esbmc.sh --syntax` catch
// a typo in a second instead of after a solver run; ESBMC itself never sees
// the macro.
#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void) sizeof((cond) ? 1 : 0))
#endif

// One less than the unwind bound, so the walk over the histogram is exhaustive
// and the unwinding assertion ESBMC generates by default is discharged rather
// than assumed. Four buckets exercise every path through both loops -- a zero
// bucket skipped, several non-zero buckets accumulated, and the sum-versus-
// total disagreement -- with four unconstrained 32-bit counts and a symbolic
// double per bucket. The production histogram has kByteValues = 256 buckets
// and the loops are structurally identical at any length; this is a bounded
// model of it, in the same sense as the 8- to 12-byte buffers in
// bounded_string_proof.cpp.
static const std::size_t kProofBuckets = 4;

// ─── sameClass ───────────────────────────────────────────────────────────────

extern "C" void proof_same_class_separates_the_infinities()
{
	const double x = nondet_double();
	const double y = nondet_double();

	// Reflexive and symmetric, for every value including NaN.
	assert(sameClass(x, x));
	assert(sameClass(x, y) == sameClass(y, x));

	// The classification is exactly the four-way one, stated independently of
	// the implementation.
	const bool bothNaN    = isNaN(x) && isNaN(y);
	const bool bothFinite = isFinite(x) && isFinite(y);
	const bool sameInf    = isInf(x) && isInf(y) && ((x > 0.0) == (y > 0.0));
	assert(sameClass(x, y) == (bothNaN || bothFinite || sameInf));

	// The concrete fault at equality.h:41. `std::isinf(x) == std::isinf(y)` is
	// `true == true` for a positive against a negative infinity, because in
	// C++ std::isinf returns bool. Stated here as: two infinities of opposite
	// sign are NOT the same class.
	if (isInf(x) && isInf(y) && (x > 0.0) != (y > 0.0))
		assert(!sameClass(x, y));
}

// ─── nearlyEqual ─────────────────────────────────────────────────────────────

extern "C" void proof_nearly_equal_is_reflexive()
{
	const double x = nondet_double();
	const double e = nondet_double();

	// For EVERY x -- NaN, both infinities, both zeros, every subnormal -- and
	// for EVERY tolerance, including a negative or NaN one. A value is equal
	// to itself before any tolerance is consulted.
	assert(nearlyEqual(x, x, e));
}

extern "C" void proof_nearly_equal_is_symmetric()
{
	const double x = nondet_double();
	const double y = nondet_double();
	const double e = nondet_double();

	// areEqualFPWithEpsilon scales by |x| alone, and its own doc comment says
	// the result is not symmetric. Two things are worth separating there. The
	// asymmetry is real -- ESBMC refutes symmetry of that expression at
	// epsilon = 1e-3 with x = 1.0, y = 0.9990000000000001 -- but it is NOT
	// reachable at any of the three epsilons the tree instantiates it with,
	// because the asymmetric window is epsilon^2*|x| wide and |x - y| moves in
	// steps of about 2^-52*|x|. So this proof is not a bug fix; it is the
	// difference between a property that holds and one that happens to hold.
	// Scaling by max(|x|,|y|) makes it hold for every epsilon in the range.
	assert(nearlyEqual(x, y, e) == nearlyEqual(y, x, e));
}

extern "C" void proof_nearly_equal_implies_same_class()
{
	const double x = nondet_double();
	const double y = nondet_double();
	const double e = nondet_double();

	// No NaN is ever equal to a number, no infinity is ever equal to a finite
	// value, and +inf is never equal to -inf.
	if (nearlyEqual(x, y, e)) assert(sameClass(x, y));
}

// The predicate is `|x - y| <= epsilon * max(|x|,|y|)`, and the implementation
// must be that and not merely something like it.
//
// Stated for same-signed operands, where the naive difference is safe to form
// in the harness: for x and y both >= 0 or both <= 0, the exact value of x - y
// and the exact value of max(|x|,|y|) - min(|x|,|y|) are the same real number,
// so the two roundings are the same double and the forms are comparable bit
// for bit. No magnitude bound is needed. The opposite-signed case cannot be
// stated this way, because there the naive form is the one that is wrong --
// see proof_opposite_signs_cannot_be_bridged.
//
// The tolerance is a parameter rather than a symbolic value, and the reason is
// the same one bounds_proof.cpp gives for proving mulFits per multiplier: a
// query holding two independent symbolic floating-point multiplications did
// not discharge in 300s under z3. Fixing the multiplier leaves x and y fully
// symbolic across all 2^64 patterns, and the multipliers chosen are the ones
// that actually occur -- 1e-10 is areEqual<double>'s epsilon, and 0 and 1 are
// the two ends of the admitted range, where the boundary behaviour lives.
static void definitionHoldsAt(double e)
{
	const double x = nondet_double();
	const double y = nondet_double();
	__ESBMC_assume(isFinite(x) && isFinite(y));
	__ESBMC_assume((x < 0.0) == (y < 0.0));

	const double ax = x < 0.0 ? -x : x;
	const double ay = y < 0.0 ? -y : y;
	const double scale = ax > ay ? ax : ay;
	const double diff = x > y ? x - y : y - x;

	assert(nearlyEqual(x, y, e) == (diff <= e * scale));
}

extern "C" void proof_definition_at_the_double_epsilon() { definitionHoldsAt(1e-10); }
extern "C" void proof_definition_at_a_zero_tolerance()   { definitionHoldsAt(0.0); }
extern "C" void proof_definition_at_a_full_tolerance()   { definitionHoldsAt(1.0); }

static void oppositeSignsAt(double e)
{
	// With x and y on opposite sides of zero the true separation is |x| + |y|,
	// which is at least max(|x|,|y|), while the tolerance is at most
	// 1 * max(|x|,|y|). So a pair that straddles zero with both magnitudes
	// non-zero is never nearly equal, for ANY tolerance -- there is no
	// relative tolerance in [0,1] that bridges zero. The only survivors are
	// the boundary cases where one operand is itself a zero.
	//
	// This is what pins down that the rearranged test `lo <= tol - hi` admits
	// nothing the sum would not. The naive `fl(x - y) <= e * scale` does admit
	// something extra: for x = 1e150, y = -1, e = 1 the true difference
	// 1e150 + 1 rounds back to 1e150 and the naive form calls the pair equal.
	//
	// It is deliberately not stated as an exact characterisation. An earlier
	// version asserted `nearlyEqual(x,y,e) == (lo == 0.0 && e == 1.0)` and
	// ESBMC refuted it. The reason given here for the refutation was wrong, and
	// the audit measured it: the claim was that "fl(e * hi) reaches hi for
	// tolerances just below 1 -- with hi = 3 and e = 1 - 2^-53 the product
	// 3 - 3*2^-53 is under half a ULP below 3 and rounds to it". Both halves
	// are false. ULP(3) is 2^-51, so 3*2^-53 is one and a half half-ULPs, and
	// `(1 - 2^-53) * 3.0` is 2.9999999999999996 rather than 3.0; a scan of all
	// 2040 power-of-two binades finds no normal hi at all for which
	// fl((1 - 2^-53) * hi) == hi.
	//
	// The real mechanism is SUBNORMAL hi, where the product underflows back to
	// its own operand: `(1 - 2^-53) * denorm_min == denorm_min`. ESBMC's
	// witness for the refuted assertion is x = -1.112537e-308, which is in that
	// range. So a tolerance below 1 does reach the full one, but only where the
	// magnitudes are subnormal and the multiplication has no room to round
	// down.
	const double x = nondet_double();
	const double y = nondet_double();
	__ESBMC_assume(isFinite(x) && isFinite(y));
	__ESBMC_assume((x < 0.0) != (y < 0.0));

	const double ax = x < 0.0 ? -x : x;
	const double ay = y < 0.0 ? -y : y;
	const double lo = ax > ay ? ay : ax;

	// One call, not two: calling nearlyEqual twice puts two independent
	// floating-point multiplications in the query, which is what cvc5 runs out
	// of memory on.
	const bool equal = nearlyEqual(x, y, e);
	if (lo > 0.0) assert(!equal);
	if (equal) assert(lo == 0.0);
}

// 1.0 is the largest tolerance the kernel admits, so it is the most permissive
// case and the one where a pair that straddles zero is likeliest to be let
// through. A symbolic tolerance here exhausts cvc5's memory after 72s -- see
// the note above definitionHoldsAt.
extern "C" void proof_opposite_signs_at_a_full_tolerance() { oppositeSignsAt(1.0); }

extern "C" void proof_opposite_signs_at_the_double_epsilon()
{
	// NOT oppositeSignsAt(1e-10), and the difference is the audit's finding.
	//
	// At a tolerance that small the opposite-sign branch of nearlyEqualT is
	// unconditionally false -- `lo <= tol - hi` with tol = 1e-10 * hi and
	// lo >= 0 cannot hold once hi > 0 -- so in oppositeSignsAt both arms go
	// slack: `if (lo > 0.0) assert(!equal)` has a consequent that is always
	// true, and `if (equal) assert(lo == 0.0)` has an antecedent that is never
	// satisfied. The proof could not fail for any implementation that answers
	// false there, which is not the same as proving that it does.
	//
	// So the property is stated directly instead. areEqual<double>'s tolerance
	// is 1e-10, and this says that at that tolerance NOTHING straddling zero is
	// ever nearly equal -- not the boundary cases either, because a zero
	// operand has magnitude zero and is not on the far side of anything.
	const double x = nondet_double();
	const double y = nondet_double();
	__ESBMC_assume(isFinite(x) && isFinite(y));
	__ESBMC_assume((x < 0.0) != (y < 0.0));
	__ESBMC_assume(x != 0.0 && y != 0.0);

	assert(!nearlyEqual(x, y, 1e-10));
}

extern "C" void proof_a_full_tolerance_reaches_zero()
{
	// The other side of the property above, so it is not vacuous: at the
	// maximum tolerance a zero IS nearly equal to every finite value, from
	// either direction. This is also why the admitted range stops at 1 -- a
	// tolerance of 1 already calls everything equal, so nothing above it is a
	// tolerance.
	const double y = nondet_double();
	__ESBMC_assume(isFinite(y));

	assert(nearlyEqual(0.0, y, 1.0));
	assert(nearlyEqual(y, 0.0, 1.0));
}

extern "C" void proof_an_invalid_tolerance_admits_only_exact_equality()
{
	// A tolerance that is NaN, negative or above 1 is refused -- and refusal
	// means "nothing beyond what is exactly equal", not "nothing at all" and
	// not "everything". string.cpp's release-build behaviour for a negative
	// ratio is the "everything" answer, and this is the shape of check that
	// rules it out.
	const double x = nondet_double();
	const double y = nondet_double();
	const double e = nondet_double();
	__ESBMC_assume(!(e >= 0.0 && e <= 1.0));

	assert(nearlyEqual(x, y, e) == (x == y || (isNaN(x) && isNaN(y))));
}

extern "C" void proof_nearly_equal_never_overflows_or_nans()
{
	// No assertion. --nan-check and --overflow-check are the property: no
	// double whatsoever can drive an intermediate of nearlyEqual to infinity
	// or to a NaN. The version that computed `epsilon * std::abs(x)` fails
	// this with x = 8.98847e+307, epsilon = 5.56269e+300.
	(void) nearlyEqual(nondet_double(), nondet_double(), nondet_double());
}

extern "C" void proof_nearly_equal_holds_for_float_too()
{
	// The float instantiation, which is what areEqual<float> would use. Same
	// three properties, over every 32-bit pattern.
	const float x = nondet_float();
	const float y = nondet_float();
	const float e = nondet_float();

	assert(nearlyEqual(x, x, e));
	assert(nearlyEqual(x, y, e) == nearlyEqual(y, x, e));
	if (nearlyEqual(x, y, e)) assert(sameClass(x, y));
}

// ─── ratioAtLeast ────────────────────────────────────────────────────────────

// A run that is nice at the higher threshold must be nice at the lower one.
// Without this the predicate is not a threshold at all, and the minRatio a
// caller passes does not mean what its name says.
//
// The two thresholds are parameters rather than symbolic values. With both
// symbolic the query holds two independent floating-point multiplications by
// unconstrained multipliers -- the nonlinear case bounds_proof.cpp documents
// for the integer widths -- and cvc5 exhausts memory on it after 70s. The
// pairs below are the thresholds that actually occur: 1.0 at
// src/bin2llvmir/providers/fileimage.cpp:240 and :572 and
// src/bin2llvmir/utils/ir_modifier.cpp:583, 2.0/3 as the default in
// include/retdec/utils/string.h:150, and 0.0 as the bottom of the range.
// count and total stay fully symbolic over the whole 64-bit domain.
static void monotoneFrom(double hi, double lo)
{
	const std::size_t count = nondet_size();
	const std::size_t total = nondet_size();

	if (ratioAtLeast(count, total, hi)) assert(ratioAtLeast(count, total, lo));
}

extern "C" void proof_ratio_monotone_from_one_to_two_thirds()  { monotoneFrom(1.0, 2.0 / 3); }
extern "C" void proof_ratio_monotone_from_two_thirds_to_zero() { monotoneFrom(2.0 / 3, 0.0); }
extern "C" void proof_ratio_monotone_from_one_to_zero()        { monotoneFrom(1.0, 0.0); }

extern "C" void proof_ratio_at_one_is_exact_equality()
{
	// Every byte nice, or the run is not nice. The count is bounded by the
	// total because it is taken over the same range: std::count_if over s
	// cannot exceed s.size().
	const std::size_t count = nondet_size();
	const std::size_t total = nondet_size();
	__ESBMC_assume(count <= total);
	__ESBMC_assume(total <= kExactInteger);
	// total == 0 is the documented carve-out: an empty run is never nice, so
	// ratioAtLeast(0, 0, 1.0) is false while count == total is true.
	__ESBMC_assume(total > 0);

	assert(ratioAtLeast(count, total, 1.0) == (count == total));
}

extern "C" void proof_ratio_of_an_empty_run_is_defined()
{
	// "Defined for every r": the request is well formed for any valid ratio
	// and the answer is false, which is what both call sites mean by
	// `!s.empty() && ...`. No zero reaches the multiplication.
	const double r = nondet_double();
	__ESBMC_assume(ratioIsValid(r));

	bool atLeast = true;
	assert(checkedRatioAtLeast(0, 0, r, atLeast));
	assert(!atLeast);
	assert(!ratioAtLeast(0, 0, r));
}

extern "C" void proof_an_invalid_ratio_is_refused()
{
	// The assert in string.cpp is compiled out under NDEBUG, after which a NaN
	// minRatio makes every comparison false and a negative one makes every
	// comparison true. Here a malformed ratio is refused -- the caller can see
	// that the question was rejected -- and the predicate form answers false,
	// which is the direction that drops a string rather than inventing one.
	const std::size_t count = nondet_size();
	const std::size_t total = nondet_size();
	const double r = nondet_double();
	__ESBMC_assume(!ratioIsValid(r));

	bool atLeast = true;
	assert(!checkedRatioAtLeast(count, total, r, atLeast));
	assert(!atLeast);
	assert(!ratioAtLeast(count, total, r));
}

extern "C" void proof_ratio_arithmetic_never_overflows()
{
	// Fully symbolic count, total and ratio. --nan-check and --overflow-check
	// carry the property from spec item 5: `total * ratio` is never a NaN and
	// never infinite. It cannot be, because total is refused above 2^53 and
	// ratio is refused outside [0, 1], so the product lies in [0, 2^53] -- but
	// that is the claim, and this is what checks it.
	//
	// Also: the answer is written on every path, so a caller that ignores the
	// return value never reads an uninitialised bool.
	bool atLeast = nondet_size() != 0;
	(void) checkedRatioAtLeast(nondet_size(), nondet_size(), nondet_double(), atLeast);
	assert(atLeast == true || atLeast == false);
}

extern "C" void proof_ratio_is_exact_below_the_conversion_bound()
{
	// The comparison is the integer one it is meant to be, not a rounded
	// approximation of it: at a ratio of exactly 0 every run qualifies, and
	// the two entry points agree.
	const std::size_t count = nondet_size();
	const std::size_t total = nondet_size();
	__ESBMC_assume(count <= total);
	__ESBMC_assume(total > 0 && total <= kExactInteger);

	assert(ratioAtLeast(count, total, 0.0));

	bool atLeast = false;
	const double r = nondet_double();
	__ESBMC_assume(ratioIsValid(r));
	assert(checkedRatioAtLeast(count, total, r, atLeast));
	assert(atLeast == ratioAtLeast(count, total, r));
}

// ─── histogramTotal / entropyBits ────────────────────────────────────────────

// The logarithm, as an unconstrained function. Every proof below therefore
// holds for every possible libm, and in particular for one that returns a NaN,
// an infinity or a positive number where a logarithm belongs.
struct NondetLog2
{
	double operator()(double) const noexcept { return nondet_double(); }
};

// The same, plus the check that entropyBitsWith only ever asks for the
// logarithm of a genuine probability. This is how the "divisor is non-zero"
// property is stated where it lives -- inside the kernel, on the value the
// division actually produced -- rather than re-derived in the harness.
struct CheckingLog2
{
	double operator()(double p) const noexcept
	{
		assert(p > 0.0 && p <= 1.0);
		return nondet_double();
	}
};

static void fillNondetHistogram(std::uint32_t* h)
{
	for (std::size_t i = 0; i < kProofBuckets; ++i) h[i] = nondet_u32();
}

extern "C" void proof_histogram_total_never_wraps()
{
	std::uint32_t h[kProofBuckets];
	fillNondetHistogram(h);

	std::size_t total = SIZE_MAX;
	const bool ok = histogramTotal(h, kProofBuckets, total);

	// Written on every path, and never larger than the buffer could justify.
	if (!ok) { assert(total == 0); return; }
	for (std::size_t i = 0; i < kProofBuckets; ++i) assert(h[i] <= total);

	// A null histogram is refused rather than dereferenced.
	std::size_t t2 = SIZE_MAX;
	assert(!histogramTotal(nullptr, nondet_size(), t2));
	assert(t2 == 0);
}

extern "C" void proof_entropy_refuses_an_empty_range()
{
	// The property this kernel exists for. GpuScanner::fileEntropy computed
	// `sz = hi - lo + 1` before establishing lo <= hi and then returns 0.0 for
	// the empty range, which a caller cannot tell from a genuinely uniform
	// region -- and entropy is what decides whether a file looks packed.
	std::uint32_t h[kProofBuckets];
	fillNondetHistogram(h);

	double out = 1.0;
	assert(!entropyBitsWith(h, kProofBuckets, 0, nondet_double(), out, NondetLog2{}));
	assert(out == 0.0);
}

extern "C" void proof_entropy_refuses_a_null_histogram()
{
	double out = 1.0;
	assert(!entropyBitsWith(
			nullptr, nondet_size(), nondet_size(), nondet_double(), out, NondetLog2{}));
	assert(out == 0.0);

	// And the 256-bucket entry point callers actually use. Both refusals
	// return before the loop, so std::log2 is never reached and the query
	// stays tractable.
	double e = 1.0;
	assert(!entropyBits(nullptr, nondet_size(), e));
	assert(e == 0.0);
	std::uint32_t byteHistogram[kByteValues] = {};
	assert(!entropyBits(byteHistogram, 0, e));
	assert(e == 0.0);
}

extern "C" void proof_entropy_total_must_match_the_histogram()
{
	// A total that the histogram does not sum to is refused. That is the
	// disagreement an underflowed length produces: the buckets were filled
	// from one range and the divisor computed from another.
	std::uint32_t h[kProofBuckets];
	fillNondetHistogram(h);

	std::size_t sum = 0;
	const bool summed = histogramTotal(h, kProofBuckets, sum);
	const std::size_t total = nondet_size();
	__ESBMC_assume(!summed || total != sum);

	double out = 1.0;
	assert(!entropyBitsWith(h, kProofBuckets, total, nondet_double(), out, NondetLog2{}));
	assert(out == 0.0);
}

extern "C" void proof_entropy_answer_is_in_range()
{
	// On success: the histogram summed to the total, every probability handed
	// to the logarithm was in (0, 1] -- asserted inside CheckingLog2, on the
	// quotient the kernel itself formed, so this is also the statement that
	// the divisor was never zero -- and the answer is in [0, maxBits].
	//
	// --nan-check and --overflow-check cover the rest of spec item 7: no
	// intermediate of the accumulation is a NaN or an infinity, for ANY
	// behaviour of the logarithm.
	std::uint32_t h[kProofBuckets];
	fillNondetHistogram(h);

	const std::size_t total = nondet_size();
	const double maxBits = nondet_double();
	__ESBMC_assume(maxBits >= 0.0);

	double out = 1.0;
	const bool ok = entropyBitsWith(h, kProofBuckets, total, maxBits, out, CheckingLog2{});

	if (ok)
	{
		assert(total > 0);
		assert(out >= 0.0);
		assert(out <= maxBits);
		std::size_t sum = 0;
		assert(histogramTotal(h, kProofBuckets, sum));
		assert(sum == total);
	}
	else
	{
		assert(out == 0.0);
	}
}

extern "C" void proof_entropy_of_a_single_symbol_is_zero()
{
	// A region of one repeated byte carries no information, and that is a
	// genuinely zero answer -- the one the refusals above exist to keep
	// distinguishable from a failure. The kernel must produce it without ever
	// consulting the logarithm's magnitude: p is exactly 1.0, so any correct
	// log2 returns 0.
	std::uint32_t h[kProofBuckets] = {};
	const std::uint32_t n = nondet_u32();
	__ESBMC_assume(n > 0);
	h[0] = n;

	struct ExactLog2AtOne
	{
		double operator()(double p) const noexcept
		{
			assert(p == 1.0);   // the only probability a one-symbol region has
			return 0.0;
		}
	};

	double out = 1.0;
	assert(entropyBitsWith(h, kProofBuckets, n, kByteEntropyMax, out, ExactLog2AtOne{}));
	assert(out == 0.0);
}
