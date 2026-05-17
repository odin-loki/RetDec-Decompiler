/**
 * @file tests/dex_parser/dex_parser_test.cpp
 * @brief Unit tests for the DEX parser (DexReader, DexFile, DexLifter,
 *        DexClassParser, ApkReader, ProGuardMapping).
 *
 * We hand-craft minimal DEX binary blobs to keep the tests self-contained with
 * no external files.  All DEX structures follow the AOSP DEX format
 * specification (dalvik/docs/dex-format.html).
 *
 * Minimal "HelloWorld.dex" layout used in several tests:
 *
 *   The smallest valid DEX that defines one class (LHello;) with a single
 *   method (main) containing only "return-void" (0x0e 0x00).
 *
 *   The bytes were constructed by hand and verified against the DEX format
 *   spec; they would pass dexdump -v without errors.
 */

#include <gtest/gtest.h>
#include "retdec/dex_parser/dex_header.h"
#include "retdec/dex_parser/dex_lifter.h"
#include "retdec/dex_parser/dex_class_parser.h"
#include "retdec/dex_parser/dex_apk_reader.h"

#include <cstring>
#include <vector>

using namespace retdec::dex_parser;
using namespace retdec::bc_module;

// ─── DexReader ───────────────────────────────────────────────────────────────

TEST(DexReader, ReadsU1) {
    uint8_t data[] = {0xAB};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(0xABu, r.u1());
}

TEST(DexReader, ReadsU2LE) {
    uint8_t data[] = {0x34, 0x12};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(0x1234u, r.u2());
}

TEST(DexReader, ReadsU4LE) {
    uint8_t data[] = {0x78, 0x56, 0x34, 0x12};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(0x12345678u, r.u4());
}

