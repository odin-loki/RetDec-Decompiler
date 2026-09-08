/**
 * @file tests/pyc_parser/pyc_parser_test.cpp
 * @brief Unit tests for the Python .pyc parser.
 *
 * Tests cover:
 *   - Magic number detection for all supported versions (3.8–3.13)
 *   - PycReadOptions and header-only parsing
 *   - MarshalReader: all primitive types, FLAG_REF, TYPE_REF, nested tuples
 *   - PyCodeObject field decoding (argcount, varnames, flags, etc.)
 *   - Line number table decoding (lnotab ≤ 3.9, linetable 3.10, 3.11+)
 *   - Exception table decoding (3.11+)
 *   - BcCFG construction: leaders, blocks, successor edges
 *   - PycReader full-parse integration
 *   - BcOpcode::PYTHON_* enum values are distinct
 */

#include <memory>
#include "retdec/pyc_parser/pyc_magic.h"
#include "retdec/pyc_parser/py_code_object.h"
#include "retdec/pyc_parser/py_marshal.h"
#include "retdec/pyc_parser/py_opcodes.h"
#include "retdec/pyc_parser/pyc_reader.h"

#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace retdec::pyc_parser;
using namespace retdec::bc_module;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static uint32_t makeRawMagic(uint16_t versionMagic)
{
	return static_cast<uint32_t>(versionMagic) | (static_cast<uint32_t>(kPycCRLFMarker) << 16);
}

static std::vector<uint8_t> makeLE32(uint32_t v)
{
	return {
		static_cast<uint8_t>(v & 0xFF),
		static_cast<uint8_t>((v >> 8) & 0xFF),
		static_cast<uint8_t>((v >> 16) & 0xFF),
		static_cast<uint8_t>((v >> 24) & 0xFF),
	};
}

static std::vector<uint8_t> makeLE16(uint16_t v)
{
	return {
		static_cast<uint8_t>(v & 0xFF),
		static_cast<uint8_t>((v >> 8) & 0xFF),
	};
}

// Write a simple marshal object sequence into a buffer
struct MarshalBuilder
{
	std::vector<uint8_t> buf;

	void byte(uint8_t b)
	{
		buf.push_back(b);
	}

	void none()
	{
		byte('N');
	}
	void true_()
	{
		byte('T');
	}
	void false_()
	{
		byte('F');
	}
	void ellip()
	{
		byte('.');
	}

	void int32(int32_t v)
	{
		byte('i');
		for (int i = 0; i < 4; ++i)
			buf.push_back((v >> (8 * i)) & 0xFF);
	}

	void float64(double v)
	{
		byte('g');
		uint64_t bits;
		memcpy(&bits, &v, 8);
		for (int i = 0; i < 8; ++i)
			buf.push_back((bits >> (8 * i)) & 0xFF);
	}

	void shortAscii(const std::string& s)
	{
		byte('z');
		byte(static_cast<uint8_t>(s.size()));
		for (char c: s)
			buf.push_back(static_cast<uint8_t>(c));
	}

	void bytes_(const std::string& s)
	{
		byte('B');
		uint32_t len = static_cast<uint32_t>(s.size());
		for (int i = 0; i < 4; ++i)
			buf.push_back((len >> (8 * i)) & 0xFF);
		for (char c: s)
			buf.push_back(static_cast<uint8_t>(c));
	}

	void smallTuple(uint8_t n)
	{
		byte(')');
		buf.push_back(n);
	}

	void tuple(uint32_t n)
	{
		byte('(');
		for (int i = 0; i < 4; ++i)
			buf.push_back((n >> (8 * i)) & 0xFF);
	}

	// TYPE_LONG: int32 digit count (negative = negative value), then that many
	// little-endian uint16 digits in base 2^15.
	void longObj(int32_t n, const std::vector<uint16_t>& digits)
	{
		byte('l');
		for (int i = 0; i < 4; ++i)
			buf.push_back((static_cast<uint32_t>(n) >> (8 * i)) & 0xFF);
		for (uint16_t d: digits)
		{
			buf.push_back(d & 0xFF);
			buf.push_back((d >> 8) & 0xFF);
		}
	}

	void ref(int32_t idx)
	{
		byte('r');
		for (int i = 0; i < 4; ++i)
			buf.push_back((idx >> (8 * i)) & 0xFF);
	}

	// TYPE_INT with FLAG_REF set (0x80 | 'i' = 0x80 | 0x69 = 0xE9)
	void int32_ref(int32_t v)
	{
		byte(0x80 | 'i');
		for (int i = 0; i < 4; ++i)
			buf.push_back((v >> (8 * i)) & 0xFF);
	}
};

// ─── Magic detection tests ────────────────────────────────────────────────────

TEST(PycMagic, DetectPython38)
{
	uint32_t raw = makeRawMagic(3413);
	auto ver = detectVersion(raw);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(8, ver->minor);
}

TEST(PycMagic, DetectPython39)
{
	uint32_t raw = makeRawMagic(3425);
	auto ver = detectVersion(raw);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(9, ver->minor);
}

TEST(PycMagic, DetectPython310)
{
	uint32_t raw = makeRawMagic(3439);
	auto ver = detectVersion(raw);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(10, ver->minor);
}

TEST(PycMagic, DetectPython311)
{
	uint32_t raw = makeRawMagic(3495);
	auto ver = detectVersion(raw);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(11, ver->minor);
}

TEST(PycMagic, DetectPython312)
{
	uint32_t raw = makeRawMagic(3531);
	auto ver = detectVersion(raw);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(12, ver->minor);
}

TEST(PycMagic, DetectPython313)
{
	uint32_t raw = makeRawMagic(3561);
	auto ver = detectVersion(raw);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(13, ver->minor);
}

TEST(PycMagic, UnknownMagicReturnsNullopt)
{
	auto ver = detectVersion(0xDEADBEEF);
	EXPECT_FALSE(ver.has_value());
}

