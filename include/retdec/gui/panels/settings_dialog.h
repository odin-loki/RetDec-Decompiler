/**
 * @file include/retdec/gui/panels/settings_dialog.h
 * @brief SettingsDialog — tabbed QDialog for all RetDec application settings.
 *
 * Tabs:
 *   General    — theme, font, language, session restore
 *   Analysis   — stage toggles, confidence thresholds, thread count
 *   OpenCL     — device, cache path, profiling
 *   ML         — model path, quant level, device, temperature
 *   Recovery   — per-detector toggles and thresholds
 *   Advanced   — verbosity, IR dump, intermediate output
 *   Plugins    — plugin list with enable/disable, install, unload
 *   Decompiler — LLVM pass pipeline toggles and optional --config JSON path
 */

#ifndef RETDEC_GUI_PANELS_SETTINGS_DIALOG_H
#define RETDEC_GUI_PANELS_SETTINGS_DIALOG_H

#include "retdec/gui/settings/settings.h"

#include <QDialog>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QToolButton;
QT_END_NAMESPACE

namespace retdec::gui::panels {

/**
 * @brief The main application settings dialog.
 *
 * Constructed with a copy of all current settings; edits are applied to the
 * copy and committed to AppSettings only when the user clicks "Apply" or "OK".
 *
 * Usage:
 *   SettingsDialog dlg(parent);
 *   if (dlg.exec() == QDialog::Accepted)
 *       AppSettings::instance().save();
 */
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private slots:
    void onApply();
    void onOK();
    void onReset();
    void onBrowseModelPath();
    void onBrowseKernelCache();
    void onBrowseIRDump();
    void onBrowseIntermediateDir();
    void onInstallPlugin();
    void onUnloadPlugin();
    void onPluginToggled(QListWidgetItem* item);
    void onFontChanged(const QFont& font);
    void onThemeChanged(int index);
    void onDecompilerPassesAll();
    void onDecompilerPassesNone();
    void onDecompilerPassesDefaults();
    void onBrowseDecompilerConfig();

private:
    // Tab builders
    QWidget* buildGeneralTab();
    QWidget* buildAnalysisTab();
    QWidget* buildCUDATab();
    QWidget* buildMLTab();
    QWidget* buildRecoveryTab();
    QWidget* buildAdvancedTab();
    QWidget* buildPluginsTab();
    QWidget* buildDecompilerTab();

    void populateFromSettings();
    void applyToSettings();

    // Helpers
    QWidget* makeRow(const QString& label, QWidget* widget, const QString& tooltip = QString{});
    QWidget* makeSeparator(const QString& title);

    // ── Tab: General ──────────────────────────────────────────────────────
    QComboBox*    themeCombo_      = nullptr;
    QFontComboBox*fontCombo_       = nullptr;
    QSpinBox*     fontSizeSpin_    = nullptr;
    QComboBox*    langCombo_       = nullptr;
    QCheckBox*    lineNumCheck_    = nullptr;
    QCheckBox*    wordWrapCheck_   = nullptr;
    QCheckBox*    restoreCheck_    = nullptr;

    // ── Tab: Analysis ─────────────────────────────────────────────────────
    QCheckBox*    enableTypingCheck_     = nullptr;
    QCheckBox*    enablePatternCheck_    = nullptr;
    QCheckBox*    enableConcurCheck_     = nullptr;
    QCheckBox*    enableCudaCheck_       = nullptr;
    QCheckBox*    enableSerialCheck_     = nullptr;
    QCheckBox*    enableModuleCheck_     = nullptr;
    QCheckBox*    enableCxxCheck_        = nullptr;
    QDoubleSpinBox* typeConfSpin_        = nullptr;
    QDoubleSpinBox* patternConfSpin_     = nullptr;
    QDoubleSpinBox* recovConfSpin_       = nullptr;
    QSpinBox*     maxTimeSpin_           = nullptr;
    QSpinBox*     threadCountSpin_       = nullptr;

