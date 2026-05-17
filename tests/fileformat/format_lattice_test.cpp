/**
 * @file tests/fileformat/format_lattice_test.cpp
 * @brief Unit tests for FormatLattice (Signature-Lattice Stage 1 parser).
 *
 * Uses hand-crafted minimal binary headers to test every lattice decision
 * node and the associated plausibility checks / corruption flags.
 */

#include "retdec/fileformat/lattice/format_lattice.h"
#include "retdec/fileformat/lattice/format_result.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace retdec::fileformat::lattice;

// ─── Fixture ─────────────────────────────────────────────────────────────────

class FormatLatticeTest : public ::testing::Test {
protected:
    FormatLattice lattice;
};

// ─── Intel HEX ───────────────────────────────────────────────────────────────

TEST_F(FormatLatticeTest, IntelHexMagic)
{
    const uint8_t data[] = {':','1','0','0','0','0','0','0',':','0','0'};
    auto r = lattice.classify(data, sizeof(data), "test.hex");
    EXPECT_EQ(r.format, DetectedFormat::IntelHex);
}

// ─── ELF32 LE ────────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMinimalELF32LE()
{
    std::vector<uint8_t> buf(0x34 + 4, 0); // header only
    // e_ident
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1;    // ELFCLASS32
    buf[5] = 1;    // ELFDATA2LSB
    buf[6] = 1;    // EV_CURRENT
    // e_type at 16: ET_EXEC = 2
    buf[16] = 2; buf[17] = 0;
    // e_machine at 18: x86 = 3
    buf[18] = 3; buf[19] = 0;
    // e_entry at 24: 0x08048000
    buf[24] = 0x00; buf[25] = 0x80; buf[26] = 0x04; buf[27] = 0x08;
    // e_phoff = 0, e_shoff = 0 (no sections/segments)
    // e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx all 0
    buf[42] = 0x20; buf[43] = 0; // e_phentsize = 32
    // e_shentsize at 46: 0 → no sections
    return buf;
}

TEST_F(FormatLatticeTest, ELF32LE)
{
    auto data = makeMinimalELF32LE();
    auto r = lattice.classify(data.data(), data.size(), "a.out");
    EXPECT_EQ(r.format,      DetectedFormat::ELF32);
    EXPECT_EQ(r.endianness,  Endianness::Little);
    EXPECT_EQ(r.addressSize, 32u);
    EXPECT_EQ(r.architecture, Arch::X86);
    EXPECT_EQ(r.entryPoint,  0x08048000u);
}

// ─── ELF64 LE ────────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMinimalELF64LE()
{
    std::vector<uint8_t> buf(0x40 + 4, 0);
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 2;    // ELFCLASS64
    buf[5] = 1;    // ELFDATA2LSB
    buf[6] = 1;
    buf[16] = 2; buf[17] = 0; // ET_EXEC
    buf[18] = 0x3E; buf[19] = 0; // x86-64
    // e_entry at 24 (8 bytes): 0x400000
    buf[24] = 0x00; buf[25] = 0x00; buf[26] = 0x40; buf[27] = 0x00;
    // e_phentsize at 54
    buf[54] = 56; buf[55] = 0;
    return buf;
}

TEST_F(FormatLatticeTest, ELF64LE)
{
    auto data = makeMinimalELF64LE();
    auto r = lattice.classify(data.data(), data.size(), "elf64");
    EXPECT_EQ(r.format,      DetectedFormat::ELF64);
    EXPECT_EQ(r.endianness,  Endianness::Little);
    EXPECT_EQ(r.addressSize, 64u);
    EXPECT_EQ(r.architecture, Arch::X86_64);
    EXPECT_EQ(r.entryPoint,  0x400000u);
}

// ─── ELF32 BE ────────────────────────────────────────────────────────────────

TEST_F(FormatLatticeTest, ELF32BE_MIPS)
{
    std::vector<uint8_t> buf(0x34 + 4, 0);
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1;    // ELFCLASS32
    buf[5] = 2;    // ELFDATA2MSB
    buf[6] = 1;
    buf[16] = 0; buf[17] = 2; // ET_EXEC (big endian)
    buf[18] = 0; buf[19] = 8; // MIPS (big endian: 0x0008)
    auto r = lattice.classify(buf.data(), buf.size(), "mips.elf");
    EXPECT_EQ(r.format,      DetectedFormat::ELF32);
    EXPECT_EQ(r.endianness,  Endianness::Big);
    EXPECT_EQ(r.architecture, Arch::MIPS);
}

