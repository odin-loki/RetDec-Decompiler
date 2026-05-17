/**
* @file include/retdec/utils/system.h
* @brief Portable system utilities.
* @copyright (c) 2017 Odin Loch Trading as Imortek
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
