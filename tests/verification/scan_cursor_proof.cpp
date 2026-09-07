/**
 * @file tests/verification/scan_cursor_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/scan_cursor.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * bounds_proof.cpp proves that a read fits. This file proves that a walk
 * finishes: that an accepted step is always a real step, that a refused one
 * leaves the cursor exactly where the caller left it, and that the number of
 * steps a walk can take is bounded by the bytes it has -- for every starting
 * cursor and every sequence of steps a file could name.
 *
 * The termination proofs contain loops, so they are bounded searches rather
 * than unbounded proofs, and the bound is stated where it is used. Everything
 * else here is loop-free and holds over the whole 64-bit domain.
 */

// ESBMC-OPTIONS: --unwind 8
//
// The longest loop in this file is the kFreeWalkSteps walk in
// proof_a_walk_takes_at_most_left_steps and its table-driven twin, which is
// four iterations (see kFreeWalkSteps below, and the paragraph there on why a
// bounded walk is enough once the induction is proved separately). Four plus
// the unwinding assertion would need 6; the bound is 8 so that raising
// kFreeWalkSteps by one does not silently need a second edit here. Unwinding assertions are on
// by default in ESBMC 8.5.0, so a bound that is too small fails loudly instead
// of silently truncating the search -- which is what makes the walk length a
// stated limitation rather than a hidden one.

// ESBMC-SOLVER: --z3
//
// Pinned for the same tool defect bounds_proof.cpp pins for. maxSteps divides
// left(c) by the step width, and bounds::countFits (reached through
// countFitsAt) divides by the element width. Unsigned division cannot overflow
// in C++ -- the only overflowing division is signed INT_MIN / -1 -- but ESBMC
// emits its div-overflow check regardless of signedness and the bitvector
// backends discharge it as SAT, returning that signed pair as the witness. z3
// does not. Measured here, not assumed: under --boolector,
// proof_max_steps_is_a_termination_bound fails with "arithmetic overflow on
// div" at scan_cursor.h:169 (the `left(c) / step` in maxSteps) while the same
// query passes under z3.
//
// The false alarm is in the safe direction -- it can invent a bug, not hide
// one -- but a verdict has to say which solver produced it.

#include "retdec/utils/scan_cursor.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::scan;
namespace bounds = retdec::utils::bounds;

extern "C" {
std::size_t nondet_size();
std::uint8_t nondet_u8();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

/// Steps in the bounded walks below. Six is enough to exhaust a buffer several
/// times over at the smallest step this header permits (1 byte), so the
/// property being checked -- accepted steps <= bytes available -- is exercised
/// on both sides of its bound rather than only where it is slack.
static const std::size_t kWalkSteps = 6;

/// Steps in the walk whose step sizes are unconstrained 64-bit values.
///
/// Shorter than kWalkSteps, and measured rather than guessed: under z3 that
/// walk discharges in 7s at two steps, 35s at three and 93s at four, and does
/// not finish inside 300s at six. Six chained symbolic 64-bit additions with a
/// symbolic acceptance counter is simply a harder query than six table-bounded
/// ones (kTableSize entries of one byte each), which do discharge at six.
///
/// The bound costs nothing, because the concrete walk is not what proves
/// termination: proof_the_walk_bound_is_inductive below does, loop-free and
/// for every walk length. This one checks the induction against the code path
/// a caller actually executes.
static const std::size_t kFreeWalkSteps = 4;

/// Entries in the symbolic step tables. The table is unconstrained, so this is
/// every 4-entry table; the loop in tableAlwaysAdvances is structurally
/// identical at 256 entries, which is the size the JVM and DEX tables use.
static const std::size_t kTableSize = 4;

/// A cursor with no constraint at all -- pos and size independent, so this
/// covers the invalid ones a caller could construct by hand as well.
static Cursor nondetCursor()
{
	Cursor c;
	c.pos = nondet_size();
	c.size = nondet_size();
	return c;
}

// ─── Property 1: a step of zero is refused ───────────────────────────────────

// The single assertion that would have caught dex_lifter.cpp:312 (the "([L)V"
// descriptor whose cursor resets to 0 and emits 65535 parameters),
// byte_value_storage.cpp:1077 at width == 0, and jvm_lifter.cpp:234 at any
// opcode whose table entry is 0.
extern "C" void proof_a_zero_step_is_refused_and_changes_nothing()
{
	Cursor c = nondetCursor();
	const Cursor before = c;

	assert(!advance(c, 0));
	assert(c.pos == before.pos);
	assert(c.size == before.size);
}

// The same claim reached from the other side: nothing about the cursor can
// make a zero step acceptable.
extern "C" void proof_no_cursor_accepts_a_zero_step()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	assert(!advance(c, 0));
}

