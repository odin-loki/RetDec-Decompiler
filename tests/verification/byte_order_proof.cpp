/**
 * @file tests/verification/byte_order_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/byte_order.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Nine hand-written copies of "turn a byte range into an integer" live in this
 * tree, and the three defects the kernel exists to stop are all defects of the
 * step AFTER the bounds check: the shift count, the byte order, and the
 * direction the value is extended in. bounds.h already proves the range fits.
 *
 * Every input below is symbolic -- buffer contents, position, size, width,
 * bits-per-unit, capacity, the value being written -- and constrained only
 * where the format genuinely constrains it. Where a property needs a buffer
 * whose end ESBMC can see, the buffer is malloc'd at exactly the symbolic size,
 * so a read one past the end is an array-bounds violation rather than a read of
 * a neighbouring stack slot that no checker would notice.
 *
 * Three checks carry most of the weight and are not written as assertions
 * because ESBMC generates them: --ub-shift-check (a shift count at or above the
 * width of its operand), --unsigned-overflow-check (a product or sum that
 * wraps), and the array-bounds check on the exactly-sized buffers.
 */

// ESBMC-OPTIONS: --unwind 11
//
// The longest loop in the kernel runs kMaxBytes = 8 times: readLE, readBE,
// readWidened, writeLE and writeBE all iterate once per byte, and all five
// refuse before the loop when n > 8. readWidened's bound is tighter still --
// widthFits forces n * bitsPerUnit <= 64 with bitsPerUnit >= 8, so n <= 8.
//
// The longest loop is in this harness rather than in the kernel: fillNondet
// runs over kBufLen = 10 bytes. Ten iterations plus the trip that proves the
// loop exits is 11. Unwinding assertions are on by default in ESBMC 8.5.0, so a
// bound that is too small is reported rather than silently truncating the
// search -- measured, not assumed: at --unwind 9 this harness reports
// "unwinding assertion loop 16" FAILED on the fill loop in
// proof_writes_stay_inside_the_capacity, and at --unwind 11 every unwinding
// assertion PASSES.

// ESBMC-SOLVER: --z3
//
// Measured, not assumed, and the measurement did not say what was expected.
// Running all sixteen proofs end to end at --unwind 11: z3 15.0s, boolector
// 23.3s, bitwuzla 11.0s, with all three returning SUCCESSFUL on every proof.
// Boolector is the usual choice for bitvector work and is what section_map_proof
// pins, but on this harness it is the slowest of the three -- the queries here
// are small (a ten-byte buffer and one 64-bit accumulator), so the bitvector
// backends have nothing to win. z3 is pinned because it is the faster of the two
// that `scripts/verify_esbmc.sh --cross` compares, and because it is the script
// default, so the pin records a measurement rather than changing behaviour.
//
// Nothing in byte_order.h or in this file divides, so the spurious
// unsigned-division overflow that pins bounds_proof.cpp to z3 cannot arise here
// at all, and --cross has no disagreement to excuse. That was checked rather
// than assumed as well: widthFits was temporarily rewritten in its division form
// (`n <= 64 / bitsPerUnit`) and all three backends still discharged
// proof_widened_refuses_rather_than_shifting clean -- the division here is on
// `unsigned`, and widthFits has already excluded a zero divisor, so the
// INT_MIN/-1 witness the bitvector backends find in bounds.h has no counterpart
// in this file. The multiplication form is kept for a reason about the call
// sites, not about the solver; see widthFits in the header.

#include "retdec/utils/byte_order.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace retdec::utils::byteorder;
namespace bounds = retdec::utils::bounds;

extern "C" {
std::uint8_t   nondet_uchar();
unsigned       nondet_uint();
std::size_t    nondet_size();
std::uint64_t  nondet_uint64();
bool           nondet_bool();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void) sizeof((cond) ? 1 : 0))
#endif

/// Two bytes wider than the widest legal read.
///
/// The width matters to whether this suite is load-bearing. At exactly
/// kMaxBytes, a read of nine bytes is refused by bounds::rangeFits before the
/// width check is ever consulted, so loosening `n > kMaxBytes` to
/// `n > kMaxBytes + 1` would pass every proof here while shifting a
/// std::uint64_t by 64. Two spare bytes make that read reach the loop, where
/// --ub-shift-check reports it. Confirmed by injecting exactly that off-by-one.
static constexpr std::size_t kBufLen = kMaxBytes + 2;

