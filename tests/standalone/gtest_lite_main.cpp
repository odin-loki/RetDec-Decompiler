/**
 * @file tests/standalone/gtest_lite_main.cpp
 * @brief Default `main` for the dependency-free GoogleTest shim.
 *
 * Compiled into a static archive so the linker pulls it in only when the test
 * translation unit does not define its own `main` — the same behaviour real
 * GoogleTest gets from `gtest_main`.
 */

#include "gtest/gtest.h"

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
