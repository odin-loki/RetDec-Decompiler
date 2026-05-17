/**
 * @file tests/gui/settings_test.cpp
 * @brief Unit tests for AppSettings, PluginManager, and SettingsDialog.
 */

#include "retdec/gui/settings/settings.h"
#include "retdec/gui/settings/plugin_interface.h"
#include "retdec/gui/settings/plugin_manager.h"
#include "retdec/gui/panels/settings_dialog.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryFile>

using namespace retdec::gui;
using namespace retdec::gui::panels;

// ─── Fixture ─────────────────────────────────────────────────────────────────

class SettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        Q_ASSERT(QApplication::instance() != nullptr);
        AppSettings::instance().resetToDefaults();
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
    }
};

// ─── AppSettings defaults ─────────────────────────────────────────────────────

TEST_F(SettingsTest, NotifySettingsChangedEmitsSignal) {
    QSignalSpy spy(&AppSettings::instance(), &AppSettings::settingsChanged);
    AppSettings::instance().notifySettingsChanged();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(SettingsTest, DefaultThemeIsDark) {
    EXPECT_EQ(AppSettings::instance().general.theme,
              GeneralSettings::Theme::Dark);
}

TEST_F(SettingsTest, DefaultAnalysisAllEnabled) {
    const auto& a = AppSettings::instance().analysis;
    EXPECT_TRUE(a.enableTyping);
    EXPECT_TRUE(a.enablePatternMatch);
    EXPECT_TRUE(a.enableConcurrency);
    EXPECT_TRUE(a.enableCudaRecovery);
}

TEST_F(SettingsTest, DefaultMLTemperature) {
    EXPECT_DOUBLE_EQ(AppSettings::instance().ml.temperature, 0.7);
}

TEST_F(SettingsTest, DefaultMLTopP) {
    EXPECT_DOUBLE_EQ(AppSettings::instance().ml.topP, 0.9);
}

TEST_F(SettingsTest, DefaultMLContextLength) {
    EXPECT_EQ(AppSettings::instance().ml.contextLength, 4096);
}

TEST_F(SettingsTest, DefaultThreadCountZero) {
    EXPECT_EQ(AppSettings::instance().analysis.threadCount, 0);
}

TEST_F(SettingsTest, DefaultRecoveryAllEnabled) {
    const auto& r = AppSettings::instance().recovery;
    EXPECT_TRUE(r.detectSTL);
    EXPECT_TRUE(r.detectCrypto);
    EXPECT_TRUE(r.detectRTTI);
    EXPECT_TRUE(r.detectExceptions);
    EXPECT_TRUE(r.detectVirtual);
}

TEST_F(SettingsTest, DefaultAdvancedVerbosityNormal) {
    EXPECT_EQ(AppSettings::instance().advanced.verbosity,
              AdvancedSettings::Verbosity::Normal);
}

TEST_F(SettingsTest, DefaultDemangleNamesTrue) {
    EXPECT_TRUE(AppSettings::instance().advanced.demangleNames);
}

TEST_F(SettingsTest, DefaultPluginAutoLoadTrue) {
    EXPECT_TRUE(AppSettings::instance().plugins.autoLoadPlugins);
}

// ─── AppSettings modification ────────────────────────────────────────────────

TEST_F(SettingsTest, ModifyTheme) {
    AppSettings::instance().general.theme = GeneralSettings::Theme::Light;
    EXPECT_EQ(AppSettings::instance().general.theme, GeneralSettings::Theme::Light);
}

TEST_F(SettingsTest, ModifyTemperature) {
    AppSettings::instance().ml.temperature = 1.2;
    EXPECT_DOUBLE_EQ(AppSettings::instance().ml.temperature, 1.2);
}

TEST_F(SettingsTest, DisableTyping) {
    AppSettings::instance().analysis.enableTyping = false;
    EXPECT_FALSE(AppSettings::instance().analysis.enableTyping);
}

TEST_F(SettingsTest, SetModelPath) {
    AppSettings::instance().ml.modelPath = "/tmp/model.gguf";
    EXPECT_EQ(AppSettings::instance().ml.modelPath, "/tmp/model.gguf");
}

TEST_F(SettingsTest, ResetToDefaultsRestoresTheme) {
    AppSettings::instance().general.theme = GeneralSettings::Theme::Light;
    AppSettings::instance().resetToDefaults();
    EXPECT_EQ(AppSettings::instance().general.theme,
              GeneralSettings::Theme::Dark);
}

TEST_F(SettingsTest, ResetToDefaultsRestoresTemperature) {
    AppSettings::instance().ml.temperature = 0.1;
    AppSettings::instance().resetToDefaults();
    EXPECT_DOUBLE_EQ(AppSettings::instance().ml.temperature, 0.7);
}

// ─── AppSettings export/import ───────────────────────────────────────────────

TEST_F(SettingsTest, ExportToFileCreatesFile) {
    QTemporaryFile f;
    f.open();
    QString path = f.fileName();
    f.close();
    bool ok = AppSettings::instance().exportToFile(path);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(QFile::exists(path));
}

TEST_F(SettingsTest, ExportImportRoundtrip) {
    AppSettings::instance().ml.temperature = 0.42;
    AppSettings::instance().general.theme  = GeneralSettings::Theme::Light;

    QTemporaryFile f;
    f.open();
    QString path = f.fileName();
    f.close();

    AppSettings::instance().exportToFile(path);
    AppSettings::instance().resetToDefaults();
    AppSettings::instance().importFromFile(path);

    EXPECT_NEAR(AppSettings::instance().ml.temperature, 0.42, 0.001);
    EXPECT_EQ(AppSettings::instance().general.theme, GeneralSettings::Theme::Light);
}

TEST_F(SettingsTest, ImportBadFileReturnsFalse) {
    EXPECT_FALSE(AppSettings::instance().importFromFile("/nonexistent/path.json"));
}

TEST_F(SettingsTest, ExportBadPathReturnsFalse) {
    EXPECT_FALSE(AppSettings::instance().exportToFile("/nonexistent/dir/settings.json"));
}

// ─── Plugin interface ────────────────────────────────────────────────────────

TEST_F(SettingsTest, PluginMetadataFields) {
    PluginMetadata m;
    m.id      = "com.test.plugin";
    m.name    = "Test Plugin";
    m.version = "1.0";
    EXPECT_EQ(m.id, "com.test.plugin");
    EXPECT_EQ(m.apiVersion, RETDEC_PLUGIN_API_VERSION);
}

// Simple concrete plugin for testing
class TestDecompilerPlugin : public IDecompilerPlugin {
public:
    PluginMetadata metadata() const override {
        PluginMetadata m;
        m.id = "com.test.decompiler"; m.name = "TestDecompiler"; m.version = "1";
        return m;
    }
    void runStage(PipelineContext& ctx) override {
        ctx.decompiledText += "\n// TestDecompilerPlugin ran\n";
    }
    bool initCalled = false;
    bool initialize() override { initCalled = true; return true; }
};

class TestOutputPlugin : public IOutputPlugin {
public:
    PluginMetadata metadata() const override {
        PluginMetadata m;
        m.id = "com.test.output"; m.name = "TestOutput"; m.version = "1";
        return m;
    }
    QString formatName() const override { return "Test Format"; }
    QString fileExtension() const override { return ".tst"; }
    QString transform(const QString& src) override { return "// TRANSFORMED\n" + src; }
};

class TestAnalysisPlugin : public IAnalysisPlugin {
public:
    PluginMetadata metadata() const override {
        PluginMetadata m;
        m.id = "com.test.analysis"; m.name = "TestAnalysis"; m.version = "1";
        return m;
    }
    void analyse(PipelineContext& ctx) override {
        ctx.decompiledText += "\n// Analysis done\n";
    }
    QString summary() const override { return "Analysis complete"; }
};

class TestVisualisationPlugin : public IVisualisationPlugin {
public:
    PluginMetadata metadata() const override {
        PluginMetadata m;
        m.id = "com.test.vis"; m.name = "TestVis"; m.version = "1";
        return m;
    }
    QWidget* createPanel(QWidget* parent) override { return new QWidget(parent); }
    QString  panelTitle() const override { return "Test Panel"; }
};

TEST_F(SettingsTest, DecompilerPluginRunStage) {
    TestDecompilerPlugin p;
    PipelineContext ctx;
    ctx.decompiledText = "int main() {}";
    p.runStage(ctx);
    EXPECT_TRUE(ctx.decompiledText.contains(QStringLiteral("TestDecompilerPlugin")));
}

TEST_F(SettingsTest, OutputPluginTransform) {
    TestOutputPlugin p;
    auto result = p.transform("int x;");
    EXPECT_TRUE(result.contains("TRANSFORMED"));
    EXPECT_TRUE(result.contains("int x;"));
}

TEST_F(SettingsTest, OutputPluginMetadata) {
    TestOutputPlugin p;
    EXPECT_EQ(p.formatName(), "Test Format");
    EXPECT_EQ(p.fileExtension(), ".tst");
}

TEST_F(SettingsTest, AnalysisPluginSummary) {
    TestAnalysisPlugin p;
    EXPECT_EQ(p.summary(), "Analysis complete");
}

TEST_F(SettingsTest, AnalysisPluginAnalyse) {
    TestAnalysisPlugin p;
    PipelineContext ctx;
    ctx.decompiledText = "void foo() {}";
    p.analyse(ctx);
    EXPECT_TRUE(ctx.decompiledText.contains("Analysis done"));
}

TEST_F(SettingsTest, VisualisationPluginCreatePanel) {
    TestVisualisationPlugin p;
    auto* w = p.createPanel(nullptr);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(p.panelTitle(), "Test Panel");
    delete w;
}

TEST_F(SettingsTest, PluginInitializeCalled) {
    TestDecompilerPlugin p;
    EXPECT_TRUE(p.initialize());
    EXPECT_TRUE(p.initCalled);
}

// ─── PluginManager ───────────────────────────────────────────────────────────

TEST_F(SettingsTest, PluginManagerIsSingleton) {
    EXPECT_EQ(&PluginManager::instance(), &PluginManager::instance());
}

TEST_F(SettingsTest, PluginManagerLoadNonExistentFile) {
    bool errorEmitted = false;
    QObject::connect(&PluginManager::instance(), &PluginManager::loadError,
                     [&](const QString&, const QString&) { errorEmitted = true; });
    PluginManager::instance().loadPlugin("/nonexistent/plugin.so");
    EXPECT_TRUE(errorEmitted);
}

TEST_F(SettingsTest, PluginManagerFindNonExistent) {
    EXPECT_EQ(PluginManager::instance().findPlugin("nonexistent.id"), nullptr);
}

TEST_F(SettingsTest, PluginManagerRunDecompilerPluginsOnEmpty) {
    PipelineContext ctx;
    ctx.decompiledText = "int x;";
    // Should not crash with no plugins loaded
    PluginManager::instance().runDecompilerPlugins(ctx);
    EXPECT_TRUE(ctx.decompiledText.contains("int x;"));
}

TEST_F(SettingsTest, PluginManagerRunAnalysisPluginsOnEmpty) {
    PipelineContext ctx;
    ctx.decompiledText = "void foo() {}";
    PluginManager::instance().runAnalysisPlugins(ctx);
    EXPECT_TRUE(ctx.decompiledText.contains("void foo"));
}

// ─── SettingsDialog ──────────────────────────────────────────────────────────

TEST_F(SettingsTest, SettingsDialogConstruction) {
    SettingsDialog dlg;
    EXPECT_NE(&dlg, nullptr);
}

TEST_F(SettingsTest, SettingsDialogWindowTitle) {
    SettingsDialog dlg;
    EXPECT_TRUE(dlg.windowTitle().contains("Settings"));
}

TEST_F(SettingsTest, SettingsDialogHasMinimumSize) {
    SettingsDialog dlg;
    EXPECT_GE(dlg.minimumWidth(), 600);
    EXPECT_GE(dlg.minimumHeight(), 400);
}
