/**
 * @file src/serdes/calling_convention.cpp
 * @brief Calling convention (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <algorithm>
#include <vector>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "retdec/common/calling_convention.h"
#include "retdec/serdes/calling_convention.h"
#include "retdec/serdes/std.h"

namespace {

const std::vector<std::string> ccStrings = {
	"unknown",
	"voidarg",
	"cdecl",
	"ellipsis",
	"stdcall",
	"pascal",
	"fastcall",
	"thiscall",
	"manual",
	"spoiled",
	"speciale",
	"specialp",
	"special",
	"watcom",
	"x64_os_default",
	"arm_default",
	"arm64_default",
	"mips_default",
	"mips64_default",
	"powerpc_default",
	"powerpc64_default",
	"pic32_default",
	// eCC gained these two; the table did not, so serialize()'s
	// `ccStrings.size() > cc.getID()` was false for both and they were written
	// as "unknown" and read back as CC_UNKNOWN. That is live data loss:
	// param_return.cpp maps demangled "vectorcall"/"regcall" onto exactly
	// these IDs and calling_convention.cpp registers handlers for them, so an
	// MSVC __vectorcall or Intel __regcall function lost its convention across
	// a config write/read round trip.
	"vectorcall",
	"regcall"};

// ccStrings is indexed by eCC, so a value added to the enum without a string
// here serialises as "unknown" and reads back as CC_UNKNOWN -- silently, since
// serialize()'s only check is that the id is in range. Pinning the count makes
// that a build failure instead: when it fires, add the string above and the
// matching case to operator<< in src/common/calling_convention.cpp.
static_assert(
	static_cast<int>(retdec::common::CallingConvention::eCC::CC_ENDING) == 24,
	"eCC gained a value: add its string to ccStrings and its case to operator<<");

} // anonymous namespace

namespace retdec {
namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::CallingConvention& cc)
{
	if (ccStrings.size() > static_cast<uint64_t>(cc.getID()))
	{
		writer.String(ccStrings[static_cast<uint64_t>(cc.getID())]);
	}
	else
	{
		writer.String(ccStrings[static_cast<uint64_t>(
				common::CallingConvention::eCC::CC_UNKNOWN)]);
	}
}
SERIALIZE_EXPLICIT_INSTANTIATION(common::CallingConvention)

void deserialize(const rapidjson::Value& val, common::CallingConvention& cc)
{
	if (val.IsNull() || !val.IsString())
	{
		return;
	}

	std::string enumStr = val.GetString();
	auto it = std::find(ccStrings.begin(), ccStrings.end(), enumStr);
	if (it == ccStrings.end())
	{
		cc.setIsUnknown();
	}
	else
	{
		cc.set(static_cast<common::CallingConvention::eCC>(
				std::distance(ccStrings.begin(), it)));
	}
}

} // namespace serdes
} // namespace retdec
