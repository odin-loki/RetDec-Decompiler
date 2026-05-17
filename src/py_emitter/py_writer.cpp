/**
 * @file src/py_emitter/py_writer.cpp
 */

#include "retdec/py_emitter/py_writer.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace retdec {
namespace py_emitter {

// ─── Keywords ─────────────────────────────────────────────────────────────────

const std::unordered_set<std::string>& PyWriter::keywords() {
    static const std::unordered_set<std::string> kw = {
        "False","None","True","and","as","assert","async","await",
        "break","class","continue","def","del","elif","else","except",
        "finally","for","from","global","if","import","in","is",
        "lambda","nonlocal","not","or","pass","raise","return","try",
        "while","with","yield"
    };
    return kw;
}

bool PyWriter::isKeyword(const std::string& name) {
    return keywords().count(name) > 0;
}

bool PyWriter::isSoftKeyword(const std::string& name) {
    // match / case / type are soft keywords in 3.10+
    static const std::unordered_set<std::string> soft = {"match", "case", "type"};
    return soft.count(name) > 0;
}

std::string PyWriter::safeName(const std::string& name) {
    if (name.empty()) return "_";
    if (isKeyword(name)) return name + "_";
    // Check first char is alpha/underscore
    if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
        return "_" + name;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return "_mangled_";
    }
    return name;
}

// ─── PyWriter ────────────────────────────────────────────────────────────────

PyWriter::PyWriter(Options opts) : opts_(opts) {}

void PyWriter::writeIndent() {
    if (lineStart_) {
        for (int i = 0; i < indentLevel_ * opts_.indentWidth; ++i)
            buf_ << ' ';
        lineStart_ = false;
    }
}

void PyWriter::rawWrite(const std::string& s) {
    buf_ << s;
}

void PyWriter::indent()  { ++indentLevel_; }
void PyWriter::dedent()  { if (indentLevel_ > 0) --indentLevel_; }

void PyWriter::line(const std::string& text) {
    writeIndent();
    rawWrite(text);
    rawWrite("\n");
    lineStart_ = true;
}

void PyWriter::blank() {
    rawWrite("\n");
    lineStart_ = true;
}

void PyWriter::write(const std::string& text) {
    writeIndent();
    rawWrite(text);
}

void PyWriter::nl() {
    rawWrite("\n");
    lineStart_ = true;
}

void PyWriter::comment(const std::string& text) {
    line("# " + text);
}

// ─── String literal escaping ─────────────────────────────────────────────────

std::string PyWriter::escapeStr(const std::string& s, char quote) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == static_cast<unsigned char>(quote)) {
            out += '\\'; out += static_cast<char>(quote);
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 32 || c == 127) {
            std::ostringstream ss;
            ss << "\\x" << std::hex << std::setw(2) << std::setfill('0') << (int)c;
            out += ss.str();
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string PyWriter::strLiteral(const std::string& s) const {
    char q = opts_.useDoubleQuotes ? '"' : '\'';
    return std::string(1, q) + escapeStr(s, q) + q;
}

std::string PyWriter::bytesLiteral(const std::string& s) const {
    char q = opts_.useDoubleQuotes ? '"' : '\'';
    return "b" + std::string(1, q) + escapeStr(s, q) + q;
}

std::string PyWriter::fmtLiteral(const std::string& s) const {
    char q = opts_.useDoubleQuotes ? '"' : '\'';
    return "f" + std::string(1, q) + escapeStr(s, q) + q;
}

} // namespace py_emitter
} // namespace retdec
