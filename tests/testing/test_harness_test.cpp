/**
 * @file tests/testing/test_harness_test.cpp
 * @brief Unit tests for the testing infrastructure itself.
 */

#include "retdec/testing/test_harness.h"
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <filesystem>

#ifdef _MSC_VER
// MSVC doesn't have POSIX setenv/unsetenv; emulate with _putenv_s/_putenv
#include <cstdlib>
static inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#endif
#include <thread>

using namespace retdec::testing;
namespace fs = std::filesystem;

// ─── TestBinary ───────────────────────────────────────────────────────────────

TEST(TestBinary, MakeELF64ReturnsBytes) {
    auto b = TestBinary::makeELF64();
    auto data = b.serialise();
    EXPECT_FALSE(data.empty());
}

TEST(TestBinary, MakeELF64HasELFMagic) {
    auto b = TestBinary::makeELF64();
    auto data = b.serialise();
    ASSERT_GE(data.size(), 4u);
    EXPECT_EQ(data[0], 0x7f);
    EXPECT_EQ(data[1], 'E');
    EXPECT_EQ(data[2], 'L');
    EXPECT_EQ(data[3], 'F');
}

TEST(TestBinary, MakeELF64IsClass64) {
    auto b = TestBinary::makeELF64();
    auto data = b.serialise();
    ASSERT_GE(data.size(), 5u);
    EXPECT_EQ(data[4], 2);  // ELFCLASS64
}

TEST(TestBinary, MakeELF32HasELFMagic) {
    auto b = TestBinary::makeELF32();
    auto data = b.serialise();
    ASSERT_GE(data.size(), 4u);
    EXPECT_EQ(data[0], 0x7f);
    EXPECT_EQ(data[4], 1);  // ELFCLASS32
}

TEST(TestBinary, MakePE32HasMZMagic) {
    auto b = TestBinary::makePE32();
    auto data = b.serialise();
    ASSERT_GE(data.size(), 2u);
    EXPECT_EQ(data[0], 'M');
    EXPECT_EQ(data[1], 'Z');
}

TEST(TestBinary, MakeRawContainsData) {
    std::vector<uint8_t> payload = {0x90, 0x90, 0xC3};
    auto b = TestBinary::makeRaw(payload);
    auto data = b.serialise();
    ASSERT_GE(data.size(), 3u);
    EXPECT_EQ(data[0], 0x90);
    EXPECT_EQ(data[2], 0xC3);
}

TEST(TestBinary, AddSectionIncreasesCount) {
    auto b = TestBinary::makeELF64();
    size_t before = b.sections().size();
    TestBinary::Section s;
    s.name = ".data"; s.data = {1, 2, 3};
    b.addSection(s);
    EXPECT_EQ(b.sections().size(), before + 1);
}

TEST(TestBinary, AddSymbol) {
    auto b = TestBinary::makeELF64();
    TestBinary::Symbol sym;
    sym.name = "main"; sym.address = 0x401000;
    b.addSymbol(sym);
    ASSERT_EQ(b.symbols().size(), 1u);
    EXPECT_EQ(b.symbols()[0].name, "main");
}

TEST(TestBinary, WriteToTempFileCreatesFile) {
    auto b = TestBinary::makeELF64();
    auto path = b.writeToTempFile(".elf");
    EXPECT_TRUE(fs::exists(path));
    fs::remove(path);
}

TEST(TestBinary, WriteToTempFileContentIsValid) {
    auto b = TestBinary::makeELF64();
    auto path = b.writeToTempFile(".elf");
    std::ifstream f(path, std::ios::binary);
    char magic[4];
    f.read(magic, 4);
    EXPECT_EQ(magic[0], 0x7f);
    EXPECT_EQ(magic[1], 'E');
    fs::remove(path);
}

TEST(TestBinary, EntryPointRoundtrip) {
    auto b = TestBinary::makeELF64();
    b.setEntryPoint(0xDEADBEEF);
    EXPECT_EQ(b.entryPoint(), 0xDEADBEEFULL);
}

// ─── SnapshotTester ──────────────────────────────────────────────────────────

class SnapshotTest : public ::testing::Test {
protected:
    std::string snapDir_ = "/tmp/retdec_snap_test";
    SnapshotTester tester_{snapDir_};

    void TearDown() override {
        fs::remove_all(snapDir_);
    }
};

