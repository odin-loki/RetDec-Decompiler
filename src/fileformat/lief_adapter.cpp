/**
 * @file src/fileformat/lief_adapter.cpp
 * @brief LIEF adapter (step 29). Active when built with RETDEC_HAS_LIEF.
 */

#include "retdec/fileformat/lief_adapter.h"

#if defined(RETDEC_HAS_LIEF)
#include <LIEF/LIEF.hpp>
#endif

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

std::vector<LiefSectionInfo> LiefAdapter::parseSections(const std::string& path)
{
#if defined(RETDEC_HAS_LIEF)
	std::vector<LiefSectionInfo> out;
	std::unique_ptr<LIEF::Binary> binary = LIEF::Parser::parse(path);
	if (!binary) {
		return out;
	}
	out.reserve(binary->sections().size());
	for (const LIEF::Section& sec : binary->sections()) {
		LiefSectionInfo info;
		info.name = sec.fullname();
		info.virtualAddress = sec.virtual_address();
		info.size = sec.size();
		out.push_back(std::move(info));
	}
	return out;
#else
	(void)path;
	return {};
#endif
}

} // namespace fileformat
} // namespace retdec
