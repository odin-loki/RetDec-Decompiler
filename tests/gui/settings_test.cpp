/**
 * @file tests/gui/settings_test.cpp
 * @brief AppSettings and the plugin ABI / PluginManager.
 */

#include "retdec/gui/model/plugins.h"
#include "retdec/gui/model/settings.h"

#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/process.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace m = retdec::gui::model;
namespace fs = std::filesystem;
using m::AdvancedSettings;
using m::AppSettings;
using m::GeneralSettings;
using m::MLSettings;

namespace {

struct TempDir
{
	fs::path path;
	TempDir()
	{
		path = fs::path(forge::os::fs::make_temp_dir("retdec-gui-settings-").value);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::string readAll(const fs::path& p)
{
	auto r = forge::os::read_file(p.string());
	return r ? r.value : std::string{};
}

class SettingsTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		AppSettings::instance().resetToDefaults();
	}
	void TearDown() override
	{
		AppSettings::instance().resetToDefaults();
	}
	TempDir tmp;
};

} // namespace

// ─── Defaults ────────────────────────────────────────────────────────────────

TEST_F(SettingsTest, NotifySettingsChangedRunsListeners)
{
	auto& s = AppSettings::instance();
	int calls = 0;
	const auto id = s.addChangeListener([&] { ++calls; });
	s.notifySettingsChanged();
	EXPECT_EQ(calls, 1);
	s.resetToDefaults();
	EXPECT_EQ(calls, 2);
	s.removeChangeListener(id);
	s.notifySettingsChanged();
	EXPECT_EQ(calls, 2);
}

TEST_F(SettingsTest, ListenerMayRemoveItself)
{
	AppSettings s;
	int calls = 0;
	AppSettings::ListenerId id = 0;
	id = s.addChangeListener([&] {
		++calls;
		s.removeChangeListener(id);
	});
	s.notifySettingsChanged();
	s.notifySettingsChanged();
	EXPECT_EQ(calls, 1);
}

TEST_F(SettingsTest, DefaultThemeIsDark)
{
	EXPECT_EQ(AppSettings::instance().general.theme, GeneralSettings::Theme::Dark);
}

TEST_F(SettingsTest, DefaultFont)
{
	const auto& f = AppSettings::instance().general.editorFont;
	EXPECT_EQ(f.family, "Cascadia Code,JetBrains Mono,Consolas");
	EXPECT_EQ(f.pointSize, 10);
	EXPECT_EQ(f.families(), (std::vector<std::string>{"Cascadia Code", "JetBrains Mono", "Consolas"}));
	EXPECT_EQ(AppSettings::instance().general.fontSize, 10);
	EXPECT_EQ(AppSettings::instance().general.language, "en");
}

TEST_F(SettingsTest, DefaultAnalysisAllEnabled)
{
	const auto& a = AppSettings::instance().analysis;
	EXPECT_TRUE(a.enableTyping);
	EXPECT_TRUE(a.enablePatternMatch);
	EXPECT_TRUE(a.enableConcurrency);
	EXPECT_TRUE(a.enableCudaRecovery);
	EXPECT_DOUBLE_EQ(a.minTypeConfidence, 0.5);
	EXPECT_DOUBLE_EQ(a.minPatternConfidence, 0.6);
	EXPECT_DOUBLE_EQ(a.minRecoveryConfidence, 0.4);
	EXPECT_EQ(a.maxAnalysisTimeSecs, 300);
}

TEST_F(SettingsTest, DefaultMLTemperature)
{
	EXPECT_DOUBLE_EQ(AppSettings::instance().ml.temperature, 0.7);
}

TEST_F(SettingsTest, DefaultMLTopP)
{
	EXPECT_DOUBLE_EQ(AppSettings::instance().ml.topP, 0.9);
}

TEST_F(SettingsTest, DefaultMLContextLength)
{
	EXPECT_EQ(AppSettings::instance().ml.contextLength, 4096);
	EXPECT_EQ(AppSettings::instance().ml.quantLevel, MLSettings::QuantLevel::Q4_K_M);
	EXPECT_EQ(AppSettings::instance().ml.inferenceDevice, MLSettings::InferenceDevice::Auto);
}

TEST_F(SettingsTest, DefaultThreadCountZero)
{
	EXPECT_EQ(AppSettings::instance().analysis.threadCount, 0);
}

