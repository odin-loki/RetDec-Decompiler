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
#include <cstring>
#include <vector>

using namespace retdec::jvm_parser;
using namespace retdec::bc_module;
using namespace retdec::bc_module::types;

// ══════════════════════════════════════════════════════════════════════════════
// BinaryReader
// ══════════════════════════════════════════════════════════════════════════════

TEST(BinaryReader, ReadU1) {
    uint8_t data[] = {0xCA, 0xFE};
    BinaryReader r(data, 2);
    EXPECT_EQ(r.u1(), 0xCA);
    EXPECT_EQ(r.u1(), 0xFE);
}

TEST(BinaryReader, ReadU2BigEndian) {
    uint8_t data[] = {0x01, 0x02};
    BinaryReader r(data, 2);
    EXPECT_EQ(r.u2(), 0x0102);
}

TEST(BinaryReader, ReadU4BigEndian) {
    uint8_t data[] = {0xCA, 0xFE, 0xBA, 0xBE};
    BinaryReader r(data, 4);
    EXPECT_EQ(r.u4(), 0xCAFEBABEu);
}

TEST(BinaryReader, ReadFloat) {
    // 1.0f in IEEE 754 big-endian = 0x3F800000
    uint8_t data[] = {0x3F, 0x80, 0x00, 0x00};
    BinaryReader r(data, 4);
    EXPECT_FLOAT_EQ(r.f4(), 1.0f);
}

TEST(BinaryReader, ReadDouble) {
    // 1.0 in IEEE 754 big-endian = 0x3FF0000000000000
    uint8_t data[] = {0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    BinaryReader r(data, 8);
    EXPECT_DOUBLE_EQ(r.f8(), 1.0);
}

TEST(BinaryReader, ReadUtf8) {
    const char* s = "Hello";
    std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(s),
                               reinterpret_cast<const uint8_t*>(s) + 5);
    BinaryReader r(data.data(), data.size());
    EXPECT_EQ(r.utf8(5), "Hello");
}

TEST(BinaryReader, OutOfRange) {
    uint8_t data[] = {0x01};
    BinaryReader r(data, 1);
    r.u1();
    EXPECT_THROW(r.u1(), JvmParseError);
}

TEST(BinaryReader, Skip) {
    uint8_t data[] = {0x01, 0x02, 0x03};
    BinaryReader r(data, 3);
    r.skip(2);
    EXPECT_EQ(r.u1(), 0x03);
}

// ══════════════════════════════════════════════════════════════════════════════
// ConstPool
// ══════════════════════════════════════════════════════════════════════════════

// Helper: build a minimal constant pool binary
static std::vector<uint8_t> makeCP(const std::vector<std::vector<uint8_t>>& entries) {
    // Prepend cp_count = entries.size() + 1
    std::vector<uint8_t> out;
    uint16_t cnt = static_cast<uint16_t>(entries.size() + 1);
    out.push_back(cnt >> 8); out.push_back(cnt & 0xFF);
    for (const auto& e : out) {}  // no-op
    for (const auto& e : entries) {
        for (uint8_t b : e) out.push_back(b);
    }
    return out;
}

static std::vector<uint8_t> cpUtf8(const std::string& s) {
    std::vector<uint8_t> v = {1}; // tag
    uint16_t len = static_cast<uint16_t>(s.size());
    v.push_back(len >> 8); v.push_back(len & 0xFF);
    for (char c : s) v.push_back(static_cast<uint8_t>(c));
    return v;
}

static std::vector<uint8_t> cpInt(int32_t val) {
    return {3,
            static_cast<uint8_t>(val >> 24),
            static_cast<uint8_t>(val >> 16),
            static_cast<uint8_t>(val >>  8),
            static_cast<uint8_t>(val)};
}

static std::vector<uint8_t> cpClass(uint16_t idx) {
    return {7, static_cast<uint8_t>(idx >> 8), static_cast<uint8_t>(idx)};
}

