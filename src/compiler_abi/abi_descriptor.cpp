/**
 * @file src/compiler_abi/abi_descriptor.cpp
 * @brief ABI descriptors, calling conventions, EH models, stdlib signatures,
 *        and name demanglers for all six supported compiler families.
 */

#include "retdec/compiler_abi/abi_descriptor.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>

#ifdef RETDEC_USE_CXXABI_DEMANGLE
#  include <cxxabi.h>
#endif

namespace retdec {
namespace compiler_abi {

// ─── compilerFamilyName ───────────────────────────────────────────────────────

const char* compilerFamilyName(CompilerFamily f) noexcept {
    switch (f) {
    case CompilerFamily::GCC_Clang: return "GCC/Clang";
    case CompilerFamily::MSVC:      return "MSVC";
    case CompilerFamily::Borland:   return "Borland/Embarcadero";
    case CompilerFamily::DMC:       return "Digital Mars C++";
    case CompilerFamily::Watcom:    return "Open Watcom C++";
    case CompilerFamily::Symbian:   return "Symbian/EPOC C++";
    default:                         return "Unknown";
    }
}

// ─── Calling convention tables ────────────────────────────────────────────────

namespace {

// ── x86-32 conventions ────────────────────────────────────────────────────────

static CallingConventionDesc cdecl_x86() {
    CallingConventionDesc d;
    d.name              = "cdecl";
    d.arch              = Arch::X86_32;
    d.param_regs        = {};             // all args on stack
    d.float_regs        = {};
    d.return_reg        = RegId::EAX;
    d.return_float_reg  = RegId::XMM0;
    d.this_reg          = RegId::None;
    d.callee_cleanup    = false;
    d.stack_align       = 4;
    return d;
}

static CallingConventionDesc stdcall_x86() {
    auto d         = cdecl_x86();
    d.name         = "stdcall";
    d.callee_cleanup = true;
    return d;
}

static CallingConventionDesc msvc_thiscall_x86() {
    auto d         = stdcall_x86();
    d.name         = "__thiscall";
    d.this_reg     = RegId::ECX;
    d.callee_cleanup = true;
    return d;
}

static CallingConventionDesc msvc_fastcall_x86() {
    CallingConventionDesc d;
    d.name              = "__fastcall";
    d.arch              = Arch::X86_32;
    d.param_regs        = {RegId::ECX, RegId::EDX};
    d.return_reg        = RegId::EAX;
    d.callee_cleanup    = true;
    d.stack_align       = 4;
    return d;
}

// Borland __fastcall (register): first 3 int args in EAX, EDX, ECX.
// Args left-to-right in registers; this = first register (EAX for static,
// or caller-provided for member calls).
static CallingConventionDesc borland_register_x86() {
    CallingConventionDesc d;
    d.name              = "borland_register";
    d.arch              = Arch::X86_32;
    d.param_regs        = {RegId::EAX, RegId::EDX, RegId::ECX};
    d.return_reg        = RegId::EAX;
    d.return_float_reg  = RegId::XMM0;
    d.this_reg          = RegId::EAX; // `this` is first arg = EAX
    d.callee_cleanup    = true;
    d.stack_align       = 4;
    d.args_left_to_right= false; // right-to-left for stack portion
    return d;
}

// Borland Pascal convention: args pushed left-to-right, callee cleans.
static CallingConventionDesc borland_pascal_x86() {
    CallingConventionDesc d;
    d.name              = "pascal";
    d.arch              = Arch::X86_32;
    d.param_regs        = {};
    d.return_reg        = RegId::EAX;
    d.callee_cleanup    = true;
    d.stack_align       = 4;
    d.args_left_to_right= true;
    return d;
}

// Watcom __watcall (register): first 4 integer args in EAX, EDX, EBX, ECX.
// For member functions `this` = EAX.
static CallingConventionDesc watcom_register_x86() {
    CallingConventionDesc d;
    d.name              = "watcall";
    d.arch              = Arch::X86_32;
    d.param_regs        = {RegId::EAX, RegId::EDX, RegId::EBX, RegId::ECX};
    d.return_reg        = RegId::EAX;
    d.this_reg          = RegId::EAX;
    d.callee_cleanup    = false; // caller cleans stack portion
    d.stack_align       = 4;
    return d;
}

// ── x86-64 conventions ────────────────────────────────────────────────────────

static CallingConventionDesc sysv_amd64() {
    CallingConventionDesc d;
    d.name         = "SystemV_AMD64";
    d.arch         = Arch::X86_64;
    d.param_regs   = {RegId::RDI, RegId::RSI, RegId::RDX,
                      RegId::RCX, RegId::R8,  RegId::R9};
    d.float_regs   = {RegId::XMM0, RegId::XMM1, RegId::XMM2, RegId::XMM3,
                      RegId::XMM4, RegId::XMM5, RegId::XMM6, RegId::XMM7};
    d.return_reg        = RegId::RAX;
    d.return_float_reg  = RegId::XMM0;
    d.this_reg          = RegId::RDI;
    d.callee_cleanup    = false;
    d.stack_align       = 16;
    return d;
}

static CallingConventionDesc win64_cc() {
    CallingConventionDesc d;
    d.name         = "Win64";
    d.arch         = Arch::X86_64;
    d.param_regs   = {RegId::RCX, RegId::RDX, RegId::R8, RegId::R9};
    d.float_regs   = {RegId::XMM0, RegId::XMM1, RegId::XMM2, RegId::XMM3};
    d.return_reg        = RegId::RAX;
    d.return_float_reg  = RegId::XMM0;
    d.this_reg          = RegId::RCX;
    d.callee_cleanup    = false;
    d.stack_align       = 16;
    d.shadow_space      = 32; // 4 x 8-byte home space
    return d;
}

// ── ARM conventions ───────────────────────────────────────────────────────────

static CallingConventionDesc arm_aapcs() {
    CallingConventionDesc d;
    d.name         = "AAPCS";
    d.arch         = Arch::ARM32;
    d.param_regs   = {RegId::R0, RegId::R1, RegId::R2, RegId::R3};
    d.return_reg        = RegId::R0;
    d.this_reg          = RegId::R0;
    d.callee_cleanup    = false;
    d.stack_align       = 8;
    return d;
}

static CallingConventionDesc aarch64_aapcs() {
    CallingConventionDesc d;
    d.name         = "AAPCS64";
    d.arch         = Arch::ARM64;
    d.param_regs   = {RegId::X0, RegId::X1, RegId::X2, RegId::X3,
                      RegId::X4, RegId::X5, RegId::X6, RegId::X7};
    d.float_regs   = {RegId::D0, RegId::D1, RegId::D2, RegId::D3,
                      RegId::D4, RegId::D5, RegId::D6, RegId::D7};
    d.return_reg        = RegId::X0;
    d.return_float_reg  = RegId::D0;
    d.this_reg          = RegId::X0;
    d.callee_cleanup    = false;
    d.stack_align       = 16;
    return d;
}

// ── EH model presets ─────────────────────────────────────────────────────────

static EhModelDesc itaniumDwarfEH() {
    EhModelDesc e;
    e.model            = EhModel::ItaniumDwarf;
    e.personalityFunc  = "__gxx_personality_v0";
    e.landingPadFunc   = "__cxa_begin_catch";
    e.throwFunc        = "__cxa_throw";
    e.rethrowFunc      = "__cxa_rethrow";
    e.terminateFunc    = "__cxa_call_terminate";
    return e;
}

static EhModelDesc msvcEH() {
    EhModelDesc e;
    e.model            = EhModel::MsvcEh;
    e.personalityFunc  = "_CxxFrameHandler3";
    e.throwFunc        = "_CxxThrowException";
    e.terminateFunc    = "terminate";
    return e;
}

static EhModelDesc msvcSEH() {
    EhModelDesc e;
    e.model            = EhModel::MsvcSeh;
    e.personalityFunc  = "_except_handler3";
    e.terminateFunc    = "terminate";
    return e;
}

static EhModelDesc borlandEH() {
    EhModelDesc e;
    e.model           = EhModel::BorlandEh;
    e.personalityFunc = "__ExceptionHandler";
    e.throwFunc       = "_RaiseException";
    e.terminateFunc   = "terminate";
    return e;
}

static EhModelDesc dmcEH() {
    EhModelDesc e;
    e.model           = EhModel::DmcEh;
    e.personalityFunc = "__exception_handler";
    e.throwFunc       = "_Throw";
    e.terminateFunc   = "__terminate";
    return e;
}

static EhModelDesc watcomEH() {
    EhModelDesc e;
    e.model           = EhModel::WatcomEh;
    e.personalityFunc = "__WEH_EpilogHook";
    e.throwFunc       = "__ThrowException";
    e.terminateFunc   = "__terminate";
    return e;
}

static EhModelDesc symbianLeave() {
    EhModelDesc e;
    e.model            = EhModel::SymbianLeave;
    e.throwFunc        = "User::Leave";
    e.leaveFunc        = "User::Leave";
    e.trapSetupFunc    = "__TRAP";
    e.cleanupPushFunc  = "CleanupStack::PushL";
    e.cleanupPopFunc   = "CleanupStack::PopAndDestroy";
    e.terminateFunc    = "User::Panic";
    return e;
}

// ── Stdlib signature helpers ──────────────────────────────────────────────────

static StdlibSig sig(const std::string& mangled,
                      const std::string& plain,
                      const std::string& ret,
                      std::vector<StdlibParam> ps,
                      bool noret = false,
                      const std::string& doc = "")
{
    StdlibSig s;
    s.mangledName = mangled;
    s.plainName   = plain;
    s.returnType  = ret;
    s.params      = std::move(ps);
    s.noReturn    = noret;
    s.briefDoc    = doc;
    return s;
}

static StdlibParam par(const std::string& type, const std::string& name,
                        StdlibParamDir dir = StdlibParamDir::In)
{ return {type, name, dir}; }

// ── GCC/Clang (Itanium) stdlib signatures ────────────────────────────────────

static std::vector<StdlibSig> gccClangSigs() {
    std::vector<StdlibSig> v;
    // Memory
    v.push_back(sig("malloc","malloc","void*",{par("size_t","size")},false,"Allocate heap memory"));
    v.push_back(sig("calloc","calloc","void*",{par("size_t","nmemb"),par("size_t","size")}));
    v.push_back(sig("realloc","realloc","void*",{par("void*","ptr"),par("size_t","size")}));
    v.push_back(sig("free","free","void",{par("void*","ptr")},false,"Free heap memory"));
    v.push_back(sig("_Znwm","operator new","void*",{par("size_t","size")}));
    v.push_back(sig("_Znam","operator new[]","void*",{par("size_t","size")}));
    v.push_back(sig("_ZdlPv","operator delete","void",{par("void*","ptr")}));
    v.push_back(sig("_ZdaPv","operator delete[]","void",{par("void*","ptr")}));
    // String
    v.push_back(sig("strlen","strlen","size_t",{par("const char*","s")},false,"String length"));
    v.push_back(sig("strcpy","strcpy","char*",{par("char*","dst",StdlibParamDir::Out),par("const char*","src")}));
    v.push_back(sig("strncpy","strncpy","char*",{par("char*","dst",StdlibParamDir::Out),par("const char*","src"),par("size_t","n")}));
    v.push_back(sig("strcmp","strcmp","int",{par("const char*","s1"),par("const char*","s2")}));
    v.push_back(sig("strcat","strcat","char*",{par("char*","dst",StdlibParamDir::InOut),par("const char*","src")}));
    v.push_back(sig("memcpy","memcpy","void*",{par("void*","dst",StdlibParamDir::Out),par("const void*","src"),par("size_t","n")}));
    v.push_back(sig("memset","memset","void*",{par("void*","dst",StdlibParamDir::Out),par("int","c"),par("size_t","n")}));
    v.push_back(sig("memcmp","memcmp","int",{par("const void*","s1"),par("const void*","s2"),par("size_t","n")}));
    // I/O
    v.push_back(sig("printf","printf","int",{par("const char*","fmt")},false,"Formatted print to stdout"));
    v.push_back(sig("fprintf","fprintf","int",{par("FILE*","stream"),par("const char*","fmt")}));
    v.push_back(sig("sprintf","sprintf","int",{par("char*","buf",StdlibParamDir::Out),par("const char*","fmt")}));
    v.push_back(sig("puts","puts","int",{par("const char*","s")}));
    v.push_back(sig("fopen","fopen","FILE*",{par("const char*","path"),par("const char*","mode")}));
    v.push_back(sig("fclose","fclose","int",{par("FILE*","stream")}));
    v.push_back(sig("fread","fread","size_t",{par("void*","buf",StdlibParamDir::Out),par("size_t","size"),par("size_t","n"),par("FILE*","stream")}));
    v.push_back(sig("fwrite","fwrite","size_t",{par("const void*","buf"),par("size_t","size"),par("size_t","n"),par("FILE*","stream")}));
    // Control flow
    v.push_back(sig("exit","exit","void",{par("int","status")},true,"Exit process"));
    v.push_back(sig("abort","abort","void",{},true,"Abort process"));
    v.push_back(sig("_exit","_exit","void",{par("int","status")},true));
    // C++ runtime
    v.push_back(sig("__cxa_throw","__cxa_throw","void",{par("void*","exc"),par("std::type_info*","ti"),par("void(*)(void*)","dest")},true));
    v.push_back(sig("__cxa_rethrow","__cxa_rethrow","void",{},true));
    v.push_back(sig("__cxa_pure_virtual","__cxa_pure_virtual","void",{},true,"Pure virtual call"));
    v.push_back(sig("__cxa_bad_cast","__cxa_bad_cast","void",{},true));
    v.push_back(sig("__stack_chk_fail","__stack_chk_fail","void",{},true,"Stack canary failure"));
    // Math
    v.push_back(sig("sin","sin","double",{par("double","x")}));
    v.push_back(sig("cos","cos","double",{par("double","x")}));
    v.push_back(sig("sqrt","sqrt","double",{par("double","x")}));
    v.push_back(sig("pow","pow","double",{par("double","x"),par("double","y")}));
    return v;
}

// ── MSVC stdlib signatures ────────────────────────────────────────────────────

static std::vector<StdlibSig> msvcSigs() {
    std::vector<StdlibSig> v;
    v.push_back(sig("??2@YAPAXI@Z","operator new","void*",{par("size_t","size")}));
    v.push_back(sig("??3@YAXPAX@Z","operator delete","void",{par("void*","ptr")}));
    v.push_back(sig("??_U@YAPAXI@Z","operator new[]","void*",{par("size_t","size")}));
    v.push_back(sig("??_V@YAXPAX@Z","operator delete[]","void",{par("void*","ptr")}));
    v.push_back(sig("malloc","malloc","void*",{par("size_t","size")}));
    v.push_back(sig("free","free","void",{par("void*","ptr")}));
    v.push_back(sig("realloc","realloc","void*",{par("void*","ptr"),par("size_t","size")}));
    v.push_back(sig("strlen","strlen","size_t",{par("const char*","s")}));
    v.push_back(sig("strcpy","strcpy","char*",{par("char*","dst",StdlibParamDir::Out),par("const char*","src")}));
    v.push_back(sig("strcmp","strcmp","int",{par("const char*","s1"),par("const char*","s2")}));
    v.push_back(sig("memcpy","memcpy","void*",{par("void*","dst",StdlibParamDir::Out),par("const void*","src"),par("size_t","n")}));
    v.push_back(sig("memset","memset","void*",{par("void*","dst",StdlibParamDir::Out),par("int","c"),par("size_t","n")}));
    v.push_back(sig("printf","printf","int",{par("const char*","fmt")}));
    v.push_back(sig("sprintf","sprintf","int",{par("char*","buf",StdlibParamDir::Out),par("const char*","fmt")}));
    v.push_back(sig("fopen","fopen","FILE*",{par("const char*","path"),par("const char*","mode")}));
    v.push_back(sig("fclose","fclose","int",{par("FILE*","stream")}));
    v.push_back(sig("exit","exit","void",{par("int","status")},true));
    v.push_back(sig("abort","abort","void",{},true));
    v.push_back(sig("_CxxThrowException","_CxxThrowException","void",{par("void*","exc"),par("_ThrowInfo*","ti")},true));
    v.push_back(sig("__chkstk","__chkstk","void",{},false,"Stack probe"));
    v.push_back(sig("_alloca","_alloca","void*",{par("size_t","size")},false,"Stack alloc"));
    v.push_back(sig("_msize","_msize","size_t",{par("void*","ptr")}));
    return v;
}

// ── Borland stdlib signatures ─────────────────────────────────────────────────

static std::vector<StdlibSig> borlandSigs() {
    std::vector<StdlibSig> v;
    // VCL runtime
    v.push_back(sig("@$bctr$qqrv","TObject::TObject","void",{}));
    v.push_back(sig("@$bdtr$qqrv","TObject::~TObject","void",{}));
    v.push_back(sig("@TObject@Free$qqrv","TObject::Free","void",{}));
    v.push_back(sig("@TObject@ClassName$qqrv","TObject::ClassName","AnsiString",{}));
    v.push_back(sig("@TObject@ClassType$qqrv","TObject::ClassType","TClass",{}));
    v.push_back(sig("@TObject@InheritsFrom$qqrpvTMetaClass$","TObject::InheritsFrom","bool",{par("TClass","aClass")}));
    v.push_back(sig("@Classes@TComponent@$bctr$qqrp17Classes@TComponent","TComponent::TComponent","void",{par("TComponent*","owner")}));
    // RTL
    v.push_back(sig("malloc","malloc","void*",{par("size_t","size")}));
    v.push_back(sig("free","free","void",{par("void*","ptr")}));
    v.push_back(sig("_new","operator new","void*",{par("size_t","size")}));
    v.push_back(sig("_del","operator delete","void",{par("void*","ptr")}));
    v.push_back(sig("strlen","strlen","size_t",{par("const char*","s")}));
    v.push_back(sig("AnsiString","AnsiString::AnsiString","void",{par("const char*","s")}));
    v.push_back(sig("printf","printf","int",{par("const char*","fmt")}));
    v.push_back(sig("exit","exit","void",{par("int","status")},true));
    v.push_back(sig("abort","abort","void",{},true));
    // Borland exceptions
    v.push_back(sig("_RaiseException","_RaiseException","void",{par("DWORD","code"),par("DWORD","flags"),par("DWORD","nargs"),par("const ULONG_PTR*","args")},true));
    v.push_back(sig("__terminate","terminate","void",{},true));
    return v;
}

// ── DMC stdlib signatures ─────────────────────────────────────────────────────

static std::vector<StdlibSig> dmcSigs() {
    std::vector<StdlibSig> v;
    v.push_back(sig("malloc","malloc","void*",{par("size_t","size")}));
    v.push_back(sig("free","free","void",{par("void*","ptr")}));
    v.push_back(sig("_new","operator new","void*",{par("size_t","size")}));
    v.push_back(sig("_delete","operator delete","void",{par("void*","ptr")}));
    v.push_back(sig("strlen","strlen","size_t",{par("const char*","s")}));
    v.push_back(sig("printf","printf","int",{par("const char*","fmt")}));
    v.push_back(sig("exit","exit","void",{par("int","status")},true));
    v.push_back(sig("abort","abort","void",{},true));
    v.push_back(sig("_Throw","_Throw","void",{par("void*","exc")},true));
    v.push_back(sig("__terminate","terminate","void",{},true));
    return v;
}

// ── Watcom stdlib signatures ──────────────────────────────────────────────────

static std::vector<StdlibSig> watcomSigs() {
    std::vector<StdlibSig> v;
    v.push_back(sig("malloc_","malloc","void*",{par("size_t","size")}));
    v.push_back(sig("free_","free","void",{par("void*","ptr")}));
    v.push_back(sig("realloc_","realloc","void*",{par("void*","ptr"),par("size_t","size")}));
    v.push_back(sig("?new","operator new","void*",{par("size_t","size")}));
    v.push_back(sig("?delete","operator delete","void",{par("void*","ptr")}));
    v.push_back(sig("strlen_","strlen","size_t",{par("const char*","s")}));
    v.push_back(sig("printf_","printf","int",{par("const char*","fmt")}));
    v.push_back(sig("sprintf_","sprintf","int",{par("char*","buf",StdlibParamDir::Out),par("const char*","fmt")}));
    v.push_back(sig("exit_","exit","void",{par("int","status")},true));
    v.push_back(sig("abort_","abort","void",{},true));
    v.push_back(sig("__ThrowException","__ThrowException","void",{par("void*","exc")},true));
    v.push_back(sig("__terminate","terminate","void",{},true));
    return v;
}

// ── Symbian stdlib signatures ─────────────────────────────────────────────────

static std::vector<StdlibSig> symbianSigs() {
    std::vector<StdlibSig> v;
    // Allocators
    v.push_back(sig("User::Alloc","User::Alloc","TAny*",{par("TInt","aSize")}));
    v.push_back(sig("User::AllocL","User::AllocL","TAny*",{par("TInt","aSize")},false,"Allocate, leaving on OOM"));
    v.push_back(sig("User::ReAlloc","User::ReAlloc","TAny*",{par("TAny*","aCell"),par("TInt","aSize")}));
    v.push_back(sig("User::Free","User::Free","void",{par("TAny*","aCell")}));
    // EH/Leave
    v.push_back(sig("User::Leave","User::Leave","void",{par("TInt","aReason")},true,"Symbian Leave (= throw)"));
    v.push_back(sig("User::LeaveIfError","User::LeaveIfError","void",{par("TInt","aError")},false,"Leave if negative error code"));
    v.push_back(sig("User::LeaveNoMemory","User::LeaveNoMemory","void",{},true));
    v.push_back(sig("User::Panic","User::Panic","void",{par("const TDesC&","aCategory"),par("TInt","aReason")},true));
    // CleanupStack
    v.push_back(sig("CleanupStack::PushL","CleanupStack::PushL","void",{par("CBase*","aPtr")}));
    v.push_back(sig("CleanupStack::Pop","CleanupStack::Pop","void",{}));
    v.push_back(sig("CleanupStack::PopAndDestroy","CleanupStack::PopAndDestroy","void",{}));
    // Descriptors
    v.push_back(sig("TDesC::Length","TDesC::Length","TInt",{}));
    v.push_back(sig("TDes::Copy","TDes::Copy","void",{par("const TDesC&","aSrc")}));
    // Common
    v.push_back(sig("Mem::Copy","Mem::Copy","TAny*",{par("TAny*","aTrg",StdlibParamDir::Out),par("const TAny*","aSrc"),par("TInt","aLength")}));
    v.push_back(sig("Mem::Fill","Mem::Fill","void",{par("TAny*","aTrg",StdlibParamDir::Out),par("TInt","aLength"),par("TUint8","aValue")}));
    v.push_back(sig("Mem::Compare","Mem::Compare","TInt",{par("const TAny*","aLeft"),par("TInt","aLeftL"),par("const TAny*","aRight"),par("TInt","aRightL")}));
    // Active objects
    v.push_back(sig("CActiveScheduler::Start","CActiveScheduler::Start","void",{}));
    v.push_back(sig("CActiveScheduler::Stop","CActiveScheduler::Stop","void",{}));
    v.push_back(sig("CActive::SetActive","CActive::SetActive","void",{}));
    v.push_back(sig("CActive::Cancel","CActive::Cancel","void",{}));
    return v;
}

} // anon namespace

// ─── stdlibSignatures ────────────────────────────────────────────────────────

std::vector<StdlibSig> stdlibSignatures(CompilerFamily f) {
    switch (f) {
    case CompilerFamily::GCC_Clang: return gccClangSigs();
    case CompilerFamily::MSVC:      return msvcSigs();
    case CompilerFamily::Borland:   return borlandSigs();
    case CompilerFamily::DMC:       return dmcSigs();
    case CompilerFamily::Watcom:    return watcomSigs();
    case CompilerFamily::Symbian:   return symbianSigs();
    default:                         return {};
    }
}

// ─── standardCallingConventions ──────────────────────────────────────────────

std::vector<CallingConventionDesc> standardCallingConventions(Arch arch) {
    switch (arch) {
    case Arch::X86_32:
        return {cdecl_x86(), stdcall_x86(), msvc_thiscall_x86(),
                msvc_fastcall_x86(), borland_register_x86(),
                borland_pascal_x86(), watcom_register_x86()};
    case Arch::X86_64:
        return {sysv_amd64(), win64_cc()};
    case Arch::ARM32:
        return {arm_aapcs()};
    case Arch::ARM64:
        return {aarch64_aapcs()};
    default:
        return {};
    }
}

// ─── AbiDescriptor::forCompiler ───────────────────────────────────────────────

AbiDescriptor AbiDescriptor::forCompiler(CompilerFamily f, Arch a) {
    AbiDescriptor d;
    d.family    = f;
    d.arch      = a;
    d.stdlibSigs= stdlibSignatures(f);

    switch (f) {
    case CompilerFamily::GCC_Clang:
        if (a == Arch::X86_64) {
            d.defaultCC  = sysv_amd64();
            d.memberCC   = sysv_amd64();
            d.variadicCC = sysv_amd64();
        } else if (a == Arch::X86_32) {
            d.defaultCC  = cdecl_x86();
            d.memberCC   = cdecl_x86(); // GCC uses cdecl for `this` too
            d.memberCC.this_reg = RegId::ECX; // pushed as first arg
            d.variadicCC = cdecl_x86();
        } else if (a == Arch::ARM32) {
            d.defaultCC = d.memberCC = d.variadicCC = arm_aapcs();
        } else if (a == Arch::ARM64) {
            d.defaultCC = d.memberCC = d.variadicCC = aarch64_aapcs();
        }
        d.ehModel = itaniumDwarfEH();
        break;

    case CompilerFamily::MSVC:
        if (a == Arch::X86_64) {
            d.defaultCC  = win64_cc();
            d.memberCC   = win64_cc();
            d.variadicCC = win64_cc();
        } else {
            d.defaultCC  = cdecl_x86();
            d.memberCC   = msvc_thiscall_x86();
            d.variadicCC = cdecl_x86();
        }
        d.ehModel = msvcEH();
        break;

    case CompilerFamily::Borland:
        d.defaultCC  = borland_register_x86();
        d.memberCC   = borland_register_x86();
        d.variadicCC = cdecl_x86(); // variadic falls back to cdecl
        d.ehModel    = borlandEH();
        break;

    case CompilerFamily::DMC:
        d.defaultCC  = cdecl_x86();
        d.memberCC   = cdecl_x86();
        d.memberCC.this_reg = RegId::ECX;
        d.variadicCC = cdecl_x86();
        d.ehModel    = dmcEH();
        break;

    case CompilerFamily::Watcom:
        d.defaultCC  = watcom_register_x86();
        d.memberCC   = watcom_register_x86();
        d.variadicCC = cdecl_x86(); // variadic must use stack
        d.ehModel    = watcomEH();
        break;

    case CompilerFamily::Symbian:
        // Symbian is ARM32
        d.defaultCC  = arm_aapcs();
        d.memberCC   = arm_aapcs();
        d.variadicCC = arm_aapcs();
        d.ehModel    = symbianLeave();
        break;

    default:
        break;
    }

    return d;
}

// ─── AbiDescriptor::findSig ───────────────────────────────────────────────────

const StdlibSig* AbiDescriptor::findSig(const std::string& name) const noexcept {
    for (const auto& s : stdlibSigs) {
        if (s.mangledName == name || s.plainName == name)
            return &s;
    }
    return nullptr;
}

// ─── Name demanglers ──────────────────────────────────────────────────────────

// ── GCC/Clang demangler ───────────────────────────────────────────────────────

class GccClangDemangler : public INameDemangler {
public:
    CompilerFamily family() const noexcept override { return CompilerFamily::GCC_Clang; }

