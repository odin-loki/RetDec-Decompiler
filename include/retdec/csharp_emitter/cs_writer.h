/**
 * @file include/retdec/csharp_emitter/cs_writer.h
 * @brief Low-level C# source code writer with indentation, brace tracking,
 *        and token-level formatting helpers.
 *
 * ## Design
 *
 * `CsWriter` is a dumb but precise text-formatting engine.  Higher-level
 * emitters (type, statement, expression) build on it.  It never makes
 * semantic decisions — it only ensures:
 *
 *   - Consistent indentation (4 spaces by default, configurable)
 *   - Correct newline handling (LF or CRLF)
 *   - Brace pairing with automatic indent tracking
 *   - Line-length–aware trailing-comment placement
 *   - XML-doc comment emission
 *   - String and character literal escaping for C#
 *   - Verbatim string (`@"..."`) and raw string literal (`"""..."""`) selection
 *   - Keyword detection (reserved words that need `@` prefix when used as names)
 *
 * ## Usage
 *
 * ```cpp
 * CsWriter w;
 * w.line("using System;");
 * w.blank();
 * w.line("namespace MyApp");
 * {
 *     auto g = w.block();          // writes "{", increases indent
 *     w.line("class Foo");
 *     {
 *         auto g2 = w.block();
 *         w.line("int x = 42;");
 *     }                            // destructor writes "}"
 * }                                // destructor writes "}"
 * std::string src = w.str();
 * ```
 */

#ifndef RETDEC_CSHARP_EMITTER_CS_WRITER_H
#define RETDEC_CSHARP_EMITTER_CS_WRITER_H

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace csharp_emitter {

// ─── CsWriter ────────────────────────────────────────────────────────────────

/**
 * @brief C# source-code writer.
 *
 * All output is buffered in memory and flushed to a `std::string` via `str()`.
 */
class CsWriter {
public:
    struct Options {
        std::string indentStr  = "    ";  ///< 4 spaces (standard C# convention)
        std::string lineEnding = "\n";    ///< LF (change to "\r\n" for Windows)
        int  commentColumn     = 60;      ///< Column for trailing //comments
        bool compactBraces     = false;   ///< "{ }" on same line for one-liners
        bool emitXmlDoc        = true;    ///< Emit <summary> / <param> docs
        int  csharpVersion     = 12;      ///< Target language version (8–12)
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit CsWriter(Options opts = defaultOptions());

    // ── Core write primitives ─────────────────────────────────────────────────

    /// Write `text` then a newline, with current indentation prepended.
    void line(std::string_view text);

    /// Write a blank line.
    void blank();

    /// Write `text` without a newline (used to build partial lines).
    void write(std::string_view text);

    /// Finish the current partial line with a newline.
    void nl();

    // ── Brace / scope management ──────────────────────────────────────────────

    /// RAII guard: writes "{" and increases indent on construction,
    /// writes "}" and decreases indent on destruction.
    class BlockGuard {
    public:
        explicit BlockGuard(CsWriter& w, std::string_view suffix = "");
        ~BlockGuard();
        BlockGuard(const BlockGuard&) = delete;
        BlockGuard& operator=(const BlockGuard&) = delete;
        BlockGuard(BlockGuard&& o) noexcept;
    private:
        CsWriter* w_;
        std::string suffix_;  ///< Text after "}" (e.g., ";" for anonymous types)
        bool moved_ = false;
    };

    /// Open a brace block: write "{", indent, return guard.
    [[nodiscard]] BlockGuard block(std::string_view suffix = "");

    /// Open a brace block with preceding content on the same line.
    /// e.g.:  writeLine("if (x)"); then block()  →  "if (x)\n{\n"
    void openBrace();

    /// Close the current brace block.
    void closeBrace(std::string_view suffix = "");

    /// Manually increase indentation.
    void indent();

    /// Manually decrease indentation.
    void dedent();

    // ── Comment helpers ───────────────────────────────────────────────────────

    /// Emit a single-line comment: `// text`.
    void comment(std::string_view text);

    /// Emit an XML doc summary block.
    void xmlDoc(std::string_view summary,
                const std::vector<std::pair<std::string,std::string>>& params = {},
                std::string_view returns = "");

    /// Append a trailing comment to the current line (moves to commentColumn).
    void trailingComment(std::string_view text);

    // ── Keyword / name helpers ────────────────────────────────────────────────

    /// If `name` is a C# reserved word, return `@name`, else return `name`.
    static std::string safeName(const std::string& name);

    /// True if the string is a C# keyword.
    static bool isKeyword(const std::string& name);

    // ── String literal helpers ────────────────────────────────────────────────

    /// Produce a C# string literal for `s`.
    /// Uses verbatim `@"..."` if the string contains backslashes or newlines.
    /// Uses raw `"""..."""` (C# 11+) for multi-line content when csharpVersion >= 11.
    std::string stringLiteral(const std::string& s) const;

    /// Produce a C# char literal for `c`.
    static std::string charLiteral(uint32_t codepoint);

    // ── Type name helpers ─────────────────────────────────────────────────────

    /// Convert a CLR fully-qualified name to its C# alias or short form.
    /// e.g.: "System.Int32" → "int", "System.String" → "string", …
    static std::string clrToCsharpType(const std::string& clrName);

    /// C# built-in type aliases.
    static const std::unordered_set<std::string>& builtinAliases();

    // ── Modifier helpers ──────────────────────────────────────────────────────

    /// Emit access modifier keywords in canonical C# order.
    void writeModifiers(const std::string& modStr);

    // ── Output ────────────────────────────────────────────────────────────────

    /// Return the full buffered source.
    std::string str() const { return buf_.str(); }

    /// Clear the buffer and reset indentation.
    void reset();

    int  indentLevel() const { return indentLevel_; }
    const Options& options() const { return opts_; }

private:
    Options      opts_;
    std::ostringstream buf_;
    int          indentLevel_ = 0;
    bool         lineStart_   = true;  ///< Next write needs indent prefix

    void writeIndent();
    void rawWrite(std::string_view s);
};

// ─── Helpers ──────────────────────────────────────────────────────────────────

/// Escape a string for C# (handles \n, \r, \t, \0, ", \, unicode)
std::string escapeCsharpString(const std::string& s);


} // namespace csharp_emitter
} // namespace retdec

#endif // RETDEC_CSHARP_EMITTER_CS_WRITER_H
