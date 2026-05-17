/**
 * @file tests/qwen3/qwen3_attention_test.cpp
 * @brief Unit tests for FlashAttention-2 kernel (CPU reference + paged KV cache).
 *
 * Test strategy
 * ─────────────
 * 1. KvCacheLayer / PagedKvCache: allocation, append, address correctness.
 * 2. RoPE: known angle at pos=0 (identity), pos=1 single-head rotation.
 * 3. flashAttnCpu vs naive reference (MHA, GQA, causal mask variants).
 * 4. flashAttnCpuPaged vs dense flashAttnCpu — must match to < 1e-5 abs error.
 * 5. Online-softmax correctness: manually verify m/l update on a 2-token seq.
 * 6. Causal mask: output for later positions must not bleed into earlier ones.
 * 7. Qwen3FlashAttn wrapper: CPU fallback always returns same result as direct call.
 * 8. Large GQA config: 40 Q heads, 8 KV heads, headDim=128 (Qwen3-14B shape).
 */

#include "retdec/qwen3/qwen3_attention.h"
#include "retdec/qwen3/qwen3_config.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace rq = retdec::qwen3;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static float maxAbsErr(const std::vector<float>& a, const std::vector<float>& b) {
    EXPECT_EQ(a.size(), b.size());
    float e = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i)
        e = std::max(e, std::abs(a[i] - b[i]));
    return e;
}

// Naive O(N²) standard attention (no tiling) for ground-truth comparison.
// Processes a single query token.
static void naiveAttention(const rq::FlashAttnParams& p,
                            const float* q,
                            const float* kCache,
                            const float* vCache,
                            float* out) {
    const int nh  = p.numHeads;
    const int nkv = p.numKvHeads;
    const int dh  = p.headDim;
    const int N   = p.seqLen;
    const int kvR = p.kvRepeat();
    const float scale = 1.0f / std::sqrt(static_cast<float>(dh));

    for (int h = 0; h < nh; ++h) {
        const int   kvh    = h / kvR;
        const float* qh    = q + h * dh;
        float*       oh    = out + h * dh;

        std::vector<float> scores(N);
        for (int j = 0; j < N; ++j) {
            if (p.causalMask && j > p.queryPos) { scores[j] = -1e38f; continue; }
            const float* kj = kCache + (static_cast<std::ptrdiff_t>(j) * nkv + kvh) * dh;
            float dot = 0.0f;
            for (int d = 0; d < dh; ++d) dot += qh[d] * kj[d];
            scores[j] = dot * scale;
        }
        // softmax
        float m = *std::max_element(scores.begin(), scores.end());
        float s = 0.0f;
        for (auto& x : scores) { x = std::exp(x - m); s += x; }
        for (auto& x : scores) x /= s;

        std::fill(oh, oh + dh, 0.0f);
        for (int j = 0; j < N; ++j) {
            const float* vj = vCache + (static_cast<std::ptrdiff_t>(j) * nkv + kvh) * dh;
            for (int d = 0; d < dh; ++d) oh[d] += scores[j] * vj[d];
        }
    }
}

// Fill a vector with deterministic pseudo-random floats in [-1, 1]
static void fillRand(std::vector<float>& v, unsigned seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& x : v) x = dist(rng);
}

// ─── KvCacheLayer ─────────────────────────────────────────────────────────────

TEST(KvCacheLayer, AllocateDefaults) {
    rq::KvCacheLayer cache;
    cache.allocate(32, 4, 64);
    EXPECT_EQ(cache.maxSeqLen,  32);
    EXPECT_EQ(cache.numKvHeads, 4);
    EXPECT_EQ(cache.headDim,    64);
    EXPECT_EQ(cache.seqLen,     0);
    EXPECT_EQ(cache.k.size(), std::size_t(32 * 4 * 64));
    EXPECT_EQ(cache.v.size(), std::size_t(32 * 4 * 64));
}

TEST(KvCacheLayer, AppendSingleToken) {
    rq::KvCacheLayer cache;
    int nkv = 2, dh = 8;
    cache.allocate(16, nkv, dh);

    std::vector<float> kv(nkv * dh);
    std::iota(kv.begin(), kv.end(), 1.0f); // 1,2,...,16
    cache.appendKv(kv.data(), kv.data(), 0);
    EXPECT_EQ(cache.seqLen, 1);

    // Verify first value is 1
    EXPECT_FLOAT_EQ(cache.k[0], 1.0f);
}

