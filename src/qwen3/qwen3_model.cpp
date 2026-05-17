/**
 * @file src/qwen3/qwen3_model.cpp
 * @brief Qwen3 transformer — GGUF loading and autoregressive inference.
 */

#include <memory>
#include "retdec/qwen3/qwen3_model.h"
#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_ops.h"
#include "retdec/qwen3/qwen3_sampler.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace retdec::qwen3 {

using namespace ops;

// ─── Ctor / Dtor ─────────────────────────────────────────────────────────────

Qwen3Model::Qwen3Model()  = default;
Qwen3Model::~Qwen3Model() { disableCUDA(); }

// ─── CUDA enable / disable ────────────────────────────────────────────────────

bool Qwen3Model::enableCUDA(float gpuFraction, int deviceIndex) {
    if (!loaded_) {
        lastError_ = "Model not loaded — call loadGguf() first";
        return false;
    }

    cuda_ = std::make_unique<Qwen3CUDA>();
    if (!cuda_->init(gpuFraction, deviceIndex)) {
        lastError_ = "CUDA init failed: " + cuda_->lastError();
        cuda_.reset();
        return false;
    }

    // Pre-upload all weight tensors to GPU
    auto upload = [&](const std::vector<uint8_t>& buf) {
        if (!buf.empty())
            cuda_->uploadWeight(buf.data(), buf.data(), buf.size());
    };

    upload(embedWeight_);
    upload(lmHead_);
    for (auto& L : layers_) {
        upload(L.wq);  upload(L.wk);  upload(L.wv);  upload(L.wo);
        upload(L.wGate); upload(L.wUp); upload(L.wDown);
    }

    ops::setCUDA(cuda_.get());

    std::fprintf(stderr,
        "[qwen3] CUDA enabled — %s (%.0f MB VRAM used for weights)\n",
        cuda_->deviceInfo().name.c_str(),
        cuda_->weightBytesOnGpu() / 1e6);
    return true;
}

