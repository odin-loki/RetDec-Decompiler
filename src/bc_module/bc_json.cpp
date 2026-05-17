/**
 * @file src/bc_module/bc_json.cpp
 * @brief BcModule JSON serialisation and deserialisation.
 *
 * Uses a hand-rolled emitter (no external JSON library) to keep the
 * bc_module library dependency-free.  The deserialiser is a minimal
 * recursive-descent parser sufficient for round-trip testing.
 */

#include <memory>
#include "retdec/bc_module/bc_json.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace retdec {
namespace bc_module {
namespace json {

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string indent(int n) { return std::string(static_cast<size_t>(n * 2), ' '); }

static std::string escapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else out += c;
    }
    return out;
}

static std::string jStr(const std::string& s) {
    return "\"" + escapeStr(s) + "\"";
}

// ─── Type serialisation ───────────────────────────────────────────────────────

std::string serialiseType(const BcType& t) {
    std::ostringstream o;
    o << "{\"kind\":";
    if (t.isPrim()) {
        o << "\"prim\",\"name\":" << jStr(t.toString());
    } else if (t.isRef()) {
        const auto& r = t.ref();
        switch (r.kind) {
        case BcRefKind::Class:
            o << "\"class\",\"name\":" << jStr(r.className);
            break;
        case BcRefKind::TypeVariable:
            o << "\"typevar\",\"name\":" << jStr(r.className);
            break;
        case BcRefKind::Array:
            o << "\"array\",\"dims\":" << r.arrayDims
              << ",\"element\":" << (r.elementType ? serialiseType(*r.elementType) : "null");
            break;
        case BcRefKind::Generic:
            o << "\"generic\",\"base\":"
              << (r.genericBase ? serialiseType(*r.genericBase) : "null")
              << ",\"args\":[";
            for (size_t i = 0; i < r.typeArgs.size(); ++i) {
                if (i) o << ',';
                o << (r.typeArgs[i] ? serialiseType(*r.typeArgs[i]) : "null");
            }
            o << "]";
            break;
        case BcRefKind::Wildcard:
            o << "\"wildcard\"";
            break;
        case BcRefKind::BoundedAbove:
            o << "\"bounded_above\",\"bound\":"
              << (r.bound ? serialiseType(*r.bound) : "null");
            break;
        case BcRefKind::BoundedBelow:
            o << "\"bounded_below\",\"bound\":"
              << (r.bound ? serialiseType(*r.bound) : "null");
            break;
        case BcRefKind::Null:
            o << "\"null_type\"";
            break;
        }
    } else if (t.isFunc()) {
        const auto& ft = t.func();
        o << "\"func\",\"params\":[";
        for (size_t i = 0; i < ft.params.size(); ++i) {
            if (i) o << ',';
            o << (ft.params[i] ? serialiseType(*ft.params[i]) : "null");
        }
        o << "],\"return\":"
          << (ft.returnType ? serialiseType(*ft.returnType) : "null");
    }
    o << "}";
    return o.str();
}

// ─── Instruction serialisation ────────────────────────────────────────────────

static std::string serialiseOperand(const BcOperand& op) {
    std::ostringstream o;
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, BcIntOperand>)
            o << "{\"kind\":\"int\",\"value\":" << v.value << "}";
        else if constexpr (std::is_same_v<T, BcFloatOperand>)
            o << "{\"kind\":\"float\",\"value\":" << v.value << "}";
        else if constexpr (std::is_same_v<T, BcStringOperand>)
            o << "{\"kind\":\"string\",\"value\":" << jStr(v.value) << "}";
        else if constexpr (std::is_same_v<T, BcTypeOperand>)
            o << "{\"kind\":\"type\",\"type\":" << serialiseType(v.type) << "}";
        else if constexpr (std::is_same_v<T, BcMethodRef>)
            o << "{\"kind\":\"methodref\",\"owner\":" << jStr(v.owner)
              << ",\"name\":" << jStr(v.name) << "}";
        else if constexpr (std::is_same_v<T, BcFieldRef>)
            o << "{\"kind\":\"fieldref\",\"owner\":" << jStr(v.owner)
              << ",\"name\":" << jStr(v.name) << "}";
        else if constexpr (std::is_same_v<T, BcLocalOperand>)
            o << "{\"kind\":\"local\",\"index\":" << v.index << "}";
        else if constexpr (std::is_same_v<T, BcBlockOperand>)
            o << "{\"kind\":\"block\",\"id\":" << v.blockId << "}";
        else if constexpr (std::is_same_v<T, BcSwitchTable>) {
            o << "{\"kind\":\"switch\",\"default\":" << v.defaultBlock
              << ",\"cases\":[";
            for (size_t i = 0; i < v.cases.size(); ++i) {
                if (i) o << ',';
                o << "{\"key\":" << v.cases[i].first
                  << ",\"block\":" << v.cases[i].second << "}";
            }
            o << "]}";
        }
    }, op);
    return o.str();
}