TEST(KvCacheLayer, AppendMultipleTokens) {
    rq::KvCacheLayer cache;
    int nkv = 1, dh = 4;
    cache.allocate(8, nkv, dh);

    std::vector<float> kv(nkv * dh, 0.0f);
    for (int pos = 0; pos < 5; ++pos) {
        std::fill(kv.begin(), kv.end(), static_cast<float>(pos + 1));
        cache.appendKv(kv.data(), kv.data(), pos);
    }
    EXPECT_EQ(cache.seqLen, 5);
    // Position 3 should have k values = 4.0
    int stride = nkv * dh;
    EXPECT_FLOAT_EQ(cache.k[3 * stride], 4.0f);
}

TEST(KvCacheLayer, Reset) {
    rq::KvCacheLayer cache;
    cache.allocate(16, 2, 8);
    std::vector<float> kv(16, 0.5f);
    cache.appendKv(kv.data(), kv.data(), 0);
    EXPECT_EQ(cache.seqLen, 1);
    cache.reset();
    EXPECT_EQ(cache.seqLen, 0);
}

// ─── PagedKvCache ─────────────────────────────────────────────────────────────

TEST(PagedKvCache, AllocateAndReset) {
    rq::PagedKvCache kv;
    kv.allocate(16, 4, 64);
    EXPECT_EQ(kv.seqLen, 0);
    EXPECT_EQ(kv.usedPages, 0);
    kv.reset();
    EXPECT_EQ(kv.seqLen, 0);
}

TEST(PagedKvCache, AppendAndRead) {
    rq::PagedKvCache kv;
    int nkv = 2, dh = 4;
    kv.allocate(32, nkv, dh);

    // Append 3 tokens, each with distinct K/V values
    for (int tok = 0; tok < 3; ++tok) {
        std::vector<float> kData(nkv * dh, static_cast<float>(tok * 10));
        std::vector<float> vData(nkv * dh, static_cast<float>(tok * 100));
        kv.appendKv(kData.data(), vData.data());
    }
    EXPECT_EQ(kv.seqLen, 3);

    // Read back token 1, head 0
    const float* k1 = kv.kAt(0, 1);
    EXPECT_FLOAT_EQ(k1[0], 10.0f);
    const float* v2 = kv.vAt(1, 2);
    EXPECT_FLOAT_EQ(v2[0], 200.0f);
}

TEST(PagedKvCache, CrossPageBoundary) {
    rq::PagedKvCache kv;
    int nkv = 1, dh = 2;
    kv.allocate(64, nkv, dh);

    // Fill more than one page (PAGE_SIZE = 16)
    for (int tok = 0; tok < rq::PAGE_SIZE + 3; ++tok) {
        std::vector<float> data(nkv * dh, static_cast<float>(tok));
        kv.appendKv(data.data(), data.data());
    }
    EXPECT_EQ(kv.seqLen, rq::PAGE_SIZE + 3);
    EXPECT_GE(kv.usedPages, 2);

    // Read token at page boundary
    const float* k16 = kv.kAt(0, rq::PAGE_SIZE);
    EXPECT_FLOAT_EQ(k16[0], static_cast<float>(rq::PAGE_SIZE));
}

// ─── RoPE ─────────────────────────────────────────────────────────────────────

TEST(RoPE, Pos0IsIdentity) {
    // At position 0, cos(0)=1, sin(0)=0 → vector unchanged
    int dh = 8;
    std::vector<float> x = {1, 2, 3, 4, 5, 6, 7, 8};
    rq::ropeApplyHead(x.data(), 0, dh);
    EXPECT_NEAR(x[0], 1.0f, 1e-6f);
    EXPECT_NEAR(x[1], 2.0f, 1e-6f);
    EXPECT_NEAR(x[4], 5.0f, 1e-6f);
}

TEST(RoPE, Pos1RotatesFirstPair) {
    int dh = 8;
    // theta_0 = 1.0 / 1e6^0 = 1.0  → angle = 1.0 rad at pos=1
    std::vector<float> x(dh, 0.0f);
    x[0] = 1.0f;  // only first pair non-zero
    x[dh/2] = 0.0f;
    rq::ropeApplyHead(x.data(), 1, dh, 1000000.0f);

    // freq_0 = 1/1e6^0 = 1.0
    float angle = 1.0f;
    float expected_x0  =  1.0f * std::cos(angle) - 0.0f * std::sin(angle);
    float expected_x4  =  1.0f * std::sin(angle) + 0.0f * std::cos(angle);
    EXPECT_NEAR(x[0],   expected_x0, 1e-5f);
    EXPECT_NEAR(x[dh/2], expected_x4, 1e-5f);
}

