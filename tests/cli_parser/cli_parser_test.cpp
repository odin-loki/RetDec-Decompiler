/**
 * @file tests/cli_parser/cli_parser_test.cpp
 * @brief Unit tests for the .NET CLI parser.
 *
 * Tests are organized into suites matching the module architecture:
 *
 *  PeReaderTest          — DOS/PE header parsing, RVA resolution
 *  CliHeapsTest          — Compressed int decoding, heap reads
 *  MetadataTablesTest    — #~ stream parsing, typed row access, coded tokens
 *  CliSigDecoderTest     — Type/method/local-var signature decoding
 *  CILLifterTest         — CIL header parsing, instruction decode, CFG build
 *  CLIReaderTest         — End-to-end PE → BcModule (synthetic PE)
 */

#include "retdec/cli_parser/cil_lifter.h"
#include "retdec/cli_parser/cli_heaps.h"
#include "retdec/cli_parser/cli_reader.h"
#include "retdec/cli_parser/cli_sig.h"
#include "retdec/cli_parser/cli_tables.h"
#include "retdec/cli_parser/pe_reader.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

using namespace retdec::cli_parser;
using namespace retdec::bc_module;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void writeU8 (std::vector<uint8_t>& v, uint8_t x)  { v.push_back(x); }
static void writeU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
static void writeU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF); }
static void writeU64(std::vector<uint8_t>& v, uint64_t x) {
    writeU32(v, static_cast<uint32_t>(x));
    writeU32(v, static_cast<uint32_t>(x >> 32)); }
static void writeStr(std::vector<uint8_t>& v, const char* s) {
    while (*s) v.push_back(static_cast<uint8_t>(*s++));
    v.push_back(0); }
static void padTo4(std::vector<uint8_t>& v) {
    while (v.size() % 4) v.push_back(0); }

// ─── PeReaderTest ─────────────────────────────────────────────────────────────

TEST(PeReaderTest, RejectsEmptyBuffer) {
    PeReader pe;
    EXPECT_FALSE(pe.open(nullptr, 0));
}

TEST(PeReaderTest, RejectsNonPE) {
    std::vector<uint8_t> buf(128, 0);
    PeReader pe;
    EXPECT_FALSE(pe.open(buf.data(), buf.size()));
}

TEST(PeReaderTest, RejectsMissingMZSignature) {
    std::vector<uint8_t> buf(256, 0);
    // No MZ header
    PeReader pe;
    EXPECT_FALSE(pe.open(buf.data(), buf.size()));
}

TEST(PeReaderTest, DetectsNonCLIAssembly) {
    // Build a minimal PE with no CLI directory
    std::vector<uint8_t> buf(512, 0);
    // MZ signature
    buf[0] = 'M'; buf[1] = 'Z';
    // PE offset at 0x3C
    buf[0x3C] = 0x40;
    // PE signature
    buf[0x40] = 'P'; buf[0x41] = 'E'; buf[0x42] = 0; buf[0x43] = 0;
    // COFF header: Machine (i386), 0 sections, OptHdrSize = 96 (PE32)
    buf[0x44] = 0x4C; buf[0x45] = 0x01;  // Machine = i386
    buf[0x46] = 0; buf[0x47] = 0;         // NumberOfSections = 0
    buf[0x50] = 0; buf[0x51] = 0;         // OptHdrSize = 0
    // No optional header — just test that parsing doesn't crash
    PeReader pe;
    // This may fail due to lack of optional header, which is expected
    // The key test is it doesn't crash
    (void)pe.open(buf.data(), buf.size());
}

TEST(PeReaderTest, RvaToOffsetNoSections) {
    PeReader pe;
    // With no sections, any RVA should return 0
    EXPECT_EQ(0u, pe.rvaToOffset(0x1000));
}

// Build a .NET PE that reaches PeReader::parseMetadataRoot() carrying a chosen
// VersionLength field and version bytes.  Everything ahead of the metadata root
// is the minimum the reader insists on: a DOS stub, the PE signature, a COFF
// header, a PE32 optional header whose COM descriptor directory points at a CLI
// header, and one section mapping RVA 0x2000 onto file offset 0x200.
//
// `tail` is appended after the version bytes.  Four zero bytes there are a
// well-formed Flags + NumberOfStreams pair; an empty tail ends the file flush
// against the version bytes, leaving no terminator for a scan to find.
static std::vector<uint8_t> buildNetPEWithVersion(
        uint32_t versionLengthField,
        const std::vector<uint8_t>& versionBytes,
        const std::vector<uint8_t>& tail) {
    constexpr size_t   kPeOff      = 0x80;
    constexpr size_t   kOptOff     = kPeOff + 4 + 20;
    constexpr size_t   kOptSize    = 224;
    constexpr size_t   kSectOff    = kOptOff + kOptSize;
    constexpr size_t   kSectionRaw = 0x200;
    constexpr uint32_t kSectionRva = 0x2000;
    constexpr uint32_t kCliRva     = kSectionRva;
    constexpr uint32_t kMdRva      = kSectionRva + 72;

    std::vector<uint8_t> buf(kSectionRaw, 0);
    auto put16 = [&buf](size_t off, uint16_t v) {
        buf[off] = v & 0xFF; buf[off + 1] = (v >> 8) & 0xFF;
    };
    auto put32 = [&buf](size_t off, uint32_t v) {
        buf[off + 0] = v & 0xFF;         buf[off + 1] = (v >> 8) & 0xFF;
        buf[off + 2] = (v >> 16) & 0xFF; buf[off + 3] = (v >> 24) & 0xFF;
    };

    buf[0] = 'M'; buf[1] = 'Z';
    put32(0x3C, kPeOff);
    put32(kPeOff, 0x00004550u);              // "PE\0\0"

    // COFF FileHeader
    put16(kPeOff + 4 + 0,  0x014C);          // Machine = i386
    put16(kPeOff + 4 + 2,  1);               // NumberOfSections
    put16(kPeOff + 4 + 16, kOptSize);        // SizeOfOptionalHeader

    // Optional header (PE32) + COM descriptor data directory (index 14)
    put16(kOptOff, 0x010B);
    const size_t comOff = kOptOff + 96 + 14 * 8;
    put32(comOff,     kCliRva);
    put32(comOff + 4, 72);                   // >= 72 marks the image managed

    // Section header: RVA 0x2000 -> file offset 0x200
    std::memcpy(&buf[kSectOff], ".text\0\0", 7);
    put32(kSectOff + 12, kSectionRva);
    put32(kSectOff + 20, static_cast<uint32_t>(kSectionRaw));

    // CLI header (72 bytes) at file offset 0x200
    std::vector<uint8_t> body(72, 0);
    auto bput32 = [&body](size_t off, uint32_t v) {
        body[off + 0] = v & 0xFF;         body[off + 1] = (v >> 8) & 0xFF;
        body[off + 2] = (v >> 16) & 0xFF; body[off + 3] = (v >> 24) & 0xFF;
    };
    bput32(0, 72);                           // cb
    body[4] = 2; body[6] = 5;                // runtime version 2.5
    bput32(8,  kMdRva);                      // MetaData.rva
    bput32(12, 0);                           // MetaData.size (span left unbuilt)

    // Metadata root: BSJB, versions, reserved, VersionLength, version bytes
    std::vector<uint8_t> md;
    const char sig[4] = {'B', 'S', 'J', 'B'};
    md.insert(md.end(), sig, sig + 4);
    writeU16(md, 1); writeU16(md, 1);        // Major/MinorVersion
    writeU32(md, 0);                         // Reserved
    writeU32(md, versionLengthField);
    md.insert(md.end(), versionBytes.begin(), versionBytes.end());
    md.insert(md.end(), tail.begin(), tail.end());

    body.insert(body.end(), md.begin(), md.end());
    buf.insert(buf.end(), body.begin(), body.end());

    // The section must cover what we appended, or rvaToOffset refuses the RVAs.
    put32(kSectOff + 16, static_cast<uint32_t>(buf.size() - kSectionRaw));
    return buf;
}

