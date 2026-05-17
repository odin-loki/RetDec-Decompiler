/**
 * @file src/gui/mainwindow.cpp
 * @brief RetDecMainWindow — v3 simplified shell.
 *
 * See `assets/retdec_gui_redesign_v3.png` for the visual target.
 */

#include <memory>

#include "retdec/gui/mainwindow.h"
#include "retdec/gui/cli_tool_paths.h"
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
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QShortcut>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QWidget>

namespace retdec {
namespace gui {

// ─── Local helpers ───────────────────────────────────────────────────────────

namespace {

std::unique_ptr<QTemporaryFile> makeLlvmPassesJsonTempFile(
        const DecompilerSettings& d,
        bool fastPreset,
        QString* errOut) {
    // Fast preset drops duplicate generic LLVM passes (simplifycfg, instcombine,
    // …). On every test binary we've tried output is byte-identical and the
    // run is ~25 % faster; only enable on user opt-in to be safe with edge
    // cases we haven't sampled.
    const QString resource = fastPreset
            ? QStringLiteral(":/retdec/llvm_passes_fast.json")
            : QStringLiteral(":/retdec/llvm_passes_default.json");
    QFile f(resource);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut)
            *errOut = QStringLiteral("Missing resource %1").arg(resource);
        return nullptr;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isArray()) {
        if (errOut)
            *errOut = QStringLiteral("Invalid llvm_passes_default.json");
        return nullptr;
    }
    const QSet<QString> disabled(d.llvmPassesDisabled.begin(),
                                 d.llvmPassesDisabled.end());
    QJsonArray out;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isString())
            continue;
        const QString s = v.toString();
        if (!disabled.contains(s))
            out.append(s);
    }
    auto t = std::make_unique<QTemporaryFile>();
    t->setAutoRemove(true);
    if (!t->open()) {
        if (errOut)
            *errOut = QStringLiteral("Could not create temporary file for LLVM passes.");
        return nullptr;
    }
    const QJsonDocument outDoc(out);
    if (t->write(outDoc.toJson(QJsonDocument::Compact)) < 0) {
        if (errOut)
            *errOut = QStringLiteral("Could not write LLVM passes JSON.");
        return nullptr;
    }
    t->flush();
    return t;
}

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
    QString base = pf->binaryPath();
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) base = base.left(dot);
    return QFileInfo(base + QStringLiteral(".gui-decompiled.c")).absoluteFilePath();
}

// Document tab indices (Centre).
constexpr int kDocDecompiledC = 0;
constexpr int kDocAssembly    = 1;
constexpr int kDocIR          = 2;
constexpr int kDocCFG         = 3;
constexpr int kDocSynced      = 4;
constexpr int kDocCompare     = 5;