std::string serialiseInstr(const BcInstruction& i, int ind) {
    std::ostringstream o;
    o << indent(ind) << "{"
      << "\"id\":" << i.id
      << ",\"off\":" << i.offset
      << ",\"op\":" << static_cast<int>(i.opcode)
      << ",\"pop\":" << (int)i.effect.pop
      << ",\"push\":" << (int)i.effect.push;
    if (i.line >= 0) o << ",\"line\":" << i.line;
    if (!i.operands.empty()) {
        o << ",\"operands\":[";
        for (size_t k = 0; k < i.operands.size(); ++k) {
            if (k) o << ',';
            o << serialiseOperand(i.operands[k]);
        }
        o << "]";
    }
    o << "}";
    return o.str();
}

// ─── Block serialisation ──────────────────────────────────────────────────────

std::string serialiseBlock(const BcBasicBlock& b, int ind) {
    std::ostringstream o;
    o << indent(ind) << "{"
      << "\"id\":" << b.id;
    if (!b.label.empty()) o << ",\"label\":" << jStr(b.label);
    // Successors / predecessors
    o << ",\"succs\":[";
    for (size_t k = 0; k < b.succs.size(); ++k) { if (k) o << ','; o << b.succs[k]; }
    o << "],\"preds\":[";
    for (size_t k = 0; k < b.preds.size(); ++k) { if (k) o << ','; o << b.preds[k]; }
    o << "]";
    // Entry/exit stack types
    if (!b.entryStack.empty()) {
        o << ",\"entryStack\":[";
        for (size_t k = 0; k < b.entryStack.size(); ++k) {
            if (k) o << ',';
            o << serialiseType(b.entryStack[k]);
        }
        o << "]";
    }
    // Instructions
    o << ",\"instrs\":[\n";
    for (size_t k = 0; k < b.instrs.size(); ++k) {
        if (k) o << ",\n";
        o << serialiseInstr(b.instrs[k], ind + 1);
    }
    o << "\n" << indent(ind) << "]}";
    return o.str();
}

// ─── CFG serialisation ────────────────────────────────────────────────────────

std::string serialiseCFG(const BcCFG& cfg, int ind) {
    std::ostringstream o;
    o << indent(ind) << "{\"blocks\":[\n";
    for (size_t k = 0; k < cfg.blockCount(); ++k) {
        if (k) o << ",\n";
        o << serialiseBlock(cfg.block(static_cast<uint32_t>(k)), ind + 1);
    }
    o << "\n" << indent(ind) << "]}";
    return o.str();
}

// ─── Field / method / class serialisation ────────────────────────────────────

std::string serialiseField(const BcField& f, int ind) {
    std::ostringstream o;
    o << indent(ind) << "{"
      << "\"name\":" << jStr(f.name)
      << ",\"type\":" << serialiseType(f.type)
      << ",\"access\":" << static_cast<uint32_t>(f.access);
    if (f.constantIntValue) o << ",\"constInt\":" << *f.constantIntValue;
    if (f.constantFltValue) o << ",\"constFlt\":" << *f.constantFltValue;
    if (f.constantStrValue) o << ",\"constStr\":" << jStr(*f.constantStrValue);
    o << "}";
    return o.str();
}