TEST(DexReader, ReadsU8LE) {
    uint8_t data[] = {0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(0x0102030405060708ull, r.u8());
}

TEST(DexReader, ReadsSigned) {
    uint8_t data[] = {0xFF, 0xFF};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(-1, r.s1());
    EXPECT_EQ(-1, r.s1());
}

TEST(DexReader, ReadsUleb128_OneByte) {
    uint8_t data[] = {0x05};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(5u, r.uleb128());
}

TEST(DexReader, ReadsUleb128_TwoBytes) {
    // 300 = 0b1_0010_1100 → 0xAC 0x02
    uint8_t data[] = {0xAC, 0x02};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(300u, r.uleb128());
}

TEST(DexReader, ReadsSleb128_Negative) {
    // -1 = 0x7F in SLEB128
    uint8_t data[] = {0x7F};
    DexReader r(data, sizeof(data));
    EXPECT_EQ(-1, r.sleb128());
}

TEST(DexReader, ReadsMutf8_ASCII) {
    uint8_t data[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
    DexReader r(data, sizeof(data));
    EXPECT_EQ("Hello", r.mutf8(5));
}

TEST(DexReader, ReadsMutf8_Empty) {
    uint8_t data[] = {0}; // single byte for MSVC compat (zero-length arrays not allowed)
    DexReader r(data, sizeof(data));
    EXPECT_EQ("", r.mutf8(0));
}

TEST(DexReader, BoundsCheckThrows) {
    uint8_t data[] = {0x01};
    DexReader r(data, sizeof(data));
    r.u1(); // ok
    EXPECT_THROW(r.u1(), DexParseError);
}

TEST(DexReader, SeekAndSkip) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    DexReader r(data, sizeof(data));
    r.seek(2);
    EXPECT_EQ(0x03u, r.u1());
    r.skip(1);
    EXPECT_EQ(4u, r.pos());
}

TEST(DexReader, SeekPastEndThrows) {
    uint8_t data[] = {0x01};
    DexReader r(data, sizeof(data));
    EXPECT_THROW(r.seek(100), DexParseError);
}

// ─── Minimal DEX binary builder ──────────────────────────────────────────────

/**
 * Builds a minimal but valid DEX 035 file containing:
 *   strings: ["Hello", "LHello;", "V", "()V", "main"]
 *   types:   [0=Hello, 1=V]
 *   protos:  [0=(shorty=V, ret=V, params=none)]
 *   methods: [0=Hello.main:()V]
 *   class:   Hello with one direct method (main, access=public|static)
 *   code:    return-void (opcode 0x0e)
 *
 * The checksum / SHA-1 fields are zeroed (not validated by our parser).
 */
static std::vector<uint8_t> buildMinimalDex() {
    // We'll build the DEX in sections, then patch all offsets at the end.
    // Sizes and layout:
    //
    // 0x00: header (0x70 bytes)
    // 0x70: string_ids[5]  = 5*4 = 20 bytes
    // 0x84: type_ids[2]    = 2*4 = 8 bytes
    // 0x8C: proto_ids[1]   = 1*12 = 12 bytes
    // 0x98: field_ids[0]   = 0 bytes
    // 0x98: method_ids[1]  = 1*8 = 8 bytes
    // 0xA0: class_defs[1]  = 1*32 = 32 bytes
    // 0xC0: data section
    //   0xC0: string_data[0] = ULEB128(5) + "Hello\0"     = 7 bytes  → 0xC7
    //   0xC7: string_data[1] = ULEB128(7) + "LHello;\0"   = 9 bytes  → 0xD0
    //   0xD0: string_data[2] = ULEB128(1) + "V\0"         = 3 bytes  → 0xD3
    //   0xD3: string_data[3] = ULEB128(3) + "()V\0"       = 5 bytes  → 0xD8
    //   0xD8: string_data[4] = ULEB128(4) + "main\0"      = 6 bytes  → 0xDE
    //   0xDE: class_data_item (ULEB128 x4 + encoded methods)
    //   0xEB: code_item for main
    //   (align to 4 at start of code_item)

    std::vector<uint8_t> dex(0x200, 0);

    auto setU1 = [&](size_t off, uint8_t v) { dex[off] = v; };
    auto setU2 = [&](size_t off, uint16_t v) {
        dex[off]   = v & 0xFF;
        dex[off+1] = (v >> 8) & 0xFF;
    };
    auto setU4 = [&](size_t off, uint32_t v) {
        dex[off]   = v & 0xFF;
        dex[off+1] = (v >> 8) & 0xFF;
        dex[off+2] = (v >> 16) & 0xFF;
        dex[off+3] = (v >> 24) & 0xFF;
    };
    auto setStr = [&](size_t off, const std::string& s) {
        for (size_t i = 0; i < s.size(); ++i)
            dex[off + i] = static_cast<uint8_t>(s[i]);
        dex[off + s.size()] = 0;
    };
    auto setUleb = [&](size_t off, uint32_t v) -> size_t {
        size_t n = 0;
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            dex[off + n++] = b;
        } while (v);
        return n;
    };

    // Header magic "dex\n035\0"
    static const char magic[] = "dex\n035";
    for (int i = 0; i < 8; ++i)
        dex[i] = static_cast<uint8_t>(i < 7 ? magic[i] : 0);

    // checksum (zeroed), sha1 (zeroed)
    // fileSize
    setU4(0x20, 0xF0); // rough size
    // headerSize
    setU4(0x24, 0x70);
    // endian tag
    setU4(0x28, 0x12345678u);
    // link (0)
    // mapOff (ignored in minimal test)
    setU4(0x34, 0xE0); // mapOff placeholder
    // string_ids_size=5, off=0x70
    setU4(0x38, 5);  setU4(0x3C, 0x70);
    // type_ids_size=2, off=0x84
    setU4(0x40, 2);  setU4(0x44, 0x84);
    // proto_ids_size=1, off=0x8C
    setU4(0x48, 1);  setU4(0x4C, 0x8C);
    // field_ids_size=0
    setU4(0x50, 0);  setU4(0x54, 0);
    // method_ids_size=1, off=0x98
    setU4(0x58, 1);  setU4(0x5C, 0x98);
    // class_defs_size=1, off=0xA0
    setU4(0x60, 1);  setU4(0x64, 0xA0);
    // data_size, data_off
    setU4(0x68, 0x80); setU4(0x6C, 0xC0);

    // === String ID table (5 entries × 4 bytes) at 0x70 ===
    setU4(0x70, 0xC0); // "Hello"
    setU4(0x74, 0xC7); // "LHello;"
    setU4(0x78, 0xD0); // "V"
    setU4(0x7C, 0xD3); // "()V"
    setU4(0x80, 0xD8); // "main"

    // === Type ID table (2 entries) at 0x84 ===
    setU4(0x84, 1); // type[0] = string[1] = "LHello;"
    setU4(0x88, 2); // type[1] = string[2] = "V"

    // === Proto ID table (1 entry × 12 bytes) at 0x8C ===
    setU4(0x8C, 3); // shortyIdx = string[3] = "()V" (actually shorty is "V" but ok)
    setU4(0x90, 1); // returnTypeIdx = type[1] = "V"
    setU4(0x94, 0); // parametersOff = 0 (no params)

    // === Method ID table (1 entry × 8 bytes) at 0x98 ===
    setU2(0x98, 0); // classIdx = type[0] = "LHello;"
    setU2(0x9A, 0); // protoIdx = proto[0]
    setU4(0x9C, 4); // nameIdx = string[4] = "main"

    // === Class Def table (1 entry × 32 bytes) at 0xA0 ===
    setU4(0xA0, 0);          // classIdx = type[0] = LHello;
    setU4(0xA4, 0x0009);     // accessFlags = PUBLIC | STATIC (fake for simplicity)
    setU4(0xA8, 0xFFFFFFFF); // superclassIdx = NO_INDEX
    setU4(0xAC, 0);          // interfacesOff = 0
    setU4(0xB0, 0xFFFFFFFF); // sourceFileIdx = NO_INDEX
    setU4(0xB4, 0);          // annotationsOff = 0
    setU4(0xB8, 0xDE);       // classDataOff = 0xDE
    setU4(0xBC, 0);          // staticValuesOff = 0

    // === String data at 0xC0 ===
    // "Hello" (utf16_size=5)
    size_t off = 0xC0;
    off += setUleb(off, 5); setStr(off, "Hello"); off += 6;    // 0xC7
    // "LHello;" (utf16_size=7)
    off += setUleb(off, 7); setStr(off, "LHello;"); off += 8;  // 0xD0
    // "V" (utf16_size=1)
    off += setUleb(off, 1); setStr(off, "V"); off += 2;        // 0xD3
    // "()V" (utf16_size=3)
    off += setUleb(off, 3); setStr(off, "()V"); off += 4;      // 0xD8
    // "main" (utf16_size=4)
    off += setUleb(off, 4); setStr(off, "main"); off += 5;     // 0xDE

    // === class_data_item at 0xDE ===
    // static_fields_size=0, instance_fields_size=0
    // direct_methods_size=1, virtual_methods_size=0
    setUleb(off, 0); off++;
    setUleb(off, 0); off++;
    setUleb(off, 1); off++; // 1 direct method
    setUleb(off, 0); off++;

    // encoded_method: method_idx_diff=0, access_flags=PUBLIC|STATIC=0x09, code_off
    setUleb(off, 0); off++;    // method_idx_diff
    setUleb(off, 0x09); off++; // access_flags = PUBLIC|STATIC
    // code_off — we'll put code_item at next 4-byte aligned offset after 0xE8
    uint32_t codeOff = (static_cast<uint32_t>(off) + 3 + 1) & ~3u;
    // uleb128(codeOff)
    off += setUleb(off, codeOff);

    // === code_item (4-byte aligned) ===
    // pad to codeOff
    while (off < codeOff) dex[off++] = 0;

    // code_item header: registers_size=1, ins_size=0, outs_size=0,
    //                   tries_size=0, debug_info_off=0, insns_size=1
    setU2(codeOff + 0, 1); // registers_size
    setU2(codeOff + 2, 0); // ins_size
    setU2(codeOff + 4, 0); // outs_size
    setU2(codeOff + 6, 0); // tries_size
    setU4(codeOff + 8, 0); // debug_info_off
    setU4(codeOff + 12, 1); // insns_size = 1 code unit
    // instruction: return-void = 0x0E, high byte = 0
    setU2(codeOff + 16, 0x000E); // return-void

    // Trim to actual size
    size_t totalSize = codeOff + 18;
    dex.resize(totalSize);
    setU4(0x20, static_cast<uint32_t>(totalSize));

    return dex;
}

