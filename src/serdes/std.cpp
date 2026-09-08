/**
 * @file src/serdes/std.cpp
 * @brief C++ standard types (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/serdes/std.h"

namespace retdec {
namespace serdes {

void deserialize(const rapidjson::Value& val, const char*& str)
{
	// GetString() asserts IsString() only in a debug build of RapidJSON; in a
	// release build it hands back whatever the union happens to hold, which for
	// a number is not a pointer at all.
	str = val.IsString() ? val.GetString() : "";
}

void deserialize(const rapidjson::Value& val, std::string& s)
{
	if (val.IsString())
	{
		s.assign(val.GetString(), val.GetStringLength());
	}
	else
	{
		s.clear();
	}
}

int64_t deserializeInt64(
	const rapidjson::Value& val,
	const std::string& key,
	int64_t defaultValue)
{
	auto res = val.FindMember(key);
	return res != val.MemberEnd() && res->value.IsInt64()
			? res->value.GetInt64()
			: defaultValue;
}

uint64_t deserializeUint64(
	const rapidjson::Value& val,
	const std::string& key,
	uint64_t defaultValue)
{
	auto res = val.FindMember(key);
	return res != val.MemberEnd() && res->value.IsUint64()
			? res->value.GetUint64()
			: defaultValue;
}

bool deserializeBool(
	const rapidjson::Value& val,
	const std::string& key,
	bool defaultValue)
{
	auto res = val.FindMember(key);
	return res != val.MemberEnd() && res->value.IsBool()
			? res->value.GetBool()
			: defaultValue;
}

double deserializeDouble(
	const rapidjson::Value& val,
	const std::string& key,
	double defaultValue)
{
	auto res = val.FindMember(key);
	return res != val.MemberEnd() && res->value.IsDouble()
			? res->value.GetDouble()
			: defaultValue;
}

std::string deserializeString(
	const rapidjson::Value& val,
	const std::string& key,
	const std::string& defaultValue)
{
	auto res = val.FindMember(key);
	return res != val.MemberEnd() && res->value.IsString()
			? res->value.GetString()
			: defaultValue;
}

} // namespace serdes
} // namespace retdec
