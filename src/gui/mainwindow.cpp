/**
 * @file src/gui/mainwindow.cpp
 * @brief RetDecMainWindow — v3 simplified shell.
 *
 * See `assets/retdec_gui_redesign_v3.png` for the visual target.
 */

#include <memory>

#include "retdec/gui/mainwindow.h"
#include "retdec/gui/batch_decompile_dialog.h"
#include "retdec/gui/batch_decompile_queue.h"
#include "retdec/gui/cli_tool_paths.h"
#include "retdec/gui/decompiler_launch.h"
#include "retdec/gui/artifact_loader.h"
#include "retdec/gui/export_bundle.h"
#include "retdec/gui/project_file.h"
#include "retdec/gui/settings/settings.h"
#include "retdec/gui/panels/settings_dialog.h"
#include "retdec/gui/panels/diagnostics_panel.h"
#include "retdec/gui/panels/diff_panel.h"
#include "retdec/gui/panels/binary_browser_panel.h"
#include "retdec/gui/panels/function_list_panel.h"
#include "retdec/gui/panels/assembly_panel.h"
#include "retdec/gui/panels/ir_panel.h"
#include "retdec/gui/panels/decompiled_c_panel.h"
#include "retdec/gui/panels/cfg_panel.h"
#include "retdec/gui/panels/type_hierarchy_panel.h"
#include "retdec/gui/panels/call_graph_panel.h"
#include "retdec/gui/panels/strings_browser_panel.h"
#include "retdec/gui/panels/ai_assistant_panel.h"
#include "retdec/gui/panels/progress_panel.h"
#include "retdec/gui/panels/inspect_panel.h"
#include "retdec/gui/panels/command_log_panel.h"
#include "retdec/gui/panels/live_console_panel.h"
#include "retdec/gui/panels/triage_banner.h"
#include "retdec/gui/panels/target_panel.h"
#include "retdec/gui/panels/signature_studio_panel.h"
#include "retdec/gui/panels/tri_pane_code_view.h"
#include "retdec/gui/widgets/empty_state_widget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QInputDialog>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QShortcut>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTextStream>
#include <QToolBar>
#include <QUrl>
#include <QWidget>

#include <cstdio>

namespace retdec {
namespace gui {

// ─── Local helpers ───────────────────────────────────────────────────────────

namespace {

bool parseEntryPointString(const QString& s, uint64_t* out) {
    const QString t = s.trimmed();
    if (t.isEmpty()) { *out = 0; return true; }
    bool ok = false;
    if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        *out = t.mid(2).toULongLong(&ok, 16);
        return ok;
    }
    *out = t.toULongLong(&ok, 10);
    return ok;
}

QString guessDecompiledCPath(const ProjectFile* pf) {
    if (!pf) return {};
    if (!pf->decompiledPath().isEmpty()) {
        const QString p = pf->decompiledPath();
        if (QFileInfo::exists(p))
            return QFileInfo(p).absoluteFilePath();
    }
    const QString outDir = AppSettings::instance().decompiler.decompileOutputDir;
    return locateGuiDecompiledCPath(pf->binaryPath(), outDir);
}

void writeHeadlessDecompileReport(qint64 decompileMs, int exitCode,
                                  const QString& outputPath,
                                  const QStringList& args = {})
{
    const QString path =
            QDir::temp().filePath(QStringLiteral("retdec-gui-decompile-timing.txt"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream ts(&f);
    ts << QStringLiteral("DECOMPILE_MS=") << decompileMs << QLatin1Char('\n');
    ts << QStringLiteral("EXIT=") << exitCode << QLatin1Char('\n');
    ts << QStringLiteral("OUTPUT=") << outputPath << QLatin1Char('\n');
    if (!args.isEmpty())
        ts << QStringLiteral("ARGS=") << args.join(QLatin1Char(' ')) << QLatin1Char('\n');
}

// Document tab indices (Centre).
constexpr int kDocDecompiledC = 0;
constexpr int kDocAssembly    = 1;
constexpr int kDocIR          = 2;
constexpr int kDocCFG         = 3;
constexpr int kDocSynced      = 4;

// Central stack: welcome screen vs document tabs.
constexpr int kCentralEmpty = 0;
constexpr int kCentralDocs  = 1;

// Workspace dock (right).
constexpr int kWsStrings = 0;
constexpr int kWsInspect = 1;
constexpr int kWsBinary  = 2;
constexpr int kWsTarget  = 3;

// Bottom output dock — Console + Problems + History + Progress.
constexpr int kOutConsole  = 0;
constexpr int kOutProblems = 1;
constexpr int kOutHistory  = 2;
constexpr int kOutProgress = 3;

QDockWidget* makeDock(const QString& title, QWidget* widget,
                      QDockWidget::DockWidgetFeatures features =
                          QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable |
                          QDockWidget::DockWidgetClosable) {
    auto* dock = new QDockWidget(title);
    dock->setWidget(widget);
    dock->setFeatures(features);
    dock->setObjectName("dock_" + title.toLower().replace(' ', '_'));
    return dock;
}

/// Pop a panel widget into a transient top-level window (used for Tools
/// menu items like Signature Studio, Call Graph, Type Hierarchy).
/// The host keeps a single Z-order-aware QPointer-equivalent via parent;
/// reopening raises the existing window instead of creating a new one.
QWidget* showAsToolWindow(QWidget* parent, QWidget* panel, const QString& title,
                          const QSize& defaultSize) {
    // If there's already a window hosting this panel, raise it.
    QWidget* existing = panel->window();
    if (existing && existing != parent && existing->isWindow()) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return existing;
    }
    // Wrap in a non-modal QDialog so it can be moved between monitors.
    auto* w = new QDialog(parent);
    w->setWindowFlag(Qt::Window);
    w->setWindowTitle(title);
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    panel->setParent(w);
    lay->addWidget(panel);
    w->resize(defaultSize);
    w->show();
    w->raise();
    w->activateWindow();
    return w;
}

bool isPanelToolWindowVisible(QWidget* panel, QWidget* mainWindow)
{
    if (!panel)
        return false;
    QWidget* w = panel->window();
    return w && w != mainWindow && w->isVisible();
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

RetDecMainWindow::RetDecMainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("RetDec — Retargetable Decompiler"));
    setMinimumSize(1280, 800);
    setAcceptDrops(true);
    setupDockOptions();
    createPanels();
    createDockLayout();
    createMenus();
    createToolBar();
    createStatusBar();
    installCodeTabShortcuts();
    restoreLayout();

    decompilerProc_ = new QProcess(this);
    // Merge stderr into stdout so the console shows interleaved output the
    // way a terminal would — no [stderr] noise, just chronological lines.
    decompilerProc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(decompilerProc_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RetDecMainWindow::onDecompilerProcessFinished);
    connect(decompilerProc_, &QProcess::errorOccurred,
            this, &RetDecMainWindow::onDecompilerProcessError);

    decompileLogPollTimer_ = new QTimer(this);
    decompileLogPollTimer_->setInterval(500);
    connect(decompileLogPollTimer_, &QTimer::timeout,
            this, &RetDecMainWindow::onDecompileLogPollTick);

    if (target_) {
        connect(&AppSettings::instance(), &AppSettings::settingsChanged,
                target_, &panels::TargetPanel::syncDecompilerConfigFromAppSettings);
    }
    connect(&AppSettings::instance(), &AppSettings::settingsChanged,
            this, &RetDecMainWindow::applyEditorFontFromSettings);
    applyEditorFontFromSettings();

    updateCentralEmptyState();

    if (qEnvironmentVariableIsEmpty("RETDEC_GUI_HEADLESS")) {
        QTimer::singleShot(0, this, [this]() { tryRestoreLastSession(); });
    }
}

RetDecMainWindow::~RetDecMainWindow() {
    saveLayout();
}

// ─── Setup helpers ───────────────────────────────────────────────────────────

void RetDecMainWindow::setupDockOptions() {
    setDockOptions(QMainWindow::AnimatedDocks
                 | QMainWindow::AllowNestedDocks
                 | QMainWindow::AllowTabbedDocks);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
}

void RetDecMainWindow::createPanels() {
    binaryBrowser_  = new panels::BinaryBrowserPanel(this);
    functionList_   = new panels::FunctionListPanel(this);
    assembly_       = new panels::AssemblyPanel(this);
    irPanel_        = new panels::IRPanel(this);
    decompiledC_    = new panels::DecompiledCPanel(this);
    cfgPanel_       = new panels::CFGPanel(this);
    typeHierarchy_  = new panels::TypeHierarchyPanel(this);
    callGraph_      = new panels::CallGraphPanel(this);
    stringsBrowser_ = new panels::StringsBrowserPanel(this);
    // AI assistant removed from the v3 layout — was floating, never useful
    // out of the box, and just disrupted the working area. The panel class
    // remains in retdec-gui-panels for the API tests; the main window does
    // not instantiate or dock it.
    aiAssistant_    = nullptr;
    diagnostics_    = new panels::DiagnosticsPanel(this);
    progressPanel_  = new panels::ProgressPanel(this);
    inspect_        = new panels::InspectPanel(this);
    commandLog_     = new panels::CommandLogPanel(this);
    liveConsole_    = new panels::LiveConsolePanel(this);
    target_         = new panels::TargetPanel(this);
    signatureStudio_ = new panels::SignatureStudioPanel(this);
    comparePanel_   = new panels::DiffPanel(this);
    triageBanner_   = new panels::TriageBanner(this);
    triPane_        = new panels::TriPaneCodeView(this);

    connect(target_, &panels::TargetPanel::applyRequested, this,
            &RetDecMainWindow::onTargetApplyToProject);

    analysisBridge_ = new panels::AnalysisBridge(this);
    progressPanel_->connectBridge(analysisBridge_);

    connect(progressPanel_, &panels::ProgressPanel::analysisCompleted,
            this, [this](qint64) { onAnalysisFinished(); });
    connect(analysisBridge_, &panels::AnalysisBridge::stageStarted,
            this, &RetDecMainWindow::onAnalysisStageChanged);

    // Function navigation.
    connect(binaryBrowser_,  &panels::BinaryBrowserPanel::addressNavigated,
            assembly_,        &panels::AssemblyPanel::onAddressNavigated);
    connect(functionList_,   &panels::FunctionListPanel::functionSelected,
            assembly_,        &panels::AssemblyPanel::onAddressNavigated);
    connect(functionList_,   &panels::FunctionListPanel::functionSelected,
            irPanel_,         &panels::IRPanel::onFunctionSelected);
    connect(functionList_,   &panels::FunctionListPanel::functionSelected,
            decompiledC_,     &panels::DecompiledCPanel::onFunctionSelected);
    connect(functionList_,   &panels::FunctionListPanel::functionSelected,
            cfgPanel_,        &panels::CFGPanel::onFunctionSelected);
    connect(functionList_,   &panels::FunctionListPanel::functionSelected,
            callGraph_,       &panels::CallGraphPanel::onFunctionSelected);
    connect(functionList_,   &panels::FunctionListPanel::functionSelected,
            triPane_,         &panels::TriPaneCodeView::onFunctionSelected);
    connect(functionList_,   &panels::FunctionListPanel::functionSelected,
            this,             &RetDecMainWindow::onFunctionArtifactViews);
    connect(functionList_,   &panels::FunctionListPanel::redecompileRequested,
            this,             &RetDecMainWindow::onRedecompileSelectedFunction);
    connect(functionList_,   &panels::FunctionListPanel::selectionChanged,
            this,             [this]() {
                if (redecompileFunctionAct_)
                    redecompileFunctionAct_->setEnabled(
                            functionList_->selectedEntry().has_value());
            });
    connect(triPane_,        &panels::TriPaneCodeView::addressNavigated,
            assembly_,        &panels::AssemblyPanel::onAddressNavigated);

    // Cross-panel navigation (Tier 1).
    connect(cfgPanel_, &panels::CFGPanel::blockNavigationRequested,
            this, [this](uint64_t addr) { navigateToAddress(addr, kDocAssembly); });
    connect(callGraph_, &panels::CallGraphPanel::functionNavigationRequested,
            this, [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); });
    connect(typeHierarchy_, &panels::TypeHierarchyPanel::classSelected,
            functionList_, &panels::FunctionListPanel::filterByClass);
    connect(typeHierarchy_, &panels::TypeHierarchyPanel::vtableSlotNavigated,
            this, [this](uint64_t addr) { navigateToAddress(addr, kDocDecompiledC); });
    connect(stringsBrowser_, &panels::StringsBrowserPanel::addressNavigated,
            this, [this](uint64_t addr) { navigateToAddress(addr, kDocAssembly); });
    connect(diagnostics_, &panels::PanelBase::addressNavigated,
            this, [this](uint64_t addr) {
        navigateToAddress(addr, kDocDecompiledC);
        if (decompiledC_ && functionList_) {
            const auto entry = functionList_->selectedEntry();
            if (entry && entry->address == addr)
                decompiledC_->scrollToFunction(addr, entry->name.isEmpty()
                                                           ? entry->rawName
                                                           : entry->name);
            else
                decompiledC_->scrollToFunction(addr, QString());
        }
    });

