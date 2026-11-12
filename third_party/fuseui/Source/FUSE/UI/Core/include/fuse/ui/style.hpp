// FUSE UI style engine: property registry, computed styles, CSS-subset style sheets, cascade. Plan §4.6, decision D4.
//
// Style sheet subset (`.fusestyle`):
//   selectors   type (widget class name), `.class`, `#id`, `*`, pseudo-classes :hover :pressed :focus :focus-visible
//               :disabled :checked :selected, descendant (` `) and child (`>`) combinators, selector lists (`,`)
//   values      px / % / fr / plain numbers, #rgb #rrggbb #rrggbbaa, rgb()/rgba(), keywords, var(--token[, fallback])
//   tokens      custom properties (`--accent: #3af`) declared in `:root` (or any rule matching the node) — theme tokens
//   cascade     specificity (ids, classes + pseudo, types), then source order; inline (node API) values win;
//               inherited properties: color, font-size, text-align
// Not yet: transitions (the animation core tweens node opacity / offsets), attribute selectors, :nth-child.
#pragma once

#include "fuse/ui/types.hpp"

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

enum class Unit : u8 { Auto, Px, Percent, Fr };
struct Length {
    f32 value = 0.f;
    Unit unit = Unit::Auto;
    static constexpr Length px(f32 v) { return {v, Unit::Px}; }
    static constexpr Length pct(f32 v) { return {v, Unit::Percent}; }
    static constexpr Length fr(f32 v) { return {v, Unit::Fr}; }
    static constexpr Length autoLen() { return {}; }
    bool isAuto() const { return unit == Unit::Auto; }
    /// Resolves against `base` (percent); Auto and Fr resolve to `fallback`.
    f32 resolve(f32 base, f32 fallback) const {
        switch (unit) {
        case Unit::Px: return value;
        case Unit::Percent: return base * value / 100.f;
        default: return fallback;
        }
    }
    friend bool operator==(const Length&, const Length&) = default;
};

enum class LayoutKind : u8 { Box, Grid, Stack, Anchor, Absolute, Flow };
enum class Direction : u8 { Row, Column };
enum class Align : u8 { Auto, Start, Center, End, Stretch };
enum class Justify : u8 { Start, Center, End, SpaceBetween, SpaceAround };
enum class Overflow : u8 { Visible, Clip, Scroll };
enum class GradientDir : u8 { None, Vertical, Horizontal };
enum class TextAlignment : u8 { Start, Center, End };

inline constexpr int kMaxGridTracks = 8;

/// The fully resolved style of a node. Trivially copyable; deduplicated in StyleTable.
struct ComputedStyle {
    // display / layout
    bool visible = true;
    LayoutKind layout = LayoutKind::Box;
    Direction direction = Direction::Column;
    Length width, height, minWidth, minHeight, maxWidth, maxHeight, basis;
    f32 grow = 0.f, shrink = 1.f;
    Edges padding, margin;
    f32 gap = 0.f;
    Align alignItems = Align::Stretch;
    Align alignSelf = Align::Auto;
    Justify justify = Justify::Start;
    bool wrap = false;
    Overflow overflow = Overflow::Visible;
    std::array<Length, kMaxGridTracks> gridColumns{};
    u8 gridColumnCount = 0;
    u8 gridColumn = 0, gridColumnSpan = 1, gridRow = 0, gridRowSpan = 1; ///< 0 = auto placement
    Length left, top, right, bottom; ///< Anchor / Absolute placement
    bool centerX = false, centerY = false;
    // paint
    Color background, background2, borderColor, color = Color::rgb(0xFFFFFF), shadowColor;
    GradientDir gradient = GradientDir::None;
    f32 borderWidth = 0.f;
    std::array<f32, 4> radius{}; ///< tl, tr, br, bl
    f32 fontSize = 14.f;
    TextAlignment textAlign = TextAlignment::Start;
    f32 opacity = 1.f;
    f32 shadowBlur = 0.f, shadowX = 0.f, shadowY = 0.f;
    f32 backdropBlur = 0.f; ///< backdrop-filter: blur(<len>) / backdrop-blur: <len> (frosted glass under the background)
    bool pointerEvents = true;
    friend bool operator==(const ComputedStyle&, const ComputedStyle&) = default;
};

