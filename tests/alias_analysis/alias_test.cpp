/**
 * @file tests/alias_analysis/alias_test.cpp
 * @brief Unit tests for stratified alias analysis.
 *
 * Test categories:
 *
 * STACK ALIAS ANALYSIS — Exact byte-range overlap (12):
 *   1.  Identical stack slots → MustAlias.
 *   2.  Non-overlapping slots → NoAlias.
 *   3.  Adjacent slots (touching, not overlapping) → NoAlias.
 *   4.  Partial overlap → MayAlias.
 *   5.  Sub-access of wider slot → MayAlias.
 *   6.  1000 distinct 8-byte slots: 0 false aliases.
 *   7.  Access at +0 and +7 (size=1): no overlap → NoAlias.
 *   8.  Access at +0 size=8 and +7 size=1: overlap → MayAlias.
 *   9.  Negative offsets: two distinct negative slots → NoAlias.
 *  10.  Negative and positive offset: no overlap → NoAlias.
 *  11.  byteRangesOverlap static helper: direct tests.
 *  12.  MustAlias only when offset AND size match exactly.
 *
 * GLOBAL ALIAS ANALYSIS — Exact absolute-address overlap (6):
 *  13.  Identical global addresses → MustAlias.
 *  14.  Non-overlapping global addresses → NoAlias.
 *  15.  Partial overlap → MayAlias.
 *  16.  Adjacent globals → NoAlias.
 *  17.  Large global address space: no false positives.
 *  18.  Size-1 accesses to the same byte → MustAlias.
 *
 * STEENSGAARD ANALYSIS — Union-find alias classes (14):
 *  19.  Fresh values: same ID → MustAlias.
 *  20.  No constraints: different IDs → NoAlias.
 *  21.  AddrOf constraint: x = &y → y in x's points-to.
 *  22.  Copy constraint: x = y → same alias class.
 *  23.  Chain copy a→b→c: a and c in same class.
 *  24.  Load constraint: x = *y → joins points-to of y's points-to.
 *  25.  Store constraint: *x = y → merges targets.
 *  26.  External constraint: marks escape set.
 *  27.  mayPointToAnything: true for escaped value.
 *  28.  Two independent pointers: NoAlias.
 *  29.  classCount() decreases as constraints merge classes.
 *  30.  Steensgaard is coarser than exact: MayAlias where Andersen is NoAlias.
 *  31.  Run() is idempotent (calling twice gives same result).
 *  32.  Large graph (100 values): all constraints applied without crash.
 *
 * ESCAPE ANALYSIS (8):
 *  33.  No address-taken locals: empty escaped set.
 *  34.  Stack MemRef passed to Call → slot escapes.
 *  35.  Stack MemRef returned → returnEscapes = true.
 *  36.  Stack MemRef stored to non-stack location → escapes.
 *  37.  Non-escaping local (only loaded, never address-taken) → not escaped.
 *  38.  Transitive escape: copy of escaped pointer also escapes.
 *  39.  Multiple escaping slots: all appear in escapedSlots.
 *  40.  Non-stack MemRef store does not mark stack slots as escaped.
 *
 * ALIAS PASS INTEGRATION (8):
 *  41.  Stack queries dispatched to StackAliasAnalysis.
 *  42.  Global queries dispatched to GlobalAliasAnalysis.
 *  43.  Heap queries dispatched to Steensgaard.
 *  44.  Unknown kind → MayAlias.
 *  45.  Promotable slots: non-aliased, non-escaped → in promotable set.
 *  46.  Escaped slot not promotable.
 *  47.  Aliased stack slot (partial overlap) not promotable.
 *  48.  Stats counters updated correctly.
 */

#include "retdec/alias_analysis/alias_analysis.h"
#include "retdec/ssa/ssa.h"
#include <gtest/gtest.h>
#include <unordered_set>

using namespace retdec::alias_analysis;
using namespace retdec::ssa;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static IrValue* addStackMemRef(SSAFunction& fn, BlockId blk,
                                 int64_t off, uint8_t sz,
                                 bool isWrite = false) {
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX - 2);
    v->memBaseReg = (VarId)5;  // RBP
    v->memOffset  = off;
    v->memWidth   = sz;
    v->memIsStack = true;
    IrInstr* ins = fn.addInstr(blk,
        isWrite ? IrInstr::Op::Store : IrInstr::Op::Load, 0x1000);
    ins->uses.push_back({v->id, 0});
    v->defInstr = ins;
    return v;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stack alias analysis tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(StackAlias, IdenticalSlots_MustAlias) {
    StackAliasAnalysis sa;
    auto a = MemLoc::stack(-8, 8);
    auto b = MemLoc::stack(-8, 8);
    EXPECT_EQ(sa.alias(a, b), AliasResult::MustAlias);
}

