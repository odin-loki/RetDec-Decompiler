/**
 * @file src/gui/model/code_address.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/code_address.h"

#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <vector>

namespace retdec::gui::model {

namespace {

bool isHex(char c)
{
	return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool isWordChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::optional<uint64_t> parseHexDigits(std::string_view digits)
{
	if (digits.empty()) return std::nullopt;
	uint64_t v = 0;
	const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v, 16);
	if (r.ec != std::errc() || r.ptr != digits.data() + digits.size()) return std::nullopt;
	return v;
}

struct HexToken
{
	std::size_t pos;
	std::size_t len;
	std::optional<uint64_t> value;
};

/// Every "0x<hex>" token in @p s from @p from.
std::vector<HexToken> hexTokens(std::string_view s, std::size_t from = 0)
{
	std::vector<HexToken> out;
	for (std::size_t i = from; i + 2 < s.size() + 1; ++i)
	{
		if (s[i] != '0' || i + 1 >= s.size() || s[i + 1] != 'x') continue;
		std::size_t e = i + 2;
		while (e < s.size() && isHex(s[e]))
			++e;
		if (e == i + 2) continue;
		out.push_back({i, e - i, parseHexDigits(s.substr(i + 2, e - i - 2))});
		i = e - 1;
	}
	return out;
}

constexpr std::array<std::string_view, 16> kBranches = {
	"call", "jmp", "je", "jne", "jz", "jnz", "ja", "jb", "jae", "jbe", "jg", "jl", "jge", "jle", "jo", "js"};

/// End offset of the first branch mnemonic (whole word, case-insensitive), npos if none.
std::size_t branchEnd(std::string_view line)
{
	std::size_t i = 0;
	while (i < line.size())
	{
		if (!isWordChar(line[i]))
		{
			++i;
			continue;
		}
		std::size_t e = i;
		while (e < line.size() && isWordChar(line[e]))
			++e;
		std::string w(line.substr(i, e - i));
		for (char& c: w)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		for (std::string_view b: kBranches)
			if (w == b) return e;
		i = e;
	}
	return std::string_view::npos;
}

} // namespace

std::optional<uint64_t> parseAddressInput(std::string_view text)
{
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
		text.remove_prefix(1);
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.remove_suffix(1);
	if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
	return parseHexDigits(text);
}

uint64_t parseHexU64(std::string_view text)
{
	return parseAddressInput(text).value_or(0);
}

bool isBranchLine(std::string_view line)
{
	return branchEnd(line) != std::string_view::npos;
}

std::optional<uint64_t> jumpTarget(std::string_view line)
{
	const std::size_t insnEnd = branchEnd(line);
	if (insnEnd == std::string_view::npos) return std::nullopt;
	const auto current = parseFirstAddress(line);
	const auto toks = hexTokens(line);
	std::optional<uint64_t> target;
	if (toks.size() >= 2)
		target = toks.back().value;
	else
		target = parseFirstAddress(line.substr(insnEnd));
	if (!target) return std::nullopt;
	if (current && *target == *current) return std::nullopt;
	return target;
}

} // namespace retdec::gui::model
