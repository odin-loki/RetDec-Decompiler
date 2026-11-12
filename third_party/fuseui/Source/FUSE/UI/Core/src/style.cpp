#include "fuse/ui/style.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace fuse::ui {

namespace {
using T = PropType;
// clang-format off
constexpr PropInfo kProps[] = {
    {Prop::Display, "display", T::Bool, false, false, true},
    {Prop::Layout, "layout", T::Enum, false, false, true},
    {Prop::Direction, "flex-direction", T::Enum, false, false, true},
    {Prop::Width, "width", T::Length, false, true, true},
    {Prop::Height, "height", T::Length, false, true, true},
    {Prop::MinWidth, "min-width", T::Length, false, true, true},
    {Prop::MinHeight, "min-height", T::Length, false, true, true},
    {Prop::MaxWidth, "max-width", T::Length, false, true, true},
    {Prop::MaxHeight, "max-height", T::Length, false, true, true},
    {Prop::Basis, "flex-basis", T::Length, false, true, true},
    {Prop::Grow, "flex-grow", T::Number, false, true, true},
    {Prop::Shrink, "flex-shrink", T::Number, false, true, true},
    {Prop::PaddingLeft, "padding-left", T::Length, false, true, true},
    {Prop::PaddingTop, "padding-top", T::Length, false, true, true},
    {Prop::PaddingRight, "padding-right", T::Length, false, true, true},
    {Prop::PaddingBottom, "padding-bottom", T::Length, false, true, true},
    {Prop::MarginLeft, "margin-left", T::Length, false, true, true},
    {Prop::MarginTop, "margin-top", T::Length, false, true, true},
    {Prop::MarginRight, "margin-right", T::Length, false, true, true},
    {Prop::MarginBottom, "margin-bottom", T::Length, false, true, true},
    {Prop::Gap, "gap", T::Length, false, true, true},
    {Prop::AlignItems, "align-items", T::Enum, false, false, true},
    {Prop::AlignSelf, "align-self", T::Enum, false, false, true},
    {Prop::Justify, "justify-content", T::Enum, false, false, true},
    {Prop::Wrap, "flex-wrap", T::Bool, false, false, true},
    {Prop::Overflow, "overflow", T::Enum, false, false, true},
    {Prop::GridColumns, "grid-template-columns", T::Tracks, false, false, true},
    {Prop::GridColumn, "grid-column", T::Number, false, false, true},
    {Prop::GridColumnSpan, "grid-column-span", T::Number, false, false, true},
    {Prop::GridRow, "grid-row", T::Number, false, false, true},
    {Prop::GridRowSpan, "grid-row-span", T::Number, false, false, true},
    {Prop::Left, "left", T::Length, false, true, true},
    {Prop::Top, "top", T::Length, false, true, true},
    {Prop::Right, "right", T::Length, false, true, true},
    {Prop::Bottom, "bottom", T::Length, false, true, true},
    {Prop::CenterX, "center-x", T::Bool, false, false, true},
    {Prop::CenterY, "center-y", T::Bool, false, false, true},
    {Prop::Background, "background", T::Color, false, true, false},
    {Prop::Background2, "background-end", T::Color, false, true, false},
    {Prop::Gradient, "gradient", T::Enum, false, false, false},
    {Prop::BorderColor, "border-color", T::Color, false, true, false},
    {Prop::BorderWidth, "border-width", T::Length, false, true, false},
    {Prop::RadiusTL, "border-top-left-radius", T::Length, false, true, false},
    {Prop::RadiusTR, "border-top-right-radius", T::Length, false, true, false},
    {Prop::RadiusBR, "border-bottom-right-radius", T::Length, false, true, false},
    {Prop::RadiusBL, "border-bottom-left-radius", T::Length, false, true, false},
    {Prop::Color, "color", T::Color, true, true, false},
    {Prop::FontSize, "font-size", T::Length, true, true, true},
    {Prop::TextAlign, "text-align", T::Enum, true, false, false},
    {Prop::Opacity, "opacity", T::Number, false, true, false},
    {Prop::ShadowColor, "shadow-color", T::Color, false, true, false},
    {Prop::ShadowBlur, "shadow-blur", T::Length, false, true, false},
    {Prop::ShadowX, "shadow-x", T::Length, false, true, false},
    {Prop::ShadowY, "shadow-y", T::Length, false, true, false},
    {Prop::PointerEvents, "pointer-events", T::Bool, false, false, false},
    {Prop::BackdropBlur, "backdrop-blur", T::Length, false, true, false},
};
// clang-format on
static_assert(sizeof(kProps) / sizeof(kProps[0]) == kPropCount);

struct EnumWord {
    Prop prop;
    std::string_view word;
    u8 value;
};
constexpr EnumWord kEnums[] = {
    {Prop::Layout, "box", u8(LayoutKind::Box)},       {Prop::Layout, "flex", u8(LayoutKind::Box)},
    {Prop::Layout, "grid", u8(LayoutKind::Grid)},     {Prop::Layout, "stack", u8(LayoutKind::Stack)},
    {Prop::Layout, "anchor", u8(LayoutKind::Anchor)}, {Prop::Layout, "absolute", u8(LayoutKind::Absolute)},
    {Prop::Layout, "flow", u8(LayoutKind::Flow)},     {Prop::Direction, "row", u8(Direction::Row)},
    {Prop::Direction, "column", u8(Direction::Column)},
    {Prop::AlignItems, "start", u8(Align::Start)},    {Prop::AlignItems, "flex-start", u8(Align::Start)},
    {Prop::AlignItems, "center", u8(Align::Center)},  {Prop::AlignItems, "end", u8(Align::End)},
    {Prop::AlignItems, "flex-end", u8(Align::End)},   {Prop::AlignItems, "stretch", u8(Align::Stretch)},
    {Prop::AlignSelf, "auto", u8(Align::Auto)},       {Prop::AlignSelf, "start", u8(Align::Start)},
    {Prop::AlignSelf, "center", u8(Align::Center)},   {Prop::AlignSelf, "end", u8(Align::End)},
    {Prop::AlignSelf, "stretch", u8(Align::Stretch)},
    {Prop::Justify, "start", u8(Justify::Start)},     {Prop::Justify, "flex-start", u8(Justify::Start)},
    {Prop::Justify, "center", u8(Justify::Center)},   {Prop::Justify, "end", u8(Justify::End)},
    {Prop::Justify, "flex-end", u8(Justify::End)},    {Prop::Justify, "space-between", u8(Justify::SpaceBetween)},
    {Prop::Justify, "space-around", u8(Justify::SpaceAround)},
    {Prop::Overflow, "visible", u8(Overflow::Visible)}, {Prop::Overflow, "hidden", u8(Overflow::Clip)},
    {Prop::Overflow, "clip", u8(Overflow::Clip)},     {Prop::Overflow, "scroll", u8(Overflow::Scroll)},
    {Prop::Overflow, "auto", u8(Overflow::Scroll)},
    {Prop::Gradient, "none", u8(GradientDir::None)},  {Prop::Gradient, "vertical", u8(GradientDir::Vertical)},
    {Prop::Gradient, "horizontal", u8(GradientDir::Horizontal)},
    {Prop::TextAlign, "left", u8(TextAlignment::Start)}, {Prop::TextAlign, "start", u8(TextAlignment::Start)},
    {Prop::TextAlign, "center", u8(TextAlignment::Center)}, {Prop::TextAlign, "right", u8(TextAlignment::End)},
    {Prop::TextAlign, "end", u8(TextAlignment::End)},
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parseNumber(std::string_view s, f32& out, std::string_view& unit) {
    s = trim(s);
    std::size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '-' || s[i] == '+')) ++i;
    if (i == 0) return false;
    std::string buf(s.substr(0, i));
    char* end = nullptr;
    out = std::strtof(buf.c_str(), &end);
    if (end != buf.c_str() + buf.size()) return false;
    unit = s.substr(i);
    return true;
}

