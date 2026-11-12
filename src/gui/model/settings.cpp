/**
 * @file src/gui/model/settings.cpp
 * @brief AppSettings: JSON persistence in the per-user config directory.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/settings.h"

#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/users.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <format>

namespace retdec::gui::model {

namespace fs = std::filesystem;
namespace json = forge::json;

namespace {

// ─── String helpers ──────────────────────────────────────────────────────────

std::string trimmed(std::string_view s)
{
	std::size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
		++b;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
		--e;
	return std::string(s.substr(b, e - b));
}

std::string lower(std::string s)
{
	for (char& c: s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::vector<std::string> splitCommas(std::string_view s)
{
	std::vector<std::string> out;
	std::size_t start = 0;
	while (start <= s.size())
	{
		std::size_t comma = s.find(',', start);
		if (comma == std::string_view::npos) comma = s.size();
		out.push_back(std::string(s.substr(start, comma - start)));
		start = comma + 1;
	}
	return out;
}

std::optional<double> parseNumber(std::string_view s)
{
	const std::string t = trimmed(s);
	if (t.empty()) return std::nullopt;
	char* end = nullptr;
	const double v = std::strtod(t.c_str(), &end);
	if (end != t.c_str() + t.size()) return std::nullopt;
	return v;
}

// ─── JSON read helpers (missing / wrong type -> default, like QVariant) ─────

const json::value* group(const json::value& root, std::string_view name)
{
	const json::value* g = root.find(name);
	return (g && g->is_obj()) ? g : nullptr;
}

const json::value* member(const json::value* g, std::string_view key)
{
	return g ? g->find(key) : nullptr;
}

bool getBool(const json::value* g, std::string_view key, bool dflt)
{
	const json::value* v = member(g, key);
	if (!v) return dflt;
	if (v->is_bool()) return v->b();
	if (v->is_num()) return v->num() != 0.0;
	if (v->is_str())
	{
		const std::string s = lower(trimmed(v->str()));
		if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
		if (s == "false" || s == "0" || s == "no" || s == "off") return false;
	}
	return dflt;
}

double getDouble(const json::value* g, std::string_view key, double dflt)
{
	const json::value* v = member(g, key);
	if (!v) return dflt;
	if (v->is_num()) return v->num();
	if (v->is_str())
		if (auto d = parseNumber(v->str())) return *d;
	return dflt;
}

int getInt(const json::value* g, std::string_view key, int dflt)
{
	const json::value* v = member(g, key);
	if (!v) return dflt;
	if (v->is_num()) return static_cast<int>(v->num());
	if (v->is_str())
		if (auto d = parseNumber(v->str())) return static_cast<int>(*d);
	return dflt;
}

std::string getString(const json::value* g, std::string_view key, std::string dflt = {})
{
	const json::value* v = member(g, key);
	if (!v || !v->is_str()) return dflt;
	return v->str();
}

std::vector<std::string> getStringList(const json::value* g, std::string_view key)
{
	std::vector<std::string> out;
	const json::value* v = member(g, key);
	if (!v) return out;
	if (v->is_str())
	{
		if (!v->str().empty()) out.push_back(v->str());
		return out;
	}
	if (!v->is_arr()) return out;
	for (const json::value& e: v->arr())
		if (e.is_str()) out.push_back(e.str());
	return out;
}

template <class E>
E getEnum(const json::value* g, std::string_view key, E dflt)
{
	return static_cast<E>(getInt(g, key, static_cast<int>(dflt)));
}

FontSpec getFont(const json::value* g, std::string_view fontKey, const FontSpec& dflt)
{
	FontSpec f = dflt;
	if (const json::value* v = member(g, fontKey); v && v->is_str())
		f = FontSpec::fromQtString(v->str(), dflt.pointSize);
	if (const json::value* p = member(g, "fontPointSize"); p && p->is_num() && p->num() > 0)
		f.pointSize = static_cast<int>(p->num());
	return f;
}

json::value stringArray(const std::vector<std::string>& v)
{
	json::array a;
	a.reserve(v.size());
	for (const std::string& s: v)
		a.emplace_back(s);
	return a;
}

std::expected<json::value, std::string> parseJson(std::string_view text)
{
	try
	{
		return json::parse(text);
	}
	catch (const std::exception& e)
	{
		return std::unexpected(std::string(e.what()));
	}
}

std::expected<std::string, std::string> readText(const fs::path& path)
{
	auto r = forge::os::read_file(path.string());
	if (!r) return std::unexpected(std::format("Cannot read {}: {}", path.string(), r.err.what));
	return std::move(r.value);
}

// ─── Storage document (QSettings group/key names) ───────────────────────────

json::value storageDocument(const SettingsSections& s)
{
	json::object root;
	root.emplace_back(
		"General",
		json::object{
			{"theme", static_cast<int>(s.general.theme)},
			{"font", s.general.editorFont.family},
			{"fontPointSize", s.general.editorFont.pointSize},
			{"fontSize", s.general.fontSize},
			{"language", s.general.language},
			{"showLineNumbers", s.general.showLineNumbers},
			{"wordWrap", s.general.wordWrap},
			{"restoreSession", s.general.restoreSession},
			{"lastOpenDir", s.general.lastOpenDir},
			{"lastBinaryPath", s.general.lastBinaryPath},
		});
	root.emplace_back(
		"Analysis",
		json::object{
			{"enableTyping", s.analysis.enableTyping},
			{"enablePatternMatch", s.analysis.enablePatternMatch},
			{"enableConcurrency", s.analysis.enableConcurrency},
			{"enableCudaRecovery", s.analysis.enableCudaRecovery},
			{"enableSerialDetect", s.analysis.enableSerialDetect},
			{"enableModuleCluster", s.analysis.enableModuleCluster},
			{"enableCxxLifter", s.analysis.enableCxxLifter},
			{"minTypeConf", s.analysis.minTypeConfidence},
			{"minPatternConf", s.analysis.minPatternConfidence},
			{"minRecovConf", s.analysis.minRecoveryConfidence},
			{"maxTimeSecs", s.analysis.maxAnalysisTimeSecs},
			{"threadCount", s.analysis.threadCount},
		});
	root.emplace_back(
		"CUDA",
		json::object{
			{"deviceName", s.cuda.deviceName},
			{"deviceIndex", s.cuda.deviceIndex},
			{"kernelCacheDir", s.cuda.kernelCacheDir},
			{"enableProfiling", s.cuda.enableProfiling},
			{"useGPU", s.cuda.useGPU},
			{"blockSize", s.cuda.blockSize},
		});
	root.emplace_back(
		"ML",
		json::object{
			{"modelPath", s.ml.modelPath},
			{"quantLevel", static_cast<int>(s.ml.quantLevel)},
			{"inferenceDevice", static_cast<int>(s.ml.inferenceDevice)},
			{"temperature", s.ml.temperature},
			{"topP", s.ml.topP},
			{"topK", s.ml.topK},
			{"maxNewTokens", s.ml.maxNewTokens},
			{"contextLength", s.ml.contextLength},
			{"streamOutput", s.ml.streamOutput},
			{"modelSha256", s.ml.modelSha256},
			{"batchRefine", s.ml.batchRefine},
		});
	root.emplace_back(
		"Recovery",
		json::object{
			{"detectSTL", s.recovery.detectSTL},
			{"detectCrypto", s.recovery.detectCrypto},
			{"detectPatterns", s.recovery.detectPatterns},
			{"detectConcurrency", s.recovery.detectConcurrency},
			{"detectCuda", s.recovery.detectCuda},
			{"detectRTTI", s.recovery.detectRTTI},
			{"detectExceptions", s.recovery.detectExceptions},
			{"detectVirtual", s.recovery.detectVirtual},
			{"stlConf", s.recovery.stlConfidence},
			{"cryptoConf", s.recovery.cryptoConfidence},
			{"patternConf", s.recovery.patternConfidence},
			{"concurrConf", s.recovery.concurrencyConfidence},
		});
	root.emplace_back(
		"Advanced",
		json::object{
			{"verbosity", static_cast<int>(s.advanced.verbosity)},
			{"irDumpPath", s.advanced.irDumpPath},
			{"intermediateDir", s.advanced.intermediateDir},
			{"dumpIR", s.advanced.dumpIR},
			{"dumpASM", s.advanced.dumpASM},
			{"dumpCFG", s.advanced.dumpCFG},
			{"dumpSSA", s.advanced.dumpSSA},
			{"colorOutput", s.advanced.colorOutput},
			{"maxFunctions", s.advanced.maxFunctions},
			{"demangleNames", s.advanced.demangleNames},
		});
	root.emplace_back(
		"Plugins",
		json::object{
			{"searchPaths", stringArray(s.plugins.searchPaths)},
			{"enabledPlugins", stringArray(s.plugins.enabledPlugins)},
			{"autoLoad", s.plugins.autoLoadPlugins},
		});
	root.emplace_back(
		"Decompiler",
		json::object{
			{"useCustomLlvmPasses", s.decompiler.useCustomLlvmPasses},
			{"llvmPassesDisabled", stringArray(s.decompiler.llvmPassesDisabled)},
			{"extraConfigPath", s.decompiler.extraConfigPath},
			{"decompileOutputDir", s.decompiler.decompileOutputDir},
			{"liveConsoleTail", s.decompiler.liveConsoleTail},
			{"outputLang", s.decompiler.outputLang},
			{"decompileProfile", s.decompiler.decompileProfile},
		});
	return root;
}

SettingsSections fromStorageDocument(const json::value& root)
{
	SettingsSections out; // every key falls back to the compile-time default, as QSettings::value(key, dflt) did
	const SettingsSections d;

	if (const json::value* g = group(root, "General"))
	{
		out.general.theme = getEnum(g, "theme", d.general.theme);
		out.general.editorFont = getFont(g, "font", d.general.editorFont);
		out.general.fontSize = getInt(g, "fontSize", d.general.fontSize);
		out.general.language = getString(g, "language", d.general.language);
		out.general.showLineNumbers = getBool(g, "showLineNumbers", d.general.showLineNumbers);
		out.general.wordWrap = getBool(g, "wordWrap", d.general.wordWrap);
		out.general.restoreSession = getBool(g, "restoreSession", d.general.restoreSession);
		out.general.lastOpenDir = getString(g, "lastOpenDir");
		out.general.lastBinaryPath = getString(g, "lastBinaryPath");
	}
	if (const json::value* g = group(root, "Analysis"))
	{
		auto& a = out.analysis;
		a.enableTyping = getBool(g, "enableTyping", d.analysis.enableTyping);
		a.enablePatternMatch = getBool(g, "enablePatternMatch", d.analysis.enablePatternMatch);
		a.enableConcurrency = getBool(g, "enableConcurrency", d.analysis.enableConcurrency);
		a.enableCudaRecovery = getBool(g, "enableCudaRecovery", d.analysis.enableCudaRecovery);
		a.enableSerialDetect = getBool(g, "enableSerialDetect", d.analysis.enableSerialDetect);
		a.enableModuleCluster = getBool(g, "enableModuleCluster", d.analysis.enableModuleCluster);
		a.enableCxxLifter = getBool(g, "enableCxxLifter", d.analysis.enableCxxLifter);
		a.minTypeConfidence = getDouble(g, "minTypeConf", d.analysis.minTypeConfidence);
		a.minPatternConfidence = getDouble(g, "minPatternConf", d.analysis.minPatternConfidence);
		a.minRecoveryConfidence = getDouble(g, "minRecovConf", d.analysis.minRecoveryConfidence);
		a.maxAnalysisTimeSecs = getInt(g, "maxTimeSecs", d.analysis.maxAnalysisTimeSecs);
		a.threadCount = getInt(g, "threadCount", d.analysis.threadCount);
	}
	if (const json::value* g = group(root, "CUDA"))
	{
		auto& c = out.cuda;
		c.deviceName = getString(g, "deviceName");
		c.deviceIndex = getInt(g, "deviceIndex", d.cuda.deviceIndex);
		c.kernelCacheDir = getString(g, "kernelCacheDir");
		c.enableProfiling = getBool(g, "enableProfiling", d.cuda.enableProfiling);
		c.useGPU = getBool(g, "useGPU", d.cuda.useGPU);
		c.blockSize = getInt(g, "blockSize", d.cuda.blockSize);
	}
	if (const json::value* g = group(root, "ML"))
	{
		auto& m = out.ml;
		m.modelPath = getString(g, "modelPath");
		m.quantLevel = getEnum(g, "quantLevel", d.ml.quantLevel);
		m.inferenceDevice = getEnum(g, "inferenceDevice", d.ml.inferenceDevice);
		m.temperature = getDouble(g, "temperature", d.ml.temperature);
		m.topP = getDouble(g, "topP", d.ml.topP);
		m.topK = getInt(g, "topK", d.ml.topK);
		m.maxNewTokens = getInt(g, "maxNewTokens", d.ml.maxNewTokens);
		m.contextLength = getInt(g, "contextLength", d.ml.contextLength);
		m.streamOutput = getBool(g, "streamOutput", d.ml.streamOutput);
		m.modelSha256 = getString(g, "modelSha256");
		m.batchRefine = getBool(g, "batchRefine", d.ml.batchRefine);
	}
	if (const json::value* g = group(root, "Recovery"))
	{
		auto& r = out.recovery;
		r.detectSTL = getBool(g, "detectSTL", d.recovery.detectSTL);
		r.detectCrypto = getBool(g, "detectCrypto", d.recovery.detectCrypto);
		r.detectPatterns = getBool(g, "detectPatterns", d.recovery.detectPatterns);
		r.detectConcurrency = getBool(g, "detectConcurrency", d.recovery.detectConcurrency);
		r.detectCuda = getBool(g, "detectCuda", d.recovery.detectCuda);
		r.detectRTTI = getBool(g, "detectRTTI", d.recovery.detectRTTI);
		r.detectExceptions = getBool(g, "detectExceptions", d.recovery.detectExceptions);
		r.detectVirtual = getBool(g, "detectVirtual", d.recovery.detectVirtual);
		r.stlConfidence = getDouble(g, "stlConf", d.recovery.stlConfidence);
		r.cryptoConfidence = getDouble(g, "cryptoConf", d.recovery.cryptoConfidence);
		r.patternConfidence = getDouble(g, "patternConf", d.recovery.patternConfidence);
		r.concurrencyConfidence = getDouble(g, "concurrConf", d.recovery.concurrencyConfidence);
	}
	if (const json::value* g = group(root, "Advanced"))
	{
		auto& a = out.advanced;
		a.verbosity = getEnum(g, "verbosity", d.advanced.verbosity);
		a.irDumpPath = getString(g, "irDumpPath");
		a.intermediateDir = getString(g, "intermediateDir");
		a.dumpIR = getBool(g, "dumpIR", d.advanced.dumpIR);
		a.dumpASM = getBool(g, "dumpASM", d.advanced.dumpASM);
		a.dumpCFG = getBool(g, "dumpCFG", d.advanced.dumpCFG);
		a.dumpSSA = getBool(g, "dumpSSA", d.advanced.dumpSSA);
		a.colorOutput = getBool(g, "colorOutput", d.advanced.colorOutput);
		a.maxFunctions = getInt(g, "maxFunctions", d.advanced.maxFunctions);
		a.demangleNames = getBool(g, "demangleNames", d.advanced.demangleNames);
	}
	if (const json::value* g = group(root, "Plugins"))
	{
		out.plugins.searchPaths = getStringList(g, "searchPaths");
		out.plugins.enabledPlugins = getStringList(g, "enabledPlugins");
		out.plugins.autoLoadPlugins = getBool(g, "autoLoad", d.plugins.autoLoadPlugins);
	}
	if (const json::value* g = group(root, "Decompiler"))
	{
		auto& dc = out.decompiler;
		dc.useCustomLlvmPasses = getBool(g, "useCustomLlvmPasses", d.decompiler.useCustomLlvmPasses);
		dc.llvmPassesDisabled = getStringList(g, "llvmPassesDisabled");
		dc.extraConfigPath = getString(g, "extraConfigPath");
		dc.decompileOutputDir = getString(g, "decompileOutputDir");
		dc.liveConsoleTail = getBool(g, "liveConsoleTail", d.decompiler.liveConsoleTail);
		dc.outputLang = coerceNativeOutputLang(getString(g, "outputLang", d.decompiler.outputLang));
		dc.decompileProfile = getString(g, "decompileProfile", d.decompiler.decompileProfile);
	}
	return out;
}

// ─── Export document (the Qt exportToFile() key names) ──────────────────────

json::value exportDocument(const SettingsSections& s)
{
	json::object root;
	root.emplace_back(
		"General",
		json::object{
			{"theme", static_cast<int>(s.general.theme)},
			{"font", s.general.editorFont.family},
			{"fontPointSize", s.general.editorFont.pointSize},
			{"fontSize", s.general.fontSize},
			{"language", s.general.language},
			{"showLineNums", s.general.showLineNumbers},
			{"wordWrap", s.general.wordWrap},
			{"restoreSession", s.general.restoreSession},
		});
	root.emplace_back(
		"Analysis",
		json::object{
			{"enableTyping", s.analysis.enableTyping},
			{"enablePatternMatch", s.analysis.enablePatternMatch},
			{"enableConcurrency", s.analysis.enableConcurrency},
			{"enableCudaRecovery", s.analysis.enableCudaRecovery},
			{"enableSerialDetect", s.analysis.enableSerialDetect},
			{"enableModuleCluster", s.analysis.enableModuleCluster},
			{"enableCxxLifter", s.analysis.enableCxxLifter},
			{"minTypeConfidence", s.analysis.minTypeConfidence},
			{"minPatternConfidence", s.analysis.minPatternConfidence},
			{"minRecoveryConfidence", s.analysis.minRecoveryConfidence},
			{"threadCount", s.analysis.threadCount},
			{"maxTimeSecs", s.analysis.maxAnalysisTimeSecs},
		});
	root.emplace_back(
		"ML",
		json::object{
			{"modelPath", s.ml.modelPath},
			{"quantLevel", static_cast<int>(s.ml.quantLevel)},
			{"inferenceDevice", static_cast<int>(s.ml.inferenceDevice)},
			{"temperature", s.ml.temperature},
			{"topP", s.ml.topP},
			{"topK", s.ml.topK},
			{"maxNewTokens", s.ml.maxNewTokens},
			{"contextLength", s.ml.contextLength},
			{"streamOutput", s.ml.streamOutput},
			{"modelSha256", s.ml.modelSha256},
			{"batchRefine", s.ml.batchRefine},
		});
	root.emplace_back(
		"CUDA",
		json::object{
			{"deviceName", s.cuda.deviceName},
			{"deviceIndex", s.cuda.deviceIndex},
			{"kernelCacheDir", s.cuda.kernelCacheDir},
			{"enableProfiling", s.cuda.enableProfiling},
			{"useGPU", s.cuda.useGPU},
			{"blockSize", s.cuda.blockSize},
		});
	root.emplace_back(
		"Recovery",
		json::object{
			{"detectSTL", s.recovery.detectSTL},
			{"detectCrypto", s.recovery.detectCrypto},
			{"detectPatterns", s.recovery.detectPatterns},
			{"detectConcurrency", s.recovery.detectConcurrency},
			{"detectCuda", s.recovery.detectCuda},
			{"detectRTTI", s.recovery.detectRTTI},
			{"detectExceptions", s.recovery.detectExceptions},
			{"detectVirtual", s.recovery.detectVirtual},
			{"stlConfidence", s.recovery.stlConfidence},
			{"cryptoConfidence", s.recovery.cryptoConfidence},
			{"patternConfidence", s.recovery.patternConfidence},
			{"concurrencyConfidence", s.recovery.concurrencyConfidence},
		});
	root.emplace_back(
		"Advanced",
		json::object{
			{"verbosity", static_cast<int>(s.advanced.verbosity)},
			{"irDumpPath", s.advanced.irDumpPath},
			{"intermediateDir", s.advanced.intermediateDir},
			{"dumpIR", s.advanced.dumpIR},
			{"dumpASM", s.advanced.dumpASM},
			{"dumpCFG", s.advanced.dumpCFG},
			{"dumpSSA", s.advanced.dumpSSA},
			{"colorOutput", s.advanced.colorOutput},
			{"maxFunctions", s.advanced.maxFunctions},
			{"demangleNames", s.advanced.demangleNames},
		});
	root.emplace_back(
		"Decompiler",
		json::object{
			{"useCustomLlvmPasses", s.decompiler.useCustomLlvmPasses},
			{"llvmPassesDisabled", stringArray(s.decompiler.llvmPassesDisabled)},
			{"extraConfigPath", s.decompiler.extraConfigPath},
			{"decompileOutputDir", s.decompiler.decompileOutputDir},
			{"liveConsoleTail", s.decompiler.liveConsoleTail},
			{"outputLang", s.decompiler.outputLang},
			{"decompileProfile", s.decompiler.decompileProfile},
		});
	root.emplace_back(
		"Plugins",
		json::object{
			{"searchPaths", stringArray(s.plugins.searchPaths)},
			{"enabledPlugins", stringArray(s.plugins.enabledPlugins)},
			{"autoLoad", s.plugins.autoLoadPlugins},
		});
	return root;
}

void applyExportDocument(const json::value& root, SettingsSections& s)
{
	if (const json::value* g = group(root, "General"))
	{
		s.general.theme = getEnum(g, "theme", GeneralSettings::Theme::Dark);
		s.general.editorFont = getFont(g, "font", FontSpec{});
		s.general.fontSize = getInt(g, "fontSize", 10);
		s.general.language = getString(g, "language", "en");
		s.general.showLineNumbers = getBool(g, "showLineNums", true);
		s.general.wordWrap = getBool(g, "wordWrap", false);
		s.general.restoreSession = getBool(g, "restoreSession", true);
	}
	if (const json::value* g = group(root, "Analysis"))
	{
		auto& a = s.analysis;
		a.enableTyping = getBool(g, "enableTyping", true);
		a.enablePatternMatch = getBool(g, "enablePatternMatch", true);
		a.enableConcurrency = getBool(g, "enableConcurrency", true);
		a.enableCudaRecovery = getBool(g, "enableCudaRecovery", true);
		a.enableSerialDetect = getBool(g, "enableSerialDetect", true);
		a.enableModuleCluster = getBool(g, "enableModuleCluster", true);
		a.enableCxxLifter = getBool(g, "enableCxxLifter", true);
		a.minTypeConfidence = getDouble(g, "minTypeConfidence", 0.5);
		a.minPatternConfidence = getDouble(g, "minPatternConfidence", 0.6);
		a.minRecoveryConfidence = getDouble(g, "minRecoveryConfidence", 0.4);
		a.threadCount = getInt(g, "threadCount", 0);
		a.maxAnalysisTimeSecs = getInt(g, "maxTimeSecs", 300);
	}
	if (const json::value* g = group(root, "ML"))
	{
		auto& m = s.ml;
		m.modelPath = getString(g, "modelPath");
		m.quantLevel = getEnum(g, "quantLevel", MLSettings::QuantLevel::Q4_K_M);
		m.inferenceDevice = getEnum(g, "inferenceDevice", MLSettings::InferenceDevice::Auto);
		m.temperature = getDouble(g, "temperature", 0.7);
		m.topP = getDouble(g, "topP", 0.9);
		m.topK = getInt(g, "topK", 40);
		m.maxNewTokens = getInt(g, "maxNewTokens", 512);
		m.contextLength = getInt(g, "contextLength", 4096);
		m.streamOutput = getBool(g, "streamOutput", true);
		m.modelSha256 = getString(g, "modelSha256");
		m.batchRefine = getBool(g, "batchRefine", false);
	}
	if (const json::value* g = group(root, "CUDA"))
	{
		auto& c = s.cuda;
		c.deviceName = getString(g, "deviceName");
		c.deviceIndex = getInt(g, "deviceIndex", 0);
		c.kernelCacheDir = getString(g, "kernelCacheDir");
		c.enableProfiling = getBool(g, "enableProfiling", false);
		c.useGPU = getBool(g, "useGPU", true);
		c.blockSize = getInt(g, "blockSize", 256);
	}
	if (const json::value* g = group(root, "Recovery"))
	{
		auto& r = s.recovery;
		r.detectSTL = getBool(g, "detectSTL", true);
		r.detectCrypto = getBool(g, "detectCrypto", true);
		r.detectPatterns = getBool(g, "detectPatterns", true);
		r.detectConcurrency = getBool(g, "detectConcurrency", true);
		r.detectCuda = getBool(g, "detectCuda", true);
		r.detectRTTI = getBool(g, "detectRTTI", true);
		r.detectExceptions = getBool(g, "detectExceptions", true);
		r.detectVirtual = getBool(g, "detectVirtual", true);
		r.stlConfidence = getDouble(g, "stlConfidence", 0.6);
		r.cryptoConfidence = getDouble(g, "cryptoConfidence", 0.7);
		r.patternConfidence = getDouble(g, "patternConfidence", 0.5);
		r.concurrencyConfidence = getDouble(g, "concurrencyConfidence", 0.6);
	}
	if (const json::value* g = group(root, "Advanced"))
	{
		auto& a = s.advanced;
		a.verbosity = getEnum(g, "verbosity", AdvancedSettings::Verbosity::Normal);
		a.irDumpPath = getString(g, "irDumpPath");
		a.intermediateDir = getString(g, "intermediateDir");
		a.dumpIR = getBool(g, "dumpIR", false);
		a.dumpASM = getBool(g, "dumpASM", false);
		a.dumpCFG = getBool(g, "dumpCFG", false);
		a.dumpSSA = getBool(g, "dumpSSA", false);
		a.colorOutput = getBool(g, "colorOutput", true);
		a.maxFunctions = getInt(g, "maxFunctions", 0);
		a.demangleNames = getBool(g, "demangleNames", true);
	}
	if (const json::value* g = group(root, "Decompiler"))
	{
		auto& d = s.decompiler;
		d.useCustomLlvmPasses = getBool(g, "useCustomLlvmPasses", false);
		d.extraConfigPath = getString(g, "extraConfigPath");
		d.decompileOutputDir = getString(g, "decompileOutputDir");
		d.liveConsoleTail = getBool(g, "liveConsoleTail", false);
		d.outputLang = coerceNativeOutputLang(getString(g, "outputLang", "c"));
		d.decompileProfile = getString(g, "decompileProfile", "balanced");
		d.llvmPassesDisabled = getStringList(g, "llvmPassesDisabled");
	}
	if (const json::value* g = group(root, "Plugins"))
	{
		s.plugins.searchPaths = getStringList(g, "searchPaths");
		s.plugins.enabledPlugins = getStringList(g, "enabledPlugins");
		s.plugins.autoLoadPlugins = getBool(g, "autoLoad", true);
	}
}

} // namespace

// ─── FontSpec / colours ──────────────────────────────────────────────────────

std::vector<std::string> FontSpec::families() const
{
	std::vector<std::string> out;
	for (const std::string& f: splitCommas(family))
	{
		std::string t = trimmed(f);
		if (!t.empty()) out.push_back(std::move(t));
	}
	return out;
}

FontSpec FontSpec::fromQtString(std::string_view s, int defaultPointSize)
{
	FontSpec out;
	out.pointSize = defaultPointSize;
	const std::vector<std::string> fields = splitCommas(s);
	std::string family;
	bool sawNumber = false;
	for (const std::string& f: fields)
	{
		if (auto n = parseNumber(f))
		{
			if (*n > 0) out.pointSize = static_cast<int>(*n);
			sawNumber = true;
			break;
		}
		if (!family.empty()) family += ',';
		family += f;
	}
	(void)sawNumber;
	if (!trimmed(family).empty()) out.family = family;
	return out;
}

std::string colorToHex(uint32_t rgb)
{
	return std::format("#{:06x}", rgb & 0xffffffu);
}

std::expected<uint32_t, std::string> colorFromHex(std::string_view hex)
{
	const std::string t = trimmed(hex);
	std::string_view d = t;
	if (!d.empty() && d.front() == '#') d.remove_prefix(1);
	std::string digits;
	if (d.size() == 3)
		for (char c: d)
		{
			digits += c;
			digits += c;
		}
	else if (d.size() == 6)
		digits = std::string(d);
	else
		return std::unexpected(std::format("Invalid colour: {}", t));
	uint32_t v = 0;
	const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v, 16);
	if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size())
		return std::unexpected(std::format("Invalid colour: {}", t));
	return v;
}

std::string coerceNativeOutputLang(std::string lang)
{
	const std::string l = lower(lang);
	if (l == "cpp" || l == "c++" || l == "cxx") return "c";
	return lang;
}

// ─── AppSettings ─────────────────────────────────────────────────────────────

AppSettings& AppSettings::instance()
{
	static AppSettings s;
	return s;
}

AppSettings::AppSettings(const AppSettings& other): SettingsSections(other) {}

AppSettings& AppSettings::operator=(const AppSettings& other)
{
	if (this != &other) static_cast<SettingsSections&>(*this) = other;
	return *this;
}

AppSettings& AppSettings::operator=(const SettingsSections& values)
{
	static_cast<SettingsSections&>(*this) = values;
	return *this;
}

fs::path AppSettings::storagePath() const
{
	if (!storagePathOverride_.empty()) return storagePathOverride_;
	return fs::path(forge::os::config_dir()) / "retdec" / "settings.json";
}

void AppSettings::setStoragePath(fs::path path)
{
	storagePathOverride_ = std::move(path);
}

std::expected<void, std::string> AppSettings::load()
{
	const fs::path p = storagePath();
	std::error_code ec;
	if (!fs::exists(p, ec)) return {};
	auto text = readText(p);
	if (!text) return std::unexpected(text.error());
	return fromStorageJson(*text);
}

std::expected<void, std::string> AppSettings::save() const
{
	const fs::path p = storagePath();
	if (p.has_parent_path())
	{
		if (auto st = forge::os::fs::make_dirs(p.parent_path().string()); !st)
			return std::unexpected(std::format("Cannot create {}: {}", p.parent_path().string(), st.err.what));
	}
	if (auto st = forge::os::fs::write_file_atomic(p.string(), toStorageJson()); !st)
		return std::unexpected(std::format("Cannot write {}: {}", p.string(), st.err.what));
	return {};
}

void AppSettings::resetToDefaults()
{
	static_cast<SettingsSections&>(*this) = SettingsSections{};
	notifySettingsChanged();
}

std::string AppSettings::toStorageJson() const
{
	return json::dump(storageDocument(*this));
}

std::string AppSettings::toExportJson() const
{
	return json::dump(exportDocument(*this));
}

std::expected<void, std::string> AppSettings::fromStorageJson(std::string_view text)
{
	auto doc = parseJson(text);
	if (!doc) return std::unexpected("Invalid settings JSON: " + doc.error());
	if (!doc->is_obj()) return std::unexpected("Invalid settings JSON: expected an object");
	static_cast<SettingsSections&>(*this) = fromStorageDocument(*doc);
	return {};
}

std::expected<void, std::string> AppSettings::fromExportJson(std::string_view text)
{
	auto doc = parseJson(text);
	if (!doc) return std::unexpected("Invalid settings JSON: " + doc.error());
	if (!doc->is_obj()) return std::unexpected("Invalid settings JSON: expected an object");
	applyExportDocument(*doc, *this);
	return {};
}

std::expected<void, std::string> AppSettings::exportToFile(const fs::path& path) const
{
	if (auto st = forge::os::write_file(path.string(), toExportJson()); !st)
		return std::unexpected(std::format("Cannot write {}: {}", path.string(), st.err.what));
	return {};
}

std::expected<void, std::string> AppSettings::importFromFile(const fs::path& path)
{
	auto text = readText(path);
	if (!text) return std::unexpected(text.error());
	if (auto r = fromExportJson(*text); !r) return r;
	notifySettingsChanged();
	return {};
}

AppSettings::ListenerId AppSettings::addChangeListener(Listener fn)
{
	const ListenerId id = nextListenerId_++;
	listeners_.emplace(id, std::move(fn));
	return id;
}

void AppSettings::removeChangeListener(ListenerId id)
{
	listeners_.erase(id);
}

void AppSettings::notifySettingsChanged()
{
	// Copy: a listener may add or remove listeners while it runs.
	const auto snapshot = listeners_;
	for (const auto& [id, fn]: snapshot)
		if (fn && listeners_.contains(id)) fn();
}

} // namespace retdec::gui::model
