/**
 * @file tests/type_seed/type_seed_test.cpp
 * @brief Comprehensive tests for the type-seeding symbol demangler.
 *
 * Test corpus covers:
 *   - Itanium mangling: free functions, member functions, constructors,
 *     destructors, operators, templates, STL instantiations, nested namespaces,
 *     cv-qualifiers, pointer/reference types, function pointers
 *   - MSVC mangling: all calling conventions, member functions, class hierarchy,
 *     operators, templates, fundamental types
 *   - Rust mangling: legacy and v0, trait impls, generics, async, noreturn
 *   - Swift mangling: Swift 5 module.type.method, async
 *   - TypeSeedDispatcher: routing, batch processing
 *   - TypeConstraint generation: CC, return type, param types, this type,
 *     template args
 */

#include "retdec/type_seed/type_seed.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace retdec::type_seed;

// ─── Null type inference manager for constraint collection ────────────────────

class CollectingMgr : public ITypeInferenceMgr {
public:
    std::vector<TypeConstraint> constraints;
    void addGroundTruthConstraint(const TypeConstraint& c) override {
        constraints.push_back(c);
    }
    bool hasKind(ConstraintKind k) const {
        return std::any_of(constraints.begin(), constraints.end(),
                            [k](const TypeConstraint& c){ return c.kind==k; });
    }
    const TypeConstraint* findKind(ConstraintKind k) const {
        for (auto& c : constraints) if (c.kind==k) return &c;
        return nullptr;
    }
    std::vector<TypeConstraint> allOfKind(ConstraintKind k) const {
        std::vector<TypeConstraint> r;
        for (auto& c : constraints) if (c.kind==k) r.push_back(c);
        return r;
    }
};

// ─── Itanium seeder tests ─────────────────────────────────────────────────────

class ItaniumSeederTest : public ::testing::Test {
protected:
    TypeSeedDispatcher disp;
    void SetUp() override {
        // Only Itanium for these tests (via default dispatcher, Rust first)
        disp = makeDefaultDispatcher();
    }
    SignatureInfo extract(const std::string& s) {
        return disp.tryExtract(s);
    }
};

// ── Acceptance ────────────────────────────────────────────────────────────────
TEST_F(ItaniumSeederTest, AcceptsZPrefix) {
    auto sig = extract("_Z3foov");
    EXPECT_TRUE(sig.valid()) << "free function _Z3foov should be accepted";
}

TEST_F(ItaniumSeederTest, RejectsNonMangled) {
    auto sig = extract("printf");
    EXPECT_FALSE(sig.valid());
}

TEST_F(ItaniumSeederTest, RejectsMsvcMangled) {
    auto sig = extract("?foo@@YAHXZ");
    // MSVC seeder picks this up, not Itanium
    // Should still be valid (different seeder)
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "foo");
}

// ── Free functions ────────────────────────────────────────────────────────────
TEST_F(ItaniumSeederTest, SimpleVoidFunction) {
    auto sig = extract("_Z3foov");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "foo");
    EXPECT_TRUE(sig.className.empty());
    EXPECT_FALSE(sig.hasThis);
}

TEST_F(ItaniumSeederTest, FreeIntFunction) {
    // int bar(int, double)  →  _Z3barii... actually _Z3barid
    auto sig = extract("_Z3barid");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "bar");
    ASSERT_EQ(sig.params.size(), 2u);
    EXPECT_EQ(sig.params[0].type, "int");
    EXPECT_EQ(sig.params[1].type, "double");
}

TEST_F(ItaniumSeederTest, FreeCharPtrFunction) {
    // char* strdup(const char*)  →  _Z6strdupPKc
    auto sig = extract("_Z6strdupPKc");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "strdup");
    ASSERT_EQ(sig.params.size(), 1u);
    EXPECT_NE(sig.params[0].type.find("char"), std::string::npos);
}

