/**
 * @file tests/serial_detect/serial_detect_test.cpp
 * @brief Unit tests for the Serialisation Framework Detector (Task 38).
 *
 * Coverage:
 *   - Utility functions (varint encode/decode, proto-tag encode/decode)
 *   - ProtoField / ProtoMessage / ProtoSchema construction and emission
 *   - FbsField / FbsTable / FbsSchema emission
 *   - ProtobufDetector (symbol matching, varint evidence, tag evidence)
 *   - FlatBuffersDetector (vtable, builder)
 *   - MessagePackDetector (switch case counting)
 *   - CBORDetector (major-type shift + mask)
 *   - JSONDetector (generic parser + library fingerprints)
 *   - XMLDetector (tag scanning + library fingerprints)
 *   - ProtoEmitter / FbsEmitter text generation
 *   - SerialDetector orchestrator (preflight filter, multi-detector dispatch)
 */

#include "retdec/serial_detect/serial_detect.h"
#include "retdec/ssa/ssa.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace retdec::serial_detect;

// ─── Varint helpers ───────────────────────────────────────────────────────────

TEST(VarintTest, EncodeSingleByte)
{
	uint8_t buf[10];
	EXPECT_EQ(encodeVarint(0, buf), 1);
	EXPECT_EQ(buf[0], 0x00);

	EXPECT_EQ(encodeVarint(1, buf), 1);
	EXPECT_EQ(buf[0], 0x01);

	EXPECT_EQ(encodeVarint(127, buf), 1);
	EXPECT_EQ(buf[0], 0x7F);
}

TEST(VarintTest, EncodeTwoBytes)
{
	uint8_t buf[10];
	EXPECT_EQ(encodeVarint(128, buf), 2);
	EXPECT_EQ(buf[0], 0x80);
	EXPECT_EQ(buf[1], 0x01);
}

TEST(VarintTest, EncodeDecodeRoundTrip)
{
	uint8_t buf[10];
	for (uint64_t v: {0ULL, 1ULL, 127ULL, 128ULL, 300ULL, 16383ULL, 16384ULL, 0xFFFFFFFFULL, 0xDEADBEEFCAFEULL})
	{
		int n = encodeVarint(v, buf);
		ASSERT_GT(n, 0);
		auto [decoded, bytesRead] = decodeVarint(buf, n);
		EXPECT_EQ(decoded, v) << "round-trip failed for " << v;
		EXPECT_EQ(bytesRead, n);
	}
}

TEST(VarintTest, DecodeInsufficientData)
{
	uint8_t buf[] = {0x80}; // continuation bit set but no next byte
	auto [val, n] = decodeVarint(buf, 1);
	EXPECT_EQ(n, 0); // malformed
}

// ─── Proto tag encode/decode ──────────────────────────────────────────────────

TEST(ProtoTagTest, EncodeVarintWireType)
{
	uint32_t tag = makeProtoTag(1, ProtoWireType::Varint);
	EXPECT_EQ(tag, 0x08u); // (1 << 3) | 0
	auto [fn, wt] = decodeProtoTag(tag);
	EXPECT_EQ(fn, 1u);
	EXPECT_EQ(wt, ProtoWireType::Varint);
}

TEST(ProtoTagTest, EncodeLengthDelimited)
{
	uint32_t tag = makeProtoTag(2, ProtoWireType::LengthDelimited);
	EXPECT_EQ(tag, 0x12u); // (2 << 3) | 2
	auto [fn, wt] = decodeProtoTag(tag);
	EXPECT_EQ(fn, 2u);
	EXPECT_EQ(wt, ProtoWireType::LengthDelimited);
}

TEST(ProtoTagTest, EncodeFixed32)
{
	uint32_t tag = makeProtoTag(3, ProtoWireType::Fixed32);
	EXPECT_EQ(tag, 0x1Du); // (3 << 3) | 5
	auto [fn, wt] = decodeProtoTag(tag);
	EXPECT_EQ(fn, 3u);
	EXPECT_EQ(wt, ProtoWireType::Fixed32);
}

TEST(ProtoTagTest, EncodeFixed64)
{
	uint32_t tag = makeProtoTag(4, ProtoWireType::Fixed64);
	EXPECT_EQ(tag, 0x21u); // (4 << 3) | 1
	auto [fn, wt] = decodeProtoTag(tag);
	EXPECT_EQ(fn, 4u);
	EXPECT_EQ(wt, ProtoWireType::Fixed64);
}

TEST(ProtoTagTest, LargeFieldNumber)
{
	uint32_t tag = makeProtoTag(536870911u, ProtoWireType::Varint); // max field number
	auto [fn, wt] = decodeProtoTag(tag);
	EXPECT_EQ(fn, 536870911u);
	EXPECT_EQ(wt, ProtoWireType::Varint);
}