    bool isMangled(const std::string& s) const noexcept override {
        return s.size() > 2 && s[0] == '_' && s[1] == 'Z';
    }

    std::string demangle(const std::string& m) const override {
#ifdef RETDEC_USE_CXXABI_DEMANGLE
        int status = 0;
        char* d = abi::__cxa_demangle(m.c_str(), nullptr, nullptr, &status);
        if (status == 0 && d) {
            std::string result(d);
            free(d);
            return result;
        }
#endif
        // Minimal built-in fallback
        if (!isMangled(m)) return m;
        const char* p   = m.data() + 2;
        const char* end = m.data() + m.size();

        // Skip leading T/V/I/N type-tag combos
        if (p < end && *p == 'T') {
            ++p;
            if (p < end && (*p == 'V'||*p == 'I'||*p == 'S'||*p == 'C')) ++p;
        }

        // Decode nested name N...E or simple length-prefixed name
        std::string result;
        if (p < end && *p == 'N') {
            ++p;
            while (p < end && *p != 'E') {
                if (!std::isdigit(static_cast<unsigned char>(*p))) { ++p; continue; }
                int len = 0;
                while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
                    len = len * 10 + (*p++ - '0');
                if (len <= 0 || p + len > end) break;
                if (!result.empty()) result += "::";
                result.append(p, len);
                p += len;
            }
        } else {
            if (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
                int len = 0;
                while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
                    len = len * 10 + (*p++ - '0');
                if (len > 0 && p + len <= end)
                    result.assign(p, len);
            }
        }
        return result.empty() ? m : result;
    }
};

// ── MSVC demangler ────────────────────────────────────────────────────────────

class MsvcDemangler : public INameDemangler {
public:
    CompilerFamily family() const noexcept override { return CompilerFamily::MSVC; }