// ─── PE32 ────────────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMinimalPE32()
{
    std::vector<uint8_t> buf(0x200, 0);
    // DOS header
    buf[0] = 'M'; buf[1] = 'Z';
    // e_lfanew at 0x3C = 0x80
    buf[0x3C] = 0x80; buf[0x3D] = 0; buf[0x3E] = 0; buf[0x3F] = 0;
    // PE signature at 0x80
    buf[0x80] = 'P'; buf[0x81] = 'E'; buf[0x82] = 0; buf[0x83] = 0;
    // COFF: machine = 0x014C (x86)
    buf[0x84] = 0x4C; buf[0x85] = 0x01;
    // num_sections = 1
    buf[0x86] = 1; buf[0x87] = 0;
    // opt_size = 0xE0 (224 = PE32 optional header)
    buf[0x94] = 0xE0; buf[0x95] = 0;
    // Optional header at 0x98: magic = 0x010B (PE32)
    buf[0x98] = 0x0B; buf[0x99] = 0x01;
    // AddressOfEntryPoint at 0x98 + 16 = 0xA8
    buf[0xA8] = 0x00; buf[0xA9] = 0x10; buf[0xAA] = 0x00; buf[0xAB] = 0x00; // RVA = 0x1000
    // ImageBase at 0x98 + 28 = 0xB4
    buf[0xB4] = 0x00; buf[0xB5] = 0x00; buf[0xB6] = 0x40; buf[0xB7] = 0x00; // 0x400000
    // SizeOfImage at 0x98 + 56 = 0xD0
    buf[0xD0] = 0x00; buf[0xD1] = 0x00; buf[0xD2] = 0x10; buf[0xD3] = 0x00; // 0x100000
    // NumberOfRvaAndSizes at 0x98 + 92 = 0xF4
    buf[0xF4] = 16; // 16 data directories
    // Section table at 0x98 + 0xE0 = 0x178
    // ".text\0\0\0", VirtualSize=0x100, VirtualAddress=0x1000, SizeOfRawData=0x200, PointerToRawData=0x200
    std::memcpy(buf.data() + 0x178, ".text\0\0\0", 8);
    buf[0x178+8]  = 0x00; buf[0x178+9]  = 0x01; // VirtualSize = 0x100
    buf[0x178+12] = 0x00; buf[0x178+13] = 0x10; // VirtualAddress = 0x1000
    // Characteristics: execute+read = 0x60000020
    buf[0x178+36] = 0x20; buf[0x178+37] = 0x00; buf[0x178+38] = 0x00; buf[0x178+39] = 0x60;
    return buf;
}

TEST_F(FormatLatticeTest, PE32Detection)
{
    auto data = makeMinimalPE32();
    auto r = lattice.classify(data.data(), data.size(), "test.exe");
    EXPECT_EQ(r.format,       DetectedFormat::PE32);
    EXPECT_EQ(r.addressSize,  32u);
    EXPECT_EQ(r.architecture, Arch::X86);
    EXPECT_EQ(r.imageBase,    0x400000u);
    EXPECT_EQ(r.endianness,   Endianness::Little);
}

TEST_F(FormatLatticeTest, PE32SectionsParsed)
{
    auto data = makeMinimalPE32();
    auto r = lattice.classify(data.data(), data.size(), "test.exe");
    ASSERT_GE(r.sections.size(), 1u);
    EXPECT_EQ(r.sections[0].name, ".text");
    EXPECT_TRUE(r.sections[0].isExecutable);
}

// ─── PE64 ────────────────────────────────────────────────────────────────────

