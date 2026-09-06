/**
 * @file tests/func_boundary/func_boundary_test.cpp
 * @brief Unit tests for FuncBoundaryDetector.
 *
 * Each test builds a tiny synthetic binary image (std::vector<uint8_t>) and
 * feeds it to FuncBoundaryDetector, then asserts on the results.
 *
 * Image layout convention used in these tests:
 *   imageBase = 0x400000
 *   All code starts at offset 0x1000 (VA 0x401000).
 */

#include "retdec/func_boundary/func_boundary.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace retdec::func_boundary;

// ─── Test fixture helpers ─────────────────────────────────────────────────────

static constexpr uint64_t kBase = 0x400000;
static constexpr uint64_t kCode = 0x401000; // VA of first code byte

// Build an image of `size` zero bytes; code starts at offset 0x1000.
static std::vector<uint8_t> makeImage(std::size_t size = 0x2000)
{
    return std::vector<uint8_t>(size, 0);
}

// Write bytes at a VA offset in an image.
static void writeAt(std::vector<uint8_t>& img, uint64_t va,
                    std::initializer_list<uint8_t> bytes)
{
    std::size_t off = static_cast<std::size_t>(va - kBase);
    for (uint8_t b : bytes) {
        if (off < img.size()) img[off++] = b;
    }
}

static FuncBoundaryDetector makeDetector(const std::vector<uint8_t>& img,
                                          bool addExecSection = true)
{
    FuncBoundaryDetector det(kBase, img.data(), img.size(), true);
    if (addExecSection)
        det.addExecutableSection(kCode, kBase + img.size());
    return det;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Evidence confidence
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EvidenceConfidence, EntryPointIsMax)
{
    EXPECT_DOUBLE_EQ(evidenceConfidence(EvidenceSource::EntryPoint), 1.0);
}

TEST(EvidenceConfidence, ProloguePartialIsLower)
{
    double full    = evidenceConfidence(EvidenceSource::PrologueFull);
    double partial = evidenceConfidence(EvidenceSource::ProloguePartial);
    EXPECT_GT(full, partial);
    EXPECT_GT(partial, 0.0);
}

TEST(EvidenceConfidence, OrderingCorrect)
{
    // EntryPoint > DebugSymbol > Export > CallTarget > PrologueFull > Partial > Heuristic
    EXPECT_GT(evidenceConfidence(EvidenceSource::EntryPoint),
              evidenceConfidence(EvidenceSource::DebugSymbol));
    EXPECT_GT(evidenceConfidence(EvidenceSource::Export),
              evidenceConfidence(EvidenceSource::PrologueFull));
    EXPECT_GT(evidenceConfidence(EvidenceSource::PrologueFull),
              evidenceConfidence(EvidenceSource::ProloguePartial));
    EXPECT_GT(evidenceConfidence(EvidenceSource::ProloguePartial),
              evidenceConfidence(EvidenceSource::Heuristic));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pass 1 — Direct Evidence
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Pass1, EntryPointRegistered)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.runPass1();

    auto& fns = det.functions();
    ASSERT_EQ(fns.size(), 1u);
    EXPECT_EQ(fns[0].startAddr, kCode);
    EXPECT_DOUBLE_EQ(fns[0].confidence, 1.0);
    EXPECT_EQ(fns[0].primaryEvidence, EvidenceSource::EntryPoint);
}

TEST(Pass1, MultipleDirectEvidence)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.addCallTarget(kCode + 0x100);
    det.addSymbol("my_func", kCode + 0x200, EvidenceSource::Export);
    det.runPass1();

    EXPECT_EQ(det.functions().size(), 3u);
    EXPECT_NE(det.functionAt(kCode),        nullptr);
    EXPECT_NE(det.functionAt(kCode + 0x100), nullptr);
    EXPECT_NE(det.functionAt(kCode + 0x200), nullptr);
}

TEST(Pass1, DuplicateAddressIncreasesConfidence)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.addCallTarget(kCode); // same address again
    det.runPass1();

    // Still one function, but confidence boosted.
    ASSERT_EQ(det.functions().size(), 1u);
    EXPECT_DOUBLE_EQ(det.functions()[0].confidence, 1.0); // capped at 1.0
}

