/**
 * @file src/gui/ui/target_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/target_panel.h"

#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/model/dlg_input.h"
#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/model/dlg_target.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

// ── DecompilerConfigEditor ───────────────────────────────────────────────────

DecompilerConfigEditor::DecompilerConfigEditor(UiContext& ui)
	: dlg_(ui, "Edit", 720.f, 520.f, "decompilerConfigEditor"), message_(ui)
{
	edit_ = w::codeEdit(ui, dlg_.body(), "", w::CodeLanguage::Plain);
	ui.setCss(edit_, "flex-grow", "1");
	ui.setCss(edit_, "min-height", "0px");
	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	save_ = w::button(ui, buttons, "Save");
	ui.addClass(save_, "primary");
	cancel_ = w::button(ui, buttons, "Cancel");
	ui.connect(save_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { save(); });
	ui.connect(cancel_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { cancel(); });
}

void DecompilerConfigEditor::warn(const std::string& title, const std::string& text)
{
	message_.show(title, text, MessageDialog::Kind::Warning);
	if (onWarning) onWarning(title, text);
}

bool DecompilerConfigEditor::show(const std::filesystem::path& file, std::function<void()> onSaved)
{
	auto text = model::readTextFile(file);
	if (!text)
	{
		warn("Decompiler config", text.error());
		return false;
	}
	file_ = file;
	onSaved_ = std::move(onSaved);
	dlg_.setTitle("Edit — " + file.filename().string());
	w::codeEditSetText(dlg_.ui(), edit_, *text);
	dlg_.open();
	dlg_.ui().setFocus(edit_, FocusReason::Keyboard);
	return true;
}

bool DecompilerConfigEditor::save()
{
	const std::string t = text();
	if (auto ok = model::validateJsonText(t); !ok)
	{
		warn("Invalid JSON", ok.error());
		return false;
	}
	if (auto wr = model::writeTextFile(file_, t); !wr)
	{
		warn("Decompiler config", wr.error());
		return false;
	}
	dlg_.close();
	if (onSaved_) onSaved_();
	return true;
}

void DecompilerConfigEditor::cancel()
{
	dlg_.close();
}

std::string DecompilerConfigEditor::text() const
{
	return w::codeEditText(dlg_.ui(), edit_);
}

void DecompilerConfigEditor::setText(std::string_view text)
{
	w::codeEditSetText(dlg_.ui(), edit_, text);
}

// ── TargetPanel ──────────────────────────────────────────────────────────────

TargetPanel::TargetPanel(UiContext& ui, NodeHandle parent, model::AppSettings& settings)
	: ui_(ui), settings_(settings), message_(ui), files_(ui), editor_(ui)
{
	dlg::ensureDialogStyles(ui);
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-target-panel");
	const NodeHandle body = w::scrollArea(ui, root_);
	ui.setCss(body, "flex-grow", "1");
	ui.setCss(body, "padding", "6px");
	ui.setCss(body, "gap", "4px");

	hint_ = dlg::noteLabel(
		ui, body,
		"Overrides live in the .retdec project. Non-empty Architecture is passed as -a to retdec-decompiler. Optional "
		"Decompiler JSON sets --config (same as Analysis -> Configure…). Enter in a target field or Alt+A applies.");

	w::label(ui, body, "Binary");
	binary_ = w::label(ui, body, "—");
	ui.setCss(binary_, "color", "#ffffff");

	auto field = [&](std::string_view label, std::string_view placeholder) {
		w::label(ui, body, label);
		const NodeHandle e = w::lineEdit(ui, body, "");
		w::setTooltip(ui, e, placeholder);
		dlg::noteLabel(ui, body, placeholder);
		ui.connect(e, SignalKind::Submitted, [this](UiContext& u, NodeHandle, const SignalArgs&) {
			if (!u.disabled(apply_)) apply();
		});
		return e;
	};
	arch_ = field("Architecture", "e.g. x86, arm, mips (optional)");
	os_ = field("OS (metadata)", "e.g. linux, windows — saved to project");
	entry_ = field("Entry point", "0x401000 or decimal; empty = 0");

	const NodeHandle applyRow = dlg::hrow(ui, body);
	apply_ = w::button(ui, applyRow, "Apply to project");
	w::setTooltip(ui, apply_, "Write fields into the in-memory project (mark modified). Alt+A.");

	w::label(ui, body, "Decompiler config (--config)");
	config_ = w::lineEdit(ui, body, "");
	ui.setFocusable(config_, false); // read-only display
	dlg::noteLabel(ui, body, "None — decompiler loads built-in default next to binary");
	const NodeHandle cfgRow = dlg::hrow(ui, body);
	browse_ = w::button(ui, cfgRow, "Browse…");
	w::setTooltip(ui, browse_, "Pick decompiler JSON. Alt+B.");
	bundled_ = w::button(ui, cfgRow, "Use bundled");
	w::setTooltip(ui, bundled_, "share/retdec/decompiler-config.json next to retdec-decompiler. Alt+U.");
	clearCfg_ = w::button(ui, cfgRow, "Clear");
	w::setTooltip(ui, clearCfg_, "Remove --config override. Alt+C.");
	editCfg_ = w::button(ui, cfgRow, "Edit JSON…");
	w::setTooltip(ui, editCfg_, "Edit current JSON when a path is set. Alt+E.");

	ui.connect(apply_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { apply(); });
	ui.connect(browse_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { browseDecompilerConfig(); });
	ui.connect(bundled_, SignalKind::Clicked,
	           [this](UiContext&, NodeHandle, const SignalArgs&) { useBundledDecompilerConfig(); });
	ui.connect(clearCfg_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { clearDecompilerConfig(); });
	ui.connect(editCfg_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { editDecompilerConfig(); });

	clear();
}

TargetPanel::~TargetPanel()
{
	releasePanelRoot(ui_, root_);
}

bool TargetPanel::handleEvent(EventContext& ec, const Event& e)
{
	if (ec.phase != Phase::Capture || e.type != EventType::KeyDown) return false;
	if (isKey(e, Key::A, kModAlt))
	{
		if (!ui_.disabled(apply_)) apply();
		return true;
	}
	if (isKey(e, Key::B, kModAlt)) return browseDecompilerConfig(), true;
	if (isKey(e, Key::U, kModAlt))
	{
		if (!ui_.disabled(bundled_)) useBundledDecompilerConfig();
		return true;
	}
	if (isKey(e, Key::C, kModAlt)) return clearDecompilerConfig(), true;
	if (isKey(e, Key::E, kModAlt))
	{
		if (!ui_.disabled(editCfg_)) editDecompilerConfig();
		return true;
	}
	return false;
}

void TargetPanel::setFieldsEnabled(bool on)
{
	for (NodeHandle n: {arch_, os_, entry_, apply_}) ui_.setDisabled(n, !on);
	// --config is global (AppSettings): usable without a project.
	ui_.setDisabled(browse_, false);
	ui_.setDisabled(clearCfg_, false);
	ui_.setDisabled(bundled_, model::resolveBundledDecompilerConfigPath().empty());
	updateConfigButtons();
}

bool TargetPanel::fieldsEnabled() const
{
	return !ui_.disabled(apply_);
}

void TargetPanel::updateConfigButtons()
{
	ui_.setDisabled(editCfg_, !model::decompilerConfigEditable(settings_.decompiler.extraConfigPath));
}

void TargetPanel::syncDecompilerConfigFromAppSettings()
{
	dlg::setEditText(ui_, config_, model::dlgTrim(settings_.decompiler.extraConfigPath));
	ui_.setDisabled(bundled_, model::resolveBundledDecompilerConfigPath().empty());
	updateConfigButtons();
}

void TargetPanel::setFromProject(model::ProjectFile* pf)
{
	if (!pf)
	{
		clear();
		return;
	}
	project_ = pf;
	setFieldsEnabled(true);
	const model::TargetFields f = model::targetFieldsFromProject(*pf);
	ui_.setText(binary_, pf->binaryPath());
	dlg::setEditText(ui_, arch_, f.arch);
	dlg::setEditText(ui_, os_, f.os);
	dlg::setEditText(ui_, entry_, f.entry);
	syncDecompilerConfigFromAppSettings();
}

void TargetPanel::clear()
{
	project_ = nullptr;
	ui_.setText(binary_, "—");
	dlg::setEditText(ui_, arch_, "");
	dlg::setEditText(ui_, os_, "");
	dlg::setEditText(ui_, entry_, "");
	setFieldsEnabled(false);
	syncDecompilerConfigFromAppSettings();
}

std::string TargetPanel::archText() const
{
	return dlg::editText(ui_, arch_);
}
std::string TargetPanel::osText() const
{
	return dlg::editText(ui_, os_);
}
std::string TargetPanel::entryText() const
{
	return dlg::editText(ui_, entry_);
}
std::string TargetPanel::binaryText() const
{
	return std::string(ui_.text(binary_));
}
std::string TargetPanel::decompilerConfigText() const
{
	return dlg::editText(ui_, config_);
}

void TargetPanel::warn(const std::string& title, const std::string& text, MessageDialog::Kind kind)
{
	message_.show(title, text, kind);
	if (onWarning) onWarning(title, text);
}

bool TargetPanel::apply()
{
	if (onApplyRequested) onApplyRequested();
	if (!project_)
	{
		if (onStatusMessage) onStatusMessage(std::string(model::kNoProjectStatus), 3000);
		return false;
	}
	if (auto ok = model::applyTargetFields(*project_, {archText(), osText(), entryText()}); !ok)
	{
		warn("Target", ok.error(), MessageDialog::Kind::Warning);
		return false;
	}
	if (onStatusMessage) onStatusMessage(std::string(model::kTargetAppliedStatus), 5000);
	if (onApplied) onApplied();
	return true;
}

void TargetPanel::browseDecompilerConfig()
{
	const std::string start =
		model::targetConfigBrowseStart(settings_.decompiler.extraConfigPath, project_ ? binaryText() : std::string());
	files_.show(FilePrompt::Mode::Open, "Decompiler configuration JSON", start, model::kJsonFileFilter,
	            [this](const std::string& path) { setDecompilerConfig(path); });
}

void TargetPanel::setDecompilerConfig(const std::filesystem::path& path)
{
	if (path.empty()) return;
	(void)model::setDecompilerConfigPath(settings_, path);
	syncDecompilerConfigFromAppSettings();
}

void TargetPanel::useBundledDecompilerConfig()
{
	const std::filesystem::path bundled = model::resolveBundledDecompilerConfigPath();
	if (bundled.empty())
	{
		warn("Bundled config", "Could not find share/retdec/decompiler-config.json next to retdec-decompiler.",
		     MessageDialog::Kind::Information);
		return;
	}
	(void)model::setDecompilerConfigPath(settings_, bundled);
	syncDecompilerConfigFromAppSettings();
}

void TargetPanel::clearDecompilerConfig()
{
	(void)model::setDecompilerConfigPath(settings_, {});
	syncDecompilerConfigFromAppSettings();
}

void TargetPanel::editDecompilerConfig()
{
	const std::string p(model::dlgTrim(settings_.decompiler.extraConfigPath));
	if (!model::decompilerConfigEditable(p)) return;
	editor_.show(p);
}

} // namespace retdec::gui::ui
