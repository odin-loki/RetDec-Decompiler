/**
 * @file src/gui/model/strings_model.cpp
 * @brief Strings & constants tables, filters, sorting and CSV export.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/strings_model.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace retdec::gui::model {

namespace {

char lower(char c)
{
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool containsCi(std::string_view s, std::string_view p)
{
	if (p.empty()) return true;
	if (s.size() < p.size()) return false;
	for (std::size_t i = 0; i + p.size() <= s.size(); ++i)
	{
		bool ok = true;
		for (std::size_t k = 0; k < p.size() && ok; ++k)
			ok = lower(s[i + k]) == lower(p[k]);
		if (ok) return true;
	}
	return false;
}

int ciCompare(std::string_view a, std::string_view b)
{
	const std::size_t n = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < n; ++i)
	{
		const char x = lower(a[i]), y = lower(b[i]);
		if (x != y) return x < y ? -1 : 1;
	}
	if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
	return a < b ? -1 : (a == b ? 0 : 1);
}

void classify(StringEntry& e)
{
	if (e.category == StringCategory::Plain) e.category = CategoryDetector::classify(e.value);
}

} // namespace

std::string utf8Left(std::string_view s, std::size_t n)
{
	std::size_t i = 0, cps = 0;
	while (i < s.size() && cps < n)
	{
		const unsigned char c = static_cast<unsigned char>(s[i]);
		std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
		i = std::min(s.size(), i + len);
		++cps;
	}
	return std::string(s.substr(0, i));
}

// ─── Strings ─────────────────────────────────────────────────────────────────

std::string_view stringsColumnHeader(int col)
{
	switch (col)
	{
	case StrColAddress: return "Address";
	case StrColType: return "Type";
	case StrColCategory: return "Category";
	case StrColPreview: return "Preview";
	case StrColRefs: return "Refs";
	default: return {};
	}
}

std::string stringCellText(const StringEntry& e, int col)
{
	switch (col)
	{
	case StrColAddress: return std::format("0x{:016x}", e.address);
	case StrColType: return std::string(stringTypeName(e.type));
	case StrColCategory: return std::string(stringCategoryName(e.category));
	case StrColPreview: return utf8Left(e.value, kStringPreviewChars);
	case StrColRefs: return std::to_string(e.refCount);
	default: return {};
	}
}

std::string stringToolTip(const StringEntry& e, int col)
{
	return col == StrColPreview ? e.value : std::string();
}

uint32_t stringCategoryColor(StringCategory c)
{
	switch (c)
	{
	case StringCategory::CryptoConst: return 0xe6a23c; // amber
	case StringCategory::URL: return 0x89b4fa;         // blue
	case StringCategory::FormatString: return 0xa6e3a1; // green
	case StringCategory::FilePath: return 0xf9e2af;    // yellow
	case StringCategory::RegEx: return 0xcba6f7;       // mauve
	default: return 0;
	}
}

void StringsTable::setStrings(std::vector<StringEntry> entries)
{
	for (auto& e: entries)
		classify(e);
	entries_ = std::move(entries);
	++revision_;
}

void StringsTable::appendStrings(std::vector<StringEntry> extra)
{
	if (extra.empty()) return;
	for (auto& e: extra)
	{
		classify(e);
		entries_.push_back(std::move(e));
	}
	++revision_;
}

void StringsTable::clear()
{
	entries_.clear();
	++revision_;
}

std::string StringsTable::exportCsv() const
{
	std::string csv = "Address,Type,Category,Value,Refs\n";
	for (const auto& e: entries_)
	{
		std::string val;
		val.reserve(e.value.size());
		for (char c: e.value)
		{
			if (c == '"') val += '"';
			val += c;
		}
		csv += std::format(
			"0x{:016x},{},{},\"{}\",{}\n", e.address, stringTypeName(e.type), stringCategoryName(e.category), val, e.refCount);
	}
	return csv;
}

void StringsFilter::setTypeFilter(std::string_view t)
{
	type_ = std::string(t);
}

void StringsFilter::setCategoryFilter(std::string_view c)
{
	cat_ = std::string(c);
}

void StringsFilter::setTextFilter(std::string_view text, bool useRegex)
{
	text_ = std::string(text);
	useRegex_ = useRegex;
	re_.reset();
	if (useRegex_)
	{
		try
		{
			re_.emplace(text_, std::regex::ECMAScript | std::regex::icase);
		}
		catch (const std::regex_error&)
		{
			re_.reset();
		}
	}
}

bool StringsFilter::accepts(const StringEntry& e) const
{
	if (!type_.empty() && !containsCi(stringTypeName(e.type), type_)) return false;
	if (!cat_.empty() && !containsCi(stringCategoryName(e.category), cat_)) return false;
	if (!text_.empty())
	{
		const std::string cell = stringCellText(e, StrColPreview);
		if (useRegex_)
		{
			if (!re_ || !std::regex_search(cell, *re_)) return false;
		}
		else if (!containsCi(cell, text_))
			return false;
	}
	return true;
}

std::pair<std::string, bool> parseStringSearch(std::string_view text, bool regexButton)
{
	std::string effective(text);
	bool useRegex = false;
	if (text.size() > 1 && text.front() == '/' && text.back() == '/')
	{
		effective = std::string(text.substr(1, text.size() - 2));
		useRegex = true;
	}
	return {effective, useRegex || regexButton};
}

bool stringLess(const StringEntry& a, const StringEntry& b, int col)
{
	switch (col)
	{
	case StrColAddress: return a.address < b.address;
	case StrColRefs: return a.refCount < b.refCount;
	default: return ciCompare(stringCellText(a, col), stringCellText(b, col)) < 0;
	}
}

void StringsProxy::sortBy(int col, bool ascending)
{
	sortCol_ = col;
	asc_ = ascending;
	dirty_ = true;
}

void StringsProxy::refresh() const
{
	if (!dirty_ && seen_ == src_->revision()) return;
	dirty_ = false;
	seen_ = src_->revision();
	rows_.clear();
	for (std::size_t i = 0; i < src_->size(); ++i)
		if (filter_.accepts(src_->entry(i))) rows_.push_back(i);
	if (sortCol_ >= 0 && sortCol_ < StrColCount)
	{
		const auto& es = src_->entries();
		std::stable_sort(rows_.begin(), rows_.end(), [&](std::size_t x, std::size_t y) {
			return asc_ ? stringLess(es[x], es[y], sortCol_) : stringLess(es[y], es[x], sortCol_);
		});
	}
}

std::size_t StringsProxy::rowCount() const
{
	refresh();
	return rows_.size();
}

std::size_t StringsProxy::sourceRow(std::size_t row) const
{
	refresh();
	return rows_[row];
}

// ─── Constants ───────────────────────────────────────────────────────────────

std::string_view constantsColumnHeader(int col)
{
	switch (col)
	{
	case ConstColAddress: return "Address";
	case ConstColSize: return "Size";
	case ConstColHex: return "Hex";
	case ConstColDec: return "Decimal";
	case ConstColLabel: return "Label";
	case ConstColRefs: return "Refs";
	default: return {};
	}
}

std::string constantCellText(const ConstantEntry& e, int col)
{
	switch (col)
	{
	case ConstColAddress: return std::format("0x{:016x}", e.address);
	case ConstColSize: return std::to_string(static_cast<int>(e.size));
	case ConstColHex: return e.hexStr;
	case ConstColDec: return e.decStr;
	case ConstColLabel: return e.label;
	case ConstColRefs: return std::to_string(e.refCount);
	default: return {};
	}
}

std::string constantToolTip(const ConstantEntry& e, int col)
{
	if (col != ConstColLabel || e.label.empty()) return {};
	return std::format("{} (0x{:x})", e.label, e.value);
}

uint32_t constantLabelColor(ConstantLabel k)
{
	switch (k)
	{
	case ConstantLabel::CryptoConstant: return 0xe6a23c;
	case ConstantLabel::MagicNumber: return 0x89b4fa;
	case ConstantLabel::FloatSpecial: return 0xa6e3a1;
	default: return 0;
	}
}

void ConstantsTable::setConstants(std::vector<ConstantEntry> entries)
{
	for (auto& e: entries)
		ConstantLabeler::label(e);
	entries_ = std::move(entries);
	++revision_;
}

void ConstantsTable::clear()
{
	entries_.clear();
	++revision_;
}

std::string ConstantsTable::exportCsv() const
{
	std::string csv = "Address,Size,Hex,Decimal,Label,Refs\n";
	for (const auto& e: entries_)
	{
		csv += std::format(
			"0x{:016x},{},{},{},\"{}\",{}\n",
			e.address,
			static_cast<int>(e.size),
			e.hexStr,
			e.decStr,
			e.label,
			e.refCount);
	}
	return csv;
}

bool ConstantsFilter::accepts(const ConstantEntry& e) const
{
	if (kind) return e.labelKind == *kind;
	if (text.empty()) return true;
	for (int c = 0; c < ConstColCount; ++c)
		if (containsCi(constantCellText(e, c), text)) return true;
	return false;
}

bool constantLess(const ConstantEntry& a, const ConstantEntry& b, int col)
{
	switch (col)
	{
	case ConstColAddress: return a.address < b.address;
	case ConstColSize: return static_cast<int>(a.size) < static_cast<int>(b.size);
	case ConstColHex:
	case ConstColDec: return a.value < b.value;
	case ConstColLabel: return ciCompare(a.label, b.label) < 0;
	case ConstColRefs: return a.refCount < b.refCount;
	default: return false;
	}
}

void ConstantsProxy::sortBy(int col, bool ascending)
{
	sortCol_ = col;
	asc_ = ascending;
	dirty_ = true;
}

void ConstantsProxy::refresh() const
{
	if (!dirty_ && seen_ == src_->revision()) return;
	dirty_ = false;
	seen_ = src_->revision();
	rows_.clear();
	for (std::size_t i = 0; i < src_->size(); ++i)
		if (filter_.accepts(src_->entry(i))) rows_.push_back(i);
	if (sortCol_ >= 0 && sortCol_ < ConstColCount)
	{
		const auto& es = src_->entries();
		std::stable_sort(rows_.begin(), rows_.end(), [&](std::size_t x, std::size_t y) {
			return asc_ ? constantLess(es[x], es[y], sortCol_) : constantLess(es[y], es[x], sortCol_);
		});
	}
}

std::size_t ConstantsProxy::rowCount() const
{
	refresh();
	return rows_.size();
}

std::size_t ConstantsProxy::sourceRow(std::size_t row) const
{
	refresh();
	return rows_[row];
}

const std::vector<std::optional<ConstantLabel>>& constantKindFilterItems()
{
	static const std::vector<std::optional<ConstantLabel>> items = {
		std::nullopt,
		ConstantLabel::MagicNumber,
		ConstantLabel::CryptoConstant,
		ConstantLabel::FloatSpecial,
		ConstantLabel::PortNumber,
		ConstantLabel::Errno,
		ConstantLabel::WinError,
		ConstantLabel::SyscallNr,
		ConstantLabel::OtherKnown,
	};
	return items;
}

} // namespace retdec::gui::model
