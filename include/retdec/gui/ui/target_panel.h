/**
 * @file include/retdec/gui/ui/target_panel.h
 * @brief TargetPanel — the project's arch / OS / entry-point overrides (the
 *        binary path read-only) and the global decompiler --config file.
 *
 *   Binary        /path/to/file.exe
 *   Architecture  [x86, arm, mips (optional)]      → -a for retdec-decompiler
 *   OS (metadata) [linux, windows — saved to project]
 *   Entry point   [0x401000 or decimal; empty = 0]
 *   [Apply to project]
 *   Decompiler config (--config)  [read-only path]
 *   [Browse…] [Use bundled] [Clear] [Edit JSON…]
 *
 * Apply (button, Enter in a field, Alt+A) writes the fields into the project
 * given to setFromProject() (model::applyTargetFields) and reports "Target
 * fields applied to project (save .retdec to persist)."; an invalid entry
 * point warns "Invalid entry point (use 0x… hex or decimal).". The config
 * buttons (Alt+B / Alt+U / Alt+C / Alt+E) change AppSettings
 * decompiler.extraConfigPath, save and notify; they work without a project.
 * Edit JSON… opens the file in an editor dialog that refuses to save
 * invalid JSON.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_TARGET_PANEL_H
#define RETDEC_GUI_UI_TARGET_PANEL_H

#include "retdec/gui/model/project_file.h"
#include "retdec/gui/model/settings.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace retdec::gui::ui {

/// The JSON editor behind "Edit JSON…": a code editor, Save / Cancel.
class DecompilerConfigEditor {
public:
	explicit DecompilerConfigEditor(fuse::ui::UiContext& ui);

	/// Load @p file and open; false (and a warning "Cannot open:\nPATH") when
	/// it cannot be read.
	bool show(const std::filesystem::path& file, std::function<void()> onSaved = {});
	/// Validate and write; false (dialog stays open, warning shown) on invalid
	/// JSON ("Invalid JSON") or a write error.
	bool save();
	void cancel();
	bool isOpen() const
	{
		return dlg_.isOpen();
	}

	std::string text() const;
	void setText(std::string_view text);
	std::string title() const
	{
		return dlg_.title();
	}
	fuse::ui::NodeHandle editor() const
	{
		return edit_;
	}
	fuse::ui::NodeHandle saveButton() const
	{
		return save_;
	}
	fuse::ui::NodeHandle cancelButton() const
	{
		return cancel_;
	}
	MessageDialog& message()
	{
		return message_;
	}

	std::function<void(const std::string& title, const std::string& message)> onWarning;

private:
	void warn(const std::string& title, const std::string& text);

	ModalDialog dlg_;
	MessageDialog message_;
	fuse::ui::NodeHandle edit_, save_, cancel_;
	std::filesystem::path file_;
	std::function<void()> onSaved_;
};

class TargetPanel {
public:
	TargetPanel(
		fuse::ui::UiContext& ui,
		fuse::ui::NodeHandle parent,
		model::AppSettings& settings = model::AppSettings::instance());
	~TargetPanel();
	TargetPanel(const TargetPanel&) = delete;
	TargetPanel& operator=(const TargetPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string panelTitle() const
	{
		return "Target";
	}

	/// Show @p pf's fields and make Apply write to it (not owned; nullptr = clear()).
	void setFromProject(model::ProjectFile* pf);
	/// No project: "—", empty fields, Apply disabled.
	void clear();
	/// Refresh the --config display from AppSettings (after Settings / Analysis → Configure…).
	void syncDecompilerConfigFromAppSettings();

	std::string archText() const;
	std::string osText() const;
	std::string entryText() const;
	std::string binaryText() const;
	std::string decompilerConfigText() const;
	bool fieldsEnabled() const;

	// ── Actions (buttons / shortcuts) ────────────────────────────────────────
	/// Apply to the project; true when the project was updated.
	bool apply();
	/// Browse…: opens the JSON file prompt (starts at the configured file or the binary's folder).
	void browseDecompilerConfig();
	/// The prompt's result (also callable directly).
	void setDecompilerConfig(const std::filesystem::path& path);
	void useBundledDecompilerConfig();
	void clearDecompilerConfig();
	/// Edit JSON…: no-op unless the configured file exists.
	void editDecompilerConfig();

	// ── Events ───────────────────────────────────────────────────────────────
	/// Apply was requested (before the project is updated).
	std::function<void()> onApplyRequested;
	/// The project was updated by Apply.
	std::function<void()> onApplied;
	std::function<void(const std::string& message, int timeoutMs)> onStatusMessage;
	/// A warning / information box was shown (title, text).
	std::function<void(const std::string& title, const std::string& message)> onWarning;

	// ── Test hooks ───────────────────────────────────────────────────────────
	fuse::ui::NodeHandle archEdit() const
	{
		return arch_;
	}
	fuse::ui::NodeHandle osEdit() const
	{
		return os_;
	}
	fuse::ui::NodeHandle entryEdit() const
	{
		return entry_;
	}
	fuse::ui::NodeHandle applyButton() const
	{
		return apply_;
	}
	fuse::ui::NodeHandle browseButton() const
	{
		return browse_;
	}
	fuse::ui::NodeHandle bundledButton() const
	{
		return bundled_;
	}
	fuse::ui::NodeHandle clearButton() const
	{
		return clearCfg_;
	}
	fuse::ui::NodeHandle editButton() const
	{
		return editCfg_;
	}
	FilePrompt& filePrompt()
	{
		return files_;
	}
	MessageDialog& message()
	{
		return message_;
	}
	DecompilerConfigEditor& configEditor()
	{
		return editor_;
	}

private:
	void setFieldsEnabled(bool on);
	void updateConfigButtons();
	void warn(const std::string& title, const std::string& text, MessageDialog::Kind kind);
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);

	fuse::ui::UiContext& ui_;
	model::AppSettings& settings_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, hint_, binary_, arch_, os_, entry_, apply_, config_;
	fuse::ui::NodeHandle browse_, bundled_, clearCfg_, editCfg_;
	model::ProjectFile* project_ = nullptr;
	MessageDialog message_;
	FilePrompt files_;
	DecompilerConfigEditor editor_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_TARGET_PANEL_H