bool parseLength(std::string_view s, Length& out) {
    s = trim(s);
    if (s == "auto") { out = {}; return true; }
    f32 v;
    std::string_view unit;
    if (!parseNumber(s, v, unit)) return false;
    if (unit.empty() || unit == "px" || unit == "dp") out = Length::px(v);
    else if (unit == "%") out = Length::pct(v);
    else if (unit == "fr") out = Length::fr(v);
    else return false;
    return true;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = char(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

struct NamedColor { std::string_view name; u32 hex; };
constexpr NamedColor kNamed[] = {{"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000},
                                 {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"gray", 0x808080}, {"grey", 0x808080},
                                 {"orange", 0xFFA500}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}};
} // namespace

const PropInfo& propInfo(Prop p) { return kProps[int(p)]; }
bool findProp(std::string_view name, Prop& out) {
    for (const auto& pi : kProps)
        if (pi.cssName == name) { out = pi.id; return true; }
    return false;
}

bool parseColor(std::string_view s, Color& out) {
    s = trim(s);
    if (s == "transparent") { out = {}; return true; }
    for (const auto& n : kNamed)
        if (n.name == s) { out = Color::rgb(n.hex); return true; }
    if (!s.empty() && s[0] == '#') {
        const std::string_view h = s.substr(1);
        int d[8];
        for (std::size_t i = 0; i < h.size() && i < 8; ++i)
            if ((d[i] = hexval(h[i])) < 0) return false;
        if (h.size() == 3 || h.size() == 4) {
            out = {f32(d[0] * 17) / 255.f, f32(d[1] * 17) / 255.f, f32(d[2] * 17) / 255.f,
                   h.size() == 4 ? f32(d[3] * 17) / 255.f : 1.f};
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            out = {f32(d[0] * 16 + d[1]) / 255.f, f32(d[2] * 16 + d[3]) / 255.f, f32(d[4] * 16 + d[5]) / 255.f,
                   h.size() == 8 ? f32(d[6] * 16 + d[7]) / 255.f : 1.f};
            return true;
        }
        return false;
    }
    if (s.starts_with("rgb")) {
        const auto open = s.find('('), close = s.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos) return false;
        std::string_view args = s.substr(open + 1, close - open - 1);
        f32 v[4] = {0, 0, 0, 1};
        int n = 0;
        while (!args.empty() && n < 4) {
            const auto comma = args.find(',');
            std::string_view unit;
            if (!parseNumber(args.substr(0, comma), v[n], unit)) return false;
            if (unit == "%") v[n] = v[n] * (n < 3 ? 255.f : 1.f) / 100.f;
            ++n;
            if (comma == std::string_view::npos) break;
            args.remove_prefix(comma + 1);
        }
        if (n < 3) return false;
        out = {v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, v[3]};
        return true;
    }
    return false;
}