/// Widest output capacity the write proofs explore: one byte more than the
/// widest legal write, so "refuses when n > outCap" and "leaves the tail alone"
/// are both reachable.
static constexpr std::size_t kMaxCap = kMaxBytes + 1;

/// A width a caller could ask for that is past anything the kernel accepts.
/// Twice kMaxBytes, so every n in 0..16 is covered including the whole refused
/// range -- that is where byte_value_storage.cpp:958 shifts by 64 and beyond.
static constexpr unsigned kProbeWidth = 2 * kMaxBytes;

namespace {

void fillNondet(std::uint8_t* buf, std::size_t len)
{
	for (std::size_t i = 0; i < len; ++i) buf[i] = nondet_uchar();
}

} // namespace

// ─── Property 1: the accept/reject predicate ─────────────────────────────────

// readLE returns true if and only if n is in 1..8 and the range is inside the
// buffer. Both directions are asserted, so an implementation that is merely
// conservative -- refusing a read it could have served -- fails too.
//
// n ranges over 0..16, which covers the whole refused band. The site this
// replaces, cli_reader.cpp:706, has no upper bound on n at all: its loop stops
// at b.size() but the sign test that follows indexes b[n - 1] regardless.
extern "C" void proof_read_le_accepts_exactly_the_reads_that_fit()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	const unsigned n = nondet_uint();
	__ESBMC_assume(n <= kProbeWidth);

	std::uint64_t out = 0;
	const bool ok = readLE(buf, size, pos, n, out);

	assert(ok == (n >= 1 && n <= kMaxBytes && bounds::rangeFits(pos, size, n)));
}

extern "C" void proof_read_be_accepts_exactly_the_reads_that_fit()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	const unsigned n = nondet_uint();
	__ESBMC_assume(n <= kProbeWidth);

	std::uint64_t out = 0;
	const bool ok = readBE(buf, size, pos, n, out);

	assert(ok == (n >= 1 && n <= kMaxBytes && bounds::rangeFits(pos, size, n)));
}

// A refused read must not write `out`. Without this a caller that checks the
// return value and a caller that does not get different answers from the same
// call, and the second one gets a half-assembled value that looks plausible --
// which is exactly what makes a truncated read hard to find later.
extern "C" void proof_a_refused_read_leaves_out_alone()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	const unsigned n = nondet_uint();
	__ESBMC_assume(n <= kProbeWidth);
	const unsigned bitsPerUnit = nondet_uint();

	const std::uint64_t sentinel = nondet_uint64();

	std::uint64_t a = sentinel;
	if (!readLE(buf, size, pos, n, a)) assert(a == sentinel);

	std::uint64_t b = sentinel;
	if (!readBE(buf, size, pos, n, b)) assert(b == sentinel);

	std::uint64_t c = sentinel;
	if (!readWidened(buf, size, pos, n, bitsPerUnit, nondet_bool(), c))
		assert(c == sentinel);

	// A null buffer is refused rather than dereferenced, on all three.
	std::uint64_t d = sentinel;
	assert(!readLE(nullptr, size, pos, n, d));
	assert(!readBE(nullptr, size, pos, n, d));
	assert(!readWidened(nullptr, size, pos, n, bitsPerUnit, nondet_bool(), d));
	assert(d == sentinel);
}

// ─── Property 2: no read outside the buffer, on any path ─────────────────────

// The buffer is malloc'd at exactly the symbolic size, so ESBMC's array-bounds
// check has the real end of the object to work with. A stack array of a fixed
// length would not: a read at index `size` would land in the array's own tail
// and no checker would object.
//
// There is no assertion for the property. The property IS the absence of an
// array-bounds violation over every n in 0..16, every pos in the whole size_t
// range, and every size in 0..8. cli_reader.cpp:706 fails this at n = 4,
// size = 2 -- ESBMC reports the dereference at b[n - 1] = b[3].
extern "C" void proof_reads_touch_no_byte_outside_an_exactly_sized_buffer()
{
	const std::size_t size = nondet_size();
	__ESBMC_assume(size >= 1 && size <= kBufLen);

	std::uint8_t* buf = static_cast<std::uint8_t*>(std::malloc(size));
	__ESBMC_assume(buf != nullptr);
	fillNondet(buf, size);

	const std::size_t pos = nondet_size();
	const unsigned n = nondet_uint();
	__ESBMC_assume(n <= kProbeWidth);
	const unsigned bitsPerUnit = nondet_uint();

	std::uint64_t out = 0;
	readLE(buf, size, pos, n, out);
	readBE(buf, size, pos, n, out);
	readWidened(buf, size, pos, n, bitsPerUnit, nondet_bool(), out);

	std::free(buf);
}