// ─── Property 2: validity is preserved ───────────────────────────────────────

extern "C" void proof_an_accepted_advance_keeps_the_cursor_valid()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const std::size_t step = nondet_size();

	if (advance(c, step)) assert(valid(c));
}

extern "C" void proof_an_accepted_seek_keeps_the_cursor_valid()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const std::size_t to = nondet_size();

	if (seek(c, to))
	{
		assert(valid(c));
		assert(c.pos == to);
	}
}

// Validity survives an arbitrary interleaving, not merely one call at a time.
// The moves are chosen symbolically, so this covers every sequence of six.
extern "C" void proof_any_sequence_of_moves_keeps_the_cursor_valid()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));

	for (std::size_t i = 0; i < kWalkSteps; ++i)
	{
		if (nondet_size() & 1u)
			advance(c, nondet_size());
		else
			seek(c, nondet_size());
		// Holds after every move, accepted or refused -- not only at the end.
		assert(valid(c));
	}
}

// The sum `c.pos + step` is never formed, so a step that could not even be
// added is refused rather than wrapping into a small one. This is the shape
// BinaryReader::check has at jvm_const_pool.cpp:16 (`pos_ + n > size_`) and
// DexReader::check does not (dex_header.cpp:34, written as a subtraction).
extern "C" void proof_advance_refuses_every_step_that_would_wrap()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const std::size_t step = nondet_size();
	__ESBMC_assume(!bounds::addFits(c.pos, step));

	assert(!advance(c, step));
}

// ─── Property 3: accepted means strictly forward, refused means inert ────────

extern "C" void proof_an_accepted_advance_moves_strictly_forward()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const Cursor before = c;
	const std::size_t step = nondet_size();

	if (advance(c, step))
	{
		assert(c.pos > before.pos);
		// Exactly the step, and stated by subtraction so the proof does not
		// form the sum the kernel refuses to form.
		assert(c.pos - before.pos == step);
		assert(c.size == before.size);
	}
}

// The convention leb128.h established and cli_sig.cpp:86 breaks:
// readTypeDefOrRef decodes a compressed integer, advances `pos` past it, then
// returns an empty MetadataToken when the two-bit tag is 3. The caller gets a
// refusal that has already moved the cursor and cannot tell it from a token.
extern "C" void proof_a_refused_advance_leaves_the_cursor_bitwise_unchanged()
{
	Cursor c = nondetCursor();
	const Cursor before = c;
	const std::size_t step = nondet_size();

	if (!advance(c, step))
	{
		assert(c.pos == before.pos);
		assert(c.size == before.size);
	}
}

extern "C" void proof_a_refused_seek_leaves_the_cursor_bitwise_unchanged()
{
	Cursor c = nondetCursor();
	const Cursor before = c;
	const std::size_t to = nondet_size();

	if (!seek(c, to))
	{
		assert(c.pos == before.pos);
		assert(c.size == before.size);
		// And it only ever refuses a target outside the buffer.
		assert(to > before.size);
	}
}

