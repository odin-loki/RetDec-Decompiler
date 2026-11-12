/**
 * @file src/gui/ui/prompt_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/prompt_dialog.h"

#include "retdec/gui/model/dlg_input.h"
#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/ui/empty_state.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

// ── dlg helpers ──────────────────────────────────────────────────────────────

namespace dlg {

namespace {

constexpr std::string_view kDialogCss = R"css(
.retdec-dlg-body { flex-grow: 1; gap: 6px; min-height: 0px; }
.retdec-form-row { flex-direction: row; gap: 6px; align-items: center; }
.retdec-form-label { width: 180px; flex-shrink: 0; color: var(--text, #e6e6eb); }
.retdec-section { flex-direction: row; gap: 8px; align-items: center; padding: 8px 0px 4px 0px; }
.retdec-section-title { color: #ffffff; font-size: 15px; }
.retdec-section-rule { flex-grow: 1; height: 1px; background: var(--border, #474955); }
.retdec-note { color: var(--text-dim, #9a9cab); font-size: 13px; }
.retdec-dlg-buttons { flex-direction: row; gap: 6px; align-items: center; justify-content: end; flex-shrink: 0; }
.retdec-hrow { flex-direction: row; gap: 6px; align-items: center; }
.retdec-error { color: #f38ba8; }
.retdec-warning-icon { color: #fab387; font-size: 22px; }
.retdec-info-icon { color: #89b4fa; font-size: 22px; }
.retdec-page { flex-grow: 1; padding: 12px; gap: 6px; min-height: 0px; }
.retdec-chat-user { color: #44aaff; }
.retdec-chat-assistant { color: #aaffaa; }
.retdec-chat-system { color: #888888; }
)css";

} // namespace

void ensureDialogStyles(UiContext& ui)
{
	ensureRetdecUiClasses(ui);
	// One sheet per context: tag the root once it is added.
	if (ui.hasClass(ui.root(), "retdec-dlg-styled")) return;
	ui.addClass(ui.root(), "retdec-dlg-styled");
	(void)ui.addStyleSheet(kDialogCss);
}

std::string px(float v)
{
	char b[32];
	std::snprintf(b, sizeof b, "%gpx", double(v));
	return b;
}

NodeHandle hrow(UiContext& ui, NodeHandle parent)
{
	const NodeHandle r = w::panel(ui, parent);
	ui.addClass(r, "retdec-hrow");
	ui.setCss(r, "flex-direction", "row");
	ui.setCss(r, "align-items", "center");
	ui.setCss(r, "gap", "6px");
	return r;
}

NodeHandle formRow(UiContext& ui, NodeHandle parent, std::string_view label, std::string_view hint)
{
	const NodeHandle r = hrow(ui, parent);
	ui.addClass(r, "retdec-form-row");
	std::string text(label);
	if (!text.empty() && text.back() != ':') text += ':';
	const NodeHandle l = w::label(ui, r, text);
	ui.addClass(l, "retdec-form-label");
	ui.setCss(l, "width", "180px");
	ui.setCss(l, "flex-shrink", "0");
	if (!hint.empty()) w::setTooltip(ui, l, hint);
	return r;
}

NodeHandle sectionHeader(UiContext& ui, NodeHandle parent, std::string_view title)
{
	const NodeHandle r = w::panel(ui, parent);
	ui.addClass(r, "retdec-section");
	ui.setCss(r, "flex-direction", "row");
	ui.setCss(r, "align-items", "center");
	const NodeHandle t = w::label(ui, r, title);
	ui.addClass(t, "retdec-section-title");
	const NodeHandle rule = w::panel(ui, r);
	ui.addClass(rule, "retdec-section-rule");
	return r;
}

NodeHandle noteLabel(UiContext& ui, NodeHandle parent, std::string_view text)
{
	const NodeHandle l = w::label(ui, parent, text);
	ui.addClass(l, "retdec-note");
	return l;
}

NodeHandle buttonRow(UiContext& ui, NodeHandle parent)
{
	const NodeHandle r = w::panel(ui, parent);
	ui.addClass(r, "retdec-dlg-buttons");
	ui.setCss(r, "flex-direction", "row");
	ui.setCss(r, "justify-content", "end");
	ui.setCss(r, "align-items", "center");
	ui.setCss(r, "flex-shrink", "0");
	return r;
}

NodeHandle growEdit(UiContext& ui, NodeHandle parent, std::string_view text)
{
	const NodeHandle e = w::lineEdit(ui, parent, text);
	ui.setCss(e, "flex-grow", "1");
	ui.setCss(e, "min-width", "60px");
	return e;
}

std::string joinLines(const std::vector<std::string>& items)
{
	std::string out;
	for (std::size_t i = 0; i < items.size(); ++i)
	{
		if (i) out += '\n';
		out += items[i];
	}
	return out;
}

std::string editText(const UiContext& ui, NodeHandle edit)
{
	if (!ui.alive(edit)) return {};
	return std::string(ui.text(edit));
}

void setEditText(UiContext& ui, NodeHandle edit, std::string_view text)
{
	setLineEditText(ui, edit, text);
	if (ui.alive(edit)) ui.editBuffer(edit).end(false);
}

int indexOf(const UiContext& ui, NodeHandle n)
{
	return ui.alive(n) ? int(std::lround(ui.value(n))) : -1;
}

void setIndex(UiContext& ui, NodeHandle n, int index)
{
	ui.setValue(n, float(index), false);
}

void setListItems(UiContext& ui, NodeHandle list, const std::vector<std::string>& items)
{
	ui.setText(list, joinLines(items));
	ui.setValue(list, -1.f);
}

double spinValue(const UiContext& ui, NodeHandle spin, int decimals)
{
	const double scale = std::pow(10.0, decimals);
	return std::round(double(ui.value(spin)) * scale) / scale;
}

} // namespace dlg

// ── ModalDialog ──────────────────────────────────────────────────────────────

ModalDialog::ModalDialog(UiContext& ui, std::string_view title, float width, float height, std::string_view id):
	ui_(ui), title_(title)
{
	dlg::ensureDialogStyles(ui);
	card_ = w::dialog(ui, title, id);
	titleLabel_ = ui.firstChild(card_);
	ui.setCss(titleLabel_, "flex-shrink", "0");
	body_ = w::dialogBody(ui, card_);
	ui.addClass(body_, "retdec-dlg-body");
	ui.setCss(body_, "flex-grow", "1");
	ui.setCss(body_, "gap", "6px");
	ui.setCss(body_, "min-height", "0px");
	if (width > 0.f) ui.setCss(card_, "width", dlg::px(width));
	if (height > 0.f) ui.setCss(card_, "height", dlg::px(height));
}

ModalDialog::~ModalDialog()
{
	if (!ui_.alive(card_)) return;
	if (isOpen()) w::closeDialog(ui_, card_);
	const NodeHandle scope = ui_.parent(card_);
	ui_.destroy(ui_.alive(scope) ? scope : card_);
}

void ModalDialog::setTitle(std::string_view title)
{
	title_ = title;
	ui_.setText(titleLabel_, title);
	ui_.setA11y(card_, A11yRole::Window, title);
}

void ModalDialog::open()
{
	w::openDialog(ui_, card_);
	open_ = true;
	openFrame_ = ui_.frameIndex();
}

void ModalDialog::close()
{
	if (isOpen()) w::closeDialog(ui_, card_);
	open_ = false;
}

bool ModalDialog::isOpen() const
{
	const NodeHandle scope = ui_.parent(card_);
	if (!open_ || !ui_.alive(scope)) return false;
	// Styles resolve on the next update: right after open() the scope is not
	// computed visible yet. Escape closes the scope behind our back, which
	// shows as "not visible" once the frame has been styled.
	return ui_.frameIndex() == openFrame_ || ui_.computed(scope).visible;
}

// ── MessageDialog ────────────────────────────────────────────────────────────

MessageDialog::MessageDialog(UiContext& ui): dlg_(ui, "", 440.f, 0.f)
{
	const NodeHandle row = dlg::hrow(ui, dlg_.body());
	ui.setCss(row, "align-items", "start");
	icon_ = w::label(ui, row, "\xE2\x84\xB9"); // ℹ
	text_ = w::label(ui, row, "");
	ui.setCss(text_, "flex-grow", "1");
	ui.setCss(text_, "flex-shrink", "1");
	ui.setCss(text_, "min-width", "0px");
	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	ok_ = w::button(ui, buttons, "OK");
	ui.addClass(ok_, "primary");
	ui.connect(ok_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { close(); });
}

MessageDialog::~MessageDialog() = default;

void MessageDialog::show(std::string_view title, std::string_view text, Kind kind, std::function<void()> onClosed)
{
	UiContext& ui = dlg_.ui();
	kind_ = kind;
	onClosed_ = std::move(onClosed);
	dlg_.setTitle(title);
	ui.setText(text_, text);
	ui.setText(icon_, kind == Kind::Warning ? "\xE2\x9A\xA0" : "\xE2\x84\xB9"); // ⚠ ℹ
	ui.removeClass(icon_, "retdec-warning-icon");
	ui.removeClass(icon_, "retdec-info-icon");
	ui.addClass(icon_, kind == Kind::Warning ? "retdec-warning-icon" : "retdec-info-icon");
	dlg_.open();
	ui.setFocus(ok_, FocusReason::Keyboard);
}

void MessageDialog::close()
{
	dlg_.close();
	if (auto cb = std::move(onClosed_))
	{
		onClosed_ = nullptr;
		cb();
	}
}

std::string MessageDialog::text() const
{
	return std::string(dlg_.ui().text(text_));
}

// ── PromptDialog ─────────────────────────────────────────────────────────────

PromptDialog::PromptDialog(UiContext& ui): dlg_(ui, "", 420.f, 0.f)
{
	label_ = w::label(ui, dlg_.body(), "");
	edit_ = w::lineEdit(ui, dlg_.body(), "");
	error_ = w::label(ui, dlg_.body(), "");
	ui.addClass(error_, "retdec-error");
	setShown(ui, error_, false);
	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	ok_ = w::button(ui, buttons, "OK");
	ui.addClass(ok_, "primary");
	cancel_ = w::button(ui, buttons, "Cancel");
	ui.connect(ok_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { accept(); });
	ui.connect(cancel_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { reject(); });
	ui.connect(edit_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs&) { accept(); });
	ui.connect(edit_, SignalKind::TextChanged, [this](UiContext&, NodeHandle, const SignalArgs&) { setError({}); });
}

PromptDialog::~PromptDialog() = default;

void PromptDialog::show(
	std::string_view title,
	std::string_view label,
	std::string_view initial,
	std::function<void(const std::string&)> onAccepted,
	std::function<void()> onCancelled)
{
	UiContext& ui = dlg_.ui();
	onAccepted_ = std::move(onAccepted);
	onCancelled_ = std::move(onCancelled);
	dlg_.setTitle(title);
	ui.setText(label_, label);
	dlg::setEditText(ui, edit_, initial);
	ui.editBuffer(edit_).selectAll();
	setError({});
	dlg_.open();
	ui.setFocus(edit_, FocusReason::Keyboard);
}

void PromptDialog::accept()
{
	if (!isOpen()) return;
	const std::string t = text();
	// The callback may keep the dialog open by setting an error.
	if (onAccepted_) onAccepted_(t);
	if (error().empty()) dlg_.close();
}

void PromptDialog::reject()
{
	dlg_.close();
	if (onCancelled_) onCancelled_();
}

void PromptDialog::setError(std::string_view message)
{
	UiContext& ui = dlg_.ui();
	ui.setText(error_, message);
	setShown(ui, error_, !message.empty());
}

std::string PromptDialog::error() const
{
	return std::string(dlg_.ui().text(error_));
}

std::string PromptDialog::text() const
{
	return dlg::editText(dlg_.ui(), edit_);
}

void PromptDialog::setText(std::string_view text)
{
	dlg::setEditText(dlg_.ui(), edit_, text);
}

std::string PromptDialog::label() const
{
	return std::string(dlg_.ui().text(label_));
}

// ── FilePrompt ───────────────────────────────────────────────────────────────

namespace {

void destroyNodeCmd(UiContext& ui, NodeHandle n, u64, u64)
{
	if (ui.alive(n)) ui.destroy(n);
}

/// The file-name field of a widgets::fileDialog (body: top row, list, name row, buttons).
NodeHandle fileDialogNameEdit(const UiContext& ui, NodeHandle dlg)
{
	const NodeHandle body = w::dialogBody(ui, dlg);
	NodeHandle row = ui.firstChild(body);
	for (int i = 0; i < 2 && ui.alive(row); ++i)
		row = ui.nextSibling(row);
	if (!ui.alive(row)) return {};
	const NodeHandle lbl = ui.firstChild(row);
	return ui.alive(lbl) ? ui.nextSibling(lbl) : NodeHandle{};
}

NodeHandle fileDialogButtons(const UiContext& ui, NodeHandle dlg)
{
	const NodeHandle body = w::dialogBody(ui, dlg);
	NodeHandle last;
	for (NodeHandle c = ui.firstChild(body); ui.alive(c); c = ui.nextSibling(c))
		last = c;
	return last;
}

} // namespace

FilePrompt::FilePrompt(UiContext& ui): ui_(ui)
{
	dlg::ensureDialogStyles(ui);
}

FilePrompt::~FilePrompt()
{
	if (!ui_.alive(dlg_)) return;
	if (isOpen()) w::closeDialog(ui_, dlg_);
	const NodeHandle scope = ui_.parent(dlg_);
	ui_.destroy(ui_.alive(scope) ? scope : dlg_);
}

void FilePrompt::destroyDialog()
{
	if (!ui_.alive(dlg_)) return;
	if (isOpen()) w::closeDialog(ui_, dlg_);
	open_ = false;
	const NodeHandle scope = ui_.parent(dlg_);
	// Deferred: show() may run from inside the old dialog's own signal.
	ui_.post(&destroyNodeCmd, ui_.alive(scope) ? scope : dlg_);
	dlg_ = {};
	chooseFolder_ = {};
}

void FilePrompt::show(
	Mode mode,
	std::string_view title,
	std::string_view start,
	std::string_view filter,
	std::function<void(const std::string&)> onChosen)
{
	namespace fs = std::filesystem;
	destroyDialog();
	mode_ = mode;
	onChosen_ = std::move(onChosen);

	std::error_code ec;
	fs::path dir = fs::path(std::string(start));
	std::string name;
	if (!dir.empty() && !fs::is_directory(dir, ec))
	{
		if (mode != Mode::Directory) name = dir.filename().string();
		dir = dir.parent_path();
	}
	if (dir.empty() || !fs::is_directory(dir, ec)) dir = fs::current_path(ec);

	// Directory mode lists folders only ('/' never occurs in a file name).
	const std::string_view flt = mode == Mode::Directory ? std::string_view("/") : filter;
	dlg_ = w::fileDialog(ui_, title, dir.string(), flt, mode == Mode::Save);
	if (!ui_.alive(dlg_)) return;
	if (!name.empty())
		if (const NodeHandle nameEdit = fileDialogNameEdit(ui_, dlg_); ui_.alive(nameEdit))
			dlg::setEditText(ui_, nameEdit, name);
	if (mode == Mode::Directory)
	{
		const NodeHandle buttons = fileDialogButtons(ui_, dlg_);
		chooseFolder_ = w::button(ui_, buttons, "Choose this folder");
		ui_.moveTo(chooseFolder_, buttons, ui_.firstChild(buttons));
		ui_.connect(chooseFolder_, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
			const std::string d = w::fileDialogDirectory(u, dlg_);
			w::closeDialog(u, dlg_);
			open_ = false;
			if (onChosen_ && !d.empty()) onChosen_(d);
		});
	}
	ui_.connect(dlg_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		open_ = false;
		if (a.text.empty() || !onChosen_) return;
		const std::string path(a.text);
		onChosen_(path);
	});
	w::openDialog(ui_, dlg_);
	open_ = true;
	openFrame_ = ui_.frameIndex();
}

void FilePrompt::close()
{
	if (isOpen()) w::closeDialog(ui_, dlg_);
	open_ = false;
}

bool FilePrompt::isOpen() const
{
	const NodeHandle scope = ui_.parent(dlg_);
	if (!open_ || !ui_.alive(scope)) return false;
	return ui_.frameIndex() == openFrame_ || ui_.computed(scope).visible;
}

std::string FilePrompt::directory() const
{
	return ui_.alive(dlg_) ? w::fileDialogDirectory(ui_, dlg_) : std::string();
}

void FilePrompt::choose(const std::string& path)
{
	close();
	if (onChosen_ && !path.empty()) onChosen_(path);
}

// ── Analysis-menu prompts ────────────────────────────────────────────────────

MaxMemoryDialog::MaxMemoryDialog(UiContext& ui): prompt_(ui) {}

void MaxMemoryDialog::show(uint64_t currentBytes, std::function<void(uint64_t)> onAccepted)
{
	prompt_.show(
		"Max memory",
		"Limit decompiler memory in bytes (0 = omit --max-memory):",
		model::dlgFormatMaxMemory(currentBytes),
		[this, cb = std::move(onAccepted)](const std::string& text) {
			const auto bytes = model::dlgParseMaxMemory(text);
			if (!bytes)
			{
				prompt_.setError(bytes.error());
				return;
			}
			if (cb) cb(*bytes);
			if (onStatusMessage) onStatusMessage(model::dlgMaxMemoryStatus(*bytes));
		});
}

PdbFilePrompt::PdbFilePrompt(UiContext& ui): files_(ui) {}

void PdbFilePrompt::show(std::string_view startDir, std::function<void(const std::string&)> onChosen)
{
	files_.show(
		FilePrompt::Mode::Open,
		"PDB symbols",
		startDir,
		model::kPdbFileFilter,
		[this, cb = std::move(onChosen)](const std::string& path) {
			if (cb) cb(path);
			if (onStatusMessage) onStatusMessage(model::pdbPathStatus(path));
		});
}

SignatureFilePrompt::SignatureFilePrompt(UiContext& ui): files_(ui) {}

void SignatureFilePrompt::show(std::string_view startDir, std::function<void(const std::string&)> onChosen)
{
	files_.show(
		FilePrompt::Mode::Open,
		"Static-code signatures",
		startDir,
		model::kSignatureFileFilter,
		[this, cb = std::move(onChosen)](const std::string& path) {
			if (cb) cb(path);
			if (onStatusMessage) onStatusMessage(model::staticSigFileStatus(path));
		});
}

} // namespace retdec::gui::ui
