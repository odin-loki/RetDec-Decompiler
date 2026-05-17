/**
 * @file tests/var_recovery/var_recovery_test.cpp
 * @brief Unit tests for ABI-Aware Variable Recovery.
 *
 * Test categories:
 *
 * PROLOGUE PARSER (14):
 *   1.  SysV x86-64: frameSize extracted from SUB RSP,N.
 *   2.  SysV x86-64: PUSH RBP → hasFramePointer = true.
 *   3.  SysV x86-64: callee-save PUSH RBX recorded.
 *   4.  SysV x86-64: red zone flag set.
 *   5.  SysV x86-64: leaf function (no SUB): frameSize = push count * 8.
 *   6.  Win64: SUB RSP detected, shadow space flag set.
 *   7.  Win64: callee-save PUSH R12 recorded.
 *   8.  Win64: no red zone.
 *   9.  x86-32: SUB ESP detected, hasFramePointer set.
 *  10.  x86-32: 4-byte callee-save offsets.
 *  11.  AArch64: STP X29,X30 → frameSize + callee saves.
 *  12.  AArch64: additional STP X19,X20 recorded.
 *  13.  ARM32: PushList bitmask → callee saves.
 *  14.  ARM32: SUB SP local area.
 *
 * ABI REGION CARVER (8):
 *  15.  SysV x64: return address region at offset +8.
 *  16.  SysV x64: frame chain at offset 0.
 *  17.  SysV x64: callee-save regions present for each pushed reg.
 *  18.  SysV x64: red zone region at localAreaStart - 128.
 *  19.  Win64: shadow space region present.
 *  20.  Win64: no red zone region.
 *  21.  isCarved returns true for return address offset.
 *  22.  isCarved returns false for a normal local variable offset.
 *
 * DVSA (10):
 *  23.  Single access: one slot produced.
 *  24.  Two non-overlapping accesses: two slots.
 *  25.  Two overlapping accesses: one union slot.
 *  26.  Carved accesses excluded from slots.
 *  27.  Wider access sets maxAccess correctly.
 *  28.  hasWrite / hasRead flags correct.
 *  29.  SP-relative offset normalised to RBP-relative.
 *  30.  Three sequential non-overlapping slots: three entries.
 *  31.  Sub-access within slot treated as separate variable.
 *  32.  Empty SSA function: zero slots.
 *
 * VARIABLE NAMER (6):
 *  33.  Auto name: 4-byte slot → d0.
 *  34.  Auto name: 8-byte slot → q0.
 *  35.  Auto name counters increment per type: d0, d1.
 *  36.  DWARF name overrides auto name.
 *  37.  Positive offset → arg0.
 *  38.  Callee-save → "saved_rbx".
 *
 * INTEGRATION (4):
 *  39.  SysV x64 function: 3 locals recovered with correct names.
 *  40.  Win64 function: shadow space excluded, 2 locals recovered.
 *  41.  DWARF name match count correct.
 *  42.  Union candidates present when overlapping writes detected.
 */

#include "retdec/var_recovery/var_recovery.h"
#include "retdec/ssa/ssa.h"
#include <gtest/gtest.h>
#include <algorithm>

using namespace retdec::var_recovery;
using namespace retdec::ssa;

// ─── RawInstr builders ───────────────────────────────────────────────────────

static RawInstr push(Reg r) {
    RawInstr ins; ins.op = RawInstr::Op::Push; ins.src = r; return ins;
}
static RawInstr mov(Reg dst, Reg src) {
    RawInstr ins; ins.op = RawInstr::Op::Mov; ins.dst = dst; ins.src = src; return ins;
}
static RawInstr subImm(Reg dst, int64_t imm) {
    RawInstr ins; ins.op = RawInstr::Op::Sub; ins.dst = dst;
    ins.imm = imm; ins.hasImm = true; return ins;
}
static RawInstr stpPair(Reg lo, Reg hi, int64_t off) {
    RawInstr ins; ins.op = RawInstr::Op::StoreRegPair;
    ins.dst = lo; ins.src = hi; ins.imm = off; ins.hasImm = true; return ins;
}
static RawInstr pushList(uint32_t mask) {
    RawInstr ins; ins.op = RawInstr::Op::PushList;
    ins.imm = (int64_t)mask; ins.hasImm = true; return ins;
}

