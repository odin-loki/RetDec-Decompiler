/**
 * @file tests/code_data/code_data_classifier_test.cpp
 * @brief Unit tests for CodeDataClassifier.
 *
 * Test organisation:
 *   - Prior / default behaviour
 *   - Each evidence source in isolation
 *   - Combined evidence (multiple sources interact correctly)
 *   - ARM Thumb interworking
 *   - Result compaction (contiguous runs)
 *   - Statistics
 *   - Edge cases
 */

#include "retdec/code_data/code_data_classifier.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace retdec::code_data;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Return a classifier seeded with a tiny dummy image (all zeros).
static CodeDataClassifier makeX86()
{
    static const uint8_t kZero[256] = {};
    return CodeDataClassifier(Arch::X86_64, 0x400000, kZero, sizeof(kZero));
}

static CodeDataClassifier makeARM()
{
    static const uint8_t kZero[256] = {};
    return CodeDataClassifier(Arch::ARM, 0x8000, kZero, sizeof(kZero));
}

// ─── Prior / default ─────────────────────────────────────────────────────────

TEST(Prior, UnclassifiedAddressReturnsData)
{
    auto clf = makeX86();
    clf.classify();
    // No evidence → posterior 0.5 (prior for unreachable) → ambiguous, but
    // the address was never inserted so labelAt returns Data.
    EXPECT_EQ(clf.labelAt(0xDEADBEEF), Label::Data);
}

TEST(Prior, DefaultPosteriorIsHalf)
{
    auto clf = makeX86();
    clf.addExecutableRange(0x400000, 0x400004);
    clf.classify();
    // No other evidence → posterior should be ~0.5 → Ambiguous.
    double p = clf.posteriorAt(0x400000);
    EXPECT_NEAR(p, 0.5, 0.01);
    EXPECT_EQ(clf.labelAt(0x400000), Label::Ambiguous);
}

// ─── Evidence 1: Reachability ─────────────────────────────────────────────────

TEST(Reachability, EntryPointIsCode)
{
    auto clf = makeX86();
    clf.addEntryPoint(0x401000);
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x401000), Label::Code);
    EXPECT_GT(clf.posteriorAt(0x401000), 0.9);
}

TEST(Reachability, ReachableRangeIsCode)
{
    auto clf = makeX86();
    clf.addReachableRange(0x401000, 16);
    clf.classify();
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(clf.labelAt(0x401000 + i), Label::Code)
            << "byte " << i << " should be code";
    }
}

TEST(Reachability, MultipleEntryPoints)
{
    auto clf = makeX86();
    clf.addEntryPoint(0x401000);
    clf.addEntryPoint(0x402000);
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x401000), Label::Code);
    EXPECT_EQ(clf.labelAt(0x402000), Label::Code);
}

// ─── Evidence 2: Instruction validity ────────────────────────────────────────

TEST(InstrValidity, ValidInstrIsCode)
{
    auto clf = makeX86();
    clf.addValidInstruction(0x401000, 3);
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x401000), Label::Code);
}

TEST(InstrValidity, InternalBytesOfValidInstrAreCode)
{
    auto clf = makeX86();
    clf.addValidInstruction(0x401000, 5);
    clf.classify();
    // Bytes 1-4 should still be code (interior bytes get 0.5× LLR).
    for (int i = 1; i < 5; ++i) {
        EXPECT_GT(clf.posteriorAt(0x401000 + i), 0.5)
            << "interior byte " << i << " should lean toward code";
    }
}

TEST(InstrValidity, InvalidInstrIsData)
{
    auto clf = makeX86();
    clf.addInvalidInstruction(0x401000);
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x401000), Label::Data);
    EXPECT_LT(clf.posteriorAt(0x401000), 0.02);
}

TEST(InstrValidity, ImpossibleSemanticIsData)
{
    auto clf = makeX86();
    clf.addImpossibleSemantic(0x401000);
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x401000), Label::Data);
}

TEST(InstrValidity, ValidInstrOverridesImpossible)
{
    // Multiple contradictory pieces of evidence: net result depends on LLR sums.
    auto clf = makeX86();
    clf.addValidInstruction(0x401000, 1);    // +1.386
    clf.addImpossibleSemantic(0x401000);     // -2.944
    // Net ≈ -1.558 → prob ≈ 0.17 → Data
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x401000), Label::Data);
}

