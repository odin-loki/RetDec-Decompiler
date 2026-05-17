/**
* @file include/retdec/utils/memory.h
* @brief Memory utilities.
* @copyright (c) 2017 Odin Loch Trading as Imortek
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