std::string serialiseMethod(const BcMethod& m, int ind) {
    std::ostringstream o;
    o << indent(ind) << "{"
      << "\"name\":" << jStr(m.name)
      << ",\"descriptor\":" << jStr(m.descriptor.jvmDescriptor())
      << ",\"access\":" << static_cast<uint32_t>(m.access)
      << ",\"isCtor\":" << (m.isConstructor ? "true" : "false")
      << ",\"maxStack\":" << m.maxStack
      << ",\"maxLocals\":" << m.maxLocals;
    if (!m.throwsList.empty()) {
        o << ",\"throws\":[";
        for (size_t k = 0; k < m.throwsList.size(); ++k) {
            if (k) o << ',';
            o << jStr(m.throwsList[k]);
        }
        o << "]";
    }
    if (!m.cfg.blocks().empty()) {
        o << ",\n\"cfg\":" << serialiseCFG(m.cfg, ind + 1);
    }
    o << "\n" << indent(ind) << "}";
    return o.str();
}

std::string serialiseClass(const BcClass& c, int ind) {
    std::ostringstream o;
    o << indent(ind) << "{"
      << "\"name\":" << jStr(c.name)
      << ",\"fqName\":" << jStr(c.fqName)
      << ",\"access\":" << static_cast<uint32_t>(c.access)
      << ",\"isInterface\":" << (c.isInterface ? "true" : "false")
      << ",\"isEnum\":" << (c.isEnum ? "true" : "false")
      << ",\"isRecord\":" << (c.isRecord ? "true" : "false");
    if (c.superClass) o << ",\"super\":" << serialiseType(*c.superClass);
    // Fields
    o << ",\n\"fields\":[\n";
    for (size_t k = 0; k < c.fields.size(); ++k) {
        if (k) o << ",\n";
        o << serialiseField(c.fields[k], ind + 1);
    }
    o << "\n" << indent(ind) << "]";
    // Methods
    o << ",\n\"methods\":[\n";
    for (size_t k = 0; k < c.methods.size(); ++k) {
        if (k) o << ",\n";
        o << serialiseMethod(c.methods[k], ind + 1);
    }
    o << "\n" << indent(ind) << "]}";
    return o.str();
}

// ─── Module serialisation ─────────────────────────────────────────────────────

std::string serialiseModule(const BcModule& mod, int ind) {
    std::ostringstream o;
    o << "{\n"
      << indent(ind+1) << "\"bcModuleVersion\":" << kVersion << ",\n"
      << indent(ind+1) << "\"name\":" << jStr(mod.name()) << ",\n"
      << indent(ind+1) << "\"sourceLang\":" << jStr(sourceLangName(mod.sourceLang())) << ",\n";
    // String pool
    o << indent(ind+1) << "\"stringPool\":[\n";
    for (uint32_t k = 0; k < mod.stringCount(); ++k) {
        if (k) o << ",\n";
        o << indent(ind+2) << jStr(mod.string(k));
    }
    o << "\n" << indent(ind+1) << "],\n";
    // Classes
    o << indent(ind+1) << "\"classes\":[\n";
    for (size_t k = 0; k < mod.classes().size(); ++k) {
        if (k) o << ",\n";
        o << serialiseClass(mod.classes()[k], ind + 2);
    }
    o << "\n" << indent(ind+1) << "]\n";
    o << "}";
    return o.str();
}

// ─── Minimal JSON deserialiser ────────────────────────────────────────────────
//
// Supports the subset emitted by the serialiser above for round-trip testing.
// Only BcModule top-level fields and BcClass + BcField + BcMethod names /
// descriptors are fully reconstructed; CFG and instruction bodies are
// validated structurally but not semantically.

namespace detail {

struct Parser {
    const std::string& src;
    size_t pos = 0;

    char peek() const { return pos < src.size() ? src[pos] : '\0'; }
    char get()  { return pos < src.size() ? src[pos++] : '\0'; }

    void skipWS() {
        while (pos < src.size() && std::isspace((unsigned char)src[pos])) ++pos;
    }

    bool expect(char c) {
        skipWS();
        if (peek() == c) { ++pos; return true; }
        return false;
    }

    std::string parseString() {
        skipWS();
        if (!expect('"')) return "";
        std::string s;
        while (pos < src.size()) {
            char c = get();
            if (c == '"') break;
            if (c == '\\') {
                char esc = get();
                switch (esc) {
                case '"':  s += '"';  break;
                case '\\': s += '\\'; break;
                case 'n':  s += '\n'; break;
                case 'r':  s += '\r'; break;
                case 't':  s += '\t'; break;
                default:   s += esc;  break;
                }
            } else {
                s += c;
            }
        }
        return s;
    }

