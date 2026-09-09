/**
 * @file src/serdes/address.cpp
 * @brief Address (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "retdec/serdes/address.h"
#include "retdec/serdes/std.h"

namespace {

const std::string JSON_start  = "start";
const std::string JSON_end    = "end";

} // anonymous namespace

namespace retdec {
namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::Address& a)
{
	writer.String(a.isDefined() ? a.toHexPrefixString() : std::string());
}
SERIALIZE_EXPLICIT_INSTANTIATION(common::Address)

void deserialize(const rapidjson::Value& val, common::Address& a)
{
	if (val.IsNull() || !val.IsString())
	{
		return;
	}

	a = common::Address(val.GetString());
}

template <typename Writer>
void serialize(Writer& writer, const common::AddressRange& r)
{
	writer.StartObject();
	if (r.getStart().isDefined() && r.getEnd().isDefined())
	{
		serialize(writer, JSON_start, r.getStart());
		serialize(writer, JSON_end, r.getEnd());
	}
	writer.EndObject();
}
SERIALIZE_EXPLICIT_INSTANTIATION(common::AddressRange)

void deserialize(const rapidjson::Value& val, common::AddressRange& r)
{
	if (val.IsNull() || !val.IsObject())
	{
		return;
	}

	common::Address s, e;
	deserialize(val, JSON_start, s);
	deserialize(val, JSON_end, e);
	// setStart()/setEnd() throw common::InvalidRangeException when the pair is
	// inconsistent, and that type lives in namespace retdec::common deriving
	// from std::exception rather than from config::Exception -- so every
	// caller of the config reader misses it and the process aborts.
	// retdec-decompiler.cpp catches only config::ParseException, and so do
	// json_config.cpp and fileinfo. A file carrying "endAddr" with no
	// "startAddr" was enough: the start is then Address::Undefined, which is
	// 0xFFFFFFFFFFFFFFFF, so any end is below it. An inconsistent pair is left
	// at the default instead, which is what every other deserializer in this
	// module does with a value it cannot use.
	if (!(e < s))
	{
		r.setStartEnd(s, e);
	}
}

} // namespace serdes
} // namespace retdec
