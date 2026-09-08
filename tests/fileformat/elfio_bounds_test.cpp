/**
 * @file tests/fileformat/elfio_bounds_test.cpp
 * @brief The declared section and segment counts of an ELF file, against what
 *        the file can actually hold.
 *
 * ELFIO is the ELF reader behind retdec::fileformat::ElfFormat, and it is
 * header-only and depends on nothing else -- so unlike the rest of that module,
 * which links the pinned LLVM, it can be driven from the fast gate.
 *
 * These are amplification tests. Everything downstream in ElfFormat walks the
 * section list: loadSections builds an ElfSection per entry, computes its
 * entropy and pushes it; computeSectionTableHashes, loadStrings, loadSymbols
 * and loadNotes each walk it again. So the number of section objects a small
 * file can conjure is the number that matters.
 */

#include "elfio/elfio.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// A 64-bit little-endian ELF header with the section and segment table fields
/// set as asked, and nothing after it.
std::string elfHeader(
	std::uint16_t shentsize,
	std::uint16_t shnum,
	std::uint64_t shoff,
	std::uint16_t phentsize = 0,
	std::uint16_t phnum = 0,
	std::uint64_t phoff = 0)
{
	std::vector<std::uint8_t> v(64, 0);
	const std::uint8_t ident[] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
	for (std::size_t i = 0; i < sizeof(ident); ++i)
		v[i] = ident[i];

	auto put16 = [&v](std::size_t off, std::uint16_t x) {
		v[off] = static_cast<std::uint8_t>(x & 0xFF);
		v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
	};
	auto put64 = [&v](std::size_t off, std::uint64_t x) {
		for (std::size_t i = 0; i < 8; ++i)
			v[off + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
	};

	put16(16, 2);  // e_type = ET_EXEC
	put16(18, 62); // e_machine = x86-64
	put64(32, phoff);
	put64(40, shoff);
	put16(52, 64); // e_ehsize
	put16(54, phentsize);
	put16(56, phnum);
	put16(58, shentsize);
	put16(60, shnum);
	put16(62, 0); // e_shstrndx

	return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

/// How many section and segment objects ELFIO builds from @p image.
struct Counts
{
	bool loaded = false;
	std::size_t sections = 0;
	std::size_t segments = 0;
};

Counts countOf(const std::string& image)
{
	std::istringstream in(image);
	ELFIO::elfio reader;
	Counts c;
	c.loaded = reader.load(in);
	c.sections = reader.sections.size();
	c.segments = reader.segments.size();
	return c;
}

} // namespace

// The defect. load_sections stopped at `offset + i * entry_size <
// real_file_length`, which with entry_size == 0 is `offset < real_file_length`
// -- constant, so the loop ran the full declared count. e_shentsize comes
// straight out of the header and nothing required it to be non-zero.
//
// Both of the units libFuzzer reported as its slowest, at 23 and 31 seconds,
// are this shape. Measured against the code as it stood:
//
//   104 bytes, e_shnum=24415, e_shentsize=0  ->  24,415 sections
//    71 bytes, e_shnum=28672, e_shentsize=0  ->  28,672 sections
//
// which is 235 and 404 section objects per byte of input. They are committed
// under tests/crash_corpus/fuzz_elf/.
TEST(ElfioBounds, AZeroSizedSectionEntryDoesNotYieldOneSectionPerDeclaredCount)
{
	const auto c = countOf(elfHeader(/*shentsize=*/0, /*shnum=*/24415, /*shoff=*/0));
	EXPECT_EQ(0u, c.sections) << "a 64-byte file produced " << c.sections << " sections";
}

TEST(ElfioBounds, AZeroSizedSegmentEntryDoesNotYieldOneSegmentPerDeclaredCount)
{
	const auto c = countOf(elfHeader(0, 0, 0, /*phentsize=*/0, /*phnum=*/24415, /*phoff=*/0));
	EXPECT_EQ(0u, c.segments) << "a 64-byte file produced " << c.segments << " segments";
}

// And the general form: a declared count larger than the file can hold is
// bounded by the file, whatever the entry size.
TEST(ElfioBounds, ADeclaredCountLargerThanTheFileIsBoundedByTheFile)
{
	// 64 bytes total, section headers at 64 (i.e. at the very end), 64 bytes
	// each: room for none.
	EXPECT_EQ(0u, countOf(elfHeader(64, 1000, 64)).sections);

	// Headers at offset 0 in a 64-byte file, 64 bytes each: room for exactly
	// one, however many are declared.
	EXPECT_LE(countOf(elfHeader(64, 1000, 0)).sections, 1u);
}
