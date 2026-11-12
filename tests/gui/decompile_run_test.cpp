/**
 * @file tests/gui/decompile_run_test.cpp
 * @brief DecompileRun with trivial children (/bin/sh) instead of retdec-decompiler.
 */

#include "retdec/gui/model/decompile_run.h"

#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <sys/types.h>
#endif

namespace m = retdec::gui::model;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct TempDir
{
	fs::path path;
	TempDir()
	{
		path = fs::path(forge::os::fs::make_temp_dir("retdec-gui-run-").value);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::string readAll(const fs::path& p)
{
	auto r = forge::os::read_file(p.string());
	return r ? r.value : std::string{};
}

class DecompileRunTest : public ::testing::Test {
protected:
	void SetUp() override
	{
#if defined(_WIN32)
		GTEST_SKIP() << "uses /bin/sh";
#else
		if (!fs::exists("/bin/sh")) GTEST_SKIP() << "no /bin/sh";
#endif
	}

	static m::DecompileRunSpec sh(const std::string& script)
	{
		m::DecompileRunSpec s;
		s.executable = "/bin/sh";
		s.arguments = {"-c", script};
		return s;
	}
};

} // namespace

TEST_F(DecompileRunTest, LogsStdoutAndStderrAndReportsExitCode)
{
	m::DecompileRun run;
	EXPECT_EQ(run.state(), m::DecompileRun::State::NotStarted);
	EXPECT_FALSE(run.poll());
	ASSERT_TRUE(run.start(sh("echo out; echo err 1>&2; exit 3")).has_value());
	EXPECT_TRUE(run.running());
	ASSERT_TRUE(run.waitFor(20s));
	EXPECT_EQ(run.state(), m::DecompileRun::State::Finished);
	ASSERT_TRUE(run.exitCode().has_value());
	EXPECT_EQ(*run.exitCode(), 3);
	EXPECT_EQ(run.termSignal(), 0);
	EXPECT_FALSE(run.timedOut());
	EXPECT_TRUE(run.errorString().empty());
	EXPECT_EQ(readAll(run.logPath()), "out\nerr\n");
	EXPECT_EQ(run.executable(), fs::path("/bin/sh"));
	EXPECT_EQ(run.workingDirectory(), fs::path("/bin"));
	ASSERT_EQ(run.arguments().size(), 2u);
	// poll() keeps reporting the finished run.
	EXPECT_TRUE(run.poll());
}

TEST_F(DecompileRunTest, PollIsNonBlocking)
{
	m::DecompileRun run;
	ASSERT_TRUE(run.start(sh("sleep 0.4; echo done")).has_value());
	const auto t0 = std::chrono::steady_clock::now();
	EXPECT_FALSE(run.poll());
	EXPECT_LT(std::chrono::steady_clock::now() - t0, 200ms);
	while (!run.poll())
		std::this_thread::sleep_for(10ms);
	EXPECT_EQ(run.exitCode().value_or(-1), 0);
	EXPECT_GE(run.elapsed(), 300ms);
	const auto frozen = run.elapsed();
	std::this_thread::sleep_for(20ms);
	EXPECT_EQ(run.elapsed(), frozen);
	EXPECT_EQ(readAll(run.logPath()), "done\n");
}

TEST_F(DecompileRunTest, LogGrowsWhileRunningAndFeedsProgress)
{
	m::DecompileRun run;
	ASSERT_TRUE(run.start(sh("echo 'Running phase: Initialization ( 0.01s )'; sleep 0.5; "
							 "echo 'Running phase: Class hierarchy analysis ( 0.50s )'"))
					.has_value());
	uint64_t offset = 0;
	std::optional<m::DecompileLogProgress> first;
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (!first && std::chrono::steady_clock::now() < deadline)
	{
		first = m::pollDecompileLogProgress(run.logPath(), &offset);
		if (!first) std::this_thread::sleep_for(10ms);
	}
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->stage, "Binary loading");
	ASSERT_TRUE(run.waitFor(20s));
	const auto second = m::pollDecompileLogProgress(run.logPath(), &offset);
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->stage, "RTTI reconstruction");
}

TEST_F(DecompileRunTest, EnvironmentAndWorkingDirectory)
{
	TempDir tmp;
	auto spec = sh("echo \"$RETDEC_TEST_VAR\"; pwd");
	spec.environment["RETDEC_TEST_VAR"] = "hello from the gui";
	spec.workingDirectory = tmp.path;
	spec.logPath = tmp.path / "explicit.log";
	{
		m::DecompileRun run;
		ASSERT_TRUE(run.start(spec).has_value());
		ASSERT_TRUE(run.waitFor(20s));
		EXPECT_EQ(run.logPath(), tmp.path / "explicit.log");
		const std::string log = readAll(run.logPath());
		EXPECT_EQ(log, "hello from the gui\n" + fs::canonical(tmp.path).string() + "\n");
	}
	// A log the caller named is not deleted with the run.
	EXPECT_TRUE(fs::exists(tmp.path / "explicit.log"));
}

