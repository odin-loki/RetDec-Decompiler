/**
 * @file src/gui/ui/settings_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/settings_dialog.h"

#include "retdec/gui/model/dlg_input.h"
#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/model/dlg_settings_form.h"
#include "retdec/gui/ui/empty_state.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

constexpr std::string_view kTabTitles = "General\nAnalysis\nCUDA\nML\nRecovery\nAdvanced\nDecompiler\nPlugins";

std::string comboEntryLabels(const std::vector<model::ComboEntry>& items)
{
	std::vector<std::string> labels;
	for (const auto& e: items)
		labels.push_back(e.label);
	return dlg::joinLines(labels);
}

} // namespace

// ── Construction ─────────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(UiContext& ui, model::AppSettings& settings, model::PluginManager& plugins):
	ui_(ui), settings_(settings), plugins_(plugins), dlg_(ui, "RetDec Settings", 760.f, 580.f, "settingsDialog"),
	message_(ui), files_(ui)
{
	ui.setCss(dlg_.card(), "min-width", "720px");
	ui.setCss(dlg_.card(), "min-height", "540px");
	tabs_ = w::tabWidget(ui, dlg_.body(), kTabTitles, "settingsTabs");
	ui.setCss(tabs_, "flex-grow", "1");
	ui.setCss(tabs_, "min-height", "0px");
	ui.setCss(w::tabWidgetBar(ui, tabs_), "flex-shrink", "0");
	if (const NodeHandle stack = ui.nextSibling(w::tabWidgetBar(ui, tabs_)); ui.alive(stack))
	{
		// The page stack must shrink to the card, so pages scroll instead of the card.
		ui.setCss(stack, "min-height", "0px");
		ui.setCss(stack, "overflow", "hidden");
	}

	auto page = [&](u32 i) {
		const NodeHandle p = w::tabPage(ui, tabs_, i);
		ui.setCss(p, "flex-grow", "1");
		ui.setCss(p, "min-height", "0px");
		if (i == Plugins)
		{
			ui.addClass(p, "retdec-page");
			ui.setCss(p, "padding", "8px");
			ui.setCss(p, "gap", "6px");
			return p;
		}
		const NodeHandle s = w::scrollArea(ui, p);
		ui.addClass(s, "retdec-page");
		ui.setCss(s, "flex-grow", "1");
		ui.setCss(s, "padding", "12px");
		ui.setCss(s, "gap", "6px");
		return s;
	};
	buildGeneral(page(General));
	buildAnalysis(page(Analysis));
	buildCuda(page(CUDA));
	buildMl(page(ML));
	buildRecovery(page(Recovery));
	buildAdvanced(page(Advanced));
	buildDecompiler(page(Decompiler));
	buildPlugins(page(Plugins));

	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	ui.setCss(buttons, "justify-content", "start");
	restore_ = w::button(ui, buttons, "Restore Defaults");
	export_ = w::button(ui, buttons, "Export…");
	import_ = w::button(ui, buttons, "Import…");
	const NodeHandle spacer = w::panel(ui, buttons);
	ui.setCss(spacer, "flex-grow", "1");
	ok_ = w::button(ui, buttons, "OK");
	ui.addClass(ok_, "primary");
	cancel_ = w::button(ui, buttons, "Cancel");
	apply_ = w::button(ui, buttons, "Apply");

	ui.connect(ok_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { ok(); });
	ui.connect(cancel_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { cancel(); });
	ui.connect(apply_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { apply(); });
	ui.connect(restore_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { restoreDefaults(); });
	ui.connect(export_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		files_.show(
			FilePrompt::Mode::Save, "Export settings", {}, model::kJsonFileFilter, [this](const std::string& path) {
				if (!exportTo(path)) warn("Failed to export settings to:\n" + path);
			});
	});
	ui.connect(import_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		files_.show(
			FilePrompt::Mode::Open, "Import settings", {}, model::kJsonFileFilter, [this](const std::string& path) {
				if (!importFrom(path)) warn("Failed to import settings from:\n" + path);
			});
	});

	populateFromSettings();
}

SettingsDialog::~SettingsDialog() = default;

// ── Field helpers ────────────────────────────────────────────────────────────

NodeHandle SettingsDialog::reg(std::string_view name, NodeHandle n)
{
	fields_[std::string(name)] = n;
	return n;
}

NodeHandle
SettingsDialog::check(NodeHandle parent, std::string_view name, std::string_view text, std::string_view tooltip)
{
	const NodeHandle c = w::checkBox(ui_, parent, text);
	if (!tooltip.empty()) w::setTooltip(ui_, c, tooltip);
	return reg(name, c);
}

NodeHandle SettingsDialog::spin(
	NodeHandle parent,
	std::string_view name,
	std::string_view label,
	float lo,
	float hi,
	float step,
	std::string_view hint,
	std::string_view suffix,
	std::string_view specialValue)
{
	const NodeHandle row = dlg::formRow(ui_, parent, label, hint);
	const NodeHandle s = w::spinBox(ui_, row, lo, hi, lo, step);
	ui_.setCss(s, "width", "140px");
	if (!specialValue.empty())
		w::setSpecialValueText(ui_, s, specialValue); // shown at the minimum (Qt specialValueText)
	if (!hint.empty()) w::setTooltip(ui_, s, hint);
	if (!suffix.empty()) dlg::noteLabel(ui_, row, suffix);
	return reg(name, s);
}

NodeHandle SettingsDialog::combo(
	NodeHandle parent,
	std::string_view name,
	std::string_view label,
	const std::vector<std::string>& items,
	std::string_view hint)
{
	const NodeHandle row = dlg::formRow(ui_, parent, label, hint);
	const NodeHandle c = w::comboBox(ui_, row, dlg::joinLines(items), 0);
	ui_.setCss(c, "flex-grow", "1");
	if (!hint.empty()) w::setTooltip(ui_, c, hint);
	return reg(name, c);
}

NodeHandle SettingsDialog::pathRow(
	NodeHandle parent,
	std::string_view name,
	std::string_view label,
	FilePrompt::Mode mode,
	std::string_view promptTitle,
	std::string_view filter)
{
	const NodeHandle row = dlg::formRow(ui_, parent, label);
	const NodeHandle e = dlg::growEdit(ui_, row);
	const NodeHandle b = w::toolButton(ui_, row, "…");
	std::string btnName(name);
	if (btnName.size() > 4 && btnName.ends_with("Edit")) btnName.resize(btnName.size() - 4);
	reg(btnName + "Btn", b);
	const std::size_t idx = pathRows_.size();
	pathRows_.push_back({e, mode, std::string(promptTitle), std::string(filter)});
	ui_.connect(b, SignalKind::Clicked, [this, idx](UiContext&, NodeHandle, const SignalArgs&) {
		const PathRow& r = pathRows_[idx];
		const NodeHandle edit = r.edit;
		files_.show(r.mode, r.title, dlg::editText(ui_, edit), r.filter, [this, edit](const std::string& p) {
			dlg::setEditText(ui_, edit, p);
		});
	});
	return reg(name, e);
}

bool SettingsDialog::checked(std::string_view name) const
{
	return ui_.checked(field(name));
}
void SettingsDialog::setChecked(std::string_view name, bool on)
{
	ui_.setChecked(field(name), on);
}
double SettingsDialog::number(std::string_view name, int decimals) const
{
	return dlg::spinValue(ui_, field(name), decimals);
}
void SettingsDialog::setNumber(std::string_view name, double v)
{
	ui_.setValue(field(name), float(v), false);
}
std::string SettingsDialog::text(std::string_view name) const
{
	return dlg::editText(ui_, field(name));
}
void SettingsDialog::setText(std::string_view name, std::string_view v)
{
	dlg::setEditText(ui_, field(name), v);
}

NodeHandle SettingsDialog::field(std::string_view name) const
{
	const auto it = fields_.find(name);
	return it == fields_.end() ? NodeHandle{} : it->second;
}

std::vector<std::string> SettingsDialog::fieldNames() const
{
	std::vector<std::string> out;
	for (const auto& [k, v]: fields_)
		out.push_back(k);
	return out;
}

int SettingsDialog::currentTab() const
{
	return dlg::indexOf(ui_, w::tabWidgetBar(ui_, tabs_));
}

void SettingsDialog::setCurrentTab(int tab)
{
	ui_.setValue(w::tabWidgetBar(ui_, tabs_), float(tab), true);
}

std::vector<std::string> SettingsDialog::tabTitles() const
{
	std::vector<std::string> out;
	std::string_view t = kTabTitles;
	while (!t.empty())
	{
		const auto nl = t.find('\n');
		out.emplace_back(t.substr(0, nl));
		if (nl == std::string_view::npos) break;
		t.remove_prefix(nl + 1);
	}
	return out;
}

// ── Tabs ─────────────────────────────────────────────────────────────────────

void SettingsDialog::buildGeneral(NodeHandle p)
{
	dlg::sectionHeader(ui_, p, "Appearance");
	combo(
		p,
		"themeCombo",
		"Theme",
		model::settingsThemeItems(),
		"Catppuccin Mocha (dark) or Latte (light); System Default follows the OS when supported");

	const NodeHandle fontRow = dlg::formRow(ui_, p, "Editor font");
	ui_.setCss(fontRow, "align-items", "start");
	fontFamilies_ = model::settingsEditorFontFamilies(settings_.general.editorFont.family);
	fontPicker_ = w::fontPicker(ui_, fontRow, dlg::joinLines(fontFamilies_), float(settings_.general.fontSize));
	ui_.setCss(fontPicker_, "flex-grow", "1");
	reg("fontCombo", fontPicker_);
	fontList_ = ui_.firstChild(fontPicker_);
	// The picker's size spin box is the "Font size (pt)" field (Qt range 6..32).
	if (const NodeHandle sizeRow = ui_.nextSibling(fontList_); ui_.alive(sizeRow))
	{
		const NodeHandle lbl = ui_.firstChild(sizeRow);
		ui_.setText(lbl, "Font size (pt)");
		fontSizeSpin_ = ui_.nextSibling(lbl);
		ui_.setRange(fontSizeSpin_, 6.f, 32.f, 1.f);
		reg("fontSizeSpin", fontSizeSpin_);
	}
	ui_.connect(fontPicker_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs&) {
		onFontPicked();
	});

	dlg::sectionHeader(ui_, p, "Internationalisation");
	std::vector<std::string> langs;
	for (const auto& e: model::settingsLanguageItems())
		langs.push_back(e.label);
	combo(p, "langCombo", "Language", langs, "Requires restart");

	dlg::sectionHeader(ui_, p, "Editor");
	check(p, "lineNumCheck", "Show line numbers");
	check(p, "wordWrapCheck", "Word wrap");

	dlg::sectionHeader(ui_, p, "Session");
	check(
		p,
		"restoreCheck",
		"Restore last session on startup",
		"Re-open the last binary from File \xE2\x86\x92 Open Binary when RetDec starts.");
}

void SettingsDialog::buildAnalysis(NodeHandle p)
{
	dlg::noteLabel(ui_, p, "F5 runs retdec-decompiler externally; these toggles affect in-process analysis only.");
	dlg::sectionHeader(ui_, p, "Pipeline Stages");
	check(p, "enableTypingCheck", "Type inference");
	check(p, "enablePatternCheck", "Pattern matching");
	check(p, "enableConcurCheck", "Concurrency detection");
	check(p, "enableCudaCheck", "CUDA host recovery");
	check(p, "enableSerialCheck", "Serialisation detection");
	check(p, "enableModuleCheck", "Module clustering");
	check(p, "enableCxxCheck", "C++ lifter");

	dlg::sectionHeader(ui_, p, "Confidence Thresholds");
	spin(p, "typeConfSpin", "Type inference minimum", 0.f, 1.f, 0.05f);
	spin(p, "patternConfSpin", "Pattern matching minimum", 0.f, 1.f, 0.05f);
	spin(p, "recovConfSpin", "Recovery minimum", 0.f, 1.f, 0.05f);

	dlg::sectionHeader(ui_, p, "Resources");
	spin(p, "maxTimeSpin", "Max analysis time", 0.f, 3600.f, 1.f, {}, "s", "Unlimited");
	spin(p, "threadCountSpin", "Thread count", 0.f, 256.f, 1.f, "0 = hardware_concurrency", {}, "Auto");
}

void SettingsDialog::buildCuda(NodeHandle p)
{
	dlg::sectionHeader(ui_, p, "Device");
	combo(p, "clDeviceCombo", "CUDA device", model::settingsCudaDeviceItems());
	check(p, "useGPUCheck", "Prefer GPU over CPU");
	spin(p, "wgSizeSpin", "Work group size", 32.f, 1024.f, 32.f);

	dlg::sectionHeader(ui_, p, "Kernel Cache");
	pathRow(p, "kernelCacheEdit", "Kernel cache dir", FilePrompt::Mode::Directory, "Kernel cache directory", {});

	dlg::sectionHeader(ui_, p, "Profiling");
	check(p, "clProfilingCheck", "Enable cl_event profiling");
}

void SettingsDialog::buildMl(NodeHandle p)
{
	dlg::sectionHeader(ui_, p, "Model");
	pathRow(p, "modelPathEdit", "Model file (.gguf)", FilePrompt::Mode::Open, "Select GGUF model", "*.gguf");
	dlg::noteLabel(
		ui_,
		p,
		"When this GGUF path exists on disk, Run Full Analysis sets RETDEC_NEURAL_REFINE on the decompiler "
		"child (llama.cpp). Context length and max new tokens come from this tab. CPU device forces "
		"RETDEC_NEURAL_N_GPU_LAYERS=0; GPU/Auto use -1. Empty path leaves refinement off.");
	combo(p, "quantCombo", "Quantisation level", model::settingsQuantItems());
	combo(p, "inferDevCombo", "Inference device", model::settingsInferenceDeviceItems());

	dlg::sectionHeader(ui_, p, "Generation Parameters");
	spin(p, "tempSpin", "Temperature", 0.f, 2.f, 0.05f);
	spin(p, "topPSpin", "Top-P", 0.f, 1.f, 0.05f);
	spin(p, "topKSpin", "Top-K", 1.f, 200.f, 1.f);
	spin(p, "maxTokensSpin", "Max new tokens", 64.f, 8192.f, 1.f);
	spin(p, "contextLenSpin", "Context length", 512.f, 131072.f, 1.f);
	check(p, "streamCheck", "Stream output tokens");

	const NodeHandle shaRow = dlg::formRow(ui_, p, "Model SHA-256");
	const NodeHandle sha = reg("mlModelSha256", dlg::growEdit(ui_, shaRow));
	w::setPlaceholderText(ui_, sha, "optional SHA-256 of the GGUF");
	check(
		p,
		"mlBatchRefine",
		"Batch refine (unused)",
		"Reserved. Does not set RETDEC_NEURAL_BATCH (removed until real batching exists).");
}

void SettingsDialog::buildRecovery(NodeHandle p)
{
	dlg::sectionHeader(ui_, p, "Enabled Detectors");
	check(p, "detectSTLCheck", "STL containers / algorithms");
	check(p, "detectCryptoCheck", "Cryptographic primitives");
	check(p, "detectPatternCheck", "Code patterns");
	check(p, "detectConcurCheck", "Concurrency primitives");
	check(p, "detectCudaCheck", "CUDA host API");
	check(p, "detectRTTICheck", "C++ RTTI / typeinfo");
	check(p, "detectEHCheck", "Exception handling");
	check(p, "detectVirtCheck", "Virtual dispatch / vtables");

	dlg::sectionHeader(ui_, p, "Confidence Thresholds");
	spin(p, "stlConfSpin", "STL confidence", 0.f, 1.f, 0.05f);
	spin(p, "cryptoConfSpin", "Crypto confidence", 0.f, 1.f, 0.05f);
	spin(p, "patConf2Spin", "Pattern confidence", 0.f, 1.f, 0.05f);
	spin(p, "concurConfSpin", "Concurrency confidence", 0.f, 1.f, 0.05f);
}

void SettingsDialog::buildAdvanced(NodeHandle p)
{
	dlg::sectionHeader(ui_, p, "Diagnostics");
	combo(p, "verbCombo", "Verbosity", model::settingsVerbosityItems());

	dlg::sectionHeader(ui_, p, "Dump Options");
	check(p, "dumpIRCheck", "Dump SSA IR");
	check(p, "dumpASMCheck", "Dump assembly");
	check(p, "dumpCFGCheck", "Dump CFG (DOT)");
	check(p, "dumpSSACheck", "Dump SSA after each stage");
	check(p, "colorOutCheck", "Colour terminal output");
	pathRow(p, "irDumpEdit", "IR dump path", FilePrompt::Mode::Directory, "IR dump directory", {});
	pathRow(
		p,
		"intermDirEdit",
		"Intermediate output dir",
		FilePrompt::Mode::Directory,
		"Intermediate output directory",
		{});

	dlg::sectionHeader(ui_, p, "Output");
	spin(p, "maxFuncSpin", "Max functions", 0.f, 100000.f, 1.f, "0 = decompile all functions", {}, "All");
	check(p, "demangleCheck", "Demangle symbol names");
}

void SettingsDialog::buildDecompiler(NodeHandle p)
{
	dlg::sectionHeader(ui_, p, "LLVM pipeline (CLI)");
	std::vector<std::string> profiles;
	for (const auto& e: model::settingsProfileItems())
		profiles.push_back(e.label);
	combo(
		p,
		"decompileProfileCombo",
		"Decompile profile",
		profiles,
		"Passed as --profile to retdec-decompiler. Analysis \xE2\x86\x92 Fast decompile overrides to fast for the next "
		"run.");
	check(
		p,
		"useCustomLlvmPassesCheck",
		"Use custom LLVM pass list when running \"Run Full Analysis\" (adds --llvm-passes-json to retdec-decompiler)");
	const NodeHandle btnRow = dlg::hrow(ui_, p);
	const NodeHandle all = reg("llvmPassesAllBtn", w::button(ui_, btnRow, "Enable all passes"));
	const NodeHandle none = reg("llvmPassesNoneBtn", w::button(ui_, btnRow, "Disable all passes"));
	const NodeHandle defs = reg("llvmPassesDefaultBtn", w::button(ui_, btnRow, "Defaults (stock pipeline)"));
	ui_.connect(all, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { enableAllPasses(); });
	ui_.connect(none, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { disableAllPasses(); });
	ui_.connect(defs, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { defaultPasses(); });

	dlg::sectionHeader(ui_, p, "Output location");
	pathRow(
		p,
		"decompileOutputDirEdit",
		"Decompile output directory",
		FilePrompt::Mode::Directory,
		"Decompile output directory",
		{});
	dlg::noteLabel(
		ui_,
		p,
		"Set a folder outside OneDrive to keep large `.gui-decompiled.*` artifacts out of sync. Leave empty "
		"to write beside the binary.");

	dlg::sectionHeader(ui_, p, "Output language");
	std::vector<std::string> langs;
	for (const auto& e: model::settingsOutputLangItems())
		langs.push_back(e.label);
	combo(p, "outputLangCombo", "Preferred output language", langs);
	dlg::noteLabel(
		ui_,
		p,
		"Passed as --output-lang for native binaries. Managed inputs (.pyc, .class, …) use format-specific "
		"emitters.");

	dlg::sectionHeader(ui_, p, "Console");
	check(p, "liveConsoleTailCheck", "Stream decompiler log to console (may slow decompile on huge logs)");

	dlg::sectionHeader(ui_, p, "Configuration file");
	pathRow(
		p,
		"decompilerConfigEdit",
		"Optional --config JSON",
		FilePrompt::Mode::Open,
		"Decompiler configuration JSON",
		model::kJsonFileFilter);

	auto names = model::settingsLlvmPassNames();
	if (!names)
	{
		passError_ = names.error();
		w::label(ui_, p, passError_);
		return;
	}
	passNames_ = std::move(*names);
	dlg::noteLabel(ui_, p, "Each name controls every occurrence of that pass in the ordered pipeline.");
	const NodeHandle list = w::panel(ui_, p);
	ui_.setCss(list, "min-height", "280px");
	ui_.setCss(list, "gap", "4px");
	passChecks_.reserve(passNames_.size());
	for (const std::string& n: passNames_)
		passChecks_.push_back(w::checkBox(ui_, list, n, true));
}

void SettingsDialog::buildPlugins(NodeHandle p)
{
	w::label(ui_, p, "Search paths");
	pathList_ = w::listView(ui_, p, "", -1, "pluginSearchPaths");
	ui_.setCss(pathList_, "height", "96px");
	ui_.setCss(pathList_, "flex-shrink", "0");
	reg("pluginSearchPaths", pathList_);
	const NodeHandle pathBtns = dlg::hrow(ui_, p);
	const NodeHandle add = reg("pluginSearchAdd", w::button(ui_, pathBtns, "Add path…"));
	const NodeHandle remove = reg("pluginSearchRemove", w::button(ui_, pathBtns, "Remove"));
	const NodeHandle rescan = reg("pluginSearchRescan", w::button(ui_, pathBtns, "Rescan"));

	w::label(ui_, p, "Installed Plugins");
	pluginList_ = w::listView(ui_, p, "", -1, "pluginList");
	ui_.setCss(pluginList_, "flex-grow", "1");
	ui_.setCss(pluginList_, "min-height", "80px");
	reg("pluginList", pluginList_);
	const NodeHandle btns = dlg::hrow(ui_, p);
	pluginEnabled_ = reg("pluginEnabledCheck", w::checkBox(ui_, btns, "Enabled"));
	ui_.setDisabled(pluginEnabled_, true);
	const NodeHandle install = reg("installPluginBtn", w::button(ui_, btns, "Install Plugin…"));
	const NodeHandle unload = reg("unloadPluginBtn", w::button(ui_, btns, "Unload Selected"));
	pluginDetail_ = dlg::noteLabel(ui_, p, "Select a plugin to see details.");
	ui_.setCss(pluginDetail_, "padding", "4px");

	ui_.connect(add, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		files_.show(FilePrompt::Mode::Directory, "Plugin search path", {}, {}, [this](const std::string& path) {
			addSearchPath(path);
		});
	});
	ui_.connect(remove, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		removeSelectedSearchPath();
	});
	ui_.connect(rescan, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { rescanPlugins(); });
	ui_.connect(install, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		files_.show(
			FilePrompt::Mode::Open,
			"Install plugin",
			{},
			model::pluginLibraryFilter(),
			[this](const std::string& path) {
				if (auto r = installPlugin(path); !r) warn(r.error());
			});
	});
	ui_.connect(unload, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		unloadSelectedPlugin();
	});
	ui_.connect(pluginList_, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		selectPlugin(int(a.value));
	});
	ui_.connect(pluginEnabled_, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		setPluginEnabled(dlg::indexOf(ui_, pluginList_), a.flag);
	});
}

// ── populate / apply ─────────────────────────────────────────────────────────

void SettingsDialog::populateFromSettings()
{
	populating_ = true;
	const model::AppSettings& s = settings_;

	// General
	dlg::setIndex(ui_, field("themeCombo"), int(s.general.theme));
	{
		const std::string fam = s.general.editorFont.family;
		auto it = std::find(fontFamilies_.begin(), fontFamilies_.end(), fam);
		if (it == fontFamilies_.end() && !fam.empty())
		{
			fontFamilies_.insert(fontFamilies_.begin(), fam);
			ui_.setText(fontList_, dlg::joinLines(fontFamilies_));
			it = fontFamilies_.begin();
		}
		const int idx = it == fontFamilies_.end() ? 0 : int(it - fontFamilies_.begin());
		ui_.setValue(fontSizeSpin_, float(s.general.fontSize), false);
		ui_.setValue(fontList_, float(idx), true); // refreshes the picker's family + preview
	}
	dlg::setIndex(ui_, field("langCombo"), model::settingsLanguageIndex(s.general.language));
	setChecked("lineNumCheck", s.general.showLineNumbers);
	setChecked("wordWrapCheck", s.general.wordWrap);
	setChecked("restoreCheck", s.general.restoreSession);

	// Analysis
	setChecked("enableTypingCheck", s.analysis.enableTyping);
	setChecked("enablePatternCheck", s.analysis.enablePatternMatch);
	setChecked("enableConcurCheck", s.analysis.enableConcurrency);
	setChecked("enableCudaCheck", s.analysis.enableCudaRecovery);
	setChecked("enableSerialCheck", s.analysis.enableSerialDetect);
	setChecked("enableModuleCheck", s.analysis.enableModuleCluster);
	setChecked("enableCxxCheck", s.analysis.enableCxxLifter);
	setNumber("typeConfSpin", s.analysis.minTypeConfidence);
	setNumber("patternConfSpin", s.analysis.minPatternConfidence);
	setNumber("recovConfSpin", s.analysis.minRecoveryConfidence);
	setNumber("maxTimeSpin", s.analysis.maxAnalysisTimeSecs);
	setNumber("threadCountSpin", s.analysis.threadCount);

	// CUDA
	setText("kernelCacheEdit", s.cuda.kernelCacheDir);
	setChecked("clProfilingCheck", s.cuda.enableProfiling);
	setChecked("useGPUCheck", s.cuda.useGPU);
	setNumber("wgSizeSpin", s.cuda.blockSize);

	// ML
	setText("modelPathEdit", s.ml.modelPath);
	dlg::setIndex(ui_, field("quantCombo"), int(s.ml.quantLevel));
	dlg::setIndex(ui_, field("inferDevCombo"), int(s.ml.inferenceDevice));
	setNumber("tempSpin", s.ml.temperature);
	setNumber("topPSpin", s.ml.topP);
	setNumber("topKSpin", s.ml.topK);
	setNumber("maxTokensSpin", s.ml.maxNewTokens);
	setNumber("contextLenSpin", s.ml.contextLength);
	setChecked("streamCheck", s.ml.streamOutput);
	setText("mlModelSha256", s.ml.modelSha256);
	setChecked("mlBatchRefine", s.ml.batchRefine);

	// Recovery
	setChecked("detectSTLCheck", s.recovery.detectSTL);
	setChecked("detectCryptoCheck", s.recovery.detectCrypto);
	setChecked("detectPatternCheck", s.recovery.detectPatterns);
	setChecked("detectConcurCheck", s.recovery.detectConcurrency);
	setChecked("detectCudaCheck", s.recovery.detectCuda);
	setChecked("detectRTTICheck", s.recovery.detectRTTI);
	setChecked("detectEHCheck", s.recovery.detectExceptions);
	setChecked("detectVirtCheck", s.recovery.detectVirtual);
	setNumber("stlConfSpin", s.recovery.stlConfidence);
	setNumber("cryptoConfSpin", s.recovery.cryptoConfidence);
	setNumber("patConf2Spin", s.recovery.patternConfidence);
	setNumber("concurConfSpin", s.recovery.concurrencyConfidence);

	// Advanced
	dlg::setIndex(ui_, field("verbCombo"), int(s.advanced.verbosity));
	setText("irDumpEdit", s.advanced.irDumpPath);
	setText("intermDirEdit", s.advanced.intermediateDir);
	setChecked("dumpIRCheck", s.advanced.dumpIR);
	setChecked("dumpASMCheck", s.advanced.dumpASM);
	setChecked("dumpCFGCheck", s.advanced.dumpCFG);
	setChecked("dumpSSACheck", s.advanced.dumpSSA);
	setChecked("colorOutCheck", s.advanced.colorOutput);
	setNumber("maxFuncSpin", s.advanced.maxFunctions);
	setChecked("demangleCheck", s.advanced.demangleNames);

	// Decompiler
	setChecked("useCustomLlvmPassesCheck", s.decompiler.useCustomLlvmPasses);
	for (std::size_t i = 0; i < passChecks_.size(); ++i)
	{
		const bool disabled =
			std::find(s.decompiler.llvmPassesDisabled.begin(), s.decompiler.llvmPassesDisabled.end(), passNames_[i])
			!= s.decompiler.llvmPassesDisabled.end();
		ui_.setChecked(passChecks_[i], !disabled);
	}
	setText("decompilerConfigEdit", s.decompiler.extraConfigPath);
	setText("decompileOutputDirEdit", s.decompiler.decompileOutputDir);
	setChecked("liveConsoleTailCheck", s.decompiler.liveConsoleTail);
	dlg::setIndex(ui_, field("outputLangCombo"), model::settingsOutputLangIndex(s.decompiler.outputLang));
	dlg::setIndex(ui_, field("decompileProfileCombo"), model::settingsProfileIndex(s.decompiler.decompileProfile));

	// Plugins
	searchPaths_ = s.plugins.searchPaths;
	refreshSearchPaths();
	refreshPluginList();
	populating_ = false;
}

void SettingsDialog::applyToSettings()
{
	model::AppSettings& s = settings_;

	// General
	s.general.theme =
		static_cast<model::GeneralSettings::Theme>(std::clamp(dlg::indexOf(ui_, field("themeCombo")), 0, 2));
	if (const std::string fam = w::fontPickerFamily(ui_, fontPicker_); !fam.empty()) s.general.editorFont.family = fam;
	s.general.fontSize = int(std::lround(ui_.value(fontSizeSpin_)));
	s.general.editorFont.pointSize = s.general.fontSize;
	s.general.language = model::settingsLanguageCode(dlg::indexOf(ui_, field("langCombo")));
	s.general.showLineNumbers = checked("lineNumCheck");
	s.general.wordWrap = checked("wordWrapCheck");
	s.general.restoreSession = checked("restoreCheck");

	// Analysis
	s.analysis.enableTyping = checked("enableTypingCheck");
	s.analysis.enablePatternMatch = checked("enablePatternCheck");
	s.analysis.enableConcurrency = checked("enableConcurCheck");
	s.analysis.enableCudaRecovery = checked("enableCudaCheck");
	s.analysis.enableSerialDetect = checked("enableSerialCheck");
	s.analysis.enableModuleCluster = checked("enableModuleCheck");
	s.analysis.enableCxxLifter = checked("enableCxxCheck");
	s.analysis.minTypeConfidence = number("typeConfSpin");
	s.analysis.minPatternConfidence = number("patternConfSpin");
	s.analysis.minRecoveryConfidence = number("recovConfSpin");
	s.analysis.maxAnalysisTimeSecs = int(number("maxTimeSpin", 0));
	s.analysis.threadCount = int(number("threadCountSpin", 0));

	// CUDA
	s.cuda.kernelCacheDir = text("kernelCacheEdit");
	s.cuda.enableProfiling = checked("clProfilingCheck");
	s.cuda.useGPU = checked("useGPUCheck");
	s.cuda.blockSize = int(number("wgSizeSpin", 0));

	// ML
	s.ml.modelPath = text("modelPathEdit");
	s.ml.quantLevel =
		static_cast<model::MLSettings::QuantLevel>(std::clamp(dlg::indexOf(ui_, field("quantCombo")), 0, 5));
	s.ml.inferenceDevice =
		static_cast<model::MLSettings::InferenceDevice>(std::clamp(dlg::indexOf(ui_, field("inferDevCombo")), 0, 2));
	s.ml.temperature = number("tempSpin");
	s.ml.topP = number("topPSpin");
	s.ml.topK = int(number("topKSpin", 0));
	s.ml.maxNewTokens = int(number("maxTokensSpin", 0));
	s.ml.contextLength = int(number("contextLenSpin", 0));
	s.ml.streamOutput = checked("streamCheck");
	s.ml.modelSha256 = std::string(model::dlgTrim(text("mlModelSha256")));
	s.ml.batchRefine = checked("mlBatchRefine");

	// Recovery
	s.recovery.detectSTL = checked("detectSTLCheck");
	s.recovery.detectCrypto = checked("detectCryptoCheck");
	s.recovery.detectPatterns = checked("detectPatternCheck");
	s.recovery.detectConcurrency = checked("detectConcurCheck");
	s.recovery.detectCuda = checked("detectCudaCheck");
	s.recovery.detectRTTI = checked("detectRTTICheck");
	s.recovery.detectExceptions = checked("detectEHCheck");
	s.recovery.detectVirtual = checked("detectVirtCheck");
	s.recovery.stlConfidence = number("stlConfSpin");
	s.recovery.cryptoConfidence = number("cryptoConfSpin");
	s.recovery.patternConfidence = number("patConf2Spin");
	s.recovery.concurrencyConfidence = number("concurConfSpin");

	// Advanced
	s.advanced.verbosity =
		static_cast<model::AdvancedSettings::Verbosity>(std::clamp(dlg::indexOf(ui_, field("verbCombo")), 0, 3));
	s.advanced.irDumpPath = text("irDumpEdit");
	s.advanced.intermediateDir = text("intermDirEdit");
	s.advanced.dumpIR = checked("dumpIRCheck");
	s.advanced.dumpASM = checked("dumpASMCheck");
	s.advanced.dumpCFG = checked("dumpCFGCheck");
	s.advanced.dumpSSA = checked("dumpSSACheck");
	s.advanced.colorOutput = checked("colorOutCheck");
	s.advanced.maxFunctions = int(number("maxFuncSpin", 0));
	s.advanced.demangleNames = checked("demangleCheck");

	// Decompiler
	s.decompiler.useCustomLlvmPasses = checked("useCustomLlvmPassesCheck");
	if (!passChecks_.empty())
	{
		std::vector<bool> on;
		for (NodeHandle c: passChecks_)
			on.push_back(ui_.checked(c));
		s.decompiler.llvmPassesDisabled = model::settingsDisabledPasses(passNames_, on);
	}
	s.decompiler.extraConfigPath = text("decompilerConfigEdit");
	s.decompiler.decompileOutputDir = std::string(model::dlgTrim(text("decompileOutputDirEdit")));
	s.decompiler.liveConsoleTail = checked("liveConsoleTailCheck");
	{
		const auto& langs = model::settingsOutputLangItems();
		const int i = std::clamp(dlg::indexOf(ui_, field("outputLangCombo")), 0, int(langs.size()) - 1);
		s.decompiler.outputLang = langs[std::size_t(i)].value;
		const auto& profiles = model::settingsProfileItems();
		const int j = std::clamp(dlg::indexOf(ui_, field("decompileProfileCombo")), 0, int(profiles.size()) - 1);
		s.decompiler.decompileProfile = profiles[std::size_t(j)].value;
	}

	// Plugins
	std::vector<std::string> enabled;
	for (const auto& [id, on]: pluginRows())
		if (on) enabled.push_back(id);
	s.plugins.enabledPlugins = std::move(enabled);
	s.plugins.searchPaths = model::settingsNonEmptyTrimmed(searchPaths_);

	s.notifySettingsChanged();
}

// ── Buttons ──────────────────────────────────────────────────────────────────

void SettingsDialog::show()
{
	populateFromSettings();
	dlg_.open();
}

void SettingsDialog::warn(const std::string& text)
{
	message_.show("Settings", text, MessageDialog::Kind::Warning);
	if (onWarning) onWarning("Settings", text);
}

bool SettingsDialog::apply()
{
	model::DecompilerSettings check;
	check.decompileOutputDir = text("decompileOutputDirEdit");
	check.extraConfigPath = text("decompilerConfigEdit");
	if (auto ok = model::validateSettingsForApply(check); !ok)
	{
		warn(ok.error());
		return false;
	}
	applyToSettings();
	(void)settings_.save();
	if (onApplied) onApplied();
	return true;
}

void SettingsDialog::ok()
{
	if (!apply()) return;
	dlg_.close();
	if (onAccepted) onAccepted();
}

void SettingsDialog::cancel()
{
	dlg_.close();
	if (onRejected) onRejected();
}

void SettingsDialog::restoreDefaults()
{
	settings_.resetToDefaults();
	populateFromSettings();
}

std::expected<void, std::string> SettingsDialog::exportTo(const std::filesystem::path& path)
{
	applyToSettings();
	return settings_.exportToFile(path);
}

std::expected<void, std::string> SettingsDialog::importFrom(const std::filesystem::path& path)
{
	auto r = settings_.importFromFile(path);
	if (!r) return r;
	populateFromSettings();
	return {};
}

void SettingsDialog::onFontPicked()
{
	if (populating_) return;
	// Live preview (Qt onFontChanged): the family applies at once with the
	// current size; Cancel does not undo it.
	const std::string fam = w::fontPickerFamily(ui_, fontPicker_);
	if (fam.empty() || fam == settings_.general.editorFont.family) return;
	settings_.general.editorFont.family = fam;
	settings_.general.editorFont.pointSize = int(std::lround(ui_.value(fontSizeSpin_)));
	settings_.general.fontSize = settings_.general.editorFont.pointSize;
	settings_.notifySettingsChanged();
}

// ── Decompiler tab ───────────────────────────────────────────────────────────

void SettingsDialog::enableAllPasses()
{
	for (NodeHandle c: passChecks_)
		ui_.setChecked(c, true);
}

void SettingsDialog::disableAllPasses()
{
	for (NodeHandle c: passChecks_)
		ui_.setChecked(c, false);
}

void SettingsDialog::defaultPasses()
{
	setChecked("useCustomLlvmPassesCheck", false);
	enableAllPasses();
}

// ── Plugins tab ──────────────────────────────────────────────────────────────

void SettingsDialog::refreshSearchPaths()
{
	dlg::setListItems(ui_, pathList_, searchPaths_);
}

void SettingsDialog::refreshPluginList()
{
	const int sel = dlg::indexOf(ui_, pluginList_);
	pluginIds_.clear();
	std::vector<std::string> rows;
	for (const auto& p: plugins_.plugins())
	{
		pluginIds_.push_back(p->metadata().id);
		rows.push_back(std::string(p->enabled() ? "[x] " : "[ ] ") + model::pluginRowLabel(p->metadata()));
	}
	ui_.setText(pluginList_, dlg::joinLines(rows));
	ui_.setValue(pluginList_, sel >= 0 && std::size_t(sel) < rows.size() ? float(sel) : -1.f);
	selectPlugin(dlg::indexOf(ui_, pluginList_));
}

std::vector<std::pair<std::string, bool>> SettingsDialog::pluginRows() const
{
	std::vector<std::pair<std::string, bool>> out;
	for (const std::string& id: pluginIds_)
	{
		const model::Plugin* p = plugins_.findPlugin(id);
		out.emplace_back(id, p && p->enabled());
	}
	return out;
}

void SettingsDialog::selectPlugin(int row)
{
	const bool valid = row >= 0 && std::size_t(row) < pluginIds_.size();
	ui_.setDisabled(pluginEnabled_, !valid);
	if (!valid)
	{
		ui_.setChecked(pluginEnabled_, false);
		ui_.setText(pluginDetail_, "Select a plugin to see details.");
		return;
	}
	if (dlg::indexOf(ui_, pluginList_) != row) ui_.setValue(pluginList_, float(row), false);
	const model::Plugin* p = plugins_.findPlugin(pluginIds_[std::size_t(row)]);
	if (!p) return;
	ui_.setChecked(pluginEnabled_, p->enabled());
	ui_.setText(pluginDetail_, model::pluginDetailText(p->metadata(), p->filePath().string()));
}

std::string SettingsDialog::pluginDetail() const
{
	return std::string(ui_.text(pluginDetail_));
}

void SettingsDialog::setPluginEnabled(int row, bool enabled)
{
	if (row < 0 || std::size_t(row) >= pluginIds_.size()) return;
	plugins_.setEnabled(pluginIds_[std::size_t(row)], enabled);
	std::vector<std::string> ids;
	for (const auto& [id, on]: pluginRows())
		if (on) ids.push_back(id);
	settings_.plugins.enabledPlugins = std::move(ids);
	(void)settings_.save();
	refreshPluginList();
}

void SettingsDialog::addSearchPath(const std::string& path)
{
	if (model::dlgTrim(path).empty()) return;
	searchPaths_.push_back(path);
	refreshSearchPaths();
}

void SettingsDialog::removeSelectedSearchPath()
{
	const int row = dlg::indexOf(ui_, pathList_);
	if (row < 0 || std::size_t(row) >= searchPaths_.size()) return;
	searchPaths_.erase(searchPaths_.begin() + row);
	refreshSearchPaths();
}

int SettingsDialog::rescanPlugins()
{
	const int n = plugins_.scanAndLoad(model::settingsNonEmptyTrimmed(searchPaths_));
	refreshPluginList();
	return n;
}

std::expected<std::string, std::string> SettingsDialog::installPlugin(const std::filesystem::path& file)
{
	auto id = plugins_.loadPlugin(file);
	if (!id) return id;
	plugins_.setEnabled(*id, true);
	refreshPluginList();
	return id;
}

void SettingsDialog::unloadSelectedPlugin()
{
	const int row = dlg::indexOf(ui_, pluginList_);
	if (row < 0 || std::size_t(row) >= pluginIds_.size()) return;
	plugins_.unloadPlugin(pluginIds_[std::size_t(row)]);
	ui_.setValue(pluginList_, -1.f);
	refreshPluginList();
}

} // namespace retdec::gui::ui
