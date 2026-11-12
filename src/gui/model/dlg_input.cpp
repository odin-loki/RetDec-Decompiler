/**
 * @file src/gui/model/dlg_input.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/dlg_input.h"

#include <format>
#include <limits>

namespace retdec::gui::model {

std::string_view dlgTrim(std::string_view s)
{
	auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
	while (!s.empty() && ws(s.front())) s.remove_prefix(1);
	while (!s.empty() && ws(s.back())) s.remove_suffix(1);
	return s;
}

std::optional<uint64_t> dlgParseUnsigned(std::string_view text, int base)
{
	text = dlgTrim(text);
	if (base < 2 || base > 36) return std::nullopt;
	if (!text.empty() && text.front() == '+') text.remove_prefix(1);
	if (text.empty()) return std::nullopt;
	uint64_t v = 0;
	for (char c: text)
	{
		int d = -1;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
		if (d < 0 || d >= base) return std::nullopt;
		if (v > (std::numeric_limits<uint64_t>::max() - uint64_t(d)) / uint64_t(base)) return std::nullopt;
		v = v * uint64_t(base) + uint64_t(d);
	}
	return v;
}

std::optional<uint64_t> dlgParseUnsignedAuto(std::string_view text)
{
	text = dlgTrim(text);
	std::string_view body = text;
	if (!body.empty() && body.front() == '+') body.remove_prefix(1);
	if (body.size() >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
		return dlgParseUnsigned(body.substr(2), 16);
	if (body.size() >= 2 && body[0] == '0') return dlgParseUnsigned(body.substr(1), 8);
	return dlgParseUnsigned(body, 10);
}

std::optional<int> dlgParseInt(std::string_view text)
{
	text = dlgTrim(text);
	bool neg = false;
	if (!text.empty() && (text.front() == '+' || text.front() == '-'))
	{
		neg = text.front() == '-';
		text.remove_prefix(1);
	}
	if (text.empty() || text.front() == '+' || text.front() == '-') return std::nullopt;
	const auto u = dlgParseUnsigned(text, 10);
	if (!u) return std::nullopt;
	const uint64_t lim = neg ? uint64_t(std::numeric_limits<int>::max()) + 1 : uint64_t(std::numeric_limits<int>::max());
	if (*u > lim) return std::nullopt;
	return neg ? int(-int64_t(*u)) : int(*u);
}

std::expected<uint64_t, std::string> dlgParseGotoAddress(std::string_view text)
{
	if (auto v = dlgParseUnsignedAuto(text)) return *v;
	return std::unexpected(std::string("Not a valid address"));
}

std::expected<int, std::string> dlgParseGotoLine(std::string_view text)
{
	const auto v = dlgParseInt(text);
	if (!v || *v < 1) return std::unexpected(std::string("Not a valid line number"));
	return *v;
}

std::expected<uint64_t, std::string> dlgParseEntryPoint(std::string_view text)
{
	const std::string_view t = dlgTrim(text);
	if (t.empty()) return uint64_t{0};
	std::optional<uint64_t> v;
	if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) v = dlgParseUnsigned(t.substr(2), 16);
	else v = dlgParseUnsigned(t, 10);
	if (!v) return std::unexpected(std::string("Invalid entry point (use 0x… hex or decimal)."));
	return *v;
}

std::string dlgFormatEntryPoint(uint64_t entryPoint)
{
	return entryPoint ? std::format("0x{:x}", entryPoint) : std::string();
}

std::expected<uint64_t, std::string> dlgParseMaxMemory(std::string_view text)
{
	const std::string_view t = dlgTrim(text);
	const auto v = dlgParseUnsigned(t, 10);
	if (!t.empty() && t != "0" && !v) return std::unexpected(std::string("Not a valid byte count."));
	return v.value_or(0);
}

std::string dlgFormatMaxMemory(uint64_t bytes)
{
	return bytes > 0 ? std::to_string(bytes) : std::string("0");
}

std::string dlgMaxMemoryStatus(uint64_t bytes)
{
	return bytes > 0 ? std::format("Next decompile will pass --max-memory {}", bytes)
					 : std::string("Next decompile will omit --max-memory");
}

} // namespace retdec::gui::model
