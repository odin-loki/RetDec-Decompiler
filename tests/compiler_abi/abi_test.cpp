/**
 * @file tests/compiler_abi/abi_test.cpp
 * @brief Unit tests for AbiDescriptor: calling conventions, EH models,
 *        stdlib signatures, and per-compiler name demanglers.
 */

#include "retdec/compiler_abi/abi_descriptor.h"

#include <gtest/gtest.h>
#include <algorithm>

using namespace retdec::compiler_abi;

// ════════════════════════════════════════════════════════════════════════════
// 1. compilerFamilyName
// ════════════════════════════════════════════════════════════════════════════

TEST(CompilerFamilyName, AllFamilies) {
    EXPECT_STREQ(compilerFamilyName(CompilerFamily::GCC_Clang), "GCC/Clang");
    EXPECT_STREQ(compilerFamilyName(CompilerFamily::MSVC),      "MSVC");
    EXPECT_STREQ(compilerFamilyName(CompilerFamily::Borland),   "Borland/Embarcadero");
    EXPECT_STREQ(compilerFamilyName(CompilerFamily::DMC),       "Digital Mars C++");
    EXPECT_STREQ(compilerFamilyName(CompilerFamily::Watcom),    "Open Watcom C++");
    EXPECT_STREQ(compilerFamilyName(CompilerFamily::Symbian),   "Symbian/EPOC C++");
    EXPECT_STREQ(compilerFamilyName(CompilerFamily::Unknown),   "Unknown");
}

// ════════════════════════════════════════════════════════════════════════════
// 2. Calling convention descriptors
// ════════════════════════════════════════════════════════════════════════════

TEST(CallingConvention, GccClangX86_32_DefaultCC) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::GCC_Clang, Arch::X86_32);
    EXPECT_EQ(d.defaultCC.arch,           Arch::X86_32);
    EXPECT_EQ(d.defaultCC.return_reg,     RegId::EAX);
    EXPECT_FALSE(d.defaultCC.callee_cleanup);
    EXPECT_TRUE(d.defaultCC.param_regs.empty()); // all args on stack (cdecl)
}

TEST(CallingConvention, GccClangX86_64_SysV) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::GCC_Clang, Arch::X86_64);
    EXPECT_EQ(d.defaultCC.arch,       Arch::X86_64);
    EXPECT_EQ(d.defaultCC.return_reg, RegId::RAX);
    EXPECT_EQ(d.defaultCC.stack_align, 16u);
    ASSERT_GE(d.defaultCC.param_regs.size(), 4u);
    EXPECT_EQ(d.defaultCC.param_regs[0], RegId::RDI); // first arg in rdi
    EXPECT_EQ(d.defaultCC.param_regs[1], RegId::RSI);
}

TEST(CallingConvention, MSVC_x86_64_Win64) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::MSVC, Arch::X86_64);
    EXPECT_EQ(d.defaultCC.return_reg, RegId::RAX);
    ASSERT_GE(d.defaultCC.param_regs.size(), 4u);
    EXPECT_EQ(d.defaultCC.param_regs[0], RegId::RCX); // first arg in rcx
    EXPECT_EQ(d.defaultCC.shadow_space, 32u);          // 32-byte home space
}

TEST(CallingConvention, MSVC_x86_32_MemberCC) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::MSVC, Arch::X86_32);
    EXPECT_EQ(d.memberCC.this_reg,       RegId::ECX); // __thiscall
    EXPECT_TRUE(d.memberCC.callee_cleanup);
}

TEST(CallingConvention, Borland_RegisterCC) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::Borland, Arch::X86_32);
    ASSERT_GE(d.defaultCC.param_regs.size(), 3u);
    EXPECT_EQ(d.defaultCC.param_regs[0], RegId::EAX);
    EXPECT_EQ(d.defaultCC.param_regs[1], RegId::EDX);
    EXPECT_EQ(d.defaultCC.param_regs[2], RegId::ECX);
    EXPECT_EQ(d.defaultCC.this_reg,      RegId::EAX);
    EXPECT_TRUE(d.defaultCC.callee_cleanup);
}

