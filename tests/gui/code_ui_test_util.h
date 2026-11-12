/**
 * @file tests/gui/code_ui_test_util.h
 * @brief Headless FuseUI fixture for the code / console / inspect panel tests:
 *        a UiContext with the bundled font and the widget set, plus input
 *        helpers that drive it the way a user's events do.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_TESTS_GUI_CODE_UI_TEST_UTIL_H
#define RETDEC_TESTS_GUI_CODE_UI_TEST_UTIL_H

#include <fuse/app/app.hpp>
#include <fuse/text/text.hpp>
#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <thread>

namespace retdec::gui::test {

inline const fuse::text::Font& bundledFont()
{
	static fuse::text::Font f = [] {
		auto r = fuse::text::Font::bundled();
		if (!r)
		{
			std::fprintf(stderr, "bundled FuseUI font missing\n");
			std::abort();
		}
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

/// A headless UiContext (no window) sized like a docked panel.
class HeadlessUi {
public:
	explicit HeadlessUi(fuse::ui::Size viewport = {1000.f, 700.f}): ui({.viewport = viewport, .font = &bundledFont()})
	{
		fuse::ui::widgets::install(ui);
		ui.registerBundledFonts(); // "monospace", bold faces, symbol fallback (as AppWindow)
		ui.setCss(ui.root(), "padding", "0px");
		ui.setCss(ui.root(), "gap", "0px");
	}

	void frame(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			ui.update(1.f / 60.f);
	}

	/// Runs frames (and lets wall time pass) until @p cond or @p timeoutMs.
	bool pumpUntil(const std::function<bool()>& cond, int timeoutMs = 5000)
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < end)
		{
			frame();
			if (cond()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		frame();
		return cond();
	}

	fuse::ui::Vec2 center(fuse::ui::NodeHandle n) const
	{
		const fuse::ui::Rect r = ui.rect(n);
		return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
	}

	void clickAt(fuse::ui::Vec2 p, fuse::ui::MouseButton b = fuse::ui::MouseButton::Left)
	{
		ui.pointerMove(p);
		ui.pointerDown(p, b);
		ui.pointerUp(p, b);
		frame();
	}

	void click(fuse::ui::NodeHandle n)
	{
		clickAt(center(n));
	}

	void doubleClickAt(fuse::ui::Vec2 p)
	{
		ui.pointerMove(p);
		ui.pointerDown(p);
		ui.pointerUp(p);
		ui.pointerDown(p);
		ui.pointerUp(p);
		frame();
	}

	void key(fuse::ui::Key k, fuse::ui::u8 mods = 0)
	{
		ui.keyDown(k, mods);
		ui.keyUp(k, mods);
		frame();
	}

	void type(std::string_view utf8)
	{
		ui.textInput(utf8);
		frame();
	}

	std::string clipboard() const
	{
		std::string s;
		ui.clipboardText(s);
		return s;
	}

	/// Renders the current frame to a PNG (for looking at a panel).
	bool writePng(const std::string& path)
	{
		fuse::ui::Image img;
		img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
		fuse::ui::CpuRasterizer r;
		r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
		r.render(ui.drawList(), img);
		return fuse::app::writePng(img, path);
	}

	fuse::ui::UiContext ui;
};

/// A fresh scratch directory under the system temp dir.
inline std::filesystem::path scratchDir(std::string_view name)
{
	const auto base = std::filesystem::temp_directory_path() / "retdec-gui-code-tests";
	const auto dir = base / std::string(name);
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	return dir;
}

} // namespace retdec::gui::test

#endif // RETDEC_TESTS_GUI_CODE_UI_TEST_UTIL_H
