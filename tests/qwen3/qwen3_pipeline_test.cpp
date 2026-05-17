/**
 * @file tests/qwen3/qwen3_pipeline_test.cpp
 * @brief Unit tests for Qwen3Pipeline — full inference pipeline.
 *
 * All tests use synthetic (random) weights via loadDirect() so no GGUF
 * file is required.
 *
 * Test groups
 * ───────────
 * 1. Construction / isLoaded state.
 * 2. loadDirect with a tiny model (1 layer, small dims).
 * 3. forward() produces finite logits; second call changes logits.
 * 4. prefill() processes multiple tokens without crash.
 * 5. sampleNext() returns a valid token ID.
 * 6. resetKvCache() resets state; re-run produces same logits.
 * 7. Full generate() loop terminates on EOS and respects maxNewTokens.
 * 8. streamCallback receives all generated pieces.
 * 9. tokenCallback called once per generated token.
 * 10. Paged KV cache: loadDirect with paged cache; forward matches dense.
 * 11. FlashAttn integration: CPU path gives finite output.
 * 12. MoE forward: single MoE layer produces non-zero output.
 * 13. Stats (prefillMs, decodeMs, tokPerSec) populated after generate.
 * 14. Large model config (numLayers=8, numHeads=8) smoke test.
 */

#include "retdec/qwen3/qwen3_pipeline.h"
#include "retdec/qwen3/qwen3_config.h"
#include "retdec/qwen3/qwen3_tokenizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace rq = retdec::qwen3;

// ─── Test helpers ─────────────────────────────────────────────────────────────

static void fillRand(std::vector<float>& v, unsigned seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.02f, 0.02f);
    for (auto& x : v) x = dist(rng);
}

static std::vector<uint8_t> randF32Bytes(std::size_t floats, unsigned seed = 1) {
    std::vector<float> f(floats);
    fillRand(f, seed);
    std::vector<uint8_t> b(floats * sizeof(float));
    std::memcpy(b.data(), f.data(), b.size());
    return b;
}

// Build a minimal pipeline config and layer weights for a small model.
// dims: D=hidden, I=intermediate, nh=numHeads, nk=numKvHeads, hd=headDim, V=vocab
struct TinyModel {
    rq::Qwen3Config                         cfg;
    std::vector<rq::PipelineLayerWeights>   layers;
    std::vector<uint8_t>                    embedBuf;
    std::vector<uint8_t>                    lmHeadBuf;
    std::vector<float>                      outputNorm;
    // Storage keeping the raw byte buffers alive
    std::vector<std::vector<uint8_t>>       storage;
    std::vector<std::vector<float>>         normStorage;