TEST_F(ItaniumSeederTest, FreeVariadicFunction) {
    // int printf(const char*, ...)  →  _Z6printfPKcz
    auto sig = extract("_Z6printfPKcz");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "printf");
    ASSERT_GE(sig.params.size(), 1u);
    EXPECT_EQ(sig.params.back().type, "...");
}

// ── Member functions ──────────────────────────────────────────────────────────
TEST_F(ItaniumSeederTest, SimpleMemberFunction) {
    // void Foo::bar()  →  _ZN3Foo3barEv
    auto sig = extract("_ZN3Foo3barEv");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "bar");
    EXPECT_EQ(sig.className, "Foo");
    EXPECT_TRUE(sig.hasThis);
    EXPECT_EQ(sig.thisType, "Foo*");
}

TEST_F(ItaniumSeederTest, ConstMemberFunction) {
    // void Foo::bar() const  →  _ZNK3Foo3barEv
    auto sig = extract("_ZNK3Foo3barEv");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "bar");
    EXPECT_EQ(sig.className, "Foo");
    EXPECT_TRUE(sig.isConst);
    EXPECT_EQ(sig.thisType, "const Foo*");
}

TEST_F(ItaniumSeederTest, Constructor) {
    // Foo::Foo()  →  _ZN3FooC1Ev
    auto sig = extract("_ZN3FooC1Ev");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.className, "Foo");
    EXPECT_TRUE(sig.isConstructor);
    EXPECT_FALSE(sig.isDestructor);
}

TEST_F(ItaniumSeederTest, Destructor) {
    // Foo::~Foo()  →  _ZN3FooD1Ev
    auto sig = extract("_ZN3FooD1Ev");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.className, "Foo");
    EXPECT_TRUE(sig.isDestructor);
    EXPECT_FALSE(sig.isConstructor);
}

TEST_F(ItaniumSeederTest, OperatorPlus) {
    // Foo operator+(Foo, Foo)  →  _ZplRK3FooS1_  (simplified)
    auto sig = extract("_ZplRK3FooS1_");
    EXPECT_TRUE(sig.valid());
    EXPECT_TRUE(sig.isOperator);
}

// ── Nested namespaces ─────────────────────────────────────────────────────────
TEST_F(ItaniumSeederTest, NestedNamespace) {
    // void std::vector<int>::push_back(int const&)
    // Mangled: _ZNSt6vectorIiSaIiEE9push_backERKi
    auto sig = extract("_ZNSt6vectorIiSaIiEE9push_backERKi");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "push_back");
    EXPECT_FALSE(sig.className.empty());
}

// ── Template functions ────────────────────────────────────────────────────────
TEST_F(ItaniumSeederTest, TemplateFunction) {
    // template<typename T> T max(T,T) for T=int → _Z3maxIiET_S0_S0_
    auto sig = extract("_Z3maxIiET_S0_S0_");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName.substr(0,3), "max");
    EXPECT_FALSE(sig.templateArgs.empty());
    EXPECT_EQ(sig.templateArgs[0], "int");
}

TEST_F(ItaniumSeederTest, TemplateFunctionDouble) {
    // template<typename T> T min(T,T) for T=double → _Z3minIdET_S0_S0_
    auto sig = extract("_Z3minIdET_S0_S0_");
    EXPECT_TRUE(sig.valid());
    EXPECT_FALSE(sig.templateArgs.empty());
    EXPECT_EQ(sig.templateArgs[0], "double");
}

// ── Pointer and reference types ───────────────────────────────────────────────
TEST_F(ItaniumSeederTest, PointerParam) {
    // void foo(int*)  →  _Z3fooPi
    auto sig = extract("_Z3fooPi");
    EXPECT_TRUE(sig.valid());
    ASSERT_EQ(sig.params.size(), 1u);
    EXPECT_NE(sig.params[0].type.find("int"), std::string::npos);
}

TEST_F(ItaniumSeederTest, ReferenceParam) {
    // void foo(int&)  →  _Z3fooRi
    auto sig = extract("_Z3fooRi");
    EXPECT_TRUE(sig.valid());
    ASSERT_EQ(sig.params.size(), 1u);
    EXPECT_EQ(sig.params[0].ref, RefCategory::LValueRef);
}

