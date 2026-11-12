/**
 * @file src/gui/ui/diagnostics_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/diagnostics_panel.h"

#include "retdec/gui/model/code_text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

double nowSeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Color severityColor(model::Severity s)
{
	switch (s)
	{
	case model::Severity::Info: return Color::rgb(0xa6e3a1);
	case model::Severity::Warning: return Color::rgb(0xf9e2af);
	case model::Severity::Error: return Color::rgb(0xf38ba8);
	}
	return Color::rgb(0xcdd6f4);
}

constexpr u32 kColumns = 5;

} // namespace

DiagnosticsPanel::DiagnosticsPanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-diagnostics");

	const NodeHandle top = toolbarRow(ui, root_);
	w::label(ui, top, "Show:");
	combo_ = w::comboBox(ui, top, "All\nInfo\nWarnings\nErrors", 0);
	search_ = w::lineEdit(ui, top, "");
	ui.setCss(search_, "width", "200px");
	w::setTooltip(ui, search_, "Filter text\xE2\x80\xA6 (replaces the severity filter while non-empty)");
	copyBtn_ = w::button(ui, top, "Copy");
	w::setTooltip(ui, copyBtn_, "Copy visible rows as TSV");
	saveBtn_ = w::button(ui, top, "Save\xE2\x80\xA6");
	w::setTooltip(ui, saveBtn_, "Save visible rows as TSV");
	const NodeHandle spacer = w::panel(ui, top);
	ui.setCss(spacer, "flex-grow", "1");
	count_ = w::label(ui, top, "0 entries");
	ui.addClass(count_, "retdec-muted");

	model_.user = this;
	model_.columns = kColumns;
	model_.rows = [](const void* u) { return u32(static_cast<const DiagnosticsPanel*>(u)->rows_.size()); };
	model_.data = [](const void* u, u32 row, u32 col, std::string& scratch) -> std::string_view {
		scratch = static_cast<const DiagnosticsPanel*>(u)->cellText(int(row), int(col));
		return scratch;
	};
	model_.header = [](const void*, u32 col) -> std::string_view {
		static constexpr std::string_view kHeaders[] = {"Severity", "Stage", "Message", "Address", "Function"};
		return col < kColumns ? kHeaders[col] : std::string_view{};
	};
	table_ = w::tableView(ui, root_, &model_);
	ui.setCss(table_, "flex-grow", "1");
	ui.setCss(table_, "min-height", "0px");
	w::setTableColumnWidth(ui, table_, 0, 100.f);
	w::setTableColumnWidth(ui, table_, 1, 120.f);
	w::setTableColumnWidth(ui, table_, 2, 460.f);
	w::setTableColumnWidth(ui, table_, 3, 110.f);
	w::setTableColumnWidth(ui, table_, 4, 140.f);

	delegate_.user = this;
	delegate_.paint = [](const void* u, const PaintContext& pc, DrawList& dl, Rect cell, u32 row, u32, std::string_view text, bool) {
		const auto* self = static_cast<const DiagnosticsPanel*>(u);
		if (row >= self->rows_.size()) return;
		const Color c = severityColor(self->store_.at(self->rows_[row]).severity);
		pc.ui.paintString(dl, text, cell, pc.tint(c), pc.style.fontSize, TextAlignment::Start, pc.node.index);
	};
	for (u32 c = 0; c < kColumns; ++c)
		w::setTableDelegate(ui, table_, c, &delegate_);

	auto* self = this;
	ui.connect(combo_, SignalKind::ValueChanged, [self](UiContext&, NodeHandle, const SignalArgs& a) {
		self->filter_ = static_cast<model::SeverityFilter>(std::clamp(int(a.value), 0, 3));
		self->refilter();
	});
	ui.connect(search_, SignalKind::TextChanged, [self](UiContext&, NodeHandle, const SignalArgs& a) {
		self->text_ = std::string(a.text);
		self->refilter();
	});
	ui.connect(copyBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->copyVisible(); });
	ui.connect(saveBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->saveVisible(); });
	ui.connect(table_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs& a) { self->activateRow(int(a.value)); });
	ui.connect(table_, SignalKind::Toggled, [self](UiContext&, NodeHandle, const SignalArgs& a) {
		self->sortCol_ = int(a.value);
		self->sortAsc_ = a.flag;
		self->refilter();
	});
	refilter();
}

DiagnosticsPanel::~DiagnosticsPanel()
{
	if (ui_.alive(saveDlg_)) ui_.destroy(saveDlg_);
	releasePanelRoot(ui_, root_);
}

bool DiagnosticsPanel::handleEvent(EventContext& ec, const Event& e)
{
	// Double-click on a table row: two left presses at the same spot.
	if (ec.phase == Phase::Capture && e.type == EventType::PointerDown && e.button == MouseButton::Left && ec.target == table_)
	{
		const double now = nowSeconds();
		const bool dbl = now - lastDown_ < 0.45 && std::abs(e.pos.x - lastDownPos_.x) < 5.f && std::abs(e.pos.y - lastDownPos_.y) < 5.f;
		lastDown_ = dbl ? -1.0 : now;
		lastDownPos_ = e.pos;
		if (dbl)
		{
			const int row = int(ui_.value(table_));
			if (row >= 0) activateRow(row);
			return true;
		}
	}
	return false;
}

std::string DiagnosticsPanel::cellText(int row, int col) const
{
	if (row < 0 || std::size_t(row) >= rows_.size()) return {};
	const model::Diagnostic& d = store_.at(rows_[std::size_t(row)]);
	switch (col)
	{
	case 0: return std::format("{} {}", model::severityIcon(d.severity), model::severityLabel(d.severity));
	case 1: return d.source;
	case 2: return d.message;
	case 3: return d.address ? model::hexAddress(d.address) : std::string();
	case 4: return d.function;
	default: return {};
	}
}

void DiagnosticsPanel::refilter()
{
	rows_ = store_.visibleRows(filter_, text_);
	if (sortCol_ >= 0)
	{
		const int col = sortCol_;
		const bool asc = sortAsc_;
		std::stable_sort(rows_.begin(), rows_.end(), [&](std::size_t a, std::size_t b) {
			const model::Diagnostic& x = store_.at(a);
			const model::Diagnostic& y = store_.at(b);
			auto less = [&](const model::Diagnostic& p, const model::Diagnostic& q) {
				switch (col)
				{
				case 0: return p.severity < q.severity;
				case 1: return p.source < q.source;
				case 2: return p.message < q.message;
				case 3: return p.address < q.address;
				default: return p.function < q.function;
				}
			};
			return asc ? less(x, y) : less(y, x);
		});
	}
	w::tableModelChanged(ui_, table_);
	if (int(ui_.value(table_)) >= int(rows_.size())) ui_.setValue(table_, -1.f);
	updateCount();
}

void DiagnosticsPanel::updateCount()
{
	ui_.setText(count_, std::format("{} entries", rows_.size()));
}

std::string DiagnosticsPanel::countText() const
{
	return std::string(ui_.text(count_));
}

void DiagnosticsPanel::addDiagnostic(model::Diagnostic d)
{
	const bool isError = d.severity == model::Severity::Error;
	const bool evicts = store_.size() >= store_.maxEntries();
	store_.add(std::move(d));
	if (evicts || sortCol_ >= 0)
		refilter();
	else
	{
		const std::size_t idx = store_.size() - 1;
		if (store_.matches(idx, filter_, text_)) rows_.push_back(idx);
		w::tableModelChanged(ui_, table_);
		updateCount();
	}
	if (isError && onErrorAdded) onErrorAdded();
}

void DiagnosticsPanel::addMessage(model::Severity severity, std::string_view stage, std::string_view message, uint64_t address)
{
	model::Diagnostic d;
	d.severity = severity;
	d.source = std::string(stage);
	d.message = std::string(message);
	d.address = address;
	addDiagnostic(std::move(d));
}

void DiagnosticsPanel::setDiagnostics(std::vector<model::Diagnostic> all)
{
	store_.clear();
	bool anyError = false;
	for (auto& d: all)
	{
		anyError = anyError || d.severity == model::Severity::Error;
		store_.add(std::move(d));
	}
	refilter();
	if (anyError && onErrorAdded) onErrorAdded();
}

void DiagnosticsPanel::clear()
{
	store_.clear();
	ui_.setValue(table_, -1.f);
	refilter();
}

void DiagnosticsPanel::setMaxEntries(int n)
{
	store_.setMaxEntries(std::size_t(std::max(0, n)));
	refilter();
}

void DiagnosticsPanel::setSeverityFilter(model::SeverityFilter f)
{
	filter_ = f;
	ui_.setValue(combo_, float(int(f)), false);
	refilter();
}

void DiagnosticsPanel::setTextFilter(std::string_view text)
{
	text_ = std::string(text);
	setLineEditText(ui_, search_, text);
	refilter();
}

std::string DiagnosticsPanel::visibleTsv() const
{
	return store_.tsv(rows_);
}

void DiagnosticsPanel::copyVisible()
{
	ui_.setClipboardText(visibleTsv());
}

bool DiagnosticsPanel::saveVisibleTo(const std::filesystem::path& path)
{
	return model::saveTextFile(path, visibleTsv()).has_value();
}

void DiagnosticsPanel::saveVisible()
{
	if (!ui_.alive(saveDlg_))
	{
		std::error_code ec;
		saveDlg_ = w::fileDialog(ui_, "Save diagnostics", std::filesystem::current_path(ec).string(), "*.tsv;*.txt", true);
		auto* self = this;
		ui_.connect(saveDlg_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs& a) {
			if (!a.text.empty()) self->saveVisibleTo(std::filesystem::path(std::string(a.text)));
		});
	}
	w::openDialog(ui_, saveDlg_);
}

void DiagnosticsPanel::activateRow(int row)
{
	if (row < 0 || std::size_t(row) >= rows_.size()) return;
	const model::Diagnostic& d = store_.at(rows_[std::size_t(row)]);
	if (d.address != 0)
	{
		if (onAddressActivated) onAddressActivated(d.address);
	}
	else if (!d.function.empty() && onFunctionActivated)
		onFunctionActivated(d.function);
}

} // namespace retdec::gui::ui