    // Status-bar forwarding.
    auto connectStatus = [this](auto* panel) {
        connect(panel, &panels::PanelBase::statusMessage,
                this, [this](const QString& msg, int ms) {
                    statusBar()->showMessage(msg, ms);
                });
    };
    connectStatus(binaryBrowser_);
    connectStatus(functionList_);
    connectStatus(assembly_);
    connectStatus(irPanel_);
    connectStatus(decompiledC_);
    connectStatus(cfgPanel_);
    connectStatus(diagnostics_);

    connect(diagnostics_, &panels::DiagnosticsPanel::errorMessageAdded, this, [this] {
        if (quitWhenDecompileFinishes_)
            return;
        focusOutputTab(kOutProblems);
    });

    // Triage banner ↔ main window.
    connect(triageBanner_, &panels::TriageBanner::decompileRequested,
            this, &RetDecMainWindow::onRunFullAnalysis);
    connect(triageBanner_, &panels::TriageBanner::unpackRequested, this, [this] {
        if (workspaceTabWidget_)
            workspaceTabWidget_->setCurrentIndex(kWsInspect);
        if (dockWorkspace_) {
            dockWorkspace_->show();
            dockWorkspace_->raise();
        }
        statusBar()->showMessage(QStringLiteral(
                "Use the Inspect panel's 'Run unpacker' button. The unpacker output path is preconfigured."),
                6000);
    });
    connect(triageBanner_, &panels::TriageBanner::moreActionsRequested, this, [this] {
        QMenu menu(this);
        menu.addAction(QStringLiteral("Re-run fileinfo"),
                       [this] { if (project_) inspect_->runFileinfo(project_->binaryPath(),
                                                                     resolveRetdecFileinfoExecutable()); });
        menu.addAction(QStringLiteral("Open in Inspect panel"), [this] {
            if (workspaceTabWidget_) workspaceTabWidget_->setCurrentIndex(kWsInspect);
            if (dockWorkspace_) { dockWorkspace_->show(); dockWorkspace_->raise(); }
        });
        menu.addAction(QStringLiteral("Configure decompiler…"),
                       this, &RetDecMainWindow::onConfigure);
        menu.exec(QCursor::pos());
    });
    connect(triageBanner_, &panels::TriageBanner::dismissed, this, [this] {
        triageBanner_->hide();
    });
    connect(triageBanner_, &panels::TriageBanner::targetDetailsRequested, this, [this] {
        focusWorkspaceTab(kWsTarget);
    });

    connect(inspect_, &panels::InspectPanel::cliToolFinished, this,
            &RetDecMainWindow::onCliToolLogged);
    connect(inspect_, &panels::InspectPanel::processStarting, this,
            [this](QProcess* p, const QString& l) { streamProcessToConsole(p, l); });
    connect(inspect_, &panels::InspectPanel::requestOpenBinary, this,
            [this](const QString& p) {
        if (!QFileInfo::exists(p)) {
            statusBar()->showMessage(QStringLiteral("File not found: %1").arg(p), 5000);
            return;
        }
        openBinary(QFileInfo(p).absoluteFilePath());
    });
    connect(inspect_, &panels::InspectPanel::fileinfoReady, binaryBrowser_,
            &panels::BinaryBrowserPanel::populateFromFileinfo);
    connect(signatureStudio_, &panels::SignatureStudioPanel::cliToolFinished, this,
            &RetDecMainWindow::onCliToolLogged);
}

void RetDecMainWindow::createDockLayout() {
    // ── CENTRAL: triage banner + document tabs ────────────────────────────────
    documentTabs_ = new QTabWidget(this);
    documentTabs_->setObjectName(QStringLiteral("documentTabs"));
    documentTabs_->setDocumentMode(true);
    documentTabs_->setMovable(true);
    documentTabs_->setTabsClosable(false);
    documentTabs_->setTabPosition(QTabWidget::North);
    documentTabs_->addTab(decompiledC_, QStringLiteral("Decompiled C"));
    documentTabs_->addTab(assembly_,    QStringLiteral("Assembly"));
    documentTabs_->addTab(irPanel_,     QStringLiteral("IR (SSA)"));
    documentTabs_->addTab(cfgPanel_,    QStringLiteral("CFG"));
    documentTabs_->addTab(triPane_,     QStringLiteral("Synced (Asm ┃ IR ┃ C)"));
    documentTabs_->setTabToolTip(kDocDecompiledC,
            QStringLiteral("Decompiled C/C++ source — primary view (Ctrl+1)."));
    documentTabs_->setTabToolTip(kDocAssembly,
            QStringLiteral("Capstone-disassembled instructions (Ctrl+2)."));
    documentTabs_->setTabToolTip(kDocIR,
            QStringLiteral("LLVM IR after RetDec passes (Ctrl+3)."));
    documentTabs_->setTabToolTip(kDocCFG,
            QStringLiteral("Function control-flow graph (Ctrl+4)."));
    documentTabs_->setTabToolTip(kDocSynced,
            QStringLiteral("Cross-highlighted Asm ┃ IR ┃ C side-by-side view (Ctrl+5)."));

    if (auto* style = this->style()) {
        documentTabs_->setTabIcon(kDocDecompiledC,
                style->standardIcon(QStyle::SP_FileIcon));
        documentTabs_->setTabIcon(kDocAssembly,
                style->standardIcon(QStyle::SP_ComputerIcon));
        documentTabs_->setTabIcon(kDocIR,
                style->standardIcon(QStyle::SP_FileDialogDetailedView));
        documentTabs_->setTabIcon(kDocCFG,
                style->standardIcon(QStyle::SP_FileDialogListView));
        documentTabs_->setTabIcon(kDocSynced,
                style->standardIcon(QStyle::SP_BrowserReload));
    }

    startEmptyState_ = new widgets::EmptyStateWidget(this);
    startEmptyState_->setTitle(QStringLiteral("Open a binary to begin"));
    startEmptyState_->setHint(QStringLiteral(
            "Drag and drop an executable here, or use the buttons below."));
    if (auto* style = this->style()) {
        startEmptyState_->setIcon(style->standardIcon(QStyle::SP_DialogOpenButton));
    }

    auto* openBinaryBtn = new QPushButton(QStringLiteral("Open Binary…"), this);
    auto* openProjectBtn = new QPushButton(QStringLiteral("Open Project…"), this);
    connect(openBinaryBtn, &QPushButton::clicked, this, &RetDecMainWindow::onOpenBinary);
    connect(openProjectBtn, &QPushButton::clicked, this, &RetDecMainWindow::onOpenProject);

    auto* startActions = new QWidget(this);
    auto* startActionsLay = new QHBoxLayout(startActions);
    startActionsLay->setContentsMargins(0, 0, 0, 0);
    startActionsLay->setSpacing(12);
    startActionsLay->addWidget(openBinaryBtn);
    startActionsLay->addWidget(openProjectBtn);

    auto* startPage = new QWidget(this);
    auto* startLay = new QVBoxLayout(startPage);
    startLay->setContentsMargins(24, 24, 24, 24);
    startLay->addStretch(1);
    startLay->addWidget(startEmptyState_, 0, Qt::AlignHCenter);
    startLay->addWidget(startActions, 0, Qt::AlignHCenter);
    startLay->addStretch(2);

    centralStack_ = new QStackedWidget(this);
    centralStack_->setObjectName(QStringLiteral("centralStack"));
    centralStack_->addWidget(startPage);
    centralStack_->addWidget(documentTabs_);

    auto* centralHost = new QWidget(this);
    auto* centralLay  = new QVBoxLayout(centralHost);
    centralLay->setContentsMargins(6, 6, 6, 0);
    centralLay->setSpacing(6);
    centralLay->addWidget(triageBanner_);
    centralLay->addWidget(centralStack_, 1);
    setCentralWidget(centralHost);

    // ── LEFT: Functions (single dock, no inner tabs) ──────────────────────────
    dockFunctions_ = makeDock(QStringLiteral("Functions"), functionList_);
    dockFunctions_->setObjectName(QStringLiteral("dock_functions"));

    // ── RIGHT: Strings + Inspect + Binary + Target ────────────────────────────
    workspaceTabWidget_ = new QTabWidget();
    workspaceTabWidget_->setDocumentMode(true);
    workspaceTabWidget_->setMovable(true);
    workspaceTabWidget_->addTab(stringsBrowser_, QStringLiteral("Strings"));
    workspaceTabWidget_->addTab(inspect_,        QStringLiteral("Inspect"));
    workspaceTabWidget_->addTab(binaryBrowser_,  QStringLiteral("Binary"));
    workspaceTabWidget_->addTab(target_,         QStringLiteral("Target"));
    dockWorkspace_ = makeDock(QStringLiteral("Workspace"), workspaceTabWidget_);
    dockWorkspace_->setObjectName(QStringLiteral("dock_workspace"));

    // ── BOTTOM: Console + Problems + History + Progress ───────────────────────
    outputTabs_ = new QTabWidget(this);
    outputTabs_->setObjectName(QStringLiteral("outputTabs"));
    outputTabs_->setDocumentMode(true);
    outputTabs_->setMovable(false);
    outputTabs_->addTab(liveConsole_,   QStringLiteral("Console"));
    outputTabs_->addTab(diagnostics_,  QStringLiteral("Problems"));
    outputTabs_->addTab(commandLog_,   QStringLiteral("History"));
    outputTabs_->addTab(progressPanel_, QStringLiteral("Progress"));
    outputTabs_->setTabVisible(kOutProgress, false);
    dockOutput_ = makeDock(QStringLiteral("Output"), outputTabs_);
    dockOutput_->setObjectName(QStringLiteral("dock_output"));

    // AI Assistant intentionally NOT docked or floated in v3.
    dockAI_ = nullptr;

    addDockWidget(Qt::LeftDockWidgetArea,   dockFunctions_);
    addDockWidget(Qt::RightDockWidgetArea,  dockWorkspace_);
    addDockWidget(Qt::BottomDockWidgetArea, dockOutput_);

    documentTabs_->setCurrentIndex(kDocDecompiledC);
    workspaceTabWidget_->setCurrentIndex(kWsStrings);

    // Reasonable default widths so the centre dominates.
    resizeDocks({dockFunctions_, dockWorkspace_}, {240, 240}, Qt::Horizontal);
    resizeDocks({dockOutput_},                    {180},      Qt::Vertical);
}

void RetDecMainWindow::createMenus() {
    // ── File ──────────────────────────────────────────────────────────────────
    fileMenu_ = menuBar()->addMenu(QStringLiteral("&File"));
    auto* openBinaryAct  = fileMenu_->addAction(QStringLiteral("Open &Binary…"));
    auto* openProjectAct = fileMenu_->addAction(QStringLiteral("Open &Project…"));
    fileMenu_->addSeparator();
    recentMenu_ = fileMenu_->addMenu(QStringLiteral("&Recent Files"));
    fileMenu_->addSeparator();
    saveProjectAct_   = fileMenu_->addAction(QStringLiteral("Save &Project"));
    saveProjectAsAct_ = fileMenu_->addAction(QStringLiteral("Save Project &As…"));
    saveProjectAsAct_->setShortcut(QKeySequence::SaveAs);
    fileMenu_->addSeparator();
    auto* saveDecompAct  = fileMenu_->addAction(QStringLiteral("Save Decompiled…"));
    auto* exportCmakeAct = fileMenu_->addAction(QStringLiteral("Export CMakeLists.txt…"));
    auto* exportBundleAct = fileMenu_->addAction(QStringLiteral("Export Decompile Bundle…"));
    auto* batchDecompileAct = fileMenu_->addAction(QStringLiteral("Batch Decompile…"));
    fileMenu_->addSeparator();
    auto* quitAct = fileMenu_->addAction(QStringLiteral("&Quit"));

    openBinaryAct->setShortcut(QKeySequence::Open);
    openProjectAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_O);
    saveDecompAct->setShortcut(QKeySequence::Save);
    saveProjectAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    quitAct->setShortcut(QKeySequence::Quit);

