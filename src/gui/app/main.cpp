/**
 * @file src/gui/app/main.cpp
 * @brief retdec-gui entry point (FuseUI).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"

#include <fuse/ui/widgets/widgets.hpp>

int main(int argc, char** argv)
{
	fuse::app::Application app{{.name = "retdec-gui", .argc = argc, .argv = argv}};
	auto& win = app.createWindow({.title = "RetDec", .width = 1280, .height = 800});
	fuse::ui::widgets::label(win.ui(), win.ui().root(), "RetDec");
	return app.run();
}
