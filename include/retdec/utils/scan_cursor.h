/**
 * @file include/retdec/utils/scan_cursor.h
 * @brief A cursor over an untrusted buffer that cannot fail to advance.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * bounds.h answers "does this read fit". It does not answer "does this loop
 * finish", and every timeout-* and oom-* artifact in this tree is the second
 * question. The surveyors found nine independent instances of the same
 * mistake, and they are one rule: a walk over an untrusted buffer must advance
 * strictly and must stay in range, and a refusal must leave the cursor where
 * the caller can see it.
 *
 * The instances are worth naming, because each looks different and none of
 * them looks wrong:
 *
 *   - src/dex_parser/dex_lifter.cpp:312. parseDexProto's array arm does
 *     `end = proto.find(';', j); ... i = end + 1;` without the
 *     `end == npos` guard its sibling 'L' arm has. On the descriptor "([L)V"
 *     there is no ';', so end is npos, `i = end + 1` is 0, and the parse
 *     restarts at the '('. The cursor then oscillates 0, 1, 0, 1, pushing two
 *     parameters per cycle until the kMaxParams = 65535 cap stops it. The cap
 *     is the only thing that stops it.
 *   - src/utils/byte_value_storage.cpp:1077. getNTWSImpl ends its loop with
 *     `address += width`. A caller asking for a wide string of width 0 reads
 *     the same non-zero element forever and pushes it into `tmp` each time.
 *   - src/jvm_parser/jvm_lifter.cpp:234. findLeaders steps by
 *     `instrSize(op)` and then writes `if (sz <= 0) sz = 1;` -- a step of zero
 *     is coerced to one rather than refused, so an undefined opcode
 *     (0xCA..0xFF) or a wrong table entry does not stop the scan, it silently
 *     puts the cursor on an operand byte and decodes it as an opcode.
 *   - src/cli_parser/cli_sig.cpp:126, 131, 216. `decodeCompressedUInt(...)
 *     .value_or(0)` turns a refusal into the value zero, so the loop that
 *     consumes the declared count runs its full length with the cursor parked
 *     at the end of the blob, and the count itself is never compared against
 *     the bytes left.
 *   - src/cli_parser/cil_lifter.cpp:322-409. The exception-section walk
 *     terminates only because `sectStart += 4` sits outside the branch that
 *     needs it; the branch that is supposed to advance does
 *     `sectStart += dataSize` with a dataSize that can be zero.
 *
 * So the cursor state lives here, and the invariant is established by
 * construction rather than maintained by convention. That is exactly the
 * difference between DexReader::check (src/dex_parser/dex_header.cpp:34),
 * which is sound because it compares by subtraction, and BinaryReader::check
 * (src/jvm_parser/jvm_const_pool.cpp:16), which forms `pos_ + n` and is not.
 *
 * Two rules the callers get for free:
 *
 *   - `advance` refuses a step of zero. Progress is not something a walk has
 *     to remember to make; it is what an accepted advance means.
 *   - `pos + step` is never formed anywhere in this header. Every comparison
 *     is against the bytes remaining, which cannot wrap.
 *
 * `seek` is deliberately not covered by the termination argument. A format
 * with an offset table genuinely needs to reposition, and refusing a backward
 * seek would make those callers write their own. What is guaranteed of seek is
 * only that it lands in range or refuses; a walk that wants a termination
 * bound must move with `advance` alone (see @ref maxSteps).
 *
 * Header-only, constexpr, no allocation, no exceptions, raw pointers and sizes
 * rather than containers. Proved in tests/verification/scan_cursor_proof.cpp.
 * Do not re-derive this at a call site; call it.
 */

#ifndef RETDEC_UTILS_SCAN_CURSOR_H
#define RETDEC_UTILS_SCAN_CURSOR_H

#include "retdec/utils/bounds.h"

#include <cstddef>
#include <cstdint>

