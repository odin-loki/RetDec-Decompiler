/**
 * @file src/gui/model/dlg_settings_form.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/dlg_settings_form.h"

#include "retdec/gui/model/decompiler_launch.h"
#include "retdec/gui/model/dlg_input.h"

#include <filesystem>
#include <format>
#include <set>
#include <system_error>

namespace retdec::gui::model {

namespace fs = std::filesystem;

const std::vector<std::string>& settingsThemeItems()
{
	static const std::vector<std::string> k{"Dark", "Light", "System Default"};
	return k;
}

const std::vector<ComboEntry>& settingsLanguageItems()
{
	static const std::vector<ComboEntry> k{
		{"English (en)", "en"}, {"German (de)", "de"}, {"French (fr)", "fr"}, {"Spanish (es)", "es"}, {"Chinese (zh)", "zh"}};
	return k;
}

static int indexOfValue(const std::vector<ComboEntry>& items, std::string_view v, int fallback)
{
	for (std::size_t i = 0; i < items.size(); ++i)
		if (items[i].value == v) return int(i);
	return fallback;
}

int settingsLanguageIndex(std::string_view code)
{
	return indexOfValue(settingsLanguageItems(), code, 0);
}

std::string settingsLanguageCode(int index)
{
	const auto& k = settingsLanguageItems();
	return (index >= 0 && std::size_t(index) < k.size()) ? k[std::size_t(index)].value : std::string("en");
}

const std::vector<std::string>& settingsQuantItems()
{
	static const std::vector<std::string> k{"Q4_0", "Q4_K_M", "Q5_K_M", "Q6_K", "F16", "F32"};
	return k;
}

const std::vector<std::string>& settingsInferenceDeviceItems()
{
	static const std::vector<std::string> k{"CPU", "GPU", "Auto"};
	return k;
}

const std::vector<std::string>& settingsVerbosityItems()
{
	static const std::vector<std::string> k{"Quiet", "Normal", "Verbose", "Debug"};
	return k;
}

const std::vector<ComboEntry>& settingsProfileItems()
{
	static const std::vector<ComboEntry> k{{"Fast", "fast"}, {"Balanced", "balanced"}, {"Quality", "quality"}};
	return k;
}

int settingsProfileIndex(std::string_view value)
{
	return indexOfValue(settingsProfileItems(), value, 1);
}

const std::vector<ComboEntry>& settingsOutputLangItems()
{
	static const std::vector<ComboEntry> k{
		{"C", "c"}, {"Python", "python"}, {"C#", "csharp"}, {"Java", "java"}, {"WAT", "wat"}};
	return k;
}

int settingsOutputLangIndex(std::string_view value)
{
	return indexOfValue(settingsOutputLangItems(), value, 0);
}

const std::vector<std::string>& settingsCudaDeviceItems()
{
	static const std::vector<std::string> k{"Auto (default)"};
	return k;
}

std::vector<std::string> settingsEditorFontFamilies(std::string_view current)
{
	std::vector<std::string> out;
	std::set<std::string> seen;
	auto add = [&](std::string s) {
		if (s.empty() || seen.contains(s)) return;
		seen.insert(s);
		out.push_back(std::move(s));
	};
	add(std::string(dlgTrim(current)));
	for (const char* f: {"Cascadia Code,JetBrains Mono,Consolas",
						 "JetBrains Mono",
						 "Cascadia Code",
						 "Consolas",
						 "Fira Code",
						 "Source Code Pro",
						 "DejaVu Sans Mono",
						 "Menlo",
						 "Monospace"})
		add(f);
	return out;
}

std::expected<std::vector<std::string>, std::string> settingsLlvmPassNames()
{
	auto all = loadLlvmPassPreset(false);
	if (!all || all->empty())
		return std::unexpected(std::string("Could not load pass list resource (llvm_passes_default.json)."));
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (std::string& n: *all)
		if (seen.insert(n).second) out.push_back(std::move(n));
	return out;
}

std::vector<std::string> settingsDisabledPasses(const std::vector<std::string>& names, const std::vector<bool>& checked)
{
	std::vector<std::string> out;
	for (std::size_t i = 0; i < names.size() && i < checked.size(); ++i)
		if (!checked[i]) out.push_back(names[i]);
	return out;
}

std::expected<void, std::string> validateSettingsForApply(const DecompilerSettings& d)
{
	const std::string dir(dlgTrim(d.decompileOutputDir));
	if (!dir.empty())
	{
		std::error_code ec;
		const fs::path p(dir);
		if (!fs::is_directory(p, ec))
		{
			ec.clear();
			fs::create_directories(p, ec);
			if (ec || !fs::is_directory(p, ec))
				return std::unexpected(std::format("Decompile output directory cannot be created:\n{}", dir));
		}
	}
	const std::string cfg(dlgTrim(d.extraConfigPath));
	if (!cfg.empty())
	{
		std::error_code ec;
		if (!fs::exists(fs::path(cfg), ec))
			return std::unexpected(std::format("Decompiler configuration file does not exist:\n{}", cfg));
	}
	return {};
}

std::string pluginRowLabel(const PluginMetadata& m)
{
	return std::format("{}  v{}", m.name, m.version);
}

std::string pluginDetailText(const PluginMetadata& m, const std::string& filePath)
{
	return std::format("{} v{} — {}\nAuthor: {}\nFile: {}", m.name, m.version, m.description, m.author, filePath);
}

std::string_view pluginLibraryFilter()
{
#if defined(_WIN32)
	return "*.dll";
#elif defined(__APPLE__)
	return "*.dylib;*.so";
#else
	return "*.so";
#endif
}

std::vector<std::string> settingsNonEmptyTrimmed(const std::vector<std::string>& in)
{
	std::vector<std::string> out;
	for (const std::string& s: in)
		if (const std::string_view t = dlgTrim(s); !t.empty()) out.emplace_back(t);
	return out;
}

} // namespace retdec::gui::model