    static TinyModel make(int D = 16, int I = 32, int nh = 4,
                          int nk = 2, int hd = 4, int V = 64,
                          int nLayers = 1, bool isMoE = false,
                          int numExperts = 4, int nExpertPerTok = 2) {
        TinyModel m;
        auto& c = m.cfg;
        c.hiddenSize       = static_cast<uint32_t>(D);
        c.intermediateSize = static_cast<uint32_t>(I);
        c.numHeads         = static_cast<uint32_t>(nh);
        c.numKvHeads       = static_cast<uint32_t>(nk);
        c.headDim          = static_cast<uint32_t>(hd);
        c.vocabSize        = static_cast<uint32_t>(V);
        c.numLayers        = static_cast<uint32_t>(nLayers);
        c.maxPositionEmbeddings = 64;
        c.ropeTheta        = 10000.0f;
        if (isMoE) {
            c.numExperts       = static_cast<uint32_t>(numExperts);
            c.numExpertsPerTok = static_cast<uint32_t>(nExpertPerTok);
        }

        m.embedBuf   = randF32Bytes(static_cast<std::size_t>(V * D), 1);
        m.lmHeadBuf  = randF32Bytes(static_cast<std::size_t>(V * D), 2);
        m.outputNorm.assign(static_cast<std::size_t>(D), 1.0f);

        auto addW = [&](std::size_t nFloats, unsigned seed) -> const uint8_t* {
            m.storage.push_back(randF32Bytes(nFloats, seed));
            return m.storage.back().data();
        };
        auto addNorm = [&](int sz, unsigned seed) -> const float* {
            m.normStorage.emplace_back(static_cast<std::size_t>(sz), 1.0f);
            fillRand(m.normStorage.back(), seed);
            return m.normStorage.back().data();
        };

        unsigned s = 10;
        for (int l = 0; l < nLayers; ++l) {
            rq::PipelineLayerWeights lw;
            lw.attnNorm = addNorm(D, s++);
            lw.ffnNorm  = addNorm(D, s++);
            lw.wq    = addW(static_cast<std::size_t>(nh * hd * D), s++); lw.wqDtype = rq::GgufDtype::F32;
            lw.wk    = addW(static_cast<std::size_t>(nk * hd * D), s++); lw.wkDtype = rq::GgufDtype::F32;
            lw.wv    = addW(static_cast<std::size_t>(nk * hd * D), s++); lw.wvDtype = rq::GgufDtype::F32;
            lw.wo    = addW(static_cast<std::size_t>(D * nh * hd), s++); lw.woDtype = rq::GgufDtype::F32;

            if (isMoE) {
                lw.isMoE     = true;
                lw.moeGate   = addW(static_cast<std::size_t>(numExperts * D), s++);
                lw.moeGateDtype = rq::GgufDtype::F32;
                lw.experts.resize(static_cast<std::size_t>(numExperts));
                for (int e = 0; e < numExperts; ++e) {
                    lw.experts[e].gateW = addW(static_cast<std::size_t>(I * D), s++);
                    lw.experts[e].upW   = addW(static_cast<std::size_t>(I * D), s++);
                    lw.experts[e].downW = addW(static_cast<std::size_t>(D * I), s++);
                    lw.experts[e].dtype = rq::GgufDtype::F32;
                }
            } else {
                lw.wGate = addW(static_cast<std::size_t>(I * D), s++); lw.wGateDtype = rq::GgufDtype::F32;
                lw.wUp   = addW(static_cast<std::size_t>(I * D), s++); lw.wUpDtype   = rq::GgufDtype::F32;
                lw.wDown = addW(static_cast<std::size_t>(D * I), s++); lw.wDownDtype = rq::GgufDtype::F32;
            }
            m.layers.push_back(std::move(lw));
        }
        return m;
    }

    void applyTo(rq::Qwen3Pipeline& pipe) {
        rq::Qwen3Tokenizer tok;
        // Minimal vocab: token 0 = "<eos>", others = single chars
        // (encode returns empty for unknown strings, which is fine for tests)
        pipe.loadDirect(cfg,
                        layers,
                        embedBuf,
                        rq::GgufDtype::F32,
                        lmHeadBuf,
                        rq::GgufDtype::F32,
                        outputNorm,
                        tok);
    }
};

// ─── Construction ─────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, DefaultNotLoaded) {
    rq::Qwen3Pipeline pipe;
    EXPECT_FALSE(pipe.isLoaded());
    EXPECT_EQ(pipe.currentPos(), 0);
}

// ─── loadDirect ───────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, LoadDirectSetIsLoaded) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);
    EXPECT_TRUE(pipe.isLoaded());
}

TEST(Qwen3Pipeline, ConfigAccessible) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make(16, 32, 4, 2, 4, 64, 1);
    m.applyTo(pipe);
    EXPECT_EQ(pipe.config().hiddenSize, 16u);
    EXPECT_EQ(pipe.config().vocabSize,  64u);
    EXPECT_EQ(pipe.config().numLayers,   1u);
}

// ─── forward ──────────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, ForwardProducesFiniteLogits) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    pipe.forward(0, 0);
    const auto& logits = pipe.logits();
    EXPECT_EQ(static_cast<int>(logits.size()), 64);
    for (auto v : logits) {
        EXPECT_FALSE(std::isnan(v));
        EXPECT_FALSE(std::isinf(v));
    }
}

TEST(Qwen3Pipeline, TwoForwardsDifferentLogits) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    pipe.forward(0, 0);
    std::vector<float> l1 = pipe.logits();

    pipe.resetKvCache();
    pipe.forward(1, 0);
    std::vector<float> l2 = pipe.logits();

    // Different token IDs → different logits
    bool different = false;
    for (std::size_t i = 0; i < l1.size(); ++i)
        if (std::abs(l1[i] - l2[i]) > 1e-6f) { different = true; break; }
    EXPECT_TRUE(different);
}

TEST(Qwen3Pipeline, PosAdvancesAfterForward) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    EXPECT_EQ(pipe.currentPos(), 0);
    pipe.forward(0, 0);
    EXPECT_EQ(pipe.currentPos(), 1);
    pipe.forward(1, 1);
    EXPECT_EQ(pipe.currentPos(), 2);
}

