/**
 * @file src/gui/ui/inspect_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/inspect_panel.h"

#include <algorithm>
#include <filesystem>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

std::string cellSafe(std::string_view s)
{
	std::string r(s);
	for (char& c: r)
		if (c == '\t' || c == '\n' || c == '\r') c = ' ';
	return r;
}

bool fileExists(const std::string& p)
{
	std::error_code ec;
	return !p.empty() && std::filesystem::exists(p, ec);
}

std::string trimmed(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.remove_suffix(1);
	return std::string(s);
}

} // namespace

InspectPanel::InspectPanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	handler_.handle = [](EventContext&, const Event&) { return false; };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-inspect");
	ui.setCss(root_, "padding", "4px");
	ui.setCss(root_, "gap", "4px");

	const NodeHandle row = toolbarRow(ui, root_);
	status_ = w::label(ui, row, "No fileinfo run yet.");
	ui.addClass(status_, "retdec-status");
	refreshBtn_ = w::button(ui, row, "Refresh fileinfo");
	decompileBtn_ = w::button(ui, row, "Decompile mode");
	w::setTooltip(ui, decompileBtn_, "Switch workspace to Decompile (centre code tabs).");
	copyHashesBtn_ = w::button(ui, row, "Copy hashes");
	w::setTooltip(ui, copyHashesBtn_, "Copy SHA-256 / MD5 / CRC32 from the last fileinfo JSON.");

	const NodeHandle urow = toolbarRow(ui, root_);
	w::label(ui, urow, "Unpack output:");
	unpackOut_ = w::lineEdit(ui, urow, "");
	ui.setCss(unpackOut_, "flex-grow", "1");
	w::setTooltip(ui, unpackOut_, "Defaults to <binary>-unpacked");
	bruteChk_ = w::checkBox(ui, urow, "Brute", false);
	w::setTooltip(ui, bruteChk_, "Pass -b|--brute to retdec-unpacker.");
	autoOpenChk_ = w::checkBox(ui, urow, "Open when done", false);
	w::setTooltip(ui, autoOpenChk_, "Load the unpacked file as the current binary when unpacker exits 0.");
	unpackBtn_ = w::button(ui, urow, "Run unpacker");
	openUnpackedBtn_ = w::button(ui, urow, "Open unpacked");
	w::setTooltip(ui, openUnpackedBtn_, "Load the unpack output path as the current binary.");

	tabs_ = w::tabWidget(ui, root_, "Summary\nJSON");
	ui.setCss(tabs_, "flex-grow", "1");
	ui.setCss(tabs_, "min-height", "0px");
	const NodeHandle sumPage = w::tabPage(ui, tabs_, 0);
	const NodeHandle jsonPage = w::tabPage(ui, tabs_, 1);
	summaryTable_ = w::tableView(ui, sumPage, &summary_.model());
	ui.setCss(summaryTable_, "flex-grow", "1");
	w::setTableColumnWidth(ui, summaryTable_, 0, 200.f);
	w::setTableColumnWidth(ui, summaryTable_, 1, 520.f);
	raw_ = std::make_unique<CodeView>(ui, jsonPage, model::CodeLang::Plain);
	raw_->setPlaceholder("JSON output from retdec-fileinfo --json");

	auto* self = this;
	ui.connect(refreshBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->refresh(); });
	ui.connect(decompileBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) {
		if (self->onRequestDecompileMode) self->onRequestDecompileMode();
	});
	ui.connect(copyHashesBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->copyHashes(); });
	ui.connect(unpackBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->runUnpacker(); });
	ui.connect(openUnpackedBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->openUnpacked(); });
	ui.connect(unpackOut_, SignalKind::TextChanged, [self](UiContext&, NodeHandle, const SignalArgs&) { self->updateButtonStates(); });
	updateButtonStates();
}

InspectPanel::~InspectPanel()
{
	if (polling_) ui_.cancelTimer(pollTimer_);
	model_.cancelAll();
	raw_.reset();
	releasePanelRoot(ui_, root_);
}

std::string InspectPanel::statusText() const
{
	return std::string(ui_.text(status_));
}

void InspectPanel::status(std::string_view s)
{
	ui_.setText(status_, s);
}

std::string InspectPanel::summaryCell(int row, int col) const
{
	if (row < 0 || u32(row) >= summary_.rows() || col < 0 || col > 1) return {};
	return std::string(summary_.cell(u32(row), u32(col)));
}

std::string InspectPanel::unpackOutput() const
{
	return ui_.editBuffer(unpackOut_).text();
}

int InspectPanel::currentTab() const
{
	return int(ui_.value(w::tabWidgetBar(ui_, tabs_)));
}

void InspectPanel::setSummaryMessage(std::string_view msg)
{
	summary_.clear();
	summary_.addRow(cellSafe(msg));
	w::tableModelChanged(ui_, summaryTable_);
}

void InspectPanel::setRawCapped(std::string_view text, std::size_t cap)
{
	if (text.size() <= cap)
	{
		raw_->setText(text);
		return;
	}
	std::size_t cut = cap;
	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
		--cut;
	std::string preview(text.substr(0, cut));
	preview += std::format("\n\n\xE2\x80\xA6[truncated \xE2\x80\x94 {} KiB total, showing first {} KiB]", text.size() / 1024, cap / 1024);
	raw_->setText(preview);
}

void InspectPanel::updateButtonStates()
{
	const bool fiBusy = model_.running(model::InspectTool::Fileinfo);
	const bool unBusy = model_.running(model::InspectTool::Unpacker);
	ui_.setDisabled(refreshBtn_, fiBusy || unBusy);
	ui_.setDisabled(unpackBtn_, binaryPath_.empty() || fiBusy || unBusy);
	const std::string out = trimmed(unpackOutput());
	ui_.setDisabled(openUnpackedBtn_, !fileExists(out) || fiBusy || unBusy);
	ui_.setDisabled(bruteChk_, unBusy);
}

void InspectPanel::ensurePolling()
{
	if (polling_) return;
	polling_ = true;
	auto* self = this;
	pollTimer_ = ui_.startTimer(0.05f, true, [self](UiContext&) { self->poll(); });
}

void InspectPanel::poll()
{
	for (const model::ToolRun& r: model_.poll())
		handleRun(r);
	if (!model_.anyRunning() && polling_)
	{
		ui_.cancelTimer(pollTimer_);
		polling_ = false;
	}
	updateButtonStates();
}

void InspectPanel::runFileinfo(std::string_view binaryPath, std::string_view fileinfoExecutable)
{
	std::string abs;
	if (!binaryPath.empty())
	{
		std::error_code ec;
		abs = std::filesystem::absolute(std::filesystem::path(std::string(binaryPath)), ec).string();
		if (ec) abs = std::string(binaryPath);
	}
	binaryPath_ = abs;
	fileinfoExe_ = std::string(fileinfoExecutable);
	setLineEditText(ui_, unpackOut_, model::defaultUnpackOutput(binaryPath_));
	if (abs.empty())
	{
		updateButtonStates();
		return;
	}
	model_.cancel(model::InspectTool::Fileinfo);
	if (fileinfoExe_.empty())
	{
		status("retdec-fileinfo not found (install or add to PATH).");
		setSummaryMessage("No fileinfo executable.");
		raw_->clear();
		updateButtonStates();
		return;
	}
	status("Running retdec-fileinfo\xE2\x80\xA6");
	setSummaryMessage("Running\xE2\x80\xA6");
	raw_->clear();
	ui_.setValue(w::tabWidgetBar(ui_, tabs_), 0.f, true);
	const auto args = model::fileinfoArgs(binaryPath_);
	const std::filesystem::path exe(fileinfoExe_);
	if (onProcessStarted) onProcessStarted("retdec-fileinfo", args, exe.has_parent_path() ? exe.parent_path().string() : std::string());
	auto r = model_.startFileinfo(binaryPath_, fileinfoExe_);
	if (!r)
	{
		status(r.error());
		updateButtonStates();
		return;
	}
	ensurePolling();
	updateButtonStates();
}

void InspectPanel::refresh()
{
	if (!binaryPath_.empty()) runFileinfo(binaryPath_, fileinfoExe_);
}

void InspectPanel::runUnpacker()
{
	if (binaryPath_.empty()) return;
	if (unpackerExe_.empty())
	{
		status("retdec-unpacker not found.");
		return;
	}
	if (model_.running(model::InspectTool::Unpacker)) return;
	std::string outPath = trimmed(unpackOutput());
	if (outPath.empty())
	{
		setLineEditText(ui_, unpackOut_, model::defaultUnpackOutput(binaryPath_));
		outPath = trimmed(unpackOutput());
	}
	if (outPath.empty())
	{
		status("Set an unpack output path.");
		return;
	}
	const bool brute = ui_.checked(bruteChk_);
	status("Running retdec-unpacker\xE2\x80\xA6");
	ui_.setDisabled(openUnpackedBtn_, true);
	const std::filesystem::path exe(unpackerExe_);
	if (onProcessStarted)
		onProcessStarted("retdec-unpacker", model::unpackerArgs(binaryPath_, outPath, brute), exe.has_parent_path() ? exe.parent_path().string() : std::string());
	auto r = model_.startUnpacker(binaryPath_, unpackerExe_, outPath, brute);
	if (!r)
	{
		status(r.error());
		updateButtonStates();
		return;
	}
	ensurePolling();
	updateButtonStates();
}

void InspectPanel::openUnpacked()
{
	const std::string p = trimmed(unpackOutput());
	if (!fileExists(p)) return;
	std::error_code ec;
	const std::string abs = std::filesystem::absolute(p, ec).string();
	if (onRequestOpenBinary) onRequestOpenBinary(ec ? p : abs);
}

void InspectPanel::copyHashes()
{
	const std::string text = info_ ? model::formatHashesForClipboard(*info_) : std::string();
	if (text.empty())
	{
		status("No hashes in fileinfo JSON yet.");
		return;
	}
	ui_.setClipboardText(text);
	status("Copied file hashes.");
}

void InspectPanel::setUnpackOutput(std::string_view path)
{
	setLineEditText(ui_, unpackOut_, path);
	updateButtonStates();
}

void InspectPanel::setBrute(bool on)
{
	ui_.setChecked(bruteChk_, on);
}

void InspectPanel::setAutoOpen(bool on)
{
	ui_.setChecked(autoOpenChk_, on);
}

void InspectPanel::clear()
{
	model_.cancelAll();
	if (polling_)
	{
		ui_.cancelTimer(pollTimer_);
		polling_ = false;
	}
	binaryPath_.clear();
	lastFileinfoBinary_.clear();
	lastJson_.clear();
	info_.reset();
	status("No binary loaded.");
	summary_.clear();
	w::tableModelChanged(ui_, summaryTable_);
	raw_->clear();
	setLineEditText(ui_, unpackOut_, "");
	updateButtonStates();
}

void InspectPanel::handleRun(const model::ToolRun& run)
{
	if (onProcessOutput && (!run.out.empty() || !run.err.empty())) onProcessOutput(run.tool, run.out, run.err);
	if (run.kind == model::InspectTool::Fileinfo) handleFileinfo(run);
	else handleUnpacker(run);
}

void InspectPanel::handleFileinfo(const model::ToolRun& run)
{
	if (onCliToolFinished) onCliToolFinished(run.tool, run.args, run.cwd, run.exitCode, run.elapsedMs, run.outputTail());
	if (run.exitCode == -1 && !run.errorText.empty())
	{
		status(std::format("fileinfo error: {}", run.errorText));
		setSummaryMessage("Process error.");
		return;
	}
	if (run.exitCode != 0)
	{
		status(std::format("fileinfo failed (exit {})", run.exitCode));
		setSummaryMessage("Non-zero exit. See JSON tab.");
		if (run.out.empty()) raw_->setText("(no output)");
		else setRawCapped(run.out, 256 * 1024);
		return;
	}
	status("fileinfo OK \xE2\x80\x94 see Summary / JSON tabs.");
	constexpr std::size_t kMaxRawBytes = 512 * 1024;
	auto parsed = model::parseFileinfoJson(run.out);
	if (!parsed)
	{
		info_.reset();
		lastJson_.clear();
		lastFileinfoBinary_.clear();
		setSummaryMessage(std::format("Could not parse JSON ({}). Raw output in JSON tab.", parsed.error()));
		setRawCapped(run.out, kMaxRawBytes);
		return;
	}
	info_ = std::move(*parsed);
	lastJson_ = run.out;
	lastFileinfoBinary_ = binaryPath_;
	summary_.clear();
	for (const auto& [k, v]: info_->summaryRows)
		summary_.addRow(cellSafe(k) + "\t" + cellSafe(v));
	if (info_->summaryRows.empty()) summary_.addRow("No flat key/value fields at root; see JSON tab.");
	w::tableModelChanged(ui_, summaryTable_);
	setRawCapped(run.out.size() <= kMaxRawBytes ? std::string_view(info_->prettyJson) : std::string_view(run.out), kMaxRawBytes);
	if (onFileinfoReady) onFileinfoReady(*info_, lastJson_, lastFileinfoBinary_);
}

void InspectPanel::handleUnpacker(const model::ToolRun& run)
{
	if (onCliToolFinished) onCliToolFinished(run.tool, run.args, run.cwd, run.exitCode, run.elapsedMs, run.outputTail());
	const std::string outp = trimmed(unpackOutput());
	if (run.exitCode == -1 && !run.errorText.empty())
	{
		status(std::format("Unpacker error: {}", run.errorText));
		ui_.setDisabled(openUnpackedBtn_, true);
		return;
	}
	if (run.exitCode == 0)
	{
		status(std::format("Unpacker finished (exit 0). Output: {}", outp));
		updateButtonStates();
		if (ui_.checked(autoOpenChk_) && fileExists(outp)) openUnpacked();
	}
	else
	{
		status(std::format("Unpacker exit {} \xE2\x80\x94 see Command log / Diagnostics.", run.exitCode));
		ui_.setDisabled(openUnpackedBtn_, true);
	}
}

} // namespace retdec::gui::ui
