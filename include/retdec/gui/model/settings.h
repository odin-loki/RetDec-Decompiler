/**
 * @file include/retdec/gui/model/settings.h
 * @brief Application settings (Qt-free port of the Qt GUI's AppSettings).
 *
 * Each logical group is a plain struct; AppSettings bundles them, persists
 * them as JSON and tells listeners when they change.
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
 *
 * Storage: `<forge::os::config_dir()>/retdec/settings.json`
 * (~/.config/retdec on Linux, ~/Library/Application Support/retdec on macOS,
 * %APPDATA%\retdec on Windows), written atomically. The document has one
 * object per group, with the group and key names the Qt version used in its
 * QSettings file ("General"/"theme", "Analysis"/"minTypeConf", ...), so the
 * values and defaults are the same:
 *
 * ```json
 * { "General": { "theme": 0, "font": "Cascadia Code,JetBrains Mono,Consolas", "fontPointSize": 10, ... },
 *   "Analysis": { ... }, "CUDA": { ... }, "ML": { ... }, "Recovery": { ... },
 *   "Advanced": { ... }, "Plugins": { ... }, "Decompiler": { ... } }
 * ```
 *
 * exportToFile()/importFromFile() keep the Qt export format (its own key
 * names, e.g. "minTypeConfidence", "showLineNums").
 *
 * Fonts: QFont became FontSpec (family list string + point size). Colours, if
 * a group ever needs one, are stored as "#rrggbb" strings.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_SETTINGS_H
#define RETDEC_GUI_MODEL_SETTINGS_H

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

// ─── FontSpec ────────────────────────────────────────────────────────────────

/// What the Qt version kept in a QFont: a family (or comma-separated fallback
/// list) and a point size.
struct FontSpec
{
	std::string family = "Cascadia Code,JetBrains Mono,Consolas";
	int pointSize = 10;

	/// Families split at commas, trimmed, empties dropped.
	std::vector<std::string> families() const;

	/// Read a QFont::toString() value ("Family,10,-1,5,400,0,...") as the Qt
	/// version stored it: every field before the first numeric one is the
	/// family list, the first numeric field the point size. A string with no
	/// numeric field is taken as the family alone (point size unchanged).
	static FontSpec fromQtString(std::string_view s, int defaultPointSize = 10);

	bool operator==(const FontSpec&) const = default;
};

/// "#rrggbb" for 0xRRGGBB.
std::string colorToHex(uint32_t rgb);
/// "#rrggbb" or "#rgb" (case-insensitive) -> 0xRRGGBB; nothing on bad input.
std::expected<uint32_t, std::string> colorFromHex(std::string_view hex);

// ─── GeneralSettings ─────────────────────────────────────────────────────────

struct GeneralSettings
{
	enum class Theme
	{
		Dark,
		Light,
		SystemDefault
	};

	Theme theme = Theme::Dark;
	FontSpec editorFont{};
	int fontSize = 10;
	std::string language = "en";
	bool showLineNumbers = true;
	bool wordWrap = false;
	bool restoreSession = true;
	std::string lastOpenDir;
	/// Last binary opened via File → Open Binary (session restore).
	std::string lastBinaryPath;

	bool operator==(const GeneralSettings&) const = default;
};

// ─── AnalysisSettings ────────────────────────────────────────────────────────

struct AnalysisSettings
{
	// Stage enable flags
	bool enableTyping = true;
	bool enablePatternMatch = true;
	bool enableConcurrency = true;
	bool enableCudaRecovery = true;
	bool enableSerialDetect = true;
	bool enableModuleCluster = true;
	bool enableCxxLifter = true;

	// Confidence thresholds (0.0–1.0)
	double minTypeConfidence = 0.5;
	double minPatternConfidence = 0.6;
	double minRecoveryConfidence = 0.4;

	// Resources
	int maxAnalysisTimeSecs = 300;
	int threadCount = 0; ///< 0 = hardware_concurrency

	bool operator==(const AnalysisSettings&) const = default;
};

// ─── CUDASettings ────────────────────────────────────────────────────────────

struct CUDASettings
{
	std::string deviceName; ///< preferred CUDA device name (empty = auto)
	int deviceIndex = 0;
	std::string kernelCacheDir;
	bool enableProfiling = false;
	bool useGPU = true;
	int blockSize = 256;

	bool operator==(const CUDASettings&) const = default;
};

// ─── MLSettings ──────────────────────────────────────────────────────────────

struct MLSettings
{
	enum class QuantLevel
	{
		Q4_0,
		Q4_K_M,
		Q5_K_M,
		Q6_K,
		F16,
		F32
	};
	enum class InferenceDevice
	{
		CPU,
		GPU,
		Auto
	};

	std::string modelPath;
	QuantLevel quantLevel = QuantLevel::Q4_K_M;
	InferenceDevice inferenceDevice = InferenceDevice::Auto;
	double temperature = 0.7;
	double topP = 0.9;
	int topK = 40;
	int maxNewTokens = 512;
	int contextLength = 4096;
	bool streamOutput = true;
	/// Optional expected SHA-256 of the GGUF (`RETDEC_NEURAL_MODEL_SHA256`).
	std::string modelSha256;
	/// Unused (N13): not published as RETDEC_NEURAL_BATCH until real batching exists.
	bool batchRefine = false;

	bool operator==(const MLSettings&) const = default;
};

// ─── RecoverySettings ────────────────────────────────────────────────────────

struct RecoverySettings
{
	// Detector enable flags
	bool detectSTL = true;
	bool detectCrypto = true;
	bool detectPatterns = true;
	bool detectConcurrency = true;
	bool detectCuda = true;
	bool detectRTTI = true;
	bool detectExceptions = true;
	bool detectVirtual = true;

	// Per-category confidence thresholds
	double stlConfidence = 0.6;
	double cryptoConfidence = 0.7;
	double patternConfidence = 0.5;
	double concurrencyConfidence = 0.6;

	bool operator==(const RecoverySettings&) const = default;
};

// ─── AdvancedSettings ────────────────────────────────────────────────────────

struct AdvancedSettings
{
	enum class Verbosity
	{
		Quiet,
		Normal,
		Verbose,
		Debug
	};

	Verbosity verbosity = Verbosity::Normal;
	std::string irDumpPath;
	std::string intermediateDir;
	bool dumpIR = false;
	bool dumpASM = false;
	bool dumpCFG = false;
	bool dumpSSA = false;
	bool colorOutput = true;
	int maxFunctions = 0; ///< 0 = unlimited
	bool demangleNames = true;

	bool operator==(const AdvancedSettings&) const = default;
};

// ─── PluginSettings ──────────────────────────────────────────────────────────

struct PluginSettings
{
	std::vector<std::string> searchPaths;
	std::vector<std::string> enabledPlugins;
	bool autoLoadPlugins = true;

	bool operator==(const PluginSettings&) const = default;
};

// ─── DecompilerSettings ──────────────────────────────────────────────────────

struct DecompilerSettings
{
	/// When true, Run Full Analysis passes --llvm-passes-json built from defaults minus disabled names.
	bool useCustomLlvmPasses = false;
	/// Pass names removed from the default pipeline (all occurrences dropped). Empty = full default list.
	std::vector<std::string> llvmPassesDisabled;
	/// If set and the file exists, Run Full Analysis prepends --config with this path.
	std::string extraConfigPath;
	/// When set, `.gui-decompiled.*` artifacts are written here (binary basename only). Empty = beside binary.
	std::string decompileOutputDir;
	/// When true, decompile log bytes are appended to the Console tab during the run (rate-limited).
	bool liveConsoleTail = false;
	/// Preferred `--output-lang` for native binaries (c|python|csharp|java|wat).
	std::string outputLang = "c";
	/// Named LLVM pass pipeline for `--profile` (fast|balanced|quality).
	std::string decompileProfile = "balanced";

	bool operator==(const DecompilerSettings&) const = default;
};

/// "cpp", "c++", "cxx" (any case) -> "c"; everything else unchanged. Applied
/// whenever outputLang is read from storage, as the Qt version did.
std::string coerceNativeOutputLang(std::string lang);

// ─── AppSettings ─────────────────────────────────────────────────────────────

/// Every settings group, by value (copyable; no listeners).
struct SettingsSections
{
	GeneralSettings general;
	AnalysisSettings analysis;
	CUDASettings cuda;
	MLSettings ml;
	RecoverySettings recovery;
	AdvancedSettings advanced;
	PluginSettings plugins;
	DecompilerSettings decompiler;

	bool operator==(const SettingsSections&) const = default;
};

/**
 * @brief All application settings plus persistence and change listeners.
 *
 * Usage:
 *   auto& s = AppSettings::instance();
 *   s.load();
 *   s.general.theme = GeneralSettings::Theme::Light;
 *   s.save();
 *   s.notifySettingsChanged();
 *
 * The singleton is what the rest of the GUI reads; tests and the settings
 * dialog may also make their own instances (copying copies the values only,
 * never the listeners or the storage path).
 *
 * Not thread-safe: use it from the UI thread (listeners run on the caller's
 * thread, synchronously).
 */
