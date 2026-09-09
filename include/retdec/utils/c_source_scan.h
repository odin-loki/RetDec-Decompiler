/**
 * @file include/retdec/utils/c_source_scan.h
 * @brief Blanking comments and literals in C source, without indexing past the end.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Several checks in this tree want to count things in C source and must not
 * count what appears inside a comment or a string literal. The neural
 * refinement gate is the one that matters: it compares the control-flow shape
 * of the original and refined function, and counting over raw text lets a
 * refinement introduce a real system() call while deleting a `/ * system * /`
 * comment, so the counts cancel and the call passes.
 *
 * Blanking is the cheap way to do that without a parser: replace the contents
 * of comments and literals with spaces, keeping length and newlines so offsets
 * and line numbers still line up, then count over the result.
 *
 * The scanner reads one character ahead, which is exactly where this kind of
 * loop goes wrong. It is header-only and free of everything but <cstddef>, so
 * tests/verification/ can prove it never indexes outside either buffer, for all
 * inputs up to the modelled length.
 */

#ifndef RETDEC_UTILS_C_SOURCE_SCAN_H
#define RETDEC_UTILS_C_SOURCE_SCAN_H

#include <cstddef>

namespace retdec {
namespace utils {
namespace source_scan {

/// What the scanner is inside at a given character.
enum class Ctx
{
	Code,
	LineComment,
	BlockComment,
	StringLit,
	CharLit
};

/// Copy @p n characters from @p in to @p out, replacing the contents of
/// comments and string/character literals with spaces.
///
/// Newlines are preserved everywhere, including inside block comments, so line
/// numbers computed from the result still match the input. Literal delimiters
/// are kept; only what is between them is blanked.
///
/// @p in and @p out must both be at least @p n characters and must not overlap.
/// A null pointer or a zero length is a no-op rather than an error.
inline void blankNonCode(const char* in, std::size_t n, char* out) noexcept
{
	if (in == nullptr || out == nullptr) return;

	Ctx ctx = Ctx::Code;
	bool escaped = false;

	for (std::size_t i = 0; i < n; ++i)
	{
		const char c = in[i];
		// Reading one ahead is only defined while there is a next character;
		// at the last one, treat the lookahead as a terminator. Every
		// two-character token below is guarded by this, so none of them can
		// step past the end.
		const bool haveNext = i + 1 < n;
		const char next = haveNext ? in[i + 1] : '\0';

		out[i] = c;

		switch (ctx)
		{
		case Ctx::Code:
			if (haveNext && c == '/' && next == '/')
			{
				ctx = Ctx::LineComment;
				out[i] = ' ';
				out[i + 1] = ' ';
				++i;
			}
			else if (haveNext && c == '/' && next == '*')
			{
				ctx = Ctx::BlockComment;
				out[i] = ' ';
				out[i + 1] = ' ';
				++i;
			}
			else if (c == '"')
			{
				ctx = Ctx::StringLit;
				escaped = false;
			}
			else if (c == '\'')
			{
				ctx = Ctx::CharLit;
				escaped = false;
			}
			break;

		case Ctx::LineComment:
			// A backslash at end of line continues the comment onto the
			// next, exactly as the preprocessor sees it.
			if (c == '\n' && !(i > 0 && in[i - 1] == '\\'))
				ctx = Ctx::Code;
			else if (c != '\n')
				out[i] = ' ';
			break;

		case Ctx::BlockComment:
			if (haveNext && c == '*' && next == '/')
			{
				out[i] = ' ';
				out[i + 1] = ' ';
				++i;
				ctx = Ctx::Code;
			}
			else if (c != '\n')
			{
				out[i] = ' ';
			}
			break;

		case Ctx::StringLit:
		case Ctx::CharLit: {
			const char closer = (ctx == Ctx::StringLit) ? '"' : '\'';
			if (escaped)
			{
				escaped = false;
				if (c != '\n') out[i] = ' ';
			}
			else if (c == '\\')
			{
				escaped = true;
				out[i] = ' ';
			}
			else if (c == closer)
			{
				ctx = Ctx::Code; // keep the delimiter itself
			}
			else if (c == '\n')
			{
				// C ends an unterminated literal at the end of the line; it
				// does not run to end of file. Running to EOF meant one stray
				// apostrophe blanked everything after it -- and an apostrophe
				// inside an `#if 0` block, which a compiler skips entirely, is
				// enough. The spawn-call gate counts identifiers over this
				// output, so a refinement could hide a system() call behind
				// one: `#if 0 / doesn't run / #endif` then system(...), and
				// the count came back unchanged.
				//
				// The newline itself is kept, as everywhere else here, so line
				// numbers computed from the result still match the input.
				ctx = Ctx::Code;
			}
			else
			{
				out[i] = ' ';
			}
			break;
		}

		// Unreachable: every Ctx enumerator has a case above. It is here
		// because the project compiles with -Wswitch-default (CMakeLists.txt)
		// and src/neural adds -Werror, so a switch without one does not build
		// there -- and src/neural/gates.cpp includes this header, which made
		// retdec-neural, an option that is ON by default, fail to compile:
		//
		//   c_source_scan.h:70:24: error: switch missing default case
		//   [-Werror=switch-default]
		//
		// The cost is that -Wswitch no longer flags a new enumerator here.
		// Anything added to Ctx must be given a case above; the default
		// leaves the character as copied, which is the Code reading.
		default: break;
		}
	}
}

} // namespace source_scan
} // namespace utils
} // namespace retdec

#endif