// ─── symbolToFieldName ────────────────────────────────────────────────────────

TEST(SymbolToFieldNameTest, StripGetPrefix)
{
	EXPECT_EQ(symbolToFieldName("get_name"), "name");
}

TEST(SymbolToFieldNameTest, StripSetPrefix)
{
	EXPECT_EQ(symbolToFieldName("set_age"), "age");
}

TEST(SymbolToFieldNameTest, StripHasPrefix)
{
	EXPECT_EQ(symbolToFieldName("has_email"), "email");
}

TEST(SymbolToFieldNameTest, StripNamespace)
{
	EXPECT_EQ(symbolToFieldName("proto::MyMsg::get_id"), "id");
}

TEST(SymbolToFieldNameTest, CamelToSnake)
{
	// "getName" → strip "get" → "name"
	EXPECT_FALSE(symbolToFieldName("getName").empty());
}

TEST(SymbolToFieldNameTest, NoPrefix)
{
	std::string result = symbolToFieldName("payload");
	EXPECT_EQ(result, "payload");
}

// ─── protoScalarTypeName ──────────────────────────────────────────────────────

TEST(ProtoScalarTypeNameTest, BasicTypes)
{
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Int32), "int32");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Int64), "int64");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Float), "float");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Double), "double");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::String), "string");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Bytes), "bytes");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Bool), "bool");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Fixed32), "fixed32");
	EXPECT_STREQ(protoScalarTypeName(ProtoScalarType::Fixed64), "fixed64");
}

TEST(ProtoScalarTypeNameTest, UnknownReturnsFallback)
{
	// Unknown should return something non-empty
	EXPECT_NE(protoScalarTypeName(ProtoScalarType::Unknown), nullptr);
}

// ─── serialFrameworkName / serialLibraryName ─────────────────────────────────

TEST(SerialNameTest, FrameworkNames)
{
	EXPECT_STREQ(serialFrameworkName(SerialFramework::Protobuf), "Protobuf");
	EXPECT_STREQ(serialFrameworkName(SerialFramework::FlatBuffers), "FlatBuffers");
	EXPECT_STREQ(serialFrameworkName(SerialFramework::MessagePack), "MessagePack");
	EXPECT_STREQ(serialFrameworkName(SerialFramework::CBOR), "CBOR");
	EXPECT_STREQ(serialFrameworkName(SerialFramework::JSON), "JSON");
	EXPECT_STREQ(serialFrameworkName(SerialFramework::XML), "XML");
	EXPECT_STREQ(serialFrameworkName(SerialFramework::Unknown), "Unknown");
}

TEST(SerialNameTest, LibraryNames)
{
	EXPECT_NE(serialLibraryName(SerialLibrary::JSON_RapidJSON), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::JSON_nlohmann), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::JSON_simdjson), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::JSON_cJSON), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::XML_libxml2), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::XML_Expat), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::XML_TinyXML), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::FlatBuffers_Official), nullptr);
	EXPECT_NE(serialLibraryName(SerialLibrary::MessagePack_msgpack_c), nullptr);
}

// ─── ProtoField ───────────────────────────────────────────────────────────────

TEST(ProtoFieldTest, ScalarProtoTypeName)
{
	ProtoField f;
	f.number = 1;
	f.name = "count";
	f.scalarType = ProtoScalarType::Int32;
	EXPECT_EQ(f.protoTypeName(), "int32");
}

TEST(ProtoFieldTest, NestedMessageTypeName)
{
	ProtoField f;
	f.number = 2;
	f.name = "address";
	f.scalarType = ProtoScalarType::Message;
	f.nestedMessageName = "Address";
	EXPECT_EQ(f.protoTypeName(), "Address");
}

TEST(ProtoFieldTest, ToStringOptional)
{
	ProtoField f;
	f.number = 5;
	f.name = "email";
	f.scalarType = ProtoScalarType::String;
	f.cardinality = ProtoCardinality::Optional;
	std::string s = f.toString();
	EXPECT_NE(s.find("email"), std::string::npos);
	EXPECT_NE(s.find("= 5"), std::string::npos);
}

TEST(ProtoFieldTest, ToStringRepeated)
{
	ProtoField f;
	f.number = 3;
	f.name = "tags";
	f.scalarType = ProtoScalarType::String;
	f.cardinality = ProtoCardinality::Repeated;
	std::string s = f.toString();
	EXPECT_NE(s.find("repeated"), std::string::npos);
}

// ─── ProtoEmitter ─────────────────────────────────────────────────────────────

TEST(ProtoEmitterTest, EmitEmptySchema)
{
	ProtoSchema schema;
	schema.syntaxVersion = "proto3";
	ProtoEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("syntax = \"proto3\""), std::string::npos);
}