    bool isMangled(const std::string& s) const noexcept override {
        return !s.empty() && s[0] == '?';
    }

    std::string demangle(const std::string& m) const override {
        if (!isMangled(m)) return m;
        // Minimal: split on '@', reverse, join with '::'
        std::vector<std::string> parts;
        std::size_t pos = 1; // skip leading '?'
        while (pos < m.size()) {
            std::size_t at = m.find('@', pos);
            if (at == std::string::npos) {
                std::string p = m.substr(pos);
                if (!p.empty() && p != "@") parts.push_back(p);
                break;
            }
            std::string p = m.substr(pos, at - pos);
            if (!p.empty()) parts.push_back(p);
            pos = at + 1;
        }
        if (parts.empty()) return m;
        std::reverse(parts.begin(), parts.end());
        // Drop empty trailing parts
        while (!parts.empty() && parts.back().empty()) parts.pop_back();
        std::string result;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) result += "::";
            result += parts[i];
        }
        return result.empty() ? m : result;
    }
};

// ── Borland demangler ─────────────────────────────────────────────────────────

class BorlandDemangler : public INameDemangler {
public:
    CompilerFamily family() const noexcept override { return CompilerFamily::Borland; }

    bool isMangled(const std::string& s) const noexcept override {
        return s.size() > 1 && s[0] == '@';
    }