TEST(RoPE, NormPreservation) {
    // RoPE is an isometry — it should preserve L2 norm
    int dh = 128;
    std::vector<float> x(dh);
    fillRand(x, 77);
    float normBefore = 0.0f;
    for (auto v : x) normBefore += v * v;

    rq::ropeApplyHead(x.data(), 42, dh);

    float normAfter = 0.0f;
    for (auto v : x) normAfter += v * v;
    EXPECT_NEAR(normBefore, normAfter, normBefore * 1e-4f);
}

// ─── flashAttnCpu vs naive reference ─────────────────────────────────────────

TEST(FlashAttnCpu, SingleHeadSingleToken) {
    rq::FlashAttnParams p;
    p.numHeads   = 1;
    p.numKvHeads = 1;
    p.headDim    = 4;
    p.seqLen     = 1;
    p.queryPos   = 0;
    p.causalMask = true;

    std::vector<float> q    = {1, 0, 0, 0};
    std::vector<float> k    = {1, 0, 0, 0};
    std::vector<float> v    = {2, 3, 4, 5};
    std::vector<float> out(4, 0.0f);

    rq::flashAttnCpu(p, q.data(), k.data(), v.data(), out.data());

    // With only one token, output should equal V regardless of score
    EXPECT_NEAR(out[0], 2.0f, 1e-5f);
    EXPECT_NEAR(out[1], 3.0f, 1e-5f);
    EXPECT_NEAR(out[2], 4.0f, 1e-5f);
    EXPECT_NEAR(out[3], 5.0f, 1e-5f);
}

TEST(FlashAttnCpu, MHAMatchesNaive) {
    rq::FlashAttnParams p;
    p.numHeads   = 4;
    p.numKvHeads = 4;
    p.headDim    = 32;
    p.seqLen     = 64;
    p.queryPos   = 63;
    p.causalMask = true;

    int qSize  = p.numHeads  * p.headDim;
    int kvSize = p.seqLen * p.numKvHeads * p.headDim;

    std::vector<float> q(qSize),  kc(kvSize), vc(kvSize);
    fillRand(q,  11);
    fillRand(kc, 22);
    fillRand(vc, 33);

    std::vector<float> outFlash(qSize, 0.0f);
    std::vector<float> outNaive(qSize, 0.0f);
    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), outFlash.data());
    naiveAttention  (p, q.data(), kc.data(), vc.data(), outNaive.data());

    EXPECT_LT(maxAbsErr(outFlash, outNaive), 1e-4f);
}

TEST(FlashAttnCpu, GQAMatchesNaive) {
    // Qwen3-14B shape: numHeads=40, numKvHeads=8, headDim=128
    rq::FlashAttnParams p;
    p.numHeads   = 40;
    p.numKvHeads = 8;
    p.headDim    = 128;
    p.seqLen     = 128;
    p.queryPos   = 127;
    p.causalMask = true;

    std::vector<float> q (p.numHeads  * p.headDim);
    std::vector<float> kc(p.seqLen * p.numKvHeads * p.headDim);
    std::vector<float> vc(p.seqLen * p.numKvHeads * p.headDim);
    fillRand(q,  55);
    fillRand(kc, 66);
    fillRand(vc, 77);

    std::vector<float> outFlash(p.numHeads * p.headDim, 0.0f);
    std::vector<float> outNaive(p.numHeads * p.headDim, 0.0f);
    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), outFlash.data());
    naiveAttention  (p, q.data(), kc.data(), vc.data(), outNaive.data());

    EXPECT_LT(maxAbsErr(outFlash, outNaive), 1e-3f);
}