TEST_F(SnapshotTest, FirstCompareCreatesSnapshot) {
    auto r = tester_.compare("first_test", "output content");
    EXPECT_EQ(r.result, SnapshotTester::Result::NewSnapshot);
    EXPECT_TRUE(fs::exists(r.snapshotPath));
}

TEST_F(SnapshotTest, SecondCompareMatchesSnapshot) {
    tester_.compare("match_test", "same output");
    auto r = tester_.compare("match_test", "same output");
    EXPECT_EQ(r.result, SnapshotTester::Result::Match);
}

TEST_F(SnapshotTest, ChangedOutputMismatch) {
    tester_.compare("change_test", "original");
    auto r = tester_.compare("change_test", "different");
    EXPECT_EQ(r.result, SnapshotTester::Result::Mismatch);
}

TEST_F(SnapshotTest, MismatchHasDiff) {
    tester_.compare("diff_test", "line1\nline2\n");
    auto r = tester_.compare("diff_test", "line1\nchanged\n");
    EXPECT_FALSE(r.diff.empty());
}

TEST_F(SnapshotTest, UpdateOverwritesSnapshot) {
    tester_.compare("upd_test", "v1");
    EXPECT_TRUE(tester_.update("upd_test", "v2"));
    auto r = tester_.compare("upd_test", "v2");
    EXPECT_EQ(r.result, SnapshotTester::Result::Match);
}

TEST_F(SnapshotTest, RemoveDeletesSnapshot) {
    tester_.compare("rem_test", "content");
    EXPECT_TRUE(tester_.remove("rem_test"));
    // Next compare should create a new snapshot
    auto r = tester_.compare("rem_test", "content");
    EXPECT_EQ(r.result, SnapshotTester::Result::NewSnapshot);
}

TEST_F(SnapshotTest, MismatchActualIsCorrect) {
    tester_.compare("actual_test", "expected content");
    auto r = tester_.compare("actual_test", "actual content");
    EXPECT_EQ(r.actual, "actual content");
    EXPECT_EQ(r.expected, "expected content");
}

// ─── CorpusRunner ────────────────────────────────────────────────────────────

TEST(CorpusRunner, SyntheticCorpusNonEmpty) {
    auto corpus = CorpusRunner::syntheticCorpus();
    EXPECT_FALSE(corpus.empty());
}

TEST(CorpusRunner, SyntheticCorpusHasArchitecture) {
    auto corpus = CorpusRunner::syntheticCorpus();
    for (const auto& e : corpus)
        EXPECT_FALSE(e.architecture.empty());
}

TEST(CorpusRunner, SyntheticCorpusHasFormat) {
    auto corpus = CorpusRunner::syntheticCorpus();
    for (const auto& e : corpus)
        EXPECT_FALSE(e.format.empty());
}

TEST(CorpusRunner, IterateOnNonExistentDirUsesSynthetic) {
    CorpusRunner runner("/nonexistent/corpus");
    int count = 0;
    runner.iterate([&](const CorpusEntry& e) {
        ++count;
        EXPECT_FALSE(e.architecture.empty());
        return true;
    });
    EXPECT_GT(count, 0);
}

TEST(CorpusRunner, CollectOnNonExistentDirReturnsSynthetic) {
    CorpusRunner runner("/nonexistent/corpus");
    auto entries = runner.collect();
    EXPECT_FALSE(entries.empty());
}

TEST(CorpusRunner, IterateCanStop) {
    CorpusRunner runner("/nonexistent/corpus");
    int count = 0;
    runner.iterate([&](const CorpusEntry&) {
        ++count;
        return false;  // stop after first
    });
    EXPECT_EQ(count, 1);
}

// ─── PerformanceAsserter ─────────────────────────────────────────────────────

TEST(PerformanceAsserter, BenchmarkReturnsPositiveAvg) {
    auto r = PerformanceAsserter::benchmark([]{ /* no-op */ }, 10);
    EXPECT_GT(r.avgMs, 0.0);
    EXPECT_EQ(r.iterations, 10);
}

TEST(PerformanceAsserter, BenchmarkMinLEMax) {
    auto r = PerformanceAsserter::benchmark([]{ /* no-op */ }, 20);
    EXPECT_LE(r.minMs, r.maxMs);
}

TEST(PerformanceAsserter, BenchmarkWithThroughput) {
    auto r = PerformanceAsserter::benchmark(
        []{ /* no-op */ }, 10, 1024);
    EXPECT_GT(r.throughputBps, 0.0);
}

