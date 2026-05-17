/**
 * @file tests/cxx_backend/cxx_backend_test.cpp
 * @brief Unit tests for the Mixed C++/C output backend.
 */

#include "retdec/cxx_backend/cxx_ast.h"
#include "retdec/cxx_backend/cxx_emitter.h"
#include "retdec/cxx_backend/cxx_lifter.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace retdec::cxx_backend;
using namespace retdec::codegen;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::shared_ptr<CType> i32Type() { return CType::make(CType::Kind::Int32); }
static std::shared_ptr<CType> voidType(){ return CType::make(CType::Kind::Void); }
static std::shared_ptr<CType> ptrType(std::shared_ptr<CType> of) {
    return CType::ptr(std::move(of));
}

static CxxMethod makeMethod(const std::string& name, bool isVirtual = false,
                              bool isPure = false, bool isCtor = false,
                              bool isDtor = false) {
    CxxMethod m;
    m.name          = name;
    m.isVirtual     = isVirtual;
    m.isPureVirtual  = isPure;
    m.isConstructor = isCtor;
    m.isDestructor  = isDtor;
    m.returnType    = voidType();
    return m;
}

// ─── CxxCastKind tests ────────────────────────────────────────────────────────

TEST(CxxCastKindTest, Names) {
    EXPECT_EQ(cxxCastKindStr(CxxCastKind::Static),       "static_cast");
    EXPECT_EQ(cxxCastKindStr(CxxCastKind::Reinterpret),  "reinterpret_cast");
    EXPECT_EQ(cxxCastKindStr(CxxCastKind::Dynamic),      "dynamic_cast");
    EXPECT_EQ(cxxCastKindStr(CxxCastKind::Const),        "const_cast");
}

// ─── CxxNewExpr tests ────────────────────────────────────────────────────────

TEST(CxxNewExprTest, SimpleNew) {
    CxxNewExpr n;
    n.allocType = i32Type();
    n.isArray   = false;
    std::string s = n.toString();
    EXPECT_NE(s.find("new"), std::string::npos);
    EXPECT_NE(s.find("int32_t"), std::string::npos);
}

TEST(CxxNewExprTest, ArrayNew) {
    CxxNewExpr n;
    n.allocType = i32Type();
    n.isArray   = true;
    n.arraySize = CExpr::lit("10");
    std::string s = n.toString();
    EXPECT_NE(s.find("new"), std::string::npos);
    EXPECT_NE(s.find("[10]"), std::string::npos);
}

// ─── CxxDeleteExpr tests ─────────────────────────────────────────────────────

TEST(CxxDeleteExprTest, SimpleDelete) {
    CxxDeleteExpr d;
    d.ptr     = CExpr::var("p");
    d.isArray = false;
    EXPECT_EQ(d.toString(), "delete p");
}

TEST(CxxDeleteExprTest, ArrayDelete) {
    CxxDeleteExpr d;
    d.ptr     = CExpr::var("arr");
    d.isArray = true;
    EXPECT_EQ(d.toString(), "delete[] arr");
}

// ─── CxxCastExpr tests ───────────────────────────────────────────────────────

TEST(CxxCastExprTest, StaticCast) {
    CxxCastExpr c;
    c.castKind   = CxxCastKind::Static;
    c.targetType = ptrType(voidType());
    c.expr       = CExpr::var("x");
    std::string s = c.toString();
    EXPECT_NE(s.find("static_cast"), std::string::npos);
    EXPECT_NE(s.find("x"), std::string::npos);
}

TEST(CxxCastExprTest, ReinterpretCast) {
    CxxCastExpr c;
    c.castKind   = CxxCastKind::Reinterpret;
    c.targetType = i32Type();
    c.expr       = CExpr::var("ptr");
    std::string s = c.toString();
    EXPECT_NE(s.find("reinterpret_cast"), std::string::npos);
}

// ─── CxxThrowExpr tests ──────────────────────────────────────────────────────

TEST(CxxThrowExprTest, ThrowExpr) {
    CxxThrowExpr t;
    t.expr = CExpr::lit("42");
    EXPECT_EQ(t.toString(), "throw 42");
}

TEST(CxxThrowExprTest, Rethrow) {
    CxxThrowExpr t; // no expr
    EXPECT_EQ(t.toString(), "throw");
}

// ─── CxxScopeExpr tests ──────────────────────────────────────────────────────

TEST(CxxScopeExprTest, SimpleScope) {
    CxxScopeExpr s;
    s.scopes = {"std", "string"};
    EXPECT_EQ(s.toString(), "std::string");
}