TEST_F(SettingsTest, DefaultRecoveryAllEnabled)
{
	const auto& r = AppSettings::instance().recovery;
	EXPECT_TRUE(r.detectSTL);
	EXPECT_TRUE(r.detectCrypto);
	EXPECT_TRUE(r.detectRTTI);
	EXPECT_TRUE(r.detectExceptions);
	EXPECT_TRUE(r.detectVirtual);
	EXPECT_DOUBLE_EQ(r.cryptoConfidence, 0.7);
}

TEST_F(SettingsTest, DefaultAdvancedVerbosityNormal)
{
	EXPECT_EQ(AppSettings::instance().advanced.verbosity, AdvancedSettings::Verbosity::Normal);
}

TEST_F(SettingsTest, DefaultDemangleNamesTrue)
{
	EXPECT_TRUE(AppSettings::instance().advanced.demangleNames);
}

TEST_F(SettingsTest, DefaultPluginAutoLoadTrue)
{
	EXPECT_TRUE(AppSettings::instance().plugins.autoLoadPlugins);
}

TEST_F(SettingsTest, DefaultDecompileOutputDirEmpty)
{
	EXPECT_TRUE(AppSettings::instance().decompiler.decompileOutputDir.empty());
	EXPECT_EQ(AppSettings::instance().decompiler.outputLang, "c");
	EXPECT_EQ(AppSettings::instance().decompiler.decompileProfile, "balanced");
}

TEST_F(SettingsTest, DefaultLiveConsoleTailFalse)
{
	EXPECT_FALSE(AppSettings::instance().decompiler.liveConsoleTail);
}

// ─── Modification ────────────────────────────────────────────────────────────

TEST_F(SettingsTest, ModifyTheme)
{
	AppSettings::instance().general.theme = GeneralSettings::Theme::Light;
	EXPECT_EQ(AppSettings::instance().general.theme, GeneralSettings::Theme::Light);
}

TEST_F(SettingsTest, ModifyTemperature)
{
	AppSettings::instance().ml.temperature = 1.2;
	EXPECT_DOUBLE_EQ(AppSettings::instance().ml.temperature, 1.2);
}

TEST_F(SettingsTest, DisableTyping)
{
	AppSettings::instance().analysis.enableTyping = false;
	EXPECT_FALSE(AppSettings::instance().analysis.enableTyping);
}

TEST_F(SettingsTest, SetModelPath)
{
	AppSettings::instance().ml.modelPath = "/tmp/model.gguf";
	EXPECT_EQ(AppSettings::instance().ml.modelPath, "/tmp/model.gguf");
}

TEST_F(SettingsTest, ResetToDefaultsRestoresTheme)
{
	AppSettings::instance().general.theme = GeneralSettings::Theme::Light;
	AppSettings::instance().resetToDefaults();
	EXPECT_EQ(AppSettings::instance().general.theme, GeneralSettings::Theme::Dark);
}

TEST_F(SettingsTest, ResetToDefaultsRestoresTemperature)
{
	AppSettings::instance().ml.temperature = 0.1;
	AppSettings::instance().resetToDefaults();
	EXPECT_DOUBLE_EQ(AppSettings::instance().ml.temperature, 0.7);
}

TEST_F(SettingsTest, CopyCopiesValuesNotListeners)
{
	AppSettings a;
	int calls = 0;
	a.addChangeListener([&] { ++calls; });
	a.ml.topK = 7;
	AppSettings b = a;
	EXPECT_EQ(b.ml.topK, 7);
	b.notifySettingsChanged();
	EXPECT_EQ(calls, 0);
	AppSettings c;
	c = static_cast<const m::SettingsSections&>(a);
	EXPECT_EQ(c.ml.topK, 7);
	EXPECT_TRUE(static_cast<const m::SettingsSections&>(a) == static_cast<const m::SettingsSections&>(c));
}

// ─── Export / import ─────────────────────────────────────────────────────────

TEST_F(SettingsTest, ExportToFileCreatesFile)
{
	const fs::path path = tmp.path / "export.json";
	EXPECT_TRUE(AppSettings::instance().exportToFile(path).has_value());
	EXPECT_TRUE(fs::exists(path));
	const auto doc = forge::json::parse(readAll(path));
	// The Qt export key names.
	EXPECT_NE(doc["General"].find("showLineNums"), nullptr);
	EXPECT_NE(doc["Analysis"].find("minTypeConfidence"), nullptr);
	EXPECT_NE(doc["Recovery"].find("concurrencyConfidence"), nullptr);
	EXPECT_NE(doc["Plugins"].find("autoLoad"), nullptr);
}