    connect(openBinaryAct,  &QAction::triggered, this, &RetDecMainWindow::onOpenBinary);
    connect(openProjectAct, &QAction::triggered, this, &RetDecMainWindow::onOpenProject);
    connect(saveProjectAct_, &QAction::triggered, this, &RetDecMainWindow::onSaveProject);
    connect(saveProjectAsAct_, &QAction::triggered, this, &RetDecMainWindow::onSaveProjectAs);
    connect(saveDecompAct,  &QAction::triggered, this, &RetDecMainWindow::onSaveDecompiled);
    connect(exportCmakeAct, &QAction::triggered, this, &RetDecMainWindow::onExportCMake);
    connect(exportBundleAct, &QAction::triggered, this, &RetDecMainWindow::onExportDecompileBundle);
    connect(batchDecompileAct, &QAction::triggered, this, &RetDecMainWindow::onBatchDecompile);
    connect(quitAct,        &QAction::triggered, qApp, &QApplication::quit);

    openAction_ = openBinaryAct;

    // ── Analysis ──────────────────────────────────────────────────────────────
    analysisMenu_ = menuBar()->addMenu(QStringLiteral("&Analysis"));
    auto* runFullAct  = analysisMenu_->addAction(QStringLiteral("Run Full Analysis"));
    redecompileFunctionAct_ = analysisMenu_->addAction(
            QStringLiteral("Re-decompile Selected Function"));
    redecompileFunctionAct_->setEnabled(false);
    redecompileFunctionAct_->setToolTip(QStringLiteral(
            "Run retdec-decompiler with --select-functions for the function "
            "selected in the Functions list. Other functions keep declarations "
            "only; use Run Full Analysis to restore all bodies."));
    auto* runStageAct = analysisMenu_->addAction(QStringLiteral("Run Stage…"));
    analysisMenu_->addSeparator();
    auto* configAct = analysisMenu_->addAction(QStringLiteral("Configure…"));
    auto* targetAct = analysisMenu_->addAction(QStringLiteral("Target"));
    analysisMenu_->addSeparator();
    fastDecompileAct_ = analysisMenu_->addAction(
            QStringLiteral("Fast decompile (skip backend optimisations)"));
    fastDecompileAct_->setCheckable(true);
    fastDecompileAct_->setToolTip(QStringLiteral(
            "Adds --backend-no-opts --disable-static-code-detection — output "
            "is less polished but the run finishes noticeably sooner. Toggle "
            "off and re-run for the full-quality pass."));
    connect(fastDecompileAct_, &QAction::toggled, this,
            [this](bool on) { fastDecompile_ = on; });

    printAfterAllAct_ = analysisMenu_->addAction(
            QStringLiteral("Print LLVM IR after every pass (next run)"));
    printAfterAllAct_->setCheckable(true);
    printAfterAllAct_->setToolTip(QStringLiteral(
            "Adds --print-after-all to the next retdec-decompiler invocation (verbose)."));
    connect(printAfterAllAct_, &QAction::toggled, this,
            [this](bool on) { decompilePrintAfterAll_ = on; });

    runFullAct->setShortcut(Qt::Key_F5);
    stopAction_ = analysisMenu_->addAction(QStringLiteral("Stop Analysis"));
    stopAction_->setEnabled(false);
    stopAction_->setShortcut(Qt::Key_F6);

    connect(runFullAct,  &QAction::triggered, this, &RetDecMainWindow::onRunFullAnalysis);
    connect(redecompileFunctionAct_, &QAction::triggered,
            this, &RetDecMainWindow::onRedecompileSelectedFunction);
    connect(runStageAct, &QAction::triggered, this, &RetDecMainWindow::onRunStage);
    connect(configAct,   &QAction::triggered, this, &RetDecMainWindow::onConfigure);
    connect(targetAct,   &QAction::triggered, this, [this] { focusWorkspaceTab(kWsTarget); });
    connect(stopAction_, &QAction::triggered, this, &RetDecMainWindow::stopAnalysis);

    analyseAction_ = runFullAct;

    // ── View ──────────────────────────────────────────────────────────────────
    viewMenu_ = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu_->addAction(dockFunctions_->toggleViewAction());
    viewMenu_->addAction(dockWorkspace_->toggleViewAction());
    viewMenu_->addAction(dockOutput_->toggleViewAction());
    viewMenu_->addSeparator();
    {
        auto addCentreAct = [this](const QString& title, int idx, const QKeySequence& seq) {
            auto* a = viewMenu_->addAction(title);
            a->setShortcut(seq);
            connect(a, &QAction::triggered, this, [this, idx] { raiseDocumentTab(idx); });
            return a;
        };
        addCentreAct(QStringLiteral("Show Decompiled C"), kDocDecompiledC,
                     QKeySequence(QStringLiteral("Ctrl+1")));
        addCentreAct(QStringLiteral("Show Assembly"),     kDocAssembly,
                     QKeySequence(QStringLiteral("Ctrl+2")));
        addCentreAct(QStringLiteral("Show IR"),           kDocIR,
                     QKeySequence(QStringLiteral("Ctrl+3")));
        addCentreAct(QStringLiteral("Show CFG"),          kDocCFG,
                     QKeySequence(QStringLiteral("Ctrl+4")));
        addCentreAct(QStringLiteral("Show Synced view"),  kDocSynced,
                     QKeySequence(QStringLiteral("Ctrl+5")));
    }
    viewMenu_->addSeparator();
    {
        auto* showConsoleAct = viewMenu_->addAction(QStringLiteral("Show Console"));
        showConsoleAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+`")));
        connect(showConsoleAct, &QAction::triggered, this, [this] {
            focusOutputTab(kOutConsole);
        });
        auto* showProblemsAct = viewMenu_->addAction(QStringLiteral("Show Problems"));
        showProblemsAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+`")));
        connect(showProblemsAct, &QAction::triggered, this, [this] {
            focusOutputTab(kOutProblems);
        });
        auto* showHistoryAct = viewMenu_->addAction(QStringLiteral("Show History"));
        connect(showHistoryAct, &QAction::triggered, this, [this] {
            focusOutputTab(kOutHistory);
        });
        auto* showProgressAct = viewMenu_->addAction(QStringLiteral("Show Progress"));
        connect(showProgressAct, &QAction::triggered, this, [this] {
            focusOutputTab(kOutProgress);
        });
    }
    viewMenu_->addSeparator();
    {
        auto* showBanner = viewMenu_->addAction(QStringLiteral("Show triage banner"));
        showBanner->setCheckable(true);
        showBanner->setChecked(triageBanner_->isVisible());
        connect(showBanner, &QAction::toggled, this, [this](bool on) {
            if (!triageBanner_) return;
            if (on && project_) triageBanner_->show();
            else                triageBanner_->hide();
        });
        connect(triageBanner_, &panels::TriageBanner::dismissed, showBanner,
                [showBanner] { showBanner->setChecked(false); });
    }

    viewMenu_->addSeparator();
    {
        auto* resetLayoutAct = viewMenu_->addAction(QStringLiteral("Reset &Layout"));
        connect(resetLayoutAct, &QAction::triggered,
                this, &RetDecMainWindow::onResetLayout);
    }

    // ── Tools ─────────────────────────────────────────────────────────────────
    toolsMenu_ = menuBar()->addMenu(QStringLiteral("&Tools"));
    auto* settingsAct    = toolsMenu_->addAction(QStringLiteral("&Settings…"));
    settingsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    toolsMenu_->addSeparator();
    auto* sigStudioAct   = toolsMenu_->addAction(QStringLiteral("Signature Studio…"));
    auto* callGraphAct   = toolsMenu_->addAction(QStringLiteral("Call Graph…"));
    auto* typeHierAct    = toolsMenu_->addAction(QStringLiteral("Type Hierarchy…"));
    toolsMenu_->addSeparator();
    auto* compareAct     = toolsMenu_->addAction(QStringLiteral("Compare…"));

    connect(settingsAct, &QAction::triggered, this, &RetDecMainWindow::onSettings);
    connect(sigStudioAct, &QAction::triggered, this, &RetDecMainWindow::onShowSignatureStudio);
    connect(callGraphAct, &QAction::triggered, this, &RetDecMainWindow::onShowCallGraph);
    connect(typeHierAct,  &QAction::triggered, this, &RetDecMainWindow::onShowTypeHierarchy);
    connect(compareAct,   &QAction::triggered, this, &RetDecMainWindow::onCompare);

    // ── Help ──────────────────────────────────────────────────────────────────
    helpMenu_ = menuBar()->addMenu(QStringLiteral("&Help"));
    auto* docAct = helpMenu_->addAction(QStringLiteral("Documentation…"));
    helpMenu_->addSeparator();
    helpMenu_->addSeparator();
    auto* shortcutsAct = helpMenu_->addAction(QStringLiteral("Keyboard Shortcuts…"));
    auto* aboutAct = helpMenu_->addAction(QStringLiteral("About RetDec…"));
    connect(docAct,   &QAction::triggered, this, &RetDecMainWindow::onOpenDocumentation);
    connect(shortcutsAct, &QAction::triggered, this, &RetDecMainWindow::onKeyboardShortcuts);
    connect(aboutAct, &QAction::triggered, this, &RetDecMainWindow::onAbout);

    updateProjectFileActions();
}

void RetDecMainWindow::createToolBar() {
    mainToolBar_ = addToolBar(QStringLiteral("Main"));
    mainToolBar_->setObjectName(QStringLiteral("mainToolbar"));
    mainToolBar_->setMovable(false);
    mainToolBar_->addAction(openAction_);
    mainToolBar_->addSeparator();
    mainToolBar_->addAction(analyseAction_);
    mainToolBar_->addAction(stopAction_);
}

void RetDecMainWindow::raiseDocumentTab(int index) {
    if (!documentTabs_) return;
    if (index < 0 || index >= documentTabs_->count())
        index = kDocDecompiledC;
    documentTabs_->setCurrentIndex(index);
    if (auto* w = documentTabs_->currentWidget())
        w->setFocus();
}

void RetDecMainWindow::navigateToAddress(uint64_t address, int documentTab) {
    if (dockFunctions_) {
        dockFunctions_->show();
        dockFunctions_->raise();
    }
    bool matchedFunction = false;
    if (functionList_) {
        functionList_->selectFunction(address);
        const auto entry = functionList_->selectedEntry();
        matchedFunction = entry.has_value() && entry->address == address;
    }
    raiseDocumentTab(documentTab);
    if (!matchedFunction && assembly_)
        assembly_->onAddressNavigated(address);
}

