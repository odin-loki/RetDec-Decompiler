/**
 * @file src/gui/model/funclist_model.cpp
 * @brief Function roster model: display text, tooltips, badges, editing.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/funclist_model.h"

#include <algorithm>
#include <format>

namespace retdec::gui::model {

namespace {

constexpr std::string_view kEmDash = "\xE2\x80\x94"; // "—"

std::string joinStrings(const std::vector<std::string>& parts, std::string_view sep)
{
	std::string out;
	for (std::size_t i = 0; i < parts.size(); ++i)
	{
		if (i) out += sep;
		out += parts[i];
	}
	return out;
}

} // namespace

std::string funclistTrim(std::string_view s)
{
	auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
	std::size_t b = 0, e = s.size();
	while (b < e && ws(s[b]))
		++b;
	while (e > b && ws(s[e - 1]))
		--e;
	return std::string(s.substr(b, e - b));
}

std::string_view functionColumnHeader(int col)
{
	switch (col)
	{
	case FnColAddress: return "Address";
	case FnColName: return "Name";
	case FnColSize: return "Size";
	case FnColInstrs: return "Instrs";
	case FnColConfidence: return "Confidence";
	case FnColPatterns: return "Patterns";
	case FnColCC: return "CC";
	case FnColNotes: return "Notes";
	default: return {};
	}
}

bool functionColumnEditable(int col)
{
	return col == FnColName || col == FnColNotes;
}

ConfidenceTier confidenceTier(float c)
{
	if (c >= 0.75f) return ConfidenceTier::High;
	if (c >= 0.40f) return ConfidenceTier::Medium;
	return ConfidenceTier::Low;
}

const std::array<PatternBadgeInfo, 5>& patternBadges()
{
	static const std::array<PatternBadgeInfo, 5> kBadges = {{
		{"St", "STL container / algorithm", 0x89b4fa},
		{"Cr", "Cryptographic primitive", 0xf38ba8},
		{"Al", "Generic well-known algorithm", 0xf9e2af},
		{"Dp", "Design pattern (singleton, factory, \xE2\x80\xA6)", 0xcba6f7},
		{"Lb", "Statically linked library code", 0x94e2d5},
	}};
	return kBadges;
}

bool patternActive(const PatternFlags& p, int i)
{
	switch (i)
	{
	case 0: return p.stl;
	case 1: return p.crypto;
	case 2: return p.algo;
	case 3: return p.design;
	case 4: return p.library;
	default: return false;
	}
}

std::string functionDisplayName(const FunctionEntry& e, bool demangle)
{
	if (!demangle && !e.rawName.empty()) return e.rawName;
	return e.name.empty() ? e.rawName : e.name;
}

std::string functionActivationName(const FunctionEntry& e)
{
	return e.name.empty() ? e.rawName : e.name;
}

std::string functionCellText(const FunctionEntry& e, int col, bool demangle)
{
	switch (col)
	{
	case FnColAddress: return std::format("0x{:016x}", e.address);
	case FnColName: return functionDisplayName(e, demangle);
	case FnColSize: return e.sizeBytes > 0 ? std::format("{} B", e.sizeBytes) : std::string(kEmDash);
	case FnColInstrs: return e.instrCount > 0 ? std::to_string(e.instrCount) : std::string(kEmDash);
	case FnColConfidence: return std::format("{} %", static_cast<int>(e.confidence.composite() * 100));
	case FnColPatterns: return {};
	case FnColCC: return e.cc.empty() ? std::string(kEmDash) : e.cc;
	case FnColNotes: {
		std::string s = e.notes;
		if (!e.tags.empty()) s += std::string(s.empty() ? "" : "  ") + "[" + joinStrings(e.tags, ", ") + "]";
		return s;
	}
	default: return {};
	}
}

std::string functionToolTip(const FunctionEntry& e, int col)
{
	switch (col)
	{
	case FnColName: {
		std::string tip = e.name;
		if (!e.rawName.empty() && e.rawName != e.name) tip += "\n" + e.rawName;
		if (!e.signature.empty()) tip += "\n" + e.signature;
		return tip;
	}
	case FnColConfidence:
		return std::format(
			"Type inference: {}%\nStructure: {}%\nVariable: {}%\nAlgorithm: {}%\nComposite: {}%",
			static_cast<int>(e.confidence.typeInference * 100),
			static_cast<int>(e.confidence.structure * 100),
			static_cast<int>(e.confidence.variable * 100),
			static_cast<int>(e.confidence.algorithm * 100),
			static_cast<int>(e.confidence.composite() * 100));
	case FnColPatterns: {
		std::vector<std::string> active;
		for (int i = 0; i < 5; ++i)
			if (patternActive(e.patterns, i)) active.emplace_back(patternBadges()[static_cast<std::size_t>(i)].tip);
		return active.empty() ? std::string("No patterns detected") : joinStrings(active, "\n");
	}
	case FnColCC: return "Calling convention: " + e.cc;
	default: return {};
	}
}

// ─── FunctionList ────────────────────────────────────────────────────────────

void FunctionList::setFunctions(std::vector<FunctionEntry> fns)
{
	fns_ = std::move(fns);
	++revision_;
}

void FunctionList::clearAll()
{
	fns_.clear();
	++revision_;
}

int FunctionList::rowForAddress(uint64_t address) const
{
	for (std::size_t i = 0; i < fns_.size(); ++i)
		if (fns_[i].address == address) return static_cast<int>(i);
	return -1;
}

const FunctionEntry* FunctionList::find(uint64_t address) const
{
	const int r = rowForAddress(address);
	return r < 0 ? nullptr : &fns_[static_cast<std::size_t>(r)];
}

bool FunctionList::setName(std::size_t row, std::string_view value)
{
	if (row >= fns_.size()) return false;
	auto& e = fns_[row];
	const std::string oldName = e.name;
	std::string newName = funclistTrim(value);
	if (newName.empty() || newName == oldName) return false;
	e.name = newName;
	++revision_;
	const uint64_t address = e.address;
	if (onRenamed) onRenamed(address, oldName, newName);
	return true;
}

bool FunctionList::renameFunction(uint64_t address, std::string_view newName)
{
	const int row = rowForAddress(address);
	if (row < 0) return false;
	return setName(static_cast<std::size_t>(row), newName);
}

bool FunctionList::setNotes(uint64_t address, std::string notes)
{
	const int row = rowForAddress(address);
	if (row < 0) return false;
	fns_[static_cast<std::size_t>(row)].notes = std::move(notes);
	++revision_;
	return true;
}

bool FunctionList::setSignature(uint64_t address, std::string_view signature)
{
	const int row = rowForAddress(address);
	std::string sig = funclistTrim(signature);
	if (row < 0 || sig.empty()) return false;
	fns_[static_cast<std::size_t>(row)].signature = std::move(sig);
	++revision_;
	return true;
}

void FunctionList::applyTag(const std::vector<uint64_t>& addresses, std::string_view tagText)
{
	const std::string tag = funclistTrim(tagText);
	if (tag.empty()) return;
	for (auto& fn: fns_)
	{
		if (std::find(addresses.begin(), addresses.end(), fn.address) == addresses.end()) continue;
		if (std::find(fn.tags.begin(), fn.tags.end(), tag) == fn.tags.end()) fn.tags.push_back(tag);
	}
	++revision_;
}

bool FunctionList::clearAnnotation(uint64_t address)
{
	const int row = rowForAddress(address);
	if (row < 0) return false;
	auto& e = fns_[static_cast<std::size_t>(row)];
	e.notes.clear();
	e.tags.clear();
	e.signature.clear();
	++revision_;
	if (!e.rawName.empty())
		renameFunction(address, e.rawName);
	else
		e.name.clear();
	return true;
}

} // namespace retdec::gui::model
