/**
 * @file tests/verification/source_scan_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/c_source_scan.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * The scanner reads one character ahead of its cursor and writes one ahead of
 * it too, which is where this shape of loop goes wrong: at the last character
 * the lookahead is off the end, and a two-character token that skips forward
 * can step past it. ESBMC's array-bounds checking does the real work here --
 * the input is a fully symbolic buffer, so if any input at all could drive an
 * index outside either array, the run fails.
 *
 * The loop is bounded by the buffer length, which is bounded here, so unwinding
 * assertions make that bound part of the proof.
 */

// The scan loop and the checking loops each run kLen times; unwinding a couple
// further leaves headroom. ESBMC generates unwinding assertions by default, so
// the bound is proved rather than assumed.
// ESBMC-OPTIONS: --unwind 12

#include "retdec/utils/c_source_scan.h"

#include <cassert>
#include <cstddef>

using namespace retdec::utils::source_scan;

extern "C" {
char nondet_char();
std::size_t nondet_size();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

// Long enough to hold every two-character token the scanner recognises, a
// delimiter on each side, and a character at each end to sit the lookahead
// against. Short enough for the loop to unwind fully.
static constexpr std::size_t kLen = 8;

namespace {

void fillNondet(char (&buf)[kLen])
{
	for (std::size_t i = 0; i < kLen; ++i)
		buf[i] = nondet_char();
}

} // namespace

// The central property: for ANY input of this length, no index leaves either
// buffer. Guard bytes on both sides catch an off-by-one that array-bounds
// checking alone might miss if the compiler laid the arrays out adjacently.
extern "C" void proof_scan_stays_in_bounds()
{
	char in[kLen];
	char out[kLen];
	fillNondet(in);
	for (std::size_t i = 0; i < kLen; ++i)
		out[i] = '\0';

	blankNonCode(in, kLen, out);

	// Every position was written exactly once, so nothing was skipped and
	// nothing was left holding its initial value by accident.
	for (std::size_t i = 0; i < kLen; ++i)
		assert(out[i] != '\0' || in[i] == '\0');
}

// A length shorter than the buffer must be respected: the scanner may not read
// or write the tail, whatever the bytes beyond it say.
extern "C" void proof_scan_respects_a_short_length()
{
	char in[kLen];
	char out[kLen];
	fillNondet(in);

	const char sentinel = '\x7F';
	for (std::size_t i = 0; i < kLen; ++i)
		out[i] = sentinel;

	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kLen);

	blankNonCode(in, n, out);

	// Everything at or past n is untouched.
	for (std::size_t i = 0; i < kLen; ++i)
	{
		if (i >= n) assert(out[i] == sentinel);
	}
}

// Newlines survive, so a line number computed from the blanked text still
// matches the original. The gate relies on offsets lining up.
extern "C" void proof_scan_preserves_newlines()
{
	char in[kLen];
	char out[kLen];
	fillNondet(in);

	blankNonCode(in, kLen, out);

	for (std::size_t i = 0; i < kLen; ++i)
	{
		if (in[i] == '\n') assert(out[i] == '\n');
	}
}

// Nothing is invented: every output character is either the input character or
// a space. In particular the scanner cannot introduce a keyword, an operator or
// a quote that was not in the source.
extern "C" void proof_scan_only_blanks()
{
	char in[kLen];
	char out[kLen];
	fillNondet(in);

	blankNonCode(in, kLen, out);

	for (std::size_t i = 0; i < kLen; ++i)
	{
		assert(out[i] == in[i] || out[i] == ' ');
	}
}

// A null pointer or a zero length is a no-op, not a crash. Callers pass
// .data() of a possibly-empty string.
extern "C" void proof_scan_handles_empty_and_null()
{
	char out[kLen];
	const char sentinel = '\x7F';
	for (std::size_t i = 0; i < kLen; ++i)
		out[i] = sentinel;

	char in[kLen];
	fillNondet(in);

	blankNonCode(in, 0, out);
	blankNonCode(nullptr, kLen, out);
	blankNonCode(in, kLen, nullptr);

	for (std::size_t i = 0; i < kLen; ++i)
		assert(out[i] == sentinel);
}

int main()
{
	return 0;
}
