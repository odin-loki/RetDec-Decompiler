/**
 * @file tests/jvm_parser/jvm_parser_test.cpp
 * @brief Unit tests for the JVM class-file / JAR parser.
 *
 * Coverage:
 *   - BinaryReader big-endian reads and bounds checking.
 *   - ConstPool: all 18 entry kinds, accessor helpers.
 *   - JvmSignatureParser: base types, class descriptors, array descriptors,
 *     method descriptors, generic class/method signatures, wildcards.
 *   - Attribute parsers: Code, BootstrapMethods, InnerClasses, Record,
 *     PermittedSubclasses, MethodParameters.
 *   - ClassFile parser: hand-crafted minimal .class bytes for Hello World.
 *   - JvmLifter: opcode-by-opcode decoding, leader detection, edge wiring,
 *     exception table.
 *   - JarReader: minimal ZIP listEntries.
 *   - Version helpers: javaRelease.
 */

#include "retdec/jvm_parser/jvm_class_parser.h"
#include "retdec/jvm_parser/jvm_jar_reader.h"
#include "retdec/jvm_parser/jvm_signature.h"
#include "retdec/jvm_parser/jvm_lifter.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace retdec::jvm_parser;
using namespace retdec::bc_module;
using namespace retdec::bc_module::types;

// ══════════════════════════════════════════════════════════════════════════════
// BinaryReader
// ══════════════════════════════════════════════════════════════════════════════

TEST(BinaryReader, ReadU1)
{
	uint8_t data[] = {0xCA, 0xFE};
	BinaryReader r(data, 2);
	EXPECT_EQ(r.u1(), 0xCA);
	EXPECT_EQ(r.u1(), 0xFE);
}

TEST(BinaryReader, ReadU2BigEndian)
{
	uint8_t data[] = {0x01, 0x02};
	BinaryReader r(data, 2);
	EXPECT_EQ(r.u2(), 0x0102);
}

TEST(BinaryReader, ReadU4BigEndian)
{
	uint8_t data[] = {0xCA, 0xFE, 0xBA, 0xBE};
	BinaryReader r(data, 4);
	EXPECT_EQ(r.u4(), 0xCAFEBABEu);
}

TEST(BinaryReader, ReadFloat)
{
	// 1.0f in IEEE 754 big-endian = 0x3F800000
	uint8_t data[] = {0x3F, 0x80, 0x00, 0x00};
	BinaryReader r(data, 4);
	EXPECT_FLOAT_EQ(r.f4(), 1.0f);
}

TEST(BinaryReader, ReadDouble)
{
	// 1.0 in IEEE 754 big-endian = 0x3FF0000000000000
	uint8_t data[] = {0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	BinaryReader r(data, 8);
	EXPECT_DOUBLE_EQ(r.f8(), 1.0);
}

TEST(BinaryReader, ReadUtf8)
{
	const char* s = "Hello";
	std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(s), reinterpret_cast<const uint8_t*>(s) + 5);
	BinaryReader r(data.data(), data.size());
	EXPECT_EQ(r.utf8(5), "Hello");
}

TEST(BinaryReader, OutOfRange)
{
	uint8_t data[] = {0x01};
	BinaryReader r(data, 1);
	r.u1();
	EXPECT_THROW(r.u1(), JvmParseError);
}

TEST(BinaryReader, Skip)
{
	uint8_t data[] = {0x01, 0x02, 0x03};
	BinaryReader r(data, 3);
	r.skip(2);
	EXPECT_EQ(r.u1(), 0x03);
}

// ══════════════════════════════════════════════════════════════════════════════
// ConstPool
// ══════════════════════════════════════════════════════════════════════════════

// Helper: build a minimal constant pool binary
static std::vector<uint8_t> makeCP(const std::vector<std::vector<uint8_t>>& entries)
{
	// Prepend cp_count = entries.size() + 1
	std::vector<uint8_t> out;
	uint16_t cnt = static_cast<uint16_t>(entries.size() + 1);
	out.push_back(cnt >> 8);
	out.push_back(cnt & 0xFF);
	for (const auto& e: out)
	{
	} // no-op
	for (const auto& e: entries)
	{
		for (uint8_t b: e)
			out.push_back(b);
	}
	return out;
}

static std::vector<uint8_t> cpUtf8(const std::string& s)
{
	std::vector<uint8_t> v = {1}; // tag
	uint16_t len = static_cast<uint16_t>(s.size());
	v.push_back(len >> 8);
	v.push_back(len & 0xFF);
	for (char c: s)
		v.push_back(static_cast<uint8_t>(c));
	return v;
}

static std::vector<uint8_t> cpInt(int32_t val)
{
	return {
		3,
		static_cast<uint8_t>(val >> 24),
		static_cast<uint8_t>(val >> 16),
		static_cast<uint8_t>(val >> 8),
		static_cast<uint8_t>(val)};
}

static std::vector<uint8_t> cpClass(uint16_t idx)
{
	return {7, static_cast<uint8_t>(idx >> 8), static_cast<uint8_t>(idx)};
}

static std::vector<uint8_t> cpString(uint16_t idx)
{
	return {8, static_cast<uint8_t>(idx >> 8), static_cast<uint8_t>(idx)};
}

static std::vector<uint8_t> cpNaT(uint16_t name, uint16_t desc)
{
	return {
		12,
		static_cast<uint8_t>(name >> 8),
		static_cast<uint8_t>(name),
		static_cast<uint8_t>(desc >> 8),
		static_cast<uint8_t>(desc)};
}

static std::vector<uint8_t> cpMethodref(uint16_t cls, uint16_t nat)
{
	return {
		10,
		static_cast<uint8_t>(cls >> 8),
		static_cast<uint8_t>(cls),
		static_cast<uint8_t>(nat >> 8),
		static_cast<uint8_t>(nat)};
}

TEST(ConstPool, ReadUtf8)
{
	// cp: [1=Utf8("Hello")]
	auto data = cpUtf8("Hello");
	// Need cp_count prefix
	std::vector<uint8_t> raw = {0, 2}; // count = 2
	for (uint8_t b: data)
		raw.push_back(b);
	BinaryReader r(raw.data(), raw.size());
	auto pool = ConstPool::read(r);
	EXPECT_EQ(pool.utf8(1), "Hello");
}

TEST(ConstPool, ReadInteger)
{
	std::vector<uint8_t> raw = {0, 2}; // cp_count
	auto entry = cpInt(42);
	for (uint8_t b: entry)
		raw.push_back(b);
	BinaryReader r(raw.data(), raw.size());
	auto pool = ConstPool::read(r);
	EXPECT_EQ(pool.tag(1), CpTag::Integer);
	EXPECT_EQ(std::get<CpInt>(pool.entry(1)).value, 42);
}

TEST(ConstPool, ReadClassAndString)
{
	// cp: [1=Utf8("Foo"), 2=Class(#1), 3=Utf8("bar"), 4=String(#3)]
	std::vector<uint8_t> raw = {0, 5};
	for (uint8_t b: cpUtf8("Foo"))
		raw.push_back(b);
	for (uint8_t b: cpClass(1))
		raw.push_back(b);
	for (uint8_t b: cpUtf8("bar"))
		raw.push_back(b);
	for (uint8_t b: cpString(3))
		raw.push_back(b);
	BinaryReader r(raw.data(), raw.size());
	auto pool = ConstPool::read(r);
	EXPECT_EQ(pool.className(2), "Foo");
	EXPECT_EQ(pool.string(4), "bar");
}

TEST(ConstPool, ReadMethodref)
{
	// [1=Utf8("Foo"), 2=Class(#1), 3=Utf8("bar"), 4=Utf8("()V"), 5=NaT(#3,#4), 6=Methodref(#2,#5)]
	std::vector<uint8_t> raw = {0, 7};
	for (uint8_t b: cpUtf8("Foo"))
		raw.push_back(b);
	for (uint8_t b: cpClass(1))
		raw.push_back(b);
	for (uint8_t b: cpUtf8("bar"))
		raw.push_back(b);
	for (uint8_t b: cpUtf8("()V"))
		raw.push_back(b);
	for (uint8_t b: cpNaT(3, 4))
		raw.push_back(b);
	for (uint8_t b: cpMethodref(2, 5))
		raw.push_back(b);
	BinaryReader r(raw.data(), raw.size());
	auto pool = ConstPool::read(r);
	EXPECT_EQ(pool.tag(6), CpTag::Methodref);
	EXPECT_EQ(pool.refClass(6), "Foo");
	EXPECT_EQ(pool.refName(6), "bar");
	EXPECT_EQ(pool.refDescriptor(6), "()V");
}

TEST(ConstPool, LongDoubleTwoSlots)
{
	// Long entry occupies two slots.
	std::vector<uint8_t> raw = {0, 3}; // cp_count = 3 (long at 1, filler at 2)
	raw.push_back(5);                  // Long tag
	int64_t val = 123456789LL;
	for (int i = 7; i >= 0; --i)
		raw.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
	BinaryReader r(raw.data(), raw.size());
	auto pool = ConstPool::read(r);
	EXPECT_EQ(pool.tag(1), CpTag::Long);
	EXPECT_EQ(std::get<CpLong>(pool.entry(1)).value, 123456789LL);
}

TEST(ConstPool, InvalidIndex)
{
	std::vector<uint8_t> raw = {0, 2};
	for (uint8_t b: cpUtf8("x"))
		raw.push_back(b);
	BinaryReader r(raw.data(), raw.size());
	auto pool = ConstPool::read(r);
	EXPECT_THROW(pool.entry(0), JvmParseError);
	EXPECT_THROW(pool.entry(99), JvmParseError);
}

// ══════════════════════════════════════════════════════════════════════════════
// JvmSignatureParser
// ══════════════════════════════════════════════════════════════════════════════

TEST(JvmSigParser, BaseTypeDescriptors)
{
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("I"), Int());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("J"), Long());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("F"), Float());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("D"), Double());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("Z"), Bool());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("B"), Byte());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("C"), Char());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("S"), Short());
	EXPECT_EQ(JvmSignatureParser::parseDescriptor("V"), Void());
}

