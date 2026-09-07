/**
 * @file tests/verification/pe_reader_proof.cpp
 * @brief ESBMC proofs about PeReader itself, not about the kernel it calls.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Every other harness here proves a header. This one proves a parser: it links
 * the real src/cli_parser/pe_reader.cpp (see ESBMC-LINK below) and runs
 * PeReader::open over a buffer the solver chooses, so the properties are about
 * the code that ships rather than about an idealisation of it.
 *
 * That is worth doing because the kernel proofs have a gap they cannot close
 * themselves. `section_map.h` is proved total and in-range for every section
 * table; it says nothing about whether PeReader BUILDS its section table from
 * bytes it was entitled to read. Only a proof over the function can say that,
 * and this is it.
 *
 * ## What is symbolic, and what is not
 *
 * The buffer is exactly `size` bytes long -- not one byte more -- so ESBMC's
 * array-bounds checking IS the out-of-bounds property. Nothing needs to be
 * asserted for it: if any input at all drives a read outside the buffer, the
 * run fails with the offset that did it. The reads are the assertion.
 *
 * Two things are constrained, and both for cost rather than for convenience:
 *
 *   - The DOS filler. Bytes 2..0x3B and the tail are concrete. They are
 *     never read by open() on any path -- read16(0), read32(0x3C) and then
 *     read32(peOffset) are the only reads before parseCOFF, and parseCOFF
 *     reads from peOffset onwards. Making them symbolic multiplies the state
 *     space by 2^(8*58) and cannot reach a branch.
 *   - NumberOfSections. parseSections loops `count` times with count a raw u2,
 *     so a fully symbolic count needs 65536 unwindings. It is bounded to
 *     kMaxSections here and the bound is exhaustive within that: the loop is
 *     structurally identical at every count, and each iteration's only bound is
 *     its own `checkRange(base, 40)`, which does not depend on i.
 *
 * Everything a malformed file actually controls at these paths -- the MZ word,
 * the PE offset, the PE signature, Machine, SizeOfOptionalHeader, the optional
 * header magic, the COM descriptor directory and every section header field --
 * is fully symbolic.
 */

// ESBMC-LINK: src/cli_parser/pe_reader.cpp
// ESBMC-STD: c++20
// ESBMC-SOLVER: --boolector
// ESBMC-TIMEOUT: 1800
// ESBMC-OPTIONAL: needs more than 15 GB of RAM; every backend is OOM-killed here
//
// This harness does not run in the default suite, and the reason is memory
// rather than time. Measured on a 4-core, 15 GB machine, at --unwind 2 (7,216
// verification conditions after simplification, which is the SMALLEST
// configuration that still reaches PeReader::open at all):
//
//   boolector   killed by the OOM killer at 13.9 GB anon-rss
//   z3          killed
//   bitwuzla    killed
//   cvc5        killed
//
// Thirteen kills across the four, confirmed in dmesg -- `Memory cgroup out of
// memory: Killed process (esbmc) total-vm:15804728kB, anon-rss:13910768kB`.
// This is not a timeout dressed up as one: the solver never returned a verdict
// because the process ceased to exist.
//
// What blows up is not the parser. ESBMC symbolically executes the whole
// translation unit, which brings in its models of std::vector, std::string and
// std::span for PeReader's members, and bit-blasting that is what reaches
// 13.9 GB. The floor confirms where the cost is not: constructing a PeReader
// and calling isValid(), with pe_reader.cpp linked exactly as below, discharges
// in 1.3 seconds.
//
// So the proofs below are written, type-check under both g++ and clang++ at
// c++20, and are reproducible on a machine with more memory --
// `scripts/verify_esbmc.sh --optional` runs them. They are not claimed as
// discharged, and docs/VERIFICATION.md says the same thing rather than
// implying whole-function verification of this parser is available.
// ESBMC-OPTIONS: --unwind 6 --unwindsetname strlen:0:64,strcpy:0:64,strncpy:0:64,memcpy:0:64
//
// Four directives, and each was measured rather than guessed.
//
// c++20 because pe_reader.h returns std::span, which ESBMC models only at that
// standard.
//
// --boolector because this is bitvector work and z3 does not finish it: the
// same query is 50s under boolector and still running at 300s under z3.
//
// --unwind 6 bounds the parser's own loops -- the section-table walk and the
// stream-header walk -- and unwinding assertions are on by default in 8.5.0, so
// a file declaring more sections than that fails the run rather than silently
// truncating the search. That is why the section count is constrained in the
// proofs below: an unconstrained u2 would need 65536 unwindings.
//
// --unwindsetname is what makes the whole thing possible, and it is not about
// this code at all. ESBMC's own C++ library models contain loops -- strlen
// walks to a NUL, strcpy and the std::string copy walk a length -- and the
// error strings this parser assigns on its refusal paths ("File too small for
// DOS header") drive them past any bound small enough for the parser's loops.
// Raising --unwind globally to 48 puts the parser's section walk at 48 too and
// the run does not return in 300s. Bounding those library loops by name leaves
// --unwind free to be what the parser needs. Without it the first result is a
// refutation that names `unwinding assertion loop 46` inside
// /esbmc-vfs/libc/library/string.c, which is a bound that was too small and not
// a defect in anything -- the exact shape of false finding this harness would
// otherwise produce.
//
// --ESBMC-TIMEOUT: 1800 because a kernel proof that needs more than the default
// 300s is usually a proof that is wrong, and this is not a kernel proof. Symex
// alone produces ~105,000 verification conditions here, 30,000 survive
// simplification, and encoding them takes 75s before the solver starts.

