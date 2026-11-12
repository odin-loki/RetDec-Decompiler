// forge::os::locale: list formatting, unit formatting and unit conversion.
#include "locale_internal.hpp"
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace forge::os::locale {
using namespace detail;

std::string format_list(std::span<const std::string> items, std::string_view locale, list_type t, list_width w) {
    if (items.empty()) return {};
    if (items.size() == 1) return items[0];
    auto b = resolve(locale, category::numeric);
    std::string type = t == list_type::and_ ? "standard" : t == list_type::or_ ? "or" : "unit";
    if (w == list_width::short_) type += "-short"; else if (w == list_width::narrow) type += "-narrow";
    auto pat = [&](const char* part) {
        if (auto v = find(b, "list/" + type + "/" + part)) return std::string(*v);
        auto base = type.substr(0, type.find('-'));
        if (auto v = find(b, "list/" + base + "/" + part)) return std::string(*v);
        return std::string(part == std::string_view("end") || part == std::string_view("2") ? "{0}, {1}" : "{0}, {1}");
    };
    // Spanish: "y" -> "e" before /i/, "o" -> "u" before /o/.
    auto fix = [&](std::string p, const std::string& next) {
        if (parse_tag(b).language != "es") return p;
        std::string n = text::to_lower(next);
        bool i_sound = (n.starts_with("i") || n.starts_with("hi") || n.starts_with("\xC3\xAD") || n.starts_with("h\xC3\xAD")) && !n.starts_with("hie") && !n.starts_with("hia");
        bool o_sound = n.starts_with("o") || n.starts_with("ho") || n.starts_with("\xC3\xB3") || n.starts_with("8") || n.starts_with("11");
        if (i_sound) p = replace_all(p, " y {1}", " e {1}");
        if (o_sound) p = replace_all(p, " o {1}", " u {1}");
        return p;
    };
    if (items.size() == 2) return sub(fix(pat("2"), items[1]), items[0], items[1]);
    std::string r = sub(fix(pat("end"), items.back()), items[items.size() - 2], items.back());
    for (std::size_t i = items.size() - 2; i-- > 1;) r = sub(pat("middle"), items[i], r);
    return sub(pat("start"), items[0], r);
}

namespace {
const char* widths[] = {"long", "short", "narrow"};
std::string strip_category(std::string_view u) {
    static const char* cats[] = {"acceleration-", "angle-", "area-", "concentr-", "consumption-", "digital-", "duration-", "electric-", "energy-", "frequency-",
                                 "graphics-", "length-", "mass-", "power-", "pressure-", "speed-", "temperature-", "volume-", "light-", "force-", "torque-"};
    for (auto c : cats) if (u.starts_with(c)) return std::string(u.substr(std::strlen(c)));
    return std::string(u);
}
}  // namespace
std::string unit_display_name(std::string_view unit, std::string_view locale, unit_width w) {
    auto b = resolve(locale, category::numeric); auto u = strip_category(unit);
    if (auto v = find(b, std::string("unit/") + widths[int(w)] + "/" + u + "/displayName")) return std::string(*v);
    return u;
}
std::string format_unit(double v, std::string_view unit, std::string_view locale, unit_width w, const number_options& o) {
    auto b = resolve(locale, category::numeric); auto u = strip_category(unit);
    auto num = format_number(v, locale, o);
    // Plural category from the visible digits.
    std::string plain; { number_options x = o; x.grouping = false; x.numbering = "latn"; plain = format_number(std::fabs(v), "en", x); plain = replace_all(plain, ",", ""); }
    auto cat = plural_name(plural_category(std::string_view(plain), locale));
    std::string pre = std::string("unit/") + widths[int(w)] + "/";
    auto pattern_for = [&](const std::string& name) -> std::optional<std::string> {
        if (auto p = find(b, pre + name + "/" + cat)) return std::string(*p);
        if (auto p = find(b, pre + name + "/other")) return std::string(*p);
        return std::nullopt;
    };
    if (auto p = pattern_for(u)) return sub(*p, num);
    if (auto per = u.find("-per-"); per != std::string::npos) {
        auto num_u = u.substr(0, per), den = u.substr(per + 5);
        std::string left = pattern_for(num_u) ? sub(*pattern_for(num_u), num) : num + " " + num_u;
        if (auto pp = find(b, pre + den + "/perUnitPattern")) return sub(*pp, left);
        std::string dn = unit_display_name(den, locale, w);
        if (auto dp = pattern_for(den)) { auto d = sub(*dp, ""); while (!d.empty() && (d.front() == ' ' || (unsigned char)d.front() == 0xC2)) d.erase(0, d.front() == ' ' ? 1 : 2); dn = d; }
        return sub(get(b, pre + "per").empty() ? "{0}/{1}" : get(b, pre + "per"), left, dn);
    }
    return num + " " + u;
}

namespace {
struct conv { const char* name; const char* kind; double factor; double offset; };
// factor/offset to the base unit of each kind: base = v * factor + offset.
constexpr conv convs[] = {
    {"meter", "length", 1, 0}, {"kilometer", "length", 1000, 0}, {"decimeter", "length", 0.1, 0}, {"centimeter", "length", 0.01, 0}, {"millimeter", "length", 0.001, 0},
    {"micrometer", "length", 1e-6, 0}, {"nanometer", "length", 1e-9, 0}, {"mile", "length", 1609.344, 0}, {"yard", "length", 0.9144, 0}, {"foot", "length", 0.3048, 0},
    {"inch", "length", 0.0254, 0}, {"nautical-mile", "length", 1852, 0}, {"light-year", "length", 9.4607304725808e15, 0}, {"mile-scandinavian", "length", 10000, 0},
    {"kilogram", "mass", 1, 0}, {"gram", "mass", 0.001, 0}, {"milligram", "mass", 1e-6, 0}, {"tonne", "mass", 1000, 0}, {"pound", "mass", 0.45359237, 0},
    {"ounce", "mass", 0.028349523125, 0}, {"stone", "mass", 6.35029318, 0}, {"ton", "mass", 907.18474, 0},
    {"kelvin", "temperature", 1, 0}, {"celsius", "temperature", 1, 273.15}, {"fahrenheit", "temperature", 5.0 / 9.0, 273.15 - 32 * 5.0 / 9.0},
    {"liter", "volume", 1, 0}, {"milliliter", "volume", 0.001, 0}, {"centiliter", "volume", 0.01, 0}, {"deciliter", "volume", 0.1, 0}, {"cubic-meter", "volume", 1000, 0},
    {"cubic-centimeter", "volume", 0.001, 0}, {"gallon", "volume", 3.785411784, 0}, {"gallon-imperial", "volume", 4.54609, 0}, {"quart", "volume", 0.946352946, 0},
    {"pint", "volume", 0.473176473, 0}, {"cup", "volume", 0.2365882365, 0}, {"fluid-ounce", "volume", 0.0295735295625, 0}, {"tablespoon", "volume", 0.01478676478125, 0},
    {"teaspoon", "volume", 0.00492892159375, 0},
    {"meter-per-second", "speed", 1, 0}, {"kilometer-per-hour", "speed", 1 / 3.6, 0}, {"mile-per-hour", "speed", 0.44704, 0}, {"knot", "speed", 1852.0 / 3600, 0},
    {"square-meter", "area", 1, 0}, {"square-kilometer", "area", 1e6, 0}, {"hectare", "area", 1e4, 0}, {"square-centimeter", "area", 1e-4, 0}, {"square-mile", "area", 2589988.110336, 0},
    {"acre", "area", 4046.8564224, 0}, {"square-foot", "area", 0.09290304, 0}, {"square-inch", "area", 0.00064516, 0},
    {"second", "duration", 1, 0}, {"millisecond", "duration", 1e-3, 0}, {"microsecond", "duration", 1e-6, 0}, {"nanosecond", "duration", 1e-9, 0}, {"minute", "duration", 60, 0},
    {"hour", "duration", 3600, 0}, {"day", "duration", 86400, 0}, {"week", "duration", 604800, 0}, {"year", "duration", 31556952, 0}, {"month", "duration", 2629746, 0},
    {"byte", "digital", 8, 0}, {"bit", "digital", 1, 0}, {"kilobyte", "digital", 8e3, 0}, {"megabyte", "digital", 8e6, 0}, {"gigabyte", "digital", 8e9, 0}, {"terabyte", "digital", 8e12, 0},
    {"petabyte", "digital", 8e15, 0}, {"kilobit", "digital", 1e3, 0}, {"megabit", "digital", 1e6, 0}, {"gigabit", "digital", 1e9, 0},
    {"kibibyte", "digital", 8192, 0}, {"mebibyte", "digital", 8388608, 0}, {"gibibyte", "digital", 8589934592.0, 0},
    {"pascal", "pressure", 1, 0}, {"hectopascal", "pressure", 100, 0}, {"kilopascal", "pressure", 1000, 0}, {"bar", "pressure", 1e5, 0}, {"millibar", "pressure", 100, 0},
    {"atmosphere", "pressure", 101325, 0}, {"pound-force-per-square-inch", "pressure", 6894.757293168, 0}, {"millimeter-ofhg", "pressure", 133.322387415, 0}, {"inch-ofhg", "pressure", 3386.389, 0},
    {"joule", "energy", 1, 0}, {"kilojoule", "energy", 1000, 0}, {"calorie", "energy", 4.184, 0}, {"kilocalorie", "energy", 4184, 0}, {"kilowatt-hour", "energy", 3.6e6, 0},
    {"watt", "power", 1, 0}, {"kilowatt", "power", 1000, 0}, {"horsepower", "power", 745.69987158227, 0},
};
const conv* find_conv(std::string_view n) { auto s = strip_category(n); for (auto& c : convs) if (s == c.name) return &c; return nullptr; }
}  // namespace
result<double> convert_unit(double v, std::string_view from, std::string_view to) {
    result<double> r; auto a = find_conv(from), b = find_conv(to);
    if (!a || !b) { r.err = {EINVAL, "convert_unit: unknown unit '" + std::string(a ? to : from) + "'"}; return r; }
    if (std::string_view(a->kind) != b->kind) { r.err = {EINVAL, "convert_unit: cannot convert " + std::string(a->kind) + " to " + b->kind}; return r; }
    r.value = (v * a->factor + a->offset - b->offset) / b->factor;
    return r;
}
}  // namespace forge::os::locale
