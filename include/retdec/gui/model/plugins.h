/**
 * @file include/retdec/gui/model/plugins.h
 * @brief retdec-gui plugins: a C ABI loaded with forge::os::dynlib, and the
 *        manager that loads, orders and runs them.
 *
 * ## The ABI (version 1)
 *
 * A plugin is a shared library (`.so` / `.dylib` / `.dll`) that exports one
 * C function:
 *
 * ```c
 * #include "retdec/gui/model/plugins.h"   // C-compatible above the C++ part
 *
 * static char* my_transform(const char* c, size_t len) { ... return malloc'd UTF-8; }
 * static void my_free(char* p) { free(p); }
 *
 * static const RetdecGuiPluginV1 kPlugin = {
 *     .struct_size = sizeof(RetdecGuiPluginV1),
 *     .abi_version = RETDEC_GUI_PLUGIN_ABI_VERSION,
 *     .id = "com.example.rust", .name = "Rust output", .version = "1.0",
 *     .kinds = RETDEC_GUI_PLUGIN_OUTPUT,
 *     .free_string = my_free,
 *     .format_name = "Rust (experimental)", .file_extension = ".rs",
 *     .transform = my_transform,
 * };
 * RETDEC_GUI_PLUGIN_EXPORT const RetdecGuiPluginV1* retdec_gui_plugin_v1(void) { return &kPlugin; }
 * ```
 *
 * Rules:
 *  - `retdec_gui_plugin_v1` returns a pointer to a descriptor that stays valid
 *    until the library is unloaded (a static is the usual choice).
 *  - `abi_version` must equal RETDEC_GUI_PLUGIN_ABI_VERSION and `struct_size`
 *    must be at least the size of the version-1 struct; a newer host reads
 *    only the fields that fit in `struct_size`, so later versions append fields.
 *  - All strings are UTF-8, NUL-terminated. Descriptor strings are owned by the
 *    plugin. Every `char*` a callback returns is allocated by the plugin and
 *    released by the host through `free_string` (required when any callback
 *    returns a string). A NULL return means "nothing / unchanged".
 *  - Callbacks are invoked on the GUI thread, one at a time, and must not
 *    throw (C++ plugins catch everything at the boundary).
 *  - `kinds` says which groups of fields are meaningful; a plugin may combine
 *    several kinds. Fields of a kind not set may be NULL.
 *
 * Kinds (the Qt GUI's plugin interfaces, as C):
 *  - DECOMPILER (IDecompilerPlugin): `run_stage` gets the current pipeline
 *    texts and returns new decompiled text (or NULL to keep it);
 *    `insert_after_stage` names the built-in stage it follows (NULL = end).
 *  - OUTPUT (IOutputPlugin): an "Export As" format — `format_name`,
 *    `file_extension` (with the dot), `transform(c, len)` -> new text.
 *  - VISUALISATION (IVisualisationPlugin): a dock panel titled `panel_title`.
 *    A plugin cannot hand the host a widget across a C ABI, so `render_panel`
 *    returns the panel's content as UTF-8 text (Markdown allowed) for the
 *    current pipeline state; the host shows it read-only and calls it again
 *    when the function or decompilation changes.
 *  - ANALYSIS (IAnalysisPlugin): `analyse` may return new decompiled text
 *    (NULL = unchanged); `summary` returns a short status-bar line.
 *
 * `initialize` (optional) runs once after loading — return 0 to refuse to load;
 * `shutdown` (optional) runs before unloading.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_PLUGINS_H
#define RETDEC_GUI_MODEL_PLUGINS_H

#include <stddef.h>
#include <stdint.h>

/* ─── C ABI ──────────────────────────────────────────────────────────────── */

#define RETDEC_GUI_PLUGIN_ABI_VERSION 1u
/** Name of the exported entry point. */
#define RETDEC_GUI_PLUGIN_ENTRY_SYMBOL "retdec_gui_plugin_v1"
/** The plugin API version string the Qt GUI used ("1.0"); kept for metadata. */
#define RETDEC_PLUGIN_API_VERSION "1.0"

#if defined(_WIN32)
#define RETDEC_GUI_PLUGIN_EXPORT_ATTR __declspec(dllexport)
#else
#define RETDEC_GUI_PLUGIN_EXPORT_ATTR __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define RETDEC_GUI_PLUGIN_EXPORT extern "C" RETDEC_GUI_PLUGIN_EXPORT_ATTR
#else
#define RETDEC_GUI_PLUGIN_EXPORT RETDEC_GUI_PLUGIN_EXPORT_ATTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum RetdecGuiPluginKind
{
	RETDEC_GUI_PLUGIN_DECOMPILER = 1u << 0,
	RETDEC_GUI_PLUGIN_OUTPUT = 1u << 1,
	RETDEC_GUI_PLUGIN_VISUALISATION = 1u << 2,
	RETDEC_GUI_PLUGIN_ANALYSIS = 1u << 3,
};

