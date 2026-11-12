/**
 * @file include/retdec/gui/ui/ir_panel.h
 * @brief IRPanel — LLVM IR / SSA viewer with a stage switch.
 *
 * "Stage:" combo: Function (the selection's IR) / Module (the whole .ll
 * sidecar); each stage keeps its own text and switching shows the other.
 * LLVM IR highlighting (instructions, types, %values / @globals, block
 * labels, comments), line numbers, Ctrl+F find bar (F3 / Shift+F3, Escape),
 * "No IR loaded" empty state.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_IR_PANEL_H
#define RETDEC_GUI_UI_IR_PANEL_H

#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace retdec::gui::ui {

class IRPanel {
public:
	enum class Stage
	{
		Function = 0,
		Module = 1,
	};

	IRPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~IRPanel();
	IRPanel(const IRPanel&) = delete;
	IRPanel& operator=(const IRPanel&) = delete;

	fuse::ui::NodeHandle root() const { return root_; }
	std::string panelTitle() const { return "IR (SSA)"; }

	/// Stores @p text as the IR of @p stage ("Module" -> module, anything else
	/// -> function), shows it, and selects the stage in the combo when it
	/// names one ("Function" / "Module"; e.g. "SSA" leaves the combo alone).
	void setIRText(std::string_view text, std::string_view stage = "SSA");
	void setStage(Stage s);
	Stage stage() const { return stage_; }
	std::string functionIr() const { return functionIr_; }
	std::string moduleIr() const { return moduleIr_; }
	void onFunctionSelected(uint64_t address, std::string_view name);
	void applyEditorFont(float px);
	void clear();

	void showFindBar();
	void hideFindBar();
	bool findBarVisible() const { return findShown_; }
	void setFindText(std::string_view text);
	void findNext();
	void findPrevious();

	std::function<void(const std::string& msg, int timeoutMs)> onStatusMessage;
	/// Context menu "Go to Function" on a line with an address.
	std::function<void(uint64_t address)> onAddressActivated;

	// Test hooks.
	CodeView& view() { return *view_; }
	EmptyState& emptyState() { return *empty_; }
	bool emptyStateShown() const { return emptyShown_; }
	std::string functionLabel() const;
	fuse::ui::NodeHandle stageCombo() const { return stageCombo_; }
	fuse::ui::NodeHandle findEdit() const { return findEdit_; }
	ContextMenu& contextMenu() { return menu_; }

private:
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);
	void onStageChanged(int index);
	void updateEmptyState();

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	ContextMenu menu_;
	fuse::ui::NodeHandle root_, funcLabel_, stageLabel_, stageCombo_, body_, findBar_, findEdit_;
	std::unique_ptr<EmptyState> empty_;
	std::unique_ptr<CodeView> view_;
	std::string functionIr_, moduleIr_;
	Stage stage_ = Stage::Function;
	bool findShown_ = false;
	bool emptyShown_ = true;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_IR_PANEL_H