TEST(CallingConvention, Watcom_RegisterCC) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::Watcom, Arch::X86_32);
    ASSERT_GE(d.defaultCC.param_regs.size(), 4u);
    EXPECT_EQ(d.defaultCC.param_regs[0], RegId::EAX);
    EXPECT_EQ(d.defaultCC.param_regs[1], RegId::EDX);
    EXPECT_EQ(d.defaultCC.param_regs[2], RegId::EBX);
    EXPECT_EQ(d.defaultCC.param_regs[3], RegId::ECX);
    EXPECT_EQ(d.defaultCC.this_reg,      RegId::EAX);
}

TEST(CallingConvention, Symbian_AAPCS) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::Symbian, Arch::ARM32);
    EXPECT_EQ(d.defaultCC.arch,       Arch::ARM32);
    EXPECT_EQ(d.defaultCC.this_reg,   RegId::R0);
    ASSERT_GE(d.defaultCC.param_regs.size(), 4u);
    EXPECT_EQ(d.defaultCC.param_regs[0], RegId::R0);
}

TEST(CallingConvention, DMC_x86_32) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::DMC, Arch::X86_32);
    EXPECT_EQ(d.defaultCC.return_reg, RegId::EAX);
    EXPECT_EQ(d.memberCC.this_reg,    RegId::ECX);
}

TEST(CallingConvention, StandardCC_x86_32_count) {
    auto ccs = standardCallingConventions(Arch::X86_32);
    EXPECT_GE(ccs.size(), 5u); // cdecl, stdcall, thiscall, fastcall, borland_register, borland_pascal, watcall
}

// ════════════════════════════════════════════════════════════════════════════
// 3. EH model descriptors
// ════════════════════════════════════════════════════════════════════════════

TEST(EhModel, GccClang_Itanium) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::GCC_Clang, Arch::X86_64);
    EXPECT_EQ(d.ehModel.model, EhModel::ItaniumDwarf);
    EXPECT_FALSE(d.ehModel.personalityFunc.empty());
    EXPECT_FALSE(d.ehModel.throwFunc.empty());
    EXPECT_EQ(d.ehModel.throwFunc, "__cxa_throw");
}

TEST(EhModel, MSVC_CxxEH) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::MSVC, Arch::X86_64);
    EXPECT_EQ(d.ehModel.model, EhModel::MsvcEh);
    EXPECT_EQ(d.ehModel.personalityFunc, "_CxxFrameHandler3");
}

TEST(EhModel, Borland) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::Borland, Arch::X86_32);
    EXPECT_EQ(d.ehModel.model, EhModel::BorlandEh);
    EXPECT_EQ(d.ehModel.personalityFunc, "__ExceptionHandler");
}

TEST(EhModel, DMC) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::DMC, Arch::X86_32);
    EXPECT_EQ(d.ehModel.model, EhModel::DmcEh);
    EXPECT_EQ(d.ehModel.personalityFunc, "__exception_handler");
}

TEST(EhModel, Watcom) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::Watcom, Arch::X86_32);
    EXPECT_EQ(d.ehModel.model, EhModel::WatcomEh);
}

TEST(EhModel, Symbian_Leave) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::Symbian, Arch::ARM32);
    EXPECT_EQ(d.ehModel.model, EhModel::SymbianLeave);
    EXPECT_EQ(d.ehModel.leaveFunc,        "User::Leave");
    EXPECT_EQ(d.ehModel.cleanupPushFunc,  "CleanupStack::PushL");
    EXPECT_EQ(d.ehModel.cleanupPopFunc,   "CleanupStack::PopAndDestroy");
}

// ════════════════════════════════════════════════════════════════════════════
// 4. Stdlib signature tables
// ════════════════════════════════════════════════════════════════════════════

static bool hasSig(const std::vector<StdlibSig>& sigs, const std::string& name) {
    return std::any_of(sigs.begin(), sigs.end(), [&](const StdlibSig& s) {
        return s.mangledName == name || s.plainName == name;
    });
}

TEST(StdlibSigs, GccClang_malloc) {
    auto sigs = stdlibSignatures(CompilerFamily::GCC_Clang);
    EXPECT_TRUE(hasSig(sigs, "malloc"));
    EXPECT_TRUE(hasSig(sigs, "free"));
    EXPECT_TRUE(hasSig(sigs, "exit"));
    EXPECT_TRUE(hasSig(sigs, "__cxa_throw"));
    EXPECT_TRUE(hasSig(sigs, "__cxa_pure_virtual"));
}

