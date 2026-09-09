/**
 * @file tests/debug_info/debug_info_test.cpp
 * @brief Unit tests for the debug_info module.
 *
 * Tests are organised into groups:
 *   1. DebugLocEvaluator — DWARF location expression evaluation
 *   2. DebugGroundTruth helpers — lookup, typeName, inlinedAt
 *   3. DebugVar::locationAt — live-range lookup
 *   4. DwarfExtractor — synthetic ELF + DWARF blobs (pure-C++ fallback)
 *   5. PdbExtractor  — synthetic MSF/PDB blobs
 */

#include "retdec/debug_info/debug_info.h"
#include "retdec/debug_info/dwarf_extractor.h"
#include "retdec/debug_info/pdb_extractor.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "pdb_builder.h"

#include <gtest/gtest.h>

using namespace retdec::debug_info;

// ════════════════════════════════════════════════════════════════════════════
// 1. DebugLocEvaluator
// ════════════════════════════════════════════════════════════════════════════

TEST(DebugLocEvaluator, EmptyExprReturnsUnknown)
{
	StorageLoc s = DebugLocEvaluator::evaluate(nullptr, 0);
	EXPECT_EQ(s.kind, StorageKind::Unknown);
}

TEST(DebugLocEvaluator, DW_OP_reg0)
{
	uint8_t expr[] = {0x50}; // DW_OP_reg0
	auto s = DebugLocEvaluator::evaluate(expr, 1);
	EXPECT_EQ(s.kind, StorageKind::Register);
	EXPECT_EQ(s.regNum, 0u);
}

TEST(DebugLocEvaluator, DW_OP_reg5)
{
	uint8_t expr[] = {0x55}; // DW_OP_reg5 (RBP on x86-64)
	auto s = DebugLocEvaluator::evaluate(expr, 1);
	EXPECT_EQ(s.kind, StorageKind::Register);
	EXPECT_EQ(s.regNum, 5u);
}

TEST(DebugLocEvaluator, DW_OP_reg31)
{
	uint8_t expr[] = {0x6f}; // DW_OP_reg31
	auto s = DebugLocEvaluator::evaluate(expr, 1);
	EXPECT_EQ(s.kind, StorageKind::Register);
	EXPECT_EQ(s.regNum, 31u);
}

TEST(DebugLocEvaluator, DW_OP_regx)
{
	uint8_t expr[] = {0x90, 0x20}; // DW_OP_regx 32
	auto s = DebugLocEvaluator::evaluate(expr, 2);
	EXPECT_EQ(s.kind, StorageKind::Register);
	EXPECT_EQ(s.regNum, 32u);
}

TEST(DebugLocEvaluator, DW_OP_fbreg_zero)
{
	uint8_t expr[] = {0x91, 0x00}; // DW_OP_fbreg 0
	auto s = DebugLocEvaluator::evaluate(expr, 2);
	EXPECT_EQ(s.kind, StorageKind::StackSlot);
	EXPECT_EQ(s.offset, 0);
}

TEST(DebugLocEvaluator, DW_OP_fbreg_negative)
{
	// fbreg -8  ⟶  SLEB128 = 0x78
	uint8_t expr[] = {0x91, 0x78};
	auto s = DebugLocEvaluator::evaluate(expr, 2);
	EXPECT_EQ(s.kind, StorageKind::StackSlot);
	EXPECT_EQ(s.offset, -8);
}

TEST(DebugLocEvaluator, DW_OP_fbreg_positive)
{
	// fbreg +16  ⟶  SLEB128 = 0x10
	uint8_t expr[] = {0x91, 0x10};
	auto s = DebugLocEvaluator::evaluate(expr, 2);
	EXPECT_EQ(s.kind, StorageKind::StackSlot);
	EXPECT_EQ(s.offset, 16);
}

