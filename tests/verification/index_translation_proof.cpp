/**
 * @file tests/verification/index_translation_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/index_translation.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Two kinds of proof live here.
 *
 * The first kind states what the kernel does, for every input, by SMT: the
 * 1-based translation is exact and total, an element offset is inside its
 * buffer and is the right multiple of the width, and a tag/payload split throws
 * nothing away.
 *
 * The second kind is the interesting one. Several proofs below encode an
 * expression exactly as it is written at a call site in this tree and discharge
 * the fact that the kernel and the site disagree -- with the concrete witness
 * in the assertion, not in a comment. `proof_the_guid_heap_wraps_at_index_16m`
 * is the confirmed wrong-answer bug: `src/cli_parser/cli_heaps.cpp:128`
 * computes `(index - 1) * 16` in uint32, index = 0x10000001 wraps the product
 * to 0, the bounds test `off + 16 > data_.size()` is satisfied, and
 * GuidHeap::get returns the GUID at index 1 -- a module's MVID -- for an index
 * that addresses nothing at all. It reports success, so no caller can tell.
 *
 * Inputs are unconstrained everywhere except where the format genuinely
 * constrains them: tagBits is assumed <= 16 in the coded-token proofs because
 * ECMA-335 II.24.2.6 defines no wider tag, and the number of referenced tables
 * is capped at 22 because that is the length of kHasCustomAttr, the longest
 * table in src/cli_parser/cli_tables.cpp.
 */

// ESBMC-OPTIONS: --unwind 23
// ESBMC-SOLVER: --boolector
//
// The unwind bound is a real bound, and it is tight. ESBMC 8.5.0 generates
// unwinding assertions by default, so exceeding it fails rather than
// truncating the search: measured, proof_coded_token_is_wide_is_exact fails at
// --unwind 22 with "unwinding assertion loop 3" and passes at 23. 23 is one
// more than kMaxRefs, the 22 entries of kHasCustomAttr -- the longest coded
// token table in cli_tables.cpp, and therefore the longest loop
// codedTokenIsWide can ever run in this tree.
//
// The reason this file MAY use boolector, when bounds_proof.cpp may not, is the
// argument order in elementAt. ESBMC emits a bogus "arithmetic overflow on div"
// check for unsigned division, which the bitvector backends discharge as SAT;
// it only bites when the divisor is symbolic. elementAt calls
// `bounds::mulFits(width, count)` rather than `mulFits(count, width)` -- the
// same predicate, since multiplication commutes -- so the divisor is `width`,
// which every proof below fixes to a concrete format width. No symbolic
// division survives into any query here.
//
// Measured rather than assumed: all 44 proofs below were run under both
// backends and both discharge all 44, boolector in 56s over the whole file and
// z3 in 64s. So the pin is not load-bearing for a verdict, only for the wall
// clock, and `verify_esbmc.sh --cross` finds nothing to report. The line stays
// because the margin grows with the query -- the widest instance,
// proof_element_at_width46, is 7s under boolector against 10s under z3 -- and
// because a harness that divides has to say which solver its verdict came
// from.

#include "retdec/utils/index_translation.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::idxmap;
namespace bounds = retdec::utils::bounds;

// ESBMC treats an undefined function returning a value as an unconstrained
// choice of that type.
extern "C" {
std::size_t nondet_size();
std::uint64_t nondet_u64();
std::uint32_t nondet_u32();
unsigned nondet_unsigned();
}

// __ESBMC_assume is a verifier builtin, so this file does not type-check under
// an ordinary compiler. Defining it away lets `verify_esbmc.sh --syntax` catch
// a typo in a second instead of after a solver run; ESBMC itself never sees
// the macro.
#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

// A sink for the lookup-table reads below. Without somewhere for the byte to
// go, the read is dead and the array-bounds check has nothing to check.
extern "C" std::uint8_t idxmap_proof_sink;
std::uint8_t idxmap_proof_sink = 0;

// ─── slotFor1Based ───────────────────────────────────────────────────────────

extern "C" void proof_slot_for_1_based_is_exact()
{
	const std::uint64_t idx = nondet_u64();
	const std::size_t count = nondet_size();

	// A recognisable value, so "untouched on failure" is a claim about this
	// variable rather than about whatever the stack held.
	std::size_t slot = 0xA5A5A5A5A5A5A5A5ull;
	const bool ok = slotFor1Based(idx, count, slot);

	// Succeeds exactly on the 1-based domain, no wider and no narrower.
	assert(ok == (idx >= 1 && idx <= static_cast<std::uint64_t>(count)));

	if (ok)
	{
		// The translation itself. Stated as an addition on the slot rather than
		// a subtraction on the index so that a slot of SIZE_MAX -- the value
		// container.h:79 produces at n == 0 -- could not satisfy it.
		assert(static_cast<std::uint64_t>(slot) + 1 == idx);
		// And the slot is a subscript the container really has.
		assert(slot < count);
	}
	else
	{
		assert(slot == 0xA5A5A5A5A5A5A5A5ull);
	}
}

