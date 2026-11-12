/**
 * @file src/gui/ui/command_log_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/command_log_panel.h"

#include "retdec/gui/model/code_text.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

CommandLogPanel::CommandLogPanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-command-log");
	ui.setCss(root_, "padding", "4px");
	ui.setCss(root_, "gap", "4px");

	const NodeHandle row = toolbarRow(ui, root_);
	copyBtn_ = w::button(ui, row, "Copy all");
	clearBtn_ = w::button(ui, row, "Clear");
	saveBtn_ = w::button(ui, row, "Save\xE2\x80\xA6");
	w::setTooltip(ui, copyBtn_, "Copy the full log to the clipboard. Ctrl+Shift+C when this panel has focus.");
	w::setTooltip(ui, clearBtn_, "Clear this view (does not affect Diagnostics).");

	view_ = std::make_unique<CodeView>(ui, root_, model::CodeLang::Plain);
	view_->setShowGutter(false);
	view_->setConsoleBuffer(&buffer_);
	view_->setPlaceholder("External tools (retdec-fileinfo, retdec-decompiler, \xE2\x80\xA6) are logged here.");

	auto* self = this;
	ui.connect(copyBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->copyAll(); });
	ui.connect(clearBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->clear(); });
	ui.connect(saveBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->saveAs(); });
}

CommandLogPanel::~CommandLogPanel()
{
	if (ui_.alive(saveDlg_)) ui_.destroy(saveDlg_);
	view_.reset();
	releasePanelRoot(ui_, root_);
}

bool CommandLogPanel::handleEvent(EventContext& ec, const Event& e)
{
	if (ec.phase == Phase::Capture && isKey(e, Key::C, kModCtrl | kModShift))
	{
		copyAll();
		return true;
	}
	return false;
}

void CommandLogPanel::appendRun(std::string_view tool, const std::vector<std::string>& args, std::string_view cwd, int exitCode,
                                int64_t elapsedMs, std::string_view outputTail)
{
	std::string entry = model::formatCommandLogEntry(tool, args, cwd, exitCode, elapsedMs, outputTail, model::isoTimestampNow());
	// appendPlainText adds a paragraph; the entry's own final newline is the
	// blank separator line.
	if (!entry.empty() && entry.back() == '\n') entry.pop_back();
	buffer_.appendParagraph(entry);
	view_->contentChanged();
	view_->scrollToEnd();
}

void CommandLogPanel::clear()
{
	buffer_.clear();
	view_->contentChanged();
	view_->setFirstVisibleLine(0);
}

void CommandLogPanel::setMaxBlocks(int blocks)
{
	buffer_.setMaxLines(std::size_t(std::max(100, blocks)));
	view_->contentChanged();
}

int CommandLogPanel::maxBlocks() const
{
	return int(buffer_.maxLines());
}

void CommandLogPanel::copyAll()
{
	ui_.setClipboardText(buffer_.text());
}

bool CommandLogPanel::saveTo(const std::filesystem::path& path)
{
	auto r = model::saveTextFile(path, buffer_.text());
	if (!r)
	{
		if (onStatusMessage) onStatusMessage("Save failed: " + r.error(), 4000);
		return false;
	}
	return true;
}

void CommandLogPanel::saveAs()
{
	if (!ui_.alive(saveDlg_))
	{
		std::error_code ec;
		saveDlg_ = w::fileDialog(ui_, "Save command log", std::filesystem::current_path(ec).string(), "*.txt", true);
		auto* self = this;
		ui_.connect(saveDlg_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs& a) {
			if (!a.text.empty()) self->saveTo(std::filesystem::path(std::string(a.text)));
		});
	}
	w::openDialog(ui_, saveDlg_);
}

} // namespace retdec::gui::ui
