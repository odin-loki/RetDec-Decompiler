/**
 * @file src/gui/ui/empty_state.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/empty_state.h"

#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/widgets/widgets.hpp>
#include <span>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

/// The panel root class: a plain flex column whose events are forwarded to the
/// PanelEventHandler stored in the node's user data.
struct PanelRootW
{
	static constexpr std::string_view kName = "retdec-panel";
	static constexpr A11yRole kRole = A11yRole::Group;
	static ComputedStyle defaults()
	{
		ComputedStyle s;
		s.direction = Direction::Column;
		s.grow = 1.f;
		s.minWidth = Length::px(0.f);
		s.minHeight = Length::px(0.f);
		return s;
	}
	static bool onEvent(EventContext& ec, const Event& e)
	{
		if (ec.phase == Phase::Target && ec.target != ec.node) return false;
		auto* h = reinterpret_cast<PanelEventHandler*>(static_cast<std::uintptr_t>(ec.ui.userData(ec.node)));
		if (!h || !h->handle) return false;
		return h->handle(ec, e);
	}
};

constexpr std::string_view kRetdecCss = R"css(
retdec-panel { background: transparent; }
.retdec-toolbar { flex-direction: row; gap: 4px; padding: 2px 4px; align-items: center; background: var(--surface, #2a2b32); }
.retdec-toolbar toolbutton { padding: 3px 8px; }
.retdec-muted { color: var(--text-dim, #9a9cab); padding: 2px 6px; }
.retdec-empty { align-items: center; justify-content: center; gap: 8px; padding: 24px; flex-grow: 1; }
.retdec-empty-icon { font-size: 40px; color: var(--text-dim, #9a9cab); }
.retdec-empty-title { font-size: 16px; color: var(--text, #e6e6eb); }
.retdec-empty-hint { color: var(--text-dim, #9a9cab); }
.retdec-badge { background: var(--surface-hi, #34353e); color: var(--text, #e6e6eb); border-radius: 4px; padding: 2px 8px; }
.retdec-badge-warning { background: #fab387; color: #1e1e2e; }
.retdec-pane-title { color: var(--text-dim, #9a9cab); padding: 2px 6px; text-align: center; background: #181825; }
.retdec-findbar { flex-direction: row; gap: 4px; padding: 2px 4px; align-items: center; background: var(--surface, #2a2b32); }
.retdec-banner { flex-direction: row; gap: 8px; padding: 6px 6px 6px 10px; align-items: center; min-height: 44px;
  background: var(--surface, #2a2b32); border: 1px var(--border, #474955); }
.retdec-banner-title { font-size: 15px; color: #ffffff; }
.retdec-status { color: var(--text-dim, #9a9cab); flex-grow: 1; }
.retdec-mono { font-family: "JetBrains Mono", monospace; }
.retdec-bold { font-weight: bold; }
.md-code { font-family: "JetBrains Mono", monospace; }
)css";

} // namespace

// ── EmptyState ───────────────────────────────────────────────────────────────

EmptyState::EmptyState(UiContext& ui, NodeHandle parent): ui_(ui)
{
	ensureRetdecUiClasses(ui);
	root_ = w::panel(ui, parent);
	ui.addClass(root_, "retdec-empty");
	ui.setCss(root_, "flex-grow", "1");
	ui.setCss(root_, "align-items", "center");
	ui.setCss(root_, "justify-content", "center");
	icon_ = w::label(ui, root_, "");
	ui.addClass(icon_, "retdec-empty-icon");
	title_ = w::label(ui, root_, "");
	ui.addClass(title_, "retdec-empty-title");
	hint_ = w::label(ui, root_, "");
	ui.addClass(hint_, "retdec-empty-hint");
	setShown(ui, icon_, false);
	setShown(ui, hint_, false);
}

void EmptyState::setTitle(std::string_view title)
{
	ui_.setText(title_, title);
}

void EmptyState::setHint(std::string_view hint)
{
	ui_.setText(hint_, hint);
	hintShown_ = !hint.empty();
	setShown(ui_, hint_, hintShown_);
}

void EmptyState::setIcon(std::string_view glyph)
{
	ui_.setText(icon_, glyph);
	iconShown_ = !glyph.empty();
	setShown(ui_, icon_, iconShown_);
}

std::string EmptyState::title() const
{
	return std::string(ui_.text(title_));
}

std::string EmptyState::hint() const
{
	return std::string(ui_.text(hint_));
}

std::string EmptyState::icon() const
{
	return std::string(ui_.text(icon_));
}

// ── Scaffolding ──────────────────────────────────────────────────────────────

void ensureRetdecUiClasses(UiContext& ui)
{
	WidgetRegistry& r = ui.registry();
	if (r.find(PanelRootW::kName) != 0xFFFFFFFFu) return;
	if (r.find("tableview") == 0xFFFFFFFFu) w::install(ui);
	// Monospace code / addresses and the symbol glyphs (✓ ▶ ⚠ …) need the
	// bundled faces; AppWindow registers them, bare contexts (tests) may not.
	if (!ui.font() || ui.resolveFont("monospace") == ui.font()) ui.registerBundledFonts();
	r.registerWidget<PanelRootW>();
	registerCodeViewClass(ui);
	(void)ui.addStyleSheet(kRetdecCss);
}

NodeHandle createPanelRoot(UiContext& ui, NodeHandle parent, PanelEventHandler* handler, std::string_view cssClass)
{
	ensureRetdecUiClasses(ui);
	auto made = ui.create(PanelRootW::kName, parent);
	if (!made) return {};
	const NodeHandle n = *made;
	ui.setUserData(n, static_cast<u64>(reinterpret_cast<std::uintptr_t>(handler)));
	if (!cssClass.empty()) ui.addClass(n, cssClass);
	ui.setCss(n, "padding", "0px");
	ui.setCss(n, "gap", "0px");
	return n;
}

void releasePanelRoot(UiContext& ui, NodeHandle root)
{
	if (!ui.alive(root)) return;
	ui.setUserData(root, 0);
	ui.destroy(root);
}

bool isTextEntry(const UiContext& ui, NodeHandle n)
{
	if (!ui.alive(n)) return false;
	const std::string& name = ui.registry().get(ui.classOf(n)).name;
	return name == "lineedit" || name == "spinbox" || name == "codeedit" || name == "datetimeedit";
}

void setShown(UiContext& ui, NodeHandle n, bool shown)
{
	if (ui.alive(n)) ui.setCss(n, "display", shown ? "flex" : "none");
}

bool isKey(const Event& e, Key key, u8 mods)
{
	constexpr u8 kMask = kModShift | kModCtrl | kModAlt;
	return e.type == EventType::KeyDown && e.key == key && (e.mods & kMask) == mods;
}

NodeHandle toolbarRow(UiContext& ui, NodeHandle parent)
{
	const NodeHandle row = w::panel(ui, parent);
	ui.addClass(row, "retdec-toolbar");
	ui.setCss(row, "flex-direction", "row");
	ui.setCss(row, "align-items", "center");
	ui.setCss(row, "gap", "4px");
	ui.setCss(row, "flex-shrink", "0");
	return row;
}

const fuse::text::Font* styledFont(const UiContext& ui, NodeHandle node, bool mono, bool bold)
{
	if (!mono && !bold) return nullptr;
	const ComputedStyle& st = ui.computed(node);
	const u16 weight = bold ? kFontWeightBold : (st.fontWeight ? st.fontWeight : kFontWeightNormal);
	const fuse::text::Font* f = mono ? ui.resolveFont(kMonoFamily, weight)
	                                 : ui.resolveFont(std::span<const u32>(st.fontFamily.data(), st.fontFamilyCount), weight);
	return f == ui.fontFor(node) ? nullptr : f;
}

void setLineEditText(UiContext& ui, NodeHandle edit, std::string_view text)
{
	if (!ui.alive(edit)) return;
	ui.editBuffer(edit).setText(std::string(text));
	ui.setText(edit, text);
}

} // namespace retdec::gui::ui