// Completeness: a step the buffer really can supply is never refused, or the
// kernel would stop well-formed input mid-file.
extern "C" void proof_advance_accepts_every_step_the_buffer_can_supply()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const std::size_t step = nondet_size();
	__ESBMC_assume(step > 0);
	__ESBMC_assume(step <= left(c));

	assert(advance(c, step));
}

// ─── Property 4: a walk driven by advance terminates ─────────────────────────

// After k accepted advances the cursor has moved at least k bytes, and it can
// never pass the end, so k <= left(c0). That is the bound the CIL section walk
// at cil_lifter.cpp:322-409 currently gets by accident: its only guaranteed
// progress is the `sectStart += 4` that sits outside the branch meant to
// advance, and the branch that is meant to (`sectStart += dataSize`) can add
// zero because dataSize is one byte out of the file.
//
// The termination argument itself is stated inductively, in
// proof_the_walk_bound_holds_initially and proof_the_walk_bound_is_inductive
// below. Those two are loop-free, so they hold for a walk of ANY length -- no
// unwinding bound applies to them at all. This one runs the real code path for
// kFreeWalkSteps attempts as a check on the induction: the steps and the
// starting cursor are fully symbolic over the whole 64-bit range.
extern "C" void proof_a_walk_takes_at_most_left_steps()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const Cursor c0 = c;

	std::size_t accepted = 0;
	for (std::size_t i = 0; i < kFreeWalkSteps; ++i)
		if (advance(c, nondet_size())) ++accepted;

	assert(valid(c));
	assert(c.pos >= c0.pos);
	// Monotone progress: each acceptance was worth at least one byte. Written
	// as a subtraction because `c0.pos + accepted` is a sum of two values the
	// file influences.
	assert(c.pos - c0.pos >= accepted);
	// Therefore the walk cannot run longer than the buffer is.
	assert(accepted <= left(c0));
	// `accepted <= maxSteps(c0, 1)` follows by transitivity from
	// proof_max_steps_is_a_termination_bound, which shows
	// maxSteps(c0, 1) == left(c0). It is not asserted here: putting the
	// symbolic 64-bit division from maxSteps into the same query as six
	// symbolic 64-bit steps timed out z3 at 300s, and splitting it out is the
	// decomposition, not a weakening -- the two halves together are the whole
	// claim.
}

// The termination bound, without a walk length at all.
//
// The invariant a walk maintains is: the cursor has moved at least as many
// bytes as it has taken steps, and it started where it started. Base case --
// the walk has taken no steps and has not moved -- and step case, below,
// together give `accepted <= left(c0)` for every walk of every length. Neither
// contains a loop, so neither is bounded by --unwind: this is the unbounded
// half of property 4, and the concrete walk above is the bounded check on it.
extern "C" void proof_the_walk_bound_holds_initially()
{
	Cursor c0 = nondetCursor();
	__ESBMC_assume(valid(c0));
	const Cursor c = c0;
	const std::size_t accepted = 0;

	assert(c.pos >= c0.pos);
	assert(c.pos - c0.pos >= accepted);
	assert(accepted <= left(c0));
}

extern "C" void proof_the_walk_bound_is_inductive()
{
	const Cursor c0 = nondetCursor();
	__ESBMC_assume(valid(c0));

	// An arbitrary state satisfying the invariant, not one reached by any
	// particular sequence -- which is what makes this cover all of them.
	Cursor c = nondetCursor();
	std::size_t accepted = nondet_size();
	__ESBMC_assume(valid(c));
	__ESBMC_assume(c.size == c0.size);
	__ESBMC_assume(c.pos >= c0.pos);
	__ESBMC_assume(c.pos - c0.pos >= accepted);

	// The invariant already implies the bound, before the step is taken.
	assert(accepted <= left(c0));

	if (advance(c, nondet_size())) ++accepted;

	// ...and still does after it. `accepted` cannot wrap here: reaching
	// accepted == SIZE_MAX needs c.pos - c0.pos == SIZE_MAX, hence
	// c.pos == c.size == SIZE_MAX, where left(c) is 0 and advance refuses.
	// --unsigned-overflow-check is what establishes that rather than my
	// saying it.
	assert(valid(c));
	assert(c.size == c0.size);
	assert(c.pos >= c0.pos);
	assert(c.pos - c0.pos >= accepted);
	assert(accepted <= left(c0));
}