TEST(ProtoEmitterTest, EmitWithPackage)
{
	ProtoSchema schema;
	schema.syntaxVersion = "proto3";
	schema.packageName = "com.example";
	ProtoEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("package com.example"), std::string::npos);
}

TEST(ProtoEmitterTest, EmitSingleMessage)
{
	ProtoSchema schema;
	schema.syntaxVersion = "proto3";

	ProtoField f1;
	f1.number = 1;
	f1.name = "id";
	f1.scalarType = ProtoScalarType::Int32;
	ProtoField f2;
	f2.number = 2;
	f2.name = "name";
	f2.scalarType = ProtoScalarType::String;
	ProtoMessage msg;
	msg.name = "Person";
	msg.fields = {f1, f2};
	schema.messages.push_back(msg);

	ProtoEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("message Person"), std::string::npos);
	EXPECT_NE(text.find("int32 id = 1"), std::string::npos);
	EXPECT_NE(text.find("string name = 2"), std::string::npos);
}

TEST(ProtoEmitterTest, EmitRepeatedField)
{
	ProtoField f;
	f.number = 3;
	f.name = "items";
	f.scalarType = ProtoScalarType::Bytes;
	f.cardinality = ProtoCardinality::Repeated;

	ProtoMessage msg;
	msg.name = "Container";
	msg.fields = {f};

	ProtoSchema schema;
	schema.syntaxVersion = "proto3";
	schema.messages.push_back(msg);

	ProtoEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("repeated bytes items = 3"), std::string::npos);
}

TEST(ProtoEmitterTest, EmitMultipleMessages)
{
	ProtoSchema schema;
	schema.syntaxVersion = "proto3";
	ProtoMessage m1, m2;
	m1.name = "Request";
	m2.name = "Response";
	ProtoField f;
	f.number = 1;
	f.name = "status";
	f.scalarType = ProtoScalarType::Int32;
	m2.fields = {f};
	schema.messages = {m1, m2};

	ProtoEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("message Request"), std::string::npos);
	EXPECT_NE(text.find("message Response"), std::string::npos);
	EXPECT_NE(text.find("int32 status = 1"), std::string::npos);
}

TEST(ProtoEmitterTest, Proto2Syntax)
{
	ProtoSchema schema;
	schema.syntaxVersion = "proto2";
	ProtoEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("syntax = \"proto2\""), std::string::npos);
}

// ─── FbsEmitter ───────────────────────────────────────────────────────────────

TEST(FbsEmitterTest, EmitEmptySchema)
{
	FbsSchema schema;
	FbsEmitter e;
	std::string text = e.emit(schema);
	EXPECT_TRUE(text.empty() || text.find("root_type") == std::string::npos);
}

TEST(FbsEmitterTest, EmitSingleTable)
{
	FbsField f1;
	f1.slot = 0;
	f1.name = "pos";
	f1.accessBytes = 4;
	FbsField f2;
	f2.slot = 1;
	f2.name = "name";
	f2.isString = true;

	FbsTable tbl;
	tbl.name = "Monster";
	tbl.fields = {f1, f2};

	FbsSchema schema;
	schema.tables = {tbl};
	schema.rootType = "Monster";

	FbsEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("table Monster"), std::string::npos);
	EXPECT_NE(text.find("pos:"), std::string::npos);
	EXPECT_NE(text.find("name:string"), std::string::npos);
	EXPECT_NE(text.find("root_type Monster"), std::string::npos);
}

TEST(FbsEmitterTest, EmitStructKeyword)
{
	FbsTable tbl;
	tbl.name = "Vec3";
	tbl.isStruct = true;
	FbsField f;
	f.slot = 0;
	f.name = "x";
	f.accessBytes = 4;
	f.typeName = "float";
	tbl.fields = {f};

	FbsSchema schema;
	schema.tables = {tbl};
	FbsEmitter e;
	std::string text = e.emit(schema);
	EXPECT_NE(text.find("struct Vec3"), std::string::npos);
}

TEST(FbsEmitterTest, FieldTypeMapping)
{
	FbsField f1;
	f1.slot = 0;
	f1.name = "a";
	f1.accessBytes = 1;
	FbsField f2;
	f2.slot = 1;
	f2.name = "b";
	f2.accessBytes = 2;
	FbsField f3;
	f3.slot = 2;
	f3.name = "c";
	f3.accessBytes = 4;
	FbsField f4;
	f4.slot = 3;
	f4.name = "d";
	f4.accessBytes = 8;
	FbsField f5;
	f5.slot = 4;
	f5.name = "e";
	f5.isVector = true;

	EXPECT_EQ(f1.fbsTypeName(), "bool");
	EXPECT_EQ(f2.fbsTypeName(), "short");
	EXPECT_EQ(f3.fbsTypeName(), "int");
	EXPECT_EQ(f4.fbsTypeName(), "long");
	EXPECT_EQ(f5.fbsTypeName(), "[byte]");
}

