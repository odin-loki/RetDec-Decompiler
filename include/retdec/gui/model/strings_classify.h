/**
 * @file include/retdec/gui/model/strings_classify.h
 * @brief String category detection and numeric-constant labelling for the
 *        Strings & Constants panel (port of the Qt CategoryDetector and
 *        ConstantLabeler).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_STRINGS_CLASSIFY_H
#define RETDEC_GUI_MODEL_STRINGS_CLASSIFY_H

#include "retdec/gui/model/types.h"

#include <string>
#include <string_view>

namespace retdec::gui::model {

/// Classifies a string literal. Priority: crypto, URL, file path, printf
/// format, regular expression, plain.
class CategoryDetector
{
public:
	static StringCategory classify(std::string_view value);

	/// printf-family conversion (`%d`, `%-08.3f`, `%%`, ...).
	static bool isFormatString(std::string_view v);
	/// http/https/ftp/ftps/file/ws/wss scheme (case-insensitive).
	static bool isURL(std::string_view v);
	/// Unix absolute, drive letter, UNC, well-known system prefix, or a path
	/// separator plus a known file extension.
	static bool isFilePath(std::string_view v);
	/// Three or more regex metacharacters.
	static bool isRegEx(std::string_view v);
	/// Hash / cipher / PEM keywords (case-insensitive).
	static bool isCryptoConst(std::string_view v);
};

/// Fills hexStr (bare upper-case hex, zero padded to the constant's width),
/// decStr, label and labelKind of a constant. Priority: magic number, crypto
/// constant, IEEE-754 special, port number.
class ConstantLabeler
{
public:
	static void label(ConstantEntry& entry);

	static std::string lookupMagic(uint64_t val, ConstantSize sz);
	static std::string lookupCrypto(uint64_t val);
	static std::string lookupFloat(uint64_t val, ConstantSize sz);
	static std::string lookupPort(uint64_t val);
};

/// Column text of a string type ("ASCII", "UTF-8", "UTF-16 LE", ...).
std::string_view stringTypeName(StringType t);
/// Column text of a category ("Plain", "FormatStr", "URL", "FilePath", "RegEx", "Crypto").
std::string_view stringCategoryName(StringCategory c);
/// Short name of a constant label kind as the label filter lists it
/// ("All" for None).
std::string_view constantLabelKindName(ConstantLabel k);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_STRINGS_CLASSIFY_H
