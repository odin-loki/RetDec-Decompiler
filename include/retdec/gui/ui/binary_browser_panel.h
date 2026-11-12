/**
 * @file include/retdec/gui/ui/binary_browser_panel.h
 * @brief BinaryBrowserPanel — section explorer + hex dump.
 *
 * Top: the binary's sections from retdec-fileinfo (Name / Address / Size; the
 * file name as the root row, sections indented under it). Bottom: hex + ASCII
 * dump of the selected section (first 64 KiB). Double-click / Enter on a row
 * navigates to its address. Context menu: Decompile this range…, Copy start
 * address, Copy VMA range, Copy hex snippet.
 *
 * File bytes are read through model::readFileRange (inspect_model).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_BINARY_BROWSER_PANEL_H
#define RETDEC_GUI_UI_BINARY_BROWSER_PANEL_H

#include "retdec/gui/model/inspect_model.h"
#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class BinaryBrowserPanel {
public:
	BinaryBrowserPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~BinaryBrowserPanel();
	BinaryBrowserPanel(const BinaryBrowserPanel&) = delete;
	BinaryBrowserPanel& operator=(const BinaryBrowserPanel&) = delete;

	fuse::ui::NodeHandle root() const { return root_; }
	std::string panelTitle() const { return "Binary Browser"; }

	/// New binary: shows a "Run Inspect or wait for fileinfo…" hint row.
	void loadBinary(std::string_view path);
	/// Fills the section tree from fileinfo JSON (ignored before loadBinary).
	void populateFromFileinfo(std::string_view fileinfoJson);
	/// Same with already-parsed sections.
	void setSections(const std::vector<model::SectionInfo>& sections);
	void clear();

	/// Selects table row @p row and shows its hex dump.
	void selectRow(int row);
	/// Double-click / Enter: navigates to the row's address (non-zero only).
	void activateRow(int row);
	/// Opens the section context menu for @p row at @p at.
	void openContextMenu(fuse::ui::Vec2 at, int row);

	/// Address of the double-clicked / activated row.
	std::function<void(uint64_t address)> onAddressActivated;
	/// "0xSTART-0xEND" for retdec-decompiler --select-ranges.
	std::function<void(const std::string& range)> onDecompileRangeRequested;

	// Test hooks.
	int rowCount() const { return int(rows_.size()); }
	std::string cellText(int row, int col) const;
	std::string hexHeaderText() const;
	std::string hexText() const { return hex_->text(); }
	std::string binaryPath() const { return binaryPath_; }
	fuse::ui::NodeHandle table() const { return table_; }
	CodeView& hexView() { return *hex_; }
	ContextMenu& contextMenu() { return menu_; }

private:
	struct Row
	{
		std::string name, address, size;
		uint64_t offset = 0;
		bool section = false; ///< selectable section row (has an offset)
	};
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);
	void showHexForRow(int row);
	void setRows(std::vector<Row> rows);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	ContextMenu menu_;
	fuse::ui::NodeHandle root_, table_, hexHeader_;
	std::unique_ptr<CodeView> hex_;
	fuse::ui::widgets::ItemModel model_;
	std::vector<Row> rows_;
	std::string binaryPath_;
	fuse::ui::Vec2 menuPos_;
	fuse::ui::TimerHandle menuTimer_;
	bool menuPending_ = false;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_BINARY_BROWSER_PANEL_H
