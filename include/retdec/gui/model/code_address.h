/**
 * @file include/retdec/gui/model/code_address.h
 * @brief Address parsing shared by the code views' context menus and the
 *        assembly view's "follow jump" (the non-UI half of the Qt
 *        address_context_menu.h).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CODE_ADDRESS_H
#define RETDEC_GUI_MODEL_CODE_ADDRESS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace retdec::gui::model {

/// "0x" + lowercase hex (same as hexAddress in types.h).
std::string formatAddressHex(uint64_t address);

/// First "0x…" token on @p line, if any (and if it fits in 64 bits).
std::optional<uint64_t> parseFirstAddress(std::string_view line);

/// Parses a user-typed address for "Go to address": hex with or without a
/// "0x" prefix, surrounding whitespace ignored.
std::optional<uint64_t> parseAddressInput(std::string_view text);

/// Parses "0x…" or bare hex (fileinfo JSON values); 0 when unparsable.
uint64_t parseHexU64(std::string_view text);

/**
 * Target of the jump / call on an assembly line ("Enter follows jump"):
 * only lines with call / jmp / jcc mnemonics qualify. With two or more 0x
 * tokens on the line the last one is the target (the first is the line's own
 * address); otherwise the first address after the mnemonic. nullopt when the
 * line is not a branch, has no target, or the target is the line's own
 * address.
 */
std::optional<uint64_t> jumpTarget(std::string_view line);

/// True when @p line contains a call / jmp / jcc mnemonic as a whole word.
bool isBranchLine(std::string_view line);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CODE_ADDRESS_H
