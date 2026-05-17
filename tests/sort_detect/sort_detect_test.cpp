/**
 * @file tests/sort_detect/sort_detect_test.cpp
 * @brief Unit tests for the sort detection module (Stage 25).
 *
 * Coverage:
 *   - ElementType::toString
 *   - SortResult::algorithmName / toString
 *   - PartitionFingerprint::analyse
 *   - SiftDownFingerprint::analyse
 *   - RecursiveHalvingFingerprint::analyse
 *   - InsertionSortFingerprint::analyse
 *   - IntrosortDetector::detect
 *   - MergesortDetector::detect
 *   - HeapsortDetector::detect
 *   - RadixsortDetector::detect
 *   - InsertionSortDetector::detect
 *   - ElementTypeRecoverer::recover
 *   - SortDetector::analyseFunction / analyseModule
 */

#include <memory>
#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>

using namespace retdec::sort_detect;
using namespace retdec;

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Build a trivial SSA function with a given set of opcodes inserted once each
// into the entry block.
static std::unique_ptr<ssa::SSAFunction> makeFunc(
        const std::string& name,
        const std::vector<ssa::IrInstr::Op>& ops,
        int extraBlocks = 0) {
    auto fn = std::make_unique<ssa::SSAFunction>(name);
    auto* entry = fn->addBlock("entry");
    for (auto op : ops) {
        fn->addInstr(entry->id, op);
    }
    for (int i = 0; i < extraBlocks; ++i) {
        fn->addBlock("blk" + std::to_string(i));
    }
    return fn;
}

// Build a function that has a phi node.
static void addPhi(ssa::SSAFunction& fn, ssa::VarId var = 0) {
    if (fn.blockCount() < 1) return;
    fn.addPhi(fn.block(0)->id, var);
}

// ─── ElementType tests ────────────────────────────────────────────────────────

TEST(ElementTypeTest, UnknownToString) {
    ElementType et;
    EXPECT_EQ(et.toString(), "unknown");
}

TEST(ElementTypeTest, Int32ToString) {
    ElementType et;
    et.kind = ElementType::Kind::Int32;
    EXPECT_EQ(et.toString(), "int32_t");
}

TEST(ElementTypeTest, UInt8ToString) {
    ElementType et;
    et.kind = ElementType::Kind::UInt8;
    EXPECT_EQ(et.toString(), "uint8_t");
}

TEST(ElementTypeTest, FloatToString) {
    ElementType et;
    et.kind = ElementType::Kind::Float;
    EXPECT_EQ(et.toString(), "float");
}

TEST(ElementTypeTest, StructWithName) {
    ElementType et;
    et.kind = ElementType::Kind::Struct;
    et.name = "MyRecord";
    EXPECT_EQ(et.toString(), "struct MyRecord");
}

// ─── SortResult tests ─────────────────────────────────────────────────────────

TEST(SortResultTest, AlgorithmNameIntrosort) {
    SortResult r;
    r.algorithm = SortAlgorithm::Introsort;
    EXPECT_NE(r.algorithmName().find("introsort"), std::string::npos);
}

TEST(SortResultTest, AlgorithmNameMergesort) {
    SortResult r;
    r.algorithm = SortAlgorithm::Mergesort;
    EXPECT_NE(r.algorithmName().find("mergesort"), std::string::npos);
}

TEST(SortResultTest, AlgorithmNameRadix) {
    SortResult r;
    r.algorithm = SortAlgorithm::Radixsort;
    EXPECT_NE(r.algorithmName().find("radix"), std::string::npos);
}

TEST(SortResultTest, ToStringContainsConfidence) {
    SortResult r;
    r.algorithm  = SortAlgorithm::Heapsort;
    r.confidence = 0.75f;
    std::string s = r.toString();
    EXPECT_NE(s.find("0.75"), std::string::npos);
}

TEST(SortResultTest, ToStringContainsCompilerGCC) {
    SortResult r;
    r.algorithm      = SortAlgorithm::Introsort;
    r.confidence     = 0.9f;
    r.compilerVariant = CompilerVariant::GCC;
    EXPECT_NE(r.toString().find("GCC"), std::string::npos);
}