    int64_t parseInt() {
        skipWS();
        bool neg = (peek() == '-');
        if (neg) ++pos;
        int64_t v = 0;
        while (pos < src.size() && std::isdigit((unsigned char)src[pos]))
            v = v * 10 + (src[pos++] - '0');
        return neg ? -v : v;
    }

    bool parseBool() {
        skipWS();
        if (src.substr(pos, 4) == "true")  { pos += 4; return true; }
        if (src.substr(pos, 5) == "false") { pos += 5; return false; }
        return false;
    }

    void skipValue() {
        skipWS();
        char c = peek();
        if (c == '"') { parseString(); return; }
        if (c == '{') { skipObject(); return; }
        if (c == '[') { skipArray(); return; }
        // Number / null / bool
        while (pos < src.size() && src[pos] != ',' && src[pos] != '}' && src[pos] != ']')
            ++pos;
    }

    void skipObject() {
        expect('{');
        skipWS();
        if (expect('}')) return; // empty object
        while (true) {
            skipValue(); // key
            expect(':');
            skipValue(); // val
            skipWS();
            if (!expect(',')) break;
            skipWS();
        }
        expect('}'); // consume closing brace
    }

    void skipArray() {
        expect('[');
        skipWS();
        if (expect(']')) return; // empty array
        while (true) {
            skipValue();
            skipWS();
            if (!expect(',')) break;
            skipWS();
        }
        expect(']'); // consume closing bracket
    }

    // Parse key-dispatching object.
    // Calls `handler(key)` for each key; handler calls parse* on value.
    template<typename Handler>
    void parseObject(Handler&& handler) {
        skipWS();
        expect('{');
        skipWS();
        while (peek() != '}' && pos < src.size()) {
            std::string key = parseString();
            expect(':');
            handler(key);
            skipWS();
            if (!expect(',')) break;
            skipWS();
        }
        expect('}');
    }

    // Parse array, calling element() for each element.
    template<typename Elem>
    void parseArray(Elem&& element) {
        skipWS();
        expect('[');
        skipWS();
        while (peek() != ']' && pos < src.size()) {
            element();
            skipWS();
            if (!expect(',')) break;
            skipWS();
        }
        expect(']');
    }
};

static BcType parseType(Parser& p) {
    BcType t;
    p.parseObject([&](const std::string& key) {
        if (key == "kind") {
            std::string kind = p.parseString();
            if (kind == "prim") {
                // Kind will be set when "name" is parsed below.
                t = types::Int(); // default
            } else if (kind == "class") {
                t = types::Object(); // set proper name below
            } else if (kind == "array") {
                BcRefType r; r.kind = BcRefKind::Array; t = BcType{r};
            } else if (kind == "generic") {
                BcRefType r; r.kind = BcRefKind::Generic; t = BcType{r};
            } else if (kind == "func") {
                BcFuncType ft; t = BcType{ft};
            } else {
                t = types::Object();
            }
        } else if (key == "name") {
            std::string nm = p.parseString();
            if (t.isPrim()) {
                // Map prim name back to kind.
                if (nm == "void")    t = types::Void();
                else if (nm == "boolean") t = types::Bool();
                else if (nm == "byte")    t = types::Byte();
                else if (nm == "char")    t = types::Char();
                else if (nm == "short")   t = types::Short();
                else if (nm == "int")     t = types::Int();
                else if (nm == "long")    t = types::Long();
                else if (nm == "float")   t = types::Float();
                else if (nm == "double")  t = types::Double();
            } else if (t.isRef()) {
                const_cast<BcRefType&>(t.ref()).className = nm;
            }
        } else if (key == "element") {
            if (t.isRef()) {
                const_cast<BcRefType&>(t.ref()).elementType =
                    std::make_shared<BcType>(parseType(p));
            } else p.skipValue();
        } else if (key == "dims") {
            if (t.isRef()) const_cast<BcRefType&>(t.ref()).arrayDims = (int)p.parseInt();
            else p.parseInt();
        } else {
            p.skipValue();
        }
    });
    return t;
}

} // namespace detail

// ─── ParseResult ─────────────────────────────────────────────────────────────