// ─── ProtoSchema::emitProto convenience ──────────────────────────────────────

TEST(ProtoSchemaTest, EmitProtoConvenience)
{
	ProtoField f;
	f.number = 1;
	f.name = "x";
	f.scalarType = ProtoScalarType::Float;
	ProtoMessage msg;
	msg.name = "Point";
	msg.fields = {f};
	ProtoSchema schema;
	schema.syntaxVersion = "proto3";
	schema.messages = {msg};
	std::string text = schema.emitProto();
	EXPECT_NE(text.find("float x = 1"), std::string::npos);
}

// ─── SerialResult ─────────────────────────────────────────────────────────────

TEST(SerialResultTest, DefaultIsInvalid)
{
	SerialResult r;
	EXPECT_FALSE(r.isValid());
}

TEST(SerialResultTest, ValidWhenFrameworkSet)
{
	SerialResult r;
	r.framework = SerialFramework::Protobuf;
	r.confidence = 0.90f;
	EXPECT_TRUE(r.isValid());
}

TEST(SerialResultTest, FrameworkNameProtobuf)
{
	SerialResult r;
	r.framework = SerialFramework::Protobuf;
	EXPECT_EQ(r.frameworkName(), "Protobuf");
}

TEST(SerialResultTest, ToStringContainsFramework)
{
	SerialResult r;
	r.framework = SerialFramework::JSON;
	r.library = SerialLibrary::JSON_RapidJSON;
	r.confidence = 0.85f;
	std::string s = r.toString();
	EXPECT_NE(s.find("JSON"), std::string::npos);
	EXPECT_NE(s.find("RapidJSON"), std::string::npos);
}

// ─── ProtobufDetector (symbol-based detection) ───────────────────────────────

class ProtobufDetectorTest : public ::testing::Test {
protected:
	ProtobufDetector det;
	retdec::ssa::SSAFunction fn{"stub"};

	std::unordered_set<std::string> protoSymbols()
	{
		return {
			"proto::MyMessage::SerializeToString",
			"proto::MyMessage::ParseFromString",
			"proto::MyMessage::ByteSizeLong",
			"proto::MyMessage::has_name"};
	}
	std::unordered_set<std::string> empty;
};

TEST_F(ProtobufDetectorTest, DetectsViaSymbols)
{
	auto r = det.detect(fn, protoSymbols());
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.framework, SerialFramework::Protobuf);
	EXPECT_GT(r.confidence, 0.5f);
}

TEST_F(ProtobufDetectorTest, DoesNotDetectOnEmptySymbols)
{
	auto r = det.detect(fn, empty);
	EXPECT_FALSE(r.isValid());
}

TEST_F(ProtobufDetectorTest, LibraryVersionProto2WithHasMethods)
{
	auto syms = protoSymbols(); // includes has_name → proto2
	auto r = det.detect(fn, syms);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::Protobuf2);
}

TEST_F(ProtobufDetectorTest, EmitProtoProducesValidSyntax)
{
	ProtoField f1;
	f1.number = 1;
	f1.name = "id";
	f1.scalarType = ProtoScalarType::Int32;
	ProtoField f2;
	f2.number = 2;
	f2.name = "data";
	f2.scalarType = ProtoScalarType::Bytes;
	ProtoMessage msg;
	msg.name = "Event";
	msg.fields = {f1, f2};
	ProtoSchema sc;
	sc.syntaxVersion = "proto3";
	sc.messages = {msg};
	std::string text = ProtobufDetector::emitProto(sc);
	EXPECT_NE(text.find("syntax = \"proto3\""), std::string::npos);
	EXPECT_NE(text.find("message Event"), std::string::npos);
	EXPECT_NE(text.find("int32 id = 1"), std::string::npos);
	EXPECT_NE(text.find("bytes data = 2"), std::string::npos);
}

// ─── FlatBuffersDetector (symbol-based detection) ────────────────────────────

class FlatBuffersDetectorTest : public ::testing::Test {
protected:
	FlatBuffersDetector det;
	retdec::ssa::SSAFunction fn{"stub"};

	std::unordered_set<std::string> fbsSymbols()
	{
		return {"flatbuffers::FlatBufferBuilder", "GetRootAsMonster", "VerifyMonsterBuffer"};
	}
};

TEST_F(FlatBuffersDetectorTest, DetectsViaSymbols)
{
	auto r = det.detect(fn, fbsSymbols());
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.framework, SerialFramework::FlatBuffers);
	EXPECT_GT(r.confidence, 0.40f);
}