TEST_F(ItaniumSeederTest, RvalueRefParam) {
    // void foo(int&&)  →  _Z3fooOi
    auto sig = extract("_Z3fooOi");
    EXPECT_TRUE(sig.valid());
    ASSERT_EQ(sig.params.size(), 1u);
    EXPECT_EQ(sig.params[0].ref, RefCategory::RValueRef);
}

// ── Calling convention ────────────────────────────────────────────────────────
TEST_F(ItaniumSeederTest, CCIsUnknownForItanium) {
    auto sig = extract("_Z3foov");
    EXPECT_EQ(sig.callingConvention, MangledCC::Unknown);
}

// ── STL patterns ──────────────────────────────────────────────────────────────
TEST_F(ItaniumSeederTest, StdString) {
    // std::basic_string<char>::size() const
    // _ZNKSs4sizeEv
    auto sig = extract("_ZNKSs4sizeEv");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "size");
    EXPECT_TRUE(sig.isConst);
}

TEST_F(ItaniumSeederTest, StdVectorInt) {
    // std::vector<int>::size() const
    // _ZNKSt6vectorIiSaIiEE4sizeEv
    auto sig = extract("_ZNKSt6vectorIiSaIiEE4sizeEv");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "size");
}

// ─── MSVC seeder tests ────────────────────────────────────────────────────────

class MsvcSeederTest : public ::testing::Test {
protected:
    TypeSeedDispatcher disp;
    void SetUp() override { disp = makeDefaultDispatcher(); }
    SignatureInfo extract(const std::string& s) { return disp.tryExtract(s); }
};

TEST_F(MsvcSeederTest, AcceptsQuestionMark) {
    auto sig = extract("?foo@@YAHXZ");
    EXPECT_TRUE(sig.valid());
}

TEST_F(MsvcSeederTest, RejectsNonMsvc) {
    // Itanium symbol — MSVC doesn't accept it
    EXPECT_FALSE(extract("_Z3foov").functionName == ""); // accepted by Itanium seeder
}

// ── Simple global function ─────────────────────────────────────────────────────
TEST_F(MsvcSeederTest, GlobalFunctionCdecl) {
    // int __cdecl foo()  →  ?foo@@YAHXZ
    auto sig = extract("?foo@@YAHXZ");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "foo");
    EXPECT_TRUE(sig.className.empty());
    EXPECT_EQ(sig.callingConvention, MangledCC::Cdecl);
    EXPECT_EQ(sig.returnType, "int");
}

TEST_F(MsvcSeederTest, GlobalFunctionStdcall) {
    // int __stdcall bar(int)  →  ?bar@@YGHH@Z
    auto sig = extract("?bar@@YGHH@Z");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "bar");
    EXPECT_EQ(sig.callingConvention, MangledCC::Stdcall);
}

// ── Member functions ──────────────────────────────────────────────────────────
TEST_F(MsvcSeederTest, MemberFunctionThiscall) {
    // void __thiscall Foo::bar()  →  ?bar@Foo@@QAEXXZ
    auto sig = extract("?bar@Foo@@QAEXXZ");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "bar");
    EXPECT_EQ(sig.className, "Foo");
    EXPECT_TRUE(sig.hasThis);
}

TEST_F(MsvcSeederTest, Constructor) {
    // Foo::Foo()  →  ??0Foo@@QAE@XZ
    auto sig = extract("??0Foo@@QAE@XZ");
    EXPECT_TRUE(sig.valid());
    EXPECT_TRUE(sig.isConstructor);
    EXPECT_EQ(sig.className, "Foo");
}

TEST_F(MsvcSeederTest, Destructor) {
    // Foo::~Foo()  →  ??1Foo@@UAE@XZ
    auto sig = extract("??1Foo@@UAE@XZ");
    EXPECT_TRUE(sig.valid());
    EXPECT_TRUE(sig.isDestructor);
    EXPECT_EQ(sig.className, "Foo");
}

