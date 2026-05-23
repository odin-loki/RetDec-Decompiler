/**
 * @file src/gui/theme.cpp
 * @brief Theme / QSS loading from AppSettings.
 */

#include "retdec/gui/theme.h"
#include "retdec/gui/settings/settings.h"

#include <QApplication>
#include <QFile>

namespace retdec::gui {

void applyThemeFromSettings(QApplication& app) {
    using Theme = GeneralSettings::Theme;

    QString qssPath = QStringLiteral(":/retdec/catppuccin_mocha.qss");
    if (AppSettings::instance().general.theme == Theme::Light)
        qssPath = QStringLiteral(":/retdec/catppuccin_latte.qss");

    QFile qssFile(qssPath);
    if (!qssFile.open(QIODevice::ReadOnly)) {
        qssFile.setFileName(QStringLiteral(":/retdec/catppuccin_mocha.qss"));
        qssFile.open(QIODevice::ReadOnly);
    }
    if (qssFile.isOpen())
        app.setStyleSheet(QString::fromUtf8(qssFile.readAll()));
}

} // namespace retdec::gui