bool parsePropValue(Prop p, std::string_view text, PropValue& out) {
    text = trim(text);
    const PropInfo& pi = propInfo(p);
    out = {};
    out.type = pi.type;
    switch (pi.type) {
    case T::Bool:
        if (p == Prop::Display) { out.b = text != "none"; return true; }
        if (p == Prop::Wrap) { out.b = text == "wrap" || text == "true"; return text == "wrap" || text == "nowrap" || text == "true" || text == "false"; }
        if (p == Prop::PointerEvents) { out.b = text != "none"; return true; }
        out.b = text == "true" || text == "1" || text == "yes";
        return text == "true" || text == "false" || text == "1" || text == "0" || text == "yes" || text == "no";
    case T::Enum:
        for (const auto& e : kEnums)
            if (e.prop == p && e.word == text) { out.e = e.value; return true; }
        return false;
    case T::Length: return parseLength(text, out.len);
    case T::Number: {
        std::string_view unit;
        if (!parseNumber(text, out.num, unit)) return false;
        if (unit == "%") out.num /= 100.f;
        return unit.empty() || unit == "%";
    }
    case T::Color: return parseColor(text, out.color);
    case T::Tracks: {
        out.trackCount = 0;
        while (!text.empty()) {
            text = trim(text);
            const auto sp = text.find_first_of(" \t");
            const std::string_view tok = text.substr(0, sp);
            if (!tok.empty()) {
                if (out.trackCount >= kMaxGridTracks || !parseLength(tok, out.tracks[out.trackCount])) return false;
                ++out.trackCount;
            }
            if (sp == std::string_view::npos) break;
            text.remove_prefix(sp);
        }
        return out.trackCount > 0;
    }
    }
    return false;
}