TEST_F(SettingsTest, ExportImportRoundtrip)
{
	auto& s = AppSettings::instance();
	s.ml.temperature = 0.42;
	s.general.theme = GeneralSettings::Theme::Light;
	const fs::path path = tmp.path / "export.json";

	ASSERT_TRUE(s.exportToFile(path).has_value());
	s.resetToDefaults();
	int calls = 0;
	const auto id = s.addChangeListener([&] { ++calls; });
	ASSERT_TRUE(s.importFromFile(path).has_value());
	s.removeChangeListener(id);

	EXPECT_NEAR(s.ml.temperature, 0.42, 0.001);
	EXPECT_EQ(s.general.theme, GeneralSettings::Theme::Light);
	EXPECT_EQ(calls, 1);
}

TEST_F(SettingsTest, ExportImportRoundtripExtended)
{
	auto& s = AppSettings::instance();
	s.cuda.useGPU = false;
	s.recovery.detectPatterns = false;
	s.advanced.dumpCFG = true;
	s.ml.maxNewTokens = 256;
	s.ml.contextLength = 2048;
	s.ml.inferenceDevice = MLSettings::InferenceDevice::CPU;
	s.ml.modelSha256 = "deadbeef";
	s.ml.batchRefine = true;
	s.analysis.enableTyping = false;
	s.general.editorFont = {"JetBrains Mono", 13};
	s.decompiler.llvmPassesDisabled = {"a", "b"};
	const fs::path path = tmp.path / "export.json";

	ASSERT_TRUE(s.exportToFile(path).has_value());
	s.resetToDefaults();
	ASSERT_TRUE(s.importFromFile(path).has_value());

	EXPECT_FALSE(s.cuda.useGPU);
	EXPECT_FALSE(s.recovery.detectPatterns);
	EXPECT_TRUE(s.advanced.dumpCFG);
	EXPECT_EQ(s.ml.maxNewTokens, 256);
	EXPECT_EQ(s.ml.contextLength, 2048);
	EXPECT_EQ(s.ml.inferenceDevice, MLSettings::InferenceDevice::CPU);
	EXPECT_EQ(s.ml.modelSha256, "deadbeef");
	EXPECT_TRUE(s.ml.batchRefine);
	EXPECT_FALSE(s.analysis.enableTyping);
	EXPECT_EQ(s.general.editorFont, (m::FontSpec{"JetBrains Mono", 13}));
	EXPECT_EQ(s.decompiler.llvmPassesDisabled, (std::vector<std::string>{"a", "b"}));
}

TEST_F(SettingsTest, ExportImportPluginsAndLanguage)
{
	auto& s = AppSettings::instance();
	s.general.language = "de";
	s.plugins.autoLoadPlugins = false;
	s.plugins.enabledPlugins = {"com.test"};
	const fs::path path = tmp.path / "export.json";

	ASSERT_TRUE(s.exportToFile(path).has_value());
	s.resetToDefaults();
	ASSERT_TRUE(s.importFromFile(path).has_value());

	EXPECT_EQ(s.general.language, "de");
	EXPECT_FALSE(s.plugins.autoLoadPlugins);
	EXPECT_EQ(s.plugins.enabledPlugins, (std::vector<std::string>{"com.test"}));
}

TEST_F(SettingsTest, ImportQtExportAndCoerceOutputLang)
{
	const fs::path path = tmp.path / "qt-export.json";
	std::ofstream(path) << R"({
      "General": {"theme": 2, "font": "Fira Code,11,-1,5,400,0,0,0,0,0,0,0,0,0,0,1", "fontSize": 11,
                  "language": "fr", "showLineNums": false, "wordWrap": true, "restoreSession": false},
      "Decompiler": {"outputLang": "C++", "llvmPassesDisabled": ["x", 3]}
    })";
	auto& s = AppSettings::instance();
	s.ml.topK = 99; // groups missing from the file are left alone
	ASSERT_TRUE(s.importFromFile(path).has_value());
	EXPECT_EQ(s.general.theme, GeneralSettings::Theme::SystemDefault);
	EXPECT_EQ(s.general.editorFont.family, "Fira Code");
	EXPECT_EQ(s.general.editorFont.pointSize, 11);
	EXPECT_FALSE(s.general.showLineNumbers);
	EXPECT_TRUE(s.general.wordWrap);
	EXPECT_EQ(s.decompiler.outputLang, "c");
	EXPECT_EQ(s.decompiler.decompileProfile, "balanced");
	EXPECT_EQ(s.decompiler.llvmPassesDisabled, (std::vector<std::string>{"x"}));
	EXPECT_EQ(s.ml.topK, 99);
}