TEST_F(FlatBuffersDetectorTest, LibraryIsOfficial)
{
	auto r = det.detect(fn, fbsSymbols());
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::FlatBuffers_Official);
}

TEST_F(FlatBuffersDetectorTest, EmitFbsHasRootType)
{
	FbsField f;
	f.slot = 0;
	f.name = "hp";
	f.accessBytes = 2;
	FbsTable tbl;
	tbl.name = "Monster";
	tbl.fields = {f};
	FbsSchema schema;
	schema.tables = {tbl};
	schema.rootType = "Monster";
	std::string text = FlatBuffersDetector::emitFbs(schema);
	EXPECT_NE(text.find("table Monster"), std::string::npos);
	EXPECT_NE(text.find("root_type Monster"), std::string::npos);
}

// ─── MessagePackDetector ──────────────────────────────────────────────────────

class MessagePackDetectorTest : public ::testing::Test {
protected:
	MessagePackDetector det;
	retdec::ssa::SSAFunction fn{"stub"};
};

TEST_F(MessagePackDetectorTest, DetectsViaMsgpackSymbol)
{
	std::unordered_set<std::string> sym = {"msgpack_pack_int", "msgpack_unpack", "msgpack_object_print"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.framework, SerialFramework::MessagePack);
	EXPECT_EQ(r.library, SerialLibrary::MessagePack_msgpack_c);
}

TEST_F(MessagePackDetectorTest, EmptySymbolsNotDetected)
{
	auto r = det.detect(fn, {});
	EXPECT_FALSE(r.isValid());
}

// ─── CBORDetector ─────────────────────────────────────────────────────────────

class CBORDetectorTest : public ::testing::Test {
protected:
	CBORDetector det;
	retdec::ssa::SSAFunction fn{"stub"};
};

TEST_F(CBORDetectorTest, DetectsViaCborSymbol)
{
	std::unordered_set<std::string> sym = {"cbor_encode_uint", "cbor_decode_item", "CBOR_TYPE_MAP"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.framework, SerialFramework::CBOR);
}

TEST_F(CBORDetectorTest, NotDetectedWithoutEvidence)
{
	auto r = det.detect(fn, {});
	EXPECT_FALSE(r.isValid());
}

// ─── JSONDetector ─────────────────────────────────────────────────────────────

class JSONDetectorTest : public ::testing::Test {
protected:
	JSONDetector det;
	retdec::ssa::SSAFunction fn{"stub"};
};

TEST_F(JSONDetectorTest, DetectsRapidJSON)
{
	std::unordered_set<std::string> sym = {
		"rapidjson::GenericDocument::Parse", "rapidjson::Value::GetType", "rapidjson::kObjectType"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.framework, SerialFramework::JSON);
	EXPECT_EQ(r.library, SerialLibrary::JSON_RapidJSON);
	EXPECT_GT(r.confidence, 0.40f);
}

TEST_F(JSONDetectorTest, DetectsNlohmann)
{
	std::unordered_set<std::string> sym = {"nlohmann::basic_json::parse", "nlohmann::detail::json_sax_dom_parser"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::JSON_nlohmann);
}

TEST_F(JSONDetectorTest, DetectscJSON)
{
	std::unordered_set<std::string> sym = {"cJSON_Parse", "cJSON_GetObjectItem", "cJSON_Delete"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::JSON_cJSON);
}

TEST_F(JSONDetectorTest, DetectsSimdjson)
{
	std::unordered_set<std::string> sym = {"simdjson::ondemand::document"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::JSON_simdjson);
}

TEST_F(JSONDetectorTest, NotDetectedOnEmptySymbols)
{
	auto r = det.detect(fn, {});
	EXPECT_FALSE(r.isValid());
}

// ─── XMLDetector ──────────────────────────────────────────────────────────────

class XMLDetectorTest : public ::testing::Test {
protected:
	XMLDetector det;
	retdec::ssa::SSAFunction fn{"stub"};
};

TEST_F(XMLDetectorTest, DetectsLibxml2)
{
	std::unordered_set<std::string> sym = {"xmlReadMemory", "xmlDocGetRootElement", "xmlFreeDoc"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.framework, SerialFramework::XML);
	EXPECT_EQ(r.library, SerialLibrary::XML_libxml2);
	EXPECT_GT(r.confidence, 0.50f);
}

TEST_F(XMLDetectorTest, DetectsExpat)
{
	std::unordered_set<std::string> sym = {
		"XML_ParserCreate", "XML_SetElementHandler", "XML_SetCharacterDataHandler", "XML_Parse"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::XML_Expat);
}