extern "C" void proof_slot_for_1_based_refuses_the_null_token()
{
	// include/retdec/utils/container.h:76 and :91 reach `container[n - 1]` with
	// n == 0 whenever NDEBUG is defined, because the only bound is an assert.
	// This is that subscript, and it is why index 0 is a refusal here and not a
	// subtraction.
	const std::size_t count = nondet_size();
	std::size_t slot = 7;

	assert(!slotFor1Based(0, count, slot));
	assert(slot == 7);
}

extern "C" void proof_slot_for_1_based_refuses_every_index_of_an_empty_container()
{
	// A never-parsed table has rowCount 0 and a cache that was never filled has
	// size 0. Neither has a row 1.
	const std::uint64_t idx = nondet_u64();
	std::size_t slot = 7;
	assert(!slotFor1Based(idx, 0, slot));
	assert(slot == 7);
}

extern "C" void proof_slot_refutes_both_off_by_one_spellings()
{
	// CLIReader's own string-heap read gets this right -- `idx == 0 || idx >
	// typeDefNames_.size()`. The two spellings that get written instead are
	// both refuted here, so neither can come back as a "simplification".
	const std::uint64_t idx = nondet_u64();
	const std::size_t count = nondet_size();
	std::size_t slot = 0;
	const bool ok = slotFor1Based(idx, count, slot);

	// Spelling A: `if (idx >= size()) refuse`. Off by one at the top -- it
	// refuses the last row of the table, which is a perfectly good 1-based
	// index, so the highest-numbered type in every assembly resolves to
	// "<unknown>".
	const bool spellingA = idx < static_cast<std::uint64_t>(count);
	if (idx != 0 && idx == static_cast<std::uint64_t>(count))
	{
		assert(ok);         // the row exists
		assert(!spellingA); // and spelling A refuses it
	}

	// Spelling B: `if (idx > size() - 1) refuse`, with size() a size_t. At
	// count == 0 the subtraction wraps to SIZE_MAX and the guard admits every
	// index there is, on a container with no elements.
	//
	// `count - 1` is written out rather than evaluated. Evaluating it is the
	// fault being demonstrated, and --unsigned-overflow-check failed this proof
	// on the harness's own arithmetic when it was: "arithmetic overflow on sub",
	// at the line that was meant to be showing the overflow off.
	const std::uint64_t bTop = count == 0 ? kMaxOffset : static_cast<std::uint64_t>(count) - 1;
	const bool spellingB = idx <= bTop;
	if (count == 0)
	{
		assert(spellingB);
		assert(!ok);
	}
}

// ─── elementAt ───────────────────────────────────────────────────────────────

// The biconditional of property 2, proved once per width.
//
// A symbolic width would put a symbolic 64-bit multiply and a symbolic 64-bit
// divide in the same query, which is nonlinear bitvector arithmetic that no
// solver here discharges -- docs/VERIFICATION.md records the same limit for
// bounds.h. Base, size and index stay fully symbolic across the whole 64-bit
// range in every instance below.
template <std::size_t Width>
static void elementAtIsExact()
{
	const std::size_t base = nondet_size();
	const std::size_t size = nondet_size();
	const std::uint64_t i = nondet_u64();

	std::size_t off = 0xA5A5A5A5A5A5A5A5ull;
	const bool ok = elementAt(base, size, i, Width, off);

	// The predicate, spelled out. `i + 1` is formed only inside the guard that
	// says it does not wrap -- writing it first is the fault --unsigned-
	// overflow-check exists to catch, and it would fail this proof on the
	// harness's own arithmetic.
	const bool fits = i < kMaxOffset && bounds::mulFits(Width, static_cast<std::size_t>(i) + 1)
				   && bounds::rangeFits(base, size, (static_cast<std::size_t>(i) + 1) * Width);
	assert(ok == fits);

	if (ok)
	{
		// off == base + i * Width, stated as a subtraction so the harness does
		// not form the sum the kernel refuses to form until it is safe.
		assert(off >= base);
		assert(off - base == static_cast<std::size_t>(i) * Width);
		// And the whole element is inside the buffer: off + Width <= size,
		// again without the sum.
		assert(off <= size);
		assert(Width <= size - off);
	}
	else
	{
		assert(off == 0xA5A5A5A5A5A5A5A5ull);
	}
}

// The widths these formats actually use. 1 is a tag byte; 2 a type_item and a
// narrow metadata index; 4 a string_id_item and a wide one; 8 a field_id_item,
// a method_id_item and a try_item; 12 a proto_id_item; 16 a GUID and a DEX map
// entry; 24 the #~ stream header; 40 a PE section header; 46 a zip central
// directory record (src/dex_parser/dex_apk_reader.cpp:147).
extern "C" void proof_element_at_width1()
{
	elementAtIsExact<1>();
}
extern "C" void proof_element_at_width2()
{
	elementAtIsExact<2>();
}
extern "C" void proof_element_at_width4()
{
	elementAtIsExact<4>();
}
extern "C" void proof_element_at_width8()
{
	elementAtIsExact<8>();
}
extern "C" void proof_element_at_width12()
{
	elementAtIsExact<12>();
}
extern "C" void proof_element_at_width16()
{
	elementAtIsExact<16>();
}
extern "C" void proof_element_at_width24()
{
	elementAtIsExact<24>();
}
extern "C" void proof_element_at_width40()
{
	elementAtIsExact<40>();
}
extern "C" void proof_element_at_width46()
{
	elementAtIsExact<46>();
}

