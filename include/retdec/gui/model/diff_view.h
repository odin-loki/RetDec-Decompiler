/**
 * @file include/retdec/gui/model/diff_view.h
 * @brief What the Diff panel shows for a DiffResult: side-by-side and unified
 *        rows (with intra-line spans on changed line pairs), hunk starts for
 *        Prev/Next navigation, the statistics line, and the named recovery
 *        stages of the stage selector.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DIFF_VIEW_H
#define RETDEC_GUI_MODEL_DIFF_VIEW_H

#include "retdec/gui/model/diff_engine.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::model {

/// Colours of the Qt panel (0xRRGGBB).
inline constexpr uint32_t kDiffRemovedBg = 0x3d1212;
inline constexpr uint32_t kDiffAddedBg = 0x122d12;
inline constexpr uint32_t kDiffChangedBg = 0x2d2d12;
inline constexpr uint32_t kDiffPlaceholderBg = 0x181825;
inline constexpr uint32_t kDiffRemovedSpanBg = 0x7a2a2a; ///< intra-line: removed characters
inline constexpr uint32_t kDiffAddedSpanBg = 0x2a6a2a;   ///< intra-line: added characters
inline constexpr uint32_t kDiffLineNumberFg = 0x555577;
/// A changed line pair gets intra-line spans only when at most this many
/// tenths of its characters changed.
inline constexpr std::size_t kIntraLineMaxChangedTenths = 6;

enum class DiffCellKind : uint8_t
{
	Equal,
	Removed,
	Added,
	Placeholder, ///< the other side has a line here, this one none
};

struct DiffCell
{
	DiffCellKind kind = DiffCellKind::Equal;
	std::string text;
	int line = -1;               ///< 0-based line in its text, -1 for a placeholder
	std::vector<DiffSpan> spans; ///< changed characters (paired changed lines only)
};

struct SideBySideRow
{
	DiffCell left, right;
	bool changed() const
	{
		return left.kind != DiffCellKind::Equal || right.kind != DiffCellKind::Equal;
	}
};

/**
 * One row per left/right line pair. Within a run of changes the k-th deleted
 * line is paired with the k-th inserted one (both get intra-line spans); the
 * longer side's surplus faces a placeholder. Spans are dropped when more than
 * kIntraLineMaxChangedTenths / 10 of a pair changed. With @p pairChanges false every
 * op gets its own row (the Qt layout: deletes then inserts, each facing a
 * placeholder).
 */
std::vector<SideBySideRow> buildSideBySide(const DiffResult& diff, bool pairChanges = true);

struct UnifiedRow
{
	DiffOpKind kind = DiffOpKind::Equal;
	std::string text;
	int leftLine = -1;  ///< 0-based, -1 for an insert
	int rightLine = -1; ///< 0-based, -1 for a delete
	std::vector<DiffSpan> spans;
	/// '+', '-' or ' '.
	char prefix() const
	{
		return kind == DiffOpKind::Insert ? '+' : kind == DiffOpKind::Delete ? '-' : ' ';
	}
};

/// One row per op; within a run of changes the removed lines come first, then
/// the added ones (as `diff -u` shows them). Removed / added lines paired as in
/// buildSideBySide() carry intra-line spans.
std::vector<UnifiedRow> buildUnified(const DiffResult& diff);

/// Indices of the rows where a run of changes starts.
std::vector<int> diffHunkStarts(const std::vector<SideBySideRow>& rows);
std::vector<int> diffHunkStarts(const std::vector<UnifiedRow>& rows);

/// "No diff", or "+<added>  -<removed>  <similarity>% similar".
std::string diffStatsText(const DiffResult& diff);

/// Cyclic Prev/Next over hunk starts. Starts before the first hunk, so Next
/// goes to the first and Prev to the last.
class DiffHunkCursor {
public:
	void reset(std::vector<int> starts);
	/// Row of the next / previous hunk, nothing when there are none.
	std::optional<int> next();
	std::optional<int> prev();
	int current() const
	{
		return cur_;
	}
	std::size_t count() const
	{
		return starts_.size();
	}

private:
	std::vector<int> starts_;
	int cur_ = -1;
};

/// A named before/after pair of the stage selector ("after_typing", ...).
struct DiffStage
{
	std::string name;
	std::string before;
	std::string after;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DIFF_VIEW_H
