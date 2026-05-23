/**
 * @file src/gui/panels/settings_dialog.cpp
 * @brief SettingsDialog implementation.
 */

#include "retdec/gui/panels/settings_dialog.h"
#include "retdec/gui/settings/plugin_manager.h"
#include "retdec/gui/settings/settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFontComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace retdec::gui::panels {

// ─── Helpers ─────────────────────────────────────────────────────────────────

QWidget* SettingsDialog::makeRow(const QString& label, QWidget* widget,
                                  const QString& tooltip) {
    auto* row = new QWidget;
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* lbl = new QLabel(label + ":", row);
    lbl->setMinimumWidth(180);
    if (!tooltip.isEmpty()) {
        lbl->setToolTip(tooltip);
        if (widget) widget->setToolTip(tooltip);
    }
    lay->addWidget(lbl);
    if (widget) lay->addWidget(widget, 1);
    return row;
}

QWidget* SettingsDialog::makeSeparator(const QString& title) {
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 8, 0, 4);
    auto* lbl = new QLabel("<b>" + title + "</b>", w);
    lbl->setStyleSheet("color:#89b4fa;");
    auto* line = new QFrame(w);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("color:#313244;");
    l->addWidget(lbl);
    l->addWidget(line, 1);
    return w;
}

static QWidget* makeScrollArea(QWidget* inner) {
    auto* scroll = new QScrollArea;
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

// ─── Constructor ─────────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("RetDec Settings");
    setMinimumSize(720, 540);
    resize(760, 580);

    tabWidget_ = new QTabWidget(this);
    tabWidget_->addTab(makeScrollArea(buildGeneralTab()),  "General");
    tabWidget_->addTab(makeScrollArea(buildAnalysisTab()), "Analysis");
    tabWidget_->addTab(makeScrollArea(buildCUDATab()),     "CUDA");
    tabWidget_->addTab(makeScrollArea(buildMLTab()),       "ML");
    tabWidget_->addTab(makeScrollArea(buildRecoveryTab()), "Recovery");
    tabWidget_->addTab(makeScrollArea(buildAdvancedTab()), "Advanced");
    tabWidget_->addTab(makeScrollArea(buildDecompilerTab()), "Decompiler");
    tabWidget_->addTab(buildPluginsTab(),                  "Plugins");

    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
        QDialogButtonBox::Apply | QDialogButtonBox::RestoreDefaults,
        this);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(tabWidget_);
    mainLayout->addWidget(buttonBox_);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &SettingsDialog::onOK);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttonBox_->button(QDialogButtonBox::Apply),
            &QPushButton::clicked, this, &SettingsDialog::onApply);
    connect(buttonBox_->button(QDialogButtonBox::RestoreDefaults),
            &QPushButton::clicked, this, &SettingsDialog::onReset);

    populateFromSettings();
}

// ─── Tab: General ─────────────────────────────────────────────────────────────

QWidget* SettingsDialog::buildGeneralTab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    l->setSpacing(6);

    l->addWidget(makeSeparator("Appearance"));

    themeCombo_ = new QComboBox;
    themeCombo_->addItems({"Dark", "Light", "System Default"});
    l->addWidget(makeRow("Theme", themeCombo_));

    fontCombo_ = new QFontComboBox;
    l->addWidget(makeRow("Editor font", fontCombo_));

    fontSizeSpin_ = new QSpinBox;
    fontSizeSpin_->setRange(6, 32);
    l->addWidget(makeRow("Font size (pt)", fontSizeSpin_));

    l->addWidget(makeSeparator("Internationalisation"));

    langCombo_ = new QComboBox;
    langCombo_->addItems({"English (en)", "German (de)", "French (fr)",
                          "Spanish (es)", "Chinese (zh)"});
    l->addWidget(makeRow("Language", langCombo_, "Requires restart"));

    l->addWidget(makeSeparator("Editor"));

    lineNumCheck_ = new QCheckBox("Show line numbers");
    wordWrapCheck_= new QCheckBox("Word wrap");
    l->addWidget(lineNumCheck_);
    l->addWidget(wordWrapCheck_);

    l->addWidget(makeSeparator("Session"));

    restoreCheck_ = new QCheckBox("Restore last session on startup");
    l->addWidget(restoreCheck_);

    l->addStretch();

    connect(fontCombo_, &QFontComboBox::currentFontChanged,
            this, &SettingsDialog::onFontChanged);
    connect(themeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onThemeChanged);

    return w;
}

// ─── Tab: Analysis ────────────────────────────────────────────────────────────

