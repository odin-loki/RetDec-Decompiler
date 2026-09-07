/**
 * @file tests/verification/align_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/align.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Alignment rounding is one line of arithmetic that this tree writes in three
 * spellings, and three of them are wrong. The properties below are the rule the
 * three disagree about, checked for ALL values of value and alignment over the
 * whole 64-bit domain by SMT rather than for sampled ones.
 *
 * Two of them are counterexamples turned into properties, so the mistakes
 * cannot come back:
 *
 *   - proof_align_up_never_moves_backwards. retdec::utils::alignUp in
 *     src/utils/alignment.cpp used to compute
 *     `alignDown(value + (alignment - 1), alignment)`. ESBMC returns
 *     value = 18446744073709551440, alignment = 512: the sum wraps to 335, the
 *     mask takes it to 256, and alignUp returns a value 2^64 smaller than its
 *     input.
 *
 *   - proof_align_up_saturating_is_monotone. src/cli_parser/pe_reader.cpp:284
 *     computes `(nameLen + 4) & ~3u`, whose mask is an unsigned int and so
 *     clears bits 32-63 as well as bits 0-1. ESBMC returns the pair
 *     m = 6917529027634356166, n = 18446744071562067948, where m < n but the
 *     expression maps m to 4288241608 and n to 2147483632. The legible member
 *     of the same family, run rather than solved: nameLen = 0x100000001 gives
 *     4 while nameLen = 0x10 gives 20, and the stream-header cursor at
 *     pe_reader.cpp:286 is computed from it.
 *
 * Both were re-derived independently as ESBMC probes against the old
 * expressions before these were written; the witnesses above are what the
 * solver returned.
 *
 * A third came out of writing these, against the kernel itself rather than
 * against the tree. alignUpSaturating first saturated at the largest a-aligned
 * std::size_t, so that its result was aligned unconditionally.
 * proof_align_up_saturating_rounds_up_and_saturates refuted `r >= n` at
 * n = 18446744073709551615, a = 4, where that gives 18446744073709551612 --
 * three bytes below the input, which is the backwards step the whole header
 * exists to stop. It saturates at SIZE_MAX now; see the note on that proof.
 *
 * Run with scripts/verify_esbmc.sh. Each entry point is verified separately
 * (--function), so a failure names the property that broke.
 */

// ESBMC-OPTIONS: --unwind 65
// ESBMC-SOLVER: --boolector
//
// The unwind bound is the reference popcount below, which visits each of the 64
// bits of a std::uint64_t exactly once. 65 is one more than the loop can run,
// so ESBMC's unwinding assertion -- on by default in 8.5.0 -- is discharged
// rather than assumed, and the bound is part of the proof. It is the only loop
// in the file; every other proof is straight-line.
//
// Boolector, and the difference is not marginal. Three proofs state their
// property against `v % a` rather than against a re-spelling of the mask the
// implementation uses, because `(v & (a - 1)) == v % a` for a power-of-two a is
// the fact being checked and a property that assumed it would check nothing.
// Symbolic 64-bit modulus is where the backends part company, measured on this
// file: proof_is_aligned_to_is_exact_for_a_real_alignment takes 8.7s under
// boolector and does not finish in 300s under z3;
// proof_align_up_saturating_rounds_up_and_saturates takes 26s against 186s.
// (The popcount pair is not the expensive part -- 0.5s against 7.8s -- which is
// the opposite of what was assumed before it was timed.)
//
// The div-overflow false alarm that pins bounds_proof.cpp to z3 does not arise
// here, and the reason is specific rather than lucky. ESBMC emits its
// "arithmetic overflow on div" check for unsigned division too, and boolector
// discharges it with the signed INT64_MIN / -1 pair -- divisor
// 0xFFFFFFFFFFFFFFFF. Every modulus in this file has `isPowerOfTwo(a)` assumed
// over it, and 0xFFFFFFFFFFFFFFFF is not a power of two, so the witness is
// assumed away. Measured on a standalone probe: the same modulus with that
// assumption passes 8/8 under boolector in 9.9s and times out under z3 at 250s.
// Nothing else in align.h divides -- padTo, alignUp, alignDown, isAlignedTo and
// alignUpSaturating are &, -, + and comparisons, and the only bounds.h function
// reached is addFits, a subtraction.
//
// `scripts/verify_esbmc.sh --cross` runs both backends over this file anyway
// and reports any disagreement as a finding rather than a preference.

#include "retdec/utils/align.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::align;

// ESBMC treats an undefined function returning a value as an unconstrained
// choice of that type.
extern "C" {
std::uint64_t nondet_u64();
std::int64_t nondet_int64();
std::size_t nondet_size();
}

// __ESBMC_assume is a verifier builtin, so this file does not type-check under
// a plain compiler without it. --syntax defines this away.
#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

