/**
 * @file include/retdec/serdes/function.h
 * @brief Function (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_SERDES_FUNCTION_H
#define RETDEC_SERDES_FUNCTION_H

#include <rapidjson/document.h>

namespace retdec {

namespace common {
class Function;
} // namespace common

namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::Function& f);
void deserialize(const rapidjson::Value& val, common::Function& f);

} // namespace serdes
} // namespace retdec

#endif