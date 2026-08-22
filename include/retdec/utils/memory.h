/**
* @file include/retdec/utils/memory.h
* @brief Memory utilities.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_UTILS_MEMORY_H
#define RETDEC_UTILS_MEMORY_H

#include <cstdlib>

namespace retdec {
namespace utils {

std::size_t getTotalSystemMemory();
bool limitSystemMemory(std::size_t limit);
bool limitSystemMemoryToHalfOfTotalSystemMemory();

} // namespace utils
} // namespace retdec

#endif
