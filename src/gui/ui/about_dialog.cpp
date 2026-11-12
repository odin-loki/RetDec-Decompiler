/**
 * @file src/gui/ui/about_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/about_dialog.h"

#include "retdec/gui/model/dlg_help.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

AboutDialog::AboutDialog(UiContext& ui)
	: dlg_(ui, model::kAboutTitle, 560.f, 0.f, "aboutDialog"), markdown_(model::aboutMarkdown())
{
	view_ = w::markdownView(ui, dlg_.body(), markdown_);
	ui.setCss(view_, "flex-grow", "1");
	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	ok_ = w::button(ui, buttons, "OK");
	ui.addClass(ok_, "primary");
	ui.connect(ok_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { close(); });
}

void AboutDialog::show(std::function<void()> onClosed)
{
	onClosed_ = std::move(onClosed);
	dlg_.open();
	dlg_.ui().setFocus(ok_, FocusReason::Keyboard);
}

void AboutDialog::close()
{
	dlg_.close();
	if (auto cb = std::move(onClosed_))
	{
		onClosed_ = nullptr;
		cb();
	}
}

} // namespace retdec::gui::ui
