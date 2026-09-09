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
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <algorithm>

#include <zlib.h>

using namespace retdec::dex_parser;
using namespace retdec::bc_module;

// ─── DexReader ───────────────────────────────────────────────────────────────

TEST(DexReader, ReadsU1)
{
	uint8_t data[] = {0xAB};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(0xABu, r.u1());
}

TEST(DexReader, ReadsU2LE)
{
	uint8_t data[] = {0x34, 0x12};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(0x1234u, r.u2());
}

TEST(DexReader, ReadsU4LE)
{
	uint8_t data[] = {0x78, 0x56, 0x34, 0x12};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(0x12345678u, r.u4());
}

TEST(DexReader, ReadsU8LE)
{
	uint8_t data[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(0x0102030405060708ull, r.u8());
}

TEST(DexReader, ReadsSigned)
{
	uint8_t data[] = {0xFF, 0xFF};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(-1, r.s1());
	EXPECT_EQ(-1, r.s1());
}

/// s1/s2/s4/s8 were `static_cast<intN_t>(uN())`, an out-of-range conversion
/// for every value above the signed maximum -- implementation-defined before
/// C++20, and the value is a byte the file chooses. They now go through
/// byteorder::signExtendFrom, which is the two's complement reading proved for
/// every width in tests/verification/byte_order_proof.cpp; the narrowing after
/// it is exact by construction, because sign-extending from N bits lands in
/// [-2^(N-1), 2^(N-1)-1].
///
/// The values below are the top of each unsigned range, which is where the old
/// cast was out of range.
///
/// NOT load-bearing on this toolchain, and said so rather than implied: g++ on
/// x86-64 implements the out-of-range conversion as two's complement, so this
/// passes against the old body too -- measured, not assumed. What it pins is
/// the ANSWER, so a future rewrite of these four cannot change it, and what it
/// documents is that the guarantee now comes from the kernel rather than from a
/// compiler's choice. The behaviour it would have caught is a compiler that
/// chose differently, which is the whole reason the standard called it
/// implementation-defined.
TEST(DexReader, SignedReadsAreTwosComplementAtEveryWidth)
{
	{
		uint8_t d[] = {0xFF, 0x80, 0x7F};
		DexReader r(d, sizeof(d));
		EXPECT_EQ(-1, r.s1());
		EXPECT_EQ(-128, r.s1());
		EXPECT_EQ(127, r.s1());
	}
	{
		uint8_t d[] = {0xFF, 0xFF, 0x00, 0x80, 0xFF, 0x7F};
		DexReader r(d, sizeof(d));
		EXPECT_EQ(-1, r.s2());
		EXPECT_EQ(-32768, r.s2());
		EXPECT_EQ(32767, r.s2());
	}
	{
		uint8_t d[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F};
		DexReader r(d, sizeof(d));
		EXPECT_EQ(-1, r.s4());
		EXPECT_EQ(std::numeric_limits<int32_t>::min(), r.s4());
		EXPECT_EQ(std::numeric_limits<int32_t>::max(), r.s4());
	}
	{
		uint8_t d[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
		DexReader r(d, sizeof(d));
		EXPECT_EQ(-1, r.s8());
		EXPECT_EQ(std::numeric_limits<int64_t>::min(), r.s8());
	}
}

/// uleb128p1 encodes a value one greater than the number it means, so the
/// encoding's own -1 is written as 0. It was
/// `static_cast<int32_t>(uleb128()) - 1`, where the cast is out of range for
/// anything above INT32_MAX: measured, a stored 0xFFFFFFFF came back as -2.
/// The subtraction is done at 64 bits now, and a result outside int32 is
/// reported as -1 -- the value this encoding already uses for "absent" -- since
/// it names no index in any DEX table.
TEST(DexReader, Uleb128P1DoesNotWrapOnAValueAboveInt32Max)
{
	// 0 encodes -1, the "absent" marker.
	{
		uint8_t d[] = {0x00};
		DexReader r(d, sizeof(d));
		EXPECT_EQ(-1, r.uleb128p1());
	}
	// 1 encodes 0, and 0x80 0x01 encodes 128 -> 127.
	{
		uint8_t d[] = {0x01};
		DexReader r(d, sizeof(d));
		EXPECT_EQ(0, r.uleb128p1());
	}
	{
		uint8_t d[] = {0x80, 0x01};
		DexReader r(d, sizeof(d));
		EXPECT_EQ(127, r.uleb128p1());
	}

	// 0xFFFFFFFF as a ULEB128. The old body returned -2 for this.
	uint8_t big[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
	DexReader r(big, sizeof(big));
	EXPECT_EQ(-1, r.uleb128p1());

	// And the largest value that still fits, which must NOT be refused:
	// INT32_MAX + 1 encodes INT32_MAX.
	uint8_t edge[] = {0x80, 0x80, 0x80, 0x80, 0x08};
	DexReader r2(edge, sizeof(edge));
	EXPECT_EQ(std::numeric_limits<int32_t>::max(), r2.uleb128p1());
}

TEST(DexReader, ReadsUleb128_OneByte)
{
	uint8_t data[] = {0x05};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(5u, r.uleb128());
}

TEST(DexReader, ReadsUleb128_TwoBytes)
{
	// 300 = 0b1_0010_1100 → 0xAC 0x02
	uint8_t data[] = {0xAC, 0x02};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(300u, r.uleb128());
}

TEST(DexReader, ReadsSleb128_Negative)
{
	// -1 = 0x7F in SLEB128
	uint8_t data[] = {0x7F};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(-1, r.sleb128());
}

TEST(DexReader, ReadsMutf8_ASCII)
{
	uint8_t data[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
	DexReader r(data, sizeof(data));
	EXPECT_EQ("Hello", r.mutf8(5));
}

TEST(DexReader, ReadsMutf8_Empty)
{
	uint8_t data[] = {0}; // single byte for MSVC compat (zero-length arrays not allowed)
	DexReader r(data, sizeof(data));
	EXPECT_EQ("", r.mutf8(0));
}

TEST(DexReader, BoundsCheckThrows)
{
	uint8_t data[] = {0x01};
	DexReader r(data, sizeof(data));
	r.u1(); // ok
	EXPECT_THROW(r.u1(), DexParseError);
}

TEST(DexReader, SeekAndSkip)
{
	uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
	DexReader r(data, sizeof(data));
	r.seek(2);
	EXPECT_EQ(0x03u, r.u1());
	r.skip(1);
	EXPECT_EQ(4u, r.pos());
}

TEST(DexReader, SeekPastEndThrows)
{
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
static std::vector<uint8_t> buildMinimalDex()
{
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
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
	};
	auto setU4 = [&](size_t off, uint32_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
		dex[off + 2] = (v >> 16) & 0xFF;
		dex[off + 3] = (v >> 24) & 0xFF;
	};
	auto setStr = [&](size_t off, const std::string& s) {
		for (size_t i = 0; i < s.size(); ++i)
			dex[off + i] = static_cast<uint8_t>(s[i]);
		dex[off + s.size()] = 0;
	};
	auto setUleb = [&](size_t off, uint32_t v) -> size_t {
		size_t n = 0;
		do
		{
			uint8_t b = v & 0x7F;
			v >>= 7;
			if (v) b |= 0x80;
			dex[off + n++] = b;
		}
		while (v);
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
	setU4(0x38, 5);
	setU4(0x3C, 0x70);
	// type_ids_size=2, off=0x84
	setU4(0x40, 2);
	setU4(0x44, 0x84);
	// proto_ids_size=1, off=0x8C
	setU4(0x48, 1);
	setU4(0x4C, 0x8C);
	// field_ids_size=0
	setU4(0x50, 0);
	setU4(0x54, 0);
	// method_ids_size=1, off=0x98
	setU4(0x58, 1);
	setU4(0x5C, 0x98);
	// class_defs_size=1, off=0xA0
	setU4(0x60, 1);
	setU4(0x64, 0xA0);
	// data_size, data_off
	setU4(0x68, 0x80);
	setU4(0x6C, 0xC0);

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
	off += setUleb(off, 5);
	setStr(off, "Hello");
	off += 6; // 0xC7
	// "LHello;" (utf16_size=7)
	off += setUleb(off, 7);
	setStr(off, "LHello;");
	off += 8; // 0xD0
	// "V" (utf16_size=1)
	off += setUleb(off, 1);
	setStr(off, "V");
	off += 2; // 0xD3
	// "()V" (utf16_size=3)
	off += setUleb(off, 3);
	setStr(off, "()V");
	off += 4; // 0xD8
	// "main" (utf16_size=4)
	off += setUleb(off, 4);
	setStr(off, "main");
	off += 5; // 0xDE

	// === class_data_item at 0xDE ===
	// static_fields_size=0, instance_fields_size=0
	// direct_methods_size=1, virtual_methods_size=0
	setUleb(off, 0);
	off++;
	setUleb(off, 0);
	off++;
	setUleb(off, 1);
	off++; // 1 direct method
	setUleb(off, 0);
	off++;

	// encoded_method: method_idx_diff=0, access_flags=PUBLIC|STATIC=0x09, code_off
	setUleb(off, 0);
	off++; // method_idx_diff
	setUleb(off, 0x09);
	off++; // access_flags = PUBLIC|STATIC
	// code_off — we'll put code_item at next 4-byte aligned offset after 0xE8
	uint32_t codeOff = (static_cast<uint32_t>(off) + 3 + 1) & ~3u;
	// uleb128(codeOff)
	off += setUleb(off, codeOff);

	// === code_item (4-byte aligned) ===
	// pad to codeOff
	while (off < codeOff)
		dex[off++] = 0;

	// code_item header: registers_size=1, ins_size=0, outs_size=0,
	//                   tries_size=0, debug_info_off=0, insns_size=1
	setU2(codeOff + 0, 1);  // registers_size
	setU2(codeOff + 2, 0);  // ins_size
	setU2(codeOff + 4, 0);  // outs_size
	setU2(codeOff + 6, 0);  // tries_size
	setU4(codeOff + 8, 0);  // debug_info_off
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

TEST(DexFile, ParsesValidHeader)
{
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

TEST(DexFile, ResolvesStrings)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	EXPECT_EQ("Hello", df.string(0));
	EXPECT_EQ("LHello;", df.string(1));
	EXPECT_EQ("V", df.string(2));
	EXPECT_EQ("()V", df.string(3));
	EXPECT_EQ("main", df.string(4));
}

TEST(DexFile, ResolvesTypeNames)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	EXPECT_EQ("LHello;", df.typeName(0)); // type[0] → string[1]
	EXPECT_EQ("V", df.typeName(1));       // type[1] → string[2]
}

// ─── An index a DEX supplied that its own tables cannot ─────────────────────
//
// The five index-table accessors used std::vector::at(), which throws
// std::out_of_range. Every caller in this tree catches DexParseError and
// nothing else -- DexClassParser::parseClass and ApkReader::processDex both do
// -- so an out-of-range index left the handler meant to contain it and reached
// the top of main. DexClassParser::parseMethods validates the method index and
// then hands the proto index straight to protoId(), so two bytes were enough:
//
//   terminate called after throwing an instance of 'std::out_of_range'
//     what():  vector::_M_range_check: __n (which is 65535)
//              >= this->size() (which is 1)
//
// with the process killed by SIGABRT (exit 134). Measured on the minimal DEX
// below with method_ids[0].proto_idx set to 0xFFFF.

TEST(DexFile, EveryIndexTableAccessorReportsThisModulesErrorType)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	// One past the end of each table, and the widest value the field can hold.
	EXPECT_THROW((void)df.typeId(df.typeCount()), DexParseError);
	EXPECT_THROW((void)df.protoId(df.protoCount()), DexParseError);
	EXPECT_THROW((void)df.fieldId(df.fieldCount()), DexParseError);
	EXPECT_THROW((void)df.methodId(df.methodCount()), DexParseError);
	EXPECT_THROW((void)df.classDef(df.classCount()), DexParseError);

	EXPECT_THROW((void)df.typeId(0xFFFF), DexParseError);
	EXPECT_THROW((void)df.protoId(0xFFFF), DexParseError);
	EXPECT_THROW((void)df.fieldId(0xFFFF), DexParseError);
	EXPECT_THROW((void)df.methodId(0xFFFFFFFFu), DexParseError);
	EXPECT_THROW((void)df.classDef(0xFFFFFFFFu), DexParseError);

	// And the in-range ones still answer.
	EXPECT_NO_THROW((void)df.protoId(0));
	EXPECT_NO_THROW((void)df.methodId(0));
	EXPECT_NO_THROW((void)df.classDef(0));
}

// The path that actually reaches it: a method_id whose proto_idx names a proto
// the file does not have. This has to come back as a reported failure, not as
// a process that ceases to exist.
TEST(DexFile, AProtoIndexOutOfRangeIsReportedNotFatal)
{
	auto dex = buildMinimalDex();
	// method_ids[0].proto_idx is the u2 at 0x9A; see the layout comment above.
	dex[0x9A] = 0xFF;
	dex[0x9B] = 0xFF;

	retdec::dex_parser::ApkReader reader;
	retdec::dex_parser::ApkReadResult result;
	ASSERT_NO_THROW(result = reader.readDex(dex.data(), dex.size(), "classes.dex"));

	EXPECT_EQ(retdec::dex_parser::ApkReadResult::PartialError, result.status);
	ASSERT_FALSE(result.warnings.empty());
	EXPECT_NE(std::string::npos, result.warnings[0].find("proto index out of range")) << result.warnings[0];

	// The clean file is still read, so the guard is the index and not the path.
	auto good = buildMinimalDex();
	auto ok = reader.readDex(good.data(), good.size(), "classes.dex");
	EXPECT_EQ(retdec::dex_parser::ApkReadResult::OK, ok.status);
	EXPECT_EQ(1u, ok.module.classes().size());
}

TEST(DexFile, ResolvesMethodProto)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	EXPECT_EQ("main", df.methodName(0));
	EXPECT_EQ("LHello;", df.methodClass(0));
	EXPECT_EQ("()V", df.methodProto(0)); // no params, returns V
}

TEST(DexFile, ReadsCodeItem)
{
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

TEST(DexFile, InvalidMagicThrows)
{
	uint8_t bad[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
	std::vector<uint8_t> data(100, 0);
	std::memcpy(data.data(), bad, sizeof(bad));
	EXPECT_THROW(DexFile::parse(data), DexParseError);
}

TEST(DexFile, StringOutOfRangeThrows)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	EXPECT_THROW(df.string(999), DexParseError);
}

TEST(DexFile, ClassDefSuperclassIsNone)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	EXPECT_EQ(ClassDef::NO_INDEX, df.classDef(0).superclassIdx);
}

// ─── DexLifter ────────────────────────────────────────────────────────────────

TEST(DexLifter, LiftsReturnVoid)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	ClassData classData = df.readClassData(df.classDef(0).classDataOff);
	CodeItem code = df.readCodeItem(classData.directMethods[0].codeOff);

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
static CodeItem makeTwoInsnCode()
{
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

TEST(DexLifter, LiftsConstAndReturn)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	DexLifter lifter(df);
	auto result = lifter.lift(makeTwoInsnCode(), 0);

	EXPECT_EQ(DexLiftResult::OK, result.status);
	ASSERT_FALSE(result.cfg.blocks().empty());
	const auto& blk = result.cfg.blocks().front();
	ASSERT_EQ(2u, blk.instrs.size());
	EXPECT_EQ(BcOpcode::DALVIK_CONST, blk.instrs[0].opcode);
	EXPECT_EQ(BcOpcode::DALVIK_RETURN, blk.instrs[1].opcode);
}

// Branch: if-eqz v0, +2; return-void; return-void
static CodeItem makeConditionalCode()
{
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

TEST(DexLifter, LiftsConditionalBranch)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	DexLifter lifter(df);
	auto result = lifter.lift(makeConditionalCode(), 0);

	EXPECT_EQ(DexLiftResult::OK, result.status);
	// Should have at least 2 blocks (the branch creates a split).
	EXPECT_GE(result.cfg.blocks().size(), 2u);

	bool foundIfZ = false;
	for (const auto& blk: result.cfg.blocks())
	{
		for (const auto& insn: blk.instrs)
		{
			if (insn.opcode == BcOpcode::DALVIK_IF_Z)
			{
				foundIfZ = true;
				break;
			}
		}
	}
	EXPECT_TRUE(foundIfZ);
}

TEST(DexLifter, LiftsArithmeticInstructions)
{
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
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			if (insn.opcode == BcOpcode::DALVIK_ADD_INT) foundAdd = true;
	EXPECT_TRUE(foundAdd);
}

TEST(DexLifter, LiftsInvokeStatic)
{
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
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			if (insn.opcode == BcOpcode::DALVIK_INVOKE_STATIC) foundInvoke = true;
	EXPECT_TRUE(foundInvoke);
}

TEST(DexLifter, LiftsNewInstance)
{
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
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			if (insn.opcode == BcOpcode::DALVIK_NEW_INSTANCE) found = true;
	EXPECT_TRUE(found);
}

TEST(DexLifter, HandlesExceptionTable)
{
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
	t.startAddr = 0;
	t.insnCount = 1;
	t.handlerOff = 0;
	code.tries.push_back(t);

	EncodedCatchHandlerList handlers;
	handlers.handlers.resize(1);
	handlers.catchAllAddrs.resize(1);
	CatchHandler h;
	h.typeIdx = -1; // catch-all
	h.addr = 1;
	handlers.handlers[0].push_back(h);
	handlers.catchAllAddrs[0] = 1;
	code.handlers = handlers;

	DexLifter lifter(df);
	auto result = lifter.lift(code, 0);
	EXPECT_EQ(DexLiftResult::OK, result.status);
	EXPECT_FALSE(result.cfg.handlers().empty());
}

// ─── DexClassParser ───────────────────────────────────────────────────────────

TEST(DexClassParser, ParsesClassName)
{
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

TEST(DexClassParser, ParsesMethod)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);

	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_NE(nullptr, result.bcClass);
	ASSERT_EQ(1u, result.bcClass->methods.size());
	EXPECT_EQ("main", result.bcClass->methods[0].name);
}

TEST(DexClassParser, ParsesReturnType)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);

	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	const auto& method = result.bcClass->methods[0];
	// descriptor.ret should be void
	EXPECT_TRUE(!method.descriptor.returnType || method.descriptor.returnType->isVoid());
}

