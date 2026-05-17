/**
 * @file include/retdec/fsharp_emitter/fs_writer.h
 * @brief Low-level F# source writer with indentation and keyword management.
 *
 * F# uses indentation-based syntax. This writer enforces 4-space indentation,
 * handles F# keyword escaping (backtick quoting), and manages blank-line
 * conventions between top-level declarations.
 */

#ifndef RETDEC_FSHARP_EMITTER_FS_WRITER_H
#define RETDEC_FSHARP_EMITTER_FS_WRITER_H

#include <sstream>
#include <string>
#include <unordered_set>

namespace retdec {
namespace fsharp_emitter {

class FsWriter {
public:
    struct Options {
        int  indentWidth    = 4;
        bool useDoubleQuotes= true;  ///< F# uses double-quoted strings
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit FsWriter(Options opts = defaultOptions());

    void indent();
    void dedent();
    void line(const std::string& text);   ///< Indented line + \n
    void blank();                          ///< Blank line
    void comment(const std::string& text); ///< // comment
    void blockComment(const std::string& text); ///< /// XML doc comment
    void write(const std::string& text);   ///< Raw append (no indent)
    void nl();

    int indentLevel() const { return indentLevel_; }

    /// Escape an identifier that clashes with an F# keyword using backticks.
    static std::string safeName(const std::string& name);
    static bool isKeyword(const std::string& name);
    static const std::unordered_set<std::string>& keywords();

    /// Produce a string literal "..." with F# escaping.
    std::string strLiteral(const std::string& s) const;
    /// Produce a char literal 'c'.
    static std::string charLiteral(char c);

    std::string str() const { return buf_.str(); }
    void reset() { buf_.str(""); buf_.clear(); indentLevel_ = 0; lineStart_ = true; }

private:
    Options opts_;
    std::ostringstream buf_;
    int  indentLevel_ = 0;
    bool lineStart_   = true;

    void writeIndent();
    static std::string escapeStr(const std::string& s);
};

} // namespace fsharp_emitter
} // namespace retdec

#endif // RETDEC_FSHARP_EMITTER_FS_WRITER_H