// ─── Property 3: no shift count reaches the width of its operand ─────────────

// Carried by --ub-shift-check rather than by an assertion, over every width a
// caller could ask for and every bits-per-unit a format could declare. Every
// shift in the header is on a std::uint64_t, so any count at or above 64 is
// undefined and ESBMC reports it here.
//
// Two live sites fail this. byte_value_storage.cpp:958 shifts a uint64_t by
// `getByteLength() * i` with i running to realSize - 1 and realSize being
// `data.size() - offset`, so a nine-byte span reaches a count of 64.
// dex_class_parser.cpp:442 accumulates into a uint32_t while shifting by
// `b * 8` for b up to argBits, three bits straight out of the file: argBits = 4
// already gives `uint32_t << 32`, and argBits = 7 gives `uint32_t << 56`.
extern "C" void proof_no_shift_count_reaches_the_width_of_its_operand()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	const unsigned n = nondet_uint();
	__ESBMC_assume(n <= kProbeWidth);
	// Every bits-per-unit a format could declare, including 0 and the whole
	// accumulator width. RawDataFormat::getByteLength returns whatever the
	// config said, so this is not a hypothetical range.
	const unsigned bitsPerUnit = nondet_uint();
	__ESBMC_assume(bitsPerUnit <= kAccumulatorBits);

	const std::uint64_t v = nondet_uint64();

	std::uint64_t out = 0;
	readLE(buf, size, pos, n, out);
	readBE(buf, size, pos, n, out);
	readWidened(buf, size, pos, n, bitsPerUnit, nondet_bool(), out);

	std::uint8_t sink[kBufLen];
	writeLE(v, n, sink, kBufLen);
	writeBE(v, n, sink, kBufLen);

	signExtendFrom(v, bitsPerUnit);
	zeroExtendFrom(v, bitsPerUnit);
	extendHigh(v, n, static_cast<unsigned>(nondet_uint()));

	// shiftFits is the predicate the rest of the header is guarded by, so it
	// gets the same fully symbolic treatment: its own multiplication must not
	// wrap either, which --unsigned-overflow-check checks here.
	const bool fits = shiftFits(nondet_size(), nondet_uint(), nondet_uint());
	(void) fits;

	// shiftFits agrees with the product wherever the product is exact. Stated
	// only under bounds that make the product safe to form in the harness --
	// forming it unguarded is the fault being proved absent.
	const std::size_t idx = nondet_size();
	__ESBMC_assume(idx <= kProbeWidth);
	const unsigned unit = nondet_uint();
	__ESBMC_assume(unit <= kAccumulatorBits);
	assert(shiftFits(idx, unit, kAccumulatorBits) == (idx * unit < kAccumulatorBits));
}

// ─── Property 4: the byte order is exact ─────────────────────────────────────

// Stated as an arithmetic identity, not as a re-derived shift chain. A shift
// chain in the harness would agree with a shift chain in the header for the
// same wrong reason; a sum of place values does not.
//
// Every operand is a byte, so the largest value here is 4294967295 and no
// intermediate overflows -- which --unsigned-overflow-check confirms.
extern "C" void proof_little_endian_is_the_arithmetic_identity()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	__ESBMC_assume(bounds::rangeFits(pos, size, 4));

	std::uint64_t v = 0;
	assert(readLE(buf, size, pos, 4, v));

	const std::uint64_t expect =
			  static_cast<std::uint64_t>(buf[pos])
			+ static_cast<std::uint64_t>(buf[pos + 1]) * 256ull
			+ static_cast<std::uint64_t>(buf[pos + 2]) * 65536ull
			+ static_cast<std::uint64_t>(buf[pos + 3]) * 16777216ull;
	assert(v == expect);

	// The one-byte read is the identity on the byte itself, which is what
	// pins the base of the chain rather than only its shape.
	std::uint64_t one = 0;
	assert(readLE(buf, size, pos, 1, one));
	assert(one == static_cast<std::uint64_t>(buf[pos]));

	// And big-endian over the same four bytes is the reversal, not a
	// coincidence of the same sum.
	std::uint64_t b = 0;
	assert(readBE(buf, size, pos, 4, b));
	const std::uint64_t expectBE =
			  static_cast<std::uint64_t>(buf[pos + 3])
			+ static_cast<std::uint64_t>(buf[pos + 2]) * 256ull
			+ static_cast<std::uint64_t>(buf[pos + 1]) * 65536ull
			+ static_cast<std::uint64_t>(buf[pos]) * 16777216ull;
	assert(b == expectBE);
}