QWidget* SettingsDialog::buildAnalysisTab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    l->setSpacing(6);

    l->addWidget(makeSeparator("Pipeline Stages"));

    enableTypingCheck_  = new QCheckBox("Type inference");
    enablePatternCheck_ = new QCheckBox("Pattern matching");
    enableConcurCheck_  = new QCheckBox("Concurrency detection");
    enableCudaCheck_    = new QCheckBox("CUDA host recovery");
    enableSerialCheck_  = new QCheckBox("Serialisation detection");
    enableModuleCheck_  = new QCheckBox("Module clustering");
    enableCxxCheck_     = new QCheckBox("C++ lifter");
    for (auto* c : {enableTypingCheck_, enablePatternCheck_,
                    enableConcurCheck_, enableCudaCheck_,
                    enableSerialCheck_, enableModuleCheck_, enableCxxCheck_})
        l->addWidget(c);

    l->addWidget(makeSeparator("Confidence Thresholds"));

    typeConfSpin_ = new QDoubleSpinBox;
    typeConfSpin_->setRange(0.0, 1.0); typeConfSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("Type inference minimum", typeConfSpin_));

    patternConfSpin_ = new QDoubleSpinBox;
    patternConfSpin_->setRange(0.0, 1.0); patternConfSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("Pattern matching minimum", patternConfSpin_));

    recovConfSpin_ = new QDoubleSpinBox;
    recovConfSpin_->setRange(0.0, 1.0); recovConfSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("Recovery minimum", recovConfSpin_));

    l->addWidget(makeSeparator("Resources"));

    maxTimeSpin_ = new QSpinBox;
    maxTimeSpin_->setRange(0, 3600); maxTimeSpin_->setSuffix(" s");
    maxTimeSpin_->setSpecialValueText("Unlimited");
    l->addWidget(makeRow("Max analysis time", maxTimeSpin_));

    threadCountSpin_ = new QSpinBox;
    threadCountSpin_->setRange(0, 256);
    threadCountSpin_->setSpecialValueText("Auto");
    l->addWidget(makeRow("Thread count", threadCountSpin_, "0 = hardware_concurrency"));

    l->addStretch();
    return w;
}

// ─── Tab: CUDA ───────────────────────────────────────────────────────────────

QWidget* SettingsDialog::buildCUDATab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    l->setSpacing(6);

    l->addWidget(makeSeparator("Device"));

    clDeviceCombo_ = new QComboBox;
    clDeviceCombo_->addItem("Auto (default)");
    l->addWidget(makeRow("CUDA device", clDeviceCombo_));

    useGPUCheck_ = new QCheckBox("Prefer GPU over CPU");
    l->addWidget(useGPUCheck_);

    wgSizeSpin_ = new QSpinBox;
    wgSizeSpin_->setRange(32, 1024); wgSizeSpin_->setSingleStep(32);
    l->addWidget(makeRow("Work group size", wgSizeSpin_));

    l->addWidget(makeSeparator("Kernel Cache"));

    {
        auto* row = new QWidget;
        auto* rl  = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("Kernel cache dir:", row);
        lbl->setMinimumWidth(180);
        kernelCacheEdit_ = new QLineEdit(row);
        kernelCacheBtn_  = new QToolButton(row);
        kernelCacheBtn_->setText("…");
        rl->addWidget(lbl);
        rl->addWidget(kernelCacheEdit_, 1);
        rl->addWidget(kernelCacheBtn_);
        l->addWidget(row);
    }

    l->addWidget(makeSeparator("Profiling"));
    clProfilingCheck_ = new QCheckBox("Enable cl_event profiling");
    l->addWidget(clProfilingCheck_);

    l->addStretch();

    connect(kernelCacheBtn_, &QToolButton::clicked, this, &SettingsDialog::onBrowseKernelCache);
    return w;
}

// ─── Tab: ML ─────────────────────────────────────────────────────────────────

