/**
 * @file tests/lua_parser/lua_parser_test.cpp
 * @brief Unit tests for LuaReader and LuaEmitter.
 */

#include "retdec/lua_parser/lua_reader.h"
#include "retdec/lua_parser/lua_emitter.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

using namespace retdec::lua_parser;

// ─── Binary builder helpers ───────────────────────────────────────────────────

class LuaBuilder {
public:
    void u8(uint8_t v)  { data_.push_back(v); }
    void u32(uint32_t v) {
        data_.push_back(v & 0xFF);
        data_.push_back((v >> 8) & 0xFF);
        data_.push_back((v >> 16) & 0xFF);
        data_.push_back((v >> 24) & 0xFF);
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) { data_.push_back(v & 0xFF); v >>= 8; }
    }
    void i32(int32_t v) { u32((uint32_t)v); }
    void str51(const std::string& s) {
        // size_t (8 bytes) + string + null
        uint64_t len = s.size() + 1;
        u64(len);
        for (char c : s) data_.push_back((uint8_t)c);
        data_.push_back(0); // null terminator
    }
    void emptyStr51() { u64(0); } // empty string = size 0

    const std::vector<uint8_t>& bytes() const { return data_; }
private:
    std::vector<uint8_t> data_;
};

// Build a minimal Lua 5.1 .luac file with one function that returns nil
static std::vector<uint8_t> minimalLua51() {
    LuaBuilder b;
    // Magic
    b.u8(0x1B); b.u8('L'); b.u8('u'); b.u8('a');
    b.u8(0x51); // version 5.1
    b.u8(0x00); // official format
    b.u8(0x01); // little-endian
    b.u8(0x04); // int size = 4
    b.u8(0x08); // size_t = 8
    b.u8(0x04); // instruction size = 4
    b.u8(0x08); // lua_Number size = 8
    b.u8(0x00); // integral flag = 0 (double)

    // Top-level proto
    b.emptyStr51();   // source name = ""
    b.i32(0);          // lineDefined
    b.i32(0);          // lastLineDefined
    b.u8(0);           // numUpvalues
    b.u8(0);           // numParams
    b.u8(0);           // isVarArg
    b.u8(2);           // maxStackSize

    // Code: RETURN 0 1 (return with 0 return values)
    b.i32(1);          // 1 instruction
    // RETURN A=0 B=1 C=0: opcode=31, A=0, B=1, C=0
    // field layout: bits 0-5=op, 6-13=A, 14-22=C, 23-31=B
    uint32_t ret_instr = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    b.u32(ret_instr);

    // Constants: 0
    b.i32(0);

    // Protos: 0
    b.i32(0);

    // Debug info (lineinfo, locals, upvalue names)
    b.i32(1);          // 1 line info entry
    b.i32(1);          // line 1
    b.i32(0);          // 0 locals
    b.i32(0);          // 0 upvalue names

    return b.bytes();
}

// Build Lua 5.4 minimal header
static std::vector<uint8_t> minimalLua54() {
    LuaBuilder b;
    // Magic
    b.u8(0x1B); b.u8('L'); b.u8('u'); b.u8('a');
    b.u8(0x54); // version 5.4
    b.u8(0x00); // format
    // LUAC_DATA
    b.u8(0x19); b.u8(0x93); b.u8(0x0D); b.u8(0x0A); b.u8(0x1A); b.u8(0x0A);
    b.u8(4);    // instruction size
    b.u8(8);    // integer size
    // Test integer (0x5678)
    b.u64(0x5678);
    // Test float (370.5)
    double testfloat = 370.5;
    uint64_t tf; std::memcpy(&tf, &testfloat, 8);
    b.u64(tf);
    // upvalue count for main chunk
    b.u8(1);

    // Top-level proto (5.4 format)
    // source name (5.3+ format: 0 = empty)
    b.u8(0); // empty string

    b.i32(0); // lineDefined
    b.i32(0); // lastLineDefined
    b.u8(0);  // numParams
    b.u8(1);  // isVarArg
    b.u8(2);  // maxStackSize

    // Code: RETURN0 A=0 (opcode=70 in 5.4)
    b.i32(1);
    // opcode=70 (0x46), A=0, B=0, C=0
    // 5.4 layout: bits 0-6=op, 7-14=A, 15-22=B, 23-31=C
    uint32_t ret0 = 70; // RETURN0
    b.u32(ret0);

    // Constants: 0
    b.i32(0);
    // Upvalues: 1 (for _ENV)
    b.i32(1);
    b.u8(1); // inStack
    b.u8(0); // idx
    b.u8(0); // kind

    // Sub-protos: 0
    b.i32(0);

    // Debug info
    b.i32(1); b.i32(1); // 1 lineinfo entry
    b.i32(0); // 0 locals
    b.i32(1); // 1 upvalue name
    // upvalue name "_ENV" (5.3+ format: length byte)
    b.u8(5);   // length 4+1=5
    b.u8('_'); b.u8('E'); b.u8('N'); b.u8('V');

    return b.bytes();
}

