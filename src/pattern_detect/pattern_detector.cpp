/**
 * @file src/pattern_detect/pattern_detector.cpp
 * @brief PatternDetector orchestrator + PatternResult utilities.
 *
 * ## Orchestration
 *
 * `PatternDetector::detectFunction` runs all registered detectors on a single
 * function and returns every pattern whose confidence meets the threshold.
 *
 * `PatternDetector::detectGroup` passes the group of functions (a class's
 * methods) to each detector's `detectGroup` override, enabling cross-function
 * analysis (Observer, Strategy, RAII).
 *
 * Multiple patterns can be detected in the same function/group — e.g., a
 * Singleton that also uses RAII internally.
 *
 * ## Registration order
 *
 * Detectors are registered from most-specific to least-specific to reduce
 * false positives when patterns share signals:
 *
 *   1. SingletonDetector  — static pointer + null-check is highly specific
 *   2. RAIIDetector       — acquire/release pair is distinctive
 *   3. FactoryDetector    — ≥2 allocation sites + switch
 *   4. CommandDetector    — vtable execute + container loop
 *   5. ObserverDetector   — push_back + indirect call in loop
 *   6. StrategyDetector   — Load chain + indirect call
 *   7. StateMachineDetector — compare chain on state var
 */

#include <memory>
#include "retdec/pattern_detect/pattern_detect.h"
#include "retdec/ssa/ssa.h"

#include <sstream>

namespace retdec {
namespace pattern_detect {

// ─── PatternResult utilities ──────────────────────────────────────────────────

std::string PatternResult::kindName() const noexcept {
    switch (kind) {
    case PatternKind::Singleton:    return "Singleton";
    case PatternKind::Factory:      return "Factory";
    case PatternKind::Observer:     return "Observer";
    case PatternKind::Command:      return "Command";
    case PatternKind::Strategy:     return "Strategy";
    case PatternKind::StateMachine: return "StateMachine";
    case PatternKind::RAII:         return "RAII";
    default:                        return "Unknown";
    }
}

std::string PatternResult::toString() const {
    std::ostringstream os;
    os << kindName() << " (confidence=" << confidence << ")";
    if (hasVariant && !variantName.empty())
        os << " [" << variantName << "]";
    if (!comment.empty())
        os << " " << comment;
    return os.str();
}

// ─── PatternDetector ─────────────────────────────────────────────────────────

PatternDetector::PatternDetector(Config cfg) : cfg_(std::move(cfg)) {
    detectors_.push_back(std::make_unique<SingletonDetector>());
    detectors_.push_back(std::make_unique<RAIIDetector>());
    detectors_.push_back(std::make_unique<FactoryDetector>());
    detectors_.push_back(std::make_unique<CommandDetector>());
    detectors_.push_back(std::make_unique<ObserverDetector>());
    detectors_.push_back(std::make_unique<StrategyDetector>());
    detectors_.push_back(std::make_unique<StateMachineDetector>());
}

bool PatternDetector::passesPreflight(const ssa::SSAFunction& fn) const {
    if ((int)fn.blockCount() < cfg_.minBlocks) return false;
    int instrs = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk) instrs += static_cast<int>(blk->instrs.size());
    }
    return instrs >= cfg_.minInstrs;
}

PatternDetector::ResultList PatternDetector::detectFunction(
        const ssa::SSAFunction& fn) const {
    ++stats_.functionsAnalysed;
    ResultList results;

    if (!passesPreflight(fn)) return results;

    for (const auto& det : detectors_) {
        auto r = det->detect(fn);
        if (r.kind != PatternKind::Unknown && r.confidence >= cfg_.minConfidence) {
            ++stats_.detections;
            ++stats_.byKind[r.kind];
            results.push_back(std::move(r));
        }
    }
    return results;
}

PatternDetector::ResultList PatternDetector::detectGroup(
        const std::vector<const ssa::SSAFunction*>& fns) const {
    ++stats_.groupsAnalysed;
    ResultList results;

    for (const auto& det : detectors_) {
        auto r = det->detectGroup(fns);
        if (r.kind != PatternKind::Unknown && r.confidence >= cfg_.minConfidence) {
            ++stats_.detections;
            ++stats_.byKind[r.kind];
            results.push_back(std::move(r));
        }
    }

    // Also run single-function analysis on each member for intra patterns.
    for (const auto* fn : fns) {
        if (!fn || !passesPreflight(*fn)) continue;
        ++stats_.functionsAnalysed;
        auto intra = detectFunction(*fn);
        for (auto& r : intra) {
            // Avoid duplicating patterns already found at group level.
            bool alreadyFound = false;
            for (const auto& existing : results)
                if (existing.kind == r.kind) { alreadyFound = true; break; }
            if (!alreadyFound)
                results.push_back(std::move(r));
        }
    }
    return results;
}

} // namespace pattern_detect
} // namespace retdec