TEST(SortResultTest, UnknownAlgorithmName) {
    SortResult r;
    EXPECT_EQ(r.algorithmName(), "unknown");
}

// ─── PartitionFingerprint tests ───────────────────────────────────────────────

TEST(PartitionFingerprintTest, EmptyFunctionNoPartition) {
    auto fn = makeFunc("empty", {});
    PartitionFingerprint pf;
    auto ev = pf.analyse(*fn);
    EXPECT_FALSE(ev.found);
    EXPECT_LT(ev.confidence, 0.30f);
}

TEST(PartitionFingerprintTest, CompareAloneGivesPartialScore) {
    auto fn = makeFunc("cmp_only", {ssa::IrInstr::Op::Compare});
    PartitionFingerprint pf;
    auto ev = pf.analyse(*fn);
    // Compare contributes +0.30 to score, threshold is 0.30.
    EXPECT_GE(ev.confidence, 0.29f);
}

TEST(PartitionFingerprintTest, FullPartitionPattern) {
    // Compare + 2 Stores + Add + Sub + CondBranch × 2 + extra blocks for phi.
    auto fn = makeFunc("partition",
        {ssa::IrInstr::Op::Compare,
         ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store,
         ssa::IrInstr::Op::Add, ssa::IrInstr::Op::Sub,
         ssa::IrInstr::Op::CondBranch, ssa::IrInstr::Op::CondBranch},
        /*extraBlocks=*/3);
    addPhi(*fn, 0);
    addPhi(*fn, 1);
    PartitionFingerprint pf;
    auto ev = pf.analyse(*fn);
    EXPECT_TRUE(ev.found);
    EXPECT_GE(ev.confidence, 0.50f);
}

// ─── SiftDownFingerprint tests ────────────────────────────────────────────────

TEST(SiftDownFingerprintTest, EmptyFunctionNoSiftDown) {
    auto fn = makeFunc("empty", {});
    SiftDownFingerprint sdf;
    auto ev = sdf.analyse(*fn);
    EXPECT_FALSE(ev.found);
}

TEST(SiftDownFingerprintTest, ShlOnePatternDetected) {
    // Shl(x, 1) is the 2*x child index arithmetic.
    auto fn = std::make_unique<ssa::SSAFunction>("sift");
    auto* blk = fn->addBlock("entry");
    auto* shl = fn->addInstr(blk->id, ssa::IrInstr::Op::Shl);
    // Attach a constant 1 as the shift amount.
    auto* immVal = fn->allocValue(ssa::ValueKind::Immediate);
    immVal->imm = 1;
    ssa::Use shiftUse;
    shiftUse.valueId = immVal->id;
    shiftUse.operandIndex = 1;
    shl->uses.push_back(shiftUse);

    SiftDownFingerprint sdf;
    auto ev = sdf.analyse(*fn);
    EXPECT_TRUE(ev.hasLeftArith);
}

TEST(SiftDownFingerprintTest, FullSiftDownSignature) {
    // Shl + Compare + CondBranch + 2 Stores.
    auto fn = makeFunc("sift_down",
        {ssa::IrInstr::Op::Shl,
         ssa::IrInstr::Op::Compare,
         ssa::IrInstr::Op::CondBranch,
         ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store},
        2);
    // Attach const 1 to the Shl.
    auto* shlInstr = fn->block(fn->entryId())->instrs[0];
    auto* imm1 = fn->allocValue(ssa::ValueKind::Immediate);
    imm1->imm = 1;
    ssa::Use u; u.valueId = imm1->id; u.operandIndex = 1;
    shlInstr->uses.push_back(u);

    SiftDownFingerprint sdf;
    auto ev = sdf.analyse(*fn);
    EXPECT_TRUE(ev.found);
    EXPECT_GE(ev.confidence, 0.6f);
}

// ─── RecursiveHalvingFingerprint tests ────────────────────────────────────────

