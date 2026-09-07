/**
 * @file tests/verification/leb128_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/leb128.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * LEB128 is decoded from attacker-controlled bytes in the EH tables, Python
 * line tables, WebAssembly and DEX. The two ways a hand-written decoder goes
 * wrong -- an unbounded shift, and a cursor that walks off the end -- are both
 * undefined behaviour rather than merely wrong answers, so they are proved
 * absent here rather than tested for.
 *
 * The buffer is symbolic: every byte is an unconstrained choice, so these hold
 * for ALL inputs of the modelled length, not for chosen ones. The decode loop
 * is bounded by leb128::kMaxBytes, so --unwinding-assertions makes the unwind
 * bound part of the proof rather than an assumption.
 *
 * Run with scripts/verify_esbmc.sh.
 */

// The decode loop runs at most leb128::kMaxBytes times; unwinding two further
// leaves headroom. ESBMC generates unwinding assertions by default, so the
// bound is part of the proof rather than an assumption: a decoder that could
// loop longer fails here instead of being silently cut off.
// ESBMC-OPTIONS: --unwind 14

#include "retdec/utils/leb128.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::leb128;

extern "C" {
std::uint8_t nondet_uchar();
std::size_t nondet_size();
std::uint64_t nondet_uint64();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

// A buffer long enough that a maximal encoding fits and still overruns if the
// decoder does not stop: kMaxBytes plus room to start partway in.
static constexpr std::size_t kBufLen = 12;

namespace {

void fillNondet(std::uint8_t (&buf)[kBufLen])
{
	for (std::size_t i = 0; i < kBufLen; ++i)
		buf[i] = nondet_uchar();
}

} // namespace

// ─── Unsigned ────────────────────────────────────────────────────────────────

// The central property. ESBMC's own array-bounds and undefined-shift checks do
// the work: if the decoder could read data[pos + used] outside the buffer, or
// shift by 64 or more, the run fails regardless of the assertions below.
extern "C" void proof_uleb_stays_in_bounds()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);

	const Result r = decodeUnsigned(buf, size, pos);

	if (r.ok)
	{
		// Consumed at least one byte, no more than an encoding can need...
		assert(r.bytesRead >= 1);
		assert(r.bytesRead <= kMaxBytes);
		// ...and every byte it consumed was inside the buffer.
		assert(pos < size);
		assert(r.bytesRead <= size - pos);
	}
	else
	{
		// A failed decode reports no progress, so a caller advancing by
		// bytesRead cannot land somewhere undefined.
		assert(r.bytesRead == 0);
		assert(r.value == 0);
	}
}

// Truncation is refused, not guessed at. A buffer whose every byte sets the
// continuation bit has no terminator, so no decode of it can succeed.
extern "C" void proof_uleb_rejects_unterminated()
{
	std::uint8_t buf[kBufLen];
	for (std::size_t i = 0; i < kBufLen; ++i)
		buf[i] = nondet_uchar() | 0x80;

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);

	assert(!decodeUnsigned(buf, size, 0).ok);
}

// A single byte below 0x80 encodes itself. Anchors the decode against the
// definition rather than only proving safety properties about it.
extern "C" void proof_uleb_single_byte_is_identity()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);
	__ESBMC_assume(buf[0] < 0x80);

	const Result r = decodeUnsigned(buf, kBufLen, 0);
	assert(r.ok);
	assert(r.bytesRead == 1);
	assert(r.value == buf[0]);
}

// Two bytes: the low seven bits of the first, then the second shifted up.
extern "C" void proof_uleb_two_byte_value()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);
	__ESBMC_assume((buf[0] & 0x80) != 0);
	__ESBMC_assume(buf[1] < 0x80);

	const Result r = decodeUnsigned(buf, kBufLen, 0);
	assert(r.ok);
	assert(r.bytesRead == 2);
	assert(r.value == (std::uint64_t(buf[0] & 0x7F) | (std::uint64_t(buf[1]) << 7)));
}

// An empty or exhausted buffer is a refusal, never a read.
extern "C" void proof_uleb_empty_and_past_end()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	__ESBMC_assume(pos >= size);

	const Result r = decodeUnsigned(buf, size, pos);
	assert(!r.ok);
	assert(r.bytesRead == 0);

	assert(!decodeUnsigned(nullptr, 0, 0).ok);
}

// ─── Signed ──────────────────────────────────────────────────────────────────

extern "C" void proof_sleb_stays_in_bounds()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t pos = nondet_size();
	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);

	const Result r = decodeSigned(buf, size, pos);

	if (r.ok)
	{
		assert(r.bytesRead >= 1);
		assert(r.bytesRead <= kMaxBytes);
		assert(pos < size);
		assert(r.bytesRead <= size - pos);
	}
	else
	{
		assert(r.bytesRead == 0);
	}
}

extern "C" void proof_sleb_rejects_unterminated()
{
	std::uint8_t buf[kBufLen];
	for (std::size_t i = 0; i < kBufLen; ++i)
		buf[i] = nondet_uchar() | 0x80;

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);

	assert(!decodeSigned(buf, size, 0).ok);
}

// A single byte with bit 6 set is negative; without it, non-negative. This is
// the sign-extension step, which is where accumulating in a signed type would
// have been undefined.
extern "C" void proof_sleb_single_byte_sign()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);
	__ESBMC_assume(buf[0] < 0x80);

	const Result r = decodeSigned(buf, kBufLen, 0);
	assert(r.ok);
	assert(r.bytesRead == 1);

	const std::int64_t v = toSigned(r.value);
	if ((buf[0] & 0x40) != 0)
		assert(v < 0);
	else
		assert(v == static_cast<std::int64_t>(buf[0]));
}

// ─── toSigned ────────────────────────────────────────────────────────────────

// Total, and a genuine bijection: no input traps, and the round trip is exact.
extern "C" void proof_to_signed_is_total_and_reversible()
{
	// Taken whole rather than assembled a byte at a time: `v = (v << 8) | b`
	// discards the top byte on the final round, and --overflow-check is right
	// to reject a proof that starts by losing information.
	const std::uint64_t v = nondet_uint64();

	const std::int64_t s = toSigned(v);
	assert(static_cast<std::uint64_t>(s) == v);
	assert((v <= static_cast<std::uint64_t>(INT64_MAX)) == (s >= 0));
}

int main()
{
	return 0;
}