void applyProp(ComputedStyle& s, Prop p, const PropValue& v) {
    switch (p) {
    case Prop::Display: s.visible = v.b; break;
    case Prop::Layout: s.layout = LayoutKind(v.e); break;
    case Prop::Direction: s.direction = Direction(v.e); break;
    case Prop::Width: s.width = v.len; break;
    case Prop::Height: s.height = v.len; break;
    case Prop::MinWidth: s.minWidth = v.len; break;
    case Prop::MinHeight: s.minHeight = v.len; break;
    case Prop::MaxWidth: s.maxWidth = v.len; break;
    case Prop::MaxHeight: s.maxHeight = v.len; break;
    case Prop::Basis: s.basis = v.len; break;
    case Prop::Grow: s.grow = v.num; break;
    case Prop::Shrink: s.shrink = v.num; break;
    case Prop::PaddingLeft: s.padding.l = v.len.value; break;
    case Prop::PaddingTop: s.padding.t = v.len.value; break;
    case Prop::PaddingRight: s.padding.r = v.len.value; break;
    case Prop::PaddingBottom: s.padding.b = v.len.value; break;
    case Prop::MarginLeft: s.margin.l = v.len.value; break;
    case Prop::MarginTop: s.margin.t = v.len.value; break;
    case Prop::MarginRight: s.margin.r = v.len.value; break;
    case Prop::MarginBottom: s.margin.b = v.len.value; break;
    case Prop::Gap: s.gap = v.len.value; break;
    case Prop::AlignItems: s.alignItems = Align(v.e); break;
    case Prop::AlignSelf: s.alignSelf = Align(v.e); break;
    case Prop::Justify: s.justify = Justify(v.e); break;
    case Prop::Wrap: s.wrap = v.b; break;
    case Prop::Overflow: s.overflow = Overflow(v.e); break;
    case Prop::GridColumns: s.gridColumns = v.tracks; s.gridColumnCount = v.trackCount; break;
    case Prop::GridColumn: s.gridColumn = u8(v.num); break;
    case Prop::GridColumnSpan: s.gridColumnSpan = u8(std::max(1.f, v.num)); break;
    case Prop::GridRow: s.gridRow = u8(v.num); break;
    case Prop::GridRowSpan: s.gridRowSpan = u8(std::max(1.f, v.num)); break;
    case Prop::Left: s.left = v.len; break;
    case Prop::Top: s.top = v.len; break;
    case Prop::Right: s.right = v.len; break;
    case Prop::Bottom: s.bottom = v.len; break;
    case Prop::CenterX: s.centerX = v.b; break;
    case Prop::CenterY: s.centerY = v.b; break;
    case Prop::Background: s.background = v.color; break;
    case Prop::Background2: s.background2 = v.color; break;
    case Prop::Gradient: s.gradient = GradientDir(v.e); break;
    case Prop::BorderColor: s.borderColor = v.color; break;
    case Prop::BorderWidth: s.borderWidth = v.len.value; break;
    case Prop::RadiusTL: s.radius[0] = v.len.value; break;
    case Prop::RadiusTR: s.radius[1] = v.len.value; break;
    case Prop::RadiusBR: s.radius[2] = v.len.value; break;
    case Prop::RadiusBL: s.radius[3] = v.len.value; break;
    case Prop::Color: s.color = v.color; break;
    case Prop::FontSize: s.fontSize = v.len.value; break;
    case Prop::TextAlign: s.textAlign = TextAlignment(v.e); break;
    case Prop::Opacity: s.opacity = std::clamp(v.num, 0.f, 1.f); break;
    case Prop::ShadowColor: s.shadowColor = v.color; break;
    case Prop::ShadowBlur: s.shadowBlur = v.len.value; break;
    case Prop::BackdropBlur: s.backdropBlur = v.len.value; break;
    case Prop::ShadowX: s.shadowX = v.len.value; break;
    case Prop::ShadowY: s.shadowY = v.len.value; break;
    case Prop::PointerEvents: s.pointerEvents = v.b; break;
    case Prop::Count: break;
    }
}

PropValue readProp(const ComputedStyle& s, Prop p) {
    switch (p) {
    case Prop::Display: return PropValue::ofBool(s.visible);
    case Prop::Opacity: return PropValue::ofNumber(s.opacity);
    case Prop::Color: return PropValue::ofColor(s.color);
    case Prop::Background: return PropValue::ofColor(s.background);
    case Prop::BorderColor: return PropValue::ofColor(s.borderColor);
    case Prop::FontSize: return PropValue::ofLength(Length::px(s.fontSize));
    case Prop::Width: return PropValue::ofLength(s.width);
    case Prop::Height: return PropValue::ofLength(s.height);
    case Prop::Grow: return PropValue::ofNumber(s.grow);
    case Prop::Gap: return PropValue::ofLength(Length::px(s.gap));
    case Prop::Layout: return PropValue::ofEnum(u8(s.layout));
    case Prop::Direction: return PropValue::ofEnum(u8(s.direction));
    case Prop::TextAlign: return PropValue::ofEnum(u8(s.textAlign));
    default: break;
    }
    PropValue v;
    v.type = propInfo(p).type;
    return v;
}

