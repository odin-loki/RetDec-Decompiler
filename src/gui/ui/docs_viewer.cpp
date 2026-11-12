/**
 * @file src/gui/ui/docs_viewer.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/docs_viewer.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

DocsViewer::DocsViewer(UiContext& ui): dlg_(ui, "Documentation", 960.f, 640.f, "docsViewer"), message_(ui)
{
	const NodeHandle split = w::splitter(ui, dlg_.body(), false);
	ui.setCss(split, "flex-grow", "1");
	ui.setCss(split, "min-height", "0px");
	const NodeHandle left = w::splitterPane(ui, split, 240.f);
	const NodeHandle right = w::splitterPane(ui, split);
	list_ = w::listView(ui, left, "", -1, "docsList");
	ui.setCss(list_, "flex-grow", "1");
	scroll_ = w::scrollArea(ui, right);
	ui.setCss(scroll_, "flex-grow", "1");
	ui.setCss(scroll_, "padding", "4px 10px");
	view_ = w::markdownView(ui, scroll_, "", "docsMarkdown");
	pathLabel_ = dlg::noteLabel(ui, dlg_.body(), "");
	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	close_ = w::button(ui, buttons, "Close");
	ui.connect(close_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { close(); });
	ui.connect(list_, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (int(a.value) != current_) showDoc(int(a.value));
	});
}

bool DocsViewer::show(std::function<void()> onClosed)
{
	UiContext& ui = dlg_.ui();
	dir_ = overrideDir_.empty() ? model::findDocsDir() : overrideDir_;
	docs_ = model::listDocs(dir_);
	if (docs_.empty())
	{
		message_.show("Documentation", model::kNoDocsMessage, MessageDialog::Kind::Information, std::move(onClosed));
		return false;
	}
	onClosed_ = std::move(onClosed);
	std::vector<std::string> titles;
	titles.reserve(docs_.size());
	for (const model::DocEntry& d: docs_)
		titles.push_back(d.title);
	dlg::setListItems(ui, list_, titles);
	current_ = -1;
	dlg_.open();
	showDoc(model::defaultDocIndex(docs_));
	ui.setFocus(list_, FocusReason::Keyboard);
	return true;
}

void DocsViewer::close()
{
	dlg_.close();
	if (auto cb = std::move(onClosed_))
	{
		onClosed_ = nullptr;
		cb();
	}
}

void DocsViewer::showDoc(int index)
{
	if (index < 0 || std::size_t(index) >= docs_.size()) return;
	UiContext& ui = dlg_.ui();
	current_ = index;
	const model::DocEntry& d = docs_[std::size_t(index)];
	auto text = model::readDoc(d.path);
	markdown_ = text ? std::move(*text) : "# " + d.title + "\n\n" + text.error();
	w::setMarkdown(ui, view_, markdown_);
	ui.scrollTo(scroll_, {0.f, 0.f});
	ui.setText(pathLabel_, d.path.generic_string());
	ui.setValue(list_, float(index), false);
}

} // namespace retdec::gui::ui