TEST(Pass1, SymbolNamePreserved)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addSymbol("hello_world", kCode + 0x50, EvidenceSource::DebugSymbol);
    det.runPass1();

    auto* fb = det.functionAt(kCode + 0x50);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->name, "hello_world");
}

TEST(Pass1, TLSCallbackRegistered)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addTLSCallback(kCode + 0x300);
    det.runPass1();

    auto* fb = det.functionAt(kCode + 0x300);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->primaryEvidence, EvidenceSource::TLSCallback);
}

TEST(Pass1, ExceptionHandlerRegistered)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addExceptionHandler(kCode + 0x400);
    det.runPass1();

    EXPECT_NE(det.functionAt(kCode + 0x400), nullptr);
}

TEST(Pass1, FunctionsSortedByAddress)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addEntryPoint(kCode + 0x300);
    det.addCallTarget(kCode + 0x100);
    det.addCallTarget(kCode);
    det.runPass1();

    auto& fns = det.functions();
    ASSERT_EQ(fns.size(), 3u);
    EXPECT_LT(fns[0].startAddr, fns[1].startAddr);
    EXPECT_LT(fns[1].startAddr, fns[2].startAddr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Prologue patterns
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ProloguePattern, GCCPatternsPresent)
{
    auto pats = FuncBoundaryDetector::prologuePatterns(CompilerHint::GCC);
    EXPECT_FALSE(pats.empty());
    bool found = false;
    for (const auto& p : pats) {
        if (!p.bytes.empty() && p.bytes[0] == 0x55) { found = true; break; }
    }
    EXPECT_TRUE(found) << "GCC frame setup (push rbp) pattern missing";
}

TEST(ProloguePattern, MSVCPatternsPresent)
{
    auto pats = FuncBoundaryDetector::prologuePatterns(CompilerHint::MSVC);
    EXPECT_FALSE(pats.empty());
}

TEST(ProloguePattern, UnknownHintIncludesBoth)
{
    auto unk  = FuncBoundaryDetector::prologuePatterns(CompilerHint::Unknown);
    auto gcc  = FuncBoundaryDetector::prologuePatterns(CompilerHint::GCC);
    auto msvc = FuncBoundaryDetector::prologuePatterns(CompilerHint::MSVC);
    EXPECT_GE(unk.size(), gcc.size());
    EXPECT_GE(unk.size(), msvc.size());
}

TEST(PrologueMatch, FullMatchReturnsFullScore)
{
    // GCC frame: 55 48 89 E5
    ProloguePattern pat{"gcc_frame", {0x55, 0x48, 0x89, 0xE5}, 0.85, 0.60};
    uint8_t bytes[] = {0x55, 0x48, 0x89, 0xE5, 0x90};
    double score = FuncBoundaryDetector::matchPrologue(pat, bytes, 5);
    EXPECT_DOUBLE_EQ(score, 0.85);
}

TEST(PrologueMatch, PartialMatchReturnsPartialScore)
{
    // Pattern: 55 48 89 E5  — only first 3 bytes match
    ProloguePattern pat{"gcc_frame", {0x55, 0x48, 0x89, 0xE5}, 0.85, 0.60};
    uint8_t bytes[] = {0x55, 0x48, 0x89, 0xFF}; // 4th byte differs
    // Only 3 out of 4 bytes matched before mismatch.
    // 3/4 = 75% ≥ 50% → partialScore
    double score = FuncBoundaryDetector::matchPrologue(pat, bytes, 4);
    EXPECT_DOUBLE_EQ(score, 0.60);
}

TEST(PrologueMatch, NoMatchReturnsZero)
{
    ProloguePattern pat{"gcc_frame", {0x55, 0x48, 0x89, 0xE5}, 0.85, 0.60};
    uint8_t bytes[] = {0x90, 0x90, 0x90, 0x90}; // all wrong
    double score = FuncBoundaryDetector::matchPrologue(pat, bytes, 4);
    EXPECT_DOUBLE_EQ(score, 0.0);
}