// The eight-byte big-endian read is the two four-byte reads concatenated. This
// is the identity JVM BinaryReader::u8 relies on (jvm_const_pool.cpp:44 builds
// it as `(hi << 32) | lo`), stated as multiplication so it does not restate the
// shift it is checking.
extern "C" void proof_big_endian_splits_at_the_word_boundary()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	std::uint64_t whole = 0, hi = 0, lo = 0;
	assert(readBE(buf, kBufLen, 0, 8, whole));
	assert(readBE(buf, kBufLen, 0, 4, hi));
	assert(readBE(buf, kBufLen, 4, 4, lo));

	// hi <= 2^32 - 1, so hi * 2^32 <= 2^64 - 2^32 and the sum with lo < 2^32
	// is at most 2^64 - 1: exact in this width, which the overflow check
	// confirms rather than the comment asserting it.
	assert(whole == hi * 4294967296ull + lo);
}

// ─── Property 5: the write path round-trips ──────────────────────────────────

// This is what pins the endianness of the WRITE path. Nothing else in the tree
// does: createBytesFromValue has no test, and a write that emits the bytes in
// the wrong order round-trips perfectly through a read that makes the matching
// mistake. Reading back with the opposite-endian reader is asserted to differ
// wherever the bytes are not a palindrome, so the two orders cannot collapse.
extern "C" void proof_le_round_trip()
{
	const std::uint64_t v = nondet_uint64();
	const std::size_t n = nondet_size();
	__ESBMC_assume(n >= 1 && n <= kMaxBytes);

	std::uint8_t buf[kBufLen];
	assert(writeLE(v, n, buf, n));

	std::uint64_t back = 0;
	assert(readLE(buf, n, 0, static_cast<unsigned>(n), back));

	// Modulo 2^(8n). lowMask(64) is all-ones, so n = 8 is the identity and
	// needs no special case here -- that is the reason lowMask is built from
	// leb128::maskFrom rather than from `(1 << bits) - 1`, which would shift
	// by 64 at exactly this point.
	assert(back == (v & lowMask(kBitsPerByte * static_cast<unsigned>(n))));
}

extern "C" void proof_be_round_trip()
{
	const std::uint64_t v = nondet_uint64();
	const std::size_t n = nondet_size();
	__ESBMC_assume(n >= 1 && n <= kMaxBytes);

	std::uint8_t buf[kBufLen];
	assert(writeBE(v, n, buf, n));

	std::uint64_t back = 0;
	assert(readBE(buf, n, 0, static_cast<unsigned>(n), back));

	assert(back == (v & lowMask(kBitsPerByte * static_cast<unsigned>(n))));

	// The two orders are genuinely different, not two spellings of one: a
	// big-endian write read back little-endian agrees only when the emitted
	// bytes are a palindrome. Without this, a writeBE that emitted
	// little-endian bytes would still pass the round trip above.
	std::uint64_t crossed = 0;
	assert(readLE(buf, n, 0, static_cast<unsigned>(n), crossed));
	bool palindrome = true;
	for (std::size_t i = 0; i < n; ++i)
		if (buf[i] != buf[n - 1 - i]) palindrome = false;
	if (!palindrome) assert(crossed != back);
}

// ─── Property 6: writes stay inside the capacity ─────────────────────────────

