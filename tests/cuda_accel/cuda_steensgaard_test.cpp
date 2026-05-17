/**
 * @file tests/cuda_accel/cuda_steensgaard_test.cpp
 */
#include "retdec/cuda_accel/cuda_steensgaard.h"
#include "retdec/cuda_accel/cuda_context.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

class CUDASteensgaardTest : public ::testing::Test {
protected:
    CUDAContext ctx;
    void SetUp() override { ctx.initialize(); }
};

TEST_F(CUDASteensgaardTest, EmptyConstraints) {
    CUDASteensgaard s(nullptr);
    auto r = s.analyze(4, {});
    EXPECT_EQ(r.aliasClass.size(), 4u);
    EXPECT_EQ(r.pointsTo.size(), 4u);
    // With no constraints, each var is its own class
    for (uint32_t i=0;i<4;i++) EXPECT_EQ(r.aliasClass[i], i);
}

TEST_F(CUDASteensgaardTest, CopyCausesAlias_CPU) {
    CUDASteensgaard s(nullptr);
    // a := b  — after analysis, a and b alias
    auto r = s.analyze(2, {{ConstraintKind::Copy, 0, 1}});
    EXPECT_TRUE(r.mayAlias(0, 1));
}

TEST_F(CUDASteensgaardTest, NoAlias_CPU) {
    CUDASteensgaard s(nullptr);
    // Three independent variables with no constraints
    auto r = s.analyze(3, {});
    EXPECT_FALSE(r.mayAlias(0, 1));
    EXPECT_FALSE(r.mayAlias(0, 2));
    EXPECT_FALSE(r.mayAlias(1, 2));
}

TEST_F(CUDASteensgaardTest, AddrOf_CPU) {
    CUDASteensgaard s(nullptr);
    // p := &x  — p points to x
    auto r = s.analyze(2, {{ConstraintKind::AddrOf, 0, 1}});
    EXPECT_TRUE(r.hasPointsTo(0));
}

TEST_F(CUDASteensgaardTest, CopyCausesAlias_GPU) {
    if (!ctx.isReady()) GTEST_SKIP() << "No CUDA device";
    CUDASteensgaard s(&ctx);
    auto r = s.analyze(2, {{ConstraintKind::Copy, 0, 1}});
    EXPECT_TRUE(r.mayAlias(0, 1));
}

TEST_F(CUDASteensgaardTest, ChainedCopy_CPU) {
    CUDASteensgaard s(nullptr);
    // a:=b, b:=c  — a, b, c all alias
    std::vector<PtsConstraint> cons = {
        {ConstraintKind::Copy, 0, 1},
        {ConstraintKind::Copy, 1, 2},
    };
    auto r = s.analyze(3, cons);
    EXPECT_TRUE(r.mayAlias(0, 1));
    EXPECT_TRUE(r.mayAlias(1, 2));
}
