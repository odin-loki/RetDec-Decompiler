/**
 * @file tests/compiler_detect/compiler_fingerprinter_test.cpp
 * @brief Unit tests for CompilerFingerprinter.
 *
 * Tests are organised by:
 *   1. Feature extraction correctness (prologue patterns)
 *   2. Decision-tree classification (family, ABI, calling convention)
 *   3. Version range estimation
 *   4. Optimisation level estimation
 *   5. Edge cases (empty input, insufficient evidence, conflicting signals)
 */

#include "retdec/compiler_detect/compiler_fingerprinter.h"
#include "retdec/compiler_detect/compiler_profile.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace retdec::compiler_detect;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Build a synthetic function prologue byte sequence.
struct PrologueBuilder {
    std::vector<uint8_t> bytes;

    // PUSH RBP (55)
    PrologueBuilder& pushRBP()
    { bytes.push_back(0x55); return *this; }

    // MOV RBP, RSP (48 89 E5)
    PrologueBuilder& movRBPRSP()
    { bytes.insert(bytes.end(), {0x48, 0x89, 0xE5}); return *this; }

    // SUB RSP, imm8 (48 83 EC xx)
    PrologueBuilder& subRSP(uint8_t imm)
    { bytes.insert(bytes.end(), {0x48, 0x83, 0xEC, imm}); return *this; }

    // AND RSP, -16 (48 83 E4 F0)
    PrologueBuilder& andRSP16()
    { bytes.insert(bytes.end(), {0x48, 0x83, 0xE4, 0xF0}); return *this; }

    // REP STOSB (F3 AA)
    PrologueBuilder& repStosb()
    { bytes.insert(bytes.end(), {0xF3, 0xAA}); return *this; }

    // REP STOSQ (F3 48 AB)
    PrologueBuilder& repStosq()
    { bytes.insert(bytes.end(), {0xF3, 0x48, 0xAB}); return *this; }

    // RET (C3)
    PrologueBuilder& ret()
    { bytes.push_back(0xC3); return *this; }

    // JMP rel32 (E9 00 00 00 00)
    PrologueBuilder& jmpRel32()
    { bytes.insert(bytes.end(), {0xE9, 0x00, 0x00, 0x00, 0x00}); return *this; }

    // NOP (90) — pad to size
    PrologueBuilder& nops(int n)
    { for (int i = 0; i < n; ++i) bytes.push_back(0x90); return *this; }

    // XMM zero-store pattern (66 0F 7F /r)
    PrologueBuilder& xmmZeroStore()
    { bytes.insert(bytes.end(), {0x66, 0x0F, 0x7F, 0x07}); return *this; }

    const uint8_t* data() const { return bytes.data(); }
    uint32_t       size() const { return static_cast<uint32_t>(bytes.size()); }
};

// Helper: create a fingerprinter with N identical functions.
static CompilerFingerprinter makeWithFuncs(const PrologueBuilder& pb, int count)
{
    CompilerFingerprinter fp;
    for (int i = 0; i < count; ++i)
        fp.addFunction(pb.data(), pb.size());
    return fp;
}

// ─── 1. Feature extraction — frame pointer ───────────────────────────────────

TEST(FeatureExtraction, FramePointerDetected)
{
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().subRSP(0x28).nops(4).ret();

    auto fp = makeWithFuncs(pb, 10);
    auto fv = fp.extractFeatures();

    EXPECT_FLOAT_EQ(fv.framePointerRatio, 1.0f);
}

TEST(FeatureExtraction, NoFramePointer)
{
    PrologueBuilder pb;
    pb.subRSP(0x28).nops(4).ret();

    auto fp = makeWithFuncs(pb, 10);
    auto fv = fp.extractFeatures();

    EXPECT_FLOAT_EQ(fv.framePointerRatio, 0.0f);
}

TEST(FeatureExtraction, MixedFramePointerRatio)
{
    CompilerFingerprinter fp;

    // 6 functions with frame pointer
    PrologueBuilder withFP;
    withFP.pushRBP().movRBPRSP().ret();
    for (int i = 0; i < 6; ++i)
        fp.addFunction(withFP.data(), withFP.size());

    // 4 functions without
    PrologueBuilder noFP;
    noFP.subRSP(0x28).ret();
    for (int i = 0; i < 4; ++i)
        fp.addFunction(noFP.data(), noFP.size());

    auto fv = fp.extractFeatures();
    EXPECT_NEAR(fv.framePointerRatio, 0.6f, 0.01f);
}