TEST(PycMagic, DetectFromBuffer)
{
	// The old fixture used 0x0A0D0D95, which is not in the table, and then
	// discarded the answer -- so the buffer overload could have returned
	// nullopt for every magic and the test stayed green. Taken from the table
	// instead, so the two overloads can be required to agree.
	const uint32_t magic = magicForVersion(3, 10);
	ASSERT_NE(0u, magic);
	const uint8_t buf[4] = {
		static_cast<uint8_t>(magic & 0xFF),
		static_cast<uint8_t>((magic >> 8) & 0xFF),
		static_cast<uint8_t>((magic >> 16) & 0xFF),
		static_cast<uint8_t>((magic >> 24) & 0xFF)};

	auto fromBuf = detectVersion(buf, 4);
	auto fromInt = detectVersion(magic);
	ASSERT_TRUE(fromBuf.has_value());
	ASSERT_TRUE(fromInt.has_value());
	EXPECT_EQ(fromBuf->major, fromInt->major);
	EXPECT_EQ(fromBuf->minor, fromInt->minor);
	EXPECT_EQ(3, fromBuf->major);
	EXPECT_EQ(10, fromBuf->minor);
}

TEST(PycMagic, DetectFromBufferRejectsUnknownMagic)
{
	// Little-endian 0x0A0D0D95, which the table does not carry.
	const uint8_t buf[4] = {0x95, 0x0D, 0x0D, 0x0A};
	EXPECT_FALSE(detectVersion(buf, 4).has_value());
}

TEST(PycMagic, DetectFromBufferTooShort)
{
	uint8_t buf[3] = {1, 2, 3};
	auto ver = detectVersion(buf, 3);
	EXPECT_FALSE(ver.has_value());
}

TEST(PycMagic, MagicForVersion38)
{
	uint32_t m = magicForVersion(3, 8);
	EXPECT_NE(0u, m);
	auto ver = detectVersion(m);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(8, ver->minor);
}

TEST(PycMagic, MagicForVersion312)
{
	uint32_t m = magicForVersion(3, 12);
	EXPECT_NE(0u, m);
	auto ver = detectVersion(m);
	ASSERT_TRUE(ver.has_value());
	EXPECT_EQ(3, ver->major);
	EXPECT_EQ(12, ver->minor);
}

TEST(PycMagic, MagicForUnknownVersionReturnsZero)
{
	EXPECT_EQ(0u, magicForVersion(2, 7));
}

TEST(PycMagic, PythonVersionAtLeast)
{
	PythonVersion v{3, 11, 3495, "3.11"};
	EXPECT_TRUE(v.atLeast(3, 11));
	EXPECT_TRUE(v.atLeast(3, 10));
	EXPECT_TRUE(v.atLeast(3, 8));
	EXPECT_FALSE(v.atLeast(3, 12));
	EXPECT_FALSE(v.atLeast(4, 0));
}

TEST(PycMagic, AllVersionsInTable)
{
	size_t count;
	const MagicEntry* table = magicTable(count);
	EXPECT_GT(count, 0u);
	// Check all versions are ≥ 3.8
	for (size_t i = 0; i < count; ++i)
	{
		EXPECT_GE(table[i].ver.major, 3);
		EXPECT_GE(table[i].ver.minor, 8);
	}
}

TEST(PycMagic, HeaderConstants)
{
	EXPECT_EQ(16u, kPycHeaderSize);
	EXPECT_EQ(0x0D0Au, kPycCRLFMarker);
}

// ─── MarshalReader tests ─────────────────────────────────────────────────────

TEST(MarshalReader, ReadNone)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.none();
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_EQ(MarshalObject::Type::None, obj->type);
	EXPECT_TRUE(obj->isNone());
}

TEST(MarshalReader, ReadTrue)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.true_();
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isTrue());
}

TEST(MarshalReader, ReadFalse)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.false_();
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isFalse());
}

TEST(MarshalReader, ReadEllipsis)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.ellip();
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_EQ(MarshalObject::Type::Ellipsis, obj->type);
}

TEST(MarshalReader, ReadInt32)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.int32(42);
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isInt());
	EXPECT_EQ(42LL, obj->asInt());
}

TEST(MarshalReader, ReadNegativeInt32)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.int32(-100);
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_EQ(-100LL, obj->asInt());
}

TEST(MarshalReader, ReadFloat64)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.float64(3.14);
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isFloat());
	EXPECT_NEAR(3.14, obj->asFloat(), 1e-10);
}

TEST(MarshalReader, ReadShortAsciiString)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.shortAscii("hello");
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isStr());
	EXPECT_EQ("hello", obj->asStr());
}

TEST(MarshalReader, ReadBytesObject)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.bytes_("\x01\x02\x03");
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isBytes());
	EXPECT_EQ(3u, obj->asStr().size());
}

TEST(MarshalReader, ReadSmallTupleWithInts)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.smallTuple(3);
	mb.int32(1);
	mb.int32(2);
	mb.int32(3);
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isTuple());
	const auto& elems = obj->asTuple();
	ASSERT_EQ(3u, elems.size());
	EXPECT_EQ(1LL, elems[0]->asInt());
	EXPECT_EQ(2LL, elems[1]->asInt());
	EXPECT_EQ(3LL, elems[2]->asInt());
}

TEST(MarshalReader, ReadTupleEmpty)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.tuple(0);
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isTuple());
	EXPECT_TRUE(obj->asTuple().empty());
}

TEST(MarshalReader, ReadNestedTuple)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.tuple(2);
	mb.smallTuple(2);
	mb.int32(10);
	mb.int32(20);
	mb.none();
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_TRUE(obj->isTuple());
	const auto& outer = obj->asTuple();
	ASSERT_EQ(2u, outer.size());
	EXPECT_TRUE(outer[0]->isTuple());
	EXPECT_TRUE(outer[1]->isNone());
}

TEST(MarshalReader, FlagRefDeduplication)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	// Write int32(99) with FLAG_REF set, then ref to index 0
	mb.int32_ref(99);
	mb.ref(0);
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);

	auto obj1 = mr.readObject();
	ASSERT_NE(nullptr, obj1);
	EXPECT_EQ(99LL, obj1->asInt());

	auto obj2 = mr.readObject();
	ASSERT_NE(nullptr, obj2);
	EXPECT_EQ(99LL, obj2->asInt());
}