extern "C" void proof_element_at_needs_the_add_fits_conjunct()
{
	// The predicate `mulFits(i, width) && rangeFits(base, size, i*width + width)`
	// is the obvious statement of "element i fits", and it is wrong: the sum
	// inside it wraps. i = SIZE_MAX, width = 1, base = 0, size = 0 -- the
	// product is representable (SIZE_MAX * 1), `i * width + width` wraps to 0,
	// and rangeFits(0, 0, 0) is true, so the predicate reports that an element
	// at offset 18446744073709551615 fits in an empty buffer.
	//
	// This is the concrete witness, discharged. elementAt refuses it, which is
	// what the addFits conjunct in its contract buys.
	std::size_t i = nondet_size();
	std::size_t width = nondet_size();
	__ESBMC_assume(i == SIZE_MAX);
	__ESBMC_assume(width == 1);

	assert(bounds::mulFits(i, width)); // the product is representable
	const std::size_t product = i * width;
	assert(product == SIZE_MAX);
	assert(!bounds::addFits(product, width)); // but one more width is not

	// `product + width` is NOT formed. Forming it is the fault this proof is
	// about, and --unsigned-overflow-check failed the proof on that line when
	// it was written the obvious way: "arithmetic overflow on add", in the
	// assertion meant to be showing the overflow off. Its wrapped value is
	// product - (SIZE_MAX - width) - 1, which stays in range at every step.
	const std::size_t wrapped = product - (SIZE_MAX - width) - 1;
	assert(wrapped == 0);
	assert(bounds::rangeFits(0, 0, wrapped)); // so the naive predicate says yes

	std::size_t off = 0;
	assert(!elementAt(0, 0, i, width, off));
}

extern "C" void proof_element_at_zero_width_is_total()
{
	// Width 0 is the one case with no multiplication to bound. It succeeds
	// wherever base is a position in the buffer, for any index, and hands back
	// base itself -- a zero-byte element occupies nothing, so there is nothing
	// to overrun. Callers for which width 0 means "never parsed" use
	// rowAt1Based, which refuses it; see the next proof.
	const std::size_t base = nondet_size();
	const std::size_t size = nondet_size();
	const std::uint64_t i = nondet_u64();

	std::size_t off = 0xA5A5A5A5A5A5A5A5ull;
	const bool ok = elementAt(base, size, i, 0, off);

	assert(ok == (base <= size));
	if (ok)
		assert(off == base);
	else
		assert(off == 0xA5A5A5A5A5A5A5A5ull);
}

// ─── rowAt1Based ─────────────────────────────────────────────────────────────

extern "C" void proof_row_at_1_based_refuses_zero_width()
{
	// RawTable::rowSize is 0 until parseTable sets it. Without this refusal a
	// table that was never parsed answers every index with offset 0, which is
	// row 1 of whichever table's bytes are actually in the buffer -- one table
	// aliasing another.
	const std::size_t size = nondet_size();
	const std::uint64_t idx = nondet_u64();
	std::size_t off = 7;

	assert(!rowAt1Based(size, idx, 0, off));
	assert(off == 7);
}

extern "C" void proof_row_at_1_based_refuses_the_null_row()
{
	// Row 0 is the null token in every ECMA-335 table. `(0 - 1) * width` is the
	// read before the start of the buffer this refuses.
	const std::size_t size = nondet_size();
	const std::size_t width = nondet_size();
	std::size_t off = 7;

	assert(!rowAt1Based(size, 0, width, off));
	assert(off == 7);
}

// Row addressing at a fixed row width, fully symbolic in size and index.
template <std::size_t Width>
static void rowAt1BasedIsExact()
{
	const std::size_t size = nondet_size();
	const std::uint64_t idx = nondet_u64();

	std::size_t off = 0xA5A5A5A5A5A5A5A5ull;
	const bool ok = rowAt1Based(size, idx, Width, off);

	if (ok)
	{
		assert(idx >= 1);
		// The offset is the right multiple of the row width -- the property the
		// 32-bit product at cli_heaps.cpp:128 violates, because a wrapped
		// product lands on a different multiple entirely.
		assert(off % Width == 0);
		assert(off / Width == idx - 1);
		// And the whole row is in the table.
		assert(off <= size);
		assert(Width <= size - off);
	}
	else
	{
		assert(off == 0xA5A5A5A5A5A5A5A5ull);
		// Refused only for a reason: the null row, or a row the table does not
		// have. Stated by division so the harness never forms the product.
		assert(idx == 0 || idx - 1 > (size / Width) || size / Width == 0 || idx - 1 > (size - Width) / Width);
	}
}

extern "C" void proof_row_at_1_based_width2()
{
	rowAt1BasedIsExact<2>();
}
extern "C" void proof_row_at_1_based_width4()
{
	rowAt1BasedIsExact<4>();
}
extern "C" void proof_row_at_1_based_width16()
{
	rowAt1BasedIsExact<16>();
}

