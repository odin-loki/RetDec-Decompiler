/**
 * @file src/qwen3/qwen3_pipeline.cpp
 * @brief Full Qwen3 inference pipeline implementation.
 *
 * Forward pass per token
 * Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 *   x   = embed(token_id)         [hidden]
 *   for l in [0, numLayers):
 *     h  = rmsnorm(x, attnNorm_l)
 *     q  = gemv(Wq, h)   k = gemv(Wk, h)   v = gemv(Wv, h)
 *     rope(q, k, pos)
 *     kvCache_l.appendKv(k, v, pos)
 *     attn.forward(params, q, kCache, vCache, attnOut)
 *     x += gemv(Wo, attnOut)
 *     h  = rmsnorm(x, ffnNorm_l)
 *     if dense:  x += ffn(h)         (gate/up/down SwiGLU)
 *     if moe:    x += moeLayer_l(h)
 *   logits = gemv(lmHead, rmsnorm(x, outputNorm))
 *
 * Generation loop
 * Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 *   1. Tokenize prompt Ã¢â€ â€™ promptIds
 *   2. Prefill: for tok in promptIds: forward(tok, pos++), build KV cache
 *   3. Decode:  while !stop: tok = sample(logits); forward(tok, pos++)
 *
 * KV cache variants
 * Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
 *   Dense:  KvCacheLayer Ã¢â‚¬â€ simple flat buffers, O(maxSeqÃƒâ€”nkvÃƒâ€”hd) memory
 *   Paged:  PagedKvCache  Ã¢â‚¬â€ page-table allocation, avoids large contiguous allocs
 */

#include <memory>
#include "retdec/qwen3/qwen3_pipeline.h"
#include "retdec/qwen3/qwen3_attention.h"
#include "retdec/qwen3/qwen3_moe.h"
#include "retdec/qwen3/qwen3_cuda.h"
#include "retdec/qwen3/qwen3_ops.h"
#include "retdec/qwen3/qwen3_tokenizer.h"
#include "retdec/qwen3/qwen3_trace.h"
#include "retdec/qwen3/qwen3_weights.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace retdec::qwen3 {

namespace {

/// Same cap as initKvCaches() — max sequence positions stored in dense KV (0 .. cap-1).
int kvSeqCap(const Qwen3Config& cfg, int maxContextLen_) {
    int modelMax = static_cast<int>(cfg.maxPositionEmbeddings);
    int maxSeq   = (maxContextLen_ > 0)
                       ? std::min(modelMax, maxContextLen_)
                       : modelMax;
    return std::max(maxSeq, 512);
}

/// GGUF 2-D matrices: dims[0] is inner (e.g. hidden), dims[1] is outer row count (e.g. vocab).
uint32_t ggufOuterRowCount(const TensorView& tv) {
    return tv.nDims() >= 2 ? static_cast<uint32_t>(tv.dim(1)) : 0u;
}

/// Vocabulary row count for embedding / lm_head: prefer dim that matches `hidden` as inner width.
uint32_t inferVocabRowsFrom2D(const TensorView& tv, uint32_t hidden) {
    if (tv.nDims() < 2 || hidden == 0) return ggufOuterRowCount(tv);
    const uint64_t d0 = tv.dim(0);
    const uint64_t d1 = tv.dim(1);
    const uint64_t ne = tv.nElements();
    if (d0 == static_cast<uint64_t>(hidden)) return static_cast<uint32_t>(d1);
    if (d1 == static_cast<uint64_t>(hidden)) return static_cast<uint32_t>(d0);
    if (ne % static_cast<uint64_t>(hidden) == 0u)
        return static_cast<uint32_t>(ne / static_cast<uint64_t>(hidden));
    return ggufOuterRowCount(tv);
}

/// Raw byte size of a quantized GGUF matrix with gemv layout [rows × cols], cols innermost.
std::size_t ggufQuantMatrixBytes(GgufDtype dt, uint64_t rows, uint64_t cols) {
    TensorInfo ti{};
    ti.dtype     = dt;
    ti.nElements = rows * cols;
    return static_cast<std::size_t>(ti.dataSizeBytes());
}

} // namespace

// --- Construction / destruction ---

Qwen3Pipeline::Qwen3Pipeline()  = default;
Qwen3Pipeline::~Qwen3Pipeline() { disableCUDA(); }

// --- CUDA ---

bool Qwen3Pipeline::enableCUDA(float gpuFraction, int deviceIndex) {
    cuda_ = std::make_unique<Qwen3CUDA>();
    if (!cuda_->init(gpuFraction, deviceIndex)) {
        lastError_ = cuda_->lastError();
        cuda_.reset();
        return false;
    }
    // Do not call ops::setCUDA(cuda_.get()).  This pipeline uses ops::gemv() (CPU) for
    // all attention/FFN/lm_head matmuls.  Registering the global CUDA backend would
    // send MoE router matmuls through Qwen3CUDA::gemv (GPU + std::async CPU split),
    // which has produced hard crashes on Windows (often on the first token) when the
    // same GPU is also used for Qt/D3D.  Initialising cuda_ here still validates the
    // driver and keeps isCUDAEnabled() meaningful for diagnostics.
    return true;
}