// The capacity is symbolic and the buffer is malloc'd at exactly it, so a write
// at index outCap is an array-bounds violation. Every byte at or above n is
// checked to still hold the pattern it was filled with, so "writes exactly n
// bytes" is a property about the buffer rather than about the loop bound.
extern "C" void proof_writes_stay_inside_the_capacity()
{
	const std::size_t outCap = nondet_size();
	__ESBMC_assume(outCap >= 1 && outCap <= kMaxCap);

	std::uint8_t* buf = static_cast<std::uint8_t*>(std::malloc(outCap));
	__ESBMC_assume(buf != nullptr);

	// A pattern no correct write produces for every byte at once, so an
	// unwritten byte is distinguishable from a written one.
	const std::uint8_t kFill = 0xAA;
	for (std::size_t i = 0; i < outCap; ++i) buf[i] = kFill;

	const std::uint64_t v = nondet_uint64();
	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kProbeWidth);

	const bool ok = writeLE(v, n, buf, outCap);
	assert(ok == (n >= 1 && n <= kMaxBytes && n <= outCap));

	if (ok)
	{
		// Nothing at or past n was touched: the write was exactly n bytes wide.
		for (std::size_t i = n; i < outCap; ++i) assert(buf[i] == kFill);
	}

	// And the same for writeBE, on a buffer refilled so the two do not stand in
	// for each other. This proof called writeLE alone until the audit pointed
	// it out: the two functions have separate index arithmetic -- one counts up
	// from the low byte, the other down from the high one -- so a proof about
	// either says nothing about the other, and the big-endian direction is the
	// one where an off-by-one lands at index n rather than at index -1.
	for (std::size_t i = 0; i < outCap; ++i) buf[i] = kFill;

	const bool okBE = writeBE(v, n, buf, outCap);
	assert(okBE == (n >= 1 && n <= kMaxBytes && n <= outCap));

	if (okBE)
	{
		for (std::size_t i = n; i < outCap; ++i) assert(buf[i] == kFill);
	}

	std::free(buf);
}

// A refused write leaves the buffer completely alone -- not a prefix, not the
// bytes that would have fitted. createBytesFromValue (byte_value_storage.cpp:978)
// resizes to a file-supplied x before it decides anything, and then runs
// `for (std::uint8_t i = 0; i < x; ++i)` against a std::uint64_t bound: for
// x = 256 the counter wraps from 255 to 0 and the loop never terminates.
//
// What this kernel does about that is NOT the counter type, and an earlier
// version of this comment said it was. The audit rewrote all three of
// byte_order.h's loops with a std::uint8_t counter -- literally the defect
// named above -- and every proof still passed, because widthFits has already
// refused any n above kMaxBytes = 8 and a uint8_t counts to 8 perfectly well.
// The fix at the call site is the refusal, not the counter: the bound reaching
// the loop is file-supplied and unbounded there, and bounded to 8 here before
// the loop is entered.
extern "C" void proof_a_refused_write_touches_nothing()
{
	const std::size_t outCap = nondet_size();
	__ESBMC_assume(outCap >= 1 && outCap <= kMaxCap);

	std::uint8_t* buf = static_cast<std::uint8_t*>(std::malloc(outCap));
	__ESBMC_assume(buf != nullptr);

	const std::uint8_t kFill = 0x5C;
	for (std::size_t i = 0; i < outCap; ++i) buf[i] = kFill;

	const std::uint64_t v = nondet_uint64();
	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kProbeWidth);

	const bool le = writeLE(v, n, buf, outCap);
	const bool be = le ? true : writeBE(v, n, buf, outCap);
	assert(be == (n >= 1 && n <= kMaxBytes && n <= outCap));

	if (!le && !be)
		for (std::size_t i = 0; i < outCap; ++i) assert(buf[i] == kFill);

	// A null destination is refused rather than written through.
	assert(!writeLE(v, n, nullptr, outCap));
	assert(!writeBE(v, n, nullptr, outCap));

	std::free(buf);
}

// ─── Property 7: readWidened refuses rather than shifting ────────────────────

