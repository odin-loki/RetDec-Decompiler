/**
 * @file src/gui/ui/binary_browser_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/binary_browser_panel.h"

#include "retdec/gui/model/code_address.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
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

constexpr uint64_t kMaxHexBytes = 64 * 1024;

} // namespace

BinaryBrowserPanel::BinaryBrowserPanel(UiContext& ui, NodeHandle parent): ui_(ui), menu_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-binary-browser");

	const NodeHandle split = w::splitter(ui, root_, true);
	ui.setCss(split, "flex-grow", "1");
	ui.setCss(split, "min-height", "0px");
	const NodeHandle top = w::splitterPane(ui, split);
	const NodeHandle bottom = w::splitterPane(ui, split);
	ui.setCss(bottom, "gap", "4px");

	model_.user = this;
	model_.columns = 3;
	model_.rows = [](const void* u) { return u32(static_cast<const BinaryBrowserPanel*>(u)->rows_.size()); };
	model_.data = [](const void* u, u32 row, u32 col, std::string& scratch) -> std::string_view {
		scratch = static_cast<const BinaryBrowserPanel*>(u)->cellText(int(row), int(col));
		return scratch;
	};
	model_.header = [](const void*, u32 col) -> std::string_view {
		static constexpr std::string_view kHeaders[] = {"Name", "Address", "Size"};
		return col < 3 ? kHeaders[col] : std::string_view{};
	};
	table_ = w::tableView(ui, top, &model_);
	ui.setCss(table_, "flex-grow", "1");
	ui.setCss(table_, "min-height", "0px");
	w::setTableColumnWidth(ui, table_, 0, 260.f);
	w::setTableColumnWidth(ui, table_, 1, 140.f);
	w::setTableColumnWidth(ui, table_, 2, 110.f);

	hexHeader_ = w::label(ui, bottom, "Hex View");
	ui.addClass(hexHeader_, "retdec-muted");
	hex_ = std::make_unique<CodeView>(ui, bottom, model::CodeLang::Plain);
	hex_->setShowGutter(false);
	hex_->setPlaceholder("Select a section to view its hex dump\xE2\x80\xA6");

	auto* self = this;
	ui.connect(table_, SignalKind::ValueChanged, [self](UiContext&, NodeHandle, const SignalArgs& a) { self->showHexForRow(int(a.value)); });
	ui.connect(table_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs& a) { self->activateRow(int(a.value)); });
}

BinaryBrowserPanel::~BinaryBrowserPanel()
{
	if (menuPending_) ui_.cancelTimer(menuTimer_);
	menu_.close();
	hex_.reset();
	releasePanelRoot(ui_, root_);
}

bool BinaryBrowserPanel::handleEvent(EventContext& ec, const Event& e)
{
	if (ec.phase != Phase::Capture || e.type != EventType::PointerDown || ec.target != table_) return false;
	if (e.button == MouseButton::Right)
	{
		// Let the table select the row under the pointer, then open the menu for it.
		menuPos_ = e.pos;
		if (!menuPending_)
		{
			menuPending_ = true;
			auto* self = this;
			menuTimer_ = ui_.startTimer(0.f, false, [self](UiContext& c) {
				self->menuPending_ = false;
				self->openContextMenu(self->menuPos_, int(c.value(self->table_)));
			});
		}
		return false;
	}
	if (e.button != MouseButton::Left) return false;
	const double now = nowSeconds();
	const bool dbl = now - lastDown_ < 0.45 && std::abs(e.pos.x - lastDownPos_.x) < 5.f && std::abs(e.pos.y - lastDownPos_.y) < 5.f;
	lastDown_ = dbl ? -1.0 : now;
	lastDownPos_ = e.pos;
	if (dbl)
	{
		activateRow(int(ui_.value(table_)));
		return true;
	}
	return false;
}

std::string BinaryBrowserPanel::cellText(int row, int col) const
{
	if (row < 0 || std::size_t(row) >= rows_.size()) return {};
	const Row& r = rows_[std::size_t(row)];
	switch (col)
	{
	case 0: return r.section ? "    " + r.name : r.name;
	case 1: return r.address;
	case 2: return r.size;
	default: return {};
	}
}

std::string BinaryBrowserPanel::hexHeaderText() const
{
	return std::string(ui_.text(hexHeader_));
}

void BinaryBrowserPanel::setRows(std::vector<Row> rows)
{
	rows_ = std::move(rows);
	ui_.setValue(table_, -1.f);
	w::tableModelChanged(ui_, table_);
}

void BinaryBrowserPanel::loadBinary(std::string_view path)
{
	std::error_code ec;
	binaryPath_ = std::filesystem::absolute(std::filesystem::path(std::string(path)), ec).string();
	if (ec) binaryPath_ = std::string(path);
	hex_->clear();
	ui_.setText(hexHeader_, "Hex View");
	setRows({Row{"Run Inspect or wait for fileinfo\xE2\x80\xA6", "", "", 0, false}});
}

void BinaryBrowserPanel::populateFromFileinfo(std::string_view fileinfoJson)
{
	if (binaryPath_.empty()) return;
	auto info = model::parseFileinfoJson(fileinfoJson);
	setSections(info ? info->sections : std::vector<model::SectionInfo>{});
}

void BinaryBrowserPanel::setSections(const std::vector<model::SectionInfo>& sections)
{
	if (binaryPath_.empty()) return;
	hex_->clear();
	ui_.setText(hexHeader_, "Hex View");
	if (sections.empty())
	{
		setRows({Row{"(no sections in fileinfo)", "", "", 0, false}});
		return;
	}
	std::vector<Row> rows;
	rows.push_back(Row{std::filesystem::path(binaryPath_).filename().string(), "", "", 0, false});
	for (const auto& s: sections)
		rows.push_back(Row{s.name, s.address, s.sizeInFile, s.offset, true});
	setRows(std::move(rows));
}

void BinaryBrowserPanel::clear()
{
	binaryPath_.clear();
	setRows({});
	hex_->clear();
	ui_.setText(hexHeader_, "Hex View");
}

void BinaryBrowserPanel::selectRow(int row)
{
	ui_.setValue(table_, float(row), false);
	showHexForRow(row);
}

void BinaryBrowserPanel::showHexForRow(int row)
{
	hex_->clear();
	if (row < 0 || std::size_t(row) >= rows_.size() || binaryPath_.empty()) return;
	const Row& r = rows_[std::size_t(row)];
	if (!r.section) return;
	const uint64_t size = model::parseHexU64(r.size);
	if (size == 0) return;
	const uint64_t readSize = std::min(size, kMaxHexBytes);
	auto bytes = model::readFileRange(binaryPath_, r.offset, readSize);
	if (!bytes)
	{
		hex_->setText(bytes.error());
		return;
	}
	ui_.setText(hexHeader_, std::format("Hex View \xE2\x80\x94 {} @ file 0x{:x} ({} bytes)", r.name, r.offset, size));
	std::string dump = model::formatHexDump(*bytes, r.offset);
	if (size > kMaxHexBytes)
		dump += std::format("\n\xE2\x80\xA6 [showing first {} KiB of {} KiB]", kMaxHexBytes / 1024, (size + 1023) / 1024);
	hex_->setText(dump);
}

void BinaryBrowserPanel::activateRow(int row)
{
	if (row < 0 || std::size_t(row) >= rows_.size()) return;
	const uint64_t addr = model::parseHexU64(rows_[std::size_t(row)].address);
	if (addr != 0 && onAddressActivated) onAddressActivated(addr);
}

void BinaryBrowserPanel::openContextMenu(Vec2 at, int row)
{
	if (row < 0 || std::size_t(row) >= rows_.size()) return;
	const Row r = rows_[std::size_t(row)];
	const uint64_t start = model::parseHexU64(r.address);
	const uint64_t size = model::parseHexU64(r.size);
	if (start == 0 || size == 0) return;
	const uint64_t end = start + size;
	const std::string range = std::format("0x{:x}-0x{:x}", start, end);
	auto* self = this;
	std::vector<MenuAction> actions;
	actions.push_back({"Decompile this range\xE2\x80\xA6", true, [self, range] {
		                   if (self->onDecompileRangeRequested) self->onDecompileRangeRequested(range);
	                   }});
	actions.push_back({"Copy start address", true, [self, start] { self->ui_.setClipboardText(model::formatAddressHex(start)); }});
	actions.push_back({"Copy VMA range", true, [self, range] { self->ui_.setClipboardText(range); }});
	actions.push_back({"Copy hex snippet", true, [self, r, size] {
		                   auto bytes = model::readFileRange(self->binaryPath_, r.offset, std::min<uint64_t>(size, 64));
		                   if (bytes && !bytes->empty()) self->ui_.setClipboardText(model::hexSnippet(*bytes));
	                   }});
	menu_.open(root_, at, std::move(actions));
}

} // namespace retdec::gui::ui