void Qwen3Model::disableCUDA() {
    if (cuda_) {
        ops::setCUDA(nullptr);
        cuda_->shutdown();
        cuda_.reset();
    }
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

bool Qwen3Model::extractTensor(Qwen3Weights& w, const std::string& name,
                                std::vector<uint8_t>& out, GgufDtype& dtype) {
    auto tv = w.load(name);
    if (!tv) return false;
    out   = tv->copyRawBytes();
    dtype = tv->dtype();
    return true;
}

static std::string layerName(int layer, const char* suffix) {
    return "blk." + std::to_string(layer) + "." + suffix;
}

bool Qwen3Model::loadWeightsFromGguf(Qwen3Weights& w) {
    // Embedding + final norm + lm_head
    if (!extractTensor(w, "token_embd.weight", embedWeight_, embedDtype_)) {
        lastError_ = "Missing 'token_embd.weight'";
        return false;
    }
    {
        GgufDtype d; std::vector<uint8_t> buf;
        if (!extractTensor(w, "output_norm.weight", buf, d)) {
            lastError_ = "Missing 'output_norm.weight'";
            return false;
        }
        // output_norm must be F32 for rmsnorm — dequantize if needed
        TensorView tv(buf, d, {static_cast<int64_t>(cfg_.hiddenSize)});
        auto f32 = tv.dataF32().value_or(std::vector<float>(cfg_.hiddenSize, 0.f));
        outputNorm_.resize(f32.size() * sizeof(float));
        std::memcpy(outputNorm_.data(), f32.data(), outputNorm_.size());
    }

    // lm_head may be tied to embedding weights
    {
        GgufDtype lhDtype;
        std::vector<uint8_t> lhBuf;
        if (extractTensor(w, "output.weight", lhBuf, lhDtype)) {
            lmHead_     = std::move(lhBuf);
            lmHeadDtype_= lhDtype;
            lmHeadTied_ = false;
        } else {
            lmHeadTied_ = true;
        }
    }

    // Per-layer weights
    layers_.resize(cfg_.numLayers);
    for (int i = 0; i < cfg_.numLayers; ++i) {
        LayerWeights& L = layers_[i];
        auto load = [&](const char* suf, std::vector<uint8_t>& buf,
                        GgufDtype& dt) -> bool {
            if (!extractTensor(w, layerName(i, suf), buf, dt)) {
                lastError_ = "Missing '" + layerName(i, suf) + "'";
                return false;
            }
            return true;
        };
        // Norms — keep as F32 (dequantize)
        auto loadNorm = [&](const char* suf, std::vector<uint8_t>& buf,
                             GgufDtype& dt) -> bool {
            std::vector<uint8_t> raw; GgufDtype rawDt;
            if (!extractTensor(w, layerName(i, suf), raw, rawDt)) {
                lastError_ = "Missing '" + layerName(i, suf) + "'";
                return false;
            }
            TensorView tv(raw, rawDt, {static_cast<int64_t>(cfg_.hiddenSize)});
            auto f32 = tv.dataF32().value_or(
                           std::vector<float>(cfg_.hiddenSize, 0.f));
            buf.resize(f32.size() * sizeof(float));
            std::memcpy(buf.data(), f32.data(), buf.size());
            dt = GgufDtype::F32;
            return true;
        };

        if (!loadNorm("attn_norm.weight", L.attnNorm, L.attnNormDtype)) return false;
        if (!load("attn_q.weight",        L.wq,       L.wqDtype))       return false;
        if (!load("attn_k.weight",        L.wk,       L.wkDtype))       return false;
        if (!load("attn_v.weight",        L.wv,       L.wvDtype))       return false;
        if (!load("attn_output.weight",   L.wo,       L.woDtype))       return false;
        if (!loadNorm("ffn_norm.weight",  L.ffnNorm,  L.ffnNormDtype))  return false;
        if (!load("ffn_gate.weight",      L.wGate,    L.wGateDtype))    return false;
        if (!load("ffn_up.weight",        L.wUp,      L.wUpDtype))      return false;
        if (!load("ffn_down.weight",      L.wDown,    L.wDownDtype))    return false;
    }
    return true;
}

bool Qwen3Model::loadVocabFromGguf(Qwen3Weights& w) {
    if (!tok_.loadFromGguf(w)) {
        lastError_ = tok_.lastError();
        return false;
    }
    return true;
}

// ─── Public: loadGguf ─────────────────────────────────────────────────────────

bool Qwen3Model::loadGguf(const std::string& path) {
    Qwen3Weights w;
    if (!w.openGguf(path)) {
        lastError_ = "Failed to open GGUF: " + path;
        return false;
    }

    // Extract config
    cfg_ = w.extractConfig();

    // Override with GGUF context length if present
    if (cfg_.maxPositionEmbeddings == 0) cfg_.maxPositionEmbeddings = 8192;

    // Load vocabulary from GGUF metadata
    if (!loadVocabFromGguf(w)) {
        // Non-fatal — user can load tokenizer separately
        std::cerr << "[qwen3] Warning: " << lastError_
                  << "\n         Call loadTokenizer() to load from file.\n";
        lastError_.clear();
    }

    if (!loadWeightsFromGguf(w)) return false;

    // Allocate scratch buffers
    int h  = cfg_.hiddenSize;
    int nh = cfg_.numHeads;
    int nk = cfg_.numKvHeads;
    int dh = cfg_.headDim;
    int di = cfg_.intermediateSize;
    int v  = cfg_.vocabSize;
    int ms = cfg_.maxPositionEmbeddings;

    x_.resize(h);
    h_.resize(h);
    q_.resize(nh * dh);
    k_.resize(nk * dh);
    v_.resize(nk * dh);
    attnOut_.resize(nh * dh);
    gate_.resize(di);
    up_.resize(di);
    logits_.resize(v);
    scratch_.resize(ms);

    // Allocate KV cache
    kvCache_.resize(cfg_.numLayers);
    for (auto& lkv : kvCache_) {
        lkv.keys.resize(ms * nk * dh, 0.f);
        lkv.values.resize(ms * nk * dh, 0.f);
        lkv.seqLen = 0;
    }

    loaded_ = true;
    return true;
}

bool Qwen3Model::loadTokenizer(const std::string& path) {
    if (!tok_.loadFromFile(path)) {
        lastError_ = "Failed to load tokenizer: " + path;
        return false;
    }
    return true;
}

// ─── resetKvCache ─────────────────────────────────────────────────────────────

void Qwen3Model::resetKvCache() {
    for (auto& lkv : kvCache_) {
        std::fill(lkv.keys.begin(),   lkv.keys.end(),   0.f);
        std::fill(lkv.values.begin(), lkv.values.end(), 0.f);
        lkv.seqLen = 0;
    }
}

// ─── forward (single token) ───────────────────────────────────────────────────

void Qwen3Model::runLayer(int layer, float* x, int pos) {
    LayerWeights& L  = layers_[layer];
    KvLayer&      kv = kvCache_[layer];
    int h   = cfg_.hiddenSize;
    int nh  = cfg_.numHeads;
    int nk  = cfg_.numKvHeads;
    int dh  = cfg_.headDim;
    int di  = cfg_.intermediateSize;

    // ── Attention ──────────────────────────────────────────────────────────────
    copyVec(h_.data(), x, h);
    rmsnorm(h_.data(),
            reinterpret_cast<const float*>(L.attnNorm.data()), h);

    // Q, K, V projections
    gemv(L.wq, L.wqDtype, nh * dh, h, h_.data(), q_.data());
    gemv(L.wk, L.wkDtype, nk * dh, h, h_.data(), k_.data());
    gemv(L.wv, L.wvDtype, nk * dh, h, h_.data(), v_.data());

    // RoPE
    rope(q_.data(), k_.data(), pos, dh, nh, nk, cfg_.ropeTheta);

    // Store K, V into cache at position `pos`
    int kvOffset = pos * nk * dh;
    copyVec(kv.keys.data()   + kvOffset, k_.data(), nk * dh);
    copyVec(kv.values.data() + kvOffset, v_.data(), nk * dh);
    kv.seqLen = pos + 1;

    // GQA attention
    gqaAttention(q_.data(),
                 kv.keys.data(), kv.values.data(),
                 kv.seqLen, nh, nk, dh,
                 attnOut_.data(), scratch_.data());

    // Output projection + residual
    gemv(L.wo, L.woDtype, h, nh * dh, attnOut_.data(), h_.data());
    addVec(x, h_.data(), h);

    // ── FFN ────────────────────────────────────────────────────────────────────
    copyVec(h_.data(), x, h);
    rmsnorm(h_.data(),
            reinterpret_cast<const float*>(L.ffnNorm.data()), h);

    gemv(L.wGate, L.wGateDtype, di, h, h_.data(), gate_.data());
    gemv(L.wUp,   L.wUpDtype,   di, h, h_.data(), up_.data());
    siluHadamard(gate_.data(), up_.data(), di);
    gemv(L.wDown, L.wDownDtype, h, di, gate_.data(), h_.data());
    addVec(x, h_.data(), h);
}

void Qwen3Model::forward(TokenId tokenId, int pos) {
    assert(loaded_);
    int h = cfg_.hiddenSize;
    int v = cfg_.vocabSize;

    // Token embedding lookup — weights stored quantized
    // GGUF dims are innermost-first: dims[0]=hiddenSize, dims[1]=vocabSize
    {
        TensorView tv(embedWeight_, embedDtype_,
                      {static_cast<int64_t>(h),
                       static_cast<int64_t>(cfg_.vocabSize)});
        auto row = tv.row(tokenId);
        copyVec(x_.data(), row.data(), h);
    }

    // Transformer layers
    for (int i = 0; i < cfg_.numLayers; ++i)
        runLayer(i, x_.data(), pos);

    // Final norm
    rmsnorm(x_.data(),
            reinterpret_cast<const float*>(outputNorm_.data()), h);

    // LM head: [vocab × hidden]
    if (lmHeadTied_) {
        // Tied weights: lm_head = embedding matrix transposed
        // logits[i] = dot(embed_row[i], x)
        // GGUF dims innermost-first: dims[0]=hiddenSize, dims[1]=vocabSize
        TensorView tv(embedWeight_, embedDtype_,
                      {static_cast<int64_t>(h),
                       static_cast<int64_t>(cfg_.vocabSize)});
        for (int i = 0; i < v; ++i) {
            auto emrow = tv.row(i);
            float s = 0.f;
            for (int d = 0; d < h; ++d) s += emrow[d] * x_[d];
            logits_[i] = s;
        }
    } else {
        gemv(lmHead_, lmHeadDtype_, v, h, x_.data(), logits_.data());
    }
}

// ─── generateFromIds ─────────────────────────────────────────────────────────

GenerateResult Qwen3Model::generateFromIds(const TokenIds& promptIds,
                                            const GenerateOptions& opts) {
    assert(loaded_);
    resetKvCache();

    SamplerConfig sc;
    sc.temperature       = opts.temperature;
    sc.topP              = opts.topP;
    sc.topK              = opts.topK;
    sc.repetitionPenalty = opts.repetitionPenalty;
    sc.seed              = opts.seed;
    Qwen3Sampler sampler(sc);

    GenerateResult res;
    res.promptTokens = static_cast<int>(promptIds.size());

    auto t0 = std::chrono::steady_clock::now();

    // Prefill prompt
    int pos = 0;
    for (auto tid : promptIds) {
        forward(tid, pos++);
    }

    // Collect stop tokens
    std::vector<TokenId> stopSet = opts.stopTokens;
    stopSet.push_back(Qwen3Tokenizer::EOS_TOKEN_ID);
    stopSet.push_back(Qwen3Tokenizer::IM_END_TOKEN_ID);

    // Generate
    std::vector<TokenId> history(promptIds.begin(), promptIds.end());
    std::vector<float>   logitsBuf(logits_.size());
    int generated = 0;

    while (generated < opts.maxNewTokens) {
        // Copy logits so sampler can modify in place
        std::copy(logits_.begin(), logits_.end(), logitsBuf.begin());

        TokenId next = sampler.sample(logitsBuf.data(),
                                      cfg_.vocabSize, history);

        // Check stop
        bool stop = false;
        for (auto s : stopSet) if (next == s) { stop = true; break; }
        if (stop) { res.hitEos = true; break; }

        history.push_back(next);
        res.tokenIds.push_back(next);
        ++generated;

        std::string piece = tok_.decode({next});
        res.text += piece;

        if (opts.tokenCallback) opts.tokenCallback(next);
        if (opts.textCallback)  opts.textCallback(piece);

        // Advance
        forward(next, pos++);
        if (pos >= cfg_.maxPositionEmbeddings) break;
    }

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    res.newTokens    = generated;
    res.tokensPerSec = (sec > 0) ? generated / sec : 0;

    return res;
}

// ─── generate (plain-text prompt) ────────────────────────────────────────────

GenerateResult Qwen3Model::generate(const std::string& prompt,
                                     const GenerateOptions& opts) {
    // Apply Qwen3 ChatML template
    std::string formatted =
        "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n"
        "<|im_start|>user\n" + prompt + "<|im_end|>\n"
        "<|im_start|>assistant\n";
    if (opts.enableThinking) formatted += "<think>\n";

    TokenIds ids = tok_.encode(formatted);
    return generateFromIds(ids, opts);
}

} // namespace retdec::qwen3