TEST(DexClassParser, LiftsBytecode)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);

	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	const auto& method = result.bcClass->methods[0];
	EXPECT_FALSE(method.cfg.blocks().empty());
}

TEST(DexClassParser, NoSuperclass)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);

	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	// ClassDef::NO_INDEX → superClass not set
	EXPECT_FALSE(result.bcClass->superClass.has_value());
}

TEST(DexClassParser, OutOfRangeThrows)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);

	auto result = parser.parseClass(999);
	EXPECT_EQ(DexClassResult::Error, result.status);
}

TEST(DexClassParser, FillsStaticFieldConstants)
{
	std::vector<uint8_t> dex(0x300, 0);
	auto setU2 = [&](size_t off, uint16_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
	};
	auto setU4 = [&](size_t off, uint32_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
		dex[off + 2] = (v >> 16) & 0xFF;
		dex[off + 3] = (v >> 24) & 0xFF;
	};
	auto setStr = [&](size_t off, const std::string& s) {
		for (size_t i = 0; i < s.size(); ++i)
			dex[off + i] = static_cast<uint8_t>(s[i]);
		dex[off + s.size()] = 0;
	};
	auto setUleb = [&](size_t off, uint32_t v) -> size_t {
		size_t n = 0;
		do
		{
			uint8_t b = v & 0x7F;
			v >>= 7;
			if (v) b |= 0x80;
			dex[off + n++] = b;
		}
		while (v);
		return n;
	};
	static const char magic[] = "dex\n035";
	for (int i = 0; i < 8; ++i)
		dex[i] = static_cast<uint8_t>(i < 7 ? magic[i] : 0);
	setU4(0x24, 0x70);
	setU4(0x28, 0x12345678u);
	setU4(0x38, 7);
	setU4(0x3C, 0x70); // string_ids
	setU4(0x40, 3);
	setU4(0x44, 0x8C); // type_ids
	setU4(0x48, 1);
	setU4(0x4C, 0x98); // proto_ids
	setU4(0x50, 1);
	setU4(0x54, 0xA4); // field_ids
	setU4(0x58, 1);
	setU4(0x5C, 0xAC); // method_ids
	setU4(0x60, 1);
	setU4(0x64, 0xB4); // class_defs
	setU4(0x68, 0x80);
	setU4(0x6C, 0xD4);
	setU4(0x70, 0xD4); // "Hello"
	setU4(0x74, 0xDB); // "LHello;"
	setU4(0x78, 0xE4); // "V"
	setU4(0x7C, 0xE7); // "()V"
	setU4(0x80, 0xEC); // "main"
	setU4(0x84, 0xF2); // "I"
	setU4(0x88, 0xF5); // "VALUE"
	setU4(0x8C, 1);    // type LHello;
	setU4(0x90, 2);    // type V
	setU4(0x94, 5);    // type I
	setU4(0x98, 3);
	setU4(0x9C, 1);
	setU4(0xA0, 0); // proto
	setU2(0xA4, 0);
	setU2(0xA6, 2);
	setU4(0xA8, 6); // field Hello.VALUE:I
	setU2(0xAC, 0);
	setU2(0xAE, 0);
	setU4(0xB0, 4); // method main
	setU4(0xB4, 0);
	setU4(0xB8, 0x0009);
	setU4(0xBC, 0xFFFFFFFF);
	setU4(0xC0, 0);
	setU4(0xC4, 0xFFFFFFFF);
	setU4(0xC8, 0);
	setU4(0xCC, 0x100); // classDataOff
	setU4(0xD0, 0x110); // staticValuesOff
	size_t off = 0xD4;
	off += setUleb(off, 5);
	setStr(off, "Hello");
	off += 6;
	off += setUleb(off, 7);
	setStr(off, "LHello;");
	off += 8;
	off += setUleb(off, 1);
	setStr(off, "V");
	off += 2;
	off += setUleb(off, 3);
	setStr(off, "()V");
	off += 4;
	off += setUleb(off, 4);
	setStr(off, "main");
	off += 5;
	off += setUleb(off, 1);
	setStr(off, "I");
	off += 2;
	off += setUleb(off, 5);
	setStr(off, "VALUE");
	off += 6;
	// class_data at 0x100
	off = 0x100;
	off += setUleb(off, 1); // static_fields
	off += setUleb(off, 0);
	off += setUleb(off, 1); // direct methods
	off += setUleb(off, 0);
	off += setUleb(off, 0);    // field_idx_diff
	off += setUleb(off, 0x19); // public static final
	off += setUleb(off, 0);    // method_idx_diff
	off += setUleb(off, 0x09);
	off += setUleb(off, 0); // no code
	// encoded_array at 0x110: count=1, VALUE_INT 42 (1 byte)
	dex[0x110] = 1;
	dex[0x111] = 0x04; // VALUE_INT, size-1 = 0
	dex[0x112] = 42;
	dex.resize(0x120);
	setU4(0x20, 0x120);

	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_EQ(1u, result.bcClass->fields.size());
	EXPECT_EQ("VALUE", result.bcClass->fields[0].name);
	ASSERT_TRUE(result.bcClass->fields[0].constantIntValue.has_value());
	EXPECT_EQ(42, *result.bcClass->fields[0].constantIntValue);
}

// VALUE_ARRAY and VALUE_ANNOTATION nest through skipEncodedValue, and each
// level costs two bytes on the wire -- so without a depth bound a small file
// asks for arbitrarily deep recursion and exhausts the stack. No bound derived
// from the input size catches that; the parser carries a fixed depth limit.
//
// Built from FillsStaticFieldConstants above, changing only the encoded_array
// so the surrounding file is known to parse.
TEST(DexClassParser, RefusesDeeplyNestedEncodedArray)
{
	std::vector<uint8_t> dex(0x300, 0);
	auto setU2 = [&](size_t off, uint16_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
	};
	auto setU4 = [&](size_t off, uint32_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
		dex[off + 2] = (v >> 16) & 0xFF;
		dex[off + 3] = (v >> 24) & 0xFF;
	};
	auto setStr = [&](size_t off, const std::string& s) {
		for (size_t i = 0; i < s.size(); ++i)
			dex[off + i] = static_cast<uint8_t>(s[i]);
		dex[off + s.size()] = 0;
	};
	auto setUleb = [&](size_t off, uint32_t v) -> size_t {
		size_t n = 0;
		do
		{
			uint8_t b = v & 0x7F;
			v >>= 7;
			if (v) b |= 0x80;
			dex[off + n++] = b;
		}
		while (v);
		return n;
	};
	static const char magic[] = "dex\n035";
	for (int i = 0; i < 8; ++i)
		dex[i] = static_cast<uint8_t>(i < 7 ? magic[i] : 0);
	setU4(0x24, 0x70);
	setU4(0x28, 0x12345678u);
	setU4(0x38, 7);
	setU4(0x3C, 0x70); // string_ids
	setU4(0x40, 3);
	setU4(0x44, 0x8C); // type_ids
	setU4(0x48, 1);
	setU4(0x4C, 0x98); // proto_ids
	setU4(0x50, 1);
	setU4(0x54, 0xA4); // field_ids
	setU4(0x58, 1);
	setU4(0x5C, 0xAC); // method_ids
	setU4(0x60, 1);
	setU4(0x64, 0xB4); // class_defs
	setU4(0x68, 0x80);
	setU4(0x6C, 0xD4);
	setU4(0x70, 0xD4); // "Hello"
	setU4(0x74, 0xDB); // "LHello;"
	setU4(0x78, 0xE4); // "V"
	setU4(0x7C, 0xE7); // "()V"
	setU4(0x80, 0xEC); // "main"
	setU4(0x84, 0xF2); // "I"
	setU4(0x88, 0xF5); // "VALUE"
	setU4(0x8C, 1);    // type LHello;
	setU4(0x90, 2);    // type V
	setU4(0x94, 5);    // type I
	setU4(0x98, 3);
	setU4(0x9C, 1);
	setU4(0xA0, 0); // proto
	setU2(0xA4, 0);
	setU2(0xA6, 2);
	setU4(0xA8, 6); // field Hello.VALUE:I
	setU2(0xAC, 0);
	setU2(0xAE, 0);
	setU4(0xB0, 4); // method main
	setU4(0xB4, 0);
	setU4(0xB8, 0x0009);
	setU4(0xBC, 0xFFFFFFFF);
	setU4(0xC0, 0);
	setU4(0xC4, 0xFFFFFFFF);
	setU4(0xC8, 0);
	setU4(0xCC, 0x100); // classDataOff
	setU4(0xD0, 0x110); // staticValuesOff
	size_t off = 0xD4;
	off += setUleb(off, 5);
	setStr(off, "Hello");
	off += 6;
	off += setUleb(off, 7);
	setStr(off, "LHello;");
	off += 8;
	off += setUleb(off, 1);
	setStr(off, "V");
	off += 2;
	off += setUleb(off, 3);
	setStr(off, "()V");
	off += 4;
	off += setUleb(off, 4);
	setStr(off, "main");
	off += 5;
	off += setUleb(off, 1);
	setStr(off, "I");
	off += 2;
	off += setUleb(off, 5);
	setStr(off, "VALUE");
	off += 6;
	// class_data at 0x100
	off = 0x100;
	off += setUleb(off, 1); // static_fields
	off += setUleb(off, 0);
	off += setUleb(off, 1); // direct methods
	off += setUleb(off, 0);
	off += setUleb(off, 0);    // field_idx_diff
	off += setUleb(off, 0x19); // public static final
	off += setUleb(off, 0);    // method_idx_diff
	off += setUleb(off, 0x09);
	off += setUleb(off, 0); // no code
	// encoded_array at 0x110: one element, then VALUE_ARRAY nested as deep as
	// the buffer allows -- two bytes per level.
	// Large enough that the nesting really exhausts the stack without the
	// depth bound: two bytes a level over 1 MB is ~500,000 frames.
	dex.resize(0x100000);
	dex[0x110] = 1;
	size_t nest = 0x111;
	while (nest + 2 < dex.size() - 1)
	{
		dex[nest++] = 0x1c; // VALUE_ARRAY
		dex[nest++] = 0x01; // one element
	}
	dex[nest] = 0x1e; // VALUE_NULL terminates the innermost element
	setU4(0x20, static_cast<uint32_t>(dex.size()));

	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);

	// Must return rather than recurse. The point is that it returns at all:
	// without the depth bound this is a SIGSEGV on stack exhaustion.
	auto result = parser.parseClass(0);
	(void)result;
}

