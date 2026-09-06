/**
 * @file tests/verification/branch_target_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/branch_target.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * A branch displacement is a signed number read out of a file, added to a
 * position also derived from that file, and the result is used as a
 * basic-block leader. Two of the three lifters in this tree get it wrong in
 * ways that are invisible on a well-formed input, so these properties are
 * stated for every input rather than for the ones a test would think of.
 *
 * There are no loops in this file. No unwinding bound applies, so every verdict
 * below is a proof over the whole domain of its inputs and not a bounded
 * search.
 *
 * On domains. Two proofs -- proof_relative_matches_the_int64_sum and
 * proof_relative_matches_the_int64_sum_at_63_bits -- need an oracle for "the
 * mathematical sum", and the only oracle available inside the harness is the
 * sum computed in a width where it provably cannot overflow. That restricts
 * those two to a base and a displacement small enough for int64 to be exact.
 * Both restrictions are real format bounds, named at each __ESBMC_assume, and
 * proof_relative_is_total_over_the_whole_domain covers what is left: for every
 * uint64 base, every int64 delta and every codeSize, the function is defined,
 * nothing in it overflows, and a success is inside the code.
 */

// ESBMC-SOLVER: --z3
//
// Measured, not assumed. Every proof below was run under both backends: they
// agree on all sixteen, and z3 is the faster of the two on this file -- the
// three tableEnd proofs take 2-3s under z3 against 7-12s under boolector, and
// nothing here is slower under z3. `scripts/verify_esbmc.sh --cross` therefore
// reports agreement rather than an expected disagreement.
//
// The pin is still worth writing down, because the obvious reading of it is
// wrong. tableEnd calls bounds::mulFits, which is `b <= SIZE_MAX / a` -- an
// unsigned division, and ESBMC 8.5.0 emits a div-overflow check for those even
// though only signed INT64_MIN / -1 can overflow. That artifact is what pins
// bounds_proof.cpp to z3. It does NOT arise here: the entry width is a
// template constant in every tableEnd proof, so the divisor folds and the
// check is discharged trivially. An earlier draft left the width symbolic over
// {2, 4, 8}; that version timed out at 300s under z3, which is what motivated
// fixing the width in the first place.

#include "retdec/utils/branch_target.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::btgt;

// ESBMC treats an undefined function returning a value as an unconstrained
// choice of that type.
extern "C" {
std::uint64_t nondet_u64();
std::int64_t  nondet_i64();
}

// __ESBMC_assume is a verifier builtin, so this file does not type-check under
// a plain compiler without a stand-in. `scripts/verify_esbmc.sh --syntax` uses
// one; it never reaches the solver.
#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void) sizeof((cond) ? 1 : 0))
#endif

/// The largest code size any of these formats can declare.
///
/// A JVM Code attribute's `code_length` is a u4; a CIL fat method header's
/// `CodeSize` is a uint32; a DEX `insns_size` is a u4 count of 16-bit units.
/// So a method's code is shorter than 2^32 units in all three.
static const std::uint64_t kMaxCodeSize = 0xFFFFFFFFu;

/// The widest branch displacement any of these formats encodes.
///
/// CIL InlineBrTarget and JVM goto_w/jsr_w/tableswitch entries all carry a
/// signed 32-bit displacement; ShortInlineBrTarget (int8) and the JVM's
/// ordinary if/goto (int16) are strictly inside that range.
static const std::int64_t kMinDisp = INT32_MIN;
static const std::int64_t kMaxDisp = INT32_MAX;

// ─── relative ────────────────────────────────────────────────────────────────

extern "C" void proof_relative_matches_the_int64_sum()
{
	std::uint64_t base = nondet_u64();
	std::int64_t  delta = nondet_i64();
	std::uint64_t codeSize = nondet_u64();
	__ESBMC_assume(base <= kMaxCodeSize);
	__ESBMC_assume(delta >= kMinDisp && delta <= kMaxDisp);
	__ESBMC_assume(codeSize <= kMaxCodeSize);

	std::uint64_t target = nondet_u64();
	const bool ok = relative(base, delta, codeSize, target);

	// The oracle. In this domain the operands are at most 2^32 and 2^31, so the
	// int64 sum is exact -- and --overflow-check is what says so, not this
	// comment. This is the sum cil_lifter.cpp:523 computes correctly and then
	// throws away by casting to uint32.
	const std::int64_t sum = static_cast<std::int64_t>(base) + delta;
	const bool truth = sum >= 0 && static_cast<std::uint64_t>(sum) < codeSize;

	assert(ok == truth);
	// Exactly the sum, with no truncation to 32 bits. base = 2, delta = -128
	// gives sum = -126: `truth` is false, so ok must be false, and the uint32
	// value 0xFFFFFF82 that cil_lifter.cpp records instead is not reachable
	// from here at all.
	if (ok) assert(target == static_cast<std::uint64_t>(sum));
}

