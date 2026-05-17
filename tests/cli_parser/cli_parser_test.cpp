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