// ─── 2. Feature extraction — stack alignment ─────────────────────────────────

TEST(FeatureExtraction, StackAlignDetected)
{
    PrologueBuilder pb;
    pb.andRSP16().subRSP(0x28).ret();

    auto fp = makeWithFuncs(pb, 20);
    auto fv = fp.extractFeatures();

    EXPECT_TRUE(fv.stackAlign16);
}

TEST(FeatureExtraction, StackAlignNotPresent)
{
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().ret();

    auto fp = makeWithFuncs(pb, 20);
    auto fv = fp.extractFeatures();

    EXPECT_FALSE(fv.stackAlign16);
}

// ─── 3. Feature extraction — shadow space ────────────────────────────────────

TEST(FeatureExtraction, ShadowSpaceDetectedAbove50Percent)
{
    // SUB RSP, 0x28 = 40 bytes ≥ 32 (shadow space)
    PrologueBuilder pb;
    pb.subRSP(0x28).ret();

    auto fp = makeWithFuncs(pb, 30);
    auto fv = fp.extractFeatures();

    EXPECT_TRUE(fv.shadowSpaceAlloc);
}

TEST(FeatureExtraction, ShadowSpaceNotDetectedSmallSub)
{
    // SUB RSP, 0x10 = 16 bytes < 32 (not shadow space)
    PrologueBuilder pb;
    pb.subRSP(0x10).ret();

    auto fp = makeWithFuncs(pb, 30);
    auto fv = fp.extractFeatures();

    EXPECT_FALSE(fv.shadowSpaceAlloc);
}

// ─── 4. Feature extraction — memset inline threshold ─────────────────────────

TEST(FeatureExtraction, RepStosqDetected)
{
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().repStosq().ret();

    auto fp = makeWithFuncs(pb, 5);
    auto fv = fp.extractFeatures();

    EXPECT_GE(fv.memsetInlineThreshold, 8u);
}

TEST(FeatureExtraction, RepStosbDetected)
{
    PrologueBuilder pb;
    pb.repStosb().ret();

    auto fp = makeWithFuncs(pb, 5);
    auto fv = fp.extractFeatures();

    EXPECT_GE(fv.memsetInlineThreshold, 1u);
}

TEST(FeatureExtraction, XmmZeroStoreDetected)
{
    PrologueBuilder pb;
    pb.xmmZeroStore().ret();

    auto fp = makeWithFuncs(pb, 5);
    auto fv = fp.extractFeatures();

    EXPECT_GE(fv.memsetInlineThreshold, 16u);
}

TEST(FeatureExtraction, NoMemsetPattern)
{
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().nops(8).ret();

    auto fp = makeWithFuncs(pb, 10);
    auto fv = fp.extractFeatures();

    EXPECT_EQ(fv.memsetInlineThreshold, 0u);
}

// ─── 5. Feature extraction — tail call ratio ─────────────────────────────────

TEST(FeatureExtraction, TailCallDetected)
{
    // Function ends with JMP rel32, no RET
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().nops(4).jmpRel32();

    auto fp = makeWithFuncs(pb, 10);
    auto fv = fp.extractFeatures();

    EXPECT_GT(fv.tailCallRatio, 0.0f);
}

TEST(FeatureExtraction, NoTailCallForRet)
{
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().nops(4).ret();

    auto fp = makeWithFuncs(pb, 10);
    auto fv = fp.extractFeatures();

    EXPECT_FLOAT_EQ(fv.tailCallRatio, 0.0f);
}

// ─── 6. Feature extraction — EH personality and mangling via setters ─────────

TEST(FeatureExtraction, EHPersonalityGxxFromImports)
{
    CompilerFingerprinter fp;
    fp.setImports({"__gxx_personality_v0", "malloc", "free"});
    auto fv = fp.extractFeatures();
    EXPECT_EQ(fv.ehPersonality, 'g');
}

TEST(FeatureExtraction, EHPersonalityMSVC)
{
    CompilerFingerprinter fp;
    fp.setEHPersonality('m');
    auto fv = fp.extractFeatures();
    EXPECT_EQ(fv.ehPersonality, 'm');
}

TEST(FeatureExtraction, StackCanaryFromImports)
{
    CompilerFingerprinter fp;
    fp.setImports({"__stack_chk_fail", "printf"});
    auto fv = fp.extractFeatures();
    EXPECT_TRUE(fv.stackCanary);
}