extern "C" void proof_relative_matches_the_int64_sum_at_63_bits()
{
	// The same oracle far outside the 32-bit formats, so the agreement above is
	// not an artifact of small operands. The bound is the largest base for
	// which base + INT32_MAX is still an exact int64.
	std::uint64_t base = nondet_u64();
	std::int64_t  delta = nondet_i64();
	std::uint64_t codeSize = nondet_u64();
	__ESBMC_assume(base <= static_cast<std::uint64_t>(INT64_MAX) - static_cast<std::uint64_t>(INT32_MAX));
	__ESBMC_assume(delta >= kMinDisp && delta <= kMaxDisp);

	std::uint64_t target = nondet_u64();
	const bool ok = relative(base, delta, codeSize, target);

	const std::int64_t sum = static_cast<std::int64_t>(base) + delta;
	const bool truth = sum >= 0 && static_cast<std::uint64_t>(sum) < codeSize;

	assert(ok == truth);
	if (ok) assert(target == static_cast<std::uint64_t>(sum));
}

extern "C" void proof_relative_no_signed_overflow_in_the_format_domain()
{
	// Property 2, and the proof is the absence of a violation rather than an
	// assertion: --overflow-check is on, and the int64 addition below is the
	// one jvm_lifter.cpp:496 performs in int32 as
	// `static_cast<int32_t>(instrPc) + offset`. A JVM code_length is a u4, so
	// instrPc reaches 2^32-1 and casts negative; offset is a full int32 from
	// goto_w. In int64 the sum is bounded and defined for every such pair.
	std::uint64_t base = nondet_u64();
	std::int64_t  delta = nondet_i64();
	__ESBMC_assume(base <= kMaxCodeSize);
	__ESBMC_assume(delta >= kMinDisp && delta <= kMaxDisp);

	const std::int64_t sum = static_cast<std::int64_t>(base) + delta;

	// Stated as well as discharged: the sum is inside [INT32_MIN, 2^32+INT32_MAX],
	// which is well within int64 and is why the widening is the fix.
	assert(sum >= kMinDisp);
	assert(sum <= static_cast<std::int64_t>(kMaxCodeSize) + kMaxDisp);

	// And relative() agrees with it, so the widening is not merely available,
	// it is what the kernel does.
	std::uint64_t target = nondet_u64();
	const std::uint64_t codeSize = nondet_u64();
	if (relative(base, delta, codeSize, target))
		assert(target == static_cast<std::uint64_t>(sum));
}

extern "C" void proof_relative_is_total_over_the_whole_domain()
{
	// No assumptions at all: any uint64 base -- including one past 2^63, where
	// `(int64)base + delta` would itself be undefined -- any int64 delta,
	// including INT64_MIN, whose negation is not representable, and any
	// codeSize. Nothing here may overflow, shift out of range, or be undefined,
	// and a success must be usable as an index.
	const std::uint64_t base = nondet_u64();
	const std::int64_t  delta = nondet_i64();
	const std::uint64_t codeSize = nondet_u64();

	std::uint64_t target = nondet_u64();
	if (relative(base, delta, codeSize, target)) {
		assert(target < codeSize);
		// Direction is preserved: a forward branch cannot land behind the
		// instruction and a backward branch cannot land ahead of it. This is
		// the property that fails at jvm_lifter.cpp:275, where a negative
		// offset from pc = 0 wraps forward to 0xFFFFFFFFFFFFFFFF.
		if (delta >= 0) assert(target >= base);
		else            assert(target < base);
	}
}

extern "C" void proof_relative_at_int64_min_does_not_negate()
{
	// The single input a plain `-delta` gets wrong. INT64_MIN means a branch
	// 2^63 bytes backwards, which no base under 2^63 can satisfy, and every
	// base at or above it lands at base - 2^63.
	const std::uint64_t base = nondet_u64();
	const std::uint64_t codeSize = nondet_u64();
	std::uint64_t target = nondet_u64();

	// Written as a literal rather than `uint64_t{1} << 63`: ESBMC 8.5.0 applies
	// its shl-overflow check to unsigned shifts too and reports
	// !overflow("shl", 1, 63) as a violation, which C++ does not agree with.
	// The constant is 2^63, the magnitude of INT64_MIN.
	const std::uint64_t twoPow63 = 0x8000000000000000ull;
	const bool ok = relative(base, INT64_MIN, codeSize, target);

	assert(ok == (base >= twoPow63 && (base - twoPow63) < codeSize));
	if (ok) assert(target == base - twoPow63);
}

