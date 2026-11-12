/**
 * @file src/gui/ui/decompiled_c_panel.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/decompiled_c_panel.h"

#include "retdec/gui/model/code_address.h"
#include "retdec/gui/model/code_text.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <filesystem>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {
std::string trimmed(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
		s.remove_suffix(1);
	return std::string(s);
}
} // namespace

DecompiledCPanel::DecompiledCPanel(UiContext& ui, NodeHandle parent): ui_(ui), menu_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-decompiled-c");

	const NodeHandle tb = toolbarRow(ui, root_);
	copyBtn_ = w::toolButton(ui, tb, "Copy");
	saveBtn_ = w::toolButton(ui, tb, "Save As\xE2\x80\xA6");
	findBtn_ = w::toolButton(ui, tb, "Find");
	w::setTooltip(ui, findBtn_, "Find in decompiled C (Ctrl+F)");
	refinedChk_ = w::checkBox(ui, tb, "Refined", false);
	ui.setDisabled(refinedChk_, true);
	w::setTooltip(ui, refinedChk_, "Show accepted neural refine sidecar (.refined.c) when present");

	funcLabel_ = w::label(ui, root_, "No function selected");
	ui.addClass(funcLabel_, "retdec-muted");

	body_ = w::panel(ui, root_);
	ui.setCss(body_, "flex-grow", "1");
	ui.setCss(body_, "min-height", "0px");
	ui.setCss(body_, "gap", "0px");
	empty_ = std::make_unique<EmptyState>(ui, body_);
	empty_->setTitle("No decompiled output yet");
	empty_->setHint("Run Analysis > Run Full Analysis (F5) to decompile the binary.");
	view_ = std::make_unique<CodeView>(ui, body_, model::CodeLang::C);

	const NodeHandle fb = w::panel(ui, root_);
	ui.addClass(fb, "retdec-findbar");
	ui.setCss(fb, "flex-direction", "row");
	ui.setCss(fb, "flex-shrink", "0");
	findEdit_ = w::lineEdit(ui, fb, "");
	ui.setCss(findEdit_, "flex-grow", "1");
	w::setTooltip(ui, findEdit_, "Find in decompiled C\xE2\x80\xA6");
	setShown(ui, fb, false);

	auto* self = this;
	ui.connect(copyBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->copyToClipboard(); });
	ui.connect(saveBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->saveAs(); });
	ui.connect(findBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->showFindBar(); });
	ui.connect(refinedChk_, SignalKind::Toggled, [self](UiContext& c, NodeHandle n, const SignalArgs&) { self->setShowRefined(c.checked(n)); });
	ui.connect(findEdit_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs&) { self->submitFind(); });
	view_->onContextMenu = [self](Vec2 at, int) { self->openContextMenu(at); };

	updateEmptyState();
}

DecompiledCPanel::~DecompiledCPanel()
{
	menu_.close();
	if (ui_.alive(saveDlg_)) ui_.destroy(saveDlg_);
	view_.reset();
	empty_.reset();
	releasePanelRoot(ui_, root_);
}

bool DecompiledCPanel::handleEvent(EventContext& ec, const Event& e)
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

void DecompiledCPanel::status(const std::string& msg, int ms)
{
	if (onStatusMessage) onStatusMessage(msg, ms);
}

void DecompiledCPanel::updateEmptyState()
{
	emptyShown_ = view_->isEmpty();
	setShown(ui_, empty_->root(), emptyShown_);
	setShown(ui_, view_->node(), !emptyShown_);
}

void DecompiledCPanel::display(std::string_view text)
{
	view_->setText(text);
	updateEmptyState();
	// The Qt panel streamed > 2 MiB sources in chunks and reported the size.
	constexpr std::size_t kAsyncThreshold = 2 * 1024 * 1024;
	if (text.size() > kAsyncThreshold) ui_.setText(funcLabel_, std::format("Loaded {} KiB", text.size() / 1024));
}

void DecompiledCPanel::setSource(std::string_view cSource)
{
	original_ = std::string(cSource);
	showingRefined_ = false;
	ui_.setChecked(refinedChk_, false);
	display(original_);
}

void DecompiledCPanel::setRefinedSource(std::string_view refined)
{
	refined_ = std::string(refined);
	ui_.setDisabled(refinedChk_, refined_.empty());
	if (refined_.empty() && showingRefined_) setShowRefined(false);
}

void DecompiledCPanel::setShowRefined(bool on)
{
	if (on && refined_.empty()) on = false;
	ui_.setChecked(refinedChk_, on);
	if (on == showingRefined_) return;
	showingRefined_ = on;
	display(on ? refined_ : original_);
}

void DecompiledCPanel::setLanguage(model::CodeLang lang)
{
	view_->setLang(lang);
}

void DecompiledCPanel::applyEditorFont(float px)
{
	view_->setFontSize(px);
}

void DecompiledCPanel::clear()
{
	original_.clear();
	refined_.clear();
	showingRefined_ = false;
	ui_.setChecked(refinedChk_, false);
	ui_.setDisabled(refinedChk_, true);
	view_->clear();
	ui_.setText(funcLabel_, "No function selected");
	lastFindQuery_.clear();
	hideFindBar();
	updateEmptyState();
}

std::string DecompiledCPanel::documentText() const
{
	return view_->text();
}

std::string DecompiledCPanel::functionLabel() const
{
	return std::string(ui_.text(funcLabel_));
}

void DecompiledCPanel::onFunctionSelected(uint64_t address, std::string_view name)
{
	ui_.setText(funcLabel_, std::format("{} @ 0x{:x}", name, address));
}

void DecompiledCPanel::scrollToFunction(std::string_view name, int startLine, int endLine)
{
	(void)endLine;
	if (startLine > 0 && startLine <= view_->lineCount())
	{
		view_->setCaret({startLine - 1, 0});
		view_->centerOnLine(startLine - 1);
		return;
	}
	if (name.empty()) return;
	if (auto line = model::findFunctionLine(view_->lines(), name))
	{
		const std::string_view L = view_->line(*line);
		const std::size_t col = L.find(name);
		view_->setCaret({*line, col == std::string_view::npos ? 0 : int(col)});
		view_->centerOnLine(*line);
	}
}

void DecompiledCPanel::scrollToFunction(uint64_t address, std::string_view name)
{
	if (view_->isEmpty()) return;
	if (auto p = model::locateFunction(view_->lines(), address, name))
	{
		view_->setCaret(*p);
		view_->centerOnLine(p->line);
	}
}

void DecompiledCPanel::showFindBar()
{
	findShown_ = true;
	setShown(ui_, ui_.parent(findEdit_), true);
	ui_.setFocus(findEdit_, FocusReason::Keyboard);
	ui_.editBuffer(findEdit_).selectAll();
}

void DecompiledCPanel::hideFindBar()
{
	findShown_ = false;
	setShown(ui_, ui_.parent(findEdit_), false);
	if (ui_.alive(view_->node())) ui_.setFocus(view_->node(), FocusReason::Programmatic);
}

void DecompiledCPanel::setFindText(std::string_view text)
{
	setLineEditText(ui_, findEdit_, text);
}

void DecompiledCPanel::submitFind()
{
	const std::string query = trimmed(ui_.editBuffer(findEdit_).text());
	if (query.empty()) return;
	if (view_->isEmpty())
	{
		status("No decompiled output to search", 2000);
		return;
	}
	lastFindQuery_ = query;
	view_->find(query, true);
}

void DecompiledCPanel::findNext()
{
	const std::string query = findShown_ ? trimmed(ui_.editBuffer(findEdit_).text()) : lastFindQuery_;
	if (query.empty())
	{
		showFindBar();
		return;
	}
	if (view_->isEmpty()) return;
	lastFindQuery_ = query;
	view_->find(query, true);
}

void DecompiledCPanel::findPrevious()
{
	const std::string query = findShown_ ? trimmed(ui_.editBuffer(findEdit_).text()) : lastFindQuery_;
	if (query.empty() || view_->isEmpty()) return;
	lastFindQuery_ = query;
	view_->find(query, false);
}

void DecompiledCPanel::copyToClipboard()
{
	ui_.setClipboardText(view_->text());
	status("Copied to clipboard", 2000);
}

bool DecompiledCPanel::saveTo(const std::filesystem::path& path)
{
	auto r = model::saveTextFile(path, view_->text());
	if (!r)
	{
		status("Save failed: " + r.error(), 4000);
		return false;
	}
	status("Saved: " + path.string(), 3000);
	return true;
}

void DecompiledCPanel::saveAs()
{
	if (!ui_.alive(saveDlg_))
	{
		std::error_code ec;
		const std::string dir = std::filesystem::current_path(ec).string();
		saveDlg_ = w::fileDialog(ui_, "Save Decompiled C", dir, "*.c", true);
		auto* self = this;
		ui_.connect(saveDlg_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs& a) {
			if (!a.text.empty()) self->saveTo(std::filesystem::path(std::string(a.text)));
		});
	}
	w::openDialog(ui_, saveDlg_);
}

void DecompiledCPanel::openContextMenu(Vec2 at)
{
	const std::optional<uint64_t> addr = model::parseFirstAddress(view_->line(view_->caret().line));
	std::vector<MenuAction> actions;
	auto* self = this;
	actions.push_back({"Copy", !view_->selectedText().empty(), [self] { self->view_->copySelection(); }});
	actions.push_back({"Select All", !view_->isEmpty(), [self] { self->view_->selectAll(); }});
	actions.push_back({"Find\xE2\x80\xA6", true, [self] { self->showFindBar(); }});
	actions.push_back({"-", true, {}});
	for (MenuAction& a: addressMenuActions(ui_, addr, [self](uint64_t a) {
		     if (self->onAddressActivated) self->onAddressActivated(a);
	     }))
		actions.push_back(std::move(a));
	menu_.open(root_, at, std::move(actions));
}

} // namespace retdec::gui::ui
