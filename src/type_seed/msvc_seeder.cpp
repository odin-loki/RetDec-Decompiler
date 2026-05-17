/**
 * @file src/type_seed/msvc_seeder.cpp
 * @brief MSVC name mangling parser for type seeding.
 *
 * ## MSVC mangling grammar (subset)
 *
 *   <mangled-name>    := '?' <decorated-name>
 *   <decorated-name>  := <symbol-name> '@' <scope-chain> '@' <type-encoding>
 *   <symbol-name>     := identifier | '?' <operator-code> | '?' '$' <template>
 *   <scope-chain>     := '@'               (global)
 *                      | <scope>+ '@'
 *   <scope>           := <source-name> '@'
 *   <type-encoding>   := <function-type>   (for functions)
 *                      | <data-type>        (for variables)
 *   <function-type>   := <calling-conv> <cv-qualifier-this> '@'
 *                        <return-type> <param-type>* '@' 'Z'
 *   <calling-conv>    := 'A'=__cdecl  'C'=__pascal  'E'=__thiscall
 *                        'G'=__stdcall 'I'=__fastcall 'K'=? 'M'=__clrcall
 *                        'O'=__eabi   'Q'=__vectorcall 'S'=__swift_async
 *   <cv-qualifier>    := 'A'=none 'B'=const 'C'=volatile 'D'=const volatile
 *   <return-type>     := '@' (void/ctor/dtor) | <type>
 *   <param-type>      := <type> | '@' (end)
 *   <type>            := <builtin-type> | 'P'<type> | 'R'<type> | 'Q'<type>
 *                      | 'A'<type> | 'U'<name> | 'V'<name> | 'W'<name>
 *                      | 'T'<name> | '?' <type> (back-reference)
 *                      | 'X' (void) | 'Z' (varargs '...')
 *   <builtin-type>    := 'C'=signed char 'D'=char 'E'=unsigned char
 *                        'F'=short 'G'=unsigned short 'H'=int 'I'=unsigned int
 *                        'J'=long 'K'=unsigned long 'L'=long long? 'M'=float
 *                        'N'=double 'O'=long double '_D'=__int8 '_E'=unsigned __int8
 *                        '_F'=__int16 '_G'=unsigned __int16 '_H'=__int32
 *                        '_I'=unsigned __int32 '_J'=__int64 '_K'=unsigned __int64
 *                        '_L'=__int128 '_M'=unsigned __int128 '_N'=bool
 *                        '_W'=wchar_t '_S'=char16_t '_U'=char32_t
 *
 * ## What we extract
 *
 *   - Full qualified name (namespace + class + function)
 *   - Calling convention (AA=cdecl, AE=thiscall, AG=stdcall, AI=fastcall, AQ=vectorcall)
 *   - CV-qualifier on `this`
 *   - Return type
 *   - All parameter types
 *   - Class membership (→ this pointer)
 *   - Constructor/destructor/operator status
 */

#include <memory>
#include "retdec/type_seed/type_seed.h"

#include <cassert>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace type_seed {

namespace {

// ─── MSVC parser ─────────────────────────────────────────────────────────────

struct MsvcParser {
    const char* p;
    const char* end;
    bool        ok = true;

    // Back-reference table for types (0..9)
    std::string typeBackRef[10];
    int         typeBackRefCount = 0;

    // Back-reference table for names (0..9)
    std::string nameBackRef[10];
    int         nameBackRefCount = 0;

    explicit MsvcParser(const char* s, std::size_t n) : p(s), end(s+n) {}

    bool atEnd() const { return p >= end; }
    char peek() const  { return p < end ? *p : '\0'; }
    char get()         { return p < end ? *p++ : '\0'; }
    bool consume(char c){ if (peek()==c){++p;return true;} return false; }

    void addTypeBackRef(const std::string& t) {
        if (typeBackRefCount < 10) typeBackRef[typeBackRefCount++] = t;
    }
    void addNameBackRef(const std::string& n) {
        if (nameBackRefCount < 10) nameBackRef[nameBackRefCount++] = n;
    }

    // ── Source name (@ terminated) ────────────────────────────────────────────

    std::string parseAtName() {
        const char* start = p;
        while (!atEnd() && peek()!='@') ++p;
        std::string s(start, p-start);
        consume('@');
        return s;
    }

    // ── Scope chain ───────────────────────────────────────────────────────────