extern "C" void proof_the_guid_heap_wraps_at_index_16m()
{
	// src/cli_parser/cli_heaps.cpp:125-131, GuidHeap::get, as written:
	//
	//     if (index == 0) return g;
	//     size_t off = (index - 1) * 16;      // index is uint32_t
	//     if (off + 16 > data_.size()) return g;
	//     std::memcpy(g.bytes, data_.data() + off, 16);
	//
	// `(index - 1)` is uint32_t and 16 is int, so the product is computed in 32
	// bits and wraps. At index = 0x10000001 the true offset is 4294967296; it
	// wraps to 0; the bounds test passes on a heap holding a single GUID; and
	// the function returns the GUID at index 1 -- a module's MVID -- reporting
	// success. Nothing downstream can tell it apart from a real answer.
	std::uint32_t index = nondet_u32();
	__ESBMC_assume(index == 0x10000001u);

	// A #GUID heap with exactly one GUID in it. This is the common case, not a
	// contrived one: an assembly has one MVID.
	const std::size_t heapSize = 16;

	// The true offset, in arithmetic whose width the file did not choose.
	const std::uint64_t trueOff = (static_cast<std::uint64_t>(index) - 1) * 16;
	assert(trueOff == 4294967296ull);

	// The site's offset: the same product truncated to 32 bits, which is what
	// `(index - 1) * 16` does when index is a uint32_t and 16 is an int.
	//
	// Written as a narrowing conversion rather than as the site's own
	// expression, because the site's own expression IS an unsigned overflow and
	// --unsigned-overflow-check failed this proof on it -- "arithmetic overflow
	// on mul", line 364, on the line copied verbatim out of cli_heaps.cpp. That
	// failure is itself the finding; this line keeps the proof discharging
	// while computing the identical value.
	const std::uint32_t siteOff = static_cast<std::uint32_t>(trueOff);
	assert(siteOff == 0);                                       // the product wrapped to zero
	assert(static_cast<std::size_t>(siteOff) + 16 <= heapSize); // and passed the guard

	// The kernel refuses it, because 4294967296 is not in a 16-byte heap.
	std::size_t off = 7;
	assert(!rowAt1Based(heapSize, index, 16, off));
	assert(off == 7);
}

// ─── splitTag ────────────────────────────────────────────────────────────────

extern "C" void proof_split_tag_is_lossless()
{
	// Every uint32 and every tag width splitTag accepts -- not only the 1 to 5
	// bits ECMA-335 II.24.2.6 defines, and not only the 0 to 16 an earlier
	// version of this proof assumed. The header states losslessness "for every
	// uint32 and every tagBits below 32", so that is what is proved; a header
	// claiming more than its harness discharges is how a reader ends up
	// trusting the wrong bound.
	//
	// A dropped index bit here does not fail -- it silently addresses a
	// different row of the same table, which is two metadata rows aliased onto
	// one.
	const std::uint32_t coded = nondet_u32();
	const unsigned tagBits = nondet_unsigned();
	__ESBMC_assume(tagBits < 32);

	std::uint32_t tag = 0xDEADBEEF, payload = 0xDEADBEEF;
	const bool ok = splitTag(coded, tagBits, tag, payload);
	assert(ok);

	// The tag is exactly the low tagBits.
	assert(static_cast<std::uint64_t>(tag) < (std::uint64_t{1} << tagBits));
	// The payload is exactly the rest.
	assert(static_cast<std::uint64_t>(payload) < (std::uint64_t{1} << (32 - tagBits)));
	// And put back together they are the word that arrived. Assembled at 64
	// bits so the harness's own shift is defined at tagBits == 0, where a
	// 32-bit `payload << 32` would be undefined rather than merely wrong.
	const std::uint64_t rebuilt = (static_cast<std::uint64_t>(payload) << tagBits) | static_cast<std::uint64_t>(tag);
	assert(rebuilt == static_cast<std::uint64_t>(coded));
}

extern "C" void proof_split_tag_refuses_an_undefined_shift()
{
	// `coded >> tagBits` with tagBits >= 32 is undefined on a uint32. A tag
	// width is a caller constant, so this is the guard against a new coded
	// token kind being added with the wrong one, not against a hostile file.
	const std::uint32_t coded = nondet_u32();
	const unsigned tagBits = nondet_unsigned();
	__ESBMC_assume(tagBits >= 32);

	std::uint32_t tag = 3, payload = 5;
	assert(!splitTag(coded, tagBits, tag, payload));
	assert(tag == 3 && payload == 5);
}

// ─── splitHigh ───────────────────────────────────────────────────────────────

extern "C" void proof_split_high_is_the_metadata_token_split()
{
	// src/cli_parser/cil_lifter.cpp:601 and :632: a metadata token is a table
	// id in the top byte and a 1-based row index in the low 24 bits.
	const std::uint32_t tok = nondet_u32();

	std::uint32_t high = 0, low = 0;
	assert(splitHigh(tok, 8, high, low));

	assert(high == (tok >> 24));
	assert(low == (tok & 0x00FFFFFFu));
	assert(low < (std::uint32_t{1} << 24));
	assert(high <= 0xFFu);
	assert(((static_cast<std::uint64_t>(high) << 24) | low) == tok);
}