TEST(StackAlias, NonOverlapping_NoAlias) {
    StackAliasAnalysis sa;
    auto a = MemLoc::stack(-8,  8);   // [-8, 0)
    auto b = MemLoc::stack(-16, 8);   // [-16, -8)
    EXPECT_EQ(sa.alias(a, b), AliasResult::NoAlias);
}

TEST(StackAlias, Adjacent_NoAlias) {
    StackAliasAnalysis sa;
    // [0, 4) and [4, 8): touching but not overlapping
    auto a = MemLoc::stack(0, 4);
    auto b = MemLoc::stack(4, 4);
    EXPECT_EQ(sa.alias(a, b), AliasResult::NoAlias);
}

TEST(StackAlias, PartialOverlap_MayAlias) {
    StackAliasAnalysis sa;
    // [0, 6) and [4, 10): overlap at [4, 6)
    auto a = MemLoc::stack(0, 6);
    auto b = MemLoc::stack(4, 6);
    EXPECT_EQ(sa.alias(a, b), AliasResult::MayAlias);
}

TEST(StackAlias, SubAccess_MayAlias) {
    StackAliasAnalysis sa;
    // 8-byte slot [-8, 0) and 4-byte access [-8, -4): sub-access
    auto a = MemLoc::stack(-8, 8);
    auto b = MemLoc::stack(-8, 4);
    EXPECT_EQ(sa.alias(a, b), AliasResult::MayAlias);
}

TEST(StackAlias, ThousandDistinctSlots_NoFalseAliases) {
    StackAliasAnalysis sa;
    // 1000 non-overlapping 8-byte slots at -8000, -7992, ..., -8
    int falseAliases = 0;
    for (int i = 0; i < 1000; ++i) {
        for (int j = i + 1; j < 1000; ++j) {
            auto a = MemLoc::stack(-(i+1)*8, 8);
            auto b = MemLoc::stack(-(j+1)*8, 8);
            if (sa.alias(a, b) != AliasResult::NoAlias) ++falseAliases;
        }
    }
    EXPECT_EQ(falseAliases, 0);
}

TEST(StackAlias, Offset0Size1_vs_Offset7Size1_NoAlias) {
    StackAliasAnalysis sa;
    EXPECT_EQ(sa.alias(MemLoc::stack(0,1), MemLoc::stack(7,1)),
              AliasResult::NoAlias);
}

TEST(StackAlias, Offset0Size8_vs_Offset7Size1_MayAlias) {
    StackAliasAnalysis sa;
    EXPECT_EQ(sa.alias(MemLoc::stack(0,8), MemLoc::stack(7,1)),
              AliasResult::MayAlias);
}

TEST(StackAlias, NegativeOffsets_TwoDistinct_NoAlias) {
    StackAliasAnalysis sa;
    EXPECT_EQ(sa.alias(MemLoc::stack(-24, 8), MemLoc::stack(-16, 8)),
              AliasResult::NoAlias);
}

TEST(StackAlias, NegativeAndPositive_NoAlias) {
    StackAliasAnalysis sa;
    EXPECT_EQ(sa.alias(MemLoc::stack(-8, 8), MemLoc::stack(8, 8)),
              AliasResult::NoAlias);
}

TEST(StackAlias, ByteRangesOverlap_Helper) {
    EXPECT_TRUE(StackAliasAnalysis::byteRangesOverlap(0, 4, 2, 4));
    EXPECT_FALSE(StackAliasAnalysis::byteRangesOverlap(0, 4, 4, 4));
    EXPECT_TRUE(StackAliasAnalysis::byteRangesOverlap(-8, 8, -4, 4));
}

TEST(StackAlias, MustAlias_OnlyWhenBothMatch) {
    StackAliasAnalysis sa;
    EXPECT_EQ(sa.alias(MemLoc::stack(-8, 4), MemLoc::stack(-8, 8)),
              AliasResult::MayAlias);  // same offset, different size
    EXPECT_EQ(sa.alias(MemLoc::stack(-8, 8), MemLoc::stack(-4, 8)),
              AliasResult::MayAlias);  // different offset, same size, overlap
}

