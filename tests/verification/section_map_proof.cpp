/**
 * @file tests/verification/section_map_proof.cpp
 * @brief ESBMC proofs for include/retdec/utils/section_map.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * The bug this kernel exists to stop was found by SMT, not by reading. Two of
 * the three address-to-offset translations in this tree tested containment as
 *
 *     rva >= s.va && rva < s.va + std::max(s.virtualSize, s.rawSize)
 *
 * in uint32. ESBMC returns the witness va = 2692743171, span = 2675966078,
 * rva = 3221225600: the true end is 5368709249, the section does contain the
 * address, the sum wrapped to 1073741953, and the test said no. The file picks
 * which section an address resolves into.
 *
 * proof_containment_never_wraps below is that counterexample turned into a
 * property, so the mistake cannot come back.
 *
 * The section table is symbolic in every proof: each field an unconstrained
 * 64-bit choice, so these hold for every table a file could declare, not for a
 * table someone thought of.
 */

// ESBMC-OPTIONS: --unwind 4
// ESBMC-SOLVER: --boolector
//
// Boolector, and the difference is not marginal: the four properties that walk
// the whole section table time out past 300s under z3 and discharge in 0-22s
// under boolector. Nothing in section_map.h divides, so the spurious
// unsigned-division overflow that pins bounds_proof.cpp to z3 cannot arise here
// -- and `scripts/verify_esbmc.sh --cross` runs both backends over this file
// anyway and reports any disagreement as a finding rather than a preference.

#include "retdec/utils/section_map.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace retdec::utils::secmap;