TEST(CxxScopeExprTest, DeepScope) {
    CxxScopeExpr s;
    s.scopes = {"ns1", "ns2", "Class", "method"};
    EXPECT_EQ(s.toString(), "ns1::ns2::Class::method");
}

// ─── CxxMethodCallExpr tests ─────────────────────────────────────────────────

TEST(CxxMethodCallExprTest, DotCall) {
    CxxMethodCallExpr mc;
    mc.object      = CExpr::var("obj");
    mc.methodName  = "foo";
    mc.arrowAccess = false;
    std::string s = mc.toString();
    EXPECT_NE(s.find("obj.foo()"), std::string::npos);
}

TEST(CxxMethodCallExprTest, ArrowCall) {
    CxxMethodCallExpr mc;
    mc.object      = CExpr::var("ptr");
    mc.methodName  = "bar";
    mc.arrowAccess = true;
    mc.args.push_back(CExpr::lit("1"));
    mc.args.push_back(CExpr::lit("2"));
    std::string s = mc.toString();
    EXPECT_NE(s.find("ptr->bar(1, 2)"), std::string::npos);
}

// ─── CxxEnum tests ───────────────────────────────────────────────────────────

TEST(CxxEnumTest, BasicEnum) {
    CxxEnum en;
    en.name    = "Color";
    en.isClass = true;
    en.enumerators = {{"Red", 0}, {"Green", 1}, {"Blue", 2}};
    std::string s = en.toString();
    EXPECT_NE(s.find("enum class Color"), std::string::npos);
    EXPECT_NE(s.find("Red = 0"),  std::string::npos);
    EXPECT_NE(s.find("Green = 1"),std::string::npos);
    EXPECT_NE(s.find("Blue = 2"), std::string::npos);
}

TEST(CxxEnumTest, EnumWithUnderlying) {
    CxxEnum en;
    en.name       = "Flags";
    en.underlying = "uint8_t";
    en.isClass    = true;
    en.enumerators = {{"A", std::nullopt}};
    std::string s = en.toString();
    EXPECT_NE(s.find(": uint8_t"), std::string::npos);
}

TEST(CxxEnumTest, PlainEnum) {
    CxxEnum en;
    en.name    = "Mode";
    en.isClass = false;
    en.enumerators = {{"ON", 1}, {"OFF", 0}};
    std::string s = en.toString();
    EXPECT_NE(s.find("enum Mode"), std::string::npos);
    EXPECT_EQ(s.find("enum class"), std::string::npos);
}

// ─── CxxUsing tests ──────────────────────────────────────────────────────────

TEST(CxxUsingTest, TypeAlias) {
    CxxUsing u;
    u.name   = "MyInt";
    u.target = "int32_t";
    EXPECT_EQ(u.toString(), "using MyInt = int32_t;");
}

TEST(CxxUsingTest, UsingNamespace) {
    CxxUsing u;
    u.name        = "std";
    u.isNamespace = true;
    EXPECT_EQ(u.toString(), "using namespace std;");
}

// ─── CxxTemplate tests ───────────────────────────────────────────────────────

TEST(CxxTemplateTest, SingleTypenameParam) {
    CxxTemplate t;
    CxxTemplate::Param p;
    p.kind = CxxTemplate::Param::Kind::Typename;
    p.name = "T";
    t.params.push_back(p);
    std::string s = t.paramStr();
    EXPECT_NE(s.find("template<"), std::string::npos);
    EXPECT_NE(s.find("typename T"), std::string::npos);
}

TEST(CxxTemplateTest, MultipleParams) {
    CxxTemplate t;
    CxxTemplate::Param p1;
    p1.kind = CxxTemplate::Param::Kind::Typename; p1.name = "T";
    CxxTemplate::Param p2;
    p2.kind = CxxTemplate::Param::Kind::Typename; p2.name = "U";
    t.params = {p1, p2};
    std::string s = t.paramStr();
    EXPECT_NE(s.find("T"), std::string::npos);
    EXPECT_NE(s.find("U"), std::string::npos);
}

// ─── CxxClass isAbstract tests ────────────────────────────────────────────────

TEST(CxxClassTest, NotAbstract) {
    CxxClass cls;
    cls.name = "Foo";
    cls.methods.push_back(makeMethod("doSomething", true, false));
    EXPECT_FALSE(cls.isAbstract());
}