TEST(FeatureExtraction, RichHeaderVSMajor)
{
    CompilerFingerprinter fp;
    fp.setRichHeader(19);
    auto fv = fp.extractFeatures();
    EXPECT_EQ(fv.richHeaderVSMajor, 19u);
}

TEST(FeatureExtraction, FunctionsAnalysedCount)
{
    PrologueBuilder pb;
    pb.ret();
    auto fp = makeWithFuncs(pb, 42);
    EXPECT_EQ(fp.extractFeatures().functionsAnalysed, 42u);
}

// ─── 7. Classification — MSVC hard evidence ──────────────────────────────────

TEST(Classification, MSVCFromRichHeader)
{
    CompilerFingerprinter fp;
    // Shadow space + Rich header = MSVC
    PrologueBuilder pb;
    pb.subRSP(0x28).ret();
    for (int i = 0; i < 50; ++i) fp.addFunction(pb.data(), pb.size());
    fp.setRichHeader(19);

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::MSVC);
    EXPECT_EQ(p.cppABI, CppABI::MSVC);
    EXPECT_EQ(p.callConv, CallingConvention::Win64);
    EXPECT_GT(p.confidence, 0.85f);
}

TEST(Classification, MSVCFromEHPersonality)
{
    CompilerFingerprinter fp;
    fp.setEHPersonality('m');
    fp.setManglingStyle(false, true);

    PrologueBuilder pb;
    pb.subRSP(0x28).ret();
    for (int i = 0; i < 30; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::MSVC);
    EXPECT_GT(p.confidence, 0.70f);
}

TEST(Classification, MSVCFromManglingAloneWithoutItanium)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(false, true);  // only MSVC mangling

    PrologueBuilder pb;
    pb.subRSP(0x28).ret();
    for (int i = 0; i < 20; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::MSVC);
}

// ─── 8. Classification — GCC Linux ───────────────────────────────────────────

TEST(Classification, GCCLinuxHighFramePointer)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);
    fp.setImports({"__gxx_personality_v0", "__stack_chk_fail"});

    // High frame-pointer ratio, no shadow space, low tail-call → GCC O0/O1
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().subRSP(0x10).nops(4).ret();
    for (int i = 0; i < 100; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::GCC);
    EXPECT_EQ(p.callConv, CallingConvention::SystemV_AMD64);
    EXPECT_EQ(p.cppABI, CppABI::Itanium);
}

TEST(Classification, GCCLinuxO0OptLevel)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);

    // Very high FP ratio (all functions) + no tail calls → O0
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().nops(16).ret();
    for (int i = 0; i < 100; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::GCC);
    EXPECT_EQ(p.optLevel, OptLevel::O0);
}

// ─── 9. Classification — Clang Linux ─────────────────────────────────────────

TEST(Classification, ClangLinuxHighTailCallRatio)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);
    fp.setImports({"__gxx_personality_v0"});

    // Low frame-pointer + high tail-call ratio + stack align → Clang O2/O3
    CompilerFingerprinter fpTailCall;
    fpTailCall.setManglingStyle(true, false);

    PrologueBuilder withFP;
    withFP.pushRBP().movRBPRSP().nops(4).ret();
    for (int i = 0; i < 15; ++i) fpTailCall.addFunction(withFP.data(), withFP.size());

    PrologueBuilder tail;
    tail.andRSP16().subRSP(0x10).nops(4).jmpRel32();
    for (int i = 0; i < 85; ++i) fpTailCall.addFunction(tail.data(), tail.size());

    auto p = fpTailCall.classify();
    EXPECT_EQ(p.family, CompilerFamily::Clang);
    EXPECT_EQ(p.callConv, CallingConvention::SystemV_AMD64);
}

TEST(Classification, ClangO3FromLowFPHighTailCall)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);

    // Very low FP ratio + high tail-call + XMM store → O3
    PrologueBuilder pb;
    pb.andRSP16().xmmZeroStore().jmpRel32();
    for (int i = 0; i < 100; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_NE(p.optLevel, OptLevel::O0);
}

// ─── 10. Classification — MinGW (Win64 + Itanium) ────────────────────────────

TEST(Classification, MinGWGCCOnWindows)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);  // Itanium (GCC/Clang on Windows)

    // Shadow space (WIN64 ABI) + low tail-call → GCC MinGW
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().subRSP(0x20).ret();
    for (int i = 0; i < 60; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.callConv, CallingConvention::Win64);
    EXPECT_EQ(p.cppABI, CppABI::Itanium);
}

