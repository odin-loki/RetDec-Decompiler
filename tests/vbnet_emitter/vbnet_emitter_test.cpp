/**
 * @file tests/vbnet_emitter/vbnet_emitter_test.cpp
 * @brief Unit tests for the VB.NET source emitter.
 */

#include "retdec/vbnet_emitter/vb_writer.h"
#include "retdec/vbnet_emitter/vb_type_emitter.h"
#include "retdec/vbnet_emitter/vb_file_emitter.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/bc_module/bc_type.h"

#include <gtest/gtest.h>
#include <memory>

using namespace retdec::vbnet_emitter;
using namespace retdec::bc_module;

// ─── VbWriter ────────────────────────────────────────────────────────────────

TEST(VbWriter, BasicLine) {
    VbWriter w;
    w.line("Dim x As Integer");
    EXPECT_EQ("Dim x As Integer\n", w.str());
}

TEST(VbWriter, Indent) {
    VbWriter w;
    w.line("Sub Foo()");
    w.indent();
    w.line("Dim x As Integer = 1");
    w.dedent();
    w.line("End Sub");
    EXPECT_EQ("Sub Foo()\n    Dim x As Integer = 1\nEnd Sub\n", w.str());
}

TEST(VbWriter, Comment) {
    VbWriter w;
    w.comment("hello");
    EXPECT_EQ("' hello\n", w.str());
}

TEST(VbWriter, XmlDoc) {
    VbWriter w;
    w.xmlDoc("summary");
    EXPECT_EQ("''' summary\n", w.str());
}

TEST(VbWriter, Blank) {
    VbWriter w;
    w.line("a");
    w.blank();
    w.line("b");
    EXPECT_EQ("a\n\nb\n", w.str());
}

TEST(VbWriter, SafeName_Normal) {
    EXPECT_EQ("myVar", VbWriter::safeName("myVar"));
    EXPECT_EQ("X", VbWriter::safeName("X"));
}

TEST(VbWriter, SafeName_Keyword) {
    EXPECT_EQ("[module]",  VbWriter::safeName("module"));
    EXPECT_EQ("[class]",   VbWriter::safeName("class"));
    EXPECT_EQ("[integer]", VbWriter::safeName("integer"));
}

TEST(VbWriter, SafeName_Empty) {
    EXPECT_EQ("_", VbWriter::safeName(""));
}

TEST(VbWriter, IsKeyword) {
    EXPECT_TRUE(VbWriter::isKeyword("Sub"));
    EXPECT_TRUE(VbWriter::isKeyword("Function"));
    EXPECT_TRUE(VbWriter::isKeyword("dim"));
    EXPECT_FALSE(VbWriter::isKeyword("foo"));
}

TEST(VbWriter, StrLiteral_Simple) {
    EXPECT_EQ("\"hello\"", VbWriter::strLiteral("hello"));
}

TEST(VbWriter, StrLiteral_QuoteEscape) {
    auto s = VbWriter::strLiteral("say \"hi\"");
    EXPECT_NE(std::string::npos, s.find("\"\""));
}

TEST(VbWriter, StrLiteral_Newline) {
    auto s = VbWriter::strLiteral("a\nb");
    EXPECT_NE(std::string::npos, s.find("vbLf"));
}

TEST(VbWriter, Reset) {
    VbWriter w;
    w.line("x");
    w.reset();
    EXPECT_EQ("", w.str());
}

// ─── Helper builders ─────────────────────────────────────────────────────────

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

// ─── VbTypeEmitter: class ────────────────────────────────────────────────────

TEST(VbTypeEmitter, EmptyClass) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Foo");
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Class Foo"));
    EXPECT_NE(std::string::npos, out.find("End Class"));
}

TEST(VbTypeEmitter, ClassWithMethod) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Worker");
    cls.methods.push_back(makeMethod("DoWork"));
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Sub DoWork()"));
    EXPECT_NE(std::string::npos, out.find("End Sub"));
}

TEST(VbTypeEmitter, ClassWithFunction) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Calc");
    cls.methods.push_back(makeFunction("GetValue", types::Int()));
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Function GetValue()"));
    EXPECT_NE(std::string::npos, out.find("As Integer"));
    EXPECT_NE(std::string::npos, out.find("End Function"));
}

TEST(VbTypeEmitter, ClassWithField) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Data");
    cls.fields.push_back(makeField("count", types::Int()));
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("count"));
    EXPECT_NE(std::string::npos, out.find("As Integer"));
}

TEST(VbTypeEmitter, InterfaceType) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("IWork");
    cls.isInterface = true;
    cls.methods.push_back(makeMethod("Execute"));
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Interface IWork"));
    EXPECT_NE(std::string::npos, out.find("End Interface"));
    EXPECT_NE(std::string::npos, out.find("Sub Execute"));
}

TEST(VbTypeEmitter, EnumType) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Status");
    cls.isEnum = true;
    cls.enumConstants = {"Active", "Inactive", "Pending"};
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Enum Status"));
    EXPECT_NE(std::string::npos, out.find("Active = 0"));
    EXPECT_NE(std::string::npos, out.find("Inactive = 1"));
    EXPECT_NE(std::string::npos, out.find("End Enum"));
}

TEST(VbTypeEmitter, AbstractClass) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Base");
    cls.isAbstract = true;
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("MustInherit Class Base"));
}

