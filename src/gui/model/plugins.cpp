/**
 * @file src/gui/model/plugins.cpp
 * @brief Plugin loading through forge::os::dynlib and the C ABI wrappers.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/plugins.h"

#include "retdec/gui/model/settings.h"

#include <forge/os/dynlib.hpp>

#include <algorithm>
#include <format>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace retdec::gui::model {

namespace fs = std::filesystem;

namespace {

std::string s(const char* p)
{
	return p ? std::string(p) : std::string{};
}

/// Pipeline context as the C callbacks see it (borrowing @p ctx's strings).
RetdecGuiPipelineContext cContext(const PipelineContext& ctx)
{
	RetdecGuiPipelineContext c{};
	c.input_binary_path = ctx.inputBinaryPath.c_str();
	c.decompiled_text = ctx.decompiledText.c_str();
	c.ir_text = ctx.irText.c_str();
	c.asm_text = ctx.asmText.c_str();
	c.analysis_complete = ctx.analysisComplete ? 1 : 0;
	return c;
}

bool isPluginFileName(const fs::path& p)
{
	const std::string ext = p.extension().string();
#if defined(_WIN32)
	return ext == ".dll" || ext == ".DLL";
#elif defined(__APPLE__)
	return ext == ".dylib" || ext == ".so";
#else
	return ext == ".so";
#endif
}

} // namespace

// ─── Descriptor helpers ──────────────────────────────────────────────────────

std::expected<void, std::string> validatePluginDescriptor(const RetdecGuiPluginV1* api)
{
	if (!api) return std::unexpected(std::string("retdec_gui_plugin_v1() returned NULL"));
	if (api->abi_version != RETDEC_GUI_PLUGIN_ABI_VERSION)
		return std::unexpected(
			std::format("API version mismatch (plugin {}, host {})", api->abi_version, RETDEC_GUI_PLUGIN_ABI_VERSION));
	if (api->struct_size < sizeof(RetdecGuiPluginV1))
		return std::unexpected(
			std::format("Descriptor too small ({} bytes, need {})", api->struct_size, sizeof(RetdecGuiPluginV1)));
	if (!api->id || !*api->id) return std::unexpected(std::string("Plugin has no id"));
	const bool returnsStrings = api->run_stage || api->transform || api->render_panel || api->analyse || api->summary;
	if (returnsStrings && !api->free_string)
		return std::unexpected(std::format("Plugin {} returns strings but has no free_string", api->id));
	return {};
}

PluginMetadata pluginMetadataFrom(const RetdecGuiPluginV1& api)
{
	PluginMetadata m;
	m.id = s(api.id);
	m.name = s(api.name);
	m.version = s(api.version);
	m.description = s(api.description);
	m.author = s(api.author);
	m.apiVersion = RETDEC_PLUGIN_API_VERSION;
	if (api.dependencies)
		for (const char* const* d = api.dependencies; *d; ++d)
			if (**d) m.dependencies.emplace_back(*d);
	m.kinds = api.kinds;
	return m;
}

// ─── Plugin ──────────────────────────────────────────────────────────────────

Plugin::Plugin(const RetdecGuiPluginV1* api, std::shared_ptr<void> library, fs::path filePath):
	api_(api), library_(std::move(library)), filePath_(std::move(filePath)), meta_(pluginMetadataFrom(*api))
{}

std::optional<std::string> Plugin::take(char* p) const
{
	if (!p) return std::nullopt;
	std::string out(p);
	if (api_->free_string) api_->free_string(p);
	return out;
}

std::string Plugin::insertAfterStage() const
{
	return s(api_->insert_after_stage);
}

void Plugin::runStage(PipelineContext& ctx) const
{
	if (!api_->run_stage) return;
	const RetdecGuiPipelineContext c = cContext(ctx);
	if (auto out = take(api_->run_stage(&c))) ctx.decompiledText = std::move(*out);
}

std::string Plugin::formatName() const
{
	return s(api_->format_name);
}

std::string Plugin::fileExtension() const
{
	return s(api_->file_extension);
}

std::expected<std::string, std::string> Plugin::transform(std::string_view decompiledC) const
{
	if (!api_->transform) return std::unexpected(std::format("Plugin {} has no transform", meta_.id));
	const std::string in(decompiledC); // NUL-terminated copy
	if (auto out = take(api_->transform(in.c_str(), in.size()))) return std::move(*out);
	return std::unexpected(std::format("Plugin {} could not transform the output", meta_.id));
}

std::string Plugin::panelTitle() const
{
	return s(api_->panel_title);
}

std::string Plugin::renderPanel(const PipelineContext& ctx) const
{
	if (!api_->render_panel) return {};
	const RetdecGuiPipelineContext c = cContext(ctx);
	return take(api_->render_panel(&c)).value_or(std::string{});
}

void Plugin::analyse(PipelineContext& ctx) const
{
	if (!api_->analyse) return;
	const RetdecGuiPipelineContext c = cContext(ctx);
	if (auto out = take(api_->analyse(&c))) ctx.decompiledText = std::move(*out);
}

std::string Plugin::summary() const
{
	if (!api_->summary) return {};
	return take(api_->summary()).value_or(std::string{});
}

// ─── PluginManager ───────────────────────────────────────────────────────────

PluginManager& PluginManager::instance()
{
	static PluginManager m;
	return m;
}

PluginManager::~PluginManager()
{
	unloadAll();
}

std::unexpected<std::string> PluginManager::loadError(const fs::path& file, std::string msg)
{
	for (const auto& fn: onLoadError)
		if (fn) fn(file, msg);
	return std::unexpected(std::move(msg));
}

bool PluginManager::enabledByDefault(const std::string& id) const
{
	const std::vector<std::string>& list =
		enabledList_ ? *enabledList_ : AppSettings::instance().plugins.enabledPlugins;
	return list.empty() || std::find(list.begin(), list.end(), id) != list.end();
}

std::expected<std::string, std::string>
PluginManager::add(const RetdecGuiPluginV1* api, std::shared_ptr<void> library, const fs::path& file)
{
	if (auto v = validatePluginDescriptor(api); !v) return loadError(file, v.error());
	const std::string id = api->id;
	if (findPlugin(id)) return loadError(file, "Plugin already loaded: " + id);
	if (api->initialize && api->initialize() == 0) return loadError(file, "Plugin initialization failed: " + id);

	auto plugin = std::make_unique<Plugin>(api, std::move(library), file);
	plugin->setEnabled(enabledByDefault(id));
	plugins_.push_back(std::move(plugin));
	sortByDependencies();

	for (const auto& fn: onLoaded)
		if (fn) fn(id);
	return id;
}

std::expected<std::string, std::string> PluginManager::loadPlugin(const fs::path& filePath)
{
	std::error_code ec;
	if (!fs::is_regular_file(filePath, ec)) return loadError(filePath, "File not found");

	const fs::path abs = fs::absolute(filePath, ec).lexically_normal();
	auto lib = forge::os::dynlib::open(abs.string());
	if (!lib) return loadError(filePath, lib.err.what.empty() ? std::string("Cannot load library") : lib.err.what);

	auto shared = std::make_shared<forge::os::dynlib>(std::move(lib.value));
	auto entry = shared->get<RetdecGuiPluginEntryV1>(RETDEC_GUI_PLUGIN_ENTRY_SYMBOL);
	if (!entry)
		return loadError(filePath, std::string("Not a retdec-gui plugin (no ") + RETDEC_GUI_PLUGIN_ENTRY_SYMBOL + ")");
	return add(entry(), std::shared_ptr<void>(shared, shared.get()), abs);
}

std::expected<std::string, std::string> PluginManager::registerPlugin(const RetdecGuiPluginV1* api, std::string label)
{
	return add(api, nullptr, fs::path(std::move(label)));
}

int PluginManager::scanAndLoad(const std::vector<std::string>& dirs)
{
	const std::vector<std::string>& searchPaths = dirs.empty() ? AppSettings::instance().plugins.searchPaths : dirs;
	int loaded = 0;
	for (const std::string& dirPath: searchPaths)
	{
		std::error_code ec;
		if (!fs::is_directory(dirPath, ec)) continue;
		std::vector<fs::path> files;
		for (fs::directory_iterator it(dirPath, ec), end; !ec && it != end; it.increment(ec))
			if (it->is_regular_file(ec) && isPluginFileName(it->path())) files.push_back(it->path());
		std::sort(files.begin(), files.end());
		for (const fs::path& f: files)
			if (loadPlugin(f)) ++loaded;
	}
	return loaded;
}

void PluginManager::unloadPlugin(const std::string& pluginId)
{
	const auto find = [this, &pluginId] {
		return std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& p) {
			return p->metadata().id == pluginId;
		});
	};
	if (find() == plugins_.end()) return;

	// Whatever the plugin created must be torn down while it is still loaded.
	for (const auto& fn: onAboutToUnload)
		if (fn) fn(pluginId);

	// A listener may have changed the list: look it up again.
	auto it = find();
	if (it == plugins_.end()) return;
	if ((*it)->api().shutdown) (*it)->api().shutdown();

	// Take it out of the list first, then let it (and its library) go, then
	// tell anyone who only needs to know it is gone.
	std::unique_ptr<Plugin> gone = std::move(*it);
	plugins_.erase(it);
	gone.reset();

	for (const auto& fn: onUnloaded)
		if (fn) fn(pluginId);
}

void PluginManager::unloadAll()
{
	while (!plugins_.empty())
		unloadPlugin(plugins_.back()->metadata().id);
}

void PluginManager::setEnabled(const std::string& pluginId, bool enabled)
{
	for (auto& p: plugins_)
		if (p->metadata().id == pluginId)
		{
			p->setEnabled(enabled);
			return;
		}
}

const Plugin* PluginManager::findPlugin(const std::string& id) const
{
	for (const auto& p: plugins_)
		if (p->metadata().id == id) return p.get();
	return nullptr;
}

std::vector<const Plugin*> PluginManager::enabledOfKind(bool (Plugin::*pred)() const) const
{
	std::vector<const Plugin*> out;
	for (const auto& p: plugins_)
		if (p->enabled() && ((*p).*pred)()) out.push_back(p.get());
	return out;
}

std::vector<const Plugin*> PluginManager::decompilerPlugins() const
{
	return enabledOfKind(&Plugin::isDecompiler);
}

std::vector<const Plugin*> PluginManager::outputPlugins() const
{
	return enabledOfKind(&Plugin::isOutput);
}

std::vector<const Plugin*> PluginManager::visualisationPlugins() const
{
	return enabledOfKind(&Plugin::isVisualisation);
}

std::vector<const Plugin*> PluginManager::analysisPlugins() const
{
	return enabledOfKind(&Plugin::isAnalysis);
}

void PluginManager::runDecompilerPlugins(PipelineContext& ctx) const
{
	for (const Plugin* p: decompilerPlugins())
		p->runStage(ctx);
}

void PluginManager::runAnalysisPlugins(PipelineContext& ctx) const
{
	for (const Plugin* p: analysisPlugins())
		p->analyse(ctx);
}

void PluginManager::sortByDependencies()
{
	// Depth-first topological order: a plugin comes after the plugins it
	// depends on; unknown dependencies are ignored, cycles broken by visit order.
	std::unordered_map<std::string, std::size_t> idToIdx;
	for (std::size_t i = 0; i < plugins_.size(); ++i)
		idToIdx[plugins_[i]->metadata().id] = i;

	std::vector<std::size_t> order;
	std::unordered_set<std::size_t> visited;
	std::function<void(std::size_t)> visit = [&](std::size_t i) {
		if (!visited.insert(i).second) return;
		for (const std::string& dep: plugins_[i]->metadata().dependencies)
			if (auto it = idToIdx.find(dep); it != idToIdx.end()) visit(it->second);
		order.push_back(i);
	};
	for (std::size_t i = 0; i < plugins_.size(); ++i)
		visit(i);

	std::vector<std::unique_ptr<Plugin>> sorted;
	sorted.reserve(plugins_.size());
	for (std::size_t idx: order)
		sorted.push_back(std::move(plugins_[idx]));
	plugins_ = std::move(sorted);
}

} // namespace retdec::gui::model