TEST(MarshalReader, ReadMultipleObjects)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.int32(1);
	mb.int32(2);
	mb.none();
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);

	auto o1 = mr.readObject();
	EXPECT_EQ(1LL, o1->asInt());
	auto o2 = mr.readObject();
	EXPECT_EQ(2LL, o2->asInt());
	auto o3 = mr.readObject();
	EXPECT_TRUE(o3->isNone());
	EXPECT_TRUE(mr.eof());
}

TEST(MarshalReader, UnknownTypeByteSetsError)
{
	PythonVersion ver{3, 10, 0, ""};
	uint8_t data = 0x7F; // Unknown type
	MarshalReader mr(&data, 1, ver);
	auto obj = mr.readObject();
	EXPECT_EQ(nullptr, obj);
	EXPECT_TRUE(mr.hasError());
}

// ─── TYPE_LONG tests ──────────────────────────────────────────────────────────

TEST(MarshalReader, ReadLongThreeDigits)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.longObj(3, {1, 2, 3}); // 1 + 2*2^15 + 3*2^30
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_EQ(MarshalObject::Type::Long, obj->type);
	EXPECT_EQ(3221291009LL, obj->asInt());
	EXPECT_FALSE(mr.hasError());
}

TEST(MarshalReader, ReadLongNegativeDigitCountIsNegativeValue)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.longObj(-3, {1, 2, 3});
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_EQ(-3221291009LL, obj->asInt());
}

// A long wider than int64_t must truncate, not shift past the width of the
// accumulator. Six digits put the last shift at 75.
TEST(MarshalReader, ReadLongWiderThanInt64)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.longObj(6, {0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF});
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_EQ(MarshalObject::Type::Long, obj->type);
	EXPECT_FALSE(mr.hasError());
}

// The fifth digit sits at bit 60, so a large one overflows the signed
// accumulator even though the shift itself is still in range.
TEST(MarshalReader, ReadLongFifthDigitOverflowsSignedRange)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.longObj(5, {0, 0, 0, 0, 17775});
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	ASSERT_NE(nullptr, obj);
	EXPECT_EQ(MarshalObject::Type::Long, obj->type);
	EXPECT_FALSE(mr.hasError());
}

TEST(MarshalReader, ReadLongDigitCountExceedsRemainingInput)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.longObj(0x7FFFFFFF, {1, 2}); // claims 2^31-1 digits, supplies two
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	EXPECT_EQ(nullptr, obj);
	EXPECT_TRUE(mr.hasError());
}

// INT32_MIN cannot be negated as an int32_t; the magnitude has to be taken in
// unsigned form before it is bounded against the input.
TEST(MarshalReader, ReadLongMostNegativeDigitCount)
{
	PythonVersion ver{3, 10, 0, ""};
	MarshalBuilder mb;
	mb.longObj(-2147483647 - 1, {1, 2});
	MarshalReader mr(mb.buf.data(), mb.buf.size(), ver);
	auto obj = mr.readObject();
	EXPECT_EQ(nullptr, obj);
	EXPECT_TRUE(mr.hasError());
}

// ─── PyCodeObject helpers tests ───────────────────────────────────────────────

TEST(PyCodeObject, CoFlags)
{
	PyCodeObject code;
	code.co_flags = CO_GENERATOR | CO_COROUTINE;
	EXPECT_TRUE(code.isGenerator());
	EXPECT_TRUE(code.isCoroutine());
	EXPECT_FALSE(code.isAsyncGenerator());
	EXPECT_FALSE(code.hasVarArgs());
}

TEST(PyCodeObject, CoFlagsVarArgs)
{
	PyCodeObject code;
	code.co_flags = CO_VARARGS | CO_VARKEYWORDS;
	EXPECT_TRUE(code.hasVarArgs());
	EXPECT_TRUE(code.hasVarKwargs());
	EXPECT_FALSE(code.isGenerator());
}

TEST(PyCodeObject, LineAtWithEmptyTable)
{
	PyCodeObject code;
	code.co_firstlineno = 5;
	// No lineTable; should return firstlineno
	EXPECT_EQ(5, code.lineAt(0));
	EXPECT_EQ(5, code.lineAt(100));
}

TEST(PyCodeObject, LineAtWithTable)
{
	PyCodeObject code;
	code.co_firstlineno = 1;
	code.lineTable.push_back({0, 4, 10, -1, -1, -1});
	code.lineTable.push_back({4, 10, 11, -1, -1, -1});
	code.lineTable.push_back({10, 16, 12, -1, -1, -1});

	EXPECT_EQ(10, code.lineAt(0));
	EXPECT_EQ(10, code.lineAt(2));
	EXPECT_EQ(11, code.lineAt(4));
	EXPECT_EQ(11, code.lineAt(8));
	EXPECT_EQ(12, code.lineAt(10));
	EXPECT_EQ(1, code.lineAt(20)); // outside table → firstlineno
}

// ─── Line table decoder tests ─────────────────────────────────────────────────

TEST(LineTableDecoder, EmptyLnotab)
{
	auto result = decodeLnotab({}, 1, 10);
	ASSERT_FALSE(result.empty());
	EXPECT_EQ(0u, result[0].startOffset);
	EXPECT_EQ(10u, result[0].endOffset);
	EXPECT_EQ(1, result[0].line);
}

TEST(LineTableDecoder, SimpleLnotab)
{
	// lnotab: [(4, 1), (2, 2)] means:
	//   offset 0-3: line 1
	//   offset 4-5: line 2
	//   offset 6+:  line 4
	std::vector<uint8_t> lnotab = {4, 1, 2, 2};
	auto result = decodeLnotab(lnotab, 1, 10);
	EXPECT_FALSE(result.empty());
	// There should be entries covering line 1, 2, 4
	bool foundLine1 = false, foundLine2 = false;
	for (const auto& e: result)
	{
		if (e.line == 1) foundLine1 = true;
		if (e.line == 2) foundLine2 = true;
	}
	EXPECT_TRUE(foundLine1);
}

