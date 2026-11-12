/**
 * @file include/retdec/gui/ui/prompt_dialog.h
 * @brief Building blocks of the dialogs in this directory and the small
 *        prompts the Qt GUI made with QInputDialog / QMessageBox /
 *        QFileDialog:
 *
 *   - ModalDialog      — an owned FuseUI modal dialog (hidden until open()).
 *   - MessageDialog    — information / warning box with OK.
 *   - PromptDialog     — one-line text prompt (QInputDialog::getText).
 *   - FilePrompt       — open / save / folder picker over widgets::fileDialog.
 *   - MaxMemoryDialog  — Analysis → Set max memory (bytes)…
 *   - PdbFilePrompt / SignatureFilePrompt — Analysis → Set PDB symbols… /
 *                        Set static-code signature file…
 *
 * Every class owns its nodes (created in the overlay layer, destroyed with the
 * object), opens with show(...) and reports through callbacks; none of them
 * blocks. The main window owns them and wires the callbacks.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_PROMPT_DIALOG_H
#define RETDEC_GUI_UI_PROMPT_DIALOG_H

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

// ── Form helpers shared by the dialogs ───────────────────────────────────────

namespace dlg {

/// A row: fixed-width "Label:" (180 px, as the Qt makeRow) then whatever the
/// caller adds; returns the row (add the editor to it). @p hint, when given,
/// becomes the row's tooltip.
fuse::ui::NodeHandle formRow(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, std::string_view label,
                             std::string_view hint = {});
/// Bold section title followed by a rule (Qt makeSeparator).
fuse::ui::NodeHandle sectionHeader(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, std::string_view title);
/// Dim, wrapping explanatory text.
fuse::ui::NodeHandle noteLabel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, std::string_view text);
/// A horizontal row of buttons aligned to the end (QDialogButtonBox).
fuse::ui::NodeHandle buttonRow(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
/// A horizontal row (gap 6 px, centred).
fuse::ui::NodeHandle hrow(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
/// An expanding line edit.
fuse::ui::NodeHandle growEdit(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, std::string_view text = {});
/// '\n'-joined items (combo boxes, list views).
std::string joinLines(const std::vector<std::string>& items);

/// Current text of a line edit (its edit buffer).
std::string editText(const fuse::ui::UiContext& ui, fuse::ui::NodeHandle edit);
/// Set a line edit's text without emitting TextChanged.
void setEditText(fuse::ui::UiContext& ui, fuse::ui::NodeHandle edit, std::string_view text);
/// Combo / list index (value()), and setting it without emitting.
int indexOf(const fuse::ui::UiContext& ui, fuse::ui::NodeHandle n);
void setIndex(fuse::ui::UiContext& ui, fuse::ui::NodeHandle n, int index);
/// Replace a list view's items (selection cleared).
void setListItems(fuse::ui::UiContext& ui, fuse::ui::NodeHandle list, const std::vector<std::string>& items);
/// A double from a spin box rounded to @p decimals (f32 storage noise removed).
double spinValue(const fuse::ui::UiContext& ui, fuse::ui::NodeHandle spin, int decimals = 2);
/// "Npx" for a CSS length.
std::string px(float v);

/// Registers the dialog style sheet (idempotent; every dialog calls it).
void ensureDialogStyles(fuse::ui::UiContext& ui);

} // namespace dlg

// ── ModalDialog ──────────────────────────────────────────────────────────────

/// A modal dialog card in the overlay layer, sized @p width x @p height
/// (0 = fit the content). Hidden until open(); destroyed with the object.
class ModalDialog {
public:
	ModalDialog(fuse::ui::UiContext& ui, std::string_view title, float width = 0.f, float height = 0.f,
	            std::string_view id = {});
	~ModalDialog();
	ModalDialog(const ModalDialog&) = delete;
	ModalDialog& operator=(const ModalDialog&) = delete;

	fuse::ui::UiContext& ui() const { return ui_; }
	/// The card (widgets::dialog's node).
	fuse::ui::NodeHandle card() const { return card_; }
	/// Where content goes (a flex column filling the card).
	fuse::ui::NodeHandle body() const { return body_; }
	std::string title() const { return title_; }
	void setTitle(std::string_view title);

	void open();
	void close();
	/// False after close() or Escape.
	bool isOpen() const;

private:
	fuse::ui::UiContext& ui_;
	fuse::ui::NodeHandle card_, body_, titleLabel_;
	std::string title_;
	bool open_ = false;
	fuse::ui::u64 openFrame_ = 0;
};

// ── MessageDialog ────────────────────────────────────────────────────────────

/// QMessageBox::information / warning: title, text, OK.
class MessageDialog {
public:
	enum class Kind
	{
		Information,
		Warning,
	};
	explicit MessageDialog(fuse::ui::UiContext& ui);
	~MessageDialog();

	void show(std::string_view title, std::string_view text, Kind kind = Kind::Information,
	          std::function<void()> onClosed = {});
	void close();
	bool isOpen() const { return dlg_.isOpen(); }

	std::string title() const { return dlg_.title(); }
	std::string text() const;
	Kind kind() const { return kind_; }
	fuse::ui::NodeHandle okButton() const { return ok_; }
	fuse::ui::NodeHandle card() const { return dlg_.card(); }

private:
	ModalDialog dlg_;
	fuse::ui::NodeHandle icon_, text_, ok_;
	Kind kind_ = Kind::Information;
	std::function<void()> onClosed_;
};

// ── PromptDialog ─────────────────────────────────────────────────────────────

/// QInputDialog::getText: a label, a line edit, OK / Cancel. Enter accepts.
class PromptDialog {
public:
	explicit PromptDialog(fuse::ui::UiContext& ui);
	~PromptDialog();

	/// Opens with @p initial selected-for-editing; @p onAccepted gets the text
	/// (untrimmed) on OK / Enter; @p onCancelled runs on Cancel.
	void show(std::string_view title, std::string_view label, std::string_view initial,
	          std::function<void(const std::string&)> onAccepted, std::function<void()> onCancelled = {});
	void accept();
	void reject();
	bool isOpen() const { return dlg_.isOpen(); }

	/// An inline error line under the field (empty hides it); the dialog stays open.
	void setError(std::string_view message);
	std::string error() const;

	std::string text() const;
	void setText(std::string_view text);
	std::string title() const { return dlg_.title(); }
	std::string label() const;
	fuse::ui::NodeHandle edit() const { return edit_; }
	fuse::ui::NodeHandle okButton() const { return ok_; }
	fuse::ui::NodeHandle cancelButton() const { return cancel_; }
	fuse::ui::NodeHandle card() const { return dlg_.card(); }

private:
	ModalDialog dlg_;
	fuse::ui::NodeHandle label_, edit_, error_, ok_, cancel_;
	std::function<void(const std::string&)> onAccepted_;
	std::function<void()> onCancelled_;
};

// ── FilePrompt ───────────────────────────────────────────────────────────────

/// QFileDialog::getOpenFileName / getSaveFileName / getExistingDirectory over
/// widgets::fileDialog. The callback runs only when a path was chosen.
class FilePrompt {
public:
	enum class Mode
	{
		Open,
		Save,
		Directory,
	};
	explicit FilePrompt(fuse::ui::UiContext& ui);
	~FilePrompt();

	/// @p start is a directory or a file (its directory is listed and, for
	/// Open / Save, its name pre-filled). @p filter: "*.json;*.txt" (empty =
	/// all files; ignored for Directory).
	void show(Mode mode, std::string_view title, std::string_view start, std::string_view filter,
	          std::function<void(const std::string& path)> onChosen);
	void close();
	bool isOpen() const;
	Mode mode() const { return mode_; }

	/// The underlying widgets::fileDialog node (invalid before show()).
	fuse::ui::NodeHandle dialog() const { return dlg_; }
	/// Directory mode: the "Choose this folder" button.
	fuse::ui::NodeHandle chooseFolderButton() const { return chooseFolder_; }
	std::string directory() const;
	/// Simulates the user accepting @p path (tests, drag and drop): the
	/// callback runs as if it was picked in the dialog.
	void choose(const std::string& path);

private:
	void destroyDialog();

	fuse::ui::UiContext& ui_;
	fuse::ui::NodeHandle dlg_, chooseFolder_;
	Mode mode_ = Mode::Open;
	bool open_ = false;
	fuse::ui::u64 openFrame_ = 0;
	std::function<void(const std::string&)> onChosen_;
};

// ── Analysis-menu prompts ────────────────────────────────────────────────────

/// Analysis → Set max memory (bytes)…: the current limit prefilled ("0" when
/// none); an invalid number shows "Not a valid byte count." (dialog stays
/// open, as the Qt warning box did not apply anything either).
class MaxMemoryDialog {
public:
	explicit MaxMemoryDialog(fuse::ui::UiContext& ui);

	void show(uint64_t currentBytes, std::function<void(uint64_t bytes)> onAccepted);
	bool isOpen() const { return prompt_.isOpen(); }
	PromptDialog& prompt() { return prompt_; }

	/// The status-bar line after accepting ("Next decompile will pass
	/// --max-memory N" / "… omit --max-memory").
	std::function<void(const std::string& message)> onStatusMessage;

private:
	PromptDialog prompt_;
};

/// Analysis → Set PDB symbols… (filter *.pdb).
class PdbFilePrompt {
public:
	explicit PdbFilePrompt(fuse::ui::UiContext& ui);
	void show(std::string_view startDir, std::function<void(const std::string& path)> onChosen);
	FilePrompt& files() { return files_; }
	/// "Next decompile will pass --pdb PATH".
	std::function<void(const std::string& message)> onStatusMessage;

private:
	FilePrompt files_;
};

/// Analysis → Set static-code signature file… (filter *.yar *.yara *.sig).
class SignatureFilePrompt {
public:
	explicit SignatureFilePrompt(fuse::ui::UiContext& ui);
	void show(std::string_view startDir, std::function<void(const std::string& path)> onChosen);
	FilePrompt& files() { return files_; }
	/// "Next decompile will pass --static-code-sigfile PATH".
	std::function<void(const std::string& message)> onStatusMessage;

private:
	FilePrompt files_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_PROMPT_DIALOG_H
