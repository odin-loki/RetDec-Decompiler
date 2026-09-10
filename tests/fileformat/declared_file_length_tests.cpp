/**
 * @file tests/fileformat/declared_file_length_tests.cpp
 * @brief What keeps the two getDeclaredFileLength overrides from wrapping.
 *
 * FileFormat::getDeclaredFileLength was fixed to form its section and segment
 * extents with bounds_kernels::regionEndSaturating. The two overrides that
 * shadow it were not, and both still spell an unclamped sum of header fields:
 *
 *   ElfFormat  max(shoff + sectionTableSize, phoff + segmentTableSize)
 *   CoffFormat symbolTableOffset + numberOfSymbols * symbolTableEntrySize,
 *              and then declaredSize + sizeOfStringTable
 *
 * Both were recorded in docs/internal/UNFIXED_AUDIT_FINDINGS.md as reachable
 * wraps. Measured, they are not, and the reason is not in either function.
 *
 * ELFIO reports a section- or segment-table size of zero unless the table lies
 * inside the file, so a 64-bit e_shoff of 0xFFFFFFFFFFFFFFFF always arrives
 * with a size of 0 and the sum is the offset itself. LLVM's COFFObjectFile does
 * the same for the symbol table: an out-of-range PointerToSymbolTable comes
 * back with a symbol count of 0, which the `if` in that override already tests,
 * and getSizeOfStringTable refuses an offset past the end before reading.
 *
 * That makes the safety of both sums a property of code some distance away, so
 * these cases pin it. If either reader ever starts reporting an extent for a
 * table it could not read, the sum wraps and the declared length falls below
 * the offset it was formed from -- which is what the assertions here say cannot
 * happen. They are not a substitute for clamping the sums; they are what makes
 * the decision not to clamp them checkable.
 *
 * On a 32-bit host the COFF product would be a different question:
 * getNumberOfCoffSymbols returns std::size_t, so 0xFFFFFFFF * 18 overflows
 * there and not here. Nothing in this tree builds 32-bit, and this file asserts
 * nothing about it.
 */

#include "retdec/fileformat/file_format/coff/coff_format.h"
#include "retdec/fileformat/file_format/elf/elf_format.h"