void RetDecMainWindow::onResetLayout() {
    QSettings settings(QStringLiteral("retdec"), QStringLiteral("retdec-gui"));
    settings.remove(QStringLiteral("geometry"));
    settings.remove(QStringLiteral("windowState"));
    settings.remove(QStringLiteral("preferredDocumentTabIndex"));

    for (QDockWidget* dock : findChildren<QDockWidget*>()) {
        dock->setFloating(false);
        dock->show();
    }
    if (dockFunctions_)
        addDockWidget(Qt::LeftDockWidgetArea, dockFunctions_);
    if (dockWorkspace_)
        addDockWidget(Qt::RightDockWidgetArea, dockWorkspace_);
    if (dockOutput_)
        addDockWidget(Qt::BottomDockWidgetArea, dockOutput_);

    resize(1280, 800);
    resizeDocks({dockFunctions_, dockWorkspace_}, {240, 240}, Qt::Horizontal);
    resizeDocks({dockOutput_}, {180}, Qt::Vertical);
    raiseDocumentTab(kDocDecompiledC);
    if (documentTabs_)
        documentTabs_->setCurrentIndex(kDocDecompiledC);
    if (workspaceTabWidget_)
        workspaceTabWidget_->setCurrentIndex(kWsStrings);
    if (outputTabs_)
        outputTabs_->setCurrentIndex(kOutConsole);
    statusBar()->showMessage(QStringLiteral("Layout reset to defaults"), 5000);
}

void RetDecMainWindow::focusOutputTab(int tabIndex) {
    if (outputTabs_) {
        if (tabIndex < 0 || tabIndex >= outputTabs_->count())
            tabIndex = kOutConsole;
        outputTabs_->setCurrentIndex(tabIndex);
    }
    if (dockOutput_) {
        dockOutput_->show();
        dockOutput_->raise();
    }
}

void RetDecMainWindow::focusWorkspaceTab(int tabIndex) {
    if (workspaceTabWidget_) {
        if (tabIndex < 0 || tabIndex >= workspaceTabWidget_->count())
            tabIndex = kWsStrings;
        workspaceTabWidget_->setCurrentIndex(tabIndex);
        if (auto* w = workspaceTabWidget_->currentWidget())
            w->setFocus();
    }
    if (dockWorkspace_) {
        dockWorkspace_->show();
        dockWorkspace_->raise();
    }
}

void RetDecMainWindow::updateProjectFileActions() {
    const bool ok = static_cast<bool>(project_);
    if (saveProjectAct_)   saveProjectAct_->setEnabled(ok);
    if (saveProjectAsAct_) saveProjectAsAct_->setEnabled(ok);
}

void RetDecMainWindow::installCodeTabShortcuts() {
    for (int i = 0; i < 5; ++i) {
        const QKeySequence seq(QStringLiteral("Ctrl+%1").arg(i + 1));
        auto* sc = new QShortcut(seq, this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this, [this, i] {
            QSettings settings(QStringLiteral("retdec"), QStringLiteral("retdec-gui"));
            settings.setValue(QStringLiteral("preferredDocumentTabIndex"), i);
            raiseDocumentTab(i);
        });
    }
}

void RetDecMainWindow::onCliToolLogged(const QString& tool, const QStringList& args,
                                       const QString& cwd, int exitCode, qint64 elapsedMs,
                                       const QString& outputTail) {
    if (commandLog_)
        commandLog_->appendRun(tool, args, cwd, exitCode, elapsedMs, outputTail);
    if (liveConsole_)
        liveConsole_->appendFooter(tool, exitCode, elapsedMs);
    if (tool == QStringLiteral("retdec-fileinfo"))
        refreshTriageFromInspect();
}

void RetDecMainWindow::onTargetApplyToProject() {
    if (!project_) {
        statusBar()->showMessage(QStringLiteral("No project loaded."), 3000);
        return;
    }
    project_->setArch(target_->archText().trimmed());
    project_->setOs(target_->osText().trimmed());
    uint64_t ep = 0;
    if (!parseEntryPointString(target_->entryText(), &ep)) {
        QMessageBox::warning(this, QStringLiteral("Target"),
                             QStringLiteral("Invalid entry point (use 0x… hex or decimal)."));
        return;
    }
    project_->setEntryPoint(ep);
    statusBar()->showMessage(QStringLiteral("Target fields applied to project (save .retdec to persist)."),
                             5000);
}

void RetDecMainWindow::onShowSignatureStudio() {
    showAsToolWindow(this, signatureStudio_, QStringLiteral("Signature Studio"),
                     QSize(900, 600));
}

void RetDecMainWindow::onShowCallGraph() {
    showAsToolWindow(this, callGraph_, QStringLiteral("Call Graph"),
                     QSize(900, 700));
}

void RetDecMainWindow::onShowTypeHierarchy() {
    showAsToolWindow(this, typeHierarchy_, QStringLiteral("Type Hierarchy"),
                     QSize(700, 600));
    refreshTypeHierarchyFromArtifacts();
}

void RetDecMainWindow::createStatusBar() {
    statusFile_    = new QLabel(this);
    statusStage_   = new QLabel(this);
    statusFnCount_ = new QLabel(this);
    statusDecompile_ = new QLabel(this);
    statusElapsed_ = new QLabel(this);
    analysisBar_   = new QProgressBar(this);
    analysisBar_->setFixedWidth(150);
    analysisBar_->setFixedHeight(14);
    analysisBar_->setVisible(false);
    analysisBar_->setTextVisible(false);

    statusBar()->addWidget(statusFile_,    2);
    statusBar()->addWidget(statusStage_,   2);
    statusBar()->addPermanentWidget(statusFnCount_);
    statusBar()->addPermanentWidget(statusDecompile_);
    statusBar()->addPermanentWidget(analysisBar_);
    statusBar()->addPermanentWidget(statusElapsed_);

    setStatusFile(QStringLiteral("No binary loaded"));
    setStatusStage(QStringLiteral("Idle"));
    statusFnCount_->setText(QStringLiteral("— functions"));
    statusDecompile_->setVisible(false);

    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(500);
    connect(elapsedTimer_, &QTimer::timeout,
            this, &RetDecMainWindow::updateElapsedTime);
}

void RetDecMainWindow::streamProcessToConsole(QProcess* proc, const QString& label) {
    if (!proc || !liveConsole_) return;
    liveConsole_->attachProcess(proc, label);
}

void RetDecMainWindow::setFastDecompilePreset(bool on) {
    fastDecompile_ = on;
    if (fastDecompileAct_) {
        fastDecompileAct_->blockSignals(true);
        fastDecompileAct_->setChecked(on);
        fastDecompileAct_->blockSignals(false);
    }
}

void RetDecMainWindow::enableQuitWhenDecompileFinishes(bool on) {
    quitWhenDecompileFinishes_ = on;
}

void RetDecMainWindow::runDecompileForBenchmark() {
    onRunFullAnalysis();
}

// ─── Decompile-artifact loader (used by both cache reuse + post-run) ─────────

bool RetDecMainWindow::loadDecompileArtifacts(const QString& cPath,
                                              std::optional<uint64_t> reselectAddress) {
    const QString absC = QFileInfo(cPath).absoluteFilePath();
    const DecompileArtifactPaths paths = pathsFromOutputC(absC);

    DecompileArtifacts art;
    QString err;
    if (!loadDecompileArtifactsFromPaths(paths, art, &err)) {
        diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Warning,
                QStringLiteral("retdec-decompiler"), err);
        return false;
    }

    if (project_)
        project_->setDecompiledPath(absC);

    loadedArtifacts_ = std::move(art);

    // Disk parse only above; all QWidget updates are deferred so openBinary and
    // other callers return immediately. Synchronous call-graph layout, TriPane
    // loadFunction, and QTextDocument work during openBinary can stall headless
    // GUI tests (ComprehensiveSmokeTest.CachedDecompileLoadsInstantlyOnReopen).
    const std::optional<uint64_t> reselect = reselectAddress;
    QTimer::singleShot(0, this, [this, absC, reselect]() {
        if (!loadedArtifacts_)
            return;

        if (!decompiledC_->setSourceFromPath(absC)) {
            diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Warning,
                    QStringLiteral("retdec-decompiler"),
                    QStringLiteral("Could not read: %1").arg(absC));
            return;
        }

        if (functionList_)
            functionList_->setFunctions(loadedArtifacts_->functions);
        setStatusFunctionCount(static_cast<int>(loadedArtifacts_->functions.size()));

        if (stringsBrowser_)
            stringsBrowser_->setStrings(loadedArtifacts_->strings);
        if (callGraph_ && !loadedArtifacts_->callGraphNodes.empty()
            && qEnvironmentVariableIsEmpty("RETDEC_GUI_HEADLESS")) {
            callGraph_->loadGraph(loadedArtifacts_->callGraphNodes,
                                 loadedArtifacts_->callGraphEdges);
        }
        if (typeHierarchy_ && isPanelToolWindowVisible(typeHierarchy_, this))
            refreshTypeHierarchyFromArtifacts();

        const qint64 cSize = QFileInfo(absC).size();
        diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Info,
                QStringLiteral("retdec-decompiler"),
                QStringLiteral("Output: %1 (%2 KiB)").arg(absC).arg(cSize / 1024));
        statusBar()->showMessage(
                QStringLiteral("Loaded decompile artifacts (%1 KiB C, %2 functions)")
                        .arg(cSize / 1024)
                        .arg(loadedArtifacts_->functions.size()),
                5000);

        if (documentTabs_)
            documentTabs_->setCurrentIndex(kDocDecompiledC);

        if (reselect && qEnvironmentVariableIsEmpty("RETDEC_GUI_HEADLESS")) {
            functionList_->selectFunction(*reselect);
        } else if (!loadedArtifacts_->functions.empty()
                   && qEnvironmentVariableIsEmpty("RETDEC_GUI_HEADLESS")) {
            const auto f0 = loadedArtifacts_->functions.front();
            onFunctionArtifactViews(f0.address, f0.name);
        }
    });

    return true;
}

void RetDecMainWindow::onFunctionArtifactViews(uint64_t address, const QString& name) {
    if (!loadedArtifacts_)
        return;
    populateFunctionViews(*loadedArtifacts_, address, name,
                          assembly_, irPanel_, cfgPanel_, triPane_);

    int startLine = -1;
    int endLine = -1;
    for (const auto& f : loadedArtifacts_->functions) {
        if (f.address == address) {
            startLine = f.startLine;
            endLine = f.endLine;
            break;
        }
    }
    if (decompiledC_)
        decompiledC_->scrollToFunction(name, startLine, endLine);
}

void RetDecMainWindow::refreshTypeHierarchyFromArtifacts() {
    if (!typeHierarchy_ || !loadedArtifacts_)
        return;
    typeHierarchy_->setHierarchy(loadedArtifacts_->typeHierarchyClasses);
}

bool RetDecMainWindow::tryLoadCachedDecompile(const QString& binaryPath) {
    if (binaryPath.isEmpty())
        return false;
    const QString outDir = AppSettings::instance().decompiler.decompileOutputDir;
    const QString cPath = locateGuiDecompiledCPath(binaryPath, outDir);
    const QFileInfo cFi(cPath);
    const QFileInfo bFi(binaryPath);
    if (!cFi.exists() || !bFi.exists())
        return false;
    // Cache is fresh iff the .c is at least as new as the binary AND the
    // sidecar .config.json exists too (needed for the Functions panel).
    if (cFi.lastModified() < bFi.lastModified())
        return false;
    const QString cfgPath = pathsFromOutputC(cPath).configPath;
    if (!QFileInfo::exists(cfgPath))
        return false;
    if (!loadDecompileArtifacts(cPath))
        return false;
    setStatusDecompileState(QStringLiteral("cached"));
    statusBar()->showMessage(
            QStringLiteral("Loaded cached decompile (%1) — press F5 to re-decompile.")
                    .arg(cFi.lastModified().toString(QStringLiteral("HH:mm"))),
            8000);
    return true;
}