TEST(RecursiveHalvingFingerprintTest, NoSelfCallsNoHalving) {
    auto fn = makeFunc("foo", {ssa::IrInstr::Op::Add, ssa::IrInstr::Op::Ret});
    RecursiveHalvingFingerprint rhf;
    auto ev = rhf.analyse(*fn);
    EXPECT_EQ(ev.selfCallCount, 0);
    EXPECT_FALSE(ev.found);
}

TEST(RecursiveHalvingFingerprintTest, TwoSelfCallsFound) {
    auto fn = std::make_unique<ssa::SSAFunction>("merge_sort");
    auto* blk = fn->addBlock("entry");
    // Add two self-calls.
    auto* c1 = fn->addInstr(blk->id, ssa::IrInstr::Op::Call);
    c1->calleeName = "merge_sort";
    auto* c2 = fn->addInstr(blk->id, ssa::IrInstr::Op::Call);
    c2->calleeName = "merge_sort";
    fn->addInstr(blk->id, ssa::IrInstr::Op::Add);
    fn->addInstr(blk->id, ssa::IrInstr::Op::Shr);

    RecursiveHalvingFingerprint rhf;
    auto ev = rhf.analyse(*fn);
    EXPECT_EQ(ev.selfCallCount, 2);
    EXPECT_TRUE(ev.found);
    EXPECT_TRUE(ev.halvingConfirmed);
}

// ─── InsertionSortFingerprint tests ──────────────────────────────────────────

TEST(InsertionSortFingerprintTest, EmptyFunctionNoInsertion) {
    auto fn = makeFunc("empty", {});
    InsertionSortFingerprint isf;
    auto ev = isf.analyse(*fn);
    EXPECT_FALSE(ev.found);
}

TEST(InsertionSortFingerprintTest, BasicInsertionPattern) {
    // Sub (decrement) + Compare + Store + extra blocks.
    auto fn = makeFunc("insertion",
        {ssa::IrInstr::Op::Sub,
         ssa::IrInstr::Op::Compare,
         ssa::IrInstr::Op::Store,
         ssa::IrInstr::Op::CondBranch},
        3);
    InsertionSortFingerprint isf;
    auto ev = isf.analyse(*fn);
    EXPECT_TRUE(ev.found);
    EXPECT_GE(ev.confidence, 0.4f);
}

TEST(InsertionSortFingerprintTest, ThresholdGuard16) {
    auto fn = std::make_unique<ssa::SSAFunction>("insertion_sort");
    auto* entry = fn->addBlock("entry");
    fn->addBlock("loop");
    fn->addBlock("exit");

    // Add instructions to entry block.
    fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
    auto* cmpI = fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Store);

    // Attach immediate 16 as a Compare operand.
    auto* imm16 = fn->allocValue(ssa::ValueKind::Immediate);
    imm16->imm = 16;
    ssa::Use u; u.valueId = imm16->id; u.operandIndex = 0;
    cmpI->uses.push_back(u);

    InsertionSortFingerprint isf;
    auto ev = isf.analyse(*fn);
    EXPECT_TRUE(ev.found);
    EXPECT_TRUE(ev.hasThresholdGuard);
    EXPECT_EQ(ev.threshold, 16);
    EXPECT_GE(ev.confidence, 0.7f);
}

// ─── IntrosortDetector tests ──────────────────────────────────────────────────

TEST(IntrosortDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("fn", {});
    IntrosortDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.45f);
}

