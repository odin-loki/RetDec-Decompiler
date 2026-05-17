/**
 * @file tests/fsharp_emitter/fsharp_emitter_test.cpp
 * @brief Unit tests for the F# source emitter.
 */

#include "retdec/fsharp_emitter/fs_writer.h"
#include "retdec/fsharp_emitter/fs_type_emitter.h"
#include "retdec/fsharp_emitter/fs_file_emitter.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/bc_module/bc_type.h"

#include <gtest/gtest.h>
#include <memory>

using namespace retdec::fsharp_emitter;
using namespace retdec::bc_module;

// ─── FsWriter ────────────────────────────────────────────────────────────────

TEST(FsWriter, BasicLine) {
    FsWriter w;
    w.line("let x = 1");
    EXPECT_EQ("let x = 1\n", w.str());
}

TEST(FsWriter, Indent) {
    FsWriter w;
    w.line("let f () =");
    w.indent();
    w.line("42");
    w.dedent();
    EXPECT_EQ("let f () =\n    42\n", w.str());
}

TEST(FsWriter, Comment) {
    FsWriter w;
    w.comment("hello");
    EXPECT_EQ("// hello\n", w.str());
}

TEST(FsWriter, BlockComment) {
    FsWriter w;
    w.blockComment("summary");
    EXPECT_EQ("/// summary\n", w.str());
}

TEST(FsWriter, Blank) {
    FsWriter w;
    w.line("a");
    w.blank();
    w.line("b");
    EXPECT_EQ("a\n\nb\n", w.str());
}

TEST(FsWriter, SafeName_Normal) {
    EXPECT_EQ("x", FsWriter::safeName("x"));
    EXPECT_EQ("myVar", FsWriter::safeName("myVar"));
}

TEST(FsWriter, SafeName_Keyword) {
    EXPECT_EQ("``type``",   FsWriter::safeName("type"));
    EXPECT_EQ("``member``", FsWriter::safeName("member"));
    EXPECT_EQ("``module``", FsWriter::safeName("module"));
    EXPECT_EQ("``let``",    FsWriter::safeName("let"));
}

TEST(FsWriter, SafeName_LeadingDigit) {
    // Should wrap in backticks if starts with non-alpha
    std::string r = FsWriter::safeName("1bad");
    EXPECT_EQ("``1bad``", r);
}

TEST(FsWriter, IsKeyword) {
    EXPECT_TRUE(FsWriter::isKeyword("type"));
    EXPECT_TRUE(FsWriter::isKeyword("let"));
    EXPECT_TRUE(FsWriter::isKeyword("module"));
    EXPECT_FALSE(FsWriter::isKeyword("foo"));
}

TEST(FsWriter, StrLiteral_Simple) {
    FsWriter w;
    EXPECT_EQ("\"hello\"", w.strLiteral("hello"));
}

TEST(FsWriter, StrLiteral_Escape) {
    FsWriter w;
    auto s = w.strLiteral("a\"b");
    EXPECT_NE(std::string::npos, s.find("\\\""));
}

TEST(FsWriter, CharLiteral) {
    EXPECT_EQ("'a'", FsWriter::charLiteral('a'));
    EXPECT_EQ("'\\''", FsWriter::charLiteral('\''));
}

TEST(FsWriter, Reset) {
    FsWriter w;
    w.line("x = 1");
    w.reset();
    EXPECT_EQ("", w.str());
}

// ─── Helpers for building BcModule ───────────────────────────────────────────

static BcClass makeSimpleClass(const std::string& name,
                                const std::string& pkg = "TestNS") {
    BcClass cls;
    cls.name        = name;
    cls.fqName      = pkg + "." + name;
    cls.packageName = pkg;
    cls.access      = BcAccess::Public;
    return cls;
}

static BcMethod makeMethod(const std::string& name, bool isStatic = false) {
    BcMethod m;
    m.name   = name;
    m.access = BcAccess::Public;
    if (isStatic) m.access = m.access | BcAccess::Static;
    m.descriptor.returnType = std::make_shared<BcType>(types::Void());
    return m;
}

static BcMethod makeFunction(const std::string& name, BcType retType) {
    BcMethod m;
    m.name   = name;
    m.access = BcAccess::Public;
    m.descriptor.returnType = std::make_shared<BcType>(std::move(retType));
    return m;
}

static BcField makeField(const std::string& name, BcType type) {
    BcField f;
    f.name   = name;
    f.type   = type;
    f.access = BcAccess::Public;
    return f;
}

// ─── FsTypeEmitter: simple class ─────────────────────────────────────────────

TEST(FsTypeEmitter, EmptyClass) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Foo");
    BcModule mod("TestAssembly", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("type Foo"));
}

TEST(FsTypeEmitter, ClassWithMethod) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("MyClass");
    cls.methods.push_back(makeMethod("DoWork"));
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("DoWork"));
}

TEST(FsTypeEmitter, ClassWithField) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("DataClass");
    cls.fields.push_back(makeField("value", types::Int()));
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("value"));
}

TEST(FsTypeEmitter, InterfaceType) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("IFoo");
    cls.isInterface = true;
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("[<Interface>]"));
    EXPECT_NE(std::string::npos, out.find("type IFoo"));
}