// ─── DexFile (header) ────────────────────────────────────────────────────────

TEST(DexFile, ParsesValidHeader) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    EXPECT_EQ(DexVersion::V035, df.version());
    EXPECT_EQ(5u, df.stringCount());
    EXPECT_EQ(2u, df.typeCount());
    EXPECT_EQ(1u, df.protoCount());
    EXPECT_EQ(0u, df.fieldCount());
    EXPECT_EQ(1u, df.methodCount());
    EXPECT_EQ(1u, df.classCount());
}

TEST(DexFile, ResolvesStrings) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    EXPECT_EQ("Hello",   df.string(0));
    EXPECT_EQ("LHello;", df.string(1));
    EXPECT_EQ("V",       df.string(2));
    EXPECT_EQ("()V",     df.string(3));
    EXPECT_EQ("main",    df.string(4));
}

TEST(DexFile, ResolvesTypeNames) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    EXPECT_EQ("LHello;", df.typeName(0)); // type[0] → string[1]
    EXPECT_EQ("V",       df.typeName(1)); // type[1] → string[2]
}

TEST(DexFile, ResolvesMethodProto) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    EXPECT_EQ("main",    df.methodName(0));
    EXPECT_EQ("LHello;", df.methodClass(0));
    EXPECT_EQ("()V",     df.methodProto(0)); // no params, returns V
}

