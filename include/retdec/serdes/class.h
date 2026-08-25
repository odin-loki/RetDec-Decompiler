/**
 * @file include/retdec/serdes/class.h
 * @brief Class (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_SERDES_CLASS_H
#define RETDEC_SERDES_CLASS_H

#include <rapidjson/document.h>

namespace retdec {

namespace common {
class Class;
} // namespace common

namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::Class& c);
void deserialize(const rapidjson::Value& val, common::Class& c);

} // namespace serdes
} // namespace retdec

#endif