// ─── LuaVersion tests ────────────────────────────────────────────────────────

TEST(LuaVersionTest, VersionStrings) {
    EXPECT_EQ(luaVersionStr(LuaVersion::Lua51), "5.1");
    EXPECT_EQ(luaVersionStr(LuaVersion::Lua52), "5.2");
    EXPECT_EQ(luaVersionStr(LuaVersion::Lua53), "5.3");
    EXPECT_EQ(luaVersionStr(LuaVersion::Lua54), "5.4");
    EXPECT_EQ(luaVersionStr(LuaVersion::Unknown), "unknown");
}

// ─── LuaConst tests ──────────────────────────────────────────────────────────

TEST(LuaConstTest, NilStr) {
    LuaConst c = LuaNil{};
    EXPECT_EQ(luaConstStr(c), "nil");
}

TEST(LuaConstTest, BoolStr) {
    EXPECT_EQ(luaConstStr(LuaConst(LuaBool{true})),  "true");
    EXPECT_EQ(luaConstStr(LuaConst(LuaBool{false})), "false");
}

TEST(LuaConstTest, IntStr) {
    EXPECT_EQ(luaConstStr(LuaConst(LuaInt{42})),   "42");
    EXPECT_EQ(luaConstStr(LuaConst(LuaInt{-1})),   "-1");
    EXPECT_EQ(luaConstStr(LuaConst(LuaInt{0})),    "0");
}

TEST(LuaConstTest, FloatStr) {
    std::string s = luaConstStr(LuaConst(LuaFloat{3.14}));
    EXPECT_NE(s.find('.'), std::string::npos);
}

TEST(LuaConstTest, FloatWholeNumber) {
    std::string s = luaConstStr(LuaConst(LuaFloat{1.0}));
    // Should contain a decimal point
    EXPECT_NE(s.find('.'), std::string::npos);
}

TEST(LuaConstTest, StringEscaping) {
    std::string s = luaConstStr(LuaConst(LuaStr{"hello\nworld"}));
    EXPECT_EQ(s, "\"hello\\nworld\"");
}

TEST(LuaConstTest, StringQuoteEscape) {
    std::string s = luaConstStr(LuaConst(LuaStr{"say \"hi\""}));
    EXPECT_NE(s.find("\\\""), std::string::npos);
}

TEST(LuaConstTest, StringBackslashEscape) {
    std::string s = luaConstStr(LuaConst(LuaStr{"a\\b"}));
    EXPECT_NE(s.find("\\\\"), std::string::npos);
}

// ─── LuaInstr field tests ────────────────────────────────────────────────────

TEST(LuaInstrTest, Opcode51) {
    LuaInstr ins;
    ins.raw = 31; // RETURN opcode = 31, all other fields 0
    EXPECT_EQ(ins.opcode51(), 31);
}

TEST(LuaInstrTest, FieldA51) {
    LuaInstr ins;
    // A in bits 6-13
    ins.raw = (5 << 6);
    EXPECT_EQ(ins.fieldA51(), 5);
}