TEST(IntrosortDetectorTest, FullIntrosortPattern) {
    // Compare + 2 Stores + Add + Sub × 2 + CondBranch × 2 + self-calls × 2 + phi × 2
    auto fn = std::make_unique<ssa::SSAFunction>("__sort");
    auto* entry = fn->addBlock("entry");
    fn->addBlock("loop");
    fn->addBlock("rec");
    fn->addBlock("ins");
    fn->addBlock("exit");

    fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Add);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    auto* c1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c1->calleeName = "__sort";
    auto* c2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c2->calleeName = "__sort";

    // Add a Compare against 0 (depth counter check).
    auto* cmpZero = fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    auto* imm0 = fn->allocValue(ssa::ValueKind::Immediate);
    imm0->imm = 0;
    ssa::Use u; u.valueId = imm0->id;
    cmpZero->uses.push_back(u);

    fn->addPhi(entry->id, 0);
    fn->addPhi(entry->id, 1);

    IntrosortDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.45f);
    EXPECT_EQ(r.algorithm, SortAlgorithm::Introsort);
    EXPECT_EQ(r.compilerVariant, CompilerVariant::GCC);
}

// ─── MergesortDetector tests ──────────────────────────────────────────────────

TEST(MergesortDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("fn", {});
    MergesortDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.40f);
}

TEST(MergesortDetectorTest, TwoRecursiveCallsBoostScore) {
    auto fn = std::make_unique<ssa::SSAFunction>("stable_sort");
    auto* entry = fn->addBlock("entry");
    fn->addBlock("merge_blk");
    fn->addBlock("exit");

    auto* c1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c1->calleeName = "stable_sort";
    auto* c2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c2->calleeName = "stable_sort";

    // malloc call for aux buffer.
    auto* mallocI = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
    mallocI->calleeName = "malloc";

    // Merge loop: 2 loads + compare + 2 cond branches.
    fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Load);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Add);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Shr);

    MergesortDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.60f);
    EXPECT_EQ(r.algorithm, SortAlgorithm::Mergesort);
}

// ─── HeapsortDetector tests ───────────────────────────────────────────────────

TEST(HeapsortDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("fn", {});
    HeapsortDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.45f);
}

TEST(HeapsortDetectorTest, FullHeapsortPattern) {
    // Shl(x,1) + Compare + CondBranch + 2×Sub + 2×Store.
    auto fn = std::make_unique<ssa::SSAFunction>("sort_heap");
    auto* entry = fn->addBlock("entry");
    fn->addBlock("build");
    fn->addBlock("sort");

    auto* shl = fn->addInstr(entry->id, ssa::IrInstr::Op::Shl);
    auto* imm1 = fn->allocValue(ssa::ValueKind::Immediate);
    imm1->imm = 1;
    ssa::Use u; u.valueId = imm1->id; u.operandIndex = 1;
    shl->uses.push_back(u);

    fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Store);

    HeapsortDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.50f);
    EXPECT_EQ(r.algorithm, SortAlgorithm::Heapsort);
}

// ─── RadixsortDetector tests ──────────────────────────────────────────────────

TEST(RadixsortDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("fn", {});
    RadixsortDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.45f);
}

TEST(RadixsortDetectorTest, ZeroComparisonsBoostScore) {
    // No Compare instructions → radix signal.
    auto fn = makeFunc("radix_sort",
        {ssa::IrInstr::Op::Shr, ssa::IrInstr::Op::And,
         ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Load,
         ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Load,
         ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store,
         ssa::IrInstr::Op::Add, ssa::IrInstr::Op::Add},
        2);
    // Attach constant shift and mask.
    auto* blk = fn->block(fn->entryId());
    // Shr with constant multiple of 8.
    auto* shrI = blk->instrs[0];
    auto* imm8 = fn->allocValue(ssa::ValueKind::Immediate);
    imm8->imm = 8;
    ssa::Use ush; ush.valueId = imm8->id; ush.operandIndex = 1;
    shrI->uses.push_back(ush);
    // And with mask 0xff.
    auto* andI = blk->instrs[1];
    auto* immMask = fn->allocValue(ssa::ValueKind::Immediate);
    immMask->imm = 0xff;
    ssa::Use uand; uand.valueId = immMask->id; uand.operandIndex = 1;
    andI->uses.push_back(uand);
    // Add phi for prefix-sum loop.
    fn->addPhi(blk->id, 0);

    RadixsortDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.50f);
    EXPECT_EQ(r.algorithm, SortAlgorithm::Radixsort);
}