/// Reference population count, defined the long way round.
///
/// This is the definition math.h's countBits is supposed to implement, written
/// so that it shares no arithmetic with isPowerOfTwo: it tests one bit at a
/// time with a shift that is always in range, and never forms `n - 1`. A
/// property comparing isPowerOfTwo against a re-spelling of `n & (n - 1)` would
/// prove nothing.
static unsigned bitCount(std::uint64_t n)
{
	unsigned c = 0;
	for (unsigned i = 0; i < 64; ++i)
		if ((n >> i) & 1) ++c;
	return c;
}

// ─── isPowerOfTwo ────────────────────────────────────────────────────────────

extern "C" void proof_is_power_of_two_is_exactly_one_bit_set()
{
	const std::uint64_t n = nondet_u64();
	assert(isPowerOfTwo(n) == (bitCount(n) == 1));
}

extern "C" void proof_is_power_of_two_or_zero_is_at_most_one_bit_set()
{
	const std::uint64_t n = nondet_u64();
	assert(isPowerOfTwoOrZero(n) == (bitCount(n) <= 1));
}

extern "C" void proof_signed_is_power_of_two_is_total()
{
	// The whole point: this must terminate with an answer for every int64,
	// INT64_MIN included, and form no signed overflow doing it. math.h:22
	// computes `number - 1` in the argument's type, so at INT64_MIN it is
	// undefined behaviour -- and --overflow-check is on for every proof here,
	// so a version that did that would fail this line rather than the assert.
	const std::int64_t n = nondet_int64();
	const bool r = isPowerOfTwoSigned(n);

	// -2^63 has a single bit set in its two's-complement representation, which
	// is why the unsigned reading of it slips through. It is not a power of
	// two: an alignment of -9223372036854775808 is not an alignment.
	if (n <= 0)
		assert(!r);
	else
		assert(r == (bitCount(static_cast<std::uint64_t>(n)) == 1));

	assert(isPowerOfTwoOrZeroSigned(n) == (n == 0 || r));
}

// ─── alignUp ─────────────────────────────────────────────────────────────────

extern "C" void proof_align_up_succeeds_exactly_when_the_rounded_value_fits()
{
	// Soundness AND completeness in one statement: no well-formed rounding is
	// rejected and no wrapping one is accepted. The right-hand side is the
	// condition stated over the *inputs*, so it does not depend on how the
	// implementation decides -- and `UINT64_MAX - v` is a subtraction, so the
	// property does not form the sum it is claiming is representable.
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	std::uint64_t out = nondet_u64();

	const bool ok = alignUp(v, a, out);

	assert(ok == (isPowerOfTwo(a) && (a - 1) <= UINT64_MAX - v));
}

extern "C" void proof_align_up_is_the_least_aligned_value_at_or_above()
{
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	std::uint64_t out = nondet_u64();

	if (!alignUp(v, a, out)) return;

	assert(out >= v);             // never moves backwards
	assert(out - v < a);          // by less than one alignment, so it is the least
	assert((out & (a - 1)) == 0); // and it really is aligned
}

extern "C" void proof_align_up_refuses_a_zero_alignment()
{
	// The old alignment.cpp computed `alignment - 1` == UINT64_MAX and handed it to
	// alignDown, whose mask is then 0, so every value rounds to 0. A PE
	// FileAlignment of 0 is a field a file may simply contain.
	const std::uint64_t v = nondet_u64();
	std::uint64_t out = nondet_u64();

	assert(!alignUp(v, 0, out));
	assert(out == v); // and specifically NOT 0
}

extern "C" void proof_align_up_never_moves_backwards()
{
	// The property the tree's helper does not have, stated for both outcomes:
	// whatever alignUp does with an out-parameter, the value in it afterwards
	// is at least the value that went in. A caller that ignores the bool gets
	// an unrounded cursor, never one that has jumped to the start of the
	// buffer. ESBMC's witness against the old expression is
	// v = 18446744073709551614, a = 4, which it rounds to 0.
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	std::uint64_t out = nondet_u64();

	alignUp(v, a, out);
	assert(out >= v);
}

extern "C" void proof_align_up_agrees_with_pad_to()
{
	// The two entry points must not drift: padTo is what a caller uses when it
	// wants the gap (the DEX try_item pad), alignUp when it wants the position,
	// and a call site picking either must get the same answer.
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	std::uint64_t out = nondet_u64();

	const std::uint64_t pad = padTo(v, a);
	if (alignUp(v, a, out)) assert(out - v == pad);
}

// ─── alignDown ───────────────────────────────────────────────────────────────