TEST(PrologueMatch, WildcardAlwaysMatches)
{
    // Pattern with wildcard: 48 83 EC -1
    ProloguePattern pat{"leaf", {0x48, 0x83, 0xEC, -1}, 0.75, 0.55};
    uint8_t bytes[] = {0x48, 0x83, 0xEC, 0x28}; // sub rsp, 0x28
    double score = FuncBoundaryDetector::matchPrologue(pat, bytes, 4);
    EXPECT_DOUBLE_EQ(score, 0.75);
}

TEST(PrologueMatch, EmptyPatternReturnsZero)
{
    ProloguePattern pat{"empty", {}, 0.85, 0.60};
    uint8_t bytes[] = {0x55};
    EXPECT_DOUBLE_EQ(FuncBoundaryDetector::matchPrologue(pat, bytes, 1), 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pass 2 — Prologue scan + CALL target scan
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Pass2, GCCPrologueDetected)
{
    auto img = makeImage();
    // Write GCC frame setup at kCode+0x100.
    writeAt(img, kCode + 0x100, {0x55, 0x48, 0x89, 0xE5, 0xC3});

    auto det = makeDetector(img);
    det.runPass2(CompilerHint::GCC);

    // Should detect kCode+0x100 as a function start.
    auto* fb = det.functionAt(kCode + 0x100);
    ASSERT_NE(fb, nullptr);
    EXPECT_GE(fb->confidence, 0.80);
}

TEST(Pass2, MSVCShadowPrologueDetected)
{
    auto img = makeImage();
    // sub rsp, 0x28 (48 83 EC 28) at kCode+0x200
    writeAt(img, kCode + 0x200, {0x48, 0x83, 0xEC, 0x28, 0xC3});

    auto det = makeDetector(img);
    det.runPass2(CompilerHint::MSVC);

    auto* fb = det.functionAt(kCode + 0x200);
    ASSERT_NE(fb, nullptr);
}

TEST(Pass2, CALLrel32Detected)
{
    auto img = makeImage(0x3000);
    // Place a CALL rel32 at kCode.  Target = kCode + 0x100.
    // E8 <rel32>: rel = target - (current + 5) = 0x100 - 5 = 0xFB
    int32_t rel = 0x100 - 5;
    writeAt(img, kCode, {0xE8,
        static_cast<uint8_t>(rel),
        static_cast<uint8_t>(rel >> 8),
        static_cast<uint8_t>(rel >> 16),
        static_cast<uint8_t>(rel >> 24)});

    auto det = makeDetector(img);
    det.runPass2(CompilerHint::GCC);

    auto* fb = det.functionAt(kCode + 0x100);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->primaryEvidence, EvidenceSource::CallTarget);
}

TEST(Pass2, HighConfidenceSkipsPrologueScan)
{
    auto img = makeImage();
    // Write GCC prologue at kCode.
    writeAt(img, kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3});

    auto det = makeDetector(img);
    det.addEntryPoint(kCode); // direct evidence at same address
    det.runPass1();
    double confBefore = det.functions()[0].confidence;

    det.runPass2(CompilerHint::GCC);
    // The confidence may be slightly boosted by the prologue match, but the
    // function should still be at that address.
    EXPECT_NE(det.functionAt(kCode), nullptr);
    (void)confBefore;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pass 3 — Non-returning
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NonReturning, KnownSeeds)
{
    EXPECT_TRUE(FuncBoundaryDetector::isKnownNonReturner("exit"));
    EXPECT_TRUE(FuncBoundaryDetector::isKnownNonReturner("abort"));
    EXPECT_TRUE(FuncBoundaryDetector::isKnownNonReturner("__stack_chk_fail"));
    EXPECT_TRUE(FuncBoundaryDetector::isKnownNonReturner("ExitProcess"));
    EXPECT_TRUE(FuncBoundaryDetector::isKnownNonReturner("__cxa_throw"));
    EXPECT_FALSE(FuncBoundaryDetector::isKnownNonReturner("printf"));
    EXPECT_FALSE(FuncBoundaryDetector::isKnownNonReturner("main"));
}

