/**
 * @file src/fsharp_emitter/fs_writer.cpp
 */

#include "retdec/fsharp_emitter/fs_writer.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace retdec {
namespace fsharp_emitter {

// ─── Keywords ────────────────────────────────────────────────────────────────

const std::unordered_set<std::string>& FsWriter::keywords() {
    static const std::unordered_set<std::string> kw = {
        "abstract","and","as","assert","asr","base","begin","class",
        "default","delegate","do","done","downcast","downto","elif",
        "else","end","exception","extern","false","finally","fixed",
        "for","fun","function","global","if","in","inherit","inline",
        "interface","internal","land","lazy","let","lor","lsl","lsr",
        "lxor","match","member","mod","module","mutable","namespace",
        "new","not","null","of","open","or","override","private",
        "public","rec","return","seq","sig","static","struct","then",
        "to","true","try","type","upcast","use","val","void","when",
        "while","with","yield","done","method","object","virtual"
    };
    return kw;
}

bool FsWriter::isKeyword(const std::string& name) {
    return keywords().count(name) > 0;
}

std::string FsWriter::safeName(const std::string& name) {
    if (name.empty()) return "_";
    if (isKeyword(name)) return "``" + name + "``";
    // Check first char
    if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
        return "``" + name + "``";
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '\'')
            return "``" + name + "``";
    return name;
}

// ─── FsWriter ────────────────────────────────────────────────────────────────

FsWriter::FsWriter(Options opts) : opts_(opts) {}

void FsWriter::writeIndent() {
    if (lineStart_) {
        for (int i = 0; i < indentLevel_ * opts_.indentWidth; ++i)
            buf_ << ' ';
        lineStart_ = false;
    }
}

void FsWriter::indent()  { ++indentLevel_; }
void FsWriter::dedent()  { if (indentLevel_ > 0) --indentLevel_; }

void FsWriter::line(const std::string& text) {
    writeIndent();
    buf_ << text << '\n';
    lineStart_ = true;
}

void FsWriter::blank() {
    buf_ << '\n';
    lineStart_ = true;
}

void FsWriter::comment(const std::string& text) {
    line("// " + text);
}

void FsWriter::blockComment(const std::string& text) {
    line("/// " + text);
}

void FsWriter::write(const std::string& text) {
    writeIndent();
    buf_ << text;
    lineStart_ = false;
}

void FsWriter::nl() {
    buf_ << '\n';
    lineStart_ = true;
}

// ─── String literals ─────────────────────────────────────────────────────────

std::string FsWriter::escapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32 || c == 127) {
            std::ostringstream ss;
            ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
            out += ss.str();
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string FsWriter::strLiteral(const std::string& s) const {
    return "\"" + escapeStr(s) + "\"";
}

std::string FsWriter::charLiteral(char c) {
    std::string s = "'";
    if (c == '\'') s += "\\'";
    else if (c == '\\') s += "\\\\";
    else s += c;
    return s + "'";
}

} // namespace fsharp_emitter
} // namespace retdec
