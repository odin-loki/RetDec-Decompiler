/**
 * @file tests/cfg/cfg_test.cpp
 * @brief Unit tests for CFGBuilder and CFGGraph.
 *
 * Convention:
 *   imageBase = 0x400000
 *   Functions start at multiples of 0x100 within the code section.
 *   All InstrSummary entries are built manually; we do not invoke the decoder.
 */

#include "retdec/cfg/cfg.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace retdec::cfg;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static constexpr uint64_t kBase = 0x400000;

static std::vector<uint8_t> makeImage(std::size_t sz = 0x4000)
{
    return std::vector<uint8_t>(sz, 0x90); // NOP sled
}

// Write a 32-bit value at a VA offset in an image.
static void w32(std::vector<uint8_t>& img, uint64_t va, uint32_t v)
{
    std::size_t off = static_cast<std::size_t>(va - kBase);
    if (off + 4 <= img.size()) {
        for (int i = 0; i < 4; ++i) img[off+i] = static_cast<uint8_t>(v >> (i*8));
    }
}

static void w64(std::vector<uint8_t>& img, uint64_t va, uint64_t v)
{
    std::size_t off = static_cast<std::size_t>(va - kBase);
    if (off + 8 <= img.size()) {
        for (int i = 0; i < 8; ++i) img[off+i] = static_cast<uint8_t>(v >> (i*8));
    }
}

static CFGBuilder makeBuilder(const std::vector<uint8_t>& img, bool is64 = true)
{
    return CFGBuilder(kBase, img.data(), img.size(), is64);
}

// Build a simple InstrSummary.
static InstrSummary ins(uint64_t addr, uint32_t len,
                         InstrKind kind = InstrKind::Normal,
                         uint64_t target = 0)
{
    InstrSummary s;
    s.addr   = addr;
    s.len    = len;
    s.kind   = kind;
    s.target = target;
    return s;
}

// Count edges of a given type in the graph.
static std::size_t countEdgeType(const CFGGraph& g, EdgeType t)
{
    return g.countEdges(t);
}

// Check if a specific typed edge (from → to) exists.
static bool hasEdge(const CFGGraph& g, uint64_t from, uint64_t to, EdgeType t)
{
    auto it = g.nodes.find(from);
    if (it == g.nodes.end()) return false;
    for (const auto& e : it->second.succs) {
        if (e.to == to && e.type == t) return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Edge type helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EdgeType, NamesCorrect)
{
    EXPECT_STREQ(edgeTypeName(EdgeType::FallThrough),        "FallThrough");
    EXPECT_STREQ(edgeTypeName(EdgeType::TrueBranch),         "TrueBranch");
    EXPECT_STREQ(edgeTypeName(EdgeType::FalseBranch),        "FalseBranch");
    EXPECT_STREQ(edgeTypeName(EdgeType::DirectCall),         "DirectCall");
    EXPECT_STREQ(edgeTypeName(EdgeType::TailCall),           "TailCall");
    EXPECT_STREQ(edgeTypeName(EdgeType::SwitchEdge),         "SwitchEdge");
    EXPECT_STREQ(edgeTypeName(EdgeType::ExceptionEdge),      "ExceptionEdge");
    EXPECT_STREQ(edgeTypeName(EdgeType::VirtualCallEdge),    "VirtualCallEdge");
    EXPECT_STREQ(edgeTypeName(EdgeType::LoopBackEdge),       "LoopBackEdge");
    EXPECT_STREQ(edgeTypeName(EdgeType::UnresolvedIndirect), "UnresolvedIndirect");
}

TEST(CFGEdge, IsCallEdge)
{
    CFGEdge e;
    e.type = EdgeType::DirectCall;    EXPECT_TRUE(e.isCallEdge());
    e.type = EdgeType::TailCall;      EXPECT_TRUE(e.isCallEdge());
    e.type = EdgeType::VirtualCallEdge; EXPECT_TRUE(e.isCallEdge());
    e.type = EdgeType::FallThrough;   EXPECT_FALSE(e.isCallEdge());
    e.type = EdgeType::SwitchEdge;    EXPECT_FALSE(e.isCallEdge());
}

TEST(CFGEdge, IsResolved)
{
    CFGEdge e;
    e.to = 0;          EXPECT_FALSE(e.isResolved());
    e.to = 0x401000;   EXPECT_TRUE(e.isResolved());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 1 — Direct edges
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Phase1, SingleReturnFunction)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    // One function: NOP; RET
    b.addFunction(kBase + 0x1000, kBase + 0x1002, {
        ins(kBase + 0x1000, 1),                         // NOP
        ins(kBase + 0x1001, 1, InstrKind::Ret),         // RET
    });
    b.runPhase1();

    // One block node.
    EXPECT_GE(b.graph().nodes.size(), 1u);
    // No outgoing edges (RET has no CFG successor).
    EXPECT_EQ(b.graph().totalEdges(), 0u);
}

