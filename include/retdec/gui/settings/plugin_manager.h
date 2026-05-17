/**
 * @file include/retdec/gui/settings/plugin_manager.h
 * @brief Plugin manager — loads, manages, and queries RetDec plugins.
 */

#ifndef RETDEC_GUI_SETTINGS_PLUGIN_MANAGER_H
#define RETDEC_GUI_SETTINGS_PLUGIN_MANAGER_H

#include "retdec/gui/settings/plugin_interface.h"

#include <QObject>
#include <QPluginLoader>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace retdec::gui {

struct LoadedPlugin {
    PluginMetadata        meta;
    IRetDecPlugin*        instance    = nullptr;
    std::unique_ptr<QPluginLoader> loader;
    bool                  enabled     = true;
    QString               filePath;
};

/**
 * @brief Manages the lifecycle of all RetDec plugins.
 *
 * Scanning: searches all configured plugin directories for shared libraries
 *           whose symbols include `retdec_create_plugin`.
 * Versioning: checks `retdec_plugin_api_version()` against RETDEC_PLUGIN_API_VERSION.
 * Dependency ordering: topological sort ensures plugins are initialised after
 *                       their declared dependencies.
 *
 * Signals:
 *   pluginLoaded(id)    — after successful plugin load and init
 *   pluginUnloaded(id)  — after plugin shutdown and unload
 *   loadError(path, msg) — when a plugin fails to load
 */
class PluginManager : public QObject {
    Q_OBJECT
public:
    static PluginManager& instance();

    /**
     * @brief Scan all search paths for plugins and load them.
     * @param paths Directories to scan. If empty, uses AppSettings paths.
     */
    void scanAndLoad(const QStringList& paths = QStringList{});

    /**
     * @brief Load a single plugin from an explicit file path.
     */
    bool loadPlugin(const QString& filePath);

    /**
     * @brief Unload a plugin by its ID.
     */
    void unloadPlugin(const QString& pluginId);

    /**
     * @brief Enable or disable a plugin.  Disabled plugins are skipped when
     *        running pipeline stages or building menus.
     */
    void setEnabled(const QString& pluginId, bool enabled);

    /**
     * @brief All loaded plugins in dependency order.
     */
    const std::vector<LoadedPlugin>& plugins() const { return plugins_; }

    /**
     * @brief Loaded plugins of a specific type.
     */
    template<typename T>
    std::vector<T*> pluginsOfType() const {
        std::vector<T*> result;
        for (const auto& p : plugins_) {
            if (!p.enabled) continue;
            auto* typed = dynamic_cast<T*>(p.instance);
            if (typed) result.push_back(typed);
        }
        return result;
    }

    /**
     * @brief Run all IDecompilerPlugin stages on the given context.
     */
    void runDecompilerPlugins(PipelineContext& ctx) const;

    /**
     * @brief Run all IAnalysisPlugin passes on the given context.
     */
    void runAnalysisPlugins(PipelineContext& ctx) const;

    /**
     * @brief Find a loaded plugin by ID.
     */
    const LoadedPlugin* findPlugin(const QString& id) const;

signals:
    void pluginLoaded  (const QString& id);
    void pluginUnloaded(const QString& id);
    void loadError     (const QString& filePath, const QString& msg);

private:
    PluginManager();
    ~PluginManager() = default;
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    std::vector<LoadedPlugin> plugins_;

    bool validateApiVersion(const QString& filePath, QPluginLoader& loader) const;
    void sortByDependencies();
};

} // namespace retdec::gui

#endif // RETDEC_GUI_SETTINGS_PLUGIN_MANAGER_H
