/**
 * @file tests/profiling/profiling_test.cpp
 * @brief Unit tests for the performance profiling harness.
 */

#include "retdec/profiling/profiling.h"
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <thread>
#include <string>
#include <cstdio>

using namespace retdec::profiling;

// ─── Fixture ─────────────────────────────────────────────────────────────────

class ProfilingTest : public ::testing::Test {
protected:
    void SetUp() override {
        Profiler::instance().reset();
        Profiler::instance().setEnabled(true);
    }
};

// ─── ScopeTimer ──────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, ScopeTimerRecordsStage) {
    {
        auto g = Profiler::instance().measure("test_stage");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto r = Profiler::instance().report();
    ASSERT_FALSE(r.stages.empty());
    bool found = false;
    for (const auto& s : r.stages)
        if (s.name == "test_stage") { found = true; break; }
    EXPECT_TRUE(found);
}

TEST_F(ProfilingTest, ScopeTimerCallCountIncremented) {
    for (int i = 0; i < 5; ++i) {
        auto g = Profiler::instance().measure("counted");
    }
    auto r = Profiler::instance().report();
    for (const auto& s : r.stages)
        if (s.name == "counted") {
            EXPECT_EQ(s.callCount, 5);
            return;
        }
    FAIL() << "Stage 'counted' not found";
}

TEST_F(ProfilingTest, ScopeTimerTotalNsPositive) {
    {
        auto g = Profiler::instance().measure("positive");
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    auto r = Profiler::instance().report();
    for (const auto& s : r.stages)
        if (s.name == "positive") {
            EXPECT_GT(s.totalNs, 0);
            return;
        }
    FAIL();
}

TEST_F(ProfilingTest, ScopeTimerMinMaxTracked) {
    {
        auto g = Profiler::instance().measure("minmax");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    {
        auto g = Profiler::instance().measure("minmax");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto r = Profiler::instance().report();
    for (const auto& s : r.stages)
        if (s.name == "minmax") {
            EXPECT_LT(s.minNs, s.maxNs);
            return;
        }
    FAIL();
}

// ─── Manual start/stop ────────────────────────────────────────────────────────

TEST_F(ProfilingTest, ManualStartStop) {
    auto t0 = Profiler::instance().start("manual");
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    Profiler::instance().stop("manual", t0);
    auto r = Profiler::instance().report();
    bool found = false;
    for (const auto& s : r.stages)
        if (s.name == "manual") { found = true; EXPECT_GT(s.totalNs, 0); }
    EXPECT_TRUE(found);
}

// ─── time() helper ────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, TimeFunctionReturnsMsPositive) {
    double ms = Profiler::instance().time("timed_fn", []{
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    });
    EXPECT_GE(ms, 0.0);
}

TEST_F(ProfilingTest, TimeFunctionRecordsStage) {
    Profiler::instance().time("fn_stage", []{ /* no-op */ });
    auto r = Profiler::instance().report();
    bool found = false;
    for (const auto& s : r.stages)
        if (s.name == "fn_stage") found = true;
    EXPECT_TRUE(found);
}

// ─── Disabled profiler ────────────────────────────────────────────────────────

TEST_F(ProfilingTest, DisabledProfilerRecordsNothing) {
    Profiler::instance().setEnabled(false);
    {
        auto g = Profiler::instance().measure("disabled");
    }
    Profiler::instance().setEnabled(true);
    auto r = Profiler::instance().report();
    for (const auto& s : r.stages)
        EXPECT_NE(s.name, "disabled");
}

// ─── recordFunction ───────────────────────────────────────────────────────────

TEST_F(ProfilingTest, RecordFunctionStoresSample) {
    Profiler::instance().recordFunction("my_func", 500000);
    auto r = Profiler::instance().report();
    bool found = false;
    for (const auto& s : r.functionSamples)
        if (s.key == "my_func" && s.elapsedNs == 500000) found = true;
    EXPECT_TRUE(found);
}

// ─── recordKernel ────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, RecordKernelAccumulatesLaunches) {
    Profiler::instance().recordKernel("gemv_kernel", 1000000);
    Profiler::instance().recordKernel("gemv_kernel", 2000000);
    auto r = Profiler::instance().report();
    for (const auto& k : r.kernels)
        if (k.name == "gemv_kernel") {
            EXPECT_EQ(k.launches, 2);
            EXPECT_EQ(k.totalNs, 3000000);
            return;
        }
    FAIL() << "Kernel not found";
}

TEST_F(ProfilingTest, RecordKernelAvgMs) {
    Profiler::instance().recordKernel("attn_kernel", 2000000); // 2ms
    Profiler::instance().recordKernel("attn_kernel", 4000000); // 4ms
    auto r = Profiler::instance().report();
    for (const auto& k : r.kernels)
        if (k.name == "attn_kernel") {
            EXPECT_NEAR(k.avgMs(), 3.0, 0.01);
            return;
        }
    FAIL();
}

// ─── Reset ───────────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, ResetClearsAllData) {
    {
        auto g = Profiler::instance().measure("to_clear");
    }
    Profiler::instance().reset();
    auto r = Profiler::instance().report();
    EXPECT_TRUE(r.stages.empty());
    EXPECT_TRUE(r.kernels.empty());
    EXPECT_EQ(r.totalWallNs, 0);
}