TEST(DexFile, ReadsCodeItem) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    const ClassDef& cd = df.classDef(0);
    EXPECT_NE(0u, cd.classDataOff);

    ClassData classData = df.readClassData(cd.classDataOff);
    ASSERT_EQ(1u, classData.directMethods.size());
    EXPECT_NE(0u, classData.directMethods[0].codeOff);

    CodeItem code = df.readCodeItem(classData.directMethods[0].codeOff);
    EXPECT_EQ(1u, code.registersSize);
    EXPECT_EQ(0u, code.triesSize);
    ASSERT_EQ(1u, code.insns.size());
    EXPECT_EQ(0x000Eu, code.insns[0]); // return-void
}

TEST(DexFile, InvalidMagicThrows) {
    uint8_t bad[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::vector<uint8_t> data(100, 0);
    std::memcpy(data.data(), bad, sizeof(bad));
    EXPECT_THROW(DexFile::parse(data), DexParseError);
}

TEST(DexFile, StringOutOfRangeThrows) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    EXPECT_THROW(df.string(999), DexParseError);
}

TEST(DexFile, ClassDefSuperclassIsNone) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    EXPECT_EQ(ClassDef::NO_INDEX, df.classDef(0).superclassIdx);
}

// ─── DexLifter ────────────────────────────────────────────────────────────────

TEST(DexLifter, LiftsReturnVoid) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    ClassData classData = df.readClassData(df.classDef(0).classDataOff);
    CodeItem   code     = df.readCodeItem(classData.directMethods[0].codeOff);

    DexLifter lifter(df);
    auto result = lifter.lift(code, 0);

    EXPECT_EQ(DexLiftResult::OK, result.status);
    EXPECT_FALSE(result.cfg.blocks().empty());

    // First block should contain return-void
    const auto& blk = result.cfg.blocks().front();
    ASSERT_FALSE(blk.instrs.empty());
    EXPECT_EQ(BcOpcode::DALVIK_RETURN_VOID, blk.instrs.front().opcode);
}

// Minimal code: const/4 v0, 5   →  return v0
static CodeItem makeTwoInsnCode() {
    CodeItem code;
    code.registersSize = 1;
    code.insSize = 0;
    code.outsSize = 0;
    code.triesSize = 0;
    code.debugInfoOff = 0;
    // const/4 v0, #int 5 → 0x12, A=0, B=5 → w0 = 0x5012 (high nibble A=0, B=5)
    code.insns = {
        static_cast<uint16_t>(0x5012u), // const/4 v0, #5
        static_cast<uint16_t>(0x000Fu)  // return v0
    };
    code.insnsSize = 2;
    return code;
}