// ─── Evidence 3: Alignment ────────────────────────────────────────────────────

TEST(Alignment, FuncAlignNudgesCode)
{
    auto clf = makeX86();
    // No other evidence, just alignment hint on top of prior 0.5.
    clf.addExecutableRange(0x401000, 4);
    clf.addAlignmentHint(0x401000, AlignHint::FunctionEntry);
    clf.classify();
    // LLR_FuncAlign ≈ +1.099 → logOdds ≈ 1.099 → p ≈ 0.75 → Code
    EXPECT_GT(clf.posteriorAt(0x401000), 0.7);
    EXPECT_EQ(clf.labelAt(0x401000), Label::Code);
}

TEST(Alignment, BranchAlignNudgesCode)
{
    auto clf = makeX86();
    clf.addExecutableRange(0x401004, 4);
    clf.addAlignmentHint(0x401004, AlignHint::BranchTarget);
    clf.classify();
    // LLR ≈ +0.693 → p ≈ 0.667 → Ambiguous (just below threshold 0.7)
    double p = clf.posteriorAt(0x401004);
    EXPECT_GT(p, 0.5);
}

TEST(Alignment, NoAlignHintNoChange)
{
    auto clf = makeX86();
    clf.addExecutableRange(0x401000, 1);
    clf.addAlignmentHint(0x401000, AlignHint::None);
    clf.classify();
    EXPECT_NEAR(clf.posteriorAt(0x401000), 0.5, 0.01);
}

// ─── Evidence 4: Reference type ───────────────────────────────────────────────

TEST(References, CallRefIsCode)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);
    clf.classify();
    // LLR_Call ≈ +3.892 → p ≈ 0.98 → Code
    EXPECT_EQ(clf.labelAt(0x401000), Label::Code);
    EXPECT_GT(clf.posteriorAt(0x401000), 0.97);
}

TEST(References, JumpRefIsCode)
{
    auto clf = makeX86();
    clf.addReference(0x401010, RefType::Jump);
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x401010), Label::Code);
}

TEST(References, LoadImmRefNudgesData)
{
    auto clf = makeX86();
    clf.addReference(0x404000, RefType::LoadImm);
    clf.classify();
    // LLR_LoadImm ≈ -1.386 → p ≈ 0.20 → Data
    EXPECT_LT(clf.posteriorAt(0x404000), 0.3);
    EXPECT_EQ(clf.labelAt(0x404000), Label::Data);
}

TEST(References, DataPtrRefIsData)
{
    auto clf = makeX86();
    clf.addReference(0x405000, RefType::DataPtr);
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x405000), Label::Data);
}

TEST(References, MultipleCallsStrengthenCode)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);
    clf.addReference(0x401000, RefType::Call);
    clf.classify();
    // Two calls: logOdds ≈ 7.784 → p ≈ 0.9996 (very close to 1.0)
    EXPECT_GT(clf.posteriorAt(0x401000), 0.999);
}

// ─── Evidence 5: ARM Thumb interworking ──────────────────────────────────────

TEST(ARMThumb, ThumbPointerLSB1IsCode)
{
    auto clf = makeARM();
    clf.addARMPointer(0x8001); // LSB=1 → target=0x8000 is Thumb
    clf.classify();
    EXPECT_EQ(clf.labelAt(0x8000), Label::Code);
    EXPECT_GT(clf.posteriorAt(0x8000), 0.97);
}

TEST(ARMThumb, ThumbPointerSetsThumbFlag)
{
    auto clf = makeARM();
    clf.addARMPointer(0x8001);
    EXPECT_TRUE(clf.isThumb(0x8000));
    EXPECT_FALSE(clf.isThumb(0x8002));
}

TEST(ARMThumb, ARMPointerLSB0IsCodeModerate)
{
    auto clf = makeARM();
    clf.addARMPointer(0x8000); // LSB=0 → ARM code
    clf.classify();
    // LLR_ARMPtr ≈ +1.386 → p ≈ 0.8 → Code
    EXPECT_EQ(clf.labelAt(0x8000), Label::Code);
    EXPECT_FALSE(clf.isThumb(0x8000));
}

