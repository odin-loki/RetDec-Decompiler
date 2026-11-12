/**
 * @file tests/gui/main_window_test.cpp
 * @brief MainWindow in a headless UiContext: the Qt mainwindow_layout_test,
 *        the main-window parts of comprehensive_smoke_test and
 *        polish_integration_test, and the whole open -> analyse -> artefacts
 *        -> panels flow against a fake retdec-decompiler
 *        (data/main_window_fake_decompiler.sh + data/main_window/fixture.*).
 *
 * Set RETDEC_GUI_SCREENSHOT_DIR to keep the rendered main-window PNGs.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/model/cli_tool_paths.h"
#include "retdec/gui/model/settings.h"
#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/ai_assistant_panel.h"
#include "retdec/gui/ui/assembly_panel.h"
#include "retdec/gui/ui/batch_decompile_dialog.h"
#include "retdec/gui/ui/c_style_dialog.h"
#include "retdec/gui/ui/call_graph_panel.h"
#include "retdec/gui/ui/cfg_panel.h"
#include "retdec/gui/ui/command_log_panel.h"
#include "retdec/gui/ui/decompiled_c_panel.h"
#include "retdec/gui/ui/diagnostics_panel.h"
#include "retdec/gui/ui/diff_panel.h"
#include "retdec/gui/ui/docs_viewer.h"
#include "retdec/gui/ui/about_dialog.h"
#include "retdec/gui/ui/function_list_panel.h"
#include "retdec/gui/ui/goto_address_dialog.h"
#include "retdec/gui/ui/ir_panel.h"
#include "retdec/gui/ui/live_console_panel.h"
#include "retdec/gui/ui/main_window.h"
#include "retdec/gui/ui/progress_panel.h"
#include "retdec/gui/ui/prompt_dialog.h"
#include "retdec/gui/ui/raw_options_dialog.h"
#include "retdec/gui/ui/run_stage_dialog.h"
#include "retdec/gui/ui/settings_dialog.h"
#include "retdec/gui/ui/shortcuts_dialog.h"
#include "retdec/gui/ui/strings_browser_panel.h"
#include "retdec/gui/ui/tri_pane_code_view.h"
#include "retdec/gui/ui/triage_banner.h"

#include <fuse/app/app.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;
namespace fs = std::filesystem;
using fuse::ui::Key;
using fuse::ui::kModCtrl;
using fuse::ui::kModShift;

namespace {

const fs::path kDataDir = RETDEC_GUI_TEST_DATA_DIR;

bool contains(std::string_view hay, std::string_view needle)
{
	return hay.find(needle) != std::string_view::npos;
}

std::string readFile(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void writeFile(const fs::path& p, std::string_view text)
{
	std::ofstream f(p, std::ios::binary);
	f << text;
}

void setEnv(const char* name, const std::string& value)
{
#if defined(_WIN32)
	_putenv_s(name, value.c_str());
#else
	if (value.empty())
		unsetenv(name);
	else
		setenv(name, value.c_str(), 1);
#endif
}

class MainWindowTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		dir = retdec::gui::test::scratchDir(std::string("main_window_")
		                                    + ::testing::UnitTest::GetInstance()->current_test_info()->name());
		// Never touch the user's settings.
		model::AppSettings::instance().setStoragePath(dir / "global-settings.json");
		settings.setStoragePath(dir / "settings.json");
		settings.general.restoreSession = false;
		store.setPath((dir / "layout.ini").string());
		setEnv("FAKE_DECOMPILER_EXIT", "");
		setEnv("FAKE_DECOMPILER_SLEEP", "");
		setEnv("FAKE_DECOMPILER_ARGS", "");
		makeWindow();
	}

	void TearDown() override
	{
		closeDialogs();
		win.reset();
		model::setApplicationDirPathOverride({});
		model::AppSettings::instance().setStoragePath({});
	}

	void makeWindow(bool theme = true)
	{
		win.reset();
		win = std::make_unique<ui::MainWindow>(
			h.ui, ui::MainWindowOptions{.headless = true, .layoutStore = &store, .settings = &settings, .applyTheme = theme});
		win->onQuitRequested = [this](int code) { quitCode = code; };
		h.frame(2);
	}

	/// Installs the fake decompiler as `<dir>/bin/retdec-decompiler` and makes
	/// the GUI look for its tools there.
	void useFakeDecompiler()
	{
		const fs::path bin = dir / "bin";
		fs::create_directories(bin);
		const fs::path exe = bin / "retdec-decompiler";
		fs::copy_file(kDataDir / "main_window_fake_decompiler.sh", exe, fs::copy_options::overwrite_existing);
		fs::permissions(exe, fs::perms::owner_all, fs::perm_options::add);
		setEnv("FAKE_DECOMPILER_FIXTURE", (kDataDir / "main_window").string());
		model::setApplicationDirPathOverride(bin);
	}

	fs::path writeFakeBinary(const std::string& name = "fake.bin", int size = 256)
	{
		std::string buf(std::size_t(size), '\0');
		buf[0] = 'M';
		buf[1] = 'Z';
		const fs::path p = dir / name;
		writeFile(p, buf);
		return p;
	}

	bool waitIdle(int timeoutMs = 20000)
	{
		return h.pumpUntil([this] { return !win->decompilerRunning() && !win->analysisRunning(); }, timeoutMs);
	}

	void closeDialogs()
	{
		if (!win) return;
		win->closeToolWindows();
		if (win->messageBox().isOpen()) win->messageBox().close();
		if (win->filePrompt().isOpen()) win->filePrompt().close();
		if (win->settingsDialog().isOpen()) win->settingsDialog().cancel();
		if (win->batchDialog().isOpen()) win->batchDialog().reject();
		if (win->runStageDialog().isOpen()) win->runStageDialog().reject();
		if (win->cStyleDialog().isOpen()) win->cStyleDialog().reject();
		if (win->rawOptionsDialog().isOpen()) win->rawOptionsDialog().reject();
		if (win->gotoAddressDialog().isOpen()) win->gotoAddressDialog().reject();
		if (win->gotoLineDialog().isOpen()) win->gotoLineDialog().reject();
		if (win->maxMemoryDialog().isOpen()) win->maxMemoryDialog().prompt().reject();
		if (win->aboutDialog().isOpen()) win->aboutDialog().close();
		if (win->shortcutsDialog().isOpen()) win->shortcutsDialog().close();
		if (win->docsViewer().isOpen()) win->docsViewer().close();
		if (win->docsViewer().message().isOpen()) win->docsViewer().message().close();
		if (win->questionOpen()) win->answerQuestion(99);
		h.frame();
	}

	/// Fake decompile of a fresh binary through the menu action; waits for the end.
	void decompileFixture(const std::string& name = "fib.exe")
	{
		useFakeDecompiler();
		binary = writeFakeBinary(name);
		win->openBinary(binary.string());
		h.frame();
		ASSERT_TRUE(win->triggerAction("analysis.run_full"));
		EXPECT_TRUE(win->decompilerRunning());
		ASSERT_TRUE(waitIdle());
		h.frame(2);
	}

	std::string screenshot(const std::string& name)
	{
		const char* keep = std::getenv("RETDEC_GUI_SCREENSHOT_DIR");
		const fs::path out = (keep && *keep) ? fs::path(keep) / name : dir / name;
		h.frame(3);
		EXPECT_TRUE(h.writePng(out.string()));
		return out.string();
	}

	HeadlessUi h{{1280.f, 800.f}};
	fs::path dir;
	fs::path binary;
	model::AppSettings settings;
	fuse::app::Settings store;
	std::unique_ptr<ui::MainWindow> win;
	std::optional<int> quitCode;
};

} // namespace

// ─── Layout (Qt mainwindow_layout_test) ──────────────────────────────────────

TEST_F(MainWindowTest, ConstructsAndHasCentralTabs)
{
	const auto docs = win->documentTabTitles();
	ASSERT_EQ(docs.size(), 5u);
	EXPECT_EQ(docs[0], "Decompiled C");
	EXPECT_EQ(docs[1], "Assembly");
	EXPECT_EQ(docs[2], "IR (SSA)");
	EXPECT_EQ(docs[3], "CFG");
	EXPECT_TRUE(contains(docs[4], "Synced"));
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocDecompiledC);
	EXPECT_TRUE(h.ui.alive(fuse::ui::widgets::tabPage(h.ui, win->documentTabsNode(), 4)));
}

TEST_F(MainWindowTest, BottomDockHasFourOutputTabs)
{
	const auto out = win->outputTabTitles();
	ASSERT_EQ(out.size(), 4u);
	EXPECT_EQ(out[0], "Console");
	EXPECT_EQ(out[1], "Problems");
	EXPECT_EQ(out[2], "History");
	EXPECT_EQ(out[3], "Progress");
	// The console is page 0 of the Output dock's tab widget.
	EXPECT_EQ(h.ui.parent(win->liveConsole().root()), fuse::ui::widgets::tabPage(h.ui, win->outputTabsNode(), 0));
	// Progress is present but hidden until analysis starts.
	EXPECT_FALSE(win->outputTabVisible(ui::MainWindow::kOutProgress));
	EXPECT_EQ(h.ui.text(fuse::ui::widgets::tabWidgetBar(h.ui, win->outputTabsNode())), "Console\nProblems\nHistory");
	win->startAnalysis();
	EXPECT_FALSE(win->analysisRunning()); // no project: nothing starts
	win->focusOutputTab(ui::MainWindow::kOutProgress);
	EXPECT_TRUE(win->outputTabVisible(ui::MainWindow::kOutProgress));
	EXPECT_EQ(win->currentOutputTab(), ui::MainWindow::kOutProgress);
}

TEST_F(MainWindowTest, LeftFunctionsDockIsSinglePanelNotTabbed)
{
	const auto dock = win->dockNode(ui::MainWindow::Dock::Functions);
	EXPECT_EQ(h.ui.idOf(dock), "dock_functions");
	const auto body = fuse::ui::widgets::dockBody(h.ui, dock);
	EXPECT_EQ(h.ui.parent(win->functionList().root()), body);
	for (auto c = h.ui.firstChild(body); h.ui.alive(c); c = h.ui.nextSibling(c))
		EXPECT_FALSE(h.ui.hasClass(c, "tabwidget"));
	EXPECT_GT(h.ui.rect(dock).w, 100.f);
}

TEST_F(MainWindowTest, RightWorkspaceHasStringsInspectBinaryTarget)
{
	const auto ws = win->workspaceTabTitles();
	ASSERT_EQ(ws.size(), 4u);
	EXPECT_EQ(ws[0], "Strings");
	EXPECT_EQ(ws[1], "Inspect");
	EXPECT_EQ(ws[2], "Binary");
	EXPECT_EQ(ws[3], "Target");
}

TEST_F(MainWindowTest, DocksAreNamedForLayoutPersistence)
{
	for (auto d: {ui::MainWindow::Dock::Functions, ui::MainWindow::Dock::Workspace, ui::MainWindow::Dock::Output})
	{
		EXPECT_FALSE(h.ui.idOf(win->dockNode(d)).empty());
		EXPECT_TRUE(win->dockOpen(d));
	}
	EXPECT_TRUE(contains(fuse::ui::widgets::dockSaveLayout(h.ui, win->dockAreaNode()), "dock dock_output bottom"));
}

TEST_F(MainWindowTest, NoModeToolbarRemains)
{
	// Toolbar: Open, Analyse, Stop — no v2 mode strip.
	EXPECT_TRUE(h.ui.alive(win->toolButton("file.open_binary")));
	EXPECT_TRUE(h.ui.alive(win->toolButton("analysis.run_full")));
	EXPECT_TRUE(h.ui.alive(win->toolButton("analysis.stop")));
	int buttons = 0;
	for (auto c = h.ui.firstChild(win->toolBarNode()); h.ui.alive(c); c = h.ui.nextSibling(c))
		buttons += h.ui.registry().get(h.ui.classOf(c)).name == "toolbutton";
	EXPECT_EQ(buttons, 3);
	EXPECT_FALSE(h.ui.alive(h.ui.find("modeToolbar")));
	EXPECT_TRUE(h.ui.disabled(win->toolButton("analysis.stop")));
	EXPECT_FALSE(h.ui.disabled(win->toolButton("analysis.run_full")));
}

TEST_F(MainWindowTest, TriageBannerStartsHidden)
{
	EXPECT_FALSE(win->triageBannerVisible());
	EXPECT_TRUE(win->triageBanner().isHidden());
}

TEST_F(MainWindowTest, StartScreenVisibleWhenNoBinary)
{
	EXPECT_EQ(win->centralStackIndex(), ui::MainWindow::kCentralEmpty);
	EXPECT_TRUE(h.ui.computed(win->startPageNode()).visible);
	EXPECT_FALSE(h.ui.computed(win->documentTabsNode()).visible);
	EXPECT_EQ(win->startEmptyState().title(), "Open a binary to begin");
	EXPECT_TRUE(h.ui.alive(win->startOpenBinaryButton()));
	EXPECT_TRUE(h.ui.alive(win->startOpenProjectButton()));
	// Open Binary… on the start page opens the file prompt.
	h.click(win->startOpenBinaryButton());
	EXPECT_TRUE(win->filePrompt().isOpen());
	win->filePrompt().choose(writeFakeBinary().string());
	h.frame();
	EXPECT_EQ(win->centralStackIndex(), ui::MainWindow::kCentralDocs);
	EXPECT_TRUE(h.ui.computed(win->documentTabsNode()).visible);
}

TEST_F(MainWindowTest, AiAssistantDockIsNotPresent)
{
	EXPECT_FALSE(h.ui.alive(h.ui.find("dock_ai_assistant")));
	EXPECT_FALSE(win->toolWindowOpen("AI Assistant"));
	EXPECT_TRUE(win->triggerAction("tools.ai_assistant"));
	EXPECT_TRUE(win->toolWindowOpen("AI Assistant"));
}

TEST_F(MainWindowTest, MenusMatchTheQtWindow)
{
	EXPECT_EQ(win->menuTitles(), (std::vector<std::string>{"File", "Analysis", "View", "Tools", "Help"}));
	const auto file = win->menuLabels("File");
	for (const char* want: {"Open Binary\xE2\x80\xA6", "Open Project\xE2\x80\xA6", "Recent Files \xE2\x96\xB8", "Save Project",
	                        "Save Project As\xE2\x80\xA6", "Save Decompiled\xE2\x80\xA6", "Export CMakeLists.txt\xE2\x80\xA6",
	                        "Export Decompile Bundle\xE2\x80\xA6", "Export Threat Intel\xE2\x80\xA6", "Export As \xE2\x96\xB8",
	                        "Batch Decompile\xE2\x80\xA6", "Quit"})
		EXPECT_NE(std::find(file.begin(), file.end(), want), file.end()) << want;
	EXPECT_EQ(win->menuLabels("Analysis").size(), 23u);
	const auto tools = win->menuLabels("Tools");
	EXPECT_NE(std::find(tools.begin(), tools.end(), "Visualisation plugins \xE2\x96\xB8"), tools.end());
	EXPECT_EQ(win->menuLabels("Help").size(), 4u);
	EXPECT_EQ(win->submenuLabels("Analysis", "Variable renamer").size(), 5u);
	EXPECT_EQ(win->submenuLabels("File", "Recent Files"), (std::vector<std::string>{"(empty)"}));
	// Save Project is disabled until something is open.
	EXPECT_FALSE(win->actionEnabled("file.save_project"));
	EXPECT_FALSE(win->actionEnabled("analysis.redecompile"));
	EXPECT_FALSE(win->actionEnabled("analysis.stop"));
}

TEST_F(MainWindowTest, MenuPopupShowsDisabledAndCheckedItems)
{
	win->setFastDecompile(true);
	const auto scope = win->openMenu("Analysis");
	ASSERT_TRUE(h.ui.alive(scope));
	h.frame();
	const auto menu = h.ui.firstChild(scope);
	int disabled = 0, checked = 0, items = 0;
	for (auto c = h.ui.firstChild(menu); h.ui.alive(c); c = h.ui.nextSibling(c))
	{
		++items;
		disabled += h.ui.disabled(c);
		checked += h.ui.checked(c);
	}
	EXPECT_EQ(items, int(win->menuLabels("Analysis").size()));
	EXPECT_EQ(disabled, 2); // Re-decompile Selected Function, Stop Analysis
	EXPECT_EQ(checked, 1);  // Fast decompile
	// Picking "Run Stage…" (line 2) runs it.
	h.ui.emit(win->menuBarNode(), fuse::ui::SignalKind::Submitted, {}); // not the owner: ignored
	const auto runStageItem = h.ui.nextSibling(h.ui.nextSibling(h.ui.firstChild(menu)));
	h.click(runStageItem);
	h.frame();
	EXPECT_FALSE(fuse::ui::widgets::popupOpen(h.ui, scope));
	// No binary: Run Stage asks for one first.
	EXPECT_TRUE(win->filePrompt().isOpen());
}

TEST_F(MainWindowTest, KeyboardShortcutsAreDistinctAndWork)
{
	std::set<std::pair<int, int>> seen;
	for (const std::string& id: win->actionIds())
	{
		const auto* a = win->action(id);
		if (a->key == Key::None) continue;
		EXPECT_TRUE(seen.insert({int(a->key), int(a->mods)}).second) << id;
	}
	EXPECT_EQ(win->action("file.open_binary")->shortcut, "Ctrl+O");
	EXPECT_EQ(win->action("file.save_project")->shortcut, "Ctrl+Shift+S");
	EXPECT_EQ(win->action("file.save_project_as")->shortcut, "Ctrl+Shift+A");
	EXPECT_EQ(win->action("analysis.run_full")->shortcut, "F5");
	EXPECT_EQ(win->action("analysis.stop")->shortcut, "F6");
	EXPECT_EQ(win->action("view.goto_address")->shortcut, "Ctrl+G");

	// Ctrl+1..5 raise the document tabs, through real key events on the window.
	h.ui.setFocus(win->root());
	h.key(Key::Num3, kModCtrl);
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocIR);
	h.key(Key::Num5, kModCtrl);
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocSynced);
	// Key events from a focused panel bubble through the window's capture handler.
	h.ui.setFocus(win->decompiledC().view().node());
	h.key(Key::Num1, kModCtrl);
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocDecompiledC);
	// Ctrl+G opens Go to address; F6 (disabled) is consumed without effect.
	h.key(Key::G, kModCtrl);
	EXPECT_TRUE(win->gotoAddressDialog().isOpen());
	win->gotoAddressDialog().reject();
	EXPECT_TRUE(win->handleShortcut(Key::F6, 0));
	EXPECT_FALSE(win->handleShortcut(Key::A, 0)); // plain letters belong to the panels
	EXPECT_TRUE(win->handleShortcut(Key::O, kModCtrl));
	EXPECT_TRUE(win->filePrompt().isOpen());
}

// ─── Comprehensive smoke (main-window parts) ─────────────────────────────────

TEST_F(MainWindowTest, EveryTabSwitches)
{
	for (int i = 0; i < ui::MainWindow::kDocCount; ++i)
	{
		win->raiseDocumentTab(i);
		h.frame();
		EXPECT_EQ(win->currentDocumentTab(), i);
	}
	for (int i = 0; i < ui::MainWindow::kWsCount; ++i)
	{
		win->focusWorkspaceTab(i);
		h.frame();
		EXPECT_EQ(win->currentWorkspaceTab(), i);
	}
	for (int i = 0; i < ui::MainWindow::kOutCount; ++i)
	{
		win->focusOutputTab(i);
		h.frame();
		EXPECT_EQ(win->currentOutputTab(), i);
	}
	for (int i = 0; i < 1000; ++i)
	{
		win->raiseDocumentTab(i % ui::MainWindow::kDocCount);
		win->focusWorkspaceTab(i % ui::MainWindow::kWsCount);
		if ((i & 0x3F) == 0) h.frame();
	}
	win->raiseDocumentTab(99); // out of range -> Decompiled C
	EXPECT_EQ(win->currentDocumentTab(), 0);
}

TEST_F(MainWindowTest, EveryDockToggleWorks)
{
	for (auto [d, id]: {std::pair{ui::MainWindow::Dock::Functions, "view.dock_functions"},
	                    std::pair{ui::MainWindow::Dock::Workspace, "view.dock_workspace"},
	                    std::pair{ui::MainWindow::Dock::Output, "view.dock_output"}})
	{
		EXPECT_TRUE(win->dockOpen(d));
		EXPECT_TRUE(win->actionChecked(id));
		win->triggerAction(id);
		h.frame();
		EXPECT_FALSE(win->dockOpen(d));
		EXPECT_FALSE(win->actionChecked(id));
		win->triggerAction(id);
		h.frame();
		EXPECT_TRUE(win->dockOpen(d));
		EXPECT_TRUE(win->actionChecked(id));
		// The dock's own ✕ keeps the menu in sync.
		fuse::ui::widgets::dockClose(h.ui, win->dockNode(d));
		EXPECT_FALSE(win->actionChecked(id));
		win->toggleDock(d);
		EXPECT_TRUE(win->dockOpen(d));
	}
}

TEST_F(MainWindowTest, EveryMenuActionTriggersWithoutCrash)
{
	for (int pass = 0; pass < 2; ++pass)
	{
		if (pass == 1) win->openBinary(writeFakeBinary().string()); // again with a project open
		for (const std::string& id: win->actionIds())
		{
			if (id == "file.quit") continue;
			win->triggerAction(id);
			h.frame();
			closeDialogs();
		}
		for (const std::string& title: win->menuTitles())
		{
			const auto scope = win->openMenu(title);
			h.frame();
			EXPECT_TRUE(fuse::ui::widgets::popupOpen(h.ui, scope)) << title;
			h.key(Key::Escape);
			h.frame();
		}
		for (int i = 0; i < 5; ++i)
			win->triggerSubmenu("Analysis", "Variable renamer", i);
		win->triggerSubmenu("File", "Recent Files", 0);
		closeDialogs();
	}
	EXPECT_EQ(win->nextRunOptions().varRenamer, "unified");
	EXPECT_FALSE(quitCode.has_value());
	SUCCEED();
}

TEST_F(MainWindowTest, DropFiles)
{
	EXPECT_TRUE(win->dropFiles({writeFakeBinary().string()}));
	EXPECT_EQ(win->centralStackIndex(), ui::MainWindow::kCentralDocs);
	EXPECT_FALSE(win->dropFiles({dir.string()}));
	EXPECT_TRUE(contains(win->statusMessageText(), "Skipped (not a file)"));
	EXPECT_FALSE(win->dropFiles({(dir / "does-not-exist.bin").string()}));
	EXPECT_FALSE(win->dropFiles({}));
}

TEST_F(MainWindowTest, OpenBinaryThenSaveAndReopenProjectRoundTrip)
{
	const fs::path bin = writeFakeBinary();
	win->openBinary(bin.string());
	h.frame();
	EXPECT_TRUE(win->actionEnabled("file.save_project"));
	EXPECT_EQ(win->statusFileText(), fs::absolute(bin).lexically_normal().string());
	EXPECT_TRUE(contains(win->windowTitle(), "fake.bin"));
	EXPECT_EQ(win->statusDecompileText(), "not decompiled");
	win->project()->setArch("x86");
	const fs::path proj = dir / "p.retdec";
	EXPECT_TRUE(win->saveProject(proj.string()));
	EXPECT_TRUE(fs::exists(proj));
	EXPECT_TRUE(win->openProject(proj.string()));
	h.frame();
	EXPECT_EQ(win->project()->arch(), "x86");
	EXPECT_EQ(win->lastProjectSavePath(), fs::absolute(proj).string());
	// Save Project (Ctrl+Shift+S) saves to the remembered path without a prompt.
	EXPECT_TRUE(win->triggerAction("file.save_project"));
	EXPECT_FALSE(win->filePrompt().isOpen());
	EXPECT_TRUE(contains(win->statusMessageText(), "Project saved"));
	// Recent files hold the binary and the project, newest first.
	ASSERT_GE(win->recentFiles().size(), 2u);
	EXPECT_EQ(win->recentFiles()[0], fs::absolute(proj).string());
}

TEST_F(MainWindowTest, OpenBinaryWithNonAsciiPath)
{
	const fs::path p = dir / "na\xC3\xAFve_ \xC3\xBC\xC3\xB1.bin";
	writeFile(p, std::string("MZ\0\0", 4));
	win->openBinary(p.string());
	h.frame();
	EXPECT_EQ(win->centralStackIndex(), ui::MainWindow::kCentralDocs);
}

TEST_F(MainWindowTest, OpenInvalidProjectShowsErrorButRecovers)
{
	const fs::path bogus = dir / "not-a-project.retdec";
	writeFile(bogus, "not json");
	EXPECT_FALSE(win->openProject(bogus.string()));
	h.frame();
	EXPECT_TRUE(win->messageBox().isOpen());
	EXPECT_TRUE(contains(win->messageBox().text(), "Failed to load project"));
	win->messageBox().close();
	win->openBinary(writeFakeBinary("after.bin").string());
	h.frame();
	EXPECT_EQ(win->centralStackIndex(), ui::MainWindow::kCentralDocs);
}

TEST_F(MainWindowTest, RepeatedOpenBinaryCyclesAreLeakFree)
{
	h.frame();
	const auto nodesBefore = h.ui.nodeCount();
	for (int i = 0; i < 50; ++i)
	{
		win->openBinary(writeFakeBinary(std::format("cycle_{}.bin", i), 64 + i).string());
		if (i % 10 == 0) h.frame();
	}
	h.frame(3);
	EXPECT_LT(h.ui.nodeCount(), nodesBefore + 400u);
	EXPECT_EQ(win->recentFiles().size(), std::size_t(ui::MainWindow::kMaxRecentFiles));
}

TEST_F(MainWindowTest, StatusBarMutatorsAreSafe)
{
	win->setStatusFile("x.bin");
	EXPECT_EQ(win->statusFileText(), "x.bin");
	win->setStatusFile("");
	EXPECT_EQ(win->statusFileText(), "No binary loaded");
	win->setStatusStage("Stage");
	EXPECT_EQ(win->statusStageText(), "Stage");
	win->setStatusFunctionCount(42);
	EXPECT_EQ(win->statusFunctionCountText(), "42 functions");
	win->setStatusFunctionCount(-1);
	EXPECT_EQ(win->statusFunctionCountText(), "\xE2\x80\x94 functions");
	win->setStatusDecompileState("cached");
	EXPECT_TRUE(win->statusDecompileVisible());
	win->setStatusDecompileState("");
	EXPECT_FALSE(win->statusDecompileVisible());
	win->setAnalysisProgress(0);
	win->setAnalysisProgress(100);
	EXPECT_FLOAT_EQ(win->progressBarValue(), 1.f);
	win->setAnalysisProgress(-9999);
	EXPECT_FLOAT_EQ(win->progressBarValue(), 0.f);
	win->setAnalysisProgress(99999);
	EXPECT_FLOAT_EQ(win->progressBarValue(), 1.f);
	win->showStatusMessage("hello", 100);
	EXPECT_EQ(win->statusMessageText(), "hello");
	h.pumpUntil([this] { return win->statusMessageText().empty(); }, 2000);
	EXPECT_EQ(win->statusMessageText(), "");
}

TEST_F(MainWindowTest, StartAndStopAnalysisLoops)
{
	win->stopAnalysis(); // never started
	win->openBinary(writeFakeBinary().string());
	for (int i = 0; i < 10; ++i)
	{
		win->startAnalysis();
		EXPECT_TRUE(win->analysisRunning());
		EXPECT_TRUE(win->progressBarVisible());
		EXPECT_FALSE(win->actionEnabled("analysis.run_full"));
		EXPECT_TRUE(win->actionEnabled("analysis.stop"));
		win->stopAnalysis();
		EXPECT_FALSE(win->analysisRunning());
		EXPECT_EQ(win->statusStageText(), "Stopped");
	}
	EXPECT_TRUE(win->outputTabVisible(ui::MainWindow::kOutProgress));
}

TEST_F(MainWindowTest, LiveConsoleHandlesPathologicalInputsAndFloods)
{
	auto& c = win->liveConsole();
	c.appendChunk(ui::LiveConsolePanel::Stream::Stdout, "");
	c.appendChunk(ui::LiveConsolePanel::Stream::Stdout, "\n\n\n");
	c.appendChunk(ui::LiveConsolePanel::Stream::Stdout, "na\xC3\xAFve \xE2\x82\xAC" "100 \x01\x02\x07 \xff\xfe\n");
	c.appendChunk(ui::LiveConsolePanel::Stream::Stdout, std::string(8192, 'A'));
	c.appendChunk(ui::LiveConsolePanel::Stream::Stdout, "\n");
	h.frame();
	EXPECT_FALSE(c.isEmpty());
	c.setMaxBlocks(5000);
	std::string buf;
	for (int i = 0; i < 100'000; ++i)
		buf += std::to_string(i) + "\n";
	c.appendChunk(ui::LiveConsolePanel::Stream::Stdout, buf);
	h.frame();
	EXPECT_LE(c.blockCount(), c.maxBlocks() + 2);
}

TEST_F(MainWindowTest, TriageBannerShowsAfterOpenBinary)
{
	EXPECT_TRUE(win->triageBanner().isHidden());
	win->openBinary(writeFakeBinary().string());
	h.frame();
	EXPECT_FALSE(win->triageBanner().isHidden());
	EXPECT_TRUE(win->triageBanner().actionsEnabled());
	EXPECT_TRUE(win->actionChecked("view.show_banner"));
	// View → Show triage banner hides / shows it.
	win->triggerAction("view.show_banner");
	EXPECT_TRUE(win->triageBanner().isHidden());
	win->triggerAction("view.show_banner");
	EXPECT_FALSE(win->triageBanner().isHidden());
	// Dismissing unticks the menu item.
	win->triageBanner().dismiss();
	EXPECT_FALSE(win->actionChecked("view.show_banner"));
	// "More ▼" lists the Qt actions.
	win->triageBanner().onMoreActionsRequested({100.f, 100.f});
	EXPECT_TRUE(win->triageMenu().isOpen());
	const auto labels = win->triageMenu().labels();
	EXPECT_NE(std::find(labels.begin(), labels.end(), "Copy binary path"), labels.end());
	EXPECT_NE(std::find(labels.begin(), labels.end(), "Copy SHA-256"), labels.end());
	ui::bindSystemClipboard(h.ui);
	win->triageMenu().trigger(std::size_t(win->triageMenu().indexOf("Copy binary path")));
	EXPECT_EQ(h.clipboard(), win->project()->binaryPath());
}

TEST_F(MainWindowTest, CachedDecompileLoadsInstantlyOnReopen)
{
	const fs::path bin = writeFakeBinary("cached.bin", 1024);
	writeFile(dir / "cached.gui-decompiled.c", "// cached decompile\nint cached_entry(void) { return 42; }\n");
	writeFile(dir / "cached.gui-decompiled.config.json",
	          R"({"functions":[{"name":"cached_entry","startAddr":"0x401000","endAddr":"0x401030",)"
	          R"("callingConvention":"cdecl","fncType":"decompilerDefined"}]})");
	const auto t0 = std::chrono::steady_clock::now();
	win->openBinary(bin.string());
	h.frame();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	EXPECT_LT(ms, 1000) << "cached open should be near-instant";
	ASSERT_TRUE(win->loadedArtifacts().has_value());
	EXPECT_EQ(win->statusDecompileText(), "cached");
	EXPECT_EQ(win->functionList().functions().size(), 1u);
	EXPECT_TRUE(contains(win->decompiledC().documentText(), "cached_entry"));
	EXPECT_EQ(win->statusFunctionCountText(), "1 functions");
}

// ─── Polish (main-window parts) ──────────────────────────────────────────────

TEST_F(MainWindowTest, EmptyStatesAndFindBarInTheDocumentTabs)
{
	win->openBinary(writeFakeBinary().string());
	h.frame();
	EXPECT_TRUE(win->decompiledC().emptyStateShown());
	EXPECT_TRUE(win->assembly().emptyStateShown());
	EXPECT_TRUE(win->irPanel().emptyStateShown());
	EXPECT_TRUE(win->cfgPanel().emptyStateVisible());
	EXPECT_FALSE(win->decompiledC().findBarVisible());
	win->decompiledC().showFindBar();
	EXPECT_TRUE(win->decompiledC().findBarVisible());
	win->decompiledC().hideFindBar();
	EXPECT_FALSE(win->decompiledC().findBarVisible());
}

// ─── Full decompile flow against the fake decompiler ─────────────────────────

TEST_F(MainWindowTest, OpenAnalyseLoadsArtefactsAndPopulatesPanels)
{
	setEnv("FAKE_DECOMPILER_ARGS", (dir / "args.txt").string());
	decompileFixture();
	ASSERT_EQ(win->lastDecompileExitCode(), 0);
	ASSERT_TRUE(win->loadedArtifacts().has_value());
	const fs::path cPath = dir / "fib.gui-decompiled.c";
	EXPECT_TRUE(fs::exists(cPath));
	const std::string args = readFile(dir / "args.txt");
	EXPECT_TRUE(contains(args, "-o\n" + fs::absolute(cPath).string())) << args;
	EXPECT_TRUE(contains(args, "-f\nplain"));

	// Status bar and actions.
	EXPECT_EQ(win->statusDecompileText(), "decompiled");
	EXPECT_EQ(win->statusFunctionCountText(), "2 functions");
	EXPECT_EQ(win->statusStageText(), "Idle");
	EXPECT_FALSE(win->progressBarVisible());
	EXPECT_TRUE(win->actionEnabled("analysis.run_full"));
	EXPECT_FALSE(win->actionEnabled("analysis.stop"));
	EXPECT_TRUE(win->outputTabVisible(ui::MainWindow::kOutProgress));

	// Function list + per-function views (first function shown).
	ASSERT_EQ(win->functionList().functions().size(), 2u);
	EXPECT_TRUE(contains(win->decompiledC().documentText(), "return fib(n - 1) + fib(n - 2);"));
	EXPECT_FALSE(win->decompiledC().emptyStateShown());
	EXPECT_TRUE(contains(win->assembly().view().text(), "push ebp"));
	EXPECT_TRUE(contains(win->irPanel().functionIr(), "define i32 @fib"));
	EXPECT_TRUE(contains(win->irPanel().moduleIr(), "define i32 @main"));
	EXPECT_EQ(win->cfgPanel().blockCount(), 3);
	EXPECT_EQ(win->cfgPanel().functionAddress(), 0x401000u);
	EXPECT_TRUE(contains(win->triPane().cPane().editor().text(), "int32_t fib"));
	EXPECT_TRUE(contains(win->triPane().asmPane().editor().text(), "0x401000"));
	EXPECT_EQ(win->strings().stringCount(), 1);
	EXPECT_EQ(win->strings().constantCount(), 1);
	EXPECT_GE(win->callGraph().graph().nodes().size(), 2u);

	// Console, history, problems.
	EXPECT_TRUE(contains(win->liveConsole().text(), "==> "));
	EXPECT_TRUE(contains(win->liveConsole().text(), "Running phase: Initialization"));
	EXPECT_TRUE(contains(win->liveConsole().text(), "<== retdec-decompiler finished (exit=0"));
	EXPECT_TRUE(contains(win->commandLog().text(), "retdec-decompiler"));
	bool logWarning = false, semantic = false, sidecars = false;
	for (int i = 0; i < win->diagnostics().totalCount(); ++i)
	{
		win->diagnostics().setTextFilter("");
	}
	for (int i = 0; i < win->diagnostics().visibleCount(); ++i)
	{
		const auto& d = win->diagnostics().visibleAt(i);
		logWarning |= contains(d.message, "unresolved indirect jump");
		semantic |= contains(d.message, "fibonacci");
		sidecars |= contains(d.message, "Sidecars:");
	}
	EXPECT_TRUE(logWarning);
	EXPECT_TRUE(semantic);
	EXPECT_TRUE(sidecars);
	EXPECT_GT(win->lastDecompileSubprocessMs(), 0);

	// Selecting another function updates every view.
	win->functionList().selectFunction(0x401030);
	h.frame();
	EXPECT_EQ(win->cfgPanel().functionAddress(), 0x401030u);
	EXPECT_TRUE(contains(win->irPanel().functionIr(), "define i32 @main"));
	EXPECT_TRUE(win->actionEnabled("analysis.redecompile"));

	// Navigation: Go to address on a function selects it and raises the tab.
	win->navigateToAddress(0x401000, ui::MainWindow::kDocAssembly);
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocAssembly);
	ASSERT_TRUE(win->functionList().selectedEntry().has_value());
	EXPECT_EQ(win->functionList().selectedEntry()->address, 0x401000u);
	// The CFG's block activation lands in Assembly.
	win->cfgPanel().onBlockActivated(0x401018);
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocAssembly);
	// The strings browser and the call graph navigate too.
	win->callGraph().onFunctionActivated(0x401030, "main");
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocDecompiledC);
	EXPECT_EQ(win->functionList().selectedEntry()->address, 0x401030u);

	// Tools → Copy selected function C.
	ui::bindSystemClipboard(h.ui);
	EXPECT_TRUE(win->triggerAction("tools.copy_function_c"));
	EXPECT_TRUE(contains(h.clipboard(), "int main(int argc")) << h.clipboard();

	// Reopening the binary reuses the cache (no decompiler run).
	win->openBinary(binary.string());
	h.frame();
	EXPECT_FALSE(win->decompilerRunning());
	EXPECT_EQ(win->statusDecompileText(), "cached");
	EXPECT_EQ(win->functionList().functions().size(), 2u);

	screenshot("main_window_loaded.png");
}

TEST_F(MainWindowTest, RedecompileSelectedFunctionPassesSelectFunctions)
{
	decompileFixture();
	setEnv("FAKE_DECOMPILER_ARGS", (dir / "args2.txt").string());
	win->functionList().selectFunction(0x401030);
	h.frame();
	ASSERT_TRUE(win->actionEnabled("analysis.redecompile"));
	EXPECT_TRUE(win->triggerAction("analysis.redecompile"));
	EXPECT_TRUE(contains(win->statusStageText(), "Re-decompiling main"));
	ASSERT_TRUE(waitIdle());
	const std::string args = readFile(dir / "args2.txt");
	EXPECT_TRUE(contains(args, "--select-functions\nmain")) << args;
	EXPECT_TRUE(contains(args, "--select-decode-only"));
	ASSERT_TRUE(win->functionList().selectedEntry().has_value());
	EXPECT_EQ(win->functionList().selectedEntry()->address, 0x401030u); // re-selected
}

TEST_F(MainWindowTest, NextRunOptionsReachTheCommandLine)
{
	useFakeDecompiler();
	setEnv("FAKE_DECOMPILER_ARGS", (dir / "args.txt").string());
	win->openBinary(writeFakeBinary("opts.exe").string());
	win->triggerAction("analysis.fast");
	win->triggerAction("analysis.keep_unreachable");
	win->triggerAction("analysis.raw_mode");
	win->triggerSubmenu("Analysis", "Variable renamer", 3); // hungarian
	win->nextRunOptions().cStyle.keepAllBrackets = true;
	ASSERT_TRUE(win->triggerAction("analysis.run_full"));
	ASSERT_TRUE(waitIdle());
	const std::string args = readFile(dir / "args.txt");
	EXPECT_TRUE(contains(args, "--backend-no-opts")) << args;
	EXPECT_TRUE(contains(args, "--keep-unreachable-funcs") || contains(args, "\n-k\n")) << args;
	EXPECT_TRUE(contains(args, "-m\nraw")) << args;
	EXPECT_TRUE(contains(args, "--backend-var-renamer\nhungarian")) << args;
	EXPECT_TRUE(contains(args, "--backend-keep-all-brackets")) << args;
}

TEST_F(MainWindowTest, DecompilerFailureIsReported)
{
	setEnv("FAKE_DECOMPILER_EXIT", "3");
	decompileFixture();
	EXPECT_EQ(win->lastDecompileExitCode(), 3);
	EXPECT_FALSE(win->loadedArtifacts().has_value());
	EXPECT_TRUE(win->messageBox().isOpen());
	EXPECT_TRUE(contains(win->messageBox().text(), "failed (exit 3)"));
	bool exitError = false;
	for (int i = 0; i < win->diagnostics().visibleCount(); ++i)
		exitError |= win->diagnostics().visibleAt(i).message == "Exit code 3";
	EXPECT_TRUE(exitError);
	EXPECT_EQ(win->currentOutputTab(), ui::MainWindow::kOutProblems); // errors raise Problems
	EXPECT_TRUE(win->actionEnabled("analysis.run_full"));
}

TEST_F(MainWindowTest, MissingDecompilerIsReported)
{
	model::setApplicationDirPathOverride(dir / "empty-bin");
	const char* path = std::getenv("PATH");
	const std::string savedPath = path ? path : "";
	setEnv("PATH", (dir / "nowhere").string());
	win->openBinary(writeFakeBinary().string());
	win->triggerAction("analysis.run_full");
	setEnv("PATH", savedPath);
	EXPECT_FALSE(win->decompilerRunning());
	EXPECT_TRUE(win->messageBox().isOpen());
	EXPECT_TRUE(contains(win->messageBox().text(), "retdec-decompiler was not found"));
}

TEST_F(MainWindowTest, StopCancelsTheRun)
{
	setEnv("FAKE_DECOMPILER_SLEEP", "30");
	useFakeDecompiler();
	win->openBinary(writeFakeBinary().string());
	ASSERT_TRUE(win->triggerAction("analysis.run_full"));
	EXPECT_TRUE(win->decompilerRunning());
	EXPECT_TRUE(win->actionEnabled("analysis.stop"));
	EXPECT_FALSE(win->triggerAction("analysis.run_full")); // disabled while running
	h.frame(3);
	const auto t0 = std::chrono::steady_clock::now();
	EXPECT_TRUE(win->handleShortcut(Key::F6, 0));
	EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
	EXPECT_FALSE(win->decompilerRunning());
	EXPECT_FALSE(win->analysisRunning());
	EXPECT_EQ(win->statusStageText(), "Stopped");
	EXPECT_FALSE(win->loadedArtifacts().has_value());
}

TEST_F(MainWindowTest, BatchDecompileRunsEveryBinary)
{
	useFakeDecompiler();
	const fs::path a = writeFakeBinary("a.exe"), b = writeFakeBinary("b.exe");
	ASSERT_TRUE(win->triggerAction("file.batch"));
	ASSERT_TRUE(win->batchDialog().isOpen());
	win->batchDialog().addPaths({a.string(), b.string()});
	win->batchDialog().start();
	EXPECT_TRUE(win->batchRunning());
	EXPECT_TRUE(contains(win->statusMessageText(), "Batch 1/2"));
	ASSERT_TRUE(h.pumpUntil([this] { return !win->batchRunning(); }, 20000));
	EXPECT_TRUE(fs::exists(dir / "a.gui-decompiled.c"));
	EXPECT_TRUE(fs::exists(dir / "b.gui-decompiled.c"));
	EXPECT_EQ(win->statusMessageText(), "Batch decompile finished (2 files)");
	EXPECT_FALSE(win->loadedArtifacts().has_value()); // batch runs do not load
}

TEST_F(MainWindowTest, HeadlessDecompileQuitsWithTheDecompilerStatus)
{
	useFakeDecompiler();
	win->enableQuitWhenDecompileFinishes(true);
	win->openBinary(writeFakeBinary("ci.exe").string());
	win->runDecompileForBenchmark();
	ASSERT_TRUE(h.pumpUntil([this] { return quitCode.has_value(); }, 20000));
	EXPECT_EQ(*quitCode, 0);
	ASSERT_TRUE(win->loadedArtifacts().has_value());
	EXPECT_EQ(win->loadedArtifacts()->functions.size(), 2u);
	EXPECT_FALSE(win->messageBox().isOpen());
	std::error_code ec;
	EXPECT_TRUE(contains(readFile(fs::temp_directory_path(ec) / "retdec-gui-decompile-timing.txt"), "EXIT=0"));

	quitCode.reset();
	setEnv("FAKE_DECOMPILER_EXIT", "5");
	win->runDecompileForBenchmark();
	ASSERT_TRUE(h.pumpUntil([this] { return quitCode.has_value(); }, 20000));
	EXPECT_EQ(*quitCode, 5);
	EXPECT_FALSE(win->messageBox().isOpen()); // never a modal box in CI mode
}

// ─── Project annotations, persistence, settings ──────────────────────────────

TEST_F(MainWindowTest, AnnotationsRoundTripThroughTheProject)
{
	decompileFixture();
	// Rename through the panel's prompt, as F2 does.
	win->functionList().triggerAction(ui::FunctionListPanel::Action::Rename, 0x401000);
	ASSERT_TRUE(win->functionList().promptOpen());
	win->functionList().acceptPrompt("fibonacci");
	EXPECT_TRUE(win->project()->isModified());
	const fs::path proj = dir / "fib.retdec";
	ASSERT_TRUE(win->saveProject(proj.string()));
	ASSERT_TRUE(win->openProject(proj.string()));
	h.frame();
	ASSERT_TRUE(win->loadedArtifacts().has_value()); // cached decompile reused
	bool renamed = false;
	for (const auto& f: win->functionList().functions())
		renamed |= f.address == 0x401000 && f.name == "fibonacci";
	EXPECT_TRUE(renamed);
}

TEST_F(MainWindowTest, LayoutAndRecentFilesPersist)
{
	const fs::path bin = writeFakeBinary();
	win->openBinary(bin.string());
	win->setDockOpen(ui::MainWindow::Dock::Output, false);
	win->triggerAction("view.show_ir");
	win->setFastDecompile(true);
	win->setWindowSize(1500, 900);
	win->saveLayout();
	makeWindow();
	EXPECT_FALSE(win->dockOpen(ui::MainWindow::Dock::Output));
	EXPECT_FALSE(win->actionChecked("view.dock_output"));
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocIR);
	EXPECT_TRUE(win->fastDecompile());
	EXPECT_TRUE(win->actionChecked("analysis.fast"));
	EXPECT_EQ(win->windowSize().w, 1500.f);
	ASSERT_FALSE(win->recentFiles().empty());
	EXPECT_EQ(win->recentFiles()[0], fs::absolute(bin).lexically_normal().string());
	EXPECT_EQ(win->submenuLabels("File", "Recent Files")[0], win->recentFiles()[0]);
	// Recent Files → item opens it; Clear empties the list.
	EXPECT_TRUE(win->triggerSubmenu("File", "Recent Files", 0));
	EXPECT_EQ(win->centralStackIndex(), ui::MainWindow::kCentralDocs);
	const auto items = win->submenuLabels("File", "Recent Files");
	EXPECT_TRUE(win->triggerSubmenu("File", "Recent Files", int(items.size()) - 1));
	EXPECT_TRUE(win->recentFiles().empty());

	unsigned rw = 0, rh = 0;
	win->onResizeRequested = [&](unsigned w, unsigned hh) { rw = w, rh = hh; };
	win->triggerAction("view.reset_layout");
	EXPECT_TRUE(win->dockOpen(ui::MainWindow::Dock::Output));
	EXPECT_EQ(win->currentDocumentTab(), ui::MainWindow::kDocDecompiledC);
	EXPECT_EQ(rw, 1280u);
	EXPECT_EQ(rh, 800u);
	EXPECT_EQ(win->statusMessageText(), "Layout reset to defaults");
}

TEST_F(MainWindowTest, SettingsChangesApplyLive)
{
	EXPECT_EQ(win->theme(), ui::ThemeKind::Mocha);
	EXPECT_TRUE(h.ui.hasClass(h.ui.root(), "retdec-theme-mocha"));
	settings.general.theme = model::GeneralSettings::Theme::Light;
	settings.notifySettingsChanged();
	h.frame();
	EXPECT_EQ(win->theme(), ui::ThemeKind::Latte);
	EXPECT_EQ(win->decompiledC().view().palette().background.r, ui::codePaletteFor(ui::ThemeKind::Latte).background.r);
	screenshot("main_window_latte.png");
	settings.general.theme = model::GeneralSettings::Theme::Dark;
	settings.general.showLineNumbers = false;
	settings.notifySettingsChanged();
	EXPECT_EQ(win->theme(), ui::ThemeKind::Mocha);
	// Analysis → Configure… stores --config and notifies (the Target panel follows).
	const fs::path cfg = dir / "cfg.json";
	writeFile(cfg, "{}");
	win->triggerAction("analysis.configure");
	ASSERT_TRUE(win->filePrompt().isOpen());
	win->filePrompt().choose(cfg.string());
	EXPECT_EQ(settings.decompiler.extraConfigPath, cfg.string());
	EXPECT_TRUE(contains(win->statusMessageText(), "Decompiler --config"));
}

TEST_F(MainWindowTest, QuitConfirmsUnsavedChanges)
{
	win->triggerAction("file.quit");
	EXPECT_EQ(quitCode, 0); // nothing open: quits at once
	quitCode.reset();
	win->openBinary(writeFakeBinary().string());
	win->project()->setArch("arm");
	win->triggerAction("file.quit");
	ASSERT_TRUE(win->questionOpen());
	EXPECT_TRUE(contains(win->questionText(), "unsaved project changes"));
	win->answerQuestion(2); // Cancel
	EXPECT_FALSE(quitCode.has_value());
	win->triggerAction("file.quit");
	win->answerQuestion(1); // Discard
	EXPECT_EQ(quitCode, 0);
	// With an analysis running, the first question is "Quit anyway?".
	quitCode.reset();
	win->startAnalysis();
	win->requestQuit();
	ASSERT_TRUE(win->questionOpen());
	EXPECT_TRUE(contains(win->questionText(), "in progress"));
	win->answerQuestion(1); // No
	EXPECT_TRUE(win->analysisRunning());
	EXPECT_FALSE(quitCode.has_value());
}

TEST_F(MainWindowTest, ToolWindowsOpenFromTheToolsMenu)
{
	for (auto [id, title]: {std::pair{"tools.signature_studio", "Signature Studio"}, std::pair{"tools.call_graph", "Call Graph"},
	                        std::pair{"tools.type_hierarchy", "Type Hierarchy"}, std::pair{"tools.ai_assistant", "AI Assistant"}})
	{
		EXPECT_TRUE(win->triggerAction(id));
		h.frame();
		EXPECT_TRUE(win->toolWindowOpen(title)) << title;
		win->closeToolWindows();
		EXPECT_FALSE(win->toolWindowOpen(title));
	}
	EXPECT_EQ(win->submenuLabels("Tools", "Visualisation plugins"),
	          (std::vector<std::string>{"(no visualisation plugins loaded)"}));
	EXPECT_FALSE(win->triggerSubmenu("Tools", "Visualisation plugins", 0));
	// Compare needs decompiled C first.
	win->triggerAction("tools.compare");
	EXPECT_TRUE(win->messageBox().isOpen());
}

TEST_F(MainWindowTest, CompareAgainstAFile)
{
	decompileFixture();
	const fs::path other = dir / "other.c";
	writeFile(other, "int main(void) { return 1; }\n");
	win->triggerAction("tools.compare");
	ASSERT_TRUE(win->filePrompt().isOpen());
	win->filePrompt().choose(other.string());
	EXPECT_TRUE(win->toolWindowOpen("Compare"));
	EXPECT_GT(win->comparePanel().currentDiff().linesAdded + win->comparePanel().currentDiff().linesRemoved, 0);
}

TEST_F(MainWindowTest, ExportCMakeAndBundle)
{
	decompileFixture();
	win->triggerAction("file.export_cmake");
	ASSERT_TRUE(win->filePrompt().isOpen());
	win->filePrompt().choose((dir / "CMakeLists.txt").string());
	const std::string cmake = readFile(dir / "CMakeLists.txt");
	EXPECT_TRUE(contains(cmake, "add_executable(fib_gui_decompiled \"fib.gui-decompiled.c\")")) << cmake;
	win->triggerAction("file.export_bundle");
	ASSERT_TRUE(win->filePrompt().isOpen());
	win->filePrompt().choose((dir / "bundle.zip").string());
	EXPECT_TRUE(fs::exists(dir / "bundle.zip"));
	EXPECT_TRUE(contains(win->statusMessageText(), "Exported bundle"));
}

TEST_F(MainWindowTest, SystemClipboardBinding)
{
	ui::bindSystemClipboard(h.ui);
	EXPECT_TRUE(h.ui.setClipboardText("retdec clipboard"));
	EXPECT_EQ(h.clipboard(), "retdec clipboard");
}

// ─── Rendering ───────────────────────────────────────────────────────────────

TEST_F(MainWindowTest, RendersEmptyWindowInAHeadlessAppWindow)
{
	// The same shell inside a fuse::app AppWindow (what the executable runs),
	// presented by the headless CPU rasteriser.
	win.reset();
	fuse::app::Application app{{.name = "retdec-gui-test", .loadSettings = false}};
	auto& aw = app.createWindow({.title = "RetDec", .width = 1280, .height = 800});
	{
		ui::MainWindow mw(aw.ui(), ui::MainWindowOptions{.headless = true, .settings = &settings});
		int code = -1;
		mw.onQuitRequested = [&](int c) {
			code = c;
			app.quit(c);
		};
		app.step(0.f);
		app.step(0.f);
		EXPECT_GE(aw.framesPresented(), 1u);
		const char* keep = std::getenv("RETDEC_GUI_SCREENSHOT_DIR");
		const fs::path out = (keep && *keep) ? fs::path(keep) / "main_window_empty.png" : dir / "main_window_empty.png";
		EXPECT_TRUE(aw.writePng(out.string()));
		// The image is not blank: the Mocha base colour fills most of it.
		const auto& img = aw.image();
		ASSERT_EQ(img.width, 1280);
		std::set<uint32_t> colours;
		for (int y = 0; y < img.height; y += 37)
			for (int x = 0; x < img.width; x += 41)
				colours.insert(img.at(x, y));
		EXPECT_GT(colours.size(), 3u);
		mw.triggerAction("file.quit");
		EXPECT_EQ(code, 0);
		EXPECT_FALSE(app.step(0.f));
	}
}