// An empty tail admits no steps at all, so a walk that has reached the end
// stops there rather than spinning.
extern "C" void proof_a_walk_at_the_end_takes_no_steps()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	__ESBMC_assume(c.pos == c.size);

	for (std::size_t i = 0; i < kWalkSteps; ++i)
		assert(!advance(c, nondet_size()));
}

// ─── Property 5: countFitsAt is countFits on the cursor ──────────────────────

extern "C" void proof_count_fits_at_is_exactly_bounds_count_fits()
{
	const Cursor c = nondetCursor();
	const std::size_t count = nondet_size();
	const std::size_t width = nondet_size();

	assert(countFitsAt(c, count, width) == bounds::countFits(c.pos, c.size, count, width));
	// The default argument is the tagged-format case: one byte per element.
	assert(countFitsAt(c, count) == bounds::countFits(c.pos, c.size, count, 1));
}

// The composition, restated on the cursor: an accepted count really can be
// read. Proved per element width, because a symbolic width would put a 64-bit
// multiply and a 64-bit divide in one query -- the nonlinear case no solver
// here discharges. Position, size and count stay symbolic across the full
// range.
template <std::size_t Width>
static void countFitsAtComposesWithFits()
{
	Cursor c = nondetCursor();
	const std::size_t count = nondet_size();

	if (countFitsAt(c, count, Width))
	{
		assert(bounds::mulFits(count, Width));
		assert(fits(c, count * Width));
		assert(count * Width <= left(c));
		// And the elements can actually be walked over.
		assert(count <= maxSteps(c, Width));
	}
}

// 1 is a tag byte; 12 and 24 are the small and fat CIL exception clauses
// (cil_lifter.cpp:341, 349); 16 is a DEX map-list entry.
extern "C" void proof_count_fits_at_width1()
{
	countFitsAtComposesWithFits<1>();
}
extern "C" void proof_count_fits_at_width12()
{
	countFitsAtComposesWithFits<12>();
}
extern "C" void proof_count_fits_at_width16()
{
	countFitsAtComposesWithFits<16>();
}
extern "C" void proof_count_fits_at_width24()
{
	countFitsAtComposesWithFits<24>();
}

// The bound is derived from the input, not chosen: an accepted count can never
// exceed the bytes left. This is what cli_sig.cpp:126 does not do -- a blob of
// five bytes whose second compressed integer is 0x1FFFFFFF drives 536870911
// iterations, each pushing an int32 into `sizes`, which is a 2 GB vector.
extern "C" void proof_an_accepted_count_never_exceeds_the_bytes_left()
{
	const Cursor c = nondetCursor();
	const std::size_t count = nondet_size();
	const std::size_t width = nondet_size();

	if (countFitsAt(c, count, width)) assert(count <= left(c));
}

// ─── Property 6: maxSteps never divides by zero, and is never vacuous ────────

extern "C" void proof_max_steps_treats_zero_width_as_one()
{
	const Cursor c = nondetCursor();

	// A hostile header declaring zero-width elements must not turn the bound
	// off, and must not trap. Reaching this assertion at all is the proof that
	// the division is guarded, since --overflow-check covers division by zero.
	assert(maxSteps(c, 0) == maxSteps(c, 1));
	assert(maxSteps(c, 0) == left(c));
}

extern "C" void proof_max_steps_is_a_termination_bound()
{
	const Cursor c = nondetCursor();
	const std::size_t width = nondet_size();
	const std::size_t n = maxSteps(c, width);

	// Never claims more steps than there are bytes, whatever the width.
	assert(n <= left(c));
	// At width 1 -- the smallest step advance() permits -- the bound is exactly
	// the bytes left, which is the number quoted in property 4.
	assert(maxSteps(c, 1) == left(c));
}