    std::string demangle(const std::string& m) const override {
        if (!isMangled(m)) return m;
        // Borland names: @ClassName@Method$signature or @ClassName$bctr$...
        // Strip leading '@', replace '@' with '::', strip '$...' suffixes.
        std::string s = m.substr(1); // drop leading '@'
        std::string result;
        bool inSig = false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '$') { inSig = true; break; }
            if (s[i] == '@') { result += "::"; continue; }
            result += s[i];
        }
        return result.empty() ? m : result;
    }
};

// ── DMC demangler ─────────────────────────────────────────────────────────────

class DmcDemangler : public INameDemangler {
public:
    CompilerFamily family() const noexcept override { return CompilerFamily::DMC; }

    bool isMangled(const std::string& s) const noexcept override {
        // DMC mangling uses '__' prefix for C++ symbols
        return s.size() > 2 && s[0] == '_' && s[1] == '_';
    }

    std::string demangle(const std::string& m) const override {
        if (!isMangled(m)) return m;
        // DMC decorated names: __ClassName__Method for simple cases
        // Strip leading '__', replace '__' with '::'
        std::string s = m.substr(2);
        std::string result;
        std::size_t i = 0;
        while (i < s.size()) {
            if (i + 1 < s.size() && s[i] == '_' && s[i+1] == '_') {
                result += "::";
                i += 2;
            } else {
                result += s[i++];
            }
        }
        return result.empty() ? m : result;
    }
};

