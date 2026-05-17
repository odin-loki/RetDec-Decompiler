/**
 * @file tests/loader_sim/loader_sim_test.cpp
 * @brief Unit tests for LoaderSim.
 *
 * Each test builds a minimal but structurally valid binary image in memory,
 * then runs one or more pipeline steps and asserts the expected result.
 *
 * Binary builders:
 *   PEBuilder   — constructs a minimal PE32+ (64-bit) image
 *   ELFBuilder  — constructs a minimal ELF64 image
 *
 * Both builders write into a std::vector<uint8_t> and expose helpers to
 * patch fields after layout (e.g. fill in RVAs after section placement).
 */

#include "retdec/loader_sim/loader_sim.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace retdec::loader_sim;

// ─── Utility: write little-endian integers into a byte buffer ─────────────────

static void w16(std::vector<uint8_t>& b, std::size_t off, uint16_t v)
{
    if (off + 2 > b.size()) b.resize(off + 2, 0);
    b[off]   = static_cast<uint8_t>(v);
    b[off+1] = static_cast<uint8_t>(v >> 8);
}
static void w32(std::vector<uint8_t>& b, std::size_t off, uint32_t v)
{
    if (off + 4 > b.size()) b.resize(off + 4, 0);
    for (int i = 0; i < 4; ++i) b[off+i] = static_cast<uint8_t>(v >> (i*8));
}
static void w64(std::vector<uint8_t>& b, std::size_t off, uint64_t v)
{
    if (off + 8 > b.size()) b.resize(off + 8, 0);
    for (int i = 0; i < 8; ++i) b[off+i] = static_cast<uint8_t>(v >> (i*8));
}
static void wStr(std::vector<uint8_t>& b, std::size_t off, const std::string& s)
{
    if (off + s.size() + 1 > b.size()) b.resize(off + s.size() + 1, 0);
    std::memcpy(b.data() + off, s.c_str(), s.size() + 1);
}

// ─── Minimal PE32+ builder ─────────────────────────────────────────────────────
//
// Layout (all at fixed offsets for simplicity):
//   0x000 DOS stub (64 bytes, e_lfanew = 0x40)
//   0x040 IMAGE_NT_HEADERS64
//         0x040: "PE\0\0" (4)
//         0x044: IMAGE_FILE_HEADER (20)
//         0x058: IMAGE_OPTIONAL_HEADER64 (240) → optional header magic, image base, etc.
//   0x148 Section table (first section at this offset, 40 bytes each)
//
// Section data appended after section table (raw data area at 0x200+).
//
// Data directories are at opt_hdr + 112 (PE32+).

class PEBuilder {
public:
    static constexpr uint64_t kImageBase = 0x140000000ULL;
    static constexpr std::size_t kNTOff  = 0x40;
    static constexpr std::size_t kOptOff = kNTOff + 4 + 20;      // 0x58
    static constexpr std::size_t kSecTbl = kOptOff + 240;         // 0x140
    static constexpr std::size_t kRawDataStart = 0x200;

    std::vector<uint8_t> buf;
    uint16_t numSections = 0;
    std::size_t rawCursor = kRawDataStart;

    PEBuilder()
    {
        buf.resize(kRawDataStart, 0);
        // DOS stub.
        buf[0] = 'M'; buf[1] = 'Z';
        w32(buf, 0x3C, static_cast<uint32_t>(kNTOff));

        // NT signature.
        buf[kNTOff]   = 'P'; buf[kNTOff+1] = 'E';

        // FILE_HEADER.
        w16(buf, kNTOff + 4,  0x8664); // Machine = AMD64
        // NumSections patched later.
        w16(buf, kNTOff + 4 + 16, 240); // SizeOfOptionalHeader

        // OPTIONAL_HEADER64.
        w16(buf, kOptOff,      0x020B); // Magic = PE32+
        w32(buf, kOptOff + 16, 0x1000); // AddressOfEntryPoint RVA
        w64(buf, kOptOff + 24, kImageBase);
        w32(buf, kOptOff + 36, 0x1000); // SectionAlignment
        w32(buf, kOptOff + 40, 0x200);  // FileAlignment
        w32(buf, kOptOff + 56, 0x10000);// SizeOfImage (dummy)
        w32(buf, kOptOff + 60, static_cast<uint32_t>(kRawDataStart)); // SizeOfHeaders
        w16(buf, kOptOff + 92, 16);     // NumberOfRvaAndSizes
    }

