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
 * Dark loads catppuccin_mocha.qss; Light loads catppuccin_latte.qss.
 * System Default follows the OS colour scheme when available (Windows
 * falls back to Mocha). Missing QSS files fall back to Mocha.
 */
void applyThemeFromSettings(QApplication& app);

} // namespace retdec::gui

#endif // RETDEC_GUI_THEME_H