/** What pipeline callbacks see. Strings are never NULL (possibly ""). */
typedef struct RetdecGuiPipelineContext
{
	const char* input_binary_path;
	const char* decompiled_text; /**< current decompiled output */
	const char* ir_text;         /**< LLVM IR */
	const char* asm_text;        /**< assembly */
	int analysis_complete;       /**< non-zero once the decompiler has finished */
} RetdecGuiPipelineContext;

typedef struct RetdecGuiPluginV1
{
	uint32_t struct_size; /**< sizeof(RetdecGuiPluginV1) as the plugin was built */
	uint32_t abi_version; /**< RETDEC_GUI_PLUGIN_ABI_VERSION */

	/* Metadata */
	const char* id; /**< unique, e.g. "com.example.myplugin" (required) */
	const char* name;
	const char* version;
	const char* description;
	const char* author;
	const char* const* dependencies; /**< NULL-terminated list of plugin ids, or NULL */
	uint32_t kinds;                  /**< RetdecGuiPluginKind bits */

	/* Lifecycle (optional) */
	int (*initialize)(void); /**< 0 = refuse to load */
	void (*shutdown)(void);
	void (*free_string)(char* s); /**< releases strings returned below */

	/* DECOMPILER */
	const char* insert_after_stage;
	char* (*run_stage)(const RetdecGuiPipelineContext* ctx);

	/* OUTPUT */
	const char* format_name;
	const char* file_extension;
	char* (*transform)(const char* decompiled_c, size_t length);

	/* VISUALISATION */
	const char* panel_title;
	char* (*render_panel)(const RetdecGuiPipelineContext* ctx);

	/* ANALYSIS */
	char* (*analyse)(const RetdecGuiPipelineContext* ctx);
	char* (*summary)(void);
} RetdecGuiPluginV1;

/** Type of the exported entry point. */
typedef const RetdecGuiPluginV1* (*RetdecGuiPluginEntryV1)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ─── C++ host side ──────────────────────────────────────────────────────── */

#ifdef __cplusplus

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

struct PluginMetadata
{
	std::string id;
	std::string name;
	std::string version;
	std::string description;
	std::string author;
	std::string apiVersion = RETDEC_PLUGIN_API_VERSION;
	std::vector<std::string> dependencies; ///< other plugin IDs required
	uint32_t kinds = 0;                    ///< RetdecGuiPluginKind bits
};

/// What pipeline plugins read and may modify (decompiledText).
struct PipelineContext
{
	std::string inputBinaryPath;
	std::string decompiledText;
	std::string irText;
	std::string asmText;
	bool analysisComplete = false;
};

/// Check a descriptor against the ABI rules (non-null, version, size, id,
/// free_string present when a string-returning callback is).
std::expected<void, std::string> validatePluginDescriptor(const RetdecGuiPluginV1* api);

/// Metadata copied out of a descriptor.
PluginMetadata pluginMetadataFrom(const RetdecGuiPluginV1& api);

/**
 * @brief A loaded plugin: its descriptor, the library it came from and C++
 *        wrappers for the callbacks (strings copied, plugin memory released).
 */
class Plugin {
public:
	Plugin(const RetdecGuiPluginV1* api, std::shared_ptr<void> library, std::filesystem::path filePath);

	const PluginMetadata& metadata() const
	{
		return meta_;
	}
	const std::filesystem::path& filePath() const
	{
		return filePath_;
	}
	const RetdecGuiPluginV1& api() const
	{
		return *api_;
	}
	bool enabled() const
	{
		return enabled_;
	}
	void setEnabled(bool on)
	{
		enabled_ = on;
	}

	bool isDecompiler() const
	{
		return (meta_.kinds & RETDEC_GUI_PLUGIN_DECOMPILER) && api_->run_stage;
	}
	bool isOutput() const
	{
		return (meta_.kinds & RETDEC_GUI_PLUGIN_OUTPUT) && api_->transform;
	}
	bool isVisualisation() const
	{
		return (meta_.kinds & RETDEC_GUI_PLUGIN_VISUALISATION) && api_->render_panel;
	}
	bool isAnalysis() const
	{
		return (meta_.kinds & RETDEC_GUI_PLUGIN_ANALYSIS) && api_->analyse;
	}

