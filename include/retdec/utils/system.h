/**
* @file include/retdec/utils/system.h
* @brief Portable system utilities.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_UTILS_SYSTEM_H
#define RETDEC_UTILS_SYSTEM_H

namespace retdec {
namespace utils {

bool isLittleEndian();

bool systemHasLongDouble();

} // namespace utils
} // namespace retdec

#endif
