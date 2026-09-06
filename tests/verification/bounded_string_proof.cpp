/**
 * @file tests/verification/bounded_string_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/bounded_string.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * The bug these functions exist to stop is `strlen` on a pointer into a mapped
 * file: the scan runs until it finds a zero byte, which the file was under no
 * obligation to place before the end. Clamping the result afterwards does not
 * help, because the read has already happened.
 *
 * So the property that matters is not just "the answer is <= len" -- it is that
 * no byte at or beyond `len` is ever examined. That is not expressible as an
 * assertion about a return value, so it is established the way the scan itself
 * is bounded: the buffers below are exactly `kMax` bytes, ESBMC's array bounds
 * checking is on, and a scan that read one byte too far would be reported as an
 * out-of-bounds access rather than as a failed assertion. Both are failures;
 * the distinction only matters for reading the output.
 *
 * The scan is a loop, so this is a bounded proof: every property holds for all
 * contents of buffers up to kMax bytes, with the bytes themselves and the
 * length fully symbolic.
 */

// ESBMC-OPTIONS: --unwind 10

#include "retdec/utils/bounded_string.h"

#include <cassert>
#include <cstddef>

using namespace retdec::utils::bstr;

extern "C" {
std::size_t nondet_size();
char nondet_char();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void) sizeof((cond) ? 1 : 0))
#endif

// One less than the unwind bound, so the loop is always fully unrolled and the
// result is a proof over every buffer this size rather than a partial search.
static const std::size_t kMax = 8;

// Fill a buffer with unconstrained bytes.
static void fill(char* buf, std::size_t n)
{
	for (std::size_t i = 0; i < n; ++i)
		buf[i] = nondet_char();
}

// ─── terminatorAt ────────────────────────────────────────────────────────────

extern "C" void proof_terminator_is_the_first_one()
{
	char buf[kMax];
	fill(buf, kMax);
	const std::size_t len = nondet_size();
	__ESBMC_assume(len <= kMax);

	const std::size_t at = terminatorAt(buf, len);

	if (at != npos)
	{
		// Inside the bound, and really a terminator.
		assert(at < len);
		assert(buf[at] == '\0');
		// The *first* one: nothing before it terminates.
		for (std::size_t i = 0; i < at; ++i)
			assert(buf[i] != '\0');
	}
	else
	{
		// npos is only reported when there is genuinely none.
		for (std::size_t i = 0; i < len; ++i)
			assert(buf[i] != '\0');
	}
}

extern "C" void proof_terminator_of_zero_length_is_npos()
{
	char buf[kMax];
	fill(buf, kMax);
	// A zero-length bound examines nothing, whatever the buffer holds.
	assert(terminatorAt(buf, 0) == npos);
}

extern "C" void proof_null_pointer_is_never_dereferenced()
{
	const std::size_t len = nondet_size();
	// Any length at all, including one a caller computed wrongly.
	assert(terminatorAt(nullptr, len) == npos);
	assert(boundedLength(nullptr, len) == 0);
	assert(!isTerminated(nullptr, len));
}

// ─── boundedLength ───────────────────────────────────────────────────────────

extern "C" void proof_bounded_length_never_exceeds_the_bound()
{
	char buf[kMax];
	fill(buf, kMax);
	const std::size_t len = nondet_size();
	__ESBMC_assume(len <= kMax);

	const std::size_t n = boundedLength(buf, len);

	// The whole point: a caller may copy n bytes with no second check.
	assert(n <= len);
	// Every byte it reports is part of the string.
	for (std::size_t i = 0; i < n; ++i)
		assert(buf[i] != '\0');
	// And it stopped for a reason: either at the bound or at a terminator.
	assert(n == len || buf[n] == '\0');
}

extern "C" void proof_bounded_length_agrees_with_terminator_at()
{
	char buf[kMax];
	fill(buf, kMax);
	const std::size_t len = nondet_size();
	__ESBMC_assume(len <= kMax);

	const std::size_t at = terminatorAt(buf, len);
	const std::size_t n  = boundedLength(buf, len);

	// The two are one function with two failure conventions, and they must not
	// drift apart: an unterminated string measures to the bound, a terminated
	// one to its terminator.
	if (at == npos)
		assert(n == len);
	else
		assert(n == at);
}

// ─── isTerminated ────────────────────────────────────────────────────────────

extern "C" void proof_is_terminated_matches_the_scan()
{
	char buf[kMax];
	fill(buf, kMax);
	const std::size_t len = nondet_size();
	__ESBMC_assume(len <= kMax);

	if (isTerminated(buf, len))
	{
		// A terminated string is strictly shorter than its bound, which is what
		// makes "read n+1 bytes to include the NUL" safe.
		assert(boundedLength(buf, len) < len);
	}
	else
	{
		assert(boundedLength(buf, len) == len);
	}
}
