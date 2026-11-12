/**
 * @file tests/gui/dlg_small_dialogs_test.cpp
 * @brief Headless tests of the dialogs that lived inline in the Qt main
 *        window: About, Keyboard Shortcuts, Go to address / line, Run Stage,
 *        C output style, Raw image / archive options, Set max memory, PDB /
 *        signature prompts, Batch Decompile and the Documentation viewer.
 *        Each opens the dialog, edits fields like a user, presses OK and
 *        checks what the callback received.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "dlg_test_support.h"

#include "retdec/gui/model/dlg_help.h"
#include "retdec/gui/ui/about_dialog.h"
#include "retdec/gui/ui/batch_decompile_dialog.h"
#include "retdec/gui/ui/c_style_dialog.h"
#include "retdec/gui/ui/docs_viewer.h"
#include "retdec/gui/ui/goto_address_dialog.h"
#include "retdec/gui/ui/prompt_dialog.h"
#include "retdec/gui/ui/raw_options_dialog.h"
#include "retdec/gui/ui/run_stage_dialog.h"
#include "retdec/gui/ui/shortcuts_dialog.h"

#include <gtest/gtest.h>

#include <fstream>
#include <optional>

using namespace retdec::gui;
using retdec::gui::test::DlgUi;
namespace w = fuse::ui::widgets;
using fuse::ui::Key;

// ── About / Shortcuts ────────────────────────────────────────────────────────

TEST(DlgAboutDialog, OpensShowsTextAndOkCloses)
{
	DlgUi t;
	ui::AboutDialog about(t.ui);
	EXPECT_FALSE(about.isOpen());
	bool closed = false;
	about.show([&] { closed = true; });
	t.step();
	EXPECT_TRUE(about.isOpen());
	EXPECT_TRUE(t.shown(about.okButton()));
	EXPECT_EQ(about.title(), "About RetDec");
	EXPECT_NE(about.markdown().find("Retargetable Decompiler"), std::string::npos);
	EXPECT_NE(about.markdown().find("F5"), std::string::npos);
	EXPECT_GT(t.ui.childCount(about.markdownNode()), 3u);
	t.png("dlg_about");
	t.click(about.okButton());
	EXPECT_FALSE(about.isOpen());
	EXPECT_TRUE(closed);
}

TEST(DlgShortcutsDialog, ListsTheQtTable)
{
	DlgUi t;
	ui::ShortcutsDialog dlg(t.ui);
	dlg.show();
	t.step();
	EXPECT_EQ(dlg.title(), "Keyboard Shortcuts");
	const auto groups = dlg.groups();
	ASSERT_EQ(groups.size(), 4u);
	EXPECT_EQ(groups[0], "File");
	EXPECT_EQ(groups[3], "Editor / panels");
	const auto rows = dlg.rows();
	EXPECT_EQ(rows.size(), 18u);
	bool hasGoto = false;
	for (const auto& [k, a]: rows)
		if (k == "Ctrl+G" && a == "Go to address") hasGoto = true;
	EXPECT_TRUE(hasGoto);
	t.png("dlg_shortcuts");
	t.click(dlg.closeButton());
	EXPECT_FALSE(dlg.isOpen());
}

TEST(DlgShortcutsDialog, EscapeCloses)
{
	DlgUi t;
	ui::ShortcutsDialog dlg(t.ui);
	dlg.show();
	t.step();
	t.key(Key::Escape);
	EXPECT_FALSE(dlg.isOpen());
}

// ── Go to address / line ─────────────────────────────────────────────────────

TEST(DlgGotoAddressDialog, TypedHexAddressIsAccepted)
{
	DlgUi t;
	ui::GotoAddressDialog dlg(t.ui);
	std::optional<uint64_t> got;
	dlg.show([&](uint64_t a) { got = a; });
	t.type(dlg.edit(), "0x401000");
	t.click(dlg.okButton());
	ASSERT_TRUE(got.has_value());
	EXPECT_EQ(*got, 0x401000u);
	EXPECT_FALSE(dlg.isOpen());
}

TEST(DlgGotoAddressDialog, EnterAcceptsDecimal)
{
	DlgUi t;
	ui::GotoAddressDialog dlg(t.ui);
	std::optional<uint64_t> got;
	dlg.show([&](uint64_t a) { got = a; });
	t.type(dlg.edit(), "4096");
	t.key(Key::Enter);
	ASSERT_TRUE(got.has_value());
	EXPECT_EQ(*got, 4096u);
}

TEST(DlgGotoAddressDialog, InvalidStaysOpenAndReportsStatus)
{
	DlgUi t;
	ui::GotoAddressDialog dlg(t.ui);
	std::string status;
	int timeout = 0;
	dlg.onStatusMessage = [&](const std::string& m, int ms) {
		status = m;
		timeout = ms;
	};
	bool accepted = false;
	dlg.show([&](uint64_t) { accepted = true; });
	t.type(dlg.edit(), "zz");
	t.click(dlg.okButton());
	EXPECT_FALSE(accepted);
	EXPECT_TRUE(dlg.isOpen());
	EXPECT_EQ(status, "Not a valid address");
	EXPECT_EQ(timeout, 3000);
	t.png("dlg_goto_address");
	t.click(dlg.cancelButton());
	EXPECT_FALSE(dlg.isOpen());
	EXPECT_FALSE(accepted);
}

TEST(DlgGotoAddressDialog, TextIsKeptBetweenOpenings)
{
	DlgUi t;
	ui::GotoAddressDialog dlg(t.ui);
	dlg.show([](uint64_t) {});
	t.type(dlg.edit(), "0x10");
	t.click(dlg.okButton());
	dlg.show([](uint64_t) {});
	EXPECT_EQ(dlg.text(), "0x10");
}

TEST(DlgGotoLineDialog, AcceptsPositiveLinesOnly)
{
	DlgUi t;
	ui::GotoLineDialog dlg(t.ui);
	std::string status;
	dlg.onStatusMessage = [&](const std::string& m, int) { status = m; };
	std::optional<int> got;
	dlg.show([&](int l) { got = l; });
	t.type(dlg.edit(), "0");
	t.click(dlg.okButton());
	EXPECT_FALSE(got);
	EXPECT_EQ(status, "Not a valid line number");
	t.type(dlg.edit(), "42");
	t.click(dlg.okButton());
	ASSERT_TRUE(got);
	EXPECT_EQ(*got, 42);
}

// ── Run Stage ────────────────────────────────────────────────────────────────

TEST(DlgRunStageDialog, ListsFiveToolsAndReturnsChoice)
{
	DlgUi t;
	ui::RunStageDialog dlg(t.ui);
	std::optional<model::RunStageChoice> got;
	dlg.show([&](model::RunStageChoice c) { got = c; });
	t.step();
	EXPECT_EQ(dlg.title(), "Run stage");
	EXPECT_EQ(dlg.label(), "Tool to run on the loaded binary:");
	EXPECT_EQ(dlg.currentIndex(), 0);
	EXPECT_EQ(dlg.currentText(), "Decompile (retdec-decompiler)");
	// keyboard: Down twice moves to "fileinfo"
	t.ui.setFocus(dlg.combo(), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down);
	t.key(Key::Down);
	EXPECT_EQ(dlg.currentText(), "fileinfo — binary format report");
	t.png("dlg_run_stage");
	t.click(dlg.okButton());
	ASSERT_TRUE(got);
	EXPECT_EQ(*got, model::RunStageChoice::Fileinfo);
}

TEST(DlgRunStageDialog, CancelGivesNothing)
{
	DlgUi t;
	ui::RunStageDialog dlg(t.ui);
	bool called = false;
	dlg.show([&](model::RunStageChoice) { called = true; });
	t.click(dlg.cancelButton());
	EXPECT_FALSE(called);
	EXPECT_FALSE(dlg.isOpen());
}

// ── C output style ───────────────────────────────────────────────────────────

TEST(DlgCStyleDialog, EditedFieldsBecomeOptions)
{
	DlgUi t;
	ui::CStyleDialog dlg(t.ui);
	std::optional<model::CStyleOptions> got;
	std::string status;
	dlg.onStatusMessage = [&](const std::string& m, int) { status = m; };
	dlg.show({}, [&](const model::CStyleOptions& o) { got = o; });
	t.click(dlg.check(ui::CStyleDialog::KeepBrackets));
	t.click(dlg.check(ui::CStyleDialog::NoOpts));
	t.ui.setFocus(dlg.callInfoCombo(), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down); // omit -> optim
	t.key(Key::Down); // -> pessim
	t.type(dlg.disabledOptsEdit(), "  CopyPropagation,DeadCode  ");
	t.type(dlg.enabledOptsEdit(), "Simplify");
	t.png("dlg_c_style");
	t.click(dlg.okButton());
	ASSERT_TRUE(got);
	EXPECT_TRUE(got->keepAllBrackets);
	EXPECT_TRUE(got->backendNoOpts);
	EXPECT_FALSE(got->noVarRenaming);
	EXPECT_EQ(got->callInfoObtainer, "pessim");
	EXPECT_EQ(got->backendDisabledOpts, "CopyPropagation,DeadCode");
	EXPECT_EQ(got->backendEnabledOpts, "Simplify");
	EXPECT_EQ(status, "C output style applied to the next decompile");

	// The options reach the launch request.
	model::DecompilerLaunchRequest req;
	model::applyCStyleOptions(*got, req);
	EXPECT_TRUE(req.keepAllBrackets);
	EXPECT_TRUE(req.backendNoOpts);
	EXPECT_EQ(req.callInfoObtainer, "pessim");
}

TEST(DlgCStyleDialog, ReopenLoadsCurrentAndCancelKeepsIt)
{
	DlgUi t;
	ui::CStyleDialog dlg(t.ui);
	model::CStyleOptions cur;
	cur.noSymbolicNames = true;
	cur.callInfoObtainer = "optim";
	cur.backendEnabledOpts = "X";
	bool called = false;
	dlg.show(cur, [&](const model::CStyleOptions&) { called = true; });
	t.step();
	EXPECT_TRUE(t.ui.checked(dlg.check(ui::CStyleDialog::NoSymbolic)));
	EXPECT_EQ(std::string(w::comboText(t.ui, dlg.callInfoCombo())), "optim");
	EXPECT_EQ(dlg.options(), cur);
	t.click(dlg.cancelButton());
	EXPECT_FALSE(called);
}

// ── Raw image / archive ──────────────────────────────────────────────────────

TEST(DlgRawOptionsDialog, FormMapsToOptions)
{
	DlgUi t;
	ui::RawOptionsDialog dlg(t.ui);
	std::optional<model::RawArchiveOptions> got;
	dlg.show({}, [&](const model::RawArchiveOptions& o) { got = o; });
	t.ui.setFocus(dlg.endianCombo(), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down); // little
	t.key(Key::Down); // big
	t.ui.setFocus(dlg.bitSizeCombo(), fuse::ui::FocusReason::Keyboard);
	t.key(Key::Down); // 16
	t.key(Key::Down); // 32
	t.type(dlg.vmaEdit(), "0x100000");
	t.type(dlg.arNameEdit(), " member.o ");
	t.type(dlg.arIndexEdit(), "3");
	t.png("dlg_raw_options");
	t.click(dlg.okButton());
	ASSERT_TRUE(got);
	EXPECT_EQ(got->endian, "big");
	EXPECT_EQ(got->bitSize, 32);
	EXPECT_EQ(got->rawSectionVma, 0x100000u);
	EXPECT_EQ(got->arName, "member.o");
	ASSERT_TRUE(got->arIndex);
	EXPECT_EQ(*got->arIndex, 3);

	model::DecompilerLaunchRequest req;
	model::applyRawArchiveOptions(*got, true, req);
	EXPECT_TRUE(req.rawMode);
	EXPECT_EQ(req.endian, "big");
	EXPECT_EQ(req.bitSize, 32);
	EXPECT_EQ(req.arIndex, 3);
}

TEST(DlgRawOptionsDialog, ReopenShowsStoredValues)
{
	DlgUi t;
	ui::RawOptionsDialog dlg(t.ui);
	model::RawArchiveOptions o;
	o.endian = "little";
	o.bitSize = 64;
	o.rawSectionVma = 0x8000;
	o.arIndex = 0;
	dlg.show(o, [](const model::RawArchiveOptions&) {});
	t.step();
	const model::RawArchiveForm f = dlg.form();
	EXPECT_EQ(f.endianText, "little");
	EXPECT_EQ(f.bitSizeText, "64");
	EXPECT_EQ(f.vmaText, "0x8000");
	EXPECT_EQ(f.arIndexText, "0");
	EXPECT_EQ(dlg.options(), o);
}

TEST(DlgRawOptionsDialog, BadIndexAndVmaAreOmitted)
{
	DlgUi t;
	ui::RawOptionsDialog dlg(t.ui);
	std::optional<model::RawArchiveOptions> got;
	dlg.show({}, [&](const model::RawArchiveOptions& o) { got = o; });
	t.type(dlg.vmaEdit(), "nope");
	t.type(dlg.arIndexEdit(), "-2");
	t.click(dlg.okButton());
	ASSERT_TRUE(got);
	EXPECT_EQ(got->rawSectionVma, 0u);
	EXPECT_FALSE(got->arIndex);
	EXPECT_TRUE(got->endian.empty());
	EXPECT_EQ(got->bitSize, 0);
}

// ── Prompts ──────────────────────────────────────────────────────────────────

TEST(DlgMaxMemoryDialog, AcceptsByteCount)
{
	DlgUi t;
	ui::MaxMemoryDialog dlg(t.ui);
	std::optional<uint64_t> got;
	std::string status;
	dlg.onStatusMessage = [&](const std::string& m) { status = m; };
	dlg.show(0, [&](uint64_t b) { got = b; });
	t.step();
	EXPECT_EQ(dlg.prompt().title(), "Max memory");
	EXPECT_EQ(dlg.prompt().text(), "0");
	EXPECT_EQ(dlg.prompt().label(), "Limit decompiler memory in bytes (0 = omit --max-memory):");
	t.type(dlg.prompt().edit(), "1073741824");
	t.click(dlg.prompt().okButton());
	ASSERT_TRUE(got);
	EXPECT_EQ(*got, 1073741824u);
	EXPECT_EQ(status, "Next decompile will pass --max-memory 1073741824");
	EXPECT_FALSE(dlg.isOpen());
}

TEST(DlgMaxMemoryDialog, InvalidKeepsDialogOpenWithError)
{
	DlgUi t;
	ui::MaxMemoryDialog dlg(t.ui);
	bool called = false;
	dlg.show(512, [&](uint64_t) { called = true; });
	t.step();
	EXPECT_EQ(dlg.prompt().text(), "512");
	t.type(dlg.prompt().edit(), "12abc");
	t.key(Key::Enter);
	EXPECT_FALSE(called);
	EXPECT_TRUE(dlg.isOpen());
	EXPECT_EQ(dlg.prompt().error(), "Not a valid byte count.");
	t.png("dlg_max_memory_error");
	t.type(dlg.prompt().edit(), "");
	t.key(Key::Enter);
	EXPECT_TRUE(called);
}

TEST(DlgFilePrompts, PdbAndSignaturePromptsReportPaths)
{
	DlgUi t;
	const auto dir = test::makeTempDir("prompts");
	{
		std::ofstream(dir / "a.pdb") << "x";
		std::ofstream(dir / "rules.yar") << "rule x {}";
		std::ofstream(dir / "other.txt") << "x";
	}
	ui::PdbFilePrompt pdb(t.ui);
	std::string status, chosen;
	pdb.onStatusMessage = [&](const std::string& m) { status = m; };
	pdb.show(dir.string(), [&](const std::string& p) { chosen = p; });
	t.step();
	ASSERT_TRUE(pdb.files().isOpen());
	// Only the .pdb is listed (folders first, none here).
	ASSERT_EQ(w::fileDialogEntryCount(t.ui, pdb.files().dialog()), 1u);
	EXPECT_EQ(w::fileDialogEntry(t.ui, pdb.files().dialog(), 0), "a.pdb");
	const std::string accepted = w::fileDialogActivate(t.ui, pdb.files().dialog(), 0);
	t.step();
	EXPECT_EQ(chosen, accepted);
	EXPECT_EQ(status, "Next decompile will pass --pdb " + accepted);

	ui::SignatureFilePrompt sig(t.ui);
	std::string sigPath;
	sig.onStatusMessage = [&](const std::string& m) { status = m; };
	sig.show(dir.string(), [&](const std::string& p) { sigPath = p; });
	t.step();
	ASSERT_EQ(w::fileDialogEntryCount(t.ui, sig.files().dialog()), 1u);
	EXPECT_EQ(w::fileDialogEntry(t.ui, sig.files().dialog(), 0), "rules.yar");
	t.png("dlg_signature_prompt");
	w::fileDialogActivate(t.ui, sig.files().dialog(), 0);
	t.step();
	EXPECT_NE(sigPath.find("rules.yar"), std::string::npos);
	EXPECT_EQ(status, "Next decompile will pass --static-code-sigfile " + sigPath);
	std::filesystem::remove_all(dir);
}

TEST(DlgFilePrompts, DirectoryModeChoosesFolder)
{
	DlgUi t;
	const auto dir = test::makeTempDir("dirmode");
	std::filesystem::create_directories(dir / "sub");
	std::ofstream(dir / "file.bin") << "x";
	ui::FilePrompt fp(t.ui);
	std::string chosen;
	fp.show(ui::FilePrompt::Mode::Directory, "Pick", dir.string(), {}, [&](const std::string& p) { chosen = p; });
	t.step();
	ASSERT_EQ(w::fileDialogEntryCount(t.ui, fp.dialog()), 1u); // folders only
	EXPECT_EQ(w::fileDialogEntry(t.ui, fp.dialog(), 0), "sub/");
	w::fileDialogActivate(t.ui, fp.dialog(), 0); // enter it
	t.step();
	t.click(fp.chooseFolderButton());
	EXPECT_EQ(std::filesystem::path(chosen).filename(), "sub");
	EXPECT_FALSE(fp.isOpen());
	std::filesystem::remove_all(dir);
}

TEST(DlgMessageDialog, ShowsWarningAndCloses)
{
	DlgUi t;
	ui::MessageDialog m(t.ui);
	bool closed = false;
	m.show("Settings", "Failed", ui::MessageDialog::Kind::Warning, [&] { closed = true; });
	t.step();
	EXPECT_TRUE(m.isOpen());
	EXPECT_EQ(m.text(), "Failed");
	t.click(m.okButton());
	EXPECT_TRUE(closed);
	EXPECT_FALSE(m.isOpen());
}

// ── Batch Decompile ──────────────────────────────────────────────────────────

TEST(DlgBatchDecompileDialog, StartDisabledUntilPathsAdded)
{
	DlgUi t;
	ui::BatchDecompileDialog dlg(t.ui);
	std::vector<std::string> started;
	dlg.show([&](const std::vector<std::string>& p) { started = p; });
	t.step();
	EXPECT_EQ(dlg.title(), "Batch Decompile");
	EXPECT_FALSE(dlg.startEnabled());
	EXPECT_TRUE(dlg.hintVisible());
	EXPECT_TRUE(t.shown(dlg.startButton()));
	t.click(dlg.startButton()); // disabled: nothing happens
	EXPECT_TRUE(started.empty());
	EXPECT_TRUE(dlg.isOpen());

	dlg.addPaths({"/tmp/a.exe", "/tmp/b.elf", "/tmp/a.exe"});
	t.step();
	ASSERT_EQ(dlg.binaryPaths().size(), 2u);
	EXPECT_TRUE(dlg.startEnabled());
	EXPECT_FALSE(dlg.hintVisible());
	t.png("dlg_batch");

	// Select the first row by clicking it, then Remove.
	t.step();
	const fuse::ui::Rect r = t.ui.contentRect(dlg.list());
	t.ui.pointerDown({r.x + 20.f, r.y + 6.f});
	t.ui.pointerUp({r.x + 20.f, r.y + 6.f});
	t.step();
	EXPECT_EQ(dlg.selectedRow(), 0);
	t.click(dlg.removeButton());
	ASSERT_EQ(dlg.binaryPaths().size(), 1u);
	EXPECT_EQ(std::filesystem::path(dlg.binaryPaths()[0]).filename(), "b.elf");

	t.click(dlg.startButton());
	ASSERT_EQ(started.size(), 1u);
	EXPECT_FALSE(dlg.isOpen());
}

TEST(DlgBatchDecompileDialog, ClearEmptiesAndAddUsesFilePrompt)
{
	DlgUi t;
	const auto dir = test::makeTempDir("batch");
	std::ofstream(dir / "prog.exe") << "MZ";
	ui::BatchDecompileDialog dlg(t.ui);
	dlg.show([](const std::vector<std::string>&) {}, dir.string());
	t.click(dlg.addButton());
	ASSERT_TRUE(dlg.filePrompt().isOpen());
	ASSERT_EQ(w::fileDialogEntryCount(t.ui, dlg.filePrompt().dialog()), 1u);
	w::fileDialogActivate(t.ui, dlg.filePrompt().dialog(), 0);
	t.step();
	ASSERT_EQ(dlg.binaryPaths().size(), 1u);
	EXPECT_TRUE(std::filesystem::path(dlg.binaryPaths()[0]).is_absolute());
	t.click(dlg.clearButton());
	EXPECT_TRUE(dlg.binaryPaths().empty());
	EXPECT_FALSE(dlg.startEnabled());
	t.click(dlg.cancelButton());
	EXPECT_FALSE(dlg.isOpen());
	std::filesystem::remove_all(dir);
}

// ── Documentation viewer ─────────────────────────────────────────────────────

TEST(DlgDocsViewer, RendersMarkdownFromDocsDir)
{
	DlgUi t;
	const auto dir = test::makeTempDir("docs");
	std::filesystem::create_directories(dir / "gui");
	std::ofstream(dir / "README.md") << "# RetDec\n\nHello docs.\n";
	std::ofstream(dir / "WINDOWS_NATIVE_BUILD.md") << "# Windows\n\n- one\n- two\n";
	std::ofstream(dir / "gui" / "PORT.md") << "## Port\n\n```\ncode\n```\n";
	ui::DocsViewer v(t.ui);
	v.setDocsDir(dir);
	ASSERT_TRUE(v.show());
	t.step();
	ASSERT_EQ(v.docs().size(), 3u);
	EXPECT_EQ(v.docs()[0].title, "README.md");
	EXPECT_EQ(v.docs()[2].title, "gui/PORT.md");
	// Opens on the Qt viewer's first choice.
	EXPECT_EQ(v.docs()[std::size_t(v.currentIndex())].title, "WINDOWS_NATIVE_BUILD.md");
	EXPECT_NE(v.currentMarkdown().find("# Windows"), std::string::npos);
	EXPECT_GE(t.ui.childCount(v.markdownNode()), 2u);
	// Selecting another document in the list renders it.
	t.ui.setFocus(v.list(), fuse::ui::FocusReason::Keyboard);
	t.key(Key::End);
	EXPECT_EQ(v.currentIndex(), 2);
	EXPECT_NE(v.currentMarkdown().find("## Port"), std::string::npos);
	t.png("dlg_docs_viewer");
	t.click(v.closeButton());
	EXPECT_FALSE(v.isOpen());
	std::filesystem::remove_all(dir);
}

TEST(DlgDocsViewer, NoDocsShowsQtMessage)
{
	DlgUi t;
	const auto dir = test::makeTempDir("nodocs");
	ui::DocsViewer v(t.ui);
	v.setDocsDir(dir);
	EXPECT_FALSE(v.show());
	t.step();
	EXPECT_FALSE(v.isOpen());
	EXPECT_TRUE(v.message().isOpen());
	EXPECT_EQ(v.message().text(), std::string(model::kNoDocsMessage));
	std::filesystem::remove_all(dir);
}

TEST(DlgDocsViewer, FindsRepositoryDocsFromSourceTree)
{
	// In a build tree the GUI resources resolve to src/gui/resources, so the
	// source tree's docs/ is a candidate.
	const auto dir = model::findDocsDir();
	if (dir.empty()) GTEST_SKIP() << "no docs directory reachable from this binary";
	const auto docs = model::listDocs(dir);
	EXPECT_FALSE(docs.empty());
	EXPECT_GE(model::defaultDocIndex(docs), 0);
}