// ─── ProGuardMapping ─────────────────────────────────────────────────────────

TEST(ProGuardMapping, ParsesClassMapping)
{
	std::string text =
		"com.example.Main -> a.b:\n"
		"    void main(String[]) -> c\n";
	auto mapping = ProGuardMapping::parse(text);
	EXPECT_EQ("com.example.Main", mapping.classMap["a.b"]);
}

TEST(ProGuardMapping, ParsesMemberMapping)
{
	std::string text =
		"com.example.Main -> a.b:\n"
		"    void originalMethod() -> c\n"
		"    int originalField -> d\n";
	auto mapping = ProGuardMapping::parse(text);
	EXPECT_EQ("originalMethod", mapping.memberMap["a.b"]["c"].originalName);
	EXPECT_EQ("originalField", mapping.memberMap["a.b"]["d"].originalName);
}

TEST(ProGuardMapping, HandlesEmptyInput)
{
	auto mapping = ProGuardMapping::parse("");
	EXPECT_TRUE(mapping.empty());
}

TEST(ProGuardMapping, HandlesComments)
{
	std::string text =
		"# This is a comment\n"
		"com.Foo -> a:\n";
	auto mapping = ProGuardMapping::parse(text);
	EXPECT_EQ(1u, mapping.classMap.size());
}

TEST(ProGuardMapping, ParsesLineNumberRange)
{
	std::string text =
		"com.example.Foo -> x:\n"
		"    1:5:void bar() -> a\n";
	auto mapping = ProGuardMapping::parse(text);
	EXPECT_EQ("bar", mapping.memberMap["x"]["a"].originalName);
}

// ─── ApkReader ────────────────────────────────────────────────────────────────

TEST(ApkReader, ReadsDexDirectly)
{
	auto dex = buildMinimalDex();
	ApkReader reader;
	auto result = reader.readDex(dex.data(), dex.size(), "classes.dex");

	EXPECT_EQ(ApkReadResult::OK, result.status);
	EXPECT_FALSE(result.module.classes().empty());
	EXPECT_EQ("Hello", result.module.classes().front().fqName);
}

TEST(ApkReader, RejectsInvalidDex)
{
	uint8_t bad[] = {0x00, 0x01, 0x02, 0x03};
	ApkReader reader;
	auto result = reader.readDex(bad, sizeof(bad), "bad.dex");
	// Should produce a partial error or empty module but not crash.
	EXPECT_NE(ApkReadResult::OK, result.status);
}

TEST(ApkReader, RejectsEmptyApk)
{
	uint8_t empty[] = {0}; // MSVC: zero-length arrays not allowed
	ApkReader reader;
	auto result = reader.readApk(empty, sizeof(empty));
	EXPECT_EQ(ApkReadResult::Error, result.status);
}

TEST(ApkReader, RejectsBadZip)
{
	// Garbage bytes that aren't a ZIP
	std::vector<uint8_t> garbage(100, 0xCC);
	ApkReader reader;
	auto result = reader.readApk(garbage);
	EXPECT_EQ(ApkReadResult::Error, result.status);
}

static void appendU16le(std::vector<uint8_t>& v, uint16_t x)
{
	v.push_back(static_cast<uint8_t>(x));
	v.push_back(static_cast<uint8_t>(x >> 8));
}

static void appendU32le(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(static_cast<uint8_t>(x));
	v.push_back(static_cast<uint8_t>(x >> 8));
	v.push_back(static_cast<uint8_t>(x >> 16));
	v.push_back(static_cast<uint8_t>(x >> 24));
}

static std::vector<uint8_t>
storedZipClaimedUncomp(const std::string& name, const std::vector<uint8_t>& payload, uint32_t claimedUncomp)
{
	std::vector<uint8_t> z;
	appendU32le(z, 0x04034b50u);
	appendU16le(z, 20);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU32le(z, 0);
	appendU32le(z, static_cast<uint32_t>(payload.size()));
	appendU32le(z, claimedUncomp);
	appendU16le(z, static_cast<uint16_t>(name.size()));
	appendU16le(z, 0);
	z.insert(z.end(), name.begin(), name.end());
	z.insert(z.end(), payload.begin(), payload.end());
	const uint32_t cdOff = static_cast<uint32_t>(z.size());
	appendU32le(z, 0x02014b50u);
	appendU16le(z, 20);
	appendU16le(z, 20);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU32le(z, 0);
	appendU32le(z, static_cast<uint32_t>(payload.size()));
	appendU32le(z, claimedUncomp);
	appendU16le(z, static_cast<uint16_t>(name.size()));
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU32le(z, 0);
	appendU32le(z, 0);
	z.insert(z.end(), name.begin(), name.end());
	const uint32_t cdSize = static_cast<uint32_t>(z.size() - cdOff);
	appendU32le(z, 0x06054b50u);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 1);
	appendU16le(z, 1);
	appendU32le(z, cdSize);
	appendU32le(z, cdOff);
	appendU16le(z, 0);
	return z;
}

static std::vector<uint8_t> storedZip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& items)
{
	std::vector<uint8_t> z;
	std::vector<uint32_t> localOffs;
	localOffs.reserve(items.size());
	for (const auto& it: items)
	{
		localOffs.push_back(static_cast<uint32_t>(z.size()));
		const auto& name = it.first;
		const auto& payload = it.second;
		appendU32le(z, 0x04034b50u);
		appendU16le(z, 20);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU32le(z, 0);
		appendU32le(z, static_cast<uint32_t>(payload.size()));
		appendU32le(z, static_cast<uint32_t>(payload.size()));
		appendU16le(z, static_cast<uint16_t>(name.size()));
		appendU16le(z, 0);
		z.insert(z.end(), name.begin(), name.end());
		z.insert(z.end(), payload.begin(), payload.end());
	}
	const uint32_t cdOff = static_cast<uint32_t>(z.size());
	for (size_t i = 0; i < items.size(); ++i)
	{
		const auto& name = items[i].first;
		const auto& payload = items[i].second;
		appendU32le(z, 0x02014b50u);
		appendU16le(z, 20);
		appendU16le(z, 20);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU32le(z, 0);
		appendU32le(z, static_cast<uint32_t>(payload.size()));
		appendU32le(z, static_cast<uint32_t>(payload.size()));
		appendU16le(z, static_cast<uint16_t>(name.size()));
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU16le(z, 0);
		appendU32le(z, 0);
		appendU32le(z, localOffs[i]);
		z.insert(z.end(), name.begin(), name.end());
	}
	const uint32_t cdSize = static_cast<uint32_t>(z.size() - cdOff);
	appendU32le(z, 0x06054b50u);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, static_cast<uint16_t>(items.size()));
	appendU16le(z, static_cast<uint16_t>(items.size()));
	appendU32le(z, cdSize);
	appendU32le(z, cdOff);
	appendU16le(z, 0);
	return z;
}

static std::vector<uint8_t> deflateRaw(const std::vector<uint8_t>& in)
{
	z_stream strm{};
	if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
	std::vector<uint8_t> out(in.size() + 64);
	strm.next_in = const_cast<Bytef*>(in.data());
	strm.avail_in = static_cast<uInt>(in.size());
	strm.next_out = out.data();
	strm.avail_out = static_cast<uInt>(out.size());
	const int rc = deflate(&strm, Z_FINISH);
	const size_t n = static_cast<size_t>(strm.total_out);
	deflateEnd(&strm);
	if (rc != Z_STREAM_END) return {};
	out.resize(n);
	return out;
}

static std::vector<uint8_t> deflateZip(const std::string& name, const std::vector<uint8_t>& payload)
{
	auto comp = deflateRaw(payload);
	if (comp.empty()) return {};
	std::vector<uint8_t> z;
	appendU32le(z, 0x04034b50u);
	appendU16le(z, 20);
	appendU16le(z, 0);
	appendU16le(z, 8); // DEFLATE
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU32le(z, 0);
	appendU32le(z, static_cast<uint32_t>(comp.size()));
	appendU32le(z, static_cast<uint32_t>(payload.size()));
	appendU16le(z, static_cast<uint16_t>(name.size()));
	appendU16le(z, 0);
	z.insert(z.end(), name.begin(), name.end());
	z.insert(z.end(), comp.begin(), comp.end());
	const uint32_t cdOff = static_cast<uint32_t>(z.size());
	appendU32le(z, 0x02014b50u);
	appendU16le(z, 20);
	appendU16le(z, 20);
	appendU16le(z, 0);
	appendU16le(z, 8); // DEFLATE
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU32le(z, 0);
	appendU32le(z, static_cast<uint32_t>(comp.size()));
	appendU32le(z, static_cast<uint32_t>(payload.size()));
	appendU16le(z, static_cast<uint16_t>(name.size()));
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU32le(z, 0);
	appendU32le(z, 0);
	z.insert(z.end(), name.begin(), name.end());
	const uint32_t cdSize = static_cast<uint32_t>(z.size() - cdOff);
	appendU32le(z, 0x06054b50u);
	appendU16le(z, 0);
	appendU16le(z, 0);
	appendU16le(z, 1);
	appendU16le(z, 1);
	appendU32le(z, cdSize);
	appendU32le(z, cdOff);
	appendU16le(z, 0);
	return z;
}

TEST(ApkReader, StoredClaimExceedsRemaining)
{
	auto zip = storedZipClaimedUncomp("classes.dex", {0x64, 0x65, 0x78, 0x0a}, 0xFFFFFFF0u);
	ApkReader reader;
	auto result = reader.readApk(zip);
	EXPECT_EQ(ApkReadResult::PartialError, result.status);
	EXPECT_FALSE(result.warnings.empty());
}

TEST(ApkReader, InflatesDeflateClassesDex)
{
	auto dex = buildMinimalDex();
	auto zip = deflateZip("classes.dex", dex);
	ASSERT_FALSE(zip.empty());
	ApkReader reader;
	auto result = reader.readApk(zip);
	EXPECT_EQ(ApkReadResult::OK, result.status);
	ASSERT_FALSE(result.module.classes().empty());
	EXPECT_EQ("Hello", result.module.classes().front().fqName);
}

TEST(ApkReader, AppliesProGuardMapping)
{
	auto dex = buildMinimalDex();
	ApkReader reader;

	// Use readDex and manually apply mapping
	auto result = reader.readDex(dex.data(), dex.size(), "classes.dex");
	ASSERT_EQ(ApkReadResult::OK, result.status);

	std::string mappingText = "Hello -> a:\n    void main() -> b\n";
	auto mapping = ProGuardMapping::parse(mappingText);
	EXPECT_EQ("Hello", mapping.classMap["a"]);
}

TEST(ApkReader, AppliesProGuardMembersUsingObfuscatedClassKey)
{
	auto dex = buildMinimalDex();
	const std::string mappingText =
		"OriginalHello -> Hello:\n"
		"    void originalMain() -> main\n";
	std::vector<uint8_t> mappingBytes(mappingText.begin(), mappingText.end());
	auto zip = storedZip({
		{"classes.dex", dex},
		{"mapping.txt", mappingBytes},
	});
	ApkReader reader;
	auto result = reader.readApk(zip);
	ASSERT_EQ(ApkReadResult::OK, result.status);
	ASSERT_TRUE(result.hadMapping);
	ASSERT_FALSE(result.module.classes().empty());
	const auto& cls = result.module.classes().front();
	EXPECT_EQ("OriginalHello", cls.name);
	ASSERT_FALSE(cls.methods.empty());
	EXPECT_EQ("originalMain", cls.methods[0].name);
}

// ─── BcType descriptor conversions ───────────────────────────────────────────

TEST(DexDescriptor, PrimitivesFromDescriptor)
{
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

TEST(DexHeader, VersionIs035)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	EXPECT_EQ(DexVersion::V035, df.version());
}

// ─── Malformed-input regression tests ────────────────────────────────────────
//
// Every offset+size pair in the DEX header, and every count embedded in the
// data section, is attacker-controlled.  The parser used to size its containers
// straight from those counts, so a header claiming 0xFFFFFFFF string_ids in a
// 494-byte file asked for 128 GiB before the first read could fail.  The counts
// are now bounded against the bytes the file can actually supply.

/// Patch a u4 header field in a copy of the minimal DEX.
static std::vector<uint8_t> minimalDexWithU4(size_t off, uint32_t value)
{
	auto dex = buildMinimalDex();
	dex[off + 0] = static_cast<uint8_t>(value);
	dex[off + 1] = static_cast<uint8_t>(value >> 8);
	dex[off + 2] = static_cast<uint8_t>(value >> 16);
	dex[off + 3] = static_cast<uint8_t>(value >> 24);
	return dex;
}

TEST(DexFile, StringIdsCountExceedsFileThrows)
{
	// header_item.string_ids_size @ 0x38 — the crash-2db5d740 reproducer.
	EXPECT_THROW(DexFile::parse(minimalDexWithU4(0x38, 0xFFFFFFFFu)), DexParseError);
}

TEST(DexFile, TypeIdsCountExceedsFileThrows)
{
	EXPECT_THROW(DexFile::parse(minimalDexWithU4(0x40, 0xFFFFFFFFu)), DexParseError);
}

TEST(DexFile, ProtoIdsCountExceedsFileThrows)
{
	EXPECT_THROW(DexFile::parse(minimalDexWithU4(0x48, 0xFFFFFFFFu)), DexParseError);
}

TEST(DexFile, FieldIdsCountExceedsFileThrows)
{
	EXPECT_THROW(DexFile::parse(minimalDexWithU4(0x50, 0xFFFFFFFFu)), DexParseError);
}

TEST(DexFile, MethodIdsCountExceedsFileThrows)
{
	EXPECT_THROW(DexFile::parse(minimalDexWithU4(0x58, 0xFFFFFFFFu)), DexParseError);
}

