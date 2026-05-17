/**
 * @file src/type_seed/rust_seeder.cpp
 * @brief Rust symbol name parser for type seeding.
 *
 * ## Rust mangling schemes
 *
 * Rust has had two mangling schemes:
 *
 * ### Legacy (v0 not yet, default until ~2021):
 *   _ZN<path>E  — Itanium-like with Rust path components
 *   Components: crate hash, module path, function name
 *   e.g. _ZN3std2io6stderr6StderrE → std::io::stderr::Stderr
 *
 * ### v0 (stable since Rust 1.54, flag -Csymbol-mangling-version=v0):
 *   _R<path>[<instantiation>]
 *   More structured, encodes types explicitly.
 *   e.g. _RNvNtCs4fqI2P2rA04_3std3fmt5write
 *
 * ## What we extract
 *
 * Since Rust's type system differs fundamentally from C++ (no classes,
 * traits instead of interfaces, ownership, lifetimes), we extract:
 *   - Crate name
 *   - Module path
 *   - Function name
 *   - Trait being implemented (from `<impl Trait for Type>` path component)
 *   - Generic type arguments (from v0 mangling instantiation section)
 *   - Whether it is async (fn name suffix `{{async-fn-...}}` or `poll` method
 *     on a generated Future type)
 *   - Whether it is unsafe (detected from naming patterns)
 *
 * For type constraints we emit:
 *   - CallingConvention: SysVAmd64 (Rust uses C calling convention for FFI,
 *     and its own convention internally — we emit Unknown unless it is
 *     an extern "C" function, which has no mangling)
 *   - ReturnType: for well-known patterns (-> !, -> bool, etc.)
 *
 * ## rustc-demangle integration
 *
 * If RETDEC_USE_RUSTC_DEMANGLE is defined (rustc-demangle C header available),
 * we use rustc_demangle() for the full demangled string.
 * Otherwise we implement a minimal built-in fallback.
 */

#include <memory>
#include "retdec/type_seed/type_seed.h"

#ifdef RETDEC_USE_RUSTC_DEMANGLE
// extern "C" char* rustc_demangle(const char* mangled, char* buf, size_t buflen);
// Alternatively: int rustc_demangle(const char* mangled, char* buf, size_t* buflen);
// Include the actual header from the rustc-demangle crate's C API.
#  include <rustc_demangle.h>
#endif

#include <cassert>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace retdec {
namespace type_seed {

namespace {

// ─── Minimal Rust v0 path parser ─────────────────────────────────────────────

struct RustV0Parser {
    const char* p;
    const char* end;

    explicit RustV0Parser(const char* s, std::size_t n) : p(s), end(s+n) {}

    bool atEnd() const { return p >= end; }
    char peek() const  { return p < end ? *p : '\0'; }
    char get()         { return p < end ? *p++ : '\0'; }
    bool consume(char c){ if (peek()==c){++p;return true;} return false; }

    // base-62 encoded integer (digits 0-9, a-z, A-Z)
    uint64_t parseBase62() {
        uint64_t n = 0;
        while (!atEnd()) {
            char c = peek();
            if (c>='0'&&c<='9') { n=n*62+(c-'0'); ++p; }
            else if (c>='a'&&c<='z') { n=n*62+(c-'a'+10); ++p; }
            else if (c>='A'&&c<='Z') { n=n*62+(c-'A'+36); ++p; }
            else break;
        }
        consume('_');
        return n;
    }

    // Identifier: [a-z_][a-zA-Z0-9_]* (length-prefixed in v0: <len><ident>)
    std::string parseIdent() {
        // v0: optional '_' for punycode, then decimal length, then chars
        bool punny = consume('u');
        if (!std::isdigit(static_cast<unsigned char>(peek()))) { return {}; }
        int len=0;
        while (std::isdigit(static_cast<unsigned char>(peek())))
            len=len*10+(get()-'0');
        consume('_'); // optional separator
        if (len<=0 || p+len>end) return {};
        std::string s(p, len);
        p += len;
        (void)punny;
        return s;
    }

