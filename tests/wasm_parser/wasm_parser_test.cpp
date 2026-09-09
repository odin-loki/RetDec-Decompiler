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
	void u8(uint8_t v)
	{
		data_.push_back(v);
	}
	void u32le(uint32_t v)
	{
		data_.push_back(v & 0xFF);
		data_.push_back((v >> 8) & 0xFF);
		data_.push_back((v >> 16) & 0xFF);
		data_.push_back((v >> 24) & 0xFF);
	}
	void uleb(uint32_t v)
	{
		do
		{
			uint8_t b = v & 0x7F;
			v >>= 7;
			if (v) b |= 0x80;
			data_.push_back(b);
		}
		while (v);
	}
	void sleb(int32_t v)
	{
		bool more = true;
		while (more)
		{
			uint8_t b = v & 0x7F;
			v >>= 7;
			bool sign = (b & 0x40) != 0;
			if ((v == 0 && !sign) || (v == -1 && sign))
				more = false;
			else
				b |= 0x80;
			data_.push_back(b);
		}
	}
	void str(const std::string& s)
	{
		uleb((uint32_t)s.size());
		for (char c: s)
			data_.push_back((uint8_t)c);
	}
	void magic()
	{
		u32le(0x6D736100);
		u32le(0x00000001);
	}

	// Returns the raw bytes of a section
	WasmBuilder& section(uint8_t id, const WasmBuilder& contents)
	{
		data_.push_back(id);
		uleb((uint32_t)contents.data_.size());
		data_.insert(data_.end(), contents.data_.begin(), contents.data_.end());
		return *this;
	}

	const std::vector<uint8_t>& bytes() const
	{
		return data_;
	}

private:
	std::vector<uint8_t> data_;
};

// Construct a minimal valid wasm: no types/functions, just magic+version
static std::vector<uint8_t> minimalWasm()
{
	WasmBuilder b;
	b.magic();
	return b.bytes();
}

// Construct wasm with one function type () -> i32 and one function body
static std::vector<uint8_t> singleFuncWasm()
{
	// Type section: [(func [] [i32])]
	WasmBuilder typeContent;
	typeContent.uleb(1);  // count
	typeContent.u8(0x60); // functype
	typeContent.uleb(0);  // 0 params
	typeContent.uleb(1);  // 1 result
	typeContent.u8(0x7F); // i32

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
	codeContent.uleb(1); // 1 function
	WasmBuilder body;
	body.uleb(0);  // 0 local decls
	body.u8(0x41); // i32.const
	body.sleb(42); // 42
	body.u8(0x0B); // end
	codeContent.uleb((uint32_t)body.bytes().size());
	for (auto by: body.bytes())
		codeContent.u8(by);

	WasmBuilder full;
	full.magic();
	full.section(1, typeContent);
	full.section(3, funcContent);
	full.section(7, exportContent);
	full.section(10, codeContent);
	return full.bytes();
}

// ─── WasmReader tests ────────────────────────────────────────────────────────

TEST(WasmReaderTest, RejectsEmptyFile)
{
	WasmReader reader(std::vector<uint8_t>{});
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_FALSE(result.error.empty());
}

TEST(WasmReaderTest, RejectsBadMagic)
{
	std::vector<uint8_t> bad = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x00, 0x00, 0x00};
	WasmReader reader(bad);
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_NE(result.error.find("magic"), std::string::npos);
}

TEST(WasmReaderTest, ParsesMinimalWasm)
{
	auto bytes = minimalWasm();
	WasmReader reader(bytes);
	auto result = reader.read();
	EXPECT_TRUE(result.ok) << result.error;
	EXPECT_TRUE(result.module.types.empty());
	EXPECT_TRUE(result.module.imports.empty());
	EXPECT_TRUE(result.module.codes.empty());
}

TEST(WasmReaderTest, ParsesSingleFunction)
{
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

TEST(WasmReaderTest, FuncNameFromExport)
{
	auto bytes = singleFuncWasm();
	WasmReader reader(bytes);
	auto result = reader.read();
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.module.funcName(0), "main");
}