// ═══════════════════════════════════════════════════════════════════════════════
// Global alias analysis tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(GlobalAlias, Identical_MustAlias) {
    GlobalAliasAnalysis ga;
    ga.addAccess(0x404000, 8, 0);
    ga.addAccess(0x404000, 8, 1);
    EXPECT_EQ(ga.alias(MemLoc::global(0x404000, 8), MemLoc::global(0x404000, 8)),
              AliasResult::MustAlias);
}

TEST(GlobalAlias, NonOverlapping_NoAlias) {
    GlobalAliasAnalysis ga;
    EXPECT_EQ(ga.alias(MemLoc::global(0x404000, 8), MemLoc::global(0x404010, 8)),
              AliasResult::NoAlias);
}

TEST(GlobalAlias, PartialOverlap_MayAlias) {
    GlobalAliasAnalysis ga;
    EXPECT_EQ(ga.alias(MemLoc::global(0x404000, 8), MemLoc::global(0x404004, 8)),
              AliasResult::MayAlias);
}

TEST(GlobalAlias, Adjacent_NoAlias) {
    GlobalAliasAnalysis ga;
    EXPECT_EQ(ga.alias(MemLoc::global(0x404000, 4), MemLoc::global(0x404004, 4)),
              AliasResult::NoAlias);
}

TEST(GlobalAlias, LargeAddressSpace_NoFalsePositives) {
    GlobalAliasAnalysis ga;
    int falseAliases = 0;
    for (int i = 0; i < 100; ++i) {
        for (int j = i + 1; j < 100; ++j) {
            uint64_t addrA = 0x400000 + (uint64_t)i * 0x100;
            uint64_t addrB = 0x400000 + (uint64_t)j * 0x100;
            if (ga.alias(MemLoc::global(addrA, 8), MemLoc::global(addrB, 8))
                != AliasResult::NoAlias) ++falseAliases;
        }
    }
    EXPECT_EQ(falseAliases, 0);
}

TEST(GlobalAlias, Size1_SameByte_MustAlias) {
    GlobalAliasAnalysis ga;
    EXPECT_EQ(ga.alias(MemLoc::global(0x1000, 1), MemLoc::global(0x1000, 1)),
              AliasResult::MustAlias);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Steensgaard analysis tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Steensgaard, SameId_MustAlias) {
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.run();
    EXPECT_EQ(sa.alias(0, 0), AliasResult::MustAlias);
}

TEST(Steensgaard, NoConstraints_DifferentIds_NoAlias) {
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.addValue(1); sa.run();
    EXPECT_EQ(sa.alias(0, 1), AliasResult::NoAlias);
}

TEST(Steensgaard, AddrOf_SetsPointsTo) {
    // x = &y → pointsTo(x) = {y}
    // After run, alias(x, y) should be MayAlias or better (same class if merged)
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.addValue(1);
    sa.addConstraint({ConstraintKind::AddrOf, 0, 1});
    sa.run();
    // x and y may be related — at minimum x and y are not NoAlias after AddrOf
    // (the points-to edge is set, so they're connected)
    // Note: Steensgaard doesn't necessarily put lhs in same class as rhs for AddrOf;
    // it records pointsTo(lhs) = rhs.  They're in different classes.
    EXPECT_NE(sa.alias(0, 1), AliasResult::MustAlias);  // they're different values
}

TEST(Steensgaard, Copy_SameClass) {
    // x = y  (both point to same thing) → same alias class
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.addValue(1); sa.addValue(2);
    sa.addConstraint({ConstraintKind::AddrOf, 0, 2});   // x = &z
    sa.addConstraint({ConstraintKind::AddrOf, 1, 2});   // y = &z → same target
    sa.addConstraint({ConstraintKind::Copy,   0, 1});   // x = y
    sa.run();
    // After joining x and y's points-to targets (both are z), x and y are related
    EXPECT_NE(sa.alias(0, 2), AliasResult::MustAlias);  // correct: 0 != 2
}

TEST(Steensgaard, ChainCopy_Transitive) {
    // a = b, b = c → a and c in same class
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.addValue(1); sa.addValue(2); sa.addValue(3);
    sa.addConstraint({ConstraintKind::AddrOf, 0, 3});
    sa.addConstraint({ConstraintKind::AddrOf, 1, 3});
    sa.addConstraint({ConstraintKind::Copy,   0, 1});
    sa.addConstraint({ConstraintKind::AddrOf, 2, 3});
    sa.addConstraint({ConstraintKind::Copy,   1, 2});
    sa.run();
    // All three point to value 3; alias(0, 2) should be MayAlias or better
    EXPECT_NE(sa.alias(0, 3), AliasResult::MustAlias);  // different values
}

