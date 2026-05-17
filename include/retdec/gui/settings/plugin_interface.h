/**
 * @file include/retdec/gui/settings/plugin_interface.h
 * @brief Plugin System interface — Qt GUI Stage 57.
 *
 * RetDec plugins are shared libraries (`.so`/`.dll`) that export a factory
 * function conforming to the `IRetDecPlugin` interface.  Each plugin is loaded
 * with `QPluginLoader` at startup or on demand.
 *
 * ## Plugin types supported
 *
 *   IDecompilerPlugin   — adds or replaces a decompilation pipeline stage
 *   IOutputPlugin       — adds a new output format emitter
 *   IVisualisationPlugin — adds a new GUI panel
 *   IAnalysisPlugin     — post-processing analysis pass on the decompiled AST
 *
 * ## ABI conventions
 *
 *   Plugins must export:
 *     extern "C" IRetDecPlugin* retdec_create_plugin();
 *     extern "C" void           retdec_destroy_plugin(IRetDecPlugin*);
 *     extern "C" const char*    retdec_plugin_api_version();
 *
 *   `retdec_plugin_api_version()` must return RETDEC_PLUGIN_API_VERSION.
 *
 * ## Metadata
 *
 *   Each plugin provides a `PluginMetadata` struct via `metadata()`.
 *
 * ## Example
 *
 *   class MyPlugin : public IDecompilerPlugin {
 *   public:
 *       PluginMetadata metadata() const override { return {"MyPlugin","1.0","..."}; }
 *       void runStage(PipelineContext& ctx) override { ... }
 *   };
 *   RETDEC_EXPORT_PLUGIN(MyPlugin)
 */

#ifndef RETDEC_GUI_SETTINGS_PLUGIN_INTERFACE_H
#define RETDEC_GUI_SETTINGS_PLUGIN_INTERFACE_H

#include <QObject>
#include <QString>
#include <QWidget>
#include <QtPlugin>

namespace retdec::gui {

#define RETDEC_PLUGIN_API_VERSION "1.0"

// ─── Plugin metadata ─────────────────────────────────────────────────────────

struct PluginMetadata {
    QString id;           ///< unique identifier, e.g. "com.example.myplugin"
    QString name;
    QString version;
    QString description;
    QString author;
    QString apiVersion = RETDEC_PLUGIN_API_VERSION;
    QStringList dependencies;  ///< other plugin IDs required
};

// ─── Plugin context ───────────────────────────────────────────────────────────

/**
 * @brief Minimal context passed to pipeline-stage plugins.
 *
 * Plugins can read the current decompiled function text and modify it, or
 * append additional information to the output.
 */
struct PipelineContext {
    QString inputBinaryPath;
    QString decompiledText;    ///< current decompiled output (may be modified)
    QString irText;            ///< SSA IR text
    QString asmText;           ///< assembly text
    bool    analysisComplete = false;
};

// ─── Base plugin interface ────────────────────────────────────────────────────

/**
 * @brief Base class for all RetDec plugins.
 */
class IRetDecPlugin {
public:
    virtual ~IRetDecPlugin() = default;

    virtual PluginMetadata metadata() const = 0;

    /**
     * @brief Called once after the plugin is loaded.
     *        Return false to abort loading.
     */
    virtual bool initialize() { return true; }

    /**
     * @brief Called before the plugin is unloaded.
     */
    virtual void shutdown() {}
};

// ─── Decompiler pipeline plugin ───────────────────────────────────────────────

/**
 * @brief Plugin that inserts or replaces a decompilation pipeline stage.
 */
class IDecompilerPlugin : public IRetDecPlugin {
public:
    /**
     * @brief Stage insertion position (before or after a named built-in stage).
     *        Empty string = append at end.
     */
    virtual QString insertAfterStage() const { return {}; }

    /**
     * @brief Run the custom decompilation stage.
     */
    virtual void runStage(PipelineContext& ctx) = 0;
};

// ─── Output format plugin ─────────────────────────────────────────────────────

/**
 * @brief Plugin that adds a new output format.
 *
 * The plugin is shown in the "Export As" menu of the main window.
 */
class IOutputPlugin : public IRetDecPlugin {
public:
    /**
     * @brief Human-readable format name, e.g. "Rust (experimental)".
     */
    virtual QString formatName() const = 0;

    /**
     * @brief File extension, e.g. ".rs".
     */
    virtual QString fileExtension() const = 0;

    /**
     * @brief Transform the decompiled C output to the new format.
     */
    virtual QString transform(const QString& decompiledC) = 0;
};

// ─── Visualisation plugin ─────────────────────────────────────────────────────

/**
 * @brief Plugin that adds a new dockable panel to the main window.
 */
class IVisualisationPlugin : public IRetDecPlugin {
public:
    /**
     * @brief Create and return the panel widget.
     *        The main window takes ownership.
     */
    virtual QWidget* createPanel(QWidget* parent) = 0;

    /**
     * @brief Panel title for the dock widget.
     */
    virtual QString panelTitle() const = 0;
};

// ─── Analysis plugin ─────────────────────────────────────────────────────────

/**
 * @brief Plugin that performs post-processing analysis on the decompiled AST.
 */
class IAnalysisPlugin : public IRetDecPlugin {
public:
    /**
     * @brief Run analysis on the given context.
     *        May annotate decompiledText or add information to ctx.
     */
    virtual void analyse(PipelineContext& ctx) = 0;

    /**
     * @brief Short summary of analysis results (for status bar).
     */
    virtual QString summary() const { return {}; }
};

// ─── Plugin export macro ──────────────────────────────────────────────────────

/**
 * @brief Convenience macro to export a plugin class.
 *
 * Usage:
 *   RETDEC_EXPORT_PLUGIN(MyPluginClass)
 *
 * This generates the three required C-linkage functions.
 */
#define RETDEC_EXPORT_PLUGIN(ClassName) \
    extern "C" ::retdec::gui::IRetDecPlugin* retdec_create_plugin() { \
        return new ClassName(); \
    } \
    extern "C" void retdec_destroy_plugin(::retdec::gui::IRetDecPlugin* p) { \
        delete p; \
    } \
    extern "C" const char* retdec_plugin_api_version() { \
        return ::retdec::gui::RETDEC_PLUGIN_API_VERSION; \
    }

} // namespace retdec::gui

#endif // RETDEC_GUI_SETTINGS_PLUGIN_INTERFACE_H
