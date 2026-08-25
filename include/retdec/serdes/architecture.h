/**
 * @file include/retdec/serdes/architecture.h
 * @brief Architecture (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_SERDES_ARCHITECTURE_H
#define RETDEC_SERDES_ARCHITECTURE_H

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

namespace retdec {

namespace common {
class Architecture;
} // namespace common

namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::Architecture& a);
void deserialize(const rapidjson::Value& val, common::Architecture& a);

} // namespace serdes
} // namespace retdec

#endif