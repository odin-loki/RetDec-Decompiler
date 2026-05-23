/**
 * @file include/retdec/common/semantic_detection.h
 * @brief Post-pipeline semantic recovery detection record.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_COMMON_SEMANTIC_DETECTION_H
#define RETDEC_COMMON_SEMANTIC_DETECTION_H

#include <sstream>
#include <iomanip>
#include <string>

namespace retdec {
namespace common {

struct SemanticDetection {
	std::string kind;       ///< e.g. "container", "sort", "algorithm", "concurrency"
	std::string label;      ///< e.g. "std::vector", "introsort"
	float confidence = 0.0f;
	std::string detail;

	std::string commentLine() const
	{
		std::ostringstream oss;
		oss << "[RetDec] " << label << " detected (confidence "
		    << std::fixed << std::setprecision(2) << confidence << ')';
		return oss.str();
	}
};

} // namespace common
} // namespace retdec

#endif