TEST_F(SettingsTest, ImportBadFileReturnsError)
{
	EXPECT_FALSE(AppSettings::instance().importFromFile("/nonexistent/path.json").has_value());
	const fs::path bad = tmp.path / "bad.json";
	std::ofstream(bad) << "{ nope";
	AppSettings::instance().ml.topK = 5;
	EXPECT_FALSE(AppSettings::instance().importFromFile(bad).has_value());
	EXPECT_EQ(AppSettings::instance().ml.topK, 5);
}

TEST_F(SettingsTest, ExportBadPathReturnsError)
{
	EXPECT_FALSE(AppSettings::instance().exportToFile("/nonexistent/dir/settings.json").has_value());
}

// ─── Persistence ─────────────────────────────────────────────────────────────

TEST_F(SettingsTest, DefaultStoragePathIsInTheUserConfigDir)
{
	AppSettings s;
	const fs::path p = s.storagePath();
	EXPECT_EQ(p.filename(), "settings.json");
	EXPECT_EQ(p.parent_path().filename(), "retdec");
}

TEST_F(SettingsTest, SaveLoadRoundTripUsesQSettingsKeys)
{
	AppSettings a;
	a.setStoragePath(tmp.path / "cfg" / "retdec" / "settings.json");
	a.general.lastOpenDir = "/home/u/bin";
	a.general.lastBinaryPath = "/home/u/bin/a.out";
	a.general.editorFont = {"Iosevka, Menlo", 14};
	a.analysis.minTypeConfidence = 0.25;
	a.recovery.concurrencyConfidence = 0.33;
	a.advanced.verbosity = AdvancedSettings::Verbosity::Debug;
	a.plugins.searchPaths = {"/p1", "/p2"};
	a.decompiler.outputLang = "python";
	a.cuda.kernelCacheDir = "/cache";
	ASSERT_TRUE(a.save().has_value());

	const auto doc = forge::json::parse(readAll(a.storagePath()));
	EXPECT_DOUBLE_EQ(doc["Analysis"]["minTypeConf"].num(), 0.25);
	EXPECT_DOUBLE_EQ(doc["Recovery"]["concurrConf"].num(), 0.33);
	EXPECT_EQ(doc["General"]["showLineNumbers"].b(), true);
	EXPECT_EQ(doc["Advanced"]["verbosity"].i(), 3);

	AppSettings b;
	b.setStoragePath(a.storagePath());
	ASSERT_TRUE(b.load().has_value());
	EXPECT_TRUE(static_cast<const m::SettingsSections&>(a) == static_cast<const m::SettingsSections&>(b));
}

TEST_F(SettingsTest, LoadMissingFileKeepsDefaultsAndBadFileFails)
{
	AppSettings s;
	s.setStoragePath(tmp.path / "nothing.json");
	EXPECT_TRUE(s.load().has_value());
	EXPECT_TRUE(static_cast<const m::SettingsSections&>(s) == m::SettingsSections{});

	std::ofstream(tmp.path / "bad.json") << "[1,2,3]";
	s.setStoragePath(tmp.path / "bad.json");
	s.ml.topK = 3;
	EXPECT_FALSE(s.load().has_value());
	EXPECT_EQ(s.ml.topK, 3);
}

TEST_F(SettingsTest, LoadFillsMissingKeysWithDefaults)
{
	std::ofstream(tmp.path / "partial.json")
		<< R"({"ML": {"topK": 12, "temperature": "0.3"}, "Decompiler": {"outputLang": "cxx"}})";
	AppSettings s;
	s.setStoragePath(tmp.path / "partial.json");
	ASSERT_TRUE(s.load().has_value());
	EXPECT_EQ(s.ml.topK, 12);
	EXPECT_DOUBLE_EQ(s.ml.temperature, 0.3);
	EXPECT_DOUBLE_EQ(s.ml.topP, 0.9);
	EXPECT_EQ(s.decompiler.outputLang, "c");
	EXPECT_EQ(s.general.language, "en");
}

TEST_F(SettingsTest, SaveIsAtomic)
{
	AppSettings s;
	s.setStoragePath(tmp.path / "s.json");
	ASSERT_TRUE(s.save().has_value());
	s.ml.topK = 1;
	ASSERT_TRUE(s.save().has_value());
	int files = 0;
	for (const auto& e: fs::directory_iterator(tmp.path))
	{
		(void)e;
		++files;
	}
	EXPECT_EQ(files, 1);
}

