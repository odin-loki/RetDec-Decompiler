// forge::script bindings for forge::os::text and forge::os::locale (pure computation: no capability gate except
// catalog loading, which needs `fs`).
//   forge::text::upper/lower/title/fold/normalize/graphemes/words/sentences/width/wrap/truncate/pad/slugify/to_ascii/
//     remove_marks/levenshtein/similarity/jaro_winkler/fuzzy_filter/compare/sort/sort_key/decode/encode/detect/
//     strip_ansi/style/bidi_visual/direction/script/props, forge::text::regex(pattern[, flags]) -> "regex" handle
//   forge::locale::current/set_current/system/preferred/canonicalize/maximize/negotiate/display_name/plural/
//     number/percent/compact/currency/bytes/parse_number/list/unit/convert/date/time/datetime/pattern/skeleton/
//     parse_date/iso/relative/duration/zone/zones/local_zone/calendar/message/first_day/hour_cycle/measurement,
//   forge::locale::catalog(path | {dir, domain[, locale]}) -> "catalog" handle
#include "interp.hpp"
#include "objects.hpp"
#include <forge/os/locale.hpp>
#include <forge/os/text.hpp>

namespace forge::script {
namespace {
namespace tx = forge::os::text;
namespace lc = forge::os::locale;
using Args = std::vector<Value>;
using Fn = Native::fn_t;
Value strv(std::string s) { return obj(make<String>(std::move(s))); }
Value nat(const std::string& name, int lo, int hi, Fn f) { auto n = make<Native>(name, std::move(f), lo, hi); n->frozen = true; return obj(n); }
std::string s_of(Interp& in, const Value& v, const char* what) { auto* s = as<String>(v); if (!s) in.raise("TypeError", std::string(what) + " expects a string, got " + Interp::type_of(v)); return s->s; }
double f_of(Interp& in, const Value& v, const char* what) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return double(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    in.raise("TypeError", std::string(what) + " expects a number, got " + Interp::type_of(v)); return 0;
}
std::string opt_s(Interp& in, const Args& a, std::size_t i, const char* what) { return i < a.size() && !is_nil(a[i]) ? s_of(in, a[i], what) : std::string(); }
const Value* kv(const Value& bag, const std::string& k) {
    const Value* r = nullptr;
    if (auto* m = as<Map>(bag)) r = m->find_str(k);
    else if (auto* l = as<List>(bag)) for (auto& e : l->items) if (auto* p = as<List>(e); p && p->items.size() == 2) if (auto* s = as<String>(p->items[0]); s && s->s == k) r = &p->items[1];
    return r && !is_nil(*r) ? r : nullptr;
}
const Value* opt_kv(const Args& a, std::size_t i, const std::string& k) { return i < a.size() && !is_nil(a[i]) ? kv(a[i], k) : nullptr; }
template <class T> T okv(Interp& in, os::result<T> r) { if (!r) in.raise("ValueError", r.err.what); return std::move(r.value); }
Value map_of(std::initializer_list<std::pair<const char*, Value>> kv) { auto m = make<Map>(); for (auto& [k, v] : kv) m->set_str(k, v); return obj(m); }
Value strs(const std::vector<std::string>& v) { auto l = make<List>(); for (auto& s : v) l->push(strv(s)); return obj(l); }
template <class V> Value views(const V& v) { auto l = make<List>(); for (auto& s : v) l->push(strv(std::string(s))); return obj(l); }
std::vector<std::string> str_list(Interp& in, const Value& v, const char* what) {
    auto* l = as<List>(v); if (!l) in.raise("TypeError", std::string(what) + " expects a list of strings");
    std::vector<std::string> r; for (auto& e : l->items) r.push_back(s_of(in, e, what)); return r;
}
Handle& h_of(Interp& in, const Value& v, const std::string& kind) {
    auto* h = as<Handle>(v);
    if (!h || h->kind != kind) in.raise("TypeError", "expected a " + kind + " handle");
    if (h->disposed) in.raise("ObjectDisposedError", kind + " handle has been disposed");
    return *h;
}
tx::norm_form norm_of(Interp& in, const std::string& s) {
    if (s.empty() || s == "nfc") return tx::norm_form::nfc; if (s == "nfd") return tx::norm_form::nfd;
    if (s == "nfkc") return tx::norm_form::nfkc; if (s == "nfkd") return tx::norm_form::nfkd;
    in.raise("ValueError", "unknown normalization form '" + s + "'"); return tx::norm_form::nfc;
}
tx::collation_options coll_opts(Interp& in, const Args& a, std::size_t i) {
    tx::collation_options o;
    if (auto* v = opt_kv(a, i, "strength")) { int s = int(f_of(in, *v, "strength")); if (s < 1 || s > 5) in.raise("ValueError", "strength must be 1..5"); o.level = tx::strength(s); }
    if (auto* v = opt_kv(a, i, "numeric")) o.numeric = Interp::truthy(*v);
    if (auto* v = opt_kv(a, i, "shifted")) o.shifted = Interp::truthy(*v);
    if (auto* v = opt_kv(a, i, "upper_first")) o.upper_first = Interp::truthy(*v);
    return o;
}
lc::number_options num_opts(Interp& in, const Args& a, std::size_t i) {
    lc::number_options o;
    if (auto* v = opt_kv(a, i, "min_fraction")) o.min_fraction = int(f_of(in, *v, "min_fraction"));
    if (auto* v = opt_kv(a, i, "max_fraction")) o.max_fraction = int(f_of(in, *v, "max_fraction"));
    if (auto* v = opt_kv(a, i, "fraction")) o.min_fraction = o.max_fraction = int(f_of(in, *v, "fraction"));
    if (auto* v = opt_kv(a, i, "significant")) o.max_significant = int(f_of(in, *v, "significant"));
    if (auto* v = opt_kv(a, i, "min_integer")) o.min_integer = int(f_of(in, *v, "min_integer"));
    if (auto* v = opt_kv(a, i, "grouping")) o.grouping = Interp::truthy(*v);
    if (auto* v = opt_kv(a, i, "numbering")) o.numbering = s_of(in, *v, "numbering");
    if (auto* v = opt_kv(a, i, "sign")) { auto s = s_of(in, *v, "sign"); o.sign = s == "always" ? lc::sign_display::always : s == "never" ? lc::sign_display::never : s == "except_zero" ? lc::sign_display::except_zero : lc::sign_display::auto_; }
    return o;
}
lc::date_style style_of(Interp& in, const std::string& s, lc::date_style def) {
    if (s.empty()) return def; if (s == "full") return lc::date_style::full; if (s == "long") return lc::date_style::long_;
    if (s == "medium") return lc::date_style::medium; if (s == "short") return lc::date_style::short_; if (s == "none") return lc::date_style::none;
    in.raise("ValueError", "date style must be full/long/medium/short/none"); return def;
}
lc::unit_width uwidth(const std::string& s) { return s == "long" ? lc::unit_width::long_ : s == "narrow" ? lc::unit_width::narrow : lc::unit_width::short_; }
// Dates: an ISO 8601 string, a Unix timestamp (seconds, UTC) or a map from forge::locale::now().
lc::datetime dt_of(Interp& in, const Value& v, const std::string& zone) {
    if (auto* s = as<String>(v)) return okv(in, lc::parse_iso8601(s->s));
    if (std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v)) {
        auto t = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(f_of(in, v, "time"))));
        if (zone.empty() || zone == "UTC") return lc::from_time_utc(t);
        return okv(in, lc::from_time(t, zone));
    }
    if (as<Map>(v) || as<List>(v)) {
        lc::datetime d; auto gi = [&](const char* k, int& out) { if (auto* x = kv(v, k)) out = int(f_of(in, *x, k)); };
        gi("year", d.year); gi("month", d.month); gi("day", d.day); gi("hour", d.hour); gi("minute", d.minute); gi("second", d.second);
        gi("nanosecond", d.nanosecond); gi("utc_offset", d.utc_offset);
        if (auto* z = kv(v, "zone")) d.zone = s_of(in, *z, "zone");
        if (auto* z = kv(v, "zone_abbrev")) d.zone_abbrev = s_of(in, *z, "zone_abbrev");
        return d;
    }
    in.raise("TypeError", "expected a date (ISO string, timestamp or date map)"); return {};
}
Value dt_v(const lc::datetime& d) {
    return map_of({{"year", std::int64_t(d.year)}, {"month", std::int64_t(d.month)}, {"day", std::int64_t(d.day)}, {"hour", std::int64_t(d.hour)},
                   {"minute", std::int64_t(d.minute)}, {"second", std::int64_t(d.second)}, {"nanosecond", std::int64_t(d.nanosecond)},
                   {"utc_offset", std::int64_t(d.utc_offset)}, {"zone", strv(d.zone)}, {"zone_abbrev", strv(d.zone_abbrev)},
                   {"weekday", std::int64_t(d.weekday())}, {"day_of_year", std::int64_t(d.day_of_year())}, {"timestamp", std::int64_t(d.to_sys().time_since_epoch().count())}});
}
lc::message_value mv_of(Interp& in, const Value& v) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return *i;
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* s = as<String>(v)) return s->s;
    if (as<Map>(v)) return dt_of(in, v, {});
    return in.str(v);
}

