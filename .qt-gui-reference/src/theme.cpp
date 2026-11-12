/**
 * @file src/gui/theme.cpp
 * @brief Theme / QSS loading from AppSettings.
 */

#include "retdec/gui/theme.h"
#include "retdec/gui/settings/settings.h"

#include <QApplication>
#include <QFile>
#include <QStyleHints>
#include <QtGlobal>

namespace retdec::gui {

namespace {

QString themeQssPath(GeneralSettings::Theme theme) {
    using Theme = GeneralSettings::Theme;

    if (theme == Theme::Light)
        return QStringLiteral(":/retdec/catppuccin_latte.qss");

    if (theme == Theme::SystemDefault) {
#ifdef Q_OS_WIN
        // No reliable system-theme API on Windows in v3 — use Mocha.
        return QStringLiteral(":/retdec/catppuccin_mocha.qss");
#else
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        const auto scheme = QApplication::styleHints()->colorScheme();
        if (scheme == Qt::ColorScheme::Light)
            return QStringLiteral(":/retdec/catppuccin_latte.qss");
#endif
#endif
    }

    return QStringLiteral(":/retdec/catppuccin_mocha.qss");
}

} // namespace

void applyThemeFromSettings(QApplication& app) {
    const QString qssPath = themeQssPath(AppSettings::instance().general.theme);

    QFile qssFile(qssPath);
    if (!qssFile.open(QIODevice::ReadOnly)) {
        qssFile.setFileName(QStringLiteral(":/retdec/catppuccin_mocha.qss"));
        qssFile.open(QIODevice::ReadOnly);
    }
    if (qssFile.isOpen())
        app.setStyleSheet(QString::fromUtf8(qssFile.readAll()));
}

} // namespace retdec::gui