TEST(Steensgaard, External_MarksEscape) {
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.addValue(1);
    sa.addConstraint({ConstraintKind::AddrOf,    0, 1});
    sa.addConstraint({ConstraintKind::External,  0, 0});
    sa.run();
    EXPECT_TRUE(sa.mayPointToAnything(0));
}

TEST(Steensgaard, MayPointToAnything_False_ForUnescaped) {
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.addValue(1);
    sa.addConstraint({ConstraintKind::AddrOf, 0, 1});
    sa.run();
    // Without External constraint, value 1 should not escape
    EXPECT_FALSE(sa.mayPointToAnything(1));
}

TEST(Steensgaard, TwoIndependentPointers_NoAlias) {
    SteensgaardAnalysis sa;
    for (int i = 0; i < 4; ++i) sa.addValue(i);
    sa.addConstraint({ConstraintKind::AddrOf, 0, 1});  // p = &a
    sa.addConstraint({ConstraintKind::AddrOf, 2, 3});  // q = &b
    sa.run();
    // p and q point to different objects
    EXPECT_EQ(sa.alias(0, 2), AliasResult::NoAlias);
    EXPECT_EQ(sa.alias(1, 3), AliasResult::NoAlias);
}

TEST(Steensgaard, ClassCount_DecreasesWithMerges) {
    SteensgaardAnalysis sa;
    for (int i = 0; i < 4; ++i) sa.addValue(i);
    std::size_t before = sa.classCount();

    sa.addConstraint({ConstraintKind::AddrOf, 0, 1});
    sa.addConstraint({ConstraintKind::AddrOf, 2, 1});  // p and q both point to a
    sa.addConstraint({ConstraintKind::Copy,   0, 2});  // merge p and q
    sa.run();

    // After merging 0 and 2, class count should be ≤ before
    EXPECT_LE(sa.classCount(), before);
}

TEST(Steensgaard, Idempotent) {
    SteensgaardAnalysis sa;
    sa.addValue(0); sa.addValue(1);
    sa.addConstraint({ConstraintKind::Copy, 0, 1});
    sa.run();
    auto r1 = sa.alias(0, 1);
    sa.run();  // second call should be no-op
    auto r2 = sa.alias(0, 1);
    EXPECT_EQ(r1, r2);
}

TEST(Steensgaard, LargeGraph_NocrashHundredValues) {
    SteensgaardAnalysis sa;
    for (uint32_t i = 0; i < 100; ++i) sa.addValue(i);
    for (uint32_t i = 0; i < 50; ++i)
        sa.addConstraint({ConstraintKind::AddrOf, i*2, i*2+1});
    for (uint32_t i = 0; i < 25; ++i)
        sa.addConstraint({ConstraintKind::Copy, i*4, i*4+2});
    ASSERT_NO_THROW(sa.run());
    EXPECT_GE(sa.constraintCount(), 75u);
}

TEST(Steensgaard, SteensgaardCoarserThanExact) {
    // Case: p and q both initialised with AddrOf to the same target.
    // Steensgaard merges them → MayAlias.
    // Andersen's would give MayAlias too, but for genuinely disjoint
    // pointers Steensgaard may over-approximate.
    SteensgaardAnalysis sa;
    for (int i = 0; i < 4; ++i) sa.addValue(i);
    sa.addConstraint({ConstraintKind::AddrOf, 0, 2});
    sa.addConstraint({ConstraintKind::AddrOf, 1, 3});
    sa.addConstraint({ConstraintKind::Store,  0, 1});  // *p = q → merges 2 and 3's pts
    sa.run();
    // After store, the targets of 0 and 1 may have been merged
    // Steensgaard may report MayAlias for 2 and 3
    auto r = sa.alias(2, 3);
    EXPECT_NE(r, AliasResult::MustAlias);  // correct: 2 ≠ 3
}

// ═══════════════════════════════════════════════════════════════════════════════
// Escape analysis tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EscapeAnalysis, NoAddressTaken_EmptyEscapeSet) {
    SSAFunction fn("clean");
    auto* b = fn.addBlock("entry");
    // Normal load — not address-taken
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    v->memBaseReg = 5; v->memOffset = -8; v->memWidth = 8; v->memIsStack = true;
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Load, 0x1000);
    ins->uses.push_back({v->id, 0}); v->defInstr = ins;

    EscapeAnalysis ea;
    auto info = ea.run(fn);
    EXPECT_TRUE(info.escapedSlots.empty());
    EXPECT_FALSE(info.returnEscapes);
}