// Workspace dock (right) — slim two tabs.
constexpr int kWsStrings = 0;
constexpr int kWsInspect = 1;

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

    if (target_) {
        connect(&AppSettings::instance(), &AppSettings::settingsChanged,
                target_, &panels::TargetPanel::syncDecompilerConfigFromAppSettings);
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
    connect(diagnostics_,    &panels::PanelBase::addressNavigated,
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
    connect(triPane_,        &panels::TriPaneCodeView::addressNavigated,
            assembly_,        &panels::AssemblyPanel::onAddressNavigated);

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

    auto* centralHost = new QWidget(this);
    auto* centralLay  = new QVBoxLayout(centralHost);
    centralLay->setContentsMargins(6, 6, 6, 0);
    centralLay->setSpacing(6);
    centralLay->addWidget(triageBanner_);
    centralLay->addWidget(documentTabs_, 1);
    setCentralWidget(centralHost);

    // ── LEFT: Functions (single dock, no inner tabs) ──────────────────────────
    dockFunctions_ = makeDock(QStringLiteral("Functions"), functionList_);
    dockFunctions_->setObjectName(QStringLiteral("dock_functions"));

    // ── RIGHT: Strings + Inspect (two tabs only) ──────────────────────────────
    workspaceTabWidget_ = new QTabWidget();
    workspaceTabWidget_->setDocumentMode(true);
    workspaceTabWidget_->setMovable(true);
    workspaceTabWidget_->addTab(stringsBrowser_, QStringLiteral("Strings"));
    workspaceTabWidget_->addTab(inspect_,        QStringLiteral("Inspect"));
    dockWorkspace_ = makeDock(QStringLiteral("Workspace"), workspaceTabWidget_);
    dockWorkspace_->setObjectName(QStringLiteral("dock_workspace"));

    // ── BOTTOM: Console only ──────────────────────────────────────────────────
    // Previous Problems / Command log / Progress tabs were either always
    // empty (Progress was only fed by an in-process analysis bridge that we
    // no longer use) or duplicated info already in the Console. Collapsing
    // to a single live console removes empty-tab confusion and matches a
    // standard terminal pane in an IDE.
    outputTabs_ = nullptr;
    dockOutput_ = makeDock(QStringLiteral("Console"), liveConsole_);
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
    fileMenu_->addSeparator();
    auto* quitAct = fileMenu_->addAction(QStringLiteral("&Quit"));

    openBinaryAct->setShortcut(QKeySequence::Open);
    openProjectAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_O);
    saveDecompAct->setShortcut(QKeySequence::Save);
    quitAct->setShortcut(QKeySequence::Quit);

    connect(openBinaryAct,  &QAction::triggered, this, &RetDecMainWindow::onOpenBinary);
    connect(openProjectAct, &QAction::triggered, this, &RetDecMainWindow::onOpenProject);
    connect(saveProjectAct_, &QAction::triggered, this, &RetDecMainWindow::onSaveProject);
    connect(saveProjectAsAct_, &QAction::triggered, this, &RetDecMainWindow::onSaveProjectAs);
    connect(saveDecompAct,  &QAction::triggered, this, &RetDecMainWindow::onSaveDecompiled);
    connect(exportCmakeAct, &QAction::triggered, this, &RetDecMainWindow::onExportCMake);
    connect(quitAct,        &QAction::triggered, qApp, &QApplication::quit);

    openAction_ = openBinaryAct;

    // ── Analysis ──────────────────────────────────────────────────────────────
    analysisMenu_ = menuBar()->addMenu(QStringLiteral("&Analysis"));
    auto* runFullAct  = analysisMenu_->addAction(QStringLiteral("Run Full Analysis"));
    auto* runStageAct = analysisMenu_->addAction(QStringLiteral("Run Stage…"));
    analysisMenu_->addSeparator();
    auto* configAct = analysisMenu_->addAction(QStringLiteral("Configure…"));
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
    connect(runStageAct, &QAction::triggered, this, &RetDecMainWindow::onRunStage);
    connect(configAct,   &QAction::triggered, this, &RetDecMainWindow::onConfigure);
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
            if (dockOutput_) { dockOutput_->show(); dockOutput_->raise(); }
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

    // ── Tools ─────────────────────────────────────────────────────────────────
    toolsMenu_ = menuBar()->addMenu(QStringLiteral("&Tools"));
    auto* settingsAct    = toolsMenu_->addAction(QStringLiteral("&Settings…"));
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
    auto* aboutAct = helpMenu_->addAction(QStringLiteral("About RetDec…"));
    connect(docAct,   &QAction::triggered, this, &RetDecMainWindow::onOpenDocumentation);
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

void RetDecMainWindow::updateProjectFileActions() {
    const bool ok = static_cast<bool>(project_);
    if (saveProjectAct_)   saveProjectAct_->setEnabled(ok);
    if (saveProjectAsAct_) saveProjectAsAct_->setEnabled(ok);
}

void RetDecMainWindow::installCodeTabShortcuts() {
    for (int i = 0; i < 6; ++i) {
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
}

void RetDecMainWindow::createStatusBar() {
    statusFile_    = new QLabel(this);
    statusStage_   = new QLabel(this);
    statusFnCount_ = new QLabel(this);
    statusElapsed_ = new QLabel(this);
    analysisBar_   = new QProgressBar(this);
    analysisBar_->setFixedWidth(150);
    analysisBar_->setFixedHeight(14);
    analysisBar_->setVisible(false);
    analysisBar_->setTextVisible(false);

    statusBar()->addWidget(statusFile_,    2);
    statusBar()->addWidget(statusStage_,   2);
    statusBar()->addPermanentWidget(statusFnCount_);
    statusBar()->addPermanentWidget(analysisBar_);
    statusBar()->addPermanentWidget(statusElapsed_);

    setStatusFile(QStringLiteral("No binary loaded"));
    setStatusStage(QStringLiteral("Idle"));

    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(500);
    connect(elapsedTimer_, &QTimer::timeout,
            this, &RetDecMainWindow::updateElapsedTime);
}

void RetDecMainWindow::streamProcessToConsole(QProcess* proc, const QString& label) {
    if (!proc || !liveConsole_) return;
    liveConsole_->attachProcess(proc, label);
}

// ─── Decompile-artifact loader (used by both cache reuse + post-run) ─────────

bool RetDecMainWindow::loadDecompileArtifacts(const QString& cPath) {
    QFile f(cPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray cBytes = f.readAll();
    f.close();

    statusBar()->showMessage(
            QStringLiteral("Loading decompiled C (%1 KiB)…")
                    .arg(cBytes.size() / 1024), 1500);
    decompiledC_->setSource(QString::fromUtf8(cBytes));
    if (project_)
        project_->setDecompiledPath(cPath);
    diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Info,
            QStringLiteral("retdec-decompiler"),
            QStringLiteral("Output: %1 (%2 KiB)").arg(cPath).arg(cBytes.size() / 1024));
    if (documentTabs_)
        documentTabs_->setCurrentIndex(kDocDecompiledC);

    // Populate Functions panel from the .config.json sidecar.
    const QString cfgPath =
            cPath.endsWith(QStringLiteral(".c"), Qt::CaseInsensitive)
                    ? cPath.left(cPath.size() - 2) + QStringLiteral(".config.json")
                    : cPath + QStringLiteral(".config.json");
    QFile cfgFile(cfgPath);
    if (cfgFile.open(QIODevice::ReadOnly)) {
        const QByteArray cfgBytes = cfgFile.readAll();
        cfgFile.close();
        QJsonParseError pe{};
        const QJsonDocument cfgDoc = QJsonDocument::fromJson(cfgBytes, &pe);
        if (cfgDoc.isObject()) {
            const QJsonArray fnArr =
                    cfgDoc.object().value(QStringLiteral("functions")).toArray();
            std::vector<panels::FunctionEntry> entries;
            entries.reserve(static_cast<size_t>(fnArr.size()));
            for (const QJsonValue& v : fnArr) {
                if (!v.isObject()) continue;
                const QJsonObject o = v.toObject();
                panels::FunctionEntry e;
                e.name    = o.value(QStringLiteral("name")).toString();
                e.rawName = o.value(QStringLiteral("demangledName")).toString();
                const QString sa = o.value(QStringLiteral("startAddr")).toString();
                const QString ea = o.value(QStringLiteral("endAddr")).toString();
                bool ok = false;
                e.address = sa.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
                        ? sa.mid(2).toULongLong(&ok, 16)
                        : sa.toULongLong(&ok, 10);
                if (!ok) e.address = 0;
                uint64_t end = ea.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
                        ? ea.mid(2).toULongLong(&ok, 16)
                        : ea.toULongLong(&ok, 10);
                if (ok && end > e.address)
                    e.sizeBytes = static_cast<int>(end - e.address);
                e.cc = o.value(QStringLiteral("callingConvention")).toString();
                const QString fncType = o.value(QStringLiteral("fncType")).toString();
                e.isLibrary = (fncType == QStringLiteral("staticallyLinked")
                            || fncType == QStringLiteral("dynamicallyLinked"));
                entries.push_back(std::move(e));
            }
            if (functionList_)
                functionList_->setFunctions(std::move(entries));
            setStatusFunctionCount(static_cast<int>(fnArr.size()));
        }
    }
    return true;
}

bool RetDecMainWindow::tryLoadCachedDecompile(const QString& binaryPath) {
    if (binaryPath.isEmpty())
        return false;
    QString base = binaryPath;
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) base = base.left(dot);
    const QString cPath = base + QStringLiteral(".gui-decompiled.c");
    const QFileInfo cFi(cPath);
    const QFileInfo bFi(binaryPath);
    if (!cFi.exists() || !bFi.exists())
        return false;
    // Cache is fresh iff the .c is at least as new as the binary AND the
    // sidecar .config.json exists too (needed for the Functions panel).
    if (cFi.lastModified() < bFi.lastModified())
        return false;
    const QString cfgPath = base + QStringLiteral(".gui-decompiled.config.json");
    if (!QFileInfo::exists(cfgPath))
        return false;
    if (!loadDecompileArtifacts(cPath))
        return false;
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
    setWindowTitle(QStringLiteral("RetDec — %1").arg(path));
    project_ = std::make_unique<ProjectFile>(path);
    lastProjectSavePath_.clear();
    setStatusFile(path);
    addRecentFile(path);
    binaryBrowser_->loadBinary(path);
    target_->setFromProject(project_.get());
    signatureStudio_->setActiveBinary(path);
    diagnostics_->clear();
    commandLog_->clear();
    liveConsole_->clear();
    progressPanel_->resetAll();
    if (triageBanner_) {
        triageBanner_->setBinary(path);
        triageBanner_->setActionsEnabled(true);
    }
    updateProjectFileActions();
    // Cache reuse: a 1 MB binary takes ~50 s to decompile from scratch on a
    // workstation. Re-opening the same binary in a session must not pay
    // that again — if a fresh `.gui-decompiled.c` sits next to it, load
    // instantly and show a "press F5 to re-decompile" hint.
    const bool cacheHit = tryLoadCachedDecompile(path);
    // Skip the standalone retdec-fileinfo run when we have cached output —
    // the config.json sidecar already contains the same metadata that
    // fileinfo would produce. Saves ~1-2 s on every cached re-open.
    if (!cacheHit)
        inspect_->runFileinfo(path, resolveRetdecFileinfoExecutable());
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
    setWindowTitle(QStringLiteral("RetDec — %1").arg(project_->binaryPath()));
    setStatusFile(project_->binaryPath());
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
    if (!tryLoadCachedDecompile(project_->binaryPath()))
        inspect_->runFileinfo(project_->binaryPath(), resolveRetdecFileinfoExecutable());
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
    analysisBar_->setRange(0, 0);
    wallClock_.start();
    elapsedTimer_->start();
    progressPanel_->resetAll();
    setStatusStage(QStringLiteral("Running…"));
}

void RetDecMainWindow::stopAnalysis() {
    if (decompilerProc_ && decompilerProc_->state() != QProcess::NotRunning) {
        decompilerProc_->terminate();
        if (!decompilerProc_->waitForFinished(2000))
            decompilerProc_->kill();
        decompilerProc_->waitForFinished(1000);
    }
    llvmPassesJsonTemp_.reset();
    if (!analysisRunning_) return;
    analysisRunning_ = false;
    analyseAction_->setEnabled(true);
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
    statusFnCount_->setText(QStringLiteral("%1 functions").arg(count));
}
void RetDecMainWindow::setAnalysisProgress(int percent) {
    analysisBar_->setRange(0, 100);
    analysisBar_->setValue(percent);
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
        onOpenBinary();
        return;
    }
    if (decompilerProc_->state() != QProcess::NotRunning) {
        statusBar()->showMessage(QStringLiteral("Decompiler already running"), 2500);
        return;
    }

    const QString decExe = resolveRetdecDecompilerExecutablePath();
    if (decExe.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Decompiler"),
                QStringLiteral("retdec-decompiler was not found next to the GUI or on PATH."));
        return;
    }

    QString base = project_->binaryPath();
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) base = base.left(dot);
    decompilerOutputPath_ = base + QStringLiteral(".gui-decompiled.c");

    QStringList args;
    auto& st = AppSettings::instance();
    if (!st.decompiler.extraConfigPath.isEmpty()) {
        if (QFileInfo::exists(st.decompiler.extraConfigPath)) {
            args << QStringLiteral("--config") << st.decompiler.extraConfigPath;
        } else {
            diagnostics_->addMessage(
                    panels::DiagnosticEntry::Severity::Warning,
                    QStringLiteral("retdec-decompiler"),
                    QStringLiteral("Configured --config file missing: %1")
                            .arg(st.decompiler.extraConfigPath));
        }
    }

    args << project_->binaryPath()
         << QStringLiteral("-o") << decompilerOutputPath_
         << QStringLiteral("-f") << QStringLiteral("plain")
         // --silent suppresses chatty informative output. Doesn't measurably
         // change runtime (we measured <1% on a 1 MB binary) but reduces
         // pipe traffic, which matters when the user has the Console docked.
         << QStringLiteral("-s");
    {
        const QString arch = project_->arch().trimmed();
        if (!arch.isEmpty()) args << QStringLiteral("-a") << arch;
    }
    if (fastDecompile_) {
        // Fast preview mode: skips backend (HLL) optimizations and the
        // statically-linked-code detector. Output reads worse but typical
        // wall time drops markedly because the HLL optimizer is a heavy
        // O(functions × passes) loop.
        args << QStringLiteral("--backend-no-opts")
             << QStringLiteral("--disable-static-code-detection");
    }
    if (decompilePrintAfterAll_) args << QStringLiteral("--print-after-all");

    llvmPassesJsonTemp_.reset();
    // Use a custom passes list whenever the user is using either the
    // settings-driven custom set OR Fast mode (which selects the trimmed
    // built-in preset).
    if (st.decompiler.useCustomLlvmPasses || fastDecompile_) {
        QString err;
        llvmPassesJsonTemp_ = makeLlvmPassesJsonTempFile(
                st.decompiler, fastDecompile_, &err);
        if (!llvmPassesJsonTemp_) {
            QMessageBox::warning(this, QStringLiteral("Decompiler"), err);
            return;
        }
        llvmPassesJsonTemp_->flush();
        llvmPassesJsonTemp_->close();
        args << QStringLiteral("--llvm-passes-json")
             << llvmPassesJsonTemp_->fileName();
    }

    analysisRunning_ = true;
    analyseAction_->setEnabled(false);
    stopAction_->setEnabled(true);
    analysisBar_->setVisible(true);
    analysisBar_->setRange(0, 0);
    wallClock_.start();
    elapsedTimer_->start();
    setStatusStage(QStringLiteral("Decompiling (retdec-decompiler)…"));

    lastDecompilerExe_  = decExe;
    lastDecompilerArgs_ = args;
    lastDecompilerCwd_  = QFileInfo(decExe).absolutePath();

    if (liveConsole_) {
        liveConsole_->appendBanner(QStringLiteral("retdec-decompiler"), args, lastDecompilerCwd_);
        if (dockOutput_) { dockOutput_->show(); dockOutput_->raise(); }
        streamProcessToConsole(decompilerProc_, QStringLiteral("retdec-decompiler"));
    }

    decompilerProc_->setProgram(decExe);
    decompilerProc_->setArguments(args);
    decompilerProc_->setWorkingDirectory(lastDecompilerCwd_);
    decompilerProc_->setProcessChannelMode(QProcess::MergedChannels);
    decompilerProc_->start();
    if (!decompilerProc_->waitForStarted(5000)) {
        QMessageBox::warning(this, QStringLiteral("Decompiler"),
                QStringLiteral("Failed to start retdec-decompiler."));
        llvmPassesJsonTemp_.reset();
        if (liveConsole_) liveConsole_->detachProcess(decompilerProc_);
        finishExternalDecompileUi();
    }
}

