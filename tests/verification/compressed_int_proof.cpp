/**
 * @file tests/verification/compressed_int_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/compressed_int.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * The buffer is symbolic in every proof below: every byte an unconstrained
 * choice, the size and the position unconstrained std::size_t. So these hold
 * for all blobs a .NET image could contain, not for blobs someone thought of.
 *
 * The property that matters most is proof_extension_depends_only_on_width. The
 * code this kernel replaces, src/cli_parser/cli_heaps.cpp:48, chooses its
 * sign-extension mask by comparing the DECODED MAGNITUDE against 0x3F and
 * 0x3FFF -- which are the value ranges of the one- and two-byte forms, not
 * their tag ranges. A one-byte encoding runs to 0x7F. ESBMC refutes the
 * property against that expression immediately: uval = 0x7F, one byte read,
 * the correct answer -1 (ECMA-335 II.23.2 gives 0x7F as the encoding of -1),
 * the returned answer 63 | 0xFFFFE000 = 0xFFFFE03F = -8129. Every odd byte in
 * 0x41..0x7F is wrong the same way, and cli_sig.cpp:132 puts all of them into
 * array lower bounds.
 *
 * All three widths are wrong, not just the one-byte form: the same solver run
 * against the two-byte payload 0x3F returns -33 where II.23.2 says -8161, and
 * against the four-byte payload 9343 returns -3521 where II.23.2 says
 * -268430785. A magnitude cannot select a width, because the same magnitude is
 * a legal payload at all three.
 *
 * Run with scripts/verify_esbmc.sh.
 */

// ESBMC-OPTIONS: --unwind 10
//
// The longest loop in the kernel or in this harness is the eight-byte buffer
// fill; the decoder's own loop runs at most kMaxBytes - 1 = 3 times and the
// encoder's at most 3. Nine unwindings cover the fill, ten leave a margin.
// ESBMC 8.5.0 generates unwinding assertions by default and nothing here turns
// them off, so the bound is part of the proof: a decoder that could loop past
// four bytes fails here rather than being silently cut off.

// ESBMC-SOLVER: --boolector
//
// Measured, and the measurement is worth stating precisely because it does NOT
// match the pattern elsewhere in this directory. All nineteen proofs discharge
// under all three backends, and the totals for the whole file are z3 6.1s,
// boolector 6.2s, bitwuzla 5.2s -- the slowest single query is
// proof_extension_depends_only_on_width at 487ms under z3. So unlike
// section_map_proof.cpp, where boolector is the difference between 22s and a
// timeout, the choice here is not load-bearing and no verdict depends on it.
//
// It is pinned anyway so that a verdict names the backend that produced it,
// and boolector rather than the script default because these are small
// bitvector queries. What makes a bitvector backend SAFE here is that nothing
// reachable from this harness divides: payloadBits and firstByteBits are a
// switch and a subtraction, and every width is shifted by, never divided by.
// The spurious "arithmetic overflow on div" that boolector and bitwuzla report
// for unsigned division -- the false alarm that pins bounds_proof.cpp to z3 --
// therefore cannot arise. `scripts/verify_esbmc.sh --cross` runs both backends
// over this file and would report any disagreement as a finding; today there
// is none to report.

#include "retdec/utils/compressed_int.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::cint;

