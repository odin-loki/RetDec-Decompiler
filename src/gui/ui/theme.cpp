/**
 * @file src/gui/ui/theme.cpp
 * @brief Catppuccin Mocha / Latte FuseUI themes (port of the Qt theme.cpp).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/theme.h"

#include "retdec/gui/model/cli_tool_paths.h"

#include <forge/os/os.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>

namespace retdec::gui::ui {

using fuse::ui::Color;

namespace {

constexpr std::string_view kMochaTag = "retdec-theme-mocha";
constexpr std::string_view kLatteTag = "retdec-theme-latte";

std::string envLower(const char* name)
{
	const char* v = std::getenv(name);
	std::string s = v ? v : "";
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

} // namespace

std::string_view themeName(ThemeKind kind)
{
	return kind == ThemeKind::Latte ? "catppuccin_latte" : "catppuccin_mocha";
}

bool systemPrefersLight()
{
	// RETDEC_GUI_COLOR_SCHEME=light|dark wins (tests, users without a desktop setting).
	const std::string forced = envLower("RETDEC_GUI_COLOR_SCHEME");
	if (forced == "light") return true;
	if (forced == "dark") return false;
	// GTK_THEME=Adwaita:dark / Adwaita-dark / ...-light.
	const std::string gtk = envLower("GTK_THEME");
	if (!gtk.empty())
	{
		if (gtk.find("dark") != std::string::npos) return false;
		if (gtk.find("light") != std::string::npos) return true;
	}
	return false;
}

ThemeKind themeKindFor(model::GeneralSettings::Theme theme)
{
	using T = model::GeneralSettings::Theme;
	switch (theme)
	{
	case T::Light: return ThemeKind::Latte;
	case T::SystemDefault: return systemPrefersLight() ? ThemeKind::Latte : ThemeKind::Mocha;
	case T::Dark: break;
	}
	return ThemeKind::Mocha;
}

std::filesystem::path themeCssPath(ThemeKind kind)
{
	return model::resolveGuiResource(std::format("themes/{}.css", themeName(kind)));
}

std::expected<std::string, std::string> loadThemeCss(ThemeKind kind)
{
	std::filesystem::path p = themeCssPath(kind);
	if (p.empty()) p = themeCssPath(ThemeKind::Mocha);
	if (p.empty()) return std::unexpected(std::format("Theme {} not found in the GUI resources", themeName(kind)));
	auto r = forge::os::read_file(p.string());
	if (!r) return std::unexpected(std::format("Cannot read {}: {}", p.string(), r.err.what));
	return std::move(r.value);
}

CodePalette codePaletteFor(ThemeKind kind)
{
	CodePalette p = defaultCodePalette();
	if (kind == ThemeKind::Mocha) return p;
	// Catppuccin Latte.
	p.background = Color::rgb(0xeff1f5);
	p.gutter = Color::rgb(0xe6e9ef);
	p.gutterText = Color::rgb(0x9ca0b0);
	p.gutterTextActive = Color::rgb(0x1e66f5);
	p.text = Color::rgb(0x4c4f69);
	p.placeholder = Color::rgb(0x9ca0b0);
	p.currentLine = Color::rgb(0x000000, 0.04f);
	p.highlightLine = Color::rgb(0xbcc0cc, 160.f / 255.f);
	p.selection = Color::rgb(0xacb0be);
	p.caret = Color::rgb(0xdc8a78);
	p.scrollThumb = Color::rgb(0x8c8fa1, 0.85f);
	using TK = model::TokenKind;
	auto set = [&](TK k, Color c) { p.tokens[static_cast<std::size_t>(k)] = c; };
	set(TK::Text, Color::rgb(0x4c4f69));
	set(TK::Keyword, Color::rgb(0x1e66f5));      // blue
	set(TK::Type, Color::rgb(0xdf8e1d));         // yellow
	set(TK::String, Color::rgb(0x40a02b));       // green
	set(TK::Number, Color::rgb(0xfe640b));       // peach
	set(TK::Comment, Color::rgb(0x9ca0b0));      // overlay0
	set(TK::Preprocessor, Color::rgb(0xd20f39)); // red
	set(TK::Register, Color::rgb(0x8839ef));     // mauve
	set(TK::Mnemonic, Color::rgb(0x1e66f5));     // blue
	set(TK::Address, Color::rgb(0x8c8fa1));      // overlay1
	set(TK::Label, Color::rgb(0xdf8e1d));        // yellow
	set(TK::SsaName, Color::rgb(0x8839ef));      // mauve
	set(TK::Builtin, Color::rgb(0x179299));      // teal
	p.console = {Color::rgb(0x4c4f69), Color::rgb(0xdf8e1d), Color::rgb(0xd20f39), Color::rgb(0x1e66f5),
	             Color::rgb(0x40a02b)};
	return p;
}

std::expected<ThemeKind, std::string> applyTheme(fuse::ui::UiContext& ui, ThemeKind kind)
{
	auto css = loadThemeCss(kind);
	if (!css) return std::unexpected(css.error());
	std::vector<fuse::ui::StyleDiagnostic> diags;
	if (!ui.addStyleSheet(*css, &diags))
		return std::unexpected(std::format("Theme {} does not parse ({} problem(s))", themeName(kind), diags.size()));
	// A Mocha fallback for a missing Latte file is still Mocha.
	const ThemeKind applied = themeCssPath(kind).empty() ? ThemeKind::Mocha : kind;
	ui.removeClass(ui.root(), kMochaTag);
	ui.removeClass(ui.root(), kLatteTag);
	ui.addClass(ui.root(), applied == ThemeKind::Latte ? kLatteTag : kMochaTag);
	return applied;
}

std::expected<ThemeKind, std::string> applyThemeFromSettings(fuse::ui::UiContext& ui, const model::AppSettings& settings)
{
	return applyTheme(ui, themeKindFor(settings.general.theme));
}

ThemeKind currentTheme(const fuse::ui::UiContext& ui)
{
	return ui.hasClass(ui.root(), kLatteTag) ? ThemeKind::Latte : ThemeKind::Mocha;
}

float editorFontPx(int pointSize)
{
	const int pt = std::clamp(pointSize, 6, 48);
	return static_cast<float>(pt) * 96.f / 72.f;
}

} // namespace retdec::gui::ui
