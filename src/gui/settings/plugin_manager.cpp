/**
 * @file src/gui/settings/plugin_manager.cpp
 * @brief PluginManager implementation.
 */

#include <memory>
#include "retdec/gui/settings/plugin_manager.h"
#include "retdec/gui/settings/settings.h"

#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace retdec::gui {

// ─── Singleton ───────────────────────────────────────────────────────────────

PluginManager& PluginManager::instance() {
    static PluginManager m;
    return m;
}

PluginManager::PluginManager() {}

// ─── loadPlugin ───────────────────────────────────────────────────────────────

bool PluginManager::validateApiVersion(const QString& filePath,
                                        QPluginLoader& loader) const {
    (void)loader;
    // Use QLibrary to resolve the version symbol without full load
    QLibrary lib(filePath);
    if (!lib.load()) return true;  // Assume ok if we can't check
    using VersionFn = const char*(*)();
    auto vfn = reinterpret_cast<VersionFn>(lib.resolve("retdec_plugin_api_version"));
    if (vfn) {
        QString ver = QString::fromLatin1(vfn());
        lib.unload();
        return ver == RETDEC_PLUGIN_API_VERSION;
    }
    lib.unload();
    return true;
}

bool PluginManager::loadPlugin(const QString& filePath) {
    QFileInfo fi(filePath);
    if (!fi.exists()) {
        emit loadError(filePath, "File not found");
        return false;
    }

    auto loader = std::make_unique<QPluginLoader>(filePath);

    if (!validateApiVersion(filePath, *loader)) {
        emit loadError(filePath, "API version mismatch");
        return false;
    }

    QObject* obj = loader->instance();
    if (!obj) {
        emit loadError(filePath, loader->errorString());
        return false;
    }

    auto* plugin = dynamic_cast<IRetDecPlugin*>(obj);
    if (!plugin) {
        loader->unload();
        emit loadError(filePath, "Not an IRetDecPlugin");
        return false;
    }

    auto meta = plugin->metadata();
    if (!plugin->initialize()) {
        loader->unload();
        emit loadError(filePath, "Plugin initialization failed: " + meta.id);
        return false;
    }

    // Check for duplicates
    for (const auto& p : plugins_) {
        if (p.meta.id == meta.id) {
            loader->unload();
            emit loadError(filePath, "Plugin already loaded: " + meta.id);
            return false;
        }
    }

    LoadedPlugin lp;
    lp.meta     = meta;
    lp.instance = plugin;
    lp.loader   = std::move(loader);
    lp.filePath = filePath;

    // Check if this plugin is in the enabled list
    const auto& enabledList = AppSettings::instance().plugins.enabledPlugins;
    lp.enabled = enabledList.isEmpty() || enabledList.contains(meta.id);

    plugins_.push_back(std::move(lp));
    sortByDependencies();

    emit pluginLoaded(meta.id);
    return true;
}

// ─── scanAndLoad ─────────────────────────────────────────────────────────────

void PluginManager::scanAndLoad(const QStringList& paths) {
    QStringList searchPaths = paths.isEmpty() ?
        AppSettings::instance().plugins.searchPaths : paths;

    for (const auto& dirPath : searchPaths) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;

        QStringList filters;
#ifdef Q_OS_WIN
        filters << "*.dll";
#elif defined(Q_OS_MAC)
        filters << "*.dylib" << "*.so";
#else
        filters << "*.so";
#endif
        const auto entries = dir.entryInfoList(filters, QDir::Files);
        for (const auto& fi : entries)
            loadPlugin(fi.absoluteFilePath());
    }
}

// ─── unloadPlugin ────────────────────────────────────────────────────────────

void PluginManager::unloadPlugin(const QString& pluginId) {
    for (auto it = plugins_.begin(); it != plugins_.end(); ++it) {
        if (it->meta.id == pluginId) {
            it->instance->shutdown();
            if (it->loader) it->loader->unload();
            emit pluginUnloaded(pluginId);
            plugins_.erase(it);
            return;
        }
    }
}

// ─── setEnabled ──────────────────────────────────────────────────────────────

void PluginManager::setEnabled(const QString& pluginId, bool enabled) {
    for (auto& p : plugins_)
        if (p.meta.id == pluginId) { p.enabled = enabled; return; }
}

// ─── findPlugin ──────────────────────────────────────────────────────────────

const LoadedPlugin* PluginManager::findPlugin(const QString& id) const {
    for (const auto& p : plugins_)
        if (p.meta.id == id) return &p;
    return nullptr;
}

// ─── runDecompilerPlugins ────────────────────────────────────────────────────

void PluginManager::runDecompilerPlugins(PipelineContext& ctx) const {
    for (const auto& p : plugins_) {
        if (!p.enabled) continue;
        auto* dp = dynamic_cast<IDecompilerPlugin*>(p.instance);
        if (dp) dp->runStage(ctx);
    }
}

void PluginManager::runAnalysisPlugins(PipelineContext& ctx) const {
    for (const auto& p : plugins_) {
        if (!p.enabled) continue;
        auto* ap = dynamic_cast<IAnalysisPlugin*>(p.instance);
        if (ap) ap->analyse(ctx);
    }
}

// ─── sortByDependencies (topological sort) ────────────────────────────────────

void PluginManager::sortByDependencies() {
    // Build adjacency: dep → plugin
    std::unordered_map<QString, int> idToIdx;
    for (int i = 0; i < static_cast<int>(plugins_.size()); ++i)
        idToIdx[plugins_[i].meta.id] = i;

    std::vector<int>  order;
    std::unordered_set<int> visited;
    std::function<void(int)> visit = [&](int i) {
        if (visited.count(i)) return;
        visited.insert(i);
        for (const auto& dep : plugins_[i].meta.dependencies) {
            auto it = idToIdx.find(dep);
            if (it != idToIdx.end()) visit(it->second);
        }
        order.push_back(i);
    };

    for (int i = 0; i < static_cast<int>(plugins_.size()); ++i)
        visit(i);

    std::vector<LoadedPlugin> sorted;
    sorted.reserve(plugins_.size());
    for (int idx : order)
        sorted.push_back(std::move(plugins_[idx]));
    plugins_ = std::move(sorted);
}

} // namespace retdec::gui
