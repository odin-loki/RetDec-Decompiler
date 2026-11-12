/**
 * @file src/gui/ui/signature_studio_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/signature_studio_panel.h"

#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/model/dlg_input.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

constexpr std::size_t kMaxLogLines = 200;

NodeHandle groupBox(UiContext& ui, NodeHandle page, std::string_view title)
{
	const NodeHandle box = w::panel(ui, page);
	ui.setCss(box, "gap", "6px");
	ui.setCss(box, "padding", "8px");
	ui.setCss(box, "border", "1px var(--border, #474955)");
	ui.setCss(box, "border-radius", "6px");
	const NodeHandle t = w::label(ui, box, title);
	ui.addClass(t, "retdec-section-title");
	return box;
}

} // namespace

SignatureStudioPanel::ToolResolver SignatureStudioPanel::defaultToolResolver()
{
	return {
		&model::resolvePythonInterpreter,
		&model::resolveSignatureFromLibraryCreatorScript,
		&model::resolveRetdecArExtractorExecutable,
		&model::resolveRetdecMachoExtractorExecutable,
		&model::resolveRetdecBin2patExecutable,
		&model::resolveRetdecPat2yaraExecutable,
	};
}

SignatureStudioPanel::SignatureStudioPanel(UiContext& ui, NodeHandle parent)
	: ui_(ui), files_(ui), tools_(defaultToolResolver())
{
	dlg::ensureDialogStyles(ui);
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-signature-studio");
	ui.setCss(root_, "padding", "6px");
	ui.setCss(root_, "gap", "6px");

	dlg::noteLabel(ui, root_,
	               "Uses RetDec tools next to the GUI or on PATH. Pick inputs below; outputs go to the Command log on "
	               "completion.");

	tabs_ = w::tabWidget(ui, root_, "Express\nAR\nMach-O\nPAT / YARA", "sigStudioTabs");
	ui.setCss(tabs_, "flex-grow", "1");
	ui.setCss(tabs_, "min-height", "0px");
	ui.setCss(w::tabWidgetBar(ui, tabs_), "flex-shrink", "0");
	if (const NodeHandle stack = ui.nextSibling(w::tabWidgetBar(ui, tabs_)); ui.alive(stack))
		ui.setCss(stack, "min-height", "0px");
	auto page = [&](u32 i) {
		const NodeHandle p = w::tabPage(ui, tabs_, i);
		ui.setCss(p, "flex-grow", "1");
		const NodeHandle s = w::scrollArea(ui, p);
		ui.setCss(s, "flex-grow", "1");
		ui.setCss(s, "padding", "6px");
		ui.setCss(s, "gap", "8px");
		return s;
	};

	// ── Express ──
	{
		const NodeHandle box = groupBox(ui, page(0), "retdec-signature-from-library-creator.py (Python 3)");
		expressLib_ = fileRow(box, "Static library / archive", FilePrompt::Mode::Open, "Library / archive", {});
		expressOut_ = fileRow(box, "Output YARA", FilePrompt::Mode::Save, "YARA output", "*.yar");
		expressRun_ = w::button(ui, box, "Run express pipeline");
		w::setTooltip(ui, expressRun_,
		              "Requires Python 3, installed RetDec scripts directory, and tools next to the script.");
		dlg::noteLabel(ui, box,
		               "Script must live in the installed scripts folder with retdec-ar-extractor, retdec-bin2pat, "
		               "retdec-pat2yara beside it (standard install layout).");
	}
	// ── AR ──
	{
		const NodeHandle box = groupBox(ui, page(1), "retdec-ar-extractor");
		arArchive_ = fileRow(box, "Archive (.a)", FilePrompt::Mode::Open, "Archive", {});
		arOut_ = fileRow(box, "Output directory", FilePrompt::Mode::Directory, "Output directory", {});
		arRun_ = w::button(ui, box, "Extract all (-e)");
	}
	// ── Mach-O ──
	{
		const NodeHandle box = groupBox(ui, page(2), "retdec-macho-extractor");
		machoFile_ = fileRow(box, "Mach-O / archive", FilePrompt::Mode::Open, "Mach-O file", {});
		machoOut_ = fileRow(box, "Output directory", FilePrompt::Mode::Directory, "Output directory", {});
		machoRun_ = w::button(ui, box, "Extract all (--all)");
	}
	// ── PAT / YARA ──
	{
		const NodeHandle p = page(3);
		const NodeHandle b2p = groupBox(ui, p, "retdec-bin2pat");
		b2pIn_ = fileRow(b2p, "Object / binary", FilePrompt::Mode::Open, "Input for bin2pat", {});
		b2pOut_ = fileRow(b2p, "Output (.pat / rules)", FilePrompt::Mode::Save, "bin2pat output", {});
		b2pRun_ = w::button(ui, b2p, "Run bin2pat");
		const NodeHandle p2y = groupBox(ui, p, "retdec-pat2yara");
		p2yIn_ = fileRow(p2y, "Input from bin2pat", FilePrompt::Mode::Open, "pat2yara input", {});
		p2yOut_ = fileRow(p2y, "Output YARA", FilePrompt::Mode::Save, "YARA output", "*.yar");
		p2yRun_ = w::button(ui, p2y, "Run pat2yara");
	}
	for (NodeHandle b: {expressRun_, arRun_, machoRun_, b2pRun_, p2yRun_}) ui.setCss(b, "align-self", "start");

	log_ = std::make_unique<CodeView>(ui, root_, model::CodeLang::Plain);
	ui.setCss(log_->node(), "height", "100px");
	ui.setCss(log_->node(), "flex-shrink", "0");
	ui.setCss(log_->node(), "flex-grow", "0");
	log_->setShowGutter(false);
	log_->setPlaceholder("Last tool output tail…");

	useAsStatic_ = w::button(ui, root_, "Use YARA as static-code sigfile", "sigStudioUseAsStatic");
	ui.setCss(useAsStatic_, "align-self", "start");
	w::setTooltip(ui, useAsStatic_, "Next interactive decompile will use this path as --static-code-sigfile.");

	ui.connect(expressRun_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { runExpress(); });
	ui.connect(arRun_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { runArExtractor(); });
	ui.connect(machoRun_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { runMachoExtractor(); });
	ui.connect(b2pRun_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { runBin2pat(); });
	ui.connect(p2yRun_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { runPat2yara(); });
	ui.connect(useAsStatic_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { useAsStaticSigFile(); });
}

SignatureStudioPanel::~SignatureStudioPanel()
{
	if (timerActive_) ui_.cancelTimer(timer_);
	runner_.cancel();
	log_.reset();
	releasePanelRoot(ui_, root_);
}

bool SignatureStudioPanel::handleEvent(EventContext&, const Event&)
{
	return false;
}

NodeHandle SignatureStudioPanel::fileRow(NodeHandle form, std::string_view label, FilePrompt::Mode mode,
                                         std::string_view promptTitle, std::string_view filter)
{
	const NodeHandle row = dlg::formRow(ui_, form, label);
	const NodeHandle e = dlg::growEdit(ui_, row);
	const NodeHandle b = w::button(ui_, row, "Browse…");
	const std::size_t idx = browseRows_.size();
	browseRows_.push_back({e, mode, std::string(promptTitle), std::string(filter)});
	ui_.connect(b, SignalKind::Clicked, [this, idx](UiContext&, NodeHandle, const SignalArgs&) {
		const BrowseRow& r = browseRows_[idx];
		const NodeHandle edit = r.edit;
		files_.show(r.mode, r.title, dlg::editText(ui_, edit), r.filter,
		            [this, edit](const std::string& p) { dlg::setEditText(ui_, edit, p); });
	});
	return e;
}

void SignatureStudioPanel::setCurrentTab(int tab)
{
	ui_.setValue(w::tabWidgetBar(ui_, tabs_), float(tab), true);
}

void SignatureStudioPanel::setActiveBinary(const std::string& path)
{
	activeBinary_ = path;
	dlg::setEditText(ui_, expressLib_, "");
	dlg::setEditText(ui_, expressOut_, "");
	syncDefaultsFromBinary();
}

void SignatureStudioPanel::syncDefaultsFromBinary()
{
	if (activeBinary_.empty()) return;
	const model::SigStudioDefaults d = model::sigStudioDefaultsForBinary(activeBinary_);
	auto fill = [&](NodeHandle e, const std::string& v) {
		if (model::dlgTrim(dlg::editText(ui_, e)).empty()) dlg::setEditText(ui_, e, v);
	};
	fill(expressLib_, d.expressLib);
	fill(expressOut_, d.expressOut);
	fill(arArchive_, d.arArchive);
	fill(machoFile_, d.machoFile);
	fill(arOut_, d.workDir);
	fill(machoOut_, d.workDir);
}

void SignatureStudioPanel::clear()
{
	activeBinary_.clear();
	logLines_.clear();
	log_->clear();
}

void SignatureStudioPanel::appendLog(std::string_view text)
{
	std::size_t at = 0;
	while (at <= text.size())
	{
		const std::size_t nl = text.find('\n', at);
		logLines_.emplace_back(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
		if (nl == std::string_view::npos) break;
		at = nl + 1;
	}
	if (logLines_.size() > kMaxLogLines) logLines_.erase(logLines_.begin(), logLines_.end() - kMaxLogLines);
	std::string all;
	for (std::size_t i = 0; i < logLines_.size(); ++i)
	{
		if (i) all += '\n';
		all += logLines_[i];
	}
	log_->setText(all);
	log_->scrollToEnd();
}

std::string SignatureStudioPanel::log() const
{
	return log_->text();
}

void SignatureStudioPanel::setBusy(bool on)
{
	for (NodeHandle b: {expressRun_, arRun_, machoRun_, b2pRun_, p2yRun_}) ui_.setDisabled(b, on);
}

bool SignatureStudioPanel::busy() const
{
	return runner_.running() || timerActive_;
}

bool SignatureStudioPanel::start(std::expected<model::SigToolCommand, std::string> cmd)
{
	if (!cmd)
	{
		appendLog(cmd.error());
		return false;
	}
	if (busy()) return false;
	if (auto st = runner_.start(*cmd); !st)
	{
		if (st.error() != "busy") appendLog(st.error());
		return false;
	}
	setBusy(true);
	timerActive_ = true;
	timer_ = ui_.startTimer(0.1f, true, [this](UiContext&) { poll(); });
	return true;
}

bool SignatureStudioPanel::poll()
{
	auto r = runner_.poll();
	if (!r) return false;
	if (timerActive_)
	{
		ui_.cancelTimer(timer_);
		timerActive_ = false;
	}
	setBusy(false);
	appendLog(model::sigToolLogEntry(*r));
	if (onCliToolFinished) onCliToolFinished(*r);
	return true;
}

bool SignatureStudioPanel::waitForTool(std::chrono::milliseconds timeout)
{
	auto r = runner_.waitFor(timeout);
	if (!r) return false;
	if (timerActive_)
	{
		ui_.cancelTimer(timer_);
		timerActive_ = false;
	}
	setBusy(false);
	appendLog(model::sigToolLogEntry(*r));
	if (onCliToolFinished) onCliToolFinished(*r);
	return true;
}

static std::filesystem::path resolve(const std::function<std::filesystem::path()>& f)
{
	return f ? f() : std::filesystem::path();
}

bool SignatureStudioPanel::runExpress()
{
	const std::string lib = dlg::editText(ui_, expressLib_), out = dlg::editText(ui_, expressOut_);
	// Validate the fields before looking for Python (the Qt order of messages).
	if (model::dlgTrim(lib).empty() || model::dlgTrim(out).empty())
		return start(model::buildExpressCommand(lib, out, {}, {}));
	return start(model::buildExpressCommand(lib, out, resolve(tools_.python), resolve(tools_.expressScript)));
}

bool SignatureStudioPanel::runArExtractor()
{
	return start(model::buildArExtractorCommand(resolve(tools_.arExtractor), dlg::editText(ui_, arArchive_),
	                                            dlg::editText(ui_, arOut_)));
}

bool SignatureStudioPanel::runMachoExtractor()
{
	return start(model::buildMachoExtractorCommand(resolve(tools_.machoExtractor), dlg::editText(ui_, machoFile_),
	                                               dlg::editText(ui_, machoOut_)));
}

bool SignatureStudioPanel::runBin2pat()
{
	auto cmd = model::buildBin2patCommand(resolve(tools_.bin2pat), dlg::editText(ui_, b2pIn_), dlg::editText(ui_, b2pOut_));
	if (cmd && !cmd->defaultedOutput.empty()) dlg::setEditText(ui_, b2pOut_, cmd->defaultedOutput);
	return start(std::move(cmd));
}

bool SignatureStudioPanel::runPat2yara()
{
	auto cmd = model::buildPat2yaraCommand(resolve(tools_.pat2yara), dlg::editText(ui_, p2yIn_), dlg::editText(ui_, p2yOut_));
	if (cmd && !cmd->defaultedOutput.empty()) dlg::setEditText(ui_, p2yOut_, cmd->defaultedOutput);
	return start(std::move(cmd));
}

void SignatureStudioPanel::useAsStaticSigFile()
{
	const auto path = model::pickStaticSigFile(dlg::editText(ui_, expressOut_), dlg::editText(ui_, p2yOut_));
	if (!path)
	{
		if (onStatusMessage) onStatusMessage("No YARA output file on disk yet.", 4000);
		return;
	}
	if (onUseAsStaticSigFile) onUseAsStaticSigFile(*path);
	if (onStatusMessage) onStatusMessage(std::format("Next decompile will use --static-code-sigfile {}", *path), 6000);
}

} // namespace retdec::gui::ui