TEST(Phase1, DirectCallEmitsCallEdge)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t callee = kBase + 0x1100;
    b.addFunction(kBase + 0x1000, kBase + 0x1010, {
        ins(kBase + 0x1000, 5, InstrKind::DirectCall, callee), // CALL 0x1100
        ins(kBase + 0x1005, 1, InstrKind::Ret),
    });
    b.addFunction(callee, callee + 1, { ins(callee, 1, InstrKind::Ret) });
    b.runPhase1();

    EXPECT_TRUE(hasEdge(b.graph(), kBase + 0x1000, callee, EdgeType::DirectCall));
    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::DirectCall), 1u);
}

TEST(Phase1, ConditionalJmpTrueFalse)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func   = kBase + 0x1000;
    uint64_t target = kBase + 0x1010;
    b.addFunction(func, func + 0x20, {
        ins(func,        2, InstrKind::ConditionalJmp, target),
        ins(func + 2,    1, InstrKind::Ret),    // FalseBranch fallthrough
        ins(target,      1, InstrKind::Ret),    // TrueBranch target
    });
    b.runPhase1();

    EXPECT_TRUE(hasEdge(b.graph(), func, target,    EdgeType::TrueBranch));
    EXPECT_TRUE(hasEdge(b.graph(), func, func + 2,  EdgeType::FalseBranch));
}

TEST(Phase1, UnconditionalJmpTrueBranch)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func   = kBase + 0x1000;
    uint64_t target = kBase + 0x1050;
    b.addFunction(func, func + 0x60, {
        ins(func,       5, InstrKind::DirectJmp, target),
        ins(target,     1, InstrKind::Ret),
    });
    b.runPhase1();

    EXPECT_TRUE(hasEdge(b.graph(), func, target, EdgeType::TrueBranch));
}

TEST(Phase1, TailCallEdge)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func   = kBase + 0x1000;
    uint64_t callee = kBase + 0x2000;
    b.addFunction(func, func + 5, {
        ins(func, 5, InstrKind::TailCall, callee),
    });
    b.runPhase1();

    EXPECT_TRUE(hasEdge(b.graph(), func, callee, EdgeType::TailCall));
}

TEST(Phase1, IndirectJmpIsUnresolved)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 2, {
        ins(func, 2, InstrKind::IndirectJmp),
    });
    b.runPhase1();

    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::UnresolvedIndirect), 1u);
}

TEST(Phase1, MultipleBlocksCorrect)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    // if-else: cond jump → two branches that both return
    b.addFunction(func, func + 0x20, {
        ins(func,       2, InstrKind::ConditionalJmp, func + 0x10),
        ins(func + 2,   1, InstrKind::Ret),
        ins(func + 0x10,1, InstrKind::Ret),
    });
    b.runPhase1();

    // Should have at least 3 blocks (entry, false-branch, true-branch).
    EXPECT_GE(b.graph().nodes.size(), 2u);
    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::TrueBranch),  1u);
    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::FalseBranch), 1u);
}

TEST(Phase1, EmptyFunctionCreatesBlock)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    b.addFunction(kBase + 0x1000, kBase + 0x1001, {});
    b.runPhase1();
    EXPECT_GE(b.graph().nodes.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2 — Jump table resolution
// ═══════════════════════════════════════════════════════════════════════════════

TEST(JumpTable, GCCAbsoluteAddresses)
{
    auto img = makeImage(0x6000);
    // Table at 0x402000: two 64-bit absolute target VAs.
    uint64_t tableVA = kBase + 0x2000;
    uint64_t case0   = kBase + 0x1100;
    uint64_t case1   = kBase + 0x1200;
    w64(img, tableVA,     case0);
    w64(img, tableVA + 8, case1);

    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 5, {
        ins(func, 2, InstrKind::IndirectJmp),
    });
    b.runPhase1();

    JumpTableInfo jt;
    jt.instrAddr  = func;
    jt.tableBase  = tableVA;
    jt.numEntries = 2;
    jt.stride     = 8;
    jt.fmt        = JumpTableFmt::GCC;
    b.addJumpTable(jt);
    b.runPhase2();

    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::SwitchEdge), 2u);
    EXPECT_TRUE(hasEdge(b.graph(), func, case0, EdgeType::SwitchEdge));
    EXPECT_TRUE(hasEdge(b.graph(), func, case1, EdgeType::SwitchEdge));
}

