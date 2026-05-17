/**
 * @file tests/call_conv/call_conv_test.cpp
 * @brief Unit tests for call_conv module (Stage 21).
 *
 * Test groups:
 *   1.  ABI register tables      — intArgRegs / fpArgRegs / intRetRegs / fpRetRegs
 *   2.  PhysReg names            — physRegName for all architectures
 *   3.  CC names                 — ccName for all CCs
 *   4.  ArgDesc / RetDesc toString
 *   5.  CallerCleanupDetector    — cdecl/stdcall/fastcall/thiscall detection
 *   6.  RegArgAnalysis           — liveness-based argument register count
 *   7.  ReturnValueAnalysis      — RAX / XMM0 live-out detection
 *   8.  VariadicDetector         — AL live-in / va_list pattern
 *   9.  CallConvPass             — full pipeline + stats
 *   10. CallingConvention        — toString, batch runAll
 */

#include "retdec/call_conv/call_conv.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <memory>

using namespace retdec::call_conv;
using namespace retdec::ssa;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

/// Create a minimal function with declared register variables.
std::unique_ptr<SSAFunction> makeRegFn(
        const std::vector<std::string>& liveInNames,
        const std::vector<std::string>& retLiveOutNames = {},
        bool addRetInstr = true,
        const std::string& fnName = "test") {

    auto fn = std::make_unique<SSAFunction>(fnName);
    BasicBlock* entry = fn->addBlock("entry");

    // Declare variables for all named registers.
    std::vector<VarId> liveInVars;
    for (const auto& name : liveInNames) {
        VarId v = fn->declareVar(name, 64);
        liveInVars.push_back(v);
    }

    // Declare return variables.
    std::vector<VarId> retVars;
    for (const auto& name : retLiveOutNames) {
        VarId v = fn->findVar(name);
        if (v == kInvalidVar) {
            v = fn->declareVar(name, 64);
        }
        retVars.push_back(v);
    }

    if (addRetInstr) {
        auto* ret = fn->addInstr(0, IrInstr::Op::Ret);
        (void)ret;
    }

    // Run SSA pass to populate dominator info (this also recomputes liveness
    // from instructions, clearing any pre-set live sets).
    SSAPass pass;
    pass.run(*fn);

    // Re-seed liveIn / liveOut AFTER SSAPass so RegArgAnalysis / ReturnValue-
    // Analysis see the registers we intend.  SSAPass::run clears these from
    // instructions only, so we must restore them here.
    entry = fn->block(fn->entryId());
    for (VarId v : liveInVars) {
        entry->liveIn.insert(v);
        entry->gen.insert(v);
    }
    for (VarId v : retVars) {
        entry->liveOut.insert(v);
    }

    return fn;
}

/// Create a minimal function with a CALL followed by an ADD (caller-cleanup).
std::unique_ptr<SSAFunction> makeCallFn(bool callerCleanup, int numCalls = 1) {
    auto fn = std::make_unique<SSAFunction>("caller");
    BasicBlock* entry = fn->addBlock("entry");

    for (int i = 0; i < numCalls; ++i) {
        fn->addInstr(0, IrInstr::Op::Call);
        if (callerCleanup) {
            // ADD ESP, 4 after the call
            auto* add = fn->addInstr(0, IrInstr::Op::Add);
            (void)add;
        }
    }
    fn->addInstr(0, IrInstr::Op::Ret);

    SSAPass pass;
    pass.run(*fn);
    return fn;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// 1. ABI register tables
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AbiRegTables, SysVAmd64_IntArgRegs_6) {
    auto regs = intArgRegs(CC::SysVAmd64);
    ASSERT_EQ(regs.size(), 6u);
    EXPECT_EQ(regs[0], PhysReg::RDI);
    EXPECT_EQ(regs[1], PhysReg::RSI);
    EXPECT_EQ(regs[2], PhysReg::RDX);
    EXPECT_EQ(regs[3], PhysReg::RCX);
    EXPECT_EQ(regs[4], PhysReg::R8);
    EXPECT_EQ(regs[5], PhysReg::R9);
}

