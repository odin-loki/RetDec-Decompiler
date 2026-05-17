/**
 * @file include/retdec/testing/test_harness.h
 * @brief Testing Infrastructure — Stage 59.
 *
 * Provides helpers and base classes used across all RetDec test suites:
 *
 *   TestBinary         — in-memory ELF/PE stub builder for unit tests
 *   SnapshotTester     — regression snapshot comparison (hash + diff)
 *   CorpusRunner       — iterate over test corpus binaries
 *   PerformanceAsserter — assert throughput/timing within bounds
 *   MockPipeline       — lightweight pipeline stub for integration tests
 *   TestLogger         — captures log output for assertion
 *
 * ## Snapshot testing
 *
 *   Snapshot files live in tests/snapshots/<testname>.snap.
 *   First run (or RETDEC_UPDATE_SNAPSHOTS=1): write the snapshot.
 *   Subsequent runs: compare; fail if hash differs and show a diff.
 *
 * ## Corpus runner
 *
 *   CorpusRunner::iterate(dir, ext) yields pairs (path, expectedOutput).
 *   Expected output is loaded from path + ".expected" if it exists,
 *   otherwise the test is "speculative" (no assertion on output).
 *
 * ## Performance assertions
 *
 *   PerformanceAsserter::assertThroughput(fn, minBytesPerSecond)
 *   PerformanceAsserter::assertMaxMs(fn, maxMs)
 *   PerformanceAsserter::benchmark(fn, iterations) → BenchmarkResult
 */

#ifndef RETDEC_TESTING_TEST_HARNESS_H
#define RETDEC_TESTING_TEST_HARNESS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

namespace retdec::testing {

// ─── TestBinary ───────────────────────────────────────────────────────────────

/**
 * @brief Minimal ELF/PE binary stub builder for unit testing without real files.
 *
 * Builds a syntactically valid but semantically trivial binary that can be
 * parsed by RetDec's loaders without crashing.  Not a real executable.
 */
class TestBinary {
public:
    enum class Format { ELF32, ELF64, PE32, PE64, Raw };

    struct Section {
        std::string          name;
        uint64_t             virtualAddr = 0;
        std::vector<uint8_t> data;
        bool                 executable  = false;
        bool                 writeable   = false;
    };

    struct Symbol {
        std::string name;
        uint64_t    address = 0;
        bool        isFunc  = true;
    };

    /**
     * @brief Create a stub binary in memory.
     */
    static TestBinary makeELF64(
        const std::vector<uint8_t>& textSection = {},
        const std::vector<Symbol>&  symbols     = {});

    static TestBinary makeELF32(
        const std::vector<uint8_t>& textSection = {},
        const std::vector<Symbol>&  symbols     = {});

    static TestBinary makePE32(
        const std::vector<uint8_t>& textSection = {});

    /**
     * @brief Raw binary (no header, just bytes).
     */
    static TestBinary makeRaw(const std::vector<uint8_t>& data,
                               uint64_t baseAddr = 0x1000);

    /**
     * @brief Add a section.
     */
    TestBinary& addSection(const Section& section);

    /**
     * @brief Add a symbol.
     */
    TestBinary& addSymbol(const Symbol& sym);

    /**
     * @brief Serialise to bytes.
     */
    std::vector<uint8_t> serialise() const;

    /**
     * @brief Write to a temporary file and return the path.
     */
    std::string writeToTempFile(const std::string& suffix = ".elf") const;

    const std::vector<Section>& sections() const { return sections_; }
    const std::vector<Symbol>&  symbols()  const { return symbols_; }
    Format format() const { return format_; }
    uint64_t entryPoint() const { return entryPoint_; }
    void setEntryPoint(uint64_t addr) { entryPoint_ = addr; }

private:
    Format                 format_     = Format::Raw;
    uint64_t               entryPoint_ = 0x1000;
    std::vector<Section>   sections_;
    std::vector<Symbol>    symbols_;

    std::vector<uint8_t> serialiseELF64() const;
    std::vector<uint8_t> serialiseELF32() const;
    std::vector<uint8_t> serialisePE32()  const;
    std::vector<uint8_t> serialiseRaw()   const;
};

// ─── SnapshotTester ──────────────────────────────────────────────────────────

/**
 * @brief Regression snapshot testing helper.
 *
 * Compares the given output against a stored snapshot file.
 * If RETDEC_UPDATE_SNAPSHOTS=1, updates the snapshot instead of failing.
 */
class SnapshotTester {
public:
    explicit SnapshotTester(const std::string& snapshotDir = "tests/snapshots");

    enum class Result { Match, Mismatch, NewSnapshot, Error };

    struct CompareResult {
        Result      result = Result::Error;
        std::string diff;     ///< unified diff if mismatched
        std::string expected; ///< content of the snapshot
        std::string actual;   ///< content being tested
        std::string snapshotPath;
    };

    /**
     * @brief Compare output against a named snapshot.
     * @param testName  Snapshot filename (without extension).
     * @param output    The actual output to compare.
     */
    CompareResult compare(const std::string& testName,
                           const std::string& output);

    /**
     * @brief Update the named snapshot with the given content.
     */
    bool update(const std::string& testName, const std::string& content);

    /**
     * @brief Delete a snapshot (for cleanup in tests).
     */
    bool remove(const std::string& testName);