extern "C" {
std::uint8_t nondet_uchar();
std::uint32_t nondet_uint32();
std::int32_t nondet_int32();
std::size_t nondet_size();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

// Twice the widest encoding, so a decode can start partway in and still have a
// full four bytes ahead of it -- and so a decode that starts near the end has
// somewhere to run off to if the bound is wrong.
static constexpr std::size_t kBufLen = 2 * kMaxBytes;

namespace {

void fillNondet(std::uint8_t (&buf)[kBufLen])
{
	for (std::size_t i = 0; i < kBufLen; ++i)
		buf[i] = nondet_uchar();
}

} // namespace

// ─── 1. The decoder never indexes outside [pos, size) ────────────────────────

// ESBMC's array-bounds check carries this one: the buffer is a real array of
// kBufLen bytes, the declared size is an unconstrained value no larger than
// that, and pos is unconstrained over the whole 64-bit range. If any path
// could read data[pos + i] outside the array -- including the paths where pos
// is within three of SIZE_MAX and `pos + 3` wraps, which is how
// cli_heaps.cpp:35 states its bound -- the check fires here.

extern "C" void proof_unsigned_decode_stays_in_bounds()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();

	const Result r = decodeUnsigned(buf, size, pos);
	// Stated so the decode is not dead code the solver can discard.
	assert(r.ok || r.bytesRead == 0);
}

extern "C" void proof_signed_decode_stays_in_bounds()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();

	const SResult s = decodeSigned(buf, size, pos);
	assert(s.ok || s.bytesRead == 0);
}

extern "C" void proof_a_null_blob_decodes_nothing()
{
	// A caller with no blob must not get an answer derived from whatever the
	// pointer happens to be.
	const Result r = decodeUnsigned(nullptr, nondet_size(), nondet_size());
	assert(!r.ok && r.bytesRead == 0 && r.value == 0);
	const SResult s = decodeSigned(nullptr, nondet_size(), nondet_size());
	assert(!s.ok && s.bytesRead == 0 && s.value == 0);
}

// ─── 2. A refusal consumes nothing ───────────────────────────────────────────

extern "C" void proof_failure_consumes_nothing()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();

	const Result r = decodeUnsigned(buf, size, pos);
	// The leb128.h convention. The caller advances by bytesRead, so a refusal
	// that reported a non-zero count would move the cursor into the middle of
	// whatever follows and desynchronise every later read from the same blob.
	if (!r.ok)
	{
		assert(r.bytesRead == 0);
		assert(r.value == 0);
	}

	const SResult s = decodeSigned(buf, size, pos);
	if (!s.ok)
	{
		assert(s.bytesRead == 0);
		assert(s.value == 0);
	}

	// Signed and unsigned refuse together: the signed form is the unsigned one
	// plus a rotate, so a caller cannot get a different answer about whether
	// the bytes were there by asking the other way round.
	assert(r.ok == s.ok);
	assert(r.bytesRead == s.bytesRead);
}

// ─── 3. What a success promises ──────────────────────────────────────────────

extern "C" void proof_success_width_and_range()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();

	const Result r = decodeUnsigned(buf, size, pos);
	if (!r.ok) return;

	// The three widths of ECMA-335 II.23.2 and no other.
	assert(r.bytesRead == 1 || r.bytesRead == 2 || r.bytesRead == 4);

	// Stated as a subtraction. `pos + r.bytesRead <= size` is the natural
	// spelling and is the mistake: with pos out of a file the sum wraps and the
	// assertion passes on exactly the inputs it exists to catch.
	assert(pos <= size);
	assert(r.bytesRead <= size - pos);

	// 29 payload bits is the whole of the encoding's range, so no caller needs
	// a second check before using the value as a count or an index base.
	assert(r.value <= kMaxValue);
	assert(kMaxValue == 0x1FFFFFFFu);
}

// ─── 4. The value is the one II.23.2 defines ─────────────────────────────────