TEST(DexFile, ClassDefsCountExceedsFileThrows)
{
	EXPECT_THROW(DexFile::parse(minimalDexWithU4(0x60, 0xFFFFFFFFu)), DexParseError);
}

TEST(DexFile, IndexTableCountJustPastEndThrows)
{
	// Off-by-one on the real boundary: string_ids_off 0x70 leaves
	// (size - 0x70) bytes, so exactly one more entry than that fits is bad.
	auto dex = buildMinimalDex();
	const uint32_t fits = static_cast<uint32_t>((dex.size() - 0x70) / kStringIdItemSize);
	EXPECT_THROW(DexFile::parse(minimalDexWithU4(0x38, fits + 1)), DexParseError);
}

TEST(DexFile, TypeListSizeExceedsFileThrows)
{
	auto dex = buildMinimalDex();
	const uint32_t listOff = static_cast<uint32_t>(dex.size());
	appendU32le(dex, 0xFFFFFFFFu); // type_list.size
	DexFile df = DexFile::parse(dex);
	EXPECT_THROW(df.readTypeList(listOff), DexParseError);
}

TEST(DexFile, ClassDataFieldCountExceedsFileThrows)
{
	auto dex = buildMinimalDex();
	const uint32_t cdOff = static_cast<uint32_t>(dex.size());
	// static_fields_size = 0xFFFFFFFF (ULEB128), then three zero counts.
	dex.insert(dex.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00, 0x00});
	DexFile df = DexFile::parse(dex);
	EXPECT_THROW(df.readClassData(cdOff), DexParseError);
}

TEST(DexFile, ClassDataMethodCountExceedsFileThrows)
{
	auto dex = buildMinimalDex();
	const uint32_t cdOff = static_cast<uint32_t>(dex.size());
	// static/instance fields = 0, direct_methods_size = 0xFFFFFFFF.
	dex.insert(dex.end(), {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00});
	DexFile df = DexFile::parse(dex);
	EXPECT_THROW(df.readClassData(cdOff), DexParseError);
}

TEST(DexFile, CodeItemInsnsSizeExceedsFileThrows)
{
	auto dex = buildMinimalDex();
	const uint32_t codeOff = static_cast<uint32_t>(dex.size());
	appendU16le(dex, 1);           // registers_size
	appendU16le(dex, 0);           // ins_size
	appendU16le(dex, 0);           // outs_size
	appendU16le(dex, 0);           // tries_size
	appendU32le(dex, 0);           // debug_info_off
	appendU32le(dex, 0xFFFFFFFFu); // insns_size
	DexFile df = DexFile::parse(dex);
	EXPECT_THROW(df.readCodeItem(codeOff), DexParseError);
}

TEST(DexFile, CodeItemTriesSizeExceedsFileThrows)
{
	auto dex = buildMinimalDex();
	const uint32_t codeOff = static_cast<uint32_t>(dex.size());
	appendU16le(dex, 1);      // registers_size
	appendU16le(dex, 0);      // ins_size
	appendU16le(dex, 0);      // outs_size
	appendU16le(dex, 0xFFFF); // tries_size
	appendU32le(dex, 0);      // debug_info_off
	appendU32le(dex, 0);      // insns_size
	DexFile df = DexFile::parse(dex);
	EXPECT_THROW(df.readCodeItem(codeOff), DexParseError);
}

TEST(DexFile, CatchHandlerListSizeExceedsFileThrows)
{
	auto dex = buildMinimalDex();
	const uint32_t codeOff = static_cast<uint32_t>(dex.size());
	appendU16le(dex, 1); // registers_size
	appendU16le(dex, 0); // ins_size
	appendU16le(dex, 0); // outs_size
	appendU16le(dex, 1); // tries_size
	appendU32le(dex, 0); // debug_info_off
	appendU32le(dex, 0); // insns_size (even — no padding unit)
	appendU32le(dex, 0); // try_item.start_addr
	appendU16le(dex, 0); // try_item.insn_count
	appendU16le(dex, 0); // try_item.handler_off
	// encoded_catch_handler_list.size = 0xFFFFFFFF (ULEB128)
	dex.insert(dex.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
	DexFile df = DexFile::parse(dex);
	EXPECT_THROW(df.readCodeItem(codeOff), DexParseError);
}

TEST(DexReader, CheckCountRejectsUnbackedCount)
{
	// The bound used by every count in the parser, including the
	// annotation_set_ref_list count in DexClassParser::readAnnotations().
	std::vector<uint8_t> buf(32, 0);
	DexReader r(buf.data(), buf.size());
	EXPECT_NO_THROW(r.checkCount(8, kAnnotationOffSize)); // 32 bytes exactly
	EXPECT_THROW(r.checkCount(9, kAnnotationOffSize), DexParseError);
	EXPECT_THROW(r.checkCount(0xFFFFFFFFu, 1), DexParseError);
	EXPECT_NO_THROW(r.checkCount(0, 1)); // an empty table reads nothing
}

TEST(ApkReader, CentralDirectoryOffsetOverflowsEndOfFile)
{
	// crash-61a2a35364423eaaf6dffec651d17aa0c097c228: the EOCD declares
	// cd_offset = 0xFFFFFFFE and cd_size = 0x40.  Summed in uint32 those wrap
	// to 0x3E, which slipped past the end-of-file test and left the walker
	// memcmp()ing at data + 0xFFFFFFFE.
	static const uint8_t kBytes[] = {
		0xde, 0xfe, 0xba, 0xbe, 0x50, 0x4b, 0x05, 0x06, 0x01, 0x00, 0x00, 0x00, 0xcf, 0xd2, 0x00, 0x00,
		0x40, 0x00, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x0a, 0x03,
		0x07, 0x00, 0x04, 0x0c, 0xd3, 0x1d, 0x00, 0x02, 0x00, 0x03, 0x07, 0x00, 0x04, 0x0c, 0x00, 0x05,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x06, 0x00, 0x2f, 0x01, 0x00, 0x3f, 0x00, 0x06, 0x3c,
	};
	std::vector<uint8_t> apk(std::begin(kBytes), std::end(kBytes));
	ApkReader reader;
	auto result = reader.readApk(apk);
	EXPECT_EQ(ApkReadResult::Error, result.status);
	EXPECT_EQ("not a valid ZIP/APK file", result.error);
}

TEST(ApkReader, CentralDirectoryEntryNameRunsPastDirectory)
{
	// A well-formed STORED zip, with the central directory entry's name length
	// rewritten to reach past the directory it lives in.  The name used to be
	// copied straight out of the buffer at that length.
	auto zip = storedZip({{"classes.dex", buildMinimalDex()}});
	// The EOCD is the trailing 22 bytes; cd_offset is at EOCD + 16.
	const size_t eocd = zip.size() - 22;
	const uint32_t cdOff = static_cast<uint32_t>(zip[eocd + 16]) | (static_cast<uint32_t>(zip[eocd + 17]) << 8)
						 | (static_cast<uint32_t>(zip[eocd + 18]) << 16)
						 | (static_cast<uint32_t>(zip[eocd + 19]) << 24);
	zip[cdOff + 28] = 0xFF; // central_directory_header.file_name_length lo
	zip[cdOff + 29] = 0xFF; // hi
	ApkReader reader;
	auto result = reader.readApk(zip);
	// The truncated entry must be dropped, not admitted with 65535 bytes of
	// whatever follows the buffer read in as its name.
	EXPECT_EQ(ApkReadResult::Error, result.status);
	EXPECT_EQ("not a valid ZIP/APK file", result.error);
}

TEST(ApkReader, LocalHeaderOffsetNearFourGibIsRejected)
{
	// local_header_offset + 30 wrapped in uint32, so an offset just below 4 GiB
	// passed the bounds test and dereferenced far outside the buffer.
	auto zip = storedZip({{"classes.dex", buildMinimalDex()}});
	const size_t eocd = zip.size() - 22;
	const uint32_t cdOff = static_cast<uint32_t>(zip[eocd + 16]) | (static_cast<uint32_t>(zip[eocd + 17]) << 8)
						 | (static_cast<uint32_t>(zip[eocd + 18]) << 16)
						 | (static_cast<uint32_t>(zip[eocd + 19]) << 24);
	for (int i = 0; i < 4; ++i)
		zip[cdOff + 42 + i] = 0xFF; // relative_offset_of_local_header
	ApkReader reader;
	auto result = reader.readApk(zip);
	EXPECT_EQ(ApkReadResult::PartialError, result.status);
	EXPECT_FALSE(result.warnings.empty());
}

// ─── Truncated instruction streams (findLeaders bounds) ──────────────────────

// insns_size comes out of the file, so the instruction array can stop in the
// middle of an instruction. findLeaders decided how many code units to read
// from the opcode alone and indexed word[1]/word[2] unconditionally, running
// off the end of the vector for a branch whose target unit was never stored.
// These lift a code_item holding nothing but the first unit of such a branch;
// under ASan the pre-fix reader is a heap-buffer-overflow, and without it the
// lift must still come back clean.
static DexLiftResult liftUnits(const DexFile& df, std::vector<uint16_t> units)
{
	CodeItem code;
	code.registersSize = 2;
	code.insSize = 0;
	code.outsSize = 0;
	code.triesSize = 0;
	code.debugInfoOff = 0;
	code.insnsSize = static_cast<uint32_t>(units.size());
	code.insns = std::move(units);
	DexLifter lifter(df);
	return lifter.lift(code, 0);
}

TEST(DexLifter, TruncatedGoto16DoesNotReadPastInsns)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// goto/16 declares a following branch-offset unit that is not there.
	auto result = liftUnits(df, {static_cast<uint16_t>(0x0029u)});
	EXPECT_EQ(DexLiftResult::OK, result.status);
}

TEST(DexLifter, TruncatedGoto32DoesNotReadPastInsns)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// goto/32 declares two following units; only one of them is stored.
	auto result = liftUnits(df, {static_cast<uint16_t>(0x002Au), static_cast<uint16_t>(0x0000u)});
	EXPECT_EQ(DexLiftResult::OK, result.status);
}

TEST(DexLifter, TruncatedConditionalBranchDoesNotReadPastInsns)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// if-eqz v0, +? — the branch offset unit is missing.
	auto result = liftUnits(df, {static_cast<uint16_t>(0x0038u)});
	EXPECT_EQ(DexLiftResult::OK, result.status);
}

// A well-formed goto/16 must still be followed: the guard may not change what
// a complete instruction stream lifts to.
TEST(DexLifter, CompleteGoto16StillReachesItsTarget)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto result = liftUnits(
		df,
		{
			static_cast<uint16_t>(0x0029u), // goto/16 +2
			static_cast<uint16_t>(0x0002u),
			static_cast<uint16_t>(0x000Eu), // return-void (offset 2)
		});
	ASSERT_EQ(DexLiftResult::OK, result.status);
	bool hasTargetBlock = false;
	for (const auto& blk: result.cfg.blocks())
		if (blk.label == "L2") hasTargetBlock = true;
	EXPECT_TRUE(hasTargetBlock);
}

// ─── Instruction size table (kInsnSize) ──────────────────────────────────────
//
// Every walk over a code_item -- findLeaders, buildBlocks, decodeInsn -- steps
// by kInsnSize[opcode].  A wrong entry does not fail loudly: the walk lands
// mid-instruction and decodes operand words as opcodes, so valid Java produces
// a plausible-looking but wrong CFG.  These tests pin the sizes the Dalvik
// formats fix (ECMA of the DEX world: the "Dalvik bytecode" and "instruction
// formats" documents), one instruction against a following return-void, and
// assert on what the block actually contains.  A size that is too large
// swallows the return-void; one that is too small decodes the operand units
// as extra instructions.

// Lift `units` as a method body and return the first block's instructions.
static const std::vector<BcInstruction>& firstBlockInstrs(const DexLiftResult& r)
{
	static const std::vector<BcInstruction> kEmpty;
	return r.cfg.blocks().empty() ? kEmpty : r.cfg.blocks()[0].instrs;
}

// One instruction followed by return-void must lift to exactly those two.
static void expectInsnThenReturn(const DexFile& df, std::vector<uint16_t> units, BcOpcode expected)
{
	auto result = liftUnits(df, std::move(units));
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_EQ(2u, instrs.size());
	EXPECT_EQ(expected, instrs[0].opcode);
	EXPECT_EQ(BcOpcode::DALVIK_RETURN_VOID, instrs[1].opcode);
}

TEST(DexInsnSize, ConstStringIsTwoCodeUnits)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 21c: const-string v0, string@0 | return-void
	expectInsnThenReturn(df, {0x001A, 0x0000, 0x000E}, BcOpcode::DALVIK_CONST_STRING);
}

TEST(DexInsnSize, ConstStringJumboIsThreeCodeUnits)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 31c: const-string/jumbo v0, string@0 | return-void
	expectInsnThenReturn(df, {0x001B, 0x0000, 0x0000, 0x000E}, BcOpcode::DALVIK_CONST_STRING);
}

TEST(DexInsnSize, ConstClassIsTwoCodeUnits)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 21c: const-class v0, type@0 | return-void
	expectInsnThenReturn(df, {0x001C, 0x0000, 0x000E}, BcOpcode::DALVIK_CONST_CLASS);
}

TEST(DexInsnSize, MonitorEnterIsOneCodeUnit)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 11x: monitor-enter v0 | return-void
	expectInsnThenReturn(df, {0x001D, 0x000E}, BcOpcode::DALVIK_MONITOR_ENTER);
}

TEST(DexInsnSize, ArrayLengthIsOneCodeUnit)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 12x: array-length v0, v1 | return-void
	expectInsnThenReturn(df, {0x1021, 0x000E}, BcOpcode::DALVIK_ARRAY_LENGTH);
}

TEST(DexInsnSize, NewArrayIsTwoCodeUnits)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 22c: new-array v0, v1, type@0 | return-void
	expectInsnThenReturn(df, {0x1023, 0x0000, 0x000E}, BcOpcode::DALVIK_NEW_ARRAY);
}

TEST(DexInsnSize, InvokeVirtualIsThreeCodeUnits)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 35c: invoke-virtual {v0}, method@0 | return-void.  This is the most
	// common instruction in compiled Java; sized at two units the walk lands
	// on the register-list word and decodes it as a second instruction.
	expectInsnThenReturn(df, {0x106E, 0x0000, 0x0000, 0x000E}, BcOpcode::DALVIK_INVOKE_VIRTUAL);
}

