/**
 * @file tests/packer/packer_detector_test.cpp
 * @brief Unit tests for EntropyProfiler and PackerDetector.
 */

#include "retdec/packer/entropy_profiler.h"
#include "retdec/packer/packer_detector.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace retdec::packer;
using namespace retdec::fileformat::lattice;

// ─── Helpers ──────────────────────────────────────────────────────────────────

/// Generate N bytes of pseudo-random data (high entropy).
static std::vector<uint8_t> randomBytes(size_t n, uint64_t seed = 42)
{
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> dist(0, 255);
    std::vector<uint8_t> buf(n);
    for (auto &b : buf) b = static_cast<uint8_t>(dist(rng));
    return buf;
}

/// Generate N bytes of a single repeated value (low entropy).
static std::vector<uint8_t> zeroBytes(size_t n)
{
    return std::vector<uint8_t>(n, 0x00);
}

/// Generate N bytes of readable ASCII text (low–medium entropy).
static std::vector<uint8_t> asciiBytes(size_t n)
{
    std::vector<uint8_t> buf(n);
    static const char alpha[] = "Hello world! This is a test string with readable text.";
    size_t al = std::strlen(alpha);
    for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(alpha[i % al]);
    return buf;
}

// ─── EntropyProfiler tests ────────────────────────────────────────────────────

TEST(EntropyProfilerTest, EmptyInput)
{
    EntropyProfiler p;
    auto prof = p.profile(nullptr, 0);
    EXPECT_EQ(prof.blocks.size(), 0u);
    EXPECT_DOUBLE_EQ(prof.highEntropyFraction, 0.0);
    EXPECT_DOUBLE_EQ(prof.averageEntropy, 0.0);
}

TEST(EntropyProfilerTest, AllZerosHaveZeroEntropy)
{
    auto data = zeroBytes(1024);
    EntropyProfiler p;
    auto prof = p.profile(data.data(), data.size());
    EXPECT_EQ(prof.blocks.size(), 4u); // 1024 / 256 = 4
    for (const auto &b : prof.blocks) {
        EXPECT_DOUBLE_EQ(b.entropy, 0.0);
        EXPECT_TRUE(b.isLow);
        EXPECT_FALSE(b.isHigh);
    }
    EXPECT_DOUBLE_EQ(prof.highEntropyFraction, 0.0);
    EXPECT_NEAR(prof.lowEntropyFraction, 1.0, 1e-9);
}

TEST(EntropyProfilerTest, RandomDataHasHighEntropy)
{
    auto data = randomBytes(2048);
    EntropyProfiler p;
    auto prof = p.profile(data.data(), data.size());
    EXPECT_GT(prof.averageEntropy, 7.0);
    EXPECT_GT(prof.highEntropyFraction, 0.9);
}

TEST(EntropyProfilerTest, BlockEntropyBounds)
{
    auto zeros = zeroBytes(256);
    EXPECT_DOUBLE_EQ(EntropyProfiler::blockEntropy(zeros.data(), zeros.size()), 0.0);

    auto rnd = randomBytes(256);
    double e = EntropyProfiler::blockEntropy(rnd.data(), rnd.size());
    EXPECT_GE(e, 0.0);
    EXPECT_LE(e, 8.0);
}

TEST(EntropyProfilerTest, AsciiLowEntropy)
{
    auto data = asciiBytes(512);
    EntropyProfiler p;
    auto prof = p.profile(data.data(), data.size());
    EXPECT_LT(prof.averageEntropy, kHighEntropyThreshold);
    EXPECT_DOUBLE_EQ(prof.highEntropyFraction, 0.0);
}

TEST(EntropyProfilerTest, SectionsBuiltCorrectly)
{
    // First 1024 bytes: zeros (low entropy); next 1024: random (high entropy)
    auto data = zeroBytes(1024);
    auto rnd  = randomBytes(1024);
    data.insert(data.end(), rnd.begin(), rnd.end());

    EntropyProfiler p;
    auto prof = p.profile(data.data(), data.size());

    // Should have at least 2 sections: low then high
    ASSERT_GE(prof.sections.size(), 2u);
    bool foundLow  = false, foundHigh = false;
    for (const auto &s : prof.sections) {
        if (s.isLow)  foundLow  = true;
        if (s.isHigh) foundHigh = true;
    }
    EXPECT_TRUE(foundLow);
    EXPECT_TRUE(foundHigh);
}

TEST(EntropyProfilerTest, PartialLastBlock)
{
    // 300 bytes (1 full block + 44-byte partial)
    auto data = randomBytes(300);
    EntropyProfiler p;
    auto prof = p.profile(data.data(), data.size());
    EXPECT_EQ(prof.blocks.size(), 2u);
}