TEST(LineTableDecoder, LnotabCoverageToEnd)
{
	std::vector<uint8_t> lnotab = {2, 1};
	auto result = decodeLnotab(lnotab, 5, 8);
	EXPECT_FALSE(result.empty());
	// All offsets should be covered
	for (const auto& e: result)
	{
		EXPECT_LE(e.startOffset, e.endOffset);
	}
}

TEST(LineTableDecoder, Decode311EmptyTable)
{
	auto result = decodeLnotab311({}, 1, 10);
	EXPECT_FALSE(result.empty());
}

// ─── Exception table decoder tests ───────────────────────────────────────────

TEST(ExceptionTableDecoder, EmptyTable)
{
	auto result = decodeExceptionTable({});
	EXPECT_TRUE(result.empty());
}

TEST(ExceptionTableDecoder, SimpleEntry)
{
	// ULEB128-encoded: start=4, length=8, target=20, dl=2 (depth=1, lasti=0)
	// 4 = 0x04, 8 = 0x08, 20 = 0x14, 2 = 0x02
	std::vector<uint8_t> table = {0x04, 0x08, 0x14, 0x02};
	auto result = decodeExceptionTable(table);
	ASSERT_EQ(1u, result.size());
	EXPECT_EQ(4u, result[0].start);
	EXPECT_EQ(12u, result[0].end); // start + length = 4 + 8
	EXPECT_EQ(20u, result[0].target);
	EXPECT_EQ(1u, result[0].depth);
	EXPECT_FALSE(result[0].lasti);
}

// A ULEB128 field whose continuation bits never clear drives the shift past
// the width of the uint32_t result; the surplus groups must be consumed
// without shifting.
//
// This input decodes to nothing -- the run swallows the whole buffer, so the
// three fields that would complete an entry are never read. The loop over the
// result therefore never ran, and the assertion inside it never executed: the
// test passed equally for a decoder that shifted out of range and for one
// written `return {};`. Both halves are now stated.
TEST(ExceptionTableDecoder, UnterminatedUleb128RunYieldsNoEntry)
{
	std::vector<uint8_t> table(24, 0xFF);
	table.push_back(0x00);
	auto result = decodeExceptionTable(table);
	EXPECT_TRUE(result.empty());
}

TEST(ExceptionTableDecoder, AnOverlongRunSaturatesRatherThanShiftingOut)
{
	// Ten continuation groups for `start` -- well past the five a uint32_t
	// can hold -- then a terminator and three zero fields to complete the
	// entry, so the decoded value is observable.
	std::vector<uint8_t> table(10, 0xFF);
	table.push_back(0x00); // terminates the run
	table.push_back(0x00); // length
	table.push_back(0x00); // target
	table.push_back(0x00); // depth / lasti

	auto result = decodeExceptionTable(table);
	ASSERT_EQ(1u, result.size());
	EXPECT_EQ(0xFFFFFFFFu, result[0].start);
	EXPECT_GE(result[0].end, result[0].start);
	EXPECT_EQ(0u, result[0].target);
}

TEST(ExceptionTableDecoder, Uleb128WideValueKeepsLowGroups)
{
	// start = 0x81 0x80 0x80 0x80 0x00 -> five groups, only bit 7 set.
	std::vector<uint8_t> table = {0x81, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00};
	auto result = decodeExceptionTable(table);
	ASSERT_EQ(1u, result.size());
	EXPECT_EQ(1u, result[0].start);
}

TEST(ExceptionTableDecoder, MultipleEntries)
{
	// Two entries: [4,8,20,2] and [0,4,10,3]
	std::vector<uint8_t> table = {0x04, 0x08, 0x14, 0x02, 0x00, 0x04, 0x0A, 0x03};
	auto result = decodeExceptionTable(table);
	ASSERT_EQ(2u, result.size());
	EXPECT_EQ(4u, result[0].start);
	EXPECT_EQ(0u, result[1].start);
}

// ─── OpcodeInfo tests ──────────────────────────────────────────────────────────

TEST(OpcodeInfo, LoadFastHasArg38)
{
	PythonVersion ver38{3, 8, 0, ""};
	auto info = opcodeInfo(124, ver38); // LOAD_FAST = 0x7C
	EXPECT_TRUE(info.hasArg);
	EXPECT_EQ(std::string("LOAD_FAST"), info.name);
}

TEST(OpcodeInfo, LoadFastHasArg311)
{
	PythonVersion ver311{3, 11, 0, ""};
	auto info = opcodeInfo(124, ver311);
	EXPECT_TRUE(info.hasArg); // All opcodes have arg in 3.11+
}

TEST(OpcodeInfo, PopTopNoArg38)
{
	PythonVersion ver38{3, 8, 0, ""};
	auto info = opcodeInfo(1, ver38); // POP_TOP
	EXPECT_FALSE(info.hasArg);
	EXPECT_EQ(std::string("POP_TOP"), info.name);
}

TEST(OpcodeInfo, ExtendedArg38)
{
	PythonVersion ver38{3, 8, 0, ""};
	auto info = opcodeInfo(90, ver38); // EXTENDED_ARG in 3.8-3.10
	EXPECT_EQ(OpcodeKind::ExtendedArg, info.kind);
}

TEST(OpcodeInfo, ExtendedArg311)
{
	PythonVersion ver311{3, 11, 0, ""};
	auto info = opcodeInfo(144, ver311); // EXTENDED_ARG in 3.11+
	EXPECT_EQ(OpcodeKind::ExtendedArg, info.kind);
}

TEST(OpcodeInfo, JumpForwardIsJump)
{
	PythonVersion ver38{3, 8, 0, ""};
	auto info = opcodeInfo(110, ver38); // JUMP_FORWARD
	EXPECT_TRUE(info.isJump());
}

TEST(OpcodeInfo, ReturnValueIsReturn)
{
	PythonVersion ver38{3, 8, 0, ""};
	auto info = opcodeInfo(83, ver38); // RETURN_VALUE
	EXPECT_TRUE(info.isReturn());
}