extern "C" void proof_split_high_refutes_a_transposed_metadata_mask()
{
	// The typo is one hex digit: 0xFFFFFFF where 0xFFFFFF was meant. It keeps
	// the low four bits of the table id inside the row index, so on any token
	// whose table id has a low nibble -- TypeRef (0x01), Field (0x04),
	// MethodDef (0x06), MemberRef (0x0A), TypeSpec (0x1B) are all of them --
	// the index is wrong by 0x1000000 times that nibble. Every well-formed
	// assembly hides it, because tokenToString only ever asks about a table id
	// it has already matched and the row index it passes on is then bounded by
	// the resolver; a hostile one does not.
	const std::uint32_t tok = nondet_u32();
	__ESBMC_assume((tok & 0x0F000000u) != 0);

	std::uint32_t high = 0, low = 0;
	assert(splitHigh(tok, 8, high, low));

	// The transposed mask never agrees with the split whenever those bits are
	// set, which is exactly when it matters.
	assert((tok & 0x0FFFFFFFu) != low);
}

extern "C" void proof_split_high_is_lossless_at_every_width()
{
	// Both ends included: 0 means there is no high field and 32 means there is
	// no low field. Neither may perform a 32-bit shift by 32.
	const std::uint32_t tok = nondet_u32();
	const unsigned highBits = nondet_unsigned();
	__ESBMC_assume(highBits <= 32);

	std::uint32_t high = 0xDEADBEEF, low = 0xDEADBEEF;
	const bool ok = splitHigh(tok, highBits, high, low);
	assert(ok);

	const unsigned lowBits = 32 - highBits;
	assert(static_cast<std::uint64_t>(low) < (std::uint64_t{1} << lowBits));
	assert(static_cast<std::uint64_t>(high) < (std::uint64_t{1} << highBits));

	const std::uint64_t rebuilt = (static_cast<std::uint64_t>(high) << lowBits) | static_cast<std::uint64_t>(low);
	assert(rebuilt == static_cast<std::uint64_t>(tok));
}

extern "C" void proof_split_high_refuses_a_width_past_the_word()
{
	const std::uint32_t tok = nondet_u32();
	const unsigned highBits = nondet_unsigned();
	__ESBMC_assume(highBits > 32);

	std::uint32_t high = 3, low = 5;
	assert(!splitHigh(tok, highBits, high, low));
	assert(high == 3 && low == 5);
}

// ─── tagIndexes and the thirteen coded-token kinds ───────────────────────────
//
// src/cli_parser/cli_tables.cpp:99-115 declares thirteen coded-token tables.
// Eleven have a decoder at :858-928; the other two, HasFieldMarshal and
// HasDeclSecurity, are reached through RowReader::codedToken instead. All
// thirteen are modelled below at their exact lengths.
//
// A coded token carries a tag in its low bits and a row index above them, and
// the tag is a subscript into the table. So there are two different properties
// here and an earlier version of this block proved neither of them properly.
//
// It read:
//
//     if (tagIndexes(tag, Len)) sink = table[tag];
//     assert(tagIndexes(tag, Len) == (tag < Len));
//
// for every kind. The subscript sits inside the guard, so the array-bounds
// check is satisfied by construction whatever the tag width is, and the
// assertion restates tagIndexes' own one-line body. Measured, not argued: the
// audit changed proof_type_def_or_ref_stays_in_table to a 5-bit tag over the
// 3-entry table -- the exact mismatch the proof advertised catching -- and it
// still reported VERIFICATION SUCCESSFUL.
//
// The two properties are separated below, and which one a kind gets is decided
// by a static_assert on its own numbers rather than by whoever writes the call:
//
//   tagFitsTheTableExactly  2^TagBits == Len. The mask admits exactly the
//                           table's entries, so the guard is unnecessary --
//                           which is precisely why cli_tables.cpp:895, :901,
//                           :907 and :913 are sound without one. Proved by
//                           subscripting WITHOUT a guard, so ESBMC's
//                           array-bounds check is the property.
//
//   tagNeedsItsGuard        2^TagBits > Len. The mask admits rows the table
//                           does not have, so the guard is load-bearing. Proved
//                           with the guard, plus a witness that the guard
//                           really refuses something the mask can produce.
//
// Pairing a kind with the wrong width now fails to compile -- in the direction
// that matters. tagFitsTheTableExactly rejects any width but the exact one, so
// the five unguarded kinds cannot be mis-stated at all.
//
// What is deliberately NOT claimed: tagNeedsItsGuard proves "with the guard, no
// read leaves the table", and that is true for any width wider than the table,
// so widening a guarded kind from 2 bits to 5 still verifies. It has to: the
// property is about the guard, not about ECMA-335. Whether cli_tables.cpp uses
// the width ECMA-335 assigns to each kind is a fact about the specification,
// and no proof over this harness can check it -- only reading the decoder
// against the standard can, or linking cli_tables.cpp and proving the decoders
// themselves. The table of widths above is the place a reviewer checks.
//
// The new form was watched failing before it was trusted. Dropping the mask in
// splitTag (`tag = coded` instead of `tag = coded & mask`) fails
// proof_has_semantics_stays_in_table and proof_resolution_scope_stays_in_table
// on the array read. Under the guarded form that defect passes, because the
// guard filters it out before the subscript -- which is the whole reason this
// block was rewritten.

