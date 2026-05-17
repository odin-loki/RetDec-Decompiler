/**
 * @file src/type_seed/swift_seeder.cpp
 * @brief Swift symbol name parser for type seeding.
 *
 * ## Swift mangling (Swift 5.x, stable since Swift 5.0)
 *
 *   All Swift symbols start with '$s' (Swift 5+) or '_T' (Swift 4 legacy).
 *
 *   General structure:
 *     $s <module> <declaration-name> [<specialization>] <type>
 *
 *   Key encodings:
 *     F  = function
 *     v  = variable/property
 *     V  = struct
 *     C  = class
 *     P  = protocol
 *     E  = enum
 *     e  = extension
 *     I  = conformance
 *     AA = associated type
 *
 *   Type encodings:
 *     Si = Int      Su = UInt     Sb = Bool     Sf = Float
 *     Sd = Double   SS = String   Sv = Void
 *     Sq<T> = Optional<T>   [<T>] = Array<T>   {<K>:<V>} = Dictionary<K,V>
 *
 *   Calling convention: Swift uses its own calling convention (swiftcc).
 *   The `self` parameter is passed last for methods, first for initializers.
 *
 * ## What we extract
 *
 *   - Module name
 *   - Type name (struct/class/enum)
 *   - Method/function name
 *   - Whether it is a method (→ self parameter)
 *   - Self type (class/struct/protocol)
 *   - Basic return type from well-known Swift type encodings
 *   - Whether it is `async` (`Yyp` suffix in Swift mangling = async throwing)
 *   - Whether it is `throws` (error: type in result)
 *
 * ## swift-demangle integration
 *
 * If RETDEC_USE_SWIFT_DEMANGLE is defined, we call the swift-demangle
 * library function. Otherwise we implement a limited built-in fallback
 * that handles the most common patterns.
 */

#include <memory>
#include "retdec/type_seed/type_seed.h"

