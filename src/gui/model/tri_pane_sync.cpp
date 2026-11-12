/**
 * @file src/gui/model/tri_pane_sync.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/tri_pane_sync.h"

#include "retdec/gui/model/code_address.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <unordered_map>

namespace retdec::gui::model {

// ── LineMapping ──────────────────────────────────────────────────────────────

void LineMapping::addEntry(int asmLine, int irLine, int cLine)
{
	const std::size_t idx = entries_.size();
	entries_.push_back({asmLine, irLine, cLine});
	if (asmLine >= 0) asmIndex_[asmLine] = idx;
	if (irLine >= 0) irIndex_[irLine] = idx;
	if (cLine >= 0) cIndex_[cLine] = idx;
}

void LineMapping::clear()
{
	entries_.clear();
	asmIndex_.clear();
	irIndex_.clear();
	cIndex_.clear();
}

namespace {
const LineMappingEntry* lookup(const std::map<int, std::size_t>& idx, const std::vector<LineMappingEntry>& e, int line)
{
	if (line < 0) return nullptr;
	const auto it = idx.find(line);
	return it == idx.end() ? nullptr : &e[it->second];
}

bool isHex(char c)
{
	return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

std::optional<uint64_t> parseNumber(std::string_view s)
{
	uint64_t v = 0;
	int base = 10;
	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		s.remove_prefix(2);
		base = 16;
	}
	const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
	if (r.ec != std::errc() || s.empty()) return std::nullopt;
	return v;
}

/// Address after the last "//" or ";" comment marker on the line.
std::optional<uint64_t> commentAddress(const std::string& line, std::string_view marker)
{
	const std::size_t c = line.find(marker);
	if (c == std::string::npos) return std::nullopt;
	return parseFirstAddress(std::string_view(line).substr(c));
}
} // namespace

const LineMappingEntry* LineMapping::byAsmLine(int line) const
{
	return lookup(asmIndex_, entries_, line);
}

const LineMappingEntry* LineMapping::byIrLine(int line) const
{
	return lookup(irIndex_, entries_, line);
}

const LineMappingEntry* LineMapping::byCLine(int line) const
{
	return lookup(cIndex_, entries_, line);
}

const LineMappingEntry* LineMapping::byLine(TriPane pane, int line) const
{
	switch (pane)
	{
	case TriPane::Asm: return byAsmLine(line);
	case TriPane::IR: return byIrLine(line);
	case TriPane::C: return byCLine(line);
	}
	return nullptr;
}

// ── Address extraction ──────────────────────────────────────────────────────

std::map<int, uint64_t> parseIrAddressMetadata(const std::vector<std::string>& irLines)
{
	std::map<int, uint64_t> out;
	for (const std::string& l: irLines)
	{
		std::string_view s(l);
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
			s.remove_prefix(1);
		if (s.size() < 2 || s[0] != '!' || !std::isdigit(static_cast<unsigned char>(s[1]))) continue;
		std::size_t e = 1;
		while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])))
			++e;
		int id = 0;
		std::from_chars(s.data() + 1, s.data() + e, id);
		const std::size_t brace = s.find("!{i64 ", e);
		if (brace == std::string_view::npos) continue;
		std::string_view num = s.substr(brace + 6);
		std::size_t ne = 0;
		while (ne < num.size() && (isHex(num[ne]) || num[ne] == 'x' || num[ne] == 'X'))
			++ne;
		if (auto v = parseNumber(num.substr(0, ne))) out[id] = *v;
	}
	return out;
}

std::optional<uint64_t> lineAddress(TriPane pane, const std::string& line, const std::map<int, uint64_t>* irMetadata)
{
	switch (pane)
	{
	case TriPane::Asm: {
		std::string_view s(line);
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
			s.remove_prefix(1);
		const std::size_t colon = s.find(':');
		if (colon != std::string_view::npos && colon > 0)
		{
			std::string_view head = s.substr(0, colon);
			if (head.size() > 2 && head[0] == '0' && (head[1] == 'x' || head[1] == 'X')) head.remove_prefix(2);
			if (head.size() >= 4 && std::all_of(head.begin(), head.end(), isHex))
			{
				uint64_t v = 0;
				if (std::from_chars(head.data(), head.data() + head.size(), v, 16).ec == std::errc()) return v;
			}
		}
		return parseFirstAddress(line);
	}
	case TriPane::IR: {
		if (auto a = commentAddress(line, ";")) return a;
		if (irMetadata)
		{
			const std::size_t at = line.find("!insn.addr !");
			if (at != std::string::npos)
			{
				int id = 0;
				const char* b = line.data() + at + 12;
				if (std::from_chars(b, line.data() + line.size(), id).ec == std::errc())
				{
					const auto it = irMetadata->find(id);
					if (it != irMetadata->end()) return it->second;
				}
			}
		}
		return std::nullopt;
	}
	case TriPane::C: {
		if (const std::size_t ar = line.find("Address range:"); ar != std::string::npos)
			return parseFirstAddress(std::string_view(line).substr(ar));
		return commentAddress(line, "//");
	}
	}
	return std::nullopt;
}

LineMapping buildLineMappingByAddress(const std::vector<std::string>& asmLines, const std::vector<std::string>& irLines,
                                      const std::vector<std::string>& cLines)
{
	const std::map<int, uint64_t> meta = parseIrAddressMetadata(irLines);
	auto firstLines = [&](TriPane pane, const std::vector<std::string>& lines) {
		std::unordered_map<uint64_t, int> first;
		std::vector<std::pair<uint64_t, int>> order;
		for (std::size_t i = 0; i < lines.size(); ++i)
			if (auto a = lineAddress(pane, lines[i], pane == TriPane::IR ? &meta : nullptr))
				if (first.emplace(*a, static_cast<int>(i) + 1).second) order.emplace_back(*a, static_cast<int>(i) + 1);
		return std::pair{first, order};
	};
	const auto [irFirst, irOrder] = firstLines(TriPane::IR, irLines);
	const auto [cFirst, cOrder] = firstLines(TriPane::C, cLines);

	LineMapping m;
	std::unordered_map<uint64_t, bool> asmSeen;
	for (std::size_t i = 0; i < asmLines.size(); ++i)
	{
		const auto a = lineAddress(TriPane::Asm, asmLines[i]);
		if (!a) continue;
		asmSeen[*a] = true;
		const auto ir = irFirst.find(*a);
		const auto c = cFirst.find(*a);
		m.addEntry(static_cast<int>(i) + 1, ir == irFirst.end() ? -1 : ir->second, c == cFirst.end() ? -1 : c->second);
	}
	// IR / C lines with an address but no assembly line: pair them with each other.
	for (const auto& [addr, line]: irOrder)
	{
		if (asmSeen.count(addr)) continue;
		const auto c = cFirst.find(addr);
		m.addEntry(-1, line, c == cFirst.end() ? -1 : c->second);
	}
	for (const auto& [addr, line]: cOrder)
	{
		if (asmSeen.count(addr) || irFirst.count(addr)) continue;
		m.addEntry(-1, -1, line);
	}
	return m;
}

int proportionalScroll(int value, int srcMax, int dstMax)
{
	if (srcMax <= 0) return 0;
	return static_cast<int>(static_cast<double>(value) / srcMax * dstMax);
}

// ── NavHistory ───────────────────────────────────────────────────────────────

void NavHistory::push(uint64_t address, std::string name)
{
	while (static_cast<int>(entries_.size()) > index_ + 1)
		entries_.pop_back();
	entries_.push_back({address, std::move(name)});
	if (entries_.size() > cap_) entries_.erase(entries_.begin());
	index_ = static_cast<int>(entries_.size()) - 1;
}

std::optional<NavHistory::Entry> NavHistory::back()
{
	if (!canGoBack()) return std::nullopt;
	--index_;
	return entries_[static_cast<std::size_t>(index_)];
}

std::optional<NavHistory::Entry> NavHistory::forward()
{
	if (!canGoForward()) return std::nullopt;
	++index_;
	return entries_[static_cast<std::size_t>(index_)];
}

void NavHistory::clear()
{
	entries_.clear();
	index_ = -1;
}

} // namespace retdec::gui::model