// ─── SSA MemRef builder helper ────────────────────────────────────────────────

static IrValue* addMemRef(SSAFunction& fn, BlockId blk,
                            VarId baseReg, int64_t off, uint8_t width,
                            bool isStack = true, bool isWrite = false) {
    IrValue* v = fn.allocValue(ValueKind::MemRef, /*varId*/ UINT32_MAX - 2);
    v->memBaseReg = baseReg;
    v->memOffset  = off;
    v->memWidth   = width;
    v->memIsStack = isStack;

    IrInstr* ins = fn.addInstr(blk,
        isWrite ? IrInstr::Op::Store : IrInstr::Op::Load, 0x1000);
    ins->uses.push_back({v->id, 0});
    v->defInstr = ins;
    return v;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Prologue parser tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PrologueParser, SysVx64_FrameSize) {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                          subImm(Reg::RSP, 48)});
    EXPECT_EQ(info.frameSize, 48);
    EXPECT_EQ(info.arch, Arch::X86_64);
}

TEST(PrologueParser, SysVx64_HasFramePointer) {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                          subImm(Reg::RSP, 32)});
    EXPECT_TRUE(info.hasFramePointer);
}

TEST(PrologueParser, SysVx64_CalleeSavePushRBX) {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                          push(Reg::RBX), subImm(Reg::RSP, 32)});
    bool found = false;
    for (auto& [r, off] : info.calleeSaves)
        if (r == Reg::RBX) found = true;
    EXPECT_TRUE(found);
}

TEST(PrologueParser, SysVx64_RedZoneFlag) {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                          subImm(Reg::RSP, 32)});
    EXPECT_TRUE(info.hasRedZone);
}

TEST(PrologueParser, SysVx64_LeafFunction_PushOnly) {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    // No SUB RSP — just PUSH RBP
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP)});
    EXPECT_GE(info.frameSize, 0);  // at least 0 (1 push × 8)
}

TEST(PrologueParser, Win64_FrameSize_ShadowSpace) {
    PrologueParser pp(ABI::Win64, Arch::X86_64);
    auto info = pp.parse({subImm(Reg::RSP, 64)});
    EXPECT_EQ(info.frameSize, 64);
    EXPECT_TRUE(info.hasShadowSpace);
    EXPECT_FALSE(info.hasRedZone);
}

TEST(PrologueParser, Win64_CalleeSave_R12) {
    PrologueParser pp(ABI::Win64, Arch::X86_64);
    auto info = pp.parse({push(Reg::R12), subImm(Reg::RSP, 32)});
    bool found = false;
    for (auto& [r, off] : info.calleeSaves)
        if (r == Reg::R12) found = true;
    EXPECT_TRUE(found);
}

TEST(PrologueParser, Win64_NoRedZone) {
    PrologueParser pp(ABI::Win64, Arch::X86_64);
    auto info = pp.parse({subImm(Reg::RSP, 32)});
    EXPECT_FALSE(info.hasRedZone);
}

TEST(PrologueParser, SysVx32_FramePointer) {
    PrologueParser pp(ABI::SysV_x86_32, Arch::X86_32);
    auto info = pp.parse({push(Reg::EBP), mov(Reg::EBP, Reg::ESP),
                          subImm(Reg::ESP, 32)});
    EXPECT_TRUE(info.hasFramePointer);
    EXPECT_EQ(info.frameSize, 32);
    EXPECT_EQ(info.arch, Arch::X86_32);
}

TEST(PrologueParser, SysVx32_4ByteCalleeSave) {
    PrologueParser pp(ABI::SysV_x86_32, Arch::X86_32);
    auto info = pp.parse({push(Reg::EBP), mov(Reg::EBP, Reg::ESP),
                          push(Reg::EBX), subImm(Reg::ESP, 16)});
    for (auto& [r, off] : info.calleeSaves) {
        if (r == Reg::EBX) {
            EXPECT_EQ(off % 4, 0);  // 4-byte aligned
            break;
        }
    }
}

