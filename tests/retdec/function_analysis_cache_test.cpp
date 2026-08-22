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
    retdec::algo_recover::AlgorithmResult algo;
    algo.kind = retdec::algo_recover::AlgorithmKind::Find;
    algo.confidence = 0.9f;
    entry.detections.algo = algo;
    retdec::sort_detect::SortResult sort;
    sort.algorithm = retdec::sort_detect::SortAlgorithm::Mergesort;
    sort.confidence = 0.8f;
    entry.detections.sort = sort;
    cache.put(std::move(entry));

    FunctionAnalysisCache::Entry intro;
    intro.name = "intro";
    intro.bodyHash = "cafe";
    retdec::sort_detect::SortResult introSort;
    introSort.algorithm = retdec::sort_detect::SortAlgorithm::Introsort;
    introSort.confidence = 0.7f;
    intro.detections.sort = introSort;
    cache.put(std::move(intro));

    FunctionAnalysisCache::Entry heap;
    heap.name = "heap";
    heap.bodyHash = "babe";
    retdec::sort_detect::SortResult heapSort;
    heapSort.algorithm = retdec::sort_detect::SortAlgorithm::Heapsort;
    heapSort.confidence = 0.6f;
    heap.detections.sort = heapSort;
    cache.put(std::move(heap));

    ASSERT_TRUE(cache.saveToFile(path.string()));

    FunctionAnalysisCache loaded = FunctionAnalysisCache::loadFromFile(path.string());
    const auto* hit = loaded.lookup("main", "deadbeef");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->name, "main");
    EXPECT_EQ(hit->bodyHash, "deadbeef");
    ASSERT_TRUE(hit->detections.algo.has_value());
    EXPECT_FLOAT_EQ(hit->detections.algo->confidence, 0.9f);
    ASSERT_TRUE(hit->detections.sort.has_value());
    EXPECT_EQ(hit->detections.sort->algorithm,
              retdec::sort_detect::SortAlgorithm::Mergesort);
    EXPECT_FLOAT_EQ(hit->detections.sort->confidence, 0.8f);

    const auto* introHit = loaded.lookup("intro", "cafe");
    ASSERT_NE(introHit, nullptr);
    ASSERT_TRUE(introHit->detections.sort.has_value());
    EXPECT_EQ(introHit->detections.sort->algorithm,
              retdec::sort_detect::SortAlgorithm::Introsort);

    const auto* heapHit = loaded.lookup("heap", "babe");
    ASSERT_NE(heapHit, nullptr);
    ASSERT_TRUE(heapHit->detections.sort.has_value());
    EXPECT_EQ(heapHit->detections.sort->algorithm,
              retdec::sort_detect::SortAlgorithm::Heapsort);

    EXPECT_EQ(loaded.lookup("main", "stale"), nullptr);

    fs::remove(path);
}