    // Add a section with the given content; return its RVA.
    uint32_t addSection(const std::string& name, const std::vector<uint8_t>& data,
                        uint32_t chars = 0x60000020 /*CODE|EXEC|READ*/)
    {
        uint32_t rva    = static_cast<uint32_t>((rawCursor + 0xFFF) & ~0xFFFULL);
        uint32_t rawOff = static_cast<uint32_t>(rawCursor);

        std::size_t secBase = kSecTbl + numSections * 40;
        if (buf.size() < secBase + 40) buf.resize(secBase + 40, 0);

        char nm[8] = {};
        std::memcpy(nm, name.c_str(), std::min(name.size(), std::size_t(8)));
        std::memcpy(buf.data() + secBase, nm, 8);

        w32(buf, secBase + 8,  static_cast<uint32_t>(data.size())); // VirtualSize
        w32(buf, secBase + 12, rva);
        w32(buf, secBase + 16, static_cast<uint32_t>(data.size())); // RawSize
        w32(buf, secBase + 20, rawOff);
        w32(buf, secBase + 36, chars);

        // Append raw data.
        if (buf.size() < rawCursor + data.size())
            buf.resize(rawCursor + data.size(), 0);
        std::memcpy(buf.data() + rawCursor, data.data(), data.size());
        rawCursor += data.size();
        // Align.
        rawCursor = (rawCursor + 0x1FF) & ~std::size_t(0x1FF);
        if (buf.size() < rawCursor) buf.resize(rawCursor, 0);

        ++numSections;
        w16(buf, kNTOff + 4 + 2, numSections);
        return rva;
    }

    // Set a data directory (idx, rva, size).
    void setDataDir(int idx, uint32_t rva, uint32_t sz)
    {
        std::size_t off = kOptOff + 112 + idx * 8;
        if (buf.size() < off + 8) buf.resize(off + 8, 0);
        w32(buf, off,     rva);
        w32(buf, off + 4, sz);
    }

    uint64_t va(uint32_t rva) const { return kImageBase + rva; }
};

// ─── Minimal ELF64 builder ────────────────────────────────────────────────────
//
// Layout:
//   0x000 ELF header (64 bytes)
//   0x040 Program headers (skipped — we don't need them for section parsing)
//   0x100 Section data area
//   After data: Section header table
//   After SHT:  .shstrtab (string table for section names)

class ELFBuilder {
public:
    static constexpr uint64_t kBase = 0x400000ULL;

    std::vector<uint8_t> buf;

    struct SecEntry {
        std::string name;
        uint32_t    type;
        uint32_t    flags;
        uint64_t    addr;
        std::size_t dataOff;
        std::size_t dataSize;
        uint64_t    entsize;
        uint32_t    link;
        uint32_t    info;
    };

    std::vector<SecEntry> secs;
    std::vector<uint8_t>  dataArea;
    std::size_t           dataBase = 0x100; // raw offset where section data starts

    ELFBuilder()
    {
        buf.resize(64, 0);
        // ELF magic.
        buf[0]=0x7F; buf[1]='E'; buf[2]='L'; buf[3]='F';
        buf[4]=2;    // EI_CLASS = ELFCLASS64
        buf[5]=1;    // EI_DATA  = ELFDATA2LSB
        buf[6]=1;    // EI_VERSION
        // e_type = ET_DYN (3) — shared object.
        w16(buf, 16, 3);
        // e_machine = EM_X86_64 (62).
        w16(buf, 18, 62);
        // e_version = 1.
        w32(buf, 20, 1);
        // e_entry (patch later).
        w64(buf, 24, kBase + 0x1000);
        // e_shentsize = 64.
        w16(buf, 58, 64);
        // e_shstrndx patched in finalise().

        // Add the mandatory null section (index 0).
        SecEntry null{};
        null.name = "";
        null.type = 0;
        secs.push_back(null);
    }

    // Add a section with content; returns the section index.
    uint32_t addSection(const std::string& name, uint32_t type, uint32_t flags,
                        uint64_t addr, const std::vector<uint8_t>& data,
                        uint64_t entsize = 0, uint32_t link = 0, uint32_t info = 0)
    {
        SecEntry se;
        se.name     = name;
        se.type     = type;
        se.flags    = flags;
        se.addr     = addr;
        se.dataOff  = dataArea.size();
        se.dataSize = data.size();
        se.entsize  = entsize;
        se.link     = link;
        se.info     = info;
        dataArea.insert(dataArea.end(), data.begin(), data.end());
        // Align to 8 bytes.
        while (dataArea.size() % 8) dataArea.push_back(0);
        secs.push_back(se);
        return static_cast<uint32_t>(secs.size() - 1);
    }