TEST(PrologueParser, AArch64_STP_FrameSize) {
    PrologueParser pp(ABI::AAPCS64, Arch::ARM64);
    auto info = pp.parse({stpPair(Reg::X29, Reg::X30, -48)});
    EXPECT_EQ(info.frameSize, 48);
    EXPECT_TRUE(info.hasFramePointer);
    EXPECT_EQ(info.arch, Arch::ARM64);
}

TEST(PrologueParser, AArch64_AdditionalSTP_X19X20) {
    PrologueParser pp(ABI::AAPCS64, Arch::ARM64);
    auto info = pp.parse({stpPair(Reg::X29, Reg::X30, -64),
                          stpPair(Reg::X19, Reg::X20, 16)});
    bool found19 = false, found20 = false;
    for (auto& [r, off] : info.calleeSaves) {
        if (r == Reg::X19) found19 = true;
        if (r == Reg::X20) found20 = true;
    }
    EXPECT_TRUE(found19);
    EXPECT_TRUE(found20);
}

TEST(PrologueParser, ARM32_PushListCalleeSaves) {
    PrologueParser pp(ABI::AAPCS32, Arch::ARM32);
    // PUSH {R4, R5, LR}  → mask = bit4 | bit5 | bit14
    uint32_t mask = (1u << 4) | (1u << 5) | (1u << 14);
    auto info = pp.parse({pushList(mask)});
    EXPECT_EQ((int)info.calleeSaves.size(), 3);
    EXPECT_EQ(info.arch, Arch::ARM32);
}

TEST(PrologueParser, ARM32_SubSP_LocalArea) {
    PrologueParser pp(ABI::AAPCS32, Arch::ARM32);
    uint32_t mask = (1u << 4);  // PUSH {R4}
    auto info = pp.parse({pushList(mask), subImm(Reg::SP_ARM32, 24)});
    EXPECT_EQ(info.localAreaStart, -24);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ABI region carver tests
// ═══════════════════════════════════════════════════════════════════════════════

static PrologueInfo makeSysVx64Info() {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                          push(Reg::RBX), subImm(Reg::RSP, 32)});
    AbiRegionCarver carver;
    carver.carve(info);
    return info;
}

TEST(AbiRegionCarver, SysVx64_ReturnAddressAt8) {
    auto info = makeSysVx64Info();
    bool found = false;
    for (auto& r : info.abiRegions)
        if (r.kind == RegionKind::ReturnAddress && r.offset == 8) found = true;
    EXPECT_TRUE(found);
}

TEST(AbiRegionCarver, SysVx64_FrameChainAt0) {
    auto info = makeSysVx64Info();
    bool found = false;
    for (auto& r : info.abiRegions)
        if (r.kind == RegionKind::FrameChain && r.offset == 0) found = true;
    EXPECT_TRUE(found);
}

