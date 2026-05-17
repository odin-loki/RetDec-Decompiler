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

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QToolBar;
class QDockWidget;
class QStatusBar;
class QButtonGroup;
class QTabWidget;
QT_END_NAMESPACE

namespace retdec {
namespace gui {

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
class TriageBanner;
class TriPaneCodeView;
} // namespace panels

/**
 * @brief RetDecMainWindow — application shell (v3 simplified layout).
 *
 *   - Central widget:  vertical stack of
 *                        TriageBanner (dismissible, shown when binary loaded)
 *                        QTabWidget of document views
 *                            Decompiled C | Assembly | IR (SSA) | CFG |
 *                            Synced (Asm ┃ IR ┃ C) | Compare
 *   - Left dock:       Functions (with prominent filter, always visible)
 *   - Right dock:      Strings | Inspect  (just two tabs)
 *   - Bottom dock:     Console (live) | Problems (diagnostics) |
 *                      Command log | Progress
 *   - Floating:        AI Assistant (hidden by default)
 *   - Tools menu:      Signature Studio…, Call Graph…, Type Hierarchy…
 *                      open as separate top-level windows on demand.
 *
 * The "mode toolbar" from v2 is intentionally removed — modes were redundant
 * with the document tabs and added cognitive load. The Synced tri-pane is
 * restored as a centre tab so power users still get cross-highlighted
 * Asm/IR/C without it competing for dock space.
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
    void setAnalysisProgress(int percent);

    // ── Testing hooks ────────────────────────────────────────────────────────

    /// Central document tab widget.
    QTabWidget* documentTabsForTest() const { return documentTabs_; }
    QTabWidget* outputTabsForTest()   const { return outputTabs_; }
    QTabWidget* workspaceTabsForTest() const { return workspaceTabWidget_; }
    panels::LiveConsolePanel* liveConsoleForTest() const { return liveConsole_; }
    panels::TriageBanner* triageBannerForTest() const { return triageBanner_; }
    QDockWidget* outputDockForTest()   const { return dockOutput_; }
    QDockWidget* symbolsDockForTest()  const { return dockFunctions_; }
    QDockWidget* workspaceDockForTest() const { return dockWorkspace_; }

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
    void onRecentFileTriggered();

    // Analysis menu.
    void onRunFullAnalysis();
    void onRunStage();
    void onConfigure();

    // Tools menu.
    void onSettings();
    void onMLAssistant();
    void onCompare();

    // Help menu.
    void onOpenDocumentation();
    void onAbout();

    void onDecompilerProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onDecompilerProcessError(QProcess::ProcessError err);

    /// Apply Settings → ML to AI Assistant.
    void syncAiAssistantFromAppSettings();

    void onAnalysisStageChanged(const QString& stage);
    void onAnalysisFinished();
    void updateElapsedTime();

    void onCliToolLogged(const QString& tool, const QStringList& args, const QString& cwd,
                         int exitCode, qint64 elapsedMs, const QString& outputTail);
    void onTargetApplyToProject();
    void onShowSignatureStudio();
    void onShowCallGraph();
    void onShowTypeHierarchy();

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
    void updateProjectFileActions();
    void restoreLayout();
    void saveLayout();
    void updateRecentFilesMenu();
    void addRecentFile(const QString& path);
    void finishExternalDecompileUi();
    /// Hook a QProcess so its stdout/stderr stream live into the console panel.
    void streamProcessToConsole(QProcess* proc, const QString& label);
    /// Refresh the triage banner from the InspectPanel's parsed fileinfo.
    void refreshTriageFromInspect();
    /// Load the decompiled .c + functions config.json sidecar produced by a
    /// prior decompile run. Used both by the post-decompile success path and
    /// by the openBinary cache-reuse path. Returns true if a usable .c was
    /// loaded.
    bool loadDecompileArtifacts(const QString& decompiledCPath);
    /// If the binary has a fresh cached decompile next to it, load it and
    /// skip spawning retdec-decompiler. Returns true if cache was used.
    bool tryLoadCachedDecompile(const QString& binaryPath);

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
    QAction* stopAction_     = nullptr;
    QAction* printAfterAllAct_ = nullptr;
    QToolBar* mainToolBar_   = nullptr;

    // UI — status bar widgets.
    QLabel*       statusFile_    = nullptr;
    QLabel*       statusStage_   = nullptr;
    QLabel*       statusFnCount_ = nullptr;
    QLabel*       statusElapsed_ = nullptr;
    QProgressBar* analysisBar_   = nullptr;
    QTimer*       elapsedTimer_  = nullptr;
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
    panels::TriageBanner*        triageBanner_    = nullptr;
    panels::TriPaneCodeView*     triPane_         = nullptr;

    // Centre + dock wrappers.
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
    QString                        decompilerOutputPath_;
    QStringList                    lastDecompilerArgs_;
    QString                        lastDecompilerExe_;
    QString                        lastDecompilerCwd_;
    QString                        lastProjectSavePath_;
    bool                           decompilePrintAfterAll_ = false;

    QString lastAiMlFingerprint_;

    /// User toggle: skip expensive backend optimizations for faster runs.
    bool fastDecompile_ = false;
    QAction* fastDecompileAct_ = nullptr;

    static constexpr int kMaxRecentFiles = 10;
};

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_MAINWINDOW_H