    /**
     * @brief Returns true if RETDEC_UPDATE_SNAPSHOTS is set.
     */
    static bool isUpdateMode();

private:
    std::string snapshotDir_;
    std::string snapshotPath(const std::string& testName) const;

    static std::string computeHash(const std::string& content);
    static std::string makeDiff  (const std::string& expected,
                                   const std::string& actual);
};

// ─── CorpusEntry ─────────────────────────────────────────────────────────────

struct CorpusEntry {
    std::string binaryPath;
    std::string expectedOutputPath;  ///< empty if no expected output
    std::string architecture;        ///< "x86", "x86_64", "arm", etc.
    std::string format;              ///< "elf", "pe", "macho"
    std::string notes;
    bool        hasExpected = false;
};

// ─── CorpusRunner ─────────────────────────────────────────────────────────────

/**
 * @brief Iterates over a test corpus directory.
 *
 * Expected directory layout:
 *   tests/corpus/
 *     elf_x86_64/
 *       hello_gcc_O0.elf
 *       hello_gcc_O0.elf.expected   ← expected decompiled output (optional)
 *     pe_x86/
 *       calc.exe
 *     ...
 */
class CorpusRunner {
public:
    explicit CorpusRunner(const std::string& corpusDir = "tests/corpus");

    /**
     * @brief Collect all corpus entries matching the given extension.
     */
    std::vector<CorpusEntry> collect(const std::string& ext = ".elf") const;

    /**
     * @brief Iterate and call fn for each entry.
     * @param fn  Called with each CorpusEntry.  Return false to stop.
     */
    void iterate(const std::function<bool(const CorpusEntry&)>& fn,
                  const std::string& ext = "") const;

    /**
     * @brief Returns a synthetic corpus of entries built from TestBinary stubs.
     *        Useful when the corpus directory does not exist (CI environment).
     */
    static std::vector<CorpusEntry> syntheticCorpus();

private:
    std::string corpusDir_;
};

// ─── BenchmarkResult ─────────────────────────────────────────────────────────

struct BenchmarkResult {
    double  totalMs       = 0.0;
    double  minMs         = 0.0;
    double  maxMs         = 0.0;
    double  avgMs         = 0.0;
    double  p50Ms         = 0.0;
    double  p95Ms         = 0.0;
    double  p99Ms         = 0.0;
    int64_t iterations    = 0;
    double  throughputBps = 0.0;  ///< bytes/s if inputBytes set

    std::string format() const;
};

// ─── PerformanceAsserter ─────────────────────────────────────────────────────

/**
 * @brief Wraps performance assertions for use in test cases.
 *
 * All assertions are soft by default (print warning instead of FAIL) when
 * RETDEC_SOFT_PERF_ASSERT=1 is set (useful in CI where timing is noisy).
 */
class PerformanceAsserter {
public:
    /**
     * @brief Run fn and assert it completes within maxMs milliseconds.
     */
    static bool assertMaxMs(const std::function<void()>& fn,
                             double maxMs,
                             const std::string& description = "");

    /**
     * @brief Run fn with inputBytes bytes of work and assert throughput
     *        is at least minBytesPerSecond.
     */
    static bool assertThroughput(const std::function<void()>& fn,
                                  int64_t inputBytes,
                                  double  minBytesPerSecond,
                                  const std::string& description = "");

    /**
     * @brief Benchmark fn over N iterations and return statistics.
     */
    static BenchmarkResult benchmark(const std::function<void()>& fn,
                                      int64_t  iterations    = 100,
                                      int64_t  inputBytes    = 0,
                                      const std::string& name = "");

    /**
     * @brief Returns true if soft-assertion mode is active.
     */
    static bool isSoftMode();
};

// ─── MockPipeline ─────────────────────────────────────────────────────────────

/**
 * @brief Lightweight mock pipeline for integration tests.
 *
 * Allows injecting test data at any stage and observing the output.
 */
class MockPipeline {
public:
    using StageHandler = std::function<std::string(const std::string& input)>;

    MockPipeline& addStage(const std::string& name, StageHandler handler);
    MockPipeline& skipStage(const std::string& name);

    /**
     * @brief Run the pipeline with the given input.
     */
    std::string run(const std::string& input);

    /**
     * @brief Returns intermediate output after each stage.
     */
    const std::unordered_map<std::string, std::string>& intermediates() const {
        return intermediates_;
    }

    /**
     * @brief List of stages that were actually executed.
     */
    const std::vector<std::string>& executedStages() const {
        return executedStages_;
    }

private:
    struct Stage {
        std::string  name;
        StageHandler handler;
        bool         skip = false;
    };
    std::vector<Stage>                            stages_;
    std::unordered_map<std::string, std::string>  intermediates_;
    std::vector<std::string>                      executedStages_;
};

// ─── TestLogger ──────────────────────────────────────────────────────────────

/**
 * @brief Captures log output for test assertions.
 *
 * Install with TestLogger::install(); restore with TestLogger::uninstall().
 * Call contains(msg) to check if a message was logged.
 */
class TestLogger {
public:
    static void install();
    static void uninstall();
    static void clear();
    static bool contains(const std::string& substring);
    static const std::vector<std::string>& messages();
    static std::string dump();

private:
    static std::vector<std::string> messages_;
};

} // namespace retdec::testing

#endif // RETDEC_TESTING_TEST_HARNESS_H
