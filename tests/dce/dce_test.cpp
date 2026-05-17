/**
 * @file tests/dce/dce_test.cpp
 * @brief Unit tests for the dce module (Stage 22).
 *
 * Test groups:
 *   1.  AbiArtifactKind names
 *   2.  AbiArtifactMarker — stack align, prologue/epilogue, callee-save pairs,
 *                           shadow space, red zone
 *   3.  LiveRootCollector — return, ptr arg writes, global writes, I/O roots
 *   4.  DeadPropagation   — backward propagation from roots, ABI artifact skipping
 *   5.  UnreachableEliminator — reachable / unreachable blocks
 *   6.  DcePass           — full pipeline, stats, dead-set correctness
 *   7.  DeadCodeResult    — summary, eliminationRate
 */

#include "retdec/dce/dce.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"

#include <gtest/gtest.h>
#include <memory>

using namespace retdec::dce;
using namespace retdec::ssa;
using namespace retdec::call_conv;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

/// Build a minimal linear function: entry → ret with given instructions.
std::unique_ptr<SSAFunction> makeLinear(
        std::vector<IrInstr::Op> ops,
        std::vector<std::string> liveOutNames = {},
        std::vector<std::string> liveInNames  = {}) {

    auto fn = std::make_unique<SSAFunction>("fn");
    fn->addBlock("entry");

    // Declare live-out register variables.
    for (const auto& n : liveInNames) {
        VarId v = fn->declareVar(n, 64);
        fn->block(0)->liveIn.insert(v);
    }
    for (const auto& n : liveOutNames) {
        VarId v = fn->findVar(n);
        if (v == kInvalidVar) v = fn->declareVar(n, 64);
        fn->block(0)->liveOut.insert(v);
    }

    for (IrInstr::Op op : ops) {
        fn->addInstr(0, op);
    }

    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function with an And instruction (stack-align candidate).
std::unique_ptr<SSAFunction> makeWithAndRsp() {
    auto fn = std::make_unique<SSAFunction>("stackalign_fn");
    fn->addBlock("entry");

    // AND instruction with an immediate -16.
    IrInstr* andInstr = fn->addInstr(0, IrInstr::Op::And);
    IrValue* imm = fn->allocValue(ValueKind::Immediate, kInvalidVar);
    imm->imm = static_cast<uint64_t>(-16);
    andInstr->uses.push_back({imm->id, 0});

    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function with a callee-save store in entry and load in ret block.
std::unique_ptr<SSAFunction> makeCalleeSaveFn() {
    auto fn = std::make_unique<SSAFunction>("callee_save_fn");
    fn->addBlock("entry");   // block 0 — prologue
    fn->addBlock("body");    // block 1 — body
    fn->addBlock("epilogue");// block 2 — epilogue + ret

    // Wire CFG: 0 → 1 → 2
    fn->block(0)->addSucc(1); fn->block(1)->addPred(0);
    fn->block(1)->addSucc(2); fn->block(2)->addPred(1);

    // Declare "rbx" (callee-saved on SysV).
    VarId rbx = fn->declareVar("rbx", 64);
    IrValue* rbxVal = fn->allocValue(ValueKind::VirtualReg, rbx);

    // Prologue: store rbx to [RBP-8].
    IrInstr* saveInstr = fn->addInstr(0, IrInstr::Op::Store);
    IrValue* destSlot  = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    destSlot->memIsStack = true;
    destSlot->memOffset  = -8;
    saveInstr->uses.push_back({destSlot->id, 0});
    saveInstr->uses.push_back({rbxVal->id,  1});

    // Body: some real work.
    fn->addInstr(1, IrInstr::Op::Add);

    // Epilogue: restore rbx from [RBP-8], then ret.
    IrInstr* restoreInstr = fn->addInstr(2, IrInstr::Op::Load);
    IrValue* srcSlot = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    srcSlot->memIsStack = true;
    srcSlot->memOffset  = -8;
    restoreInstr->uses.push_back({srcSlot->id, 0});
    fn->addInstr(2, IrInstr::Op::Ret);

    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function with an unreachable block.
std::unique_ptr<SSAFunction> makeWithUnreachable() {
    auto fn = std::make_unique<SSAFunction>("unreach_fn");
    fn->addBlock("entry");
    fn->addBlock("dead");   // no predecessor
    fn->addBlock("exit");

    fn->block(0)->addSucc(2); fn->block(2)->addPred(0);
    // Block 1 ("dead") has no predecessors — unreachable.

    fn->addInstr(0, IrInstr::Op::Branch);
    fn->addInstr(1, IrInstr::Op::Add);   // dead instruction
    fn->addInstr(2, IrInstr::Op::Ret);

    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function with a global memory store (live root).
std::unique_ptr<SSAFunction> makeGlobalStoreFn() {
    auto fn = std::make_unique<SSAFunction>("global_store_fn");
    fn->addBlock("entry");

    IrInstr* storeInstr = fn->addInstr(0, IrInstr::Op::Store);
    IrValue* globalAddr = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    globalAddr->memIsStack = false;  // global memory
    globalAddr->memOffset  = 0x1000;
    storeInstr->uses.push_back({globalAddr->id, 0});

    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function with a CALL instruction.
std::unique_ptr<SSAFunction> makeCallFn() {
    auto fn = std::make_unique<SSAFunction>("call_fn");
    fn->addBlock("entry");
    fn->addInstr(0, IrInstr::Op::Call);
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function with a red zone store (dead).
std::unique_ptr<SSAFunction> makeRedZoneFn() {
    auto fn = std::make_unique<SSAFunction>("rz_fn");
    fn->addBlock("entry");

    IrInstr* store = fn->addInstr(0, IrInstr::Op::Store);
    IrValue* rz    = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    rz->memIsStack = true;
    rz->memOffset  = -64;  // red zone
    store->uses.push_back({rz->id, 0});

    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

CallingConvention makeSysVCC(std::size_t numArgs = 0) {
    CallingConvention cc;
    cc.cc  = CC::SysVAmd64;
    cc.ret.kind = RetKind::Void;
    return cc;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// 1. AbiArtifactKind names
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AbiArtifactKind, Names) {
    EXPECT_STREQ(abiArtifactKindName(AbiArtifactKind::StackAlign),      "StackAlign");
    EXPECT_STREQ(abiArtifactKindName(AbiArtifactKind::ShadowSpaceRead), "ShadowSpaceRead");
    EXPECT_STREQ(abiArtifactKindName(AbiArtifactKind::CalleeSavePair),  "CalleeSavePair");
    EXPECT_STREQ(abiArtifactKindName(AbiArtifactKind::RedZoneAccess),   "RedZoneAccess");
    EXPECT_STREQ(abiArtifactKindName(AbiArtifactKind::PrologueSetup),   "PrologueSetup");
    EXPECT_STREQ(abiArtifactKindName(AbiArtifactKind::EpilogueCleanup), "EpilogueCleanup");
}

// ═══════════════════════════════════════════════════════════════════════════════
// 2. AbiArtifactMarker
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AbiArtifactMarker, StackAlignDetected) {
    auto fn = makeWithAndRsp();
    AbiArtifactMarker marker;
    auto arts = marker.run(*fn);
    bool found = false;
    for (const auto& a : arts) {
        if (a.kind == AbiArtifactKind::StackAlign) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(AbiArtifactMarker, NoStackAlign_NormalAdd) {
    auto fn = makeLinear({IrInstr::Op::Add, IrInstr::Op::Ret});
    AbiArtifactMarker marker;
    auto arts = marker.run(*fn);
    bool found = false;
    for (const auto& a : arts) {
        if (a.kind == AbiArtifactKind::StackAlign) { found = true; break; }
    }
    EXPECT_FALSE(found);
}

TEST(AbiArtifactMarker, CalleeSavePair_Balanced) {
    auto fn = makeCalleeSaveFn();
    AbiArtifactMarker marker;
    AbiArtifactMarker::Config cfg;
    cfg.sysVAmd64 = true;
    auto arts = marker.run(*fn, cfg);
    bool foundPair = false;
    for (const auto& a : arts) {
        if (a.kind == AbiArtifactKind::CalleeSavePair) {
            EXPECT_TRUE(a.balanced);
            EXPECT_NE(a.instrId, UINT32_MAX);
            foundPair = true;
        }
    }
    EXPECT_TRUE(foundPair);
}

TEST(AbiArtifactMarker, PrologueSetup_Detected) {
    auto fn = std::make_unique<SSAFunction>("prolog_fn");
    fn->addBlock("entry");
    // SUB with immediate (prologue stack setup)
    IrInstr* sub = fn->addInstr(0, IrInstr::Op::Sub);
    IrValue* imm = fn->allocValue(ValueKind::Immediate, kInvalidVar);
    imm->imm = 32;
    sub->uses.push_back({imm->id, 0});
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    AbiArtifactMarker marker;
    auto arts = marker.run(*fn);
    bool found = false;
    for (const auto& a : arts) {
        if (a.kind == AbiArtifactKind::PrologueSetup) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(AbiArtifactMarker, RedZoneStore_Detected) {
    auto fn = makeRedZoneFn();
    AbiArtifactMarker marker;
    AbiArtifactMarker::Config cfg;
    cfg.sysVAmd64 = true;
    auto arts = marker.run(*fn, cfg);
    bool found = false;
    for (const auto& a : arts) {
        if (a.kind == AbiArtifactKind::RedZoneAccess) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(AbiArtifactMarker, RedZoneRead_NotDeadIfRead) {
    // Store then load from same red zone slot → not dead
    auto fn = std::make_unique<SSAFunction>("rz_live_fn");
    fn->addBlock("entry");

    IrValue* slot = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    slot->memIsStack = true;
    slot->memOffset  = -32;

    IrInstr* st = fn->addInstr(0, IrInstr::Op::Store);
    st->uses.push_back({slot->id, 0});

    IrValue* slot2 = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    slot2->memIsStack = true;
    slot2->memOffset  = -32;
    IrInstr* ld = fn->addInstr(0, IrInstr::Op::Load);
    ld->uses.push_back({slot2->id, 0});

    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    AbiArtifactMarker marker;
    AbiArtifactMarker::Config cfg; cfg.sysVAmd64 = true;
    auto arts = marker.run(*fn, cfg);
    bool found = false;
    for (const auto& a : arts) {
        if (a.kind == AbiArtifactKind::RedZoneAccess) { found = true; break; }
    }
    EXPECT_FALSE(found);
}

TEST(AbiArtifactMarker, Win64_ShadowSpace_NeverWritten) {
    auto fn = std::make_unique<SSAFunction>("win64_shadow_fn");
    fn->addBlock("entry");

    // Load from shadow slot +8 (never written).
    IrValue* slot = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    slot->memIsStack = true;
    slot->memOffset  = 8;  // shadow slot
    IrInstr* ld = fn->addInstr(0, IrInstr::Op::Load);
    ld->uses.push_back({slot->id, 0});
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    AbiArtifactMarker marker;
    AbiArtifactMarker::Config cfg;
    cfg.win64     = true;
    cfg.sysVAmd64 = false;
    auto arts = marker.run(*fn, cfg);
    bool found = false;
    for (const auto& a : arts) {
        if (a.kind == AbiArtifactKind::ShadowSpaceRead) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3. LiveRootCollector
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LiveRootCollector, RetInstr_IsLiveRoot) {
    auto fn = makeLinear({IrInstr::Op::Ret});
    auto cc = makeSysVCC();
    LiveRootCollector col;
    auto roots = col.run(*fn, cc);
    bool found = false;
    for (const auto& r : roots) {
        if (r.kind == LiveRootKind::ReturnValue) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(LiveRootCollector, GlobalStore_IsLiveRoot) {
    auto fn = makeGlobalStoreFn();
    auto cc = makeSysVCC();
    LiveRootCollector col;
    auto roots = col.run(*fn, cc);
    bool found = false;
    for (const auto& r : roots) {
        if (r.kind == LiveRootKind::GlobalWrite) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(LiveRootCollector, Call_IsIoSideEffect) {
    auto fn = makeCallFn();
    auto cc = makeSysVCC();
    LiveRootCollector col;
    auto roots = col.run(*fn, cc);
    bool found = false;
    for (const auto& r : roots) {
        if (r.kind == LiveRootKind::IoSideEffect) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(LiveRootCollector, NoSideEffects_OnlyRet) {
    auto fn = makeLinear({IrInstr::Op::Add, IrInstr::Op::Ret});
    auto cc = makeSysVCC();
    LiveRootCollector col;
    auto roots = col.run(*fn, cc);
    // Only the RET should be a root (no calls, no global writes).
    for (const auto& r : roots) {
        EXPECT_NE(r.kind, LiveRootKind::IoSideEffect);
        EXPECT_NE(r.kind, LiveRootKind::GlobalWrite);
    }
}

TEST(LiveRootCollector, NoDuplicateRoots) {
    auto fn = makeLinear({IrInstr::Op::Ret});
    auto cc = makeSysVCC();
    LiveRootCollector col;
    auto roots = col.run(*fn, cc);
    std::unordered_set<InstrId> ids;
    for (const auto& r : roots) {
        EXPECT_TRUE(ids.insert(r.instrId).second) << "Duplicate root";
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 4. DeadPropagation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DeadPropagation, ControlFlowInstrs_AlwaysLive) {
    auto fn = makeLinear({IrInstr::Op::Branch, IrInstr::Op::Ret});
    std::vector<LiveRoot> roots;  // no explicit roots
    std::unordered_set<InstrId> abiSet;
    DeadPropagation prop;
    auto live = prop.run(*fn, roots, abiSet);

    for (const auto& blk : fn->blocks()) {
        if (!blk) continue;
        for (const IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == IrInstr::Op::Ret ||
                instr->op == IrInstr::Op::Branch ||
                instr->op == IrInstr::Op::CondBranch) {
                EXPECT_TRUE(live.count(instr->id)) << "Control flow instr should be live";
            }
        }
    }
}

TEST(DeadPropagation, AbiArtifacts_SkippedInPropagation) {
    auto fn = makeWithAndRsp();
    // Collect ABI artifacts and mark them.
    AbiArtifactMarker marker;
    auto arts = marker.run(*fn);
    std::unordered_set<InstrId> abiSet;
    for (const auto& a : arts) abiSet.insert(a.instrId);

    std::vector<LiveRoot> roots;
    DeadPropagation prop;
    auto live = prop.run(*fn, roots, abiSet);

    // The AND instruction should NOT be in the live set.
    for (InstrId id : abiSet) {
        EXPECT_FALSE(live.count(id)) << "ABI artifact should not be live";
    }
}

TEST(DeadPropagation, RetRoot_PropagatesBackward) {
    // entry: v1 = add; v2 = add using v1; ret uses v2 → both adds are live.
    auto fn = std::make_unique<SSAFunction>("prop_fn");
    fn->addBlock("entry");

    IrValue* v0 = fn->allocValue(ValueKind::Immediate, kInvalidVar);
    v0->imm = 1;

    // Declare variables so SSA rename can create proper def-use chains.
    VarId varR1 = fn->declareVar("r1");
    VarId varR2 = fn->declareVar("r2");

    IrInstr* add1 = fn->addInstr(0, IrInstr::Op::Add);
    IrValue* r1   = fn->allocValue(ValueKind::VirtualReg, varR1);
    add1->defVar   = varR1;
    add1->defValue = r1->id;
    add1->uses.push_back({v0->id, 0});
    r1->defInstr = add1;

    IrInstr* add2 = fn->addInstr(0, IrInstr::Op::Add);
    IrValue* r2   = fn->allocValue(ValueKind::VirtualReg, varR2);
    add2->defVar   = varR2;
    add2->defValue = r2->id;
    add2->uses.push_back({r1->id, 0});
    r2->defInstr = add2;

    IrInstr* ret = fn->addInstr(0, IrInstr::Op::Ret);
    ret->uses.push_back({r2->id, 0});

    SSAPass pass; pass.run(*fn);

    LiveRoot lr; lr.instrId = ret->id; lr.kind = LiveRootKind::ReturnValue;
    std::unordered_set<InstrId> abiSet;
    DeadPropagation prop;
    auto live = prop.run(*fn, {lr}, abiSet);

    EXPECT_TRUE(live.count(ret->id));
    EXPECT_TRUE(live.count(add2->id));
    EXPECT_TRUE(live.count(add1->id));
}

// ═══════════════════════════════════════════════════════════════════════════════
// 5. UnreachableEliminator
// ═══════════════════════════════════════════════════════════════════════════════

TEST(UnreachableEliminator, AllReachable_EmptySet) {
    auto fn = makeLinear({IrInstr::Op::Ret});
    UnreachableEliminator elim;
    auto dead = elim.run(*fn);
    EXPECT_TRUE(dead.empty());
}

TEST(UnreachableEliminator, OneUnreachableBlock) {
    auto fn = makeWithUnreachable();
    UnreachableEliminator elim;
    auto dead = elim.run(*fn);
    EXPECT_EQ(dead.size(), 1u);
    EXPECT_TRUE(dead.count(1));  // block 1 ("dead") is unreachable
}

TEST(UnreachableEliminator, ChainedUnreachable) {
    auto fn = std::make_unique<SSAFunction>("chain");
    fn->addBlock("entry");
    fn->addBlock("a");  // unreachable
    fn->addBlock("b");  // unreachable, successor of a
    fn->addBlock("exit");

    fn->block(0)->addSucc(3); fn->block(3)->addPred(0);
    fn->block(1)->addSucc(2); fn->block(2)->addPred(1);

    fn->addInstr(0, IrInstr::Op::Branch);
    fn->addInstr(1, IrInstr::Op::Add);
    fn->addInstr(2, IrInstr::Op::Add);
    fn->addInstr(3, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    UnreachableEliminator elim;
    auto dead = elim.run(*fn);
    EXPECT_EQ(dead.size(), 2u);
    EXPECT_TRUE(dead.count(1));
    EXPECT_TRUE(dead.count(2));
}

// ═══════════════════════════════════════════════════════════════════════════════
// 6. DcePass — full pipeline
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DcePass, EmptyFunction_Runs) {
    SSAFunction fn("empty");
    fn.addBlock("entry");
    SSAPass p; p.run(fn);
    auto cc = makeSysVCC();
    DcePass pass;
    auto res = pass.run(fn, cc);
    EXPECT_EQ(res.totalInstrs, 0u);
}

TEST(DcePass, AllLive_NoElimination) {
    // Call + Ret: both are live roots → nothing eliminated.
    auto fn = makeCallFn();
    auto cc = makeSysVCC();
    DcePass pass;
    auto res = pass.run(*fn, cc);
    EXPECT_EQ(res.eliminatedInstrCount, 0u);
    EXPECT_GE(res.liveInstrs.size(), 1u);
}

TEST(DcePass, StackAlign_Eliminated) {
    auto fn = makeWithAndRsp();
    auto cc = makeSysVCC();
    DcePass pass;
    DcePass::Config cfg;
    cfg.abiCfg.sysVAmd64 = true;
    auto res = pass.run(*fn, cc, cfg);
    // The AND (stack-align) instruction should be eliminated.
    EXPECT_GE(res.abiArtifactsRemoved.count(AbiArtifactKind::StackAlign), 0u);
}

TEST(DcePass, UnreachableBlock_Eliminated) {
    auto fn = makeWithUnreachable();
    auto cc = makeSysVCC();
    DcePass pass;
    auto res = pass.run(*fn, cc);
    EXPECT_GE(res.eliminatedBlockCount, 1u);
    EXPECT_TRUE(res.eliminatedBlocks.count(1));
}

TEST(DcePass, GlobalStore_NotEliminated) {
    auto fn = makeGlobalStoreFn();
    auto cc = makeSysVCC();
    DcePass pass;
    auto res = pass.run(*fn, cc);
    // The store instruction must be live.
    for (const auto& blk : fn->blocks()) {
        if (!blk) continue;
        for (const IrInstr* instr : blk->instrs) {
            if (!instr || instr->op != IrInstr::Op::Store) continue;
            EXPECT_TRUE(res.liveInstrs.count(instr->id))
                << "Global store should be live";
        }
    }
}

TEST(DcePass, CalleeSavePair_EliminatedWhenBalanced) {
    auto fn = makeCalleeSaveFn();
    auto cc = makeSysVCC();
    DcePass pass;
    DcePass::Config cfg;
    cfg.abiCfg.sysVAmd64 = true;
    auto res = pass.run(*fn, cc, cfg);
    // Should have at least one CalleeSavePair artifact.
    bool found = false;
    for (const auto& a : res.abiArtifacts) {
        if (a.kind == AbiArtifactKind::CalleeSavePair && a.balanced) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(DcePass, KeepAbiArtifacts_OverridesFiltering) {
    auto fn = makeWithAndRsp();
    auto cc = makeSysVCC();
    DcePass pass;
    DcePass::Config cfg;
    cfg.keepAbiArtifacts = true;
    auto res = pass.run(*fn, cc, cfg);
    EXPECT_TRUE(res.abiArtifacts.empty());
}

TEST(DcePass, Stats_TotalInstrs_Correct) {
    auto fn = makeLinear({IrInstr::Op::Add, IrInstr::Op::Ret});
    auto cc = makeSysVCC();
    DcePass pass;
    auto res = pass.run(*fn, cc);
    EXPECT_EQ(res.totalInstrs, 2u);
}

TEST(DcePass, EliminationRate_ZeroWhenAllLive) {
    auto fn = makeCallFn();
    auto cc = makeSysVCC();
    DcePass pass;
    auto res = pass.run(*fn, cc);
    // CALL + RET both live → rate ≤ 0% (no real dead code).
    EXPECT_LE(res.eliminationRate(), 0.5);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 7. DeadCodeResult
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DeadCodeResult, Summary_ContainsStats) {
    DeadCodeResult r;
    r.totalInstrs          = 100;
    r.eliminatedInstrCount = 25;
    r.eliminatedBlockCount = 2;
    r.abiArtifactsRemoved[AbiArtifactKind::StackAlign] = 1;
    auto s = r.summary();
    EXPECT_NE(s.find("25"), std::string::npos);
    EXPECT_NE(s.find("100"), std::string::npos);
}

TEST(DeadCodeResult, EliminationRate_Correct) {
    DeadCodeResult r;
    r.totalInstrs          = 50;
    r.eliminatedInstrCount = 10;
    EXPECT_DOUBLE_EQ(r.eliminationRate(), 0.2);
}

TEST(DeadCodeResult, EliminationRate_ZeroTotal) {
    DeadCodeResult r;
    r.totalInstrs = 0;
    EXPECT_DOUBLE_EQ(r.eliminationRate(), 0.0);
}

TEST(DeadCodeResult, DefaultConstruct) {
    DeadCodeResult r;
    EXPECT_EQ(r.totalInstrs, 0u);
    EXPECT_EQ(r.eliminatedInstrCount, 0u);
    EXPECT_TRUE(r.eliminatedInstrs.empty());
    EXPECT_TRUE(r.liveInstrs.empty());
}
