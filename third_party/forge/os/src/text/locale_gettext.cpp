// forge::os::locale: gettext catalogs. GNU .mo (either byte order) and .po (msgctxt, plurals, multi-line strings,
// escapes, fuzzy entries skipped), Plural-Forms C-expression evaluation and locale-chain lookup.
#include "locale_internal.hpp"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace forge::os::locale {
using namespace detail;

// ---------------------------------------------------------------- Plural-Forms expressions
namespace {
struct pexpr {
    std::string_view s; std::size_t i = 0; std::uint64_t n; bool bad = false; int depth = 0;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i; }
    bool eat(std::string_view t) { ws(); if (s.substr(i).starts_with(t)) { i += t.size(); return true; } return false; }
    bool peek(std::string_view t) { ws(); return s.substr(i).starts_with(t); }
    std::uint64_t cond() {
        if (++depth > 200) { bad = true; return 0; }
        auto c = lor();
        if (eat("?")) { auto a = cond(); if (!eat(":")) bad = true; auto b = cond(); --depth; return c ? a : b; }
        --depth; return c;
    }
    std::uint64_t lor() { auto a = land(); while (eat("||")) { auto b = land(); a = a || b; } return a; }
    std::uint64_t land() { auto a = eq(); while (eat("&&")) { auto b = eq(); a = a && b; } return a; }
    std::uint64_t eq() { auto a = rel(); while (true) { if (eat("==")) a = a == rel(); else if (eat("!=")) a = a != rel(); else return a; } }
    std::uint64_t rel() {
        auto a = add();
        while (true) {
            if (eat("<=")) a = a <= add(); else if (eat(">=")) a = a >= add();
            else if (peek("<") ) { ++i; a = a < add(); } else if (peek(">")) { ++i; a = a > add(); } else return a;
        }
    }
    std::uint64_t add() { auto a = mul(); while (true) { if (eat("+")) a += mul(); else if (eat("-")) a -= mul(); else return a; } }
    std::uint64_t mul() {
        auto a = unary();
        while (true) {
            if (eat("*")) a *= unary();
            else if (eat("/")) { auto b = unary(); if (!b) { bad = true; return 0; } a /= b; }
            else if (eat("%")) { auto b = unary(); if (!b) { bad = true; return 0; } a %= b; }
            else return a;
        }
    }
    std::uint64_t unary() {
        if (peek("!") && !peek("!=")) {   // "!!!!..." recurses here, not through cond(): same depth budget
            ++i; if (++depth > 200) { bad = true; return 0; }
            auto v = !unary(); --depth; return v;
        }
        ws();
        if (eat("(")) { auto v = cond(); if (!eat(")")) bad = true; return v; }
        if (i < s.size() && s[i] == 'n') { ++i; return n; }
        if (i < s.size() && std::isdigit((unsigned char)s[i])) { std::uint64_t v = 0; while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + std::uint64_t(s[i++] - '0'); return v; }
        bad = true; return 0;
    }
};
}  // namespace
result<std::uint64_t> eval_plural_expr(std::string_view e, std::uint64_t n) {
    result<std::uint64_t> r; pexpr p{e, 0, n};
    auto v = p.cond(); p.ws();
    if (p.bad || p.i != e.size()) { r.err = {EINVAL, "invalid plural expression: " + std::string(e)}; return r; }
    r.value = v; return r;
}

// ---------------------------------------------------------------- catalog
struct catalog::impl {
    std::unordered_map<std::string, std::vector<std::string>> map;
    std::string header; std::string plural_expr = "n != 1"; unsigned nplurals = 2;
    void set_header(std::string h) {
        header = std::move(h);
        auto pf = catalog_header_field(header, "Plural-Forms");
        if (auto a = pf.find("nplurals="); a != std::string::npos) nplurals = unsigned(std::strtoul(pf.c_str() + a + 9, nullptr, 10));
        if (auto a = pf.find("plural="); a != std::string::npos) { auto e = pf.substr(a + 7); if (auto sc = e.find(';'); sc != std::string::npos) e.resize(sc); plural_expr = e; }
    }
    static std::string catalog_header_field(const std::string& h, std::string_view f) {
        std::istringstream in(h); std::string line;
        while (std::getline(in, line)) {
            auto c = line.find(':'); if (c == std::string::npos) continue;
            if (text::to_lower(line.substr(0, c)) == text::to_lower(f)) { auto v = line.substr(c + 1); while (!v.empty() && v.front() == ' ') v.erase(0, 1); while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back(); return v; }
        }
        return {};
    }
};
catalog::catalog() : p_(std::make_unique<impl>()) {}
catalog::~catalog() = default;
catalog::catalog(catalog&&) noexcept = default;
catalog& catalog::operator=(catalog&&) noexcept = default;