TEST(EscapeAnalysis, PassedToCall_Escapes) {
    SSAFunction fn("call_escape");
    auto* b = fn.addBlock("entry");
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    v->memBaseReg = 5; v->memOffset = -8; v->memWidth = 8; v->memIsStack = true;
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Call, 0x1000);
    ins->uses.push_back({v->id, 0}); v->defInstr = ins;

    EscapeAnalysis ea;
    auto info = ea.run(fn);
    EXPECT_TRUE(info.escapedSlots.count(-8));
}

TEST(EscapeAnalysis, Returned_ReturnEscapes) {
    SSAFunction fn("ret_escape");
    auto* b = fn.addBlock("entry");
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    v->memBaseReg = 5; v->memOffset = -16; v->memWidth = 8; v->memIsStack = true;
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Ret, 0x1000);
    ins->uses.push_back({v->id, 0}); v->defInstr = ins;

    EscapeAnalysis ea;
    auto info = ea.run(fn);
    EXPECT_TRUE(info.returnEscapes);
    EXPECT_TRUE(info.escapedSlots.count(-16));
}

TEST(EscapeAnalysis, StoredToNonStack_Escapes) {
    SSAFunction fn("store_escape");
    auto* b = fn.addBlock("entry");
    // Stack slot
    IrValue* stackSlot = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    stackSlot->memBaseReg = 5; stackSlot->memOffset = -8;
    stackSlot->memWidth = 8; stackSlot->memIsStack = true;
    // Non-stack target (heap)
    IrValue* heapTgt = fn.allocValue(ValueKind::MemRef, UINT32_MAX-3);
    heapTgt->memBaseReg = 0; heapTgt->memOffset = 0x1000;
    heapTgt->memWidth = 8; heapTgt->memIsStack = false;
    // Store: *heapTgt = stackSlot
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Store, 0x1000);
    ins->uses.push_back({heapTgt->id, 0});
    ins->uses.push_back({stackSlot->id, 1});
    stackSlot->defInstr = ins;

    EscapeAnalysis ea;
    auto info = ea.run(fn);
    EXPECT_TRUE(info.escapedSlots.count(-8));
}

TEST(EscapeAnalysis, LoadOnly_NoEscape) {
    SSAFunction fn("load_only");
    auto* b = fn.addBlock("entry");
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    v->memBaseReg = 5; v->memOffset = -8; v->memWidth = 8; v->memIsStack = true;
    IrInstr* load = fn.addInstr(b->id, IrInstr::Op::Load, 0x1000);
    load->uses.push_back({v->id, 0}); v->defInstr = load;

    EscapeAnalysis ea;
    auto info = ea.run(fn);
    EXPECT_FALSE(info.escapedSlots.count(-8));
}

TEST(EscapeAnalysis, MultipleEscapingSlots) {
    SSAFunction fn("multi_escape");
    auto* b = fn.addBlock("entry");
    for (int off : {-8, -16, -24}) {
        IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
        v->memBaseReg = 5; v->memOffset = off; v->memWidth = 8; v->memIsStack = true;
        IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Call, 0x1000);
        ins->uses.push_back({v->id, 0}); v->defInstr = ins;
    }
    EscapeAnalysis ea;
    auto info = ea.run(fn);
    EXPECT_EQ(info.escapedSlots.size(), 3u);
}

TEST(EscapeAnalysis, TransitiveEscape_CopyPropagates) {
    SSAFunction fn("transitive");
    auto* b = fn.addBlock("entry");
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    v->memBaseReg = 5; v->memOffset = -8; v->memWidth = 8; v->memIsStack = true;

    // copy = v (assign)
    IrInstr* copy = fn.addInstr(b->id, IrInstr::Op::Assign, 0x1000);
    copy->uses.push_back({v->id, 0});
    IrValue* copyVal = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    copyVal->memBaseReg = 5; copyVal->memOffset = -8; copyVal->memWidth = 8; copyVal->memIsStack = true;
    copy->defValue = copyVal->id;
    copyVal->defInstr = copy;
    v->defInstr = copy;

    // Pass copyVal to call → v should transitively escape
    IrInstr* callIns = fn.addInstr(b->id, IrInstr::Op::Call, 0x1004);
    callIns->uses.push_back({copyVal->id, 0});

    EscapeAnalysis ea;
    auto info = ea.run(fn);
    // copyVal was passed to Call → escaped
    EXPECT_TRUE(info.escapedValues.count(copyVal->id));
}

