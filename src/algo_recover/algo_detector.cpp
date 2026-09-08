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

#include <algorithm>
#include <memory>
#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

#include <numeric>
#include <sstream>

namespace retdec {
namespace algo_recover {

// ─── AlgorithmResult utilities ────────────────────────────────────────────────

namespace {

/// One table, read in both directions. The function-analysis cache used to
/// hold its own list of five bare names ("Transform", "Find", ...) while
/// kindName() wrote "std::transform", "std::find" -- so no kind ever matched
/// on read-back, every cached algorithm came out Unknown, and a warm decompile
/// printed "unknown detected" where a cold one printed "std::find_if
/// detected". Two lists that had to agree, and nothing making them.
struct KindName { AlgorithmKind kind; const char* name; };

constexpr KindName kKindNames[] = {
	{AlgorithmKind::Transform,    "std::transform"},
	{AlgorithmKind::Accumulate,   "std::accumulate"},
	{AlgorithmKind::MaxElement,   "std::max_element"},
	{AlgorithmKind::MinElement,   "std::min_element"},
	{AlgorithmKind::Find,         "std::find"},
	{AlgorithmKind::FindIf,       "std::find_if"},
	{AlgorithmKind::BinarySearch, "binary_search"},
	{AlgorithmKind::Partition,    "std::partition"},
	{AlgorithmKind::ForEach,      "std::for_each"},
	{AlgorithmKind::Copy,         "std::copy"},
	{AlgorithmKind::Fill,         "std::fill"},
	{AlgorithmKind::Count,        "std::count"},
	{AlgorithmKind::AnyOf,        "std::any_of"},
	{AlgorithmKind::AllOf,        "std::all_of"},
	{AlgorithmKind::NoneOf,       "std::none_of"},
	{AlgorithmKind::Reverse,      "std::reverse"},
	{AlgorithmKind::RotateLeft,   "std::rotate"},
};

} // anonymous namespace

std::string AlgorithmResult::kindName() const noexcept
{
	for (const auto& kn : kKindNames)
	{
		if (kn.kind == kind) return kn.name;
	}
	return "unknown";
}

AlgorithmKind algorithmKindFromName(const std::string& name) noexcept
{
	for (const auto& kn : kKindNames)
	{
		if (name == kn.name) return kn.kind;
	}
	return AlgorithmKind::Unknown;
}

std::string AlgorithmResult::toString() const
{
	std::ostringstream os;
	os << kindName() << " (confidence=" << confidence << ", tier=";
	switch (tier)
	{
	case EmissionTier::High: os << "high"; break;
	case EmissionTier::Medium: os << "medium"; break;
	default: os << "low"; break;
	}
	os << ")";
	if (!emittedForm.empty()) os << " => " << emittedForm;
	return os.str();
}

// ─── AlgorithmDetector ────────────────────────────────────────────────────────

AlgorithmDetector::AlgorithmDetector(Config cfg): cfg_(std::move(cfg))
{
	// Register detectors in priority order.
	if (cfg_.runBinarySearch) detectors_.push_back(std::make_unique<BinarySearchDetector>());
	if (cfg_.runFind) detectors_.push_back(std::make_unique<FindDetector>());
	if (cfg_.runAccumulate) detectors_.push_back(std::make_unique<AccumulateDetector>());
	if (cfg_.runTransform) detectors_.push_back(std::make_unique<TransformDetector>());
	if (cfg_.runPartition) detectors_.push_back(std::make_unique<PartitionDetector>());
	if (cfg_.runForEach) detectors_.push_back(std::make_unique<ForEachDetector>());
}

bool AlgorithmDetector::passesPreflight(const ssa::SSAFunction& fn) const
{
	if ((int)fn.blockCount() < cfg_.minBlocks) return false;
	int instrs = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (blk) instrs += static_cast<int>(blk->instrs.size());
	}
	return instrs >= cfg_.minInstrs;
}

EmissionTier AlgorithmDetector::assignTier(float confidence) const
{
	if (confidence >= cfg_.highTierThreshold) return EmissionTier::High;
	if (confidence >= cfg_.mediumTierThreshold) return EmissionTier::Medium;
	return EmissionTier::Low;
}

AlgorithmResult AlgorithmDetector::detect(const ssa::SSAFunction& fn) const
{
	++stats_.functionsAnalysed;

	if (!passesPreflight(fn))
	{
		++stats_.functionsSkipped;
		return {};
	}

	AlgorithmResult best;
	best.confidence = 0.0f;

	for (const auto& det: detectors_)
	{
		auto r = det->detect(fn);
		if (r.confidence > best.confidence) best = r;
	}

	if (best.kind == AlgorithmKind::Unknown || best.confidence < 0.01f) return best;

	// Re-apply tier with our config thresholds.
	best.tier = assignTier(best.confidence);

	// Augment emitted form with iterator pattern recovery.
	auto iterResult = iterRecover_.recover(fn);
	if (iterResult.isBeginEnd && best.tier == EmissionTier::High)
	{
		// Prefer range-for form in the emitted annotation comment.
		if (!iterResult.rangeForForm.empty()) best.emittedForm += " // range: " + iterResult.rangeForForm;
	}
	if (iterResult.hasBackInserter) best.hasBackInserter = true;
	if (iterResult.isReverseIter) best.isReverse = true;

	++stats_.detections;
	switch (best.tier)
	{
	case EmissionTier::High: ++stats_.highTier; break;
	case EmissionTier::Medium: ++stats_.mediumTier; break;
	default: ++stats_.lowTier; break;
	}

	return best;
}

AlgorithmDetector::DetectionMap AlgorithmDetector::detectModule(const std::vector<const ssa::SSAFunction*>& fns) const
{
	DetectionMap results;
	for (const auto* fn: fns)
	{
		if (!fn) continue;
		auto r = detect(*fn);
		if (r.kind != AlgorithmKind::Unknown) results.emplace_back(fn->name(), std::move(r));
	}
	return results;
}

} // namespace algo_recover
} // namespace retdec