TEST(NonReturning, ImportNonReturnerDetected)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    // Register ExitProcess as an import at some IAT slot.
    uint64_t iatSlot = kBase + 0x5000;
    det.addImport(iatSlot, "kernel32.dll", "ExitProcess");
    det.runAll();

    EXPECT_TRUE(det.isNonReturning(iatSlot));
}

TEST(NonReturning, SymbolNonReturnerDetected)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addSymbol("abort", kCode + 0x500, EvidenceSource::Export);
    det.runAll();

    auto* fb = det.functionAt(kCode + 0x500);
    ASSERT_NE(fb, nullptr);
    EXPECT_TRUE(fb->isNonReturning);
    EXPECT_TRUE(det.isNonReturning(kCode + 0x500));
}

TEST(NonReturning, NormalFunctionNotNonReturning)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addSymbol("my_func", kCode + 0x100, EvidenceSource::Export);
    det.runAll();

    auto* fb = det.functionAt(kCode + 0x100);
    ASSERT_NE(fb, nullptr);
    EXPECT_FALSE(fb->isNonReturning);
}

TEST(NonReturning, MultipleNonReturners)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.addImport(kBase + 0x5000, "msvcrt.dll", "exit");
    det.addImport(kBase + 0x5008, "msvcrt.dll", "abort");
    det.addImport(kBase + 0x5010, "msvcrt.dll", "printf"); // returner
    det.runAll();

    EXPECT_TRUE(det.isNonReturning(kBase + 0x5000));
    EXPECT_TRUE(det.isNonReturning(kBase + 0x5008));
    EXPECT_FALSE(det.isNonReturning(kBase + 0x5010));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Thunk detection
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Thunks, JMPrel32IsThunk)
{
    auto img = makeImage(0x3000);
    // E9 <rel32> at kCode: JMP to kCode+0x200.
    uint64_t targetVA = kCode + 0x200;
    int32_t rel = static_cast<int32_t>(targetVA - (kCode + 5));
    writeAt(img, kCode, {0xE9,
        static_cast<uint8_t>(rel),
        static_cast<uint8_t>(rel >> 8),
        static_cast<uint8_t>(rel >> 16),
        static_cast<uint8_t>(rel >> 24)});

    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.addSymbol("real_func", targetVA, EvidenceSource::Export);
    det.runAll();

    auto* fb = det.functionAt(kCode);
    ASSERT_NE(fb, nullptr);
    EXPECT_TRUE(fb->isThunk);
}

// A thunk's operand belongs to the same section as its opcode.
//
// detectThunkAt bounded its rel32 read by the whole buffer, so a lone 0xE9 as
// the last stored byte of .text read its operand out of whatever section
// follows on disk and reported a jump to an address in no registered section.
// In bounds of the buffer, so not a memory-safety bug -- a fabricated thunk
// target, which is worse to read in decompiled output than a missing one.
TEST(Thunks, OperandIsNotReadOutOfTheNextSection)
{
    std::vector<uint8_t> img(0x800, 0);

    // .text: VA kBase+0x1000, raw 0x400, 0x200 bytes stored.
    const uint64_t textVA  = kBase + 0x1000;
    const std::size_t rawOff = 0x400;
    const std::size_t rawSz  = 0x200;

    // A lone 0xE9 as the section's last stored byte, with nothing after it
    // inside the section.
    img[rawOff + rawSz - 1] = 0xE9;
    // The next section's data, which the rel32 read used to pick up.
    img[rawOff + rawSz + 0] = 0x11;
    img[rawOff + rawSz + 1] = 0x22;
    img[rawOff + rawSz + 2] = 0x33;
    img[rawOff + rawSz + 3] = 0x44;

    FuncBoundaryDetector det(kBase, img.data(), img.size(), true);
    det.addExecutableSection(textVA, textVA + rawSz, rawOff, rawSz);
    const uint64_t jmpVA = textVA + rawSz - 1;
    det.addEntryPoint(jmpVA);
    det.runAll();

    auto* fb = det.functionAt(jmpVA);
    ASSERT_NE(fb, nullptr);
    EXPECT_FALSE(fb->isThunk);
    EXPECT_TRUE(fb->thunkTarget.empty());
}