void Qwen3Pipeline::disableCUDA() {
    ops::setCUDA(nullptr);
    if (cuda_) { cuda_->shutdown(); cuda_.reset(); }
}

bool Qwen3Pipeline::isCUDAEnabled() const {
    return cuda_ && cuda_->isReady();
}

// --- KV cache initialisation ---

void Qwen3Pipeline::initKvCaches() {
    int n  = static_cast<int>(cfg_.numLayers);
    int nk = static_cast<int>(cfg_.numKvHeads);
    int hd = static_cast<int>(cfg_.headDim);
    // Cap the context window (shared formula with forward / generateFromIds).
    int maxSeq = kvSeqCap(cfg_, maxContextLen_);

    // Estimate and warn if the allocation looks large.
    // Each layer needs 2 Ãƒâ€” maxSeq Ãƒâ€” nk Ãƒâ€” hd Ãƒâ€” sizeof(float) bytes.
    std::size_t bytesPerLayer =
        2ULL * static_cast<std::size_t>(maxSeq) * nk * hd * sizeof(float);
    std::size_t totalBytes = static_cast<std::size_t>(n) * bytesPerLayer;
    if (totalBytes > 4ULL * 1024 * 1024 * 1024) {
        // Over 4 GB Ã¢â‚¬â€ warn but don't block; the user set maxContextLen_ explicitly.
        lastError_ = "Warning: KV cache will consume ~" +
                     std::to_string(totalBytes / (1024 * 1024)) + " MB. "
                     "Consider calling setMaxContextLen() before load() to reduce it.";
    }

    try {
        if (usePagedKv_) {
            int maxPages = (maxSeq + PAGE_SIZE - 1) / PAGE_SIZE;
            pagedCaches_.resize(static_cast<std::size_t>(n));
            for (auto& pc : pagedCaches_) pc.allocate(maxPages, nk, hd);
        } else {
            denseCaches_.resize(static_cast<std::size_t>(n));
            for (auto& dc : denseCaches_) dc.allocate(maxSeq, nk, hd);
        }
    } catch (const std::bad_alloc&) {
        denseCaches_.clear();
        pagedCaches_.clear();
        // Retry with a smaller context if possible.
        int fallback = std::max(512, maxSeq / 4);
        lastError_ = "KV cache allocation failed for context=" +
                     std::to_string(maxSeq) +
                     " tokens. Retrying with " + std::to_string(fallback) + ".";
        if (usePagedKv_) {
            int maxPages = (fallback + PAGE_SIZE - 1) / PAGE_SIZE;
            pagedCaches_.resize(static_cast<std::size_t>(n));
            for (auto& pc : pagedCaches_) pc.allocate(maxPages, nk, hd);
        } else {
            denseCaches_.resize(static_cast<std::size_t>(n));
            for (auto& dc : denseCaches_) dc.allocate(fallback, nk, hd);
        }
    }
}

// --- FlashAttention initialisation ---

void Qwen3Pipeline::initFlashAttn() {
    flashAttn_.init(cfg_);  // may fail silently Ã¢â€ â€™ uses CPU path
}

// --- MoE layer initialisation ---

void Qwen3Pipeline::initMoeLayers() {
    if (!cfg_.isMoE()) return;
    moeLayers_.resize(static_cast<std::size_t>(cfg_.numLayers));
    MoeConfig moeCfg = MoeConfig::fromQwen3Config(cfg_);
    for (auto& ml : moeLayers_)
        ml = std::make_unique<Qwen3MoeLayer>(moeCfg);
}

// --- Direct initialisation (used for testing) ---

void Qwen3Pipeline::loadDirect(const Qwen3Config&                   cfg,
                                 std::vector<PipelineLayerWeights>    layers,
                                 std::vector<uint8_t>                 embedBuf,
                                 GgufDtype                            embedDtype,
                                 std::vector<uint8_t>                 lmHeadBuf,
                                 GgufDtype                            lmHeadDtype,
                                 std::vector<float>                   outputNorm,
                                 Qwen3Tokenizer                       tok) {
    cfg_           = cfg;
    layers_        = std::move(layers);
    embedBuf_      = std::move(embedBuf);
    embedDtype_    = embedDtype;
    lmHeadBuf_     = std::move(lmHeadBuf);
    lmHeadDtype_   = lmHeadDtype;
    outputNorm_    = std::move(outputNorm);
    tok_           = std::move(tok);

    embedTensorInfo_           = TensorInfo{};
    embedTensorInfo_.nDims     = 2;
    embedTensorInfo_.dims[0]   = cfg.hiddenSize;
    embedTensorInfo_.dims[1]   = cfg.vocabSize;
    embedTensorInfo_.dtype     = embedDtype;
    embedTensorInfo_.nElements = static_cast<uint64_t>(cfg.hiddenSize) * cfg.vocabSize;
    lmHeadTensorInfo_          = embedTensorInfo_;

    // Allocate scratch buffers
    x_.resize(cfg_.hiddenSize);
    h_.resize(cfg_.hiddenSize);
    q_.resize(cfg_.numHeads   * cfg_.headDim);
    k_.resize(cfg_.numKvHeads * cfg_.headDim);
    v_.resize(cfg_.numKvHeads * cfg_.headDim);
    attnOut_.resize(cfg_.numHeads * cfg_.headDim);
    gate_.resize(cfg_.intermediateSize);
    up_.resize  (cfg_.intermediateSize);
    logits_.resize(cfg_.vocabSize);

    initKvCaches();
    initFlashAttn();
    initMoeLayers();

    loaded_     = true;
    currentPos_ = 0;
}

