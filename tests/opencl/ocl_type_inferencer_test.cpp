/**
 * @file tests/opencl/ocl_type_inferencer_test.cpp
 * @brief Unit tests for OCLTypeInferencer (parallel union-find type propagation).
 *
 * Tests exercise the CPU-fallback path.  GPU tests are additionally skipped if
 * no OpenCL device is present.
 */

#include <memory>
#include "retdec/opencl/ocl_type_inferencer.h"
#include "retdec/opencl/ocl_context.h"

#include <gtest/gtest.h>

using namespace retdec::opencl;

// ─── Fixture ─────────────────────────────────────────────────────────────────

class TypeInferencerCPUTest : public ::testing::Test {
protected:
    OCLTypeInferencer inf{nullptr};
};

class TypeInferencerGPUTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        _gpuOk = _ctx.initialize();
        _inf   = std::make_unique<OCLTypeInferencer>(_gpuOk ? &_ctx : nullptr);
    }

    OCLContext                          _ctx;
    bool                                _gpuOk = false;
    std::unique_ptr<OCLTypeInferencer>  _inf;
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

static FunctionTypeData makeSimpleFunc(
    std::uint32_t numSlots,
    std::vector<TypeConstraint> constraints = {},
    std::vector<OperandHint>    hints       = {})
{
    FunctionTypeData f;
    f.numSlots    = numSlots;
    f.constraints = std::move(constraints);
    f.operandHints = std::move(hints);
    return f;
}

// ─── CPU tests ───────────────────────────────────────────────────────────────

TEST_F(TypeInferencerCPUTest, EmptyInputReturnsEmpty)
{
    EXPECT_TRUE(inf.infer({}).empty());
}

TEST_F(TypeInferencerCPUTest, NoConstraintsNoHintsAllUnknown)
{
    auto f = makeSimpleFunc(4);
    auto result = inf.infer({f});

    ASSERT_EQ(result.size(), 4u);
    for (const auto& s : result) {
        EXPECT_EQ(s.widthBytes, 0u);
        EXPECT_EQ(s.sign, TypeSign::Unknown);
        EXPECT_FALSE(s.isPointer);
    }
}

TEST_F(TypeInferencerCPUTest, OperandHintSetsSingleSlot)
{
    FunctionTypeData f;
    f.numSlots = 3;
    f.operandHints.push_back({1, 4, TypeSign::Signed, false});

    auto result = inf.infer({f});
    ASSERT_EQ(result.size(), 3u);

    EXPECT_EQ(result[0].widthBytes, 0u);
    EXPECT_EQ(result[1].widthBytes, 4u);
    EXPECT_EQ(result[1].sign, TypeSign::Signed);
    EXPECT_EQ(result[2].widthBytes, 0u);
}

TEST_F(TypeInferencerCPUTest, ConstraintUnitesTwoSlots)
{
    FunctionTypeData f;
    f.numSlots = 2;
    f.operandHints.push_back({0, 8, TypeSign::Unsigned, false});
    f.constraints.push_back({0, 1});  // slot 0 and slot 1 must have same type

    auto result = inf.infer({f});
    ASSERT_EQ(result.size(), 2u);

    // After union, both slots should have the seeded type from slot 0.
    EXPECT_EQ(result[0].widthBytes, 8u);
    EXPECT_EQ(result[1].widthBytes, 8u);
    EXPECT_EQ(result[0].sign, TypeSign::Unsigned);
    EXPECT_EQ(result[1].sign, TypeSign::Unsigned);
}

TEST_F(TypeInferencerCPUTest, ChainedConstraintsPropagateType)
{
    // 0 → 1 → 2 → 3; hint on 0 should reach all.
    FunctionTypeData f;
    f.numSlots = 4;
    f.operandHints.push_back({0, 4, TypeSign::Signed, false});
    f.constraints.push_back({0, 1});
    f.constraints.push_back({1, 2});
    f.constraints.push_back({2, 3});

    auto result = inf.infer({f});
    ASSERT_EQ(result.size(), 4u);

    for (const auto& s : result) {
        EXPECT_EQ(s.widthBytes, 4u);
        EXPECT_EQ(s.sign, TypeSign::Signed);
    }
}

TEST_F(TypeInferencerCPUTest, WidthMergeTakesMax)
{
    FunctionTypeData f;
    f.numSlots = 2;
    f.operandHints.push_back({0, 2, TypeSign::Unknown, false});
    f.operandHints.push_back({1, 8, TypeSign::Unknown, false});
    f.constraints.push_back({0, 1});

    auto result = inf.infer({f});
    ASSERT_EQ(result.size(), 2u);

    EXPECT_EQ(result[0].widthBytes, 8u);
    EXPECT_EQ(result[1].widthBytes, 8u);
}