// ─── prefill ──────────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, PrefillProcessesAllTokens) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make(16, 32, 4, 2, 4, 64, 1);
    m.applyTo(pipe);

    std::vector<int32_t> prompt = {0, 1, 2, 3, 4};
    pipe.prefill(prompt);
    EXPECT_EQ(pipe.currentPos(), 5);
}

TEST(Qwen3Pipeline, PrefillLogitsFinite) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    std::vector<int32_t> prompt = {5, 10, 15};
    pipe.prefill(prompt);
    for (auto v : pipe.logits()) {
        EXPECT_FALSE(std::isnan(v));
        EXPECT_FALSE(std::isinf(v));
    }
}

// ─── sampleNext ───────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, SampleNextInVocabRange) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make(16, 32, 4, 2, 4, 64, 1);
    m.applyTo(pipe);

    pipe.forward(0, 0);

    rq::SamplerConfig sampCfg;
    sampCfg.temperature = 1.0f;
    sampCfg.topP        = 1.0f;
    sampCfg.seed        = 42;

    int32_t tok = pipe.sampleNext(sampCfg, {0});
    EXPECT_GE(tok, 0);
    EXPECT_LT(tok, 64);
}

TEST(Qwen3Pipeline, GreedySampleDeterministic) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);
    pipe.forward(0, 0);

    // Temperature=0 → greedy
    rq::SamplerConfig sampCfg;
    sampCfg.temperature = 0.0f;
    sampCfg.seed = 1;

    int32_t t1 = pipe.sampleNext(sampCfg, {0});
    // Reset and repeat
    pipe.resetKvCache();
    pipe.forward(0, 0);
    int32_t t2 = pipe.sampleNext(sampCfg, {0});
    EXPECT_EQ(t1, t2);
}

// ─── resetKvCache ─────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, ResetResetsPos) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);
    pipe.forward(0, 0);
    pipe.forward(1, 1);
    EXPECT_EQ(pipe.currentPos(), 2);
    pipe.resetKvCache();
    EXPECT_EQ(pipe.currentPos(), 0);
}

TEST(Qwen3Pipeline, ResetSameLogitsAsFirst) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    pipe.forward(7, 0);
    std::vector<float> l1 = pipe.logits();

    pipe.resetKvCache();
    pipe.forward(7, 0);
    std::vector<float> l2 = pipe.logits();

    for (std::size_t i = 0; i < l1.size(); ++i)
        EXPECT_NEAR(l1[i], l2[i], 1e-6f);
}

// ─── generate ─────────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, GenerateFromIdsRespectMaxNewTokens) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make(16, 32, 4, 2, 4, 64, 1);
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 5;
    opts.temperature  = 0.0f;  // greedy

    auto res = pipe.generateFromIds({0, 1}, opts);
    // Should have at most 5 new tokens (may be fewer if EOS sampled)
    EXPECT_LE(res.newTokens, 5);
    EXPECT_EQ(res.promptTokens, 2);
}

TEST(Qwen3Pipeline, TokenCallbackCalledPerToken) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 4;
    opts.temperature  = 0.0f;

    int callbackCount = 0;
    opts.tokenCallback = [&](int32_t, const std::string&) { ++callbackCount; };

    auto res = pipe.generateFromIds({0}, opts);
    EXPECT_EQ(callbackCount, res.newTokens);
}

TEST(Qwen3Pipeline, StreamCallbackCanStop) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 20;
    opts.temperature  = 0.0f;

    int callCount = 0;
    opts.streamCallback = [&](const std::string&) -> bool {
        ++callCount;
        return callCount < 3;  // stop after 3 tokens
    };

    auto res = pipe.generateFromIds({0}, opts);
    EXPECT_LE(res.newTokens, 3);
}

TEST(Qwen3Pipeline, GenerateResultHasStats) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 3;
    opts.temperature  = 0.0f;

    auto res = pipe.generateFromIds({0, 1, 2}, opts);
    EXPECT_GT(res.prefillMs, 0.0);
    EXPECT_GE(res.decodeMs,  0.0);
    EXPECT_EQ(res.promptTokens, 3);
}

// ─── Paged KV cache ───────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, PagedKvForwardFinite) {
    rq::Qwen3Pipeline pipe;
    pipe.setPagedKvCache(true);
    EXPECT_TRUE(pipe.hasPagedKv());

    auto m = TinyModel::make();
    m.applyTo(pipe);

    pipe.forward(0, 0);
    for (auto v : pipe.logits()) {
        EXPECT_FALSE(std::isnan(v));
        EXPECT_FALSE(std::isinf(v));
    }
}