TEST(WasmReaderTest, TotalFuncCount)
{
	auto bytes = singleFuncWasm();
	WasmReader reader(bytes);
	auto result = reader.read();
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.module.totalFuncCount(), 1u);
	EXPECT_EQ(result.module.importedFuncCount(), 0u);
}

TEST(WasmReaderTest, ParsesImport)
{
	// Type + Import sections
	WasmBuilder typeContent;
	typeContent.uleb(1);
	typeContent.u8(0x60);
	typeContent.uleb(0);
	typeContent.uleb(0); // () -> ()

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

TEST(WasmReaderTest, ParsesMemory)
{
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

TEST(WasmReaderTest, ParsesMemoryWithMax)
{
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

TEST(WasmReaderTest, ParsesGlobal)
{
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

TEST(WasmReaderTest, ParsesDataSegment)
{
	WasmBuilder dataContent;
	dataContent.uleb(1); // 1 segment
	dataContent.uleb(0); // flags=0 (active, mem 0)
	// offset expr: i32.const 0, end
	dataContent.u8(0x41);
	dataContent.sleb(0);
	dataContent.u8(0x0B);
	dataContent.uleb(5);
	dataContent.u8('h');
	dataContent.u8('e');
	dataContent.u8('l');
	dataContent.u8('l');
	dataContent.u8('o');

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

TEST(WasmReaderTest, ParsesStartSection)
{
	// Need at least one function
	WasmBuilder typeContent;
	typeContent.uleb(1);
	typeContent.u8(0x60);
	typeContent.uleb(0);
	typeContent.uleb(0);

	WasmBuilder funcContent;
	funcContent.uleb(1);
	funcContent.uleb(0);

	WasmBuilder startContent;
	startContent.uleb(0); // func index 0

	WasmBuilder bodyContent;
	bodyContent.uleb(1);
	WasmBuilder body;
	body.uleb(0);
	body.u8(0x0B);
	bodyContent.uleb((uint32_t)body.bytes().size());
	for (auto by: body.bytes())
		bodyContent.u8(by);

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

TEST(WasmReaderTest, ParsesTable)
{
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

// ─── Malformed-input tests ───────────────────────────────────────────────────

// An element segment (flags=4) whose element-expression vector claims far more
// entries than the file has bytes. The count used to be trusted as a loop
// bound, and readConstExpr returned an empty expression at end of input rather
// than failing, so the loop appended ~268M vectors and exhausted memory.
TEST(WasmReaderTest, RejectsElementSegmentExprCountBeyondInput)
{
	WasmBuilder elemContent;
	elemContent.uleb(1); // 1 segment
	elemContent.uleb(4); // flags=4: active, table 0, elem exprs
	elemContent.u8(0x41);
	elemContent.sleb(0);
	elemContent.u8(0x0B);         // offset
	elemContent.uleb(0x0FFFFFFF); // element count, nothing follows it

	WasmBuilder full;
	full.magic();
	full.section(9, elemContent);

	WasmReader reader(full.bytes());
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_FALSE(result.error.empty());
	EXPECT_TRUE(result.module.elements.empty());
}

// The same shape with a count small enough to allocate: before the fix this
// parsed "successfully" into 4096 empty expressions read from zero bytes, which
// is the behaviour that scales into the out-of-memory above.
TEST(WasmReaderTest, ElementSegmentExprCountIsBoundedByInput)
{
	WasmBuilder elemContent;
	elemContent.uleb(1); // 1 segment
	elemContent.uleb(4); // flags=4: active, table 0, elem exprs
	elemContent.u8(0x41);
	elemContent.sleb(0);
	elemContent.u8(0x0B);   // offset
	elemContent.uleb(4096); // element count, nothing follows it

	WasmBuilder full;
	full.magic();
	full.section(9, elemContent);

	WasmReader reader(full.bytes());
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(result.module.elements.empty());
}

// Same wrong assumption in the func-index variant of the element section.
TEST(WasmReaderTest, RejectsElementSegmentFuncCountBeyondInput)
{
	WasmBuilder elemContent;
	elemContent.uleb(1); // 1 segment
	elemContent.uleb(0); // flags=0: active, table 0, func indices
	elemContent.u8(0x41);
	elemContent.sleb(0);
	elemContent.u8(0x0B);         // offset
	elemContent.uleb(0x0FFFFFFF); // func index count, nothing follows it

	WasmBuilder full;
	full.magic();
	full.section(9, elemContent);

	WasmReader reader(full.bytes());
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(result.module.elements.empty());
}

// A section-level vector count that no section body could supply.
TEST(WasmReaderTest, RejectsSectionVectorCountBeyondInput)
{
	WasmBuilder typeContent;
	typeContent.uleb(0x0FFFFFFF); // functype count, nothing follows it

	WasmBuilder full;
	full.magic();
	full.section(1, typeContent);

	WasmReader reader(full.bytes());
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(result.module.types.empty());
}

// A functype carries its param/result vectors with no length prefix of their
// own, so they are bounded by the rest of the file.
TEST(WasmReaderTest, RejectsFuncTypeParamCountBeyondInput)
{
	WasmBuilder typeContent;
	typeContent.uleb(1);
	typeContent.u8(0x60);
	typeContent.uleb(0x0FFFFFFF); // param count
	typeContent.uleb(0);

	WasmBuilder full;
	full.magic();
	full.section(1, typeContent);

	WasmReader reader(full.bytes());
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(result.module.types.empty());
}

// A function body whose local-declaration vector is larger than the body.
TEST(WasmReaderTest, RejectsCodeLocalCountBeyondBody)
{
	WasmBuilder body;
	body.uleb(0x0FFFFFFF); // local decl count
	body.u8(0x0B);

	WasmBuilder codeContent;
	codeContent.uleb(1);
	codeContent.uleb((uint32_t)body.bytes().size());
	for (auto by: body.bytes())
		codeContent.u8(by);

	WasmBuilder full;
	full.magic();
	full.section(10, codeContent);

	WasmReader reader(full.bytes());
	auto result = reader.read();
	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(result.module.codes.empty());
}

// The name section is best-effort and must not fail the module, but an
// impossible name count must not be looped over either.
TEST(WasmReaderTest, ClampsNameSectionCountBeyondInput)
{
	WasmBuilder sub;
	sub.uleb(0x0FFFFFFF); // func name count, nothing follows it

	WasmBuilder customContent;
	customContent.str("name");
	customContent.u8(1); // func names subsection
	customContent.uleb((uint32_t)sub.bytes().size());
	for (auto by: sub.bytes())
		customContent.u8(by);

	WasmBuilder full;
	full.magic();
	full.section(0, customContent);

	WasmReader reader(full.bytes());
	auto result = reader.read();
	EXPECT_TRUE(result.ok) << result.error;
	ASSERT_TRUE(result.module.names.has_value());
	EXPECT_TRUE(result.module.names->funcNames.empty());
}

// ─── ValType tests ────────────────────────────────────────────────────────────

TEST(ValTypeTest, Names)
{
	EXPECT_EQ(valTypeStr(ValType::I32), "i32");
	EXPECT_EQ(valTypeStr(ValType::I64), "i64");
	EXPECT_EQ(valTypeStr(ValType::F32), "f32");
	EXPECT_EQ(valTypeStr(ValType::F64), "f64");
	EXPECT_EQ(valTypeStr(ValType::V128), "v128");
	EXPECT_EQ(valTypeStr(ValType::FuncRef), "funcref");
	EXPECT_EQ(valTypeStr(ValType::ExternRef), "externref");
}

// ─── WatEmitter tests ─────────────────────────────────────────────────────────

TEST(WatEmitterTest, EmitsModuleWrapper)
{
	WasmModule mod;
	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("(module"), std::string::npos);
	EXPECT_NE(result.source.find(")"), std::string::npos);
}

TEST(WatEmitterTest, EmitsTypeSection)
{
	WasmModule mod;
	FuncType ft;
	ft.params = {ValType::I32, ValType::I64};
	ft.results = {ValType::F32};
	mod.types.push_back(ft);

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("(type"), std::string::npos);
	EXPECT_NE(result.source.find("(param i32)"), std::string::npos);
	EXPECT_NE(result.source.find("(param i64)"), std::string::npos);
	EXPECT_NE(result.source.find("(result f32)"), std::string::npos);
}

TEST(WatEmitterTest, EmitsImportInlineMode)
{
	WasmModule mod;
	FuncType ft;
	mod.types.push_back(ft); // () -> ()

	Import imp;
	imp.module = "env";
	imp.name = "log";
	imp.kind = ExternKind::Func;
	imp.index = 0;
	mod.imports.push_back(imp);

	WatEmitOptions opts;
	opts.inlineImports = true;
	WatEmitter emitter(opts);
	auto result = emitter.emit(mod);

	EXPECT_NE(result.source.find("(import"), std::string::npos);
	EXPECT_NE(result.source.find("\"env\""), std::string::npos);
	EXPECT_NE(result.source.find("\"log\""), std::string::npos);
}

TEST(WatEmitterTest, EmitsExportAnnotation)
{
	WasmModule mod;
	FuncType ft;
	ft.results = {ValType::I32};
	mod.types.push_back(ft);
	mod.funcTypeIndices.push_back(0);

	Export exp;
	exp.name = "main";
	exp.kind = ExternKind::Func;
	exp.index = 0;
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

TEST(WatEmitterTest, EmitsMemory)
{
	WasmModule mod;
	MemType mt;
	mt.limits.min = 1;
	mod.memories.push_back(mt);

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("(memory"), std::string::npos);
	EXPECT_NE(result.source.find("1"), std::string::npos);
}

TEST(WatEmitterTest, EmitsGlobal)
{
	WasmModule mod;
	WasmGlobal g;
	g.type.valType = ValType::I32;
	g.type.isMutable = true;
	g.initExpr = {0x41, 0x00, 0x0B}; // i32.const 0
	mod.globals.push_back(g);

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("(global"), std::string::npos);
	EXPECT_NE(result.source.find("(mut i32)"), std::string::npos);
}

TEST(WatEmitterTest, EmitsDataSegment)
{
	WasmModule mod;
	DataSegment seg;
	seg.bytes = {'h', 'i'};
	seg.offsetExpr = {0x41, 0x00, 0x0B};
	mod.dataSegments.push_back(seg);
	// Add memory so it's valid
	MemType mt;
	mt.limits.min = 1;
	mod.memories.push_back(mt);

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("(data"), std::string::npos);
	EXPECT_NE(result.source.find("hi"), std::string::npos);
}

TEST(WatEmitterTest, EmitsStartFunction)
{
	WasmModule mod;
	FuncType ft;
	mod.types.push_back(ft);
	mod.funcTypeIndices.push_back(0);
	mod.startFunc = 0;
	FuncCode code;
	code.body = {0x0B};
	mod.codes.push_back(code);

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("(start"), std::string::npos);
}

TEST(WatEmitterTest, DisassemblesFuncBody)
{
	auto bytes = singleFuncWasm();
	WasmReader reader(bytes);
	auto rr = reader.read();
	ASSERT_TRUE(rr.ok);

	WatEmitter emitter;
	auto result = emitter.emit(rr.module);
	EXPECT_NE(result.source.find("i32.const"), std::string::npos);
	EXPECT_NE(result.source.find("42"), std::string::npos);
}

// emitFuncBody used to sit inside the `typeIdx < mod.types.size()` guard, so a
// Function section naming a type index the Type section does not have dropped
// the whole disassembly of that function -- silently, with no marker in the
// output to say anything had been left out. A module malformed enough to have
// one is exactly when the instruction listing is worth reading.
TEST(WatEmitterTest, AnOutOfRangeTypeIndexStillDisassemblesTheBody)
{
	auto bytes = singleFuncWasm();
	WasmReader reader(bytes);
	auto rr = reader.read();
	ASSERT_TRUE(rr.ok) << rr.error;
	ASSERT_EQ(1u, rr.module.funcTypeIndices.size());

	WasmModule mod = rr.module;
	mod.funcTypeIndices[0] = static_cast<uint32_t>(mod.types.size()) + 7;

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("i32.const"), std::string::npos)
		<< "the body bytes do not depend on the type index:\n" << result.source;
	EXPECT_NE(result.source.find("out of range"), std::string::npos)
		<< "and dropping the signature should say so:\n" << result.source;
}

TEST(WatEmitterTest, EmitsFuncNameFromExport)
{
	auto bytes = singleFuncWasm();
	WasmReader reader(bytes);
	auto rr = reader.read();
	ASSERT_TRUE(rr.ok);

	WatEmitOptions opts;
	opts.useNames = true;
	WatEmitter emitter(opts);
	auto result = emitter.emit(rr.module);
	EXPECT_NE(result.source.find("$main"), std::string::npos);
}

TEST(WatEmitterTest, EmitsConstExprI32)
{
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

TEST(WatEmitterTest, DataStrEscaping)
{
	WasmModule mod;
	DataSegment seg;
	seg.bytes = {'"', '\\', 0x00, 0x1F, 0x7E};
	seg.offsetExpr = {0x41, 0x00, 0x0B};
	mod.dataSegments.push_back(seg);
	MemType mt;
	mt.limits.min = 1;
	mod.memories.push_back(mt);

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("\\\\"), std::string::npos);
	EXPECT_NE(result.source.find("\\\""), std::string::npos);
}

TEST(WatEmitterTest, EmitsTable)
{
	WasmModule mod;
	TableType tt;
	tt.refType = ValType::FuncRef;
	tt.limits.min = 5;
	mod.tables.push_back(tt);

	WatEmitter emitter;
	auto result = emitter.emit(mod);
	EXPECT_NE(result.source.find("(table"), std::string::npos);
	EXPECT_NE(result.source.find("funcref"), std::string::npos);
	EXPECT_NE(result.source.find("5"), std::string::npos);
}

TEST(WatEmitterTest, FullRoundTrip)
{
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

// ─── LEB128 shift bounds ─────────────────────────────────────────────────────

// The signed readers accumulated into the signed result type with no bound on
// the shift. That is undefined behaviour twice over on a hostile module:
// (int32_t)0x7F << 28 already overflows the signed range, and a run of
// continuation bytes drives the shift past the type's width. The name-section
// reader had no bound at all -- a fuzzer run with a larger -max_len shifted a
// uint32_t by 35.
TEST(WasmLeb128, SignedReadersSurviveAContinuationRun)
{
	// A module truncated after a run of continuation bytes: whatever the reader
	// returns, it must not be undefined behaviour getting there.
	std::vector<uint8_t> mod = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
	for (int i = 0; i < 40; ++i)
		mod.push_back(0xFF);

	WasmReader reader(mod.data(), mod.size());
	EXPECT_NO_THROW((void)reader.read());
}

// Value correctness for these encodings is covered where the shared helpers
// live -- tests/bounds carries the DWARF standard's LEB128 vectors and
// tests/verification proves the shift bounds. The readers here are private, so
// what this suite can check is that a hostile module does not reach undefined
// behaviour through the public entry point.
TEST(WasmLeb128, NameSectionContinuationRunIsBounded)
{
	// Minimised from the fuzzer's reproducer. A custom section actually named
	// "name" -- which is what routes it to parseNameSection -- whose first
	// subsection length is a run of continuation bytes. Twelve of them drive
	// readULEB_local's shift to 35 on a uint32_t, which UBSan reports as
	// "shift exponent 35 is too large for 32-bit type". 28 bytes total.
	const std::vector<uint8_t> mod = {
		0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, // magic, version
		0x00,                                           // custom section
		0x12,                                           // payload length 18
		0x04, 'n',  'a',  'm',  'e',                    // section name
		0x00,                                           // subsection id 0
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,             // continuation run
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	};

	WasmReader reader(mod.data(), mod.size());
	EXPECT_NO_THROW((void)reader.read());
}

// The emitter had four more hand-rolled LEB128 readers, in
// WatEmitter::constExprStr -- i32.const, i64.const, ref.func and global.get.
// The two signed ones accumulated into an int, so a run of continuation bytes
// overflowed the signed range before the shift count ever became the problem:
//
//   src/wasm_parser/wat_emitter.cpp:72: runtime error: left shift of 127 by 28
//   places cannot be represented in type 'int'
//
// constExprStr is private, so this drives it through emit() with a module built
// by hand -- which is also the reachable path, since the init expression of a
// global comes straight from the file.
TEST(WatEmitterLeb128, ConstExpressionsSurviveAContinuationRun)
{
	for (uint8_t op: {uint8_t(0x41), uint8_t(0x42), uint8_t(0xD2), uint8_t(0x23)})
	{
		WasmModule mod;

		WasmGlobal g;
		g.initExpr.push_back(op);
		for (int i = 0; i < 12; ++i)
			g.initExpr.push_back(0xFF);
		g.initExpr.push_back(0x00);
		mod.globals.push_back(g);

		DataSegment d;
		d.offsetExpr = g.initExpr;
		mod.dataSegments.push_back(d);

		WatEmitter em;
		EXPECT_NO_THROW((void)em.emit(mod)) << "opcode 0x" << std::hex << int(op);
	}
}

// And a run long enough that the encoding cannot denote a 64-bit value at all
// is rejected rather than silently truncated to whatever fitted.
TEST(WatEmitterLeb128, AnOverlongConstExpressionDoesNotProduceAValue)
{
	WasmModule mod;
	WasmGlobal g;
	g.initExpr.push_back(0x41);
	for (int i = 0; i < 20; ++i)
		g.initExpr.push_back(0xFF);
	g.initExpr.push_back(0x7F);
	mod.globals.push_back(g);

	WatEmitter em;
	const auto res = em.emit(mod);
	EXPECT_NE(res.source.find("i32.const 0"), std::string::npos) << res.source;
}

// The same defect twenty lines further down the same file, in the function-body
// disassembler rather than in constExprStr. readVecULEB / readVecSLEB /
// readVecSLEB64 bounded the cursor and not the shift count, and the two signed
// ones accumulated into int32_t and int64_t, where (int32_t)0x7F << 28 is
// already outside the signed range. Measured on this shape under
// -fsanitize=undefined, against the code as it stood:
//
//   src/wasm_parser/wat_emitter.cpp:534: runtime error: left shift of 127 by 28
//   places cannot be represented in type 'int'
//
// The three functions are static, so this drives them the only way a file can:
// a code section whose body carries the run. That is also the reachable path.
namespace {

/// A module with one () -> () function whose body is @p body.
std::vector<uint8_t> moduleWithBody(const std::vector<uint8_t>& body)
{
	auto uleb = [](std::vector<uint8_t>& v, uint64_t x) {
		do
		{
			uint8_t b = x & 0x7F;
			x >>= 7;
			if (x) b |= 0x80;
			v.push_back(b);
		}
		while (x);
	};
	auto section = [&uleb](std::vector<uint8_t>& m, uint8_t id, const std::vector<uint8_t>& payload) {
		m.push_back(id);
		uleb(m, payload.size());
		m.insert(m.end(), payload.begin(), payload.end());
	};

	std::vector<uint8_t> m = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};

	std::vector<uint8_t> types;
	uleb(types, 1);
	types.push_back(0x60); // func
	uleb(types, 0);        // no params
	uleb(types, 0);        // no results
	section(m, 0x01, types);

	std::vector<uint8_t> funcs;
	uleb(funcs, 1);
	uleb(funcs, 0); // type 0
	section(m, 0x03, funcs);

	std::vector<uint8_t> code;
	uleb(code, 1);
	uleb(code, body.size());
	code.insert(code.end(), body.begin(), body.end());
	section(m, 0x0A, code);

	return m;
}

/// @p opcode followed by @p runLength continuation bytes and a terminator,
/// wrapped as a function body with no locals.
std::vector<uint8_t> bodyWithRun(uint8_t opcode, int runLength)
{
	std::vector<uint8_t> body;
	body.push_back(0x00); // zero local declarations
	body.push_back(opcode);
	for (int i = 0; i < runLength; ++i)
		body.push_back(0xFF);
	body.push_back(0x00);
	body.push_back(0x0B); // end
	return body;
}

} // namespace

TEST(WatEmitterLeb128, FunctionBodyImmediatesSurviveAContinuationRun)
{
	// 0x41 i32.const (signed 32), 0x42 i64.const (signed 64), 0x0C br and
	// 0x10 call (unsigned) -- one opcode for each of the three readers.
	for (uint8_t op: {uint8_t(0x41), uint8_t(0x42), uint8_t(0x0C), uint8_t(0x10)})
	{
		const auto image = moduleWithBody(bodyWithRun(op, 21));

		WasmReader reader(image);
		const auto parsed = reader.read();
		ASSERT_TRUE(parsed.ok) << "opcode 0x" << std::hex << int(op) << ": " << parsed.error;

		WatEmitter em;
		EXPECT_NO_THROW((void)em.emit(parsed.module)) << "opcode 0x" << std::hex << int(op);
	}
}

// A run the encoding cannot denote reads as 0, the same answer constExprStr
// gives, rather than as whatever fitted before the shift ran off the width.
TEST(WatEmitterLeb128, AnOverlongFunctionBodyImmediateDoesNotProduceAValue)
{
	const auto image = moduleWithBody(bodyWithRun(0x41, 20));

	WasmReader reader(image);
	const auto parsed = reader.read();
	ASSERT_TRUE(parsed.ok) << parsed.error;

	WatEmitter em;
	const auto res = em.emit(parsed.module);
	EXPECT_NE(res.source.find("i32.const 0"), std::string::npos) << res.source;
}

// What the change must not cost: a well-formed body still disassembles to the
// values it encodes.
TEST(WatEmitterLeb128, WellFormedFunctionBodyImmediatesStillRead)
{
	std::vector<uint8_t> body;
	body.push_back(0x00); // zero local declarations
	body.push_back(0x41); // i32.const
	body.push_back(0xC0); // -64, two-byte SLEB128
	body.push_back(0x7F); //
	body.push_back(0x41); // i32.const
	body.push_back(0xE5); // 101, two-byte SLEB128
	body.push_back(0x00); //
	body.push_back(0x0B); // end

	const auto image = moduleWithBody(body);
	WasmReader reader(image);
	const auto parsed = reader.read();
	ASSERT_TRUE(parsed.ok) << parsed.error;

	WatEmitter em;
	const auto res = em.emit(parsed.module);
	EXPECT_NE(res.source.find("i32.const -64"), std::string::npos) << res.source;
	EXPECT_NE(res.source.find("i32.const 101"), std::string::npos) << res.source;
}

// A memarg's `align` is the alignment EXPONENT, a u32 straight out of the
// function body (WebAssembly Core 1.0, 5.4.6 "Memory Instructions"), and it was
// used as `1u << align`. Anything from 32 up is undefined. Measured on this
// shape under -fsanitize=undefined, against the code as it stood:
//
//   src/wasm_parser/wat_emitter.cpp:606: runtime error: shift exponent 40 is
//   too large for 32-bit type 'unsigned int'
//
// Found while fixing the three body readers above -- the value comes out of
// readVecULEB, so bounding those readers did not bound this.
TEST(WatEmitterMemArg, AnAlignmentExponentTooLargeToShiftIsNotShifted)
{
	std::vector<uint8_t> body;
	body.push_back(0x00); // zero local declarations
	body.push_back(0x28); // i32.load, which takes a memarg
	body.push_back(0x28); // align exponent = 40, far past the 32-bit width
	body.push_back(0x00); // offset = 0
	body.push_back(0x0B); // end

	const auto image = moduleWithBody(body);
	WasmReader reader(image);
	const auto parsed = reader.read();
	ASSERT_TRUE(parsed.ok) << parsed.error;

	WatEmitter em;
	const auto res = em.emit(parsed.module);
	// The exponent is what the file said, so that is what the text carries.
	EXPECT_NE(res.source.find("align=2^40"), std::string::npos) << res.source;
}

// The ordinary case must still print the alignment in bytes.
TEST(WatEmitterMemArg, AWellFormedAlignmentIsStillPrintedInBytes)
{
	std::vector<uint8_t> body;
	body.push_back(0x00); // zero local declarations
	body.push_back(0x28); // i32.load
	body.push_back(0x02); // align exponent 2 -> 4 bytes, the natural alignment
	body.push_back(0x08); // offset = 8
	body.push_back(0x0B); // end

	const auto image = moduleWithBody(body);
	WasmReader reader(image);
	const auto parsed = reader.read();
	ASSERT_TRUE(parsed.ok) << parsed.error;

	WatEmitter em;
	const auto res = em.emit(parsed.module);
	EXPECT_NE(res.source.find("offset=8"), std::string::npos) << res.source;
	EXPECT_NE(res.source.find("align=4"), std::string::npos) << res.source;
}

// ─── Declared counts that exceed what the input can supply ───────────────────
//
// Two quantities in a function used to be trusted as loop bounds without a
// ceiling: a br_table label count and a local group's repetition count. Both
// are attacker-chosen 32-bit numbers reachable from a few bytes, so both let a
// module ask for output no input of that size could justify.

TEST(WatEmitterDeclaredCounts, ABrTableReadsNoMoreLabelsThanTheBodyCanHold)
{
	WasmModule mod;
	FuncType ft;
	mod.types.push_back(ft);
	mod.funcTypeIndices.push_back(0);

	// br_table with a label count of 1,000,000 followed by a single byte.
	// Every label is a LEB128 of at least one byte, so at most one of them
	// is actually present.
	FuncCode code;
	code.body = {0x0E, 0xC0, 0x84, 0x3D, 0x0B};
	mod.codes.push_back(code);

	WatEmitter emitter;
	auto result = emitter.emit(mod);

	// Unbounded, the loop appends a label per declared count: about two
	// megabytes of " 0" from five bytes of input.
	EXPECT_LT(result.source.size(), 4096u);
	EXPECT_NE(result.source.find("br_table"), std::string::npos);
	EXPECT_NE(result.source.find("more label(s) declared than the body can hold"), std::string::npos);
}

TEST(WatEmitterDeclaredCounts, AWellFormedBrTablePrintsEveryLabel)
{
	WasmModule mod;
	FuncType ft;
	mod.types.push_back(ft);
	mod.funcTypeIndices.push_back(0);

	// br_table 2 -> three labels (7, 8, and the default 9), all present.
	FuncCode code;
	code.body = {0x0E, 0x02, 0x07, 0x08, 0x09, 0x0B};
	mod.codes.push_back(code);

	WatEmitter emitter;
	auto result = emitter.emit(mod);

	EXPECT_NE(result.source.find("br_table 7 8 9"), std::string::npos);
	EXPECT_EQ(result.source.find("more label(s) declared"), std::string::npos);
}

TEST(WatEmitterDeclaredCounts, ALocalGroupDoesNotExpandPastTheEmissionLimit)
{
	WasmModule mod;
	FuncType ft;
	mod.types.push_back(ft);
	mod.funcTypeIndices.push_back(0);

	// A local group's count is a repetition multiplier, not a vector of
	// encoded elements, so six bytes on the wire declare as many locals as a
	// uint32_t can hold.
	FuncCode code;
	WasmLocal lc;
	lc.count = 1000000;
	lc.type = ValType::I32;
	code.locals.push_back(lc);
	code.body = {0x0B};
	mod.codes.push_back(code);

	WatEmitter emitter;
	auto result = emitter.emit(mod);

	// One "(local ...)" line each would be some twenty megabytes here, and
	// grows without limit as the declared count does.
	EXPECT_LT(result.source.size(), 2u * 1024u * 1024u);
	EXPECT_NE(result.source.find("further i32 local(s) declared, past the emission limit"), std::string::npos);
}

TEST(WatEmitterDeclaredCounts, ALocalGroupWithinTheLimitIsStillExpandedInFull)
{
	WasmModule mod;
	FuncType ft;
	mod.types.push_back(ft);
	mod.funcTypeIndices.push_back(0);

	FuncCode code;
	WasmLocal lc;
	lc.count = 3;
	lc.type = ValType::I64;
	code.locals.push_back(lc);
	code.body = {0x0B};
	mod.codes.push_back(code);

	WatEmitter emitter;
	auto result = emitter.emit(mod);

	EXPECT_NE(result.source.find("(local 0 i64)"), std::string::npos);
	EXPECT_NE(result.source.find("(local 1 i64)"), std::string::npos);
	EXPECT_NE(result.source.find("(local 2 i64)"), std::string::npos);
	EXPECT_EQ(result.source.find("past the emission limit"), std::string::npos);
}