TEST_F(DecompileRunTest, PrivateTempLogIsRemovedWithTheRun)
{
	fs::path log;
	{
		m::DecompileRun run;
		ASSERT_TRUE(run.start(sh("echo x")).has_value());
		ASSERT_TRUE(run.waitFor(20s));
		log = run.logPath();
		EXPECT_TRUE(fs::exists(log));
	}
	EXPECT_FALSE(fs::exists(log));
}

TEST_F(DecompileRunTest, CancelKillsTheProcessTree)
{
	TempDir tmp;
	const fs::path pidFile = tmp.path / "child.pid";
	m::DecompileRun run;
	ASSERT_TRUE(run.start(sh("sleep 30 & echo $! > '" + pidFile.string() + "'; wait")).has_value());
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (readAll(pidFile).empty() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(10ms);
	const std::string pidText = readAll(pidFile);
	ASSERT_FALSE(pidText.empty());

	const auto t0 = std::chrono::steady_clock::now();
	run.cancel();
	EXPECT_LT(std::chrono::steady_clock::now() - t0, 5s);
	EXPECT_EQ(run.state(), m::DecompileRun::State::Cancelled);
	EXPECT_FALSE(run.running());
	EXPECT_TRUE(run.poll());
	EXPECT_LT(run.elapsed(), 10s);

#if defined(__linux__)
	// The grandchild (background sleep) went with it: gone or a zombie awaiting its reaper.
	const fs::path stat = "/proc/" + std::to_string(std::stoi(pidText)) + "/stat";
	bool alive = false;
	for (int i = 0; i < 200; ++i)
	{
		const std::string s = readAll(stat);
		const auto close = s.rfind(')');
		alive = !s.empty() && close != std::string::npos && close + 2 < s.size() && s[close + 2] != 'Z';
		if (!alive) break;
		std::this_thread::sleep_for(10ms);
	}
	EXPECT_FALSE(alive) << "grandchild " << pidText << " survived cancel()";
#endif
	run.cancel(); // no-op once ended
	EXPECT_EQ(run.state(), m::DecompileRun::State::Cancelled);
}

TEST_F(DecompileRunTest, DestructorKillsARunningChild)
{
	const auto t0 = std::chrono::steady_clock::now();
	{
		m::DecompileRun run;
		ASSERT_TRUE(run.start(sh("sleep 30")).has_value());
	}
	EXPECT_LT(std::chrono::steady_clock::now() - t0, 5s);
}

TEST_F(DecompileRunTest, TimeoutKillsAndReports124)
{
	auto spec = sh("sleep 30");
	spec.timeoutMs = 200;
	m::DecompileRun run;
	ASSERT_TRUE(run.start(spec).has_value());
	ASSERT_TRUE(run.waitFor(20s));
	EXPECT_EQ(run.state(), m::DecompileRun::State::Finished);
	EXPECT_TRUE(run.timedOut());
	EXPECT_EQ(run.exitCode().value_or(-1), 124);
}

TEST_F(DecompileRunTest, KilledBySignal)
{
	m::DecompileRun run;
	ASSERT_TRUE(run.start(sh("kill -9 $$")).has_value());
	ASSERT_TRUE(run.waitFor(20s));
	EXPECT_EQ(run.state(), m::DecompileRun::State::Finished);
	EXPECT_EQ(run.exitCode().value_or(-1), 137);
	EXPECT_EQ(run.termSignal(), 9);
}

TEST_F(DecompileRunTest, StartFailures)
{
	m::DecompileRun run;
	m::DecompileRunSpec missing;
	missing.executable = "/nonexistent/retdec-decompiler";
	auto r = run.start(missing);
	ASSERT_FALSE(r.has_value());
	EXPECT_NE(r.error().find("retdec-decompiler"), std::string::npos);
	EXPECT_EQ(run.state(), m::DecompileRun::State::NotStarted);

	auto badCwd = sh("true");
	badCwd.workingDirectory = "/nonexistent/dir";
	EXPECT_FALSE(run.start(badCwd).has_value());

	auto badLog = sh("true");
	badLog.logPath = "/nonexistent/dir/run.log";
	ASSERT_TRUE(run.start(badLog).has_value()); // the OS reports it from the worker
	ASSERT_TRUE(run.waitFor(20s));
	EXPECT_EQ(run.state(), m::DecompileRun::State::FailedToStart);
	EXPECT_FALSE(run.errorString().empty());
	EXPECT_FALSE(run.exitCode().has_value());

	EXPECT_FALSE(run.start(m::DecompileRunSpec{}).has_value());
}

TEST_F(DecompileRunTest, OneRunAtATimeAndRestart)
{
	m::DecompileRun run;
	ASSERT_TRUE(run.start(sh("sleep 0.3")).has_value());
	EXPECT_FALSE(run.start(sh("true")).has_value());
	ASSERT_TRUE(run.waitFor(20s));
	const fs::path firstLog = run.logPath();

	ASSERT_TRUE(run.start(sh("echo second; exit 0")).has_value());
	EXPECT_FALSE(fs::exists(firstLog)); // the previous private log is gone
	ASSERT_TRUE(run.waitFor(20s));
	EXPECT_EQ(run.exitCode().value_or(-1), 0);
	EXPECT_EQ(readAll(run.logPath()), "second\n");
}
