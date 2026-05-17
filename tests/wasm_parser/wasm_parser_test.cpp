/**
 * @file tests/wasm_parser/wasm_parser_test.cpp
 * @brief Unit tests for WasmReader and WatEmitter.
 */

#include "retdec/wasm_parser/wasm_reader.h"
#include "retdec/wasm_parser/wat_emitter.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <string>

using namespace retdec::wasm_parser;

// ─── Wasm binary builder helper ───────────────────────────────────────────────

class WasmBuilder {
public:
    void u8(uint8_t v)  { data_.push_back(v); }
    void u32le(uint32_t v) {
        data_.push_back(v & 0xFF);
        data_.push_back((v >> 8) & 0xFF);
        data_.push_back((v >> 16) & 0xFF);
        data_.push_back((v >> 24) & 0xFF);
    }
    void uleb(uint32_t v) {
        do {
            uint8_t b = v & 0x7F; v >>= 7;
            if (v) b |= 0x80;
            data_.push_back(b);
        } while (v);
    }
    void sleb(int32_t v) {
        bool more = true;
        while (more) {
            uint8_t b = v & 0x7F; v >>= 7;
            bool sign = (b & 0x40) != 0;
            if ((v == 0 && !sign) || (v == -1 && sign)) more = false;
            else b |= 0x80;
            data_.push_back(b);
        }
    }
    void str(const std::string& s) {
        uleb((uint32_t)s.size());
        for (char c : s) data_.push_back((uint8_t)c);
    }
    void magic() { u32le(0x6D736100); u32le(0x00000001); }

    // Returns the raw bytes of a section
    WasmBuilder& section(uint8_t id, const WasmBuilder& contents) {
        data_.push_back(id);
        uleb((uint32_t)contents.data_.size());
        data_.insert(data_.end(), contents.data_.begin(), contents.data_.end());
        return *this;
    }

    const std::vector<uint8_t>& bytes() const { return data_; }
private:
    std::vector<uint8_t> data_;
};

// Construct a minimal valid wasm: no types/functions, just magic+version
static std::vector<uint8_t> minimalWasm() {
    WasmBuilder b;
    b.magic();
    return b.bytes();
}

// Construct wasm with one function type () -> i32 and one function body
static std::vector<uint8_t> singleFuncWasm() {
    // Type section: [(func [] [i32])]
    WasmBuilder typeContent;
    typeContent.uleb(1);   // count
    typeContent.u8(0x60);  // functype
    typeContent.uleb(0);   // 0 params
    typeContent.uleb(1);   // 1 result
    typeContent.u8(0x7F);  // i32

    // Function section: [typeIndex=0]
    WasmBuilder funcContent;
    funcContent.uleb(1);
    funcContent.uleb(0);

    // Export section: [(func 0 "main")]
    WasmBuilder exportContent;
    exportContent.uleb(1);
    exportContent.str("main");
    exportContent.u8(0x00); // func
    exportContent.uleb(0);

    // Code section: one function body
    // body: (local) i32.const 42, end
    WasmBuilder codeContent;
    codeContent.uleb(1);     // 1 function
    WasmBuilder body;
    body.uleb(0);            // 0 local decls
    body.u8(0x41);           // i32.const
    body.sleb(42);           // 42
    body.u8(0x0B);           // end
    codeContent.uleb((uint32_t)body.bytes().size());
    for (auto by : body.bytes()) codeContent.u8(by);

    WasmBuilder full;
    full.magic();
    full.section(1, typeContent);
    full.section(3, funcContent);
    full.section(7, exportContent);
    full.section(10, codeContent);
    return full.bytes();
}

// ─── WasmReader tests ────────────────────────────────────────────────────────

TEST(WasmReaderTest, RejectsEmptyFile) {
    WasmReader reader(std::vector<uint8_t>{});
    auto result = reader.read();
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST(WasmReaderTest, RejectsBadMagic) {
    std::vector<uint8_t> bad = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x00, 0x00, 0x00};
    WasmReader reader(bad);
    auto result = reader.read();
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("magic"), std::string::npos);
}

TEST(WasmReaderTest, ParsesMinimalWasm) {
    auto bytes = minimalWasm();
    WasmReader reader(bytes);
    auto result = reader.read();
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.module.types.empty());
    EXPECT_TRUE(result.module.imports.empty());
    EXPECT_TRUE(result.module.codes.empty());
}

