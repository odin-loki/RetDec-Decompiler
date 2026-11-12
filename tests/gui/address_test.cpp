/**
 * @file tests/gui/address_test.cpp
 */

#include "retdec/gui/model/address.h"

#include <gtest/gtest.h>

namespace m = retdec::gui::model;

TEST(Address, FormatAddressHex)
{
	EXPECT_EQ(m::formatAddressHex(0x401000), "0x401000");
	EXPECT_EQ(m::formatAddressHex(0), "0x0");
	EXPECT_EQ(m::formatAddressHex(0xDEADBEEFull), "0xdeadbeef");
	EXPECT_STREQ(m::kCopyAddressLabel, "Copy Address");
	EXPECT_STREQ(m::kGoToFunctionLabel, "Go to Function");
}

TEST(Address, ParseFirstAddress)
{
	EXPECT_EQ(m::parseFirstAddress("0x401000:   55   push ebp"), 0x401000u);
	EXPECT_EQ(m::parseFirstAddress("call 0xABCdef ; 0x10"), 0xabcdefu);
	EXPECT_EQ(m::parseFirstAddress("x0x12"), 0x12u);
	EXPECT_FALSE(m::parseFirstAddress("no address here").has_value());
	EXPECT_FALSE(m::parseFirstAddress("0x").has_value());
	EXPECT_FALSE(m::parseFirstAddress("0X10").has_value());                // lowercase x only, as before
	EXPECT_FALSE(m::parseFirstAddress("0x1ffffffffffffffff").has_value()); // does not fit
	EXPECT_EQ(m::parseFirstAddress("0xg 0x5"), 0x5u);
}

TEST(Address, ParseAddress)
{
	EXPECT_EQ(m::parseAddress(" 0x401000 "), 0x401000u);
	EXPECT_EQ(m::parseAddress("0X1F"), 0x1fu);
	EXPECT_EQ(m::parseAddress("4198400"), 4198400u);
	EXPECT_FALSE(m::parseAddress("").has_value());
	EXPECT_FALSE(m::parseAddress("0x").has_value());
	EXPECT_FALSE(m::parseAddress("12ab").has_value());
	EXPECT_FALSE(m::parseAddress("-1").has_value());
}
