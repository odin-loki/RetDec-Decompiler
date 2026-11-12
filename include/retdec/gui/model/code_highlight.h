/**
 * @file include/retdec/gui/model/code_highlight.h
 * @brief Syntax tokenizer for the code views (assembly, LLVM IR / SSA, C and
 *        the other decompiler output languages).
 *
 * Pure logic, no UI: a view asks for the token spans of one line and paints
 * them in its palette. Multi-line state (C-style block comments) is carried
 * from line to line through an int, as QSyntaxHighlighter's block state was.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CODE_HIGHLIGHT_H
#define RETDEC_GUI_MODEL_CODE_HIGHLIGHT_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// Languages a code view can colour. Asm and IR are the tri-pane's left panes;
/// the rest are the decompiler's output languages (Settings -> Output language).
enum class CodeLang : uint8_t
{
	Plain,
	Asm,
	IR,
	C,
	Cxx,
	CSharp,
	Java,
	Python,
	Lua,
};

enum class TokenKind : uint8_t
{
	Text,
	Keyword,      ///< language keyword / IR instruction
	Type,         ///< type names (uint32_t, i32, ...)
	String,       ///< string and character literals
	Number,       ///< numeric literals, asm immediates
	Comment,      ///< line and block comments
	Preprocessor, ///< #include, #define, ...
	Register,     ///< asm registers
	Mnemonic,     ///< asm instruction mnemonic
	Address,      ///< hex addresses (asm 0x..., listing address prefix)
	Label,        ///< asm / IR labels, IR basic-block headers
	SsaName,      ///< IR %values and @globals
	Builtin,      ///< Python builtins
};

/// Number of TokenKind values (palette arrays are indexed by TokenKind).
inline constexpr std::size_t kTokenKindCount = 13;

struct TokenSpan
{
	uint32_t start = 0;  ///< byte offset in the line
	uint32_t length = 0; ///< bytes
	TokenKind kind = TokenKind::Text;
	friend bool operator==(const TokenSpan&, const TokenSpan&) = default;
};

/// Line state carried between lines: 0 = normal, 1 = inside a block comment.
inline constexpr int kStateNormal = 0;
inline constexpr int kStateInBlockComment = 1;

/**
 * Tokenizes @p line. @p state is the state at the start of the line (the
 * value returned for the previous line, kStateNormal for the first) and is
 * updated to the state at the end of the line. Returned spans are sorted,
 * non-overlapping and cover only non-Text tokens.
 */
std::vector<TokenSpan> highlightLine(CodeLang lang, std::string_view line, int& state);

/// Convenience: tokenizes every line of @p lines, threading the block state.
std::vector<std::vector<TokenSpan>> highlightLines(CodeLang lang, const std::vector<std::string>& lines);

/// Token kind of the byte at @p column, Text when no span covers it.
TokenKind tokenAt(const std::vector<TokenSpan>& spans, uint32_t column);

/// True when @p lang has C-style block comments (C, C++, C#, Java).
bool hasBlockComments(CodeLang lang);

/// Maps the settings value of Decompiler -> Output language ("c", "cpp",
/// "c++", "csharp", "cs", "java", "python", "py", "lua"; case-insensitive,
/// trimmed) to a code language; anything else is C.
CodeLang codeLangFromSettingsString(std::string_view s);

/// Display name of an output language as the tri-pane language combo shows it
/// ("C", "C++", "C#", "Java", "Python", "Lua"; "Assembly", "IR", "Text").
std::string_view codeLangName(CodeLang lang);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CODE_HIGHLIGHT_H