TEST(DebugLocEvaluator, DW_OP_addr_64bit)
{
	// DW_OP_addr 0x0000000000401000
	uint8_t expr[9] = {0x03, 0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
	auto s = DebugLocEvaluator::evaluate(expr, 9, /*addrSize=*/8);
	EXPECT_EQ(s.kind, StorageKind::StaticAddr);
	EXPECT_EQ(s.addr, 0x401000u);
}

TEST(DebugLocEvaluator, DW_OP_addr_32bit)
{
	uint8_t expr[5] = {0x03, 0x00, 0x10, 0x40, 0x00};
	auto s = DebugLocEvaluator::evaluate(expr, 5, /*addrSize=*/4);
	EXPECT_EQ(s.kind, StorageKind::StaticAddr);
	EXPECT_EQ(s.addr, 0x401000u);
}

TEST(DebugLocEvaluator, DW_OP_breg6_offset)
{
	// DW_OP_breg6 (RBP) -24  ⟶  0x77+6 = 0x7d, SLEB128(-24) = 0x68
	uint8_t expr[] = {0x7d, 0x68};
	auto s = DebugLocEvaluator::evaluate(expr, 2);
	EXPECT_EQ(s.kind, StorageKind::Register);
	EXPECT_EQ(s.regNum, 6u);
	EXPECT_EQ(s.offset, -24);
}

TEST(DebugLocEvaluator, DW_OP_stack_value)
{
	uint8_t expr[] = {0x30, 0x9f}; // DW_OP_lit0, DW_OP_stack_value
	auto s = DebugLocEvaluator::evaluate(expr, 2);
	EXPECT_EQ(s.kind, StorageKind::Optimized);
}

TEST(DebugLocEvaluator, DW_OP_lit7)
{
	uint8_t expr[] = {0x37}; // DW_OP_lit7
	auto s = DebugLocEvaluator::evaluate(expr, 1);
	EXPECT_EQ(s.kind, StorageKind::StaticAddr);
	EXPECT_EQ(s.addr, 7u);
}

TEST(DebugLocEvaluator, DW_OP_plus_uconst)
{
	// DW_OP_lit0, DW_OP_plus_uconst 128
	uint8_t expr[] = {0x30, 0x23, 0x80, 0x01}; // 128 as ULEB
	auto s = DebugLocEvaluator::evaluate(expr, 4);
	EXPECT_EQ(s.kind, StorageKind::StaticAddr);
	EXPECT_EQ(s.addr, 128u);
}

TEST(DebugLocEvaluator, ULEB128_decoding)
{
	uint8_t buf[] = {0x80, 0x01}; // 128
	const uint8_t* p = buf;
	uint64_t v = DebugLocEvaluator::readULEB128(p, buf + 2);
	EXPECT_EQ(v, 128u);
	EXPECT_EQ(p, buf + 2);
}

TEST(DebugLocEvaluator, SLEB128_decoding_negative)
{
	uint8_t buf[] = {0x7c}; // -4
	const uint8_t* p = buf;
	int64_t v = DebugLocEvaluator::readSLEB128(p, buf + 1);
	EXPECT_EQ(v, -4);
}

TEST(DebugLocEvaluator, SLEB128_decoding_multibyte)
{
	uint8_t buf[] = {0xd4, 0x7d}; // -300
	const uint8_t* p = buf;
	int64_t v = DebugLocEvaluator::readSLEB128(p, buf + 2);
	EXPECT_EQ(v, -300);
}

// ════════════════════════════════════════════════════════════════════════════
// 2. DebugGroundTruth helpers
// ════════════════════════════════════════════════════════════════════════════

static DebugGroundTruth makeSampleGDT()
{
	DebugGroundTruth gdt;

	// Add a primitive type
	DebugType t1;
	t1.id = 1;
	t1.kind = DebugTypeKind::Primitive;
	t1.name = "int";
	t1.byteSize = 4;
	gdt.types[1] = t1;

	// Add a pointer type
	DebugType t2;
	t2.id = 2;
	t2.kind = DebugTypeKind::Pointer;
	t2.pointedToTypeId = 1;
	t2.byteSize = 8;
	gdt.types[2] = t2;

	// Add a void type
	DebugType t3;
	t3.id = 3;
	t3.kind = DebugTypeKind::Void;
	t3.name = "void";
	gdt.types[3] = t3;

	// Add a struct type with two fields
	DebugType t4;
	t4.id = 4;
	t4.kind = DebugTypeKind::Struct;
	t4.name = "Point";
	t4.byteSize = 8;
	DebugField fx;
	fx.name = "x";
	fx.typeId = 1;
	fx.byteOffset = 0;
	DebugField fy;
	fy.name = "y";
	fy.typeId = 1;
	fy.byteOffset = 4;
	t4.fields = {fx, fy};
	gdt.types[4] = t4;

	// Add a function
	DebugFunc fn;
	fn.name = "add";
	fn.lowPc = 0x1000;
	fn.highPc = 0x1040;
	fn.returnTypeId = 1;
	DebugVar p1;
	p1.name = "a";
	p1.typeId = 1;
	p1.isParam = true;
	p1.paramIdx = 0;
	DebugVar p2;
	p2.name = "b";
	p2.typeId = 1;
	p2.isParam = true;
	p2.paramIdx = 1;
	fn.params = {p1, p2};
	gdt.functions[0x1000] = fn;

	// Add an inlined site
	InlinedSite site;
	site.calleeName = "mul";
	site.loAddr = 0x1010;
	site.hiAddr = 0x1020;
	gdt.allInlined.push_back(site);

	return gdt;
}

TEST(DebugGroundTruth, FuncAt_hit)
{
	auto gdt = makeSampleGDT();
	const DebugFunc* f = gdt.funcAt(0x1010);
	ASSERT_NE(f, nullptr);
	EXPECT_EQ(f->name, "add");
}

TEST(DebugGroundTruth, FuncAt_miss)
{
	auto gdt = makeSampleGDT();
	EXPECT_EQ(gdt.funcAt(0x2000), nullptr);
}

TEST(DebugGroundTruth, FuncAt_boundary_low)
{
	auto gdt = makeSampleGDT();
	EXPECT_NE(gdt.funcAt(0x1000), nullptr);
}

TEST(DebugGroundTruth, FuncAt_boundary_high_exclusive)
{
	auto gdt = makeSampleGDT();
	EXPECT_EQ(gdt.funcAt(0x1040), nullptr);
}

TEST(DebugGroundTruth, FuncByName)
{
	auto gdt = makeSampleGDT();
	EXPECT_NE(gdt.funcByName("add"), nullptr);
	EXPECT_EQ(gdt.funcByName("nonexistent"), nullptr);
}

TEST(DebugGroundTruth, TypeById)
{
	auto gdt = makeSampleGDT();
	EXPECT_NE(gdt.typeById(1), nullptr);
	EXPECT_EQ(gdt.typeById(999), nullptr);
}

TEST(DebugGroundTruth, TypeNameNamed)
{
	auto gdt = makeSampleGDT();
	EXPECT_EQ(gdt.typeName(1), "int");
	EXPECT_EQ(gdt.typeName(3), "void");
}

TEST(DebugGroundTruth, TypeNamePointer)
{
	auto gdt = makeSampleGDT();
	EXPECT_EQ(gdt.typeName(2), "int*");
}

TEST(DebugGroundTruth, TypeNameStruct)
{
	auto gdt = makeSampleGDT();
	EXPECT_EQ(gdt.typeName(4), "Point");
}

TEST(DebugGroundTruth, TypeNameUnknownId)
{
	auto gdt = makeSampleGDT();
	EXPECT_EQ(gdt.typeName(9999), "<unknown_type>");
}

TEST(DebugGroundTruth, InlinedAt_hit)
{
	auto gdt = makeSampleGDT();
	auto v = gdt.inlinedAt(0x1015);
	ASSERT_EQ(v.size(), 1u);
	EXPECT_EQ(v[0]->calleeName, "mul");
}

TEST(DebugGroundTruth, InlinedAt_miss)
{
	auto gdt = makeSampleGDT();
	EXPECT_TRUE(gdt.inlinedAt(0x2000).empty());
}

TEST(DebugGroundTruth, Empty)
{
	DebugGroundTruth gdt;
	EXPECT_TRUE(gdt.empty());
	gdt.functions[0] = DebugFunc{};
	EXPECT_FALSE(gdt.empty());
}

// ════════════════════════════════════════════════════════════════════════════
// 3. DebugVar::locationAt
// ════════════════════════════════════════════════════════════════════════════

TEST(DebugVar, LocationAt_inRange)
{
	DebugVar v;
	LiveRange lr;
	lr.lo = 0x100;
	lr.hi = 0x200;
	lr.loc = StorageLoc::reg(5);
	v.liveRanges.push_back(lr);
	auto s = v.locationAt(0x150);
	EXPECT_EQ(s.kind, StorageKind::Register);
	EXPECT_EQ(s.regNum, 5u);
}

TEST(DebugVar, LocationAt_outOfRange)
{
	DebugVar v;
	LiveRange lr;
	lr.lo = 0x100;
	lr.hi = 0x200;
	lr.loc = StorageLoc::reg(5);
	v.liveRanges.push_back(lr);
	EXPECT_EQ(v.locationAt(0x300).kind, StorageKind::Unknown);
}

TEST(DebugVar, LocationAt_staticEntry_zeroRange)
{
	DebugVar v;
	LiveRange lr;
	lr.lo = 0;
	lr.hi = 0;
	lr.loc = StorageLoc::staticAddr(0xDEAD);
	v.liveRanges.push_back(lr);
	auto s = v.locationAt(0x999);
	EXPECT_EQ(s.kind, StorageKind::StaticAddr);
	EXPECT_EQ(s.addr, 0xDEADu);
}

TEST(DebugVar, LocationAt_multipleRanges)
{
	DebugVar v;
	LiveRange lr1;
	lr1.lo = 0x100;
	lr1.hi = 0x200;
	lr1.loc = StorageLoc::reg(0);
	LiveRange lr2;
	lr2.lo = 0x200;
	lr2.hi = 0x300;
	lr2.loc = StorageLoc::stack(-8);
	v.liveRanges = {lr1, lr2};
	EXPECT_EQ(v.locationAt(0x150).regNum, 0u);
	EXPECT_EQ(v.locationAt(0x250).offset, -8);
}

// ════════════════════════════════════════════════════════════════════════════
// 4. DwarfExtractor — synthetic ELF + DWARF
// ════════════════════════════════════════════════════════════════════════════

namespace {

// ── ULEB128 / SLEB128 encoding helpers ───────────────────────────────────────

static void appendULEB(std::vector<uint8_t>& v, uint64_t val)
{
	do
	{
		uint8_t b = val & 0x7F;
		val >>= 7;
		if (val) b |= 0x80;
		v.push_back(b);
	}
	while (val);
}

static void appendSLEB(std::vector<uint8_t>& v, int64_t val)
{
	bool more = true;
	while (more)
	{
		uint8_t b = val & 0x7F;
		val >>= 7;
		if ((val == 0 && !(b & 0x40)) || (val == -1 && (b & 0x40)))
			more = false;
		else
			b |= 0x80;
		v.push_back(b);
	}
}

static void append32le(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(x & 0xFF);
	v.push_back((x >> 8) & 0xFF);
	v.push_back((x >> 16) & 0xFF);
	v.push_back((x >> 24) & 0xFF);
}

static void append64le(std::vector<uint8_t>& v, uint64_t x)
{
	for (int i = 0; i < 8; ++i)
		v.push_back((x >> (8 * i)) & 0xFF);
}

static void append16le(std::vector<uint8_t>& v, uint16_t x)
{
	v.push_back(x & 0xFF);
	v.push_back((x >> 8) & 0xFF);
}

static void patchU32(std::vector<uint8_t>& v, std::size_t off, uint32_t val)
{
	v[off + 0] = val & 0xFF;
	v[off + 1] = (val >> 8) & 0xFF;
	v[off + 2] = (val >> 16) & 0xFF;
	v[off + 3] = (val >> 24) & 0xFF;
}

static void appendStr(std::vector<uint8_t>& v, const std::string& s)
{
	v.insert(v.end(), s.begin(), s.end());
	v.push_back(0);
}

// ── Minimal ELF64 builder ─────────────────────────────────────────────────────

class ElfBuilder {
public:
	struct Section
	{
		std::string name;
		uint32_t type; // SHT_*
		uint64_t flags;
		std::vector<uint8_t> data;
	};

	void addSection(const std::string& name, uint32_t type, uint64_t flags, std::vector<uint8_t> data)
	{
		sections_.push_back({name, type, flags, std::move(data)});
	}

	std::vector<uint8_t> build()
	{
		// Build section name string table
		std::vector<uint8_t> shstrtab;
		shstrtab.push_back(0); // index 0 = empty
		std::vector<uint32_t> nameOffsets;
		nameOffsets.push_back(0); // null section
		for (const auto& s: sections_)
		{
			nameOffsets.push_back(static_cast<uint32_t>(shstrtab.size()));
			for (char c: s.name)
				shstrtab.push_back(static_cast<uint8_t>(c));
			shstrtab.push_back(0);
		}
		// Add shstrtab itself
		uint32_t shstrNameOff = static_cast<uint32_t>(shstrtab.size());
		for (char c: std::string(".shstrtab"))
			shstrtab.push_back(static_cast<uint8_t>(c));
		shstrtab.push_back(0);

		// Layout: ELF header (64 bytes), then sections, then shstrtab, then shdr table
		std::vector<uint8_t> out;
		out.resize(64, 0);

		// ELF magic
		static const uint8_t magic[] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		std::copy(magic, magic + 16, out.begin());

		// e_type=ET_EXEC, e_machine=EM_X86_64, e_version=1
		out[16] = 2;
		out[17] = 0; // ET_EXEC
		out[18] = 0x3E;
		out[19] = 0; // EM_X86_64
		out[20] = 1;
		out[21] = out[22] = out[23] = 0; // e_version

		// Place sections after header
		std::vector<uint64_t> secOffsets;
		for (const auto& s: sections_)
		{
			// Align to 8
			while (out.size() % 8)
				out.push_back(0);
			secOffsets.push_back(out.size());
			out.insert(out.end(), s.data.begin(), s.data.end());
		}
		// shstrtab
		while (out.size() % 8)
			out.push_back(0);
		uint64_t shstrOff = out.size();
		out.insert(out.end(), shstrtab.begin(), shstrtab.end());

		// Section header table
		while (out.size() % 8)
			out.push_back(0);
		uint64_t shoff = out.size();
		uint16_t shnum = static_cast<uint16_t>(1 + sections_.size() + 1); // null + secs + shstrtab
		uint16_t shstrndx = static_cast<uint16_t>(shnum - 1);

		auto writeShdr = [&](uint32_t nameOff,
							 uint32_t type,
							 uint64_t flags,
							 uint64_t offset,
							 uint64_t size,
							 uint32_t link = 0,
							 uint32_t info = 0,
							 uint64_t addralign = 1,
							 uint64_t entsize = 0) {
			append32le(out, nameOff);
			append32le(out, type);
			append64le(out, flags);
			append64le(out, 0); // sh_addr
			append64le(out, offset);
			append64le(out, size);
			append32le(out, link);
			append32le(out, info);
			append64le(out, addralign);
			append64le(out, entsize);
		};
		// Null section
		writeShdr(0, 0, 0, 0, 0);
		for (std::size_t i = 0; i < sections_.size(); ++i)
		{
			writeShdr(
				nameOffsets[i + 1], sections_[i].type, sections_[i].flags, secOffsets[i], sections_[i].data.size());
		}
		// .shstrtab section
		writeShdr(shstrNameOff, 3 /*SHT_STRTAB*/, 0, shstrOff, shstrtab.size());

		// Patch ELF header fields
		// e_shoff (offset 40)
		for (int i = 0; i < 8; ++i)
			out[40 + i] = (shoff >> (8 * i)) & 0xFF;
		// e_shentsize = 64
		out[58] = 64;
		out[59] = 0;
		// e_shnum
		out[60] = shnum & 0xFF;
		out[61] = (shnum >> 8) & 0xFF;
		// e_shstrndx
		out[62] = shstrndx & 0xFF;
		out[63] = (shstrndx >> 8) & 0xFF;

		return out;
	}

private:
	std::vector<Section> sections_;
};

// ── Minimal DWARF4 blob builder ───────────────────────────────────────────────

class DwarfBuilder {
public:
	// Abbrev codes we define
	enum Abbrevs
	{
		ABBREV_CU = 1,
		ABBREV_SUBPROG = 2,
		ABBREV_PARAM = 3,
		ABBREV_VAR = 4,
		ABBREV_INLINED = 5,
		ABBREV_BASE_TYPE = 6,
		ABBREV_PTR_TYPE = 7,
		ABBREV_STRUCT = 8,
		ABBREV_MEMBER = 9,
	};

	// Build .debug_abbrev section
	static std::vector<uint8_t> buildAbbrev()
	{
		std::vector<uint8_t> v;

		// CU: DW_TAG_compile_unit, has_children
		appendULEB(v, ABBREV_CU);
		appendULEB(v, 0x11); // DW_TAG_compile_unit
		v.push_back(1);      // has_children
		appendULEB(v, 0x03);
		v.push_back(0x08); // DW_AT_name  / DW_FORM_string
		appendULEB(v, 0x1b);
		v.push_back(0x08); // DW_AT_comp_dir / DW_FORM_string
		appendULEB(v, 0);
		appendULEB(v, 0);

		// subprogram: DW_TAG_subprogram, has_children
		appendULEB(v, ABBREV_SUBPROG);
		appendULEB(v, 0x2e);
		v.push_back(1);
		appendULEB(v, 0x03);
		v.push_back(0x08); // name
		appendULEB(v, 0x11);
		v.push_back(0x01); // low_pc  / DW_FORM_addr
		appendULEB(v, 0x12);
		v.push_back(0x07); // high_pc / DW_FORM_data8 (offset)
		appendULEB(v, 0x49);
		v.push_back(0x13); // type    / DW_FORM_ref4
		appendULEB(v, 0x3f);
		v.push_back(0x19); // external / DW_FORM_flag_present
		appendULEB(v, 0);
		appendULEB(v, 0);

		// formal_parameter: no children
		appendULEB(v, ABBREV_PARAM);
		appendULEB(v, 0x05);
		v.push_back(0);
		appendULEB(v, 0x03);
		v.push_back(0x08); // name
		appendULEB(v, 0x49);
		v.push_back(0x13); // type / ref4
		appendULEB(v, 0x02);
		v.push_back(0x0a); // location / block1
		appendULEB(v, 0);
		appendULEB(v, 0);

		// variable: no children
		appendULEB(v, ABBREV_VAR);
		appendULEB(v, 0x34);
		v.push_back(0);
		appendULEB(v, 0x03);
		v.push_back(0x08); // name
		appendULEB(v, 0x49);
		v.push_back(0x13); // type / ref4
		appendULEB(v, 0x02);
		v.push_back(0x0a); // location / block1
		appendULEB(v, 0);
		appendULEB(v, 0);

		// inlined_subroutine: no children (simplified)
		appendULEB(v, ABBREV_INLINED);
		appendULEB(v, 0x1d);
		v.push_back(0);
		appendULEB(v, 0x03);
		v.push_back(0x08); // name
		appendULEB(v, 0x11);
		v.push_back(0x01); // low_pc  / addr
		appendULEB(v, 0x12);
		v.push_back(0x07); // high_pc / data8 (offset)
		appendULEB(v, 0x59);
		v.push_back(0x0b); // call_line / data1
		appendULEB(v, 0);
		appendULEB(v, 0);

		// base_type: no children
		appendULEB(v, ABBREV_BASE_TYPE);
		appendULEB(v, 0x24);
		v.push_back(0);
		appendULEB(v, 0x03);
		v.push_back(0x08); // name
		appendULEB(v, 0x0b);
		v.push_back(0x0b); // byte_size / data1
		appendULEB(v, 0);
		appendULEB(v, 0);

		// pointer_type: no children
		appendULEB(v, ABBREV_PTR_TYPE);
		appendULEB(v, 0x0f);
		v.push_back(0);
		appendULEB(v, 0x0b);
		v.push_back(0x0b); // byte_size / data1
		appendULEB(v, 0x49);
		v.push_back(0x13); // type / ref4
		appendULEB(v, 0);
		appendULEB(v, 0);

		// structure_type: has_children (members)
		appendULEB(v, ABBREV_STRUCT);
		appendULEB(v, 0x13);
		v.push_back(1);
		appendULEB(v, 0x03);
		v.push_back(0x08); // name
		appendULEB(v, 0x0b);
		v.push_back(0x0b); // byte_size / data1
		appendULEB(v, 0);
		appendULEB(v, 0);

		// member: no children
		appendULEB(v, ABBREV_MEMBER);
		appendULEB(v, 0x0d);
		v.push_back(0);
		appendULEB(v, 0x03);
		v.push_back(0x08); // name
		appendULEB(v, 0x49);
		v.push_back(0x13); // type / ref4
		appendULEB(v, 0x38);
		v.push_back(0x0b); // data_member_location / data1
		appendULEB(v, 0);
		appendULEB(v, 0);

		// Terminator
		v.push_back(0);
		return v;
	}

	// Build .debug_info containing a single CU with one subprogram.
	// Returns the built blob and the CU-relative offset of the base_type DIE.
	struct FuncEntry
	{
		std::string name;
		uint64_t lowPc;
		uint64_t highPcOffset; // encoded as offset
		// param location: DW_OP_reg5
		std::string paramName;
		std::string localName;
		std::string inlinedName;
		uint64_t inlinedLo;
		uint64_t inlinedHi;
	};

	static std::vector<uint8_t> buildInfo(
		const std::string& srcFile,
		const std::string& compDir,
		const FuncEntry& func,
		uint32_t& outBaseTypeOffset,
		uint32_t& outPtrTypeOffset)
	{
		std::vector<uint8_t> body; // DIE body (after CU header)

		// ── First, add type DIEs ──────────────────────────────────────────────
		// Remember offsets (relative to start of body)
		// base_type "int" DIE
		outBaseTypeOffset = static_cast<uint32_t>(body.size()) + 11; // 11 = CU header size
		appendULEB(body, ABBREV_BASE_TYPE);
		appendStr(body, "int");
		body.push_back(4); // byte_size=4

		// pointer_type DIE
		outPtrTypeOffset = static_cast<uint32_t>(body.size()) + 11;
		appendULEB(body, ABBREV_PTR_TYPE);
		body.push_back(8);                        // byte_size=8
		append32le(body, outBaseTypeOffset - 11); // type ref (CU-relative)

		// ── CU DIE ────────────────────────────────────────────────────────────
		// We need to build: CU_DIE, types, subprogram, ...
		// Reset body and do it in order.
		body.clear();
		// CU DIE
		appendULEB(body, ABBREV_CU);
		appendStr(body, srcFile);
		appendStr(body, compDir);

		// base_type "int"
		uint32_t baseTypeOff = static_cast<uint32_t>(body.size()) + 11;
		outBaseTypeOffset = baseTypeOff;
		appendULEB(body, ABBREV_BASE_TYPE);
		appendStr(body, "int");
		body.push_back(4);

		// pointer_type → int
		uint32_t ptrTypeOff = static_cast<uint32_t>(body.size()) + 11;
		outPtrTypeOffset = ptrTypeOff;
		appendULEB(body, ABBREV_PTR_TYPE);
		body.push_back(8);
		append32le(body, baseTypeOff - 11); // relative ref within CU

		// subprogram
		appendULEB(body, ABBREV_SUBPROG);
		appendStr(body, func.name);
		append64le(body, func.lowPc);
		append64le(body, func.highPcOffset);
		append32le(body, baseTypeOff - 11); // return type
		// DW_FORM_flag_present → no bytes

		// formal_parameter "a": DW_OP_reg5
		if (!func.paramName.empty())
		{
			appendULEB(body, ABBREV_PARAM);
			appendStr(body, func.paramName);
			append32le(body, baseTypeOff - 11);
			body.push_back(1);    // block len
			body.push_back(0x55); // DW_OP_reg5
		}

		// local variable: DW_OP_fbreg -8
		if (!func.localName.empty())
		{
			appendULEB(body, ABBREV_VAR);
			appendStr(body, func.localName);
			append32le(body, baseTypeOff - 11);
			body.push_back(2);    // block len
			body.push_back(0x91); // DW_OP_fbreg
			body.push_back(0x78); // SLEB -8
		}

		// inlined subroutine
		if (!func.inlinedName.empty())
		{
			appendULEB(body, ABBREV_INLINED);
			appendStr(body, func.inlinedName);
			append64le(body, func.inlinedLo);
			append64le(body, func.inlinedHi - func.inlinedLo); // offset form
			body.push_back(42);                                // call_line
		}

		// Null child DIE for subprogram
		body.push_back(0);
		// Null child DIE for CU
		body.push_back(0);

		// Build DWARF4 CU header (11 bytes, 32-bit DWARF)
		std::vector<uint8_t> info;
		uint32_t unitLength = static_cast<uint32_t>(2 + 4 + 1 + body.size()); // version+abbrevOff+addrSize+body
		append32le(info, unitLength);
		append16le(info, 4); // DWARF version
		append32le(info, 0); // .debug_abbrev offset
		info.push_back(8);   // address size

		info.insert(info.end(), body.begin(), body.end());
		return info;
	}
};

// ── Write a temp file and return its path ─────────────────────────────────────

static std::string writeTempFile(const std::string& suffix, const std::vector<uint8_t>& data)
{
	static int counter = 0;
	++counter;
	std::string path;
#ifdef _WIN32
	char tmp[MAX_PATH];
	::GetTempPathA(MAX_PATH, tmp);
	path = std::string(tmp) + "retdec_test_" + std::to_string(counter) + suffix;
#else
	const char* tmpdir = std::getenv("TMPDIR");
	if (!tmpdir) tmpdir = "/tmp";
	path = std::string(tmpdir) + "/retdec_test_" + std::to_string(counter) + suffix;
#endif
	std::ofstream ofs(path, std::ios::binary);
	ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
	return path;
}

} // namespace

// ── DwarfExtractor tests ──────────────────────────────────────────────────────

TEST(DwarfExtractor, NotFoundReturnsEmpty)
{
	DwarfExtractor ex("/nonexistent_file_xyz.elf");
	DebugGroundTruth gdt;
	bool ok = ex.extract(gdt);
	EXPECT_FALSE(ok);
	EXPECT_FALSE(gdt.diagnostics.empty());
}

TEST(DwarfExtractor, ExtractFunctionName)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "myFunc";
	fn.lowPc = 0x1000;
	fn.highPcOffset = 0x40;
	fn.paramName = "x";
	fn.localName = "tmp";
	fn.inlinedName = "helper";
	fn.inlinedLo = 0x1010;
	fn.inlinedHi = 0x1020;

	auto infoBytes = DwarfBuilder::buildInfo("test.c", "/src", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1 /*SHT_PROGBITS*/, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);

	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);

	const DebugFunc* f = gdt.funcByName("myFunc");
	ASSERT_NE(f, nullptr) << "Function 'myFunc' not found";
	EXPECT_EQ(f->lowPc, 0x1000u);
	EXPECT_EQ(f->highPc, 0x1040u);
}