extern "C" void proof_align_down_is_exact()
{
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	std::uint64_t out = nondet_u64();

	const bool ok = alignDown(v, a, out);

	// Rounding down cannot overflow, so a power-of-two alignment is the only
	// requirement -- there is no second failure mode to get wrong.
	assert(ok == isPowerOfTwo(a));
	if (!ok)
	{
		assert(out == v);
		return;
	}
	assert(out <= v);
	assert(v - out < a);
	assert((out & (a - 1)) == 0);
	// The floor stated as arithmetic rather than as a mask, so a wrong mask is
	// refuted rather than merely reproduced: out == v - (v mod a).
	assert(out == v - (v & (a - 1)));
}

extern "C" void proof_align_down_refuses_every_non_power_of_two()
{
	// `v & ~(a - 1)` with a = 3 has the mask ~2 == 0xFFFF...FD: it clears bit 1
	// and leaves bit 0, so 3 "rounds down" to 1. There is no alignment for
	// which that is the answer, so the only correct behaviour is to refuse.
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	__ESBMC_assume(!isPowerOfTwo(a));
	std::uint64_t out = nondet_u64();

	assert(!alignDown(v, a, out));
	assert(out == v);
}

// ─── isAlignedTo ─────────────────────────────────────────────────────────────

extern "C" void proof_is_aligned_to_is_exact_for_a_real_alignment()
{
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	__ESBMC_assume(isPowerOfTwo(a));
	std::uint64_t rem = nondet_u64();

	const bool aligned = isAlignedTo(v, a, rem);

	// a is a power of two, so `v & (a - 1)` is v mod a -- but the property is
	// stated against the modulus itself, computed independently, so a wrong
	// mask cannot satisfy it by agreeing with itself.
	assert(rem == v % a);
	assert(aligned == (rem == 0));
}

extern "C" void proof_is_aligned_to_refuses_a_zero_alignment()
{
	// The old alignment.cpp masked with `0 - 1` == UINT64_MAX, so remainder became v
	// and every non-zero value is reported unaligned -- an answer, not a
	// refusal. pe_format_parser.h:123 reports that as a header anomaly.
	const std::uint64_t v = nondet_u64();
	std::uint64_t rem = nondet_u64();

	assert(!isAlignedTo(v, 0, rem));
	assert(rem == v);
}

extern "C" void proof_is_aligned_to_refuses_every_non_power_of_two()
{
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	__ESBMC_assume(!isPowerOfTwo(a));
	std::uint64_t rem = nondet_u64();

	// False for a = 3 even when v is 3 -- which IS a multiple of 3. That is the
	// point: the function does not answer questions about alignments it cannot
	// compute with, it says so.
	assert(!isAlignedTo(v, a, rem));
	assert(rem == v);
}

// ─── padTo ───────────────────────────────────────────────────────────────────

extern "C" void proof_pad_to_is_the_distance_to_the_next_multiple()
{
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	__ESBMC_assume(isPowerOfTwo(a));

	const std::uint64_t pad = padTo(v, a);

	assert(pad < a);
	// v + pad is a multiple of a. Stated without forming v + pad, which is the
	// sum that need not be representable: pad is the additive inverse of the
	// remainder modulo a.
	const std::uint64_t rem = v % a;
	assert(pad == (rem == 0 ? 0 : a - rem));
}

extern "C" void proof_pad_to_is_zero_for_a_meaningless_alignment()
{
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	__ESBMC_assume(!isPowerOfTwo(a));

	// 0 is the only answer that leaves a cursor where it is. Returning `a - rem`
	// off a garbage mask would advance it by a garbage amount.
	assert(padTo(v, a) == 0);
	assert(padTo(v, 0) == 0);
}

// ─── alignUpSaturating ───────────────────────────────────────────────────────

extern "C" void proof_align_up_saturating_is_monotone()
{
	// The property that keeps a stream-header cursor moving forward, over the
	// whole size_t range and for every alignment, including the ones that are
	// not powers of two. `(nameLen + 4) & ~3u` does not have it: 0x100000001
	// maps to 4 and 0x10 maps to 0x14, so a larger input produces a smaller
	// answer and pe_reader.cpp:286 walks the table backwards.
	const std::size_t m = nondet_size();
	const std::size_t n = nondet_size();
	const std::size_t a = nondet_size();
	__ESBMC_assume(m <= n);

	assert(alignUpSaturating(m, a) <= alignUpSaturating(n, a));
}

