/**
 * @file src/type_seed/itanium_seeder.cpp
 * @brief Itanium C++ ABI mangling parser for type seeding.
 *
 * ## Itanium mangling grammar (subset implemented)
 *
 *   <mangled-name> := _Z <encoding>
 *   <encoding>     := <function-name> <bare-function-type>
 *                   | <data-name>
 *                   | <special-name>
 *   <function-name>:= <nested-name>    (class member)
 *                   | <unscoped-name>  (free function or std:: shorthand)
 *                   | <local-name>
 *   <nested-name>  := N [CV-quals] [ref-qualifier] <prefix>* <unqualified-name> E
 *   <prefix>       := <unqualified-name> | <template-prefix> <template-args>
 *   <unqualified-name> := <operator-name> | <ctor-dtor-name> | <source-name>
 *   <source-name>  := <positive-length> <identifier>
 *   <bare-function-type> := <type>+          (first element = return type for templates)
 *   <type>         := <builtin-type> | <qualified-type> | <function-type>
 *                   | <class-enum-type> | <array-type> | <pointer-to-member-type>
 *                   | <template-param> | <template-template-param> <template-args>
 *                   | <substitution> | P<type> | R<type> | O<type>
 *                   | K<type> (const) | V<type> (volatile) | r<type> (restrict)
 *   <builtin-type> := v(void) i(int) j(unsigned int) l(long) m(unsigned long)
 *                     x(long long) y(unsigned long long) s(short) t(unsigned short)
 *                     c(char) a(signed char) h(unsigned char) w(wchar_t) b(bool)
 *                     f(float) d(double) e(long double) z(...)
 *                     Dn(decltype(nullptr)) Di(char32_t) Ds(char16_t) Da(auto)
 *   <template-args>:= I <template-arg>+ E
 *   <template-arg> := <type> | X<expression>E | <expr-primary>
 *
 * We parse enough of this grammar to extract:
 *   - Class name (if member function)
 *   - Function name
 *   - All parameter types
 *   - Return type (for template functions — it is the FIRST type in
 *     bare-function-type; for non-template functions it is absent)
 *   - CV-qualifiers on `this` (const member → isConst)
 *   - Whether it is a constructor/destructor/operator
 *   - Template arguments for well-known STL types
 */

#include <memory>
#include "retdec/type_seed/type_seed.h"