QWidget* SettingsDialog::buildMLTab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    l->setSpacing(6);

    l->addWidget(makeSeparator("Model"));

    {
        auto* row = new QWidget;
        auto* rl  = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("Model file (.gguf):", row);
        lbl->setMinimumWidth(180);
        modelPathEdit_ = new QLineEdit(row);
        modelPathBtn_  = new QToolButton(row);
        modelPathBtn_->setText("…");
        rl->addWidget(lbl);
        rl->addWidget(modelPathEdit_, 1);
        rl->addWidget(modelPathBtn_);
        l->addWidget(row);
    }

    {
        auto* hint = new QLabel(
            QStringLiteral(
                "Qwen3-compatible GGUF (including Qwen2.5-Coder / “Qwen Code”–style "
                "checkpoints). After Apply or OK, this path is pushed to the AI Assistant "
                "and the model loads if the file exists."),
            w);
        hint->setWordWrap(true);
        hint->setForegroundRole(QPalette::PlaceholderText);
        auto hintFont = hint->font();
        hintFont.setPointSize(qMax(9, hintFont.pointSize() - 1));
        hint->setFont(hintFont);
        l->addWidget(hint);
    }

    quantCombo_ = new QComboBox;
    quantCombo_->addItems({"Q4_0","Q4_K_M","Q5_K_M","Q6_K","F16","F32"});
    l->addWidget(makeRow("Quantisation level", quantCombo_));

    inferDevCombo_ = new QComboBox;
    inferDevCombo_->addItems({"CPU","GPU","Auto"});
    l->addWidget(makeRow("Inference device", inferDevCombo_));

    l->addWidget(makeSeparator("Generation Parameters"));

    tempSpin_ = new QDoubleSpinBox;
    tempSpin_->setRange(0.0, 2.0); tempSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("Temperature", tempSpin_));

    topPSpin_ = new QDoubleSpinBox;
    topPSpin_->setRange(0.0, 1.0); topPSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("Top-P", topPSpin_));

    topKSpin_ = new QSpinBox;
    topKSpin_->setRange(1, 200);
    l->addWidget(makeRow("Top-K", topKSpin_));

    maxTokensSpin_ = new QSpinBox;
    maxTokensSpin_->setRange(64, 8192);
    l->addWidget(makeRow("Max new tokens", maxTokensSpin_));

    contextLenSpin_ = new QSpinBox;
    contextLenSpin_->setRange(512, 131072);
    l->addWidget(makeRow("Context length", contextLenSpin_));

    streamCheck_ = new QCheckBox("Stream output tokens");
    l->addWidget(streamCheck_);

    l->addStretch();

    connect(modelPathBtn_, &QToolButton::clicked, this, &SettingsDialog::onBrowseModelPath);
    return w;
}

// ─── Tab: Recovery ───────────────────────────────────────────────────────────

QWidget* SettingsDialog::buildRecoveryTab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    l->setSpacing(6);

    l->addWidget(makeSeparator("Enabled Detectors"));

    detectSTLCheck_    = new QCheckBox("STL containers / algorithms");
    detectCryptoCheck_ = new QCheckBox("Cryptographic primitives");
    detectPatternCheck_= new QCheckBox("Code patterns");
    detectConcurCheck_ = new QCheckBox("Concurrency primitives");
    detectCudaCheck_   = new QCheckBox("CUDA host API");
    detectRTTICheck_   = new QCheckBox("C++ RTTI / typeinfo");
    detectEHCheck_     = new QCheckBox("Exception handling");
    detectVirtCheck_   = new QCheckBox("Virtual dispatch / vtables");
    for (auto* c : {detectSTLCheck_, detectCryptoCheck_, detectPatternCheck_,
                    detectConcurCheck_, detectCudaCheck_,
                    detectRTTICheck_, detectEHCheck_, detectVirtCheck_})
        l->addWidget(c);

    l->addWidget(makeSeparator("Confidence Thresholds"));

    stlConfSpin_ = new QDoubleSpinBox;
    stlConfSpin_->setRange(0.0, 1.0); stlConfSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("STL confidence", stlConfSpin_));

    cryptoConfSpin_ = new QDoubleSpinBox;
    cryptoConfSpin_->setRange(0.0, 1.0); cryptoConfSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("Crypto confidence", cryptoConfSpin_));

    patConf2Spin_ = new QDoubleSpinBox;
    patConf2Spin_->setRange(0.0, 1.0); patConf2Spin_->setSingleStep(0.05);
    l->addWidget(makeRow("Pattern confidence", patConf2Spin_));

    concurConfSpin_ = new QDoubleSpinBox;
    concurConfSpin_->setRange(0.0, 1.0); concurConfSpin_->setSingleStep(0.05);
    l->addWidget(makeRow("Concurrency confidence", concurConfSpin_));

    l->addStretch();
    return w;
}

// ─── Tab: Advanced ───────────────────────────────────────────────────────────