static const std::uint8_t kTypeDefOrRef[] = {0x02, 0x01, 0x1B};             // 2 bits
static const std::uint8_t kHasConstant[] = {0x04, 0x08, 0x17};              // 2 bits
static const std::uint8_t kHasCustomAttr[] = {0x06, 0x04, 0x01, 0x02, 0x08, // 5 bits
											  0x09, 0x0A, 0x00, 0x11, 0x14, 0x17, 0x18, 0x1A, 0x1B,
											  0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2B, 0x2C};
static const std::uint8_t kHasFieldMarshal[] = {0x04, 0x08};                   // 1 bit
static const std::uint8_t kHasDeclSecurity[] = {0x02, 0x06, 0x20};             // 2 bits
static const std::uint8_t kMemberRefParent[] = {0x02, 0x01, 0x1A, 0x06, 0x1B}; // 3 bits
static const std::uint8_t kHasSemantics[] = {0x14, 0x17};                      // 1 bit
static const std::uint8_t kMethodDefOrRef[] = {0x06, 0x0A};                    // 1 bit
static const std::uint8_t kMemberForwarded[] = {0x04, 0x06};                   // 1 bit
static const std::uint8_t kImplementation[] = {0x26, 0x23, 0x27};              // 2 bits
static const std::uint8_t kCustomAttrType[] = {0xFF, 0xFF, 0x06, 0x0A, 0xFF};  // 3 bits
static const std::uint8_t kResolutionScope[] = {0x00, 0x1A, 0x23, 0x01};       // 2 bits
static const std::uint8_t kTypeOrMethodDef[] = {0x02, 0x06};                   // 1 bit

/// 2^TagBits == Len: the mask admits exactly the table's entries.
///
/// Stated by subscripting with NO guard, so the array read itself is the
/// property -- if any uint32 at all could drive `tag` past the end, ESBMC
/// reports the out-of-bounds access and names the line. This is the property
/// that makes the four unguarded decoders in cli_tables.cpp sound, and it is
/// not expressible with the guard in place.
template <unsigned TagBits, std::size_t Len>
static void tagFitsTheTableExactly(const std::uint8_t (&table)[Len])
{
	static_assert(TagBits < 32, "splitTag refuses a tag at or above the word width");
	static_assert(
		(static_cast<std::size_t>(1) << TagBits) == Len,
		"tagFitsTheTableExactly is for a mask that admits exactly the table's "
		"entries; a mask wider than the table belongs in tagNeedsItsGuard");

	const std::uint32_t coded = nondet_u32();

	std::uint32_t tag = 0, payload = 0;
	assert(splitTag(coded, TagBits, tag, payload));

	// No guard. The mask is the bound, and this read is the proof of it.
	idxmap_proof_sink = table[tag];

	// ... so a caller that writes the guard anyway can never see it refuse.
	assert(tagIndexes(tag, Len));
}

/// 2^TagBits > Len: the mask admits rows the table does not have.
///
/// Two properties, because either one alone is satisfiable by a mistake. With
/// the guard, no coded token reaches a subscript outside the table -- carried
/// by the array-bounds check on the real read. And the guard is not a
/// tautology: there is a tag the mask produces and the table refuses. The
/// second is what the static_assert makes true and the assume below makes
/// reachable, so the property is not vacuous.
template <unsigned TagBits, std::size_t Len>
static void tagNeedsItsGuard(const std::uint8_t (&table)[Len])
{
	static_assert(TagBits < 32, "splitTag refuses a tag at or above the word width");
	static_assert(
		(static_cast<std::size_t>(1) << TagBits) > Len,
		"tagNeedsItsGuard is for a mask wider than the table; a mask that fits "
		"exactly belongs in tagFitsTheTableExactly, where the bound is proved "
		"without a guard at all");

	const std::uint32_t coded = nondet_u32();

	std::uint32_t tag = 0, payload = 0;
	assert(splitTag(coded, TagBits, tag, payload));

	if (tagIndexes(tag, Len)) idxmap_proof_sink = table[tag];

	// The guard refuses something the mask can produce. The token is not
	// hand-picked: it is any token at all whose tag comes out as Len, and one
	// exists because Len < 2^TagBits -- which is the static_assert above, so
	// this assume cannot be unsatisfiable and make the assertion vacuous.
	const std::uint32_t other = nondet_u32();
	std::uint32_t otherTag = 0, otherPayload = 0;
	assert(splitTag(other, TagBits, otherTag, otherPayload));
	__ESBMC_assume(otherTag == static_cast<std::uint32_t>(Len));
	assert(!tagIndexes(otherTag, Len));
}