// ─── PackerDetector signal tests ─────────────────────────────────────────────

class PackerDetectorTest : public ::testing::Test {
protected:
    PackerDetector detector;
};

// ── Signal 4: ImportSparsitySignal ─────────────────────────────────────────

TEST_F(PackerDetectorTest, NoImportsMeansHighSparsityScore)
{
    // No format result → sparsity = 0 (no info)
    auto r = detector.detect(nullptr, 0, nullptr);
    EXPECT_DOUBLE_EQ(r.signals.importSparsityScore, 0.0);

    // Format result with 0 imports → score = 1.0
    FormatResult fmt;
    auto data = randomBytes(512);
    auto r2   = detector.detect(data.data(), data.size(), &fmt);
    EXPECT_DOUBLE_EQ(r2.signals.importSparsityScore, 1.0);
}

TEST_F(PackerDetectorTest, ManyImportsMeansLowSparsityScore)
{
    FormatResult fmt;
    for (int i = 0; i < 20; ++i) {
        ImportEntry ie;
        ie.functionName = "func" + std::to_string(i);
        fmt.imports.push_back(ie);
    }
    auto data = asciiBytes(512);
    auto r    = detector.detect(data.data(), data.size(), &fmt);
    EXPECT_DOUBLE_EQ(r.signals.importSparsityScore, 0.0);
}

// ── Signal 1: EntropySectionSignal ─────────────────────────────────────────

TEST_F(PackerDetectorTest, HighEntropyFileScoresHighEntropySectionSignal)
{
    auto data = randomBytes(4096);
    auto r    = detector.detect(data.data(), data.size(), nullptr);
    EXPECT_GT(r.signals.entropySectionScore, 0.5);
}

TEST_F(PackerDetectorTest, LowEntropyFileScoresZeroEntropySectionSignal)
{
    auto data = zeroBytes(4096);
    auto r    = detector.detect(data.data(), data.size(), nullptr);
    EXPECT_DOUBLE_EQ(r.signals.entropySectionScore, 0.0);
}

TEST_F(PackerDetectorTest, AsciiFileScoresZeroEntropySectionSignal)
{
    auto data = asciiBytes(4096);
    auto r    = detector.detect(data.data(), data.size(), nullptr);
    EXPECT_DOUBLE_EQ(r.signals.entropySectionScore, 0.0);
}

// ── Signal 3: EntryPointSignal ────────────────────────────────────────────

TEST_F(PackerDetectorTest, EPInTextSectionScoresZero)
{
    FormatResult fmt;
    SectionInfo sec;
    sec.name           = ".text";
    sec.virtualAddress = 0x1000;
    sec.virtualSize    = 0x1000;
    sec.isExecutable   = true;
    sec.isWritable     = false;
    fmt.sections.push_back(sec);
    fmt.entryPoint = 0x1200;

    auto data = asciiBytes(512);
    auto r    = detector.detect(data.data(), data.size(), &fmt);
    EXPECT_DOUBLE_EQ(r.signals.entryPointScore, 0.0);
}

TEST_F(PackerDetectorTest, EPInWritableExecSectionScoresHigh)
{
    FormatResult fmt;
    SectionInfo sec;
    sec.name           = ".pack";
    sec.virtualAddress = 0x1000;
    sec.virtualSize    = 0x8000;
    sec.isExecutable   = true;
    sec.isWritable     = true; // W+X
    fmt.sections.push_back(sec);
    fmt.entryPoint = 0x2000;

    auto data = randomBytes(512);
    auto r    = detector.detect(data.data(), data.size(), &fmt);
    EXPECT_DOUBLE_EQ(r.signals.entryPointScore, 1.0);
}

TEST_F(PackerDetectorTest, EPInUnknownSectionScoresMedium)
{
    FormatResult fmt;
    SectionInfo sec;
    sec.name           = ".stub";
    sec.virtualAddress = 0x5000;
    sec.virtualSize    = 0x1000;
    sec.isExecutable   = true;
    sec.isWritable     = false;
    fmt.sections.push_back(sec);
    fmt.entryPoint = 0x5100;

    auto data = randomBytes(512);
    auto r    = detector.detect(data.data(), data.size(), &fmt);
    EXPECT_GT(r.signals.entryPointScore, 0.0);
    EXPECT_LT(r.signals.entryPointScore, 1.0);
}

// ── Overall packing decision ──────────────────────────────────────────────

