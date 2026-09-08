/**
 * @file include/retdec/serdes/basic_block.h
 * @brief Basic block (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_SERDES_BASIC_BLOCK_H
#define RETDEC_SERDES_BASIC_BLOCK_H

#include <rapidjson/document.h>

// CallEntry is a member of BasicBlock, and naming a nested type requires the
// enclosing class to be complete, so a forward declaration cannot serve here --
// this header did not compile on its own with one:
//
//   basic_block.h:22:58: error: invalid use of incomplete type
//   'class retdec::common::BasicBlock'
//
// It compiled in-tree only because both of its users happen to include
// common/basic_block.h ahead of it, and .clang-format sets SortIncludes: Never,
// so nothing holds that order. serdes/pattern.h has the same shape for
// common::Pattern::Match and includes the definition for the same reason.
#include "retdec/common/basic_block.h"

namespace retdec {
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