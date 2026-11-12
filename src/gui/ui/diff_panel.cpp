/**
 * @file src/gui/ui/diff_panel.cpp
 * @brief Diff / Compare panel (FuseUI).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/diff_panel.h"

#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/theme.h"

#include "retdec/gui/model/funclist_export.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using fuse::ui::Color;
using fuse::ui::Event;
using fuse::ui::EventType;
using fuse::ui::Key;
using fuse::ui::NodeHandle;
using fuse::ui::Rect;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;
using fuse::ui::Vec2;
using model::DiffCellKind;
using model::DiffSpan;

namespace {

constexpr std::string_view kPaneClass = "rd-diffpane";
constexpr float kFontPx = 13.f;
// Mocha colours; themed() gives the Latte ones under the light theme.
constexpr uint32_t kPaneBg = 0x1e1e2e;
constexpr uint32_t kGutterBg = 0x181825;
constexpr uint32_t kText = 0xcdd6f4;
constexpr uint32_t kCurrentLine = 0x45475a;
constexpr uint32_t kGreen = 0xa6e3a1;
constexpr uint32_t kRed = 0xf38ba8;

/// @p rgb (a Mocha colour or one of the model's dark diff tints) in the theme of @p ui.
uint32_t themed(const UiContext& ui, uint32_t rgb)
{
	if (currentTheme(ui) != ThemeKind::Latte) return rgb;
	switch (rgb)
	{
	case model::kDiffRemovedBg: return 0xf9d8de;
	case model::kDiffAddedBg: return 0xdcf1d6;
	case model::kDiffRemovedSpanBg: return 0xf0a5b3;
	case model::kDiffAddedSpanBg: return 0xa9dc9c;
	case model::kDiffLineNumberFg: return 0x8c8fa1;
	default: return themedColor(ThemeKind::Latte, rgb);
	}
}

uint32_t darker(uint32_t rgb, float f)
{
	auto ch = [&](int s) { return static_cast<uint32_t>(static_cast<float>((rgb >> s) & 0xff) / f) & 0xff; };
	return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

int digits(int n)
{
	int d = 1;
	for (n = std::max(1, n); n >= 10; n /= 10)
		++d;
	return d;
}

// Tabs expand to 4 columns for display; spans (byte offsets) follow.
std::string expandTabs(std::string_view s, std::vector<DiffSpan>* spans)
{
	if (s.find('\t') == std::string_view::npos) return std::string(s);
	std::string out;
	std::vector<std::size_t> map(s.size() + 1);
	std::size_t col = 0;
	for (std::size_t i = 0; i < s.size(); ++i)
	{
		map[i] = out.size();
		if (s[i] == '\t')
		{
			const std::size_t n = 4 - col % 4;
			out.append(n, ' ');
			col += n;
		}
		else
		{
			out += s[i];
			if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++col;
		}
	}
	map[s.size()] = out.size();
	if (spans)
		for (auto& sp: *spans)
			sp = {map[std::min(sp.begin, s.size())], map[std::min(sp.end, s.size())]};
	return out;
}

std::string_view cellBgKind(DiffCellKind k, uint32_t& bg)
{
	switch (k)
	{
	case DiffCellKind::Removed: bg = model::kDiffRemovedBg; return "-";
	case DiffCellKind::Added: bg = model::kDiffAddedBg; return "+";
	case DiffCellKind::Placeholder: bg = model::kDiffPlaceholderBg; return "";
	default: bg = 0; return "";
	}
}

void setEditText(UiContext& ui, NodeHandle edit, std::string_view text)
{
	ui.editBuffer(edit).setText(text);
	ui.editBuffer(edit).end(false);
	ui.setText(edit, text);
}

} // namespace

// ─── Pane widget class ───────────────────────────────────────────────────────

fuse::ui::Size DiffPanel::measurePane(UiContext&, NodeHandle, fuse::ui::Size)
{
	return {120.f, 80.f};
}

bool DiffPanel::paneEvent(fuse::ui::EventContext& ec, const Event& e)
{
	if (ec.phase != fuse::ui::Phase::Target) return false;
	auto* ref = reinterpret_cast<PaneRef*>(static_cast<std::uintptr_t>(ec.ui.userData(ec.node)));
	if (!ref || !ref->panel) return false;
	return ref->panel->onPaneEvent(e);
}

bool DiffPanel::onPaneEvent(const Event& e)
{
	const int vis = visibleRows();
	const int rows = static_cast<int>(rowCount());
	auto scrollBy = [&](int d) { scrollToRowTop(first_ + d); };
	switch (e.type)
	{
	case EventType::Wheel:
		if (e.wheel.x != 0.f || (e.mods & fuse::ui::kModShift))
		{
			const float d = e.wheel.x != 0.f ? e.wheel.x : e.wheel.y;
			xScroll_ = std::max(0.f, xScroll_ - d * 40.f);
			invalidatePanes();
		}
		else
			scrollBy(-static_cast<int>(std::lround(e.wheel.y * 3.f)));
		return true;
	case EventType::PointerDown: {
		NodeHandle pane = mode_ == ViewMode::Unified ? uniPane_ : leftPane_;
		const Rect c = ui_.contentRect(pane);
		const int row = first_ + static_cast<int>((e.pos.y - c.y) / lineHeight());
		if (row >= 0 && row < rows)
		{
			cursorRow_ = row;
			invalidatePanes();
		}
		return true;
	}
	case EventType::KeyDown:
		switch (e.key)
		{
		case Key::Up: scrollBy(-1); return true;
		case Key::Down: scrollBy(1); return true;
		case Key::PageUp: scrollBy(-vis); return true;
		case Key::PageDown: scrollBy(vis); return true;
		case Key::Home: scrollToRowTop(0); return true;
		case Key::End: scrollToRowTop(rows); return true;
		case Key::Left:
			xScroll_ = std::max(0.f, xScroll_ - 40.f);
			invalidatePanes();
			return true;
		case Key::Right:
			xScroll_ += 40.f;
			invalidatePanes();
			return true;
		case Key::N: nextHunk(); return true;
		case Key::P: prevHunk(); return true;
		case Key::C:
			if (!(e.mods & fuse::ui::kModCtrl)) return false;
			copyDiff();
			return true;
		default: return false;
		}
	default: return false;
	}
}

void DiffPanel::paintPane(const fuse::ui::PaintContext& pc, fuse::ui::DrawList& dl)
{
	UiContext& ui = pc.ui;
	auto* ref = reinterpret_cast<PaneRef*>(static_cast<std::uintptr_t>(ui.userData(pc.node)));
	if (!ref || !ref->panel) return;
	const DiffPanel& p = *ref->panel;
	const Rect c = pc.content;
	const float lh = p.lineHeight();
	const float px = kFontPx;
	const float charW = std::max(6.f, ui.measureString("9", px, pc.node));
	const bool unified = ref->kind == PaneKind::Unified;
	const int numDigits = unified ? std::max(p.leftDigits_, p.rightDigits_)
								  : (ref->kind == PaneKind::Left ? p.leftDigits_ : p.rightDigits_);
	const float numW = charW * static_cast<float>(numDigits + 1) + 6.f;
	const float gutterW = unified ? numW * 2.f + charW * 2.f : numW;
	const Color textCol = pc.tint(Color::rgb(themed(ui, kText)));
	const Color numCol = pc.tint(Color::rgb(themed(ui, model::kDiffLineNumberFg)));

	dl.pushClip(c);
	dl.rect(c, pc.tint(Color::rgb(themed(ui, kPaneBg))));
	dl.rect({c.x, c.y, gutterW, c.h}, pc.tint(Color::rgb(themed(ui, kGutterBg))));
	const int rows = static_cast<int>(p.rowCount());
	const float textX = c.x + gutterW + 6.f - p.xScroll_;
	for (int r = p.first_; r < rows; ++r)
	{
		const float y = c.y + static_cast<float>(r - p.first_) * lh;
		if (y >= c.bottom()) break;
		std::string_view text;
		std::vector<DiffSpan> spans;
		int line = -1, line2 = -1;
		uint32_t bg = 0;
		char prefix = ' ';
		uint32_t spanColor = 0;
		if (unified)
		{
			const auto& u = p.uni_[static_cast<std::size_t>(r)];
			text = u.text;
			spans = u.spans;
			line = u.leftLine;
			line2 = u.rightLine;
			prefix = u.prefix();
			bg = u.kind == model::DiffOpKind::Delete ? model::kDiffRemovedBg
			   : u.kind == model::DiffOpKind::Insert ? model::kDiffAddedBg
													 : 0;
			spanColor = u.kind == model::DiffOpKind::Delete ? model::kDiffRemovedSpanBg : model::kDiffAddedSpanBg;
		}
		else
		{
			const auto& row = p.sbs_[static_cast<std::size_t>(r)];
			const model::DiffCell& cell = ref->kind == PaneKind::Left ? row.left : row.right;
			text = cell.text;
			spans = cell.spans;
			line = cell.line;
			cellBgKind(cell.kind, bg);
			spanColor = cell.kind == DiffCellKind::Removed ? model::kDiffRemovedSpanBg : model::kDiffAddedSpanBg;
		}
		if (bg)
		{
			dl.rect({c.x + gutterW, y, c.w - gutterW, lh}, pc.tint(Color::rgb(themed(ui, bg))));
			dl.rect({c.x, y, gutterW - 2.f, lh}, pc.tint(Color::rgb(darker(themed(ui, bg), 1.3f))));
		}
		if (r == p.cursorRow_)
			dl.rect(
				{c.x + gutterW, y, c.w - gutterW, lh}, pc.tint(Color::rgb(themed(ui, kCurrentLine), 160.f / 255.f)));
		if (line >= 0)
			ui.paintString(
				dl,
				std::to_string(line + 1),
				{c.x, y, numW - 4.f, lh},
				numCol,
				px - 1.f,
				fuse::ui::TextAlignment::End,
				pc.node.index);
		if (unified)
		{
			if (line2 >= 0)
				ui.paintString(
					dl,
					std::to_string(line2 + 1),
					{c.x + numW, y, numW - 4.f, lh},
					numCol,
					px - 1.f,
					fuse::ui::TextAlignment::End,
					pc.node.index);
			const char pre[2] = {prefix, 0};
			const Color preCol = pc.tint(Color::rgb(themed(ui, prefix == '+' ? kGreen : prefix == '-' ? kRed : kText)));
			ui.paintString(
				dl,
				pre,
				{c.x + numW * 2.f, y, charW * 2.f, lh},
				preCol,
				px,
				fuse::ui::TextAlignment::Center,
				pc.node.index);
		}
		if (text.empty()) continue;
		const std::string shown = expandTabs(text, &spans);
		dl.pushClip({c.x + gutterW, y, c.w - gutterW, lh});
		for (const auto& sp: spans)
		{
			const float x0 = ui.measureString(std::string_view(shown).substr(0, sp.begin), px, pc.node);
			const float x1 = ui.measureString(std::string_view(shown).substr(0, sp.end), px, pc.node);
			dl.rect(
				{textX + x0, y + 1.f, std::max(2.f, x1 - x0), lh - 2.f},
				pc.tint(Color::rgb(themed(ui, spanColor))),
				2.f);
		}
		ui.paintString(dl, shown, {textX, y, 100000.f, lh}, textCol, px, fuse::ui::TextAlignment::Start, pc.node.index);
		dl.popClip();
	}
	if (p.rowCount() > static_cast<std::size_t>(p.visibleRows()))
	{
		// Scroll position indicator.
		const float trackH = c.h;
		const float thumbH = std::max(16.f, trackH * static_cast<float>(p.visibleRows()) / static_cast<float>(rows));
		const float maxFirst = static_cast<float>(std::max(1, rows - p.visibleRows()));
		const float ty = c.y + (trackH - thumbH) * std::min(1.f, static_cast<float>(p.first_) / maxFirst);
		dl.rect({c.right() - 6.f, ty, 4.f, thumbH}, pc.tint(Color::rgb(0x8a8a96, 0.7f)), 2.f);
	}
	if (pc.states & fuse::ui::kStateFocus) dl.roundedRect(pc.rect, {}, {}, pc.tint(Color::rgb(0x3d8bfd, 0.6f)), 1.f);
	dl.popClip();
}

// ═════════════════════════════════════════════════════════════════════════════

DiffPanel::DiffPanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	leftRef_ = {this, PaneKind::Left};
	rightRef_ = {this, PaneKind::Right};
	uniRef_ = {this, PaneKind::Unified};
	build(parent);
	updateStats();
}

DiffPanel::~DiffPanel()
{
	for (NodeHandle pane: {leftPane_, rightPane_, uniPane_})
		if (ui_.alive(pane)) ui_.setUserData(pane, 0);
	if (ui_.alive(fileDlg_)) ui_.destroy(ui_.parent(fileDlg_));
	for (NodeHandle t: tooltips_)
		if (ui_.alive(t)) ui_.destroy(t);
	if (ui_.alive(root_)) ui_.destroy(root_);
}

void DiffPanel::tooltip(NodeHandle n, std::string_view text)
{
	tooltips_.push_back(w::setTooltip(ui_, n, text));
}

NodeHandle DiffPanel::makePane(NodeHandle parent, PaneRef* ref)
{
	if (ui_.registry().find(kPaneClass) == 0xFFFFFFFFu)
	{
		fuse::ui::WidgetClass cls;
		cls.name = std::string(kPaneClass);
		cls.role = fuse::ui::A11yRole::ScrollArea;
		cls.focusable = true;
		cls.interactive = true;
		cls.measure = &DiffPanel::measurePane;
		cls.paint = &DiffPanel::paintPane;
		cls.onEvent = &DiffPanel::paneEvent;
		// Qt: "Cascadia Code, JetBrains Mono, Consolas, monospace" — the panes are monospace.
		cls.defaults.fontFamilyCount = fuse::ui::u8(fuse::ui::parseFontFamilies(kMonoFamily, cls.defaults.fontFamily));
		ui_.registry().add(std::move(cls));
	}
	const NodeHandle pane = ui_.create(ui_.registry().find(kPaneClass), parent);
	ui_.setUserData(pane, static_cast<fuse::ui::u64>(reinterpret_cast<std::uintptr_t>(ref)));
	ui_.setCss(pane, "flex-grow", "1");
	ui_.setCss(pane, "overflow", "clip");
	return pane;
}

void DiffPanel::build(NodeHandle parent)
{
	if (ui_.registry().find("lineedit") == 0xFFFFFFFFu) w::registerTierA(ui_);
	w::registerTierB(ui_);

	root_ = w::panel(ui_, parent);
	ui_.addClass(root_, "rd-diff");
	ui_.setA11y(root_, fuse::ui::A11yRole::Group, kTitle);
	ui_.setCss(root_, "flex-grow", "1");
	ui_.setCss(root_, "padding", "0px");
	ui_.setCss(root_, "gap", "2px");

	// ── toolbar ─────────────────────────────────────────────────────────────
	const NodeHandle bar = w::panel(ui_, root_);
	ui_.setCss(bar, "flex-direction", "row");
	ui_.setCss(bar, "align-items", "center");
	ui_.setCss(bar, "gap", "6px");
	ui_.setCss(bar, "padding", "3px 4px");
	ui_.setCss(bar, "flex-wrap", "wrap");
	auto compact = [&](NodeHandle n) {
		ui_.setCss(n, "padding", "3px 8px");
		ui_.setCss(n, "font-size", "13px");
		ui_.setCss(n, "flex-shrink", "0");
	};
	compact(w::label(ui_, bar, "Stage:"));
	stageCombo_ = w::comboBox(ui_, bar, "<manual>");
	compact(stageCombo_);
	ui_.setCss(stageCombo_, "min-width", "160px");
	tooltip(stageCombo_, "Recovery stage whose before / after output is compared");
	modeCombo_ = w::comboBox(ui_, bar, "Side by side\nUnified");
	compact(modeCombo_);
	tooltip(modeCombo_, "Two panes, or one unified listing");
	prevBtn_ = w::button(ui_, bar, "\xE2\x96\xB2 Prev"); // ▲
	nextBtn_ = w::button(ui_, bar, "\xE2\x96\xBC Next"); // ▼
	exportUBtn_ = w::button(ui_, bar, "Unified");
	exportHBtn_ = w::button(ui_, bar, "HTML");
	copyBtn_ = w::button(ui_, bar, "Copy");
	for (NodeHandle b: {prevBtn_, nextBtn_, exportUBtn_, exportHBtn_, copyBtn_})
		compact(b);
	tooltip(prevBtn_, "Jump to previous diff hunk (P)");
	tooltip(nextBtn_, "Jump to next diff hunk (N)");
	tooltip(exportUBtn_, "Export as unified diff (.patch)");
	tooltip(exportHBtn_, "Export as HTML report");
	tooltip(copyBtn_, "Copy unified diff to clipboard");
	const NodeHandle spacer = w::panel(ui_, bar);
	ui_.setCss(spacer, "flex-grow", "1");
	ui_.setCss(spacer, "padding", "0px");
	stats_ = w::panel(ui_, bar);
	ui_.setCss(stats_, "flex-direction", "row");
	ui_.setCss(stats_, "gap", "10px");
	ui_.setCss(stats_, "padding", "0px 6px");
	statsAdded_ = w::label(ui_, stats_, "");
	ui_.addClass(statsAdded_, "retdec-success");
	statsRemoved_ = w::label(ui_, stats_, "");
	ui_.addClass(statsRemoved_, "retdec-danger");
	statsSimilar_ = w::label(ui_, stats_, "No diff");
	ui_.addClass(statsSimilar_, "retdec-muted");
	for (NodeHandle l: {statsAdded_, statsRemoved_, statsSimilar_})
		ui_.setCss(l, "font-size", "13px");

	ui_.connect(stageCombo_, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		selectStage(static_cast<int>(a.value));
	});
	ui_.connect(modeCombo_, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		setViewMode(a.value >= 0.5f ? ViewMode::Unified : ViewMode::SideBySide);
	});
	ui_.connect(prevBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { prevHunk(); });
	ui_.connect(nextBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { nextHunk(); });
	ui_.connect(exportUBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		openExportDialog(false);
	});
	ui_.connect(exportHBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		openExportDialog(true);
	});
	ui_.connect(copyBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { copyDiff(); });

	// ── side by side ────────────────────────────────────────────────────────
	auto header = [&](NodeHandle parent, std::string_view text) {
		const NodeHandle l = w::label(ui_, parent, text);
		ui_.addClass(l, "retdec-pane-title");
		ui_.setCss(l, "padding", "2px");
		ui_.setCss(l, "text-align", "center");
		ui_.setCss(l, "font-size", "13px");
		return l;
	};
	sbsView_ = w::splitter(ui_, root_, false);
	ui_.setCss(sbsView_, "flex-grow", "1");
	for (int side = 0; side < 2; ++side)
	{
		const NodeHandle pane = w::splitterPane(ui_, sbsView_);
		ui_.setCss(pane, "padding", "0px");
		ui_.setCss(pane, "gap", "0px");
		if (side == 0)
		{
			leftLabel_ = header(pane, "Before");
			leftPane_ = makePane(pane, &leftRef_);
			ui_.setA11y(leftPane_, fuse::ui::A11yRole::ScrollArea, "Before");
		}
		else
		{
			rightLabel_ = header(pane, "After");
			rightPane_ = makePane(pane, &rightRef_);
			ui_.setA11y(rightPane_, fuse::ui::A11yRole::ScrollArea, "After");
		}
	}

	// ── unified ─────────────────────────────────────────────────────────────
	uniView_ = w::panel(ui_, root_);
	ui_.setCss(uniView_, "flex-grow", "1");
	ui_.setCss(uniView_, "padding", "0px");
	ui_.setCss(uniView_, "gap", "0px");
	ui_.setCss(uniView_, "display", "none");
	header(uniView_, "Unified");
	uniPane_ = makePane(uniView_, &uniRef_);
	ui_.setA11y(uniPane_, fuse::ui::A11yRole::ScrollArea, "Unified diff");
}

// ─── Data ────────────────────────────────────────────────────────────────────

void DiffPanel::setDiff(std::string_view before, std::string_view after, std::string_view stageName)
{
	auto diff = model::MyersDiff::diffText(std::string(before), std::string(after));
	ui_.setText(leftLabel_, stageName.empty() ? std::string("Before") : "Before: " + std::string(stageName));
	ui_.setText(rightLabel_, stageName.empty() ? std::string("After") : "After: " + std::string(stageName));
	apply(std::move(diff));
}

void DiffPanel::addStage(std::string_view name, std::string_view before, std::string_view after)
{
	stages_.push_back({std::string(name), std::string(before), std::string(after)});
	updateStageCombo();
	ui_.setValue(stageCombo_, static_cast<float>(stages_.size()), false);
	setDiff(before, after, name);
}

void DiffPanel::selectStage(int index)
{
	if (index <= 0 || index > static_cast<int>(stages_.size())) return;
	ui_.setValue(stageCombo_, static_cast<float>(index), false);
	const auto& s = stages_[static_cast<std::size_t>(index - 1)];
	setDiff(s.before, s.after, s.name);
}

void DiffPanel::updateStageCombo()
{
	std::string items = "<manual>";
	for (const auto& s: stages_)
		items += "\n" + s.name;
	ui_.setText(stageCombo_, items);
}

void DiffPanel::clear()
{
	diff_ = {};
	sbs_.clear();
	uni_.clear();
	hunks_.reset({});
	first_ = 0;
	cursorRow_ = -1;
	xScroll_ = 0.f;
	stages_.clear();
	updateStageCombo();
	ui_.setValue(stageCombo_, 0.f, false);
	ui_.setText(leftLabel_, "Before");
	ui_.setText(rightLabel_, "After");
	updateStats();
	invalidatePanes();
}

void DiffPanel::apply(model::DiffResult diff)
{
	diff_ = std::move(diff);
	sbs_ = model::buildSideBySide(diff_);
	uni_ = model::buildUnified(diff_);
	int maxL = 0, maxR = 0;
	for (const auto& op: diff_.ops)
	{
		maxL = std::max(maxL, op.leftLine + 1);
		maxR = std::max(maxR, op.rightLine + 1);
	}
	leftDigits_ = digits(maxL);
	rightDigits_ = digits(maxR);
	first_ = 0;
	cursorRow_ = -1;
	xScroll_ = 0.f;
	resetHunks();
	updateStats();
	invalidatePanes();
	if (onDiffChanged) onDiffChanged(diff_);
}

void DiffPanel::resetHunks()
{
	hunks_.reset(mode_ == ViewMode::Unified ? model::diffHunkStarts(uni_) : model::diffHunkStarts(sbs_));
}

void DiffPanel::updateStats()
{
	if (diff_.isEmpty())
	{
		ui_.setText(statsAdded_, "");
		ui_.setText(statsRemoved_, "");
		ui_.setText(statsSimilar_, "No diff");
		return;
	}
	ui_.setText(statsAdded_, std::format("+{}", diff_.linesAdded));
	ui_.setText(statsRemoved_, std::format("-{}", diff_.linesRemoved));
	ui_.setText(statsSimilar_, std::format("{}% similar", static_cast<int>(diff_.similarity * 100.0)));
	if (!diff_.exact)
		ui_.setText(
			statsSimilar_,
			std::format("{}% similar (too large to diff exactly)", static_cast<int>(diff_.similarity * 100.0)));
}

// ─── View ────────────────────────────────────────────────────────────────────

void DiffPanel::setViewMode(ViewMode m)
{
	mode_ = m;
	ui_.setValue(modeCombo_, m == ViewMode::Unified ? 1.f : 0.f, false);
	ui_.setCss(sbsView_, "display", m == ViewMode::SideBySide ? "flex" : "none");
	ui_.setCss(uniView_, "display", m == ViewMode::Unified ? "flex" : "none");
	first_ = 0;
	cursorRow_ = -1;
	resetHunks();
	invalidatePanes();
}

std::size_t DiffPanel::rowCount() const
{
	return mode_ == ViewMode::Unified ? uni_.size() : sbs_.size();
}

float DiffPanel::lineHeight() const
{
	return (ui_.alive(leftPane_) ? ui_.lineHeight(kFontPx, leftPane_) : ui_.lineHeight(kFontPx)) + 2.f;
}

int DiffPanel::visibleRows() const
{
	const NodeHandle pane = mode_ == ViewMode::Unified ? uniPane_ : leftPane_;
	return std::max(1, static_cast<int>(ui_.contentRect(pane).h / lineHeight()));
}

void DiffPanel::scrollToRowTop(int row)
{
	const int maxFirst = std::max(0, static_cast<int>(rowCount()) - visibleRows());
	const int f = std::clamp(row, 0, maxFirst);
	if (f == first_) return;
	first_ = f;
	invalidatePanes();
}

void DiffPanel::scrollToRow(int row)
{
	// Keep a little context above the target.
	scrollToRowTop(row - 2);
}

void DiffPanel::invalidatePanes()
{
	for (NodeHandle p: {leftPane_, rightPane_, uniPane_})
		if (ui_.alive(p)) ui_.invalidate(p);
}

int DiffPanel::nextHunk()
{
	const auto r = hunks_.next();
	if (!r) return -1;
	cursorRow_ = *r;
	scrollToRow(*r);
	invalidatePanes();
	return *r;
}

int DiffPanel::prevHunk()
{
	const auto r = hunks_.prev();
	if (!r) return -1;
	cursorRow_ = *r;
	scrollToRow(*r);
	invalidatePanes();
	return *r;
}

// ─── Export ──────────────────────────────────────────────────────────────────

std::expected<void, std::string> DiffPanel::exportUnified(const std::string& path) const
{
	return model::funclistWriteTextFile(path, unifiedText());
}

std::expected<void, std::string> DiffPanel::exportHtml(const std::string& path) const
{
	return model::funclistWriteTextFile(path, htmlText());
}

bool DiffPanel::copy(const std::string& text)
{
	if (clipboardWriter) return clipboardWriter(text);
	return ui_.setClipboardText(text);
}

void DiffPanel::copyDiff()
{
	copy(unifiedText());
}

void DiffPanel::openExportDialog(bool html)
{
	if (ui_.alive(fileDlg_)) ui_.destroy(ui_.parent(fileDlg_));
	fileDlgHtml_ = html;
	std::error_code ec;
	fileDlg_ = w::fileDialog(
		ui_,
		html ? "Export Diff HTML" : "Export Diff",
		std::filesystem::current_path(ec).generic_string(),
		html ? "*.html" : "*.patch;*.diff",
		true);
	const NodeHandle body = w::dialogBody(ui_, fileDlg_);
	NodeHandle row = ui_.firstChild(body);
	for (int i = 0; i < 2 && ui_.alive(row); ++i)
		row = ui_.nextSibling(row);
	if (ui_.alive(row))
	{
		const NodeHandle nameEdit = ui_.nextSibling(ui_.firstChild(row));
		if (ui_.alive(nameEdit)) setEditText(ui_, nameEdit, html ? "diff.html" : "diff.patch");
	}
	ui_.connect(fileDlg_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (a.text.empty()) return;
		const std::string path(a.text);
		const auto r = fileDlgHtml_ ? exportHtml(path) : exportUnified(path);
		if (!r) ui_.setText(statsSimilar_, "Export failed: " + r.error());
	});
	w::openDialog(ui_, fileDlg_);
}

} // namespace retdec::gui::ui
