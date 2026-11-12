/**
 * @file include/retdec/gui/model/console_log.h
 * @brief Text model of the live console and the command log: bounded line
 *        buffer with QTextDocument block semantics, log line classification
 *        (warning / error / banner colouring), ANSI stripping, UTF-8 repair,
 *        banner / footer / command-log formatting and the thread-safe pending
 *        output feed drained a capped number of bytes per UI tick.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CONSOLE_LOG_H
#define RETDEC_GUI_MODEL_CONSOLE_LOG_H

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

enum class ConsoleStream : uint8_t
{
	Stdout,
	Stderr,
};

/// How a console line is coloured.
enum class ConsoleLineKind : uint8_t
{
	Plain,   ///< default text colour ([INFO] / [OK] lines included)
	Warning, ///< contains "warn" / "deprecated" (case-insensitive)
	Error,   ///< contains "error" / "fatal" / "abort" (case-insensitive)
	Banner,  ///< "==> " tool start banner
	Footer,  ///< "<== " tool finished footer
};

/**
 * Classifies one line as the Qt console highlighter did: "==> " banner,
 * "<== " footer, "[INFO]" / "[OK]" plain, then error keywords before warning
 * keywords. Channels are merged, so classification is by content only.
 */
ConsoleLineKind classifyConsoleLine(std::string_view line);

/// Removes ANSI CSI ("ESC [ ... letter") and OSC ("ESC ] ... BEL") sequences.
std::string stripAnsi(std::string_view in);

/// Decodes permissively: invalid UTF-8 sequences become U+FFFD.
std::string sanitizeUtf8(std::string_view bytes);

/// Local time as "YYYY-MM-DDTHH:MM:SS.mmm" (Qt::ISODateWithMs).
std::string isoTimestampNow();

/// Command line as shown in banners: args separated by spaces, args with
/// whitespace double-quoted (inner quotes backslash-escaped).
std::string formatCommandLine(std::string_view tool, const std::vector<std::string>& args);

/// "==> [ts] tool args" and, when @p cwd is non-empty, "==>   cwd: <cwd>".
std::vector<std::string> formatBanner(
	std::string_view tool, const std::vector<std::string>& args, std::string_view cwd, std::string_view timestamp);

/// "<== tool finished (exit=N, time=M ms)".
std::string formatFooter(std::string_view tool, int exitCode, int64_t elapsedMs);

/// One command-log record (ends with an empty line).
std::string formatCommandLogEntry(
	std::string_view tool,
	const std::vector<std::string>& args,
	std::string_view cwd,
	int exitCode,
	int64_t elapsedMs,
	std::string_view outputTail,
	std::string_view timestamp);

/**
 * Append-only line buffer with QPlainTextEdit semantics: always at least one
 * (possibly empty) line, '\n' starts a new line, the oldest lines are dropped
 * beyond maxLines (setMaximumBlockCount). Each line keeps its classification.
 */
class ConsoleBuffer {
public:
	explicit ConsoleBuffer(std::size_t maxLines = 50'000);

	/// Appends already-decoded text (may contain several lines or a partial one).
	void append(std::string_view text);
	/// Appends a whole line: a '\n' is added when missing.
	void appendLine(std::string_view line);
	/// Appends a new paragraph (QPlainTextEdit::appendPlainText): starts a
	/// new line first unless the buffer is empty.
	void appendParagraph(std::string_view text);
	void clear();

	void setMaxLines(std::size_t n);
	std::size_t maxLines() const
	{
		return maxLines_;
	}

	bool isEmpty() const
	{
		return lines_.size() == 1 && lines_.front().empty();
	}
	std::size_t lineCount() const
	{
		return lines_.size();
	}
	const std::string& line(std::size_t i) const
	{
		return lines_[i];
	}
	ConsoleLineKind kind(std::size_t i) const
	{
		return kinds_[i];
	}
	std::string text() const;
	/// Total lines ever dropped from the top (views keep their scroll anchored).
	std::uint64_t droppedLines() const
	{
		return dropped_;
	}
	/// Bumped on every change.
	std::uint64_t revision() const
	{
		return revision_;
	}

private:
	void trim();
	std::deque<std::string> lines_;
	std::deque<ConsoleLineKind> kinds_;
	std::size_t maxLines_;
	std::uint64_t dropped_ = 0;
	std::uint64_t revision_ = 0;
};

/**
 * Takes at most @p cap bytes from the front of @p src: preferably up to and
 * including the last '\n' in the first cap bytes (when it lies past cap / 2),
 * otherwise cut on a UTF-8 boundary. Returns the head; @p src keeps the rest.
 */
std::string takeCapped(std::string& src, std::size_t cap);

/**
 * Thread-safe pending output of running tools. Producers (process reader
 * threads) push bytes for a source; the UI thread drains them at most
 * `cap` bytes per stream per source per tick, so a flood never blocks a frame.
 */
class ConsoleFeed {
public:
	struct Chunk
	{
		uint64_t source = 0;
		ConsoleStream stream = ConsoleStream::Stdout;
		std::string bytes;
	};

	/// Registers a source; @p label names it (e.g. "retdec-decompiler").
	uint64_t attach(std::string label);
	/// Removes a source and returns whatever it still had pending.
	std::vector<Chunk> detach(uint64_t source);
	/// Any thread. Unknown sources are ignored.
	void push(uint64_t source, ConsoleStream stream, std::string_view bytes);
	/// UI thread: takes up to @p cap bytes per stream per source. @p more is
	/// set when bytes remain pending afterwards.
	std::vector<Chunk> drain(std::size_t cap, bool& more);
	/// Drops all pending bytes (sources stay attached).
	void discardPending();
	bool attached(uint64_t source) const;
	std::string label(uint64_t source) const;
	std::size_t sourceCount() const;
	std::size_t pendingBytes() const;

private:
	struct Source
	{
		std::string label;
		std::string out;
		std::string err;
	};
	mutable std::mutex mutex_;
	std::map<uint64_t, Source> sources_;
	uint64_t next_ = 1;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CONSOLE_LOG_H
