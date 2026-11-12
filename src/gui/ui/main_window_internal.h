/**
 * @file src/gui/ui/main_window_internal.h
 * @brief Private pieces shared by the main_window*.cpp files.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_SRC_UI_MAIN_WINDOW_INTERNAL_H
#define RETDEC_GUI_SRC_UI_MAIN_WINDOW_INTERNAL_H

#include "retdec/gui/ui/main_window.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <fuse/ui/ui.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec::gui::ui {

namespace detail {
/// Default dock region sizes (Qt resizeDocks 240 / 240 / 180, widened for the
/// function list's filter bar and the Workspace tables).
inline constexpr int kDefaultLeftDockPx = 300;
inline constexpr int kDefaultRightDockPx = 320;
inline constexpr int kDefaultBottomDockPx = 200;
} // namespace detail

/// A Qt top-level tool window: a FuseUI dialog with a body and a Close button.
struct MainWindow::ToolWindow
{
	ToolWindow(fuse::ui::UiContext& ui, const std::string& title, float width, float height);
	~ToolWindow();
	std::string title;
	float width = 0.f, height = 0.f;
	std::unique_ptr<ModalDialog> dlg;
	fuse::ui::NodeHandle body, closeButton;
	/// Visualisation plugins: the markdown view and the text it shows.
	fuse::ui::NodeHandle pluginView;
	std::string pluginText;
	std::string pluginId;
};

/// QMessageBox::question with up to three buttons.
struct MainWindow::Question
{
	explicit Question(fuse::ui::UiContext& ui);
	ModalDialog dlg;
	fuse::ui::NodeHandle text, buttons;
	std::vector<fuse::ui::NodeHandle> buttonNodes;
	std::function<void(int)> onAnswer;
	std::string message;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_SRC_UI_MAIN_WINDOW_INTERNAL_H