void RetDecMainWindow::onRunStage() {
    if (!project_) { onOpenBinary(); return; }
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
    if (dockOutput_) { dockOutput_->show(); dockOutput_->raise(); }
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

void RetDecMainWindow::syncAiAssistantFromAppSettings() {
    // AI assistant intentionally removed from v3. Slot kept as a stub for
    // ABI compatibility with the signal/slot wiring; no-op here.
}

void RetDecMainWindow::onSettings() {
    panels::SettingsDialog dlg(this);
    dlg.exec();
}

void RetDecMainWindow::onMLAssistant() {
    statusBar()->showMessage(
            QStringLiteral("AI Assistant has been removed from this build."), 4000);
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
    analysisRunning_ = false;
    analyseAction_->setEnabled(true);
    stopAction_->setEnabled(false);
    elapsedTimer_->stop();
    analysisBar_->setVisible(false);
    setStatusStage(QStringLiteral("Idle"));
}

void RetDecMainWindow::onDecompilerProcessFinished(int exitCode, QProcess::ExitStatus) {
    llvmPassesJsonTemp_.reset();
    // Drain the tail. LiveConsolePanel also drains in its own finished slot,
    // but our slot may fire first (depending on connection order); reading
    // here is safe — QProcess marks the buffers as consumed on first read.
    const QByteArray stdoutTail = decompilerProc_->readAllStandardOutput();
    const QByteArray stderrTail = decompilerProc_->readAllStandardError();
    if (liveConsole_) {
        if (!stdoutTail.isEmpty())
            liveConsole_->appendChunk(panels::LiveConsolePanel::Stream::Stdout, stdoutTail);
        if (!stderrTail.isEmpty())
            liveConsole_->appendChunk(panels::LiveConsolePanel::Stream::Stderr, stderrTail);
    }

    // Performance fix: do NOT dump the entire decompiler output into a single
    // Diagnostic row — for a multi-MB log this used to stutter the table for
    // several seconds when the Problems tab was raised. Instead, scan the
    // tail (and any leftover stderr) for warning/error lines and add one
    // diagnostic per match; the full output stays available in the Console.
    auto addLineDiagnostics = [this](const QByteArray& blob,
                                     panels::DiagnosticEntry::Severity defaultSev) {
        if (blob.isEmpty())
            return;
        const QString text = QString::fromUtf8(blob);
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        // Cap how many we surface so a runaway pass can't flood Problems either.
        int added = 0;
        for (const QString& l : lines) {
            const QString trimmed = l.trimmed();
            if (trimmed.isEmpty()) continue;
            auto sev = defaultSev;
            if (trimmed.contains(QStringLiteral("error"), Qt::CaseInsensitive) ||
                trimmed.startsWith(QStringLiteral("[error]"), Qt::CaseInsensitive)) {
                sev = panels::DiagnosticEntry::Severity::Error;
            } else if (trimmed.contains(QStringLiteral("warning"), Qt::CaseInsensitive) ||
                       trimmed.startsWith(QStringLiteral("[warn]"), Qt::CaseInsensitive)) {
                sev = panels::DiagnosticEntry::Severity::Warning;
            } else if (defaultSev == panels::DiagnosticEntry::Severity::Info) {
                // Skip routine info lines — they're already in the Console.
                continue;
            }
            diagnostics_->addMessage(sev, QStringLiteral("retdec-decompiler"), trimmed);
            if (++added >= 200)
                break;
        }
    };
    addLineDiagnostics(stderrTail, panels::DiagnosticEntry::Severity::Warning);
    addLineDiagnostics(stdoutTail, panels::DiagnosticEntry::Severity::Info);

    if (commandLog_) {
        QString tail = QString::fromUtf8(stdoutTail + stderrTail);
        if (tail.size() > 4000) tail = tail.left(4000) + QStringLiteral("\n…");
        commandLog_->appendRun(QStringLiteral("retdec-decompiler"), lastDecompilerArgs_,
                               lastDecompilerCwd_, exitCode, wallClock_.elapsed(), tail);
    }
    if (liveConsole_) {
        liveConsole_->appendFooter(QStringLiteral("retdec-decompiler"),
                                   exitCode, wallClock_.elapsed());
        liveConsole_->detachProcess(decompilerProc_);
    }
    if (exitCode == 0) {
        if (!loadDecompileArtifacts(decompilerOutputPath_)) {
            diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Warning,
                    QStringLiteral("retdec-decompiler"),
                    QStringLiteral("Finished but could not read: %1")
                            .arg(decompilerOutputPath_));
        }
    } else {
        diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Error,
                                 QStringLiteral("retdec-decompiler"),
                                 QStringLiteral("Exit code %1").arg(exitCode));
        QMessageBox::warning(this, QStringLiteral("Decompiler"),
                QStringLiteral("retdec-decompiler failed (exit %1). See Problems.")
                        .arg(exitCode));
    }
    finishExternalDecompileUi();
}