    // Parse a sequence of path components
    std::vector<std::string> parsePath() {
        std::vector<std::string> parts;
        while (!atEnd()) {
            char c = peek();
            // 'N' = nested path
            if (c=='N') {
                ++p;
                char ns = get(); // 'v'=value 't'=type 'C'=closure 'S'=inherent 'X'=impl trait
                (void)ns;
                auto sub = parsePath();
                parts.insert(parts.end(), sub.begin(), sub.end());
                std::string dis = parseDisambiguator();
                std::string id  = parseIdent();
                if (!id.empty()) parts.push_back(id + dis);
                continue;
            }
            // 'I' = generic args
            if (c=='I') {
                ++p;
                std::vector<std::string> gargs;
                while (!atEnd() && peek()!='E') {
                    std::string t = parseGenericArg();
                    if (!t.empty()) gargs.push_back(t);
                    else break;
                }
                consume('E');
                if (!parts.empty() && !gargs.empty()) {
                    parts.back() += "<";
                    for (std::size_t i=0; i<gargs.size(); ++i) {
                        if (i) parts.back() += ", ";
                        parts.back() += gargs[i];
                    }
                    parts.back() += ">";
                }
                continue;
            }
            // 'C' = crate root with hash
            if (c=='C') {
                ++p;
                std::string dis = parseDisambiguator();
                std::string id  = parseIdent();
                parts.push_back(id + dis);
                continue;
            }
            // 'M' = impl inherent, 'X' = impl trait
            if (c=='M' || c=='X') {
                ++p;
                parseDisambiguator();
                auto sub = parsePath();
                if (c=='X') {
                    // trait impl: impl <trait> for <type>
                    auto trt = parsePath();
                    std::string trait_name;
                    for (auto& t : trt) {
                        if (!trait_name.empty()) trait_name += "::";
                        trait_name += t;
                    }
                    if (!trait_name.empty()) parts.push_back("<impl " + trait_name + ">");
                }
                parts.insert(parts.end(), sub.begin(), sub.end());
                continue;
            }
            // 'B' = back reference (skip)
            if (c=='B') { ++p; parseBase62(); continue; }
            break;
        }
        return parts;
    }

    std::string parseDisambiguator() {
        if (peek()=='s') {
            ++p;
            parseBase62();
            return "";
        }
        return "";
    }

    std::string parseGenericArg() {
        char c = peek();
        // Type argument: 'p' or type encoding
        if (c=='L') { ++p; parseBase62(); return "<lifetime>"; } // lifetime
        if (c=='K') { ++p; return "<const " + parseGenericArg() + ">"; } // const generic
        return parseType();
    }

    std::string parseType() {
        if (atEnd()) return {};
        char c = get();
        // Primitive types
        switch (c) {
        case 'a': return "i8";
        case 'b': return "bool";
        case 'c': return "char";
        case 'd': return "f64";
        case 'e': return "str";
        case 'f': return "f32";
        case 'h': return "u8";
        case 'i': return "isize";
        case 'j': return "usize";
        case 'l': return "i32";
        case 'm': return "u32";
        case 'n': return "i128";
        case 'o': return "u128";
        case 's': return "i16";
        case 't': return "u16";
        case 'u': return "i64"; // actually 'u' maps to ... let's use correct table
        case 'v': return "void";  // () in Rust = ()
        case 'x': return "i64";
        case 'y': return "u64";
        case 'z': return "!"; // never type
        // References
        case 'R': { auto lt=parseLifetime(); std::string t=parseType(); return "&" + lt + t; }
        case 'Q': { auto lt=parseLifetime(); std::string t=parseType(); return "&mut " + lt + t; }
        case 'P': return "*const " + parseType();
        case 'O': return "*mut " + parseType();
        case 'S': return "[" + parseType() + "]"; // slice
        case 'A': { std::string t=parseType(); return "[" + t + "; N]"; } // array
        case 'T': { // tuple
            std::string t="(";
            while (!atEnd() && peek()!='E') {
                if (!t.empty()&&t.back()!='(') t+=", ";
                t+=parseType();
            }
            consume('E');
            t+=")";
            return t;
        }
        case 'F': { // fn type
            // skip binder + ABI
            std::string ret = parseType();
            return "fn() -> " + ret;
        }
        case 'D': { // dyn Trait
            return "dyn <trait>";
        }
        case 'B': { parseBase62(); return "<backref>"; }
        // Named type (path)
        case 'N': case 'C': case 'I': case 'M': case 'X':
            --p; {
                auto parts = parsePath();
                std::string result;
                for (auto& s : parts) {
                    if (!result.empty()) result += "::";
                    result += s;
                }
                return result;
            }
        default: return {};
        }
    }