TEST(StdlibSigs, GccClang_exit_noreturn) {
    auto sigs = stdlibSignatures(CompilerFamily::GCC_Clang);
    auto it = std::find_if(sigs.begin(), sigs.end(),
        [](const StdlibSig& s){ return s.plainName == "exit"; });
    ASSERT_NE(it, sigs.end());
    EXPECT_TRUE(it->noReturn);
}

TEST(StdlibSigs, MSVC_operatorNew) {
    auto sigs = stdlibSignatures(CompilerFamily::MSVC);
    EXPECT_TRUE(hasSig(sigs, "??2@YAPAXI@Z")); // operator new (32-bit)
    EXPECT_TRUE(hasSig(sigs, "??3@YAXPAX@Z")); // operator delete
}

TEST(StdlibSigs, Borland_TObjectFree) {
    auto sigs = stdlibSignatures(CompilerFamily::Borland);
    EXPECT_TRUE(hasSig(sigs, "@TObject@Free$qqrv"));
}

TEST(StdlibSigs, Symbian_UserLeave) {
    auto sigs = stdlibSignatures(CompilerFamily::Symbian);
    EXPECT_TRUE(hasSig(sigs, "User::Leave"));
    EXPECT_TRUE(hasSig(sigs, "CleanupStack::PushL"));
    EXPECT_TRUE(hasSig(sigs, "User::Panic"));
}

TEST(StdlibSigs, Symbian_Leave_noreturn) {
    auto sigs = stdlibSignatures(CompilerFamily::Symbian);
    auto it = std::find_if(sigs.begin(), sigs.end(),
        [](const StdlibSig& s){ return s.plainName == "User::Leave"; });
    ASSERT_NE(it, sigs.end());
    EXPECT_TRUE(it->noReturn);
}

TEST(StdlibSigs, FindSig_byMangledName) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::GCC_Clang, Arch::X86_64);
    const StdlibSig* s = d.findSig("malloc");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->returnType, "void*");
    ASSERT_GE(s->params.size(), 1u);
    EXPECT_EQ(s->params[0].typeName, "size_t");
}

TEST(StdlibSigs, FindSig_missing) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::GCC_Clang, Arch::X86_64);
    EXPECT_EQ(d.findSig("__totally_unknown_xyz"), nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// 5. Name demanglers
// ════════════════════════════════════════════════════════════════════════════

TEST(Demangler, GccClang_isMangled) {
    auto dem = makeDemangler(CompilerFamily::GCC_Clang);
    EXPECT_TRUE(dem->isMangled("_ZN3FooC1Ev"));
    EXPECT_FALSE(dem->isMangled("main"));
    EXPECT_FALSE(dem->isMangled("printf"));
}

TEST(Demangler, GccClang_demangle_simpleClass) {
    auto dem = makeDemangler(CompilerFamily::GCC_Clang);
    std::string result = dem->demangle("_ZN3Foo3barEv");
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result, "_ZN3Foo3barEv"); // should have changed
    EXPECT_TRUE(result.find("Foo") != std::string::npos);
    EXPECT_TRUE(result.find("bar") != std::string::npos);
}

TEST(Demangler, GccClang_demangle_nestedNS) {
    auto dem = makeDemangler(CompilerFamily::GCC_Clang);
    // _ZN3std6vectorIiSaIiEE4sizeEv
    // Just check it doesn't crash and changes the string
    std::string result = dem->demangle("_ZN3std6vectorIiSaIiEE4sizeEv");
    EXPECT_FALSE(result.empty());
}

TEST(Demangler, GccClang_notMangled_passthrough) {
    auto dem = makeDemangler(CompilerFamily::GCC_Clang);
    EXPECT_EQ(dem->demangle("printf"), "printf");
}

TEST(Demangler, MSVC_isMangled) {
    auto dem = makeDemangler(CompilerFamily::MSVC);
    EXPECT_TRUE(dem->isMangled("?bar@Foo@@QAEXXZ"));
    EXPECT_FALSE(dem->isMangled("main"));
    EXPECT_FALSE(dem->isMangled("_ZN3Foo3barEv"));
}