// --- GGUF loading ---

bool Qwen3Pipeline::load(const std::string& path) {
    Qwen3Weights w;
    if (!w.openGguf(path)) {
        lastError_ = w.lastError();
        return false;
    }

    cfg_ = w.extractConfig();
    if (traceEnabled()) {
        tracef("load: path=\"%s\"", path.c_str());
        tracef("load: config layers=%u hidden=%u vocab=%u MoE=%d experts=%u per_tok=%u",
               cfg_.numLayers, cfg_.hiddenSize, cfg_.vocabSize,
               cfg_.isMoE() ? 1 : 0, cfg_.numExperts, cfg_.numExpertsPerTok);
    }

    // Allocate scratch buffers
    x_.resize(cfg_.hiddenSize);
    h_.resize(cfg_.hiddenSize);
    q_.resize(cfg_.numHeads   * cfg_.headDim);
    k_.resize(cfg_.numKvHeads * cfg_.headDim);
    v_.resize(cfg_.numKvHeads * cfg_.headDim);
    attnOut_.resize(cfg_.numHeads * cfg_.headDim);
    gate_.resize(cfg_.intermediateSize);
    up_.resize  (cfg_.intermediateSize);
    logits_.resize(cfg_.vocabSize);

    // Extract global weights (read tensor shapes for vocab rows — metadata can overshoot tokenizer).
    uint32_t embedVocabRows = 0;
    uint32_t lmVocabRows    = 0;
    {
        auto embTv = w.load("token_embd.weight");
        if (!embTv) {
            lastError_ = "Missing token_embd.weight";
            return false;
        }
        embedTensorInfo_ = embTv->info();
        embedVocabRows   = inferVocabRowsFrom2D(*embTv, cfg_.hiddenSize);
        embedBuf_        = embTv->copyRawBytes();
        embedDtype_      = embTv->dtype();
    }
    lmVocabRows = embedVocabRows;
    {
        auto outTv = w.load("output.weight");
        if (outTv) {
            lmHeadTensorInfo_ = outTv->info();
            lmVocabRows       = inferVocabRowsFrom2D(*outTv, cfg_.hiddenSize);
            lmHeadBuf_        = outTv->copyRawBytes();
            lmHeadDtype_      = outTv->dtype();
        } else {
            // Some models tie lm_head to embeddings
            lmHeadTensorInfo_ = embedTensorInfo_;
            lmHeadBuf_        = embedBuf_;
            lmHeadDtype_      = embedDtype_;
        }
    }
    {
        auto tv = w.load("output_norm.weight");
        if (tv) {
            auto f = tv->dataF32().value_or(std::vector<float>{});
            outputNorm_ = f;
        } else {
            outputNorm_.assign(cfg_.hiddenSize, 1.0f);
        }
    }

    // Extract per-layer weights
    layers_.resize(static_cast<std::size_t>(cfg_.numLayers));
    for (uint32_t l = 0; l < cfg_.numLayers; ++l) {
        auto& lw = layers_[l];
        std::string pfx = "blk." + std::to_string(l) + ".";

        auto extractNorm = [&](const std::string& sfx) -> std::vector<float> {
            auto tv = w.load(pfx + sfx);
            if (!tv) return std::vector<float>(cfg_.hiddenSize, 1.0f);
            return tv->dataF32().value_or(std::vector<float>(cfg_.hiddenSize, 1.0f));
        };
        // Norms Ã¢â‚¬â€ store as F32 directly
        std::vector<float> attnNormF = extractNorm("attn_norm.weight");
        std::vector<float> ffnNormF  = extractNorm("ffn_norm.weight");

        // Store norm float data in layerStorage_ and set pointer
        layerStorage_.emplace_back(attnNormF.size() * sizeof(float));
        std::memcpy(layerStorage_.back().data(), attnNormF.data(),
                    layerStorage_.back().size());
        lw.attnNorm = reinterpret_cast<const float*>(layerStorage_.back().data());

        layerStorage_.emplace_back(ffnNormF.size() * sizeof(float));
        std::memcpy(layerStorage_.back().data(), ffnNormF.data(),
                    layerStorage_.back().size());
        lw.ffnNorm = reinterpret_cast<const float*>(layerStorage_.back().data());

        auto extractW = [&](const std::string& sfx,
                             const uint8_t*& ptr, GgufDtype& dt) {
            auto tv = w.load(pfx + sfx);
            if (!tv) { ptr = nullptr; dt = GgufDtype::F32; return; }
            layerStorage_.emplace_back(tv->copyRawBytes());
            ptr = layerStorage_.back().data();
            dt  = tv->dtype();
        };

        extractW("attn_q.weight",  lw.wq, lw.wqDtype);
        extractW("attn_k.weight",  lw.wk, lw.wkDtype);
        extractW("attn_v.weight",  lw.wv, lw.wvDtype);
        extractW("attn_output.weight", lw.wo, lw.woDtype);

        if (cfg_.isMoE()) {
            lw.isMoE = true;
            extractW("ffn_gate_inp.weight", lw.moeGate, lw.moeGateDtype);

            int E = static_cast<int>(cfg_.numExperts);
            lw.experts.resize(static_cast<std::size_t>(E));
            for (int e = 0; e < E; ++e) {
                std::string ep = pfx + "ffn_gate_exps." + std::to_string(e) + ".";
                extractW(ep + "weight", lw.experts[e].gateW, lw.experts[e].dtype);
                ep = pfx + "ffn_up_exps."   + std::to_string(e) + ".";
                extractW(ep + "weight",   lw.experts[e].upW,  lw.experts[e].dtype);
                ep = pfx + "ffn_down_exps." + std::to_string(e) + ".";
                extractW(ep + "weight", lw.experts[e].downW, lw.experts[e].dtype);
            }

            auto moeExpertsComplete = [&]() -> bool {
                for (int e = 0; e < E; ++e) {
                    const auto& ew = lw.experts[static_cast<std::size_t>(e)];
                    if (!ew.gateW || !ew.upW || !ew.downW) return false;
                }
                return true;
            };

            // llama.cpp-style GGUF: stacked 3-D tensors, expert axis outermost (dims[0]=inner).
            // (1) Separate gate/up/down: gate/up [hidden, interm, E], down [interm, hidden, E].
            // (2) Qwen3 Next / Qwen3.5 MoE: fused gate+up [hidden, 2*interm, E] + down as above.
            if (!moeExpertsComplete()) {
                for (int e = 0; e < E; ++e)
                    lw.experts[static_cast<std::size_t>(e)] = ExpertWeights{};

                const uint64_t H = cfg_.hiddenSize;
                const uint64_t I = cfg_.intermediateSize;

                auto loadStacked = [&](const char* stem, uint64_t d0, uint64_t d1,
                                       uint64_t gemvRows, uint64_t gemvCols)
                    -> std::pair<const uint8_t*, GgufDtype> {
                    auto tv = w.load(pfx + std::string(stem) + ".weight");
                    if (!tv || tv->nDims() != 3)
                        return {nullptr, GgufDtype::F32};
                    if (tv->dim(0) != d0 || tv->dim(1) != d1 ||
                        tv->dim(2) != static_cast<uint64_t>(E))
                        return {nullptr, GgufDtype::F32};
                    const std::size_t total = tv->rawBytes();
                    const std::size_t per   = total / static_cast<std::size_t>(E);
                    if (per * static_cast<std::size_t>(E) != total) return {nullptr, GgufDtype::F32};
                    if (ggufQuantMatrixBytes(tv->dtype(), gemvRows, gemvCols) != per)
                        return {nullptr, GgufDtype::F32};
                    layerStorage_.emplace_back(tv->copyRawBytes());
                    return {layerStorage_.back().data(), tv->dtype()};
                };

                bool stackedOk = false;
                {
                    auto gateSt = loadStacked("ffn_gate_exps", H, I, I, H);
                    auto upSt   = loadStacked("ffn_up_exps", H, I, I, H);
                    auto downSt = loadStacked("ffn_down_exps", I, H, H, I);
                    const uint8_t* gPtr = gateSt.first;
                    const uint8_t* uPtr = upSt.first;
                    const uint8_t* dPtr = downSt.first;
                    GgufDtype        gDt  = gateSt.second;
                    GgufDtype        uDt  = upSt.second;
                    GgufDtype        dDt  = downSt.second;
                    if (gPtr && uPtr && dPtr && gDt == uDt && gDt == dDt) {
                        const std::size_t perExpert = ggufQuantMatrixBytes(gDt, I, H);
                        for (int e = 0; e < E; ++e) {
                            const std::size_t off = static_cast<std::size_t>(e) * perExpert;
                            lw.experts[static_cast<std::size_t>(e)].gateW = gPtr + off;
                            lw.experts[static_cast<std::size_t>(e)].upW   = uPtr + off;
                            lw.experts[static_cast<std::size_t>(e)].downW = dPtr + off;
                            lw.experts[static_cast<std::size_t>(e)].dtype = gDt;
                        }
                        stackedOk = moeExpertsComplete();
                    }
                }

                if (!stackedOk) {
                    for (int e = 0; e < E; ++e)
                        lw.experts[static_cast<std::size_t>(e)] = ExpertWeights{};

                    auto guTv = w.load(pfx + "ffn_gate_up_exps.weight");
                    auto dnTv = w.load(pfx + "ffn_down_exps.weight");
                    if (guTv && dnTv && guTv->nDims() == 3 && dnTv->nDims() == 3 &&
                        guTv->dtype() == dnTv->dtype()) {
                        const GgufDtype dt = guTv->dtype();
                        if (guTv->dim(0) == H && guTv->dim(1) == 2 * I &&
                            guTv->dim(2) == static_cast<uint64_t>(E) &&
                            dnTv->dim(0) == I && dnTv->dim(1) == H &&
                            dnTv->dim(2) == static_cast<uint64_t>(E)) {
                            const std::size_t perGU =
                                ggufQuantMatrixBytes(dt, 2 * I, H);
                            const std::size_t perDN =
                                ggufQuantMatrixBytes(dt, H, I);
                            const std::size_t gateUpSplit =
                                ggufQuantMatrixBytes(dt, I, H);
                            const std::size_t guTotal = guTv->rawBytes();
                            const std::size_t dnTotal = dnTv->rawBytes();
                            if (guTotal == perGU * static_cast<std::size_t>(E) &&
                                dnTotal == perDN * static_cast<std::size_t>(E) &&
                                2 * gateUpSplit == perGU) {
                                layerStorage_.emplace_back(guTv->copyRawBytes());
                                const uint8_t* guBase = layerStorage_.back().data();
                                layerStorage_.emplace_back(dnTv->copyRawBytes());
                                const uint8_t* dnBase = layerStorage_.back().data();
                                for (int e = 0; e < E; ++e) {
                                    const std::size_t guOff =
                                        static_cast<std::size_t>(e) * perGU;
                                    const std::size_t dnOff =
                                        static_cast<std::size_t>(e) * perDN;
                                    auto& ew = lw.experts[static_cast<std::size_t>(e)];
                                    ew.gateW = guBase + guOff;
                                    ew.upW   = guBase + guOff + gateUpSplit;
                                    ew.downW = dnBase + dnOff;
                                    ew.dtype = dt;
                                }
                                stackedOk = moeExpertsComplete();
                            }
                        }
                    }
                }

                if (!stackedOk) {
                    lastError_ =
                        "MoE expert weights missing or invalid for layer " + std::to_string(l) +
                        ": need per-expert blk.N.ffn_{gate,up,down}_exps.{0.." +
                        std::to_string(E - 1) + "}.weight, or stacked "
                        "ffn_gate_exps / ffn_up_exps / ffn_down_exps "
                        "([hidden,interm,E] + [interm,hidden,E]), or fused "
                        "ffn_gate_up_exps ([hidden,2*interm,E]) + ffn_down_exps; "
                        "dims[0] innermost.";
                    unloadModel();
                    return false;
                }
            }

            if (!lw.moeGate) {
                lastError_ = "MoE router weight missing: expected " + pfx + "ffn_gate_inp.weight";
                unloadModel();
                return false;
            }
        } else {
            extractW("ffn_gate.weight", lw.wGate, lw.wGateDtype);
            extractW("ffn_up.weight",   lw.wUp,   lw.wUpDtype);
            extractW("ffn_down.weight", lw.wDown, lw.wDownDtype);
        }
    }

    // Tokenizer from GGUF metadata (required; do not use loadFromFile on .gguf).
    if (!tok_.loadFromGguf(w)) {
        lastError_ = "Failed to load tokenizer from GGUF: " + tok_.lastError();
        unloadModel();
        return false;
    }

    // Align cfg_.vocabSize with embedding rows, lm_head rows, and tokenizer entries.
    // If llm.vocab_size is larger than tokenizer.ggml.tokens, sampling can pick ids that
    // decode to U+FFFD (often shown as '?' in the UI).
    {
        const uint32_t tokVocab = static_cast<uint32_t>(tok_.vocabSize());
        const uint32_t metaV    = cfg_.vocabSize;
        uint32_t        V       = metaV;
        if (embedVocabRows > 0) V = std::min(V, embedVocabRows);
        if (lmVocabRows > 0) V = std::min(V, lmVocabRows);
        if (tokVocab > 0) V = std::min(V, tokVocab);
        if (V == 0) {
            lastError_ = "Could not determine a positive vocabulary size from metadata, "
                         "weights, and tokenizer.";
            unloadModel();
            return false;
        }
        if (V != metaV) {
            cfg_.vocabSize = V;
            logits_.resize(cfg_.vocabSize);
            lastError_ =
                "Warning: llm.vocab_size (" + std::to_string(metaV) + ") exceeded the "
                "smaller of tokenizer / weight rows (" + std::to_string(V) +
                "). Using " + std::to_string(V) + " for inference.";
        }
    }

    // Ensure embedding rows decode to hiddenSize — wrong GGUF dim order used to cause OOB reads.
    {
        TensorView ev(embedTensorInfo_, embedBuf_.data(), embedBuf_.size());
        auto r0 = ev.row(0);
        if (r0.size() != static_cast<std::size_t>(cfg_.hiddenSize)) {
            lastError_ =
                "token_embd.weight layout is not supported (first row has " +
                std::to_string(r0.size()) + " elements, expected hiddenSize " +
                std::to_string(cfg_.hiddenSize) +
                "). This GGUF may use a non-standard tensor dimension order.";
            unloadModel();
            return false;
        }
        TensorView lh(lmHeadTensorInfo_, lmHeadBuf_.data(), lmHeadBuf_.size());
        auto lr0 = lh.row(0);
        if (lr0.size() != static_cast<std::size_t>(cfg_.hiddenSize)) {
            lastError_ =
                "output.weight layout is not supported (first row has " +
                std::to_string(lr0.size()) + " elements, expected hiddenSize " +
                std::to_string(cfg_.hiddenSize) + ").";
            unloadModel();
            return false;
        }
    }

    initKvCaches();
    initFlashAttn();
    initMoeLayers();

    loaded_     = true;
    currentPos_ = 0;
    if (traceEnabled())
        tracef("load: finished OK (KV+flash+MoE init)");
    return true;
}

