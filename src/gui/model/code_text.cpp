/**
 * @file src/gui/model/code_text.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/code_text.h"

#include "retdec/gui/model/code_address.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>

namespace retdec::gui::model {

namespace {

char fold(char c)
{
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Position of @p q in @p s at or after @p from (forward) / starting at or
/// before @p from (backward); npos when none.
std::size_t findIn(std::string_view s, std::string_view q, std::size_t from, bool forward, bool cs)
{
	if (q.empty() || q.size() > s.size()) return std::string_view::npos;
	auto eq = [&](std::size_t at) {
		for (std::size_t k = 0; k < q.size(); ++k)
		{
			const char a = s[at + k], b = q[k];
			if (cs ? a != b : fold(a) != fold(b)) return false;
		}
		return true;
	};
	const std::size_t last = s.size() - q.size();
	if (forward)
	{
		for (std::size_t at = from; at <= last; ++at)
			if (eq(at)) return at;
		return std::string_view::npos;
	}
	if (from == std::string_view::npos) return std::string_view::npos;
	for (std::size_t at = std::min(from, last) + 1; at-- > 0;)
		if (eq(at)) return at;
	return std::string_view::npos;
}

bool isWordChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::optional<TextRange>
scanForward(const std::vector<std::string>& lines, std::string_view q, TextPos from, TextPos stop, bool cs)
{
	for (int l = std::max(0, from.line); l < static_cast<int>(lines.size()) && l <= stop.line; ++l)
	{
		const std::size_t start = l == from.line ? static_cast<std::size_t>(std::max(0, from.col)) : 0;
		const std::size_t at = findIn(lines[l], q, start, true, cs);
		if (at == std::string_view::npos) continue;
		const TextRange r{{l, static_cast<int>(at)}, {l, static_cast<int>(at + q.size())}};
		if (r.begin > stop) return std::nullopt;
		return r;
	}
	return std::nullopt;
}

std::optional<TextRange>
scanBackward(const std::vector<std::string>& lines, std::string_view q, TextPos before, TextPos stop, bool cs)
{
	// Matches must end at or before `before`.
	for (int l = std::min(before.line, static_cast<int>(lines.size()) - 1); l >= 0 && l >= stop.line; --l)
	{
		const std::string& s = lines[l];
		std::size_t limit;
		if (l == before.line)
		{
			if (before.col < static_cast<int>(q.size())) continue;
			limit = static_cast<std::size_t>(before.col) - q.size();
		}
		else
		{
			if (s.size() < q.size()) continue;
			limit = s.size() - q.size();
		}
		const std::size_t at = findIn(s, q, limit, false, cs);
		if (at == std::string_view::npos) continue;
		const TextRange r{{l, static_cast<int>(at)}, {l, static_cast<int>(at + q.size())}};
		if (r.begin < stop) return std::nullopt;
		return r;
	}
	return std::nullopt;
}

} // namespace

std::vector<std::string> splitLines(std::string_view text)
{
	std::vector<std::string> out;
	std::size_t at = 0;
	for (;;)
	{
		const std::size_t nl = text.find('\n', at);
		std::string_view piece = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
		if (!piece.empty() && piece.back() == '\r') piece.remove_suffix(1);
		out.emplace_back(piece);
		if (nl == std::string_view::npos) break;
		at = nl + 1;
	}
	return out;
}

std::string joinLines(const std::vector<std::string>& lines)
{
	std::size_t n = 0;
	for (const auto& l: lines)
		n += l.size() + 1;
	std::string r;
	r.reserve(n);
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		if (i) r += '\n';
		r += lines[i];
	}
	return r;
}

std::string textInRange(const std::vector<std::string>& lines, const TextRange& range)
{
	TextPos a = std::min(range.begin, range.end), b = std::max(range.begin, range.end);
	if (lines.empty() || a == b) return {};
	a.line = std::clamp(a.line, 0, static_cast<int>(lines.size()) - 1);
	b.line = std::clamp(b.line, 0, static_cast<int>(lines.size()) - 1);
	auto clampCol = [&](const TextPos& p) {
		return static_cast<std::size_t>(std::clamp(p.col, 0, static_cast<int>(lines[p.line].size())));
	};
	if (a.line == b.line) return lines[a.line].substr(clampCol(a), clampCol(b) - clampCol(a));
	std::string r = lines[a.line].substr(clampCol(a));
	for (int i = a.line + 1; i < b.line; ++i)
	{
		r += '\n';
		r += lines[i];
	}
	r += '\n';
	r += lines[b.line].substr(0, clampCol(b));
	return r;
}

std::optional<TextRange>
findText(const std::vector<std::string>& lines, std::string_view query, const TextRange& from, const FindOptions& opt)
{
	if (query.empty() || lines.empty()) return std::nullopt;
	const TextPos begin = std::min(from.begin, from.end), end = std::max(from.begin, from.end);
	const TextPos docStart{0, 0};
	const TextPos docEnd{static_cast<int>(lines.size()) - 1, static_cast<int>(lines.back().size())};
	if (opt.forward)
	{
		if (auto r = scanForward(lines, query, end, docEnd, opt.caseSensitive)) return r;
		if (opt.wrap) return scanForward(lines, query, docStart, docEnd, opt.caseSensitive);
		return std::nullopt;
	}
	if (auto r = scanBackward(lines, query, begin, docStart, opt.caseSensitive)) return r;
	if (opt.wrap) return scanBackward(lines, query, docEnd, docStart, opt.caseSensitive);
	return std::nullopt;
}

std::size_t countMatches(const std::vector<std::string>& lines, std::string_view query, bool caseSensitive)
{
	if (query.empty()) return 0;
	std::size_t n = 0;
	for (const auto& l: lines)
	{
		std::size_t at = 0;
		while ((at = findIn(l, query, at, true, caseSensitive)) != std::string_view::npos)
		{
			++n;
			at += query.size();
		}
	}
	return n;
}

std::optional<int> findFunctionLine(const std::vector<std::string>& lines, std::string_view name)
{
	if (name.empty()) return std::nullopt;
	for (std::size_t li = 0; li < lines.size(); ++li)
	{
		const std::string& s = lines[li];
		std::size_t at = 0;
		while ((at = s.find(name, at)) != std::string::npos)
		{
			const std::size_t end = at + name.size();
			const bool leftOk = at == 0 || !isWordChar(s[at - 1]) || !isWordChar(name.front());
			const bool rightOk = end >= s.size() || !isWordChar(s[end]) || !isWordChar(name.back());
			if (leftOk && rightOk)
			{
				std::size_t k = end;
				while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k])))
					++k;
				if (k < s.size() && s[k] == '(') return static_cast<int>(li);
			}
			++at;
		}
	}
	return std::nullopt;
}

std::optional<TextPos> findFirst(const std::vector<std::string>& lines, std::string_view needle)
{
	if (needle.empty()) return std::nullopt;
	for (std::size_t li = 0; li < lines.size(); ++li)
	{
		const std::size_t at = lines[li].find(needle);
		if (at != std::string::npos) return TextPos{static_cast<int>(li), static_cast<int>(at)};
	}
	return std::nullopt;
}

std::optional<TextPos> locateFunction(const std::vector<std::string>& lines, uint64_t address, std::string_view name)
{
	const std::string needles[] = {
		std::string(name),
		std::format("function_{:x}", address),
		std::format("0x{:x}", address),
	};
	for (const std::string& n: needles)
		if (auto p = findFirst(lines, n)) return p;
	return std::nullopt;
}

std::optional<int> findAddressLine(const std::vector<std::string>& lines, uint64_t address)
{
	const std::string needle = std::format("0x{:x}", address);
	for (std::size_t i = 0; i < lines.size(); ++i)
		if (findIn(lines[i], needle, 0, true, false) != std::string_view::npos) return static_cast<int>(i);
	return std::nullopt;
}

std::optional<int> findListingLine(const std::vector<std::string>& lines, uint64_t address)
{
	for (std::size_t i = 0; i < lines.size(); ++i)
		if (listingAddress(lines[i]) == address) return static_cast<int>(i);
	return findAddressLine(lines, address);
}

std::expected<void, std::string> saveTextFile(const std::filesystem::path& path, std::string_view text)
{
	std::FILE* f = std::fopen(path.string().c_str(), "wb");
	if (!f) return std::unexpected(std::format("cannot open {} for writing", path.string()));
	const std::size_t written = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), f);
	const bool flushed = std::fflush(f) == 0;
	const bool err = std::ferror(f) != 0;
	const bool closed = std::fclose(f) == 0;
	if (written != text.size() || !flushed || err || !closed)
		return std::unexpected(std::format("write to {} failed", path.string()));
	return {};
}

std::expected<std::string, std::string> loadTextFile(const std::filesystem::path& path)
{
	std::FILE* f = std::fopen(path.string().c_str(), "rb");
	if (!f) return std::unexpected(std::format("cannot open {}", path.string()));
	std::string out;
	char buf[65536];
	std::size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
		out.append(buf, n);
	const bool err = std::ferror(f) != 0;
	std::fclose(f);
	if (err) return std::unexpected(std::format("read of {} failed", path.string()));
	return out;
}

} // namespace retdec::gui::model
