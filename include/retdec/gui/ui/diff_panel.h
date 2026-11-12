/**
 * @file include/retdec/gui/ui/diff_panel.h
 * @brief Diff / Compare panel (FuseUI port of the Qt DiffPanel): before/after
 *        comparison of two decompilations, or of the output before and after
 *        a recovery stage.
 *
 * Layout
 *   [toolbar: Stage combo | Side by side / Unified | Prev Next | Unified HTML Copy]
 *   [stats: "+N  -M  K% similar"]
 *   side by side: two line-numbered panes ("Before[: stage]" / "After[: stage]")
 *                 in a splitter, scrolled together
 *   unified:      one pane with both line numbers and +/- prefixes
 * Removed lines have a red tint, added lines green, the missing side of a
 * changed line a dark placeholder; within a changed line pair the characters
 * that differ are highlighted more strongly (intra-line diff). Prev / Next
 * (also P / N, Up / Down, PageUp / PageDown, Home / End on the focused pane)
 * jump between hunks and scroll. "Unified" / "HTML" export through a save
 * dialog; "Copy" puts the unified diff on the clipboard.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_DIFF_PANEL_H
#define RETDEC_GUI_UI_DIFF_PANEL_H

#include "retdec/gui/model/diff_engine.h"
#include "retdec/gui/model/diff_view.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

class DiffPanel {
public:
	static constexpr std::string_view kTitle = "Diff / Compare";

	enum class ViewMode
	{
		SideBySide,
		Unified,
	};

	DiffPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~DiffPanel();
	DiffPanel(const DiffPanel&) = delete;
	DiffPanel& operator=(const DiffPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}

	// ── data in ─────────────────────────────────────────────────────────────
	/// Diffs @p before against @p after and shows it; @p stageName labels the
	/// panes ("Before: <stage>").
	void setDiff(std::string_view before, std::string_view after, std::string_view stageName = {});
	/// Registers a named recovery-stage pair in the stage selector and shows it.
	void addStage(std::string_view name, std::string_view before, std::string_view after);
	/// Shows stage @p index (1-based like the combo; 0 = "<manual>" does nothing).
	void selectStage(int index);
	void clear();

	const model::DiffResult& currentDiff() const
	{
		return diff_;
	}
	const std::vector<model::DiffStage>& stages() const
	{
		return stages_;
	}

	// ── view ────────────────────────────────────────────────────────────────
	void setViewMode(ViewMode m);
	ViewMode viewMode() const
	{
		return mode_;
	}
	/// Next / previous hunk (cyclic). Returns the row scrolled to, -1 if none.
	int nextHunk();
	int prevHunk();
	/// Top visible row of the panes.
	int firstVisibleRow() const
	{
		return first_;
	}
	/// Highlighted row (the hunk jumped to, or the clicked row), -1 = none.
	int cursorRow() const
	{
		return cursorRow_;
	}
	void scrollToRow(int row);
	std::string statsText() const
	{
		return model::diffStatsText(diff_);
	}

	// ── export ──────────────────────────────────────────────────────────────
	std::string unifiedText() const
	{
		return diff_.toUnifiedDiff("before", "after");
	}
	std::string htmlText() const
	{
		return diff_.toHtml();
	}
	std::expected<void, std::string> exportUnified(const std::string& path) const;
	std::expected<void, std::string> exportHtml(const std::string& path) const;
	void openExportDialog(bool html);
	void copyDiff();

	// ── events out ──────────────────────────────────────────────────────────
	/// The displayed diff changed (setDiff / addStage / stage selection / clear).
	std::function<void(const model::DiffResult&)> onDiffChanged;
	/// Clipboard writer; unset = UiContext::setClipboardText.
	std::function<bool(const std::string&)> clipboardWriter;

	// ── test hooks ──────────────────────────────────────────────────────────
	const std::vector<model::SideBySideRow>& sideBySideRows() const
	{
		return sbs_;
	}
	const std::vector<model::UnifiedRow>& unifiedRows() const
	{
		return uni_;
	}
	std::size_t rowCount() const;
	fuse::ui::NodeHandle leftPane() const
	{
		return leftPane_;
	}
	fuse::ui::NodeHandle rightPane() const
	{
		return rightPane_;
	}
	fuse::ui::NodeHandle unifiedPane() const
	{
		return uniPane_;
	}
	fuse::ui::NodeHandle leftLabel() const
	{
		return leftLabel_;
	}
	fuse::ui::NodeHandle rightLabel() const
	{
		return rightLabel_;
	}
	fuse::ui::NodeHandle statsNode() const
	{
		return stats_;
	}
	fuse::ui::NodeHandle stageCombo() const
	{
		return stageCombo_;
	}
	fuse::ui::NodeHandle modeCombo() const
	{
		return modeCombo_;
	}
	fuse::ui::NodeHandle prevButton() const
	{
		return prevBtn_;
	}
	fuse::ui::NodeHandle nextButton() const
	{
		return nextBtn_;
	}
	fuse::ui::NodeHandle copyButton() const
	{
		return copyBtn_;
	}
	fuse::ui::NodeHandle exportUnifiedButton() const
	{
		return exportUBtn_;
	}
	fuse::ui::NodeHandle exportHtmlButton() const
	{
		return exportHBtn_;
	}
	fuse::ui::NodeHandle fileDialogNode() const
	{
		return fileDlg_;
	}
	/// Rows that fit in a pane.
	int visibleRows() const;

private:
	enum class PaneKind
	{
		Left,
		Right,
		Unified,
	};
	struct PaneRef
	{
		DiffPanel* panel = nullptr;
		PaneKind kind = PaneKind::Left;
	};

	void build(fuse::ui::NodeHandle parent);
	fuse::ui::NodeHandle makePane(fuse::ui::NodeHandle parent, PaneRef* ref);
	void apply(model::DiffResult diff);
	void resetHunks();
	void updateStats();
	void scrollToRowTop(int row);
	void invalidatePanes();
	void tooltip(fuse::ui::NodeHandle n, std::string_view text);
	void updateStageCombo();
	bool copy(const std::string& text);
	float lineHeight() const;
	bool onPaneEvent(const fuse::ui::Event& e);
	static void paintPane(const fuse::ui::PaintContext& pc, fuse::ui::DrawList& dl);
	static fuse::ui::Size measurePane(fuse::ui::UiContext&, fuse::ui::NodeHandle, fuse::ui::Size);
	static bool paneEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);

	fuse::ui::UiContext& ui_;
	model::DiffResult diff_;
	std::vector<model::SideBySideRow> sbs_;
	std::vector<model::UnifiedRow> uni_;
	model::DiffHunkCursor hunks_;
	std::vector<model::DiffStage> stages_;
	ViewMode mode_ = ViewMode::SideBySide;
	int first_ = 0;
	int cursorRow_ = -1;  ///< highlighted row (hunk jumped to / clicked)
	float xScroll_ = 0.f; ///< horizontal scroll of the panes (px)
	int leftDigits_ = 1, rightDigits_ = 1;

	PaneRef leftRef_{}, rightRef_{}, uniRef_{};
	fuse::ui::NodeHandle root_, stageCombo_, modeCombo_, prevBtn_, nextBtn_, exportUBtn_, exportHBtn_, copyBtn_;
	fuse::ui::NodeHandle statsAdded_, statsRemoved_, statsSimilar_;
	fuse::ui::NodeHandle stats_, sbsView_, uniView_, leftLabel_, rightLabel_, leftPane_, rightPane_, uniPane_;
	fuse::ui::NodeHandle fileDlg_;
	bool fileDlgHtml_ = false;
	std::vector<fuse::ui::NodeHandle> tooltips_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_DIFF_PANEL_H