TEST_F(TypeInferencerCPUTest, SignedWinsOnConflict)
{
    FunctionTypeData f;
    f.numSlots = 2;
    f.operandHints.push_back({0, 4, TypeSign::Signed,   false});
    f.operandHints.push_back({1, 4, TypeSign::Unsigned, false});
    f.constraints.push_back({0, 1});

    auto result = inf.infer({f});
    ASSERT_EQ(result.size(), 2u);

    EXPECT_EQ(result[0].sign, TypeSign::Signed);
    EXPECT_EQ(result[1].sign, TypeSign::Signed);
}

TEST_F(TypeInferencerCPUTest, PointerFlagPropagates)
{
    FunctionTypeData f;
    f.numSlots = 2;
    f.operandHints.push_back({0, 8, TypeSign::Unknown, true});
    f.constraints.push_back({0, 1});

    auto result = inf.infer({f});
    ASSERT_EQ(result.size(), 2u);

    EXPECT_TRUE(result[0].isPointer);
    EXPECT_TRUE(result[1].isPointer);
}

TEST_F(TypeInferencerCPUTest, MultipleFunctionsIndependent)
{
    FunctionTypeData f0;
    f0.numSlots = 2;
    f0.operandHints.push_back({0, 1, TypeSign::Unsigned, false});

    FunctionTypeData f1;
    f1.numSlots = 3;
    f1.operandHints.push_back({0, 8, TypeSign::Signed, true});
    f1.constraints.push_back({0, 1});
    f1.constraints.push_back({1, 2});

    auto result = inf.infer({f0, f1});
    ASSERT_EQ(result.size(), 5u);

    // f0 slots
    EXPECT_EQ(result[0].widthBytes, 1u);
    EXPECT_EQ(result[1].widthBytes, 0u); // no hint, no constraint to 0

    // f1 slots
    EXPECT_EQ(result[2].widthBytes, 8u);
    EXPECT_EQ(result[3].widthBytes, 8u);
    EXPECT_EQ(result[4].widthBytes, 8u);
    EXPECT_TRUE(result[2].isPointer);
}

TEST_F(TypeInferencerCPUTest, ConvergesInFewIterations)
{
    // A 100-slot chain; should converge in O(α(100)) amortised iterations.
    FunctionTypeData f;
    f.numSlots = 100;
    f.operandHints.push_back({0, 4, TypeSign::Unsigned, false});
    for (std::uint32_t i = 0; i < 99; ++i) {
        f.constraints.push_back({i, i + 1});
    }

    auto result = inf.infer({f});
    ASSERT_EQ(result.size(), 100u);

    for (const auto& s : result) {
        EXPECT_EQ(s.widthBytes, 4u);
    }
    EXPECT_LE(inf.lastIterations(), OCLTypeInferencer::kMaxIterations);
}

// ─── GPU tests ────────────────────────────────────────────────────────────────

TEST_F(TypeInferencerGPUTest, GPUSimpleConstraint)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }

    FunctionTypeData f;
    f.numSlots = 2;
    f.operandHints.push_back({0, 4, TypeSign::Signed, false});
    f.constraints.push_back({0, 1});

    auto result = _inf->infer({f});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].widthBytes, 4u);
    EXPECT_EQ(result[1].widthBytes, 4u);
}

TEST_F(TypeInferencerGPUTest, GPUAndCPUAgree)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }

    FunctionTypeData f;
    f.numSlots = 5;
    f.operandHints.push_back({0, 8, TypeSign::Unsigned, true});
    f.operandHints.push_back({3, 2, TypeSign::Signed,   false});
    f.constraints.push_back({0, 1});
    f.constraints.push_back({1, 2});
    f.constraints.push_back({3, 4});

    OCLTypeInferencer cpuInf(nullptr);
    auto cpuResult = cpuInf.infer({f});
    auto gpuResult = _inf->infer({f});

    ASSERT_EQ(cpuResult.size(), gpuResult.size());
    for (std::size_t i = 0; i < cpuResult.size(); ++i) {
        EXPECT_EQ(cpuResult[i].widthBytes, gpuResult[i].widthBytes) << "slot " << i;
        EXPECT_EQ(cpuResult[i].sign,       gpuResult[i].sign)       << "slot " << i;
        EXPECT_EQ(cpuResult[i].isPointer,  gpuResult[i].isPointer)  << "slot " << i;
    }
}
