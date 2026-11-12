/**
 * @file include/retdec/gui/model/funclist_export.h
 * @brief CSV / JSON text of the function list, byte-for-byte what the Qt
 *        panel wrote, and the indented JSON writer it (and the progress
 *        export) use.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_FUNCLIST_EXPORT_H
#define RETDEC_GUI_MODEL_FUNCLIST_EXPORT_H

#include "retdec/gui/model/types.h"

#include <forge/json/json.hpp>

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/**
 * JSON text in the layout QJsonDocument::toJson(Indented) produces: object
 * keys sorted, four-space indent, `"key": value`, a newline after the top
 * level value; integral numbers without a fraction, others in the shortest
 * form that reads back exactly.
 */
std::string funclistIndentedJson(const forge::json::value& v);

/// "Address,Name,SizeBytes,Instrs,Confidence,CC,STL,Crypto,Algo,Design,Lib,Notes,Tags"
/// header, then one line per function (fields are not quoted, as in Qt).
std::string functionListCsv(std::span<const FunctionEntry* const> rows);
/// The array the Qt "Export JSON" wrote (address, name, rawName, signature,
/// sizeBytes, instrCount, confidence, confidenceDetail, patterns, cc, notes, tags).
forge::json::value functionListJsonValue(std::span<const FunctionEntry* const> rows);
std::string functionListJson(std::span<const FunctionEntry* const> rows);

/// Writes @p text to @p path (forge::os::write_file). Error text on failure.
std::expected<void, std::string> funclistWriteTextFile(const std::string& path, std::string_view text);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_FUNCLIST_EXPORT_H