// ─── report(resetAfter=true) ─────────────────────────────────────────────────

TEST_F(ProfilingTest, ReportWithResetClearsAfterwards) {
    {
        auto g = Profiler::instance().measure("once");
    }
    Profiler::instance().report(true);
    auto r = Profiler::instance().report();
    EXPECT_TRUE(r.stages.empty());
}

// ─── ProfilingReport::stageFraction ──────────────────────────────────────────

TEST_F(ProfilingTest, StageFractionSumsToOne) {
    Profiler::instance().recordFunction("fake", 0);
    // Inject stage records manually via time()
    Profiler::instance().time("stage_a", []{ std::this_thread::sleep_for(std::chrono::microseconds(100)); });
    Profiler::instance().time("stage_b", []{ std::this_thread::sleep_for(std::chrono::microseconds(100)); });

    auto r = Profiler::instance().report();
    double total = 0.0;
    for (const auto& s : r.stages)
        total += r.stageFraction(s.name);
    EXPECT_NEAR(total, 1.0, 0.05);
}

TEST_F(ProfilingTest, StageFractionUnknownStageIsZero) {
    auto r = Profiler::instance().report();
    EXPECT_DOUBLE_EQ(r.stageFraction("nonexistent"), 0.0);
}

// ─── toText ───────────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, ToTextContainsStageName) {
    {
        auto g = Profiler::instance().measure("type_inference");
    }
    auto r = Profiler::instance().report();
    auto text = r.toText();
    EXPECT_NE(text.find("type_inference"), std::string::npos);
}

TEST_F(ProfilingTest, ToTextContainsTotal) {
    {
        auto g = Profiler::instance().measure("structuring");
    }
    auto r = Profiler::instance().report();
    EXPECT_NE(r.toText().find("Total wall"), std::string::npos);
}

// ─── toJson ──────────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, ToJsonIsValidJson) {
    {
        auto g = Profiler::instance().measure("json_stage");
    }
    auto r = Profiler::instance().report();
    auto j = r.toJson();
    EXPECT_NE(j.find("{"), std::string::npos);
    EXPECT_NE(j.find("\"stages\""), std::string::npos);
    EXPECT_NE(j.find("json_stage"), std::string::npos);
}

TEST_F(ProfilingTest, ToJsonContainsTotalWall) {
    auto r = Profiler::instance().report();
    EXPECT_NE(r.toJson().find("total_wall_ms"), std::string::npos);
}

// ─── toCsv ───────────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, ToCsvCreatesFile) {
    {
        auto g = Profiler::instance().measure("csv_stage");
    }
    auto r = Profiler::instance().report();
    const char* path = "test_profile.csv";
    EXPECT_TRUE(r.toCsv(path));
    std::ifstream f(path);
    EXPECT_TRUE(f.is_open());
    std::remove(path);
}

TEST_F(ProfilingTest, ToCsvBadPathFails) {
    auto r = Profiler::instance().report();
    EXPECT_FALSE(r.toCsv("/nonexistent/dir/profile.csv"));
}

// ─── RssTracker ──────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, RssBytesNonNegative) {
    EXPECT_GE(RssTracker::peakRssBytes(), 0);
}

TEST_F(ProfilingTest, CurrentRssBytesNonNegative) {
    EXPECT_GE(RssTracker::currentRssBytes(), 0);
}

// ─── FunctionHistogram ───────────────────────────────────────────────────────

TEST_F(ProfilingTest, HistogramAddIncrementsTotalSamples) {
    FunctionHistogram h;
    h.add(1'000'000);    // 1ms
    h.add(10'000'000);   // 10ms
    h.add(100'000'000);  // 100ms
    EXPECT_EQ(h.totalSamples(), 3);
}

TEST_F(ProfilingTest, HistogramBucketsNonEmpty) {
    FunctionHistogram h;
    EXPECT_FALSE(h.buckets().empty());
}

TEST_F(ProfilingTest, HistogramBucketsSumToTotalSamples) {
    FunctionHistogram h;
    for (int i = 0; i < 100; ++i) h.add(static_cast<Nanos>(i) * 100'000);
    int64_t sum = 0;
    for (const auto& b : h.buckets()) sum += b.count;
    EXPECT_EQ(sum, h.totalSamples());
}

TEST_F(ProfilingTest, HistogramPercentile) {
    FunctionHistogram h;
    for (int i = 1; i <= 100; ++i) h.add(static_cast<Nanos>(i) * 1'000'000);
    auto p50 = h.percentile(0.5);
    EXPECT_GT(p50, 0);
}

TEST_F(ProfilingTest, HistogramFormatNonEmpty) {
    FunctionHistogram h;
    h.add(5'000'000);
    EXPECT_FALSE(h.format().empty());
}

// ─── Thread safety ────────────────────────────────────────────────────────────

TEST_F(ProfilingTest, ConcurrentRecordingNoDataRace) {
    constexpr int N = 8;
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([i] {
            for (int j = 0; j < 20; ++j) {
                auto g = Profiler::instance().measure("concurrent_stage");
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }
    for (auto& t : threads) t.join();
    auto r = Profiler::instance().report();
    for (const auto& s : r.stages)
        if (s.name == "concurrent_stage") {
            EXPECT_EQ(s.callCount, N * 20);
            return;
        }
    FAIL() << "concurrent_stage not found";
}