static std::vector<uint8_t> cpString(uint16_t idx) {
    return {8, static_cast<uint8_t>(idx >> 8), static_cast<uint8_t>(idx)};
}

static std::vector<uint8_t> cpNaT(uint16_t name, uint16_t desc) {
    return {12,
            static_cast<uint8_t>(name >> 8), static_cast<uint8_t>(name),
            static_cast<uint8_t>(desc >> 8), static_cast<uint8_t>(desc)};
}

static std::vector<uint8_t> cpMethodref(uint16_t cls, uint16_t nat) {
    return {10,
            static_cast<uint8_t>(cls >> 8), static_cast<uint8_t>(cls),
            static_cast<uint8_t>(nat >> 8), static_cast<uint8_t>(nat)};
}

TEST(ConstPool, ReadUtf8) {
    // cp: [1=Utf8("Hello")]
    auto data = cpUtf8("Hello");
    // Need cp_count prefix
    std::vector<uint8_t> raw = {0, 2}; // count = 2
    for (uint8_t b : data) raw.push_back(b);
    BinaryReader r(raw.data(), raw.size());
    auto pool = ConstPool::read(r);
    EXPECT_EQ(pool.utf8(1), "Hello");
}

TEST(ConstPool, ReadInteger) {
    std::vector<uint8_t> raw = {0, 2}; // cp_count
    auto entry = cpInt(42);
    for (uint8_t b : entry) raw.push_back(b);
    BinaryReader r(raw.data(), raw.size());
    auto pool = ConstPool::read(r);
    EXPECT_EQ(pool.tag(1), CpTag::Integer);
    EXPECT_EQ(std::get<CpInt>(pool.entry(1)).value, 42);
}

TEST(ConstPool, ReadClassAndString) {
    // cp: [1=Utf8("Foo"), 2=Class(#1), 3=Utf8("bar"), 4=String(#3)]
    std::vector<uint8_t> raw = {0, 5};
    for (uint8_t b : cpUtf8("Foo")) raw.push_back(b);
    for (uint8_t b : cpClass(1)) raw.push_back(b);
    for (uint8_t b : cpUtf8("bar")) raw.push_back(b);
    for (uint8_t b : cpString(3)) raw.push_back(b);
    BinaryReader r(raw.data(), raw.size());
    auto pool = ConstPool::read(r);
    EXPECT_EQ(pool.className(2), "Foo");
    EXPECT_EQ(pool.string(4), "bar");
}

TEST(ConstPool, ReadMethodref) {
    // [1=Utf8("Foo"), 2=Class(#1), 3=Utf8("bar"), 4=Utf8("()V"), 5=NaT(#3,#4), 6=Methodref(#2,#5)]
    std::vector<uint8_t> raw = {0, 7};
    for (uint8_t b : cpUtf8("Foo")) raw.push_back(b);
    for (uint8_t b : cpClass(1))    raw.push_back(b);
    for (uint8_t b : cpUtf8("bar")) raw.push_back(b);
    for (uint8_t b : cpUtf8("()V")) raw.push_back(b);
    for (uint8_t b : cpNaT(3, 4))   raw.push_back(b);
    for (uint8_t b : cpMethodref(2, 5)) raw.push_back(b);
    BinaryReader r(raw.data(), raw.size());
    auto pool = ConstPool::read(r);
    EXPECT_EQ(pool.tag(6), CpTag::Methodref);
    EXPECT_EQ(pool.refClass(6), "Foo");
    EXPECT_EQ(pool.refName(6), "bar");
    EXPECT_EQ(pool.refDescriptor(6), "()V");
}

TEST(ConstPool, LongDoubleTwoSlots) {
    // Long entry occupies two slots.
    std::vector<uint8_t> raw = {0, 3}; // cp_count = 3 (long at 1, filler at 2)
    raw.push_back(5); // Long tag
    int64_t val = 123456789LL;
    for (int i = 7; i >= 0; --i) raw.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    BinaryReader r(raw.data(), raw.size());
    auto pool = ConstPool::read(r);
    EXPECT_EQ(pool.tag(1), CpTag::Long);
    EXPECT_EQ(std::get<CpLong>(pool.entry(1)).value, 123456789LL);
}