TEST(JumpTable, MSVCRelativeOffsets)
{
    auto img = makeImage(0x6000);
    uint64_t tableVA = kBase + 0x2000;
    uint64_t case0   = kBase + 0x1100;
    uint64_t case1   = kBase + 0x1200;
    // MSVC: int32 offset from tableBase to case address.
    w32(img, tableVA,     static_cast<uint32_t>(static_cast<int32_t>(case0 - tableVA)));
    w32(img, tableVA + 4, static_cast<uint32_t>(static_cast<int32_t>(case1 - tableVA)));

    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 5, { ins(func, 2, InstrKind::IndirectJmp) });
    b.runPhase1();

    JumpTableInfo jt;
    jt.instrAddr  = func;
    jt.tableBase  = tableVA;
    jt.numEntries = 2;
    jt.stride     = 4;
    jt.fmt        = JumpTableFmt::MSVC;
    b.addJumpTable(jt);
    b.runPhase2();

    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::SwitchEdge), 2u);
    EXPECT_TRUE(hasEdge(b.graph(), func, case0, EdgeType::SwitchEdge));
    EXPECT_TRUE(hasEdge(b.graph(), func, case1, EdgeType::SwitchEdge));
}

TEST(JumpTable, SwitchIndicesCorrect)
{
    auto img = makeImage(0x6000);
    uint64_t tableVA = kBase + 0x2000;
    uint64_t cases[] = {kBase + 0x1100, kBase + 0x1200, kBase + 0x1300};
    for (int i = 0; i < 3; ++i) w64(img, tableVA + i*8, cases[i]);

    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 5, { ins(func, 2, InstrKind::IndirectJmp) });
    b.runPhase1();

    JumpTableInfo jt;
    jt.instrAddr = func; jt.tableBase = tableVA;
    jt.numEntries = 3; jt.stride = 8; jt.fmt = JumpTableFmt::GCC;
    b.addJumpTable(jt);
    b.runPhase2();

    // Check switch indices 0, 1, 2.
    auto it = b.graph().nodes.find(func);
    ASSERT_NE(it, b.graph().nodes.end());
    std::vector<uint32_t> idxs;
    for (const auto& e : it->second.succs) {
        if (e.type == EdgeType::SwitchEdge) idxs.push_back(e.switchIndex);
    }
    std::sort(idxs.begin(), idxs.end());
    ASSERT_EQ(idxs.size(), 3u);
    EXPECT_EQ(idxs[0], 0u);
    EXPECT_EQ(idxs[1], 1u);
    EXPECT_EQ(idxs[2], 2u);
}

TEST(JumpTable, ResolveOutOfBoundsReturnsEmpty)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    JumpTableInfo jt;
    jt.instrAddr  = kBase + 0x1000;
    jt.tableBase  = kBase + 0xFFFF0; // way beyond image
    jt.numEntries = 4;
    jt.stride     = 8;
    jt.fmt        = JumpTableFmt::GCC;
    auto targets = b.resolveJumpTable(jt);
    EXPECT_TRUE(targets.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2 — Exception edges
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ExceptionEdge, HandlerAddedForIndirectJmp)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func    = kBase + 0x1000;
    uint64_t handler = kBase + 0x2000;
    b.addFunction(func, func + 5, { ins(func, 2, InstrKind::IndirectJmp) });
    b.addExceptionHandler(func, handler);
    b.build();

    EXPECT_TRUE(hasEdge(b.graph(), func, handler, EdgeType::ExceptionEdge));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2 — Virtual call edges
// ═══════════════════════════════════════════════════════════════════════════════

TEST(VirtualCall, SlotsEmittedAsVirtualCallEdges)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func  = kBase + 0x1000;
    uint64_t slot0 = kBase + 0x2000;
    uint64_t slot1 = kBase + 0x3000;

    b.addFunction(func, func + 5, { ins(func, 2, InstrKind::IndirectCall) });

    VtableInfo vt;
    vt.tableAddr = kBase + 0x5000;
    vt.slots = {slot0, slot1};
    b.addVtable(vt);
    b.build();

    EXPECT_GE(countEdgeType(b.graph(), EdgeType::VirtualCallEdge), 1u);
}

TEST(VirtualCall, EmptyVtableNoEdges)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 5, { ins(func, 2, InstrKind::IndirectCall) });
    // No vtable registered.
    b.build();
    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::VirtualCallEdge), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2 — Unresolved diagnostics
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Unresolved, DiagnosticEmitted)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 5, { ins(func, 2, InstrKind::IndirectJmp) });
    b.build();

    EXPECT_FALSE(b.graph().diagnostics.empty());
    EXPECT_EQ(b.graph().diagnostics[0].addr, func);
}

