/**
 * @file include/retdec/ar-extractor/detection.h
 * @brief Detection methods.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_AR_EXTRACTOR_DETECTION_H
#define RETDEC_AR_EXTRACTOR_DETECTION_H

#include <string>

namespace retdec {
namespace ar_extractor {

bool isArchive(const std::string &path);

bool isThinArchive(const std::string &path);

bool isNormalArchive(const std::string &path);

bool isFatMachOArchive(const std::string &path);

} // namespace ar_extractor
} // namespace retdec

#endif
