/**
 * @file include/retdec/serdes/semantic_detection.h
 * @brief SemanticDetection (de)serialization.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_SERDES_SEMANTIC_DETECTION_H
#define RETDEC_SERDES_SEMANTIC_DETECTION_H

#include <rapidjson/document.h>

namespace retdec {

namespace common {
struct SemanticDetection;
} // namespace common

namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::SemanticDetection& d);
void deserialize(const rapidjson::Value& val, common::SemanticDetection& d);

} // namespace serdes
} // namespace retdec

#endif