TEST(PeReaderTest, MetadataVersionLengthPaddingDoesNotWrap) {
    // VersionLength is rounded up to a 4-byte boundary.  Rounded in uint32,
    // 0xFFFFFFFD + 3 wraps to 0, so the largest length the file can name
    // arrives at the range check disguised as the smallest one and sails
    // through it.  Rounding in 64 bits makes the overflow visible instead.
    //
    // The four zero bytes of tail are what makes this test load-bearing: with
    // the wrap in place the truncated length parses cleanly to the end and
    // open() *succeeds*, so nothing but the range rejection distinguishes the
    // two versions.
    auto buf = buildNetPEWithVersion(0xFFFFFFFDu, {'v', '4', '.', '0'},
                                     {0, 0, 0, 0});
    PeReader pe;
    EXPECT_FALSE(pe.open(buf.data(), buf.size()));
    EXPECT_NE(std::string::npos, pe.error().find("out of range"));
}

TEST(PeReaderTest, MetadataVersionStringNotScannedPastDeclaredLength) {
    // The version string is NUL-terminated within VersionLength bytes only if
    // the file says so.  Here it is not: eight version bytes with no
    // terminator, and the file ends flush against them.  strlen() runs off the
    // end of the buffer looking for one; taking std::min afterwards is too
    // late, the read has already happened.  Bounded scanning stops at the
    // declared length.
    //
    // Both versions reject this image -- the header past the version string is
    // missing either way -- so the over-read is the whole difference.  Build
    // with EXTRA_CXXFLAGS="-fsanitize=address -g" to see it: unfixed, this is
    // a heap-buffer-overflow inside strlen.
    auto buf = buildNetPEWithVersion(
        8u, {'v', '4', '.', '0', '.', '3', '0', '3'}, {});
    PeReader pe;
    EXPECT_FALSE(pe.open(buf.data(), buf.size()));
    EXPECT_NE(std::string::npos, pe.error().find("stream count truncated"));
}

TEST(PeReaderTest, MetadataVersionStringStopsAtEmbeddedNul) {
    // The ordinary case still has to work: a terminator inside the declared
    // length ends the string, and the padding bytes after it are not part of
    // it.
    auto buf = buildNetPEWithVersion(
        12u, {'v', '4', '.', '0', '.', '3', '0', '3', '1', '9', 0, 0},
        {0, 0, 0, 0});
    PeReader pe;
    ASSERT_TRUE(pe.open(buf.data(), buf.size())) << pe.error();
    EXPECT_EQ("v4.0.30319", pe.clrVersion());
}

// ─── CliHeapsTest ─────────────────────────────────────────────────────────────

TEST(CliHeapsTest, CompressedUIntOneByte) {
    std::vector<uint8_t> blob = {0x03};
    size_t pos = 0;
    auto val = decodeCompressedUInt({blob.data(), blob.size()}, pos);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(3u, *val);
    EXPECT_EQ(1u, pos);
}

TEST(CliHeapsTest, CompressedUIntTwoByte) {
    std::vector<uint8_t> blob = {0x81, 0x05};
    size_t pos = 0;
    auto val = decodeCompressedUInt({blob.data(), blob.size()}, pos);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(0x0105u, *val);
    EXPECT_EQ(2u, pos);
}

TEST(CliHeapsTest, CompressedUIntFourByte) {
    std::vector<uint8_t> blob = {0xC0, 0x00, 0x40, 0x00};
    size_t pos = 0;
    auto val = decodeCompressedUInt({blob.data(), blob.size()}, pos);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(4u, pos);
}

TEST(CliHeapsTest, CompressedUIntEmpty) {
    std::vector<uint8_t> blob;
    size_t pos = 0;
    auto val = decodeCompressedUInt({blob.data(), blob.size()}, pos);
    EXPECT_FALSE(val.has_value());
}

TEST(CliHeapsTest, CompressedIntPositive) {
    // Value 3 encoded as 6 (positive, sign bit = 0) → but §II.23.2.6
    // actually encodes as 3*2 = 6 since positive, stored as 0x06
    std::vector<uint8_t> blob = {0x06};
    size_t pos = 0;
    auto val = decodeCompressedInt({blob.data(), blob.size()}, pos);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(3, *val);
}

TEST(CliHeapsTest, StringsHeapEmpty) {
    StringsHeap h({});
    EXPECT_EQ("", h.get(0));
    EXPECT_TRUE(h.empty());
}