TEST(AbiRegTables, SysVAmd64_FpArgRegs_8) {
    auto regs = fpArgRegs(CC::SysVAmd64);
    ASSERT_EQ(regs.size(), 8u);
    EXPECT_EQ(regs[0], PhysReg::XMM0);
    EXPECT_EQ(regs[7], PhysReg::XMM7);
}

TEST(AbiRegTables, Win64_IntArgRegs_4) {
    auto regs = intArgRegs(CC::Win64);
    ASSERT_EQ(regs.size(), 4u);
    EXPECT_EQ(regs[0], PhysReg::RCX);
    EXPECT_EQ(regs[1], PhysReg::RDX);
    EXPECT_EQ(regs[2], PhysReg::R8);
    EXPECT_EQ(regs[3], PhysReg::R9);
}

TEST(AbiRegTables, Win64_FpArgRegs_4) {
    auto regs = fpArgRegs(CC::Win64);
    ASSERT_EQ(regs.size(), 4u);
    EXPECT_EQ(regs[0], PhysReg::XMM0);
    EXPECT_EQ(regs[3], PhysReg::XMM3);
}

TEST(AbiRegTables, AArch64_IntArgRegs_8) {
    auto regs = intArgRegs(CC::AArch64SysV);
    ASSERT_EQ(regs.size(), 8u);
    EXPECT_EQ(regs[0], PhysReg::X0);
    EXPECT_EQ(regs[7], PhysReg::X7);
}

TEST(AbiRegTables, AArch64_FpArgRegs_8) {
    auto regs = fpArgRegs(CC::AArch64SysV);
    ASSERT_EQ(regs.size(), 8u);
    EXPECT_EQ(regs[0], PhysReg::V0);
}

TEST(AbiRegTables, Arm32_IntArgRegs_4) {
    auto regs = intArgRegs(CC::Arm32Aapcs);
    ASSERT_EQ(regs.size(), 4u);
    EXPECT_EQ(regs[0], PhysReg::R0);
    EXPECT_EQ(regs[3], PhysReg::R3);
}

TEST(AbiRegTables, Cdecl_NoArgRegs) {
    EXPECT_TRUE(intArgRegs(CC::Cdecl).empty());
    EXPECT_TRUE(fpArgRegs(CC::Cdecl).empty());
}

TEST(AbiRegTables, SysV_RetRegs_HasRaxRdx) {
    auto regs = intRetRegs(CC::SysVAmd64);
    ASSERT_GE(regs.size(), 1u);
    EXPECT_EQ(regs[0], PhysReg::RAX);
}

TEST(AbiRegTables, Win64_RetReg_OnlyRax) {
    auto regs = intRetRegs(CC::Win64);
    ASSERT_EQ(regs.size(), 1u);
    EXPECT_EQ(regs[0], PhysReg::RAX);
}