extern "C" void proof_relative_with_zero_delta_is_the_position_itself()
{
	const std::uint64_t base = nondet_u64();
	const std::uint64_t codeSize = nondet_u64();
	std::uint64_t target = nondet_u64();

	const bool ok = relative(base, 0, codeSize, target);
	assert(ok == (base < codeSize));
	if (ok) assert(target == base);
}

// ─── absolute, and its agreement with relative ───────────────────────────────

extern "C" void proof_absolute_accepts_exactly_the_offsets_inside_the_code()
{
	const std::uint64_t t = nondet_u64();
	const std::uint64_t codeSize = nondet_u64();
	std::uint64_t target = nondet_u64();

	const bool ok = absolute(t, codeSize, target);
	assert(ok == (t < codeSize));
	if (ok) assert(target == t);
}

extern "C" void proof_relative_and_absolute_agree()
{
	// Property 6. A lifter must not get a different verdict depending on which
	// form of the opcode it met -- pyc_reader.cpp:535-543 takes both branches
	// in one switch, absolute before Python 3.11 and relative after.
	const std::uint64_t base = nondet_u64();
	const std::int64_t  delta = nondet_i64();
	const std::uint64_t codeSize = nondet_u64();

	std::uint64_t t = nondet_u64();
	if (relative(base, delta, codeSize, t)) {
		std::uint64_t t2 = nondet_u64();
		assert(absolute(t, codeSize, t2));
		assert(t2 == t);
	}
}

// ─── the out parameter is written only on success ────────────────────────────

extern "C" void proof_refusal_never_writes_the_target()
{
	// Property 3, and the one that makes the rest of this useful: a caller that
	// drops the return value still cannot record an out-of-range leader,
	// because there is nothing new in the variable to record. Both lifters
	// today compute the target unconditionally and hand it straight to
	// addBlock()/leaders.insert().
	const std::uint64_t sentinel = nondet_u64();

	std::uint64_t target = sentinel;
	if (!relative(nondet_u64(), nondet_i64(), nondet_u64(), target))
		assert(target == sentinel);

	std::uint64_t target2 = sentinel;
	if (!absolute(nondet_u64(), nondet_u64(), target2))
		assert(target2 == sentinel);
}

extern "C" void proof_region_refusal_never_writes_the_end()
{
	const std::uint64_t sentinel = nondet_u64();
	std::uint64_t end = sentinel;
	if (!region(nondet_u64(), nondet_u64(), nondet_u64(), end))
		assert(end == sentinel);
}

// The same property for tableEnd is proved inside tableEndFitsAtWidth<W>
// below, not here. Stated here it needs a symbolic width, and a symbolic width
// over even {2, 4, 8} puts the division inside bounds::mulFits and the 64-bit
// multiplication into one query: that version passed in 2s twice and then timed
// out at 300s on a third run. That is not a proof, it is luck.

// ─── region ──────────────────────────────────────────────────────────────────

extern "C" void proof_region_refuses_unless_the_range_fits()
{
	// Property 4, over the whole 64-bit domain: start, len and codeSize each
	// unconstrained, because tryOffset and tryLength are raw uint32 fields at
	// cil_lifter.cpp:363-375 and nothing between there and :779 bounds them.
	const std::uint64_t start = nondet_u64();
	const std::uint64_t len = nondet_u64();
	const std::uint64_t codeSize = nondet_u64();

	std::uint64_t end = nondet_u64();
	const bool ok = region(start, len, codeSize, end);

	assert(ok == retdec::utils::bounds::rangeFits(
		static_cast<std::size_t>(start),
		static_cast<std::size_t>(codeSize),
		static_cast<std::size_t>(len)));

	if (ok) {
		// A region never ends before it begins. tryOffset = 0xFFFFFFFF with
		// tryLength = 2 gives endOffset = 1 at cil_lifter.cpp:779, and the
		// consumers of BcExceptionHandler compute end - start as 0xFFFFFFFE.
		assert(end >= start);
		assert(end <= codeSize);
		assert(end - start == len);
	}
}

// ─── tableEnd ────────────────────────────────────────────────────────────────

