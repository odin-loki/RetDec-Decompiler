/**
 * @file tests/serdes/calling_convention_tests.cpp
 * @brief Tests for the calling convention module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <gtest/gtest.h>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "retdec/common/calling_convention.h"
#include "retdec/serdes/calling_convention.h"
#include "retdec/utils/string.h"

#include <sstream>
#include <string>

using namespace ::testing;

namespace retdec {
namespace serdes {
namespace tests {

class CallingConventionTests : public Test
{
	protected:
		common::CallingConvention cc;

		std::string _serialize()
		{
			rapidjson::StringBuffer sb;
			rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);

			serialize(writer, cc);
			std::string ret = sb.GetString();
			// First and last character should be '"'.
			ret = utils::trim(ret, "\"");
			return ret;
		}
};

TEST_F(CallingConventionTests, CheckSerialization)
{
	// Uninitialized CC is unknown.
	EXPECT_EQ("unknown", _serialize());

	cc.setIsUnknown();
	EXPECT_EQ("unknown", _serialize());

	cc.setIsVoidarg();
	EXPECT_EQ("voidarg", _serialize());

	cc.setIsCdecl();
	EXPECT_EQ("cdecl", _serialize());

	cc.setIsEllipsis();
	EXPECT_EQ("ellipsis", _serialize());

	cc.setIsStdcall();
	EXPECT_EQ("stdcall", _serialize());

	cc.setIsPascal();
	EXPECT_EQ("pascal", _serialize());

	cc.setIsFastcall();
	EXPECT_EQ("fastcall", _serialize());

	cc.setIsThiscall();
	EXPECT_EQ("thiscall", _serialize());

	cc.setIsManual();
	EXPECT_EQ("manual", _serialize());

	cc.setIsSpoiled();
	EXPECT_EQ("spoiled", _serialize());

	cc.setIsSpecialE();
	EXPECT_EQ("speciale", _serialize());

	cc.setIsSpecialP();
	EXPECT_EQ("specialp", _serialize());

	cc.setIsSpecial();
	EXPECT_EQ("special", _serialize());
}

TEST_F(CallingConventionTests, CheckDeserialization)
{
	EXPECT_TRUE(cc.isUnknown());

	deserialize(rapidjson::Value(""), cc);
	EXPECT_TRUE(cc.isUnknown());

	deserialize(rapidjson::Value("unknown"), cc);
	EXPECT_TRUE(cc.isUnknown());

	deserialize(rapidjson::Value("voidarg"), cc);
	EXPECT_TRUE(cc.isVoidarg());

	deserialize(rapidjson::Value("cdecl"), cc);
	EXPECT_TRUE(cc.isCdecl());

	deserialize(rapidjson::Value("ellipsis"), cc);
	EXPECT_TRUE(cc.isEllipsis());

	deserialize(rapidjson::Value("stdcall"), cc);
	EXPECT_TRUE(cc.isStdcall());

	deserialize(rapidjson::Value("pascal"), cc);
	EXPECT_TRUE(cc.isPascal());

	deserialize(rapidjson::Value("fastcall"), cc);
	EXPECT_TRUE(cc.isFastcall());

	deserialize(rapidjson::Value("thiscall"), cc);
	EXPECT_TRUE(cc.isThiscall());

	deserialize(rapidjson::Value("manual"), cc);
	EXPECT_TRUE(cc.isManual());

	deserialize(rapidjson::Value("spoiled"), cc);
	EXPECT_TRUE(cc.isSpoiled());

	deserialize(rapidjson::Value("speciale"), cc);
	EXPECT_TRUE(cc.isSpecialE());

	deserialize(rapidjson::Value("specialp"), cc);
	EXPECT_TRUE(cc.isSpecialP());

	deserialize(rapidjson::Value("special"), cc);
	EXPECT_TRUE(cc.isSpecial());
}

// ccStrings is indexed by eCC and ended two rows short of it, so serialize()'s
// only check -- `ccStrings.size() > cc.getID()` -- was false for both new
// conventions and they were written as "unknown" and read back as CC_UNKNOWN.
// That is live data loss: param_return.cpp maps demangled "vectorcall" and
// "regcall" onto exactly these ids and calling_convention.cpp registers
// handlers for them, so an MSVC __vectorcall or Intel __regcall function lost
// its convention across a config write/read round trip.
TEST_F(CallingConventionTests, VectorcallAndRegcallRoundTrip)
{
	using eCC = common::CallingConvention::eCC;

	cc = common::CallingConvention(eCC::CC_VECTORCALL);
	EXPECT_EQ("vectorcall", _serialize());

	cc = common::CallingConvention(eCC::CC_REGCALL);
	EXPECT_EQ("regcall", _serialize());

	deserialize(rapidjson::Value("vectorcall"), cc);
	EXPECT_EQ(eCC::CC_VECTORCALL, cc.getID());

	deserialize(rapidjson::Value("regcall"), cc);
	EXPECT_EQ(eCC::CC_REGCALL, cc.getID());
}

// Every eCC value has a string, so a convention added to the enum cannot go on
// silently serialising as "unknown".
TEST_F(CallingConventionTests, EveryConventionHasAString)
{
	using eCC = common::CallingConvention::eCC;
	for (int i = 0; i < static_cast<int>(eCC::CC_ENDING); ++i)
	{
		cc = common::CallingConvention(static_cast<eCC>(i));
		const std::string text = _serialize();
		if (i != static_cast<int>(eCC::CC_UNKNOWN))
			EXPECT_NE("unknown", text) << "eCC value " << i << " has no string of its own";

		common::CallingConvention back;
		deserialize(rapidjson::Value(text.c_str(), static_cast<rapidjson::SizeType>(text.size())), back);
		EXPECT_EQ(static_cast<eCC>(i), back.getID()) << text;
	}
}

// operator<< prints the convention's name, and printed "UNHANDLED" for these
// two while the enum carried them.
TEST_F(CallingConventionTests, EveryConventionPrintsItsName)
{
	using eCC = common::CallingConvention::eCC;
	for (int i = 0; i < static_cast<int>(eCC::CC_ENDING); ++i)
	{
		std::ostringstream os;
		os << static_cast<eCC>(i);
		EXPECT_NE("UNHANDLED", os.str()) << "eCC value " << i;
	}
}

} // namespace tests
} // namespace serdes
} // namespace retdec