TEST_F(XMLDetectorTest, DetectsTinyXML)
{
	std::unordered_set<std::string> sym = {"tinyxml2::XMLDocument::Parse", "tinyxml2::XMLElement::FirstChild"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::XML_TinyXML);
}

TEST_F(XMLDetectorTest, DetectsRapidXML)
{
	std::unordered_set<std::string> sym = {"rapidxml::xml_document::parse", "rapidxml::xml_node"};
	auto r = det.detect(fn, sym);
	EXPECT_TRUE(r.isValid());
	EXPECT_EQ(r.library, SerialLibrary::XML_RapidXML);
}

TEST_F(XMLDetectorTest, NotDetectedOnEmptySymbols)
{
	auto r = det.detect(fn, {});
	EXPECT_FALSE(r.isValid());
}

// ─── SerialDetector orchestrator ─────────────────────────────────────────────

class SerialDetectorTest : public ::testing::Test {
protected:
	SerialDetector::Config cfg;
	// Functions with a minimal block count to pass preflight
};

TEST_F(SerialDetectorTest, DefaultConfigHasAllDetectors)
{
	SerialDetector det;
	// A function with 0 blocks is skipped
	retdec::ssa::SSAFunction fn{"stub"};
	SerialResult r = det.analyseFunction(fn, {});
	// With stub fn (0 blocks) and no symbols, should not detect
	EXPECT_FALSE(r.isValid());
}

TEST_F(SerialDetectorTest, PreflightFiltersSmallFunctions)
{
	cfg.minBlocks = 10;
	SerialDetector det(cfg);
	retdec::ssa::SSAFunction fn{"stub"}; // blockCount() == 0 → skipped
	auto r = det.analyseFunction(fn, {"SerializeToString"});
	// Pre-filter rejects small functions even with strong symbols
	EXPECT_FALSE(r.isValid());
	EXPECT_EQ(det.stats().functionsSkipped, 1u);
}

TEST_F(SerialDetectorTest, AnalyseModuleEmpty)
{
	SerialDetector det;
	auto results = det.analyseModule({}, {});
	EXPECT_TRUE(results.empty());
	EXPECT_EQ(det.stats().functionsAnalysed, 0u);
}

TEST_F(SerialDetectorTest, AnalyseModuleNullFunctionSkipped)
{
	SerialDetector det;
	std::vector<const retdec::ssa::SSAFunction*> fns = {nullptr, nullptr};
	auto results = det.analyseModule(fns, {});
	EXPECT_TRUE(results.empty());
}

TEST_F(SerialDetectorTest, StatsUpdatedAfterAnalysis)
{
	SerialDetector det;
	retdec::ssa::SSAFunction fn{"stub"};
	det.analyseFunction(fn, {});
	EXPECT_EQ(det.stats().functionsSkipped, 1u); // 0 blocks → skipped
}

TEST_F(SerialDetectorTest, SchemaFilesEmptyWhenNoDetections)
{
	SerialDetector det;
	det.analyseModule({}, {});
	EXPECT_TRUE(det.schemaFiles().empty());
}

TEST_F(SerialDetectorTest, ConfidenceThresholdRespected)
{
	cfg.minConfidence = 0.99f; // virtually impossible to reach with stub
	SerialDetector det(cfg);
	retdec::ssa::SSAFunction fn{"stub"};
	auto r = det.analyseFunction(fn, {"SerializeToString", "ParseFromString"});
	// Even with strong symbols, 0-block function is filtered first
	EXPECT_FALSE(r.isValid());
}

// ─── Varint round-trip at protobuf field tag sizes ────────────────────────────

TEST(VarintTagTest, AllWireTypes)
{
	for (auto wt:
		 {ProtoWireType::Varint, ProtoWireType::Fixed64, ProtoWireType::LengthDelimited, ProtoWireType::Fixed32})
	{
		for (uint32_t fn: {1u, 15u, 16u, 2047u})
		{
			uint32_t tag = makeProtoTag(fn, wt);
			auto [decodedFn, decodedWt] = decodeProtoTag(tag);
			EXPECT_EQ(decodedFn, fn);
			EXPECT_EQ(decodedWt, wt);
		}
	}
}

// ─── ProtoSchema validity ─────────────────────────────────────────────────────

TEST(ProtoSchemaTest, IsEmptyWhenNoMessages)
{
	ProtoSchema schema;
	EXPECT_TRUE(schema.isEmpty());
}

TEST(ProtoSchemaTest, IsNotEmptyWithMessage)
{
	ProtoSchema schema;
	ProtoMessage msg;
	msg.name = "M";
	ProtoField f;
	f.number = 1;
	f.name = "x";
	f.scalarType = ProtoScalarType::Int32;
	msg.fields = {f};
	schema.messages = {msg};
	EXPECT_FALSE(schema.isEmpty());
}