// ─── Triage refresh ──────────────────────────────────────────────────────────

void RetDecMainWindow::refreshTriageFromInspect() {
    if (!triageBanner_ || !project_)
        return;
    // The InspectPanel holds the JSON internally; pull a few well-known fields
    // via its public summary text. For now we just use the project's stored
    // arch/os if set, and a best-effort packer hint from diagnostics.
    QString arch = project_->arch();
    QString os   = project_->os();
    qint64 sz    = QFileInfo(project_->binaryPath()).size();
    // Detect "packed" from a simple keyword scan of the fileinfo raw output
    // (avoids exposing a private JSON cache in InspectPanel).
    QString packer;
    if (auto* raw = inspect_->findChild<QPlainTextEdit*>()) {
        const QString t = raw->toPlainText();
        if (t.contains(QStringLiteral("UPX"), Qt::CaseInsensitive)) packer = QStringLiteral("UPX");
        else if (t.contains(QStringLiteral("ASPack"), Qt::CaseInsensitive)) packer = QStringLiteral("ASPack");
        else if (t.contains(QStringLiteral("\"packed\": true"))) packer = QStringLiteral("unknown");
    }
    QString format;
    if (!project_->binaryPath().isEmpty()) {
        const QString ext = QFileInfo(project_->binaryPath()).suffix().toLower();
        if (ext == QStringLiteral("exe") || ext == QStringLiteral("dll")) format = QStringLiteral("PE");
        else if (ext == QStringLiteral("so") || ext == QStringLiteral("elf")) format = QStringLiteral("ELF");
        else if (ext == QStringLiteral("dylib")) format = QStringLiteral("Mach-O");
    }
    triageBanner_->setMetadata(format, arch, os, sz, packer);
}

// ─── Project management ──────────────────────────────────────────────────────

void RetDecMainWindow::openBinary(const QString& path) {
    const QString absPath = QFileInfo(path).absoluteFilePath();
    setWindowTitle(QStringLiteral("RetDec — %1").arg(absPath));
    project_ = std::make_unique<ProjectFile>(absPath);
    lastProjectSavePath_.clear();
    loadedArtifacts_.reset();
    setStatusFile(absPath);
    statusFnCount_->setText(QStringLiteral("— functions"));
    setStatusDecompileState(QString());
    addRecentFile(absPath);
    AppSettings::instance().general.lastBinaryPath = absPath;
    AppSettings::instance().save();
    binaryBrowser_->loadBinary(absPath);
    target_->setFromProject(project_.get());
    signatureStudio_->setActiveBinary(absPath);
    diagnostics_->clear();
    commandLog_->clear();
    liveConsole_->clear();
    progressPanel_->resetAll();
    if (triageBanner_) {
        triageBanner_->setBinary(absPath);
        triageBanner_->setActionsEnabled(true);
    }
    updateProjectFileActions();
    updateCentralEmptyState();
    // Cache reuse: a 1 MB binary takes ~50 s to decompile from scratch on a
    // workstation. Re-opening the same binary in a session must not pay
    // instantly and show a "press F5 to re-decompile" hint.
    if (tryLoadCachedDecompile(absPath)) {
        // setStatusDecompileState("cached") done inside tryLoadCachedDecompile.
    } else {
        setStatusDecompileState(QStringLiteral("not decompiled"));
    }
    // Always run fileinfo so Binary Browser gets sectionTable (unless headless).
    if (!quitWhenDecompileFinishes_
        && qEnvironmentVariableIsEmpty("RETDEC_GUI_HEADLESS")) {
        inspect_->runFileinfo(absPath, resolveRetdecFileinfoExecutable());
    }
}

void RetDecMainWindow::openProject(const QString& path) {
    auto pf = std::make_unique<ProjectFile>();
    if (!pf->load(path)) {
        QMessageBox::warning(this, QStringLiteral("Open Project"),
                             QStringLiteral("Failed to load project:\n%1").arg(pf->lastError()));
        return;
    }
    project_ = std::move(pf);
    lastProjectSavePath_ = QFileInfo(path).absoluteFilePath();
    loadedArtifacts_.reset();
    setWindowTitle(QStringLiteral("RetDec — %1").arg(project_->binaryPath()));
    setStatusFile(project_->binaryPath());
    statusFnCount_->setText(QStringLiteral("— functions"));
    setStatusDecompileState(QString());
    addRecentFile(path);
    binaryBrowser_->loadBinary(project_->binaryPath());
    target_->setFromProject(project_.get());
    signatureStudio_->setActiveBinary(project_->binaryPath());
    diagnostics_->clear();
    commandLog_->clear();
    liveConsole_->clear();
    progressPanel_->resetAll();
    if (triageBanner_) {
        triageBanner_->setBinary(project_->binaryPath());
        triageBanner_->setActionsEnabled(true);
    }
    updateProjectFileActions();
    updateCentralEmptyState();
    if (!tryLoadCachedDecompile(project_->binaryPath())) {
        setStatusDecompileState(QStringLiteral("not decompiled"));
        if (!quitWhenDecompileFinishes_
            && qEnvironmentVariableIsEmpty("RETDEC_GUI_HEADLESS")) {
            inspect_->runFileinfo(project_->binaryPath(), resolveRetdecFileinfoExecutable());
        }
    }
}

bool RetDecMainWindow::saveProject(const QString& path) {
    if (!project_) return false;
    if (!project_->save(path)) {
        QMessageBox::warning(this, QStringLiteral("Save Project"),
                             QStringLiteral("Failed to save:\n%1").arg(project_->lastError()));
        return false;
    }
    return true;
}

void RetDecMainWindow::onSaveProject() {
    if (!project_) return;
    if (lastProjectSavePath_.isEmpty()) {
        onSaveProjectAs();
        return;
    }
    if (saveProject(lastProjectSavePath_))
        statusBar()->showMessage(QStringLiteral("Project saved: %1").arg(lastProjectSavePath_), 5000);
}

void RetDecMainWindow::onSaveProjectAs() {
    if (!project_) return;
    QString start = lastProjectSavePath_;
    if (start.isEmpty()) {
        const QFileInfo fi(project_->binaryPath());
        start = QDir(fi.absolutePath()).filePath(fi.completeBaseName() + QStringLiteral(".retdec"));
    }
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save RetDec Project"), start,
                                                QStringLiteral("RetDec Projects (*.retdec);;All Files (*)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".retdec"), Qt::CaseInsensitive))
        path += QStringLiteral(".retdec");
    if (saveProject(path)) {
        lastProjectSavePath_ = QFileInfo(path).absoluteFilePath();
        statusBar()->showMessage(QStringLiteral("Project saved: %1").arg(lastProjectSavePath_), 5000);
    }
}

// ─── Analysis lifecycle ──────────────────────────────────────────────────────

void RetDecMainWindow::startAnalysis() {
    if (analysisRunning_ || !project_) return;
    analysisRunning_ = true;
    analyseAction_->setEnabled(false);
    stopAction_->setEnabled(true);
    analysisBar_->setVisible(true);
    analysisBar_->setTextVisible(false);
    analysisBar_->setRange(0, 0);
    wallClock_.start();
    elapsedTimer_->start();
    progressPanel_->resetAll();
    setStatusStage(QStringLiteral("Running…"));
    if (outputTabs_) {
        outputTabs_->setTabVisible(kOutProgress, true);
        focusOutputTab(kOutProgress);
    }
}

void RetDecMainWindow::stopAnalysis() {
    const bool stopBatch = batchRunning_;
    if (decompilerProc_ && decompilerProc_->state() != QProcess::NotRunning) {
        decompilerProc_->terminate();
        if (!decompilerProc_->waitForFinished(2000))
            decompilerProc_->kill();
        decompilerProc_->waitForFinished(1000);
    }
    decompileLogPollTimer_->stop();
    decompileLogOffset_ = 0;
    decompileConsoleTailOffset_ = 0;
    decompilerProc_->setStandardOutputFile(QString());
    decompilerLogTemp_.reset();
    llvmPassesJsonTemp_.reset();
    if (stopBatch)
        finishBatchDecompile(true);
    if (!analysisRunning_) return;
    analysisRunning_ = false;
    analyseAction_->setEnabled(true);
    if (redecompileFunctionAct_)
        redecompileFunctionAct_->setEnabled(
                functionList_ && functionList_->selectedEntry().has_value());
    stopAction_->setEnabled(false);
    elapsedTimer_->stop();
    analysisBar_->setVisible(false);
    setStatusStage(QStringLiteral("Stopped"));
}

void RetDecMainWindow::setStatusFile(const QString& path) {
    statusFile_->setText(path.isEmpty() ? QStringLiteral("No binary loaded") : path);
}
void RetDecMainWindow::setStatusStage(const QString& stage)   { statusStage_->setText(stage); }
void RetDecMainWindow::setStatusFunctionCount(int count) {
    if (count < 0)
        statusFnCount_->setText(QStringLiteral("— functions"));
    else
        statusFnCount_->setText(QStringLiteral("%1 functions").arg(count));
}
void RetDecMainWindow::setStatusDecompileState(const QString& state) {
    if (!statusDecompile_) return;
    statusDecompile_->setText(state);
    statusDecompile_->setVisible(!state.isEmpty());
}
void RetDecMainWindow::setAnalysisProgress(int percent) {
    analysisBar_->setRange(0, 100);
    analysisBar_->setValue(percent);
}

void RetDecMainWindow::updateCentralEmptyState() {
    if (!centralStack_) return;
    const bool hasBinary = project_ && !project_->binaryPath().isEmpty();
    centralStack_->setCurrentIndex(hasBinary ? kCentralDocs : kCentralEmpty);
}

void RetDecMainWindow::tryRestoreLastSession() {
    const auto& g = AppSettings::instance().general;
    if (!g.restoreSession || g.lastBinaryPath.isEmpty())
        return;
    if (!QFileInfo::exists(g.lastBinaryPath))
        return;
    openBinary(g.lastBinaryPath);
}

void RetDecMainWindow::onOpenBinary() {
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Binary"),
        QString(),
        QStringLiteral("Executable Files (*.exe *.dll *.elf *.so *.dylib *.bin);;All Files (*)"));
    if (!path.isEmpty()) openBinary(path);
}

void RetDecMainWindow::onOpenProject() {
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open RetDec Project"),
        QString(), QStringLiteral("RetDec Projects (*.retdec);;All Files (*)"));
    if (!path.isEmpty()) openProject(path);
}

void RetDecMainWindow::onSaveDecompiled() { decompiledC_->onSaveAs(); }

