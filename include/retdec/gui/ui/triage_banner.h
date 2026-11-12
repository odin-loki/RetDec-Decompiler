/**
 * @file include/retdec/gui/ui/triage_banner.h
 * @brief TriageBanner — dismissible strip above the document tabs.
 *
 * File name + format / architecture / OS / size badges, an orange
 * "⚠ <packer>-packed" badge when fileinfo detected a packer, and the next
 * actions inline: "Unpack first", "Decompile anyway", "More ▼" and ✕.
 * Clicking the architecture or OS badge asks for the Target panel. Hidden
 * while no binary is loaded; ✕ hides it until the next setBinary().
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_TRIAGE_BANNER_H
#define RETDEC_GUI_UI_TRIAGE_BANNER_H

#include "retdec/gui/model/inspect_model.h"
#include "retdec/gui/ui/empty_state.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace retdec::gui::ui {

class TriageBanner {
public:
	TriageBanner(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~TriageBanner();
	TriageBanner(const TriageBanner&) = delete;
	TriageBanner& operator=(const TriageBanner&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}

	/// Shows the banner for @p path (badges reset to "…" until setMetadata);
	/// an empty path hides it.
	void setBinary(std::string_view path);
	/// Empty values show "—"; @p sizeBytes <= 0 keeps the current size badge;
	/// a non-empty @p packerName shows the packed badge.
	void setMetadata(
		std::string_view format,
		std::string_view arch,
		std::string_view os,
		int64_t sizeBytes,
		std::string_view packerName = {});
	/// setMetadata from parsed fileinfo.
	void setFileinfo(const model::FileinfoInfo& info, int64_t sizeBytes = 0);
	/// Greys the action buttons out (e.g. while a child process runs).
	void setActionsEnabled(bool on);
	/// What ✕ does: hides and reports onDismissed.
	void dismiss();

	bool isHidden() const
	{
		return hidden_;
	}
	bool actionsEnabled() const
	{
		return actionsEnabled_;
	}

	std::function<void()> onUnpackRequested;
	std::function<void()> onDecompileRequested;
	/// "More ▼": the host opens a popup at @p anchor (context coordinates).
	std::function<void(fuse::ui::Vec2 anchor)> onMoreActionsRequested;
	std::function<void()> onDismissed;
	/// Architecture or OS badge clicked: open the Target panel.
	std::function<void()> onTargetDetailsRequested;

	// Test hooks.
	std::string titleText() const;
	std::string titleTooltip() const
	{
		return titleTooltip_;
	}
	std::string formatText() const;
	std::string archText() const;
	std::string osText() const;
	std::string sizeText() const;
	std::string packerText() const;
	bool packerShown() const
	{
		return packerShown_;
	}
	fuse::ui::NodeHandle unpackButton() const
	{
		return unpackBtn_;
	}
	fuse::ui::NodeHandle decompileButton() const
	{
		return decompileBtn_;
	}
	fuse::ui::NodeHandle moreButton() const
	{
		return moreBtn_;
	}
	fuse::ui::NodeHandle closeButton() const
	{
		return closeBtn_;
	}
	fuse::ui::NodeHandle archBadge() const
	{
		return archBadge_;
	}
	fuse::ui::NodeHandle osBadge() const
	{
		return osBadge_;
	}

private:
	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, title_, titleTip_, formatBadge_, archBadge_, osBadge_, sizeBadge_, packerBadge_;
	fuse::ui::NodeHandle unpackBtn_, decompileBtn_, moreBtn_, closeBtn_;
	std::string titleTooltip_;
	bool hidden_ = true;
	bool packerShown_ = false;
	bool actionsEnabled_ = true;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_TRIAGE_BANNER_H
