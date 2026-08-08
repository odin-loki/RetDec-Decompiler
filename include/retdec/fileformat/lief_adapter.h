/**
 * @file include/retdec/fileformat/lief_adapter.h
 * @brief LIEF-backed binary parser facade (step 29 scaffold).
 *
 * Incremental adoption behind RETDEC_ENABLE_LIEF. Default build uses existing
 * fileformat parsers only.
 */

#ifndef RETDEC_FILEFORMAT_LIEF_ADAPTER_H
#define RETDEC_FILEFORMAT_LIEF_ADAPTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace fileformat {

struct LiefSectionInfo {
    std::string name;
    std::uint64_t virtualAddress = 0;
    std::uint64_t size           = 0;
};

/**
 * @brief Thin adapter over LIEF when RETDEC_HAS_LIEF is defined.
 */
class LiefAdapter {
public:
    /// True when built with LIEF and load succeeded.
    static bool available();

    /// Parse sections from a binary path (empty on failure / no LIEF).
    static std::vector<LiefSectionInfo> parseSections(const std::string& path);
};

} // namespace fileformat
} // namespace retdec

#endif