TEST(LuaInstrTest, FieldBx) {
    LuaInstr ins;
    // Bx = bits 14-31 (18 bits)
    ins.raw = (100 << 14) | 1; // op=1, Bx=100
    EXPECT_EQ(ins.fieldBx(), 100);
}

TEST(LuaInstrTest, FieldSBx) {
    LuaInstr ins;
    // sBx bias = (1<<17)-1 = 131071
    // sBx of 0 → raw Bx = 131071
    ins.raw = (131071 << 14);
    EXPECT_EQ(ins.fieldsBx(), 0);
}

TEST(LuaInstrTest, FieldSBxNeg) {
    LuaInstr ins;
    // sBx of -1 → raw Bx = 131070
    ins.raw = (131070 << 14);
    EXPECT_EQ(ins.fieldsBx(), -1);
}

TEST(LuaInstrTest, IsRK) {
    LuaInstr ins; ins.raw = 0;
    EXPECT_FALSE(ins.isRK(0));
    EXPECT_TRUE(ins.isRK(0x100));
    EXPECT_TRUE(ins.isRK(0x1FF));
}

TEST(LuaInstrTest, RkIdx) {
    LuaInstr ins; ins.raw = 0;
    EXPECT_EQ(ins.rkIdx(0x105), 5);
}

// ─── LuaProto helpers ────────────────────────────────────────────────────────

TEST(LuaProtoTest, LineForPc) {
    LuaProto proto;
    proto.lineInfo = {1, 2, 3, 4};
    EXPECT_EQ(proto.lineForPc(0), 1);
    EXPECT_EQ(proto.lineForPc(3), 4);
    EXPECT_EQ(proto.lineForPc(99), 0); // out of range
}

TEST(LuaProtoTest, UpvalueNameFallback) {
    LuaProto proto;
    EXPECT_EQ(proto.upvalueName(0), "upval0");
    EXPECT_EQ(proto.upvalueName(5), "upval5");
}

TEST(LuaProtoTest, UpvalueNameFromDebug) {
    LuaProto proto;
    LuaUpvalue uv; uv.name = "_ENV";
    proto.upvalues.push_back(uv);
    EXPECT_EQ(proto.upvalueName(0), "_ENV");
}

// ─── LuaReader tests ─────────────────────────────────────────────────────────

TEST(LuaReaderTest, RejectsEmptyFile) {
    LuaReader reader(std::vector<uint8_t>{});
    auto result = reader.read();
    EXPECT_FALSE(result.ok);
}

TEST(LuaReaderTest, RejectsBadMagic) {
    std::vector<uint8_t> bad = {0xDE, 0xAD, 0xBE, 0xEF};
    LuaReader reader(bad);
    auto result = reader.read();
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("magic"), std::string::npos);
}

TEST(LuaReaderTest, ParsesMinimalLua51) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.module.version, LuaVersion::Lua51);
    EXPECT_EQ(result.module.topLevel.code.size(), 1u);
    EXPECT_TRUE(result.module.topLevel.constants.empty());
    EXPECT_TRUE(result.module.topLevel.protos.empty());
}

TEST(LuaReaderTest, ParsesMinimalLua54) {
    auto bytes = minimalLua54();
    LuaReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.module.version, LuaVersion::Lua54);
    EXPECT_EQ(result.module.topLevel.code.size(), 1u);
}

TEST(LuaReaderTest, Lua51MaxStack) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.module.topLevel.maxStackSize, 2);
}

TEST(LuaReaderTest, Lua51LineInfo) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.module.topLevel.lineInfo.size(), 1u);
    EXPECT_EQ(result.module.topLevel.lineInfo[0], 1);
}

TEST(LuaReaderTest, Lua51InstrOpcode) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.module.topLevel.code.size(), 1u);
    EXPECT_EQ(result.module.topLevel.code[0].opcode51(), 31u); // RETURN
}

TEST(LuaReaderTest, LittleEndianFlag) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.module.littleEndian);
}