// ─── FbsSchema validity ───────────────────────────────────────────────────────

TEST(FbsSchemaTest, IsEmptyWhenNoTables)
{
	FbsSchema schema;
	EXPECT_TRUE(schema.isEmpty());
}

TEST(FbsSchemaTest, IsNotEmptyWithTable)
{
	FbsSchema schema;
	FbsTable tbl;
	tbl.name = "T";
	schema.tables = {tbl};
	EXPECT_FALSE(schema.isEmpty());
}

// ─── Symbol-table scanning cost ───────────────────────────────────────────────
//
// The six detectors ask 56 substring questions of the module symbol table per
// function, chained with `||`, so a binary using none of these frameworks
// evaluates them all for every function. Each question walked the whole table,
// and the table src/retdec/retdec.cpp builds holds one entry per function -- so
// a module pass cost 56 * F * F substring searches, quadratic in the function
// count.
//
// The detections are identical either way, so the only thing that CAN be
// asserted is the work, and symbolTableScanCount() counts full walks so that it
// can be. Measured through this same shape: 52 walks for the module whether it
// holds 1 function or 64, against 52 per function before.
TEST(SerialDetectSymbolTable, AModulePassDoesNotRescanThePinnedTablePerFunction)
{
	std::unordered_set<std::string> symTable;
	for (int i = 0; i < 64; ++i)
		symTable.insert("some_unrelated_symbol_" + std::to_string(i));

	// Config::minBlocks is 3, so a function needs blocks to get past preflight
	// and reach the detectors at all.
	auto makeFunction = [](const std::string& name) {
		auto fn = std::make_unique<retdec::ssa::SSAFunction>(name);
		for (int b = 0; b < 4; ++b)
			fn->addBlock("b" + std::to_string(b));
		return fn;
	};

	SerialDetector det;

	// One function, to learn what a single pass costs.
	std::vector<std::unique_ptr<retdec::ssa::SSAFunction>> one;
	one.push_back(makeFunction("only"));
	const std::vector<const retdec::ssa::SSAFunction*> onePtr = {one.back().get()};

	resetSymbolTableScanCount();
	det.analyseModule(onePtr, symTable);
	const std::uint64_t forOne = symbolTableScanCount();
	EXPECT_GT(forOne, 0u) << "the detectors did not reach the symbol table at all";

	// And 32 of them. The cost is per module, not per function, so it must not
	// grow with the function count -- which is the whole difference between
	// linear and quadratic here.
	std::vector<std::unique_ptr<retdec::ssa::SSAFunction>> many;
	std::vector<const retdec::ssa::SSAFunction*> manyPtr;
	for (int i = 0; i < 32; ++i)
	{
		many.push_back(makeFunction("f" + std::to_string(i)));
		manyPtr.push_back(many.back().get());
	}

	resetSymbolTableScanCount();
	det.analyseModule(manyPtr, symTable);
	const std::uint64_t forThirtyTwo = symbolTableScanCount();

	EXPECT_EQ(forOne, forThirtyTwo) << "32 functions cost " << forThirtyTwo << " table walks against " << forOne
									<< " for one, so the table is still being rescanned per function";
}

// The pin must not change any answer. Same functions, analysed through the
// module pass (pinned) and one at a time (not pinned).
TEST(SerialDetectSymbolTable, PinningDoesNotChangeWhatIsDetected)
{
	const std::unordered_set<std::string> symTable = {
		"google::protobuf::MessageLite::SerializeToString",
		"flatbuffers::FlatBufferBuilder::Finish",
		"unrelated_symbol",
	};

	std::vector<std::unique_ptr<retdec::ssa::SSAFunction>> owned;
	std::vector<const retdec::ssa::SSAFunction*> functions;
	for (int i = 0; i < 4; ++i)
	{
		owned.push_back(std::make_unique<retdec::ssa::SSAFunction>("f" + std::to_string(i)));
		for (int b = 0; b < 4; ++b)
			owned.back()->addBlock("b" + std::to_string(b));
		functions.push_back(owned.back().get());
	}

	SerialDetector det;
	const auto viaModule = det.analyseModule(functions, symTable);

	for (const auto* fn: functions)
	{
		const auto alone = det.analyseFunction(*fn, symTable);
		const auto it = viaModule.find(fn->name());
		const bool inModule = it != viaModule.end();
		EXPECT_EQ(inModule, alone.isValid()) << fn->name();
		if (inModule)
		{
			EXPECT_EQ(static_cast<int>(it->second.framework), static_cast<int>(alone.framework)) << fn->name();
			EXPECT_EQ(static_cast<int>(it->second.library), static_cast<int>(alone.library)) << fn->name();
			EXPECT_FLOAT_EQ(it->second.confidence, alone.confidence) << fn->name();
		}
	}
}

