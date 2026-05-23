/**
 * @file tests/retdec/function_analysis_cache_test.cpp
 * @brief Unit tests for per-function analysis cache helpers.
 */

#include "retdec/retdec/function_analysis_cache.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

using namespace retdec::analysis;

TEST(FunctionAnalysisCacheTest, CachePathDerivedFromOutputC)
{
    EXPECT_EQ(functionAnalysisCachePath("/tmp/out/demo.c"),
              "/tmp/out/demo.retdec-fn-cache.json");
    EXPECT_EQ(functionAnalysisCachePath("demo"),
              "demo.retdec-fn-cache.json");
}

TEST(FunctionAnalysisCacheTest, ParallelAnalysisDefaultFollowsHardware)
{
#ifdef _WIN32
    _putenv_s("RETDEC_PARALLEL_ANALYSIS", "");
#else
    unsetenv("RETDEC_PARALLEL_ANALYSIS");
#endif
    const bool expected = std::thread::hardware_concurrency() > 2;
    EXPECT_EQ(parallelAnalysisEnabled(), expected);
}

TEST(FunctionAnalysisCacheTest, ParallelAnalysisEnvOverridesDefault)
{
#ifdef _WIN32
    _putenv_s("RETDEC_PARALLEL_ANALYSIS", "0");
    EXPECT_FALSE(parallelAnalysisEnabled());
    _putenv_s("RETDEC_PARALLEL_ANALYSIS", "1");
    EXPECT_TRUE(parallelAnalysisEnabled());
    _putenv_s("RETDEC_PARALLEL_ANALYSIS", "");
#else
    setenv("RETDEC_PARALLEL_ANALYSIS", "0", 1);
    EXPECT_FALSE(parallelAnalysisEnabled());
    setenv("RETDEC_PARALLEL_ANALYSIS", "1", 1);
    EXPECT_TRUE(parallelAnalysisEnabled());
    unsetenv("RETDEC_PARALLEL_ANALYSIS");
#endif
}

TEST(FunctionAnalysisCacheTest, RoundTripSaveAndLoad)
{
    const fs::path path =
        fs::temp_directory_path() / "retdec_fn_cache_test.json";

    FunctionAnalysisCache cache;
    FunctionAnalysisCache::Entry entry;
    entry.name = "main";
    entry.bodyHash = "deadbeef";
    entry.detections.concurrency.isMT = true;
    cache.put(std::move(entry));

    ASSERT_TRUE(cache.saveToFile(path.string()));

    FunctionAnalysisCache loaded = FunctionAnalysisCache::loadFromFile(path.string());
    const auto* hit = loaded.lookup("main", "deadbeef");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->name, "main");
    EXPECT_EQ(hit->bodyHash, "deadbeef");
    EXPECT_TRUE(hit->detections.concurrency.isMT);
    EXPECT_EQ(loaded.lookup("main", "stale"), nullptr);

    fs::remove(path);
}
