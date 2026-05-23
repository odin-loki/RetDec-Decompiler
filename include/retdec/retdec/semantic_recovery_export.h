/**
 * @file include/retdec/retdec/semantic_recovery_export.h
 * @brief Export post-pipeline semantic detections to config JSON and decompiled C.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_RETDEC_SEMANTIC_RECOVERY_EXPORT_H
#define RETDEC_RETDEC_SEMANTIC_RECOVERY_EXPORT_H

#include <string>
#include <unordered_map>
#include <vector>

#include "retdec/common/semantic_detection.h"
#include "retdec/config/config.h"
#include "retdec/algo_recover/algo_recover.h"
#include "retdec/concurrency_detect/concurrency_detect.h"
#include "retdec/container_detect/container_detect.h"
#include "retdec/sort_detect/sort_detect.h"

namespace retdec {
namespace analysis {

using SemanticDetectionMap =
		std::unordered_map<std::string, std::vector<common::SemanticDetection>>;

SemanticDetectionMap buildSemanticDetectionMap(
		const container_detect::ContainerDetector::DetectionMap& containers,
		const std::vector<std::pair<std::string, algo_recover::AlgorithmResult>>& algos,
		const sort_detect::SortDetector::DetectionMap& sorts,
		const concurrency_detect::ConcurrencyModel& concurrency,
		const std::string& outputLang = {});

void mergeSemanticDetectionsIntoConfig(
		config::Config& config,
		const SemanticDetectionMap& detections);

void injectSemanticCommentsIntoOutput(
		const config::Config& config,
		std::string* outString);

void exportSemanticRecovery(
		config::Config& config,
		const SemanticDetectionMap& detections,
		std::string* outString);

} // namespace analysis
} // namespace retdec

#endif
