/**
 * @file src/gui/model/address.cpp
 * @brief Address formatting / parsing.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/address.h"

#include <forge/os/clipboard.hpp>

#include <charconv>
#include <format>

namespace retdec::gui::model {

namespace {

bool isHexDigit(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view s)
{
	while (!s.empty() && isSpace(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && isSpace(s.back()))
		s.remove_suffix(1);
	return s;
}

std::optional<uint64_t> parseDigits(std::string_view digits, int base)
{
	if (digits.empty()) return std::nullopt;
	uint64_t v = 0;
	const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
	if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size()) return std::nullopt;
	return v;
}

} // namespace

std::string formatAddressHex(uint64_t address)
{
	return std::format("0x{:x}", address);
}

std::optional<uint64_t> parseFirstAddress(std::string_view line)
{
	for (std::size_t i = 0; i + 1 < line.size(); ++i)
	{
		if (line[i] != '0' || line[i + 1] != 'x') continue;
		std::size_t j = i + 2;
		while (j < line.size() && isHexDigit(line[j]))
			++j;
		if (j == i + 2) continue; // "0x" with no digits
		return parseDigits(line.substr(i + 2, j - i - 2), 16);
	}
	return std::nullopt;
}

std::optional<uint64_t> parseAddress(std::string_view text)
{
	const std::string_view t = trim(text);
	if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return parseDigits(t.substr(2), 16);
	return parseDigits(t, 10);
}

bool copyAddressToClipboard(uint64_t address)
{
	return static_cast<bool>(forge::os::clipboard::set(formatAddressHex(address)));
}

} // namespace retdec::gui::model
