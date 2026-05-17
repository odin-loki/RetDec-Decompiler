/**
 * @file include/retdec/serdes/tool_info.h
 * @brief Tool information (de)serialization.
 * @copyright (c) 2019 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_SERDES_TOOL_INFO_H
#define RETDEC_SERDES_TOOL_INFO_H

#include <rapidjson/document.h>

namespace retdec {

namespace common {
class ToolInfo;
} // namespace common

namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::ToolInfo& ti);
void deserialize(const rapidjson::Value& val, common::ToolInfo& ti);

} // namespace serdes
} // namespace retdec

#endif