void RetDecMainWindow::onExportCMake() {
    if (!project_) {
        QMessageBox::information(this, QStringLiteral("Export CMake"),
                                 QStringLiteral("Open a binary or project first."));
        return;
    }
    const QString cAbs = guessDecompiledCPath(project_.get());
    if (!QFileInfo::exists(cAbs)) {
        QMessageBox::information(
                this, QStringLiteral("Export CMake"),
                QStringLiteral("No decompiled C file found:\n%1\n\n"
                               "Run Analysis → Run Full Analysis first.").arg(cAbs));
        return;
    }
    const QString defaultCmake = QFileInfo(cAbs).absolutePath()
            + QStringLiteral("/CMakeLists.txt");
    const QString cmakePath = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export CMakeLists.txt"), defaultCmake,
            QStringLiteral("CMake project (CMakeLists.txt);;All files (*)"));
    if (cmakePath.isEmpty()) return;

    const QString cmakeDir = QFileInfo(cmakePath).absolutePath();
    QString rel = QDir(cmakeDir).relativeFilePath(cAbs);
    rel = QDir::cleanPath(rel).replace(QLatin1Char('\\'), QLatin1Char('/'));

    QString target = QFileInfo(cAbs).completeBaseName();
    target.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9_]")),
                   QStringLiteral("_"));
    if (target.isEmpty() || target.startsWith(QLatin1Char('_')))
        target = QStringLiteral("retdec_recovered");
    if (!target.isEmpty() && target[0].isDigit())
        target = QStringLiteral("t_") + target;

    const QString content = QStringLiteral(
            "# Skeleton to build recovered C produced by RetDec — adjust flags and toolchain.\n"
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(%1 LANGUAGES C)\n"
            "add_executable(%1 \"%2\")\n"
            "target_compile_features(%1 PRIVATE c_std_11)\n").arg(target, rel);

    QFile out(cmakePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Export CMake"), out.errorString());
        return;
    }
    out.write(content.toUtf8());
    statusBar()->showMessage(QStringLiteral("Wrote %1").arg(cmakePath), 5000);
}

void RetDecMainWindow::onExportDecompileBundle() {
    const bool hasLoaded = loadedArtifacts_.has_value();
    const QString cGuess = project_ ? guessDecompiledCPath(project_.get()) : QString{};
    const bool hasOnDisk = !cGuess.isEmpty() && QFileInfo::exists(cGuess);
    if (!hasLoaded && !hasOnDisk) {
        QMessageBox::information(
                this, QStringLiteral("Export Decompile Bundle"),
                QStringLiteral("No decompiled artifacts found.\n\n"
                               "Run Analysis → Run Full Analysis first."));
        return;
    }

    const QString cPath = (hasLoaded && !loadedArtifacts_->cPath.isEmpty())
                                  ? loadedArtifacts_->cPath
                                  : cGuess;
    const QFileInfo cFi(cPath);
    const QString defaultZip = cFi.absolutePath() + QLatin1Char('/')
            + cFi.completeBaseName() + QStringLiteral("-bundle.zip");
    const QString zipPath = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export Decompile Bundle"), defaultZip,
            QStringLiteral("ZIP archives (*.zip);;All files (*)"));
    if (zipPath.isEmpty())
        return;

    DecompileBundleInput bundle;
    bundle.cPath           = cPath;
    bundle.decompilerExe   = lastDecompilerExe_;
    bundle.decompilerArgs  = lastDecompilerArgs_;
    bundle.decompilerCwd   = lastDecompilerCwd_;
    if (decompilerLogTemp_)
        bundle.logFilePath = decompilerLogTemp_->fileName();
    else if (liveConsole_ && !liveConsole_->isEmpty()) {
        QTemporaryFile tmp;
        tmp.setAutoRemove(true);
        if (tmp.open()) {
            tmp.close();
            if (liveConsole_->saveAs(tmp.fileName())) {
                QFile f(tmp.fileName());
                if (f.open(QIODevice::ReadOnly))
                    bundle.logInline = f.readAll();
            }
        }
    }

    QString err;
    if (!exportDecompileBundle(bundle, zipPath, &err)) {
        QMessageBox::warning(this, QStringLiteral("Export Decompile Bundle"), err);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Exported bundle %1").arg(zipPath), 5000);
}

void RetDecMainWindow::onRecentFileTriggered() {
    auto* act = qobject_cast<QAction*>(sender());
    if (!act) return;
    const QString p = act->data().toString();
    if (p.endsWith(QStringLiteral(".retdec"), Qt::CaseInsensitive))
        openProject(p);
    else
        openBinary(p);
}

void RetDecMainWindow::onRunFullAnalysis() {
    if (!project_) {
        const QString path = QFileDialog::getOpenFileName(
                this, QStringLiteral("Open Binary"),
                QString(),
                QStringLiteral("Executable Files (*.exe *.dll *.elf *.so *.dylib *.bin);;All Files (*)"));
        if (path.isEmpty())
            return;
        openBinary(path);
    }
    if (!project_)
        return;
    if (batchRunning_) {
        statusBar()->showMessage(QStringLiteral("Batch decompile in progress"), 2500);
        return;
    }
    if (decompilerProc_->state() != QProcess::NotRunning) {
        statusBar()->showMessage(QStringLiteral("Decompiler already running"), 2500);
        return;
    }
    pendingReselectAddress_.reset();
    launchDecompilerForBinary(QFileInfo(project_->binaryPath()).absoluteFilePath(),
                                project_->arch());
}

void RetDecMainWindow::onRedecompileSelectedFunction() {
    if (!project_) {
        statusBar()->showMessage(QStringLiteral("Open a binary first"), 2500);
        return;
    }
    if (batchRunning_) {
        statusBar()->showMessage(QStringLiteral("Batch decompile in progress"), 2500);
        return;
    }
    if (decompilerProc_->state() != QProcess::NotRunning) {
        statusBar()->showMessage(QStringLiteral("Decompiler already running"), 2500);
        return;
    }
    if (!functionList_) {
        return;
    }
    const auto entry = functionList_->selectedEntry();
    if (!entry) {
        statusBar()->showMessage(QStringLiteral("Select a function in the Functions list"), 2500);
        return;
    }

    pendingReselectAddress_ = entry->address;
    const QString cliName = entry->selectFunctionsCliName();
    if (!launchDecompilerForBinary(
                QFileInfo(project_->binaryPath()).absoluteFilePath(),
                project_->arch(),
                {cliName})) {
        pendingReselectAddress_.reset();
    }
}

bool RetDecMainWindow::launchDecompilerForBinary(const QString& absBinary,
                                                  const QString& arch,
                                                  const QStringList& selectedFunctions) {
    if (decompilerProc_->state() != QProcess::NotRunning)
        return false;

    const QString decExe = resolveRetdecDecompilerExecutablePath();
    if (decExe.isEmpty()) {
        if (!quitWhenDecompileFinishes_) {
            QMessageBox::warning(this, QStringLiteral("Decompiler"),
                    QStringLiteral("retdec-decompiler was not found next to the GUI or on PATH."));
        } else {
            diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Error,
                    QStringLiteral("retdec-decompiler"),
                    QStringLiteral("retdec-decompiler was not found next to the GUI or on PATH."));
            writeHeadlessDecompileReport(0, -2, QString());
            QCoreApplication::quit();
        }
        return false;
    }

    auto& st = AppSettings::instance();
    // Headless / parity runs must match a plain CLI invocation — ignore saved
    // decompiler overrides (--config, custom LLVM pass lists, output dir, etc.).
    const QString outDir = quitWhenDecompileFinishes_
            ? QString()
            : st.decompiler.decompileOutputDir.trimmed();
    if (!outDir.isEmpty() && !QDir().mkpath(outDir)) {
        if (!quitWhenDecompileFinishes_) {
            QMessageBox::warning(this, QStringLiteral("Decompiler"),
                    QStringLiteral("Could not create decompile output directory:\n%1").arg(outDir));
        } else {
            diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Error,
                    QStringLiteral("retdec-decompiler"),
                    QStringLiteral("Could not create decompile output directory: %1").arg(outDir));
            writeHeadlessDecompileReport(0, -3, QString());
            QCoreApplication::quit();
        }
        return false;
    }
    decompilerOutputPath_ = resolveGuiDecompiledCPath(absBinary, outDir);

    DecompilerLaunchRequest req;
    req.binaryPath    = absBinary;
    req.outputPath    = decompilerOutputPath_;
    req.arch          = arch;
    req.fastDecompile = fastDecompile_;
    req.printAfterAll = quitWhenDecompileFinishes_ ? false : decompilePrintAfterAll_;
    req.selectedFunctions = selectedFunctions;
    req.decompiler    = quitWhenDecompileFinishes_ ? DecompilerSettings{} : st.decompiler;

    QString buildErr;
    llvmPassesJsonTemp_.reset();
    const QStringList args = buildDecompilerArguments(
            req, &buildErr, &llvmPassesJsonTemp_);
    if (args.isEmpty()) {
        if (!quitWhenDecompileFinishes_) {
            QMessageBox::warning(this, QStringLiteral("Decompiler"),
                    buildErr.isEmpty()
                            ? QStringLiteral("Could not build decompiler arguments.")
                            : buildErr);
        } else {
            diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Error,
                    QStringLiteral("retdec-decompiler"), buildErr);
        }
        return false;
    }
    if (!buildErr.isEmpty()) {
        diagnostics_->addMessage(
                panels::DiagnosticEntry::Severity::Warning,
                QStringLiteral("retdec-decompiler"), buildErr);
    }

    decompilerLogTemp_ = std::make_unique<QTemporaryFile>();
    decompilerLogTemp_->setAutoRemove(true);
    if (!decompilerLogTemp_->open()) {
        if (!quitWhenDecompileFinishes_) {
            QMessageBox::warning(this, QStringLiteral("Decompiler"),
                    QStringLiteral("Could not create a temporary log file for decompiler output."));
        }
        llvmPassesJsonTemp_.reset();
        decompilerLogTemp_.reset();
        return false;
    }
    decompilerLogTemp_->close();
    const QString logPath = decompilerLogTemp_->fileName();

    analysisRunning_ = true;
    analyseAction_->setEnabled(false);
    if (redecompileFunctionAct_)
        redecompileFunctionAct_->setEnabled(false);
    stopAction_->setEnabled(true);
    analysisBar_->setVisible(true);
    analysisBar_->setRange(0, 0);
    wallClock_.start();
    elapsedTimer_->start();
    decompileLogOffset_ = 0;
    decompileConsoleTailOffset_ = 0;
    if (batchRunning_)
        updateBatchStatusMessage(QStringLiteral("Decompiling…"));
    else if (!selectedFunctions.isEmpty())
        setStatusStage(QStringLiteral("Re-decompiling %1…").arg(selectedFunctions.first()));
    else
        setStatusStage(QStringLiteral("Decompiling (retdec-decompiler)…"));

    lastDecompilerExe_  = decExe;
    lastDecompilerArgs_ = args;
    lastDecompilerCwd_  = QFileInfo(decExe).absolutePath();

    if (liveConsole_) {
        liveConsole_->appendBanner(QStringLiteral("retdec-decompiler"), args, lastDecompilerCwd_);
        const bool streamLog = st.decompiler.liveConsoleTail && !quitWhenDecompileFinishes_;
        liveConsole_->appendLine(
                panels::LiveConsolePanel::Stream::Stdout,
                streamLog
                        ? QStringLiteral("(stdout/stderr redirected to %1 — streaming log to console)")
                                .arg(logPath)
                        : QStringLiteral("(stdout/stderr redirected to %1 — console updates when the run finishes)")
                                .arg(logPath));
        if (dockOutput_) focusOutputTab(kOutConsole);
    }

    decompilerProc_->setProgram(decExe);
    decompilerProc_->setArguments(args);
    decompilerProc_->setWorkingDirectory(lastDecompilerCwd_);
    decompilerProc_->setProcessChannelMode(QProcess::MergedChannels);
    decompilerProc_->setStandardOutputFile(logPath);
    decompilerProc_->start();
    if (!decompilerProc_->waitForStarted(5000)) {
        if (!quitWhenDecompileFinishes_) {
            QMessageBox::warning(this, QStringLiteral("Decompiler"),
                    QStringLiteral("Failed to start retdec-decompiler."));
        }
        decompilerProc_->setStandardOutputFile(QString());
        llvmPassesJsonTemp_.reset();
        decompilerLogTemp_.reset();
        finishExternalDecompileUi();
        if (quitWhenDecompileFinishes_) {
            writeHeadlessDecompileReport(wallClock_.elapsed(), -1, decompilerOutputPath_);
            QCoreApplication::quit();
        }
        return false;
    }

    if (!quitWhenDecompileFinishes_)
        decompileLogPollTimer_->start();
    return true;
}