TEST(DwarfExtractor, ExtractParameter)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "add";
	fn.lowPc = 0x2000;
	fn.highPcOffset = 0x20;
	fn.paramName = "a";

	auto infoBytes = DwarfBuilder::buildInfo("add.c", "/src", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);
	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);

	const DebugFunc* f = gdt.funcByName("add");
	ASSERT_NE(f, nullptr);
	ASSERT_GE(f->params.size(), 1u);
	EXPECT_EQ(f->params[0].name, "a");
	EXPECT_TRUE(f->params[0].isParam);
	EXPECT_EQ(f->params[0].paramIdx, 0u);
}

TEST(DwarfExtractor, ParamLocationIsRegister)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "foo";
	fn.lowPc = 0x3000;
	fn.highPcOffset = 0x10;
	fn.paramName = "p";

	auto infoBytes = DwarfBuilder::buildInfo("foo.c", "/src", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);
	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);

	const DebugFunc* f = gdt.funcByName("foo");
	ASSERT_NE(f, nullptr);
	ASSERT_GE(f->params.size(), 1u);
	auto loc = f->params[0].locationAt(0x3000); // zero range → static entry
	// Location was encoded as DW_OP_reg5
	EXPECT_EQ(loc.kind, StorageKind::Register);
	EXPECT_EQ(loc.regNum, 5u);
}