QWidget* SettingsDialog::buildAdvancedTab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    l->setSpacing(6);

    l->addWidget(makeSeparator("Diagnostics"));

    verbCombo_ = new QComboBox;
    verbCombo_->addItems({"Quiet","Normal","Verbose","Debug"});
    l->addWidget(makeRow("Verbosity", verbCombo_));

    l->addWidget(makeSeparator("Dump Options"));

    dumpIRCheck_  = new QCheckBox("Dump SSA IR");
    dumpASMCheck_ = new QCheckBox("Dump assembly");
    dumpCFGCheck_ = new QCheckBox("Dump CFG (DOT)");
    dumpSSACheck_ = new QCheckBox("Dump SSA after each stage");
    colorOutCheck_= new QCheckBox("Colour terminal output");
    for (auto* c : {dumpIRCheck_, dumpASMCheck_, dumpCFGCheck_,
                    dumpSSACheck_, colorOutCheck_})
        l->addWidget(c);

    {
        auto* row = new QWidget;
        auto* rl  = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("IR dump path:", row);
        lbl->setMinimumWidth(180);
        irDumpEdit_ = new QLineEdit(row);
        irDumpBtn_  = new QToolButton(row);
        irDumpBtn_->setText("…");
        rl->addWidget(lbl);
        rl->addWidget(irDumpEdit_, 1);
        rl->addWidget(irDumpBtn_);
        l->addWidget(row);
    }

    {
        auto* row = new QWidget;
        auto* rl  = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("Intermediate output dir:", row);
        lbl->setMinimumWidth(180);
        intermDirEdit_ = new QLineEdit(row);
        intermDirBtn_  = new QToolButton(row);
        intermDirBtn_->setText("…");
        rl->addWidget(lbl);
        rl->addWidget(intermDirEdit_, 1);
        rl->addWidget(intermDirBtn_);
        l->addWidget(row);
    }

    l->addWidget(makeSeparator("Output"));

    maxFuncSpin_ = new QSpinBox;
    maxFuncSpin_->setRange(0, 100000);
    maxFuncSpin_->setSpecialValueText("All");
    l->addWidget(makeRow("Max functions", maxFuncSpin_,
                          "0 = decompile all functions"));

    demangleCheck_ = new QCheckBox("Demangle symbol names");
    l->addWidget(demangleCheck_);

    l->addStretch();

    connect(irDumpBtn_,   &QToolButton::clicked, this, &SettingsDialog::onBrowseIRDump);
    connect(intermDirBtn_,&QToolButton::clicked, this, &SettingsDialog::onBrowseIntermediateDir);
    return w;
}

// ─── Tab: Decompiler ─────────────────────────────────────────────────────────

static bool loadLlvmPassUniqueNames(QStringList& outOrder) {
    QFile f(":/retdec/llvm_passes_default.json");
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (!doc.isArray())
        return false;
    QSet<QString> seen;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isString())
            continue;
        const QString n = v.toString();
        if (seen.contains(n))
            continue;
        seen.insert(n);
        outOrder.append(n);
    }
    return !outOrder.isEmpty();
}

QWidget* SettingsDialog::buildDecompilerTab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(12, 12, 12, 12);
    l->setSpacing(6);

    l->addWidget(makeSeparator("LLVM pipeline (CLI)"));

    useCustomLlvmPassesCheck_ = new QCheckBox(
        "Use custom LLVM pass list when running \"Run Full Analysis\" "
        "(adds --llvm-passes-json to retdec-decompiler)");
    l->addWidget(useCustomLlvmPassesCheck_);

    auto* btnRow = new QHBoxLayout;
    llvmPassesAllBtn_     = new QPushButton("Enable all passes");
    llvmPassesNoneBtn_    = new QPushButton("Disable all passes");
    llvmPassesDefaultBtn_ = new QPushButton("Defaults (stock pipeline)");
    btnRow->addWidget(llvmPassesAllBtn_);
    btnRow->addWidget(llvmPassesNoneBtn_);
    btnRow->addWidget(llvmPassesDefaultBtn_);
    btnRow->addStretch();
    l->addLayout(btnRow);

    connect(llvmPassesAllBtn_, &QPushButton::clicked,
            this, &SettingsDialog::onDecompilerPassesAll);
    connect(llvmPassesNoneBtn_, &QPushButton::clicked,
            this, &SettingsDialog::onDecompilerPassesNone);
    connect(llvmPassesDefaultBtn_, &QPushButton::clicked,
            this, &SettingsDialog::onDecompilerPassesDefaults);

    l->addWidget(makeSeparator("Output location"));

    {
        auto* row = new QWidget;
        auto* rl  = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("Decompile output directory:", row);
        lbl->setMinimumWidth(180);
        decompileOutputDirEdit_ = new QLineEdit(row);
        decompileOutputDirBtn_  = new QToolButton(row);
        decompileOutputDirBtn_->setText(QStringLiteral("…"));
        rl->addWidget(lbl);
        rl->addWidget(decompileOutputDirEdit_, 1);
        rl->addWidget(decompileOutputDirBtn_);
        l->addWidget(row);
        l->addWidget(new QLabel(
            "<span style='color:#888;'>Set a folder outside OneDrive to keep large "
            "`.gui-decompiled.*` artifacts out of sync. Leave empty to write beside the binary.</span>"));
    }

    connect(decompileOutputDirBtn_, &QToolButton::clicked,
            this, &SettingsDialog::onBrowseDecompileOutputDir);

    l->addWidget(makeSeparator("Console"));

    liveConsoleTailCheck_ = new QCheckBox(
        "Stream decompiler log to console (may slow decompile on huge logs)");
    l->addWidget(liveConsoleTailCheck_);

    l->addWidget(makeSeparator("Configuration file"));

    {
        auto* row = new QWidget;
        auto* rl  = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("Optional --config JSON:", row);
        lbl->setMinimumWidth(180);
        decompilerConfigEdit_ = new QLineEdit(row);
        decompilerConfigBtn_  = new QToolButton(row);
        decompilerConfigBtn_->setText(QStringLiteral("…"));
        rl->addWidget(lbl);
        rl->addWidget(decompilerConfigEdit_, 1);
        rl->addWidget(decompilerConfigBtn_);
        l->addWidget(row);
    }

    connect(decompilerConfigBtn_, &QToolButton::clicked,
            this, &SettingsDialog::onBrowseDecompilerConfig);

    llvmPassUniqueNames_.clear();
    llvmPassCheckboxes_.clear();
    if (!loadLlvmPassUniqueNames(llvmPassUniqueNames_)) {
        l->addWidget(new QLabel(
            "Could not load pass list resource (:/retdec/llvm_passes_default.json)."));
        l->addStretch();
        return w;
    }

    auto* inner = new QWidget;
    auto* innerLay = new QVBoxLayout(inner);
    innerLay->setContentsMargins(0, 0, 0, 0);
    innerLay->setSpacing(4);
    innerLay->addWidget(new QLabel(
        "<span style='color:#888;'>Each name controls every occurrence of that pass in the ordered pipeline.</span>"));
    for (const QString& name : llvmPassUniqueNames_) {
        auto* c = new QCheckBox(name);
        c->setChecked(true);
        llvmPassCheckboxes_.append(c);
        innerLay->addWidget(c);
    }
    innerLay->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(280);
    l->addWidget(scroll, 1);

    return w;
}