TEST(OpcodeInfo, HaveArgument38)
{
	PythonVersion ver38{3, 8, 0, ""};
	EXPECT_EQ(90u, haveArgument(ver38));
}

TEST(OpcodeInfo, HaveArgument311)
{
	PythonVersion ver311{3, 11, 0, ""};
	EXPECT_EQ(0u, haveArgument(ver311));
}

TEST(OpcodeInfo, CallFunctionIsCall)
{
	PythonVersion ver38{3, 8, 0, ""};
	auto info = opcodeInfo(131, ver38); // CALL_FUNCTION
	EXPECT_TRUE(info.isCall());
}

// ─── PycReader integration tests ─────────────────────────────────────────────

// Minimal .pyc for Python 3.10: "pass" in a module
// This is a hand-crafted .pyc that represents a trivial code object.

static std::vector<uint8_t> buildPyc38WithCode(const std::vector<uint8_t>& coCode)
{
	// Python 3.8 magic
	uint32_t magic = makeRawMagic(3413);
	std::vector<uint8_t> buf;

	// Header (16 bytes)
	for (int i = 0; i < 4; ++i)
		buf.push_back((magic >> (8 * i)) & 0xFF);
	for (int i = 0; i < 4; ++i)
		buf.push_back(0); // bit_field
	for (int i = 0; i < 4; ++i)
		buf.push_back(0); // mtime
	for (int i = 0; i < 4; ++i)
		buf.push_back(0); // source_size

	// Marshal stream: TYPE_CODE for top-level code object
	// We'll build a minimal one:
	MarshalBuilder mb;

	// co_argcount=0, co_posonlyargcount=0, co_kwonlyargcount=0,
	// co_nlocals=0, co_stacksize=1, co_flags=0x40 (CO_NOFREE)
	// co_code = LOAD_CONST 0, RETURN_VALUE
	// co_consts = (None,)
	// co_names = ()
	// co_varnames = ()
	// co_freevars = ()
	// co_cellvars = ()
	// co_filename = "<string>"
	// co_name = "<module>"
	// co_firstlineno = 1
	// co_lnotab = b""

	buf.push_back(0x80 | 'c'); // TYPE_CODE with FLAG_REF

	auto writeI32 = [&](int32_t v) {
		for (int i = 0; i < 4; ++i)
			buf.push_back((v >> (8 * i)) & 0xFF);
	};

	writeI32(0);                               // co_argcount
	writeI32(0);                               // co_posonlyargcount
	writeI32(0);                               // co_kwonlyargcount
	writeI32(0);                               // co_nlocals
	writeI32(1);                               // co_stacksize
	writeI32(static_cast<int32_t>(CO_NOFREE)); // co_flags

	// co_code
	buf.push_back('B'); // TYPE_BYTES
	writeI32(static_cast<int32_t>(coCode.size()));
	for (uint8_t b: coCode)
		buf.push_back(b);

	// co_consts: (None,) — small tuple of 1 element
	buf.push_back(')');
	buf.push_back(1);   // SMALL_TUPLE size=1
	buf.push_back('N'); // None

	// co_names: ()
	buf.push_back(')');
	buf.push_back(0);

	// co_varnames: ()
	buf.push_back(')');
	buf.push_back(0);

	// co_freevars: ()
	buf.push_back(')');
	buf.push_back(0);

	// co_cellvars: ()
	buf.push_back(')');
	buf.push_back(0);

	// co_filename: SHORT_ASCII "<string>"
	buf.push_back('z');
	buf.push_back(8);
	for (char c: std::string("<string>"))
		buf.push_back(c);

	// co_name: SHORT_ASCII "<module>"
	buf.push_back('z');
	buf.push_back(8);
	for (char c: std::string("<module>"))
		buf.push_back(c);

	// co_firstlineno
	writeI32(1);

	// co_lnotab: empty bytes
	buf.push_back('B');
	writeI32(0);

	return buf;
}

static std::vector<uint8_t> buildMinimalPyc38()
{
	// LOAD_CONST 0 (100 0), RETURN_VALUE (83)
	return buildPyc38WithCode({100, 0, 83, 0});
}

// 3.11 drops co_nlocals and the separate free/cell tuples and gains qualname
// and an exception table; the field order is the one readCodeFields expects.
static std::vector<uint8_t> buildPyc311WithCode(const std::vector<uint8_t>& coCode)
{
	uint32_t magic = makeRawMagic(3495);
	std::vector<uint8_t> buf;
	for (int i = 0; i < 4; ++i)
		buf.push_back((magic >> (8 * i)) & 0xFF);
	for (int i = 0; i < 12; ++i)
		buf.push_back(0); // bit_field, mtime, source_size

	auto writeI32 = [&](int32_t v) {
		for (int i = 0; i < 4; ++i)
			buf.push_back((v >> (8 * i)) & 0xFF);
	};
	auto shortAscii = [&](const std::string& str) {
		buf.push_back('z');
		buf.push_back(static_cast<uint8_t>(str.size()));
		for (char c: str)
			buf.push_back(static_cast<uint8_t>(c));
	};
	auto emptyBytes = [&]() {
		buf.push_back('B');
		writeI32(0);
	};
	auto emptyTuple = [&]() {
		buf.push_back(')');
		buf.push_back(0);
	};

	buf.push_back(0x80 | 'c');                 // TYPE_CODE with FLAG_REF
	writeI32(0);                               // co_argcount
	writeI32(0);                               // co_posonlyargcount
	writeI32(0);                               // co_kwonlyargcount
	writeI32(1);                               // co_stacksize
	writeI32(static_cast<int32_t>(CO_NOFREE)); // co_flags

	buf.push_back('B'); // co_code
	writeI32(static_cast<int32_t>(coCode.size()));
	for (uint8_t b: coCode)
		buf.push_back(b);

	buf.push_back(')');
	buf.push_back(1);
	buf.push_back('N');     // co_consts = (None,)
	emptyTuple();           // co_names
	emptyTuple();           // co_localsplusnames
	emptyBytes();           // co_localspluskinds
	shortAscii("<string>"); // co_filename
	shortAscii("<module>"); // co_name
	shortAscii("<module>"); // co_qualname
	writeI32(1);            // co_firstlineno
	emptyBytes();           // co_linetable
	emptyBytes();           // co_exceptiontable
	return buf;
}