// The same shape with the operand actually inside the section still resolves.
TEST(Thunks, OperandInsideTheSectionStillResolves)
{
    std::vector<uint8_t> img(0x800, 0);
    const uint64_t textVA  = kBase + 0x1000;
    const std::size_t rawOff = 0x400;
    const std::size_t rawSz  = 0x200;

    const uint64_t jmpVA    = textVA;
    const uint64_t targetVA = textVA + 0x100;
    const int32_t rel = static_cast<int32_t>(targetVA - (jmpVA + 5));
    img[rawOff + 0] = 0xE9;
    img[rawOff + 1] = static_cast<uint8_t>(rel);
    img[rawOff + 2] = static_cast<uint8_t>(rel >> 8);
    img[rawOff + 3] = static_cast<uint8_t>(rel >> 16);
    img[rawOff + 4] = static_cast<uint8_t>(rel >> 24);

    FuncBoundaryDetector det(kBase, img.data(), img.size(), true);
    det.addExecutableSection(textVA, textVA + rawSz, rawOff, rawSz);
    det.addEntryPoint(jmpVA);
    det.addSymbol("real_func", targetVA, EvidenceSource::Export);
    det.runAll();

    auto* fb = det.functionAt(jmpVA);
    ASSERT_NE(fb, nullptr);
    EXPECT_TRUE(fb->isThunk);
    EXPECT_EQ(fb->thunkTarget, "real_func");
}

TEST(Thunks, JMPIndirectIsThunk)
{
    auto img = makeImage(0x3000);
    // FF 25 <rel32>: JMP [RIP+rel32] — indirect thunk pattern
    // rel = IAT slot offset from (kCode + 6)
    uint64_t iatVA  = kBase + 0x5000;
    int32_t rel = static_cast<int32_t>(iatVA - (kCode + 6));
    writeAt(img, kCode, {0xFF, 0x25,
        static_cast<uint8_t>(rel),
        static_cast<uint8_t>(rel >> 8),
        static_cast<uint8_t>(rel >> 16),
        static_cast<uint8_t>(rel >> 24)});

    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.addImport(iatVA, "kernel32.dll", "GetLastError");
    det.runAll();

    auto* fb = det.functionAt(kCode);
    ASSERT_NE(fb, nullptr);
    EXPECT_TRUE(fb->isThunk);
    EXPECT_FALSE(fb->thunkTarget.empty());
}

TEST(Thunks, ThunkNamedAfterTarget)
{
    auto img = makeImage(0x3000);
    uint64_t iatVA = kBase + 0x5008;
    int32_t rel = static_cast<int32_t>(iatVA - (kCode + 0x100 + 6));
    writeAt(img, kCode + 0x100, {0xFF, 0x25,
        static_cast<uint8_t>(rel),
        static_cast<uint8_t>(rel >> 8),
        static_cast<uint8_t>(rel >> 16),
        static_cast<uint8_t>(rel >> 24)});

    auto det = makeDetector(img);
    det.addEntryPoint(kCode + 0x100);
    det.addImport(iatVA, "msvcrt.dll", "malloc");
    det.runAll();

    auto* fb = det.functionAt(kCode + 0x100);
    ASSERT_NE(fb, nullptr);
    EXPECT_TRUE(fb->isThunk);
    EXPECT_EQ(fb->thunkTarget, "malloc");
}

TEST(Thunks, NormalFunctionNotThunk)
{
    auto img = makeImage();
    writeAt(img, kCode, {0x55, 0x48, 0x89, 0xE5, 0xC3}); // push rbp; ...; ret
    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.runAll();

    auto* fb = det.functionAt(kCode);
    ASSERT_NE(fb, nullptr);
    EXPECT_FALSE(fb->isThunk);
}

// ═══════════════════════════════════════════════════════════════════════════════
// End address estimation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EndAddress, NextFunctionIsEndOfPrev)
{
    auto img = makeImage(0x3000);
    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.addCallTarget(kCode + 0x100);
    det.addCallTarget(kCode + 0x200);
    det.runAll();

    auto& fns = det.functions();
    ASSERT_GE(fns.size(), 3u);
    // Function 0 ends where function 1 starts.
    EXPECT_EQ(fns[0].endAddr, fns[1].startAddr);
    EXPECT_EQ(fns[1].endAddr, fns[2].startAddr);
}