void inheritFrom(ComputedStyle& s, const ComputedStyle& parent) {
    s.color = parent.color;
    s.fontSize = parent.fontSize;
    s.textAlign = parent.textAlign;
}

// ---- Parser --------------------------------------------------------------------------------------------------------
namespace {

struct Cursor {
    std::string_view s;
    std::size_t i = 0;
    u32 line = 1;
    void skipWs() {
        for (;;) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
                if (s[i] == '\n') ++line;
                ++i;
            }
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                const auto e = s.find("*/", i + 2);
                for (std::size_t k = i; k < (e == std::string_view::npos ? s.size() : e); ++k)
                    if (s[k] == '\n') ++line;
                i = e == std::string_view::npos ? s.size() : e + 2;
                continue;
            }
            break;
        }
    }
};


bool parseSelector(std::string_view text, StyleRule& rule, bool& isRoot) {
    rule.chain.clear();
    isRoot = false;
    std::size_t i = 0;
    auto comb = SimpleSelector::Combinator::None;
    u32 ids = 0, cls = 0, types = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
            if (!rule.chain.empty() && comb == SimpleSelector::Combinator::None) comb = SimpleSelector::Combinator::Descendant;
        }
        if (i >= text.size()) break;
        if (text[i] == '>') {
            comb = SimpleSelector::Combinator::Child;
            ++i;
            continue;
        }
        SimpleSelector sel;
        sel.combinator = rule.chain.empty() ? SimpleSelector::Combinator::None : comb;
        bool any = false;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '>') {
            const char c = text[i];
            auto ident = [&](std::size_t from) {
                std::size_t k = from;
                while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '-' || text[k] == '_')) ++k;
                return k;
            };
            if (c == '*') { ++i; any = true; }
            else if (c == '#') { const auto e = ident(i + 1); sel.idHash = hashId(text.substr(i + 1, e - i - 1)); i = e; ++ids; any = true; }
            else if (c == '.') {
                const auto e = ident(i + 1);
                if (sel.classCount >= 4) return false;
                sel.classes[sel.classCount++] = hashId(text.substr(i + 1, e - i - 1));
                i = e; ++cls; any = true;
            } else if (c == ':') {
                const auto e = ident(i + 1);
                const auto name = text.substr(i + 1, e - i - 1);
                i = e; any = true;
                if (name == "root") { isRoot = true; continue; }
                u32 bit = 0;
                if (name == "hover") bit = kStateHover;
                else if (name == "pressed" || name == "active") bit = kStatePressed;
                else if (name == "focus") bit = kStateFocus;
                else if (name == "focus-visible") bit = kStateFocusVisible;
                else if (name == "disabled") bit = kStateDisabled;
                else if (name == "checked") bit = kStateChecked;
                else if (name == "selected") bit = kStateSelected;
                else return false;
                sel.states |= bit;
                ++cls;
            } else if (std::isalpha(static_cast<unsigned char>(c))) {
                const auto e = ident(i);
                sel.typeHash = hashId(text.substr(i, e - i));
                i = e; ++types; any = true;
            } else return false;
        }
        if (!any) return false;
        rule.chain.push_back(sel);
        comb = SimpleSelector::Combinator::None;
    }
    rule.specificity = (std::min(ids, 255u) << 16) | (std::min(cls, 255u) << 8) | std::min(types, 255u);
    return !rule.chain.empty() || isRoot;
}

void addDecl(StyleRule& rule, Prop p, std::string_view value, u32 line, std::vector<StyleDiagnostic>* diags) {
    Declaration d;
    d.prop = p;
    if (value.find("var(") != std::string_view::npos) {
        d.raw.assign(value);
        rule.decls.push_back(std::move(d));
        return;
    }
    if (!parsePropValue(p, value, d.value)) {
        if (diags) diags->push_back({StyleError::BadValue, line, std::string(propInfo(p).cssName) + ": " + std::string(value)});
        return;
    }
    rule.decls.push_back(std::move(d));
}

void splitWords(std::string_view v, std::string_view* out, int& n, int max) {
    n = 0;
    std::size_t i = 0;
    while (i < v.size() && n < max) {
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        std::size_t s = i;
        int depth = 0;
        while (i < v.size() && (depth > 0 || !std::isspace(static_cast<unsigned char>(v[i])))) {
            if (v[i] == '(') ++depth;
            if (v[i] == ')') --depth;
            ++i;
        }
        if (i > s) out[n++] = v.substr(s, i - s);
    }
}