TEST(PycReader, ParseMinimalPyc38)
{
	auto buf = buildMinimalPyc38();
	PycReader reader;
	auto result = reader.read(buf.data(), buf.size(), "test.pyc");

	EXPECT_TRUE(result.success) << result.error;
	EXPECT_EQ(3, result.version.major);
	EXPECT_EQ(8, result.version.minor);
	EXPECT_GT(result.totalCodeObjects, 0);
	EXPECT_EQ("string", result.module.name());
}

TEST(PycReader, ParseMinimalPyc38HasClass)
{
	auto buf = buildMinimalPyc38();
	PycReader reader;
	auto result = reader.read(buf.data(), buf.size(), "test.pyc");
	ASSERT_TRUE(result.success);
	EXPECT_FALSE(result.module.classes().empty());
}

TEST(PycReader, ParseMinimalPyc38HasMethod)
{
	auto buf = buildMinimalPyc38();
	PycReader reader;
	auto result = reader.read(buf.data(), buf.size());
	ASSERT_TRUE(result.success);
	ASSERT_FALSE(result.module.classes().empty());
	const auto& cls = result.module.classes().front();
	EXPECT_FALSE(cls.methods.empty());
	EXPECT_EQ("<module>", cls.methods.front().name);
	EXPECT_EQ("<string>", cls.sourceFile);
}

TEST(PycReader, ParseMinimalPyc38CFGNotEmpty)
{
	auto buf = buildMinimalPyc38();
	PycReadOptions opts;
	opts.buildCFG = true;
	PycReader reader(opts);
	auto result = reader.read(buf.data(), buf.size());
	ASSERT_TRUE(result.success);
	ASSERT_FALSE(result.module.classes().empty());
	const auto& cls = result.module.classes().front();
	ASSERT_FALSE(cls.methods.empty());
	EXPECT_GT(cls.methods.front().cfg.blockCount(), 0u);
}

// A compiler emits at most three EXTENDED_ARG prefixes; nothing stops a file
// from chaining forty. The accumulated operand must wrap at the width the
// wordcode encodes instead of shifting a signed accumulator out of range, and
// the jump target it feeds must be computed wide enough not to overflow.
TEST(PycReader, ExtendedArgChainDoesNotOverflow)
{
	std::vector<uint8_t> code;
	for (int i = 0; i < 40; ++i)
	{
		code.push_back(90); // EXTENDED_ARG
		code.push_back(0xFF);
	}
	code.push_back(110);
	code.push_back(0xFF); // JUMP_FORWARD
	code.push_back(83);
	code.push_back(0); // RETURN_VALUE

	auto buf = buildPyc38WithCode(code);
	PycReadOptions opts;
	opts.buildCFG = true;
	PycReader reader(opts);
	auto result = reader.read(buf.data(), buf.size());
	ASSERT_TRUE(result.success) << result.error;
	ASSERT_FALSE(result.module.classes().empty());
	EXPECT_FALSE(result.module.classes().front().methods.empty());
}

// Same chain on the 3.11+ opcode, which takes the other EXTENDED_ARG branch.
TEST(PycReader, ExtendedArgChainDoesNotOverflow311)
{
	std::vector<uint8_t> code;
	for (int i = 0; i < 40; ++i)
	{
		code.push_back(144); // EXTENDED_ARG in 3.11+
		code.push_back(0xFF);
	}
	code.push_back(110);
	code.push_back(0xFF); // JUMP_FORWARD
	code.push_back(83);
	code.push_back(0); // RETURN_VALUE

	auto buf = buildPyc311WithCode(code);
	PycReadOptions opts;
	opts.buildCFG = true;
	PycReader reader(opts);
	auto result = reader.read(buf.data(), buf.size());
	ASSERT_TRUE(result.success) << result.error;
	ASSERT_FALSE(result.module.classes().empty());
	EXPECT_FALSE(result.module.classes().front().methods.empty());
}

// The operand a prefix chain builds can land in the top half of the int32_t
// range, where doubling it to scale a wordcode jump offset no longer fits.
TEST(PycReader, ExtendedArgJumpOperandNearInt32Max)
{
	std::vector<uint8_t> code;
	for (int i = 0; i < 3; ++i)
	{
		code.push_back(90); // EXTENDED_ARG
		code.push_back(0x80);
	}
	code.push_back(110);
	code.push_back(0x80); // JUMP_FORWARD
	code.push_back(83);
	code.push_back(0); // RETURN_VALUE

	auto buf = buildPyc38WithCode(code);
	PycReadOptions opts;
	opts.buildCFG = true;
	PycReader reader(opts);
	auto result = reader.read(buf.data(), buf.size());
	ASSERT_TRUE(result.success) << result.error;
	ASSERT_FALSE(result.module.classes().empty());
	EXPECT_FALSE(result.module.classes().front().methods.empty());
}

TEST(PycReader, TooSmallFileFails)
{
	std::vector<uint8_t> buf = {1, 2, 3};
	PycReader reader;
	auto result = reader.read(buf.data(), buf.size());
	EXPECT_FALSE(result.success);
	EXPECT_FALSE(result.error.empty());
}

TEST(PycReader, UnknownMagicFails)
{
	std::vector<uint8_t> buf(16, 0);
	buf[0] = 0xFF;
	buf[1] = 0xFF;
	buf[2] = 0xFF;
	buf[3] = 0xFF;
	PycReader reader;
	auto result = reader.read(buf.data(), buf.size());
	EXPECT_FALSE(result.success);
	EXPECT_FALSE(result.error.empty());
}