// --- resetKvCache / unloadModel ---

void Qwen3Pipeline::unloadModel() {
    flashAttn_.shutdown();
    moeLayers_.clear();
    layers_.clear();
    layerStorage_.clear();
    embedBuf_.clear();
    embedTensorInfo_ = TensorInfo{};
    lmHeadBuf_.clear();
    lmHeadTensorInfo_ = TensorInfo{};
    outputNorm_.clear();
    denseCaches_.clear();
    pagedCaches_.clear();
    x_.clear(); h_.clear(); q_.clear(); k_.clear();
    v_.clear(); attnOut_.clear(); gate_.clear(); up_.clear(); logits_.clear();
    loaded_     = false;
    currentPos_ = 0;
}

void Qwen3Pipeline::resetKvCache() {
    for (auto& dc : denseCaches_) dc.reset();
    for (auto& pc : pagedCaches_) pc.reset();
    currentPos_ = 0;
}

// --- runLayer ---

void Qwen3Pipeline::runLayer(int l, float* x, int pos) {
    const auto& lw = layers_[static_cast<std::size_t>(l)];
    int D  = static_cast<int>(cfg_.hiddenSize);
    int nh = static_cast<int>(cfg_.numHeads);
    int nk = static_cast<int>(cfg_.numKvHeads);
    int hd = static_cast<int>(cfg_.headDim);
    int di = static_cast<int>(cfg_.intermediateSize);

    if (traceEnabled())
        tracef("runLayer L=%d pos=%d MoE=%d", l, pos, lw.isMoE ? 1 : 0);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Attention block Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬

    // h = rmsnorm(x, attn_norm)
    std::copy(x, x + D, h_.begin());
    ops::rmsnorm(h_.data(), lw.attnNorm, D);

    // Q, K, V projections
    if (lw.wq) ops::gemv(lw.wq, lw.wqDtype, nh * hd, D, h_.data(), q_.data());
    if (lw.wk) ops::gemv(lw.wk, lw.wkDtype, nk * hd, D, h_.data(), k_.data());
    if (lw.wv) ops::gemv(lw.wv, lw.wvDtype, nk * hd, D, h_.data(), v_.data());
    if (traceEnabled()) tracef("runLayer L=%d QKV proj done", l);

    // RoPE
    ops::rope(q_.data(), k_.data(), pos, hd, nh, nk,
              static_cast<float>(cfg_.ropeTheta));

    // Append to KV cache and get pointers to full cache
    const float* kFull = nullptr;
    const float* vFull = nullptr;
    int seqLen = 0;

    auto li = static_cast<std::size_t>(l);
    if (usePagedKv_) {
        pagedCaches_[li].appendKv(k_.data(), v_.data());
        seqLen = pagedCaches_[li].seqLen;
    } else {
        denseCaches_[li].appendKv(k_.data(), v_.data(), pos);
        seqLen = denseCaches_[li].seqLen;
        kFull  = denseCaches_[li].k.data();
        vFull  = denseCaches_[li].v.data();
    }

    // FlashAttention-2
    FlashAttnParams ap;
    ap.numHeads   = nh;
    ap.numKvHeads = nk;
    ap.headDim    = hd;
    ap.seqLen     = seqLen;
    ap.queryPos   = pos;
    ap.causalMask = true;

    if (usePagedKv_) {
        flashAttn_.forwardPaged(ap, q_.data(), pagedCaches_[li], attnOut_.data());
    } else {
        flashAttn_.forward(ap, q_.data(), kFull, vFull, attnOut_.data());
    }
    if (traceEnabled())
        tracef("runLayer L=%d flash_attn done seqLen=%d paged=%d", l, seqLen,
               usePagedKv_ ? 1 : 0);

    // Output projection: x += Wo Ã‚Â· attnOut
    std::vector<float> tmp(static_cast<std::size_t>(D));
    if (lw.wo) ops::gemv(lw.wo, lw.woDtype, D, nh * hd, attnOut_.data(), tmp.data());
    ops::addVec(x, tmp.data(), D);
    if (traceEnabled()) tracef("runLayer L=%d attn Wo done", l);

    // Ã¢â€â‚¬Ã¢â€â‚¬ FFN block Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬

    // h = rmsnorm(x, ffn_norm)
    std::copy(x, x + D, h_.begin());
    ops::rmsnorm(h_.data(), lw.ffnNorm, D);

    if (lw.isMoE && moeLayers_[li]) {
        if (traceEnabled()) tracef("runLayer L=%d MoE forward start", l);
        // MoE dispatch
        std::vector<float> moeOut(static_cast<std::size_t>(D), 0.0f);
        moeLayers_[li]->forward(h_.data(),
                                 lw.moeGate, static_cast<const void*>(lw.moeGate),
                                 lw.moeGateDtype,
                                 lw.experts,
                                 lw.sharedExpert,
                                 moeOut.data());
        ops::addVec(x, moeOut.data(), D);
        if (traceEnabled()) tracef("runLayer L=%d MoE forward end", l);
    } else if (lw.wGate) {
        if (traceEnabled()) tracef("runLayer L=%d dense FFN start", l);
        // Dense SwiGLU FFN
        ops::gemv(lw.wGate, lw.wGateDtype, di, D, h_.data(), gate_.data());
        ops::gemv(lw.wUp,   lw.wUpDtype,   di, D, h_.data(), up_.data());
        ops::siluHadamard(gate_.data(), up_.data(), di);
        ops::gemv(lw.wDown, lw.wDownDtype, D, di, gate_.data(), tmp.data());
        ops::addVec(x, tmp.data(), D);
        if (traceEnabled()) tracef("runLayer L=%d dense FFN end", l);
    }
    if (traceEnabled()) tracef("runLayer L=%d complete", l);
}