    // ── Tab: OpenCL ───────────────────────────────────────────────────────
    QComboBox*    clDeviceCombo_     = nullptr;
    QLineEdit*    kernelCacheEdit_   = nullptr;
    QToolButton*  kernelCacheBtn_    = nullptr;
    QCheckBox*    clProfilingCheck_  = nullptr;
    QCheckBox*    useGPUCheck_       = nullptr;
    QSpinBox*     wgSizeSpin_        = nullptr;

    // ── Tab: ML ───────────────────────────────────────────────────────────
    QLineEdit*    modelPathEdit_     = nullptr;
    QToolButton*  modelPathBtn_      = nullptr;
    QComboBox*    quantCombo_        = nullptr;
    QComboBox*    inferDevCombo_     = nullptr;
    QDoubleSpinBox* tempSpin_        = nullptr;
    QDoubleSpinBox* topPSpin_        = nullptr;
    QSpinBox*     topKSpin_          = nullptr;
    QSpinBox*     maxTokensSpin_     = nullptr;
    QSpinBox*     contextLenSpin_    = nullptr;
    QCheckBox*    streamCheck_       = nullptr;

    // ── Tab: Recovery ─────────────────────────────────────────────────────
    QCheckBox*    detectSTLCheck_    = nullptr;
    QCheckBox*    detectCryptoCheck_ = nullptr;
    QCheckBox*    detectPatternCheck_= nullptr;
    QCheckBox*    detectConcurCheck_ = nullptr;
    QCheckBox*    detectCudaCheck_   = nullptr;
    QCheckBox*    detectRTTICheck_   = nullptr;
    QCheckBox*    detectEHCheck_     = nullptr;
    QCheckBox*    detectVirtCheck_   = nullptr;
    QDoubleSpinBox* stlConfSpin_     = nullptr;
    QDoubleSpinBox* cryptoConfSpin_  = nullptr;
    QDoubleSpinBox* patConf2Spin_    = nullptr;
    QDoubleSpinBox* concurConfSpin_  = nullptr;

    // ── Tab: Advanced ─────────────────────────────────────────────────────
    QComboBox*    verbCombo_         = nullptr;
    QLineEdit*    irDumpEdit_        = nullptr;
    QToolButton*  irDumpBtn_         = nullptr;
    QLineEdit*    intermDirEdit_     = nullptr;
    QToolButton*  intermDirBtn_      = nullptr;
    QCheckBox*    dumpIRCheck_       = nullptr;
    QCheckBox*    dumpASMCheck_      = nullptr;
    QCheckBox*    dumpCFGCheck_      = nullptr;
    QCheckBox*    dumpSSACheck_      = nullptr;
    QCheckBox*    colorOutCheck_     = nullptr;
    QSpinBox*     maxFuncSpin_       = nullptr;
    QCheckBox*    demangleCheck_     = nullptr;

    // ── Tab: Decompiler ───────────────────────────────────────────────────
    QCheckBox*    useCustomLlvmPassesCheck_ = nullptr;
    QVector<QCheckBox*> llvmPassCheckboxes_;
    QStringList         llvmPassUniqueNames_;
    QPushButton*        llvmPassesAllBtn_    = nullptr;
    QPushButton*        llvmPassesNoneBtn_   = nullptr;
    QPushButton*        llvmPassesDefaultBtn_ = nullptr;
    QLineEdit*          decompilerConfigEdit_ = nullptr;
    QToolButton*        decompilerConfigBtn_  = nullptr;

    // ── Tab: Plugins ──────────────────────────────────────────────────────
    QListWidget*  pluginList_        = nullptr;
    QPushButton*  installPluginBtn_  = nullptr;
    QPushButton*  unloadPluginBtn_   = nullptr;
    QLabel*       pluginDetailLabel_ = nullptr;

    // ── Buttons ───────────────────────────────────────────────────────────
    QDialogButtonBox* buttonBox_     = nullptr;
    QTabWidget*       tabWidget_     = nullptr;
};

} // namespace retdec::gui::panels

#endif // RETDEC_GUI_PANELS_SETTINGS_DIALOG_H
