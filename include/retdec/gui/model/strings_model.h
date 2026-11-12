/**
 * @file include/retdec/gui/model/strings_model.h
 * @brief Tables behind the Strings & Constants panel: rows, column text,
 *        filters, sorting and CSV export (port of the Qt StringsModel,
 *        ConstantsModel and TypeFilterProxy).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_STRINGS_MODEL_H
#define RETDEC_GUI_MODEL_STRINGS_MODEL_H

#include "retdec/gui/model/strings_classify.h"
#include "retdec/gui/model/types.h"

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

// ─── Strings ─────────────────────────────────────────────────────────────────

/// Strings tab columns: Address | Type | Category | Preview | Refs.
enum StringsColumn : int
{
	StrColAddress = 0,
	StrColType = 1,
	StrColCategory = 2,
	StrColPreview = 3,
	StrColRefs = 4,
	StrColCount = 5,
};
std::string_view stringsColumnHeader(int col);
/// Preview column width in characters (UTF-8 code points).
inline constexpr std::size_t kStringPreviewChars = 80;
/// The first @p n code points of a UTF-8 string.
std::string utf8Left(std::string_view s, std::size_t n);
std::string stringCellText(const StringEntry& e, int col);
/// Tooltip (the full value on the Preview column, else empty).
std::string stringToolTip(const StringEntry& e, int col);
/// Preview colour by category (0xRRGGBB), 0 when the default text colour applies.
uint32_t stringCategoryColor(StringCategory c);

class StringsTable
{
public:
	/// Entries still in the default Plain category are classified.
	void setStrings(std::vector<StringEntry> entries);
	void appendStrings(std::vector<StringEntry> entries);
	void clear();
	std::size_t size() const { return entries_.size(); }
	const StringEntry& entry(std::size_t row) const { return entries_[row]; }
	const std::vector<StringEntry>& entries() const { return entries_; }
	uint64_t revision() const { return revision_; }
	/// "Address,Type,Category,Value,Refs" + every row (value quoted, `"` doubled).
	std::string exportCsv() const;

private:
	std::vector<StringEntry> entries_;
	uint64_t revision_ = 1;
};

/**
 * Strings filter: type and category are case-insensitive "contains" matches on
 * the column text (empty = all); the text filter looks at the Preview column
 * only, as a case-insensitive substring or (regex mode) an ECMAScript regular
 * expression searched case-insensitively. An invalid regex shows nothing.
 */
class StringsFilter
{
public:
	void setTypeFilter(std::string_view typeName);
	void setCategoryFilter(std::string_view catName);
	void setTextFilter(std::string_view text, bool useRegex = false);
	bool accepts(const StringEntry& e) const;
	bool regexValid() const { return !useRegex_ || re_.has_value(); }

	const std::string& typeFilter() const { return type_; }
	const std::string& categoryFilter() const { return cat_; }
	const std::string& textFilter() const { return text_; }
	bool useRegex() const { return useRegex_; }

private:
	std::string type_, cat_, text_;
	bool useRegex_ = false;
	std::optional<std::regex> re_;
};

/// The search box text the Qt panel accepted: "/pattern/" means regex.
/// Returns (effective text, regex?) with @p regexButton forcing regex mode.
std::pair<std::string, bool> parseStringSearch(std::string_view text, bool regexButton);

bool stringLess(const StringEntry& a, const StringEntry& b, int col);