// ─── Tab: Plugins ─────────────────────────────────────────────────────────────

QWidget* SettingsDialog::buildPluginsTab() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(8, 8, 8, 8);
    l->setSpacing(6);

    pluginList_ = new QListWidget;
    pluginList_->setAlternatingRowColors(true);

    // Populate from plugin manager
    const auto& plugins = PluginManager::instance().plugins();
    for (const auto& p : plugins) {
        auto* item = new QListWidgetItem(p.meta.name + "  v" + p.meta.version);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(p.enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, p.meta.id);
        item->setToolTip(p.meta.description);
        pluginList_->addItem(item);
    }

    auto* btnRow = new QHBoxLayout;
    installPluginBtn_ = new QPushButton("Install Plugin…");
    unloadPluginBtn_  = new QPushButton("Unload Selected");
    btnRow->addWidget(installPluginBtn_);
    btnRow->addWidget(unloadPluginBtn_);
    btnRow->addStretch();

    pluginDetailLabel_ = new QLabel("Select a plugin to see details.");
    pluginDetailLabel_->setWordWrap(true);
    pluginDetailLabel_->setStyleSheet("color:#888; padding:4px;");

    l->addWidget(new QLabel("<b>Installed Plugins</b>"));
    l->addWidget(pluginList_, 1);
    l->addLayout(btnRow);
    l->addWidget(pluginDetailLabel_);

    connect(installPluginBtn_, &QPushButton::clicked, this, &SettingsDialog::onInstallPlugin);
    connect(unloadPluginBtn_,  &QPushButton::clicked, this, &SettingsDialog::onUnloadPlugin);
    connect(pluginList_, &QListWidget::itemChanged, this, &SettingsDialog::onPluginToggled);
    connect(pluginList_, &QListWidget::currentItemChanged,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
        if (!cur) return;
        QString id = cur->data(Qt::UserRole).toString();
        const auto* lp = PluginManager::instance().findPlugin(id);
        if (lp) {
            pluginDetailLabel_->setText(
                "<b>" + lp->meta.name + "</b> v" + lp->meta.version +
                " — " + lp->meta.description +
                "<br><i>Author: " + lp->meta.author + "</i>" +
                "<br><i>File: " + lp->filePath + "</i>");
        }
    });

    return w;
}

// ─── populateFromSettings ────────────────────────────────────────────────────