TEST(CliHeapsTest, StringsHeapRead) {
    const char* data = "\0Hello\0World\0";
    std::span<const uint8_t> sp{
        reinterpret_cast<const uint8_t*>(data),
        static_cast<size_t>(13)};
    StringsHeap h(sp);
    EXPECT_EQ("", h.get(0));
    EXPECT_EQ("Hello", h.get(1));
    EXPECT_EQ("World", h.get(7));
}

TEST(CliHeapsTest, BlobHeapRead) {
    std::vector<uint8_t> data = {
        0x03, 0xAA, 0xBB, 0xCC,   // blob at offset 0: length=3, bytes={AA,BB,CC}
        0x01, 0xFF                  // blob at offset 4: length=1, bytes={FF}
    };
    BlobHeap h({data.data(), data.size()});
    auto b1 = h.get(0);
    ASSERT_EQ(3u, b1.size());
    EXPECT_EQ(0xAA, b1[0]);
    EXPECT_EQ(0xBB, b1[1]);
    EXPECT_EQ(0xCC, b1[2]);

    auto b2 = h.get(4);
    ASSERT_EQ(1u, b2.size());
    EXPECT_EQ(0xFF, b2[0]);
}

TEST(CliHeapsTest, GuidHeapRead) {
    std::vector<uint8_t> data(32, 0);
    // GUID 1: first 16 bytes
    for (int i = 0; i < 16; ++i) data[i] = static_cast<uint8_t>(i);
    GuidHeap h({data.data(), data.size()});

    Guid g1 = h.get(1);  // 1-based
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(static_cast<uint8_t>(i), g1.bytes[i]);

    Guid g0 = h.get(0);  // Index 0 = all zeros
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(0, g0.bytes[i]);
}

TEST(CliHeapsTest, GuidToString) {
    Guid g{};
    // {00010203-0405-0607-0809-0A0B0C0D0E0F}
    for (int i = 0; i < 16; ++i) g.bytes[i] = static_cast<uint8_t>(i);
    std::string s = g.toString();
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(36u, s.size());  // 8-4-4-4-12 with dashes
}

TEST(CliHeapsTest, UserStringsHeapAscii) {
    // Encode "Hi" as UTF-16LE: [0x48,0x00, 0x69,0x00] + trailing byte 0x00
    // Blob length = 5 (4 chars bytes + 1 trailing)
    std::vector<uint8_t> data = {0x05, 0x48, 0x00, 0x69, 0x00, 0x00};
    UserStringsHeap h({data.data(), data.size()});
    EXPECT_EQ("Hi", h.get(0));
}

// ─── MetadataTablesTest ───────────────────────────────────────────────────────

// Build a minimal valid #~ stream for testing
static std::vector<uint8_t> buildMinimalTildeStream() {
    std::vector<uint8_t> v;

    // Header
    writeU32(v, 0);          // Reserved
    writeU8(v, 2);           // MajorVersion
    writeU8(v, 0);           // MinorVersion
    writeU8(v, 0);           // HeapSizes (all 2-byte)
    writeU8(v, 1);           // Reserved2
    // Valid bitmask: only Module (bit 0) and TypeRef (bit 1)
    writeU64(v, 0x0000000000000003ULL);
    // Sorted bitmask
    writeU64(v, 0);

    // Row counts: Module=1, TypeRef=2
    writeU32(v, 1);  // Module
    writeU32(v, 2);  // TypeRef

    // Module row: Generation(2) + Name(2) + MvId(2) + EncId(2) + EncBase(2)
    writeU16(v, 0);     // Generation
    writeU16(v, 1);     // Name → offset 1 in #Strings
    writeU16(v, 1);     // MvId → GUID index 1
    writeU16(v, 0);     // EncId
    writeU16(v, 0);     // EncBaseId

    // TypeRef row 1: ResolutionScope coded(2) + Name(2) + Namespace(2)
    // ResolutionScope: Module(0) << 2 | 0 = 0x0000 (table=Module, idx=0)
    writeU16(v, 0x0000);  // ResolutionScope
    writeU16(v, 5);       // Name
    writeU16(v, 11);      // Namespace

    // TypeRef row 2
    writeU16(v, 0x0000);
    writeU16(v, 5);
    writeU16(v, 11);

    return v;
}

TEST(MetadataTablesTest, ParseMinimalTildeStream) {
    auto tilde = buildMinimalTildeStream();

    // Build minimal heaps
    const char strData[] = "\0test\0System\0";
    std::span<const uint8_t> strSpan{
        reinterpret_cast<const uint8_t*>(strData), sizeof(strData)};

    CliHeaps heaps(strSpan, {}, {}, {}, 0);

    MetadataTables tables;
    EXPECT_TRUE(tables.parse({tilde.data(), tilde.size()}, heaps));
    EXPECT_TRUE(tables.isValid());
    EXPECT_EQ(1u, tables.rowCount(TableId::Module));
    EXPECT_EQ(2u, tables.rowCount(TableId::TypeRef));
    EXPECT_EQ(0u, tables.rowCount(TableId::TypeDef));
}

TEST(MetadataTablesTest, ModuleRowAccess) {
    auto tilde = buildMinimalTildeStream();
    const char strData[] = "\0test\0System\0";
    std::span<const uint8_t> strSpan{
        reinterpret_cast<const uint8_t*>(strData), sizeof(strData)};
    CliHeaps heaps(strSpan, {}, {}, {}, 0);

    MetadataTables tables;
    tables.parse({tilde.data(), tilde.size()}, heaps);

    auto mod = tables.module(1);
    EXPECT_EQ(0u, mod.generation);
    EXPECT_EQ(1u, mod.name);  // #Strings offset
}

TEST(MetadataTablesTest, TypeRefRowAccess) {
    auto tilde = buildMinimalTildeStream();
    const char strData[] = "\0test\0System\0";
    std::span<const uint8_t> strSpan{
        reinterpret_cast<const uint8_t*>(strData), sizeof(strData)};
    CliHeaps heaps(strSpan, {}, {}, {}, 0);

    MetadataTables tables;
    tables.parse({tilde.data(), tilde.size()}, heaps);

    auto tr = tables.typeRef(1);
    EXPECT_EQ(5u, tr.name);
    EXPECT_EQ(11u, tr.ns);
}