TEST_F(SettingsTest, FontAndColourHelpers)
{
	const auto f = m::FontSpec::fromQtString("Cascadia Code,JetBrains Mono,Consolas,12,-1,5,50,0,0,0,0,0");
	EXPECT_EQ(f.family, "Cascadia Code,JetBrains Mono,Consolas");
	EXPECT_EQ(f.pointSize, 12);
	EXPECT_EQ(m::FontSpec::fromQtString("Hack", 9), (m::FontSpec{"Hack", 9}));
	EXPECT_EQ(m::FontSpec::fromQtString("", 9).family, m::FontSpec{}.family);

	EXPECT_EQ(m::colorToHex(0x1e1e2e), "#1e1e2e");
	EXPECT_EQ(m::colorFromHex("#1E1E2E").value_or(0), 0x1e1e2eu);
	EXPECT_EQ(m::colorFromHex("abc").value_or(0), 0xaabbccu);
	EXPECT_FALSE(m::colorFromHex("#12345").has_value());
	EXPECT_FALSE(m::colorFromHex("#gggggg").has_value());

	EXPECT_EQ(m::coerceNativeOutputLang("CPP"), "c");
	EXPECT_EQ(m::coerceNativeOutputLang("python"), "python");
}

// ─── Plugin ABI ──────────────────────────────────────────────────────────────

namespace {

char* dupString(const std::string& s)
{
	char* p = static_cast<char*>(std::malloc(s.size() + 1));
	std::memcpy(p, s.c_str(), s.size() + 1);
	return p;
}

void freeString(char* p)
{
	std::free(p);
}

int gInitCalls = 0;
int gShutdownCalls = 0;
int initOk()
{
	++gInitCalls;
	return 1;
}
int initRefuse()
{
	return 0;
}
void shutdownCount()
{
	++gShutdownCalls;
}

char* runStage(const RetdecGuiPipelineContext* ctx)
{
	return dupString(std::string(ctx->decompiled_text) + "\n// TestDecompilerPlugin ran\n");
}
char* transform(const char* c, size_t)
{
	return dupString(std::string("// TRANSFORMED\n") + c);
}
char* transformFails(const char*, size_t)
{
	return nullptr;
}
char* analyse(const RetdecGuiPipelineContext* ctx)
{
	return dupString(std::string(ctx->decompiled_text) + "\n// Analysis done\n");
}
char* summary()
{
	return dupString("Analysis complete");
}
char* renderPanel(const RetdecGuiPipelineContext* ctx)
{
	return dupString(std::string("# Panel for ") + ctx->input_binary_path);
}

RetdecGuiPluginV1 base(const char* id, uint32_t kinds)
{
	RetdecGuiPluginV1 p{};
	p.struct_size = sizeof(RetdecGuiPluginV1);
	p.abi_version = RETDEC_GUI_PLUGIN_ABI_VERSION;
	p.id = id;
	p.name = id;
	p.version = "1";
	p.kinds = kinds;
	p.free_string = freeString;
	return p;
}

const RetdecGuiPluginV1& decompilerPlugin()
{
	static const RetdecGuiPluginV1 p = [] {
		auto d = base("com.test.decompiler", RETDEC_GUI_PLUGIN_DECOMPILER);
		d.initialize = initOk;
		d.shutdown = shutdownCount;
		d.insert_after_stage = "Type inference";
		d.run_stage = runStage;
		return d;
	}();
	return p;
}

const RetdecGuiPluginV1& outputPlugin()
{
	static const char* const deps[] = {"com.test.analysis", "com.not.loaded", nullptr};
	static const RetdecGuiPluginV1 p = [] {
		auto d = base("com.test.output", RETDEC_GUI_PLUGIN_OUTPUT);
		d.dependencies = deps;
		d.format_name = "Test Format";
		d.file_extension = ".tst";
		d.transform = transform;
		return d;
	}();
	return p;
}

const RetdecGuiPluginV1& analysisPlugin()
{
	static const RetdecGuiPluginV1 p = [] {
		auto d = base("com.test.analysis", RETDEC_GUI_PLUGIN_ANALYSIS);
		d.analyse = analyse;
		d.summary = summary;
		return d;
	}();
	return p;
}

const RetdecGuiPluginV1& visualisationPlugin()
{
	static const RetdecGuiPluginV1 p = [] {
		auto d = base("com.test.vis", RETDEC_GUI_PLUGIN_VISUALISATION);
		d.panel_title = "Test Panel";
		d.render_panel = renderPanel;
		return d;
	}();
	return p;
}

/// Plugin manager that ignores AppSettings' enabled list.
struct Manager : m::PluginManager
{
	Manager()
	{
		setEnabledList(std::vector<std::string>{});
	}
};

} // namespace

