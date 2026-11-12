/**
 * @file include/retdec/gui/model/diff_engine.h
 * @brief Line diff of two texts (port of the Qt MyersDiff / DiffResult) plus
 *        intra-line (character) diffs for changed line pairs.
 *
 * The line diff produces exactly the edit script the Qt panel did: the shared
 * head and tail are trimmed in O(N+M), the middle is diffed with an LCS table
 * (minimal script; ties prefer the deletion) bounded to kMaxDiffCells cells,
 * past which the middle is reported as a wholesale replacement and
 * DiffResult::exact is false.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DIFF_ENGINE_H
#define RETDEC_GUI_MODEL_DIFF_ENGINE_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

enum class DiffOpKind
{
	Equal,
	Insert,
	Delete
};

struct DiffOp
{
	DiffOpKind kind = DiffOpKind::Equal;
	std::string line;
	int leftLine = -1;  ///< 0-based line in the left (before) text, -1 for Insert
	int rightLine = -1; ///< 0-based line in the right (after) text, -1 for Delete
};

struct DiffResult
{
	std::vector<DiffOp> ops;
	int linesAdded = 0;
	int linesRemoved = 0;
	int linesEqual = 0;
	double similarity = 0.0; ///< equal / all ops, 1.0 for two empty texts
	/// False when the differing region was too large to diff exactly.
	bool exact = true;

	bool isEmpty() const
	{
		return ops.empty();
	}

	/// Unified diff (--- / +++ / @@ hunks with @p context lines; hunks closer
	/// than 2*context merge).
	std::string toUnifiedDiff(
		const std::string& leftName = "before", const std::string& rightName = "after", int context = 3) const;
	/// Stand-alone HTML report (one coloured span per op, text escaped).
	std::string toHtml() const;
};

/// LCS table ceiling (cells); about 100 MB.
inline constexpr std::size_t kMaxDiffCells = 25000000;

class MyersDiff {
public:
	static DiffResult diff(const std::vector<std::string>& left, const std::vector<std::string>& right);
	/// Splits both texts into lines (std::getline: a trailing newline adds no
	/// empty line) and diffs them.
	static DiffResult diffText(const std::string& leftText, const std::string& rightText);
	static std::vector<std::string> splitLines(const std::string& text);
};

/// A byte range [begin, end) of a line that differs from its counterpart.
struct DiffSpan
{
	std::size_t begin = 0;
	std::size_t end = 0;
	friend bool operator==(const DiffSpan&, const DiffSpan&) = default;
};

/// Character-level difference of two versions of one line: the spans of each
/// that are not part of their longest common subsequence (UTF-8 code points;
/// adjacent changed characters merge into one span). Lines longer than
/// @p maxChars each are compared by common prefix / suffix only.
struct IntraLineDiff
{
	std::vector<DiffSpan> left;
	std::vector<DiffSpan> right;
};
IntraLineDiff intraLineDiff(std::string_view left, std::string_view right, std::size_t maxChars = 2000);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DIFF_ENGINE_H
