/**
 * @file include/retdec/gui/model/funclist_model.h
 * @brief Function roster data behind the Functions panel: the editable list
 *        (rename, notes, tags, signature), its per-column display text,
 *        tooltips, confidence tiers and pattern badges.
 *
 * Port of the non-UI half of the Qt FunctionListModel / FunctionPatternDelegate.
 * No UI types: retdec::gui::ui::FunctionListPanel draws what this computes.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_FUNCLIST_MODEL_H
#define RETDEC_GUI_MODEL_FUNCLIST_MODEL_H

#include "retdec/gui/model/types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// Columns of the function table (same order as the Qt panel).
enum FunctionColumn : int
{
	FnColAddress = 0,
	FnColName = 1,
	FnColSize = 2,
	FnColInstrs = 3,
	FnColConfidence = 4,
	FnColPatterns = 5,
	FnColCC = 6,
	FnColNotes = 7,
	FnColCount = 8,
};

/// Header text of @p col ("Address", "Name", ...); empty for an unknown column.
std::string_view functionColumnHeader(int col);

/// Columns the user can edit in place (Name, Notes).
bool functionColumnEditable(int col);

/// Colour band of a composite confidence: >= 0.75 high (green), >= 0.40 medium
/// (yellow), else low (red).
enum class ConfidenceTier
{
	High,
	Medium,
	Low,
};
ConfidenceTier confidenceTier(float composite);

/// One of the five pattern badges (STL, Crypto, Algo, Design, Lib).
struct PatternBadgeInfo
{
	std::string_view label; ///< 2-character badge text
	std::string_view tip;   ///< tooltip line
	uint32_t rgb = 0;       ///< 0xRRGGBB badge colour (Catppuccin Mocha)
};
const std::array<PatternBadgeInfo, 5>& patternBadges();
bool patternActive(const PatternFlags& p, int badge);

/// Name shown in the Name column: the raw symbol when @p demangle is off and
/// one exists, else the demangled name, falling back to the raw one.
std::string functionDisplayName(const FunctionEntry& e, bool demangle = true);

/// Name reported when a function is activated (name, else raw name).
std::string functionActivationName(const FunctionEntry& e);

/// Display text of one cell, exactly as the Qt table showed it ("0x" + 16
/// zero-padded hex digits, "100 B" / "—", "80 %", ...). Patterns is empty
/// (drawn as badges).
std::string functionCellText(const FunctionEntry& e, int col, bool demangle = true);

/// Tooltip of one cell (name + raw + signature; confidence breakdown; pattern
/// list; calling convention). Empty when the column has none.
std::string functionToolTip(const FunctionEntry& e, int col);

/**
 * @brief The editable function list.
 *
 * Rows are positions in functions(); a row means nothing once setFunctions()
 * replaced the vector, so anything that must survive a reload (a dialog that
 * is open while a decompile finishes) keeps the address and asks
 * rowForAddress() again.
 */
class FunctionList
{
public:
	void setFunctions(std::vector<FunctionEntry> fns);
	void clearAll();

	const std::vector<FunctionEntry>& functions() const { return fns_; }
	std::size_t size() const { return fns_.size(); }
	bool empty() const { return fns_.empty(); }
	const FunctionEntry& entry(std::size_t row) const { return fns_[row]; }

	/// Row holding @p address, or -1.
	int rowForAddress(uint64_t address) const;
	const FunctionEntry* find(uint64_t address) const;

	/// Rename row @p row (the Qt setData on the Name column): the new name is
	/// trimmed; empty or unchanged names are refused. Fires onRenamed.
	bool setName(std::size_t row, std::string_view newName);
	/// Rename by address; false when the address is unknown or the name refused.
	bool renameFunction(uint64_t address, std::string_view newName);
	/// Replace the notes (not trimmed, may be empty). False when unknown.
	bool setNotes(uint64_t address, std::string notes);
	/// Replace the signature (trimmed; empty refused). False when unknown.
	bool setSignature(uint64_t address, std::string_view signature);
	/// Add @p tag (trimmed, non-empty) to every listed function that lacks it.
	void applyTag(const std::vector<uint64_t>& addresses, std::string_view tag);
	/// Clear notes, tags and signature and restore the raw name (a rename,
	/// so onRenamed fires) or, without one, clear the name. False when unknown.
	bool clearAnnotation(uint64_t address);

	/// Bumped by every change; views cache against it.
	uint64_t revision() const { return revision_; }

	/// (address, oldName, newName) after a successful rename.
	std::function<void(uint64_t, const std::string&, const std::string&)> onRenamed;

private:
	std::vector<FunctionEntry> fns_;
	uint64_t revision_ = 1;
};

/// Leading/trailing ASCII whitespace removed.
std::string funclistTrim(std::string_view s);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_FUNCLIST_MODEL_H
