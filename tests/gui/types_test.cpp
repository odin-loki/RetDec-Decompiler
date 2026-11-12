/**
 * @file tests/gui/types_test.cpp
 * @brief retdec::gui::model types.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/types.h"

#include <gtest/gtest.h>

using namespace retdec::gui::model;

TEST(GuiModelTypes, SelectFunctionsNamePrefersDemangled)
{
	FunctionEntry f;
	f.address = 0x401000;
	EXPECT_EQ(f.selectFunctionsCliName(), "function_401000");
	f.rawName = "_Z3foov";
	EXPECT_EQ(f.selectFunctionsCliName(), "_Z3foov");
	f.name = "foo()";
	EXPECT_EQ(f.selectFunctionsCliName(), "foo()");
}

TEST(GuiModelTypes, CompositeConfidenceWeights)
{
	RecoveryConfidence c{1.f, 0.f, 0.f, 0.f};
	EXPECT_FLOAT_EQ(c.composite(), 0.30f);
	EXPECT_EQ(hexAddress(0x401000), "0x401000");
}