TEST_F(SettingsTest, PluginMetadataFields)
{
	m::PluginMetadata md;
	md.id = "com.test.plugin";
	md.name = "Test Plugin";
	md.version = "1.0";
	EXPECT_EQ(md.id, "com.test.plugin");
	EXPECT_EQ(md.apiVersion, RETDEC_PLUGIN_API_VERSION);

	const auto from = m::pluginMetadataFrom(outputPlugin());
	EXPECT_EQ(from.id, "com.test.output");
	EXPECT_EQ(from.dependencies, (std::vector<std::string>{"com.test.analysis", "com.not.loaded"}));
	EXPECT_EQ(from.kinds, static_cast<uint32_t>(RETDEC_GUI_PLUGIN_OUTPUT));
}

TEST_F(SettingsTest, DescriptorValidation)
{
	EXPECT_FALSE(m::validatePluginDescriptor(nullptr).has_value());
	auto d = base("x", RETDEC_GUI_PLUGIN_OUTPUT);
	EXPECT_TRUE(m::validatePluginDescriptor(&d).has_value());
	auto v = d;
	v.abi_version = 2;
	EXPECT_NE(m::validatePluginDescriptor(&v).error().find("API version mismatch"), std::string::npos);
	auto shrunk = d;
	shrunk.struct_size = 8;
	EXPECT_FALSE(m::validatePluginDescriptor(&shrunk).has_value());
	auto noId = d;
	noId.id = "";
	EXPECT_FALSE(m::validatePluginDescriptor(&noId).has_value());
	auto noFree = d;
	noFree.transform = transform;
	noFree.free_string = nullptr;
	EXPECT_FALSE(m::validatePluginDescriptor(&noFree).has_value());
}

TEST_F(SettingsTest, DecompilerPluginRunStage)
{
	Manager mgr;
	gInitCalls = 0;
	ASSERT_TRUE(mgr.registerPlugin(&decompilerPlugin()).has_value());
	EXPECT_EQ(gInitCalls, 1);
	const m::Plugin* p = mgr.findPlugin("com.test.decompiler");
	ASSERT_NE(p, nullptr);
	EXPECT_TRUE(p->isDecompiler());
	EXPECT_FALSE(p->isOutput());
	EXPECT_EQ(p->insertAfterStage(), "Type inference");

	m::PipelineContext ctx;
	ctx.decompiledText = "int main() {}";
	mgr.runDecompilerPlugins(ctx);
	EXPECT_NE(ctx.decompiledText.find("TestDecompilerPlugin"), std::string::npos);
	EXPECT_NE(ctx.decompiledText.find("int main() {}"), std::string::npos);
}

TEST_F(SettingsTest, OutputPluginTransformAndMetadata)
{
	Manager mgr;
	ASSERT_TRUE(mgr.registerPlugin(&outputPlugin()).has_value());
	const auto outs = mgr.outputPlugins();
	ASSERT_EQ(outs.size(), 1u);
	EXPECT_EQ(outs[0]->formatName(), "Test Format");
	EXPECT_EQ(outs[0]->fileExtension(), ".tst");
	const auto result = outs[0]->transform("int x;");
	ASSERT_TRUE(result.has_value());
	EXPECT_NE(result->find("TRANSFORMED"), std::string::npos);
	EXPECT_NE(result->find("int x;"), std::string::npos);

	auto failing = base("com.test.failing", RETDEC_GUI_PLUGIN_OUTPUT);
	failing.transform = transformFails;
	ASSERT_TRUE(mgr.registerPlugin(&failing).has_value());
	EXPECT_FALSE(mgr.findPlugin("com.test.failing")->transform("x").has_value());
	mgr.unloadAll();
}

TEST_F(SettingsTest, AnalysisPluginAnalyseAndSummary)
{
	Manager mgr;
	ASSERT_TRUE(mgr.registerPlugin(&analysisPlugin()).has_value());
	const m::Plugin* p = mgr.findPlugin("com.test.analysis");
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(p->summary(), "Analysis complete");
	m::PipelineContext ctx;
	ctx.decompiledText = "void foo() {}";
	mgr.runAnalysisPlugins(ctx);
	EXPECT_NE(ctx.decompiledText.find("Analysis done"), std::string::npos);
}

TEST_F(SettingsTest, VisualisationPluginRendersPanelText)
{
	Manager mgr;
	ASSERT_TRUE(mgr.registerPlugin(&visualisationPlugin()).has_value());
	const auto vis = mgr.visualisationPlugins();
	ASSERT_EQ(vis.size(), 1u);
	EXPECT_EQ(vis[0]->panelTitle(), "Test Panel");
	m::PipelineContext ctx;
	ctx.inputBinaryPath = "/bin/ls";
	EXPECT_EQ(vis[0]->renderPanel(ctx), "# Panel for /bin/ls");
}