/// Filtered, sorted rows of a StringsTable (lazy, follows its revision).
class StringsProxy
{
public:
	explicit StringsProxy(const StringsTable* source): src_(source) {}
	StringsFilter& filter() { dirty_ = true; return filter_; }
	const StringsFilter& filter() const { return filter_; }
	void setTypeFilter(std::string_view t) { filter().setTypeFilter(t); }
	void setCategoryFilter(std::string_view c) { filter().setCategoryFilter(c); }
	void setTextFilter(std::string_view t, bool useRegex = false) { filter().setTextFilter(t, useRegex); }
	void sortBy(int col, bool ascending);
	int sortColumn() const { return sortCol_; }
	bool sortAscending() const { return asc_; }
	void invalidate() { dirty_ = true; }
	std::size_t rowCount() const;
	std::size_t sourceRow(std::size_t row) const;
	const StringEntry& entry(std::size_t row) const { return src_->entry(sourceRow(row)); }

private:
	void refresh() const;
	const StringsTable* src_;
	StringsFilter filter_;
	int sortCol_ = -1;
	bool asc_ = true;
	mutable bool dirty_ = true;
	mutable uint64_t seen_ = 0;
	mutable std::vector<std::size_t> rows_;
};

// ─── Constants ───────────────────────────────────────────────────────────────

/// Constants tab columns: Address | Size | Hex | Decimal | Label | Refs.
enum ConstantsColumn : int
{
	ConstColAddress = 0,
	ConstColSize = 1,
	ConstColHex = 2,
	ConstColDec = 3,
	ConstColLabel = 4,
	ConstColRefs = 5,
	ConstColCount = 6,
};
std::string_view constantsColumnHeader(int col);
std::string constantCellText(const ConstantEntry& e, int col);
/// "<label> (0x<hex>)" on the Label column when labelled, else empty.
std::string constantToolTip(const ConstantEntry& e, int col);
/// Row colour by label kind (0xRRGGBB): crypto amber, magic blue, float green; 0 = default.
uint32_t constantLabelColor(ConstantLabel k);

class ConstantsTable
{
public:
	/// Every entry is (re)labelled by ConstantLabeler.
	void setConstants(std::vector<ConstantEntry> entries);
	void clear();
	std::size_t size() const { return entries_.size(); }
	const ConstantEntry& entry(std::size_t row) const { return entries_[row]; }
	const std::vector<ConstantEntry>& entries() const { return entries_; }
	uint64_t revision() const { return revision_; }
	/// "Address,Size,Hex,Decimal,Label,Refs" + every row (label quoted).
	std::string exportCsv() const;

private:
	std::vector<ConstantEntry> entries_;
	uint64_t revision_ = 1;
};

/**
 * Constants filter. With a label kind selected only that kind is shown (the
 * text is ignored, as in Qt where the kind filter took over the proxy);
 * otherwise rows whose any column contains the text (case-insensitive).
 */
struct ConstantsFilter
{
	std::string text;
	std::optional<ConstantLabel> kind; ///< nullopt = all kinds
	bool accepts(const ConstantEntry& e) const;
};

bool constantLess(const ConstantEntry& a, const ConstantEntry& b, int col);

class ConstantsProxy
{
public:
	explicit ConstantsProxy(const ConstantsTable* source): src_(source) {}
	void setTextFilter(std::string_view text) { filter_.text = std::string(text); dirty_ = true; }
	void setKindFilter(std::optional<ConstantLabel> kind) { filter_.kind = kind; dirty_ = true; }
	const ConstantsFilter& filter() const { return filter_; }
	void sortBy(int col, bool ascending);
	int sortColumn() const { return sortCol_; }
	bool sortAscending() const { return asc_; }
	void invalidate() { dirty_ = true; }
	std::size_t rowCount() const;
	std::size_t sourceRow(std::size_t row) const;
	const ConstantEntry& entry(std::size_t row) const { return src_->entry(sourceRow(row)); }

private:
	void refresh() const;
	const ConstantsTable* src_;
	ConstantsFilter filter_;
	int sortCol_ = -1;
	bool asc_ = true;
	mutable bool dirty_ = true;
	mutable uint64_t seen_ = 0;
	mutable std::vector<std::size_t> rows_;
};

/// The label kinds the constants filter combo lists, in order (index 0 = All).
const std::vector<std::optional<ConstantLabel>>& constantKindFilterItems();

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_STRINGS_MODEL_H