TEST(WasmReaderTest, ParsesSingleFunction) {
    auto bytes = singleFuncWasm();
    WasmReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;

    const WasmModule& mod = result.module;
    ASSERT_EQ(mod.types.size(), 1u);
    EXPECT_TRUE(mod.types[0].params.empty());
    ASSERT_EQ(mod.types[0].results.size(), 1u);
    EXPECT_EQ(mod.types[0].results[0], ValType::I32);

    ASSERT_EQ(mod.funcTypeIndices.size(), 1u);
    EXPECT_EQ(mod.funcTypeIndices[0], 0u);

    ASSERT_EQ(mod.exports.size(), 1u);
    EXPECT_EQ(mod.exports[0].name, "main");
    EXPECT_EQ(mod.exports[0].kind, ExternKind::Func);
    EXPECT_EQ(mod.exports[0].index, 0u);

    ASSERT_EQ(mod.codes.size(), 1u);
    EXPECT_FALSE(mod.codes[0].body.empty());
}

TEST(WasmReaderTest, FuncNameFromExport) {
    auto bytes = singleFuncWasm();
    WasmReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.module.funcName(0), "main");
}

TEST(WasmReaderTest, TotalFuncCount) {
    auto bytes = singleFuncWasm();
    WasmReader reader(bytes);
    auto result = reader.read();
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.module.totalFuncCount(), 1u);
    EXPECT_EQ(result.module.importedFuncCount(), 0u);
}

TEST(WasmReaderTest, ParsesImport) {
    // Type + Import sections
    WasmBuilder typeContent;
    typeContent.uleb(1);
    typeContent.u8(0x60); typeContent.uleb(0); typeContent.uleb(0); // () -> ()

    WasmBuilder importContent;
    importContent.uleb(1);
    importContent.str("env");
    importContent.str("print");
    importContent.u8(0x00); // func
    importContent.uleb(0);  // type index

    WasmBuilder full;
    full.magic();
    full.section(1, typeContent);
    full.section(2, importContent);

    WasmReader reader(full.bytes());
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.module.imports.size(), 1u);
    EXPECT_EQ(result.module.imports[0].module, "env");
    EXPECT_EQ(result.module.imports[0].name, "print");
    EXPECT_EQ(result.module.imports[0].kind, ExternKind::Func);
    EXPECT_EQ(result.module.importedFuncCount(), 1u);
}

TEST(WasmReaderTest, ParsesMemory) {
    WasmBuilder memContent;
    memContent.uleb(1);
    memContent.u8(0x00); // no max
    memContent.uleb(1);  // min=1

    WasmBuilder full;
    full.magic();
    full.section(5, memContent);

    WasmReader reader(full.bytes());
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.module.memories.size(), 1u);
    EXPECT_EQ(result.module.memories[0].limits.min, 1u);
    EXPECT_FALSE(result.module.memories[0].limits.max.has_value());
}

TEST(WasmReaderTest, ParsesMemoryWithMax) {
    WasmBuilder memContent;
    memContent.uleb(1);
    memContent.u8(0x01); // has max
    memContent.uleb(2);  // min=2
    memContent.uleb(8);  // max=8

    WasmBuilder full;
    full.magic();
    full.section(5, memContent);

    WasmReader reader(full.bytes());
    auto result = reader.read();
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.module.memories.size(), 1u);
    EXPECT_EQ(result.module.memories[0].limits.min, 2u);
    ASSERT_TRUE(result.module.memories[0].limits.max.has_value());
    EXPECT_EQ(*result.module.memories[0].limits.max, 8u);
}

TEST(WasmReaderTest, ParsesGlobal) {
    WasmBuilder globalContent;
    globalContent.uleb(1);
    globalContent.u8(0x7F); // i32
    globalContent.u8(0x01); // mutable
    globalContent.u8(0x41); // i32.const
    globalContent.sleb(99);
    globalContent.u8(0x0B); // end

    WasmBuilder full;
    full.magic();
    full.section(6, globalContent);

    WasmReader reader(full.bytes());
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.module.globals.size(), 1u);
    EXPECT_EQ(result.module.globals[0].type.valType, ValType::I32);
    EXPECT_TRUE(result.module.globals[0].type.isMutable);
    EXPECT_FALSE(result.module.globals[0].initExpr.empty());
}

TEST(WasmReaderTest, ParsesDataSegment) {
    WasmBuilder dataContent;
    dataContent.uleb(1);      // 1 segment
    dataContent.uleb(0);      // flags=0 (active, mem 0)
    // offset expr: i32.const 0, end
    dataContent.u8(0x41); dataContent.sleb(0); dataContent.u8(0x0B);
    dataContent.uleb(5);
    dataContent.u8('h'); dataContent.u8('e'); dataContent.u8('l');
    dataContent.u8('l'); dataContent.u8('o');

    WasmBuilder full;
    full.magic();
    full.section(11, dataContent);

    WasmReader reader(full.bytes());
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.module.dataSegments.size(), 1u);
    EXPECT_EQ(result.module.dataSegments[0].bytes.size(), 5u);
    EXPECT_EQ(result.module.dataSegments[0].bytes[0], 'h');
}

