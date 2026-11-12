// forge::os::locale: ICU MessageFormat (v1) subset: {arg}, number/date/time styles, plural (offset, =N, #),
// selectordinal, select, and apostrophe quoting.
#include "locale_internal.hpp"
#include <cerrno>
#include <cmath>

namespace forge::os::locale {
using namespace detail;
namespace {
struct mf {
    std::string_view p; std::size_t i = 0; const message_args& args; std::string loc; std::string err;
    bool fail(std::string m) { if (err.empty()) err = m + " at offset " + std::to_string(i); return false; }
    void ws() { while (i < p.size() && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) ++i; }
    std::string ident() { ws(); std::size_t s = i; while (i < p.size() && p[i] != ',' && p[i] != '}' && p[i] != '{' && p[i] != ' ' && p[i] != '\t' && p[i] != '\n') ++i; return std::string(p.substr(s, i - s)); }
    static std::optional<double> as_num(const message_value& v) {
        if (auto x = std::get_if<std::int64_t>(&v)) return double(*x);
        if (auto x = std::get_if<double>(&v)) return *x;
        if (auto x = std::get_if<std::string>(&v)) { char* e; double d = std::strtod(x->c_str(), &e); if (e != x->c_str()) return d; }
        return std::nullopt;
    }
    std::string fmt_num(double v, std::string_view style) {
        if (style.empty()) return format_number(v, loc);
        if (style == "integer") { number_options o; o.max_fraction = 0; return format_number(v, loc, o); }
        if (style == "percent") return format_percent(v, loc);
        if (style.starts_with("currency")) { auto c = style.size() > 9 ? style.substr(9) : std::string_view("USD"); return format_currency(v, c, loc); }
        if (style.starts_with("::")) {   // tiny number skeleton subset
            number_options o; auto sk = style.substr(2);
            if (auto d = sk.find('.'); d != std::string_view::npos) { auto f = sk.substr(d + 1); int mn = 0, mx = 0; for (char ch : f) { if (ch == '0') { ++mn; ++mx; } else if (ch == '#') ++mx; } o.min_fraction = mn; o.max_fraction = mx; }
            if (sk.find("group-off") != std::string_view::npos) o.grouping = false;
            if (sk.starts_with("percent")) return format_percent(v, loc, o);
            if (sk.starts_with("compact-short")) return format_compact(v, loc, compact_width::short_);
            if (sk.starts_with("compact-long")) return format_compact(v, loc, compact_width::long_);
            if (sk.starts_with("currency/")) return format_currency(v, sk.substr(9, 3), loc);
            return format_number(v, loc, o);
        }
        return format_number(v, loc);
    }
    static date_style style_of(std::string_view s, date_style def) {
        if (s == "short") return date_style::short_; if (s == "medium") return date_style::medium;
        if (s == "long") return date_style::long_; if (s == "full") return date_style::full; return def;
    }
    // Skips a {...} sub-message without output.
    bool skip_block() {
        if (i >= p.size() || p[i] != '{') return fail("expected '{'");
        int depth = 0; bool q = false;
        for (; i < p.size(); ++i) {
            char c = p[i];
            if (c == '\'') { if (i + 1 < p.size() && p[i + 1] == '\'') { ++i; continue; } if (!q && i + 1 < p.size() && (p[i + 1] == '{' || p[i + 1] == '}' || p[i + 1] == '#' || p[i + 1] == '|')) { q = true; continue; } if (q) { q = false; continue; } continue; }
            if (q) continue;
            if (c == '{') ++depth; else if (c == '}' && --depth == 0) { ++i; return true; }
        }
        return fail("unterminated sub-message");
    }
    // Formats text until a closing '}' at depth 0 (or end). pound: replacement for '#'.
    bool text(std::string& out, const std::string* pound, bool nested) {
        while (i < p.size()) {
            char c = p[i];
            if (c == '\'') {
                if (i + 1 < p.size() && p[i + 1] == '\'') { out += '\''; i += 2; continue; }
                if (i + 1 < p.size() && (p[i + 1] == '{' || p[i + 1] == '}' || p[i + 1] == '#' || p[i + 1] == '|')) {
                    ++i;
                    while (i < p.size()) { if (p[i] == '\'') { if (i + 1 < p.size() && p[i + 1] == '\'') { out += '\''; i += 2; continue; } ++i; break; } out += p[i++]; }
                    continue;
                }
                out += c; ++i; continue;
            }
            if (c == '}') { if (nested) return true; return fail("unbalanced '}'"); }
            if (c == '#' && pound) { out += *pound; ++i; continue; }
            if (c == '{') { ++i; if (!argument(out)) return false; continue; }
            out += c; ++i;
        }
        if (nested) return fail("missing '}'");
        return true;
    }
    bool argument(std::string& out) {
        auto name = ident(); ws();
        if (name.empty()) return fail("empty argument name");
        auto it = args.find(name);
        if (i < p.size() && p[i] == '}') {
            ++i;
            if (it == args.end()) { out += "{" + name + "}"; return true; }
            auto& v = it->second;
            if (auto s = std::get_if<std::string>(&v)) out += *s;
            else if (auto d = std::get_if<datetime>(&v)) out += format_datetime(*d, loc);
            else if (auto n = as_num(v)) out += fmt_num(*n, {});
            return true;
        }
        if (i >= p.size() || p[i] != ',') return fail("expected ',' or '}'");
        ++i; auto type = ident(); ws();
        std::string style;
        if (type == "plural" || type == "selectordinal" || type == "select") {
            if (i >= p.size() || p[i] != ',') return fail("expected ','"); ++i;
            return choice(out, type, it == args.end() ? nullptr : &it->second);
        }
        if (i < p.size() && p[i] == ',') { ++i; ws(); std::size_t s = i; int depth = 0; while (i < p.size() && (p[i] != '}' || depth)) { if (p[i] == '{') ++depth; if (p[i] == '}') --depth; ++i; } style = std::string(p.substr(s, i - s)); while (!style.empty() && style.back() == ' ') style.pop_back(); }
        if (i >= p.size() || p[i] != '}') return fail("expected '}'"); ++i;
        if (it == args.end()) { out += "{" + name + "}"; return true; }
        auto& v = it->second;
        if (type == "number") { auto n = as_num(v); if (!n) return fail("argument '" + name + "' is not a number"); out += fmt_num(*n, style); return true; }
        if (type == "date" || type == "time") {
            auto d = std::get_if<datetime>(&v);
            datetime tmp; if (!d) { auto n = as_num(v); if (!n) return fail("argument '" + name + "' is not a date"); tmp = from_time_utc(std::chrono::system_clock::time_point(std::chrono::seconds(std::int64_t(*n)))); d = &tmp; }
            if (style.starts_with("::")) { out += format_skeleton(*d, std::string_view(style).substr(2), loc); return true; }
            if (!style.empty() && style != "short" && style != "medium" && style != "long" && style != "full") { out += format_pattern(*d, style, loc); return true; }
            out += type == "date" ? format_date(*d, loc, style_of(style, date_style::medium)) : format_time(*d, loc, style_of(style, date_style::short_));
            return true;
        }
        if (type == "spellout" || type == "ordinal" || type == "duration") { auto n = as_num(v); out += n ? fmt_num(*n, {}) : ""; return true; }
        return fail("unknown argument type '" + type + "'");
    }
    bool choice(std::string& out, const std::string& type, const message_value* v) {
        double offset = 0; std::optional<double> n; std::string sel;
        if (v) { if (type == "select") { if (auto s = std::get_if<std::string>(v)) sel = *s; else if (auto x = as_num(*v)) sel = format_number(*x, "en", number_options{.grouping = false}); } else n = as_num(*v); }
        ws();
        if (type == "plural" && p.substr(i).starts_with("offset:")) { i += 7; ws(); std::size_t s = i; while (i < p.size() && (std::isdigit((unsigned char)p[i]) || p[i] == '.')) ++i; offset = std::strtod(std::string(p.substr(s, i - s)).c_str(), nullptr); }
        std::string cat;
        if (n && type != "select") cat = plural_name(plural_category(*n - offset, loc, type == "selectordinal"));
        std::string pound; if (n) pound = fmt_num(*n - offset, {});
        bool done = false; std::size_t other_at = std::string::npos, chosen_at = std::string::npos;
        while (true) {
            ws(); if (i >= p.size()) return fail("unterminated choice");
            if (p[i] == '}') { ++i; break; }
            auto key = ident(); ws();
            if (key.empty()) return fail("expected selector");
            std::size_t at = i;
            if (!skip_block()) return false;
            if (done) continue;
            bool match = false;
            if (type == "select") match = key == sel;
            else if (key[0] == '=') match = n && std::strtod(key.c_str() + 1, nullptr) == *n;
            else match = key == cat;
            if (key == "other") other_at = at;
            if (match) { chosen_at = at; done = key[0] == '=' || type == "select"; if (!done && key != "other") done = true; }
        }
        std::size_t use = chosen_at != std::string::npos ? chosen_at : other_at;
        if (use == std::string::npos) return fail("missing 'other' case");
        std::size_t save = i; i = use + 1;
        std::string sub_out; bool ok = text(sub_out, type == "select" ? nullptr : &pound, true);
        i = save; if (!ok) return false; out += sub_out; return true;
    }
};
}  // namespace

result<std::string> format_message(std::string_view pattern, const message_args& args, std::string_view locale) {
    result<std::string> r;
    mf m{pattern, 0, args, locale.empty() ? current() : std::string(locale), {}};
    std::string out;
    if (!m.text(out, nullptr, false)) { r.err = {EINVAL, "format_message: " + m.err}; return r; }
    r.value = std::move(out); return r;
}
}  // namespace forge::os::locale