extern "C" void proof_align_up_saturating_rounds_up_and_saturates()
{
	// Covers property 6 of the spec for every power-of-two alignment at once
	// rather than for the enumerated set {2, 4, 8, 16, 0x200, 0x1000} the
	// formats use -- there is no multiplication or division here, so the
	// symbolic alignment is tractable and strictly stronger. Those six are PE
	// FileAlignment/SectionAlignment, the 4 of the CLI stream-header and CIL
	// EH-section pads, and the 8/16 of the DEX map list.
	//
	// The spec asked for saturation at the largest a-aligned std::size_t. This
	// proof is why it does not say that: with `return n & ~(a - 1)` in the
	// saturating branch, ESBMC refutes `r >= n` at n = 18446744073709551615,
	// a = 4, where the answer is 18446744073709551612 -- three bytes below the
	// input. Unconditional alignment and never-backwards are incompatible at
	// the top of the range, because the next multiple is not representable. The
	// header keeps never-backwards and states alignment for the range where the
	// rounding exists, which is what the assertions below say.
	const std::size_t n = nondet_size();
	const std::size_t a = nondet_size();
	__ESBMC_assume(isPowerOfTwo(a));

	const std::size_t r = alignUpSaturating(n, a);

	assert(r >= n);    // never backwards, saturated or not
	assert(r - n < a); // and within one alignment of the input either way

	// alignUp succeeds exactly for n at or below the largest a-aligned
	// std::size_t -- that is proof_align_up_succeeds_exactly_when_the_rounded_
	// value_fits, restated at size_t width. Below it the answer is the true
	// rounding; above it the answer is SIZE_MAX, which no bounds check passes,
	// rather than a wrapped offset that looks readable.
	const std::size_t ceiling = SIZE_MAX & ~(a - 1);
	if (n <= ceiling)
	{
		assert((r & (a - 1)) == 0);
		assert(r == n + (n % a == 0 ? 0 : a - n % a));
	}
	else
	{
		assert(r == SIZE_MAX);
	}
}

extern "C" void proof_align_up_saturating_leaves_a_bad_alignment_alone()
{
	const std::size_t n = nondet_size();
	const std::size_t a = nondet_size();
	__ESBMC_assume(!isPowerOfTwo(a));

	// Including a == 0. alignment.cpp's alignUp returns 0 here for every input.
	assert(alignUpSaturating(n, a) == n);
	assert(alignUpSaturating(n, 0) == n);
}

extern "C" void proof_align_up_saturating_agrees_with_align_up_where_it_fits()
{
	// The saturating and the reporting entry point must give the same answer
	// wherever the reporting one succeeds, so a call site choosing between them
	// is choosing how to handle the top of the range and nothing else.
	const std::size_t n = nondet_size();
	const std::size_t a = nondet_size();
	std::uint64_t out = nondet_u64();

	if (alignUp(n, a, out)) assert(alignUpSaturating(n, a) == out);
}

// ─── the whole header, at once ───────────────────────────────────────────────

extern "C" void proof_no_arithmetic_in_the_header_wraps()
{
	// Property 7 of the spec. --overflow-check and --unsigned-overflow-check
	// are applied to every proof here, so this one carries no assertion of its
	// own: it exists to reach every function with fully unconstrained inputs so
	// that a wrap anywhere in the header is a violation. Every other proof
	// constrains its alignment one way or another; this one does not.
	const std::uint64_t v = nondet_u64();
	const std::uint64_t a = nondet_u64();
	std::uint64_t out = nondet_u64();
	std::uint64_t rem = nondet_u64();

	(void)isPowerOfTwo(a);
	(void)isPowerOfTwoOrZero(a);
	(void)isPowerOfTwoSigned(nondet_int64());
	(void)isPowerOfTwoOrZeroSigned(nondet_int64());
	(void)padTo(v, a);
	(void)alignUp(v, a, out);
	(void)alignDown(v, a, out);
	(void)isAlignedTo(v, a, rem);
	(void)alignUpSaturating(nondet_size(), nondet_size());
}

extern "C" void proof_no_arithmetic_wraps_at_the_extremes()
{
	// The corners the spec names, pinned rather than left to the solver to
	// find: v == UINT64_MAX, where `v + (a - 1)` wraps for every a > 1, and
	// a == 1, where `a - 1` is 0 and the mask is UINT64_MAX. Together with
	// a == 0 these are the three places every hand-written version breaks.
	std::uint64_t out = nondet_u64();
	std::uint64_t rem = nondet_u64();

	// a == 1: everything is aligned to 1, nothing moves, nothing wraps.
	assert(alignUp(UINT64_MAX, 1, out) && out == UINT64_MAX);
	assert(alignDown(UINT64_MAX, 1, out) && out == UINT64_MAX);
	assert(isAlignedTo(UINT64_MAX, 1, rem) && rem == 0);
	assert(padTo(UINT64_MAX, 1) == 0);

	// v == UINT64_MAX with a real alignment: the rounded value is 2^64 and is
	// not representable, so alignUp refuses and leaves the cursor put.
	const std::uint64_t a = nondet_u64();
	__ESBMC_assume(isPowerOfTwo(a) && a > 1);
	assert(!alignUp(UINT64_MAX, a, out));
	assert(out == UINT64_MAX);
	assert(alignDown(UINT64_MAX, a, out) && out == UINT64_MAX - (a - 1));
	assert(padTo(UINT64_MAX, a) == 1);
}