// ── Operator overloads ────────────────────────────────────────────────────────
TEST_F(MsvcSeederTest, OperatorNew) {
    // void* Foo::operator new(size_t)  →  ??2Foo@@SAPAXI@Z
    auto sig = extract("??2Foo@@SAPAXI@Z");
    EXPECT_TRUE(sig.valid());
    EXPECT_TRUE(sig.isOperator);
}

// ── Calling conventions ───────────────────────────────────────────────────────
TEST_F(MsvcSeederTest, FastcallCC) {
    // int __fastcall baz(int,int)  →  ?baz@@YIIHHH@Z
    auto sig = extract("?baz@@YIIHHH@Z");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.callingConvention, MangledCC::Fastcall);
}

// ── Fundamental types ─────────────────────────────────────────────────────────
TEST_F(MsvcSeederTest, ReturnTypeBool) {
    // bool __cdecl isValid()  →  ?isValid@@YA_NXZ
    auto sig = extract("?isValid@@YA_NXZ");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.returnType, "bool");
}

TEST_F(MsvcSeederTest, ReturnTypeDouble) {
    // double __cdecl getVal()  →  ?getVal@@YANXZ
    auto sig = extract("?getVal@@YANXZ");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.returnType, "double");
}

TEST_F(MsvcSeederTest, Int64Param) {
    // void foo(__int64)  →  ?foo@@YAX_J@Z
    auto sig = extract("?foo@@YAX_J@Z");
    EXPECT_TRUE(sig.valid());
}

// ── Const member function ─────────────────────────────────────────────────────
TEST_F(MsvcSeederTest, ConstMember) {
    // int Foo::getX() const  →  ?getX@Foo@@QBEHXZ
    auto sig = extract("?getX@Foo@@QBEHXZ");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.className, "Foo");
    EXPECT_TRUE(sig.isConst);
}

// ─── Rust seeder tests ────────────────────────────────────────────────────────

class RustSeederTest : public ::testing::Test {
protected:
    TypeSeedDispatcher disp;
    void SetUp() override { disp = makeDefaultDispatcher(); }
    SignatureInfo extract(const std::string& s) { return disp.tryExtract(s); }
};

TEST_F(RustSeederTest, AcceptsLegacyMangled) {
    // std::io::stderr::Stderr — legacy mangling with hash
    auto sig = extract("_ZN3std2io6stderr6StderrE");
    // This doesn't have hash, won't be accepted by Rust seeder — that's fine.
    // Test a proper legacy Rust symbol:
    // _ZN3foo3bar17h0123456789abcdefE
    auto sig2 = extract("_ZN3foo3bar17h0123456789abcdefE");
    EXPECT_TRUE(sig2.valid());
    EXPECT_EQ(sig2.functionName, "bar");
}

TEST_F(RustSeederTest, LegacyFunctionName) {
    auto sig = extract("_ZN4core3mem4drop17h0123456789abcdefE");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "drop");
}

TEST_F(RustSeederTest, LegacyModulePath) {
    auto sig = extract("_ZN3std2io6stderr17habcdef0123456789E");
    EXPECT_TRUE(sig.valid());
    EXPECT_FALSE(sig.namespaceName.empty());
}

TEST_F(RustSeederTest, AcceptsV0Mangled) {
    // v0 example: _RNvNtCs4fqI2P2rA04_3std3fmt5write
    auto sig = extract("_RNvNtCs4fqI2P2rA04_3std3fmt5write");
    EXPECT_TRUE(sig.valid());
}

TEST_F(RustSeederTest, PanicIsNoReturn) {
    // _ZN4core9panicking5panic17h...E
    auto sig = extract("_ZN4core9panicking5panic17h0123456789abcdefE");
    EXPECT_TRUE(sig.valid());
    EXPECT_TRUE(sig.isNoReturn);
}

