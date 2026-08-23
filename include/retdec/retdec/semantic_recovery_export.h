/**
 * @file include/retdec/retdec/semantic_recovery_export.h
 * @brief Export post-pipeline semantic detections to config JSON and decompiled C.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_RETDEC_SEMANTIC_RECOVERY_EXPORT_H
#define RETDEC_RETDEC_SEMANTIC_RECOVERY_EXPORT_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/common/semantic_detection.h"
#include "retdec/concurrency_detect/concurrency_detect.h"
#include "retdec/config/config.h"
#include "retdec/container_detect/container_detect.h"
#include "retdec/sort_detect/sort_detect.h"

namespace retdec {

namespace ssa {
class SSAFunction;
}

namespace analysis {

using SemanticDetectionMap = std::unordered_map<std::string, std::vector<common::SemanticDetection>>;

/// Run CryptoDetector on `fn` and append kind="crypto" hits (label from
/// algorithmName()). Extract does not map kind=crypto.
void appendCryptoDetections(SemanticDetectionMap& map, const ssa::SSAFunction& fn);

/// Run SerialDetector on `fn` and append kind="serial" hits (label from
/// frameworkName()). Name-only symbol-table hits prefix detail with
/// evidence:symbol_name. Extract does not map kind=serial.
void appendSerialDetections(
	SemanticDetectionMap& map, const ssa::SSAFunction& fn, const std::unordered_set<std::string>& symTable);

/// Run PatternDetector on `fn` and append kind="pattern" hits (label from
/// kindName()). RAII table hits, Singleton lock names, Command
/// execute/undo, and Observer subscribe/notify prefix detail with
/// evidence:symbol_name. Extract does not map kind=pattern.
void appendPatternDetections(SemanticDetectionMap& map, const ssa::SSAFunction& fn);

SemanticDetectionMap buildSemanticDetectionMap(
	const container_detect::ContainerDetector::DetectionMap& containers,
	const std::vector<std::pair<std::string, algo_recover::AlgorithmResult>>& algos,
	const std::vector<std::pair<std::string, algo_recover::IdiomResult>>& idioms,
	const sort_detect::SortDetector::DetectionMap& sorts,
	const concurrency_detect::ConcurrencyModel& concurrency,
	const std::string& outputLang = {});

void mergeSemanticDetectionsIntoConfig(config::Config& config, const SemanticDetectionMap& detections);

void injectSemanticCommentsIntoOutput(const config::Config& config, std::string* outString);

void exportSemanticRecovery(config::Config& config, const SemanticDetectionMap& detections, std::string* outString);

/// When RETDEC_EMIT_BUILDABLE is set (non-empty, not "0"), write
/// `<stem>.h`, `<stem>_stubs.c`, and `<stem>.buildable.c` next to outputCPath.
/// `.buildable.c` is a single translation unit: libc headers for undeclared
/// libc calls, extra-arity wrappers, temp injects, and weak link stubs plus
/// `main` when the recovered C has none. Empty outputCPath is a no-op.
/// Does not overwrite the original .c.
void maybeWriteBuildableSidecars(const std::string& outputCPath, const std::string& cSource);

} // namespace analysis
} // namespace retdec

#endif
