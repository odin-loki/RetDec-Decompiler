/**
 * @file src/serdes/semantic_detection.cpp
 * @brief SemanticDetection (de)serialization.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "retdec/common/semantic_detection.h"
#include "retdec/serdes/semantic_detection.h"

#include "retdec/serdes/std.h"

namespace {

const std::string JSON_sdKind       = "kind";
const std::string JSON_sdLabel      = "label";
const std::string JSON_sdConfidence = "confidence";
const std::string JSON_sdDetail     = "detail";
const std::string JSON_sdCHint      = "cHint";

} // anonymous namespace

namespace retdec {
namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::SemanticDetection& d)
{
	writer.StartObject();
	serializeString(writer, JSON_sdKind, d.kind);
	serializeString(writer, JSON_sdLabel, d.label);
	serializeDouble(writer, JSON_sdConfidence, d.confidence);
	serializeString(writer, JSON_sdDetail, d.detail);
	if (!d.cHint.empty())
	{
		serializeString(writer, JSON_sdCHint, d.cHint);
	}
	writer.EndObject();
}
SERIALIZE_EXPLICIT_INSTANTIATION(common::SemanticDetection)

void deserialize(const rapidjson::Value& val, common::SemanticDetection& d)
{
	if (val.IsNull() || !val.IsObject())
	{
		return;
	}

	d.kind = deserializeString(val, JSON_sdKind);
	d.label = deserializeString(val, JSON_sdLabel);
	d.confidence = static_cast<float>(deserializeDouble(val, JSON_sdConfidence));
	d.detail = deserializeString(val, JSON_sdDetail);
	d.cHint = deserializeString(val, JSON_sdCHint);
}

} // namespace serdes
} // namespace retdec
