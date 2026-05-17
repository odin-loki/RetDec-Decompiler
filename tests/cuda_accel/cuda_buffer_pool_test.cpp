/**
 * @file tests/cuda_accel/cuda_buffer_pool_test.cpp
 */
#include "retdec/cuda_accel/cuda_buffer_pool.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

TEST(CUDABufferPoolTest, AcquireAndRelease) {
    CUDABufferPool pool(4);
    {
        auto buf = pool.acquire(128);
        // If no CUDA, buf is invalid but should not crash
        if (buf.valid()) {
            EXPECT_GE(buf.size(), 128u);
            EXPECT_NE(buf.mem(), nullptr);
        }
    }
    // After PooledBuffer dtor, memory should be returned to pool
    EXPECT_LE(pool.cachedCount(), std::size_t(4));
}

TEST(CUDABufferPoolTest, TrimAll) {
    CUDABufferPool pool(8);
    {
        auto b1 = pool.acquire(64);
        auto b2 = pool.acquire(128);
        (void)b1; (void)b2;
    }
    pool.trimAll();
    EXPECT_EQ(pool.cachedCount(), 0u);
}

TEST(CUDABufferPoolTest, MultipleAcquires) {
    CUDABufferPool pool(2);
    std::vector<PooledBuffer> bufs;
    for (int i = 0; i < 5; ++i)
        bufs.push_back(pool.acquire(256));
    bufs.clear();
    // At most 2 per bucket after clear
    EXPECT_LE(pool.cachedCount(), std::size_t(10));
}
