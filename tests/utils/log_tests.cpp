/**
 * @file tests/utils/log_tests.cpp
 * @brief The process-wide logger table must survive concurrent use.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Log::writers is a process-wide array read by Log::get and written by
 * Log::set, and neither took a lock. Log::info()/debug()/error() return a
 * Logger BY VALUE, copy-constructing it from the reference Log::get hands
 * back -- so a thread calling Log::set destroyed the object another thread was
 * halfway through copying:
 *
 *     ERROR: AddressSanitizer: heap-use-after-free  READ of size 8
 *       #0 retdec::utils::io::Logger::Logger(Logger const&) logger.cpp:65
 *       #1 retdec::utils::io::Log::info()                   log.cpp:57
 *     freed by thread T2: ... Log::set                      log.cpp:52
 *
 * That is reachable from the public API: retdec::decompile() calls
 * setLogsFrom(), which is two Log::set calls, and parallelBatchDecompile runs N
 * of them on one thread pool.
 *
 * These run under the sanitizers job in standalone-check.yml, which is where
 * the evidence above came from; without a sanitizer they are a smoke test that
 * the table still works at all.
 */

#include "retdec/utils/io/log.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace ::testing;

namespace retdec {
namespace utils {
namespace io {
namespace tests {

namespace {

/// Enough iterations to hit a window that is narrow but real, and few enough
/// that the suite stays quick under a sanitizer.
constexpr int kIterations = 40000;

} // namespace

TEST(LogTests, SettingAWriterWhileAnotherThreadReadsItIsSafe)
{
	std::atomic<int> reads{0};

	auto setter = []() {
		for (int i = 0; i < kIterations; ++i)
		{
			static thread_local std::ostringstream sink;
			Log::set(Log::Type::Info, Logger::Ptr(new Logger(sink, false)));
		}
	};
	auto reader = [&reads]() {
		for (int i = 0; i < kIterations; ++i)
		{
			Logger logger = Log::info();
			reads.fetch_add(1, std::memory_order_relaxed);
		}
	};

	std::vector<std::thread> threads;
	threads.emplace_back(setter);
	threads.emplace_back(setter);
	threads.emplace_back(reader);
	threads.emplace_back(reader);
	for (auto& t: threads)
		t.join();

	EXPECT_EQ(reads.load(), 2 * kIterations);
}

// The same for the other two shortcuts, which copy the same way.
TEST(LogTests, ErrorAndDebugAreSafeAgainstAConcurrentSet)
{
	std::atomic<int> reads{0};

	auto setter = [](Log::Type type) {
		for (int i = 0; i < kIterations; ++i)
		{
			static thread_local std::ostringstream sink;
			Log::set(type, Logger::Ptr(new Logger(sink, false)));
		}
	};
	auto reader = [&reads]() {
		for (int i = 0; i < kIterations; ++i)
		{
			Logger e = Log::error();
			Logger d = Log::debug();
			reads.fetch_add(1, std::memory_order_relaxed);
		}
	};

	std::thread a(setter, Log::Type::Error);
	std::thread b(setter, Log::Type::Debug);
	std::thread c(reader);
	a.join();
	b.join();
	c.join();

	EXPECT_EQ(reads.load(), kIterations);
}

// And the ordinary contract: a writer that was set is the one that is used.
TEST(LogTests, AWriterThatWasSetIsTheOneThatIsUsed)
{
	// verbose, or the logger drops what it is given by design -- which is what
	// the `false` in the concurrency tests above is for: they are about the
	// lifetime of the object, not about its output.
	std::ostringstream sink;
	Log::set(Log::Type::Info, Logger::Ptr(new Logger(sink, true)));
	Log::info() << "marker-value";
	EXPECT_NE(sink.str().find("marker-value"), std::string::npos) << sink.str();

	// Put the default back so the rest of the suite logs where it expects to.
	Log::set(Log::Type::Info, Logger::Ptr());
}


// ─── --log-file ──────────────────────────────────────────────────────────────
//
// setLogsFrom() puts a FileLogger into the table through Logger::Ptr, which is
// std::unique_ptr<Logger>, and Log::set turns that into a
// std::shared_ptr<Logger> -- adopting unique_ptr's deleter, which is
// `delete (Logger*)`. Logger's destructor was not virtual, so ~FileLogger never
// ran, its std::ofstream member was never destroyed, and nothing flushed it.
// The kernel closes the descriptor at exit; it does not know about a userspace
// stream buffer.

namespace {

/// A scratch path in the temp directory, unique to the test that asks for one.
std::filesystem::path scratchPath(const char* stem)
{
	static std::atomic<int> counter{0};
	return std::filesystem::temp_directory_path()
		 / ("retdec-log-" + std::string(stem) + "-" + std::to_string(counter.fetch_add(1)) + ".log");
}

std::string readAll(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(LogTests, AFileLoggerFlushesWhatItWroteWhenTheTableReleasesIt)
{
	const std::filesystem::path path = scratchPath("flush");
	std::error_code ec;
	std::filesystem::remove(path, ec);

	Log::set(Log::Type::Info, Logger::Ptr(new FileLogger(path.string(), true)));
	Log::info() << "phase: initialization";

	// What setLogsFrom does on the next decompile(), and what static
	// destruction does at exit: drop the writer. That has to be the point at
	// which the bytes reach the file.
	Log::set(Log::Type::Info, Logger::Ptr());

	const std::string content = readAll(path);
	EXPECT_NE(std::string::npos, content.find("phase: initialization"))
		<< "log file held " << content.size() << " byte(s)";

	std::filesystem::remove(path, ec);
}

// The same thing one level down, with no table involved: deleting a FileLogger
// through a Logger* has to run ~FileLogger. This is the destructor-is-virtual
// property on its own, so a regression is attributed to the right line.
TEST(LogTests, DeletingAFileLoggerThroughABasePointerClosesTheFile)
{
	const std::filesystem::path path = scratchPath("delete");
	std::error_code ec;
	std::filesystem::remove(path, ec);

	{
		Logger::Ptr log(new FileLogger(path.string(), true));
		*log << "written through the base";
	}

	const std::string content = readAll(path);
	EXPECT_NE(std::string::npos, content.find("written through the base"))
		<< "log file held " << content.size() << " byte(s)";

	std::filesystem::remove(path, ec);
}

// A FileLogger binds Logger::_out to its own std::ofstream member. Base classes
// are initialised before members, so the reference used to be bound to storage
// that held no object yet -- and the open() that made it usable happened in the
// derived constructor's body, after the base was already built. Writing through
// the base subobject during construction is the case that has to work.
TEST(LogTests, AFileLoggerIsUsableAsSoonAsItIsConstructed)
{
	const std::filesystem::path path = scratchPath("ctor");
	std::error_code ec;
	std::filesystem::remove(path, ec);

	{
		FileLogger log(path.string(), true);
		Logger& asBase = log;
		asBase << "usable immediately";
	}

	EXPECT_NE(std::string::npos, readAll(path).find("usable immediately"));
	std::filesystem::remove(path, ec);
}

// Opening a file that cannot be written still throws, and throws before the
// logger is handed to anyone.
TEST(LogTests, AFileLoggerThatCannotOpenItsFileThrows)
{
	const std::filesystem::path path = std::filesystem::temp_directory_path() / "retdec-log-no-such-dir" / "x.log";
	EXPECT_THROW(FileLogger(path.string(), true), std::runtime_error);
}

// ─── the stream outlives the Logger that was handed out ─────────────────────
//
// Holding a counted reference across the copy in Log::info() kept the SOURCE
// alive for the duration of the copy, but the copy shares the source's stream
// BY REFERENCE, and for a FileLogger that stream is a member of the source. So
// the caller of Log::info() was left writing through an ofstream a concurrent
// Log::set had already destroyed:
//
//     ERROR: AddressSanitizer: heap-use-after-free  READ of size 8
//       #0 std::endl<char, std::char_traits<char>>(std::ostream&)
//       #2 retdec::utils::io::Logger::operator<<   logger.h:125
//     freed by thread T0: ... FileLogger::~FileLogger()
//
// Reproduced in three runs out of three before the fix.

TEST(LogTests, AHandedOutLoggerKeepsItsFileAliveWhileItIsWrittenTo)
{
	const std::filesystem::path path = scratchPath("outlive");
	std::error_code ec;
	std::filesystem::remove(path, ec);

	Log::set(Log::Type::Info, Logger::Ptr(new FileLogger(path.string(), true)));

	std::atomic<bool> stop{false};
	std::atomic<int> writes{0};

	std::thread writer([&stop, &writes]() {
		while (!stop.load(std::memory_order_relaxed))
		{
			// The gap between taking the logger and writing through it is the
			// window; a real caller has one too, it is just shorter.
			Logger logger = Log::info();
			std::this_thread::yield();
			logger << "x" << std::endl;
			writes.fetch_add(1, std::memory_order_relaxed);
		}
	});

	// Wait for the writer to be actually running before the swap storm starts.
	// Without this the main thread could finish all 2000 swaps and set `stop`
	// before the writer was ever scheduled, and `writes` came back 0 -- which
	// this test then reported as a failure of the logger rather than of the
	// scheduler. Seen once on a four-CPU machine under parallel builds.
	while (writes.load(std::memory_order_relaxed) == 0)
	{
		std::this_thread::yield();
	}

	const std::filesystem::path other = scratchPath("outlive-other");
	for (int i = 0; i < 2000; ++i)
	{
		Log::set(Log::Type::Info, Logger::Ptr(new FileLogger(other.string(), true)));
	}
	stop.store(true, std::memory_order_relaxed);
	writer.join();

	// Guaranteed by the wait above; kept so the intent survives a refactor.
	EXPECT_GT(writes.load(), 0);

	// Put the default back before the scratch files go away.
	Log::set(Log::Type::Info, Logger::Ptr());
	std::filesystem::remove(path, ec);
	std::filesystem::remove(other, ec);
}

// The single-threaded shape of the same property, so a regression is visible
// without needing the race to be lost.
TEST(LogTests, AHandedOutLoggerSurvivesTheTableDroppingItsWriter)
{
	const std::filesystem::path path = scratchPath("survive");
	std::error_code ec;
	std::filesystem::remove(path, ec);

	Log::set(Log::Type::Info, Logger::Ptr(new FileLogger(path.string(), true)));

	Logger held = Log::info();
	Log::set(Log::Type::Info, Logger::Ptr()); // the table lets go
	held << "written after the table let go" << std::endl;

	// Destroying the last co-owner is what closes the file.
	{
		Logger discard = std::move(held);
		(void)discard;
	}

	const std::string content = readAll(path);
	EXPECT_NE(std::string::npos, content.find("written after the table let go"))
		<< "log file held " << content.size() << " byte(s)";

	std::filesystem::remove(path, ec);
}

} // namespace tests
} // namespace io
} // namespace utils
} // namespace retdec
