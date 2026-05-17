/**
 * @file tests/cuda_accel/cuda_profiler_test.cpp
 */
#include "retdec/cuda_accel/cuda_profiler.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

TEST(CUDAProfilerTest, RecordAndRetrieve) {
    auto& p = CUDAProfiler::instance();
    p.reset();

    p.record("kernel_a", 1000000u);  // 1 ms
    p.record("kernel_a", 2000000u);  // 2 ms
    p.record("kernel_b", 500000u);   // 0.5 ms

    EXPECT_EQ(p.totalLaunches(), 3u);

    auto sa = p.statsFor("kernel_a");
    EXPECT_EQ(sa.launches, 2u);
    EXPECT_EQ(sa.totalNs, 3000000u);
    EXPECT_EQ(sa.minNs, 1000000u);
    EXPECT_EQ(sa.maxNs, 2000000u);
    EXPECT_NEAR(sa.avgMs(), 1.5, 1e-6);

    auto sb = p.statsFor("kernel_b");
    EXPECT_EQ(sb.launches, 1u);
}

TEST(CUDAProfilerTest, Reset) {
    auto& p = CUDAProfiler::instance();
    p.record("k", 100u);
    p.reset();
    EXPECT_EQ(p.totalLaunches(), 0u);
    auto s = p.statsFor("k");
    EXPECT_EQ(s.launches, 0u);
}

TEST(CUDAProfilerTest, Report) {
    auto& p = CUDAProfiler::instance();
    p.reset();
    p.record("slow",  9000000u);
    p.record("fast",  1000000u);
    auto r = p.report();
    ASSERT_EQ(r.size(), 2u);
    // sorted descending by total time
    EXPECT_GE(r[0].second.totalNs, r[1].second.totalNs);
}