TEST(MetadataTablesTest, CodedTokenTypeDefOrRef) {
    MetadataTables tables;
    // TypeDef coded: tag=0 → TypeDef
    auto tok = tables.decodeTypeDefOrRef(0x0008);  // idx=2, tag=0 → TypeDef, idx=2
    EXPECT_EQ(static_cast<uint8_t>(TableId::TypeDef), tok.table);
    EXPECT_EQ(2u, tok.index);

    // TypeRef coded: tag=1 → TypeRef
    tok = tables.decodeTypeDefOrRef(0x0009);  // idx=2, tag=1 → TypeRef, idx=2
    EXPECT_EQ(static_cast<uint8_t>(TableId::TypeRef), tok.table);
    EXPECT_EQ(2u, tok.index);
}

TEST(MetadataTablesTest, CodedTokenResolutionScope) {
    MetadataTables tables;
    // ResolutionScope: 2 bits, Module=0, ModuleRef=1, AssemblyRef=2, TypeRef=3
    auto tok = tables.decodeResolutionScope(0x000C);  // idx=3, tag=0 → Module
    EXPECT_EQ(static_cast<uint8_t>(TableId::Module), tok.table);
    EXPECT_EQ(3u, tok.index);

    tok = tables.decodeResolutionScope(0x000D);  // idx=3, tag=1 → ModuleRef
    EXPECT_EQ(static_cast<uint8_t>(TableId::ModuleRef), tok.table);
    EXPECT_EQ(3u, tok.index);
}

TEST(MetadataTablesTest, EmptyStreamFails) {
    MetadataTables tables;
    CliHeaps heaps({}, {}, {}, {}, 0);
    EXPECT_FALSE(tables.parse({}, heaps));
}

// A #~ stream whose header is well formed but whose declared row counts are
// larger than the stream could ever supply. The row counts are attacker data;
// before RowReader carried a size, the row loop read straight off the end of
// the stream and kept going for as many rows as the header claimed.
static std::vector<uint8_t> buildTildeStream(
        uint64_t validMask,
        const std::vector<uint32_t>& rowCounts,
        const std::vector<uint8_t>& rowBytes) {
    std::vector<uint8_t> v;
    writeU32(v, 0);          // Reserved
    writeU8(v, 2);           // MajorVersion
    writeU8(v, 0);           // MinorVersion
    writeU8(v, 0);           // HeapSizes (all 2-byte)
    writeU8(v, 1);           // Reserved2
    writeU64(v, validMask);  // Valid
    writeU64(v, 0);          // Sorted
    for (uint32_t rc : rowCounts) writeU32(v, rc);
    v.insert(v.end(), rowBytes.begin(), rowBytes.end());
    return v;
}

TEST(MetadataTablesTest, RowCountBeyondStreamRejected) {
    // Module declares 0xFFFFFFFF rows; the stream carries none.
    auto tilde = buildTildeStream(0x1ULL, {0xFFFFFFFFu}, {});
    CliHeaps heaps({}, {}, {}, {}, 0);
    MetadataTables tables;
    EXPECT_FALSE(tables.parse({tilde.data(), tilde.size()}, heaps));
    EXPECT_FALSE(tables.isValid());
}

TEST(MetadataTablesTest, RowCountOverflowingRowSizeProductRejected) {
    // 0x10000000 rows × the 48-byte storage row is exactly 3·2^32, so the
    // uint32 product used to size the row buffer wrapped to zero: resize(0)
    // followed by writes through the row pointer.
    auto tilde = buildTildeStream(0x1ULL, {0x10000000u}, {});
    CliHeaps heaps({}, {}, {}, {}, 0);
    MetadataTables tables;
    EXPECT_FALSE(tables.parse({tilde.data(), tilde.size()}, heaps));
}

TEST(MetadataTablesTest, PartialFinalRowRejected) {
    // Module rows are 10 bytes with narrow heaps. Declare two, supply 15.
    std::vector<uint8_t> rows(15, 0);
    auto tilde = buildTildeStream(0x1ULL, {2u}, rows);
    CliHeaps heaps({}, {}, {}, {}, 0);
    MetadataTables tables;
    EXPECT_FALSE(tables.parse({tilde.data(), tilde.size()}, heaps));
}

TEST(MetadataTablesTest, SecondTableRowCountBoundedByRemainingBytes) {
    // Module (1 row, 10 bytes) is satisfied; TypeRef then claims 0x40000000
    // rows out of the 8 bytes left. The bound has to be against what is left
    // after Module, not against the whole stream.
    std::vector<uint8_t> rows(18, 0);
    auto tilde = buildTildeStream(0x3ULL, {1u, 0x40000000u}, rows);
    CliHeaps heaps({}, {}, {}, {}, 0);
    MetadataTables tables;
    EXPECT_FALSE(tables.parse({tilde.data(), tilde.size()}, heaps));
}

TEST(MetadataTablesTest, ExactlySizedStreamStillParses) {
    // The bound must not reject a stream that supplies exactly the declared
    // rows and not one byte more.
    auto tilde = buildMinimalTildeStream();
    const char strData[] = "\0test\0System\0";
    std::span<const uint8_t> strSpan{
        reinterpret_cast<const uint8_t*>(strData), sizeof(strData)};
    CliHeaps heaps(strSpan, {}, {}, {}, 0);
    MetadataTables tables;
    ASSERT_TRUE(tables.parse({tilde.data(), tilde.size()}, heaps));
    EXPECT_EQ(1u, tables.rowCount(TableId::Module));
    EXPECT_EQ(2u, tables.rowCount(TableId::TypeRef));
    EXPECT_EQ(1u, tables.module(1).name);
    EXPECT_EQ(5u, tables.typeRef(2).name);
}

// ─── previously undecoded tables ─────────────────────────────────────────────
//
// Metadata rows are laid out end to end with no length prefix and no padding,
// so a table whose width the decoder does not know hides where the *next*
// table starts. Six tables in the standard set fell through decodeRow's
// default arm on the stated grounds that they "are always empty in practice"
// -- DeclSecurity and FieldLayout among them, which any signed assembly and
// any explicit-layout struct carries. Each consumed no bytes, so every table
// after it decoded at the wrong offset, and the zero width also handed each of
// them the whole remaining stream as a row count to zero-fill at 48 bytes a
// row.

