#include "fuse/ui/doc/json.hpp"

#include <charconv>
#include <cstdlib>

namespace fuse::ui::doc {

const JsonValue* JsonValue::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

namespace {
constexpr int kMaxDepth = 128;

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    JsonError err;
    bool failed = false;

    bool fail(const char* msg) {
        if (failed) return false;
        failed = true;
        err.line = 1;
        err.column = 1;
        for (std::size_t k = 0; k < i && k < s.size(); ++k) {
            if (s[k] == '\n') { ++err.line; err.column = 1; }
            else ++err.column;
        }
        err.message = msg;
        return false;
    }
    void ws() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                const auto e = s.find("*/", i + 2);
                i = e == std::string_view::npos ? s.size() : e + 2;
            } else break;
        }
    }
    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(std::uint32_t& v) {
        if (i + 4 > s.size()) return fail("truncated \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= std::uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= std::uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= std::uint32_t(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        return true;
    }
    bool str(std::string& out) {
        ++i; // opening quote
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            const char c = s[i++];
            if (c == '"') return true;
            if (std::uint8_t(c) < 0x20) return fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return fail("unterminated escape");
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    std::uint32_t lo;
                    if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return fail("lone high surrogate");
                    i += 2;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo >= 0xE000) return fail("bad low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) return fail("lone low surrogate");
                utf8(out, cp);
                break;
            }
            default: return fail("unknown escape");
            }
        }
    }
    bool value(JsonValue& v, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end of input");
        const char c = s[i];
        if (c == '{') {
            v.type = JsonValue::Type::Object;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            while (true) {
                ws();
                if (i < s.size() && s[i] == '}') { ++i; return true; } // trailing comma
                if (i >= s.size() || s[i] != '"') return fail("expected member name");
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                v.object.emplace_back(std::move(key), JsonValue{});
                if (!value(v.object.back().second, depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = JsonValue::Type::Array;
            ++i;
            while (true) {
                ws();
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                v.array.emplace_back();
                if (!value(v.array.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = JsonValue::Type::String; return str(v.string); }
        auto word = [&](std::string_view w) {
            if (s.substr(i, w.size()) != w) return false;
            i += w.size();
            return true;
        };
        if (word("true")) { v.type = JsonValue::Type::Bool; v.boolean = true; return true; }
        if (word("false")) { v.type = JsonValue::Type::Bool; return true; }
        if (word("null")) return true;
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t j = i;
            if (s[j] == '-') ++j;
            if (j >= s.size() || !(s[j] >= '0' && s[j] <= '9')) return fail("bad number");
            if (s[j] == '0' && j + 1 < s.size() && s[j + 1] >= '0' && s[j + 1] <= '9') return fail("leading zero");
            while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.' || s[j] == 'e' || s[j] == 'E' ||
                                    s[j] == '+' || s[j] == '-'))
                ++j;
            const auto r = std::from_chars(s.data() + i, s.data() + j, v.number);
            if (r.ec != std::errc{} || r.ptr != s.data() + j) return fail("bad number");
            v.type = JsonValue::Type::Number;
            i = j;
            return true;
        }
        return fail("unexpected character");
    }
};
} // namespace

std::expected<JsonValue, JsonError> parseJson(std::string_view text) {
    Parser p;
    p.s = text;
    JsonValue v;
    if (!p.value(v, 0)) return std::unexpected(p.err);
    p.ws();
    if (p.i != text.size()) {
        p.fail("trailing characters");
        return std::unexpected(p.err);
    }
    return v;
}

} // namespace fuse::ui::doc