extern "C" void proof_value_matches_the_definition()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();

	const Result r = decodeUnsigned(buf, size, pos);
	if (!r.ok) return;

	// Safety proved by proof_success_width_and_range; restated here so this
	// query does not depend on that one having run.
	__ESBMC_assume(pos < size);
	const std::uint8_t b0 = buf[pos];

	// The definition written out with the literal masks of the II.23.2 table,
	// independent of the kernel's firstByteBits/lowMask derivation. If that
	// derivation is off by a bit in either direction this refutes it.
	if (r.bytesRead == 1)
	{
		assert((b0 & 0x80u) == 0);
		assert(r.value == b0);
	}
	else if (r.bytesRead == 2)
	{
		assert((b0 & 0xC0u) == 0x80u);
		assert(r.value == ((static_cast<std::uint32_t>(b0 & 0x3Fu) << 8) | static_cast<std::uint32_t>(buf[pos + 1])));
	}
	else
	{
		assert(r.bytesRead == 4);
		assert((b0 & 0xE0u) == 0xC0u);
		assert(
			r.value
			== ((static_cast<std::uint32_t>(b0 & 0x1Fu) << 24) | (static_cast<std::uint32_t>(buf[pos + 1]) << 16)
				| (static_cast<std::uint32_t>(buf[pos + 2]) << 8) | static_cast<std::uint32_t>(buf[pos + 3])));
	}
}

extern "C" void proof_undefined_tag_is_refused()
{
	// 111xxxxx has no meaning in II.23.2. Accepting it -- by falling through to
	// the four-byte case, which the 0xE0 test is one bit away from doing --
	// would consume four bytes of a blob on the strength of a byte the format
	// does not define.
	std::uint8_t buf[kBufLen];
	fillNondet(buf);
	const std::size_t pos = nondet_size();
	__ESBMC_assume(pos < kBufLen);
	__ESBMC_assume((buf[pos] & 0xE0u) == 0xE0u);

	assert(!decodeUnsigned(buf, kBufLen, pos).ok);
}

// ─── 5. THE headline property ────────────────────────────────────────────────

extern "C" void proof_extension_depends_only_on_width()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();

	const Result r = decodeUnsigned(buf, size, pos);
	const SResult s = decodeSigned(buf, size, pos);
	if (!r.ok) return;

	const unsigned bits = payloadBits(r.bytesRead);
	assert(bits == 7 || bits == 14 || bits == 29);

	// The definition, in arithmetic rather than in masks: the low bit is the
	// sign, the rest is the magnitude, and a negative value is that magnitude
	// less 2^(bits-1). The 2^(bits-1) term is a function of `bits` alone, and
	// `bits` is payloadBits(bytesRead) -- so the whole point of the property is
	// that r.value appears here only as `>> 1` and `& 1`, never in a range test.
	const std::int64_t half = static_cast<std::int64_t>(std::uint64_t{1} << (bits - 1));
	const std::int64_t want = static_cast<std::int64_t>(r.value >> 1) - ((r.value & 1u) ? half : 0);

	assert(static_cast<std::int64_t>(s.value) == want);
}

extern "C" void proof_signed_range_is_set_by_width()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();

	const SResult s = decodeSigned(buf, size, pos);
	if (!s.ok) return;

	// The same property from the outside, as three closed intervals. This is
	// the form a reader can check against the II.23.2 text directly, and it is
	// the form the live code fails: one byte 0x7F returns -8129, which is not
	// in [-64, 63], and 0x41 returns -8160.
	if (s.bytesRead == 1)
	{
		assert(s.value >= -64 && s.value <= 63);
	}
	else if (s.bytesRead == 2)
	{
		assert(s.value >= -8192 && s.value <= 8191);
	}
	else
	{
		assert(s.bytesRead == 4);
		assert(s.value >= -268435456 && s.value <= 268435455);
	}
}