void addShorthand(StyleRule& rule, std::string_view name, std::string_view value, u32 line,
                  std::vector<StyleDiagnostic>* diags) {
    std::string_view w[4];
    int n = 0;
    auto four = [&](Prop t, Prop r, Prop b, Prop l) {
        splitWords(value, w, n, 4);
        if (n == 0) return;
        std::string_view top = w[0], right = n > 1 ? w[1] : w[0], bottom = n > 2 ? w[2] : w[0],
                         left = n > 3 ? w[3] : (n > 1 ? w[1] : w[0]);
        addDecl(rule, t, top, line, diags);
        addDecl(rule, r, right, line, diags);
        addDecl(rule, b, bottom, line, diags);
        addDecl(rule, l, left, line, diags);
    };
    if (name == "padding") four(Prop::PaddingTop, Prop::PaddingRight, Prop::PaddingBottom, Prop::PaddingLeft);
    else if (name == "margin") four(Prop::MarginTop, Prop::MarginRight, Prop::MarginBottom, Prop::MarginLeft);
    else if (name == "border-radius") four(Prop::RadiusTL, Prop::RadiusTR, Prop::RadiusBR, Prop::RadiusBL);
    else if (name == "border") {
        splitWords(value, w, n, 4);
        for (int k = 0; k < n; ++k) {
            PropValue tmp;
            if (parsePropValue(Prop::BorderWidth, w[k], tmp)) addDecl(rule, Prop::BorderWidth, w[k], line, diags);
            else if (w[k] == "solid" || w[k] == "none") continue;
            else addDecl(rule, Prop::BorderColor, w[k], line, diags);
        }
    } else if (name == "flex") {
        splitWords(value, w, n, 3);
        if (n > 0) addDecl(rule, Prop::Grow, w[0], line, diags);
        if (n > 1) addDecl(rule, Prop::Shrink, w[1], line, diags);
        if (n > 2) addDecl(rule, Prop::Basis, w[2], line, diags);
    } else if (name == "box-shadow") { // <x> <y> <blur> <color>
        splitWords(value, w, n, 4);
        if (n == 4) {
            addDecl(rule, Prop::ShadowX, w[0], line, diags);
            addDecl(rule, Prop::ShadowY, w[1], line, diags);
            addDecl(rule, Prop::ShadowBlur, w[2], line, diags);
            addDecl(rule, Prop::ShadowColor, w[3], line, diags);
        } else if (diags) diags->push_back({StyleError::BadValue, line, "box-shadow"});
    } else if (name == "background" && value.starts_with("linear-gradient")) {
        // linear-gradient(vertical|horizontal, c0, c1)
        const auto open = value.find('('), close = value.rfind(')');
        std::string_view args = value.substr(open + 1, close - open - 1);
        std::string_view parts[3];
        int k = 0;
        int depth = 0;
        std::size_t s = 0;
        for (std::size_t i = 0; i <= args.size() && k < 3; ++i) {
            if (i < args.size() && args[i] == '(') ++depth;
            if (i < args.size() && args[i] == ')') --depth;
            if (i == args.size() || (args[i] == ',' && depth == 0)) {
                parts[k++] = trim(args.substr(s, i - s));
                s = i + 1;
            }
        }
        if (k == 3) {
            addDecl(rule, Prop::Gradient, parts[0] == "to right" ? "horizontal" : (parts[0] == "to bottom" ? "vertical" : parts[0]), line, diags);
            addDecl(rule, Prop::Background, parts[1], line, diags);
            addDecl(rule, Prop::Background2, parts[2], line, diags);
        } else if (diags) diags->push_back({StyleError::BadValue, line, "linear-gradient"});
    } else if (diags) diags->push_back({StyleError::UnknownProperty, line, std::string(name)});
}

} // namespace