void RetDecMainWindow::onBatchDecompile() {
    if (quitWhenDecompileFinishes_)
        return;
    if (batchRunning_) {
        statusBar()->showMessage(QStringLiteral("Batch decompile already running"), 2500);
        return;
    }
    if (decompilerProc_->state() != QProcess::NotRunning) {
        statusBar()->showMessage(QStringLiteral("Decompiler already running"), 2500);
        return;
    }

    BatchDecompileDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QStringList paths = dlg.binaryPaths();
    if (paths.isEmpty())
        return;

    startBatchDecompile(paths);
}

void RetDecMainWindow::startBatchDecompile(const QStringList& paths) {
    batchQueue_   = paths;
    batchTotal_   = paths.size();
    batchRunning_ = true;
    processNextBatchItem();
}

void RetDecMainWindow::processNextBatchItem() {
    if (!batchRunning_ || batchQueue_.isEmpty()) {
        finishBatchDecompile(false);
        return;
    }

    const QString absBinary = QFileInfo(batchQueue_.first()).absoluteFilePath();
    const QString arch =
            (project_ &&
             QFileInfo(project_->binaryPath()).absoluteFilePath() == absBinary)
                    ? project_->arch()
                    : QString();

    updateBatchStatusMessage(QStringLiteral("Starting…"));

    if (launchDecompilerForBinary(absBinary, arch))
        return;

    batchDecompilePopFront(&batchQueue_);
    if (batchRunning_ && !batchQueue_.isEmpty())
        QTimer::singleShot(0, this, &RetDecMainWindow::processNextBatchItem);
    else
        finishBatchDecompile(false);
}

void RetDecMainWindow::finishBatchDecompile(bool cancelled) {
    if (!batchRunning_ && batchQueue_.isEmpty() && batchTotal_ == 0)
        return;

    const int total = batchTotal_;
    batchQueue_.clear();
    batchTotal_   = 0;
    batchRunning_ = false;

    if (cancelled) {
        statusBar()->showMessage(QStringLiteral("Batch decompile cancelled"), 5000);
    } else if (total > 0) {
        statusBar()->showMessage(
                QStringLiteral("Batch decompile finished (%1 files)").arg(total), 8000);
    }
}

void RetDecMainWindow::updateBatchStatusMessage(const QString& stageSuffix) {
    if (!batchRunning_ || batchQueue_.isEmpty())
        return;
    const int index = batchDecompileCurrentIndex(batchTotal_, batchQueue_.size());
    QString msg = batchDecompileStatusLabel(index, batchTotal_, batchQueue_.first());
    if (!stageSuffix.isEmpty())
        msg += QStringLiteral(" — ") + stageSuffix;
    statusBar()->showMessage(msg);
}

void RetDecMainWindow::onRunStage() {
    if (!project_) {
        const QString path = QFileDialog::getOpenFileName(
                this, QStringLiteral("Open Binary"),
                QString(),
                QStringLiteral("Executable Files (*.exe *.dll *.elf *.so *.dylib *.bin);;All Files (*)"));
        if (path.isEmpty())
            return;
        openBinary(path);
    }
    if (!project_)
        return;
    const QStringList items = {
        QStringLiteral("Decompile (retdec-decompiler)"),
        QStringLiteral("fileinfo — binary format report"),
    };
    bool ok = false;
    const QString choice = QInputDialog::getItem(
            this, QStringLiteral("Run stage"),
            QStringLiteral("Tool to run on the loaded binary:"), items, 0, false,
            &ok);
    if (!ok) return;
    if (choice == items.at(0)) { onRunFullAnalysis(); return; }
    const QString fi = resolveRetdecFileinfoExecutable();
    if (fi.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("fileinfo"),
                QStringLiteral("retdec-fileinfo was not found next to the GUI or on PATH."));
        return;
    }
    inspect_->runFileinfo(project_->binaryPath(), fi);
    focusOutputTab(kOutConsole);
}

void RetDecMainWindow::onConfigure() {
    QString start = AppSettings::instance().decompiler.extraConfigPath;
    if (start.isEmpty() && project_)
        start = QFileInfo(project_->binaryPath()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Decompiler configuration JSON"), start,
            QStringLiteral("JSON (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    AppSettings::instance().decompiler.extraConfigPath = path;
    AppSettings::instance().save();
    AppSettings::instance().notifySettingsChanged();
    statusBar()->showMessage(QStringLiteral("Decompiler --config: %1").arg(path), 5000);
}

void RetDecMainWindow::applyEditorFontFromSettings() {
    const auto& g = AppSettings::instance().general;
    QFont font = g.editorFont;
    font.setPointSize(g.fontSize);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    if (decompiledC_) decompiledC_->applyEditorFont(font);
    if (assembly_)    assembly_->applyEditorFont(font);
    if (irPanel_)     irPanel_->applyEditorFont(font);
    if (triPane_)     triPane_->applyEditorFont(font);
}

void RetDecMainWindow::onSettings() {
    panels::SettingsDialog dlg(this);
    dlg.exec();
}

void RetDecMainWindow::onCompare() {
    const QString left = decompiledC_->documentText();
    if (left.trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Compare"),
                QStringLiteral("The Decompiled C view is empty. Run decompilation first."));
        return;
    }
    const QString rightPath = QFileDialog::getOpenFileName(
            this, QStringLiteral("Compare with file"), QString(),
            QStringLiteral("C/C++ sources (*.c *.cpp *.h *.hpp);;All files (*)"));
    if (rightPath.isEmpty()) return;
    QFile rf(rightPath);
    if (!rf.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Compare"), rf.errorString());
        return;
    }
    const QString right = QString::fromUtf8(rf.readAll());

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Compare — Decompiled C vs file"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* diff = new panels::DiffPanel(&dlg);
    diff->setDiff(left, right, QFileInfo(rightPath).fileName());
    lay->addWidget(diff, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(box);
    dlg.resize(1100, 700);
    dlg.exec();
}

void RetDecMainWindow::onOpenDocumentation() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList docNames = {
        QStringLiteral("WINDOWS_NATIVE_BUILD.md"),
        QStringLiteral("README.md"),
    };
    const QStringList relDirs = {
        QStringLiteral("../share/retdec/doc/"),
        QStringLiteral("../../share/retdec/doc/"),
        QStringLiteral("../../docs/"),
        QStringLiteral("../../../docs/"),
    };
    for (const QString& docName : docNames) {
        for (const QString& rel : relDirs) {
            const QString p = QDir(appDir).filePath(rel + docName);
            if (QFileInfo::exists(p)) {
                QDesktopServices::openUrl(
                        QUrl::fromLocalFile(QFileInfo(p).absoluteFilePath()));
                return;
            }
        }
    }
    QMessageBox::information(this, QStringLiteral("Documentation"),
            QStringLiteral("Could not find documentation under share/retdec/doc/ or docs/."));
}

void RetDecMainWindow::finishExternalDecompileUi() {
    decompileLogPollTimer_->stop();
    decompileLogOffset_ = 0;
    decompileConsoleTailOffset_ = 0;
    analysisRunning_ = false;
    analyseAction_->setEnabled(true);
    if (redecompileFunctionAct_)
        redecompileFunctionAct_->setEnabled(
                functionList_ && functionList_->selectedEntry().has_value());
    stopAction_->setEnabled(false);
    elapsedTimer_->stop();
    analysisBar_->setVisible(false);
    if (outputTabs_)
        outputTabs_->setTabVisible(kOutProgress, false);
    setStatusStage(QStringLiteral("Idle"));
}

void RetDecMainWindow::onDecompilerProcessFinished(int exitCode, QProcess::ExitStatus) {
    const qint64 decompileMs = wallClock_.elapsed();
    lastDecompileSubprocessMs_ = decompileMs;

    QString logPath;
    if (decompilerLogTemp_)
        logPath = decompilerLogTemp_->fileName();

    decompilerProc_->setStandardOutputFile(QString());

    if (liveConsole_) {
        if (!logPath.isEmpty()) {
            const bool streamed =
                    AppSettings::instance().decompiler.liveConsoleTail &&
                    !quitWhenDecompileFinishes_;
            if (streamed) {
                while (appendDecompilerLogIncrementalToConsole(
                        liveConsole_, logPath, &decompileConsoleTailOffset_))
                    ;
            } else {
                appendDecompilerLogToConsole(liveConsole_, logPath);
            }
        }
        liveConsole_->appendFooter(QStringLiteral("retdec-decompiler"), exitCode, decompileMs);
    }

    scanDecompilerLogDiagnostics(diagnostics_, logPath);

    if (commandLog_) {
        QString tail;
        if (!logPath.isEmpty()) {
            QFile f(logPath);
            if (f.open(QIODevice::ReadOnly)) {
                constexpr qint64 kTailBytes = 4000;
                const qint64 sz = f.size();
                if (sz > kTailBytes)
                    f.seek(sz - kTailBytes);
                tail = QString::fromUtf8(f.readAll());
                if (sz > kTailBytes)
                    tail = QStringLiteral("…\n") + tail;
            }
        }
        commandLog_->appendRun(QStringLiteral("retdec-decompiler"), lastDecompilerArgs_,
                               lastDecompilerCwd_, exitCode, decompileMs, tail);
    }

    llvmPassesJsonTemp_.reset();
    decompilerLogTemp_.reset();

    const bool wasBatch = batchRunning_;
    if (wasBatch)
        batchDecompilePopFront(&batchQueue_);

    if (exitCode == 0) {
        if (!quitWhenDecompileFinishes_ && !wasBatch) {
            const QString cPath = decompilerOutputPath_;
            const auto reselect = pendingReselectAddress_;
            pendingReselectAddress_.reset();
            QTimer::singleShot(0, this, [this, cPath, decompileMs, reselect]() {
                wallClock_.restart();
                const bool ok = loadDecompileArtifacts(cPath, reselect);
                const qint64 loadMs = wallClock_.elapsed();
                if (!ok) {
                    diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Warning,
                            QStringLiteral("retdec-decompiler"),
                            QStringLiteral("Finished but could not read: %1").arg(cPath));
                } else {
                    setStatusDecompileState(QStringLiteral("decompiled"));
                    statusBar()->showMessage(
                            QStringLiteral("Decompile %1 s · load %2 s")
                                    .arg(decompileMs / 1000.0, 0, 'f', 1)
                                    .arg(loadMs / 1000.0, 0, 'f', 1),
                            8000);
                }
            });
        }
    } else {
        pendingReselectAddress_.reset();
        diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Error,
                                 QStringLiteral("retdec-decompiler"),
                                 QStringLiteral("Exit code %1").arg(exitCode));
        if (!quitWhenDecompileFinishes_ && !wasBatch) {
            QMessageBox::warning(this, QStringLiteral("Decompiler"),
                    QStringLiteral("retdec-decompiler failed (exit %1). See Problems.")
                            .arg(exitCode));
        }
    }
    finishExternalDecompileUi();

    if (wasBatch && batchRunning_) {
        if (!batchQueue_.isEmpty()) {
            QTimer::singleShot(0, this, &RetDecMainWindow::processNextBatchItem);
            return;
        }
        finishBatchDecompile(false);
        return;
    }

    if (quitWhenDecompileFinishes_) {
        writeHeadlessDecompileReport(decompileMs, exitCode, decompilerOutputPath_,
                                     lastDecompilerArgs_);
        QCoreApplication::quit();
    }
}