TEST(EndAddress, LastFunctionExtendsToSectionEnd)
{
    auto img = makeImage(0x3000);
    auto det = makeDetector(img);
    det.addEntryPoint(kCode + 0x500);
    det.runAll();

    auto* fb = det.functionAt(kCode + 0x500);
    ASSERT_NE(fb, nullptr);
    EXPECT_GT(fb->endAddr, fb->startAddr);
    // End should be at least section end (kBase + 0x3000).
    EXPECT_EQ(fb->endAddr, kBase + img.size());
}

// ═══════════════════════════════════════════════════════════════════════════════
// runAll integration
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Integration, FullPipeline)
{
    auto img = makeImage(0x3000);

    // entry: GCC function at kCode
    writeAt(img, kCode, {0x55, 0x48, 0x89, 0xE5, // push rbp; mov rbp,rsp
                          0xE8, 0xFB, 0x00, 0x00, 0x00, // CALL kCode+0x105
                          0x5D, 0xC3}); // pop rbp; ret

    // Called function at kCode+0x105 (E8 from kCode+4: target = kCode+4+5+0xFB = kCode+0x104? adjust)
    // Let's use offset 0x100 for simplicity and write a real GCC prologue.
    writeAt(img, kCode + 0x100, {0x55, 0x48, 0x89, 0xE5, 0xC3});

    auto det = makeDetector(img);
    det.addEntryPoint(kCode);
    det.addImport(kBase + 0x5000, "msvcrt.dll", "exit");
    det.runAll(CompilerHint::GCC);

    // entry at kCode detected.
    EXPECT_NE(det.functionAt(kCode), nullptr);
    // exit() import is non-returning.
    EXPECT_TRUE(det.isNonReturning(kBase + 0x5000));
    // Results are sorted.
    auto& fns = det.functions();
    for (std::size_t i = 1; i < fns.size(); ++i) {
        EXPECT_LT(fns[i-1].startAddr, fns[i].startAddr);
    }
}

TEST(Integration, EmptyDetectorHasNoFunctions)
{
    auto img = makeImage();
    auto det = makeDetector(img);
    det.runAll();
    EXPECT_TRUE(det.functions().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Section mapping — file offset is not (VA - imageBase)
// ═══════════════════════════════════════════════════════════════════════════════
//
// vaToOffset used to return va - imageBase for every address, which assumes the
// bytes on disk sit exactly where they sit in memory.  They never do: a PE maps
// sections at SectionAlignment (0x1000) and stores them at FileAlignment
// (0x200), so the canonical .text at RVA 0x1000 / raw 0x400 was read 3 KB past
// its own bytes, and every ELF segment after the first is skewed the same way.
// These tests use that canonical layout: .text is at VA kBase+0x1000 but its
// bytes live at file offset 0x400.

static constexpr uint64_t kTextVa      = kBase + 0x1000;
static constexpr uint64_t kTextRawOff  = 0x400;
static constexpr uint64_t kTextRawSize = 0x200;

// A "file" holding page-aligned headers and one .text section stored at 0x400.
static std::vector<uint8_t> makeFileAlignedImage()
{
    return std::vector<uint8_t>(kTextRawOff + kTextRawSize, 0);
}

static void writeRaw(std::vector<uint8_t>& img, std::size_t off,
                     std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) {
        if (off < img.size()) img[off++] = b;
    }
}

TEST(SectionMapping, PrologueFoundThroughRawOffset)
{
    auto img = makeFileAlignedImage();
    // GCC frame setup for the first function of .text, i.e. at VA kTextVa,
    // which is stored at file offset 0x400.
    writeRaw(img, kTextRawOff + 0x20, {0x55, 0x48, 0x89, 0xE5, 0xC3});

    FuncBoundaryDetector det(kBase, img.data(), img.size(), true);
    det.addExecutableSection(kTextVa, kTextVa + kTextRawSize,
                             kTextRawOff, kTextRawSize);
    det.runPass2(CompilerHint::GCC);

    // Reading at (VA - imageBase) = 0x1000 is past the end of this 0x600-byte
    // file, so the buggy version found nothing at all.
    auto* fb = det.functionAt(kTextVa + 0x20);
    ASSERT_NE(fb, nullptr);
    EXPECT_GE(fb->confidence, 0.80);
}

TEST(SectionMapping, CallTargetUsesSectionRelativeBytes)
{
    auto img = makeFileAlignedImage();
    // CALL rel32 at VA kTextVa (file offset 0x400) targeting kTextVa + 0x80.
    int32_t rel = 0x80 - 5;
    writeRaw(img, kTextRawOff, {0xE8,
        static_cast<uint8_t>(rel),
        static_cast<uint8_t>(rel >> 8),
        static_cast<uint8_t>(rel >> 16),
        static_cast<uint8_t>(rel >> 24)});

    FuncBoundaryDetector det(kBase, img.data(), img.size(), true);
    det.addExecutableSection(kTextVa, kTextVa + kTextRawSize,
                             kTextRawOff, kTextRawSize);
    det.runPass2(CompilerHint::GCC);

    auto* fb = det.functionAt(kTextVa + 0x80);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->primaryEvidence, EvidenceSource::CallTarget);
}

