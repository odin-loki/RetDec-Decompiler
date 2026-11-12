/**
 * @file include/retdec/gui/model/funclist_filter.h
 * @brief Filtering and sorting of the function list (port of the Qt
 *        FunctionFilterProxy).
 *
 * A row is shown when ALL active criteria hold:
 *  - name glob (empty = pass): wildcard, case-insensitive, whole-name match
 *    against the demangled or the raw name
 *  - address >= low (0 = no bound), address <= high (0 = no bound)
 *  - composite confidence >= minimum
 *  - at least one enabled pattern category (when any is enabled)
 *  - class filter: the name starts with "<class>::"
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_FUNCLIST_FILTER_H
#define RETDEC_GUI_MODEL_FUNCLIST_FILTER_H

#include "retdec/gui/model/funclist_model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/**
 * Wildcard match with the rules Qt's QRegularExpression::wildcardToRegularExpression
 * (Qt 6 default) gives: the whole text must match, `*` is any run and `?` any
 * one character except the path separator '/', `[abc]` / `[!abc]` / `[a-z]`
 * are character classes. ASCII case-insensitive.
 */
bool funclistWildcardMatch(std::string_view glob, std::string_view text);

/// Parse an address-range field like QString::toULongLong(&ok, 16): optional
/// surrounding blanks, optional 0x/0X prefix, hex digits. Nothing when empty or
/// malformed.
std::optional<uint64_t> funclistParseHex(std::string_view text);

struct FunctionFilter
{
	std::string nameGlob;
	uint64_t addrLow = 0;  ///< 0 = no lower bound
	uint64_t addrHigh = 0; ///< 0 = no upper bound
	float minConfidence = 0.f;
	bool stl = false, crypto = false, algo = false, design = false, lib = false;
	std::string classFilter;

	bool anyPattern() const { return stl || crypto || algo || design || lib; }
	bool accepts(const FunctionEntry& e) const;
};

/// Strict-weak "less" of two functions on @p col (address, names case-
/// insensitively, sizes, confidence, number of patterns, CC, notes).
bool functionLess(const FunctionEntry& a, const FunctionEntry& b, int col, bool demangle = true);

/**
 * @brief Filtered, sorted view of a FunctionList (rows = indices into it).
 *
 * Recomputes lazily whenever the source's revision() or a criterion changed,
 * so it is always in step with the list it watches. Sorting is stable;
 * sortColumn() < 0 keeps the source order.
 */
class FunctionListProxy
{
public:
	explicit FunctionListProxy(const FunctionList* source);

	void setNameGlob(std::string_view glob);
	void setAddressRange(uint64_t low, uint64_t high);
	void setMinConfidence(float minConf);
	void setPatternFilter(bool stl, bool crypto, bool algo, bool design, bool lib);
	void setClassFilter(std::string_view className);
	void setFilter(FunctionFilter f);
	const FunctionFilter& filter() const { return filter_; }

	void sortBy(int col, bool ascending);
	int sortColumn() const { return sortCol_; }
	bool sortAscending() const { return asc_; }
	void setDemangleNames(bool on);

	/// Forces a recompute (the source changed without a revision bump).
	void invalidate() { dirty_ = true; }

	std::size_t rowCount() const;
	/// Index into the source list of view row @p row.
	std::size_t sourceRow(std::size_t row) const;
	/// View row showing source row @p src, or -1 when filtered out.
	int rowForSource(std::size_t src) const;
	/// View row showing @p address, or -1.
	int rowForAddress(uint64_t address) const;
	const FunctionEntry& entry(std::size_t row) const;
	const std::vector<std::size_t>& rows() const;

private:
	void refresh() const;

	const FunctionList* src_;
	FunctionFilter filter_;
	int sortCol_ = -1;
	bool asc_ = true;
	bool demangle_ = true;
	mutable bool dirty_ = true;
	mutable uint64_t seenRevision_ = 0;
	mutable std::vector<std::size_t> rows_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_FUNCLIST_FILTER_H