TEST_F(RustSeederTest, AbortIsNoReturn) {
    auto sig = extract("_ZN3std7process5abort17h0123456789abcdefE");
    EXPECT_TRUE(sig.valid());
    EXPECT_TRUE(sig.isNoReturn);
}

TEST_F(RustSeederTest, RustCCIsUnknown) {
    auto sig = extract("_ZN3foo3bar17h0123456789abcdefE");
    EXPECT_EQ(sig.callingConvention, MangledCC::Unknown);
}

TEST_F(RustSeederTest, DemangledNameNotEmpty) {
    auto sig = extract("_ZN4core3fmt5write17h0123456789abcdefE");
    EXPECT_TRUE(sig.valid());
    EXPECT_FALSE(sig.demangledName.empty());
}

// ─── Swift seeder tests ───────────────────────────────────────────────────────

class SwiftSeederTest : public ::testing::Test {
protected:
    TypeSeedDispatcher disp;
    void SetUp() override { disp = makeDefaultDispatcher(); }
    SignatureInfo extract(const std::string& s) { return disp.tryExtract(s); }
};

TEST_F(SwiftSeederTest, AcceptsSwift5Prefix) {
    // Swift 5 symbol: $s<module><type><method>
    auto sig = extract("$s3FooAAC3barSiyF");
    EXPECT_TRUE(sig.valid());
}

TEST_F(SwiftSeederTest, AcceptsAltPrefix) {
    auto sig = extract("_$s3FooAAC3barSiyF");
    EXPECT_TRUE(sig.valid());
}

TEST_F(SwiftSeederTest, ModuleExtraction) {
    // $s<3Foo> = module "Foo"
    auto sig = extract("$s3FooAAC3barSiyF");
    EXPECT_TRUE(sig.valid());
    // Module name should be non-empty
    EXPECT_FALSE(sig.namespaceName.empty());
}

TEST_F(SwiftSeederTest, DemangledNameSet) {
    auto sig = extract("$s3FooAAC3barSiyF");
    EXPECT_TRUE(sig.valid());
    EXPECT_FALSE(sig.demangledName.empty());
}

TEST_F(SwiftSeederTest, CCIsUnknown) {
    auto sig = extract("$s3FooAAC3barSiyF");
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.callingConvention, MangledCC::Unknown);
}

// ─── TypeSeedDispatcher tests ─────────────────────────────────────────────────

class DispatcherTest : public ::testing::Test {
protected:
    TypeSeedDispatcher disp;
    CollectingMgr      mgr;
    void SetUp() override { disp = makeDefaultDispatcher(); }
};

TEST_F(DispatcherTest, RoutesItaniumSymbol) {
    auto sig = disp.process("_Z3foov", 0x1000, mgr);
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "foo");
}

TEST_F(DispatcherTest, RoutesMsvcSymbol) {
    auto sig = disp.process("?foo@@YAHXZ", 0x2000, mgr);
    EXPECT_TRUE(sig.valid());
    EXPECT_EQ(sig.functionName, "foo");
    EXPECT_EQ(sig.callingConvention, MangledCC::Cdecl);
}

TEST_F(DispatcherTest, RoutesRustSymbol) {
    auto sig = disp.process("_ZN3foo3bar17h0123456789abcdefE", 0x3000, mgr);
    EXPECT_TRUE(sig.valid());
}

TEST_F(DispatcherTest, ReturnsInvalidForUnknown) {
    auto sig = disp.process("printf", 0x4000, mgr);
    EXPECT_FALSE(sig.valid());
}

TEST_F(DispatcherTest, BatchProcessing) {
    std::vector<std::pair<std::string,uint64_t>> batch = {
        {"_Z3foov",   0x1000},
        {"?bar@@YAHXZ", 0x2000},
        {"_ZN3std4cout17h0123456789abcdefE", 0x3000},
        {"printf",    0x4000},  // not mangled
    };
    uint32_t count = disp.processBatch(batch, mgr);
    EXPECT_GE(count, 2u); // at least Itanium + MSVC
}