    // Parses all scope components until '@@' (global scope sentinel) or '@'
    // Returns components from innermost to outermost.
    std::vector<std::string> parseScopeChain() {
        std::vector<std::string> scopes;
        while (!atEnd()) {
            if (peek()=='@') { ++p; break; } // end of scope chain
            // Back-reference
            if (peek()>='0' && peek()<='9') {
                int idx = get()-'0';
                if (idx < nameBackRefCount) scopes.push_back(nameBackRef[idx]);
                else scopes.push_back("<backref?>");
                continue;
            }
            // Template instance: '?' '$' <name> '@' <template-args>
            if (peek()=='?' && p+1<end && p[1]=='$') {
                p += 2;
                std::string tname = parseAtName();
                // Template args follow — skip to next '@' boundary for simplicity
                // (parse individual args)
                std::vector<std::string> targs;
                while (!atEnd() && peek()!='@') {
                    std::string t = parseType();
                    if (!t.empty()) targs.push_back(t);
                    else { ++p; break; }
                }
                consume('@');
                std::string tinst = tname;
                if (!targs.empty()) {
                    tinst += "<";
                    for (std::size_t i=0; i<targs.size(); ++i) {
                        if (i) tinst += ",";
                        tinst += targs[i];
                    }
                    tinst += ">";
                }
                scopes.push_back(tinst);
                addNameBackRef(tinst);
                continue;
            }
            std::string s = parseAtName();
            if (s.empty()) break;
            scopes.push_back(s);
            addNameBackRef(s);
        }
        return scopes;
    }

    // ── Operator decoding ─────────────────────────────────────────────────────

    std::string decodeOperator() {
        // After '?' but before scope chain — operator code
        static const struct { char c; const char* name; } ops[] = {
            {'0',"constructor"}, {'1',"destructor"}, {'2',"operator new"},
            {'3',"operator delete"}, {'4',"operator="}, {'5',"operator>>"},
            {'6',"operator<<"}, {'7',"operator!"}, {'8',"operator=="},
            {'9',"operator!="}, {'A',"operator[]"}, {'B',"operator <cast>"},
            {'C',"operator->"}, {'D',"operator*"}, {'E',"operator++"},
            {'F',"operator--"}, {'G',"operator-"}, {'H',"operator+"},
            {'I',"operator&"}, {'J',"operator->*"}, {'K',"operator/"},
            {'L',"operator%"}, {'M',"operator<"}, {'N',"operator<="},
            {'O',"operator>"}, {'P',"operator>="}, {'Q',"operator,"},
            {'R',"operator()"}, {'S',"operator~"}, {'T',"operator^"},
            {'U',"operator|"}, {'V',"operator&&"}, {'W',"operator||"},
            {'X',"operator*="}, {'Y',"operator+="}, {'Z',"operator-="},
        };
        char c = peek();
        for (auto& e : ops) {
            if (e.c == c) { ++p; return e.name; }
        }
        // Two-char operators '_' <c>
        if (c == '_' && p+1<end) {
            ++p;
            static const struct { char c; const char* name; } ops2[] = {
                {'0',"operator/="}, {'1',"operator%="}, {'2',"operator>>="},
                {'3',"operator<<="}, {'4',"operator&="}, {'5',"operator|="},
                {'6',"operator^="}, {'7',"vftable"}, {'8',"vbtable"},
                {'9',"vcall"}, {'A',"typeof"}, {'B',"local static guard"},
                {'C',"string"}, {'D',"vbase destructor"}, {'E',"vector deleting destructor"},
                {'F',"default constructor closure"}, {'G',"scalar deleting destructor"},
                {'H',"vector constructor iterator"}, {'I',"vector destructor iterator"},
                {'J',"vector vbase constructor iterator"}, {'K',"virtual displacement map"},
                {'L',"eh vector constructor iterator"}, {'M',"eh vector destructor iterator"},
                {'N',"eh vector vbase constructor iterator"}, {'O',"copy constructor closure"},
                {'S',"local vftable"}, {'T',"local vftable constructor closure"},
                {'U',"operator new[]"}, {'V',"operator delete[]"},
            };
            for (auto& e : ops2) {
                if (e.c==peek()) { ++p; return e.name; }
            }
            return "<op?>";
        }
        return {};
    }

    // ── Calling convention ────────────────────────────────────────────────────