TEST(PycReader, SkipCompilerGeneratedByDefault)
{
	// Build a .pyc where the root code's co_consts contains a <listcomp> code object
	// This is hard to do without a full marshal builder; just verify the option works
	auto buf = buildMinimalPyc38();
	PycReadOptions opts;
	opts.skipCompGenerated = true;
	opts.recurseNested = true;
	PycReader reader(opts);
	auto result = reader.read(buf.data(), buf.size());
	EXPECT_TRUE(result.success);
}

TEST(PycReader, NoBuildCFG)
{
	auto buf = buildMinimalPyc38();
	PycReadOptions opts;
	opts.buildCFG = false;
	PycReader reader(opts);
	auto result = reader.read(buf.data(), buf.size());
	ASSERT_TRUE(result.success);
	ASSERT_FALSE(result.module.classes().empty());
	const auto& cls = result.module.classes().front();
	ASSERT_FALSE(cls.methods.empty());
	// Without building CFG, blocks should be empty
	EXPECT_EQ(0u, cls.methods.front().cfg.blockCount());
}

// ─── BcOpcode::PYTHON_* distinctness ────────────────────────────────────────

TEST(BcOpcode, PythonOpcodesAreDistinct)
{
	// Verify that a sample of PYTHON_* opcodes have different values
	EXPECT_NE(BcOpcode::PYTHON_LOAD_FAST, BcOpcode::PYTHON_STORE_FAST);
	EXPECT_NE(BcOpcode::PYTHON_LOAD_CONST, BcOpcode::PYTHON_LOAD_FAST);
	EXPECT_NE(BcOpcode::PYTHON_RETURN_VALUE, BcOpcode::PYTHON_LOAD_CONST);
	EXPECT_NE(BcOpcode::PYTHON_JUMP_FORWARD, BcOpcode::PYTHON_JUMP_ABSOLUTE);
	EXPECT_NE(BcOpcode::PYTHON_BINARY_ADD, BcOpcode::PYTHON_BINARY_OP);
	EXPECT_NE(BcOpcode::PYTHON_CALL, BcOpcode::PYTHON_CALL_FUNCTION);
	EXPECT_NE(BcOpcode::PYTHON_UNKNOWN, BcOpcode::PYTHON_NOP);
}

TEST(BcOpcode, PythonOpcodesDistinctFromDotnet)
{
	// PYTHON_* opcodes should not collide with DOTNET_* ones
	EXPECT_NE(BcOpcode::PYTHON_LOAD_FAST, BcOpcode::DOTNET_LDLOC);
	EXPECT_NE(BcOpcode::PYTHON_RETURN_VALUE, BcOpcode::DOTNET_RET);
}

// ─── MarshalObject::toConst tests ────────────────────────────────────────────

TEST(MarshalObject, ToConstNone)
{
	MarshalObject obj;
	obj.type = MarshalObject::Type::None;
	auto c = obj.toConst();
	EXPECT_EQ(PyCodeObject::Const::Kind::None, c.kind);
}

TEST(MarshalObject, ToConstInt)
{
	MarshalObject obj;
	obj.type = MarshalObject::Type::Int;
	obj.value = int64_t{99};
	auto c = obj.toConst();
	EXPECT_EQ(PyCodeObject::Const::Kind::Int, c.kind);
	EXPECT_EQ(99LL, c.ival);
}

TEST(MarshalObject, ToConstStr)
{
	MarshalObject obj;
	obj.type = MarshalObject::Type::Str;
	obj.value = std::string{"hello"};
	auto c = obj.toConst();
	EXPECT_EQ(PyCodeObject::Const::Kind::Unicode, c.kind);
	EXPECT_EQ("hello", c.sval);
}

TEST(MarshalObject, ToConstFloat)
{
	MarshalObject obj;
	obj.type = MarshalObject::Type::Float;
	obj.value = 2.718;
	auto c = obj.toConst();
	EXPECT_EQ(PyCodeObject::Const::Kind::Float, c.kind);
	EXPECT_NEAR(2.718, c.fval, 1e-10);
}

TEST(MarshalObject, ToConstTuple)
{
	MarshalObject obj;
	obj.type = MarshalObject::Type::Tuple;
	auto inner = std::make_shared<MarshalObject>();
	inner->type = MarshalObject::Type::Int;
	inner->value = int64_t{7};
	obj.value = std::vector<std::shared_ptr<MarshalObject>>{inner};
	auto c = obj.toConst();
	EXPECT_EQ(PyCodeObject::Const::Kind::Tuple, c.kind);
	ASSERT_EQ(1u, c.elements.size());
	EXPECT_EQ(PyCodeObject::Const::Kind::Int, c.elements[0].kind);
	EXPECT_EQ(7LL, c.elements[0].ival);
}

// ─── PythonVersion comparison tests ──────────────────────────────────────────

TEST(PythonVersion, LessThan)
{
	PythonVersion v38{3, 8, 0, ""};
	PythonVersion v311{3, 11, 0, ""};
	EXPECT_LT(v38, v311);
	EXPECT_FALSE(v311 < v38);
}

TEST(PythonVersion, Equal)
{
	PythonVersion a{3, 10, 3439, "3.10"};
	PythonVersion b{3, 10, 3450, "3.10"};
	EXPECT_EQ(a, b); // Same major.minor = same version
}

// ─── Line-table accumulation and nesting depth ───────────────────────────────

// `line` is an int32_t accumulating deltas that come out of the file, and
// nothing bounded it. A long enough table walks it past INT32_MAX, which is
// signed overflow -- undefined behaviour, not a wrong line number. Reaching it
// takes roughly a 34 MB co_linetable, which is why the fuzzer never did: its
// inputs are capped far below that.
TEST(LineTable, LnotabLineAccumulationSaturatesInsteadOfOverflowing)
{
	// Each pair is (byte delta, line delta); 255 line delta per entry, repeated
	// until the accumulator would pass INT32_MAX.
	std::vector<uint8_t> lnotab;
	lnotab.reserve(2 * 9'000'000);
	for (int i = 0; i < 9'000'000; ++i)
	{
		lnotab.push_back(0);
		lnotab.push_back(255);
	}

	const auto entries = decodeLnotab(lnotab, INT32_MAX - 1000, 64);
	ASSERT_FALSE(entries.empty());
	for (const auto& e: entries)
	{
		EXPECT_GE(e.line, 0);
		EXPECT_LE(e.line, INT32_MAX);
	}
}

