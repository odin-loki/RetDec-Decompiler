/**
 * @file src/csharp_emitter/cs_writer.cpp
 * @brief C# source-code writer implementation.
 */

#include "retdec/csharp_emitter/cs_writer.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace retdec {
namespace csharp_emitter {

// ─── C# keywords (ECMA-334 §6.4.4 + contextual keywords) ─────────────────────

static const std::unordered_set<std::string> kKeywords = {
    "abstract", "as", "base", "bool", "break", "byte", "case", "catch",
    "char", "checked", "class", "const", "continue", "decimal", "default",
    "delegate", "do", "double", "else", "enum", "event", "explicit", "extern",
    "false", "finally", "fixed", "float", "for", "foreach", "goto", "if",
    "implicit", "in", "int", "interface", "internal", "is", "lock", "long",
    "namespace", "new", "null", "object", "operator", "out", "override",
    "params", "private", "protected", "public", "readonly", "ref", "return",
    "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string",
    "struct", "switch", "this", "throw", "true", "try", "typeof", "uint",
    "ulong", "unchecked", "unsafe", "ushort", "using", "virtual", "void",
    "volatile", "while",
    // Contextual — safe to prefix with @ in most cases
    "add", "alias", "ascending", "async", "await", "by", "descending",
    "dynamic", "equals", "field", "from", "get", "global", "group", "init",
    "into", "join", "let", "managed", "nameof", "nint", "not", "notnull",
    "nuint", "on", "or", "orderby", "partial", "record", "remove", "required",
    "scoped", "select", "set", "unmanaged", "value", "var", "when", "where",
    "with", "yield",
};

// ─── CLR → C# type alias map ──────────────────────────────────────────────────

static const std::unordered_map<std::string, std::string> kClrAlias = {
    {"System.Boolean",  "bool"},
    {"System.Byte",     "byte"},
    {"System.SByte",    "sbyte"},
    {"System.Char",     "char"},
    {"System.Int16",    "short"},
    {"System.UInt16",   "ushort"},
    {"System.Int32",    "int"},
    {"System.UInt32",   "uint"},
    {"System.Int64",    "long"},
    {"System.UInt64",   "ulong"},
    {"System.Single",   "float"},
    {"System.Double",   "double"},
    {"System.Decimal",  "decimal"},
    {"System.String",   "string"},
    {"System.Object",   "object"},
    {"System.Void",     "void"},
    {"System.IntPtr",   "nint"},
    {"System.UIntPtr",  "nuint"},
};

// ─── CsWriter ─────────────────────────────────────────────────────────────────

CsWriter::CsWriter(Options opts) : opts_(std::move(opts)) {}

void CsWriter::writeIndent() {
    for (int i = 0; i < indentLevel_; ++i)
        buf_ << opts_.indentStr;
}

void CsWriter::rawWrite(std::string_view s) {
    buf_ << s;
}

void CsWriter::write(std::string_view text) {
    if (lineStart_ && !text.empty()) {
        writeIndent();
        lineStart_ = false;
    }
    rawWrite(text);
}

void CsWriter::nl() {
    rawWrite(opts_.lineEnding);
    lineStart_ = true;
}

void CsWriter::line(std::string_view text) {
    if (lineStart_)
        writeIndent();
    rawWrite(text);
    rawWrite(opts_.lineEnding);
    lineStart_ = true;
}

void CsWriter::blank() {
    rawWrite(opts_.lineEnding);
    lineStart_ = true;
}

void CsWriter::indent() {
    ++indentLevel_;
}

void CsWriter::dedent() {
    if (indentLevel_ > 0) --indentLevel_;
}

void CsWriter::openBrace() {
    line("{");
    ++indentLevel_;
}

void CsWriter::closeBrace(std::string_view suffix) {
    if (indentLevel_ > 0) --indentLevel_;
    if (suffix.empty())
        line("}");
    else {
        write("}");
        rawWrite(suffix);
        nl();
    }
}

// ─── BlockGuard ──────────────────────────────────────────────────────────────

CsWriter::BlockGuard::BlockGuard(CsWriter& w, std::string_view suffix)
    : w_(&w), suffix_(suffix) {
    w_->openBrace();
}

CsWriter::BlockGuard::~BlockGuard() {
    if (!moved_)
        w_->closeBrace(suffix_);
}

CsWriter::BlockGuard::BlockGuard(BlockGuard&& o) noexcept
    : w_(o.w_), suffix_(std::move(o.suffix_)), moved_(false) {
    o.moved_ = true;
}

CsWriter::BlockGuard CsWriter::block(std::string_view suffix) {
    return BlockGuard(*this, suffix);
}

// ─── Comments ─────────────────────────────────────────────────────────────────

void CsWriter::comment(std::string_view text) {
    write("// ");
    rawWrite(text);
    nl();
}

void CsWriter::xmlDoc(std::string_view summary,
                       const std::vector<std::pair<std::string,std::string>>& params,
                       std::string_view returns) {
    if (!opts_.emitXmlDoc) return;
    if (!summary.empty()) {
        line("/// <summary>");
        write("/// "); rawWrite(summary); nl();
        line("/// </summary>");
    }
    for (const auto& [name, desc] : params) {
        write("/// <param name=\"");
        rawWrite(name);
        rawWrite("\">");
        rawWrite(desc);
        rawWrite("</param>");
        nl();
    }
    if (!returns.empty()) {
        write("/// <returns>"); rawWrite(returns); rawWrite("</returns>"); nl();
    }
}

void CsWriter::trailingComment(std::string_view text) {
    // We don't track column precisely in this design, just append
    rawWrite("  // ");
    rawWrite(text);
}

// ─── Keyword / name helpers ───────────────────────────────────────────────────

bool CsWriter::isKeyword(const std::string& name) {
    return kKeywords.count(name) > 0;
}

std::string CsWriter::safeName(const std::string& name) {
    if (name.empty()) return "_";
    if (isKeyword(name)) return "@" + name;
    // Names starting with digit or invalid chars
    if (std::isdigit(static_cast<unsigned char>(name[0])))
        return "_" + name;
    return name;
}

// ─── String literal ───────────────────────────────────────────────────────────

std::string escapeCsharpString(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"':  out << "\\\""; break;
        case '\n': out << "\\n";  break;
        case '\r': out << "\\r";  break;
        case '\t': out << "\\t";  break;
        case '\0': out << "\\0";  break;
        case '\a': out << "\\a";  break;
        case '\b': out << "\\b";  break;
        case '\f': out << "\\f";  break;
        case '\v': out << "\\v";  break;
        default:
            if (c < 0x20 || c > 0x7E) {
                out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(c);
            } else {
                out << c;
            }
            break;
        }
    }
    return out.str();
}

