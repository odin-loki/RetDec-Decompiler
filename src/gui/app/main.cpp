/**
 * @file src/gui/app/main.cpp
 * @brief retdec-gui entry point (FuseUI).
 *
 *   retdec-gui [binary | project.retdec]
 *   retdec-gui --headless [--headless-exit-ms N]          (RETDEC_GUI_HEADLESS=1)
 *   retdec-gui --headless-decompile [--fast] <binary>     (CI / CLI parity)
 *
 * Without --headless and with a display, the window is a real OS window
 * (fuse::app::NativePlatform); otherwise the AppWindow presents to nothing.
 * --headless-decompile runs retdec-decompiler exactly as the CLI would, loads
 * the artefacts and exits with the decompiler's status.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/artifact_loader.h"
#include "retdec/gui/model/decompiler_launch.h"
#include "retdec/gui/model/launch_options.h"
#include "retdec/gui/model/plugins.h"
#include "retdec/gui/model/settings.h"
#include "retdec/gui/ui/main_window.h"

#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using retdec::gui::model::AppSettings;

constexpr const char* kUsage =
	"Usage: retdec-gui [options] [binary | project.retdec]\n"
	"\n"
	"Retargetable Decompiler GUI.\n"
	"\n"
	"Options:\n"
	"  --headless               run without an OS window (also RETDEC_GUI_HEADLESS=1)\n"
	"  --headless-exit-ms N     quit after N ms (automated smoke / debug)\n"
	"  --headless-decompile     decompile the binary exactly as the CLI would, load the\n"
	"                           artefacts and exit with the decompiler's status\n"
	"  --fast, --fast-decompile with --headless-decompile: the Fast preset\n"
	"  -h, --help               this help\n";

/// A square RetDec glyph (Catppuccin Mocha: base background, blue braces) at
/// @p size px: the window icon, drawn rather than shipped as a file.
fuse::ui::Image makeIcon(int size)
{
	using fuse::ui::Color;
	fuse::ui::Image img;
	img.resize(size, size, 0);
	const auto bg = fuse::ui::packPremultiplied(Color::rgb(0x1e1e2e));
	const auto fg = fuse::ui::packPremultiplied(Color::rgb(0x89b4fa));
	const auto fg2 = fuse::ui::packPremultiplied(Color::rgb(0xa6e3a1));
	const float r = size * 0.18f;
	auto inRounded = [&](float x, float y) {
		const float cx = std::clamp(x, r, size - r), cy = std::clamp(y, r, size - r);
		return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
	};
	const float t = std::max(1.f, size * 0.08f);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			const float fx = x + 0.5f, fy = y + 0.5f;
			if (!inRounded(fx, fy)) continue;
			auto& px = img.pixels[std::size_t(y) * std::size_t(size) + std::size_t(x)];
			px = bg;
			const float u = fx / size, v = fy / size;
			// "{" left and "}" right: vertical bars with a notch in the middle.
			const bool leftBar = std::abs(u - 0.28f) * size < t * 0.5f && v > 0.2f && v < 0.8f;
			const bool rightBar = std::abs(u - 0.72f) * size < t * 0.5f && v > 0.2f && v < 0.8f;
			const bool caps = (std::abs(v - 0.2f) * size < t * 0.5f || std::abs(v - 0.8f) * size < t * 0.5f)
			                  && ((u > 0.28f && u < 0.38f) || (u > 0.62f && u < 0.72f));
			const bool notch = std::abs(v - 0.5f) * size < t * 0.5f && ((u > 0.18f && u < 0.28f) || (u > 0.72f && u < 0.82f));
			const bool dot = std::abs(u - 0.5f) * size < t * 0.6f && std::abs(v - 0.5f) * size < t * 0.6f;
			if (leftBar || rightBar || caps || notch) px = fg;
			if (dot) px = fg2;
		}
	return img;
}

} // namespace

int main(int argc, char** argv)
{
	namespace ui = retdec::gui::ui;
	namespace model = retdec::gui::model;

	model::ParsedLaunchOptions launch = model::parseLaunchOptions(argc, argv);
	for (const std::string& a: launch.argStorage)
		if (a == "-h" || a == "--help")
		{
			std::fputs(kUsage, stdout);
			return 0;
		}
	const bool headless = launch.headless || launch.headlessDecompile;
	if (launch.headlessDecompile && !launch.binaryPath)
	{
		std::fprintf(stderr, "retdec-gui: --headless-decompile requires a binary path\n");
		return 2;
	}

	auto& settings = AppSettings::instance();
	if (auto r = settings.load(); !r) std::fprintf(stderr, "retdec-gui: settings: %s\n", r.error().c_str());
	if (settings.plugins.autoLoadPlugins && !settings.plugins.searchPaths.empty())
		model::PluginManager::instance().scanAndLoad(settings.plugins.searchPaths);

	fuse::app::Application app{{.name = "retdec-gui", .argc = launch.argc, .argv = launch.argvPtrs.data()}};
	const double w = app.settings().number("window.width", ui::MainWindow::kDefaultWidth);
	const double h = app.settings().number("window.height", ui::MainWindow::kDefaultHeight);
	auto& win = app.createWindow({.title = "RetDec \xE2\x80\x94 Retargetable Decompiler",
	                              .width = std::clamp(unsigned(w), 640u, 8192u),
	                              .height = std::clamp(unsigned(h), 480u, 8192u)});
	ui::bindSystemClipboard(win.ui());

	std::optional<fuse::app::NativePlatform> native;
	auto mw = std::make_unique<ui::MainWindow>(
		win.ui(), ui::MainWindowOptions{.headless = headless, .layoutStore = &app.settings(), .settings = &settings});
	mw->setWindowSize(unsigned(win.size().w), unsigned(win.size().h));
	mw->onQuitRequested = [&app](int code) { app.quit(code); };
	mw->onTitleChanged = [&win](const std::string& title) { win.setTitle(title); };
	mw->onResizeRequested = [&native, &win](unsigned width, unsigned height) {
		if (native && native->kind(win) != fuse::app::PresenterKind::Headless)
			native->resizeWindow(win, width, height);
		else
			win.resize(width, height);
	};

	if (!headless)
	{
		native.emplace(app);
		const fuse::app::PresenterKind kind = native->show(win);
		if (kind != fuse::app::PresenterKind::Headless)
		{
			std::vector<fuse::ui::Image> icons;
			for (int s: {16, 32, 48, 256})
				icons.push_back(makeIcon(s));
			native->setIcon(win, icons);
		}
		else if (!native->available())
			std::fprintf(stderr, "retdec-gui: no display; running without a window\n");
	}

	if (launch.headlessDecompile)
	{
		mw->setFastDecompilePreset(launch.fastDecompile);
		mw->enableQuitWhenDecompileFinishes(true);
		// Start from a clean slate so a stale cache is never what gets checked.
		const fs::path cPath = model::resolveGuiDecompiledCPath(fs::absolute(*launch.binaryPath));
		std::error_code ec;
		fs::remove(cPath, ec);
		fs::remove(model::pathsFromOutputC(cPath).configPath, ec);
	}
	if (launch.binaryPath) mw->openPath(*launch.binaryPath);

	ui::MainWindow* mwp = mw.get();
	if (launch.headlessDecompile)
		win.ui().startTimer(0.1f, false, [mwp](fuse::ui::UiContext&) { mwp->runDecompileForBenchmark(); });
	else if (launch.headlessExitMs > 0)
	{
		fuse::app::Application* appp = &app;
		win.ui().startTimer(float(launch.headlessExitMs) / 1000.f, false, [appp](fuse::ui::UiContext&) { appp->quit(0); });
	}

	while (true)
	{
		mw->setWindowSize(unsigned(win.size().w), unsigned(win.size().h));
		mw->snapshotLayout();
		if (!app.step()) break;
		if (app.windowCount() == 0) break; // the OS closed the window: `win` is gone
	}
	if (app.windowCount() == 0)
	{
		// The context the window drew into has been destroyed with the AppWindow:
		// stop child processes, then let the process exit without the UI teardown.
		mw->abandonUi();
		(void)mw.release();
	}
	else
	{
		mw->shutdown();
		mw.reset();
	}
	native.reset();
	// The loop has ended (quit() or no windows left): run() returns at once with
	// the exit code quit() recorded (the decompiler's status for --headless-decompile).
	return app.run();
}
