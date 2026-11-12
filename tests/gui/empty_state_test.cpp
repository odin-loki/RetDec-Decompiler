/**
 * @file tests/gui/empty_state_test.cpp
 * @brief EmptyState, the panel scaffolding, the address context menu, and the
 *        Qt polish_integration checks of the code panels (empty states, find
 *        bar, unified context-menu labels).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/assembly_panel.h"
#include "retdec/gui/ui/decompiled_c_panel.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/ir_panel.h"

#include <gtest/gtest.h>

#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;

TEST(CodePolishIntegration, EmptyStateWidgetConstructs)
{
	HeadlessUi h;
	ui::EmptyState w(h.ui, h.ui.root());
	w.setTitle("No content");
	w.setHint("Run decompilation or select a function.");
	w.setIcon("");
	h.frame();
	EXPECT_TRUE(h.ui.alive(w.root()));
	EXPECT_TRUE(h.ui.computed(w.root()).visible);
	EXPECT_EQ(w.title(), "No content");
	EXPECT_EQ(w.hint(), "Run decompilation or select a function.");
	EXPECT_TRUE(w.hintShown());
	EXPECT_FALSE(w.iconShown());
	w.setHint("");
	w.setIcon("?");
	h.frame();
	EXPECT_FALSE(w.hintShown());
	EXPECT_TRUE(w.iconShown());
	EXPECT_EQ(w.icon(), "?");
	EXPECT_GT(h.ui.rect(w.root()).h, 0.f);
}

TEST(CodePolishIntegration, DecompiledCPanelFindBarConstructs)
{
	HeadlessUi h;
	ui::DecompiledCPanel panel(h.ui, h.ui.root());
	h.frame();
	EXPECT_FALSE(panel.findBarVisible());
	EXPECT_FALSE(h.ui.computed(h.ui.parent(panel.findEdit())).visible);

	panel.setSource(std::string{});
	EXPECT_NO_THROW(panel.showFindBar());
	h.frame();
	EXPECT_TRUE(panel.findBarVisible());
	EXPECT_TRUE(h.ui.computed(h.ui.parent(panel.findEdit())).visible);
	EXPECT_NO_THROW(panel.hideFindBar());
	h.frame();
	EXPECT_FALSE(h.ui.computed(h.ui.parent(panel.findEdit())).visible);

	panel.setSource("int main(void) { return 0; }\n");
	panel.showFindBar();
	panel.setFindText("main");
	EXPECT_NO_THROW(panel.showFindBar());
	EXPECT_NO_THROW(panel.hideFindBar());
}

TEST(CodePolishIntegration, EmptyStatesSwapWithContent)
{
	HeadlessUi h;
	ui::AssemblyPanel a(h.ui, h.ui.root());
	h.frame();
	EXPECT_TRUE(h.ui.computed(a.emptyState().root()).visible);
	EXPECT_FALSE(h.ui.computed(a.view().node()).visible);
	a.setAssemblyText("0x401000:  push ebp\n");
	h.frame();
	EXPECT_FALSE(h.ui.computed(a.emptyState().root()).visible);
	EXPECT_TRUE(h.ui.computed(a.view().node()).visible);
}

TEST(CodePolishIntegration, UnifiedContextMenuLabels)
{
	EXPECT_STREQ(ui::kCopyAddressLabel, "Copy Address");
	EXPECT_STREQ(ui::kGoToFunctionLabel, "Go to Function");
}

TEST(CodePolishIntegration, ParseFirstAddressFromDsmLine)
{
	const auto addr = model::parseFirstAddress("0x401000:  push    ebp");
	ASSERT_TRUE(addr.has_value());
	EXPECT_EQ(*addr, 0x401000ull);
	EXPECT_FALSE(model::parseFirstAddress("nop").has_value());
}

TEST(CodePolishIntegration, AssemblyPanelUsesCustomContextMenu)
{
	HeadlessUi h;
	ui::AssemblyPanel panel(h.ui, h.ui.root());
	panel.setAssemblyText("0x401000:  push ebp\n");
	h.frame();
	const fuse::ui::Rect r = h.ui.contentRect(panel.view().node());
	h.clickAt({r.x + 60.f, r.y + 6.f}, fuse::ui::MouseButton::Right);
	ASSERT_TRUE(panel.contextMenu().isOpen());
	EXPECT_EQ(panel.contextMenu().labels(), (std::vector<std::string>{"Copy Address", "Go to Function"}));
}

// ── ContextMenu / address menu ───────────────────────────────────────────────

TEST(AddressMenu, ActionsRunAndDisabledItemsDoNot)
{
	HeadlessUi h;
	ui::ContextMenu menu(h.ui);
	int ran = 0;
	menu.open(h.ui.root(), {50.f, 50.f},
	          {{"First", true, [&] { ran += 1; }}, {"-", true, {}}, {"Disabled", false, [&] { ran += 100; }}, {"Last", true, [&] { ran += 10; }}});
	h.frame();
	ASSERT_TRUE(menu.isOpen());
	EXPECT_EQ(menu.labels().size(), 4u);
	EXPECT_FALSE(menu.isEnabled(1)); // separator
	EXPECT_FALSE(menu.isEnabled(2));
	EXPECT_FALSE(menu.trigger(2));
	EXPECT_EQ(menu.indexOf("Last"), 3);
	EXPECT_EQ(menu.indexOf("nope"), -1);
	// Keyboard: focus starts on the first enabled item; Down skips nothing
	// disabled in FuseUI's own menu, so pick "Last" with the pointer.
	const fuse::ui::NodeHandle overlay = fuse::ui::widgets::overlayLayer(h.ui);
	fuse::ui::NodeHandle scope = h.ui.firstChild(overlay);
	while (h.ui.alive(h.ui.nextSibling(scope)))
		scope = h.ui.nextSibling(scope);
	fuse::ui::NodeHandle item = h.ui.firstChild(h.ui.firstChild(scope));
	for (int i = 0; i < 3; ++i)
		item = h.ui.nextSibling(item);
	EXPECT_EQ(h.ui.text(item), "Last");
	h.click(item);
	EXPECT_EQ(ran, 10);
	EXPECT_FALSE(menu.isOpen());
	// Clicking the disabled item does nothing (the menu stays open).
	menu.open(h.ui.root(), {50.f, 50.f}, {{"A", false, [&] { ran = -1; }}, {"B", true, [&] { ran = 7; }}});
	h.frame();
	scope = h.ui.firstChild(overlay);
	while (h.ui.alive(h.ui.nextSibling(scope)))
		scope = h.ui.nextSibling(scope);
	h.click(h.ui.firstChild(h.ui.firstChild(scope)));
	EXPECT_EQ(ran, 10);
	EXPECT_TRUE(menu.isOpen());
	h.key(fuse::ui::Key::Escape);
	h.frame();
	EXPECT_FALSE(menu.isOpen());
}

TEST(AddressMenu, CopyAddressAndGoToFunction)
{
	HeadlessUi h;
	uint64_t went = 0;
	auto actions = ui::addressMenuActions(h.ui, 0x401abc, [&](uint64_t a) { went = a; });
	ASSERT_EQ(actions.size(), 2u);
	EXPECT_EQ(actions[0].label, "Copy Address");
	EXPECT_TRUE(actions[0].enabled);
	actions[0].run();
	EXPECT_EQ(h.clipboard(), "0x401abc");
	actions[1].run();
	EXPECT_EQ(went, 0x401abcu);
	auto none = ui::addressMenuActions(h.ui, std::nullopt, [&](uint64_t) { went = 1; });
	EXPECT_FALSE(none[0].enabled);
	EXPECT_FALSE(none[1].enabled);
	ui::copyAddressToClipboard(h.ui, 0x10);
	EXPECT_EQ(h.clipboard(), "0x10");
}

TEST(PanelScaffolding, ShortcutMatchingAndTextEntry)
{
	HeadlessUi h;
	fuse::ui::Event e;
	e.type = fuse::ui::EventType::KeyDown;
	e.key = fuse::ui::Key::F;
	e.mods = fuse::ui::kModCtrl;
	EXPECT_TRUE(ui::isKey(e, fuse::ui::Key::F, fuse::ui::kModCtrl));
	EXPECT_FALSE(ui::isKey(e, fuse::ui::Key::F));
	e.mods = fuse::ui::kModCtrl | fuse::ui::kModSuper; // Super is ignored
	EXPECT_TRUE(ui::isKey(e, fuse::ui::Key::F, fuse::ui::kModCtrl));
	const auto edit = fuse::ui::widgets::lineEdit(h.ui, h.ui.root(), "");
	const auto label = fuse::ui::widgets::label(h.ui, h.ui.root(), "x");
	EXPECT_TRUE(ui::isTextEntry(h.ui, edit));
	EXPECT_FALSE(ui::isTextEntry(h.ui, label));
	ui::setLineEditText(h.ui, edit, "abc");
	EXPECT_EQ(h.ui.text(edit), "abc");
	EXPECT_EQ(h.ui.editBuffer(edit).text(), "abc");
}

TEST(PanelScaffolding, PanelsCanBeDestroyedAndRecreated)
{
	HeadlessUi h;
	const auto before = h.ui.nodeCount();
	for (int i = 0; i < 3; ++i)
	{
		auto c = std::make_unique<ui::DecompiledCPanel>(h.ui, h.ui.root());
		auto a = std::make_unique<ui::AssemblyPanel>(h.ui, h.ui.root());
		auto ir = std::make_unique<ui::IRPanel>(h.ui, h.ui.root());
		c->setSource("int x;");
		h.frame();
	}
	h.frame();
	// Only tooltips (kept in the overlay layer) outlive their panels.
	EXPECT_LT(h.ui.nodeCount(), before + 200u);
}