TEST(WasmReaderTest, ParsesStartSection) {
    // Need at least one function
    WasmBuilder typeContent;
    typeContent.uleb(1); typeContent.u8(0x60); typeContent.uleb(0); typeContent.uleb(0);

    WasmBuilder funcContent;
    funcContent.uleb(1); funcContent.uleb(0);

    WasmBuilder startContent;
    startContent.uleb(0); // func index 0

    WasmBuilder bodyContent;
    bodyContent.uleb(1);
    WasmBuilder body;
    body.uleb(0); body.u8(0x0B);
    bodyContent.uleb((uint32_t)body.bytes().size());
    for (auto by : body.bytes()) bodyContent.u8(by);

    WasmBuilder full;
    full.magic();
    full.section(1, typeContent);
    full.section(3, funcContent);
    full.section(8, startContent);
    full.section(10, bodyContent);

    WasmReader reader(full.bytes());
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.module.startFunc.has_value());
    EXPECT_EQ(*result.module.startFunc, 0u);
}

TEST(WasmReaderTest, ParsesTable) {
    WasmBuilder tableContent;
    tableContent.uleb(1);
    tableContent.u8(0x70); // funcref
    tableContent.u8(0x00); // no max
    tableContent.uleb(10); // min=10

    WasmBuilder full;
    full.magic();
    full.section(4, tableContent);

    WasmReader reader(full.bytes());
    auto result = reader.read();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.module.tables.size(), 1u);
    EXPECT_EQ(result.module.tables[0].refType, ValType::FuncRef);
    EXPECT_EQ(result.module.tables[0].limits.min, 10u);
}

// ─── ValType tests ────────────────────────────────────────────────────────────

TEST(ValTypeTest, Names) {
    EXPECT_EQ(valTypeStr(ValType::I32),      "i32");
    EXPECT_EQ(valTypeStr(ValType::I64),      "i64");
    EXPECT_EQ(valTypeStr(ValType::F32),      "f32");
    EXPECT_EQ(valTypeStr(ValType::F64),      "f64");
    EXPECT_EQ(valTypeStr(ValType::V128),     "v128");
    EXPECT_EQ(valTypeStr(ValType::FuncRef),  "funcref");
    EXPECT_EQ(valTypeStr(ValType::ExternRef),"externref");
}

// ─── WatEmitter tests ─────────────────────────────────────────────────────────

TEST(WatEmitterTest, EmitsModuleWrapper) {
    WasmModule mod;
    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("(module"), std::string::npos);
    EXPECT_NE(result.source.find(")"), std::string::npos);
}

TEST(WatEmitterTest, EmitsTypeSection) {
    WasmModule mod;
    FuncType ft;
    ft.params  = {ValType::I32, ValType::I64};
    ft.results = {ValType::F32};
    mod.types.push_back(ft);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("(type"), std::string::npos);
    EXPECT_NE(result.source.find("(param i32)"), std::string::npos);
    EXPECT_NE(result.source.find("(param i64)"), std::string::npos);
    EXPECT_NE(result.source.find("(result f32)"), std::string::npos);
}

TEST(WatEmitterTest, EmitsImportInlineMode) {
    WasmModule mod;
    FuncType ft; mod.types.push_back(ft); // () -> ()

    Import imp;
    imp.module = "env"; imp.name = "log"; imp.kind = ExternKind::Func;
    imp.index  = 0;
    mod.imports.push_back(imp);

    WatEmitOptions opts;
    opts.inlineImports = true;
    WatEmitter emitter(opts);
    auto result = emitter.emit(mod);

    EXPECT_NE(result.source.find("(import"), std::string::npos);
    EXPECT_NE(result.source.find("\"env\""), std::string::npos);
    EXPECT_NE(result.source.find("\"log\""), std::string::npos);
}

TEST(WatEmitterTest, EmitsExportAnnotation) {
    WasmModule mod;
    FuncType ft; ft.results = {ValType::I32};
    mod.types.push_back(ft);
    mod.funcTypeIndices.push_back(0);

    Export exp; exp.name = "main"; exp.kind = ExternKind::Func; exp.index = 0;
    mod.exports.push_back(exp);

    // Code: i32.const 1, end
    FuncCode code;
    code.body = {0x41, 0x01, 0x0B};
    mod.codes.push_back(code);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("\"main\""), std::string::npos);
    EXPECT_NE(result.source.find("(func"), std::string::npos);
}