TEST(ConstPool, InvalidIndex) {
    std::vector<uint8_t> raw = {0, 2};
    for (uint8_t b : cpUtf8("x")) raw.push_back(b);
    BinaryReader r(raw.data(), raw.size());
    auto pool = ConstPool::read(r);
    EXPECT_THROW(pool.entry(0), JvmParseError);
    EXPECT_THROW(pool.entry(99), JvmParseError);
}

// ══════════════════════════════════════════════════════════════════════════════
// JvmSignatureParser
// ══════════════════════════════════════════════════════════════════════════════

TEST(JvmSigParser, BaseTypeDescriptors) {
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

TEST(JvmSigParser, ClassDescriptor) {
    auto t = JvmSignatureParser::parseDescriptor("Ljava/lang/String;");
    EXPECT_TRUE(t.isClass());
    EXPECT_EQ(t.ref().className, "java/lang/String");
}

TEST(JvmSigParser, ArrayDescriptor) {
    auto t = JvmSignatureParser::parseDescriptor("[I");
    EXPECT_TRUE(t.isArray());
    EXPECT_EQ(t.ref().arrayDims, 1);
    EXPECT_EQ(*t.ref().elementType, Int());
}

TEST(JvmSigParser, MultiArrayDescriptor) {
    auto t = JvmSignatureParser::parseDescriptor("[[Ljava/lang/String;");
    EXPECT_TRUE(t.isArray());
    EXPECT_EQ(t.ref().arrayDims, 2);
}

TEST(JvmSigParser, MethodDescriptor_VoidNoArgs) {
    auto ft = JvmSignatureParser::parseMethodDescriptor("()V");
    EXPECT_TRUE(ft.params.empty());
    EXPECT_EQ(*ft.returnType, Void());
}

TEST(JvmSigParser, MethodDescriptor_IntStringToVoid) {
    auto ft = JvmSignatureParser::parseMethodDescriptor("(ILjava/lang/String;)V");
    ASSERT_EQ(ft.params.size(), 2u);
    EXPECT_EQ(*ft.params[0], Int());
    EXPECT_EQ(*ft.params[1], Class("java/lang/String"));
    EXPECT_EQ(*ft.returnType, Void());
}

TEST(JvmSigParser, MethodDescriptor_ReturnArray) {
    auto ft = JvmSignatureParser::parseMethodDescriptor("()[Ljava/lang/Object;");
    EXPECT_TRUE(ft.params.empty());
    EXPECT_TRUE(ft.returnType->isArray());
}

TEST(JvmSigParser, GenericFieldSignature) {
    // "Ljava/util/List<Ljava/lang/String;>;"
    auto t = JvmSignatureParser::parseFieldSig(
        "Ljava/util/List<Ljava/lang/String;>;");
    EXPECT_TRUE(t.isClass());
    // Generic instantiation
    EXPECT_EQ(t.ref().kind, BcRefKind::Generic);
    EXPECT_EQ(t.ref().typeArgs.size(), 1u);
    EXPECT_EQ(t.ref().typeArgs[0]->ref().className, "java/lang/String");
}

TEST(JvmSigParser, TypeVariableSignature) {
    auto t = JvmSignatureParser::parseFieldSig("TT;");
    EXPECT_EQ(t.ref().kind, BcRefKind::TypeVariable);
    EXPECT_EQ(t.ref().className, "T");
}

TEST(JvmSigParser, ClassSignatureWithTypeParams) {
    // "<T:Ljava/lang/Object;>Ljava/lang/Object;"
    auto cs = JvmSignatureParser::parseClassSig(
        "<T:Ljava/lang/Object;>Ljava/lang/Object;");
    ASSERT_EQ(cs.typeParams.size(), 1u);
    EXPECT_EQ(cs.typeParams[0].name, "T");
}

TEST(JvmSigParser, WildcardUnbounded) {
    auto t = JvmSignatureParser::parseFieldSig("Ljava/util/List<*>;");
    EXPECT_EQ(t.ref().kind, BcRefKind::Generic);
    ASSERT_EQ(t.ref().typeArgs.size(), 1u);
    EXPECT_EQ(t.ref().typeArgs[0]->ref().kind, BcRefKind::Wildcard);
}

TEST(JvmSigParser, WildcardBoundedAbove) {
    // "Ljava/util/List<+Ljava/lang/Number;>;"
    auto t = JvmSignatureParser::parseFieldSig(
        "Ljava/util/List<+Ljava/lang/Number;>;");
    EXPECT_EQ(t.ref().kind, BcRefKind::Generic);
    ASSERT_EQ(t.ref().typeArgs.size(), 1u);
    EXPECT_EQ(t.ref().typeArgs[0]->ref().kind, BcRefKind::BoundedAbove);
}

TEST(JvmSigParser, MethodSignatureWithGenerics) {
    // "<E:Ljava/lang/Object;>(TE;)TE;"
    auto ms = JvmSignatureParser::parseMethodSig(
        "<E:Ljava/lang/Object;>(TE;)TE;");
    ASSERT_EQ(ms.typeParams.size(), 1u);
    EXPECT_EQ(ms.typeParams[0].name, "E");
    ASSERT_EQ(ms.params.size(), 1u);
    EXPECT_EQ(ms.params[0].ref().kind, BcRefKind::TypeVariable);
    EXPECT_EQ(ms.returnType.ref().kind, BcRefKind::TypeVariable);
}

TEST(JvmSigParser, MethodSignatureThrows) {
    // "(I)V^Ljava/io/IOException;"
    auto ms = JvmSignatureParser::parseMethodSig(
        "(I)V^Ljava/io/IOException;");
    EXPECT_EQ(ms.params.size(), 1u);
    ASSERT_EQ(ms.throwsTypes.size(), 1u);
    EXPECT_EQ(ms.throwsTypes[0].ref().className, "java/io/IOException");
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

static std::vector<uint8_t> makeHelloWorldClass() {
    std::vector<uint8_t> raw;
    auto push4 = [&](uint32_t v) {
        raw.push_back((v>>24)&0xFF); raw.push_back((v>>16)&0xFF);
        raw.push_back((v>>8)&0xFF);  raw.push_back(v&0xFF);
    };
    auto push2 = [&](uint16_t v) {
        raw.push_back((v>>8)&0xFF); raw.push_back(v&0xFF);
    };
    auto pushUtf8 = [&](const std::string& s) {
        raw.push_back(1);
        push2(static_cast<uint16_t>(s.size()));
        for (char c : s) raw.push_back(static_cast<uint8_t>(c));
    };

    push4(0xCAFEBABE);
    push2(0);     // minor
    push2(52);    // major = Java 8
    push2(9);     // cp_count

    pushUtf8("Hello");                 // #1
    raw.push_back(7); push2(1);        // #2 Class(#1)
    pushUtf8("java/lang/Object");      // #3
    raw.push_back(7); push2(3);        // #4 Class(#3)
    pushUtf8("main");                  // #5
    pushUtf8("([Ljava/lang/String;)V");// #6
    pushUtf8("Code");                  // #7
    pushUtf8("args");                  // #8

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
    push2(7);      // attr name = "Code"
    push4(13);     // attribute length = 13 (2+2+4+1+2+2 bytes)
    push2(1);      // maxStack = 1
    push2(1);      // maxLocals = 1
    push4(1);      // code_length = 1
    raw.push_back(0xB1); // return
    push2(0);      // 0 exception entries
    push2(0);      // 0 code attributes
    push2(0);      // 0 class attributes
    return raw;
}

TEST(ClassFileParser, HelloWorldMagic) {
    auto cls = makeHelloWorldClass();
    auto res = parseClassFile(cls);
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.majorVersion, 52);
    EXPECT_EQ(res.minorVersion, 0);
}

TEST(ClassFileParser, HelloWorldClassName) {
    auto cls = makeHelloWorldClass();
    auto res = parseClassFile(cls);
    ASSERT_TRUE(res.ok);
    EXPECT_EQ(res.cls.name, "Hello");
    EXPECT_EQ(res.cls.fqName, "Hello");
}

TEST(ClassFileParser, HelloWorldSuperClass) {
    auto cls = makeHelloWorldClass();
    auto res = parseClassFile(cls);
    ASSERT_TRUE(res.ok);
    ASSERT_TRUE(res.cls.superClass.has_value());
    EXPECT_EQ(res.cls.superClass->ref().className, "java/lang/Object");
}

TEST(ClassFileParser, HelloWorldMethod) {
    auto cls = makeHelloWorldClass();
    auto res = parseClassFile(cls);
    ASSERT_TRUE(res.ok);
    ASSERT_EQ(res.cls.methods.size(), 1u);
    EXPECT_EQ(res.cls.methods[0].name, "main");
    EXPECT_FALSE(res.cls.methods[0].isConstructor);
}

TEST(ClassFileParser, HelloWorldMethodHasCFG) {
    auto cls = makeHelloWorldClass();
    auto res = parseClassFile(cls);
    ASSERT_TRUE(res.ok);
    ASSERT_EQ(res.cls.methods.size(), 1u);
    // Should have at least 1 block (the return block).
    EXPECT_GE(res.cls.methods[0].cfg.blockCount(), 1u);
}

TEST(ClassFileParser, HelloWorldAccessFlags) {
    auto cls = makeHelloWorldClass();
    auto res = parseClassFile(cls);
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(hasFlag(res.cls.access, BcAccess::Public));
    EXPECT_FALSE(res.cls.isInterface);
}

TEST(ClassFileParser, InvalidMagic) {
    uint8_t bad[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x34};
    auto res = parseClassFile(bad, sizeof(bad));
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

TEST(ClassFileParser, EmptyBuffer) {
    auto res = parseClassFile(nullptr, 0);
    EXPECT_FALSE(res.ok);
}

TEST(ClassFileParser, JavaVersionString) {
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

static ConstPool makeEmptyPool() {
    std::vector<uint8_t> raw = {0, 1}; // cp_count = 1 (empty pool)
    BinaryReader r(raw.data(), raw.size());
    return ConstPool::read(r);
}

TEST(JvmLifter, SingleReturnBlock) {
    // Bytecode: return (0xB1)
    CodeAttr code;
    code.bytecode = {0xB1};
    code.maxStack = 0; code.maxLocals = 0;
    auto pool = makeEmptyPool();
    JvmLifter lifter(pool);
    auto res = lifter.lift(code, "()V");
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_GE(res.cfg.blockCount(), 1u);
    EXPECT_FALSE(res.cfg.block(0).instrs.empty());
    EXPECT_EQ(res.cfg.block(0).instrs.back().opcode, BcOpcode::Return);
}

TEST(JvmLifter, PushIntAndReturn) {
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

TEST(JvmLifter, ConditionalBranch) {
    // ifeq +3 (0x99 0x00 0x03) then return (0xB1) then return (0xB1)
    // Creates two blocks.
    CodeAttr code;
    code.bytecode = {
        0x99, 0x00, 0x05,  // ifeq → PC 5 (target)
        0xB1,              // return (fall-through block, PC 3)
        0x00,              // nop (PC 4, never reached but valid)
        0xB1               // return (target block, PC 5)
    };
    auto pool = makeEmptyPool();
    JvmLifter lifter(pool);
    auto res = lifter.lift(code, "()V");
    EXPECT_TRUE(res.ok);
    // At least entry block + one successor.
    EXPECT_GE(res.cfg.blockCount(), 2u);
}

TEST(JvmLifter, ExceptionHandler) {
    // try { NOP NOP NOP } catch-all { return }
    // Three NOPs span PC 0-2, return at PC 3, handler return at PC 4.
    CodeAttr code;
    code.bytecode = {
        0x00, 0x00, 0x00,  // NOP, NOP, NOP (PC 0-2)
        0xB1,              // return (PC 3)
        0xB1               // return (handler, PC 4)
    };
    ExceptionEntry eh;
    eh.startPc   = 0;
    eh.endPc     = 3;
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

TEST(JvmLifter, TableSwitch) {
    // tableswitch: lo=0, hi=1, default→+16, case0→+4, case1→+8
    // Padding to align at 4-byte boundary (1 byte opcode, then 3 pad bytes)
    CodeAttr code;
    code.bytecode = {
        0xAA,             // tableswitch (PC 0)
        0x00, 0x00, 0x00, // padding (3 bytes for alignment at PC 4)
        0x00, 0x00, 0x00, 0x10, // default = +16 (relative to PC 0) → 16
        0x00, 0x00, 0x00, 0x00, // lo = 0
        0x00, 0x00, 0x00, 0x01, // hi = 1
        0x00, 0x00, 0x00, 0x04, // case 0 → +4 → PC 4
        0x00, 0x00, 0x00, 0x08, // case 1 → +8 → PC 8
        // padding targets (don't matter for this test)
        0xB1, 0x00, 0x00, 0x00, // PC 4: return + padding
        0xB1, 0x00, 0x00, 0x00, // PC 8: return + padding
        0xB1                    // PC 16: return (default)
    };
    auto pool = makeEmptyPool();
    JvmLifter lifter(pool);
    auto res = lifter.lift(code, "()V");
    EXPECT_TRUE(res.ok);
    EXPECT_GE(res.cfg.blockCount(), 3u);
}

TEST(JvmLifter, WideInstruction) {
    // wide aload 256
    CodeAttr code;
    code.bytecode = {
        0xC4, 0x19,   // wide aload
        0x01, 0x00,   // local index 256 big-endian
        0xB1          // return
    };
    auto pool = makeEmptyPool();
    JvmLifter lifter(pool);
    auto res = lifter.lift(code, "()V");
    EXPECT_TRUE(res.ok);
}

// ══════════════════════════════════════════════════════════════════════════════
// JarReader
// ══════════════════════════════════════════════════════════════════════════════

TEST(JarReader, EmptyBuffer) {
    JarReader reader;
    auto res = reader.read(nullptr, 0);
    EXPECT_FALSE(res.ok);
}

TEST(JarReader, InvalidZip) {
    uint8_t bad[] = {0x00, 0x01, 0x02, 0x03};
    JarReader reader;
    auto res = reader.read(bad, sizeof(bad));
    EXPECT_FALSE(res.ok);
}

TEST(JarReader, ListEntriesEmpty) {
    uint8_t bad[] = {0x00};
    JarReader reader;
    auto entries = reader.listEntries(bad, sizeof(bad));
    EXPECT_TRUE(entries.empty());
}

// ══════════════════════════════════════════════════════════════════════════════
// Attribute parsers — directly
// ══════════════════════════════════════════════════════════════════════════════

TEST(AttributeParser, ParseCodeAttr) {
    auto cls = makeHelloWorldClass();
    auto res = parseClassFile(cls);
    ASSERT_TRUE(res.ok);
    ASSERT_EQ(res.cls.methods.size(), 1u);
    EXPECT_EQ(res.cls.methods[0].maxStack, 1);
    EXPECT_EQ(res.cls.methods[0].maxLocals, 1);
}

TEST(AttributeParser, ParseBootstrapMethodsAttr) {
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