TEST(AbiRegTables, SysV_FpRetReg_Xmm0) {
    auto regs = fpRetRegs(CC::SysVAmd64);
    ASSERT_EQ(regs.size(), 1u);
    EXPECT_EQ(regs[0], PhysReg::XMM0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 2. PhysReg names
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PhysRegName, CommonRegisters) {
    EXPECT_STREQ(physRegName(PhysReg::RAX),  "rax");
    EXPECT_STREQ(physRegName(PhysReg::RDI),  "rdi");
    EXPECT_STREQ(physRegName(PhysReg::RSI),  "rsi");
    EXPECT_STREQ(physRegName(PhysReg::XMM0), "xmm0");
    EXPECT_STREQ(physRegName(PhysReg::XMM7), "xmm7");
    EXPECT_STREQ(physRegName(PhysReg::X0),   "x0");
    EXPECT_STREQ(physRegName(PhysReg::V0),   "v0");
    EXPECT_STREQ(physRegName(PhysReg::R0),   "r0");
    EXPECT_STREQ(physRegName(PhysReg::AL),   "al");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3. CC names
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CcName, AllConventions) {
    EXPECT_STREQ(ccName(CC::Cdecl),       "cdecl");
    EXPECT_STREQ(ccName(CC::Stdcall),     "stdcall");
    EXPECT_STREQ(ccName(CC::Fastcall),    "fastcall");
    EXPECT_STREQ(ccName(CC::Thiscall),    "thiscall");
    EXPECT_STREQ(ccName(CC::SysVAmd64),   "sysv_amd64");
    EXPECT_STREQ(ccName(CC::Win64),       "win64");
    EXPECT_STREQ(ccName(CC::AArch64SysV), "aarch64_sysv");
    EXPECT_STREQ(ccName(CC::Arm32Aapcs),  "arm32_aapcs");
    EXPECT_STREQ(ccName(CC::Unknown),     "unknown");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 4. ArgDesc / RetDesc toString
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ArgDescToString, RegisterArg) {
    ArgDesc d;
    d.kind  = ArgKind::Register;
    d.reg   = PhysReg::RDI;
    d.width = 64;
    d.isFp  = false;
    auto s = d.toString();
    EXPECT_NE(s.find("rdi"), std::string::npos);
    EXPECT_NE(s.find("64"), std::string::npos);
}

TEST(ArgDescToString, FpArg) {
    ArgDesc d;
    d.kind  = ArgKind::Register;
    d.reg   = PhysReg::XMM0;
    d.width = 64;
    d.isFp  = true;
    auto s = d.toString();
    EXPECT_NE(s.find("xmm0"), std::string::npos);
    EXPECT_NE(s.find("fp"), std::string::npos);
}

TEST(ArgDescToString, StackArg) {
    ArgDesc d;
    d.kind        = ArgKind::Stack;
    d.stackOffset = 8;
    d.width       = 32;
    auto s = d.toString();
    EXPECT_NE(s.find("stack"), std::string::npos);
}

TEST(RetDescToString, Void) {
    RetDesc r;
    r.kind = RetKind::Void;
    EXPECT_EQ(r.toString(), "void");
}

TEST(RetDescToString, IntRax) {
    RetDesc r;
    r.kind  = RetKind::Integer;
    r.width = 64;
    r.regs  = { PhysReg::RAX };
    auto s = r.toString();
    EXPECT_NE(s.find("rax"), std::string::npos);
}

TEST(CallingConventionToString, Basic) {
    CallingConvention cc;
    cc.cc  = CC::SysVAmd64;
    cc.ret.kind = RetKind::Void;
    auto s = cc.toString();
    EXPECT_NE(s.find("sysv"), std::string::npos);
}

TEST(CallingConventionToString, Variadic) {
    CallingConvention cc;
    cc.cc = CC::Cdecl;
    cc.isVariadic = true;
    cc.ret.kind = RetKind::Void;
    auto s = cc.toString();
    EXPECT_NE(s.find("variadic"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 5. CallerCleanupDetector
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CallerCleanupDetector, NoCallSites_Unknown) {
    auto fn = std::make_unique<SSAFunction>("no_calls");
    fn->addBlock("entry");
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    CallerCleanupDetector det;
    auto res = det.run(*fn);
    EXPECT_EQ(res.cc, CC::Unknown);
    EXPECT_TRUE(res.sites.empty());
}

TEST(CallerCleanupDetector, CallerCleanup_Cdecl) {
    // CALL followed by ADD → cdecl
    auto fn = makeCallFn(true, 3);
    CallerCleanupDetector det;
    auto res = det.run(*fn);
    EXPECT_EQ(res.cc, CC::Cdecl);
    EXPECT_GT(res.callerCleanupVotes, 0);
}

TEST(CallerCleanupDetector, CalleeCleanup_Stdcall) {
    // CALL not followed by ADD → stdcall (majority)
    auto fn = makeCallFn(false, 3);
    CallerCleanupDetector det;
    auto res = det.run(*fn);
    EXPECT_NE(res.cc, CC::Cdecl);
    EXPECT_GT(res.calleeCleanupVotes, 0);
}

TEST(CallerCleanupDetector, MixedSites_MajorityWins) {
    auto fn = std::make_unique<SSAFunction>("mixed");
    fn->addBlock("entry");
    // 4 calls: 3 caller-cleanup, 1 callee
    for (int i = 0; i < 3; ++i) {
        fn->addInstr(0, IrInstr::Op::Call);
        fn->addInstr(0, IrInstr::Op::Add);
    }
    fn->addInstr(0, IrInstr::Op::Call);  // no ADD after
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    CallerCleanupDetector det;
    auto res = det.run(*fn);
    EXPECT_EQ(res.cc, CC::Cdecl);  // majority caller-cleanup
}

// ═══════════════════════════════════════════════════════════════════════════════
// 6. RegArgAnalysis
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RegArgAnalysis, SysV_ThreeIntArgs) {
    // rdi, rsi, rdx live-in → 3 integer args
    auto fn = makeRegFn({"rdi", "rsi", "rdx"});
    RegArgAnalysis ana;
    auto args = ana.run(*fn, CC::SysVAmd64);
    EXPECT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0].reg, PhysReg::RDI);
    EXPECT_EQ(args[1].reg, PhysReg::RSI);
    EXPECT_EQ(args[2].reg, PhysReg::RDX);
    for (const auto& a : args) EXPECT_FALSE(a.isFp);
}

