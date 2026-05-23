#ifndef RETDEC_GUI_MAINWINDOW_H
#define RETDEC_GUI_MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>

#include "retdec/gui/artifact_loader.h"

#include <optional>

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QToolBar;
class QDockWidget;
class QStatusBar;
class QButtonGroup;
class QTabWidget;
class QStackedWidget;
QT_END_NAMESPACE

namespace retdec {
namespace gui {

namespace widgets {
class EmptyStateWidget;
}

class ProjectFile;
namespace panels {
class BinaryBrowserPanel;
class FunctionListPanel;
class AssemblyPanel;
class IRPanel;
class DecompiledCPanel;
class CFGPanel;
class TypeHierarchyPanel;
class CallGraphPanel;
class StringsBrowserPanel;
class AIAssistantPanel;
class DiagnosticsPanel;
class ProgressPanel;
class AnalysisBridge;
class InspectPanel;
class CommandLogPanel;
class LiveConsolePanel;
class TargetPanel;
class SignatureStudioPanel;
class DiffPanel;
class TriageBanner;
class TriPaneCodeView;
} // namespace panels

/**
 * @brief RetDecMainWindow — application shell (v3 simplified layout).
 *
 *   - Central widget:  vertical stack of
 *                        TriageBanner (dismissible, shown when binary loaded)
 *                        QTabWidget — five document tabs:
 *                            Decompiled C | Assembly | IR (SSA) | CFG |
 *                            Synced (Asm ┃ IR ┃ C)
 *   - Left dock:       Functions (with prominent filter, always visible)
 *   - Right dock:      Workspace — Strings | Inspect | Binary
 *   - Bottom dock:     Output — Console | Problems | History | Progress (during analysis)
 *   - Tools menu:      Signature Studio…, Call Graph…, Type Hierarchy…
 *                      open as separate top-level windows on demand.
 *
 * The v2 mode toolbar is removed — document tabs replace mode switching.
 * AI Assistant is not docked in v3; use retdec-qwen3-runner externally.
 */
class RetDecMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit RetDecMainWindow(QWidget* parent = nullptr);
    ~RetDecMainWindow() override;

    // Project management.
    void openBinary(const QString& path);
    void openProject(const QString& path);
    bool saveProject(const QString& path);

    // Analysis lifecycle.
    void startAnalysis();
    void stopAnalysis();

    // Status bar helpers (callable from any panel).
    void setStatusFile(const QString& path);
    void setStatusStage(const QString& stage);
    void setStatusFunctionCount(int count);
    void setStatusDecompileState(const QString& state);
    void setAnalysisProgress(int percent);

    // ── Testing hooks ────────────────────────────────────────────────────────

    /// Central stack: empty start screen (0) vs document tabs (1).
    QStackedWidget* centralStackForTest() const { return centralStack_; }

    /// Central document tab widget.
    QTabWidget* documentTabsForTest() const { return documentTabs_; }
    QTabWidget* outputTabsForTest()   const { return outputTabs_; }
    QTabWidget* workspaceTabsForTest() const { return workspaceTabWidget_; }
    panels::LiveConsolePanel* liveConsoleForTest() const { return liveConsole_; }
    panels::TriageBanner* triageBannerForTest() const { return triageBanner_; }
    QDockWidget* outputDockForTest()   const { return dockOutput_; }
    QDockWidget* symbolsDockForTest()  const { return dockFunctions_; }
    QDockWidget* workspaceDockForTest() const { return dockWorkspace_; }