ParseResult deserialiseModule(const std::string& src) {
    ParseResult res;
    detail::Parser p{src, 0};
    BcModule& mod = res.module;
    try {
        p.parseObject([&](const std::string& key) {
            if (key == "bcModuleVersion") {
                p.parseInt(); // Check version but don't fail on mismatch.
            } else if (key == "name") {
                // BcModule constructed with empty name; reconstruct.
                std::string nm = p.parseString();
                res.module = BcModule(nm, SourceLang::Unknown);
            } else if (key == "sourceLang") {
                std::string l = p.parseString();
                if (l == "Java")        res.module.setSourceLang(SourceLang::Java);
                else if (l == "CSharp") res.module.setSourceLang(SourceLang::CSharp);
                else if (l == "Python") res.module.setSourceLang(SourceLang::Python);
                else if (l == "WebAssembly") res.module.setSourceLang(SourceLang::WebAssembly);
                else if (l == "Lua")    res.module.setSourceLang(SourceLang::Lua);
            } else if (key == "stringPool") {
                p.parseArray([&]() {
                    res.module.internString(p.parseString());
                });
            } else if (key == "classes") {
                p.parseArray([&]() {
                    BcClass cls;
                    p.parseObject([&](const std::string& k) {
                        if (k == "name")       cls.name   = p.parseString();
                        else if (k == "fqName") cls.fqName = p.parseString();
                        else if (k == "isInterface") cls.isInterface = p.parseBool();
                        else if (k == "isEnum")      cls.isEnum      = p.parseBool();
                        else if (k == "isRecord")    cls.isRecord    = p.parseBool();
                        else if (k == "fields") {
                            p.parseArray([&]() {
                                BcField f;
                                p.parseObject([&](const std::string& fk) {
                                    if (fk == "name") f.name = p.parseString();
                                    else if (fk == "type") f.type = detail::parseType(p);
                                    else p.skipValue();
                                });
                                cls.fields.push_back(std::move(f));
                            });
                        } else if (k == "methods") {
                            p.parseArray([&]() {
                                BcMethod m;
                                p.parseObject([&](const std::string& mk) {
                                    if (mk == "name") m.name = p.parseString();
                                    else if (mk == "isCtor") m.isConstructor = p.parseBool();
                                    else if (mk == "maxStack")  m.maxStack  = (uint16_t)p.parseInt();
                                    else if (mk == "maxLocals") m.maxLocals = (uint16_t)p.parseInt();
                                    else p.skipValue();
                                });
                                cls.methods.push_back(std::move(m));
                            });
                        } else {
                            p.skipValue();
                        }
                    });
                    res.module.addClass(std::move(cls));
                });
            } else {
                p.skipValue();
            }
        });
        res.ok = true;
    } catch (const std::exception& e) {
        res.ok    = false;
        res.error = std::string("Parse error: ") + e.what();
    }
    return res;
}

// ─── Round-trip ───────────────────────────────────────────────────────────────

bool roundTripEquals(const BcModule& original, std::string* diffOut) {
    std::string s = serialiseModule(original);
    auto result   = deserialiseModule(s);
    if (!result.ok) {
        if (diffOut) *diffOut = "Deserialisation failed: " + result.error;
        return false;
    }
    const auto& a = original;
    const auto& b = result.module;
    if (a.name() != b.name()) {
        if (diffOut) *diffOut = "name mismatch: " + a.name() + " vs " + b.name();
        return false;
    }
    if (a.sourceLang() != b.sourceLang()) {
        if (diffOut) *diffOut = "sourceLang mismatch";
        return false;
    }
    if (a.classes().size() != b.classes().size()) {
        if (diffOut) *diffOut = "class count mismatch";
        return false;
    }
    for (size_t i = 0; i < a.classes().size(); ++i) {
        if (a.classes()[i].fqName != b.classes()[i].fqName) {
            if (diffOut)
                *diffOut = "class[" + std::to_string(i) + "] fqName mismatch";
            return false;
        }
        if (a.classes()[i].fields.size() != b.classes()[i].fields.size()) {
            if (diffOut) *diffOut = "field count mismatch in " + a.classes()[i].fqName;
            return false;
        }
        if (a.classes()[i].methods.size() != b.classes()[i].methods.size()) {
            if (diffOut) *diffOut = "method count mismatch in " + a.classes()[i].fqName;
            return false;
        }
    }
    return true;
}

} // namespace json
} // namespace bc_module
} // namespace retdec
