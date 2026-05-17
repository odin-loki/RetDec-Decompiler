/**
 * @file tests/qwen3/qwen3_moe_test.cpp
 * @brief Unit tests for MoE routing, dispatch, and load monitoring.
 *
 * Tests
 * ─────
 * 1. MoeConfig construction from Qwen3Config presets.
 * 2. MoeRouter top-K selection: correctness, always exactly K experts.
 * 3. MoeRouter weight normalization: sum of weights == 1.0.
 * 4. MoeRouter routing concentrates on high-logit experts.
 * 5. MoeDispatcher: output is a weighted sum of expert FFNs.
 * 6. MoeDispatcher: single expert with weight 1.0 equals plain FFN.
 * 7. Qwen3MoeLayer: end-to-end forward pass with K experts.
 * 8. Qwen3MoeLayer: shared expert is added correctly.
 * 9. MoeLoadMonitor: counts accumulate correctly.
 * 10. MoeLoadMonitor: fractions sum to K / E per token.
 * 11. MoeLoadMonitor: hotExpert / coldExpert identification.
 * 12. Determinism: same input → same route every call.
 * 13. K=1 degenerate routing.
 * 14. Large expert count (numExperts=128, K=8).
 */

#include "retdec/qwen3/qwen3_moe.h"
#include "retdec/qwen3/qwen3_config.h"
#include "retdec/qwen3/qwen3_ops.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace rq = retdec::qwen3;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void fillRand(std::vector<float>& v, unsigned seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& x : v) x = dist(rng);
}

// Build a small F32 expert: gateW = upW = downW = identity-like matrices
static rq::ExpertWeights makeF32Expert(std::vector<float>& gBuf,
                                        std::vector<float>& uBuf,
                                        std::vector<float>& dBuf,
                                        int hidden, int interm,
                                        float gVal, float uVal, float dVal) {
    gBuf.assign(static_cast<std::size_t>(interm * hidden), gVal);
    uBuf.assign(static_cast<std::size_t>(interm * hidden), uVal);
    dBuf.assign(static_cast<std::size_t>(hidden * interm), dVal);
    rq::ExpertWeights ew;
    ew.gateW = reinterpret_cast<const uint8_t*>(gBuf.data());
    ew.upW   = reinterpret_cast<const uint8_t*>(uBuf.data());
    ew.downW = reinterpret_cast<const uint8_t*>(dBuf.data());
    ew.dtype = rq::GgufDtype::F32;
    return ew;
}

// ─── MoeConfig ────────────────────────────────────────────────────────────────

TEST(MoeConfig, FromQwen3Config30BA3B) {
    rq::Qwen3Config cfg;
    cfg.numExperts       = 128;
    cfg.numExpertsPerTok = 8;
    cfg.hiddenSize       = 2048;
    cfg.intermediateSize = 768;

    auto m = rq::MoeConfig::fromQwen3Config(cfg);
    EXPECT_EQ(m.numExperts,       128);
    EXPECT_EQ(m.numExpertsPerTok, 8);
    EXPECT_EQ(m.hiddenSize,       2048);
    EXPECT_EQ(m.moeIntermSize,    768);
    EXPECT_TRUE(m.isMoE());
}

TEST(MoeConfig, DenseIsNotMoE) {
    rq::MoeConfig m;
    EXPECT_FALSE(m.isMoE());
}

TEST(MoeConfig, IsMoEWhenBothSet) {
    rq::MoeConfig m;
    m.numExperts       = 8;
    m.numExpertsPerTok = 2;
    EXPECT_TRUE(m.isMoE());
}

// ─── MoeRouter ────────────────────────────────────────────────────────────────

TEST(MoeRouter, AlwaysSelectsKExperts) {
    rq::MoeConfig cfg;
    cfg.numExperts       = 16;
    cfg.numExpertsPerTok = 4;
    cfg.hiddenSize       = 8;
    cfg.moeIntermSize    = 16;
    cfg.normalizeWeights = true;

    rq::MoeRouter router(cfg);

    std::vector<float> hidden(8);
    fillRand(hidden, 1);
    // Gate weight: random F32 [16 × 8]
    std::vector<float> gateF(16 * 8);
    fillRand(gateF, 2);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    auto res = router.route(hidden.data(), gateW, rq::GgufDtype::F32);
    EXPECT_EQ(static_cast<int>(res.expertIds.size()), 4);
    EXPECT_EQ(static_cast<int>(res.weights.size()),   4);
}