TEST(CxxClassTest, IsAbstract) {
    CxxClass cls;
    cls.name = "IFoo";
    cls.methods.push_back(makeMethod("pureMethod", true, true));
    EXPECT_TRUE(cls.isAbstract());
}

// ─── CxxMethod toString tests ─────────────────────────────────────────────────

TEST(CxxMethodTest, VirtualMethod) {
    CxxMethod m = makeMethod("draw", true, false);
    m.isConst = true;
    CxxEmitter em;
    std::string s = em.emitMethod(m, 1);
    EXPECT_NE(s.find("virtual"), std::string::npos);
    EXPECT_NE(s.find("draw"), std::string::npos);
    EXPECT_NE(s.find("const"), std::string::npos);
}

TEST(CxxMethodTest, PureVirtualMethod) {
    CxxMethod m = makeMethod("render", true, true);
    CxxEmitter em;
    std::string s = em.emitMethod(m, 1);
    EXPECT_NE(s.find("virtual"), std::string::npos);
    EXPECT_NE(s.find("= 0"), std::string::npos);
}

TEST(CxxMethodTest, Destructor) {
    CxxMethod m = makeMethod("MyClass", false, false, false, true);
    CxxEmitter em;
    std::string s = em.emitMethod(m, 1);
    EXPECT_NE(s.find("~"), std::string::npos);
}

TEST(CxxMethodTest, Constructor) {
    CxxMethod m = makeMethod("MyClass", false, false, true, false);
    CxxEmitter em;
    std::string s = em.emitMethod(m, 1);
    EXPECT_NE(s.find("MyClass"), std::string::npos);
    // No return type prefix
    EXPECT_EQ(s.find("void MyClass"), std::string::npos);
}

// ─── CxxEmitter::emitClass tests ─────────────────────────────────────────────

TEST(CxxEmitterTest, EmitsClassKeyword) {
    CxxClass cls;
    cls.name = "Animal";
    cls.kind = CxxClass::Kind::Class;
    CxxEmitter em;
    std::string s = em.emitClass(cls, 0);
    EXPECT_NE(s.find("class Animal"), std::string::npos);
    EXPECT_NE(s.find("{"), std::string::npos);
    EXPECT_NE(s.find("};"), std::string::npos);
}

TEST(CxxEmitterTest, EmitsStructKeyword) {
    CxxClass cls;
    cls.name = "Point";
    cls.kind = CxxClass::Kind::Struct;
    CxxEmitter em;
    std::string s = em.emitClass(cls, 0);
    EXPECT_NE(s.find("struct Point"), std::string::npos);
}

TEST(CxxEmitterTest, EmitsBaseClass) {
    CxxClass cls;
    cls.name = "Dog";
    cls.kind = CxxClass::Kind::Class;
    CxxClass::BaseClass base;
    base.name   = "Animal";
    base.access = "public";
    cls.bases.push_back(base);
    CxxEmitter em;
    std::string s = em.emitClass(cls, 0);
    EXPECT_NE(s.find("public Animal"), std::string::npos);
}

TEST(CxxEmitterTest, EmitsField) {
    CxxClass cls;
    cls.name = "Point";
    cls.kind = CxxClass::Kind::Struct;
    CxxClass::Field fx; fx.name = "x"; fx.type = i32Type(); fx.access = "public";
    CxxClass::Field fy; fy.name = "y"; fy.type = i32Type(); fy.access = "public";
    cls.fields = {fx, fy};
    CxxEmitter em;
    std::string s = em.emitClass(cls, 0);
    EXPECT_NE(s.find("x"), std::string::npos);
    EXPECT_NE(s.find("y"), std::string::npos);
}

// ─── CxxEmitter::emitTry tests ───────────────────────────────────────────────

TEST(CxxEmitterTest, EmitsTryBlock) {
    CxxTryStmt t;
    t.tryBody = CStmt::block();
    CxxCatchClause c;
    c.exceptionType = nullptr; // catch(...)
    c.body          = CStmt::block();
    t.catches.push_back(c);

    CxxEmitter em;
    std::string s = em.emitTry(t, 0);
    EXPECT_NE(s.find("try"), std::string::npos);
    EXPECT_NE(s.find("catch"), std::string::npos);
    EXPECT_NE(s.find("..."), std::string::npos);
}