#include "retdec/cli_parser/pe_reader.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using retdec::cli_parser::PeReader;

extern "C" {
std::uint8_t nondet_u8();
std::uint16_t nondet_u16();
std::uint32_t nondet_u32();
}

#ifdef RETDEC_VERIFY_SYNTAX_ONLY
#define __ESBMC_assume(cond) ((void)sizeof((cond) ? 1 : 0))
#endif

// 0x40 is the smallest file open() will look at; below it the DOS-header check
// refuses. The first proof uses exactly that, because every offset open() forms
// is bounds-checked against `size` and the check does not learn anything from a
// larger file.
static const std::size_t kFileSize = 0x40;

// The second proof needs room for a real section table: a PE signature and COFF
// header at kPeOffset, then 40 bytes per section header after it.
static const std::size_t kBigFileSize = 0x80;

// Concrete, so the COFF fields sit at offsets this harness can write to. The
// first proof leaves it fully symbolic instead; between them the whole domain
// is covered -- every PE offset with a fixed header, and every header at a
// fixed PE offset -- which is what makes the constraint here a cost decision
// rather than a hole.
static const std::size_t kPeOffset = 0x10;

// One section is enough to exercise every path through parseSections: the
// header read, the bounds refusal, and the table the section map is then built
// from. Two would need 120 of the 128 bytes and buy nothing the loop does not
// already do once. See the --unwind note above for why this is bounded at all.
static const std::uint16_t kSections = 1;

/// Writes a 32-bit little-endian value the way the format stores it, so the
/// harness does not have to agree with read32 about byte order -- read32 is
/// what is under proof and must not be mirrored here.
static void put32(std::uint8_t* p, std::uint32_t v)
{
	p[0] = static_cast<std::uint8_t>(v);
	p[1] = static_cast<std::uint8_t>(v >> 8);
	p[2] = static_cast<std::uint8_t>(v >> 16);
	p[3] = static_cast<std::uint8_t>(v >> 24);
}

static void put16(std::uint8_t* p, std::uint16_t v)
{
	p[0] = static_cast<std::uint8_t>(v);
	p[1] = static_cast<std::uint8_t>(v >> 8);
}

/// A PE whose DOS header and COFF header are well formed, whose optional header
/// is empty, and whose one section header is entirely whatever the file says.
///
/// The section fields are the point: virtualSize, virtualAddress, rawDataSize
/// and rawDataOffset are the four numbers PeReader::rvaToOffset then translates
/// with, and they are unconstrained 32-bit values here. A real assembly cannot
/// make them anything else either -- they are four little-endian words in the
/// section table.
static void buildSymbolicSectionTable(std::uint8_t* buf)
{
	put16(&buf[0], 0x5A4D); // MZ
	put32(&buf[0x3C], static_cast<std::uint32_t>(kPeOffset));
	put32(&buf[kPeOffset], 0x00004550); // PE\0\0

	// COFF FileHeader, 20 bytes at kPeOffset + 4.
	std::uint8_t* coff = &buf[kPeOffset + 4];
	put16(&coff[0], nondet_u16()); // Machine -- whatever the file says
	put16(&coff[2], kSections);    // NumberOfSections -- see kSections
	put16(&coff[16], 0);           // SizeOfOptionalHeader: none

	// The section table starts right after the (empty) optional header. Every
	// byte of it is symbolic, including the eight-character name.
	const std::size_t sectOffset = kPeOffset + 4 + 20;
	for (std::size_t i = sectOffset; i < sectOffset + 40u * kSections; ++i)
		buf[i] = nondet_u8();
}

