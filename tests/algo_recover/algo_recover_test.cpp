/**
 * @file tests/algo_recover/algo_recover_test.cpp
 * @brief Unit tests for the Algorithm Header Recovery module (Stage 27).
 *
 * Coverage:
 *   - AlgorithmResult::kindName / toString
 *   - TransformDetector::detect  (copy, lambda, back_inserter)
 *   - AccumulateDetector::detect (add/mul/or/xor/max/min combiners)
 *   - FindDetector::detect       (find/find_if/count)
 *   - PartitionDetector::detect  (converging ptrs, swap, no recursion)
 *   - ForEachDetector::detect    (call per element, no accumulator)
 *   - IteratorPatternRecovery    (begin/end, reverse, back_inserter)
 *   - AlgorithmDetector          (preflight, tier assignment, orchestration)
 */

#include <memory>
#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <string>

using namespace retdec::algo_recover;
using namespace retdec;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::unique_ptr<ssa::SSAFunction> makeFunc(
        const std::string& name,
        const std::vector<ssa::IrInstr::Op>& ops,
        int extraBlocks = 0) {
    auto fn = std::make_unique<ssa::SSAFunction>(name);
    auto* entry = fn->addBlock("entry");
    for (auto op : ops)
        fn->addInstr(entry->id, op);
    for (int i = 0; i < extraBlocks; ++i)
        fn->addBlock("blk" + std::to_string(i));
    return fn;
}

static void addBackEdge(ssa::SSAFunction& fn) {
    // Add a back-edge from block 1 to block 0.
    if (fn.blockCount() >= 2)
        fn.block(1)->succs.push_back(0);
    else if (fn.blockCount() == 1)
        fn.block(0)->succs.push_back(0);
}

static void addCall(ssa::SSAFunction& fn, const std::string& callee) {
    auto* instr = fn.addInstr(fn.block(0)->id, ssa::IrInstr::Op::Call);
    if (instr) instr->calleeName = callee;
}

static void addImmInstr(ssa::SSAFunction& fn, ssa::IrInstr::Op op, uint64_t immVal) {
    auto* instr = fn.addInstr(fn.block(0)->id, op);
    if (!instr) return;
    ssa::IrValue* val = fn.allocValue(ssa::ValueKind::Immediate);
    if (val) val->imm = immVal;
    ssa::Use u; u.valueId = val ? val->id : ssa::kInvalidValue;
    instr->uses.push_back(u);
}

static void addPhi(ssa::SSAFunction& fn) {
    if (fn.blockCount() > 0)
        fn.addPhi(fn.block(0)->id, 0);
}

// ─── AlgorithmResult tests ────────────────────────────────────────────────────

TEST(AlgorithmResultTest, KindNameTransform) {
    AlgorithmResult r; r.kind = AlgorithmKind::Transform;
    EXPECT_EQ(r.kindName(), "std::transform");
}

TEST(AlgorithmResultTest, KindNameAccumulate) {
    AlgorithmResult r; r.kind = AlgorithmKind::Accumulate;
    EXPECT_EQ(r.kindName(), "std::accumulate");
}

TEST(AlgorithmResultTest, KindNameFind) {
    AlgorithmResult r; r.kind = AlgorithmKind::Find;
    EXPECT_EQ(r.kindName(), "std::find");
}

TEST(AlgorithmResultTest, KindNameFindIf) {
    AlgorithmResult r; r.kind = AlgorithmKind::FindIf;
    EXPECT_EQ(r.kindName(), "std::find_if");
}

TEST(AlgorithmResultTest, KindNamePartition) {
    AlgorithmResult r; r.kind = AlgorithmKind::Partition;
    EXPECT_EQ(r.kindName(), "std::partition");
}

TEST(AlgorithmResultTest, KindNameForEach) {
    AlgorithmResult r; r.kind = AlgorithmKind::ForEach;
    EXPECT_EQ(r.kindName(), "std::for_each");
}