class AppSettings : public SettingsSections {
public:
	using Listener = std::function<void()>;
	using ListenerId = std::uint64_t;

	static AppSettings& instance();

	AppSettings() = default;
	AppSettings(const AppSettings& other);
	AppSettings& operator=(const AppSettings& other);
	AppSettings& operator=(const SettingsSections& values);

	/// `<forge::os::config_dir()>/retdec/settings.json`, or the override.
	std::filesystem::path storagePath() const;
	/// Use another file for load()/save() (tests, portable installs). Empty = default.
	void setStoragePath(std::filesystem::path path);

	/**
	 * @brief Load all settings from storagePath(). A missing file is not an
	 *        error (defaults stay); a malformed one is (defaults stay too).
	 *        Missing keys keep their defaults. Does not notify listeners.
	 */
	std::expected<void, std::string> load();

	/// Persist all settings to storagePath() (directory created, atomic write).
	std::expected<void, std::string> save() const;

	/// Reset every group to its defaults and notify listeners.
	void resetToDefaults();

	/// Write the Qt-compatible export JSON to @p path.
	std::expected<void, std::string> exportToFile(const std::filesystem::path& path) const;

	/// Read an export JSON written by exportToFile() (or the Qt GUI) and notify
	/// listeners. Groups missing from the file are left as they are.
	std::expected<void, std::string> importFromFile(const std::filesystem::path& path);

	/// The persisted document (what save() writes) / the export document, as text.
	std::string toStorageJson() const;
	std::string toExportJson() const;
	/// Apply a persisted / exported document. Errors leave the settings unchanged.
	std::expected<void, std::string> fromStorageJson(std::string_view json);
	std::expected<void, std::string> fromExportJson(std::string_view json);

	/// Register @p fn to run on every notifySettingsChanged(); returns an id for removal.
	ListenerId addChangeListener(Listener fn);
	void removeChangeListener(ListenerId id);
	/// Run every listener (after external code, e.g. the Settings dialog, mutated settings).
	void notifySettingsChanged();

private:
	std::filesystem::path storagePathOverride_;
	std::map<ListenerId, Listener> listeners_;
	ListenerId nextListenerId_ = 1;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_SETTINGS_H
