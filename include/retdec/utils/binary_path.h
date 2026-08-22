/**
 * @file include/retdec/utils/binary_path.h
 * @brief Absolute path of currently running binary getters.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_UTILS_BINARY_PATH_H
#define RETDEC_UTILS_BINARY_PATH_H

#include "retdec/utils/filesystem.h"

namespace retdec {
namespace utils {

fs::path getThisBinaryPath();
fs::path getThisBinaryDirectoryPath();

} // namespace utils
} // namespace retdec

#endif