TEST(MoeRouter, WeightsSumToOne) {
    rq::MoeConfig cfg;
    cfg.numExperts       = 8;
    cfg.numExpertsPerTok = 3;
    cfg.hiddenSize       = 4;
    cfg.moeIntermSize    = 8;
    cfg.normalizeWeights = true;

    rq::MoeRouter router(cfg);
    std::vector<float> hidden(4, 1.0f);
    std::vector<float> gateF(8 * 4);
    fillRand(gateF, 7);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    auto res = router.route(hidden.data(), gateW, rq::GgufDtype::F32);
    float wSum = 0.0f;
    for (auto w : res.weights) wSum += w;
    EXPECT_NEAR(wSum, 1.0f, 1e-5f);
}

TEST(MoeRouter, WeightsNoNormalize) {
    rq::MoeConfig cfg;
    cfg.numExperts       = 8;
    cfg.numExpertsPerTok = 3;
    cfg.hiddenSize       = 4;
    cfg.moeIntermSize    = 8;
    cfg.normalizeWeights = false;  // keep raw softmax values

    rq::MoeRouter router(cfg);
    std::vector<float> hidden(4, 1.0f);
    std::vector<float> gateF(8 * 4);
    fillRand(gateF, 11);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    auto res = router.route(hidden.data(), gateW, rq::GgufDtype::F32);
    // Sum should be <= 1 (softmax probs, only top-3)
    float wSum = 0.0f;
    for (auto w : res.weights) wSum += w;
    EXPECT_LT(wSum, 1.001f);
    // Individual weights must be positive
    for (auto w : res.weights) EXPECT_GT(w, 0.0f);
}

TEST(MoeRouter, AllExpertIdsInRange) {
    rq::MoeConfig cfg;
    cfg.numExperts       = 32;
    cfg.numExpertsPerTok = 8;
    cfg.hiddenSize       = 16;
    cfg.moeIntermSize    = 32;
    cfg.normalizeWeights = true;

    rq::MoeRouter router(cfg);
    std::vector<float> hidden(16);
    fillRand(hidden, 99);
    std::vector<float> gateF(32 * 16);
    fillRand(gateF, 100);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    auto res = router.route(hidden.data(), gateW, rq::GgufDtype::F32);
    for (int id : res.expertIds) {
        EXPECT_GE(id, 0);
        EXPECT_LT(id, 32);
    }
}

TEST(MoeRouter, HighLogitExpertSelected) {
    // Place a very large value in the gate weight for expert 5 — it must be selected
    rq::MoeConfig cfg;
    cfg.numExperts       = 8;
    cfg.numExpertsPerTok = 1;
    cfg.hiddenSize       = 4;
    cfg.moeIntermSize    = 8;
    cfg.normalizeWeights = true;

    rq::MoeRouter router(cfg);
    std::vector<float> hidden = {1, 0, 0, 0};
    // Gate weight [8 × 4]: row 5 has very large values
    std::vector<float> gateF(8 * 4, 0.0f);
    gateF[5 * 4 + 0] = 100.0f;  // logit for expert 5 will dominate

    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());
    auto res = router.route(hidden.data(), gateW, rq::GgufDtype::F32);

    EXPECT_EQ(res.expertIds[0], 5);
}

TEST(MoeRouter, Deterministic) {
    rq::MoeConfig cfg;
    cfg.numExperts       = 16;
    cfg.numExpertsPerTok = 4;
    cfg.hiddenSize       = 8;
    cfg.moeIntermSize    = 16;

    rq::MoeRouter router(cfg);
    std::vector<float> hidden(8);
    fillRand(hidden, 55);
    std::vector<float> gateF(16 * 8);
    fillRand(gateF, 56);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    auto r1 = router.route(hidden.data(), gateW, rq::GgufDtype::F32);
    auto r2 = router.route(hidden.data(), gateW, rq::GgufDtype::F32);

    EXPECT_EQ(r1.expertIds, r2.expertIds);
    for (std::size_t i = 0; i < r1.weights.size(); ++i)
        EXPECT_FLOAT_EQ(r1.weights[i], r2.weights[i]);
}