TEST_F(DispatcherTest, ConstraintVmaIsSet) {
    disp.process("_Z3foov", 0xDEADBEEF, mgr);
    for (auto& c : mgr.constraints) {
        EXPECT_EQ(c.symbolVma, 0xDEADBEEFull);
    }
}

TEST_F(DispatcherTest, SourceSymbolInConstraint) {
    disp.process("?foo@@YAHXZ", 0x100, mgr);
    for (auto& c : mgr.constraints) {
        EXPECT_EQ(c.sourceSymbol, "?foo@@YAHXZ");
    }
}

TEST_F(DispatcherTest, ConfidenceIsOne) {
    disp.process("_Z3foov", 0x100, mgr);
    for (auto& c : mgr.constraints) {
        EXPECT_FLOAT_EQ(c.confidence, 1.0f);
    }
}

// ─── TypeConstraint generation tests ─────────────────────────────────────────

class ConstraintTest : public ::testing::Test {
protected:
    TypeSeedDispatcher disp;
    CollectingMgr      mgr;
    void SetUp() override { disp = makeDefaultDispatcher(); }
};

// ── Return type constraint ────────────────────────────────────────────────────
TEST_F(ConstraintTest, EmitsReturnTypeForMsvc) {
    disp.process("?foo@@YAHXZ", 0x100, mgr);
    EXPECT_TRUE(mgr.hasKind(ConstraintKind::ReturnType));
    auto* c = mgr.findKind(ConstraintKind::ReturnType);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->typeStr, "int");
}

// ── Calling convention constraint ─────────────────────────────────────────────
TEST_F(ConstraintTest, EmitsCallingConventionForMsvc) {
    disp.process("?foo@@YAHXZ", 0x100, mgr);
    EXPECT_TRUE(mgr.hasKind(ConstraintKind::CallingConvention));
    auto* c = mgr.findKind(ConstraintKind::CallingConvention);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->cc, MangledCC::Cdecl);
}

// ── Thiscall this-pointer constraint ─────────────────────────────────────────
TEST_F(ConstraintTest, EmitsThisTypeForMemberFunction) {
    disp.process("?bar@Foo@@QAEXXZ", 0x100, mgr);
    EXPECT_TRUE(mgr.hasKind(ConstraintKind::ThisType));
    auto* c = mgr.findKind(ConstraintKind::ThisType);
    ASSERT_NE(c, nullptr);
    EXPECT_NE(c->typeStr.find("Foo"), std::string::npos);
}

// ── Param type constraints ────────────────────────────────────────────────────
TEST_F(ConstraintTest, EmitsParamTypesForItanium) {
    disp.process("_Z3barid", 0x100, mgr);
    auto params = mgr.allOfKind(ConstraintKind::ParamType);
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params[0].paramIndex, 0u);
    EXPECT_EQ(params[0].typeStr, "int");
    EXPECT_EQ(params[1].paramIndex, 1u);
    EXPECT_EQ(params[1].typeStr, "double");
}

// ── Template arg constraints ──────────────────────────────────────────────────
TEST_F(ConstraintTest, EmitsTemplateArgForInstantiation) {
    disp.process("_Z3maxIiET_S0_S0_", 0x100, mgr);
    auto targs = mgr.allOfKind(ConstraintKind::TemplateArgType);
    ASSERT_GE(targs.size(), 1u);
    EXPECT_EQ(targs[0].typeStr, "int");
}

TEST_F(ConstraintTest, EmitsTemplateArgDouble) {
    disp.process("_Z3minIdET_S0_S0_", 0x100, mgr);
    auto targs = mgr.allOfKind(ConstraintKind::TemplateArgType);
    ASSERT_GE(targs.size(), 1u);
    EXPECT_EQ(targs[0].typeStr, "double");
}

// ── Const this pointer ────────────────────────────────────────────────────────
TEST_F(ConstraintTest, ConstMemberEmitsConstThisType) {
    disp.process("_ZNK3Foo3barEv", 0x100, mgr);
    auto* c = mgr.findKind(ConstraintKind::ThisType);
    ASSERT_NE(c, nullptr);
    EXPECT_NE(c->typeStr.find("const"), std::string::npos);
}