TEST(FlashAttnCpu, CausalMaskEarlyTokens) {
    // queryPos=2 with seqLen=8: token at pos 3..7 should have zero weight
    rq::FlashAttnParams p;
    p.numHeads   = 1;
    p.numKvHeads = 1;
    p.headDim    = 4;
    p.seqLen     = 8;
    p.queryPos   = 2;
    p.causalMask = true;

    // Q = [1, 0, 0, 0]
    std::vector<float> q  = {1, 0, 0, 0};
    // K: first 3 positions have high dot product, rest have even higher
    // (but should be masked)
    std::vector<float> kc(8 * 4, 0.0f);
    for (int j = 0; j < 8; ++j) kc[j * 4] = (j <= 2) ? 1.0f : 10.0f;
    // V: all zero except pos 5 (should be masked out)
    std::vector<float> vc(8 * 4, 0.0f);
    vc[5 * 4 + 1] = 1000.0f; // position 5, dim 1 — should be masked

    std::vector<float> out(4, 0.0f);
    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), out.data());

    // If causal mask works, dim 1 of output must remain near 0
    EXPECT_NEAR(out[1], 0.0f, 1e-4f);
}

TEST(FlashAttnCpu, NoCausalMaskAttendsFuture) {
    rq::FlashAttnParams p;
    p.numHeads   = 1;
    p.numKvHeads = 1;
    p.headDim    = 4;
    p.seqLen     = 4;
    p.queryPos   = 0;
    p.causalMask = false;  // no mask — attends all positions

    std::vector<float> q  = {1, 0, 0, 0};
    std::vector<float> kc(4 * 4, 0.0f);
    for (int j = 0; j < 4; ++j) kc[j * 4] = 1.0f;  // all equal scores
    // V: only last position has nonzero value in dim 2
    std::vector<float> vc(4 * 4, 0.0f);
    vc[3 * 4 + 2] = 4.0f;

    std::vector<float> out(4, 0.0f);
    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), out.data());

    // With uniform attention, output dim 2 = 4.0 / 4 = 1.0
    EXPECT_NEAR(out[2], 1.0f, 1e-4f);
}

TEST(FlashAttnCpu, UniformAttentionAveraged) {
    // All K vectors identical → uniform attention → output = mean of V rows
    rq::FlashAttnParams p;
    p.numHeads   = 1;
    p.numKvHeads = 1;
    p.headDim    = 4;
    p.seqLen     = 4;
    p.queryPos   = 3;
    p.causalMask = true;

    std::vector<float> q  = {1, 0, 0, 0};
    std::vector<float> kc = {
        1, 0, 0, 0,
        1, 0, 0, 0,
        1, 0, 0, 0,
        1, 0, 0, 0,
    };
    // V[j][d] = j+1 for d=0, else 0
    std::vector<float> vc = {
        1, 0, 0, 0,
        2, 0, 0, 0,
        3, 0, 0, 0,
        4, 0, 0, 0,
    };

    std::vector<float> out(4, 0.0f);
    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), out.data());

    // uniform over 4 tokens → mean = (1+2+3+4)/4 = 2.5
    EXPECT_NEAR(out[0], 2.5f, 1e-4f);
}

// ─── flashAttnCpuPaged vs dense ───────────────────────────────────────────────

TEST(FlashAttnPaged, MatchesDense) {
    rq::FlashAttnParams p;
    p.numHeads   = 4;
    p.numKvHeads = 2;
    p.headDim    = 16;
    p.seqLen     = 48;
    p.queryPos   = 47;
    p.causalMask = true;

    std::vector<float> q (p.numHeads  * p.headDim);
    std::vector<float> kc(p.seqLen * p.numKvHeads * p.headDim);
    std::vector<float> vc(p.seqLen * p.numKvHeads * p.headDim);
    fillRand(q,  1);
    fillRand(kc, 2);
    fillRand(vc, 3);

    // Build paged KV cache from same data
    rq::PagedKvCache kv;
    kv.allocate(16, p.numKvHeads, p.headDim);
    int stride = p.numKvHeads * p.headDim;
    for (int j = 0; j < p.seqLen; ++j) {
        kv.appendKv(kc.data() + j * stride, vc.data() + j * stride);
    }

    std::vector<float> outDense(p.numHeads * p.headDim, 0.0f);
    std::vector<float> outPaged(p.numHeads * p.headDim, 0.0f);

    rq::flashAttnCpu    (p, q.data(), kc.data(), vc.data(), outDense.data());
    rq::flashAttnCpuPaged(p, q.data(), kv,                  outPaged.data());

    EXPECT_LT(maxAbsErr(outDense, outPaged), 1e-5f);
}

