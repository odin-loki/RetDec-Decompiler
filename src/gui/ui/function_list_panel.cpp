/**
 * @file src/gui/ui/function_list_panel.cpp
 * @brief Functions panel (FuseUI).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/function_list_panel.h"
#include "retdec/gui/ui/theme.h"

#include "retdec/gui/model/funclist_export.h"
#include "retdec/gui/ui/empty_state.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>

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

namespace {

// ─── Capture-phase event hook on the node that hosts the table ───────────────
// The table widget handles selection (Extended mode: Ctrl / Shift clicks and
// keys, Ctrl+A) and activation (double click / Enter) itself; the host sees
// every event first and adds the context menu, the cell tooltips and the
// F2 / Ctrl+C / Shift+F10 shortcuts.
using Hook = std::function<bool(UiContext&, const Event&)>;
struct HookKey
{
	const UiContext* ui;
	uint32_t index, gen;
	auto operator<=>(const HookKey&) const = default;
};
std::map<HookKey, std::weak_ptr<Hook>>& hookMap()
{
	static std::map<HookKey, std::weak_ptr<Hook>> m;
	return m;
}

struct FunclistHost
{
	static constexpr std::string_view kName = "rd-funclist-host";
	static bool onEvent(fuse::ui::EventContext& ec, const Event& e)
	{
		if (ec.phase != fuse::ui::Phase::Capture) return false;
		auto it = hookMap().find({&ec.ui, ec.node.index, ec.node.generation});
		if (it == hookMap().end()) return false;
		if (auto h = it->second.lock()) return (*h)(ec.ui, e);
		return false;
	}
};

// Text on the (fixed-colour) pattern badges.
constexpr uint32_t kBadgeText = 0x1e1e2e;

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

float rowHeight(UiContext& ui, NodeHandle n)
{
	const float px = ui.computed(n).fontSize;
	return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}

void setEditText(UiContext& ui, NodeHandle edit, std::string_view text)
{
	ui.editBuffer(edit).setText(text);
	ui.editBuffer(edit).end(false);
	ui.setText(edit, text);
}

uint32_t tierColor(const ThemeColors& p, model::ConfidenceTier t)
{
	switch (t)
	{
	case model::ConfidenceTier::High: return p.green;
	case model::ConfidenceTier::Medium: return p.yellow;
	default: return p.red;
	}
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════

FunctionListPanel::FunctionListPanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	proxy_.sortBy(model::FnColAddress, true);
	list_.onRenamed = [this](uint64_t a, const std::string& o, const std::string& n) {
		if (onFunctionRenamed) onFunctionRenamed(a, o, n);
	};
	build(parent);
	refresh(false);
}

FunctionListPanel::~FunctionListPanel()
{
	hookMap().erase({&ui_, host_.index, host_.generation});
	for (NodeHandle dlg: {promptDlg_, fileDlg_})
		if (ui_.alive(dlg)) ui_.destroy(ui_.parent(dlg));
	if (ui_.alive(menu_)) ui_.destroy(menu_);
	if (cellTipPending_) ui_.cancelTimer(cellTipTimer_);
	if (ui_.alive(cellTip_)) ui_.destroy(cellTip_);
	for (NodeHandle t: tooltips_)
		if (ui_.alive(t)) ui_.destroy(t);
	if (ui_.alive(root_)) ui_.destroy(root_);
}

void FunctionListPanel::tooltip(NodeHandle n, std::string_view text)
{
	tooltips_.push_back(w::setTooltip(ui_, n, text));
}

void FunctionListPanel::build(NodeHandle parent)
{
	if (ui_.registry().find("lineedit") == 0xFFFFFFFFu) w::registerTierA(ui_);
	w::registerTierB(ui_);
	if (ui_.registry().find(FunclistHost::kName) == 0xFFFFFFFFu) ui_.registry().registerWidget<FunclistHost>();

	root_ = w::panel(ui_, parent);
	ui_.addClass(root_, "rd-funclist");
	ui_.setA11y(root_, fuse::ui::A11yRole::Group, kTitle);
	ui_.setCss(root_, "flex-grow", "1");
	ui_.setCss(root_, "padding", "0px");
	ui_.setCss(root_, "gap", "0px");

	const NodeHandle bar = w::panel(ui_, root_);
	ui_.addClass(bar, "rd-filter-bar");
	ui_.setA11y(bar, fuse::ui::A11yRole::Group, "Function list filters");
	ui_.setCss(bar, "flex-direction", "row");
	ui_.setCss(bar, "align-items", "center");
	ui_.setCss(bar, "gap", "4px");
	ui_.setCss(bar, "padding", "3px 4px");
	ui_.setCss(bar, "flex-wrap", "wrap");
	buildFilterBar(bar);
	for (NodeHandle c = ui_.firstChild(bar); ui_.alive(c); c = ui_.nextSibling(c))
		ui_.setCss(c, "flex-shrink", "0");
	// In a narrow dock the text fields give way first.
	for (NodeHandle e: {nameEdit_, addrLowEdit_, addrHighEdit_})
	{
		ui_.setCss(e, "flex-shrink", "1");
		ui_.setCss(e, "min-width", "48px");
	}

	// Table area: empty-state message or the table.
	const NodeHandle area = w::panel(ui_, root_);
	ui_.setCss(area, "flex-grow", "1");
	ui_.setCss(area, "padding", "0px");

	emptyState_ = w::panel(ui_, area);
	ui_.setCss(emptyState_, "flex-grow", "1");
	ui_.setCss(emptyState_, "align-items", "center");
	ui_.setCss(emptyState_, "justify-content", "center");
	ui_.setCss(emptyState_, "gap", "6px");
	const NodeHandle et = w::label(ui_, emptyState_, "No functions yet");
	ui_.setCss(et, "font-size", "16px");
	const NodeHandle eh =
		w::label(ui_, emptyState_, "Open a binary and run analysis (F5) to populate the function list.");
	ui_.addClass(eh, "dim");

	host_ = ui_.create(ui_.registry().find(FunclistHost::kName), area);
	ui_.setCss(host_, "flex-grow", "1");
	hook_ = std::make_shared<Hook>([this](UiContext&, const Event& e) { return onTableEvent(e); });
	hookMap()[{&ui_, host_.index, host_.generation}] = hook_;

	itemModel_.user = this;
	itemModel_.columns = model::FnColCount;
	itemModel_.rows = [](const void* u) {
		return static_cast<fuse::ui::u32>(static_cast<const FunctionListPanel*>(u)->proxy_.rowCount());
	};
	itemModel_.data = [](const void* u, fuse::ui::u32 r, fuse::ui::u32 c, std::string& scratch) -> std::string_view {
		const auto* p = static_cast<const FunctionListPanel*>(u);
		if (r >= p->proxy_.rowCount()) return {};
		scratch = model::functionCellText(p->proxy_.entry(r), static_cast<int>(c), p->demangle_);
		return scratch;
	};
	itemModel_.header = [](const void*, fuse::ui::u32 c) { return model::functionColumnHeader(static_cast<int>(c)); };

	table_ = w::tableView(ui_, host_, &itemModel_);
	ui_.setA11y(table_, fuse::ui::A11yRole::List, "Function list table");
	ui_.setCss(table_, "flex-grow", "1");
	ui_.addClass(table_, "retdec-table");
	ui_.setCss(table_, "font-size", "13px");
	constexpr float kWidths[model::FnColCount] = {150.f, 240.f, 70.f, 64.f, 92.f, 92.f, 76.f, 220.f};
	for (int c = 0; c < model::FnColCount; ++c)
	{
		w::setTableColumnWidth(ui_, table_, static_cast<fuse::ui::u32>(c), kWidths[c]);
		painters_[static_cast<std::size_t>(c)] = {this, c};
		delegates_[static_cast<std::size_t>(c)] = {
			&painters_[static_cast<std::size_t>(c)], &FunctionListPanel::paintCell};
		w::setTableDelegate(ui_, table_, static_cast<fuse::ui::u32>(c), &delegates_[static_cast<std::size_t>(c)]);
	}
	// Qt ExtendedSelection; double click / Enter activate.
	w::setTableSelectionMode(ui_, table_, w::SelectionMode::Extended);
	w::setTableActivation(ui_, table_, true);

	ui_.connect(table_, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		sortBy(static_cast<int>(a.value), a.flag);
	});
	// The current row (value) moves first, then the selection changes.
	ui_.connect(table_, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (applyingSelection_) return;
		const int row = static_cast<int>(a.value);
		if (row >= 0 && static_cast<std::size_t>(row) < proxy_.rowCount())
			current_ = proxy_.entry(static_cast<std::size_t>(row)).address;
	});
	ui_.connect(table_, SignalKind::SelectionChanged, [this](UiContext&, NodeHandle, const SignalArgs&) {
		if (applyingSelection_) return;
		syncSelectionFromTable();
		selectionChangedInternal();
	});
	ui_.connect(table_, SignalKind::Activated, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		activateRow(static_cast<int>(a.value), a.index); // index: column (-1 = Enter)
	});
	// Context menu picks arrive on the host (the menu's owner).
	ui_.connect(host_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		const auto i = static_cast<std::size_t>(a.value);
		if (i < menuActions_.size()) triggerAction(menuActions_[i], menuAddress_);
	});

	status_ = w::label(ui_, root_, "Ready");
	ui_.addClass(status_, "dim");
	ui_.setCss(status_, "padding", "2px 6px");
	ui_.setCss(status_, "font-size", "12px");
}

void FunctionListPanel::buildFilterBar(NodeHandle bar)
{
	auto compact = [&](NodeHandle n) {
		ui_.setCss(n, "padding", "3px 6px");
		ui_.setCss(n, "font-size", "13px");
	};
	nameEdit_ = w::lineEdit(ui_, bar);
	ui_.setCss(nameEdit_, "width", "140px");
	compact(nameEdit_);
	ui_.setA11y(nameEdit_, fuse::ui::A11yRole::TextField, "Search");
	w::setPlaceholderText(ui_, nameEdit_, std::string("Search") + std::string(kEllipsis));
	tooltip(nameEdit_, "Filter by name (wildcards * ? [])");

	const NodeHandle addrLabel = w::label(ui_, bar, "Addr:");
	ui_.addClass(addrLabel, "dim");
	addrLowEdit_ = w::lineEdit(ui_, bar);
	ui_.setCss(addrLowEdit_, "width", "80px");
	compact(addrLowEdit_);
	w::setPlaceholderText(ui_, addrLowEdit_, "0x0");
	tooltip(addrLowEdit_, "Lowest address (hex)");
	const NodeHandle dash = w::label(ui_, bar, "\xE2\x80\x93");
	ui_.addClass(dash, "dim");
	addrHighEdit_ = w::lineEdit(ui_, bar);
	ui_.setCss(addrHighEdit_, "width", "80px");
	compact(addrHighEdit_);
	w::setPlaceholderText(ui_, addrHighEdit_, std::string("0xFFFF") + std::string(kEllipsis));
	tooltip(addrHighEdit_, "Highest address (hex)");

	const NodeHandle confLabel = w::label(ui_, bar, "Conf\xE2\x89\xA5");
	ui_.addClass(confLabel, "dim");
	confSpin_ = w::spinBox(ui_, bar, 0.f, 1.f, 0.f, 0.05f);
	ui_.setCss(confSpin_, "width", "64px");
	compact(confSpin_);

	static constexpr std::string_view kPatternNames[5] = {"STL", "Crypto", "Algo", "Pat", "Lib"};
	for (std::size_t i = 0; i < 5; ++i)
	{
		patternChecks_[i] = w::checkBox(ui_, bar, kPatternNames[i]);
		ui_.setCss(patternChecks_[i], "font-size", "13px");
		tooltip(patternChecks_[i], model::patternBadges()[i].tip);
	}

	const NodeHandle spacer = w::panel(ui_, bar);
	ui_.setCss(spacer, "flex-grow", "1");

	csvBtn_ = w::button(ui_, bar, "CSV");
	jsonBtn_ = w::button(ui_, bar, "JSON");
	copyBtn_ = w::button(ui_, bar, "Copy");
	for (NodeHandle b: {csvBtn_, jsonBtn_, copyBtn_})
		compact(b);
	tooltip(csvBtn_, "Export the visible functions as CSV");
	tooltip(jsonBtn_, "Export the visible functions as JSON");
	tooltip(copyBtn_, "Copy the visible functions as CSV");

	auto onFilter = [this](UiContext&, NodeHandle, const SignalArgs&) { onFilterChanged(); };
	for (NodeHandle e: {nameEdit_, addrLowEdit_, addrHighEdit_})
		ui_.connect(e, SignalKind::TextChanged, onFilter);
	ui_.connect(confSpin_, SignalKind::ValueChanged, onFilter);
	for (NodeHandle c: patternChecks_)
		ui_.connect(c, SignalKind::Toggled, onFilter);

	ui_.connect(csvBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		triggerAction(Action::ExportCsv);
	});
	ui_.connect(jsonBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		triggerAction(Action::ExportJson);
	});
	ui_.connect(copyBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		triggerAction(Action::CopyCsv);
	});
}

// ─── Data ────────────────────────────────────────────────────────────────────

void FunctionListPanel::setFunctions(std::vector<model::FunctionEntry> fns)
{
	list_.setFunctions(std::move(fns));
	refresh(false);
}

void FunctionListPanel::clear()
{
	list_.clearAll();
	for (NodeHandle e: {nameEdit_, addrLowEdit_, addrHighEdit_})
		setEditText(ui_, e, "");
	ui_.setValue(confSpin_, 0.f);
	for (NodeHandle c: patternChecks_)
		ui_.setChecked(c, false);
	onFilterChanged();
	refresh(false);
}

void FunctionListPanel::filterByClass(std::string_view className)
{
	proxy_.setClassFilter(className);
	refresh(true);
}

void FunctionListPanel::setDemangleNames(bool on)
{
	demangle_ = on;
	proxy_.setDemangleNames(on);
	refresh(true);
}

std::string FunctionListPanel::nameFilterText() const
{
	return std::string(ui_.text(nameEdit_));
}

void FunctionListPanel::setNameFilterText(std::string_view text)
{
	if (ui_.text(nameEdit_) == text) return;
	setEditText(ui_, nameEdit_, text);
	onFilterChanged();
}

void FunctionListPanel::setAddressRangeText(std::string_view low, std::string_view high)
{
	setEditText(ui_, addrLowEdit_, low);
	setEditText(ui_, addrHighEdit_, high);
	onFilterChanged();
}

void FunctionListPanel::setMinConfidence(float minConfidence)
{
	ui_.setValue(confSpin_, minConfidence);
	onFilterChanged();
}

void FunctionListPanel::setPatternFilter(bool stl, bool crypto, bool algo, bool design, bool lib)
{
	const bool v[5] = {stl, crypto, algo, design, lib};
	for (std::size_t i = 0; i < 5; ++i)
		ui_.setChecked(patternChecks_[i], v[i]);
	onFilterChanged();
}

void FunctionListPanel::onFilterChanged()
{
	proxy_.setNameGlob(ui_.text(nameEdit_));
	const auto lo = model::funclistParseHex(ui_.text(addrLowEdit_));
	const auto hi = model::funclistParseHex(ui_.text(addrHighEdit_));
	proxy_.setAddressRange(lo.value_or(0), hi.value_or(0));
	proxy_.setMinConfidence(ui_.value(confSpin_));
	proxy_.setPatternFilter(
		ui_.checked(patternChecks_[0]),
		ui_.checked(patternChecks_[1]),
		ui_.checked(patternChecks_[2]),
		ui_.checked(patternChecks_[3]),
		ui_.checked(patternChecks_[4]));
	refresh(true);
}

void FunctionListPanel::sortBy(int column, bool ascending)
{
	proxy_.sortBy(column, ascending);
	refresh(true);
}

void FunctionListPanel::refresh(bool keepSelection)
{
	proxy_.invalidate();
	const std::size_t before = selected_.size();
	if (!keepSelection)
	{
		selected_.clear();
		current_.reset();
		anchorRow_ = -1;
	}
	else
	{
		// Rows that are no longer shown leave the selection (as in Qt).
		std::set<uint64_t> still;
		for (std::size_t r = 0; r < proxy_.rowCount(); ++r)
			if (selected_.count(proxy_.entry(r).address)) still.insert(proxy_.entry(r).address);
		selected_ = std::move(still);
		if (current_ && proxy_.rowForAddress(*current_) < 0) current_.reset();
		anchorRow_ = current_ ? proxy_.rowForAddress(*current_) : -1;
	}
	w::tableModelChanged(ui_, table_);
	applySelectionToTable(); // rows moved: re-mark the selected addresses
	ui_.invalidate(table_);
	updateStatus();
	if (before != selected_.size() && onSelectionChanged) onSelectionChanged();
}

void FunctionListPanel::updateStatus()
{
	const bool empty = list_.empty();
	ui_.setCss(emptyState_, "display", empty ? "flex" : "none");
	ui_.setCss(host_, "display", empty ? "none" : "flex");
	ui_.setText(status_, statusText());
}

std::string FunctionListPanel::statusText() const
{
	const std::size_t total = list_.size(), visible = proxy_.rowCount();
	if (total == 0) return "No functions \xE2\x80\x94 run Analysis (F5)";
	return visible == total ? std::format("{} functions", total) : std::format("{} / {} functions", visible, total);
}

// ─── Selection ───────────────────────────────────────────────────────────────

void FunctionListPanel::selectRow(int row, uint8_t mods)
{
	if (row < 0 || static_cast<std::size_t>(row) >= proxy_.rowCount()) return;
	const uint64_t addr = proxy_.entry(static_cast<std::size_t>(row)).address;
	if (mods & fuse::ui::kModCtrl)
	{
		if (!selected_.erase(addr)) selected_.insert(addr);
		anchorRow_ = row;
	}
	else if ((mods & fuse::ui::kModShift) && anchorRow_ >= 0)
	{
		selected_.clear();
		const int a = std::min(anchorRow_, row), b = std::max(anchorRow_, row);
		for (int r = a; r <= b; ++r)
			selected_.insert(proxy_.entry(static_cast<std::size_t>(r)).address);
	}
	else
	{
		selected_ = {addr};
		anchorRow_ = row;
	}
	current_ = addr;
	applySelectionToTable();
	selectionChangedInternal();
}

void FunctionListPanel::setCurrentRow(int row, bool scroll)
{
	applyingSelection_ = true;
	ui_.setValue(table_, static_cast<float>(row), false);
	if (scroll && row >= 0) w::scrollToRow(ui_, table_, static_cast<fuse::ui::u32>(row));
	applyingSelection_ = false;
}

void FunctionListPanel::applySelectionToTable()
{
	applyingSelection_ = true;
	w::tableClearSelection(ui_, table_);
	for (std::size_t r = 0; r < proxy_.rowCount(); ++r)
		if (selected_.count(proxy_.entry(r).address))
			w::setTableRowSelected(ui_, table_, static_cast<fuse::ui::u32>(r), true);
	ui_.setValue(table_, current_ ? static_cast<float>(proxy_.rowForAddress(*current_)) : -1.f, false);
	applyingSelection_ = false;
}

void FunctionListPanel::syncSelectionFromTable()
{
	selected_.clear();
	for (const fuse::ui::u32 r: w::tableSelectedRows(ui_, table_))
		if (r < proxy_.rowCount()) selected_.insert(proxy_.entry(r).address);
	const int cur = static_cast<int>(ui_.value(table_));
	if (cur >= 0 && static_cast<std::size_t>(cur) < proxy_.rowCount())
	{
		current_ = proxy_.entry(static_cast<std::size_t>(cur)).address;
		anchorRow_ = cur;
	}
}

void FunctionListPanel::selectionChangedInternal()
{
	ui_.invalidate(table_);
	if (onSelectionChanged) onSelectionChanged();
}

void FunctionListPanel::selectFunction(uint64_t address)
{
	const int row = proxy_.rowForAddress(address);
	if (row < 0) return;
	selected_ = {address};
	current_ = address;
	anchorRow_ = row;
	applySelectionToTable();
	setCurrentRow(row, true);
	selectionChangedInternal();
	const auto& e = proxy_.entry(static_cast<std::size_t>(row));
	if (onFunctionActivated) onFunctionActivated(e.address, model::functionActivationName(e));
}

std::optional<model::FunctionEntry> FunctionListPanel::selectedEntry() const
{
	if (current_ && selected_.count(*current_))
		if (const auto* e = list_.find(*current_)) return *e;
	for (std::size_t r = 0; r < proxy_.rowCount(); ++r)
		if (selected_.count(proxy_.entry(r).address)) return proxy_.entry(r);
	return std::nullopt;
}

std::vector<uint64_t> FunctionListPanel::selectedAddresses() const
{
	std::vector<uint64_t> out;
	for (std::size_t r = 0; r < proxy_.rowCount(); ++r)
		if (selected_.count(proxy_.entry(r).address)) out.push_back(proxy_.entry(r).address);
	return out;
}

// ─── Table events ────────────────────────────────────────────────────────────

int FunctionListPanel::rowAt(Vec2 p) const
{
	const Rect c = ui_.contentRect(table_);
	const float rh = rowHeight(ui_, table_);
	if (p.y < c.y + rh || p.y >= c.bottom()) return -1;
	const std::size_t r = w::tableFirstVisible(ui_, table_) + static_cast<std::size_t>((p.y - c.y - rh) / rh);
	return r < proxy_.rowCount() ? static_cast<int>(r) : -1;
}

int FunctionListPanel::columnAt(float x) const
{
	const Rect c = ui_.contentRect(table_);
	float at = c.x;
	for (int k = 0; k < model::FnColCount; ++k)
	{
		at += w::tableColumnWidth(ui_, table_, static_cast<fuse::ui::u32>(k));
		if (x < at) return k;
	}
	return -1;
}

bool FunctionListPanel::onTableEvent(const Event& e)
{
	using fuse::ui::kModCtrl;
	using fuse::ui::kModShift;
	if (e.type == EventType::KeyDown)
	{
		hideCellTip();
		const bool ctrl = e.mods & kModCtrl, shift = e.mods & kModShift;
		if (ctrl && e.key == Key::C)
		{
			triggerAction(Action::CopyAddress);
			return true;
		}
		if (e.key == Key::F2)
		{
			triggerAction(Action::Rename);
			return true;
		}
		if (shift && e.key == Key::F10)
		{
			const int row = current_ ? proxy_.rowForAddress(*current_) : -1;
			const Rect c = ui_.contentRect(table_);
			Vec2 at{c.x + 24.f, c.y + 24.f};
			if (row >= 0) at = cellCenter(static_cast<std::size_t>(row), model::FnColName);
			openContextMenu(at, row);
			return true;
		}
		return false;
	}
	if (e.type == EventType::PointerMove)
	{
		if (!ui_.pressed().valid()) hoverCell(e.pos);
		return false;
	}
	if (e.type == EventType::PointerLeave || e.type == EventType::Wheel)
	{
		hideCellTip();
		tipRow_ = tipCol_ = -1;
		return false;
	}
	if (e.type != EventType::PointerDown) return false;
	hideCellTip();
	// Left clicks: the table selects (Extended) and activates on a double click.
	if (e.button != fuse::ui::MouseButton::Right) return false;

	const Rect c = ui_.contentRect(table_);
	if (!c.contains(e.pos)) return false;
	const float rh = rowHeight(ui_, table_);
	if (e.pos.y < c.y + rh) return false; // header: sorting / column resize
	const auto visibleRows = static_cast<std::size_t>(std::max(1.f, (c.h - rh) / rh));
	if (proxy_.rowCount() > visibleRows && e.pos.x >= c.right() - 8.f) return false; // scroll bar

	const int row = rowAt(e.pos);
	if (row >= 0 && !selected_.count(proxy_.entry(static_cast<std::size_t>(row)).address)) selectRow(row, 0);
	openContextMenu(e.pos, row);
	return true;
}

void FunctionListPanel::hoverCell(Vec2 p)
{
	const int row = rowAt(p);
	const int col = row >= 0 ? columnAt(p.x) : -1;
	if (row == tipRow_ && col == tipCol_) return;
	tipRow_ = row;
	tipCol_ = col;
	hideCellTip();
	if (row < 0 || col < 0) return;
	if (model::functionToolTip(proxy_.entry(static_cast<std::size_t>(row)), col).empty()) return;
	tipPos_ = p;
	cellTipPending_ = true;
	cellTipTimer_ = ui_.startTimer(0.5f, false, [this](UiContext&) {
		cellTipPending_ = false;
		if (tipRow_ < 0 || static_cast<std::size_t>(tipRow_) >= proxy_.rowCount()) return;
		const std::string text = model::functionToolTip(proxy_.entry(static_cast<std::size_t>(tipRow_)), tipCol_);
		if (!ui_.alive(cellTip_))
		{
			cellTip_ = ui_.create("tooltip", w::overlayLayer(ui_)).value_or(NodeHandle{});
			if (!ui_.alive(cellTip_)) return;
			ui_.addClass(cellTip_, "tooltip");
		}
		ui_.setText(cellTip_, text);
		ui_.setCss(cellTip_, "left", std::format("{}px", tipPos_.x + 12.f));
		ui_.setCss(cellTip_, "top", std::format("{}px", tipPos_.y + 18.f));
		w::overlayLayer(ui_);
		ui_.setCss(cellTip_, "display", "flex");
	});
}

void FunctionListPanel::hideCellTip()
{
	if (cellTipPending_)
	{
		ui_.cancelTimer(cellTipTimer_);
		cellTipPending_ = false;
	}
	if (ui_.alive(cellTip_)) ui_.setCss(cellTip_, "display", "none");
}

void FunctionListPanel::activateRow(int row, int col)
{
	if (row < 0 || static_cast<std::size_t>(row) >= proxy_.rowCount()) return;
	const auto& e = proxy_.entry(static_cast<std::size_t>(row));
	if (col == model::FnColName)
	{
		openPrompt(Prompt::Rename, e.address);
		return;
	}
	if (col == model::FnColNotes)
	{
		openPrompt(Prompt::Notes, e.address);
		return;
	}
	if (onFunctionActivated) onFunctionActivated(e.address, model::functionActivationName(e));
}

// ─── Actions ─────────────────────────────────────────────────────────────────

void FunctionListPanel::openContextMenu(Vec2 at, int row)
{
	if (ui_.alive(menu_)) w::closePopup(ui_, menu_);
	menuActions_.clear();
	menuLabels_.clear();
	menuAddress_.reset();
	const bool has = row >= 0 && static_cast<std::size_t>(row) < proxy_.rowCount();
	if (has) menuAddress_ = proxy_.entry(static_cast<std::size_t>(row)).address;
	const bool hasSelection = !selected_.empty();

	std::string items;
	auto add = [&](Action a, std::string label) {
		if (!items.empty()) items += '\n';
		items += label;
		menuActions_.push_back(a);
		menuLabels_.push_back(std::move(label));
	};
	auto sep = [&] {
		if (!items.empty()) items += "\n-";
	};
	const std::string dots(kEllipsis);
	if (has)
	{
		add(Action::GoToFunction, "Go to Function");
		add(Action::Redecompile, "Re-decompile Selected Function");
		add(Action::Rename, "Rename" + dots);
		add(Action::EditSignature, "Edit signature" + dots);
		add(Action::EditNotes, "Edit notes" + dots);
		add(Action::ClearAnnotation, "Clear annotation");
		sep();
	}
	if (hasSelection)
	{
		add(Action::AddTag, "Add Tag to Selected" + dots);
		sep();
	}
	if (has)
	{
		add(Action::CopyAddress, "Copy Address");
		add(Action::CopySignature, "Copy Signature");
		sep();
	}
	add(Action::ExportCsv, "Export CSV");
	add(Action::ExportJson, "Export JSON");
	// popupMenu numbers items by line, separators included: map lines back.
	std::vector<Action> byLine;
	std::size_t k = 0;
	std::size_t start = 0;
	while (start <= items.size())
	{
		const std::size_t nl = std::min(items.find('\n', start), items.size());
		const std::string_view line(items.data() + start, nl - start);
		byLine.push_back(line == "-" ? Action::CopyCsv : menuActions_[k]);
		if (line != "-") ++k;
		start = nl + 1;
	}
	menuActions_ = std::move(byLine);
	menu_ = w::popupMenu(ui_, host_, items, at);
}

void FunctionListPanel::triggerAction(Action a, std::optional<uint64_t> address)
{
	if (!address) address = current_;
	const model::FunctionEntry* e = address ? list_.find(*address) : nullptr;
	switch (a)
	{
	case Action::GoToFunction:
		if (e && onAddressActivated) onAddressActivated(e->address);
		break;
	case Action::Redecompile:
		if (!e) break;
		{
			const uint64_t addr = e->address;
			const std::string name = model::functionActivationName(*e);
			const std::string cli = e->selectFunctionsCliName();
			if (onFunctionActivated) onFunctionActivated(addr, name);
			if (onRedecompileRequested) onRedecompileRequested(addr, cli);
		}
		break;
	case Action::Rename:
		if (e) openPrompt(Prompt::Rename, e->address);
		break;
	case Action::EditSignature:
		if (e) openPrompt(Prompt::Signature, e->address);
		break;
	case Action::EditNotes:
		if (e) openPrompt(Prompt::Notes, e->address);
		break;
	case Action::ClearAnnotation:
		if (e)
		{
			const uint64_t addr = e->address;
			list_.clearAnnotation(addr);
			notifyAnnotation(addr);
			refresh(true);
		}
		break;
	case Action::AddTag:
		if (!selected_.empty()) openPrompt(Prompt::Tag, address.value_or(0));
		break;
	case Action::CopyAddress:
		if (e) copy(model::hexAddress(e->address));
		break;
	case Action::CopySignature:
		if (e) copy(e->signature);
		break;
	case Action::ExportCsv: openExportDialog(false); break;
	case Action::ExportJson: openExportDialog(true); break;
	case Action::CopyCsv:
		copy(csvText());
		ui_.setText(status_, std::format("Copied {} functions", proxy_.rowCount()));
		break;
	}
}

bool FunctionListPanel::copy(const std::string& text)
{
	if (clipboardWriter) return clipboardWriter(text);
	return ui_.setClipboardText(text);
}

void FunctionListPanel::notifyAnnotation(uint64_t address)
{
	if (!onAnnotationChanged) return;
	if (const auto* e = list_.find(address)) onAnnotationChanged(*e);
}

// ─── Prompt dialog ───────────────────────────────────────────────────────────

void FunctionListPanel::buildPrompt()
{
	if (ui_.alive(promptDlg_)) return;
	promptDlg_ = w::dialog(ui_, "Rename Function");
	promptTitle_ = ui_.firstChild(promptDlg_);
	const NodeHandle body = w::dialogBody(ui_, promptDlg_);
	ui_.setCss(body, "gap", "8px");
	promptLabel_ = w::label(ui_, body, "");
	promptEdit_ = w::lineEdit(ui_, body);
	ui_.setCss(promptEdit_, "width", "360px");
	const NodeHandle buttons = w::panel(ui_, body);
	ui_.addClass(buttons, "row");
	ui_.addClass(buttons, "buttons");
	const NodeHandle ok = w::button(ui_, buttons, "OK");
	ui_.addClass(ok, "primary");
	const NodeHandle cancel = w::button(ui_, buttons, "Cancel");
	auto accept = [this](UiContext& u, NodeHandle, const SignalArgs&) {
		applyPrompt(std::string(u.text(promptEdit_)));
	};
	ui_.connect(ok, SignalKind::Clicked, accept);
	ui_.connect(promptEdit_, SignalKind::Submitted, accept);
	ui_.connect(cancel, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { cancelPrompt(); });
}

void FunctionListPanel::openPrompt(Prompt p, uint64_t address)
{
	const model::FunctionEntry* e = list_.find(address);
	if (p != Prompt::Tag && !e) return;
	buildPrompt();
	prompt_ = p;
	promptAddress_ = address;
	promptTargets_.clear();
	std::string title, label, value;
	const std::string name = e ? model::functionActivationName(*e) : std::string();
	switch (p)
	{
	case Prompt::Rename:
		title = "Rename Function";
		label = std::format("New name for  0x{:x}:", address);
		value = name;
		break;
	case Prompt::Signature:
		title = "Edit signature";
		label = name;
		value = e->signature;
		break;
	case Prompt::Notes:
		title = "Edit notes";
		label = name;
		value = e->notes;
		break;
	case Prompt::Tag:
		title = "Add Tag";
		label = "Tag name:";
		promptTargets_ = selectedAddresses();
		break;
	case Prompt::None: return;
	}
	ui_.setText(promptTitle_, title);
	ui_.setText(promptLabel_, label);
	setEditText(ui_, promptEdit_, value);
	ui_.editBuffer(promptEdit_).selectAll();
	w::openDialog(ui_, promptDlg_);
	ui_.setFocus(promptEdit_, fuse::ui::FocusReason::Keyboard);
}

bool FunctionListPanel::promptOpen() const
{
	return prompt_ != Prompt::None && ui_.alive(promptDlg_) && w::popupOpen(ui_, ui_.parent(promptDlg_));
}

void FunctionListPanel::acceptPrompt(std::string_view text)
{
	if (prompt_ == Prompt::None) return;
	setEditText(ui_, promptEdit_, text);
	applyPrompt(std::string(text));
}

void FunctionListPanel::cancelPrompt()
{
	prompt_ = Prompt::None;
	if (ui_.alive(promptDlg_)) w::closeDialog(ui_, promptDlg_);
	ui_.setFocus(table_, fuse::ui::FocusReason::Programmatic);
}

void FunctionListPanel::applyPrompt(const std::string& text)
{
	const Prompt p = prompt_;
	prompt_ = Prompt::None;
	if (ui_.alive(promptDlg_)) w::closeDialog(ui_, promptDlg_);
	ui_.setFocus(table_, fuse::ui::FocusReason::Programmatic);
	switch (p)
	{
	case Prompt::Rename:
		// Looked up again by address: a reload while the dialog was open
		// replaced the rows, an address survives that.
		list_.renameFunction(promptAddress_, text);
		break;
	case Prompt::Signature:
		if (list_.setSignature(promptAddress_, text)) notifyAnnotation(promptAddress_);
		break;
	case Prompt::Notes:
		if (list_.setNotes(promptAddress_, text)) notifyAnnotation(promptAddress_);
		break;
	case Prompt::Tag:
		if (!model::funclistTrim(text).empty())
		{
			list_.applyTag(promptTargets_, text);
			for (uint64_t a: promptTargets_)
				notifyAnnotation(a);
		}
		break;
	case Prompt::None: return;
	}
	refresh(true);
}

// ─── Export ──────────────────────────────────────────────────────────────────

std::vector<const model::FunctionEntry*> FunctionListPanel::visibleEntries() const
{
	std::vector<const model::FunctionEntry*> out;
	out.reserve(proxy_.rowCount());
	for (std::size_t r = 0; r < proxy_.rowCount(); ++r)
		out.push_back(&proxy_.entry(r));
	return out;
}

std::string FunctionListPanel::csvText() const
{
	const auto rows = visibleEntries();
	return model::functionListCsv(rows);
}

std::string FunctionListPanel::jsonText() const
{
	const auto rows = visibleEntries();
	return model::functionListJson(rows);
}

std::expected<void, std::string> FunctionListPanel::exportCsv(const std::string& path) const
{
	return model::funclistWriteTextFile(path, csvText());
}

std::expected<void, std::string> FunctionListPanel::exportJson(const std::string& path) const
{
	return model::funclistWriteTextFile(path, jsonText());
}

void FunctionListPanel::openExportDialog(bool json)
{
	if (ui_.alive(fileDlg_)) ui_.destroy(ui_.parent(fileDlg_));
	std::error_code ec;
	const std::string dir = std::filesystem::current_path(ec).generic_string();
	fileDlgJson_ = json;
	fileDlg_ = w::fileDialog(
		ui_, json ? "Export Function List (JSON)" : "Export Function List (CSV)", dir, json ? "*.json" : "*.csv", true);
	// Pre-fill the file name (third row of the dialog body: label + line edit).
	const NodeHandle body = w::dialogBody(ui_, fileDlg_);
	NodeHandle row = ui_.firstChild(body);
	for (int i = 0; i < 2 && ui_.alive(row); ++i)
		row = ui_.nextSibling(row);
	if (ui_.alive(row))
	{
		const NodeHandle nameEdit = ui_.nextSibling(ui_.firstChild(row));
		if (ui_.alive(nameEdit)) setEditText(ui_, nameEdit, json ? "functions.json" : "functions.csv");
	}
	ui_.connect(fileDlg_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (a.text.empty()) return;
		const std::string path(a.text);
		const auto r = fileDlgJson_ ? exportJson(path) : exportCsv(path);
		ui_.setText(
			status_,
			r ? std::format("Exported {} functions to {}", proxy_.rowCount(), path) : "Export failed: " + r.error());
	});
	w::openDialog(ui_, fileDlg_);
}

// ─── Test hooks ──────────────────────────────────────────────────────────────

std::string FunctionListPanel::cellText(std::size_t row, int col) const
{
	if (row >= proxy_.rowCount()) return {};
	return model::functionCellText(proxy_.entry(row), col, demangle_);
}

uint64_t FunctionListPanel::rowAddress(std::size_t row) const
{
	return row < proxy_.rowCount() ? proxy_.entry(row).address : 0;
}

Vec2 FunctionListPanel::cellCenter(std::size_t row, int col) const
{
	const Rect c = ui_.contentRect(table_);
	const float rh = rowHeight(ui_, table_);
	float x = c.x;
	for (int k = 0; k < col; ++k)
		x += w::tableColumnWidth(ui_, table_, static_cast<fuse::ui::u32>(k));
	x += w::tableColumnWidth(ui_, table_, static_cast<fuse::ui::u32>(col)) * 0.5f;
	const float rel = static_cast<float>(row) - static_cast<float>(w::tableFirstVisible(ui_, table_));
	return {x, c.y + rh + rel * rh + rh * 0.5f};
}

Vec2 FunctionListPanel::headerCenter(int col) const
{
	const Rect c = ui_.contentRect(table_);
	const float rh = rowHeight(ui_, table_);
	float x = c.x;
	for (int k = 0; k < col; ++k)
		x += w::tableColumnWidth(ui_, table_, static_cast<fuse::ui::u32>(k));
	x += w::tableColumnWidth(ui_, table_, static_cast<fuse::ui::u32>(col)) * 0.5f;
	return {x, c.y + rh * 0.5f};
}

// ─── Cell painting ───────────────────────────────────────────────────────────

void FunctionListPanel::paintCell(
	const void* user,
	const fuse::ui::PaintContext& pc,
	fuse::ui::DrawList& dl,
	Rect cell,
	fuse::ui::u32 row,
	fuse::ui::u32 col,
	std::string_view text,
	bool selected)
{
	const auto* painter = static_cast<const ColumnPainter*>(user);
	const FunctionListPanel& p = *painter->panel;
	if (row >= p.proxy_.rowCount()) return;
	const model::FunctionEntry& e = p.proxy_.entry(row);
	UiContext& ui = pc.ui;
	const float fs = pc.style.fontSize;
	const ThemeColors& P = themeColors(ui);

	switch (static_cast<int>(col))
	{
	case model::FnColConfidence: {
		const float conf = std::clamp(e.confidence.composite(), 0.f, 1.f);
		const Rect track{cell.x - 2.f, cell.y + 4.f, cell.w + 4.f, cell.h - 8.f};
		dl.rect(track, pc.tint(Color::rgb(P.surface0)), 3.f);
		const Rect fill{track.x, track.y, std::round(track.w * conf), track.h};
		if (fill.w > 0.f)
			dl.rect(fill, pc.tint(Color::rgb(tierColor(P, model::confidenceTier(conf)), 200.f / 255.f)), 3.f);
		// Text on the bar's fill where it covers the middle, body text over the track.
		const uint32_t textColor = conf >= 0.5f ? P.onAccent : P.text;
		ui.paintString(
			dl, text, track, pc.tint(Color::rgb(textColor)), fs - 2.f, fuse::ui::TextAlignment::Center, pc.node.index);
		break;
	}
	case model::FnColPatterns: {
		constexpr float kBadge = 14.f, kSpacing = 2.f;
		float x = cell.x - 2.f;
		const float y = cell.y + std::floor((cell.h - kBadge) * 0.5f);
		for (int i = 0; i < 5; ++i)
		{
			const Rect r{x, y, kBadge, kBadge};
			const auto& badge = model::patternBadges()[static_cast<std::size_t>(i)];
			if (model::patternActive(e.patterns, i))
			{
				dl.rect(r, pc.tint(Color::rgb(badge.rgb)), 2.f);
				ui.paintString(
					dl,
					badge.label,
					r,
					pc.tint(Color::rgb(kBadgeText)),
					8.f,
					fuse::ui::TextAlignment::Center,
					pc.node.index);
			}
			else
				dl.roundedRect(
					r, pc.tint(Color::rgb(P.surface0)), {2.f, 2.f, 2.f, 2.f}, pc.tint(Color::rgb(P.surface1)), 0.5f);
			x += kBadge + kSpacing;
		}
		break;
	}
	default: {
		uint32_t color = P.text;
		switch (static_cast<int>(col))
		{
		case model::FnColAddress: color = P.overlay1; break;
		case model::FnColName: color = e.isLibrary ? P.teal : P.text; break;
		case model::FnColSize:
		case model::FnColInstrs:
		case model::FnColNotes: color = P.subtext0; break;
		case model::FnColCC: color = P.mauve; break;
		default: break;
		}
		// Qt: Address and Name in the monospace face.
		const bool mono = col == model::FnColAddress || col == model::FnColName;
		ui.paintString(
			dl,
			text,
			cell,
			pc.tint(Color::rgb(color)),
			fs,
			fuse::ui::TextAlignment::Start,
			pc.node.index,
			mono ? styledFont(ui, pc.node, true, false) : nullptr);
		break;
	}
	}
}

} // namespace retdec::gui::ui
