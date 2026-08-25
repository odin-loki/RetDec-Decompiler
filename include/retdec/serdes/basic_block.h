/**
 * @file include/retdec/serdes/basic_block.h
 * @brief Basic block (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_SERDES_BASIC_BLOCK_H
#define RETDEC_SERDES_BASIC_BLOCK_H

#include <rapidjson/document.h>

namespace retdec {

namespace common {
class BasicBlock;
} // namespace common

namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::BasicBlock::CallEntry& ce);
void deserialize(const rapidjson::Value& val, common::BasicBlock::CallEntry& ce);

template <typename Writer>
void serialize(Writer& writer, const common::BasicBlock& bb);
void deserialize(const rapidjson::Value& val, common::BasicBlock& bb);

} // namespace serdes
} // namespace retdec

#endif