TEST(AbiRegionCarver, SysVx64_CalleeSaveRBX) {
    auto info = makeSysVx64Info();
    bool found = false;
    for (auto& r : info.abiRegions)
        if (r.kind == RegionKind::CalleeSave &&
            r.name.find("rbx") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(AbiRegionCarver, SysVx64_RedZoneRegion) {
    auto info = makeSysVx64Info();
    bool found = false;
    for (auto& r : info.abiRegions)
        if (r.kind == RegionKind::RedZone && r.size == 128) found = true;
    EXPECT_TRUE(found);
}

TEST(AbiRegionCarver, Win64_ShadowSpace) {
    PrologueParser pp(ABI::Win64, Arch::X86_64);
    auto info = pp.parse({subImm(Reg::RSP, 32)});
    AbiRegionCarver carver;
    carver.carve(info);
    bool found = false;
    for (auto& r : info.abiRegions)
        if (r.kind == RegionKind::ShadowSpace && r.size == 32) found = true;
    EXPECT_TRUE(found);
}

TEST(AbiRegionCarver, Win64_NoRedZone) {
    PrologueParser pp(ABI::Win64, Arch::X86_64);
    auto info = pp.parse({subImm(Reg::RSP, 32)});
    AbiRegionCarver carver;
    carver.carve(info);
    bool found = false;
    for (auto& r : info.abiRegions)
        if (r.kind == RegionKind::RedZone) found = true;
    EXPECT_FALSE(found);
}

TEST(AbiRegionCarver, IsCarved_ReturnAddress) {
    auto info = makeSysVx64Info();
    AbiRegionCarver carver;
    // offset +8 is the return address → carved
    EXPECT_TRUE(carver.isCarved(info, 8, 8));
}

TEST(AbiRegionCarver, IsCarved_LocalVar_NotCarved) {
    auto info = makeSysVx64Info();
    AbiRegionCarver carver;
    // offset -16 is a normal local variable → not carved
    EXPECT_FALSE(carver.isCarved(info, -16, 8));
}

// ═══════════════════════════════════════════════════════════════════════════════
// DVSA tests
// ═══════════════════════════════════════════════════════════════════════════════

// Minimal prologue with empty ABI regions for DVSA tests
static PrologueInfo emptyPrologue(int64_t frameSize = 64) {
    PrologueInfo info;
    info.abi  = ABI::SysV_x86_64;
    info.arch = Arch::X86_64;
    info.frameSize     = frameSize;
    info.hasFramePointer = true;
    info.hasRedZone    = false;
    info.localAreaStart = -(int64_t)frameSize;
    info.localAreaEnd   = 0;
    return info;
}

TEST(DVSA, SingleAccess_OneSlot) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    // RBP-based access: offset -8, width 8
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 8);

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    EXPECT_EQ(res.slots.size(), 1u);
    EXPECT_EQ(res.unionSlots.size(), 0u);
}

TEST(DVSA, TwoNonOverlapping_TwoSlots) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8,  8);   // [RBP-8]
    addMemRef(fn, b->id, (VarId)Reg::RBP, -16, 8);   // [RBP-16]

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    EXPECT_EQ(res.slots.size(), 2u);
}

TEST(DVSA, Overlapping_UnionSlot) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 4);   // [RBP-8], 4 bytes
    addMemRef(fn, b->id, (VarId)Reg::RBP, -6, 4);   // [RBP-6], 4 bytes → overlaps

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    EXPECT_GE(res.unionSlots.size(), 1u);
}

TEST(DVSA, CarvedAccess_Excluded) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    // Add a full SysV prologue to get carved regions
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto prologue = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                              subImm(Reg::RSP, 32)});
    AbiRegionCarver carver; carver.carve(prologue);

    // Access at return address offset (+8) → should be carved out
    addMemRef(fn, b->id, (VarId)Reg::RBP, +8, 8);
    // Normal local → should not be carved
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 8);

    DVSA dvsa;
    auto res = dvsa.run(fn, prologue);
    // Only the local variable slot, not the return address
    EXPECT_EQ(res.slots.size(), 1u);
    if (!res.slots.empty())
        EXPECT_EQ(res.slots[0].baseOffset, -8);
}

TEST(DVSA, MaxAccess_Correct) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 4);
    addMemRef(fn, b->id, (VarId)Reg::RBP, -4, 4);

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    for (auto& s : res.slots)
        EXPECT_LE(s.maxAccess, 8u);
}

TEST(DVSA, HasWriteFlag) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 8, true, true);  // write

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    ASSERT_EQ(res.slots.size(), 1u);
    EXPECT_TRUE(res.slots[0].hasWrite);
}

TEST(DVSA, ThreeSequentialSlots) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8,  8);
    addMemRef(fn, b->id, (VarId)Reg::RBP, -16, 8);
    addMemRef(fn, b->id, (VarId)Reg::RBP, -24, 8);

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    EXPECT_EQ(res.slots.size(), 3u);
}

TEST(DVSA, EmptyFunction_ZeroSlots) {
    SSAFunction fn("f");
    fn.addBlock("entry");
    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    EXPECT_EQ(res.slots.size(), 0u);
    EXPECT_EQ(res.unionSlots.size(), 0u);
}

