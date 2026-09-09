/**
 * @file tests/serdes/std_tests.cpp
 * @brief Tests for the (de)serialization of C++ standard types.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rapidjson/document.h>

#include "retdec/serdes/std.h"

using namespace ::testing;

namespace retdec {
namespace serdes {
namespace tests {

class DeserializeStdStringTests : public Test {
protected:
	rapidjson::Document parse(const std::string& json)
	{
		rapidjson::Document d;
		d.Parse(json.c_str());
		EXPECT_FALSE(d.HasParseError()) << "test fixture JSON is malformed";
		return d;
	}
};

TEST_F(DeserializeStdStringTests, StringMemberIsRead)
{
	auto d = parse(R"({ "k": "value" })");

	std::string s = "untouched";
	deserialize(d["k"], s);

	EXPECT_EQ("value", s);
}

TEST_F(DeserializeStdStringTests, AnEmbeddedNulSurvives)
{
	// The length has to come from GetStringLength(); a strlen() of the pointer
	// would stop at the escape.
	auto d = parse(R"({ "k": "a\u0000b" })");

	std::string s;
	deserialize(d["k"], s);

	EXPECT_EQ(std::string("a\0b", 3), s);
}

//
// Every other deserialize() in this module refuses a value of the wrong JSON
// type and leaves its output at the default. These check that the two string
// overloads do the same rather than reading the union as a pointer.
//

TEST_F(DeserializeStdStringTests, ANumberIsRefusedAndNotReadAsAPointer)
{
	auto d = parse(R"({ "k": 123456789 })");

	std::string s = "untouched";
	deserialize(d["k"], s);

	EXPECT_TRUE(s.empty());
}

TEST_F(DeserializeStdStringTests, EveryNonStringTypeIsRefused)
{
	auto d = parse(R"({ "n": 1, "d": 1.5, "b": true, "o": {}, "a": [], "z": null })");

	for (const char* key: {"n", "d", "b", "o", "a", "z"})
	{
		std::string s = "untouched";
		deserialize(d[key], s);
		EXPECT_TRUE(s.empty()) << "member " << key << " was not refused";
	}
}

TEST_F(DeserializeStdStringTests, ANumberIsRefusedByTheCharPointerOverload)
{
	auto d = parse(R"({ "k": 123456789 })");

	const char* str = nullptr;
	deserialize(d["k"], str);

	ASSERT_NE(nullptr, str);
	EXPECT_STREQ("", str);
}

TEST_F(DeserializeStdStringTests, AContainerOfStringsSurvivesANumericElement)
{
	auto d = parse(R"({ "paths": ["/a", 123456789, "/b"] })");

	std::vector<std::string> paths;
	deserializeContainer(d, "paths", paths);

	// The refused element still occupies a slot, exactly as a refused object
	// does for every other container element type in this module.
	ASSERT_EQ(3u, paths.size());
	EXPECT_EQ("/a", paths[0]);
	EXPECT_TRUE(paths[1].empty());
	EXPECT_EQ("/b", paths[2]);
}

TEST_F(DeserializeStdStringTests, AKeyedStringOfTheWrongTypeIsRefused)
{
	auto d = parse(R"({ "k": 123456789 })");

	std::string s = "untouched";
	deserialize(d, "k", s);

	EXPECT_TRUE(s.empty());
}

TEST_F(DeserializeStdStringTests, AMissingKeyLeavesTheOutputAlone)
{
	auto d = parse(R"({ "other": "x" })");

	std::string s = "untouched";
	deserialize(d, "k", s);

	EXPECT_EQ("untouched", s);
}

//
//=============================================================================
// The keyed helpers
//=============================================================================
//

class DeserializeKeyedTests : public DeserializeStdStringTests {};

TEST_F(DeserializeKeyedTests, TypedHelpersFallBackToTheDefaultOnAMismatch)
{
	auto d = parse(R"({ "s": "text" })");

	EXPECT_EQ(7, deserializeInt64(d, "s", 7));
	EXPECT_EQ(7u, deserializeUint64(d, "s", 7));
	EXPECT_TRUE(deserializeBool(d, "s", true));
	EXPECT_DOUBLE_EQ(1.5, deserializeDouble(d, "s", 1.5));
	EXPECT_EQ("fallback", deserializeString(d, "n", "fallback"));
}

} // namespace tests
} // namespace serdes
} // namespace retdec
