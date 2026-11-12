#pragma once
#include <cmath>
#include <functional>
// Minimal, strict JSON reader/writer (config.json, tokenizer.json, safetensors headers, manifests).
#include <forge/core/error.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace forge::json {

struct value;
using array = std::vector<value>;
using object = std::vector<std::pair<std::string, value>>;  // insertion order preserved

struct value {
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<array>, std::shared_ptr<object>> v = nullptr;
    value() = default;
    value(std::nullptr_t) {}
    value(bool b) : v(b) {}
    value(double d) : v(d) {}
    value(int d) : v(double(d)) {}
    value(long d) : v(double(d)) {}
    value(long long d) : v(double(d)) {}
    value(unsigned long d) : v(double(d)) {}
    value(std::string s) : v(std::move(s)) {}
    value(const char* s) : v(std::string(s)) {}
    value(array a) : v(std::make_shared<array>(std::move(a))) {}
    value(object o) : v(std::make_shared<object>(std::move(o))) {}

    bool is_null() const { return v.index() == 0; }
    bool is_bool() const { return v.index() == 1; }
    bool is_num() const { return v.index() == 2; }
    bool is_str() const { return v.index() == 3; }
    bool is_arr() const { return v.index() == 4; }
    bool is_obj() const { return v.index() == 5; }
    bool b() const { if (!is_bool()) raise(errc::parse_error, "json: expected bool"); return std::get<1>(v); }
    double num() const { if (!is_num()) raise(errc::parse_error, "json: expected number"); return std::get<2>(v); }
    std::int64_t i() const { return std::int64_t(num()); }
    const std::string& str() const { if (!is_str()) raise(errc::parse_error, "json: expected string"); return std::get<3>(v); }
    const array& arr() const { if (!is_arr()) raise(errc::parse_error, "json: expected array"); return *std::get<4>(v); }
    const object& obj() const { if (!is_obj()) raise(errc::parse_error, "json: expected object"); return *std::get<5>(v); }
    object& obj_mut() { if (!is_obj()) raise(errc::parse_error, "json: expected object"); return *std::get<5>(v); }
    const value* find(std::string_view k) const {
        if (!is_obj()) return nullptr;
        for (auto& [kk, vv] : obj()) if (kk == k) return &vv;
        return nullptr;
    }
    const value& operator[](std::string_view k) const {
        if (auto* p = find(k)) return *p;
        raise(errc::not_found, "json: missing key '{}'", k);
    }
    const value& operator[](std::size_t i) const { return arr().at(i); }
    double num_or(std::string_view k, double d) const { auto* p = find(k); return p && p->is_num() ? p->num() : d; }
    std::string str_or(std::string_view k, std::string d) const { auto* p = find(k); return p && p->is_str() ? p->str() : d; }
};

namespace detail {
struct parser {
    std::string_view s; std::size_t p = 0;
    [[noreturn]] void fail(const char* what) { raise(errc::parse_error, "json: {} at offset {}", what, p); }
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; }
    char peek() { ws(); if (p >= s.size()) fail("unexpected end"); return s[p]; }
    void expect(char c) { if (peek() != c) fail("unexpected character"); ++p; }
    static void utf8(std::string& o, std::uint32_t cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 63)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
    }
    std::uint32_t hex4() {
        if (p + 4 > s.size()) fail("bad \\u escape");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[p++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= std::uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= std::uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= std::uint32_t(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }
    std::string string() {
        expect('"'); std::string o;
        while (true) {
            if (p >= s.size()) fail("unterminated string");
            char c = s[p++];
            if (c == '"') return o;
            if (c != '\\') { o += c; continue; }
            if (p >= s.size()) fail("bad escape");
            char e = s[p++];
            switch (e) {
            case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
            case 'r': o += '\r'; break; case 't': o += '\t'; break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                    p += 2; std::uint32_t lo = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp); break;
            }
            default: fail("bad escape");
            }
        }
    }
    int depth = 0;
    value parse() {
        struct guard { int& d; ~guard() { --d; } } g{++depth};
        if (depth > 512) fail("nesting too deep");
        char c = peek();
        if (c == '{') {
            ++p; object o;
            if (peek() == '}') { ++p; return o; }
            while (true) {
                auto k = string(); expect(':'); o.emplace_back(std::move(k), parse());
                char d = peek(); ++p;
                if (d == '}') return o;
                if (d != ',') fail("expected , or }");
            }
        }
        if (c == '[') {
            ++p; array a;
            if (peek() == ']') { ++p; return a; }
            while (true) {
                a.push_back(parse());
                char d = peek(); ++p;
                if (d == ']') return a;
                if (d != ',') fail("expected , or ]");
            }
        }
        if (c == '"') return string();
        if (s.substr(p, 4) == "true") { p += 4; return true; }
        if (s.substr(p, 5) == "false") { p += 5; return false; }
        if (s.substr(p, 4) == "null") { p += 4; return nullptr; }
        if (s.substr(p, 3) == "NaN") { p += 3; return std::nan(""); }
        std::size_t st = p;
        if (s[p] == '-' || s[p] == '+') ++p;
        while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '-' || s[p] == '+')) ++p;
        if (st == p) fail("unexpected character");
        std::string txt(s.substr(st, p - st)); char* end = nullptr;
        double v = std::strtod(txt.c_str(), &end);
        if (end != txt.c_str() + txt.size()) fail("bad number");
        return v;
    }
};
inline void esc(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, 8, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    o += '"';
}
} // namespace detail

inline value parse(std::string_view s) {
    detail::parser ps{s};
    auto v = ps.parse(); ps.ws();
    if (ps.p != s.size()) ps.fail("trailing characters");
    return v;
}

inline void dump(std::string& o, const value& v) {
    switch (v.v.index()) {
    case 0: o += "null"; break;
    case 1: o += v.b() ? "true" : "false"; break;
    case 2: {
        double d = v.num();
        if (d == double(std::int64_t(d)) && std::fabs(d) < 1e15) o += std::to_string(std::int64_t(d));
        else { char b[32]; std::snprintf(b, 32, "%.9g", d); o += b; }
        break;
    }
    case 3: detail::esc(o, v.str()); break;
    case 4: { o += '['; bool f = true; for (auto& x : v.arr()) { if (!f) o += ','; f = false; dump(o, x); } o += ']'; break; }
    case 5: { o += '{'; bool f = true; for (auto& [k, x] : v.obj()) { if (!f) o += ','; f = false; detail::esc(o, k); o += ':'; dump(o, x); } o += '}'; break; }
    }
}
inline std::string dump(const value& v) { std::string o; dump(o, v); return o; }

} // namespace forge::json
