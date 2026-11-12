/**
 * @file src/gui/ui/ir_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/ir_panel.h"

#include "retdec/gui/model/code_address.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

IRPanel::IRPanel(UiContext& ui, NodeHandle parent): ui_(ui), menu_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-ir");

	const NodeHandle top = toolbarRow(ui, root_);
	funcLabel_ = w::label(ui, top, "No function selected");
	ui.addClass(funcLabel_, "retdec-muted");
	ui.setCss(funcLabel_, "flex-grow", "1");
	stageLabel_ = w::label(ui, top, "Stage:");
	stageCombo_ = w::comboBox(ui, top, "Function\nModule", 0);
	w::setTooltip(ui, stageCombo_, "Function IR for the selection, or the full module .ll sidecar");

	body_ = w::panel(ui, root_);
	ui.setCss(body_, "flex-grow", "1");
	ui.setCss(body_, "min-height", "0px");
	ui.setCss(body_, "gap", "0px");
	empty_ = std::make_unique<EmptyState>(ui, body_);
	empty_->setTitle("No IR loaded");
	empty_->setHint("Select a function after decompilation to inspect its SSA IR.");
	view_ = std::make_unique<CodeView>(ui, body_, model::CodeLang::IR);

	findBar_ = w::panel(ui, root_);
	ui.addClass(findBar_, "retdec-findbar");
	ui.setCss(findBar_, "flex-direction", "row");
	ui.setCss(findBar_, "flex-shrink", "0");
	findEdit_ = w::lineEdit(ui, findBar_, "");
	ui.setCss(findEdit_, "flex-grow", "1");
	w::setPlaceholderText(ui, findEdit_, "Find in IR\xE2\x80\xA6");
	setShown(ui, findBar_, false);

	auto* self = this;
	ui.connect(stageCombo_, SignalKind::ValueChanged, [self](UiContext&, NodeHandle, const SignalArgs& a) { self->onStageChanged(int(a.value)); });
	ui.connect(findEdit_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs&) { self->findNext(); });
	view_->onContextMenu = [self](Vec2 at, int line) {
		const auto addr = line >= 0 ? model::parseFirstAddress(self->view_->line(line)) : std::nullopt;
		self->menu_.open(self->root_, at, addressMenuActions(self->ui_, addr, [self](uint64_t a) {
			                 if (self->onAddressActivated) self->onAddressActivated(a);
		                 }));
	};
	updateEmptyState();
}

IRPanel::~IRPanel()
{
	menu_.close();
	view_.reset();
	empty_.reset();
	releasePanelRoot(ui_, root_);
}

bool IRPanel::handleEvent(EventContext& ec, const Event& e)
{
	if (e.type != EventType::KeyDown) return false;
	if (ec.phase == Phase::Capture)
	{
		if (isKey(e, Key::F, kModCtrl))
		{
			showFindBar();
			return true;
		}
		if (isKey(e, Key::F3))
		{
			findNext();
			return true;
		}
		if (isKey(e, Key::F3, kModShift))
		{
			findPrevious();
			return true;
		}
		return false;
	}
	if (ec.phase == Phase::Bubble && isKey(e, Key::Escape) && findShown_)
	{
		hideFindBar();
		return true;
	}
	return false;
}

std::string IRPanel::functionLabel() const
{
	return std::string(ui_.text(funcLabel_));
}

void IRPanel::updateEmptyState()
{
	emptyShown_ = view_->isEmpty();
	setShown(ui_, empty_->root(), emptyShown_);
	setShown(ui_, view_->node(), !emptyShown_);
}

void IRPanel::setIRText(std::string_view text, std::string_view stage)
{
	if (stage == "Module") moduleIr_ = std::string(text);
	else functionIr_ = std::string(text);
	view_->setText(text);
	if (stage == "Function") stage_ = Stage::Function;
	else if (stage == "Module") stage_ = Stage::Module;
	if (stage == "Function" || stage == "Module") ui_.setValue(stageCombo_, float(int(stage_)), false);
	updateEmptyState();
}

void IRPanel::setStage(Stage s)
{
	ui_.setValue(stageCombo_, float(int(s)), false);
	onStageChanged(int(s));
}

void IRPanel::onStageChanged(int index)
{
	stage_ = index == 1 ? Stage::Module : Stage::Function;
	const std::string& chosen = stage_ == Stage::Module ? moduleIr_ : functionIr_;
	if (!chosen.empty()) view_->setText(chosen);
	updateEmptyState();
	if (onStatusMessage) onStatusMessage(std::format("IR stage: {}", stage_ == Stage::Module ? "Module" : "Function"), 3000);
}

void IRPanel::onFunctionSelected(uint64_t address, std::string_view name)
{
	ui_.setText(funcLabel_, std::format("{} @ 0x{:x}", name, address));
}

void IRPanel::applyEditorFont(float px)
{
	view_->setFontSize(px);
}

void IRPanel::clear()
{
	view_->clear();
	ui_.setText(funcLabel_, "No function selected");
	functionIr_.clear();
	moduleIr_.clear();
	hideFindBar();
	updateEmptyState();
}

void IRPanel::showFindBar()
{
	findShown_ = true;
	setShown(ui_, findBar_, true);
	ui_.setFocus(findEdit_, FocusReason::Keyboard);
	ui_.editBuffer(findEdit_).selectAll();
}

void IRPanel::hideFindBar()
{
	findShown_ = false;
	setShown(ui_, findBar_, false);
}

void IRPanel::setFindText(std::string_view text)
{
	setLineEditText(ui_, findEdit_, text);
}

void IRPanel::findNext()
{
	const std::string q = ui_.editBuffer(findEdit_).text();
	if (q.empty())
	{
		showFindBar();
		return;
	}
	view_->find(q, true);
}

void IRPanel::findPrevious()
{
	const std::string q = ui_.editBuffer(findEdit_).text();
	if (!q.empty()) view_->find(q, false);
}

} // namespace retdec::gui::ui
