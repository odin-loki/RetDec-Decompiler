/**
 * @file include/retdec/utils/io/logger.h
 * @brief Implementation of a logging class.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_UTILS_IO_LOGGER_H
#define RETDEC_UTILS_IO_LOGGER_H

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace retdec {
namespace utils {
namespace io {

/**
 * @brief Provides Logger inteface that is used for logging events during decompilation.
 */
class Logger {
public:
	using Ptr = std::unique_ptr<Logger>;

public:
	enum Action : int
	{
		Phase,
		SubPhase,
		SubSubPhase,
		ElapsedTime,
		Error,
		Warning,
		NoAction
	};

	enum class Color : int
	{
		Red,
		Green,
		Blue,
		Yellow,
		DarkCyan,
		Default
	};

protected:
	typedef std::ostream& (*StreamManipulator)(std::ostream&);

public:
	Logger(std::ostream& stream, bool verbose = true);
	Logger(const Logger& logger);

	/// Copies @a logger and keeps @a streamOwner alive for as long as the copy
	/// lives.
	///
	/// A copy shares the original's stream by reference, and for a FileLogger
	/// that stream is a member of the original. Log::info() used to hand back
	/// such a copy and let its own counted reference go, so a concurrent
	/// Log::set() that dropped the last other reference destroyed the ofstream
	/// the caller was about to write through:
	///
	///   ERROR: AddressSanitizer: heap-use-after-free  READ of size 8
	///     #0 std::endl<char, std::char_traits<char>>(std::ostream&)
	///     #2 retdec::utils::io::Logger::operator<<   logger.h:125
	///   freed by thread T0: ... retdec::utils::io::FileLogger::~FileLogger()
	///
	/// Passing the owning handle in makes the copy a co-owner, so the stream
	/// outlives every Logger that refers to it.
	Logger(const Logger& logger, std::shared_ptr<const void> streamOwner);

	// Virtual, because FileLogger derives from this and Logger::Ptr is
	// std::unique_ptr<Logger>. Log::set turns that unique_ptr into a
	// std::shared_ptr<Logger>, which adopts unique_ptr's deleter --
	// `delete (Logger*)` -- so with a non-virtual destructor ~FileLogger never
	// ran, its std::ofstream member was never destroyed, and nothing flushed
	// it. The kernel closes the descriptor at exit; it knows nothing about a
	// userspace stream buffer, so --log-file came out EMPTY. Measured: 0 bytes.
	virtual ~Logger();

	template <typename T>
	Logger& operator<<(const T& p);

	Logger& operator<<(const StreamManipulator& manip);
	Logger& operator<<(const Action& ia);
	Logger& operator<<(const Color& lc);

private:
	bool isRedirected(const std::ostream& stream) const;

protected:
	std::ostream& _out;

	/// Non-null when _out belongs to an object this Logger shares ownership of.
	/// Type-erased because it owns the Logger that owns the stream, which is
	/// this very class.
	std::shared_ptr<const void> _streamOwner;

	bool _verbose = true;
	Color _currentBrush = Color::Default;

	bool _modifiedTerminalProperty = false;
	bool _terminalNotSupported = false;
};

namespace detail {

/// Holds a FileLogger's output stream.
///
/// It is a base rather than a member so that it is constructed BEFORE Logger:
/// bases are initialised in declaration order and members only after all of
/// them, so `FileLogger(...): Logger(_file, verbose)` used to bind Logger::_out
/// to storage that held no std::ofstream yet, and open() it afterwards from the
/// derived constructor's body. Nothing read through the reference in between,
/// which is the only reason that worked; it is not a property the base has to
/// keep. This way the stream is open before Logger sees it.
class FileLoggerStream {
protected:
	explicit FileLoggerStream(const std::string& file);

	std::ofstream _file;
};

} // namespace detail

class FileLogger : private detail::FileLoggerStream, public Logger {
public:
	FileLogger(const std::string& file, bool verbose = true);
};

template <typename T>
inline Logger& Logger::operator<<(const T& p)
{
	if (!_verbose) return *this;

	_out << p;

	return *this;
}

inline Logger& Logger::operator<<(const Logger::StreamManipulator& p)
{
	if (!_verbose) return *this;

	_out << p;

	return *this;
}

} // namespace io
} // namespace utils
} // namespace retdec

#endif