TEST(WatEmitterTest, EmitsMemory) {
    WasmModule mod;
    MemType mt; mt.limits.min = 1;
    mod.memories.push_back(mt);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("(memory"), std::string::npos);
    EXPECT_NE(result.source.find("1"), std::string::npos);
}

TEST(WatEmitterTest, EmitsGlobal) {
    WasmModule mod;
    WasmGlobal g;
    g.type.valType = ValType::I32; g.type.isMutable = true;
    g.initExpr = {0x41, 0x00, 0x0B}; // i32.const 0
    mod.globals.push_back(g);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("(global"), std::string::npos);
    EXPECT_NE(result.source.find("(mut i32)"), std::string::npos);
}

TEST(WatEmitterTest, EmitsDataSegment) {
    WasmModule mod;
    DataSegment seg;
    seg.bytes = {'h', 'i'};
    seg.offsetExpr = {0x41, 0x00, 0x0B};
    mod.dataSegments.push_back(seg);
    // Add memory so it's valid
    MemType mt; mt.limits.min = 1; mod.memories.push_back(mt);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("(data"), std::string::npos);
    EXPECT_NE(result.source.find("hi"), std::string::npos);
}

TEST(WatEmitterTest, EmitsStartFunction) {
    WasmModule mod;
    FuncType ft; mod.types.push_back(ft);
    mod.funcTypeIndices.push_back(0);
    mod.startFunc = 0;
    FuncCode code; code.body = {0x0B}; mod.codes.push_back(code);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("(start"), std::string::npos);
}

TEST(WatEmitterTest, DisassemblesFuncBody) {
    auto bytes = singleFuncWasm();
    WasmReader reader(bytes);
    auto rr = reader.read();
    ASSERT_TRUE(rr.ok);

    WatEmitter emitter;
    auto result = emitter.emit(rr.module);
    EXPECT_NE(result.source.find("i32.const"), std::string::npos);
    EXPECT_NE(result.source.find("42"), std::string::npos);
}

TEST(WatEmitterTest, EmitsFuncNameFromExport) {
    auto bytes = singleFuncWasm();
    WasmReader reader(bytes);
    auto rr = reader.read();
    ASSERT_TRUE(rr.ok);

    WatEmitOptions opts; opts.useNames = true;
    WatEmitter emitter(opts);
    auto result = emitter.emit(rr.module);
    EXPECT_NE(result.source.find("$main"), std::string::npos);
}

TEST(WatEmitterTest, EmitsConstExprI32) {
    WasmModule mod;
    WasmGlobal g;
    g.type.valType = ValType::I32;
    // i32.const 77 — SLEB128 encoding of 77: 0xCD 0x00 (two bytes, positive)
    g.initExpr = {0x41, 0xCD, 0x00, 0x0B};
    mod.globals.push_back(g);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("77"), std::string::npos);
}

TEST(WatEmitterTest, DataStrEscaping) {
    WasmModule mod;
    DataSegment seg;
    seg.bytes = {'"', '\\', 0x00, 0x1F, 0x7E};
    seg.offsetExpr = {0x41, 0x00, 0x0B};
    mod.dataSegments.push_back(seg);
    MemType mt; mt.limits.min = 1; mod.memories.push_back(mt);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("\\\\"), std::string::npos);
    EXPECT_NE(result.source.find("\\\""), std::string::npos);
}

TEST(WatEmitterTest, EmitsTable) {
    WasmModule mod;
    TableType tt; tt.refType = ValType::FuncRef; tt.limits.min = 5;
    mod.tables.push_back(tt);

    WatEmitter emitter;
    auto result = emitter.emit(mod);
    EXPECT_NE(result.source.find("(table"), std::string::npos);
    EXPECT_NE(result.source.find("funcref"), std::string::npos);
    EXPECT_NE(result.source.find("5"), std::string::npos);
}

TEST(WatEmitterTest, FullRoundTrip) {
    auto bytes = singleFuncWasm();
    WasmReader reader(bytes);
    auto rr = reader.read();
    ASSERT_TRUE(rr.ok) << rr.error;

    WatEmitter emitter;
    auto result = emitter.emit(rr.module);
    EXPECT_FALSE(result.source.empty());
    EXPECT_NE(result.source.find("(module"), std::string::npos);
    EXPECT_NE(result.source.find("(func"), std::string::npos);
    EXPECT_NE(result.source.find("(type"), std::string::npos);
    EXPECT_NE(result.source.find("i32.const"), std::string::npos);
}
