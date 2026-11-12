/**
 * @file src/gui/ui/main_window.cpp
 * @brief MainWindow construction: layout, panels, actions, menus, toolbar,
 *        status bar, shortcuts, panel wiring and layout persistence.
 *
 * Actions: main_window_actions.cpp. Decompiling: main_window_decompile.cpp.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/main_window.h"

#include "main_window_internal.h"

#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/model/plugins.h"
#include "retdec/gui/ui/about_dialog.h"
#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/ai_assistant_panel.h"
#include "retdec/gui/ui/assembly_panel.h"
#include "retdec/gui/ui/batch_decompile_dialog.h"
#include "retdec/gui/ui/binary_browser_panel.h"
#include "retdec/gui/ui/c_style_dialog.h"
#include "retdec/gui/ui/call_graph_panel.h"
#include "retdec/gui/ui/cfg_panel.h"
#include "retdec/gui/ui/command_log_panel.h"
#include "retdec/gui/ui/decompiled_c_panel.h"
#include "retdec/gui/ui/diagnostics_panel.h"
#include "retdec/gui/ui/diff_panel.h"
#include "retdec/gui/ui/docs_viewer.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/function_list_panel.h"
#include "retdec/gui/ui/goto_address_dialog.h"
#include "retdec/gui/ui/inspect_panel.h"
#include "retdec/gui/ui/ir_panel.h"
#include "retdec/gui/ui/live_console_panel.h"
#include "retdec/gui/ui/progress_panel.h"
#include "retdec/gui/ui/prompt_dialog.h"
#include "retdec/gui/ui/raw_options_dialog.h"
#include "retdec/gui/ui/run_stage_dialog.h"
#include "retdec/gui/ui/settings_dialog.h"
#include "retdec/gui/ui/shortcuts_dialog.h"
#include "retdec/gui/ui/signature_studio_panel.h"
#include "retdec/gui/ui/strings_browser_panel.h"
#include "retdec/gui/ui/target_panel.h"
#include "retdec/gui/ui/tri_pane_code_view.h"
#include "retdec/gui/ui/triage_banner.h"
#include "retdec/gui/ui/type_hierarchy_panel.h"

#include <fuse/app/app.hpp>
#include <forge/os/clipboard.hpp>

#include <algorithm>
#include <format>
#include <mutex>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

// ─── System clipboard ────────────────────────────────────────────────────────

namespace {

struct ClipboardState
{
	std::mutex mutex;
	std::string local; ///< used when the system clipboard is unavailable
};

ClipboardState& clipboardState()
{
	static ClipboardState s;
	return s;
}

bool clipboardSet(void* user, std::string_view utf8)
{
	auto* s = static_cast<ClipboardState*>(user);
	{
		std::lock_guard lock(s->mutex);
		s->local.assign(utf8);
	}
	if (forge::os::clipboard::available()) (void)forge::os::clipboard::set(std::string(utf8));
	return true;
}

bool clipboardGet(void* user, std::string& out)
{
	auto* s = static_cast<ClipboardState*>(user);
	if (forge::os::clipboard::available())
	{
		auto r = forge::os::clipboard::get();
		if (r)
		{
			out = std::move(r.value);
			return !out.empty();
		}
	}
	std::lock_guard lock(s->mutex);
	out = s->local;
	return !out.empty();
}

constexpr std::string_view kDocTitles = "Decompiled C\nAssembly\nIR (SSA)\nCFG\nSynced (Asm | IR | C)";
constexpr std::string_view kWorkspaceTitles = "Strings\nInspect\nBinary\nTarget";
constexpr std::array<std::string_view, MainWindow::kOutCount> kOutputTitles = {"Console", "Problems", "History",
                                                                                 "Progress"};

std::vector<std::string> splitLines(std::string_view s)
{
	std::vector<std::string> out;
	std::size_t at = 0;
	while (at <= s.size())
	{
		std::size_t nl = s.find('\n', at);
		if (nl == std::string_view::npos) nl = s.size();
		out.emplace_back(s.substr(at, nl - at));
		at = nl + 1;
	}
	return out;
}

std::string joinLines(const std::vector<std::string>& v)
{
	std::string out;
	for (const auto& s: v)
	{
		if (!out.empty()) out += '\n';
		out += s;
	}
	return out;
}

void fill(UiContext& ui, NodeHandle n)
{
	ui.setCss(n, "flex-grow", "1");
	ui.setCss(n, "min-height", "0px");
	ui.setCss(n, "min-width", "0px");
	ui.setCss(n, "padding", "0px");
	ui.setCss(n, "gap", "0px");
}

std::string defaultDockLayout()
{
	return std::format(
		"fusedock 1\nsize left {}\nsize right {}\nsize bottom {}\n"
		"dock dock_functions left 0 open\ndock dock_workspace right 0 open\ndock dock_output bottom 0 open\n",
		detail::kDefaultLeftDockPx, detail::kDefaultRightDockPx, detail::kDefaultBottomDockPx);
}

} // namespace

void bindSystemClipboard(UiContext& ui)
{
	ClipboardHooks hooks;
	hooks.set = &clipboardSet;
	hooks.get = &clipboardGet;
	hooks.user = &clipboardState();
	ui.setClipboard(hooks);
}

// ─── Tool windows / question box ─────────────────────────────────────────────

MainWindow::ToolWindow::ToolWindow(UiContext& ui, const std::string& t, float w, float h)
	: title(t), width(w), height(h), dlg(std::make_unique<ModalDialog>(ui, t, w, h))
{
	body = widgets::panel(ui, dlg->body());
	ui.addClass(body, "retdec-toolwindow-body");
	fill(ui, body);
	const NodeHandle row = dlg::buttonRow(ui, dlg->body());
	closeButton = widgets::button(ui, row, "Close");
	ModalDialog* d = dlg.get();
	ui.connect(closeButton, SignalKind::Clicked, [d](UiContext&, NodeHandle, const SignalArgs&) { d->close(); });
}

MainWindow::ToolWindow::~ToolWindow() = default;

MainWindow::Question::Question(UiContext& ui): dlg(ui, "Question", 480.f, 0.f)
{
	text = widgets::label(ui, dlg.body(), "");
	ui.setCss(text, "flex-shrink", "0");
	buttons = dlg::buttonRow(ui, dlg.body());
}

// ─── Construction ────────────────────────────────────────────────────────────

MainWindow::MainWindow(UiContext& ui, MainWindowOptions options)
	: ui_(ui), opts_(options), settings_(options.settings ? *options.settings : model::AppSettings::instance())
{
	ensureRetdecUiClasses(ui_);
	if (ui_.registry().find("popup") == 0xFFFFFFFFu) widgets::install(ui_);

	createLayout();
	createActions();
	createMenus();
	createToolBar();
	createStatusBar();
	wirePanels();
	loadLayoutStore();
	restoreLayout();

	setTitle("RetDec \xE2\x80\x94 Retargetable Decompiler");
	settingsListener_ = settings_.addChangeListener([this] { applySettings(); });
	{
		std::weak_ptr<bool> alive = alive_;
		auto& pm = model::PluginManager::instance();
		pm.onLoaded.push_back([this, alive](const std::string&) {
			if (alive.lock()) pluginsChanged();
		});
		pm.onAboutToUnload.push_back([this, alive](const std::string& id) {
			if (alive.lock()) dropPluginUi(id);
		});
		pm.onUnloaded.push_back([this, alive](const std::string&) {
			if (alive.lock()) pluginsChanged();
		});
	}
	applySettings();
	updateCentralEmptyState();
	updateProjectFileActions();
	ui_.setFocus(root_);

	if (!opts_.headless)
	{
		// Qt: QTimer::singleShot(0, tryRestoreLastSession).
		ui_.startTimer(0.f, false, [this](UiContext&) {
			const auto& g = settings_.general;
			if (!g.restoreSession || g.lastBinaryPath.empty() || project_) return;
			std::error_code ec;
			if (!std::filesystem::is_regular_file(g.lastBinaryPath, ec)) return;
			openBinary(g.lastBinaryPath);
		});
	}
}

MainWindow::~MainWindow()
{
	*alive_ = false;
	shutdown();
	if (settingsListener_) settings_.removeChangeListener(settingsListener_);
	for (TimerHandle* t: {&pollTimer_, &elapsedTimer_, &statusTimer_, &intelTimer_})
		ui_.cancelTimer(*t);
	intelRun_.reset();
	run_.reset();
	question_.reset();
	toolWindows_.clear();
	// Panels and dialogs first (their destructors release their own nodes),
	// then the shell's subtree.
	ai_.reset();
	sigStudio_.reset();
	typeHierarchy_.reset();
	callGraph_.reset();
	compare_.reset();
	triageMenu_.reset();
	docsDlg_.reset();
	shortcutsDlg_.reset();
	aboutDlg_.reset();
	maxMemDlg_.reset();
	gotoLineDlg_.reset();
	gotoDlg_.reset();
	rawDlg_.reset();
	cStyleDlg_.reset();
	runStageDlg_.reset();
	batchDlg_.reset();
	settingsDlg_.reset();
	files_.reset();
	message_.reset();
	progress_.reset();
	commandLog_.reset();
	diagnostics_.reset();
	console_.reset();
	target_.reset();
	binary_.reset();
	inspect_.reset();
	strings_.reset();
	startEmpty_.reset();
	triage_.reset();
	triPane_.reset();
	cfg_.reset();
	ir_.reset();
	assembly_.reset();
	decompiledC_.reset();
	functionList_.reset();
	releasePanelRoot(ui_, root_);
}

void MainWindow::createLayout()
{
	rootHandler_ = std::make_unique<PanelEventHandler>();
	rootHandler_->handle = [this](EventContext& ec, const Event& e) { return handleRootEvent(ec, e); };
	root_ = createPanelRoot(ui_, ui_.root(), rootHandler_.get(), "retdec-main");
	fill(ui_, root_);
	ui_.setCss(root_, "flex-direction", "column");
	ui_.setFocusable(root_, true);
	ui_.setA11y(root_, A11yRole::Window, "RetDec");

	menuBar_ = widgets::menuBar(ui_, root_, "retdec-menubar");
	ui_.setCss(menuBar_, "flex-shrink", "0");
	toolBar_ = widgets::panel(ui_, root_, "mainToolbar");
	ui_.addClass(toolBar_, "retdec-maintoolbar");
	ui_.setCss(toolBar_, "flex-direction", "row");
	ui_.setCss(toolBar_, "flex-shrink", "0");
	ui_.setCss(toolBar_, "align-items", "center");

	dockArea_ = widgets::dockArea(ui_, root_, "retdec-dockarea");
	ui_.setCss(dockArea_, "min-height", "0px");

	// ── CENTRAL: triage banner + start screen / document tabs ────────────────
	central_ = widgets::dockRegion(ui_, dockArea_, widgets::DockSide::Center);
	ui_.addClass(central_, "retdec-central");
	ui_.setCss(central_, "flex-direction", "column");
	ui_.setCss(central_, "min-width", "0px");
	ui_.setCss(central_, "min-height", "0px");
	triage_ = std::make_unique<TriageBanner>(ui_, central_);
	ui_.setCss(triage_->root(), "flex-shrink", "0");
	centralStack_ = widgets::panel(ui_, central_, "centralStack");
	fill(ui_, centralStack_);

	startPage_ = widgets::panel(ui_, centralStack_, "startPage");
	fill(ui_, startPage_);
	ui_.addClass(startPage_, "retdec-start");
	ui_.setCss(startPage_, "align-items", "center");
	ui_.setCss(startPage_, "justify-content", "center");
	startEmpty_ = std::make_unique<EmptyState>(ui_, startPage_);
	ui_.setCss(startEmpty_->root(), "flex-grow", "0");
	startEmpty_->setTitle("Open a binary to begin");
	startEmpty_->setHint("Drag and drop an executable here, or use the buttons below.");
	startEmpty_->setIcon("\xE2\x8C\x82"); // ⌂
	const NodeHandle startActions = widgets::panel(ui_, startPage_);
	ui_.addClass(startActions, "retdec-start-actions");
	ui_.setCss(startActions, "flex-direction", "row");
	ui_.setCss(startActions, "gap", "12px");
	startOpenBinary_ = widgets::button(ui_, startActions, "Open Binary\xE2\x80\xA6", "startOpenBinary");
	ui_.addClass(startOpenBinary_, "primary");
	startOpenProject_ = widgets::button(ui_, startActions, "Open Project\xE2\x80\xA6", "startOpenProject");
	ui_.connect(startOpenBinary_, SignalKind::Clicked,
	            [this](UiContext&, NodeHandle, const SignalArgs&) { triggerAction("file.open_binary"); });
	ui_.connect(startOpenProject_, SignalKind::Clicked,
	            [this](UiContext&, NodeHandle, const SignalArgs&) { triggerAction("file.open_project"); });

	docTabs_ = widgets::tabWidget(ui_, centralStack_, kDocTitles, "documentTabs");
	fill(ui_, docTabs_);
	docHost_ = docTabs_;
	auto page = [this](NodeHandle tabs, u32 i) {
		const NodeHandle p = widgets::tabPage(ui_, tabs, i);
		fill(ui_, p);
		return p;
	};
	decompiledC_ = std::make_unique<DecompiledCPanel>(ui_, page(docTabs_, kDocDecompiledC));
	assembly_ = std::make_unique<AssemblyPanel>(ui_, page(docTabs_, kDocAssembly));
	ir_ = std::make_unique<IRPanel>(ui_, page(docTabs_, kDocIR));
	cfg_ = std::make_unique<CfgPanel>(ui_, page(docTabs_, kDocCFG));
	triPane_ = std::make_unique<TriPaneCodeView>(ui_, page(docTabs_, kDocSynced));
	for (NodeHandle r: {decompiledC_->root(), assembly_->root(), ir_->root(), cfg_->root(), triPane_->root()})
		ui_.setCss(r, "flex-grow", "1");

	// ── LEFT: Functions ──────────────────────────────────────────────────────
	dockFunctions_ = widgets::dockWidget(ui_, dockArea_, "Functions", widgets::DockSide::Left, "dock_functions");
	ui_.setA11y(dockFunctions_, A11yRole::Group, "Functions panel");
	fill(ui_, widgets::dockBody(ui_, dockFunctions_));
	functionList_ = std::make_unique<FunctionListPanel>(ui_, widgets::dockBody(ui_, dockFunctions_));
	ui_.setCss(functionList_->root(), "flex-grow", "1");

	// ── RIGHT: Workspace — Strings | Inspect | Binary | Target ───────────────
	dockWorkspace_ = widgets::dockWidget(ui_, dockArea_, "Workspace", widgets::DockSide::Right, "dock_workspace");
	ui_.setA11y(dockWorkspace_, A11yRole::Group, "Workspace panel");
	fill(ui_, widgets::dockBody(ui_, dockWorkspace_));
	wsTabs_ = widgets::tabWidget(ui_, widgets::dockBody(ui_, dockWorkspace_), kWorkspaceTitles, "workspaceTabs");
	fill(ui_, wsTabs_);
	strings_ = std::make_unique<StringsBrowserPanel>(ui_, page(wsTabs_, kWsStrings));
	inspect_ = std::make_unique<InspectPanel>(ui_, page(wsTabs_, kWsInspect));
	binary_ = std::make_unique<BinaryBrowserPanel>(ui_, page(wsTabs_, kWsBinary));
	target_ = std::make_unique<TargetPanel>(ui_, page(wsTabs_, kWsTarget), settings_);
	for (NodeHandle r: {strings_->root(), inspect_->root(), binary_->root(), target_->root()})
		ui_.setCss(r, "flex-grow", "1");

	// ── BOTTOM: Output — Console | Problems | History | Progress ─────────────
	dockOutput_ = widgets::dockWidget(ui_, dockArea_, "Output", widgets::DockSide::Bottom, "dock_output");
	ui_.setA11y(dockOutput_, A11yRole::Group, "Output panel");
	fill(ui_, widgets::dockBody(ui_, dockOutput_));
	std::string outTitles;
	for (std::string_view t: kOutputTitles)
	{
		if (!outTitles.empty()) outTitles += '\n';
		outTitles += t;
	}
	outTabs_ = widgets::tabWidget(ui_, widgets::dockBody(ui_, dockOutput_), outTitles, "outputTabs");
	fill(ui_, outTabs_);
	console_ = std::make_unique<LiveConsolePanel>(ui_, page(outTabs_, kOutConsole));
	diagnostics_ = std::make_unique<DiagnosticsPanel>(ui_, page(outTabs_, kOutProblems));
	commandLog_ = std::make_unique<CommandLogPanel>(ui_, page(outTabs_, kOutHistory));
	progress_ = std::make_unique<ProgressPanel>(ui_, page(outTabs_, kOutProgress));
	for (NodeHandle r: {console_->root(), diagnostics_->root(), commandLog_->root(), progress_->root()})
		ui_.setCss(r, "flex-grow", "1");
	setOutputTabVisible(kOutProgress, false);

	(void)widgets::dockRestoreLayout(ui_, dockArea_, defaultDockLayout());

	// ── Tool windows (Qt: separate top-level windows) ────────────────────────
	callGraph_ = std::make_unique<CallGraphPanel>(ui_, toolWindow("Call Graph", 900.f, 700.f).body);
	typeHierarchy_ = std::make_unique<TypeHierarchyPanel>(ui_, toolWindow("Type Hierarchy", 700.f, 600.f).body);
	sigStudio_ = std::make_unique<SignatureStudioPanel>(ui_, toolWindow("Signature Studio", 900.f, 600.f).body);
	ai_ = std::make_unique<AIAssistantPanel>(ui_, toolWindow("AI Assistant", 720.f, 640.f).body,
	                                         model::defaultAssistantBackendFactory(), settings_);
	compare_ = std::make_unique<DiffPanel>(ui_, toolWindow("Compare", 1100.f, 700.f).body);
	for (NodeHandle r: {callGraph_->root(), typeHierarchy_->root(), sigStudio_->root(), ai_->root(), compare_->root()})
		ui_.setCss(r, "flex-grow", "1");

	// ── Dialogs ──────────────────────────────────────────────────────────────
	message_ = std::make_unique<MessageDialog>(ui_);
	files_ = std::make_unique<FilePrompt>(ui_);
	settingsDlg_ = std::make_unique<SettingsDialog>(ui_, settings_, model::PluginManager::instance());
	batchDlg_ = std::make_unique<BatchDecompileDialog>(ui_);
	runStageDlg_ = std::make_unique<RunStageDialog>(ui_);
	cStyleDlg_ = std::make_unique<CStyleDialog>(ui_);
	rawDlg_ = std::make_unique<RawOptionsDialog>(ui_);
	gotoDlg_ = std::make_unique<GotoAddressDialog>(ui_);
	gotoLineDlg_ = std::make_unique<GotoLineDialog>(ui_);
	maxMemDlg_ = std::make_unique<MaxMemoryDialog>(ui_);
	aboutDlg_ = std::make_unique<AboutDialog>(ui_);
	shortcutsDlg_ = std::make_unique<ShortcutsDialog>(ui_);
	docsDlg_ = std::make_unique<DocsViewer>(ui_);
	triageMenu_ = std::make_unique<ContextMenu>(ui_);
	question_ = std::make_unique<Question>(ui_);
}

MainWindow::ToolWindow& MainWindow::toolWindow(const std::string& title, float w, float h)
{
	for (auto& t: toolWindows_)
		if (t->title == title) return *t;
	toolWindows_.push_back(std::make_unique<ToolWindow>(ui_, title, w, h));
	return *toolWindows_.back();
}

void MainWindow::showToolWindow(const std::string& title)
{
	ToolWindow* tw = nullptr;
	for (auto& t: toolWindows_)
		if (t->title == title) tw = t.get();
	if (!tw) return;
	// Fit the card into the window (the Qt default sizes are for a large screen).
	const Size vp = ui_.viewport();
	const float wpx = std::max(320.f, std::min(tw->width, vp.w - 40.f));
	const float hpx = std::max(240.f, std::min(tw->height, vp.h - 40.f));
	ui_.setCss(tw->dlg->card(), "width", dlg::px(wpx));
	ui_.setCss(tw->dlg->card(), "height", dlg::px(hpx));
	if (!tw->dlg->isOpen()) tw->dlg->open();
}

bool MainWindow::toolWindowOpen(std::string_view title) const
{
	for (const auto& t: toolWindows_)
		if (t->title == title) return t->dlg->isOpen();
	return false;
}

void MainWindow::closeToolWindows()
{
	for (auto& t: toolWindows_)
		if (t->dlg->isOpen()) t->dlg->close();
}

std::string MainWindow::pluginPanelText(std::string_view title) const
{
	for (const auto& t: toolWindows_)
		if (t->title == title) return t->pluginText;
	return {};
}

// ─── Actions ─────────────────────────────────────────────────────────────────

MainWindow::Action& MainWindow::addAction(Action a)
{
	actions_.push_back(std::move(a));
	return actions_.back();
}

MainWindow::Action* MainWindow::findAction(std::string_view id)
{
	for (auto& a: actions_)
		if (a.id == id) return &a;
	return nullptr;
}

const MainWindow::Action* MainWindow::action(std::string_view id) const
{
	for (const auto& a: actions_)
		if (a.id == id) return &a;
	return nullptr;
}

std::vector<std::string> MainWindow::actionIds() const
{
	std::vector<std::string> out;
	for (const auto& a: actions_)
		out.push_back(a.id);
	return out;
}

bool MainWindow::actionEnabled(std::string_view id) const
{
	const Action* a = action(id);
	return a && a->enabled;
}

bool MainWindow::actionChecked(std::string_view id) const
{
	const Action* a = action(id);
	return a && a->checked;
}

void MainWindow::setActionEnabled(std::string_view id, bool on)
{
	if (Action* a = findAction(id); a && a->enabled != on)
	{
		a->enabled = on;
		refreshToolBar();
	}
}

void MainWindow::setActionChecked(std::string_view id, bool on)
{
	if (Action* a = findAction(id)) a->checked = on;
}

bool MainWindow::triggerAction(std::string_view id)
{
	Action* a = findAction(id);
	if (!a || !a->enabled) return false;
	if (a->checkable) a->checked = !a->checked;
	// Copy: the action may rebuild the action list (it does not today) or
	// destroy what captured it.
	const std::function<void()> run = a->run;
	if (run) run();
	return true;
}

bool MainWindow::handleShortcut(Key key, u8 mods)
{
	if (key == Key::None || key == Key::Other) return false;
	const u8 m = mods & (kModCtrl | kModShift | kModAlt);
	for (const auto& a: actions_)
	{
		if (a.key != key || a.mods != m) continue;
		if (!a.enabled) return true; // a disabled shortcut is still consumed (Qt)
		const std::string id = a.id;
		triggerAction(id);
		return true;
	}
	return false;
}

bool MainWindow::handleRootEvent(EventContext& ec, const Event& e)
{
	if (e.type != EventType::KeyDown) return false;
	if (ec.phase != Phase::Capture && ec.phase != Phase::Target) return false;
	// Only modified keys and function keys are global; plain letters belong to
	// the focused panel (Assembly G / F, find fields).
	const bool fkey = e.key == Key::F5 || e.key == Key::F6;
	if (!(e.mods & kModCtrl) && !fkey) return false;
	return handleShortcut(e.key, e.mods);
}

void MainWindow::createActions()
{
	const std::string ell = "\xE2\x80\xA6"; // …
	auto act = [this](std::string id, std::string label, std::function<void()> run, std::string shortcut = {},
	                  Key key = Key::None, u8 mods = 0) -> Action& {
		Action a;
		a.id = std::move(id);
		a.label = std::move(label);
		a.shortcut = std::move(shortcut);
		a.key = key;
		a.mods = mods;
		a.run = std::move(run);
		return addAction(std::move(a));
	};
	auto toggle = [&](std::string id, std::string label, std::string tip, std::function<void(bool)> set) -> Action& {
		Action& a = act(std::move(id), std::move(label), {});
		a.checkable = true;
		a.toolTip = std::move(tip);
		const std::string aid = a.id;
		a.run = [this, aid, set] { set(actionChecked(aid)); };
		return a;
	};
	const u8 C = kModCtrl, CS = kModCtrl | kModShift;

	// ── File ────────────────────────────────────────────────────────────────
	act("file.open_binary", "Open Binary" + ell, [this] { onOpenBinary(); }, "Ctrl+O", Key::O, C);
	act("file.open_project", "Open Project" + ell, [this] { onOpenProject(); }, "Ctrl+Shift+O", Key::O, CS);
	act("file.save_project", "Save Project", [this] { onSaveProject(); }, "Ctrl+Shift+S", Key::S, CS);
	act("file.save_project_as", "Save Project As" + ell, [this] { onSaveProjectAs(); }, "Ctrl+Shift+A", Key::A, CS);
	act("file.save_decompiled", "Save Decompiled" + ell, [this] { onSaveDecompiled(); }, "Ctrl+S", Key::S, C);
	act("file.export_cmake", "Export CMakeLists.txt" + ell, [this] { onExportCMake(); });
	act("file.export_bundle", "Export Decompile Bundle" + ell, [this] { onExportDecompileBundle(); });
	act("file.export_intel", "Export Threat Intel" + ell, [this] { onExportThreatIntel(); });
	act("file.batch", "Batch Decompile" + ell, [this] { onBatchDecompile(); });
	act("file.quit", "Quit", [this] { requestQuit(); }, "Ctrl+Q", Key::Q, C);

	// ── Analysis ────────────────────────────────────────────────────────────
	act("analysis.run_full", "Run Full Analysis", [this] { runFullAnalysis(); }, "F5", Key::F5, 0);
	{
		Action& a = act("analysis.redecompile", "Re-decompile Selected Function", [this] { redecompileSelectedFunction(); });
		a.enabled = false;
		a.toolTip = "Run retdec-decompiler with --select-functions for the function selected in the Functions list. "
		            "Other functions keep declarations only; use Run Full Analysis to restore all bodies.";
	}
	act("analysis.run_stage", "Run Stage" + ell, [this] { onRunStage(); });
	act("analysis.configure", "Configure" + ell, [this] { onConfigure(); });
	act("analysis.target", "Target", [this] { focusWorkspaceTab(kWsTarget); });
	toggle("analysis.fast", "Fast decompile (skip backend optimisations)",
	       "Adds --backend-no-opts --disable-static-code-detection — output is less polished but the run finishes "
	       "noticeably sooner. Toggle off and re-run for the full-quality pass.",
	       [this](bool on) { fastDecompile_ = on; });
	toggle("analysis.print_after_all", "Print LLVM IR after every pass (next run)",
	       "Adds --print-after-all to the next retdec-decompiler invocation (verbose).",
	       [this](bool on) { printAfterAll_ = on; });
	toggle("analysis.print_before_all", "Print LLVM IR before every pass (next run)",
	       "Adds --print-before-all to the next retdec-decompiler invocation.",
	       [this](bool on) { nextRun_.printBeforeAll = on; });
	toggle("analysis.keep_unreachable", "Keep unreachable functions (next run)",
	       "Adds -k / --keep-unreachable-funcs so discarded helpers stay in the C.",
	       [this](bool on) { nextRun_.keepUnreachableFuncs = on; });
	toggle("analysis.cleanup", "Delete intermediates after run (--cleanup)", "",
	       [this](bool on) { nextRun_.cleanup = on; });
	toggle("analysis.try_emulation", "Try emulation unpacking (next run)",
	       "Adds --try-emulation. Experimental; can be slow or fail on packed samples.",
	       [this](bool on) { nextRun_.tryEmulation = on; });
	toggle("analysis.keep_library", "Keep library functions (next run)",
	       "Adds --backend-keep-library-funcs so libc-like helpers stay in the C.",
	       [this](bool on) { nextRun_.keepLibraryFuncs = on; });
	act("analysis.max_memory", "Set max memory (bytes)" + ell, [this] {
		maxMemDlg_->show(nextRun_.maxMemoryBytes, [this](uint64_t bytes) { nextRun_.maxMemoryBytes = bytes; });
	});
	toggle("analysis.no_memory_limit", "No memory limit (next run)", "Adds --no-memory-limit (overrides --max-memory).",
	       [this](bool on) { nextRun_.noMemoryLimit = on; });
	act("analysis.c_style", "C output style" + ell, [this] {
		cStyleDlg_->show(nextRun_.cStyle, [this](const model::CStyleOptions& o) { nextRun_.cStyle = o; });
	}).toolTip = "Backend C flags for the next run (brackets, renaming, call-info).";
	toggle("analysis.raw_mode", "Treat input as raw image (-m raw)", "Adds -m raw for firmware dumps without a container header.",
	       [this](bool on) { nextRun_.rawMode = on; });
	act("analysis.raw_options", "Raw image / archive options" + ell, [this] {
		rawDlg_->show(nextRun_.raw, [this](const model::RawArchiveOptions& o) { nextRun_.raw = o; });
	}).toolTip = "Set --endian, --bit-size, --raw-section-vma, and --ar-name for the next run.";
	act("analysis.pdb", "Set PDB symbols" + ell, [this] {
		files_->show(FilePrompt::Mode::Open, "PDB symbols", fileDialogStartDir(), model::kPdbFileFilter,
		             [this](const std::string& path) {
			             if (path.empty()) return;
			             nextRun_.pdbPath = path;
			             rememberLastOpenDir(path);
			             showStatusMessage(model::pdbPathStatus(path), 5000);
		             });
	});
	act("analysis.sig_file", "Set static-code signature file" + ell, [this] {
		files_->show(FilePrompt::Mode::Open, "Static-code signatures", fileDialogStartDir(), model::kSignatureFileFilter,
		             [this](const std::string& path) {
			             if (path.empty()) return;
			             nextRun_.staticCodeSigFile = path;
			             rememberLastOpenDir(path);
			             showStatusMessage(model::staticSigFileStatus(path), 5000);
		             });
	});
	act("analysis.stop", "Stop Analysis", [this] { stopAnalysis(); }, "F6", Key::F6, 0).enabled = false;

	// ── View ────────────────────────────────────────────────────────────────
	toggle("view.dock_functions", "Functions", "", [this](bool on) { setDockOpen(Dock::Functions, on); }).checked = true;
	toggle("view.dock_workspace", "Workspace", "", [this](bool on) { setDockOpen(Dock::Workspace, on); }).checked = true;
	toggle("view.dock_output", "Output", "", [this](bool on) { setDockOpen(Dock::Output, on); }).checked = true;
	static constexpr std::array<std::pair<const char*, const char*>, kDocCount> kShow = {{
		{"view.show_c", "Show Decompiled C"},
		{"view.show_assembly", "Show Assembly"},
		{"view.show_ir", "Show IR"},
		{"view.show_cfg", "Show CFG"},
		{"view.show_synced", "Show Synced view"},
	}};
	static constexpr std::array<Key, kDocCount> kNumKeys = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5};
	for (int i = 0; i < kDocCount; ++i)
	{
		act(kShow[std::size_t(i)].first, kShow[std::size_t(i)].second, [this, i] {
			if (opts_.layoutStore) opts_.layoutStore->set("preferredDocumentTabIndex", double(i));
			raiseDocumentTab(i);
		}, std::format("Ctrl+{}", i + 1), kNumKeys[std::size_t(i)], C);
	}
	act("view.goto_address", "Go to address" + ell, [this] { onGotoAddress(); }, "Ctrl+G", Key::G, C);
	act("view.goto_line", "Go to line" + ell, [this] { onGotoLine(); }, "Ctrl+L", Key::L, C);
	// FuseUI has no key code for '`' (Key::Other), so the Qt Ctrl+` / Ctrl+Shift+`
	// sequences are shown but cannot be bound.
	act("view.show_console", "Show Console", [this] { focusOutputTab(kOutConsole); }, "Ctrl+`");
	act("view.show_problems", "Show Problems", [this] { focusOutputTab(kOutProblems); }, "Ctrl+Shift+`");
	act("view.show_history", "Show History", [this] { focusOutputTab(kOutHistory); });
	act("view.show_progress", "Show Progress", [this] { focusOutputTab(kOutProgress); });
	toggle("view.show_banner", "Show triage banner", "", [this](bool on) {
		bannerWanted_ = on;
		if (on && project_)
			triage_->setBinary(project_->binaryPath());
		else if (!on && !triage_->isHidden())
			triage_->dismiss();
	}).checked = true;
	act("view.reset_layout", "Reset Layout", [this] { resetLayout(); });

	// ── Tools ───────────────────────────────────────────────────────────────
	// Ctrl+, cannot be bound either (no comma key in FuseUI's Key set).
	act("tools.settings", "Settings" + ell, [this] { onSettings(); }, "Ctrl+,");
	act("tools.signature_studio", "Signature Studio" + ell, [this] { onShowSignatureStudio(); });
	act("tools.call_graph", "Call Graph" + ell, [this] { onShowCallGraph(); });
	act("tools.type_hierarchy", "Type Hierarchy" + ell, [this] { onShowTypeHierarchy(); });
	act("tools.ai_assistant", "AI Assistant" + ell, [this] { onShowAiAssistant(); });
	act("tools.compare", "Compare" + ell, [this] { onCompare(); });
	act("tools.compare_refined", "Compare original vs refined" + ell, [this] { onCompareRefined(); });
	act("tools.copy_function_c", "Copy selected function C", [this] { onCopyFunctionC(); }).toolTip =
		"Copy the decompiled C for the function selected in the Functions list.";

	// ── Help ────────────────────────────────────────────────────────────────
	act("help.documentation", "Documentation" + ell, [this] { onOpenDocumentation(); });
	act("help.shortcuts", "Keyboard Shortcuts" + ell, [this] { onKeyboardShortcuts(); });
	act("help.about", "About RetDec" + ell, [this] { onAbout(); });
}

// ─── Menus ───────────────────────────────────────────────────────────────────

void MainWindow::createMenus()
{
	using K = MenuEntry::Kind;
	auto A = [](std::string id) {
		MenuEntry e;
		e.kind = K::Action;
		e.actionId = std::move(id);
		return e;
	};
	auto S = [] {
		MenuEntry e;
		e.kind = K::Separator;
		return e;
	};
	using Items = std::vector<std::tuple<std::string, bool, std::function<void()>>>;
	auto Sub = [](std::string title, std::function<Items()> items) {
		MenuEntry e;
		e.kind = K::Submenu;
		e.title = std::move(title);
		e.items = std::move(items);
		return e;
	};

	Menu file{"File", {}, {}};
	file.entries = {A("file.open_binary"), A("file.open_project"), S(),
	                Sub("Recent Files",
	                    [this] {
		                    Items out;
		                    for (const std::string& p: recentFiles_)
			                    out.emplace_back(p, true, [this, p] { openPath(p); });
		                    if (recentFiles_.empty())
		                    {
			                    out.emplace_back("(empty)", false, nullptr);
			                    return out;
		                    }
		                    out.emplace_back("-", false, nullptr);
		                    out.emplace_back("Clear recent files", true, [this] { clearRecentFiles(); });
		                    return out;
	                    }),
	                S(), A("file.save_project"), A("file.save_project_as"), S(), A("file.save_decompiled"),
	                A("file.export_cmake"), A("file.export_bundle"), A("file.export_intel"),
	                Sub("Export As",
	                    [] {
		                    Items out;
		                    for (const model::Plugin* p: model::PluginManager::instance().outputPlugins())
			                    out.emplace_back(p->formatName(), true, nullptr); // run bound below
		                    if (out.empty()) out.emplace_back("(no output plugins loaded)", false, nullptr);
		                    return out;
	                    }),
	                A("file.batch"), S(), A("file.quit")};
	// The Export As runs need `this`; bind them here.
	file.entries[12].items = [this] {
		Items out;
		for (const model::Plugin* p: model::PluginManager::instance().outputPlugins())
		{
			const std::string id = p->metadata().id;
			out.emplace_back(p->formatName(), true, [this, id] { onExportAs(id); });
		}
		if (out.empty()) out.emplace_back("(no output plugins loaded)", false, nullptr);
		return out;
	};

	Menu analysis{"Analysis", {}, {}};
	analysis.entries = {A("analysis.run_full"), A("analysis.redecompile"), A("analysis.run_stage"), S(),
	                    A("analysis.configure"), A("analysis.target"), S(), A("analysis.fast"),
	                    A("analysis.print_after_all"), A("analysis.print_before_all"), A("analysis.keep_unreachable"),
	                    A("analysis.cleanup"), A("analysis.try_emulation"), A("analysis.keep_library"),
	                    A("analysis.max_memory"), A("analysis.no_memory_limit"), A("analysis.c_style"),
	                    A("analysis.raw_mode"), A("analysis.raw_options"), A("analysis.pdb"), A("analysis.sig_file"),
	                    Sub("Variable renamer",
	                        [this] {
		                        static constexpr std::array<const char*, 5> kStyles = {"readable", "simple", "address",
		                                                                               "hungarian", "unified"};
		                        Items out;
		                        const std::string cur = nextRun_.varRenamer.empty() ? "readable" : nextRun_.varRenamer;
		                        for (const char* s: kStyles)
		                        {
			                        const std::string style = s;
			                        out.emplace_back((style == cur ? "\xE2\x80\xA2 " : "   ") + style, true,
			                                         [this, style] { nextRun_.varRenamer = style == "readable" ? "" : style; });
		                        }
		                        return out;
	                        }),
	                    A("analysis.stop")};

	Menu view{"View", {}, {}};
	view.entries = {A("view.dock_functions"), A("view.dock_workspace"), A("view.dock_output"), S(),
	                A("view.show_c"), A("view.show_assembly"), A("view.show_ir"), A("view.show_cfg"),
	                A("view.show_synced"), A("view.goto_address"), S(), A("view.show_console"),
	                A("view.show_problems"), A("view.show_history"), A("view.show_progress"), S(),
	                A("view.show_banner"), S(), A("view.reset_layout")};

	Menu tools{"Tools", {}, {}};
	tools.entries = {A("tools.settings"), S(), A("tools.signature_studio"), A("tools.call_graph"),
	                 A("tools.type_hierarchy"), A("tools.ai_assistant"), S(), A("tools.compare"),
	                 A("tools.compare_refined"), A("tools.copy_function_c"), S(),
	                 Sub("Visualisation plugins", [this] {
		                 Items out;
		                 for (const model::Plugin* p: model::PluginManager::instance().visualisationPlugins())
		                 {
			                 const std::string id = p->metadata().id;
			                 out.emplace_back(p->panelTitle(), true, [this, id] { onShowVisualisationPlugin(id); });
		                 }
		                 if (out.empty()) out.emplace_back("(no visualisation plugins loaded)", false, nullptr);
		                 return out;
	                 })};

	Menu help{"Help", {}, {}};
	help.entries = {A("help.documentation"), S(), A("help.shortcuts"), A("help.about")};

	menus_ = {std::move(file), std::move(analysis), std::move(view), std::move(tools), std::move(help)};
	for (std::size_t i = 0; i < menus_.size(); ++i)
	{
		Menu& m = menus_[i];
		m.button = widgets::toolButton(ui_, menuBar_, m.title, std::format("menu_{}", m.title));
		ui_.addClass(m.button, "retdec-menu-title");
		ui_.setA11y(m.button, A11yRole::Button, m.title);
		const int index = int(i);
		ui_.connect(m.button, SignalKind::Clicked, [this, index](UiContext&, NodeHandle, const SignalArgs&) {
			openMenu(menus_[std::size_t(index)].title);
		});
		ui_.connect(m.button, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
			const auto line = std::size_t(a.value);
			if (line >= openMenu_.runs.size() || !openMenu_.runs[line]) return;
			const std::function<void()> run = openMenu_.runs[line];
			openMenu_.runs.clear();
			run();
		});
	}
}

MainWindow::Menu* MainWindow::findMenu(std::string_view title)
{
	for (auto& m: menus_)
		if (m.title == title) return &m;
	return nullptr;
}

const MainWindow::Menu* MainWindow::findMenu(std::string_view title) const
{
	for (const auto& m: menus_)
		if (m.title == title) return &m;
	return nullptr;
}

std::string MainWindow::menuText(const Menu& m, std::vector<std::function<void()>>& runs, std::vector<int>& topLines) const
{
	std::vector<std::string> lines;
	runs.clear();
	topLines.clear();
	for (const MenuEntry& e: m.entries)
	{
		topLines.push_back(int(lines.size()));
		switch (e.kind)
		{
		case MenuEntry::Kind::Separator:
			lines.emplace_back("-");
			runs.emplace_back();
			break;
		case MenuEntry::Kind::Action: {
			const Action* a = action(e.actionId);
			std::string label = a ? a->label : e.actionId;
			if (a && !a->shortcut.empty()) label += "      " + a->shortcut;
			lines.push_back(std::move(label));
			const std::string id = e.actionId;
			auto* self = const_cast<MainWindow*>(this);
			runs.emplace_back([self, id] { self->triggerAction(id); });
			break;
		}
		case MenuEntry::Kind::Submenu: {
			lines.push_back(e.title + " \xE2\x96\xB8"); // ▸
			runs.emplace_back();
			for (auto& [label, enabled, run]: e.items())
			{
				lines.push_back("\t" + label);
				runs.push_back(enabled ? run : nullptr);
			}
			break;
		}
		}
	}
	return joinLines(lines);
}

NodeHandle MainWindow::openMenu(std::string_view title)
{
	const Menu* m = findMenu(title);
	if (!m) return {};
	std::vector<int> top;
	const std::string text = menuText(*m, openMenu_.runs, top);
	const Rect r = ui_.rect(m->button);
	const NodeHandle scope = widgets::popupMenu(ui_, m->button, text, {r.x, r.y + r.h});
	if (!ui_.alive(scope)) return {};
	// Disabled / checked state on the top-level items (menu children follow the
	// top-level lines in order: one menuitem or separator per entry).
	const NodeHandle menu = ui_.firstChild(scope);
	NodeHandle child = ui_.firstChild(menu);
	for (const MenuEntry& e: m->entries)
	{
		if (!ui_.alive(child)) break;
		if (e.kind == MenuEntry::Kind::Action)
		{
			if (const Action* a = action(e.actionId))
			{
				ui_.setDisabled(child, !a->enabled);
				ui_.setChecked(child, a->checkable && a->checked);
				if (!a->toolTip.empty()) ui_.setA11y(child, A11yRole::ListItem, a->label);
			}
		}
		child = ui_.nextSibling(child);
	}
	return scope;
}

std::vector<std::string> MainWindow::menuTitles() const
{
	std::vector<std::string> out;
	for (const auto& m: menus_)
		out.push_back(m.title);
	return out;
}

std::vector<std::string> MainWindow::menuLabels(std::string_view title) const
{
	std::vector<std::string> out;
	const Menu* m = findMenu(title);
	if (!m) return out;
	for (const MenuEntry& e: m->entries)
	{
		if (e.kind == MenuEntry::Kind::Separator)
			out.emplace_back("-");
		else if (e.kind == MenuEntry::Kind::Submenu)
			out.push_back(e.title + " \xE2\x96\xB8");
		else if (const Action* a = action(e.actionId))
			out.push_back(a->label);
	}
	return out;
}

std::vector<std::string> MainWindow::submenuLabels(std::string_view title, std::string_view submenu) const
{
	std::vector<std::string> out;
	const Menu* m = findMenu(title);
	if (!m) return out;
	for (const MenuEntry& e: m->entries)
		if (e.kind == MenuEntry::Kind::Submenu && e.title == submenu)
			for (auto& [label, enabled, run]: e.items())
				out.push_back(label);
	return out;
}

bool MainWindow::triggerSubmenu(std::string_view title, std::string_view submenu, int index)
{
	const Menu* m = findMenu(title);
	if (!m) return false;
	for (const MenuEntry& e: m->entries)
	{
		if (e.kind != MenuEntry::Kind::Submenu || e.title != submenu) continue;
		auto items = e.items();
		if (index < 0 || index >= int(items.size())) return false;
		auto& [label, enabled, run] = items[std::size_t(index)];
		if (!enabled || !run) return false;
		run();
		return true;
	}
	return false;
}

// ─── Toolbar / status bar ────────────────────────────────────────────────────

void MainWindow::createToolBar()
{
	auto add = [this](const std::string& id) {
		const Action* a = action(id);
		const NodeHandle b = widgets::toolButton(ui_, toolBar_, a ? a->label : id, "tb_" + id);
		toolButtons_.emplace_back(id, b);
		const std::size_t index = toolButtons_.size() - 1;
		ui_.connect(b, SignalKind::Clicked, [this, index](UiContext&, NodeHandle, const SignalArgs&) {
			triggerAction(toolButtons_[index].first);
		});
		std::string tip = a ? a->label : id;
		if (a && !a->shortcut.empty()) tip += " (" + a->shortcut + ")";
		(void)widgets::setTooltip(ui_, b, tip);
	};
	add("file.open_binary");
	const NodeHandle sep = widgets::panel(ui_, toolBar_);
	ui_.addClass(sep, "retdec-toolbar-sep");
	add("analysis.run_full");
	add("analysis.stop");
	refreshToolBar();
}

void MainWindow::refreshToolBar()
{
	for (const auto& [id, node]: toolButtons_)
		if (const Action* a = action(id)) ui_.setDisabled(node, !a->enabled);
}

NodeHandle MainWindow::toolButton(std::string_view actionId) const
{
	for (const auto& [id, node]: toolButtons_)
		if (id == actionId) return node;
	return {};
}

void MainWindow::createStatusBar()
{
	statusBar_ = widgets::panel(ui_, root_, "statusBar");
	ui_.addClass(statusBar_, "retdec-statusbar");
	ui_.setCss(statusBar_, "flex-direction", "row");
	ui_.setCss(statusBar_, "flex-shrink", "0");
	ui_.setCss(statusBar_, "align-items", "center");
	statusFile_ = widgets::label(ui_, statusBar_, "No binary loaded", "statusFile");
	statusStage_ = widgets::label(ui_, statusBar_, "Idle", "statusStage");
	ui_.addClass(statusStage_, "retdec-status-stage");
	statusMsg_ = widgets::label(ui_, statusBar_, "", "statusMessage");
	ui_.addClass(statusMsg_, "retdec-status-message");
	ui_.setCss(statusMsg_, "flex-grow", "1");
	ui_.setCss(statusMsg_, "min-width", "0px");
	statusFnCount_ = widgets::label(ui_, statusBar_, "\xE2\x80\x94 functions", "statusFunctions");
	statusDecompile_ = widgets::label(ui_, statusBar_, "", "statusDecompile");
	setShown(ui_, statusDecompile_, false);
	statusNeural_ = widgets::label(ui_, statusBar_, "Neural: off", "statusNeural");
	statusProgress_ = widgets::progressBar(ui_, statusBar_, 0.f, "statusProgress");
	ui_.setCss(statusProgress_, "width", "150px");
	setShown(ui_, statusProgress_, false);
	statusElapsed_ = widgets::label(ui_, statusBar_, "", "statusElapsed");
}

std::string MainWindow::statusFileText() const { return std::string(ui_.text(statusFile_)); }
std::string MainWindow::statusStageText() const { return std::string(ui_.text(statusStage_)); }
std::string MainWindow::statusFunctionCountText() const { return std::string(ui_.text(statusFnCount_)); }
std::string MainWindow::statusDecompileText() const { return std::string(ui_.text(statusDecompile_)); }
std::string MainWindow::statusElapsedText() const { return std::string(ui_.text(statusElapsed_)); }
std::string MainWindow::statusNeuralText() const { return std::string(ui_.text(statusNeural_)); }
std::string MainWindow::statusMessageText() const { return std::string(ui_.text(statusMsg_)); }
float MainWindow::progressBarValue() const { return ui_.value(statusProgress_); }

void MainWindow::setStatusFile(const std::string& path)
{
	ui_.setText(statusFile_, path.empty() ? "No binary loaded" : path);
}

void MainWindow::setStatusStage(const std::string& stage)
{
	ui_.setText(statusStage_, stage);
}

void MainWindow::setStatusFunctionCount(int count)
{
	ui_.setText(statusFnCount_, count < 0 ? std::string("\xE2\x80\x94 functions") : std::format("{} functions", count));
}

void MainWindow::setStatusDecompileState(const std::string& state)
{
	ui_.setText(statusDecompile_, state);
	decompileStateShown_ = !state.empty();
	setShown(ui_, statusDecompile_, decompileStateShown_);
}

void MainWindow::setAnalysisProgress(int percent)
{
	widgets::setProgressIndeterminate(ui_, statusProgress_, false);
	ui_.setValue(statusProgress_, float(std::clamp(percent, 0, 100)) / 100.f);
}

void MainWindow::showStatusMessage(const std::string& message, int timeoutMs)
{
	ui_.setText(statusMsg_, message);
	if (statusTimerOn_) ui_.cancelTimer(statusTimer_);
	statusTimerOn_ = false;
	if (timeoutMs > 0 && !message.empty())
	{
		statusTimerOn_ = true;
		statusTimer_ = ui_.startTimer(float(timeoutMs) / 1000.f, false, [this](UiContext&) {
			statusTimerOn_ = false;
			ui_.setText(statusMsg_, "");
		});
	}
}

void MainWindow::setTitle(const std::string& t)
{
	title_ = t;
	if (onTitleChanged) onTitleChanged(t);
}

// ─── Panel wiring ────────────────────────────────────────────────────────────

void MainWindow::wirePanels()
{
	auto status = [this](const std::string& msg, int ms) { showStatusMessage(msg, ms); };
	decompiledC_->onStatusMessage = status;
	ir_->onStatusMessage = status;
	triPane_->onStatusMessage = status;
	commandLog_->onStatusMessage = status;
	target_->onStatusMessage = status;
	sigStudio_->onStatusMessage = status;
	gotoDlg_->onStatusMessage = status;
	gotoLineDlg_->onStatusMessage = status;
	cStyleDlg_->onStatusMessage = status;
	rawDlg_->onStatusMessage = status;
	maxMemDlg_->onStatusMessage = [this](const std::string& msg) { showStatusMessage(msg, 5000); };
	settingsDlg_->onApplied = [this] { applySettings(); };

	// Function list.
	functionList_->onFunctionActivated = [this](uint64_t addr, const std::string& name) {
		if (addr != shownFunction_) showFunction(addr, name);
	};
	functionList_->onSelectionChanged = [this] {
		updateRedecompileEnabled();
		if (auto e = functionList_->selectedEntry(); e && e->address != shownFunction_)
			showFunction(e->address, e->name.empty() ? e->rawName : e->name);
	};
	functionList_->onAddressActivated = [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); };
	functionList_->onRedecompileRequested = [this](uint64_t, const std::string&) { redecompileSelectedFunction(); };
	functionList_->onFunctionRenamed = [this](uint64_t addr, const std::string&, const std::string& newName) {
		if (!project_) return;
		auto ann = project_->annotation(addr).value_or(model::ProjectFile::Annotation{});
		ann.name = newName;
		project_->setAnnotation(addr, ann);
	};
	functionList_->onAnnotationChanged = [this](const model::FunctionEntry& e) {
		if (!project_) return;
		auto ann = project_->annotation(e.address).value_or(model::ProjectFile::Annotation{});
		ann.comment = e.notes;
		ann.tags = e.tags;
		ann.signatureOverride = e.signature;
		project_->setAnnotation(e.address, ann);
	};

	// Cross-panel navigation.
	binary_->onAddressActivated = [this](uint64_t addr) { assembly_->navigateTo(addr); };
	binary_->onDecompileRangeRequested = [this](const std::string& range) {
		if (!project_ || range.empty()) return;
		if (decompilerRunning())
		{
			showStatusMessage("Decompiler already running", 2500);
			return;
		}
		launchDecompilerForBinary(std::filesystem::absolute(project_->binaryPath()).string(), project_->arch(), {}, {range});
	};
	triPane_->onAddressNavigated = [this](uint64_t addr) { assembly_->navigateTo(addr); };
	triPane_->onFunctionRequested = [this](uint64_t addr) { navigateToAddress(addr, kDocSynced); };
	cfg_->onBlockActivated = [this](uint64_t addr) { navigateToAddress(addr, kDocAssembly); };
	cfg_->onAddressNavigated = [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); };
	callGraph_->onFunctionActivated = [this](uint64_t addr, const std::string&) {
		navigateToAddress(addr, kDocDecompiledC);
	};
	typeHierarchy_->onClassSelected = [this](const std::string& cls) { functionList_->filterByClass(cls); };
	typeHierarchy_->onVtableSlotActivated = [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); };
	strings_->onAddressActivated = [this](uint64_t addr) { navigateToAddress(addr, kDocAssembly); };
	assembly_->onAddressActivated = [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); };
	ir_->onAddressActivated = [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); };
	decompiledC_->onAddressActivated = [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); };
	ai_->onAddressNavigated = [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); };
	diagnostics_->onAddressActivated = [this](uint64_t addr) {
		navigateToAddress(addr, kDocDecompiledC);
		const auto entry = functionList_->selectedEntry();
		if (entry && entry->address == addr)
			decompiledC_->scrollToFunction(addr, entry->name.empty() ? entry->rawName : entry->name);
		else
			decompiledC_->scrollToFunction(addr, "");
	};
	diagnostics_->onFunctionActivated = [this](const std::string& fn) {
		if (!loadedArtifacts_) return;
		for (const auto& f: loadedArtifacts_->functions)
			if (f.name == fn || f.rawName == fn)
			{
				navigateToAddress(f.address, kDocDecompiledC);
				return;
			}
	};
	diagnostics_->onErrorAdded = [this] {
		if (!quitWhenDecompileFinishes_) focusOutputTab(kOutProblems);
	};

	// Triage banner.
	triage_->onDecompileRequested = [this] { runFullAnalysis(); };
	triage_->onUnpackRequested = [this] {
		focusWorkspaceTab(kWsInspect);
		showStatusMessage("Use the Inspect panel's 'Run unpacker' button. The unpacker output path is preconfigured.", 6000);
	};
	triage_->onMoreActionsRequested = [this](Vec2 anchor) { onTriageMoreActions(anchor); };
	triage_->onDismissed = [this] {
		bannerWanted_ = false;
		setActionChecked("view.show_banner", false);
	};
	triage_->onTargetDetailsRequested = [this] { focusWorkspaceTab(kWsTarget); };

	// Inspect: fileinfo / unpacker runs.
	inspect_->setUnpackerExecutable(model::resolveRetdecUnpackerExecutable().string());
	inspect_->onProcessStarted = [this](const std::string& label, const std::vector<std::string>& args,
	                                    const std::string& cwd) { console_->appendBanner(label, args, cwd); };
	inspect_->onProcessOutput = [this](const std::string&, const std::string& out, const std::string& err) {
		if (!out.empty()) console_->appendChunk(LiveConsolePanel::Stream::Stdout, out);
		if (!err.empty()) console_->appendChunk(LiveConsolePanel::Stream::Stderr, err);
	};
	inspect_->onCliToolFinished = [this](const std::string& tool, const std::vector<std::string>& args,
	                                     const std::string& cwd, int exitCode, int64_t ms, const std::string& tail) {
		commandLog_->appendRun(tool, args, cwd, exitCode, ms, tail);
		console_->appendFooter(tool, exitCode, ms);
		if (tool.find("fileinfo") != std::string::npos) refreshTriageFromInspect();
	};
	inspect_->onFileinfoReady = [this](const model::FileinfoInfo&, const std::string& json, const std::string&) {
		binary_->populateFromFileinfo(json);
		refreshTriageFromInspect();
	};
	inspect_->onRequestOpenBinary = [this](const std::string& p) {
		std::error_code ec;
		if (!std::filesystem::exists(p, ec))
		{
			showStatusMessage("File not found: " + p, 5000);
			return;
		}
		openBinary(std::filesystem::absolute(p).string());
	};
	inspect_->onRequestDecompileMode = [this] {
		raiseDocumentTab(kDocDecompiledC);
		setDockOpen(Dock::Functions, true);
		showStatusMessage("Decompile view", 3000);
	};

	// Signature Studio.
	sigStudio_->onCliToolFinished = [this](const model::SigToolResult& r) {
		commandLog_->appendRun(r.tool, r.arguments, r.workingDirectory, r.exitCode, r.elapsedMs, r.outputTail);
	};
	sigStudio_->onUseAsStaticSigFile = [this](const std::string& path) {
		nextRun_.staticCodeSigFile = path;
		showStatusMessage("Next decompile will use --static-code-sigfile " + path, 6000);
	};

	// Progress panel → status.
	progress_->onStageStarted = [this](const std::string& stage) { onAnalysisStageChanged(stage); };

	// Docks closed with their ✕ keep the View menu in sync.
	ui_.connect(dockFunctions_, SignalKind::Toggled,
	            [this](UiContext&, NodeHandle, const SignalArgs& a) { setActionChecked("view.dock_functions", a.flag); });
	ui_.connect(dockWorkspace_, SignalKind::Toggled,
	            [this](UiContext&, NodeHandle, const SignalArgs& a) { setActionChecked("view.dock_workspace", a.flag); });
	ui_.connect(dockOutput_, SignalKind::Toggled,
	            [this](UiContext&, NodeHandle, const SignalArgs& a) { setActionChecked("view.dock_output", a.flag); });
}

// ─── View helpers ────────────────────────────────────────────────────────────

void MainWindow::raiseDocumentTab(int index)
{
	if (index < 0 || index >= kDocCount) index = kDocDecompiledC;
	const NodeHandle bar = widgets::tabWidgetBar(ui_, docTabs_);
	if (int(ui_.value(bar)) != index) ui_.setValue(bar, float(index), true);
}

int MainWindow::currentDocumentTab() const
{
	return int(ui_.value(widgets::tabWidgetBar(ui_, docTabs_)));
}

void MainWindow::setOutputTabsText()
{
	// Hidden tabs are dropped from the bar; only the last one (Progress) is
	// ever hidden, so the remaining indices are unchanged.
	std::string titles;
	int count = 0;
	for (int i = 0; i < kOutCount; ++i)
	{
		if (!outputTabShown_[i]) continue;
		if (!titles.empty()) titles += '\n';
		titles += kOutputTitles[std::size_t(i)];
		++count;
	}
	const NodeHandle bar = widgets::tabWidgetBar(ui_, outTabs_);
	ui_.setText(bar, titles);
	ui_.setRange(bar, 0.f, float(std::max(1, count) - 1), 1.f);
}

void MainWindow::setOutputTabVisible(int index, bool visible)
{
	if (index < 0 || index >= kOutCount) return;
	if (index != kOutProgress) visible = true; // only Progress hides
	if (outputTabShown_[index] == visible) return;
	outputTabShown_[index] = visible;
	setOutputTabsText();
	if (!visible && currentOutputTab() == index) focusOutputTab(kOutConsole);
}

bool MainWindow::outputTabVisible(int index) const
{
	return index >= 0 && index < kOutCount && outputTabShown_[index];
}

void MainWindow::focusOutputTab(int index)
{
	if (index < 0 || index >= kOutCount) index = kOutConsole;
	if (!outputTabShown_[index]) setOutputTabVisible(index, true);
	const NodeHandle bar = widgets::tabWidgetBar(ui_, outTabs_);
	if (int(ui_.value(bar)) != index) ui_.setValue(bar, float(index), true);
	setDockOpen(Dock::Output, true);
}

int MainWindow::currentOutputTab() const
{
	return int(ui_.value(widgets::tabWidgetBar(ui_, outTabs_)));
}

void MainWindow::focusWorkspaceTab(int index)
{
	if (index < 0 || index >= kWsCount) index = kWsStrings;
	const NodeHandle bar = widgets::tabWidgetBar(ui_, wsTabs_);
	if (int(ui_.value(bar)) != index) ui_.setValue(bar, float(index), true);
	setDockOpen(Dock::Workspace, true);
}

int MainWindow::currentWorkspaceTab() const
{
	return int(ui_.value(widgets::tabWidgetBar(ui_, wsTabs_)));
}

NodeHandle MainWindow::dockNode(Dock d) const
{
	switch (d)
	{
	case Dock::Functions: return dockFunctions_;
	case Dock::Workspace: return dockWorkspace_;
	case Dock::Output: return dockOutput_;
	}
	return {};
}

bool MainWindow::dockOpen(Dock d) const
{
	return !ui_.hasClass(dockNode(d), "dock-closed");
}

void MainWindow::setDockOpen(Dock d, bool open)
{
	const NodeHandle n = dockNode(d);
	if (open == dockOpen(d))
	{
		setActionChecked(d == Dock::Functions ? "view.dock_functions"
		                 : d == Dock::Workspace ? "view.dock_workspace"
		                                        : "view.dock_output",
		                 open);
		return;
	}
	if (open)
		widgets::dockTo(ui_, n,
		                d == Dock::Functions   ? widgets::DockSide::Left
		                : d == Dock::Workspace ? widgets::DockSide::Right
		                                       : widgets::DockSide::Bottom);
	else
		widgets::dockClose(ui_, n);
}

bool MainWindow::triageBannerVisible() const
{
	return !triage_->isHidden();
}

std::vector<std::string> MainWindow::documentTabTitles() const
{
	return splitLines(ui_.text(widgets::tabWidgetBar(ui_, docTabs_)));
}

std::vector<std::string> MainWindow::workspaceTabTitles() const
{
	return splitLines(ui_.text(widgets::tabWidgetBar(ui_, wsTabs_)));
}

std::vector<std::string> MainWindow::outputTabTitles() const
{
	std::vector<std::string> out;
	for (std::string_view t: kOutputTitles)
		out.emplace_back(t);
	return out;
}

void MainWindow::updateCentralEmptyState()
{
	const bool hasBinary = project_ && !project_->binaryPath().empty();
	centralIndex_ = hasBinary ? kCentralDocs : kCentralEmpty;
	setShown(ui_, startPage_, centralIndex_ == kCentralEmpty);
	setShown(ui_, docTabs_, centralIndex_ == kCentralDocs);
}

void MainWindow::updateProjectFileActions()
{
	const bool ok = static_cast<bool>(project_);
	setActionEnabled("file.save_project", ok);
	setActionEnabled("file.save_project_as", ok);
}

void MainWindow::updateRedecompileEnabled()
{
	setActionEnabled("analysis.redecompile", !analysisRunning_ && functionList_->selectedEntry().has_value());
}

void MainWindow::setFastDecompile(bool on)
{
	fastDecompile_ = on;
	setActionChecked("analysis.fast", on);
}

void MainWindow::navigateToAddress(uint64_t address, int documentTab)
{
	setDockOpen(Dock::Functions, true);
	bool matched = false;
	if (loadedArtifacts_)
	{
		for (const auto& f: functionList_->functions())
			if (f.address == address)
			{
				matched = true;
				break;
			}
	}
	if (matched)
	{
		functionList_->selectFunction(address);
		if (auto e = functionList_->selectedEntry(); e && e->address == address && shownFunction_ != address)
			showFunction(address, e->name.empty() ? e->rawName : e->name);
	}
	raiseDocumentTab(documentTab);
	if (!matched) assembly_->navigateTo(address);
}

// ─── Layout persistence ──────────────────────────────────────────────────────

void MainWindow::loadLayoutStore()
{
	fuse::app::Settings* st = opts_.layoutStore;
	if (!st) return;
	recentFiles_.clear();
	for (std::string& p: splitLines(st->get("recentFiles")))
		if (!p.empty()) recentFiles_.push_back(std::move(p));
	// Drop recent files that no longer exist (Qt updateRecentFilesMenu).
	std::erase_if(recentFiles_, [](const std::string& p) {
		std::error_code ec;
		return !std::filesystem::exists(p, ec);
	});
	if (recentFiles_.size() > kMaxRecentFiles) recentFiles_.resize(kMaxRecentFiles);
	const std::string lps(st->get("lastProjectSavePath"));
	std::error_code ec;
	lastProjectSavePath_ = (!lps.empty() && std::filesystem::exists(lps, ec)) ? lps : std::string();
	printAfterAll_ = st->flag("decompilePrintAfterAll", false);
	setActionChecked("analysis.print_after_all", printAfterAll_);
	setFastDecompile(st->flag("fastDecompile", false));
	const double width = st->number("window.width", 0.0), height = st->number("window.height", 0.0);
	if (width >= 320.0 && height >= 240.0) windowSize_ = {float(width), float(height)};
}

void MainWindow::restoreLayout()
{
	fuse::app::Settings* st = opts_.layoutStore;
	if (st)
	{
		const std::string_view layout = st->get("windowState");
		if (!layout.empty() && !widgets::dockRestoreLayout(ui_, dockArea_, layout))
			(void)widgets::dockRestoreLayout(ui_, dockArea_, defaultDockLayout());
	}
	for (Dock d: {Dock::Functions, Dock::Workspace, Dock::Output})
		setActionChecked(d == Dock::Functions ? "view.dock_functions"
		                 : d == Dock::Workspace ? "view.dock_workspace"
		                                        : "view.dock_output",
		                 dockOpen(d));
	raiseDocumentTab(st ? int(st->number("preferredDocumentTabIndex", kDocDecompiledC)) : kDocDecompiledC);
}

void MainWindow::saveLayout()
{
	snapshotLayout();
	if (opts_.layoutStore) (void)opts_.layoutStore->save();
}

void MainWindow::snapshotLayout()
{
	fuse::app::Settings* st = opts_.layoutStore;
	if (!st) return;
	st->set("window.width", double(windowSize_.w));
	st->set("window.height", double(windowSize_.h));
	st->set("windowState", widgets::dockSaveLayout(ui_, dockArea_));
	st->set("recentFiles", joinLines(recentFiles_));
	st->set("lastProjectSavePath", lastProjectSavePath_);
	st->set("decompilePrintAfterAll", printAfterAll_ ? 1.0 : 0.0);
	st->set("fastDecompile", fastDecompile_ ? 1.0 : 0.0);
	st->set("preferredDocumentTabIndex", double(currentDocumentTab()));
}

void MainWindow::resetLayout()
{
	if (fuse::app::Settings* st = opts_.layoutStore)
	{
		st->remove("window.width");
		st->remove("window.height");
		st->remove("windowState");
		st->remove("preferredDocumentTabIndex");
	}
	(void)widgets::dockRestoreLayout(ui_, dockArea_, defaultDockLayout());
	for (Dock d: {Dock::Functions, Dock::Workspace, Dock::Output})
		setDockOpen(d, true);
	windowSize_ = {float(kDefaultWidth), float(kDefaultHeight)};
	if (onResizeRequested) onResizeRequested(kDefaultWidth, kDefaultHeight);
	raiseDocumentTab(kDocDecompiledC);
	focusWorkspaceTab(kWsStrings);
	focusOutputTab(kOutConsole);
	showStatusMessage("Layout reset to defaults", 5000);
}

void MainWindow::setWindowSize(unsigned width, unsigned height)
{
	if (width > 0 && height > 0) windowSize_ = {float(width), float(height)};
}

void MainWindow::addRecentFile(const std::string& path)
{
	std::erase(recentFiles_, path);
	recentFiles_.insert(recentFiles_.begin(), path);
	if (recentFiles_.size() > kMaxRecentFiles) recentFiles_.resize(kMaxRecentFiles);
}

void MainWindow::clearRecentFiles()
{
	recentFiles_.clear();
}

} // namespace retdec::gui::ui