#include "fileformat/fileformat_tests.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace retdec {
namespace fileformat {
namespace tests {
namespace {

constexpr std::uint64_t kU64Max = 0xFFFFFFFFFFFFFFFFull;
constexpr std::uint32_t kU32Max = 0xFFFFFFFFu;

/// A 64-bit little-endian ELF image with the table fields set as asked, padded
/// out so the file itself is a plausible length.
std::vector<std::uint8_t> elfImage(
	std::uint64_t shoff,
	std::uint16_t shnum,
	std::uint16_t shentsize,
	std::uint64_t phoff,
	std::uint16_t phnum,
	std::uint16_t phentsize)
{
	std::vector<std::uint8_t> v(64 + 512, 0);
	const std::uint8_t ident[] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
	for (std::size_t i = 0; i < sizeof(ident); ++i)
	{
		v[i] = ident[i];
	}

	auto put16 = [&v](std::size_t o, std::uint16_t x) {
		v[o] = static_cast<std::uint8_t>(x & 0xFF);
		v[o + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
	};
	auto put64 = [&v](std::size_t o, std::uint64_t x) {
		for (std::size_t i = 0; i < 8; ++i)
		{
			v[o + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
		}
	};

	put16(16, 2);  // e_type = ET_EXEC
	put16(18, 62); // e_machine = EM_X86_64
	put64(32, phoff);
	put64(40, shoff);
	put16(52, 64); // e_ehsize
	put16(54, phentsize);
	put16(56, phnum);
	put16(58, shentsize);
	put16(60, shnum);

	return v;
}

/// The benign case, so the assertions below are known to be reachable at all
/// rather than passing because nothing parses.
TEST(DeclaredFileLengthTests, AnElfWithTablesInsideTheFileReportsThem)
{
	const auto img = elfImage(64, 2, 64, 0, 0, 0);
	ElfFormat f(img.data(), img.size());

	EXPECT_EQ(64u, f.getSectionTableOffset());
	EXPECT_EQ(128u, f.getSectionTableSize());
	EXPECT_EQ(192u, f.getDeclaredFileLength());
}

/// e_shoff at the top of the address space. ELFIO cannot read a table there, so
/// it reports no size, and `shoff + 0` does not wrap.
TEST(DeclaredFileLengthTests, AnElfSectionTableOffsetAtTheTopDoesNotWrap)
{
	const std::uint64_t offsets[] = {kU64Max, kU64Max - 63, 0xFFFFFFFF00000000ull};
	for (const std::uint64_t shoff: offsets)
	{
		const auto img = elfImage(shoff, 0xFFFF, 64, 0, 0, 0);
		ElfFormat f(img.data(), img.size());

		EXPECT_EQ(shoff, f.getSectionTableOffset());
		EXPECT_EQ(0u, f.getSectionTableSize()) << "ELFIO reported an extent for a table it could not read; the sum "
												  "in ElfFormat::getDeclaredFileLength now wraps";
		EXPECT_GE(f.getDeclaredFileLength(), f.getSectionTableOffset());
	}
}

/// The same for the segment table, which is the other half of the same max().
TEST(DeclaredFileLengthTests, AnElfSegmentTableOffsetAtTheTopDoesNotWrap)
{
	const auto img = elfImage(64, 2, 64, kU64Max, 0xFFFF, 56);
	ElfFormat f(img.data(), img.size());

	EXPECT_EQ(kU64Max, f.getSegmentTableOffset());
	EXPECT_EQ(0u, f.getSegmentTableSize()) << "ELFIO reported an extent for a segment table it could not read";
	EXPECT_GE(f.getDeclaredFileLength(), f.getSegmentTableOffset());
}

/// The benign COFF, again so the hostile cases are known to be doing something.
TEST(DeclaredFileLengthTests, ACoffWithASymbolTableInsideTheFileReportsIt)
{
	CoffFormat f(coffBytes.data(), coffBytes.size());

	EXPECT_EQ(20u, f.getNumberOfCoffSymbols());
	EXPECT_EQ(504u, f.getCoffSymbolTableOffset());
	EXPECT_LE(f.getCoffSymbolTableOffset(), f.getLoadedFileLength());
	EXPECT_GE(f.getDeclaredFileLength(), f.getCoffSymbolTableOffset());
}

/// PointerToSymbolTable and NumberOfSymbols are 32-bit header fields with
/// nothing above them. Driven to their maxima, LLVM's reader declines the table
/// and the override's `if` never forms the product.
TEST(DeclaredFileLengthTests, ACoffSymbolTableOutsideTheFileIsNotCounted)
{
	struct
	{
		std::uint32_t symOff, nSyms;
	} cases[] = {
		{0x1f8, kU32Max},
		{kU32Max, 0x14},
		{kU32Max, kU32Max},
		{kU32Max - 1, kU32Max},
	};

	for (const auto& c: cases)
	{
		auto v = coffBytes;
		auto put32 = [&v](std::size_t o, std::uint32_t x) {
			for (std::size_t i = 0; i < 4; ++i)
			{
				v[o + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
			}
		};
		put32(8, c.symOff); // PointerToSymbolTable
		put32(12, c.nSyms); // NumberOfSymbols

		CoffFormat f(v.data(), v.size());

		EXPECT_EQ(0u, f.getNumberOfCoffSymbols()) << "LLVM counted symbols for a table it could not read; the product "
													 "in CoffFormat::getDeclaredFileLength is now formed on "
												  << c.nSyms << " symbols";
		EXPECT_EQ(0u, f.getSizeOfStringTable()) << "a string table was read from past the end of the file";
		EXPECT_LE(f.getDeclaredFileLength(), f.getLoadedFileLength());
	}
}

} // namespace
} // namespace tests
} // namespace fileformat
} // namespace retdec