#ifdef RETDEC_USE_CXXABI_DEMANGLE
#  include <cxxabi.h>
#endif

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace type_seed {

// ─── Itanium parser state ─────────────────────────────────────────────────────

namespace {

struct ItaniumParser {
    const char* p;
    const char* end;
    bool        ok = true;

    // Substitution table: S_ S0_ S1_ ...
    std::vector<std::string> subs;

    explicit ItaniumParser(const char* s, std::size_t n)
        : p(s), end(s + n) {}

    bool atEnd() const { return p >= end; }
    char peek() const { return (p < end) ? *p : '\0'; }
    char get()  { return (p < end) ? *p++ : '\0'; }
    bool consume(char c) { if (peek()==c){++p;return true;} return false; }
    bool consume(const char* s) {
        std::size_t n = std::strlen(s);
        if (end - p >= (ptrdiff_t)n && std::memcmp(p, s, n)==0) { p+=n; return true; }
        return false;
    }

    // ── Builtin type map ──────────────────────────────────────────────────────

    // Returns empty string if not a builtin.
    std::string tryBuiltin() {
        static const struct { char code; const char* name; } table[] = {
            {'v',"void"}, {'b',"bool"}, {'c',"char"}, {'a',"signed char"},
            {'h',"unsigned char"}, {'s',"short"}, {'t',"unsigned short"},
            {'i',"int"}, {'j',"unsigned int"}, {'l',"long"},
            {'m',"unsigned long"}, {'x',"long long"}, {'y',"unsigned long long"},
            {'n',"__int128"}, {'o',"unsigned __int128"},
            {'f',"float"}, {'d',"double"}, {'e',"long double"},
            {'g',"__float128"}, {'z',"..."}, {'w',"wchar_t"},
        };
        for (auto& e : table) {
            if (peek() == e.code) { ++p; return e.name; }
        }
        // Two-character builtins starting with 'D'
        if (peek() == 'D' && p+1 < end) {
            char c2 = p[1];
            if (c2=='n'){p+=2;return "decltype(nullptr)";}
            if (c2=='i'){p+=2;return "char32_t";}
            if (c2=='s'){p+=2;return "char16_t";}
            if (c2=='u'){p+=2;return "char8_t";}
            if (c2=='a'){p+=2;return "auto";}
            if (c2=='f'){p+=2;return "_Decimal32";}
            if (c2=='d'){p+=2;return "_Decimal64";}
            if (c2=='e'){p+=2;return "_Decimal128";}
        }
        return {};
    }

    // ── Source name (length-prefixed identifier) ──────────────────────────────

    std::string parseSourceName() {
        if (!std::isdigit(static_cast<unsigned char>(peek()))) { ok=false; return {}; }
        int len = 0;
        while (std::isdigit(static_cast<unsigned char>(peek())))
            len = len*10 + (get()-'0');
        if (len <= 0 || p+len > end) { ok=false; return {}; }
        std::string s(p, len);
        p += len;
        return s;
    }

    // ── Template args: I <arg>+ E → "arg1, arg2, ..." ────────────────────────

    std::string parseTemplateArgs(std::vector<std::string>* extractedArgs = nullptr) {
        if (!consume('I')) return {};
        std::vector<std::string> args;
        while (!atEnd() && peek() != 'E') {
            std::string t = parseType();
            if (t.empty()) {
                // Non-type or expression argument: skip to E
                // Consume until matching E or end
                int depth=1;
                if (consume('X')) {
                    while (!atEnd()) {
                        if (peek()=='X'||peek()=='I') ++depth;
                        if (peek()=='E') { --depth; get(); if(!depth) break; } else get();
                    }
                    args.push_back("<expr>");
                } else if (std::isdigit(static_cast<unsigned char>(peek())) ||
                           peek()=='-' || peek()=='L') {
                    // literal: skip to end of literal
                    if (consume('L')) {
                        parseType();
                        // read integer
                        bool neg=consume('n');
                        std::string num;
                        while(std::isdigit(static_cast<unsigned char>(peek()))) num+=get();
                        consume('E');
                        args.push_back(neg ? "-"+num : num);
                    } else {
                        args.push_back("<int>");
                    }
                } else {
                    break;
                }
            } else {
                args.push_back(t);
            }
        }
        consume('E');
        if (extractedArgs) *extractedArgs = args;
        if (args.empty()) return "<>";
        std::string result = "<";
        for (std::size_t i=0; i<args.size(); ++i) {
            if (i) result += ", ";
            result += args[i];
        }
        result += '>';
        return result;
    }

    // ── Operator name ─────────────────────────────────────────────────────────

    std::string tryOperator() {
        static const struct { const char* code; const char* name; } ops[] = {
            {"nw","operator new"},    {"na","operator new[]"},
            {"dl","operator delete"}, {"da","operator delete[]"},
            {"ps","operator+"},       {"ng","operator-"},
            {"ad","operator&"},       {"de","operator*"},
            {"co","operator~"},       {"pl","operator+"},
            {"mi","operator-"},       {"ml","operator*"},
            {"dv","operator/"},       {"rm","operator%"},
            {"an","operator&"},       {"or","operator|"},
            {"eo","operator^"},       {"aS","operator="},
            {"pL","operator+="},      {"mI","operator-="},
            {"mL","operator*="},      {"dV","operator/="},
            {"rM","operator%="},      {"aN","operator&="},
            {"oR","operator|="},      {"eO","operator^="},
            {"ls","operator<<"},      {"rs","operator>>"},
            {"lS","operator<<="},     {"rS","operator>>="},
            {"eq","operator=="},      {"ne","operator!="},
            {"lt","operator<"},       {"gt","operator>"},
            {"le","operator<="},      {"ge","operator>="},
            {"ss","operator<=>"},     {"nt","operator!"},
            {"aa","operator&&"},      {"oo","operator||"},
            {"pp","operator++"},      {"mm","operator--"},
            {"cm","operator,"},       {"pm","operator->*"},
            {"pt","operator->"},      {"cl","operator()"},
            {"ix","operator[]"},      {"qu","operator?"},
            {"cv","operator (cast)"},
        };
        for (auto& e : ops) {
            if (end-p >= 2 && p[0]==e.code[0] && p[1]==e.code[1]) {
                p += 2; return e.name;
            }
        }
        return {};
    }

    // ── Ctor/Dtor name ────────────────────────────────────────────────────────

    std::string tryCtorDtor(bool& isCtor, bool& isDtor) {
        if (p+1 < end && p[0]=='C' && (p[1]>='1'&&p[1]<='5')) {
            isCtor=true; p+=2; return "<constructor>";
        }
        if (p+1 < end && p[0]=='D' && (p[1]=='0'||p[1]=='1'||p[1]=='2'||p[1]=='5'||p[1]=='4'||p[1]=='9')) {
            isDtor=true; p+=2; return "<destructor>";
        }
        return {};
    }

    // ── Substitution ──────────────────────────────────────────────────────────

    std::string trySubstitution() {
        if (!consume('S')) return {};
        // S_ = subs[0], S0_ = subs[1], S1_ = subs[2], ...
        // Well-known substitutions
        static const struct { char c; const char* name; } well[] = {
            {'t',"std"}, {'a',"std::allocator"}, {'b',"std::basic_string"},
            {'s',"std::string"}, {'i',"std::istream"}, {'o',"std::ostream"},
            {'d',"std::iostream"},
        };
        for (auto& e : well) {
            if (peek()==e.c) { ++p; return e.name; }
        }
        // Numbered substitutions
        if (peek()=='_') { ++p; return subs.empty() ? "<sub0>" : subs[0]; }
        int idx=0;
        while (peek() && peek()!='_' && std::isalnum(static_cast<unsigned char>(peek()))) {
            char c=get();
            if (c>='0'&&c<='9') idx=idx*36+(c-'0');
            else if (c>='A'&&c<='Z') idx=idx*36+(c-'A'+10);
            else if (c>='a'&&c<='z') idx=idx*36+(c-'a'+36);
        }
        consume('_');
        int realIdx = idx+1; // S0_ is subs[1]
        if (realIdx < (int)subs.size()) return subs[realIdx];
        return "<sub?>";
    }

    // ── Main type parser ──────────────────────────────────────────────────────

    std::string parseType() {
        if (atEnd()) return {};

        // Qualifiers
        std::string qual;
        while (true) {
            if (peek()=='K') { ++p; qual = "const " + qual; }
            else if (peek()=='V') { ++p; qual = "volatile " + qual; }
            else if (peek()=='r') { ++p; qual = "__restrict " + qual; }
            else break;
        }

        std::string base;

        // Pointer / reference / rvalue-ref
        if (consume('P')) { std::string inner=parseType(); base=inner+"*"; }
        else if (consume('R')) { std::string inner=parseType(); base=inner+"&"; }
        else if (consume('O')) { std::string inner=parseType(); base=inner+"&&"; }
        else if (consume('U')) {
            // Vendor extended type (skip name)
            parseSourceName(); base = parseType();
        }
        else if (consume('A')) {
            // Array: A<len>_<type> or A_<type> (unsized)
            std::string dim;
            while (peek()&&peek()!='_'&&std::isdigit(static_cast<unsigned char>(peek())))
                dim += get();
            consume('_');
            std::string elem = parseType();
            base = elem + "[" + dim + "]";
        }
        else if (consume('F')) {
            // Function type: F <return-type> <param-type>* E
            std::string ret = parseType();
            std::vector<std::string> params;
            while (!atEnd() && peek()!='E') {
                std::string pt = parseType();
                if (pt.empty()) break;
                params.push_back(pt);
            }
            consume('E');
            base = ret + "()(";
            for (std::size_t i=0; i<params.size(); ++i) {
                if (i) base += ", ";
                base += params[i];
            }
            base += ")";
        }
        else if (consume('M')) {
            // Pointer to member: M <class-type> <member-type>
            std::string cls = parseType();
            std::string mem = parseType();
            base = mem + " " + cls + "::*";
        }
        else if (consume('T')) {
            // Template parameter: Ty = param y
            if (peek()=='_') { ++p; base="<T0>"; }
            else { int idx=0; while(std::isdigit(static_cast<unsigned char>(peek()))) idx=idx*10+(get()-'0'); consume('_'); base="<T"+std::to_string(idx+1)+">"; }
        }
        else if (peek()=='S') {
            base = trySubstitution();
            if (base.empty()) { ok=false; return {}; }
            // May be followed by template args
            if (peek()=='I') {
                std::vector<std::string> targs;
                std::string ta = parseTemplateArgs(&targs);
                base += ta;
            }
        }
        else if (peek()=='N') {
            base = parseNestedName(nullptr, nullptr, nullptr, nullptr);
        }
        else if (peek()=='Z') {
            // Local name type — skip
            ++p; base = "<local>";
        }
        else if (peek()=='D') {
            std::string bt = tryBuiltin();
            if (!bt.empty()) base = bt;
            else { ok=false; return {}; }
        }
        else {
            std::string bt = tryBuiltin();
            if (!bt.empty()) {
                base = bt;
            } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
                // Source name (class/struct type)
                std::string sn = parseSourceName();
                base = sn;
                if (peek()=='I') {
                    std::vector<std::string> targs;
                    std::string ta = parseTemplateArgs(&targs);
                    base += ta;
                }
                subs.push_back(base);
            } else {
                ok = false;
                return {};
            }
        }

        if (!qual.empty()) base = qual + base;
        return base;
    }

    // ── Nested name: N [CV] [ref] <prefix>* <unqualified-name> E ────────────

    std::string parseNestedName(std::string* outClass,
                                 bool* outConst, bool* outIsCtor,
                                 bool* outIsDtor)
    {
        if (!consume('N')) return {};

        bool isConst = false, isVolatile = false;
        while (true) {
            if (peek()=='K') { ++p; isConst=true; }
            else if (peek()=='V') { ++p; isVolatile=true; }
            else if (peek()=='R'&&p+1<end&&p[1]!='K') break;
            else break;
        }
        if (outConst) *outConst = isConst;

        std::vector<std::string> parts;
        while (!atEnd() && peek()!='E') {
            // Substitution
            if (peek()=='S') {
                std::string s = trySubstitution();
                if (s.empty()) break;
                if (peek()=='I') {
                    std::string ta = parseTemplateArgs();
                    s += ta;
                }
                parts.push_back(s);
                subs.push_back(s);
                continue;
            }
            // Template instantiation of last part
            if (peek()=='I') {
                if (!parts.empty()) {
                    std::string ta = parseTemplateArgs();
                    parts.back() += ta;
                    subs.push_back(parts.back());
                } else {
                    parseTemplateArgs(); // discard
                }
                continue;
            }
            // Ctor/Dtor
            bool isCtor=false, isDtor=false;
            std::string cd = tryCtorDtor(isCtor, isDtor);
            if (!cd.empty()) {
                if (outIsCtor) *outIsCtor=isCtor;
                if (outIsDtor) *outIsDtor=isDtor;
                parts.push_back(cd);
                continue;
            }
            // Operator
            std::string op = tryOperator();
            if (!op.empty()) { parts.push_back(op); continue; }
            // Source name
            if (std::isdigit(static_cast<unsigned char>(peek()))) {
                std::string sn = parseSourceName();
                parts.push_back(sn);
                subs.push_back(sn);
                continue;
            }
            break;
        }
        consume('E');

        if (outClass && parts.size() >= 2) {
            // Last part is the function name; everything before is the class chain.
            std::string cls;
            for (std::size_t i=0; i+1<parts.size(); ++i) {
                if (i) cls += "::";
                cls += parts[i];
            }
            *outClass = cls;
        }

        if (parts.empty()) return {};
        std::string result;
        for (std::size_t i=0; i<parts.size(); ++i) {
            if (i) result += "::";
            result += parts[i];
        }
        return result;
    }

    // ── Bare function type ────────────────────────────────────────────────────

    // Fills params. If isTemplate=true, the first type is the return type.
    void parseBareFunction(bool isTemplate,
                            std::string* retType,
                            std::vector<ParamInfo>* params)
    {
        if (isTemplate && retType) {
            *retType = parseType();
        }
        while (!atEnd()) {
            std::string t = parseType();
            if (t.empty() || !ok) break;
            if (t == "void" && params->empty()) break; // () → no params
            ParamInfo pi;
            // Strip trailing const/& for the ParamInfo fields
            if (t.size()>6 && t.substr(0,6)=="const ") {
                pi.isConst = true;
                t = t.substr(6);
            }
            if (!t.empty() && t.back()=='&') {
                pi.ref = RefCategory::LValueRef;
                t.pop_back();
                if (!t.empty() && t.back()=='&') {
                    pi.ref = RefCategory::RValueRef;
                    t.pop_back();
                }
            }
            pi.type = t;
            params->push_back(pi);
        }
    }
};

// ─── Itanium seeder implementation ───────────────────────────────────────────

class ItaniumSeeder : public ITypeSeeder {
public:
    const char* name() const noexcept override { return "Itanium"; }

    bool accepts(const std::string& s) const noexcept override {
        return s.size() > 3 && s[0]=='_' && s[1]=='Z';
    }

    SignatureInfo extract(const std::string& symbol) const override {
        SignatureInfo info;
        info.mangledName = symbol;

        // ── 1. Full demangled name via __cxa_demangle ─────────────────────────
#ifdef RETDEC_USE_CXXABI_DEMANGLE
        {
            int status = 0;
            char* d = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status);
            if (status == 0 && d) {
                info.demangledName = d;
                free(d);
            }
        }
#endif
        if (info.demangledName.empty()) {
            // Fallback: use our minimal parser result as demangled name
        }

        // ── 2. Structural parse ────────────────────────────────────────────────
        const char* raw = symbol.c_str();
        std::size_t  len = symbol.size();

        // Skip _Z
        ItaniumParser par(raw + 2, len - 2);

        // noreturn? (_ZNr...)  — actually 'r' prefix after _Z for some compilers
        // more commonly it's in the function attributes, skip for now.

        // Encoding
        std::string funcName;
        std::string className;
        bool isConst = false, isCtor = false, isDtor = false;
        bool isTemplate = false;

        if (par.peek()=='N') {
            funcName = par.parseNestedName(&className, &isConst, &isCtor, &isDtor);
            info.isConst = isConst;
            info.isConstructor = isCtor;
            info.isDestructor  = isDtor;
        } else if (par.peek()=='L') {
            // Local name: _ZL...
            ++par.p;
            funcName = par.parseNestedName(&className, &isConst, &isCtor, &isDtor);
        } else {
            // Unscoped name (free function or std:: shorthand)
            if (par.peek()=='S') {
                funcName = par.trySubstitution();
            } else {
                // Try operator
                funcName = par.tryOperator();
                if (funcName.empty()) {
                    // Source name
                    funcName = par.parseSourceName();
                }
            }
        }

        // After the name, template args may follow (for a template function)
        if (par.peek()=='I') {
            isTemplate = true;
            std::vector<std::string> targs;
            par.parseTemplateArgs(&targs);
            info.templateArgs = targs;
            // Append to function name for display
            if (!targs.empty()) {
                std::string ta = "<";
                for (std::size_t i=0; i<targs.size(); ++i) {
                    if (i) ta += ", ";
                    ta += targs[i];
                }
                ta += ">";
                funcName += ta;
            }
        }

        info.functionName = funcName;
        info.className    = className;

        // Rebuild namespace / class split
        {
            auto pos = className.rfind("::");
            if (pos != std::string::npos) {
                info.namespaceName = className.substr(0, pos);
                info.className     = className.substr(pos + 2);
            }
        }

        // this pointer for member functions
        if (!info.className.empty() && !info.isConstructor) {
            info.hasThis = true;
            std::string thisBase = className.empty() ? info.className : className;
            // Remove template args from this type for cleanliness
            auto lt = thisBase.find('<');
            if (lt != std::string::npos) thisBase = thisBase.substr(0, lt);
            info.thisType = (isConst ? "const " : "") + thisBase + "*";
        }

        // ── 3. Bare function type (parameters + optional return type) ─────────
        if (par.ok) {
            par.parseBareFunction(isTemplate, &info.returnType, &info.params);
        }

        // ── 4. If demangled name still empty, build it from parts ─────────────
        if (info.demangledName.empty()) {
            info.demangledName = funcName;
        }

        // Mark operators
        if (funcName.find("operator") != std::string::npos) {
            info.isOperator = true;
        }

        // Itanium always uses platform default CC (no encoding in the mangling)
        // For x86-32 GCC that is cdecl; for x86-64 it is SysVAmd64.
        // We emit Unknown and let the ABI descriptor decide.
        info.callingConvention = MangledCC::Unknown;

        return info;
    }
};

} // anon namespace

// ─── Public factory ───────────────────────────────────────────────────────────

std::unique_ptr<ITypeSeeder> makeItaniumSeeder() {
    return std::make_unique<ItaniumSeeder>();
}

} // namespace type_seed
} // namespace retdec
