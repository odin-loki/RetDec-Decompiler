/**
 * @file src/gui/model/console_log.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/console_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <format>

namespace retdec::gui::model {

namespace {

bool containsCI(std::string_view hay, std::string_view needle)
{
	if (needle.size() > hay.size()) return false;
	for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
	{
		std::size_t k = 0;
		while (k < needle.size()
			   && std::tolower(static_cast<unsigned char>(hay[i + k])) == static_cast<unsigned char>(needle[k]))
			++k;
		if (k == needle.size()) return true;
	}
	return false;
}

} // namespace

ConsoleLineKind classifyConsoleLine(std::string_view text)
{
	if (text.empty()) return ConsoleLineKind::Plain;
	if (text.starts_with("==> ")) return ConsoleLineKind::Banner;
	if (text.starts_with("<== ")) return ConsoleLineKind::Footer;
	if (text.starts_with("[INFO]") || text.starts_with("[OK]")) return ConsoleLineKind::Plain;
	if (containsCI(text, "error") || containsCI(text, "fatal") || containsCI(text, "abort")) return ConsoleLineKind::Error;
	if (containsCI(text, "warn") || containsCI(text, "deprecated")) return ConsoleLineKind::Warning;
	return ConsoleLineKind::Plain;
}

std::string stripAnsi(std::string_view in)
{
	if (in.find('\x1b') == std::string_view::npos) return std::string(in);
	std::string out;
	out.reserve(in.size());
	for (std::size_t i = 0; i < in.size(); ++i)
	{
		const char c = in[i];
		if (c != '\x1b' || i + 1 >= in.size())
		{
			out += c;
			continue;
		}
		if (in[i + 1] == '[')
		{
			// CSI: ESC [ [0-9;?]* letter
			std::size_t j = i + 2;
			while (j < in.size() && (std::isdigit(static_cast<unsigned char>(in[j])) || in[j] == ';' || in[j] == '?'))
				++j;
			if (j < in.size() && std::isalpha(static_cast<unsigned char>(in[j])))
			{
				i = j;
				continue;
			}
			out += c;
			continue;
		}
		if (in[i + 1] == ']')
		{
			const std::size_t bel = in.find('\x07', i + 2);
			if (bel != std::string_view::npos)
			{
				i = bel;
				continue;
			}
		}
		out += c;
	}
	return out;
}

std::string sanitizeUtf8(std::string_view b)
{
	std::string out;
	out.reserve(b.size());
	static constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
	std::size_t i = 0;
	while (i < b.size())
	{
		const unsigned char c = static_cast<unsigned char>(b[i]);
		if (c < 0x80)
		{
			out += static_cast<char>(c);
			++i;
			continue;
		}
		int len = 0;
		uint32_t cp = 0;
		if ((c & 0xE0) == 0xC0)
		{
			len = 2;
			cp = c & 0x1F;
		}
		else if ((c & 0xF0) == 0xE0)
		{
			len = 3;
			cp = c & 0x0F;
		}
		else if ((c & 0xF8) == 0xF0)
		{
			len = 4;
			cp = c & 0x07;
		}
		bool ok = len > 0 && i + static_cast<std::size_t>(len) <= b.size();
		for (int k = 1; ok && k < len; ++k)
		{
			const unsigned char cc = static_cast<unsigned char>(b[i + static_cast<std::size_t>(k)]);
			if ((cc & 0xC0) != 0x80) ok = false;
			else cp = (cp << 6) | (cc & 0x3F);
		}
		if (ok)
		{
			// Overlong forms, surrogates and out-of-range code points are invalid.
			const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
			if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) ok = false;
		}
		if (!ok)
		{
			out += kReplacement;
			++i;
			continue;
		}
		out.append(b.substr(i, static_cast<std::size_t>(len)));
		i += static_cast<std::size_t>(len);
	}
	return out;
}

std::string isoTimestampNow()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t t = std::chrono::system_clock::to_time_t(now);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	std::tm tm{};
#if defined(_WIN32)
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
					   tm.tm_min, tm.tm_sec, static_cast<int>(ms));
}

std::string formatCommandLine(std::string_view tool, const std::vector<std::string>& args)
{
	std::string cmd(tool);
	for (const std::string& a: args)
	{
		cmd += ' ';
		if (a.find(' ') != std::string::npos || a.find('\t') != std::string::npos)
		{
			cmd += '"';
			for (char c: a)
			{
				if (c == '"') cmd += '\\';
				cmd += c;
			}
			cmd += '"';
		}
		else
			cmd += a;
	}
	return cmd;
}

std::vector<std::string> formatBanner(std::string_view tool, const std::vector<std::string>& args, std::string_view cwd,
                                      std::string_view timestamp)
{
	std::vector<std::string> out;
	out.push_back(std::format("==> [{}] {}", timestamp, formatCommandLine(tool, args)));
	if (!cwd.empty()) out.push_back(std::format("==>   cwd: {}", cwd));
	return out;
}

std::string formatFooter(std::string_view tool, int exitCode, int64_t elapsedMs)
{
	return std::format("<== {} finished (exit={}, time={} ms)", tool, exitCode, elapsedMs);
}

std::string formatCommandLogEntry(std::string_view tool, const std::vector<std::string>& args, std::string_view cwd,
                                  int exitCode, int64_t elapsedMs, std::string_view outputTail, std::string_view timestamp)
{
	std::string s = std::format("[{}] {}\n  cwd: {}\n  exit: {}  time: {} ms\n  cmd:", timestamp, tool, cwd, exitCode, elapsedMs);
	for (const std::string& a: args)
	{
		s += ' ';
		s += a;
	}
	s += '\n';
	const bool tailBlank = std::all_of(outputTail.begin(), outputTail.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
	if (!tailBlank)
	{
		s += "  --- output (tail) ---\n";
		s += outputTail;
		s += '\n';
	}
	s += '\n';
	return s;
}

// ── ConsoleBuffer ────────────────────────────────────────────────────────────

ConsoleBuffer::ConsoleBuffer(std::size_t maxLines): maxLines_(std::max<std::size_t>(1, maxLines))
{
	lines_.emplace_back();
	kinds_.push_back(ConsoleLineKind::Plain);
}

void ConsoleBuffer::append(std::string_view text)
{
	if (text.empty()) return;
	std::size_t at = 0;
	for (;;)
	{
		const std::size_t nl = text.find('\n', at);
		std::string_view piece = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
		if (!piece.empty() && piece.back() == '\r') piece.remove_suffix(1);
		if (!piece.empty())
		{
			lines_.back().append(piece);
			kinds_.back() = classifyConsoleLine(lines_.back());
		}
		if (nl == std::string_view::npos) break;
		lines_.emplace_back();
		kinds_.push_back(ConsoleLineKind::Plain);
		at = nl + 1;
	}
	trim();
	++revision_;
}

void ConsoleBuffer::appendLine(std::string_view line)
{
	append(line);
	if (line.empty() || line.back() != '\n') append("\n");
}

void ConsoleBuffer::appendParagraph(std::string_view text)
{
	if (!isEmpty()) append("\n");
	append(text);
	if (text.empty()) ++revision_;
}

void ConsoleBuffer::clear()
{
	lines_.clear();
	kinds_.clear();
	lines_.emplace_back();
	kinds_.push_back(ConsoleLineKind::Plain);
	++revision_;
}

void ConsoleBuffer::setMaxLines(std::size_t n)
{
	maxLines_ = std::max<std::size_t>(1, n);
	trim();
	++revision_;
}

void ConsoleBuffer::trim()
{
	while (lines_.size() > maxLines_)
	{
		lines_.pop_front();
		kinds_.pop_front();
		++dropped_;
	}
}

std::string ConsoleBuffer::text() const
{
	std::string r;
	std::size_t n = 0;
	for (const auto& l: lines_)
		n += l.size() + 1;
	r.reserve(n);
	for (std::size_t i = 0; i < lines_.size(); ++i)
	{
		if (i) r += '\n';
		r += lines_[i];
	}
	return r;
}

// ── Capped draining ─────────────────────────────────────────────────────────

std::string takeCapped(std::string& src, std::size_t cap)
{
	cap = std::max<std::size_t>(1, cap);
	if (src.size() <= cap)
	{
		std::string all;
		all.swap(src);
		return all;
	}
	std::size_t cut = cap;
	const std::size_t lastNl = src.rfind('\n', cap - 1);
	if (lastNl != std::string::npos && lastNl > cap / 2)
		cut = lastNl + 1;
	else
	{
		while (cut > 0 && (static_cast<unsigned char>(src[cut]) & 0xC0) == 0x80)
			--cut;
		if (cut == 0) cut = cap;
	}
	std::string head = src.substr(0, cut);
	src.erase(0, cut);
	return head;
}

uint64_t ConsoleFeed::attach(std::string label)
{
	std::lock_guard lock(mutex_);
	const uint64_t id = next_++;
	sources_[id].label = label.empty() ? std::string("subprocess") : std::move(label);
	return id;
}

std::vector<ConsoleFeed::Chunk> ConsoleFeed::detach(uint64_t source)
{
	std::lock_guard lock(mutex_);
	std::vector<Chunk> out;
	const auto it = sources_.find(source);
	if (it == sources_.end()) return out;
	if (!it->second.out.empty()) out.push_back({source, ConsoleStream::Stdout, std::move(it->second.out)});
	if (!it->second.err.empty()) out.push_back({source, ConsoleStream::Stderr, std::move(it->second.err)});
	sources_.erase(it);
	return out;
}

void ConsoleFeed::push(uint64_t source, ConsoleStream stream, std::string_view bytes)
{
	if (bytes.empty()) return;
	std::lock_guard lock(mutex_);
	const auto it = sources_.find(source);
	if (it == sources_.end()) return;
	(stream == ConsoleStream::Stdout ? it->second.out : it->second.err).append(bytes);
}

std::vector<ConsoleFeed::Chunk> ConsoleFeed::drain(std::size_t cap, bool& more)
{
	std::lock_guard lock(mutex_);
	std::vector<Chunk> out;
	more = false;
	for (auto& [id, s]: sources_)
	{
		if (!s.out.empty())
		{
			out.push_back({id, ConsoleStream::Stdout, takeCapped(s.out, cap)});
			more = more || !s.out.empty();
		}
		if (!s.err.empty())
		{
			out.push_back({id, ConsoleStream::Stderr, takeCapped(s.err, cap)});
			more = more || !s.err.empty();
		}
	}
	return out;
}

void ConsoleFeed::discardPending()
{
	std::lock_guard lock(mutex_);
	for (auto& [id, s]: sources_)
	{
		s.out.clear();
		s.err.clear();
	}
}

bool ConsoleFeed::attached(uint64_t source) const
{
	std::lock_guard lock(mutex_);
	return sources_.count(source) != 0;
}

std::string ConsoleFeed::label(uint64_t source) const
{
	std::lock_guard lock(mutex_);
	const auto it = sources_.find(source);
	return it == sources_.end() ? std::string() : it->second.label;
}

std::size_t ConsoleFeed::sourceCount() const
{
	std::lock_guard lock(mutex_);
	return sources_.size();
}

std::size_t ConsoleFeed::pendingBytes() const
{
	std::lock_guard lock(mutex_);
	std::size_t n = 0;
	for (const auto& [id, s]: sources_)
		n += s.out.size() + s.err.size();
	return n;
}

} // namespace retdec::gui::model