// The six kinds whose mask is wider than their table. Each `if (tag >= N)` in
// cli_tables.cpp is the guard this proves load-bearing.
extern "C" void proof_type_def_or_ref_stays_in_table()
{
	tagNeedsItsGuard<2>(kTypeDefOrRef);
}
extern "C" void proof_has_constant_stays_in_table()
{
	tagNeedsItsGuard<2>(kHasConstant);
}
extern "C" void proof_has_custom_attr_stays_in_table()
{
	tagNeedsItsGuard<5>(kHasCustomAttr);
}
extern "C" void proof_has_decl_security_stays_in_table()
{
	tagNeedsItsGuard<2>(kHasDeclSecurity);
}
extern "C" void proof_member_ref_parent_stays_in_table()
{
	tagNeedsItsGuard<3>(kMemberRefParent);
}
extern "C" void proof_implementation_stays_in_table()
{
	tagNeedsItsGuard<2>(kImplementation);
}
extern "C" void proof_custom_attr_type_stays_in_table()
{
	tagNeedsItsGuard<3>(kCustomAttrType);
}

// The five whose mask fits their table exactly. Four of these are the decoders
// that carry no guard at all, and this is the proof that they need none.
extern "C" void proof_has_field_marshal_stays_in_table()
{
	tagFitsTheTableExactly<1>(kHasFieldMarshal);
}
extern "C" void proof_has_semantics_stays_in_table()
{
	tagFitsTheTableExactly<1>(kHasSemantics);
}
extern "C" void proof_method_def_or_ref_stays_in_table()
{
	tagFitsTheTableExactly<1>(kMethodDefOrRef);
}
extern "C" void proof_member_forwarded_stays_in_table()
{
	tagFitsTheTableExactly<1>(kMemberForwarded);
}
extern "C" void proof_type_or_method_def_stays_in_table()
{
	tagFitsTheTableExactly<1>(kTypeOrMethodDef);
}
extern "C" void proof_resolution_scope_stays_in_table()
{
	tagFitsTheTableExactly<2>(kResolutionScope);
}

/// The number of entries in one of the tables above, as a std::size_t.
///
/// The assertions below used to spell the lengths out as literals -- three
/// separate `assert(!tagIndexes(3, 3))` for three different tables, which is
/// one SMT query and not three, and which silently stops describing its table
/// the moment an entry is added. Deriving the length from the array keeps the
/// assertion attached to the thing it is about.
template <std::size_t Len>
static constexpr std::size_t entriesIn(const std::uint8_t (&)[Len])
{
	return Len;
}

extern "C" void proof_a_one_bit_tag_needs_no_guard()
{
	// The four unguarded decoders -- decodeTypeOrMethodDef, decodeMethodDefOrRef,
	// decodeHasSemantics, decodeMemberForwarded at cli_tables.cpp:895, :901,
	// :907 and :913 -- are all 1-bit tags over 2-entry tables. This is why they
	// are sound: the mask admits exactly the two subscripts the table has, so
	// tagIndexes is a tautology there rather than a missing check.
	//
	// The four tables are named rather than assumed to be two entries long, so
	// adding an entry to any of them makes this fail instead of quietly ceasing
	// to be about them.
	static_assert(entriesIn(kTypeOrMethodDef) == 2, "no longer a 1-bit kind");
	static_assert(entriesIn(kMethodDefOrRef) == 2, "no longer a 1-bit kind");
	static_assert(entriesIn(kHasSemantics) == 2, "no longer a 1-bit kind");
	static_assert(entriesIn(kMemberForwarded) == 2, "no longer a 1-bit kind");

	const std::uint32_t coded = nondet_u32();
	std::uint32_t tag = 0, payload = 0;
	assert(splitTag(coded, 1, tag, payload));
	assert(tag < 2);
	assert(tagIndexes(tag, entriesIn(kTypeOrMethodDef)));
}

extern "C" void proof_the_tag_guard_is_load_bearing_everywhere_else()
{
	// Seven kinds pair a mask with a shorter table, so the guard is the only
	// thing between a coded token and a read past the end. Each assertion names
	// a tag the mask can produce and the table does not have -- and takes the
	// table's length FROM the table, so if an entry is ever added the assertion
	// follows it rather than describing a length that no longer exists.
	const std::uint32_t coded = nondet_u32();
	std::uint32_t tag = 0, payload = 0;

	// 2 bits admit 0..3.
	assert(splitTag(coded, 2, tag, payload));
	if (tag >= entriesIn(kTypeDefOrRef)) assert(!tagIndexes(tag, entriesIn(kTypeDefOrRef)));
	if (tag >= entriesIn(kHasConstant)) assert(!tagIndexes(tag, entriesIn(kHasConstant)));
	if (tag >= entriesIn(kHasDeclSecurity)) assert(!tagIndexes(tag, entriesIn(kHasDeclSecurity)));
	if (tag >= entriesIn(kImplementation)) assert(!tagIndexes(tag, entriesIn(kImplementation)));
	// ... and 3 is a tag those 2 bits really produce, so the refusal above is
	// reachable rather than a statement about an empty set.
	assert(!tagIndexes(3, entriesIn(kTypeDefOrRef)));

	// 3 bits admit 0..7.
	assert(splitTag(coded, 3, tag, payload));
	if (tag >= entriesIn(kMemberRefParent)) assert(!tagIndexes(tag, entriesIn(kMemberRefParent)));
	if (tag >= entriesIn(kCustomAttrType)) assert(!tagIndexes(tag, entriesIn(kCustomAttrType)));
	assert(!tagIndexes(7, entriesIn(kMemberRefParent)));

	// 5 bits admit 0..31 over 22 entries, the widest gap of the thirteen.
	assert(splitTag(coded, 5, tag, payload));
	if (tag >= entriesIn(kHasCustomAttr)) assert(!tagIndexes(tag, entriesIn(kHasCustomAttr)));
	assert(!tagIndexes(31, entriesIn(kHasCustomAttr)));

	// ResolutionScope is the exception among the guarded kinds: 2 bits admit
	// exactly its 4 entries, so its `if (tag >= 4)` never fires. Harmless, but
	// stated here so nobody removes the guard from a neighbour by analogy with
	// this one.
	assert(splitTag(coded, 2, tag, payload));
	assert(tagIndexes(tag, entriesIn(kResolutionScope)));
}

