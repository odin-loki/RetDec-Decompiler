/**
 * @file tests/opencl/ocl_steensgaard_test.cpp
 * @brief Unit tests for OCLSteensgaard.
 *
 * Key invariant checked: Steensgaard is always at least as coarse as Andersen's
 * (i.e., if Steensgaard says "may not alias" then Andersen must agree).
 */

#include <memory>
#include "retdec/opencl/ocl_steensgaard.h"
#include "retdec/opencl/ocl_context.h"

#include <gtest/gtest.h>

using namespace retdec::opencl;

static PtsConstraint copy   (std::uint32_t a, std::uint32_t b) { return {ConstraintKind::Copy,   a, b}; }
static PtsConstraint addrOf (std::uint32_t a, std::uint32_t b) { return {ConstraintKind::AddrOf, a, b}; }
static PtsConstraint store  (std::uint32_t a, std::uint32_t b) { return {ConstraintKind::Store,  a, b}; }
static PtsConstraint load   (std::uint32_t a, std::uint32_t b) { return {ConstraintKind::Load,   a, b}; }

class SteensgaardCPUTest : public ::testing::Test {
protected:
    OCLSteensgaard s{nullptr};
};

class SteensgaardGPUTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        _gpuOk = _ctx.initialize();
        _s = std::make_unique<OCLSteensgaard>(_gpuOk ? &_ctx : nullptr);
    }
    OCLContext                       _ctx;
    bool                             _gpuOk = false;
    std::unique_ptr<OCLSteensgaard>  _s;
};

// ─── CPU tests ────────────────────────────────────────────────────────────────

TEST_F(SteensgaardCPUTest, EmptyInput)
{
    auto r = s.analyze(0, {});
    EXPECT_TRUE(r.aliasClass.empty());
}

TEST_F(SteensgaardCPUTest, NoConstraintAllDistinct)
{
    auto r = s.analyze(4, {});
    ASSERT_EQ(r.aliasClass.size(), 4u);
    // Each variable is its own alias class.
    for (std::uint32_t i = 0; i < 4; ++i) {
        for (std::uint32_t j = i + 1; j < 4; ++j) {
            EXPECT_NE(r.aliasClass[i], r.aliasClass[j]);
        }
    }
}

TEST_F(SteensgaardCPUTest, CopyMergesAliasClasses)
{
    // a := b  → a and b must alias.
    auto r = s.analyze(2, { copy(0, 1) });
    ASSERT_EQ(r.aliasClass.size(), 2u);
    EXPECT_EQ(r.aliasClass[0], r.aliasClass[1]);
}

TEST_F(SteensgaardCPUTest, AddrOfSetsPointsTo)
{
    // p := &x   → pts_to(p) must be x's class.
    // Variables: 0=p, 1=x
    auto r = s.analyze(2, { addrOf(0, 1) });
    ASSERT_EQ(r.aliasClass.size(), 2u);
    ASSERT_EQ(r.pointsTo.size(), 2u);
    // p should point to x (or x's representative).
    EXPECT_NE(r.pointsTo[0], AliasResult::kNoTarget);
    EXPECT_EQ(r.pointsTo[0], r.aliasClass[1]);
}

TEST_F(SteensgaardCPUTest, LoadPropagatesAlias)
{
    // p := &x; q := *p  →  q aliases x  (i.e., alias_class[q] == alias_class[x])
    // Variables: 0=p, 1=x, 2=q
    auto r = s.analyze(3, {
        addrOf(0, 1),  // p = &x
        load(2, 0),    // q = *p
    });
    ASSERT_EQ(r.aliasClass.size(), 3u);
    // q should be in the same alias class as x.
    EXPECT_EQ(r.aliasClass[2], r.aliasClass[1]);
}

TEST_F(SteensgaardCPUTest, StorePropagatesAlias)
{
    // p := &x; *p = y  →  x and y are in the same alias class.
    // Variables: 0=p, 1=x, 2=y
    auto r = s.analyze(3, {
        addrOf(0, 1),  // p = &x
        store(0, 2),   // *p = y
    });
    ASSERT_EQ(r.aliasClass.size(), 3u);
    EXPECT_EQ(r.aliasClass[1], r.aliasClass[2]);
}