std::string CsWriter::stringLiteral(const std::string& s) const {
    bool hasBackslash = s.find('\\') != std::string::npos;
    bool hasNewline   = s.find('\n') != std::string::npos ||
                        s.find('\r') != std::string::npos;
    bool hasNul       = s.find('\0') != std::string::npos;

    // Prefer verbatim string literals (@"...") for strings containing backslashes
    // or newlines — verbatim is supported since C# 1.0 and avoids double-escaping.
    // Raw string literals (C# 11+) are only used as a fallback for multiline
    // strings that contain embedded quotes (making verbatim impractical).
    if (!hasNul && (hasBackslash || hasNewline)) {
        // Verbatim: @"..." — double internal quotes
        std::string verb = s;
        size_t pos = 0;
        while ((pos = verb.find('"', pos)) != std::string::npos) {
            verb.insert(pos, "\"");
            pos += 2;
        }
        return "@\"" + verb + "\"";
    }

    return "\"" + escapeCsharpString(s) + "\"";
}

std::string CsWriter::charLiteral(uint32_t cp) {
    if (cp < 0x80) {
        char c = static_cast<char>(cp);
        switch (c) {
        case '\'': return "'\\''";
        case '\\': return "'\\\\'";
        case '\n': return "'\\n'";
        case '\r': return "'\\r'";
        case '\t': return "'\\t'";
        case '\0': return "'\\0'";
        default:
            if (c >= 0x20 && c <= 0x7E) {
                return std::string("'") + c + "'";
            }
        }
    }
    std::ostringstream out;
    out << "'\\u" << std::hex << std::setw(4) << std::setfill('0') << cp << "'";
    return out.str();
}

// ─── Type name helpers ────────────────────────────────────────────────────────

std::string CsWriter::clrToCsharpType(const std::string& clrName) {
    // Handle array suffix recursively: "System.Int32[]" → "int[]"
    if (clrName.size() >= 2 && clrName.substr(clrName.size()-2) == "[]") {
        return clrToCsharpType(clrName.substr(0, clrName.size()-2)) + "[]";
    }
    auto it = kClrAlias.find(clrName);
    if (it != kClrAlias.end()) return it->second;
    // Strip leading "System." for System types (preserving sub-namespaces like "IO.Stream")
    if (clrName.size() > 7 && clrName.substr(0, 7) == "System.")
        return clrName.substr(7);
    // For non-System types, strip any namespace prefix and return just the class name
    auto dot = clrName.rfind('.');
    if (dot != std::string::npos)
        return clrName.substr(dot + 1);
    return clrName;
}

const std::unordered_set<std::string>& CsWriter::builtinAliases() {
    static const std::unordered_set<std::string> s = {
        "bool","byte","sbyte","char","short","ushort","int","uint",
        "long","ulong","float","double","decimal","string","object","void",
        "nint","nuint"
    };
    return s;
}

// ─── Modifier helpers ─────────────────────────────────────────────────────────

void CsWriter::writeModifiers(const std::string& modStr) {
    // modStr is already space-separated in canonical order
    if (!modStr.empty()) {
        write(modStr);
        write(" ");
    }
}

void CsWriter::reset() {
    buf_.str("");
    buf_.clear();
    indentLevel_ = 0;
    lineStart_   = true;
}

} // namespace csharp_emitter
} // namespace retdec