TEST(JvmSigParser, ClassDescriptor)
{
	auto t = JvmSignatureParser::parseDescriptor("Ljava/lang/String;");
	EXPECT_TRUE(t.isClass());
	EXPECT_EQ(t.ref().className, "java/lang/String");
}

TEST(JvmSigParser, ArrayDescriptor)
{
	auto t = JvmSignatureParser::parseDescriptor("[I");
	EXPECT_TRUE(t.isArray());
	EXPECT_EQ(t.ref().arrayDims, 1);
	EXPECT_EQ(*t.ref().elementType, Int());
}

TEST(JvmSigParser, MultiArrayDescriptor)
{
	auto t = JvmSignatureParser::parseDescriptor("[[Ljava/lang/String;");
	EXPECT_TRUE(t.isArray());
	EXPECT_EQ(t.ref().arrayDims, 2);
}

TEST(JvmSigParser, MethodDescriptor_VoidNoArgs)
{
	auto ft = JvmSignatureParser::parseMethodDescriptor("()V");
	EXPECT_TRUE(ft.params.empty());
	EXPECT_EQ(*ft.returnType, Void());
}

TEST(JvmSigParser, MethodDescriptor_IntStringToVoid)
{
	auto ft = JvmSignatureParser::parseMethodDescriptor("(ILjava/lang/String;)V");
	ASSERT_EQ(ft.params.size(), 2u);
	EXPECT_EQ(*ft.params[0], Int());
	EXPECT_EQ(*ft.params[1], Class("java/lang/String"));
	EXPECT_EQ(*ft.returnType, Void());
}

TEST(JvmSigParser, MethodDescriptor_ReturnArray)
{
	auto ft = JvmSignatureParser::parseMethodDescriptor("()[Ljava/lang/Object;");
	EXPECT_TRUE(ft.params.empty());
	EXPECT_TRUE(ft.returnType->isArray());
}

TEST(JvmSigParser, GenericFieldSignature)
{
	// "Ljava/util/List<Ljava/lang/String;>;"
	auto t = JvmSignatureParser::parseFieldSig("Ljava/util/List<Ljava/lang/String;>;");
	EXPECT_TRUE(t.isClass());
	// Generic instantiation
	EXPECT_EQ(t.ref().kind, BcRefKind::Generic);
	EXPECT_EQ(t.ref().typeArgs.size(), 1u);
	EXPECT_EQ(t.ref().typeArgs[0]->ref().className, "java/lang/String");
}

TEST(JvmSigParser, TypeVariableSignature)
{
	auto t = JvmSignatureParser::parseFieldSig("TT;");
	EXPECT_EQ(t.ref().kind, BcRefKind::TypeVariable);
	EXPECT_EQ(t.ref().className, "T");
}

TEST(JvmSigParser, ClassSignatureWithTypeParams)
{
	// "<T:Ljava/lang/Object;>Ljava/lang/Object;"
	auto cs = JvmSignatureParser::parseClassSig("<T:Ljava/lang/Object;>Ljava/lang/Object;");
	ASSERT_EQ(cs.typeParams.size(), 1u);
	EXPECT_EQ(cs.typeParams[0].name, "T");
}

TEST(JvmSigParser, WildcardUnbounded)
{
	auto t = JvmSignatureParser::parseFieldSig("Ljava/util/List<*>;");
	EXPECT_EQ(t.ref().kind, BcRefKind::Generic);
	ASSERT_EQ(t.ref().typeArgs.size(), 1u);
	EXPECT_EQ(t.ref().typeArgs[0]->ref().kind, BcRefKind::Wildcard);
}

TEST(JvmSigParser, WildcardBoundedAbove)
{
	// "Ljava/util/List<+Ljava/lang/Number;>;"
	auto t = JvmSignatureParser::parseFieldSig("Ljava/util/List<+Ljava/lang/Number;>;");
	EXPECT_EQ(t.ref().kind, BcRefKind::Generic);
	ASSERT_EQ(t.ref().typeArgs.size(), 1u);
	EXPECT_EQ(t.ref().typeArgs[0]->ref().kind, BcRefKind::BoundedAbove);
}

TEST(JvmSigParser, MethodSignatureWithGenerics)
{
	// "<E:Ljava/lang/Object;>(TE;)TE;"
	auto ms = JvmSignatureParser::parseMethodSig("<E:Ljava/lang/Object;>(TE;)TE;");
	ASSERT_EQ(ms.typeParams.size(), 1u);
	EXPECT_EQ(ms.typeParams[0].name, "E");
	ASSERT_EQ(ms.params.size(), 1u);
	EXPECT_EQ(ms.params[0].ref().kind, BcRefKind::TypeVariable);
	EXPECT_EQ(ms.returnType.ref().kind, BcRefKind::TypeVariable);
}

TEST(JvmSigParser, MethodSignatureThrows)
{
	// "(I)V^Ljava/io/IOException;"
	auto ms = JvmSignatureParser::parseMethodSig("(I)V^Ljava/io/IOException;");
	EXPECT_EQ(ms.params.size(), 1u);
	ASSERT_EQ(ms.throwsTypes.size(), 1u);
	EXPECT_EQ(ms.throwsTypes[0].ref().className, "java/io/IOException");
}

// ── Runaway nesting ──────────────────────────────────────────────────────────
// A signature is a constant-pool Utf8, so the file bounds its length but not
// its nesting: "L<" is two bytes and buys one more recursive descent, so a
// single constant of the size the pool already permits used to recurse tens of
// thousands of levels and blow the stack. The parser must reject the nesting
// instead of following it.

TEST(JvmSigParser, RejectsRunawayNesting)
{
	// 32767 levels of "L<…", the most a 65535-byte Utf8 constant can encode.
	std::string sig;
	for (int i = 0; i < 32767; ++i)
		sig += "L<";
	EXPECT_THROW(JvmSignatureParser::parseFieldSig(sig), JvmParseError);
	EXPECT_THROW(JvmSignatureParser::parseDescriptor(sig), JvmParseError);
}

TEST(JvmSigParser, RejectsRunawayNestingInMethodDescriptor)
{
	// Same shape reached through a method descriptor's parameter list.
	std::string desc = "(";
	for (int i = 0; i < 32767; ++i)
		desc += "L<";
	EXPECT_THROW(JvmSignatureParser::parseMethodDescriptor(desc), JvmParseError);
}

TEST(JvmSigParser, AcceptsNestingUpToTheLimit)
{
	// The cap must not cost valid input: a signature nested as deep as the
	// limit allows still parses. Depth counts every ReferenceTypeSignature,
	// so MAX_SIGNATURE_DEPTH levels of "L<" plus the innermost "Ljava/lang/
	// Object;" is exactly at the limit.
	const unsigned levels = JvmSignatureParser::MAX_SIGNATURE_DEPTH - 1;
	std::string sig;
	for (unsigned i = 0; i < levels; ++i)
		sig += "L<";
	sig += "Ljava/lang/Object;";
	for (unsigned i = 0; i < levels; ++i)
		sig += ">;";
	EXPECT_NO_THROW(JvmSignatureParser::parseFieldSig(sig));
}

// ══════════════════════════════════════════════════════════════════════════════
// ClassFile parser — hand-crafted minimal .class bytes
// ══════════════════════════════════════════════════════════════════════════════

// A minimal valid Java 8 class file for:
//   public class Hello {
//     public static void main(String[] args) { return; }
//   }
//
// Structure:
//   CAFEBABE 0000 0034                 magic + version 52.0 (Java 8)
//   0009                               cp_count = 9
//   [1] Utf8 "Hello"
//   [2] Class #1
//   [3] Utf8 "java/lang/Object"
//   [4] Class #3
//   [5] Utf8 "main"
//   [6] Utf8 "([Ljava/lang/String;)V"
//   [7] Utf8 "Code"
//   [8] Utf8 "args"
//   0021                               public + super
//   0002                               this = #2
//   0004                               super = #4
//   0000                               0 interfaces
//   0000                               0 fields
//   0001                               1 method
//     0009 0005 0006 0001              public static main([Ljava/lang/String;)V, 1 attr
//       0007 00000011                  attr name=#7 (Code), len=17
//         0001 0001                    maxStack=1 maxLocals=1
//         00000001                     codeLen=1
//           B1                        return
//         0000                        0 exceptions
//         0000                        0 code attributes
//   0000                               0 class attributes

static std::vector<uint8_t> makeHelloWorldClass()
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};

	push4(0xCAFEBABE);
	push2(0);  // minor
	push2(52); // major = Java 8
	push2(9);  // cp_count

	pushUtf8("Hello"); // #1
	raw.push_back(7);
	push2(1);                     // #2 Class(#1)
	pushUtf8("java/lang/Object"); // #3
	raw.push_back(7);
	push2(3);                           // #4 Class(#3)
	pushUtf8("main");                   // #5
	pushUtf8("([Ljava/lang/String;)V"); // #6
	pushUtf8("Code");                   // #7
	pushUtf8("args");                   // #8

	push2(0x0021); // public + super
	push2(2);      // this = #2
	push2(4);      // super = #4
	push2(0);      // 0 interfaces
	push2(0);      // 0 fields
	push2(1);      // 1 method
	// Method: public static void main(String[] args)
	push2(0x0009); // public + static
	push2(5);      // name = "main"
	push2(6);      // descriptor = "([Ljava/lang/String;)V"
	push2(1);      // 1 attribute
	// Code attribute
	push2(7);            // attr name = "Code"
	push4(13);           // attribute length = 13 (2+2+4+1+2+2 bytes)
	push2(1);            // maxStack = 1
	push2(1);            // maxLocals = 1
	push4(1);            // code_length = 1
	raw.push_back(0xB1); // return
	push2(0);            // 0 exception entries
	push2(0);            // 0 code attributes
	push2(0);            // 0 class attributes
	return raw;
}

TEST(ClassFileParser, HelloWorldMagic)
{
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	EXPECT_TRUE(res.ok) << res.error;
	EXPECT_EQ(res.majorVersion, 52);
	EXPECT_EQ(res.minorVersion, 0);
}