// The two call sites, file_format.cpp:1998 and image.cpp:378, both write
//
//     x * getByteLength() > sizeof(res) * CHAR_BIT
//
// with x a std::uint64_t the caller supplies and getByteLength() a std::size_t
// the format supplies. The product wraps: x = 2305843009213693953 and a byte
// length of 8 gives 8 after wrapping, which is not greater than 64, so the
// guard passes and the assembly proceeds with a width of 2^61.
//
// widthFits never forms that product until both factors are known to be at most
// 64. The bound is asserted here in both directions, with the harness's own
// product guarded by the same bounds so the harness does not commit the fault
// it is proving absent.
extern "C" void proof_widened_refuses_rather_than_shifting()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	const std::size_t size = nondet_size();
	__ESBMC_assume(size <= kBufLen);
	const std::size_t pos = nondet_size();
	// Fully symbolic over the whole size_t range: this is the value that wraps.
	const std::size_t n = nondet_size();
	// Fully symbolic over the whole unsigned range: this is getByteLength().
	const unsigned bitsPerUnit = nondet_uint();

	// Held rather than drawn twice: the agreement below is with the
	// little-endian reader, so the harness has to know which order it asked
	// for. Passing nondet_bool() at both places asks for one order and checks
	// the other, and ESBMC said so -- it returned bigEndian = true, n = 4,
	// bytes 00 e4 02 02, readWidened = 429530473472 against readLE's
	// 33612544, which is the same four bytes in the other order.
	const bool bigEndian = nondet_bool();

	std::uint64_t out = 0;
	const bool ok = readWidened(buf, size, pos, n, bitsPerUnit, bigEndian, out);

	// && short-circuits, so `n * bitsPerUnit` is formed only once both factors
	// are known to be at most 64 and the product is at most 4096.
	const bool widthOk = n >= 1 && n <= kAccumulatorBits
			&& bitsPerUnit >= kBitsPerByte && bitsPerUnit <= kAccumulatorBits
			&& n * bitsPerUnit <= kAccumulatorBits;

	assert(ok == (widthOk && bounds::rangeFits(pos, size, n)));

	if (ok)
	{
		// A unit of 8 bits is the only case this tree actually meets, and it
		// must agree exactly with the plain reader -- otherwise getXByte and
		// PeReader::read32 would disagree about the same four bytes.
		if (bitsPerUnit == kBitsPerByte)
		{
			std::uint64_t plain = 0;
			if (bigEndian)
				assert(readBE(buf, size, pos, static_cast<unsigned>(n), plain));
			else
				assert(readLE(buf, size, pos, static_cast<unsigned>(n), plain));
			assert(out == plain);
		}
	}
}

// ─── Property 8: sign extension ──────────────────────────────────────────────

// The low `bits` bits survive unchanged, and bits == 0 yields 0. Stated over
// the whole 0..64 range of `bits`, including both ends, because the DEX version
// (dex_class_parser.cpp:79) special-cases exactly those two and gets the middle
// right only by relying on two implementation-defined operations.
extern "C" void proof_sign_extend_reproduces_its_low_bits()
{
	const std::uint64_t v = nondet_uint64();
	const unsigned bits = nondet_uint();
	__ESBMC_assume(bits <= kAccumulatorBits);

	const std::int64_t s = signExtendFrom(v, bits);
	// Back to unsigned without assuming anything about the value: the same
	// total conversion the kernel uses, in the other direction.
	const std::uint64_t u = static_cast<std::uint64_t>(s);

	if (bits == 0)
	{
		assert(s == 0);
	}
	else
	{
		assert(zeroExtendFrom(u, bits) == zeroExtendFrom(v, bits));
	}
}

// Every bit at or above bits - 1 equals bit bits - 1, and for bits < 64 the
// result lies in [-2^(bits-1), 2^(bits-1) - 1]. Together with the low-bits
// property above this determines signExtendFrom completely.
extern "C" void proof_sign_extend_fills_the_top_from_the_sign_bit()
{
	const std::uint64_t v = nondet_uint64();
	const unsigned bits = nondet_uint();
	__ESBMC_assume(bits >= 1 && bits <= kAccumulatorBits);

	const std::uint64_t u = static_cast<std::uint64_t>(signExtendFrom(v, bits));

	// bits - 1 is at most 63, so this shift is defined.
	const std::uint64_t sign = (v >> (bits - 1)) & 1u;
	const std::uint64_t top = retdec::utils::leb128::maskFrom(bits - 1);
	assert((u & top) == (sign != 0 ? top : std::uint64_t{0}));

	if (bits < kAccumulatorBits)
	{
		// bits - 1 is at most 62 here, so 1 << (bits - 1) is defined and the
		// bound is exact rather than saturated.
		const std::int64_t half = static_cast<std::int64_t>(std::uint64_t{1} << (bits - 1));
		const std::int64_t s = signExtendFrom(v, bits);
		assert(s >= -half);
		assert(s <= half - 1);
	}
}

// ─── Property 9: extendHigh puts the supplied bytes at the TOP ───────────────