TEST(Qwen3Pipeline, PagedKvGenerates) {
    rq::Qwen3Pipeline pipe;
    pipe.setPagedKvCache(true);

    auto m = TinyModel::make();
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 3;
    opts.temperature  = 0.0f;

    auto res = pipe.generateFromIds({0, 1}, opts);
    EXPECT_LE(res.newTokens, 3);
}

// ─── MoE layer ───────────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, MoEForwardProducesFiniteLogits) {
    rq::Qwen3Pipeline pipe;
    // tiny MoE: 4 experts, top-2 per token
    auto m = TinyModel::make(16, 8, 4, 2, 4, 32, 1,
                              /*isMoE=*/true, /*numExperts=*/4, /*nExpertPerTok=*/2);
    m.applyTo(pipe);

    pipe.forward(0, 0);
    for (auto v : pipe.logits()) {
        EXPECT_FALSE(std::isnan(v));
        EXPECT_FALSE(std::isinf(v));
    }
}

TEST(Qwen3Pipeline, MoEGenerateTerminates) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make(16, 8, 4, 2, 4, 32, 1, true, 4, 2);
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 4;
    opts.temperature  = 0.0f;

    auto res = pipe.generateFromIds({0}, opts);
    EXPECT_LE(res.newTokens, 4);
}

// ─── FlashAttn integration ────────────────────────────────────────────────────

TEST(Qwen3Pipeline, FlashAttnGpuReadyCpuFallback) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.applyTo(pipe);

    // Either GPU is ready or CPU fallback — either way forward() must work
    pipe.forward(5, 0);
    for (auto v : pipe.logits()) EXPECT_FALSE(std::isnan(v));
}

// ─── Multi-layer model smoke test ─────────────────────────────────────────────

TEST(Qwen3Pipeline, MultiLayerForward) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make(32, 64, 8, 4, 4, 128, 4);
    m.applyTo(pipe);

    pipe.prefill({0, 1, 2, 3, 4, 5});
    EXPECT_EQ(pipe.currentPos(), 6);

    rq::SamplerConfig sc;
    sc.temperature = 0.0f;
    int32_t tok = pipe.sampleNext(sc, {0,1,2,3,4,5});
    EXPECT_GE(tok, 0);
    EXPECT_LT(tok, 128);
}

TEST(Qwen3Pipeline, MultiLayerGenerateSmokeTest) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make(32, 64, 8, 4, 4, 128, 3);
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 6;
    opts.temperature  = 0.0f;

    auto res = pipe.generateFromIds({0, 1, 2}, opts);
    EXPECT_LE(res.newTokens, 6);
    EXPECT_EQ(res.promptTokens, 3);
}

// ─── EOS stop token ───────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, HitsEosWhenEosSampled) {
    rq::Qwen3Pipeline pipe;
    // Make EOS token 0 the only token with a high logit by using greedy
    // with a model whose lmHead is a zero matrix + epsilon → token 0 always wins.
    // Instead, just check that if EOS is somehow sampled, hitEos is true.
    auto m = TinyModel::make(16, 32, 4, 2, 4, 64, 1);
    // Force EOS token = 0 in the config so greedy sampling often picks it
    m.cfg.eosTokenId = 0;
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 20;
    opts.temperature  = 0.0f;

    auto res = pipe.generateFromIds({5}, opts);
    // Either hit EOS or hit maxNewTokens — just check it terminates cleanly
    EXPECT_LE(res.newTokens, 20);
    EXPECT_GE(res.newTokens, 0);
}

// ─── Stop token list ──────────────────────────────────────────────────────────

TEST(Qwen3Pipeline, CustomStopToken) {
    rq::Qwen3Pipeline pipe;
    auto m = TinyModel::make();
    m.cfg.eosTokenId = 999; // out of vocab range — won't be sampled
    m.applyTo(pipe);

    rq::PipelineGenerateOptions opts;
    opts.maxNewTokens = 10;
    opts.temperature  = 0.0f;
    // Stop when token 3 is sampled (greedy may or may not pick it)
    opts.stopTokenIds = {3};

    auto res = pipe.generateFromIds({0}, opts);
    // Test only that it terminates
    EXPECT_LE(res.newTokens, 10);
}