TEST_F(SettingsTest, PluginInitializeRefusalAndDuplicates)
{
	Manager mgr;
	std::vector<std::string> errors;
	mgr.onLoadError.push_back([&](const fs::path&, const std::string& msg) { errors.push_back(msg); });

	auto refusing = base("com.test.refuse", RETDEC_GUI_PLUGIN_OUTPUT);
	refusing.initialize = initRefuse;
	auto r = mgr.registerPlugin(&refusing);
	ASSERT_FALSE(r.has_value());
	EXPECT_NE(r.error().find("initialization failed"), std::string::npos);
	EXPECT_EQ(mgr.findPlugin("com.test.refuse"), nullptr);

	ASSERT_TRUE(mgr.registerPlugin(&analysisPlugin()).has_value());
	EXPECT_FALSE(mgr.registerPlugin(&analysisPlugin()).has_value());
	ASSERT_EQ(errors.size(), 2u);
	EXPECT_NE(errors[1].find("already loaded"), std::string::npos);
}

TEST_F(SettingsTest, PluginsAreOrderedByDependencies)
{
	Manager mgr;
	ASSERT_TRUE(mgr.registerPlugin(&outputPlugin()).has_value()); // depends on analysis
	ASSERT_TRUE(mgr.registerPlugin(&decompilerPlugin()).has_value());
	ASSERT_TRUE(mgr.registerPlugin(&analysisPlugin()).has_value());
	std::vector<std::string> ids;
	for (const auto& p: mgr.plugins())
		ids.push_back(p->metadata().id);
	const auto pos = [&](const std::string& id) { return std::find(ids.begin(), ids.end(), id) - ids.begin(); };
	EXPECT_LT(pos("com.test.analysis"), pos("com.test.output"));
	EXPECT_EQ(ids.size(), 3u);
}

TEST_F(SettingsTest, EnableDisableAndEnabledList)
{
	Manager mgr;
	ASSERT_TRUE(mgr.registerPlugin(&analysisPlugin()).has_value());
	mgr.setEnabled("com.test.analysis", false);
	EXPECT_TRUE(mgr.analysisPlugins().empty());
	m::PipelineContext ctx;
	ctx.decompiledText = "x";
	mgr.runAnalysisPlugins(ctx);
	EXPECT_EQ(ctx.decompiledText, "x");
	mgr.setEnabled("com.test.analysis", true);
	EXPECT_EQ(mgr.analysisPlugins().size(), 1u);

	m::PluginManager listed;
	listed.setEnabledList(std::vector<std::string>{"com.test.output"});
	ASSERT_TRUE(listed.registerPlugin(&analysisPlugin()).has_value());
	ASSERT_TRUE(listed.registerPlugin(&outputPlugin()).has_value());
	EXPECT_FALSE(listed.findPlugin("com.test.analysis")->enabled());
	EXPECT_TRUE(listed.findPlugin("com.test.output")->enabled());
}

TEST_F(SettingsTest, UnloadRunsListenersAroundShutdown)
{
	Manager mgr;
	std::vector<std::string> events;
	mgr.onLoaded.push_back([&](const std::string& id) { events.push_back("loaded " + id); });
	mgr.onAboutToUnload.push_back([&](const std::string& id) {
		// Still there and safe to touch.
		ASSERT_NE(mgr.findPlugin(id), nullptr);
		events.push_back("about " + id + " shutdowns=" + std::to_string(gShutdownCalls));
	});
	mgr.onUnloaded.push_back([&](const std::string& id) {
		EXPECT_EQ(mgr.findPlugin(id), nullptr);
		events.push_back("gone " + id + " shutdowns=" + std::to_string(gShutdownCalls));
	});
	gShutdownCalls = 0;
	ASSERT_TRUE(mgr.registerPlugin(&decompilerPlugin()).has_value());
	mgr.unloadPlugin("com.test.decompiler");
	mgr.unloadPlugin("com.test.decompiler"); // unknown now: nothing happens
	EXPECT_EQ(
		events,
		(std::vector<std::string>{
			"loaded com.test.decompiler",
			"about com.test.decompiler shutdowns=0",
			"gone com.test.decompiler shutdowns=1"}));
}

TEST_F(SettingsTest, PluginManagerIsSingleton)
{
	EXPECT_EQ(&m::PluginManager::instance(), &m::PluginManager::instance());
}