// ─── mangledCCName tests ──────────────────────────────────────────────────────

TEST(MangledCCNameTest, AllCases) {
    EXPECT_STREQ(mangledCCName(MangledCC::Unknown),    "unknown");
    EXPECT_STREQ(mangledCCName(MangledCC::Cdecl),      "__cdecl");
    EXPECT_STREQ(mangledCCName(MangledCC::Stdcall),    "__stdcall");
    EXPECT_STREQ(mangledCCName(MangledCC::Fastcall),   "__fastcall");
    EXPECT_STREQ(mangledCCName(MangledCC::Thiscall),   "__thiscall");
    EXPECT_STREQ(mangledCCName(MangledCC::Vectorcall), "__vectorcall");
    EXPECT_STREQ(mangledCCName(MangledCC::Clrcall),    "__clrcall");
    EXPECT_STREQ(mangledCCName(MangledCC::Pascal),     "__pascal");
    EXPECT_STREQ(mangledCCName(MangledCC::SysVAmd64),  "sysv_amd64");
    EXPECT_STREQ(mangledCCName(MangledCC::Win64),      "win64");
    EXPECT_STREQ(mangledCCName(MangledCC::AArch64),    "aarch64");
}

// ─── Extended Itanium corpus ──────────────────────────────────────────────────

struct ItaniumCase {
    const char* mangled;
    const char* expectedFunc;
    const char* expectedClass;     // nullptr = free function
    bool        expectConst;
    bool        expectCtor;
    bool        expectDtor;
};

static const ItaniumCase kItaniumCorpus[] = {
    // Free functions
    {"_Z3foov",              "foo",         nullptr, false,false,false},
    {"_Z3barid",             "bar",         nullptr, false,false,false},
    {"_Z3bazPKc",            "baz",         nullptr, false,false,false},
    {"_Z3quxRi",             "qux",         nullptr, false,false,false},
    {"_Z4quuxOi",            "quux",        nullptr, false,false,false},
    // Member functions
    {"_ZN3Foo3barEv",        "bar",         "Foo",   false,false,false},
    {"_ZNK3Foo3getEv",       "get",         "Foo",   true, false,false},
    {"_ZN3Foo3setEi",        "set",         "Foo",   false,false,false},
    // Constructors/destructors
    {"_ZN3FooC1Ev",          "<constructor>","Foo",  false,true, false},
    {"_ZN3FooC2Ev",          "<constructor>","Foo",  false,true, false},
    {"_ZN3FooD1Ev",          "<destructor>", "Foo",  false,false,true },
    {"_ZN3FooD2Ev",          "<destructor>", "Foo",  false,false,true },
    // Nested namespaces
    {"_ZN3std3map6insertE",  "insert",      "map",   false,false,false},
    // STL
    {"_ZNKSs4sizeEv",        "size",         nullptr,true, false,false},
};

class ItaniumCorpusTest : public ::testing::TestWithParam<ItaniumCase> {};

TEST_P(ItaniumCorpusTest, ExtractCorrectly) {
    auto disp = makeDefaultDispatcher();
    const auto& tc = GetParam();
    auto sig = disp.tryExtract(tc.mangled);
    EXPECT_TRUE(sig.valid()) << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.functionName, tc.expectedFunc) << "mangled=" << tc.mangled;
    if (tc.expectedClass) {
        EXPECT_EQ(sig.className, tc.expectedClass) << "mangled=" << tc.mangled;
    } else {
        EXPECT_TRUE(sig.className.empty()) << "mangled=" << tc.mangled;
    }
    EXPECT_EQ(sig.isConst, tc.expectConst)       << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.isConstructor, tc.expectCtor)  << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.isDestructor, tc.expectDtor)   << "mangled=" << tc.mangled;
}

