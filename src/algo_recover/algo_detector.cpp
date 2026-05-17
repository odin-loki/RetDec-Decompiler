/**
 * @file src/algo_recover/algo_detector.cpp
 * @brief AlgorithmDetector orchestrator + AlgorithmResult utilities.
 *
 * ## Orchestration
 *
 * `AlgorithmDetector::detect` runs all registered per-algorithm detectors in
 * a fixed priority order and returns the result with the highest confidence.
 *
 * Priority order (most discriminating detectors first):
 *   1. FindDetector      — early-exit branch is highly specific.
 *   2. AccumulateDetector— phi + no-store combination is distinctive.
 *   3. TransformDetector — dual-pointer advance is distinct from for_each.
 *   4. PartitionDetector — converging pointers + swap.
 *   5. ForEachDetector   — most general; last resort.
 *
 * After selecting the best detector, `IteratorPatternRecovery` is run to
 * augment the emitted form with range-based for syntax where applicable.
 *
 * ## Emission tier assignment
 *
 * Confidence thresholds (configurable):
 *   High   ≥ 0.75 → full `std::` call
 *   Medium ≥ 0.45 → comment-annotated loop
 *   Low    <  0.45 → plain loop, no annotation
 *
 * ## Preflight filter
 *
 * Functions with fewer than `minBlocks` basic blocks or fewer than `minInstrs`
 * total instructions are skipped — they are too small to contain a meaningful
 * loop pattern.
 */

#include <memory>
#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

#include <sstream>

namespace retdec {
namespace algo_recover {

// ─── AlgorithmResult utilities ────────────────────────────────────────────────

std::string AlgorithmResult::kindName() const noexcept {
    switch (kind) {
    case AlgorithmKind::Transform:   return "std::transform";
    case AlgorithmKind::Accumulate:  return "std::accumulate";
    case AlgorithmKind::MaxElement:  return "std::max_element";
    case AlgorithmKind::MinElement:  return "std::min_element";
    case AlgorithmKind::Find:        return "std::find";
    case AlgorithmKind::FindIf:      return "std::find_if";
    case AlgorithmKind::Partition:   return "std::partition";
    case AlgorithmKind::ForEach:     return "std::for_each";
    case AlgorithmKind::Copy:        return "std::copy";
    case AlgorithmKind::Fill:        return "std::fill";
    case AlgorithmKind::Count:       return "std::count";
    case AlgorithmKind::AnyOf:       return "std::any_of";
    case AlgorithmKind::AllOf:       return "std::all_of";
    case AlgorithmKind::NoneOf:      return "std::none_of";
    case AlgorithmKind::Reverse:     return "std::reverse";
    case AlgorithmKind::RotateLeft:  return "std::rotate";
    default:                         return "unknown";
    }
}

std::string AlgorithmResult::toString() const {
    std::ostringstream os;
    os << kindName() << " (confidence=" << confidence << ", tier=";
    switch (tier) {
    case EmissionTier::High:   os << "high";   break;
    case EmissionTier::Medium: os << "medium"; break;
    default:                   os << "low";    break;
    }
    os << ")";
    if (!emittedForm.empty()) os << " => " << emittedForm;
    return os.str();
}

// ─── AlgorithmDetector ────────────────────────────────────────────────────────

AlgorithmDetector::AlgorithmDetector(Config cfg) : cfg_(std::move(cfg)) {
    // Register detectors in priority order.
    if (cfg_.runFind)       detectors_.push_back(std::make_unique<FindDetector>());
    if (cfg_.runAccumulate) detectors_.push_back(std::make_unique<AccumulateDetector>());
    if (cfg_.runTransform)  detectors_.push_back(std::make_unique<TransformDetector>());
    if (cfg_.runPartition)  detectors_.push_back(std::make_unique<PartitionDetector>());
    if (cfg_.runForEach)    detectors_.push_back(std::make_unique<ForEachDetector>());
}

bool AlgorithmDetector::passesPreflight(const ssa::SSAFunction& fn) const {
    if ((int)fn.blockCount() < cfg_.minBlocks) return false;
    int instrs = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk) instrs += static_cast<int>(blk->instrs.size());
    }
    return instrs >= cfg_.minInstrs;
}

EmissionTier AlgorithmDetector::assignTier(float confidence) const {
    if (confidence >= cfg_.highTierThreshold)   return EmissionTier::High;
    if (confidence >= cfg_.mediumTierThreshold) return EmissionTier::Medium;
    return EmissionTier::Low;
}

AlgorithmResult AlgorithmDetector::detect(const ssa::SSAFunction& fn) const {
    ++stats_.functionsAnalysed;

    if (!passesPreflight(fn)) {
        ++stats_.functionsSkipped;
        return {};
    }

    AlgorithmResult best;
    best.confidence = 0.0f;

    for (const auto& det : detectors_) {
        auto r = det->detect(fn);
        if (r.confidence > best.confidence) best = r;
    }

    if (best.kind == AlgorithmKind::Unknown || best.confidence < 0.01f)
        return best;

    // Re-apply tier with our config thresholds.
    best.tier = assignTier(best.confidence);

    // Augment emitted form with iterator pattern recovery.
    auto iterResult = iterRecover_.recover(fn);
    if (iterResult.isBeginEnd && best.tier == EmissionTier::High) {
        // Prefer range-for form in the emitted annotation comment.
        if (!iterResult.rangeForForm.empty())
            best.emittedForm += " // range: " + iterResult.rangeForForm;
    }
    if (iterResult.hasBackInserter)
        best.hasBackInserter = true;
    if (iterResult.isReverseIter)
        best.isReverse = true;

    ++stats_.detections;
    switch (best.tier) {
    case EmissionTier::High:   ++stats_.highTier;   break;
    case EmissionTier::Medium: ++stats_.mediumTier; break;
    default:                   ++stats_.lowTier;    break;
    }

    return best;
}

AlgorithmDetector::DetectionMap AlgorithmDetector::detectModule(
        const std::vector<const ssa::SSAFunction*>& fns) const {
    DetectionMap results;
    for (const auto* fn : fns) {
        if (!fn) continue;
        auto r = detect(*fn);
        if (r.kind != AlgorithmKind::Unknown)
            results.emplace_back(fn->name(), std::move(r));
    }
    return results;
}

} // namespace algo_recover
} // namespace retdec
