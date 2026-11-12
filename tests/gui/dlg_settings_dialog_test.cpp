/**
 * @file tests/gui/dlg_settings_dialog_test.cpp
 * @brief The UI half of the Qt settings_test (SettingsDialog) plus headless
 *        tests that edit every page like a user and check what Apply / OK
 *        writes into model::AppSettings.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "dlg_test_support.h"

#include "retdec/gui/model/dlg_settings_form.h"
#include "retdec/gui/ui/settings_dialog.h"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>

using namespace retdec::gui;
using retdec::gui::test::DlgUi;
namespace w = fuse::ui::widgets;
using fuse::ui::Key;

namespace {

const RetdecGuiPluginV1 kTestPlugin = {
	.struct_size = sizeof(RetdecGuiPluginV1),
	.abi_version = RETDEC_GUI_PLUGIN_ABI_VERSION,
	.id = "com.example.settings-test",
	.name = "Settings Test",
	.version = "1.2",
	.description = "A plugin for the settings dialog test",
	.author = "Test Author",
	.dependencies = nullptr,
	.kinds = 0,
};

struct DlgSettingsDialog : ::testing::Test
{
	DlgUi t;
	std::filesystem::path dir = test::makeTempDir("settings");
	model::AppSettings settings;
	model::PluginManager plugins;
	int notified = 0;

	DlgSettingsDialog()
	{
		settings.setStoragePath(dir / "settings.json");
		settings.addChangeListener([this] { ++notified; });
		plugins.setEnabledList(std::vector<std::string>{});
	}
	~DlgSettingsDialog() override
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
};

} // namespace

// ── Qt settings_test: SettingsDialog ─────────────────────────────────────────

TEST_F(DlgSettingsDialog, Construction)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	EXPECT_FALSE(dlg.isOpen());
	EXPECT_TRUE(t.ui.alive(dlg.card()));
}

TEST_F(DlgSettingsDialog, WindowTitle)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	EXPECT_NE(dlg.windowTitle().find("Settings"), std::string::npos);
}

TEST_F(DlgSettingsDialog, HasMinimumSize)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	EXPECT_GE(dlg.minimumSize().w, 600.f);
	EXPECT_GE(dlg.minimumSize().h, 400.f);
	dlg.show();
	t.step();
	const fuse::ui::Rect r = t.ui.rect(dlg.card());
	EXPECT_GE(r.w, 720.f);
	EXPECT_GE(r.h, 540.f);
}

// ── Structure ────────────────────────────────────────────────────────────────

TEST_F(DlgSettingsDialog, HasEveryTabAndButton)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	const auto tabs = dlg.tabTitles();
	const std::vector<std::string> expected{"General", "Analysis", "CUDA", "ML", "Recovery", "Advanced", "Decompiler",
	                                        "Plugins"};
	EXPECT_EQ(tabs, expected);
	dlg.show();
	t.step();
	for (auto b: {dlg.okButton(), dlg.applyButton(), dlg.cancelButton(), dlg.restoreDefaultsButton(),
	              dlg.exportButton(), dlg.importButton()})
		EXPECT_TRUE(t.shown(b));
	EXPECT_EQ(std::string(t.ui.text(dlg.restoreDefaultsButton())), "Restore Defaults");
	// Every Qt field exists.
	for (const char* name: {"themeCombo", "fontCombo", "fontSizeSpin", "langCombo", "lineNumCheck", "wordWrapCheck",
	                        "restoreCheck", "enableTypingCheck", "enableCxxCheck", "typeConfSpin", "patternConfSpin",
	                        "recovConfSpin", "maxTimeSpin", "threadCountSpin", "clDeviceCombo", "useGPUCheck",
	                        "wgSizeSpin", "kernelCacheEdit", "kernelCacheBtn", "clProfilingCheck", "modelPathEdit",
	                        "modelPathBtn", "quantCombo", "inferDevCombo", "tempSpin", "topPSpin", "topKSpin",
	                        "maxTokensSpin", "contextLenSpin", "streamCheck", "mlModelSha256", "mlBatchRefine",
	                        "detectSTLCheck", "detectVirtCheck", "stlConfSpin", "concurConfSpin", "verbCombo",
	                        "dumpIRCheck", "dumpSSACheck", "colorOutCheck", "irDumpEdit", "intermDirEdit",
	                        "maxFuncSpin", "demangleCheck", "decompileProfileCombo", "useCustomLlvmPassesCheck",
	                        "llvmPassesAllBtn", "llvmPassesNoneBtn", "llvmPassesDefaultBtn", "decompileOutputDirEdit",
	                        "decompileOutputDirBtn", "outputLangCombo", "liveConsoleTailCheck", "decompilerConfigEdit",
	                        "decompilerConfigBtn", "pluginSearchPaths", "pluginSearchAdd", "pluginSearchRemove",
	                        "pluginSearchRescan", "installPluginBtn", "unloadPluginBtn"})
		EXPECT_TRUE(t.ui.alive(dlg.field(name))) << name;
}

TEST_F(DlgSettingsDialog, PopulatesDefaults)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	t.step();
	EXPECT_EQ(ui::dlg::indexOf(t.ui, dlg.field("themeCombo")), 0);
	EXPECT_EQ(std::string(w::comboText(t.ui, dlg.field("themeCombo"))), "Dark");
	EXPECT_NEAR(t.ui.value(dlg.field("tempSpin")), 0.7f, 1e-4f);
	EXPECT_NEAR(t.ui.value(dlg.field("topPSpin")), 0.9f, 1e-4f);
	EXPECT_EQ(t.ui.value(dlg.field("contextLenSpin")), 4096.f);
	EXPECT_EQ(t.ui.value(dlg.field("threadCountSpin")), 0.f);
	EXPECT_EQ(t.ui.value(dlg.field("fontSizeSpin")), 10.f);
	EXPECT_EQ(std::string(w::comboText(t.ui, dlg.field("decompileProfileCombo"))), "Balanced");
	EXPECT_EQ(std::string(w::comboText(t.ui, dlg.field("outputLangCombo"))), "C");
	EXPECT_EQ(std::string(w::comboText(t.ui, dlg.field("verbCombo"))), "Normal");
	EXPECT_EQ(std::string(w::comboText(t.ui, dlg.field("quantCombo"))), "Q4_K_M");
	EXPECT_TRUE(t.ui.checked(dlg.field("demangleCheck")));
	EXPECT_TRUE(t.ui.checked(dlg.field("restoreCheck")));
	EXPECT_FALSE(t.ui.checked(dlg.field("liveConsoleTailCheck")));
	EXPECT_EQ(w::fontPickerFamily(t.ui, dlg.field("fontCombo")), settings.general.editorFont.family);
	// Applying untouched fields changes nothing.
	const model::SettingsSections before = settings;
	EXPECT_TRUE(dlg.apply());
	EXPECT_EQ(static_cast<const model::SettingsSections&>(settings), before);
}

// ── Editing like a user, then OK ─────────────────────────────────────────────

TEST_F(DlgSettingsDialog, EditAndOkWritesSettings)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	int applied = 0, accepted = 0;
	dlg.onApplied = [&] { ++applied; };
	dlg.onAccepted = [&] { ++accepted; };
	dlg.show();

	// General: theme Light via keyboard, line numbers off, language German.
	t.ui.setFocus(dlg.field("themeCombo"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down);
	t.click(dlg.field("lineNumCheck"));
	t.ui.setFocus(dlg.field("langCombo"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down);
	t.ui.setFocus(dlg.field("fontSizeSpin"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Up);
	t.key(Key::Up);
	t.png("dlg_settings_general");

	// Analysis: typing off, type confidence +0.05 twice.
	dlg.setCurrentTab(ui::SettingsDialog::Analysis);
	t.step();
	EXPECT_EQ(dlg.currentTab(), int(ui::SettingsDialog::Analysis));
	t.click(dlg.field("enableTypingCheck"));
	t.ui.setFocus(dlg.field("typeConfSpin"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Up);
	t.key(Key::Up);
	t.png("dlg_settings_analysis");

	// ML: model path typed, SHA trimmed, device CPU, temperature down one step.
	dlg.setCurrentTab(ui::SettingsDialog::ML);
	t.type(dlg.field("modelPathEdit"), "/models/qwen.gguf");
	t.type(dlg.field("mlModelSha256"), "  abc123  ");
	t.ui.setFocus(dlg.field("inferDevCombo"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Home);
	t.key(Key::Up);
	t.key(Key::Up);
	t.ui.setFocus(dlg.field("tempSpin"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down);
	t.png("dlg_settings_ml");

	// Recovery / Advanced / CUDA
	dlg.setCurrentTab(ui::SettingsDialog::Recovery);
	t.click(dlg.field("detectCryptoCheck"));
	dlg.setCurrentTab(ui::SettingsDialog::Advanced);
	t.click(dlg.field("dumpCFGCheck"));
	t.ui.setFocus(dlg.field("verbCombo"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down); // Verbose
	dlg.setCurrentTab(ui::SettingsDialog::CUDA);
	t.ui.setFocus(dlg.field("wgSizeSpin"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Up); // 256 -> 288
	t.type(dlg.field("kernelCacheEdit"), "/tmp/kcache");

	// Decompiler: profile Quality, output lang Java, live console on, output dir (created on apply).
	dlg.setCurrentTab(ui::SettingsDialog::Decompiler);
	t.ui.setFocus(dlg.field("decompileProfileCombo"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down);
	t.ui.setFocus(dlg.field("outputLangCombo"), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down);
	t.key(Key::Down);
	t.key(Key::Down);
	t.click(dlg.field("liveConsoleTailCheck"));
	const auto outDir = dir / "out" / "nested";
	t.type(dlg.field("decompileOutputDirEdit"), outDir.string());
	t.png("dlg_settings_decompiler");

	t.click(dlg.okButton());
	EXPECT_FALSE(dlg.isOpen());
	EXPECT_EQ(applied, 1);
	EXPECT_EQ(accepted, 1);
	EXPECT_GE(notified, 1);

	EXPECT_EQ(settings.general.theme, model::GeneralSettings::Theme::Light);
	EXPECT_FALSE(settings.general.showLineNumbers);
	EXPECT_EQ(settings.general.language, "de");
	EXPECT_EQ(settings.general.fontSize, 12);
	EXPECT_EQ(settings.general.editorFont.pointSize, 12);
	EXPECT_FALSE(settings.analysis.enableTyping);
	EXPECT_DOUBLE_EQ(settings.analysis.minTypeConfidence, 0.6);
	EXPECT_EQ(settings.ml.modelPath, "/models/qwen.gguf");
	EXPECT_EQ(settings.ml.modelSha256, "abc123");
	EXPECT_EQ(settings.ml.inferenceDevice, model::MLSettings::InferenceDevice::CPU);
	EXPECT_DOUBLE_EQ(settings.ml.temperature, 0.65);
	EXPECT_FALSE(settings.recovery.detectCrypto);
	EXPECT_TRUE(settings.advanced.dumpCFG);
	EXPECT_EQ(settings.advanced.verbosity, model::AdvancedSettings::Verbosity::Verbose);
	EXPECT_EQ(settings.cuda.blockSize, 288);
	EXPECT_EQ(settings.cuda.kernelCacheDir, "/tmp/kcache");
	EXPECT_EQ(settings.decompiler.decompileProfile, "quality");
	EXPECT_EQ(settings.decompiler.outputLang, "java");
	EXPECT_TRUE(settings.decompiler.liveConsoleTail);
	EXPECT_EQ(settings.decompiler.decompileOutputDir, outDir.string());
	EXPECT_TRUE(std::filesystem::is_directory(outDir));

	// Saved to the storage file.
	model::AppSettings reloaded;
	reloaded.setStoragePath(dir / "settings.json");
	ASSERT_TRUE(reloaded.load());
	EXPECT_EQ(static_cast<const model::SettingsSections&>(reloaded), static_cast<const model::SettingsSections&>(settings));
}

TEST_F(DlgSettingsDialog, CancelWritesNothing)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	int rejected = 0;
	dlg.onRejected = [&] { ++rejected; };
	dlg.show();
	t.click(dlg.field("lineNumCheck"));
	t.click(dlg.cancelButton());
	EXPECT_FALSE(dlg.isOpen());
	EXPECT_EQ(rejected, 1);
	EXPECT_TRUE(settings.general.showLineNumbers);
	EXPECT_FALSE(std::filesystem::exists(dir / "settings.json"));
}

TEST_F(DlgSettingsDialog, ApplyKeepsDialogOpen)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::Recovery);
	t.click(dlg.field("detectRTTICheck"));
	t.click(dlg.applyButton());
	EXPECT_TRUE(dlg.isOpen());
	EXPECT_FALSE(settings.recovery.detectRTTI);
	EXPECT_TRUE(std::filesystem::exists(dir / "settings.json"));
}

// ── Validation ───────────────────────────────────────────────────────────────

TEST_F(DlgSettingsDialog, MissingConfigFileBlocksApply)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	std::string warning;
	dlg.onWarning = [&](const std::string&, const std::string& m) { warning = m; };
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::Decompiler);
	const std::string missing = (dir / "nope.json").string();
	t.type(dlg.field("decompilerConfigEdit"), missing);
	t.click(dlg.field("liveConsoleTailCheck"));
	t.click(dlg.okButton());
	EXPECT_TRUE(dlg.isOpen()); // OK did not close
	EXPECT_EQ(warning, "Decompiler configuration file does not exist:\n" + missing);
	EXPECT_TRUE(dlg.message().isOpen());
	EXPECT_FALSE(settings.decompiler.liveConsoleTail); // nothing written
	t.png("dlg_settings_warning");
	t.click(dlg.message().okButton());
	// With an existing file it applies.
	std::ofstream(dir / "cfg.json") << "{}";
	t.type(dlg.field("decompilerConfigEdit"), (dir / "cfg.json").string());
	t.click(dlg.okButton());
	EXPECT_FALSE(dlg.isOpen());
	EXPECT_EQ(settings.decompiler.extraConfigPath, (dir / "cfg.json").string());
	EXPECT_TRUE(settings.decompiler.liveConsoleTail);
}

TEST_F(DlgSettingsDialog, UncreatableOutputDirBlocksApply)
{
	std::ofstream(dir / "file") << "x";
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	std::string warning;
	dlg.onWarning = [&](const std::string&, const std::string& m) { warning = m; };
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::Decompiler);
	const std::string bad = (dir / "file" / "sub").string();
	t.type(dlg.field("decompileOutputDirEdit"), bad);
	EXPECT_FALSE(dlg.apply());
	EXPECT_EQ(warning, "Decompile output directory cannot be created:\n" + bad);
	EXPECT_TRUE(settings.decompiler.decompileOutputDir.empty());
}

// ── Restore defaults / export / import ───────────────────────────────────────

TEST_F(DlgSettingsDialog, RestoreDefaults)
{
	settings.general.theme = model::GeneralSettings::Theme::Light;
	settings.ml.temperature = 1.5;
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	t.step();
	EXPECT_EQ(ui::dlg::indexOf(t.ui, dlg.field("themeCombo")), 1);
	t.click(dlg.restoreDefaultsButton());
	EXPECT_EQ(settings.general.theme, model::GeneralSettings::Theme::Dark);
	EXPECT_DOUBLE_EQ(settings.ml.temperature, 0.7);
	EXPECT_EQ(ui::dlg::indexOf(t.ui, dlg.field("themeCombo")), 0);
	EXPECT_NEAR(t.ui.value(dlg.field("tempSpin")), 0.7f, 1e-4f);
}

TEST_F(DlgSettingsDialog, ExportImportRoundTrip)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::Recovery);
	t.click(dlg.field("detectSTLCheck"));
	const auto file = dir / "export.json";
	ASSERT_TRUE(dlg.exportTo(file));
	EXPECT_TRUE(std::filesystem::exists(file));
	EXPECT_FALSE(settings.recovery.detectSTL); // export applies the fields first

	settings.resetToDefaults();
	dlg.populateFromSettings();
	EXPECT_TRUE(t.ui.checked(dlg.field("detectSTLCheck")));
	ASSERT_TRUE(dlg.importFrom(file));
	EXPECT_FALSE(settings.recovery.detectSTL);
	EXPECT_FALSE(t.ui.checked(dlg.field("detectSTLCheck")));
	EXPECT_FALSE(dlg.importFrom(dir / "missing.json"));
}

TEST_F(DlgSettingsDialog, ExportButtonUsesSavePrompt)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	t.click(dlg.exportButton());
	ASSERT_TRUE(dlg.filePrompt().isOpen());
	EXPECT_EQ(dlg.filePrompt().mode(), ui::FilePrompt::Mode::Save);
	const auto file = dir / "via_button.json";
	dlg.filePrompt().choose(file.string());
	EXPECT_TRUE(std::filesystem::exists(file));
}

// ── Decompiler tab: LLVM passes ──────────────────────────────────────────────

TEST_F(DlgSettingsDialog, LlvmPassButtons)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	ASSERT_TRUE(dlg.llvmPassError().empty()) << dlg.llvmPassError();
	ASSERT_FALSE(dlg.llvmPassNames().empty());
	const std::size_t n = dlg.llvmPassNames().size();
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::Decompiler);
	t.click(dlg.field("useCustomLlvmPassesCheck"));
	t.click(dlg.field("llvmPassesNoneBtn"));
	for (std::size_t i = 0; i < n; ++i) EXPECT_FALSE(t.ui.checked(dlg.llvmPassCheck(i)));
	t.ui.setChecked(dlg.llvmPassCheck(0), true);
	ASSERT_TRUE(dlg.apply());
	EXPECT_TRUE(settings.decompiler.useCustomLlvmPasses);
	EXPECT_EQ(settings.decompiler.llvmPassesDisabled.size(), n - 1);
	EXPECT_EQ(std::find(settings.decompiler.llvmPassesDisabled.begin(), settings.decompiler.llvmPassesDisabled.end(),
	                    dlg.llvmPassNames()[0]),
	          settings.decompiler.llvmPassesDisabled.end());

	t.click(dlg.field("llvmPassesDefaultBtn"));
	EXPECT_FALSE(t.ui.checked(dlg.field("useCustomLlvmPassesCheck")));
	ASSERT_TRUE(dlg.apply());
	EXPECT_FALSE(settings.decompiler.useCustomLlvmPasses);
	EXPECT_TRUE(settings.decompiler.llvmPassesDisabled.empty());

	// Reopening shows the stored disabled list.
	settings.decompiler.llvmPassesDisabled = {dlg.llvmPassNames()[1]};
	dlg.populateFromSettings();
	EXPECT_TRUE(t.ui.checked(dlg.llvmPassCheck(0)));
	EXPECT_FALSE(t.ui.checked(dlg.llvmPassCheck(1)));
}

TEST_F(DlgSettingsDialog, BrowseButtonFillsField)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::ML);
	t.click(dlg.field("modelPathBtn"));
	ASSERT_TRUE(dlg.filePrompt().isOpen());
	EXPECT_EQ(dlg.filePrompt().mode(), ui::FilePrompt::Mode::Open);
	dlg.filePrompt().choose("/models/x.gguf");
	EXPECT_EQ(ui::dlg::editText(t.ui, dlg.field("modelPathEdit")), "/models/x.gguf");
}

// ── General tab: font picker live preview ────────────────────────────────────

TEST_F(DlgSettingsDialog, FontPickAppliesLive)
{
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	t.step();
	const int before = notified;
	const fuse::ui::NodeHandle list = t.ui.firstChild(dlg.field("fontCombo"));
	t.ui.setFocus(list, fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down); // next family
	EXPECT_GT(notified, before);
	EXPECT_NE(settings.general.editorFont.family, model::FontSpec{}.family);
	EXPECT_EQ(settings.general.editorFont.family, w::fontPickerFamily(t.ui, dlg.field("fontCombo")));
}

// ── Plugins tab ──────────────────────────────────────────────────────────────

TEST_F(DlgSettingsDialog, PluginsTab)
{
	ASSERT_TRUE(plugins.registerPlugin(&kTestPlugin));
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::Plugins);
	t.step();
	ASSERT_EQ(dlg.pluginRows().size(), 1u);
	EXPECT_EQ(dlg.pluginRows()[0].first, "com.example.settings-test");
	EXPECT_TRUE(dlg.pluginRows()[0].second);
	EXPECT_EQ(dlg.pluginDetail(), "Select a plugin to see details.");
	EXPECT_TRUE(t.ui.disabled(dlg.pluginEnabledCheck()));

	// Select the row with the pointer.
	const fuse::ui::Rect r = t.ui.contentRect(dlg.pluginList());
	t.ui.pointerDown({r.x + 20.f, r.y + 6.f});
	t.ui.pointerUp({r.x + 20.f, r.y + 6.f});
	t.step();
	EXPECT_NE(dlg.pluginDetail().find("Settings Test v1.2 — A plugin for the settings dialog test"), std::string::npos);
	EXPECT_NE(dlg.pluginDetail().find("Author: Test Author"), std::string::npos);
	EXPECT_FALSE(t.ui.disabled(dlg.pluginEnabledCheck()));
	EXPECT_TRUE(t.ui.checked(dlg.pluginEnabledCheck()));
	t.png("dlg_settings_plugins");

	// Untick: the plugin manager and the saved settings follow immediately.
	t.click(dlg.pluginEnabledCheck());
	EXPECT_FALSE(plugins.findPlugin("com.example.settings-test")->enabled());
	EXPECT_TRUE(settings.plugins.enabledPlugins.empty());
	EXPECT_TRUE(std::filesystem::exists(dir / "settings.json"));
	t.click(dlg.pluginEnabledCheck());
	EXPECT_EQ(settings.plugins.enabledPlugins, std::vector<std::string>{"com.example.settings-test"});

	t.click(dlg.field("unloadPluginBtn"));
	EXPECT_EQ(plugins.findPlugin("com.example.settings-test"), nullptr);
	EXPECT_TRUE(dlg.pluginRows().empty());
}

TEST_F(DlgSettingsDialog, SearchPaths)
{
	settings.plugins.searchPaths = {"/opt/a"};
	ui::SettingsDialog dlg(t.ui, settings, plugins);
	dlg.show();
	dlg.setCurrentTab(ui::SettingsDialog::Plugins);
	t.step();
	EXPECT_EQ(dlg.searchPaths(), std::vector<std::string>{"/opt/a"});
	t.click(dlg.field("pluginSearchAdd"));
	ASSERT_TRUE(dlg.filePrompt().isOpen());
	EXPECT_EQ(dlg.filePrompt().mode(), ui::FilePrompt::Mode::Directory);
	dlg.filePrompt().choose(dir.string());
	EXPECT_EQ(dlg.searchPaths().size(), 2u);
	EXPECT_EQ(dlg.rescanPlugins(), 0); // nothing loadable there
	t.ui.setValue(dlg.searchPathList(), 0.f, true);
	t.click(dlg.field("pluginSearchRemove"));
	EXPECT_EQ(dlg.searchPaths(), std::vector<std::string>{dir.string()});
	ASSERT_TRUE(dlg.apply());
	EXPECT_EQ(settings.plugins.searchPaths, std::vector<std::string>{dir.string()});
}
