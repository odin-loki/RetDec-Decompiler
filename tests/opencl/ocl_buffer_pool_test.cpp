/**
 * @file tests/opencl/ocl_buffer_pool_test.cpp
 * @brief Unit tests for OCLBufferPool (requires a live OpenCL runtime).
 *
 * Tests are conditionally disabled when no OpenCL device is available.
 */

#include "retdec/opencl/ocl_buffer_pool.h"
#include "retdec/opencl/ocl_context.h"

#include <gtest/gtest.h>
#include <CL/cl.h>

#include <cstdint>
#include <vector>

using namespace retdec::opencl;

// ─── Fixture ─────────────────────────────────────────────────────────────────

class OCLBufferPoolTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        _available = _ctx.initialize();
    }

    OCLContext   _ctx;
    bool         _available = false;
};

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST_F(OCLBufferPoolTest, AcquireInvalidWhenNoContext)
{
    // Build a pool with a null context — acquire must return invalid guard.
    OCLBufferPool pool(nullptr, nullptr);
    auto guard = pool.acquire(64);
    EXPECT_FALSE(guard.valid());
}

TEST_F(OCLBufferPoolTest, AcquireValidBuffer)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    OCLBufferPool pool(_ctx.clContext(), _ctx.clDevice());
    auto guard = pool.acquire(256);
    ASSERT_TRUE(guard.valid());
    EXPECT_GE(guard.size(), 256u);
}

TEST_F(OCLBufferPoolTest, SizeRoundedUpToPowerOfTwo)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    OCLBufferPool pool(_ctx.clContext(), _ctx.clDevice());
    auto g = pool.acquire(300); // nearest pow2 >= 300 is 512
    ASSERT_TRUE(g.valid());
    EXPECT_EQ(g.size(), 512u);
}

TEST_F(OCLBufferPoolTest, BufferReturnedToPool)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    OCLBufferPool pool(_ctx.clContext(), _ctx.clDevice(), /*maxCached=*/4);
    EXPECT_EQ(pool.cachedCount(), 0u);

    {
        auto g = pool.acquire(64);
        ASSERT_TRUE(g.valid());
        EXPECT_EQ(pool.cachedCount(), 0u); // in use
    }
    // After guard goes out of scope, buffer should be in the pool.
    EXPECT_EQ(pool.cachedCount(), 1u);
}

TEST_F(OCLBufferPoolTest, PoolReusesBuffer)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    OCLBufferPool pool(_ctx.clContext(), _ctx.clDevice(), 4);

    cl_mem first_ptr = nullptr;
    {
        auto g = pool.acquire(64);
        ASSERT_TRUE(g.valid());
        first_ptr = g.mem();
    }

    // Acquire same size again — should get same cl_mem back from pool.
    auto g2 = pool.acquire(64);
    ASSERT_TRUE(g2.valid());
    EXPECT_EQ(g2.mem(), first_ptr);
}

TEST_F(OCLBufferPoolTest, TrimAllReleasesCached)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    OCLBufferPool pool(_ctx.clContext(), _ctx.clDevice(), 8);
    {
        auto g1 = pool.acquire(64);
        auto g2 = pool.acquire(64);
        (void)g1; (void)g2;
    }
    EXPECT_EQ(pool.cachedCount(), 2u);
    pool.trimAll();
    EXPECT_EQ(pool.cachedCount(), 0u);
}

TEST_F(OCLBufferPoolTest, RoundtripDataViaBuffer)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    const std::vector<std::uint32_t> input = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::size_t bytes = input.size() * sizeof(std::uint32_t);

    auto guard = OCLBufferPool(_ctx.clContext(), _ctx.clDevice()).acquire(bytes);
    ASSERT_TRUE(guard.valid());

    ASSERT_EQ(_ctx.writeBuffer(guard.mem(), input.data(), bytes), CL_SUCCESS);

    std::vector<std::uint32_t> output(input.size(), 0);
    ASSERT_EQ(_ctx.readBuffer(guard.mem(), output.data(), bytes), CL_SUCCESS);

    EXPECT_EQ(input, output);
}