namespace retdec {
namespace utils {
namespace scan {

/// A position in a buffer, carried together with the buffer's size.
///
/// The two are one value because every bug above came from carrying them
/// separately: a `pos` that some other line advanced, compared against a
/// `size` that meant something slightly different.
struct Cursor
{
	std::size_t pos = 0;  ///< bytes consumed so far; always <= size
	std::size_t size = 0; ///< bytes the buffer holds
};

/// A cursor at the start of a buffer of @p size bytes.
constexpr Cursor cursorOver(std::size_t size) noexcept
{
	return Cursor{0, size};
}

/// The class invariant: the cursor is at or before the end of its buffer.
///
/// Every function below preserves this, and nothing below can produce a cursor
/// that violates it, so a caller never has to test it. It is stated so the
/// proof can.
constexpr bool valid(const Cursor& c) noexcept
{
	return c.pos <= c.size;
}

/// Bytes still readable at the cursor.
constexpr std::size_t left(const Cursor& c) noexcept
{
	return bounds::remaining(c.pos, c.size);
}

/// True when @p len bytes can be read at the cursor.
///
/// Delegates to bounds::rangeFits rather than writing `c.pos + len <= c.size`,
/// which is the formulation that passes when the sum wraps.
constexpr bool fits(const Cursor& c, std::size_t len) noexcept
{
	return bounds::rangeFits(c.pos, c.size, len);
}

/// Move the cursor forward by @p step, or refuse and leave it untouched.
///
/// Refuses when @p step is 0. That is the whole point of this header: a walk
/// driven by a size that a file chose can be handed a zero, and the two
/// answers a caller can give -- loop forever (byte_value_storage.cpp:1077) or
/// substitute a 1 (jvm_lifter.cpp:343) -- are both wrong. Refusing makes the
/// malformed step visible at the point it is used.
///
/// Refuses when the step would leave the buffer, so `valid` is preserved.
/// On a refusal the cursor is bitwise unchanged, which is the leb128.h
/// convention: a caller that has just been told "no" must not also have to
/// wonder where its cursor ended up.
constexpr bool advance(Cursor& c, std::size_t step) noexcept
{
	if (step == 0) return false;
	if (!fits(c, step)) return false;
	// Safe now, and only now: fits() has established step <= size - pos.
	c.pos += step;
	return true;
}

/// Put the cursor at absolute offset @p pos, or refuse and leave it untouched.
///
/// For the formats that hand out offsets rather than lengths -- a DEX map
/// list, a .NET stream header, an ELF section table. `pos == c.size` is
/// accepted: that is the end of the buffer, where a walk legitimately finishes.
///
/// A seek may move backwards. See the file comment: that is a real need, and
/// it is why @ref maxSteps bounds walks that use @ref advance only.
constexpr bool seek(Cursor& c, std::size_t pos) noexcept
{
	if (pos > c.size) return false;
	c.pos = pos;
	return true;
}

/// The most accepted @ref advance calls a walk from @p c can make, given that
/// every step is at least @p minStep bytes.
///
/// This is the termination bound. A walk whose body advances by at least
/// @p minStep on every iteration -- which, since advance refuses 0, means
/// minStep = 1 for any walk at all -- runs at most this many times, because
/// each accepted advance consumes at least that many of the bytes left and
/// there are only `left(c)` of them.
///
/// @p minStep of 0 is treated as 1, so a header declaring zero-width elements
/// cannot make the bound vacuous. That is the same corner bounds::countFits
/// handles, and it is also what keeps this from dividing by zero.
constexpr std::size_t maxSteps(const Cursor& c, std::size_t minStep) noexcept
{
	const std::size_t step = minStep == 0 ? 1 : minStep;
	return left(c) / step;
}

/// True when a container declaring @p count elements could be satisfied by the
/// bytes left at the cursor.
///
/// bounds::countFits stated on the cursor, so the four unbounded declared-count
/// loops become one call rather than four hand-written comparisons. The
/// composition proved for countFits carries over: whatever this accepts,
/// `fits(c, count * minBytesPerElement)` also accepts, with no wrap in the
/// multiplication.
constexpr bool countFitsAt(const Cursor& c, std::size_t count, std::size_t minBytesPerElement = 1) noexcept
{
	return bounds::countFits(c.pos, c.size, count, minBytesPerElement);
}

/// True when @p a is a power of two, and not zero.
///
/// Alignment arithmetic is undefined for anything else -- `value & ~(a - 1)`
/// with a = 3 clears the wrong bits -- and the callers take their alignment
/// from the file, so it has to be asked rather than assumed.
constexpr bool isPowerOfTwo(std::size_t a) noexcept
{
	return a != 0 && (a & (a - 1)) == 0;
}

/// Round the cursor up to the next multiple of @p a, or refuse and leave it
/// untouched.
///
/// Refuses when @p a is not a power of two, and when the padding would leave
/// the buffer. Never decreases c.pos -- which is the property the CIL section
/// walk depends on and does not have.
///
/// Computes the padding from the remainder rather than by forming
/// `pos + (a - 1)`. That sum is where this goes wrong everywhere it is written
/// out by hand: at cil_lifter.cpp:326 and 408 as
/// `sectStart = (sectStart + 3) & ~3ULL`, for a position within 3 bytes of
/// SIZE_MAX the sum wraps and the result is *smaller* than the input --
/// pos = 0xFFFFFFFFFFFFFFFE rounds to 0, which sends the section walk back to
/// the start of the buffer. From the remainder the sum is never formed at all,
/// and bounds::rangeFits then decides whether the padding fits.
///
/// retdec::utils::align::alignUp in align.h is the same rule stated for a bare
/// value, and it is proved in tests/verification/align_proof.cpp. This does not
/// call it because a cursor move is not just an arithmetic result: it has to
/// leave the cursor untouched on refusal, which is a property of this type and
/// not of the number. (retdec::utils::alignUp, the older non-constexpr helper
/// in src/utils/alignment.cpp, could not be used from a header-only kernel in
/// any case -- it now calls align.h itself.)
constexpr bool alignForward(Cursor& c, std::size_t a) noexcept
{
	if (!isPowerOfTwo(a)) return false;
	// a is a power of two, so a - 1 is its mask and this is c.pos mod a.
	const std::size_t rem = c.pos & (a - 1);
	if (rem == 0) return true;
	const std::size_t pad = a - rem;
	if (!fits(c, pad)) return false;
	c.pos += pad;
	return true;
}

/// The step @p table declares for @p key, or 0 when it declares none.
///
/// A key outside the table is 0 -- an undefined JVM opcode, a DEX unit that is
/// not an instruction -- which @ref advanceByTable then refuses. The
/// alternative, indexing anyway, is how a 256-entry table gets read at 0x1F4.
constexpr std::size_t stepFromTable(const std::uint8_t* table, std::size_t count, std::size_t key) noexcept
{
	if (table == nullptr || key >= count) return 0;
	return table[key];
}

/// Advance by the step @p table declares for @p key, or refuse.
///
/// This is the shape all three lifters walk their bytecode with: an opcode
/// indexes a size table and the size becomes the step. Routing it through
/// @ref advance is what makes a zero entry a refusal instead of a hang
/// (byte_value_storage.cpp:1077) or a silent one-byte slide
/// (jvm_lifter.cpp:343). The walk then terminates for EVERY table, including a
/// wrong one -- which matters, because the table at jvm_lifter.cpp:189 has
/// twenty-three wrong entries today.
constexpr bool advanceByTable(Cursor& c, const std::uint8_t* table, std::size_t count, std::size_t key) noexcept
{
	return advance(c, stepFromTable(table, count, key));
}

/// True when every entry of @p table is a real step, so no key can stall a walk.
///
/// The call-site obligation, made checkable. A walk over a table satisfying
/// this refuses only at the end of the buffer, never in the middle; a walk over
/// a table that does not still terminates, it just stops early. Both are safe;
/// this distinguishes them.
///
/// An empty table -- or no table at all -- is false, not vacuously true. The
/// vacuous reading is the tempting one, since a table with no entries has no
/// zero entry in it, and it was what this returned. It makes the contract above
/// false: stepFromTable has nothing to return but 0 for a count of 0, so
/// `advanceByTable(c, table, 0, key)` refuses for EVERY key with the buffer
/// still full, which is precisely the stall in the middle that this predicate
/// exists to rule out. Found by the audit, not by a proof -- the proofs all fix
/// count at kTableSize, so none of them could see it.
constexpr bool tableAlwaysAdvances(const std::uint8_t* table, std::size_t count) noexcept
{
	if (table == nullptr || count == 0) return false;
	for (std::size_t i = 0; i < count; ++i)
		if (table[i] == 0) return false;
	return true;
}

} // namespace scan
} // namespace utils
} // namespace retdec

#endif // RETDEC_UTILS_SCAN_CURSOR_H
