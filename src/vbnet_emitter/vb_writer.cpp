/**
 * @file src/vbnet_emitter/vb_writer.cpp
 */

#include "retdec/vbnet_emitter/vb_writer.h"

#include <algorithm>
#include <cctype>

namespace retdec {
namespace vbnet_emitter {

// ─── Keywords ────────────────────────────────────────────────────────────────

const std::unordered_set<std::string>& VbWriter::keywords() {
    static const std::unordered_set<std::string> kw = {
        "addhandler","addressof","alias","and","andalso","as",
        "boolean","byref","byte","byval",
        "call","case","catch","cbool","cbyte","cchar","cdate","cdbl",
        "cdec","char","cint","class","clng","cobj","const","continue",
        "csbyte","cshort","csng","cstr","ctype","cuint","culng","cushort",
        "date","decimal","declare","default","delegate","dim","directcast",
        "do","double",
        "each","else","elseif","end","endif","enum","erase","error",
        "event","exit",
        "false","finally","for","friend","function",
        "get","gettype","getxmlnamespace","global","gosub","goto",
        "handles",
        "if","implements","imports","in","inherits","integer","interface",
        "is","isnot",
        "let","lib","like","long","loop",
        "me","mod","module","mustinherit","mustoverride","mybase","myclass",
        "namespace","narrowing","new","next","not","nothing","notinheritable",
        "notoverridable",
        "object","of","on","operator","option","optional","or","orelse",
        "out","overloads","overridable","overrides",
        "paramarray","partial","private","property","protected","public",
        "raiseevent","readonly","redim","rem","removehandler","resume",
        "return",
        "sbyte","select","set","shadows","shared","short","single","static",
        "step","stop","string","structure","sub","synclock",
        "then","throw","to","true","try","trycast","typeof",
        "uinteger","ulong","ushort","using",
        "variant",
        "when","while","widening","with","withevents","writeonly",
        "xor"
    };
    return kw;
}

bool VbWriter::isKeyword(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return keywords().count(lower) > 0;
}

std::string VbWriter::safeName(const std::string& name) {
    if (name.empty()) return "_";
    if (isKeyword(name)) return "[" + name + "]";
    if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
        return "_" + name;
    return name;
}

// ─── VbWriter ────────────────────────────────────────────────────────────────

VbWriter::VbWriter(Options opts) : opts_(opts) {}

void VbWriter::writeIndent() {
    if (lineStart_) {
        for (int i = 0; i < indentLevel_ * opts_.indentWidth; ++i)
            buf_ << ' ';
        lineStart_ = false;
    }
}

void VbWriter::indent()  { ++indentLevel_; }
void VbWriter::dedent()  { if (indentLevel_ > 0) --indentLevel_; }

void VbWriter::line(const std::string& text) {
    writeIndent();
    buf_ << text << '\n';
    lineStart_ = true;
}

void VbWriter::blank() {
    buf_ << '\n';
    lineStart_ = true;
}

void VbWriter::comment(const std::string& text) {
    line("' " + text);
}

void VbWriter::xmlDoc(const std::string& text) {
    line("''' " + text);
}

void VbWriter::write(const std::string& text) {
    writeIndent();
    buf_ << text;
    lineStart_ = false;
}

void VbWriter::nl() {
    buf_ << '\n';
    lineStart_ = true;
}

// ─── String literal ──────────────────────────────────────────────────────────

std::string VbWriter::strLiteral(const std::string& s) {
    // VB.NET: double-quote delimited; "" for embedded quote
    // No other escape sequences — use Chr() for special chars
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else if (c == '\n') { out += "\" & vbLf & \""; }
        else if (c == '\r') { out += "\" & vbCr & \""; }
        else if (c == '\t') { out += "\" & vbTab & \""; }
        else out += c;
    }
    return out + "\"";
}

} // namespace vbnet_emitter
} // namespace retdec