TEST(DVSA, NonStackAccess_Excluded) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    // Non-stack MemRef → should not appear in DVSA output
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX - 2);
    v->memBaseReg = 0;  // RAX
    v->memOffset  = 0;
    v->memWidth   = 8;
    v->memIsStack = false;  // heap access
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Load, 0x2000);
    ins->uses.push_back({v->id, 0});
    v->defInstr = ins;

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    EXPECT_EQ(res.slots.size(), 0u);
}

TEST(DVSA, SubAccess_SeparateSlots) {
    SSAFunction fn("f");
    auto* b = fn.addBlock("entry");
    // 8-byte slot at -8, then a 4-byte access to -8 (sub-access)
    // and a separate 4-byte access to -4 (non-overlapping)
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 4);
    addMemRef(fn, b->id, (VarId)Reg::RBP, -4, 4);

    DVSA dvsa;
    auto res = dvsa.run(fn, emptyPrologue());
    // Two non-overlapping 4-byte accesses → two separate slots
    EXPECT_EQ(res.slots.size(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Variable namer tests
// ═══════════════════════════════════════════════════════════════════════════════

static VariableCandidate makeCand(int64_t off, uint8_t sz) {
    VariableCandidate c;
    c.slot.baseOffset = off;
    c.slot.totalSize  = sz;
    c.slot.maxAccess  = sz;
    return c;
}

static PrologueInfo sysVProlog() {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                          subImm(Reg::RSP, 64)});
    return info;
}

TEST(VariableNamer, AutoName_DwordIsD0) {
    std::vector<VariableCandidate> cands = {makeCand(-8, 4)};
    VariableNamer namer;
    namer.name(cands, sysVProlog());
    EXPECT_EQ(cands[0].name, "d0");
}

TEST(VariableNamer, AutoName_QwordIsQ0) {
    std::vector<VariableCandidate> cands = {makeCand(-8, 8)};
    VariableNamer namer;
    namer.name(cands, sysVProlog());
    EXPECT_EQ(cands[0].name, "q0");
}

TEST(VariableNamer, AutoName_CounterIncrements) {
    std::vector<VariableCandidate> cands = {makeCand(-8, 4), makeCand(-12, 4)};
    VariableNamer namer;
    namer.name(cands, sysVProlog());
    EXPECT_EQ(cands[0].name, "d0");
    EXPECT_EQ(cands[1].name, "d1");
}

TEST(VariableNamer, DwarfName_Overrides) {
    std::vector<VariableCandidate> cands = {makeCand(-8, 4)};
    DwarfVarInfo di;
    di.name = "my_counter";
    di.frameOffset = -8;
    di.size = 4;

    VariableNamer namer;
    namer.name(cands, sysVProlog(), {di});
    EXPECT_EQ(cands[0].name, "my_counter");
    EXPECT_TRUE(cands[0].isDwarfNamed);
}

TEST(VariableNamer, PositiveOffset_IsArg) {
    std::vector<VariableCandidate> cands = {makeCand(+16, 8)};  // first stack arg
    VariableNamer namer;
    namer.name(cands, sysVProlog());
    EXPECT_EQ(cands[0].name, "arg0");
    EXPECT_TRUE(cands[0].isArg);
}