namespace {
std::vector<std::string> split0(std::string_view s) { std::vector<std::string> r; std::size_t a = 0; while (true) { auto z = s.find('\0', a); r.emplace_back(s.substr(a, z == std::string_view::npos ? std::string_view::npos : z - a)); if (z == std::string_view::npos) break; a = z + 1; } return r; }
}  // namespace
result<catalog> catalog::parse_mo(std::string_view b) {
    result<catalog> r;
    auto fail = [&](const char* m) { r.err = {EINVAL, std::string("parse_mo: ") + m}; return std::move(r); };
    if (b.size() < 28) return fail("file too short");
    auto u = reinterpret_cast<const unsigned char*>(b.data());
    std::uint32_t magic = std::uint32_t(u[0]) | std::uint32_t(u[1]) << 8 | std::uint32_t(u[2]) << 16 | std::uint32_t(u[3]) << 24;
    bool le;
    if (magic == 0x950412deu) le = true; else if (magic == 0xde120495u) le = false; else return fail("bad magic");
    auto rd = [&](std::size_t o) -> std::uint32_t { if (o + 4 > b.size()) return 0xFFFFFFFFu; return le ? (std::uint32_t(u[o]) | std::uint32_t(u[o + 1]) << 8 | std::uint32_t(u[o + 2]) << 16 | std::uint32_t(u[o + 3]) << 24) : (std::uint32_t(u[o]) << 24 | std::uint32_t(u[o + 1]) << 16 | std::uint32_t(u[o + 2]) << 8 | u[o + 3]); };
    std::uint32_t count = rd(8), orig = rd(12), trans = rd(16);
    if (std::uint64_t(orig) + 8ull * count > b.size() || std::uint64_t(trans) + 8ull * count > b.size()) return fail("truncated tables");
    catalog c;
    for (std::uint32_t k = 0; k < count; ++k) {
        std::uint32_t ol = rd(orig + 8 * k), oo = rd(orig + 8 * k + 4), tl = rd(trans + 8 * k), to = rd(trans + 8 * k + 4);
        if (std::uint64_t(oo) + ol > b.size() || std::uint64_t(to) + tl > b.size()) return fail("string out of range");
        auto key = std::string(b.substr(oo, ol)); if (auto z = key.find('\0'); z != std::string::npos) key.resize(z);   // msgid_plural follows NUL
        auto vals = split0(b.substr(to, tl));
        if (key.empty()) c.p_->set_header(vals.empty() ? std::string() : vals[0]);
        else c.p_->map[key] = std::move(vals);
    }
    r.value = std::move(c); return r;
}
namespace {
bool po_string(std::string_view line, std::string& out) {
    auto a = line.find('"'), z = line.rfind('"'); if (a == std::string_view::npos || z <= a) return false;
    for (std::size_t i = a + 1; i < z; ++i) {
        char ch = line[i];
        if (ch != '\\' || i + 1 >= z) { out += ch; continue; }
        char e = line[++i];
        switch (e) {
        case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; case 'a': out += '\a'; break;
        case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'v': out += '\v'; break;
        case 'x': { int v = 0, n = 0; while (n < 2 && i + 1 < z && std::isxdigit((unsigned char)line[i + 1])) { char h = line[++i]; v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0' : (h | 32) - 'a' + 10); ++n; } out += char(v); break; }
        default:
            if (e >= '0' && e <= '7') { int v = e - '0', n = 1; while (n < 3 && i + 1 < z && line[i + 1] >= '0' && line[i + 1] <= '7') { v = v * 8 + (line[++i] - '0'); ++n; } out += char(v); }
            else out += e;
        }
    }
    return true;
}
}  // namespace
result<catalog> catalog::parse_po(std::string_view t) {
    result<catalog> r; catalog c;
    std::string ctx, id, idp; std::vector<std::string> strs; bool has_ctx = false, fuzzy = false, have = false;
    std::string* cur = nullptr; int line_no = 0;
    auto flush = [&] {
        if (have && !fuzzy) {
            bool any = false; for (auto& s : strs) if (!s.empty()) any = true;
            std::string key = has_ctx ? ctx + '\x04' + id : id;
            if (id.empty() && !has_ctx) c.p_->set_header(strs.empty() ? std::string() : strs[0]);
            else if (any) c.p_->map[key] = strs;
        } else if (have && fuzzy && id.empty() && !has_ctx && !strs.empty()) c.p_->set_header(strs[0]);   // fuzzy header still counts
        ctx.clear(); id.clear(); idp.clear(); strs.clear(); has_ctx = fuzzy = have = false; cur = nullptr;
    };
    std::size_t pos = 0;
    while (pos <= t.size()) {
        auto e = t.find('\n', pos); auto line = t.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos); pos = e == std::string_view::npos ? t.size() + 1 : e + 1; ++line_no;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) continue;
        if (line.starts_with("#,")) { if (have) flush(); if (line.find("fuzzy") != std::string_view::npos) fuzzy = true; continue; }
        if (line.starts_with("#~")) continue;
        if (line[0] == '#') continue;
        auto bad = [&] { r.err = {EINVAL, "parse_po: syntax error on line " + std::to_string(line_no)}; return std::move(r); };
        if (line.starts_with("msgctxt")) { if (have) flush(); has_ctx = true; cur = &ctx; if (!po_string(line, ctx)) return bad(); continue; }
        if (line.starts_with("msgid_plural")) { cur = &idp; if (!po_string(line, idp)) return bad(); continue; }
        if (line.starts_with("msgid")) { if (have && !strs.empty()) flush(); have = true; cur = &id; if (!po_string(line, id)) return bad(); continue; }
        if (line.starts_with("msgstr")) {
            std::size_t idx = 0;
            if (line.size() > 6 && line[6] == '[') idx = std::strtoul(std::string(line.substr(7)).c_str(), nullptr, 10);
            if (idx > 1000) return bad();
            if (strs.size() <= idx) strs.resize(idx + 1);
            cur = &strs[idx]; if (!po_string(line, *cur)) return bad(); continue;
        }
        if (line[0] == '"') { if (!cur || !po_string(line, *cur)) return bad(); continue; }
        return bad();
    }
    flush();
    r.value = std::move(c); return r;
}
result<catalog> catalog::load(std::string_view path) {
    result<catalog> r;
    std::ifstream f{std::filesystem::path(std::string(path)), std::ios::binary};
    if (!f) { r.err = {ENOENT, "cannot open " + std::string(path)}; return r; }
    std::stringstream ss; ss << f.rdbuf(); auto s = ss.str();
    if (s.size() >= 4) {
        auto u = reinterpret_cast<const unsigned char*>(s.data());
        if ((u[0] == 0xde && u[1] == 0x12 && u[2] == 0x04 && u[3] == 0x95) || (u[0] == 0x95 && u[1] == 0x04 && u[2] == 0x12 && u[3] == 0xde)) return parse_mo(s);
    }
    if (s.starts_with("\xEF\xBB\xBF")) s.erase(0, 3);
    return parse_po(s);
}
result<catalog> catalog::find(std::string_view dir, std::string_view domain, std::string_view locale) {
    result<catalog> r; bool found = false; catalog acc;
    std::vector<std::string> names;
    for (auto& l : fallback_chain(locale.empty() ? current() : locale)) {
        if (l == "root") continue;
        names.push_back(replace_all(l, "-", "_")); if (l.find('-') != std::string::npos) names.push_back(l);
        auto t = parse_tag(l);
        if (!t.script.empty() && !t.region.empty()) names.push_back(t.language + "_" + t.region);
    }
    for (auto& n : names) for (auto ext : {".mo", ".po"}) for (bool lc : {true, false}) {
        auto p = std::filesystem::path(std::string(dir)) / n; if (lc) p /= "LC_MESSAGES"; p /= std::string(domain) + ext;
        std::error_code ec; if (!std::filesystem::exists(p, ec)) { auto flat = std::filesystem::path(std::string(dir)) / (n + ext); if (lc || !std::filesystem::exists(flat, ec)) continue; p = flat; }
        auto c = load(p.string());
        if (!c) { r.err = c.err; return r; }
        if (!found) acc = std::move(c.value); else acc.merge(std::move(c.value));
        found = true;
    }
    if (!found) { r.err = {ENOENT, "no catalog for domain '" + std::string(domain) + "' in " + std::string(dir)}; return r; }
    r.value = std::move(acc); return r;
}
std::optional<std::string_view> catalog::lookup(std::string_view ctx, std::string_view msgid, std::uint64_t n) const {
    std::string key = ctx.empty() ? std::string(msgid) : std::string(ctx) + '\x04' + std::string(msgid);
    auto it = p_->map.find(key); if (it == p_->map.end()) return std::nullopt;
    auto& v = it->second; unsigned idx = v.size() > 1 ? plural_index(n) : 0;
    if (idx >= v.size() || v[idx].empty()) return std::nullopt;
    return std::string_view(v[idx]);
}
std::string_view catalog::gettext(std::string_view id) const { return lookup({}, id).value_or(id); }
std::string_view catalog::ngettext(std::string_view s, std::string_view pl, std::uint64_t n) const { if (auto v = lookup({}, s, n)) return *v; return n == 1 ? s : pl; }
std::string_view catalog::pgettext(std::string_view c, std::string_view id) const { return lookup(c, id).value_or(id); }
std::string_view catalog::npgettext(std::string_view c, std::string_view s, std::string_view pl, std::uint64_t n) const { if (auto v = lookup(c, s, n)) return *v; return n == 1 ? s : pl; }
std::string catalog::header(std::string_view f) const { return impl::catalog_header_field(p_->header, f); }
std::size_t catalog::size() const { return p_->map.size(); }
unsigned catalog::plural_index(std::uint64_t n) const {
    auto r = eval_plural_expr(p_->plural_expr, n);
    unsigned v = r ? unsigned(r.value) : (n != 1);
    return v < p_->nplurals ? v : 0;
}
void catalog::merge(catalog&& o) {
    for (auto& [k, v] : o.p_->map) p_->map.try_emplace(k, std::move(v));
    if (p_->header.empty()) p_->set_header(o.p_->header);
}
}  // namespace forge::os::locale