void RetDecMainWindow::onDecompilerProcessError(QProcess::ProcessError) {
    llvmPassesJsonTemp_.reset();
    if (!analysisRunning_) return;
    const QString err = decompilerProc_->errorString();
    diagnostics_->addMessage(panels::DiagnosticEntry::Severity::Error,
                             QStringLiteral("retdec-decompiler"), err);
    if (commandLog_) {
        commandLog_->appendRun(QStringLiteral("retdec-decompiler"), lastDecompilerArgs_,
                               lastDecompilerCwd_, -1, wallClock_.elapsed(), err);
    }
    if (liveConsole_) {
        liveConsole_->appendLine(panels::LiveConsolePanel::Stream::Stderr, err);
        liveConsole_->detachProcess(decompilerProc_);
    }
    QMessageBox::warning(this, QStringLiteral("Decompiler"), err);
    finishExternalDecompileUi();
}

void RetDecMainWindow::onAbout() {
    QMessageBox::about(this, QStringLiteral("About RetDec"),
        QStringLiteral(
        "<h3>RetDec — Retargetable Decompiler</h3>"
        "<p>A machine-code decompiler producing readable C/C++ output with "
        "full RTTI, exception handling, STL, and cryptographic primitive "
        "recovery.</p>"
        "<p>Centre tabs: <b>Ctrl+1</b>…<b>Ctrl+5</b> "
        "(Decompiled C, Assembly, IR, CFG, Synced).</p>"
        "<p>Toggle the live console with <b>Ctrl+`</b>.</p>"
        "<p>Theme: Catppuccin Mocha &nbsp;|&nbsp; Qt6</p>"));
}

void RetDecMainWindow::onAnalysisStageChanged(const QString& stage) {
    setStatusStage(stage);
    if (progressPanel_) progressPanel_->setStageState(stage, panels::StageState::Running);
}

void RetDecMainWindow::onAnalysisFinished() {
    analysisRunning_ = false;
    analyseAction_->setEnabled(true);
    stopAction_->setEnabled(false);
    elapsedTimer_->stop();
    analysisBar_->setRange(0, 100);
    analysisBar_->setValue(100);
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