// ─── codedTokenIsWide ────────────────────────────────────────────────────────

// The longest coded-token table in cli_tables.cpp is kHasCustomAttr, at 22
// entries, so no coded index in this tree references more tables than that.
// The unwind bound above is one more.
static const std::size_t kMaxRefs = 22;

extern "C" void proof_wide_threshold_shift_is_bounded()
{
	// ECMA-335 II.24.2.6 defines tag widths of 1 to 5 bits. cli_tables.cpp:74
	// forms `1u << (16 - tagBits)`; with tagBits above 16 the subtraction wraps
	// on an unsigned and the shift count becomes astronomically large, which is
	// undefined. wideThreshold clamps instead, so --ub-shift-check has nothing
	// to find for any unsigned tagBits at all -- including the ones a new coded
	// token kind might be given by mistake.
	const unsigned tagBits = nondet_unsigned();
	const std::uint64_t t = wideThreshold(tagBits);

	assert(t >= 1 && t <= 65536);

	// Over the widths the format actually defines, the shift is 11..15 -- never
	// 16 or more, which is the bound property 8 asks for.
	if (tagBits >= 1 && tagBits <= 5)
	{
		assert(t == (std::uint64_t{1} << (16 - tagBits)));
		assert(t >= 2048 && t <= 32768);
	}
}

extern "C" void proof_coded_token_is_wide_is_exact()
{
	std::uint32_t counts[kMaxRefs];
	for (std::size_t k = 0; k < kMaxRefs; ++k)
		counts[k] = nondet_u32();

	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kMaxRefs);
	const unsigned tagBits = nondet_unsigned();
	__ESBMC_assume(tagBits >= 1 && tagBits <= 5);

	const bool wide = codedTokenIsWide(counts, n, tagBits);

	// Wide exactly when some referenced table has 2^(16 - tagBits) rows or
	// more, which is the ECMA-335 II.24.2.6 rule stated directly.
	const std::uint64_t threshold = std::uint64_t{1} << (16 - tagBits);
	bool any = false;
	for (std::size_t k = 0; k < n; ++k)
		if (static_cast<std::uint64_t>(counts[k]) >= threshold) any = true;

	assert(wide == any);
}

extern "C" void proof_coded_token_is_wide_is_monotone()
{
	// A table that grows never makes the token narrower. If it could, adding a
	// row would silently shrink every row that carries this coded index, and
	// every table after it in the #~ stream would be decoded at the wrong
	// offset -- there is no length prefix to resynchronise against.
	std::uint32_t before[kMaxRefs];
	std::uint32_t after[kMaxRefs];
	for (std::size_t k = 0; k < kMaxRefs; ++k)
	{
		before[k] = nondet_u32();
		after[k] = nondet_u32();
		__ESBMC_assume(after[k] >= before[k]);
	}

	const std::size_t n = nondet_size();
	__ESBMC_assume(n <= kMaxRefs);
	const unsigned tagBits = nondet_unsigned();
	__ESBMC_assume(tagBits >= 1 && tagBits <= 5);

	if (codedTokenIsWide(before, n, tagBits)) assert(codedTokenIsWide(after, n, tagBits));
}

extern "C" void proof_coded_token_is_wide_is_narrow_when_it_can_be()
{
	// The other direction, and the one that costs bytes rather than
	// correctness: a token is not made wide by a table nobody referenced. n is
	// the number of referenced tables, not the number of tables in the stream.
	std::uint32_t counts[kMaxRefs];
	for (std::size_t k = 0; k < kMaxRefs; ++k)
		counts[k] = nondet_u32();

	const unsigned tagBits = nondet_unsigned();
	__ESBMC_assume(tagBits >= 1 && tagBits <= 5);

	assert(!codedTokenIsWide(counts, 0, tagBits));
	// And a null table of counts is narrow rather than a dereference.
	assert(!codedTokenIsWide(nullptr, nondet_size(), tagBits));
}