// ─── Property 7: alignForward ────────────────────────────────────────────────

extern "C" void proof_align_forward_never_decreases_the_cursor()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const Cursor before = c;
	const std::size_t a = nondet_size();

	alignForward(c, a);

	// The property the CIL walk depends on and does not have: at
	// cil_lifter.cpp:326 and 408, `(sectStart + 3) & ~3ULL` wraps for a
	// position within three bytes of SIZE_MAX and rounds *down* to 0.
	assert(c.pos >= before.pos);
	assert(c.size == before.size);
	assert(valid(c));
}

extern "C" void proof_align_forward_lands_on_a_multiple()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const Cursor before = c;
	const std::size_t a = nondet_size();
	__ESBMC_assume(isPowerOfTwo(a));

	if (alignForward(c, a))
	{
		// a is a power of two, so a - 1 is its mask and this says c.pos is a
		// multiple of a. Written with the mask rather than %, which would put
		// a second symbolic division in the query.
		assert((c.pos & (a - 1)) == 0);
		// Minimal: it rounds up to the *next* multiple, never further.
		assert(c.pos - before.pos < a);
	}
}

extern "C" void proof_align_forward_refuses_a_non_power_of_two()
{
	Cursor c = nondetCursor();
	const Cursor before = c;
	const std::size_t a = nondet_size();
	__ESBMC_assume(!isPowerOfTwo(a));

	// Including a == 0: `value & ~(a - 1)` is meaningless there, and the
	// callers take their alignment from the file.
	assert(!alignForward(c, a));
	assert(c.pos == before.pos);
	assert(c.size == before.size);
}

extern "C" void proof_align_forward_refuses_to_leave_the_buffer()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const Cursor before = c;
	const std::size_t a = nondet_size();

	if (!alignForward(c, a))
	{
		assert(c.pos == before.pos);
		assert(c.size == before.size);
	}
	else
	{
		// An accepted alignment leaves a readable cursor, so the caller may go
		// on to read at it.
		assert(valid(c));
	}
}

extern "C" void proof_align_forward_is_idempotent()
{
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const std::size_t a = nondet_size();
	__ESBMC_assume(isPowerOfTwo(a));

	if (alignForward(c, a))
	{
		const Cursor once = c;
		// A second alignment must be a no-op, or a walk that aligns on every
		// iteration would drift.
		assert(alignForward(c, a));
		assert(c.pos == once.pos);
	}
}

// ─── Property 8: the step table cannot stall a walk ──────────────────────────

// The call-site obligation, proved over the table rather than over the cursor:
// whatever the table says, a step taken through it is either refused or is
// real progress. This is what makes the twenty-three wrong entries in the table at
// jvm_lifter.cpp:189 a decoding error rather than a hang -- and what makes
// jvm_lifter.cpp:343's `if (sz <= 0) sz = 1` unnecessary.
extern "C" void proof_a_table_step_is_progress_or_a_refusal()
{
	std::uint8_t table[kTableSize];
	for (std::size_t i = 0; i < kTableSize; ++i)
		table[i] = nondet_u8();

	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const Cursor before = c;
	const std::size_t key = nondet_size();

	if (advanceByTable(c, table, kTableSize, key))
	{
		assert(c.pos > before.pos);
		assert(valid(c));
		// The step really was the table's entry, so the walk stays in step
		// with the format.
		assert(c.pos - before.pos == stepFromTable(table, kTableSize, key));
	}
	else
	{
		assert(c.pos == before.pos);
		assert(c.size == before.size);
	}
}