TEST(ClassFileParser, HelloWorldClassName)
{
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	EXPECT_EQ(res.cls.name, "Hello");
	EXPECT_EQ(res.cls.fqName, "Hello");
}

TEST(ClassFileParser, HelloWorldSuperClass)
{
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	ASSERT_TRUE(res.cls.superClass.has_value());
	EXPECT_EQ(res.cls.superClass->ref().className, "java/lang/Object");
}

TEST(ClassFileParser, HelloWorldMethod)
{
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	ASSERT_EQ(res.cls.methods.size(), 1u);
	EXPECT_EQ(res.cls.methods[0].name, "main");
	EXPECT_FALSE(res.cls.methods[0].isConstructor);
}

TEST(ClassFileParser, HelloWorldMethodHasCFG)
{
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	ASSERT_EQ(res.cls.methods.size(), 1u);
	// Should have at least 1 block (the return block).
	EXPECT_GE(res.cls.methods[0].cfg.blockCount(), 1u);
}

TEST(ClassFileParser, HelloWorldAccessFlags)
{
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	EXPECT_TRUE(hasFlag(res.cls.access, BcAccess::Public));
	EXPECT_FALSE(res.cls.isInterface);
}

static std::vector<uint8_t> makeClassWithSourceAndSignature()
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};

	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(9);        // cp_count
	pushUtf8("Box"); // #1
	raw.push_back(7);
	push2(1);                     // #2 Class(#1)
	pushUtf8("java/lang/Object"); // #3
	raw.push_back(7);
	push2(3);                       // #4 Class(#3)
	pushUtf8("SourceFile");         // #5
	pushUtf8("Box.java");           // #6
	pushUtf8("Signature");          // #7
	pushUtf8("Ljava/lang/Object;"); // #8
	push2(0x0021);                  // public + super
	push2(2);
	push2(4);
	push2(0); // interfaces
	push2(0); // fields
	push2(0); // methods
	push2(2); // class attributes
	push2(5);
	push4(2);
	push2(6); // SourceFile → #6
	push2(7);
	push4(2);
	push2(8); // Signature → #8
	return raw;
}

static std::vector<uint8_t> makeClassWithFieldSignature()
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};

	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(9);
	pushUtf8("Box"); // #1
	raw.push_back(7);
	push2(1);                     // #2 Class(#1)
	pushUtf8("java/lang/Object"); // #3
	raw.push_back(7);
	push2(3);                          // #4 Class(#3)
	pushUtf8("items");                 // #5
	pushUtf8("Ljava/util/List;");      // #6
	pushUtf8("Signature");             // #7
	pushUtf8("Ljava/util/List<TE;>;"); // #8
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(1);      // 1 field
	push2(0x0001); // public
	push2(5);
	push2(6);
	push2(1); // 1 attr
	push2(7);
	push4(2);
	push2(8); // Signature
	push2(0); // methods
	push2(0); // class attrs
	return raw;
}

TEST(ClassFileParser, ParsesSourceFileAndClassSignature)
{
	auto cls = makeClassWithSourceAndSignature();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_EQ(res.cls.sourceFile, "Box.java");
	EXPECT_EQ(res.cls.signature, "Ljava/lang/Object;");
}

TEST(ClassFileParser, ParsesFieldSignature)
{
	auto cls = makeClassWithFieldSignature();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cls.fields.size(), 1u);
	EXPECT_EQ(res.cls.fields[0].name, "items");
	EXPECT_EQ(res.cls.fields[0].signature, "Ljava/util/List<TE;>;");
	EXPECT_EQ(res.cls.fields[0].type.ref().kind, BcRefKind::Generic);
}

static std::vector<uint8_t> makeClassWithThrows()
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};

	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(11);       // cp_count
	pushUtf8("Box"); // #1
	raw.push_back(7);
	push2(1);                     // #2 Class(#1)
	pushUtf8("java/lang/Object"); // #3
	raw.push_back(7);
	push2(3);                        // #4 Class(#3)
	pushUtf8("foo");                 // #5
	pushUtf8("()V");                 // #6
	pushUtf8("Code");                // #7
	pushUtf8("Exceptions");          // #8
	pushUtf8("java/io/IOException"); // #9
	raw.push_back(7);
	push2(9); // #10 Class(#9)
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(1);      // 1 method
	push2(0x0001); // public
	push2(5);
	push2(6);
	push2(2); // Code + Exceptions
	push2(7);
	push4(13);
	push2(0);
	push2(0);
	push4(1);
	raw.push_back(0xB1);
	push2(0);
	push2(0);
	push2(8);
	push4(4);
	push2(1);
	push2(10);
	push2(0);
	return raw;
}

static std::vector<uint8_t> makeClassWithConstantInt()
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};

	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(9);
	pushUtf8("Box"); // #1
	raw.push_back(7);
	push2(1);                     // #2 Class(#1)
	pushUtf8("java/lang/Object"); // #3
	raw.push_back(7);
	push2(3);                  // #4 Class(#3)
	pushUtf8("MAX");           // #5
	pushUtf8("I");             // #6
	pushUtf8("ConstantValue"); // #7
	raw.push_back(3);
	push4(42); // #8 Integer 42
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(1);      // 1 field
	push2(0x0019); // public static final
	push2(5);
	push2(6);
	push2(1);
	push2(7);
	push4(2);
	push2(8);
	push2(0);
	push2(0);
	return raw;
}

TEST(ClassFileParser, ParsesMethodExceptions)
{
	auto cls = makeClassWithThrows();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cls.methods.size(), 1u);
	ASSERT_EQ(res.cls.methods[0].throwsList.size(), 1u);
	EXPECT_EQ(res.cls.methods[0].throwsList[0], "java/io/IOException");
}

TEST(ClassFileParser, ParsesFieldConstantValue)
{
	auto cls = makeClassWithConstantInt();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cls.fields.size(), 1u);
	ASSERT_TRUE(res.cls.fields[0].constantIntValue.has_value());
	EXPECT_EQ(*res.cls.fields[0].constantIntValue, 42);
}

TEST(ClassFileParser, ParsesAbstractNativeFlags)
{
	// Reuse Hello World (public static, not abstract/native).
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	ASSERT_EQ(res.cls.methods.size(), 1u);
	EXPECT_FALSE(res.cls.methods[0].isAbstract);
	EXPECT_FALSE(res.cls.methods[0].isNative);
}

TEST(ClassFileParser, ParsesAbstractMethod)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(7);
	pushUtf8("Box");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Object");
	raw.push_back(7);
	push2(3);
	pushUtf8("foo");
	pushUtf8("()V");
	push2(0x0421); // public super abstract class
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(1);
	push2(0x0401); // public abstract method
	push2(5);
	push2(6);
	push2(0); // no method attrs
	push2(0);
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_TRUE(res.cls.isAbstract);
	ASSERT_EQ(res.cls.methods.size(), 1u);
	EXPECT_TRUE(res.cls.methods[0].isAbstract);
	EXPECT_FALSE(res.cls.methods[0].isNative);
}

TEST(ClassFileParser, ParsesModuleFlag)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(53);
	push2(5);
	pushUtf8("module-info");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Object");
	raw.push_back(7);
	push2(3);
	push2(0x8000); // ACC_MODULE
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(0);
	push2(0);
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_TRUE(res.cls.isModule);
}

TEST(ClassFileParser, ParsesNestHostAndMembers)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(55);
	push2(8);
	pushUtf8("Inner");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Object");
	raw.push_back(7);
	push2(3);
	pushUtf8("Outer");
	raw.push_back(7);
	push2(5);
	pushUtf8("NestHost");
	push2(0x0021); // public super
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(0);
	push2(1);
	push2(7);
	push4(2);
	push2(6); // NestHost, host = Outer
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_EQ("Outer", res.cls.nestHost);
}

TEST(ClassFileParser, ParsesPermittedSubclasses)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(61);
	push2(8);
	pushUtf8("Shape");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Object");
	raw.push_back(7);
	push2(3);
	pushUtf8("Circle");
	raw.push_back(7);
	push2(5);
	pushUtf8("PermittedSubclasses");
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(0);
	push2(1);
	push2(7);
	push4(4);
	push2(1);
	push2(6); // 1 permitted = Circle
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(1u, res.cls.permittedSubclasses.size());
	EXPECT_EQ("Circle", res.cls.permittedSubclasses[0]);
	EXPECT_TRUE(hasFlag(res.cls.access, BcAccess::Sealed));
}

TEST(ClassFileParser, ParsesAnnotationDefault)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(9);
	pushUtf8("Ann");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Object");
	raw.push_back(7);
	push2(3);
	pushUtf8("value");
	pushUtf8("()I");
	pushUtf8("AnnotationDefault");
	raw.push_back(3);
	push4(42);     // Integer 42 at cp 8
	push2(0x2001); // public annotation
	push2(2);
	push2(4);
	push2(0);      // interfaces
	push2(0);      // fields
	push2(1);      // methods
	push2(0x0401); // public abstract
	push2(5);
	push2(6);
	push2(1);
	push2(7);
	push4(3);
	raw.push_back('I');
	push2(8);
	push2(0);
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(1u, res.cls.methods.size());
	bool found = false;
	for (const auto& a: res.cls.methods[0].annotations)
	{
		if (a.typeName != "AnnotationDefault") continue;
		auto it = a.elements.find("value");
		ASSERT_NE(it, a.elements.end());
		EXPECT_EQ(BcAnnotationValue::Kind::Int, it->second.kind);
		EXPECT_EQ(42, it->second.intValue);
		found = true;
	}
	EXPECT_TRUE(found);
}

TEST(ClassFileParser, ParsesClassTypeParams)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	const std::string sig = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(7);
	pushUtf8("Box");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Object");
	raw.push_back(7);
	push2(3);
	pushUtf8("Signature");
	pushUtf8(sig);
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(0);
	push2(1);
	push2(5);
	push4(2);
	push2(6);
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cls.typeParams.size(), 1u);
	EXPECT_EQ(res.cls.typeParams[0], "T");
}