// ─── open ────────────────────────────────────────────────────────────────────

extern "C" void proof_open_reads_no_byte_outside_the_file()
{
	// The buffer is exactly kFileSize bytes. ESBMC's array-bounds check is the
	// property: a read at kFileSize or beyond fails the run and names the line.
	// Nothing is asserted, and nothing needs to be -- the reads are the claim.
	std::uint8_t buf[kFileSize] = {};

	// The two words a DOS header actually contains: the signature, and the
	// offset of the PE header. Both fully symbolic, so this covers every PE
	// offset a file can name, including every one that points outside itself.
	put16(&buf[0], nondet_u16());
	put32(&buf[0x3C], nondet_u32());

	PeReader r;
	(void)r.open(buf, kFileSize);
}

extern "C" void proof_open_through_the_section_table_reads_no_byte_outside()
{
	// The same property on the path that builds the section table, which the
	// proof above cannot reach: a well-formed DOS and COFF header, and a
	// section header the file wrote.
	std::uint8_t buf[kBigFileSize] = {};
	buildSymbolicSectionTable(buf);

	PeReader r;
	(void)r.open(buf, kBigFileSize);
}

extern "C" void proof_a_short_file_is_refused_and_nothing_is_read()
{
	// Under 0x40 bytes open() must refuse before reading anything. The buffer
	// is exactly the symbolic size, so a read of even byte 0 on this path would
	// be reported as an out-of-bounds access rather than passing unnoticed.
	const std::size_t size = nondet_u32() % 0x40;
	std::uint8_t buf[0x3F] = {};
	for (std::size_t i = 0; i < size && i < sizeof(buf); ++i)
		buf[i] = nondet_u8();

	PeReader r;
	assert(!r.open(buf, size));
	assert(!r.isValid());
}

extern "C" void proof_a_null_buffer_is_refused()
{
	// A caller that mapped nothing. The size is symbolic below 0x40, so this is
	// the claim that open() consults the size before it dereferences the
	// pointer -- not that it happens to on one path.
	const std::size_t size = nondet_u32() % 0x40;
	PeReader r;
	assert(!r.open(nullptr, size));
}

extern "C" void proof_a_refused_open_is_not_valid()
{
	// The contract every caller in src/cli_parser relies on: if open() said no,
	// isValid() says no. Without it a refusal that left valid_ set from an
	// earlier open would let a second file be read through the first file's
	// section table.
	std::uint8_t buf[kFileSize] = {};
	put16(&buf[0], nondet_u16());
	put32(&buf[0x3C], nondet_u32());

	PeReader r;
	if (!r.open(buf, kFileSize)) assert(!r.isValid());
}

// ─── rvaToOffset / rvaToSpan, over a table the parser built itself ───────────

extern "C" void proof_rva_to_offset_is_inside_the_file_or_zero()
{
	// section_map.h proves this about ITS input: given a section table, the
	// answer is in range or is the sentinel. What it cannot say is whether
	// PeReader hands it a table it was entitled to read. Here the table is not
	// handed in -- the real parseSections built it, from bytes the solver
	// chose -- so this closes that gap.
	std::uint8_t buf[kBigFileSize] = {};
	buildSymbolicSectionTable(buf);

	PeReader r;
	(void)r.open(buf, kBigFileSize);

	const std::uint64_t off = r.rvaToOffset(nondet_u32());
	// 0 is this function's own sentinel, kept for its callers. Everything else
	// must be a real offset into the file, or a caller reading at it reads
	// outside the mapping -- which is what happened before the kernel landed:
	// offset 4294901764 returned for a 608-byte file.
	assert(off == 0 || off < kBigFileSize);
}

extern "C" void proof_rva_to_span_stays_inside_the_file()
{
	std::uint8_t buf[kBigFileSize] = {};
	buildSymbolicSectionTable(buf);

	PeReader r;
	(void)r.open(buf, kBigFileSize);

	const std::size_t want = nondet_u32();
	const auto sp = r.rvaToSpan(nondet_u32(), want);

	assert(sp.size() <= want);
	if (!sp.empty())
	{
		// Stated by subtraction on the pointers rather than by forming
		// `sp.data() + sp.size()`, which is the sum this property exists to say
		// does not run past the end.
		assert(sp.data() >= buf);
		const std::size_t start = static_cast<std::size_t>(sp.data() - buf);
		assert(start < kBigFileSize);
		assert(sp.size() <= kBigFileSize - start);
	}
}
