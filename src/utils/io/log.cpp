/**
 * @file src/utils/io/logger.cpp
 * @brief Provides unified logging interface.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cassert>
#include <memory>
#include <mutex>

#include "retdec/utils/io/log.h"

namespace retdec {
namespace utils {
namespace io {

// Initialization of shortcuts
const Log::Action Log::Error = Log::Action::Error;
const Log::Action Log::Warning = Log::Action::Warning;
const Log::Action Log::Phase = Log::Action::Phase;
const Log::Action Log::SubPhase = Log::Action::SubPhase;
const Log::Action Log::SubSubPhase = Log::Action::SubSubPhase;
const Log::Action Log::ElapsedTime = Log::Action::ElapsedTime;

std::shared_ptr<Logger> Log::writers[] = {
	/*Info*/ /*default*/ nullptr,
	/*Debug*/ /*default*/ nullptr,
	/*Error*/ std::make_shared<Logger>(std::cerr),
	/*Undefined*/ std::make_shared<Logger>(std::cout, false)};

namespace {

/// Guards Log::writers. The table is process-wide and every decompile() writes
/// to it through setLogsFrom(), so under parallelBatchDecompile one job's
/// Log::set raced another job's Log::info -- a data race on the array under
/// TSan, and a heap-use-after-free under ASan when the losing thread was
/// mid-copy of the Logger being replaced.
std::mutex& writerLock()
{
	static std::mutex m;
	return m;
}

} // anonymous namespace

std::shared_ptr<Logger> Log::getShared(const Log::Type& logType)
{
	assert(static_cast<int>(logType) <= static_cast<int>(Log::Type::Undefined));

	std::lock_guard<std::mutex> lock(writerLock());
	return writers[static_cast<int>(logType)];
}

Logger Log::defaultLogger(std::cout, true);

Logger& Log::get(const Log::Type& logType)
{
	// A raw reference into the table. Safe for the setup-time call sites that
	// use it and finish with it before anything else runs -- the four CLI tools
	// and unpacker/plugin.h -- and NOT safe against a concurrent Log::set, which
	// is why info()/debug()/error() below go through getShared() instead. Those
	// are the ones a decompilation actually calls.
	if (auto logger = getShared(logType)) return *logger;

	// Fallback usage of logger.
	return defaultLogger;
}

void Log::set(const Log::Type& lt, Logger::Ptr&& logger)
{
	// This can happen only after adding new Log::Type
	// after Log::Type::Undefined in Log::Type enum.
	assert(static_cast<int>(lt) <= static_cast<int>(Log::Type::Undefined));

	// The old writer is released when the last reader lets go of it, not here,
	// so a thread copying it inside Log::info() finishes against a live object.
	std::shared_ptr<Logger> next(std::move(logger));
	std::lock_guard<std::mutex> lock(writerLock());
	writers[static_cast<int>(lt)] = std::move(next);
}

Logger Log::info()
{
	// Through the shared_ptr, not through get(): the copy below is what raced,
	// and holding a counted reference across it is the fix.
	if (auto logger = getShared(Log::Type::Info)) return Logger(*logger);
	return Logger(defaultLogger);
}

void Log::phase(const std::string& phase, const Log::Action& action)
{
	Log::info() << action << phase << Log::ElapsedTime << std::endl;
}

Logger Log::debug()
{
	// See Log::info().
	if (auto logger = getShared(Log::Type::Debug)) return Logger(*logger);
	return Logger(defaultLogger);
}

Logger Log::error()
{
	// See Log::info().
	if (auto logger = getShared(Log::Type::Error)) return Logger(*logger);
	return Logger(defaultLogger);
}

} // namespace io
} // namespace utils
} // namespace retdec
