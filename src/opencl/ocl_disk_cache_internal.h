#pragma once

#include <string>
#include <vector>

namespace retdec {
namespace opencl {
namespace detail {

/// Composite key: cacheKey + "|" + source (see OCLContext::ensureProgram).
std::string oclCacheDir();
std::string oclCachePathForCompositeKey(const std::string &compositeKey);
bool loadProgramBinary(const std::string &compositeKey, std::vector<unsigned char> &out);
bool saveProgramBinary(const std::string &compositeKey, const std::vector<unsigned char> &in);

} // namespace detail
} // namespace opencl
} // namespace retdec
