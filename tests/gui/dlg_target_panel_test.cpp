/**
 * @file tests/gui/dlg_target_panel_test.cpp
 * @brief TargetPanel: the comprehensive_smoke_test null-project case, applying
 *        overrides to a ProjectFile through the fields, the --config buttons
 *        and the JSON editor.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "dlg_test_support.h"

#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/ui/target_panel.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace retdec::gui;
using fuse::ui::Key;
using retdec::gui::test::DlgUi;

namespace {

struct DlgTargetPanel : ::testing::Test
{
	DlgUi t;
	std::filesystem::path dir = test::makeTempDir("target");
	model::AppSettings settings;
	int notified = 0;
	DlgTargetPanel()
	{
		settings.setStoragePath(dir / "settings.json");
		settings.addChangeListener([this] { ++notified; });
	}
	~DlgTargetPanel() override
	{
		model::setApplicationDirPathOverride({});
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
};

} // namespace

// comprehensive_smoke_test: TargetPanelHandlesNullProject
TEST_F(DlgTargetPanel, HandlesNullProject)
{
	ui::TargetPanel p(t.ui, t.ui.root(), settings);
	p.setFromProject(nullptr);
	p.clear();
	EXPECT_TRUE(p.archText().empty());
	EXPECT_TRUE(p.osText().empty());
	EXPECT_TRUE(p.entryText().empty() || p.entryText() == "0x0");
	EXPECT_EQ(p.binaryText(), "—");
	EXPECT_FALSE(p.fieldsEnabled());
	EXPECT_EQ(p.panelTitle(), "Target");
	std::string status;
	p.onStatusMessage = [&](const std::string& m, int) { status = m; };
	EXPECT_FALSE(p.apply());
	EXPECT_EQ(status, "No project loaded.");
}

TEST_F(DlgTargetPanel, ShowsProjectAndAppliesTypedOverrides)
{
	model::ProjectFile pf("/bin/demo.elf");
	pf.setArch("x86");
	pf.setOs("linux");
	pf.setEntryPoint(0x401000);
	pf.clearModified();
	ui::TargetPanel p(t.ui, t.ui.root(), settings);
	p.setFromProject(&pf);
	t.step();
	EXPECT_EQ(p.binaryText(), "/bin/demo.elf");
	EXPECT_EQ(p.archText(), "x86");
	EXPECT_EQ(p.osText(), "linux");
	EXPECT_EQ(p.entryText(), "0x401000");
	EXPECT_TRUE(p.fieldsEnabled());

	std::string status;
	int applied = 0, requested = 0;
	p.onStatusMessage = [&](const std::string& m, int) { status = m; };
	p.onApplied = [&] { ++applied; };
	p.onApplyRequested = [&] { ++requested; };
	t.type(p.archEdit(), " arm ");
	t.type(p.osEdit(), "windows");
	t.type(p.entryEdit(), "0X8000");
	t.png("dlg_target_panel");
	t.click(p.applyButton());
	EXPECT_EQ(requested, 1);
	EXPECT_EQ(applied, 1);
	EXPECT_EQ(pf.arch(), "arm");
	EXPECT_EQ(pf.os(), "windows");
	EXPECT_EQ(pf.entryPoint(), 0x8000u);
	EXPECT_TRUE(pf.isModified());
	EXPECT_EQ(status, "Target fields applied to project (save .retdec to persist).");

	// Enter in a field applies too; decimal entry.
	t.type(p.entryEdit(), "4096");
	t.key(Key::Enter);
	EXPECT_EQ(pf.entryPoint(), 4096u);
	// Alt+A applies from anywhere in the panel; empty entry = 0.
	t.type(p.entryEdit(), "");
	t.key(Key::A, fuse::ui::kModAlt);
	EXPECT_EQ(pf.entryPoint(), 0u);
}

TEST_F(DlgTargetPanel, InvalidEntryPointWarns)
{
	model::ProjectFile pf("/bin/demo.elf");
	pf.setEntryPoint(0x10);
	ui::TargetPanel p(t.ui, t.ui.root(), settings);
	p.setFromProject(&pf);
	std::string title, warning;
	p.onWarning = [&](const std::string& ti, const std::string& m) {
		title = ti;
		warning = m;
	};
	t.type(p.entryEdit(), "0xZZ");
	EXPECT_FALSE(p.apply());
	EXPECT_EQ(title, "Target");
	EXPECT_EQ(warning, "Invalid entry point (use 0x… hex or decimal).");
	EXPECT_TRUE(p.message().isOpen());
	EXPECT_EQ(pf.entryPoint(), 0x10u);
}

TEST_F(DlgTargetPanel, ConfigButtonsUpdateSettings)
{
	const auto cfg = dir / "my-config.json";
	std::ofstream(cfg) << R"({"a": 1})";
	ui::TargetPanel p(t.ui, t.ui.root(), settings);
	t.step();
	EXPECT_TRUE(p.decompilerConfigText().empty());
	EXPECT_TRUE(t.ui.disabled(p.editButton()));
	EXPECT_FALSE(t.ui.disabled(p.browseButton())); // usable without a project

	t.click(p.browseButton());
	ASSERT_TRUE(p.filePrompt().isOpen());
	p.filePrompt().choose(cfg.string());
	EXPECT_EQ(settings.decompiler.extraConfigPath, cfg.generic_string());
	EXPECT_EQ(p.decompilerConfigText(), cfg.generic_string());
	EXPECT_FALSE(t.ui.disabled(p.editButton()));
	EXPECT_GE(notified, 1);
	EXPECT_TRUE(std::filesystem::exists(dir / "settings.json"));

	t.click(p.clearButton());
	EXPECT_TRUE(settings.decompiler.extraConfigPath.empty());
	EXPECT_TRUE(p.decompilerConfigText().empty());
	EXPECT_TRUE(t.ui.disabled(p.editButton()));
}

TEST_F(DlgTargetPanel, UseBundledFindsShareConfig)
{
	// <app>/retdec-decompiler + <app>/../share/retdec/decompiler-config.json
	const auto bin = dir / "bin";
	std::filesystem::create_directories(bin);
	std::filesystem::create_directories(dir / "share" / "retdec");
	const auto exe = bin / test::toolFileName("retdec-decompiler");
	{
		std::ofstream(exe) << "#!/bin/sh\n";
		std::ofstream(dir / "share" / "retdec" / "decompiler-config.json") << "{}";
	}
	std::filesystem::permissions(exe, std::filesystem::perms::owner_all);
	model::setApplicationDirPathOverride(bin);
	ui::TargetPanel p(t.ui, t.ui.root(), settings);
	t.step();
	ASSERT_FALSE(t.ui.disabled(p.bundledButton()));
	t.click(p.bundledButton());
	EXPECT_NE(settings.decompiler.extraConfigPath.find("decompiler-config.json"), std::string::npos);
}

TEST_F(DlgTargetPanel, UseBundledWithoutOneShowsInformation)
{
	model::setApplicationDirPathOverride(dir);
	ui::TargetPanel p(t.ui, t.ui.root(), settings);
	std::string warning;
	p.onWarning = [&](const std::string&, const std::string& m) { warning = m; };
	if (!model::resolveBundledDecompilerConfigPath().empty()) GTEST_SKIP() << "a bundled config is on PATH";
	EXPECT_TRUE(t.ui.disabled(p.bundledButton()));
	p.useBundledDecompilerConfig();
	EXPECT_EQ(warning, "Could not find share/retdec/decompiler-config.json next to retdec-decompiler.");
	EXPECT_TRUE(settings.decompiler.extraConfigPath.empty());
}

TEST_F(DlgTargetPanel, EditJsonRejectsInvalidAndSavesValid)
{
	const auto cfg = dir / "edit.json";
	std::ofstream(cfg) << R"({"x": 1})";
	settings.decompiler.extraConfigPath = cfg.string();
	ui::TargetPanel p(t.ui, t.ui.root(), settings);
	t.step();
	ASSERT_FALSE(t.ui.disabled(p.editButton()));
	t.click(p.editButton());
	ui::DecompilerConfigEditor& ed = p.configEditor();
	ASSERT_TRUE(ed.isOpen());
	EXPECT_EQ(ed.title(), "Edit — edit.json");
	EXPECT_EQ(ed.text(), R"({"x": 1})");
	t.png("dlg_target_config_editor");

	std::string warnTitle;
	ed.onWarning = [&](const std::string& ti, const std::string&) { warnTitle = ti; };
	ed.setText("{ broken");
	t.click(ed.saveButton());
	EXPECT_EQ(warnTitle, "Invalid JSON");
	EXPECT_TRUE(ed.isOpen());
	t.click(ed.message().okButton());

	ed.setText(R"({"x": 2})");
	t.click(ed.saveButton());
	EXPECT_FALSE(ed.isOpen());
	std::ifstream in(cfg);
	std::string body((std::istreambuf_iterator<char>(in)), {});
	EXPECT_EQ(body, R"({"x": 2})");
}
