/**
 * @file src/fileformat/lief_adapter.cpp
 * @brief LIEF adapter stub (step 29). Real implementation when RETDEC_HAS_LIEF.
 */

#include "retdec/fileformat/lief_adapter.h"

namespace retdec {
namespace fileformat {

bool LiefAdapter::available()
{
#if defined(RETDEC_HAS_LIEF)
    return true;
#else
    return false;
#endif
}

std::vector<LiefSectionInfo> LiefAdapter::parseSections(const std::string& /*path*/)
{
#if defined(RETDEC_HAS_LIEF)
    // TODO: LIEF::Parser::parse(path) → section list
    return {};
#else
    (void)0;
    return {};
#endif
}

} // namespace fileformat
} // namespace retdec