TEST_F(PackerDetectorTest, HighEntropyFileDetectedAsPacked)
{
    // Simulate a fully encrypted/compressed binary:
    //  - random bytes (high entropy)
    //  - no imports
    //  - EP in W+X section
    auto data = randomBytes(8192);
    FormatResult fmt;
    SectionInfo sec;
    sec.name           = ".packed";
    sec.virtualAddress = 0x1000;
    sec.virtualSize    = 0x2000;
    sec.isExecutable   = true;
    sec.isWritable     = true;
    fmt.sections.push_back(sec);
    fmt.entryPoint = 0x1000;
    // No imports

    auto r = detector.detect(data.data(), data.size(), &fmt);
    EXPECT_TRUE(r.isPacked);
    EXPECT_GE(r.confidence, 0.6);
}

TEST_F(PackerDetectorTest, CleanFileNotDetectedAsPacked)
{
    // Simulate a normal binary:
    //  - ASCII/text data (low entropy)
    //  - many imports
    //  - EP in .text
    auto data = asciiBytes(8192);
    FormatResult fmt;
    SectionInfo sec;
    sec.name           = ".text";
    sec.virtualAddress = 0x1000;
    sec.virtualSize    = 0x4000;
    sec.isExecutable   = true;
    sec.isWritable     = false;
    fmt.sections.push_back(sec);
    fmt.entryPoint = 0x1000;
    for (int i = 0; i < 20; ++i) {
        ImportEntry ie;
        ie.functionName = "func" + std::to_string(i);
        fmt.imports.push_back(ie);
    }

    auto r = detector.detect(data.data(), data.size(), &fmt);
    EXPECT_FALSE(r.isPacked);
    EXPECT_LT(r.confidence, 0.6);
}

// ── UPX detection ─────────────────────────────────────────────────────────

TEST_F(PackerDetectorTest, UPXSectionNameDetected)
{
    auto data = randomBytes(4096);
    FormatResult fmt;
    SectionInfo sec1;
    sec1.name           = "UPX0";
    sec1.virtualAddress = 0x1000;
    sec1.virtualSize    = 0x4000;
    sec1.isExecutable   = true;
    sec1.isWritable     = true;
    fmt.sections.push_back(sec1);
    SectionInfo sec2;
    sec2.name           = "UPX1";
    sec2.virtualAddress = 0x5000;
    sec2.virtualSize    = 0x1000;
    fmt.sections.push_back(sec2);
    fmt.entryPoint = 0x1000;

    auto r = detector.detect(data.data(), data.size(), &fmt);
    if (r.isPacked) {
        EXPECT_EQ(r.likelyFamily, PackerFamily::UPX);
        EXPECT_EQ(r.familyName,   "UPX");
    }
}

// ── MPRESS detection ──────────────────────────────────────────────────────

TEST_F(PackerDetectorTest, MPRESSSectionNameDetected)
{
    auto data = randomBytes(4096);
    FormatResult fmt;
    SectionInfo sec;
    sec.name           = "MPRESS1";
    sec.virtualAddress = 0x1000;
    sec.virtualSize    = 0x2000;
    sec.isExecutable   = true;
    sec.isWritable     = true;
    fmt.sections.push_back(sec);
    fmt.entryPoint = 0x1000;

    auto r = detector.detect(data.data(), data.size(), &fmt);
    if (r.isPacked) {
        EXPECT_EQ(r.likelyFamily, PackerFamily::MPRESS);
    }
}

// ── EntropyProfile in result ───────────────────────────────────────────────

TEST_F(PackerDetectorTest, ResultContainsEntropyProfile)
{
    auto data = randomBytes(1024);
    auto r    = detector.detect(data.data(), data.size(), nullptr);
    EXPECT_FALSE(r.entropyProfile.blocks.empty());
    EXPECT_EQ(r.entropyProfile.fileSize, 1024u);
}

// ── Weighted sum sanity ───────────────────────────────────────────────────

TEST_F(PackerDetectorTest, WeightedSumBounds)
{
    SignalScores s;
    s.entropySectionScore  = 0.5;
    s.sectionMismatchScore = 0.5;
    s.entryPointScore      = 0.5;
    s.importSparsityScore  = 0.5;
    EXPECT_NEAR(s.combined(), 0.5, 1e-9);

    s.entropySectionScore  = 1.0;
    s.sectionMismatchScore = 1.0;
    s.entryPointScore      = 1.0;
    s.importSparsityScore  = 1.0;
    EXPECT_NEAR(s.combined(), 1.0, 1e-9);

    s.entropySectionScore  = 0.0;
    s.sectionMismatchScore = 0.0;
    s.entryPointScore      = 0.0;
    s.importSparsityScore  = 0.0;
    EXPECT_NEAR(s.combined(), 0.0, 1e-9);
}