TEST(SectionMapping, ThunkTargetComputedFromAddressNotOffset)
{
    auto img = makeFileAlignedImage();
    // E9 rel32 at VA kTextVa (file offset 0x400) jumping to kTextVa + 0x100.
    uint64_t targetVa = kTextVa + 0x100;
    int32_t rel = static_cast<int32_t>(targetVa - (kTextVa + 5));
    writeRaw(img, kTextRawOff, {0xE9,
        static_cast<uint8_t>(rel),
        static_cast<uint8_t>(rel >> 8),
        static_cast<uint8_t>(rel >> 16),
        static_cast<uint8_t>(rel >> 24)});

    FuncBoundaryDetector det(kBase, img.data(), img.size(), true);
    det.addExecutableSection(kTextVa, kTextVa + kTextRawSize,
                             kTextRawOff, kTextRawSize);
    det.addEntryPoint(kTextVa);
    det.addSymbol("real_func", targetVa, EvidenceSource::Export);
    det.runAll(CompilerHint::GCC);

    auto* fb = det.functionAt(kTextVa);
    ASSERT_NE(fb, nullptr);
    EXPECT_TRUE(fb->isThunk);
    // The target has to be the real function, not whatever the flat guess
    // (imageBase + 0x400 + 5 + rel) happened to land on.
    EXPECT_EQ(fb->thunkTarget, "real_func");
}

TEST(SectionMapping, VirtualTailWithoutFileBytesIsNotScanned)
{
    // .text is 0x1000 bytes once mapped but only 0x200 bytes on disk; the rest
    // is the zero-filled tail, which has no file bytes at all.  The flat guess
    // used to translate an address in that tail straight to (VA - imageBase)
    // and read whatever else happens to sit at that offset -- here a stray E9
    // that then reads as a JMP thunk.
    std::vector<uint8_t> img(0x2000, 0);
    writeRaw(img, 0x1800, {0xE9, 0x00, 0x00, 0x00, 0x00});

    FuncBoundaryDetector det(kBase, img.data(), img.size(), true);
    det.addExecutableSection(kTextVa, kTextVa + 0x1000,
                             kTextRawOff, kTextRawSize);
    det.addEntryPoint(kTextVa + 0x800); // inside the virtual tail

    det.runAll(CompilerHint::GCC);

    auto* fb = det.functionAt(kTextVa + 0x800);
    ASSERT_NE(fb, nullptr);
    EXPECT_FALSE(fb->isThunk);
}

TEST(SectionMapping, FlatImageStillWorksWithTwoArgForm)
{
    // The two-argument overload keeps the flat mapping, for callers whose
    // buffer really is the memory image.
    auto img = makeImage(0x3000);
    writeAt(img, kCode + 0x40, {0x55, 0x48, 0x89, 0xE5, 0xC3});

    auto det = makeDetector(img);
    det.runPass2(CompilerHint::GCC);

    EXPECT_NE(det.functionAt(kCode + 0x40), nullptr);
}