TEST(Classification, MinGWClangOnWindowsHighTailCall)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);

    PrologueBuilder pb;
    pb.subRSP(0x20).jmpRel32();  // shadow space + tail call
    for (int i = 0; i < 80; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.callConv, CallingConvention::Win64);
    EXPECT_EQ(p.cppABI, CppABI::Itanium);
    // High tail-call with shadow → Clang
    EXPECT_EQ(p.family, CompilerFamily::Clang);
}

// ─── 11. Version range ────────────────────────────────────────────────────────

TEST(VersionRange, GCCWithStackCanaryAndXMM)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);
    fp.setImports({"__stack_chk_fail"});

    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().xmmZeroStore().ret();
    for (int i = 0; i < 50; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::GCC);
    EXPECT_GE(p.version.major_lo, 10u);
}

TEST(VersionRange, MSVCFromRichHeaderVS2019)
{
    CompilerFingerprinter fp;
    fp.setRichHeader(16);  // VS 2019 toolset 16.x

    PrologueBuilder pb;
    pb.subRSP(0x28).ret();
    for (int i = 0; i < 20; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::MSVC);
    // VS 2019 major == 2019
    EXPECT_EQ(p.version.major_lo, 2019u);
    EXPECT_EQ(p.version.major_hi, 2019u);
}

TEST(VersionRange, ClangHighTailCallNarrowsToNewVersion)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);
    fp.setImports({"__stack_chk_fail"});

    // Very high tail-call ratio (>15%) → Clang 15+
    PrologueBuilder pb;
    pb.andRSP16().jmpRel32();
    for (int i = 0; i < 90; ++i) fp.addFunction(pb.data(), pb.size());

    PrologueBuilder pb2;
    pb2.ret();
    for (int i = 0; i < 10; ++i) fp.addFunction(pb2.data(), pb2.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::Clang);
    EXPECT_GE(p.version.major_lo, 14u);
}

// ─── 12. Optimisation level ───────────────────────────────────────────────────

TEST(OptLevel, O0HighFramePointerNoTailCall)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);

    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().nops(20).ret();
    for (int i = 0; i < 100; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.optLevel, OptLevel::O0);
}

TEST(OptLevel, O3LowFPHighTailCallLargeMemset)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);

    // Very low frame-pointer + high tail-call + XMM memset → O3
    PrologueBuilder pb;
    pb.xmmZeroStore().xmmZeroStore().xmmZeroStore().jmpRel32();
    for (int i = 0; i < 100; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_NE(p.optLevel, OptLevel::O0);
    EXPECT_NE(p.optLevel, OptLevel::O1);
}

TEST(OptLevel, OsSmallMemsetModerateFP)
{
    CompilerFingerprinter fp;
    fp.setManglingStyle(true, false);

    // Moderate FP (40%), no memset inline, low tail-call → Os
    PrologueBuilder withFP;
    withFP.pushRBP().movRBPRSP().ret();
    for (int i = 0; i < 40; ++i) fp.addFunction(withFP.data(), withFP.size());

    PrologueBuilder noFP;
    noFP.subRSP(0x10).ret();
    for (int i = 0; i < 60; ++i) fp.addFunction(noFP.data(), noFP.size());

    auto p = fp.classify();
    // FP ratio ≈ 0.40, tail-call ≈ 0, memset = 0 → Os
    EXPECT_EQ(p.optLevel, OptLevel::Os);
}

// ─── 13. Edge cases ───────────────────────────────────────────────────────────

TEST(EdgeCases, EmptyInput)
{
    CompilerFingerprinter fp;
    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::Unknown);
    EXPECT_FLOAT_EQ(p.confidence, 0.0f);
}

TEST(EdgeCases, NullFunctionBytesIgnored)
{
    CompilerFingerprinter fp;
    fp.addFunction(nullptr, 0);
    fp.addFunction(nullptr, 100);
    auto fv = fp.extractFeatures();
    EXPECT_EQ(fv.functionsAnalysed, 0u);
}

TEST(EdgeCases, TinyFunction1Byte)
{
    uint8_t ret = 0xC3;
    CompilerFingerprinter fp;
    fp.addFunction(&ret, 1);
    // Should not crash; features should be near-zero.
    auto fv = fp.extractFeatures();
    EXPECT_EQ(fv.functionsAnalysed, 1u);
}