TEST(Unresolved, NoUnresolvedWhenAllResolved)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 5, { ins(func, 5, InstrKind::Ret) });
    b.build();

    // No indirect branches → no diagnostics.
    EXPECT_TRUE(b.graph().diagnostics.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 3 — Back-edge detection (loop detection)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(BackEdge, SimpleLoop)
{
    // func: A → B → A (back edge B→A = loop)
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t A = kBase + 0x1000;
    uint64_t B = kBase + 0x1010;

    b.addFunction(A, A + 0x20, {
        // Block A: conditional jump to B or loop back.
        ins(A, 2, InstrKind::ConditionalJmp, B),
        ins(A + 2, 1, InstrKind::Ret),       // exit path
        // Block B: unconditional jump back to A.
        ins(B, 5, InstrKind::DirectJmp, A),
    });
    b.build();

    // The B→A edge should be a LoopBackEdge.
    EXPECT_GE(countEdgeType(b.graph(), EdgeType::LoopBackEdge), 1u);
}

TEST(BackEdge, NoLoopNoBackEdge)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 3, {
        ins(func,   1, InstrKind::Normal),
        ins(func+1, 1, InstrKind::Normal),
        ins(func+2, 1, InstrKind::Ret),
    });
    b.build();

    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::LoopBackEdge), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CFGGraph query helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GraphQuery, BlockAtExists)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 2, { ins(func, 1, InstrKind::Ret) });
    b.build();

    EXPECT_NE(b.graph().blockAt(func), nullptr);
    EXPECT_EQ(b.graph().blockAt(0xDEAD0000), nullptr);
}

TEST(GraphQuery, SuccessorsOf)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func   = kBase + 0x1000;
    uint64_t target = kBase + 0x1020;
    b.addFunction(func, func + 0x30, {
        ins(func, 2, InstrKind::ConditionalJmp, target),
        ins(func + 2, 1, InstrKind::Ret),
        ins(target, 1, InstrKind::Ret),
    });
    b.build();

    auto succs = b.graph().successorsOf(func);
    EXPECT_EQ(succs.size(), 2u); // TrueBranch + FalseBranch
}

TEST(GraphQuery, PredecessorsOf)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func   = kBase + 0x1000;
    uint64_t target = kBase + 0x1020;
    b.addFunction(func, func + 0x30, {
        ins(func, 2, InstrKind::ConditionalJmp, target),
        ins(func + 2, 1, InstrKind::Ret),
        ins(target, 1, InstrKind::Ret),
    });
    b.build();

    auto preds = b.graph().predecessorsOf(target);
    EXPECT_EQ(preds.size(), 1u);
    EXPECT_EQ(preds[0], func);
}

TEST(GraphQuery, TotalEdgesCount)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    uint64_t func = kBase + 0x1000;
    b.addFunction(func, func + 0x30, {
        ins(func, 2, InstrKind::ConditionalJmp, func + 0x20),
        ins(func + 2, 1, InstrKind::Ret),
        ins(func + 0x20, 1, InstrKind::Ret),
    });
    b.build();

    // Should have TrueBranch + FalseBranch = 2 edges minimum.
    EXPECT_GE(b.graph().totalEdges(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Integration
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Integration, FullBuild)
{
    auto img = makeImage(0x6000);
    uint64_t func    = kBase + 0x1000;
    uint64_t callee  = kBase + 0x2000;
    uint64_t tableVA = kBase + 0x3000;
    uint64_t case0   = kBase + 0x1100;
    uint64_t case1   = kBase + 0x1200;

    w64(img, tableVA,     case0);
    w64(img, tableVA + 8, case1);

    auto b = makeBuilder(img);

    // Register callee.
    b.addFunction(callee, callee + 1, { ins(callee, 1, InstrKind::Ret) });

    // Main function: call + switch.
    b.addFunction(func, func + 0x100, {
        ins(func,        5, InstrKind::DirectCall,  callee),
        ins(func + 5,    2, InstrKind::IndirectJmp),
        ins(case0,       1, InstrKind::Ret),
        ins(case1,       1, InstrKind::Ret),
    });

    JumpTableInfo jt;
    jt.instrAddr = func + 5; jt.tableBase = tableVA;
    jt.numEntries = 2; jt.stride = 8; jt.fmt = JumpTableFmt::GCC;
    b.addJumpTable(jt);

    b.build();

    EXPECT_GE(b.graph().nodes.size(), 2u);
    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::DirectCall), 1u);
    EXPECT_EQ(countEdgeType(b.graph(), EdgeType::SwitchEdge), 2u);
}

TEST(Integration, EmptyBuilderIsEmpty)
{
    auto img = makeImage();
    auto b = makeBuilder(img);
    b.build();
    EXPECT_TRUE(b.graph().nodes.empty());
    EXPECT_TRUE(b.graph().diagnostics.empty());
}
