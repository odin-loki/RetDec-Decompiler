/**
 * @file tests/gui/table_panels_test_util.h
 * @brief Headless FuseUI harness for the function list, strings, diff and
 *        progress panel tests: a UiContext with the bundled font and the
 *        default widgets, user-style input helpers, tree search and PNG
 *        rendering (set RETDEC_GUI_TEST_PNG_DIR to keep the images).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_TESTS_TABLE_PANELS_TEST_UTIL_H
#define RETDEC_GUI_TESTS_TABLE_PANELS_TEST_UTIL_H

#include <fuse/app/app.hpp>
#include <fuse/text/text.hpp>
#include <fuse/ui/draw_list.hpp>
#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <string_view>

namespace retdec_gui_tp_test {

inline const fuse::text::Font& bundledFont()
{
	static fuse::text::Font f = [] {
		auto r = fuse::text::Font::bundled();
		if (!r) throw std::runtime_error("bundled font missing");
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

class Headless
{
public:
	explicit Headless(fuse::ui::Size viewport = {1200.f, 700.f}):
		ui(fuse::ui::UiContextDesc{viewport, &bundledFont()})
	{
		fuse::ui::widgets::install(ui);
		ui.registerBundledFonts(); // "monospace", bold faces, symbol fallback (as AppWindow)
		ui.setCss(ui.root(), "padding", "0px");
		ui.setCss(ui.root(), "gap", "0px");
	}

	fuse::ui::UiContext ui;

	void frame(int n = 2)
	{
		for (int i = 0; i < n; ++i)
			ui.update(0.016f);
	}

	void click(fuse::ui::Vec2 p, fuse::ui::MouseButton b = fuse::ui::MouseButton::Left, fuse::ui::u8 mods = 0)
	{
		ui.setModifiers(mods);
		ui.pointerMove(p);
		ui.pointerDown(p, b);
		ui.pointerUp(p, b);
		ui.setModifiers(0);
		frame();
	}

	void clickNode(fuse::ui::NodeHandle n)
	{
		const fuse::ui::Rect r = ui.rect(n);
		click({r.x + r.w * 0.5f, r.y + r.h * 0.5f});
	}

	/// Focuses @p edit, clears it and types @p text like a user.
	void typeInto(fuse::ui::NodeHandle edit, std::string_view text)
	{
		ui.setFocus(edit, fuse::ui::FocusReason::Keyboard);
		ui.keyDown(fuse::ui::Key::A, fuse::ui::kModCtrl);
		ui.keyDown(fuse::ui::Key::Backspace);
		if (!text.empty()) ui.textInput(text);
		frame();
	}

	void key(fuse::ui::Key k, fuse::ui::u8 mods = 0)
	{
		ui.keyDown(k, mods);
		ui.keyUp(k, mods);
		frame();
	}

	/// First live node (depth-first from the root) whose text is @p text.
	fuse::ui::NodeHandle findByText(std::string_view text) const { return findRec(ui.root(), text); }

	/// Renders the frame; writes `<RETDEC_GUI_TEST_PNG_DIR>/<name>` when that
	/// variable is set. Returns the number of opaque-ish pixels that differ
	/// from the top-left pixel (a cheap "something was drawn" check).
	std::size_t render(const std::string& name)
	{
		frame();
		fuse::ui::Image img;
		img.resize(static_cast<int>(ui.viewport().w), static_cast<int>(ui.viewport().h), 0xFF000000u);
		fuse::ui::CpuRasterizer r;
		r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
		r.render(ui.drawList(), img);
		for (auto& p: img.pixels)
			p |= 0xFF000000u;
		if (const char* dir = std::getenv("RETDEC_GUI_TEST_PNG_DIR"); dir && *dir)
		{
			std::filesystem::create_directories(dir);
			fuse::app::writePng(img, (std::filesystem::path(dir) / name).string());
		}
		std::size_t differing = 0;
		const auto first = img.pixels.empty() ? 0u : img.pixels[0];
		for (auto p: img.pixels)
			differing += p != first;
		return differing;
	}

private:
	fuse::ui::NodeHandle findRec(fuse::ui::NodeHandle n, std::string_view text) const
	{
		if (!ui.alive(n)) return {};
		if (ui.text(n) == text) return n;
		for (auto c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c))
			if (auto f = findRec(c, text); f.valid()) return f;
		return {};
	}
};

} // namespace retdec_gui_tp_test

#endif // RETDEC_GUI_TESTS_TABLE_PANELS_TEST_UTIL_H