TEST(RegArgAnalysis, SysV_OneFloatArg) {
    // xmm0 live-in → 1 float arg
    auto fn = makeRegFn({"xmm0"});
    RegArgAnalysis ana;
    auto args = ana.run(*fn, CC::SysVAmd64);
    ASSERT_GE(args.size(), 1u);
    // Find the fp arg
    bool foundFp = false;
    for (const auto& a : args) {
        if (a.isFp && a.reg == PhysReg::XMM0) foundFp = true;
    }
    EXPECT_TRUE(foundFp);
}

TEST(RegArgAnalysis, SysV_NoLiveInRegs_NoArgs) {
    auto fn = makeRegFn({});
    RegArgAnalysis ana;
    auto args = ana.run(*fn, CC::SysVAmd64);
    EXPECT_TRUE(args.empty());
}

TEST(RegArgAnalysis, Win64_TwoArgs) {
    // rcx, rdx live-in → 2 args
    auto fn = makeRegFn({"rcx", "rdx"});
    RegArgAnalysis ana;
    auto args = ana.run(*fn, CC::Win64);
    EXPECT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0].reg, PhysReg::RCX);
    EXPECT_EQ(args[1].reg, PhysReg::RDX);
}

TEST(RegArgAnalysis, AArch64_FourArgs) {
    auto fn = makeRegFn({"x0", "x1", "x2", "x3"});
    RegArgAnalysis ana;
    auto args = ana.run(*fn, CC::AArch64SysV);
    EXPECT_EQ(args.size(), 4u);
    EXPECT_EQ(args[0].reg, PhysReg::X0);
    EXPECT_EQ(args[3].reg, PhysReg::X3);
}

