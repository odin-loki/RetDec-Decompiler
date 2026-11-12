/**
 * @file tests/gui/dlg_test_support.h
 * @brief Headless UiContext harness for the dialog / panel tests: bundled
 *        font, user-like input (click a node, type into a field) and PNG
 *        snapshots (CpuRasterizer + fuse::app::writePng).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_TESTS_DLG_TEST_SUPPORT_H
#define RETDEC_GUI_TESTS_DLG_TEST_SUPPORT_H

#include "retdec/gui/ui/empty_state.h"

#include <fuse/app/app.hpp>
#include <fuse/text/text.hpp>
#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include <string_view>
#include <thread>

namespace retdec::gui::test {

inline const fuse::text::Font& bundledFont()
{
	static const fuse::text::Font f = [] {
		auto r = fuse::text::Font::bundled();
		if (!r) std::abort();
		// FuseUI's glyph fallback chain (DejaVu Sans: ✓ ▶ ⚠ …), as fuse::app::defaultFont() has.
		static const std::vector<fuse::text::Font> fallbacks = fuse::text::Font::bundledFallbacks();
		std::vector<const fuse::text::Font*> chain;
		for (const auto& x: fallbacks)
			chain.push_back(&x);
		r->setFallbacks(std::move(chain));
		return std::move(*r);
	}();
	return f;
}

/// A headless UI with the retdec classes registered.
struct DlgUi
{
	fuse::ui::UiContext ui;

	explicit DlgUi(float w = 1100.f, float h = 820.f): ui({.viewport = {w, h}, .font = &bundledFont()})
	{
		retdec::gui::ui::ensureRetdecUiClasses(ui);
		ui.update();
	}

	void step(int frames = 2)
	{
		for (int i = 0; i < frames; ++i)
			ui.update(1.f / 60.f);
	}

	/// Press and release the pointer over the centre of @p n, as a user click.
	void click(fuse::ui::NodeHandle n)
	{
		step();
		const fuse::ui::Rect r = ui.rect(n);
		const fuse::ui::Vec2 c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
		ui.pointerMove(c);
		ui.pointerDown(c);
		ui.pointerUp(c);
		step();
	}

	/// Focus @p edit, select everything, replace it with @p text (typed).
	void type(fuse::ui::NodeHandle edit, std::string_view text)
	{
		step();
		ui.setFocus(edit, fuse::ui::FocusReason::Keyboard);
		ui.keyDown(fuse::ui::Key::A, fuse::ui::kModCtrl);
		ui.keyDown(fuse::ui::Key::Backspace);
		if (!text.empty()) ui.textInput(text);
		step();
	}

	void key(fuse::ui::Key k, fuse::ui::u8 mods = 0)
	{
		ui.keyDown(k, mods);
		ui.keyUp(k, mods);
		step();
	}

	/// Pump frames until @p cond holds (worker threads post back via ui.post).
	bool waitUntil(const std::function<bool()>& cond, int timeoutMs = 10000)
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (!cond())
		{
			if (std::chrono::steady_clock::now() > end) return false;
			step(1);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return true;
	}

	/// Render the current frame to `<RETDEC_GUI_TEST_PNG_DIR or temp>/<name>.png`; returns the path.
	std::string png(std::string_view name)
	{
		step();
		fuse::ui::Image img;
		img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
		fuse::ui::CpuRasterizer r;
		r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
		r.render(ui.drawList(), img);
		std::filesystem::path dir;
		if (const char* d = std::getenv("RETDEC_GUI_TEST_PNG_DIR"); d && *d)
			dir = d;
		else
			dir = std::filesystem::temp_directory_path() / "retdec-gui-dlg-png";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::string path = (dir / (std::string(name) + ".png")).string();
		if (!fuse::app::writePng(img, path)) return {};
		return path;
	}

	/// True when @p n and every ancestor is displayed (styles resolved).
	bool shown(fuse::ui::NodeHandle n) const
	{
		for (fuse::ui::NodeHandle c = n; ui.alive(c); c = ui.parent(c))
			if (!ui.computed(c).visible) return false;
		return ui.alive(n);
	}
};

/// A fresh empty directory under the system temp dir (removed by the caller if wanted).
inline std::filesystem::path makeTempDir(std::string_view tag)
{
	static int counter = 0;
	const auto base = std::filesystem::temp_directory_path()
					/ ("retdec-dlg-" + std::string(tag) + "-" + std::to_string(std::random_device{}()) + "-"
					   + std::to_string(++counter));
	std::error_code ec;
	std::filesystem::remove_all(base, ec);
	std::filesystem::create_directories(base, ec);
	return base;
}

} // namespace retdec::gui::test

#endif // RETDEC_GUI_TESTS_DLG_TEST_SUPPORT_H