extern "C" void proof_ecma_signed_examples()
{
	// The worked encodings from the ECMA-335 II.23.2 table, decoded. A property
	// stated over all inputs can still be the wrong property; these anchor it
	// to the document. 0x7F and 0x01 are the two the magnitude-driven mask gets
	// wrong and right respectively, despite both being one byte.
	const std::uint8_t m1[] = {0x7F};                    // -1
	const std::uint8_t m64[] = {0x01};                   // -64
	const std::uint8_t m3[] = {0x7B};                    // -3
	const std::uint8_t p3[] = {0x06};                    //  3
	const std::uint8_t p64[] = {0x80, 0x80};             //  64
	const std::uint8_t m8k[] = {0x80, 0x01};             // -8192
	const std::uint8_t big[] = {0xDF, 0xFF, 0xFF, 0xFE}; //  268435455
	const std::uint8_t sml[] = {0xC0, 0x00, 0x00, 0x01}; // -268435456

	assert(decodeSigned(m1, 1, 0).value == -1);
	assert(decodeSigned(m64, 1, 0).value == -64);
	assert(decodeSigned(m3, 1, 0).value == -3);
	assert(decodeSigned(p3, 1, 0).value == 3);
	assert(decodeSigned(p64, 2, 0).value == 64);
	assert(decodeSigned(m8k, 2, 0).value == -8192);
	assert(decodeSigned(big, 4, 0).value == 268435455);
	assert(decodeSigned(sml, 4, 0).value == -268435456);
}

// ─── 6. Round trip ───────────────────────────────────────────────────────────

extern "C" void proof_unsigned_round_trip()
{
	const std::uint32_t v = nondet_uint32();
	__ESBMC_assume(v <= kMaxValue);

	std::uint8_t buf[kMaxBytes];
	const std::size_t n = encodeUnsigned(v, buf, kMaxBytes);
	assert(n == 1 || n == 2 || n == 4);

	const Result r = decodeUnsigned(buf, n, 0);
	assert(r.ok);
	assert(r.value == v);
	assert(r.bytesRead == n);
}

extern "C" void proof_unsigned_encode_is_narrowest()
{
	// Not merely "some encoding": the shortest one. A decoder is free to accept
	// a padded encoding, but two encodings of the same value mean two byte
	// counts, and the byte count is what advances the cursor.
	const std::uint32_t v = nondet_uint32();
	__ESBMC_assume(v <= kMaxValue);

	std::uint8_t buf[kMaxBytes];
	const std::size_t n = encodeUnsigned(v, buf, kMaxBytes);

	if (v <= 0x7Fu)
		assert(n == 1);
	else if (v <= 0x3FFFu)
		assert(n == 2);
	else
		assert(n == 4);
}

extern "C" void proof_unencodable_value_is_refused()
{
	// Above 2^29-1 there is no encoding. Truncating to 29 bits would hand back
	// a byte count the caller then trusts, with a value that is not the one it
	// was given.
	const std::uint32_t v = nondet_uint32();
	__ESBMC_assume(v > kMaxValue);
	std::uint8_t buf[kMaxBytes] = {0, 0, 0, 0};
	assert(encodeUnsigned(v, buf, kMaxBytes) == 0);
	// Nothing was written.
	assert(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0);
}

extern "C" void proof_signed_round_trip()
{
	const std::int32_t v = nondet_int32();
	__ESBMC_assume(v >= -268435456 && v <= 268435455);

	std::uint8_t buf[kMaxBytes];
	const std::size_t n = encodeSigned(v, buf, kMaxBytes);
	assert(n == 1 || n == 2 || n == 4);

	const SResult s = decodeSigned(buf, n, 0);
	assert(s.ok);
	assert(s.value == v);
	assert(s.bytesRead == n);
}

extern "C" void proof_one_byte_signed_re_encodes_to_itself()
{
	// The other direction of the round trip, over every one-byte encoding.
	// This is the exhaustive statement of the live defect: 32 of these 128
	// bytes decode to a value the encoder then writes back as two bytes,
	// because the decoded value is outside the one-byte signed range.
	const std::uint8_t b = nondet_uchar();
	__ESBMC_assume(b < 0x80u);

	const SResult s = decodeSigned(&b, 1, 0);
	assert(s.ok && s.bytesRead == 1);

	std::uint8_t out[kMaxBytes];
	assert(encodeSigned(s.value, out, kMaxBytes) == 1);
	assert(out[0] == b);
}

// ─── 7. Truncation and shift bounds ──────────────────────────────────────────