void SettingsDialog::populateFromSettings() {
    const auto& s = AppSettings::instance();

    // General
    themeCombo_->setCurrentIndex(static_cast<int>(s.general.theme));
    fontCombo_->setCurrentFont(s.general.editorFont);
    fontSizeSpin_->setValue(s.general.fontSize);
    langCombo_->setCurrentIndex(0);  // simplified
    lineNumCheck_->setChecked(s.general.showLineNumbers);
    wordWrapCheck_->setChecked(s.general.wordWrap);
    restoreCheck_->setChecked(s.general.restoreSession);

    // Analysis
    enableTypingCheck_->setChecked(s.analysis.enableTyping);
    enablePatternCheck_->setChecked(s.analysis.enablePatternMatch);
    enableConcurCheck_->setChecked(s.analysis.enableConcurrency);
    enableCudaCheck_->setChecked(s.analysis.enableCudaRecovery);
    enableSerialCheck_->setChecked(s.analysis.enableSerialDetect);
    enableModuleCheck_->setChecked(s.analysis.enableModuleCluster);
    enableCxxCheck_->setChecked(s.analysis.enableCxxLifter);
    typeConfSpin_->setValue(s.analysis.minTypeConfidence);
    patternConfSpin_->setValue(s.analysis.minPatternConfidence);
    recovConfSpin_->setValue(s.analysis.minRecoveryConfidence);
    maxTimeSpin_->setValue(s.analysis.maxAnalysisTimeSecs);
    threadCountSpin_->setValue(s.analysis.threadCount);

    // CUDA
    kernelCacheEdit_->setText(s.cuda.kernelCacheDir);
    clProfilingCheck_->setChecked(s.cuda.enableProfiling);
    useGPUCheck_->setChecked(s.cuda.useGPU);
    wgSizeSpin_->setValue(s.cuda.blockSize);

    // ML
    modelPathEdit_->setText(s.ml.modelPath);
    quantCombo_->setCurrentIndex(static_cast<int>(s.ml.quantLevel));
    inferDevCombo_->setCurrentIndex(static_cast<int>(s.ml.inferenceDevice));
    tempSpin_->setValue(s.ml.temperature);
    topPSpin_->setValue(s.ml.topP);
    topKSpin_->setValue(s.ml.topK);
    maxTokensSpin_->setValue(s.ml.maxNewTokens);
    contextLenSpin_->setValue(s.ml.contextLength);
    streamCheck_->setChecked(s.ml.streamOutput);

    // Recovery
    detectSTLCheck_->setChecked(s.recovery.detectSTL);
    detectCryptoCheck_->setChecked(s.recovery.detectCrypto);
    detectPatternCheck_->setChecked(s.recovery.detectPatterns);
    detectConcurCheck_->setChecked(s.recovery.detectConcurrency);
    detectCudaCheck_->setChecked(s.recovery.detectCuda);
    detectRTTICheck_->setChecked(s.recovery.detectRTTI);
    detectEHCheck_->setChecked(s.recovery.detectExceptions);
    detectVirtCheck_->setChecked(s.recovery.detectVirtual);
    stlConfSpin_->setValue(s.recovery.stlConfidence);
    cryptoConfSpin_->setValue(s.recovery.cryptoConfidence);
    patConf2Spin_->setValue(s.recovery.patternConfidence);
    concurConfSpin_->setValue(s.recovery.concurrencyConfidence);

    // Advanced
    verbCombo_->setCurrentIndex(static_cast<int>(s.advanced.verbosity));
    irDumpEdit_->setText(s.advanced.irDumpPath);
    intermDirEdit_->setText(s.advanced.intermediateDir);
    dumpIRCheck_->setChecked(s.advanced.dumpIR);
    dumpASMCheck_->setChecked(s.advanced.dumpASM);
    dumpCFGCheck_->setChecked(s.advanced.dumpCFG);
    dumpSSACheck_->setChecked(s.advanced.dumpSSA);
    colorOutCheck_->setChecked(s.advanced.colorOutput);
    maxFuncSpin_->setValue(s.advanced.maxFunctions);
    demangleCheck_->setChecked(s.advanced.demangleNames);

    // Decompiler
    if (useCustomLlvmPassesCheck_) {
        useCustomLlvmPassesCheck_->setChecked(s.decompiler.useCustomLlvmPasses);
        const QSet<QString> disabled(s.decompiler.llvmPassesDisabled.begin(),
                                     s.decompiler.llvmPassesDisabled.end());
        for (int i = 0; i < llvmPassCheckboxes_.size(); ++i) {
            const QString& n = llvmPassUniqueNames_.at(i);
            llvmPassCheckboxes_[i]->setChecked(!disabled.contains(n));
        }
    }
    if (decompilerConfigEdit_)
        decompilerConfigEdit_->setText(s.decompiler.extraConfigPath);
    if (decompileOutputDirEdit_)
        decompileOutputDirEdit_->setText(s.decompiler.decompileOutputDir);
    if (liveConsoleTailCheck_)
        liveConsoleTailCheck_->setChecked(s.decompiler.liveConsoleTail);
}