extern "C" {
std::uint64_t nondet_u64();
std::size_t nondet_size();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

// One less than the unwind bound, so the search over the table is exhaustive.
// Three sections is enough to exercise every path through the search -- a
// first-match win, a fall-through to a later section, and exhaustion -- while
// keeping the symbolic state (four unconstrained 64-bit fields each) small
// enough for the solver. Four timed out at 280s.
static const std::size_t kMaxSections = 3;

static Section nondetSection()
{
	Section s;
	s.address = nondet_u64();
	s.virtSize = nondet_u64();
	s.rawSize = nondet_u64();
	s.rawOffset = nondet_u64();
	return s;
}

// ─── contains ────────────────────────────────────────────────────────────────

extern "C" void proof_containment_never_wraps()
{
	const Section s = nondetSection();
	const std::uint64_t addr = nondet_u64();

	const std::uint64_t span = s.virtSize > s.rawSize ? s.virtSize : s.rawSize;

	// The definition, stated without forming the sum at all: an address is in
	// the section when it is at or after the start and less than a span away.
	const bool truth = (span != 0) && (addr >= s.address) && (addr - s.address < span);

	assert(contains(s, addr) == truth);
}

extern "C" void proof_containment_agrees_with_128_bit_arithmetic()
{
	// The same property from the other side: compare against the sum computed
	// where it genuinely cannot wrap. Restricting the operands to 32 bits is
	// what makes the 64-bit sum exact, and 32 bits is the width the formats
	// store -- so this is the real case, stated so that a wrapping
	// implementation is refuted rather than merely disagreed with.
	Section s;
	s.address = nondet_u64();
	__ESBMC_assume(s.address <= 0xFFFFFFFFu);
	s.virtSize = nondet_u64();
	__ESBMC_assume(s.virtSize <= 0xFFFFFFFFu);
	s.rawSize = nondet_u64();
	__ESBMC_assume(s.rawSize <= 0xFFFFFFFFu);
	s.rawOffset = 0;
	std::uint64_t addr = nondet_u64();
	__ESBMC_assume(addr <= 0xFFFFFFFFu);

	const std::uint64_t span = s.virtSize > s.rawSize ? s.virtSize : s.rawSize;
	const bool wide = (span != 0) && addr >= s.address && addr < s.address + span;

	assert(contains(s, addr) == wide);
}

extern "C" void proof_empty_section_contains_nothing()
{
	Section s = nondetSection();
	s.virtSize = 0;
	s.rawSize = 0;
	assert(!contains(s, nondet_u64()));
}

// ─── addressToOffset ─────────────────────────────────────────────────────────

extern "C" void proof_offset_is_readable_or_unmapped()
{
	Section secs[kMaxSections];
	for (std::size_t i = 0; i < kMaxSections; ++i)
		secs[i] = nondetSection();

	const std::size_t count = nondet_size();
	__ESBMC_assume(count <= kMaxSections);
	const std::size_t fileSize = nondet_size();
	const std::uint64_t addr = nondet_u64();

	const std::uint64_t off = addressToOffset(addr, secs, count, fileSize);

	// The whole contract: kUnmapped is the only out-of-range answer, so a
	// caller that tests against it may read at anything else. Two of the three
	// implementations this replaces returned a file-controlled sum here, which
	// is neither in range nor equal to the sentinel.
	assert(off == kUnmapped || off < fileSize);
}

extern "C" void proof_a_null_table_is_unmapped()
{
	// A caller that has no sections yet must not get an answer derived from
	// whatever the pointer happens to be.
	assert(addressToOffset(nondet_u64(), nullptr, nondet_size(), nondet_size()) == kUnmapped);
}

extern "C" void proof_empty_table_is_unmapped()
{
	Section secs[kMaxSections];
	for (std::size_t i = 0; i < kMaxSections; ++i)
		secs[i] = nondetSection();
	assert(addressToOffset(nondet_u64(), secs, 0, nondet_size()) == kUnmapped);
}

extern "C" void proof_offset_comes_from_a_containing_section()
{
	Section secs[kMaxSections];
	for (std::size_t i = 0; i < kMaxSections; ++i)
		secs[i] = nondetSection();
	const std::size_t count = nondet_size();
	__ESBMC_assume(count <= kMaxSections);
	const std::size_t fileSize = nondet_size();
	const std::uint64_t addr = nondet_u64();

	const std::uint64_t off = addressToOffset(addr, secs, count, fileSize);

	if (off != kUnmapped)
	{
		// A mapped answer means some section really contained the address and
		// really stored the byte. Without this the sentinel could be satisfied
		// by returning any in-range number at all.
		bool found = false;
		for (std::size_t i = 0; i < count; ++i)
		{
			if (!contains(secs[i], addr)) continue;
			const std::uint64_t delta = addr - secs[i].address;
			if (delta >= secs[i].rawSize) continue;
			// Stated by subtraction. Writing it as
			// `secs[i].rawOffset + delta == off` forms the very sum the kernel
			// refuses to form until it knows it fits, so the harness would
			// commit the fault it is proving absent -- and ESBMC said so,
			// reporting an overflow on add at this line rather than a failed
			// property.
			if (off >= secs[i].rawOffset && off - secs[i].rawOffset == delta) found = true;
		}
		assert(found);
	}
}

extern "C" void proof_virtual_tail_is_unmapped()
{
	// A .bss, or a PE virtual-size overhang: mapped in memory, absent from the
	// file. Handing back an offset here is how a reader ends up in whatever
	// section follows on disk.
	Section s = nondetSection();
	__ESBMC_assume(s.rawSize < s.virtSize);
	const std::uint64_t addr = nondet_u64();
	__ESBMC_assume(addr >= s.address);
	__ESBMC_assume(addr - s.address >= s.rawSize);
	__ESBMC_assume(addr - s.address < s.virtSize);

	assert(addressToOffset(addr, &s, 1, nondet_size()) == kUnmapped);
}

// ─── readableAt ──────────────────────────────────────────────────────────────

extern "C" void proof_readable_span_stays_inside_the_file()
{
	Section secs[kMaxSections];
	for (std::size_t i = 0; i < kMaxSections; ++i)
		secs[i] = nondetSection();
	const std::size_t count = nondet_size();
	__ESBMC_assume(count <= kMaxSections);
	const std::size_t fileSize = nondet_size();
	const std::uint64_t addr = nondet_u64();
	const std::size_t want = nondet_size();

	const std::size_t n = readableAt(addr, secs, count, fileSize, want);

	assert(n <= want);
	if (n > 0)
	{
		const std::uint64_t off = addressToOffset(addr, secs, count, fileSize);
		assert(off != kUnmapped);
		// off + n is the end of the readable span and must be inside the file.
		assert(n <= fileSize - static_cast<std::size_t>(off));
	}
}

extern "C" void proof_unmapped_address_reads_nothing()
{
	Section secs[kMaxSections];
	for (std::size_t i = 0; i < kMaxSections; ++i)
		secs[i] = nondetSection();
	const std::size_t count = nondet_size();
	__ESBMC_assume(count <= kMaxSections);
	const std::size_t fileSize = nondet_size();
	const std::uint64_t addr = nondet_u64();

	if (addressToOffset(addr, secs, count, fileSize) == kUnmapped)
		assert(readableAt(addr, secs, count, fileSize, nondet_size()) == 0);
}
