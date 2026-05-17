/**
 * @file src/qwen3/qwen3_moe.cpp
 * @brief Mixture-of-Experts routing and dispatch for Qwen3 MoE variants.
 *
 * Routing
 * ───────
 * The gating GEMV (hidden [D] × gate_weight^T [E×D] → logits [E]) is
 * dispatched through ops::gemv(), which routes to GPU+CPU hybrid if
 * ops::getOpenCL() is non-null.
 *
 * Softmax is applied over all E logits.  Top-K selection uses a partial
 * sort (nth_element), which is O(E) instead of O(E log E).
 *
 * Expert dispatch
 * ───────────────
 * For each selected expert, we evaluate the SwiGLU FFN:
 *   gate = gemv(gateW, hidden)       [moeIntermSize]
 *   up   = gemv(upW,   hidden)       [moeIntermSize]
 *   silu(gate) * up → gate (in-place, via ops::siluHadamard)
 *   down = gemv(downW, gate)          [hiddenSize]
 *   out += weight * down
 *
 * The experts are evaluated serially.  This is acceptable because at
 * inference time K is small (8) and each expert FFN is much smaller than
 * the dense model's FFN.
 *
 * Shared expert
 * ─────────────
 * Added on top of the routed sum if sharedExpertIntermSize > 0.  Uses the
 * same SwiGLU path but with a larger intermediate dim.
 */

#include "retdec/qwen3/qwen3_moe.h"
#include "retdec/qwen3/qwen3_ops.h"
#include "retdec/qwen3/qwen3_trace.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace retdec::qwen3 {

// ─── MoeConfig ────────────────────────────────────────────────────────────────

MoeConfig MoeConfig::fromQwen3Config(const Qwen3Config& cfg) {
    MoeConfig m;
    m.numExperts       = static_cast<int>(cfg.numExperts);
    m.numExpertsPerTok = static_cast<int>(cfg.numExpertsPerTok);
    m.hiddenSize       = static_cast<int>(cfg.hiddenSize);
    // MoE intermediate size: for Qwen3 it is stored in intermediateSize
    // when isMoE() is true, intermediateSize refers to the per-expert size.
    m.moeIntermSize    = static_cast<int>(cfg.intermediateSize);
    // Shared expert: not in the base Qwen3Config — callers set this directly.
    m.sharedExpertIntermSize = 0;
    m.normalizeWeights = true;
    return m;
}

// ─── MoeRouter ────────────────────────────────────────────────────────────────

MoeRouter::MoeRouter(const MoeConfig& cfg) : cfg_(cfg) {
    logits_.resize(static_cast<std::size_t>(cfg.numExperts));
}

MoeRouteResult MoeRouter::route(const float*   hidden,
                                 const uint8_t* gateW,
                                 const void*    gateKey,
                                 GgufDtype      dtype) const {
    const int E = cfg_.numExperts;
    const int D = cfg_.hiddenSize;
    const int K = cfg_.numExpertsPerTok;

    // Gating GEMV: logits[E] = gateW[E×D] · hidden[D]
    ops::gemvKeyed(gateW, gateKey, dtype, E, D, hidden, logits_.data());

    // Softmax over all experts
    ops::softmax(logits_.data(), E);

    // Top-K selection via partial sort on indices
    std::vector<int> idx(static_cast<std::size_t>(E));
    std::iota(idx.begin(), idx.end(), 0);
    std::nth_element(idx.begin(), idx.begin() + K, idx.end(),
                     [&](int a, int b) { return logits_[a] > logits_[b]; });
    idx.resize(static_cast<std::size_t>(K));
    // Sort selected experts by index (stable ordering for reproducibility)
    std::sort(idx.begin(), idx.end());

    MoeRouteResult res;
    res.expertIds.resize(static_cast<std::size_t>(K));
    res.weights.resize(static_cast<std::size_t>(K));

    float wSum = 0.0f;
    for (int i = 0; i < K; ++i) {
        res.expertIds[i] = idx[i];
        res.weights[i]   = logits_[idx[i]];
        wSum            += res.weights[i];
    }

    // Normalise routing weights so they sum to 1 over selected experts
    if (cfg_.normalizeWeights && wSum > 0.0f) {
        float inv = 1.0f / wSum;
        for (auto& w : res.weights) w *= inv;
    }

    return res;
}

MoeRouteResult MoeRouter::route(const float*   hidden,
                                 const uint8_t* gateW,
                                 GgufDtype      dtype) const {
    return route(hidden, gateW, static_cast<const void*>(gateW), dtype);
}

// ─── MoeDispatcher ────────────────────────────────────────────────────────────

MoeDispatcher::MoeDispatcher(const MoeConfig& cfg) : cfg_(cfg) {
    gate_.resize(static_cast<std::size_t>(cfg.moeIntermSize));
    up_.resize  (static_cast<std::size_t>(cfg.moeIntermSize));
    down_.resize(static_cast<std::size_t>(cfg.hiddenSize));
}

// Apply one SwiGLU expert FFN and accumulate into `out` with weight `w`.
// `gate` and `up` must be at least intermSize floats.
// `down` must be at least hiddenSize floats (separate from gate/up to avoid
// the buffer-overrun that occurs when hiddenSize > intermSize).
static void applyExpertFFN(const float*         hidden,
                            const ExpertWeights& ew,
                            int hiddenSize, int intermSize,
                            float weight,
                            float* gate, float* up,
                            float* down,
                            float* out) {
    if (!ew.gateW || !ew.upW || !ew.downW) return;
    // gate_proj
    ops::gemv(ew.gateW, ew.dtype, intermSize, hiddenSize, hidden, gate);
    // up_proj
    ops::gemv(ew.upW,   ew.dtype, intermSize, hiddenSize, hidden, up);
    // SiLU(gate) * up  in-place into gate
    ops::siluHadamard(gate, up, intermSize);
    // down_proj: [hiddenSize × intermSize] × gate → down
    ops::gemv(ew.downW, ew.dtype, hiddenSize, intermSize, gate, down);
    // Weighted accumulate into out
    for (int d = 0; d < hiddenSize; ++d)
        out[d] += weight * down[d];
}