TEST_F(SettingsTest, PluginManagerLoadNonExistentFile)
{
	Manager mgr;
	bool errorEmitted = false;
	mgr.onLoadError.push_back([&](const fs::path&, const std::string&) { errorEmitted = true; });
	auto r = mgr.loadPlugin("/nonexistent/plugin.so");
	EXPECT_FALSE(r.has_value());
	EXPECT_TRUE(errorEmitted);
}

TEST_F(SettingsTest, PluginManagerRejectsNonPluginLibrary)
{
	Manager mgr;
	const fs::path junk = tmp.path / "libjunk.so";
	std::ofstream(junk) << "not an ELF";
	EXPECT_FALSE(mgr.loadPlugin(junk).has_value());
	EXPECT_EQ(mgr.scanAndLoad({tmp.path.string(), "/nonexistent/dir"}), 0);
	EXPECT_TRUE(mgr.plugins().empty());
}

TEST_F(SettingsTest, PluginManagerFindNonExistent)
{
	EXPECT_EQ(m::PluginManager::instance().findPlugin("nonexistent.id"), nullptr);
}

TEST_F(SettingsTest, PluginManagerRunDecompilerPluginsOnEmpty)
{
	Manager mgr;
	m::PipelineContext ctx;
	ctx.decompiledText = "int x;";
	mgr.runDecompilerPlugins(ctx);
	EXPECT_EQ(ctx.decompiledText, "int x;");
}

TEST_F(SettingsTest, PluginManagerRunAnalysisPluginsOnEmpty)
{
	Manager mgr;
	m::PipelineContext ctx;
	ctx.decompiledText = "void foo() {}";
	mgr.runAnalysisPlugins(ctx);
	EXPECT_EQ(ctx.decompiledText, "void foo() {}");
}

TEST_F(SettingsTest, LoadsACompiledCPluginThroughDynlib)
{
#if defined(_WIN32)
	GTEST_SKIP() << "builds the fixture with a POSIX C compiler";
#else
	std::string cc;
	for (const char* name: {"cc", "gcc", "clang"})
		if (cc = forge::os::find_executable(name); !cc.empty()) break;
	if (cc.empty()) GTEST_SKIP() << "no C compiler on PATH to build the fixture plugin";

	const fs::path src = fs::path(RETDEC_GUI_TEST_DATA_DIR) / "test_plugin.c";
	const fs::path include = fs::path(RETDEC_SOURCE_DIR) / "include";
	const fs::path dir = tmp.path / "plugins";
	fs::create_directories(dir);
#if defined(__APPLE__)
	const fs::path lib = dir / "libretdec_test_plugin.dylib";
#else
	const fs::path lib = dir / "libretdec_test_plugin.so";
#endif
	forge::os::spawn_spec sp;
	sp.argv = {cc, "-std=c11", "-shared", "-fPIC", "-I" + include.string(), "-o", lib.string(), src.string()};
	sp.timeout_ms = 120000;
	auto built = forge::os::spawn_wait(sp);
	ASSERT_TRUE(static_cast<bool>(built)) << built.err.what;
	ASSERT_EQ(built.value.exit_code, 0) << built.value.err;

	Manager mgr;
	ASSERT_EQ(mgr.scanAndLoad({dir.string()}), 1);
	const m::Plugin* p = mgr.findPlugin("com.retdec.test.cplugin");
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(p->metadata().name, "C test plugin");
	EXPECT_EQ(p->metadata().version, "1.2");
	EXPECT_EQ(p->metadata().author, "retdec tests");
	EXPECT_EQ(fs::canonical(p->filePath()), fs::canonical(lib));
	EXPECT_TRUE(p->isOutput());
	EXPECT_TRUE(p->isAnalysis());
	EXPECT_FALSE(p->isDecompiler());
	EXPECT_EQ(p->summary(), "initialized");

	const auto out = p->transform("int main(void);");
	ASSERT_TRUE(out.has_value());
	EXPECT_EQ(*out, "// TRANSFORMED BY C PLUGIN\nint main(void);");

	m::PipelineContext ctx;
	ctx.decompiledText = "int x;";
	mgr.runAnalysisPlugins(ctx);
	EXPECT_EQ(ctx.decompiledText, "int x;\n// analysed by C plugin\n");

	// Loading the same file again is a duplicate.
	EXPECT_FALSE(mgr.loadPlugin(lib).has_value());
	mgr.unloadPlugin("com.retdec.test.cplugin");
	EXPECT_TRUE(mgr.plugins().empty());
#endif
}