// ─── applyToSettings ─────────────────────────────────────────────────────────

void SettingsDialog::applyToSettings() {
    auto& s = AppSettings::instance();

    // General
    s.general.theme         = static_cast<GeneralSettings::Theme>(themeCombo_->currentIndex());
    s.general.editorFont    = fontCombo_->currentFont();
    s.general.fontSize      = fontSizeSpin_->value();
    s.general.showLineNumbers = lineNumCheck_->isChecked();
    s.general.wordWrap      = wordWrapCheck_->isChecked();
    s.general.restoreSession = restoreCheck_->isChecked();

    // Analysis
    s.analysis.enableTyping          = enableTypingCheck_->isChecked();
    s.analysis.enablePatternMatch    = enablePatternCheck_->isChecked();
    s.analysis.enableConcurrency     = enableConcurCheck_->isChecked();
    s.analysis.enableCudaRecovery    = enableCudaCheck_->isChecked();
    s.analysis.enableSerialDetect    = enableSerialCheck_->isChecked();
    s.analysis.enableModuleCluster   = enableModuleCheck_->isChecked();
    s.analysis.enableCxxLifter       = enableCxxCheck_->isChecked();
    s.analysis.minTypeConfidence     = typeConfSpin_->value();
    s.analysis.minPatternConfidence  = patternConfSpin_->value();
    s.analysis.minRecoveryConfidence = recovConfSpin_->value();
    s.analysis.maxAnalysisTimeSecs   = maxTimeSpin_->value();
    s.analysis.threadCount           = threadCountSpin_->value();

    // CUDA
    s.cuda.kernelCacheDir  = kernelCacheEdit_->text();
    s.cuda.enableProfiling = clProfilingCheck_->isChecked();
    s.cuda.useGPU          = useGPUCheck_->isChecked();
    s.cuda.blockSize       = wgSizeSpin_->value();

    // ML
    s.ml.modelPath       = modelPathEdit_->text();
    s.ml.quantLevel      = static_cast<MLSettings::QuantLevel>(quantCombo_->currentIndex());
    s.ml.inferenceDevice = static_cast<MLSettings::InferenceDevice>(inferDevCombo_->currentIndex());
    s.ml.temperature     = tempSpin_->value();
    s.ml.topP            = topPSpin_->value();
    s.ml.topK            = topKSpin_->value();
    s.ml.maxNewTokens    = maxTokensSpin_->value();
    s.ml.contextLength   = contextLenSpin_->value();
    s.ml.streamOutput    = streamCheck_->isChecked();

    // Recovery
    s.recovery.detectSTL          = detectSTLCheck_->isChecked();
    s.recovery.detectCrypto       = detectCryptoCheck_->isChecked();
    s.recovery.detectPatterns     = detectPatternCheck_->isChecked();
    s.recovery.detectConcurrency  = detectConcurCheck_->isChecked();
    s.recovery.detectCuda         = detectCudaCheck_->isChecked();
    s.recovery.detectRTTI         = detectRTTICheck_->isChecked();
    s.recovery.detectExceptions   = detectEHCheck_->isChecked();
    s.recovery.detectVirtual      = detectVirtCheck_->isChecked();
    s.recovery.stlConfidence      = stlConfSpin_->value();
    s.recovery.cryptoConfidence   = cryptoConfSpin_->value();
    s.recovery.patternConfidence  = patConf2Spin_->value();
    s.recovery.concurrencyConfidence = concurConfSpin_->value();

    // Advanced
    s.advanced.verbosity      = static_cast<AdvancedSettings::Verbosity>(verbCombo_->currentIndex());
    s.advanced.irDumpPath     = irDumpEdit_->text();
    s.advanced.intermediateDir = intermDirEdit_->text();
    s.advanced.dumpIR         = dumpIRCheck_->isChecked();
    s.advanced.dumpASM        = dumpASMCheck_->isChecked();
    s.advanced.dumpCFG        = dumpCFGCheck_->isChecked();
    s.advanced.dumpSSA        = dumpSSACheck_->isChecked();
    s.advanced.colorOutput    = colorOutCheck_->isChecked();
    s.advanced.maxFunctions   = maxFuncSpin_->value();
    s.advanced.demangleNames  = demangleCheck_->isChecked();

    // Decompiler
    if (useCustomLlvmPassesCheck_) {
        s.decompiler.useCustomLlvmPasses = useCustomLlvmPassesCheck_->isChecked();
        s.decompiler.llvmPassesDisabled.clear();
        for (int i = 0; i < llvmPassCheckboxes_.size(); ++i) {
            if (!llvmPassCheckboxes_[i]->isChecked())
                s.decompiler.llvmPassesDisabled.append(llvmPassUniqueNames_.at(i));
        }
    }
    if (decompilerConfigEdit_)
        s.decompiler.extraConfigPath = decompilerConfigEdit_->text();
    if (decompileOutputDirEdit_)
        s.decompiler.decompileOutputDir = decompileOutputDirEdit_->text().trimmed();
    if (liveConsoleTailCheck_)
        s.decompiler.liveConsoleTail = liveConsoleTailCheck_->isChecked();

    s.notifySettingsChanged();
}