extern "C" void proof_truncated_wide_prefix_is_refused()
{
	// A first byte that declares four bytes over a blob holding one, two or
	// three. Decoding it needs shifts of 24, 16 and 8; a decoder that fetches
	// before it checks reads past the end, and one that assembles what it did
	// get returns a value built from bytes it never read. --ub-shift-check
	// covers the shifts on this path, and the array-bounds check the reads.
	std::uint8_t buf[kBufLen];
	fillNondet(buf);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	__ESBMC_assume(pos < size);
	__ESBMC_assume(size - pos < kMaxBytes);      // fewer than four bytes left
	__ESBMC_assume((buf[pos] & 0xE0u) == 0xC0u); // but four are declared

	const Result r = decodeUnsigned(buf, size, pos);
	assert(!r.ok);
	assert(r.bytesRead == 0 && r.value == 0);

	// The signed form refuses on the same bytes, for the same reason.
	const SResult s = decodeSigned(buf, size, pos);
	assert(!s.ok);
}

extern "C" void proof_two_byte_prefix_needs_two_bytes()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf);
	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	__ESBMC_assume(pos < size);
	__ESBMC_assume(size - pos == 1);
	__ESBMC_assume((buf[pos] & 0xC0u) == 0x80u);

	assert(!decodeUnsigned(buf, size, pos).ok);
}

// ─── Encoder bounds ──────────────────────────────────────────────────────────

extern "C" void proof_encoder_never_writes_past_the_cap()
{
	// The buffer is DELIBERATELY larger than any cap this proof allows, and
	// that is the whole point of the shape.
	//
	// The first version of this proof sized the buffer at kMaxBytes and assumed
	// `cap <= kMaxBytes`, then said the array-bounds check refuted a partial
	// write. It does not and cannot: a write at index 0 with a cap of 0 is
	// inside the array, so the bounds check has nothing to say about it, and
	// the proof passed for the wrong reason. The audit demonstrated it by
	// injecting an unconditional `out[0] = ...` before the cap test.
	//
	// A tail of sentinel bytes past the cap is what actually carries the
	// property: they are checked byte by byte, so any write at or beyond `cap`
	// -- including one the array happily accommodates -- is a failed assertion
	// naming the byte.
	static const std::size_t kSlack = kMaxBytes;
	std::uint8_t buf[kMaxBytes + kSlack];
	for (std::size_t i = 0; i < kMaxBytes + kSlack; ++i)
		buf[i] = 0;

	const std::uint32_t v = nondet_uint32();
	const std::size_t cap = nondet_size();
	__ESBMC_assume(cap <= kMaxBytes);

	const std::size_t n = encodeUnsigned(v, buf, cap);
	assert(n <= cap);
	// Nothing at or past the cap was touched, whether the encode succeeded or
	// refused. On a refusal that is every byte, which is the partial-write
	// property; on success it is the tail, which is the overrun property.
	for (std::size_t i = cap; i < kMaxBytes + kSlack; ++i)
		assert(buf[i] == 0);
	if (n == 0)
		for (std::size_t i = 0; i < kMaxBytes + kSlack; ++i)
			assert(buf[i] == 0);

	std::uint8_t sbuf[kMaxBytes + kSlack];
	for (std::size_t i = 0; i < kMaxBytes + kSlack; ++i)
		sbuf[i] = 0;

	const std::size_t m = encodeSigned(nondet_int32(), sbuf, cap);
	assert(m <= cap);
	for (std::size_t i = cap; i < kMaxBytes + kSlack; ++i)
		assert(sbuf[i] == 0);
	if (m == 0)
		for (std::size_t i = 0; i < kMaxBytes + kSlack; ++i)
			assert(sbuf[i] == 0);
}

extern "C" void proof_encoder_refuses_a_null_buffer()
{
	assert(encodeUnsigned(nondet_uint32(), nullptr, nondet_size()) == 0);
	assert(encodeSigned(nondet_int32(), nullptr, nondet_size()) == 0);
}