TEST(ClassFileParser, ParsesEmptyRecordAttribute)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(61);
	push2(6); // Java 17
	pushUtf8("Point");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Record");
	raw.push_back(7);
	push2(3);
	pushUtf8("Record");
	push2(0x0031); // public final super
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(0);
	push2(1);
	push2(5);
	push4(2);
	push2(0); // Record, 0 components
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_TRUE(res.cls.isRecord);
}

TEST(ClassFileParser, ParsesEnumConstantField)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(7);
	pushUtf8("Color");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Enum");
	raw.push_back(7);
	push2(3);
	pushUtf8("RED");
	pushUtf8("LColor;");
	push2(0x4021); // public super enum
	push2(2);
	push2(4);
	push2(0);
	push2(1);
	push2(0x4019); // public static final enum
	push2(5);
	push2(6);
	push2(0);
	push2(0);
	push2(0);
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_TRUE(res.cls.isEnum);
	ASSERT_EQ(res.cls.enumConstants.size(), 1u);
	EXPECT_EQ(res.cls.enumConstants[0], "RED");
}

TEST(ClassFileParser, ParsesRuntimeVisibleAnnotation)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(7);        // cp_count = last index + 1
	pushUtf8("Box"); // #1
	raw.push_back(7);
	push2(1);                     // #2 Class(#1)
	pushUtf8("java/lang/Object"); // #3
	raw.push_back(7);
	push2(3);                              // #4 Class(#3)
	pushUtf8("RuntimeVisibleAnnotations"); // #5
	pushUtf8("Ljava/lang/Deprecated;");    // #6
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(0);
	push2(1); // 1 class attr
	push2(5);
	push4(6);
	push2(1);
	push2(6);
	push2(0);
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cls.annotations.size(), 1u);
	EXPECT_EQ(res.cls.annotations[0].typeName, "java/lang/Deprecated");
	EXPECT_TRUE(res.cls.annotations[0].isVisible);
}

TEST(ClassFileParser, ParsesDeprecatedAttribute)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(6);
	pushUtf8("Box");
	raw.push_back(7);
	push2(1);
	pushUtf8("java/lang/Object");
	raw.push_back(7);
	push2(3);
	pushUtf8("Deprecated");
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(0);
	push2(1);
	push2(5);
	push4(0);
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cls.annotations.size(), 1u);
	EXPECT_EQ(res.cls.annotations[0].typeName, "java/lang/Deprecated");
}

TEST(ClassFileParser, ParsesParameterAnnotations)
{
	std::vector<uint8_t> raw;
	auto push4 = [&](uint32_t v) {
		raw.push_back((v >> 24) & 0xFF);
		raw.push_back((v >> 16) & 0xFF);
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto push2 = [&](uint16_t v) {
		raw.push_back((v >> 8) & 0xFF);
		raw.push_back(v & 0xFF);
	};
	auto pushUtf8 = [&](const std::string& s) {
		raw.push_back(1);
		push2(static_cast<uint16_t>(s.size()));
		for (char c: s)
			raw.push_back(static_cast<uint8_t>(c));
	};
	push4(0xCAFEBABE);
	push2(0);
	push2(52);
	push2(9);
	pushUtf8("Box"); // #1
	raw.push_back(7);
	push2(1);                     // #2
	pushUtf8("java/lang/Object"); // #3
	raw.push_back(7);
	push2(3);                                       // #4
	pushUtf8("foo");                                // #5
	pushUtf8("(I)V");                               // #6
	pushUtf8("RuntimeVisibleParameterAnnotations"); // #7
	pushUtf8("Ljava/lang/Deprecated;");             // #8
	push2(0x0021);
	push2(2);
	push2(4);
	push2(0);
	push2(0);
	push2(1);      // 1 method
	push2(0x0401); // public abstract
	push2(5);
	push2(6);
	push2(1); // 1 attr
	push2(7);
	push4(7);
	raw.push_back(1); // 1 parameter
	push2(1);
	push2(8);
	push2(0);
	push2(0); // class attrs
	auto res = parseClassFile(raw);
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cls.methods.size(), 1u);
	ASSERT_EQ(res.cls.methods[0].paramAnnotations.size(), 1u);
	ASSERT_EQ(res.cls.methods[0].paramAnnotations[0].size(), 1u);
	EXPECT_EQ(res.cls.methods[0].paramAnnotations[0][0].typeName, "java/lang/Deprecated");
}

TEST(ClassFileParser, InvalidMagic)
{
	uint8_t bad[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x34};
	auto res = parseClassFile(bad, sizeof(bad));
	EXPECT_FALSE(res.ok);
	EXPECT_FALSE(res.error.empty());
}

TEST(ClassFileParser, EmptyBuffer)
{
	auto res = parseClassFile(nullptr, 0);
	EXPECT_FALSE(res.ok);
}

TEST(ClassFileParser, JavaVersionString)
{
	EXPECT_EQ(javaRelease(52), 8);
	EXPECT_EQ(javaRelease(55), 11);
	EXPECT_EQ(javaRelease(61), 17);
	EXPECT_EQ(javaRelease(65), 21);
	auto s = javaVersionString(52, 0);
	EXPECT_NE(s.find("Java 8"), std::string::npos);
}

// ══════════════════════════════════════════════════════════════════════════════
// JvmLifter
// ══════════════════════════════════════════════════════════════════════════════

static ConstPool makeEmptyPool()
{
	std::vector<uint8_t> raw = {0, 1}; // cp_count = 1 (empty pool)
	BinaryReader r(raw.data(), raw.size());
	return ConstPool::read(r);
}

TEST(JvmLifter, SingleReturnBlock)
{
	// Bytecode: return (0xB1)
	CodeAttr code;
	code.bytecode = {0xB1};
	code.maxStack = 0;
	code.maxLocals = 0;
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok) << res.error;
	EXPECT_GE(res.cfg.blockCount(), 1u);
	EXPECT_FALSE(res.cfg.block(0).instrs.empty());
	EXPECT_EQ(res.cfg.block(0).instrs.back().opcode, BcOpcode::Return);
}

TEST(JvmLifter, PushIntAndReturn)
{
	// iconst_5 (0x08) then ireturn (0xAC)
	CodeAttr code;
	code.bytecode = {0x08, 0xAC};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()I");
	EXPECT_TRUE(res.ok);
	ASSERT_GE(res.cfg.blockCount(), 1u);
	const auto& instrs = res.cfg.block(0).instrs;
	ASSERT_GE(instrs.size(), 2u);
	EXPECT_EQ(instrs[0].opcode, BcOpcode::PushInt);
	EXPECT_EQ(instrs[0].intOp(), 5);
	EXPECT_EQ(instrs[1].opcode, BcOpcode::ReturnValue);
}

TEST(JvmLifter, ConditionalBranch)
{
	// ifeq +3 (0x99 0x00 0x03) then return (0xB1) then return (0xB1)
	// Creates two blocks.
	CodeAttr code;
	code.bytecode = {
		0x99,
		0x00,
		0x05, // ifeq → PC 5 (target)
		0xB1, // return (fall-through block, PC 3)
		0x00, // nop (PC 4, never reached but valid)
		0xB1  // return (target block, PC 5)
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok);
	// At least entry block + one successor.
	EXPECT_GE(res.cfg.blockCount(), 2u);
}

TEST(JvmLifter, ExceptionHandler)
{
	// try { NOP NOP NOP } catch-all { return }
	// Three NOPs span PC 0-2, return at PC 3, handler return at PC 4.
	CodeAttr code;
	code.bytecode = {
		0x00,
		0x00,
		0x00, // NOP, NOP, NOP (PC 0-2)
		0xB1, // return (PC 3)
		0xB1  // return (handler, PC 4)
	};
	ExceptionEntry eh;
	eh.startPc = 0;
	eh.endPc = 3;
	eh.handlerPc = 4;
	eh.catchType = 0; // catch-all / finally
	code.exceptionTable.push_back(eh);

	auto pool = makeEmptyPool();

	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok);
	EXPECT_FALSE(res.cfg.handlers().empty());
	EXPECT_TRUE(res.cfg.handlers()[0].isFinally);
}

TEST(JvmLifter, TableSwitch)
{
	// tableswitch: lo=0, hi=1, default→+16, case0→+4, case1→+8
	// Padding to align at 4-byte boundary (1 byte opcode, then 3 pad bytes)
	CodeAttr code;
	code.bytecode = {
		0xAA, // tableswitch (PC 0)
		0x00,
		0x00,
		0x00, // padding (3 bytes for alignment at PC 4)
		0x00,
		0x00,
		0x00,
		0x10, // default = +16 (relative to PC 0) → 16
		0x00,
		0x00,
		0x00,
		0x00, // lo = 0
		0x00,
		0x00,
		0x00,
		0x01, // hi = 1
		0x00,
		0x00,
		0x00,
		0x04, // case 0 → +4 → PC 4
		0x00,
		0x00,
		0x00,
		0x08, // case 1 → +8 → PC 8
		// padding targets (don't matter for this test)
		0xB1,
		0x00,
		0x00,
		0x00, // PC 4: return + padding
		0xB1,
		0x00,
		0x00,
		0x00, // PC 8: return + padding
		0xB1  // PC 16: return (default)
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok);
	EXPECT_GE(res.cfg.blockCount(), 3u);
}

TEST(JvmLifter, WideInstruction)
{
	// wide aload 256
	CodeAttr code;
	code.bytecode = {
		0xC4,
		0x19, // wide aload
		0x01,
		0x00, // local index 256 big-endian
		0xB1  // return
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok);
}

// ── Malformed code[] ─────────────────────────────────────────────────────────
// code[] is copied verbatim out of an attacker-controlled class file, so an
// instruction's operands and a switch's jump table need not be inside it.
// These feed bodies that stop mid-instruction or declare a jump table far
// larger than the array can hold; the lifter must stay inside code[].

// Returns the first switch table decoded anywhere in the CFG, or nullptr.
static const BcSwitchTable* firstSwitchTable(const BcCFG& cfg)
{
	for (uint32_t b = 0; b < cfg.blockCount(); ++b)
		for (const auto& in: cfg.block(b).instrs)
			if (in.opcode == BcOpcode::TableSwitch || in.opcode == BcOpcode::LookupSwitch)
				return &std::get<BcSwitchTable>(in.operands[0]);
	return nullptr;
}