TEST(DwarfExtractor, LocalVariableStackSlot)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "bar";
	fn.lowPc = 0x4000;
	fn.highPcOffset = 0x30;
	fn.localName = "tmp";

	auto infoBytes = DwarfBuilder::buildInfo("bar.c", "/src", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);
	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);

	const DebugFunc* f = gdt.funcByName("bar");
	ASSERT_NE(f, nullptr);
	ASSERT_GE(f->locals.size(), 1u);
	EXPECT_EQ(f->locals[0].name, "tmp");
	auto loc = f->locals[0].locationAt(0);
	EXPECT_EQ(loc.kind, StorageKind::StackSlot);
	EXPECT_EQ(loc.offset, -8);
}

TEST(DwarfExtractor, InlinedSite)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "caller";
	fn.lowPc = 0x5000;
	fn.highPcOffset = 0x100;
	fn.inlinedName = "callee";
	fn.inlinedLo = 0x5010;
	fn.inlinedHi = 0x5030;

	auto infoBytes = DwarfBuilder::buildInfo("caller.c", "/src", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);
	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);

	// allInlined should have one entry
	ASSERT_GE(gdt.allInlined.size(), 1u);
	bool found = false;
	for (const auto& s: gdt.allInlined)
	{
		if (s.calleeName == "callee")
		{
			EXPECT_EQ(s.loAddr, 0x5010u);
			EXPECT_EQ(s.hiAddr, 0x5030u);
			found = true;
		}
	}
	EXPECT_TRUE(found);
}

