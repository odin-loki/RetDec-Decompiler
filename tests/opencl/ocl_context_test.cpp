/**
 * @file tests/opencl/ocl_context_test.cpp
 * @brief Unit tests for OCLContext: device enumeration, kernel JIT, profiling.
 *
 * Tests that require a live OpenCL device are skipped when none is available.
 */

#include "retdec/opencl/ocl_context.h"
#include "retdec/opencl/ocl_profiler.h"
#include "retdec/opencl/ocl_error.h"

#include <gtest/gtest.h>
#include <CL/cl.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace retdec::opencl;

// ─── Fixture ─────────────────────────────────────────────────────────────────

class OCLContextTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        OCLProfiler::instance().reset();
        _available = _ctx.initialize();
    }

    OCLContext _ctx;
    bool       _available = false;
};

// ─── Basic lifecycle ──────────────────────────────────────────────────────────

TEST_F(OCLContextTest, DefaultNotReady)
{
    OCLContext empty;
    EXPECT_FALSE(empty.isReady());
}

TEST_F(OCLContextTest, InitializeSucceedsOrNoDevice)
{
    // isReady must be consistent with initialize()'s return value.
    EXPECT_EQ(_ctx.isReady(), _available);
}

TEST_F(OCLContextTest, ResetMakesNotReady)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }
    ASSERT_TRUE(_ctx.isReady());
    _ctx.reset();
    EXPECT_FALSE(_ctx.isReady());
}

// ─── Device enumeration ───────────────────────────────────────────────────────

TEST_F(OCLContextTest, PrimaryDeviceHasName)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }
    EXPECT_FALSE(_ctx.primaryDevice().name.empty());
}

TEST_F(OCLContextTest, AllDevicesNonEmpty)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }
    EXPECT_GE(_ctx.allDevices().size(), 1u);
}

TEST_F(OCLContextTest, AllDevicesSortedByScoreDesc)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    const auto& devs = _ctx.allDevices();
    for (std::size_t i = 1; i < devs.size(); ++i) {
        EXPECT_GE(devs[i-1].score(), devs[i].score());
    }
}

TEST_F(OCLContextTest, PrimaryDeviceMatchesFirstInAllDevices)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }
    EXPECT_EQ(_ctx.primaryDevice().id, _ctx.allDevices().front().id);
}

// ─── Kernel JIT compilation ───────────────────────────────────────────────────

static constexpr const char* kSimpleKernelSource = R"cl(
__kernel void retdec_test_add_one(__global int* buf)
{
    int gid = get_global_id(0);
    buf[gid] += 1;
}
)cl";

TEST_F(OCLContextTest, EnsureProgramFailsWhenNotReady)
{
    OCLContext empty;
    std::string log;
    EXPECT_FALSE(empty.ensureProgram("key", kSimpleKernelSource, "", &log));
    EXPECT_FALSE(log.empty());
}

TEST_F(OCLContextTest, EnsureProgramCompilesSimpleKernel)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    std::string buildLog;
    bool ok = _ctx.ensureProgram("test_add_one", kSimpleKernelSource, "", &buildLog);
    EXPECT_TRUE(ok) << "Build log: " << buildLog;
    EXPECT_TRUE(buildLog.empty()) << "Unexpected build log: " << buildLog;
}

TEST_F(OCLContextTest, EnsureProgramCacheHit)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    _ctx.ensureProgram("cache_key", kSimpleKernelSource);
    // Second call must succeed immediately (no recompile).
    std::string log;
    bool ok = _ctx.ensureProgram("cache_key", kSimpleKernelSource, "", &log);
    EXPECT_TRUE(ok);
}

TEST_F(OCLContextTest, EnsureProgramReturnsLogOnBadKernel)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    std::string log;
    bool ok = _ctx.ensureProgram("bad", "this is not valid OpenCL C", "", &log);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(log.empty());
}

// ─── Kernel execution with profiling ─────────────────────────────────────────