void install(Shared& sh) {
    auto& B = sh.builtins; auto& M = sh.methods;
    // ---------------------------------------------------------------- text
    auto s1 = [&](const char* name, std::string (*f)(std::string_view)) {
        B[name] = nat(name, 1, 1, [f, name](Interp& in, Args& a) -> Value { return strv(f(s_of(in, a[0], name))); });
    };
    B["forge::text::upper"] = nat("forge::text::upper", 1, 2, [](Interp& in, Args& a) -> Value { return strv(tx::to_upper(s_of(in, a[0], "upper"), opt_s(in, a, 1, "lang"))); });
    B["forge::text::lower"] = nat("forge::text::lower", 1, 2, [](Interp& in, Args& a) -> Value { return strv(tx::to_lower(s_of(in, a[0], "lower"), opt_s(in, a, 1, "lang"))); });
    B["forge::text::title"] = nat("forge::text::title", 1, 2, [](Interp& in, Args& a) -> Value { return strv(tx::to_title(s_of(in, a[0], "title"), opt_s(in, a, 1, "lang"))); });
    B["forge::text::fold"] = nat("forge::text::fold", 1, 1, [](Interp& in, Args& a) -> Value { return strv(tx::case_fold(s_of(in, a[0], "fold"))); });
    B["forge::text::equals_ignore_case"] = nat("forge::text::equals_ignore_case", 2, 2, [](Interp& in, Args& a) -> Value { return tx::equals_ignore_case(s_of(in, a[0], "a"), s_of(in, a[1], "b")); });
    B["forge::text::normalize"] = nat("forge::text::normalize", 1, 2, [](Interp& in, Args& a) -> Value { return strv(tx::normalize(s_of(in, a[0], "normalize"), norm_of(in, opt_s(in, a, 1, "form")))); });
    B["forge::text::is_normalized"] = nat("forge::text::is_normalized", 1, 2, [](Interp& in, Args& a) -> Value { return tx::is_normalized(s_of(in, a[0], "is_normalized"), norm_of(in, opt_s(in, a, 1, "form"))); });
    B["forge::text::graphemes"] = nat("forge::text::graphemes", 1, 1, [](Interp& in, Args& a) -> Value { auto s = s_of(in, a[0], "graphemes"); return views(tx::graphemes(s)); });
    B["forge::text::words"] = nat("forge::text::words", 1, 1, [](Interp& in, Args& a) -> Value { auto s = s_of(in, a[0], "words"); return views(tx::words(s)); });
    B["forge::text::sentences"] = nat("forge::text::sentences", 1, 1, [](Interp& in, Args& a) -> Value { auto s = s_of(in, a[0], "sentences"); return views(tx::sentences(s)); });
    B["forge::text::length"] = nat("forge::text::length", 1, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(tx::count_graphemes(s_of(in, a[0], "length"))); });
    B["forge::text::code_points"] = nat("forge::text::code_points", 1, 1, [](Interp& in, Args& a) -> Value { auto l = make<List>(); for (auto c : tx::utf8_to_utf32(s_of(in, a[0], "code_points"))) l->push(std::int64_t(c)); return obj(l); });
    B["forge::text::width"] = nat("forge::text::width", 1, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(tx::display_width_ansi(s_of(in, a[0], "width"))); });
    B["forge::text::wrap"] = nat("forge::text::wrap", 2, 3, [](Interp& in, Args& a) -> Value {
        tx::wrap_options o; o.width = int(f_of(in, a[1], "wrap width"));
        if (auto* v = opt_kv(a, 2, "indent")) o.indent = s_of(in, *v, "indent");
        if (auto* v = opt_kv(a, 2, "subsequent_indent")) o.subsequent_indent = s_of(in, *v, "subsequent_indent");
        return strv(tx::wrap(s_of(in, a[0], "wrap"), o));
    });
    B["forge::text::truncate"] = nat("forge::text::truncate", 2, 3, [](Interp& in, Args& a) -> Value {
        std::string e = a.size() > 2 && !is_nil(a[2]) ? s_of(in, a[2], "ellipsis") : std::string("\xE2\x80\xA6");
        return strv(tx::truncate(s_of(in, a[0], "truncate"), int(f_of(in, a[1], "width")), e));
    });
    B["forge::text::pad"] = nat("forge::text::pad", 2, 3, [](Interp& in, Args& a) -> Value {
        auto al = opt_s(in, a, 2, "align"); auto A = al == "right" ? tx::align::right : al == "center" ? tx::align::center : tx::align::left;
        return strv(tx::pad(s_of(in, a[0], "pad"), int(f_of(in, a[1], "width")), A));
    });
    B["forge::text::slugify"] = nat("forge::text::slugify", 1, 2, [](Interp& in, Args& a) -> Value { auto sep = opt_s(in, a, 1, "separator"); return strv(tx::slugify(s_of(in, a[0], "slugify"), sep.empty() ? '-' : sep[0])); });
    B["forge::text::to_ascii"] = nat("forge::text::to_ascii", 1, 1, [](Interp& in, Args& a) -> Value { return strv(tx::to_ascii(s_of(in, a[0], "to_ascii"))); });
    s1("forge::text::remove_marks", +[](std::string_view s) { return tx::remove_marks(s); });
    s1("forge::text::strip_ansi", +[](std::string_view s) { return tx::strip_ansi(s); });
    s1("forge::text::sanitize", +[](std::string_view s) { return tx::sanitize_utf8(s); });
    B["forge::text::bidi_visual"] = nat("forge::text::bidi_visual", 1, 1, [](Interp& in, Args& a) -> Value { return strv(tx::bidi_visual(s_of(in, a[0], "bidi_visual"))); });
    B["forge::text::direction"] = nat("forge::text::direction", 1, 1, [](Interp& in, Args& a) -> Value { return strv(tx::base_direction(s_of(in, a[0], "direction")) == tx::direction::rtl ? "rtl" : "ltr"); });
    B["forge::text::props"] = nat("forge::text::props", 1, 1, [](Interp& in, Args& a) -> Value {
        auto s = s_of(in, a[0], "props"); std::size_t i = 0; auto c = tx::decode_utf8(s, i);
        return map_of({{"code_point", std::int64_t(c)}, {"script", strv(std::string(tx::script(c)))}, {"east_asian_width", strv(std::string(tx::east_asian_width(c)))},
                       {"bidi_class", strv(std::string(tx::bidi_class_name(c)))}, {"letter", tx::is_letter(c)}, {"digit", tx::is_digit(c)}, {"space", tx::is_space(c)},
                       {"upper", tx::is_upper(c)}, {"lower", tx::is_lower(c)}, {"emoji", tx::is_emoji(c)}, {"punct", tx::is_punct(c)}, {"width", std::int64_t(tx::width(c))}});
    });
    B["forge::text::has_property"] = nat("forge::text::has_property", 2, 2, [](Interp& in, Args& a) -> Value {
        auto s = s_of(in, a[0], "has_property"); std::size_t i = 0; auto c = tx::decode_utf8(s, i); bool known = true;
        bool r = tx::has_property(c, s_of(in, a[1], "property"), &known); if (!known) in.raise("ValueError", "unknown Unicode property"); return r;
    });
    B["forge::text::levenshtein"] = nat("forge::text::levenshtein", 2, 2, [](Interp& in, Args& a) -> Value { return std::int64_t(tx::levenshtein(s_of(in, a[0], "a"), s_of(in, a[1], "b"))); });
    B["forge::text::similarity"] = nat("forge::text::similarity", 2, 2, [](Interp& in, Args& a) -> Value { return tx::similarity(s_of(in, a[0], "a"), s_of(in, a[1], "b")); });
    B["forge::text::jaro_winkler"] = nat("forge::text::jaro_winkler", 2, 2, [](Interp& in, Args& a) -> Value { return tx::jaro_winkler(s_of(in, a[0], "a"), s_of(in, a[1], "b")); });
    B["forge::text::fuzzy_filter"] = nat("forge::text::fuzzy_filter", 2, 3, [](Interp& in, Args& a) -> Value {
        auto items = str_list(in, a[1], "fuzzy_filter items"); std::size_t lim = a.size() > 2 && !is_nil(a[2]) ? std::size_t(f_of(in, a[2], "limit")) : 0;
        auto l = make<List>(); for (auto& h : tx::fuzzy_filter(s_of(in, a[0], "pattern"), items, lim)) l->push(map_of({{"item", strv(items[h.index])}, {"index", std::int64_t(h.index)}, {"score", std::int64_t(h.score)}}));
        return obj(l);
    });
    B["forge::text::compare"] = nat("forge::text::compare", 2, 4, [](Interp& in, Args& a) -> Value { tx::collator c(opt_s(in, a, 2, "locale"), coll_opts(in, a, 3)); return std::int64_t(c.compare(s_of(in, a[0], "a"), s_of(in, a[1], "b"))); });
    B["forge::text::sort"] = nat("forge::text::sort", 1, 3, [](Interp& in, Args& a) -> Value { auto v = str_list(in, a[0], "sort"); tx::sort(v, opt_s(in, a, 1, "locale"), coll_opts(in, a, 2)); return strs(v); });
    B["forge::text::sort_key"] = nat("forge::text::sort_key", 1, 3, [](Interp& in, Args& a) -> Value { tx::collator c(opt_s(in, a, 1, "locale"), coll_opts(in, a, 2)); return strv(c.sort_key(s_of(in, a[0], "sort_key"))); });
    B["forge::text::charsets"] = nat("forge::text::charsets", 0, 0, [](Interp&, Args&) -> Value { return strs(tx::charsets()); });
    B["forge::text::decode"] = nat("forge::text::decode", 2, 2, [](Interp& in, Args& a) -> Value { return strv(okv(in, tx::decode(s_of(in, a[0], "bytes"), s_of(in, a[1], "charset")))); });
    B["forge::text::encode"] = nat("forge::text::encode", 2, 2, [](Interp& in, Args& a) -> Value { return strv(okv(in, tx::encode(s_of(in, a[0], "text"), s_of(in, a[1], "charset")))); });
    B["forge::text::detect"] = nat("forge::text::detect", 1, 1, [](Interp& in, Args& a) -> Value { std::string used; auto t = tx::to_utf8_auto(s_of(in, a[0], "bytes"), &used); return map_of({{"charset", strv(used)}, {"text", strv(t)}}); });
    B["forge::text::style"] = nat("forge::text::style", 2, 2, [](Interp& in, Args& a) -> Value {
        auto st = tx::parse_style(s_of(in, a[1], "style spec")); if (!st) in.raise("ValueError", "bad style spec"); return strv(tx::style(s_of(in, a[0], "style"), *st));
    });
    B["forge::text::regex"] = nat("forge::text::regex", 1, 2, [](Interp& in, Args& a) -> Value {
        auto r = std::make_shared<tx::regex>(okv(in, tx::regex::compile(s_of(in, a[0], "regex pattern"), opt_s(in, a, 1, "flags"))));
        auto h = make<Handle>("regex"); h->payload = r; return obj(h);
    });
    auto rx = [](Interp& in, const Value& v) { return h_of(in, v, "regex").payload_as<tx::regex>(); };
    auto match_v = [](const tx::regex_match& m, const std::string& s) {
        auto g = make<List>(); auto named = make<Map>();
        for (std::size_t i = 0; i < m.groups.size(); ++i) {
            Value v = m.groups[i].first == std::string::npos ? Value(Nil{}) : strv(std::string(m.str(s, i)));
            g->push(v); if (i < m.names.size() && !m.names[i].empty()) named->set_str(m.names[i], v);
        }
        return map_of({{"text", strv(std::string(m.str(s)))}, {"start", std::int64_t(m.begin())}, {"end", std::int64_t(m.end())}, {"groups", obj(g)}, {"named", obj(named)}});
    };
    auto& R = M["handle:regex"];
    R["test"] = nat("regex.test", 2, 2, [rx](Interp& in, Args& a) -> Value { return bool(rx(in, a[0])->search(s_of(in, a[1], "subject"))); });
    R["match"] = nat("regex.match", 2, 2, [rx, match_v](Interp& in, Args& a) -> Value { auto s = s_of(in, a[1], "subject"); auto m = rx(in, a[0])->match(s); return m ? match_v(*m, s) : Value(Nil{}); });
    R["search"] = nat("regex.search", 2, 3, [rx, match_v](Interp& in, Args& a) -> Value {
        auto s = s_of(in, a[1], "subject"); auto m = rx(in, a[0])->search(s, a.size() > 2 ? std::size_t(f_of(in, a[2], "from")) : 0); return m ? match_v(*m, s) : Value(Nil{});
    });
    R["find_all"] = nat("regex.find_all", 2, 2, [rx, match_v](Interp& in, Args& a) -> Value { auto s = s_of(in, a[1], "subject"); auto l = make<List>(); for (auto& m : rx(in, a[0])->find_all(s)) l->push(match_v(m, s)); return obj(l); });
    R["replace"] = nat("regex.replace", 3, 4, [rx](Interp& in, Args& a) -> Value { return strv(rx(in, a[0])->replace(s_of(in, a[1], "subject"), s_of(in, a[2], "replacement"), a.size() < 4 || Interp::truthy(a[3]))); });
    R["split"] = nat("regex.split", 2, 2, [rx](Interp& in, Args& a) -> Value { return strs(rx(in, a[0])->split(s_of(in, a[1], "subject"))); });
    R["close"] = nat("regex.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });

    // ---------------------------------------------------------------- locale
    B["forge::locale::current"] = nat("forge::locale::current", 0, 0, [](Interp&, Args&) -> Value { return strv(lc::current()); });
    B["forge::locale::set_current"] = nat("forge::locale::set_current", 1, 1, [](Interp& in, Args& a) -> Value { lc::set_current(s_of(in, a[0], "locale")); return Nil{}; });
    B["forge::locale::system"] = nat("forge::locale::system", 0, 0, [](Interp&, Args&) -> Value { return strv(lc::system_locale()); });
    B["forge::locale::preferred"] = nat("forge::locale::preferred", 0, 0, [](Interp&, Args&) -> Value { return strs(lc::preferred_languages()); });
    B["forge::locale::bundled"] = nat("forge::locale::bundled", 0, 0, [](Interp&, Args&) -> Value { return strs(lc::bundled_locales()); });
    B["forge::locale::canonicalize"] = nat("forge::locale::canonicalize", 1, 1, [](Interp& in, Args& a) -> Value { return strv(lc::canonicalize(s_of(in, a[0], "tag"))); });
    B["forge::locale::maximize"] = nat("forge::locale::maximize", 1, 1, [](Interp& in, Args& a) -> Value { return strv(lc::maximize(lc::parse_tag(s_of(in, a[0], "tag"))).str()); });
    B["forge::locale::minimize"] = nat("forge::locale::minimize", 1, 1, [](Interp& in, Args& a) -> Value { return strv(lc::minimize(lc::parse_tag(s_of(in, a[0], "tag"))).str()); });
    B["forge::locale::negotiate"] = nat("forge::locale::negotiate", 2, 3, [](Interp& in, Args& a) -> Value {
        auto r = str_list(in, a[0], "requested"), v = str_list(in, a[1], "available"); auto fb = opt_s(in, a, 2, "fallback"); return strv(lc::negotiate(r, v, fb.empty() ? "en" : fb));
    });
    B["forge::locale::display_name"] = nat("forge::locale::display_name", 1, 2, [](Interp& in, Args& a) -> Value { return strv(lc::display_name(s_of(in, a[0], "language"), opt_s(in, a, 1, "locale"))); });
    B["forge::locale::plural"] = nat("forge::locale::plural", 1, 3, [](Interp& in, Args& a) -> Value {
        bool ord = a.size() > 2 && Interp::truthy(a[2]); auto loc = opt_s(in, a, 1, "locale");
        if (auto* s = as<String>(a[0])) return strv(lc::plural_name(lc::plural_category(std::string_view(s->s), loc, ord)));
        return strv(lc::plural_name(lc::plural_category(f_of(in, a[0], "plural"), loc, ord)));
    });
    B["forge::locale::number"] = nat("forge::locale::number", 1, 3, [](Interp& in, Args& a) -> Value { return strv(lc::format_number(f_of(in, a[0], "number"), opt_s(in, a, 1, "locale"), num_opts(in, a, 2))); });
    B["forge::locale::percent"] = nat("forge::locale::percent", 1, 3, [](Interp& in, Args& a) -> Value { return strv(lc::format_percent(f_of(in, a[0], "percent"), opt_s(in, a, 1, "locale"), num_opts(in, a, 2))); });
    B["forge::locale::compact"] = nat("forge::locale::compact", 1, 3, [](Interp& in, Args& a) -> Value {
        return strv(lc::format_compact(f_of(in, a[0], "compact"), opt_s(in, a, 1, "locale"), opt_s(in, a, 2, "width") == "long" ? lc::compact_width::long_ : lc::compact_width::short_));
    });
    B["forge::locale::currency"] = nat("forge::locale::currency", 2, 4, [](Interp& in, Args& a) -> Value {
        lc::number_options o = num_opts(in, a, 3);
        auto disp = lc::currency_display::symbol; bool acct = false;
        if (auto* v = opt_kv(a, 3, "display")) { auto s = s_of(in, *v, "display"); disp = s == "code" ? lc::currency_display::code : s == "name" ? lc::currency_display::name : s == "narrow" ? lc::currency_display::narrow_symbol : lc::currency_display::symbol; }
        if (auto* v = opt_kv(a, 3, "accounting")) acct = Interp::truthy(*v);
        return strv(lc::format_currency(f_of(in, a[0], "amount"), s_of(in, a[1], "currency"), opt_s(in, a, 2, "locale"), disp, acct, o));
    });
    B["forge::locale::bytes"] = nat("forge::locale::bytes", 1, 2, [](Interp& in, Args& a) -> Value { return strv(lc::format_bytes(std::uint64_t(f_of(in, a[0], "bytes")), opt_s(in, a, 1, "locale"))); });
    B["forge::locale::parse_number"] = nat("forge::locale::parse_number", 1, 2, [](Interp& in, Args& a) -> Value { return okv(in, lc::parse_number(s_of(in, a[0], "parse_number"), opt_s(in, a, 1, "locale"))); });
    B["forge::locale::list"] = nat("forge::locale::list", 1, 4, [](Interp& in, Args& a) -> Value {
        auto items = str_list(in, a[0], "list"); auto t = opt_s(in, a, 2, "type"), w = opt_s(in, a, 3, "width");
        return strv(lc::format_list(items, opt_s(in, a, 1, "locale"), t == "or" ? lc::list_type::or_ : t == "unit" ? lc::list_type::unit : lc::list_type::and_,
                                    w == "short" ? lc::list_width::short_ : w == "narrow" ? lc::list_width::narrow : lc::list_width::wide));
    });
    B["forge::locale::unit"] = nat("forge::locale::unit", 2, 4, [](Interp& in, Args& a) -> Value { return strv(lc::format_unit(f_of(in, a[0], "value"), s_of(in, a[1], "unit"), opt_s(in, a, 2, "locale"), uwidth(opt_s(in, a, 3, "width")))); });
    B["forge::locale::convert"] = nat("forge::locale::convert", 3, 3, [](Interp& in, Args& a) -> Value { return okv(in, lc::convert_unit(f_of(in, a[0], "value"), s_of(in, a[1], "from"), s_of(in, a[2], "to"))); });
    B["forge::locale::now"] = nat("forge::locale::now", 0, 1, [](Interp& in, Args& a) -> Value { return dt_v(lc::now(opt_s(in, a, 0, "zone"))); });
    B["forge::locale::to_date"] = nat("forge::locale::to_date", 1, 2, [](Interp& in, Args& a) -> Value { return dt_v(dt_of(in, a[0], opt_s(in, a, 1, "zone"))); });
    B["forge::locale::date"] = nat("forge::locale::date", 1, 3, [](Interp& in, Args& a) -> Value { return strv(lc::format_date(dt_of(in, a[0], {}), opt_s(in, a, 1, "locale"), style_of(in, opt_s(in, a, 2, "style"), lc::date_style::medium))); });
    B["forge::locale::time"] = nat("forge::locale::time", 1, 3, [](Interp& in, Args& a) -> Value { return strv(lc::format_time(dt_of(in, a[0], {}), opt_s(in, a, 1, "locale"), style_of(in, opt_s(in, a, 2, "style"), lc::date_style::short_))); });
    B["forge::locale::datetime"] = nat("forge::locale::datetime", 1, 4, [](Interp& in, Args& a) -> Value {
        return strv(lc::format_datetime(dt_of(in, a[0], {}), opt_s(in, a, 1, "locale"), style_of(in, opt_s(in, a, 2, "date style"), lc::date_style::medium), style_of(in, opt_s(in, a, 3, "time style"), lc::date_style::short_)));
    });
    B["forge::locale::pattern"] = nat("forge::locale::pattern", 2, 3, [](Interp& in, Args& a) -> Value { return strv(lc::format_pattern(dt_of(in, a[0], {}), s_of(in, a[1], "pattern"), opt_s(in, a, 2, "locale"))); });
    B["forge::locale::skeleton"] = nat("forge::locale::skeleton", 2, 3, [](Interp& in, Args& a) -> Value { return strv(lc::format_skeleton(dt_of(in, a[0], {}), s_of(in, a[1], "skeleton"), opt_s(in, a, 2, "locale"))); });
    B["forge::locale::parse_date"] = nat("forge::locale::parse_date", 2, 3, [](Interp& in, Args& a) -> Value { return dt_v(okv(in, lc::parse_pattern(s_of(in, a[0], "text"), s_of(in, a[1], "pattern"), opt_s(in, a, 2, "locale")))); });
    B["forge::locale::iso"] = nat("forge::locale::iso", 1, 1, [](Interp& in, Args& a) -> Value { return strv(lc::format_iso8601(dt_of(in, a[0], {}))); });
    B["forge::locale::relative"] = nat("forge::locale::relative", 2, 4, [](Interp& in, Args& a) -> Value {
        static const char* names[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};
        auto u = s_of(in, a[1], "unit"); int k = -1; for (int i = 0; i < 8; ++i) if (u == names[i] || u == std::string(names[i]) + "s") k = i;
        if (k < 0) in.raise("ValueError", "unknown relative-time unit '" + u + "'");
        auto w = opt_s(in, a, 3, "width");
        return strv(lc::format_relative(f_of(in, a[0], "value"), lc::rel_unit(k), opt_s(in, a, 2, "locale"), w == "short" ? lc::rel_width::short_ : w == "narrow" ? lc::rel_width::narrow : lc::rel_width::long_));
    });
    B["forge::locale::ago"] = nat("forge::locale::ago", 1, 2, [](Interp& in, Args& a) -> Value {
        auto t = dt_of(in, a[0], {}).to_sys(); auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
        return strv(lc::format_relative_time(t, now, opt_s(in, a, 1, "locale")));
    });
    B["forge::locale::duration"] = nat("forge::locale::duration", 1, 4, [](Interp& in, Args& a) -> Value {
        int mx = a.size() > 3 && !is_nil(a[3]) ? int(f_of(in, a[3], "max_units")) : 2;
        return strv(lc::format_duration(std::chrono::seconds(std::int64_t(f_of(in, a[0], "seconds"))), opt_s(in, a, 1, "locale"), uwidth(a.size() > 2 && !is_nil(a[2]) ? s_of(in, a[2], "width") : "long"), mx));
    });
    B["forge::locale::zone"] = nat("forge::locale::zone", 0, 2, [](Interp& in, Args& a) -> Value {
        auto t = a.size() > 1 && !is_nil(a[1]) ? dt_of(in, a[1], {}).to_sys() : std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
        auto z = okv(in, lc::zone_at(opt_s(in, a, 0, "zone"), t));
        return map_of({{"offset", std::int64_t(z.offset.count())}, {"dst", z.dst}, {"abbrev", strv(z.abbrev)}});
    });
    B["forge::locale::zones"] = nat("forge::locale::zones", 0, 0, [](Interp&, Args&) -> Value { return strs(lc::zone_names()); });
    B["forge::locale::local_zone"] = nat("forge::locale::local_zone", 0, 0, [](Interp&, Args&) -> Value { return strv(lc::local_zone()); });
    B["forge::locale::calendar"] = nat("forge::locale::calendar", 2, 2, [](Interp& in, Args& a) -> Value {
        auto c = lc::calendar_from_name(s_of(in, a[1], "calendar")); if (!c) in.raise("ValueError", "unknown calendar");
        auto d = dt_of(in, a[0], {}); auto r = lc::from_gregorian(*c, d.year, d.month, d.day);
        return map_of({{"year", std::int64_t(r.year)}, {"month", std::int64_t(r.month)}, {"day", std::int64_t(r.day)}, {"era", strv(lc::era_name(*c, r.era))}, {"month_name", strv(lc::calendar_month_name(*c, r))}});
    });
    B["forge::locale::message"] = nat("forge::locale::message", 1, 3, [](Interp& in, Args& a) -> Value {
        lc::message_args args;
        if (a.size() > 1 && !is_nil(a[1])) {
            if (auto* m = as<Map>(a[1])) { for (auto& [k, v] : m->entries) if (auto* s = as<String>(k)) args.emplace(s->s, mv_of(in, v)); }
            else if (auto* l = as<List>(a[1])) { for (auto& e : l->items) if (auto* p = as<List>(e); p && p->items.size() == 2) args.emplace(s_of(in, p->items[0], "message key"), mv_of(in, p->items[1])); }
        }
        return strv(okv(in, lc::format_message(s_of(in, a[0], "message"), args, opt_s(in, a, 2, "locale"))));
    });
    B["forge::locale::first_day"] = nat("forge::locale::first_day", 0, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(int(lc::first_day_of_week(opt_s(in, a, 0, "locale")))); });
    B["forge::locale::hour_cycle"] = nat("forge::locale::hour_cycle", 0, 1, [](Interp& in, Args& a) -> Value {
        static const char* n[] = {"h11", "h12", "h23", "h24"}; return strv(n[int(lc::preferred_hour_cycle(opt_s(in, a, 0, "locale")))]);
    });
    B["forge::locale::currency_for_region"] = nat("forge::locale::currency_for_region", 1, 1, [](Interp& in, Args& a) -> Value { return strv(lc::currency_for_region(s_of(in, a[0], "region"))); });
    B["forge::locale::catalog"] = nat("forge::locale::catalog", 1, 3, [](Interp& in, Args& a) -> Value {
        if (!in.caps.fs) in.raise("PermissionError", "file access is disabled in this sandbox");
        std::shared_ptr<lc::catalog> c;
        enforce_scope(in, SecScope::fs_read, s_of(in, a[0], a.size() >= 2 ? "dir" : "path"));
        if (a.size() >= 2) c = std::make_shared<lc::catalog>(okv(in, lc::catalog::find(s_of(in, a[0], "dir"), s_of(in, a[1], "domain"), opt_s(in, a, 2, "locale"))));
        else c = std::make_shared<lc::catalog>(okv(in, lc::catalog::load(s_of(in, a[0], "path"))));
        auto h = make<Handle>("catalog"); h->payload = c; return obj(h);
    });
    auto& C = M["handle:catalog"];
    auto cat = [](Interp& in, const Value& v) { return h_of(in, v, "catalog").payload_as<lc::catalog>(); };
    C["gettext"] = nat("catalog.gettext", 2, 2, [cat](Interp& in, Args& a) -> Value { return strv(std::string(cat(in, a[0])->gettext(s_of(in, a[1], "msgid")))); });
    C["ngettext"] = nat("catalog.ngettext", 4, 4, [cat](Interp& in, Args& a) -> Value { auto s = s_of(in, a[1], "singular"), p = s_of(in, a[2], "plural"); return strv(std::string(cat(in, a[0])->ngettext(s, p, std::uint64_t(f_of(in, a[3], "n"))))); });
    C["pgettext"] = nat("catalog.pgettext", 3, 3, [cat](Interp& in, Args& a) -> Value { auto c = s_of(in, a[1], "context"), m = s_of(in, a[2], "msgid"); return strv(std::string(cat(in, a[0])->pgettext(c, m))); });
    C["header"] = nat("catalog.header", 2, 2, [cat](Interp& in, Args& a) -> Value { return strv(cat(in, a[0])->header(s_of(in, a[1], "field"))); });
    C["size"] = nat("catalog.size", 1, 1, [cat](Interp& in, Args& a) -> Value { return std::int64_t(cat(in, a[0])->size()); });
    C["close"] = nat("catalog.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
}
} // namespace
void install_text(Shared& sh) { install(sh); }
} // namespace forge::script