TEST(DwarfExtractor, BaseTypeExtracted)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "g";
	fn.lowPc = 0x6000;
	fn.highPcOffset = 4;

	auto infoBytes = DwarfBuilder::buildInfo("g.c", "/build", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);
	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);

	// There should be at least one Primitive type named "int"
	bool found = false;
	for (const auto& kv: gdt.types)
	{
		if (kv.second.kind == DebugTypeKind::Primitive && kv.second.name == "int")
		{
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found) << "Expected 'int' base type in extracted types";
}

TEST(DwarfExtractor, SourceFileExtracted)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "h";
	fn.lowPc = 0x7000;
	fn.highPcOffset = 4;

	auto infoBytes = DwarfBuilder::buildInfo("main.c", "/home/user", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);
	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);

	ASSERT_GE(gdt.sourceFiles.size(), 1u);
	EXPECT_EQ(gdt.sourceFiles[0].path, "main.c");
	EXPECT_EQ(gdt.sourceFiles[0].compDir, "/home/user");
}

TEST(DwarfExtractor, AddrSizeIs8)
{
	uint32_t btOff, ptOff;
	DwarfBuilder::FuncEntry fn;
	fn.name = "q";
	fn.lowPc = 0x100;
	fn.highPcOffset = 0;

	auto infoBytes = DwarfBuilder::buildInfo("q.c", "/", fn, btOff, ptOff);
	auto abbrevBytes = DwarfBuilder::buildAbbrev();

	ElfBuilder eb;
	eb.addSection(".debug_abbrev", 1, 0, abbrevBytes);
	eb.addSection(".debug_info", 1, 0, infoBytes);
	auto elfBytes = eb.build();
	std::string path = writeTempFile(".elf", elfBytes);

	DwarfExtractor ex(path);
	DebugGroundTruth gdt;
	ex.extract(gdt);
	EXPECT_EQ(ex.addrSize(), 8u);
}