// --- forward ---

void Qwen3Pipeline::forward(int32_t tokenId, int pos) {
    assert(loaded_);
    int D = static_cast<int>(cfg_.hiddenSize);
    int V = static_cast<int>(cfg_.vocabSize);

    if (tokenId < 0 || tokenId >= V) {
        throw std::runtime_error(
            "Qwen3Pipeline::forward: token id " + std::to_string(tokenId) +
            " out of range [0, " + std::to_string(V) + ")");
    }

    const int cap = kvSeqCap(cfg_, maxContextLen_);
    if (pos < 0 || pos >= cap) {
        throw std::runtime_error(
            "Qwen3Pipeline::forward: position " + std::to_string(pos) +
            " out of KV capacity [0, " + std::to_string(cap) + ")");
    }

    if (traceEnabled())
        tracef("forward: tokenId=%d pos=%d", static_cast<int>(tokenId), pos);

    // Use on-disk TensorInfo for strides (do not synthesize shape — wrong dims → memcpy AV).
    if (embedDtype_ == GgufDtype::F32 && embedTensorInfo_.nDims >= 2 &&
        embedTensorInfo_.dims[0] == static_cast<uint64_t>(D)) {
        const float* row = reinterpret_cast<const float*>(embedBuf_.data())
                         + static_cast<std::ptrdiff_t>(tokenId) * D;
        ops::copyVec(x_.data(), row, D);
    } else {
        TensorView tv(embedTensorInfo_, embedBuf_.data(), embedBuf_.size());
        auto rowF = tv.row(static_cast<int64_t>(tokenId));
        if (rowF.size() != static_cast<std::size_t>(D)) {
            throw std::runtime_error(
                "Qwen3Pipeline::forward: embedding dequantize size mismatch (expected " +
                std::to_string(D) + ", got " + std::to_string(rowF.size()) + ")");
        }
        ops::copyVec(x_.data(), rowF.data(), D);
    }
    if (traceEnabled()) tracef("forward: embed done D=%d", D);

    // Run transformer layers
    for (int l = 0; l < static_cast<int>(cfg_.numLayers); ++l)
        runLayer(l, x_.data(), pos);
    if (traceEnabled()) tracef("forward: all layers done");

    // Output RMSNorm + LM head
    ops::rmsnorm(x_.data(), outputNorm_.data(), D);
    if (lmHeadDtype_ == GgufDtype::F32) {
        ops::gemvF32(reinterpret_cast<const float*>(lmHeadBuf_.data()),
                     V, D, x_.data(), logits_.data());
    } else {
        ops::gemv(lmHeadBuf_.data(), lmHeadDtype_, V, D, x_.data(), logits_.data());
    }
    if (traceEnabled()) tracef("forward: lm_head done V=%d", V);

    currentPos_ = pos + 1;
}