// ---- Property registry ---------------------------------------------------------------------------------------------
enum class Prop : u8 {
    Display, Layout, Direction, Width, Height, MinWidth, MinHeight, MaxWidth, MaxHeight, Basis, Grow, Shrink,
    PaddingLeft, PaddingTop, PaddingRight, PaddingBottom, MarginLeft, MarginTop, MarginRight, MarginBottom, Gap,
    AlignItems, AlignSelf, Justify, Wrap, Overflow, GridColumns, GridColumn, GridColumnSpan, GridRow, GridRowSpan,
    Left, Top, Right, Bottom, CenterX, CenterY, Background, Background2, Gradient, BorderColor, BorderWidth,
    RadiusTL, RadiusTR, RadiusBR, RadiusBL, Color, FontSize, TextAlign, Opacity, ShadowColor, ShadowBlur, ShadowX,
    ShadowY, PointerEvents, BackdropBlur, Count
};
inline constexpr int kPropCount = int(Prop::Count);

enum class PropType : u8 { Bool, Enum, Length, Number, Color, Tracks };
struct PropInfo {
    Prop id;
    std::string_view cssName;
    PropType type;
    bool inherited;
    bool animatable;
    bool affectsLayout;
};
/// The single registry shared by style sheets, inline setters, animation, the inspector and Lua bindings.
const PropInfo& propInfo(Prop p);
/// Looks up a CSS name (longhands only; shorthands are expanded by the parser). False when unknown.
bool findProp(std::string_view cssName, Prop& out);

/// A typed property value.
struct PropValue {
    PropType type = PropType::Number;
    bool b = false;
    u8 e = 0;
    Length len;
    f32 num = 0.f;
    fuse::ui::Color color;
    std::array<Length, kMaxGridTracks> tracks{};
    u8 trackCount = 0;
    static PropValue ofBool(bool v) { PropValue p; p.type = PropType::Bool; p.b = v; return p; }
    static PropValue ofEnum(u8 v) { PropValue p; p.type = PropType::Enum; p.e = v; return p; }
    static PropValue ofLength(Length v) { PropValue p; p.type = PropType::Length; p.len = v; return p; }
    static PropValue ofNumber(f32 v) { PropValue p; p.type = PropType::Number; p.num = v; return p; }
    static PropValue ofColor(fuse::ui::Color v) { PropValue p; p.type = PropType::Color; p.color = v; return p; }
};
void applyProp(ComputedStyle& s, Prop p, const PropValue& v);
PropValue readProp(const ComputedStyle& s, Prop p);
/// Copies inherited properties from parent into s.
void inheritFrom(ComputedStyle& s, const ComputedStyle& parent);

// ---- State bits used by pseudo-classes -----------------------------------------------------------------------------
enum StateBits : u32 {
    kStateHover = 1u << 0,
    kStatePressed = 1u << 1,
    kStateFocus = 1u << 2,
    kStateFocusVisible = 1u << 3,
    kStateDisabled = 1u << 4,
    kStateChecked = 1u << 5,
    kStateSelected = 1u << 6,
};

// ---- Style sheets --------------------------------------------------------------------------------------------------
enum class StyleError : u8 { Syntax, UnknownProperty, BadValue };
struct StyleDiagnostic {
    StyleError error;
    u32 line;
    std::string message;
};

struct SimpleSelector {
    u32 typeHash = 0; ///< 0 = any
    u32 idHash = 0;
    std::array<u32, 4> classes{};
    u8 classCount = 0;
    u32 states = 0;
    enum class Combinator : u8 { None, Descendant, Child } combinator = Combinator::None; ///< to the previous (left)
};