    MangledCC parseCalling() {
        char c = get();
        switch (c) {
        case 'A': return MangledCC::Cdecl;
        case 'C': return MangledCC::Pascal;
        case 'E': return MangledCC::Thiscall;
        case 'G': return MangledCC::Stdcall;
        case 'I': return MangledCC::Fastcall;
        case 'M': return MangledCC::Clrcall;
        case 'O': return MangledCC::Cdecl;    // __eabi (ARM)
        case 'Q': return MangledCC::Vectorcall;
        default:  return MangledCC::Unknown;
        }
    }

    // ── CV qualifier for `this` ───────────────────────────────────────────────

    // Returns isConst
    bool parseCVThis() {
        char c = peek();
        if (c=='A') { ++p; return false; }  // no qualifiers
        if (c=='B') { ++p; return true;  }  // const
        if (c=='C') { ++p; return false; }  // volatile
        if (c=='D') { ++p; return true;  }  // const volatile
        return false;
    }

    // ── Type ──────────────────────────────────────────────────────────────────

    std::string parseType() {
        if (atEnd()) return {};
        char c = peek();

        // Pointer / reference modifier
        bool isPtr=false, isRef=false, isArray=false;
        std::string ptrQual;
        if (c=='P'||c=='Q'||c=='R'||c=='S') {
            if (c=='P') isPtr=true;
            else if (c=='R') isRef=true;
            else if (c=='Q') { isArray=true; } // array ref
            ++p;
            // CV qualifier on pointee: A=none, B=const, C=volatile, D=const volatile
            char cv = peek();
            if (cv=='A') ++p;
            else if (cv=='B') { ++p; ptrQual="const "; }
            else if (cv=='C') { ++p; ptrQual="volatile "; }
            else if (cv=='D') { ++p; ptrQual="const volatile "; }
        } else if (c=='A') {
            isRef=true; ++p;
            char cv=peek();
            if (cv=='A'||cv=='B'||cv=='C'||cv=='D') {
                if (cv=='B') ptrQual="const ";
                ++p;
            }
        }

        std::string base = parseBaseType();
        if (base.empty()) return {};

        std::string result;
        if (!ptrQual.empty()) result = ptrQual + base;
        else result = base;

        if (isPtr)   result += "*";
        if (isRef)   result += "&";
        if (isArray) result += "[]";

        addTypeBackRef(result);
        return result;
    }

    std::string parseBaseType() {
        if (atEnd()) return {};
        char c = get();

        // Builtin types (uppercase)
        switch (c) {
        case 'X': return "void";
        case 'D': return "char";
        case 'C': return "signed char";
        case 'E': return "unsigned char";
        case 'F': return "short";
        case 'G': return "unsigned short";
        case 'H': return "int";
        case 'I': return "unsigned int";
        case 'J': return "long";
        case 'K': return "unsigned long";
        case 'L': return "long";        // MS long
        case 'M': return "float";
        case 'N': return "double";
        case 'O': return "long double";
        case 'Z': return "...";
        case '_': {
            char c2 = get();
            switch (c2) {
            case 'D': return "__int8";
            case 'E': return "unsigned __int8";
            case 'F': return "__int16";
            case 'G': return "unsigned __int16";
            case 'H': return "__int32";
            case 'I': return "unsigned __int32";
            case 'J': return "__int64";
            case 'K': return "unsigned __int64";
            case 'L': return "__int128";
            case 'M': return "unsigned __int128";
            case 'N': return "bool";
            case 'W': return "wchar_t";
            case 'S': return "char16_t";
            case 'U': return "char32_t";
            case 'Z': return "...";
            default:  return "<_" + std::string(1,c2) + ">";
            }
        }
        // Class/struct/union/enum types: U/V/W/T followed by @-terminated name
        case 'U': case 'V': case 'W': case 'T': {
            std::string sname = parseAtName();
            if (sname.empty()) {
                // Possibly back-reference
            }
            addNameBackRef(sname);
            return sname;
        }
        // Function pointer type: follow with calling-conv + type sequence
        case '6': {
            // function pointer: '6' <calling> <ret-type> <param>* '@'
            MangledCC cc = parseCalling();
            (void)cc;
            std::string ret = parseType();
            std::vector<std::string> params;
            while (!atEnd() && peek()!='@' && peek()!='Z') {
                std::string pt = parseType();
                if (!pt.empty()) params.push_back(pt);
                else break;
            }
            consume('@'); consume('Z');
            std::string fs = ret + "(*)(" ;
            for (std::size_t i=0; i<params.size(); ++i) {
                if (i) fs += ", ";
                fs += params[i];
            }
            fs += ")";
            return fs;
        }
        // Back reference 0..9
        default:
            if (c>='0' && c<='9') {
                int idx = c-'0';
                if (idx < typeBackRefCount) return typeBackRef[idx];
                return "<backref?>";
            }
            return {};
        }
    }
};

// ─── MSVC seeder ─────────────────────────────────────────────────────────────

class MsvcSeeder : public ITypeSeeder {
public:
    const char* name() const noexcept override { return "MSVC"; }