TEST_F(FormatLatticeTest, PE64MagicRecognised)
{
    std::vector<uint8_t> buf(0x200, 0);
    buf[0] = 'M'; buf[1] = 'Z';
    buf[0x3C] = 0x80;
    buf[0x80] = 'P'; buf[0x81] = 'E';
    buf[0x84] = 0x64; buf[0x85] = 0x86; // machine = 0x8664 (x64)
    buf[0x94] = 0xF0; buf[0x95] = 0; // opt_size = 240 (PE64 standard)
    buf[0x98] = 0x0B; buf[0x99] = 0x02; // PE64 magic
    // ImageBase (8 bytes) at 0x98 + 24 = 0xB0
    buf[0xB0] = 0x00; buf[0xB1] = 0x00; buf[0xB2] = 0x00; buf[0xB3] = 0x00;
    buf[0xB4] = 0x00; buf[0xB5] = 0x00; buf[0xB6] = 0x40; buf[0xB7] = 0x00; // 0x0000400000000000? actually high bytes
    // We'll get something; just check format is PE64
    auto r = lattice.classify(buf.data(), buf.size(), "x64.exe");
    EXPECT_EQ(r.format,       DetectedFormat::PE64);
    EXPECT_EQ(r.addressSize,  64u);
    EXPECT_EQ(r.architecture, Arch::X86_64);
}

// ─── Mach-O 32 LE ────────────────────────────────────────────────────────────

TEST_F(FormatLatticeTest, MachO32LE)
{
    std::vector<uint8_t> buf(32, 0);
    // FEEDFACE in LE
    buf[0] = 0xCE; buf[1] = 0xFA; buf[2] = 0xED; buf[3] = 0xFE;
    // cputype = 7 (x86) in LE
    buf[4] = 7; buf[5] = 0; buf[6] = 0; buf[7] = 0;
    auto r = lattice.classify(buf.data(), buf.size(), "macho32");
    EXPECT_EQ(r.format,       DetectedFormat::MachO32);
    EXPECT_EQ(r.addressSize,  32u);
    EXPECT_EQ(r.architecture, Arch::X86);
    EXPECT_EQ(r.endianness,   Endianness::Little); // CE FA ED FE = MH_CIGAM = little-endian MachO
}

TEST_F(FormatLatticeTest, MachO64LE)
{
    std::vector<uint8_t> buf(32, 0);
    // FEEDFACF in LE: bytes CF FA ED FE
    buf[0] = 0xCF; buf[1] = 0xFA; buf[2] = 0xED; buf[3] = 0xFE;
    buf[4] = 0x07; // cputype low = 7 | CPU_ARCH_ABI64
    buf[7] = 0x01; // CPU_ARCH_ABI64 high byte
    auto r = lattice.classify(buf.data(), buf.size(), "macho64");
    EXPECT_EQ(r.format,       DetectedFormat::MachO64);
    EXPECT_EQ(r.addressSize,  64u);
}

// ─── Mach-O Fat (CAFEBABE) ───────────────────────────────────────────────────

TEST_F(FormatLatticeTest, CafeBabeFatMachO)
{
    // CAFEBABE with nfat_arch = 2
    std::vector<uint8_t> buf(8 + 2*20, 0);
    buf[0] = 0xCA; buf[1] = 0xFE; buf[2] = 0xBA; buf[3] = 0xBE;
    buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 2; // nfat_arch = 2 (big-endian)
    auto r = lattice.classify(buf.data(), buf.size(), "fat.macho");
    EXPECT_EQ(r.format, DetectedFormat::MachOFat);
}

TEST_F(FormatLatticeTest, CafeBabeJavaUnknown)
{
    // CAFEBABE with second word 50 → Java .class version 50 (Java 6)
    std::vector<uint8_t> buf(8, 0);
    buf[0] = 0xCA; buf[1] = 0xFE; buf[2] = 0xBA; buf[3] = 0xBE;
    buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 50; // big-endian 50
    auto r = lattice.classify(buf.data(), buf.size(), "Foo.class");
    EXPECT_EQ(r.format, DetectedFormat::Unknown);
}

// ─── AR Archive ──────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMinimalAR(std::vector<std::pair<std::string, std::vector<uint8_t>>> members)
{
    std::vector<uint8_t> ar;
    // Global header
    const uint8_t hdr[] = "!<arch>\n";
    ar.insert(ar.end(), hdr, hdr + 8);
    for (auto &[mname, mdata] : members) {
        // Member header: 60 bytes
        uint8_t mhdr[60];
        std::memset(mhdr, ' ', 60);
        // Name (16 bytes)
        std::memcpy(mhdr, mname.c_str(), std::min(mname.size(), (size_t)16));
        // Timestamp (12 bytes at 16): "0"
        mhdr[16] = '0';
        // UID/GID/Mode (6+6+8 bytes): spaces
        // Size (10 bytes at 48)
        std::string sz = std::to_string(mdata.size());
        std::memcpy(mhdr + 48, sz.c_str(), sz.size());
        // End magic (2 bytes at 58)
        mhdr[58] = '`'; mhdr[59] = '\n';
        ar.insert(ar.end(), mhdr, mhdr + 60);
        ar.insert(ar.end(), mdata.begin(), mdata.end());
        if (mdata.size() & 1) ar.push_back('\n'); // padding
    }
    return ar;
}

