/**
 * @file include/retdec/gui/ui/diagnostics_panel.h
 * @brief DiagnosticsPanel — the Problems list (model::Diagnostic).
 *
 * "Show:" severity filter (All / Info / Warnings / Errors), a text filter that
 * replaces the severity filter while non-empty, Copy / Save… of the visible
 * rows as TSV, "N entries" count, severity-coloured rows (info green, warning
 * yellow, error red). Double-click or Enter on a row navigates: its address
 * when it has one, else its function. Retained entries are capped (10 000,
 * oldest evicted).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_DIAGNOSTICS_PANEL_H
#define RETDEC_GUI_UI_DIAGNOSTICS_PANEL_H

#include "retdec/gui/model/console_diagnostics.h"
#include "retdec/gui/model/types.h"
#include "retdec/gui/ui/empty_state.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class DiagnosticsPanel {
public:
	DiagnosticsPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~DiagnosticsPanel();
	DiagnosticsPanel(const DiagnosticsPanel&) = delete;
	DiagnosticsPanel& operator=(const DiagnosticsPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string panelTitle() const
	{
		return "Diagnostics";
	}

	// ── Data in ──────────────────────────────────────────────────────────────
	void addDiagnostic(model::Diagnostic d);
	/// Qt-style convenience: @p stage becomes the diagnostic's source.
	void addMessage(model::Severity severity, std::string_view stage, std::string_view message, uint64_t address = 0);
	void setDiagnostics(std::vector<model::Diagnostic> all);
	void clear();
	/// Cap on retained entries (minimum 100).
	void setMaxEntries(int n);
	int maxEntries() const
	{
		return int(store_.maxEntries());
	}

	// ── Filtering / export ───────────────────────────────────────────────────
	void setSeverityFilter(model::SeverityFilter f);
	model::SeverityFilter severityFilter() const
	{
		return filter_;
	}
	void setTextFilter(std::string_view text);
	/// Visible rows as TSV ("Severity\tStage\tMessage").
	std::string visibleTsv() const;
	void copyVisible();
	bool saveVisibleTo(const std::filesystem::path& path);
	/// Opens a save dialog.
	void saveVisible();
	/// Activates visible row @p row (double-click / Enter).
	void activateRow(int row);

	// ── Events out ───────────────────────────────────────────────────────────
	std::function<void(uint64_t address)> onAddressActivated;
	std::function<void(const std::string& function)> onFunctionActivated;
	/// An Error-severity diagnostic was added (not warnings / info).
	std::function<void()> onErrorAdded;

	// ── Test hooks ───────────────────────────────────────────────────────────
	int totalCount() const
	{
		return int(store_.size());
	}
	int visibleCount() const
	{
		return int(rows_.size());
	}
	const model::Diagnostic& visibleAt(int row) const
	{
		return store_.at(rows_[std::size_t(row)]);
	}
	/// Cell text as the table shows it.
	std::string cellText(int row, int col) const;
	std::string countText() const;
	fuse::ui::NodeHandle table() const
	{
		return table_;
	}
	fuse::ui::NodeHandle filterCombo() const
	{
		return combo_;
	}
	fuse::ui::NodeHandle searchEdit() const
	{
		return search_;
	}
	fuse::ui::NodeHandle copyButton() const
	{
		return copyBtn_;
	}
	fuse::ui::NodeHandle saveButton() const
	{
		return saveBtn_;
	}

private:
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);
	void refilter();
	void updateCount();

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, combo_, search_, copyBtn_, saveBtn_, count_, table_, saveDlg_;
	model::DiagnosticsStore store_;
	std::vector<std::size_t> rows_;
	model::SeverityFilter filter_ = model::SeverityFilter::All;
	std::string text_;
	fuse::ui::widgets::ItemModel model_;
	fuse::ui::widgets::CellDelegate delegate_;
	int sortCol_ = -1;
	bool sortAsc_ = true;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_DIAGNOSTICS_PANEL_H
