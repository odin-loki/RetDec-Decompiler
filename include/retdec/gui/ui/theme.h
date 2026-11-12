/**
 * @file include/retdec/gui/ui/theme.h
 * @brief Application theme: the Catppuccin Mocha (dark) / Latte (light)
 *        FuseUI style sheets and the matching code-view palettes.
 *
 * The style sheets are run-time resources (`themes/catppuccin_mocha.css`,
 * `themes/catppuccin_latte.css` under model::resolveGuiResourceDir()), never
 * compiled in. GeneralSettings::Theme::SystemDefault follows the desktop's
 * colour scheme where one can be read (GTK_THEME / a "-light" / "-dark" theme
 * name, macOS AppleInterfaceStyle via the environment the Qt build did not
 * have either) and otherwise uses Mocha, as the Qt GUI did on Windows.
 *
 * A context can switch themes at run time: applyTheme() appends the new
 * sheet, and both sheets style the same selectors, so the last one wins.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_THEME_H
#define RETDEC_GUI_UI_THEME_H

#include "retdec/gui/model/settings.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace retdec::gui::ui {

enum class ThemeKind
{
	Mocha, ///< Catppuccin Mocha (dark, the default)
	Latte, ///< Catppuccin Latte (light)
};

/// The Catppuccin colours of a flavour (0xRRGGBB), for what panels paint
/// themselves (table cells, badges, severity marks). Style sheet colours are
/// the theme files' job; these follow the same palette.
struct ThemeColors
{
	uint32_t base, mantle, crust;
	uint32_t surface0, surface1, surface2;
	uint32_t overlay0, overlay1;
	uint32_t text, subtext0, subtext1;
	uint32_t blue, green, yellow, red, peach, mauve, teal, maroon;
	/// Text on an accent-coloured fill (badges): base in both flavours.
	uint32_t onAccent;
};

/// Mocha or Latte colours.
const ThemeColors& themeColors(ThemeKind kind);

/// "catppuccin_mocha" / "catppuccin_latte".
std::string_view themeName(ThemeKind kind);

/// Dark -> Mocha, Light -> Latte, SystemDefault -> systemPrefersLight() ? Latte : Mocha.
ThemeKind themeKindFor(model::GeneralSettings::Theme theme);

/// Best-effort desktop light-mode detection (environment only; false when unknown).
bool systemPrefersLight();

/// `themes/<name>.css` resolved through model::resolveGuiResource (empty when missing).
std::filesystem::path themeCssPath(ThemeKind kind);

/// The style sheet text of @p kind; falls back to the Mocha file when the
/// requested one is missing. Error when neither file can be read.
std::expected<std::string, std::string> loadThemeCss(ThemeKind kind);

/// Code / console view colours for @p kind (Mocha = defaultCodePalette()).
CodePalette codePaletteFor(ThemeKind kind);

/**
 * @brief Add the theme's sheet to @p ui (and tag the root with
 *        `retdec-theme-mocha` / `retdec-theme-latte`). The previously applied
 *        theme of the context is overridden. Returns the kind applied, or the
 *        error when no theme file could be loaded (the context then keeps the
 *        FuseUI default theme).
 */
std::expected<ThemeKind, std::string> applyTheme(fuse::ui::UiContext& ui, ThemeKind kind);

/// applyTheme(ui, themeKindFor(settings.general.theme)).
std::expected<ThemeKind, std::string>
applyThemeFromSettings(fuse::ui::UiContext& ui, const model::AppSettings& settings = model::AppSettings::instance());

/// The theme last applied to @p ui by applyTheme (Mocha when none was).
ThemeKind currentTheme(const fuse::ui::UiContext& ui);

/// themeColors(currentTheme(ui)): the palette of the theme @p ui shows.
const ThemeColors& themeColors(const fuse::ui::UiContext& ui);

/// A Catppuccin Mocha colour (0xRRGGBB) as the theme of @p ui shows it: the
/// same role in Latte (green -> Latte green ...); colours outside the Mocha
/// palette, and every colour under Mocha, are returned unchanged. For the
/// model's Mocha-coded colours (string categories, constant labels).
uint32_t themedColor(ThemeKind kind, uint32_t mochaRgb);
uint32_t themedColor(const fuse::ui::UiContext& ui, uint32_t mochaRgb);

/// Point size (Settings → General) to the px size the code views take (96 dpi).
float editorFontPx(int pointSize);

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_THEME_H