TEST_F(OCLContextTest, RunKernelAndProfileTiming)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    std::string log;
    ASSERT_TRUE(_ctx.ensureProgram("test_add_one", kSimpleKernelSource, "", &log))
        << log;

    // Prepare host data.
    const std::size_t N = 16;
    std::vector<int> hostData(N, 5);
    const std::size_t bytes = N * sizeof(int);

    cl_int err = CL_SUCCESS;
    cl_mem devBuf = _ctx.createBuffer(bytes, &err);
    ASSERT_EQ(err, CL_SUCCESS);
    ASSERT_NE(devBuf, nullptr);

    ASSERT_EQ(_ctx.writeBuffer(devBuf, hostData.data(), bytes), CL_SUCCESS);

    OCLKernelLaunch launch;
    launch.programKey  = "test_add_one";
    launch.kernelName  = "retdec_test_add_one";
    launch.globalSize  = N;
    launch.localSize   = 0;
    launch.setArgs     = [&](cl_kernel k) {
        clSetKernelArg(k, 0, sizeof(cl_mem), &devBuf);
    };

    err = _ctx.runKernel(launch, /*blockUntilDone=*/true);
    EXPECT_EQ(err, CL_SUCCESS) << clErrorString(err);

    std::vector<int> result(N, 0);
    ASSERT_EQ(_ctx.readBuffer(devBuf, result.data(), bytes), CL_SUCCESS);

    for (std::size_t i = 0; i < N; ++i) {
        EXPECT_EQ(result[i], 6) << "Element " << i << " is wrong";
    }

    clReleaseMemObject(devBuf);

    // Profiler must have recorded at least one launch.
    EXPECT_GE(OCLProfiler::instance().totalLaunches(), 1u);
    auto stats = OCLProfiler::instance().statsFor("retdec_test_add_one");
    EXPECT_GE(stats.launches, 1u);
    EXPECT_GT(stats.totalNs, 0u);
}

// ─── Buffer helpers ───────────────────────────────────────────────────────────

TEST_F(OCLContextTest, CreateBufferFailsWhenNotReady)
{
    OCLContext empty;
    cl_int err = CL_SUCCESS;
    cl_mem buf = empty.createBuffer(64, &err);
    EXPECT_EQ(buf, nullptr);
    EXPECT_EQ(err, CL_INVALID_CONTEXT);
}

TEST_F(OCLContextTest, CreateBufferRoundtrip)
{
    if (!_available) { GTEST_SKIP() << "No OpenCL device"; }

    const std::vector<std::uint64_t> src = {0xDEADBEEF, 0xCAFEBABE, 0x12345678};
    const std::size_t bytes = src.size() * sizeof(std::uint64_t);

    cl_int err = CL_SUCCESS;
    cl_mem buf = _ctx.createBuffer(bytes, &err);
    ASSERT_EQ(err, CL_SUCCESS);
    ASSERT_NE(buf, nullptr);

    EXPECT_EQ(_ctx.writeBuffer(buf, src.data(), bytes), CL_SUCCESS);

    std::vector<std::uint64_t> dst(src.size(), 0);
    EXPECT_EQ(_ctx.readBuffer(buf, dst.data(), bytes), CL_SUCCESS);

    EXPECT_EQ(src, dst);
    clReleaseMemObject(buf);
}

// ─── Error helper ─────────────────────────────────────────────────────────────

TEST(OCLErrorTest, ErrorStringForKnownCodes)
{
    EXPECT_EQ(clErrorString(CL_SUCCESS),       "CL_SUCCESS");
    EXPECT_EQ(clErrorString(CL_INVALID_VALUE), "CL_INVALID_VALUE");
    EXPECT_EQ(clErrorString(CL_OUT_OF_RESOURCES), "CL_OUT_OF_RESOURCES");
}

TEST(OCLErrorTest, ErrorStringForUnknownCode)
{
    const std::string s = clErrorString(-9999);
    EXPECT_NE(s.find("Unknown"), std::string::npos);
    EXPECT_NE(s.find("-9999"), std::string::npos);
}

TEST(OCLErrorTest, OCLExceptionHoldsCode)
{
    try {
        throw OCLException("test msg", CL_BUILD_PROGRAM_FAILURE);
        FAIL() << "Expected OCLException";
    } catch (const OCLException& ex) {
        EXPECT_EQ(ex.code(), CL_BUILD_PROGRAM_FAILURE);
        EXPECT_NE(std::string(ex.what()).find("CL_BUILD_PROGRAM_FAILURE"),
                  std::string::npos);
    }
}