TEST(RadixsortDetectorTest, WithComparisonsLowScore) {
    // Has Compare → not a strong radix signal.
    auto fn = makeFunc("cmp_sort",
        {ssa::IrInstr::Op::Compare, ssa::IrInstr::Op::CondBranch,
         ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Store});
    RadixsortDetector det;
    auto r = det.detect(*fn);
    // Compare in a block that also has Load → countElementComparisons > 0.
    EXPECT_LT(r.confidence, 0.65f);
}

// ─── InsertionSortDetector tests ──────────────────────────────────────────────

TEST(InsertionSortDetectorTest, EmptyFunctionLowConfidence) {
    auto fn = makeFunc("fn", {});
    InsertionSortDetector det;
    auto r = det.detect(*fn);
    EXPECT_LT(r.confidence, 0.40f);
}

TEST(InsertionSortDetectorTest, BasicPattern) {
    auto fn = makeFunc("insertion",
        {ssa::IrInstr::Op::Sub, ssa::IrInstr::Op::Compare,
         ssa::IrInstr::Op::Store, ssa::IrInstr::Op::CondBranch},
        3);
    InsertionSortDetector det;
    auto r = det.detect(*fn);
    EXPECT_GE(r.confidence, 0.40f);
    EXPECT_EQ(r.algorithm, SortAlgorithm::InsertionSort);
}

// ─── ElementTypeRecoverer tests ───────────────────────────────────────────────

TEST(ElementTypeRecovererTest, NoCompareReturnsUnknown) {
    auto fn = makeFunc("fn", {ssa::IrInstr::Op::Add});
    ElementTypeRecoverer rec;
    SortResult dummy;
    auto et = rec.recover(*fn, dummy);
    EXPECT_EQ(et.kind, ElementType::Kind::Unknown);
}

TEST(ElementTypeRecovererTest, CompareWithLoadOperandReturnsType) {
    auto fn = std::make_unique<ssa::SSAFunction>("sort_fn");
    auto* blk = fn->addBlock("entry");

    // Create a Load instruction whose result is used by a Compare.
    auto* loadI = fn->addInstr(blk->id, ssa::IrInstr::Op::Load);
    auto* loadVal = fn->allocValue(ssa::ValueKind::VirtualReg);
    loadVal->width = 32;
    loadVal->defInstr = loadI;
    loadI->defValue = loadVal->id;

    auto* cmpI = fn->addInstr(blk->id, ssa::IrInstr::Op::Compare);
    ssa::Use u; u.valueId = loadVal->id; u.operandIndex = 0;
    cmpI->uses.push_back(u);

    ElementTypeRecoverer rec;
    SortResult dummy;
    auto et = rec.recover(*fn, dummy);
    EXPECT_EQ(et.kind, ElementType::Kind::Int32);
    EXPECT_EQ(et.byteWidth, 4);
}

// ─── SortDetector orchestration tests ────────────────────────────────────────

TEST(SortDetectorTest, EmptyModuleNoDetections) {
    SortDetector det;
    auto res = det.analyseModule({});
    EXPECT_TRUE(res.empty());
    EXPECT_EQ(det.stats().functionsAnalysed, 0u);
}

TEST(SortDetectorTest, TrivialFunctionSkipped) {
    SortDetector det;  // default cfg: minBlocks=3, minInstrs=15
    auto fn = makeFunc("tiny", {ssa::IrInstr::Op::Ret});
    det.analyseFunction(*fn);
    EXPECT_GT(det.stats().functionsSkipped, 0u);
}