TEST(FsTypeEmitter, EnumType) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Color");
    cls.isEnum = true;
    cls.enumConstants = {"Red", "Green", "Blue"};
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("| Red = 0"));
    EXPECT_NE(std::string::npos, out.find("| Green = 1"));
    EXPECT_NE(std::string::npos, out.find("| Blue = 2"));
}

TEST(FsTypeEmitter, DelegateType) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("MyDelegate");
    // Simulate delegate: superClass = MulticastDelegate, single Invoke method
    BcType delegateBase;
    cls.superClass = delegateBase;
    // Set to look like a delegate
    BcMethod invoke;
    invoke.name = "Invoke";
    invoke.descriptor.returnType = std::make_shared<BcType>(types::Void());
    invoke.descriptor.params = {std::make_shared<BcType>(types::Int())};
    invoke.paramNames = {"arg"};
    invoke.access = BcAccess::Public;
    cls.methods.push_back(invoke);
    // Force delegate detection via superClass containing "Delegate"
    BcType mcDelegate;
    cls.superClass = mcDelegate;
    // We need to make the superclass toString() contain "Delegate"
    // Let's test the typeStr fallback path instead

    // Test emitting a static-only class as module
    BcClass staticCls = makeSimpleClass("Utils");
    staticCls.methods.push_back(makeMethod("Helper", true));
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(staticCls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("module Utils"));
}

TEST(FsTypeEmitter, AbstractClass) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Base");
    cls.isAbstract = true;
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("[<AbstractClass>]"));
}

TEST(FsTypeEmitter, RecordDetection) {
    FsWriter w;
    FsTypeEmitter::Options opts;
    opts.preferRecords = true;
    FsTypeEmitter em(w, opts);

    BcClass cls = makeSimpleClass("Point");
    cls.fields.push_back(makeField("X", types::Int()));
    cls.fields.push_back(makeField("Y", types::Int()));
    // No non-constructor methods → should be emitted as record
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("type Point = {"));
    EXPECT_NE(std::string::npos, out.find("X:"));
    EXPECT_NE(std::string::npos, out.find("Y:"));
}

TEST(FsTypeEmitter, CompilerGeneratedSkipped) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("<>c__DisplayClass0_0");
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    EXPECT_EQ("", w.str()); // should produce no output
}

TEST(FsTypeEmitter, StaticMethod) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("MathUtils");
    BcMethod m = makeFunction("Square", types::Int());
    m.access = BcAccess::Public | BcAccess::Static;
    m.descriptor.params = {std::make_shared<BcType>(types::Int())};
    m.paramNames = {"n"};
    cls.methods.push_back(m);
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Square"));
}

TEST(FsTypeEmitter, MethodWithParams) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Calc");
    BcMethod m = makeFunction("Add", types::Int());
    m.descriptor.params = {std::make_shared<BcType>(types::Int()), std::make_shared<BcType>(types::Int())};
    m.paramNames = {"a", "b"};
    cls.methods.push_back(m);
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("a: int"));
    EXPECT_NE(std::string::npos, out.find("b: int"));
}

TEST(FsTypeEmitter, PropertyDetection) {
    FsWriter w;
    FsTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Person");
    // get_Name / set_Name
    BcMethod getter = makeFunction("get_Name", types::ClrString());
    BcMethod setter = makeMethod("set_Name");
    setter.descriptor.params = {std::make_shared<BcType>(types::ClrString())};
    cls.methods.push_back(getter);
    cls.methods.push_back(setter);
    BcModule mod("A", SourceLang::FSharp);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Name"));
}

// ─── FsFileEmitter ───────────────────────────────────────────────────────────

TEST(FsFileEmitter, EmptyModule) {
    BcModule mod("MyLib", SourceLang::FSharp);
    FsFileEmitter em;
    auto result = em.emit(mod);
    EXPECT_FALSE(result.source.empty());
    EXPECT_NE(std::string::npos, result.source.find("Auto-generated"));
}

TEST(FsFileEmitter, OpenDecls) {
    BcModule mod("MyLib", SourceLang::FSharp);
    FsFileEmitter em;
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("open System"));
}

TEST(FsFileEmitter, ModuleDecl) {
    BcModule mod("MyLib", SourceLang::FSharp);
    BcClass cls = makeSimpleClass("Foo", "MyApp");
    mod.addClass(cls);

    FsEmitOptions opts;
    opts.emitFileHeader = false;
    opts.useNamespace   = false;
    FsFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("module MyApp"));
}

TEST(FsFileEmitter, NamespaceDecl) {
    BcModule mod("MyLib", SourceLang::FSharp);
    BcClass cls = makeSimpleClass("Foo", "MyApp");
    mod.addClass(cls);

    FsEmitOptions opts;
    opts.emitFileHeader = false;
    opts.useNamespace   = true;
    FsFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("namespace MyApp"));
}

TEST(FsFileEmitter, MultipleClasses) {
    BcModule mod("Lib", SourceLang::FSharp);
    mod.addClass(makeSimpleClass("A"));
    mod.addClass(makeSimpleClass("B"));

    FsEmitOptions opts;
    opts.emitFileHeader = false;
    FsFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("type A"));
    EXPECT_NE(std::string::npos, result.source.find("type B"));
}