// --- prefill ---

void Qwen3Pipeline::prefill(const std::vector<int32_t>& tokenIds, int startPos) {
    if (traceEnabled())
        tracef("prefill: %zu tokens startPos=%d", tokenIds.size(), startPos);
    int pos = startPos;
    for (int32_t tok : tokenIds) {
        forward(tok, pos++);
    }
    if (traceEnabled()) tracef("prefill: done lastPos=%d", pos - 1);
}

// --- sampleNext ---

int32_t Qwen3Pipeline::sampleNext(const SamplerConfig& cfg,
                                    const std::vector<int32_t>& history) {
    if (traceEnabled())
        tracef("sampleNext: history_len=%zu vocab=%u",
               history.size(), cfg_.vocabSize);
    Qwen3Sampler sampler(cfg);
    std::vector<TokenId> histU(history.begin(), history.end());
    const int32_t out = static_cast<int32_t>(sampler.sample(logits_.data(),
                                                static_cast<int>(cfg_.vocabSize),
                                                histU));
    if (traceEnabled()) tracef("sampleNext: picked token %d", static_cast<int>(out));
    return out;
}

// --- generate ---

PipelineResult Qwen3Pipeline::generateFromIds(
    const std::vector<int32_t>& promptIds,
    const PipelineGenerateOptions& opts)
{
    assert(loaded_);

    const int cap = kvSeqCap(cfg_, maxContextLen_);
    if (static_cast<int>(promptIds.size()) >= cap) {
        throw std::runtime_error(
            "Prompt length " + std::to_string(promptIds.size()) +
            " must be below KV context capacity " + std::to_string(cap) +
            " (leave room for generation). Lower Context Length or shorten the prompt.");
    }

    resetKvCache();
    if (traceEnabled())
        tracef("generateFromIds: prompt_tokens=%zu max_new=%d",
               promptIds.size(), opts.maxNewTokens);

    SamplerConfig sampCfg;
    sampCfg.temperature       = opts.temperature;
    sampCfg.topP              = opts.topP;
    sampCfg.topK              = opts.topK;
    sampCfg.repetitionPenalty = opts.repetitionPenalty;
    sampCfg.seed              = opts.seed;

    // Collect stop tokens
    std::vector<int32_t> stopToks = opts.stopTokenIds;
    stopToks.push_back(static_cast<int32_t>(cfg_.eosTokenId));
    stopToks.push_back(static_cast<int32_t>(cfg_.imEndId));

    // Prefill phase
    auto t0 = std::chrono::steady_clock::now();
    prefill(promptIds, 0);
    auto t1 = std::chrono::steady_clock::now();
    if (traceEnabled()) tracef("generateFromIds: prefill phase finished");

    PipelineResult res;
    res.promptTokens = static_cast<int>(promptIds.size());

    std::vector<int32_t> history(promptIds);

    // Decode phase
    auto tDec0 = t1;
    while (res.newTokens < opts.maxNewTokens) {
        int32_t tok = sampleNext(sampCfg, history);
        history.push_back(tok);

        bool isStop = std::find(stopToks.begin(), stopToks.end(), tok) != stopToks.end();

        std::string piece = tok_.decode({static_cast<TokenId>(tok)});
        res.tokenIds.push_back(tok);
        res.text    += piece;
        ++res.newTokens;

        if (opts.tokenCallback) opts.tokenCallback(tok, piece);
        if (opts.streamCallback && !opts.streamCallback(piece)) break;

        if (isStop) { res.hitEos = true; break; }

        if (currentPos_ >= cap) {
            throw std::runtime_error(
                "KV context full (" + std::to_string(cap) +
                " tokens). Lower max new tokens or context length.");
        }

        if (traceEnabled())
            tracef("generateFromIds: decode forward tok=%d pos=%d newTok=%d",
                   static_cast<int>(tok), currentPos_, res.newTokens);
        forward(tok, currentPos_);
    }

    auto tEnd = std::chrono::steady_clock::now();

    using Ms = std::chrono::duration<double, std::milli>;
    res.prefillMs  = Ms(t1  - t0  ).count();
    res.decodeMs   = Ms(tEnd - tDec0).count();
    res.tokPerSec  = (res.decodeMs > 0.0)
                   ? res.newTokens / (res.decodeMs / 1000.0)
                   : 0.0;

    return res;
}

