/**
* @file tests/fileformat/coff_format_tests.cpp
* @brief Tests for the @c coff_format module.
* @copyright (c) 2019 Odin Loch Trading as Imortek
*/

#include <gtest/gtest.h>

#include "retdec/fileformat/utils/format_detection.h"
#include "fileformat/fileformat_tests.h"

using namespace ::testing;

namespace retdec {
namespace fileformat {
namespace tests {

extern const std::vector<uint8_t> coffBytes;
extern const std::vector<uint8_t> elfBytes;
extern const std::vector<uint8_t> machoBytes;
extern const std::vector<uint8_t> peBytes;
extern const std::string ihexBytes;
extern const std::string rawBytes;

/**
 * Tests for the @c coff_format module - using istream constructor.
 */
class FileFormatDetectionTests : public Test
{

};

TEST_F(FileFormatDetectionTests, DetectCoff_istream)
{
	std::stringstream stream;
	stream << std::string(coffBytes.begin(), coffBytes.end());
	EXPECT_EQ(Format::COFF, detectFileFormat(stream));
}

TEST_F(FileFormatDetectionTests, DetectCoff_data)
{
	EXPECT_EQ(
			Format::COFF,
			detectFileFormat(coffBytes.data(), coffBytes.size()));
}

TEST_F(FileFormatDetectionTests, DetectElf_istream)
{
	std::stringstream stream;
	stream << std::string(elfBytes.begin(), elfBytes.end());
	EXPECT_EQ(Format::ELF, detectFileFormat(stream));
}

TEST_F(FileFormatDetectionTests, DetectElf_data)
{
	EXPECT_EQ(
			Format::ELF,
			detectFileFormat(elfBytes.data(), elfBytes.size()));
}

TEST_F(FileFormatDetectionTests, DetectMacho_istream)
{
	std::stringstream stream;
	stream << std::string(machoBytes.begin(), machoBytes.end());
	EXPECT_EQ(Format::MACHO, detectFileFormat(stream));
}

TEST_F(FileFormatDetectionTests, DetectMacho_data)
{
	EXPECT_EQ(
			Format::MACHO,
			detectFileFormat(machoBytes.data(), machoBytes.size()));
}

TEST_F(FileFormatDetectionTests, DetectPe_istream)
{
	std::stringstream stream;
	stream << std::string(peBytes.begin(), peBytes.end());
	EXPECT_EQ(Format::PE, detectFileFormat(stream));
}

TEST_F(FileFormatDetectionTests, DetectPe_data)
{
	EXPECT_EQ(
			Format::PE,
			detectFileFormat(peBytes.data(), peBytes.size()));
}

TEST_F(FileFormatDetectionTests, DetectIhex_istream)
{
	std::stringstream stream;
	stream << std::string(ihexBytes.begin(), ihexBytes.end());
	EXPECT_EQ(Format::INTEL_HEX, detectFileFormat(stream));
}

TEST_F(FileFormatDetectionTests, DetectIhex_data)
{
	EXPECT_EQ(
			Format::INTEL_HEX,
			detectFileFormat(
					reinterpret_cast<const uint8_t*>(ihexBytes.data()),
					ihexBytes.size()));
}

TEST_F(FileFormatDetectionTests, DetectRaw_istream)
{
	std::stringstream stream;
	stream << std::string(rawBytes.begin(), rawBytes.end());
	EXPECT_EQ(Format::RAW_DATA, detectFileFormat(stream, true));
}

TEST_F(FileFormatDetectionTests, DetectRaw_data)
{
	EXPECT_EQ(
			Format::RAW_DATA,
			detectFileFormat(
					reinterpret_cast<const uint8_t*>(rawBytes.data()),
					rawBytes.size(),
					true));
}

// Stage 1 — CA FE BA BE polyglot: Java .class uses same magic as Mach-O fat.
// Second big-endian word as Mach-O = arch count (small); as class file = version pack (>30).
TEST_F(FileFormatDetectionTests, DetectJavaClassNotMachO_polyglot)
{
	const std::vector<std::uint8_t> javaHdr = {
		0xCA, 0xFE, 0xBA, 0xBE,
		0x00, 0x00, 0x00, 0x34 // BE u32 0x34 = 52 (Java class version) → >30
	};
	EXPECT_EQ(Format::UNKNOWN, detectFileFormat(javaHdr.data(), javaHdr.size()));
}

TEST_F(FileFormatDetectionTests, DetectFatMachoMagic_staysMacho)
{
	const std::vector<std::uint8_t> fatHdr = {
		0xCA, 0xFE, 0xBA, 0xBE,
		0x00, 0x00, 0x00, 0x02 // nfat_arch = 2
	};
	EXPECT_EQ(Format::MACHO, detectFileFormat(fatHdr.data(), fatHdr.size()));
}

// 0xCAFEBABE without second word — cannot distinguish Java vs Mach-O fat.
TEST_F(FileFormatDetectionTests, DetectCafeMagicTooShort_unknown)
{
	const std::vector<std::uint8_t> cafeOnly = {
		0xCA, 0xFE, 0xBA, 0xBE,
		0x00,
		0x00
	};
	EXPECT_EQ(Format::UNKNOWN, detectFileFormat(cafeOnly.data(), cafeOnly.size()));
}

TEST_F(FileFormatDetectionTests, DetectUnixArArchive_unknown)
{
	const std::vector<std::uint8_t> arHdr = {
		'!', '<', 'a', 'r', 'c', 'h', '>', '\n'
	};
	EXPECT_EQ(Format::UNKNOWN, detectFileFormat(arHdr.data(), arHdr.size()));
}

TEST_F(FileFormatDetectionTests, DetectThinArArchive_unknown)
{
	const std::vector<std::uint8_t> thinHdr = {
		'!', '<', 't', 'h', 'i', 'n', '>', '\n'
	};
	EXPECT_EQ(Format::UNKNOWN, detectFileFormat(thinHdr.data(), thinHdr.size()));
}

TEST_F(FileFormatDetectionTests, LatticeHints_elfPeSamples)
{
	auto hElf = computeFormatLatticeHints(elfBytes.data(), elfBytes.size());
	EXPECT_EQ(100u, hElf.elfStrength);
	EXPECT_EQ(0u, hElf.peStrength);

	auto hPe = computeFormatLatticeHints(peBytes.data(), peBytes.size());
	EXPECT_EQ(100u, hPe.peStrength);
	EXPECT_EQ(0u, hPe.elfStrength);
}

TEST_F(FileFormatDetectionTests, LatticeHints_mzOnlyWeakPe)
{
	const std::vector<std::uint8_t> mz = {'M', 'Z'};
	auto h = computeFormatLatticeHints(mz.data(), mz.size());
	EXPECT_EQ(25u, h.peStrength);
	EXPECT_EQ(0u, h.elfStrength);
}

TEST_F(FileFormatDetectionTests, LatticeHints_cafeJavaVsFatMacho)
{
	const std::vector<std::uint8_t> javaHdr = {
		0xCA, 0xFE, 0xBA, 0xBE,
		0x00, 0x00, 0x00, 0x34
	};
	auto j = computeFormatLatticeHints(javaHdr.data(), javaHdr.size());
	EXPECT_EQ(52u, j.cafeBabeSecondWord);
	EXPECT_EQ(100u, j.javaClassLatticeStrength);
	EXPECT_EQ(0u, j.machoFatLatticeStrength);

	const std::vector<std::uint8_t> fatHdr = {
		0xCA, 0xFE, 0xBA, 0xBE,
		0x00, 0x00, 0x00, 0x02
	};
	auto f = computeFormatLatticeHints(fatHdr.data(), fatHdr.size());
	EXPECT_EQ(2u, f.cafeBabeSecondWord);
	EXPECT_EQ(0u, f.javaClassLatticeStrength);
	EXPECT_EQ(100u, f.machoFatLatticeStrength);
}

TEST_F(FileFormatDetectionTests, LatticeHints_ihexCoffMachoSliceAr)
{
	auto hi = computeFormatLatticeHints(
			reinterpret_cast<const std::uint8_t *>(ihexBytes.data()),
			ihexBytes.size());
	EXPECT_EQ(100u, hi.ihexStrength);

	auto hc = computeFormatLatticeHints(coffBytes.data(), coffBytes.size());
	EXPECT_EQ(100u, hc.coffStrength);

	auto hm = computeFormatLatticeHints(machoBytes.data(), machoBytes.size());
	EXPECT_EQ(100u, hm.machoSliceStrength);

	const std::vector<std::uint8_t> arHdr = {
		'!', '<', 'a', 'r', 'c', 'h', '>', '\n'
	};
	auto ha = computeFormatLatticeHints(arHdr.data(), arHdr.size());
	EXPECT_EQ(100u, ha.arArchiveStrength);
}

} // namespace tests
} // namespace fileformat
} // namespace retdec