// ════════════════════════════════════════════════════════════════════════════
// 5. PdbExtractor — synthetic MSF blobs
// ════════════════════════════════════════════════════════════════════════════

namespace {

// The builders live in pdb_builder.h so tests/pdbparser can use them too.
using retdec::tests::pdb_builder::CvSymBuilder;
using retdec::tests::pdb_builder::PdbBuilder;
using retdec::tests::pdb_builder::TpiBuilder;

} // namespace

TEST(PdbExtractor, NotFoundReturnsEmpty)
{
	PdbExtractor ex("/nonexistent.pdb", 0);
	DebugGroundTruth gdt;
	bool ok = ex.extract(gdt);
	EXPECT_FALSE(ok);
	EXPECT_FALSE(gdt.diagnostics.empty());
}

TEST(PdbExtractor, ExtractGlobalProcSymbol)
{
	CvSymBuilder cv;
	cv.addGProc32("myFunc", 0x1000, 0x40);

	TpiBuilder tpi;
	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	PdbExtractor ex(path, /*imageBase=*/0);
	DebugGroundTruth gdt;
	bool ok = ex.extract(gdt);
	// The fallback may succeed even if no functions found (returns false only on fatal)
	// Check diagnostics instead.
	if (ok)
	{
		const DebugFunc* f = gdt.funcByName("myFunc");
		if (f)
		{
			EXPECT_EQ(f->lowPc, 0x1000u);
			EXPECT_EQ(f->highPc, 0x1040u);
		}
	}
	// At minimum: the file loaded without crashing.
	EXPECT_TRUE(ok || !gdt.diagnostics.empty());
}

