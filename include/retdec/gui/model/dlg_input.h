/**
 * @file include/retdec/gui/model/dlg_input.h
 * @brief Parsing of what the user types into the small dialogs (go to
 *        address / line, entry point, max memory, archive index), with the
 *        exact rules the Qt GUI's QString::toULongLong / toInt calls had.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DLG_INPUT_H
#define RETDEC_GUI_MODEL_DLG_INPUT_H

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace retdec::gui::model {

/// Leading / trailing ASCII whitespace removed.
std::string_view dlgTrim(std::string_view s);

/// Unsigned integer in @p base (2..36), whole string, optional leading '+'
/// (QString::toULongLong(&ok, base) on trimmed text). Nothing on overflow or
/// on any stray character.
std::optional<uint64_t> dlgParseUnsigned(std::string_view text, int base);

/// QString::toULongLong(&ok, 0): "0x" / "0X" prefix hex, a leading "0" octal,
/// otherwise decimal. Surrounding whitespace is ignored; empty fails.
std::optional<uint64_t> dlgParseUnsignedAuto(std::string_view text);

/// Signed decimal int (QString::toInt): optional sign, whole string.
std::optional<int> dlgParseInt(std::string_view text);

/// Go to address (View → Go to address…): toULongLong(&ok, 0) on the trimmed
/// text. Error text: "Not a valid address".
std::expected<uint64_t, std::string> dlgParseGotoAddress(std::string_view text);

/// Go to line (Ctrl+L): a decimal int >= 1. Error: "Not a valid line number".
std::expected<int, std::string> dlgParseGotoLine(std::string_view text);

/// Target panel entry point: empty = 0; "0x…" (any case) hex; otherwise
/// decimal. Error: "Invalid entry point (use 0x… hex or decimal)."
std::expected<uint64_t, std::string> dlgParseEntryPoint(std::string_view text);

/// What the Target panel shows for an entry point: "0x401000", or "" for 0.
std::string dlgFormatEntryPoint(uint64_t entryPoint);

/// Analysis → Set max memory: empty or "0" = 0 (omit --max-memory); otherwise
/// a decimal byte count. Error: "Not a valid byte count."
std::expected<uint64_t, std::string> dlgParseMaxMemory(std::string_view text);

/// The prompt's initial text for a stored limit ("0" when none).
std::string dlgFormatMaxMemory(uint64_t bytes);

/// Status-bar line after the max-memory prompt is accepted.
std::string dlgMaxMemoryStatus(uint64_t bytes);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DLG_INPUT_H