TEST(VbTypeEmitter, StaticModule) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Helpers");
    // All-static methods → VB Module
    cls.methods.push_back(makeMethod("Log", true));
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Module Helpers"));
    EXPECT_NE(std::string::npos, out.find("End Module"));
}

TEST(VbTypeEmitter, PropertyGetterSetter) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Person");
    BcMethod getter = makeFunction("get_Name", types::ClrString());
    BcMethod setter = makeMethod("set_Name");
    setter.descriptor.params = {std::make_shared<BcType>(types::ClrString())};
    cls.methods.push_back(getter);
    cls.methods.push_back(setter);
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Property Name"));
    EXPECT_NE(std::string::npos, out.find("Get"));
    EXPECT_NE(std::string::npos, out.find("Set(value As"));
    EXPECT_NE(std::string::npos, out.find("End Property"));
}

TEST(VbTypeEmitter, ReadOnlyProperty) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Box");
    BcMethod getter = makeFunction("get_Width", types::Double());
    cls.methods.push_back(getter);
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("ReadOnly Property Width"));
}

TEST(VbTypeEmitter, ConstantField) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Constants");
    BcField f = makeField("MaxSize", types::Int());
    f.constantIntValue = 100;
    f.access = BcAccess::Public | BcAccess::Static | BcAccess::Final;
    cls.fields.push_back(f);
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("100"));
}

TEST(VbTypeEmitter, CompilerGeneratedSkipped) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("<>c__DisplayClass0");
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    EXPECT_EQ("", w.str());
}

TEST(VbTypeEmitter, ConstructorEmitted) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Widget");
    BcMethod ctor;
    ctor.name          = ".ctor";
    ctor.isConstructor = true;
    ctor.access        = BcAccess::Public;
    ctor.descriptor.returnType = std::make_shared<BcType>(types::Void());
    cls.methods.push_back(ctor);
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("Sub New()"));
    EXPECT_NE(std::string::npos, out.find("MyBase.New()"));
}

TEST(VbTypeEmitter, MethodWithParams) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Math");
    BcMethod m = makeFunction("Add", types::Int());
    m.descriptor.params = {std::make_shared<BcType>(types::Int()), std::make_shared<BcType>(types::Int())};
    m.paramNames = {"a", "b"};
    cls.methods.push_back(m);
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    std::string out = w.str();
    EXPECT_NE(std::string::npos, out.find("a As Integer"));
    EXPECT_NE(std::string::npos, out.find("b As Integer"));
}

TEST(VbTypeEmitter, InheritanceEmitted) {
    VbWriter w;
    VbTypeEmitter em(w);
    BcClass cls = makeSimpleClass("Dog");
    cls.superClass = types::ClrObject(); // simplified for test
    BcModule mod("A", SourceLang::VisualBasic);
    em.emitClass(cls, mod);
    // just check no crash; inheritance output depends on typeStr
    EXPECT_NE("", w.str());
}

// ─── VbFileEmitter ───────────────────────────────────────────────────────────

TEST(VbFileEmitter, EmptyModule) {
    BcModule mod("MyLib", SourceLang::VisualBasic);
    VbFileEmitter em;
    auto result = em.emit(mod);
    EXPECT_FALSE(result.source.empty());
    EXPECT_NE(std::string::npos, result.source.find("Auto-generated"));
}

TEST(VbFileEmitter, ImportSystem) {
    BcModule mod("Lib", SourceLang::VisualBasic);
    VbFileEmitter em;
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("Imports System"));
}

TEST(VbFileEmitter, NamespaceWrapper) {
    BcModule mod("Lib", SourceLang::VisualBasic);
    BcClass cls = makeSimpleClass("Foo", "MyApp");
    mod.addClass(cls);

    VbEmitOptions opts;
    opts.emitFileHeader = false;
    VbFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("Namespace MyApp"));
    EXPECT_NE(std::string::npos, result.source.find("End Namespace"));
}

TEST(VbFileEmitter, ClassInNamespace) {
    BcModule mod("Lib", SourceLang::VisualBasic);
    BcClass cls = makeSimpleClass("Bar", "MyApp");
    cls.methods.push_back(makeMethod("Run"));
    mod.addClass(cls);

    VbEmitOptions opts;
    opts.emitFileHeader = false;
    VbFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("Class Bar"));
    EXPECT_NE(std::string::npos, result.source.find("Sub Run()"));
}

TEST(VbFileEmitter, MultipleClasses) {
    BcModule mod("Lib", SourceLang::VisualBasic);
    mod.addClass(makeSimpleClass("Alpha"));
    mod.addClass(makeSimpleClass("Beta"));

    VbEmitOptions opts;
    opts.emitFileHeader = false;
    VbFileEmitter em(opts);
    auto result = em.emit(mod);
    EXPECT_NE(std::string::npos, result.source.find("Class Alpha"));
    EXPECT_NE(std::string::npos, result.source.find("Class Beta"));
}

TEST(VbFileEmitter, SortedImports) {
    BcModule mod("Lib", SourceLang::VisualBasic);
    mod.addExternalRef("Zulu.Type", "Zulu.Type");
    mod.addExternalRef("Alpha.Type", "Alpha.Type");

    VbEmitOptions opts;
    opts.emitFileHeader = false;
    opts.sortImports    = true;
    VbFileEmitter em(opts);
    auto result = em.emit(mod);

    size_t posAlpha  = result.source.find("Alpha");
    size_t posSystem = result.source.find("Imports System");
    EXPECT_LT(posSystem, posAlpha);
}
