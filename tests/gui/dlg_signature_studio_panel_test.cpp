/**
 * @file tests/gui/dlg_signature_studio_panel_test.cpp
 * @brief SignatureStudioPanel: defaults from the active binary, field
 *        validation messages, running a tool (a stand-in script) through
 *        DecompileRun and reporting it, and "Use YARA as static-code sigfile".
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "dlg_test_support.h"

#include "retdec/gui/ui/signature_studio_panel.h"

#include <gtest/gtest.h>

#include <fstream>
#include <optional>

using namespace retdec::gui;
using retdec::gui::test::DlgUi;

namespace {

struct DlgSignatureStudio : ::testing::Test
{
	DlgUi t;
	std::filesystem::path dir = test::makeTempDir("sigstudio");
	~DlgSignatureStudio() override
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	/// A stand-in tool: prints its arguments, creates the -o file, exits 0.
	std::filesystem::path fakeTool(const std::string& name)
	{
		const auto p = dir / name;
		std::ofstream(p)
			<< "#!/bin/sh\necho \"tool $0 args: $*\"\n"
			   "while [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then echo out > \"$2\"; fi; shift; done\nexit 0\n";
		std::filesystem::permissions(p, std::filesystem::perms::owner_all);
		return p;
	}
};

} // namespace

TEST_F(DlgSignatureStudio, ActiveBinaryFillsDefaults)
{
	ui::SignatureStudioPanel p(t.ui, t.ui.root());
	EXPECT_EQ(p.panelTitle(), "Signature Studio");
	p.setActiveBinary("/work/lib/libfoo.a");
	t.step();
	EXPECT_EQ(ui::dlg::editText(t.ui, p.expressLibEdit()), "/work/lib/libfoo.a");
	EXPECT_EQ(ui::dlg::editText(t.ui, p.expressOutEdit()), "/work/lib/libfoo-libsig.yar");
	EXPECT_EQ(ui::dlg::editText(t.ui, p.arArchiveEdit()), "/work/lib/libfoo.a");
	EXPECT_EQ(ui::dlg::editText(t.ui, p.machoFileEdit()), "/work/lib/libfoo.a");
	EXPECT_EQ(ui::dlg::editText(t.ui, p.arOutDirEdit()), "/work/lib/retdec-sig-work");
	EXPECT_EQ(ui::dlg::editText(t.ui, p.machoOutDirEdit()), "/work/lib/retdec-sig-work");
	t.png("dlg_signature_studio");
	// Express fields reset on a new binary; others keep the user's text.
	p.setActiveBinary("/other/x.o");
	EXPECT_EQ(ui::dlg::editText(t.ui, p.expressLibEdit()), "/other/x.o");
	EXPECT_EQ(ui::dlg::editText(t.ui, p.arArchiveEdit()), "/work/lib/libfoo.a");
}

TEST_F(DlgSignatureStudio, MissingFieldsAndToolsAreLogged)
{
	ui::SignatureStudioPanel p(t.ui, t.ui.root());
	ui::SignatureStudioPanel::ToolResolver none;
	p.setToolResolver(none);
	EXPECT_FALSE(p.runArExtractor());
	EXPECT_NE(p.log().find("Set archive and output directory."), std::string::npos);
	EXPECT_FALSE(p.runMachoExtractor());
	EXPECT_NE(p.log().find("Set Mach-O file and output directory."), std::string::npos);
	EXPECT_FALSE(p.runBin2pat());
	EXPECT_NE(p.log().find("Set bin2pat input file."), std::string::npos);
	EXPECT_FALSE(p.runPat2yara());
	EXPECT_NE(p.log().find("Set pat2yara input file."), std::string::npos);
	EXPECT_FALSE(p.runExpress());
	EXPECT_NE(p.log().find("Express: set library input and YARA output path."), std::string::npos);
	p.setActiveBinary("/x/lib.a");
	EXPECT_FALSE(p.runExpress());
	EXPECT_NE(
		p.log().find("Express: Python 3 or retdec-signature-from-library-creator.py not found."), std::string::npos);
	t.type(p.bin2patInEdit(), "/x/obj.o");
	EXPECT_FALSE(p.runBin2pat());
	EXPECT_NE(p.log().find("Tool not found: retdec-bin2pat"), std::string::npos);
	// The defaulted output was filled in even though the tool was missing.
	EXPECT_EQ(ui::dlg::editText(t.ui, p.bin2patOutEdit()), "/x/obj.o.pat");
	p.clear();
	EXPECT_TRUE(p.log().empty());
}

#ifndef _WIN32
TEST_F(DlgSignatureStudio, RunsToolAndReportsToCommandLog)
{
	ui::SignatureStudioPanel p(t.ui, t.ui.root());
	ui::SignatureStudioPanel::ToolResolver r;
	const auto tool = fakeTool("retdec-bin2pat");
	r.bin2pat = [tool] { return tool; };
	p.setToolResolver(r);
	std::optional<model::SigToolResult> got;
	p.onCliToolFinished = [&](const model::SigToolResult& res) { got = res; };

	std::ofstream(dir / "obj.o") << "x";
	p.setCurrentTab(3);
	t.type(p.bin2patInEdit(), (dir / "obj.o").string());
	t.type(p.bin2patOutEdit(), (dir / "obj.pat").string());
	t.click(p.bin2patRunButton());
	ASSERT_TRUE(p.busy());
	EXPECT_TRUE(t.ui.disabled(p.bin2patRunButton()));
	EXPECT_TRUE(t.ui.disabled(p.expressRunButton()));
	// The panel's own timer collects the result.
	ASSERT_TRUE(t.waitUntil([&] { return got.has_value(); }, 20000));
	EXPECT_FALSE(p.busy());
	EXPECT_FALSE(t.ui.disabled(p.bin2patRunButton()));
	EXPECT_EQ(got->tool, "retdec-bin2pat");
	EXPECT_EQ(got->exitCode, 0);
	const std::vector<std::string> args{"-o", (dir / "obj.pat").string(), (dir / "obj.o").string()};
	EXPECT_EQ(got->arguments, args);
	EXPECT_NE(got->outputTail.find("args: -o"), std::string::npos);
	EXPECT_NE(p.log().find("--- retdec-bin2pat exit 0 ---"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(dir / "obj.pat"));
	t.png("dlg_signature_studio_ran");
}

TEST_F(DlgSignatureStudio, ExpressRunsPythonWithScript)
{
	ui::SignatureStudioPanel p(t.ui, t.ui.root());
	ui::SignatureStudioPanel::ToolResolver r;
	const auto py = fakeTool("python3");
	const auto script = dir / "scripts" / "retdec-signature-from-library-creator.py";
	std::filesystem::create_directories(script.parent_path());
	std::ofstream(script) << "#";
	r.python = [py] { return py; };
	r.expressScript = [script] { return script; };
	p.setToolResolver(r);
	std::optional<model::SigToolResult> got;
	p.onCliToolFinished = [&](const model::SigToolResult& res) { got = res; };
	p.setActiveBinary((dir / "libz.a").string());
	ASSERT_TRUE(p.runExpress());
	ASSERT_TRUE(p.waitForTool(std::chrono::seconds(20)));
	ASSERT_TRUE(got);
	EXPECT_EQ(got->tool, "retdec-signature-from-library-creator.py");
	ASSERT_EQ(got->arguments.size(), 4u);
	EXPECT_EQ(got->arguments[0], script.string());
	EXPECT_EQ(got->arguments[1], "-o");
	EXPECT_EQ(std::filesystem::path(got->workingDirectory), script.parent_path());

	// The YARA output now exists: "Use YARA as static-code sigfile" picks it.
	std::string sig, status;
	p.onUseAsStaticSigFile = [&](const std::string& s) { sig = s; };
	p.onStatusMessage = [&](const std::string& m, int) { status = m; };
	t.click(p.useAsStaticButton());
	EXPECT_NE(sig.find("libz-libsig.yar"), std::string::npos);
	EXPECT_EQ(status, "Next decompile will use --static-code-sigfile " + sig);
}
#endif

TEST_F(DlgSignatureStudio, UseAsStaticWithoutOutputReportsStatus)
{
	ui::SignatureStudioPanel p(t.ui, t.ui.root());
	std::string status, sig;
	p.onStatusMessage = [&](const std::string& m, int) { status = m; };
	p.onUseAsStaticSigFile = [&](const std::string& s) { sig = s; };
	t.click(p.useAsStaticButton());
	EXPECT_EQ(status, "No YARA output file on disk yet.");
	EXPECT_TRUE(sig.empty());
	// Falls back to the pat2yara output.
	std::ofstream(dir / "rules.yar") << "rule r {}";
	ui::dlg::setEditText(t.ui, p.pat2yaraOutEdit(), (dir / "rules.yar").string());
	p.useAsStaticSigFile();
	EXPECT_NE(sig.find("rules.yar"), std::string::npos);
}