// The DEX encoding drops the low-order zero bytes of a float or double, so the
// k bytes that survive are the MOST significant ones. dex_class_parser.cpp:95
// and :103 assemble them as the least significant ones, and the resulting bit
// pattern is a different number entirely. A probe of that site returns the
// witness value_arg = 1, bytes 00 10: the code produces 0x00001000 = 4096, a
// denormal of about 5.7e-42, where the encoding means 0x10000000, about
// 2.5e-29. 1.0f is the same story -- 0x3F800000 encodes as the two bytes
// 80 3F, and assembling them low gives 0x00003F80.
//
// Stated for totalBytes 4 and 8, which are the only two widths the encoding
// has, with k fully symbolic across each. No shift count reaches 32 or 64,
// which --ub-shift-check confirms.
extern "C" void proof_extend_high_places_supplied_bytes_at_the_top()
{
	std::uint8_t buf[kBufLen];
	fillNondet(buf, kBufLen);

	const unsigned k = nondet_uint();
	__ESBMC_assume(k >= 1 && k <= kMaxBytes);

	// The little-endian value of the k supplied bytes, as the encoding stores
	// them, straight from the proved reader.
	std::uint64_t supplied = 0;
	assert(readLE(buf, kBufLen, 0, k, supplied));

	if (k <= 4)
	{
		// A shortened VALUE_FLOAT: the k bytes are the top k bytes of a 32-bit
		// pattern. Multiplication rather than a shift, so the identity does not
		// restate the operation it is checking. 2^(8 * (4 - k)) for k in 1..4.
		std::uint64_t scale = 1;
		for (unsigned i = k; i < 4; ++i) scale *= 256ull;
		assert(extendHigh(supplied, k, 4) == supplied * scale);
		// And the whole answer fits a 32-bit pattern: nothing spilled above.
		assert(extendHigh(supplied, k, 4) <= 0xFFFFFFFFull);
	}

	// A shortened VALUE_DOUBLE. For k = 8 this is the identity, which is the
	// only case dex_class_parser.cpp:103 gets right.
	std::uint64_t scale8 = 1;
	for (unsigned i = k; i < 8; ++i) scale8 *= 256ull;
	assert(extendHigh(supplied, k, 8) == supplied * scale8);

	// Out-of-range requests are refused rather than shifted by a count they
	// cannot justify.
	assert(extendHigh(supplied, k, 0) == 0);
	assert(extendHigh(supplied, 0, 8) == 0);
	assert(extendHigh(supplied, k, k > 1 ? k - 1 : 0) == 0);
	assert(extendHigh(supplied, k, kMaxBytes + 1) == 0);
}

// ─── Property 10: the two extension paths are distinct ───────────────────────

// A caller asking for zero extension never reaches signExtendFrom, and the
// difference is observable rather than notional. dex_class_parser.cpp:92 routes
// VALUE_CHAR -- unsigned by the DEX specification -- through the sign-extending
// path, so a one-byte 0x80 decodes as -128 instead of 128.
//
// Proved as: the zero-extended answer never has a bit at or above `bits` set
// (so it is never sign-extended by accident), and whenever the sign bit is set
// and there is room above it, the two answers genuinely differ.
extern "C" void proof_zero_and_sign_extension_are_distinct()
{
	const std::uint64_t v = nondet_uint64();
	const unsigned bits = nondet_uint();
	__ESBMC_assume(bits >= 1 && bits <= kAccumulatorBits);

	const std::uint64_t z = zeroExtendFrom(v, bits);
	const std::uint64_t s = static_cast<std::uint64_t>(signExtendFrom(v, bits));

	// Zero extension sets no bit at or above `bits`. maskFrom(64) is 0, so
	// bits == 64 says the whole word is allowed, which is right.
	assert((z & retdec::utils::leb128::maskFrom(bits)) == 0);

	// The low bits are the same either way; only the top differs.
	assert(zeroExtendFrom(s, bits) == z);

	const bool signSet = ((v >> (bits - 1)) & 1u) != 0;
	if (bits < kAccumulatorBits && signSet)
	{
		// There is room above the sign bit and the sign is set, so the
		// sign-extended answer has bits the zero-extended one does not. This
		// is the 0x80-becomes-minus-128 case at bits = 8.
		assert(s != z);
		assert(signExtendFrom(v, bits) < 0);
	}
	else
	{
		// bits == 64 leaves no room above the sign, and a clear sign fills
		// nothing: the two answers must coincide exactly.
		assert(s == z);
	}
}
