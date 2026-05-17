/**
 * @file src/jvm_parser/jvm_signature.cpp
 * @brief JVM generic-signature grammar parser per JVMS §4.7.9.1.
 */

#include <memory>
#include "retdec/jvm_parser/jvm_signature.h"
#include "retdec/jvm_parser/jvm_const_pool.h"

#include <stdexcept>

namespace retdec {
namespace jvm_parser {

using namespace bc_module;
using namespace bc_module::types;

// ─── Cursor ───────────────────────────────────────────────────────────────────

char JvmSignatureParser::Cursor::get() {
    if (pos >= s.size())
        throw JvmParseError("unexpected end of signature: " + s);
    return s[pos++];
}

bool JvmSignatureParser::Cursor::expect(char c) {
    if (pos < s.size() && s[pos] == c) { ++pos; return true; }
    return false;
}

std::string JvmSignatureParser::Cursor::identifier() {
    std::string id;
    while (pos < s.size()) {
        char c = s[pos];
        if (c == ':' || c == '<' || c == '>' || c == ';' || c == '.' || c == '/')
            break;
        id += c;
        ++pos;
    }
    return id;
}

// ─── Base types ───────────────────────────────────────────────────────────────

BcType JvmSignatureParser::parseBaseType(char tag) {
    switch (tag) {
    case 'B': return Byte();
    case 'C': return Char();
    case 'D': return Double();
    case 'F': return Float();
    case 'I': return Int();
    case 'J': return Long();
    case 'S': return Short();
    case 'Z': return Bool();
    case 'V': return Void();
    default:
        throw JvmParseError(std::string("unknown base type '") + tag + "'");
    }
}

// ─── Reference type signatures ────────────────────────────────────────────────

BcType JvmSignatureParser::parseTypeVariableSignature(Cursor& c) {
    // 'T' Identifier ';'
    std::string id = c.identifier();
    if (!c.expect(';'))
        throw JvmParseError("expected ';' after type variable " + id);
    return TypeVar(id);
}

BcType JvmSignatureParser::parseTypeArgument(Cursor& c) {
    char ch = c.peek();
    if (ch == '*') { c.get(); return Wildcard(); }
    if (ch == '+') { c.get(); return BoundedAbove(parseReferenceTypeSignature(c)); }
    if (ch == '-') { c.get(); return BoundedBelow(parseReferenceTypeSignature(c)); }
    return parseReferenceTypeSignature(c);
}

BcType JvmSignatureParser::parseClassTypeSignature(Cursor& c) {
    // 'L' (PackageSpecifier)? SimpleClassTypeSignature ClassTypeSignatureSuffix* ';'
    std::string name;
    // Read package specifiers and class name (separated by '/' or '.')
    while (c.pos < c.s.size()) {
        char ch = c.peek();
        if (ch == '<' || ch == ';' || ch == '.') break;
        name += c.get();
    }
    // Type arguments
    std::vector<BcType> args;
    if (c.peek() == '<') {
        c.get(); // consume '<'
        while (c.peek() != '>') args.push_back(parseTypeArgument(c));
        c.get(); // consume '>'
    }
    // Inner class suffixes
    while (c.peek() == '.') {
        c.get(); // consume '.'
        std::string inner;
        while (c.pos < c.s.size()) {
            char ch = c.peek();
            if (ch == '<' || ch == ';' || ch == '.') break;
            inner += c.get();
        }
        name += "$" + inner;
        if (c.peek() == '<') {
            c.get();
            while (c.peek() != '>') args.push_back(parseTypeArgument(c));
            c.get();
        }
    }
    if (!c.expect(';'))
        throw JvmParseError("expected ';' after class type signature for " + name);

    BcType base = Class(name);
    if (args.empty()) return base;
    return Generic(base, std::move(args));
}

BcType JvmSignatureParser::parseArrayTypeSignature(Cursor& c) {
    // '[' JavaTypeSignature
    int dims = 0;
    while (c.peek() == '[') { c.get(); ++dims; }
    BcType elem = parseJavaTypeSignature(c);
    return Array(elem, dims);
}

BcType JvmSignatureParser::parseReferenceTypeSignature(Cursor& c) {
    char ch = c.peek();
    if (ch == 'L') { c.get(); return parseClassTypeSignature(c); }
    if (ch == 'T') { c.get(); return parseTypeVariableSignature(c); }
    if (ch == '[') { return parseArrayTypeSignature(c); }
    throw JvmParseError(std::string("expected reference type, got '") + ch + "'");
}

BcType JvmSignatureParser::parseJavaTypeSignature(Cursor& c) {
    char ch = c.peek();
    if (ch == 'L' || ch == 'T' || ch == '[')
        return parseReferenceTypeSignature(c);
    c.get();
    return parseBaseType(ch);
}

// ─── Type parameters ──────────────────────────────────────────────────────────

JvmTypeParam JvmSignatureParser::parseTypeParameter(Cursor& c) {
    JvmTypeParam tp;
    tp.name = c.identifier();
    // ':' ClassBound
    if (!c.expect(':'))
        throw JvmParseError("expected ':' in type parameter " + tp.name);
    // Class bound may be empty (just ':')
    if (c.peek() == ':' || c.peek() == '>') {
        tp.classBound = Object();
    } else {
        tp.classBound = parseReferenceTypeSignature(c);
    }
    // InterfaceBounds
    while (c.peek() == ':') {
        c.get();
        tp.interfaceBounds.push_back(parseReferenceTypeSignature(c));
    }
    return tp;
}

std::vector<JvmTypeParam> JvmSignatureParser::parseTypeParameters(Cursor& c) {
    std::vector<JvmTypeParam> params;
    if (c.peek() != '<') return params;
    c.get(); // consume '<'
    while (c.peek() != '>') params.push_back(parseTypeParameter(c));
    c.get(); // consume '>'
    return params;
}

// ─── Public interface ─────────────────────────────────────────────────────────

BcType JvmSignatureParser::parseFieldSig(const std::string& sig) {
    Cursor c{sig, 0};
    return parseReferenceTypeSignature(c);
}

ParsedClassSignature JvmSignatureParser::parseClassSig(const std::string& sig) {
    Cursor c{sig, 0};
    ParsedClassSignature cs;
    cs.typeParams = parseTypeParameters(c);
    // SuperclassSignature
    if (c.peek() == 'L') { c.get(); cs.superclass = parseClassTypeSignature(c); }
    else cs.superclass = Object();
    // SuperinterfaceSignatures
    while (!c.atEnd())
        cs.interfaces.push_back(parseReferenceTypeSignature(c));
    return cs;
}

ParsedMethodSignature JvmSignatureParser::parseMethodSig(const std::string& sig) {
    Cursor c{sig, 0};
    ParsedMethodSignature ms;
    ms.typeParams = parseTypeParameters(c);
    if (!c.expect('('))
        throw JvmParseError("expected '(' in method signature: " + sig);
    while (c.peek() != ')')
        ms.params.push_back(parseJavaTypeSignature(c));
    c.get(); // consume ')'
    ms.returnType = parseJavaTypeSignature(c);
    // ThrowsSignatures
    while (c.peek() == '^') {
        c.get();
        ms.throwsTypes.push_back(parseReferenceTypeSignature(c));
    }
    return ms;
}

// ─── Descriptor parser (non-generic) ─────────────────────────────────────────

BcType JvmSignatureParser::parseDescriptor(const std::string& desc) {
    if (desc.empty()) return Object();
    Cursor c{desc, 0};
    return parseJavaTypeSignature(c);
}

bc_module::BcFuncType JvmSignatureParser::parseMethodDescriptor(const std::string& desc) {
    bc_module::BcFuncType ft;
    Cursor c{desc, 0};
    if (!c.expect('('))
        throw JvmParseError("method descriptor must start with '(': " + desc);
    while (c.peek() != ')') {
        auto t = parseJavaTypeSignature(c);
        ft.params.push_back(std::make_shared<BcType>(std::move(t)));
    }
    c.get(); // consume ')'
    auto ret = parseJavaTypeSignature(c);
    ft.returnType = std::make_shared<BcType>(std::move(ret));
    return ft;
}

} // namespace jvm_parser
} // namespace retdec