// ── Watcom demangler ──────────────────────────────────────────────────────────

class WatcomDemangler : public INameDemangler {
public:
    CompilerFamily family() const noexcept override { return CompilerFamily::Watcom; }

    bool isMangled(const std::string& s) const noexcept override {
        // Watcom uses 'W?' prefix or '__watcpp' prefix
        return (s.size() > 2 && s[0] == 'W' && s[1] == '?') ||
               (s.size() > 7 && s.substr(0,7) == "W?__cpp");
    }

    std::string demangle(const std::string& m) const override {
        if (!isMangled(m)) return m;
        // Strip 'W?' prefix, then decode similarly to MSVC '@' separators
        std::string s = m.substr(2);
        // Watcom uses '$' as component separator in some forms
        std::string result;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '$') { result += "::"; continue; }
            result += s[i];
        }
        return result.empty() ? m : result;
    }
};

// ── Symbian demangler ─────────────────────────────────────────────────────────

class SymbianDemangler : public INameDemangler {
public:
    CompilerFamily family() const noexcept override { return CompilerFamily::Symbian; }

    bool isMangled(const std::string& s) const noexcept override {
        // Symbian uses RVCT mangling (similar to ARM EABI / Itanium):
        // starts with '_Z' or is a plain identifier.
        return (s.size() > 2 && s[0] == '_' && s[1] == 'Z') ||
               (!s.empty() && (s[0] == 'C' || s[0] == 'T' ||
                               s[0] == 'R' || s[0] == 'M'));
    }

    std::string demangle(const std::string& m) const override {
        if (m.size() > 2 && m[0] == '_' && m[1] == 'Z') {
            // Delegate to Itanium-style demangling
            return GccClangDemangler{}.demangle(m);
        }
        // Plain Symbian convention names are already human-readable.
        return m;
    }
};

// ─── makeDemangler factory ────────────────────────────────────────────────────

std::unique_ptr<INameDemangler> makeDemangler(CompilerFamily f) {
    switch (f) {
    case CompilerFamily::GCC_Clang: return std::make_unique<GccClangDemangler>();
    case CompilerFamily::MSVC:      return std::make_unique<MsvcDemangler>();
    case CompilerFamily::Borland:   return std::make_unique<BorlandDemangler>();
    case CompilerFamily::DMC:       return std::make_unique<DmcDemangler>();
    case CompilerFamily::Watcom:    return std::make_unique<WatcomDemangler>();
    case CompilerFamily::Symbian:   return std::make_unique<SymbianDemangler>();
    default:                         return std::make_unique<GccClangDemangler>();
    }
}

} // namespace compiler_abi
} // namespace retdec