TEST(Demangler, MSVC_demangle_simple) {
    auto dem = makeDemangler(CompilerFamily::MSVC);
    std::string result = dem->demangle("?bar@Foo@@QAEXXZ");
    EXPECT_TRUE(result.find("Foo") != std::string::npos ||
                result.find("bar") != std::string::npos);
}

TEST(Demangler, MSVC_demangle_plainPassthrough) {
    auto dem = makeDemangler(CompilerFamily::MSVC);
    EXPECT_EQ(dem->demangle("printf"), "printf");
}

TEST(Demangler, Borland_isMangled) {
    auto dem = makeDemangler(CompilerFamily::Borland);
    EXPECT_TRUE(dem->isMangled("@TForm1@Button1Click$qqrp14Classes@TObject"));
    EXPECT_FALSE(dem->isMangled("printf"));
    EXPECT_FALSE(dem->isMangled("_ZN3Foo3barEv"));
}

TEST(Demangler, Borland_demangle_simple) {
    auto dem = makeDemangler(CompilerFamily::Borland);
    std::string result = dem->demangle("@TForm1@Button1Click$qqrp14Classes@TObject");
    EXPECT_TRUE(result.find("TForm1") != std::string::npos);
    EXPECT_TRUE(result.find("Button1Click") != std::string::npos);
}

TEST(Demangler, DMC_isMangled) {
    auto dem = makeDemangler(CompilerFamily::DMC);
    EXPECT_TRUE(dem->isMangled("__Foo__bar"));
    EXPECT_FALSE(dem->isMangled("printf"));
}

TEST(Demangler, DMC_demangle_simple) {
    auto dem = makeDemangler(CompilerFamily::DMC);
    std::string result = dem->demangle("__Foo__bar");
    EXPECT_TRUE(result.find("Foo") != std::string::npos);
    EXPECT_TRUE(result.find("bar") != std::string::npos);
}

TEST(Demangler, Watcom_isMangled) {
    auto dem = makeDemangler(CompilerFamily::Watcom);
    EXPECT_TRUE(dem->isMangled("W?Foo$bar"));
    EXPECT_FALSE(dem->isMangled("main"));
}

TEST(Demangler, Watcom_demangle_simple) {
    auto dem = makeDemangler(CompilerFamily::Watcom);
    std::string result = dem->demangle("W?Foo$bar");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("Foo") != std::string::npos);
}

TEST(Demangler, Symbian_itaniumPrefix) {
    auto dem = makeDemangler(CompilerFamily::Symbian);
    EXPECT_TRUE(dem->isMangled("_ZN5CBase7IsClassEv"));
    std::string r = dem->demangle("_ZN5CBase7IsClassEv");
    EXPECT_TRUE(r.find("CBase") != std::string::npos);
}

TEST(Demangler, Symbian_plainName) {
    auto dem = makeDemangler(CompilerFamily::Symbian);
    EXPECT_TRUE(dem->isMangled("CMyServer"));
    // Plain names should pass through
    EXPECT_EQ(dem->demangle("CMyServer"), "CMyServer");
}

// ════════════════════════════════════════════════════════════════════════════
// 6. AbiDescriptor::forCompiler consistency
// ════════════════════════════════════════════════════════════════════════════

TEST(AbiDescriptor, ForCompiler_familySet) {
    for (auto f : {CompilerFamily::GCC_Clang, CompilerFamily::MSVC,
                   CompilerFamily::Borland,   CompilerFamily::DMC,
                   CompilerFamily::Watcom,    CompilerFamily::Symbian})
    {
        AbiDescriptor d = AbiDescriptor::forCompiler(
            f, (f == CompilerFamily::Symbian) ? Arch::ARM32 : Arch::X86_32);
        EXPECT_EQ(d.family, f);
        EXPECT_FALSE(d.stdlibSigs.empty()) << "No stdlib sigs for "
            << compilerFamilyName(f);
        EXPECT_NE(d.ehModel.model, EhModel::None) << "No EH model for "
            << compilerFamilyName(f);
    }
}

TEST(AbiDescriptor, ForCompiler_x64_GccClang) {
    AbiDescriptor d = AbiDescriptor::forCompiler(CompilerFamily::GCC_Clang, Arch::X86_64);
    EXPECT_EQ(d.arch, Arch::X86_64);
    EXPECT_EQ(d.defaultCC.arch, Arch::X86_64);
}