TEST_F(FormatLatticeTest, ARArchiveDetection)
{
    auto elfData = makeMinimalELF32LE();
    auto ar = makeMinimalAR({{"foo.o", elfData}});
    auto r  = lattice.classify(ar.data(), ar.size(), "lib.a");
    EXPECT_EQ(r.format, DetectedFormat::ARArchive);
    ASSERT_GE(r.arMembers.size(), 1u);
    EXPECT_EQ(r.arMembers[0].format, DetectedFormat::ELF32);
}

TEST_F(FormatLatticeTest, ARMultipleMembers)
{
    auto e1 = makeMinimalELF32LE();
    auto e2 = makeMinimalELF64LE();
    auto ar = makeMinimalAR({{"a.o", e1}, {"b.o", e2}});
    auto r  = lattice.classify(ar.data(), ar.size(), "multi.a");
    EXPECT_EQ(r.format, DetectedFormat::ARArchive);
    EXPECT_EQ(r.arMembers.size(), 2u);
}

// ─── Empty / tiny input ──────────────────────────────────────────────────────

TEST_F(FormatLatticeTest, NullDataReturnsUnknown)
{
    auto r = lattice.classify(nullptr, 0, "");
    EXPECT_EQ(r.format, DetectedFormat::Unknown);
}

TEST_F(FormatLatticeTest, SingleByteReturnsRawOrUnknown)
{
    const uint8_t b = 0xAA;
    auto r = lattice.classify(&b, 1, "single");
    EXPECT_NE(r.format, DetectedFormat::ELF32);
    EXPECT_NE(r.format, DetectedFormat::PE32);
}

// ─── Malformed PE: corrupt e_lfanew ─────────────────────────────────────────

TEST_F(FormatLatticeTest, CorruptPElfanew)
{
    auto data = makeMinimalPE32();
    // Corrupt e_lfanew to point too far
    data[0x3C] = 0xFF; data[0x3D] = 0xFF; data[0x3E] = 0; data[0x3F] = 0;
    auto r = lattice.classify(data.data(), data.size(), "corrupt.exe");
    // Should either detect Unknown or attempt heuristic scan
    // Either way, must not crash
    EXPECT_TRUE(r.format == DetectedFormat::Unknown || r.format == DetectedFormat::PE32);
}

// ─── ELF: insane section count flagged ───────────────────────────────────────

TEST_F(FormatLatticeTest, ELFInsaneSectionCount)
{
    auto data = makeMinimalELF32LE();
    // Overwrite e_shnum at offset 48 with 9999+1 = 10000
    data[48] = 0x10; data[49] = 0x27; // 10000 in LE
    auto r = lattice.classify(data.data(), data.size(), "insane.elf");
    EXPECT_EQ(r.format, DetectedFormat::ELF32);
    EXPECT_TRUE(r.corruption.sectionCountSuspicious);
}

// ─── Raw fallback ────────────────────────────────────────────────────────────

TEST_F(FormatLatticeTest, UnknownBytesReturnRaw)
{
    const uint8_t data[] = {0xFF, 0xFE, 0x00, 0x00, 0xDE, 0xAD};
    auto r = lattice.classify(data, sizeof(data), "unknown");
    // Not a known format → raw
    EXPECT_EQ(r.format, DetectedFormat::Raw);
}

// ─── FormatResult helpers ────────────────────────────────────────────────────

TEST_F(FormatLatticeTest, FormatResultIsValid)
{
    auto data = makeMinimalELF64LE();
    auto r = lattice.classify(data.data(), data.size(), "v");
    EXPECT_TRUE(r.isValid());
    EXPECT_TRUE(r.is64Bit());
}

TEST_F(FormatLatticeTest, UnknownFormatNotValid)
{
    FormatResult r;
    EXPECT_FALSE(r.isValid());
}