TEST(JvmLifter, TableSwitchTableRunsPastEndOfCode)
{
	// hi - lo + 1 claims 0x40000001 cases while only two 4-byte entries are
	// present. Each case costs 4 bytes, so at most two can exist here.
	CodeAttr code;
	code.bytecode = {
		0xAA,                   // tableswitch (PC 0)
		0x00, 0x00, 0x00,       // padding
		0x00, 0x00, 0x00, 0x10, // default = +16
		0x00, 0x00, 0x00, 0x00, // lo = 0
		0x40, 0x00, 0x00, 0x00, // hi = 0x40000000
		0x00, 0x00, 0x00, 0x04, // case 0 → +4
		0x00, 0x00, 0x00, 0x08  // case 1 → +8   (code[] ends here)
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok) << res.error;
	const BcSwitchTable* sw = firstSwitchTable(res.cfg);
	ASSERT_NE(sw, nullptr);
	EXPECT_EQ(sw->cases.size(), 2u);
}

TEST(JvmLifter, TableSwitchHeaderRunsPastEndOfCode)
{
	// The 12-byte default/lo/hi header itself is truncated.
	CodeAttr code;
	code.bytecode = {
		0xAA, // tableswitch (PC 0)
		0x00,
		0x00,
		0x00, // padding
		0x00,
		0x00,
		0x00,
		0x10, // default = +16
		0x00,
		0x00 // lo, truncated
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok) << res.error;
	const BcSwitchTable* sw = firstSwitchTable(res.cfg);
	ASSERT_NE(sw, nullptr);
	EXPECT_TRUE(sw->cases.empty());
}

TEST(JvmLifter, LookupSwitchPairCountRunsPastEndOfCode)
{
	// npairs claims INT32_MAX pairs with a single 8-byte pair present.
	CodeAttr code;
	code.bytecode = {
		0xAB,                   // lookupswitch (PC 0)
		0x00, 0x00, 0x00,       // padding
		0x00, 0x00, 0x00, 0x0C, // default = +12
		0x7F, 0xFF, 0xFF, 0xFF, // npairs = 2147483647
		0x00, 0x00, 0x00, 0x01, // key = 1
		0x00, 0x00, 0x00, 0x0C  // → +12          (code[] ends here)
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok) << res.error;
	const BcSwitchTable* sw = firstSwitchTable(res.cfg);
	ASSERT_NE(sw, nullptr);
	EXPECT_EQ(sw->cases.size(), 1u);
	EXPECT_EQ(sw->cases[0].first, 1);
}

TEST(JvmLifter, LookupSwitchNegativePairCount)
{
	// A negative npairs used to make the decode loop a no-op only by accident;
	// it must not be sign-extended into a huge unsigned count either.
	CodeAttr code;
	code.bytecode = {
		0xAB, // lookupswitch (PC 0)
		0x00,
		0x00,
		0x00, // padding
		0x00,
		0x00,
		0x00,
		0x08, // default = +8
		0xFF,
		0xFF,
		0xFF,
		0xFF, // npairs = -1
		0xB1  // return (PC 12)
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok) << res.error;
	const BcSwitchTable* sw = firstSwitchTable(res.cfg);
	ASSERT_NE(sw, nullptr);
	EXPECT_TRUE(sw->cases.empty());
}

TEST(JvmLifter, OperandTruncatedAtEndOfCode)
{
	// getstatic is the last byte of code[]: its u2 constant-pool index is not
	// in the array at all. The index cannot resolve against the empty pool, so
	// the lift reports an error — the point is that it stays inside code[].
	CodeAttr code;
	code.bytecode = {0x00, 0xB2}; // nop, getstatic <operand missing>
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_FALSE(res.ok);
}

TEST(JvmLifter, InvokeVirtualIsTheWholeCodeArray)
{
	// code_length = 1 and the only byte is invokevirtual, whose u2 constant
	// pool index therefore lies entirely outside code[]. The decoder must not
	// read it. The index cannot resolve either, so the lift reports an error.
	CodeAttr code;
	code.bytecode = {0xB6};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_FALSE(res.ok);
}

TEST(JvmLifter, BranchOperandTruncatedAtEndOfCode)
{
	// goto with only one of its two offset bytes present.
	CodeAttr code;
	code.bytecode = {0x00, 0xA7, 0x00}; // nop, goto <offset truncated>
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok) << res.error;
}

TEST(JvmLifter, WideOpcodeAtEndOfCode)
{
	// The wide prefix is the last byte, so the opcode it modifies is missing.
	CodeAttr code;
	code.bytecode = {0x00, 0xC4}; // nop, wide <modified opcode missing>
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool);
	auto res = lifter.lift(code, "()V");
	EXPECT_TRUE(res.ok) << res.error;
}

// ══════════════════════════════════════════════════════════════════════════════
// JarReader
// ══════════════════════════════════════════════════════════════════════════════

TEST(JarReader, EmptyBuffer)
{
	JarReader reader;
	auto res = reader.read(nullptr, 0);
	EXPECT_FALSE(res.ok);
}

TEST(JarReader, InvalidZip)
{
	uint8_t bad[] = {0x00, 0x01, 0x02, 0x03};
	JarReader reader;
	auto res = reader.read(bad, sizeof(bad));
	EXPECT_FALSE(res.ok);
}

