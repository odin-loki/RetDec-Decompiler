/**
 * @file src/gui/ui/raw_options_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/raw_options_dialog.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

NodeHandle field(UiContext& ui, NodeHandle body, std::string_view label, std::string_view hint, std::string_view id)
{
	w::label(ui, body, label);
	const NodeHandle e = w::lineEdit(ui, body, "", id);
	w::setPlaceholderText(ui, e, hint); // Qt: placeholderText
	return e;
}

int itemIndex(const std::vector<std::string>& items, const std::string& text)
{
	const auto it = std::find(items.begin(), items.end(), text);
	return it == items.end() ? 0 : int(it - items.begin());
}

} // namespace

RawOptionsDialog::RawOptionsDialog(UiContext& ui)
	: dlg_(ui, "Raw image / archive options", 420.f, 0.f, "rawArchiveOptionsDialog")
{
	const NodeHandle b = dlg_.body();
	w::label(ui, b, "Endian (-e)");
	endian_ = w::comboBox(ui, b, dlg::joinLines(model::rawEndianItems()), 0, "rawOptEndian");
	w::label(ui, b, "Bit size (-b)");
	bits_ = w::comboBox(ui, b, dlg::joinLines(model::rawBitSizeItems()), 0, "rawOptBitSize");
	vma_ = field(ui, b, "Raw section VMA", "0x100000 (empty = omit)", "rawOptVma");
	arName_ = field(ui, b, "Archive member (--ar-name)", "member.o (empty = omit)", "rawOptArName");
	arIndex_ = field(ui, b, "Archive member index (--ar-index)", "0 (empty = omit; ignored if name is set)",
	                 "rawOptArIndex");
	const NodeHandle buttons = dlg::buttonRow(ui, b);
	ok_ = w::button(ui, buttons, "OK");
	ui.addClass(ok_, "primary");
	cancel_ = w::button(ui, buttons, "Cancel");
	ui.connect(ok_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { accept(); });
	ui.connect(cancel_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { reject(); });
}

model::RawArchiveForm RawOptionsDialog::form() const
{
	const UiContext& ui = dlg_.ui();
	model::RawArchiveForm f;
	f.endianText = std::string(w::comboText(ui, endian_));
	f.bitSizeText = std::string(w::comboText(ui, bits_));
	f.vmaText = dlg::editText(ui, vma_);
	f.arNameText = dlg::editText(ui, arName_);
	f.arIndexText = dlg::editText(ui, arIndex_);
	return f;
}

void RawOptionsDialog::load(const model::RawArchiveOptions& o)
{
	UiContext& ui = dlg_.ui();
	const model::RawArchiveForm f = model::rawArchiveFormFromOptions(o);
	dlg::setIndex(ui, endian_, itemIndex(model::rawEndianItems(), f.endianText));
	dlg::setIndex(ui, bits_, itemIndex(model::rawBitSizeItems(), f.bitSizeText));
	dlg::setEditText(ui, vma_, f.vmaText);
	dlg::setEditText(ui, arName_, f.arNameText);
	dlg::setEditText(ui, arIndex_, f.arIndexText);
}

void RawOptionsDialog::show(const model::RawArchiveOptions& current,
                            std::function<void(const model::RawArchiveOptions&)> onAccepted)
{
	onAccepted_ = std::move(onAccepted);
	load(current);
	dlg_.open();
}

void RawOptionsDialog::accept()
{
	const model::RawArchiveOptions o = options();
	dlg_.close();
	if (onAccepted_) onAccepted_(o);
	if (onStatusMessage) onStatusMessage("Raw / archive options applied to the next decompile", 5000);
}

void RawOptionsDialog::reject()
{
	dlg_.close();
}

} // namespace retdec::gui::ui
