/**
 * @file include/retdec/gui/model/code_text.h
 * @brief Line-based text documents for the read-only code views: splitting,
 *        find (forward / backward with wrap-around, as QTextDocument::find),
 *        locating a function or an address, saving.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CODE_TEXT_H
#define RETDEC_GUI_MODEL_CODE_TEXT_H

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// Position in a line-split document: 0-based line, byte column.
struct TextPos
{
	int line = 0;
	int col = 0;
	friend bool operator==(const TextPos&, const TextPos&) = default;
	friend auto operator<=>(const TextPos&, const TextPos&) = default;
};

/// [begin, end) range; begin == end is an empty range (a caret).
struct TextRange
{
	TextPos begin;
	TextPos end;
	bool empty() const { return begin == end; }
	friend bool operator==(const TextRange&, const TextRange&) = default;
};

/**
 * Splits @p text into lines the way a QTextDocument splits into blocks: one
 * line per '\n'-separated piece, so "a\nb\n" has three lines (the last empty)
 * and "" has one. A trailing '\r' on each line is dropped.
 */
std::vector<std::string> splitLines(std::string_view text);

/// Inverse of splitLines (lines joined by '\n').
std::string joinLines(const std::vector<std::string>& lines);

/// Text covered by @p range (lines joined by '\n').
std::string textInRange(const std::vector<std::string>& lines, const TextRange& range);

struct FindOptions
{
	bool forward = true;
	bool caseSensitive = false; ///< QTextDocument::find default: case-insensitive
	bool wrap = true;           ///< restart from the start / end when nothing is found
};

/**
 * Finds @p query in @p lines. Forward searches start at @p from.end (so a
 * repeated find moves past the current match), backward searches end before
 * @p from.begin. Returns the match range or nullopt. Matches do not span lines.
 */
std::optional<TextRange> findText(const std::vector<std::string>& lines, std::string_view query,
                                  const TextRange& from, const FindOptions& opt = {});

/// Number of (non-overlapping, case-insensitive) occurrences of @p query.
std::size_t countMatches(const std::vector<std::string>& lines, std::string_view query, bool caseSensitive = false);

/// First line (0-based) with a definition-like occurrence of @p name: the name
/// as a whole word followed by optional whitespace and '(' (`\bname\s*\(`).
std::optional<int> findFunctionLine(const std::vector<std::string>& lines, std::string_view name);

/// First occurrence of @p needle (case-sensitive), or nullopt.
std::optional<TextPos> findFirst(const std::vector<std::string>& lines, std::string_view needle);

/**
 * Best-effort location of a function in a full decompiled source: tries the
 * name, then "function_<hex>", then "0x<hex>" (as DecompiledCPanel did).
 */
std::optional<TextPos> locateFunction(const std::vector<std::string>& lines, uint64_t address, std::string_view name);

/// First line (0-based) containing "0x<hex address>" (case-insensitive).
std::optional<int> findAddressLine(const std::vector<std::string>& lines, uint64_t address);

/// Line (0-based) of an assembly listing whose own address is @p address
/// (listingAddress), else findAddressLine's first line mentioning it.
std::optional<int> findListingLine(const std::vector<std::string>& lines, uint64_t address);

/// Writes @p text (UTF-8) to @p path, truncating. Reports write, flush and
/// close failures, not only open failures.
std::expected<void, std::string> saveTextFile(const std::filesystem::path& path, std::string_view text);

/// Reads a whole file as text.
std::expected<std::string, std::string> loadTextFile(const std::filesystem::path& path);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CODE_TEXT_H
