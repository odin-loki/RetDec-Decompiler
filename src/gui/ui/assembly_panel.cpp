/**
 * @file src/gui/ui/assembly_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/assembly_panel.h"

#include "retdec/gui/model/code_address.h"
#include "retdec/gui/model/code_text.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

AssemblyPanel::AssemblyPanel(UiContext& ui, NodeHandle parent): ui_(ui), menu_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-assembly");

	const NodeHandle tb = toolbarRow(ui, root_);
	goToBtn_ = w::toolButton(ui, tb, "Go to Address (G)");
	findBtn_ = w::toolButton(ui, tb, "Find (F)");

	funcLabel_ = w::label(ui, root_, "No function selected");
	ui.addClass(funcLabel_, "retdec-muted");

	body_ = w::panel(ui, root_);
	ui.setCss(body_, "flex-grow", "1");
	ui.setCss(body_, "min-height", "0px");
	ui.setCss(body_, "gap", "0px");
	empty_ = std::make_unique<EmptyState>(ui, body_);
	empty_->setTitle("No disassembly yet");
	empty_->setHint("Select a function in the Functions list to view its assembly.");
	view_ = std::make_unique<CodeView>(ui, body_, model::CodeLang::Asm);

	searchBar_ = w::panel(ui, root_);
	ui.addClass(searchBar_, "retdec-findbar");
	ui.setCss(searchBar_, "flex-direction", "row");
	ui.setCss(searchBar_, "flex-shrink", "0");
	// Mode label: the placeholder (Qt's hint) hides while the field has focus,
	// and the bar opens focused.
	searchLabel_ = w::label(ui, searchBar_, "Find:");
	ui.addClass(searchLabel_, "retdec-muted");
	searchEdit_ = w::lineEdit(ui, searchBar_, "");
	ui.setCss(searchEdit_, "flex-grow", "1");
	w::setPlaceholderText(ui, searchEdit_, "Find in disassembly\xE2\x80\xA6");
	setShown(ui, searchBar_, false);

	auto* self = this;
	ui.connect(goToBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) {
		self->goToAddress();
	});
	ui.connect(findBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->find(); });
	ui.connect(searchEdit_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs&) {
		self->submitSearch();
	});
	view_->onContextMenu = [self](Vec2 at, int line) { self->openContextMenu(at, line); };

	updateEmptyState();
}

AssemblyPanel::~AssemblyPanel()
{
	menu_.close();
	view_.reset();
	empty_.reset();
	releasePanelRoot(ui_, root_);
}

bool AssemblyPanel::handleEvent(EventContext& ec, const Event& e)
{
	if (e.type != EventType::KeyDown || ec.phase != Phase::Bubble) return false;
	const bool typing = isTextEntry(ui_, ec.target);
	if (!typing && isKey(e, Key::G))
	{
		goToAddress();
		return true;
	}
	if (!typing && isKey(e, Key::F))
	{
		find();
		return true;
	}
	if (ec.target == view_->node() && isKey(e, Key::Enter))
	{
		followJump();
		return true;
	}
	if (isKey(e, Key::Escape))
	{
		navigateBack();
		return true;
	}
	return false;
}

std::string AssemblyPanel::functionLabel() const
{
	return std::string(ui_.text(funcLabel_));
}

void AssemblyPanel::updateEmptyState()
{
	emptyShown_ = view_->isEmpty();
	setShown(ui_, empty_->root(), emptyShown_);
	setShown(ui_, view_->node(), !emptyShown_);
}

void AssemblyPanel::scrollToAddress(uint64_t address)
{
	if (view_->isEmpty()) return;
	if (auto line = model::findListingLine(view_->lines(), address))
	{
		view_->setCaret({*line, 0});
		view_->centerOnLine(*line);
	}
}

void AssemblyPanel::navigateTo(uint64_t address)
{
	pendingScrollAddr_ = address;
	ui_.setText(funcLabel_, std::format("Address: 0x{:x}", address));
	scrollToAddress(address);
}

void AssemblyPanel::setAssemblyText(std::string_view text)
{
	view_->setText(text);
	updateEmptyState();
	if (pendingScrollAddr_ != 0) scrollToAddress(pendingScrollAddr_);
}

void AssemblyPanel::applyEditorFont(float px)
{
	view_->setFontSize(px);
}

void AssemblyPanel::clear()
{
	view_->clear();
	ui_.setText(funcLabel_, "No function selected");
	pendingScrollAddr_ = 0;
	navStack_.clear();
	updateEmptyState();
}

void AssemblyPanel::showSearch(SearchMode mode)
{
	mode_ = mode;
	ui_.setText(searchLabel_, mode == SearchMode::GoTo ? "Go to:" : "Find:");
	w::setPlaceholderText(
		ui_,
		searchEdit_,
		mode == SearchMode::GoTo ? "Go to address (hex)\xE2\x80\xA6" : "Find in disassembly\xE2\x80\xA6");
	searchShown_ = true;
	setShown(ui_, searchBar_, true);
	setLineEditText(ui_, searchEdit_, "");
	ui_.setFocus(searchEdit_, FocusReason::Keyboard);
}

void AssemblyPanel::goToAddress()
{
	showSearch(SearchMode::GoTo);
}

void AssemblyPanel::find()
{
	showSearch(SearchMode::Find);
}

void AssemblyPanel::hideSearchBar()
{
	searchShown_ = false;
	setShown(ui_, searchBar_, false);
}

void AssemblyPanel::setSearchText(std::string_view text)
{
	setLineEditText(ui_, searchEdit_, text);
}

void AssemblyPanel::submitSearch()
{
	std::string query = ui_.editBuffer(searchEdit_).text();
	while (!query.empty() && (query.back() == ' ' || query.back() == '\t'))
		query.pop_back();
	while (!query.empty() && (query.front() == ' ' || query.front() == '\t'))
		query.erase(query.begin());
	if (query.empty()) return;
	if (mode_ == SearchMode::GoTo)
	{
		if (auto addr = model::parseAddressInput(query)) navigateTo(*addr);
		return;
	}
	view_->find(query, true);
}

bool AssemblyPanel::followJump()
{
	const std::string_view line = view_->line(view_->caret().line);
	const auto target = model::jumpTarget(line);
	if (!target) return false;
	if (const auto current = model::parseFirstAddress(line); current && *current != *target)
		navStack_.push_back(*current);
	navigateTo(*target);
	return true;
}

bool AssemblyPanel::navigateBack()
{
	if (navStack_.empty())
	{
		if (searchShown_) hideSearchBar();
		return false;
	}
	const uint64_t addr = navStack_.back();
	navStack_.pop_back();
	navigateTo(addr);
	return true;
}

void AssemblyPanel::openContextMenu(Vec2 at, int line)
{
	const auto addr = line >= 0 ? model::parseFirstAddress(view_->line(line)) : std::nullopt;
	auto* self = this;
	menu_.open(root_, at, addressMenuActions(ui_, addr, [self](uint64_t a) {
				   if (self->onAddressActivated) self->onAddressActivated(a);
			   }));
}

} // namespace retdec::gui::ui