TEST(RegArgAnalysis, Cdecl_NoRegArgs) {
    auto fn = makeRegFn({"eax", "ecx"});
    RegArgAnalysis ana;
    auto args = ana.run(*fn, CC::Cdecl);
    // cdecl has no int arg registers → empty
    EXPECT_TRUE(args.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// 7. ReturnValueAnalysis
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ReturnValueAnalysis, SysV_VoidReturn) {
    // Neither rax nor xmm0 live-out at RET → void
    auto fn = makeRegFn({}, {}, true);
    ReturnValueAnalysis ana;
    auto ret = ana.run(*fn, CC::SysVAmd64);
    EXPECT_EQ(ret.kind, RetKind::Void);
}

TEST(ReturnValueAnalysis, SysV_IntReturnRax) {
    auto fn = makeRegFn({}, {"rax"}, true);
    ReturnValueAnalysis ana;
    auto ret = ana.run(*fn, CC::SysVAmd64);
    EXPECT_EQ(ret.kind, RetKind::Integer);
    EXPECT_FALSE(ret.regs.empty());
}

TEST(ReturnValueAnalysis, SysV_FloatReturnXmm0) {
    auto fn = makeRegFn({}, {"xmm0"}, true);
    ReturnValueAnalysis ana;
    auto ret = ana.run(*fn, CC::SysVAmd64);
    EXPECT_EQ(ret.kind, RetKind::Float);
    EXPECT_TRUE(ret.isFp);
}

TEST(ReturnValueAnalysis, SysV_128bitReturn_RaxRdx) {
    auto fn = makeRegFn({}, {"rax", "rdx"}, true);
    ReturnValueAnalysis ana;
    auto ret = ana.run(*fn, CC::SysVAmd64);
    EXPECT_EQ(ret.kind, RetKind::Struct);
    EXPECT_EQ(ret.width, 128u);
}

TEST(ReturnValueAnalysis, Win64_IntReturn) {
    auto fn = makeRegFn({}, {"rax"}, true);
    ReturnValueAnalysis ana;
    auto ret = ana.run(*fn, CC::Win64);
    EXPECT_EQ(ret.kind, RetKind::Integer);
}

TEST(ReturnValueAnalysis, AArch64_IntReturn) {
    auto fn = makeRegFn({}, {"x0"}, true);
    ReturnValueAnalysis ana;
    auto ret = ana.run(*fn, CC::AArch64SysV);
    EXPECT_EQ(ret.kind, RetKind::Integer);
}

TEST(ReturnValueAnalysis, NoRetInstr_Void) {
    auto fn = std::make_unique<SSAFunction>("noret");
    fn->addBlock("entry");
    fn->addInstr(0, IrInstr::Op::Branch);
    SSAPass pass; pass.run(*fn);
    ReturnValueAnalysis ana;
    auto ret = ana.run(*fn, CC::SysVAmd64);
    EXPECT_EQ(ret.kind, RetKind::Void);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 8. VariadicDetector
// ═══════════════════════════════════════════════════════════════════════════════

TEST(VariadicDetector, SysV_AlLiveIn_IsVariadic) {
    auto fn = makeRegFn({"al"});
    VariadicDetector det;
    EXPECT_TRUE(det.run(*fn, CC::SysVAmd64));
}

TEST(VariadicDetector, SysV_NoAlLiveIn_NotVariadic) {
    auto fn = makeRegFn({"rdi", "rsi"});
    VariadicDetector det;
    EXPECT_FALSE(det.run(*fn, CC::SysVAmd64));
}

TEST(VariadicDetector, Win64_VaListPattern_Detected) {
    // Create a function with 3 ADD +8 to the same value.
    auto fn = std::make_unique<SSAFunction>("varfn");
    fn->addBlock("entry");

    // Declare a pointer variable.
    VarId ptrVar = fn->declareVar("va_ptr", 64);
    IrValue* ptrVal = fn->allocValue(ValueKind::VirtualReg, ptrVar);

    // Create immediate 8.
    IrValue* imm8 = fn->allocValue(ValueKind::Immediate, kInvalidVar);
    imm8->imm = 8;

    for (int i = 0; i < 3; ++i) {
        auto* add = fn->addInstr(0, IrInstr::Op::Add);
        add->uses.push_back({ptrVal->id, 0});
        add->uses.push_back({imm8->id, 1});
    }
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    VariadicDetector det;
    EXPECT_TRUE(det.run(*fn, CC::Win64));
}

TEST(VariadicDetector, Win64_NoVaListPattern_NotVariadic) {
    auto fn = makeRegFn({"rcx", "rdx"});
    VariadicDetector det;
    EXPECT_FALSE(det.run(*fn, CC::Win64));
}

// ═══════════════════════════════════════════════════════════════════════════════
// 9. CallConvPass
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CallConvPass, SysV_TwoIntArgs_IntReturn) {
    auto fn = makeRegFn({"rdi", "rsi"}, {"rax"});
    CallConvPass pass;
    CallConvPass::Config cfg;
    cfg.platformCC = CC::SysVAmd64;
    auto cc = pass.run(*fn, cfg);
    EXPECT_EQ(cc.cc, CC::SysVAmd64);
    EXPECT_GE(cc.args.size(), 2u);
    EXPECT_EQ(cc.ret.kind, RetKind::Integer);
    EXPECT_FALSE(cc.isVariadic);
}

TEST(CallConvPass, SysV_Variadic_AlLiveIn) {
    auto fn = makeRegFn({"rdi", "al"}, {"rax"});
    CallConvPass pass;
    CallConvPass::Config cfg;
    cfg.platformCC = CC::SysVAmd64;
    auto cc = pass.run(*fn, cfg);
    EXPECT_TRUE(cc.isVariadic);
}

TEST(CallConvPass, Win64_FourArgs) {
    auto fn = makeRegFn({"rcx", "rdx", "r8", "r9"}, {"rax"});
    CallConvPass pass;
    CallConvPass::Config cfg;
    cfg.platformCC = CC::Win64;
    auto cc = pass.run(*fn, cfg);
    EXPECT_EQ(cc.cc, CC::Win64);
    EXPECT_EQ(cc.args.size(), 4u);
}

TEST(CallConvPass, X86_32_CdeclDetected) {
    auto fn = makeCallFn(true, 2);
    CallConvPass pass;
    CallConvPass::Config cfg;
    cfg.is32bit    = true;
    cfg.platformCC = CC::Cdecl;
    auto cc = pass.run(*fn, cfg);
    EXPECT_EQ(cc.cc, CC::Cdecl);
}

TEST(CallConvPass, X86_32_StdcallDetected) {
    auto fn = makeCallFn(false, 2);
    CallConvPass pass;
    CallConvPass::Config cfg;
    cfg.is32bit    = true;
    cfg.platformCC = CC::Stdcall;
    auto cc = pass.run(*fn, cfg);
    EXPECT_NE(cc.cc, CC::Cdecl);
}

TEST(CallConvPass, VoidReturn_NoRetRegs) {
    auto fn = makeRegFn({});
    CallConvPass pass;
    auto cc = pass.run(*fn);
    EXPECT_EQ(cc.ret.kind, RetKind::Void);
}

TEST(CallConvPass, Stats_AccumulatedCorrectly) {
    auto fn1 = makeRegFn({"rdi"}, {"rax"});
    auto fn2 = makeRegFn({"al"});

    CallConvPass pass;
    CallConvPass::Config cfg;
    cfg.platformCC = CC::SysVAmd64;
    pass.run(*fn1, cfg);
    pass.run(*fn2, cfg);

    EXPECT_EQ(pass.stats().totalFunctions, 2u);
    EXPECT_GE(pass.stats().sysvFunctions, 2u);
    EXPECT_GE(pass.stats().variadicFunctions, 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 10. CallingConvention / runAll
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CallConvPass, RunAll_BatchMode) {
    auto fn1 = makeRegFn({"rdi"}, {"rax"}, true, "fn1");
    auto fn2 = makeRegFn({"rcx"}, {"rax"}, true, "fn2");
    fn1->name(); fn2->name();

    std::vector<const SSAFunction*> fns = { fn1.get(), fn2.get() };
    CallConvPass pass;
    CallConvPass::Config cfg;
    cfg.platformCC = CC::SysVAmd64;
    auto results = pass.runAll(fns, cfg);
    EXPECT_EQ(results.size(), 2u);
}

TEST(CallConvPass, RunAll_NullSkipped) {
    auto fn1 = makeRegFn({"rdi"});
    std::vector<const SSAFunction*> fns = { fn1.get(), nullptr };
    CallConvPass pass;
    auto results = pass.runAll(fns);
    EXPECT_EQ(results.size(), 1u);
}

TEST(CallingConvention, DefaultConstruct_Unknown) {
    CallingConvention cc;
    EXPECT_EQ(cc.cc, CC::Unknown);
    EXPECT_FALSE(cc.isVariadic);
    EXPECT_TRUE(cc.args.empty());
    EXPECT_EQ(cc.ret.kind, RetKind::Void);
}

TEST(ArgDesc, DefaultConstruct) {
    ArgDesc d;
    EXPECT_EQ(d.kind, ArgKind::Register);
    EXPECT_EQ(d.reg, PhysReg::Invalid);
    EXPECT_EQ(d.ssaValueId, UINT32_MAX);
}