	// DECOMPILER
	std::string insertAfterStage() const;
	/// Run the stage; ctx.decompiledText is replaced when the plugin returns text.
	void runStage(PipelineContext& ctx) const;

	// OUTPUT
	std::string formatName() const;
	std::string fileExtension() const;
	std::expected<std::string, std::string> transform(std::string_view decompiledC) const;

	// VISUALISATION
	std::string panelTitle() const;
	std::string renderPanel(const PipelineContext& ctx) const;

	// ANALYSIS
	void analyse(PipelineContext& ctx) const;
	std::string summary() const;

private:
	std::optional<std::string> take(char* s) const;

	const RetdecGuiPluginV1* api_;
	std::shared_ptr<void> library_; ///< keeps the code mapped while the Plugin exists
	std::filesystem::path filePath_;
	PluginMetadata meta_;
	bool enabled_ = true;
};

/**
 * @brief Loads, orders (dependencies first) and runs plugins.
 *
 * Scanning: every `*.so` (Linux), `*.dylib` / `*.so` (macOS) or `*.dll`
 * (Windows) in the search directories that exports retdec_gui_plugin_v1.
 * A plugin is enabled when AppSettings' `plugins.enabledPlugins` is empty or
 * lists its id (the list passed to scanAndLoad()/loadPlugin()).
 *
 * Listeners replace the Qt signals; they run synchronously on the caller's
 * thread. onAboutToUnload runs while the plugin is still loaded (tear down
 * anything that came from it there); onUnloaded after it is gone.
 */
class PluginManager {
public:
	static PluginManager& instance();

	PluginManager() = default;
	~PluginManager();
	PluginManager(const PluginManager&) = delete;
	PluginManager& operator=(const PluginManager&) = delete;

	/// Scan @p dirs (default: AppSettings plugins.searchPaths) and load every plugin found.
	/// Returns how many loaded.
	int scanAndLoad(const std::vector<std::string>& dirs = {});

	/// Load one library. Returns the plugin id.
	std::expected<std::string, std::string> loadPlugin(const std::filesystem::path& filePath);

	/// Register a descriptor that is already in this process (built-in plugins, tests).
	std::expected<std::string, std::string>
	registerPlugin(const RetdecGuiPluginV1* api, std::string label = "<built-in>");

	/// Shut down and unload a plugin by id (no-op when unknown).
	void unloadPlugin(const std::string& pluginId);
	/// Unload everything (reverse dependency order).
	void unloadAll();

	/// Enable or disable a plugin. Disabled plugins are skipped by the run / list calls below.
	void setEnabled(const std::string& pluginId, bool enabled);

	/// All loaded plugins in dependency order.
	const std::vector<std::unique_ptr<Plugin>>& plugins() const
	{
		return plugins_;
	}
	const Plugin* findPlugin(const std::string& id) const;

	/// Enabled plugins of a kind, in dependency order.
	std::vector<const Plugin*> decompilerPlugins() const;
	std::vector<const Plugin*> outputPlugins() const;
	std::vector<const Plugin*> visualisationPlugins() const;
	std::vector<const Plugin*> analysisPlugins() const;

	/// Run every enabled DECOMPILER / ANALYSIS plugin on @p ctx, in order.
	void runDecompilerPlugins(PipelineContext& ctx) const;
	void runAnalysisPlugins(PipelineContext& ctx) const;

	/// Ids allowed to be enabled (empty = all). Defaults to AppSettings plugins.enabledPlugins.
	void setEnabledList(std::optional<std::vector<std::string>> ids)
	{
		enabledList_ = std::move(ids);
	}

	// Listeners
	std::vector<std::function<void(const std::string& id)>> onLoaded;
	std::vector<std::function<void(const std::string& id)>> onAboutToUnload;
	std::vector<std::function<void(const std::string& id)>> onUnloaded;
	std::vector<std::function<void(const std::filesystem::path& file, const std::string& message)>> onLoadError;

private:
	std::expected<std::string, std::string>
	add(const RetdecGuiPluginV1* api, std::shared_ptr<void> library, const std::filesystem::path& file);
	std::unexpected<std::string> loadError(const std::filesystem::path& file, std::string msg);
	bool enabledByDefault(const std::string& id) const;
	void sortByDependencies();
	std::vector<const Plugin*> enabledOfKind(bool (Plugin::*pred)() const) const;

	std::vector<std::unique_ptr<Plugin>> plugins_;
	std::optional<std::vector<std::string>> enabledList_;
};

} // namespace retdec::gui::model

#endif // __cplusplus

#endif // RETDEC_GUI_MODEL_PLUGINS_H