// A key outside the table -- an undefined JVM opcode in 0xCA..0xFF -- is a
// refusal, not an out-of-bounds read. ESBMC's array-bounds checking carries
// this one: an implementation that indexed anyway would fail here rather than
// merely disagree.
extern "C" void proof_a_key_outside_the_table_is_a_refusal()
{
	std::uint8_t table[kTableSize];
	for (std::size_t i = 0; i < kTableSize; ++i)
		table[i] = nondet_u8();

	Cursor c = nondetCursor();
	const Cursor before = c;
	const std::size_t key = nondet_size();
	__ESBMC_assume(key >= kTableSize);

	assert(stepFromTable(table, kTableSize, key) == 0);
	assert(!advanceByTable(c, table, kTableSize, key));
	assert(c.pos == before.pos);
}

extern "C" void proof_a_null_table_is_a_refusal()
{
	Cursor c = nondetCursor();
	const Cursor before = c;
	assert(stepFromTable(nullptr, nondet_size(), nondet_size()) == 0);
	assert(!advanceByTable(c, nullptr, nondet_size(), nondet_size()));
	assert(c.pos == before.pos);
}

// The termination bound of property 4, carried through the table-driven form:
// a bytecode walk terminates for EVERY table, including a wrong one.
extern "C" void proof_a_table_driven_walk_takes_at_most_left_steps()
{
	std::uint8_t table[kTableSize];
	for (std::size_t i = 0; i < kTableSize; ++i)
		table[i] = nondet_u8();

	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const Cursor c0 = c;

	std::size_t accepted = 0;
	for (std::size_t i = 0; i < kWalkSteps; ++i)
		if (advanceByTable(c, table, kTableSize, nondet_size())) ++accepted;

	assert(c.pos - c0.pos >= accepted);
	assert(accepted <= left(c0));
}

// tableAlwaysAdvances is exact, so a call site can assert its own table is a
// step table instead of hoping. The claim it makes: with such a table, the
// only reason a step is refused is that the buffer ran out.
extern "C" void proof_a_table_that_always_advances_refuses_only_at_the_end()
{
	std::uint8_t table[kTableSize];
	for (std::size_t i = 0; i < kTableSize; ++i)
		table[i] = nondet_u8();
	__ESBMC_assume(tableAlwaysAdvances(table, kTableSize));

	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	const std::size_t key = nondet_size();
	__ESBMC_assume(key < kTableSize);

	const std::size_t step = stepFromTable(table, kTableSize, key);
	assert(step >= 1);
	if (!advanceByTable(c, table, kTableSize, key)) assert(step > left(c));
}

extern "C" void proof_an_empty_table_never_always_advances()
{
	// The audit's finding, turned into a property so it cannot come back.
	// tableAlwaysAdvances used to answer true for a count of 0, on the reading
	// that a table with no entries has no zero entry -- while advanceByTable
	// refuses every key at that count with the buffer still full, which is a
	// stall in the middle of a walk and is exactly what this predicate promises
	// does not happen. Stated for a table the solver chooses, so it is not a
	// claim about one array.
	std::uint8_t table[kTableSize];
	for (std::size_t i = 0; i < kTableSize; ++i)
		table[i] = nondet_u8();

	assert(!tableAlwaysAdvances(table, 0));
	assert(!tableAlwaysAdvances(nullptr, 0));
	assert(!tableAlwaysAdvances(nullptr, nondet_size()));

	// And the contract the answer is about: at count 0 every key really is
	// refused, with room left in the buffer.
	Cursor c = nondetCursor();
	__ESBMC_assume(valid(c));
	__ESBMC_assume(left(c) > 0);
	assert(!advanceByTable(c, table, 0, nondet_size()));
}

extern "C" void proof_a_table_with_a_zero_entry_is_reported()
{
	std::uint8_t table[kTableSize];
	for (std::size_t i = 0; i < kTableSize; ++i)
		table[i] = nondet_u8();

	const std::size_t k = nondet_size();
	__ESBMC_assume(k < kTableSize);
	__ESBMC_assume(table[k] == 0);

	// Sound in the direction that matters: a table with a hole never passes.
	assert(!tableAlwaysAdvances(table, kTableSize));
}