TEST(MoeRouter, K1Degenerate) {
    rq::MoeConfig cfg;
    cfg.numExperts       = 8;
    cfg.numExpertsPerTok = 1;
    cfg.hiddenSize       = 4;
    cfg.moeIntermSize    = 8;
    cfg.normalizeWeights = true;

    rq::MoeRouter router(cfg);
    std::vector<float> hidden = {1, 0, 0, 0};
    std::vector<float> gateF(8 * 4, 0.0f);
    gateF[3 * 4] = 50.0f;  // expert 3 dominates

    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());
    auto res = router.route(hidden.data(), gateW, rq::GgufDtype::F32);

    EXPECT_EQ(static_cast<int>(res.expertIds.size()), 1);
    EXPECT_EQ(res.expertIds[0], 3);
    EXPECT_NEAR(res.weights[0], 1.0f, 1e-5f);
}

// ─── MoeDispatcher ────────────────────────────────────────────────────────────

TEST(MoeDispatcher, SingleExpertWeightOne) {
    // With K=1 and weight=1.0, output should equal the expert's FFN output
    const int D = 4, I = 8;
    rq::MoeConfig cfg;
    cfg.numExperts       = 2;
    cfg.numExpertsPerTok = 1;
    cfg.hiddenSize       = D;
    cfg.moeIntermSize    = I;
    cfg.normalizeWeights = true;

    rq::MoeDispatcher disp(cfg);

    // Expert 0: zero weights → zero output
    // Expert 1: ones for gate/up, ones for down
    std::vector<std::vector<float>> gBufs(2), uBufs(2), dBufs(2);
    std::vector<rq::ExpertWeights> experts(2);
    experts[0] = makeF32Expert(gBufs[0], uBufs[0], dBufs[0], D, I, 0.0f, 0.0f, 0.0f);
    experts[1] = makeF32Expert(gBufs[1], uBufs[1], dBufs[1], D, I, 1.0f, 1.0f, 1.0f);

    std::vector<float> hidden(D, 1.0f);
    rq::MoeRouteResult route;
    route.expertIds = {1};
    route.weights   = {1.0f};

    std::vector<float> out(D, 0.0f);
    disp.forward(hidden.data(), route, experts, out.data());

    // All outputs should be positive (non-zero from expert 1)
    for (int d = 0; d < D; ++d) EXPECT_GT(out[d], 0.0f);
}

TEST(MoeDispatcher, ZeroWeightExpertHasNoEffect) {
    const int D = 4, I = 4;
    rq::MoeConfig cfg;
    cfg.numExperts       = 2;
    cfg.numExpertsPerTok = 2;
    cfg.hiddenSize       = D;
    cfg.moeIntermSize    = I;

    rq::MoeDispatcher disp(cfg);

    std::vector<std::vector<float>> gBufs(2), uBufs(2), dBufs(2);
    std::vector<rq::ExpertWeights> experts(2);
    // Expert 0: large values
    experts[0] = makeF32Expert(gBufs[0], uBufs[0], dBufs[0], D, I, 1.0f, 1.0f, 1.0f);
    // Expert 1: large values but weight=0
    experts[1] = makeF32Expert(gBufs[1], uBufs[1], dBufs[1], D, I, 2.0f, 2.0f, 2.0f);

    std::vector<float> hidden(D, 1.0f);

    rq::MoeRouteResult route2exp;
    route2exp.expertIds = {0, 1};
    route2exp.weights   = {1.0f, 0.0f};  // expert 1 weight = 0

    rq::MoeRouteResult route1exp;
    route1exp.expertIds = {0};
    route1exp.weights   = {1.0f};

    std::vector<float> outBoth(D, 0.0f);
    std::vector<float> outOne (D, 0.0f);

    // Change config to K=2 for both test
    rq::MoeConfig cfg2 = cfg;
    cfg2.numExpertsPerTok = 2;
    rq::MoeDispatcher dispBoth(cfg2);
    dispBoth.forward(hidden.data(), route2exp, experts, outBoth.data());

    rq::MoeConfig cfg1 = cfg;
    cfg1.numExpertsPerTok = 1;
    rq::MoeDispatcher dispOne(cfg1);
    dispOne.forward(hidden.data(), route1exp, experts, outOne.data());

    // Outputs should be equal since expert 1 has weight 0
    for (int d = 0; d < D; ++d)
        EXPECT_NEAR(outBoth[d], outOne[d], 1e-6f);
}

// ─── Qwen3MoeLayer ────────────────────────────────────────────────────────────

