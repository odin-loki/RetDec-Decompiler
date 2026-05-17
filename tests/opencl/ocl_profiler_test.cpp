/**
 * @file tests/opencl/ocl_profiler_test.cpp
 * @brief Unit tests for OCLProfiler.
 */

#include "retdec/opencl/ocl_profiler.h"

#include <gtest/gtest.h>

using namespace retdec::opencl;

class OCLProfilerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        OCLProfiler::instance().reset();
    }
};

TEST_F(OCLProfilerTest, InitiallyEmpty)
{
    EXPECT_EQ(OCLProfiler::instance().totalLaunches(), 0u);
    EXPECT_TRUE(OCLProfiler::instance().report().empty());
}

TEST_F(OCLProfilerTest, RecordSingleKernel)
{
    OCLProfiler::instance().record("my_kernel", 1'000'000u);

    EXPECT_EQ(OCLProfiler::instance().totalLaunches(), 1u);

    auto stats = OCLProfiler::instance().statsFor("my_kernel");
    EXPECT_EQ(stats.launches, 1u);
    EXPECT_EQ(stats.totalNs, 1'000'000u);
    EXPECT_EQ(stats.minNs, 1'000'000u);
    EXPECT_EQ(stats.maxNs, 1'000'000u);
}

TEST_F(OCLProfilerTest, RecordMultipleLaunches)
{
    OCLProfiler::instance().record("kern", 100u);
    OCLProfiler::instance().record("kern", 300u);
    OCLProfiler::instance().record("kern", 200u);

    auto stats = OCLProfiler::instance().statsFor("kern");
    EXPECT_EQ(stats.launches, 3u);
    EXPECT_EQ(stats.totalNs, 600u);
    EXPECT_EQ(stats.minNs, 100u);
    EXPECT_EQ(stats.maxNs, 300u);
    EXPECT_NEAR(stats.avgMs(), 600.0 / 3 / 1e6, 1e-12);
}

TEST_F(OCLProfilerTest, RecordMultipleKernels)
{
    OCLProfiler::instance().record("kernA", 500u);
    OCLProfiler::instance().record("kernB", 1000u);
    OCLProfiler::instance().record("kernA", 500u);

    EXPECT_EQ(OCLProfiler::instance().totalLaunches(), 3u);

    auto r = OCLProfiler::instance().report();
    ASSERT_EQ(r.size(), 2u);
    // Sorted by totalNs desc: kernA (1000) before kernB? No: kernA = 1000, kernB = 1000.
    // kernA total = 1000, kernB total = 1000 — order is implementation defined.
    bool foundA = false, foundB = false;
    for (const auto& [name, s] : r) {
        if (name == "kernA") { foundA = true; EXPECT_EQ(s.launches, 2u); }
        if (name == "kernB") { foundB = true; EXPECT_EQ(s.launches, 1u); }
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);
}

TEST_F(OCLProfilerTest, ReportSortedByTotalTimeDesc)
{
    OCLProfiler::instance().record("fast", 100u);
    OCLProfiler::instance().record("slow", 9'000u);
    OCLProfiler::instance().record("medium", 500u);

    auto r = OCLProfiler::instance().report();
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].first, "slow");
    EXPECT_EQ(r[1].first, "medium");
    EXPECT_EQ(r[2].first, "fast");
}

TEST_F(OCLProfilerTest, ResetClearsAllStats)
{
    OCLProfiler::instance().record("kern", 42u);
    EXPECT_EQ(OCLProfiler::instance().totalLaunches(), 1u);

    OCLProfiler::instance().reset();
    EXPECT_EQ(OCLProfiler::instance().totalLaunches(), 0u);
    EXPECT_TRUE(OCLProfiler::instance().report().empty());
}

TEST_F(OCLProfilerTest, NullKernelNameTreatedAsUnknown)
{
    OCLProfiler::instance().record(nullptr, 100u);
    auto stats = OCLProfiler::instance().statsFor("(unknown)");
    EXPECT_EQ(stats.launches, 1u);
}

TEST_F(OCLProfilerTest, RecordEventNullIsNoop)
{
    // Should not crash.
    OCLProfiler::instance().recordEvent("kern", nullptr);
    EXPECT_EQ(OCLProfiler::instance().totalLaunches(), 0u);
}
