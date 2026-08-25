/**
 * @file include/retdec/serdes/object.h
 * @brief Object (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_SERDES_OBJECT_H
#define RETDEC_SERDES_OBJECT_H

#include <rapidjson/document.h>

namespace retdec {

namespace common {
class Object;
} // namespace common

namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::Object& o);
void deserialize(const rapidjson::Value& val, common::Object& o);

} // namespace serdes
} // namespace retdec

#endif