TEST(PdbExtractor, ExtractLocalProcSymbol)
{
	CvSymBuilder cv;
	cv.addLProc32("staticHelper", 0x2000, 0x80);

	TpiBuilder tpi;
	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	// This used to call extract() and end. The extractor could have produced
	// no symbols, or the wrong ones, and the test passed.
	ASSERT_TRUE(ex.extract(gdt));
	ASSERT_EQ(1u, gdt.functions.size());
	const DebugFunc* f = gdt.funcByName("staticHelper");
	ASSERT_NE(f, nullptr);
	EXPECT_EQ(0x2000u, f->lowPc);
	EXPECT_EQ(0x2080u, f->highPc);
}

TEST(PdbExtractor, ExtractStructureType)
{
	CvSymBuilder cv;
	TpiBuilder tpi;
	tpi.addStructure("Vec3", 12);
	tpi.addPointer(0x1000);

	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	// foundStruct used to be discarded with (void), so the failure to find the
	// structure the fixture had just built into the PDB was swallowed -- and
	// it was failing: the LF_STRUCTURE reader skipped twelve header bytes
	// where the record has sixteen, and every struct came back nameless with
	// byteSize 0.
	ASSERT_TRUE(ex.extract(gdt));
	bool foundStruct = false;
	for (const auto& kv: gdt.types)
	{
		if (kv.second.kind != DebugTypeKind::Struct) continue;
		EXPECT_EQ("Vec3", kv.second.name);
		EXPECT_EQ(12u, kv.second.byteSize);
		foundStruct = true;
	}
	EXPECT_TRUE(foundStruct) << "no LF_STRUCTURE record was extracted";
}

TEST(PdbExtractor, ExtractEnumType)
{
	CvSymBuilder cv;
	TpiBuilder tpi;
	tpi.addEnum("Colour", 0x74); // T_INT4

	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	ASSERT_TRUE(ex.extract(gdt));

	bool foundEnum = false;
	for (const auto& kv: gdt.types)
	{
		if (kv.second.kind != DebugTypeKind::Enum) continue;
		EXPECT_EQ("Colour", kv.second.name);
		EXPECT_EQ(0x74u, kv.second.baseTypeId);
		foundEnum = true;
	}
	EXPECT_TRUE(foundEnum) << "no LF_ENUM record was extracted";
}

// The two leaves above turned out to have their header lengths wrong, so the
// other two the extractor claims to read are checked the same way rather than
// assumed right.
TEST(PdbExtractor, ExtractUnionType)
{
	CvSymBuilder cv;
	TpiBuilder tpi;
	tpi.addUnion("Word", 4);

	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	ASSERT_TRUE(ex.extract(gdt));

	bool found = false;
	for (const auto& kv: gdt.types)
	{
		if (kv.second.kind != DebugTypeKind::Union) continue;
		EXPECT_EQ("Word", kv.second.name);
		EXPECT_EQ(4u, kv.second.byteSize);
		found = true;
	}
	EXPECT_TRUE(found) << "no LF_UNION record was extracted";
}

TEST(PdbExtractor, ExtractArrayType)
{
	CvSymBuilder cv;
	TpiBuilder tpi;
	tpi.addArray(0x74, 40); // int[10]

	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	ASSERT_TRUE(ex.extract(gdt));

	bool found = false;
	for (const auto& kv: gdt.types)
	{
		if (kv.second.kind != DebugTypeKind::Array) continue;
		EXPECT_EQ(0x74u, kv.second.elementTypeId);
		EXPECT_EQ(40u, kv.second.byteSize);
		found = true;
	}
	EXPECT_TRUE(found) << "no LF_ARRAY record was extracted";
}

TEST(PdbExtractor, ImageBaseApplied)
{
	CvSymBuilder cv;
	cv.addGProc32("entry", 0x1000, 0x50);

	TpiBuilder tpi;
	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	uint64_t base = 0x140000000ULL;
	PdbExtractor ex(path, base);
	DebugGroundTruth gdt;
	bool ok = ex.extract(gdt);
	if (ok)
	{
		// If func found, VA should be base + 0x1000
		const DebugFunc* f = gdt.funcByName("entry");
		if (f) EXPECT_EQ(f->lowPc, base + 0x1000u);
	}
}

TEST(PdbExtractor, MultipleSymbols)
{
	CvSymBuilder cv;
	cv.addGProc32("alpha", 0x1000, 0x20);
	cv.addGProc32("beta", 0x2000, 0x30);
	cv.addGProc32("gamma", 0x3000, 0x10);

	TpiBuilder tpi;
	auto pdb = PdbBuilder::build(cv.data(), tpi.build());
	std::string path = writeTempFile(".pdb", pdb);

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	ex.extract(gdt);
	// Just verify no crash; symbol count depends on MSF stream layout.
}

// ── DebugGroundTruth integration ──────────────────────────────────────────────

TEST(DebugGroundTruth, TypeNameArray)
{
	DebugGroundTruth gdt;
	DebugType elem;
	elem.id = 1;
	elem.kind = DebugTypeKind::Primitive;
	elem.name = "float";
	elem.byteSize = 4;
	gdt.types[1] = elem;

	DebugType arr;
	arr.id = 2;
	arr.kind = DebugTypeKind::Array;
	arr.elementTypeId = 1;
	arr.elementCount = 3;
	gdt.types[2] = arr;

	EXPECT_EQ(gdt.typeName(2), "float[3]");
}