TEST(ARMThumb, X86IgnoresARMPointer)
{
    auto clf = makeX86();
    clf.addARMPointer(0x401001); // Should be ignored for x86.
    clf.classify();
    // No evidence was added (non-ARM arch) → prior 0.5 → Ambiguous, but
    // the address was not added to _logOdds so labelAt returns Data.
    EXPECT_NE(clf.labelAt(0x401001), Label::Code);
}

TEST(ARMThumb, MixedThumbAndARM)
{
    auto clf = makeARM();
    clf.addARMPointer(0x8001); // Thumb at 0x8000
    clf.addARMPointer(0x8100); // ARM at 0x8100
    clf.classify();

    EXPECT_TRUE(clf.isThumb(0x8000));
    EXPECT_FALSE(clf.isThumb(0x8100));
    EXPECT_EQ(clf.labelAt(0x8000), Label::Code);
    EXPECT_EQ(clf.labelAt(0x8100), Label::Code);
}

// ─── Combined evidence ────────────────────────────────────────────────────────

TEST(Combined, ReachablePlusValidInstr)
{
    auto clf = makeX86();
    clf.addReachableRange(0x401000, 4);
    clf.addValidInstruction(0x401000, 4);
    clf.classify();
    // Both strong code signals → posterior very high
    EXPECT_GT(clf.posteriorAt(0x401000), 0.99);
    EXPECT_EQ(clf.labelAt(0x401000), Label::Code);
}

TEST(Combined, CallRefPlusInvalidInstr)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);   // +3.892
    clf.addInvalidInstruction(0x401000);          // -4.595
    clf.classify();
    // Net ≈ -0.703 → p ≈ 0.33 → Ambiguous
    double p = clf.posteriorAt(0x401000);
    EXPECT_GT(p, 0.25);
    EXPECT_LT(p, 0.5);
}

TEST(Combined, FiveSourcesAllCode)
{
    auto clf = makeARM();
    clf.addEntryPoint(0x8000);
    clf.addValidInstruction(0x8000, 4);
    clf.addAlignmentHint(0x8000, AlignHint::FunctionEntry);
    clf.addReference(0x8000, RefType::Call);
    clf.addARMPointer(0x8001); // Thumb pointer to 0x8000
    clf.classify();

    EXPECT_EQ(clf.labelAt(0x8000), Label::Code);
    EXPECT_GT(clf.posteriorAt(0x8000), 0.9999);
    EXPECT_TRUE(clf.isThumb(0x8000));
}

TEST(Combined, FiveSourcesAllData)
{
    auto clf = makeX86();
    clf.addInvalidInstruction(0x404000);
    clf.addImpossibleSemantic(0x404000);
    clf.addReference(0x404000, RefType::DataPtr);
    clf.addReference(0x404000, RefType::LoadImm);
    clf.classify();

    EXPECT_EQ(clf.labelAt(0x404000), Label::Data);
    EXPECT_LT(clf.posteriorAt(0x404000), 0.001);
}

// ─── Result compaction ────────────────────────────────────────────────────────

TEST(Results, ContiguousCodeRunMerged)
{
    auto clf = makeX86();
    clf.addReachableRange(0x401000, 8);
    clf.classify();

    auto res = clf.results();
    // All 8 bytes are code → should produce a single contiguous result.
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].addr, 0x401000u);
    EXPECT_EQ(res[0].size, 8u);
    EXPECT_EQ(res[0].label, Label::Code);
}

TEST(Results, CodeAndDataRunsSeparated)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);    // code
    clf.addReference(0x401010, RefType::DataPtr); // data
    clf.classify();

    auto res = clf.results();
    ASSERT_GE(res.size(), 2u);
    // Find code and data regions.
    bool foundCode = false, foundData = false;
    for (auto& r : res) {
        if (r.addr == 0x401000 && r.label == Label::Code) foundCode = true;
        if (r.addr == 0x401010 && r.label == Label::Data) foundData = true;
    }
    EXPECT_TRUE(foundCode);
    EXPECT_TRUE(foundData);
}

TEST(Results, CodeRegionsFilter)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);
    clf.addReference(0x402000, RefType::DataPtr);
    clf.classify();

    auto codeR = clf.codeRegions();
    for (auto& r : codeR) {
        EXPECT_EQ(r.label, Label::Code);
    }
    EXPECT_FALSE(codeR.empty());
}