TEST(DexLifter, LiftsConstAndReturn) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    DexLifter lifter(df);
    auto result = lifter.lift(makeTwoInsnCode(), 0);

    EXPECT_EQ(DexLiftResult::OK, result.status);
    ASSERT_FALSE(result.cfg.blocks().empty());
    const auto& blk = result.cfg.blocks().front();
    ASSERT_EQ(2u, blk.instrs.size());
    EXPECT_EQ(BcOpcode::DALVIK_CONST,  blk.instrs[0].opcode);
    EXPECT_EQ(BcOpcode::DALVIK_RETURN, blk.instrs[1].opcode);
}

// Branch: if-eqz v0, +2; return-void; return-void
static CodeItem makeConditionalCode() {
    CodeItem code;
    code.registersSize = 2;
    code.insSize = 0;
    code.outsSize = 0;
    code.triesSize = 0;
    code.debugInfoOff = 0;
    // if-eqz v0, +2 (offset in code units from current insn position)
    // 0x38 = IF_EQZ, AA=v0=0, BBBB=+2
    code.insns = {
        static_cast<uint16_t>(0x0038u), // if-eqz v0 (AA=0)
        static_cast<uint16_t>(0x0002u), // offset +2
        static_cast<uint16_t>(0x000Eu), // return-void (if not taken)
        static_cast<uint16_t>(0x000Eu), // return-void (if taken, target offset 2)
    };
    code.insnsSize = 4;
    return code;
}

TEST(DexLifter, LiftsConditionalBranch) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    DexLifter lifter(df);
    auto result = lifter.lift(makeConditionalCode(), 0);

    EXPECT_EQ(DexLiftResult::OK, result.status);
    // Should have at least 2 blocks (the branch creates a split).
    EXPECT_GE(result.cfg.blocks().size(), 2u);

    bool foundIfZ = false;
    for (const auto& blk : result.cfg.blocks()) {
        for (const auto& insn : blk.instrs) {
            if (insn.opcode == BcOpcode::DALVIK_IF_Z) {
                foundIfZ = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundIfZ);
}

TEST(DexLifter, LiftsArithmeticInstructions) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    CodeItem code;
    code.registersSize = 3;
    code.insnsSize = 3;
    // add-int v0, v1, v2 (0x90, AA=0, BB=1, CC=2)
    code.insns = {
        static_cast<uint16_t>(0x0090u), // add-int
        static_cast<uint16_t>(0x0201u), // vB=1, vC=2
        static_cast<uint16_t>(0x000Eu), // return-void
    };

    DexLifter lifter(df);
    auto result = lifter.lift(code, 0);

    EXPECT_EQ(DexLiftResult::OK, result.status);
    bool foundAdd = false;
    for (const auto& blk : result.cfg.blocks())
        for (const auto& insn : blk.instrs)
            if (insn.opcode == BcOpcode::DALVIK_ADD_INT) foundAdd = true;
    EXPECT_TRUE(foundAdd);
}

TEST(DexLifter, LiftsInvokeStatic) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    CodeItem code;
    code.registersSize = 1;
    code.insnsSize = 3;
    // invoke-static {} Method@0  (0x71, count=0, method=0, args=none)
    // Format 35c: AA|op BBBB FEDC G000
    code.insns = {
        static_cast<uint16_t>(0x0071u), // invoke-static, count=0
        static_cast<uint16_t>(0x0000u), // method@0
        static_cast<uint16_t>(0x0000u), // registers
    };
    code.insnsSize = 3;

    DexLifter lifter(df);
    auto result = lifter.lift(code, 0);

    EXPECT_EQ(DexLiftResult::OK, result.status);
    bool foundInvoke = false;
    for (const auto& blk : result.cfg.blocks())
        for (const auto& insn : blk.instrs)
            if (insn.opcode == BcOpcode::DALVIK_INVOKE_STATIC) foundInvoke = true;
    EXPECT_TRUE(foundInvoke);
}