TEST(CxxEmitterTest, EmitsTryCatchTyped) {
    CxxTryStmt t;
    t.tryBody = CStmt::block();
    CxxCatchClause c;
    c.exceptionType = CType::make(CType::Kind::Struct);
    c.exceptionType->name = "std::exception";
    c.varName = "e";
    c.body    = CStmt::block();
    t.catches.push_back(c);

    CxxEmitter em;
    std::string s = em.emitTry(t, 0);
    EXPECT_NE(s.find("catch"), std::string::npos);
    EXPECT_NE(s.find("e"), std::string::npos);
}

// ─── CxxEmitter::emitEnum tests ──────────────────────────────────────────────

TEST(CxxEmitterTest, EmitsEnumClass) {
    CxxEnum en;
    en.name    = "Dir";
    en.isClass = true;
    en.enumerators = {{"North", 0}, {"South", 1}};
    CxxEmitter em;
    std::string s = em.emitEnum(en, 0);
    EXPECT_NE(s.find("enum class Dir"), std::string::npos);
    EXPECT_NE(s.find("North = 0"),  std::string::npos);
}

// ─── CxxEmitter::emitNamespace tests ─────────────────────────────────────────

TEST(CxxEmitterTest, EmitsNamespace) {
    CxxNamespace ns;
    ns.name     = "mylib";
    ns.contents = "    int x;\n";
    CxxEmitter em;
    std::string s = em.emitNamespace(ns, 0);
    EXPECT_NE(s.find("namespace mylib"), std::string::npos);
    EXPECT_NE(s.find("int x"), std::string::npos);
}

TEST(CxxEmitterTest, EmitsAnonymousNamespace) {
    CxxNamespace ns;
    ns.name     = "";
    ns.contents = "";
    CxxEmitter em;
    std::string s = em.emitNamespace(ns, 0);
    EXPECT_NE(s.find("namespace {"), std::string::npos);
}

// ─── CxxEmitter::emitUnit tests ──────────────────────────────────────────────

TEST(CxxEmitterTest, EmitsUnitHeader) {
    CxxUnit unit;
    unit.isCxx = true;
    CxxEmitter em;
    std::string s = em.emitUnit(unit);
    EXPECT_NE(s.find("Generated by RetDec"), std::string::npos);
    EXPECT_NE(s.find(".cpp"), std::string::npos);
}

TEST(CxxEmitterTest, EmitsCUnitHeader) {
    CxxUnit unit;
    unit.isCxx = false;
    CxxEmitter em;
    std::string s = em.emitUnit(unit);
    EXPECT_NE(s.find(".c"), std::string::npos);
}

TEST(CxxEmitterTest, EmitsUsings) {
    CxxUnit unit;
    unit.isCxx = true;
    CxxUsing u; u.name = "std"; u.isNamespace = true;
    unit.usings.push_back(u);
    CxxEmitter em;
    std::string s = em.emitUnit(unit);
    EXPECT_NE(s.find("using namespace std"), std::string::npos);
}

TEST(CxxEmitterTest, EmitsSystemIncludes) {
    CxxUnit unit;
    unit.isCxx = true;
    unit.systemIncludes = {"cstdint", "memory"};
    CxxEmitter em;
    std::string s = em.emitUnit(unit);
    EXPECT_NE(s.find("#include <cstdint>"), std::string::npos);
    EXPECT_NE(s.find("#include <memory>"), std::string::npos);
}

// ─── CxxLifter tests ─────────────────────────────────────────────────────────

TEST(CxxLifterTest, PlainCPassthrough) {
    CUnit cunit;
    cunit.filename = "test.c";
    CxxLiftContext ctx;
    ctx.hasCxxEvidence = false;

    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    EXPECT_FALSE(result.isCxx);
    EXPECT_FALSE(result.unit.isCxx);
}

TEST(CxxLifterTest, VtableEvidenceMakesCxx) {
    CUnit cunit;
    CxxLiftContext ctx;
    VtableEntry vt;
    vt.className = "Foo";
    vt.virtualFunctions = {"doWork"};
    ctx.vtables.push_back(vt);

    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    EXPECT_TRUE(result.isCxx);
    ASSERT_FALSE(result.unit.classes.empty());
    EXPECT_EQ(result.unit.classes[0].name, "Foo");
}

TEST(CxxLifterTest, VtableVirtualMethods) {
    CUnit cunit;
    CxxLiftContext ctx;
    VtableEntry vt;
    vt.className = "Base";
    vt.virtualFunctions = {"update", "render"};
    ctx.vtables.push_back(vt);

    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    ASSERT_EQ(result.unit.classes.size(), 1u);
    EXPECT_EQ(result.unit.classes[0].methods.size(), 2u);
    EXPECT_TRUE(result.unit.classes[0].methods[0].isVirtual);
}