    bool accepts(const std::string& s) const noexcept override {
        return !s.empty() && s[0]=='?';
    }

    SignatureInfo extract(const std::string& symbol) const override {
        SignatureInfo info;
        info.mangledName = symbol;

        const char* raw = symbol.c_str();
        std::size_t len  = symbol.size();
        MsvcParser  par(raw+1, len-1); // skip leading '?'

        // ── 1. Symbol name ────────────────────────────────────────────────────
        std::string funcName;
        bool isCtor=false, isDtor=false, isOp=false;

        if (par.peek()=='?') {
            // Operator or special name
            ++par.p;
            std::string op = par.decodeOperator();
            if (!op.empty()) funcName = op;
            if (op == "constructor") { isCtor=true; funcName = "<constructor>"; }
            if (op == "destructor")  { isDtor=true; funcName = "<destructor>";  }
            isOp = (!isCtor && !isDtor);
        } else {
            // Regular identifier
            funcName = par.parseAtName();
        }

        info.functionName  = funcName;
        info.isConstructor = isCtor;
        info.isDestructor  = isDtor;
        info.isOperator    = isOp;

        // ── 2. Scope chain ────────────────────────────────────────────────────
        std::vector<std::string> scopes = par.parseScopeChain();
        // scopes[0] = innermost scope (immediate class), scopes[1..] = outer namespaces
        if (!scopes.empty()) {
            info.className = scopes[0];
            std::string ns;
            for (std::size_t i=1; i<scopes.size(); ++i) {
                if (i>1) ns = "::" + ns;
                ns = scopes[i] + (ns.empty() ? "" : "::") + ns;
            }
            info.namespaceName = ns;
        }

        // Build demangled name
        {
            std::string dn;
            for (int i=(int)scopes.size()-1; i>=0; --i) {
                if (!dn.empty()) dn += "::";
                dn += scopes[i];
            }
            if (!dn.empty()) dn += "::";
            dn += funcName;
            info.demangledName = dn;
        }

        // ── 3. Access/storage specifier (skip 1 char) ─────────────────────────
        // Format: <access><storage> where access=[A-Z] storage=[A-Z]
        // We skip the access level and static/virtual modifiers.
        // The next meaningful chars are calling convention + cv-this
        if (!par.atEnd()) { par.get(); } // access flags

        // ── 4. Calling convention + CV-this ───────────────────────────────────
        if (!par.atEnd()) {
            info.callingConvention = par.parseCalling();
        }
        bool isConst = false;
        if (!par.atEnd()) {
            isConst = par.parseCVThis();
        }
        info.isConst = isConst;

        // Class membership → this pointer
        if (!info.className.empty()) {
            info.hasThis = true;
            std::string thisBase = info.className;
            auto lt = thisBase.find('<');
            if (lt != std::string::npos) thisBase = thisBase.substr(0,lt);
            info.thisType = (isConst?"const ":"") + thisBase + "*";
        }

        // ── 5. '@' separator before return type ───────────────────────────────
        par.consume('@');

        // ── 6. Return type ────────────────────────────────────────────────────
        if (!par.atEnd() && par.peek()!='@') {
            info.returnType = par.parseType();
        }
        if (isCtor || isDtor) info.returnType = "void";

        // ── 7. Parameter types until '@' or 'Z' ──────────────────────────────
        while (!par.atEnd() && par.peek()!='@' && par.peek()!='Z') {
            std::string pt = par.parseType();
            if (pt.empty()) break;
            ParamInfo pi;
            // Strip leading const
            if (pt.size()>6 && pt.substr(0,6)=="const ") {
                pi.isConst = true; pt = pt.substr(6);
            }
            if (!pt.empty() && pt.back()=='&') {
                pi.ref = RefCategory::LValueRef; pt.pop_back();
            }
            pi.type = pt;
            info.params.push_back(pi);
        }

        return info;
    }
};

} // anon namespace

std::unique_ptr<ITypeSeeder> makeMsvcSeeder() {
    return std::make_unique<MsvcSeeder>();
}

} // namespace type_seed
} // namespace retdec