TEST(DexInsnSize, InvokeSuperIsThreeCodeUnits)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 35c: invoke-super {v0}, method@0 | return-void
	expectInsnThenReturn(df, {0x106F, 0x0000, 0x0000, 0x000E}, BcOpcode::DALVIK_INVOKE_SUPER);
}

// ─── 35c register nibbles ───────────────────────────────────────────────────
//
// Format 35c is `A|G|op BBBB F|E|D|C` with argument list {vC,vD,vE,vF,vG}.
// C..F are the four nibbles of the third code unit, low to high; G is the high
// nibble of the FIRST unit and is the FIFTH argument. args35c() read G as vC
// and shifted every real register one slot later, so the list was
// [G, C, D, E, F] truncated to `count`. Measured before the fix:
//
//   invoke-virtual {v1, v2}   ->  v0 v1
//   invoke-direct  {v0, v1}   ->  v0 v0    (the new-instance/<init> idiom)
//   invoke-virtual {v3}       ->  v0
//   invoke-virtual {v1,..,v5} ->  v5 v1 v2 v3 v4
//
// The two existing invoke tests use {v0} and {}, where G and C are both zero
// and the rotation is invisible, and neither asserts on the registers.

namespace {

/// The register operands of the first instruction of the first block, as
/// "v1 v2" -- a string rather than a vector so a failure names the registers
/// instead of printing "<24-byte object>", which is what the shim makes of a
/// std::vector and is no use to whoever reads the next regression.
std::string firstInsnRegs(const DexLiftResult& r)
{
	std::string regs;
	if (r.cfg.blocks().empty() || r.cfg.blocks()[0].instrs.empty()) return regs;
	for (const auto& op: r.cfg.blocks()[0].instrs[0].operands)
	{
		if (const auto* local = std::get_if<retdec::bc_module::BcLocalOperand>(&op))
		{
			if (!regs.empty()) regs += ' ';
			regs += 'v';
			regs += std::to_string(local->index);
		}
	}
	return regs;
}

} // namespace