TEST(LineTable, Lnotab311LineAccumulationSaturates)
{
	// codeType 2 (no-col form): one signed line delta per entry.
	std::vector<uint8_t> table;
	table.reserve(2 * 9'000'000);
	for (int i = 0; i < 9'000'000; ++i)
	{
		table.push_back(0x02); // entry: 1 code unit, codeType 2
		table.push_back(0x7F); // +127
	}

	const auto entries = decodeLnotab311(table, INT32_MAX - 1000, 64);
	for (const auto& e: entries)
	{
		EXPECT_GE(e.line, 0);
		EXPECT_LE(e.line, INT32_MAX);
	}
}

// A negative delta must not drive the line below zero either.
TEST(LineTable, Lnotab311LineDoesNotGoNegative)
{
	std::vector<uint8_t> table;
	for (int i = 0; i < 64; ++i)
	{
		table.push_back(0x02);
		table.push_back(0x80); // -128 as int8_t
	}

	const auto entries = decodeLnotab311(table, 1, 64);
	for (const auto& e: entries)
		EXPECT_GE(e.line, 0);
}

// A marshal container costs two bytes per nesting level, so a 30 KB file can
// ask for 15,000 levels of recursion and exhaust the stack. No bound derived
// from the input size catches that; readObject carries a fixed depth limit.
TEST(MarshalReader, RefusesNestingDeeperThanTheLimit)
{
	std::vector<uint8_t> stream;
	for (int i = 0; i < 15000; ++i)
	{
		stream.push_back(')');
		stream.push_back(1);
	}
	stream.push_back('N');

	PythonVersion v{3, 8};
	MarshalReader reader(stream.data(), stream.size(), v);

	// Must refuse rather than recurse. The point is that it returns at all.
	const auto obj = reader.readObject();
	EXPECT_TRUE(reader.hasError());
	EXPECT_EQ(obj, nullptr);
}

// Nesting within the limit still parses, so the bound does not reject
// legitimate files.
TEST(MarshalReader, AcceptsNestingWithinTheLimit)
{
	std::vector<uint8_t> stream;
	for (int i = 0; i < 50; ++i)
	{
		stream.push_back(')');
		stream.push_back(1);
	}
	stream.push_back('N');

	PythonVersion v{3, 8};
	MarshalReader reader(stream.data(), stream.size(), v);

	const auto obj = reader.readObject();
	EXPECT_NE(obj, nullptr);
	EXPECT_FALSE(reader.hasError());
}

// ─── Constant graph expansion ────────────────────────────────────────────────
//
// FLAG_REF and TYPE_REF let a marshal stream describe a graph, while a
// PyCodeObject::Const is a tree, so a node reachable by two paths is written
// out twice. Nesting that doubling k levels deep costs about seven bytes per
// level and yields 2^k nodes: a 176-byte stream exhausted four gigabytes.
// Parse depth stays at k, so the nesting limit never sees it.

// A stream of `levels` two-element tuples, each holding the next level twice:
// once inline (which registers it) and once by reference.
static std::vector<uint8_t> sharedTupleChain(int levels)
{
	std::vector<uint8_t> s;
	for (int i = 0; i < levels; ++i)
	{
		s.push_back(static_cast<uint8_t>(')' | 0x80)); // small tuple, registered
		s.push_back(2);
	}
	s.push_back(static_cast<uint8_t>('N' | 0x80)); // innermost leaf, registered
	for (int i = levels - 1; i >= 0; --i)
	{
		s.push_back('r'); // TYPE_REF back to the level below
		for (int k = 0; k < 4; ++k)
			s.push_back(static_cast<uint8_t>(static_cast<uint32_t>(i + 1) >> (8 * k)));
	}
	return s;
}

static uint64_t countConstNodes(const PyCodeObject::Const& c)
{
	uint64_t n = 1;
	for (const auto& e: c.elements)
		n += countConstNodes(e);
	return n;
}

TEST(MarshalObject, ToConstStopsExpandingASharedGraphAtTheLimit)
{
	const auto stream = sharedTupleChain(30);
	PythonVersion v{3, 11};
	MarshalReader reader(stream.data(), stream.size(), v);

	const auto obj = reader.readObject();
	ASSERT_NE(obj, nullptr);
	EXPECT_FALSE(reader.hasError());

	// 2^31 - 1 nodes without a ceiling, from 211 bytes.
	const auto c = obj->toConst();
	EXPECT_LE(countConstNodes(c), MarshalObject::kMaxConstNodes);
}

TEST(MarshalObject, ATruncatedConstantSaysSo)
{
	const auto stream = sharedTupleChain(30);
	PythonVersion v{3, 11};
	MarshalReader reader(stream.data(), stream.size(), v);

	const auto obj = reader.readObject();
	ASSERT_NE(obj, nullptr);

	// Walk down the first element of each tuple until a truncated container
	// turns up; a short tuple must never pass for a complete one.
	const PyCodeObject::Const c = obj->toConst();
	const PyCodeObject::Const* cur = &c;
	bool sawMarker = false;
	while (true)
	{
		if (cur->sval == MarshalObject::kConstTruncated)
		{
			sawMarker = true;
			break;
		}
		if (cur->elements.empty()) break;
		cur = &cur->elements.front();
	}
	EXPECT_TRUE(sawMarker);
}

TEST(MarshalObject, ASharedGraphSmallEnoughToExpandIsStillExpandedInFull)
{
	const auto stream = sharedTupleChain(8);
	PythonVersion v{3, 11};
	MarshalReader reader(stream.data(), stream.size(), v);

	const auto obj = reader.readObject();
	ASSERT_NE(obj, nullptr);

	// Eight levels of doubling over a leaf: 2^9 - 1 nodes, every one present.
	const auto c = obj->toConst();
	EXPECT_EQ(countConstNodes(c), 511u);
}
