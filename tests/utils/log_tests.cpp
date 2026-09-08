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
#include <sstream>
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

} // namespace tests
} // namespace io
} // namespace utils
} // namespace retdec
