/**
 * @file include/retdec/gui/address_context_menu.h
 * @brief Shared address formatting / parsing for panel context menus.
 */

#ifndef RETDEC_GUI_ADDRESS_CONTEXT_MENU_H
#define RETDEC_GUI_ADDRESS_CONTEXT_MENU_H

#include <QApplication>
#include <QClipboard>
#include <QRegularExpression>
#include <QString>

#include <cstdint>
#include <optional>

namespace retdec::gui {

inline constexpr const char* kCopyAddressLabel   = "Copy Address";
inline constexpr const char* kGoToFunctionLabel  = "Go to Function";

inline QString formatAddressHex(uint64_t address)
{
    return QStringLiteral("0x%1").arg(address, 0, 16);
}

inline void copyAddressToClipboard(uint64_t address)
{
    QApplication::clipboard()->setText(formatAddressHex(address));
}

/** First 0x… token on @p line, if any. */
inline std::optional<uint64_t> parseFirstAddress(const QString& line)
{
    static const QRegularExpression re(QStringLiteral("(0x[0-9a-fA-F]+)"));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch())
        return std::nullopt;
    bool ok = false;
    const uint64_t addr = m.captured(1).toULongLong(&ok, 16);
    return ok ? std::optional<uint64_t>{addr} : std::nullopt;
}

} // namespace retdec::gui

#endif // RETDEC_GUI_ADDRESS_CONTEXT_MENU_H