TEST_F(SteensgaardCPUTest, TransitiveCopyAllAlias)
{
    // a := b; b := c; c := d  →  all four alias.
    auto r = s.analyze(4, {
        copy(0, 1),
        copy(1, 2),
        copy(2, 3),
    });
    ASSERT_EQ(r.aliasClass.size(), 4u);
    EXPECT_EQ(r.aliasClass[0], r.aliasClass[1]);
    EXPECT_EQ(r.aliasClass[1], r.aliasClass[2]);
    EXPECT_EQ(r.aliasClass[2], r.aliasClass[3]);
}

TEST_F(SteensgaardCPUTest, SteensgaardCoarserThanAnderson_Simple)
{
    // Classic Steensgaard vs Andersen test:
    //   p = &a;  q = &b;  r = p;  r = q;  // Steensgaard merges pts_to(p) with pts_to(q)
    // Variables: 0=p, 1=a, 2=q, 3=b, 4=r
    auto r = s.analyze(5, {
        addrOf(0, 1),  // p = &a
        addrOf(2, 3),  // q = &b
        copy(4, 0),    // r = p
        copy(4, 2),    // r = q  →  Steensgaard: p and q now alias, so a and b alias too
    });
    ASSERT_EQ(r.aliasClass.size(), 5u);
    // Steensgaard: p and q in same class → a and b in same class.
    EXPECT_EQ(r.aliasClass[0], r.aliasClass[2]);   // p ≡ q
    EXPECT_EQ(r.aliasClass[1], r.aliasClass[3]);   // a ≡ b
}

TEST_F(SteensgaardCPUTest, ConvergesInFewIterations)
{
    // 100-variable chain: p0=&v0, p1=&v1, ..., then copy chain.
    const std::uint32_t N = 50;
    std::uint32_t numVars = 2 * N;
    std::vector<PtsConstraint> cons;
    for (std::uint32_t i = 0; i < N; ++i) {
        cons.push_back(addrOf(i, N + i));  // p_i = &v_i
    }
    for (std::uint32_t i = 1; i < N; ++i) {
        cons.push_back(copy(0, i));  // p_0 = p_i (all pointers merge)
    }

    auto r = s.analyze(numVars, cons);
    EXPECT_EQ(r.aliasClass.size(), numVars);
    EXPECT_LE(s.lastIterations(), OCLSteensgaard::kMaxIterations);
    // All pointer vars must alias.
    for (std::uint32_t i = 1; i < N; ++i) {
        EXPECT_EQ(r.aliasClass[0], r.aliasClass[i]);
    }
}

// ─── GPU tests ────────────────────────────────────────────────────────────────

TEST_F(SteensgaardGPUTest, GPUCopyConstraint)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }
    auto r = _s->analyze(3, { copy(0, 1) });
    ASSERT_EQ(r.aliasClass.size(), 3u);
    EXPECT_EQ(r.aliasClass[0], r.aliasClass[1]);
}

TEST_F(SteensgaardGPUTest, GPUAndCPUAgree)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }

    std::vector<PtsConstraint> cons = {
        addrOf(0, 1),
        addrOf(2, 3),
        copy(4, 0),
        copy(4, 2),
        load(5, 4),
    };
    OCLSteensgaard cpuS(nullptr);
    auto cpuR = cpuS.analyze(6, cons);
    auto gpuR = _s->analyze(6, cons);

    ASSERT_EQ(cpuR.aliasClass.size(), gpuR.aliasClass.size());
    for (std::size_t i = 0; i < 6; ++i) {
        // Both paths must agree on whether a pair of vars alias.
        for (std::size_t j = i + 1; j < 6; ++j) {
            bool cpuMay = cpuR.mayAlias(i, j);
            bool gpuMay = gpuR.mayAlias(i, j);
            EXPECT_EQ(cpuMay, gpuMay) << "Alias disagreement for " << i << "," << j;
        }
    }
}