PipelineResult Qwen3Pipeline::generate(const std::string& prompt,
                                         const PipelineGenerateOptions& opts) {
    assert(loaded_);

    auto tagOf = [this](uint32_t cfgId, const char* fallback) -> std::string {
        auto s = tok_.idToToken(static_cast<TokenId>(cfgId));
        return s ? *s : std::string(fallback);
    };

    static constexpr const char* kFbImStart = "<|im_start|>";
    static constexpr const char* kFbImEnd   = "<|im_end|>";
    static constexpr const char* kFbThink   = "<|think|>";

    const std::string imStart = tagOf(cfg_.imStartId, kFbImStart);
    const std::string imEnd   = tagOf(cfg_.imEndId, kFbImEnd);
    const std::string thinkT  = tagOf(cfg_.thinkStartId, kFbThink);

    static constexpr const char* kDefaultSystem =
        "You are a helpful coding assistant integrated with the RetDec decompiler. "
        "Help with reverse engineering, decompiled C, assembly, and malware analysis. "
        "Be concise and accurate.";

    std::string tmpl;
    if (!opts.skipSystemPrompt) {
        const std::string& sys =
            opts.systemPrompt.empty() ? std::string(kDefaultSystem) : opts.systemPrompt;
        tmpl += imStart + "system\n" + sys + imEnd + "\n";
    }
    tmpl += imStart + "user\n" + prompt + imEnd + "\n" + imStart + "assistant\n";
    if (opts.enableThinking)
        tmpl += thinkT + "\n";

    auto ids_u = tok_.encode(tmpl);
    std::vector<int32_t> ids(ids_u.begin(), ids_u.end());
    return generateFromIds(ids, opts);
}

} // namespace retdec::qwen3
