/**
 * @file include/retdec/gui/theme.h
 * @brief Application theme / QSS loading from AppSettings.
 */

#ifndef RETDEC_GUI_THEME_H
#define RETDEC_GUI_THEME_H

class QApplication;

namespace retdec::gui {

/**
 * @brief Apply the QSS theme selected in AppSettings::general.theme.
 *
 * Only Catppuccin Mocha is bundled in v3. Light theme attempts
 * catppuccin_latte.qss and falls back to Mocha when unavailable.
 */
void applyThemeFromSettings(QApplication& app);

} // namespace retdec::gui

#endif // RETDEC_GUI_THEME_H
