/**
 * @file src/opencl/ocl_disk_cache.cpp
 * @brief Persist compiled OpenCL program binaries under ~/.retdec/ocl-cache (or %USERPROFILE%\\.retdec\\ocl-cache).
 */

#include "ocl_disk_cache_internal.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace retdec {
namespace opencl {
namespace detail {
namespace {

std::uint64_t fnv1a64(std::string_view s)
{
	constexpr std::uint64_t offset = 14695981039346656037ULL;
	constexpr std::uint64_t prime = 1099511628211ULL;
	std::uint64_t h = offset;
	for (unsigned char c : s) {
		h ^= static_cast<std::uint64_t>(c);
		h *= prime;
	}
	return h;
}

std::string hexU64(std::uint64_t v)
{
	std::ostringstream oss;
	oss << std::hex << std::setfill('0') << std::setw(16) << v;
	return oss.str();
}

} // namespace

std::string oclCacheDir()
{
	namespace fs = std::filesystem;
	const char *home = nullptr;
#ifdef _WIN32
	home = std::getenv("USERPROFILE");
#else
	home = std::getenv("HOME");
#endif
	if (!home || !home[0]) {
		return {};
	}
	fs::path base = fs::path(home) / ".retdec" / "ocl-cache";
	std::error_code ec;
	fs::create_directories(base, ec);
	if (ec) {
		return {};
	}
	return base.string();
}

std::string oclCachePathForCompositeKey(const std::string &compositeKey)
{
	const std::string dir = oclCacheDir();
	if (dir.empty()) {
		return {};
	}
	return dir + "/" + hexU64(fnv1a64(compositeKey)) + ".bin";
}

bool loadProgramBinary(const std::string &compositeKey, std::vector<unsigned char> &out)
{
	const std::string path = oclCachePathForCompositeKey(compositeKey);
	if (path.empty()) {
		return false;
	}
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		return false;
	}
	const auto end = f.tellg();
	if (end <= 0) {
		return false;
	}
	f.seekg(0);
	out.resize(static_cast<size_t>(end));
	if (!f.read(reinterpret_cast<char *>(out.data()), end)) {
		out.clear();
		return false;
	}
	return true;
}

bool saveProgramBinary(const std::string &compositeKey, const std::vector<unsigned char> &in)
{
	const std::string path = oclCachePathForCompositeKey(compositeKey);
	if (path.empty() || in.empty()) {
		return false;
	}
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f.write(reinterpret_cast<const char *>(in.data()), static_cast<std::streamsize>(in.size()))) {
		return false;
	}
	return true;
}

} // namespace detail
} // namespace opencl
} // namespace retdec