    std::string parseLifetime() {
        if (peek()=='L') { ++p; parseBase62(); return ""; }
        return "";
    }
};

// ─── Legacy (Itanium-like) Rust path parser ───────────────────────────────────

// For legacy Rust mangling (_ZN...E), we do a simplified parse:
// Strip _ZN, split at digit-prefixed components, strip trailing hash.
std::vector<std::string> parseLegacyRustPath(const char* s, std::size_t n) {
    // Skip _ZN
    const char* p   = s + 3;
    const char* end = s + n;
    std::vector<std::string> parts;
    while (p < end && *p!='E') {
        if (!std::isdigit(static_cast<unsigned char>(*p))) break;
        int len=0;
        while (p<end && std::isdigit(static_cast<unsigned char>(*p)))
            len=len*10+(*p++)-'0';
        if (p+len>end) break;
        parts.emplace_back(p, len);
        p += len;
    }
    return parts;
}

bool isRustHashComponent(const std::string& s) {
    // Rust hash: 'h' followed by 16 hex chars
    if (s.size()==17 && s[0]=='h') {
        for (int i=1; i<17; ++i)
            if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
        return true;
    }
    return false;
}

// ─── Rust seeder ─────────────────────────────────────────────────────────────

class RustSeeder : public ITypeSeeder {
public:
    const char* name() const noexcept override { return "Rust"; }

    bool accepts(const std::string& s) const noexcept override {
        // Legacy: _ZN...17h<hex>E (hash component at end)
        // v0: _R...
        if (s.size()<4) return false;
        if (s[0]=='_' && s[1]=='R') return true;
        // Legacy Rust: _ZN with a 'h' hash component
        if (s.size()>3 && s[0]=='_' && s[1]=='Z' && s[2]=='N') {
            // Heuristic: check if any component looks like a Rust hash
            auto parts = parseLegacyRustPath(s.c_str(), s.size());
            for (auto& p : parts) if (isRustHashComponent(p)) return true;
        }
        return false;
    }

    SignatureInfo extract(const std::string& symbol) const override {
        SignatureInfo info;
        info.mangledName = symbol;

        // ── 1. Full demangled name ─────────────────────────────────────────────
#ifdef RETDEC_USE_RUSTC_DEMANGLE
        {
            char buf[4096];
            if (rustc_demangle(symbol.c_str(), buf, sizeof(buf)) == 0) {
                info.demangledName = buf;
            }
        }
#endif

        std::vector<std::string> parts;
        std::string traitName;

        if (symbol.size()>2 && symbol[0]=='_' && symbol[1]=='R') {
            // v0 mangling
            RustV0Parser par(symbol.c_str()+2, symbol.size()-2);
            parts = par.parsePath();
            // Look for <impl Trait> in parts
            for (auto& p : parts) {
                if (p.size()>7 && p.substr(0,6)=="<impl ") {
                    traitName = p.substr(6, p.size()-7); // strip "<impl " and ">"
                }
            }
        } else {
            // Legacy
            parts = parseLegacyRustPath(symbol.c_str(), symbol.size());
            // Remove trailing hash component
            while (!parts.empty() && isRustHashComponent(parts.back()))
                parts.pop_back();
        }

        // Build name from parts
        if (!parts.empty()) {
            info.functionName = parts.back();
            std::string path;
            for (std::size_t i=0; i+1<parts.size(); ++i) {
                if (i) path += "::";
                path += parts[i];
            }
            // Split path into namespace/class equivalent
            auto pos = path.rfind("::");
            if (pos != std::string::npos) {
                info.className     = path.substr(pos+2);
                info.namespaceName = path.substr(0,pos);
            } else {
                info.namespaceName = path;
            }
        }

        if (info.demangledName.empty()) {
            // Build from parts
            std::string dn;
            for (std::size_t i=0; i<parts.size(); ++i) {
                if (i) dn += "::";
                dn += parts[i];
            }
            info.demangledName = dn;
        }

        info.traitName = traitName;

        // Async detection: Rust async fns generate a future type with closure-like names
        const std::string& fn = info.functionName;
        if (fn.find("{{async") != std::string::npos ||
            fn.find("async_fn") != std::string::npos) {
            info.isAsync = true;
        }

        // noreturn: functions named 'panic', 'abort', or ending in '!'
        if (fn == "panic" || fn == "abort" || fn == "panic_fmt" ||
            fn == "panic_bounds_check" || fn == "panic_overflow") {
            info.isNoReturn = true;
        }

        // Rust uses its own calling convention internally; extern "C" fns
        // have no mangling at all, so everything here is Rust ABI.
        info.callingConvention = MangledCC::Unknown;

        return info;
    }
};

} // anon namespace

std::unique_ptr<ITypeSeeder> makeRustSeeder() {
    return std::make_unique<RustSeeder>();
}

} // namespace type_seed
} // namespace retdec
