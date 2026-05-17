/**
 * @file tests/gui/launch_options_test.cpp
 */

#include "retdec/gui/launch_options.h"

#include <gtest/gtest.h>

#include <cstring>

TEST(LaunchOptions, StripsHeadlessFlag) {
    char a0[] = "retdec-gui";
    char a1[] = "--headless";
    char a2[] = "--version";
    char* argv[] = {a0, a1, a2};
    auto p = retdec::gui::parseLaunchOptions(3, argv);
    EXPECT_TRUE(p.headless);
    ASSERT_EQ(p.argc, 2);
    EXPECT_STREQ(p.argStorage[0].c_str(), "retdec-gui");
    EXPECT_STREQ(p.argStorage[1].c_str(), "--version");
}

TEST(LaunchOptions, HeadlessExitMs) {
    char a0[] = "prog";
    char a1[] = "--headless-exit-ms";
    char a2[] = "42";
    char* argv[] = {a0, a1, a2};
    auto p = retdec::gui::parseLaunchOptions(3, argv);
    EXPECT_TRUE(p.headless);
    EXPECT_EQ(p.headlessExitMs, 42);
    ASSERT_EQ(p.argc, 1);
}

TEST(LaunchOptions, HeadlessExitMsEqualsForm) {
    char a0[] = "prog";
    char a1[] = "--headless-exit-ms=99";
    char* argv[] = {a0, a1};
    auto p = retdec::gui::parseLaunchOptions(2, argv);
    EXPECT_TRUE(p.headless);
    EXPECT_EQ(p.headlessExitMs, 99);
}
