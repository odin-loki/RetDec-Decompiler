/**
 * @file include/retdec/serdes/function.h
 * @brief Function (de)serialization.
 * @copyright (c) 2019 Odin Loch Trading as Imortek
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