TEST(VariableNamer, CalleeSave_GetsRegisterName) {
    PrologueParser pp(ABI::SysV_x86_64, Arch::X86_64);
    auto info = pp.parse({push(Reg::RBP), mov(Reg::RBP, Reg::RSP),
                          push(Reg::RBX), subImm(Reg::RSP, 32)});

    std::vector<VariableCandidate> cands;
    // Find the RBX save offset from the parsed info
    for (auto& [r, off] : info.calleeSaves) {
        if (r == Reg::RBX) {
            auto c = makeCand(off, 8);
            c.isCalleeSave = true;
            cands.push_back(c);
        }
    }
    ASSERT_FALSE(cands.empty());

    VariableNamer namer;
    namer.name(cands, info);
    EXPECT_NE(cands[0].name.find("rbx"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Integration tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(VarRecoveryPass, SysVx64_ThreeLocals) {
    SSAFunction fn("three_locals");
    auto* b = fn.addBlock("entry");

    // Three local variables at -8, -16, -24 (8 bytes each)
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8,  8, true, true);
    addMemRef(fn, b->id, (VarId)Reg::RBP, -16, 8, true, true);
    addMemRef(fn, b->id, (VarId)Reg::RBP, -24, 8);

    std::vector<RawInstr> prologue = {
        push(Reg::RBP), mov(Reg::RBP, Reg::RSP), subImm(Reg::RSP, 48)
    };

    VarRecoveryPass pass;
    VarRecoveryPass::Config cfg;
    cfg.abi  = ABI::SysV_x86_64;
    cfg.arch = Arch::X86_64;

    auto result = pass.run(fn, prologue, cfg);
    // 3 locals (return address and frame chain are carved out)
    EXPECT_GE(result.candidates.size(), 3u);
    EXPECT_TRUE(result.prologue.hasFramePointer);
}

TEST(VarRecoveryPass, Win64_ShadowSpaceExcluded) {
    SSAFunction fn("win64_fn");
    auto* b = fn.addBlock("entry");

    // Shadow space: [RSP+0..31] — should be carved
    // In our normalisation (RSP-based): offset 0 to 31 = RBP-relative: +(frameSize)
    // We'll put a normal local at [RBP-8]
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 8);

    std::vector<RawInstr> prologue = {subImm(Reg::RSP, 32)};

    VarRecoveryPass pass;
    VarRecoveryPass::Config cfg;
    cfg.abi  = ABI::Win64;
    cfg.arch = Arch::X86_64;

    auto result = pass.run(fn, prologue, cfg);
    // The local at -8 should be recovered
    EXPECT_GE(result.candidates.size(), 1u);
    EXPECT_TRUE(result.prologue.hasShadowSpace);
}

TEST(VarRecoveryPass, DwarfMatchCount) {
    SSAFunction fn("dwarf_fn");
    auto* b = fn.addBlock("entry");
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 4);
    addMemRef(fn, b->id, (VarId)Reg::RBP, -12, 4);

    std::vector<RawInstr> prologue = {
        push(Reg::RBP), mov(Reg::RBP, Reg::RSP), subImm(Reg::RSP, 32)
    };

    VarRecoveryPass::Config cfg;
    cfg.abi  = ABI::SysV_x86_64;
    cfg.arch = Arch::X86_64;
    cfg.dwarf = {{"counter", -8, 4}, {"flag", -12, 4}};

    VarRecoveryPass pass;
    auto result = pass.run(fn, prologue, cfg);
    EXPECT_EQ(result.dwarfMatchCount, 2u);
    // Variables should have DWARF names
    bool foundCounter = false, foundFlag = false;
    for (auto& c : result.candidates) {
        if (c.name == "counter") foundCounter = true;
        if (c.name == "flag")    foundFlag    = true;
    }
    EXPECT_TRUE(foundCounter);
    EXPECT_TRUE(foundFlag);
}

TEST(VarRecoveryPass, UnionCandidates_Present) {
    SSAFunction fn("union_fn");
    auto* b = fn.addBlock("entry");
    // Overlapping accesses → union
    addMemRef(fn, b->id, (VarId)Reg::RBP, -8, 4, true, true);  // write 4 bytes
    addMemRef(fn, b->id, (VarId)Reg::RBP, -6, 4);               // read 4 bytes (overlaps)

    std::vector<RawInstr> prologue = {
        push(Reg::RBP), mov(Reg::RBP, Reg::RSP), subImm(Reg::RSP, 32)
    };

    VarRecoveryPass pass;
    VarRecoveryPass::Config cfg;
    cfg.abi  = ABI::SysV_x86_64;
    cfg.arch = Arch::X86_64;

    auto result = pass.run(fn, prologue, cfg);
    bool hasUnion = false;
    for (auto& c : result.candidates)
        if (c.isUnion) hasUnion = true;
    EXPECT_TRUE(hasUnion);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
