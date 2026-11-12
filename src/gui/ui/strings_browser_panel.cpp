/**
 * @file src/gui/ui/strings_browser_panel.cpp
 * @brief Strings & Constants panel (FuseUI).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/strings_browser_panel.h"

#include "retdec/gui/ui/empty_state.h"

#include "retdec/gui/model/funclist_export.h"

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

// Capture-phase hook on the node hosting each table (context menu, cell
// tooltips, Ctrl+C, Shift+F10); see function_list_panel.cpp. Double click /
// Enter: the table's Activated signal.
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

struct StringsHost
{
	static constexpr std::string_view kName = "rd-strings-host";
	static bool onEvent(fuse::ui::EventContext& ec, const Event& e)
	{
		if (ec.phase != fuse::ui::Phase::Capture) return false;
		auto it = hookMap().find({&ec.ui, ec.node.index, ec.node.generation});
		if (it == hookMap().end()) return false;
		if (auto h = it->second.lock()) return (*h)(ec.ui, e);
		return false;
	}
};

constexpr uint32_t kAddrColor = 0x6c7086;
constexpr uint32_t kTextColor = 0xcdd6f4;
constexpr uint32_t kDimColor = 0xa6adc8;

constexpr std::string_view kTypeItems = "All Types\nASCII\nUTF-8\nUTF-16 LE\nUTF-16 BE\nWide\nPascal";
constexpr std::string_view kCatItems = "All Cats\nPlain\nFormatStr\nURL\nFilePath\nRegEx\nCrypto";

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

std::string_view comboItem(std::string_view items, int index)
{
	std::size_t at = 0;
	for (int i = 0; i < index; ++i)
	{
		at = items.find('\n', at);
		if (at == std::string_view::npos) return {};
		++at;
	}
	const std::size_t end = items.find('\n', at);
	return items.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
}

} // namespace

StringsBrowserPanel::StringsBrowserPanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	build(parent);
	refresh(Table::Strings);
	refresh(Table::Constants);
}

StringsBrowserPanel::~StringsBrowserPanel()
{
	for (auto& k: tables_)
		hookMap().erase({&ui_, k.host.index, k.host.generation});
	if (ui_.alive(fileDlg_)) ui_.destroy(ui_.parent(fileDlg_));
	if (ui_.alive(menu_)) ui_.destroy(menu_);
	if (cellTipPending_) ui_.cancelTimer(cellTipTimer_);
	if (ui_.alive(cellTip_)) ui_.destroy(cellTip_);
	for (NodeHandle t: tooltips_)
		if (ui_.alive(t)) ui_.destroy(t);
	if (ui_.alive(root_)) ui_.destroy(root_);
}

void StringsBrowserPanel::tooltip(NodeHandle n, std::string_view text)
{
	tooltips_.push_back(w::setTooltip(ui_, n, text));
}

void StringsBrowserPanel::build(NodeHandle parent)
{
	if (ui_.registry().find("lineedit") == 0xFFFFFFFFu) w::registerTierA(ui_);
	w::registerTierB(ui_);
	if (ui_.registry().find(StringsHost::kName) == 0xFFFFFFFFu) ui_.registry().registerWidget<StringsHost>();

	root_ = w::panel(ui_, parent);
	ui_.addClass(root_, "rd-strings");
	ui_.setA11y(root_, fuse::ui::A11yRole::Group, kTitle);
	ui_.setCss(root_, "flex-grow", "1");
	ui_.setCss(root_, "padding", "0px");
	ui_.setCss(root_, "gap", "0px");

	tabs_ = w::tabWidget(ui_, root_, "Strings\nConstants");
	ui_.setCss(tabs_, "flex-grow", "1");
	for (int t = 0; t < 2; ++t)
	{
		const NodeHandle page = w::tabPage(ui_, tabs_, static_cast<fuse::ui::u32>(t));
		ui_.setCss(page, "flex-grow", "1");
		ui_.setCss(page, "padding", "4px 4px 2px 4px");
		ui_.setCss(page, "gap", "4px");
		buildTable(static_cast<Table>(t), page);
	}
}

void StringsBrowserPanel::buildTable(Table t, NodeHandle page)
{
	TableKit& k = tables_[idx(t)];
	const bool isStr = t == Table::Strings;

	// ── toolbar ─────────────────────────────────────────────────────────────
	const NodeHandle bar = w::panel(ui_, page);
	ui_.setCss(bar, "flex-direction", "row");
	ui_.setCss(bar, "align-items", "center");
	ui_.setCss(bar, "gap", "6px");
	ui_.setCss(bar, "padding", "0px");
	ui_.setCss(bar, "flex-wrap", "wrap");
	auto compact = [&](NodeHandle n) {
		ui_.setCss(n, "padding", "3px 6px");
		ui_.setCss(n, "font-size", "13px");
		ui_.setCss(n, "flex-shrink", "0");
	};

	k.search = w::lineEdit(ui_, bar);
	ui_.setCss(k.search, "width", isStr ? "240px" : "220px");
	compact(k.search);
	ui_.setA11y(k.search, fuse::ui::A11yRole::TextField, isStr ? "Filter strings" : "Filter constants");
	w::setPlaceholderText(ui_, k.search, isStr ? "Filter strings\xE2\x80\xA6  (text or /regex/)" : "Filter constants\xE2\x80\xA6");
	ui_.connect(k.search, SignalKind::TextChanged, [this, isStr](UiContext&, NodeHandle, const SignalArgs&) {
		if (isStr)
			applyStringFilter();
		else
			applyConstantFilter();
	});

	if (isStr)
	{
		compact(w::label(ui_, bar, "Type:"));
		typeCombo_ = w::comboBox(ui_, bar, kTypeItems);
		compact(typeCombo_);
		ui_.setCss(typeCombo_, "width", "110px");
		compact(w::label(ui_, bar, "Cat:"));
		catCombo_ = w::comboBox(ui_, bar, kCatItems);
		compact(catCombo_);
		ui_.setCss(catCombo_, "width", "110px");
		regexBtn_ = w::toolButton(ui_, bar, ".*");
		compact(regexBtn_);
		ui_.setCss(regexBtn_, "border-width", "1px");
		ui_.setCss(regexBtn_, "border-color", "#474955");
		tooltip(regexBtn_, "Enable regex search");
		auto onCombo = [this](UiContext&, NodeHandle, const SignalArgs&) { applyStringFilter(); };
		ui_.connect(typeCombo_, SignalKind::ValueChanged, onCombo);
		ui_.connect(catCombo_, SignalKind::ValueChanged, onCombo);
		ui_.connect(regexBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
			setRegexEnabled(!regexEnabled());
		});
	}
	else
	{
		std::string items;
		for (const auto& kind: model::constantKindFilterItems())
		{
			if (!items.empty()) items += '\n';
			items += model::constantLabelKindName(kind.value_or(model::ConstantLabel::None));
		}
		kindCombo_ = w::comboBox(ui_, bar, items);
		compact(kindCombo_);
		ui_.setCss(kindCombo_, "width", "110px");
		tooltip(kindCombo_, "Show one kind of labelled constant (the text filter applies to All)");
		ui_.connect(kindCombo_, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs&) {
			applyConstantFilter();
		});
	}

	k.exportBtn = w::toolButton(ui_, bar, "CSV");
	compact(k.exportBtn);
	ui_.setCss(k.exportBtn, "border-width", "1px");
	ui_.setCss(k.exportBtn, "border-color", "#474955");
	tooltip(k.exportBtn, isStr ? "Export strings to CSV" : "Export constants to CSV");
	ui_.connect(k.exportBtn, SignalKind::Clicked, [this, t](UiContext&, NodeHandle, const SignalArgs&) {
		openExportDialog(t);
	});

	// ── table ───────────────────────────────────────────────────────────────
	k.host = ui_.create(ui_.registry().find(StringsHost::kName), page);
	ui_.setCss(k.host, "flex-grow", "1");
	k.hook = std::make_shared<Hook>([this, t](UiContext&, const Event& e) { return onTableEvent(t, e); });
	hookMap()[{&ui_, k.host.index, k.host.generation}] = k.hook;

	k.painter = {this, static_cast<int>(t)};
	k.model.user = &k.painter;
	k.model.columns = static_cast<fuse::ui::u32>(isStr ? static_cast<int>(model::StrColCount) : static_cast<int>(model::ConstColCount));
	k.model.rows = [](const void* u) {
		const auto* p = static_cast<const Painter*>(u);
		return static_cast<fuse::ui::u32>(p->panel->visibleRowCount(static_cast<Table>(p->table)));
	};
	k.model.data = [](const void* u, fuse::ui::u32 r, fuse::ui::u32 c, std::string& scratch) -> std::string_view {
		const auto* p = static_cast<const Painter*>(u);
		scratch = p->panel->cellText(static_cast<Table>(p->table), r, static_cast<int>(c));
		return scratch;
	};
	if (isStr)
		k.model.header = [](const void*, fuse::ui::u32 c) { return model::stringsColumnHeader(static_cast<int>(c)); };
	else
		k.model.header = [](const void*, fuse::ui::u32 c) { return model::constantsColumnHeader(static_cast<int>(c)); };
	k.table = w::tableView(ui_, k.host, &k.model);
	ui_.setA11y(k.table, fuse::ui::A11yRole::List, isStr ? "Strings table" : "Constants table");
	ui_.setCss(k.table, "flex-grow", "1");
	ui_.setCss(k.table, "background", "#1e1e2e");
	ui_.setCss(k.table, "color", "#cdd6f4");
	ui_.setCss(k.table, "font-size", "13px");
	k.delegate = {&k.painter, &StringsBrowserPanel::paintCell};
	if (isStr)
	{
		constexpr float kW[model::StrColCount] = {150.f, 84.f, 84.f, 520.f, 56.f};
		for (int c = 0; c < model::StrColCount; ++c)
			w::setTableColumnWidth(ui_, k.table, static_cast<fuse::ui::u32>(c), kW[c]);
	}
	else
	{
		constexpr float kW[model::ConstColCount] = {150.f, 50.f, 150.f, 170.f, 220.f, 56.f};
		for (int c = 0; c < model::ConstColCount; ++c)
			w::setTableColumnWidth(ui_, k.table, static_cast<fuse::ui::u32>(c), kW[c]);
	}
	for (fuse::ui::u32 c = 0; c < k.model.columns; ++c)
		w::setTableDelegate(ui_, k.table, c, &k.delegate);
	w::setTableActivation(ui_, k.table, true);

	ui_.connect(k.table, SignalKind::Toggled, [this, t](UiContext&, NodeHandle, const SignalArgs& a) {
		sortBy(t, static_cast<int>(a.value), a.flag);
	});
	ui_.connect(k.table, SignalKind::Activated, [this, t](UiContext&, NodeHandle, const SignalArgs& a) {
		activate(t, static_cast<int>(a.value)); // double click / Enter
	});
	ui_.connect(k.host, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		// Context menu: 0 = Copy Address, 1 = Copy Value.
		copyRow(menuTable_, menuRow_, a.value >= 0.5f);
	});

	k.status = w::label(ui_, page, isStr ? "0 strings" : "0 constants");
	ui_.addClass(k.status, "dim");
	ui_.setCss(k.status, "text-align", "end");
	ui_.setCss(k.status, "font-size", "12px");
}

// ─── Data ────────────────────────────────────────────────────────────────────

void StringsBrowserPanel::setStrings(std::vector<model::StringEntry> entries)
{
	strings_.setStrings(std::move(entries));
	refresh(Table::Strings);
}

void StringsBrowserPanel::appendStrings(std::vector<model::StringEntry> entries)
{
	strings_.appendStrings(std::move(entries));
	refresh(Table::Strings);
}

void StringsBrowserPanel::setConstants(std::vector<model::ConstantEntry> entries)
{
	constants_.setConstants(std::move(entries));
	refresh(Table::Constants);
}

void StringsBrowserPanel::clear()
{
	strings_.clear();
	constants_.clear();
	setEditText(ui_, tables_[0].search, "");
	setEditText(ui_, tables_[1].search, "");
	ui_.setValue(typeCombo_, 0.f);
	ui_.setValue(catCombo_, 0.f);
	ui_.setValue(kindCombo_, 0.f);
	applyStringFilter();
	applyConstantFilter();
}

// ─── Filters ─────────────────────────────────────────────────────────────────

void StringsBrowserPanel::applyStringFilter()
{
	const int type = static_cast<int>(ui_.value(typeCombo_));
	const int cat = static_cast<int>(ui_.value(catCombo_));
	strProxy_.setTypeFilter(type == 0 ? std::string_view{} : comboItem(kTypeItems, type));
	strProxy_.setCategoryFilter(cat == 0 ? std::string_view{} : comboItem(kCatItems, cat));
	const auto [text, regex] = model::parseStringSearch(ui_.text(tables_[0].search), regexEnabled());
	strProxy_.setTextFilter(text, regex);
	refresh(Table::Strings);
}

void StringsBrowserPanel::applyConstantFilter()
{
	const auto& kinds = model::constantKindFilterItems();
	const auto i = static_cast<std::size_t>(std::max(0.f, ui_.value(kindCombo_)));
	constProxy_.setKindFilter(i < kinds.size() ? kinds[i] : std::nullopt);
	constProxy_.setTextFilter(ui_.text(tables_[1].search));
	refresh(Table::Constants);
}

void StringsBrowserPanel::setStringFilterText(std::string_view text)
{
	setEditText(ui_, tables_[0].search, text);
	applyStringFilter();
}

std::string StringsBrowserPanel::stringFilterText() const
{
	return std::string(ui_.text(tables_[0].search));
}

void StringsBrowserPanel::setTypeFilterIndex(int index)
{
	ui_.setValue(typeCombo_, static_cast<float>(index));
	applyStringFilter();
}

void StringsBrowserPanel::setCategoryFilterIndex(int index)
{
	ui_.setValue(catCombo_, static_cast<float>(index));
	applyStringFilter();
}

void StringsBrowserPanel::setRegexEnabled(bool on)
{
	ui_.setChecked(regexBtn_, on);
	ui_.setCss(regexBtn_, "background", on ? "#3d8bfd" : "transparent");
	applyStringFilter();
}

bool StringsBrowserPanel::regexEnabled() const
{
	return ui_.checked(regexBtn_);
}

void StringsBrowserPanel::setConstantFilterText(std::string_view text)
{
	setEditText(ui_, tables_[1].search, text);
	applyConstantFilter();
}

void StringsBrowserPanel::setConstantKindIndex(int index)
{
	ui_.setValue(kindCombo_, static_cast<float>(index));
	applyConstantFilter();
}

void StringsBrowserPanel::sortBy(Table t, int column, bool ascending)
{
	if (t == Table::Strings)
		strProxy_.sortBy(column, ascending);
	else
		constProxy_.sortBy(column, ascending);
	refresh(t);
}

void StringsBrowserPanel::showTab(int index)
{
	ui_.setValue(w::tabWidgetBar(ui_, tabs_), static_cast<float>(index), true);
}

int StringsBrowserPanel::currentTab() const
{
	return static_cast<int>(ui_.value(w::tabWidgetBar(ui_, tabs_)));
}

void StringsBrowserPanel::refresh(Table t)
{
	if (t == Table::Strings)
		strProxy_.invalidate();
	else
		constProxy_.invalidate();
	const NodeHandle table = tables_[idx(t)].table;
	w::tableModelChanged(ui_, table);
	if (ui_.value(table) >= static_cast<float>(visibleRowCount(t))) ui_.setValue(table, -1.f);
	ui_.invalidate(table);
	updateStatus();
}

void StringsBrowserPanel::updateStatus()
{
	ui_.setText(tables_[0].status, statusText(Table::Strings));
	ui_.setText(tables_[1].status, statusText(Table::Constants));
}

std::string StringsBrowserPanel::statusText(Table t) const
{
	return t == Table::Strings ? std::format("{} / {} strings", strProxy_.rowCount(), strings_.size())
							   : std::format("{} / {} constants", constProxy_.rowCount(), constants_.size());
}

// ─── Table events ────────────────────────────────────────────────────────────

std::size_t StringsBrowserPanel::visibleRowCount(Table t) const
{
	return t == Table::Strings ? strProxy_.rowCount() : constProxy_.rowCount();
}

std::string StringsBrowserPanel::cellText(Table t, std::size_t row, int col) const
{
	if (row >= visibleRowCount(t)) return {};
	return t == Table::Strings ? model::stringCellText(strProxy_.entry(row), col)
							   : model::constantCellText(constProxy_.entry(row), col);
}

uint64_t StringsBrowserPanel::rowAddress(Table t, std::size_t row) const
{
	if (row >= visibleRowCount(t)) return 0;
	return t == Table::Strings ? strProxy_.entry(row).address : constProxy_.entry(row).address;
}

int StringsBrowserPanel::rowAt(Table t, Vec2 p) const
{
	const NodeHandle table = tables_[idx(t)].table;
	const Rect c = ui_.contentRect(table);
	const float rh = rowHeight(ui_, table);
	if (p.y < c.y + rh || p.y >= c.bottom()) return -1;
	const std::size_t r = w::tableFirstVisible(ui_, table) + static_cast<std::size_t>((p.y - c.y - rh) / rh);
	return r < visibleRowCount(t) ? static_cast<int>(r) : -1;
}

bool StringsBrowserPanel::onTableEvent(Table t, const Event& e)
{
	TableKit& k = tables_[idx(t)];
	const int current = static_cast<int>(ui_.value(k.table));
	if (e.type == EventType::PointerMove)
	{
		if (!ui_.pressed().valid()) hoverCell(t, e.pos);
		return false;
	}
	if (e.type == EventType::PointerLeave || e.type == EventType::Wheel)
	{
		hideCellTip();
		tipRow_ = tipCol_ = -1;
		return false;
	}
	hideCellTip();
	if (e.type == EventType::KeyDown)
	{
		if ((e.mods & fuse::ui::kModCtrl) && e.key == Key::C)
		{
			copyRow(t, current, false);
			return true;
		}
		if ((e.mods & fuse::ui::kModShift) && e.key == Key::F10)
		{
			const Rect c = ui_.contentRect(k.table);
			openContextMenu(
				t,
				current >= 0 ? cellCenter(t, static_cast<std::size_t>(current), 0) : Vec2{c.x + 24.f, c.y + 24.f},
				current);
			return true;
		}
		return false;
	}
	if (e.type != EventType::PointerDown) return false;
	const Rect c = ui_.contentRect(k.table);
	const float rh = rowHeight(ui_, k.table);
	if (!c.contains(e.pos) || e.pos.y < c.y + rh) return false;
	const int row = rowAt(t, e.pos);
	if (e.button == fuse::ui::MouseButton::Right)
	{
		if (row >= 0) ui_.setValue(k.table, static_cast<float>(row), true);
		openContextMenu(t, e.pos, row);
		return true;
	}
	return false; // the table selects the row (and activates on a double click)
}

int StringsBrowserPanel::columnAt(Table t, float x) const
{
	const NodeHandle table = tables_[idx(t)].table;
	float at = ui_.contentRect(table).x;
	const int cols = t == Table::Strings ? static_cast<int>(model::StrColCount) : static_cast<int>(model::ConstColCount);
	for (int k = 0; k < cols; ++k)
	{
		at += w::tableColumnWidth(ui_, table, static_cast<fuse::ui::u32>(k));
		if (x < at) return k;
	}
	return -1;
}

void StringsBrowserPanel::hoverCell(Table t, Vec2 p)
{
	const int row = rowAt(t, p);
	const int col = row >= 0 ? columnAt(t, p.x) : -1;
	if (t == tipTable_ && row == tipRow_ && col == tipCol_) return;
	tipTable_ = t;
	tipRow_ = row;
	tipCol_ = col;
	hideCellTip();
	if (row < 0 || col < 0) return;
	tipPos_ = p;
	cellTipPending_ = true;
	cellTipTimer_ = ui_.startTimer(0.5f, false, [this](UiContext&) {
		cellTipPending_ = false;
		if (tipRow_ < 0 || static_cast<std::size_t>(tipRow_) >= visibleRowCount(tipTable_)) return;
		const auto r = static_cast<std::size_t>(tipRow_);
		const std::string text = tipTable_ == Table::Strings ? model::stringToolTip(strProxy_.entry(r), tipCol_)
															 : model::constantToolTip(constProxy_.entry(r), tipCol_);
		if (text.empty()) return;
		if (!ui_.alive(cellTip_))
		{
			cellTip_ = ui_.create("tooltip", w::overlayLayer(ui_)).value_or(NodeHandle{});
			if (!ui_.alive(cellTip_)) return;
			ui_.addClass(cellTip_, "tooltip");
			ui_.setCss(cellTip_, "max-width", "640px");
		}
		ui_.setText(cellTip_, text);
		ui_.setCss(cellTip_, "left", std::format("{}px", tipPos_.x + 12.f));
		ui_.setCss(cellTip_, "top", std::format("{}px", tipPos_.y + 18.f));
		w::overlayLayer(ui_);
		ui_.setCss(cellTip_, "display", "flex");
	});
}

void StringsBrowserPanel::hideCellTip()
{
	if (cellTipPending_)
	{
		ui_.cancelTimer(cellTipTimer_);
		cellTipPending_ = false;
	}
	if (ui_.alive(cellTip_)) ui_.setCss(cellTip_, "display", "none");
}

void StringsBrowserPanel::activate(Table t, int row)
{
	if (row < 0 || static_cast<std::size_t>(row) >= visibleRowCount(t)) return;
	if (onAddressActivated) onAddressActivated(rowAddress(t, static_cast<std::size_t>(row)));
}

void StringsBrowserPanel::openContextMenu(Table t, Vec2 at, int row)
{
	if (ui_.alive(menu_)) w::closePopup(ui_, menu_);
	menuTable_ = t;
	menuRow_ = row;
	menuLabels_.clear();
	if (row < 0 || static_cast<std::size_t>(row) >= visibleRowCount(t)) return;
	menuLabels_ = {"Copy Address", "Copy Value"};
	menu_ = w::popupMenu(ui_, tables_[idx(t)].host, "Copy Address\nCopy Value", at);
}

void StringsBrowserPanel::copyRow(Table t, int row, bool value)
{
	if (row < 0 || static_cast<std::size_t>(row) >= visibleRowCount(t)) return;
	const auto r = static_cast<std::size_t>(row);
	if (!value)
	{
		copy(std::format("0x{:016x}", rowAddress(t, r)));
		return;
	}
	if (t == Table::Strings)
		copy(strProxy_.entry(r).value);
	else
		copy("0x" + constProxy_.entry(r).hexStr);
}

bool StringsBrowserPanel::copy(const std::string& text)
{
	if (clipboardWriter) return clipboardWriter(text);
	return ui_.setClipboardText(text);
}

// ─── Export ──────────────────────────────────────────────────────────────────

std::expected<void, std::string> StringsBrowserPanel::exportStringsCsv(const std::string& path) const
{
	return model::funclistWriteTextFile(path, stringsCsv());
}

std::expected<void, std::string> StringsBrowserPanel::exportConstantsCsv(const std::string& path) const
{
	return model::funclistWriteTextFile(path, constantsCsv());
}

void StringsBrowserPanel::openExportDialog(Table t)
{
	if (ui_.alive(fileDlg_)) ui_.destroy(ui_.parent(fileDlg_));
	const bool isStr = t == Table::Strings;
	fileDlgTable_ = t;
	std::error_code ec;
	fileDlg_ = w::fileDialog(
		ui_, isStr ? "Export Strings" : "Export Constants", std::filesystem::current_path(ec).generic_string(), "*.csv", true);
	const NodeHandle body = w::dialogBody(ui_, fileDlg_);
	NodeHandle row = ui_.firstChild(body);
	for (int i = 0; i < 2 && ui_.alive(row); ++i)
		row = ui_.nextSibling(row);
	if (ui_.alive(row))
	{
		const NodeHandle nameEdit = ui_.nextSibling(ui_.firstChild(row));
		if (ui_.alive(nameEdit)) setEditText(ui_, nameEdit, isStr ? "strings.csv" : "constants.csv");
	}
	ui_.connect(fileDlg_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (a.text.empty()) return;
		const std::string path(a.text);
		const Table tt = fileDlgTable_;
		const auto r = tt == Table::Strings ? exportStringsCsv(path) : exportConstantsCsv(path);
		ui_.setText(tables_[idx(tt)].status, r ? statusText(tt) + "  \xE2\x80\x94  exported to " + path : "Export failed: " + r.error());
	});
	w::openDialog(ui_, fileDlg_);
}

// ─── Test hooks ──────────────────────────────────────────────────────────────

Vec2 StringsBrowserPanel::cellCenter(Table t, std::size_t row, int col) const
{
	const NodeHandle table = tables_[idx(t)].table;
	const Rect c = ui_.contentRect(table);
	const float rh = rowHeight(ui_, table);
	float x = c.x;
	for (int k = 0; k < col; ++k)
		x += w::tableColumnWidth(ui_, table, static_cast<fuse::ui::u32>(k));
	x += w::tableColumnWidth(ui_, table, static_cast<fuse::ui::u32>(col)) * 0.5f;
	const float rel = static_cast<float>(row) - static_cast<float>(w::tableFirstVisible(ui_, table));
	return {x, c.y + rh + rel * rh + rh * 0.5f};
}

Vec2 StringsBrowserPanel::headerCenter(Table t, int col) const
{
	const NodeHandle table = tables_[idx(t)].table;
	const Rect c = ui_.contentRect(table);
	float x = c.x;
	for (int k = 0; k < col; ++k)
		x += w::tableColumnWidth(ui_, table, static_cast<fuse::ui::u32>(k));
	x += w::tableColumnWidth(ui_, table, static_cast<fuse::ui::u32>(col)) * 0.5f;
	return {x, c.y + rowHeight(ui_, table) * 0.5f};
}

// ─── Painting ────────────────────────────────────────────────────────────────

void StringsBrowserPanel::paintCell(
	const void* user,
	const fuse::ui::PaintContext& pc,
	fuse::ui::DrawList& dl,
	Rect cell,
	fuse::ui::u32 row,
	fuse::ui::u32 col,
	std::string_view text,
	bool)
{
	const auto* p = static_cast<const Painter*>(user);
	const StringsBrowserPanel& panel = *p->panel;
	const auto t = static_cast<Table>(p->table);
	if (row >= panel.visibleRowCount(t)) return;
	uint32_t color = kTextColor;
	if (col == 0)
		color = kAddrColor;
	else if (t == Table::Strings)
	{
		const auto& e = panel.strProxy_.entry(row);
		if (col == model::StrColPreview)
		{
			if (const uint32_t c = model::stringCategoryColor(e.category)) color = c;
		}
		else
			color = kDimColor;
	}
	else
	{
		const auto& e = panel.constProxy_.entry(row);
		if (const uint32_t c = model::constantLabelColor(e.labelKind))
			color = c;
		else if (col != model::ConstColLabel)
			color = kDimColor;
	}
	const bool right = (t == Table::Strings && col == model::StrColRefs)
					|| (t == Table::Constants && (col == model::ConstColRefs || col == model::ConstColSize));
	// Addresses and the constants' hex / decimal values: monospace.
	const bool mono = col == 0 || (t == Table::Constants && (col == model::ConstColHex || col == model::ConstColDec));
	pc.ui.paintString(
		dl,
		text,
		cell,
		pc.tint(Color::rgb(color)),
		pc.style.fontSize,
		right ? fuse::ui::TextAlignment::End : fuse::ui::TextAlignment::Start,
		pc.node.index,
		mono ? styledFont(pc.ui, pc.node, true, false) : nullptr);
}

} // namespace retdec::gui::ui