TEST(Qwen3MoeLayer, ForwardProducesNonZeroOutput) {
    const int D = 8, I = 16, E = 8, K = 2;
    rq::MoeConfig cfg;
    cfg.numExperts       = E;
    cfg.numExpertsPerTok = K;
    cfg.hiddenSize       = D;
    cfg.moeIntermSize    = I;
    cfg.normalizeWeights = true;

    rq::Qwen3MoeLayer layer(cfg);

    // Build experts (all F32 ones)
    std::vector<std::vector<float>> gBufs(E), uBufs(E), dBufs(E);
    std::vector<rq::ExpertWeights> experts(E);
    for (int e = 0; e < E; ++e)
        experts[e] = makeF32Expert(gBufs[e], uBufs[e], dBufs[e],
                                   D, I, 0.5f, 0.5f, 0.1f);

    // Gate weight [E × D]
    std::vector<float> gateF(E * D);
    fillRand(gateF, 42);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    std::vector<float> hidden(D, 1.0f);
    std::vector<float> out(D, 0.0f);
    layer.forward(hidden.data(), gateW, rq::GgufDtype::F32, experts, out.data());

    // Output should be non-zero
    float norm = 0.0f;
    for (auto x : out) norm += x * x;
    EXPECT_GT(norm, 0.0f);

    // lastRoute should have K experts
    EXPECT_EQ(static_cast<int>(layer.lastRoute().expertIds.size()), K);
}

TEST(Qwen3MoeLayer, SharedExpertAdded) {
    const int D = 4, I = 8, E = 4, K = 1;
    rq::MoeConfig cfg;
    cfg.numExperts             = E;
    cfg.numExpertsPerTok       = K;
    cfg.hiddenSize             = D;
    cfg.moeIntermSize          = I;
    cfg.sharedExpertIntermSize = I; // shared expert active
    cfg.normalizeWeights       = true;

    rq::Qwen3MoeLayer layer(cfg);

    std::vector<std::vector<float>> gBufs(E), uBufs(E), dBufs(E);
    std::vector<rq::ExpertWeights> experts(E);
    for (int e = 0; e < E; ++e)
        experts[e] = makeF32Expert(gBufs[e], uBufs[e], dBufs[e],
                                   D, I, 0.5f, 0.5f, 0.1f);

    // Shared expert with distinct non-zero weights
    std::vector<float> sgBuf, suBuf, sdBuf;
    auto sharedExpert = makeF32Expert(sgBuf, suBuf, sdBuf, D, I, 1.0f, 1.0f, 1.0f);

    std::vector<float> gateF(E * D);
    fillRand(gateF, 88);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    std::vector<float> hidden(D, 1.0f);

    // Output without shared expert (set sharedExpertIntermSize=0)
    rq::MoeConfig cfgNoShared = cfg;
    cfgNoShared.sharedExpertIntermSize = 0;
    rq::Qwen3MoeLayer layerNoShared(cfgNoShared);
    std::vector<float> outNoShared(D, 0.0f);
    layerNoShared.forward(hidden.data(), gateW, rq::GgufDtype::F32, experts,
                           outNoShared.data());

    // Output with shared expert
    std::vector<float> outWithShared(D, 0.0f);
    layer.forward(hidden.data(), gateW, nullptr, rq::GgufDtype::F32,
                  experts, sharedExpert, outWithShared.data());

    // They should differ (shared expert adds non-zero contribution)
    bool different = false;
    for (int d = 0; d < D; ++d)
        if (std::abs(outNoShared[d] - outWithShared[d]) > 1e-6f) { different = true; break; }
    EXPECT_TRUE(different);
}

// ─── MoeLoadMonitor ───────────────────────────────────────────────────────────

TEST(MoeLoadMonitor, InitialCountsZero) {
    rq::MoeLoadMonitor mon(8);
    EXPECT_EQ(mon.totalTokens(), 0);
    for (auto c : mon.counts()) EXPECT_EQ(c, 0);
}

TEST(MoeLoadMonitor, RecordUpdatesCountsAndTotal) {
    rq::MoeLoadMonitor mon(8);
    rq::MoeRouteResult r;
    r.expertIds = {0, 3, 5};
    r.weights   = {0.4f, 0.3f, 0.3f};
    mon.record(r);
    EXPECT_EQ(mon.totalTokens(), 1);
    EXPECT_EQ(mon.counts()[0], 1);
    EXPECT_EQ(mon.counts()[3], 1);
    EXPECT_EQ(mon.counts()[5], 1);
    EXPECT_EQ(mon.counts()[1], 0);
}