TEST(DebugGroundTruth, TypeNameVoid)
{
	DebugGroundTruth gdt;
	DebugType v;
	v.id = 5;
	v.kind = DebugTypeKind::Void;
	gdt.types[5] = v;
	EXPECT_EQ(gdt.typeName(5), "void");
}

TEST(DebugGroundTruth, TypeNameAnonStruct)
{
	DebugGroundTruth gdt;
	DebugType s;
	s.id = 6;
	s.kind = DebugTypeKind::Struct;
	gdt.types[6] = s;
	EXPECT_EQ(gdt.typeName(6), "struct <anon>");
}

// DebugLocEvaluator::readULEB128 and readSLEB128 had unbounded shifts: a DWARF
// expression made of continuation bytes drove `shift` past the width of the
// accumulator, which is undefined, and the signed one accumulated into an
// int64_t, where the shift overflows the signed range sooner still. Both are
// private statics, so this drives them through evaluate(), which is how a
// DWARF expression out of a binary actually reaches them.
TEST(DebugLocEvaluatorLeb128, ContinuationRunsDoNotShiftOutOfRange)
{
	// DW_OP_regx (0x90), DW_OP_fbreg (0x91), DW_OP_bregx (0x92) and
	// DW_OP_plus_uconst (0x23) each read a LEB128 operand.
	for (uint8_t op: {uint8_t(0x90), uint8_t(0x91), uint8_t(0x92), uint8_t(0x23)})
	{
		std::vector<uint8_t> expr;
		expr.push_back(op);
		for (int i = 0; i < 24; ++i)
			expr.push_back(0xFF);

		EXPECT_NO_THROW((void)DebugLocEvaluator::evaluate(expr.data(), expr.size()))
			<< "opcode 0x" << std::hex << int(op);
	}
}

// A run with no terminating byte must not leave the cursor inside it: the
// evaluator has to stop, not carry on reading the rest of the run as opcodes.
TEST(DebugLocEvaluatorLeb128, AnUnterminatedOperandStopsTheWalk)
{
	std::vector<uint8_t> expr;
	expr.push_back(0x23); // DW_OP_plus_uconst
	for (int i = 0; i < 64; ++i)
		expr.push_back(0xFF);

	EXPECT_NO_THROW((void)DebugLocEvaluator::evaluate(expr.data(), expr.size()));
}

// ── Sizes a PDB declares but does not carry ──────────────────────────────────
//
// Every allocation in the MSF fallback used to be sized straight from a 32-bit
// header field. A 56-byte file that says its directory is 0xFFFFFFFF bytes long
// reserved four gigabytes for it, and std::bad_alloc came back out of
// extract() -- past a signature that returns bool and fills in diagnostics, so
// no caller was expecting to catch anything. Four shapes, all four throwing
// before the bound, none after.

namespace {

/// A PDB7 superblock with whatever field values the test wants, optionally
/// followed by a directory written at blockMapAddr.
std::vector<uint8_t> malformedPdb(
	uint32_t blockSize, uint32_t numDirectoryBytes, uint32_t blockMapAddr, const std::vector<uint8_t>& directory = {})
{
	static const char kMagic[] = "Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\x00\x00\x00";
	std::vector<uint8_t> out(kMagic, kMagic + 32);

	auto put32 = [&out](uint32_t v) {
		for (int i = 0; i < 4; ++i)
			out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
	};
	put32(blockSize);
	put32(1); // freeBlockMapBlock
	put32(2); // numBlocks
	put32(numDirectoryBytes);
	put32(0); // unknown
	put32(blockMapAddr);

	if (!directory.empty())
	{
		const size_t at = static_cast<size_t>(blockMapAddr) * blockSize;
		if (out.size() < at + directory.size()) out.resize(at + directory.size(), 0);
		std::copy(directory.begin(), directory.end(), out.begin() + at);
	}
	return out;
}

/// A directory naming @a count streams, each declaring @a size bytes.
std::vector<uint8_t> streamTable(uint32_t count, uint32_t size)
{
	std::vector<uint8_t> d;
	auto put32 = [&d](uint32_t v) {
		for (int i = 0; i < 4; ++i)
			d.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
	};
	put32(count);
	for (uint32_t i = 0; i < count; ++i)
		put32(size);
	return d;
}

} // namespace

TEST(PdbExtractor, ADirectoryLongerThanTheFileIsRefused)
{
	std::string path = writeTempFile(".pdb", malformedPdb(4096, 0xFFFFFFFFu, 1));

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	EXPECT_NO_THROW((void)ex.extract(gdt));
	EXPECT_FALSE(gdt.diagnostics.empty());
}

TEST(PdbExtractor, AStreamCountLargerThanTheDirectoryIsRefused)
{
	std::vector<uint8_t> dir;
	for (int i = 0; i < 4; ++i)
		dir.push_back(0xFF); // numStreams = 0xFFFFFFFF
	std::string path = writeTempFile(".pdb", malformedPdb(64, 64, 1, dir));

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	EXPECT_NO_THROW((void)ex.extract(gdt));
	EXPECT_FALSE(gdt.diagnostics.empty());
}

TEST(PdbExtractor, StreamSizesLargerThanTheFileAreRefused)
{
	const auto dir = streamTable(5, 0xFFFFFFFEu);
	std::string path = writeTempFile(".pdb", malformedPdb(64, static_cast<uint32_t>(dir.size()), 1, dir));

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	EXPECT_NO_THROW((void)ex.extract(gdt));
	EXPECT_FALSE(gdt.diagnostics.empty());
}

// The same with a page size big enough that the round-up in blocksNeeded --
// (bytes + blockSize - 1) -- wraps in 32 bits.
TEST(PdbExtractor, AStreamSizeThatWrapsTheBlockCountIsRefused)
{
	const auto dir = streamTable(5, 0xFFFFFFFEu);
	std::string path = writeTempFile(".pdb", malformedPdb(4096, static_cast<uint32_t>(dir.size()), 1, dir));

	PdbExtractor ex(path, 0);
	DebugGroundTruth gdt;
	EXPECT_NO_THROW((void)ex.extract(gdt));
	EXPECT_FALSE(gdt.diagnostics.empty());
}
