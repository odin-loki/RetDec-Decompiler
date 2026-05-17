/**
 * @file tests/ipa/ipa_test.cpp
 * @brief Unit tests for the ipa module (Stage 23).
 *
 * Test groups:
 *   1.  CallGraph         — build, successors, predecessors, callCount
 *   2.  TarjanScc         — single node, linear chain, mutual recursion, SCC order
 *   3.  FunctionSummary   — equality, toString, isPure, isNoReturn
 *   4.  SummaryComputer   — param types, global r/w, instruction count
 *   5.  IpaPropagation    — type propagation across call sites
 *   6.  GlobalTyper       — global write collection, type conflict
 *   7.  InlineCandidateFinder — pure small single-call functions
 *   8.  IpaPass           — full pipeline, IpaResult summary
 */

#include "retdec/ipa/ipa.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"

#include <gtest/gtest.h>
#include <memory>

using namespace retdec::ipa;
using namespace retdec::ssa;
using namespace retdec::call_conv;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

/// Build a minimal pure function (no CALLs, no global stores).
std::unique_ptr<SSAFunction> makePureFn(const std::string& name,
                                         std::size_t instrCount = 3) {
    auto fn = std::make_unique<SSAFunction>(name);
    fn->addBlock("entry");
    for (std::size_t i = 1; i < instrCount; ++i) {
        fn->addInstr(0, IrInstr::Op::Add);
    }
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function that calls another by name.
std::unique_ptr<SSAFunction> makeCallerFn(const std::string& callerName,
                                           const std::string& calleeName) {
    auto fn = std::make_unique<SSAFunction>(callerName);
    fn->addBlock("entry");
    IrInstr* call = fn->addInstr(0, IrInstr::Op::Call);
    call->calleeName = calleeName;
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a function that writes to a global (non-stack) memory.
std::unique_ptr<SSAFunction> makeGlobalWriterFn(const std::string& name,
                                                  int64_t addr = 0x1000) {
    auto fn = std::make_unique<SSAFunction>(name);
    fn->addBlock("entry");
    IrInstr* store = fn->addInstr(0, IrInstr::Op::Store);
    IrValue* gAddr = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    gAddr->memIsStack = false;
    gAddr->memOffset  = addr;
    gAddr->memWidth   = 8;
    store->uses.push_back({gAddr->id, 0});
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

/// Build a self-recursive function.
std::unique_ptr<SSAFunction> makeSelfRecursiveFn(const std::string& name) {
    auto fn = std::make_unique<SSAFunction>(name);
    fn->addBlock("entry");
    IrInstr* call = fn->addInstr(0, IrInstr::Op::Call);
    call->calleeName = name;  // calls itself
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);
    return fn;
}

CallingConvention makeVoidCC() {
    CallingConvention cc;
    cc.cc = CC::SysVAmd64;
    cc.ret.kind = RetKind::Void;
    return cc;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// 1. CallGraph
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CallGraph, EmptyModule_EmptyGraph) {
    CallGraph cg;
    cg.build({});
    EXPECT_TRUE(cg.nodes().empty());
    EXPECT_TRUE(cg.edges().empty());
}

TEST(CallGraph, SingleFunction_NoEdges) {
    auto fn = makePureFn("foo");
    CallGraph cg;
    cg.build({fn.get()});
    EXPECT_EQ(cg.nodes().size(), 1u);
    EXPECT_TRUE(cg.edges().empty());
    EXPECT_EQ(cg.callCount("foo"), 0u);
}

TEST(CallGraph, TwoFunctions_OneEdge) {
    auto foo = makePureFn("foo");
    auto bar = makeCallerFn("bar", "foo");
    CallGraph cg;
    cg.build({foo.get(), bar.get()});
    EXPECT_EQ(cg.edges().size(), 1u);
    EXPECT_EQ(cg.edges()[0].caller, "bar");
    EXPECT_EQ(cg.edges()[0].callee, "foo");
    EXPECT_EQ(cg.callCount("foo"), 1u);
    EXPECT_EQ(cg.callCount("bar"), 0u);

    const auto& succs = cg.successors("bar");
    ASSERT_EQ(succs.size(), 1u);
    EXPECT_EQ(succs[0], "foo");

    const auto& preds = cg.predecessors("foo");
    ASSERT_EQ(preds.size(), 1u);
    EXPECT_EQ(preds[0], "bar");
}

TEST(CallGraph, SelfCall_OneEdge) {
    auto rec = makeSelfRecursiveFn("rec");
    CallGraph cg;
    cg.build({rec.get()});
    EXPECT_EQ(cg.edges().size(), 1u);
    EXPECT_EQ(cg.callCount("rec"), 1u);
}

TEST(CallGraph, MultipleCallers) {
    auto leaf  = makePureFn("leaf");
    auto c1    = makeCallerFn("c1", "leaf");
    auto c2    = makeCallerFn("c2", "leaf");
    CallGraph cg;
    cg.build({leaf.get(), c1.get(), c2.get()});
    EXPECT_EQ(cg.callCount("leaf"), 2u);
}

TEST(CallGraph, NullFunctionSkipped) {
    auto fn = makePureFn("fn");
    CallGraph cg;
    cg.build({fn.get(), nullptr});
    EXPECT_EQ(cg.nodes().size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 2. TarjanScc
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TarjanScc, SingleNode_OneScc_NonRecursive) {
    auto fn = makePureFn("foo");
    CallGraph cg; cg.build({fn.get()});
    TarjanScc t;
    auto sccs = t.run(cg);
    ASSERT_EQ(sccs.size(), 1u);
    EXPECT_EQ(sccs[0].members.size(), 1u);
    EXPECT_FALSE(sccs[0].isRecursive);
}

TEST(TarjanScc, LinearChain_ThreeSCCs_Ordered) {
    // a calls b, b calls c (no cycles)
    auto a = makeCallerFn("a", "b");
    auto b = makeCallerFn("b", "c");
    auto c = makePureFn("c");
    CallGraph cg; cg.build({a.get(), b.get(), c.get()});
    TarjanScc t;
    auto sccs = t.run(cg);
    EXPECT_EQ(sccs.size(), 3u);
    // Reverse topo: c must come before b, b before a.
    std::unordered_map<std::string, int> pos;
    for (int i = 0; i < (int)sccs.size(); ++i) {
        for (const FnName& m : sccs[i].members) pos[m] = i;
    }
    EXPECT_LT(pos["c"], pos["b"]);
    EXPECT_LT(pos["b"], pos["a"]);
}

TEST(TarjanScc, SelfRecursive_IsRecursive) {
    auto rec = makeSelfRecursiveFn("rec");
    CallGraph cg; cg.build({rec.get()});
    TarjanScc t;
    auto sccs = t.run(cg);
    ASSERT_EQ(sccs.size(), 1u);
    EXPECT_TRUE(sccs[0].isRecursive);
}

TEST(TarjanScc, MutualRecursion_OneSCC_IsRecursive) {
    // a calls b, b calls a
    auto a = makeCallerFn("a", "b");
    auto b = makeCallerFn("b", "a");
    CallGraph cg; cg.build({a.get(), b.get()});
    TarjanScc t;
    auto sccs = t.run(cg);
    ASSERT_EQ(sccs.size(), 1u);
    EXPECT_TRUE(sccs[0].isRecursive);
    EXPECT_EQ(sccs[0].members.size(), 2u);
}

TEST(TarjanScc, EmptyGraph_EmptySccs) {
    CallGraph cg; cg.build({});
    TarjanScc t;
    auto sccs = t.run(cg);
    EXPECT_TRUE(sccs.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3. FunctionSummary
// ═══════════════════════════════════════════════════════════════════════════════

TEST(FunctionSummary, DefaultConstruct) {
    FunctionSummary s;
    EXPECT_TRUE(s.isVoid);
    EXPECT_FALSE(s.isPure);
    EXPECT_FALSE(s.isNoReturn);
    EXPECT_TRUE(s.params.empty());
}

TEST(FunctionSummary, Equality_SameParams) {
    FunctionSummary a, b;
    a.params.push_back({64, false, false, false, false, UINT32_MAX});
    b.params.push_back({64, false, false, false, false, UINT32_MAX});
    EXPECT_EQ(a, b);
}

TEST(FunctionSummary, Equality_DiffWidth) {
    FunctionSummary a, b;
    a.params.push_back({32, false, false, false, false, UINT32_MAX});
    b.params.push_back({64, false, false, false, false, UINT32_MAX});
    EXPECT_NE(a, b);
}

TEST(FunctionSummary, ToString_VoidNoParams) {
    FunctionSummary s;
    s.name   = "foo";
    s.isVoid = true;
    auto str = s.toString();
    EXPECT_NE(str.find("foo"), std::string::npos);
    EXPECT_NE(str.find("void"), std::string::npos);
}

TEST(FunctionSummary, ToString_Pure) {
    FunctionSummary s;
    s.name   = "bar";
    s.isPure = true;
    s.isVoid = true;
    auto str = s.toString();
    EXPECT_NE(str.find("pure"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 4. SummaryComputer
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SummaryComputer, PureFunction_IsPure) {
    auto fn = makePureFn("foo", 5);
    SummaryComputer cmp;
    auto s = cmp.compute(*fn, makeVoidCC());
    EXPECT_TRUE(s.isPure);
    EXPECT_FALSE(s.isNoReturn);
    EXPECT_EQ(s.name, "foo");
}

TEST(SummaryComputer, GlobalWrite_NotPure) {
    auto fn = makeGlobalWriterFn("writer");
    SummaryComputer cmp;
    auto s = cmp.compute(*fn, makeVoidCC());
    EXPECT_FALSE(s.isPure);
    EXPECT_FALSE(s.globalWrites.empty());
}

TEST(SummaryComputer, GlobalRead_Recorded) {
    auto fn = std::make_unique<SSAFunction>("reader");
    fn->addBlock("entry");
    IrInstr* load = fn->addInstr(0, IrInstr::Op::Load);
    IrValue* gAddr = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    gAddr->memIsStack = false;
    gAddr->memOffset  = 0x2000;
    load->uses.push_back({gAddr->id, 0});
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    SummaryComputer cmp;
    auto s = cmp.compute(*fn, makeVoidCC());
    EXPECT_FALSE(s.globalReads.empty());
}

TEST(SummaryComputer, NoReturn_IsNoReturn) {
    auto fn = std::make_unique<SSAFunction>("noret");
    fn->addBlock("entry");
    fn->addInstr(0, IrInstr::Op::Add);
    // No RET instruction.
    SSAPass pass; pass.run(*fn);

    SummaryComputer cmp;
    auto s = cmp.compute(*fn, makeVoidCC());
    EXPECT_TRUE(s.isNoReturn);
}

TEST(SummaryComputer, InstrCount_Correct) {
    auto fn = makePureFn("foo", 5);
    SummaryComputer cmp;
    auto s = cmp.compute(*fn, makeVoidCC());
    EXPECT_EQ(s.instrCount, 5u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 5. IpaPropagation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(IpaPropagation, NoCalls_SummariesUnchanged) {
    auto fn = makePureFn("fn");
    CallGraph cg; cg.build({fn.get()});
    TarjanScc t;
    auto sccs = t.run(cg);

    std::unordered_map<FnName, FunctionSummary> sums;
    sums["fn"] = FunctionSummary{}; sums["fn"].name = "fn";

    std::unordered_map<FnName, const SSAFunction*> fnMap = {{"fn", fn.get()}};
    std::unordered_map<FnName, CallingConvention> ccMap  = {{"fn", makeVoidCC()}};

    IpaPropagation prop;
    auto before = sums;
    prop.propagate(sccs, sums, fnMap, ccMap);
    EXPECT_EQ(sums["fn"], before["fn"]);
}

TEST(IpaPropagation, GlobalWrite_PropagatesUpward) {
    // writer writes to global; caller calls writer → global should propagate.
    auto writer = makeGlobalWriterFn("writer");
    auto caller = makeCallerFn("caller", "writer");

    CallGraph cg; cg.build({writer.get(), caller.get()});
    TarjanScc t;
    auto sccs = t.run(cg);

    std::unordered_map<FnName, FunctionSummary> sums;
    SummaryComputer cmp;
    sums["writer"] = cmp.compute(*writer, makeVoidCC());
    sums["caller"] = cmp.compute(*caller, makeVoidCC());

    std::unordered_map<FnName, const SSAFunction*> fnMap = {
        {"writer", writer.get()}, {"caller", caller.get()}
    };
    std::unordered_map<FnName, CallingConvention> ccMap = {
        {"writer", makeVoidCC()}, {"caller", makeVoidCC()}
    };

    IpaPropagation prop;
    prop.propagate(sccs, sums, fnMap, ccMap);

    // caller's summary should now reflect writer's global writes.
    EXPECT_FALSE(sums["caller"].globalWrites.empty());
    EXPECT_FALSE(sums["caller"].isPure);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 6. GlobalTyper
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GlobalTyper, TwoFunctions_SameGlobal_Unified) {
    auto w1 = makeGlobalWriterFn("w1", 0x1000);
    auto w2 = makeGlobalWriterFn("w2", 0x1000);

    std::unordered_map<FnName, FunctionSummary> sums;
    GlobalTyper typer;
    auto globals = typer.run({w1.get(), w2.get()}, sums);

    EXPECT_EQ(globals.size(), 1u);
    const auto& gv = globals.begin()->second;
    EXPECT_EQ(gv.writers.size(), 2u);
    EXPECT_TRUE(gv.writers.count("w1"));
    EXPECT_TRUE(gv.writers.count("w2"));
}

TEST(GlobalTyper, TwoFunctions_DiffGlobal_TwoEntries) {
    auto w1 = makeGlobalWriterFn("w1", 0x1000);
    auto w2 = makeGlobalWriterFn("w2", 0x2000);

    std::unordered_map<FnName, FunctionSummary> sums;
    GlobalTyper typer;
    auto globals = typer.run({w1.get(), w2.get()}, sums);

    EXPECT_EQ(globals.size(), 2u);
}

TEST(GlobalTyper, StackAccess_NotGlobal) {
    auto fn = std::make_unique<SSAFunction>("fn");
    fn->addBlock("entry");
    IrInstr* st = fn->addInstr(0, IrInstr::Op::Store);
    IrValue* slot = fn->allocValue(ValueKind::MemRef, kInvalidVar);
    slot->memIsStack = true;  // stack — not global
    slot->memOffset  = -8;
    st->uses.push_back({slot->id, 0});
    fn->addInstr(0, IrInstr::Op::Ret);
    SSAPass pass; pass.run(*fn);

    std::unordered_map<FnName, FunctionSummary> sums;
    GlobalTyper typer;
    auto globals = typer.run({fn.get()}, sums);
    EXPECT_TRUE(globals.empty());
}

TEST(GlobalTyper, GlobalVarInfoToString) {
    GlobalVarInfo gv;
    gv.name    = "g_0x1000";
    gv.address = 0x1000;
    gv.width   = 64;
    auto s = gv.toString();
    EXPECT_NE(s.find("g_0x1000"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 7. InlineCandidateFinder
// ═══════════════════════════════════════════════════════════════════════════════

TEST(InlineCandidateFinder, PureSmallSingleCall_IsCandidate) {
    auto leaf   = makePureFn("leaf", 5);
    auto caller = makeCallerFn("caller", "leaf");

    CallGraph cg; cg.build({leaf.get(), caller.get()});
    TarjanScc t;
    auto sccs = t.run(cg);

    std::unordered_map<FnName, FunctionSummary> sums;
    SummaryComputer cmp;
    sums["leaf"]   = cmp.compute(*leaf,   makeVoidCC());
    sums["caller"] = cmp.compute(*caller, makeVoidCC());
    sums["leaf"].callCount = 1;

    InlineCandidateFinder finder;
    auto cands = finder.run(cg, sccs, sums);
    ASSERT_EQ(cands.size(), 1u);
    EXPECT_EQ(cands[0].name, "leaf");
    EXPECT_EQ(cands[0].callerName, "caller");
}

TEST(InlineCandidateFinder, TooLarge_NotCandidate) {
    auto leaf   = makePureFn("big", 25);  // 25 > 20
    auto caller = makeCallerFn("caller", "big");
    CallGraph cg; cg.build({leaf.get(), caller.get()});
    TarjanScc t; auto sccs = t.run(cg);

    std::unordered_map<FnName, FunctionSummary> sums;
    SummaryComputer cmp;
    sums["big"]    = cmp.compute(*leaf,   makeVoidCC());
    sums["caller"] = cmp.compute(*caller, makeVoidCC());
    sums["big"].callCount = 1;

    InlineCandidateFinder finder;
    auto cands = finder.run(cg, sccs, sums);
    EXPECT_TRUE(cands.empty());
}

TEST(InlineCandidateFinder, NotPure_NotCandidate) {
    auto writer = makeGlobalWriterFn("writer");
    auto caller = makeCallerFn("caller", "writer");
    CallGraph cg; cg.build({writer.get(), caller.get()});
    TarjanScc t; auto sccs = t.run(cg);

    std::unordered_map<FnName, FunctionSummary> sums;
    SummaryComputer cmp;
    sums["writer"] = cmp.compute(*writer, makeVoidCC());
    sums["caller"] = cmp.compute(*caller, makeVoidCC());
    sums["writer"].callCount = 1;

    InlineCandidateFinder finder;
    auto cands = finder.run(cg, sccs, sums);
    EXPECT_TRUE(cands.empty());
}

TEST(InlineCandidateFinder, Recursive_NotCandidate) {
    auto rec = makeSelfRecursiveFn("rec");
    CallGraph cg; cg.build({rec.get()});
    TarjanScc t; auto sccs = t.run(cg);

    std::unordered_map<FnName, FunctionSummary> sums;
    SummaryComputer cmp;
    sums["rec"] = cmp.compute(*rec, makeVoidCC());
    sums["rec"].callCount = 1;

    InlineCandidateFinder finder;
    auto cands = finder.run(cg, sccs, sums);
    EXPECT_TRUE(cands.empty());
}

TEST(InlineCandidateFinder, MultipleCalls_NotCandidate) {
    // Same function called from two callers.
    auto leaf  = makePureFn("leaf", 3);
    auto c1    = makeCallerFn("c1", "leaf");
    auto c2    = makeCallerFn("c2", "leaf");
    CallGraph cg; cg.build({leaf.get(), c1.get(), c2.get()});
    TarjanScc t; auto sccs = t.run(cg);

    std::unordered_map<FnName, FunctionSummary> sums;
    SummaryComputer cmp;
    sums["leaf"] = cmp.compute(*leaf, makeVoidCC());
    sums["c1"]   = cmp.compute(*c1,   makeVoidCC());
    sums["c2"]   = cmp.compute(*c2,   makeVoidCC());
    sums["leaf"].callCount = 2;

    InlineCandidateFinder finder;
    auto cands = finder.run(cg, sccs, sums);
    EXPECT_TRUE(cands.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// 8. IpaPass — full pipeline
// ═══════════════════════════════════════════════════════════════════════════════

TEST(IpaPass, EmptyModule) {
    IpaPass pass;
    auto res = pass.run({}, {});
    EXPECT_TRUE(res.summaries.empty());
    EXPECT_TRUE(res.sccs.empty());
}

TEST(IpaPass, SinglePureFunction) {
    auto fn = makePureFn("foo", 5);
    std::unordered_map<FnName, CallingConvention> ccMap = {{"foo", makeVoidCC()}};
    IpaPass pass;
    auto res = pass.run({fn.get()}, ccMap);
    EXPECT_EQ(res.summaries.size(), 1u);
    EXPECT_TRUE(res.summaries.count("foo"));
    EXPECT_TRUE(res.summaries.at("foo").isPure);
}

TEST(IpaPass, GlobalWriterNotPure) {
    auto fn = makeGlobalWriterFn("gwriter");
    std::unordered_map<FnName, CallingConvention> ccMap = {{"gwriter", makeVoidCC()}};
    IpaPass pass;
    auto res = pass.run({fn.get()}, ccMap);
    EXPECT_FALSE(res.summaries.at("gwriter").isPure);
    EXPECT_EQ(res.globals.size(), 1u);
}

TEST(IpaPass, InlineCandidate_FoundByPass) {
    auto leaf   = makePureFn("leaf", 5);
    auto caller = makeCallerFn("caller", "leaf");
    std::unordered_map<FnName, CallingConvention> ccMap = {
        {"leaf", makeVoidCC()}, {"caller", makeVoidCC()}
    };
    IpaPass pass;
    auto res = pass.run({leaf.get(), caller.get()}, ccMap);
    EXPECT_EQ(res.inlineCandidates.size(), 1u);
    EXPECT_EQ(res.inlineCandidates[0].name, "leaf");
}

TEST(IpaPass, SccCount_Correct) {
    // a calls b, b calls c → 3 SCCs
    auto a = makeCallerFn("a", "b");
    auto b = makeCallerFn("b", "c");
    auto c = makePureFn("c");
    std::unordered_map<FnName, CallingConvention> ccMap = {
        {"a", makeVoidCC()}, {"b", makeVoidCC()}, {"c", makeVoidCC()}
    };
    IpaPass pass;
    auto res = pass.run({a.get(), b.get(), c.get()}, ccMap);
    EXPECT_EQ(res.sccs.size(), 3u);
}

TEST(IpaPass, RecursiveSCC_MarkedRecursive) {
    auto rec = makeSelfRecursiveFn("rec");
    std::unordered_map<FnName, CallingConvention> ccMap = {{"rec", makeVoidCC()}};
    IpaPass pass;
    auto res = pass.run({rec.get()}, ccMap);
    EXPECT_TRUE(res.summaries.at("rec").isRecursive);
}

TEST(IpaPass, IpaResultSummary_ContainsStats) {
    auto fn = makePureFn("foo");
    IpaPass pass;
    auto res = pass.run({fn.get()}, {{"foo", makeVoidCC()}});
    auto s = res.summary();
    EXPECT_NE(s.find("IPA"), std::string::npos);
    EXPECT_NE(s.find("1"), std::string::npos);  // 1 function
}

TEST(IpaPass, NullFunctionSkipped) {
    auto fn = makePureFn("fn");
    IpaPass pass;
    auto res = pass.run({fn.get(), nullptr}, {{"fn", makeVoidCC()}});
    EXPECT_EQ(res.summaries.size(), 1u);
}
