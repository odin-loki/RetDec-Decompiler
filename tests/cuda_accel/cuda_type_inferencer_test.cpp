/**
 * @file tests/cuda_accel/cuda_type_inferencer_test.cpp
 */
#include "retdec/cuda_accel/cuda_type_inferencer.h"
#include "retdec/cuda_accel/cuda_context.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

class CUDATypeInferencerTest : public ::testing::Test {
protected:
    CUDAContext ctx;
    void SetUp() override { ctx.initialize(); }
};

TEST_F(CUDATypeInferencerTest, NoFunctions) {
    CUDATypeInferencer inf(nullptr);
    auto r = inf.infer({});
    EXPECT_TRUE(r.empty());
}

TEST_F(CUDATypeInferencerTest, SingleFunction_NoConstraints_CPU) {
    FunctionTypeData f;
    f.numSlots = 3;
    CUDATypeInferencer inf(nullptr);
    auto r = inf.infer({f});
    ASSERT_EQ(r.size(), 3u);
    for (auto& s : r) {
        EXPECT_EQ(s.widthBytes, 0u);
        EXPECT_EQ(s.sign, TypeSign::Unknown);
        EXPECT_FALSE(s.isPointer);
    }
}

TEST_F(CUDATypeInferencerTest, PropagateWidth_CPU) {
    FunctionTypeData f;
    f.numSlots = 2;
    f.constraints = {{0, 1}};
    // Seed slot 0 with i32
    f.operandHints = {{0, 4, TypeSign::Unknown, false}};

    CUDATypeInferencer inf(nullptr);
    auto r = inf.infer({f});
    ASSERT_EQ(r.size(), 2u);
    // After propagation, slot 1 should also be i32
    EXPECT_EQ(r[0].widthBytes, 4u);
    EXPECT_EQ(r[1].widthBytes, 4u);
}

TEST_F(CUDATypeInferencerTest, SignednessConflict_CPU) {
    // Signed + Unsigned → Signed wins
    FunctionTypeData f;
    f.numSlots = 2;
    f.constraints = {{0, 1}};
    f.operandHints = {
        {0, 4, TypeSign::Signed,   false},
        {1, 4, TypeSign::Unsigned, false},
    };
    CUDATypeInferencer inf(nullptr);
    auto r = inf.infer({f});
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].sign, TypeSign::Signed);
}

TEST_F(CUDATypeInferencerTest, GPU_MatchesCPU) {
    if (!ctx.isReady()) GTEST_SKIP() << "No CUDA device";

    FunctionTypeData f;
    f.numSlots = 4;
    f.constraints = {{0,1},{1,2},{2,3}};
    f.operandHints = {{0, 8, TypeSign::Signed, true}};

    CUDATypeInferencer cpuInf(nullptr), gpuInf(&ctx);
    auto cpuResult = cpuInf.infer({f});
    auto gpuResult = gpuInf.infer({f});

    ASSERT_EQ(cpuResult.size(), gpuResult.size());
    for (std::size_t i=0; i<cpuResult.size(); i++) {
        EXPECT_EQ(cpuResult[i].widthBytes, gpuResult[i].widthBytes) << "at slot " << i;
        EXPECT_EQ(cpuResult[i].isPointer,  gpuResult[i].isPointer)  << "at slot " << i;
    }
}