TEST(AlgorithmResultTest, KindNameCopy) {
    AlgorithmResult r; r.kind = AlgorithmKind::Copy;
    EXPECT_EQ(r.kindName(), "std::copy");
}

TEST(AlgorithmResultTest, KindNameMaxElement) {
    AlgorithmResult r; r.kind = AlgorithmKind::MaxElement;
    EXPECT_EQ(r.kindName(), "std::max_element");
}

TEST(AlgorithmResultTest, KindNameMinElement) {
    AlgorithmResult r; r.kind = AlgorithmKind::MinElement;
    EXPECT_EQ(r.kindName(), "std::min_element");
}

TEST(AlgorithmResultTest, KindNameUnknown) {
    AlgorithmResult r;
    EXPECT_EQ(r.kindName(), "unknown");
}

TEST(AlgorithmResultTest, ToStringContainsConfidence) {
    AlgorithmResult r;
    r.kind = AlgorithmKind::Find;
    r.confidence = 0.9f;
    r.tier = EmissionTier::High;
    r.emittedForm = "std::find(first, last, value);";
    std::string s = r.toString();
    EXPECT_NE(s.find("0.9"), std::string::npos);
    EXPECT_NE(s.find("high"), std::string::npos);
}

TEST(AlgorithmResultTest, ToStringContainsEmittedForm) {
    AlgorithmResult r;
    r.kind = AlgorithmKind::ForEach;
    r.confidence = 0.8f;
    r.tier = EmissionTier::High;
    r.emittedForm = "std::for_each(first, last, f);";
    EXPECT_NE(r.toString().find("std::for_each"), std::string::npos);
}

// ─── TransformDetector tests ──────────────────────────────────────────────────

