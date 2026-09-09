/**
 * @file src/serdes/basic_block.cpp
 * @brief Basic block (de)serialization.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "retdec/common/basic_block.h"
#include "retdec/serdes/address.h"
#include "retdec/serdes/basic_block.h"
#include "retdec/serdes/std.h"

namespace {

const std::string JSON_startAddr  = "startAddr";
const std::string JSON_endAddr    = "endAddr";
const std::string JSON_preds      = "preds";
const std::string JSON_succs      = "succs";
const std::string JSON_srcAddr    = "srcAddr";
const std::string JSON_targetAddr = "targetAddr";
const std::string JSON_calls      = "calls";

} // anonymous namespace

namespace retdec {
namespace serdes {

template <typename Writer>
void serialize(Writer& writer, const common::BasicBlock::CallEntry& ce)
{
	writer.StartObject();
	serialize(writer, JSON_srcAddr, ce.srcAddr);
	serialize(writer, JSON_targetAddr, ce.targetAddr);
	writer.EndObject();
}
SERIALIZE_EXPLICIT_INSTANTIATION(common::BasicBlock::CallEntry)

void deserialize(const rapidjson::Value& val, common::BasicBlock::CallEntry& ce)
{
	if (val.IsNull() || !val.IsObject())
	{
		return;
	}

	deserialize(val, JSON_srcAddr, ce.srcAddr);
	deserialize(val, JSON_targetAddr, ce.targetAddr);
}

template <typename Writer>
void serialize(Writer& writer, const common::BasicBlock& bb)
{
	writer.StartObject();

	serialize(writer, JSON_startAddr, bb.getStart());
	serialize(writer, JSON_endAddr, bb.getEnd());

	serializeContainer(writer, JSON_preds, bb.preds);
	serializeContainer(writer, JSON_succs, bb.succs);
	serializeContainer(writer, JSON_calls, bb.calls);

	writer.EndObject();
}
SERIALIZE_EXPLICIT_INSTANTIATION(common::BasicBlock)

void deserialize(const rapidjson::Value& val, common::BasicBlock& bb)
{
	if (val.IsNull() || !val.IsObject())
	{
		return;
	}

	common::Address s, e;
	deserialize(val, JSON_startAddr, s);
	deserialize(val, JSON_endAddr, e);
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
		bb.setStart(s);
		bb.setEnd(e);
	}

	deserializeContainer(val, JSON_preds, bb.preds);
	deserializeContainer(val, JSON_succs, bb.succs);
	deserializeContainer(val, JSON_calls, bb.calls);
}

} // namespace serdes
} // namespace retdec