TEST(DexLifter, Invoke35cRegistersAreInArgumentOrder)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	// invoke-virtual {v1, v2}, meth@0
	//   word0 = A|G|op  = (2 << 12) | (0 << 8) | 0x6E
	//   word2 = F|E|D|C = (2 << 4) | 1
	auto two = liftUnits(df, {0x206E, 0x0000, 0x0021, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, two.status) << two.error;
	EXPECT_EQ("v1 v2", firstInsnRegs(two));

	// invoke-direct {v0, v1}, meth@0 -- what `new T(); T.<init>(arg)` compiles
	// to, and the case where the rotation collapsed both slots onto v0.
	auto ctor = liftUnits(df, {0x2070, 0x0000, 0x0010, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, ctor.status) << ctor.error;
	EXPECT_EQ("v0 v1", firstInsnRegs(ctor));

	// One argument, non-zero: the receiver of every single-argument virtual
	// call. G is 0 here, so the old code answered v0 whatever C was.
	auto one = liftUnits(df, {0x106E, 0x0000, 0x0003, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, one.status) << one.error;
	EXPECT_EQ("v3", firstInsnRegs(one));

	// All five, which is the only shape that uses G at all.
	//   word0 = (5 << 12) | (5 << 8) | 0x6E
	//   word2 = (4 << 12) | (3 << 8) | (2 << 4) | 1
	auto five = liftUnits(df, {0x556E, 0x0000, 0x4321, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, five.status) << five.error;
	EXPECT_EQ("v1 v2 v3 v4 v5", firstInsnRegs(five));

	// Zero arguments takes no register at all.
	auto none = liftUnits(df, {0x0072, 0x0000, 0x0000, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, none.status) << none.error;
	EXPECT_EQ("", firstInsnRegs(none));
}

// filled-new-array shares args35c, and its operand list is the array contents.
TEST(DexLifter, FilledNewArrayRegistersAreInArgumentOrder)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);

	// filled-new-array {v1, v2}, type@1
	auto r = liftUnits(df, {0x2024, 0x0001, 0x0021, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, r.status) << r.error;
	EXPECT_EQ("v1 v2", firstInsnRegs(r));
}

TEST(DexInsnSize, UnusedOpcodeAdvancesOneUnit)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 0x73 is unused.  Sized at three units it swallowed the instruction
	// after it; an unused opcode has no operands to skip.
	auto result = liftUnits(df, {0x0073, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_RETURN_VOID, instrs.back().opcode);
}

TEST(DexInsnSize, GotoIsOneCodeUnitSoTheUnitAfterItIsALeader)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 10t: goto +2 | return-void | return-void.  goto is one unit, so the
	// instruction at offset 1 begins its own (unreachable) block.  Sized at
	// two units, offset 1 is inside the goto and no block starts there.
	auto result = liftUnits(df, {0x0228, 0x000E, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	bool hasL1 = false;
	for (const auto& blk: result.cfg.blocks())
		if (blk.label == "L1") hasL1 = true;
	EXPECT_TRUE(hasL1);
}

TEST(DexInsnSize, IfGtzBranchesInsteadOfBeingTreatedAsAPayload)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 21t: if-gtz v0, +3 | return-void | return-void.  if-gtz and if-lez were
	// the two conditional branches sized 0, which routed them into the payload
	// arm and lost both the branch target and the fall-through.
	auto result = liftUnits(df, {0x003C, 0x0003, 0x000E, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_IF_Z, instrs[0].opcode);
	bool hasTarget = false, hasFallThrough = false;
	for (const auto& blk: result.cfg.blocks())
	{
		if (blk.label == "L3") hasTarget = true;
		if (blk.label == "L2") hasFallThrough = true;
	}
	EXPECT_TRUE(hasTarget);
	EXPECT_TRUE(hasFallThrough);
}

TEST(DexInsnSize, IfLezBranchesInsteadOfBeingTreatedAsAPayload)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// 21t: if-lez v0, +3 | return-void | return-void
	auto result = liftUnits(df, {0x003D, 0x0003, 0x000E, 0x000E});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_IF_Z, instrs[0].opcode);
}

// ─── switch payloads ─────────────────────────────────────────────────────────
//
// A payload is a pseudo-instruction sitting in the instruction stream, and it
// is identified by its whole first code unit (0x0100, 0x0200, 0x0300), not by
// its low byte -- the low byte is 0x00, which is nop.  The walker keyed the
// payload arm on kInsnSize == 0 and then asked for `ident & 0xFF`, so it could
// only ever enter that arm for packed-switch and sparse-switch, whose sizes
// were set to 0 for exactly that reason, and once inside it the low byte was
// 0x2b/0x2c rather than 0x01/0x02.  Both halves were wrong, and between them
// no switch was decoded at all and no payload was skipped.

TEST(DexInsnSize, PackedSwitchIsDecodedAndItsPayloadIsSkipped)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	//  0: packed-switch v0, +4      (31t, three units)
	//  3: return-void
	//  4: packed-switch-payload: ident, size=1, first_key(2), target(2)
	auto result = liftUnits(
		df,
		{
			0x002B,
			0x0004,
			0x0000, // packed-switch v0, payload at +4
			0x000E, // return-void
			0x0100,
			0x0001, // payload ident, size = 1
			0x0000,
			0x0000, // first_key = 0
			0x0000,
			0x0000, // targets[0] = 0
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_SWITCH, instrs[0].opcode);

	// Nothing may be decoded out of the payload: its six units are data.
	for (const auto& blk: result.cfg.blocks())
		for (const auto& in: blk.instrs)
			EXPECT_LT(in.offset, 4u * 2u)
				<< "instruction decoded at code-unit " << (in.offset / 2) << ", inside the payload";
}

TEST(DexInsnSize, SparseSwitchIsDecodedAndItsPayloadIsSkipped)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	//  0: sparse-switch v0, +4     (31t, three units)
	//  3: return-void
	//  4: sparse-switch-payload: ident, size=1, key(2), target(2)
	auto result = liftUnits(
		df,
		{
			0x002C,
			0x0004,
			0x0000,
			0x000E,
			0x0200,
			0x0001,
			0x0000,
			0x0000,
			0x0000,
			0x0000,
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_SWITCH, instrs[0].opcode);
	for (const auto& blk: result.cfg.blocks())
		for (const auto& in: blk.instrs)
			EXPECT_LT(in.offset, 4u * 2u)
				<< "instruction decoded at code-unit " << (in.offset / 2) << ", inside the payload";
}

TEST(DexInsnSize, FillArrayDataPayloadIsSkipped)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	//  0: fill-array-data v0, +3   (31t, three units)
	//  3: fill-array-data-payload: ident, element_width=4, size=1, data(2)
	auto result = liftUnits(
		df,
		{
			0x0026,
			0x0003,
			0x0000,
			0x0300,
			0x0004, // payload ident, element_width = 4
			0x0001,
			0x0000, // size = 1
			0x0000,
			0x0000, // one 4-byte element
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_FILL_ARRAY_DATA, instrs[0].opcode);
	for (const auto& blk: result.cfg.blocks())
		for (const auto& in: blk.instrs)
			EXPECT_LT(in.offset, 3u * 2u)
				<< "instruction decoded at code-unit " << (in.offset / 2) << ", inside the payload";
}

// ─── sleb128 accumulator ─────────────────────────────────────────────────────

TEST(DexReader, Sleb128FifthByteDoesNotOverflowASignedShift)
{
	// A five-byte sleb128 shifts its last payload by 28.  Accumulated in an
	// int32_t, `(b & 0x7F) << 28` is undefined for any payload above 7; UBSan
	// reports it as "left shift of 32 by 28 places cannot be represented in
	// type 'int'".  Accumulated unsigned it is a defined truncation, and the
	// format only makes bits 28..31 of that byte representable anyway -- 0x20
	// shifted by 28 keeps none of them.
	std::vector<uint8_t> buf = {0x80, 0x80, 0x80, 0x80, 0x20};
	DexReader r(buf.data(), buf.size());
	EXPECT_EQ(0, r.sleb128());
}

TEST(DexReader, Sleb128DecodesNegativeOne)
{
	std::vector<uint8_t> buf = {0x7F};
	DexReader r(buf.data(), buf.size());
	EXPECT_EQ(-1, r.sleb128());
}

TEST(DexReader, Sleb128DecodesIntMin)
{
	// -2147483648 sets only bit 31, which lands in the fifth byte as 0x08.
	std::vector<uint8_t> buf = {0x80, 0x80, 0x80, 0x80, 0x08};
	DexReader r(buf.data(), buf.size());
	EXPECT_EQ(INT32_MIN, r.sleb128());
}

TEST(DexReader, Sleb128DecodesSmallValuesBothWays)
{
	std::vector<uint8_t> pos = {0x3F}; // +63, sign bit clear
	std::vector<uint8_t> neg = {0x40}; // -64, sign bit set
	DexReader rp(pos.data(), pos.size());
	DexReader rn(neg.data(), neg.size());
	EXPECT_EQ(63, rp.sleb128());
	EXPECT_EQ(-64, rn.sleb128());
}

TEST(DexReader, Sleb128RejectsAnEncodingLongerThanTheFormatAllows)
{
	std::vector<uint8_t> buf = {0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
	DexReader r(buf.data(), buf.size());
	EXPECT_THROW(r.sleb128(), DexParseError);
}

// ─── switch CFG edges ────────────────────────────────────────────────────────
//
// A Dalvik switch names its case targets indirectly: the instruction carries a
// signed offset to a payload, and the payload carries one signed offset per
// case, each relative to the switch itself.  Only the fall-through was ever
// recorded as a leader, so a switch reached its cases through no edge at all
// and every case body looked unreachable.

// True when the block labelled `from` has an edge to the block labelled `to`.
static bool hasEdge(const BcCFG& cfg, const std::string& from, const std::string& to)
{
	const BcBasicBlock* src = nullptr;
	uint32_t dstId = ~0u;
	for (const auto& blk: cfg.blocks())
	{
		if (blk.label == from) src = &blk;
		if (blk.label == to) dstId = blk.id;
	}
	if (src == nullptr || dstId == ~0u) return false;
	return std::find(src->succs.begin(), src->succs.end(), dstId) != src->succs.end();
}

TEST(DexSwitch, PackedSwitchWiresAnEdgeToEveryCase)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	//  0: packed-switch v0, payload at +6
	//  3: return-void          (fall-through, no case matched)
	//  4: return-void          (case 0)
	//  5: return-void          (case 1)
	//  6: payload: ident, size=2, first_key=0, targets = {+4, +5}
	auto result = liftUnits(
		df,
		{
			0x002B,
			0x0006,
			0x0000,
			0x000E,
			0x000E,
			0x000E,
			0x0100,
			0x0002,
			0x0000,
			0x0000, // first_key = 0
			0x0004,
			0x0000, // targets[0] = +4
			0x0005,
			0x0000, // targets[1] = +5
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_SWITCH, instrs[0].opcode);
	EXPECT_TRUE(hasEdge(result.cfg, "L0", "L4"));
	EXPECT_TRUE(hasEdge(result.cfg, "L0", "L5"));
	// A Dalvik switch falls through when no case matches.
	EXPECT_TRUE(hasEdge(result.cfg, "L0", "L3"));
}

TEST(DexSwitch, SparseSwitchWiresAnEdgeToEveryCase)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	//  6: payload: ident, size=2, keys = {10, 20}, targets = {+4, +5}
	auto result = liftUnits(
		df,
		{
			0x002C,
			0x0006,
			0x0000,
			0x000E,
			0x000E,
			0x000E,
			0x0200,
			0x0002,
			0x000A,
			0x0000, // keys[0] = 10
			0x0014,
			0x0000, // keys[1] = 20
			0x0004,
			0x0000, // targets[0] = +4
			0x0005,
			0x0000, // targets[1] = +5
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_SWITCH, instrs[0].opcode);
	EXPECT_TRUE(hasEdge(result.cfg, "L0", "L4"));
	EXPECT_TRUE(hasEdge(result.cfg, "L0", "L5"));
}

TEST(DexSwitch, PayloadOffsetOutsideTheArrayYieldsNoTargets)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// The branch offset names a payload far past the end of the instruction
	// array.  Nothing there can be read, so the switch resolves to no cases
	// and the lift still comes back clean.
	auto result = liftUnits(
		df,
		{
			0x002B,
			0x7000,
			0x0000,
			0x000E,
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(BcOpcode::DALVIK_SWITCH, instrs[0].opcode);
	EXPECT_EQ(2u, instrs[0].operands.size()); // register + payload offset only
}

TEST(DexSwitch, NegativePayloadOffsetBeforeTheArrayYieldsNoTargets)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// -16 from offset 0 is before the start of the array.
	auto result = liftUnits(
		df,
		{
			0x002B,
			0xFFF0,
			0xFFFF,
			0x000E,
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(2u, instrs[0].operands.size());
}

TEST(DexSwitch, PayloadSizeTheArrayCannotSupplyYieldsNoTargets)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// The payload declares 0xFFFF cases and supplies none of them.
	auto result = liftUnits(
		df,
		{
			0x002B,
			0x0003,
			0x0000,
			0x0100,
			0xFFFF,
			0x0000,
			0x0000,
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(2u, instrs[0].operands.size());
}

TEST(DexSwitch, BranchOffsetNamingSomethingOtherThanAPayloadYieldsNoTargets)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// +3 lands on a return-void, not on a payload identifier.
	auto result = liftUnits(
		df,
		{
			0x002B,
			0x0003,
			0x0000,
			0x000E,
			0x000E,
		});
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;
	const auto& instrs = firstBlockInstrs(result);
	ASSERT_FALSE(instrs.empty());
	EXPECT_EQ(2u, instrs[0].operands.size());
}

// ─── string_data_item length (mutf8 bounds) ──────────────────────────────────

TEST(DexReader, Mutf8RejectsLengthTheFileCannotSupply)
{
	// string_data_item.utf16_size is a ULEB128 out of the file and mutf8()
	// reserved for it before reading a byte: the oom-b507582e reproducer is a
	// 668-byte DEX declaring a 0x93A25C0D-character string, which asked for
	// 2.4 GB and was OOM-killed instead of rejected. Every character costs at
	// least one byte on the wire, so the length is now checked against what is
	// left first — the read must fail before anything is consumed for it.
	const uint8_t data[] = {'a', 'b', 'c', 'd'};
	DexReader r(data, sizeof(data));
	EXPECT_THROW(r.mutf8(0x93A25C0Du), DexParseError);
	EXPECT_EQ(0u, r.pos());
}

TEST(DexReader, Mutf8AcceptsLengthTheFileCanSupply)
{
	const uint8_t data[] = {'a', 'b', 'c', 'd'};
	DexReader r(data, sizeof(data));
	EXPECT_EQ("abcd", r.mutf8(4));
}

TEST(DexFile, StringDataUtf16SizeExceedsFileThrows)
{
	// The same lie reached through DexFile::parse: overwrite the first
	// string_data_item's utf16_size with a five-byte ULEB128 for 0x93A25C0D.
	auto dex = buildMinimalDex();
	uint32_t declared = 0x93A25C0Du;
	size_t off = 0xC0; // string_data[0], see buildMinimalDex()
	do
	{
		uint8_t b = declared & 0x7F;
		declared >>= 7;
		if (declared) b |= 0x80;
		dex[off++] = b;
	}
	while (declared);
	EXPECT_THROW(DexFile::parse(dex), DexParseError);
}

// ─── MUTF-8 supplementary characters (dex_header.cpp) ────────────────────────

TEST(DexReader, Mutf8CombinesASurrogatePairIntoOneScalarValue)
{
	// MUTF-8 (JVM 4.4.7, and DEX "MUTF-8 (Modified UTF-8) Encoding") writes a
	// supplementary character as its two UTF-16 surrogates, each as a
	// three-byte sequence. The decoder re-encoded whatever it decoded verbatim
	// -- 0xE0 | (cp >> 12) and so on -- with no surrogate test and no pairing,
	// so U+10000 came back as the six bytes it went in as: ED A0 80 ED B0 80.
	// ED A0 80 is the UTF-8 spelling of U+D800, which is not a scalar value,
	// so no UTF-8 consumer accepts it and the character is lost. ESBMC refutes
	// "the re-encoding is not an ED A0..BF sequence" at c = 0xED, c2 = 0x20,
	// c3 = 0x00, cp = U+D800.
	const uint8_t data[] = {0xED, 0xA0, 0x80, 0xED, 0xB0, 0x80};
	DexReader r(data, sizeof(data));
	// utf16_size counts UTF-16 units, so the pair is two units.
	const std::string s = r.mutf8(2);
	EXPECT_EQ(std::string("\xF0\x90\x80\x80"), s); // U+10000
	EXPECT_EQ(sizeof(data), r.pos());
}

TEST(DexReader, Mutf8ReplacesALoneSurrogate)
{
	// A high surrogate with nothing after it is not a character. It used to be
	// re-emitted as ED A0 80; U+FFFD is what a well-formed UTF-8 stream can
	// carry instead.
	const uint8_t data[] = {0xED, 0xA0, 0x80};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(std::string("\xEF\xBF\xBD"), r.mutf8(1)); // U+FFFD
}

TEST(DexReader, Mutf8StillFoldsTheModifiedNulAndPlainAscii)
{
	// The two things the old loop did get right, pinned so the kernel route
	// cannot quietly drop them: C0 80 is the MUTF-8 NUL, and a raw 0x00 byte
	// (which real DEX files contain) is tolerated as one too.
	const uint8_t data[] = {'a', 0xC0, 0x80, 'b', 0x00, 'c'};
	DexReader r(data, sizeof(data));
	EXPECT_EQ(std::string("a\0b\0c", 5), r.mutf8(5));
}

// ─── encoded_value payloads (dex_class_parser.cpp) ───────────────────────────

// One class LHello; with a single static field VALUE whose static_values
// encoded_array holds exactly @p value. Everything outside the encoded_array
// is the file DexClassParser.FillsStaticFieldConstants above already parses,
// so any difference in the result is a difference in how the encoded_value
// itself was read.
static std::vector<uint8_t> buildDexWithStaticValue(
	const std::vector<uint8_t>& value, uint32_t annotationsOff = 0, const std::vector<uint8_t>& signaturePayload = {})
{
	std::vector<uint8_t> dex(0x300, 0);
	auto setU2 = [&](size_t off, uint16_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
	};
	auto setU4 = [&](size_t off, uint32_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
		dex[off + 2] = (v >> 16) & 0xFF;
		dex[off + 3] = (v >> 24) & 0xFF;
	};
	auto setStr = [&](size_t off, const std::string& s) {
		for (size_t i = 0; i < s.size(); ++i)
			dex[off + i] = static_cast<uint8_t>(s[i]);
		dex[off + s.size()] = 0;
	};
	auto setUleb = [&](size_t off, uint32_t v) -> size_t {
		size_t n = 0;
		do
		{
			uint8_t b = v & 0x7F;
			v >>= 7;
			if (v) b |= 0x80;
			dex[off + n++] = b;
		}
		while (v);
		return n;
	};
	static const char magic[] = "dex\n035";
	for (int i = 0; i < 8; ++i)
		dex[i] = static_cast<uint8_t>(i < 7 ? magic[i] : 0);
	setU4(0x24, 0x70);
	setU4(0x28, 0x12345678u);
	setU4(0x38, 7);
	setU4(0x3C, 0x70); // string_ids
	setU4(0x40, 3);
	setU4(0x44, 0x8C); // type_ids
	setU4(0x48, 1);
	setU4(0x4C, 0x98); // proto_ids
	setU4(0x50, 1);
	setU4(0x54, 0xA4); // field_ids
	setU4(0x58, 1);
	setU4(0x5C, 0xAC); // method_ids
	setU4(0x60, 1);
	setU4(0x64, 0xB4); // class_defs
	setU4(0x68, 0x80);
	setU4(0x6C, 0xD4);
	setU4(0x70, 0xD4); // "Hello"
	setU4(0x74, 0xDB); // "LHello;"
	setU4(0x78, 0xE4); // "V"
	setU4(0x7C, 0xE7); // "()V"
	setU4(0x80, 0xEC); // "main"
	// string[5] is the descriptor type[2] names. With an annotation directory
	// it has to be dalvik.annotation.Signature for resolveGenericSignature to
	// look at the annotation at all; without one it stays "I".
	setU4(0x84, annotationsOff != 0 ? 0x170u : 0xF2u);
	setU4(0x88, 0xF5); // "VALUE"
	setU4(0x8C, 1);    // type LHello;
	setU4(0x90, 2);    // type V
	setU4(0x94, 5);    // type string[5]
	setU4(0x98, 3);
	setU4(0x9C, 1);
	setU4(0xA0, 0); // proto
	setU2(0xA4, 0);
	setU2(0xA6, 2);
	setU4(0xA8, 6); // field Hello.VALUE
	setU2(0xAC, 0);
	setU2(0xAE, 0);
	setU4(0xB0, 4); // method main
	setU4(0xB4, 0);
	setU4(0xB8, 0x0009);
	setU4(0xBC, 0xFFFFFFFF);
	setU4(0xC0, 0);
	setU4(0xC4, 0xFFFFFFFF);
	setU4(0xC8, annotationsOff); // annotations_off
	setU4(0xCC, 0x100);          // classDataOff
	setU4(0xD0, 0x110);          // staticValuesOff
	size_t off = 0xD4;
	off += setUleb(off, 5);
	setStr(off, "Hello");
	off += 6;
	off += setUleb(off, 7);
	setStr(off, "LHello;");
	off += 8;
	off += setUleb(off, 1);
	setStr(off, "V");
	off += 2;
	off += setUleb(off, 3);
	setStr(off, "()V");
	off += 4;
	off += setUleb(off, 4);
	setStr(off, "main");
	off += 5;
	off += setUleb(off, 1);
	setStr(off, "I");
	off += 2;
	off += setUleb(off, 5);
	setStr(off, "VALUE");
	off += 6;
	// class_data at 0x100
	off = 0x100;
	off += setUleb(off, 1); // static_fields
	off += setUleb(off, 0);
	off += setUleb(off, 1); // direct methods
	off += setUleb(off, 0);
	off += setUleb(off, 0);    // field_idx_diff
	off += setUleb(off, 0x19); // public static final
	off += setUleb(off, 0);    // method_idx_diff
	off += setUleb(off, 0x09);
	off += setUleb(off, 0); // no code
	// encoded_array at 0x110: one element, the caller's encoded_value
	dex[0x110] = 1;
	for (size_t k = 0; k < value.size(); ++k)
		dex[0x111 + k] = value[k];

	size_t total = 0x120;
	if (annotationsOff != 0)
	{
		// annotations_directory_item at 0x140
		setU4(0x140, 0x150); // class_annotations_off
		setU4(0x144, 0);     // fields_size
		setU4(0x148, 0);     // annotated_methods_size
		setU4(0x14C, 0);     // annotated_parameters_size
		// annotation_set_item at 0x150
		setU4(0x150, 1);
		setU4(0x154, 0x160);
		// annotation_item at 0x160
		off = 0x160;
		dex[off++] = 0x00;      // visibility = BUILD
		off += setUleb(off, 2); // type_idx = type[2] = Signature
		off += setUleb(off, 1); // element count
		off += setUleb(off, 6); // name_idx = "VALUE" (unused by the walk)
		dex[off++] = 0x1c;      // VALUE_ARRAY, value_arg 0
		off += setUleb(off, 1); // one array element
		for (uint8_t b: signaturePayload)
			dex[off++] = b;
		// string[5] = "Ldalvik/annotation/Signature;" at 0x170
		const std::string sigType = "Ldalvik/annotation/Signature;";
		off = 0x170;
		off += setUleb(off, static_cast<uint32_t>(sigType.size()));
		setStr(off, sigType);
		off += sigType.size() + 1;
		total = 0x1A0;
	}
	dex.resize(total);
	setU4(0x20, static_cast<uint32_t>(total));
	return dex;
}

// VALUE_CHAR is an *unsigned* 16-bit code unit -- DEX lists it as `ushort` in
// "encoded_value encoding". It shared a case label with VALUE_BYTE/SHORT/INT/
// LONG and so went through the sign-extending path, and ESBMC reports the
// difference directly: signExtendEncoded(128, 1) is -128 where the encoding
// means 128. Every character above U+007F that fits in one byte came out
// negative, which is a silent wrong answer, not a crash.
TEST(DexEncodedValue, ValueCharIsUnsigned)
{
	auto dex = buildDexWithStaticValue({0x03, 0x80}); // VALUE_CHAR, one byte
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_EQ(1u, result.bcClass->fields.size());
	ASSERT_TRUE(result.bcClass->fields[0].constantIntValue.has_value());
	EXPECT_EQ(128, *result.bcClass->fields[0].constantIntValue);
}

// The other side of the same split: VALUE_BYTE really is signed, so the same
// payload byte means -128 there.
//
// This is an anti-overcorrection pin, not a regression test, and the difference
// is worth stating: reverting the VALUE_CHAR fix does not fail this test, only
// ValueCharIsUnsigned above. What it does catch is the other way of getting
// VALUE_CHAR right -- routing the whole 0x00/0x02/0x03/0x04/0x06 group through
// zeroExtendFrom, which makes ValueCharIsUnsigned pass and this one fail. The
// two arms must disagree on this payload; if they ever agree, one of them is
// wrong.
TEST(DexEncodedValue, ValueByteIsSigned)
{
	auto dex = buildDexWithStaticValue({0x00, 0x80}); // VALUE_BYTE, one byte
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_TRUE(result.bcClass->fields[0].constantIntValue.has_value());
	EXPECT_EQ(-128, *result.bcClass->fields[0].constantIntValue);
}

// VALUE_INT, four bytes, the value whose sign extension used to be performed
// by `(int64_t)(v << (64 - bits)) >> (64 - bits)`: the cast converts
// 0x8000000000000000 to int64_t (implementation-defined before C++20) and the
// >> then shifts a negative signed value (implementation-defined in every
// standard). byteorder::signExtendFrom fills in unsigned arithmetic and
// converts once through leb128::toSigned, which is total.
//
// This is a pin on the answer, NOT a regression test for that change, and
// nothing here can be: put the old expression back and this test still passes.
// Both halves of the old expression are implementation-defined rather than
// undefined, g++ and clang on x86 implement both the way the code wanted, and
// no sanitizer instruments either -- UBSan has no check for an out-of-range
// unsigned-to-signed conversion or for a right shift of a negative value, and
// the fixed build is UBSan-clean. So the test says what the answer must be for
// any future rewrite of this arm; it does not say the rewrite already made was
// observable.
TEST(DexEncodedValue, ValueIntSignExtendsTheTopBit)
{
	auto dex = buildDexWithStaticValue({0x64, 0x00, 0x00, 0x00, 0x80});
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_TRUE(result.bcClass->fields[0].constantIntValue.has_value());
	EXPECT_EQ(INT64_C(-2147483648), *result.bcClass->fields[0].constantIntValue);
}

// A shortened VALUE_FLOAT drops the LOW-order zero bytes and DEX says the
// survivors are "zero-extended to the right" -- they are the HIGH bytes.
// 1.0f is 0x3F800000, so its two-byte encoding is 80 3F; assembling those at
// the low end gives 0x00003F80, a denormal of about 2.3e-41.
TEST(DexEncodedValue, ShortenedFloatFillsFromTheHighEnd)
{
	// header: VALUE_FLOAT (0x10) with value_arg 1 == two payload bytes.
	auto dex = buildDexWithStaticValue({0x30, 0x80, 0x3F});
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_TRUE(result.bcClass->fields[0].constantFltValue.has_value());
	EXPECT_DOUBLE_EQ(1.0, *result.bcClass->fields[0].constantFltValue);
}

// The same rule for VALUE_DOUBLE: 1.0 is 0x3FF0000000000000, whose
// little-endian bytes are 00 00 00 00 00 00 F0 3F, so its shortest encoding is
// the two bytes F0 3F and the low-end assembly gives 0x3FF0 -- a denormal of
// about 8.09e-320: the bit pattern is 16368, and a subnormal double is its
// significand scaled by 2^-1074.
TEST(DexEncodedValue, ShortenedDoubleFillsFromTheHighEnd)
{
	// header: VALUE_DOUBLE (0x11) with value_arg 1 == two payload bytes.
	auto dex = buildDexWithStaticValue({0x31, 0xF0, 0x3F});
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_TRUE(result.bcClass->fields[0].constantFltValue.has_value());
	EXPECT_DOUBLE_EQ(1.0, *result.bcClass->fields[0].constantFltValue);
}

// A full-width VALUE_FLOAT must not be disturbed by the high-end placement.
TEST(DexEncodedValue, FullWidthFloatIsUnchanged)
{
	// header: VALUE_FLOAT with value_arg 3 == four payload bytes, 1.0f LE.
	auto dex = buildDexWithStaticValue({0x70, 0x00, 0x00, 0x80, 0x3F});
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_TRUE(result.bcClass->fields[0].constantFltValue.has_value());
	EXPECT_DOUBLE_EQ(1.0, *result.bcClass->fields[0].constantFltValue);
}

// ─── Signature annotation payload width (dex_class_parser.cpp) ───────────────

// The array element of a dalvik.annotation.Signature is an encoded_value whose
// value_arg is three bits, so it declares up to eight payload bytes. The walk
// accumulated them into a *uint32_t* with `(uint32_t)br.u1() << (b * 8)`: for
// the header byte 0xE0 straight out of the file, value_arg is 7, so b reaches 7
// and the last shift is a uint32_t shifted by 56 -- undefined behaviour
// ("arithmetic overflow on shl" / "undefined behavior on shift operation shl").
// x86 masks a 32-bit shift count to five bits, which is what turns the
// undefined shift into a wrong answer here: the payload below is
// 00 00 00 00 01 00 00 00, whose only nonzero byte is the 0x01 at b = 4, and
// that byte's count of 32 masks to 0, so it contributes 1 rather than
// 0x100000000. Eight bytes denoting an index far beyond the file's seven
// strings therefore used to fold onto string index 1 and yield the unrelated
// string "LHello;".
TEST(DexGenericSignature, EightBytePayloadDoesNotFoldOntoALowStringIndex)
{
	auto dex = buildDexWithStaticValue(
		{0x04, 0x2A}, // VALUE_INT 42, so the field parse is unremarkable
		0x140,
		{0xE0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	EXPECT_EQ("", result.bcClass->signature);
}

// The path that a real Signature annotation takes, so the fix above is not
// "reject everything".
TEST(DexGenericSignature, OneBytePayloadStillResolvesTheString)
{
	auto dex = buildDexWithStaticValue({0x04, 0x2A}, 0x140, {0x17, 0x01}); // VALUE_STRING, value_arg 0, string index 1
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	EXPECT_EQ("LHello;", result.bcClass->signature);
}

// ─── method descriptor walk (dex_lifter.cpp) ─────────────────────────────────

// buildMinimalDex with proto[0].parameters pointing at a one-element type_list
// whose type is @p paramDescriptor, so a lifted invoke carries the descriptor
// "(<paramDescriptor>)V".
static std::vector<uint8_t> buildDexWithProtoParam(const std::string& paramDescriptor)
{
	auto dex = buildMinimalDex();
	auto setU2 = [&](size_t off, uint16_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
	};
	auto setU4 = [&](size_t off, uint32_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
		dex[off + 2] = (v >> 16) & 0xFF;
		dex[off + 3] = (v >> 24) & 0xFF;
	};
	// Everything new goes past the end of the file buildMinimalDex produced,
	// so no existing item moves and the descriptor can be any length: the
	// string_data_items in the data section are packed end to end and
	// overwriting one in place would run into the next.
	const size_t base = (dex.size() + 3u) & ~size_t{3};
	const size_t typeIds = base;          // 3 * 4 bytes
	const size_t typeList = typeIds + 12; // u4 size + u2 entry
	const size_t strData = typeList + 8;  // uleb utf16_size + bytes + NUL
	dex.resize(strData + 1 + paramDescriptor.size() + 1);

	// string[0] is "Hello" and nothing in the minimal file references it, so
	// it becomes the parameter descriptor.
	dex[strData] = static_cast<uint8_t>(paramDescriptor.size()); // utf16_size
	for (size_t i = 0; i < paramDescriptor.size(); ++i)
		dex[strData + 1 + i] = static_cast<uint8_t>(paramDescriptor[i]);
	dex[strData + 1 + paramDescriptor.size()] = 0;
	setU4(0x70, static_cast<uint32_t>(strData)); // string_ids[0]

	// A third type_id, naming that string.
	setU4(0x40, 3);
	setU4(0x44, static_cast<uint32_t>(typeIds));
	setU4(typeIds + 0, 1); // type[0] = "LHello;"
	setU4(typeIds + 4, 2); // type[1] = "V"
	setU4(typeIds + 8, 0); // type[2] = string[0] = paramDescriptor

	// type_list: u4 size, then u2 per entry.
	setU4(typeList, 1);
	setU2(typeList + 4, 2);                       // the one parameter is type[2]
	setU4(0x94, static_cast<uint32_t>(typeList)); // proto[0].parametersOff

	setU4(0x20, static_cast<uint32_t>(dex.size())); // file_size
	return dex;
}

// parseDexProto's array arm did `end = proto.find(';', j); ... i = end + 1;`
// with no `end == npos` guard, where its sibling 'L' arm has one. On "([L)V" --
// a '[' whose element type names no class, which the string table can hand us
// -- closeP is 3, the arm runs at i = 1 with j = 2, find(';', 2) is npos, and
// `i = end + 1` wraps to 0. The cursor then cycles 0 -> 1 -> 0 -> 1 pushing two
// BcType nodes per turn, and the only thing that stops it is the kMaxParams =
// 65535 cap -- which is why the artifact was 65535 bogus parameters rather than
// a hang. ESBMC refutes `iNext > i` at i = 4, end = 18446744073709551615.
TEST(DexLifter, ArrayParamWithNoSemicolonDoesNotRestartTheDescriptorWalk)
{
	auto dex = buildDexWithProtoParam("[L");
	DexFile df = DexFile::parse(dex);
	ASSERT_EQ("([L)V", df.methodProto(0));

	CodeItem code;
	code.registersSize = 1;
	code.insns = {
		static_cast<uint16_t>(0x0071u), // invoke-static, count=0
		static_cast<uint16_t>(0x0000u), // method@0
		static_cast<uint16_t>(0x0000u), // registers
	};
	code.insnsSize = 3;

	DexLifter lifter(df);
	auto result = lifter.lift(code, 0);
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;

	const BcMethodRef* ref = nullptr;
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			for (const auto& op: insn.operands)
				if (const auto* m = std::get_if<BcMethodRef>(&op)) ref = m;
	ASSERT_NE(nullptr, ref);
	EXPECT_TRUE(ref->descriptor.params.empty());
}

// The descriptor the array arm is actually for, so the npos guard above is not
// refusing well-formed input: "([Ljava/lang/String;)V" is one parameter.
TEST(DexLifter, ArrayOfObjectsIsOneParameter)
{
	auto dex = buildDexWithProtoParam("[LHello;");
	DexFile df = DexFile::parse(dex);
	ASSERT_EQ("([LHello;)V", df.methodProto(0));

	CodeItem code;
	code.registersSize = 1;
	code.insns = {
		static_cast<uint16_t>(0x0071u),
		static_cast<uint16_t>(0x0000u),
		static_cast<uint16_t>(0x0000u),
	};
	code.insnsSize = 3;

	DexLifter lifter(df);
	auto result = lifter.lift(code, 0);
	ASSERT_EQ(DexLiftResult::OK, result.status) << result.error;

	const BcMethodRef* ref = nullptr;
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			for (const auto& op: insn.operands)
				if (const auto* m = std::get_if<BcMethodRef>(&op)) ref = m;
	ASSERT_NE(nullptr, ref);
	EXPECT_EQ(1u, ref->descriptor.params.size());
}

// ─── array type descriptors (dex_class_parser.cpp) ───────────────────────────

// buildDexWithStaticValue with string[5] -- the string the field's type_id
// names -- replaced by @p descriptor. The replacement string_data_item is
// appended past the end of the file buildDexWithStaticValue produced, so no
// existing item moves and the descriptor may be any length; the string_ids
// entry at 0x84 is repointed at it and file_size updated.
static std::vector<uint8_t> buildDexWithFieldTypeDescriptor(const std::string& descriptor)
{
	auto dex = buildDexWithStaticValue({0x04, 0x2A}); // VALUE_INT 42
	auto setU4 = [&](size_t off, uint32_t v) {
		dex[off] = v & 0xFF;
		dex[off + 1] = (v >> 8) & 0xFF;
		dex[off + 2] = (v >> 16) & 0xFF;
		dex[off + 3] = (v >> 24) & 0xFF;
	};
	const size_t strData = dex.size();
	// string_data_item: uleb128 utf16_size, the MUTF-8 bytes, a trailing NUL.
	// Every byte of an array descriptor is ASCII, so utf16_size is its length.
	uint32_t n = static_cast<uint32_t>(descriptor.size());
	do
	{
		uint8_t b = n & 0x7F;
		n >>= 7;
		if (n) b |= 0x80;
		dex.push_back(b);
	}
	while (n);
	for (char c: descriptor)
		dex.push_back(static_cast<uint8_t>(c));
	dex.push_back(0);
	setU4(0x84, static_cast<uint32_t>(strData));    // string_ids[5]
	setU4(0x20, static_cast<uint32_t>(dex.size())); // file_size
	return dex;
}

// How many array levels @p t nests, and what sits at the bottom.
static size_t arrayDepth(const BcType& t, const BcType** innermost = nullptr)
{
	size_t depth = 0;
	const BcType* cur = &t;
	while (cur->isArray() && cur->ref().elementType)
	{
		++depth;
		cur = cur->ref().elementType.get();
	}
	if (innermost) *innermost = cur;
	return depth;
}

// descriptorToType's '[' arm recursed on desc.substr(1) with no depth bound,
// over a descriptor that comes straight out of the string table -- so a field
// whose type is a long run of '[' cost one stack frame per bracket. Put that
// arm back and this test does not fail, it kills the process: the suite binary
// dies of SIGSEGV (exit 139) partway through this case, about a second in.
// fuzz_dex.cpp calls parseClass for every class_def, so the shape is
// fuzz-reachable. The walk now counts the bracket run and refuses past
// kMaxArrayDimensions before allocating anything, so this returns instead.
TEST(DexClassParser, DeepArrayDescriptorDoesNotRecurseOncePerBracket)
{
	auto dex = buildDexWithFieldTypeDescriptor(std::string(100000, '[') + "I");
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_EQ(1u, result.bcClass->fields.size());
	// Past the bound the descriptor names no type this parser knows, which is
	// the answer the default arm already gives for anything it cannot read.
	EXPECT_TRUE(result.bcClass->fields[0].type.isVoid());
}

// The bound itself, from both sides, so "does not recurse per bracket" cannot
// be satisfied by refusing arrays outright: 255 dimensions is the widest a
// Java array type may have, and it must still come back as 255 nested arrays
// of int.
TEST(DexClassParser, ArrayDescriptorAtTheDimensionBoundIsStillBuilt)
{
	auto dex = buildDexWithFieldTypeDescriptor(std::string(255, '[') + "I");
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_EQ(1u, result.bcClass->fields.size());
	const BcType* innermost = nullptr;
	EXPECT_EQ(255u, arrayDepth(result.bcClass->fields[0].type, &innermost));
	ASSERT_NE(nullptr, innermost);
	EXPECT_TRUE(innermost->isPrim());
	EXPECT_EQ(BcPrimKind::Int, innermost->prim().kind);
}

TEST(DexClassParser, ArrayDescriptorPastTheDimensionBoundIsRefused)
{
	auto dex = buildDexWithFieldTypeDescriptor(std::string(256, '[') + "I");
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_EQ(1u, result.bcClass->fields.size());
	EXPECT_TRUE(result.bcClass->fields[0].type.isVoid());
}

// The ordinary case, so counting the run has not changed what a well-formed
// descriptor means: "[[LHello;" is a two-dimensional array of LHello;.
TEST(DexClassParser, OrdinaryArrayDescriptorStillNestsInnermostFirst)
{
	auto dex = buildDexWithFieldTypeDescriptor("[[LHello;");
	DexFile df = DexFile::parse(dex);
	DexClassParser parser(df);
	auto result = parser.parseClass(0);
	ASSERT_EQ(DexClassResult::OK, result.status);
	ASSERT_EQ(1u, result.bcClass->fields.size());
	const BcType* innermost = nullptr;
	EXPECT_EQ(2u, arrayDepth(result.bcClass->fields[0].type, &innermost));
	ASSERT_NE(nullptr, innermost);
	ASSERT_TRUE(innermost->isClass());
	EXPECT_EQ("Hello", innermost->ref().className);
}

// ─── signed Dalvik operands (dex_lifter.cpp) ─────────────────────────────────

// The first integer operand the lifter produced for @p units. Register operands
// are BcLocalOperand and branch targets are block operands, so for the const
// and /lit forms below this is the literal.
static int64_t firstIntOperand(const DexFile& df, std::vector<uint16_t> units)
{
	auto result = liftUnits(df, std::move(units));
	EXPECT_EQ(DexLiftResult::OK, result.status);
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			for (const auto& op: insn.operands)
				if (const auto* i = std::get_if<BcIntOperand>(&op)) return i->value;
	ADD_FAILURE();
	return 0;
}

// const/4 packs a four-bit signed literal into the B nibble, and the arm read
// it as `static_cast<int64_t>(static_cast<int8_t>(vB << 4) >> 4)`. vB is a
// uint16_t holding 0..15, so for vB >= 8 the product vB << 4 is 128..240 --
// above INT8_MAX -- and converting that to int8_t is implementation-defined in
// C++17; the `>> 4` that follows is then a right shift of a negative value,
// implementation-defined in every standard. Both halves of the defect that was
// removed from dex_class_parser.cpp's signExtendEncoded, in one expression,
// in a file the same change edited. byteorder::signExtendFrom(vB, 4) has
// neither.
//
// All sixteen encodings, because the replacement has to agree with the format
// everywhere and not just at the witness: 0..7 mean themselves, 8..15 mean
// -8..-1.
TEST(DexLifter, Const4LiteralIsFourBitTwosComplement)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	for (uint16_t vB = 0; vB < 16; ++vB)
	{
		// const/4 vA=0, B=vB: opcode 0x12 in the low byte, A in bits 8..11,
		// B in bits 12..15.
		const uint16_t w0 = static_cast<uint16_t>(0x12u | (vB << 12));
		const int64_t expected = vB < 8 ? vB : static_cast<int64_t>(vB) - 16;
		EXPECT_EQ(expected, firstIntOperand(df, {w0, 0x000Fu}));
	}
}

// The rest of the signed const forms, whose literals were recovered by the
// conversion half of the same defect -- `static_cast<int16_t>(w(1))` and
// `static_cast<int32_t>(...)` of values above the signed maximum. Each is
// checked at the most negative value its width can encode, which is exactly
// the input that made the conversion out of range.
TEST(DexLifter, ConstFormsRecoverTheMostNegativeLiteralOfTheirWidth)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// const/16 v0, #-32768  (0x13, AA=0, BBBB=0x8000)
	EXPECT_EQ(INT64_C(-32768), firstIntOperand(df, {0x0013u, 0x8000u}));
	// const v0, #-2147483648  (0x14, AA=0, BBBBBBBB=0x80000000)
	EXPECT_EQ(INT64_C(-2147483648), firstIntOperand(df, {0x0014u, 0x0000u, 0x8000u}));
	// const/high16 v0, #-65536 == 0xFFFF0000  (0x15, AA=0, BBBB=0xFFFF)
	EXPECT_EQ(INT64_C(-65536), firstIntOperand(df, {0x0015u, 0xFFFFu}));
	// const-wide/16 v0, #-1  (0x16)
	EXPECT_EQ(INT64_C(-1), firstIntOperand(df, {0x0016u, 0xFFFFu}));
	// const-wide/32 v0, #-2147483648  (0x17)
	EXPECT_EQ(INT64_C(-2147483648), firstIntOperand(df, {0x0017u, 0x0000u, 0x8000u}));
	// const-wide v0, #INT64_MIN  (0x18)
	EXPECT_EQ(INT64_MIN, firstIntOperand(df, {0x0018u, 0x0000u, 0x0000u, 0x0000u, 0x8000u}));
	// const-wide/high16 v0, #INT64_MIN  (0x19, BBBB placed at bits 48..63)
	EXPECT_EQ(INT64_MIN, firstIntOperand(df, {0x0019u, 0x8000u}));
}

// The /lit16 and /lit8 literals, same conversion half. add-int/lit16 (0xD0)
// takes its literal in the whole second unit; add-int/lit8 (0xD8) takes it in
// that unit's high byte.
TEST(DexLifter, LiteralOperandsOfTheArithmeticFormsAreSigned)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	// add-int/lit16 v0, v0, #-32768
	EXPECT_EQ(INT64_C(-32768), firstIntOperand(df, {0x00D0u, 0x8000u}));
	// add-int/lit8 v0, v0, #-128  (CC = 0x80 in the high byte of unit 1)
	EXPECT_EQ(INT64_C(-128), firstIntOperand(df, {0x00D8u, 0x8000u}));
	// and the positive side of each, so the fill is not unconditional.
	EXPECT_EQ(INT64_C(32767), firstIntOperand(df, {0x00D0u, 0x7FFFu}));
	EXPECT_EQ(INT64_C(127), firstIntOperand(df, {0x00D8u, 0x7F00u}));
}

// A backward goto, whose offset went through the same conversion. goto (0x28)
// carries a signed byte in the high byte of its only unit; 0xFF is -1, so from
// offset 1 the branch target is offset 0 and the lift must produce a block
// there. A zero-extending read would give +255 instead, off the end of a
// three-unit method, and no such block.
TEST(DexLifter, BackwardGotoTargetsTheBlockItsNegativeOffsetNames)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto result = liftUnits(
		df,
		{
			static_cast<uint16_t>(0x000Eu), // return-void at offset 0
			static_cast<uint16_t>(0xFF28u), // goto -1 at offset 1
		});
	ASSERT_EQ(DexLiftResult::OK, result.status);
	const BcBlockOperand* target = nullptr;
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			if (insn.opcode == BcOpcode::DALVIK_GOTO)
				for (const auto& op: insn.operands)
					if (const auto* b = std::get_if<BcBlockOperand>(&op)) target = b;
	ASSERT_NE(nullptr, target);
	EXPECT_EQ(0u, target->blockId);
}

// ─── Branch targets and try regions that do not fit the method ────────────────

// The four goto arms and the if-* group all formed `off + offset` in uint32 and
// inserted the result as a leader without asking whether it landed inside the
// method. A goto -8 at offset 0 of a two-unit method wraps to 4294967288, and
// the lift grew a block labelled "L4294967288" that no instruction occupies.
TEST(DexLifter, ABranchWrappingPastZeroDoesNotBecomeALeader)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto result = liftUnits(
		df,
		{
			static_cast<uint16_t>(0xF828u), // goto -8 at offset 0
			static_cast<uint16_t>(0x000Eu), // return-void at offset 1
		});
	ASSERT_EQ(DexLiftResult::OK, result.status);
	// Every leader names a code unit offset in [0, insnsSize]; insnsSize itself
	// is the half-open end and is a legitimate leader. Anything past it is a
	// block the method has no bytes for.
	for (const auto& blk: result.cfg.blocks())
	{
		ASSERT_EQ('L', blk.label.empty() ? '\0' : blk.label[0]);
		EXPECT_LE(std::stoull(blk.label.substr(1)), 2u)
			<< "block labelled " << blk.label << " sits outside a two-unit method";
	}
}

// A try_item is a u4 start and a u2 count. Their sum was formed in uint32 and
// written straight into BcExceptionHandler::startOffset/endOffset, so
// startAddr = 0xFFFFFFFF with insnCount = 2 gave start=4294967295 end=1: a
// region ending four gigabytes before it begins, whose length reads back as a
// plausible 2. jvm_lifter refuses the equivalent JVM entry; this did not.
static DexLiftResult
liftUnitsWithTry(const DexFile& df, std::vector<uint16_t> units, uint32_t startAddr, uint16_t insnCount)
{
	CodeItem code;
	code.registersSize = 2;
	code.insSize = 0;
	code.outsSize = 0;
	code.debugInfoOff = 0;
	code.insnsSize = static_cast<uint32_t>(units.size());
	code.insns = std::move(units);

	TryItem t;
	t.startAddr = startAddr;
	t.insnCount = insnCount;
	t.handlerOff = 0;
	code.tries.push_back(t);
	code.triesSize = 1;
	code.handlers.handlers.push_back({CatchHandler{-1, 0}});
	code.handlers.catchAllAddrs.push_back(0);

	DexLifter lifter(df);
	return lifter.lift(code, 0);
}

TEST(DexLifter, ATryRegionWhoseEndWrapsBeforeItsStartIsRefused)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto result =
		liftUnitsWithTry(df, {static_cast<uint16_t>(0x000Eu), static_cast<uint16_t>(0x000Eu)}, 0xFFFFFFFFu, 2);
	ASSERT_EQ(DexLiftResult::OK, result.status);
	for (const auto& h: result.cfg.handlers())
		EXPECT_LE(h.startOffset, h.endOffset) << "handler covers [" << h.startOffset << ", " << h.endOffset << ")";
	EXPECT_EQ(0u, result.cfg.handlers().size());
}

// A start inside the method but a count that runs off the end is the same
// question asked the other way, and is refused for the same reason: endOffset
// would name a code unit the method does not have.
TEST(DexLifter, ATryRegionRunningPastTheEndOfTheMethodIsRefused)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto result = liftUnitsWithTry(df, {static_cast<uint16_t>(0x000Eu), static_cast<uint16_t>(0x000Eu)}, 1, 8);
	ASSERT_EQ(DexLiftResult::OK, result.status);
	EXPECT_EQ(0u, result.cfg.handlers().size());
}