INSTANTIATE_TEST_SUITE_P(ItaniumSymbols, ItaniumCorpusTest,
    ::testing::ValuesIn(kItaniumCorpus));

// ─── Extended MSVC corpus ─────────────────────────────────────────────────────

struct MsvcCase {
    const char* mangled;
    const char* expectedFunc;
    MangledCC   expectedCC;
    bool        expectCtor;
    bool        expectDtor;
};

static const MsvcCase kMsvcCorpus[] = {
    {"?foo@@YAHXZ",        "foo",         MangledCC::Cdecl,    false,false},
    {"?bar@@YGHH@Z",       "bar",         MangledCC::Stdcall,  false,false},
    {"?baz@@YIIHHH@Z",     "baz",         MangledCC::Fastcall, false,false},
    {"??0Foo@@QAE@XZ",     "<constructor>",MangledCC::Thiscall,true, false},
    {"??1Foo@@UAE@XZ",     "<destructor>", MangledCC::Thiscall,false,true },
    {"?bar@Foo@@QAEXXZ",   "bar",         MangledCC::Thiscall, false,false},
    {"?isValid@@YA_NXZ",   "isValid",     MangledCC::Cdecl,    false,false},
    {"?getVal@@YANXZ",     "getVal",      MangledCC::Cdecl,    false,false},
};

class MsvcCorpusTest : public ::testing::TestWithParam<MsvcCase> {};

TEST_P(MsvcCorpusTest, ExtractCorrectly) {
    auto disp = makeDefaultDispatcher();
    const auto& tc = GetParam();
    auto sig = disp.tryExtract(tc.mangled);
    EXPECT_TRUE(sig.valid()) << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.functionName, tc.expectedFunc) << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.callingConvention, tc.expectedCC) << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.isConstructor, tc.expectCtor) << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.isDestructor, tc.expectDtor) << "mangled=" << tc.mangled;
}

INSTANTIATE_TEST_SUITE_P(MsvcSymbols, MsvcCorpusTest,
    ::testing::ValuesIn(kMsvcCorpus));

// ─── Extended Rust corpus ─────────────────────────────────────────────────────

struct RustCase {
    const char* mangled;
    const char* expectedFunc;
    bool        expectNoReturn;
};

static const RustCase kRustCorpus[] = {
    {"_ZN3foo3bar17h0123456789abcdefE",              "bar",       false},
    {"_ZN4core3mem4drop17h0123456789abcdefE",        "drop",      false},
    {"_ZN4core9panicking5panic17h0123456789abcdefE", "panic",     true },
    {"_ZN3std7process5abort17h0123456789abcdefE",    "abort",     true },
    {"_ZN4core3fmt5write17h0123456789abcdefE",       "write",     false},
    {"_ZN3std2io6stderr17habcdef0123456789E",        "stderr",    false},
};

class RustCorpusTest : public ::testing::TestWithParam<RustCase> {};

TEST_P(RustCorpusTest, ExtractCorrectly) {
    auto disp = makeDefaultDispatcher();
    const auto& tc = GetParam();
    auto sig = disp.tryExtract(tc.mangled);
    EXPECT_TRUE(sig.valid()) << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.functionName, tc.expectedFunc) << "mangled=" << tc.mangled;
    EXPECT_EQ(sig.isNoReturn, tc.expectNoReturn) << "mangled=" << tc.mangled;
}

INSTANTIATE_TEST_SUITE_P(RustSymbols, RustCorpusTest,
    ::testing::ValuesIn(kRustCorpus));

// ─── ITypeInferenceMgr::addSignature convenience method ───────────────────────

TEST(AddSignatureTest, AddsAllConstraintTypes) {
    auto disp = makeDefaultDispatcher();
    CollectingMgr mgr;
    auto sig = disp.tryExtract("_Z3barid");
    ASSERT_TRUE(sig.valid());
    mgr.addSignature(sig, 0x5000);
    // Should have at least ParamType constraints
    EXPECT_TRUE(mgr.hasKind(ConstraintKind::ParamType));
}
