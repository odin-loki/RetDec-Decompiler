/**
 * @file tests/qwen3/qwen3_cuda_test.cpp
 * @brief Unit tests for Qwen3CUDA — device init, weight upload, GEMV correctness.
 *
 * Tests are split into two categories:
 *   - CPU-side tests: always run, verify stub / non-CUDA behaviour
 *   - GPU tests: gated on Qwen3CUDA::enumDevices() finding a device.
 *               Skipped gracefully if no CUDA GPU is present.
 *
 * GEMV correctness: for every supported dtype we compute the same GEMV on CPU
 * and on CUDA and compare results within a tolerance that accounts for
 * quantisation rounding and FP32 accumulation differences.
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_ops.h"
#include "retdec/qwen3/qwen3_weights.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

using namespace retdec::qwen3;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static bool hasCudaGpu() {
    auto devs = Qwen3CUDA::enumDevices();
    return !devs.empty();
}

/// Build a trivial F32 weight matrix: W[r][c] = (r * cols + c) * scale
static std::vector<uint8_t> makeF32Weight(int rows, int cols, float scale = 0.01f) {
    std::vector<float> W(static_cast<std::size_t>(rows) * cols);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            W[static_cast<std::size_t>(r) * cols + c] =
                static_cast<float>(r * cols + c) * scale;
    std::vector<uint8_t> out(W.size() * sizeof(float));
    std::memcpy(out.data(), W.data(), out.size());
    return out;
}

/// Build a Q8_0 weight matrix where all scale blocks = 1.0 (as f16) and
/// quantized values are clipped int8 of (r * cols + c) % 64 - 32.
static std::vector<uint8_t> makeQ8_0Weight(int rows, int cols) {
    // Q8_0: 32-elem blocks, 2-byte f16 scale + 32 bytes i8 = 34 bytes/block
    assert(cols % 32 == 0);
    int nb = cols / 32;
    int bpb = 34;
    std::vector<uint8_t> out(static_cast<std::size_t>(rows) * nb * bpb, 0);
    // f16 representation of 1.0 = 0x3C00
    uint16_t scale1 = 0x3C00u;

    for (int r = 0; r < rows; r++) {
        for (int b = 0; b < nb; b++) {
            uint8_t* blk = out.data() +
                           (static_cast<std::size_t>(r) * nb + b) * bpb;
            std::memcpy(blk, &scale1, 2);  // f16 scale = 1.0
            signed char* qs = reinterpret_cast<signed char*>(blk + 2);
            for (int i = 0; i < 32; i++) {
                int val = (r * cols + b * 32 + i) % 64 - 32;
                qs[i] = static_cast<signed char>(
                    std::clamp(val, -128, 127));
            }
        }
    }
    return out;
}

/// Reference CPU GEMV for F32 (used to verify CUDA output).
static std::vector<float> cpuGemvF32(const std::vector<uint8_t>& wBytes,
                                      int rows, int cols,
                                      const std::vector<float>& x) {
    std::vector<float> y(rows, 0.0f);
    ops::gemv(wBytes.data(), GgufDtype::F32, rows, cols, x.data(), y.data());
    return y;
}

static std::vector<float> cpuGemvQ8_0(const std::vector<uint8_t>& wBytes,
                                       int rows, int cols,
                                       const std::vector<float>& x) {
    std::vector<float> y(rows, 0.0f);
    ops::gemv(wBytes.data(), GgufDtype::Q8_0, rows, cols, x.data(), y.data());
    return y;
}

static float maxAbsDiff(const std::vector<float>& a,
                         const std::vector<float>& b) {
    float diff = 0.0f;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); i++)
        diff = std::max(diff, std::abs(a[i] - b[i]));
    return diff;
}

// ─── CPU-side / stub tests ────────────────────────────────────────────────────

TEST(Qwen3CUDAStub, EnumDevicesReturnsVector) {
    // Should not crash regardless of whether CUDA is present
    auto devs = Qwen3CUDA::enumDevices();
    // If CUDA is present we expect at least one entry; otherwise empty.
    // Either way: no throw, returns a valid vector.
    SUCCEED();
}

TEST(Qwen3CUDAStub, InitFailsGracefullyWithNoDevice) {
    if (hasCudaGpu()) GTEST_SKIP() << "CUDA GPU present — skipping stub test";
    Qwen3CUDA cuda;
    EXPECT_FALSE(cuda.init());
    EXPECT_FALSE(cuda.isReady());
    EXPECT_FALSE(cuda.hasGpu());
}

TEST(Qwen3CUDAStub, GemvFallsBackToCpuWhenNotReady) {
    Qwen3CUDA cuda;
    // Do NOT call init() — cuda.isReady() == false
    constexpr int rows = 4, cols = 8;
    auto wBytes = makeF32Weight(rows, cols);
    std::vector<float> x(cols);
    std::iota(x.begin(), x.end(), 1.0f);
    std::vector<float> y(rows, 0.0f);

    bool dispatched = cuda.gemv(wBytes.data(), wBytes.data(),
                                GgufDtype::F32, rows, cols,
                                x.data(), y.data());
    EXPECT_FALSE(dispatched);  // stub returns false

    // Output should still be computed by CPU fallback
    auto ref = cpuGemvF32(wBytes, rows, cols, x);
    EXPECT_FLOAT_EQ(y[0], ref[0]);
    EXPECT_FLOAT_EQ(y[rows - 1], ref[rows - 1]);
}

TEST(Qwen3CUDAStub, UploadWeightReturnsFalseWhenNotReady) {
    Qwen3CUDA cuda;
    auto w = makeF32Weight(4, 8);
    EXPECT_FALSE(cuda.uploadWeight(w.data(), w.data(), w.size()));
}

TEST(Qwen3CUDAStub, OpsDispatchUsesGlobalCuda) {
    // Verify ops::setCUDA / getCUDA round-trip
    ops::setCUDA(nullptr);
    EXPECT_EQ(ops::getCUDA(), nullptr);

    Qwen3CUDA cuda;
    ops::setCUDA(&cuda);
    EXPECT_EQ(ops::getCUDA(), &cuda);

    ops::setCUDA(nullptr);
    EXPECT_EQ(ops::getCUDA(), nullptr);
}

// ─── GPU tests (skipped if no CUDA device) ────────────────────────────────────

class Qwen3CUDATest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!hasCudaGpu())
            GTEST_SKIP() << "No CUDA GPU found — skipping GPU tests";
        ASSERT_TRUE(cuda_.init(1.0f))  // 100% GPU for determinism
            << "CUDA init failed: " << cuda_.lastError();
    }

    void TearDown() override {
        cuda_.shutdown();
        ops::setCUDA(nullptr);
    }

    Qwen3CUDA cuda_;
};

TEST_F(Qwen3CUDATest, DeviceInfoPopulated) {
    const auto& info = cuda_.deviceInfo();
    EXPECT_FALSE(info.name.empty()) << "Device name should be non-empty";
    EXPECT_GT(info.totalMemBytes, 0u);
    EXPECT_GT(info.multiProcessors, 0);
    EXPECT_FALSE(info.computeCapability.empty());
    std::cout << "  CUDA device: " << info.name
              << " (SM " << info.computeCapability << ", "
              << info.totalMemBytes / (1u << 20) << " MB)\n";
}

TEST_F(Qwen3CUDATest, EnumDevicesMatchesInit) {
    auto devs = Qwen3CUDA::enumDevices();
    EXPECT_FALSE(devs.empty());
    EXPECT_FALSE(devs[0].name.empty());
    EXPECT_GT(devs[0].totalMemBytes, 0u);
}

TEST_F(Qwen3CUDATest, IsReadyAfterInit) {
    EXPECT_TRUE(cuda_.isReady());
    EXPECT_TRUE(cuda_.hasGpu());
}

// ── Weight upload and cache ────────────────────────────────────────────────────

TEST_F(Qwen3CUDATest, UploadWeightSucceeds) {
    auto w = makeF32Weight(16, 32);
    EXPECT_TRUE(cuda_.uploadWeight(w.data(), w.data(), w.size()));
    EXPECT_GT(cuda_.weightBytesOnGpu(), 0u);
}

TEST_F(Qwen3CUDATest, UploadWeightIsIdempotent) {
    auto w = makeF32Weight(8, 16);
    EXPECT_TRUE(cuda_.uploadWeight(w.data(), w.data(), w.size()));
    std::size_t after1 = cuda_.weightBytesOnGpu();
    // Second upload with same key — should be a no-op
    EXPECT_TRUE(cuda_.uploadWeight(w.data(), w.data(), w.size()));
    EXPECT_EQ(cuda_.weightBytesOnGpu(), after1);
}

TEST_F(Qwen3CUDATest, ReleaseWeightDecreasesUsage) {
    auto w = makeF32Weight(8, 16);
    cuda_.uploadWeight(w.data(), w.data(), w.size());
    std::size_t before = cuda_.weightBytesOnGpu();
    cuda_.releaseWeight(w.data());
    EXPECT_LT(cuda_.weightBytesOnGpu(), before);
}

TEST_F(Qwen3CUDATest, ReleaseAllWeightsClearsAll) {
    auto w1 = makeF32Weight(4, 8);
    auto w2 = makeF32Weight(4, 8);
    cuda_.uploadWeight(w1.data(), w1.data(), w1.size());
    cuda_.uploadWeight(w2.data(), w2.data(), w2.size());
    cuda_.releaseAllWeights();
    EXPECT_EQ(cuda_.weightBytesOnGpu(), 0u);
}

// ── GEMV correctness ──────────────────────────────────────────────────────────

TEST_F(Qwen3CUDATest, GemvF32MatchesCpu) {
    constexpr int rows = 128, cols = 256;
    auto wBytes = makeF32Weight(rows, cols, 1e-3f);
    std::vector<float> x(cols);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& v : x) v = dist(rng);

    std::vector<float> yCuda(rows, 0.0f);
    bool dispatched = cuda_.gemv(wBytes.data(), wBytes.data(),
                                  GgufDtype::F32, rows, cols,
                                  x.data(), yCuda.data());
    EXPECT_TRUE(dispatched) << "CUDA should have handled this GEMV";

    auto yCpu = cpuGemvF32(wBytes, rows, cols, x);

    float maxDiff = maxAbsDiff(yCuda, yCpu);
    EXPECT_LT(maxDiff, 1e-3f) << "Max F32 GEMV difference: " << maxDiff;
}

TEST_F(Qwen3CUDATest, GemvQ8_0MatchesCpu) {
    constexpr int rows = 64, cols = 128;
    auto wBytes = makeQ8_0Weight(rows, cols);
    std::vector<float> x(cols);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    for (auto& v : x) v = dist(rng);

    std::vector<float> yCuda(rows, 0.0f);
    bool dispatched = cuda_.gemv(wBytes.data(), wBytes.data(),
                                  GgufDtype::Q8_0, rows, cols,
                                  x.data(), yCuda.data());
    EXPECT_TRUE(dispatched);

    auto yCpu = cpuGemvQ8_0(wBytes, rows, cols, x);

    float maxDiff = maxAbsDiff(yCuda, yCpu);
    EXPECT_LT(maxDiff, 1e-4f) << "Max Q8_0 GEMV difference: " << maxDiff;
}

TEST_F(Qwen3CUDATest, GemvReturnsCorrectDimensionality) {
    constexpr int rows = 32, cols = 64;
    auto wBytes = makeF32Weight(rows, cols);
    std::vector<float> x(cols, 1.0f);
    std::vector<float> y(rows, -99.0f);

    cuda_.gemv(wBytes.data(), wBytes.data(),
               GgufDtype::F32, rows, cols, x.data(), y.data());

    // y[r] = sum_c W[r][c] * x[c] = sum_c (r*cols+c)*0.01 * 1.0
    //       = 0.01 * sum_{c=0}^{cols-1}(r*cols+c)
    //       = 0.01 * (r*cols*cols + cols*(cols-1)/2)
    for (int r = 0; r < rows; r++) {
        float expected = 0.01f * (static_cast<float>(r) * cols * cols
                                 + cols * (cols - 1) / 2.0f);
        EXPECT_NEAR(y[r], expected, 1e-2f)
            << "Row " << r << ": expected " << expected << " got " << y[r];
    }
}

TEST_F(Qwen3CUDATest, SmallGemvBelowMinRowsUsesCpu) {
    // Force minGpuRows high so this GEMV goes to CPU
    cuda_.setMinGpuRows(10000);
    constexpr int rows = 4, cols = 8;
    auto wBytes = makeF32Weight(rows, cols);
    std::vector<float> x(cols, 1.0f);
    std::vector<float> y(rows, 0.0f);

    bool dispatched = cuda_.gemv(wBytes.data(), wBytes.data(),
                                  GgufDtype::F32, rows, cols,
                                  x.data(), y.data());
    // Should fall back to CPU (returns false)
    EXPECT_FALSE(dispatched);

    // But output should still be correct
    auto ref = cpuGemvF32(wBytes, rows, cols, x);
    EXPECT_FLOAT_EQ(y[0], ref[0]);
}

TEST_F(Qwen3CUDATest, GpuFractionOneHundredPercent) {
    cuda_.setGpuFraction(1.0f);
    constexpr int rows = 64, cols = 64;
    auto wBytes = makeF32Weight(rows, cols, 0.001f);
    std::vector<float> x(cols, 1.0f);
    std::vector<float> y(rows, 0.0f);

    bool dispatched = cuda_.gemv(wBytes.data(), wBytes.data(),
                                  GgufDtype::F32, rows, cols,
                                  x.data(), y.data());
    EXPECT_TRUE(dispatched);

    auto ref = cpuGemvF32(wBytes, rows, cols, x);
    EXPECT_LT(maxAbsDiff(y, ref), 1e-3f);
}

TEST_F(Qwen3CUDATest, MultipleGemvCallsReuseWeightCache) {
    constexpr int rows = 32, cols = 64;
    auto wBytes = makeF32Weight(rows, cols);
    std::vector<float> x(cols, 0.5f);
    std::vector<float> y(rows, 0.0f);

    // First call: uploads the weight
    cuda_.gemv(wBytes.data(), wBytes.data(),
               GgufDtype::F32, rows, cols, x.data(), y.data());
    std::size_t afterFirst = cuda_.weightBytesOnGpu();

    // Second call: should reuse the cached buffer, same VRAM usage
    std::fill(y.begin(), y.end(), 0.0f);
    cuda_.gemv(wBytes.data(), wBytes.data(),
               GgufDtype::F32, rows, cols, x.data(), y.data());
    EXPECT_EQ(cuda_.weightBytesOnGpu(), afterFirst);
}

TEST_F(Qwen3CUDATest, ShutdownAndReinit) {
    cuda_.shutdown();
    EXPECT_FALSE(cuda_.isReady());
    EXPECT_EQ(cuda_.weightBytesOnGpu(), 0u);

    // Reinit should work
    ASSERT_TRUE(cuda_.init(0.8f));
    EXPECT_TRUE(cuda_.isReady());
}

TEST_F(Qwen3CUDATest, OpsDispatchRoutesThroughCuda) {
    ops::setCUDA(&cuda_);

    constexpr int rows = 32, cols = 32;
    auto wBytes = makeF32Weight(rows, cols);
    std::vector<float> x(cols, 1.0f);
    std::vector<float> y(rows, 0.0f);

    // ops::gemvKeyed should route through our CUDA instance
    ops::gemvKeyed(wBytes.data(), wBytes.data(),
                   GgufDtype::F32, rows, cols, x.data(), y.data());

    auto ref = cpuGemvF32(wBytes, rows, cols, x);
    EXPECT_LT(maxAbsDiff(y, ref), 1e-3f);

    ops::setCUDA(nullptr);
}