TEST(MetadataTablesTest, AllSixFormerlyUndecodedTablesKeepTheStreamAligned) {
    std::vector<uint8_t> rows;
    // DeclSecurity: Action(2) + Parent coded HasDeclSecurity(2) + Blob(2)
    writeU16(rows, 0x0002); writeU16(rows, 0x0000); writeU16(rows, 0x0000);
    // FieldLayout: Offset(4) + Field(2)
    writeU32(rows, 0x11223344); writeU16(rows, 0x0000);
    // AssemblyProcessor: Processor(4)
    writeU32(rows, 0x00000006);
    // AssemblyOS: PlatformID(4) + Major(4) + Minor(4)
    writeU32(rows, 2); writeU32(rows, 6); writeU32(rows, 1);
    // AssemblyRefProcessor: Processor(4) + AssemblyRef(2)
    writeU32(rows, 0x00000006); writeU16(rows, 0x0000);
    // AssemblyRefOS: PlatformID(4) + Major(4) + Minor(4) + AssemblyRef(2)
    writeU32(rows, 2); writeU32(rows, 6); writeU32(rows, 1); writeU16(rows, 0);
    // NestedClass: NestedClass(2) + EnclosingClass(2) -- the marker.  It sits
    // after all six, so it lands on its own bytes only if every one of them
    // consumed exactly the width it declares.
    writeU16(rows, 0x0041); writeU16(rows, 0x0042);

    const uint64_t valid =
        (1ULL << 0x0E) | (1ULL << 0x10) | (1ULL << 0x21) | (1ULL << 0x22) |
        (1ULL << 0x24) | (1ULL << 0x25) | (1ULL << 0x29);
    auto tilde = buildTildeStream(valid, {1, 1, 1, 1, 1, 1, 1}, rows);

    CliHeaps heaps({}, {}, {}, {}, 0);
    MetadataTables tables;
    ASSERT_TRUE(tables.parse({tilde.data(), tilde.size()}, heaps))
        << tables.error();
    EXPECT_EQ(1u, tables.rowCount(TableId::DeclSecurity));
    EXPECT_EQ(1u, tables.rowCount(TableId::FieldLayout));
    EXPECT_EQ(1u, tables.rowCount(TableId::AssemblyRefOS));

    auto nc = tables.nestedClass(1);
    EXPECT_EQ(0x41u, nc.nestedClass);
    EXPECT_EQ(0x42u, nc.enclosingClass);
}

TEST(MetadataTablesTest, ATableWithNoLayoutIsRejectedNotZeroFilled) {
    // Bit 0x03 is FieldPtr, one of the uncompressed-metadata tables a `#~`
    // stream does not carry and this decoder has no layout for.  Because such
    // a table consumes no bytes, the row-count bound was evaluated at one byte
    // per row and handed it the whole remaining stream: 900 rows here, each
    // zero-filled to 48 bytes, from 900 bytes of input -- and the rows would
    // have been meaningless anyway, since nothing knows where they end.
    std::vector<uint8_t> rows(900, 0);
    auto tilde = buildTildeStream(1ULL << 0x03, {900}, rows);

    CliHeaps heaps({}, {}, {}, {}, 0);
    MetadataTables tables;
    EXPECT_FALSE(tables.parse({tilde.data(), tilde.size()}, heaps));
    EXPECT_FALSE(tables.isValid());
    EXPECT_NE(std::string::npos, tables.error().find("cannot lay out"));
}

TEST(MetadataTablesTest, AFailedParseLeavesNoRowsBehind) {
    // Row counts are copied for all 45 tables before any of them is parsed, so
    // a parse that fails partway used to leave the new counts standing over
    // whatever the previous file had allocated.  rowFields() gated on the
    // count alone, so the typed accessors read one against the other.
    const char strData[] = "\0test\0System\0";
    std::span<const uint8_t> strSpan{
        reinterpret_cast<const uint8_t*>(strData), sizeof(strData)};
    CliHeaps heaps(strSpan, {}, {}, {}, 0);

    MetadataTables tables;
    auto good = buildMinimalTildeStream();
    ASSERT_TRUE(tables.parse({good.data(), good.size()}, heaps));
    ASSERT_EQ(2u, tables.rowCount(TableId::TypeRef));

    // Now a stream that fails: TypeRef declares far more rows than it carries.
    auto bad = buildTildeStream(0x3ULL, {1, 0x00FFFFFFu}, {});
    EXPECT_FALSE(tables.parse({bad.data(), bad.size()}, heaps));
    EXPECT_FALSE(tables.isValid());
    for (int t = 0; t < static_cast<int>(TableId::_Count); ++t)
        EXPECT_EQ(0u, tables.rowCount(static_cast<TableId>(t)))
            << "table 0x" << std::hex << t << " kept its row count";
    EXPECT_EQ(0u, tables.module(1).name);
    EXPECT_EQ(0u, tables.typeRef(1).name);
}

// ─── CliSigDecoderTest ────────────────────────────────────────────────────────