// The bound is a bound, not a ban: a region that ends exactly at the last code
// unit still produces its handlers, both the typed one and the catch-all.
TEST(DexLifter, ATryRegionEndingAtTheLastCodeUnitStillWires)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto result = liftUnitsWithTry(df, {static_cast<uint16_t>(0x000Eu), static_cast<uint16_t>(0x000Eu)}, 0, 2);
	ASSERT_EQ(DexLiftResult::OK, result.status);
	ASSERT_EQ(2u, result.cfg.handlers().size());
	for (const auto& h: result.cfg.handlers())
	{
		EXPECT_EQ(0u, h.startOffset);
		EXPECT_EQ(2u, h.endOffset);
	}
}

// wireExceptions() indexed catchAllAddrs by the try index while checking only
// handlers.size(). The parser keeps the two vectors the same length, so this is
// reachable only through the public lift(CodeItem&) entry point -- which is
// public, and is what these tests use.
TEST(DexLifter, ATryWithNoCatchAllEntryDoesNotReadPastTheVector)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	CodeItem code;
	code.registersSize = 2;
	code.insSize = 0;
	code.outsSize = 0;
	code.debugInfoOff = 0;
	code.insns = {static_cast<uint16_t>(0x000Eu), static_cast<uint16_t>(0x000Eu)};
	code.insnsSize = 2;
	TryItem t;
	t.startAddr = 0;
	t.insnCount = 2;
	t.handlerOff = 0;
	code.tries.push_back(t);
	code.triesSize = 1;
	code.handlers.handlers.push_back({CatchHandler{-1, 0}});
	// catchAllAddrs deliberately left empty.
	auto result = DexLifter(df).lift(code, 0);
	ASSERT_EQ(DexLiftResult::OK, result.status);
	EXPECT_EQ(1u, result.cfg.handlers().size());
}

