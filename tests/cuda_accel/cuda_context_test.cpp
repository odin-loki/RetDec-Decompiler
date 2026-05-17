/**
 * @file tests/cuda_accel/cuda_context_test.cpp
 * @brief Tests for CUDAContext.
 */
#include "retdec/cuda_accel/cuda_context.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

TEST(CUDAContextTest, DefaultConstructed) {
    CUDAContext ctx;
    EXPECT_FALSE(ctx.isReady());
    EXPECT_EQ(ctx.deviceId(), -1);
}

TEST(CUDAContextTest, InitializeAndReset) {
    CUDAContext ctx;
    bool ok = ctx.initialize();
    // Either CUDA is available or not — both paths are valid
    EXPECT_EQ(ok, ctx.isReady());
    if (ok) {
        EXPECT_GE(ctx.deviceId(), 0);
        EXPECT_FALSE(ctx.primaryDevice().name.empty());
        EXPECT_GT(ctx.allDevices().size(), 0u);
    }
    ctx.reset();
    EXPECT_FALSE(ctx.isReady());
}

TEST(CUDAContextTest, BufferAllocFreeRoundTrip) {
    CUDAContext ctx;
    if (!ctx.initialize()) {
        GTEST_SKIP() << "No CUDA device";
    }
    void* buf = ctx.createBuffer(1024);
    ASSERT_NE(buf, nullptr);

    int data[256];
    for (int i = 0; i < 256; ++i) data[i] = i;
    EXPECT_EQ(ctx.writeBuffer(buf, data, sizeof(data)), 0);

    int readback[256]{};
    EXPECT_EQ(ctx.readBuffer(buf, readback, sizeof(readback)), 0);
    EXPECT_EQ(ctx.synchronize(), 0);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(readback[i], i);

    ctx.freeBuffer(buf);
}

TEST(CUDAContextTest, DeviceScores) {
    CUDAContext ctx;
    ctx.initialize();
    for (auto& d : ctx.allDevices()) {
        EXPECT_GE(d.score(), -1);
    }
}
