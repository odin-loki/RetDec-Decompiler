/**
 * @file include/retdec/gui/ui/main_window.h
 * @brief MainWindow — the retdec-gui application shell on FuseUI (port of
 *        the Qt RetDecMainWindow, v3 layout).
 *
 *   ┌ menu bar: File  Analysis  View  Tools  Help ──────────────────────────┐
 *   ├ toolbar: [Open Binary…] | [Run Full Analysis] [Stop Analysis] ─────────┤
 *   │┌ Functions ┐┌ centre ─────────────────────────────┐┌ Workspace ──────┐│
 *   ││           ││ TriageBanner (after a binary opens) ││ Strings│Inspect ││
 *   ││ function  ││ start screen  ── or ── document tabs││ Binary │Target  ││
 *   ││ list      ││ Decompiled C|Assembly|IR (SSA)|CFG| ││                 ││
 *   ││           ││ Synced (Asm ┃ IR ┃ C)               ││                 ││
 *   │└───────────┘└─────────────────────────────────────┘└─────────────────┘│
 *   │┌ Output: Console | Problems | History | Progress* ───────────────────┐│
 *   ├ status: file │ stage │ message … │ N functions │ state │ Neural │ ▭ │ t┤
 *   └────────────────────────────────────────────────────────────────────────┘
 *
 * Docks are a FuseUI dockArea (left / right / bottom regions, resizable,
 * closable); the View menu toggles them and Reset Layout restores the
 * defaults. Signature Studio, Call Graph, Type Hierarchy, the AI Assistant,
 * Compare and visualisation plugins open in tool windows (FuseUI dialogs over
 * the main window). Progress is a hidden output tab until an analysis runs.
 *
 * The window works on any UiContext: the executable passes its AppWindow's
 * context, tests a headless one. Everything the user can do (menu items,
 * shortcuts, buttons) is an *action* with a stable id ("file.open_binary",
 * "analysis.run_full", ...), so tests trigger exactly what a click does.
 *
 * Decompiling: buildDecompilerArguments -> model::DecompileRun (stdout and
 * stderr to a log file) polled on a FuseUI timer that tails the log into the
 * console, maps it to progress / stages and Problems; on exit 0 the sidecars
 * are loaded (loadDecompileArtifactsFromPaths) and every panel is populated.
 *
 * Persistence: window geometry, dock layout, recent files, the last project
 * path, the Fast / print-after-all toggles and the preferred document tab go
 * to a fuse::app::Settings store (the Qt QSettings "retdec-gui" keys); the
 * application settings stay in model::AppSettings.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_MAIN_WINDOW_H
#define RETDEC_GUI_UI_MAIN_WINDOW_H

#include "retdec/gui/model/artifact_loader.h"
#include "retdec/gui/model/decompile_run.h"
#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/model/project_file.h"
#include "retdec/gui/model/settings.h"
#include "retdec/gui/ui/theme.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::app {
class AppWindow;
class Settings;
} // namespace fuse::app

namespace retdec::gui::ui {

class AIAssistantPanel;
class AboutDialog;
class AssemblyPanel;
class BatchDecompileDialog;
class BinaryBrowserPanel;
class CStyleDialog;
class CallGraphPanel;
class CfgPanel;
class CommandLogPanel;
class ContextMenu;
class DecompiledCPanel;
class DiagnosticsPanel;
class DiffPanel;
class DocsViewer;
class EmptyState;
class FilePrompt;
class FunctionListPanel;
class GotoAddressDialog;
class GotoLineDialog;
class IRPanel;
class InspectPanel;
class LiveConsolePanel;
class MaxMemoryDialog;
class MessageDialog;
class ModalDialog;
class ProgressPanel;
class RawOptionsDialog;
class RunStageDialog;
class SettingsDialog;
class ShortcutsDialog;
class SignatureStudioPanel;
class StringsBrowserPanel;
class TargetPanel;
class TriPaneCodeView;
class TriageBanner;
class TypeHierarchyPanel;
struct PanelEventHandler;

/// Binds @p ui's clipboard hooks to the system clipboard
/// (forge::os::clipboard: Win32, pbcopy / pbpaste, wl-copy, xclip / xsel).
/// When the system clipboard is unavailable (no display or helper) a
/// process-wide buffer stands in, so copy / paste inside the app still works.
void bindSystemClipboard(fuse::ui::UiContext& ui);

struct MainWindowOptions
{
	/// No OS window (RETDEC_GUI_HEADLESS / --headless): no session restore,
	/// fileinfo is not run automatically on open (Qt's headless behaviour).
	bool headless = false;
	/// Window geometry / dock layout / recent files store (not owned; may be
	/// null = nothing persisted). The executable passes Application::settings().
	fuse::app::Settings* layoutStore = nullptr;
	/// Application settings (default: the singleton).
	model::AppSettings* settings = nullptr;
	/// Apply the theme from settings (off in tests that render the FuseUI default).
	bool applyTheme = true;
};

class MainWindow {
public:
	// ── Indices (the Qt constants) ───────────────────────────────────────────
	enum DocTab
	{
		kDocDecompiledC = 0,
		kDocAssembly = 1,
		kDocIR = 2,
		kDocCFG = 3,
		kDocSynced = 4,
		kDocCount
	};
	enum WorkspaceTab
	{
		kWsStrings = 0,
		kWsInspect = 1,
		kWsBinary = 2,
		kWsTarget = 3,
		kWsCount
	};
	enum OutputTab
	{
		kOutConsole = 0,
		kOutProblems = 1,
		kOutHistory = 2,
		kOutProgress = 3,
		kOutCount
	};
	enum CentralPage
	{
		kCentralEmpty = 0,
		kCentralDocs = 1
	};
	enum class Dock
	{
		Functions,
		Workspace,
		Output
	};
	static constexpr int kMaxRecentFiles = 10;
	static constexpr unsigned kDefaultWidth = 1280, kDefaultHeight = 800;

	/// One menu / toolbar / shortcut action.
	struct Action
	{
		std::string id;       ///< stable id, e.g. "file.open_binary"
		std::string label;    ///< menu text, e.g. "Open Binary…"
		std::string shortcut; ///< shown text, e.g. "Ctrl+O" (empty = none)
		fuse::ui::Key key = fuse::ui::Key::None;
		fuse::ui::u8 mods = 0;
		bool checkable = false;
		bool checked = false;
		bool enabled = true;
		std::string toolTip;
		std::function<void()> run; ///< checkable: runs after the checked state flipped
	};

	MainWindow(fuse::ui::UiContext& ui, MainWindowOptions options = {});
	/// Stops a running decompile and saves the layout.
	~MainWindow();
	MainWindow(const MainWindow&) = delete;
	MainWindow& operator=(const MainWindow&) = delete;

	fuse::ui::UiContext& ui() const
	{
		return ui_;
	}
	fuse::ui::NodeHandle root() const
	{
		return root_;
	}

	// ── Project management ───────────────────────────────────────────────────
	void openBinary(const std::string& path);
	/// False (and a warning box) when the project cannot be loaded.
	bool openProject(const std::string& path);
	/// Saves annotations / filter / depth into the project and writes it.
	bool saveProject(const std::string& path);
	/// .retdec -> openProject, anything else -> openBinary (drag and drop, command line).
	void openPath(const std::string& path);
	/// Drag and drop: opens the first existing regular file; others are reported
	/// in the status bar. True when one was opened.
	bool dropFiles(const std::vector<std::string>& paths);
	model::ProjectFile* project() const
	{
		return project_.get();
	}
	const std::string& lastProjectSavePath() const
	{
		return lastProjectSavePath_;
	}

	// ── Analysis lifecycle ───────────────────────────────────────────────────
	/// Qt startAnalysis(): progress UI only (no process).
	void startAnalysis();
	/// Stops the decompiler (whole process tree) and any batch.
	void stopAnalysis();
	/// F5. Opens a file prompt first when nothing is loaded.
	void runFullAnalysis();
	void redecompileSelectedFunction();
	bool analysisRunning() const
	{
		return analysisRunning_;
	}
	bool decompilerRunning() const;
	bool batchRunning() const
	{
		return batchRunning_;
	}
	/// Batch-decompile @p paths one after the other (File → Batch Decompile…'s Start).
	void startBatchDecompile(const std::vector<std::string>& paths);
	/// Loads `<binary>.gui-decompiled.c` + sidecars when present and fresh enough.
	bool tryLoadCachedDecompile(const std::string& binaryPath);
	/// Loads the artefacts of @p cPath and populates every panel.
	bool
	loadDecompileArtifacts(const std::filesystem::path& cPath, std::optional<uint64_t> reselectAddress = std::nullopt);
	const std::optional<model::DecompileArtifacts>& loadedArtifacts() const
	{
		return loadedArtifacts_;
	}
	/// Shows @p address / @p name in Assembly, IR, CFG, Synced and C (function selection).
	void showFunction(uint64_t address, const std::string& name);
	/// Selects @p address in the function list when known, raises @p documentTab.
	void navigateToAddress(uint64_t address, int documentTab);
	/// Per-run options of the Analysis menu (C style, raw, PDB, ...).
	model::NextRunOptions& nextRunOptions()
	{
		return nextRun_;
	}
	bool fastDecompile() const
	{
		return fastDecompile_;
	}
	void setFastDecompile(bool on);

	// ── Status bar ───────────────────────────────────────────────────────────
	void setStatusFile(const std::string& path);
	void setStatusStage(const std::string& stage);
	/// < 0 shows "— functions".
	void setStatusFunctionCount(int count);
	/// Empty hides the field.
	void setStatusDecompileState(const std::string& state);
	/// Determinate progress (clamped 0..100).
	void setAnalysisProgress(int percent);
	/// Temporary message (Qt statusBar()->showMessage); @p timeoutMs <= 0 keeps it.
	void showStatusMessage(const std::string& message, int timeoutMs = 0);

	// ── CI / headless ────────────────────────────────────────────────────────
	void setFastDecompilePreset(bool on)
	{
		setFastDecompile(on);
	}
	/// Quit (onQuitRequested with the decompiler's exit status) once the run
	/// ends; the run then matches a plain CLI invocation (no saved overrides).
	void enableQuitWhenDecompileFinishes(bool on)
	{
		quitWhenDecompileFinishes_ = on;
	}
	bool quitWhenDecompileFinishes() const
	{
		return quitWhenDecompileFinishes_;
	}
	void runDecompileForBenchmark()
	{
		runFullAnalysis();
	}
	int64_t lastDecompileSubprocessMs() const
	{
		return lastDecompileSubprocessMs_;
	}
	std::optional<int> lastDecompileExitCode() const
	{
		return lastExitCode_;
	}
	/// One poll of the running decompiler (what the timer does).
	void pollDecompile();

	// ── Settings / layout ────────────────────────────────────────────────────
	/// Theme, editor font, word wrap / line numbers, output language, Target
	/// panel, plugin menus — runs on every AppSettings change.
	void applySettings();
	ThemeKind theme() const
	{
		return theme_;
	}
	/// Writes the layout keys into the store (no file write; the store saves
	/// when dirty). Cheap enough to call every loop iteration.
	void snapshotLayout();
	/// snapshotLayout() and save the store.
	void saveLayout();
	void restoreLayout();
	/// View → Reset Layout.
	void resetLayout();
	/// Current window size (main.cpp reports resizes; persisted by saveLayout).
	void setWindowSize(unsigned width, unsigned height);
	fuse::ui::Size windowSize() const
	{
		return windowSize_;
	}
	/// File → Quit / the window's close request: confirms a running analysis
	/// and unsaved project changes, saves the layout, then onQuitRequested(0).
	void requestQuit();
	/// The OS window's close button (AppWindow::setCloseRequestHandler): the
	/// same questions as File → Quit (running analysis, unsaved project). True
	/// only when the window may close at once; otherwise the window stays and
	/// the loop ends through onQuitRequested once the questions are answered.
	bool handleWindowCloseRequest();
	/// Installs @p window's OS hooks: the close button (handleWindowCloseRequest)
	/// and files dropped from the OS (dropFiles). The window must outlive these
	/// hooks or be detached with detachWindow() first.
	void attachWindow(fuse::app::AppWindow& window);
	/// Removes the hooks attachWindow() installed.
	void detachWindow(fuse::app::AppWindow& window);
	/// Stop everything and save the layout (the app is exiting regardless).
	void shutdown();
	/// The UiContext is about to disappear under the window (the OS closed it):
	/// kill child processes without touching the UI. The caller then releases
	/// (leaks) the MainWindow instead of destroying it.
	void abandonUi();

	// ── Actions / menus ──────────────────────────────────────────────────────
	/// Runs action @p id like a click (no-op and false when disabled / unknown).
	bool triggerAction(std::string_view id);
	const Action* action(std::string_view id) const;
	std::vector<std::string> actionIds() const;
	bool actionEnabled(std::string_view id) const;
	bool actionChecked(std::string_view id) const;
	/// Menu titles in order ("File", "Analysis", "View", "Tools", "Help").
	std::vector<std::string> menuTitles() const;
	/// Item labels of menu @p title as the popup shows them ("-" separators,
	/// "Submenu ▸" for submenus).
	std::vector<std::string> menuLabels(std::string_view title) const;
	/// Item labels of the submenu @p submenu of menu @p title.
	std::vector<std::string> submenuLabels(std::string_view title, std::string_view submenu) const;
	/// Runs entry @p index of submenu @p submenu (recent file, export plugin, renamer, ...).
	bool triggerSubmenu(std::string_view title, std::string_view submenu, int index);
	/// Opens menu @p title's popup (as clicking its title does). Returns the popup scope.
	fuse::ui::NodeHandle openMenu(std::string_view title);
	/// The global key handler (menu shortcuts). True when an action ran.
	bool handleShortcut(fuse::ui::Key key, fuse::ui::u8 mods);
	const std::vector<std::string>& recentFiles() const
	{
		return recentFiles_;
	}
	void addRecentFile(const std::string& path);
	void clearRecentFiles();

	// ── View ─────────────────────────────────────────────────────────────────
	void raiseDocumentTab(int index);
	int currentDocumentTab() const;
	void focusOutputTab(int index);
	int currentOutputTab() const;
	void focusWorkspaceTab(int index);
	int currentWorkspaceTab() const;
	bool outputTabVisible(int index) const;
	void setOutputTabVisible(int index, bool visible);
	bool dockOpen(Dock d) const;
	void setDockOpen(Dock d, bool open);
	/// The View menu's dock toggle.
	void toggleDock(Dock d)
	{
		setDockOpen(d, !dockOpen(d));
	}
	int centralStackIndex() const
	{
		return centralIndex_;
	}
	bool triageBannerVisible() const;
	std::vector<std::string> documentTabTitles() const;
	std::vector<std::string> workspaceTabTitles() const;
	std::vector<std::string> outputTabTitles() const;
	std::string windowTitle() const
	{
		return title_;
	}

	// ── Events out ───────────────────────────────────────────────────────────
	/// File → Quit, the end of a --headless-decompile run (exit status), ...
	std::function<void(int exitCode)> onQuitRequested;
	/// The window title changed ("RetDec — /path/bin").
	std::function<void(const std::string& title)> onTitleChanged;
	/// Reset Layout asks for the default window size.
	std::function<void(unsigned width, unsigned height)> onResizeRequested;

	// ── Test hooks ───────────────────────────────────────────────────────────
	fuse::ui::NodeHandle menuBarNode() const
	{
		return menuBar_;
	}
	fuse::ui::NodeHandle toolBarNode() const
	{
		return toolBar_;
	}
	fuse::ui::NodeHandle toolButton(std::string_view actionId) const;
	fuse::ui::NodeHandle dockAreaNode() const
	{
		return dockArea_;
	}
	fuse::ui::NodeHandle dockNode(Dock d) const;
	fuse::ui::NodeHandle documentTabsNode() const
	{
		return docTabs_;
	}
	fuse::ui::NodeHandle workspaceTabsNode() const
	{
		return wsTabs_;
	}
	fuse::ui::NodeHandle outputTabsNode() const
	{
		return outTabs_;
	}
	fuse::ui::NodeHandle startPageNode() const
	{
		return startPage_;
	}
	fuse::ui::NodeHandle statusBarNode() const
	{
		return statusBar_;
	}
	fuse::ui::NodeHandle startOpenBinaryButton() const
	{
		return startOpenBinary_;
	}
	fuse::ui::NodeHandle startOpenProjectButton() const
	{
		return startOpenProject_;
	}
	std::string statusFileText() const;
	std::string statusStageText() const;
	std::string statusFunctionCountText() const;
	std::string statusDecompileText() const;
	bool statusDecompileVisible() const
	{
		return decompileStateShown_;
	}
	std::string statusElapsedText() const;
	std::string statusNeuralText() const;
	std::string statusMessageText() const;
	bool progressBarVisible() const
	{
		return progressShown_;
	}
	float progressBarValue() const;

	FunctionListPanel& functionList()
	{
		return *functionList_;
	}
	DecompiledCPanel& decompiledC()
	{
		return *decompiledC_;
	}
	AssemblyPanel& assembly()
	{
		return *assembly_;
	}
	IRPanel& irPanel()
	{
		return *ir_;
	}
	CfgPanel& cfgPanel()
	{
		return *cfg_;
	}
	TriPaneCodeView& triPane()
	{
		return *triPane_;
	}
	TriageBanner& triageBanner()
	{
		return *triage_;
	}
	StringsBrowserPanel& strings()
	{
		return *strings_;
	}
	InspectPanel& inspect()
	{
		return *inspect_;
	}
	BinaryBrowserPanel& binaryBrowser()
	{
		return *binary_;
	}
	TargetPanel& target()
	{
		return *target_;
	}
	LiveConsolePanel& liveConsole()
	{
		return *console_;
	}
	DiagnosticsPanel& diagnostics()
	{
		return *diagnostics_;
	}
	CommandLogPanel& commandLog()
	{
		return *commandLog_;
	}
	ProgressPanel& progressPanel()
	{
		return *progress_;
	}
	CallGraphPanel& callGraph()
	{
		return *callGraph_;
	}
	TypeHierarchyPanel& typeHierarchy()
	{
		return *typeHierarchy_;
	}
	SignatureStudioPanel& signatureStudio()
	{
		return *sigStudio_;
	}
	AIAssistantPanel& aiAssistant()
	{
		return *ai_;
	}
	DiffPanel& comparePanel()
	{
		return *compare_;
	}
	EmptyState& startEmptyState()
	{
		return *startEmpty_;
	}

	MessageDialog& messageBox()
	{
		return *message_;
	}
	FilePrompt& filePrompt()
	{
		return *files_;
	}
	SettingsDialog& settingsDialog()
	{
		return *settingsDlg_;
	}
	BatchDecompileDialog& batchDialog()
	{
		return *batchDlg_;
	}
	RunStageDialog& runStageDialog()
	{
		return *runStageDlg_;
	}
	CStyleDialog& cStyleDialog()
	{
		return *cStyleDlg_;
	}
	RawOptionsDialog& rawOptionsDialog()
	{
		return *rawDlg_;
	}
	GotoAddressDialog& gotoAddressDialog()
	{
		return *gotoDlg_;
	}
	GotoLineDialog& gotoLineDialog()
	{
		return *gotoLineDlg_;
	}
	MaxMemoryDialog& maxMemoryDialog()
	{
		return *maxMemDlg_;
	}
	AboutDialog& aboutDialog()
	{
		return *aboutDlg_;
	}
	ShortcutsDialog& shortcutsDialog()
	{
		return *shortcutsDlg_;
	}
	DocsViewer& docsViewer()
	{
		return *docsDlg_;
	}
	ContextMenu& triageMenu()
	{
		return *triageMenu_;
	}
	/// The three-button question box (quit confirmation / unsaved changes).
	bool questionOpen() const;
	std::string questionText() const;
	/// Picks button @p index of the open question (0 = first, as shown).
	void answerQuestion(int index);
	/// Tool windows ("Signature Studio", "Call Graph", "Type Hierarchy",
	/// "AI Assistant", "Compare", plugin titles): open?
	bool toolWindowOpen(std::string_view title) const;
	void closeToolWindows();
	/// Text a visualisation plugin rendered into its tool window.
	std::string pluginPanelText(std::string_view title) const;

private:
	struct ToolWindow;
	struct MenuEntry
	{
		enum class Kind
		{
			Action,
			Separator,
			Submenu
		} kind = Kind::Action;
		std::string actionId;
		std::string title; ///< submenu title
		/// Submenu items (label, enabled, run), built when the menu opens.
		std::function<std::vector<std::tuple<std::string, bool, std::function<void()>>>()> items;
	};
	struct Menu
	{
		std::string title;
		std::vector<MenuEntry> entries;
		fuse::ui::NodeHandle button;
	};
	struct OpenMenu
	{
		std::vector<std::function<void()>> runs; ///< by popup line index (null = nothing)
	};

	// construction (main_window.cpp)
	void createPanels();
	void createLayout();
	void createActions();
	void createMenus();
	void createToolBar();
	void createStatusBar();
	void wirePanels();
	Action& addAction(Action a);
	Action* findAction(std::string_view id);
	void setActionEnabled(std::string_view id, bool on);
	void setActionChecked(std::string_view id, bool on);
	void refreshToolBar();
	Menu* findMenu(std::string_view title);
	const Menu* findMenu(std::string_view title) const;
	std::string menuText(const Menu& m, std::vector<std::function<void()>>& runs, std::vector<int>& actionLines) const;
	bool handleRootEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);
	void setTitle(const std::string& t);
	void updateCentralEmptyState();
	void updateProjectFileActions();
	void updateRedecompileEnabled();
	void setOutputTabsText();
	ToolWindow& toolWindow(const std::string& title, float w, float h);
	void showToolWindow(const std::string& title);

	// actions (main_window_actions.cpp)
	void onOpenBinary();
	void onOpenProject();
	void onSaveProject();
	void onSaveProjectAs();
	void onSaveDecompiled();
	void onExportCMake();
	void onExportDecompileBundle();
	void onExportThreatIntel();
	void onExportAs(const std::string& pluginId);
	void onBatchDecompile();
	void onRunStage();
	void onConfigure();
	void onSettings();
	void onCompare();
	void onCompareRefined();
	void onCopyFunctionC();
	void onShowSignatureStudio();
	void onShowCallGraph();
	void onShowTypeHierarchy();
	void onShowAiAssistant();
	void onShowVisualisationPlugin(const std::string& pluginId);
	void onOpenDocumentation();
	void onKeyboardShortcuts();
	void onAbout();
	void onGotoAddress();
	void onGotoLine();
	void onTriageMoreActions(fuse::ui::Vec2 anchor);
	void refreshTriageFromInspect();
	void refreshTypeHierarchyFromArtifacts();
	void applyEditorFont();
	void warn(const std::string& title, const std::string& text);
	void info(const std::string& title, const std::string& text);
	void copyText(const std::string& text, const std::string& what);
	void askQuestion(
		const std::string& title,
		const std::string& text,
		std::vector<std::string> buttons,
		std::function<void(int)> onAnswer);
	void finishQuit();
	std::string fileDialogStartDir() const;
	void rememberLastOpenDir(const std::string& filePath);
	std::filesystem::path guessDecompiledCPath() const;
	void pluginsChanged();
	void dropPluginUi(const std::string& id);
	void loadLayoutStore();

	// decompiling (main_window_decompile.cpp)
	bool launchDecompilerForBinary(
		const std::string& absBinary,
		const std::string& arch,
		const std::vector<std::string>& selectedFunctions = {},
		const std::vector<std::string>& selectedRanges = {});
	void onDecompileFinished();
	void finishExternalDecompileUi();
	void processNextBatchItem();
	void finishBatchDecompile(bool cancelled);
	void updateBatchStatusMessage(const std::string& stageSuffix = {});
	void populateFromArtifacts(const std::filesystem::path& absC, std::optional<uint64_t> reselect);
	void onAnalysisStageChanged(const std::string& stage);
	void onAnalysisFinished();
	void updateElapsedTime();
	void startElapsedTimer();
	void stopElapsedTimer();
	void stopPollTimer();
	void headlessFail(int exitCode, const std::string& message);
	void writeHeadlessDecompileReport(
		int64_t decompileMs, int exitCode, const std::string& outputPath, const std::vector<std::string>& args = {});
	void resetDecompileUiForOpen();

	fuse::ui::UiContext& ui_;
	MainWindowOptions opts_;
	model::AppSettings& settings_;
	std::unique_ptr<PanelEventHandler> rootHandler_;
	fuse::ui::NodeHandle root_, menuBar_, toolBar_, dockArea_, central_, centralStack_, startPage_, docHost_;
	fuse::ui::NodeHandle startOpenBinary_, startOpenProject_;
	fuse::ui::NodeHandle docTabs_, wsTabs_, outTabs_;
	fuse::ui::NodeHandle dockFunctions_, dockWorkspace_, dockOutput_;
	fuse::ui::NodeHandle statusBar_, statusFile_, statusStage_, statusMsg_, statusFnCount_, statusDecompile_,
		statusNeural_, statusProgress_, statusElapsed_;
	std::vector<std::pair<std::string, fuse::ui::NodeHandle>> toolButtons_;

	// panels
	std::unique_ptr<FunctionListPanel> functionList_;
	std::unique_ptr<DecompiledCPanel> decompiledC_;
	std::unique_ptr<AssemblyPanel> assembly_;
	std::unique_ptr<IRPanel> ir_;
	std::unique_ptr<CfgPanel> cfg_;
	std::unique_ptr<TriPaneCodeView> triPane_;
	std::unique_ptr<TriageBanner> triage_;
	std::unique_ptr<EmptyState> startEmpty_;
	std::unique_ptr<StringsBrowserPanel> strings_;
	std::unique_ptr<InspectPanel> inspect_;
	std::unique_ptr<BinaryBrowserPanel> binary_;
	std::unique_ptr<TargetPanel> target_;
	std::unique_ptr<LiveConsolePanel> console_;
	std::unique_ptr<DiagnosticsPanel> diagnostics_;
	std::unique_ptr<CommandLogPanel> commandLog_;
	std::unique_ptr<ProgressPanel> progress_;
	std::vector<std::unique_ptr<ToolWindow>> toolWindows_;
	std::unique_ptr<CallGraphPanel> callGraph_;
	std::unique_ptr<TypeHierarchyPanel> typeHierarchy_;
	std::unique_ptr<SignatureStudioPanel> sigStudio_;
	std::unique_ptr<AIAssistantPanel> ai_;
	std::unique_ptr<DiffPanel> compare_;

	// dialogs
	std::unique_ptr<MessageDialog> message_;
	std::unique_ptr<FilePrompt> files_;
	std::unique_ptr<SettingsDialog> settingsDlg_;
	std::unique_ptr<BatchDecompileDialog> batchDlg_;
	std::unique_ptr<RunStageDialog> runStageDlg_;
	std::unique_ptr<CStyleDialog> cStyleDlg_;
	std::unique_ptr<RawOptionsDialog> rawDlg_;
	std::unique_ptr<GotoAddressDialog> gotoDlg_;
	std::unique_ptr<GotoLineDialog> gotoLineDlg_;
	std::unique_ptr<MaxMemoryDialog> maxMemDlg_;
	std::unique_ptr<AboutDialog> aboutDlg_;
	std::unique_ptr<ShortcutsDialog> shortcutsDlg_;
	std::unique_ptr<DocsViewer> docsDlg_;
	std::unique_ptr<ContextMenu> triageMenu_;
	struct Question;
	std::unique_ptr<Question> question_;

	// actions & menus
	std::vector<Action> actions_;
	std::vector<Menu> menus_;
	OpenMenu openMenu_;

	// state
	std::unique_ptr<model::ProjectFile> project_;
	std::optional<model::DecompileArtifacts> loadedArtifacts_;
	std::vector<std::string> recentFiles_;
	std::string lastProjectSavePath_;
	std::string title_;
	model::NextRunOptions nextRun_;
	bool fastDecompile_ = false;
	bool printAfterAll_ = false;
	bool quitWhenDecompileFinishes_ = false;
	bool analysisRunning_ = false;
	bool quitting_ = false;
	bool shutDown_ = false;
	int centralIndex_ = kCentralEmpty;
	bool outputTabShown_[kOutCount] = {true, true, true, true};
	bool decompileStateShown_ = false;
	bool progressShown_ = false;
	bool bannerWanted_ = true;
	ThemeKind theme_ = ThemeKind::Mocha;
	fuse::ui::Size windowSize_{float(kDefaultWidth), float(kDefaultHeight)};
	uint64_t shownFunction_ = ~uint64_t(0);
	std::string currentProgressStage_;
	int64_t currentStageStartMs_ = 0;

	// decompiler run
	std::unique_ptr<model::DecompileRun> run_;
	std::unique_ptr<model::TemporaryFile> llvmPassesFile_;
	std::filesystem::path decompilerOutputPath_;
	std::filesystem::path lastLogPath_;
	std::vector<std::string> lastDecompilerArgs_;
	std::string lastDecompilerExe_;
	std::string lastDecompilerCwd_;
	uint64_t logProgressOffset_ = 0, logConsoleOffset_ = 0, logDiagOffset_ = 0;
	std::optional<uint64_t> pendingReselectAddress_;
	int64_t lastDecompileSubprocessMs_ = 0;
	std::optional<int> lastExitCode_;
	std::chrono::steady_clock::time_point wallStart_{};
	fuse::ui::TimerHandle pollTimer_, elapsedTimer_, statusTimer_;
	bool pollTimerOn_ = false, elapsedTimerOn_ = false, statusTimerOn_ = false;

	// export threat intel (async)
	std::unique_ptr<model::DecompileRun> intelRun_;
	std::unique_ptr<model::TemporaryFile> intelFileinfo_;
	std::string intelOutPath_;
	fuse::ui::TimerHandle intelTimer_;
	bool intelTimerOn_ = false;

	// batch
	std::vector<std::string> batchQueue_;
	bool batchRunning_ = false;
	int batchTotal_ = 0;

	// listeners
	model::AppSettings::ListenerId settingsListener_ = 0;
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_MAIN_WINDOW_H