// ─── Slots ───────────────────────────────────────────────────────────────────

void SettingsDialog::onApply() {
    applyToSettings();
    AppSettings::instance().save();
}

void SettingsDialog::onOK() {
    onApply();
    accept();
}

void SettingsDialog::onReset() {
    AppSettings::instance().resetToDefaults();
    populateFromSettings();
}

void SettingsDialog::onBrowseModelPath() {
    QString path = QFileDialog::getOpenFileName(
        this, "Select GGUF model", modelPathEdit_->text(),
        "GGUF models (*.gguf);;All files (*)");
    if (!path.isEmpty()) modelPathEdit_->setText(path);
}

void SettingsDialog::onBrowseKernelCache() {
    QString path = QFileDialog::getExistingDirectory(
        this, "Kernel cache directory", kernelCacheEdit_->text());
    if (!path.isEmpty()) kernelCacheEdit_->setText(path);
}

void SettingsDialog::onBrowseIRDump() {
    QString path = QFileDialog::getExistingDirectory(
        this, "IR dump directory", irDumpEdit_->text());
    if (!path.isEmpty()) irDumpEdit_->setText(path);
}

void SettingsDialog::onBrowseIntermediateDir() {
    QString path = QFileDialog::getExistingDirectory(
        this, "Intermediate output directory", intermDirEdit_->text());
    if (!path.isEmpty()) intermDirEdit_->setText(path);
}

void SettingsDialog::onInstallPlugin() {
    QString path = QFileDialog::getOpenFileName(
        this, "Install plugin", "",
#ifdef Q_OS_WIN
        "Shared libraries (*.dll);;All files (*)"
#else
        "Shared libraries (*.so);;All files (*)"
#endif
    );
    if (path.isEmpty()) return;
    if (PluginManager::instance().loadPlugin(path)) {
        const auto& plugins = PluginManager::instance().plugins();
        if (!plugins.empty()) {
            const auto& p = plugins.back();
            auto* item = new QListWidgetItem(p.meta.name + "  v" + p.meta.version);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
            item->setData(Qt::UserRole, p.meta.id);
            pluginList_->addItem(item);
        }
    }
}

void SettingsDialog::onUnloadPlugin() {
    auto* item = pluginList_->currentItem();
    if (!item) return;
    QString id = item->data(Qt::UserRole).toString();
    PluginManager::instance().unloadPlugin(id);
    delete pluginList_->takeItem(pluginList_->row(item));
}

void SettingsDialog::onPluginToggled(QListWidgetItem* item) {
    if (!item) return;
    QString id      = item->data(Qt::UserRole).toString();
    bool    enabled = (item->checkState() == Qt::Checked);
    PluginManager::instance().setEnabled(id, enabled);
}

void SettingsDialog::onFontChanged(const QFont& font) {
    (void)font;
}

void SettingsDialog::onThemeChanged(int) {
}

void SettingsDialog::onDecompilerPassesAll() {
    for (QCheckBox* c : llvmPassCheckboxes_)
        if (c) c->setChecked(true);
}

void SettingsDialog::onDecompilerPassesNone() {
    for (QCheckBox* c : llvmPassCheckboxes_)
        if (c) c->setChecked(false);
}

void SettingsDialog::onDecompilerPassesDefaults() {
    if (useCustomLlvmPassesCheck_)
        useCustomLlvmPassesCheck_->setChecked(false);
    onDecompilerPassesAll();
}

void SettingsDialog::onBrowseDecompilerConfig() {
    QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Decompiler configuration JSON"),
            decompilerConfigEdit_ ? decompilerConfigEdit_->text() : QString(),
            QStringLiteral("JSON (*.json);;All files (*)"));
    if (!path.isEmpty() && decompilerConfigEdit_)
        decompilerConfigEdit_->setText(path);
}

void SettingsDialog::onBrowseDecompileOutputDir() {
    QString path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Decompile output directory"),
            decompileOutputDirEdit_ ? decompileOutputDirEdit_->text() : QString());
    if (!path.isEmpty() && decompileOutputDirEdit_)
        decompileOutputDirEdit_->setText(path);
}

} // namespace retdec::gui::panels
