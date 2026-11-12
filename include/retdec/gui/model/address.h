/**
 * @file include/retdec/gui/model/address.h
 * @brief Address formatting and parsing shared by every panel (context menus,
 *        "Go to address", log lines).
 *
 * Port of the non-UI part of the Qt GUI's address_context_menu.h.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_ADDRESS_H
#define RETDEC_GUI_MODEL_ADDRESS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace retdec::gui::model {

/// Context-menu labels used by every panel that shows addresses.
inline constexpr const char* kCopyAddressLabel = "Copy Address";
inline constexpr const char* kGoToFunctionLabel = "Go to Function";

/// "0x401000": 0x-prefixed lowercase hex, no padding (same as hexAddress()).
std::string formatAddressHex(uint64_t address);

/// First `0x…` token on @p line, if any (case-insensitive hex digits).
/// Nothing when there is none or it does not fit in 64 bits.
std::optional<uint64_t> parseFirstAddress(std::string_view line);

/// Parse a whole token as an address: `0x`/`0X` prefix = hex, otherwise
/// decimal. Surrounding whitespace is ignored; anything else makes it fail.
/// This is how the decompiler's config.json addresses are read.
std::optional<uint64_t> parseAddress(std::string_view text);

/// Put formatAddressHex(@p address) on the system clipboard (forge::os::clipboard).
/// False when no clipboard is available (headless, no helper installed).
bool copyAddressToClipboard(uint64_t address);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_ADDRESS_H
