/**
 * @file tests/retdec/thread_pool_test.cpp
 * @brief Smoke tests for retdec::utils::ThreadPool.
 */

#include "retdec/utils/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using retdec::utils::ThreadPool;

TEST(ThreadPoolTest, RunsSubmittedTasks)
{
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    auto f1 = pool.submit([&counter]() { counter.fetch_add(1); });
    auto f2 = pool.submit([&counter]() { counter.fetch_add(10); });
    f1.get();
    f2.get();

    EXPECT_EQ(counter.load(), 11);
}

TEST(ThreadPoolTest, WaitBlocksUntilAllTasksFinish)
{
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    for (int i = 0; i < 8; ++i) {
        pool.submit([&counter]() { counter.fetch_add(1); });
    }
    pool.wait();

    EXPECT_EQ(counter.load(), 8);
}

TEST(ThreadPoolTest, ReturnsValueFromSubmit)
{
    ThreadPool pool(1);
    auto fut = pool.submit([]() { return 42; });
    EXPECT_EQ(fut.get(), 42);
}