    /// CI / parity benchmark: enable Fast preset before decompile.
    void setFastDecompilePreset(bool on);
    /// CI / parity benchmark: quit the app when the decompiler subprocess exits.
    void enableQuitWhenDecompileFinishes(bool on);
    /// Trigger onRunFullAnalysis (bypasses menu).
    void runDecompileForBenchmark();
    /// Wall time of the last finished decompiler subprocess (ms).
    qint64 lastDecompileSubprocessMsForTest() const { return lastDecompileSubprocessMs_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    // File menu.
    void onOpenBinary();
    void onOpenProject();
    void onSaveDecompiled();
    void onSaveProject();
    void onSaveProjectAs();
    void onExportCMake();
    void onExportDecompileBundle();
    void onExportThreatIntel();
    void onRecentFileTriggered();
    void onBatchDecompile();

    // Analysis menu.
    void onRunFullAnalysis();
    void onRedecompileSelectedFunction();
    void onRunStage();
    void onConfigure();

    // Tools menu.
    void onSettings();
    void onCompare();

    // Help menu.
    void onOpenDocumentation();
    void onKeyboardShortcuts();
    void onAbout();

    void onDecompilerProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onDecompilerProcessError(QProcess::ProcessError err);
    void onDecompileLogPollTick();

    /// Apply Settings → General editor font to code views.
    void applyEditorFontFromSettings();

    void onAnalysisStageChanged(const QString& stage);
    void onAnalysisFinished();
    void updateElapsedTime();

    void onCliToolLogged(const QString& tool, const QStringList& args, const QString& cwd,
                         int exitCode, qint64 elapsedMs, const QString& outputTail);
    void onTargetApplyToProject();
    void onShowSignatureStudio();
    void onShowCallGraph();
    void onShowTypeHierarchy();
    void onResetLayout();

    void onFunctionArtifactViews(uint64_t address, const QString& name);
    void refreshTypeHierarchyFromArtifacts();
    /// Select @p address in the function list (when known), show @p documentTab,
    /// and refresh per-function views.
    void navigateToAddress(uint64_t address, int documentTab);

private:
    // Init helpers.
    void createMenus();
    void createToolBar();
    void createStatusBar();
    void createPanels();
    void createDockLayout();
    void setupDockOptions();
    void installCodeTabShortcuts();
    void raiseDocumentTab(int index);
    /// Show the bottom output dock and select Console (0) or Problems (1).
    void focusOutputTab(int tabIndex);
    /// Show the workspace dock and select a tab (Strings/Inspect/Binary/Target).
    void focusWorkspaceTab(int tabIndex);
    void updateProjectFileActions();
    void restoreLayout();
    void saveLayout();
    void updateRecentFilesMenu();
    void addRecentFile(const QString& path);
    void updateCentralEmptyState();
    void tryRestoreLastSession();
    void finishExternalDecompileUi();
    /// Hook a QProcess so its stdout/stderr stream live into the console panel.
    void streamProcessToConsole(QProcess* proc, const QString& label);
    /// Refresh the triage banner from the InspectPanel's parsed fileinfo.
    void refreshTriageFromInspect();
    /// Load the decompiled .c + functions config.json sidecar produced by a
    /// prior decompile run. Used both by the post-decompile success path and
    /// by the openBinary cache-reuse path. Returns true if a usable .c was
    /// loaded. When @p reselectAddress is set, that function is re-selected
    /// instead of the first entry.
    bool loadDecompileArtifacts(const QString& decompiledCPath,
                                std::optional<uint64_t> reselectAddress = std::nullopt);
    /// If the binary has a fresh cached decompile next to it, load it and
    /// skip spawning retdec-decompiler. Returns true if cache was used.
    bool tryLoadCachedDecompile(const QString& binaryPath);
    /// Launch retdec-decompiler for @p absBinary (shared by full analysis and batch).
    bool launchDecompilerForBinary(const QString& absBinary,
                                   const QString& arch,
                                   const QStringList& selectedFunctions = {});
    void startBatchDecompile(const QStringList& paths);
    void processNextBatchItem();
    void finishBatchDecompile(bool cancelled = false);
    void updateBatchStatusMessage(const QString& stageSuffix = QString());

    // UI — menus.
    QMenu* fileMenu_     = nullptr;
    QMenu* recentMenu_   = nullptr;
    QMenu* analysisMenu_ = nullptr;
    QMenu* viewMenu_     = nullptr;
    QMenu* toolsMenu_    = nullptr;
    QMenu* helpMenu_     = nullptr;

    // UI — toolbar actions.
    QAction* openAction_       = nullptr;
    QAction* saveProjectAct_   = nullptr;
    QAction* saveProjectAsAct_ = nullptr;
    QAction* analyseAction_  = nullptr;
    QAction* redecompileFunctionAct_ = nullptr;
    QAction* stopAction_     = nullptr;
    QAction* printAfterAllAct_ = nullptr;
    QToolBar* mainToolBar_   = nullptr;

    // UI — status bar widgets.
    QLabel*       statusFile_    = nullptr;
    QLabel*       statusStage_   = nullptr;
    QLabel*       statusFnCount_ = nullptr;
    QLabel*       statusDecompile_ = nullptr;
    QLabel*       statusElapsed_ = nullptr;
    QProgressBar* analysisBar_   = nullptr;
    QTimer*       elapsedTimer_  = nullptr;
    QTimer*       decompileLogPollTimer_ = nullptr;
    qint64        decompileLogOffset_    = 0;
    qint64        decompileConsoleTailOffset_ = 0;
    QElapsedTimer wallClock_;

    // Panels.
    panels::BinaryBrowserPanel*  binaryBrowser_   = nullptr;
    panels::FunctionListPanel*   functionList_    = nullptr;
    panels::AssemblyPanel*       assembly_        = nullptr;
    panels::IRPanel*             irPanel_         = nullptr;
    panels::DecompiledCPanel*    decompiledC_     = nullptr;
    panels::CFGPanel*            cfgPanel_        = nullptr;
    panels::TypeHierarchyPanel*  typeHierarchy_   = nullptr;
    panels::CallGraphPanel*      callGraph_       = nullptr;
    panels::StringsBrowserPanel* stringsBrowser_  = nullptr;
    panels::AIAssistantPanel*    aiAssistant_     = nullptr;
    panels::DiagnosticsPanel*    diagnostics_     = nullptr;
    panels::ProgressPanel*       progressPanel_   = nullptr;
    panels::AnalysisBridge*      analysisBridge_  = nullptr;
    panels::InspectPanel*        inspect_         = nullptr;
    panels::CommandLogPanel*     commandLog_      = nullptr;
    panels::LiveConsolePanel*    liveConsole_     = nullptr;
    panels::TargetPanel*         target_          = nullptr;
    panels::SignatureStudioPanel* signatureStudio_ = nullptr;
    panels::DiffPanel*           comparePanel_    = nullptr;
    panels::TriageBanner*        triageBanner_    = nullptr;
    panels::TriPaneCodeView*     triPane_         = nullptr;

    // Centre + dock wrappers.
    QStackedWidget* centralStack_      = nullptr;
    widgets::EmptyStateWidget* startEmptyState_ = nullptr;
    QTabWidget*  documentTabs_       = nullptr; ///< Centre: Decompiled C | Asm | IR | CFG | Synced | Compare
    QDockWidget* dockFunctions_      = nullptr;
    QDockWidget* dockWorkspace_      = nullptr;
    QDockWidget* dockOutput_         = nullptr;
    QDockWidget* dockAI_             = nullptr;
    QTabWidget*  workspaceTabWidget_ = nullptr;
    QTabWidget*  outputTabs_         = nullptr;

    // Project state.
    std::unique_ptr<ProjectFile> project_;
    bool analysisRunning_ = false;
    QStringList recentFiles_;

    QProcess*                      decompilerProc_ = nullptr;
    std::unique_ptr<QTemporaryFile> llvmPassesJsonTemp_;
    /// Decompiler stdout/stderr redirected here (not a pipe) so console UI
    /// work cannot block the child process on a full OS pipe buffer.
    std::unique_ptr<QTemporaryFile> decompilerLogTemp_;
    QString                        decompilerOutputPath_;
    QStringList                    lastDecompilerArgs_;
    QString                        lastDecompilerExe_;
    QString                        lastDecompilerCwd_;
    QString                        lastProjectSavePath_;
    bool                           decompilePrintAfterAll_ = false;
    std::optional<uint64_t>        pendingReselectAddress_;

    QString lastAiMlFingerprint_;

    /// User toggle: skip expensive backend optimizations for faster runs.
    bool fastDecompile_ = false;
    QAction* fastDecompileAct_ = nullptr;
    bool   quitWhenDecompileFinishes_ = false;
    qint64 lastDecompileSubprocessMs_ = 0;

    std::optional<DecompileArtifacts> loadedArtifacts_;

    QStringList batchQueue_;
    bool        batchRunning_ = false;
    int         batchTotal_   = 0;

    static constexpr int kMaxRecentFiles = 10;
};

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_MAINWINDOW_H
