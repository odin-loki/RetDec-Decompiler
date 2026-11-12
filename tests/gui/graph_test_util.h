/**
 * @file tests/gui/graph_test_util.h
 * @brief Headless FuseUI helpers for the graph panel tests (CFG, call graph, type hierarchy).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_TESTS_GRAPH_TEST_UTIL_H
#define RETDEC_GUI_TESTS_GRAPH_TEST_UTIL_H

#include <fuse/app/app.hpp>
#include <fuse/text/text.hpp>
#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace retdec::gui::test {

inline const fuse::text::Font& bundledFont()
{
	static fuse::text::Font f = [] {
		auto r = fuse::text::Font::bundled();
		if (!r) std::abort();
		return std::move(*r);
	}();
	return f;
}

/// A headless UiContext with every widget class and the default theme installed.
struct Headless
{
	fuse::ui::UiContext ui;
	explicit Headless(float w = 1200.f, float h = 800.f): ui(fuse::ui::UiContextDesc{{w, h}, &bundledFont()})
	{
		fuse::ui::widgets::install(ui);
	}
	void frame(float dt = 1.f / 60.f) { ui.update(dt); }
	fuse::ui::Vec2 center(fuse::ui::NodeHandle n) const
	{
		const fuse::ui::Rect r = ui.rect(n);
		return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
	}
	void click(fuse::ui::Vec2 p, fuse::ui::MouseButton b = fuse::ui::MouseButton::Left)
	{
		ui.pointerMove(p);
		ui.pointerDown(p, b);
		ui.pointerUp(p, b);
		frame();
	}
	void click(fuse::ui::NodeHandle n) { click(center(n)); }
	void key(fuse::ui::Key k, fuse::ui::u8 mods = 0)
	{
		ui.keyDown(k, mods);
		ui.keyUp(k, mods);
		frame();
	}
	void type(std::string_view s)
	{
		ui.textInput(s);
		frame();
	}
	/// First live node under `from` (depth first) whose text is exactly `text`.
	fuse::ui::NodeHandle findText(std::string_view text, fuse::ui::NodeHandle from = {}) const
	{
		if (!from.valid()) from = ui.root();
		if (ui.text(from) == text) return from;
		for (auto c = ui.firstChild(from); c.valid(); c = ui.nextSibling(c))
			if (auto f = findText(text, c); f.valid()) return f;
		return {};
	}
	/// Rasterises the current frame.
	fuse::ui::Image render()
	{
		frame();
		fuse::ui::Image img;
		const auto vp = ui.viewport();
		img.resize(int(vp.w), int(vp.h), 0xFF000000u);
		fuse::ui::CpuRasterizer r;
		r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
		r.render(ui.drawList(), img);
		return img;
	}
};

/// Where screenshots go: $RETDEC_GUI_TEST_SHOTS, else the temp directory.
inline std::filesystem::path shotPath(std::string_view name)
{
	const char* dir = std::getenv("RETDEC_GUI_TEST_SHOTS");
	std::filesystem::path p = dir && *dir ? std::filesystem::path(dir) : std::filesystem::temp_directory_path();
	return p / std::string(name);
}

} // namespace retdec::gui::test

#endif // RETDEC_GUI_TESTS_GRAPH_TEST_UTIL_H
