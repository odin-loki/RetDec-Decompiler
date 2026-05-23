/**
 * @file include/retdec/gui/settings/settings.h
 * @brief Application Settings — Qt GUI Stage 57.
 *
 * All persistent application settings are stored through `AppSettings`, which
 * wraps `QSettings` (INI file on Linux/macOS, registry on Windows).  Each
 * logical group of settings is represented by a plain struct for easy passing
 * through the codebase without QObject coupling.
 *
 * Settings groups:
 *   GeneralSettings    — UI theme, font, language
 *   AnalysisSettings   — stage enable/disable, confidence thresholds, threads
 *   CUDASettings       — device selection, kernel cache, profiling
 *   MLSettings         — model path, quantisation, inference device, temperature
 *   RecoverySettings   — per-detector enable/disable, confidence thresholds
 *   AdvancedSettings   — verbosity, IR dump, intermediate output
 *   PluginSettings     — search paths, loaded plugin IDs
 *   DecompilerSettings — optional LLVM pass pipeline overrides for CLI runs from the GUI
 */

#ifndef RETDEC_GUI_SETTINGS_SETTINGS_H
#define RETDEC_GUI_SETTINGS_SETTINGS_H

#include <QColor>
#include <QFont>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdint>

namespace retdec::gui {

// ─── GeneralSettings ─────────────────────────────────────────────────────────

struct GeneralSettings {
    enum class Theme { Dark, Light, SystemDefault };

    Theme   theme            = Theme::Dark;
    QFont   editorFont       = QFont("Cascadia Code,JetBrains Mono,Consolas", 10);
    int     fontSize         = 10;
    QString language         = "en";
    bool    showLineNumbers  = true;
    bool    wordWrap         = false;
    bool    restoreSession   = true;
    QString lastOpenDir;
    /// Last binary opened via File → Open Binary (session restore).
    QString lastBinaryPath;
};

// ─── AnalysisSettings ────────────────────────────────────────────────────────

struct AnalysisSettings {
    // Stage enable flags
    bool enableTyping          = true;
    bool enablePatternMatch    = true;
    bool enableConcurrency     = true;
    bool enableCudaRecovery    = true;
    bool enableSerialDetect    = true;
    bool enableModuleCluster   = true;
    bool enableCxxLifter       = true;

    // Confidence thresholds (0.0–1.0)
    double minTypeConfidence     = 0.5;
    double minPatternConfidence  = 0.6;
    double minRecoveryConfidence = 0.4;

    // Resources
    int    maxAnalysisTimeSecs   = 300;
    int    threadCount           = 0;  ///< 0 = hardware_concurrency
};

// ─── CUDASettings ────────────────────────────────────────────────────────────

struct CUDASettings {
    QString deviceName;         ///< preferred CUDA device name (empty = auto)
    int     deviceIndex  = 0;
    QString kernelCacheDir;
    bool    enableProfiling = false;
    bool    useGPU          = true;
    int     blockSize       = 256;
};

// ─── MLSettings ──────────────────────────────────────────────────────────────

struct MLSettings {
    enum class QuantLevel { Q4_0, Q4_K_M, Q5_K_M, Q6_K, F16, F32 };
    enum class InferenceDevice { CPU, GPU, Auto };

    QString         modelPath;
    QuantLevel      quantLevel      = QuantLevel::Q4_K_M;
    InferenceDevice inferenceDevice = InferenceDevice::Auto;
    double          temperature     = 0.7;
    double          topP            = 0.9;
    int             topK            = 40;
    int             maxNewTokens    = 512;
    int             contextLength   = 4096;
    bool            streamOutput    = true;
};

// ─── RecoverySettings ────────────────────────────────────────────────────────

struct RecoverySettings {
    // Detector enable flags
    bool detectSTL         = true;
    bool detectCrypto      = true;
    bool detectPatterns    = true;
    bool detectConcurrency = true;
    bool detectCuda        = true;
    bool detectRTTI        = true;
    bool detectExceptions  = true;
    bool detectVirtual     = true;

