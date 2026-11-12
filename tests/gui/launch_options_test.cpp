/**
 * @file tests/gui/launch_options_test.cpp
 */

#include "retdec/gui/model/launch_options.h"

#include <forge/os/os.hpp>

#include <gtest/gtest.h>

#include <cstring>

using retdec::gui::model::parseLaunchOptions;

namespace {

/// Clears RETDEC_GUI_HEADLESS for the test and restores it afterwards.
class LaunchOptions : public ::testing::Test {
protected:
	void SetUp() override
	{
		had_ = forge::os::has_env("RETDEC_GUI_HEADLESS");
		old_ = forge::os::getenv("RETDEC_GUI_HEADLESS", "");
		forge::os::unsetenv("RETDEC_GUI_HEADLESS");
	}
	void TearDown() override
	{
		if (had_)
			forge::os::setenv("RETDEC_GUI_HEADLESS", old_);
		else
			forge::os::unsetenv("RETDEC_GUI_HEADLESS");
	}
	bool had_ = false;
	std::string old_;
};

} // namespace

TEST_F(LaunchOptions, StripsHeadlessFlag)
{
	char a0[] = "retdec-gui";
	char a1[] = "--headless";
	char a2[] = "--version";
	char* argv[] = {a0, a1, a2};
	auto p = parseLaunchOptions(3, argv);
	EXPECT_TRUE(p.headless);
	ASSERT_EQ(p.argc, 2);
	EXPECT_STREQ(p.argStorage[0].c_str(), "retdec-gui");
	EXPECT_STREQ(p.argStorage[1].c_str(), "--version");
	ASSERT_EQ(p.argvPtrs.size(), 3u);
	EXPECT_STREQ(p.argvPtrs[1], "--version");
	EXPECT_EQ(p.argvPtrs[2], nullptr);
	EXPECT_FALSE(p.binaryPath.has_value());
}

TEST_F(LaunchOptions, HeadlessExitMs)
{
	char a0[] = "prog";
	char a1[] = "--headless-exit-ms";
	char a2[] = "42";
	char* argv[] = {a0, a1, a2};
	auto p = parseLaunchOptions(3, argv);
	EXPECT_TRUE(p.headless);
	EXPECT_EQ(p.headlessExitMs, 42);
	ASSERT_EQ(p.argc, 1);
}

TEST_F(LaunchOptions, HeadlessExitMsEqualsForm)
{
	char a0[] = "prog";
	char a1[] = "--headless-exit-ms=99";
	char* argv[] = {a0, a1};
	auto p = parseLaunchOptions(2, argv);
	EXPECT_TRUE(p.headless);
	EXPECT_EQ(p.headlessExitMs, 99);
}

TEST_F(LaunchOptions, HeadlessDecompileFastAndBinary)
{
	char a0[] = "prog";
	char a1[] = "--headless-decompile";
	char a2[] = "--fast";
	char a3[] = "/tmp/fib.elf";
	char* argv[] = {a0, a1, a2, a3};
	auto p = parseLaunchOptions(4, argv);
	EXPECT_TRUE(p.headless);
	EXPECT_TRUE(p.headlessDecompile);
	EXPECT_TRUE(p.fastDecompile);
	ASSERT_TRUE(p.binaryPath.has_value());
	EXPECT_EQ(*p.binaryPath, "/tmp/fib.elf");
	ASSERT_EQ(p.argc, 2);
	EXPECT_EQ(p.argStorage[1], "/tmp/fib.elf");
}

TEST_F(LaunchOptions, FastDecompileLongForm)
{
	char a0[] = "prog";
	char a1[] = "--fast-decompile";
	char* argv[] = {a0, a1};
	auto p = parseLaunchOptions(2, argv);
	EXPECT_TRUE(p.fastDecompile);
	EXPECT_FALSE(p.headless);
	EXPECT_EQ(p.argc, 1);
}

TEST_F(LaunchOptions, PositionalArgumentsKeepOrderAndSkipOptions)
{
	char a0[] = "prog";
	char a1[] = "--some-toolkit-flag";
	char a2[] = "project.retdec";
	char a3[] = "second";
	char* argv[] = {a0, a1, a2, a3};
	auto p = parseLaunchOptions(4, argv);
	ASSERT_EQ(p.positional.size(), 2u);
	EXPECT_EQ(p.positional[0], "project.retdec");
	EXPECT_EQ(p.positional[1], "second");
	EXPECT_EQ(p.binaryPath.value_or(""), "project.retdec");
	EXPECT_EQ(p.argc, 4);
}

TEST_F(LaunchOptions, EnvironmentRequestsHeadless)
{
	char a0[] = "prog";
	char* argv[] = {a0};
	for (const char* v: {"1", "true", "YES", "On"})
	{
		forge::os::setenv("RETDEC_GUI_HEADLESS", v);
		EXPECT_TRUE(parseLaunchOptions(1, argv).headless) << v;
	}
	for (const char* v: {"0", "no", "false", ""})
	{
		forge::os::setenv("RETDEC_GUI_HEADLESS", v);
		EXPECT_FALSE(parseLaunchOptions(1, argv).headless) << v;
	}
}

TEST_F(LaunchOptions, CopyRepointsArgv)
{
	char a0[] = "prog";
	char a1[] = "file.bin";
	char* argv[] = {a0, a1};
	auto p = parseLaunchOptions(2, argv);
	const auto copy = p;
	ASSERT_EQ(copy.argvPtrs.size(), 3u);
	EXPECT_EQ(copy.argvPtrs[0], copy.argStorage[0].data());
	EXPECT_EQ(copy.argvPtrs[1], copy.argStorage[1].data());
	EXPECT_NE(copy.argvPtrs[1], p.argvPtrs[1]);
}

TEST_F(LaunchOptions, NullArgvIsHarmless)
{
	auto p = parseLaunchOptions(0, nullptr);
	EXPECT_EQ(p.argc, 0);
	ASSERT_EQ(p.argvPtrs.size(), 1u);
	EXPECT_EQ(p.argvPtrs[0], nullptr);
}