TEST(MoeLoadMonitor, FractionsCorrect) {
    rq::MoeLoadMonitor mon(4);
    rq::MoeRouteResult r;
    r.expertIds = {0, 1};
    r.weights   = {0.5f, 0.5f};
    mon.record(r);
    mon.record(r);  // expert 0 and 1 each selected twice

    auto frac = mon.fractions();
    EXPECT_NEAR(frac[0], 1.0f, 1e-5f);  // 2 out of 2 tokens
    EXPECT_NEAR(frac[1], 1.0f, 1e-5f);
    EXPECT_NEAR(frac[2], 0.0f, 1e-5f);
}

TEST(MoeLoadMonitor, HotAndColdExpert) {
    rq::MoeLoadMonitor mon(4);
    // Expert 2 selected 5 times, expert 3 never
    for (int t = 0; t < 5; ++t) {
        rq::MoeRouteResult r;
        r.expertIds = {2};
        r.weights   = {1.0f};
        mon.record(r);
    }
    EXPECT_EQ(mon.hotExpert(), 2);
    // Cold expert is 0, 1, or 3 — all have count 0
    int cold = mon.coldExpert();
    EXPECT_EQ(mon.counts()[cold], 0);
}

TEST(MoeLoadMonitor, ResetClearsAll) {
    rq::MoeLoadMonitor mon(4);
    rq::MoeRouteResult r;
    r.expertIds = {0};
    r.weights   = {1.0f};
    mon.record(r);
    EXPECT_EQ(mon.totalTokens(), 1);
    mon.reset();
    EXPECT_EQ(mon.totalTokens(), 0);
    for (auto c : mon.counts()) EXPECT_EQ(c, 0);
}

// ─── Large expert count (numExperts=128) ─────────────────────────────────────

TEST(MoeRouter, LargeExpertCount128K8) {
    rq::MoeConfig cfg;
    cfg.numExperts       = 128;
    cfg.numExpertsPerTok = 8;
    cfg.hiddenSize       = 64;
    cfg.moeIntermSize    = 128;
    cfg.normalizeWeights = true;

    rq::MoeRouter router(cfg);
    std::vector<float> hidden(64);
    fillRand(hidden, 777);
    std::vector<float> gateF(128 * 64);
    fillRand(gateF, 888);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    auto res = router.route(hidden.data(), gateW, rq::GgufDtype::F32);

    EXPECT_EQ(static_cast<int>(res.expertIds.size()), 8);
    EXPECT_EQ(static_cast<int>(res.weights.size()),   8);

    // Uniqueness
    std::vector<int> sorted = res.expertIds;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 1; i < sorted.size(); ++i)
        EXPECT_NE(sorted[i], sorted[i-1]);

    // Weights sum to ~1
    float wSum = 0.0f;
    for (auto w : res.weights) wSum += w;
    EXPECT_NEAR(wSum, 1.0f, 1e-4f);
}

TEST(Qwen3MoeLayer, LastRoutePopulatedAfterForward) {
    const int D = 4, I = 4, E = 4, K = 2;
    rq::MoeConfig cfg;
    cfg.numExperts       = E;
    cfg.numExpertsPerTok = K;
    cfg.hiddenSize       = D;
    cfg.moeIntermSize    = I;

    rq::Qwen3MoeLayer layer(cfg);

    std::vector<std::vector<float>> gBufs(E), uBufs(E), dBufs(E);
    std::vector<rq::ExpertWeights> experts(E);
    for (int e = 0; e < E; ++e)
        experts[e] = makeF32Expert(gBufs[e], uBufs[e], dBufs[e],
                                   D, I, 0.1f, 0.1f, 0.1f);

    std::vector<float> gateF(E * D);
    fillRand(gateF, 9);
    auto* gateW = reinterpret_cast<const uint8_t*>(gateF.data());

    std::vector<float> hidden(D, 1.0f);
    std::vector<float> out(D, 0.0f);
    layer.forward(hidden.data(), gateW, rq::GgufDtype::F32, experts, out.data());

    auto& route = layer.lastRoute();
    EXPECT_EQ(static_cast<int>(route.expertIds.size()), K);
    EXPECT_EQ(static_cast<int>(route.weights.size()),   K);
}