TEST(JarReader, ListEntriesEmpty)
{
	uint8_t bad[] = {0x00};
	JarReader reader;
	auto entries = reader.listEntries(bad, sizeof(bad));
	EXPECT_TRUE(entries.empty());
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

// Minimal STORED ZIP whose central-directory uncompressed size exceeds remaining
// payload bytes (zip-bomb style). Must not allocate the claimed size.
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

TEST(JarReader, StoredClaimExceedsRemaining)
{
	auto zip = storedZipClaimedUncomp("Foo.class", {0x00, 0x01, 0x02, 0x03}, 0xFFFFFFF0u);
	JarReader reader;
	auto res = reader.read(zip.data(), zip.size());
	EXPECT_TRUE(res.ok);
	EXPECT_GE(res.parseErrors, 1u);
}

TEST(JarReader, ParsesBootInfLibJar)
{
	auto inner = storedZip({{"Hello.class", makeHelloWorldClass()}});
	auto outer = storedZip({{"BOOT-INF/lib/dep.jar", inner}});
	JarReader reader; // parseBoot defaults true
	auto res = reader.read(outer.data(), outer.size());
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_GE(res.classesParsed, 1u);
	ASSERT_NE(res.module.findClass("Hello"), nullptr);
}

TEST(JarReader, LooseLibJarSkippedUnlessParseNested)
{
	auto inner = storedZip({{"Hello.class", makeHelloWorldClass()}});
	auto outer = storedZip({{"lib/dep.jar", inner}});
	JarReadOptions opts;
	opts.parseBoot = true;
	opts.parseNestedJars = false;
	JarReader reader(opts);
	auto res = reader.read(outer.data(), outer.size());
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_EQ(res.classesParsed, 0u);
	EXPECT_EQ(res.module.findClass("Hello"), nullptr);
}

TEST(JarReader, ParseNestedJarsAnyPath)
{
	auto inner = storedZip({{"Hello.class", makeHelloWorldClass()}});
	auto outer = storedZip({{"lib/dep.jar", inner}});
	JarReadOptions opts;
	opts.parseNestedJars = true;
	JarReader reader(opts);
	auto res = reader.read(outer.data(), outer.size());
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_GE(res.classesParsed, 1u);
	ASSERT_NE(res.module.findClass("Hello"), nullptr);
}

// ══════════════════════════════════════════════════════════════════════════════
// Attribute parsers — directly
// ══════════════════════════════════════════════════════════════════════════════

TEST(AttributeParser, ParseCodeAttr)
{
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	ASSERT_EQ(res.cls.methods.size(), 1u);
	EXPECT_EQ(res.cls.methods[0].maxStack, 1);
	EXPECT_EQ(res.cls.methods[0].maxLocals, 1);
}

TEST(AttributeParser, ParseBootstrapMethodsAttr)
{
	// Build a minimal class with a BootstrapMethods attribute.
	// [1=Utf8("Test"), 2=Class#1, 3=Utf8("java/lang/Object"), 4=Class#3,
	//  5=Utf8("BootstrapMethods"), 6=MethodHandle, 7=Utf8("()V")]
	// We'll skip full binary construction and just verify the parser
	// handles the Hello World class (which has no BSM) gracefully.
	auto cls = makeHelloWorldClass();
	auto res = parseClassFile(cls);
	ASSERT_TRUE(res.ok);
	// No BootstrapMethods expected.
	EXPECT_EQ(res.bootstrap.methods.size(), 0u);
}

// ── Runaway annotation nesting ───────────────────────────────────────────────
// An element_value can be an array of element_values, and nothing in the file
// bounds that nesting: a '[' tag plus its u2 count is three bytes, so a modest
// RuntimeVisibleAnnotations blob used to drive hundreds of thousands of
// recursive calls and exhaust the stack. getAnnotations() swallows parse
// errors, so the visible symptom of the fix is that it returns instead of
// crashing.

TEST(AttributeParser, RejectsRunawayAnnotationNesting)
{
	// cp: [1=Utf8("x"), 2=Integer(42)]
	std::vector<uint8_t> raw = {0, 3};
	for (uint8_t b: cpUtf8("x"))
		raw.push_back(b);
	for (uint8_t b: cpInt(42))
		raw.push_back(b);
	BinaryReader pr(raw.data(), raw.size());
	auto pool = ConstPool::read(pr);

	// num_annotations=1, type_index=1, num_pairs=1, name_index=1,
	// then 200000 nested one-element arrays, then an 'I' leaf.
	std::vector<uint8_t> blob = {0, 1, 0, 1, 0, 1, 0, 1};
	for (int i = 0; i < 200000; ++i)
	{
		blob.push_back('[');
		blob.push_back(0);
		blob.push_back(1);
	}
	blob.push_back('I');
	blob.push_back(0);
	blob.push_back(2);

	std::vector<ParsedAttr> attrs;
	attrs.push_back(RawAttr{"RuntimeVisibleAnnotations", blob});
	auto anns = getAnnotations(attrs, pool);
	EXPECT_TRUE(anns.empty());
}

TEST(AttributeParser, AcceptsAnnotationNestingUpToTheLimit)
{
	// The cap must not cost valid input: nesting inside the limit still
	// decodes, so the annotation comes back rather than being dropped.
	// cp: [1=Utf8("x"), 2=Integer(42)]
	std::vector<uint8_t> raw = {0, 3};
	for (uint8_t b: cpUtf8("x"))
		raw.push_back(b);
	for (uint8_t b: cpInt(42))
		raw.push_back(b);
	BinaryReader pr(raw.data(), raw.size());
	auto pool = ConstPool::read(pr);

	// MAX_ANNOTATION_DEPTH nested arrays put the 'I' leaf exactly at the cap.
	std::vector<uint8_t> blob = {0, 1, 0, 1, 0, 1, 0, 1};
	for (unsigned i = 0; i < MAX_ANNOTATION_DEPTH; ++i)
	{
		blob.push_back('[');
		blob.push_back(0);
		blob.push_back(1);
	}
	blob.push_back('I');
	blob.push_back(0);
	blob.push_back(2);

	std::vector<ParsedAttr> attrs;
	attrs.push_back(RawAttr{"RuntimeVisibleAnnotations", blob});
	auto anns = getAnnotations(attrs, pool);
	ASSERT_EQ(anns.size(), 1u);
	EXPECT_EQ(anns[0].typeName, "x");
}

// ══════════════════════════════════════════════════════════════════════════════
// Regressions for the SMT-confirmed defects
// ══════════════════════════════════════════════════════════════════════════════

// ── BinaryReader::check formed pos_ + n ──────────────────────────────────────
// ESBMC refuted `!(pos + n > size) == (n <= size - pos)` under pos <= size.
// Reduced: pos = 4, size = 8, n = SIZE_MAX - 3 makes pos_ + n exactly 0, so the
// old `pos_ + n > size_` was false and check() accepted a read of 2^64-4 bytes
// out of an eight-byte buffer. skip() is the one caller that then does nothing
// but advance, so it shows the acceptance without also dereferencing.

TEST(BinaryReader, CheckRefusesALengthThatWrapsThePosition)
{
	uint8_t data[8] = {0};
	BinaryReader r(data, sizeof(data));
	r.skip(4);
	ASSERT_EQ(r.pos(), 4u);
	// 4 + (SIZE_MAX - 3) == 0 in size_t arithmetic.
	EXPECT_THROW(r.skip(SIZE_MAX - 3), JvmParseError);
	// And the reader is still where it was, not wrapped round to 0.
	EXPECT_EQ(r.pos(), 4u);
}

TEST(BinaryReader, CheckStillAcceptsAReadThatExactlyFits)
{
	// The fix must not cost the boundary case: n == size_ - pos_ is legal.
	uint8_t data[8] = {0};
	BinaryReader r(data, sizeof(data));
	r.skip(4);
	EXPECT_NO_THROW(r.skip(4));
	EXPECT_EQ(r.pos(), 8u);
	EXPECT_THROW(r.skip(1), JvmParseError);
}

// ── BinaryReader::mutf8 passed surrogate pairs through ───────────────────────
// MUTF-8 (JVMS 4.4.7) writes a supplementary character as its two UTF-16
// surrogates, each as its own three-byte sequence. The old loop folded C0 80
// and copied everything else byte for byte, so U+1F600 left the parser as
// ED A0 BD ED B8 80 -- six bytes that are not UTF-8 at all, since D83D and
// DE00 are not scalar values.

TEST(BinaryReader, Mutf8CombinesASurrogatePairIntoOneCharacter)
{
	// U+1F600 GRINNING FACE: UTF-16 D83D DE00, MUTF-8 ED A0 BD ED B8 80.
	uint8_t data[] = {0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80};
	BinaryReader r(data, sizeof(data));
	const std::string s = r.mutf8(sizeof(data));
	EXPECT_EQ(s, std::string("\xF0\x9F\x98\x80"));
	EXPECT_EQ(s.size(), 4u);
	EXPECT_EQ(r.pos(), sizeof(data));
}

TEST(BinaryReader, Mutf8ReplacesALoneSurrogate)
{
	// A high surrogate with nothing after it denotes no character; it becomes
	// U+FFFD rather than an ED A0..BF sequence no UTF-8 decoder accepts.
	uint8_t data[] = {0xED, 0xA0, 0xBD};
	BinaryReader r(data, sizeof(data));
	EXPECT_EQ(r.mutf8(sizeof(data)), std::string("\xEF\xBF\xBD"));
}

TEST(BinaryReader, Mutf8StillFoldsTheNulEncodingAndPlainAscii)
{
	// The one thing the old loop got right must survive the rewrite.
	uint8_t data[] = {0xC0, 0x80, 'A', 'B'};
	BinaryReader r(data, sizeof(data));
	const std::string s = r.mutf8(sizeof(data));
	ASSERT_EQ(s.size(), 3u);
	EXPECT_EQ(s, std::string("\0AB", 3));
}

// ── instrSize disagreed with JVMS 6.5 in 23 places ───────────────────────────
// findLeaders() walks code[] by instrSize(op). A wrong size parks the cursor on
// an operand byte, which is then decoded as an opcode, and every leader found
// from there is wrong. Each case below is a code array whose leader set differs
// between the JVMS size and the size the table used to claim, so the block
// count is the observable.
//
// The three remaining wrong entries -- 0x99 ifeq, 0xBF athrow and 0xC9 jsr_w --
// are not observable here: findLeaders assigns sz explicitly in the branch,
// throw and wide-goto arms before it ever consults the table, so those entries
// were dead. They are corrected for the table's own sake.

namespace {

struct LeaderCase
{
	const char* what;
	std::vector<uint8_t> code;
	uint32_t expectedBlocks;
};

// Leaders only; the stack annotator and the ldc resolver would object to these
// deliberately meaningless instruction sequences and are not under test.
LiftOptions leadersOnly()
{
	LiftOptions o;
	o.annotateStack = false;
	o.mapLineNumbers = false;
	o.resolveLdc = false;
	return o;
}

} // namespace

TEST(JvmLifter, InstructionSizesFollowJvms_NoPoolNeeded)
{
	std::vector<LeaderCase> cases;

	// 0x2E-0x34 iaload..caload: the table said 2, JVMS 6.5 says 1 (no operand).
	// [op, return, return]: with the true size the first return is at pc 1 and
	// makes pc 2 a leader; with the old size the cursor jumps over it to pc 2
	// and nothing after the opcode is ever a leader.
	for (uint8_t op = 0x2E; op <= 0x34; ++op)
		cases.push_back({"one-byte array load", {op, 0xB1, 0xB1}, 2});

	// 0x36-0x3A istore..astore: the table said 1, JVMS says 2 -- they carry a
	// local-variable index. With the old size that index byte, 0xB1, decoded as
	// a return and split the block.
	for (uint8_t op = 0x36; op <= 0x3A; ++op)
		cases.push_back({"store with an index", {op, 0xB1, 0xB1}, 1});

	// 0xBC newarray: the table said 3, JVMS says 2 (a one-byte atype).
	// atype 10 = T_INT.
	cases.push_back({"newarray", {0xBC, 0x0A, 0xB1, 0xB1}, 2});

	// 0xBE arraylength: the table said 3, JVMS says 1.
	cases.push_back({"arraylength", {0xBE, 0xB1, 0xB1}, 2});

	auto pool = makeEmptyPool();
	for (const auto& c: cases)
	{
		CodeAttr code;
		code.bytecode = c.code;
		JvmLifter lifter(pool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << c.what << ": " << res.error;
		EXPECT_EQ(res.cfg.blockCount(), c.expectedBlocks)
			<< c.what << " (opcode 0x" << std::hex << static_cast<unsigned>(c.code[0]) << ")";
	}
}

// A pool whose entry 17 is a Methodref, so getstatic/putstatic/getfield/
// putfield/invokeinterface #17 resolve. 17 is 0x0011 on the wire, and 0x11 is
// sipush: that is what makes a one-byte step through the opcode desynchronise
// the scan instead of accidentally landing back on the next instruction.
static ConstPool makeRefPoolWithEntry17()
{
	std::vector<std::vector<uint8_t>> e;
	e.push_back(cpUtf8("Owner")); // 1
	e.push_back(cpClass(1));      // 2
	e.push_back(cpUtf8("f"));     // 3
	e.push_back(cpUtf8("()V"));   // 4
	e.push_back(cpNaT(3, 4));     // 5
	while (e.size() < 16)
		e.push_back(cpUtf8("p"));   // 6..16
	e.push_back(cpMethodref(2, 5)); // 17
	auto raw = makeCP(e);
	BinaryReader r(raw.data(), raw.size());
	return ConstPool::read(r);
}

// The same shape with a Class at 17, for anewarray.
static ConstPool makeClassPoolWithEntry17()
{
	std::vector<std::vector<uint8_t>> e;
	e.push_back(cpUtf8("Owner")); // 1
	while (e.size() < 16)
		e.push_back(cpUtf8("p")); // 2..16
	e.push_back(cpClass(1));      // 17
	auto raw = makeCP(e);
	BinaryReader r(raw.data(), raw.size());
	return ConstPool::read(r);
}

TEST(JvmLifter, InstructionSizesFollowJvms_FieldAndInvokeOpcodes)
{
	// 0xB2-0xB5 getstatic/putstatic/getfield/putfield: the table said 1, JVMS
	// says 3 (a u2 constant-pool index). These are in every non-trivial method,
	// so the desynchronised walk was the normal case: from `getstatic #17` the
	// old size stepped one byte, read the index high byte 0x00 as nop and its
	// low byte 0x11 as sipush, and swallowed the return that followed.
	auto refPool = makeRefPoolWithEntry17();
	for (uint8_t op: {0xB2, 0xB3, 0xB4, 0xB5})
	{
		CodeAttr code;
		code.bytecode = {op, 0x00, 0x11, 0xB1, 0xB1};
		JvmLifter lifter(refPool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << res.error;
		EXPECT_EQ(res.cfg.blockCount(), 2u) << "opcode 0x" << std::hex << static_cast<unsigned>(op);
	}

	// 0xB9 invokeinterface: the table said 3, JVMS says 5 -- a u2 index, a
	// one-byte argument count, and a reserved zero byte.
	{
		CodeAttr code;
		code.bytecode = {0xB9, 0x00, 0x11, 0x11, 0x00, 0xB1, 0xB1};
		JvmLifter lifter(refPool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << res.error;
		EXPECT_EQ(res.cfg.blockCount(), 2u);
	}

	// 0xBD anewarray: the table said 1, JVMS says 3.
	{
		auto classPool = makeClassPoolWithEntry17();
		CodeAttr code;
		code.bytecode = {0xBD, 0x00, 0x11, 0xB1, 0xB1};
		JvmLifter lifter(classPool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << res.error;
		EXPECT_EQ(res.cfg.blockCount(), 2u);
	}
}

TEST(JvmLifter, GetStaticThenGotoStillFindsTheBranchTarget)
{
	// The end-to-end symptom: `getstatic #17; goto +4; nop; return`. Stepping
	// one byte from the getstatic desynchronised the scan onto its operand
	// bytes, the goto was never seen as a branch, and its target never became a
	// leader -- so the method came back as one straight-line block.
	auto pool = makeRefPoolWithEntry17();
	CodeAttr code;
	code.bytecode = {
		0xB2,
		0x00,
		0x11, // pc 0: getstatic #17
		0xA7,
		0x00,
		0x04, // pc 3: goto +4 -> pc 7
		0x00, // pc 6: nop
		0xB1  // pc 7: return
	};
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	// Leaders 0, 6 (fall-through past the goto) and 7 (the branch target).
	ASSERT_EQ(res.cfg.blockCount(), 3u);
	ASSERT_FALSE(res.cfg.block(2).instrs.empty());
	EXPECT_EQ(res.cfg.block(2).instrs.front().offset, 7u);
	// And the goto's edge reaches it.
	EXPECT_TRUE(res.cfg.hasEdge(0, 2));
}

// ── pc + off wrapped, and the narrowed result became a leader ────────────────
// probe_jvm275.cpp asserted `leader < codeLen` for the eight
// `leaders.insert(static_cast<uint32_t>(pc + off))` sites and ESBMC refuted it
// twice: forward with codeLen = 33012, pc = 193, def = 94928897 giving leader
// 94929090, and backward with codeLen = 32773, pc = 32769, def = -1073774593,
// whose true target 32769 - 1073774593 = -1073741824 wrapped and narrowed to
// 0xC0000000. Both produced a block that no instruction falls into.

TEST(JvmLifter, ForwardBranchPastTheEndOfCodeIsNotALeader)
{
	CodeAttr code;
	// goto +32767 from pc 0, in a four-byte method.
	code.bytecode = {0xA7, 0x7F, 0xFF, 0xB1};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	// Leaders are 0 and 3 (the byte after the goto). 32767 is not one.
	EXPECT_EQ(res.cfg.blockCount(), 2u);
	for (uint32_t b = 0; b < res.cfg.blockCount(); ++b)
		EXPECT_FALSE(res.cfg.block(b).instrs.empty()) << "block " << b << " leads nothing";
}

TEST(JvmLifter, BackwardBranchBeforeTheStartOfCodeIsNotALeader)
{
	CodeAttr code;
	// nop, then goto -16 from pc 1: the true target is -15, before the method.
	code.bytecode = {0x00, 0xA7, 0xFF, 0xF0, 0xB1};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	// Leaders are 0 and 4. The old code inserted (uint32_t)(1 - 16) =
	// 0xFFFFFFF1 as a third.
	EXPECT_EQ(res.cfg.blockCount(), 2u);
	for (uint32_t b = 0; b < res.cfg.blockCount(); ++b)
		EXPECT_FALSE(res.cfg.block(b).instrs.empty()) << "block " << b << " leads nothing";
}

TEST(JvmLifter, TableSwitchDefaultPastTheEndOfCodeIsNotALeader)
{
	// The same defect at the tableswitch default site.
	CodeAttr code;
	code.bytecode = {
		0xAA,                   // pc 0: tableswitch
		0x00, 0x00, 0x00,       // pad to a 4-byte boundary
		0x7F, 0xFF, 0xFF, 0xFF, // default = +INT32_MAX
		0x00, 0x00, 0x00, 0x00, // lo = 0
		0x00, 0x00, 0x00, 0x00, // hi = 0
		0x00, 0x00, 0x00, 0x14, // case 0 -> +20 -> pc 20
		0xB1                    // pc 20: return
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	// Leaders: 0 and 20. The default lands 2147483647 bytes past pc 0 and is
	// no leader at all; the in-range case still is.
	ASSERT_EQ(res.cfg.blockCount(), 2u);
	ASSERT_FALSE(res.cfg.block(1).instrs.empty());
	EXPECT_EQ(res.cfg.block(1).instrs.front().offset, 20u);
}

TEST(JvmLifter, InRangeBranchesStillResolve)
{
	// The range check must not cost a valid branch: both directions inside the
	// method still produce leaders and edges.
	CodeAttr code;
	code.bytecode = {
		0x00, // pc 0: nop
		0xA7,
		0x00,
		0x03, // pc 1: goto +3 -> pc 4
		0xA7,
		0xFF,
		0xFD, // pc 4: goto -3 -> pc 1
	};
	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	// Leaders 0, 1 and 4.
	ASSERT_EQ(res.cfg.blockCount(), 3u);
	EXPECT_EQ(res.cfg.block(1).instrs.front().offset, 1u);
	EXPECT_EQ(res.cfg.block(2).instrs.front().offset, 4u);
	EXPECT_TRUE(res.cfg.hasEdge(1, 2)); // pc 1 -> pc 4
	EXPECT_TRUE(res.cfg.hasEdge(2, 1)); // pc 4 -> pc 1
	// And decodeInstr recorded the real pc, not the "no target" sentinel: the
	// range check must not cost a valid branch its operand either.
	EXPECT_EQ(res.cfg.block(1).instrs.front().blockOp(0), 4u);
	EXPECT_EQ(res.cfg.block(2).instrs.front().blockOp(0), 1u);
}

// ── decodeInstr's branchPc recorded an out-of-range displacement as a pc ─────
// findLeaders and decodeInstr resolve the same displacement twice, and only the
// first of the two was checked. A target outside code[] is dropped by
// findLeaders, so no block is created for it and no edge can be wired to it
// either way -- what the two branchPc bodies disagree about is the value
// decodeInstr writes into the instruction's BcBlockOperand, which is the branch
// target every later pass reads. The old body,
// `static_cast<uint32_t>(static_cast<int32_t>(instrPc) + offset)`, wrote the
// raw sum there with no range check at all.

TEST(JvmLifter, OutOfRangeBranchTargetIsNotRecordedAsAPc)
{
	// UINT32_MAX is the lifter's kNoBranchTarget: "this branch leaves the
	// method". It is not a reachable pc, because a resolved target is always
	// below the code size and the code size is capped at UINT32_MAX.
	constexpr uint32_t kNoTarget = UINT32_MAX;
	auto pool = makeEmptyPool();

	{
		// pc 0: goto +100, in a four-byte method. The old body recorded 100 --
		// a pc 96 bytes past the end of code[].
		CodeAttr code;
		code.bytecode = {0xA7, 0x00, 0x64, 0xB1};
		JvmLifter lifter(pool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << res.error;
		ASSERT_GE(res.cfg.blockCount(), 1u);
		ASSERT_FALSE(res.cfg.block(0).instrs.empty());
		const auto& g = res.cfg.block(0).instrs.front();
		ASSERT_EQ(g.opcode, BcOpcode::Goto);
		EXPECT_EQ(g.blockOp(0), kNoTarget) << "forward branch past the end of code[] recorded as a pc";
	}
	{
		// pc 1: goto -8, whose true target is -7. The old body recorded
		// (uint32_t)(1 - 8) = 0xFFFFFFF9.
		CodeAttr code;
		code.bytecode = {0x00, 0xA7, 0xFF, 0xF8, 0xB1};
		JvmLifter lifter(pool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << res.error;
		ASSERT_GE(res.cfg.blockCount(), 1u);
		ASSERT_GE(res.cfg.block(0).instrs.size(), 2u);
		const auto& g = res.cfg.block(0).instrs[1];
		ASSERT_EQ(g.opcode, BcOpcode::Goto);
		EXPECT_EQ(g.blockOp(0), kNoTarget) << "backward branch before the start of code[] recorded as a pc";
	}
	{
		// The same at a conditional branch: ifeq +200 at pc 0 of a five-byte
		// method. The old body recorded 200.
		CodeAttr code;
		code.bytecode = {0x99, 0x00, 0xC8, 0x00, 0xB1};
		JvmLifter lifter(pool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << res.error;
		ASSERT_GE(res.cfg.blockCount(), 1u);
		ASSERT_FALSE(res.cfg.block(0).instrs.empty());
		const auto& b = res.cfg.block(0).instrs.front();
		ASSERT_EQ(b.opcode, BcOpcode::IfEq);
		EXPECT_EQ(b.blockOp(0), kNoTarget) << "conditional branch past the end of code[] recorded as a pc";
	}
	{
		// goto_w carries a full int32 displacement, so the old body's
		// `static_cast<int32_t>(instrPc) + offset` overflows int32 from pc 1
		// with an offset of INT32_MAX -- a seven-byte method is enough to make
		// it undefined, no 2 GiB code array needed. Where it wraps rather than
		// trapping it lands on 0x80000000.
		CodeAttr code;
		code.bytecode = {0x00, 0xC8, 0x7F, 0xFF, 0xFF, 0xFF, 0xB1};
		JvmLifter lifter(pool, leadersOnly());
		auto res = lifter.lift(code, "()V");
		ASSERT_TRUE(res.ok) << res.error;
		ASSERT_GE(res.cfg.blockCount(), 1u);
		ASSERT_GE(res.cfg.block(0).instrs.size(), 2u);
		const auto& g = res.cfg.block(0).instrs[1];
		ASSERT_EQ(g.opcode, BcOpcode::Goto);
		EXPECT_EQ(g.blockOp(0), kNoTarget) << "goto_w +INT32_MAX recorded as a pc";
	}
}

// ── the two exception-table leader sites had no range check ─────────────────
// start_pc and handler_pc are u2 fields copied verbatim out of the class file
// and jvm_attr's parser checks neither against code_length, so
// `leaders.insert(e.handlerPc)` put a leader at a pc no instruction falls into
// and buildBlocks() then created an empty block there.

TEST(JvmLifter, ExceptionHandlerPcPastTheEndOfCodeIsNotALeader)
{
	CodeAttr code;
	code.bytecode = {0x00, 0xB1}; // nop; return
	ExceptionEntry e;
	e.startPc = 0;
	e.endPc = 2;       // the whole method: a well-formed region
	e.handlerPc = 900; // 898 bytes past the end of code[]
	e.catchType = 0;
	code.exceptionTable.push_back(e);

	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	// Only leader 0. The old code inserted 900 as a second one.
	EXPECT_EQ(res.cfg.blockCount(), 1u);
	for (uint32_t b = 0; b < res.cfg.blockCount(); ++b)
		EXPECT_FALSE(res.cfg.block(b).instrs.empty()) << "block " << b << " leads nothing";
	// And with no block at pc 900 there is nothing to wire the handler to.
	EXPECT_TRUE(res.cfg.handlers().empty());
}

TEST(JvmLifter, ExceptionStartPcAtTheEndOfCodeIsNotALeader)
{
	// start_pc == code_length is the boundary case that still survives the
	// protected-region check (an empty region at the very end), so it is the
	// one that reaches the start_pc leader site. There is no instruction at
	// pc 2 for a block to lead.
	CodeAttr code;
	code.bytecode = {0x00, 0xB1}; // nop; return
	ExceptionEntry e;
	e.startPc = 2;
	e.endPc = 2;
	e.handlerPc = 1;
	e.catchType = 0;
	code.exceptionTable.push_back(e);

	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	// Leaders 0 and 1 (the handler). The old code inserted 2 as a third.
	EXPECT_EQ(res.cfg.blockCount(), 2u);
	for (uint32_t b = 0; b < res.cfg.blockCount(); ++b)
		EXPECT_FALSE(res.cfg.block(b).instrs.empty()) << "block " << b << " leads nothing";
}

// ── the protected region reached the CFG unchecked ──────────────────────────
// wireExceptions assigns startPc/endPc straight into
// BcExceptionHandler::startOffset/endOffset. Both are u2 file fields, so
// end_pc < start_pc and end_pc past the end of code[] both got through, and a
// consumer asking how long the region is by `endOffset - startOffset` in
// uint32 got an extent near 4 GB.

TEST(JvmLifter, ProtectedRegionThatEndsBeforeItBeginsIsNotWired)
{
	CodeAttr code;
	code.bytecode = {0x00, 0x00, 0x00, 0xB1, 0xB1};
	ExceptionEntry e;
	e.startPc = 4;
	e.endPc = 0; // 0 - 4 in uint32 is 4294967292
	e.handlerPc = 4;
	e.catchType = 0;
	code.exceptionTable.push_back(e);

	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	for (const auto& h: res.cfg.handlers())
		EXPECT_LE(h.startOffset, h.endOffset) << "protected region ends before it begins";
	EXPECT_TRUE(res.cfg.handlers().empty());
}

TEST(JvmLifter, ProtectedRegionPastTheEndOfCodeIsNotWired)
{
	CodeAttr code;
	code.bytecode = {0x00, 0x00, 0x00, 0xB1, 0xB1};
	ExceptionEntry e;
	e.startPc = 0;
	e.endPc = 900; // 895 bytes past the end of code[]
	e.handlerPc = 4;
	e.catchType = 0;
	code.exceptionTable.push_back(e);

	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	for (const auto& h: res.cfg.handlers())
		EXPECT_LE(h.endOffset, 5u) << "protected region ends past the end of code[]";
	EXPECT_TRUE(res.cfg.handlers().empty());
}

TEST(JvmLifter, ProtectedRegionCoveringTheWholeMethodIsStillWired)
{
	// The check must not cost a real handler: JVMS 4.7.3 lets end_pc equal
	// code_length, which is the boundary the range test is most likely to get
	// wrong in the other direction.
	CodeAttr code;
	code.bytecode = {0x00, 0x00, 0x00, 0xB1, 0xB1};
	ExceptionEntry e;
	e.startPc = 0;
	e.endPc = 5; // == code_length
	e.handlerPc = 4;
	e.catchType = 0;
	code.exceptionTable.push_back(e);

	auto pool = makeEmptyPool();
	JvmLifter lifter(pool, leadersOnly());
	auto res = lifter.lift(code, "()V");
	ASSERT_TRUE(res.ok) << res.error;
	ASSERT_EQ(res.cfg.handlers().size(), 1u);
	EXPECT_EQ(res.cfg.handlers()[0].startOffset, 0u);
	EXPECT_EQ(res.cfg.handlers()[0].endOffset, 5u);
	EXPECT_TRUE(res.cfg.block(res.cfg.handlers()[0].handlerBlock).isExceptionHandler);
}

// ── multiReleaseVersion compared 19 characters against an 18-character literal
// `path.substr(0, 19) != "META-INF/versions/"` is true for every entry in every
// JAR, so multi-release selection was dead code and every versioned class was
// added a second time under a package name derived from the unstripped path.

TEST(JarReader, MultiReleaseEntryReportsItsVersion)
{
	auto zip = storedZip({
		{"META-INF/versions/9/Hello.class", {0x00}},
		{"Hello.class", {0x00}},
		{"META-INF/versions/17/Hello.class", {0x00}},
	});
	JarReader reader;
	auto entries = reader.listEntries(zip.data(), zip.size());
	ASSERT_EQ(entries.size(), 3u);
	EXPECT_EQ(entries[0].version, 9);
	EXPECT_EQ(entries[1].version, 0);
	EXPECT_EQ(entries[2].version, 17);
}

TEST(JarReader, MultiReleaseNonVersionsAreNotVersions)
{
	// A near miss and a malformed run must both stay at 0 rather than being
	// read as a version: decimalRun reports an empty digit run as failure, and
	// the run has to be a whole path segment.
	auto zip = storedZip({
		{"META-INF/versionsX/9/Hello.class", {0x00}},
		{"META-INF/versions//Hello.class", {0x00}},
		{"META-INF/versions/9x/Hello.class", {0x00}},
		{"META-INF/versions/", {0x00}},
	});
	JarReader reader;
	auto entries = reader.listEntries(zip.data(), zip.size());
	ASSERT_EQ(entries.size(), 4u);
	for (const auto& e: entries)
		EXPECT_EQ(e.version, 0) << e.path;
}

TEST(JarReader, MultiReleaseClassIsNotCountedTwice)
{
	// The base class and its Java 9 override are one class, not two.
	auto cls = makeHelloWorldClass();
	auto zip = storedZip({
		{"Hello.class", cls},
		{"META-INF/versions/9/Hello.class", cls},
	});
	JarReader reader; // targetJavaVersion defaults to 21
	auto res = reader.read(zip.data(), zip.size());
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_EQ(res.classesFound, 1u);
}

TEST(JarReader, MultiReleaseEntryAboveTheTargetIsSkipped)
{
	// Selection is live again, so a versioned entry newer than the target
	// runtime is dropped and the base class is the one that is kept.
	auto cls = makeHelloWorldClass();
	auto zip = storedZip({
		{"Hello.class", cls},
		{"META-INF/versions/17/Hello.class", cls},
	});
	JarReadOptions opts;
	opts.targetJavaVersion = 11;
	JarReader reader(opts);
	auto res = reader.read(zip.data(), zip.size());
	ASSERT_TRUE(res.ok) << res.error;
	EXPECT_EQ(res.classesFound, 1u);
	EXPECT_GE(res.classesParsed, 1u);
}

// ─── Instruction ids are method-global ───────────────────────────────────────

// buildBlocks() restarted its instruction counter at 0 in every block, so a
// method's instructions carried ids 0,1 / 0,1 / 0,1,2 instead of 0..6. Every
// method-wide map keyed by BcInstruction::id -- StackSimResult::instrInfo,
// slot_coalesce's slotDefInstr/slotUseInstr, pattern_lift's
// firstInstrId/lastInstrId -- then held one entry per block ordinal rather than
// one per instruction, and each block's instruction 0 read back whatever the
// last-simulated block wrote there.
TEST(JvmLifter, InstructionIdsAreUniqueAcrossTheWholeMethod)
{
	CodeAttr code;
	code.bytecode = {
		0x04,             // 0: iconst_1
		0x99, 0x00, 0x05, // 1: ifeq +5 -> 6
		0x05,             // 4: iconst_2
		0x57,             // 5: pop
		0x01,             // 6: aconst_null
		0x57,             // 7: pop
		0xB1,             // 8: return
	};
	code.maxStack = 4;
	code.maxLocals = 2;
	ConstPool pool;
	JvmLifter lifter(pool);
	auto result = lifter.lift(code, "()V");
	ASSERT_TRUE(result.ok) << result.error;
	ASSERT_LT(1u, result.cfg.blockCount()) << "the branch must split the method";

	std::vector<uint32_t> ids;
	for (uint32_t b = 0; b < result.cfg.blockCount(); ++b)
		for (const auto& insn: result.cfg.block(b).instrs) ids.push_back(insn.id);
	ASSERT_EQ(9u - 2u, ids.size()); // seven instructions, ifeq is three bytes
	std::vector<uint32_t> sorted = ids;
	std::sort(sorted.begin(), sorted.end());
	ASSERT_EQ(sorted.end(), std::unique(sorted.begin(), sorted.end()))
		<< "two instructions share an id";
	// And they are the consecutive run 0..n-1, so a vector indexed by id works.
	for (size_t i = 0; i < sorted.size(); ++i) EXPECT_EQ(static_cast<uint32_t>(i), sorted[i]);
}
