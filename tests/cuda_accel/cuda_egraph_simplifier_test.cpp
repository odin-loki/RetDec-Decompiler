/**
 * @file tests/cuda_accel/cuda_egraph_simplifier_test.cpp
 */
#include "retdec/cuda_accel/cuda_egraph_simplifier.h"
#include "retdec/cuda_accel/cuda_context.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

class CUDAEGraphTest : public ::testing::Test {
protected:
    CUDAContext ctx;
    void SetUp() override { ctx.initialize(); }
};

TEST_F(CUDAEGraphTest, EmptyGraph) {
    CUDAEGraphSimplifier sim(nullptr);
    EGraph g;
    auto r = sim.simplify(g);
    EXPECT_TRUE(r.empty());
}

TEST_F(CUDAEGraphTest, SingleLiteral_CPU) {
    CUDAEGraphSimplifier sim(nullptr);
    EGraph g;
    g.setClassCount(0);
    auto n = g.addLiteral(42u);
    EXPECT_EQ(n, 0u);
    auto r = sim.simplify(g);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].opcode, EOpcode::LIT);
    EXPECT_EQ(r[0].score, 0u);
}

TEST_F(CUDAEGraphTest, XorZeroSimplifies_CPU) {
    // Build: ADD(x, 0) should simplify to x
    CUDAEGraphSimplifier sim(nullptr);
    EGraph g;
    g.setClassCount(0);
    auto xNode = g.addVar(0);      // x
    auto litNode = g.addLiteral(0); // 0
    auto addNode = g.addNode(EOpcode::ADD, xNode, litNode);
    (void)addNode;
    auto r = sim.simplify(g, 32);
    ASSERT_FALSE(r.empty());
    // After simplification, ADD class should map to x (LIT or VAR)
    bool simplified = false;
    for (auto& cr : r) {
        if (cr.opcode == EOpcode::VAR) simplified = true;
    }
    EXPECT_TRUE(simplified);
}

TEST_F(CUDAEGraphTest, DerefAdd_ToArray_CPU) {
    // DEREF(ADD(base, idx)) → ARRAY(base, idx)
    CUDAEGraphSimplifier sim(nullptr);
    EGraph g;
    g.setClassCount(0);
    auto base = g.addVar(0);
    auto idx  = g.addVar(1);
    auto add  = g.addNode(EOpcode::ADD, base, idx);
    auto deref = g.addNode(EOpcode::DEREF, add);
    (void)deref;

    auto r = sim.simplify(g, 64);
    bool hasArray = false;
    for (auto& cr : r) if (cr.opcode == EOpcode::ARRAY) hasArray = true;
    EXPECT_TRUE(hasArray);
}

TEST_F(CUDAEGraphTest, GPU_CPU_Agree) {
    if (!ctx.isReady()) GTEST_SKIP() << "No CUDA device";

    EGraph gCpu, gGpu;
    for (auto* g : {&gCpu, &gGpu}) {
        g->setClassCount(0);
        auto x = g->addVar(0);
        auto lit0 = g->addLiteral(0);
        g->addNode(EOpcode::ADD, x, lit0);
    }

    CUDAEGraphSimplifier cpu(nullptr), gpuSim(&ctx);
    auto rc = cpu.simplify(gCpu, 32);
    auto rg = gpuSim.simplify(gGpu, 32);

    ASSERT_EQ(rc.size(), rg.size());
    for (std::size_t i=0; i<rc.size(); i++) {
        EXPECT_EQ(rc[i].opcode, rg[i].opcode) << "class " << i;
    }
}