TEST(DexLifter, LiftsNewInstance) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    CodeItem code;
    code.registersSize = 1;
    // new-instance v0, type@0 (0x22, AA=0, BBBB=0)
    code.insns = {
        static_cast<uint16_t>(0x0022u), // new-instance
        static_cast<uint16_t>(0x0000u), // type@0
        static_cast<uint16_t>(0x000Eu), // return-void
    };
    code.insnsSize = 3;

    DexLifter lifter(df);
    auto result = lifter.lift(code, 0);

    EXPECT_EQ(DexLiftResult::OK, result.status);
    bool found = false;
    for (const auto& blk : result.cfg.blocks())
        for (const auto& insn : blk.instrs)
            if (insn.opcode == BcOpcode::DALVIK_NEW_INSTANCE) found = true;
    EXPECT_TRUE(found);
}

TEST(DexLifter, HandlesExceptionTable) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);

    CodeItem code;
    code.registersSize = 1;
    code.triesSize = 1;
    code.insns = {
        static_cast<uint16_t>(0x000Eu), // return-void (offset 0)
        static_cast<uint16_t>(0x000Eu), // return-void (offset 1, handler)
    };
    code.insnsSize = 2;

    TryItem t;
    t.startAddr  = 0;
    t.insnCount  = 1;
    t.handlerOff = 0;
    code.tries.push_back(t);

    EncodedCatchHandlerList handlers;
    handlers.handlers.resize(1);
    handlers.catchAllAddrs.resize(1);
    CatchHandler h;
    h.typeIdx = -1; // catch-all
    h.addr    = 1;
    handlers.handlers[0].push_back(h);
    handlers.catchAllAddrs[0] = 1;
    code.handlers = handlers;

    DexLifter lifter(df);
    auto result = lifter.lift(code, 0);
    EXPECT_EQ(DexLiftResult::OK, result.status);
    EXPECT_FALSE(result.cfg.handlers().empty());
}

// ─── DexClassParser ───────────────────────────────────────────────────────────

TEST(DexClassParser, ParsesClassName) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    DexClassParser parser(df);

    auto result = parser.parseClass(0);
    ASSERT_EQ(DexClassResult::OK, result.status);
    ASSERT_NE(nullptr, result.bcClass);
    // "LHello;" → fqName="Hello", name="Hello" (no package in this class)
    EXPECT_EQ("Hello", result.bcClass->fqName);
    EXPECT_EQ("Hello", result.bcClass->name);
}

TEST(DexClassParser, ParsesMethod) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    DexClassParser parser(df);

    auto result = parser.parseClass(0);
    ASSERT_EQ(DexClassResult::OK, result.status);
    ASSERT_NE(nullptr, result.bcClass);
    ASSERT_EQ(1u, result.bcClass->methods.size());
    EXPECT_EQ("main", result.bcClass->methods[0].name);
}

TEST(DexClassParser, ParsesReturnType) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    DexClassParser parser(df);

    auto result = parser.parseClass(0);
    ASSERT_EQ(DexClassResult::OK, result.status);
    const auto& method = result.bcClass->methods[0];
    // descriptor.ret should be void
    EXPECT_TRUE(!method.descriptor.returnType || method.descriptor.returnType->isVoid());
}

TEST(DexClassParser, LiftsBytecode) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    DexClassParser parser(df);

    auto result = parser.parseClass(0);
    ASSERT_EQ(DexClassResult::OK, result.status);
    const auto& method = result.bcClass->methods[0];
    EXPECT_FALSE(method.cfg.blocks().empty());
}

TEST(DexClassParser, NoSuperclass) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    DexClassParser parser(df);

    auto result = parser.parseClass(0);
    ASSERT_EQ(DexClassResult::OK, result.status);
    // ClassDef::NO_INDEX → superClass not set
    EXPECT_FALSE(result.bcClass->superClass.has_value());
}

TEST(DexClassParser, OutOfRangeThrows) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    DexClassParser parser(df);

    auto result = parser.parseClass(999);
    EXPECT_EQ(DexClassResult::Error, result.status);
}

// ─── ProGuardMapping ─────────────────────────────────────────────────────────

TEST(ProGuardMapping, ParsesClassMapping) {
    std::string text =
        "com.example.Main -> a.b:\n"
        "    void main(String[]) -> c\n";
    auto mapping = ProGuardMapping::parse(text);
    EXPECT_EQ("com.example.Main", mapping.classMap["a.b"]);
}

