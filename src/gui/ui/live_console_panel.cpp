/**
 * @file src/gui/ui/live_console_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/live_console_panel.h"

#include "retdec/gui/model/code_text.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

LiveConsolePanel::LiveConsolePanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-console");
	ui.setCss(root_, "padding", "4px");
	ui.setCss(root_, "gap", "4px");

	const NodeHandle row = toolbarRow(ui, root_);
	copyBtn_ = w::button(ui, row, "Copy all");
	saveBtn_ = w::button(ui, row, "Save\xE2\x80\xA6");
	clearBtn_ = w::button(ui, row, "Clear");
	autoChk_ = w::checkBox(ui, row, "Auto-scroll", autoScroll_);
	status_ = w::label(ui, row, "(idle)");
	ui.addClass(status_, "retdec-status");
	w::setTooltip(ui, copyBtn_, "Copy entire console to clipboard.");
	w::setTooltip(ui, saveBtn_, "Save console output to a text file.");
	w::setTooltip(ui, clearBtn_, "Clear the console buffer.");
	w::setTooltip(ui, autoChk_, "Follow new lines as they arrive. Untick to scroll back without losing position.");

	view_ = std::make_unique<CodeView>(ui, root_, model::CodeLang::Plain);
	view_->setShowGutter(false);
	view_->setConsoleBuffer(&buffer_);
	view_->setPlaceholder("Live output from retdec-decompiler / retdec-fileinfo / retdec-unpacker will appear here as it runs.");

	auto* self = this;
	ui.connect(copyBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->copyAll(); });
	ui.connect(saveBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->saveAsDialog(); });
	ui.connect(clearBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->clear(); });
	ui.connect(autoChk_, SignalKind::Toggled, [self](UiContext& c, NodeHandle n, const SignalArgs&) { self->autoScroll_ = c.checked(n); });
	startTimer();
}

LiveConsolePanel::~LiveConsolePanel()
{
	ui_.cancelTimer(timer_);
	if (rearmPending_) ui_.cancelTimer(rearm_);
	if (ui_.alive(saveDlg_)) ui_.destroy(saveDlg_);
	view_.reset();
	releasePanelRoot(ui_, root_);
}

bool LiveConsolePanel::handleEvent(EventContext& ec, const Event& e)
{
	if (ec.phase == Phase::Capture && isKey(e, Key::C, kModCtrl | kModShift))
	{
		copyAll();
		return true;
	}
	return false;
}

void LiveConsolePanel::startTimer()
{
	if (timer_.valid()) ui_.cancelTimer(timer_);
	auto* self = this;
	timer_ = ui_.startTimer(1.f / float(flushHz_), true, [self](UiContext&) { self->flush(); });
}

// ── Sources ──────────────────────────────────────────────────────────────────

uint64_t LiveConsolePanel::attachSource(std::string_view label)
{
	return feed_->attach(std::string(label));
}

void LiveConsolePanel::detachSource(uint64_t source)
{
	insertChunks(feed_->detach(source));
}

void LiveConsolePanel::pushOutput(uint64_t source, Stream stream, std::string_view bytes)
{
	feed_->push(source, stream, bytes);
}

void LiveConsolePanel::flush()
{
	bool more = false;
	insertChunks(feed_->drain(std::size_t(std::max(4096, maxBytesPerFlush_)), more));
	if (more && !rearmPending_)
	{
		// Drain the rest on the next frame instead of waiting a whole tick.
		rearmPending_ = true;
		auto* self = this;
		rearm_ = ui_.startTimer(0.f, false, [self](UiContext&) {
			self->rearmPending_ = false;
			self->flush();
		});
	}
}

void LiveConsolePanel::insertChunks(std::vector<model::ConsoleFeed::Chunk> chunks)
{
	for (const auto& c: chunks)
		appendChunk(c.stream, c.bytes);
}

// ── Text ingress ─────────────────────────────────────────────────────────────

void LiveConsolePanel::insert(std::string_view decoded)
{
	if (decoded.empty()) return;
	buffer_.append(decoded);
	view_->contentChanged();
	if (autoScroll_) view_->scrollToEnd();
}

void LiveConsolePanel::appendChunk(Stream stream, std::string_view bytes)
{
	(void)stream; // channels are merged; lines are coloured by content
	if (bytes.empty()) return;
	std::string text = model::sanitizeUtf8(bytes);
	if (text.find('\x1b') != std::string::npos) text = model::stripAnsi(text);
	insert(text);
}

void LiveConsolePanel::appendLine(Stream stream, std::string_view line)
{
	std::string withLf(line);
	if (withLf.empty() || withLf.back() != '\n') withLf += '\n';
	appendChunk(stream, withLf);
}

void LiveConsolePanel::appendBanner(std::string_view tool, const std::vector<std::string>& args, std::string_view cwd)
{
	for (const std::string& l: model::formatBanner(tool, args, cwd, model::isoTimestampNow()))
		appendLine(Stream::Stdout, l);
	ui_.setText(status_, std::format("running: {}", tool));
}

void LiveConsolePanel::appendFooter(std::string_view tool, int exitCode, int64_t elapsedMs)
{
	appendLine(Stream::Stdout, model::formatFooter(tool, exitCode, elapsedMs));
	ui_.setText(status_, std::format("idle (last: {}, exit={})", tool, exitCode));
}

// ── Configuration ────────────────────────────────────────────────────────────

void LiveConsolePanel::setMaxBlocks(int blocks)
{
	buffer_.setMaxLines(std::size_t(std::max(1000, blocks)));
	view_->contentChanged();
}

int LiveConsolePanel::maxBlocks() const
{
	return int(buffer_.maxLines());
}

void LiveConsolePanel::setFlushHz(int hz)
{
	flushHz_ = std::clamp(hz, 2, 250);
	startTimer();
}

void LiveConsolePanel::setMaxBytesPerFlush(int bytes)
{
	maxBytesPerFlush_ = std::max(4 * 1024, bytes);
}

void LiveConsolePanel::setAutoScroll(bool on)
{
	autoScroll_ = on;
	ui_.setChecked(autoChk_, on);
	if (on) view_->scrollToEnd();
}

// ── Actions ──────────────────────────────────────────────────────────────────

void LiveConsolePanel::copyAll()
{
	ui_.setClipboardText(buffer_.text());
}

bool LiveConsolePanel::saveAs(const std::filesystem::path& path)
{
	auto r = model::saveTextFile(path, buffer_.text());
	if (!r)
	{
		lastError_ = r.error();
		return false;
	}
	lastError_.clear();
	return true;
}

void LiveConsolePanel::saveAsDialog()
{
	if (!ui_.alive(saveDlg_))
	{
		std::error_code ec;
		saveDlg_ = w::fileDialog(ui_, "Save console output", std::filesystem::current_path(ec).string(), "*.txt", true);
		auto* self = this;
		ui_.connect(saveDlg_, SignalKind::Submitted, [self](UiContext& c, NodeHandle, const SignalArgs& a) {
			if (a.text.empty()) return;
			const std::string path(a.text);
			if (self->saveAs(path)) c.setText(self->status_, std::format("saved {}", path));
			else c.setText(self->status_, std::format("save error: {}", self->lastError_));
		});
	}
	w::openDialog(ui_, saveDlg_);
}

void LiveConsolePanel::clear()
{
	buffer_.clear();
	feed_->discardPending();
	view_->contentChanged();
	view_->setFirstVisibleLine(0);
	ui_.setText(status_, "(idle)");
}

std::string LiveConsolePanel::statusText() const
{
	return std::string(ui_.text(status_));
}

} // namespace retdec::gui::ui
