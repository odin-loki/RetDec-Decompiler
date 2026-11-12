/**
 * @file src/gui/ui/triage_banner.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/triage_banner.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <filesystem>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {
constexpr std::string_view kDash = "\xE2\x80\x94";     // —
constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // …
} // namespace

TriageBanner::TriageBanner(UiContext& ui, NodeHandle parent): ui_(ui)
{
	handler_.handle = [](EventContext&, const Event&) { return false; };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-banner");
	ui.setCss(root_, "flex-direction", "row");
	ui.setCss(root_, "flex-grow", "0");
	ui.setCss(root_, "flex-shrink", "0");
	ui.setCss(root_, "align-items", "center");
	ui.setCss(root_, "gap", "8px");
	ui.setCss(root_, "padding", "6px 6px 6px 10px");
	ui.setCss(root_, "min-height", "44px");

	title_ = w::label(ui, root_, "");
	ui.addClass(title_, "retdec-banner-title");
	titleTip_ = w::setTooltip(ui, title_, "Currently loaded binary");
	formatBadge_ = w::label(ui, root_, kDash);
	archBadge_ = w::toolButton(ui, root_, kDash);
	osBadge_ = w::toolButton(ui, root_, kDash);
	sizeBadge_ = w::label(ui, root_, kDash);
	for (NodeHandle b: {formatBadge_, archBadge_, osBadge_, sizeBadge_})
		ui.addClass(b, "retdec-badge");
	w::setTooltip(ui, formatBadge_, "Container format reported by retdec-fileinfo");
	w::setTooltip(ui, archBadge_, "Target architecture \xE2\x80\x94 click to open the Target panel");
	w::setTooltip(ui, osBadge_, "Target operating system \xE2\x80\x94 click to open the Target panel");
	w::setTooltip(ui, sizeBadge_, "File size on disk");
	packerBadge_ = w::label(ui, root_, "");
	ui.addClass(packerBadge_, "retdec-badge");
	ui.addClass(packerBadge_, "retdec-badge-warning");
	w::setTooltip(
		ui,
		packerBadge_,
		"fileinfo detected a packer signature. Use 'Unpack first' or unpack manually before decompiling for best "
		"results.");
	setShown(ui, packerBadge_, false);

	const NodeHandle spacer = w::panel(ui, root_);
	ui.setCss(spacer, "flex-grow", "1");
	unpackBtn_ = w::button(ui, root_, "Unpack first");
	decompileBtn_ = w::button(ui, root_, "Decompile anyway");
	moreBtn_ = w::button(ui, root_, "More \xE2\x96\xBC"); // ▼
	w::setTooltip(ui, unpackBtn_, "Run retdec-unpacker on the current binary and reopen the result.");
	w::setTooltip(ui, decompileBtn_, "Run retdec-decompiler on the current binary as-is (Ctrl+R / F5).");
	w::setTooltip(
		ui, moreBtn_, "Show secondary triage tools (re-run fileinfo, copy hash, view raw output\xE2\x80\xA6).");
	closeBtn_ = w::toolButton(ui, root_, "\xE2\x9C\x95"); // ✕
	w::setTooltip(ui, closeBtn_, "Hide this banner for the current binary.");

	auto* self = this;
	ui.connect(unpackBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) {
		if (self->onUnpackRequested) self->onUnpackRequested();
	});
	ui.connect(decompileBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) {
		if (self->onDecompileRequested) self->onDecompileRequested();
	});
	ui.connect(moreBtn_, SignalKind::Clicked, [self](UiContext& c, NodeHandle n, const SignalArgs&) {
		const Rect r = c.rect(n);
		if (self->onMoreActionsRequested) self->onMoreActionsRequested({r.x, r.bottom()});
	});
	ui.connect(closeBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->dismiss(); });
	for (NodeHandle b: {archBadge_, osBadge_})
		ui.connect(b, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) {
			if (self->onTargetDetailsRequested) self->onTargetDetailsRequested();
		});
	setBinary({}); // start hidden
}

TriageBanner::~TriageBanner()
{
	releasePanelRoot(ui_, root_);
}

void TriageBanner::setBinary(std::string_view path)
{
	if (path.empty())
	{
		hidden_ = true;
		setShown(ui_, root_, false);
		ui_.setText(title_, "");
		titleTooltip_.clear();
		for (NodeHandle b: {formatBadge_, archBadge_, osBadge_, sizeBadge_})
			ui_.setText(b, kDash);
		packerShown_ = false;
		setShown(ui_, packerBadge_, false);
		return;
	}
	const std::filesystem::path p{std::string(path)};
	std::error_code ec;
	const std::filesystem::path abs = std::filesystem::absolute(p, ec);
	ui_.setText(title_, p.filename().string());
	titleTooltip_ = ec ? p.string() : abs.string();
	ui_.setText(titleTip_, titleTooltip_);
	ui_.setText(sizeBadge_, model::humanSize(model::fileSize(p)));
	for (NodeHandle b: {formatBadge_, archBadge_, osBadge_})
		ui_.setText(b, kEllipsis);
	packerShown_ = false;
	setShown(ui_, packerBadge_, false);
	hidden_ = false;
	setShown(ui_, root_, true);
}

void TriageBanner::setMetadata(
	std::string_view format, std::string_view arch, std::string_view os, int64_t sizeBytes, std::string_view packerName)
{
	ui_.setText(formatBadge_, format.empty() ? kDash : format);
	ui_.setText(archBadge_, arch.empty() ? kDash : arch);
	ui_.setText(osBadge_, os.empty() ? kDash : os);
	if (sizeBytes > 0) ui_.setText(sizeBadge_, model::humanSize(sizeBytes));
	packerShown_ = !packerName.empty();
	if (packerShown_) ui_.setText(packerBadge_, std::format("\xE2\x9A\xA0 {}-packed", packerName)); // ⚠
	setShown(ui_, packerBadge_, packerShown_);
}

void TriageBanner::setFileinfo(const model::FileinfoInfo& info, int64_t sizeBytes)
{
	setMetadata(info.format, info.arch, info.os, sizeBytes, info.packer);
}

void TriageBanner::setActionsEnabled(bool on)
{
	actionsEnabled_ = on;
	for (NodeHandle b: {unpackBtn_, decompileBtn_, moreBtn_})
		ui_.setDisabled(b, !on);
}

void TriageBanner::dismiss()
{
	hidden_ = true;
	setShown(ui_, root_, false);
	if (onDismissed) onDismissed();
}

std::string TriageBanner::titleText() const
{
	return std::string(ui_.text(title_));
}
std::string TriageBanner::formatText() const
{
	return std::string(ui_.text(formatBadge_));
}
std::string TriageBanner::archText() const
{
	return std::string(ui_.text(archBadge_));
}
std::string TriageBanner::osText() const
{
	return std::string(ui_.text(osBadge_));
}
std::string TriageBanner::sizeText() const
{
	return std::string(ui_.text(sizeBadge_));
}
std::string TriageBanner::packerText() const
{
	return std::string(ui_.text(packerBadge_));
}

} // namespace retdec::gui::ui