    // Finalise: write data area + SHT + shstrtab into buf.
    std::vector<uint8_t> finalise()
    {
        // Build shstrtab.
        std::vector<uint8_t> strtab;
        strtab.push_back(0); // index 0 = empty name
        std::vector<uint32_t> nameOffs(secs.size());
        for (std::size_t i = 0; i < secs.size(); ++i) {
            nameOffs[i] = static_cast<uint32_t>(strtab.size());
            const auto& nm = secs[i].name;
            strtab.insert(strtab.end(), nm.begin(), nm.end());
            strtab.push_back(0);
        }
        // Add shstrtab section.
        SecEntry shstr;
        shstr.name     = ".shstrtab";
        shstr.type     = 3; // SHT_STRTAB
        shstr.flags    = 0;
        shstr.addr     = 0;
        shstr.dataOff  = dataArea.size();
        shstr.dataSize = strtab.size();
        shstr.entsize  = 0;
        shstr.link = shstr.info = 0;
        dataArea.insert(dataArea.end(), strtab.begin(), strtab.end());
        while (dataArea.size() % 8) dataArea.push_back(0);

        uint32_t shstrIdx = static_cast<uint32_t>(secs.size());
        // Fix shstrIdx nameoff.
        nameOffs.push_back(static_cast<uint32_t>(
            1 + [&]{ uint32_t o=1; for(auto& s:secs){ o+=(uint32_t)s.name.size()+1; } return o; }()
        ));
        secs.push_back(shstr);

        // Layout: buf = ELF header (64) + data area + SHT
        std::size_t dataOff = 64; // immediately after ELF header
        std::size_t shtOff  = dataOff + dataArea.size();
        std::size_t totalSz = shtOff + secs.size() * 64;

        std::vector<uint8_t> out(totalSz, 0);
        std::memcpy(out.data(), buf.data(), 64);
        std::memcpy(out.data() + dataOff, dataArea.data(), dataArea.size());

        // Patch ELF header.
        w64(out, 40, static_cast<uint64_t>(shtOff)); // e_shoff
        w16(out, 60, static_cast<uint16_t>(secs.size())); // e_shnum
        w16(out, 62, static_cast<uint16_t>(shstrIdx));    // e_shstrndx
        w16(out, 58, 64);                                  // e_shentsize

        // Build section header table.
        for (std::size_t i = 0; i < secs.size(); ++i) {
            std::size_t sh = shtOff + i * 64;
            const auto& se = secs[i];

            uint32_t nameIdx = (i < nameOffs.size()) ? nameOffs[i] : 0;
            // Find nameIdx in shstrtab (simple: use accumulated offset).
            w32(out, sh + 0,  nameIdx);
            w32(out, sh + 4,  se.type);
            w64(out, sh + 8,  se.flags);
            w64(out, sh + 16, se.addr);
            w64(out, sh + 24, static_cast<uint64_t>(dataOff + se.dataOff));
            w64(out, sh + 32, static_cast<uint64_t>(se.dataSize));
            w32(out, sh + 40, se.link);
            w32(out, sh + 44, se.info);
            w64(out, sh + 48, 8);         // alignment
            w64(out, sh + 56, se.entsize);
        }
        return out;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: LoadedImage query helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoadedImageHelpers, IsNonDecompilable)
{
    LoadedImage img;
    img.nonDecompilable.push_back({0x1000, 0x1100, "test"});

    EXPECT_TRUE(img.isNonDecompilable(0x1000));
    EXPECT_TRUE(img.isNonDecompilable(0x10FF));
    EXPECT_FALSE(img.isNonDecompilable(0x1100)); // end is exclusive
    EXPECT_FALSE(img.isNonDecompilable(0x0FFF));
}

TEST(LoadedImageHelpers, ResolveImport)
{
    LoadedImage img;
    ImportRef r;
    r.vma = 0x5000; r.dll = "kernel32.dll"; r.symbol = "ExitProcess";
    img.imports.push_back(r);
    img.importByVma[0x5000] = 0;

    const ImportRef* found = img.resolveImport(0x5000);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->symbol, "ExitProcess");

    EXPECT_EQ(img.resolveImport(0x5008), nullptr);
}