/// Property 5 for one entry width, which must be a constant.
///
/// A query holding both a symbolic 64-bit multiplication and the symbolic
/// division inside bounds::mulFits is nonlinear bitvector arithmetic, and z3
/// does not discharge it: with `width` left symbolic over just {2, 4, 8} this
/// same property timed out at 300s. With the width fixed, `SIZE_MAX / width`
/// folds to a constant and the whole query lands in 2s. pos, n and codeSize
/// stay fully symbolic over the entire 64-bit range in each instance.
template <std::uint64_t W>
static void tableEndFitsAtWidth()
{
	const std::uint64_t pos = nondet_u64();
	const std::uint64_t n = nondet_u64();
	const std::uint64_t codeSize = nondet_u64();

	std::uint64_t end = nondet_u64();
	const std::uint64_t sentinel = end;
	const bool ok = tableEnd(pos, n, W, codeSize, end);

	// Property 3 for tableEnd: on refusal the end is not written, so a caller
	// that ignores the verdict cannot measure a switch's case targets from a
	// base outside the method.
	if (!ok) assert(end == sentinel);

	// Stated in two steps because the product may not exist: n * W is formed
	// only under mulOk, which is the whole point of the first test.
	const bool mulOk = retdec::utils::bounds::mulFits(
		static_cast<std::size_t>(n), static_cast<std::size_t>(W));
	bool truth = false;
	if (mulOk)
		truth = retdec::utils::bounds::rangeFits(
			static_cast<std::size_t>(pos),
			static_cast<std::size_t>(codeSize),
			static_cast<std::size_t>(n * W));
	assert(ok == truth);

	if (ok) {
		// The base every case target of this switch is measured from is inside
		// the method. cil_lifter.cpp:549 truncates it to uint32 instead, so a
		// count of 2^30 puts afterSwitch back near zero and every case label of
		// that switch is resolved against the wrong base.
		assert(end <= codeSize);
		assert(end >= pos);
		assert(end - pos == n * W);
	}
}

/// 16-bit code units: a DEX packed-switch target array is one unit per entry
/// and a sparse-switch key/target pair is two, both counted in units at
/// dex_lifter.cpp:235,238.
extern "C" void proof_table_end_fits_at_width_2() { tableEndFitsAtWidth<2>(); }

/// A CIL InlineSwitch label (cil_lifter.cpp:550 forms n * 4) and a JVM
/// tableswitch jump-offset entry (jvm_lifter.cpp:278).
extern "C" void proof_table_end_fits_at_width_4() { tableEndFitsAtWidth<4>(); }

/// A JVM lookupswitch match/offset pair, jvm_lifter.cpp:294.
extern "C" void proof_table_end_fits_at_width_8() { tableEndFitsAtWidth<8>(); }

extern "C" void proof_table_end_rejects_the_widest_jvm_tableswitch()
{
	// jvm_lifter.cpp:277 widens `hi - lo + 1` to int64 for exactly this pair:
	// (lo, hi) = (INT32_MIN, INT32_MAX) declares 2^32 entries, which is what
	// the int32 spelling would have wrapped to 0. Four bytes each is 2^34
	// bytes of table, and no method under 2^32 bytes can hold it.
	const std::int64_t lo = INT32_MIN;
	const std::int64_t hi = INT32_MAX;
	const std::int64_t declared = hi - lo + 1;
	assert(declared == (static_cast<std::int64_t>(1) << 32));

	const std::uint64_t pos = nondet_u64();
	std::uint64_t codeSize = nondet_u64();
	__ESBMC_assume(codeSize <= kMaxCodeSize);

	std::uint64_t end = nondet_u64();
	const std::uint64_t sentinel = end;
	assert(!tableEnd(pos, static_cast<std::uint64_t>(declared), 4, codeSize, end));
	assert(end == sentinel);
}

extern "C" void proof_table_end_with_no_entries_is_the_position()
{
	// A switch with an empty table still has an end, and it is where the table
	// would have started. Refusing here would lose the fall-through leader.
	// The width may stay symbolic here: n is the constant 0, so mulFits short
	// circuits on `a == 0` before reaching the division and no product is
	// formed. This is the one shape where a symbolic width is cheap.
	const std::uint64_t pos = nondet_u64();
	const std::uint64_t codeSize = nondet_u64();
	std::uint64_t width = nondet_u64();
	__ESBMC_assume(width == 2 || width == 4 || width == 8);

	std::uint64_t end = nondet_u64();
	const bool ok = tableEnd(pos, 0, width, codeSize, end);
	assert(ok == (pos <= codeSize));
	if (ok) assert(end == pos);
}