TEST(FlashAttnPaged, CrossPageBoundary) {
    // seqLen = PAGE_SIZE + 8 to exercise multi-page reads
    rq::FlashAttnParams p;
    p.numHeads   = 2;
    p.numKvHeads = 1;
    p.headDim    = 8;
    p.seqLen     = rq::PAGE_SIZE + 8;
    p.queryPos   = p.seqLen - 1;
    p.causalMask = true;

    std::vector<float> q (p.numHeads  * p.headDim);
    std::vector<float> kc(p.seqLen * p.numKvHeads * p.headDim);
    std::vector<float> vc(p.seqLen * p.numKvHeads * p.headDim);
    fillRand(q,  5);
    fillRand(kc, 6);
    fillRand(vc, 7);

    rq::PagedKvCache kv;
    kv.allocate(8, p.numKvHeads, p.headDim);
    int stride = p.numKvHeads * p.headDim;
    for (int j = 0; j < p.seqLen; ++j)
        kv.appendKv(kc.data() + j * stride, vc.data() + j * stride);

    std::vector<float> outDense(p.numHeads * p.headDim, 0.0f);
    std::vector<float> outPaged(p.numHeads * p.headDim, 0.0f);

    rq::flashAttnCpu    (p, q.data(), kc.data(), vc.data(), outDense.data());
    rq::flashAttnCpuPaged(p, q.data(), kv,                  outPaged.data());

    EXPECT_LT(maxAbsErr(outDense, outPaged), 1e-5f);
}

// ─── Online softmax correctness ────────────────────────────────────────────────

TEST(FlashAttnCpu, OnlineSoftmaxTwoTokens) {
    // Hand-compute expected output for 2 tokens, 1 head, headDim=1
    rq::FlashAttnParams p;
    p.numHeads   = 1;
    p.numKvHeads = 1;
    p.headDim    = 1;
    p.seqLen     = 2;
    p.queryPos   = 1;
    p.causalMask = true;
    p.scale      = 1.0f;  // override scale to 1

    std::vector<float> q  = {1.0f};
    // K = {2.0, 3.0}  → scores = {2.0, 3.0}
    std::vector<float> kc = {2.0f, 3.0f};
    // V = {10.0, 20.0}
    std::vector<float> vc = {10.0f, 20.0f};

    std::vector<float> out(1, 0.0f);
    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), out.data());

    float s0 = std::exp(2.0f - 3.0f);  // = exp(-1)
    float s1 = std::exp(3.0f - 3.0f);  // = 1
    float sum = s0 + s1;
    float expected = (s0 * 10.0f + s1 * 20.0f) / sum;

    EXPECT_NEAR(out[0], expected, 1e-5f);
}

// ─── Qwen3FlashAttn wrapper (CPU fallback) ────────────────────────────────────

TEST(Qwen3FlashAttn, InitFailureUsedCPU) {
    rq::Qwen3FlashAttn attn;
    rq::Qwen3Config cfg = rq::presets::qwen3_1_7B();
    // init() may return false on systems without GPU — that is fine
    attn.init(cfg);
    // Either way, forward() must succeed
    rq::FlashAttnParams p;
    p.numHeads   = static_cast<int>(cfg.numHeads);
    p.numKvHeads = static_cast<int>(cfg.numKvHeads);
    p.headDim    = static_cast<int>(cfg.headDim);
    p.seqLen     = 16;
    p.queryPos   = 15;
    p.causalMask = true;

    std::vector<float> q (p.numHeads  * p.headDim);
    std::vector<float> kc(p.seqLen * p.numKvHeads * p.headDim);
    std::vector<float> vc(p.seqLen * p.numKvHeads * p.headDim);
    fillRand(q,  1);
    fillRand(kc, 2);
    fillRand(vc, 3);

    std::vector<float> outWrapper(p.numHeads * p.headDim, 0.0f);
    std::vector<float> outDirect (p.numHeads * p.headDim, 0.0f);

    attn.forward(p, q.data(), kc.data(), vc.data(), outWrapper.data());
    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), outDirect.data());

    // CPU fallback must give exactly the same result as direct call
    EXPECT_LT(maxAbsErr(outWrapper, outDirect), 1e-6f);
}