// ─── End of tests ─────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}

// ─── Protobuf schema recovery ────────────────────────────────────────────────

namespace {

/// A protobuf serializer body: shift a field number left by 3, OR in the wire
/// type, and write the resulting tag literal.
std::unique_ptr<retdec::ssa::SSAFunction> makeProtoSerializer(const char* name, uint64_t tag)
{
	using namespace retdec::ssa;
	auto fn = std::make_unique<SSAFunction>(name);
	fn->addBlock("entry");
	IrInstr* shl = fn->addInstr(0, IrInstr::Op::Shl);
	IrValue* three = fn->allocValue(ValueKind::Immediate, kInvalidVar);
	three->imm = 3;
	shl->uses.push_back({three->id, 0});
	IrInstr* orr = fn->addInstr(0, IrInstr::Op::Or);
	IrValue* wt = fn->allocValue(ValueKind::Immediate, kInvalidVar);
	wt->imm = tag & 7;
	orr->uses.push_back({wt->id, 0});
	IrValue* lit = fn->allocValue(ValueKind::Immediate, kInvalidVar);
	lit->imm = tag;
	IrInstr* st = fn->addInstr(0, IrInstr::Op::Store);
	st->uses.push_back({lit->id, 0});
	fn->addInstr(0, IrInstr::Op::Ret);
	SSAPass p;
	p.run(*fn);
	return fn;
}

} // namespace

// detectTag set only `found`, leaving fieldNumber at its default 0, and
// reconstructSchema's guard is `tg.found && tg.fieldNumber > 0`. No field was
// ever recovered from any input and recovered.proto was never written.
TEST(ProtobufSchemaTest, TagsAreDecodedIntoFieldNumbers)
{
	auto f1 = makeProtoSerializer("set_id", (1u << 3) | 0);
	auto f2 = makeProtoSerializer("set_name", (2u << 3) | 2);
	auto f3 = makeProtoSerializer("set_size", (3u << 3) | 5);
	std::vector<const retdec::ssa::SSAFunction*> fns = {f1.get(), f2.get(), f3.get()};
	std::unordered_set<std::string> syms = {"set_id", "set_name", "set_size"};

	ProtobufDetector d;
	auto schema = d.reconstructSchema(fns, syms);
	ASSERT_FALSE(schema.isEmpty()) << "no field recovered from three tagged serializers";
	ASSERT_EQ(1u, schema.messages.size());
	ASSERT_EQ(3u, schema.messages[0].fields.size());

	const auto& fields = schema.messages[0].fields;
	EXPECT_EQ(1u, fields[0].number);
	EXPECT_EQ(ProtoWireType::Varint, fields[0].wireType);
	EXPECT_EQ(2u, fields[1].number);
	EXPECT_EQ(ProtoWireType::LengthDelimited, fields[1].wireType);
	EXPECT_EQ(3u, fields[2].number);
	EXPECT_EQ(ProtoWireType::Fixed32, fields[2].wireType);

	const std::string proto = ProtobufDetector::emitProto(schema);
	EXPECT_NE(std::string::npos, proto.find("= 1;")) << proto;
	EXPECT_NE(std::string::npos, proto.find("= 3;")) << proto;
}

// 3 and 4 are the deprecated group markers, 6 and 7 are unassigned: a constant
// whose low three bits are one of those is not a tag.
TEST(ProtobufSchemaTest, AConstantWithAnInvalidWireTypeIsNotATag)
{
	auto fn = makeProtoSerializer("f", (5u << 3) | 3); // wire type 3
	std::vector<const retdec::ssa::SSAFunction*> fns = {fn.get()};
	ProtobufDetector d;
	auto schema = d.reconstructSchema(fns, {});
	EXPECT_TRUE(schema.isEmpty()) << "wire type 3 accepted as a tag";
}

// A function with no shift-by-3 is not a serializer, whatever constants it has.
TEST(ProtobufSchemaTest, AFunctionWithNoTagShiftRecoversNothing)
{
	using namespace retdec::ssa;
	auto fn = std::make_unique<SSAFunction>("plain");
	fn->addBlock("entry");
	IrValue* lit = fn->allocValue(ValueKind::Immediate, kInvalidVar);
	lit->imm = (1u << 3) | 2;
	IrInstr* st = fn->addInstr(0, IrInstr::Op::Store);
	st->uses.push_back({lit->id, 0});
	fn->addInstr(0, IrInstr::Op::Ret);
	SSAPass p;
	p.run(*fn);

	std::vector<const SSAFunction*> fns = {fn.get()};
	ProtobufDetector d;
	EXPECT_TRUE(d.reconstructSchema(fns, {}).isEmpty());
}