void RetDecMainWindow::onDecompilerProcessError(QProcess::ProcessError) {
    decompileLogPollTimer_->stop();
    decompilerProc_->setStandardOutputFile(QString());
    llvmPassesJsonTemp_.reset();
    decompilerLogTemp_.reset();
    if (!analysisRunning_) return;
    const bool wasBatch = batchRunning_;
    if (wasBatch)
        batchDecompilePopFront(&batchQueue_);
    const QString err = decompilerProc_->errorString();
    diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Error,
                             QStringLiteral("retdec-decompiler"), err);
    if (commandLog_) {
        commandLog_->appendRun(QStringLiteral("retdec-decompiler"), lastDecompilerArgs_,
                               lastDecompilerCwd_, -1, wallClock_.elapsed(), err);
    }
    if (liveConsole_) {
        liveConsole_->appendLine(panels::LiveConsolePanel::Stream::Stderr, err);
    }
    if (!quitWhenDecompileFinishes_ && !wasBatch) {
        QMessageBox::warning(this, QStringLiteral("Decompiler"), err);
    } else if (quitWhenDecompileFinishes_) {
        writeHeadlessDecompileReport(wallClock_.elapsed(), -1, decompilerOutputPath_);
        QCoreApplication::quit();
    }
    finishExternalDecompileUi();

    if (wasBatch && batchRunning_) {
        if (!batchQueue_.isEmpty()) {
            QTimer::singleShot(0, this, &RetDecMainWindow::processNextBatchItem);
            return;
        }
        finishBatchDecompile(false);
    }
}

void RetDecMainWindow::onAbout() {
    QMessageBox::about(this, QStringLiteral("About RetDec"),
        QStringLiteral(
        "<h3>RetDec — Retargetable Decompiler</h3>"
        "<p>A machine-code decompiler producing readable C/C++ output with "
        "RTTI, exception handling, STL, and cryptographic primitive recovery.</p>"
        "<p><b>Layout (v3):</b> Functions dock (left), document tabs in the centre "
        "(Decompiled C, Assembly, IR, CFG, Synced), Workspace (Strings, Inspect, Binary) on "
        "the right, Output (Console, Problems, History, Progress) at the bottom.</p>"
        "<p>Centre tabs: <b>Ctrl+1</b>…<b>Ctrl+5</b>. "
        "Console: <b>Ctrl+`</b>; Problems: <b>Ctrl+Shift+`</b>.</p>"
        "<p><b>Ctrl+S</b> saves decompiled C; <b>Ctrl+Shift+S</b> saves the project. "
        "<b>F5</b> runs full analysis; <b>Ctrl+,</b> opens Settings.</p>"
        "<p>File → Batch Decompile queues multiple binaries. "
        "Tools → Call Graph / Type Hierarchy open in separate windows.</p>"
        "<p>No AI assistant panel in this build.</p>"
        "<p>Theme: Catppuccin Mocha &nbsp;|&nbsp; Qt6</p>"));
}

void RetDecMainWindow::onKeyboardShortcuts() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* browser = new QTextBrowser(&dlg);
    browser->setOpenExternalLinks(false);
    browser->setHtml(QStringLiteral(
            "<h3>File</h3><table cellpadding='4'>"
            "<tr><td><b>Ctrl+O</b></td><td>Open binary</td></tr>"
            "<tr><td><b>Ctrl+S</b></td><td>Save decompiled C</td></tr>"
            "<tr><td><b>Ctrl+Shift+S</b></td><td>Save project</td></tr>"
            "<tr><td><b>Ctrl+Q</b></td><td>Quit</td></tr>"
            "</table>"
            "<h3>Analysis</h3><table cellpadding='4'>"
            "<tr><td><b>F5</b></td><td>Run full analysis (decompile)</td></tr>"
            "<tr><td><b>F6</b></td><td>Stop analysis / batch</td></tr>"
            "</table>"
            "<h3>View</h3><table cellpadding='4'>"
            "<tr><td><b>Ctrl+1</b>…<b>Ctrl+5</b></td><td>Centre document tabs</td></tr>"
            "<tr><td><b>Ctrl+`</b></td><td>Show Console</td></tr>"
            "<tr><td><b>Ctrl+Shift+`</b></td><td>Show Problems</td></tr>"
            "<tr><td><b>Ctrl+,</b></td><td>Settings</td></tr>"
            "</table>"
            "<h3>Editor / panels</h3><table cellpadding='4'>"
            "<tr><td><b>G</b></td><td>Assembly: go to address (when panel focused)</td></tr>"
            "<tr><td><b>F</b></td><td>Assembly: find in disassembly</td></tr>"
            "<tr><td><b>Alt+← / Alt+→</b></td><td>Synced tab: navigation history</td></tr>"
            "</table>"));
    lay->addWidget(browser, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    lay->addWidget(box);
    dlg.resize(520, 480);
    dlg.exec();
}

void RetDecMainWindow::onAnalysisStageChanged(const QString& stage) {
    setStatusStage(stage);
    if (progressPanel_) progressPanel_->setStageState(stage, panels::StageState::Running);
}

void RetDecMainWindow::onDecompileLogPollTick() {
    if (!decompilerLogTemp_)
        return;

    const QString logPath = decompilerLogTemp_->fileName();

    DecompileLogProgress prog;
    if (pollDecompileLogProgress(logPath, &decompileLogOffset_, &prog)) {
        if (batchRunning_)
            updateBatchStatusMessage(prog.stage);
        else
            setStatusStage(prog.stage);
        setAnalysisProgress(prog.percent);
    }

    if (liveConsole_ && AppSettings::instance().decompiler.liveConsoleTail &&
        !quitWhenDecompileFinishes_) {
        appendDecompilerLogIncrementalToConsole(
                liveConsole_, logPath, &decompileConsoleTailOffset_);
    }
}

void RetDecMainWindow::onAnalysisFinished() {
    analysisRunning_ = false;
    analyseAction_->setEnabled(true);
    if (redecompileFunctionAct_)
        redecompileFunctionAct_->setEnabled(
                functionList_ && functionList_->selectedEntry().has_value());
    stopAction_->setEnabled(false);
    elapsedTimer_->stop();
    analysisBar_->setRange(0, 100);
    analysisBar_->setValue(100);
    analysisBar_->setTextVisible(true);
    if (outputTabs_)
        outputTabs_->setTabVisible(kOutProgress, false);
    setStatusStage(QStringLiteral("Analysis complete"));
}

void RetDecMainWindow::updateElapsedTime() {
    qint64 ms = wallClock_.elapsed();
    QString text;
    if (ms < 60000)
        text = QStringLiteral("%1.%2s").arg(ms / 1000).arg((ms % 1000) / 100);
    else
        text = QStringLiteral("%1m %2s").arg(ms / 60000).arg((ms % 60000) / 1000);
    statusElapsed_->setText(text);
}

// ─── Layout persistence ──────────────────────────────────────────────────────

void RetDecMainWindow::restoreLayout() {
    QSettings settings(QStringLiteral("retdec"), QStringLiteral("retdec-gui"));
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
    recentFiles_ = settings.value(QStringLiteral("recentFiles")).toStringList();
    updateRecentFilesMenu();
    const QString lps = settings.value(QStringLiteral("lastProjectSavePath")).toString();
    if (!lps.isEmpty() && QFileInfo::exists(lps))
        lastProjectSavePath_ = QFileInfo(lps).absoluteFilePath();
    else
        lastProjectSavePath_.clear();
    decompilePrintAfterAll_ = settings.value(QStringLiteral("decompilePrintAfterAll"), false).toBool();
    if (printAfterAllAct_) {
        printAfterAllAct_->blockSignals(true);
        printAfterAllAct_->setChecked(decompilePrintAfterAll_);
        printAfterAllAct_->blockSignals(false);
    }
    fastDecompile_ = settings.value(QStringLiteral("fastDecompile"), false).toBool();
    if (fastDecompileAct_) {
        fastDecompileAct_->blockSignals(true);
        fastDecompileAct_->setChecked(fastDecompile_);
        fastDecompileAct_->blockSignals(false);
    }
    const int docIdx = settings.value(QStringLiteral("preferredDocumentTabIndex"),
                                       kDocDecompiledC).toInt();
    raiseDocumentTab(docIdx);
}

void RetDecMainWindow::saveLayout() {
    QSettings settings(QStringLiteral("retdec"), QStringLiteral("retdec-gui"));
    settings.setValue(QStringLiteral("geometry"),    saveGeometry());
    settings.setValue(QStringLiteral("windowState"), saveState());
    settings.setValue(QStringLiteral("recentFiles"), recentFiles_);
    settings.setValue(QStringLiteral("lastProjectSavePath"), lastProjectSavePath_);
    settings.setValue(QStringLiteral("decompilePrintAfterAll"), decompilePrintAfterAll_);
    settings.setValue(QStringLiteral("fastDecompile"), fastDecompile_);
    if (documentTabs_)
        settings.setValue(QStringLiteral("preferredDocumentTabIndex"),
                          documentTabs_->currentIndex());
}

void RetDecMainWindow::addRecentFile(const QString& path) {
    recentFiles_.removeAll(path);
    recentFiles_.prepend(path);
    while (recentFiles_.size() > kMaxRecentFiles)
        recentFiles_.removeLast();
    updateRecentFilesMenu();
}

void RetDecMainWindow::updateRecentFilesMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    for (const auto& path : recentFiles_) {
        auto* act = recentMenu_->addAction(path);
        act->setData(path);
        connect(act, &QAction::triggered, this, &RetDecMainWindow::onRecentFileTriggered);
    }
    if (recentFiles_.isEmpty())
        recentMenu_->addAction(QStringLiteral("(empty)"))->setEnabled(false);
}

// ─── Drag and drop ───────────────────────────────────────────────────────────

void RetDecMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void RetDecMainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    for (const QUrl& u : urls) {
        const QString localPath = u.toLocalFile();
        if (localPath.isEmpty()) continue;
        const QFileInfo fi(localPath);
        if (!fi.exists() || !fi.isFile()) {
            statusBar()->showMessage(QStringLiteral("Skipped (not a file): %1").arg(localPath), 4000);
            continue;
        }
        if (localPath.endsWith(QStringLiteral(".retdec"), Qt::CaseInsensitive))
            openProject(localPath);
        else
            openBinary(localPath);
        event->acceptProposedAction();
        return;
    }
}

// ─── Close ───────────────────────────────────────────────────────────────────

void RetDecMainWindow::closeEvent(QCloseEvent* event) {
    if (analysisRunning_) {
        auto btn = QMessageBox::question(this, QStringLiteral("Quit"),
            QStringLiteral("A decompilation or analysis run is in progress. Quit anyway?"),
            QMessageBox::Yes | QMessageBox::No);
        if (btn != QMessageBox::Yes) { event->ignore(); return; }
        stopAnalysis();
    }
    if (project_ && project_->isModified()) {
        auto btn = QMessageBox::question(this, QStringLiteral("Unsaved Changes"),
            QStringLiteral("You have unsaved project changes. Save before quitting?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (btn == QMessageBox::Cancel) { event->ignore(); return; }
        if (btn == QMessageBox::Save) {
            QString path = lastProjectSavePath_;
            if (path.isEmpty()) {
                QString start;
                if (project_) {
                    const QFileInfo fi(project_->binaryPath());
                    start = QDir(fi.absolutePath()).filePath(
                        fi.completeBaseName() + QStringLiteral(".retdec"));
                }
                path = QFileDialog::getSaveFileName(
                    this, QStringLiteral("Save Project"), start,
                    QStringLiteral("RetDec Projects (*.retdec);;All Files (*)"));
                if (path.isEmpty()) { event->ignore(); return; }
                if (!path.endsWith(QStringLiteral(".retdec"), Qt::CaseInsensitive))
                    path += QStringLiteral(".retdec");
            }
            if (!saveProject(path)) { event->ignore(); return; }
            lastProjectSavePath_ = QFileInfo(path).absoluteFilePath();
        }
    }
    saveLayout();
    event->accept();
}

} // namespace gui
} // namespace retdec