    // Per-category confidence thresholds
    double stlConfidence        = 0.6;
    double cryptoConfidence     = 0.7;
    double patternConfidence    = 0.5;
    double concurrencyConfidence = 0.6;
};

// ─── AdvancedSettings ────────────────────────────────────────────────────────

struct AdvancedSettings {
    enum class Verbosity { Quiet, Normal, Verbose, Debug };

    Verbosity verbosity      = Verbosity::Normal;
    QString   irDumpPath;
    QString   intermediateDir;
    bool      dumpIR         = false;
    bool      dumpASM        = false;
    bool      dumpCFG        = false;
    bool      dumpSSA        = false;
    bool      colorOutput    = true;
    int       maxFunctions   = 0;  ///< 0 = unlimited
    bool      demangleNames  = true;
};

// ─── PluginSettings ──────────────────────────────────────────────────────────

struct PluginSettings {
    QStringList searchPaths;
    QStringList enabledPlugins;
    bool        autoLoadPlugins = true;
};

// ─── DecompilerSettings ───────────────────────────────────────────────────────

struct DecompilerSettings {
    /// When true, Run Full Analysis passes --llvm-passes-json built from defaults minus disabled names.
    bool useCustomLlvmPasses = false;
    /// Pass names removed from the default pipeline (all occurrences dropped). Empty = full default list.
    QStringList llvmPassesDisabled;
    /// If set and the file exists, Run Full Analysis prepends --config with this path.
    QString extraConfigPath;
    /// When set, `.gui-decompiled.*` artifacts are written here (binary basename only). Empty = beside binary.
    QString decompileOutputDir;
    /// When true, decompile log bytes are appended to the Console tab during the run (rate-limited).
    bool liveConsoleTail = false;
    /// Preferred `--output-lang` for native binaries (c|cpp|python|csharp|java|wat).
    QString outputLang = QStringLiteral("c");
};

// ─── AppSettings (main facade) ────────────────────────────────────────────────

/**
 * @brief Singleton facade for all application settings.
 *
 * Persists to:
 *   Linux/macOS: ~/.config/retdec/settings.ini
 *   Windows:     HKCU\Software\retdec\settings
 *
 * Usage:
 *   auto& s = AppSettings::instance();
 *   s.load();
 *   s.general.theme = GeneralSettings::Theme::Light;
 *   s.save();
 */
class AppSettings : public QObject {
    Q_OBJECT
public:
    static AppSettings& instance();

    GeneralSettings  general;
    AnalysisSettings analysis;
    CUDASettings     cuda;
    MLSettings       ml;
    RecoverySettings recovery;
    AdvancedSettings   advanced;
    PluginSettings     plugins;
    DecompilerSettings decompiler;

    /**
     * @brief Load all settings from persistent storage.
     *        Call once at application startup.
     */
    void load();

    /**
     * @brief Persist all settings to storage.
     */
    void save() const;

    /**
     * @brief Reset all settings to their compile-time defaults.
     */
    void resetToDefaults();

    /**
     * @brief Export settings to a JSON file.
     */
    bool exportToFile(const QString& path) const;

    /**
     * @brief Import settings from a JSON file.
     */
    bool importFromFile(const QString& path);

    /**
     * @brief Emit settingsChanged() after external code mutates settings (e.g. Settings dialog).
     */
    void notifySettingsChanged();

signals:
    void settingsChanged();

private:
    AppSettings();
    ~AppSettings() = default;
    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

    void loadGeneral (::QSettings& s);
    void loadAnalysis(::QSettings& s);
    void loadCUDA    (::QSettings& s);
    void loadML      (::QSettings& s);
    void loadRecovery(::QSettings& s);
    void loadAdvanced(::QSettings& s);
    void loadPlugins   (::QSettings& s);
    void loadDecompiler(::QSettings& s);

    void saveGeneral (::QSettings& s) const;
    void saveAnalysis(::QSettings& s) const;
    void saveCUDA    (::QSettings& s) const;
    void saveML      (::QSettings& s) const;
    void saveRecovery(::QSettings& s) const;
    void saveAdvanced(::QSettings& s) const;
    void savePlugins   (::QSettings& s) const;
    void saveDecompiler(::QSettings& s) const;
};

} // namespace retdec::gui

#endif // RETDEC_GUI_SETTINGS_SETTINGS_H
