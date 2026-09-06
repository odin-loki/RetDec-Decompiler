/**
 * @file tests/verification/bounds_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/bounds.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Every property below is checked for ALL values of its inputs, over the whole
 * 64-bit domain, by SMT — not for sampled values. There are no loops, so no
 * unwinding bound applies and the result is a proof rather than a bounded
 * search.
 *
 * Run with scripts/verify_esbmc.sh. Each entry point is verified separately
 * (--function), so a failure names the property that broke.
 *
 * One limit is worth stating plainly. A query containing BOTH a symbolic
 * 64-bit multiplication and a symbolic 64-bit division is nonlinear bitvector
 * arithmetic, and no solver here (Z3, Boolector, Bitwuzla) discharges it --
 * they run out of time, at 16 bits as readily as at 64. So the properties that
 * multiply a count by an element width are proved for each width that actually
 * occurs in these formats (1, 2, 4, 8, 16 bytes) rather than for a symbolic
 * width. Position, size and count stay fully symbolic over the whole 64-bit
 * range in every one of those. Where a property holds for symbolic width, it is
 * proved that way.
 *
 * These proofs cover the header the parsers actually call. That is the point:
 * a proof about a re-typed copy of the logic would say nothing about the code
 * that runs.
 */

#include "retdec/utils/bounds.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::bounds;

// ESBMC treats an undefined function returning a value as an unconstrained
// choice of that type.
extern "C" {
std::size_t nondet_size();
std::int64_t nondet_int64();
}

// __ESBMC_assume is a verifier builtin, so this file does not type-check under
// an ordinary compiler. Defining it away lets `verify_esbmc.sh --syntax` catch
// a typo in a second instead of after a solver run; ESBMC itself never sees
// the macro.
#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void) sizeof((cond) ? 1 : 0))
#endif

// ─── remaining ───────────────────────────────────────────────────────────────

extern "C" void proof_remaining()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t r = remaining(pos, size);

	// Never reports more than the buffer holds.
	assert(r <= size);
	// Non-zero only when there is genuinely something left.
	assert(r == 0 || pos < size);
	// Exact, not merely safe: pos + r is the end of the buffer.
	assert(pos >= size || pos + r == size);
	// Saturates instead of wrapping when the caller is already past the end.
	assert(pos < size || r == 0);
}

// ─── addFits / mulFits ───────────────────────────────────────────────────────

extern "C" void proof_add_fits()
{
	const std::size_t a = nondet_size();
	const std::size_t b = nondet_size();

	if (addFits(a, b))
	{
		// Sound: the sum really does not wrap.
		assert(a + b >= a);
		assert(a + b >= b);
	}
	else
	{
		// Complete: it only refuses sums that really would wrap. Stated as a
		// subtraction because forming `a + b` here is the very overflow
		// --unsigned-overflow-check is watching for; the proof must not commit
		// the fault it is proving absent.
		assert(b > SIZE_MAX - a);
	}
}

// mulFits, proved per multiplier. A symbolic a * b together with the
// SIZE_MAX / a inside mulFits is the nonlinear case no solver here discharges;
// fixing one operand makes both operations linear without weakening what is
// being checked about the other.
template <std::size_t A>
static void mulFitsIsExact()
{
	const std::size_t b = nondet_size();

	if (mulFits(A, b))
	{
		// Sound: the product is recoverable, so it did not wrap.
		assert(A == 0 || (A * b) / A == b);
	}
	else
	{
		// Complete: it only refuses products that really would overflow.
		assert(A != 0 && b > SIZE_MAX / A);
	}
}

extern "C" void proof_mul_fits_by0()  { mulFitsIsExact<0>(); }
extern "C" void proof_mul_fits_by1()  { mulFitsIsExact<1>(); }
extern "C" void proof_mul_fits_by2()  { mulFitsIsExact<2>(); }
extern "C" void proof_mul_fits_by4()  { mulFitsIsExact<4>(); }
extern "C" void proof_mul_fits_by8()  { mulFitsIsExact<8>(); }
extern "C" void proof_mul_fits_by16() { mulFitsIsExact<16>(); }

// Zero on either side never overflows, for any partner. Symbolic and linear.
extern "C" void proof_mul_fits_zero()
{
	const std::size_t a = nondet_size();
	assert(mulFits(a, 0));
	assert(mulFits(0, a));
}

// ─── rangeFits ───────────────────────────────────────────────────────────────

extern "C" void proof_range_fits()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t len = nondet_size();

	if (rangeFits(pos, size, len))
	{
		// The sum is safe to form...
		assert(addFits(pos, len));
		// ...and lands inside the buffer.
		assert(pos + len <= size);
	}
}

extern "C" void proof_range_fits_complete()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t len = nondet_size();

	// Any range that genuinely is in bounds must be accepted, or the check
	// would reject well-formed input.
	__ESBMC_assume(pos <= size);
	__ESBMC_assume(len <= size - pos);
	assert(rangeFits(pos, size, len));
}

// The reason rangeFits is not written as `pos + len <= size`.
//
// That formulation accepts any range whose sum wraps, because the wrapped value
// is small. The property below says rangeFits never does: a range that cannot
// even be added is always refused. Stated without forming the sum, since doing
// so would itself be the overflow under test.
extern "C" void proof_range_fits_refuses_wrapping_ranges()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t len = nondet_size();

	__ESBMC_assume(!addFits(pos, len));
	assert(!rangeFits(pos, size, len));
}

// ─── countFits ───────────────────────────────────────────────────────────────

