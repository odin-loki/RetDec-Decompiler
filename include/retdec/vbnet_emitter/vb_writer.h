/**
 * @file include/retdec/vbnet_emitter/vb_writer.h
 * @brief Low-level VB.NET source writer.
 *
 * VB.NET uses:
 *   - 4-space indentation
 *   - End-block keywords (End Class, End Sub, etc.)
 *   - Double-quoted strings (no escape chars; "" for literal quote)
 *   - Case-insensitive keywords (we emit Title Case)
 *   - Line continuation with _ (not needed here; we don't break long lines)
 *   - Single-quote ' for comments
 */

#ifndef RETDEC_VBNET_EMITTER_VB_WRITER_H
#define RETDEC_VBNET_EMITTER_VB_WRITER_H

#include <sstream>
#include <string>
#include <unordered_set>

namespace retdec {
namespace vbnet_emitter {

class VbWriter {
public:
    struct Options {
        int indentWidth = 4;
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit VbWriter(Options opts = defaultOptions());

    void indent();
    void dedent();
    void line(const std::string& text);    ///< Indented line + \n
    void blank();                           ///< Blank line
    void comment(const std::string& text); ///< ' comment
    void xmlDoc(const std::string& text);  ///< ''' XML doc
    void write(const std::string& text);   ///< Raw append
    void nl();

    int indentLevel() const { return indentLevel_; }

    /// Escape a VB.NET identifier (wrap in [] if keyword).
    static std::string safeName(const std::string& name);
    static bool isKeyword(const std::string& name);
    static const std::unordered_set<std::string>& keywords();

    /// VB.NET string literal: double-quote delimited, "" for embedded quote.
    static std::string strLiteral(const std::string& s);

    std::string str() const { return buf_.str(); }
    void reset() { buf_.str(""); buf_.clear(); indentLevel_ = 0; lineStart_ = true; }

private:
    Options opts_;
    std::ostringstream buf_;
    int  indentLevel_ = 0;
    bool lineStart_   = true;

    void writeIndent();
};

} // namespace vbnet_emitter
} // namespace retdec

#endif // RETDEC_VBNET_EMITTER_VB_WRITER_H
