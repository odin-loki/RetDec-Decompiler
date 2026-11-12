/**
 * @file tests/gui/diagnostics_panel_test.cpp
 * @brief DiagnosticsPanel (Problems) in a headless UiContext: severity and
 *        text filters, TSV copy / save, navigation on double-click / Enter,
 *        error notification, eviction (Qt comprehensive-smoke check).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/diagnostics_panel.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;
using model::Severity;

class DiagnosticsPanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		panel = std::make_unique<ui::DiagnosticsPanel>(h.ui, h.ui.root());
		panel->onAddressActivated = [this](uint64_t a) { addrs.push_back(a); };
		panel->onFunctionActivated = [this](const std::string& f) { funcs.push_back(f); };
		panel->onErrorAdded = [this] { ++errors; };
		h.frame();
	}
	void fill()
	{
		panel->addMessage(Severity::Info, "Loading", "Loaded 12 functions");
		panel->addMessage(Severity::Warning, "decompiler log", "Unknown section .foo", 0x401000);
		model::Diagnostic d;
		d.severity = Severity::Error;
		d.source = "semantic";
		d.message = "Decompilation of main failed";
		d.function = "main";
		panel->addDiagnostic(d);
		h.frame();
	}
	HeadlessUi h;
	std::unique_ptr<ui::DiagnosticsPanel> panel;
	std::vector<uint64_t> addrs;
	std::vector<std::string> funcs;
	int errors = 0;
};

TEST_F(DiagnosticsPanelTest, ShowsRowsAndCount)
{
	EXPECT_EQ(panel->countText(), "0 entries");
	fill();
	EXPECT_EQ(panel->visibleCount(), 3);
	EXPECT_EQ(panel->countText(), "3 entries");
	EXPECT_EQ(panel->cellText(1, 0).find("Warning") != std::string::npos, true);
	EXPECT_EQ(panel->cellText(1, 1), "decompiler log");
	EXPECT_EQ(panel->cellText(1, 2), "Unknown section .foo");
	EXPECT_EQ(panel->cellText(1, 3), "0x401000");
	EXPECT_EQ(panel->cellText(0, 3), "");
	EXPECT_EQ(panel->cellText(2, 4), "main");
	EXPECT_EQ(errors, 1);
}

TEST_F(DiagnosticsPanelTest, SeverityFilterCombo)
{
	fill();
	h.ui.setValue(panel->filterCombo(), 2.f, true); // Warnings
	EXPECT_EQ(panel->visibleCount(), 1);
	EXPECT_EQ(panel->visibleAt(0).severity, Severity::Warning);
	EXPECT_EQ(panel->countText(), "1 entries");
	panel->setSeverityFilter(model::SeverityFilter::Errors);
	EXPECT_EQ(panel->visibleCount(), 1);
	EXPECT_EQ(panel->visibleAt(0).severity, Severity::Error);
	// New entries respect the filter.
	panel->addMessage(Severity::Info, "x", "info");
	EXPECT_EQ(panel->visibleCount(), 1);
	panel->addMessage(Severity::Error, "x", "another error");
	EXPECT_EQ(panel->visibleCount(), 2);
	panel->setSeverityFilter(model::SeverityFilter::All);
	EXPECT_EQ(panel->visibleCount(), 5);
}

TEST_F(DiagnosticsPanelTest, TextFilterReplacesSeverityFilterWhileNonEmpty)
{
	fill();
	panel->setSeverityFilter(model::SeverityFilter::Info);
	h.ui.setFocus(panel->searchEdit());
	h.type("MAIN");
	EXPECT_EQ(panel->visibleCount(), 1);
	EXPECT_EQ(panel->visibleAt(0).severity, Severity::Error);
	EXPECT_EQ(panel->countText(), "1 entries");
	panel->setTextFilter("");
	EXPECT_EQ(panel->visibleCount(), 1);
	EXPECT_EQ(panel->visibleAt(0).severity, Severity::Info);
}

TEST_F(DiagnosticsPanelTest, CopyAndSaveVisibleTsv)
{
	fill();
	panel->setSeverityFilter(model::SeverityFilter::Warnings);
	h.click(panel->copyButton());
	EXPECT_EQ(h.clipboard(), "Warning\tdecompiler log\tUnknown section .foo");
	const auto dir = retdec::gui::test::scratchDir("diagnostics");
	ASSERT_TRUE(panel->saveVisibleTo(dir / "d.tsv"));
	std::ifstream in(dir / "d.tsv");
	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	EXPECT_EQ(content, "Warning\tdecompiler log\tUnknown section .foo");
}

TEST_F(DiagnosticsPanelTest, EnterAndDoubleClickNavigate)
{
	fill();
	// Keyboard: select the warning row, Enter.
	h.ui.setFocus(panel->table());
	h.key(fuse::ui::Key::Down); // row 0
	h.key(fuse::ui::Key::Down); // row 1
	h.key(fuse::ui::Key::Enter);
	ASSERT_EQ(addrs.size(), 1u);
	EXPECT_EQ(addrs[0], 0x401000u);
	// Row without an address but with a function.
	panel->activateRow(2);
	ASSERT_EQ(funcs.size(), 1u);
	EXPECT_EQ(funcs[0], "main");
	// Row with neither does nothing.
	panel->activateRow(0);
	EXPECT_EQ(addrs.size(), 1u);
	EXPECT_EQ(funcs.size(), 1u);
	// Double-click: click the selected row twice at the same spot.
	const fuse::ui::Rect r = h.ui.contentRect(panel->table());
	for (float y = r.y + 32.f; y < r.bottom(); y += 6.f)
	{
		h.clickAt({r.x + 300.f, y});
		if (int(h.ui.value(panel->table())) == 1)
		{
			h.doubleClickAt({r.x + 300.f, y});
			break;
		}
	}
	ASSERT_EQ(addrs.size(), 2u);
	EXPECT_EQ(addrs[1], 0x401000u);
}

TEST_F(DiagnosticsPanelTest, EvictsBeyondCapAndClears)
{
	panel->setMaxEntries(1000);
	for (int i = 0; i < 5000; ++i)
		panel->addMessage(Severity::Info, "stress", std::to_string(i));
	EXPECT_LE(panel->totalCount(), 1000);
	EXPECT_EQ(panel->visibleAt(0).message, "4000");
	panel->clear();
	EXPECT_EQ(panel->visibleCount(), 0);
	EXPECT_EQ(panel->countText(), "0 entries");
}

TEST_F(DiagnosticsPanelTest, SetDiagnosticsReplacesAll)
{
	fill();
	panel->setDiagnostics({{Severity::Warning, "w", "src", 0, ""}});
	EXPECT_EQ(panel->totalCount(), 1);
	EXPECT_EQ(errors, 1); // no new error
}