// countFits composes with rangeFits: whatever it accepts is genuinely in
// bounds, and the bytes needed cannot be computed by an operation that wraps.
//
// Proved once per element width, because a symbolic width would put a 64-bit
// multiply and a 64-bit divide in the same query. Position, size and count stay
// symbolic across the full range.
//
// This composition is also why countFits checks `pos <= size` rather than
// leaning on remaining() saturating: ESBMC produced pos = SIZE_MAX-34, size = 0,
// count = 0, where the saturating version answered "fits" and rangeFits
// answered no.
template <std::size_t Width>
static void countFitsComposesWithRangeFits()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t count = nondet_size();

	if (countFits(pos, size, count, Width))
	{
		assert(mulFits(count, Width));
		assert(count * Width <= remaining(pos, size));
		assert(rangeFits(pos, size, count * Width));
		assert(addFits(pos, count * Width));
		assert(pos + count * Width <= size);
	}
}

// The widths these formats actually use: a tag byte, a 16-bit index, a 32-bit
// word, a 64-bit word, and a 16-byte DEX map entry.
extern "C" void proof_count_fits_width1()  { countFitsComposesWithRangeFits<1>(); }
extern "C" void proof_count_fits_width2()  { countFitsComposesWithRangeFits<2>(); }
extern "C" void proof_count_fits_width4()  { countFitsComposesWithRangeFits<4>(); }
extern "C" void proof_count_fits_width8()  { countFitsComposesWithRangeFits<8>(); }
extern "C" void proof_count_fits_width16() { countFitsComposesWithRangeFits<16>(); }

extern "C" void proof_count_fits_zero_width_not_vacuous()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t count = nondet_size();

	// A hostile header declaring zero-width elements must not turn the bound
	// off; it is treated as one byte per element.
	assert(countFits(pos, size, count, 0) == countFits(pos, size, count, 1));
}

extern "C" void proof_count_fits_bounded_by_input()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t count = nondet_size();
	const std::size_t width = nondet_size();

	// The property that makes this a data-derived bound rather than a cap: an
	// accepted count can never exceed the number of bytes left.
	if (countFits(pos, size, count, width)) assert(count <= remaining(pos, size));
}

extern "C" void proof_signed_count_fits()
{
	const std::int64_t count = nondet_int64();
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t width = nondet_size();

	// A negative count is refused before it can become a huge size_t.
	if (count < 0) assert(!signedCountFits(count, pos, size, width));

	if (signedCountFits(count, pos, size, width))
	{
		assert(count >= 0);
		assert(countFits(pos, size, static_cast<std::size_t>(count), width));
	}
}

// ─── pageCount ───────────────────────────────────────────────────────────────

extern "C" void proof_page_count()
{
	const std::size_t bytes = nondet_size();
	const std::size_t pageSize = nondet_size();
	__ESBMC_assume(pageSize > 0);

	const std::size_t n = pageCount(bytes, pageSize);

	// Always maps something, even for an empty buffer.
	assert(n >= 1);
	// Never more pages than bytes, once there is at least one byte.
	assert(bytes == 0 || n <= bytes);

	// Coverage and minimality, stated by division rather than by forming
	// n * pageSize. The product is genuinely not always representable --
	// bytes = SIZE_MAX with pageSize = 2 gives n = 2^63 and n * 2 wraps -- so
	// asserting mulFits(n, pageSize) here would be asserting something false.
	// ESBMC caught that; the header now documents the contract.
	//
	// (bytes - 1) / pageSize == n - 1 says exactly that n pages cover the
	// bytes and n - 1 would not.
	assert(bytes == 0 || (bytes - 1) / pageSize == n - 1);
}

extern "C" void proof_page_count_no_division_by_zero()
{
	const std::size_t bytes = nondet_size();
	// A zero page size has no meaningful answer and must not trap.
	assert(pageCount(bytes, 0) == 0);
}

// ─── clamp / reserveFor ──────────────────────────────────────────────────────

extern "C" void proof_reserve_for()
{
	const std::size_t declared = nondet_size();
	const std::size_t cap = nondet_size();
	const std::size_t r = reserveFor(declared, cap);

	// Never commits more than the cap, and never more than will be used.
	assert(r <= cap);
	assert(r <= declared);
	// Exact below the cap, so small containers are not penalised.
	assert(declared >= cap || r == declared);
}

// ─── arrayFits / offsetFits ──────────────────────────────────────────────────

template <std::size_t Elem>
static void arrayFitsIsSound()
{
	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	const std::size_t count = nondet_size();

	if (arrayFits(pos, size, count, Elem))
	{
		assert(mulFits(count, Elem));
		assert(rangeFits(pos, size, count * Elem));
		assert(addFits(pos, count * Elem));
		assert(pos + count * Elem <= size);
	}
}

extern "C" void proof_array_fits_elem1()  { arrayFitsIsSound<1>(); }
extern "C" void proof_array_fits_elem2()  { arrayFitsIsSound<2>(); }
extern "C" void proof_array_fits_elem4()  { arrayFitsIsSound<4>(); }
extern "C" void proof_array_fits_elem8()  { arrayFitsIsSound<8>(); }
extern "C" void proof_array_fits_elem12() { arrayFitsIsSound<12>(); }
extern "C" void proof_array_fits_elem16() { arrayFitsIsSound<16>(); }

extern "C" void proof_offset_fits()
{
	const std::size_t offset = nondet_size();
	const std::size_t len = nondet_size();
	const std::size_t size = nondet_size();

	if (offsetFits(offset, len, size))
	{
		assert(addFits(offset, len));
		assert(offset + len <= size);
		assert(offset <= size);
	}
}

int main() { return 0; }