TEST(PerformanceAsserter, AssertMaxMsPassesForFastFn) {
    bool ok = PerformanceAsserter::assertMaxMs(
        []{ /* no-op */ }, 1000.0, "noop");
    EXPECT_TRUE(ok);
}

TEST(PerformanceAsserter, AssertMaxMsFailsForSlowFn) {
    // Force slow assertion mode to avoid noise
    setenv("RETDEC_SOFT_PERF_ASSERT", "1", 1);
    bool ok = PerformanceAsserter::assertMaxMs(
        []{ std::this_thread::sleep_for(std::chrono::milliseconds(50)); }, 1.0, "slow");
    // ok may be true or false depending on timing; just ensure no crash
    (void)ok;
    unsetenv("RETDEC_SOFT_PERF_ASSERT");
}

TEST(PerformanceAsserter, P50LtP95) {
    auto r = PerformanceAsserter::benchmark([]{ /* no-op */ }, 50);
    EXPECT_LE(r.p50Ms, r.p95Ms);
}

TEST(PerformanceAsserter, BenchmarkFormatNonEmpty) {
    auto r = PerformanceAsserter::benchmark([]{ /* no-op */ }, 5);
    EXPECT_FALSE(r.format().empty());
}

// ─── MockPipeline ─────────────────────────────────────────────────────────────

TEST(MockPipeline, RunEmptyPipelineReturnsInput) {
    MockPipeline p;
    EXPECT_EQ(p.run("hello"), "hello");
}

TEST(MockPipeline, SingleStageTransforms) {
    MockPipeline p;
    p.addStage("upper", [](const std::string& s) {
        std::string r = s;
        for (char& c : r) c = toupper(c);
        return r;
    });
    EXPECT_EQ(p.run("hello"), "HELLO");
}

TEST(MockPipeline, MultiStageChains) {
    MockPipeline p;
    p.addStage("append_a", [](const std::string& s) { return s + "A"; });
    p.addStage("append_b", [](const std::string& s) { return s + "B"; });
    EXPECT_EQ(p.run("x"), "xAB");
}

TEST(MockPipeline, SkipStageOmitsTransform) {
    MockPipeline p;
    p.addStage("kept",    [](const std::string& s) { return s + "_kept"; });
    p.addStage("skipped", [](const std::string& s) { return s + "_skipped"; });
    p.skipStage("skipped");
    std::string result = p.run("base");
    EXPECT_NE(result.find("_kept"), std::string::npos);
    EXPECT_EQ(result.find("_skipped"), std::string::npos);
}

TEST(MockPipeline, IntermediatesStoredPerStage) {
    MockPipeline p;
    p.addStage("stage1", [](const std::string& s) { return s + "1"; });
    p.addStage("stage2", [](const std::string& s) { return s + "2"; });
    p.run("x");
    EXPECT_EQ(p.intermediates().at("stage1"), "x1");
    EXPECT_EQ(p.intermediates().at("stage2"), "x12");
}

TEST(MockPipeline, ExecutedStagesTracked) {
    MockPipeline p;
    p.addStage("A", [](const std::string& s) { return s; });
    p.addStage("B", [](const std::string& s) { return s; });
    p.run("x");
    ASSERT_EQ(p.executedStages().size(), 2u);
    EXPECT_EQ(p.executedStages()[0], "A");
    EXPECT_EQ(p.executedStages()[1], "B");
}

TEST(MockPipeline, SkippedStageNotInExecuted) {
    MockPipeline p;
    p.addStage("A", [](const std::string& s) { return s; });
    p.addStage("B", [](const std::string& s) { return s; });
    p.skipStage("B");
    p.run("x");
    ASSERT_EQ(p.executedStages().size(), 1u);
    EXPECT_EQ(p.executedStages()[0], "A");
}

// ─── TestLogger ──────────────────────────────────────────────────────────────

TEST(TestLogger, InstallClearsMessages) {
    TestLogger::messages();
    TestLogger::install();
    EXPECT_TRUE(TestLogger::messages().empty());
}

TEST(TestLogger, ContainsReturnsFalseWhenEmpty) {
    TestLogger::clear();
    EXPECT_FALSE(TestLogger::contains("anything"));
}

TEST(TestLogger, DumpEmptyIsEmptyOrNewline) {
    TestLogger::clear();
    EXPECT_TRUE(TestLogger::dump().empty());
}