std::expected<StyleSheet, StyleDiagnostic> StyleSheet::parse(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    StyleSheet sheet;
    Cursor c{css};
    u32 order = 0;
    for (;;) {
        c.skipWs();
        if (c.i >= css.size()) break;
        const auto brace = css.find('{', c.i);
        if (brace == std::string_view::npos) return std::unexpected(StyleDiagnostic{StyleError::Syntax, c.line, "expected '{'"});
        const std::string_view selectors = trim(css.substr(c.i, brace - c.i));
        const auto close = css.find('}', brace);
        if (close == std::string_view::npos) return std::unexpected(StyleDiagnostic{StyleError::Syntax, c.line, "expected '}'"});
        const u32 line = c.line;
        for (std::size_t k = c.i; k < close; ++k)
            if (css[k] == '\n') ++c.line;
        StyleRule proto;
        std::string_view body = css.substr(brace + 1, close - brace - 1);
        std::vector<std::pair<u32, std::string>> tokens;
        // declarations
        while (!body.empty()) {
            const auto semi = body.find(';');
            std::string_view decl = trim(body.substr(0, semi));
            if (!decl.empty()) {
                const auto colon = decl.find(':');
                if (colon == std::string_view::npos) {
                    if (diags) diags->push_back({StyleError::Syntax, line, std::string(decl)});
                } else {
                    const auto name = trim(decl.substr(0, colon));
                    const auto value = trim(decl.substr(colon + 1));
                    Prop p;
                    if (name.starts_with("--")) {
                        Declaration d;
                        d.customHash = hashId(name);
                        d.raw.assign(value);
                        tokens.emplace_back(d.customHash, d.raw);
                        proto.decls.push_back(std::move(d));
                    } else if (findProp(name, p)) addDecl(proto, p, value, line, diags);
                    else addShorthand(proto, name, value, line, diags);
                }
            }
            if (semi == std::string_view::npos) break;
            body.remove_prefix(semi + 1);
        }
        // one rule per selector in the list
        std::string_view list = selectors;
        while (!list.empty()) {
            const auto comma = list.find(',');
            const auto one = trim(list.substr(0, comma));
            StyleRule rule;
            bool isRoot = false;
            if (!parseSelector(one, rule, isRoot)) {
                if (diags) diags->push_back({StyleError::Syntax, line, "bad selector: " + std::string(one)});
            } else {
                rule.decls = proto.decls;
                rule.order = order++;
                rule.root = isRoot;
                if (isRoot) for (auto& t : tokens) sheet.m_tokens.push_back(t);
                if (!rule.chain.empty() || isRoot) sheet.m_rules.push_back(std::move(rule));
            }
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        c.i = close + 1;
    }
    return sheet;
}

bool StyleSheet::token(u32 h, std::string_view& out) const {
    for (auto it = m_tokens.rbegin(); it != m_tokens.rend(); ++it)
        if (it->first == h) { out = it->second; return true; }
    return false;
}

// ---- Engine --------------------------------------------------------------------------------------------------------
StyleEngine::StyleEngine() {
    m_table.reserve(256);
    m_hashIndex.assign(1024, 0);
    m_scratch.reserve(64);
    m_localTokens.reserve(16);
    intern(ComputedStyle{});
}

void StyleEngine::clearSheets() {
    m_sheets.clear();
    m_ordered.clear();
    ++m_version;
}

void StyleEngine::addSheet(StyleSheet sheet) {
    m_sheets.push_back(std::move(sheet));
    m_ordered.clear();
    u32 sheetIdx = 0;
    std::vector<std::pair<u64, const StyleRule*>> keyed;
    for (const auto& sh : m_sheets) {
        for (const auto& r : sh.m_rules) keyed.emplace_back((u64(r.specificity) << 40) | (u64(sheetIdx) << 32) | r.order, &r);
        ++sheetIdx;
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& k : keyed) m_ordered.push_back(k.second);
    ++m_version;
}

bool StyleEngine::matchSimple(const StyleSubject& s, u32 node, const SimpleSelector& sel) const {
    if (sel.typeHash && s.typeHash(node) != sel.typeHash) return false;
    if (sel.idHash && s.idHash(node) != sel.idHash) return false;
    for (u8 k = 0; k < sel.classCount; ++k)
        if (!s.hasClass(node, sel.classes[k])) return false;
    if ((s.states(node) & sel.states) != sel.states) return false;
    return true;
}

bool StyleEngine::matches(const StyleSubject& s, u32 node, const StyleRule& rule) const {
    if (rule.root) return s.parent(node) == 0xFFFFFFFFu;
    int k = int(rule.chain.size()) - 1;
    if (!matchSimple(s, node, rule.chain[std::size_t(k)])) return false;
    u32 cur = node;
    while (k > 0) {
        const auto comb = rule.chain[std::size_t(k)].combinator;
        --k;
        const auto& want = rule.chain[std::size_t(k)];
        if (comb == SimpleSelector::Combinator::Child) {
            cur = s.parent(cur);
            if (cur == 0xFFFFFFFFu || !matchSimple(s, cur, want)) return false;
        } else {
            for (;;) {
                cur = s.parent(cur);
                if (cur == 0xFFFFFFFFu) return false;
                if (matchSimple(s, cur, want)) break;
            }
        }
    }
    return true;
}

bool StyleEngine::resolveVar(std::string_view raw, Prop p, PropValue& out, int depth) const {
    if (depth > 8) return false;
    std::string_view v = trim(raw);
    const auto at = v.find("var(");
    if (at == std::string_view::npos) return parsePropValue(p, v, out);
    // whole-value var(--name[, fallback]); partial substitution only for single var use
    const auto close = v.rfind(')');
    std::string_view inner = v.substr(at + 4, close - at - 4);
    const auto comma = inner.find(',');
    const std::string_view name = trim(inner.substr(0, comma));
    const std::string_view fallback = comma == std::string_view::npos ? std::string_view{} : trim(inner.substr(comma + 1));
    const u32 h = hashId(name);
    std::string_view tok;
    bool found = false;
    for (auto it = m_localTokens.rbegin(); it != m_localTokens.rend() && !found; ++it)
        if (it->first == h) { tok = it->second; found = true; }
    for (auto it = m_sheets.rbegin(); it != m_sheets.rend() && !found; ++it) found = it->token(h, tok);
    if (!found) {
        if (fallback.empty()) return false;
        tok = fallback;
    }
    return resolveVar(tok, p, out, depth + 1);
}

u32 StyleEngine::intern(const ComputedStyle& cs) {
    // FNV over the bytes of a value-initialised copy is not padding-safe; hash fields that matter most and compare.
    u64 h = 1469598103934665603ull;
    auto mix = [&](f32 f) { u32 b; std::memcpy(&b, &f, 4); h = (h ^ b) * 1099511628211ull; };
    mix(cs.background.r); mix(cs.background.g); mix(cs.background.a); mix(cs.color.r); mix(cs.color.a);
    mix(cs.width.value); mix(cs.height.value); mix(cs.padding.l); mix(cs.padding.t); mix(cs.gap); mix(cs.fontSize);
    mix(cs.opacity); mix(cs.borderWidth); mix(cs.radius[0]); mix(cs.grow);
    h = (h ^ (u64(cs.layout) | u64(cs.direction) << 8 | u64(cs.visible) << 16 | u64(cs.alignItems) << 24 | u64(cs.justify) << 32)) * 1099511628211ull;
    const std::size_t mask = m_hashIndex.size() - 1;
    std::size_t slot = std::size_t(h >> 20) & mask;
    for (;;) {
        const u32 e = m_hashIndex[slot];
        if (e == 0) break;
        if (m_table[e - 1] == cs) return e - 1;
        slot = (slot + 1) & mask;
    }
    if ((m_table.size() + 1) * 2 > m_hashIndex.size()) {
        // grow and rehash
        std::vector<u32> bigger(m_hashIndex.size() * 2, 0);
        m_hashIndex.swap(bigger);
        const auto keep = std::move(m_table);
        m_table.clear();
        m_table.reserve(keep.size() * 2);
        for (const auto& k : keep) intern(k);
        return intern(cs);
    }
    m_table.push_back(cs);
    m_hashIndex[slot] = u32(m_table.size());
    return u32(m_table.size() - 1);
}

u32 StyleEngine::compute(const StyleSubject& s, u32 node, const ComputedStyle* parent, const ComputedStyle* defaults,
                         const std::vector<std::pair<Prop, PropValue>>& inl) {
    ComputedStyle cs = defaults ? *defaults : ComputedStyle{};
    if (parent) inheritFrom(cs, *parent);
    m_scratch.clear();
    m_localTokens.clear();
    for (const StyleRule* r : m_ordered)
        if (matches(s, node, *r)) {
            m_scratch.push_back(r);
            for (const auto& d : r->decls)
                if (d.prop == Prop::Count) m_localTokens.emplace_back(d.customHash, std::string_view(d.raw));
        }
    for (const StyleRule* r : m_scratch)
        for (const auto& d : r->decls) {
            if (d.prop == Prop::Count) continue;
            if (d.raw.empty()) applyProp(cs, d.prop, d.value);
            else {
                PropValue v;
                if (resolveVar(d.raw, d.prop, v, 0)) applyProp(cs, d.prop, v);
            }
        }
    for (const auto& [p, v] : inl) applyProp(cs, p, v);
    return intern(cs);
}

} // namespace fuse::ui
