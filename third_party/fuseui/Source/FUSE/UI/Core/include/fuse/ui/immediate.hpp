// FUSE UI immediate-mode front-end (plan §4.13 "both models on one core", D3). The API mirrors Relight's developer
// overlay Ui (fuse::relight::overlay::Ui: passes, one event per pass, sameLine, button, textField, widgetRect) so the
// overlay can move onto fuse::ui; the equivalence gate (fuse_ui_relight_equivalence) drives both with the same event
// script and requires identical widget rects, clicks, focus and field contents.
//
// Output is a fuse::ui::DrawList (rounded rects + real glyphs from the shared Font/GlyphAtlas) instead of 5x7 text.
// Layout metrics default to the overlay's cell grid (Metrics::overlay()) so rects match; Metrics::fromFont() sizes
// cells from the font for a native look. Steady state allocates nothing (ids are FNV hashes, buffers keep capacity).
#pragma once

#include "fuse/text/text.hpp"
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::immediate {

struct IRect {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    bool contains(std::int32_t px, std::int32_t py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    friend bool operator==(const IRect&, const IRect&) = default;
};

enum class EventType : std::uint8_t { None = 0, MouseMove, MouseDown, MouseUp, Wheel, KeyDown, Char, Toggle };
struct InputEvent {
    EventType type = EventType::None;
    std::int32_t x = 0, y = 0;
    std::int32_t value = 0; ///< Wheel notches; KeyDown: virtual key (kVk*); Char: character
};
inline constexpr std::int32_t kVkBack = 0x08, kVkReturn = 0x0D, kVkEscape = 0x1B;

struct Metrics {
    std::int32_t cellWidth = 6, glyphHeight = 7, lineHeight = 10; ///< unscaled pixels
    static Metrics overlay() { return {}; }
    static Metrics fromFont(const fuse::text::Font& f, float px);
};

struct Theme {
    Color panel = Color::rgb(0x10121A, 0.85f), title = Color::rgb(0x28468C), text = Color::rgb(0xE6E8EE),
          dim = Color::rgb(0x969CAA), accent = Color::rgb(0x78C8FF), button = Color::rgb(0x343A4C),
          buttonHot = Color::rgb(0x48526C), buttonOn = Color::rgb(0x2E78C8), field = Color::rgb(0x08080C);
    float radius = 3.f;
};

/// One text draw of the last pass, kept with its string so a client can show it with another renderer or inspect it
/// (Relight's overlay keeps semantic text commands). Its glyphs are draws()[firstCmd, firstCmd + cmdCount).
struct TextRun {
    std::int32_t x = 0, y = 0, clipW = 0;
    Color color;
    std::uint32_t textBegin = 0, textLength = 0;
    std::uint32_t firstCmd = 0, cmdCount = 0;
};

class ImmediateUi {
public:
    explicit ImmediateUi(Metrics m = Metrics::overlay());
    /// Font for glyphs (null: rects only). Non-owning; the atlas must outlive draws().
    void setFont(const fuse::text::Font* font, fuse::text::GlyphAtlas* atlas, float pixelSize);
    void setTheme(const Theme& t) { m_theme = t; }

    void begin(const IRect& panel, std::int32_t scale, const InputEvent* event);
    void end();

    void sameLine() { m_sameLine = true; }
    void spacing(std::int32_t lines = 1);
    void separator();
    bool hasRoom(std::int32_t lines = 1) const { return linesLeft() >= lines; }
    std::int32_t linesLeft() const;
    std::int32_t charsLeft() const;

    void text(std::string_view s);
    void text(std::string_view s, Color c);
    void title(std::string_view s);
    bool button(std::string_view id, std::string_view label, bool selected = false);
    bool textField(std::string_view id, std::string& value, std::int32_t widthChars, std::size_t maxLength = 64);

    const InputEvent& event() const { return m_event; }
    std::int32_t keyPressed() const { return (m_event.type == EventType::KeyDown && !m_keyConsumed) ? m_event.value : 0; }
    std::int32_t wheel() const { return m_event.type == EventType::Wheel ? m_event.value : 0; }
    bool mouseInPanel() const { return m_panel.contains(m_mouseX, m_mouseY); }

    const DrawList& draws() const { return m_draws; }
    const std::vector<TextRun>& textRuns() const { return m_runs; }
    std::string_view text(const TextRun& r) const { return {m_runText.data() + r.textBegin, r.textLength}; }
    std::int32_t scale() const { return m_scale; }
    bool widgetRect(std::string_view id, IRect& out) const;
    const IRect& panel() const { return m_panel; }
    std::uint32_t focused() const { return m_focus; }
    std::uint32_t widgetCount() const { return std::uint32_t(m_lastWidgets.size()); }

private:
    struct Widget {
        std::uint32_t id = 0;
        IRect rect;
    };
    bool place(std::int32_t widthPx, IRect& out);
    bool interact(std::uint32_t id, const IRect& r, bool& hot);
    void fill(const IRect& r, Color c);
    void glyphs(std::int32_t x, std::int32_t y, std::string_view s, Color c, std::int32_t clipW);

    Metrics m_metrics;
    Theme m_theme;
    const fuse::text::Font* m_font = nullptr;
    fuse::text::GlyphAtlas* m_atlas = nullptr;
    float m_px = 12.f;
    IRect m_panel;
    std::int32_t m_scale = 1, m_pad = 2, m_cursorX = 0, m_cursorY = 0, m_lineY = 0;
    bool m_sameLine = false, m_lineOpen = false;
    InputEvent m_event;
    std::int32_t m_mouseX = -1, m_mouseY = -1;
    std::uint32_t m_active = 0, m_focus = 0;
    bool m_keyConsumed = false;
    DrawList m_draws;
    std::vector<Widget> m_widgets, m_lastWidgets;
    std::vector<fuse::text::ShapedGlyph> m_shape;
    std::vector<TextRun> m_runs;
    std::vector<char> m_runText;
};

} // namespace fuse::ui::immediate
