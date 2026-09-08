/**
 * @file include/retdec/utils/ord_lookup.h
 * @brief Converts well-known ordinals to function names
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_UTILS_ORD_LOOKUP_H
#define RETDEC_UTILS_ORD_LOOKUP_H

// std::string and std::size_t are named below and were declared by nothing:
// this header compiled only where an earlier include happened to have
// pulled them in.
#include <cstddef>
#include <string>

namespace retdec {
namespace utils {

std::string ordLookUp(const std::string& libName, const std::size_t& ordNum, bool forceNameFromOrdinal);

} // namespace utils
} // namespace retdec

#endif