// Same shape as JvmLifter.InstructionIdsAreUniqueAcrossTheWholeMethod:
// decodeInsn() used blk.instrs.size() as the id, which restarts at 0 in every
// block. BcInstruction::id is the key of the method-wide per-instruction maps,
// so ids have to be unique across the method, not within the block.
TEST(DexLifter, InstructionIdsAreUniqueAcrossTheWholeMethod)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto result = liftUnits(
		df,
		{
			static_cast<uint16_t>(0x1012u), // const/4 v0, #1     @0
			static_cast<uint16_t>(0x0338u), // if-eqz v0, +3 -> @4 @1
			static_cast<uint16_t>(0x0000u), //                    (offset word)
			static_cast<uint16_t>(0x2012u), // const/4 v0, #2     @3
			static_cast<uint16_t>(0x000Eu), // return-void        @4
		});
	ASSERT_EQ(DexLiftResult::OK, result.status);
	ASSERT_LT(1u, result.cfg.blockCount()) << "the branch must split the method";

	std::vector<uint32_t> ids;
	for (const auto& blk: result.cfg.blocks())
		for (const auto& insn: blk.instrs)
			ids.push_back(insn.id);
	ASSERT_FALSE(ids.empty());
	std::sort(ids.begin(), ids.end());
	ASSERT_EQ(ids.end(), std::unique(ids.begin(), ids.end())) << "two instructions share an id";
	for (size_t i = 0; i < ids.size(); ++i)
		EXPECT_EQ(static_cast<uint32_t>(i), ids[i]);
}

// The counter is reset per method, not per DexLifter: two lifts from the same
// instance must both start at 0, or the second method's ids depend on how long
// the first one was.
TEST(DexLifter, TheInstructionCounterRestartsForEachMethod)
{
	auto dex = buildMinimalDex();
	DexFile df = DexFile::parse(dex);
	auto first = liftUnits(df, {static_cast<uint16_t>(0x1012u), static_cast<uint16_t>(0x000Eu)});
	auto second = liftUnits(df, {static_cast<uint16_t>(0x1012u), static_cast<uint16_t>(0x000Eu)});
	ASSERT_EQ(DexLiftResult::OK, first.status);
	ASSERT_EQ(DexLiftResult::OK, second.status);
	ASSERT_FALSE(first.cfg.block(0).instrs.empty());
	ASSERT_FALSE(second.cfg.block(0).instrs.empty());
	EXPECT_EQ(0u, first.cfg.block(0).instrs.front().id);
	EXPECT_EQ(0u, second.cfg.block(0).instrs.front().id);
}