TEST(LoadedImageHelpers, SectionAt)
{
    LoadedImage img;
    SectionDesc s;
    s.name = ".text"; s.vma = 0x1000; s.virtSize = 0x500;
    img.sections.push_back(s);

    EXPECT_NE(img.sectionAt(0x1000), nullptr);
    EXPECT_NE(img.sectionAt(0x14FF), nullptr);
    EXPECT_EQ(img.sectionAt(0x1500), nullptr);
    EXPECT_EQ(img.sectionAt(0x0FFF), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: PE section parsing
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PESectionParsing, BasicSectionNames)
{
    PEBuilder pb;
    pb.addSection(".text",  {0xC3}, 0x60000020);
    pb.addSection(".rdata", {0x00}, 0x40000040);
    pb.addSection(".data",  {0x00}, 0xC0000040);

    LoaderSim sim(pb.buf.data(), pb.buf.size(),
                  PEBuilder::kImageBase, true, false);
    auto secs = sim.parseSections();

    ASSERT_EQ(secs.size(), 3u);
    EXPECT_EQ(std::string(secs[0].name).substr(0,5), ".text");
    EXPECT_EQ(std::string(secs[1].name).substr(0,6), ".rdata");
    EXPECT_EQ(std::string(secs[2].name).substr(0,5), ".data");
}

TEST(PESectionParsing, ExecutableFlag)
{
    PEBuilder pb;
    pb.addSection(".text", {0x90}, 0x60000020); // executable
    pb.addSection(".data", {0x00}, 0xC0000040); // writable, not exec

    LoaderSim sim(pb.buf.data(), pb.buf.size(),
                  PEBuilder::kImageBase, true, false);
    auto secs = sim.parseSections();

    ASSERT_EQ(secs.size(), 2u);
    EXPECT_TRUE(secs[0].executable);
    EXPECT_FALSE(secs[1].executable);
    EXPECT_TRUE(secs[1].writable);
}

TEST(PESectionParsing, EmptyImageReturnsNoSections)
{
    std::vector<uint8_t> empty(16, 0);
    LoaderSim sim(empty.data(), empty.size(), 0, true, false);
    EXPECT_TRUE(sim.parseSections().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: PE IAT import resolution
// ═══════════════════════════════════════════════════════════════════════════════

// Build a minimal PE with one import descriptor for "test.dll" importing
// "TestFunc" by name and one ordinal import.
static std::vector<uint8_t> buildPEWithImports(uint64_t& outImpVMA)
{
    PEBuilder pb;

    // We'll build the import section manually.
    // Layout (all offsets relative to section start):
    //   0x00  IMAGE_IMPORT_DESCRIPTOR for test.dll (20 bytes)
    //   0x14  IMAGE_IMPORT_DESCRIPTOR sentinel    (20 bytes)
    //   0x28  IMAGE_IMPORT_BY_NAME for "TestFunc" (hint + name)
    //   0x38  DLL name "test.dll\0"
    //   0x42  INT array: [RVA_to_0x28, 0x80000001(ordinal 1), 0]
    //   0x56  IAT array: [RVA_to_0x28, 0x80000001, 0]
    //
    // All RVAs are relative to image base; section is at some RVA R.
    // We'll compute R after addSection returns it.

    const std::size_t kIBNOff  = 0x28;
    const std::size_t kDLLOff  = 0x38;
    const std::size_t kINTOff  = 0x42;
    const std::size_t kIATOff  = 0x56;
    const std::size_t kSecSize = 0x70;

    std::vector<uint8_t> impSec(kSecSize, 0);

    // Placeholder descriptor — we'll patch RVAs after knowing the section RVA.
    // For now just set name/thunk placeholders.
    wStr(impSec, kDLLOff, "test.dll");
    // IMAGE_IMPORT_BY_NAME: Hint(2) + "TestFunc\0"
    w16(impSec, kIBNOff, 1);
    wStr(impSec, kIBNOff + 2, "TestFunc");

    uint32_t secRva = pb.addSection(".idata", impSec, 0x40000040);

    // Now patch RVAs into descriptor.
    // IMAGE_IMPORT_DESCRIPTOR at offset 0 in section → raw offset = secRva_in_file.
    // OriginalFirstThunk (INT) RVA = secRva + kINTOff
    // Name RVA               = secRva + kDLLOff
    // FirstThunk (IAT) RVA   = secRva + kIATOff

    uint32_t intRva  = secRva + static_cast<uint32_t>(kINTOff);
    uint32_t dllRva  = secRva + static_cast<uint32_t>(kDLLOff);
    uint32_t iatRva  = secRva + static_cast<uint32_t>(kIATOff);
    uint32_t ibnRva  = secRva + static_cast<uint32_t>(kIBNOff);

    // Find raw offset of section in pb.buf.
    std::size_t secRawOff = 0;
    for (std::size_t i = 0; i < (std::size_t)pb.numSections; ++i) {
        std::size_t sh = PEBuilder::kSecTbl + i * 40;
        uint32_t rva = (pb.buf[sh+12]) | (pb.buf[sh+13]<<8) | (pb.buf[sh+14]<<16) | (pb.buf[sh+15]<<24);
        if (rva == secRva) {
            secRawOff = (pb.buf[sh+20]) | (pb.buf[sh+21]<<8) | (pb.buf[sh+22]<<16) | (pb.buf[sh+23]<<24);
            break;
        }
    }

    // Patch descriptor.
    w32(pb.buf, secRawOff + 0,  intRva);    // OriginalFirstThunk
    w32(pb.buf, secRawOff + 12, dllRva);    // Name
    w32(pb.buf, secRawOff + 16, iatRva);    // FirstThunk

    // Patch INT: [ibnRva, 0x80000001(ord1), 0]
    w64(pb.buf, secRawOff + kINTOff,      ibnRva);  // named import
    w64(pb.buf, secRawOff + kINTOff + 8,  (1ULL << 63) | 1); // ordinal 1
    w64(pb.buf, secRawOff + kINTOff + 16, 0);       // sentinel

    // Patch IAT: same as INT initially.
    w64(pb.buf, secRawOff + kIATOff,      ibnRva);
    w64(pb.buf, secRawOff + kIATOff + 8,  (1ULL << 63) | 1);
    w64(pb.buf, secRawOff + kIATOff + 16, 0);

    // Set import data directory.
    pb.setDataDir(1, secRva, static_cast<uint32_t>(kSecSize));

    outImpVMA = PEBuilder::kImageBase + iatRva;
    return pb.buf;
}

TEST(PEImports, NamedImportResolved)
{
    uint64_t iatBase;
    auto buf = buildPEWithImports(iatBase);

    LoaderSim sim(buf.data(), buf.size(), PEBuilder::kImageBase, true, false);
    auto imports = sim.resolvePEImports();

    ASSERT_FALSE(imports.empty());
    bool found = false;
    for (const auto& imp : imports) {
        if (imp.symbol == "TestFunc") { found = true; break; }
    }
    EXPECT_TRUE(found) << "TestFunc import not found";
}

TEST(PEImports, OrdinalImportResolved)
{
    uint64_t iatBase;
    auto buf = buildPEWithImports(iatBase);

    LoaderSim sim(buf.data(), buf.size(), PEBuilder::kImageBase, true, false);
    auto imports = sim.resolvePEImports();

    bool found = false;
    for (const auto& imp : imports) {
        if (imp.ordinal == 1 && imp.dll == "test.dll") { found = true; break; }
    }
    EXPECT_TRUE(found) << "Ordinal-1 import not found";
}

TEST(PEImports, DLLNameLowercased)
{
    uint64_t iatBase;
    auto buf = buildPEWithImports(iatBase);

    LoaderSim sim(buf.data(), buf.size(), PEBuilder::kImageBase, true, false);
    auto imports = sim.resolvePEImports();

    for (const auto& imp : imports) {
        for (char c : imp.dll) {
            EXPECT_FALSE(c >= 'A' && c <= 'Z') << "DLL name not lowercase: " << imp.dll;
        }
    }
}

TEST(PEImports, IATSlotMarkedNonDecompilable)
{
    uint64_t iatBase;
    auto buf = buildPEWithImports(iatBase);

    LoaderSim sim(buf.data(), buf.size(), PEBuilder::kImageBase, true, false);
    LoadedImage img = sim.load();

    // IAT entries for both imports should be non-decompilable.
    EXPECT_TRUE(img.isNonDecompilable(iatBase));
    EXPECT_TRUE(img.isNonDecompilable(iatBase + 8));
}

TEST(PEImports, EmptyImportDir)
{
    PEBuilder pb;
    pb.addSection(".text", {0xC3}, 0x60000020);
    // No import directory set.

    LoaderSim sim(pb.buf.data(), pb.buf.size(), PEBuilder::kImageBase, true, false);
    EXPECT_TRUE(sim.resolvePEImports().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: PE base relocations
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PERelocations, Dir64EntryRecorded)
{
    PEBuilder pb;
    // Create a .reloc section with one DIR64 relocation.
    // IMAGE_BASE_RELOCATION: VirtualAddress (4), SizeOfBlock (4), entries (2 each).
    // One DIR64 entry at offset 0x100 in page 0x1000.
    std::vector<uint8_t> reloc(12, 0);
    w32(reloc, 0, 0x1000);  // VirtualAddress of page
    w32(reloc, 4, 12);      // SizeOfBlock = 8 header + 4 for 2 entries
    uint16_t entry = (10 << 12) | 0x100; // type=DIR64, offset=0x100
    w16(reloc, 8, entry);
    w16(reloc, 10, 0); // padding entry (type=0)

    uint32_t relocRva = pb.addSection(".reloc", reloc, 0x42000040);
    pb.setDataDir(5, relocRva, static_cast<uint32_t>(reloc.size()));

    LoaderSim sim(pb.buf.data(), pb.buf.size(), PEBuilder::kImageBase, true, false);
    auto recs = sim.applyPERelocations(PEBuilder::kImageBase + 0x10000);

    ASSERT_GE(recs.size(), 1u);
    EXPECT_EQ(recs[0].type, 10u);
}

TEST(PERelocations, EmptyRelocDir)
{
    PEBuilder pb;
    pb.addSection(".text", {0xC3});
    LoaderSim sim(pb.buf.data(), pb.buf.size(), PEBuilder::kImageBase, true, false);
    EXPECT_TRUE(sim.applyPERelocations(PEBuilder::kImageBase + 0x1000).empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: PE TLS callbacks
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PETLSCallbacks, TwoCallbacksEnumerated)
{
    PEBuilder pb;

    // TLS section layout (PE32+):
    //   0x00  IMAGE_TLS_DIRECTORY64 (40 bytes)
    //     StartAddressOfRawData  (8)
    //     EndAddressOfRawData    (8)
    //     AddressOfIndex         (8)
    //     AddressOfCallBacks     (8) ← VA pointing to callback table
    //     SizeOfZeroFill         (4)
    //     Characteristics        (4)
    //   0x28  Callback table: [VA_cb0, VA_cb1, 0]
    // Total: 40 + 24 = 64 bytes

    const uint32_t kCBTableOff = 40;
    std::vector<uint8_t> tlsSec(64, 0);

    uint32_t tlsRva   = pb.addSection(".tls", tlsSec, 0xC0000040);
    uint64_t cbTableVA = PEBuilder::kImageBase + tlsRva + kCBTableOff;

    // Patch AddressOfCallBacks field (offset 24 in IMAGE_TLS_DIRECTORY64).
    // Find raw offset.
    std::size_t secRawOff = 0;
    for (std::size_t i = 0; i < (std::size_t)pb.numSections; ++i) {
        std::size_t sh = PEBuilder::kSecTbl + i * 40;
        uint32_t rva = (pb.buf[sh+12])|(pb.buf[sh+13]<<8)|(pb.buf[sh+14]<<16)|(pb.buf[sh+15]<<24);
        if (rva == tlsRva) {
            secRawOff = (pb.buf[sh+20])|(pb.buf[sh+21]<<8)|(pb.buf[sh+22]<<16)|(pb.buf[sh+23]<<24);
            break;
        }
    }
    // IMAGE_TLS_DIRECTORY64.AddressOfCallBacks at offset 24.
    w64(pb.buf, secRawOff + 24, cbTableVA);

    // Callback table: two callback VAs + null terminator.
    uint64_t cb0 = PEBuilder::kImageBase + 0x2000;
    uint64_t cb1 = PEBuilder::kImageBase + 0x3000;
    w64(pb.buf, secRawOff + kCBTableOff,      cb0);
    w64(pb.buf, secRawOff + kCBTableOff +  8, cb1);
    w64(pb.buf, secRawOff + kCBTableOff + 16, 0);

    // Set TLS data directory (index 9).
    pb.setDataDir(9, tlsRva, 40);

    LoaderSim sim(pb.buf.data(), pb.buf.size(), PEBuilder::kImageBase, true, false);
    auto cbs = sim.parsePETLS();

    ASSERT_EQ(cbs.size(), 2u);
    EXPECT_EQ(cbs[0].vma, cb0);
    EXPECT_EQ(cbs[1].vma, cb1);
    EXPECT_EQ(cbs[0].syntheticName, "__tls_callback_0");
    EXPECT_EQ(cbs[1].syntheticName, "__tls_callback_1");
}

TEST(PETLSCallbacks, NoTLSDir)
{
    PEBuilder pb;
    pb.addSection(".text", {0xC3});
    LoaderSim sim(pb.buf.data(), pb.buf.size(), PEBuilder::kImageBase, true, false);
    EXPECT_TRUE(sim.parsePETLS().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: ELF section parsing
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ELFSectionParsing, SectionNamesAndAttributes)
{
    ELFBuilder eb;
    eb.addSection(".text",  1/*SHT_PROGBITS*/, 6/*AX*/, 0x401000, {0x90}, 0);
    eb.addSection(".data",  1,                  3/*WA*/, 0x402000, {0x00}, 0);
    eb.addSection(".rodata",1,                  2/*A*/,  0x403000, {0x00}, 0);

    auto buf = eb.finalise();
    LoaderSim sim(buf.data(), buf.size(), ELFBuilder::kBase, true, true);
    auto secs = sim.parseSections();

    // Check that .text, .data, .rodata are present (null section not in parseSections).
    bool hasText = false, hasData = false;
    for (const auto& s : secs) {
        if (s.name == ".text")  { hasText = true; EXPECT_TRUE(s.executable); }
        if (s.name == ".data")  { hasData = true; EXPECT_TRUE(s.writable); }
    }
    EXPECT_TRUE(hasText);
    EXPECT_TRUE(hasData);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: ELF PLT import resolution
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ELFImports, PLTImportsResolved)
{
    ELFBuilder eb;

    // .dynstr: "\0printf\0exit\0"
    std::vector<uint8_t> dynstr = {0, 'p','r','i','n','t','f',0, 'e','x','i','t',0};
    uint32_t dynstrIdx = eb.addSection(".dynstr", 3/*SHT_STRTAB*/, 2, 0, dynstr, 0);

    // .dynsym: 2 symbols (printf at index 1, exit at index 2).
    // Each Elf64_Sym = 24 bytes: st_name(4), st_info(1), st_other(1), st_shndx(2),
    //                             st_value(8), st_size(8).
    std::vector<uint8_t> dynsym(3 * 24, 0);
    w32(dynsym, 0,  0); // null symbol
    w32(dynsym, 24, 1); // printf: st_name = 1
    w32(dynsym, 48, 8); // exit:   st_name = 8
    uint32_t dynsymIdx = eb.addSection(".dynsym", 11/*SHT_DYNSYM*/, 2, 0,
                                        dynsym, 24, dynstrIdx, 1);

    // .plt: 3 * 16 = 48 bytes (resolver + 2 stubs).
    std::vector<uint8_t> plt(48, 0x90);
    uint32_t pltIdx = eb.addSection(".plt", 1, 6/*AX*/, 0x401000, plt, 16);
    (void)pltIdx;

    // .rela.plt: 2 entries (one per PLT stub).
    // Elf64_Rela: r_offset(8), r_info(8), r_addend(8) = 24 bytes each.
    std::vector<uint8_t> relaPlt(2 * 24, 0);
    // Entry 0: symbol 1 (printf), R_X86_64_JUMP_SLOT (7)
    w64(relaPlt, 0,  0x404000);                         // r_offset (GOT slot VA)
    w64(relaPlt, 8,  (uint64_t(1) << 32) | 7);          // r_info: sym=1, type=7
    // Entry 1: symbol 2 (exit), R_X86_64_JUMP_SLOT (7)
    w64(relaPlt, 24, 0x404008);
    w64(relaPlt, 32, (uint64_t(2) << 32) | 7);
    eb.addSection(".rela.plt", 4/*SHT_RELA*/, 0x42, 0,
                   relaPlt, 24, dynsymIdx, 0);

    auto buf = eb.finalise();
    LoaderSim sim(buf.data(), buf.size(), ELFBuilder::kBase, true, true);
    auto imports = sim.resolveELFImports();

    ASSERT_EQ(imports.size(), 2u);
    EXPECT_EQ(imports[0].symbol, "printf");
    EXPECT_EQ(imports[1].symbol, "exit");
    // PLT stub 0 is at 0x401000 + 16 + 0*16 = 0x401010
    EXPECT_EQ(imports[0].vma, 0x401010ULL);
    // PLT stub 1 at 0x401000 + 16 + 1*16 = 0x401020
    EXPECT_EQ(imports[1].vma, 0x401020ULL);
}

TEST(ELFImports, PLTMarkedNonDecompilable)
{
    ELFBuilder eb;
    std::vector<uint8_t> dynstr = {0, 'm','a','l','l','o','c',0};
    uint32_t dynstrIdx = eb.addSection(".dynstr", 3, 2, 0, dynstr, 0);
    std::vector<uint8_t> dynsym(2 * 24, 0);
    w32(dynsym, 24, 1);
    uint32_t dynsymIdx = eb.addSection(".dynsym", 11, 2, 0, dynsym, 24, dynstrIdx, 1);
    std::vector<uint8_t> plt(32, 0x90);
    eb.addSection(".plt", 1, 6, 0x401000, plt, 16);
    std::vector<uint8_t> relaPlt(24, 0);
    w64(relaPlt, 0, 0x404000);
    w64(relaPlt, 8, (uint64_t(1) << 32) | 7);
    eb.addSection(".rela.plt", 4, 0x42, 0, relaPlt, 24, dynsymIdx, 0);

    auto buf = eb.finalise();
    LoaderSim sim(buf.data(), buf.size(), ELFBuilder::kBase, true, true);
    LoadedImage img = sim.load();

    // .plt at 0x401000 should be non-decompilable.
    EXPECT_TRUE(img.isNonDecompilable(0x401000));
    EXPECT_TRUE(img.isNonDecompilable(0x401010));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: ELF .init_array / TLS callbacks
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ELFInitArray, TwoCallbacksEnumerated)
{
    ELFBuilder eb;
    // .init_array: two 64-bit function pointers.
    std::vector<uint8_t> initArr(16, 0);
    w64(initArr, 0, 0x401100); // cb0
    w64(initArr, 8, 0x401200); // cb1
    eb.addSection(".init_array", 14/*SHT_INIT_ARRAY*/, 3, 0x405000, initArr, 8);

    auto buf = eb.finalise();
    LoaderSim sim(buf.data(), buf.size(), ELFBuilder::kBase, true, true);
    auto cbs = sim.parseELFInitArray();

    ASSERT_EQ(cbs.size(), 2u);
    EXPECT_EQ(cbs[0].vma, 0x401100ULL);
    EXPECT_EQ(cbs[1].vma, 0x401200ULL);
    EXPECT_EQ(cbs[0].syntheticName, "__tls_callback_0");
}

TEST(ELFInitArray, ZeroPointerSkipped)
{
    ELFBuilder eb;
    std::vector<uint8_t> initArr(24, 0);
    w64(initArr, 0, 0x401100);
    w64(initArr, 8, 0);          // zero pointer — must be skipped
    w64(initArr, 16, 0x401200);
    eb.addSection(".init_array", 14, 3, 0x405000, initArr, 8);

    auto buf = eb.finalise();
    LoaderSim sim(buf.data(), buf.size(), ELFBuilder::kBase, true, true);
    auto cbs = sim.parseELFInitArray();

    ASSERT_EQ(cbs.size(), 2u);
    EXPECT_EQ(cbs[0].vma, 0x401100ULL);
    EXPECT_EQ(cbs[1].vma, 0x401200ULL);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: ELF RELA relocations
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ELFRelocations, RelativeRelocationRecorded)
{
    ELFBuilder eb;
    // .rela.dyn: one R_X86_64_RELATIVE (type=8) entry.
    std::vector<uint8_t> relaDyn(24, 0);
    w64(relaDyn, 0,  0x402008);            // r_offset
    w64(relaDyn, 8,  8u);                  // r_info: sym=0, type=8 (RELATIVE)
    w64(relaDyn, 16, 0x400000);            // r_addend
    eb.addSection(".rela.dyn", 4/*SHT_RELA*/, 0, 0, relaDyn, 24, 0, 0);

    auto buf = eb.finalise();
    LoaderSim sim(buf.data(), buf.size(), ELFBuilder::kBase, true, true);
    auto recs = sim.applyELFRelocations(ELFBuilder::kBase);

    ASSERT_GE(recs.size(), 1u);
    EXPECT_EQ(recs[0].type, 8u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: PE delay-load imports
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PEDelayLoad, ImportsResolvedAsDelayLoad)
{
    PEBuilder pb;

    // ImgDelayDescr layout (32 bytes):
    //   Attrs     (4): bit0=1 → RVA-based
    //   rvaDLLName(4): RVA of DLL name string
    //   rvaHmod   (4): RVA of module handle slot
    //   rvaIAT    (4): RVA of delay IAT
    //   rvaINT    (4): RVA of INT (name table)
    //   rvaBound  (4)
    //   rvaUnload (4)
    //   dwTimeStamp(4)
    //   Sentinel  (32 bytes of zeros)
    //
    // INT: [RVA to IMAGE_IMPORT_BY_NAME, 0]
    // IBN: Hint(2) + "DelayFunc\0"
    // IAT: [0, 0] (null initially)
    // DLL: "delay.dll\0"

    const std::size_t kDescOff  = 0;
    const std::size_t kSentOff  = 32;
    const std::size_t kDLLOff   = 64;
    const std::size_t kINTOff   = 80;
    const std::size_t kIBNOff   = 96;
    const std::size_t kIATOff   = 116;
    const std::size_t kSecSize  = 136;

    std::vector<uint8_t> dlySec(kSecSize, 0);
    wStr(dlySec, kDLLOff, "delay.dll");

    // IMAGE_IMPORT_BY_NAME: hint=0 + "DelayFunc"
    wStr(dlySec, kIBNOff + 2, "DelayFunc");

    uint32_t dlyRva = pb.addSection(".didat", dlySec, 0x40000040);

    // Find raw offset of the section.
    std::size_t secRawOff = 0;
    for (std::size_t i = 0; i < (std::size_t)pb.numSections; ++i) {
        std::size_t sh = PEBuilder::kSecTbl + i * 40;
        uint32_t rva = (pb.buf[sh+12])|(pb.buf[sh+13]<<8)|(pb.buf[sh+14]<<16)|(pb.buf[sh+15]<<24);
        if (rva == dlyRva) {
            secRawOff=(pb.buf[sh+20])|(pb.buf[sh+21]<<8)|(pb.buf[sh+22]<<16)|(pb.buf[sh+23]<<24);
            break;
        }
    }

    uint32_t dllRva  = dlyRva + static_cast<uint32_t>(kDLLOff);
    uint32_t intRva  = dlyRva + static_cast<uint32_t>(kINTOff);
    uint32_t ibnRva  = dlyRva + static_cast<uint32_t>(kIBNOff);
    uint32_t iatRva  = dlyRva + static_cast<uint32_t>(kIATOff);

    // ImgDelayDescr.
    w32(pb.buf, secRawOff + kDescOff +  0, 1);       // Attrs = RVA-based
    w32(pb.buf, secRawOff + kDescOff +  4, dllRva);
    w32(pb.buf, secRawOff + kDescOff +  8, 0);        // hmod (irrelevant)
    w32(pb.buf, secRawOff + kDescOff + 12, iatRva);
    w32(pb.buf, secRawOff + kDescOff + 16, intRva);

    // INT: [ibnRva, 0]
    w64(pb.buf, secRawOff + kINTOff,     ibnRva);
    w64(pb.buf, secRawOff + kINTOff + 8, 0);

    // Set delay-load data directory (index 13).
    pb.setDataDir(13, dlyRva, static_cast<uint32_t>(kSecSize));

    LoaderSim sim(pb.buf.data(), pb.buf.size(), PEBuilder::kImageBase, true, false);
    auto imports = sim.resolvePEDelayImports();

    ASSERT_GE(imports.size(), 1u);
    EXPECT_EQ(imports[0].symbol, "DelayFunc");
    EXPECT_EQ(imports[0].dll, "delay.dll");
    EXPECT_TRUE(imports[0].isDelayLoad);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests: full load() pipeline
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LoadPipeline, PEEntryPoint)
{
    PEBuilder pb;
    pb.addSection(".text", {0xC3}, 0x60000020);

    LoaderSim sim(pb.buf.data(), pb.buf.size(), PEBuilder::kImageBase, true, false);
    LoadedImage img = sim.load();

    // Entry point RVA = 0x1000 (set in PEBuilder constructor).
    EXPECT_EQ(img.entryPoint, PEBuilder::kImageBase + 0x1000);
}

TEST(LoadPipeline, ELFEntryPoint)
{
    ELFBuilder eb;
    eb.addSection(".text", 1, 6, 0x401000, {0x90});
    auto buf = eb.finalise();

    LoaderSim sim(buf.data(), buf.size(), ELFBuilder::kBase, true, true);
    LoadedImage img = sim.load();

    // Entry point = ELFBuilder sets e_entry = kBase + 0x1000.
    EXPECT_EQ(img.entryPoint, ELFBuilder::kBase + 0x1000);
}

TEST(LoadPipeline, ImportByVmaMapPopulated)
{
    uint64_t iatBase;
    auto buf = buildPEWithImports(iatBase);

    LoaderSim sim(buf.data(), buf.size(), PEBuilder::kImageBase, true, false);
    LoadedImage img = sim.load();

    // importByVma should have entries.
    EXPECT_FALSE(img.importByVma.empty());
    // All mapped VMAs should resolve.
    for (auto& [vma, idx] : img.importByVma) {
        EXPECT_NE(img.resolveImport(vma), nullptr);
    }
}