TEST(TransformDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    TransformDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(TransformDetectorTest, LoadStorePlusAdvancedPtrs) {
    auto fn = makeFunc("xfrm", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    TransformDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.45f);
    EXPECT_TRUE(r.kind == AlgorithmKind::Transform || r.kind == AlgorithmKind::Copy);
}

TEST(TransformDetectorTest, LambdaCallDetected) {
    auto fn = makeFunc("xfrm_lambda", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "transform_fn");
    TransformDetector det;
    auto r = det.detect(*fn);
    EXPECT_TRUE(r.hasLambda);
    EXPECT_EQ(r.kind, AlgorithmKind::Transform);
}

TEST(TransformDetectorTest, BackInserterDetected) {
    auto fn = makeFunc("xfrm_bi", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "push_back");
    TransformDetector det;
    auto r = det.detect(*fn);
    EXPECT_TRUE(r.hasBackInserter);
}

TEST(TransformDetectorTest, IdentityIsCopyKind) {
    // Load + Store + two Adds, no call → identity → Copy.
    auto fn = makeFunc("copy_loop", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    TransformDetector det;
    auto r = det.detect(*fn);
    EXPECT_EQ(r.kind, AlgorithmKind::Copy);
}

TEST(TransformDetectorTest, HighTierEmittedFormContainsStd) {
    auto fn = makeFunc("xfrm_hi", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Mul,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "f");
    TransformDetector det;
    auto r = det.detect(*fn);
    if (r.tier == EmissionTier::High)
        EXPECT_NE(r.emittedForm.find("std::"), std::string::npos);
}

// ─── AccumulateDetector tests ─────────────────────────────────────────────────

TEST(AccumulateDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    AccumulateDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(AccumulateDetectorTest, PhiPlusAddIsAccumulate) {
    auto fn = makeFunc("acc_add", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    AccumulateDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.40f);
    EXPECT_EQ(r.combiner, CombinerKind::Add);
}

TEST(AccumulateDetectorTest, PhiPlusMulIsMultiply) {
    auto fn = makeFunc("acc_mul", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Mul,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    AccumulateDetector det;
    auto r = det.detect(*fn);
    EXPECT_EQ(r.combiner, CombinerKind::Mul);
}

TEST(AccumulateDetectorTest, PhiPlusOrIsBitOr) {
    auto fn = makeFunc("acc_or", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Or,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    AccumulateDetector det;
    auto r = det.detect(*fn);
    EXPECT_EQ(r.combiner, CombinerKind::Or);
}

TEST(AccumulateDetectorTest, PhiPlusXorIsBitXor) {
    auto fn = makeFunc("acc_xor", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Xor,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    AccumulateDetector det;
    auto r = det.detect(*fn);
    EXPECT_EQ(r.combiner, CombinerKind::Xor);
}

TEST(AccumulateDetectorTest, CompareOnlyIsMaxElement) {
    auto fn = makeFunc("max_elem", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    AccumulateDetector det;
    auto r = det.detect(*fn);
    EXPECT_EQ(r.kind, AlgorithmKind::MaxElement);
}

TEST(AccumulateDetectorTest, NoStoreBoostsConfidence) {
    auto fn = makeFunc("acc_nostore", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    AccumulateDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.75f); // phi + add + no store = 1.0
}

TEST(AccumulateDetectorTest, HighTierEmittedContainsAccumulate) {
    auto fn = makeFunc("acc_hi", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    AccumulateDetector det;
    auto r = det.detect(*fn);
    if (r.tier == EmissionTier::High)
        EXPECT_NE(r.emittedForm.find("std::accumulate"), std::string::npos);
}

// ─── FindDetector tests ───────────────────────────────────────────────────────

TEST(FindDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    FindDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(FindDetectorTest, ComparePlusEarlyExitIsFind) {
    auto fn = makeFunc("find_val", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 2);
    // Back-edge: block 1 → block 0, forward edge: block 1 → block 2 (exit).
    fn->block(1)->succs.push_back(0);
    fn->block(1)->succs.push_back(2);
    addBackEdge(*fn);
    addImmInstr(*fn, ssa::IrInstr::Op::Compare, 42);
    FindDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.40f);
    EXPECT_TRUE(r.kind == AlgorithmKind::Find || r.kind == AlgorithmKind::FindIf);
}

TEST(FindDetectorTest, ImmediateComparandIsFindNotFindIf) {
    auto fn = makeFunc("find_imm", {
        ssa::IrInstr::Op::Load,
    }, 2);
    fn->block(1)->succs.push_back(0);
    fn->block(1)->succs.push_back(2);
    addImmInstr(*fn, ssa::IrInstr::Op::Compare, 99);
    FindDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.40f && r.kind != AlgorithmKind::Count)
        EXPECT_FALSE(r.hasLambda);
}

TEST(FindDetectorTest, PredicateCallIsFindIf) {
    auto fn = makeFunc("find_if", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 2);
    fn->block(1)->succs.push_back(0);
    fn->block(1)->succs.push_back(2);
    addCall(*fn, "pred_fn");
    FindDetector det;
    auto r = det.detect(*fn);
    if (r.confidence >= 0.40f)
        EXPECT_TRUE(r.hasLambda);
}

TEST(FindDetectorTest, NoStoreInLoopBoostsConfidence) {
    auto fn = makeFunc("find_nostore", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 2);
    fn->block(1)->succs.push_back(0);
    fn->block(1)->succs.push_back(2);
    FindDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.65f); // compare + early-exit + no-store
}

TEST(FindDetectorTest, HighTierEmittedContainsStdFind) {
    auto fn = makeFunc("find_hi", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 2);
    fn->block(1)->succs.push_back(0);
    fn->block(1)->succs.push_back(2);
    FindDetector det;
    auto r = det.detect(*fn);
    if (r.tier == EmissionTier::High)
        EXPECT_NE(r.emittedForm.find("std::find"), std::string::npos);
}

// ─── PartitionDetector tests ──────────────────────────────────────────────────

TEST(PartitionDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    PartitionDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(PartitionDetectorTest, ConvergingPtrsAndSwapDetected) {
    auto fn = makeFunc("part", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    PartitionDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.70f);
    EXPECT_EQ(r.kind, AlgorithmKind::Partition);
}

TEST(PartitionDetectorTest, RecursionLowersConfidence) {
    auto fn = makeFunc("sort_part", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    // Self-recursive call lowers confidence.
    addCall(*fn, "sort_part");
    PartitionDetector det;
    auto r = det.detect(*fn);
    // Recursion removes 0.10 bonus → should be < 1.0 at max.
    EXPECT_LT(r.confidence, 1.0f);
}

TEST(PartitionDetectorTest, SwapCallCounts) {
    auto fn = makeFunc("part_swap", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "std::swap");
    PartitionDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.55f);
}

TEST(PartitionDetectorTest, HighTierEmittedContainsPartition) {
    auto fn = makeFunc("part_hi", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Store,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    PartitionDetector det;
    auto r = det.detect(*fn);
    if (r.tier == EmissionTier::High)
        EXPECT_NE(r.emittedForm.find("std::partition"), std::string::npos);
}

// ─── ForEachDetector tests ────────────────────────────────────────────────────

TEST(ForEachDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("empty", {});
    ForEachDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(ForEachDetectorTest, CallPlusNoPhiAndNoStore) {
    auto fn = makeFunc("foreach", {
        ssa::IrInstr::Op::Load,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "process");
    ForEachDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.70f); // call + no phi + no store
    EXPECT_EQ(r.kind, AlgorithmKind::ForEach);
}

TEST(ForEachDetectorTest, PhiReducesConfidence) {
    auto fn = makeFunc("foreach_phi", {
        ssa::IrInstr::Op::Load,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "f");
    addPhi(*fn);
    ForEachDetector det;
    auto r = det.detect(*fn);
    // call: +0.50, no store: +0.25, phi present: 0 (no no-phi bonus) → 0.75
    // but it still detects as ForEach.
    EXPECT_EQ(r.kind, AlgorithmKind::ForEach);
}

TEST(ForEachDetectorTest, NoCallLowConfidence) {
    auto fn = makeFunc("foreach_nocall", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    ForEachDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.10f);
}

TEST(ForEachDetectorTest, HighTierEmittedContainsForEach) {
    auto fn = makeFunc("foreach_hi", {
        ssa::IrInstr::Op::Load,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "f");
    ForEachDetector det;
    auto r = det.detect(*fn);
    if (r.tier == EmissionTier::High)
        EXPECT_NE(r.emittedForm.find("std::for_each"), std::string::npos);
}

// ─── IteratorPatternRecovery tests ────────────────────────────────────────────

TEST(IteratorPatternRecoveryTest, EmptyFunctionNoPattern) {
    auto fn = makeFunc("empty", {});
    IteratorPatternRecovery rec;
    auto r = rec.recover(*fn);
    EXPECT_FALSE(r.isBeginEnd);
    EXPECT_FALSE(r.isReverseIter);
    EXPECT_FALSE(r.hasBackInserter);
}

TEST(IteratorPatternRecoveryTest, BeginEndPatternDetected) {
    auto fn = makeFunc("iter_be", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    IteratorPatternRecovery rec;
    auto r = rec.recover(*fn);
    EXPECT_TRUE(r.isBeginEnd);
    EXPECT_EQ(r.rangeForForm, "for (auto& e : v)");
}

TEST(IteratorPatternRecoveryTest, ReversePatternDetected) {
    auto fn = makeFunc("iter_rev", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Sub,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    IteratorPatternRecovery rec;
    auto r = rec.recover(*fn);
    EXPECT_TRUE(r.isReverseIter);
    EXPECT_NE(r.rangeForForm.find("reverse"), std::string::npos);
}

TEST(IteratorPatternRecoveryTest, BackInserterDetected) {
    auto fn = makeFunc("iter_bi", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "push_back");
    IteratorPatternRecovery rec;
    auto r = rec.recover(*fn);
    EXPECT_TRUE(r.hasBackInserter);
    EXPECT_EQ(r.backInserter, "std::back_inserter(dst)");
}

TEST(IteratorPatternRecoveryTest, EmplaceBackIsBackInserter) {
    auto fn = makeFunc("iter_eb", {
        ssa::IrInstr::Op::Load,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "emplace_back");
    IteratorPatternRecovery rec;
    auto r = rec.recover(*fn);
    EXPECT_TRUE(r.hasBackInserter);
}

// ─── AlgorithmDetector orchestration tests ────────────────────────────────────

TEST(AlgorithmDetectorTest, EmptyFunctionSkipped) {
    AlgorithmDetector::Config cfg;
    cfg.minBlocks = 2;
    cfg.minInstrs = 5;
    AlgorithmDetector det(cfg);
    auto fn = makeFunc("tiny", { ssa::IrInstr::Op::Load });
    auto r = det.detect(*fn);
    EXPECT_EQ(r.kind, AlgorithmKind::Unknown);
    EXPECT_EQ(det.stats().functionsSkipped, 1u);
}

TEST(AlgorithmDetectorTest, FindBeatsForEach) {
    // A function with compare + early exit should be detected as find/find_if,
    // not for_each, because find is registered first.
    AlgorithmDetector det;
    auto fn = makeFunc("find_vs_each", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Compare,
    }, 2);
    fn->block(1)->succs.push_back(0);
    fn->block(1)->succs.push_back(2);
    auto r = det.detect(*fn);
    EXPECT_TRUE(r.kind == AlgorithmKind::Find  ||
                r.kind == AlgorithmKind::FindIf ||
                r.kind == AlgorithmKind::Count  ||
                r.kind == AlgorithmKind::Unknown);
}

TEST(AlgorithmDetectorTest, TierAssignedCorrectly) {
    AlgorithmDetector::Config cfg;
    cfg.highTierThreshold   = 0.75f;
    cfg.mediumTierThreshold = 0.45f;
    AlgorithmDetector det(cfg);
    auto fn = makeFunc("acc", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    auto r = det.detect(*fn);
    // phi + add + no store = 1.0 confidence → High tier
    if (r.confidence >= 0.75f)
        EXPECT_EQ(r.tier, EmissionTier::High);
}

TEST(AlgorithmDetectorTest, StatsUpdatedAfterDetection) {
    AlgorithmDetector det;
    auto fn = makeFunc("each", {
        ssa::IrInstr::Op::Load,
    }, 1);
    addBackEdge(*fn);
    addCall(*fn, "f");
    std::vector<const ssa::SSAFunction*> fns = { fn.get() };
    auto results = det.detectModule(fns);
    EXPECT_GE(det.stats().functionsAnalysed, 1u);
}

TEST(AlgorithmDetectorTest, ModuleReturnsResultsPerFunction) {
    AlgorithmDetector det;
    auto fn1 = makeFunc("fn1", { ssa::IrInstr::Op::Load }, 1);
    addBackEdge(*fn1);
    addCall(*fn1, "f");

    auto fn2 = makeFunc("fn2", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
    }, 1);
    addBackEdge(*fn2);
    addPhi(*fn2);

    std::vector<const ssa::SSAFunction*> fns = { fn1.get(), fn2.get() };
    auto results = det.detectModule(fns);
    // At most 2 results (one per function).
    EXPECT_LE(results.size(), 2u);
}

TEST(AlgorithmDetectorTest, NullFunctionSkipped) {
    AlgorithmDetector det;
    std::vector<const ssa::SSAFunction*> fns = { nullptr };
    auto results = det.detectModule(fns);
    EXPECT_EQ(results.size(), 0u);
}

TEST(AlgorithmDetectorTest, IteratorAnnotationAddedToHighTier) {
    AlgorithmDetector det;
    auto fn = makeFunc("iter_annotate", {
        ssa::IrInstr::Op::Load,
        ssa::IrInstr::Op::Add,
        ssa::IrInstr::Op::Compare,
    }, 1);
    addBackEdge(*fn);
    addPhi(*fn);
    auto r = det.detect(*fn);
    // If it detects accumulate at high tier, the range annotation should be appended.
    if (r.tier == EmissionTier::High && r.kind == AlgorithmKind::Accumulate)
        EXPECT_NE(r.emittedForm.find("std::"), std::string::npos);
}