TEST(SortDetectorTest, StrongIntrosortSignalDetected) {
    // Build a function with a strong introsort-like signature.
    auto fn = std::make_unique<ssa::SSAFunction>("__introsort_loop");
    auto* entry = fn->addBlock("entry");
    fn->addBlock("blk1");
    fn->addBlock("blk2");
    fn->addBlock("blk3");
    fn->addBlock("blk4");

    fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Store);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Add);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
    fn->addInstr(entry->id, ssa::IrInstr::Op::Sub);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    fn->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    auto* c1 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c1->calleeName = "__introsort_loop";
    auto* c2 = fn->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c2->calleeName = "__introsort_loop";
    auto* cz = fn->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    auto* imm0 = fn->allocValue(ssa::ValueKind::Immediate);
    imm0->imm = 0;
    ssa::Use uz; uz.valueId = imm0->id;
    cz->uses.push_back(uz);
    fn->addPhi(entry->id, 0);
    fn->addPhi(entry->id, 1);

    // More instructions to pass minInstrs=15.
    for (int i = 0; i < 4; ++i)
        fn->addInstr(entry->id, ssa::IrInstr::Op::Add);

    SortDetector det;
    auto r = det.analyseFunction(*fn);
    EXPECT_EQ(r.algorithm, SortAlgorithm::Introsort);
    EXPECT_GE(r.confidence, SortDetector::Config{}.minConfidence);
}

TEST(SortDetectorTest, ModuleAnalysisMultipleFunctions) {
    SortDetector det;
    // A very small function (will be skipped) + a larger one.
    auto tiny = makeFunc("tiny", {ssa::IrInstr::Op::Ret});

    auto merge = std::make_unique<ssa::SSAFunction>("stable_sort");
    auto* entry = merge->addBlock("entry");
    for (int i = 0; i < 3; ++i) merge->addBlock("b" + std::to_string(i));
    auto* c1 = merge->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c1->calleeName = "stable_sort";
    auto* c2 = merge->addInstr(entry->id, ssa::IrInstr::Op::Call);
    c2->calleeName = "stable_sort";
    auto* mallocI = merge->addInstr(entry->id, ssa::IrInstr::Op::Call);
    mallocI->calleeName = "malloc";
    merge->addInstr(entry->id, ssa::IrInstr::Op::Load);
    merge->addInstr(entry->id, ssa::IrInstr::Op::Load);
    merge->addInstr(entry->id, ssa::IrInstr::Op::Compare);
    merge->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    merge->addInstr(entry->id, ssa::IrInstr::Op::CondBranch);
    merge->addInstr(entry->id, ssa::IrInstr::Op::Add);
    merge->addInstr(entry->id, ssa::IrInstr::Op::Shr);
    // Add 5 more instructions to reach minInstrs=15.
    for (int i = 0; i < 5; ++i)
        merge->addInstr(entry->id, ssa::IrInstr::Op::Add);

    std::vector<const ssa::SSAFunction*> fns = {tiny.get(), merge.get()};
    auto results = det.analyseModule(fns);

    // The "tiny" function should be skipped (not in results or low confidence).
    EXPECT_FALSE(results.count("tiny") && results.at("tiny").algorithm != SortAlgorithm::Unknown);
}

TEST(SortDetectorTest, StatsTracking) {
    SortDetector det;
    auto fn1 = makeFunc("f1", {ssa::IrInstr::Op::Ret});
    auto fn2 = makeFunc("f2", {ssa::IrInstr::Op::Ret});
    det.analyseFunction(*fn1);
    det.analyseFunction(*fn2);
    EXPECT_EQ(det.stats().functionsAnalysed, 2u);
}

TEST(SortDetectorTest, CustomMinConfidence) {
    SortDetector::Config cfg;
    cfg.minConfidence = 0.99f; // Very high threshold — nothing should pass.
    SortDetector det(cfg);

    auto fn = makeFunc("sort_fn",
        {ssa::IrInstr::Op::Compare, ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Add,
         ssa::IrInstr::Op::Sub, ssa::IrInstr::Op::CondBranch,
         ssa::IrInstr::Op::CondBranch, ssa::IrInstr::Op::CondBranch,
         ssa::IrInstr::Op::Load, ssa::IrInstr::Op::Load,
         ssa::IrInstr::Op::Store, ssa::IrInstr::Op::Store},
        5);
    fn->addPhi(fn->block(fn->entryId())->id, 0);
    auto r = det.analyseFunction(*fn);
    EXPECT_EQ(r.algorithm, SortAlgorithm::Unknown);
}