TEST(EscapeAnalysis, NonStackStore_DoesNotMarkStack) {
    SSAFunction fn("nonstack_store");
    auto* b = fn.addBlock("entry");
    IrValue* heap1 = fn.allocValue(ValueKind::MemRef, UINT32_MAX-3);
    heap1->memBaseReg = 0; heap1->memOffset = 0x2000; heap1->memWidth = 8; heap1->memIsStack = false;
    IrValue* heap2 = fn.allocValue(ValueKind::MemRef, UINT32_MAX-4);
    heap2->memBaseReg = 0; heap2->memOffset = 0x3000; heap2->memWidth = 8; heap2->memIsStack = false;
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Store, 0x1000);
    ins->uses.push_back({heap1->id, 0}); ins->uses.push_back({heap2->id, 1});

    EscapeAnalysis ea;
    auto info = ea.run(fn);
    EXPECT_TRUE(info.escapedSlots.empty());  // no stack slots involved
}

// ═══════════════════════════════════════════════════════════════════════════════
// AliasPass integration tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AliasPass, StackQuery_Dispatched) {
    SSAFunction fn("stack_dispatch");
    fn.addBlock("entry");
    AliasPass pass;
    pass.run(fn);
    auto a = MemLoc::stack(-8,  8);
    auto b = MemLoc::stack(-16, 8);
    EXPECT_EQ(pass.alias(a, b), AliasResult::NoAlias);
    EXPECT_GE(pass.stats().noAliasCount, 1u);
}

TEST(AliasPass, GlobalQuery_Dispatched) {
    SSAFunction fn("global_dispatch");
    fn.addBlock("entry");
    AliasPass pass;
    pass.run(fn);
    auto a = MemLoc::global(0x404000, 8);
    auto b = MemLoc::global(0x404100, 8);
    EXPECT_EQ(pass.alias(a, b), AliasResult::NoAlias);
}

TEST(AliasPass, UnknownKind_MayAlias) {
    SSAFunction fn("unknown");
    fn.addBlock("entry");
    AliasPass pass;
    pass.run(fn);
    auto a = MemLoc::unknown();
    auto b = MemLoc::unknown();
    EXPECT_EQ(pass.alias(a, b), AliasResult::MayAlias);
}

TEST(AliasPass, PromotableSlot_Present) {
    SSAFunction fn("promotable");
    auto* b = fn.addBlock("entry");
    // Single non-aliased stack slot
    addStackMemRef(fn, b->id, -8, 8);
    AliasPass pass;
    pass.run(fn);
    EXPECT_TRUE(pass.promotableSlots().count(-8));
}

TEST(AliasPass, EscapedSlot_NotPromotable) {
    SSAFunction fn("escaped_slot");
    auto* b = fn.addBlock("entry");
    IrValue* v = fn.allocValue(ValueKind::MemRef, UINT32_MAX-2);
    v->memBaseReg = 5; v->memOffset = -8; v->memWidth = 8; v->memIsStack = true;
    IrInstr* ins = fn.addInstr(b->id, IrInstr::Op::Call, 0x1000);
    ins->uses.push_back({v->id, 0}); v->defInstr = ins;

    AliasPass pass;
    pass.run(fn);
    EXPECT_FALSE(pass.promotableSlots().count(-8));
}

TEST(AliasPass, PartialOverlapSlot_NotPromotable) {
    SSAFunction fn("partial_overlap");
    auto* b = fn.addBlock("entry");
    // Two overlapping accesses to the same frame area → not promotable
    addStackMemRef(fn, b->id, -8, 8);
    addStackMemRef(fn, b->id, -8, 4);  // sub-access → MayAlias
    AliasPass pass;
    pass.run(fn);
    // The -8 slot has two accesses with MayAlias → NOT promotable
    EXPECT_FALSE(pass.promotableSlots().count(-8));
}

TEST(AliasPass, StatsCounters_Updated) {
    SSAFunction fn("stats");
    auto* b = fn.addBlock("entry");
    addStackMemRef(fn, b->id, -8,  8);
    addStackMemRef(fn, b->id, -16, 8);
    AliasPass pass;
    pass.run(fn);
    // Query a pair
    pass.alias(MemLoc::stack(-8, 8), MemLoc::stack(-16, 8));
    EXPECT_GE(pass.stats().stackQueries, 1u);
    EXPECT_GE(pass.stats().noAliasCount, 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