// ─── LuaEmitter tests ────────────────────────────────────────────────────────

TEST(LuaEmitterTest, EmitsFileHeader) {
    LuaModule mod;
    mod.version = LuaVersion::Lua54;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("Generated by RetDec"), std::string::npos);
    EXPECT_NE(result.source.find("5.4"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsVersionInHeader) {
    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel.source = "@test.lua";

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("5.1"), std::string::npos);
    EXPECT_NE(result.source.find("test.lua"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsReturnInstruction51) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto rr = reader.read();
    ASSERT_TRUE(rr.ok) << rr.error;

    LuaEmitter emitter;
    auto result = emitter.emit(rr.module);
    EXPECT_NE(result.source.find("return"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsReturnInstruction54) {
    auto bytes = minimalLua54();
    LuaReader reader(bytes);
    auto rr = reader.read();
    ASSERT_TRUE(rr.ok) << rr.error;

    LuaEmitter emitter;
    auto result = emitter.emit(rr.module);
    EXPECT_NE(result.source.find("return"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsFunctionWithParams) {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;
    proto.numParams = 2;
    proto.maxStackSize = 4;
    LuaLocal p0; p0.name = "x"; p0.startPc = 0; p0.endPc = 10;
    LuaLocal p1; p1.name = "y"; p1.startPc = 0; p1.endPc = 10;
    proto.locals = {p0, p1};
    // RETURN A=0 B=1: return nothing
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    proto.code = {ret};
    proto.lineInfo = {1};
    proto.lineDefined = 1;
    proto.lastLineDefined = 5;

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("function main(x, y)"), std::string::npos);
    EXPECT_NE(result.source.find("return"), std::string::npos);
    EXPECT_NE(result.source.find("end"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsMoveInstr) {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;
    proto.maxStackSize = 4;
    LuaLocal r0; r0.name = "a"; r0.startPc = 0; r0.endPc = 5;
    LuaLocal r1; r1.name = "b"; r1.startPc = 0; r1.endPc = 5;
    proto.locals = {r0, r1};
    proto.lineDefined = 0; proto.lastLineDefined = 0;

    // MOVE A=0 B=1: a = b
    LuaInstr mv; mv.raw = 0 | (0 << 6) | (0 << 14) | (1 << 23);
    // RETURN
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    proto.code = {mv, ret};
    proto.lineInfo = {1, 2};

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("a = b"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsArithmeticInstr) {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;
    proto.maxStackSize = 4;
    LuaLocal r0; r0.name = "c"; r0.startPc = 0; r0.endPc = 5;
    LuaLocal r1; r1.name = "x"; r1.startPc = 0; r1.endPc = 5;
    LuaLocal r2; r2.name = "y"; r2.startPc = 0; r2.endPc = 5;
    proto.locals = {r0, r1, r2};
    proto.lineDefined = 0; proto.lastLineDefined = 0;

    // ADD A=0 B=1 C=2 (RK): c = x + y (opcode=13)
    LuaInstr add;
    add.raw = 13 | (0 << 6) | (2 << 14) | (1 << 23);
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    proto.code = {add, ret};
    proto.lineInfo = {1, 2};

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("c = x + y"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsConstantLoad) {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;
    proto.maxStackSize = 2;
    LuaLocal r0; r0.name = "val"; r0.startPc = 0; r0.endPc = 5;
    proto.locals = {r0};
    proto.lineDefined = 0; proto.lastLineDefined = 0;
    proto.constants = {LuaFloat{3.14}};

    // LOADK A=0 Bx=0: val = 3.14
    LuaInstr lk; lk.raw = 1 | (0 << 6) | (0 << 14);
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    proto.code = {lk, ret};
    proto.lineInfo = {1, 2};

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("val = "), std::string::npos);
    EXPECT_NE(result.source.find("3.14"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsUpvalueName) {
    LuaProto proto;
    proto.version = LuaVersion::Lua54;
    proto.maxStackSize = 2;
    proto.lineDefined = 0; proto.lastLineDefined = 0;
    LuaUpvalue uv; uv.name = "_ENV";
    proto.upvalues = {uv};

    LuaModule mod;
    mod.version = LuaVersion::Lua54;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("_ENV"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsNestedFunction) {
    LuaProto inner;
    inner.version = LuaVersion::Lua51;
    inner.maxStackSize = 2;
    inner.numParams = 0;
    inner.lineDefined = 3; inner.lastLineDefined = 5;
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    inner.code = {ret};
    inner.lineInfo = {4};

    LuaProto top;
    top.version = LuaVersion::Lua51;
    top.maxStackSize = 4;
    top.lineDefined = 1; top.lastLineDefined = 10;
    top.protos = {inner};
    LuaInstr ret2 = ret;
    top.code = {ret2};
    top.lineInfo = {2};

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = top;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    // Should emit a nested function
    EXPECT_NE(result.source.find("local function"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsVararg) {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;
    proto.maxStackSize = 2;
    proto.isVarArg = true;
    proto.lineDefined = 1; proto.lastLineDefined = 5;
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    proto.code = {ret};
    proto.lineInfo = {2};

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("..."), std::string::npos);
}

TEST(LuaEmitterTest, EmitsInstructionCount) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto rr = reader.read();
    ASSERT_TRUE(rr.ok);

    LuaEmitter emitter;
    auto result = emitter.emit(rr.module);
    EXPECT_NE(result.source.find("instructions: 1"), std::string::npos);
}

TEST(LuaEmitterTest, DisableFileHeader) {
    LuaModule mod;
    mod.version = LuaVersion::Lua51;

    LuaEmitOptions opts;
    opts.emitFileHeader = false;
    LuaEmitter emitter(opts);
    auto result = emitter.emit(mod);
    EXPECT_EQ(result.source.find("Generated by"), std::string::npos);
}

TEST(LuaEmitterTest, LineInfoComment) {
    auto bytes = minimalLua51();
    LuaReader reader(bytes);
    auto rr = reader.read();
    ASSERT_TRUE(rr.ok);

    LuaEmitOptions opts;
    opts.emitLineInfo = true;
    LuaEmitter emitter(opts);
    auto result = emitter.emit(rr.module);
    EXPECT_NE(result.source.find("line"), std::string::npos);
}

TEST(LuaEmitterTest, EmitsConcatInstr) {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;
    proto.maxStackSize = 4;
    LuaLocal r0; r0.name = "s"; r0.startPc = 0; r0.endPc = 5;
    LuaLocal r1; r1.name = "a"; r1.startPc = 0; r1.endPc = 5;
    LuaLocal r2; r2.name = "b"; r2.startPc = 0; r2.endPc = 5;
    proto.locals = {r0, r1, r2};
    proto.lineDefined = 0; proto.lastLineDefined = 0;

    // CONCAT A=0 B=1 C=2: s = a .. b (opcode=22)
    LuaInstr cat;
    cat.raw = 22 | (0 << 6) | (2 << 14) | (1 << 23);
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    proto.code = {cat, ret};
    proto.lineInfo = {1, 2};

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find(".."), std::string::npos);
}

TEST(LuaEmitterTest, EmitsNewTable) {
    LuaProto proto;
    proto.version = LuaVersion::Lua51;
    proto.maxStackSize = 2;
    LuaLocal r0; r0.name = "t"; r0.startPc = 0; r0.endPc = 5;
    proto.locals = {r0};
    proto.lineDefined = 0; proto.lastLineDefined = 0;

    // NEWTABLE A=0 (opcode=11)
    LuaInstr nt; nt.raw = 11;
    LuaInstr ret; ret.raw = 31 | (0 << 6) | (0 << 14) | (1 << 23);
    proto.code = {nt, ret};
    proto.lineInfo = {1, 2};

    LuaModule mod;
    mod.version = LuaVersion::Lua51;
    mod.topLevel = proto;

    LuaEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("t = {}"), std::string::npos);
}