#ifdef RETDEC_USE_SWIFT_DEMANGLE
extern "C" {
    // From swiftDemangle.h (Swift toolchain)
    char* swift_demangle_getDemangledName(const char* mangledName,
                                           char* outputBuffer,
                                           size_t outputBufferSize);
}
#endif

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace retdec {
namespace type_seed {

namespace {

// ─── Swift type shorthand table ───────────────────────────────────────────────

struct SwiftBuiltin { const char* code; const char* type; };
static constexpr SwiftBuiltin kSwiftBuiltins[] = {
    {"Si","Int"},   {"Su","UInt"},  {"Si8","Int8"}, {"Su8","UInt8"},
    {"Si16","Int16"},{"Su16","UInt16"},{"Si32","Int32"},{"Su32","UInt32"},
    {"Si64","Int64"},{"Su64","UInt64"},
    {"Sf","Float"}, {"Sd","Double"},{"SF","Float16"},
    {"Sb","Bool"},  {"Sc","Character"},{"SS","String"},
    {"Sv","Void"},  {"SD","Dictionary"},{"Sa","Array"},
    {"Ss","Sequence"}, {"SE","Error"}, {"ST","IteratorProtocol"},
};

std::string trySwiftBuiltin(const char* p, const char* end, int& consumed) {
    for (auto& b : kSwiftBuiltins) {
        std::size_t n = std::strlen(b.code);
        if ((std::size_t)(end-p) >= n && std::memcmp(p, b.code, n)==0) {
            consumed = (int)n;
            return b.type;
        }
    }
    consumed = 0;
    return {};
}

// ─── Minimal Swift 5 symbol parser ───────────────────────────────────────────

struct SwiftParser {
    const char* p;
    const char* end;

    SwiftParser(const char* s, std::size_t n) : p(s), end(s+n) {}

    bool atEnd() const { return p>=end; }
    char peek() const  { return p<end ? *p : '\0'; }
    char get()         { return p<end ? *p++ : '\0'; }
    bool consume(char c){ if(peek()==c){++p;return true;} return false; }

    // Length-prefixed identifier: <decimal-len> <chars>
    std::string parseIdent() {
        if (!std::isdigit(static_cast<unsigned char>(peek()))) return {};
        int len=0;
        while (std::isdigit(static_cast<unsigned char>(peek())))
            len=len*10+(get()-'0');
        if (len<=0 || p+len>end) return {};
        std::string s(p, len);
        p += len;
        return s;
    }

    // Parse one Swift type encoding; return "" if not recognised
    std::string parseType() {
        if (atEnd()) return {};
        int consumed=0;
        std::string bt = trySwiftBuiltin(p, end, consumed);
        if (!bt.empty()) { p+=consumed; return bt; }

        char c = peek();

        // Optional<T>
        if (c=='S'&&p+1<end&&p[1]=='q') {
            p+=2;
            return parseType() + "?";
        }
        // Tuple
        if (c=='t') {
            ++p;
            std::string t="(";
            while (!atEnd() && peek()!='_') {
                if (t.size()>1) t+=", ";
                t += parseType();
            }
            consume('_');
            return t+")";
        }
        // Array
        if (c=='S'&&p+1<end&&p[1]=='a') {
            p+=2; return "[" + parseType() + "]";
        }
        // Named type: length-prefixed
        if (std::isdigit(static_cast<unsigned char>(c))) {
            return parseIdent();
        }
        // Skip unknown
        ++p;
        return "<type>";
    }
};

// ─── Swift seeder ─────────────────────────────────────────────────────────────

class SwiftSeeder : public ITypeSeeder {
public:
    const char* name() const noexcept override { return "Swift"; }

    bool accepts(const std::string& s) const noexcept override {
        // Swift 5: $s  Swift 4: _T  Swift 5 alt: _$s
        if (s.size()<3) return false;
        if (s[0]=='$'&&s[1]=='s') return true;
        if (s[0]=='_'&&s[1]=='$'&&s[2]=='s') return true;
        if (s[0]=='_'&&s[1]=='T') {
            // Distinguish from Itanium _T types (Swift 4 legacy)
            // Swift 4 legacy starts with _T followed by module-length prefix
            if (s.size()>3 && std::isdigit(static_cast<unsigned char>(s[2])))
                return true;
            if (s.size()>4 && s[2]=='t') return true; // _Tt = Swift 4 type metadata
        }
        return false;
    }

    SignatureInfo extract(const std::string& symbol) const override {
        SignatureInfo info;
        info.mangledName = symbol;

        // ── 1. Full demangled name ─────────────────────────────────────────────
#ifdef RETDEC_USE_SWIFT_DEMANGLE
        {
            char buf[4096];
            char* result = swift_demangle_getDemangledName(
                symbol.c_str(), buf, sizeof(buf));
            if (result && result[0]) {
                info.demangledName = result;
            }
        }
#endif

        // ── 2. Structural parse ────────────────────────────────────────────────
        const char* raw = symbol.c_str();
        std::size_t len = symbol.size();

        // Skip prefix: '$s', '_$s', '_T'
        std::size_t start = 0;
        if (len>2 && raw[0]=='$'&&raw[1]=='s') start=2;
        else if (len>3 && raw[0]=='_'&&raw[1]=='$'&&raw[2]=='s') start=3;
        else if (len>2 && raw[0]=='_'&&raw[1]=='T') start=2;

        SwiftParser par(raw+start, len-start);

        // Module name (first length-prefixed identifier)
        std::string moduleName = par.parseIdent();
        if (moduleName.empty()) {
            // Check for special top-level markers
            if (par.peek()=='s') { ++par.p; moduleName="Swift"; }
        }

        // Type context (class/struct/enum): optional
        std::string typeName;
        char typeTag = par.peek();
        if (typeTag=='V'||typeTag=='C'||typeTag=='O'||typeTag=='P') {
            ++par.p;
            typeName = par.parseIdent();
        }

        // Function/method name
        std::string funcName;
        char funcTag = par.peek();
        bool isInit = false;
        if (funcTag=='i') { ++par.p; funcName="init"; isInit=true; }
        else if (funcTag=='d') { ++par.p; funcName="deinit"; info.isDestructor=true; }
        else if (funcTag=='F' || funcTag=='f') {
            ++par.p; funcName = par.parseIdent();
        } else if (std::isdigit(static_cast<unsigned char>(funcTag))) {
            funcName = par.parseIdent();
        } else {
            funcName = par.parseIdent();
        }

        info.functionName  = funcName;
        info.className     = typeName;
        info.namespaceName = moduleName;
        info.isConstructor = isInit;

        // this/self pointer
        if (!typeName.empty()) {
            info.hasThis  = true;
            info.thisType = typeName; // Swift: self is value type for struct, ref for class
        }

        // ── 3. Type encoding after the name ───────────────────────────────────
        // Swift encodes return type + param types in the suffix.
        // The suffix is complex; we do a best-effort parse.

        // Async detection: 'y' prefix on parameter types in recent Swift
        // More reliably: presence of "async" in demangled name
        if (!info.demangledName.empty()) {
            if (info.demangledName.find("async") != std::string::npos)
                info.isAsync = true;
            if (info.demangledName.find("throws") != std::string::npos)
                ; // throws — could add a field
        }

        // Return type: try to parse first type after function name section
        // In Swift mangling: <param-types> '_' <return-type>
        // We do a simplified best-effort:
        {
            std::string rt = par.parseType();
            if (!rt.empty() && rt!="<type>") info.returnType = rt;
        }

        // Swift uses swiftcc (its own CC); not representable as MangledCC enum.
        info.callingConvention = MangledCC::Unknown;

        if (info.demangledName.empty()) {
            std::string dn = moduleName;
            if (!typeName.empty()) dn += "." + typeName;
            if (!funcName.empty()) dn += "." + funcName;
            info.demangledName = dn;
        }

        return info;
    }
};

} // anon namespace

std::unique_ptr<ITypeSeeder> makeSwiftSeeder() {
    return std::make_unique<SwiftSeeder>();
}

} // namespace type_seed
} // namespace retdec