TEST(CxxLifterTest, MangleDetectsCtor) {
    CUnit cunit;
    CFunction fn;
    fn.name       = "_ZN3FooC1Ev";
    fn.returnType = voidType();
    fn.body       = CStmt::block();
    cunit.functions.push_back(fn);

    CxxLiftContext ctx;
    ctx.hasCxxEvidence = true;

    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    EXPECT_TRUE(result.isCxx);
}

TEST(CxxLifterTest, MangleDetectsDtor) {
    CUnit cunit;
    CFunction fn;
    fn.name       = "_ZN3FooD1Ev";
    fn.returnType = voidType();
    fn.body       = CStmt::block();
    cunit.functions.push_back(fn);

    CxxLiftContext ctx;
    ctx.hasCxxEvidence = true;

    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    EXPECT_TRUE(result.isCxx);
}

TEST(CxxLifterTest, EhEvidenceMakesCxx) {
    CUnit cunit;
    CxxLiftContext ctx;
    ctx.ehRegions["myFunc"] = {EhRegion{0, 100, "std::exception", "e", 200}};

    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    EXPECT_TRUE(result.isCxx);
}

TEST(CxxLifterTest, FunctionsPassedThrough) {
    CUnit cunit;
    CFunction fn;
    fn.name       = "hello";
    fn.returnType = voidType();
    fn.body       = CStmt::block();
    cunit.functions.push_back(fn);

    CxxLiftContext ctx;
    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    ASSERT_EQ(result.unit.functions.size(), 1u);
    EXPECT_EQ(result.unit.functions[0].name, "hello");
}

TEST(CxxLifterTest, IncludesPopulated) {
    CUnit cunit;
    CxxLiftContext ctx;
    ctx.hasCxxEvidence = true;

    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    EXPECT_FALSE(result.unit.systemIncludes.empty());
}

TEST(CxxLifterTest, MallocDetectedAsNewHint) {
    CUnit cunit;
    CFunction fn;
    fn.name = "createObj";
    fn.returnType = ptrType(voidType());
    fn.body = CStmt::block();
    // Simulate malloc call: assign = malloc(...)
    auto mallocCall = CExpr::call("malloc", {CExpr::lit("sizeof(Foo)")});
    auto stmt = CStmt::assign(CExpr::var("p"), mallocCall);
    fn.body->children.push_back(stmt);
    cunit.functions.push_back(fn);

    CxxLiftContext ctx;
    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    // malloc detection should produce a replacement annotation
    ASSERT_EQ(result.unit.functions.size(), 1u);
    // The function body should have grown (comment inserted)
    EXPECT_GE(result.unit.functions[0].body->children.size(), 1u);
}

TEST(CxxLifterTest, FilenamePreserved) {
    CUnit cunit;
    cunit.filename = "myprogram.c";
    CxxLiftContext ctx;
    CxxLifter lifter;
    auto result = lifter.lift(cunit, ctx);
    EXPECT_EQ(result.unit.filename, "myprogram.c");
}

TEST(CxxLifterTest, NamespaceGrouping) {
    CUnit cunit;
    CxxLiftContext ctx;
    ctx.hasCxxEvidence = true;
    VtableEntry vt;
    vt.className = "Widget";
    ctx.vtables.push_back(vt);
    ctx.demangledNames["Widget"] = "ui::Widget";

    CxxLiftOptions opts;
    opts.groupNamespaces = true;
    opts.detectVtables   = true;
    CxxLifter lifter(opts);
    auto result = lifter.lift(cunit, ctx);
    // Should have at least one namespace
    EXPECT_FALSE(result.unit.namespaces.empty());
}

TEST(CxxLifterTest, DisableNewDeleteRecovery) {
    CUnit cunit;
    CFunction fn;
    fn.name = "createObj";
    fn.returnType = ptrType(voidType());
    fn.body = CStmt::block();
    auto mallocCall = CExpr::call("malloc", {CExpr::lit("4")});
    fn.body->children.push_back(CStmt::assign(CExpr::var("p"), mallocCall));
    cunit.functions.push_back(fn);

    CxxLiftContext ctx;
    CxxLiftOptions opts;
    opts.recoverNewDelete = false;
    CxxLifter lifter(opts);
    auto result = lifter.lift(cunit, ctx);
    // Without recovery, no comment should be inserted
    EXPECT_EQ(result.unit.functions[0].body->children.size(), 1u);
}
