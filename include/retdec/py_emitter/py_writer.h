/**
 * @file include/retdec/py_emitter/py_writer.h
 * @brief Low-level Python source code writer with indentation management.
 *
 * Handles:
 *   - 4-space indentation (PEP 8)
 *   - 88-character line length (Black-compatible)
 *   - String literal escaping (single, double, triple quotes, raw, bytes)
 *   - Keyword detection for safe identifiers
 *   - Comment emission
 *   - Operator precedence constants
 */

#ifndef RETDEC_PY_EMITTER_PY_WRITER_H
#define RETDEC_PY_EMITTER_PY_WRITER_H

#include <sstream>
#include <string>
#include <unordered_set>

namespace retdec {
namespace py_emitter {

/// Python operator precedence levels (higher = tighter binding).
enum class PyPrec : int {
    None_    =  0,
    Lambda   =  5,
    IfExp    = 10,   // x if c else y
    Or       = 15,
    And      = 20,
    Not      = 25,
    Compare  = 30,
    BitOr    = 35,
    BitXor   = 40,
    BitAnd   = 45,
    Shift    = 50,
    AddSub   = 55,
    MulDiv   = 60,
    Unary    = 65,
    Pow      = 70,   // right-associative
    Await    = 75,
    Atom     = 80,
};

class PyWriter {
public:
    struct Options {
        int  indentWidth  = 4;
        int  maxLineLen   = 88;
        bool useDoubleQuotes = true;
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit PyWriter(Options opts = defaultOptions());

    // ── Output control ──────────────────────────────────────────────────────
    void indent();
    void dedent();
    void line(const std::string& text);       ///< Emit indented line + \n
    void blank();                              ///< Emit blank line
    void write(const std::string& text);       ///< Append raw text
    void nl();                                 ///< Emit newline
    void comment(const std::string& text);     ///< Emit # comment line

    int  indentLevel() const { return indentLevel_; }

    // ── String literals ─────────────────────────────────────────────────────
    std::string strLiteral(const std::string& s) const;
    std::string bytesLiteral(const std::string& s) const;
    std::string fmtLiteral(const std::string& s) const; ///< f"..." prefix

    // ── Safe names ──────────────────────────────────────────────────────────
    static std::string safeName(const std::string& name);
    static bool isKeyword(const std::string& name);
    static bool isSoftKeyword(const std::string& name); ///< match/case/type 3.10+
    static const std::unordered_set<std::string>& keywords();

    // ── Buffer access ───────────────────────────────────────────────────────
    std::string str() const { return buf_.str(); }
    void reset() { buf_.str(""); buf_.clear(); indentLevel_ = 0; lineStart_ = true; }

private:
    Options opts_;
    std::ostringstream buf_;
    int  indentLevel_ = 0;
    bool lineStart_   = true;

    void writeIndent();
    void rawWrite(const std::string& s);

    static std::string escapeStr(const std::string& s, char quote);
};

} // namespace py_emitter
} // namespace retdec

#endif // RETDEC_PY_EMITTER_PY_WRITER_H