struct Declaration {
    Prop prop = Prop::Count;   ///< Count => custom property definition
    u32 customHash = 0;        ///< custom property name (definition) or var() reference
    PropValue value;
    std::string raw;           ///< unresolved text when it references var(); custom property text
};

struct StyleRule {
    std::vector<SimpleSelector> chain; ///< left to right
    u32 specificity = 0;
    u32 order = 0;
    std::vector<Declaration> decls;
    bool root = false; ///< :root
};

/// What the matcher needs to know about a node and its ancestors.
struct StyleSubject {
    virtual ~StyleSubject() = default;
    virtual u32 typeHash(u32 node) const = 0;
    virtual u32 idHash(u32 node) const = 0;
    virtual bool hasClass(u32 node, u32 classHash) const = 0;
    virtual u32 states(u32 node) const = 0;
    virtual u32 parent(u32 node) const = 0; ///< 0xFFFFFFFF at the root
};

class StyleSheet {
public:
    /// Parses CSS-subset text. Unknown properties and bad values are skipped and reported in `diagnostics`;
    /// unrecoverable syntax errors fail.
    static std::expected<StyleSheet, StyleDiagnostic> parse(std::string_view css,
                                                            std::vector<StyleDiagnostic>* diagnostics = nullptr);
    const std::vector<StyleRule>& rules() const { return m_rules; }
    /// Theme tokens (from :root).
    bool token(u32 nameHash, std::string_view& out) const;
    const std::vector<std::pair<u32, std::string>>& tokens() const { return m_tokens; }
    /// Rebuilds a parsed sheet from its parts (cooked `.fusestyle` loading; see fuse/ui/doc/cook.hpp).
    static StyleSheet fromParts(std::vector<StyleRule> rules, std::vector<std::pair<u32, std::string>> tokens) {
        StyleSheet s;
        s.m_rules = std::move(rules);
        s.m_tokens = std::move(tokens);
        return s;
    }

private:
    friend class StyleEngine;
    std::vector<StyleRule> m_rules;
    std::vector<std::pair<u32, std::string>> m_tokens;
};

/// Parses one value for `p` (also used by inline setters and markup). False on a bad value.
bool parsePropValue(Prop p, std::string_view text, PropValue& out);
bool parseColor(std::string_view text, Color& out);

/// Ordered sheets + deduplicated computed-style table + matcher.
class StyleEngine {
public:
    StyleEngine();
    void clearSheets();
    void addSheet(StyleSheet sheet);
    u32 sheetVersion() const { return m_version; }
    /// Computes the style of `node`: defaults → inherited from `parentStyle` → matched rules → `inlineValues`.
    /// Returns an index into the table (allocates only for a never-seen combination).
    u32 compute(const StyleSubject& subject, u32 node, const ComputedStyle* parentStyle,
                const ComputedStyle* defaults, const std::vector<std::pair<Prop, PropValue>>& inlineValues);
    const ComputedStyle& style(u32 index) const { return m_table[index]; }
    u32 tableSize() const { return u32(m_table.size()); }
    bool matches(const StyleSubject& subject, u32 node, const StyleRule& rule) const;

private:
    bool matchSimple(const StyleSubject& s, u32 node, const SimpleSelector& sel) const;
    bool resolveVar(std::string_view raw, Prop p, PropValue& out, int depth) const;
    u32 intern(const ComputedStyle& cs);
    std::vector<StyleSheet> m_sheets;
    std::vector<const StyleRule*> m_ordered; ///< all rules, sorted by (specificity, sheet, order)
    std::vector<const StyleRule*> m_scratch;
    std::vector<std::pair<u32, std::string_view>> m_localTokens;
    std::vector<ComputedStyle> m_table;
    std::vector<u32> m_hashIndex; ///< open addressing into m_table (+1)
    u32 m_version = 0;
};

} // namespace fuse::ui