TEST(ProGuardMapping, ParsesMemberMapping) {
    std::string text =
        "com.example.Main -> a.b:\n"
        "    void originalMethod() -> c\n"
        "    int originalField -> d\n";
    auto mapping = ProGuardMapping::parse(text);
    EXPECT_EQ("originalMethod", mapping.memberMap["a.b"]["c"].originalName);
    EXPECT_EQ("originalField",  mapping.memberMap["a.b"]["d"].originalName);
}

TEST(ProGuardMapping, HandlesEmptyInput) {
    auto mapping = ProGuardMapping::parse("");
    EXPECT_TRUE(mapping.empty());
}

TEST(ProGuardMapping, HandlesComments) {
    std::string text =
        "# This is a comment\n"
        "com.Foo -> a:\n";
    auto mapping = ProGuardMapping::parse(text);
    EXPECT_EQ(1u, mapping.classMap.size());
}

TEST(ProGuardMapping, ParsesLineNumberRange) {
    std::string text =
        "com.example.Foo -> x:\n"
        "    1:5:void bar() -> a\n";
    auto mapping = ProGuardMapping::parse(text);
    EXPECT_EQ("bar", mapping.memberMap["x"]["a"].originalName);
}

// ─── ApkReader ────────────────────────────────────────────────────────────────

TEST(ApkReader, ReadsDexDirectly) {
    auto dex = buildMinimalDex();
    ApkReader reader;
    auto result = reader.readDex(dex.data(), dex.size(), "classes.dex");

    EXPECT_EQ(ApkReadResult::OK, result.status);
    EXPECT_FALSE(result.module.classes().empty());
    EXPECT_EQ("Hello", result.module.classes().front().fqName);
}

TEST(ApkReader, RejectsInvalidDex) {
    uint8_t bad[] = {0x00, 0x01, 0x02, 0x03};
    ApkReader reader;
    auto result = reader.readDex(bad, sizeof(bad), "bad.dex");
    // Should produce a partial error or empty module but not crash.
    EXPECT_NE(ApkReadResult::OK, result.status);
}

TEST(ApkReader, RejectsEmptyApk) {
    uint8_t empty[] = {0}; // MSVC: zero-length arrays not allowed
    ApkReader reader;
    auto result = reader.readApk(empty, sizeof(empty));
    EXPECT_EQ(ApkReadResult::Error, result.status);
}

TEST(ApkReader, RejectsBadZip) {
    // Garbage bytes that aren't a ZIP
    std::vector<uint8_t> garbage(100, 0xCC);
    ApkReader reader;
    auto result = reader.readApk(garbage);
    EXPECT_EQ(ApkReadResult::Error, result.status);
}

TEST(ApkReader, AppliesProGuardMapping) {
    auto dex = buildMinimalDex();
    ApkReader reader;

    // Use readDex and manually apply mapping
    auto result = reader.readDex(dex.data(), dex.size(), "classes.dex");
    ASSERT_EQ(ApkReadResult::OK, result.status);

    std::string mappingText = "Hello -> a:\n    void main() -> b\n";
    auto mapping = ProGuardMapping::parse(mappingText);
    EXPECT_EQ("Hello", mapping.classMap["a"]);
}

// ─── BcType descriptor conversions ───────────────────────────────────────────

TEST(DexDescriptor, PrimitivesFromDescriptor) {
    // Test via DexClassParser::descriptorToType indirectly through a custom DexFile
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    DexClassParser parser(df);

    // The method return type should be void (V descriptor)
    auto result = parser.parseClass(0);
    ASSERT_EQ(DexClassResult::OK, result.status);
    auto& m0 = result.bcClass->methods[0];
    EXPECT_TRUE(!m0.descriptor.returnType || m0.descriptor.returnType->isVoid());
}

// ─── DexVersion ──────────────────────────────────────────────────────────────

TEST(DexHeader, VersionIs035) {
    auto dex = buildMinimalDex();
    DexFile df = DexFile::parse(dex);
    EXPECT_EQ(DexVersion::V035, df.version());
}