TEST(EdgeCases, MaxFunctionsCapAt1000)
{
    CompilerFingerprinter fp;
    uint8_t ret = 0xC3;
    for (int i = 0; i < 2000; ++i) fp.addFunction(&ret, 1);
    auto fv = fp.extractFeatures();
    EXPECT_EQ(fv.functionsAnalysed, 1000u);
}

TEST(EdgeCases, ConflictingSignalsMSVCWinsOverMangling)
{
    CompilerFingerprinter fp;
    // Rich header (strong MSVC signal) + Itanium mangling (contradiction).
    // Rich header should win.
    fp.setRichHeader(19);
    fp.setManglingStyle(true, true);

    PrologueBuilder pb;
    pb.subRSP(0x28).ret();
    for (int i = 0; i < 30; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::MSVC);
}

TEST(EdgeCases, GCCWithNoManglingFewFunctions)
{
    CompilerFingerprinter fp;
    // Only 2 functions, no mangling info → should fall through to Unknown or low-conf.
    PrologueBuilder pb;
    pb.pushRBP().movRBPRSP().ret();
    fp.addFunction(pb.data(), pb.size());
    fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    // Without hard evidence and only 2 functions: low confidence or Unknown.
    EXPECT_LT(p.confidence, 0.6f);
}

// ─── 14. toString helpers ─────────────────────────────────────────────────────

TEST(Helpers, ToStringCompilerFamily)
{
    EXPECT_EQ(toString(CompilerFamily::GCC),     "GCC");
    EXPECT_EQ(toString(CompilerFamily::Clang),   "Clang");
    EXPECT_EQ(toString(CompilerFamily::MSVC),    "MSVC");
    EXPECT_EQ(toString(CompilerFamily::Unknown), "Unknown");
}

TEST(Helpers, ToStringOptLevel)
{
    EXPECT_EQ(toString(OptLevel::O0), "O0");
    EXPECT_EQ(toString(OptLevel::O2), "O2");
    EXPECT_EQ(toString(OptLevel::Os), "Os");
}

TEST(Helpers, ToStringCallingConvention)
{
    EXPECT_EQ(toString(CallingConvention::SystemV_AMD64), "SystemV_AMD64");
    EXPECT_EQ(toString(CallingConvention::Win64),         "Win64");
}

TEST(Helpers, VersionRangeToString)
{
    VersionRange vr{11, 13};
    EXPECT_EQ(vr.toString(), "11-13");

    VersionRange exact{12, 12};
    EXPECT_EQ(exact.toString(), "12");

    VersionRange unknown{0, 0};
    EXPECT_EQ(unknown.toString(), "unknown");
    EXPECT_FALSE(unknown.isKnown());
}

TEST(Helpers, CompilerProfileSummary)
{
    CompilerProfile p;
    p.family   = CompilerFamily::GCC;
    p.version  = {11, 12};
    p.optLevel = OptLevel::O2;
    p.callConv = CallingConvention::SystemV_AMD64;
    p.confidence = 0.87f;

    std::string s = p.summary();
    EXPECT_NE(s.find("GCC"),           std::string::npos);
    EXPECT_NE(s.find("11-12"),         std::string::npos);
    EXPECT_NE(s.find("O2"),            std::string::npos);
    EXPECT_NE(s.find("SystemV_AMD64"), std::string::npos);
    EXPECT_NE(s.find("87%"),           std::string::npos);
}

// ─── 15. MSVC optimisation level ─────────────────────────────────────────────

TEST(MSVCOptLevel, O0NeitherTailCallNorMemset)
{
    CompilerFingerprinter fp;
    fp.setRichHeader(19);
    fp.setEHPersonality('m');

    PrologueBuilder pb;
    pb.subRSP(0x28).nops(10).ret();
    for (int i = 0; i < 50; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::MSVC);
    EXPECT_EQ(p.optLevel, OptLevel::O0);
}

TEST(MSVCOptLevel, O2WithMemsetInline)
{
    CompilerFingerprinter fp;
    fp.setRichHeader(19);

    PrologueBuilder pb;
    pb.subRSP(0x28).xmmZeroStore().xmmZeroStore().ret();
    for (int i = 0; i < 50; ++i) fp.addFunction(pb.data(), pb.size());

    auto p = fp.classify();
    EXPECT_EQ(p.family, CompilerFamily::MSVC);
    EXPECT_EQ(p.optLevel, OptLevel::O2);
}