TEST(Results, NonCodeRegionsFilter)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);
    clf.addReference(0x402000, RefType::DataPtr);
    clf.classify();

    auto nonCode = clf.nonCodeRegions();
    for (auto& r : nonCode) {
        EXPECT_NE(r.label, Label::Code);
    }
    EXPECT_FALSE(nonCode.empty());
}

// ─── Statistics ───────────────────────────────────────────────────────────────

TEST(Stats, TotalCountsCorrect)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);    // code
    clf.addReference(0x402000, RefType::DataPtr); // data
    clf.addExecutableRange(0x403000, 0x403001);   // ambiguous (prior 0.5)
    clf.classify();

    auto s = clf.stats();
    EXPECT_EQ(s.totalBytes, s.codeBytes + s.dataBytes + s.ambiguousBytes);
    EXPECT_GE(s.totalBytes, 3u);
    EXPECT_GE(s.codeBytes, 1u);
    EXPECT_GE(s.dataBytes, 1u);
}

// ─── Edge cases ───────────────────────────────────────────────────────────────

TEST(EdgeCases, EmptyClassifier)
{
    auto clf = makeX86();
    clf.classify();

    EXPECT_TRUE(clf.results().empty());
    auto s = clf.stats();
    EXPECT_EQ(s.totalBytes, 0u);
}

TEST(EdgeCases, LabelNameHelper)
{
    EXPECT_STREQ(labelName(Label::Code),      "code");
    EXPECT_STREQ(labelName(Label::Data),      "data");
    EXPECT_STREQ(labelName(Label::Ambiguous), "ambiguous");
}

TEST(EdgeCases, ClassifyIdempotent)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);
    clf.classify();
    double p1 = clf.posteriorAt(0x401000);
    clf.classify(); // call again — should not change result
    double p2 = clf.posteriorAt(0x401000);
    EXPECT_DOUBLE_EQ(p1, p2);
}

TEST(EdgeCases, ResultsBeforeClassifyIsEmpty)
{
    auto clf = makeX86();
    clf.addReference(0x401000, RefType::Call);
    // classify() not called.
    EXPECT_TRUE(clf.results().empty());
    EXPECT_EQ(clf.labelAt(0x401000), Label::Data); // default
}

TEST(EdgeCases, SameAddressMultipleEvidence)
{
    auto clf = makeX86();
    clf.addValidInstruction(0x401000, 1);
    clf.addAlignmentHint(0x401000, AlignHint::FunctionEntry);
    clf.addReference(0x401000, RefType::Call);
    clf.addEntryPoint(0x401000);
    clf.classify();

    EXPECT_EQ(clf.labelAt(0x401000), Label::Code);
    EXPECT_GT(clf.posteriorAt(0x401000), 0.9999);
}

TEST(EdgeCases, PosteriorClampedTo1)
{
    auto clf = makeX86();
    // Pile on extreme code evidence.
    for (int i = 0; i < 20; ++i)
        clf.addReference(0x401000, RefType::Call);
    clf.classify();
    double p = clf.posteriorAt(0x401000);
    EXPECT_LE(p, 1.0);
    EXPECT_GE(p, 0.0);
}

TEST(EdgeCases, PosteriorClampedTo0)
{
    auto clf = makeX86();
    for (int i = 0; i < 20; ++i)
        clf.addInvalidInstruction(0x404000);
    clf.classify();
    double p = clf.posteriorAt(0x404000);
    EXPECT_LE(p, 1.0);
    EXPECT_GE(p, 0.0);
}

TEST(EdgeCases, NonExistentAddressDefault)
{
    auto clf = makeX86();
    clf.addEntryPoint(0x401000);
    clf.classify();

    // An address we never mentioned.
    EXPECT_NEAR(clf.posteriorAt(0xDEAD0000), 0.5, 0.01);
    EXPECT_EQ(clf.labelAt(0xDEAD0000), Label::Data); // not in _posterior
}

// ─── ARM architecture — no false Thumb marks on x86 ─────────────────────────

TEST(ARMThumb, NoThumbFlagOnX86)
{
    auto clf = makeX86();
    // addARMPointer should no-op for x86.
    clf.addARMPointer(0x401001);
    clf.classify();
    EXPECT_FALSE(clf.isThumb(0x401000));
    EXPECT_FALSE(clf.isThumb(0x401001));
}