void MoeDispatcher::forward(const float*                      hidden,
                             const MoeRouteResult&             route,
                             const std::vector<ExpertWeights>& experts,
                             float*                            out) const {
    const int K = cfg_.numExpertsPerTok;
    const int D = cfg_.hiddenSize;
    const int I = cfg_.moeIntermSize;

    assert(static_cast<int>(route.expertIds.size()) == K);
    assert(static_cast<int>(experts.size()) >= cfg_.numExperts);

    std::fill(out, out + D, 0.0f);

    for (int i = 0; i < K; ++i) {
        int eid = route.expertIds[i];
        if (eid < 0 || eid >= static_cast<int>(experts.size())) continue;
        float wt = route.weights[i];
        if (traceVerbose())
            tracef("moe dispatch: slot=%d expert=%d weight=%g", i, eid, static_cast<double>(wt));
        applyExpertFFN(hidden, experts[static_cast<std::size_t>(eid)], D, I, wt,
                       gate_.data(), up_.data(), down_.data(), out);
        if (traceVerbose())
            tracef("moe dispatch: slot=%d expert=%d applyExpertFFN done", i, eid);
    }
}

void MoeDispatcher::applySharedExpert(const float*         hidden,
                                       const ExpertWeights& sharedExpert,
                                       float*               out) const {
    const int D = cfg_.hiddenSize;
    const int I = cfg_.sharedExpertIntermSize;
    if (I <= 0) return;

    // Temporary buffers: gate/up sized to intermSize, down sized to hiddenSize
    std::vector<float> gBuf(static_cast<std::size_t>(I));
    std::vector<float> uBuf(static_cast<std::size_t>(I));
    std::vector<float> dBuf(static_cast<std::size_t>(D));

    // Weight = 1.0 (always fully active)
    applyExpertFFN(hidden, sharedExpert, D, I, 1.0f,
                   gBuf.data(), uBuf.data(), dBuf.data(), out);
}

// ─── Qwen3MoeLayer ────────────────────────────────────────────────────────────

Qwen3MoeLayer::Qwen3MoeLayer(const MoeConfig& cfg)
    : router_(cfg), dispatcher_(cfg) {}

void Qwen3MoeLayer::forward(const float*                       hidden,
                              const uint8_t*                     gateW,
                              const void*                        gateKey,
                              GgufDtype                          gateDtype,
                              const std::vector<ExpertWeights>&  experts,
                              const ExpertWeights&               sharedExpert,
                              float*                             out) const {
    if (traceEnabled())
        tracef("moe: route begin (experts=%zu K=%d)", experts.size(),
               router_.config().numExpertsPerTok);
    lastRoute_ = router_.route(hidden, gateW, gateKey, gateDtype);
    if (traceEnabled()) {
        const int K = static_cast<int>(lastRoute_.expertIds.size());
        tracef("moe: route done K=%d first_expert=%d", K, K > 0 ? lastRoute_.expertIds[0] : -1);
    }
    dispatcher_.forward(hidden, lastRoute_, experts, out);

    if (dispatcher_.config().sharedExpertIntermSize > 0) {
        if (traceEnabled()) tracef("moe: shared_expert apply");
        dispatcher_.applySharedExpert(hidden, sharedExpert, out);
    }
    if (traceEnabled()) tracef("moe: forward return");
}

void Qwen3MoeLayer::forward(const float*                       hidden,
                              const uint8_t*                     gateW,
                              GgufDtype                          gateDtype,
                              const std::vector<ExpertWeights>&  experts,
                              float*                             out) const {
    static const ExpertWeights noShared{};
    forward(hidden, gateW, static_cast<const void*>(gateW), gateDtype,
            experts, noShared, out);
}

// ─── MoeLoadMonitor ───────────────────────────────────────────────────────────

MoeLoadMonitor::MoeLoadMonitor(int numExperts)
    : counts_(static_cast<std::size_t>(numExperts), 0) {}

void MoeLoadMonitor::record(const MoeRouteResult& route) {
    for (int id : route.expertIds) {
        if (id >= 0 && id < static_cast<int>(counts_.size()))
            ++counts_[id];
    }
    ++totalTokens_;
}

void MoeLoadMonitor::reset() {
    std::fill(counts_.begin(), counts_.end(), 0);
    totalTokens_ = 0;
}

std::vector<float> MoeLoadMonitor::fractions() const {
    std::vector<float> f(counts_.size(), 0.0f);
    if (totalTokens_ == 0) return f;
    float inv = 1.0f / static_cast<float>(totalTokens_);
    for (std::size_t i = 0; i < counts_.size(); ++i)
        f[i] = static_cast<float>(counts_[i]) * inv;
    return f;
}

int MoeLoadMonitor::hotExpert() const {
    return static_cast<int>(
        std::max_element(counts_.begin(), counts_.end()) - counts_.begin());
}

int MoeLoadMonitor::coldExpert() const {
    return static_cast<int>(
        std::min_element(counts_.begin(), counts_.end()) - counts_.begin());
}

} // namespace retdec::qwen3
