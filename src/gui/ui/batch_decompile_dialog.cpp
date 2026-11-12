/**
 * @file src/gui/ui/batch_decompile_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/batch_decompile_dialog.h"

#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/model/dlg_target.h"
#include "retdec/gui/ui/empty_state.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

BatchDecompileDialog::BatchDecompileDialog(UiContext& ui)
	: dlg_(ui, "Batch Decompile", 560.f, 360.f, "batchDecompileDialog"), files_(ui)
{
	const NodeHandle b = dlg_.body();
	w::label(ui, b, "Binaries are decompiled sequentially using the same settings as Run Full Analysis.");
	list_ = w::listView(ui, b, "", -1);
	ui.setCss(list_, "flex-grow", "1");
	hint_ = dlg::noteLabel(ui, b, "Add at least one binary to enable Start.");
	const NodeHandle row = dlg::hrow(ui, b);
	add_ = w::button(ui, row, "Add…");
	remove_ = w::button(ui, row, "Remove");
	clear_ = w::button(ui, row, "Clear");
	const NodeHandle buttons = dlg::buttonRow(ui, b);
	start_ = w::button(ui, buttons, "Start");
	ui.addClass(start_, "primary");
	cancel_ = w::button(ui, buttons, "Cancel");

	ui.connect(add_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		files_.show(FilePrompt::Mode::Open, "Add binaries", startDir_, {},
		            [this](const std::string& path) { addPaths({path}); });
	});
	ui.connect(remove_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { removeSelected(); });
	ui.connect(clear_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { clear(); });
	ui.connect(start_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { start(); });
	ui.connect(cancel_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { reject(); });
	refresh();
}

void BatchDecompileDialog::show(std::function<void(const std::vector<std::string>&)> onStart, std::string_view startDir)
{
	onStart_ = std::move(onStart);
	startDir_ = model::fileDialogStartDir(startDir);
	paths_.clear();
	refresh();
	dlg_.open();
}

void BatchDecompileDialog::start()
{
	if (!startEnabled()) return;
	const std::vector<std::string> paths = paths_;
	dlg_.close();
	if (onStart_) onStart_(paths);
}

void BatchDecompileDialog::reject()
{
	dlg_.close();
}

void BatchDecompileDialog::addPaths(const std::vector<std::string>& paths)
{
	model::batchAddPaths(paths_, paths);
	refresh();
}

void BatchDecompileDialog::removeSelected()
{
	const int row = selectedRow();
	if (row < 0) return;
	model::batchRemoveIndices(paths_, {row});
	refresh();
}

void BatchDecompileDialog::clear()
{
	paths_.clear();
	refresh();
}

void BatchDecompileDialog::setSelectedRow(int row)
{
	dlg_.ui().setValue(list_, float(row), true);
}

int BatchDecompileDialog::selectedRow() const
{
	const int r = dlg::indexOf(dlg_.ui(), list_);
	return (r >= 0 && std::size_t(r) < paths_.size()) ? r : -1;
}

bool BatchDecompileDialog::startEnabled() const
{
	return !dlg_.ui().disabled(start_);
}

void BatchDecompileDialog::refresh()
{
	UiContext& ui = dlg_.ui();
	ui.setText(list_, dlg::joinLines(paths_));
	ui.setValue(list_, -1.f);
	const bool hasItems = !paths_.empty();
	ui.setDisabled(start_, !hasItems);
	hintShown_ = !hasItems;
	setShown(ui, hint_, hintShown_);
}

} // namespace retdec::gui::ui