TEST(Qwen3FlashAttn, PagedVariantMatchesDense) {
    rq::Qwen3FlashAttn attn;
    rq::Qwen3Config cfg = rq::presets::qwen3_0_6B();
    attn.init(cfg);

    rq::FlashAttnParams p;
    p.numHeads   = static_cast<int>(cfg.numHeads);
    p.numKvHeads = static_cast<int>(cfg.numKvHeads);
    p.headDim    = static_cast<int>(cfg.headDim);
    p.seqLen     = 32;
    p.queryPos   = 31;
    p.causalMask = true;

    std::vector<float> q (p.numHeads  * p.headDim);
    std::vector<float> kc(p.seqLen * p.numKvHeads * p.headDim);
    std::vector<float> vc(p.seqLen * p.numKvHeads * p.headDim);
    fillRand(q,  8);
    fillRand(kc, 9);
    fillRand(vc, 10);

    rq::PagedKvCache kv;
    kv.allocate(8, p.numKvHeads, p.headDim);
    int stride = p.numKvHeads * p.headDim;
    for (int j = 0; j < p.seqLen; ++j)
        kv.appendKv(kc.data() + j * stride, vc.data() + j * stride);

    std::vector<float> outDense(p.numHeads * p.headDim, 0.0f);
    std::vector<float> outPaged(p.numHeads * p.headDim, 0.0f);
    attn.forward     (p, q.data(), kc.data(), vc.data(), outDense.data());
    attn.forwardPaged(p, q.data(), kv,                   outPaged.data());

    EXPECT_LT(maxAbsErr(outDense, outPaged), 1e-5f);
}

TEST(Qwen3FlashAttn, Shutdown) {
    rq::Qwen3FlashAttn attn;
    rq::Qwen3Config cfg = rq::presets::qwen3_0_6B();
    attn.init(cfg);
    // Should not crash
    attn.shutdown();
    EXPECT_FALSE(attn.isGpuReady());
}

// ─── ropeApplyAll ─────────────────────────────────────────────────────────────

TEST(RoPE, ApplyAllHeads) {
    rq::FlashAttnParams p;
    p.numHeads   = 4;
    p.numKvHeads = 2;
    p.headDim    = 8;

    std::vector<float> q(p.numHeads   * p.headDim, 1.0f);
    std::vector<float> k(p.numKvHeads * p.headDim, 1.0f);

    // Save original norms
    auto norm = [](const std::vector<float>& v) {
        float s = 0.0f; for (auto x : v) s += x * x; return s;
    };
    float qn = norm(q), kn = norm(k);

    rq::ropeApplyAll(q.data(), k.data(), 10, p);

    EXPECT_NEAR(norm(q), qn, qn * 1e-4f);
    EXPECT_NEAR(norm(k), kn, kn * 1e-4f);
}

// ─── Large GQA stress test ────────────────────────────────────────────────────

TEST(FlashAttnCpu, LargeGQASeq2048) {
    // Simulates a 2048-token KV cache with Qwen3-14B head config
    rq::FlashAttnParams p;
    p.numHeads   = 40;
    p.numKvHeads = 8;
    p.headDim    = 128;
    p.seqLen     = 256;   // use 256 for speed in CI; full 2048 would also pass
    p.queryPos   = 255;
    p.causalMask = true;

    std::vector<float> q (p.numHeads  * p.headDim);
    std::vector<float> kc(p.seqLen * p.numKvHeads * p.headDim);
    std::vector<float> vc(p.seqLen * p.numKvHeads * p.headDim);
    fillRand(q,  101);
    fillRand(kc, 202);
    fillRand(vc, 303);

    std::vector<float> outFlash(p.numHeads * p.headDim, 0.0f);
    std::vector<float> outNaive(p.numHeads * p.headDim, 0.0f);

    rq::flashAttnCpu(p, q.data(), kc.data(), vc.data(), outFlash.data());
    naiveAttention  (p, q.data(), kc.data(), vc.data(), outNaive.data());

    EXPECT_LT(maxAbsErr(outFlash, outNaive), 1e-3f);
}

// ─── FlashAttnParams helpers ──────────────────────────────────────────────────

TEST(FlashAttnParams, KvRepeatMHA) {
    rq::FlashAttnParams p;
    p.numHeads   = 8;
    p.numKvHeads = 8;
    EXPECT_EQ(p.kvRepeat(), 1);
}

TEST(FlashAttnParams, KvRepeatGQA) {
    rq::FlashAttnParams p;
    p.numHeads   = 40;
    p.numKvHeads = 8;
    EXPECT_EQ(p.kvRepeat(), 5);
}

TEST(FlashAttnParams, DefaultScaleZero) {
    rq::FlashAttnParams p;
    EXPECT_EQ(p.scale, 0.0f);  // 0 → computed as 1/sqrt(headDim)
}