TEST(CliSigDecoderTest, DecodeVoidField) {
    // FieldSig: 0x06 [ELEMENT_TYPE_VOID]
    std::vector<uint8_t> blob = {0x06, 0x01};
    CliSigDecoder dec;
    auto ct = dec.decodeField({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    EXPECT_TRUE(ct->base.isVoid());
}

TEST(CliSigDecoderTest, DecodeI4Field) {
    std::vector<uint8_t> blob = {0x06, 0x08};  // FieldSig + ELEMENT_TYPE_I4
    CliSigDecoder dec;
    auto ct = dec.decodeField({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    EXPECT_FALSE(ct->base.isVoid());
}

TEST(CliSigDecoderTest, DecodeStringField) {
    std::vector<uint8_t> blob = {0x06, 0x0E};  // FieldSig + ELEMENT_TYPE_STRING
    CliSigDecoder dec;
    auto ct = dec.decodeField({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
}

TEST(CliSigDecoderTest, DecodeMethodSignature) {
    // MethodSig: default calling convention (0x00), param count = 0, ret = void (0x01)
    std::vector<uint8_t> blob = {0x00, 0x00, 0x01};
    CliSigDecoder dec;
    auto sig = dec.decodeMethod({blob.data(), blob.size()});
    ASSERT_TRUE(sig.has_value());
    EXPECT_EQ(CallingConvention::Default, sig->callingConv);
    EXPECT_EQ(0u, sig->params.size());
    EXPECT_TRUE(sig->retType.base.isVoid());
}

TEST(CliSigDecoderTest, DecodeMethodWithParams) {
    // MethodSig: default (0x00), paramCount=2, ret=void (0x01), param1=I4 (0x08), param2=R8 (0x0D)
    std::vector<uint8_t> blob = {0x00, 0x02, 0x01, 0x08, 0x0D};
    CliSigDecoder dec;
    auto sig = dec.decodeMethod({blob.data(), blob.size()});
    ASSERT_TRUE(sig.has_value());
    EXPECT_EQ(2u, sig->params.size());
    EXPECT_TRUE(sig->retType.base.isVoid());
}

TEST(CliSigDecoderTest, DecodeHasThis) {
    // MethodSig: HasThis (0x20), paramCount=0, ret=void
    std::vector<uint8_t> blob = {0x20, 0x00, 0x01};
    CliSigDecoder dec;
    auto sig = dec.decodeMethod({blob.data(), blob.size()});
    ASSERT_TRUE(sig.has_value());
    EXPECT_TRUE(sig->hasThis);
}

TEST(CliSigDecoderTest, DecodeSzArray) {
    // FieldSig: 0x06 + SZARRAY (0x1D) + I4 (0x08)
    std::vector<uint8_t> blob = {0x06, 0x1D, 0x08};
    CliSigDecoder dec;
    auto ct = dec.decodeField({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    // Should be array type
    EXPECT_TRUE(ct->base.isArray());
}

TEST(CliSigDecoderTest, DecodeByRef) {
    // FieldSig: 0x06 + BYREF (0x10) + I4 (0x08)
    std::vector<uint8_t> blob = {0x06, 0x10, 0x08};
    CliSigDecoder dec;
    auto ct = dec.decodeField({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
    EXPECT_TRUE(ct->byRef);
}

TEST(CliSigDecoderTest, DecodeLocalVarSig) {
    // LocalVarSig: 0x07 + count=2 + I4 + String
    std::vector<uint8_t> blob = {0x07, 0x02, 0x08, 0x0E};
    CliSigDecoder dec;
    auto lv = dec.decodeLocalVar({blob.data(), blob.size()});
    ASSERT_TRUE(lv.has_value());
    EXPECT_EQ(2u, lv->locals.size());
}

TEST(CliSigDecoderTest, DecodeTypeSpec) {
    // TypeSpec: just a type (I4)
    std::vector<uint8_t> blob = {0x08};  // ELEMENT_TYPE_I4
    CliSigDecoder dec;
    auto ct = dec.decodeTypeSpec({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
}

TEST(CliSigDecoderTest, ClrNameToTypePrimitives) {
    auto t = CliSigDecoder::clrNameToType("System.Int32");
    EXPECT_FALSE(t.isVoid());

    auto tv = CliSigDecoder::clrNameToType("System.Void");
    EXPECT_TRUE(tv.isVoid());
}

TEST(CliSigDecoderTest, ToBcFuncTypeVoidReturn) {
    std::vector<uint8_t> blob = {0x00, 0x00, 0x01};
    CliSigDecoder dec;
    auto sig = dec.decodeMethod({blob.data(), blob.size()});
    ASSERT_TRUE(sig.has_value());
    BcFuncType ft = dec.toBcFuncType(*sig);
    EXPECT_EQ(nullptr, ft.returnType);
    EXPECT_TRUE(ft.params.empty());
}

TEST(CliSigDecoderTest, GenericInstType) {
    // GENERICINST CLASS TypeDefOrRef<I4>:
    // 0x15 CLASS 0x12 [compressed TypeDefOrRef = 4 = TypeDef<<Module>=1)] 0x01 [count=1] 0x08 [I4]
    // Actually we encode TypeDefOrRef as TypeDef#1 = compressed uint = (1<<2)|0x0 = 0x04
    std::vector<uint8_t> blob = {0x06, 0x15, 0x12, 0x04, 0x01, 0x08};
    CliSigDecoder dec;
    auto ct = dec.decodeField({blob.data(), blob.size()});
    ASSERT_TRUE(ct.has_value());
}

// ─── CILLifterTest ────────────────────────────────────────────────────────────

TEST(CILLifterTest, TinyHeader) {
    // Tiny: low 2 bits = 0x2, high 6 bits = code size (e.g. 1 byte)
    // Code: just a ret (0x2A)
    std::vector<uint8_t> body = {
        0x02 | (1 << 2),  // Tiny header: codeSize=1
        0x2A              // ret
    };
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_TRUE(hdr.isTiny);
    EXPECT_EQ(1u, hdr.codeSize);
    EXPECT_EQ(8u, hdr.maxStack);
}

// A method body that ends in the middle of an instruction's operand. Only the
// opcode byte was ever bounds-checked, so an operand read ran off the end of
// the code span -- and the code span ends where the body does, so it ran off
// the end of the caller's buffer too.
static std::vector<uint8_t> tinyBody(const std::vector<uint8_t>& code) {
    std::vector<uint8_t> body;
    body.push_back(0x02 | (static_cast<uint8_t>(code.size()) << 2));
    body.insert(body.end(), code.begin(), code.end());
    return body;
}

TEST(CILLifterTest, TruncatedInlineOperandDoesNotReadPastCode) {
    // ldc.i4 (0x20) wants a 4-byte operand; the body ends after the opcode.
    auto body = tinyBody({0x20});
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_EQ(0u, cfg.blockCount());
}

TEST(CILLifterTest, TruncatedShortOperandDoesNotReadPastCode) {
    // ldc.i4.s (0x1F) wants one operand byte; supply none.
    auto body = tinyBody({0x1F});
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_EQ(0u, cfg.blockCount());
}

TEST(CILLifterTest, TruncatedTokenOperandDoesNotReadPastCode) {
    // call (0x28) wants a 4-byte token; supply three bytes of it.
    auto body = tinyBody({0x28, 0x01, 0x02, 0x03});
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_EQ(0u, cfg.blockCount());
}

TEST(CILLifterTest, JmpConsumesItsMethodToken) {
    // jmp (0x27) carries a 4-byte InlineMethod token.  It appeared in neither
    // the no-operand list nor the token list, so it fell to the default arm and
    // its operand was never consumed: the four token bytes were decoded as the
    // next four instructions and the decoder stayed out of step for the rest of
    // the method.  Here the token bytes are 0x2A 0x00 0x00 0x00 -- the first of
    // them is `ret`, so unconsumed they end the method one instruction early
    // and hide the `nop` that really follows.
    //   jmp <token 0x0000002A> ; nop ; ret
    auto body = tinyBody({0x27, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x2A});
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    ASSERT_FALSE(cfg.blocks().empty());
    const auto& instrs = cfg.blocks()[0].instrs;
    ASSERT_EQ(3u, instrs.size());
    EXPECT_EQ(BcOpcode::DOTNET_NOP, instrs[1].opcode);
    EXPECT_EQ(BcOpcode::DOTNET_RET, instrs[2].opcode);
}

TEST(CILLifterTest, TruncatedBranchOperandDoesNotReadPastCode) {
    // br.s (0x2B) wants a 1-byte displacement; supply none.
    auto body = tinyBody({0x2B});
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_EQ(0u, cfg.blockCount());
}

TEST(CILLifterTest, TruncatedTwoByteOperandDoesNotReadPastCode) {
    // 0xFE 0x09 = ldarg <uint16>; the operand's second byte is missing.
    auto body = tinyBody({0xFE, 0x09, 0x00});
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_EQ(0u, cfg.blockCount());
}

TEST(CILLifterTest, CompleteOperandsStillDecode) {
    // The guard must not reject a body whose last instruction is complete.
    auto body = tinyBody({
        0x20, 0x2A, 0x00, 0x00, 0x00,  // ldc.i4 42
        0x1F, 0x07,                    // ldc.i4.s 7
        0x2A                           // ret
    });
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    ASSERT_FALSE(cfg.blocks().empty());
    const auto& blk = cfg.blocks()[0];
    ASSERT_EQ(3u, blk.instrs.size());
    EXPECT_EQ(BcOpcode::DOTNET_RET, blk.instrs[2].opcode);
}

TEST(CILLifterTest, FatHeaderBasic) {
    // Fat header: Flags=0x3003 (fat+initLocals), MaxStack=8, CodeSize=1, LocalVarSigTok=0
    // Code: ret (0x2A)
    std::vector<uint8_t> body = {
        0x13, 0x30,  // Flags low/high: size=3 dwords, fat bit set, InitLocals
        0x08, 0x00,  // MaxStack = 8
        0x01, 0x00, 0x00, 0x00,  // CodeSize = 1
        0x00, 0x00, 0x00, 0x00,  // LocalVarSigTok = 0
        0x2A         // ret
    };
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_FALSE(hdr.isTiny);
    EXPECT_EQ(8u, hdr.maxStack);
    EXPECT_EQ(1u, hdr.codeSize);
    EXPECT_TRUE(hdr.initLocals);
}

TEST(CILLifterTest, NopSequence) {
    // Tiny header + 4 nops + ret
    std::vector<uint8_t> body;
    body.push_back(0x02 | (5 << 2));  // tiny, 5 bytes
    body.push_back(0x00); body.push_back(0x00);  // nop nop
    body.push_back(0x00); body.push_back(0x00);  // nop nop
    body.push_back(0x2A);  // ret

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_EQ(5u, hdr.codeSize);
    EXPECT_FALSE(cfg.blocks().empty());
}

TEST(CILLifterTest, SimpleBranch) {
    // Tiny: ldc.i4.0 (0x16) + brfalse.s (0x2C) +1 + ldc.i4.1 (0x17) + ret (0x2A)
    // brfalse.s target = 0x2C offset + 2 (next insn start) + 1 = offset 5 (ldc.i4.1)
    // Actually: brfalse.s (0x2C), operand = int8 delta
    // Instructions: [0] ldc.i4.0 at offset 0 (1 byte)
    //               [1] brfalse.s at offset 1 (2 bytes): delta = 1 → target = 4
    //               [2] ldc.i4.1 at offset 3 (1 byte)
    //               [3] ret at offset 4 (1 byte)
    std::vector<uint8_t> code = {
        0x16,       // ldc.i4.0
        0x2C, 0x01, // brfalse.s +1 (target = 3 + 1 = 4)
        0x17,       // ldc.i4.1
        0x2A        // ret
    };
    std::vector<uint8_t> body;
    body.push_back(0x02 | (static_cast<uint8_t>(code.size()) << 2));
    body.insert(body.end(), code.begin(), code.end());

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    // Should have at least 2 basic blocks (branch creates a new one)
    EXPECT_GE(cfg.blockCount(), 2u);
}

TEST(CILLifterTest, SwitchOpcode) {
    // tiny: switch with 2 targets, then ret
    // switch(n=2): 0x45, {n=2 as LE32}, {delta1=0 as LE32}, {delta2=1 as LE32}
    std::vector<uint8_t> code = {
        0x45, 0x02, 0x00, 0x00, 0x00,  // switch n=2
        0x00, 0x00, 0x00, 0x00,         // target1: delta=0
        0x01, 0x00, 0x00, 0x00,         // target2: delta=1
        0x2A                            // ret
    };
    std::vector<uint8_t> body;
    body.push_back(0x02 | (static_cast<uint8_t>(code.size()) << 2));
    body.insert(body.end(), code.begin(), code.end());

    CILLifter lifter;
    CILMethodHeader hdr;
    // Should not crash
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    EXPECT_EQ(code.size(), hdr.codeSize);
}

TEST(CILLifterTest, TwoByteOpcodes) {
    // 0xFE 0x01 = ceq, 0xFE 0x02 = cgt, 0x2A = ret
    std::vector<uint8_t> code = {0xFE, 0x01, 0xFE, 0x02, 0x2A};
    std::vector<uint8_t> body;
    body.push_back(0x02 | (static_cast<uint8_t>(code.size()) << 2));
    body.insert(body.end(), code.begin(), code.end());

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    ASSERT_FALSE(cfg.blocks().empty());
    const auto& blk = cfg.blocks()[0];
    EXPECT_EQ(BcOpcode::DOTNET_CEQ, blk.instrs[0].opcode);
    EXPECT_EQ(BcOpcode::DOTNET_CGT, blk.instrs[1].opcode);
    EXPECT_EQ(BcOpcode::DOTNET_RET, blk.instrs[2].opcode);
}

TEST(CILLifterTest, LdcI4Variants) {
    // ldc.i4.m1 (0x15) ldc.i4.0 .. ldc.i4.8, ldc.i4.s (0x1F), ldc.i4 (0x20)
    std::vector<uint8_t> code = {
        0x15,                                // ldc.i4.m1
        0x16, 0x17, 0x18, 0x19, 0x1A,       // ldc.i4.0 to ldc.i4.4
        0x1B, 0x1C, 0x1D, 0x1E,             // ldc.i4.5 to ldc.i4.8
        0x1F, 0x7F,                          // ldc.i4.s 127
        0x20, 0x01, 0x00, 0x00, 0x00,        // ldc.i4 1
        0x2A                                 // ret
    };
    std::vector<uint8_t> body;
    body.push_back(0x02 | (static_cast<uint8_t>(code.size()) << 2));
    body.insert(body.end(), code.begin(), code.end());

    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    ASSERT_FALSE(cfg.blocks().empty());
}

TEST(CILLifterTest, EmptyBody) {
    std::vector<uint8_t> body;
    CILLifter lifter;
    CILMethodHeader hdr;
    auto cfg = lifter.lift({body.data(), body.size()}, hdr);
    // Should return empty CFG without crashing
    EXPECT_EQ(0u, cfg.blockCount());
}

// ─── CLIReaderTest ────────────────────────────────────────────────────────────

// Helper: build a minimal valid .NET PE in memory
// (just enough to pass PeReader, no actual metadata)
static std::vector<uint8_t> buildMinimalNetPE() {
    // We'll build a fake PE that has:
    // - DOS header with MZ signature
    // - PE signature
    // - COFF header (0 sections, 0 opt header size)
    // This is not a valid .NET PE (no CLI dir), but tests the rejection path.
    std::vector<uint8_t> buf(256, 0);
    buf[0] = 'M'; buf[1] = 'Z';    // MZ
    buf[0x3C] = 0x40;               // PE offset
    buf[0x40] = 'P'; buf[0x41] = 'E'; buf[0x42] = 0; buf[0x43] = 0;
    buf[0x44] = 0x4C; buf[0x45] = 0x01;  // Machine = i386
    // NumberOfSections = 0, TimeDateStamp, etc. = 0
    buf[0x50] = 0; buf[0x51] = 0;  // OptHdrSize = 0
    return buf;
}

TEST(CLIReaderTest, RejectsNonPE) {
    CLIReader reader;
    std::vector<uint8_t> buf(64, 0);
    auto result = reader.read(buf.data(), buf.size(), "test");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST(CLIReaderTest, RejectsNonCLIPE) {
    CLIReader reader;
    auto buf = buildMinimalNetPE();
    // May fail at PE parsing or at CLI check
    auto result = reader.read(buf.data(), buf.size(), "test");
    // We just want it not to crash; it should fail gracefully
    // (may or may not succeed depending on parsing strictness)
    EXPECT_FALSE(result.error.empty());
}

TEST(CLIReaderTest, EmptyBufferFails) {
    CLIReader reader;
    auto result = reader.read(nullptr, 0, "empty");
    EXPECT_FALSE(result.success);
}

TEST(CLIReaderTest, TypeDefNameFallback) {
    CLIReader reader;
    // Before any assembly is loaded, typeDefName should return a placeholder
    std::string name = reader.typeDefName(0);
    EXPECT_EQ("<unknown>", name);
    name = reader.typeDefName(999);
    EXPECT_EQ("<unknown>", name);
}

// ─── Integration: compressed int round-trip ───────────────────────────────────

TEST(IntegrationTest, CompressedUIntRoundTrip) {
    // Test values that exercise each encoding tier
    std::vector<uint32_t> vals = {0, 1, 63, 64, 127, 128, 8191, 8192, 0x1FFFFFFF};
    for (uint32_t v : vals) {
        std::vector<uint8_t> enc;
        if (v < 0x80) {
            enc.push_back(static_cast<uint8_t>(v));
        } else if (v < 0x4000) {
            enc.push_back(static_cast<uint8_t>(0x80 | (v >> 8)));
            enc.push_back(static_cast<uint8_t>(v & 0xFF));
        } else {
            enc.push_back(static_cast<uint8_t>(0xC0 | (v >> 24)));
            enc.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            enc.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            enc.push_back(static_cast<uint8_t>(v & 0xFF));
        }
        size_t pos = 0;
        auto decoded = decodeCompressedUInt({enc.data(), enc.size()}, pos);
        ASSERT_TRUE(decoded.has_value()) << "Failed for v=" << v;
        EXPECT_EQ(v, *decoded) << "Mismatch for v=" << v;
    }
}

TEST(IntegrationTest, BlobHeapGetVec) {
    std::vector<uint8_t> data = {0x03, 0x11, 0x22, 0x33};
    BlobHeap h({data.data(), data.size()});
    auto vec = h.getVec(0);
    ASSERT_EQ(3u, vec.size());
    EXPECT_EQ(0x11, vec[0]);
    EXPECT_EQ(0x22, vec[1]);
    EXPECT_EQ(0x33, vec[2]);
}

TEST(IntegrationTest, CliHeapsConstructor) {
    CliHeaps heaps({}, {}, {}, {}, 0x07);  // All heap sizes set
    EXPECT_TRUE(heaps.wideStrings());
    EXPECT_TRUE(heaps.wideGuid());
    EXPECT_TRUE(heaps.wideBlob());
}

TEST(IntegrationTest, CliHeapsNoFlags) {
    CliHeaps heaps({}, {}, {}, {}, 0x00);
    EXPECT_FALSE(heaps.wideStrings());
    EXPECT_FALSE(heaps.wideGuid());
    EXPECT_FALSE(heaps.wideBlob());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
